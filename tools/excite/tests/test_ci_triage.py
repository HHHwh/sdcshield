"""ci_triage 状态存储（Task 1）与红扫描/同根因收敛（Task 2）测试。

用例口径（Task 1 brief 2 例）:
  1 test_state_store_roundtrip: save 后 load 等值——完整 state(含 Entry 列表
    + auth_failure_streak)经 JSON 往返后逐字段等值,entries(state) 还原为
    类型化 Entry 对象且全部 16 字段等值;
  2 test_state_store_atomic_no_partial: 写入中断(替换 os.replace 抛异常)
    时,旧 state.json 原样可读、不残留 tmp 文件被 load 读到——load 只读
    state.json,save 是「写 tmp → os.replace」原子替换。

用例口径（Task 2 brief 4 例,全部 fake gh: monkeypatch 模块级 gh_json/issue 助手）:
  3 test_scan_filters_by_workflow_event_branch: 混合 run 列表中只留
    Sanitizer/schedule/failure/main——工作流×事件×分支×结论四维过滤;
  4 test_first_red_run_creates_issue: 无既有 issue → 调一次 issue 创建
    (fake 记录调用参数: 标题/正文含标记、UNTRUSTED_BANNER、失败日志摘录),
    state 新增 DETECTED→ISSUED 条目;
  5 test_second_red_run_merges_into_active_entry: 同 workflow 活跃条目
    (status 非 DONE/ESCALATED)存在 → 新 run 只挂 extra_runs、追加评论 1 次,
    issue 创建调用数为 0(同根因收敛);
  6 test_find_issue_by_run_uses_marker: 标记匹配 body → 返回 issue 号。

用例口径（Task 2 检视修复,2 例覆盖 Important#1/#2 + Minor#3）:
  7 test_closed_marker_issue_not_recreated: CLOSED issue 携带标记(open 搜索
    无命中)→ 人工接管信号——不建 issue、不评论、不写入 state,整体跳过;
  8 test_empty_workflows_env_falls_back_to_default: CITRIAGE_WORKFLOWS=""
    (及全空白项)→ 回落默认 4 个名单,显式覆盖仍生效(空名单失明修复)。

用例口径（Task 3,5 例: agent 派发与生命周期）:
  9 test_dispatch_sources_agent_env: monkeypatch run_background,捕获命令串
    含 source agent.env、--dangerously-skip-permissions、timeout 3600,
    cwd=CITRIAGE_CLONE_DIR(monkeypatch.setenv 重定向 tmp 克隆目录);
 10 test_prompt_contains_redlines: prompt 含 5 条红线 + issue 号 + fix 分支名;
 11 test_auth_failure_circuit_breaker_counts_across_entries: 3 条目各 auth_fail
    → 熔断(PAUSED 文件写入 tmp STATE_DIR、≥1 条升级评论、streak==3);
 12 test_single_flight_blocks_second_dispatch: 活 agent(os.getpid() 顶替 PID,
    真活)占位 → dispatch_agent 返回 None,run_background 不被调用;
 13 test_check_agent_outcome_classes: 五份伪造条目(活 PID+新时间/成功尾行/
    认证失效日志/死 PID/超时起点)→ 分类 running/success/auth_fail/dead/timeout。

用例口径（Task 3 检视修复,4 例;检视主证: 旧实现 run_background 返回
  wrapper shell 的 PID,整条生命周期判档跟着错——cron 每轮新进程会把健康
  agent 误判 dead → 重复派发 → 单飞失效 → 误升级 @人工）:
 14 test_run_background_returns_job_pid_not_wrapper_pid: 真实 spawn 合成
    命令 `nohup bash -c 'echo $$ > pidfile; sleep 30' >> log 2>&1 & echo $!`
    (零 claude/零触网)——返回值必须是后台任务自身 PID(任务自报 $$ 与
    返回值相等,wrapper shell 与任务是两个进程不可能同 PID);
 15 test_auth_fail_kills_live_agent_before_clearing_pid: auth_fail 清
    agent_pid 前必须击杀仍存活的 agent(真实 sleep 30 子进程)——未达阈值
    与达阈值(经 _escalate)两路都: 引用清空且进程死亡,单飞不漏无主活进程;
 16 test_lying_done_increments_attempts_before_requeue: 日志称 done 但
    查无 open PR(谎报)→ 与 dead/timeout 同口径 attempts+1 后重派;
 17 test_agent_reported_escalation_routes_to_escalate: 日志含结束语契约
    `ci-triage: escalated <reason>` → 新判档 escalated_reported,直接升级
    (理由尾巴进 @ 评论、不重派、不计 attempts)。

用例口径（Task 4,6 例: 车道 B bot 循环 + 单树自愈 + scan 编排。全部离线:
  fake gh(FakeGh 补 issue close 分支)/fake run_background/fake pr_loop 函数
  (monkeypatch ct.pr_loop.*)/fake _git——绝不触网、绝不真实派 claude):
 18 test_lane_b_dispatches_once_per_head（A6,fix round 1 修订）: PR_OPEN
    条目 + fake fetch_pr/classify_state/render_collect + fake
    run_background,READY_TO_FIX → 派 1 次 + rounds+1 + last_dispatched_head
    记 head + status→BOT_FIX;agent 在跑(活 PID)第二轮 → 跳过不重派;
    agent 退场 + 同 head → 清 head 锚后允许重派(Critical#1 裁定——原
    「退场后同 head 不重派」即死锁本身,该断言迁移至用例 25);head 变化
    → 再派(共 3 次);
 19 test_lane_b_merge_ready_notifies_and_closes: READY_TO_MERGE → issue
    评论含 @wangxumarshall、FakeGh 记录 issue close 调用(gh issue close
    <n> 分支)、条目 DONE;评论措辞「本跟踪 issue 现已关闭」与当场 close
    动作一致(Minor#7,不再虚报「将自动关闭」);
 20 test_scan_heals_dirty_tree_when_no_agent（Review Focus #4）:
    monkeypatch ct._git → 克隆 HEAD 非 main 且 dirty → 断言完整恢复序列
    (fetch --all --prune → checkout main → reset --hard origin/main →
    clean -fd;无 agent 时才自愈,活 agent 在跑时零 git 恢复调用);
 21 test_paused_blocks_dispatch_not_observation（A5）: STATE_DIR/PAUSED
    存在 → dispatch_agent 返回 None 且 run_background 零调用;观测路径
    (triage 的 gh run list/issue 搜索)照常执行;
 22 test_state_loss_recovers_from_github_markers（Review Focus #3）:
    空 state + open_search 携标记 issue → 复用不重建(issue 创建数 0、
    条目恢复 issue_number==77);
 23 test_scan_lock_excludes_concurrent（A4）: 测试内先 flock 占住
    scan.lock → cmd_scan(fake gh/git)立即返回且零 gh 调用;
 24 test_bot_fix_auth_fail_counts_toward_circuit_breaker(裁定 7 补强):
    BOT_FIX 条目 auth_fail 同样计入 streak——未达阈值击杀清引用归还
    PR_OPEN(清 head 锚允许同 head 重派,否则熔断永不触发);达阈值写
    PAUSED + ESCALATED(跨车道统一熔断,只豁免车道 A 的 success 查 PR /
    dead 重派推进)。

用例口径（Task 4 fix round 1,4 例: 检视修复 Critical#1/Important#2/
  Important#3/Important#4。全部离线,口径同 Task 4 主批）:
 25 test_bot_fix_dead_agent_returns_pr_open_and_re_dispatches
    (Critical#1): bot agent dead/timeout 退场(非 auth_fail)→ lane_b
    归还 PR_OPEN 且清 head 锚 → 同 head 允许重派(rounds+1);rounds
    达 4(MAX_ROUNDS)后再跑 → 不再派发而是 ESCALATED——钉死「死/超时
    退场不再永久停滞、重派由轮数上限兜底」;
 26 test_cmd_scan_toplevel_gherror_returns_1_and_saves_state
    (Important#2): lane_b 阶段 fake gh 抛 GhError → cmd_scan 返回 1、
    stderr 有错误信息、GhError 前已推进的 state(reap 记账)落盘——顶层
    捕获不裸 traceback,推进成果不因崩溃丢失;
 27 test_lane_b_rounds_cap_is_inclusive(Important#3): rounds=4 ==
    MAX_ROUNDS → lane_b 不派发、ESCALATED(rounds 上限 ≤4 轮,旧
    `rounds > MAX_ROUNDS` 是 off-by-one 派第 5 轮);
 28 test_bot_fix_success_resets_auth_streak(Important#4): streak=2 的
    BOT_FIX 条目 outcome=success → reap 只记录结局,但 streak 同样归零
    (成功即证明凭据活着;不清零会把「连续 ≥3」退化成「累计 ≥3」,跨周期
    间歇 auth_fail 过早全局 PAUSED)。
"""
import argparse
import os, shlex, signal, subprocess, sys, time
from datetime import datetime, timedelta, timezone

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "..", "scripts"))
import ci_triage as ct

# 测试统一锚定的目标仓库 slug(经 CITRIAGE_REPO 固定,杜绝依赖环境 git remote)
REPO = "test/repo"


def _sample_entry(**overrides):
    """全字段非默认值的样例 Entry(None 字段除外),用于 round-trip 等值断言。"""
    base = dict(
        workflow="Multi-OS Verify",
        run_id=123456,
        status="in_progress",
        issue_number=None,
        fix_branch="ci-triage/fix-run-123456",
        pr_number=None,
        attempts=1,
        rounds=0,
        agent_pid=98765,
        agent_started_at="2026-10-13T08:00:00Z",
        agent_log="/tmp/ci-triage/run-123456.log",
        last_dispatched_head="272c268b1234567890abcdef",
        extra_runs=[1234567, 1234568],
        created_at="2026-10-13T07:59:00Z",
        updated_at="2026-10-13T08:01:00Z",
    )
    base.update(overrides)
    return ct.Entry(**base)


# ---------------------------------------------------------------------------
# 用例 1: save → load 全量等值

def test_state_store_roundtrip(tmp_path, monkeypatch):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    store = ct.StateStore()

    entries = [_sample_entry(), _sample_entry(workflow="Sanitizer", run_id=222,
                                              status="fixed", pr_number=31,
                                              extra_runs=[])]
    state = {"entries": [e.to_dict() for e in entries],
             "auth_failure_streak": 2}
    store.save(state)

    loaded = store.load()
    assert loaded == state                       # dict 层等值
    assert loaded["auth_failure_streak"] == 2

    restored = store.entries(loaded)             # 类型化还原
    assert restored == entries                   # Entry 逐字段等值(16 字段)
    assert restored[0].workflow == "Multi-OS Verify"
    assert restored[1].extra_runs == []
    # 空状态语义: 无 state.json → 初始空 state
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path / "fresh"))
    fresh = ct.StateStore().load()
    assert fresh == {"entries": [], "auth_failure_streak": 0}


# ---------------------------------------------------------------------------
# 用例 2: 原子写——替换中断不留 tmp、不读到半份

def test_state_store_atomic_no_partial(tmp_path, monkeypatch):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    store = ct.StateStore()

    old = {"entries": [], "auth_failure_streak": 0}
    store.save(old)

    # 让原子替换中途失败(tmp 已写出,replace 抛异常 → save 须清理/不落半份)
    real_replace = os.replace
    def boom(src, dst):
        raise OSError("simulated crash between tmp write and replace")
    monkeypatch.setattr(os, "replace", boom)

    new_state = {"entries": [_sample_entry().to_dict()],
                 "auth_failure_streak": 5}
    with pytest.raises(OSError):
        store.save(new_state)
    monkeypatch.setattr(os, "replace", real_replace)

    # load 读到的仍是完整的旧 state,且目录里无 tmp 残留被后续读到
    assert store.load() == old
    leftovers = [n for n in os.listdir(tmp_path) if n.endswith(".tmp")]
    assert leftovers == []


# ===========================================================================
# Task 2: 红扫描 / 幂等 issue / 同根因收敛
# 测试口径: 全部 fake gh——monkeypatch 模块级 gh_json 与 issue 助手,绝不触网。
# ===========================================================================

def _run(database_id, workflow, event, conclusion, branch="main",
         head_sha="a" * 40, created_at=None):
    """gh run list --json 单条 run 的字典形态(createdAt 缺省=当下,lookback 内)。"""
    if created_at is None:
        created_at = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return {
        "databaseId": database_id,
        "workflowName": workflow,
        "event": event,
        "conclusion": conclusion,
        "headBranch": branch,
        "headSha": head_sha,
        "createdAt": created_at,
        "url": f"https://github.com/example/sdcshield/actions/runs/{database_id}",
    }


class FakeGh:
    """记录 gh 调用的 fake: run_list(读) / issue_create(写) / comment(写)。

    search_result 按 --state 分桶: open 搜索(open_search)与 closed 搜索
    (closed_search)各自返回独立结果——覆盖「open 无命中但 closed 携标记」
    的人工接管场景。
    """

    def __init__(self, runs=None, search_result=None, closed_search=None,
                 log_failed="job x failed", prs=None):
        self.runs = runs or []
        self.open_search = search_result or []   # gh search issues --state open
        self.closed_search = closed_search or [] # gh search issues --state closed
        self.log_failed = log_failed
        self.prs = prs or []                     # gh pr list(success 结局核实)
        self.calls = []                            # 全部 gh_json 调用参数
        self.created_issues = []                   # (title, body)
        self.comments = []                         # (issue_number, body)
        self.closed_issues = []                    # Task 4: issue_number 列表

    def gh_json(self, args, input_text=None):
        """模块级 gh_json 的替身: 按 gh 子命令分派。

        input_text 对应实现的 stdin 传参(gh issue create --body-file -)。
        """
        self.calls.append(list(args))
        if args[:2] == ["run", "list"]:
            return self.runs
        if args[:2] == ["pr", "list"]:
            return self.prs
        if args[:2] == ["search", "issues"]:
            # gh search issues --repo <slug> --state <open|closed> …
            state = args[args.index("--state") + 1]
            return self.open_search if state == "open" else self.closed_search
        if args[:3] == ["issue", "create", "--repo"]:
            # gh issue create --body-file -: 正文经 stdin(input_text)传入
            self.created_issues.append((args[args.index("--title") + 1],
                                        input_text))
            return "https://github.com/example/sdcshield/issues/77"
        if args[:2] == ["issue", "comment"] and args[2].isdigit():
            # gh issue comment <n> --repo <slug> --body <text>
            self.comments.append((int(args[2]), args[args.index("--body") + 1]))
            return "https://github.com/example/sdcshield/issues/77#comment"
        if args[:2] == ["issue", "close"] and args[2].isdigit():
            # gh issue close <n> --repo <slug>(Task 4: 车道 B 条目收尾)
            self.closed_issues.append(int(args[2]))
            return None
        if args[:2] == ["run", "view"] and "--log-failed" in args:
            # gh run view <id> --repo <slug> --log-failed(纯文本输出)
            return self.log_failed
        raise AssertionError(f"未预期的 gh 调用: {args}")


# ---------------------------------------------------------------------------
# 用例 3: 扫描过滤——工作流 × 事件 × 分支 × 结论

def test_scan_filters_by_workflow_event_branch(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    # 混合列表: 只有 run#2 (Sanitizer/schedule/failure/main) 应被保留
    runs = [
        _run(101, "Multi-OS Verify", "schedule", "success"),      # 绿: 排除
        _run(102, "Sanitizer", "schedule", "failure"),            # ✓ 命中
        _run(103, "Sanitizer", "schedule", "failure", branch="feat/x"),  # 非法分支
        _run(104, "Sanitizer", "pull_request", "failure"),        # 非法事件
        _run(105, "Sanitizer", "schedule", "cancelled"),          # 非失败结论
        _run(106, "Unit Tests", "schedule", "failure"),           # 非监控工作流
        _run(107, "Multi-OS Verify", "push", "failure"),          # ✓ 命中(push 也算)
        _run(108, "CodeQL", "schedule", "failure"),               # ✓ 命中
    ]
    fake = FakeGh(runs=runs)
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)

    red = ct.scan_red_runs()

    ids = [r["databaseId"] for r in red]
    assert ids == [102, 107, 108]
    # gh run list 调用必须锚定 main 分支与测试 slug(不依赖环境 git remote)
    assert any(a[:2] == ["run", "list"] and REPO in a
               and "--branch" in a and a[a.index("--branch") + 1] == "main"
               for a in fake.calls)


# ---------------------------------------------------------------------------
# 用例 4: 首个红 run → 建一个 issue(标记/横幅/失败日志摘录),条目 ISSUED

def test_first_red_run_creates_issue(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    fake = FakeGh(runs=[], search_result=[], log_failed="E: assert 1 == 2")
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)

    run = _run(38482214201, "Sanitizer", "schedule", "failure")
    state = {"entries": [], "auth_failure_streak": 0}
    new = ct.triage_new_red_runs(state, [run])

    assert len(new) == 1
    e = new[0]
    assert e.status == "ISSUED"
    assert e.workflow == "Sanitizer"
    assert e.run_id == 38482214201
    assert e.issue_number == 77            # fake 返回的 issue 号
    assert e.extra_runs == []
    # 恰好一次 issue 创建;参数含标题(run id+日期)、正文(标记/横幅/日志)
    assert len(fake.created_issues) == 1
    title, body = fake.created_issues[0]
    assert "38482214201" in title and "Sanitizer" in title
    assert "<!-- ci-triage:run:38482214201 -->" in body
    assert ct.UNTRUSTED_BANNER in body
    assert "E: assert 1 == 2" in body      # 失败日志摘录进了正文
    assert fake.comments == []             # 新建不评论

    # state 已写入新条目(供 StateStore.save 的调用方/或函数内已保存)
    assert any(d["run_id"] == 38482214201 for d in state["entries"])


# ---------------------------------------------------------------------------
# 用例 5: 同 workflow 活跃条目存在 → 新红 run 收敛(挂 extra_runs + 1 条评论)

def test_second_red_run_merges_into_active_entry(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    fake = FakeGh(runs=[], search_result=[], log_failed="E: boom")
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)

    active = _sample_entry(workflow="Sanitizer", run_id=38482214201,
                           status="in_progress", issue_number=77, extra_runs=[])
    state = {"entries": [active.to_dict()], "auth_failure_streak": 0}

    run2 = _run(38482214399, "Sanitizer", "schedule", "failure")
    new = ct.triage_new_red_runs(state, [run2])

    # 不建新 issue、不派发——只挂 extra_runs + 追加一条评论(含 run 链接)
    assert len(fake.created_issues) == 0
    assert len(fake.comments) == 1
    assert fake.comments[0][0] == 77
    assert "38482214399" in fake.comments[0][1]
    # 活跃条目被更新: extra_runs 收下新 run,状态不变
    e = [x for x in ct.StateStore.entries(state)
         if x.workflow == "Sanitizer"][0]
    assert 38482214399 in e.extra_runs
    assert e.status == "in_progress"
    assert new == []                      # 无新条目产生
    # 终态条目(DONE/ESCALATED)不算活跃: 不同 workflow 各自独立开 issue
    done = _sample_entry(workflow="CodeQL", run_id=999, status="DONE")
    state2 = {"entries": [done.to_dict()], "auth_failure_streak": 0}
    run3 = _run(888, "CodeQL", "schedule", "failure")
    new2 = ct.triage_new_red_runs(state2, [run3])
    assert len(new2) == 1 and new2[0].status == "ISSUED"
    assert len(fake.created_issues) == 1


# ---------------------------------------------------------------------------
# 用例 6: 按 run 标记找既有 issue(open only)

def test_find_issue_by_run_uses_marker(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    search_result = [
        {"number": 12, "title": "unrelated", "body": "no marker here"},
        {"number": 77, "title": "[ci-triage] Sanitizer 红",
         "body": "run 链接\n<!-- ci-triage:run:38482214201 -->\n更多"},
    ]
    fake = FakeGh(runs=[], search_result=search_result)
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)

    assert ct.find_issue_by_run(38482214201) == 77
    # 无匹配 → None(调用方据此决定创建)
    fake.open_search = []
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)
    assert ct.find_issue_by_run(12345) is None
    # 调用形态: gh search issues --state open(限定测试 slug),文本 "ci-triage:run"
    assert any(a[0] == "search" and a[1] == "issues" and "--state" in a
               and a[a.index("--state") + 1] == "open" and REPO in a
               for a in fake.calls)


# ---------------------------------------------------------------------------
# 用例 7: CLOSED issue 携带标记 → 人工接管信号,不重建、不评论、不入 state

def test_closed_marker_issue_not_recreated(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    # open 搜索无命中;closed 搜索返回携标记的已关闭 issue #55
    fake = FakeGh(runs=[], search_result=[], closed_search=[
        {"number": 55, "title": "[ci-triage] Sanitizer 红(人工关闭)",
         "body": "<!-- ci-triage:run:38482214201 -->\n人工接管处理"},
    ])
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)

    run = _run(38482214201, "Sanitizer", "schedule", "failure")
    state = {"entries": [], "auth_failure_streak": 0}
    new = ct.triage_new_red_runs(state, [run])

    # 不建 issue、不评论、不写入 state,整体跳过该 run
    assert new == []
    assert fake.created_issues == []
    assert fake.comments == []
    assert state["entries"] == []
    # 调用顺序: 先搜 open(无命中)再搜 closed(命中 #55)——closed 搜索确实发生
    search_states = [a[a.index("--state") + 1]
                     for a in fake.calls if a[:2] == ["search", "issues"]]
    assert search_states == ["open", "closed"]
    assert ct.find_closed_marker_issue(38482214201) == 55
    # open 搜索本身有命中时,走复用路径、不触发 closed 搜索
    fake2 = FakeGh(runs=[], search_result=[
        {"number": 66, "title": "[ci-triage] open",
         "body": "<!-- ci-triage:run:38482214201 -->"},
    ], closed_search=[
        {"number": 55, "title": "also closed",
         "body": "<!-- ci-triage:run:38482214201 -->"},
    ])
    monkeypatch.setattr(ct, "gh_json", fake2.gh_json)
    state2 = {"entries": [], "auth_failure_streak": 0}
    new2 = ct.triage_new_red_runs(state2, [run])
    assert len(new2) == 1 and new2[0].issue_number == 66
    assert fake2.created_issues == []
    assert fake2.comments == []
    search_states2 = [a[a.index("--state") + 1]
                      for a in fake2.calls if a[:2] == ["search", "issues"]]
    assert search_states2 == ["open"]    # open 命中即止,不查 closed


# ---------------------------------------------------------------------------
# 用例 8: CITRIAGE_WORKFLOWS 空串 → 回落默认名单(空名单失明修复)

def test_empty_workflows_env_falls_back_to_default(monkeypatch):
    # env 存在但为空串 → 默认 4 个(不是空名单)
    monkeypatch.setenv("CITRIAGE_WORKFLOWS", "")
    assert ct.monitored_workflows() == ["Multi-OS Verify", "Sanitizer",
                                        "CodeQL", "Security"]
    # 全空白项同理 → 默认 4 个
    monkeypatch.setenv("CITRIAGE_WORKFLOWS", " , , ")
    assert ct.monitored_workflows() == ["Multi-OS Verify", "Sanitizer",
                                        "CodeQL", "Security"]
    # 未设置 → 默认 4 个
    monkeypatch.delenv("CITRIAGE_WORKFLOWS", raising=False)
    assert ct.monitored_workflows() == ["Multi-OS Verify", "Sanitizer",
                                        "CodeQL", "Security"]
    # 显式覆盖仍然生效(不因修复破坏覆盖语义)
    monkeypatch.setenv("CITRIAGE_WORKFLOWS", "Multi-OS Verify, My Extra WF")
    assert ct.monitored_workflows() == ["Multi-OS Verify", "My Extra WF"]


# ===========================================================================
# Task 3: agent 派发与生命周期
# 测试口径: dispatch 经 monkeypatch run_background(绝不真实派 claude);
#           check_agent_outcome 走真实 os.kill/文件读(死/活 PID 皆真实可控);
#           reap_or_advance 经 fake gh(gh pr list / 评论)与 monkeypatch
#           check_agent_outcome——绝不触网、绝不真实派发。
#           例外(用例 14): run_background 本体的 PID 契约用真实 spawn 验证
#           ——合成 sleep 命令,零 claude/零触网,finally 里 SIGKILL 清场。
# ===========================================================================

def _fake_run_detail(run_id=123456):
    """build_agent_prompt 的 run_detail 参数样例(gh run view --json 形态)。"""
    return {
        "databaseId": run_id,
        "workflowName": "Sanitizer",
        "url": f"https://github.com/example/sdcshield/actions/runs/{run_id}",
        "headSha": "a" * 40,
        "createdAt": "2026-10-13T08:00:00Z",
    }


class FakeBg:
    """run_background 的替身: 记录 (cmd, cwd, log),不真实 spawn。"""

    def __init__(self):
        self.calls = []   # (cmd, cwd, log)

    def __call__(self, cmd, cwd, log):
        self.calls.append((cmd, cwd, log))
        return 424242    # 伪 PID


# ---------------------------------------------------------------------------
# 用例 9: dispatch 命令形态——source agent.env + 危险许可旁路 + timeout,
#          且在克隆目录(CITRIAGE_CLONE_DIR)执行,entry 记 PID/日志/起点

def test_dispatch_sources_agent_env(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    clone_dir = tmp_path / "clone"
    clone_dir.mkdir()
    monkeypatch.setenv("CITRIAGE_CLONE_DIR", str(clone_dir))

    bg = FakeBg()
    monkeypatch.setattr(ct, "run_background", bg)

    entry = _sample_entry(status="ISSUED", agent_pid=None,
                          agent_started_at=None, agent_log=None, attempts=0)
    state = {"entries": [entry.to_dict()], "auth_failure_streak": 0}

    pid = ct.dispatch_agent(entry, state)

    assert pid == 424242
    assert len(bg.calls) == 1
    cmd, cwd, log = bg.calls[0]
    assert cwd == str(clone_dir)             # 在克隆目录执行(绝不在用户工作区)
    assert "source" in cmd
    assert "agent.env" in cmd                # 认证环境经 STATE_DIR/agent.env 注入
    assert "--dangerously-skip-permissions" in cmd
    assert "timeout 3600" in cmd             # agent 最长运行 1 小时
    assert "claude -p" in cmd                # prompt 经 -p 传入
    # entry 已记录派发现场: PID/日志路径/起点 + attempts+1
    assert entry.agent_pid == 424242
    assert entry.agent_log is not None and "agent.env" not in entry.agent_log
    assert entry.agent_started_at is not None
    assert entry.attempts == 1


# ---------------------------------------------------------------------------
# 用例 10: prompt 红线与任务契约——5 红线 + issue 号 + fix 分支名

def test_prompt_contains_redlines():
    entry = _sample_entry(issue_number=77, run_id=123456,
                          fix_branch="fix/ci-123456")
    prompt = ct.build_agent_prompt(entry, _fake_run_detail())

    # 5 条红线逐字命中
    for redline in ("绝不推 main", "绝不 merge", "绝不 resolve",
                    "绝不碰用户工作区", "未复现"):
        assert redline in prompt, f"prompt 缺红线: {redline}"
    # 任务契约锚点: issue 号 + fix 分支名 + 结束语约定
    assert "77" in prompt
    assert "fix/ci-123456" in prompt
    assert "ci-triage: done" in prompt
    assert "ci-triage: escalated" in prompt
    assert ct.UNTRUSTED_BANNER in prompt      # 不可信数据横幅(pr_loop 同源)


# ---------------------------------------------------------------------------
# 用例 11: 认证熔断跨条目累计——3 次 auth_fail → PAUSED + 升级评论 + streak==3

def test_auth_failure_circuit_breaker_counts_across_entries(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    fake = FakeGh()
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)

    # 伪 PID 用必死值(>pid_max): reap 清引用前会探活击杀,低段伪 PID 有
    # 撞上真进程被 SIGKILL 的风险(检视修复 2 的联动约束)
    entries = [_sample_entry(workflow="Sanitizer", run_id=i, status="DIAGNOSING",
                             issue_number=100 + i, agent_pid=999999901 + i,
                             agent_log=str(tmp_path / f"run-{i}.log"))
               for i in range(3)]
    state = {"entries": [e.to_dict() for e in entries], "auth_failure_streak": 0}
    # 三个条目全部判为 auth_fail(日志尾部含认证失效特征)
    monkeypatch.setattr(ct, "check_agent_outcome", lambda e: "auth_fail")

    ct.reap_or_advance(state)

    # 熔断: PAUSED 文件已写入 state 目录(停止继续派发)
    assert (tmp_path / "PAUSED").exists()
    # 跨条目累计计数 = 3
    assert state["auth_failure_streak"] == 3
    # 至少一条升级评论提及认证失效与恢复动作
    assert len(fake.comments) >= 1
    assert any("认证" in body for _, body in fake.comments)
    # 触达熔断阈值(第 3 次)的条目升级;前两次未达阈值,保持待重派
    statuses = [e.status for e in ct.StateStore.entries(state)]
    assert statuses == ["DIAGNOSING", "DIAGNOSING", "ESCALATED"]


# ---------------------------------------------------------------------------
# 用例 12: 单飞——活 agent 占位时拒绝第二次派发

def test_single_flight_blocks_second_dispatch(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    bg = FakeBg()
    monkeypatch.setattr(ct, "run_background", bg)

    # 条目 A 持有真实活 PID(本测试进程),状态 DIAGNOSING → has_running_agent
    a = _sample_entry(workflow="Sanitizer", run_id=1, status="DIAGNOSING",
                      agent_pid=os.getpid(), extra_runs=[])
    b = _sample_entry(workflow="CodeQL", run_id=2, status="ISSUED",
                      agent_pid=None, attempts=0, extra_runs=[])
    state = {"entries": [a.to_dict(), b.to_dict()], "auth_failure_streak": 0}

    assert ct.has_running_agent(state) is True
    pid = ct.dispatch_agent(b, state)

    # 拒绝派发: 返回 None,零 spawn,b 不变
    assert pid is None
    assert bg.calls == []
    assert b.agent_pid is None and b.attempts == 0


# ---------------------------------------------------------------------------
# 用例 13: 结局五分类——timeout → auth_fail → success → dead → running

def test_check_agent_outcome_classes(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    now = datetime.now(timezone.utc)

    def _iso(dt):
        return dt.strftime("%Y-%m-%dT%H:%M:%SZ")

    def _log(name, text):
        p = tmp_path / name
        p.write_text(text, encoding="utf-8")
        return str(p)

    # running: 活 PID(本测试进程)+ 刚启动 + 空日志
    running = _sample_entry(
        status="DIAGNOSING", agent_pid=os.getpid(),
        agent_started_at=_iso(now), agent_log=_log("a.log", "…工作中…"))
    # success: 日志含结束语(优先于 dead——agent 输出 done 后可能短暂存活)
    success = _sample_entry(
        status="DIAGNOSING", agent_pid=os.getpid(),
        agent_started_at=_iso(now), agent_log=_log("b.log", "修复完成\nci-triage: done fix/ci-9\n"))
    # auth_fail: 日志尾部含认证失效特征(优先于 success 判定)
    auth_fail = _sample_entry(
        status="DIAGNOSING", agent_pid=os.getpid(),
        agent_started_at=_iso(now),
        agent_log=_log("c.log", "API Error: 403 Not logged in\n"))
    # dead: 死 PID + 无特征日志
    dead = _sample_entry(
        status="DIAGNOSING", agent_pid=999999999,
        agent_started_at=_iso(now), agent_log=_log("d.log", "…中途暴毙…"))
    # timeout: 起点早于 AGENT_TIMEOUT_SEC(即使 PID 仍活也判超时)
    timeout = _sample_entry(
        status="DIAGNOSING", agent_pid=os.getpid(),
        agent_started_at=_iso(now - timedelta(
            seconds=ct.AGENT_TIMEOUT_SEC + 60)),
        agent_log=_log("e.log", "…卡死…"))

    assert ct.check_agent_outcome(running) == "running"
    assert ct.check_agent_outcome(success) == "success"
    assert ct.check_agent_outcome(auth_fail) == "auth_fail"
    assert ct.check_agent_outcome(dead) == "dead"
    assert ct.check_agent_outcome(timeout) == "timeout"


# ===========================================================================
# Task 3 检视修复: run_background PID 契约 / auth_fail 击杀 / 谎报计数 /
#                 自报升级判档
# ===========================================================================

def _wait_dead(proc, timeout=5.0):
    """轮询直至子进程退出(SIGKILL 送达异步,裸 poll 瞬时可能仍 None)。"""
    for _ in range(int(timeout / 0.02)):
        if proc.poll() is not None:
            return proc.returncode
        time.sleep(0.02)
    return proc.poll()


# ---------------------------------------------------------------------------
# 用例 14: run_background 真实 spawn——返回任务自身 PID,不是 wrapper shell PID

def test_run_background_returns_job_pid_not_wrapper_pid(tmp_path):
    log = tmp_path / "bg.log"
    pidfile = tmp_path / "job.pid"
    # 与 dispatch_agent 同构的命令形态: 任务自身输出重定向进 log,wrapper 的
    # stdout 只承载 `echo $!` 一行;任务首步自报 $$ 落盘,作身份铁证。
    inner = f"echo $$ > {shlex.quote(str(pidfile))}; sleep 30"
    cmd = (f"nohup bash -c {shlex.quote(inner)} "
           f">> {shlex.quote(str(log))} 2>&1 & echo $!")
    pid = ct.run_background(cmd, cwd=str(tmp_path), log=str(log))
    try:
        # (a) 返回的 PID 探活为活
        assert ct.agent_alive(pid)
        # 等后台任务 exec 后的 bash 完成自报 PID 落盘(≤4s,消除 fork/exec 竞态)
        for _ in range(200):
            if pidfile.exists():
                break
            time.sleep(0.02)
        assert pidfile.exists(), "后台任务未在期限内自报 PID(未启动?)"
        # (b)+(c) 身份证明: 任务自身 $$ == 返回值。wrapper shell 与后台任务
        # 是 fork 出的两个进程,不可能同 PID——旧实现返回 wrapper PID 必败于此
        assert int(pidfile.read_text().strip()) == pid
        # 佐证: 该 PID 的 cmdline 是后台任务本身,不是 `sh -c <整条命令>` 的
        # wrapper(wrapper 的 argv 含 "echo $!" 字样)
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            argv = f.read().decode("utf-8", errors="replace")
        assert "sleep 30" in argv.replace("\0", " ")
        assert "echo $!" not in argv
    finally:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


# ---------------------------------------------------------------------------
# 用例 15: auth_fail 清 agent_pid 前必须击杀仍存活的 agent(单飞不漏无主活进程)

def test_auth_fail_kills_live_agent_before_clearing_pid(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    fake = FakeGh()
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)
    monkeypatch.setattr(ct, "check_agent_outcome", lambda e: "auth_fail")

    # 场景 1: 未达熔断阈值——被误判/竞态下仍存活的 agent,清引用前先击杀
    p1 = subprocess.Popen(["sleep", "30"], start_new_session=True)
    a = _sample_entry(workflow="Sanitizer", run_id=11, status="DIAGNOSING",
                      issue_number=101, agent_pid=p1.pid, attempts=1,
                      agent_log=str(tmp_path / "a.log"), extra_runs=[])
    state = {"entries": [a.to_dict()], "auth_failure_streak": 0}
    ct.reap_or_advance(state)
    ea = ct.StateStore.entries(state)[0]
    assert state["auth_failure_streak"] == 1     # 该结局只计一次
    assert ea.agent_pid is None                  # 引用已清
    assert _wait_dead(p1) is not None            # 且进程已死(清引用前被击杀)
    assert ea.status == "DIAGNOSING"             # 未达阈值不升级

    # 场景 2: 达阈值走 _escalate 路径——同样先击杀再清引用
    p2 = subprocess.Popen(["sleep", "30"], start_new_session=True)
    b = _sample_entry(workflow="CodeQL", run_id=12, status="DIAGNOSING",
                      issue_number=102, agent_pid=p2.pid, attempts=1,
                      agent_log=str(tmp_path / "b.log"), extra_runs=[])
    state2 = {"entries": [b.to_dict()], "auth_failure_streak": 2}
    ct.reap_or_advance(state2)
    eb = ct.StateStore.entries(state2)[0]
    assert (tmp_path / "PAUSED").exists()        # 熔断
    assert eb.status == "ESCALATED"
    assert eb.agent_pid is None
    assert _wait_dead(p2) is not None            # 升级路径也不留活 agent
    assert any("认证" in body for _, body in fake.comments)


# ---------------------------------------------------------------------------
# 用例 16: 谎报 done(日志有 done、查无 open PR)→ attempts+1 后走重派路径

def test_lying_done_increments_attempts_before_requeue(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    fake = FakeGh(prs=[])          # gh pr list 无命中——日志称 done 但无 PR
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)
    monkeypatch.setattr(ct, "check_agent_outcome", lambda e: "success")

    entry = _sample_entry(workflow="Sanitizer", run_id=13, status="DIAGNOSING",
                          issue_number=103, agent_pid=999999999, attempts=1,
                          agent_log=str(tmp_path / "c.log"), extra_runs=[])
    state = {"entries": [entry.to_dict()], "auth_failure_streak": 0}
    ct.reap_or_advance(state)

    e = ct.StateStore.entries(state)[0]
    # 与 dead/timeout 同口径计数(1→2): 缺 +1 时反复谎报的 agent 会比死亡
    # 路径多白嫖一次派发(3 次才升级 vs 2 次)
    assert e.attempts == 2
    assert e.status == "DIAGNOSING"              # ≤MAX_AGENT_ATTEMPTS 待重派
    assert e.agent_pid is None
    assert fake.comments == []                   # 未超限,不升级评论


# ---------------------------------------------------------------------------
# 用例 17: agent 自报升级(结束语契约 `ci-triage: escalated <reason>`)
#          → 新判档 escalated_reported,直接升级、不重派、不计 attempts

def test_agent_reported_escalation_routes_to_escalate(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    fake = FakeGh()
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)

    log = tmp_path / "agent.log"
    log.write_text("工作 3590s 仍未复现\n"
                   "ci-triage: escalated 无法在本机复现失败(仅日志级诊断)\n",
                   encoding="utf-8")
    # agent 输出结束语后可能仍短暂存活——诚实升级不能被当成还在跑/dead
    proc = subprocess.Popen(["sleep", "30"], start_new_session=True)
    now_iso = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    entry = _sample_entry(workflow="Sanitizer", run_id=14, status="DIAGNOSING",
                          issue_number=104, agent_pid=proc.pid, attempts=1,
                          agent_started_at=now_iso, agent_log=str(log),
                          extra_runs=[])
    state = {"entries": [entry.to_dict()], "auth_failure_streak": 0}

    # 判档: 日志含升级行、PID 仍活 → escalated_reported(非 running/dead)
    assert ct.check_agent_outcome(entry) == "escalated_reported"

    ct.reap_or_advance(state)
    e = ct.StateStore.entries(state)[0]
    assert e.status == "ESCALATED"               # 直接升级,不重派
    assert e.attempts == 1                       # 诚实上报不是失败重试,不计数
    assert e.agent_pid is None
    assert len(fake.comments) == 1
    body = fake.comments[0][1]
    assert "wangxumarshall" in body              # @人工
    assert "无法在本机复现失败" in body          # 升级理由尾巴进了评论
    assert _wait_dead(proc) is not None          # 活 PID 被击杀,不留无主进程


# ===========================================================================
# Task 4: 车道 B bot 循环 + 单树自愈 + scan 编排
# 测试口径: 全部离线——fake gh(FakeGh 含 issue close 分支)/fake
# run_background/fake pr_loop 函数(monkeypatch ct.pr_loop.*——lane_b 经
# ct 模块命名空间的 pr_loop 引用调用,零触网)/fake _git(heal_clone 的
# git 子进程唯一出口)。绝不真实派 claude、绝不真实 git 恢复。
# ===========================================================================

def _fake_pr(number=88, head_sha="b" * 40, state="open"):
    """pr_loop.PRData 的最小替身(lane_b 只用 state/draft/fork/head_sha/
    head_ref/url/number/title/base_ref 等少量属性,SimpleNamespace 顶替)。"""
    from types import SimpleNamespace
    return SimpleNamespace(
        number=number, title=f"fix #{number}", state=state, draft=False,
        fork=False, head_sha=head_sha, head_ref="fix/ci-1",
        base_ref="main", url=f"https://github.com/example/sdcshield/pull/{number}",
    )


def _fake_pr_loop(monkeypatch, pr, state="READY_TO_FIX"):
    """三件套替换: fetch_pr / classify_state / render_collect 全走 fake。

    classify_state 可由后续用例覆写;render_collect 返回固定导出文本。
    """
    monkeypatch.setattr(ct.pr_loop, "fetch_pr", lambda slug, num: pr)
    monkeypatch.setattr(ct.pr_loop, "classify_state",
                        lambda p, quiet, now: state)
    monkeypatch.setattr(ct.pr_loop, "render_collect",
                        lambda p, slug, quiet: "# collect 导出(fake)\n")


# ---------------------------------------------------------------------------
# 用例 18: 同 head 单派的边界(A6,fix round 1 修订)——READY_TO_FIX 派 bot
#          agent;agent 在跑(活 PID)不重派;agent 退场后同 head 的重派
#          裁定归用例 25(原「退场后同 head 不重派」断言即 Critical#1 的
#          死锁本身);head 变化才无条件允许新一轮

def test_lane_b_dispatches_once_per_head(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    monkeypatch.setattr(ct, "gh_json", FakeGh().gh_json)
    bg = FakeBg()
    monkeypatch.setattr(ct, "run_background", bg)

    entry = _sample_entry(workflow="Sanitizer", run_id=21, status="PR_OPEN",
                          issue_number=77, pr_number=88, rounds=1,
                          agent_pid=None, agent_started_at=None,
                          agent_log=None, last_dispatched_head=None,
                          extra_runs=[])
    state = {"entries": [entry.to_dict()], "auth_failure_streak": 0}

    # 第 1 轮: READY_TO_FIX → 派 bot 处置 agent(rounds+1、记 head、BOT_FIX)
    pr1 = _fake_pr(head_sha="b" * 40)
    _fake_pr_loop(monkeypatch, pr1, "READY_TO_FIX")
    ct.lane_b_advance(state, dry_run=False)

    assert len(bg.calls) == 1                       # 恰一次派发
    e = ct.StateStore.entries(state)[0]
    assert e.status == "BOT_FIX"
    assert e.rounds == 2                            # rounds+1,不动 attempts
    assert e.attempts == 1                          # 采样值未被 lane B 篡改
    assert e.last_dispatched_head == "b" * 40
    assert e.agent_pid == 424242
    # collect 导出已落盘(供 bot agent 读取)
    collect = tmp_path / "collect-pr88.md"
    assert collect.exists()

    # 第 2 轮: agent 仍在跑(真实活 PID 顶替,本测试进程)→ 跳过不重派
    e1 = ct.StateStore.entries(state)[0]
    e1.agent_pid = os.getpid()
    state["entries"] = [e1.to_dict()]
    ct.lane_b_advance(state, dry_run=False)
    assert len(bg.calls) == 1
    e2 = ct.StateStore.entries(state)[0]
    assert e2.status == "BOT_FIX"                   # 活 agent 在跑,条目不动
    assert e2.last_dispatched_head == "b" * 40

    # 第 3 轮: agent 退场(死伪 PID)且 head 变化(新 push)→ 归还 PR_OPEN 后
    # 允许新一轮派发(共 2 次;head 变化的重派不受同 head 单派规则约束)
    e2.agent_pid = 999999901
    state["entries"] = [e2.to_dict()]
    pr2 = _fake_pr(head_sha="c" * 40)
    _fake_pr_loop(monkeypatch, pr2, "READY_TO_FIX")
    ct.lane_b_advance(state, dry_run=False)
    assert len(bg.calls) == 2
    e3 = ct.StateStore.entries(state)[0]
    assert e3.last_dispatched_head == "c" * 40
    assert e3.rounds == 3


# ---------------------------------------------------------------------------
# 用例 19: READY_TO_MERGE → @wangxumarshall 请人工 merge + issue close + DONE

def test_lane_b_merge_ready_notifies_and_closes(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    fake = FakeGh()
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)
    bg = FakeBg()
    monkeypatch.setattr(ct, "run_background", bg)

    entry = _sample_entry(workflow="Sanitizer", run_id=22, status="PR_OPEN",
                          issue_number=77, pr_number=88, rounds=2,
                          agent_pid=None, agent_started_at=None,
                          agent_log=None, extra_runs=[])
    state = {"entries": [entry.to_dict()], "auth_failure_streak": 0}

    _fake_pr_loop(monkeypatch, _fake_pr(), "READY_TO_MERGE")
    ct.lane_b_advance(state, dry_run=False)

    # 评论 @人工请 merge;issue 关闭;条目 DONE;零派发
    assert len(fake.comments) == 1
    assert fake.comments[0][0] == 77
    assert "wangxumarshall" in fake.comments[0][1]
    assert "merge" in fake.comments[0][1].lower()
    # Minor#7: 措辞与动作一致——代码当场 gh issue close,评论不得虚报
    # 「merge 后将自动关闭」
    assert "现已关闭" in fake.comments[0][1]
    assert "自动关闭" not in fake.comments[0][1]
    assert fake.closed_issues == [77]
    e = ct.StateStore.entries(state)[0]
    assert e.status == "DONE"
    assert bg.calls == []


# ---------------------------------------------------------------------------
# 用例 20: 无 agent 时自愈脏克隆树(Review Focus #4)——完整恢复序列

def test_scan_heals_dirty_tree_when_no_agent(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    fake = FakeGh(runs=[])
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)

    git_calls = []

    def fake_git(args):
        git_calls.append(list(args))
        # rev-parse --abbrev-ref HEAD → 非 main;status --porcelain → 脏
        if args[:3] == ["rev-parse", "--abbrev-ref", "HEAD"]:
            return "fix/ci-999"
        if args[:2] == ["status", "--porcelain"]:
            return " M scripts/ci_triage.py"
        return ""

    monkeypatch.setattr(ct, "_git", fake_git)

    state = {"entries": [], "auth_failure_streak": 0}
    ct.heal_clone(state, dry_run=False)

    # 完整恢复序列: fetch --all --prune → 查 HEAD → 查 dirty →
    # checkout main → reset --hard origin/main → clean -fd
    assert git_calls[0][:3] == ["fetch", "--all", "--prune"]
    assert git_calls[1][:3] == ["rev-parse", "--abbrev-ref", "HEAD"]
    assert git_calls[2][:2] == ["status", "--porcelain"]
    assert git_calls[3][:2] == ["checkout", "main"]
    assert git_calls[4][:3] == ["reset", "--hard", "origin/main"]
    assert git_calls[5][:2] == ["clean", "-fd"]
    assert len(git_calls) == 6

    # 活 agent 在跑时零 git 恢复调用(绝不踩正在干活的 agent 工作区)
    git_calls.clear()
    busy = _sample_entry(workflow="Sanitizer", run_id=23, status="DIAGNOSING",
                         agent_pid=os.getpid(), extra_runs=[])
    state2 = {"entries": [busy.to_dict()], "auth_failure_streak": 0}
    ct.heal_clone(state2, dry_run=False)
    assert git_calls == []

    # dry-run: 零 git 调用,只打印意图
    git_calls.clear()
    state3 = {"entries": [], "auth_failure_streak": 0}
    ct.heal_clone(state3, dry_run=True)
    assert git_calls == []


# ---------------------------------------------------------------------------
# 用例 21: PAUSED 挡派发不挡观测(A5)——dispatch 拒绝,triage 观测照常

def test_paused_blocks_dispatch_not_observation(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    # open_search 空且 closed_search 空 → triage 走 create_issue 分支,
    # 观测(搜索)是否执行的证据在 fake.calls 里
    fake = FakeGh(runs=[], search_result=[])
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)
    bg = FakeBg()
    monkeypatch.setattr(ct, "run_background", bg)

    # PAUSED 文件存在 → dispatch 拒绝(车道 A/B 同受闸)
    (tmp_path / "PAUSED").write_text("auth failure streak\n", encoding="utf-8")
    entry = _sample_entry(workflow="Sanitizer", run_id=24, status="ISSUED",
                          issue_number=None, agent_pid=None,
                          agent_started_at=None, agent_log=None, attempts=0,
                          extra_runs=[])
    state = {"entries": [entry.to_dict()], "auth_failure_streak": 3}
    assert ct.dispatch_agent(entry, state) is None
    assert bg.calls == []                           # 零 spawn
    assert entry.attempts == 0                      # 条目未被派发记账

    # 观测不受影响: scan 的红 run 扫描 + triage 幂等搜索照常执行
    run = _run(999, "Sanitizer", "schedule", "failure")
    state_obs = {"entries": [], "auth_failure_streak": 0}
    ct.triage_new_red_runs(state_obs, [run])
    search_states = [a[a.index("--state") + 1]
                     for a in fake.calls if a[:2] == ["search", "issues"]]
    assert search_states == ["open", "closed"]      # 幂等搜索确实发生
    assert len(fake.created_issues) == 1            # 观测/issue 建立照常


# ---------------------------------------------------------------------------
# 用例 22: state 丢失后按 GitHub 标记恢复(Review Focus #3)——复用不重建

def test_state_loss_recovers_from_github_markers(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    # state 全丢,但 GitHub 上该 run 的标记 issue 仍 open → 复用
    fake = FakeGh(runs=[], search_result=[
        {"number": 77, "title": "[ci-triage] Sanitizer 红",
         "body": "run 链接\n<!-- ci-triage:run:38482214201 -->\n更多"},
    ])
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)

    run = _run(38482214201, "Sanitizer", "schedule", "failure")
    state = {"entries": [], "auth_failure_streak": 0}   # 空 state(丢失现场)
    new = ct.triage_new_red_runs(state, [run])

    # 复用不重建: issue 创建数 0;条目恢复 issue_number==77
    assert fake.created_issues == []
    assert len(new) == 1
    assert new[0].issue_number == 77
    assert new[0].status == "ISSUED"
    assert any(d["issue_number"] == 77 for d in state["entries"])


# ---------------------------------------------------------------------------
# 用例 23: scan 互斥锁(A4)——锁被占时立即返回、零 gh 调用

def test_scan_lock_excludes_concurrent(monkeypatch, tmp_path):
    import fcntl
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    fake = FakeGh(runs=[])
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)
    monkeypatch.setattr(ct, "_git", lambda args: "")

    # 测试内先占住 scan.lock(与 cmd_scan 相同的 LOCK_EX|LOCK_NB 语义)
    lock_path = tmp_path / "scan.lock"
    held = open(lock_path, "w")
    fcntl.flock(held.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    try:
        rc = ct.cmd_scan(argparse.Namespace(dry_run=False))
        assert rc == 0                       # 拿不到锁: 打印并退出码 0
        assert fake.calls == []              # 零 gh 调用(观测都没做)
    finally:
        fcntl.flock(held.fileno(), fcntl.LOCK_UN)
        held.close()


# ---------------------------------------------------------------------------
# 用例 24: BOT_FIX 的 auth_fail 同样计入熔断(裁定 7: 跨车道统一)——
#          controller 括注「现有逻辑已如此」与 Task 3 实码不符(BOT_FIX 分支
#          在 streak 逻辑前即 continue),本用例把主条款钉进测试。

def test_bot_fix_auth_fail_counts_toward_circuit_breaker(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    fake = FakeGh()
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)
    monkeypatch.setattr(ct, "check_agent_outcome", lambda e: "auth_fail")

    # 场景 1: 未达阈值(第 2 次)——BOT_FIX 条目计入 streak、击杀清引用、
    # 归还 PR_OPEN 且清 head 锚(同 head 重派不被单派规则拦死)
    p1 = subprocess.Popen(["sleep", "30"], start_new_session=True)
    a = _sample_entry(workflow="Sanitizer", run_id=31, status="BOT_FIX",
                      issue_number=111, pr_number=88, agent_pid=p1.pid,
                      attempts=1, rounds=2,
                      agent_log=str(tmp_path / "a.log"),
                      last_dispatched_head="d" * 40, extra_runs=[])
    state = {"entries": [a.to_dict()], "auth_failure_streak": 1}
    ct.reap_or_advance(state)
    ea = ct.StateStore.entries(state)[0]
    assert state["auth_failure_streak"] == 2        # 跨车道计入
    assert ea.status == "PR_OPEN"                   # 归还车道 B 重新判定
    assert ea.agent_pid is None
    assert ea.last_dispatched_head is None          # head 锚已清(可同 head 重派)
    assert _wait_dead(p1) is not None               # 存活 agent 被击杀
    assert not (tmp_path / "PAUSED").exists()       # 未达阈值不熔断

    # 场景 2: 达阈值(第 3 次)——写 PAUSED + ESCALATED + @人工评论
    p2 = subprocess.Popen(["sleep", "30"], start_new_session=True)
    b = _sample_entry(workflow="CodeQL", run_id=32, status="BOT_FIX",
                      issue_number=112, pr_number=89, agent_pid=p2.pid,
                      attempts=1, rounds=3, agent_log=str(tmp_path / "b.log"),
                      extra_runs=[])
    state2 = {"entries": [b.to_dict()], "auth_failure_streak": 2}
    ct.reap_or_advance(state2)
    eb = ct.StateStore.entries(state2)[0]
    assert (tmp_path / "PAUSED").exists()           # 跨车道熔断
    assert eb.status == "ESCALATED"
    assert eb.agent_pid is None
    assert _wait_dead(p2) is not None
    assert any("认证" in body for _, body in fake.comments)


# ===========================================================================
# Task 4 fix round 1: 检视修复测试(Critical#1 死锁 / Important#2 顶层
# GhError / Important#3 轮数 off-by-one / Important#4 streak 归零)。
# 口径同 Task 4 主批: 全部离线——fake gh / fake run_background / fake
# pr_loop 函数 / fake _git,绝不触网、绝不真实派 claude。
# ===========================================================================

# ---------------------------------------------------------------------------
# 用例 25(Critical#1): bot agent dead/timeout 退场 → 归还 PR_OPEN 且清
# head 锚 → 同 head 允许重派(rounds+1);rounds 达 MAX_ROUNDS(4)后再跑
# → 不再派发而是 ESCALATED——死/超时退场不再永久停滞,重派由轮数上限
# 有界兜底。对称参照: auth_fail 归还路径的清锚(用例 24 场景 1)。

def test_bot_fix_dead_agent_returns_pr_open_and_re_dispatches(
        monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    monkeypatch.setattr(ct, "gh_json", FakeGh().gh_json)
    bg = FakeBg()
    monkeypatch.setattr(ct, "run_background", bg)

    pr = _fake_pr(head_sha="b" * 40)
    _fake_pr_loop(monkeypatch, pr, "READY_TO_FIX")

    # 场景 1a: dead/timeout 退场(非 auth_fail)——BOT_FIX + 死 PID + head 锚
    # 已记。先用 WAITING_BOTS 隔离观察「归还」语义: 归还 PR_OPEN 且清
    # head 锚(旧实现不清锚——归还后锚仍指向 "b"*40,即 Critical#1 死锁)。
    entry = _sample_entry(workflow="Sanitizer", run_id=41, status="BOT_FIX",
                          issue_number=77, pr_number=88,
                          agent_pid=999999901,   # 必死伪 PID(>pid_max)
                          agent_started_at=datetime.now(
                              timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                          agent_log=str(tmp_path / "agent-41.log"),
                          last_dispatched_head="b" * 40, rounds=2,
                          extra_runs=[])
    (tmp_path / "agent-41.log").write_text("…中途暴毙…\n", encoding="utf-8")
    state = {"entries": [entry.to_dict()], "auth_failure_streak": 0}

    _fake_pr_loop(monkeypatch, pr, "WAITING_BOTS")
    ct.lane_b_advance(state, dry_run=False)
    e = ct.StateStore.entries(state)[0]
    assert e.status == "PR_OPEN"                # 归还车道 B 重新判定
    assert e.agent_pid is None                  # 单飞配额已归还
    assert e.last_dispatched_head is None       # head 锚已清(Critical#1 修复)
    assert len(bg.calls) == 0                   # WAITING_BOTS 不派发

    # 场景 1b: 同 head(锚已清)→ READY_TO_FIX 允许重派(rounds+1,记新锚)
    _fake_pr_loop(monkeypatch, pr, "READY_TO_FIX")
    ct.lane_b_advance(state, dry_run=False)
    e2 = ct.StateStore.entries(state)[0]
    assert e2.status == "BOT_FIX"               # 重派记账
    assert e2.rounds == 3                       # 同 head 重派消耗一轮(rounds+1)
    assert len(bg.calls) == 1                   # 同 head 允许重派(死锁已除)
    assert e2.last_dispatched_head == "b" * 40  # 重派记新锚

    # 场景 2: rounds 已达 MAX_ROUNDS(4)的 BOT_FIX 第三次退场 → 归还清锚
    # 后不再重派而是升级人工(轮数上限有界兜底;旧 `>` 是 off-by-one——
    # rounds=4 时还会派第 5 轮)。锚同 head,排除「同 head 单派拦截」对
    # 「上限拦截」的假阳性掩护——不派发只能因轮数已达上限。
    third = _sample_entry(workflow="Sanitizer", run_id=41, status="BOT_FIX",
                          issue_number=77, pr_number=88,
                          agent_pid=999999903,   # 又死了
                          agent_started_at=datetime.now(
                              timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                          agent_log=str(tmp_path / "agent-41.log"),
                          last_dispatched_head="b" * 40, rounds=4,
                          extra_runs=[])
    state["entries"] = [third.to_dict()]
    ct.lane_b_advance(state, dry_run=False)
    e3 = ct.StateStore.entries(state)[0]
    assert e3.status == "ESCALATED"             # 不再派发,升级人工
    assert e3.agent_pid is None
    assert e3.rounds == 4                       # 轮数不再增长(未派第 5 轮)
    assert len(bg.calls) == 1                   # 达上限后零新派发


# ---------------------------------------------------------------------------
# 用例 26(Important#2): 顶层 GhError 捕获覆盖全部推进主体——lane_b 阶段
# gh 抛 GhError → cmd_scan 返回 1、stderr 有错误信息、GhError 发生前已
# 推进的 state(reap 记账)落盘。旧实现 try 只包 scan_red_runs,lane_b 的
# 裸 GhError 会崩掉整个 scan: cron 日志不可读、state 不落盘,已发生的
# 升级评论下轮可能重复。

def test_cmd_scan_toplevel_gherror_returns_1_and_saves_state(
        monkeypatch, tmp_path, capsys):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)

    # lane_b 阶段: classify=READY_TO_MERGE,单条目级 try 内的 fetch_pr 走
    # fake 不抛;抛点选该分支的 gh issue close(lane_b 的真实 gh 写路径,
    # 无单条目 except 包裹)——正是检视所述「lane_b 的 gh 写任一抛 GhError
    # 都会裸 traceback」的场景。
    pr = _fake_pr()
    _fake_pr_loop(monkeypatch, pr, "READY_TO_MERGE")

    fake = FakeGh(runs=[])
    # READY_TO_MERGE 分支的 gh issue close 抛 GhError(非 fetch 路径——
    # 那条已被单条目 except 消化,构不成顶层验证)
    def gh_boom(args, input_text=None):
        fake.calls.append(list(args))
        if args[:2] == ["issue", "close"]:
            raise ct.GhError("模拟 gh issue close 失败")
        return fake.gh_json(args, input_text)
    monkeypatch.setattr(ct, "gh_json", gh_boom)

    bg = FakeBg()
    monkeypatch.setattr(ct, "run_background", bg)
    monkeypatch.setattr(ct, "_git", lambda args: "")

    # 预置: 一个 reap 待收割条目(dead → attempts 1→2 记账)+ 一个 lane_b
    # 待收尾条目(READY_TO_MERGE → issue close 抛 GhError)
    reaped = _sample_entry(
        workflow="Sanitizer", run_id=51, status="DIAGNOSING", issue_number=151,
        agent_pid=999999901, attempts=1,
        agent_started_at=datetime.now(timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ"),
        agent_log=str(tmp_path / "agent-51.log"), extra_runs=[])
    (tmp_path / "agent-51.log").write_text("…暴毙…\n", encoding="utf-8")
    laneb = _sample_entry(
        workflow="CodeQL", run_id=52, status="PR_OPEN", issue_number=152,
        pr_number=88, rounds=1, agent_pid=None, agent_started_at=None,
        agent_log=None, last_dispatched_head=None, extra_runs=[])

    store = ct.StateStore()
    state = {"entries": [reaped.to_dict(), laneb.to_dict()],
             "auth_failure_streak": 0}
    store.save(state)                            # 预置 state 落盘

    rc = ct.cmd_scan(argparse.Namespace(dry_run=False))
    captured = capsys.readouterr()

    assert rc == 1                               # 顶层捕获 → 返回 1
    assert "GhError" in captured.err             # stderr 有错误信息
    # GhError 发生前已推进的 state 落盘: reap 记账 attempts 1→2 + 派发扫
    # 重派 2→3(顺带确认派发记账也未丢)
    saved = ct.StateStore().load()
    saved_entries = {e["run_id"]: e for e in saved["entries"]}
    assert saved_entries[51]["attempts"] == 3    # reap+派发扫推进均未丢
    assert saved_entries[51]["status"] == "DIAGNOSING"


# ---------------------------------------------------------------------------
# 用例 27(Important#3): 轮数上限 ≤4 轮(inclusive)——rounds=4 ==
# MAX_ROUNDS → 不派发、ESCALATED。旧 `rounds > MAX_ROUNDS` 允许 rounds=4
# 时派第 5 轮(off-by-one,违反全局约束「车道 B ≤4 rounds」)。

def test_lane_b_rounds_cap_is_inclusive(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    monkeypatch.setattr(ct, "gh_json", FakeGh().gh_json)
    bg = FakeBg()
    monkeypatch.setattr(ct, "run_background", bg)
    assert ct.MAX_ROUNDS == 4                    # 全局约束锚

    _fake_pr_loop(monkeypatch, _fake_pr(head_sha="e" * 40), "READY_TO_FIX")
    entry = _sample_entry(workflow="Sanitizer", run_id=53, status="PR_OPEN",
                          issue_number=77, pr_number=88, rounds=4,
                          agent_pid=None, agent_started_at=None,
                          agent_log=None, last_dispatched_head="d" * 40,
                          extra_runs=[])
    state = {"entries": [entry.to_dict()], "auth_failure_streak": 0}

    ct.lane_b_advance(state, dry_run=False)

    e = ct.StateStore.entries(state)[0]
    assert e.status == "ESCALATED"               # rounds 已达 4 → 升级
    assert e.rounds == 4                         # 不再消耗第 5 轮
    assert len(bg.calls) == 0                    # 零派发


# ---------------------------------------------------------------------------
# 用例 28(Important#4): BOT_FIX success → auth_failure_streak 归零。
# 车道 A 的 success 归零已存在(reap :950);BOT_FIX 落「只记录」分支不清
# → 「连续 ≥3」退化成「累计 ≥3」,跨周期间歇 auth_fail 会过早全局 PAUSED。

def test_bot_fix_success_resets_auth_streak(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("CITRIAGE_REPO", REPO)
    fake = FakeGh()
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)
    monkeypatch.setattr(ct, "check_agent_outcome", lambda e: "success")

    entry = _sample_entry(workflow="Sanitizer", run_id=54, status="BOT_FIX",
                          issue_number=77, pr_number=88, agent_pid=999999901,
                          attempts=1, rounds=2,
                          agent_log=str(tmp_path / "agent-54.log"),
                          last_dispatched_head="b" * 40, extra_runs=[])
    (tmp_path / "agent-54.log").write_text(
        "处置完成\nci-triage: done fix/ci-54\n", encoding="utf-8")
    state = {"entries": [entry.to_dict()], "auth_failure_streak": 2}

    ct.reap_or_advance(state)

    e = ct.StateStore.entries(state)[0]
    assert e.status == "BOT_FIX"                 # pr_loop 接管期只记录
    assert e.agent_pid == 999999901              # 引用不动(条目推进归 lane_b)
    assert state["auth_failure_streak"] == 0     # 成功即证明凭据活着,归零
    # 对照: 归零后单次 auth_fail(条目 A)未达阈值;若未归零,2+1=3 会熔断
    other = _sample_entry(workflow="CodeQL", run_id=55, status="DIAGNOSING",
                          issue_number=78, agent_pid=999999902, attempts=1,
                          agent_log=str(tmp_path / "agent-55.log"),
                          extra_runs=[])
    (tmp_path / "agent-55.log").write_text("API Error: 403\n",
                                           encoding="utf-8")
    state["entries"].append(other.to_dict())
    monkeypatch.setattr(ct, "check_agent_outcome",
                        lambda e: "auth_fail" if e.run_id == 55 else "success")
    ct.reap_or_advance(state)
    assert state["auth_failure_streak"] == 1     # 从零重新累计(未熔断)
    assert not (tmp_path / "PAUSED").exists()
