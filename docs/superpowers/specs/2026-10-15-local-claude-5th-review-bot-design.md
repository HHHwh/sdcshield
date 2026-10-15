# 本地 claude code 作为第 5 个 review bot — 设计文档

日期：2026-10-15
状态：待用户批准
作者：会话产物（brainstorming），「用户已确认」= 用户拍板项；「controller 默认」= 作者代定，批准时请一并审。

## 1. 意图（why）

现有 pr-loop 依赖四个外部 SaaS 检视 bot（coderabbit / pr-agent / greptile / codex），它们是
事件驱动、托管在 GitHub 侧，用户无法直接控制其检视质量与「对抗性」程度。本设计新增
**第 5 个 bot = 本地无头 claude code**：用 `claude -p` 跑一个「对抗性代码检视」，把结构化
意见发布到 GitHub 上，与四 bot **同等对待**（计入 wait 信号、collect 导出、闭环处置）。

核心动机：本仓库（SDCShield）已经为 ci-triage 配好了 claude headless 认证（`ANTHROPIC_AUTH_TOKEN`
+ `ANTHROPIC_BASE_URL` + 模型映射，见 spec C3 / `agent.env`）。复用同一套凭据，零额外
成本，即可把一个「可控、可调、对抗性」的检视者接入 pr-loop 闭环，且它的意见和四 bot 一样
在 GitHub 上可见、可逐条答复。

## 2. 目标 / 非目标 / 反目标

**目标**
1. 本地无头 claude code 以「对抗性代码检视」审查 PR，产出结构化意见（file:line + 证据 + 严重级）。
2. 意见以 gh CLI（用户本人身份 + 唯一标记）发布到 GitHub，与四 bot 意见同源可见。
3. pr_loop 把第 5 bot 纳入判据：`BOT_ORDER` 4 → 5，wait / status / collect / 意见计数全链路同等。
4. 两条触发线：**事件触发**（push 后作为 pr-loop 阶段 2 等待的第 5 bot）+ **时间触发**（cron 定时审查 open PR）。
5. agent 处置阶段对 5 个 bot 的**每条**意见在 GitHub 逐条闭环答复（接纳 / 变通 / 驳回都要准确完整回复）。

**非目标**
- 不新增 GitHub bot 账号 / 专用 token（身份 = 现有用户 `wangxumarshall` + 标记）。
- 不改造四 bot 的既有判据语义（只做「新增第 5 bot」，不重写 codex/coderabbit/pr-agent/greptile 判定）。
- 不做通用「本地 review 服务」——只服务本仓库本机的 pr-loop / ci-triage。

**反目标（做了就算失败）**
- 第 5 bot 意见伪造/空泛（对抗性检视禁止「LGTM」式空话；每条必须有证据）。
- 与用户本人的手工评论混淆（标记缺失导致「本地 claude 意见」和「人评论」不可区分）。
- 闭环答复漏答/错答（接纳的没说清为什么、驳回的没给实证）。
- 两条触发线对同一 head 重复发意见（幂等去重）。

## 3. 约束与事实

- C1 claude headless 认证已打通：`ANTHROPIC_AUTH_TOKEN` + `ANTHROPIC_BASE_URL` + 模型映射，
  `env -i` + source `~/.local/state/sdcshield-ci-triage/agent.env` 下 `OK-MINENV3` 实测通过（ci-triage spec C3）。
- C2 `gh` 已认证（wangxumarshall，repo+workflow+gist 等 scope），可发 PR review / comment / reply。
- C3 `pr_loop.py` 现有四 bot 判据：`BOT_ORDER = ("codex","coderabbit","pr-agent","greptile")`，
  各 bot 靠「作者 login / check-run 名 / 内容标记」判定（见 `scripts/pr_loop.py` 头注释与常量段）。
- C4 `ci_triage.py` 车道 B 复用 `pr_loop.classify_state` / `fetch_pr`——判据若在 pr_loop 内统一扩展，ci_triage 自动认知第 5 bot。
- C5 本地无 webhook；「事件触发」在本地语境下靠 pr-loop 流程主动触发（见 §6）。
- C6 时区统一 UTC（沿用）。

## 4. 架构

```
第 5 bot（本地 claude 对抗审查）两条线共用同一引擎：

  引擎：scripts/pr_local_review.py <PR>
    ├─ source agent.env（claude headless 凭据）
    ├─ 拉取 PR diff + 上下文（gh pr diff / gh pr view，注入防注入横幅）
    ├─ claude -p "<对抗性检视 prompt>" --dangerously-skip-permissions
    ├─ 解析结构化意见（JSON / markdown 段）
    └─ gh api 发布 review（本人身份 + 标记 <!-- local-claude-review -->）

  事件触发线：pr_loop.py wait 阶段
    wait <PR> 在等四 bot 时，先/同时跑一次 pr_local_review.py <PR>（若该 head 尚未发过）
    → 发布后第 5 bot 信号即存在 → wait 把第 5 bot 计入完成判定

  时间触发线：cron（复用 ci-triage 的 agent.env）
    */30 * * * * python3 scripts/pr_local_review.py --all-open（幂等：同 head 只审一次）
```

设计原则沿用「每个 tick 只做有限推进、不阻塞长等」：事件线在 wait 内主动触发（一次），
时间线 cron 兜底补漏，两条线靠「同 head 幂等去重」避免重复。

## 5. 第 5 bot 判据与身份（controller 默认）

- **身份**：gh CLI 以 `wangxumarshall`（当前登录用户）发 review；作者 login = `wangxumarshall`。
- **唯一标记**：review body / 评论正文首行含 `<!-- local-claude-review -->`（与 pr-agent 的
  `<!-- pr-agent:review:full -->` 同构）。判据 = `作者 wangxumarshall` **且** `正文含该标记`。
  —— 双条件，避免与用户本人的普通评论混淆（反目标 2）。
- **BOT_ORDER** 扩展为 `("codex","coderabbit","pr-agent","greptile","local-claude")`（第 5 位置，
  顺序即 status/wait 展示顺序）。
- **判据落点**（builder check 修正）：`bot_of()` 是纯「作者 login → bot」映射，直接把
  `wangxumarshall` 加入 `LOGIN_TO_BOT` 会把人类普通评论误判为第 5 bot。必须新增专用判定分支
  （类似 `pragent_comment()`），对 review / 行级评论 / PR 评论三载体都做「作者 wangxumarshall
  **且** 正文含标记」双条件判定。
- **信号形态**（builder check 修正）：第 5 bot 的每条 finding 尽量挂成**行级评论**（挂在 diff
  相关行，可逐条 reply、逐条计数，coderabbit 式）；挂不上 diff 行的 finding 退到 review body
  的 JSON 段（pr-agent 式，需新增解析计数，不能仅靠「每 review 计 1」）。review body 首行带标记。

## 6. 两条触发线

**缺位语义（用户已确认：硬门槛）**：第 5 bot 与四 bot 同等——五 bot 等齐才闭环，缺第 5 bot
即 `WAITING_BOTS`。为避免本地 bot 缺位导致死锁，两条线**双保险保证触发**：事件线在 wait 内
总是主动触发本地审查，时间线 cron 兜底；ci_triage 车道 B 也在 reap 后主动触发（见 §9）。

### 6.1 事件触发（push 后，pr-loop 阶段 2 等待的第 5 bot）

- `pr_loop.py wait <PR>` 在进入四 bot 阻塞轮询前，先判断第 5 bot 是否已对该 head 发过信号
  （查 review body 标记 + head 锚定）。
- 未发过 → 主动跑一次 `pr_local_review.py <PR>`（同步/异步均可，倾向同步跑完再进入轮询），
  发布 review。
- 之后第 5 bot 与四 bot 同等待遇：wait 把它计入「当前 head 完成信号」，status 显示第 5 位完成度。
- **同 head 幂等**：`last_dispatched_head` 语义下，同一 head 只触发一次本地审查（沿用现有幂等）。

### 6.2 时间触发（cron 定时审查 open PR）

- cron（建议 `*/30 * * * *`）跑 `pr_local_review.py --all-open`：枚举 open PR，对每个「尚无
  第 5 bot 信号」的当前 head 跑对抗审查并发布。
- 幂等去重：同 head 已发过 → 跳过（除非加 `--force` 手动重审）。
- 这条线独立于用户是否跑 pr-loop，兜底「push 后无人跑 /pr-loop」的场景。

## 7. 对抗性检视 prompt（核心，5 点方法论）

调研来源：`poteto/noodle` adversarial-review skill、Codacy Verity、Adversarial Review（Qiu & Gill 2026）、
MOSAIC-Bench（pentester framing 实证）、GitHub skills differential-review。

第 5 bot 的 prompt 内嵌以下对抗性契约（实现者据此细化措辞）：

1. **pentester framing**（最高优先）：角色定位为「对抗性渗透测试者」，任务 = 找出这段 diff
   如何被利用、绕过、触发缺陷，而非「礼貌提建议」。MOSAIC-Bench 实证：该 framing 使漏检从
   24.8% 降至 3–17.6%。
2. **多批判镜头**：一次审查从 ≥2 个镜头交叉攻击——Skeptic（挑战每个假设/边界）、Architect
   （结构·耦合·抽象泄漏）、Minimalist（复杂度/过度设计）。按 diff 大小缩放：<50 行 1 镜头、
   50–200 行 2 镜头、200+ 行 3 镜头。因用户指定 claude，不换模型，靠「换镜头 + pentester
   framing」对冲同模型偏见（记录为已知折衷）。
3. **结构化分歧 + 证据锚定**：每条 finding 必须 `file:line + 具体证据（引代码/引构建/引
   golden 比对）+ 严重级（Critical/Important/Minor）+ 分类（正确性/安全/并发/内存/性能/可维护性）`。
   禁止「可能有问题」「建议改进」类空泛句。
4. **意图镜（intent lens）**：对照 PR 标题/描述/关联 issue 的目标，抓 scope creep 与
   「逐行都对、整体跑偏」的 drift。
5. **差分审查顺序**：先看「删除」（安全回归：曾因 security/fix/CVE 删除又被加回）→ 再看
   「新增」（新攻击面）→ 最后验证「影响链」（调用点/生命周期）。结合本仓库 SDC 语境，
   额外核查 AGENTS.md「SDC 激发能力」8 项（golden 字节级比对、per-lane、TEST_LOOP 定时压测、
   FP 可复现等）。

输出契约：结构化 review，body 首行放 `<!-- local-claude-review -->`，其后是可被 pr_loop
现有意见计数解析的条目（含行级评论）。

## 8. 闭环完整答复

- pr-loop 阶段 4 agent 处置后，**对 5 个 bot 的每一条意见**（review body 里的每条 finding +
  每条行级评论）在 GitHub 上**逐条 reply**。
- 每条答复固定结构：`结论（采纳 / 变通 / 驳回） + 理由 + 证据（采纳=已提交 commit SHA；
  驳回=引用代码/构建/回归真实输出）+ 关联 commit/PR 链接`。
- 采纳 → 引用修复 commit SHA；驳回 → 给出实证依据（CLAUDE.md 规则 2：ninja 零警告、真实
  输出、定向回归、x86 非回归）。
- 机制（载体分两种，均逐条答复）：
  - 行级评论 finding → `gh api` 逐条 reply（`POST .../pulls/{pr}/comments/{id}/replies`）。
  - body JSON 段 finding（挂不上 diff 行）→ 闭环时发一条 issue comment，按 finding 逐条列出
    答复（接纳带 SHA / 驳回带实证），每条 finding 独立成段。
- 完整性校验：闭环结束时，脚本核对「待处置意见数 == 已答复数」，有漏答即报错不结案
  （诚实性，反目标 3）。

## 9. 与 ci-triage 的关系

- 车道 B 复用 `pr_loop.classify_state` → 判据扩展后，车道 B 自动把第 5 bot 计入 bot 完成度
  （逻辑零改动）。**但硬门槛语义下，车道 B 必须在 reap 后主动触发本地审查**，否则无人值守的
  `fix/ci-*` PR 没人跑本地审查会永久 `WAITING_BOTS` 卡死——这是 builder check 发现的死锁点，
  已纳入车道 B 推进（在派发修复 agent 前先确保第 5 bot 信号存在）。
- 时间触发线的 cron 与 ci-triage 的 cron 共用 `agent.env` 凭据（C1）。
- **审查上下文（builder check 决策）**：本地审查用**独立只读 worktree**（`git worktree add` 或
  临时 checkout）读完整源码，既不碰用户主工作区，也不抢 ci_triage 修复 agent 的 `autoci` clone
  → 审查与修复 agent 无需共享单飞槽（审查不碰修复 agent 的树，只共享「claude 进程配额」层面
  的轻约束，避免同时跑多个 claude 烧配额）。此项为实现细节，builder 定。

## 10. 文件清单（预期）

- 新增 `scripts/pr_local_review.py`：本地 claude 对抗审查 + 发布意见引擎（pr_loop 姊妹件，零第三方依赖）。
- 修改 `scripts/pr_loop.py`：`BOT_ORDER` 扩到 5；新增第 5 bot 专用判定分支（双条件，非 LOGIN_TO_BOT）；
  wait 内主动触发本地审查；意见计数支持 body JSON 段解析；闭环逐条答复。
- 修改 `scripts/ci_triage.py`：车道 B 在 reap 后主动触发本地审查（硬门槛死锁规避）；同步「四 bot→五 bot」文案。
- **`.github/workflows/bot-coverage.yml` 保持四 bot 不变**（GitHub-hosted 无法触发本地 claude）——
  由此产生「pr_loop 等五 bot、workflow 校验四 bot」的判据分裂，作为**已知不变量破例**显式记录
  （在 pr-loop.md 与 pr-review-bots.md 注明），不强行改造该 workflow。
- 修改 `docs/build-deploy-pr-ci/pr-review-bots.md`：新增第 5 bot 章节（判据、触发、对抗性方法论、四/五 bot 分裂说明）。
- 修改 `docs/build-deploy-pr-ci/pr-loop.md`：五 bot 流程更新。
- 修改 `CLAUDE.md` / `AGENTS.md`：文档地图 pr-loop 条目同步（若有「四 bot」字样）。
- 新增测试 `tools/excite/tests/test_pr_local_review.py`（或并入 test_pr_loop.py）：判据/幂等/答复闭环锚定。

## 11. 留给 builder 的自由度

- `pr_local_review.py` 的 CLI 旗标名、意见 JSON 结构、prompt 具体措辞。
- 第 5 bot 判据标记字符串（默认 `<!-- local-claude-review -->`，可改）。
- 事件线同步 vs 异步跑本地审查（倾向同步：wait 前跑完，信号即时）。
- 时间线 cron 的具体间隔与去重策略（默认 `*/30` + 同 head 幂等）。
- 闭环答复的 reply 目标（review body finding vs 行级评论）与完整性校验实现。
- 单飞槽共享策略（§9）。

## 12. 验收标准（实现后逐项真实验证）

- V1 第 5 bot 对真实 PR 发 review，body 含标记、作者 wangxumarshall，GitHub 页面可见。
- V2 pr_loop `status` 显示第 5 bot 完成度；`wait` 等齐五 bot。
- V3 时间线 cron 对无信号 open PR 补发；同 head 复跑不重复（幂等实证）。
- V4 agent 处置后，5 bot 的每条意见在 GitHub 都有逐条 reply，接纳带 SHA、驳回带实证；无漏答。
- V5 对抗性检视对已知缺陷样本（如本仓库的 golden 比对/per-lane/TEST_LOOP 违规）能给出
  file:line + 证据级 finding（对抗性能力实证）。
