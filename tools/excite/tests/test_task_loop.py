"""task_loop 统一引擎测试(PR-1b 起): schema v2 / 状态归一 / 目录迁移。"""
import sys, os, json
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "..", "scripts"))
import task_loop as tl


def test_entry_from_dict_v1_normalized():
    """v1 state(旧状态名、无 source/kind/audit_hash)加载归一为 v2。"""
    d = {"workflow": "Sanitizer", "run_id": 123, "status": "DIAGNOSING",
         "issue_number": 9, "fix_branch": "fix/ci-123", "pr_number": None,
         "attempts": 1, "rounds": 0, "agent_pid": 42, "agent_started_at": None,
         "agent_log": None, "last_dispatched_head": None, "extra_runs": [],
         "created_at": "2026-10-01T00:00:00Z", "updated_at": "2026-10-01T00:00:00Z"}
    e = tl.Entry.from_dict(d)
    assert e.status == "DISPATCHED"
    assert e.source == "ci" and e.kind == "bug" and e.audit_hash is None


def test_entry_roundtrip_keeps_v2_fields():
    e = tl.Entry(workflow="", run_id=0, status="QUEUED", issue_number=7,
                 fix_branch="", pr_number=None, attempts=0, rounds=0,
                 agent_pid=None, agent_started_at=None, agent_log=None,
                 last_dispatched_head=None, extra_runs=[], created_at="t",
                 updated_at="t", source="session", kind="feature",
                 audit_hash=None)
    assert tl.Entry.from_dict(e.to_dict()) == e


# ============ Task 3: state 目录迁移 + env 双轨 ============

def test_state_dir_env_priority_and_default(tmp_path, monkeypatch):
    monkeypatch.delenv("TASKLOOP_STATE_DIR", raising=False)
    monkeypatch.delenv("CITRIAGE_STATE_DIR", raising=False)
    monkeypatch.setattr(tl, "DEFAULT_STATE_DIR", str(tmp_path / "new"))
    assert tl._resolve_state_dir() == str(tmp_path / "new")
    monkeypatch.setenv("CITRIAGE_STATE_DIR", str(tmp_path / "old"))
    assert tl._resolve_state_dir() == str(tmp_path / "old")
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "win"))
    assert tl._resolve_state_dir() == str(tmp_path / "win")


def test_migrate_legacy_state_dir_idempotent(tmp_path, monkeypatch):
    legacy = tmp_path / "legacy"; new = tmp_path / "new"
    legacy.mkdir(); new.mkdir()
    (legacy / "state.json").write_text('{"entries": [], "auth_failure_streak": 0}')
    (legacy / "agent.env").write_text("# env\n")
    monkeypatch.setattr(tl, "LEGACY_STATE_DIR", str(legacy))
    monkeypatch.setattr(tl, "DEFAULT_STATE_DIR", str(new))
    monkeypatch.delenv("TASKLOOP_STATE_DIR", raising=False)
    monkeypatch.delenv("CITRIAGE_STATE_DIR", raising=False)
    tl._migrate_legacy_state_dir()
    assert (new / "state.json").exists() and (new / "agent.env").exists()
    assert (legacy / "state.json").exists()          # 旧目录保留归档
    tl._migrate_legacy_state_dir()                    # 复跑零副作用
    assert (new / "state.json").read_text() == '{"entries": [], "auth_failure_streak": 0}'


def test_state_store_load_quarantines_corrupt_json(tmp_path, monkeypatch):
    """轮5 B41: 损坏 state.json 不裸崩——隔离现场 + 空状态起步。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    (tmp_path / "state.json").write_text('{"entries": [',)   # 截断 JSON
    store = tl.StateStore()
    state = store.load()
    assert state == {"entries": [], "auth_failure_streak": 0}
    quarantined = list(tmp_path.glob("state.json.corrupt-*"))
    assert len(quarantined) == 1 and not (tmp_path / "state.json").exists()
    # 复跑: 隔离后 load 走首跑语义,不再崩溃
    assert store.load() == {"entries": [], "auth_failure_streak": 0}


def test_state_store_load_non_dict_json(tmp_path, monkeypatch):
    """轮5 B41: 有效 JSON 但非对象(手编成数组)同处置。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    (tmp_path / "state.json").write_text('[]')
    state = tl.StateStore().load()
    assert state == {"entries": [], "auth_failure_streak": 0}
    assert len(list(tmp_path.glob("state.json.corrupt-*"))) == 1


def test_state_entries_skip_malformed_entry(tmp_path, monkeypatch):
    """轮5 B41: 单条目缺硬键跳过并告警,不掀翻整个 entries 解析。"""
    good = {"workflow": "Sanitizer", "run_id": 1, "status": "QUEUED",
            "issue_number": 9, "fix_branch": "fix/ci-1", "pr_number": None,
            "attempts": 0, "rounds": 0, "agent_pid": None,
            "agent_started_at": None, "agent_log": None,
            "last_dispatched_head": None, "extra_runs": [],
            "created_at": "t", "updated_at": "t"}
    bad = dict(good); del bad["run_id"]                 # 手编漏键
    entries = tl.StateStore.entries({"entries": [good, bad, "oops"]})
    assert len(entries) == 1 and entries[0].run_id == 1


def test_migrate_heals_missing_agent_env(tmp_path, monkeypatch):
    """轮5 B42/会话#2: 半迁移自愈——state.json 已就位但 agent.env 缺失
    (两次拷贝间被杀),下 tick 补齐而不被早退条件漏掉。"""
    legacy = tmp_path / "legacy"; new = tmp_path / "new"
    legacy.mkdir(); new.mkdir()
    (legacy / "state.json").write_text('{"entries": [], "auth_failure_streak": 0}')
    (legacy / "agent.env").write_text("# env\n")
    (new / "state.json").write_text('{"entries": [], "auth_failure_streak": 0}')
    monkeypatch.setattr(tl, "LEGACY_STATE_DIR", str(legacy))
    monkeypatch.setattr(tl, "DEFAULT_STATE_DIR", str(new))
    monkeypatch.delenv("TASKLOOP_STATE_DIR", raising=False)
    monkeypatch.delenv("CITRIAGE_STATE_DIR", raising=False)
    tl._migrate_legacy_state_dir()
    assert (new / "agent.env").exists()
    assert (new / "state.json").read_text() == \
        '{"entries": [], "auth_failure_streak": 0}'   # 状态不被旧目录覆写


def test_migrate_agent_env_without_legacy_state(tmp_path, monkeypatch):
    """轮5 会话#2 场景(a): legacy 有凭据无 state.json(凭据先就位、旧
    工具从未落盘)——agent.env 仍补齐,不建 state.json。"""
    legacy = tmp_path / "legacy"; new = tmp_path / "new"
    legacy.mkdir(); new.mkdir()
    (legacy / "agent.env").write_text("# env\n")
    monkeypatch.setattr(tl, "LEGACY_STATE_DIR", str(legacy))
    monkeypatch.setattr(tl, "DEFAULT_STATE_DIR", str(new))
    monkeypatch.delenv("TASKLOOP_STATE_DIR", raising=False)
    monkeypatch.delenv("CITRIAGE_STATE_DIR", raising=False)
    tl._migrate_legacy_state_dir()
    assert (new / "agent.env").exists()
    assert not (new / "state.json").exists()           # 无状态可迁不凭空造


def test_migrate_failure_no_partial_and_runtime_error(tmp_path, monkeypatch):
    """轮5 B42: 拷贝中途失败不留半份 state.json,转 RuntimeError(顶层
    except 认它干净退出),旧目录原样保留。"""
    legacy = tmp_path / "legacy"; new = tmp_path / "new"
    legacy.mkdir(); new.mkdir()
    (legacy / "state.json").write_text('{"entries": [], "auth_failure_streak": 0}')
    monkeypatch.setattr(tl, "LEGACY_STATE_DIR", str(legacy))
    monkeypatch.setattr(tl, "DEFAULT_STATE_DIR", str(new))
    monkeypatch.delenv("TASKLOOP_STATE_DIR", raising=False)
    monkeypatch.delenv("CITRIAGE_STATE_DIR", raising=False)

    real_replace = os.replace
    def boom(src, dst):
        if str(dst).endswith("state.json"):
            raise OSError("disk full (simulated)")
        return real_replace(src, dst)
    monkeypatch.setattr(tl.os, "replace", boom)
    try:
        tl._migrate_legacy_state_dir()
        raised = False
    except RuntimeError as exc:
        raised = "迁移失败" in str(exc)
    assert raised
    assert not (new / "state.json").exists()           # 无半份
    assert (legacy / "state.json").exists()            # 旧目录保留
    assert not list(new.glob("state.json.tmp-migrate"))  # tmp 现场已清理


def test_paused_checks_both_paths(tmp_path, monkeypatch):
    monkeypatch.setattr(tl, "LEGACY_STATE_DIR", str(tmp_path / "l"))
    monkeypatch.setattr(tl, "DEFAULT_STATE_DIR", str(tmp_path / "n"))
    (tmp_path / "l").mkdir(); (tmp_path / "l" / "PAUSED").touch()
    assert tl._paused() is True                       # 过渡期旧 PAUSED 也生效


# ============ Task 4: gh label 原语 + 幂等写重试 ============

class FakeGh:
    def __init__(self, fail_transient=0):
        self.calls = []; self._fail = fail_transient
    def gh_json(self, args, input_text=None):
        self.calls.append((tuple(args), input_text))
        if self._fail > 0:
            self._fail -= 1
            raise tl.GhError("gh api … 失败(exit 1): HTTP 500")
        if args[0] == "issue" and args[1] == "create":
            return "https://github.com/x/y/issues/42"
        return None

def test_gh_json_idem_retries_transient(tmp_path, monkeypatch):
    fake = FakeGh(fail_transient=2)
    monkeypatch.setattr(tl, "gh_json", fake.gh_json)
    tl.gh_json_idem(["issue", "comment", "1", "--body", "x"])   # 3 次尝试内成功
    assert len(fake.calls) == 3

def test_gh_json_idem_no_retry_on_4xx(monkeypatch):
    fake = FakeGh(fail_transient=0)
    def always_4xx(args, input_text=None):
        fake.calls.append(tuple(args))
        raise tl.GhError("gh api … 失败(exit 1): HTTP 404")
    monkeypatch.setattr(tl, "gh_json", always_4xx)
    try:
        tl.gh_json_idem(["issue", "comment", "1", "--body", "x"])
        assert False, "4xx 必须首次即抛"
    except tl.GhError:
        assert len(fake.calls) == 1

def test_create_task_issue_labels_and_marker(monkeypatch):
    fake = FakeGh()
    monkeypatch.setattr(tl, "gh_json", fake.gh_json)
    n = tl.create_task_issue("t", "body", ["agent-task", "agent-bug"])
    assert n == 42
    args, body = fake.calls[0]
    assert "--label" in args and "agent-task" in args


# ============ Task 5: intake_session CLI ============

def test_enqueue_bug_creates_labeled_issue(tmp_path, monkeypatch, capsys):
    fake = FakeGh()
    monkeypatch.setattr(tl, "gh_json", fake.gh_json)
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    bf = tmp_path / "b.md"; bf.write_text("正文")
    rc = tl.main(["enqueue-bug", "--title", "t", "--body-file", str(bf)])
    assert rc == 0
    args, body = fake.calls[0]
    assert "agent-task" in args and "agent-bug" in args and "bug" in args
    assert tl.UNTRUSTED_BANNER in body

def test_enqueue_bug_findings_file_does_not_read_stdin(tmp_path, monkeypatch,
                                                       capsys):
    """PR#317 检视#9: --findings-file 路径正文完全不用,不得先读 stdin——
    规范用法(dev-loop.md)恰是不带 --body-file 的裸调用,人工在终端执行
    会永久挂起等 stdin。"""
    class _Boom:
        def read(self, *a):
            raise AssertionError(
                "enqueue-bug --findings-file 不得读 stdin(人工终端会挂起)")
    monkeypatch.setattr(tl.sys, "stdin", _Boom())
    fake = FakeGh()
    monkeypatch.setattr(tl, "gh_json", fake.gh_json)
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    ff = tmp_path / "f.json"
    ff.write_text(json.dumps([
        {"path": "a.c", "line": 1, "severity": "Minor", "category": "正确性",
         "message": "m1", "evidence": "e1"},
        {"path": "b.c", "line": 2, "severity": "Minor", "category": "性能",
         "message": "m2", "evidence": "e2"}]))
    rc = tl.main(["enqueue-bug", "--findings-file", str(ff)])
    assert rc == 0
    created = [c for c in fake.calls if c[0][:2] == ("issue", "create")]
    assert len(created) == 2                       # 一条 finding 一个 issue
    bodies = [c[1] for c in created]
    assert any("m1" in b for b in bodies) and tl.UNTRUSTED_BANNER in bodies[0]

def test_enqueue_bug_skips_non_dict_findings(tmp_path, monkeypatch, capsys):
    """轮5(近miss A5): findings 文件混入非字典元素——跳过并告警,不
    AttributeError 裸 traceback 中途崩;合法条目照常建。"""
    class _Boom:
        def read(self, *a):
            raise AssertionError("findings 路径不得读 stdin")
    monkeypatch.setattr(tl.sys, "stdin", _Boom())
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    monkeypatch.setattr(tl, "_search_finding_issue", lambda h: None)
    created = []
    monkeypatch.setattr(tl, "create_task_issue",
                        lambda title, body, labels:
                        created.append(title) or 42)
    good = {"path": "a.c", "line": 1, "severity": "Minor",
            "category": "正确性", "message": "m1", "evidence": "e1"}
    ff = tmp_path / "bad.json"
    ff.write_text(json.dumps([good, "oops", 42]), encoding="utf-8")
    assert tl.main(["enqueue-bug", "--findings-file", str(ff)]) == 0
    assert created == ["[review] m1"]        # 只有合法条目建了 issue
    assert "跳过非对象 finding" in capsys.readouterr().err


def test_enqueue_bug_bad_json_clean_error(tmp_path, monkeypatch, capsys):
    """轮6(#53): findings 文件本身不是合法 JSON(截断/手编坏)——干净报错
    返回 2,不裸 traceback(A5 只覆盖了数组内非 dict 元素,漏了这道)。"""
    class _Boom:
        def read(self, *a):
            raise AssertionError("findings 路径不得读 stdin")
    monkeypatch.setattr(tl.sys, "stdin", _Boom())
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    ff = tmp_path / "truncated.json"
    ff.write_text('[{"path": "a.c", "line": 1,', encoding="utf-8")
    assert tl.main(["enqueue-bug", "--findings-file", str(ff)]) == 2
    assert "不是合法 JSON" in capsys.readouterr().err


def test_audit_paused_observes_range_then_skips_claude(
        tmp_path, monkeypatch, capsys):
    """轮6(#54): PAUSED 时 audit 先完成观测(fetch/区间计算,与 scan 线
    「观测照常」对齐)再跳过 claude——不再在观测之前就整体早退。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("TASKLOOP_CLONE_DIR", str(tmp_path / "clone"))
    base = "base0000000000000000000000000000000000000000"
    head = "head1111111111111111111111111111111111111111"
    git_out = {"fetch": "", "rev-parse": head, "log": "abc1234 commit one"}
    monkeypatch.setattr(tl, "_git", lambda args, clone_dir=None:
                        git_out.get(args[0], ""))
    monkeypatch.setattr(tl, "_git_diff", lambda b, h: "+diff")
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    monkeypatch.setattr(tl, "_paused", lambda: True)
    claude_calls = []
    monkeypatch.setattr(tl.pr_local_review, "run_claude",
                        lambda *a, **k: claude_calls.append(a))
    tl.StateStore(str(tmp_path)).save(
        {"entries": [], "auth_failure_streak": 0,
         "audit_cursor": {"sha": base, "at": "t"}})
    assert tl.run_audit(dry_run=False) == 0
    assert claude_calls == []                    # claude 被闸
    out = capsys.readouterr().out
    assert "本轮仅观测" in out and "1 commits" in out   # 观测照常展示区间


def test_enqueue_bug_findings_dedup_via_marker(tmp_path, monkeypatch, capsys):
    """PR#317 检视#16(轮2): session findings 批量路径幂等——每条 finding
    以 _audit_hash 归一化 hash 嵌 body marker(<!-- task-loop:finding:h -->),
    gh search 命中 issue 即跳过;中途瞬态失败后重跑不重复建 issue
    (对照: ci 源有 RUN_MARKER_FMT、audit 源有双源查重,session 源原为裸奔)。
    轮5(会话#1): 双态搜索——marker issue 被人工驳回关闭后,重跑不得
    重建被拒 finding 并自动派 agent(与检视#35 audit 侧同缺陷类)。"""
    class _Boom:
        def read(self, *a):
            raise AssertionError("findings 路径不得读 stdin")
    monkeypatch.setattr(tl.sys, "stdin", _Boom())
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    f1 = {"path": "a.c", "line": 1, "severity": "Minor", "category": "正确性",
          "message": "m1", "evidence": "e1"}
    f2 = {"path": "b.c", "line": 2, "severity": "Minor", "category": "性能",
          "message": "m2", "evidence": "e2"}
    h1, h2 = tl._audit_hash(f1), tl._audit_hash(f2)
    # 重跑场景: f2 的 marker issue 已被人工驳回关闭(轮5 双态核心场景)
    hit_bodies = {h2: "closed"}          # h → 该 marker issue 所处状态
    states_queried = []
    created = []
    def fake_gh(args, input_text=None):
        if args[:2] == ["search", "issues"]:
            states_queried.append(args[args.index("--state") + 1])
            q = args[-1]
            for h, st in hit_bodies.items():
                if q == f"task-loop:finding:{h}" and \
                        st == args[args.index("--state") + 1]:
                    return [{"number": 4242, "title": "t",
                             "body": tl.FINDING_MARKER_FMT.format(hash=h)}]
            return []
        if args[:2] == ["issue", "create"]:
            created.append((args, input_text))
            return "https://github.com/x/y/issues/42"
        return None
    monkeypatch.setattr(tl, "gh_json", fake_gh)
    ff = tmp_path / "f.json"; ff.write_text(json.dumps([f1, f2]))
    assert tl.main(["enqueue-bug", "--findings-file", str(ff)]) == 0
    assert len(created) == 1, "marker 命中的 finding(含已关闭)不得重复建 issue"
    assert states_queried[:2] == ["open", "closed"], \
        "查重须双态搜索(open 命中即短路,closed 兜底)"
    args, body = created[0]
    assert tl.FINDING_MARKER_FMT.format(hash=h1) in body, \
        "新建 issue 正文须嵌 finding marker(供重跑查重)"
    assert tl.UNTRUSTED_BANNER in body
    out = capsys.readouterr().out
    assert "已见过" in out
    # 完整重跑: 两条全部命中 → 零建
    hit_bodies[h1] = "open"
    created.clear()
    ff2 = tmp_path / "f2.json"; ff2.write_text(json.dumps([f1, f2]))
    assert tl.main(["enqueue-bug", "--findings-file", str(ff2)]) == 0
    assert created == []

def test_enqueue_bug_empty_findings_file_is_noop(tmp_path, monkeypatch, capsys):
    """PR#317 检视#9 附: 空 findings 数组 = 零问题待入队——零 issue、零
    stdin 读取(旧实现会落入单条路径读 stdin 挂起/建占位 issue)。"""
    class _Boom:
        def read(self, *a):
            raise AssertionError("空 findings 也不得读 stdin")
    monkeypatch.setattr(tl.sys, "stdin", _Boom())
    fake = FakeGh()
    monkeypatch.setattr(tl, "gh_json", fake.gh_json)
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    ff = tmp_path / "empty.json"; ff.write_text("[]")
    assert tl.main(["enqueue-bug", "--findings-file", str(ff)]) == 0
    assert [c for c in fake.calls if c[0][:2] == ("issue", "create")] == []


def test_enqueue_task_requires_approval_hint(capsys, tmp_path, monkeypatch):
    fake = FakeGh()
    monkeypatch.setattr(tl, "gh_json", fake.gh_json)
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    bf = tmp_path / "b.md"; bf.write_text("正文")
    rc = tl.main(["enqueue-task", "--title", "t", "--body-file", str(bf)])
    assert rc == 0
    out = capsys.readouterr().out
    assert "agent-approved" in out          # 提示人工确认门

def test_approve_adds_label(monkeypatch):
    fake = FakeGh()
    monkeypatch.setattr(tl, "gh_json", fake.gh_json)
    assert tl.main(["approve", "42"]) == 0
    # 轮5(近miss B5): 先幂等 ensure_agent_labels(label list),再打标
    assert fake.calls[0][0][:2] == ("label", "list")
    assert any(c[0][:4] == ("issue", "edit", "42", "--repo")
               for c in fake.calls)


# ============ Task 6: 队列对账 reconcile_issues + HUMAN_CLOSED ============

def _labels(*names):
    return [{"name": n} for n in names]

def test_issue_eligible_matrix():
    f = tl.issue_eligible
    assert f(["agent-task", "agent-bug"]) is True
    assert f(["agent-task", "agent-approved"]) is True
    assert f(["agent-task", "agent-bug", "agent-approved"]) is True
    assert f(["agent-task"]) is False                    # 既非 bug 也未确认
    assert f(["agent-task", "agent-feature"]) is False   # feature 未确认
    assert f(["agent-task", "agent-bug", "agent-wip"]) is False
    assert f(["agent-task", "agent-bug", "agent-escalated"]) is False

def test_reconcile_creates_manual_entry_and_respects_state(monkeypatch):
    state = {"entries": [], "auth_failure_streak": 0}
    monkeypatch.setattr(tl, "gh_json", lambda args, input_text=None: [
        {"number": 3, "title": "t", "labels": _labels("agent-task", "agent-feature", "agent-approved")},
        {"number": 5, "title": "t", "labels": _labels("agent-task", "agent-bug")},
    ] if args[0] == "issue" and args[1] == "list" else None)
    tl.reconcile_issues(state, dry_run=False)
    es = tl.StateStore.entries(state)
    assert [(e.issue_number, e.source, e.kind) for e in es] == [(3, "manual", "feature"), (5, "manual", "bug")]
    assert all(e.status == "QUEUED" for e in es)
    # 复跑: state 已跟踪 → 零新建
    tl.reconcile_issues(state, dry_run=False)
    assert len(tl.StateStore.entries(state)) == 2

def test_reconcile_human_closed_sweep(monkeypatch):
    state = {"entries": [dict(
        workflow="", run_id=0, status="QUEUED", issue_number=7, fix_branch="",
        pr_number=None, attempts=0, rounds=0, agent_pid=None,
        agent_started_at=None, agent_log=None, last_dispatched_head=None,
        extra_runs=[], created_at="t", updated_at="t",
        source="manual", kind="bug", audit_hash=None)], "auth_failure_streak": 0}
    def fake_gh(args, input_text=None):
        if args[:2] == ["issue", "list"]:
            return []                       # open 列表已无 #7(被人工关闭)
        if args[:2] == ["issue", "view"]:
            return {"state": "closed"}
        return None
    monkeypatch.setattr(tl, "gh_json", fake_gh)
    tl.reconcile_issues(state, dry_run=False)
    e = tl.StateStore.entries(state)[0]
    assert e.status == "HUMAN_CLOSED" and e.agent_pid is None


def test_reconcile_classifies_source_by_body_marker(monkeypatch):
    """轮5 B46: 入队按 body marker 还原来路——audit/finding marker(引擎
    自己嵌的)→ source=audit/session + audit_hash;无 marker → manual。
    兑现 Entry v2 的四源枚举(此前恒 manual,audit_hash 恒 None 死字段)。"""
    def mk(number, body):
        return {"number": number, "title": "t",
                "labels": [{"name": tl.AGENT_LABEL_TASK},
                           {"name": tl.AGENT_LABEL_BUG}],
                "body": body}
    issues = [
        mk(101, f"审查发现\n{tl.AUDIT_MARKER_FMT.format(hash='aabbccddeeff0011')}"),
        mk(102, f"[review] m\n{tl.FINDING_MARKER_FMT.format(hash='1122334455667788')}"),
        mk(103, "手建 issue,无 marker"),
        mk(104, f"CI 红\n{tl.RUN_MARKER_FMT.format(run_id=40596064101)}"),
    ]
    monkeypatch.setattr(tl, "list_open_agent_issues", lambda: issues)
    state = {"entries": [], "auth_failure_streak": 0}
    tl.reconcile_issues(state, dry_run=False)
    by_no = {e.issue_number: e for e in tl.StateStore.entries(state)}
    assert by_no[101].source == "audit" and \
        by_no[101].audit_hash == "aabbccddeeff0011"
    assert by_no[102].source == "session" and \
        by_no[102].audit_hash == "1122334455667788"
    assert by_no[103].source == "manual" and by_no[103].audit_hash is None
    # 轮6(#52): ci marker → source=ci + run_id 还原(fix_branch 依此回到
    # fix/ci-<runid>,state 丢失重建不再退化为 manual/fix-issue-N)
    assert by_no[104].source == "ci" and by_no[104].run_id == 40596064101
    assert tl._default_fix_branch(by_no[104]) == "fix/ci-40596064101"


def test_reconcile_offlist_view_failure_skips_entry_not_tick(monkeypatch, capsys):
    """轮5(会话#3): 离列表复核的 gh 失败只跳过本条目——不掀翻整个
    reconcile(否则 v1 迁移 ci 条目每 tick 走此路径,一次 5xx/4xx 就跳过
    本 tick 的 heal/派发扫/lane_b,并推迟人工接管的 agent 击杀)。"""
    state = {"entries": [dict(
        workflow="", run_id=0, status="QUEUED", issue_number=7, fix_branch="",
        pr_number=None, attempts=0, rounds=0, agent_pid=None,
        agent_started_at=None, agent_log=None, last_dispatched_head=None,
        extra_runs=[], created_at="t", updated_at="t",
        source="manual", kind="bug", audit_hash=None)], "auth_failure_streak": 0}
    def fake_gh(args, input_text=None):
        if args[:2] == ["issue", "list"]:
            return []
        if args[:2] == ["issue", "view"]:
            # 4xx 确定性错误: gh_json_idem 首次即抛(测试不等退避)
            raise tl.GhError("gh issue view … 失败(exit 1): HTTP 404")
        return None
    monkeypatch.setattr(tl, "gh_json", fake_gh)
    tl.reconcile_issues(state, dry_run=False)      # 不抛 = tick 存活
    e = tl.StateStore.entries(state)[0]
    assert e.status == "QUEUED"      # 条目原样,下 tick 幂等重试
    assert "状态复核失败" in capsys.readouterr().err


# ============ Task 7: 任务派发契约(分支命名/prompt/stdin runner/wip 标签) ============

def _task_entry(**kw):
    base = dict(workflow="", run_id=0, status="QUEUED", issue_number=33,
                fix_branch="", pr_number=None, attempts=0, rounds=0,
                agent_pid=None, agent_started_at=None, agent_log=None,
                last_dispatched_head=None, extra_runs=[], created_at="t",
                updated_at="t", source="session", kind="bug", audit_hash=None)
    base.update(kw)
    return tl.Entry(**base)

def test_default_fix_branch():
    assert tl._default_fix_branch(_task_entry(source="ci", run_id=123,
                                              issue_number=9)) == "fix/ci-123"
    assert tl._default_fix_branch(_task_entry()) == "fix/issue-33"
    assert tl._default_fix_branch(_task_entry(kind="feature")) == "feat/issue-33"


def test_bot_prompt_emits_new_done_marks():
    """轮5(会话#7): 车道 B bot prompt(全源)结束语契约走新常量插值——
    写侧不再逐字吐旧 ci-triage: 标记(读侧双轨保留,覆盖在飞旧 agent),
    过渡窗口有界、旧标记可安全退役。"""
    from types import SimpleNamespace
    pr = SimpleNamespace(number=305, url="https://github.com/x/y/pull/305",
                         head_sha="b" * 40)
    e = _task_entry(fix_branch="fix/issue-33", rounds=1)
    prompt = tl.build_bot_prompt(e, pr, "/tmp/collect-305.md")
    assert tl.TASK_DONE_MARK in prompt
    assert tl.TASK_ESCALATED_MARK in prompt
    assert "ci-triage: done" not in prompt
    assert "ci-triage: escalated" not in prompt

def test_task_prompt_contract():
    p = tl.build_task_prompt(_task_entry())
    assert "issue #33" in p or "#33" in p
    assert "fix/issue-33" in p
    assert "Closes #33" in p                       # PR body 契约
    assert tl.TASK_DONE_MARK in p and tl.TASK_ESCALATED_MARK in p
    assert tl.UNTRUSTED_BANNER in p
    assert "未复现不修" in p                       # bug 红线
    f = tl.build_task_prompt(_task_entry(kind="feature"))
    assert "偏离需求即 escalated" in f             # feature 红线

def test_check_agent_outcome_accepts_both_marks(tmp_path):
    log = tmp_path / "a.log"; log.write_text("x\ntask-loop: done fix/issue-1\n")
    e = _task_entry(status="DISPATCHED", agent_log=str(log))
    assert tl.check_agent_outcome(e) == "success"
    log.write_text("ci-triage: escalated 复现不出\n")
    assert tl.check_agent_outcome(e) == "escalated_reported"

def test_dispatch_writes_prompt_file_and_wip_label(tmp_path, monkeypatch):
    state = {"entries": [], "auth_failure_streak": 0}
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    (tmp_path / "agent.env").write_text("# test env\n")
    monkeypatch.setenv("TASKLOOP_CLONE_DIR", str(tmp_path / "clone"))
    labels = []
    monkeypatch.setattr(tl, "gh_label_add",
                        lambda n, lb: labels.append((n, lb)))
    captured = {}
    def fake_run_background(cmd, cwd, log):
        captured["cmd"] = cmd
        return 4321
    monkeypatch.setattr(tl, "run_background", fake_run_background)
    e = _task_entry()
    pid = tl.dispatch_agent(e, state)
    assert pid == 4321 and e.status == "DISPATCHED" and e.fix_branch == "fix/issue-33"
    assert labels == [(33, "agent-wip")]
    # stdin 化: 命令里不再出现被引用的超长 prompt,而是 < prompt 文件
    assert "< " in captured["cmd"] and "claude -p" in captured["cmd"]
    prompt_files = list(tmp_path.glob("prompt-*"))
    assert len(prompt_files) == 1 and "Closes #33" in prompt_files[0].read_text()


def test_dispatch_refuses_when_agent_env_missing(tmp_path, monkeypatch, capsys):
    """轮5 B44: agent.env 缺失——真实派发拒绝(条目保持 QUEUED、attempts
    不增、无 prompt 落盘、无 wip),dry-run 如实预告该拒绝。"""
    state = {"entries": [], "auth_failure_streak": 0}
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("TASKLOOP_CLONE_DIR", str(tmp_path / "clone"))
    spawned = []
    monkeypatch.setattr(tl, "run_background",
                        lambda *a, **k: spawned.append(a) or 1111)
    labels = []
    monkeypatch.setattr(tl, "gh_label_add",
                        lambda n, lb: labels.append((n, lb)))
    e = _task_entry()          # QUEUED,attempts=0
    assert tl.dispatch_agent(e, state) is None
    assert e.status == "QUEUED" and e.attempts == 0 and e.agent_pid is None
    assert spawned == [] and labels == []
    assert not list(tmp_path.glob("prompt-*"))
    assert "agent.env 缺失" in capsys.readouterr().err
    # dry-run: 警告但不拒绝预演
    assert tl.dispatch_agent(e, state, dry_run=True) is None
    assert "agent.env 缺失" in capsys.readouterr().out
    assert e.status == "QUEUED" and e.attempts == 0


# ============ PR#317 检视#6: 派发记账即时落盘(跨进程崩溃窗口) ============

def test_scan_persists_dispatch_accounting_before_lane_b(tmp_path, monkeypatch):
    """检视#6(Critical): 任务派发扫派成 agent 后,state 须在进入车道 B 之前
    就落盘——原实现唯一落盘点在 finally,其间隔着整条车道 B(ensure_local_review
    的本地 claude 审查可达 CLAUDE_TIMEOUT=3600s);进程一旦死亡,nohup 孤儿
    agent 仍在克隆里干活而磁盘 state 无 PID → 下一 tick 单飞失明 + heal_clone
    reset --hard 踩活 agent 工作区(生产事故 9d6ba452 同类根因)。"""
    from types import SimpleNamespace
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    (tmp_path / "state.json").write_text(json.dumps(
        {"entries": [dict(workflow="", run_id=0, status="QUEUED", issue_number=5,
                          fix_branch="", pr_number=None, attempts=0, rounds=0,
                          agent_pid=None, agent_started_at=None, agent_log=None,
                          last_dispatched_head=None, extra_runs=[],
                          created_at="t", updated_at="t",
                          source="manual", kind="bug", audit_hash=None)],
         "auth_failure_streak": 0}))
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    monkeypatch.setattr(tl, "gh_label_add", lambda n, lb: None)
    monkeypatch.setattr(tl, "reconcile_issues", lambda s, dry_run: None)
    monkeypatch.setattr(tl, "scan_red_runs", lambda: [])
    monkeypatch.setattr(tl, "heal_clone", lambda s, dry_run: None)
    # 车道 B 入口 = 崩溃窗口探针: 此刻磁盘上有什么,进程死亡后下一 tick
    # 就只能看到什么(finally 的 save 在崩溃场景不会执行)。
    disk_at_lane_b = {}
    def fake_lane_b(state, dry_run):
        disk_at_lane_b["state"] = tl.StateStore(str(tmp_path)).load()
    monkeypatch.setattr(tl, "lane_b_advance", fake_lane_b)
    def fake_dispatch(entry, state, dry_run=False, prompt_override=None,
                      increment_rounds=False):
        entry.status = "DISPATCHED"; entry.agent_pid = os.getpid()
        return os.getpid()                      # 确定性真活 PID(探活必真)
    monkeypatch.setattr(tl, "dispatch_agent", fake_dispatch)
    assert tl._run_scan_orchestration(dry_run=False) == 0
    e = disk_at_lane_b["state"]["entries"][0]
    assert e["status"] == "DISPATCHED", "派发状态未在车道 B 前落盘"
    assert e["agent_pid"] == os.getpid(), "agent_pid 未在车道 B 前落盘(单飞失明窗口)"


def test_lane_b_persists_dispatch_accounting_immediately(tmp_path, monkeypatch):
    """检视#6(车道 B 半边): bot 处置 agent 派成后即时落盘——本函数返回后
    编排层还要跑其余条目才到 finally,期间崩溃同样丢 PID。"""
    from types import SimpleNamespace
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    (tmp_path / "agent.env").write_text("# test env\n")
    monkeypatch.setenv("TASKLOOP_REPO", "selftest/local")
    pr = SimpleNamespace(number=88, head_sha="b" * 40, url="https://x/88",
                         bot_done={"local-claude": "t"})
    monkeypatch.setattr(tl.pr_loop, "fetch_pr", lambda slug, num: pr)
    monkeypatch.setattr(tl.pr_loop, "classify_state",
                        lambda p, quiet, now: "READY_TO_FIX")
    monkeypatch.setattr(tl.pr_loop, "render_collect",
                        lambda p, slug, quiet: "# collect 导出(fake)\n")
    monkeypatch.setattr(tl.pr_loop, "ensure_local_review",
                        lambda num, sha: None)
    monkeypatch.setattr(tl, "gh_label_add", lambda n, lb: None)
    monkeypatch.setattr(tl, "run_background",
                        lambda cmd, cwd, log: os.getpid())
    entry = _task_entry(status="PR_OPEN", pr_number=88, rounds=0,
                        last_dispatched_head=None)
    state = {"entries": [entry.to_dict()], "auth_failure_streak": 0}
    tl.lane_b_advance(state, dry_run=False)     # 调用方不 save,直接看磁盘
    e = tl.StateStore(str(tmp_path)).load()["entries"][0]
    assert e["status"] == "BOT_FIX" and e["agent_pid"] == os.getpid(), \
        "车道 B 派发记账未即时落盘"
    assert e["rounds"] == 1 and e["last_dispatched_head"] == "b" * 40


# ============ PR#317 检视#11: paused 横幅与闸门同口径(双路径) ============

def test_scan_paused_banner_honors_legacy_dir(tmp_path, monkeypatch, capsys):
    """检视#11: 过渡期旧目录 PAUSED 同样生效(dispatch_agent 闸门走
    _paused() 新旧双路径,手册 §5.1),scan 编排的全局熔断横幅不得只查新
    目录单路径——否则旧目录 touch PAUSED 时派发全被拒却零横幅,运维遥测
    与实际闸门状态矛盾。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    # 检视#19(轮2)后 _paused() 的 env 守卫: 重定向 run 不查 legacy——
    # 本用例锚"默认目录 run 尊重 legacy 双路径",故把 DEFAULT 也指到
    # tmp(非重定向场景),夹具对齐新契约而非放松断言。
    monkeypatch.setattr(tl, "DEFAULT_STATE_DIR", str(tmp_path))
    legacy = tmp_path / "legacy"; legacy.mkdir()
    (legacy / "PAUSED").touch()
    monkeypatch.setattr(tl, "LEGACY_STATE_DIR", str(legacy))
    (tmp_path / "state.json").write_text(json.dumps(
        {"entries": [dict(workflow="", run_id=0, status="QUEUED", issue_number=5,
                          fix_branch="", pr_number=None, attempts=0, rounds=0,
                          agent_pid=None, agent_started_at=None, agent_log=None,
                          last_dispatched_head=None, extra_runs=[],
                          created_at="t", updated_at="t",
                          source="manual", kind="bug", audit_hash=None)],
         "auth_failure_streak": 0}))
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    monkeypatch.setattr(tl, "reconcile_issues", lambda s, dry_run: None)
    monkeypatch.setattr(tl, "scan_red_runs", lambda: [])
    monkeypatch.setattr(tl, "heal_clone", lambda s, dry_run: None)
    monkeypatch.setattr(tl, "lane_b_advance", lambda s, dry_run: None)
    def fake_dispatch(entry, state, dry_run=False, prompt_override=None,
                      increment_rounds=False):
        # 镜像真实 dispatch_agent 的 PAUSED 闸门契约(闸门本体在 dispatch_agent
        # 内,走 _paused() 双路径);本测试锚的是编排层横幅与闸门口径一致
        if tl._paused():
            return None
        dispatched.append(entry.issue_number)
        return None
    dispatched = []
    monkeypatch.setattr(tl, "dispatch_agent", fake_dispatch)
    assert tl._run_scan_orchestration(dry_run=False) == 0
    assert dispatched == []                        # PAUSED: 零派发
    out = capsys.readouterr().out
    assert "[paused] PAUSED 熔断生效——跳过一切派发" in out, "全局横幅未随旧目录 PAUSED 触发"
    assert "[paused] 车道 A 派发被 PAUSED 闸门拦下" in out


# ============ PR#317 检视#12+#13: 标签协议=派发时控制面 + 清扫去 N+1 ============

def test_scan_dispatch_gate_freezes_ineligible(tmp_path, monkeypatch, capsys):
    """检视#12: 资格判定不能只是入队时刻的一次性闸门——人工对已入队 issue
    补 agent-escalated(冻结)/摘 agent-task(离开 open 列表)后,任务派发扫
    与重派扫都必须复查当前标签;资格正常的照常派发。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    def _e(n, status):
        return dict(workflow="", run_id=0, status=status, issue_number=n,
                    fix_branch="", pr_number=None, attempts=0, rounds=0,
                    agent_pid=None, agent_started_at=None, agent_log=None,
                    last_dispatched_head=None, extra_runs=[],
                    created_at="t", updated_at="t",
                    source="manual", kind="bug", audit_hash=None)
    (tmp_path / "state.json").write_text(json.dumps(
        {"entries": [_e(5, "QUEUED"), _e(6, "QUEUED"), _e(7, "DISPATCHED")],
         "auth_failure_streak": 0}))
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    monkeypatch.setattr(tl, "gh_label_add", lambda n, lb: None)
    # 对账返回当前 open+agent-task 标签快照: #5 人工补 escalated;#6 资格
    # 正常;#7 被人工摘 agent-task(不在 open 列表)——re派扫同受闸。
    monkeypatch.setattr(tl, "reconcile_issues", lambda s, dry_run: {
        5: ["agent-task", "agent-bug", "agent-escalated"],
        6: ["agent-task", "agent-bug"]})
    monkeypatch.setattr(tl, "scan_red_runs", lambda: [])
    monkeypatch.setattr(tl, "heal_clone", lambda s, dry_run: None)
    monkeypatch.setattr(tl, "lane_b_advance", lambda s, dry_run: None)
    dispatched = []
    def fake_dispatch(entry, state, dry_run=False, prompt_override=None,
                      increment_rounds=False):
        dispatched.append(entry.issue_number)
        entry.status = "DISPATCHED"; entry.agent_pid = os.getpid()
        return os.getpid()
    monkeypatch.setattr(tl, "dispatch_agent", fake_dispatch)
    assert tl._run_scan_orchestration(dry_run=False) == 0
    assert dispatched == [6], "冻结条目不得派发,资格正常条目照常派发"
    out = capsys.readouterr().out
    assert "[gate] issue #5" in out and "[gate] issue #7" in out
    saved = tl.StateStore(str(tmp_path)).load()
    by_n = {e["issue_number"]: e for e in saved["entries"]}
    assert by_n[5]["status"] == "QUEUED" and by_n[5]["agent_pid"] is None
    assert by_n[7]["status"] == "DISPATCHED" and by_n[7]["agent_pid"] is None

def test_reconcile_human_closed_sweep_no_nplus1(monkeypatch):
    """检视#13: HUMAN_CLOSED 清扫复用对账拉取的 open 列表——仍在列表的
    QUEUED/DISPATCHED 条目零 gh issue view(旧实现每条每 tick 一次、各带
    45s 超时且全程持 scan.lock);只有离开列表的才复核一次。"""
    def _e(n, status):
        return dict(workflow="", run_id=0, status=status, issue_number=n,
                    fix_branch="", pr_number=None, attempts=0, rounds=0,
                    agent_pid=None, agent_started_at=None, agent_log=None,
                    last_dispatched_head=None, extra_runs=[],
                    created_at="t", updated_at="t",
                    source="manual", kind="bug", audit_hash=None)
    state = {"entries": [_e(3, "QUEUED"), _e(4, "QUEUED"), _e(5, "QUEUED"),
                         _e(9, "DISPATCHED")],
             "auth_failure_streak": 0}
    views = []
    def fake_gh(args, input_text=None):
        if args[:2] == ["issue", "list"]:
            # open+agent-task 列表: #3/#4/#5 仍在;#9 已关闭不在
            return [{"number": n, "title": "t",
                     "labels": [{"name": "agent-task"},
                                {"name": "agent-bug"}]}
                    for n in (3, 4, 5)]
        if args[:2] == ["issue", "view"]:
            views.append(args[2])
            return {"state": "closed"}
        return None
    monkeypatch.setattr(tl, "gh_json", fake_gh)
    open_labels = tl.reconcile_issues(state, dry_run=False)
    assert views == ["9"], "open 列表内的条目零 issue view(去 N+1)"
    by_n = {e.issue_number: e for e in tl.StateStore.entries(state)}
    assert by_n[9].status == "HUMAN_CLOSED"      # 离开列表 → 复核一次 → 接管
    assert all(by_n[n].status == "QUEUED" for n in (3, 4, 5))
    assert open_labels == {3: ["agent-task", "agent-bug"],
                           4: ["agent-task", "agent-bug"],
                           5: ["agent-task", "agent-bug"]}, \
        "对账返回 open 标签快照供派发扫复用(一次 gh 调用,不加 N+1)"


# ============ PR#317 检视#14: dry-run 零本地写(复跑无痕兑现) ============

def test_dry_run_scan_zero_local_writes(tmp_path, monkeypatch):
    """检视#14: dry-run「复跑无痕」= 零本地写——原实现在判定 dry 之前就
    makedirs 状态目录并 O_CREAT scan.lock,与自身 docstring 及手册矛盾,
    且在部署机上提前建目录会掩盖 cron 重定向目录缺失类问题在演练期暴露
    的机会(检视#8 的发现路径被堵)。锁保护的是 state 写,dry-run 不写
    state,无需互斥。"""
    state_dir = tmp_path / "never-created"
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(state_dir))
    monkeypatch.setattr(tl, "LEGACY_STATE_DIR", str(tmp_path / "legacy"))
    monkeypatch.setattr(tl, "reconcile_issues", lambda s, dry_run: None)
    monkeypatch.setattr(tl, "scan_red_runs", lambda: [])
    monkeypatch.setattr(tl, "heal_clone", lambda s, dry_run: None)
    monkeypatch.setattr(tl, "lane_b_advance", lambda s, dry_run: None)
    assert tl._run_scan_orchestration(dry_run=True) == 0
    assert not state_dir.exists(), "dry-run 不得创建状态目录/锁文件"

def test_dry_run_before_migration_previews_from_legacy_state(
        tmp_path, monkeypatch, capsys):
    """轮5(会话#5): dry-run 预演保真——迁移前的机器(新目录无 state.json、
    legacy 有)上,dry-run 只读回退加载 legacy 状态预演,与真实首 tick(先
    迁移再 skip)对齐;不再从空状态预演「将为已跟踪的 run 开 issue」。"""
    new = tmp_path / "new"; legacy = tmp_path / "legacy"
    new.mkdir(); legacy.mkdir()
    entry = dict(workflow="Sanitizer", run_id=40596064101, status="PR_OPEN",
                 issue_number=304, fix_branch="fix/ci-40596064101",
                 pr_number=305, attempts=3, rounds=0, agent_pid=None,
                 agent_started_at=None, agent_log=None,
                 last_dispatched_head=None, extra_runs=[],
                 created_at="t", updated_at="t", source="ci", kind="bug",
                 audit_hash=None)
    (legacy / "state.json").write_text(json.dumps(
        {"entries": [entry], "auth_failure_streak": 0}), encoding="utf-8")
    monkeypatch.setattr(tl, "DEFAULT_STATE_DIR", str(new))
    monkeypatch.setattr(tl, "LEGACY_STATE_DIR", str(legacy))
    monkeypatch.delenv("TASKLOOP_STATE_DIR", raising=False)
    monkeypatch.delenv("CITRIAGE_STATE_DIR", raising=False)
    monkeypatch.setattr(tl, "scan_red_runs", lambda: [
        {"databaseId": 40596064101, "workflowName": "Sanitizer"}])
    monkeypatch.setattr(tl, "reconcile_issues", lambda s, dry_run: {})
    monkeypatch.setattr(tl, "heal_clone", lambda s, dry_run: None)
    lane_b_state = {}
    def spy_lane_b(s, dry_run):
        lane_b_state["entries"] = list(s.get("entries", []))
    monkeypatch.setattr(tl, "lane_b_advance", spy_lane_b)
    assert tl._run_scan_orchestration(dry_run=True) == 0
    out = capsys.readouterr().out
    assert "只读预演 legacy 状态" in out
    assert "[skip] run 40596064101 已在跟踪(state 命中)" in out
    assert "将为 Sanitizer run 40596064101 开 issue" not in out
    # 车道 B 预演看得见 legacy 的 PR_OPEN 条目(真实首 tick 语义)
    assert any(e["run_id"] == 40596064101 and e["status"] == "PR_OPEN"
               for e in lane_b_state.get("entries", []))
    assert not (new / "state.json").exists()   # 零写不变


def test_dry_run_audit_zero_local_writes(tmp_path, monkeypatch):
    """检视#14(audit 半边): run_audit 的 audit.lock/makedirs 同理只在非
    dry 时建立。"""
    state_dir = tmp_path / "never-created"
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(state_dir))
    monkeypatch.setenv("TASKLOOP_CLONE_DIR", str(tmp_path / "clone"))
    monkeypatch.setattr(tl, "LEGACY_STATE_DIR", str(tmp_path / "legacy"))
    monkeypatch.setattr(tl, "_git", lambda args, clone_dir=None:
                        "same" if args[0] in ("rev-parse", "rev-list")
                        else "")
    assert tl.run_audit(dry_run=True) == 0
    assert not state_dir.exists(), "dry-run audit 不得创建状态目录/锁文件"


# ============ Task 8: scan 编排接入 + status ============

def test_scan_dispatches_task_fifo_respecting_single_flight(tmp_path, monkeypatch):
    """两个 QUEUED 任务,单飞 → 只派 FIFO 首个(issue 号小者)。"""
    state = {"entries": [
        dict(workflow="", run_id=0, status="QUEUED", issue_number=9,
             fix_branch="", pr_number=None, attempts=0, rounds=0,
             agent_pid=None, agent_started_at=None, agent_log=None,
             last_dispatched_head=None, extra_runs=[], created_at="t",
             updated_at="t", source="session", kind="bug", audit_hash=None),
        dict(workflow="", run_id=0, status="QUEUED", issue_number=5,
             fix_branch="", pr_number=None, attempts=0, rounds=0,
             agent_pid=None, agent_started_at=None, agent_log=None,
             last_dispatched_head=None, extra_runs=[], created_at="t",
             updated_at="t", source="manual", kind="bug", audit_hash=None)],
        "auth_failure_streak": 0}
    # 编排从 StateStore 装 state(brief 的用例只构造了局部 dict,从未注入)
    # ——先落盘进 tmp;env 重定向让 StateStore/scan.lock/落盘全在 tmp,
    # 绝不碰真实默认目录(夹具真设置,非注释括注)。
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    (tmp_path / "state.json").write_text(json.dumps(state))
    # 引擎缝全部离线(真实 scan 触网): 标签确保(Ruling: 非 dry 编排会真调
    # ensure_agent_labels → 真实 gh label 写,必须打桩)/派发/对账/扫描/
    # 自愈/车道 B。
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    monkeypatch.setattr(tl, "gh_label_add", lambda n, lb: None)
    dispatched = []
    def fake_dispatch(entry, state, dry_run=False, prompt_override=None,
                      increment_rounds=False):
        # Ruling 5: fake 自身镜像真实单飞契约(单飞属 dispatch_agent,不属
        # 编排层),且派成的 agent 用 os.getpid()(确定性真活 PID,探活必真;
        # 伪 PID 如 99 探活不确定,ci_triage 用例 12 同款手法)。
        if tl.has_running_agent(state):
            return None
        dispatched.append(entry.issue_number)
        entry.status = "DISPATCHED"; entry.agent_pid = os.getpid()
        return os.getpid()
    monkeypatch.setattr(tl, "dispatch_agent", fake_dispatch)
    monkeypatch.setattr(tl, "reconcile_issues", lambda s, dry_run: None)
    monkeypatch.setattr(tl, "scan_red_runs", lambda: [])
    monkeypatch.setattr(tl, "heal_clone", lambda s, dry_run: None)
    monkeypatch.setattr(tl, "lane_b_advance", lambda s, dry_run: None)
    rc = tl._run_scan_orchestration(dry_run=False)
    assert rc == 0 and dispatched == [5]        # FIFO issue 号,单飞拦下第二个


def test_cmd_status_prints_queue_and_entries(tmp_path, monkeypatch, capsys):
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    (tmp_path / "state.json").write_text(json.dumps({
        "entries": [dict(workflow="Sanitizer", run_id=1, status="PR_OPEN",
                         issue_number=9, fix_branch="fix/ci-1", pr_number=3,
                         attempts=1, rounds=0, agent_pid=None,
                         agent_started_at=None, agent_log=None,
                         last_dispatched_head=None, extra_runs=[],
                         created_at="t", updated_at="t", source="ci",
                         kind="bug", audit_hash=None)],
        "auth_failure_streak": 0}))
    monkeypatch.setattr(tl, "list_open_agent_issues", lambda: [])
    assert tl.main(["status"]) == 0
    out = capsys.readouterr().out
    assert "PR_OPEN" in out and "#9" in out


# ============ Task 9: audit 无人值守 main 增量对抗审查 ============

def test_audit_hash_stable_and_normalized():
    a = {"path": "a.c", "line": 1, "category": "正确性",
         "message": " x ", "evidence": "e1"}
    b = {"path": "a.c", "line": 1, "category": "正确性",
         "message": "x", "evidence": "DIFFERENT"}     # evidence 不进 hash
    assert tl._audit_hash(a) == tl._audit_hash(b)
    c = dict(a, line=2)
    assert tl._audit_hash(a) != tl._audit_hash(c)

def test_run_audit_dedups_and_advances_cursor(tmp_path, monkeypatch):
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("TASKLOOP_CLONE_DIR", str(tmp_path / "clone"))
    state = {"entries": [], "auth_failure_streak": 0,
             "audit_cursor": {"sha": "base0000000000000000000000000000000000000000",
                              "at": "2026-10-29T00:00:00Z"}}
    store = tl.StateStore(str(tmp_path)); store.save(state)
    git_out = {"fetch": "", "rev-parse": "head1111111111111111111111111111111111111111",
               "log": "abc1234 commit one\ndef5678 commit two"}
    monkeypatch.setattr(tl, "_git", lambda args, clone_dir=None:
                        git_out.get(args[0], ""))
    monkeypatch.setattr(tl, "_git_diff", lambda base, head: "+diff body")
    monkeypatch.setattr(tl.pr_local_review, "_worktree_add", lambda sha: "/tmp/wt")
    monkeypatch.setattr(tl.pr_local_review, "_worktree_remove", lambda p: None)
    monkeypatch.setattr(tl.pr_local_review, "_run_claude",
                        lambda cmd, cwd, prompt: json.dumps([
                            {"path": "a.c", "line": 3, "severity": "Important",
                             "category": "正确性", "message": "m",
                             "evidence": "e"},
                            {"path": "a.c", "line": 3, "severity": "Important",
                             "category": "正确性", "message": "m",
                             "evidence": "e"}]))          # 同 finding ×2 → 去重为 1
    created = []
    monkeypatch.setattr(tl, "create_task_issue",
                        lambda title, body, labels:
                        created.append(body) or 42)
    monkeypatch.setattr(tl, "_search_audit_issue", lambda h: None)
    rc = tl.run_audit(dry_run=False)
    assert rc == 0 and len(created) == 1
    assert tl.AUDIT_MARKER_FMT.format(hash=tl._audit_hash(
        {"path": "a.c", "line": 3, "category": "正确性", "message": "m"})) in created[0]
    s2 = store.load()
    assert s2["audit_cursor"]["sha"].startswith("head1111")   # cursor 推进
    assert len(s2["audit_seen"]) == 1

def test_run_audit_no_new_commits_early_exit(tmp_path, monkeypatch):
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    state = {"entries": [], "auth_failure_streak": 0,
             "audit_cursor": {"sha": "same", "at": "t"}}
    monkeypatch.setattr(tl, "_git", lambda args, clone_dir=None:
                        "same" if args[0] == "rev-parse" else "")
    assert tl.run_audit(dry_run=False) == 0    # 无新提交,零 claude 调用


# ============ PR#317 检视#7: 空输出/schema 漂移 ≠ 零 findings 成功审查 ============

def _audit_env(tmp_path, monkeypatch, claude_output):
    """run_audit 离线环境: git/claude/worktree 缝全 fake,claude 输出可注入。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("TASKLOOP_CLONE_DIR", str(tmp_path / "clone"))
    state = {"entries": [], "auth_failure_streak": 2,
             "audit_cursor": {"sha": "base0000000000000000000000000000000000000000",
                              "at": "2026-10-29T00:00:00Z"}}
    store = tl.StateStore(str(tmp_path)); store.save(state)
    git_out = {"fetch": "", "rev-parse": "head1111111111111111111111111111111111111111",
               "log": "abc1234 commit one"}
    monkeypatch.setattr(tl, "_git", lambda args, clone_dir=None:
                        git_out.get(args[0], ""))
    monkeypatch.setattr(tl, "_git_diff", lambda base, head: "+diff body")
    monkeypatch.setattr(tl.pr_local_review, "_worktree_add", lambda sha: "/tmp/wt")
    monkeypatch.setattr(tl.pr_local_review, "_worktree_remove", lambda p: None)
    monkeypatch.setattr(tl.pr_local_review, "_run_claude",
                        lambda cmd, cwd, prompt: claude_output)
    created = []
    monkeypatch.setattr(tl, "create_task_issue",
                        lambda title, body, labels: created.append(body) or 42)
    monkeypatch.setattr(tl, "_search_audit_issue", lambda h: None)
    return store, created

def test_audit_empty_output_does_not_advance_cursor(tmp_path, monkeypatch):
    """检视#7: claude 空输出被当「零 findings 成功」→ cursor 永久推进 +
    streak 被空跑清零。修复后: 空输出 = 审查失败(rc 1),cursor 不动、
    streak 不清零、零 issue,下轮重审同区间。"""
    store, created = _audit_env(tmp_path, monkeypatch, claude_output="")
    assert tl.run_audit(dry_run=False) == 1
    s2 = store.load()
    assert s2["audit_cursor"]["sha"].startswith("base0000")   # cursor 未推进
    assert s2["auth_failure_streak"] == 2                     # 未被空跑清零
    assert created == [] and s2.get("audit_seen") is None     # 零入库

def test_audit_all_malformed_output_does_not_advance_cursor(tmp_path, monkeypatch):
    """检视#7(schema 漂移半边): 输出是 JSON 数组但每条都缺 message——
    _parse_findings 逐条静默跳过后同样不得被当零 findings 成功。"""
    store, created = _audit_env(
        tmp_path, monkeypatch,
        claude_output=json.dumps([{"summary": "x"}, {"note": "y"}]))
    assert tl.run_audit(dry_run=False) == 1
    s2 = store.load()
    assert s2["audit_cursor"]["sha"].startswith("base0000")
    assert s2["auth_failure_streak"] == 2
    assert created == []

def test_audit_genuine_zero_findings_advances_cursor(tmp_path, monkeypatch):
    """检视#7 反向锚: 真·零 findings(claude 输出空 JSON 数组)仍是成功
    审查——cursor 推进、streak 归零不被误伤。"""
    store, created = _audit_env(tmp_path, monkeypatch, claude_output="[]")
    assert tl.run_audit(dry_run=False) == 0
    s2 = store.load()
    assert s2["audit_cursor"]["sha"].startswith("head1111")
    assert s2["auth_failure_streak"] == 0
    assert created == []


# ============ Task 9 fix round 1: Ruling 11 跨锁覆写 ============

def test_audit_does_not_clobber_concurrent_scan_state(tmp_path, monkeypatch):
    """Ruling 11: audit 不得覆写并发 scan 写入的 entries——state 写入只合
    并 audit 自有键(audit_cursor/audit_seen)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("TASKLOOP_CLONE_DIR", str(tmp_path / "clone"))
    state = {"entries": [], "auth_failure_streak": 0,
             "audit_cursor": {"sha": "base0000000000000000000000000000000000000000",
                              "at": "2026-10-29T00:00:00Z"}}
    store = tl.StateStore(str(tmp_path)); store.save(state)
    # scan 在 audit load 之后写入的推进(派发记账: run 1 → DISPATCHED + 活 PID)
    scan_entry = dict(workflow="Sanitizer", run_id=1, status="DISPATCHED",
                      issue_number=9, fix_branch="fix/ci-1", pr_number=None,
                      attempts=1, rounds=0, agent_pid=123,
                      agent_started_at=None, agent_log=None,
                      last_dispatched_head=None, extra_runs=[],
                      created_at="t", updated_at="t", source="ci",
                      kind="bug", audit_hash=None)
    git_out = {"fetch": "", "rev-parse": "head1111111111111111111111111111111111111111",
               "log": "abc1234 commit one"}
    monkeypatch.setattr(tl, "_git", lambda args, clone_dir=None:
                        git_out.get(args[0], ""))
    monkeypatch.setattr(tl, "_git_diff", lambda base, head: "+diff body")
    monkeypatch.setattr(tl.pr_local_review, "_worktree_add", lambda sha: "/tmp/wt")
    monkeypatch.setattr(tl.pr_local_review, "_worktree_remove", lambda p: None)
    def fake_run_claude(cmd, cwd, prompt):
        # claude 运行(分钟级)期间,并发 scan tick(持 scan.lock)落盘了
        # 派发记账——audit 收尾若整份覆写启动时快照,这条 entries 会丢
        disk = tl.StateStore(str(tmp_path)).load()
        disk["entries"] = [scan_entry]
        tl.StateStore(str(tmp_path)).save(disk)
        return json.dumps([
            {"path": "a.c", "line": 3, "severity": "Important",
             "category": "正确性", "message": "m", "evidence": "e"}])
    monkeypatch.setattr(tl.pr_local_review, "_run_claude", fake_run_claude)
    created = []
    monkeypatch.setattr(tl, "create_task_issue",
                        lambda title, body, labels:
                        created.append(body) or 42)
    monkeypatch.setattr(tl, "_search_audit_issue", lambda h: None)
    rc = tl.run_audit(dry_run=False)
    assert rc == 0 and len(created) == 1
    s2 = store.load()
    # 并发 scan 的派发记账幸存(entries 不被 audit 启动时的空快照回滚)
    assert any(e["run_id"] == 1 and e["status"] == "DISPATCHED"
               and e["agent_pid"] == 123 for e in s2["entries"])
    # audit 自有键照常推进
    assert s2["audit_cursor"]["sha"].startswith("head1111")
    assert len(s2["audit_seen"]) == 1


def _audit_chunk_fakes(monkeypatch, tmp_path, per_commit_lines):
    """轮5 B48 测试基建: N 条 commit(base..head),第 i 条自身 diff 行数
    per_commit_lines[i](numstat 口径);diff 按行数合成。返回 (head_sha,
    审查过的区间记录, 创建的 issue 数)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("TASKLOOP_CLONE_DIR", str(tmp_path / "clone"))
    shas = [f"{i:040x}" for i in range(1, len(per_commit_lines) + 1)]
    base = "0" * 40          # 字典序先于全部 shas(测试构造,自增尾数)
    head = shas[-1]
    numstat = []
    for sha, n in zip(reversed(shas), reversed(per_commit_lines)):
        numstat.append(sha)      # git log 新→旧序输出
        numstat.append(f"{n}\t0\tfile.c")
    reviewed = {}
    def fake_git(args, clone_dir=None):
        if args[0] == "fetch":
            return ""
        if args[0] == "rev-parse":
            return head
        if args[0] == "merge-base":
            return base
        if args[:2] == ["log", "--numstat"]:
            return "\n".join(numstat)
        if args[:2] == ["log", "--oneline"]:
            return "\n".join(f"{s[:7]} msg" for s in shas)
        return ""
    monkeypatch.setattr(tl, "_git", fake_git)
    def fake_diff(b, h):
        n = sum(per_commit_lines[i] for i, s in enumerate(shas)
                if b < s <= h)          # sha 字典序 = 提交序(测试构造)
        reviewed[(b, h)] = n
        return "\n".join("+" for _ in range(n))
    monkeypatch.setattr(tl, "_git_diff", fake_diff)
    monkeypatch.setattr(tl.pr_local_review, "_worktree_add", lambda sha: "/tmp/wt")
    monkeypatch.setattr(tl.pr_local_review, "_worktree_remove", lambda p: None)
    monkeypatch.setattr(tl.pr_local_review, "_run_claude",
                        lambda cmd, cwd, prompt: "[]")   # 零 findings 成功
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    return base, head, reviewed


def test_audit_chunk_head_picks_oldest_side_under_cap(monkeypatch, tmp_path):
    """轮5 B48: 块基选择——最旧侧累积到即将超限为止,返回块内最新
    commit;单 commit 超限仍整块纳入(不卡死)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    shas = [f"{i:040x}" for i in range(1, 5)]
    numstat = []
    for sha, n in zip(reversed(shas), [800, 1200, 1500, 1000]):
        numstat += [sha, f"{n}\t0\tf.c"]   # git log 新→旧序输出
    monkeypatch.setattr(tl, "_git", lambda args, clone_dir=None:
                        "\n".join(numstat)
                        if args[:2] == ["log", "--numstat"] else "")
    # cap=3000: 最旧两条 1000+1500=2500 ✓,再加 1200 超 → 块基 = shas[1]
    assert tl._audit_chunk_head("b" * 40, shas[-1]) == shas[1]
    # 单 commit 超限: 仍纳入该条(至少一条)
    monkeypatch.setattr(tl, "AUDIT_MAX_DIFF_LINES", 500)
    assert tl._audit_chunk_head("b" * 40, shas[-1]) == shas[0]


def test_audit_oversized_range_reviews_chunk_and_cursor_advances_to_it(
        tmp_path, monkeypatch, capsys):
    """轮5 B48: 超限区间只审最旧块,cursor 推进到块基(不是 head)——
    余量下一轮续审,超大区间不再每轮 strict 失败死循环烧配额。"""
    per_commit = [1000, 1500, 1200, 800]    # 总 4500 > cap 3000
    base, head, reviewed = _audit_chunk_fakes(monkeypatch, tmp_path, per_commit)
    monkeypatch.setattr(tl, "AUDIT_MAX_DIFF_LINES", 3000)
    state = {"entries": [], "auth_failure_streak": 0,
             "audit_cursor": {"sha": base, "at": "t"}}
    tl.StateStore(str(tmp_path)).save(state)
    rc = tl.run_audit(dry_run=False)
    assert rc == 0
    # 全量探测(4500 行)后切块,实际只审 base..shas[1](最旧两条,2500 ≤ 3000)
    chunk = [(b, h) for (b, h), n in reviewed.items() if h != head]
    assert len(chunk) == 1 and chunk[0][0] == base
    assert reviewed[chunk[0]] == 2500
    s2 = tl.StateStore(str(tmp_path)).load()
    assert s2["audit_cursor"]["sha"] != head and \
        s2["audit_cursor"]["sha"].endswith("2")   # 推进到块基 shas[1]
    assert "切块审查" in capsys.readouterr().out
    # 下一轮: cursor=块基,续审余量(1200+800=2000 ≤ cap)→ cursor 到 head
    reviewed.clear()
    rc2 = tl.run_audit(dry_run=False)
    assert rc2 == 0
    chunk2 = [(b, h) for (b, h), n in reviewed.items()]
    assert len(chunk2) == 1 and reviewed[chunk2[0]] == 2000
    assert chunk2[0][1] == head
    assert tl.StateStore(str(tmp_path)).load()["audit_cursor"]["sha"] == head


def test_audit_success_streak_reset_respects_timeline(tmp_path, monkeypatch):
    """轮5(会话#8): audit 成功归零带时间线守卫——审查完成时间早于磁盘上
    最后一次认证失败(auth_failure_at)时**不**归零(陈旧成功不抹掉并发
    scan 在审查期间记入的真实失败);不早于则归零(裁定 10 防累计仍成立)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("TASKLOOP_CLONE_DIR", str(tmp_path / "clone"))
    base = "base0000000000000000000000000000000000000000"
    head = "head1111111111111111111111111111111111111111"
    git_out = {"fetch": "", "rev-parse": head, "log": "abc1234 commit one"}
    monkeypatch.setattr(tl, "_git", lambda args, clone_dir=None:
                        git_out.get(args[0], ""))
    monkeypatch.setattr(tl, "_git_diff", lambda b, h: "+diff")
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    monkeypatch.setattr(tl.pr_local_review, "_worktree_add", lambda sha: "/tmp/wt")
    monkeypatch.setattr(tl.pr_local_review, "_worktree_remove", lambda p: None)
    monkeypatch.setattr(tl.pr_local_review, "_run_claude",
                        lambda cmd, cwd, prompt: "[]")
    # ① 陈旧成功: 最后一次失败在审查完成之后(模拟审查运行期间凭据失效、
    #    并发 scan 已锁内记入) → 不归零
    st = {"entries": [], "auth_failure_streak": 2,
          "auth_failure_at": "2999-01-01T00:00:00Z",
          "audit_cursor": {"sha": base, "at": "t"}}
    tl.StateStore(str(tmp_path)).save(st)
    assert tl.run_audit(dry_run=False) == 0
    assert tl.StateStore(str(tmp_path)).load()["auth_failure_streak"] == 2
    # ② 新鲜成功: 最后一次失败在远古 → 归零(cursor 重置回 base 再跑)
    st2 = tl.StateStore(str(tmp_path)).load()
    st2["auth_failure_streak"] = 2
    st2["auth_failure_at"] = "2000-01-01T00:00:00Z"
    st2["audit_cursor"] = {"sha": base, "at": "t"}
    tl.StateStore(str(tmp_path)).save(st2)
    assert tl.run_audit(dry_run=False) == 0
    assert tl.StateStore(str(tmp_path)).load()["auth_failure_streak"] == 0
    # ③ 增量路径记时间戳(_audit_streak_add 与 scan 侧 reap 同款判据)
    tl._audit_streak_add(1)
    s3 = tl.StateStore(str(tmp_path)).load()
    assert s3["auth_failure_streak"] == 1 and s3.get("auth_failure_at")


def test_audit_auth_fail_increments_fresh_disk_streak(tmp_path, monkeypatch):
    """Ruling 11: streak 增量对锁内 fresh 磁盘值做,不吃启动时快照。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    monkeypatch.setenv("TASKLOOP_CLONE_DIR", str(tmp_path / "clone"))
    state = {"entries": [], "auth_failure_streak": 0,
             "audit_cursor": {"sha": "base0000000000000000000000000000000000000000",
                              "at": "2026-10-29T00:00:00Z"}}
    store = tl.StateStore(str(tmp_path)); store.save(state)
    git_out = {"fetch": "", "rev-parse": "head1111111111111111111111111111111111111111",
               "log": "abc1234 commit one"}
    monkeypatch.setattr(tl, "_git", lambda args, clone_dir=None:
                        git_out.get(args[0], ""))
    monkeypatch.setattr(tl, "_git_diff", lambda base, head: "+diff body")
    monkeypatch.setattr(tl.pr_local_review, "_worktree_add", lambda sha: "/tmp/wt")
    monkeypatch.setattr(tl.pr_local_review, "_worktree_remove", lambda p: None)
    def fake_run_claude_auth_fail(cmd, cwd, prompt):
        # claude 运行期间并发 scan 把磁盘 streak 推到 2(如另车道 auth_fail
        # 计数)——audit 的 +1 必须对这个 fresh 值做,而非启动时快照的 0
        disk = tl.StateStore(str(tmp_path)).load()
        disk["auth_failure_streak"] = 2
        tl.StateStore(str(tmp_path)).save(disk)
        raise tl.GhError("claude 检视失败(exit 1): API Error 403")
    monkeypatch.setattr(tl.pr_local_review, "_run_claude",
                        fake_run_claude_auth_fail)
    # 子阈值分支: limit 抬到 5 使 3 < 5(实际常量 3 的边界行为见下半段)
    monkeypatch.setattr(tl, "AUTH_FAILURE_STREAK_LIMIT", 5)
    assert tl.run_audit(dry_run=False) == 1
    s2 = store.load()
    assert s2["auth_failure_streak"] == 3      # fresh 2+1,而非快照 0+1
    assert not (tmp_path / "PAUSED").exists()  # 3 < 5: 未达熔断
    # 真实常量边界: limit=3 时同样的 fresh 2+1=3 >= 3 → PAUSED 熔断落地
    monkeypatch.setattr(tl, "AUTH_FAILURE_STREAK_LIMIT", 3)
    assert tl.run_audit(dry_run=False) == 1
    s3 = store.load()
    assert s3["auth_failure_streak"] == 3      # 再次 fresh 2+1
    assert (tmp_path / "PAUSED").exists()


# ============ 终审修复: HUMAN_CLOSED 是终态(同根因收敛不得误判活跃) ============

def test_human_closed_ci_entry_not_active_for_convergence(tmp_path, monkeypatch):
    """终审 Important#1: HUMAN_CLOSED 是终态——同 workflow 新红 run 不得收敛
    进它(不在已关闭 issue 上评论、吞失败),而应建新 issue 开新条目。"""
    state = {"entries": [dict(
        workflow="Sanitizer", run_id=1, status="HUMAN_CLOSED", issue_number=7,
        fix_branch="", pr_number=None, attempts=0, rounds=0, agent_pid=None,
        agent_started_at=None, agent_log=None, last_dispatched_head=None,
        extra_runs=[], created_at="t", updated_at="t", source="ci",
        kind="bug", audit_hash=None)], "auth_failure_streak": 0}
    created, comments = [], []
    monkeypatch.setattr(tl, "create_issue", lambda run: created.append(run) or 9)
    monkeypatch.setattr(tl, "comment_on_issue",
                        lambda n, body: comments.append((n, body)))
    monkeypatch.setattr(tl, "find_issue_by_run", lambda run_id: None)
    monkeypatch.setattr(tl, "find_closed_marker_issue", lambda run_id: None)
    run2 = {"databaseId": 2, "workflowName": "Sanitizer", "event": "schedule",
            "conclusion": "failure", "headBranch": "main", "headSha": "x",
            "createdAt": "2026-10-29T00:00:00Z", "url": "u"}
    tl.triage_new_red_runs(state, [run2], dry_run=False)
    assert len(created) == 1            # 新红 run 开新 issue(未被死条目吞掉)
    assert comments == []               # 零评论——不在已关闭 issue 上碰
    assert state["entries"][0]["extra_runs"] == []   # 死条目未被挂 extra_runs
    assert state["entries"][1]["run_id"] == 2 and state["entries"][1]["status"] == "QUEUED"


def test_paused_env_redirect_ignores_legacy_dir(tmp_path, monkeypatch):
    """PR#317 检视#19(轮2): env 重定向的 run 不检查真实 legacy 目录——与
    _migrate_legacy_state_dir 的 env 守卫同口径,多实例互不误拦。"""
    legacy = tmp_path / "legacy"; legacy.mkdir()
    (legacy / "PAUSED").touch()
    monkeypatch.setattr(tl, "DEFAULT_STATE_DIR", str(tmp_path / "inst-a"))
    monkeypatch.setattr(tl, "LEGACY_STATE_DIR", str(legacy))
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "inst-b"))
    assert tl._paused() is False          # 重定向实例: legacy PAUSED 不拦
    monkeypatch.delenv("TASKLOOP_STATE_DIR", raising=False)
    monkeypatch.delenv("CITRIAGE_STATE_DIR", raising=False)
    assert tl._paused() is True           # 默认目录 run: legacy 仍生效(过渡期)


def test_auth_fail_pattern_no_false_positive_on_bare_403():
    """PR#317 检视#23(轮2): 认证判定收紧——裸 '403'(行号/URL 片段)不再
    误判入熔断 streak;API Error/Not logged in/HTTP 403|401 照常命中。
    轮5 B43 追加: API Error 须邻接 40x 状态码——瞬时 5xx/超时形
    'API Error (Request timed out)' 不再计入熔断。"""
    assert tl.AUTH_FAIL_PATTERN.search("API Error: 403 Not logged in")
    assert tl.AUTH_FAIL_PATTERN.search("API Error 401")
    assert tl.AUTH_FAIL_PATTERN.search("HTTP 403 Forbidden")
    assert tl.AUTH_FAIL_PATTERN.search("HTTP 401")
    assert not tl.AUTH_FAIL_PATTERN.search("  at foo.c:403 in bar()")
    assert not tl.AUTH_FAIL_PATTERN.search("see line 403 of the config")
    assert not tl.AUTH_FAIL_PATTERN.search("API Error (Request timed out)")
    assert not tl.AUTH_FAIL_PATTERN.search('API Error: 500 {"type":"error"}')


def test_check_agent_outcome_auth_fail_requires_dead_and_tail(tmp_path):
    """轮5 B43: auth_fail 双条件——活 PID 复述认证字样=running(不熔断);
    死 PID + 认证字样在转写中部(非临终窗)=dead;死 PID + 临终窗命中=
    auth_fail;超窗活 PID + 临终窗命中=timeout(击杀归 attempts 路径)。"""
    from datetime import datetime, timezone, timedelta
    now = datetime.now(timezone.utc)
    dead_pid, live_pid = 999999998, os.getpid()

    def _entry(pid, started, text):
        p = tmp_path / f"log-{abs(hash(text)) % 99999}.log"
        p.write_text(text, encoding="utf-8")
        return tl.Entry(workflow="W", run_id=1, status="DISPATCHED",
                        issue_number=9, fix_branch="fix/ci-1", pr_number=None,
                        attempts=0, rounds=0, agent_pid=pid,
                        agent_started_at=started.strftime(
                            "%Y-%m-%dT%H:%M:%SZ"),
                        agent_log=str(p), last_dispatched_head=None,
                        extra_runs=[], created_at="t", updated_at="t",
                        source="ci", kind="bug", audit_hash=None)

    # ① 活 agent 尾部复述 CI 日志认证字样 → running(核心防误熔断)
    assert tl.check_agent_outcome(_entry(
        live_pid, now, "复述 CI 日志: API Error: 403 Forbidden\n")) == "running"
    # ② 死 PID + 认证字样被 600 字符后续输出推出临终窗 → dead
    assert tl.check_agent_outcome(_entry(
        dead_pid, now, "API Error: 403 Forbidden\n" + "x" * 600)) == "dead"
    # ③ 死 PID + 临终窗命中 → auth_fail
    assert tl.check_agent_outcome(_entry(
        dead_pid, now, "工作被中断\nAPI Error: 403 Forbidden\n")) == "auth_fail"
    # ④ 超窗活 PID + 临终窗命中 → timeout(auth_fail 让位于死条件)
    assert tl.check_agent_outcome(_entry(
        live_pid, now - timedelta(seconds=tl.AGENT_TIMEOUT_SEC + 60),
        "API Error: 403 Forbidden\n")) == "timeout"


def test_approve_ensures_labels_before_add(monkeypatch):
    """轮5(近miss B5): approve 先幂等确保 agent-* 标签在位——全新部署
    (任何 scan tick 未跑过)时 agent-approved 不存在,裸 gh issue edit
    会 label-not-found 裸 traceback。"""
    calls = []
    monkeypatch.setattr(tl, "ensure_agent_labels",
                        lambda: calls.append("ensure"))
    monkeypatch.setattr(tl, "gh_label_add",
                        lambda n, lb: calls.append((n, lb)))
    assert tl.main(["approve", "77"]) == 0
    assert calls == ["ensure", (77, "agent-approved")]


def test_agent_env_path_dead_constant_removed():
    """PR#317 检视#22(轮2): AGENT_ENV_PATH 导入期快照死代码已删——防止
    重新引入第二个可分叉真相源(build_claude_command 用 agent_env_path())。"""
    import pr_local_review as plr
    assert not hasattr(plr, "AGENT_ENV_PATH")


# ===== PR#317 轮 3 检视修复 =====

def test_ensure_agent_labels_uses_json_flag(monkeypatch):
    """检视#25(轮3,Critical): gh label list 必须带 --json——缺省输出
    三列文本表格,gh_json 原样返回 str,逐字符迭代 l["name"] 抛
    TypeError 且编排层不捕,首个非 dry scan tick 必崩(真机实证)。"""
    calls = []
    def fake_gh_json(args, input_text=None):
        calls.append(tuple(args))
        if args[0] == "label" and args[1] == "list":
            return [{"name": n} for n in
                    ("agent-task", "agent-bug", "agent-feature",
                     "agent-approved", "agent-wip", "agent-escalated")]
        return None
    monkeypatch.setattr(tl, "gh_json", fake_gh_json)
    tl.ensure_agent_labels()
    label_list = [c for c in calls if c[:2] == ("label", "list")]
    assert len(label_list) == 1
    assert "--json" in label_list[0] and "name" in label_list[0]
    assert not any(c[:2] == ("label", "create") for c in calls)


def test_auth_fail_below_threshold_removes_wip_label(monkeypatch, tmp_path):
    """检视#26(轮3,Critical): auth_fail 未达熔断阈值的重派等待路径必须
    摘 agent-wip——残留 wip 让下一 tick 的派发闸把 DISPATCHED+无 PID
    条目判'不具资格'永久冻结,一次凭据失效即死锁。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    removed = []
    monkeypatch.setattr(tl, "gh_label_remove",
                        lambda n, lb: removed.append((n, lb)))
    monkeypatch.setattr(tl, "check_agent_outcome", lambda e: "auth_fail")
    monkeypatch.setattr(tl, "_kill_agent_if_alive",
                        lambda e: setattr(e, "agent_pid", None))
    state = {"entries": [dict(
        workflow="Sanitizer", run_id=7, status="DISPATCHED", issue_number=42,
        fix_branch="fix/ci-7", pr_number=None, attempts=1, rounds=0,
        agent_pid=123, agent_started_at=None, agent_log=None,
        last_dispatched_head=None, extra_runs=[], created_at="t",
        updated_at="t", source="ci", kind="bug", audit_hash=None)],
        "auth_failure_streak": 0}
    tl.reap_or_advance(state, dry_run=False)
    assert state["auth_failure_streak"] == 1           # 未达 3,不熔断
    assert removed == [(42, "agent-wip")]              # 关键: wip 已摘
    e = tl.StateStore.entries(state)[0]
    assert e.status == "DISPATCHED" and e.agent_pid is None


def _entry(**kw):
    base = dict(workflow="", run_id=0, status="QUEUED", issue_number=9,
                fix_branch="", pr_number=None, attempts=0, rounds=0,
                agent_pid=None, agent_started_at=None, agent_log=None,
                last_dispatched_head=None, extra_runs=[], created_at="t",
                updated_at="t", source="manual", kind="bug", audit_hash=None)
    base.update(kw)
    return tl.Entry(**base)


def test_dispatch_gate_ci_source_exempt_from_membership_freeze():
    """检视#27(轮3): v1 迁移的 ci 条目 issue 无 agent-task 标签(旧
    create_issue 不带 --label)永不在 open_labels 快照——ci 源豁免成员
    资格冻结(冻结面=关闭 issue/PAUSED);task 源维持冻结语义。"""
    ci = _entry(source="ci", status="DISPATCHED", issue_number=9)
    manual = _entry(source="manual", issue_number=8)
    open_labels = {}                       # 两 issue 均不在快照
    assert tl._dispatch_gate_ok(ci, open_labels) is True
    assert tl._dispatch_gate_ok(manual, open_labels) is False


def test_kill_agent_killpg_for_leader_single_kill_fallback(monkeypatch):
    """检视#28(轮3): 组长形态(setsid 派发)整组击杀 bash→timeout→claude;
    非组长(历史形态/直起进程)回退单杀——防 killpg 误伤调用方自己的组。"""
    killed = []
    monkeypatch.setattr(tl.os, "getpgid", lambda pid: pid)     # 组长
    monkeypatch.setattr(tl.os, "killpg",
                        lambda pgid, sig: killed.append(("pg", pgid)))
    monkeypatch.setattr(tl.os, "kill",
                        lambda pid, sig: killed.append(("pid", pid)))
    e = _entry(status="DISPATCHED", agent_pid=777)
    monkeypatch.setattr(tl, "agent_alive", lambda pid: True)
    tl._kill_agent_if_alive(e)
    assert killed == [("pg", 777)] and e.agent_pid is None
    killed.clear()
    e.agent_pid = 778
    monkeypatch.setattr(tl.os, "getpgid", lambda pid: 4321)    # 非组长
    tl._kill_agent_if_alive(e)
    assert killed == [("pid", 778)] and e.agent_pid is None


def test_dispatch_command_uses_setsid(tmp_path, monkeypatch):
    """检视#28(轮3): 派发命令内层 bash 经 setsid 成为会话/组长——$!
    记录的 PID 即 pgid,killpg 可整树击杀(claude 不再孤儿化)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    (tmp_path / "agent.env").write_text("# test env\n")
    monkeypatch.setenv("TASKLOOP_CLONE_DIR", str(tmp_path / "clone"))
    monkeypatch.setattr(tl, "gh_label_add", lambda n, lb: None)
    captured = {}
    monkeypatch.setattr(tl, "run_background",
                        lambda cmd, cwd, log: captured.update(cmd=cmd) or 99)
    tl.dispatch_agent(_entry(), {"entries": [], "auth_failure_streak": 0})
    assert "setsid" in captured["cmd"] and "claude -p" in captured["cmd"]


def test_audit_dead_base_revision_self_heals(tmp_path, monkeypatch, capsys):
    """检视#29(轮3): cursor 的 base 因 prune/改写不在 origin/main 历史
    → merge-base 失败时回退 24h 窗口重derive,不崩溃循环;git 层其它
    RuntimeError 优雅 return 1 不裸 traceback。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    store = tl.StateStore(str(tmp_path))
    store.save({"entries": [], "auth_failure_streak": 0,
                "audit_cursor": {"sha": "deadbeef" + "0" * 33, "at": "t"}})
    git_map = {"rev-parse": "head1111111111111111111111111111111111111111",
               "rev-list": "base9990000000000000000000000000000000000000",
               "log": "abc1234 x", "diff": "+d"}
    def fake_git(args, clone_dir=None):
        if args[0] == "merge-base":
            raise RuntimeError("git merge-base … 失败(exit 128): fatal")
        return git_map.get(args[0], "")
    monkeypatch.setattr(tl, "_git", fake_git)
    monkeypatch.setattr(tl, "_git_diff", lambda b, h: "+diff")
    assert tl.run_audit(dry_run=True) == 0
    out = capsys.readouterr().out
    assert "回退 24h 窗口" in out and "base999000"[:8] in out
    # 其它 git 失败(fetch 阶段)→ 优雅退出
    def boom(args, clone_dir=None):
        raise RuntimeError("git fetch … 失败(exit 128)")
    monkeypatch.setattr(tl, "_git", boom)
    assert tl.run_audit(dry_run=True) == 1


# ===== PR#317 轮 4 检视修复 =====

def test_timeout_outcome_kills_live_agent_before_requeue(monkeypatch, tmp_path):
    """检视#33(轮4): timeout 判档先于探活,PID 可能仍活——重派路径清引用
    前必须击杀(孤儿化+单飞失明+heal 踩活工作区);dead 结局击杀幂等无害。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    killed = []
    monkeypatch.setattr(tl, "check_agent_outcome", lambda e: "timeout")
    monkeypatch.setattr(tl, "_kill_agent_if_alive",
                        lambda e: (killed.append(e.agent_pid),
                                   setattr(e, "agent_pid", None)))
    state = {"entries": [dict(
        workflow="Sanitizer", run_id=3, status="DISPATCHED", issue_number=11,
        fix_branch="fix/ci-3", pr_number=None, attempts=1, rounds=0,
        agent_pid=555, agent_started_at=None, agent_log=None,
        last_dispatched_head=None, extra_runs=[], created_at="t",
        updated_at="t", source="ci", kind="bug", audit_hash=None)],
        "auth_failure_streak": 0}
    tl.reap_or_advance(state, dry_run=False)
    assert killed == [555]                    # timeout 活 PID 被击杀
    e = tl.StateStore.entries(state)[0]
    assert e.attempts == 2 and e.agent_pid is None   # 走重派记账


def test_audit_search_finding_issue_checks_closed_too(monkeypatch):
    """检视#35(轮4): audit 幂等查重双态搜索——创建轮中途崩溃+人工驳回
    关闭的组合窗口下,不得重建人工已驳回的 issue。"""
    states_queried = []
    def fake_gh(args, input_text=None):
        if tuple(args[:2]) == ("search", "issues"):
            states_queried.append(args[args.index("--state") + 1])
            if args[args.index("--state") + 1] == "closed":
                return [{"number": 77, "body": "x "
                         + tl.AUDIT_MARKER_FMT.format(hash="ab" * 8) + " y"}]
            return []
        return None
    monkeypatch.setattr(tl, "gh_json", fake_gh)
    assert tl._search_audit_issue("ab" * 8) == 77
    assert states_queried == ["open", "closed"]     # 双态都查了


def test_audit_non_dry_ensures_labels(monkeypatch, tmp_path):
    """检视#36(轮4): audit 单独存活的部署(scan 停用)下,建 finding issue
    前须幂等确保 agent-* 标签在位——否则每个 create 都 GhError,cursor
    永不推进,覆盖面静默归零。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path))
    called = []
    monkeypatch.setattr(tl, "ensure_agent_labels",
                        lambda: called.append(1))
    monkeypatch.setattr(tl, "_with_scan_lock", lambda fn: called.append("m"))
    monkeypatch.setattr(tl, "_paused", lambda: True)   # PAUSED 早退前已确保
    assert tl.run_audit(dry_run=False) == 0
    assert called == ["m", 1]                # 迁移后、PAUSED 闸前已确保标签
