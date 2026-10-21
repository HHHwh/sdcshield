# 战役引擎参数最大化强化 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 2026-10-16 实测标定的内存最大化参数注入 sdc-excite-reproduce.sh 战役引擎的 eigen 单线程段与 L4 驻留段，使 7×24 战役的向量/浮点用例压力按"当前内存容量最大化"运行。

**Architecture:** 引擎是 bash 状态机（cycle 内 cold→l2→l3→l5→l4 顺序阶段）。本 patch 只改两个阶段的参数装配，不改状态机结构：(1) `phase_l3` 的 eigen 单线程段（`-n 1`，单线程形态，吃大 mdim）注入 3 个 `-O` 覆盖并把调度量子从 60s 提到 260s 以容纳 ~100s/迭代的大矩阵 SVD；(2) `phase_l4` 驻留循环对 `openblas_dgemm` 满核驻留注入 mdim=1536（满核安全上限）。T_EIGEN 预算 300s→900s 让量子提升后仍能覆盖多个 eigen 用例。

**Tech Stack:** bash（引擎脚本）、sdcshield `-O test.knob=v` 运行时参数链（sdc_knob_int_clamped，越界自动 clamp 不报错）。

**Spec:** 本 plan 自带标定数据（2026-10-16 本机实测，TaiShan 2280 / Kunpeng 920 / 126 核在线 / 28G 可用）。上游目标：用户 goal"根据当前内存容量最大化所有用例的参数（尤其向量、浮点如 eigen）"。

## Global Constraints

- 满核（126 线程）形态安全上限（实测 MemAvailable 低点，2026-10-16）：`eigen_svd_double.mdim=1024`（1536=OOM 26G 打满）、`eigen_gemm_double_dynamic_square.mdim=2048`（17G 峰值）、`openblas_dgemm.mdim=1536`（2048 已 23G，与伴层叠加超界）。
- 单线程（-n 1 / --cpuset）形态实测：`eigen_svd_double.mdim=2048` → RSS 558MB、迭代 99.7s；`eigen_gemm_double_dynamic_square.mdim=4096` → 1.03GiB、64s；`openblas_dgemm.mdim=4096` → 1.17GiB、65s（4096 是 eigen_gemm clamp 上限 [16,4096]，svd clamp 上限 [64,2048]）。
- 调度量子必须 ≥ 2×单次迭代时长，否则窗口内完不成比对（无压力意义）：svd@2048 需量子 ≥ 200s，取 260s。
- `-O` 对 clamp 范围外的值静默截断到界内（sdc_knob_int_clamped 语义），对不在 `-e` 列表的测试无副作用。
- x86-64 不受影响（引擎脚本是 ARM64 部署侧运维脚本，不触碰 x86 测试代码）。
- 引擎脚本修改后必须 `bash -n` 语法校验；行为验证不能跑全量 smoke（会与在跑的 scan 抢 CPU），用等价单命令实测。

## Review Focus

- `-O` 旋钮名拼错 → sdc_knob 静默用默认值，参数注入无声失效。测试步骤必须断言实测迭代时长/内存与标定值同量级（mdim 生效的证据），而非仅看 pass。
- 量子 260s 与 T_EIGEN 预算的相对关系：预算必须 ≥ 单量子，否则 run_bounded 的 `-T` 先到，首个测试被中途掐断。900s ≥ 260s ✓。
- L4 驻留段 `-t each`（each=remain/n，可达数小时）与 `--max-test-loop-count=0` 的固定模式：注入 mdim 后单迭代时长变长（oblas@1536 满核 ~10s 级），不影响固定模式语义。
- DWELL_OVERRIDE 事件驱动切换（事件测试优先驻留）：条件注入必须只匹配 `openblas_dgemm` 精确名，不能把 oblas 的 -O 带给事件测试。
- smoke 模式回归：T_EIGEN 在 smoke 档是 30s，量子若全局改 260s 会让 smoke 的 eigen 段单测试跑 260s（-t 260s）超预算——需确认 smoke 不跑出 hang 判定（run_bounded 的 timeout 兜底 = 预算+量子+120s = 410s，安全）。

---

### Task 1: l3_eigen_n1 段参数最大化注入 + T_EIGEN 延长

**Files:**
- Modify: `scripts/sdc-excite-reproduce/sdc-excite-reproduce.sh:23`（T_EIGEN=300s → 900s，full 档）
- Modify: `scripts/sdc-excite-reproduce/sdc-excite-reproduce.sh:315`（eigen_n1 段量子与 -O 注入）

**Interfaces:**
- Consumes: `run_bounded <label> <预算秒> <单测量子秒> <args...>`（本文件既有）、`resolve 'eigen*'`（既有）、`FLAGS` 数组（既有）。
- Produces: `l3_eigen_n1_c${CYCLE}` 波次命令含 `-O eigen_svd_double.mdim=2048 -O eigen_svd_double2.mdim=2048 -O eigen_gemm_double_dynamic_square.mdim=4096`；Task 2 的验证复用本任务的标定断言方法。

- [ ] **Step 1: 修改 T_EIGEN full 档**

`scripts/sdc-excite-reproduce/sdc-excite-reproduce.sh:23`，把：

```bash
    T_HEAT=30m;  T_TARGET=30m; T_EIGEN=300s
```

改为：

```bash
    T_HEAT=30m;  T_TARGET=30m; T_EIGEN=900s
```

（只动 full 档行 23；smoke 档行 18 的 `T_EIGEN=30s` 不动。）

- [ ] **Step 2: 修改 l3_eigen_n1 段**

同文件 `phase_l3()` 内，把：

```bash
    [ -n "$eig" ] && run_bounded "l3_eigen_n1_c${CYCLE}" "$(DUR_S "$T_EIGEN")" 60 -e "$eig" -n 1 "${FLAGS[@]}"
```

改为：

```bash
    # eigen 单线程大参数档（2026-10-16 标定: svd@2048=558MB/99.7s迭代, gemm@4096=1.03G/64s;
    # 量子 260s 保 ≥2 次完整迭代——60s 量子在 100s 迭代下零比对, 大参数形同虚设）
    [ -n "$eig" ] && run_bounded "l3_eigen_n1_c${CYCLE}" "$(DUR_S "$T_EIGEN")" 260 -e "$eig" -n 1 \
        -O eigen_svd_double.mdim=2048 -O eigen_svd_double2.mdim=2048 \
        -O eigen_gemm_double_dynamic_square.mdim=4096 "${FLAGS[@]}"
```

- [ ] **Step 3: 语法校验**

Run: `bash -n scripts/sdc-excite-reproduce/sdc-excite-reproduce.sh`
Expected: 无输出，退出码 0。

- [ ] **Step 4: 行为实测（等价命令形态，绕开与在跑 scan 的 CPU 冲突——单线程绑定核 8）**

Run:

```bash
/usr/bin/time -v ./builddir/sdcshield --cpuset=8 -t 260s -Y --quality=0 \
  -e eigen_svd_double -O eigen_svd_double.mdim=2048 -o /tmp/eng_patch_svd.yaml \
  > /tmp/eng_patch_svd.log 2>&1
grep -E 'test-runtime|result' /tmp/eng_patch_svd.yaml | head -4
```

Expected: `test-runtime` ≈ 100000ms 量级（mdim=2048 生效证据——默认自适应约 511 维时单迭代 <15s）；`result: pass`。

- [ ] **Step 5: zstd19 回归（CLAUDE.md 标准）**

Run: `./builddir/sdcshield -e zstd19 -t 2000 -n 1`
Expected: `exit: pass`。

- [ ] **Step 6: 提交**

```bash
git add scripts/sdc-excite-reproduce/sdc-excite-reproduce.sh
git commit -s -m 'feat(excite): eigen 单线程段参数最大化——svd mdim 2048/gemm 4096 + 量子 260s

2026-10-16 标定(126核/28G): svd@2048 单线程 558MB/99.7s 迭代, gemm@4096 1.03G/64s。
60s 量子下 100s 迭代零比对, 量子提至 260s 保 ≥2 次完整迭代; T_EIGEN 300s→900s
覆盖提升后的单量子。满核形态安全上限(svd1024/gemm2048)不变, L3 满核段不注入。'
```

### Task 2: L4 驻留段 openblas_dgemm 满核参数化

**Files:**
- Modify: `scripts/sdc-excite-reproduce/sdc-excite-reproduce.sh`（`phase_l4()` 驻留循环）

**Interfaces:**
- Consumes: `phase_l4()` 既有结构（DWELL_DEFAULT 列表迭代 + `run_sdc` + `--max-test-loop-count=0` 固定模式）；Task 1 的标定断言方法。
- Produces: `l4_dwell_openblas_dgemm_c${CYCLE}` 波次满核 mdim=1536 驻留（实测 13G 量级峰值预估，来自 oblas@2048=23G 的平方比例外推）。

- [ ] **Step 1: 修改驻留循环**

`phase_l4()` 内，把：

```bash
    for t in "${arr[@]}"; do
        run_sdc "l4_dwell_${t}_c${CYCLE}" 0 -e "$t" -t "${each}s" --max-test-loop-count=0 "${FLAGS[@]}"
    done
```

改为：

```bash
    for t in "${arr[@]}"; do
        # 满核驻留大参数档（2026-10-16 标定: oblas mdim=2048 满核峰值 23G 与伴层叠加
        # 超界, 降档 1536 ≈13G; 条件注入——DWELL_OVERRIDE 事件测试不带 oblas 参数）
        local o=()
        [ "$t" = "openblas_dgemm" ] && o=(-O openblas_dgemm.mdim=1536)
        run_sdc "l4_dwell_${t}_c${CYCLE}" 0 -e "$t" -t "${each}s" --max-test-loop-count=0 "${o[@]}" "${FLAGS[@]}"
    done
```

（bash 函数内 `local` 在循环体内合法；若 lint 偏好，可把 `local o` 提到循环前——两可，保持最小 diff 用循环内声明。）

- [ ] **Step 2: 语法校验**

Run: `bash -n scripts/sdc-excite-reproduce/sdc-excite-reproduce.sh`
Expected: 无输出，退出码 0。

- [ ] **Step 3: 行为实测（满核 oblas@1536 短跑验证峰值与 pass——90s）**

Run:

```bash
( while :; do awk '/MemAvailable/{print $2; fflush()}' /proc/meminfo; sleep 0.3; done > /tmp/eng_patch_oblas_mem.txt ) & S=$!
./builddir/sdcshield -t 90s -Y --quality=0 -e openblas_dgemm -O openblas_dgemm.mdim=1536 -o /tmp/eng_patch_oblas.yaml > /tmp/eng_patch_oblas.log 2>&1
kill $S 2>/dev/null
sort -n /tmp/eng_patch_oblas_mem.txt | head -1
grep -cE 'result: pass' /tmp/eng_patch_oblas.yaml
```

Expected: MemAvailable 低点 > 8GiB（预估占用 ~13G，留伴层余量）；pass 计数 ≥ 1；无 crash。

注意：此满核实测与在跑 scan 并存时总内存 = scan(~19G 峰值) + oblas(~13G) 会超界——**必须在 scan 完成后执行本步**，或临时把 scan 的内存余量计入（若 scan 未完成则等待巡检确认 matrix.tsv 出现后再跑）。执行顺序由整体战役时序保证。

- [ ] **Step 4: zstd19 回归**

Run: `./builddir/sdcshield -e zstd19 -t 2000 -n 1`
Expected: `exit: pass`。

- [ ] **Step 5: 提交**

```bash
git add scripts/sdc-excite-reproduce/sdc-excite-reproduce.sh
git commit -s -m 'feat(excite): L4 驻留 openblas_dgemm 满核 mdim 1536——驻留段参数最大化

2026-10-16 标定: oblas mdim 2048 满核峰值 23G(MemAvailable 低点 3.9G)与 mem_saturate
伴层叠加超界, 降档 1536(≈13G, 平方比例外推 2048 实测 23G)。条件注入精确匹配
openblas_dgemm——DWELL_OVERRIDE 事件驱动切换时不携带。'
```

### Task 3: 引擎恢复启动（战役重启）

**Files:**
- 无代码修改。操作任务：scan 完成后启动强化版引擎 + mem_saturate。

**Interfaces:**
- Consumes: Task 1/2 的引擎代码；`~/sdc-excite-reproduce/state.json`（cycle 断点——引擎重启后从 state 的 cycle+1 开始新循环）；巡检 cron（ecb42ac8）负责在 scan 完成时触发本任务。
- Produces: 7×24 战役运行态（引擎 + 三伴层 + 巡检 cron + scan 看守）。

- [ ] **Step 1: 确认 scan 完成且无未决事件**

Run: `ls /home/sdc/sdc-run/scan-maxparam-1016-2150/matrix.tsv`（巡检 cron 已在每次触发时检查）
Expected: 存在；`report.md` 无未定性 fail（已知噪声表排除后）。

- [ ] **Step 2: 启动引擎（强化版）**

```bash
cd /home/sdc/wangxu/sdcshield
nohup bash scripts/sdc-excite-reproduce/sdc-excite-reproduce.sh full >> ~/sdc-excite-reproduce/driver.log 2>&1 & disown
```

- [ ] **Step 3: 启动 mem_saturate（内存拉满伴层）**

```bash
nohup bash ~/sdc-run/mem_saturate.sh >> ~/sdc-run/mem_saturate.log 2>&1 & disown
```

- [ ] **Step 4: 验证三件套运行态**

Run: `pgrep -a -f 'sdc-excite-reproduce.sh|mem_saturate.sh|cpu_backfill.sh|user_monitor.sh' | grep -v pgrep; tail -3 ~/sdc-excite-reproduce/driver.log`
Expected: 4 个进程在列；driver.log 尾部为 `战役启动 mode=full ... cycle起点=...`；3 分钟后 free -g 显示内存被逐步拉满。

---

## 验证边界诚实声明

- 引擎 `full` 模式全链路（cold 30m → l2 → l3 6h → l5 → l4 至 07:30）无法在本次会话内完整端到端验证——验证到"新参数形态的等价单命令实测 + 语法校验 + zstd19 回归"为止，全链路行为由 7×24 巡检持续确认（driver.log 波次行 + 巡检 cron）。
- 本 patch 不触碰 x86-64 测试代码（引擎脚本是部署侧运维脚本，per-arch 隔离不受影响）。

### Task 2b: L3 段 sm3sm4 data_size 固定 1M（病理区规避）

**Files:**
- Modify: `scripts/sdc-excite-reproduce/sdc-excite-reproduce.sh`（`phase_l3()` 的 l3_fixed/l3_rand 调用）

**Interfaces:**
- Consumes: 2026-10-16 实测病理定性——OpenSSL SM4 软实现 ~2.2MB/s 稳态、每迭代 5×data_size EVP 操作：4M 首迭代 128s、8M/16M 单迭代超窗 interrupted（retest 同超时即败）。自适应 hi（600‰ 预算 ≈22M）落在病理区 = 上轮战役 sm3sm4 全核 fail 的机制。
- Produces: L3 两波次带 `-O openssl_sm3sm4.data_size=1048576`（1M ≈ 2.2s/迭代，60s 量子 ~25 迭代）。

- [ ] **Step 1**: phase_l3 内 dis 构造后加 `smo=(-O openssl_sm3sm4.data_size=1048576)`，两个 run_bounded 调用追加 `"${smo[@]}"`。
- [ ] **Step 2**: `bash -n` 校验。
- [ ] **Step 3**: 行为实测 `./builddir/sdcshield --cpuset=<空闲核> -t 60s -e openssl_sm3sm4 -O openssl_sm3sm4.data_size=1048576` → pass 且多迭代（test-runtime ≪ 60s）。
- [ ] **Step 4**: zstd19 回归 pass 后提交。
