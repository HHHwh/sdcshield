# Eigen 用例 P0-P3 修复设计（NUMA 感知 sizing + 内存最大化 + OOM 防护）

日期：2026-10-27
状态：设计定稿（brainstorming 完成，待实现）
分支：feat/eigen-p0-p3-fix
实测基线：本机 sdc1-01-04（128 核 / 61GB / 4 NUMA node 但**内存全在 node0** /
SVE-256）119 个测量点（Phase A 94 + B 21 + C 4 + R 17，另含多用例连跑验证）。

## 1. 问题清单（全部实证）

### P0 真实场景稳定失败（产品 bug，两次独立复现）
- **P0-1 `eigen_svd_cdouble_sve`**：全核 auto `-t 5000` → 321s timeout、42.6GB
  （物理 66.7%）。根因：preinit 时间模型 `t(N)=243.4·(N/4400)³+1`（单线程
  标定，svd_cdouble_sve.cpp:177）不含全核争抢；本机 42.6GB 全挤 node0 →
  带宽争抢实测 ~20×（单解 8s → >160s）→ 300s 硬宽限被击穿。
- **P0-2 `eigen_svd_bidiag_sve`**：同场景 343s timeout、16.9GB。根因：**无
  任何时间校准**（唯一未接 `sdc_calibrated_dim` 的大计算量用例），auto mdim
  直撞 2048 knob 上限。

### P1 内存预算失效（"最大化"没有发生）
- 模型虚报 5-61×：GEMM 4.7-5.7×、SVD 5.9-7.9×、sparse 23×、Jacobi 34-61×。
  真实场景 eigen 家族只吃物理内存 **5-18%**（最高 11.1GB/61GB）。
- 预算只读 `_SC_PHYS_PAGES` 总量：不看 MemAvailable、不看 cgroup
  memory.max（有 "job oom" 实测前科，svd_cdouble_sve.cpp:114-120 注释）。
- 无 per-NUMA-node 预算（22 处 sizing 无一感知节点）。
- GEMM init 共享矩阵（3·S·n² × 4 slice）与 128 worker 同刻分配峰值不在模型。

### P2 OOM 防护缺失
- SVD/GEMM 模板 `test_run` 无 `bad_alloc` catch（仅 eigen_sparse 有）→ 分配
  失败走 malloc SIGKILL/abort，用户看到被杀而非诊断。
- 显式 `-O` 旁路全部防护（无校准、无预期内存警告），仅剩 knob clamp。

### P3 一致性
- 三套并行 sizing（helper / sve 手写 :109-145 / gemm 手写），系数与上限
  各异（0.65 vs 650‰ vs 850‰；ratio 12 vs 6 vs 2.0）。
- 时间上限与内存上限从不联合求解。
- 卫生：check-arm64-requires.py 不扫 tests/cpu/sleef（sleef_sve 漏 tag）；
  CLAUDE.md/meson 注释仍称 tests_sve 固定 128-bit（与 per-tier 现实矛盾）；
  docs/coding/writing_tests.md 残留 merge conflict 标记。

## 2. 目标 / 非目标

**目标**：
1. P0 两用例在全核 auto 下 `exit: pass`（不再 timeout）。
2. 内存消耗 ≥ 现状 2×（GEMM 9.3→≥20GB 等）且无 OOM（≤ MemAvailable×0.9）。
3. 显式 `-O` 超预算时有警告；分配失败有降级而非裸 SIGKILL。
4. sizing 统一到单一 helper，时间/内存联合求解。
5. x86-64 行为 byte-identical（rule 5）。

**非目标**：
- Eigen 上游 complex `ptranspose` 1024 档缺陷修复（单独规划）。
- Jacobi 族的 SVD 算法替换。
- SVE 512/1024 档的本机实测（无硬件；gem5-fi 验证）。

## 3. 总体架构

```
[启动/选择期]                      [child test_init]               [worker/child 退出]
sdc_numa_probe() ──┐
  节点/内存/CPU映射  ├─→ sdc_joint_dim_slice() ─→ mdim ─→ 分配 ─→ 计算
  numa_class 查表    │   = min(n_mem, n_run300, clamp)          │
                    │   n_mem = sqrt((node_budget−sh)/(T·c))    ↓
sdc_budget_bytes() ─┘   n_run300 = (240s/(AR·2·k))^(1/p)    VmHWM(sdc_read_vm_hwm)
                                                            写 PerThreadData::Main
RSS_RATIO_TABLE ──→ c 初值（族→实测系数）                      （每 slice 一槽）
                    ↑                                            │
                    └──── 下个 fracture 的 init 读 ◄── EMA 修正 ◄┘
```

四个子系统：NUMA 探测与分档系数、预算升级（含 per-node）、联合求解 helper、
VmHWM 反馈闭环。P2/P3 是模板层与卫生收尾。

## 4. 子系统设计

### 4.1 `framework/sdc_numa.{h,cpp}`（新，arm64 门控）
`sdc_numa_probe()`：静态缓存一次。读 `/sys/devices/system/node/nodeN/meminfo`
（MemTotal/MemAvailable）与 `cpulist` → `struct sdc_numa_topology { int
nodes_with_mem; int threads_per_mem_node; double numa_class; }`。

分档表（常量数组 + 注释记录实测锚点）：

| 有内存节点数 | 线程/节点 | numa_class | 来源 |
|---|---|---|---|
| 1 | 全部 | 20.0 | 本机实测（SVD 18-25×，2026-10-27 Phase C） |
| 2 | ~64 | 8.0 | 待标定（保守初值） |
| 4 | ~32 | 4.0 | 待标定（保守初值） |
| ≥8 | — | 3.0 | 保守 |
| 探测失败 | — | 20.0 | 最差档兜底 |

同源锚点：`sdc_calibrated_dim` 注释记录的 sparse 24.9×（单节点内存假设下
同量级）。探测失败/无 sysfs → 最差档，永不向上猜。

### 4.2 `sdc_budget_bytes(permille)`（sdc_stress_config.h）
`min(物理×‰/1000, MemAvailable×0.9, cgroup memory.max−memory.current)`；
任一来源读失败即跳过该来源；cgroup v2 路径 `/sys/fs/cgroup/memory.max`，
值为 "max" 视为无限制。per-node 版 `sdc_node_budget_bytes(permille, node)`：
读该 slice 归属节点的 nodeinfo MemAvailable（节点归属由 device_range 的
CPU → numa_node 映射得出，topology 已有）。

### 4.3 `sdc_joint_dim_slice()`（联合求解核心，新）
扩展 `sdc_calibrated_dim` 使其向外返回拟合参数 (k, p)（出参结构体，默认
nullptr 保持现签名兼容）。联合层：

```c
n_opt = min(
    n_mem   = sqrt((node_budget − node_slices·shared) / (node_threads · c)),
    n_run300 = (240s / (numa_class · 2 · k_ms))^(1/p),   // 300s×0.8
    knob_hi)
```

`node_threads` = **该 NUMA 节点上的全部 worker 数**（不是本 slice 线程数）：
Heuristic 默认按 NUMA 边界切 slice（slicing.cpp core group），此时
slice≡节点、node_threads=slice_threads；但 `--max-cores-per-slice` 可把
一个节点切成多个 slice，若按本 slice 线程数分预算，同节点各 slice 合计
会超节点预算（每 slice 各自 N 也必须按节点总线程数收缩，否则
节点内合计 = node_slices × node_budget）。同节点内各 slice 算出的 N
相同（输入相同），保持 Eigen 对象跨 slice 同构不是约束（各 child 的
test->data 本就独立），但内存安全要求按节点合计口径。

- `c` 来源优先级：VmHWM 反馈（本 slice 槽有效且命中同 test）→ RSS_RATIO_TABLE
  静态初值 → 调用方传统 (bytes×matrices×rss_ratio) 三元组（未迁移用例）。
- RSS_RATIO_TABLE：算法族 key → 实测 c（KB/n²/线程），Phase A 值：
  BDCSVD-f32 0.067 / BDCSVD-f64 0.135 / BDCSVD-c128 0.205 / Jacobi
  0.031-0.124 / GEMM-f64 0.029 / GEMM-f32 0.016 / GEMM-c128 0.063 /
  sparse 0.009 / bidiag 0.031。c 是算法内在属性（矩阵驻留数×元素大小），
  跨机可移植；Eigen 版本升级需重标定（表注释注明）。
- knob 三层 fallback（knob > env > auto）语义保留；显式路径跳过联合求解
  但打预期内存警告（P2b）。
- 命名双语义：`_slice` 后缀 = per-node/slice 语义（slice 线程数、节点预算）；
  现有 `sdc_auto_square_dim` 全局语义不动、标 deprecated 注释。

### 4.4 VmHWM 反馈闭环
- **存放**：`PerThreadData::Main` 追加字段（每 slice 一个实例，位于
  thread_data_offset 之后的**可写区**；shmem header 被 `protect_shmem`
  mprotect 只读，不可放——LSP 核实修正 #1）：
  ```c
  struct MemFeedback { uint64_t test_id_hash; double eff_ratio;
                       uint32_t samples; };   // POD, 保持 trivially_copyable
  ```
- **读取**：新增 `sdc_read_vm_hwm()`（独立实现；`memfpt_current_high_water_mark`
  在 release 返回 0 且转正会激活 x86 诊断路径，不动——修正 #2）。
- **写入点**：`run_one_test_inner` 尾部（test_cleanup 之后、返回之前，
  sandstone_run.cpp:1283-1288），worker 已全部 join，VmHWM 即本 child 峰值。
- **算法**：`eff_ratio = VmHWM/(n²·elem·slice_threads)`；EMA α=0.5 写入本
  slice 槽；±15% 死区内不调整（防抖）。多 slice 各写各槽无竞态；读侧取
  max（保守）。
- **跨 fracture**：MAP_SHARED memfd，上代 child 的写对下代可见（结果回传
  同机制）。exec_each_test 模式 shmemfd 经 argv 传递，同样可见。
- **安全退化**：槽无效（test_id_hash 不匹配 / samples=0）→ 用静态表初值。

### 4.5 P0 落点（两个用例）
- **bidiag**（P0-2）：init 的 `sdc_auto_square_dim(...)` 调用替换为
  `sdc_joint_dim_slice(..., k_floor=3.0, solve_once=bidiag 求解)`（接入
  时间校准 + numa_class）。
- **svd_cdouble_sve**（P0-1）：mdim 计算从 preinit 迁到 child `test_init`
  （`sve_probe_and_init` 内，tier 选择后）；preinit 只留 desired_duration
  （用最保守 numa_class=20 档推导，保证框架时序）；child 里
  `sdc_joint_dim_slice` per-node 精算。天然修复 exec_each_test 模式
  preinit 不跑导致 g_dim 恒 300 的分歧（LSP 核实修正 #3）；exec 模式下
  desired_duration 走 -t/默认兜底（记录为已知边界）。
- **gemm_double_max_arm**：同样迁移（static dim + preinit 同款模式）。

### 4.6 P2a/P2b
- **P2a bad_alloc 降级**：SVD/GEMM/sparse 模板 run()（sandstone_eigen_common.h、
  gemm 各模板）包 try/catch：catch → `log_warning` 降 mdim=lo 重分配一次 →
  仍失败 `report_fail_msg`（附 mdim/预算/VmHWM 数字，诚实 FAIL）。
- **P2b -O 警告**：`sdc_knob_int_clamped` 显式命中分支（或联合求解外层）
  计算预期合计内存并打 `[欠压力]`/`[内存越界]`（沿用现有日志格式）。

### 4.7 P3
- 删 svd_cdouble_sve.cpp:109-145 与 gemm_double_max_arm.cpp choose_dim 手写
  sizing（迁移后自然消失）；删 GEMM worker 侧 `gemm_mdim` 重复计算路径。
- 卫生三件套：check-arm64-requires.py 补扫 tests/cpu/sleef；CLAUDE.md +
  tests/cpu/meson.build 注释更新（tests_sve per-tier 现实）；writing_tests.md
  merge 标记清理。

## 5. x86-64 非回归（rule 5）

新逻辑全部 `#if defined(__aarch64__) && defined(__linux__)` 门控：
sdc_numa.{h,cpp}、sdc_read_vm_hwm、PerThreadData::Main 新字段（字段本身
无 arch 依赖，但写入/读取调用点门控；字段在 x86 编译也存在以避免布局
分叉——注释说明）、联合求解的 arm64 分支。x86 走原 sdc_auto_square_dim
路径不变。CI x86 构建 + 代码检视验证。

## 6. 验证矩阵（rule 2）

| 验证项 | 方法 | 通过标准 |
|---|---|---|
| P0 修复 | 本机 `-e eigen_svd_cdouble_sve -t 5000`、`-e eigen_svd_bidiag_sve -t 5000` | 两者 `exit: pass`；其余 17 个默认集用例非回归 pass |
| P1 提升 | 21 用例全核 auto（含 --quality=-1 jacobi） | 峰值 ≥ 原 2× 且 ≤ MemAvailable×0.9 |
| VmHWM 收敛 | `-t 30000`（fracture ≥2） | 第 2 child mdim 方向正确，eff_ratio 收敛 ±15% |
| per-node 退化 | 本机 node0-only | 行为与全局模型对照一致 |
| 显式 -O | 16/32/48GiB 档抽测 4 点 | pass + 警告出现 |
| 回归 | `-e zstd19 -t 2000 -n 1` | `exit: pass` |
| SVE 512/1024 档 | gem5-fi（CHAOS/SE，same-source） | 各 tier checksum + `SDCSHIELD_FAILS=0` |
| x86 非回归 | CI x86 构建 + 检视 | 零新警告，行为不变 |
| 真多节点 per-node | 诚实边界 | 本机不可验；gem5-fi FS 多节点可行则补，否则记录 |

## 7. commit 切分（rule 1，依赖序）

1. `sdc_numa_probe()` + 分档表（新文件，独立可测）
2. `sdc_budget_bytes()` + per-node 版
3. `sdc_joint_dim_slice()` + RSS_RATIO_TABLE + calibrated_dim 出参扩展
4. bidiag 接入（P0-2）
5. svd_cdouble_sve 迁移 + 模型接入（P0-1 + per-node + exec 分歧修复）
6. gemm_double_max_arm 同迁移
7. VmHWM 反馈闭环（sdc_read_vm_hwm + Main 字段 + EMA + 写入点）
8. 其余 19 用例迁移（含 GEMM 手写删除）
9. bad_alloc 降级（P2a）
10. -O 警告（P2b）
11. 卫生三件套（拆 2-3 个）
12. 文档总 sync（CLAUDE.md/README/docs，rule 7）

每个 commit 前置：build 零新警告 + 本机受影响用例实测 + zstd19 回归。

## 8. 风险与缓解

- MemAvailable 波动（他进程）→ 每次 fracture 重读 + 0.9 系数。
- eff_ratio 震荡 → EMA α=0.5 + ±15% 死区。
- 反馈槽读到陈旧值（同 test 重跑不同配置）→ test_id_hash + samples 计数。
- bal 档系数未标定 → 保守初值 + TODO(实测) 注释。
- Main 结构布局变化 → 全内自洽（同二进制内 parent/child 一致）；trivially
  copyable static_assert 保持。
- SVE 下放后 init 时序 → tier 仍由 spec_get（选择期已缓存）决定；VL 一致性
  由框架 worker pin 既有机制保证。

## 9. 已知边界（诚实声明）

- numa_class 的 2/4/≥8 节点档为保守估计，未实测（无真多节点内存机器）。
- per-node 预算的真多节点行为本机不可验证（node0-only 退化路径可验证）。
- exec_each_test 模式下 desired_duration 仍走默认/-t（preinit 时序限制）。
- SVE 512/1024 档验证依赖 gem5-fi，其模拟的 NUMA/内存模型与真机有差异。
