# SDCShield ARM64 浮点向量单元 SDC 激发增强提案

> 2026-09-16,主会话综合(4 路并行 subagent 研究的交叉产物)
> 证据源:A-papers.md(9 篇论文精读 + 31 篇蒸馏综合)、B-cpu.md(cn23154/920 微架构与故障签名)、C-sve-sme.md(SVE/SVE2/SME/SME2 ISA 与框架接入)、D-tests.md(304 测试盘点与覆盖矩阵)
> 性质:**研究提案,未写任何代码**。实施按 CLAUDE.md 走 superpowers:writing-plans → one-patch-per-unit。

---

## 0. 执行摘要

四路研究交叉后收敛出一条主线、三个乘数:

**主线(激发面)**:本仓 SVE 指令级覆盖只有 11 个测试且全部满谓词 `svptrue`,集中在常规 FMLA 链 + 单一 gather 变体;而论文侧(SEVI/PinDrop)证明向量 FMA 族是第一大 SDC 源、**新指令代际风险最高**,且 cn23154 的**已知故障签名指令(stnt1d/ldnt1d)在全仓零覆盖**。新增 8 个指令族测试(§3)把 0xd22 上实际存在的独立硅片数据通路逐一打满:non-temporal 访存、FMMLA 矩阵外积、SVE2 跨精度乘加、FCMLA 复数、谓词/归约、FPCR 配置空间、gather 变体、SVE512 di/dt。

**乘数一(检测强度,§4)**:约 2 成测试用容差校验(fma 1e-6 / acl_gemm 1e-3),对 1-bit 翻转全盲;论文铁律是全位宽字节精确(SEVI:exponent/符号位也翻,误差可达 10240)。**激发做得再多,校验是盲的等于白做**——修复弱校验是新增激发的前置。

**乘数二(campaign 形态,§5)**:已知故障 ~19min 节律 + 簇锚定,默认 5s 单轮测试对它完全盲。需要簇锚定绑定、≥30min 驻留、健康簇对照、双节奏(长驻 + 频繁切换)的 campaign 模式;论文侧(PinDrop/Ripple)证明持续与双节奏比单次快照多出数量级检出。

**乘数三(诊断,§6)**:崩溃时自动解析 si_addr 位签名(VA[55:48] 非零 = 直接命中已知签名)、失败时 lane 直方图、VA[63:48] canary 常态化——把"激发到了"变成"可判读的证据"。

SME 结论先行:cn23154 **硬件确有 SME+SME2**(SMFR0 实测),但 5.10 内核不暴露 HWCAP2_SME(需 ≥5.19),用户态 SMSTART 预期 SIGILL。SME 测试按"现在写、运行时干净 skip"入库(仅 clang 可编),当下即战力是 SVE2(§3),SME 是面向 Neoverse V3 代的前瞻布局(§7)。

---

## 1. 证据基础与核心洞察

### 1.1 四路研究的交叉点

| 证据 | 来源 | 对测试设计的含义 |
|---|---|---|
| 向量 FMA 是第一大 SDC 源(>92% 事故,top-20 指令 19/20 是 FMA) | SEVI Obs.2(ASPLOS'26) | FMLA/FMMLA/FDOT 族是核心弹药 |
| 256-bit FMA 有 128-bit 没有的低频(<10⁻⁵)SDC 窗口;功耗→电压裕量机理 | SEVI Obs.4 | SVE 512-bit 风险面外推(推断),需更长循环命中低频窗口 |
| 新指令在其首代架构失败率最高 | PinDrop Obs.12 | SVE2/SME 谓词、跨精度族 = 最高风险区;0xd22 的 SVE2 全集未打过 |
| gather SDC 76% 是错偏移读、从不 crash、输入级复现≈0 | SEVI Obs.9 | gather 测试必须显式验证"读到的 == 预期索引的值" |
| exponent/符号位也翻(不止尾数),相对误差达 10240 | SEVI Obs.8/17(推翻 SOSP23 尾数论) | 全位宽字节精确 memcmp 唯一安全 |
| 乘法器缺陷短前缀检出崩塌(0.01× 前缀 → 0%) | Harpocrates++ Fig.6 | FMA 测试必须打满时间窗,不用短前缀 |
| execution context 是首要预测因子;热循环加一条 no-op 都可能杀触发 | ITHICA Finding 7/Obs.11 | 热循环零扰动纪律;同一数学多种调度 = 独立样本 |
| 31% 测试曾是某机器唯一检出者;多样性 > 单负载强度 | PinDrop Obs.7 | 每个未覆盖数据通路都是潜在唯一检出者 |
| 长深测与高频切换互补;7% 缺陷只有频繁切换才触发 | Fleetscanner/Ripple | campaign 双节奏 |
| 温度 log-线性触发;高利用率独立于温度也提频 | SOSP23 Obs.10 | 无 cpufreq 的板上,全核并发是唯一温度代理 |
| 故障签名:RF 读出→旁路→AGU→SVE LSU 地址入端,VA[55:48],栈重装载 2-3 指令窗口 + 簇饱和 ≥36 线程,~19min 节律;HPL 70h/eigen 68.65h 零复现 | B 路(memory 档案 + cn23154.md) | **结构性激发 ≠ 吞吐激发**;测试要复刻结构,不是加压 |
| stnt1d/ldnt1d 全仓零覆盖 | C 路 grep 实测 | 已知故障触发指令没有测试在打 |
| 11 个 SVE 测试全部满谓词;FPCR 舍入模式全仓零操控;FCMLA/BF16/FP16/归约/除法零覆盖 | D 路矩阵 | 谓词网络、舍入网络、复数通路、低精度管线全是空白 |
| OpenDCDiag 上游 FP 乘法器覆盖仅 58.2%(GEMM 混合乘加天然不足) | Harpocrates ISCA'24 Fig.6 | 需要高乘法密度、低加法占比的微内核档 |

### 1.2 核心洞察

1. **"激发"由三个独立变量决定:指令代际 × 数据通路独占性 × 结构要素重合度。** HPL 70 小时输给 19 分钟配方,因为激发不是吞吐量的函数。新提案的每个测试都要回答:它打的通路是否独占硅片面积?是否复刻了某种结构要素(重装载窗口/谓词部分活跃/舍入配置)?
2. **空白与证据精确对齐**:已知故障签名指令(stnt1d)零覆盖 + 论文最高风险指令族(FMA 变体/FMMLA)零覆盖 + 配置空间(FPCR)零覆盖——三者都不是"锦上添花",而是"已证明要紧但没打"。
3. **检测强度是激发的前置乘数**:fma 家族 1e-6 容差、acl_gemm 1e-3 容差对单 bit 翻转全盲(fsu_byteexact_arm 注释已自我诊断);20 个文件用 `std::mt19937(random_device)` 破坏 `-s seed` 可重放性——失败不可复现等于证据丢失。

---

## 2. 提案总览(优先级矩阵)

排序依据:SDC 激发价值(论文+故障签名证据强度)× 可行性(cn23154 当下可跑 / 框架改动量 / 工具链约束)。

| # | 提案 | 层 | 激发价值 | 可行性 | 证据 |
|---|---|---|---|---|---|
| P0-1 | non-temporal 访存 + 栈重装载窗口参数化家族(ldnt1d/stnt1d/ld1rd) | 新测试 | ★★★(已知故障签名指令,零覆盖) | 高(SVE 库即可) | B§2 + C§2.5 |
| P0-2 | 弱校验测试升级 byte-exact(fma/fma_patterns/acl_gemm)+ RNG 可重放修复 | 改造 | ★★★(检测前置) | 高 | D§4.8/4.9 + SEVI |
| P1-1 | FMMLA f32/f64 矩阵外积链(ZFR0 f32mm/f64mm=1) | 新测试 | ★★★(SVE 内最高 FLOPS 密度独立单元,零覆盖) | 高 | C§2.3 + SEVI/PinDrop |
| P1-2 | SVE2 跨精度乘加族(FDMLA/FMLALB·T/BFDOT/BF16) | 新测试 | ★★★(第一代新指令 + 独立低精度管线) | 中(新库 armv9-a+sve2;GCC12 intrinsic 完整度需探针) | C§2.2 + PinDrop Obs.12 |
| P1-3 | FCMLA 复数 FMA(0/90/180/270 全旋转) | 新测试 | ★★(复数 datapath 零覆盖) | 高 | C§2.3 + D§4.5 |
| P1-4 | 谓词维度:部分谓词/翻转/whilelt 边界 + FADDV/FMAXV 归约 + LDFF1D/FFR | 新测试 | ★★★(SVE 独有新通路,11 测试全满谓词) | 高 | D§4.3 + PinDrop Obs.12 |
| P1-5 | FPCR 舍入模式 × FZ × 操作数笛卡尔(含 SVE 版) | 新测试 | ★★(配置空间全仓零扫描) | 中(需 mrs/msr fpcr helper,零先例) | D§4.1/4.2 |
| P1-6 | gather 变体扩展 + 读偏移显式验证(向量基址/32b 索引/sxtw/跨 64KB 页) | 扩展 | ★★★(SEVI 76% 错偏移;现有 1/8 变体) | 高 | C§2.5 + SEVI Obs.9 |
| P1-7 | 既有 sve512 家族加双值域档(bounded/unbounded)+ 双 seed 旋钮 | 改造 | ★★(SEVI 245× 差;低成本) | 高 | A-P2 + SEVI Obs.19 |
| P2-1 | 簇锚定 campaign 模式(--cpus 语义化 + 健康簇对照 + ≥30min 驻留) | 框架 | ★★★(19min 节律 + 簇锚定的唯一解) | 中(框架级) | B§5.2/5.8 + PinDrop/Ripple |
| P2-2 | SVE512 di/dt 病毒(power_virus_dit 的 SVE 变体 + burst 内嵌地址生成) | 新测试 | ★★(512b 通路瞬态电流×4,推断) | 中(跨核 barrier 缺失是已知短板) | B§5.3 + cpu-sdc 机理 |
| P2-3 | 跨簇共享数据 × FP 计算(mesh 的 FP 版) | 新测试 | ★★(已知签名 = 远端访问×计算复合;mesh 27 测试全整型) | 中 | D§4.7 + B§2 |
| P2-4 | 诊断增强:si_addr 位签名解析 + lane 直方图 + VA canary 常态化 | 框架 | ★★(把崩溃变成可判读证据) | 中 | B§2 + SEVI Obs.13 |
| P3-1 | SME FMOPA/ZA/SMSTART-SMSTOP 测试(clang-only,运行时 skip) | 新测试 | ★(cn23154 内核 5.10 预期不可达;面向 V3 代) | 中(仅 clang;SIGILL-probe 先行) | C§4/§5.1 |
| P3-2 | simd-arm.conf neoverse_v2 错挂 sme 的修正 | 修复 | —(数据准确性) | 高(一行) | C§1.1 |

---

## 3. 第一层:cn23154 当下可跑的新激发测试(纯 SVE/SVE2)

> 通用工程纪律(全部沿用 sve512 家族已验证干净的模式,C§1.3):ACLE intrinsics;init 首行 `getauxval` 手写探测(+sve 编译的 TU 内不能用 device_has_feature)→ 干净 `EXIT_SKIP`;标量同序 golden + 字节级 memcmp;特殊值按类别比较。当次开发机(Kunpeng 920,part 0xd01)无 SVE,故只能验证到编译 + SVE 代码生成 + 干净 skip;真实验证在 cn23154。**这是按机结论,不是按仓结论**:换机须按 CLAUDE.md 第 2 条现场探测。

### P0-1: non-temporal 访存 + 栈重装载窗口家族 `sve512_nt_reload_arm`(最高价值)

- **为什么**:已知 NUMA3 故障的触发指令窗口就是 ld1rd/stnt1d + base 指针栈重装载后 2-3 条指令内进 SVE 寻址(B§2.2-2);而 `stnt/ldnt` 全仓零覆盖(C§2.5 实测 grep)。这是"已证明要紧但没打"的最大单项。
- **形态**:`svldnt1d/svstnt1d`(绕 cache 提示)+ `svld1rd`(广播装载)的热循环;base 指针故意从栈槽重装载(阻止编译器把它钉在寄存器);**reload→use 距离做成 1/2/3/4 条指令的扫描**(B§5.1:把故障窗口参数化,直接复刻签名结构);与 FMA 计算交织形成"地址生成 × 计算"复合压力。
- **golden**:整块可识别数据(如 `buf[i] = i` 编码)byte-exact 比对;地址侧加 VA[63:48] canary(stencil_axis 已示范)。
- **依据**:B§2 故障签名实据;C§2.5 覆盖空白实测;ITHICA execution context(距离扫描的合理性)。
- **实现落点**:tests/cpu/arm64/ 新文件,进 `tests_arm64_sve` 库(现有 `-march=armv8.2-a+sve` 即含这些 SVE1 指令)。

### P1-1: FMMLA 矩阵外积链 `sve512_fmmla_{f32,f64}`

- **为什么**:FMMLA 是 SVE 内 FLOPS 密度最高的独立矩阵单元(每指令 VL/2 平方乘加),cn23154 ZFR0 f32mm/f64mm=1(ID 寄存器实测,B§1.1);全仓零覆盖(C§2.3)。SEVI/PinDrop:向量乘加复合链是第一大 SDC 源 + 新指令代际最高风险。
- **形态**:`svmmla_f32/f64`(需 `+f32mm/+f64mm` 编译修饰)长链,矩阵块遍历(可复用 SVD 家族已修复的块主序布局,progress.md 2026-09-15 修复经验:分配量级 20MB 级、尾块 clamp、whilelt 谓词);乘法密度拉满(外积本身就是"高乘低加"——正好补 Harpocrates 指出的 GEMM 乘法器覆盖不足)。
- **golden**:标量三重循环同序重算,字节比较。
- **实现落点**:需在 `tests_arm64_sve` 库 flags 追加 `+f32mm,+f64mm`(或新建小库,实施时以编译探测定)。

### P1-2: SVE2 跨精度乘加族 `sve2_cross_precision`(FDMLA / FMLALB·T / BFDOT / BF16 FMLAL)

- **为什么**:FP16→FP32、BF16→FP32、FP32→FP64 的跨精度乘加各自走独立的低精度管线与移位/对齐网络(C§2.2);SVE2 是"第一代新指令"(PinDrop Obs.12 最高风险);BF16 是服务器 ML 负载主流。cn23154 SVE2 全集 + bf16(ID 实测)。
- **形态**:`svmlalb/svmlalt`(FP16)、`svdot`/`svbfdot`(BF16)、`svfdmla`(FP32→FP64,仅 SVE2);同一数学多个精度档;操作数含高 Hamming 交替(DelayAVF toggle 依赖)。
- **golden**:全部用 FP64 标量同序精确重算(低精度输入在 double 下无舍入损失)。
- **实现落点**:新库 `tests_arm64_sve2`(`-march=armv9-a+sve2`,独立库防基线代码生成被污染,C§5.2);GCC 12.3 的 SVE2 intrinsic 完整度需先跑最小编译探针,不行就 clang-only 构建(目标机 BiSheng clang 19.1.7 链路已验证通)。

### P1-3: FCMLA 复数 FMA 链 `sve512_fcmla`

- **为什么**:复数交叉乘+旋转选择是独立 datapath(实/虚部双 FMA);eigen_svd_cdouble 只走标量展开,指令级零覆盖(D§4.5);通信/AI 复数负载的真实通路。SVE1 即有,cn23154 可跑。
- **形态**:`svcmla` 旋转 0/90/180/270 全变体链 + 复数 gather 变体;操作数含复平面单位圆边界值。
- **golden**:std::complex 标量同序(注意 complex 乘法在 C++ 中是先乘后加非融合,与 FCMLA 的融合语义有 ULP 差——golden 必须按 FCMLA 融合语义手写展开,这是实施时的关键细节)。

### P1-4: 谓词维度 `sve512_pred_{ops,reduce}`

- **为什么**:SVE 谓词网络(逐 lane 掩蔽、whilelt 生成、部分活跃)是 SVE 区别于 NEON 的全新数据通路,11 个 SVE 测试无一触碰(D§4.3);谓词位翻错的形态是"通道静默屏蔽/多算",位级仍合法——正是 SDC 形态。
- **形态**:(a) 谓词密度扫描 1/8→8/8 活跃通道 × FMA/访存组合,per-lane 校验(哪些 lane 该算/不该算都验证);(b) `svaddv/svmaxv` 树形归约链(独立于 FMA 的加法树);(c) `svldff1` first-fault + FFR 状态检查;(d) whilelt 边界(恰好整除/余 1/余 N)。
- **golden**:标量按谓词逐 lane 条件执行同序重算。

### P1-5: FPCR 配置空间 `fpcr_rounding_cartesian`(NEON+SVE 双版)

- **为什么**:舍入网络(RNE/RTZ/RDN/RUP 四模式 × FZ 开关 × denormal)是独立硅区域;全仓零 FPCR 操控(D§4.1,grep 证实)。论文(蒸馏综合):边界值 × 配置组合是最脆弱组合之一。操作数空间扫了(fpu_special_values 12 值笛卡尔是好开端),配置空间没扫。
- **形态**:热循环内 `mrs/msr fpcr` 切换档位(注意:msr 是 serializing 类操作,放在循环外层分段,每段内配置恒定——保持热循环零扰动纪律);操作数表 = 12 特殊值 × 正常值;FMA + FADD + FMUL + FCVT 混排(舍入路径不同,D§4.12)。
- **golden**:每档配置用 `fesetround` 同模式标量重算(FZ 档 golden 需手动模拟 flush)。
- **实现落点**:框架无 FPCR helper(D§3)——本测试自带最小 inline asm helper;若后续多测试要用,再提炼公共 helper(避免过度设计)。

### P1-6: gather/scatter 变体扩展 + 读偏移显式验证

- **为什么**:SEVI Obs.9——76% 访存类 SDC 是错偏移读(有效数据、错误地址)、从不 crash、输入级复现≈0;现有 `sve512_gather_scatter_*` 只有 `u64index` 1/8 变体且校验是"同间接 golden"(方向对,但偏移正确性不显式)。另:cn23154 仅 64KB 页(TGran4=0xf),跨页 gather 是 TCR/TBI 语义的天然应力点(B§5.4)。
- **形态**:(a) 数据构造为 `buf[i]=i` 类可识别编码,gather 后**逐 lane 断言 读到的值 == 索引指向的预期值**(显式偏移验证,不只是整体 memcmp);(b) 变体:向量基址 `[z0.d, z1.d]`、32-bit 索引 sxtw/uxtw、`lsl #3` 移位;(c) 索引跨 64KB 页边界、跨 NUMA 距离梯度(本地→SCN→远端,B§3 表);(d) 大跨度索引强制 cache miss(ITHICA MemDiv 思想:迫使走主存路径)。
- **实现落点**:优先改造 `sve512_gather_scatter_arm`(加变体与显式断言)而非新文件——减小表面积。

### P1-7: 既有 sve512 家族双值域档 + 双 seed 旋钮(低成本改造)

- **为什么**:SEVI Obs.19 同核 bounded(-1,1) vs unbounded 输入 SDC 频率差 245×;37% case 有输入位偏置(FP32 偏尾数位、FP64 偏指数位)。现有 9 个测试的操作数值域是固定的——加一个 `bounded` 旋钮(`-O test.bounded=1`)即多一档激发模式。双 seed:默认 fracturing 广域扫 + `-s` 固定驻留档。
- **实现落点**:`get_testspecific_knob_value_int` 先例(openblas mdim),每测试一个旋钮读取,init 里选操作数生成策略;文档说明两档用途。

---

## 4. 检测强度修复(前置必做,否则新增激发是白做)

### P0-2a: 弱校验升级 byte-exact

- fma.cpp:21-30 与 fma_patterns(1e-6 容差)、acl_gemm.cpp:118-127(1e-3)对 1-bit 翻转全盲(D§4.8;fsu_byteexact_arm.cpp:10-15 注释已自我诊断此问题但原测试未改)。SEVI Obs.8/17:exponent/符号位翻转误差达 10240——容差校验会放过 HEHM 类 SDC。
- **改法**:golden 用 `fmaf/fma` 标量同序重算 + memcmp_or_fail(fsu_byteexact 模式);acl_gemm 若 ACL 库 kernel 与标量序不可复现,则改"ACL 输出 vs OpenBLAS 同输入输出"双库交叉比对(不同实现偶发同错概率极低,SEVI 向量 vs 标量参考的同理)。
- **注意**:升级后可能暴露既有真差异(1e-6 内的不同舍入路径)——先在 Kunpeng 920 全量跑确认无假阳再上 PROD;若有合法 ULP 差,记录并降为 BETA 或修 golden 序。

### P0-2b: RNG 可重放修复

- 20 个文件(fma/fma_patterns/fmatail/gather 家族)用 `std::mt19937(std::random_device{}())`,违反 writing_tests.md 可重放要求(D§4.9):失败无法 `-s seed` 复现,证据丢失。统一换框架 RNG。

### 配套:库输出全量 golden 纪律

- ITHICA:8 台机器的错误只在 Zlib/OpenSSL 库内部可检出,最终输出被 logical masking 掩蔽——压缩/哈希测试的校验必须覆盖完整输出缓冲(现有 zstd/zlib/ipsec 已是全量比对,维持;新增库测试时作为硬要求)。

---

## 5. 第二层:campaign 模式与框架增强

### P2-1: 簇锚定 campaign 模式(框架级,对已知故障的唯一解)

- **为什么**:NUMA3 故障簇锚定(cpus 114-151)+ ~19min 节律,而默认单轮 5s、线程按全体 CPU 铺开(B§5.2/5.8)。HPL 70h 零复现已证明"时间长度"单独不够——需要**簇内满饱和 × 长驻留 × 健康簇对照**三者齐备。
- **形态**:
  - CLI:`--cpus 114-151` 已存在(`--cpuset`),补语义化别名(如 `--cluster 3` 从 sysfs cluster_cpus_list 解析簇边界)+ 文档化"簇锚定用法";
  - 驻留档:`-t 1800+` 长驻模式文档 + scripts/run/ 编排脚本(先例已有);fork 模式天然隔离崩溃;
  - 健康簇对照:同测试同时长跑一个健康簇,差异即证据;
  - 每簇独立结果聚合(YAML 已有 per-cpu 记录,加按簇分组视图)。
- **论文侧**:PinDrop 持续测试(>4 年才首败的机器存在)、Ripple 双节奏(7% 缺陷只有频繁切换触发)——campaign 脚本要有"长驻循环"与"高频轮换"两种节奏模板。

### P2-2: SVE512 di/dt 病毒 `power_virus_dit_sve`

- **为什么**:power_virus_dit 现为 NEON128 burst/stall;0xd22 的 512b 通路下同 burst 瞬态电流与位翻转密度×4(推断,B§5.3);cpu-sdc.md 的 MIS/Vdroop 机理 + SEVI 宽向量低频窗口(Obs.4)同向支持。
- **形态**:burst 段 = SVE FMA 链 + 高 Hamming 操作数交替 + **内嵌地址生成**(对准故障通路);stall 段 = 让流水线排空。
- **已知短板**:power_virus_dit 无跨核同步栅栏,lockstep 相位实际漂移(D§3)——SVE 版若要真 lockstep,需先给框架加最小 barrier helper(或接受相位随机、靠簇饱和本身的统计效应;实施时二选一并记录)。

### P2-3: 跨簇共享数据 × FP 计算(mesh FP 版)

- **为什么**:mesh_upi 27 个测试全整型,没有一个跨核测试在共享数据上做浮点(D§4.7);而已知签名正是"远端/共享访问 × SVE 计算"复合(B§2)。
- **形态**:mesh 的生产-消费对模式,载荷从 int32x4 换成 f64 SVE 向量:生产者 FMA 链写共享行,消费者读后做同序 FMA,双端 byte-exact;变体覆盖 symm/asymm/L3 尺寸(mesh 现有维度)。
- **落点**:tests/cpu/mesh/ 加 FP 变体文件(meson sourceset 追加),或 arm64 下新文件——实施时看 mesh 框架复用度。

### P2-4: 诊断与可观测性(把激发变成可判读证据)

1. **崩溃 si_addr 位签名自动解析**:崩溃时自动检查 VA[55:48] 是否非零 + 低 48 位是否为合法映射域——直接命中已知签名则报告"NUMA3 VA 通路签名匹配"(B§5.8;判别判据来自 memory 档案)。落点:child_debug.cpp CrashContext 处理。
2. **失败 lane 直方图**:memcmp_or_fail 失败时输出 lane 编号分布(SEVI Obs.13:98.5% 单 lane、96% 相邻——相邻 lane 模式是"单物理单元坏"指纹)。memcmp_or_fail 已打印 64B hex,补 lane 位置解析即可。
3. **VA[63:48] canary 常态化**:stencil_axis 的 canary 手法推广为可选 helper(测试自查关键指针高位),零成本预警高位翻转。
4. **PMU/SPE 伴随监控(可选档)**:campaign 脚本伴随采 mem_stall_*/l1d/l2 refill/remote_access;0xd22 imp-def 事件需先 raw 扫描标定(B§4,920 方法可移植);SPE 可用(cn23154 实测)。命中故障时导出簇级快照,把 19min 节律与微架构事件对齐。

---

## 6. 面向未来硅片:SME/SME2(P3)

- **结论**:cn23154 **硬件有 SME+SME2**(SMFR0 f64f64/b16f32/f32f32/i8i32=1,fa64=0),但 Kylin 5.10 内核不暴露 HWCAP2_SME(主线 5.19 才引入)且未做 ZA/streaming 上下文管理——用户态 SMSTART 预期 SIGILL,**需 SIGILL-probe 实测定论,不能假设可跑**(C§4/§5.1)。服务器 landscape:V2 代(Grace/Graviton4/Cobalt)全无 SME,V3(2024+)才标配。
- **建议形态**:"现在写、运行时干净 skip"入库:
  - 特性检测零改动(cpu_feature_sme bit 55 + 16 子位已生成,C§1.1 实测——比预想完整得多);
  - 新库 `tests_arm64_sme`(`-march=armv9-a+sme`),**仅 clang 构建**(GCC 12.3 arm_sme.h 是空壳、不支持 +sme2;clang 17 实测 830 ZA intrinsics 完整;目标机 BiSheng clang 19.1.7 推断完整)——meson 按编译器能力条件构建,GCC 下不建库并打印降级消息(third-party 探测惯例);
  - 测试内容:FMOPA/FMOPS 外积链 + ZA 读写(svld1_hor_za/svread)+ SMSTART/SMSTOP 高频切换(流水线冲刷/power gating 抖动是 SME 独有压力);
  - 探测:init 首行 getauxval(AT_HWCAP2) & HWCAP2_SME → 0 干净 skip(遵循 sleef_sve 手写探测模式);可选 SIGILL-probe 子进程实测内核态度;
  - fa64=0 约束:计算核用 `__arm_locally_streaming` 函数隔离,骨架/比较留在非流模式;SMSTOP 后再 report。
- **价值定位**:前瞻布局(独立矩阵引擎硅片面积,FMOPA 是全 ISA 最高 FLOPS/cyc 路径);当下对 cn23154 的即战力是 §3 的 SVE2,不是 SME。

### P3-2: simd-arm.conf 修正(顺手小 patch)

`arch=neoverse_v2 neoverse_n2 sve2p1,sme,sme2`(conf:136)与公开资料矛盾(V2 无 SME)——会导致 dump-cpu-info 误报。单独一个小 patch 修掉(C§1.1)。

---

## 7. 实施路线图(映射 one-patch-per-unit)

> 顺序原则:先修检测(P0-2,否则新激发测不出)→ 最高价值新测试(P0-1)→ 并行铺指令族(P1)→ campaign/框架(P2)→ SME(P3)。每个 patch 一个单元,全部按 CLAUDE.md 自验证(ninja 零告警 + 实际运行输出 + zstd19 回归 + x86 不动)。

| 波次 | 内容 | patch 数 | 本机可验证到 | cn23154 复验点 |
|---|---|---|---|---|
| W1 | P0-2a 弱校验升级、P0-2b RNG 修复、P3-2 conf 修正 | ~4 | 全量跑(本机即真硬件) | — |
| W2 | P0-1 nt_reload 家族 | 1 | 编译+代码生成+skip | 单核先跑,再满簇 |
| W3 | P1-1 fmmla、P1-3 fcmla、P1-4 谓词、P1-6 gather 扩展 | ~4 | 同上 | 各自独立跑 |
| W4 | P1-2 sve2 跨精度(GCC 探针先行)、P1-5 fpcr、P1-7 双值域旋钮 | ~4 | 同上(fpcr 本机可真跑) | sve2 族 |
| W5 | P2-1 簇锚定 campaign、P2-4 诊断增强 | ~3 | 本机框架级验证 | **核心复现场景** |
| W6 | P2-2 dit_sve、P2-3 mesh FP | 2 | 编译+skip | 满簇 |
| W7 | P3-1 SME 库+测试(clang-only) | ~3 | clang 编译+skip | SIGILL-probe 定论 |

关键依赖:P1-2 依赖编译探针结果;P2-2 的 lockstep 依赖 barrier 决策;W5 是对已知故障复现的主战场(与 memory 记录的"已知软件 bug 清零后若再崩,崩点即新证据"衔接)。

## 8. 诚实边界

- 当次开发机(Kunpeng 920,part 0xd01)无 SVE/SVE2/SME:§3 全部新测试在那台机上只能验证到编译 + SVE 代码生成(反汇编)+ 干净 skip;真实激发效果只能在 cn23154 验证。这与 sve512 家族的既有验证口径一致。**注意:本仓已有 SVE 能力各异的板子(如 SVE-256 且无 SVE2/SME 的一档),换机请按 CLAUDE.md 第 2 条探测,勿沿用本条旧设定。**
- SEVI 的"宽向量低频窗口"是从 AVX2 256-bit 外推到 SVE 512-bit(推断,论文未测 SVE);FMMLA/BF16/FCMLA 的具体 SDC 易损性无直接论文数据,依据是"独立数据通路 + 新代际"两条间接原则。
- cn23154 核内 OoO 参数(ROB/PRF/端口)未公开(B 附录),涉及参数的测试设计(如 reload→use 距离档位)以扫描代替精确值。
- SME 在 cn23154 的用户态可达性未定(内核 5.10);硅片支持矩阵部分来自 WebSearch 摘要(可信度降级,C§来源声明)。
- 目标机树是手工改造版(库名 tests_arm64_sve512、含他人本地新增 sme_fmopa_arm.cpp)——部署新测试时不能整文件覆盖其 meson.build,需按文件同步(memory 记录)。
