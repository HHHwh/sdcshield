# SDC 故障注入完整实验方案（SDCShield ARM64）

> 撰写：2026-09-22
> 依据：`docs/paper/ref/research-notes/`（A-papers 9 篇精读 + B/C/D 三路调研 + ENHANCEMENT_PROPOSAL 19 任务 + experiments/E01–E31 复现档案 + SYNTHESIS 31 篇综合）+ `docs/paper/SDC_RESEARCH_SYNTHESIS_CN.md` + gem5-fi 仓（`/home/sdc/wangxu/gem5-fi-fuzz`，github.com/wangxumarshall/gem5-fi）既有 FI 基础设施实测盘点。
> 性质：**研究实验方案，本文件不包含任何已写代码**。实施按 CLAUDE.md 纪律走 `superpowers:writing-plans` → one-patch-per-unit（§7 路线图）。
> 诚实声明：所有引用数字均转抄自上述档案（档案逐页核对过 PDF 原文）；推断处标注【推断】；论文身份按 SYNTHESIS §0 勘误（E19 文件实为 Mukherjee Top Picks、E20 文件实为 Biswas ISCA'05、E22 实为 SIGMETRICS/Performance'06）。

---

## 0. 执行摘要

**要解决的问题**：SDCShield 的新激发套件（feat/sdc-excitation-enhancement-20260917，19 任务，PROD 279→291）需要一个**可量化、可复现、可与文献锚点对打**的检出能力证据。真实 SDC 缺陷太稀少（Meta 4 年 0.035% 机器、阿里 32 个月 3.61‱，E02/E05），无法作为统计样本；手头真实缺陷只有两处（本机 CORE179、cn23154 NUMA3 簇），各是单一签名，也撑不起覆盖率结论。**故障注入（FI）是文献共识的替代路径**：golden-run 基线 + 受控注入 + 失效分类，雅典系（E14/E17/E23/E26/E27/E29/E30）、Orthrus（E04）、ITHICA（E11）全部依赖它。

**本方案的三层证据结构**（对抗 E24 VulnStack 层间外推失真——PVF vs AVF 排序相反 27–31%）：

| 层 | 手段 | 度量什么 | 不能度量什么 |
|---|---|---|---|
| **L1 真机注入** | ptrace 运行时在架构态（寄存器/内存/FPCR）位翻 + 指令跳过 | **检测端**：给定一个已成形于架构态的错误，套件各测试能否捕获（校验强度 + 数据流传播 + 逃逸面） | 真实缺陷的激发率（微架构/电气层之下的东西） |
| **L2 模拟器结构注入** | gem5-fi 仓既有 19+ 注入器，SDCShield 内核化作负载 | **激发端**：结构级故障（L1D/PRF/TLB/FSU/旁路…）经全栈传播到输出的 SDC 暴露率 | 真实硅片（仿真器间差异 7.20pp > ISA 间 0.55pp，E17） |
| **L3 真实缺陷现场** | 本机 CORE179 + cn23154 NUMA3（既有协议，不重复设计） | 终极有效性：自然缺陷检出 + 已知签名回放验收 | 统计覆盖面（n=2 的自然样本） |

**L1 是本方案的新增核心**：一个外部 ptrace 注入器（`sdcfi`，PTRACE_SEIZE/INTERRUPT，不改任何被测代码），双模式（时间随机 = 无偏传播率；定点断言 = 单元检出率，Orthrus Table 2 同构），三判定通道（测试判定 / 全内存指纹 / 审计钩子），六类失效分类 + Leveugle 统计口径。关键预期结果（预注册，可被推翻）：

1. byte-exact 测试对传播到比对数据的错误检出率 ≈100%，容差测试（旧 fma 1e-6）对尾数 1-bit 翻逃逸率 ≈100%——**P0-2 修复的实证**（锚 SEVI：exponent/符号位也翻，误差达 10240）；
2. 长链测试（sve512 512 步链、openblas 微核）的注入传播率显著高于短序列基线（SiliFuzz 平均 18.6 指令/snapshot）——**结构性优势的直接度量**；
3. 全内存指纹通道可捕获"输出正确但状态被污染"的 ESC 类逃逸（E23/E24：不可见 SDC 占故障效应 29%/最高 62%）；
4. SVD 类迭代算法存在可量化的自愈吸收（GemFI 教训：收敛性迭代"洗掉"错误）——第一次在真机上量化这个假说。

**预算**：L1 全矩阵 ≈13 万次注入运行（§3.8 分解），本机 192 核并行数小时内完成；L2 复用 gem5-fi campaign.py，n=384（5%@95%）标准、关键 cell 663；总实施 10 个波次（W0–W9），映射 one-patch-per-unit。

---

## 1. 背景与动机

### 1.1 为什么需要故障注入

1. **真实缺陷不可作为统计样本**：PinDrop（E02）0.035% 机器终生 ≥1 次 SDC 测试失败（≈1/2850，架构间 0.014%–0.178%）；SOSP23（E05）总患病率 3.61‱。任何"检出率"结论若依赖真实坏机，需要的机器规模只有 Meta/阿里有。
2. **FI 是文献共识的检出度量方法**：golden-run + 注入 + SDC/Crash/Masked/Timeout 分类——8 份模拟器 FI 档案全部采用（E14/E15/E16/E17/E24/E26/E27 交叉）；真机侧 Orthrus（E04）用它测运行时检测器检出率（Table 2：等核 97.2/97.6/97.6/98.9%）；ITHICA（E11）的 EDR/EF/TTD 指标族同源。**SYNTHESIS §4.2 对照组 A 已把它列为 SDCShield 论文的对打轴**："注入检出率（按四单元分桶）/ 首检时间分布 / 单位 CPU 时间的有效激发密度"。
3. **SYNTHESIS T0 复现优先级的现状**：T0-2（CHAOS gem5 注入）**已被 gem5-fi 仓大幅超越**（上游 CHAOS 3 模块 → 本仓 19+ 注入器，含 cache 字段级/TLB 活页/RAT-ROB read-trace/FSU formal 数据）；T0-1（SiliFuzz aarch64 对打）仍未做，本方案 L1 将其纳入。**真机 ptrace 注入层（L1）是文献空位**：E04 档案明确"现有框架 x86 专属、ARM64 需重写"，E04/E11/E01 均无 SVE/ARM64 注入口径——L1 本身是可发表增量。

### 1.2 研究问题

| RQ | 问题 | 层 | 论文叙事对应（SYNTHESIS §4.1） |
|---|---|---|---|
| **RQ1 检出端强度** | 给定已成形于架构态的错误（位翻/跳指令/配置位翻），各测试的检出率是多少？按单元分桶（Arithmetic/FP/Vector/Cache，Orthrus 口径）怎么分布？ | L1 | 主张 3（相对容差校验：byte-exact 唯一安全） |
| **RQ2 逃逸面** | 哪些错误形态逃逸？容差校验放过多小的位翻（P0-2 修复实证）？库内部错误被 logical masking 的比率（ITHICA 8 台仅库内检出）？迭代算法自愈吸收率（SVD）？ESC 不可见 SDC 在真机上存在吗？ | L1 | 主张 3 + ITHICA 互证 |
| **RQ3 对基线的优势** | 同机同预算下，新套件 vs SiliFuzz aarch64：注入传播率 / 检出率 / 单位 CPU 时间检出密度 / 指令族覆盖 的差异？ | L1 | 主张 1（相对随机短测：短前缀对乘法器崩塌、execution context 不可积累） |
| **RQ4 激发端暴露** | 模拟器结构注入下，SDCShield 内核（FMA 链/GEMM/SVD/gather/stencil）对 L1D/PRF/FSU/TLB 故障的 SDC 暴露率是否高于基线负载（MiBench 锚点）？ | L2 | 主张 2（激发结构复刻）+ 主张 4（不可被软件层模型替代） |
| **RQ5 终极有效性** | 自然缺陷（CORE179/NUMA3）上套件真实检出 + 已知签名回放注入的验收 | L3 | 全部主张的落地点 |

### 1.3 层间失真防线（必须前置声明）

E24（VulnStack）实测：**PVF（程序层注入）与 AVF（微架构注入）的结构排序相反率 27–31%**；Δ-encoding 加固后高层测量报改善 3.3–3.8×、真实 AVF 反升 1.3×（sha 2.3%→3.0%）。E17（DifferentialFI）：同 ISA 跨仿真器差异 7.20 个百分点 > 跨 ISA 0.55 个百分点。**因此**：

- L1 的架构态注入结果**只回答检测端问题**（"测试能不能抓住这个错误"），不得表述为"该测试对该单元真实缺陷的检出率"；
- L2 的结构注入结果**只在该仿真器口径内比较**（同配置、同分类器、同 oracle），锚点数字只做排序对照；
- L3 是唯一能裁决"激发真的发生"的层；
- 三层结论在论文中分别报告，**禁止跨层拼接比例数字**（SYNTHESIS §4.4-5 口径纪律）。

---

## 2. 统一实验学（三层共用）

### 2.1 故障模型谱系（F1–F7）

| # | 模型 | 操作定义 | 文献依据 | 预期主要结局 |
|---|---|---|---|---|
| **F1** | 瞬态单 bit 寄存器翻 | 随机选 victim 寄存器（x0–x28 / v0–v31）随机翻 1 bit | SEVI/雅典系主流瞬态模型；E29 中子束 >95% 单 bit | Detected（活值）/ Masked（死值）/ Crash（作地址） |
| **F2** | 瞬态多 bit 相邻 lane | 向量寄存器相邻两 lane 同 bit 位翻转 | SEVI Obs.13（多 lane 时 96% 相邻）；DelayAVF（~50% SDF 多位错）；PinDrop Obs.11（多位>单位） | Detected（整块 memcmp 捕获——验证"整块比对不可用抽检替代"） |
| **F3** | 定点重复注入（间歇近似） | 同一注入点（同 PC/同寄存器/同 bit）在运行期内重复施加 K 次 | Orthrus stuckat 系；SOSP23 边际缺陷间歇发作 | 与 F1 对比量化"重复暴露"的检出增益 |
| **F4** | 指令跳过（nop 等价） | victim 停点处 PC 前跳 4 字节（跳过一条指令） | Orthrus 四故障型之 nop | Detected/Masked（From Gates：控制流错误从不 SDC——PC/跳转指令落点预期 Crash） |
| **F5** | FPCR 配置位翻 | 翻 FPCR.RMode（2 bit）/FZ（1 bit） | SOSP23 配置×边界最脆弱组合；本仓 fpcr_rounding_cartesian 测试的对偶面 | Detected（若 golden 按档位算）/ **静默数值漂移**（若 golden 固定 RNE——揭示配置盲区） |
| **F6** | 内存数据位翻 | 测试工作集（匿名堆页）随机 offset 翻 1 bit | CHAOS CHAOSMem 同构；L1D/主存数据通路 emulation | Detected（写脏整行再回读的测试）/ **Escaped-ESC**（驻留不比对区域——全内存指纹通道专捕） |
| **F7** | 已知签名回放 | 定点：SVE 访存前翻 base 寄存器 VA[55:48]（NUMA3 签名）/ 翻 store 数据通路尾数位（CORE179 形态：float 尾数 85%/double 93%，每次 10–39 bit） | B§2 故障签名实据；docs/cases/CORE179_SDC_REPORT_CN.md | **验收注入**：对应测试必须 100% 检出（不是统计，是断言） |

**永久 stuck-at 的诚实处理**：真机用户态无法拦截"每次寄存器写都重施加翻转"（需持续 trace，代价与扰动不可接受）——L1 不做真永久，用 F3（定点重复）作近似并明示；**真永久仅 L2 做**（gem5-fi CHAOS 系全部支持 permanent 重施加）。

### 2.2 失效分类学（六类 + 双分母）

分类在注入 run 结束后由分析器依据三通道（§3.3）自动判定：

| 类 | 操作判据 | 文献对应 |
|---|---|---|
| **Detected-SDC** | 测试报告 fail（memcmp 失配 / report_fail）且注入 run 与 golden run 存在可比对差异 | SDC 检出（所有 FI 文献共识） |
| **Escaped-SDC** | 测试 **pass**，但全内存指纹 ≠ golden（错误传播到了状态却未被比对覆盖）——**最坏情形，专有通道捕获** | E23/E24 不可见 SDC（ESC 29%/62%）；ITHICA 一致错误漏检的反面 |
| **Masked** | 测试 pass 且全内存指纹 == golden（错误未留存） | Masked（共识） |
| **Crash(DUE)** | 子进程崩溃（SIGSEGV/SIGILL/SIGABRT…，CrashContext 报告） | Crash/DUE（E17/E26 六分类） |
| **Timeout** | 运行时长 > 3× golden 同配置时长（runner 强杀） | E17/E26：Timeout 阈值 = 3× 无故障时间 |
| **Infra** | 注入落点在框架/libc/分配器代码段（非测试计算路径）——剔出主统计、单独报告 | Orthrus Profiling 阶段同思想（只注入被执行的目标指令） |

**双分母口径**（继承 gem5-fi CE-6 + E23 §9.8 教训）：每个比例同时报 **raw**（分母=全部有效注入，剔 Infra）与 **active**（分母=产生任何可观测效应的注入）。E23 Fig.3 的 53.4% 等是条件概率口径（非 Benign 中 SDC 概率）——与 AVF 口径（全部注入）**禁止混拼**，引用时逐条标注分母。

**两阶段分开报告**（E23 框架）：第一阶段 P(效应存在)（指纹改变 ∪ 检出 ∪ 崩溃），第二阶段 P(检出 | 效应存在)——检测器的条件检出率。两阶段乘积才是 raw 检出率。

### 2.3 单元分桶（Orthrus 四桶的 aarch64 映射）

Orthrus（E04 附录 A.4）按 x86 MIR opcode 分四桶并按阿里 SOSP23 观察配比 ALU:SIMD:FPU:cache=1:2:2:1。aarch64 映射（定点注入模式的落点分类器）：

| 桶 | 判据（注入点 PC 处指令 / victim 寄存器数据流角色） | 对标 |
|---|---|---|
| **Vector** | NEON（advSIMD）/SVE/SME 指令（含 FMLA/FMMLA/gather/scatter/谓词） | Orthrus SIMD 桶 |
| **FPU** | 标量 FP 指令（fadd/fmul/fmadd 标量形式、FCVT、FPCR 相关） | Orthrus FPU 桶 |
| **Cache/一致性** | 独占访问（LDXR/STXR/LSE 原子）、锁路径、cache 维护指令（dc civac） | Orthrus cache 桶（原子原语之间=一致性单元） |
| **ALU/AGU** | 其余整数运算与地址生成 | Orthrus ALU 桶 |

分类实现：注入器记录停点 PC → 符号表（nm，16665 个 text 符号实测可用）定位函数 → objdump 反汇编段内指令分类。配比声明：**不强行复刻 1:2:2:1**（那是阿里 x86 机队观察），本实验按落点自然分布 + 各桶分别达到样本量，报告时给出实际桶分布（Orthrus 自己也声明配比是模型假设，单点来源，E04 §9）。

### 2.4 统计口径

1. **样本量**（Leveugle DATE'09，全部档案唯一共同依据；二项最坏情形 p=0.5 正态近似 n=(z/2e)²，本仓复算全部吻合）：
   - n=384 → ±5%@95%（gem5-fi 仓 §4.6 标准，E16 Low 档）
   - n=663 → ±5%@99%（E17）
   - **n=1000 → ±4%@99%（L1 主矩阵标准；E29/E27 口径）**
   - n=2000 → ±2.88%@99%（关键对比 cell；E23/E17/E26 口径）
   - 引用陷阱：n=1000 在文献族有两个口径（E14 记 3%@95%，E29/E27 记 4%@99%）——本方案统一 **4%@99** 并在论文中写明置信水平。
2. **置信区间**：每个比例附 Wilson 95% CI（gem5-fi classify.py 既有实现沿用；比例量正确分布）。反例警示：E28 因图无误差棒被档案 §9.4 列为缺陷。
3. **配对比较**：老 vs 新校验（P0-2 实证）用**同一故障规格清单回放**（同 test/同 seed/同注入点/同 bit）——McNemar 检验；这是比两独立样本强得多的设计。
4. **种子方差**：每配置 ≥3 seed，报告跨 seed 极差（锚：Harpocrates++ 种子影响 <1%–17%，E09 Fig.5）。
5. **多重比较**：逐 cell 报 CI，不做事后全池合并；聚合仅在声明加权方式后进行（执行时间加权 wAVF 口径，E14 §V-A；或几何平均，E28——两种不混用）。
6. **逃逸率上界的阴性结论**：n=1000 全零逃逸 → 95% 置信上界 0.3%（rule of three 3/n）——用于"byte-exact 逃逸为 0"的严谨表述。

### 2.5 引用与报告纪律（直接沿用 SYNTHESIS §4.4，追加注入特有条款）

1. 分场景报告（Aging 教训：FPU R 场景随机基线反超结构化）；
2. 机器级 vs incident 级分开（SEVI 100% vs ~60%）；
3. 崩溃与 SDC 分开统计（Veritas：机队 crash 是 SDC 的 2–3 倍）；
4. 检出 ≠ 定位，措辞"疑似 XX 指令族相关"（ITHICA：44% 服务器同一缺陷跨多指令类型）；
5. 单机/小集群规模边界 → "激发效率"叙事 + 0 检出的统计置信上界；
6. 输入分布显式声明并参数化（SEVI 未披露 RNG 是复现最大缺口，245× 值域差）；
7. 注入特有：**均匀随机故障点 ≠ 真实故障物理**（E15/E16 自认）——绝对百分比只在本实验口径内比较；
8. **ED 教训（gem5-fi CE-1 实证）**：直接检出率优先于任何复合覆盖预测子（自研 ED 度量 AUC 0.51–0.55 落败于论文 ACE 标量 0.60–0.67）；本方案一切结论以注入结局为准，覆盖率只作辅助解释变量；
9. oracle 超时按负载校准（gem5-fi 教训：conflict_seq golden 8min > 默认 300s 造成假象）——每测试的 Timeout 阈值取 3× 该测试实测 golden 时长，不用全局常数。

---

## 3. L1：真机 ptrace 故障注入（核心层）

### 3.1 资产盘点与前置验证（W0 试点，全部通过才进 W1）

| # | 验证项 | 方法 | 已知状态 |
|---|---|---|---|
| W0-1 | Yama ptrace 权限 | `cat /proc/sys/kernel/yama/ptrace_scope` | **本机已实测 =0（无障碍）**；cn23154（Kylin 5.10）待现场验证；scope=1 时注入器以 sdcshield 祖先进程身份可 trace 后代；scope=2 时降级方案 = 框架 env 门控 `PR_SET_PTRACER_ANY`（一个 FI-only 小补丁） |
| W0-2 | 增强分支构建 | 合 main 或基于 feat/sdc-excitation-enhancement-20260917 出 FI 构建分支 | 本机现 builddir（329 tests）不含增强测试，须重建 |
| W0-3 | **确定性底噪试点（硬门）** | 同配置（setarch -R + -n 1 + 固定 seed + --max-test-loop-count）连跑 5 次干净 run，注入器观察模式抓"终点全内存指纹"（checkpoint 断点处 process_vm_readv 全部匿名私有页，页粒度哈希），两两比对 | 预期差页 = 0（确定性测试）；若稳定小噪声集 → 建立排除页清单；若混乱 → **降级**：全内存指纹通道关闭，六分类退化为 verdict-only（Detected/Crash/Timeout/Pass——Escaped 与 Masked 合并，如实报告降级） |
| W0-4 | 寄存器读写往返 | PTRACE_SEIZE+INTERRUPT+GETREGSET/SETREGSET（NT_PRSTATUS / NT_FPREGSET / **NT_ARM_SVE**）在 920（NEON）与 cn23154（SVE512）各验证一轮"读→改→写→读" | ARM64 SVE regset 是变长 payload，需按内核 `struct user_sve_header` 布局处理；920 无 SVE 走 NT_FPREGSET 128-bit vregs |
| W0-5 | 与框架崩溃机制共存 | 注入后立即 PTRACE_DETACH；验证 CrashContext / --on-crash=context 在注入 run 上仍工作 | 框架 child_debug.cpp:1186 已有 `prctl(PR_SET_PTRACER, getppid())`（父进程崩溃回溯用）；注入器 trace 窗口仅 µs 级（SEIZE→INTERRUPT→改→DETACH），与父进程事后 attach 不冲突；child 在 trace 窗口内崩溃的罕见情形 → 崩溃回溯可能缺失，结局类仍正确（forkfd 照常收尸），如实记录该限制 |
| W0-6 | 注入吞吐定标 | 100 次试点注入测单次 run 端到端开销 | 预算表（§3.8）据此修正 |

### 3.2 注入器设计（`sdcfi`，sdcshield 仓新 `tools/fi/` 目录，独立 meson target）

**形态**：外部 C++ 小工具（~1k 行），零框架依赖，只依赖内核 ptrace API 与 /proc。**不改任何被测测试代码**（测的是现状套件）。

**进程拓扑**：

```
sdcfi（注入器，campaign 每次拉起一个）
  └─ sdcshield -e <test> -n 1 -s <seed> --max-test-loop-count <K> -t <ms>   （直接子进程）
       └─ fork 测试子进程（fork_each_test 默认模式；sdcfi 扫 /proc/*/stat 的 PPid 链发现）
            └─ worker 线程（-n 1 时唯一 worker；victim tid = 非 main 的绑核线程）
```

**双模式**：

1. **时间随机模式**（无偏传播率）：在 [0.2T, 0.9T] 均匀随机取注入时刻（T=该测试 golden 实测时长，RNG 按 (test, iteration) 播种可重放）；到达时刻 PTRACE_SEIZE victim tid → PTRACE_INTERRUPT → wait 停点 → 读寄存器组 → 按故障模型 F1/F2/F5 改写（F4 改 PC；F6 用 PEEKDATA+POKEDATA 改内存字）→ SETREGSET → PTRACE_DETACH。停点 PC 与符号落入记录。**不筛选落点**（死寄存器 → Masked 正是测量对象）；仅 Infra 类事后分类剔除。
2. **定点断言模式**（单元检出率，Orthrus Table 2 同构）：从 nm+objdump 预生成的"注入点清单"（测试热函数内按指令类选点：FMLA/FMMLA=Vector、fmadd 标量=FPU、LDXR/STXR=Cache、add/地址计算=ALU）中选点，在目标指令处下断（POKEDATA 写 BRK #0），命中后按故障模型改**目标指令的目的寄存器**（模拟该指令结果损坏——与 Orthrus"注入指令结果"同语义）→ 恢复指令 → 单步 → DETACH。

**记录**（JSONL，一行一注入）：`{test, seed, iter, mode, fault_model, target_site(pc/symbol/instr_class), victim_reg, bit_index(s), inject_monotonic_ns, outcome_class, ttd_ms, golden_fingerprint_diff_pages, notes}`。

**安全边界**：注入器只 trace 自己的后代进程树；DETACH 保证父进程 wait 语义不被偷走；所有故障规格可序列化重放（配对实验与复现）。

### 3.3 判定三通道

| 通道 | 实现 | 回答 | 开销 |
|---|---|---|---|
| **A 测试判定** | 解析 sdcshield YAML 输出（tests[].result: pass/fail/crash/…）+ 退出码 | Detected / Crash / Timeout / pass | 零 |
| **B 全内存指纹** | 框架加一个 no-op 符号 `sandstone_fi_checkpoint(test)`（child 在 test_cleanup 返回后调用；正常构建零开销）；sdcfi 在该符号下断，停点处 process_vm_readv 全部匿名私有页（排除 [stack]/[vvar] 等）做页粒度 FNV-1a 哈希；与 golden run（同配置无注入）比对 | Escaped（pass 但指纹差）/ Masked（pass 且指纹同）；**捕获 ESC 类不可见 SDC** | 仅 FI run；观察一轮 ~几十 ms |
| **C 审计钩子** | 框架 memcmp_or_fail 在 env `SDCSHIELD_FI_AUDIT=1` 时把 (cpu, 调用序号, 缓冲哈希) 记入旁路文件 | 比对内容的逐次指纹（ Detected 时哪次比对抓到、库测试比对覆盖了什么） | FI-only；热循环有 hashing 开销，定标后用于关键子集 |

通道 B 是 Escaped/Masked 判别的**主通道**（通道 C 是细化诊断）。B 的前提是 W0-3 底噪试点通过（确定性测试 + no-ASLR + 固定 seed + `-n 1`）。**多线程测试**（mesh/spinlock 等，-n>1）指纹不可靠（调度交织噪声）→ 退化为 verdict-only，如实标注。

**注**：通道 B 的能力边界——捕获"终点仍留存"的状态污染；库内部瞬态差异若被后续计算洗掉则不留痕（ITHICA 8 台库内检出的完全等价物不可得，此差距如实声明）。

### 3.4 实验矩阵

**测试集**（按家族选代表，全部来自增强分支；920 可跑 vs cn23154 SVE 族分列）：

| 家族 | 测试（920） | 测试（cn23154） | 为什么选 |
|---|---|---|---|
| FMA/算术链 | fma（新 byte-exact）、fma_patterns_ps、fpu_special_values、fsu_byteexact_arm、power_virus_dit | sve512_f64_chain、sve512_f32_chain、sve512_fmmla、sve512_fcmla、sve2_cross_precision | RQ1 核心弹药（SEVI FMA>92%） |
| GEMM/库级 | openblas_dgemm、eigen_gemm_double_dynamic_square、acl_gemm、sleef_neon | sleef_sve、eigen_svd_cdouble_sve | 乘法密度微核（Harpocrates 58.2% 缺口） |
| 压缩/哈希 | zstd19、zlib1、openssl_sha、ipsec_aes128_cbc_hmac_sha1_avx | — | 库内掩蔽轴（ITHICA Zlib 最多） |
| 迭代算法 | eigen_svd_double、eigen_sparse | — | 自愈吸收假说 |
| 访存/地址 | sve512_gather_scatter_arm（920 上 skip → cn23154 跑）、agu_stress_2src、lsu_store_forward_arm | sve512_gather_scatter（含新变体）、sve512_nt_reload、sve512_stencil_axis | SEVI 76% 错偏移；已知签名通路 |
| 谓词/配置 | fpcr_rounding_cartesian_arm | sve512_pred_ops | SVE 独有通路（F5 的对偶） |
| 多线程（verdict-only） | mesh_upi_avx512_symm_f64、spinlock 一个 | mesh/dit_sve | 一致性类（SOSP23 8/27 必须多线程） |

**矩阵维度**：主矩阵 = 时间随机 × {F1, F2, F6} × n=1000/格 × 3 seed；定点模式 = 4 桶 × 代表测试 × n=1000/桶；F4/F5 = 代表测试子集 n=1000；F3 = F1 的 3 个代表测试 n=1000（K=8 重复）；F7 = 验收断言（nt_reload/gather/stencil，n=300 全检出即 PASS）。上表测试名为家族代表，执行时以增强构建的 `--list-tests` 实际清单逐格核对。

**运行口径**：`setarch -R ./builddir/sdcshield -e <test> -n 1 -s <seed> --max-test-loop-count <K>`，K 按测试定标使单 run 1–3s；**--cpuset 排除 core 179**（本机已知缺陷核，B 通道指纹会被自然缺陷污染）+ 排除 OS 噪声核；多线程子实验 -n 4 单列。

### 3.5 专用子实验

1. **P0-2 修复实证（RQ2 主戏，配对设计）**：从 31cbfc4 的父提交构建"旧校验"fma/fma_patterns（1e-6 容差）与增强分支"新校验"版；同一故障规格清单（定点模式，只注入 fma 结果寄存器，bit 位置分层抽样：尾数低位/尾数高位/指数/符号）各回放 n=2000。预期：旧版尾数 1-bit 逃逸率 ≈100%、指数/符号位 ≈0%（1e-6 容差仍能抓大错）；新版全 0（95% 上界 0.15%）。产出"逃逸率 × bit 位置 × 校验策略"曲线——**SEVI LELM/LEHM/HEHM 分型结论的检测端对偶**。
2. **库内 logical masking 定位**：zstd19/zlib1/openssl_sha，按注入落点符号（libzstd/libcrypto 内部 vs 测试框架代码）分桶报告结局分布——量化"库内部错误被最终输出比对掩盖"的比例（ITHICA 口径的静态近似：真机无法做库内比对，只报告落点分布与逃逸率的关联）。
3. **SVD 迭代自愈假说**：eigen_svd_double × F1 × n=1000（-n 1，避开 192 核 ULP 假阳已知问题）；比较其"指纹改变但 pass"（自愈吸收）比率 vs eigen_gemm（无自愈）同口径——第一次在真机量化 GemFI"收敛性迭代洗掉错误"论断。
4. **控制通路 never-SDC 验证**：SP/LR 各 n=384（预期 Crash 主导、零 SDC——From Gates to SDCs ret/call/push/pop 从不 SDC 的 ARM64 复核；若出现 SDC 则是大发现，如实报告）。
5. **已知签名回放验收（F7）**：① NUMA3 签名——定点在 sve512_nt_reload 的 SVE 访存前翻 base 寄存器 VA[55:48]，断言偏移验证 100% 检出（若不检出 = 测试设计缺陷，回修测试——注入成为测试验收工具）；② CORE179 形态——对 movbe/GEMM 路径定点注入多 bit 尾数翻转（10–39 bit 形态），断言 memcmp 捕获。**这一子实验把"已知故障签名"变成测试套件的验收用例**——L1 与 L3 的闭环。

### 3.6 SiliFuzz aarch64 对打协议（RQ3）

**基线部署**（SYNTHESIS T0-1）：github.com/google/silifuzz（E10 实测 aarch64 已支持、开源），本机构建 + 语料（fxgen/Unicorn 代理生成或官方语料），生产式后台扫描跑通。

**对比轴**（诚实设计——两者"传播后检出率"预期都 ≈100%，因为都是精确比对；差异必须打在结构轴上）：

| 轴 | SDCShield 侧 | SiliFuzz 侧 | 预期差异机理 |
|---|---|---|---|
| **传播率** | P(指纹改变 ∪ fail \| F1 注入) | P(终态改变 \| snapshot 初始态注入) | 长链活值密度 ≫ 18.6 指令短测的死值密度（ITHICA：execution context 需积累） |
| **指令族覆盖** | 套件反汇编清单 × 单元桶 | 语料指令统计 | SVE2/FMMLA/谓词族 vs 语料覆盖（PinDrop Obs.12 新代最高风险） |
| **单位 CPU 时间检出密度** | 检出数/CPU·s（注入 campaign） | snapshot 数/s × 传播率 | 短测高吞吐 vs 长测高单发 |
| **传播后逃逸率** | ≈0（byte-exact）/容差测试>0 | ≈0（全终态比对） | 只在弱校验测试上分化 |

**SiliFuzz 注入实现**：snapshot 执行为 µs 级，ptrace 时间随机会完全打不中 → 改用**初始态注入**（改 snapshot 寄存器/内存一字节后重放，比对期望终态）——语义是"快照执行起点的架构态故障"，与 SDCShield 时间随机注入不完全同轴，**报告时明示口径差异**。试点（W6 门）：验证公开 API 支持 per-snapshot 初始态改写；不支持则该轴降级为"覆盖+密度"两轴，如实声明。

### 3.7 指标定义（全部带文献口径源）

| 指标 | 定义 | 口径源 |
|---|---|---|
| **注入检出率** | P(fail \| 注入)，按桶/按测试，raw 与 active 双分母 | Orthrus Table 2 同构 |
| **传播率** | P(效应存在 \| 注入)（两阶段的第一阶段） | E23 两阶段框架 |
| **条件检出率** | P(fail \| 效应存在) | 检测器本征强度 |
| **逃逸率** | P(pass ∧ 指纹差 \| 效应存在) | ESC/ITHICA 盲区 |
| **EDR / EF / TTD** | EDR=有检出 run 比；EF=错误数/run；TTD=注入时刻→report_fail 时刻 | ITHICA Table 3（ITHICA/Native 锚：EDR 1.78×、EF 7.51×、TTD 0.68） |
| **检出密度** | 检出数 / CPU·s（同预算下） | SYNTHESIS §4.2-③ |
| **位模式分解** | 逃逸/检出的 bit 位置分布（尾数/指数/符号分层） | SEVI LELM/LEHM/HEHM（字段 1/4 阈值） |
| **lane 直方图** | F2 多 bit 注入的检出 lane 分布 | SEVI Obs.13（98.5% 单 lane/96% 相邻） |
| **portfolio 联合检出** | 1−Π(1−pᵢ) 跨测试上界 + 互补性检验（Spearman） | gem5-fi CE-3.3（诚实警示：高相关时增益来自叠加而非互补） |

### 3.8 样本量与预算（按 W0-6 定标修正）

| 块 | 格数 | n/格 | run 数 | 估算（单 run ~2s，192 核并行） |
|---|---|---|---|---|
| 主矩阵 920（13 测试 × F1/F2/F6） | 39 | 1000 | 39,000 | ~8 min/seed ×3 |
| 定点 4 桶 × 6 测试 | 24 | 1000 | 24,000 | ~5 min/seed |
| F4/F5/F3 子集 | ~12 | 1000 | 12,000 | ~3 min |
| P0-2 配对（2 构建 × 3 bit 层） | 6 | 2000 | 12,000 | ~3 min |
| SVD/gather/多线程/控制通路 | ~10 | 384–1000 | ~8,000 | ~2 min |
| 920 小计 | — | — | **~95,000** | **单机数小时** |
| cn23154 SVE 矩阵（10 测试 × F1/F2/F6 + F7） | ~32 | 1000 | 32,000 | 600 核并行 <1h/seed |
| SiliFuzz 对打 | — | — | 语料级（~10⁵ snapshot 重放） | 待试点定标 |

注入器自身开销（SEIZE/INTERRUPT/断点恢复）预计 <5ms/run，相对 1–3s 的 run 可忽略（W0-6 实测定论）。

### 3.9 工具接口规格（后续 writing-plans 的输入）

```
tools/fi/sdcfi --mode random|targeted|observe|replay
               --test <name> --seed <s> --loops <K> --budget-ms <T>
               --fault F1|F2|F3|F4|F5|F6|F7 [--bits <spec>] [--reg <list>] [--site <spec>]
               --golden-fingerprint <file>        # 观察模式产出，注入模式比对
               --record <jsonl>                    # 注入记录
scripts/fi/run_fi_campaign.sh   # 并行拉起（chunk 到核）、收集、去 Infra
scripts/fi/analyze_fi.py        # stdlib-only（容器兼容口径，沿用 scripts/gha 惯例）：
                                # 六分类 + Wilson CI + 双分母 + McNemar + 位模式分解 + portfolio
```

---

## 4. L2：gem5-fi 结构注入（激发端）

### 4.1 已有资产（实测盘点，勿重复建设）

gem5-fi 仓（`/home/sdc/wangxu/gem5-fi-fuzz`）已具备：19+ CHAOS 注入器（Cache 字段级 tag/valid/dirty/repl/coh/victim、ArmTLB dTLB/iTLB/F5 活页/parity、FPU、Exec、BPU、FreeList、IQ、L1DForward、LSQFwd、ExMon、ArmSysReg、AddrPath、RAS、Mem ecc_logic、PhysReg、ROB、RAT）；21+ 内核负载库（golden native==gem5 三方一致）；campaign.py/runner.py/classify.py（manifest v2 schema、六分类+Wilson、双层超时组杀）；kp920_proxy 配置；FS checkpoint 流水线（boot 890s→restore 注入）；SE 模式 SVE VL=512 已验证。**SYNTHESIS T0-2（CHAOS）已被此仓超越一个量级，L2 增量不是建注入器，是接负载**。

### 4.2 增量工作

1. **SDCShield 内核入库**：把套件热循环提炼为独立小内核（与既有 kernel 库同规格：native==gem5 golden 三方一致）：`fma512_chain`（sve512_f64_chain 热循环）、`fmmla_block`、`gather_scatter_idx`、`stencil_reload`（ld1rd/stnt1d 窗口）、`gemm_copy_verify`（openblas copy→compute→verify 形态）、`svd_iter`（自愈对照）。基线负载沿用仓内 MiBench 型锚点（sha/qsort 型）+ 既有 gemm/svd 内核。
2. **战役矩阵**：内核 × {L1D-data, PRF/PhysReg, FSU(CHAOSFPU), L1DForward, TLB-F5, Exec(阴性对照)} × {transient bitflip, permanent SA0/SA1}，n=384/格（仓标准），关键格（L1D×gemm/svd）663。
3. **判定**：仓内 classify 六分类 + 双分母直接沿用；oracle 超时按内核实测 golden 时长 ×3（CE-2 教训条款）。

### 4.3 锚点与预期

| 锚 | 数值 | 用途 |
|---|---|---|
| Arm L1D SDC wAVF（MARVEL） | 20.6%（总 23.8%） | L1D 格 sanity 带 |
| L1D 永久 SDC（CrossISA/ETS） | Arm 5.1–53.3% | permanent 格排序带 |
| PRF 永久 | 零 SDC（未观测到） | 阴性带 |
| sha 类掩蔽最少 / 加法器 Crash>80% | From Gates | 负载类型排序带 |
| gem5-fi 自有 FSU formal | double 13.6–18.0%/float 60.2–65.8%/fma 47.3–53.9%/svd 64.8–69.5% | 同仓可比直接锚 |
| VFADD GEMM 98.7%/VFMUL GEMM 49.6% | ETS'24 | FU 格对照 |

预期：SDCShield 内核的 L1D/FSU SDC 暴露率 ≥ MiBench 基线（结构化长链 + 写脏回读设计），且 `stencil_reload` 在 TLB/AddrPath 注入下暴露率显著（已知签名的模拟器侧复刻——与 L1-F7、L3 三层同源互证）。

### 4.4 方法论教训继承（gem5-fi SDC-ED 战役实证，2026-09-21）

1. **复合覆盖度量不可作为主判据**（ED AUC 0.51–0.55 < ACE 0.60–0.67）——一切结论以注入结局为准；
2. **wrapper/骨架底噪**必须先测（l_short/l_dead 的 ED 被 wrapper 抬高）——L2 内核提炼时骨架最小化 + 底噪 run 基线差分；L1 同理由走 W0-3；
3. **足迹盲区**：地址足迹型激活（conflict_seq 96 窗×4KiB 步进）不被常规覆盖轴看见——L2 分析时对 gather/stencil 类内核补充地址跨度剖面；
4. **分母耦合**：fitness/比例类指标在协议内此消彼长——只用结局计数，不用内生比率做优化目标；
5. **BPU 臂恒 0 是 squash 自愈**（384/384 Masked）——预期为零的臂要预先声明预期，避免"跑出 0"被误读为工具故障。

### 4.5 诚实边界

- 仿真器身份方差（E17：7.20pp > ISA 0.55pp）——L2 数字只在本仓 kp920_proxy 口径内比较与排序，绝对值不外推真机；
- SE 模式无 OS/SoC（E29：OS 使 SDC ×2.1–2.4、SoC 使 DUE ×97.7）——FS checkpoint 战役覆盖内核态，其余如实标注为 SE 口径；
- 永久 stuck-at ≠ 边际时序缺陷（E30 明示）——时序维度本方案不可达（DelayAVF 需门级 netlist，T2 级），如实声明。

---

## 5. L3：真实缺陷验证（既有协议，引用不重设计）

### 5.1 本机 CORE179（docs/cases/CORE179_SDC_REPORT_CN.md：36 样本/562 bit，core 179 锚定从不漂移，ttf 0.27–250s，movbe/GEMM 路径，每次 fail 10–39 bit，float 尾数 85%/double 93%）

- 套件对 179 定点长跑（--cpuset 179）vs 健康核对照——增强套件对已知缺陷的自然检出复验（既有 movbe_dump 探针族已做过，新套件补 SVE2 族——撰写时所用开发机无 SVE（SVE 能力须现场探测，见 CLAUDE.md 第 2 条），故实际只补 fma byte-exact 化与 power_virus 类）；
- 与 L1-F7②（CORE179 形态回放注入）闭环：注入形态来自 179 实测位谱。

### 5.2 cn23154 NUMA3（memory 档案：cpus 114–151，VA[55:48]，~19min 节律，ld1rd/stnt1d 栈重装载窗口，≥36 线程簇饱和，HPL 70h/eigen 68.65h 零复现）

- 执行序 = SYNTHESIS §5 已定：`--selftests -e selftest_sme_sigill_probe` → 单核 `sve512_nt_reload_arm` → 满簇 `run_cluster_anchored.sh`（增强分支 d3a0c25：-C 114-151 -t 1800 -r 4 + 健康簇对照 + -q 快速轮换档）；
- 判读纪律：崩点必须过 VA[55:48] 位签名判读 + 已知软件 bug 清零后崩点才是新证据；
- 与 L1-F7①（NUMA3 签名回放注入）+ L2-`stencil_reload`（TLB/AddrPath 注入）三层同源互证。

### 5.3 L3 在论文中的角色

L1/L2 证明"能检出注入的错误"与"结构暴露高"，L3 证明"真实缺陷也检出"——三层缺一不可的完整证据链；L3 的 n=2 局限如实声明（SYNTHESIS §4.4-6 规模诚实条款）。

---

## 6. 统计设计与分析计划（报告模板）

**论文实验章节的统计声明模板**（依 §2.4，一段式）：

> 本实验对每个`测试 × 故障模型`格均匀随机注入 n ≥ 1,000 次（Leveugle DATE'09 统计抽样；二项最坏情形 ±4% @99% 置信；配对子实验 n=2,000 → ±2.88%）。所有比例以有效注入为分母（raw 口径，剔除 Infra 类），另报 active 口径（分母=有效应注入）；按 Detected-SDC/Escaped-SDC/Masked/Crash/Timeout 六分类，两阶段（效应存在 × 条件检出）分别报告，每项附 Wilson 95% CI。SDC 判定 = 测试判定 ∪ 终点全内存指纹比对（no-ASLR + 固定 seed + 单线程确定性口径）。故障模型为瞬态位翻/多 bit/配置位翻/指令跳过（F1–F7），单 run 单故障假设；永久故障仅 L2 模拟器层。配对比较用 McNemar；阴性结论报 rule-of-three 上界。

**分析产出清单**：①主矩阵六分类热力图（测试 × 故障模型）；②检出率 × 单元桶（对标 Orthrus Table 2 版式）；③P0-2 逃逸率 × bit 位置曲线；④传播率对比（套件 vs SiliFuzz）；⑤TTD 分布（对标 SEVI >80%<1s 带与 ITHICA TTD）；⑥portfolio 联合检出与互补性；⑦L2 激发暴露排序（对标 §4.3 锚带）。

---

## 7. 实施路线图（one-patch-per-unit 映射）

> 纪律：每 patch 一个单元；自验证 = ninja 零新告警 + 真实命令输出引用 + zstd19 回归 + x86 不动；commit -s；推 feature 分支（`feat/sdc-fault-injection`）不推 main。前置：增强分支合入 main（或 FI 分支基于其上）。

| 波次 | 内容 | patch | 验证口径 | 平台 |
|---|---|---|---|---|
| **W0** | 试点验证（§3.1 六项；脚本形态，不改框架） | 0–1 | 每项出实测报告（Yama 值/底噪差页数/SVE regset 往返/吞吐） | 920（+cn23154 现场） |
| **W1** | `sdcfi` 注入器核心（SEIZE/INTERRUPT/读写/DATA 改写/记录，random+observe 模式） | 1 | 自测：observe 指纹 5 连跑零差页；F1 注入一个 fma 测试全链路出六分类 | 920 |
| **W2** | 框架 FI 钩子（`sandstone_fi_checkpoint` no-op 符号 + env 门控 audit） | 1 | 正常构建零行为变化（zstd19 全量回归）+ FI 模式 checkpoint 可断点 | 920 |
| **W3** | 定点模式 + PC→单元分类器（nm/objdump 驱动注入点清单） | 1 | 对 sve512_f64_chain 的 FMLA 点注入被正确归类 Vector 桶 | 920 |
| **W4** | campaign runner + 分析器（run_fi_campaign.sh + analyze_fi.py，stdlib-only） | 1 | 小矩阵（2 测试 × 100 注入）端到端出六分类+CI 报告 | 920 |
| **W5** | 920 主矩阵 + 全部子实验（§3.5）执行与数据落盘 | 1（数据+docs） | 数据完整性校验（格数/n/CI 齐全）；P0-2 曲线出图 | 920 |
| **W6** | SiliFuzz 基线集成 + 对打（试点门：初始态注入 API 可行性） | 1–2 | 覆盖表 + 传播率对比表（或降级声明） | 920 |
| **W7** | cn23154 现场战役（Yama 验证 + SVE 矩阵 + F7 验收 + NUMA3 L3 复验） | 0（运行） | F7 全检出断言 PASS；矩阵数据落盘 | cn23154 |
| **W8** | L2 接入（gem5-fi 仓：内核入库 6 个 + 战役矩阵 + 分析） | gem5-fi 仓 2–3 | native==gem5 golden 三方一致 + 首批 n=384 战役 | 本机 |
| **W9** | 论文数据整合 + 文档同步（README/docs + 本方案结果回填） | 1 | §6 产出清单齐全 | — |

**依赖链**：W0-3 是 W1–W5 的硬门（不过则 B 通道降级，W2 简化）；W3 依赖 W1；W6 独立可并行；W8 独立可并行（gem5-fi 仓节奏）；W7 需现场窗口。

---

## 8. 风险与未决项

| # | 风险 | 概率×影响 | 缓解 |
|---|---|---|---|
| R1 | 底噪试点不过（指纹通道不可用） | 中×高 | 降级 verdict-only + 审计钩子；六分类退化如实报告（Escaped 并入 Masked 口径声明） |
| R2 | cn23154 Yama/内核限制 ptrace | 低×中 | scope=1 祖先链可行；scope=2 走 env 门控 PR_SET_PTRACER_ANY 框架小补丁；或 root |
| R3 | NT_ARM_SVE 变长 regset 复杂度 | 中×中 | W0-4 先行验证；920 用 NT_FPREGSET 先跑通主矩阵，SVE 扩展后置 |
| R4 | SiliFuzz 初始态注入 API 不暴露 | 中×低 | 对打降级为覆盖+密度两轴（诚实声明口径） |
| R5 | 注入 run 吞吐低于预算 | 低×中 | --max-test-loop-count 降 K；矩阵分优先级（F1 主矩阵先行） |
| R6 | gem5 SE 向量 PRF 注入支持面不足 | 中×中 | L2 先做 L1D/TLB/FSU/Forward 格（仓内已验证）；向量 PRF 格视 CHAOSPhysReg 现状定 |
| R7 | 多线程测试指纹噪声 | 高×低 | 预设 verdict-only（已在矩阵设计中单列） |
| R8 | 增强分支与 main 的合并时序 | — | W0 前置条件：FI 分支基于增强分支或其合入后 |
| R9 | 旧校验构建配对需历史构建树 | 低×低 | git worktree checkout 31cbfc4^ 构建（DCO 历史完整，可复现） |

---

## 9. 诚实边界汇总（论文写作时逐条对应）

1. **架构态注入 ≠ 真实缺陷激发**：L1 测检测端（给定架构级错误能否捕获），真实缺陷是否产生该错误由微架构/电气语境决定（ITHICA execution context；E24 层间失真 27–31%）。L1 结论**不得**表述为"对 X 单元真实缺陷的检出率"。
2. **均匀随机故障点 ≠ 真实故障物理**（E15/E16 自认）；绝对百分比只在口径内比较。
3. **用户态不可达的结构**（PRF 物理位、LSQ、旁路网络、BPU 内部）L1 打不到——这是 Harpocrates 实测的 IRF<5% 盲区的注入侧等价物；只有 L2 覆盖。
4. **永久故障 L1 缺席**（F3 只是间歇近似）；时序/边际缺陷（DelayAVF 域）三层都不可达，需门级（T2 级，明确不做）。
5. **指纹通道的留痕边界**：洗掉的库内瞬态差异不可见（ITHICA 完全等价物不可得）。
6. **-n 1 主口径**：一致性类缺陷（SOSP23 8/27）的注入覆盖仅多线程小样本。
7. **SiliFuzz 对打的口径差**（时间随机 vs 初始态）与降级可能。
8. **L2 仿真器身份方差**（E17）与 SE/FS 口径差异（E29 OS/SoC 因子）。
9. **L3 n=2**（CORE179 + NUMA3），只做有效性证明不做统计覆盖。
10. **n=1000 对 <10⁻⁵ 频率无检出力**（SEVI 教训：P(漏检)≈e⁻¹）——阴性结论只报上界不报"为零"。

---

## 10. 引用锚点总表（档案号 → 数字 → 本方案用途）

| 锚点 | 数值 | 档案 | 用途节 |
|---|---|---|---|
| Orthrus 等核检出 | 97.2/97.6/97.6/98.9%（Arith/FP/Vec/Cache） | E04 Table 2 | §3.7 对标版式 |
| Orthrus 四故障型 + 1:2:2:1 | bitflip/stuckat0/stuckat1/nop | E04 §2.5 | §2.1 F4、§2.3 |
| SEVI FMA 占比 | >75% case / >92% 事故；top-20 指令 19/20 | E01 Obs.2 | §3.4 家族选择 |
| SEVI 位模式 | LELM 62/LEHM 61/HEHM 178；65 符号位；误差达 10240 | E01 Obs.8/17 | §3.5-1 bit 分层 |
| SEVI 值域差 | 245×（bounded vs unbounded） | E01 Obs.19 | 输入分布声明（§2.5-6） |
| SEVI lane | 98.5% 单 lane；96% 相邻 | E01 Obs.13 | §3.7 lane 直方图 |
| SEVI gather | 76% 错偏移、从不 crash、复现≈0 | E01 Obs.9 | §3.4 访存家族 |
| SEVI 首错 | >80% <1s（10K 轮）；长尾 42h | E01 Obs.5 | §6-⑤ TTD 带 |
| ITHICA 指标族 | EDR 1.78×/EF 7.51×/TTD 0.68 vs Native | E11 Table 5 | §3.7 |
| ITHICA 库内掩蔽 | 8 台仅 Zlib/OpenSSL 库内检出 | E11 | §3.5-2 |
| ITHICA 上下文 | 59% 频率不判别；44% 跨指令类型 | E11 Finding 7 | §1.3、§2.5-4 |
| SiliFuzz 短测 | <100B/snapshot、45% 独占发现；平均 18.6 指令（Harpocrates 侧录） | E10 §2/§3；E08/E09 | §3.6 传播率轴 |
| Harpocrates 基线 | OpenDCDiag SSE FP adder 98.5%/mul 58.2%；IRF<5% | E08 Fig.4/5/6 | §3.4 家族、§9-3 |
| Arm L1D SDC | 瞬态 1.2–43%/永久 5.1–53.3%；wAVF 20.6% | E27/E14 | §4.3 锚带 |
| PRF 永久 | 零 SDC（未观测到） | E27 | §4.3 阴性带 |
| From Gates | 加法器 Crash>80%/SDC 0–18%；控制流从不 SDC | E25 | §3.5-4 |
| VulnStack | PVF vs AVF 排序相反 27–31%；ESC 29%/62% | E24 | §1.3、§3.3-B |
| Leveugle | 384/663/1000/2000 = 5%@95%/5%@99%/4%@99%/2.88%@99% | E16/E17/E29/E23 | §2.4 |
| E23 条件概率陷阱 | Fig.3 是"非 Benign 中 SDC 概率"非 AVF | E23 §9.8 | §2.2 双分母 |
| ETS'24 FU | VFADD GEMM 98.7%/VFMUL GEMM 49.6% | E30 | §4.3 |
| GemFI 自愈 | Jacobi 收敛"洗掉"错误 | E15 | §3.5-3 |
| gem5-fi ED 教训 | ED AUC 0.51–0.55 < ACE 0.60–0.67；四缺陷 | gem5-fi docs/sdc-ed | §2.5-8、§4.4 |
| gem5-fi FSU formal | double 13.6–18.0%/float 60.2–65.8%/fma 47.3–53.9%/svd 64.8–69.5% | gem5-fi artifacts | §4.3 同仓锚 |
| CORE179 | 36 样本/562 bit；尾数 float 85%/double 93%；10–39 bit/fail | docs/cases | §3.5-5、§5.1 |
| NUMA3 | VA[55:48]；~19min；ld1rd/stnt1d 窗口；≥36 线程 | B§2/memory | §3.5-5、§5.2 |
