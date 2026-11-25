# adaptive shm 0 字节映射 SIGBUS 修复 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 消除 `adaptive_mem_sdc` / `adaptive_l3_sdc` 两处 `shm_setup()` 中 latecomer 子进程对 0 字节 tmpfs 对象 mmap 后首次触碰的 SIGBUS（BUS_ADRERR），手段是幂等 ftruncate——所有打开者（creator 与 latecomer）在 mmap 前一律 `ftruncate`。

**Architecture:** 崩溃根因（2026-11-24 板上 fail，本机反汇编+复现实证）：`shm_open(O_CREAT|O_EXCL)` 与 creator 的 `ftruncate` 之间存在窗口，latecomer 可在此窗口内 `shm_open(O_RDWR)` + `mmap` 成功（Linux 对 0 长度 tmpfs 文件 mmap 只建 VMA 不查大小），随后 828 行 `__atomic_load_n(&shm->ready, ACQUIRE)` 首次触碰 page fault 超出 i_size → `VM_FAULT_SIGBUS`。creator 死于窗口内则对象永久 0 字节，后续所有 child 确定性撞死。修复：把 `if (creator && ftruncate(...))` 改为人人 ftruncate（幂等、零成本），V1（现场竞态）/V3（残留）两类触发一并消除。`ready` 自旋协议（anchor 可见性）语义不变。

**Tech Stack:** POSIX shm（`shm_open`/`ftruncate`/`mmap`）、GCC `__atomic_*`、sdcshield meson/ninja 构建。

**Spec:** 本文档即 spec（根因分析随附于 2026-11-25 会话，复现器 V1/V2/V3 实证）。

## Global Constraints

- CLAUDE.md 规则 1：一 patch 一单元——计划文档、`adaptive_mem_sdc` 修复、`adaptive_l3_sdc` 修复分别为独立 commit，逐个验证后 push。
- CLAUDE.md 规则 2：commit 前 100% 实证——`./scripts/build.sh` 零新警告；受影响行为真实运行引用真实输出；回归 `-e zstd19 -t 2000 -n 1` → `exit: pass`。
- CLAUDE.md 规则 3：`git commit -s`（末行 `Signed-off-by: wangxu <wangxumarshall@qq.com>`，无 Co-Authored-By）；push 到 feature 分支（`fix/adaptive-shm-sigbus`），永不 push main；commit 前先 `git branch --show-current`。
- CLAUDE.md 规则 5：x86-64 不动——两处编辑均位于 `#if defined(__aarch64__)` 块内（`adaptive_mem_sdc.cpp:765`、`adaptive_l3_sdc.cpp:141` 起），逐条引用守卫行核实。
- 不改自旋协议、不改锚点语义、不新增 creator 接管逻辑（超时 idle 回退是既有设计行为，保持）。
- 验证里所有 `/dev/shm` 试验对象用后必须删除。

---

### Task 1: 建分支 + 预修复基线（failing test）

**Files:**
- Create: `docs/superpowers/plans/2026-11-25-adaptive-shm-sigbus-fix.md`（本文件）
- 无代码修改。

**Interfaces:**
- Produces: 分支 `fix/adaptive-shm-sigbus`；预修复基线输出（SIGBUS fail 的真实引用），作为 Task 2/3 修复后的 A/B 对照。
- 验证前提：当前 `builddir/sdcshield` 与 HEAD 一致。

- [ ] **Step 1: 建分支**

```bash
cd /home/sdc/wangxu/operator/sdcshield
git branch --show-current          # 期望 main（若已在他分支，停下人工确认）
git checkout -b fix/adaptive-shm-sigbus
```

- [ ] **Step 2: 提交计划文档**

```bash
git add docs/superpowers/plans/2026-11-25-adaptive-shm-sigbus-fix.md
git commit -s -m "docs(plan): 2026-11-25 adaptive shm 0字节对象映射 SIGBUS 修复计划（幂等 ftruncate）"
```

- [ ] **Step 3: 构建当前代码**

```bash
./scripts/build.sh
```
Expected: 零警告完成；`builddir/sdcshield` 为 HEAD 状态。

- [ ] **Step 4: 布设确定性 V3 陷阱（adaptive_mem_sdc，0 字节残留对象）并运行——期望 SIGBUS fail**

```bash
rm -f /dev/shm/adaptive_mem_sdc_fixab
: > /dev/shm/adaptive_mem_sdc_fixab          # 0 字节对象 = creator 死于窗口内的残骸现场
ls -l /dev/shm/adaptive_mem_sdc_fixab        # 确认 0 字节
ADAPTIVE_MEM_SDC_KEY=fixab ./builddir/sdcshield -e adaptive_mem_sdc -t 3000 2>&1 | tee /tmp/fixab_prefix_run1.log; echo exit=$?
grep -c 'signal 7\|Bus error' /tmp/fixab_prefix_run1.log
```
Expected: 输出含 `Received signal 7 (Bus error) ... BUS_ADRERR`（crash handler 打印），`exit` 非 0（test fail）。此即"失败测试"基线，引用真实输出。

- [ ] **Step 5: 同样陷阱布给 adaptive_l3_sdc——期望同样 SIGBUS fail**

```bash
rm -f /dev/shm/adaptive_l3_sdc_fixab
: > /dev/shm/adaptive_l3_sdc_fixab
ADAPTIVE_L3_SDC_KEY=fixab ./builddir/sdcshield -e adaptive_l3_sdc -t 3000 2>&1 | tee /tmp/fixab_prefix_run2.log; echo exit=$?
grep -c 'signal 7\|Bus error' /tmp/fixab_prefix_run2.log
```
Expected: 同样 SIGBUS fail。

- [ ] **Step 6: 清理陷阱对象**

```bash
rm -f /dev/shm/adaptive_mem_sdc_fixab /dev/shm/adaptive_l3_sdc_fixab
```

### Task 2: 修复 adaptive_mem_sdc.cpp

**Files:**
- Modify: `tests/cpu/misc/adaptive_mem_sdc.cpp:781-784`（shm_setup 头注释）、`:807-810`（ftruncate 条件）

**Interfaces:**
- Consumes: Task 1 的预修复基线（同名陷阱修复后必须转为 pass）。
- Produces: `shm_setup()` 中人人 ftruncate；对 Task 3 无代码依赖（L3 为镜像编辑）。

- [ ] **Step 1: 编辑函数头注释**（`tests/cpu/misc/adaptive_mem_sdc.cpp:781-784`）

旧文本：
```c
/* Map (or create) the named shm.  O_EXCL decides the creator; the
 * creator publishes the anchor then sets ready (release).  Latecomers
 * spin on ready (acquire) with a bounded wait — on timeout the mapping
 * is returned anyway and pick_combo() falls back to idle. */
```
新文本：
```c
/* Map (or create) the named shm.  O_EXCL decides the creator; the
 * creator publishes the anchor then sets ready (release).  Latecomers
 * spin on ready (acquire) with a bounded wait — on timeout the mapping
 * is returned anyway and pick_combo() falls back to idle.
 * EVERY opener ftruncates before mmap (idempotent), not just the
 * creator: mmap of a 0-byte tmpfs file succeeds with a bare VMA, and
 * the first touch (the ready spin, offset 0) then raises SIGBUS /
 * BUS_ADRERR — reachable while the creator is still between shm_open
 * and ftruncate, or forever after a creator died there leaving a
 * stale 0-byte object (2026-11-24 board fail at the line-828 ldar;
 * V1/V3 repro on the dev host). */
```

- [ ] **Step 2: 编辑 ftruncate 条件**（`tests/cpu/misc/adaptive_mem_sdc.cpp:807-810`）

旧文本：
```c
    if (creator && ftruncate(fd, sizeof(struct AdaptiveShm)) != 0) {
        close(fd);
        return nullptr;
    }
```
新文本：
```c
    if (ftruncate(fd, sizeof(struct AdaptiveShm)) != 0) {
        close(fd);
        return nullptr;
    }
```
说明：`creator` 变量在下方 `if (creator) { memset/anchor/ready store } else { spin }` 仍被使用，不会产生 unused 警告。

- [ ] **Step 3: 重建**

```bash
./scripts/build.sh
```
Expected: 零新警告。

- [ ] **Step 4: 重布同款陷阱——期望 pass（A/B 翻转）**

```bash
rm -f /dev/shm/adaptive_mem_sdc_fixab
: > /dev/shm/adaptive_mem_sdc_fixab
ADAPTIVE_MEM_SDC_KEY=fixab ./builddir/sdcshield -e adaptive_mem_sdc -t 20000 2>&1 | tee /tmp/fixab_postfix_run1.log; echo exit=$?
rm -f /dev/shm/adaptive_mem_sdc_fixab
```
Expected: `exit=0`（pass）。行为：child 幂等 ftruncate 撑起 0 字节对象 → bounded spin（200000×10µs，实测约 2~12s，故 `-t 20000`）超时 → `pick_combo` 的 ready 守卫 → idle → EXIT_SUCCESS。若框架在 spin 中途按 `-t` 杀 child 导致 fail，改 `-t 40000` 重跑一次并引用输出。

- [ ] **Step 5: 双实例同 KEY 并发冒烟（V1 路径压力）**

```bash
rm -f /dev/shm/adaptive_mem_sdc_fixov
ADAPTIVE_MEM_SDC_KEY=fixov ./builddir/sdcshield -e adaptive_mem_sdc -t 5000 > /tmp/fixov_a.log 2>&1 &
ADAPTIVE_MEM_SDC_KEY=fixov ./builddir/sdcshield -e adaptive_mem_sdc -t 5000 > /tmp/fixov_b.log 2>&1 &
wait
grep -l 'Bus error' /tmp/fixov_a.log /tmp/fixov_b.log; echo grep_exit=$?
rm -f /dev/shm/adaptive_mem_sdc_fixov
```
Expected: `grep_exit=1`（两实例均无 Bus error）。

- [ ] **Step 6: verify 模式 round-robin 冒烟 + 常规无 KEY 路径**

```bash
ADAPTIVE_MEM_SDC_VERIFY=1 ADAPTIVE_MEM_SDC_KEY=fixvf ./builddir/sdcshield -e adaptive_mem_sdc -t 8000 2>&1 | tail -5; echo exit=$?
rm -f /dev/shm/adaptive_mem_sdc_fixvf
./builddir/sdcshield -e adaptive_mem_sdc -t 3000 2>&1 | tail -3; echo exit=$?
```
Expected: 两路均 `exit=0`；verify 路径走 `verify_ctr` fetch_add（shm 上的原子链路）。

- [ ] **Step 7: 回归**

```bash
./builddir/sdcshield -e zstd19 -t 2000 -n 1 2>&1 | tail -3; echo exit=$?
```
Expected: `exit: pass`。

- [ ] **Step 8: commit + push**

```bash
git branch --show-current   # 期望 fix/adaptive-shm-sigbus
git add tests/cpu/misc/adaptive_mem_sdc.cpp
git commit -s -m "fix(arm64): adaptive_mem_sdc shm 幂等 ftruncate——修 latecomer 对 0 字节对象 mmap 后首次触碰 SIGBUS(2026-11-24 板上 fail 根因)"
git push -u origin fix/adaptive-shm-sigbus
```

### Task 3: 修复 adaptive_l3_sdc.cpp（镜像编辑）

**Files:**
- Modify: `tests/cpu/L3_cache_920/adaptive_l3_sdc.cpp:1090`（al3_shm_setup 头注释）、`:1112-1115`（ftruncate 条件）

**Interfaces:**
- Consumes: Task 2 的验证流程（同款步骤镜像）。
- Produces: 无下游依赖。

- [ ] **Step 1: 编辑函数头注释**（`tests/cpu/L3_cache_920/adaptive_l3_sdc.cpp:1090`）

旧文本：
```c
/* Map (or create) the named shm; creator publishes the anchor */
```
新文本：
```c
/* Map (or create) the named shm; creator publishes the anchor.  Every
 * opener ftruncates before mmap (idempotent): mmap of a 0-byte tmpfs
 * file succeeds with a bare VMA and the first ready-spin touch then
 * raises SIGBUS/BUS_ADRERR if the creator is mid-window or died there
 * — same 2026-11-24 root cause as adaptive_mem_sdc's shm_setup. */
```

- [ ] **Step 2: 编辑 ftruncate 条件**（`tests/cpu/L3_cache_920/adaptive_l3_sdc.cpp:1112-1115`）

旧文本：
```c
    if (creator && ftruncate(fd, sizeof(struct AL3Shm)) != 0) {
        close(fd);
        return nullptr;
    }
```
新文本：
```c
    if (ftruncate(fd, sizeof(struct AL3Shm)) != 0) {
        close(fd);
        return nullptr;
    }
```

- [ ] **Step 3: 重建**

```bash
./scripts/build.sh
```
Expected: 零新警告。

- [ ] **Step 4: 重布 L3 陷阱——期望 pass**

```bash
rm -f /dev/shm/adaptive_l3_sdc_fixab
: > /dev/shm/adaptive_l3_sdc_fixab
ADAPTIVE_L3_SDC_KEY=fixab ./builddir/sdcshield -e adaptive_l3_sdc -t 20000 2>&1 | tee /tmp/fixab_postfix_run2.log; echo exit=$?
rm -f /dev/shm/adaptive_l3_sdc_fixab
```
Expected: `exit=0`（idle 回退，同 Task 2 Step 4 机理；L3 的 `al3_pick_combo` 亦有 `ready != 1u → return -1` 守卫，`adaptive_l3_sdc.cpp:1195`）。

- [ ] **Step 5: L3 常规无 KEY 路径冒烟**

```bash
./builddir/sdcshield -e adaptive_l3_sdc -t 3000 2>&1 | tail -3; echo exit=$?
```
Expected: `exit=0`。

- [ ] **Step 6: 回归**

```bash
./builddir/sdcshield -e zstd19 -t 2000 -n 1 2>&1 | tail -3; echo exit=$?
```
Expected: `exit: pass`。

- [ ] **Step 7: commit + push**

```bash
git branch --show-current   # 期望 fix/adaptive-shm-sigbus
git add tests/cpu/L3_cache_920/adaptive_l3_sdc.cpp
git commit -s -m "fix(arm64): adaptive_l3_sdc 同款 shm 幂等 ftruncate——al3_shm_setup 与 adaptive_mem_sdc 同根因(creator-only ftruncate 致 latecomer 0 字节映射 SIGBUS)"
git push
```

### Task 4: 终局验证 + x86 非回归声明

**Files:**
- 无代码修改（纯验证与记录）。

**Interfaces:**
- Consumes: Task 2/3 的全部验证结果。
- Produces: 终局验证记录（引用真实输出）。

- [ ] **Step 1: 全量构建复核**

```bash
./scripts/build.sh
```
Expected: 零警告。

- [ ] **Step 2: 两测试最终各跑一路 + 回归**

```bash
./builddir/sdcshield -e adaptive_mem_sdc -t 3000 >/dev/null 2>&1; echo mem_exit=$?
./builddir/sdcshield -e adaptive_l3_sdc -t 3000 >/dev/null 2>&1; echo l3_exit=$?
./builddir/sdcshield -e zstd19 -t 2000 -n 1 >/dev/null 2>&1; echo zstd_exit=$?
```
Expected: `mem_exit=0`、`l3_exit=0`、`zstd_exit=0`。

- [ ] **Step 3: x86-64 非回归（检视声明）**

核实并引用：两处编辑均在 aarch64 守卫内——`tests/cpu/misc/adaptive_mem_sdc.cpp` 的 `shm_setup` 位于 `#if defined(__aarch64__)`（765 行起）块内；`tests/cpu/L3_cache_920/adaptive_l3_sdc.cpp` 的 `al3_shm_setup` 位于 `#if defined(__aarch64__)`（141 行起）块内（1385 行 `#else` 为非 aarch64 桩）。x86-64 构建的该两 TU 文本不变。

- [ ] **Step 4: 残留对象清点**

```bash
ls -l /dev/shm/ | grep -c adaptive_
```
Expected: `0`（所有试验对象已删）。

- [ ] **Step 5: 最终 push 确认**

```bash
git log --oneline origin/main..fix/adaptive-shm-sigbus
git status --short --branch | head -3
```
Expected: 3 个 commit（plan + 两个 fix）已全部推送，工作区无相关未提交修改。

## Self-Review 结论

- 覆盖：根因（creator-only ftruncate）→ 两文件同修；V1（Step 5 并发）/V3（Step 4 确定性陷阱）两触发路径都被 A/B 验证；协议语义不变已由 Step 6 verify 冒烟覆盖。
- 无占位符：所有步骤含完整命令与期望输出。
- 类型一致：`struct AdaptiveShm`/`struct AL3Shm` 各归各文件，无跨文件符号引用。

---

## 设计修订（2026-11-25 当日，幂等 ftruncate → fstat 门控）

初版修复（creator 与 latecomer 一律 ftruncate，幂等自愈）执行完毕并推送后，用户提出对抗性复审并给出替代设计：**latecomer 在 mmap 前 fstat 门控等待 creator 完成尺寸化，超时（500000×usleep(10)）返回 nullptr → idle；latecomer 保持纯观察者，只有 creator 尺寸化对象**。

复审裁决（两设计 in-model 正确性等价，差异在残留风险选择）：
- 门控版优势：latecomer **零文件系统写入面**——彻底消除幂等版新引入的 symlink-truncate 放大（R1）与混版本收缩两个风险面；Boost.Interprocess 式 readiness gating，模式辨识度更高。
- 幂等版优势被下调为"化妆性"：死 creator 下 ready 永不发布，愈尺寸不愈功能，两设计的死键同样功能性死亡、同样依赖 campaign 侧清理；此优势不足以抵偿写入面收益。
- 采纳门控版。重置分支重做两个 fix commit。

修订后实测补充（诚实口径）：
- 500000×(fstat+usleep(10)) 本机实测 **31.3s**（timer slack 使 usleep(10) 实际 ~62µs），"5s 标称"是纯算术口径，已写入代码注释；既有 ready-spin（200000 次）真实上界同为 ~12.5s 而非其注释宣称的 2s（既有文本，本补丁不动）。
- 死键（0 字节残骸）场景下，门控版每子轮询 ~31s 后 idle；`-t 30000` 的 campaign 实例窗内 child 会被框架在轮询中途按窗终杀（正常生命周期，非 crash、无 covfile 行）——运维观察死键应改看"covfile 无行"而非"combo=-1 洪峰"。
- 预修复基线不变（两测试各 11×Bus error、exit=1）；门控版同陷阱翻转 pass，三实例并发全 pass，latecomer 经门控读到 creator 锚点跑真组合（COVFILE 实证 combo 12），verify/常规/zstd19 全 pass。

