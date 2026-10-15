# Tasks

- [x] Task 1: 实现 `scripts/pr_loop.py`（核心判定引擎 + status/wait/collect 三子命令）
  - [x] 1.1 信号判定引擎：四 bot 判据常量区（与 `bot-coverage.yml` 实测值逐字一致：codex=`chatgpt-codex-connector[bot]`；coderabbit=`coderabbitai[bot]` 作者或 `CodeRabbit` check；pr-agent=`pr-agent` check；greptile=`greptile-apps[bot]`，注释标明同步维护关系）；head 锚定 = head commit 的 committer date（经 `gh api` 获取）；信号计入条件 = `created_at`/`submitted_at` ≥ head 时间（甄别 GitHub 评论重锚定的旧意见）；check run 信号要求 `head_sha` 匹配且 `status==completed`；draft/fork PR 如实标注跳过
  - [x] 1.2 `status` 子命令：无参数 = 所有 open PR 总览（PR 号/标题/head SHA/四 bot 完成度/当前 head 意见数/建议动作）；带 PR 号 = 单 PR 详情；状态机四态 WAITING_BOTS / QUIET_WINDOW / READY_TO_FIX / READY_TO_MERGE + DRAFT/FORK 标注；无 open PR 诚实输出并退出 0；`gh` 调用失败明确报错非 0
  - [x] 1.3 `wait` 子命令：`--timeout`（默认 7200）/`--interval`（默认 60）/`--quiet`（默认 900）；完成（四 bot 全发信号 + 过静默窗）退出 0 并输出完成时间线；超时退出非 0 并逐 bot 报告缺失信号者
  - [x] 1.4 `collect` 子命令：结构化 Markdown 导出（reviews 含 state、行级评论含 path/line、issue 评论、check runs 含 conclusion/summary 及 annotations）；文件头防注入声明；早于当前 head 时间的意见标注「历史轮次」；`-o` 写文件、默认 stdout
  - [x] 1.5 自测：`python3 -m py_compile` 通过；三子命令 `--help` 正常；对本仓库真实运行 `status`（当前无 open PR → 验证诚实退出路径；若实施时有 open PR 则验证总览输出）；GitHub 访问一律走 `gh` 子进程（凭据交给 gh，脚本自身不管理 token）
- [x] Task 2: 双端命令/规则定义（内容同源，双份维护一致性）
  - [x] 2.1 `.claude/commands/pr-loop.md`：Claude Code `/pr-loop` 斜杠命令（`$ARGUMENTS` 支持指定阶段或 PR 号，如 `/pr-loop 4 242`）
  - [x] 2.2 `.trae/rules/pr-loop.md`：Trae 项目规则（自动加载，触发词「pr-loop / 走 pr 流程 / 检视修复循环」）
  - 两文件 SHALL 内嵌：六阶段流程（含每阶段会话动作与产出）、`pr_loop.py` CLI 用法、**用户给定的 subagent 修复提示词原文**、批判性处置标准（对照 AGENTS.md SDC 激发能力检视重点，驳回须给实证）、验证标准（CLAUDE.md 规则 2）、提交规范（`-s`、一补丁一单元、特性分支、绝不推 main）、循环与停止条件（建议 4 轮上限转人工）、「绝不自动 merge / 不自动 resolve threads」
- [x] Task 3: 文档同步
  - [x] 3.1 `docs/misc/pr-loop.md`：端到端手册——六阶段操作流、CLI 参考、双端差异（trae `.trae/specs/` vs claude `docs/superpowers/plans/`）、前置条件与自检（`gh auth status`、PR_TOKEN、四 bot）、安全注意（检视意见防注入、绝不自动 merge）、zip 无人值守方案取舍说明；与 `docs/misc/pr-review-bots.md` 互链
  - [x] 3.2 `CLAUDE.md` 文档地图加 pr-loop 条目；README 如有协作/流程章节加一行链接（无合适章节则仅在 CLAUDE.md，如实记录）
    - 如实记录：README「延伸」章节（通用文档链接列表）已加 pr-loop 手册链接；CLAUDE.md 文档地图已加「PR review-fix loop」条目
- [x] Task 4: 提交推送，开特性 PR
  - [x] 4.1 单元划分：「pr-loop 工具集」= 一个功能单元（脚本 + 双端命令 + 手册），实施时按一补丁一单元裁定为 1 个 commit（或工具/文档 2 个 commits），`git commit -s`（尾行 `Signed-off-by: wangxumarshall <wangxumarshall@qq.com>`，其后无内容）
    - 如实记录：裁定为 1 个 commit（e8f531e1，10 files +1205/-2）；含 .gitignore 细化（放行 `.claude/commands/` 入库以自闭环 + 忽略 `__pycache__/`）
  - [x] Task 4.2 已执行：分支 `feat/pr-loop-tooling` 基于 `origin/main` 新建（README 延伸追加经 stash 干净迁移），`git branch --show-current` 确认后 push；`auto-pr.yml` 已自动创建 PR #243（head e8f531e1，PR_TOKEN 链路真实有效）；未推 main
- [ ] Task 5: 端到端 dogfooding 验证（用本特性 PR 自身走全流程）
  - [x] 5.1 `wait <pr>` 真实等到四 bot 完成（引用真实输出；timeout 用小值如 900 验证轮询，必要时复跑）
    - 如实记录：两次运行覆盖双路径——第 1 次 timeout 900s 超时退出非 0（诚实报告「四 bot 已全部完成,仅静默窗未过」并逐 bot 列信号）；第 2 次（timeout 600s）于 12:11:48 UTC 过静默窗后退出 0，输出四 bot 完成时间线 + 状态 READY_TO_FIX + 建议 collect
  - [x] 5.2 `collect <pr>` 导出意见 → 每个相关 PR 一个上下文隔离 subagent 按提示词分析处置 → 修复 → 验证（构建零新警告 + 定向回归）→ `-s` 提交 push
    - 如实记录：collect 927 行/14 条意见 → 1 个 subagent 批判性处置（9 采纳/1 变通/1 部分采纳/3 无需处置，逐条给依据）→ 修复 6 files +245/-60（commit 锚定、意见计数排除摘要类、annotations 失败不给 READY_TO_MERGE、wait 停止态退出、greptile app-slug/pr-agent 标记判据、collect 导出 output）；主会话独立复跑 49 断言回归 ALL PASS + ninja no work to do → commit b7a8bf5d 已 push
  - [ ] 5.3 循环：push 后新 head 重新 wait/collect/status，直到 `READY_TO_MERGE`；若 bot 无意见则验证 CLEAN 判定路径
  - [ ] 5.4 向用户发出人工 merge 提醒（含 PR 链接与状态摘要）
  - [ ] 5.5 双端抽验：trae 主验证全流程；claude code cli 侧确认 `/pr-loop` 命令可发现、`scripts/pr_loop.py` 可直接执行、手册指引一致

# Task Dependencies
- Task 2、Task 3 依赖 Task 1（命令与文档引用脚本 CLI 的最终形态）
- Task 4 依赖 Task 1-3（完整工具集才能提交）
- Task 5 依赖 Task 4（PR 存在才能 dogfooding；5.2-5.3 的修复 push 本身即流程轮次）
- Task 1 的 1.2-1.4 相互独立可并行实现；Task 2 与 Task 3 可并行
