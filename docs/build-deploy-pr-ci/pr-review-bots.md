# PR 代码检视机器人（聚焦 SDC 激发）

本文件汇总 SDCShield 仓库接入的四个外部 SaaS 检视机器人（codex / CodeRabbit /
PR-Agent / Greptile）与第 5 个本地检视机器人（本地无头 claude code，对抗性检视，
见第 8 节），各自的配置/引擎与触发方式、机器人生效的核查方法，以及四份配置共同
内嵌的「SDC 激发专属检视要点」。所有账号/标识与配置路径均以仓库实际状态为准；
Greptile 已于 2026-10-13 完成安装授权并实测生效（首次产出见 PR #238），详见第 3 节与第 7 节。

## 1. 概述

四个外部检视机器人及其在 PR 上使用的账号/标识、触发载体与配置文件（第 5 个本地
bot 见第 8 节）：

| 机器人 | PR 上的账号 / 标识 | 触发载体 | 仓库配置文件 | 读取内容 |
| --- | --- | --- | --- | --- |
| codex | 作者 login `chatgpt-codex-connector[bot]` | GitHub App（PR 事件） | `AGENTS.md`（仓库根） | 仓库级上下文 + SDC 激发检视指令 |
| CodeRabbit | 作者 login `coderabbitai[bot]`；另产出 check-run `CodeRabbit` | GitHub App（PR 事件） | `.coderabbit.yaml`（仓库根） | `language: zh-CN`、`reviews.profile: assertive`、`reviews.path_instructions`、`chat.auto_reply` 等 |
| PR-Agent | check-run `pr-agent` | workflow `.github/workflows/pr-agent.yml` | `.pr_agent.toml`（仓库根） | `[config].response_language`、`[pr_reviewer].extra_instructions` 等 |
| Greptile | 作者 login `greptile-apps[bot]`（**已生效**） | GitHub App（PR 事件） | `.greptile/config.json` + `.greptile/rules.md` | 结构化 rules 与人读版检视上下文 |

**触发方式**：当 PR 处于 opened / reopened / synchronize 事件时自动触发检视。
codex、CodeRabbit、Greptile 由各自的 GitHub App 在 PR 事件上触发；PR-Agent 由 `.github/workflows/pr-agent.yml` 触发——该
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
| Greptile | 作者 login 命中 `greptile-apps[bot]`（issue 评论 / review / review 评论） |

## 3. Greptile 安装记录

Greptile 以 GitHub App（账号 `greptile-apps[bot]`）形式工作，**只能由仓库管理员在
greptile.com 网页授权安装**到本仓库，**无法通过代码或 CLI 完成**。

**状态：已完成（2026-10-13）。** 管理员在 greptile.com 完成授权安装后，Greptile 已识别
仓库根 `.greptile/config.json`（结构化 rules）与 `.greptile/rules.md`（人读版上下文），
并在 PR #238 上首次实测产出检视：review（COMMENTED）+ 4 条行级 review 评论 +
check-run "Greptile Review" pass，bot-coverage 判据命中（汇总 4/4）。

安装步骤（已完成，供恢复 / 重装时参考）：

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
  SDC 激发检视指令，使能后即可按其中的 8 条要点检视本仓库 PR。
- 复核方式：见第 2 节的 `gh` 命令——若 issue 评论 / review 中出现作者
  `chatgpt-codex-connector[bot]`，即表示 codex 已对本 PR 生效。

## 5. 检视聚焦 SDC 激发能力

SDC 检视的独特之处在于：不仅看代码「对不对」，更看它**是否真的能把计算单元逼出
错误比特**。四份配置文件（`AGENTS.md`、`.coderabbit.yaml`、`.pr_agent.toml`、
`.greptile/rules.md`）均围绕下列 8 条要点组织检视指令：

1. **激发强度与覆盖**：是否真正压到目标计算单元（FMA / SIMD / 压缩 / SVD / crypto），
   靶点优先 FMA/乘法器（现场向量 SDC >75% 来自 FMA）；数据宽度覆盖目标位宽
   （128/256/512-bit 向量、FP16/BF16/FP32/FP64）；`TEST_LOOP` 定时持续压满目标单元
   （SDC 激发窗口概率 <10⁻⁵，需长时压测捕获）；指令序列长度达数十条级下限
   （数十条级短循环即可检出崩塌）。
2. **数据模式与缺陷激活**：随机 / 全 0 / 全 1 / 边界值 / NaN/Inf / 极值多模式覆盖；
   长时固定电平后翻转（BTI）、SDF 模式、双值域档（有界/无界）、FP64 指数/符号位
   敏感；标注所需的频率/电压条件空间。
3. **golden 比对**：字节级 `memcmp`（`memcmp_or_fail`）而非近似/容差比较（容差即
   盲区）；浮点逐位可复现（SNaN/NaN 位型跨架构一致，依赖框架在 `sandstone_data.cpp`
   统一 quiet SNaN）；per-lane 独立比对（98.5% 向量 SDC 仅单 lane 出错），禁止跨
   lane 聚合校验。
4. **正确性基线（x86-64 非回归）**：是否以 `#elif __aarch64__` / 按架构 meson 守卫的
   方式**增量**添加，未破坏 x86-64 参考路径；是否**不当扩大** `#ifdef __x86_64__`
   （仅当代码真正跨架构共享时才允许）。
5. **诚实性**：ARM64 未实现项是否从 `test_init` 返回 `EXIT_SKIP`、且理由为
   `"to be implemented (placeholder): <缺失内容>"`，禁止以 `EXIT_SUCCESS` 假通过的
   no-op 测试。
6. **架构相关缺陷（SVE/NEON VL）**：是否存在与向量长度（VL）相关的越界读写或误报；
   注意 CI runner 的 VL=128 会**掩盖** 512-bit 等高 VL 下才暴露的缺陷；关注新指令
   代际效应（SVE2 / FCMLA / FMMLA / 谓词等首代实现优先覆盖）。
7. **失败现场留存**：失败路径是否保留冷路径的 bit dump、sum-bit、failing-vector
   记录与 lane 偏移直方图，并保证 RNG 可重放（`-s seed`），以免过早返回而丢弃
   足以定位 SDC 的现场信息。
8. **多样性与检出率口径**：新增测试须满足激发维度多样性要求（相对既有覆盖是否
   带来新维度）；检出能力的论证须给出检出率下界口径。

**四份配置文件各自的内嵌方式**：

- [`AGENTS.md`](../../AGENTS.md)：以「代码检视重点：SDC 激发能力」小节列出上述 8 条
  要点，作为 codex 的仓库级上下文与检视指令。
- [`.coderabbit.yaml`](../../.coderabbit.yaml)：通过 `reviews.path_instructions` 按路径
  （`tests/cpu/**`、`framework/**`、`device/**`）分别内嵌 8 条要点，并配以
  `language: zh-CN`、`reviews.profile: assertive`、`reviews.auto_review`
  （`enabled` + `auto_incremental_review`）、`chat.auto_reply`。
- [`.pr_agent.toml`](../../.pr_agent.toml)：在 `[pr_reviewer].extra_instructions` 中内嵌
  8 条要点与**降噪约束**（只聚焦 SDC 激发能力、正确性与架构守卫，不输出无关的风格/
  命名/格式意见），并设 `[config].response_language = "zh-CN"`。
- [`.greptile/rules.md`](../../.greptile/rules.md)：以人读版上下文呈现 8 条要点；
  与之配套的 [`.greptile/config.json`](../../.greptile/config.json) 以 `triggerOnUpdates: true`
  与 8 条结构化 rules（对应 `sdc-excitation-intensity-coverage`、
  `sdc-golden-bytewise-memcmp`、`x86-64-non-regression`、`arm64-placeholder-honesty`、
  `sve-neon-vector-length`、`failure-scene-recording`，及「数据模式与缺陷激活」
  「多样性与检出率口径」两条新增 rules）落地同一套要点。

## 6. 覆盖度校验 workflow 用法

`.github/workflows/bot-coverage.yml` 用于核查四个机器人是否已对目标 PR 产出检视。

- **触发**：`pull_request` 的 opened / reopened / synchronize 事件，以及手动
  `workflow_dispatch`（输入 `pr-number`）。
- **行为**：有界轮询四个判据——codex（作者 `chatgpt-codex-connector[bot]`）、
  CodeRabbit（作者 `coderabbitai[bot]` 或 check-run `CodeRabbit`）、PR-Agent
  （check-run `pr-agent`）、Greptile（作者 `greptile-apps[bot]` 或 check-run app
  `greptile-apps`；Greptile 有时只产出 check-run 而不发 review/评论——如 PR #240，
  故以 App 归属而非 check-run 名称为备选判据），直到全部命中或到达等待上限。
  check-run 判据的口径（PR #241 加固）：check-run **名称**与创建方 **App slug**
  分属不同命名空间（App slug 全局唯一且全小写），脚本一次拉取后拆成
  `checks_names` / `checks_slugs` 两个集合——名称判据（CodeRabbit、pr-agent）
  只查名称集，slug 判据（Greptile）只查 slug 集，互不串扰；且只统计
  `status == "completed"` 的 check-run——queued / in_progress 不算产出（轮询
  继续等），completed 但 conclusion=failure 仍算已检视产出（避免失败场景空轮
  到等待上限）。
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

## 7. 当前状态

| 机器人 | 当前状态 | 备注 |
| --- | --- | --- |
| codex | 已生效（有 PR 产物证据；注意外部配额可能耗尽，限额提示时无实际检视内容） | 读取根 `AGENTS.md` |
| CodeRabbit | 已生效（有 PR 产物证据） | 读取根 `.coderabbit.yaml` |
| PR-Agent | 已生效（实测：PR #235 已产出 pr-agent 检视） | workflow 位于 `main`；读取根 `.pr_agent.toml`；LLM 用 Gemini 免费层；2026-10-13 修复回退模型（2.5-flash 已对新用户下线，见 7.1） |
| Greptile | 已生效（实测：PR #238 已产出 greptile-apps[bot] 检视） | 2026-10-13 完成安装授权；读取 `.greptile/config.json` + `rules.md` |

> 诚实说明：Greptile 曾长期如实标注为「待人工安装」；2026-10-13 管理员完成 GitHub App
> 安装授权后，已在 PR #238 实测产出检视（review + 行级评论 + check-run pass），上表
> 随之更新为「已生效」。

### 7.1 PR-Agent 的免费 LLM 配置（Google Gemini 免费层）

PR-Agent 软件本身免费、公共仓库 Actions 分钟数也免费，唯一成本是它调用的 LLM。
`.github/workflows/pr-agent.yml` 走 **Google AI Studio 免费层** + `gemini/gemini-3.8-flash`：

- `CONFIG.MODEL: "gemini/gemini-3.8-flash"`
- `CONFIG.FALLBACK_MODELS: '["gemini/gemini-3.5-flash-lite", "gemini/gemini-3.6-flash"]'`

回退链演进：初始配置的回退模型 `gemini/gemini-2.5-flash` 已被 Google 下线——免费层
新项目调用一律 404「no longer available to new users」（2.5 系列仅向历史活跃用户开放）。
2026-10-13 PR #237 恰逢主模型 503（免费层高峰限流，瞬时）叠加回退 404，回退链失效导致
check 失败；已将回退链换为对新项目开放的 `3.5-flash-lite` + `3.6-flash`。注意
`3.1-flash-lite` 将于 2027-05-07 关停，勿用作回退。

密钥配置（仓库 Secret `GEMINI_API_KEY`，已配置并实测通过）：

1. 打开 [Google AI Studio](https://aistudio.google.com/apikey) 创建 Key。Key 必须挂在某个
   Google Cloud 项目下；AI Studio 自 2026-05-28 起改发 `AQ.` 前缀的「授权密钥」，该格式
   经实测与 PR-Agent（LiteLLM）兼容。
2. 在仓库 `Settings → Secrets and variables → Actions` 建立/更新 Secret **`GEMINI_API_KEY`**。
3. 无需改代码：下一次 PR（opened/reopened/synchronize）即以免费层运行检视。

> 历史问题：早期 Key 所属项目**预付费额度耗尽**（HTTP `402 RESOURCE_EXHAUSTED`）导致失败；
> 该错误**不会自动恢复**，需更换为额度未耗尽的 Key（现已更换，PR #235 实测成功）。

> 说明：gemini-cli 的免费额度靠交互式 OAuth 登录，**无法**在 GitHub Actions 中无人值守
> 使用；PR-Agent 在 CI 只能走 API Key，故此处选用 AI Studio 免费层 API Key。

### 7.2 自动开 PR：一个特性分支一个 PR

GitHub 天然保证「同一 head 分支 → 同一 base 分支**只能有 1 个 open PR**」：对同一特性
分支再 push 只会触发 `pull_request: synchronize`、**复用/更新**该 PR，不会新建。因此
「一个特性分支 = 一个 PR」无需额外实现；本节新增的
[`.github/workflows/auto-pr.yml`](../../.github/workflows/auto-pr.yml) 只补齐**首次 push
免手动点建**这一步。

- **触发**：`push` 到非 `main`、非 `gh-pages` 的分支（`branches-ignore: [main, gh-pages]`）。
- **为何排除 `main`**：本仓约定不直接推 `main`，故不为 `main` 建 PR。
- **为何排除 `gh-pages`**：[`.github/workflows/multi-os-verify.yml`](../../.github/workflows/multi-os-verify.yml)
  的 `benchmark-trend` job 用 `benchmark-action auto-push: true` **自动推送** `gh-pages`
  分支，若不一并排除会被误开一个无意义的 `gh-pages -> main` PR。
- **行为**：先用 `gh pr list --head <branch> --state open` 判重——已有 open PR 则跳过
  （后续 push 会 `synchronize` 更新它）；否则 `gh pr create --base main --head <branch>`。
  分支相对 `main` 无新提交、或 PR 已存在时 `gh pr create` 非 0 退出，以 `::notice` 优雅
  收场，**不让 job 变红**。

**必须用 `PR_TOKEN`（而非默认 `GITHUB_TOKEN`）建 PR**——这是本 workflow 的关键约束：

- 用默认 `GITHUB_TOKEN` 创建的事件（push / PR / comment 等）**不会启动新的 workflow
  run**，这是 GitHub 为防工作流自触发死循环的刻意设计（例外仅 `workflow_dispatch` /
  `repository_dispatch`）。后果是 `pr-agent.yml` / `bot-coverage.yml` **不会在该 PR 上
  被触发**，四 bot 也就不会在 `opened` 时评审——症状是「静默」：无失败 run，必需检查
  永远停在 `Expected — Waiting for status to be reported`。
- 用 **fine-grained PAT**（或 GitHub App token）发起的 PR 被视为「人」创建，可**正常
  触发**下游检视机器人。这是选用 PAT 的唯一理由。
- 额外原因：本仓 Actions 的 `default_workflow_permissions` 为 `read`、
  `can_approve_pull_request_reviews: false`，默认 `GITHUB_TOKEN` 连 PR 都**建不了**；改用
  PAT 后该设置无需改动。

配置步骤（仓库 Secret `PR_TOKEN`）：

1. 建 fine-grained PAT：`Settings → Developer settings → Personal access tokens →
   Fine-grained tokens`；Repository access 选 `wangxumarshall/sdcshield`；权限
   `Contents: Read`、`Pull requests: Read and write`；生成后复制。
2. 仓库 `Settings → Secrets and variables → Actions → New repository secret`，名
   `PR_TOKEN`，值粘贴 PAT。
3. （可选）设置过期时间并在到期前轮换，以减小暴露面。

> 诚实说明：`PR_TOKEN` **未配置**时本 workflow **优雅跳过**——日志输出
> `::warning::PR_TOKEN 未配置 → 跳过自动开 PR`，job 仍绿，**绝不假报「已开 PR」**。
> 备选方案：长期/团队场景可用 GitHub App token（`actions/create-github-app-token`）
> 替代 PAT，权限更细、不受个人账号过期影响。

## 8. 第 5 bot：本地 claude 对抗检视

前四 bot 均由 GitHub 托管（App / workflow）触发；第 5 bot 是**本地无头 claude
code**，以对抗性渗透测试者身份检视 PR，意见上 GitHub 后与四 bot **同等对待**（计入
`pr_loop` 的 wait 信号、collect 导出、闭环逐条处置）。引擎
[`scripts/pr_local_review.py`](../../scripts/pr_local_review.py)（`pr_loop` 姊妹件，
Python3 标准库 + `gh`/`claude` 子进程，零第三方依赖；claude 凭据 `source
<agent.env>`——**轮5 起与 task_loop 同一解析**：`TASKLOOP_STATE_DIR`/
`CITRIAGE_STATE_DIR` 重定向目录无条件优先，默认实例
`~/.local/state/sdcshield-task-loop/agent.env` 优先、
`~/.local/state/sdcshield-ci-triage/agent.env` 回退（迁移过渡期两份并存
时**换 token 要换 task_loop 目录那份**，旧目录只是回退）。

### 8.1 判据（双条件）

第 5 bot 的检视信号判定为**双条件**，缺一不可：

- 作者 login == `wangxumarshall`（gh CLI 以当前登录用户身份发 review；第 5 bot 展示名 `wangxumarshall(claude and glm)`，claude+glm 双模型对抗审查）；
- 正文含标记 `<!-- local-claude-review -->`（review body 与每条行级评论正文首行均带）。

双条件（而非纯作者映射）是为了避免把用户本人的普通评论误判为第 5 bot——与 pr-agent
的「作者 `github-actions[bot]` 且正文含 `<!-- pr-agent:review:full -->`」同构。
`pr_loop.py` 的 `BOT_ORDER` 已扩展为 `("codex","coderabbit","pr-agent","greptile",
"local-claude")`，第 5 bot 是**硬门槛**：五 bot 等齐才闭环，缺第 5 bot 即
`WAITING_BOTS`。

### 8.2 两条触发线

| 触发线 | 载体 | 说明 |
| --- | --- | --- |
| 事件线 | `pr_loop.py wait` 内 `ensure_local_review()`；`ci_triage.py` 车道 B reap 后 | 进入阻塞轮询 / 派发修复 agent 前，先确保该 head 已有第 5 bot 信号（未审则同步跑一次并发布） |
| 时间线 | cron `*/30 * * * *` 跑 `scripts/pr_local_review.py --all-open` | 枚举全部 open PR，对尚无第 5 bot 信号的 head 跑一次对抗审查并发布，兜底补漏 |

同 head 幂等：`already_reviewed`（body 含标记且 `commit_id` 锚定当前 head）为真即
跳过——事件线 + 时间线撞车只审一次。第 5 bot 是辅助触发线，**非硬前置**：触发失败
（gh 失败 / claude 未装 / agent.env 缺失 / head 未 fetch / 输出非 JSON）仅告警降级，
不阻塞轮询（等齐五 bot 的等待继续）。

### 8.3 对抗性检视方法论（五要素 + SDC 语境核查）

prompt 由 `build_adversarial_prompt()` 构造，五要素：

1. **pentester framing**：以对抗性渗透测试者身份审视 diff——只找「如何被利用 / 被
   绕过 / 触发缺陷」，宁可过度怀疑，不产出「友好改进建议」。
2. **多批判镜头（按 diff 大小缩放）**：镜头库恒含 Skeptic（怀疑论者，挑战每处假设
   与边界）/ Architect（架构师，审视结构、耦合、抽象泄漏）/ Minimalist（极简主义者，
   审视复杂度与过度设计）；按 diff 行数缩放须采用的镜头数——<50 行 1 镜头、50–200
   行 2 镜头、>200 行 3 镜头。
3. **结构化证据锚定（硬性输出契约）**：每条 finding 必须给出 file:line + 具体证据
   （引用代码片段 / 构建 / 测试真实输出 / golden 比对结果）+ 严重级（Critical /
   Important / Minor 三者取一）+ 分类（正确性 / 安全 / 并发 / 内存 / 性能 / 可维护性
   取其一）；禁止「可能有问题」「建议改进」「LGTM」类空泛句。
4. **意图镜（intent lens）**：对照 PR 标题 / 描述 / 关联 issue 的目标，抓 scope
   creep（超出目标的改动）与 drift（逐行都对、整体跑偏）。
5. **差分审查顺序**：先看「删除」（安全回归——被删代码是否曾因 security/fix/CVE
   引入却在此误删）→ 再查「新增」（新攻击面）→ 最后验证「影响链」（改动如何影响
   所有调用点）。

外加 **SDC 仓库语境核查**（对照 `AGENTS.md`「SDC 激发能力」）：golden 比对必须字节级
`memcmp`（禁止近似/容差）；向量校验必须 per-lane 独立比对（禁止跨 lane 聚合）；
`TEST_LOOP` 必须定时持续压测（非固定迭代次数）；浮点结果必须逐位可复现（FP 可复现）。

### 8.4 意见形态

- 行级 finding（`path` 且 `line` 非 None）→ 挂 diff 行的**行级评论**（可逐条 reply、
  逐条计数，coderabbit 式）；
- 挂不上 diff 行的（`line` 为 None 或无 `path`）→ 收进 review body 的 ```json 段
  （pr-agent 式）；
- review body 与每条行级评论正文首行均带 `<!-- local-claude-review -->`；
- `commit_id` 锚定当前 head，供 `pr_loop` 的 commit 锚定过滤。

意见计数：`pr_loop` 对第 5 bot 的计数 = body JSON 段 findings 数 + 行级评论数
（`count_local_claude_findings`）。

### 8.5 判据分裂（已知不变量破例）

`.github/workflows/bot-coverage.yml` **保持四 bot 不变**——它是 GitHub-hosted
workflow，触发不了本地 claude（本地审查只能由本机 gh 身份跑，workflow 无法调用本机
claude）。由此产生「pr_loop 等五 bot、bot-coverage 校验四 bot」的判据分裂，作为
**已知不变量破例**显式记录：bot-coverage 报告的「汇总 4/4」仅指四个外部 SaaS bot，
不代表第 5 bot 已检视；第 5 bot 的完成度以 `pr_loop status` 的第 5 位（local-claude）
为准。

### 8.6 注入面与沙箱边界（诚实声明）

第 5 bot **无沙箱**：`claude` 以 `--dangerously-skip-permissions` 在可写的隔离
worktree 内运行，可以执行任意命令、读写任意文件。`DIFF_BANNER` / `PR_CONTEXT_BANNER`
（以及发布到 GitHub 的 `UNTRUSTED_BANNER`）**只是对检视者/处置 agent 的提示注入缓解
（prompt 内要求其忽略 diff/标题/描述中的指令），不是安全边界、不构成沙箱**。攻击面 =
同仓 PR 内容（diff / 标题 / 描述 / 关联 issue）——一个恶意 PR 若能让 claude 违背
横幅约束，即可在运行第 5 bot 的机器上执行任意命令。触发第 5 bot 之前应确认 PR 来自
可信作者；不要把「写文件无害且即弃」理解为安全保证。

## 9. 延伸阅读

- [代码检视代理仓库上下文 `AGENTS.md`](../../AGENTS.md)
- [CodeRabbit 配置 `.coderabbit.yaml`](../../.coderabbit.yaml)
- [PR-Agent 配置 `.pr_agent.toml`](../../.pr_agent.toml)
- [Greptile 检视上下文 `.greptile/rules.md`](../../.greptile/rules.md)
- [仓库 README](../../README.md)
- [覆盖度校验 workflow `.github/workflows/bot-coverage.yml`](../../.github/workflows/bot-coverage.yml)
- [自动开 PR workflow `.github/workflows/auto-pr.yml`](../../.github/workflows/auto-pr.yml)
