# task-loop：统一任务编排（issue 驱动开发闭环）

**定位**：统一任务编排引擎——**GitHub issues（`agent-task` 标签）是唯一任务队列**。
四类触发源（CI 红 run / main 定期对抗审查 audit / 会话 enqueue / GitHub 手建
issue）统一入队 → scan tick 按标签资格 FIFO 出队 → **单飞** headless claude
agent 在独立克隆里修复/实现/验证/push → `fix|feat/issue-<N>` 分支 → agent
自开 PR（`Closes #N`）→ 复用 [pr-loop](pr-loop.md) 五 bot 检视闭环 →
READY_TO_MERGE 时 @人工 merge。取代 `ci_triage.py` 的编排职责（经模块别名
薄壳过渡，PR-5 删除）；CI 红从唯一触发源降级为四分之一。**人工点仅两类：
merge 永远人工；feature 类需求确认（`agent-approved`）**。

- 引擎：[`scripts/task_loop.py`](../../scripts/task_loop.py)（Python 3.11
  标准库 + `gh` CLI，零第三方运行时依赖；`import pr_loop` 复用 PR 判定，
  `import pr_local_review` 复用对抗审查引擎）
- 前代手册（历史保留）：[ci-triage.md](ci-triage.md)——cron 编排/双车道/
  熔断语义的搬移来源，本文是其泛化后继
- 测试：`tools/excite/tests/test_task_loop.py` +
  `tools/excite/tests/test_ci_triage.py`（经薄壳，锚定搬移零语义变化），
  共 111 用例离线锚定全部关键语义；全套 `tools/excite/tests/` 238 passed
- 设计规格（会话产物，不入库）：主仓
  `docs/superpowers/specs/2026-10-29-task-loop-design.md`

| # | 触发源 | 自动/手动 | 触发机制 | 人工点 |
|---|---|---|---|---|
| 1 | CI 红 run | 自动 | cron 每 30min `scan` → `gh run list` 扫 main 红 run（回看 3 天） | 0（除 merge） |
| 2 | main 定期对抗审查 | 自动 | cron 每 6h `audit` → main 增量 commit → 对抗审查 → findings 建 bug issue | 0（除 merge） |
| 3 | 会话内 review 发现 | 手动发起，入队后全自动 | 会话 findings JSON → `enqueue-bug`（`/dev-loop`） | 0（除 merge） |
| 4 | 用户新功能需求 | 手动发起 | 会话起草 issue 正文给用户过目 → `enqueue-task` → `approve` | 1：需求确认 |
| 5 | GitHub 手建 issue | 手动 | 模板建 issue + 打 `agent-task`（bug 即入队；feature 再打 `agent-approved`） | 1（feature） |

## 1. 架构一览

```
cron */30                          cron 每 6h(17 分)
  └─ task_loop.py scan               └─ task_loop.py audit(audit.lock 独立)
       ├─ flock scan.lock(单实例;          ├─ cursor(上次审到的 SHA;首跑回看 24h)
       │  拿不到锁本轮退出)                ├─ git fetch + log cursor..origin/main
       ├─ 首启动迁移(旧 state 目录→新,§3)   │  无新 commit → 退出
       ├─ 标签渠道就位(幂等补 agent-*)      ├─ 复用 pr_local_review 对抗审查引擎
       ├─ PAUSED 检查(存在→派发全停)        │  (即用即弃 worktree,不占单飞槽)
       ├─ intake_ci: main 红 run →         ├─ findings hash 双源查重
       │  幂等建 issue → QUEUED             │  (state audit_seen + gh body marker)
       ├─ 收割 reap_or_advance             └─ 未见过 → 建 bug issue(入队)
       ├─ reconcile(队列对账): GitHub open
       │  +agent-task 且资格达成 ∖ state → QUEUED;
       │  条目 issue 被人工关闭 → HUMAN_CLOSED
       ├─ heal: 恢复干净的 main(单树基线)
       ├─ ci 重派扫(DISPATCHED 无 PID)
       ├─ 任务 FIFO 派发扫(QUEUED 全部 source,按 issue 号)
       │    → 单飞派发 headless claude(60 min 预算,prompt 经 stdin)
       │    → fix/ci-<runid> 或 fix|feat/issue-<N> 分支
       └─ 车道 B: PR_OPEN/BOT_FIX → pr_loop.classify_state(五 bot 判据)
            WAITING_BOTS/QUIET_WINDOW → 不动;READY_TO_FIX → collect 导出+派
            bot 处置 agent(rounds≤4,同 head 只派一次);判定前补第 5 bot 信号;
            READY_TO_MERGE/CLOSED → @人工 merge + 关 issue + DONE
```

**scan tick 顺序**（实码序，`_run_scan_orchestration`）：flock `scan.lock`
（LOCK_EX|LOCK_NB，拿不到打印退出码 0）→ 首启动迁移（仅非 dry）→ 标签渠道
就位（仅非 dry，失败仅告警）→ PAUSED 检查（派发全停、观测照常）→
intake_ci（`scan_red_runs` 回看 3 天 → `triage_new_red_runs`：幂等建/复用
issue → QUEUED；同 workflow 活跃条目 → 挂 `extra_runs` 追加评论不开新
issue，同根因收敛）→ 收割 `reap_or_advance` → 队列对账
`reconcile_issues`（资格达成的 manual issue 入队 + HUMAN_CLOSED 清扫——
清扫以对账拉取的 open 列表预筛，列表内条目零 `gh issue view`，离列表才
逐条复核一次，PR#317 检视#13 去掉了每条每 tick 的 N+1 探查）→
`heal_clone` →
ci 重派扫（DISPATCHED 且无 PID）→ 任务 FIFO 派发扫（QUEUED 全部
source，按 issue 号排序；两扫先经 `_dispatch_gate_ok` 标签资格复查，
见 §2）→ 车道 B `lane_b_advance` → state 原子落盘
（每次成功派发即时落盘，检视#6；finally：GhError 中途崩溃也落盘已推进
部分；dry-run 不落盘）。

**audit 独立线**（`run_audit`，与 scan 并行不互斥）：

- 独立 `audit.lock`（flock）——**不占修复单飞槽**（read-only 审查与第 5 bot
  `pr_local_review --all-open` 同级定位）；PAUSED 同闸（熔断闸同样管 audit
  的 claude 调用）。
- cursor：state 的 `audit_cursor`（上次审到的 SHA + 时间）；无 cursor 首跑
  回看 24h。`git fetch` 后 `cursor..origin/main` 无新 commit → 本轮退出。
- **单轮 diff 上限（轮5 B48）**：区间 diff 超 3000 行时从**最旧侧切块**，
  本轮只审一块、cursor 只推进到块基，余量下一轮（6h）续审——PAUSED 长
  挂恢复后 base 停在数周前的超大区间不会再因 claude 上下文溢出（空输出/
  畸形 → strict 抛错 → cursor 不推进）而每 6h 重复烧配额死循环；单个
  commit 自身超限时仍整块审查（尽力，不卡死在它身上）。
- 审查复用 `pr_local_review` 对抗审查引擎（prompt 泛化：diff + commit
  messages 替代 PR context），在即用即弃 worktree 里跑，绝不碰用户工作区。
- findings 逐条 hash 查重（`path+line+category+message` 归一化的 sha256
  前 16 hex；evidence 不进键）：**双源**——state `audit_seen` 集合 +
  `gh search issues` body marker `<!-- task-loop:audit:<hash> -->`。未见过
  才建 bug issue（labels `agent-task agent-bug bug`，正文含 marker）。
  findings 入库中途 GhError → 本轮零落盘，下轮靠 gh body marker 查重兜底
  （双源设计的意义）。
- **state 写入纪律（Ruling 11）——为什么走 `_audit_state_merge_write`**：
  audit 只持 audit.lock，而 claude 运行数分钟期间并发 scan tick（持
  scan.lock）会推进 entries（派发记账/收割/对账）——若把 audit 启动时
  load 的整份快照 save 回去，会把 scan 的推进整体回滚：丢 agent_pid/状态
  → 单飞失明 → 同一克隆重复派 agent（生产事故 9d6ba452 同类根因）。故
  audit 的一切持久化都经 `_audit_state_merge_write`：拿 scan.lock（阻塞式
  flock——audit 自己的临界区只有一次 load+save，亚秒级；scan 从不碰
  audit.lock，锁序单向无死锁）后 load 磁盘**最新** state，只 update audit
  自有键（`audit_cursor`/`audit_seen`/`auth_failure_streak`），再原子 save。
  **entries 的权威属于 scan，audit 永不写它**。认证失效计数
  `_audit_streak_add` 同一纪律（scan.lock 内对磁盘最新值读-改-写）。
  **等待时长的如实声明（轮6 #51）**：「亚秒级」说的是 audit 自己的临界区；
  **等 scan 放锁**的时长可达一个 scan tick 的全长——scan 持锁期间可在
  lane_b 内同步跑本地 claude 审查（至多 1h），audit 此时会阻塞等待且持着
  audit.lock（下一 audit tick 锁忙退出，无死锁、无 state 损坏）。部署行
  的 `timeout 7500` 把这一等待封顶（被杀点若在落盘前 = 设计好的「本轮零
  落盘」语义，findings 查重由 gh body marker 兜底，下轮重试）。

**关键约束**（全部由单测锚定）：全局同时最多 **1 个修复 agent**（跨全部
source 共槽；audit 不占槽）；attempts ≤2、rounds ≤4、认证连续 ≥3 →
PAUSED 熔断；第 5 bot（local-claude）连续 3 tick 缺位仅告警不降门槛。

## 2. 标签协议（Copilot assignment 模式）

六个 `agent-*` 标签，由 `ensure_agent_labels()` 每 tick 幂等创建缺失项
（颜色统一 `d4c5f9`；创建失败仅告警不阻断）：

| 标签 | 语义 |
|---|---|
| `agent-task` | 任务资格（intake 自动打 / 手动打即入队） |
| `agent-bug` | bug 类 → 自动派发 |
| `agent-feature` | feature 类 → 需 `agent-approved` 才派发 |
| `agent-approved` | 人工确认（`approve` 子命令或 GitHub 手动） |
| `agent-wip` | agent 已认领（派发时打，重派/升级/终态时摘） |
| `agent-escalated` | 升级（attempts/rounds 超限） |

- **资格判定公式**：open ∧ `agent-task` ∧ (`agent-bug` ∨ `agent-approved`)
  ∧ ¬`agent-wip` ∧ ¬`agent-escalated`，FIFO 按 issue 号。
  公式在**入队与每次派发**双时刻生效（PR#317 检视#12）：派发扫（任务首派
  与死 agent 重派）以对账 tick 拉取的 open+agent-task 列表复查当前标签
  （复用对账的那一次 `gh issue list`，零额外调用）——人工对已入队 issue 补
  `agent-escalated`、摘 `agent-task`/`agent-approved` 即冻结（保持
  QUEUED/DISPATCHED，不派发、不改状态），恢复标签后自动续派；摘
  `agent-task` 但仍 open 的条目同样冻结（不误判 HUMAN_CLOSED）。边界：
  open 列表 `--limit 100`（与 manual 入队同限），超限的 open issue 不在
  快照内会被冻结——设计队列规模（单仓库同时活跃任务数）远小于此。
  **控制面边界（PR#317 检视#27/#31/#37 轮3-4）**：① ci 源条目**整体**
  豁免"不在 open+agent-task 列表"的成员资格冻结（论据是 v1 存量 ci
  issue 由旧 `create_issue` 创建、永不在快照内；新 ci issue 同样适用，
  无日落条件——代价是"摘 agent-task"对 ci 条目不构成冻结手段）；ci
  条目的冻结面 = 关闭 issue（HUMAN_CLOSED）或全局 PAUSED。② 车道 B 的 bot 处置
  派发（PR_OPEN→BOT_FIX）**不**复查标签资格——对在检 PR 冻结 bot
  处置的手段是关闭 issue 或 PAUSED，不是打标签。
- intake 自动打的标签：ci 源 `agent-task agent-bug`；audit 源
  `agent-task agent-bug bug`；会话 bug `agent-task agent-bug bug`；会话
  feature `agent-task agent-feature enhancement`。现有 bug/enhancement
  模板标签保留不冲突。
- **agent-wip 同步时机**：派发成功即打、重派等待期/升级/终态时摘。标签是
  跨 tick 与 state 丢失场景的**建议性恢复线索，state 为权威**——同步失败
  只告警，绝不阻断派发/收割记账。

## 3. 部署

### 3.1 终态 crontab（两行）

```cron
*/30 * * * * mkdir -p ~/.local/state/sdcshield-task-loop && cd /home/sdc/wangxu/operator/sdcshield-autoci && /usr/bin/python3 scripts/task_loop.py scan >> ~/.local/state/sdcshield-task-loop/cron.log 2>&1
17 */6 * * * mkdir -p ~/.local/state/sdcshield-task-loop && cd /home/sdc/wangxu/operator/sdcshield-autoci && timeout 7500 /usr/bin/python3 scripts/task_loop.py audit >> ~/.local/state/sdcshield-task-loop/audit.log 2>&1
```

（audit 取 17 分——避开整点扎堆；scan 行沿用 */30 既有节奏仅换入口。
**`mkdir -p` 前缀是硬要求**：shell 在命令启动前打开重定向目标，父目录
缺失时 `>>` 直接失败、命令体根本不执行——而创建该目录的又恰是这个
python 进程本身（`_run_scan_orchestration`/`run_audit` 启动时的
makedirs 与首启动迁移）。没有此前缀，换行即鸡生蛋死锁：首个 tick 静默
死亡，`tail cron.log` 只会看到文件不存在。`~` 由 cron 的 `/bin/sh -c`
作波浪线展开，与既有行同机制。）
安装：`crontab -e` 把既有 `ci_triage.py scan` 行替换为上行 scan、新增
audit 行——`mkdir -p` 前缀使换行**无时序依赖**（不必等首个经薄壳的
scan 建好新目录）。**cron PATH 注意（轮5 会话#6 如实声明）**——cron
环境只有 `/usr/bin:/bin` 量级裸 PATH，故用显式 `/usr/bin/python3`。
**引擎自身的 gh 调用（label ensure / run list / issue create·search·close
/ pr list）直接跑在 cron 的 PATH 上、从不 source agent.env**——`gh` 必须
对 cron PATH 可达（本机 gh 在 `~/.local/bin`，靠 crontab 顶部一条
`PATH=` 头提供；fresh 机器/新 crontab 若无该头，每 tick 会在第一个 gh
调用处 exit 1）。agent 派发所需的 `claude`/`gh` 由 agent.env 内的
`PATH` 提供（dispatch 命令先 `source agent.env` 再起 claude）——两个
PATH 需求相互独立，缺一不可。

**部署时序依赖（如实声明）**：autoci 克隆跟踪 main——本 PR 合入**前**，
crontab 的 `ci_triage.py scan` 走 main 上的旧文件，不受影响；合入
**后**，main 的 `ci_triage.py` 已是模块别名薄壳（`import task_loop`），
旧 cron 行自动经壳切到新引擎（首个非 dry scan 触发下述 state 目录迁移，
输出仍重定向到旧目录 cron.log，无害）；**cron 行换成 `task_loop.py` +
新增 audit 行是人工步骤**——audit 线在人工添加前不启动。第 5 bot 时间
触发线（`pr_local_review.py --all-open`，主工作区）不属于本引擎 cron，
见 [ci-triage.md](ci-triage.md) §2.2，切换不受影响。

### 3.2 首启动自动迁移（state 目录）

`~/.local/state/sdcshield-ci-triage/` → `~/.local/state/sdcshield-task-loop/`，
`_migrate_legacy_state_dir()` 在 scan/audit 拿到锁后执行（仅非 dry）：

- 触发条件：生效 state 目录为默认新目录（env 重定向的测试 run 不迁——
  不动真实目录）∧ 新目录无 `state.json` ∧ 旧目录有；
- 动作：拷贝 `state.json` + `agent.env`（目标已存在则跳过该文件）；
  `state.json` v1→v2 在**加载时**归一——旧状态名 DETECTED/ISSUED→QUEUED、
  DIAGNOSING→DISPATCHED，`source`/`kind`/`audit_hash` 缺省回落
  ci/bug/None；
- 旧目录**保留归档不动**；`PAUSED` 不搬——`_paused()` 对新旧两目录双路径
  检查（过渡期任一存在即生效）；
- 幂等可重跑（单测锚定）。

### 3.3 agent.env（凭据文件，不入库）

`~/.local/state/sdcshield-task-loop/agent.env`（由 §3.2 迁移就位；**全新
部署无 legacy 可迁时须手工创建**——派发侧有前置检查，缺文件会拒绝派发并
在 stderr 提示本节，条目保持 QUEUED 不烧 attempts）：agent
派发前 `source` 的环境快照——`ANTHROPIC_AUTH_TOKEN` /
`ANTHROPIC_BASE_URL` / 模型映射 / `PATH`。权限**必须 600**；内容绝不进
日志/issue/state.json。第 5 bot（`pr_local_review.py`）经 `agent_env_path()`
与本引擎**共用**此文件（env 重定向目录无条件优先，与 `_resolve_state_dir`
同口径——重定向实例绝不用默认实例的凭据；默认实例新目录优先、旧目录
回退）。失效症状 = agent 日志全体 `API Error`/`403`/`Not logged in`，
连续 ≥3 次自动熔断（§5.2；判定只认**死进程临终 500 字符**内的认证
特征——活 agent 复述 issue 内嵌 CI 日志中的认证字样不计入）。

### 3.4 完全停用

```console
crontab -e    # 删两行部署行(停调度)
touch ~/.local/state/sdcshield-task-loop/PAUSED   # 保险:已在跑的 tick 不再派发
```

## 4. 状态机速查

条目主键 `issue_number`（ci 源条目 `run_id` 降为普通字段、`extra_runs`
同根因收敛语义保留）。状态存于 `state.json` 的 `entries[]`：

| 状态 | 含义 | 进入条件 | issue 上发生什么 |
| --- | --- | --- | --- |
| `QUEUED` | issue 就绪待派发 | intake 建好（ci/audit/session）或 reconcile 入队（manual） | 正文含任务上下文 + 幂等标记 + 防注入横幅 |
| `DISPATCHED` | 修复/实现 agent 在跑 | 派发成功（打 `agent-wip`，attempts+1） | （prompt 要求 agent 最终把报告贴 issue） |
| `PR_OPEN` | agent 自开 PR 已核实 | 收割核实 `gh pr list --head` 命中 open PR | 贴 PR 简报评论（车道 A→B 的唯一铰链） |
| `BOT_FIX` | bot 意见处置 agent 在跑 | 车道 B READY_TO_FIX → collect 导出 + 派 agent（rounds+1，**同 head 只派一次**） | 处置报告贴 issue |
| `DONE` | 结案 | 车道 B 判定 READY_TO_MERGE/CLOSED | @wangxumarshall 请人工 merge（或合并/关闭总结）+ 关 issue + 摘 wip |
| `ESCALATED` | 升级人工，冻结 | attempts>2 / rounds≥4 / agent 自报 escalated / 熔断 | @wangxumarshall 评论附日志尾证据与建议 + `agent-escalated` 标签 + 摘 wip |
| `HUMAN_CLOSED` | 人工接管，工具永不再碰 | QUEUED/DISPATCHED 条目 issue 已被人工关闭（无 PR 路径；有 PR 路径的由车道 B 收尾） | 不 @ 不评论（DISPATCHED 先击杀在跑 agent、摘 wip） |

**全局机制**：

- **单飞**：全局同时最多 1 个修复 agent，跨全部 source 共槽——派发前
  `has_running_agent` 检查（PID 探活），冲突拒绝派发下一轮再试；audit
  不占槽。
- **attempts ≤ 2**：dead/timeout/谎报 done（日志称 done 但 `gh pr list`
  查无 PR）各 +1，超限升级；agent 诚实自报 escalated **不计** attempts
  （诚实上报不是失败重试）。
- **rounds ≤ 4**：车道 B bot 意见处置轮数，`rounds ≥ 4` → ESCALATED。
- **认证熔断**：`auth_failure_streak` 跨条目/跨车道/含 audit 累计（任何
  一次 success 归零——防「连续 ≥3」退化成「累计 ≥3」）；连续 ≥3 → 写
  `PAUSED` + 升级评论。
- **同根因收敛**：同 workflow 有活跃条目（非 DONE/ESCALATED/HUMAN_CLOSED
  三终态，与上表一致）时新红 run 不开新 issue，只挂 `extra_runs` + 追加评论；
  HUMAN_CLOSED 是终态——人工已接管的死条目不参与收敛，新红 run 照常开新
  issue（不在已关闭 issue 上评论、不吞失败）。

## 5. 运维

### 5.1 kill switch（一键停派发）

```console
touch ~/.local/state/sdcshield-task-loop/PAUSED   # 停:派发全停,观测照常
rm    ~/.local/state/sdcshield-task-loop/PAUSED   # 恢复
```

PAUSED 语义：`dispatch_agent` 内部闸门——存在即拒绝一切派发（车道 A/B
同闸），audit 的 claude 调用同样被拦；扫描/issue 搜索/收割观测照常。
kill switch 与熔断共用 PAUSED 文件；过渡期旧目录
（`sdcshield-ci-triage`）的 PAUSED 同样生效（双路径检查）。

### 5.2 熔断恢复

熔断触发（连续 ≥3 次认证失败）会写 PAUSED 并在 issue 上发升级评论。
恢复步骤：**先**更新 `~/.local/state/sdcshield-task-loop/agent.env` 里的
凭据（ANTHROPIC_AUTH_TOKEN 等），**再** `rm PAUSED`——顺序颠倒会在旧
凭据上继续空转烧配额。

### 5.3 手动重跑 / 观测

```console
cd /home/sdc/wangxu/operator/sdcshield-autoci
python3 scripts/task_loop.py status            # 队列总览: streak/PAUSED/单飞占用
                                               #   + 全部条目 + 待入队任务
python3 scripts/task_loop.py dry-run           # scan 只读演练: 零 gh 写/零 git/零派发/零本地写
                                               #   (不建状态目录/锁文件),复跑无痕;
                                               #   迁移前(新目录无 state、legacy 有)只读
                                               #   回退加载 legacy 状态预演——与真实首 tick
                                               #   (先迁移再 skip)对齐,不为已跟踪 run
                                               #   虚假预告开 issue/派 agent
python3 scripts/task_loop.py audit --dry-run   # audit 演练: 展示将审查的提交范围,
                                               #   零 claude 调用,state 不落盘,
                                               #   零本地写(不建锁文件)——但
                                               #   git fetch --all --prune 是真实
                                               #   远端引用刷新(克隆内写,检视#39)
```

env 重定向（测试/多实例）：`TASKLOOP_STATE_DIR`（>
`CITRIAGE_STATE_DIR` 兼容旧测试/部署）、`TASKLOOP_CLONE_DIR`（>
`CITRIAGE_CLONE_DIR`）、`TASKLOOP_REPO`（> `CITRIAGE_REPO`）；
`CITRIAGE_WORKFLOWS` 逗号分隔覆盖监控名单（空串视同未设置）。

### 5.4 日志与状态文件

`~/.local/state/sdcshield-task-loop/` 下：

| 文件 | 内容 |
| --- | --- |
| `state.json` | 全部条目 + 熔断计数 + audit cursor/seen（原子写：tmp → os.replace） |
| `PAUSED` | 存在即 kill switch/熔断生效 |
| `scan.lock` | scan 互斥锁（flock） |
| `audit.lock` | audit 互斥锁（与 scan 分离，不占修复单飞槽） |
| `agent.env` | 凭据环境快照（600，§3.3） |
| `agent-<runid 或 issue-N>-<UTC 时间戳>.log` | 每次派发的 agent 完整 stdout/stderr（ci 源按 run_id、其余源按 issue 号命名；重派不覆盖上一轮现场） |
| `prompt-<issue 号>-<UTC 时间戳>.txt` | 派发 prompt 落盘（0600，经 stdin 喂给 claude——修 argv 传 prompt 的 ARG_MAX 隐患；issue 号缺省时用 run_id） |
| `collect-pr<N>.md` | 车道 B 派发前导出的 bot 检视意见（不可信检视数据） |
| `cron.log` / `audit.log` | crontab 重定向的 scan / audit 输出 |

旧目录 `~/.local/state/sdcshield-ci-triage/` 迁移后保留归档（含历史
agent 日志与 cron.log）。

## 6. FAQ

**Q：认证失效怎么办？**
看 agent 日志尾部（`API Error`/`403`/`Not logged in`）。更新 `agent.env`
凭据后删 PAUSED（§5.2）。连续 ≥3 次会自动熔断，不会无限烧配额。

**Q：为什么我的 PR 没被接管？**
车道 B 只接管引擎**自己派生**分支的 PR——`fix/ci-<runid>` 与
`fix|feat/issue-<N>`（`pr_number` 唯一来源是收割 success 路径的
`gh pr list --head <fix_branch>` 查询，车道 B 从不扫描存量 open PR）。
用户在研 PR 完全不碰——继续用交互式 `/pr-loop`（D6 决策，根除同一 PR
并发 actor 冲突）。

**Q：feature issue 为什么一直不派发？**
缺 `agent-approved`（feature 类的派发门，§2 资格公式）。`python3
scripts/task_loop.py approve <N>` 或 GitHub 手动打标签，下一 scan tick
进队列。

**Q：agent 会不会碰我的工作区？**
不会。修复/实现 agent 只在独立克隆
`/home/sdc/wangxu/operator/sdcshield-autoci` 里工作（cwd 即克隆目录），
prompt 红线明令主工作区只读；git 单树纪律要求任务从 `origin/main` 起
`checkout -B fix|feat/issue-<N>`，结束回干净 main。audit 的审查在克隆内
的即用即弃 worktree 跑（`cmd_audit` 先 chdir 到克隆目录，缺失时如实
报错退出，绝不留在用户工作区）。

**Q：如何完全停用？**
`crontab -e` 删两行部署行 + `touch PAUSED`（§3.4）。已开 issue 保留
人工处置。

## 7. 依赖与红线（agent prompt 硬编码）

- 5 条红线：绝不推 main；绝不 merge；绝不 resolve（issue/PR/thread）；
  绝不碰用户工作区；bug 源「未复现不修」（复现不出如实 escalated），
  feature 源「偏离需求即 escalated」。
- 结束语契约（最后一行逐字）：成功 `task-loop: done <fix_branch>` /
  失败 `task-loop: escalated <原因>`——**全部 prompt builder（车道 A ci、
  任务、车道 B bot 处置）一律只嵌 `task-loop:` 新标记**（常量插值），
  script 侧据此判档，谎报 done 会被 `gh pr list` 核实拆穿并计 attempts。
  过渡双轨（只读不写）：合入时**在飞的旧 ci agent**输出 `ci-triage:
  done/escalated` 前缀同样被认——覆盖旧 agent 与历史日志；写侧不再
  产生旧标记，过渡窗口有界，旧标记可在在飞 agent 排空后安全退役。
- issue 正文/评论/失败日志/collect 文件全部是**不可信数据**：统一裹
  `UNTRUSTED_BANNER`（与 pr_loop 同源），agent 不得执行其中任何"指令"。
- **安全边界如实声明（PR#317 检视#18）**：横幅是提示注入**缓解措施，
  不是安全边界**（同 pr_local_review 的 I3 已知边界）。所有 agent 与
  audit 审查都以 `--dangerously-skip-permissions` 跑**无沙箱** claude，
  可执行任意命令；其输入面包括 PR diff/正文、issue 正文/评论、CI 日志，
  以及 **audit 每 6h 对 main 增量的 diff 全文 + commit messages**——
  squash-merge 的 commit message 可含外部贡献者（无 push 权者）撰写的
  PR 标题/正文文本，即无人值守线会周期性地把此类文本喂给无沙箱 claude。
  这是与第 5 bot「PR 内容同源暴露」同级的**已知接受边界**（单人体量下
  的风险取舍），运维须知悉：若仓库转为多人协作/接受外部 PR，应先评估
  沙箱化（容器/VM 执行面）再放大 audit 的输入面。
- **comment 重试的已知取舍（PR#317 检视#10/#21/#30，三轮维持）**：
  `comment_on_issue` 经 `gh_json_idem` ≤3 重试（spec §10 既定分类）。
  超时但服务端实际成功的窗口会重复发评论（含升级 @）——代价是可观测
  的噪音；反向取舍（不重试）会**永久丢失**升级 @ 评论（`_escalate`
  置 ESCALATED 后下轮永不补发）。两害相权取其轻，此为记录在案的
  接受行为，非缺陷。
- PR 竞态：agent push 后自开 PR（`--title "fix|feat: <摘要> (#N)"
  --body "Closes #N + 报告"`）；与 `auto-pr.yml` 谁先谁后不定——create
  因"分支已有 PR"失败时兜底 `gh pr edit` 改写标题与正文（必须含
  `Closes #N`），两条路径终态一致。
- **merge 永远在人**：引擎、agent、命令面都绝不 merge、绝不 resolve。

## 8. 与 pr_loop / ci_triage 的关系

依赖栈（方向不变）：`pr_local_review`（基座：gh 原语 + 本地对抗审查）
→ `pr_loop`（中层：PR 五 bot 状态机）→ `task_loop`（顶层：cron 编排 +
issue 生命周期 + agent 派发收割）。

- **pr_loop** 管"已存在 PR 的检视-修复循环"——车道 B 全量复用其
  `fetch_pr`/`classify_state`/`render_collect`/`ensure_local_review`
  （判定前主动补第 5 bot 信号，幂等），零重复实现。
- **`scripts/ci_triage.py`** 是过渡**模块别名薄壳**（`import task_loop`
  后 `sys.modules["ci_triage"] = task_loop`——保住测试的 monkeypatch
  命名空间）：PR-1a 起全部职责已搬至 task_loop.py，旧 cron 行/旧测试经
  壳零改动继续工作；**PR-5 删除**（cron 切换并观察后）。其历史手册
  [ci-triage.md](ci-triage.md) 保留（含首跑暴露的 Critical 缺陷记录等
  经验教训）。

## 9. 会话命令（/dev-loop）

`/dev-loop`（`.claude/commands/dev-loop.md` + `.trae/rules/dev-loop.md`
同源双份，仿 pr-loop 模式）：会话内把「review 发现的问题」/「功能需求」
一键转成带标签的 issue 进队列。

- `/dev-loop enqueue bug`：把本会话 review findings 整理为 JSON 数组
  （path/line/severity/category/message/evidence，schema 同第 5 bot），
  `python3 scripts/task_loop.py enqueue-bug --findings-file <f>`（一条
  finding 一个 issue，对齐一补丁一单元；每条以 `_audit_hash` 归一化键嵌
  `<!-- task-loop:finding:<hash> -->` marker 并经 `gh search` 查重——
  中途瞬态失败后重跑，已建条目逐条跳过不重复建；跨源与 audit 各自独立；
  单条路径按 spec §8.3「会话源不做自动查重（人发起）」）；或单条
  `--title --body-file`。
- `/dev-loop enqueue task`：用户口述的功能需求——起草 issue 正文（对齐
  `.github/ISSUE_TEMPLATE/feature_request.md`）**给用户过目确认后**
  `enqueue-task --title … --body-file …`；提示 `approve` 后自动进派发
  队列。
- `/dev-loop approve <N>`：`python3 scripts/task_loop.py approve <N>`。
- `/dev-loop status`（缺省）：队列总览。
- **PR 开出后路由到 `/pr-loop` 阶段 2–6**（wait → collect → 批判性处置
  → 循环 → merge 提醒），零重复实现。

## 10. 验收记录（待填）

TODO（PR 合入并切换 cron 后记录）：首个真实任务全周期时间线——issue →
agent 派发 → PR → bot 循环 → merge 提醒；异常结局（如 ESCALATED/
HUMAN_CLOSED 路径）如实记录。切换后观察首个 tick：
`tail -5 ~/.local/state/sdcshield-task-loop/cron.log`。

## 11. 延伸阅读

- [pr-loop：会话内 PR 检视-修复闭环](pr-loop.md) — 车道 B 复用的检视闭环
- [PR 代码检视机器人](pr-review-bots.md) — 五 bot 判据与 auto-pr
- [ci-triage：CI 无人值守分诊（历史手册）](ci-triage.md) — 前代引擎，
  本引擎的搬移来源与生产经验（含首跑缺陷教训）
- 设计规格（会话产物，主仓 docs/superpowers/specs/，不入库）：
  `2026-10-29-task-loop-design.md`
