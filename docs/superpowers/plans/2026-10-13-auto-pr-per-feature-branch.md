# 特性分支自动开 PR（每分支一个 PR 且触发检视机器人）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 新增 `.github/workflows/auto-pr.yml`：向非 `main` 的特性分支 push 时，若该分支尚未有 open PR，则自动开一个 `-> main` 的 PR；同一分支后续 push 由 GitHub 复用该 PR（`pull_request: synchronize`），实现「一个特性分支 = 一个 PR」。

**Architecture:** 单 workflow，`on: push: branches-ignore: [main, gh-pages]`。用 `gh pr create`（先 `gh pr list --head` 判重）。**关键约束**：用仓库 Secret `PR_TOKEN`（fine-grained PAT / GitHub App token）而非默认 `GITHUB_TOKEN` 建 PR——`GITHUB_TOKEN` 创建的 PR **不会触发 `pull_request` 事件**（GitHub 防工作流自触发死循环的刻意设计，例外仅 `workflow_dispatch`/`repository_dispatch`），会导致 `pr-agent.yml`/`bot-coverage.yml` 不评审；PAT 发起的 PR 视为「人」创建，正常触发。`PR_TOKEN` 未配置时优雅跳过（`::warning` + exit 0）。

**Tech Stack:** GitHub Actions（ubuntu-latest 自带 `gh`）+ 本地 `actionlint`（`/tmp/actionlint -shellcheck= -pyflakes= <file>`）。

**Spec:** 本计划内嵌。设计依据为本会话实测：
- `main` 当前**未被**保护（`gh api .../branches/main/protection` → 404；`.../rulesets` → `[]`）；「不推 main」是 CLAUDE.md 规则 3 的**约定**而非平台强制。
- 仓库 Actions 权限现状：`{"default_workflow_permissions":"read","can_approve_pull_request_reviews":false}` → 默认 `GITHUB_TOKEN` 连 PR 都建不了；采用 PAT 后该设置无需改动。
- GitHub 天然保证「同一 head 分支 → 同一 base 只有 1 个 open PR」（现存实例 #235 `chore/pr-review-bots-sdc -> main`、#234 `jules-... -> main`）。
- `benchmark-trend` job 用 `benchmark-action auto-push: true` 推 `gh-pages` 分支 → 必须排除，否则会被误开 `gh-pages -> main` 的 PR。

## Global Constraints

- **一补丁一单元**：本单元 = 「自动开 PR」这一 CI 能力 + 其文档 + 本计划文件，单一 commit。
- 提交：`git commit -s`，尾行 `Signed-off-by: wangxumarshall <wangxumarshall@qq.com>`，其后无内容、无 Co-Authored-By；提交前 `git branch --show-current` 确认。
- 落在既有特性分支 `chore/pr-review-bots-sdc`（其 PR #235 尚未合并；`docs/misc/pr-review-bots.md` 仅存在于该分支，本单元需在同一 PR 内追加 commit，符合规则 1「一单元一 commit」+ 规则 3「每个 commit 推到特性分支」）；**绝不推 main**。
- **不改**任何 `tests/`、`framework/`、构建源码；x86-64 non-regression 为 vacuous（如实声明）。
- workflow 内注释用中文（与 `pr-agent.yml` 一致）；YAML 校验走 `actionlint`。
- **诚实性**：`PR_TOKEN` 未配置 → 明确 `::warning` 跳过，绝不假报「已开 PR」。

## Review Focus

（最可能咬人的输入 → 钉住它的处理）

1. **已有 open PR 的分支再次 push**（如本仓 `chore/pr-review-bots-sdc`）→ 必须**跳过**、不重复建 PR → 由 `gh pr list --head` 判重。
2. **`gh-pages` 自动推送**（benchmark-trend）→ 不得开 PR → 由 `branches-ignore` 排除。
3. **分支相对 main 无新提交**（或 PR 已存在）→ `gh pr create` 会非 0 退出 → 不得让 job 变红 → `if` 包裹 + `::notice` 收场。
4. **`PR_TOKEN` 缺失**（首次使用者）→ 不得红、不得假通过 → `::warning` + exit 0。
5. **四 bot 触发**：PAT 建的 PR 必须能触发 `pr-agent.yml`/`bot-coverage.yml`（这是选 PAT 的**唯一理由**）→ 端到端实测。

---

### Task 1: 新增 workflow 文件

**Files:**
- Create: `/home/sdc/wangxu/sdcshield/.github/workflows/auto-pr.yml`（完整内容见下，执行时原样落盘）

**Interfaces:**
- Consumes: 仓库 Secret `PR_TOKEN`（用户配置）；`gh` CLI（runner 预装）。
- Produces: 对每次「非 main / 非 gh-pages 分支 push」执行判重 +（按需）建 PR；`::warning`（无 token）/`::notice`（无可建 PR）/ 成功三态输出。

- [ ] **Step 1: 落盘 workflow**

```yaml
# .github/workflows/auto-pr.yml
# 特性分支(非 main)首次推送后自动开一个 PR;同一分支后续 push 由 GitHub 复用
# 该 PR(pull_request: synchronize),从而「一个特性分支 = 一个 PR」。
#
# 为何用 PAT 而非默认 GITHUB_TOKEN 建 PR:
#   GITHUB_TOKEN 创建的 PR **不会**触发 pull_request 事件(GitHub 为防工作流
#   自触发死循环的刻意设计),会导致 pr-agent.yml / bot-coverage.yml 不在 opened
#   时评审;PAT 发起的 PR 视为「人」创建,可正常触发下游检视机器人。
#
# 前置:仓库 Secret `PR_TOKEN` = 一个有 contents:read + pull_requests:write 的
#   fine-grained PAT(或 GitHub App token)。未配置时本 workflow 优雅跳过。
name: Auto PR

on:
  push:
    # main 不建 PR;gh-pages 是 benchmark-trend 自动推送的趋势分支,不建 PR
    branches-ignore: [main, gh-pages]

permissions:
  contents: read            # 建 PR 走 PAT,GITHUB_TOKEN 保持最小只读

concurrency:
  group: auto-pr-${{ github.ref }}
  cancel-in-progress: false

jobs:
  auto-pr:
    runs-on: ubuntu-latest
    steps:
      - name: Checkout (for gh pr create branch context)
        uses: actions/checkout@v7
        with:
          fetch-depth: 0            # gh pr create 需比较分支与 base 的提交差异
          persist-credentials: false
      - name: Create PR for this branch if none open
        env:
          GH_TOKEN: ${{ secrets.PR_TOKEN }}
        run: |
          set -euo pipefail
          if [ -z "${GH_TOKEN:-}" ]; then
            echo "::warning::PR_TOKEN 未配置 → 跳过自动开 PR(可手动 gh pr create)"
            exit 0
          fi
          b="${GITHUB_REF_NAME}"
          # 已有 open PR → 什么都不做(后续 push 自动 synchronize 到同一 PR)
          if gh pr list --repo "$GITHUB_REPOSITORY" --head "$b" --state open \
               --json number --jq '.[0].number' | grep -q .; then
            echo "PR 已存在($b) → push 会自动更新同一 PR,跳过"
            exit 0
          fi
          # 分支相对 main 无新提交(或已存在)时 gh pr create 非 0;以 notice 优雅收场
          if gh pr create --repo "$GITHUB_REPOSITORY" --base main --head "$b" \
               --title "WIP: $b" \
               --body "自动为特性分支 \`$b\` 创建。后续 push 会更新本 PR 并触发检视机器人。"; then
            echo "已自动创建 PR($b → main)"
          else
            echo "::notice::未创建 PR(分支相对 main 可能无新提交,或已存在)"
          fi
```

- [ ] **Step 2: actionlint 零报错**

Run: `/tmp/actionlint -shellcheck= -pyflakes= .github/workflows/auto-pr.yml`
Expected: 无输出、退出码 0。

### Task 2: 文档同步

**Files:**
- Modify: `docs/misc/pr-review-bots.md`（在 §7 之后新增 §7.2「自动开 PR：一个特性分支一个 PR」）

**Interfaces:**
- Consumes: Task 1 的最终行为与 `PR_TOKEN` 前置。
- Produces: 文档描述与 workflow 行为 100% 一致（CLAUDE.md 规则 7）。

- [ ] **Step 1: 新增 §7.2**，内容要点（措辞以现场为准，事实不变）：
  - GitHub 天然保证「一 head 分支 → 一 base 只有 1 个 open PR」；本 workflow 只补「首次 push 免手动点建」。
  - `branches-ignore: [main, gh-pages]` 的原因。
  - 为什么必须用 `PR_TOKEN`（`GITHUB_TOKEN` 建的 PR 不触发 `pull_request` → 四 bot 不评审）。
  - `PR_TOKEN` 配置步骤：fine-grained PAT（Contents: read + Pull requests: write，仅限本仓）→ 仓库 Secret `PR_TOKEN`；未配置时 workflow `::warning` 跳过。
  - 备选：GitHub App token（`actions/create-github-app-token`）更利于长期/团队。

### Task 3: 验证 + 提交 + 推送

**Files:**
- Commit: `.github/workflows/auto-pr.yml` + `docs/misc/pr-review-bots.md` + 本计划文件
- Ship: 追加 commit 到既有特性分支 `chore/pr-review-bots-sdc`（更新 PR #235）→ GitHub

**Interfaces:**
- Consumes: Task 1-2 全部产物。
- Produces: 追加 commit 到 PR #235；合入 main 后，未来每个特性分支首次 push 自动建 PR。

- [ ] **Step 1: 确认留在既有特性分支**

```bash
git branch --show-current   # 必须输出 chore/pr-review-bots-sdc（不要切分支/新建分支）
```

- [ ] **Step 2: 提交（先确认分支）**

```bash
git add .github/workflows/auto-pr.yml docs/misc/pr-review-bots.md \
        docs/superpowers/plans/2026-10-13-auto-pr-per-feature-branch.md
git commit -s -m "ci: 特性分支自动开 PR（每分支一个 PR，PAT 建 PR 以触发检视机器人）"
git log -1 --format=%B       # 尾行 Signed-off-by，其后无内容
```

- [ ] **Step 3: 推送并观察**

```bash
git push origin chore/pr-review-bots-sdc
```

Expected（本次 push 即自测）：workflow 触发；`PR_TOKEN` 未配置时日志出现
`::warning::PR_TOKEN 未配置 → 跳过自动开 PR`，job 绿。用户配置 `PR_TOKEN` 后，
再 push（或手动 `workflow_dispatch` 不适用，因仅 `push` 触发）任意新特性分支 →
应自动出现 PR，且 `pr-agent.yml`/`bot-coverage.yml` 在该 PR 上被触发（验证 Review
Focus #5）。

---

## 用户需执行的前置（非仓库代码）

1. 建 fine-grained PAT：`Settings → Developer settings → Personal access tokens →
   Fine-grained tokens`，Repository access 选 `wangxumarshall/sdcshield`，权限
   `Contents: Read`、`Pull requests: Read and write`；生成后复制。
2. 仓库 `Settings → Secrets and variables → Actions → New repository secret`，
   名 `PR_TOKEN`，值粘贴 PAT。
3. （可选，减小暴露面）设置 PAT 过期时间，到期前轮换。

## 被拒备选（记录供追溯）

1. **用默认 `GITHUB_TOKEN` 建 PR + 改仓库 Workflow permissions**：可行但
   **四 bot 不会在 `opened` 时评审**（GITHUB_TOKEN 不触发 workflow），与本仓
   「SDC 检视机器人」目标冲突 → 弃。
2. **建 PR 后显式 `gh workflow run` 踢机器人**：需给 `pr-agent.yml` 加
   `workflow_dispatch` 并传 PR 号，侵入既有评审 workflow 且逻辑绕 → 弃。
3. **不加 workflow，保持手动一键**：GitHub 已保证一分支一 PR、后续 push 全自动，
   仅欠首次一次点击；本计划提供的增量价值只在「免点击」→ 用户已明确选择自动化。
