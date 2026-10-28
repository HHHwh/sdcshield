# Eigen 用例 P0-P3 修复设计（NUMA 感知 sizing + 内存最大化 + OOM 防护）

日期：2026-10-27（v2，对抗性审查修订版）
状态：设计定稿（brainstorming + LSP 深度核实 + 隔离对抗性审查后修订）
分支：feat/eigen-p0-p3-fix
实测基线：本机 sdc1-01-04（128 核 / 61GB / 4 NUMA node 但**内存全在 node0** /
SVE-256）119 个测量点（Phase A 94 + B 21 + C 4 + R 17，另含多用例连跑验证）。

> v2 修订记录（对抗审查 findings：C1/C2/M1-M8/m1-m8/q1-q4，审查全文见
> 会话记录；v1→v2 变更在各节以【v2】标注）：
> C1 per-node 退化规则；C2 反馈闭环重新定位；M1 预算 240→200s、AR 取实测
> 上界；M2 AR 表二维化（族×拓扑）；M3 三点拟合扣固定开销；M4 节点预算
> 合成公式 + cgroup v1；M5 反馈槽生命周期/static_assert/访问器；M6 no_fork
> 门控；M7 验证判据分族；M8 指称修正（dynamic_square vs max_arm）；
> m1 选择偏差、m2 bad_alloc→SKIP 对齐先例、m3 quick 模式、m4 dumb-plan
> 归属、q1 表口径、q3 gem5 验证面收窄。

## 1. 问题清单（全部实证）

### P0 真实场景稳定失败（产品 bug，两次独立复现）
- **P0-1 `eigen_svd_cdouble_sve`**：全核 auto `-t 5000` → 321s timeout、42.6GB
  （物理 66.7%）。根因：preinit 时间模型 `t(N)=243.4·(N/4400)³+1`（单线程
  标定，svd_cdouble_sve.cpp:177）不含全核争抢；本机 42.6GB 全挤 node0 →
  带宽争抢实测 SVD 族 18-25×（单解 8s → >160s）→ 300s 硬宽限被击穿。
- **P0-2 `eigen_svd_bidiag_sve`**：同场景 343s timeout、16.9GB。根因：**无
  任何时间校准**（`eigen_gemm_double_max_arm` 同样未接——【v2/M8】v1 称
  "唯一"不实），auto mdim 直撞 2048 knob 上限。

### P1 内存预算失效（"最大化"没有发生）
- 模型虚报 5-61×：GEMM 4.7-5.7×、SVD 5.9-7.9×、sparse 23×、Jacobi 34-61×。
  真实场景 eigen 家族只吃物理内存 **5-18%**（最高 11.1GB/61GB）。
- 预算只读 `_SC_PHYS_PAGES` 总量：不看 MemAvailable、不看 cgroup（有
  "job oom" 实测前科，svd_cdouble_sve.cpp:114-120 注释）。
- 无 per-NUMA-node 预算（22 处 sizing 无一感知节点）。
- init 共享矩阵与 128 worker 同刻分配峰值不在模型。【v2/M8】init 共享
  矩阵（3·S·n²/child）与 worker 侧 `gemm_mdim` 重复计算属于
  **dynamic_square 家族**（gemm_double_dynamic_square.cpp:64-74/87）；
  `eigen_gemm_double_max_arm` 无 init 矩阵（Phase A 实测 base=4.3MB 证实）、
  无 worker 重算路径。

### P2 OOM 防护缺失
- SVD/GEMM 模板 `test_run` 无 `bad_alloc` catch（仅 eigen_sparse 有）→ 分配
  失败走 malloc SIGKILL/abort，用户看到被杀而非诊断。
- 显式 `-O` 旁路全部防护（无校准、无预期内存警告），仅剩 knob clamp。

### P3 一致性
- 三套并行 sizing（helper / sve 手写 :109-145 / gemm 手写），系数与上限
  各异（0.65 vs 650‰ vs 850‰；ratio 12 vs 6 vs 2.0）。
- 时间上限与内存上限从不联合求解。
- 卫生：【v2/m5】原列三条中两条已在 f2af45d1（sleef 扫描+tag）与
  82ab5556（merge 标记清理）修复，撤销；仅剩 tests/cpu/meson.build:449-465
  注释过时（仍称 tests_sve 固定 128-bit 锁）。

## 2. 目标 / 非目标

**目标**：
1. P0 两用例在全核 auto 下 `exit: pass`（不再 timeout）。
2. 内存消耗：GEMM/dynamic_square 族 ≥ 现状 2×；SVD/sve/bidiag 族不
   timeout 且峰值 ≥ 现状 80%（时间上限是物理约束，【v2/M7】分族判据）。
   全部用例无 OOM（≤ 预算上界）。
3. 显式 `-O` 超预算时有警告；分配失败有降级而非裸 SIGKILL。
4. sizing 统一到单一 helper，时间/内存联合求解。
5. x86-64 行为 byte-identical（rule 5）。

**非目标**：
- Eigen 上游 complex `ptranspose` 1024 档缺陷修复（单独规划）。
- Jacobi 族的 SVD 算法替换。
- SVE 512/1024 档的本机实测（无硬件；gem5-fi 验证，见 §6）。

## 3. 总体架构

```
[启动/选择期]                      [child test_init]               [worker/child 退出]
sdc_numa_probe() ──┐
  节点/内存/CPU映射  ├─→ sdc_joint_dim_slice() ─→ mdim ─→ 分配 ─→ 计算
  AR 表(族×拓扑)     │   = min(n_mem, n_run200, clamp)          │
                    │   n_mem = sqrt((node_budget−sh)/(node_thr·c)) ↓
sdc_budget_*() ─────┘   n_run200 = (200s/(AR·2·a_ms))^(1/p)  VmHWM(sdc_read_vm_hwm)
                                                             写 PerThreadData::Main
RSS_RATIO_TABLE ──→ c 初值（族→实测系数）                      （每 slice 一槽）
                    ↑                                            │
                    └── 内存受限多 fracture 的 init 读 ◄── EMA ◄┘
                        （SVD 类默认时序不触发，见 §4.4）
```

四个子系统：NUMA 探测与 AR（慢化）分档表、预算升级（含 per-node 与退化
规则）、联合求解 helper、VmHWM 反馈闭环（定位收窄）。P2/P3 是模板层与
卫生收尾。

## 4. 子系统设计

### 4.1 `framework/sdc_numa.{h,cpp}`（新，arm64 门控）
`sdc_numa_probe()`：静态缓存一次。读 `/sys/devices/system/node/nodeN/meminfo`
与 `cpulist` → `struct sdc_numa_topology { int nodes_with_mem; }`。

**【v2/M2】AR（全核慢化）表二维化：慢化是负载×拓扑的函数，不是拓扑属性**
（同拓扑实测 GEMM 4.4× vs SVD 18-25×）。`sdc_ar_factor(family, topo)`：

| 有内存节点数 | 内存密集族（SVD/sparse/bidiag） | 计算密集族（GEMM） | 来源 |
|---|---|---|---|
| 1 | 25 | 5 | SVD 实测带 18-25 取上界；GEMM 实测 4.4+余量 |
| 2 | 10 | 4 | 保守估计，待标定 |
| 4 | 5 | 3 | 保守估计，待标定 |
| ≥8 | 4 | 2.5 | 保守估计 |
| 探测失败 | 25 | 5 | 最差档兜底，永不向上猜 |

保守方向的定义（【v2/M2】明确）：AR **高估** = 时间预算收紧 = mdim 偏小 =
内存欠压（P1 损失）；AR **低估** = 时间超预算 = timeout 风险（P0 复发）。
表值宁可高估（保 P0），P1 靠 §4.4 反馈与族区分兜底。同源锚点：
`sdc_calibrated_dim` 注释的 sparse 24.9×（单节点内存）。

`--max-cores-per-slice` 的 dumb plan（slicing.cpp:104-118）按连续 CPU 切、
可跨节点——slice 归属节点取其**首 CPU 的节点**（近似，注释记录）。【v2/m4】

### 4.2 `sdc_budget_bytes(permille)`（sdc_stress_config.h）
全局：`min(物理×‰/1000, MemAvailable×0.9, cgroup 限额−已用)`；任一来源读
失败即跳过；cgroup **双路径【v2/M4】**：v2 `/sys/fs/cgroup/memory.max −
memory.current`（值为 "max" 跳过），v1 `/sys/fs/cgroup/memory/
memory.limit_in_bytes − memory.usage_in_bytes`（limit ≥ 物理内存视为无限制）。
本机实测为 v1——"job oom" 前科场景恰在 v1。

**per-node 版 `sdc_node_budget_bytes(permille, cpu)`【v2/M4+ C1】**（PR#302 检视 #7 勘误: 第二参数是 **CPU ID** 而非节点号——与实现/声明/调用方一致, 函数内部经 sdc_numa_node_of_cpu 映射到该 CPU 所在节点; 原稿写 node 会让照抄者把节点号当 CPU 号传入、预算记到错节点）：
节点 meminfo **没有 MemAvailable 字段**（实测仅 MemTotal/MemFree/…），合成：
`node_avail ≈ global_MemAvailable × (node_MemFree / Σ_nodes node_MemFree)`；
MemFree=0 的节点分摊为 0。

**退化规则（C1，必须实现）**：`node_threads[n]` = 该节点 CPU 上的 worker 数；
**无内存节点（MemTotal=0）的线程需求并入其内存 fallback 节点**（内核
first-touch 的实际行为，本机即 node0）：`fallback_node` = 有内存节点中
MemFree 最大者；无内存节点的 slice 的预算与线程数都记到 fallback_node 头上。
node0-only 机器：全部线程计入 node0、node_budget ≈ 全局 MemAvailable×0.9 →
**精确退化为全局模型**（与验证行"行为与全局模型对照一致"自洽）。

### 4.3 `sdc_joint_dim_slice()`（联合求解核心，新）

**拟合口径【v2/M3，防 SVD 内存倒退 2.4×】**：`sdc_calibrated_dim` 现有
两点差分拟合对固定开销敏感——`t(N)=243.4·(N/4400)³+1` 的 +1s 在 N=300
探针档占 93%，k_floor 只救指数不救系数，外推系数被抬高 ~14× → 立方根
2.4× 收缩。修正：(a) probe 档抬升阈值从 <1ms 改为 **<100ms** 才翻倍
（使固定开销占比 <10%）；(b) 拟合改为**三点 t=a·n^p+b**（最小二乘或
逐差），系数 a 用扣除 b 后的值；两者取一，推荐 (a)（改动最小）。
`sdc_calibrated_dim` 增加出参 `{a_ms, p}`（默认 nullptr 保持现签名兼容）。

```c
n_opt = min(
    n_mem  = sqrt((node_budget − node_slices·shared) / (node_threads · c)),
    n_run200 = (200s / (AR(family, topo) · 2 · a_ms))^(1/p),   // 300s×0.67
    knob_hi)
```

- `node_threads` = 该节点全部 worker 数（含并入的无内存节点需求）。
  Heuristic 默认 slice≡NUMA（build_topology 分组键 numa_id 优先，
  topology.cpp:1755-1759）；`--max-cores-per-slice` 下同节点多 slice 按
  节点合计口径收缩（各 slice 输入相同 → N 一致）。
- 时间预算 240s→**200s**【v2/M1】：300s 硬宽限需容纳 init 探针 + fill +
  AR 方差（SVD 实测带 18-25×，25 时迭代+init ≈296s 刀锋）。
- `c` 来源优先级：VmHWM 反馈（本 slice 槽有效且命中同 test）→
  RSS_RATIO_TABLE 静态初值 → 调用方传统三元组（未迁移用例）。
- RSS_RATIO_TABLE：算法族 → 实测 c（KB/n²/线程）。【v2/q1 口径注明】
  值来自 Phase A 的 **(child@T4 − child@T1)/3 差分增量口径**
  （BDCSVD-f32 0.067 / f64 0.135 / c128 0.205 / Jacobi 0.031-0.124 /
  GEMM-f64 0.029 / f32 0.016 / c128 0.063 / sparse 0.009 / bidiag 0.031），
  与 svd_cdouble_sve.cpp:92-95 注释的"单进程 9.7-10× 输入体积"（~155B）
  口径不同（后者含基线）——模型用途取差分口径，差异 35% 属口径差非矛盾。
  c 是算法内在属性（矩阵驻留数×元素大小），跨机可移植；Eigen 版本升级
  需重标定（表注释注明）。
- knob 三层 fallback（knob > env > auto）语义保留；显式路径跳过联合求解
  但打预期内存警告（P2b）。
- **quick 模式【v2/m3】**：`sdc_quick_run()` → 直接返回 lo（与
  sdc_auto_square_dim:300-305 同捷径），防 CI smoke 单迭代 200s。
- 命名双语义：`_slice` 后缀 = per-node 语义；现有 `sdc_auto_square_dim`
  全局语义不动、标 deprecated 注释。

### 4.4 VmHWM 反馈闭环（【v2/C2】定位收窄）
**触发前提的诚实声明**：默认时序下 `desired_duration = 0.95×单迭代`
（svd_cdouble_sve.cpp:178）⇒ fracture 循环在 1 个 child 后退出
（sandstone_run.cpp:1831-1833 `runtime >= duration`）⇒ **SVD 类默认
不发生多 fracture，闭环无读者**；memfd 每次运行新建（sandstone.cpp:342）
⇒ 无跨运行记忆。闭环实际服务：(a) **内存受限多 fracture**（n_mem <
n_run200，即 GEMM/dynamic_square 类——它们迭代快、fracture 多）；
(b) 失败重试间 mdim 漂移修正；(c) 用户显式长 `-t`。SVD 类的 c 修正
依赖静态表准确性（局限记录于 §9）。

- **存放**：`PerThreadData::Main` 追加字段（每 slice 一个实例，位于
  thread_data_offset 之后的可写区；shmem header 被 `protect_shmem`
  mprotect 只读——LSP 核实修正 #1）：
  ```c
  struct MemFeedback { uint64_t test_id_hash; double eff_ratio;
                       uint32_t samples; };   // POD, 保持 trivially_copyable
  ```
- **生命周期【v2/M5，防闭环静默死亡】**：`init_per_thread_data()`
  （sandstone_run.cpp:1204）在 test_init **之前**运行且父进程 skip 路径
  （1608/1623）清全部槽——**MemFeedback 不进 `Main::init()`**，仅在
  parent 的 `init_shmem` 首次清零一次，语义为"跨 test/fracture 持久"。
  `static_assert(sizeof(PerThreadData::Main) == 64)`（sandstone.cpp:322）
  同 commit 更新为 128。
- **读取**：新增 `sdc_read_vm_hwm()`（`memfpt_current_high_water_mark`
  在 release 返回 0 且转正会激活 x86 诊断路径，不动——修正 #2）。
  基线污染：fork child VmHWM 起点 = 父进程当时 RSS（实测 8.3MB），eigen
  家族 <1% 可忽略；MAP_SHARED shmem 页各触碰者全额计账（实测子进程
  RssShmem 仅 24KB）——两项均记录为已知口径。【v2 审查实测】
- **写入点**：`run_one_test_inner` 尾部（test_cleanup 之后、返回之前，
  sandstone_run.cpp:1283-1288）。**fork 模式门控【v2/M6】**：
  `no_fork`（`-f none`）下测试在主进程跑、VmHWM 跨测试累计 → 虚高，
  该模式不写入。
- **算法**：`eff_ratio = VmHWM/(n²·elem·slice_threads)`；EMA α=0.5 写入
  本 slice 槽；±15% 死区。多 slice 各写各槽无竞态；读侧聚合取
  **median**【v2/q4：max 取最噪样本，median + 死区已防抖】。
- **跨 fracture**：MAP_SHARED memfd，上代 child 的写对下代可见；exec
  模式 shmemfd 经 argv 传递同样可见。
- **选择偏差【v2/m1，局限】**：超时/被杀 child 到不了写入点 → 反馈只收
  成功样本 → c 系统性偏乐观（mdim 偏大方向不自愈），靠死区 + 静态表
  下限（c 不低于表的 70%）缓解，如实记录。
- **兄弟槽访问【v2/M5】**：`main_thread_data(i>0)` 在子进程有 assert
  （sandstone_p.h:457）→ 新增内部裸指针访问器。

### 4.5 P0 落点（三个用例）
- **bidiag**（P0-2）：init 的 `sdc_auto_square_dim(...)` 调用替换为
  `sdc_joint_dim_slice(..., k_floor=3.0, solve_once=bidiag SVE 求解)`
  【v2/m7：solve_once 必须用与 run 期同构的 SVE 内核路径（非标量参考，
  两者差 4-8×）】。
- **svd_cdouble_sve**（P0-1）：mdim 计算从 preinit 迁到 child `test_init`
  （`sve_probe_and_init` 内，tier 选择后）；preinit 只留 desired_duration
  ——【v2/q2】用最保守 AR（内存密集族单节点 25）+ 静态表 c 推 **n 的
  上界**再推 duration，不变式：child 精算 n ≤ preinit n 上界。child 里
  `sdc_joint_dim_slice` per-node 精算。天然修复 exec_each_test 模式
  preinit 不跑导致 g_dim 恒 300 的分歧（LSP 核实修正 #3）；
  **exec 模式下 desired_duration 的说明【v2 修正】：`test_duration()`
  在父进程用 preinit 写入的值（fork 模式有效）；exec 模式子进程无
  preinit 但父进程有 → desired_duration 经 fork 前的 test 结构不随 exec
  传递，exec child 走 `-t`/默认时长（v1 表述"走 -t 兜底"结论不变，
  机理更正为：preinit 在 exec 父进程也运行，仅子进程映像不继承写值）**。
- **gemm_double_max_arm**：同样迁移 + 接入时间校准（【v2/M8】它同样
  无校准，v1 漏列）。

### 4.6 P2a/P2b
- **P2a bad_alloc 降级【v2/m2 重设计，对齐先例 + 消除竞态】**：
  - **init 线程**（单线程，无竞态）：分配失败 → `log_warning` 降
    mdim=lo 重分配一次 → 仍失败 `log_skip(资源性)` 返回 EXIT_SKIP。
  - **worker 期**：`sandstone_eigen_common.h`/GEMM 模板 run() 包
    try/catch → `log_skip(RuntimeSkipCategory, "memory …")` 线程级
    EXIT_SKIP——**对齐 eigen_sparse 先例（bad_alloc→SKIP）**，语义：
    资源性失败不是 SDC，不该打红。v1 的"降 mdim 重试 + FAIL"撤销
    （worker 改共享 d->dim 有竞态，FAIL 语义与先例相反）。
- **P2b -O 警告**：`sdc_knob_int_clamped` 显式命中分支（或联合求解外层）
  计算预期合计内存并打 `[欠压力]/[内存越界]`（沿用现有日志格式）。

### 4.7 P3
- 删 svd_cdouble_sve.cpp:109-145 与 gemm_double_max_arm.cpp choose_dim 手写
  sizing（迁移后自然消失）。
- **dynamic_square 家族**（gemm_*_dynamic_square*.cpp）迁移时清理：
  init 共享矩阵保留但计入 shared 项、worker 侧 `gemm_mdim` 重复计算
  改读 init 缓存。【v2/M8 指称修正】
- 卫生：tests/cpu/meson.build:449-465 注释更新（tests_sve per-tier 现实）。

## 5. x86-64 非回归（rule 5）

新逻辑全部 `#if defined(__aarch64__) && defined(__linux__)` 门控：
sdc_numa.{h,cpp}、sdc_read_vm_hwm、联合求解的 arm64 分支、反馈写入/读取
调用点。`PerThreadData::Main` 新字段本身无 arch 依赖且在 x86 也编译
（避免布局分叉），但 x86 无写入者 → 恒为零值 → 无行为影响（注释说明）。
x86 走原 sdc_auto_square_dim 路径不变。CI x86 构建 + 代码检视验证。

## 6. 验证矩阵（rule 2，【v2/M7/C2/q3】修订）

| 验证项 | 方法 | 通过标准 |
|---|---|---|
| P0 修复 | 本机 `-e eigen_svd_cdouble_sve -t 5000`、`-e eigen_svd_bidiag_sve -t 5000`、`-e eigen_gemm_double_max_arm -t 5000` | `exit: pass`；其余 16 个默认集用例非回归 pass |
| P1 提升（分族） | 21 用例全核 auto | GEMM/dynamic_square 峰值 ≥ 原 2×；SVD/sve/bidiag 峰值 ≥ 原 80% 且 pass；全部 ≤ 预算上界（无 OOM） |
| VmHWM 闭环 | **GEMM 类长 -t 300000**（迭代快、fracture≥3，验证写+读收敛）+ **小 -O mdim 单元验证写侧**（SVD 类默认时序不触发，如实声明） | 第 2 fracture 起 mdim 方向正确，eff_ratio 收敛 ±15% |
| per-node 退化 | 本机 node0-only | 行为与全局模型对照一致（数值 diff） |
| 显式 -O | 16/32/48GiB 档抽测 4 点 | pass + 警告出现 |
| quick 模式 | `--quick -e eigen_svd_double` 等 | mdim=lo，单迭代秒级 |
| no_fork | `-f no -e eigen_gemm_double_dynamic_square -t 5000`（PR#302 检视 #8 勘误: `-f none` 是无效值→EX_USAGE；执行用 `-f no`，日志确认 no_fork 模式） | pass，且无反馈写入（VmHWM 不污染） |
| 回归 | `-e zstd19 -t 2000 -n 1` | `exit: pass` |
| SVE 512/1024 档 | gem5-fi（CHAOS/SE，same-source）。**【v2/q3 收窄】SE 无 sysfs NUMA/meminfo → 实际验证面 = fallback 档（探测失败→最差 AR）+ 联合求解正确性**，NUMA 感知逻辑本身不在 gem5 覆盖内 | 各 tier checksum + `SDCSHIELD_FAILS=0` |
| x86 非回归 | CI x86 构建 + 检视 | 零新警告，行为不变 |
| 真多节点 per-node | 诚实边界 | 本机不可验；gem5-fi FS 多节点若可配置则补，否则记录 |

## 7. commit 切分（rule 1，依赖序）

1. `sdc_numa_probe()` + 二维 AR 表（新文件，独立可测）
2. `sdc_budget_bytes()`：cgroup v1/v2 + 节点合成 + **退化规则**
3. `sdc_joint_dim_slice()` + 三点拟合/探针阈值 + RSS_RATIO_TABLE（含口径注释）
4. bidiag 接入（P0-2）
5. svd_cdouble_sve 迁移 + 模型接入（P0-1 + per-node + exec 分歧修复）
6. gemm_double_max_arm 迁移 + 时间校准接入
7. VmHWM 反馈闭环（sdc_read_vm_hwm + Main 字段 + static_assert 更新 +
   裸指针访问器 + no_fork 门控 + median 聚合 + 生命周期语义）
8. dynamic_square 家族迁移（init 矩阵计 shared、gemm_mdim 改缓存）
9. bad_alloc 降级（init 降 mdim + worker SKIP，对齐 sparse 先例）
10. -O 警告
11. meson 注释卫生
12. 文档总 sync（CLAUDE.md/README/docs，rule 7）

每个 commit 前置：build 零新警告 + 本机受影响用例实测 + zstd19 回归。
commit 4-6 阶段 VmHWM 反馈未实现 → c 走静态表（优先级链退化成立，
【v2/m8】P0 稳健性此时完全押注静态表 + AR 假设——正是 §6 前两行先行
验证的原因）。

## 8. 风险与缓解

- MemAvailable 波动（他进程）→ 每次 fracture 重读 + 0.9 系数（PR#302 检视 #21 口径澄清: 重读的是**全局** MemAvailable/cgroup——sdc_budget_bytes 每次调用现读；节点分摊比例 MemFree/ΣMemFree 是探测期快照(magic static)，聚合预算仍逐次新鲜、仅多节点机上的节点间分摊为慢漂移近似——单内存节点精确还原全局，多节点漂移已记录为边界（fallback 节点选择也依赖快照，中途漂移会破坏 C1 归属一致性，故不逐次重探））。
- eff_ratio 震荡 → EMA α=0.5 + ±15% 死区 + c 下限（静态表 70%）。
- 反馈槽读到陈旧值（同 test 重跑不同配置）→ test_id_hash + samples 计数。
- AR 表未标定档（2/4/≥8 节点）→ 宁高估（保 P0 牺牲 P1），TODO(实测)。
- 固定开销污染 → 探针阈值 100ms / 三点拟合（§4.3），M3 数学已验证。
- Main 布局变化 → static_assert 同 commit 更新；全内自洽。
- SVE 下放后 init 时序 → tier 仍由 spec_get 决定；VL 一致性由框架
  worker pin 既有机制保证。
- 时间预算刀锋 → 200s 预算 + AR 上界 + 验证矩阵先行实测。

## 9. 已知边界（诚实声明）

- AR 表的 2/4/≥8 节点档为保守估计，未实测（无真多节点内存机器）。
- per-node 预算的真多节点行为本机不可验证（node0-only 退化路径可验证）。
- **反馈闭环对 SVD 类默认时序不生效**（单 fracture），其 c 修正依赖静态
  表；反馈只收成功样本（超时样本丢失）→ c 偏乐观方向。
- exec_each_test 模式下子进程不继承 preinit 写入的 desired_duration，
  走 `-t`/默认时长。
- SVE 512/1024 档验证依赖 gem5-fi SE，**其实际验证面为 fallback 档**
  （SE 无 sysfs NUMA/meminfo），NUMA 感知逻辑不在其覆盖内。
- no_fork 模式不参与反馈闭环。
