# PR 代码检视机器人（聚焦 SDC 激发）

本文件汇总 SDCShield 仓库接入的四个 PR 代码检视机器人（codex / CodeRabbit /
PR-Agent / Greptile）、各自的配置文件与触发方式、机器人生效的核查方法，以及四份
配置共同内嵌的「SDC 激发专属检视要点」。所有账号/标识与配置路径均以仓库实际状态
为准；**Greptile 当前尚未安装到本仓库**，详见第 3 节与第 7 节。

## 1. 概述

四个检视机器人及其在 PR 上使用的账号/标识、触发载体与配置文件：

| 机器人 | PR 上的账号 / 标识 | 触发载体 | 仓库配置文件 | 读取内容 |
| --- | --- | --- | --- | --- |
| codex | 作者 login `chatgpt-codex-connector[bot]` | GitHub App（PR 事件） | `AGENTS.md`（仓库根） | 仓库级上下文 + SDC 激发检视指令 |
| CodeRabbit | 作者 login `coderabbitai[bot]`；另产出 check-run `CodeRabbit` | GitHub App（PR 事件） | `.coderabbit.yaml`（仓库根） | `language: zh-CN`、`reviews.profile: assertive`、`reviews.path_instructions`、`chat.auto_reply` 等 |
| PR-Agent | check-run `pr-agent` | workflow `.github/workflows/pr-agent.yml` | `.pr_agent.toml`（仓库根） | `[config].response_language`、`[pr_reviewer].extra_instructions` 等 |
| Greptile | 作者 login `greptile-apps[bot]`（**待安装**） | GitHub App（PR 事件） | `.greptile/config.json` + `.greptile/rules.md` | 结构化 rules 与人读版检视上下文 |

**触发方式**：当 PR 处于 opened / reopened / synchronize 事件时自动触发检视。
codex、CodeRabbit、Greptile 由各自的 GitHub App 在 PR 事件上触发（Greptile 需先由
管理员安装后才会生效）；PR-Agent 由 `.github/workflows/pr-agent.yml` 触发——该
workflow 位于 `main` 分支，故仅对以 `main` 为基的 PR 生效，且 fork PR 因无法读取
仓库 Secret 会自动跳过。此外，`.github/workflows/bot-coverage.yml` 对本次 PR 做
**非阻塞**的四机器人覆盖度校验（见第 6 节）。

## 2. 机器人生效核查

需已通过 `gh` 认证（例如 `gh auth login`，或导出 `GH_TOKEN="$GITHUB_PERSONAL_ACCESS_TOKEN"`）。

可复现的核查命令（把 `<PR>` 换成目标 PR 编号）：

```bash
# 1) 查看 PR 上全部 check-run（含 CodeRabbit / pr-agent 等 check-run 名称）
gh pr checks <PR>

# 2) 列出 PR 上 issue 评论的作者 login（codex / CodeRabbit / Greptile 会在此出现）
gh api repos/wangxumarshall/sdcshield/issues/<PR>/comments --jq '.[].user.login'

# 3) 列出 PR 上 review 的作者 login
gh api repos/wangxumarshall/sdcshield/pulls/<PR>/reviews --jq '.[].user.login'

# 4) 查看 PR-Agent 的 workflow 运行记录（workflow 名为 "PR Agent Review"）
gh run list --workflow "PR Agent Review"
```

**账号 / 标识对照（用于判定各机器人是否已检视）**：

| 机器人 | 匹配判据 |
| --- | --- |
| codex | 作者 login 命中 `chatgpt-codex-connector[bot]`（issue 评论 / review / review 评论） |
| CodeRabbit | 作者 login 命中 `coderabbitai[bot]`，或 check-run 名称命中 `CodeRabbit` |
| PR-Agent | check-run 名称命中 `pr-agent` |
| Greptile | 作者 login 命中 `greptile-apps[bot]`（安装生效后） |

## 3. Greptile 手动安装步骤

Greptile 以 GitHub App（账号 `greptile-apps[bot]`）形式工作，**只能由仓库管理员在
greptile.com 网页授权安装**到本仓库，**无法通过代码或 CLI 完成**。在管理员完成
授权前，`.greptile/` 下的配置不会被识别，PR 上也不会出现 `greptile-apps[bot]` 的评论。

安装步骤：

1. 由仓库管理员登录 [greptile.com](https://greptile.com)，授权安装 Greptile GitHub App。
2. 在授权范围内只勾选本仓库 `wangxumarshall/sdcshield`（或按组织策略选择）。
3. 安装完成后，打开一个测试 PR（或对现有 PR 推送新 commit）触发检视。
4. 确认 Greptile 已识别仓库根 `.greptile/config.json`（结构化 rules）与
   `.greptile/rules.md`（人读版上下文）。
5. 确认该 PR 上出现作者为 `greptile-apps[bot]` 的检视评论。

## 4. Codex 使能复核项

- 在 Codex / Codex code review 设置中，确认本仓库 `wangxumarshall/sdcshield` 已**使能**，
  即已在对应集成中完成授权与仓库选择。
- codex 会读取**仓库根**的 [`AGENTS.md`](../../AGENTS.md) 作为仓库上下文；该文件已内嵌
  SDC 激发检视指令，使能后即可按其中的 6 条要点检视本仓库 PR。
- 复核方式：见第 2 节的 `gh` 命令——若 issue 评论 / review 中出现作者
  `chatgpt-codex-connector[bot]`，即表示 codex 已对本 PR 生效。

## 5. 检视聚焦 SDC 激发能力

SDC 检视的独特之处在于：不仅看代码「对不对」，更看它**是否真的能把计算单元逼出
错误比特**。四份配置文件（`AGENTS.md`、`.coderabbit.yaml`、`.pr_agent.toml`、
`.greptile/rules.md`）均围绕下列 6 条要点组织检视指令：

1. **激发强度与覆盖**：是否真正压到目标计算单元（FMA / SIMD / 压缩 / SVD / crypto）、
   覆盖目标数据宽度（如 128/256/512-bit 向量、FP16/BF16/FP32/FP64）与数据模式
   （随机、全 0/全 1、边界值、NaN/Inf、极值）；`TEST_LOOP` 的定时压力是否充足，
   能否在给定时长内持续打满目标单元。
2. **golden 比对**：是否使用字节级 `memcmp`（`memcmp_or_fail`）而非近似/容差比较；
   浮点是否逐位可复现（SNaN/NaN 位型跨架构一致，依赖框架在 `sandstone_data.cpp`
   统一 quiet SNaN）。
3. **正确性基线（x86-64 非回归）**：是否以 `#elif __aarch64__` / 按架构 meson 守卫的
   方式**增量**添加，未破坏 x86-64 参考路径；是否**不当扩大** `#ifdef __x86_64__`
   （仅当代码真正跨架构共享时才允许）。
4. **诚实性**：ARM64 未实现项是否从 `test_init` 返回 `EXIT_SKIP`、且理由为
   `"to be implemented (placeholder): <缺失内容>"`，禁止以 `EXIT_SUCCESS` 假通过的
   no-op 测试。
5. **架构相关缺陷（SVE/NEON VL）**：是否存在与向量长度（VL）相关的越界读写或误报；
   注意 CI runner 的 VL=128 会**掩盖** 512-bit 等高 VL 下才暴露的缺陷。
6. **失败现场留存**：失败路径是否保留冷路径的 bit dump、sum-bit、failing-vector
   记录，以免过早返回而丢弃足以定位 SDC 的现场信息。

**四份配置文件各自的内嵌方式**：

- [`AGENTS.md`](../../AGENTS.md)：以「代码检视重点：SDC 激发能力」小节列出上述 6 条
  要点，作为 codex 的仓库级上下文与检视指令。
- [`.coderabbit.yaml`](../../.coderabbit.yaml)：通过 `reviews.path_instructions` 按路径
  （`tests/cpu/**`、`framework/**`、`device/**`）分别内嵌 6 条要点，并配以
  `language: zh-CN`、`reviews.profile: assertive`、`reviews.auto_review`
  （`enabled` + `auto_incremental_review`）、`chat.auto_reply`。
- [`.pr_agent.toml`](../../.pr_agent.toml)：在 `[pr_reviewer].extra_instructions` 中内嵌
  6 条要点与**降噪约束**（只聚焦 SDC 激发能力、正确性与架构守卫，不输出无关的风格/
  命名/格式意见），并设 `[config].response_language = "zh-CN"`。
- [`.greptile/rules.md`](../../.greptile/rules.md)：以人读版上下文呈现 6 条要点；
  与之配套的 [`.greptile/config.json`](../../.greptile/config.json) 以 `triggerOnUpdates: true`
  与 6 条结构化 rules（对应 `sdc-excitation-intensity-coverage`、
  `sdc-golden-bytewise-memcmp`、`x86-64-non-regression`、`arm64-placeholder-honesty`、
  `sve-neon-vector-length`、`failure-scene-recording`）落地同一套要点。

## 6. 覆盖度校验 workflow 用法

`.github/workflows/bot-coverage.yml` 用于核查四个机器人是否已对目标 PR 产出检视。

- **触发**：`pull_request` 的 opened / reopened / synchronize 事件，以及手动
  `workflow_dispatch`（输入 `pr-number`）。
- **行为**：有界轮询四个判据——codex（作者 `chatgpt-codex-connector[bot]`）、
  CodeRabbit（作者 `coderabbitai[bot]` 或 check-run `CodeRabbit`）、PR-Agent
  （check-run `pr-agent`）、Greptile（作者 `greptile-apps[bot]`），直到全部命中或到达
  等待上限。
- **输出位置**：结果表格写入 `$GITHUB_STEP_SUMMARY`（step summary），同时对每个缺失
  项以 `::warning::` 告警，并在 PR 上发布/更新一条带隐藏标记 `<!-- bot-coverage-report -->`
  的评论（已存在则更新，避免重复刷屏）。
- **可配 env**：等待上限 `BOT_COVERAGE_MAX_WAIT`（默认 `900` 秒）、轮询间隔
  `BOT_COVERAGE_INTERVAL`（默认 `30` 秒）。
- **非阻塞语义**：脚本**恒 `exit 0`**，即使存在缺失项也只告警、**绝不阻塞合并**。

手动触发示例：

```bash
gh workflow run bot-coverage.yml -f pr-number=<N>
```

## 7. 当前状态与待人工项

| 机器人 | 当前状态 | 备注 |
| --- | --- | --- |
| codex | 已生效（有 PR 产物证据） | 读取根 `AGENTS.md` |
| CodeRabbit | 已生效（有 PR 产物证据） | 读取根 `.coderabbit.yaml` |
| PR-Agent | 已生效（有 PR 产物证据） | workflow 位于 `main`；读取根 `.pr_agent.toml` |
| Greptile | **待人工安装** | 需管理员在 greptile.com 授权安装 GitHub App 后方生效 |

> 诚实说明：Greptile 的 `.greptile/` 配置已就位，但在管理员完成 Greptile GitHub App
> 安装授权之前，**不会**对本仓库 PR 产生任何检视；上表如实标注为「待人工安装」。

## 8. 延伸阅读

- [代码检视代理仓库上下文 `AGENTS.md`](../../AGENTS.md)
- [CodeRabbit 配置 `.coderabbit.yaml`](../../.coderabbit.yaml)
- [PR-Agent 配置 `.pr_agent.toml`](../../.pr_agent.toml)
- [Greptile 检视上下文 `.greptile/rules.md`](../../.greptile/rules.md)
- [仓库 README](../../README.md)
- [覆盖度校验 workflow `.github/workflows/bot-coverage.yml`](../../.github/workflows/bot-coverage.yml)
