# Checklist

## 脚本（scripts/pr_loop.py）
- [x] 文件存在；`python3 -m py_compile` 通过；`status`/`wait`/`collect` 三子命令 `--help` 正常
- [x] 四 bot 判据与 `bot-coverage.yml` 逐字一致：`chatgpt-codex-connector[bot]` / `coderabbitai[bot]` 或 `CodeRabbit` check / `pr-agent` check / `greptile-apps[bot]`，且注释标明同步维护关系
- [x] `status`：无 open PR 时诚实输出并退出 0；`gh` 未认证/PR 不存在时明确报错非 0（无假通过）
- [x] 状态机四态输出正确：WAITING_BOTS / QUIET_WINDOW / READY_TO_FIX / READY_TO_MERGE，draft/fork 如实标注
- [x] `wait`：四 bot 完成 + 过静默窗退出 0（输出时间线）；超时退出非 0 且逐 bot 报告缺失信号者
  - 实测：超时路径（900s 到期 exit 1，逐 bot 列完成信号并明示「仅静默窗未过」）与完成路径（复跑 exit 0，时间线 + READY_TO_FIX）双路径均真实验证
- [x] `collect`：输出头部含防注入声明；四类意见源齐全（reviews/行级评论/issue 评论/check runs+annotations）；早于当前 head commit 时间的意见标注「历史轮次」
- [x] 仅用 Python3 标准库 + `gh` 子进程；GitHub 凭据全权交给 gh，脚本不管理 token（仓库自闭环、无第三方依赖）

## 双端命令
- [x] `.claude/commands/pr-loop.md` 与 `.trae/rules/pr-loop.md` 存在且内容一致（六阶段、CLI 用法、提示词、停止条件）
- [x] 内嵌用户给定 subagent 修复提示词原文（「访问github，对每个open的pr分别使用subagent……最终形成解决方案报告，记录问题分析过程、解决方案细节、实施结果及经验总结。」）
- [x] 明确「绝不自动 merge / 不自动 resolve review threads」与建议轮次上限（4 轮转人工）

## 文档
- [x] `docs/misc/pr-loop.md` 六阶段描述与脚本实际行为 100% 一致；含 CLI 参考、双端差异、前置自检（gh auth status / PR_TOKEN / 四 bot）、zip 取舍说明；与 `docs/misc/pr-review-bots.md` 互链
- [x] `CLAUDE.md` 文档地图含 pr-loop 条目（README 是否加链接以实施时实际章节为准，如实记录）——README「延伸」章节已加链接

## 仓库规则合规
- [x] 未触碰 `tests/`、`framework/`、`meson.build` 等构建源码；`ninja -C builddir` 无新目标、零新警告（实测 `ninja: no work to do.`）
- [x] x86-64 非回归：纯新增脚本/文档/配置，无既有源码修改（vacuous，如实声明）
- [x] 提交：`git commit -s`、尾行 `Signed-off-by: wangxumarshall <wangxumarshall@qq.com>` 其后无内容、提交前 `git branch --show-current` 确认特性分支、绝不推 main

## 端到端 dogfooding（本特性 PR 自身走全流程）
- [x] push 后 `auto-pr.yml` 自动创建 PR（验证 PR_TOKEN 链路真实有效）——实测 push feat/pr-loop-tooling 后 50s 内 PR #243 自动创建（非 draft、OPEN、base main）
- [x] `wait` 真实等到四 bot 完成（引用真实输出，非推测）——codex 11:49:30 / coderabbit 11:49:55 / pr-agent 11:55:25 / greptile 11:56:46 UTC，12:11:48 过静默窗 exit 0
- [ ] `collect` 导出 → subagent 批判性处置 → 修复 → 验证（构建/回归真实输出）→ push，至少一轮完整循环
- [ ] `status` 最终报 READY_TO_MERGE（或 bot 无意见时验证 CLEAN 判定路径）→ 已向用户发出人工 merge 提醒（含 PR 链接）
- [ ] trae 会话内全流程走通；claude code cli 侧 `/pr-loop` 命令可发现、`scripts/pr_loop.py` 可独立执行、手册指引一致
