# Multi-OS Verify CI 修复（--full-log 逃生门）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 Multi-OS Verify 每日 CI 恢复绿：三个按测试条目解析 YAML 的 GHA 消费者显式加 `--full-log`，退出 8b5267a9 引入的紧凑日志默认。

**Architecture:** 8b5267a9（PR #772，2026-11-15 合入 main）把 YAML 日志默认改为紧凑模式：pass/skip 条目折叠为单行内联 `- { test: x, result: pass }`（无 test-runtime、无 state/seed 行）。仓内三个 CI 消费者用旧多行块格式（`^- test:` / `^  result:`）逐条解析，内联格式全部不可见 → pass 计数恒 0 → 门禁红。该提交自带逃生门 `--full-log`（`compact_log=false`，每个测试恢复完整条目）；人机交互保留紧凑默认，CI 消费者 opt-in。零框架改动、零解析器改动。

**Tech Stack:** 纯脚本/Workflow YAML（Python3 stdlib / bash / GitHub Actions）；无 C/C++ 改动。

**Spec:** 本计划 Background 节即根因证据链（2026-11-17 会话从 run 43863891250 artifact 实证），无独立 spec 文档。

## Background: 根因证据链（已实证，勿重复调查）

- 失败 run：`Multi-OS Verify` 43863891250（2026-11-16）、43709619760（2026-11-15），15 矩阵全红于 `Verify (full parameter sweep)`；11-14 run 43587899480 绿（headSha 060812ea）。
- 两 run 之间 main 恰好只合入 `8b5267a9 feat: add --full-log flag and reduce default log verbosity`（f7817cb3 merge，PR #772）。
- 失败现场（20.03-LTS-SP2 job log）：
  `[all-quality(-n1)] tests=9 pass=0 skip=0 soft=0 fail=0 crash=0 seeds=9 (FAIL)`；232 个条目是内联 `- { test: ..., result: pass }`，9 个旧格式块全是 `result: timed out`（eigen 60s 超时，`\S+` 截断 "timed" 故 soft=0）。
- 紧凑 pass 条目**不含 test-runtime 与 state/seed 行** → 教解析器认新格式救不回耗时矩阵与 F-009 种子链，opt-in `--full-log` 是唯一保全证据的修法。
- `benchmark.sh`（同 workflow，本次因 Verify 先死被跳过）awk 同款解析 → 不修则修好 Verify 后次日接力红。
- `pr.yaml` quick 套件 → `report-summary.py` PR 矩阵同款静默退化（非门禁，`|| true`）。
- 失败模式是 **fail-closed**（红了但不静默过）——这是可接受底线；本修复不加解析器防御（YAGNI），契约写进注释与提交说明。

## Global Constraints

- One patch per unit：每个消费者一个 commit，各自验证后提交（rule 1）。
- 每 commit 自验证 100% 实证：改前先本地复现红、改后引真实输出（rule 2）；回归 `-e zstd19 -t 2000 -n 1` → `exit: pass`；x86 非回归按检查（本修复零框架改动，flag 解析架构无关）。
- DCO：`git commit -s`，末行 `Signed-off-by: wangxu <wangxumarshall@qq.com>`，其后无内容、无 Co-Authored-By（rule 3）。
- 提交前 `git branch --show-current`；只推 feature 分支（fix/multi-os-verify-full-log），永不 main。
- 本机（SVE1-256 主机）构建：`./scripts/build.sh` 干净零新警告为基线。
- 不动 x86-64（rule 5）：零 C/C++ 文件改动。

## Review Focus

1. **未来调用忘加 `--full-log`** → 必须 fail-closed：Task 1 改前复现步骤钉住（未修脚本对紧凑 YAML 判 FAIL 红，非静默过）。
2. **老二进制无 `--full-log` 选项**（板端部署）会拒认该 flag → 本修复**只**加在 GHA 每次从 main 现构建的链路；板端/campaign 工具（sdc-run.sh、sdc_common.sh、sdc-excite-reproduce.sh、extract_fail_iterations_clean.sh）明确不碰，PR 披露为跟进项。
3. **F-009 种子链**：`--full-log` 必须恢复逐测试 `state: { seed:` 行 → Task 1 断言 smoke seeds≥1 且全量式命令解析出 seed。
4. **TSV 三方契约**（test/wall_seconds/loop_count）不变 → Task 2 用 `benchmark-gate.py verify` rc=0 钉住。
5. **smoke 路径独立组命令**（不走 COMMON）→ Task 1 的 --smoke 本地实跑钉住，否则 workflow_dispatch smoke 档仍坏。

---

### Task 1: verify-params.py 全面 opt-in `--full-log`（每日红的直接修复）

**Files:**
- Modify: `scripts/gha/verify-params.py:219-220`（COMMON）、`:258-259`（smoke zstd19 cmd）、`:336-338`（openblas mdim cmd）、COMMON 注释块（:213-218）

**Interfaces:**
- Consumes: `builddir/sdcshield`（本机已建，main @ 8b5267a9 之后，含 `--full-log`）。
- Produces: `results/smoke_zstd19.yaml`、`results/allquality.yaml` 等恢复旧格式（多行 `- test:` + `  result:` + `  state:`），`report` job 与 `report-summary.py` 无需改动即可工作。

- [ ] **Step 1: 建分支**

Run: `git checkout -b fix/multi-os-verify-full-log && git branch --show-current`
Expected: `fix/multi-os-verify-full-log`

- [ ] **Step 2: 改前本地复现（fail-closed 证明）**

Run: `python3 -u scripts/gha/verify-params.py --bin builddir/sdcshield --out /tmp/vp-repro --smoke`（未修脚本）
Expected: `RESULT: FAIL`，行内 `pass=0`、`seeds=0`（或极少）——与 CI 每日红同根因的本地实锤；留下完整输出引文。

- [ ] **Step 3: 施改**

三处加 `"--full-log"`：COMMON 列表（`"-vvv"` 之后，同 COMMON 注释块补一行契约说明：「`--full-log`: 8b5267a9 起紧凑日志为默认，pass/skip 条目折叠为单行内联且无 seed/runtime——本脚本的逐条解析（RESULT_RE/RESULT_VAL/SEED_*_RE）与 F-009 种子链、report 耗时矩阵都依赖完整条目，必须 opt-in」）；smoke zstd19 cmd（`-vvv` 后）；openblas mdim cmd（`-vvv` 后）。

- [ ] **Step 4: 改后验证（真实输出）**

Run: `python3 -u scripts/gha/verify-params.py --bin builddir/sdcshield --out /tmp/vp-fixed --smoke`
Expected: `RESULT: PASS`，`pass=1 seeds>=1`；并抽 `/tmp/vp-fixed/results/smoke_zstd19.yaml` 引一行 `  state: { seed: '...'` 证种子链回归。
Run: `builddir/sdcshield --quality=-1 -n 1 -t 5000 -vvv --retest-on-failure=0 --max-test-loop-count 3 --timeout=60s --max-messages 0 --ignore-timeout --disable mesh_upi* --disable memcpy_rewr --full-log -e zstd19 -o /tmp/vp-allq-subset.yaml` 后用 `python3 -c` 调 `parse_result_file` 断言 pass=1、seeds≥1（全量段命令形状同款走查）。
Run: `builddir/sdcshield -e zstd19 -t 2000 -n 1` → `exit: pass`（回归）。

- [ ] **Step 5: Commit**

```bash
git add scripts/gha/verify-params.py
git commit -s -m "fix(gha): verify-params.py opt in --full-log — 紧凑日志默认(8b5267a9)使逐条解析全盲"
```

### Task 2: benchmark.sh opt-in `--full-log`（同 workflow 的下一个红）

**Files:**
- Modify: `scripts/gha/benchmark.sh:97-98`（每测试 `$BIN` 调用）+ 头部注释一行契约

**Interfaces:**
- Produces: `benchmark.tsv` 每行 wall_seconds 恢复数值（或良性 skip），`benchmark-gate.py`/`benchmark-to-json.py` 无需改动。

- [ ] **Step 1: 改前复现（单测试级）**

Run: 未修脚本形状——`builddir/sdcshield --max-test-loop-count 2 --retest-on-failure=0 -n 1 -e crc32 -o /tmp/bm-c.yaml` 后跑脚本同款 awk 提取 RES/RT。
Expected: RES 为空（内联条目无 `^- test: crc32` 块）→ 脚本将落 `fail` 行交闸门红。

- [ ] **Step 2: 施改**

`"$BIN"` 调用加 `--full-log`（与 `-o "$YAML"` 同续行），头部注释补一行同 Task 1 契约句。

- [ ] **Step 3: 全脚本真实跑（受影响行为）**

Run: `bash scripts/gha/benchmark.sh "$(pwd)/builddir/sdcshield" /tmp/bm-fixed`（后台，10 测试实测数分钟）
Expected: TSV 10 数据行全数值（合法 skip 可）、无 `fail` 行；`python3 scripts/gha/benchmark-gate.py verify /tmp/bm-fixed/benchmark.tsv` → rc=0。引用 TSV 全文。

- [ ] **Step 4: Commit**

```bash
git add scripts/gha/benchmark.sh
git commit -s -m "fix(gha): benchmark.sh opt in --full-log — 同根因,修好 Verify 后的次日红"
```

### Task 3: pr.yaml quick 套件 opt-in（PR 页矩阵恢复）

**Files:**
- Modify: `.github/workflows/pr.yaml:137-144`（quick suite `builddir/sdcshield --quick ...` 命令）

**Interfaces:**
- Consumes: Task 1 的契约（同 flag）。
- Produces: `quick.yaml` 旧格式 → `report-summary.py` 矩阵恢复 `PASS[耗时s]`。

- [ ] **Step 1: 施改**

quick 套件命令加 `--full-log`（`--retest-on-failure=0` 后），命令上方注释补一行契约句。

- [ ] **Step 2: 本地真实跑（受影响行为）**

Run: 按 pr.yaml 原样命令本地执行（`--quick -n4 --retest-on-failure=0 --disable … --full-log -o /tmp/quick.yaml`），再 `python3 scripts/gha/report-summary.py`（把 /tmp/quick.yaml 摆成 `all-results/verify-local/results/allquality.yaml`）。
Expected: 矩阵出现带耗时的 PASS 行；引首几行。
x86 非回归（检查）：flag 为纯选项解析、无架构分支；pr.yaml 矩阵在 x86 runner 同样从源构建含该 flag。

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/pr.yaml
git commit -s -m "fix(ci): pr.yaml quick 套件 opt in --full-log — 恢复 PR 页结果矩阵的逐条解析"
```

### Task 4: 推送 + PR + 交接验证

- [ ] **Step 1:** `git push -u origin fix/multi-os-verify-full-log`，`gh pr create` 到 main。PR body 含：根因链（run 43863891250/43709619760、8b5267a9）、本地验证输出引文、**披露跟进项**（板端/campaign 工具同款解析未修；usage.md 未载 --full-log 系 8b5267a9 文档缺口）。
- [ ] **Step 2:** PR CI（pr.yaml 自身）绿；指出 11-17 进行中的 run 43998000729 预期仍红（main 未修）。
- [ ] **Step 3:** 用户 merge 后：对 main 手动 `workflow_dispatch`（verify_depth=smoke）证 Verify 步骤机械恢复；次日 12:00 定时全量 run 绿为终验。
