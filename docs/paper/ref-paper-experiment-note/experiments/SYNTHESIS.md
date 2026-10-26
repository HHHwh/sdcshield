# 31 篇 SDC 论文实验复现综合报告

> 2026-09-21,主会话综合(10 路并行 subagent 深读的交叉产物)
> 素材:`docs/paper/ref/experiments/E01–E31`(每篇一份「实验复现档案」,统一 9 节模板,共 6236 行)
> 目的:①给"复现论文实现"排出优先级与最小可行路径;②定义"证明 SDCShield 新实验设计更优"的实验协议(对比轴 + 论文基线数字锚点)。
> 诚实声明:所有数值均转抄自档案(档案又逐页核对 PDF 原文并标注图表号);凡原文未给出一律注明;推断处标注【推断】。

---

## 0. 必知的三个勘误(论文身份修正,影响引用准确性)

读 PDF 原文页脚/内容核实(非猜测):

1. `Measuring_architectural_vulnerability_factors.pdf` 实际是 **Mukherjee 等 IEEE Micro 2003 "MICRO Top Picks" 杂志版**(MICRO'03 的缩写版),**不是 Biswas ISCA'05**;
2. Biswas ISCA'05(地址位 AVF、L1 D-tag)**实际在 `Computing_..._address-based_structures.pdf` 里**(页脚明确 ISCA'05,作者 Biswas/Racunas/Emer/Mukherjee,无 Li)——文件名有误导性;
3. `Applying_AVA_to_Hard_Faults` 2 页短文页脚是 **SIGMETRICS/Performance'06**,不是 DSN'06。

引用时按修正后归属;档案 E19/E20/E22 已按实际内容重写。

---

## 1. 31 篇论文的实验谱系(五类)

| 类别 | 论文(档案号) | 实验形态 | 可复现性核心结论 |
|---|---|---|---|
| **A. 真实机队测量**(8 篇) | SEVI E01、PinDrop E02、Ripple E03、SOSP23 E05、Veritas E06、ITHICA E11、HWSentinel E12、Saboteurs E31 | 数百万台 × 数年 × 持续测试 | 规模不可复现;**协议级**复现(测试形态/判定/统计口径)可行 |
| **B. 真机测试生成/激发**(3 篇) | SiliFuzz E10、Harpocrates×2 E08/E09、Aging-ASPLOS24 E07 | 生成式程序 + 硬件在环/gem5 评估 | **SiliFuzz 开源且支持 aarch64(唯一可真机对打)**;Harpocrates 闭源(gem5 评估);Aging 需门级 netlist(不可行) |
| **C. 运行时检测机制**(2 篇) | Orthrus E04、SparsePMC E13 | 应用内插桩/PMC 异常检测 | **Orthrus 官方 artifact 完整(x86)**;PMC 全流程开源数据集 |
| **D. 模拟器故障注入**(8 篇) | gem5-MARVEL E14、GemFI E15、CHAOS E16、DiffFI E17、MeRLiN E26、VulnStack E24、CrossISA E27、TC23 E23 | gem5/QEMU/RTL 位翻或 stuck-at | **CHAOS 唯一开源且兼容 gem5 20+**;GeFIN 系全闭源;开源 RTL 仅 Ibex/RISC-V |
| **E. AVF 方法论**(7 篇) | Mukherjee E18/E19、Biswas E20、Nair E21、Bower E22、DelayAVF E28、ArmSoftError E29、ETS2024 E30 | 解析建模 + 注入验证 | 公式/协议可直接复用;数值依赖闭源微架构,只能对照排序 |

---

## 2. 复现优先级排序(意图:复现他们的实现)

### T0 — 立即可复现(开源代码/数据可得,真机 ARM64 可跑)

| 优先级 | 对象 | 开源状态(实测) | 最小复现路径 | 预期工作量 |
|---|---|---|---|---|
| **T0-1** | **SiliFuzz**(E10) | github.com/google/silifuzz,**已支持 aarch64**;snapshot 格式/player/词料生成全套 | 在 Kunpeng 920 / cn23154 上跑 aarch64 snapshot 扫描,得到"随机短序列语料 vs SDCShield 结构化长链"的同机对比;FCOS 式 prologue→payload 方法迁移到 cn23154 VA-path 故障复现 | 1–2 周(拉仓库+构建+跑批) |
| **T0-2** | **CHAOS**(E16) | github.com/eliovinciguerra/CHAOS,兼容 gem5 20+ | 在 gem5 ARM ISA 下重跑 Reg/L1D/主存注入;借其 **HPC 指纹方法**(Δmean 20 计数器)给 SDCShield 激发测试加 PMU 旁路观测 | 2–3 周(gem5 环境) |
| **T0-3** | **Orthrus**(E04) | 官方 artifact 完整:运行时+4应用+justfile+docker;FaultInjection 仓库(改造 LLVM+注入数据集) | x86 机器 `just test-all`(~7h)先验证;ARM64 需把单元分类规则改到 aarch64 MIR(NEON/SVE=Vector,LSE=Cache,附录 A.4 声明架构无关) | x86 验证 1 周;aarch64 移植 3–4 周【推断】 |
| **T0-4** | **SparsePMC**(E13) | SuiteSparse 矩阵全开放;perf+sklearn 全开源 | 照抄注入网格(e×i 100 组)+决策树,ARM64 perf 事件清单已在档案 7.1.1 给出;对照实验设计(交替执行/固定亲和)是超越点 | 1 周 |
| **T0-5** | **DelayAVF artifact**(E28) | github.com/viniul/micro-artifact(Docker:Yosys+Verilator+NanGate45,MIT) | 原样跑通 Ibex 全部数值(方法论校验基准);d 扫描曲线形状用于设计电压/频率边缘扫描档位 | 1 周(x86 服务器) |

### T1 — 协议级复现(代码闭源/规模不可得,但方法可完整照搬)

| 优先级 | 对象 | 复现什么 | 关键协议 |
|---|---|---|---|
| **T1-1** | **SEVI**(E01) | 向量-标量交叉验证 + 单指令隔离 + 位模式分析流水线 | 246 用例→ARM64 换 NEON+SVE 指令清单;100 万轮/核;三遍实验(2 同配置+1 lane 统一输入);输入位偏置(>0.8 判偏置位)+ LELM/LEHM/HEHM 三分类(字段 1/4 阈值);256B 读空间记录法(gather 错偏移 vs 无效数据二分);matmul 值域实验(bounded/unbounded × dim{10,25,100})。**官方仓库空壳(实测只有占位 README)**——自行实现即贡献 |
| **T1-2** | **SOSP23 深度实验协议**(E05) | 27 颗坏 CPU 的实验协议 | occurrence frequency(错误/分钟)度量;bitflip mask 统计(XOR→>5% 同 mask 判 pattern);温度三件套(cooling device 读数+stress 预热+lg(freq)-温度最小二乘);温度/压力解耦(压核组+被测核组);**论文原文推荐 OpenDCDiag,SDCShield 天然承接** |
| **T1-3** | **ITHICA**(E11) | EDR/EF/TTD 指标 + 复现器方法论 | LLVM IR pass(Arith/Br/Mem 变换 IR 级架构无关);100×1h/台协议;PC/BB Sensitivity 与 Input Breadth 分析;被测程序(zlib/zstd/OpenSSL)SDCShield 全有 |
| **T1-4** | **Aging-ASPLOS24 的对照实验模板**(E07) | "结构化 vs 随机"基线协议 | 同数量同风格随机指令+随机输入、10 次平均;B/L 跨测试检出统计(测试 i 的故障被 j<i 提前检出);失效三模式(C=0/1/R)分桶报告。**注意:用户预期的 FMAX/电压扫描在原文不存在**(Vega 无扫频扫压实验) |
| **T1-5** | **gem5 注入系**(E14/E23/E24/E25/E26/E27) | 用 CHAOS 替代闭源 GeFIN,复现雅典系结论 | 最小配置:gem5 DerivO3CPU ARM + full-system + MiBench;每结构 1000 注入(4%/99% 口径);对照锚点:L1D SDC 主导(Arm 瞬态 1.2–43%/永久 5.1–53.3%,E27 Fig.3/5)、PRF 永久零 SDC(E27)、ROB/LQ/SQ 零 SDC(E23 Fig.3) |

### T2 — 只能引用(规模/硬件/数据不可得)

机队绝对数值(PinDrop 0.035%、SEVI 0.072‱、SOSP23 3.61‱、Veritas 相对率 4580 vs 21、HWSentinel 1.41×–1.92×)、中子束实测(E29)、门级 netlist(E07/E25 的 Veritas 工具链)、Asim/Intel 内部模拟器(E18)。**用法:作为新实验论文的 related-work 数字锚点与动机论据,不做复现尝试。**

---

## 3. 跨论文实验方法论共识(带数字锚点)

这些是 31 篇交叉后互相印证、可直接写进论文实验设计章节的"铁律":

1. **样本量口径**:每结构 ≥1000 注入 = 误差 4% @99% 置信(gem5-MARVEL/CrossISA/ArmSoftError 共用,溯源 Leveugle);2000 = 2.88%;Harpocrates 收敛 FU≈1000/L1D≈2000/IRF≈5000 代。真机激发实验的"轮数×指令数"设计引用此口径换算等效置信度。
2. **失效分类标准**:SDC/Crash(DUE)/Masked/Timeout 四分类(或六分类,+Assert/non-propagated);**golden-run 逐字节比对**是 SDC 判定的唯一共识(SEVI 用向量-标量双路径、ITHICA 用指令复制、雅典系用 golden diff——同一哲学三种实现)。
3. **结构脆弱性排序(注入侧)**:L1D 是 SDC 主导结构(Arm L1D SDC wAVF 20.6%,E14;SDC/Crash≈6.4:1);PRF/L1I/ROB/LQ/SQ 以 Crash 为主或零 SDC;**组合逻辑(FMA/乘法器)是硬故障脆弱高地**(Bower 加法器 H-AVF 0.1488 >> cache 0.00486,与 SEVI FMA>92% 跨 20 年互证)。
4. **控制流/栈指令故障从不产生 SDC**(From Gates to SDCs E25:ret/call/push/pop 零 SDC)——激发内核应无分支、算术密度最大化。
5. **乘法高位截断是天然掩蔽**(E25:64×64 高 64 位丢弃 = Masked External 主因)——激发内核应让乘法结果全宽参与后续计算。
6. **tag 位几乎打不中**(Biswas E20:L1 D-tag AVF 0.41% = 数据位 6% 的 1/15;例外 store buffer tag 7.7% > data 4%,因持有唯一副本)——地址类激发要构造汉明距-1 邻接,数据类要"写后驻留再读回"。
7. **flush/高周转反而降 AVF**(E20:每 100K 指令 flush → AVF 减半,IPC 仅损 0.19%)——**低周转+唯一副本才是脆弱态,测试应驻留而非抖动**(与直觉相反,重要设计原则)。
8. **多位翻转普遍**:DelayAVF ~50% SDF 诱发多位错;PinDrop 多位>单位;SOSP23 多 bit 常见——**整块 memcmp 不可用抽检替代**(SDCShield 现有设计正确)。
9. **宽发射/宽向量放大脆弱性**:Nair 4-wide vs 2-wide SER +81%;SEVI 256-bit FMA 有 128-bit 没有的 <10⁻⁵ 低频窗口——SVE 512-bit 外推【推断,SEVI 未测 SVE】。
10. **执行上下文是首要预测因子**(ITHICA:指令频率在 59% 案例中不是判别器;热循环加一条 no-op 都可能杀触发)——热循环零扰动纪律。
11. **短前缀对边际/间歇故障不够**(Harpocrates++ 自认:0.01× 前缀时 SSE FP 乘法器检出崩塌到 0%;乘法器需要长序列)——长时压测的直接理论依据,也是 SDCShield 相对 SiliFuzz 短测式的结构性优势。
12. **测试多样性 = 检出率**(PinDrop 91.4% 测试至少失败过一次、31% 曾是唯一检出者;SiliFuzz 随机与覆盖引导互补 45% 独占;SOSP23 633 testcase 中 560 从未检出)。
13. **两层测试节奏互补**(Ripple 7% 缺陷只有频繁切换触发;15 天高频 ≈ 70% 等效覆盖;Fleetscanner 6 个月全舰队轮一遍 93% 覆盖)。
14. **ECC 盲区存在**(DelayAVF:regfile 加 SECDED 后 sAVF=0 但 DelayAVF>0;wordline 延迟读到合法错行数据校验通过)——**硬件校验不可替代功能级 golden 比对**。
15. **层间外推失真**(VulnStack:PVF vs AVF 排序相反 27–31%;Δ-encoding 高层报 3.3–3.8× 改善,AVF 实升 10–30%;同 ISA 跨仿真器差 7.20 点 > 跨 ISA 0.55 点)——**真机全栈实验的正当性论证基石,反对任何"用 x86 数据/软件层模型外推 ARM64"的做法**。

---

## 4. "证明新实验设计更优"的实验协议

### 4.1 核心叙事(从档案证据推出)

SDCShield 新实验(结构化长链激发 + 字节精确 golden + 簇锚定 campaign)的对比优势主张按证据强度排:

1. **相对随机短测**:SiliFuzz 检出画像 IRF<5%、SSE FP≈0(Harpocrates ISCA'24 gem5 SFI 实测),且短 snapshot 无法积累执行上下文状态(ITHICA:execution context 是首要预测因子)+ 短前缀对乘法器崩塌(Harpocrates++ Fig.6)→ 结构化 FMA/SVE 长链在这些轴上应显著更高。
2. **相对快照式/单轮测试**:PinDrop 持续测试(近 4 年才首败的机器存在)、Ripple 7% 只有频繁切换触发、SEVI 低频 <10⁻⁵ 需 42h 深测 → 簇锚定 + ≥30min 驻留 + 双节奏 campaign 是检出长尾的唯一解。
3. **相对容差校验**:SEVI exponent/符号位也翻(相对误差达 10240;65 case 翻符号位)推翻 SOSP23 尾数论 → 字节精确 memcmp 是唯一安全比对(fma 1e-6/acl_gemm 1e-3 容差对 1-bit 翻转全盲——本仓 W1 波次已修复)。
4. **相对软件层模型**:VulnStack 排序相反 27–31% + DiffFI 工具差 7.20 点 > ISA 差 0.55 点 → 真机 ARM64 原生实验不可被 x86 数据/IR 级模型替代。

### 4.2 对比实验矩阵(三组对照,全部真机)

**对照组 A:SDCShield 新套件 vs SiliFuzz aarch64(开源可真机对打)**

| 要素 | 设计 |
|---|---|
| 平台 | 同一批机器(Kunpeng 920 健康机 + cn23154 故障机),同核绑定 |
| SiliFuzz 侧 | 官方 aarch64 词料 + player,同 CPU 时间预算 |
| SDCShield 侧 | 新激发套件(19 任务产物:nt_reload/fmmla/sve2_cross_precision/fcmla/谓词/fpcr/gather 变体/dit_sve/mesh_fp),同 CPU 时间预算 |
| 注入验证(健康机上) | 软件级故障注入构造可控 SDC:ptrace/信号改写寄存器位、或编译器级注入(Orthrus FaultInjection 仓库的 1:2:2:1 单元配比 + 四故障类型口径,Arithmetic/FP/Vector/Cache 直接同表对比其 Table 2:97.2/97.6/97.6/98.9%) |
| 主指标 | ①注入检出率(按四单元分桶);②首检时间分布;③单位 CPU 时间的有效激发密度 |
| 论文锚点 | SiliFuzz IRF<5%/SSE≈0(Harpocrates Fig.4/5/6);Orthrus Table 2;SEVI Fig.7(>80% 首错 <1s 的对照带) |

**对照组 B:campaign 节奏对比(自家消融,证明双节奏必要性)**

| 要要素 | 设计 |
|---|---|
| 变量 | 单轮 5s 快照 / 长驻 30min / 双节奏(长驻+高频切换)/ 簇锚定 × 健康簇对照 |
| 平台 | cn23154 NUMA3 簇(cpus 114-151)+ 健康簇,run_cluster_anchored.sh |
| 主指标 | 已知故障(~19min 节律)的复现率与首现时间;VA[55:48] 签名命中率 |
| 论文锚点 | Ripple 15 天 70% 等效覆盖 / 7% 切换独占;SEVI 42h 深测;PinDrop 持续性;Fleetscanner 93%/23% |
| 判读纪律 | 崩点必须过位签名判读(VA[55:48] 非零 + 低 48 位合法映射域 + NUMA3 锚定),已知软件 bug 清零后崩点才是新证据(memory 记录) |

**对照组 C:激发维度消融(证明每个设计维度有增量)**

| 维度 | 消融轴 | 论文依据 |
|---|---|---|
| 值域 | bounded(-1,1) vs unbounded 双档 | SEVI 245×(Fig.21) |
| 输入熵 | 高熵随机 vs 规整数据 | DelayAVF md5≫libstrstr |
| 指令代际 | NEON(v8.0) vs SVE(v8.4) vs SVE2(v9) | PinDrop Obs.12 首代最高 |
| 向量宽度 | 128 vs 512-bit | SEVI Obs.4(128 vs 256 外推) |
| 依赖距离 | reload→use 1/2/3/4 条扫描 | ITHICA execution context;cn23154 故障窗口 |
| 驻留 vs 抖动 | 写后长驻留再读回 vs 高周转 | Biswas E20(AVF 减半反直觉) |
| 舍入配置 | FPCR 4 模式 × FZ | SOSP23/SEVI 位翻模式谱 |
| PMU 旁路 | 有/无 HPC 指纹记录 | CHAOS(输出正确但 HPC 巨变 = 潜在漏检) |

### 4.3 指标定义(直接借用论文口径,保证可比)

- **检出率**:按注入单元分桶(Orthrus 四类口径) + 按结构画像(雅典系 SDC/Crash/Masked 分类);
- **EDR/EF/TTD**(ITHICA 定义):错误检出率/暴露频率/首错时间——新实验 vs SDCShield 旧 golden 检查的 EDR 比,对标 ITHICA/Native = 1.78×;
- **B/L 跨测试提前检出**(Aging ASPLOS24):测套件冗余度与编排鲁棒性;
- **种子方差**(Harpocrates++ Fig.5 口径):跨种子检出稳定性,对照带 <1%–17%;
- **occurrence frequency**(SOSP23):错误/分钟,按 (test, cpu) 聚合;
- **位翻模式流水线**(SEVI):输入位偏置(>0.8 阈值)+ LELM/LEHM/HEHM(字段 1/4 阈值)+ 符号位单列 + 相对误差 CDF;
- **lane 直方图**(SEVI Obs.13:98.5% 单 lane/96% 相邻——相邻 lane 模式是单物理单元坏指纹)。

### 4.4 写论文时的口径纪律(从各篇"局限与坑"提炼)

1. 分场景报告,防单一场景反例(Aging 教训:FPU R 场景随机基线反超结构化);
2. 机器级 vs incident 级检出率分开(SEVI:100% vs ~60%);
3. 崩溃:SDC 比例单独统计(Veritas:机队 crash 是 SDC 的 2–3 倍);
4. 检出 ≠ 定位:44% 服务器同一缺陷跨多指令类型(ITHICA)——报告措辞用"疑似 XX 指令族相关";
5. 工具对比避免口径混用(CHAOS 教训:每故障开销 vs 整体开销不可直比);
6. 承认单机/小集群规模边界:转"激发效率"叙事——论文用 42h/机检出的东西,用更短时间+更强激发检出;或长期跑积累"0 检出"的置信上界(统计上有意义的负面结果);
7. 输入分布是最大未控制变量(SEVI 未披露 RNG;245× 值域差)——新实验必须显式声明并参数化输入生成;
8. 引用修正后的论文归属(§0 勘误)。

---

## 5. 与 SDCShield 现状的对齐

- 19 任务/20 commits 已实施完成(feat/sdc-excitation-enhancement-20260917,PROD 279→291)——对照组 C 的 SDCShield 侧弹药已在手;
- cn23154 复验入口:`--selftests -e selftest_sme_sigill_probe` → 单核 `sve512_nt_reload_arm` → 满簇 `run_cluster_anchored.sh`(对照组 B 的执行序);
- 当次开发机(Kunpeng 920,part 0xd01)无 SVE:SVE/SVE2 族在那台机上只能验证到编译+代码生成+干净 skip,真实激发在 cn23154——论文实验配置表须如实分列两机角色(换机请按 CLAUDE.md 第 2 条探测,勿沿用"本机无 SVE"旧设定);
- OpenDCDiag 上游基线数字可直接引用并同源对比:Harpocrates 实测其 L1D>80%、SSE FP 加法器 98.5%、**SSE FP 乘法器仅 58.2%**——SDCShield 的纯乘法密度微内核档(P0-2 修复后的 fma 家族 + fmmla 外积)正是补这个 58.2% 缺口的直接论据。

## 6. 档案索引

E01 SEVI / E02 PinDrop / E03 Fleetscanner-Ripple / E04 Orthrus / E05 SOSP23 / E06 Veritas / E07 Aging-ASPLOS24 / E08 Harpocrates-ISCA24 / E09 Harpocrates-Micro26 / E10 SiliFuzz / E11 ITHICA / E12 HardwareSentinel / E13 SparsePMC / E14 gem5-MARVEL / E15 GemFI / E16 CHAOS / E17 DifferentialFI / E18 Mukherjee-MICRO03 / E19 Mukherjee-Micro03-TopPicks / E20 Biswas-ISCA05 / E21 Nair-HPCA12 / E22 Bower-HardFaults / E23 TC23-Perspectives / E24 VulnerabilityStack / E25 FromGatesToSDCs / E26 MeRLiN / E27 CrossISA / E28 DelayAVF / E29 ArmSoftError / E30 ETS2024 / E31 StealthySaboteurs
