#!/usr/bin/env python3
"""task_loop_pipeline.py — 夜间全自动流水线编排器(spec 2026-11-12-full-auto-pipeline)。

cron 0 23 启动: 窗口循环(每 PIPELINE_CYCLE_SEC 一轮) → 三泳道并行派发
(issue 修复/pr 检视/意见修复, 各 SLOT_KINDS 个槽) + audit 后台线 + merge 门
(引擎 PR 全门槛自动 squash merge; docs-only 快速通道) + 精确归因 revert。
09:00 软停: 不再派新 → 等在跑 ≤SOFT_STOP_GRACE_SEC → 击杀(不计 attempts,
条目回队列) → 清 worktree → 写夜间报告。设计细节一律见 spec,本文件不重述。
"""
# SPDX-License-Identifier: Apache-2.0
import collections
import fcntl
import os
import re
import shlex
import sys
import time
from datetime import datetime, timedelta, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import task_loop as tl  # noqa: E402  (与 tl.pr_loop 同款命名空间引用,测试可 monkeypatch)
from task_loop import GhError, StateStore  # noqa: E402

PIPELINE_LOCK_NAME = "pipeline.lock"
NO_AUTO_MERGE_NAME = "NO_AUTO_MERGE"
SLOT_KINDS = {"issue_fix": 2, "pr_review": 2, "pr_repair": 2, "audit": 1}
PIPELINE_CYCLE_SEC = 300
SOFT_STOP_GRACE_SEC = 1800
SOFT_STOP_KILL_GRACE_SEC = 10     # SIGTERM 后给仍活者的收尾窗,再 SIGKILL 补刀
REVIEW_AGENT_TIMEOUT_SEC = 7500     # M13(2026-11-13 终审): 内层最坏 = claude
                                    # 3600 + 解析失败重试 3600 = 7200+,外层
                                    # 旧值 5400 必被内层反超——检视全部死于
                                    # 外层 timeout,审查白跑;7500 留 ~5min 余量
REVIEW_PER_HEAD_PER_NIGHT = 2
DOCS_QUIET_SEC = 300
AUDIT_LINE_TIMEOUT_SEC = 7500      # audit 后台线自身 timeout 预算(≈2h/轮)
MEMINFO_PATH = "/proc/meminfo"

# audit 独占 state 键(task_loop 裁定 11: audit 子进程只经 _audit_state_merge_write
# 在 scan.lock 内读-合-写)——pipeline 不写,pipeline 落盘时以磁盘为权威回填。
AUDIT_OWNED_KEYS = ("audit_cursor", "audit_seen")
# 双写者键(fix-round 1 Important#1 修正,controller 裁定 2026-11-13):
# auth_failure_streak / auth_failure_at 由 pipeline(reap_or_advance 每轮在
# 内存 bump/归零)与 audit 子进程(_audit_streak_add,scan.lock 下)两线共管
# ——pipeline 落盘合并取近似: streak=max(盘,内存)、at=取两侧较新。裸拷
# 磁盘旧值会把 pipeline 的 bump 在内存与磁盘同时抹掉,「连续 ≥3 → PAUSED」
# 熔断跨轮结构性失效(凭据过期夜=整夜空转 ~120 轮不熔断不升级)。


def min_mem_gb() -> int:
    """内存闸阈值(GB)——env PIPELINE_MIN_MEM_GB 调用时读(测试可 setenv)。"""
    try:
        return int(os.environ.get("PIPELINE_MIN_MEM_GB", "8"))
    except ValueError:
        return 8


def _cgroup_mem_limit_mb() -> "int | None":
    """cgroup 内存限额(MB);无限制/不可读 → None(不参与取小)。"""
    for path in ("/sys/fs/cgroup/memory.max",           # cgroup v2 统一层级
                 "/sys/fs/cgroup/memory/memory.limit_in_bytes"):  # v1
        try:
            with open(path) as f:
                raw = f.read().strip()
        except OSError:
            continue
        if raw in ("", "max"):
            return None
        v = int(raw)
        if v > (1 << 40):          # v1 时常给 2^63 量级哨兵 = 无限制
            return None
        return v // (1024 * 1024)
    return None


def mem_available_mb() -> int:
    """MemAvailable(MB) 与 cgroup 限额取小——两者任一吃紧都算吃紧。"""
    avail = None
    try:
        with open(MEMINFO_PATH) as f:
            for line in f:
                if line.startswith("MemAvailable:"):
                    avail = int(line.split()[1]) // 1024
                    break
    except OSError:
        return 0
    if avail is None:
        return 0
    limit = _cgroup_mem_limit_mb()
    return min(avail, limit) if limit is not None else avail


def mem_gate_ok() -> bool:
    """派发前内存闸: MemAvailable ≥ 阈值才放行新 agent(宁慢勿爆,spec §7)。"""
    return mem_available_mb() >= min_mem_gb() * 1024


def boot_id() -> str:
    """本机启动标识——槽位记录启动时 boot_id,收割时对比即知 agent 是否死于重启。"""
    with open("/proc/sys/kernel/random/boot_id") as f:
        return f.read().strip()


def in_window(now: datetime, start_hour: int = 23, end_hour: int = 9) -> bool:
    """流水线窗口(本地时间): 23:00–09:00,跨午夜区间。"""
    h = now.hour
    return h >= start_hour or h < end_hour


def _now_local_iso():
    return datetime.now().astimezone().isoformat(timespec="seconds")


def _slots(state, kind):
    return state.setdefault("slots", {}).setdefault(kind, [])


def slot_get(state, kind, key):
    for s in _slots(state, kind):
        if s["key"] == key:
            return s
    return None


def slot_acquire(state, kind, key, pid, pgid, log, worktree,
                 started_iso, boot):
    """占用一槽;同 key 互斥(同条目/同 PR 至多 1 agent)、满槽拒绝。"""
    live = [s for s in _slots(state, kind) if tl.agent_alive(s["pid"])]
    if slot_get(state, kind, key) is not None:
        return False
    if len(live) >= SLOT_KINDS.get(kind, 0):
        return False
    _slots(state, kind).append(
        {"key": key, "pid": pid, "pgid": pgid, "log": log,
         "worktree": worktree, "started_at": started_iso, "boot": boot})
    return True


def slot_release(state, kind, key):
    s = _slots(state, kind)
    state["slots"][kind] = [x for x in s if x["key"] != key]


def free_slot_count(state, kind):
    """空槽数 = 槽上限 − 活 PID 占用数(死槽即时回收)。"""
    live = sum(1 for s in _slots(state, kind) if tl.agent_alive(s["pid"]))
    return max(0, SLOT_KINDS.get(kind, 0) - live)


def slot_janitor(state, now_iso):
    """清死槽并回收其 worktree;返回被清清单(boot 变化 → environmental=True,
    供启动对账做 attempts 豁免;软停/正常退场走 slot_release 不经此)。"""
    released = []
    for kind in SLOT_KINDS:
        alive = []
        for s in _slots(state, kind):
            if tl.agent_alive(s["pid"]):
                alive.append(s)
                continue
            released.append({"kind": kind, **s,
                             "environmental": s.get("boot") != boot_id()})
            if s.get("worktree"):
                tl.worktree_cleanup(s["worktree"])
        state["slots"][kind] = alive
    return released


def startup_reconcile(state):
    """pipeline 启动对账: 重启(boot_id 变)期间死亡的 agent 条目干净回队、
    不计 attempts(#611 attempts=4 假性升级教训);其余死槽只清槽,条目判档
    仍交 tl.reap_or_advance(单飞语义外的正常 dead/timeout 记账)。"""
    entries = None
    for info in slot_janitor(state, _now_local_iso()):
        if not info["environmental"]:
            continue
        if entries is None:
            # StateStore.entries 每次 from_dict 出新副本——只解析一次,
            # 推进在循环外统一回写,否则下一轮解析即丢。
            entries = tl.StateStore.entries(state)
        for e in entries:
            keys = (f"issue-{e.issue_number or e.run_id}",)
            if e.pr_number is not None:
                keys = keys + (f"pr-{e.pr_number}",)
            if info["key"] in keys and e.status in tl.AGENT_STATES:
                e.status = "PR_OPEN" if e.status == "BOT_FIX" else "QUEUED"
                e.agent_pid = None
                e.agent_started_at = None
                e.last_dispatched_head = None
                e.updated_at = _now_local_iso()
                print(f"[reconcile] {info['key']} 死于重启(boot 变化)——"
                      f"干净回队 {e.status},不计 attempts")
    if entries is not None:
        # Entry 是解析副本,推进须 to_dict 序列化回 state dict(与
        # tl.reap_or_advance 同款 write-back),否则下次 entries 拿回旧值。
        state["entries"] = [e.to_dict() for e in entries]


# ============ 泳道2 检视派发器(Task 6) ============


def reset_review_tries(state):
    """每晚边界: pipeline 启动时清零检视尝试计数——「每晚每 (pr,head) ≤2
    次」的"夜"以流水线启动对账为界(spec §4 泳道2 边界列)。"""
    state["review_tries"] = {}


def dispatch_review(state, pr_num, head):
    """泳道2 派发: 每晚每 (pr,head) ≤REVIEW_PER_HEAD_PER_NIGHT 次;满槽/内存
    不足返回 None。spawn 命令与修复 agent 同形(setsid+timeout),凭据经 agent.env。

    派发序(T10 顺手修①②③④,R3 语义保持): 内存闸 → 同 key **活占位**前置
    早退(先于每晚上限计数——活 agent 在跑不是一次检视尝试,零 spawn 零
    killpg;死槽自释放仍按 R3 只清本 key,不破坏「先 janitor 后派发」序)
    → 每晚上限 → 满槽 → agent.env 存在性(B44 同款: 缺凭据拒绝且不烧预算,
    不拦则第 5 bot 从未启动却烧满 2 次/晚)→ spawn;acquire 失败(仅剩同轮
    并发竞态)killpg 整组击杀不留孤儿。"""
    if not mem_gate_ok():
        print(f"[mem-gate] MemAvailable 不足({min_mem_gb()}GB 闸),"
              f"本轮不派检视 agent")
        return None
    key = f"pr-{pr_num}"
    existing = slot_get(state, "pr_review", key)
    if existing is not None and tl.agent_alive(existing["pid"]):
        # 顺手修②: 活占位早退——旧形态 spawn 后 acquire 同 key 互斥拒再
        # killpg,白白起落一次进程;击杀网只保留给 acquire 竞态兜底。
        print(f"[review] pr_review 槽 {key} 活 agent 在跑,本轮跳过")
        return None
    # R3(控制器裁定): 同 PR 重试先释放自身 key 的死槽——janitor 每轮才跑,
    # 不自清则同 key 互斥把「agent 已死、本可重派」的 PR 卡到下一轮
    # (释放只看本 key,不破坏「先 janitor 后派发」的全局清理序)。
    if existing is not None:
        slot_release(state, "pr_review", key)
    tries = state.setdefault("review_tries", {})
    tk = f"{pr_num}:{head}"
    if tries.get(tk, 0) >= REVIEW_PER_HEAD_PER_NIGHT:
        print(f"[review] PR #{pr_num} head {head[:8]} 已试 {tries[tk]} 次/晚,"
              f"留待明晚")
        return None
    if free_slot_count(state, "pr_review") <= 0:
        print(f"[slots] pr_review 满槽,拒绝为 {key} 派检视")
        return None
    env_file = os.path.join(tl._resolve_state_dir(), tl.AGENT_ENV_NAME)
    if not os.path.exists(env_file):
        # 顺手修①(B44 同款前置拒绝): 缺凭据是部署问题——真实派发拒绝且
        # 不烧每晚预算/不占槽,按 task-loop.md §3.3 放置凭据后自动恢复。
        print(f"[review] agent.env 缺失({env_file})——按 task-loop.md §3.3 "
              f"放置凭据(权限 600)后恢复;本轮不派检视、不烧每晚预算")
        return None
    # 顺手修④: 时间戳 naive 本地 → UTC 带 Z(与 tl.dispatch_agent 的
    # _now_iso UTC 口径对齐;文件名模板 review-pr<N>-<ts>.log 不变)。
    ts = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    log = os.path.join(tl._resolve_state_dir(), f"review-pr{pr_num}-{ts}.log")
    inner = (f"source {shlex.quote(os.path.join(tl._resolve_state_dir(), 'agent.env'))}"
             f" && timeout {REVIEW_AGENT_TIMEOUT_SEC} /usr/bin/python3 "
             f"{shlex.quote(os.path.join(tl._resolve_clone_dir(), 'scripts', 'pr_local_review.py'))}"
             f" --pr {pr_num}")
    cmd = f"nohup setsid bash -c {shlex.quote(inner)} >> {shlex.quote(log)} 2>&1 & echo $!"
    pid = tl.run_background(cmd, cwd=tl._resolve_clone_dir(), log=log)
    if not slot_acquire(state, "pr_review", key, pid, pid, log, None,
                        _now_local_iso(), boot_id()):
        # 占位失败只剩同轮并发竞态(活占位早闸已拦常态)——killpg 整组击杀
        # 刚派生的 agent 不留孤儿(setsid 使 $! PID 即 pgid),且不烧每晚
        # 预算(tries 未增,击杀不是一次检视尝试)。
        print(f"[review] pr_review 槽 {key} 占位失败(同 key 竞态),"
              f"已击杀 PID {pid}")
        try:
            os.killpg(pid, 9)
        except OSError:
            pass
        return None
    tries[tk] = tries.get(tk, 0) + 1
    return pid


# ============ 泳道3 merge 门(Task 7) ============

GateResult = collections.namedtuple("GateResult", "ok fast reasons")

# 必需 check 常量(裁定#1 校准,2026-11-13 对现役 PR #661/#660/#643/#642
# `gh pr view --json statusCheckRollup` 实测四 PR 完全一致): rollup 的
# CheckRun 实名是 lint / git-sanity / Build binary for CPU (Linux) /
# zizmor (workflow security)——分支保护 UI 建议集的写法「Security /
# zizmor」「Build binary … (GCC-arm64, -arm, unittests, selftests)」在
# rollup 不出现;checks_ok 按名字前缀匹配,常量写主干即可(矩阵后缀形态
# 由前缀覆盖;zizmor 的状态上下文裸名「zizmor」同被主干覆盖)。
REQUIRED_CHECKS_FULL = ["lint", "git-sanity",
                        "Build binary for CPU (Linux)", "zizmor"]
REQUIRED_CHECKS_DOCS = ["lint", "git-sanity"]
ENGINE_BRANCH_RE = re.compile(r"^fix/(issue-\d+|ci-\d+|revert-pr\d+)")
BASELINE_MERGED_DEPTH = 3


def docs_only(files):
    """docs-only 判定委托 tl._docs_only(T5 单一真源——同一判定同时喂
    max_rounds_for 轮次上限与本门快道,口径必须同源不漂移)。"""
    return tl._docs_only(files)


def wip_gated(title):
    """用户 WIP 闸(spec §5 docs 例外): WIP: 前缀 = 显式不同意当夜合入,
    去掉前缀即显式同意;只拦 docs 快道(引擎 PR 走 classify_state 五 bot)。"""
    return bool(title) and title.startswith("WIP:")


def baseline_reds(slug, workflow):
    """继承红基线(controller 裁定 2026-11-13,fix-round 1 Important#2 重写):
    最近 BASELINE_MERGED_DEPTH 个已合入 PR 的 rollup 中,同 workflowName 的
    红 job 名并集——贴 PR #426/#343「同基底已合入 PR 对照」先例的实操化。
    原 main-run 形态(--event schedule,push)对 PR CI workflow("Pull
    Request",lint/build 所在)结构性为空: 该 workflow 仅 pull_request 事件
    跑在 PR head,main 上无对应 run,继承红永不放行、特性死亡。拉不到/
    空 → 空集(不放行任何红,宁严勿松);单个基线 PR 拉取失败跳过不炸整链。
    PR 号经 str().split() 归一: gh --jq '.[].number' 多值输出多行裸串,
    单值经 gh_json 的 json.loads 得 int。"""
    try:
        nums = tl.gh_json_idem(["pr", "list", "--repo", slug, "--state",
                                "merged", "--limit", str(BASELINE_MERGED_DEPTH),
                                "--json", "number", "--jq", ".[].number"])
    except GhError:
        return set()
    jobs = set()
    for n in [x.strip() for x in str(nums or "").split() if x.strip()]:
        try:
            rollup = tl.gh_json_idem(
                ["pr", "view", n, "--repo", slug, "--json",
                 "statusCheckRollup", "--jq",
                 '.statusCheckRollup[] | [.workflowName, .name, .conclusion] | @tsv'])
        except GhError:
            continue
        for line in str(rollup or "").splitlines():
            parts = line.split("\t")
            if len(parts) < 3:
                continue
            if parts[0].strip() == workflow and \
                    parts[2].strip().lower() == "failure":
                jobs.add(parts[1].strip())
    return jobs


def checks_ok(pr, required, slug):
    """PR 的 check rollup: required 集(按名字前缀匹配,主干覆盖矩阵后缀)
    全绿 → True;红 job 按 rollup 行自带的 workflowName 分组对照已合入
    PR 基线(裁定#2 数据流: PRData 无 workflow_name 字段,红 job 自证所属
    workflow,evaluate_merge_gate 不传 pr.workflow_name;基线 = baseline_reds
    最近 BASELINE_MERGED_DEPTH 个已合入 PR 同 workflowName 的红 job 并集),
    全组 ⊆ 基线 → 继承红放行(spec §5 门槛 3)。结论按小写归一——实测
    rollup 结论是大写 SUCCESS/SKIPPED/FAILURE;SKIPPED 不算红(docs-only PR
    的重 job 常见)。
    """
    try:
        rollup = tl.gh_json_idem(
            ["pr", "view", str(getattr(pr, "pr_number", None)
                               or getattr(pr, "number", None)),
             "--repo", slug, "--json", "statusCheckRollup", "--jq",
             ".statusCheckRollup[] | [.name, .conclusion, .workflowName] | @tsv"])
    except GhError as exc:
        return False, [f"check rollup 拉取失败: {exc}"]
    conclusions = {}
    failed_by_wf = {}       # workflowName → 红 job 名集合(继承红分组单位)
    for line in (rollup or "").splitlines():   # gh 空输出 None → 零 check → 缺席拒
        if not line.strip():
            continue
        name, _, rest = line.partition("\t")
        concl, _, workflow = rest.partition("\t")
        name = name.strip()
        conclusions[name] = concl.strip()
        if concl.strip().lower() not in ("success", "skipped"):
            failed_by_wf.setdefault(workflow.strip(), set()).add(name)
    missing = [r for r in required
               if not any(n.startswith(r) for n in conclusions)]
    if missing:
        return False, [f"required check 缺席: {missing}"]
    if not failed_by_wf:
        return True, []
    evidence_ok, evidence_bad = [], []
    for workflow, jobs in sorted(failed_by_wf.items()):
        base = baseline_reds(slug, workflow)
        wf_label = workflow or "<无 workflow>"
        if jobs <= base:
            evidence_ok.append(
                f"[{wf_label}] {sorted(jobs)} ⊆ 已合入 PR 基线红 {sorted(base)}")
        else:
            evidence_bad.append(
                f"[{wf_label}] {sorted(jobs - base)} 不在已合入 PR 基线红 "
                f"{sorted(base)}")
    if evidence_bad:
        return False, [f"红 job 非基线继承: {'; '.join(evidence_bad)}"]
    return True, [f"继承红放行: {'; '.join(evidence_ok)}"]


def docs_ready(pr, now):
    """docs 快道就绪(门槛 2 的 docs 例外,spec §5 决策表 #9): 第 5 bot
    (local-claude)信号在 ∧ 静默窗 DOCS_QUIET_SEC(900→300)过 ∧ 零可行动
    意见(annotations 完整性/超容差意见同 classify_state 口径)——外部
    4 bot 事件驱动照跑,不等待。"""
    if pr.bot_done.get("local-claude") is None:
        return False
    if pr.annotations_fetch_failed or pr.pending_skew_opinions > 0 \
            or pr.opinion_count > 0:
        return False
    if pr.latest_signal is None:
        return False
    return (now - pr.latest_signal).total_seconds() >= DOCS_QUIET_SEC


def evaluate_merge_gate(state, entry, pr, files, now, no_auto_merge):
    """六门槛全查(spec §5),只判不执行(执行是 execute_merge,T8)。任一
    拒即 return,reasons 携带证据(审计评论/夜间报告用);熔断(PAUSED/
    NO_AUTO_MERGE)前置到入口——零 gh 成本,夜间熔断时省全部后续 gh
    (fix-round 1 Minor #4);entry=None 时仅 docs 快道可过(用户 docs PR,
    用户非 docs PR 结构性排除);no_auto_merge 由 T8 的 no_auto_merge_active()
    检测后传入(裁定#4,熔断存在 → 纯报告模式=灰度)。mergeable 经 getattr
    容错(真实 PRData 无该字段,T8 接线时经 gh pr view 补,缺字段视为未报
    冲突不炸门);fork 是 PRData 既有字段(fix-round 1 Critical #1 补拒)。"""
    fast = docs_only(files)
    # 门槛 5(前置): 熔断(PAUSED / NO_AUTO_MERGE——任一存在即转纯报告模式)
    if tl._paused():
        return GateResult(False, fast, ["PAUSED 生效"])
    if no_auto_merge:
        return GateResult(False, fast, ["NO_AUTO_MERGE 熔断(灰度: 只报告)"])
    # 门槛 1: 归属(引擎条目 ∧ fix/issue-N|fix/ci-N|fix/revert-prN 分支;
    # docs-only 例外: 不要求引擎归属,适用任意 PR)
    if entry is None:
        if not fast:
            return GateResult(False, False, ["非引擎 PR 且非 docs-only——不自动 merge"])
    else:
        if not ENGINE_BRANCH_RE.match(entry.fix_branch or ""):
            return GateResult(False, False, [f"非引擎分支: {entry.fix_branch}"])
    # 门槛 4: 状态(open ∧ 非 DRAFT ∧ 非 fork ∧ 无冲突;WIP 草稿闸只对
    # docs 快道——fork PR 不自动 merge,docs 快道亦不放行)
    if pr.state != "open" or pr.draft:
        return GateResult(False, fast, ["非 open/DRAFT"])
    if fast and wip_gated(pr.title):
        return GateResult(False, True, [f"WIP: 草稿闸({pr.title!r})"])
    if getattr(pr, "fork", False):
        return GateResult(False, fast, ["fork PR——不自动 merge"])
    if getattr(pr, "mergeable", None) is False:
        return GateResult(False, fast, ["与 main 冲突(mergeable=false)"])
    # 门槛 2: 检视(docs 快道 = 第5bot 唯一+300s;全门槛 = classify_state
    # 五 bot READY_TO_MERGE)
    if fast:
        if not docs_ready(pr, now):
            return GateResult(False, True, ["docs 快道未就绪(第5bot/静默窗/意见)"])
    else:
        st_class = tl.pr_loop.classify_state(pr, tl.pr_loop.DEFAULT_QUIET, now)
        if st_class != "READY_TO_MERGE":
            return GateResult(False, False, [f"classify={st_class}≠READY_TO_MERGE"])
    # 门槛 3: CI check(docs 只查快 job;红 job 继承红基线对照在 checks_ok 内)
    ok, why = checks_ok(pr, REQUIRED_CHECKS_DOCS if fast
                        else REQUIRED_CHECKS_FULL, tl.repo_slug())
    if not ok:
        return GateResult(False, fast, why)
    return GateResult(True, fast, why)      # why=继承红放行的证据,审计用


# ============ 泳道3 merge 执行 + 记账(Task 8) ============


def fill_mergeable(pr) -> None:
    """补 PRData 缺失的 mergeable 字段(公开接缝,裁定#1): 真实 PRData
    无该字段,调用方(T10 merge_sweep)过门前先过本函数,门的 mergeable
    False 拒臂才有数据。缺/None 才拉(有值不重复拉);GhError → 保持
    None 并 [warn];gh 空输出 None → 保持 None——门按 T7 过渡语义
    (None 不拒)放行,接线遗漏由 gate 测试锁形。PR 号经双形解析
    pr_number|number(与 execute_merge 同款,fix-round 1 Minor#1——
    同文件同一对象不得两种取号形态)。"""
    if getattr(pr, "mergeable", None) is not None:
        return
    slug = tl.repo_slug()
    num = getattr(pr, "pr_number", None) or getattr(pr, "number", None)
    try:
        v = tl.gh_json_idem(["pr", "view", str(num), "--repo", slug,
                              "--json", "mergeable", "--jq", ".mergeable"])
    except GhError as exc:
        print(f"[warn] PR #{num} mergeable 拉取失败"
              f"(保持 None,门按过渡语义不拒): {exc}")
        return
    if v is None:
        return
    pr.mergeable = v


def no_auto_merge_active() -> bool:
    """NO_AUTO_MERGE 熔断探测(STATE_DIR/NO_AUTO_MERGE 存在即 True): 纯
    存在性探查——幂等、零 gh、零状态变更。门只收 no_auto_merge: bool,
    由编排器调本函数传入(裁定#4: 熔断存在 → 门转纯报告模式=灰度)。"""
    return os.path.exists(
        os.path.join(tl._resolve_state_dir(), NO_AUTO_MERGE_NAME))


def execute_merge(state, entry, pr, gate, dry_run=False) -> "str | None":
    """过门后执行 squash merge: PR 评论贴门槛证据 → `gh pr merge --squash
    --delete-branch` → 回读新 main sha 记 entry.merged_at/merged_sha
    (revert 归因主数据,人工 merge 永不写)→ 引擎条目 DONE + issue 收尾
    评论/关闭(entry 为 None 的用户 docs PR 只 merge+评论)。merge 执行权
    在编排器代码,绝不在 LLM(spec §1 决策 7)。失败语义按不可逆点(merge
    本身)分界(fix-round 1 Important #1/3): 前侧评论/merge 步 GhError
    原样上抛——条目不置 DONE,下轮门复评;后侧两类例外都不上抛——sha
    回读失败/空降级为 merged_sha=None + [warn](已合入是事实,记账反映
    现实,崩溃不撤销 merge 只丢账),issue close 失败被 [warn] 吞(state
    为权威)。PR 号经双形解析 pr_number|number(真实 PRData 字段名是
    number,与 checks_ok/T5 _pr_number 同款契约)。"""
    slug = tl.repo_slug()
    num = getattr(pr, "pr_number", None) or getattr(pr, "number", None)
    evidence = "\n".join(f"- {r}" for r in gate.reasons) or "- 全门槛通过"
    body = (f"🤖 task-loop pipeline 自动 merge(spec 2026-11-12):\n"
            f"- 门槛证据:\n{evidence}\n"
            f"- 方式: squash + delete-branch;"
            f"{'docs-only 快速通道' if gate.fast else '全门槛'}")
    if dry_run:
        print(f"[dry-run] 将 merge PR #{num}: {body[:120]}…")
        return None
    tl.gh_json_idem(["pr", "comment", str(num), "--repo", slug,
                     "--body", body])
    tl.gh_json_idem(["pr", "merge", str(num), "--repo", slug,
                     "--squash", "--delete-branch"])
    raw = None
    try:
        raw = tl.gh_json_idem(
            ["pr", "view", str(num), "--repo", slug, "--json",
             "mergeCommit", "--jq", ".mergeCommit.oid"])
    except GhError as exc:
        print(f"[warn] PR #{num} mergeCommit 回读失败"
              f"——已合入但 merged_sha 缺: {exc}")
    merged_sha = (raw or "").strip() or None
    if merged_sha is None:
        print(f"[warn] PR #{num} 已 squash merge 但 sha 回读为空"
              f"——merged_sha=None(revert 归因不可见该 PR)")
    if entry is not None:
        entry.merged_at = _now_local_iso()
        entry.merged_sha = merged_sha
        entry.status = "DONE"
        entry.updated_at = _now_local_iso()
        if entry.issue_number is not None:
            tl.comment_on_issue(
                entry.issue_number,
                f"修复 PR #{num} 已由 pipeline 自动 squash merge"
                f"(main {(merged_sha or '?')[:12]})。本 issue 闭环。")
            try:
                tl.gh_json_idem(["issue", "close", str(entry.issue_number),
                                 "--repo", slug])
            except GhError as exc:
                print(f"[warn] issue 关闭失败(state 为权威): {exc}")
    print(f"[merge] PR #{num} 已自动合入 → {(merged_sha or '?')[:12]}")
    return merged_sha


# ============ 精确归因自动 revert(Task 9) ============

REVERT_FOR_FMT = "revert-for: PR #{pr} @ {sha}"
REVERT_MARKER_FMT = "<!-- task-loop:revert:{sha} -->"


def _revert_issue_exists(slug, merged_sha):
    """双态 marker 幂等搜索(fix-round 1 Important#1 + I3 裸 sha 查询词,
    先例直接移植 tl._search_audit_issue/_search_finding_issue 的 gh search
    issues 形态,检视#35 同一缺陷类): open 与 closed 各查一次,body 含
    完整 REVERT_MARKER_FMT marker 即命中——查询词用**裸 sha**(正文
    `revert-for: PR #N @ <sha>` 是纯文本必被索引;HTML 注释
    `<!-- task-loop:revert:<sha> -->` 有引擎已知的正文索引漏命中特征,
    押它会让漏命中每轮重复建 issue),搜索宽松命中但正文无完整 marker 的
    干扰项被 Python 侧复核滤掉(与 audit 先例同款双保险)。closed 命中
    兑现「人工关闭的 revert issue 不重建」的人工接管语义。LOOKBACK_DAYS
    窗内坏 merge 未修时红 run 每轮重见,引擎自书不变量「幂等由 marker
    查重保证」(ci 源 _search_marker_issues / audit 源 _search_audit_issue
    同款)在此兑现;state revert_seen(I3 主查重,调用方先查)命中时本
    函数不被触达——它是 state 丢失/重启后的兜底闸。"""
    marker = REVERT_MARKER_FMT.format(sha=merged_sha)
    for st in ("open", "closed"):
        res = tl.gh_json(["search", "issues", "--repo", slug,
                          "--state", st, "--json", "number,body",
                          "--limit", "20",
                          merged_sha])
        for it in (res if isinstance(res, list) else []):
            if marker in (it.get("body") or ""):
                return True
    return False


def revert_attribution(state, red_runs, dry_run=False):
    """精确归因才回滚(spec §6): 红 run headSha(scan_red_runs 真实键——
    gh run list --json 的 camelCase 原始 dict;brief 假设的 head_sha/
    html_url 蛇形键以实码对齐)命中本流水线自动 merge 的 merged_sha 集 →
    建 revert issue(走 issue 队列→泳道1,分支随 issue 约定 fix/issue-<N>;
    spec 的 fix/revert-pr<N> 降级为标题约定,不为命名改 dispatch)。人工
    merge 的 sha 永不在集(merged_sha 只由 execute_merge 写;失败/回读空
    → None 自然不命中)。归因不到 → 空清单(前向修复语义不变,intake 照常
    建普通红 issue——语义在 T10 接线)。source=manual/kind=bug 按真实签名
    create_task_issue(title, body, labels) 编码(grep 实核,brief 的
    source/kind 参数不存在): 正文无 source marker → reconcile 归 manual
    (task-loop:revert marker 不入 _SOURCE_MARKER_RES),agent-task+
    agent-bug 标签 → kind=bug(ci 红路径同款协议);labels 由编排层
    ensure_agent_labels 幂等就位。二阶链闸(防 revert 打乒乓球,链深 ≤1):
    命中条目的 issue 标题以 [revert] 开头 = 该 PR 本身是 revert 产物 →
    打印 escalation 转人工(夜间报告可见),不自动建「revert 的 revert」;
    标题探针拉取失败同样转人工([warn] 留痕,宁严勿松——红 run 仍由 intake
    前向修复,零丢失)。同批内同一 merge 的多个红 run(多 workflow 同 sha
    皆红是常态)只建一个 revert issue——revert 的对象是坏 merge 不是红
    run,不去重则双 agent 同 revert 竞态;批内去重打印前缀 [revert-dedup],
    与二阶链升级提示 [revert] 分离(T10 夜报 grep "revert" 时去重噪声不得
    混入人工升级信号)。

    返回 (issues, absorbed_run_ids) 二元组(I2,2026-11-13 终审): issues =
    本批新建 revert issue 号清单;absorbed_run_ids = 已由 revert issue 接管
    的红 run databaseId 集(建成的 + 查重命中已存在的——含同批同 merge 去重
    与 state/gh 双层查重命中,幂等即吸收)。cycle_once 把 absorbed 的 run
    从传给 triage 的清单里滤除——归因命中的红 run 若再走前向修复,同一故障
    会同时有 revert agent 与「修复」agent 互相冲突。查重搜索失败/标题探针
    失败/二阶链命中转人工的 run **不**吸收(照旧前向修复,维持 T9 兜底
    零丢失)。dry_run: 拟建输出即真实将建集,absorbed 同步收录(预演口径
    与真实一致),state 零写(不记 revert_seen)。

    跨调用幂等双层(I3): ①state["revert_seen"] 主查重——建成 revert issue
    即把 sha 记入(state 键属 _pipeline_save「其余键」类,pipeline 唯一写者,
    落盘随 cycle 走;查重先看 state,命中零 gh);②_revert_issue_exists 双态
    (open+closed)marker 搜索兜底(fix-round 1 Important#1,先例移植
    _search_audit_issue——state 丢失/重启后仍「人工关闭的 revert issue 不
    重建」;查询词裸 sha,正文 revert-for 纯文本必被索引)。镜像 audit 的
    audit_seen 双源设计。搜索失败幂等不可证 → 宁严勿松转人工(不吸收)。"""
    merged = {}
    for e in tl.StateStore.entries(state):
        if e.merged_sha and e.pr_number:
            merged[e.merged_sha] = e
    issues = []
    absorbed = set()
    seen_merges = set()
    for run in red_runs:
        hit = merged.get(run.get("headSha"))
        if hit is None:
            continue
        if hit.merged_sha in seen_merges:
            print(f"[revert-dedup] merge {hit.merged_sha[:12]} 本批已处理"
                  f"——红 run {run.get('url')} 同 merge 去重跳过")
            absorbed.add(run.get("databaseId"))
            continue
        seen_merges.add(hit.merged_sha)
        # I3 主查重①: state revert_seen(pipeline 唯一写者,跨轮第一道闸,
        # 命中零 gh)。红 run 在 LOOKBACK_DAYS 窗内每轮重见,不查重则同一
        # 坏 merge 反复建 revert issue(双 agent 同 revert 竞态/attempts
        # 燃烧/假性 ESCALATED 唤醒人工)。零建成不落键(setdefault 会把空键
        # 塞进 state——查重只在建成路径记账)。
        if hit.merged_sha in state.get("revert_seen", []):
            print(f"[revert-dedup] merge {hit.merged_sha[:12]} 已在 state "
                  f"revert_seen(本流水线建过 revert issue)——跳过")
            absorbed.add(run.get("databaseId"))
            continue
        # I3 主查重②(兜底闸): 建 issue(含 dry-run 打印)之前双态 marker
        # 搜索——覆盖 state 丢失/重启场景。搜索失败幂等不可证 → 宁严勿松
        # 转人工(误建代价 > 漏建,漏建仍有 intake 前向修复兜底,不吸收)。
        try:
            if _revert_issue_exists(tl.repo_slug(), hit.merged_sha):
                print(f"[revert-dedup] merge {hit.merged_sha[:12]} 已有 "
                      f"revert issue(双态查重命中,人工关闭亦不重建)——跳过")
                absorbed.add(run.get("databaseId"))
                continue
        except GhError as exc:
            print(f"[warn] revert 查重搜索失败——幂等不可证,转人工"
                  f"(宁严勿松,夜间报告可见): {exc}")
            continue
        # 二阶链闸: revert PR 也是引擎 PR,其 merge 再红且归因命中时不
        # 自动建「revert 的 revert」。正文不可得,链深按命中条目 issue
        # 标题的 [revert] 前缀判定(标题约定由本函数建 issue 处保证);
        # 无 issue 条目不可能是 revert 产物(revert 必由 issue 派生)。
        if hit.issue_number:
            try:
                is_revert_product = tl.issue_title_starts(
                    hit.issue_number, "[revert]")
            except GhError as exc:
                print(f"[warn] issue #{hit.issue_number} 标题拉取失败"
                      f"——二阶链不可判,转人工(宁严勿松,夜间报告可见): "
                      f"{exc}")
                continue
            if is_revert_product:
                print(f"[revert] PR #{hit.pr_number} 本身是 revert 产物"
                      f"——二阶链不自动回滚,转人工(夜间报告可见)")
                continue
        body = (f"自动 merge 的 PR #{hit.pr_number}({hit.merged_sha[:12]})后,"
            f"main 红 run [{run.get('workflowName')}]({run.get('url')}"
            f")——精确归因命中,请 revert:\n\n"
            f"```\ngit revert -m 1 {hit.merged_sha}\n```\n\n"
            f"{REVERT_FOR_FMT.format(pr=hit.pr_number, sha=hit.merged_sha)}\n"
            f"{REVERT_MARKER_FMT.format(sha=hit.merged_sha)}\n\n"
            f"完成后以结束语 `task-loop: done <fix_branch>` 收尾。")
        if dry_run:
            print(f"[dry-run] 将建 revert issue: {body[:100]}…")
            absorbed.add(run.get("databaseId"))   # 拟建即真实将建集(预演口径)
            continue
        num = tl.create_task_issue(
            title=f"[revert] PR #{hit.pr_number} 自动 merge 后 main 红"
                  f"({run.get('workflowName')})",
            body=body,
            labels=[tl.AGENT_LABEL_TASK, tl.AGENT_LABEL_BUG])
        issues.append(num)
        # I3 主查重记账: 建成即记 sha(下一轮 state 命中零 gh)
        state.setdefault("revert_seen", []).append(hit.merged_sha)
        absorbed.add(run.get("databaseId"))
        # 临时观察键: T10 落盘 state 时无害但留意(非契约字段,测试断言用)
        state["_last_revert_body"] = body
    return issues, absorbed


# ============ 编排器主循环 + 软停 + 夜报 + CLI(Task 10) ============


def _pipeline_save(store, state):
    """pipeline 的 state 落盘唯一通道: scan.lock 内 merge-save。

    键分三类(controller 裁定 2026-11-13,fix-round 1 Important#1):
    - audit 独占键(AUDIT_OWNED_KEYS = audit_cursor/audit_seen): audit 子进程
      经 _audit_state_merge_write 锁下写,pipeline 不写 → 磁盘权威直接回填
      (pipeline 整夜持有内存 state,裸 save 会把 audit 同窗推进写回旧值,
      生产事故 9d6ba452 同类丢记账)。
    - 双写者键 auth_failure_streak/auth_failure_at: pipeline 的
      reap_or_advance 每轮在内存 bump/归零,audit 子进程亦在锁下
      merge-write(_audit_streak_add) → 合并取**近似**(宁严勿松): streak=
      max(盘,内存)——两源任一见 ≥3 连败即触发熔断;成功归零可能被另一线
      未归零值掩盖 → 熔断宁早勿晚(fail-safe)。at=取两侧较新(缺侧取有;
      两侧皆 _now_iso 同形,字典序即时间序)。
    - 其余键: pipeline 唯一写者,内存为准(revert_seen——I3 revert 主查重
      记账——同此类: audit 子进程不写它,无需合并特殊处理)。
    合并结果同步回内存对象——下一轮的 bump 从合并值起步,不再被磁盘旧值
    抹掉(裸拷的回归点)。阻塞 flock 的临界区只有 load+save(亚秒级);常态
    下 scan.lock 唯一的生产持有者就是 audit 合并写。过渡期注意(crontab
    3 删 2 增部署前): 旧 cron 的 scan tick 仍整段持有 scan.lock(分钟级),
    pipeline 落盘与之串行(有界等待,不构成死锁)。"""
    def _merge():
        disk = store.load()
        for k in AUDIT_OWNED_KEYS:
            if k in disk:
                state[k] = disk[k]
        if "auth_failure_streak" in disk:
            state["auth_failure_streak"] = max(
                int(disk["auth_failure_streak"]),
                int(state.get("auth_failure_streak", 0)))
        mem_at = state.get("auth_failure_at")
        disk_at = disk.get("auth_failure_at")
        if mem_at is not None and disk_at is not None:
            state["auth_failure_at"] = max(mem_at, disk_at)
        elif disk_at is not None:
            state["auth_failure_at"] = disk_at
        # mem_at 有而盘无 → 内存值本就在 state,不动
        store.save(state)
    tl._with_scan_lock(_merge)


def needs_review_list():
    """全部 open PR 中当前 head 无第 5 bot 信号者(spec §4 泳道2 取材)。
    already_reviewed 是 REST reviews 的 commit 锚定幂等判据(T6 报告已核);
    畸形行/空 headRefOid 宁跳过不炸扫(gh_json 非输出 JSON 的子命令原样
    返回文本,gh 空输出 None 契约 → (out or "") 防裸崩,pr_files 先例)。"""
    import pr_local_review
    out = tl.gh_json_idem(["pr", "list", "--state", "open", "--json",
                           "number,headRefOid", "--jq",
                           ".[] | [.number, .headRefOid] | @tsv"])
    prs = []
    for line in (out or "").splitlines():
        if not line.strip():
            continue
        num_s, sep, head = line.partition("\t")
        head = head.strip()
        if not sep or not head:
            continue
        try:
            num = int(num_s.strip())
        except ValueError:
            continue
        if not pr_local_review.already_reviewed(num, head):
            prs.append((num, head))
    return prs


def open_docs_prs():
    """open PR 三元组(号/标题/head)取材: WIP 过滤(wip_gated)与 docs-only
    过滤(docs_only)都在 merge_sweep 的门评前做——本函数只取材,spec §5
    docs 快道是门的事不是取材的事。"""
    out = tl.gh_json_idem(["pr", "list", "--state", "open", "--json",
                           "number,title,headRefOid", "--jq",
                           ".[] | [.number, .title, .headRefOid] | @tsv"])
    prs = []
    for line in (out or "").splitlines():
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        try:
            num = int(parts[0].strip())
        except ValueError:
            continue
        prs.append({"pr_number": num, "title": parts[1],
                    "head": parts[2].strip()})
    return prs


def audit_line_running(state):
    """audit 后台子进程是否在跑(槽 kind=audit,满 1 即在跑)。"""
    return free_slot_count(state, "audit") == 0


def _seconds_to_window_end(now, end_hour):
    """距窗口结束(本地 end_hour:00 的下一次出现)的秒数——与 in_window
    同款本地时钟口径(跨午夜窗: 当前时刻已过 end_hour 时取次日)。"""
    end = now.replace(hour=end_hour, minute=0, second=0, microsecond=0)
    if end <= now:
        end += timedelta(days=1)
    return (end - now).total_seconds()


def spawn_audit(state, dry_run, window_end=9, now=None):
    """audit 后台线(spec §2): 槽空则 spawn 一发 `task_loop.py audit`
    (timeout AUDIT_LINE_TIMEOUT_SEC=7500s ≈ 2h;切块审查 3000 行/轮,
    余量由后续轮次续审接力)。run_audit 自带 PAUSED/audit.lock/内存外的
    全部闸,编排层只补内存闸(spec §7「audit 同过闸」)与克隆目录存在性
    (cmd_audit 同款前置,缺目录时 run_background 的 Popen 直接
    FileNotFoundError 炸整轮)。窗口末端守卫(fix-round 1 Minor#1): 距窗口
    结束剩余 < AUDIT_LINE_TIMEOUT_SEC(audit 自身 timeout 预算)不再
    spawn——audit 唯一持久化是收尾一次 merge-write,09:00 软停击杀会丢掉
    最长 ~2h 的在审块(白审)。now 可注入(裁定#9 时钟纪律),缺省本地钟
    (与 in_window 同族;cycle_once 传入的 now 已归一为 UTC,本地小时
    算术须本地钟,故不接 cycle 时钟)。dry_run 只打印意图零 spawn。"""
    if now is None:
        now = datetime.now()
    if audit_line_running(state):
        return
    if tl._paused():
        print("[audit] PAUSED 熔断生效,本轮不 spawn audit 线")
        return
    if not mem_gate_ok():
        print(f"[audit] 内存闸不足({min_mem_gb()}GB),本轮不 spawn audit 线")
        return
    clone_dir = tl._resolve_clone_dir()
    if not os.path.isdir(clone_dir):
        print(f"[audit] 克隆目录不存在: {clone_dir}——本轮不 spawn audit 线")
        return
    remaining = _seconds_to_window_end(now, window_end)
    if remaining < AUDIT_LINE_TIMEOUT_SEC:
        print(f"[audit] 距窗口结束仅 {int(remaining)}s(< 审查预算 "
              f"{AUDIT_LINE_TIMEOUT_SEC}s)——不再 spawn audit 线"
              f"(软停会击杀在审块,收尾 merge-write 丢失)")
        return
    if dry_run:
        print(f"[dry-run] 将 spawn audit 后台线(audit-nightly,"
              f"timeout {AUDIT_LINE_TIMEOUT_SEC}s)")
        return
    log = os.path.join(tl._resolve_state_dir(), "audit.log")
    inner = (f"timeout {AUDIT_LINE_TIMEOUT_SEC} "
             f"/usr/bin/python3 scripts/task_loop.py audit")
    cmd = f"nohup setsid bash -c {shlex.quote(inner)} >> {shlex.quote(log)} 2>&1 & echo $!"
    pid = tl.run_background(cmd, cwd=clone_dir, log=log)
    if not slot_acquire(state, "audit", "audit-nightly", pid, pid, log, None,
                        _now_local_iso(), boot_id()):
        # 与 dispatch_agent/dispatch_review 同款击杀网: 只剩同轮竞态,
        # killpg 整组击杀刚派生的 audit 线不留孤儿(setsid 使 PID 即 pgid)。
        try:
            os.killpg(pid, 9)
        except OSError:
            pass
        print(f"[audit] audit 槽占位失败(同 key 竞态),已击杀 PID {pid}")
        return
    print(f"[audit] audit 后台线已 spawn(PID {pid},日志 {log})")


def _upsert_event(events, ev):
    """同 (kind,pr) 事件去重-更新: events 是夜报数据源,merge 门每
    PIPELINE_CYCLE_SEC 轮询(一夜最多 ~120 轮),不做重复账——同键再判据
    时原位更新为最新门槛证据(CI 红→绿的演化在报告里反映为最新一次)。"""
    for i, old in enumerate(events):
        if old.get("kind") == ev["kind"] and old.get("pr") == ev.get("pr"):
            events[i] = ev
            return
    events.append(ev)


def _pipeline_night(now: datetime) -> str:
    """评估时刻所属 pipeline 夜的标识(ISO 日期串 = 该夜 23:00 的本地日期)。

    夜 = **pipeline 夜**(23:00 起跨午夜到 09:00),不是本地日历日——
    23:30 与次日 01:30 同属一夜;本地日期在 00:00 的翻转会把一夜劈成
    两夜,连续冲突夜计数被虚增(I6 fix-round 2)。h<9 的凌晨段归昨晚
    起始的夜((date-1));h≥23 归当日;窗口外 09:00–23:00(夜间编排恒在
    窗内,仅手动 scan 会发生)归**即将开始**的一夜(当日)——22:30 的
    评估与 23:30 起的同夜评估同桶,同桶不重复计数(宁少勿多,防假性
    ESCALATED)。与 in_window 同族本地钟口径(run_pipeline/cycle_once 传入
    的 now 即本地钟)。"""
    if now.hour < 9:
        return (now.date() - timedelta(days=1)).isoformat()
    return now.date().isoformat()


def _merge_one(state, events, num, entry, pr, files, now, no_auto_merge, dry_run, es):
    """单 PR 门评+执行/记录(引擎与 docs 共用): ok → execute_merge(引擎条目
    记账即时回写落盘——merged_sha 是 revert 归因主数据,丢账 = 归因失明);
    NO_AUTO_MERGE 熔断拒 → 复评一次不带熔断(灰度纯报告模式的「本应
    merge+门槛证据」,spec §11.4;T7 把熔断前置到门入口省后续 gh,这里
    的复评 gh 成本是灰度夜报证据的代价,只读幂等);复评过门记 would-merge
    绝不执行,不过记 gate-reject 携真实 reasons。

    I6(spec §5 承诺补全 + fix-round 2 夜边界锚定,引擎条目): 与 main 冲突
    的拒因(mergeable=false →「与 main 冲突」)按 _pipeline_night 口径
    **逐夜** conflict_nights +1——同夜不重复计数(last_conflict_night 夜锚:
    run_pipeline 整夜持内存 state、每 PIPELINE_CYCLE_SEC 一轮、merge_sweep
    是尾步,冲突条目当夜被评上百次,每评 +1 会在当夜第 2 轮 cycle 即假性
    2 → 假性 ESCALATED,升级文案还谎称「已持续 2 晚」)。计数在 no_auto_merge 复评
    之后(熔断夜的真实拒因同样可见);连续第 2 夜(≥2)首次评估 →
    tl._escalate(复用升级路径,文案引用 pipeline 夜口径与 spec §5: 不自动
    rebase,rebase 决策留给人工;升级即终局即时落盘);过门/非冲突拒 →
    conflict_nights 与 last_conflict_night **双清零**(只数连续冲突夜;只清
    计数不清夜锚,清零后同夜再冲突会被夜锚错误吞掉)。计数写回 state
    dict——reject 路径无既有写回,不写回则每轮从旧 dict 重新解析,永远
    停在 1、升级结构性不可达。"""
    gr = evaluate_merge_gate(state, entry, pr, files, now, no_auto_merge)
    if gr.ok and entry is not None:
        entry.conflict_nights = 0               # 过门清零(只数连续冲突夜)
        entry.last_conflict_night = None
    if gr.ok:
        sha = execute_merge(state, entry, pr, gr, dry_run)
        if entry is not None:
            state["entries"] = [x.to_dict() for x in es]
            if not dry_run:
                _pipeline_save(StateStore(), state)
        _upsert_event(events, {"kind": "merge" if entry is not None
                               else "merge-docs", "pr": num, "sha": sha})
        return
    if no_auto_merge and any("NO_AUTO_MERGE" in r for r in gr.reasons):
        gr = evaluate_merge_gate(state, entry, pr, files, now, False)
    if entry is not None:
        if any("冲突" in r for r in gr.reasons):
            night = _pipeline_night(now)
            if entry.last_conflict_night != night:
                entry.conflict_nights += 1
                entry.last_conflict_night = night
            if entry.conflict_nights >= 2:
                tl._escalate(
                    entry, state,
                    f"与 main 冲突已持续 {entry.conflict_nights} 晚"
                    f"(pipeline 夜 23:00–09:00 逐夜计数,spec §5: 不自动 "
                    f"rebase——rebase 决策留给人工)",
                    "人工 rebase/merge 该 PR,或人工关闭之", dry_run)
                # 升级即终局: 计数写回 + 即时落盘(与 lane_b 派发记账同纪律
                # ——进程随时可能死,终态转换不能只活在内存)
                state["entries"] = [x.to_dict() for x in es]
                if not dry_run:
                    _pipeline_save(StateStore(), state)
        else:
            entry.conflict_nights = 0           # 非冲突拒因清零(双清)
            entry.last_conflict_night = None
        state["entries"] = [x.to_dict() for x in es]
    _upsert_event(events, {"kind": "would-merge" if gr.ok else "gate-reject",
                           "pr": num, "reasons": gr.reasons})


def merge_sweep(state, events, dry_run, now=None):
    """merge 门扫: 引擎条目(PR_OPEN ∧ 有 PR——BOT_FIX 已退场者在同轮
    早段经 lane_b 归还 PR_OPEN;repair 槽活 agent 在跑的 PR 等它退场)+
    用户 open docs PR(引擎条目的 PR 不双扫,防同 PR 双评估/双 merge)。
    NO_AUTO_MERGE 生效 → 门拒后复评不带熔断、只记 would-merge/gate-reject
    事件,绝不执行(裁定#3: no_auto_merge 只传参,不用它跳过 docs 循环——纯报告
    模式要能看到 would-merge 证据)。单 PR GhError 只跳过该 PR(lane_b
    单条目降级同口径,不炸整扫)。fetch_pr 后、过门前 fill_mergeable
    (T8 载荷: 真实 PRData 无 mergeable 字段)。"""
    if now is None:
        now = datetime.now(timezone.utc)
    no_auto_merge = no_auto_merge_active()
    slug = None
    es = tl.StateStore.entries(state)
    for e in es:
        if e.status != "PR_OPEN" or e.pr_number is None:
            continue
        if any(s["key"] == f"pr-{e.pr_number}" and tl.agent_alive(s["pid"])
               for s in state.get("slots", {}).get("pr_repair", [])):
            continue                      # 修复 agent 在跑,等它退场
        if slug is None:
            slug = tl.repo_slug()
        try:
            pr = tl.pr_loop.fetch_pr(slug, e.pr_number)
            fill_mergeable(pr)
            files = tl.pr_files(e.pr_number)
            _merge_one(state, events, e.pr_number, e, pr, files, now, no_auto_merge,
                       dry_run, es)
        except GhError as exc:
            print(f"[merge-sweep] PR #{e.pr_number} 评估失败: {exc}")
    engine_prs = {x.pr_number for x in es if x.pr_number is not None}
    for d in open_docs_prs():
        num = d["pr_number"]
        if num in engine_prs:
            continue      # 引擎条目(含 docs-only 引擎 PR)已走上方循环
        if wip_gated(d["title"]):
            continue                      # WIP: = 用户显式不同意当夜合入
        if slug is None:
            slug = tl.repo_slug()
        try:
            pr = tl.pr_loop.fetch_pr(slug, num)
            files = tl.pr_files(num)
            if not docs_only(files):
                continue
            fill_mergeable(pr)
            _merge_one(state, events, num, None, pr, files, now, no_auto_merge,
                       dry_run, es)
        except GhError as exc:
            print(f"[merge-sweep] docs PR #{num} 评估失败: {exc}")


def cycle_once(state, dry_run, events, now, window_end=9):
    """单轮循环(裁定#2 顺序载荷,不可破坏——同 key 死槽依赖 janitor 先清,
    janitor 先于一切派发): intake(scan_red_runs → revert 归因 → triage)
    → janitor → 收割 → 对账 → 自愈 → 泳道1 派发 → 泳道2 派发 → 泳道3
    (lane_b) → audit 后台线 → merge 门扫。revert 转人工/去重信号在
    revert_attribution 的 stdout(经 cron 重定向归档 pipeline.log,夜报
    正文引路径),本函数只收其返回的 (issue 清单, absorbed 集) 二元组
    (裁定#4 + I2: absorbed 的红 run 从 triage 前向修复清单滤除,防 revert
    与「修复」双 agent 对同一故障互踩)。window_end 仅供 spawn_audit 的窗口
    末端守卫用(fix-round 1 Minor#1,run_pipeline 的 --window-end 透传;
    缺省 9 与 spec 窗口一致)。"""
    if now.tzinfo is None:
        now = now.astimezone()   # brief 调用形态 naive → 感知(docs_ready 减法需感知时)
    runs = tl.scan_red_runs()
    issues, absorbed_run_ids = revert_attribution(state, runs, dry_run)
    if issues:
        events.append({"kind": "revert-issue", "issues": issues})
    # I2(2026-11-13 终审): 归因命中的红 run 已由 revert issue 接管(absorbed
    # = 建成 + 查重命中已存在),从 triage 前向修复清单里滤除——不滤则同一
    # 故障同时建 revert issue 与「修复」issue,双 agent 互踩。查重/探针失败
    # 转人工的 run 不在 absorbed 里,照旧前向修复(T9 兜底,零丢失)。
    fwd_runs = [r for r in runs
                if r.get("databaseId") not in absorbed_run_ids]
    tl.triage_new_red_runs(state, fwd_runs, dry_run=dry_run)
    released = slot_janitor(state, _now_local_iso())
    if released:
        events.append({"kind": "slot-released", "released": released})
    tl.reap_or_advance(state, dry_run=dry_run)
    open_labels = tl.reconcile_issues(state, dry_run=dry_run)
    tl.heal_clone(state, dry_run=dry_run)
    # 泳道1: state 队列 FIFO × 空槽——取材含 QUEUED 与「DISPATCHED 且无
    # agent」(dead/timeout 经 _requeue_or_escalate 保持 DISPATCHED 待下一
    # 轮重派;只扫 QUEUED 会让这一群永久滞留,scan 旧编排由重派扫消化)。
    es = tl.StateStore.entries(state)
    lane1 = sorted(
        (e for e in es
         if e.status == "QUEUED"
         or (e.status == "DISPATCHED" and e.agent_pid is None)),
        key=lambda e: (e.issue_number is None, e.issue_number or 0))
    for e in lane1:
        if free_slot_count(state, "issue_fix") <= 0:
            break
        if not tl._dispatch_gate_ok(e, open_labels):
            continue
        pid = tl.dispatch_agent(e, state, dry_run=dry_run)
        # 回写+即时落盘(scan 检视#6/Task 4 R2 纪律): Entry 是解析副本,
        # 不回写则 save 落旧 dict,派发记账全丢 = 每轮重派。
        state["entries"] = [x.to_dict() for x in es]
        if pid is not None and not dry_run:
            _pipeline_save(StateStore(), state)
    # 泳道2: 检视 × 空槽——PAUSED 熔断同闸(紧急停机覆盖全部派发,spec §1
    # 人工保留点;dispatch_agent/run_audit 自带闸,检视线在编排层拦);
    # dry_run 只打印意图零派发(dry-run 零 spawn 契约)。
    if tl._paused():
        print("[paused] 泳道2 检视派发被 PAUSED 闸门拦下")
    else:
        for num, head in needs_review_list():
            if free_slot_count(state, "pr_review") <= 0:
                break
            if dry_run:
                print(f"[dry-run] 将派第 5 bot 检视 PR #{num}"
                      f"(head {head[:8]})")
                continue
            dispatch_review(state, num, head)
    # 泳道3: 车道 B(其派发已槽位化为 pr_repair)
    tl.lane_b_advance(state, dry_run=dry_run)
    # audit 后台线
    spawn_audit(state, dry_run, window_end=window_end)
    # merge 门
    merge_sweep(state, events, dry_run, now)


def write_report(state, events, dry_run):
    """夜报 report-YYYYMMDD.md + state["last_report"](窗口结束由
    run_pipeline 调,status 顶部摘要读 last_report)。revert 转人工/升级/
    去重留痕只在 stdout(经 cron 重定向归档 pipeline.log),正文引其路径
    (裁定#4)。dry_run 只返回路径零写。"""
    day = datetime.now().strftime("%Y%m%d")
    path = os.path.join(tl._resolve_state_dir(), f"report-{day}.md")
    body = [f"# pipeline 夜间报告 {day}", "",
            f"- 事件数: {len(events)}"]
    for ev in events:
        body.append(f"- {ev}")
    log_path = os.path.join(tl._resolve_state_dir(), "pipeline.log")
    body += ["",
             f"- 人工介入信号(转人工/升级/查重/去重留痕)见 {log_path}"]
    if not dry_run:
        os.makedirs(tl._resolve_state_dir(), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(body) + "\n")
        state["last_report"] = os.path.basename(path)
    return path


def soft_stop(state, grace_sec=SOFT_STOP_GRACE_SEC):
    """09:00 软停(spec §4): 在跑 agent 先给 ≤grace_sec 自然退出,超时击杀;
    条目干净回队不计 attempts(#611 attempts=4 假性升级教训);清 worktree。
    击杀两段: SIGTERM 让其收尾,SOFT_STOP_KILL_GRACE_SEC 后对仍活者 SIGKILL
    补刀(_kill_agent_if_alive 同款纪律——宁可补刀不留无主活进程)。回队
    循环只认 issue_fix/pr_repair 槽(fix-round 1 Important#2): pr_review 与
    pr_repair 槽对同一 PR 共用 key f"pr-<N>"(泳道2+3 并行是 spec 设计流,
    reviewer 7500s 超时 > 300s 轮询,碰撞不罕见),按 key 匹配任意活槽会把
    BOT_FIX 条目误降 QUEUED、或把 pr_repair 臂刚写的 PR_OPEN 再抹掉——
    pr_review/audit 槽只击杀清槽,条目归其所属泳道的槽处置。自然退场的槽
    只清槽/worktree,条目不动(真实结局归下一轮/次夜 reap 记账,软停不冒充
    收割)。报告由调用方写(run_pipeline)。"""
    deadline = time.time() + grace_sec
    while time.time() < deadline:
        live = [s for lst in (state.get("slots") or {}).values()
                for s in lst if tl.agent_alive(s["pid"])]
        if not live:
            break
        print(f"[soft-stop] 等待 {len(live)} 个 agent 自然退出"
              f"(剩余 {int(deadline - time.time())}s)")
        time.sleep(60)
    entries = tl.StateStore.entries(state)
    killed = []
    # 第一段: SIGTERM + 条目干净回队(只动 status/引用,不动 attempts/rounds;
    # 仅 AGENT_STATES 条目——终态条目不复活。清 head 锚与 lane_b 退场归还
    # 路径对称,否则同 head 单派规则把重派拦到死)。
    for kind, lst in list((state.get("slots") or {}).items()):
        for s in lst:
            if not tl.agent_alive(s["pid"]):
                continue
            try:
                os.killpg(s.get("pgid") or s["pid"], 15)
            except OSError:
                pass
            killed.append(s["key"])
            # 泳道过滤(fix-round 1 Important#2): 检视/audit 槽只击杀清槽,
            # 不动条目——pr_review 槽 key 与 pr_repair 槽同形 f"pr-<N>",
            # 按 key 匹配任意活槽会把 BOT_FIX 条目误降 QUEUED(pr_review 槽
            # 命中时 kind≠pr_repair),同 key 双槽时还会把 pr_repair 臂刚写
            # 的 PR_OPEN 按 dict 序再抹掉。
            if kind not in ("issue_fix", "pr_repair"):
                continue
            for e in entries:
                if e.status not in tl.AGENT_STATES:
                    continue
                if s["key"] in (f"issue-{e.issue_number or e.run_id}",
                                f"pr-{e.pr_number}"):
                    e.status = "PR_OPEN" if kind == "pr_repair" else "QUEUED"
                    e.agent_pid = None
                    e.agent_started_at = None
                    e.last_dispatched_head = None
                    e.updated_at = _now_local_iso()
    # 第二段: SIGKILL 补刀(仍活者)
    if killed:
        time.sleep(SOFT_STOP_KILL_GRACE_SEC)
        for kind, lst in list((state.get("slots") or {}).items()):
            for s in lst:
                if tl.agent_alive(s["pid"]):
                    try:
                        os.killpg(s.get("pgid") or s["pid"], 9)
                    except OSError:
                        pass
    # 槽清 + worktree 清(自然退场者同样清: worktree 是易耗品,软停是
    # 确定性收尾点;槽不清则明晚启动对账前 janitor 还得替软停收尾)。
    for kind in list((state.get("slots") or {}).keys()):
        for s in state["slots"][kind]:
            if s.get("worktree"):
                tl.worktree_cleanup(s["worktree"])
        state["slots"][kind] = []
    # 记账回写: Entry 是解析副本,不回写则调用方 save 落旧 dict(软停推进全丢)。
    state["entries"] = [e.to_dict() for e in entries]
    print(f"[soft-stop] 击杀 {len(killed)} 个超时 agent(不计 attempts): "
          f"{killed}")


def run_pipeline(dry_run=False, window_start=23, window_end=9) -> int:
    """夜间窗口循环(spec §2): cron `0 23` 拉起,pipeline.lock flock
    LOCK_EX|LOCK_NB(占用 → 打印+退出 0,锁检查先于一切副作用,R4);
    前奏(迁移/标签/清检视每晚计数/启动对账)→ 窗口内每 PIPELINE_CYCLE_SEC
    一轮 cycle_once(GhError 单轮消化不炸整窗;其余异常同样消化续跑——
    10h 窗口内一次意外不该弃守整夜)→ 出窗软停 → 报告+落盘。
    dry_run: 单轮全流程预演,零本地写(不建目录/不碰锁/state 不落盘/报告
    不写,scan dry-run 检视#14 同口径);gh 读照跑(真实只读预演)。"""
    if dry_run:
        events = []
        st = {"entries": [], "auth_failure_streak": 0}
        try:
            cycle_once(st, True, events, datetime.now())
        except GhError as exc:
            print(f"[dry-run] 单轮预演因 gh 失败中断(如实记录): {exc}",
                  file=sys.stderr)
            return 1
        write_report(st, events, True)
        print("[dry-run] pipeline 全流程预演完成,零副作用")
        return 0
    state_dir = tl._resolve_state_dir()
    os.makedirs(state_dir, exist_ok=True)
    lock_fd = os.open(os.path.join(state_dir, PIPELINE_LOCK_NAME),
                      os.O_CREAT | os.O_RDWR, 0o644)
    try:
        try:
            fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            print("[lock] pipeline 已在跑(锁占用),本实例退出")
            return 0
        tl._migrate_legacy_state_dir()
        try:
            tl.ensure_agent_labels()
        except GhError as exc:
            print(f"[warn] 标签确保失败: {exc}")
        store = StateStore()
        state = store.load()
        reset_review_tries(state)
        startup_reconcile(state)
        _pipeline_save(store, state)
        events = []
        while in_window(datetime.now(), window_start, window_end):
            try:
                cycle_once(state, False, events, datetime.now(),
                           window_end=window_end)
            except GhError as exc:
                print(f"[cycle] 本轮失败(下轮重试): {exc}", file=sys.stderr)
            except Exception as exc:
                import traceback
                print(f"[cycle] 本轮意外异常(消化续跑): "
                      f"{type(exc).__name__}: {exc}", file=sys.stderr)
                traceback.print_exc()
            _pipeline_save(store, state)
            time.sleep(PIPELINE_CYCLE_SEC)
        soft_stop(state)
        # 先写报告(记 last_report)再落盘——brief 原序 last_report 只进内存
        # 永不落盘,status 摘要永远读不到,此为修正。
        report = write_report(state, events, False)
        _pipeline_save(store, state)
        print(f"[pipeline] 窗口结束,报告: {report}")
        return 0
    finally:
        os.close(lock_fd)


def run_pipeline_single_cycle() -> int:
    """scan 薄壳(裁定#7): 单轮非 dry pipeline 循环——会话内手动推进用,
    cron 不再调 scan。与 run_pipeline 共 pipeline.lock(夜间流水线在跑时
    锁占用 → 打印+退出 0)。前奏取 run_pipeline 的幂等子集(迁移/标签/
    启动对账),但**不做** reset_review_tries: 检视「每晚 (pr,head) ≤2」
    的夜边界是流水线启动对账(spec §4 泳道2 边界列),手动 scan 不得清零
    每晚预算。GhError 消化为退出码 1(已推进的 state 仍落盘,旧 scan 先例
    语义);单轮不写夜报(夜报是整夜账,归 run_pipeline),事件末尾打印
    给会话操作者。"""
    state_dir = tl._resolve_state_dir()
    os.makedirs(state_dir, exist_ok=True)
    lock_fd = os.open(os.path.join(state_dir, PIPELINE_LOCK_NAME),
                      os.O_CREAT | os.O_RDWR, 0o644)
    try:
        try:
            fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            print("[lock] pipeline 已在跑(锁占用),本实例退出")
            return 0
        tl._migrate_legacy_state_dir()
        try:
            tl.ensure_agent_labels()
        except GhError as exc:
            print(f"[warn] 标签确保失败: {exc}")
        store = StateStore()
        state = store.load()
        startup_reconcile(state)
        _pipeline_save(store, state)
        events = []
        exit_code = 0
        try:
            cycle_once(state, False, events, datetime.now())
        except GhError as exc:
            print(f"scan(pipeline 单轮)失败(GhError,已推进的 state 仍落盘):"
                  f" {exc}", file=sys.stderr)
            exit_code = 1
        _pipeline_save(store, state)
        for ev in events:
            print(f"[event] {ev}")
        return exit_code
    finally:
        os.close(lock_fd)
