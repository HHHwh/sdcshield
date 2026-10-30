# ci-triage：CI 无人值守分诊与修复闭环

> **已由 task-loop 取代（2026-10-29，PR #317）**：ci_triage.py 的编排职责已并入
> 统一任务引擎 `scripts/task_loop.py`（经模块别名薄壳过渡）。本文保留为历史
> 手册；现行运维见 [task-loop.md](task-loop.md)。cron 部署行以 task-loop.md 为准。

**定位**：cron 每 30 分钟驱动 `scripts/ci_triage.py scan`——main 分支 CI 红 run
自动开跟踪 issue → 派 headless claude agent 在**独立克隆**里修复 → push
`fix/ci-<runid>` 分支 → `auto-pr.yml` 自动开 PR → 复用
[pr-loop](pr-loop.md) 四 bot 检视闭环 → READY_TO_MERGE 时 @人工 merge。
**merge 永远在人**；`fix/ci-*` 之外的用户在研 PR 完全不碰（spec D6）。

- 引擎：[`scripts/ci_triage.py`](../../scripts/ci_triage.py)（Python 3.11
  标准库 + `gh` CLI，零第三方依赖；`import pr_loop` 复用 PR 判定）
- 设计规格（会话产物，不入库）：主仓
  `docs/superpowers/specs/2026-10-13-unattended-ci-triage-design.md`
- 测试：`tools/excite/tests/test_ci_triage.py`（31 用例，离线锚定全部关键语义）

## 1. 架构一览

```
cron(每 30 分钟)
  └─ cd ~/operator/sdcshield-autoci && python3 scripts/ci_triage.py scan
       ├─ flock scan.lock(单实例;拿不到锁本轮退出)
       ├─ PAUSED 检查(存在→派发全停,观测照常)
       ├─ 车道 A 收割(reap_or_advance): 先归一历史 ISSUED+PID 条目→DIAGNOSING,
       │    再按结局推进(成功→PR_OPEN;失败→重派/超限→ESCALATED)
       ├─ heal: 无 agent 在跑时 fetch + 恢复干净的 main(自愈 agent 遗留脏树)
       ├─ 车道 A 派发扫: gh run list 扫 main 红 run
       │    → 幂等开/复用 issue → ISSUED → 派 agent(单飞,60 min 预算)
       │    → dispatch 成功即置 DIAGNOSING(单一权威,9d6ba452)
       ├─ 车道 B: 对 PR_OPEN 条目 fetch_pr + classify_state(复用 pr_loop)
       │    WAITING_BOTS/QUIET_WINDOW → 不动;READY_TO_FIX → collect 导出+派
       │    bot 处置 agent(rounds≤4,同 head 只派一次,派发即置 BOT_FIX);
       │    READY_TO_MERGE/CLOSED → @人工 + 关 issue + DONE
       └─ state.json 原子落盘(先写 .tmp 再 os.replace)
```

监控面：workflow ∈ {Multi-OS Verify, Sanitizer, CodeQL, Security}（env
`CITRIAGE_WORKFLOWS` 逗号分隔可覆盖）、event ∈ {schedule, push}、分支 =
main、回看 3 天、conclusion = failure。

关键约束（全部由单测锚定）：全局同时最多 **1 个 agent**（车道 A/B 共槽）；
同 workflow 有活跃条目时新红 run 只追加评论不开新 issue（同根因收敛）；
认证失效连续 ≥3 次全局熔断。
（单飞约束曾存在未锚定路径——车道 A 派发后状态未转 DIAGNOSING 导致
单飞失明，生产首跑暴露；**已于 9d6ba452 修复**：dispatch 派发成功统一置
DIAGNOSING（车道 A）/BOT_FIX（车道 B），历史 ISSUED+PID 条目由 reap
归一收割，单测 29–31 锚定。首跑现场与后续演进见 §8 A3。）

## 2. 部署（一次性）

| # | 步骤 | 状态 |
| --- | --- | --- |
| 1 | `git clone git@github.com:wangxumarshall/sdcshield.git ~/operator/sdcshield-autoci` | ✅ 已完成（Task 4 部署，2026-10-14） |
| 2 | 写 `~/.local/state/sdcshield-ci-triage/agent.env`（凭据文件，**人工步骤**） | ✅ 已存在（419 B，权限 600） |
| 3 | 首跑 `python3 scripts/ci_triage.py dry-run` 观测红 run 检出 | ✅ 已完成（Task 4，见 §8 验收 A1） |
| 4 | crontab 安装部署行（spec §11 原文） | ✅ 已完成（2026-10-15，见 §2.2 与 §8 验收记录） |
| 5 | 本手册 + CLAUDE.md 文档地图 | ✅ 本文档 |

**部署时序依赖（如实声明）**：`scripts/ci_triage.py` 随 feat/ci-triage 分支交付，
autoci 克隆的 main 在该分支 PR 合入前**没有此文件**——期间 crontab 每半小时
触发会向 cron.log 写一行 `can't open file` 错误（无害，PR 合入后自动恢复，
无需人工干预）。

### 2.1 agent.env（凭据文件，不入库）

`~/.local/state/sdcshield-ci-triage/agent.env`：agent 派发前 `source` 的
环境快照——`ANTHROPIC_AUTH_TOKEN` / `ANTHROPIC_BASE_URL` / 模型映射
（`ANTHROPIC_DEFAULT_*_MODEL` / `CLAUDE_DEFAULT_MODEL`）/ `PATH`。权限
**必须 600**。cron 裸 PATH 下没有它 claude 既找不到也认证不了（cron 环境
只有 `/usr/bin:/bin` 量级裸 PATH + claude/gh 凭据需环境快照提供）。
该文件是凭据文件，内容绝不进日志/issue/state.json。

注意：env 快照可能随网关 key 轮换失效——失效症状 = agent 日志全体
`API Error`/`403`/`Not logged in`，连续 ≥3 次自动熔断（见 §4）。

### 2.2 crontab 部署行（spec §11 原文）

```cron
*/30 * * * * cd /home/sdc/wangxu/operator/sdcshield-autoci && /usr/bin/python3 scripts/ci_triage.py scan >> ~/.local/state/sdcshield-ci-triage/cron.log 2>&1
```

安装：`crontab -e` 追加上行（幂等：已有同内容行则跳过）。**cron PATH
注意**——cron 环境只有 `/usr/bin:/bin` 量级的裸 PATH，故本行用显式
`/usr/bin/python3`；agent 派发所需的 `claude`/`gh` 由 agent.env 内的
`PATH` 提供（dispatch 命令先 `source agent.env` 再起 claude），cron 自身
PATH 里有没有它们都无所谓。

**第 5 bot（本地 claude）时间触发线**（补漏/周期重审）：

```cron
*/30 * * * * cd /home/sdc/wangxu/operator/sdcshield && /usr/bin/python3 scripts/pr_local_review.py --all-open >> ~/.local/state/sdcshield-ci-triage/cron.log 2>&1
```

该行在**主工作区**运行（`pr_local_review.py` 随特性分支交付；其 run_review
需在主仓库内 `git worktree add` checkout head，故 cd 主工作区而非 autoci
克隆）。`--all-open` 枚举全部 open PR，对尚无第 5 bot 信号的 head 跑一次
对抗审查并发布（幂等：同 head 已审跳过）。它是第 5 bot 的**时间触发线**
——补漏事件线（车道 B / `pr_loop wait` 的主动触发）未覆盖的场景，并周期
重审新 push 的 head；与车道 B 主动触发互为冗余，确保无人值守的 fix/ci PR
也有第 5 bot 信号，避免 `WAITING_BOTS` 硬门槛死锁。

### 2.3 完全停用

```console
crontab -e                      # 删除部署行(停调度)
touch ~/.local/state/sdcshield-ci-triage/PAUSED   # 保险:已在跑的 tick 也不再派发
```

## 3. 状态机速查

条目状态存于 `state.json` 的 `entries[]`（每条 = 一个红 run 主键 run_id；
同 workflow 后续红 run 挂 `extra_runs` 收敛）。

> **历史缺陷注记（已修复）**：生产首跑（2026-10-15）曾暴露 Critical 缺陷
> ——`dispatch_agent` 派发后不把条目置为 DIAGNOSING，单飞/收割失明。**已于
> 9d6ba452 修复**：dispatch 派发成功统一置 DIAGNOSING（车道 A）/BOT_FIX
> （车道 B），历史 ISSUED+PID 条目由 reap 归一收割，单测 29–31 锚定。
> 下表即当前实码语义。首跑现场与修复后的后续演进见 §8 A3。

### 3.1 车道 A（红 run → 修复 PR）

| 状态 | 含义 | 进入条件 | issue 上发生什么 |
| --- | --- | --- | --- |
| `DETECTED` | 扫到新红 run | state/GitHub 双源均无该 run 的条目 | （瞬态，建/复用 issue 后立即转 ISSUED） |
| `ISSUED` | 跟踪 issue 就绪 | issue 创建（幂等标记核实）或复用既有 | issue 已开，正文含失败日志摘录 + 幂等标记 + 防注入横幅 |
| `DIAGNOSING` | 修复 agent 在跑 | 派发扫对 ISSUED/DIAGNOSING 且无 agent 的条目派 agent | （prompt 要求 agent 最终把报告贴 issue） |
| `PR_OPEN` | 修复 PR 已开 | agent 报 done **且** `gh pr list --head fix/ci-<runid>` 核实到 open PR | agent 已贴修复报告（其报告含 PR 链接） |
| `DONE` | 结案 | 车道 B 判定 READY_TO_MERGE/CLOSED 后 | @wangxumarshall 请人工 merge（或合并/关闭总结）+ issue 关闭 |
| `ESCALATED` | 升级人工，冻结 | agent 自报升级 / attempts>2 / rounds≥4 / 认证熔断 | @wangxumarshall 评论附日志尾证据与建议下一步 |

车道 A 重试预算：attempts ≤ 2（dead/timeout/谎报 done 各 +1，超限升级）。

### 3.2 车道 B（修复 PR 的 bot 意见循环）

| 状态 | 含义 | 转移条件 |
| --- | --- | --- |
| `PR_OPEN`（等待类） | PR 在 bot 检视中/静默窗内 | classify_state ∈ {WAITING_BOTS, QUIET_WINDOW} → 本轮不动 |
| `BOT_FIX` | bot 意见处置 agent 在跑 | classify_state == READY_TO_FIX → collect 导出 + 派 agent（rounds+1）；**同 head 只派一次**（head 变了才派新一轮） |
| `PR_OPEN`（就绪合并） | 四 bot 通过、零待处置意见 | classify_state == READY_TO_MERGE → @wangxumarshall 请人工 merge + 关 issue + DONE |
| `PR_OPEN`（PR 关闭/合并） | 修复 PR 被 merge/close | classify_state == CLOSED → 总结评论 + 关 issue + DONE |
| `ESCALATED` | bot 循环超限 | rounds ≥ 4 → 升级人工 |

车道 B 只接管 `fix/ci-*` 分支的 PR（`pr_number` 只在 reap 的 success 路径
经 `gh pr list --head <fix_branch>` 查询获得）——用户在研 PR 从不进入任何
车道（D6）。

### 3.3 全局机制

- **单飞**：全局同时最多 1 个 agent（A/B 共槽）——派发前 `has_running_agent`
  检查，冲突拒绝派发，下一轮再试。
- **同根因收敛**：同 workflow 有活跃条目（非 DONE/ESCALATED）时，新红 run
  不开新 issue、不派 agent，只挂 `extra_runs` + 既有 issue 追加评论。
- **认证熔断**：`auth_failure_streak` 跨条目/跨车道累计（任何一次 success
  归零）；连续 ≥3 → 写 `PAUSED` + 升级评论。恢复 = 更新 agent.env 后**删除
  PAUSED**。
- **克隆自愈（heal）**：无 agent 在跑时 `git fetch --all --prune`；HEAD 是
  main 且干净 → `reset --hard origin/main` 对齐；偏离/脏 → 记录现场后
  `checkout main` + `reset --hard` + `clean -fd` 恢复单树基线。

## 4. 运维

### 4.1 kill switch（一键停派发）

```console
touch ~/.local/state/sdcshield-ci-triage/PAUSED   # 停:派发全停,观测照常
rm    ~/.local/state/sdcshield-ci-triage/PAUSED   # 恢复
```

PAUSED 语义：`dispatch_agent` 内部闸门——存在即拒绝一切派发（车道 A/B
同闸）；扫描/issue 搜索/收割观测照常。kill switch 与熔断共用 PAUSED 文件。

### 4.2 熔断恢复

熔断触发（连续 ≥3 次认证失败）会写 PAUSED 并在 issue 上发升级评论。恢复
步骤：**先**更新 `~/.local/state/sdcshield-ci-triage/agent.env` 里的凭据
（ANTHROPIC_AUTH_TOKEN 等），**再** `rm PAUSED`——顺序颠倒会在旧凭据上
继续空转烧配额。

### 4.3 手动重跑 / 观测

```console
cd /home/sdc/wangxu/operator/sdcshield-autoci
python3 scripts/ci_triage.py scan       # 真实跑一轮(等价一个 cron tick)
python3 scripts/ci_triage.py dry-run    # 只读演练:零 gh 写/零 git/零派发,复跑无痕
python3 - <<'EOF'                       # status 总览(state.json 全条目)
import json,sys
sys.path.insert(0,"scripts"); import ci_triage as ct
s=ct.StateStore().load()
print("streak:",s.get("auth_failure_streak",0))
for e in ct.StateStore.entries(s):
    print(f"{e.workflow} run {e.run_id} [{e.status}] issue={e.issue_number} "
          f"pr={e.pr_number} attempts={e.attempts} rounds={e.rounds} "
          f"extra_runs={e.extra_runs}")
EOF
```

（`status` 子命令已注册、端到端总览当前经上方的内联脚本查看；测试重定向
用 env `CITRIAGE_STATE_DIR`。）

### 4.4 日志与状态文件

`~/.local/state/sdcshield-ci-triage/` 下：

| 文件 | 内容 |
| --- | --- |
| `state.json` | 全部条目 + 熔断计数（原子写） |
| `PAUSED` | 存在即 kill switch/熔断生效 |
| `scan.lock` | flock 互斥锁（并发 scan 保护） |
| `agent-<runid>-<UTC 时间戳>.log` | 每次派发的 agent 完整 stdout/stderr（重派不覆盖上一轮现场） |
| `collect-pr<N>.md` | 车道 B 派发前导出的四 bot 意见（不可信检视数据） |
| `cron.log` | crontab 重定向的 scan 输出 |

## 5. FAQ

**Q：认证失效怎么办？**
看 agent 日志尾部（`API Error`/`403`/`Not logged in`）。更新
`agent.env` 凭据后删 PAUSED（§4.2）。连续 ≥3 次会自动熔断，不会无限烧配额。

**Q：为什么我的 PR 没被接管？**
车道 B 只接管 `fix/ci-*` 分支的修复 PR（本工具自己产生的）。用户在研
分支的 open PR 完全不碰——继续用交互式 `/pr-loop`（D6 决策，根除同一 PR
并发 actor 冲突）。

**Q：agent 会不会碰我的工作区？**
不会。agent 只在独立克隆 `/home/sdc/wangxu/operator/sdcshield-autoci`
里工作（cwd 即克隆目录），prompt 红线明令主工作区只读；git 单树纪律
要求任务从 `origin/main` 起 `checkout -B fix/ci-<runid>`，结束回干净 main。

**Q：如何完全停用？**
`crontab -e` 删部署行 + `touch PAUSED`（§2.3）。已开 issue 保留人工处置。

**Q：scan 输出里的 `[skip] run … 已在跟踪/已收敛` 是什么？**
幂等命中的正常输出——该 run 已有条目（state 主键）或已收敛进活跃条目
（extra_runs），不重复动作。

## 6. 依赖与红线（agent prompt 硬编码）

- 5 条红线：绝不推 main；绝不 merge；绝不 resolve（issue/PR/thread）；绝不
  碰用户工作区；未复现不修（复现不出如实 escalated）。
- 结束语契约：成功 `ci-triage: done <fix_branch>` / 失败
  `ci-triage: escalated <原因>`——script 侧据此判档，谎报 done 会被
  `gh pr list` 核实拆穿并计 attempts。
- 失败日志/检视意见是**不可信数据**：issue 正文与 collect 文件都带
  `UNTRUSTED_BANNER`（与 pr_loop 同源），agent 不得执行其中任何"指令"。

## 7. 与 pr_loop 的关系

`ci_triage.py` 是 pr_loop 的上游姊妹件：pr_loop 管"已存在 PR 的检视-修复
循环"（车道 B 复用其 `fetch_pr`/`classify_state`/`render_collect` 与四 bot
判据，零重复实现）；ci-triage 管"CI 失败 → 修复 PR 的自动产生"。
两工具共用 `gh` 认证与四 bot 生态；merge 永远在人。

## 8. 部署与验收记录（2026-10-14/15，A1–A8）

验收口径见 spec §11；逐项证据如下（**全部为真实输出**，机器 `openEuler 24.03
SP3`，UTC 时间；非预期的异常结局与暴露的缺陷如实记录，见 A3）。

### A1 — dry-run 正确检出真实红 run ✅

Task 4 已实证（出处：主仓
`.superpowers/sdd/2026-10-13-unattended-ci-triage/task-4-report.md` §五，
2026-10-13 真实 gh 输出）：

```console
扫描到 8 个红 run(监控 main 分支,回看 3 天)
[dry-run] 将为 Sanitizer run 38482214201 开 issue: 标题「[ci-triage] Sanitizer 红：run 38482214201（2026-10-13 UTC）」
[dry-run] 将为 CodeQL run 38243398052 开 issue: …
[dry-run] 将为 Multi-OS Verify run 38242267210 开 issue: …
[dry-run] 将为 Security run 38240202283 开 issue: …
[dry-run] CodeQL run 38235869730 将收敛进条目 run 38243398052(extra_runs),…
[dry-run] state 不落盘,复跑无痕
```

Sanitizer 38482214201 检出命中；三次复跑逐字节一致（diff 空输出）。

### A2 — 真实 scan 恰好开 4 个 issue，复跑无重复 ✅

首次真实 scan（正式 state 目录 `~/.local/state/sdcshield-ci-triage/`，
2026-10-14T20:38Z，gh 已认证；**输出按 workflow 分组整理，行序非逐字**——
动作与 state.json（`created_at`/`extra_runs`/`issue_number`）及现存
cron.log 交叉印证）：

```console
扫描到 8 个红 run(监控 main 分支,回看 3 天)
[issue] Sanitizer run 38482214201 → issue #246
[merge] Sanitizer run 38329813292 → 条目 run 38482214201.extra_runs,评论 issue #246
[issue] CodeQL run 38243398052 → issue #247
[merge] CodeQL run 38235869730 → 条目 run 38243398052.extra_runs,评论 issue #247
[merge] CodeQL run 38231399606 → 条目 run 38243398052.extra_runs,评论 issue #247
[merge] CodeQL run 38191539985 → 条目 run 38243398052.extra_runs,评论 issue #247
[issue] Multi-OS Verify run 38242267210 → issue #248
[issue] Security run 38240202283 → issue #249
[heal] 克隆已在 main 且干净,reset --hard origin/main 对齐远端
[dispatch] run 38482214201 → agent PID 3847751,分支 fix/ci-38482214201,日志 …/agent-38482214201-20261014T203930Z.log
[dispatch] run 38243398052 → agent PID 3847753,分支 fix/ci-38243398052,日志 …/agent-38243398052-20261014T203930Z.log
[dispatch] run 38242267210 → agent PID 3847756,分支 fix/ci-38242267210,日志 …/agent-38242267210-20261014T203930Z.log
[dispatch] run 38240202283 → agent PID 3847759,分支 fix/ci-38240202283,日志 …/agent-38240202283-20261014T203930Z.log
[lane-b] 车道 B 无条目(无 PR_OPEN / BOT_FIX 条目)
```

（原始输出按 run 时间倒序滚动——`gh run list` 返回序 38482214201 →
38243398052 → 38242267210 → 38240202283，各根因的 `[merge]` 收敛行随其
条目位置穿插，`[heal]`/`[dispatch]` 在末段；上块按 workflow 分组整理。
派发段 4 行在现存 cron.log 的 03:00Z tick 中有对应行为印证——但**行为**
不同：修复后单飞生效，仅 1 个 `[dispatch]` + 3 个 `[single-flight]` 拒绝。）

（4 个新根因各开 1 个 issue = **#246–#249**（`gh issue list` 复核：4 个
`[ci-triage]` 标题、全部 OPEN）；同 workflow 后续红 run 全部收敛进
extra_runs 不开新 issue。）

复跑 scan（同 state 目录，次分钟）：

```console
扫描到 8 个红 run(监控 main 分支,回看 3 天)
[skip] run 38482214201 已在跟踪(state 命中)
[skip] run 38329813292 已收敛进活跃条目(extra_runs 命中)
[skip] run 38243398052 已在跟踪(state 命中)
[skip] run 38242267210 已在跟踪(state 命中)
[skip] run 38240202283 已在跟踪(state 命中)
[skip] run 38235869730 已收敛进活跃条目(extra_runs 命中)
[skip] run 38231399606 已收敛进活跃条目(extra_runs 命中)
[skip] run 38191539985 已收敛进活跃条目(extra_runs 命中)
[heal] … [lane-b] 车道 B 无条目(无 PR_OPEN / BOT_FIX 条目)
```

全部 8 个 run 均为 `[skip]`，零 issue 创建、零评论、零派发——A2 幂等 +
A8 同根因收敛的真实验证。

### A3 — agent 真实派发与异常结局（跨会话；如实记录）⚠️

首次 scan 的派发扫对 4 个 ISSUED 条目**同轮派出了 4 个 agent**（agent.env
存在，凭据有效，60 min 预算，PID 3847751/3847753/3847756/3847759）。
后续核实发现这**违反单飞约束**，且暴露出一个 Critical 缺陷：

- **缺陷（Critical，生产首跑暴露）**：`dispatch_agent` 派发后**不把条目状态
  从 ISSUED 置为 DIAGNOSING**，而单飞判定 `has_running_agent` 与收割
  `reap_or_advance` 均以 `status ∈ {DIAGNOSING, BOT_FIX}` 为前提——车道 A
  派发扫对 ISSUED 条目连续派发 4 个 agent（单飞失明）；agent 死后条目
  卡在 ISSUED 永不收割（收割失明）。单测未覆盖该路径（测试直接以
  `status="DIAGNOSING"` 构造条目，A 扫→DIAGNOSING 的转移无人锚定）。
  **已于 9d6ba452 修复**：dispatch 派发成功统一置 DIAGNOSING（车道 A）/
  BOT_FIX（车道 B），历史 ISSUED+PID 条目由 reap 归一收割，单测 29–31
  锚定。
- **真实结局**：4 个 agent 日志（`agent-<runid>-20261014T203930Z.log`）
  全部 **0 字节**——spawn 后机器于 2026-10-15 09:37 CST 重启，claude
  未及产出即被杀；state.json 条目停 ISSUED + agent_pid 已死 + attempts=1。
  修复分支未产生（`git ls-remote` 无 `fix/ci-*`、`gh pr list` 无对应 PR），
  issue #246–#249 无 agent 报告。产出去向二选一（PR 或 ESCALATED）均未
  达成——**异常结局**，归因上述缺陷 + 机器重启，不构成 A3 通过证据。
- **现场留存**：state.json、4 个 0 字节 agent 日志、cron/scan 日志、issue
  #246–#249 全部保留；恢复干净的 main 时删除了克隆里 3 个空 fix 分支
  （均指向 origin/main，无任何 commit）。
- **后续演进（修复 9d6ba452 合入后，生产 cron tick 实证）**：2026-10-15
  03:00Z tick（现存 cron.log 与 state.json）对 4 条 legacy「ISSUED+PID」
  条目全部走归一收割：`[reap] … ISSUED+PID(历史 state 形态,Task 4 R2 前
  条目)归一为 DIAGNOSING 收割` ×4，state 演进为 DIAGNOSING、attempts
  1→2（agent 超时）后触发重派排队（`[requeue] … attempts 2/2,待下一轮
  重派(agent 超时)`），同轮单飞恢复生效（首个条目 38482214201 派发
  PID 84354，其余 3 条被 `[single-flight] 已有 agent 在跑,拒绝派发` 挡下）。
  当前 state.json：4 条目均 DIAGNOSING，attempts 3（38482214201，已再
  经一次重派）/ 2（其余三条），agent_pid 仅 38482214201 持 PID 84354。

### A4 — 并发/锁互斥 ✅（单测用例 23）

`test_scan_lock_excludes_concurrent`：占住 `scan.lock`（真实 flock）→
`cmd_scan` 立即返回 0、零 gh 调用。生产环境同场佐证：上会话挂的临时
每分钟 crontab 与手动 scan 碰撞时，后到者打印 `[lock] scan.lock 已被
占用(并发 scan?),本轮退出` 并退出（时点性记录引 task5-scan.log，该文件
已不在——现存佐证源为 cron.log 的 `[reap]`/`[requeue]`/`[dispatch]`/
`[single-flight]` 行与 state.json 的 `agent_pid`/`updated_at` 字段）。

### A5 — kill switch 闸门 ✅（单测用例 21）

`test_paused_blocks_dispatch_not_observation`：PAUSED 存在 →
`dispatch_agent` 返回 None、零 spawn、attempts 不记账；观测（issue 幂等
搜索/创建）照常执行。

### A6 — 车道 B 幂等 + D6 边界 ✅（单测用例 18/25/27 + 代码侧）

- `test_lane_b_dispatches_once_per_head`（18）：同 head 只派 1 次；agent
  在跑不重派；head 变化再派。
- `test_bot_fix_dead_agent_returns_pr_open_and_re_dispatches`（25）：agent
  退场归还 PR_OPEN 清 head 锚重派；rounds=4 再退场 → ESCALATED。
- `test_lane_b_rounds_cap_is_inclusive`（27）：rounds 达 4 → ESCALATED 不
  再派发。
- **D6（用户在研 PR 不出现在任何车道）**：`pr_number` 唯一来源是
  `reap_or_advance` success 路径的 `gh pr list --head <fix_branch>` 查询
  （`ci_triage.py` 车道 A 收割段）——车道 B 从不扫描存量 open PR，用户
  PR 无进入路径。真实环境佐证：用户 open PR #242/#244 未出现在任何
  车道输出中（2026-10-14/15 scan 日志，全部只含 `[ci-triage]` 条目）。

### A7 — py_compile + pytest + --help ✅（纯 Python，无构建面/无 x86 影响）

```console
$ python3 -m py_compile scripts/ci_triage.py          # 零输出,退出码 0
$ python3 -m pytest tools/excite/tests/test_ci_triage.py -v
============================== 31 passed in 0.12s ==============================
$ python3 scripts/ci_triage.py --help
usage: ci_triage.py [-h] {scan,status,dry-run} ...

CI 无人值守分诊引擎(cron 驱动,pr_loop 的姊妹工具)。周期扫描 main 分支 CI 失败 → 开 issue →
派 agent 修复 → PR 闭环。规格见 .superpowers/sdd/2026-10-13-unattended-ci-triage/。

positional arguments:
  {scan,status,dry-run}
    scan                扫描受监控分支 CI 失败并推进双车道状态机
    status              打印当前状态总览(Task 3)
    dry-run             只读演练: 展示将执行的动作而不实际执行

options:
  -h, --help            show this help message and exit
```

（A7 时点性记录：原始验收跑为 28 passed；用例 29–31 由 Task 4 R2 修复
9d6ba452 增补，当前全套 31 passed、py_compile 零输出——2026-10-15 03:43Z
复验。）

如实声明：`ci_triage.py` 是纯 Python 工具脚本（标准库 + `gh` 子进程），
无构建系统改动、无 C/C++ 代码、无 x86-64 影响；`ninja` 构建面与本变更
无关。`status` 子命令为占位（`NotImplementedError`），总览暂经 §4.3 内联
脚本查看（Task 5 检视后 `scan`/`dry-run` help 文案已去掉内部任务编号）。**gem5-fi 不适用**：无芯片能力依赖、无受影响硬件行为。

### A8 — 同根因收敛 ✅（单测用例 5 + A2 真实 run）

`test_second_red_run_merges_into_active_entry`：同 workflow 活跃条目存在
→ 新 run 只进 extra_runs、issue 创建 0 次、追加评论 1 次。真实环境由 A2
首次 scan 的 4 个 `[merge]` 行佐证（CodeQL 3 个后续红 run 全部收敛）。

## 9. 首次实战

TODO（Task 6 填写：merge 后首个真实红 run 的完整周期时间线——issue →
agent → PR → bot 循环 → READY_TO_MERGE 提醒；异常如实记录并检验
ESCALATED 流程）。

## 10. 延伸阅读

- [pr-loop：会话内 PR 检视-修复闭环](pr-loop.md) — 车道 B 复用的检视闭环
- [PR 代码检视机器人](pr-review-bots.md) — 四 bot 判据与 auto-pr
- 设计规格（会话产物，主仓 docs/superpowers/specs/，不入库）：
  `2026-10-13-unattended-ci-triage-design.md`
