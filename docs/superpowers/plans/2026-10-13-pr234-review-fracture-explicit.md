# PR #234 检视意见落实：显式 fracture_loop_count=0 + README 更激进参考 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 落实 PR #234（分支 `jules-6179826238522825613-c1296a5b`）上的检视意见：13 个 ARM64/SVE 测试把删除的 `.fracture_loop_count = -1` 恢复为显式 `.fracture_loop_count = 0`；README 删除无效且语义颠倒的 `SDC_EXTRA_ARGS` 命令行，换成正确的 `--max-test-loop-count` "更激进（深驻留）"参考。

**Architecture:** 两个独立提交单元（CLAUDE.md 规则 1：one patch per unit）：(1) 测试注册字段显式化——值语义与省略字段完全等价（C 零初始化），纯意图自文档化；(2) README 文档修正。不实现 `SDC_EXTRA_ARGS` 透传（三重无效 + 全局禁用 fracture 会破坏战役 L1–L3/L5 多样性轮转设计——那些阶段就是要换种子的；"更激进"的正确落点是深驻留语义参考，战役 L4 阶段已内置）。

**Tech Stack:** C++20 测试注册（`DECLARE_TEST` 宏）、meson/ninja 构建、bash 战役脚本。

**Spec:** 本计划即规格（源自 GitHub PR #234 检视意见：CodeRabbit `discussion_r4254157409` + wangxumarshall 2026-10-13 评论）。

## Global Constraints

- **DCO**：`git commit -s`，最后一行 `Signed-off-by: wangxu <wangxumarshall@qq.com>`，其后无任何内容、无 Co-Authored-By。
- **分支**：全部提交 push 到 PR #234 head 分支 `jules-6179826238522825613-c1296a5b`（本地 `pr-234`），**绝不 push main**。提交前 `git branch --show-current` 确认。
- **自验证 100% 真实**（CLAUDE.md 规则 2）：(a) `ninja -C builddir` 干净构建零新警告；(b) 跑受影响行为并引用真实输出；(c) 回归 `./builddir/sdcshield -e zstd19 -t 2000 -n 1` → `exit: pass`；(d) x86 非回归 by inspection（13 个文件均在 `tests/cpu/arm64/`、`tests/cpu/sve/`——ARM64/SVE 专属测试，不进 x86 构建目标）。
- **已验证的框架语义**（写代码/文档时引用，不得偏离）：
  - `fracture_loop_count`（framework/sandstone.h:403-408）：`<0` 从不 fracture；`0` 默认自动（内循环计数 40 起步、首 10ms 窗口内按需翻倍，每片段换新随机种子）；`>1` 强制 fracture，值为触发内循环计数。
  - `--max-test-loop-count`（framework/sandstone_opts.cpp:242-250 帮助文本 + sandstone_run.cpp:1757,1813）：**指定即禁用 test fracturing**；`=0` 转为 `INT_MAX`（内循环无限制）。"更激进"= `=0` 的固定 seed 单模式连续深驻留（README:323 既有定性："CORE179 类单模式持续暴露"）。
  - `SDC_EXTRA_ARGS` 在 scripts/ 全目录零匹配；systemd 单元只注入 `SDC_EXCITE_REPRODUCE_DIR`；sudo 默认 env_reset —— 该环境变量三重无法到达 sdcshield。
- **回复检视线程**用 `gh api repos/wangxumarshall/sdcshield/pulls/comments/<id>/replies`（线程内回复，不发顶层评论）。

---

### Task 1: 13 个测试显式恢复 `.fracture_loop_count = 0`

**Files（全部 Modify，每文件 1 行插入）：**
- `tests/cpu/arm64/sme_fmopa_stress_arm.cpp`（原 :248 处，`.test_cleanup` 行后）
- `tests/cpu/arm64/sme_za_tileview_arm.cpp`
- `tests/cpu/arm64/sve512_f32_chain_arm.cpp`
- `tests/cpu/arm64/sve512_f32_chain_svd.cpp`
- `tests/cpu/arm64/sve512_f64_chain_arm.cpp`
- `tests/cpu/arm64/sve512_f64_chain_svd.cpp`
- `tests/cpu/arm64/sve512_f64_special_arm.cpp`
- `tests/cpu/arm64/sve512_gather_scatter_arm.cpp`
- `tests/cpu/arm64/sve512_gather_scatter_svd.cpp`
- `tests/cpu/arm64/sve512_stencil_axis_arm.cpp`
- `tests/cpu/arm64/sve512_xlswave_ctx_arm.cpp`
- `tests/cpu/sve/fma2/fma_sve.cpp`
- `tests/cpu/sve/fma2/fpu_special_values_sve.cpp`

**Interfaces:**
- Consumes: PR 分支现状（这些文件中该行已被 PR 删除）；`DECLARE_TEST` 注册块结构 `.test_cleanup = <fn>,` → `.quality_level = ...,`。
- Produces: 每个注册块 `.test_cleanup` 与 `.quality_level` 之间一行 `    .fracture_loop_count = 0,`（4 空格缩进，与 main 上原 `-1` 行同位置）。

- [ ] **Step 1: 切换到 PR 分支并确认状态**

```bash
git branch --show-current        # 期望当前不在 pr-234
git checkout pr-234
git log --oneline -1             # 期望 598a38b8 Maximize SDC excitation based on research
grep -c "fracture_loop_count" tests/cpu/sve/fma2/fma_sve.cpp   # 期望 0（行已被 PR 删除）
```

- [ ] **Step 2: 批量插入显式 `.fracture_loop_count = 0,`**

GNU sed 在每个文件的 `.test_cleanup = ` 行后追加（一次命令处理 13 个文件）：

```bash
sed -i '/^    \.test_cleanup = /a\    .fracture_loop_count = 0,' \
  tests/cpu/arm64/sme_fmopa_stress_arm.cpp \
  tests/cpu/arm64/sme_za_tileview_arm.cpp \
  tests/cpu/arm64/sve512_f32_chain_arm.cpp \
  tests/cpu/arm64/sve512_f32_chain_svd.cpp \
  tests/cpu/arm64/sve512_f64_chain_arm.cpp \
  tests/cpu/arm64/sve512_f64_chain_svd.cpp \
  tests/cpu/arm64/sve512_f64_special_arm.cpp \
  tests/cpu/arm64/sve512_gather_scatter_arm.cpp \
  tests/cpu/arm64/sve512_gather_scatter_svd.cpp \
  tests/cpu/arm64/sve512_stencil_axis_arm.cpp \
  tests/cpu/arm64/sve512_xlswave_ctx_arm.cpp \
  tests/cpu/sve/fma2/fma_sve.cpp \
  tests/cpu/sve/fma2/fpu_special_values_sve.cpp
```

- [ ] **Step 3: 逐处检查插入结果**

```bash
git diff --stat                                   # 期望恰好 13 个文件、每文件 +1 行
git diff -U2 | grep -A2 -B2 "fracture_loop_count" # 每处上下文应为 .test_cleanup 之上、.quality_level 之下
grep -rn "fracture_loop_count = 0" tests/cpu/arm64/ tests/cpu/sve/ | wc -l  # 期望 13
```

确认每处形如（以 fma_sve.cpp 为例）：
```c
    .test_cleanup = fma_sve_finish,
    .fracture_loop_count = 0,
    .quality_level = TEST_QUALITY_PROD,
```

- [ ] **Step 4: 构建验证（零新警告）**

```bash
ninja -C builddir 2>&1 | tail -5    # 若 builddir 缺失：PKG_CONFIG_PATH=./third-party/eigen5 meson setup builddir --buildtype=release && ninja -C builddir
```
期望：构建成功，输出无新 warning。

- [ ] **Step 5: 行为验证——受影响测试真实运行**

13 个测试逐个跑（本机 Kunpeng 920：SVE 测试可跑；SME 测试无 SME 硬件会 SKIP——SKIP 是预期，不得是 FAIL）：

```bash
for t in sme_fmopa_stress_arm sme_za_tileview_arm sve512_f32_chain_arm \
         sve512_f32_chain_svd sve512_f64_chain_arm sve512_f64_chain_svd \
         sve512_f64_special_arm sve512_gather_scatter_arm \
         sve512_gather_scatter_svd sve512_stencil_axis_arm \
         sve512_xlswave_ctx_arm fma_sve fpu_special_values_sve; do
  ./builddir/sdcshield -e "$t" -t 3000 -n 1 2>&1 | tail -2
done
```
期望：无 FAIL/CRASH（pass 或 skip）。

fracture 生效观测（挑一个可跑的，如 `fma_sve`，与 main 的 `-1` 行为对比——本机 main 版本可对照 `git stash` 前先记录）：
```bash
./builddir/sdcshield -e fma_sve -t 5000 -n 1 -v 2>&1 | grep -iE "fractur|seed|iterat" | head -20
```
期望：窗口内出现多次片段重启/种子推进的痕迹（具体字段名以实际 -v 输出为准，如实记录）。

- [ ] **Step 6: 回归验证**

```bash
./builddir/sdcshield -e zstd19 -t 2000 -n 1; echo "exit=$?"
```
期望：`exit: pass`（exit=0）。

- [ ] **Step 7: 提交并 push**

```bash
git branch --show-current    # 必须在 pr-234
git add -u
git commit -s -F- <<'EOF'
tests(arm64,sve): make default auto-fracture explicit via fracture_loop_count = 0 (PR #234 review)

Reviewer asked for the default mode to be spelled out as
"fracture_loop_count == 0" instead of relying on field omission.
fracture_loop_count = 0 selects the framework's automatic fracturing:
inner-loop count starts at 40 and doubles within the first 10 ms window
when needed, and every fragment restarts with a fresh random seed —
maximizing data-pattern diversity for SDC excitation. Semantically
identical to omitting the field (C zero-initialization), but self-
documenting.

This completes the reversal of -1 (commit 7c4dc566 "run light SVE/SME
tests without fracture for full load"): per the PR's research rationale
(>75% of vector SDC cases; multi-P/V/T/data-pattern activation of
marginal defects), per-fragment seed diversity outweighs the fork churn
it costs on these 13 high-value compute tests.

Signed-off-by: wangxu <wangxumarshall@qq.com>
EOF
git log -1 --format=%B | tail -2   # 校验 DCO 尾行、无 Co-Authored-By
git push <remote> pr-234:jules-6179826238522825613-c1296a5b   # remote 名以 git remote -v 为准
```

---

### Task 2: README 修正——删无效行，给出正确的 `--max-test-loop-count` 更激进参考

**Files:**
- Modify: `README.md:81-86`（"一键最大程度激发 SDC（7×24 战役总入口）"代码块 + 其后说明区）

**Interfaces:**
- Consumes: Task 1 已提交（README 描述的行为与之配套：默认 fracture 自动模式）。
- Produces: 代码块恢复 4 行干净命令；代码块后新增一段"更激进"参考（`--max-test-loop-count=0` 深驻留语义 + 等效命令 + 指向战役 L4 内置与模式 D）。

- [ ] **Step 1: 删除无效的 SDC_EXTRA_ARGS 行**

删除 README 中这一行（PR 中 Jules 加入的）：
```
SDC_EXTRA_ARGS="--max-test-loop-count=0" sudo bash scripts/sdc-excite-reproduce/excite.sh # 更激进的激发模式（启用默认的 fracture 自动按需调整机制）
```

- [ ] **Step 2: 在代码块后新增正确的"更激进"参考段**

在代码块结束 ``` 之后、"**使用方法要点**"段落之前插入（文本定稿如下）：

```markdown
**更激进（单模式深驻留）**：给 sdcshield 追加 `--max-test-loop-count=0`——关闭
fracturing，测试窗口内不再换种子重启，单模式连续运行（CORE179 类单模式持续暴露，
语义见 `docs/research/usage.md`）。7×24 战役的 L4 深驻留阶段已内置该模式；以深驻留
为主体的独立战役用模式 D（`scripts/run/run_sdc_spectrum.sh` 阶段 2）。等效命令：

```bash
./builddir/sdcshield --quality=0 -T forever -t 60s --max-test-loop-count=0 -Y -F -o sdc-$(date +%m%d-%H%M).yaml
```
```

（注：等效命令与 README:61-66 既有等效命令同形，仅追加 `--max-test-loop-count=0`；预构建路径 `./run-sdcshield.sh` 同理。执行时按上下文微调措辞使风格一致，但三要素不得缺：①`=0` 关闭 fracturing 的正确语义 ②战役 L4 已内置 ③模式 D 参考。）

- [ ] **Step 3: 验证**

```bash
git diff README.md            # 肉眼复核：删 1 行、增 1 段；代码块恢复 4 行
grep -n "SDC_EXTRA_ARGS" README.md scripts/sdc-excite-reproduce/*.sh   # 期望全部零匹配
grep -n "max-test-loop-count" README.md   # 期望 :323 既有行 + 新增段落出现
./builddir/sdcshield --quality=0 -e zstd19 -t 2000 --max-test-loop-count=0 -n 1; echo "exit=$?"
# 等效命令里的新选项真实可跑（不引 -T forever 以免挂死；验证选项组合有效即可）
```
期望：grep 零匹配/正确出现；选项组合运行 `exit: pass`。

- [ ] **Step 4: 提交并 push**

```bash
git branch --show-current    # 必须在 pr-234
git add README.md
git commit -s -F- <<'EOF'
docs(readme): replace broken SDC_EXTRA_ARGS line with correct --max-test-loop-count deep-dwell reference (PR #234 review)

The SDC_EXTRA_ARGS line added earlier in this PR was triply ineffective:
excite.sh and the campaign runner never read it, sudo scrubs the
environment by default, and the systemd unit only injects
SDC_EXCITE_REPRODUCE_DIR. Its comment was also inverted:
--max-test-loop-count disables test fracturing (see the option's help
text in framework/sandstone_opts.cpp), it does not enable automatic
fracture adjustment.

Replace it with a correct "more aggressive" reference per review:
--max-test-loop-count=0 closes fracturing for a fixed-seed single-
pattern deep dwell (CORE179-class sustained exposure), already built
into the campaign's L4 dwell stage; standalone equivalent command and
the mode-D (run_sdc_spectrum.sh stage 2) pointer included.

Addresses CodeRabbit discussion_r4254157409.

Signed-off-by: wangxu <wangxumarshall@qq.com>
EOF
git log -1 --format=%B | tail -2   # 校验 DCO 尾行
git push <remote> pr-234:jules-6179826238522825613-c1296a5b
```

---

### Task 3: 回复 GitHub 检视线程

**Files:** 无仓库文件（GitHub API 操作）。

- [ ] **Step 1: 回复 CodeRabbit 意见 C 线程（comment id 4254157409）**

```bash
gh api repos/wangxumarshall/sdcshield/pulls/234/comments/4254157409/replies \
  -f body="Verified and fixed differently per reviewer direction: the \`SDC_EXTRA_ARGS\` line is removed (it was never consumed by excite.sh/the runner, and its comment inverted the option's semantics). README now documents \`--max-test-loop-count=0\` correctly as the more-aggressive deep-dwell mode (closes fracturing; fixed-seed single-pattern sustained run, CORE179-class exposure), notes the campaign's L4 dwell stage already builds it in, and points to mode D / usage.md."
```

- [ ] **Step 2: 确认 PR 状态**

```bash
gh pr view 234 --repo wangxumarshall/sdcshield --json headRefOphem 2>/dev/null || gh pr view 234 --repo wangxumarshall/sdcshield
# 确认新提交出现在 PR 时间线、检视线程已有回复
```

---

## Self-Review 结论

- **规格覆盖**：用户要求 ①显式 `fracture_loop_count == 0` → Task 1；②README 更激进 `--max-test-loop-count` 参考（尤其一句话命令区块）→ Task 2；CodeRabbit 意见 C（删无效行）→ Task 2 Step 1；意见 A/B 已在 PR 内解决（最终 diff 无 virtualization/fpcr 文件改动），无需任务。✓
- **占位符扫描**：无 TBD/TODO；每步含具体命令与期望输出。✓
- **一致性**：`.fracture_loop_count = 0,` 写法全文一致；`--max-test-loop-count=0` 语义表述与 framework 帮助文本、README:323、usage.md:60 三处一致。✓
- **明确不做**：不实现 `SDC_EXTRA_ARGS` 透传（三重无效是表象，本质是全局禁用 fracture 会破坏战役 L1–L3/L5 多样性轮转设计——该设计正是 PR 主题）。
