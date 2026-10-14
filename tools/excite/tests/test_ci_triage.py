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
"""
import os, sys
from datetime import datetime, timezone

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "..", "scripts"))
import ci_triage as ct


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
    """记录 gh 调用的 fake: run_list(读) / issue_create(写) / comment(写)。"""

    def __init__(self, runs=None, search_result=None, log_failed="job x failed"):
        self.runs = runs or []
        self.search_result = search_result or []   # gh search issues --json 返回
        self.log_failed = log_failed
        self.calls = []                            # 全部 gh_json 调用参数
        self.created_issues = []                   # (title, body)
        self.comments = []                         # (issue_number, body)

    def gh_json(self, args, input_text=None):
        """模块级 gh_json 的替身: 按 gh 子命令分派。

        input_text 对应实现的 stdin 传参(gh issue create --body-file -)。
        """
        self.calls.append(list(args))
        if args[:2] == ["run", "list"]:
            return self.runs
        if args[:2] == ["search", "issues"]:
            return self.search_result
        if args[:3] == ["issue", "create", "--repo"]:
            # gh issue create --body-file -: 正文经 stdin(input_text)传入
            self.created_issues.append((args[args.index("--title") + 1],
                                        input_text))
            return "https://github.com/example/sdcshield/issues/77"
        if args[:2] == ["issue", "comment"] and args[2].isdigit():
            # gh issue comment <n> --repo <slug> --body <text>
            self.comments.append((int(args[2]), args[args.index("--body") + 1]))
            return "https://github.com/example/sdcshield/issues/77#comment"
        if args[:2] == ["run", "view"] and "--log-failed" in args:
            # gh run view <id> --repo <slug> --log-failed(纯文本输出)
            return self.log_failed
        raise AssertionError(f"未预期的 gh 调用: {args}")


# ---------------------------------------------------------------------------
# 用例 3: 扫描过滤——工作流 × 事件 × 分支 × 结论

def test_scan_filters_by_workflow_event_branch(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
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
    # gh run list 调用必须锚定 main 分支
    assert any(a[:2] == ["run", "list"] and ct.repo_slug() in a
               and "--branch" in a and a[a.index("--branch") + 1] == "main"
               for a in fake.calls)


# ---------------------------------------------------------------------------
# 用例 4: 首个红 run → 建一个 issue(标记/横幅/失败日志摘录),条目 ISSUED

def test_first_red_run_creates_issue(monkeypatch, tmp_path):
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path))
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
    search_result = [
        {"number": 12, "title": "unrelated", "body": "no marker here"},
        {"number": 77, "title": "[ci-triage] Sanitizer 红",
         "body": "run 链接\n<!-- ci-triage:run:38482214201 -->\n更多"},
    ]
    fake = FakeGh(runs=[], search_result=search_result)
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)

    assert ct.find_issue_by_run(38482214201) == 77
    # 无匹配 → None(调用方据此决定创建)
    fake.search_result = []
    monkeypatch.setattr(ct, "gh_json", fake.gh_json)
    assert ct.find_issue_by_run(12345) is None
    # 调用形态: gh search issues --state open,限定 "ci-triage:run" 文本
    assert any(a[0] == "search" and a[1] == "issues" and "--state" in a
               and a[a.index("--state") + 1] == "open"
               for a in fake.calls)
