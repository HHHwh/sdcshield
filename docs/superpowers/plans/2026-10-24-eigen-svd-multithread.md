# eigen_svd 家族多线程拉满——实施计划（P0/P1/P2）

日期：2026-10-24
分支：feat/eigen-svd-multithread
前置：设计讨论（2026-10-23 会话）+ 本机满核实证（3×240s 全绿）+ gem5-fi FS
模式双线程实证（`gem5-fi` 仓 `feat/fs-sdcshield-multithread` 分支，
`eigen_svd_double -n 2 → pass`）。

## 0. 背景与结论（实证）

- **结构层已多线程**：eigen_svd 家族 10 用例（BDCSVD×6 + JacobiSVD×4）统一走
  `EigenSVDTest` 模板 = per-thread 输入 + per-thread 同源 golden（H11'/P14，
  46141ed4 模式）。本机 64 核满跑 3×240s×3 seed 全绿。
- **CLAUDE.md 的 "eigen_svd_double 满线程 ULP 假阳 → use -n 1" 是 H11' 改造前
  的过时记录**（旧结构 init 共享 golden 跨线程比对）。
- **真残留**：
  1. `eigen_sparse` 仍是 init 共享 golden（CLAUDE.md 点名两个用例中唯一旧模式）；
  2. 自适应维度上限 2048 时单次 BDCSVD 数分钟，`-t` 无法打断迭代内计算
     （实测 2 分钟 timeout 杀不掉）；
  3. 文档口径未解除 `-n 1`。

## 1. Patch 拆分（one patch per unit）

### P1: eigen_sparse per-thread 化（`tests/cpu/eigen_sparse/eigen_sparse.cpp`）

对齐 `EigenSVDTest` H11' 模式：
- 删除 init 的 A/b/golden 共享生成与单点 golden 求解；
- run 内每线程独立：生成稀疏矩阵（保持 10% 密度 + 对角占优 + `[-1,1)` 值域，
  良态可解）与 b（框架 RNG per-thread 流），**同线程先解出 golden 再重解比对**
  （same-source repeat）；
- `kRerollEvery = 16` 次重掷（解除当前"故意不 reroll"的冻结——冻结原因是旧
  共享 golden 结构下的 flakiness，per-thread 化后该理由消除）；
- init 保留维度计算（`sparse_n`）与数据骨架。
- 共享文件逻辑修复（同 H11' 先例），x86 同向受益，无架构分支。

验证：
- `./scripts/build.sh` 零新警告；
- 满核 64×≥300s×2 seed 全绿（reroll 生效，矩阵随时间变化）；
- `-n 1` 对照 pass；
- `-e zstd19 -t 2000 -n 1` 回归 pass；
- x86 非回归：检视（无 `#ifdef` 分支变化）。

### P2: EigenSVDTest 维度时间校准（`tests/cpu/eigen_svd/sandstone_eigen_common.h`）

先例：`svd_cdouble_sve` init 按"10 分钟预算内完成整个测试"校准 N。
本 patch 把校准推广到模板：
- 模板 init（或首次 run 前一次性）以基准维度（128）试解一次 `calculate_once`
  计时，按 O(n³) 外推，取 `dim = min(用户 dim, 使单次迭代 ≤ 剩余时间预算 1/4 的上限)`；
- 显式 `-O mdim=N`（knob/env 显式指定）时跳过校准（用户自担）；
- 仅自适应路径（`sdc_auto_square_dim`）受校准约束。
- 共享模板改动，x86 同向受益。

验证：
- build 零新警告；
- `-O mdim=2048` 显式档不受校准影响（行为不变——校准只作用于自适应路径，
  用 `-t` 短时 + 自适应档观测实际维度 ≤ 校准上限，日志引用真实输出）；
- 满核回归 + zstd19 回归。

### P0: 文档更正（`CLAUDE.md`，如需 README）

- Known platform quirks 条目更正：`eigen_svd_double` 满核假阳已由 H11'
  per-thread golden 消除（本计划长跑 + 既有 3×240s 实证）；`-n 1` 建议仅保留给
  `eigen_sparse`（P1 合入后该条亦可解除——P1 完成后一并更新为"已消除"）。

验证：文档条目与实测输出一致（引用真实 YAML/terminal 输出）。

## 2. 执行顺序（全部完成，2026-10-24）

1. ✅ 满核长跑链：eigen_svd_double 10min mdim=128 `pass` / mdim=256
   LCG:777 `pass`；eigen_sparse 段实测自适应大维度 timed out（SIGQUIT），
   该现象归入 P2 问题域（非 ULP 假阳）；
2. ✅ P1 代码 + 验证 + commit（98188ec9）；
3. ✅ P2 代码 + 验证 + commit（05819d8e）——实施中两项实测驱动的加固：
   probe 档自适应抬升（<1ms 翻倍，规则带状 probe 会把稀疏成本低估 3 个
   数量级）、k_floor（小档实测 k 偏低，BDCSVD 64→128 实测 1.8 为预处理
   主导，取 max(实测, 理论) 修正）；
4. ✅ P0 文档更正（CLAUDE.md 平台怪癖条目 + README 结果判读）；
5. ✅ push feature 分支。

## 3. 风险与诚实边界

- P1 reroll 引入稀疏结构随机性：对角占优保证可解性（SimplicialCholesky
  正定），`solver.info() != Success` 时按现状 report_fail；
- P2 校准的外推是启发式（O(n³) 拟合单点）——上限宁小勿大（拉满优先于单轮
  极限维度）；显式 knob 优先，绝不覆盖用户显式指定；
- gem5-fi FS 侧 SVE 变体（svd_cdouble_sve）本轮不动——其校准逻辑已有先例
  且本机无 SVE，改动须 gem5-fi 四档验证（超出本计划范围）。
