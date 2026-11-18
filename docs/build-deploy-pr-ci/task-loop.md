# task-loop：统一任务编排（issue 驱动开发闭环）

**定位**：统一任务编排引擎——**GitHub issues（`agent-task` 标签）是唯一任务队列**。
四类触发源（CI 红 run / main 定期对抗审查 audit / 会话 enqueue / GitHub 手建
issue）统一入队 → 按标签资格 FIFO 出队 → headless claude agent 在独立
worktree 里修复/实现/验证/push → `fix|feat/issue-<N>` 分支 → agent
自开 PR（`Closes #N`）→ 复用 [pr-loop](pr-loop.md) 五 bot 检视闭环 →
READY_TO_MERGE 时过 merge 门。取代 `ci_triage.py` 的编排职责（经模块别名
薄壳过渡，PR-5 删除）；CI 红从唯一触发源降级为四分之一。**2026-11-13 起
调度为 nightly pipeline**（spec 2026-11-12-full-auto-pipeline，§1/§3.1）：
cron `0 23 * * *` 拉起 `pipeline` 子命令，23:00–09:00 窗口内三泳道并行 +
merge 门每 ~5min 一轮，09:00 软停、`@reboot` 落窗自愈。**人工保留点**：
feature 类需求确认（`agent-approved`）、紧急停机（`PAUSED` /
`NO_AUTO_MERGE` 熔断）、ESCALATED 回流、用户非 docs PR 的 merge——
**merge 红线已反转**：引擎 PR（`fix/issue-N|fix/ci-N|fix/revert-prN`）与
全 docs-only PR 由 merge 门六门槛全过后自动 squash merge（§7）。

- 引擎：[`scripts/task_loop.py`](../../scripts/task_loop.py)（Python 3.11
  标准库 + `gh` CLI，零第三方运行时依赖；`import pr_loop` 复用 PR 判定，
  `import pr_local_review` 复用对抗审查引擎）
- 前代手册（历史保留）：[ci-triage.md](ci-triage.md)——cron 编排/双车道/
  熔断语义的搬移来源，本文是其泛化后继
- 测试：`tools/excite/tests/test_task_loop.py` +
  `tools/excite/tests/test_ci_triage.py`（经薄壳，锚定搬移零语义变化），
  离线锚定全部关键语义；全套 `tools/excite/tests/` 372 passed（2026-11-13
  实测，含 pipeline 泳道/merge 门/轮次/软增补/终审修复波用例——C1 跨模块
  集成、revert 吸收/主查重、冲突夜计数、检视超时锚）
- 设计规格（会话产物，不入库）：主仓
  `docs/superpowers/specs/2026-10-29-task-loop-design.md`（初代）、
  `docs/superpowers/specs/2026-11-12-full-auto-pipeline-design.md`
  （nightly pipeline 红线反转）

| # | 触发源 | 自动/手动 | 触发机制 | 人工点 |
|---|---|---|---|---|
| 1 | CI 红 run | 自动 | nightly pipeline intake（窗口内每轮循环）：`gh run list` 扫 main 红 run（回看 3 天）→ 幂等建 issue | 0 |
| 2 | main 定期对抗审查 | 自动 | pipeline 内 spawn 的 audit 后台线（并发 1）：main 增量 commit → 对抗审查 → findings 建 bug issue | 0 |
| 3 | 会话内 review 发现 | 手动发起，入队后全自动 | 会话 findings JSON → `enqueue-bug`（`/dev-loop`） | 0 |
| 4 | 用户新功能需求 | 手动发起 | 会话起草 issue 正文给用户过目 → `enqueue-task` → `approve` | 1：需求确认 |
| 5 | GitHub 手建 issue | 手动 | 模板建 issue + 打 `agent-task`（bug 即入队；feature 再打 `agent-approved`） | 1（feature） |

## 1. 架构一览（nightly pipeline，2026-11-13 起）

```
cron 0 23(timeout 39600)                    @reboot scripts/pipeline_selfheal.sh
  └─ task_loop.py pipeline                     （重启落在 23:00–09:00 窗口内
       ├─ flock pipeline.lock(单实例;            则重新拉起 pipeline,锁双保险)
       │  拿不到锁本轮退出)
       ├─ heal_clone: reset --hard origin/main
       │    (自举:昨晚合入的新引擎代码今晚生效;agent 全在独立
       │     worktree 跑,主克隆每轮无条件 heal)
       ├─ 前奏: 首启动迁移(§3.2)/标签渠道就位/清检视每晚计数/启动对账
       └─ 窗口循环: 23:00–09:00 每 PIPELINE_CYCLE_SEC=300s 一轮(cycle_once)
            ├ intake: scan_red_runs(main 红 run,回看 3 天)
            │    → revert 归因(merged_sha 集命中 → revert issue;命中 run
            │      吸收——不再前向修复,防双 agent 互踩)
            │    → triage_new_red_runs(吸收后的红 run 幂等建 issue →
            │      QUEUED;同根因收敛)
            ├ 槽位 janitor(超时判死回收;boot_id 变化期死亡不计 attempts)
            ├ 收割 reap_or_advance / 队列对账 reconcile_issues
            ├ 泳道1 issue-agent ×2 空槽: QUEUED FIFO(资格公式 §2)
            │    → worktree 切分支 → 修复 → 推分支 → 开 PR → PR_OPEN
            ├ 泳道2 pr-review-agent ×2 空槽: 无第 5 bot 信号的 open PR head
            │    → 本地对抗检视发布(每晚每 head ≤2 次,timeout 7500s)
            ├ 泳道3 pr-repair-agent ×2 空槽: READY_TO_FIX 的引擎 PR
            │    → collect 导出 → 意见批判处置 → 新 head
            │      (rounds ≤ 动态上限 2–3,§4)
            ├ audit 后台线(并发 1,timeout 7500s;距窗末 <7500s 不 spawn)
            └─ merge 门扫: 引擎 PR + 用户 docs PR 连续评估
                 → 六门槛全过(§7) → squash merge --delete-branch
                 + 门槛证据审计评论 + merged_at/merged_sha 记账
       09:00 软停: 停新派 → 在跑 agent ≤30min(SOFT_STOP_GRACE_SEC)缓冲,
       超时击杀**不计 attempts**、条目回队次夜续 → worktree 全清
       → 夜报 report-YYYYMMDD.md + state["last_report"] → 退出
       (timeout 39600s 外层兜底)
```

三泳道全部异步、互不阻塞；intake 是循环步而非前置阶段。每次派发前过
**内存闸**：`MemAvailable ≥ 8G`（env `PIPELINE_MIN_MEM_GB` 可调，宁慢勿爆）；
低于阈值本轮不派新，在跑不动、记日志；audit 同过闸。09:00 后到次日
23:00 前**无任何自动 tick**（白天引擎闲置，会话手动用 `scan` 薄壳/`dry-run`）。

**cycle_once 轮内顺序**（实码序，`task_loop_pipeline.cycle_once`，裁定#2
顺序载荷不可破坏——同 key 死槽依赖 janitor 先清，janitor 先于一切派发）：
intake（`scan_red_runs` → `revert_attribution` → `triage_new_red_runs`）→
槽位 janitor → 收割 `reap_or_advance` → 队列对账 `reconcile_issues` →
`heal_clone` → 泳道1 派发（取材含 QUEUED 与「DISPATCHED 且无 agent」，
经 `_dispatch_gate_ok` 标签资格复查，§2）→ 泳道2 检视派发（PAUSED 同闸；
`needs_review_list` + `already_reviewed` 幂等）→ 泳道3 车道 B
（`lane_b_advance`，派发已槽位化为 pr_repair）→ audit 后台线 spawn →
merge 门扫 `merge_sweep` → state 原子落盘（`_pipeline_save`：pipeline 是
state 唯一写者；每次成功派发/merge 记账即时落盘，见下）。

**scan 薄壳与 dry-run**：`scan` 子命令降级为「单轮 pipeline 循环」薄壳
（`run_pipeline_single_cycle`，与 pipeline 共用 pipeline.lock 和同一编排；
终态 cron 不再调它——旧 scan 行删除随 §3.1 runbook 部署，供会话内手动
推进）——旧 `_run_scan_orchestration` 编排保留
给 `dry-run` 子命令的只读预演。`pipeline --dry-run` 全流程零副作用演练
（零 gh 写/零 git/零 spawn/零本地写）。

**audit 后台线**（`run_audit`，pipeline 内 spawn，并发 1——`spawn_audit`
槽空则 spawn 一发 `task_loop.py audit` 子进程，timeout 7500s/发）：

- 独立 `audit.lock`（flock）——**不占三泳道槽**（独立 audit 槽 ×1）；
  PAUSED 同闸（熔断闸同样管 audit 的 claude 调用）。编排层（`spawn_audit`）
  另补三前置：内存闸不足不 spawn、克隆目录缺失不 spawn、**距窗末剩余
  < 7500s 不 spawn**（软停击杀会丢收尾 merge-write 白审 ~2h）。
  子进程 `setsid` 独立——pipeline 死/机器重启，audit 照样跑完。
- cursor：state 的 `audit_cursor`（上次审到的 SHA + 时间）；无 cursor 首跑
  回看 24h。`git fetch` 后 `cursor..origin/main` 无新 commit → 本轮退出。
- **单轮 diff 上限（轮5 B48）**：区间 diff 超 3000 行时从**最旧侧切块**，
  本轮只审一块、cursor 只推进到块基，余量由后续 spawn 续审接力（窗口内
  audit 槽空即再 spawn 一发）——PAUSED 长
  挂恢复后 base 停在数周前的超大区间不会再因 claude 上下文溢出（空输出/
  畸形 → strict 抛错 → cursor 不推进）而重复烧配额死循环；单个
  commit 自身超限时仍整块审查（尽力，不卡死在它身上）。
- 审查复用 `pr_local_review` 对抗审查引擎（prompt 泛化：diff + commit
  messages 替代 PR context），在即用即弃 worktree 里跑，绝不碰用户工作区。
- findings 逐条 hash 查重（`path+line+category+message` 归一化的 sha256
  前 16 hex；evidence 不进键）：**双源**——state `audit_seen` 集合 +
  `gh search issues` body marker `<!-- task-loop:audit:<hash> -->`。未见过
  才建 bug issue（labels `agent-task agent-bug bug`，正文含 marker）。
  findings 入库中途 GhError → 本轮零落盘，下轮靠 gh body marker 查重兜底
  （双源设计的意义）。
- **state 写入纪律（Ruling 11，pipeline 期演进）——为什么走
  `_audit_state_merge_write`**：audit 子进程只持 audit.lock，而其 claude
  运行数分钟期间并发的编排循环会推进 entries（派发记账/收割/对账）——
  若把 audit 启动时 load 的整份快照 save 回去，会把编排器的推进整体回滚：
  丢 agent_pid/状态 → 槽位失明 → 同一 worktree 重复派 agent（生产事故
  9d6ba452 同类根因）。故 audit 的一切持久化都经 `_audit_state_merge_write`：
  拿 scan.lock（阻塞式 flock——audit 自己的临界区只有一次 load+save，
  亚秒级；编排器从不持 scan.lock，锁序单向无死锁）后 load 磁盘**最新**
  state，只 update audit 自有键（`audit_cursor`/`audit_seen`/
  `auth_failure_streak`），再原子 save。**entries 的权威属于 pipeline
  （唯一写者），audit 永不写它**。认证失效计数 `_audit_streak_add`
  同一纪律（scan.lock 内对磁盘最新值读-改-写）。
  **pipeline 侧对称纪律（`_pipeline_save`）**：pipeline 整夜持有内存
  state，落盘走 scan.lock 内 merge-save——audit 独占键
  （`audit_cursor`/`audit_seen`）以磁盘为权威直接回填（裸 save 会把
  audit 同窗推进写回旧值）；双写者键 `auth_failure_streak` 合并取
  max(盘, 内存)——两源任一见 ≥3 连败即熔断，成功归零可能被另一线未
  归零值掩盖 → **熔断宁早勿晚**（fail-safe）；其余键 pipeline 唯一
  写者、内存为准。过渡期注意（§3.1 crontab 3 删 2 增部署前）：旧 cron
  的 scan tick 仍整段持有 scan.lock（分钟级），pipeline 落盘与之串行
  （有界等待，不构成死锁）。
  **等待时长的如实声明（轮6 #51）**：「亚秒级」说的是 audit 自己的临界区；
  等待其他 scan.lock 持有者放锁的时长以其临界区为界（过渡期旧 scan tick
  分钟级）。spawn 侧 `timeout 7500` 把整发 audit 封顶（被杀点若在落盘前 =
  设计好的「本轮零落盘」语义，findings 查重由 gh body marker 兜底，下一发
  重试）。

**关键约束**（全部由单测锚定）：三泳道并行槽位 **issue 修复 ×2 / pr 检视
×2 / 意见修复 ×2**（同 key 互斥、满槽拒绝、超时判死回收；audit 独立
×1 不占泳道槽）；attempts ≤2、**rounds ≤ 动态上限 2–3**（docs-only 或
简单 diff（总增删 ≤100 行 ∧ 变更文件 ≤5）→ 2，其余 → 3；取代旧固定
rounds≤4）、认证连续 ≥3 → PAUSED 熔断；merge 门独立双熔断（PAUSED /
`NO_AUTO_MERGE`，§7）；三泳道每次派发前过内存闸（MemAvailable ≥ 8G）；
第 5 bot（local-claude）连续 3 tick 缺位仅告警不降门槛。

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

### 3.1 终态 crontab（nightly pipeline，2026-11-13 红线反转）

**3 删**：`33 */6` scan 行（cron 漂移实况，见下）、`17 */6` audit 行、
`*/30` pr_local_review（第 5 bot 时间线）行。**2 增**（monitor 采集行
不动，与本体系无关）：

```cron
0 23 * * * mkdir -p ~/.local/state/sdcshield-task-loop && cd /home/sdc/wangxu/operator/sdcshield-autoci && timeout 39600 /usr/bin/python3 scripts/task_loop.py pipeline >> ~/.local/state/sdcshield-task-loop/pipeline.log 2>&1
@reboot /home/sdc/wangxu/operator/sdcshield/scripts/pipeline_selfheal.sh
```

- scan/audit/第 5 bot 检视三条旧 cron 线全部折进 pipeline：CI 红 intake
  与收割对账是循环步，audit 是 pipeline 内 spawn 的后台线（§1），第 5
  bot 检视是泳道 2（`pr_local_review.py --pr N` / `--all-open` CLI 保留
  供会话手动）。
- `0 23` 启动 + `timeout 39600`（11h，I4：软停等待窗最长 3600s+1800s
  grace，外层与窗口**零裕量**咬合时软停等待段必被外层 timeout 杀——夜报/
  记账丢失；11h 留 30min 裕量）外层兜底；pipeline 自身按
  `--window-start/--window-end`（默认 23/9）窗口循环、09:00 软停。
- `@reboot` 自愈：机器重启落在 23:00–09:00 窗口内则重新拉起同一命令
  （`scripts/pipeline_selfheal.sh`，锁语义双保险——已在跑则秒退）。
  本机频繁重启是常态（2026-11-12 当天 13h 内 5 次 boot）。
- **`mkdir -p` 前缀是硬要求**（沿袭旧约）：shell 在命令启动前打开重定向
  目标，父目录缺失时 `>>` 直接失败、命令体根本不执行。`~` 由 cron 的
  `/bin/sh -c` 作波浪线展开。**cron PATH 注意**：cron 环境只有
  `/usr/bin:/bin` 量级裸 PATH，故用显式 `/usr/bin/python3`；引擎自身的
  `gh` 调用直接跑在 cron 的 PATH 上（本机靠 crontab 顶部 `PATH=` 头），
  agent 派发所需的 `claude`/`gh` 由 agent.env 内 `PATH` 提供（§3.3）。
- 历史注记（如实记录）：2026-10-31 切换时 scan 部署为 `*/30`，实况后
  漂移为 `33 */6`（2026-11-12 实测，`crontab -l` 为准）；audit `17 */6`
  与文档一致；第 5 bot 时间线日志此前仍写旧目录
  `sdcshield-ci-triage/cron.log`。三行将随本节 runbook 部署删除，漂移随之
  消解——runbook 是**合入 main 后的人工步骤**，执行前三行旧 cron 仍在，
  实况以 `crontab -l` 为准。

**部署 runbook（合入 main 后人工执行，四步）**：

1. `cd /home/sdc/wangxu/operator/sdcshield-autoci && git fetch origin && git reset --hard origin/main`（自举：pipeline 跑的必须是含本引擎的新 main）
2. crontab 编辑：删 `33 */6` scan 行、`17 */6` audit 行、`*/30`
   pr_local_review 行；增上方两行
3. `touch ~/.local/state/sdcshield-task-loop/NO_AUTO_MERGE`（灰度首夜：
   merge 门转纯报告模式，只出「本应 merge + 门槛证据」夜报）
4. 次晨 `tail -50 ~/.local/state/sdcshield-task-loop/pipeline.log` + 最新
   `report-*.md` 人工复核 merge 门判定 → 复核数晚无误后
   `rm ~/.local/state/sdcshield-task-loop/NO_AUTO_MERGE` 放开真自动 merge

### 3.2 首启动自动迁移（state 目录）

`~/.local/state/sdcshield-ci-triage/` → `~/.local/state/sdcshield-task-loop/`，
`_migrate_legacy_state_dir()` 在 pipeline 前奏 / scan 薄壳 / audit 拿到锁后
执行（仅非 dry）：

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
crontab -e    # 删两行部署行(0 23 pipeline + @reboot 自愈,停调度)
touch ~/.local/state/sdcshield-task-loop/PAUSED   # 保险:已在跑的循环不再派发
touch ~/.local/state/sdcshield-task-loop/NO_AUTO_MERGE  # 保险:只停自动 merge(派发照常)
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
| `DONE` | 结案 | 引擎 PR：merge 门六门槛全过自动 squash merge（§7）后置 DONE，**记账 `merged_at`/`merged_sha`（revert 归因主数据，人工 merge 永不写）**；用户 docs PR：merge 门过即合（无条目，不进状态机） | PR 评论贴门槛证据审计 + issue 贴闭环评论 + 关 issue + 摘 wip |
| `ESCALATED` | 升级人工，冻结 | attempts>2 / rounds 超动态上限（2/3，§4 全局机制）/ agent 自报 escalated / 熔断 / revert 二阶链 / 与 main 冲突连续 ≥2 夜（`conflict_nights`，merge 门逐夜计数——夜 = pipeline 夜 23:00–09:00 跨午夜，同夜多轮 cycle 不重复计数；spec §5——不自动 rebase） | @wangxumarshall 评论附日志尾证据与建议 + `agent-escalated` 标签 + 摘 wip |
| `HUMAN_CLOSED` | 人工接管，工具永不再碰 | QUEUED/DISPATCHED 条目 issue 已被人工关闭（无 PR 路径；有 PR 路径的由车道 B 收尾） | 不 @ 不评论（DISPATCHED 先击杀在跑 agent、摘 wip） |

**全局机制**：

- **三泳道槽位**：issue 修复 / pr 检视 / 意见修复各 ×2 并行（audit 独立
  ×1 不占泳道槽）——槽位租约带 `started_at`+PID+`boot_id`，超时判死回收、
  同 key 互斥、满槽拒绝下一轮再试；取代旧「全局单飞 1 agent」约束。
- **attempts ≤ 2**：dead/timeout/谎报 done（日志称 done 但 `gh pr list`
  查无 PR）各 +1，超限升级；agent 诚实自报 escalated **不计** attempts
  （诚实上报不是失败重试）；**窗口软停击杀与机器重启（boot_id 变化）死亡
  同样不计**——环境性失败 ≠ agent 失败（PR #611 attempts=4 假性升级教训），
  条目干净回队次夜续跑。
- **rounds 动态上限 2–3**：检视-修复轮数按复杂度定（`max_rounds_for`：
  docs-only 或简单 diff——总增删 ≤100 行 ∧ 变更文件 ≤5 → 上限 2；
  其余 → 上限 3），超限 ESCALATED——取代旧固定 `rounds ≤ 4`。
- **认证熔断**：`auth_failure_streak` 跨条目/跨泳道/含 audit 累计（任何
  一次 success 归零——防「连续 ≥3」退化成「累计 ≥3」；pipeline 落盘与
  audit 锁下合并写取 max，宁严勿松）；连续 ≥3 → 写 `PAUSED` + 升级评论。
- **merge 记账与 revert 归因**：引擎 PR 自动 merge 后 entry 记
  `merged_at`/`merged_sha`——只由编排器 merge 执行写（**人工 merge 的
  sha 永不进归因集**，回读失败降级 None 并 [warn]——已合入是事实，账面
  如实反映）；夜间 intake 发现 main 红 run 且 `head_sha` 命中 `merged_sha`
  集 → 建 revert issue（分支随 issue 约定 `fix/issue-<N>`——issue 走泳道1
  派发即按 issue 号建分支，不为 revert 单设命名；merge 门正则
  `fix/revert-prN` 模式为防御性冗余保留，正文 `revert-for: PR #N @ <sha>`
  双源查重——state `revert_seen` 主查重（建成即记，跨轮零 gh）+ gh 裸 sha
  双态搜索兜底（state 丢失/重启后，open/closed 命中均不重建））入泳道1，
  revert PR 走**同一 merge 门**；归因命中且已建/已有 revert issue 的红 run
  **吸收**——不再走 triage 前向修复（防 revert agent 与修复 agent 对同一
  故障互踩），归因不到/查重与探针失败转人工的红照旧前向修复（兜底零
  丢失）；revert 链深度 ≤1——revert
  PR 再引红直接 ESCALATED（防 revert 打乒乓球）。
- **同根因收敛**：同 workflow 有活跃条目（非 DONE/ESCALATED/HUMAN_CLOSED
  三终态，与上表一致）时新红 run 不开新 issue，只挂 `extra_runs` + 追加评论；
  HUMAN_CLOSED 是终态——人工已接管的死条目不参与收敛，新红 run 照常开新
  issue（不在已关闭 issue 上评论、不吞失败）。

## 5. 运维

### 5.1 kill switch（双闸：停派发 / 停自动 merge）

```console
touch ~/.local/state/sdcshield-task-loop/PAUSED   # 停:一切派发全停,观测照常
rm    ~/.local/state/sdcshield-task-loop/PAUSED   # 恢复
touch ~/.local/state/sdcshield-task-loop/NO_AUTO_MERGE   # 只停自动 merge:merge 门转纯报告模式
rm    ~/.local/state/sdcshield-task-loop/NO_AUTO_MERGE   # 恢复自动 merge
```

PAUSED 语义：`dispatch_agent` 内部闸门——存在即拒绝一切派发（三泳道
同闸），audit 的 claude 调用与泳道2 检视派发同样被拦；扫描/issue 搜索/
收割观测照常。kill switch 与熔断共用 PAUSED 文件；过渡期旧目录
（`sdcshield-ci-triage`）的 PAUSED 同样生效（双路径检查）。

`NO_AUTO_MERGE` 语义（merge 门专用熔断，spec §11 灰度）：存在即 merge
门转**纯报告模式**——检视/修复/audit 全部照常跑，merge 只在夜报记
would-merge（PR 号 + 门槛逐条证据）绝不执行；部署灰度与运行期紧急停
自动 merge 共用此文件（§3.1 runbook 第 3/4 步）。

### 5.2 熔断恢复

熔断触发（连续 ≥3 次认证失败）会写 PAUSED 并在 issue 上发升级评论。
恢复步骤：**先**更新 `~/.local/state/sdcshield-task-loop/agent.env` 里的
凭据（ANTHROPIC_AUTH_TOKEN 等），**再** `rm PAUSED`——顺序颠倒会在旧
凭据上继续空转烧配额。

### 5.3 手动重跑 / 观测

```console
cd /home/sdc/wangxu/operator/sdcshield-autoci
python3 scripts/task_loop.py status            # 队列总览: streak/PAUSED/槽位占用
                                               #   + 顶部最近一夜报告行(state["last_report"])
                                               #   + 全部条目 + 待入队任务
python3 scripts/task_loop.py scan              # 单轮 pipeline 循环薄壳: 会话内手动推进,
                                               #   与 pipeline 共用 pipeline.lock 和同一编排;
                                               #   (终态不再由 cron 调,夜间编排归 pipeline;
                                               #    旧 scan 行删除随 §3.1 runbook)
python3 scripts/task_loop.py pipeline --dry-run # nightly pipeline 全流程零副作用演练:
                                               #   零 gh 写/零 git/零 spawn/零本地写
python3 scripts/task_loop.py dry-run           # 旧 scan 编排只读演练: 零 gh 写/零 git/零派发/零本地写
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
| `state.json` | 全部条目（含 `merged_at`/`merged_sha`）+ 槽位账本 `slots` + 熔断计数 + audit cursor/seen + `last_report`（原子写：tmp → os.replace；pipeline 唯一写者，`_pipeline_save` 锁下 merge-save） |
| `PAUSED` | 存在即 kill switch/认证熔断生效（派发全停） |
| `NO_AUTO_MERGE` | 存在即 merge 门转纯报告模式（§5.1） |
| `pipeline.lock` | pipeline/scan 薄壳互斥锁（flock） |
| `scan.lock` | state merge-save 互斥锁（flock；audit 子进程合并写与 `_pipeline_save` 共用） |
| `audit.lock` | audit 互斥锁（与编排锁分离，不占泳道槽） |
| `agent.env` | 凭据环境快照（600，§3.3） |
| `agent-<runid 或 issue-N>-<UTC 时间戳>.log` | 每次派发的 agent 完整 stdout/stderr（ci 源按 run_id、其余源按 issue 号命名；重派不覆盖上一轮现场） |
| `prompt-<issue 号>-<UTC 时间戳>.txt` | 派发 prompt 落盘（0600，经 stdin 喂给 claude——修 argv 传 prompt 的 ARG_MAX 隐患；issue 号缺省时用 run_id） |
| `collect-pr<N>.md` | 车道 B 派发前导出的 bot 检视意见（不可信检视数据） |
| `pipeline.log` | crontab `0 23` 重定向的 pipeline 输出（revert 转人工/升级/查重留痕在此，夜报正文引其路径） |
| `report-YYYYMMDD.md` | 每晚夜间报告＝**事件清单**（`write_report` 实落内容：merge/would-merge/gate-reject/revert-issue/槽回收事件，一事一行）+ 人工介入信号见 `pipeline.log`（转人工/升级/查重/去重留痕只在 stdout，夜报正文只引其路径，不复制内容）；`state["last_report"]` 记最新一份，`status` 顶部展示 |
| `audit.log` | pipeline spawn 的 audit 后台线输出 |
| `cron.log` | 旧 scan 输出归档（终态 cron 不再调 scan——旧 scan 行删除随 §3.1 runbook） |

旧目录 `~/.local/state/sdcshield-ci-triage/` 迁移后保留归档（含历史
agent 日志与 cron.log）。

## 6. FAQ

**Q：认证失效怎么办？**
看 agent 日志尾部（`API Error`/`403`/`Not logged in`）。更新 `agent.env`
凭据后删 PAUSED（§5.2）。连续 ≥3 次会自动熔断，不会无限烧配额。

**Q：为什么我的 PR 没被接管？**
车道 B（泳道3）只接管引擎**自己派生**分支的 PR——`fix/ci-<runid>` 与
`fix|feat/issue-<N>`（`pr_number` 唯一来源是收割 success 路径的
`gh pr list --head <fix_branch>` 查询，车道 B 从不扫描存量 open PR）。
用户在研 PR 的检视-修复不接管——继续用交互式 `/pr-loop`（D6 决策，根除
同一 PR 并发 actor 冲突）。**例外是 merge 门的 docs-only 快道**（§7）：
用户 docs PR 标题无 `WIP:` 前缀且零可行动意见时会被当夜自动合入——
`WIP:` 前缀即草稿闸（不想被合就保留前缀）；检视信号（第 5 bot）仍照常
提供，有意见则不合入、留人工。

**Q：feature issue 为什么一直不派发？**
缺 `agent-approved`（feature 类的派发门，§2 资格公式）。`python3
scripts/task_loop.py approve <N>` 或 GitHub 手动打标签，下一轮编排循环
进队列（夜间 pipeline 自动；白天可 `scan` 薄壳手动推进，§5.3）。

**Q：agent 会不会碰我的工作区？**
不会。修复/实现 agent 只在独立克隆
`/home/sdc/wangxu/operator/sdcshield-autoci` 的**每条目独立 worktree**
（`<clone>/.worktrees/<lane>-<key>`，派发前 `git worktree add`，收割/软停
remove、残留由每轮无条件 heal 清）里工作，
prompt 红线明令主工作区只读；git 单树纪律要求任务从 `origin/main` 起
`checkout -B fix|feat/issue-<N>`。audit 的审查在克隆内
的即用即弃 worktree 跑（`cmd_audit` 先 chdir 到克隆目录，缺失时如实
报错退出，绝不留在用户工作区）。

**Q：如何完全停用？**
`crontab -e` 删两行部署行（`0 23` pipeline + `@reboot` 自愈）+
`touch PAUSED`（§3.4）。已开 issue 保留人工处置。

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
  以及 **audit 后台线（旧「每 6h」cron 将随 §3.1 runbook 部署删除，终态为
  pipeline 窗口内 spawn 的多轮接力，§1）对 main 增量的 diff 全文 + commit messages**——
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
- **merge 红线（2026-11-13 反转，spec 2026-11-12-full-auto-pipeline §5）**：
  merge 执行权在**编排器确定性门槛代码**（`evaluate_merge_gate` →
  `execute_merge`），绝不在 LLM——agent prompt 红线「绝不 merge」沿用，
  agent 仍一律不许 merge。范围与门槛：
  - **引擎 PR**（state 有条目 ∧ head 分支匹配
    `fix/issue-N|fix/ci-N|fix/revert-prN`）与**全 docs-only PR**（全部
    变更文件 `.md`；不要求引擎归属、适用任意 PR）过门自动
    `gh pr merge --squash --delete-branch`；**用户非 docs PR 永不自动
    merge**，由人执行。
  - **六门槛**（任一不过不动，只判不执行分离）：①归属（docs-only 例外
    放宽）；②检视——`classify_state == READY_TO_MERGE`，docs 快道放宽为
    第 5 bot 唯一必需信号（外部 4 bot 事件驱动照跑不等待）+ 静默窗
    900→300s + 零可行动意见；③CI check——全门槛查 `lint`/`git-sanity`/
    `Build binary for CPU (Linux)`/`zizmor`，docs-only 只查前二；红 job 与
    **最近 3 个已合入 PR 同 workflowName 的红 job 集**对照，全组 ⊆
    基线 → 判「继承红」放行（PR #426/#343 先例，门槛内建基线对照）；
    ④open ∧ 非 DRAFT ∧ 非 fork ∧ mergeable；⑤熔断——PAUSED 与
    `NO_AUTO_MERGE` 双熔断（任一存在 → merge 门转纯报告模式，§5.1）；
    ⑥审计——merge 前 PR 评论贴门槛逐条证据，merge 后 state 记
    `merged_at`/`merged_sha`（revert 归因主数据，人工 merge 永不写）、
    issue 贴结案评论。
  - **docs 快道草稿闸**：标题 `WIP:` 前缀 = 草稿永不自动 merge，去掉
    前缀 = 显式同意当夜合入（auto-pr 生成标题恰为 `WIP: <分支>`，闸与
    现状自洽）。
  - **`NO_AUTO_MERGE` 永远有效**：`touch` 后 merge 门纯报告（would-merge
    证据进夜报），复核后 `rm` 放开——部署灰度与紧急停共用（§3.1/§5.1）。
  - **自动 resolve review threads 仍然禁止**（该红线未反转）：检视意见
    逐条批判性处置 + reply 答复（§1 泳道3），绝不自动 resolve/mark
    resolved；剩余 threads 保留供人工。
  - merge 后 main 红 → 精确归因自动 revert（`merged_sha` 集命中才回滚，
    §4 全局机制）；归因不到照旧前向修复。

## 8. 与 pr_loop / ci_triage 的关系

依赖栈（方向不变）：`pr_local_review`（基座：gh 原语 + 本地对抗审查）
→ `pr_loop`（中层：PR 五 bot 状态机）→ `task_loop`（顶层：nightly
pipeline 编排 + issue 生命周期 + agent 派发收割 + merge 门，
`task_loop_pipeline` 模块承载泳道/门槛/软停/夜报）。

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
  → 循环 → merge 交接），零重复实现。

## 10. 验收记录（实测，2026-10-31）

> 历史注记：本节是 **scan-cron 时代**（`*/30` scan + `17 */6` audit 双
> cron、「READY_TO_MERGE 时 @人工 merge」旧红线）的部署切换与验收记录，
> 全部为当时真实事件，保留供追溯；2026-11-13 起调度与 merge 政策已被
> nightly pipeline 取代（§1/§3.1/§7），下述 cron 形态与 merge 提醒语义
> 不再是现状。

**部署切换时间线（全部真实事件，本机观测）**：

- 2026-10-30 18:58（本地）：PR #317 squash 合入 main（`8010e60e`）。
- 合入后旧 cron 行（`ci_triage.py scan`）**零人工干预**完成过渡：autoci
  克隆经 tick 内 heal 对齐新 main 后，模块别名薄壳自动切到 task_loop；
  state 目录迁移就位（`state.json`+`agent.env` 拷贝、v1→v2 归一，5 条
  存量条目全部续接，旧目录保留归档）。过渡期两引擎短暂并存（旧引擎最后
  一次写旧目录 2026-10-30 19:00 本地，此后仅新目录推进，无双写损坏）。
- 无人值守稳态运行约 19 小时（10-30 19:00 → 10-31 13:00 本地）：每
  30min tick 正常，cron.log 全程「扫描到 0 个红 run」+ heal 对齐 +
  车道 B 判定，无异常、无 PAUSED。
- **首个真实收尾周期（继承条目，车道 B 全程零人工）**：条目 #304
  （ci 源 Sanitizer run 40596064101，迁移时 PR_OPEN，PR #305）→
  2026-10-31 11:30 PR #305 squash 合入 main（`45b2ad70`）→ 引擎下个
  tick（12:00）判定 PR CLOSED → issue #304 收尾 + 条目 DONE
  （`updated_at=2026-10-31T04:00:30Z`），state 与 GitHub 侧一致。
- 2026-10-31 13:16（本地）：crontab 切换为 §3.1 终态两行（scan `*/30`
  + audit `17 */6` 含 `timeout 7500`；备份 `~/crontab.backup.20261031`）。
  此后 tick 输出写入新目录 `cron.log`。

**待观察项（如实记录，后续追加）**：

- audit 线首跑：cron 切换后首次触发 18:17（本地），首跑按 24h 回看窗口
  审查（`audit_cursor` 为空）——首个 audit 周期结果待记录。
- 首个**新任务**（非继承条目）全周期：issue → agent 派发 → PR → bot
  循环 → merge 提醒；异常结局（ESCALATED/HUMAN_CLOSED 路径）如实记录。
  此项同时是 PR-5（删薄壳）的前置条件。

观测命令（nightly pipeline 时代，§5.3/§5.4）：
`tail -50 ~/.local/state/sdcshield-task-loop/pipeline.log`（pipeline）、
最新 `report-YYYYMMDD.md`（夜报）、`python3 scripts/task_loop.py status`
（队列总览，顶部含最近一夜报告行）。

## 11. 延伸阅读

- [pr-loop：会话内 PR 检视-修复闭环](pr-loop.md) — 车道 B 复用的检视闭环
- [PR 代码检视机器人](pr-review-bots.md) — 五 bot 判据与 auto-pr
- [ci-triage：CI 无人值守分诊（历史手册）](ci-triage.md) — 前代引擎，
  本引擎的搬移来源与生产经验（含首跑缺陷教训）
- 设计规格（会话产物，主仓 docs/superpowers/specs/，不入库）：
  `2026-10-29-task-loop-design.md`
