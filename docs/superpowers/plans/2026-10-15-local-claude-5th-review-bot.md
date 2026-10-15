# 本地 claude code 作为第 5 个 review bot 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让本地无头 claude code 成为 pr-loop 的第 5 个 review bot——对抗性检视、意见上 GitHub、与四 bot 同等对待、agent 对 5 个 bot 的每条意见逐条闭环答复。

**Architecture:** 新增 `scripts/pr_local_review.py`（本地 claude 对抗审查引擎，零第三方依赖），扩展 `scripts/pr_loop.py`（五 bot 判据 + wait 内触发 + 闭环答复），扩展 `scripts/ci_triage.py`（车道 B 主动触发避免硬门槛死锁）。两条触发线：事件线（pr-loop wait 内主动触发）+ 时间线（cron 兜底）。

**Tech Stack:** Python 3 标准库 + `gh` CLI 子进程 + `claude -p --dangerously-skip-permissions`（认证 source `agent.env`，ANTHROPIC_AUTH_TOKEN + ANTHROPIC_BASE_URL + 模型映射）。

**Spec:** `docs/superpowers/specs/2026-10-15-local-claude-5th-review-bot-design.md`

## Global Constraints

- 第 5 bot 身份：gh CLI 以 `wangxumarshall` 发 review，正文含标记 `<!-- local-claude-review -->`；判据 = `作者 wangxumarshall` **且** `正文含标记`（双条件，专用判定分支，非 LOGIN_TO_BOT 纯作者映射）。
- `BOT_ORDER` 扩展为 `("codex", "coderabbit", "pr-agent", "greptile", "local-claude")`。
- 硬门槛：五 bot 等齐才闭环，缺第 5 bot 即 `WAITING_BOTS`。
- 两条触发线：wait 内主动触发（事件）+ cron `*/30`（时间），同 head 幂等（`last_dispatched_head` 语义）。
- 意见形态：每条 finding 尽量挂**行级评论**（diff 相关行）；挂不上 diff 行的退到 review body 的 JSON 段。review body 首行带标记。
- 闭环逐条答复：行级评论 finding → `gh api` 逐条 reply；body JSON 段 finding → 闭环时发一条 issue comment 按 finding 逐条列出答复（接纳带 SHA / 驳回带实证）；漏答即报错。
- 对抗性 prompt 五要素：pentester framing、多批判镜头（Skeptic/Architect/Minimalist，按 diff 大小缩放）、结构化证据锚定（file:line + 证据 + 严重级 + 分类）、意图镜、差分审查顺序（先删后增再影响）。
- 审查用独立只读 worktree（`git worktree add` 或临时 checkout），不碰用户主工作区、不抢 autoci clone。
- `.github/workflows/bot-coverage.yml` 保持四 bot 不变（判据分裂显式记录，不改该 workflow）。
- 零第三方依赖（stdlib + gh + claude）；认证 `source ~/.local/state/sdcshield-ci-triage/agent.env`。
- 每个 commit `git commit -s`（尾行 `Signed-off-by: wangxu <wangxumarshall@qq.com>`）；提交前 `git branch --show-current` 确认在特性分支。

## Review Focus

1. **第 5 bot 缺位**（认证失效/配额/cron 未配）→ 硬门槛下 wait 超时逐 bot 报告缺失者；ci_triage 车道 B 在 reap 后主动触发本地审查避免 fix/ci PR 永久卡死。
2. **人类评论污染**：wangxumarshall 的普通评论（无标记）不得被计入第 5 bot 信号——双条件判据测试锚定。
3. **同 head 重复审查**：事件线 + 时间线撞车只审一次——幂等去重测试锚定。
4. **闭环漏答**：待处置意见数 != 已答复数时闭环报错不结案——完整性校验测试锚定。
5. **对抗性检视空泛**：prompt 输出必须 file:line + 证据，拒绝「LGTM/可能有问题」——prompt 契约 + 解析测试锚定。

---

### Task 1: pr_local_review.py 骨架 + 对抗性 prompt 引擎

**Files:**
- Create: `scripts/pr_local_review.py`
- Test: `tools/excite/tests/test_pr_local_review.py`

**Interfaces:**
- Produces:
  - `run_review(pr_number: int, head_sha: str, force: bool = False) -> list[dict]`：拉 diff + 上下文 → 跑 claude → 返回结构化 findings（每条 `{path, line, severity, category, message, evidence}`）。`force=True` 跳过幂等检查。
  - `build_adversarial_prompt(diff: str, pr_context: dict) -> str`：返回对抗性检视 prompt 字符串。
  - `already_reviewed(pr_number: int, head_sha: str) -> bool`：同 head 幂等检查（查已发的 review body 标记 + head 锚定）。
  - `LOCAL_CLAUDE_MARKER = "<!-- local-claude-review -->"`

- [ ] **Step 1: 写失败测试** — `test_build_adversarial_prompt_has_five_elements`：对一段含 `memcmp` 的 diff 生成 prompt，断言含「pentester/攻击者」framing、「Skeptic/Architect/Minimalist」多镜头、「file:line + 证据」锚定要求、「意图/scope」镜、「先看删除再查新增」差分顺序；`test_prompt_contains_sdc_checks`：断言 prompt 含 AGENTS.md 的 SDC 激发关键词（`memcmp`/per-lane/`TEST_LOOP`/FP 可复现）。

- [ ] **Step 2: 运行验证失败** — `python3 -m pytest tools/excite/tests/test_pr_local_review.py -v` → FAIL（ModuleNotFoundError）。

- [ ] **Step 3: 实现** `build_adversarial_prompt(diff, pr_context)` + `LOCAL_CLAUDE_MARKER` 常量在 `scripts/pr_local_review.py`。prompt 结构：角色定位（对抗性渗透测试者）→ 多镜头 → 输出契约（JSON 数组，每条含 path/line/severity/category/message/evidence）→ 差分审查顺序 → SDC 8 项核查。diff 以防注入横幅包裹（diff 是不可信输入）。`run_review` 用**独立只读 worktree**（`git worktree add` 到临时目录读完整源码，结束后 `git worktree remove`）——不碰用户主工作区、不碰 autoci clone（设计文档 §9）。

- [ ] **Step 4: 运行验证通过** — `python3 -m pytest tools/excite/tests/test_pr_local_review.py -v` → PASS。

- [ ] **Step 5: Commit** — `git add scripts/pr_local_review.py tools/excite/tests/test_pr_local_review.py && git commit -s -m "tools(ci-triage): pr_local_review 对抗性检视 prompt 引擎骨架"`

### Task 2: 第 5 bot 判据 + 发布意见上 GitHub

**Files:**
- Modify: `scripts/pr_local_review.py`
- Modify: `scripts/pr_loop.py`（BOT_ORDER + 判据）
- Test: `tools/excite/tests/test_pr_local_review.py`、`tools/excite/tests/test_pr_loop.py`

**Interfaces:**
- Consumes: Task 1 的 `run_review`/`build_adversarial_prompt`/`LOCAL_CLAUDE_MARKER`。
- Produces:
  - `publish_review(pr_number: int, head_sha: str, findings: list[dict]) -> None`：把 findings 发布为「行级评论（挂 diff 行）+ review body（标记 + 挂不上 diff 行的 JSON 段）」，作者 wangxumarshall。
  - pr_loop.py 内 `LOCAL_CLAUDE_LOGIN = "wangxumarshall"`、`LOCAL_CLAUDE_MARKER`（从 pr_local_review import）、`BOT_ORDER` 五元组、`local_claude_comment(author, body) -> bool`（双条件）。

- [ ] **Step 1: 写失败测试** — `test_publish_review_posts_marker_and_inline`（fake gh：断言 review body 首行含 `<!-- local-claude-review -->`、行级评论挂到指定 path+line）；`test_local_claude_requires_author_and_marker`（pr_loop：wangxumarshall 无标记评论 → 不算第 5 bot；wangxumarshall 有标记 → 算）；`test_bot_order_has_five`（断言 BOT_ORDER == 五元组）。

- [ ] **Step 2: 运行验证失败** — 两个测试文件相关用例 → FAIL。

- [ ] **Step 3: 实现** `publish_review`（`gh api` 发 review，行级评论 `POST /repos/{o}/{r}/pulls/{pr}/reviews` 带 `comments` 数组）；pr_loop.py 的 BOT_ORDER 五元组 + `local_claude_comment`（在 reviews / review_comments / issue_comments 三载体判定，双条件，仿 `pragent_comment`）。

- [ ] **Step 4: 运行验证通过** — 两个测试文件全绿。

- [ ] **Step 5: Commit** — `tools(ci-triage): 第 5 bot 判据双条件 + 意见发布上 GitHub`

### Task 3: wait 内主动触发（事件线）+ 同 head 幂等

**Files:**
- Modify: `scripts/pr_loop.py`（wait 子命令）
- Test: `tools/excite/tests/test_pr_loop.py`

**Interfaces:**
- Consumes: Task 2 的 `BOT_ORDER`/`local_claude_comment`/`run_review`/`already_reviewed`。
- Produces:
  - `ensure_local_review(pr_number: int, head_sha: str) -> None`：若 `already_reviewed` 为假 → 调 `run_review` + `publish_review`。wait 进入四 bot 轮询前调用。

- [ ] **Step 1: 写失败测试** — `test_wait_triggers_local_review_once`（fake run_review：wait 调用一次 ensure_local_review；`already_reviewed` 为真时不再调用）；`test_ensure_local_review_idempotent_same_head`（同 head 二次调用不触发）。

- [ ] **Step 2: 运行验证失败** — pytest → FAIL。

- [ ] **Step 3: 实现** `ensure_local_review` + 在 wait 子命令入口（进入阻塞轮询前）调用。

- [ ] **Step 4: 运行验证通过** — pytest → PASS。

- [ ] **Step 5: Commit** — `tools(ci-triage): wait 内主动触发第 5 bot 审查（事件线）`

### Task 4: 意见计数 + 闭环逐条答复

**Files:**
- Modify: `scripts/pr_loop.py`
- Test: `tools/excite/tests/test_pr_loop.py`

**Interfaces:**
- Consumes: Task 2/3 的判据与触发。
- Produces:
  - `count_local_claude_findings(review) -> int`：解析 review body JSON 段 + 行级评论数。
  - `reply_to_findings(pr_number: int, findings: list[dict]) -> None`：行级 finding 逐条 `gh api` reply；body 段 finding 发一条 issue comment 逐条列出答复；返回后核对「待处置 == 已答复」。

- [ ] **Step 1: 写失败测试** — `test_count_local_claude_findings_parses_body_json`（body JSON 段 3 条 + 2 行级 → 计数 5）；`test_reply_all_findings_no_miss`（fake gh：5 条意见 → 5 条答复；完整性校验：漏 1 条 → 抛错/非零退出）；`test_reply_accept_has_sha_reject_has_evidence`（答复正文：接纳含 commit SHA、驳回含证据字段）。

- [ ] **Step 2: 运行验证失败** — pytest → FAIL。

- [ ] **Step 3: 实现** `count_local_claude_findings` + `reply_to_findings`，接入 pr-loop 阶段 4 处置后的闭环段（对 5 bot 意见统一逐条答复）。

- [ ] **Step 4: 运行验证通过** — pytest → PASS。

- [ ] **Step 5: Commit** — `tools(ci-triage): 第 5 bot 意见计数 + 闭环逐条答复`

### Task 5: ci_triage 车道 B 主动触发 + cron 时间线

**Files:**
- Modify: `scripts/ci_triage.py`
- Test: `tools/excite/tests/test_ci_triage.py`

**Interfaces:**
- Consumes: Task 3 的 `ensure_local_review`（从 pr_loop/pr_local_review import）。
- Produces:
  - ci_triage 车道 B 在 reap 后、派发修复 agent 前调用 `ensure_local_review`（硬门槛死锁规避）。

- [ ] **Step 1: 写失败测试** — `test_lane_b_triggers_local_review_before_dispatch`（fake ensure_local_review：车道 B 对 PR_OPEN 条目派发前调用一次）。

- [ ] **Step 2: 运行验证失败** — pytest → FAIL。

- [ ] **Step 3: 实现** 车道 B 触发点 + cron 时间线文档（`docs/build-deploy-pr-ci/ci-triage.md` 增 cron 行 `*/30 * * * * python3 scripts/pr_local_review.py --all-open`）。

- [ ] **Step 4: 运行验证通过** — pytest → PASS。

- [ ] **Step 5: Commit** — `tools(ci-triage): 车道 B 主动触发第 5 bot + cron 时间线`

### Task 6: 文档同步（五 bot 流程 + 判据分裂说明）

**Files:**
- Modify: `docs/build-deploy-pr-ci/pr-review-bots.md`
- Modify: `docs/build-deploy-pr-ci/pr-loop.md`
- Modify: `CLAUDE.md`、`AGENTS.md`

**Interfaces:**
- Consumes: 全部前序任务。

- [ ] **Step 1: 更新 pr-review-bots.md** — 新增第 5 bot 章节（判据 = wangxumarshall + 标记、两条触发线、对抗性方法论 5 点、四/五 bot 判据分裂说明——bot-coverage.yml 保持四 bot）。

- [ ] **Step 2: 更新 pr-loop.md** — 六阶段流程「四 bot」→「五 bot」，阶段 2/3/4 补第 5 bot 触发与闭环逐条答复。

- [ ] **Step 3: 更新 CLAUDE.md / AGENTS.md** — pr-loop 条目「four bots」→「five bots（含本地 claude 对抗检视）」。

- [ ] **Step 4: Commit** — `docs(ci-triage): 五 bot 流程文档同步`

---

## 验收（全链路，实现后真实验证）

- V1 第 5 bot 对真实 PR 发 review，body 含标记、作者 wangxumarshall，GitHub 可见。
- V2 `status` 显示第 5 bot 完成度；`wait` 等齐五 bot。
- V3 cron 对无信号 open PR 补发；同 head 复跑不重复。
- V4 agent 处置后 5 bot 每条意见都有逐条 reply，接纳带 SHA、驳回带实证，无漏答。
- V5 对抗性检视对已知缺陷样本给出 file:line + 证据级 finding。
