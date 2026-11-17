"""task_loop_pipeline 测试: 槽位常量 / 窗口判定 / 内存闸 / boot_id /
槽位账本(租约/janitor/启动对账)/ 槽位化派发 + worktree 隔离(Task 4)。"""
import sys, os, re, shutil, subprocess
import fcntl
import pytest
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "..", "scripts"))
import task_loop_pipeline as tp
import task_loop as tl
from types import SimpleNamespace as NS
from datetime import datetime, timedelta, timezone


def test_slot_kinds_max():
    assert tp.SLOT_KINDS == {"issue_fix": 2, "pr_review": 2,
                             "pr_repair": 2, "audit": 1}


def test_in_window_cross_midnight():
    assert tp.in_window(datetime(2026, 11, 12, 23, 0))
    assert tp.in_window(datetime(2026, 11, 13, 3, 0))
    assert tp.in_window(datetime(2026, 11, 13, 8, 59))
    assert not tp.in_window(datetime(2026, 11, 13, 9, 0))
    assert not tp.in_window(datetime(2026, 11, 12, 12, 0))


def test_min_mem_gb_env(monkeypatch):
    monkeypatch.setenv("PIPELINE_MIN_MEM_GB", "12")
    assert tp.min_mem_gb() == 12
    monkeypatch.delenv("PIPELINE_MIN_MEM_GB")
    assert tp.min_mem_gb() == 8


def test_mem_available_reads_proc_meminfo(tmp_path, monkeypatch):
    fake = tmp_path / "meminfo"
    fake.write_text("MemTotal:      29000000 kB\nMemAvailable:   9000000 kB\n")
    monkeypatch.setattr(tp, "MEMINFO_PATH", str(fake))
    monkeypatch.setattr(tp, "_cgroup_mem_limit_mb", lambda: None)
    # MemAvailable 按整 MB 取整: 9000000 kB // 1024 = 8789 MB
    assert tp.mem_available_mb() == 8789


def test_mem_gate_ok_threshold(monkeypatch):
    monkeypatch.setattr(tp, "mem_available_mb", lambda: 4096)
    assert tp.mem_gate_ok() is False
    monkeypatch.setattr(tp, "mem_available_mb", lambda: 8192)
    assert tp.mem_gate_ok() is True


def test_boot_id_shape():
    # 真实 /proc 可读；形如 36 字符 hex-uuid
    v = tp.boot_id()
    assert len(v) == 36 and "-" in v


def test_entry_new_fields_roundtrip_and_legacy():
    from task_loop import Entry
    base = dict(workflow="Sanitizer", run_id=1, status="DONE", issue_number=2,
                fix_branch="fix/issue-2", pr_number=3, attempts=1, rounds=0,
                agent_pid=None, agent_started_at=None, agent_log=None,
                last_dispatched_head=None, extra_runs=[], created_at="t",
                updated_at="t", source="audit", kind="bug", audit_hash=None)
    e = Entry(**base, lane="issue_fix", worktree="/w", merged_at="m",
              merged_sha="ab", conflict_nights=2,
              last_conflict_night="2026-11-13")
    d = e.to_dict()
    assert d["lane"] == "issue_fix" and d["merged_sha"] == "ab"
    assert d["conflict_nights"] == 2          # I6 第 23 字段: 连续冲突夜计数
    assert d["last_conflict_night"] == "2026-11-13"   # I6 fix-2 第 24 字段: 夜锚
    # 旧 state(无 4 新键)加载容错: lane="" 其余 None
    legacy = Entry.from_dict(base)
    assert legacy.lane == "" and legacy.worktree is None
    assert legacy.merged_at is None and legacy.merged_sha is None
    assert legacy.conflict_nights == 0        # I6 缺省容错
    assert legacy.last_conflict_night is None # I6 fix-2 缺省容错
    # 往返
    assert Entry.from_dict(d).to_dict() == d


# ============ 槽位账本(Task 3) ============


def _mkstate():
    return {"entries": [], "auth_failure_streak": 0}


def test_slot_acquire_respects_kind_max_and_dup_key(monkeypatch):
    # 探活 monkeypatch 钉死: 低号 PID 在真机是 root 内核线程(ksoftirqd/
    # kworker),os.kill → EPERM → agent_alive 判活,宿主相关必假活。
    monkeypatch.setattr(tl, "agent_alive", lambda pid: True)
    st = _mkstate()
    assert tp.slot_acquire(st, "issue_fix", "issue-1", 101, 100, "/l", "/w",
                           "t", "b") is True
    assert tp.slot_acquire(st, "issue_fix", "issue-1", 102, 100, "/l", "/w",
                           "t", "b") is False          # 同 key 互斥
    assert tp.slot_acquire(st, "issue_fix", "issue-2", 103, 100, "/l", "/w",
                           "t", "b") is True
    assert tp.slot_acquire(st, "issue_fix", "issue-3", 104, 100, "/l", "/w",
                           "t", "b") is False          # 超 2 槽


def test_slot_release_and_free_count(monkeypatch):
    monkeypatch.setattr(tl, "agent_alive", lambda pid: False)
    st = _mkstate()
    tp.slot_acquire(st, "pr_review", "pr-9", 201, 200, "/l", None, "t", "b")
    assert tp.free_slot_count(st, "pr_review") == 2   # 死 PID → janitor 清 → 空槽
    tp.slot_release(st, "pr_review", "pr-9")
    assert tp.slot_get(st, "pr_review", "pr-9") is None


def test_slot_janitor_marks_environmental(monkeypatch):
    monkeypatch.setattr(tl, "agent_alive", lambda pid: False)
    st = _mkstate()
    tp.slot_acquire(st, "issue_fix", "issue-1", 999999, 0, "/l", "/w",
                    "t", "old-boot")
    monkeypatch.setattr(tp, "boot_id", lambda: "new-boot")
    gone = tp.slot_janitor(st, "now")
    assert len(gone) == 1 and gone[0]["environmental"] is True


def test_startup_reconcile_requeues_without_attempt(monkeypatch):
    st = _mkstate()
    st["entries"] = [dict(workflow="w", run_id=1, status="DISPATCHED",
                          issue_number=5, fix_branch="fix/issue-5",
                          pr_number=None, attempts=3, rounds=0, agent_pid=111,
                          agent_started_at="t", agent_log="/l",
                          last_dispatched_head="abc", extra_runs=[],
                          created_at="t", updated_at="t", source="audit",
                          kind="bug", audit_hash=None)]
    tp.slot_acquire(st, "issue_fix", "issue-5", 111, 0, "/l", "/w",
                    "t", "old-boot")
    monkeypatch.setattr(tp, "boot_id", lambda: "new-boot")
    monkeypatch.setattr(tl, "agent_alive", lambda pid: False)
    tp.startup_reconcile(st)
    e = tl.StateStore.entries(st)[0]
    assert e.status == "QUEUED" and e.agent_pid is None
    assert e.attempts == 3          # 未 +1: 环境性死亡豁免
    assert tp.slot_get(st, "issue_fix", "issue-5") is None


# ============ T3 遗留三臂补测(控制器裁定#3,同 commit) ============


def _entry(**kw):
    d = dict(workflow="w", run_id=1, status="DISPATCHED", issue_number=5,
             fix_branch="fix/issue-5", pr_number=None, attempts=2, rounds=0,
             agent_pid=111, agent_started_at="t", agent_log="/l",
             last_dispatched_head=None, extra_runs=[], created_at="t",
             updated_at="t", source="audit", kind="bug", audit_hash=None)
    d.update(kw)
    return d


def test_startup_reconcile_bot_fix_to_pr_open(monkeypatch):
    """臂①: boot 变化的死槽撞 BOT_FIX 条目(issue 键)——status 归
    PR_OPEN,attempts 不动。"""
    monkeypatch.setattr(tl, "agent_alive", lambda pid: False)
    st = _mkstate()
    st["entries"] = [_entry(status="BOT_FIX", pr_number=88)]
    tp.slot_acquire(st, "issue_fix", "issue-5", 111, 0, "/l", "/w",
                    "t", "old-boot")
    monkeypatch.setattr(tp, "boot_id", lambda: "new-boot")
    tp.startup_reconcile(st)
    e = tl.StateStore.entries(st)[0]
    assert e.status == "PR_OPEN" and e.attempts == 2
    assert tp.slot_get(st, "issue_fix", "issue-5") is None


def test_startup_reconcile_matches_pr_key(monkeypatch):
    """臂②: 槽 key = f"pr-{pr_number}"(车道 B 派发形态)同样命中条目
    (非 BOT_FIX → QUEUED)。"""
    monkeypatch.setattr(tl, "agent_alive", lambda pid: False)
    st = _mkstate()
    st["entries"] = [_entry(pr_number=88)]
    tp.slot_acquire(st, "pr_repair", "pr-88", 222, 0, "/l", "/w",
                    "t", "old-boot")
    monkeypatch.setattr(tp, "boot_id", lambda: "new-boot")
    tp.startup_reconcile(st)
    e = tl.StateStore.entries(st)[0]
    assert e.status == "QUEUED" and e.agent_pid is None
    assert tp.slot_get(st, "pr_repair", "pr-88") is None


def test_startup_reconcile_nonenvironmental_frees_slot_only(monkeypatch):
    """臂③: 非 environmental 死槽(boot 未变)只清槽,条目不动——判档仍归
    tl.reap_or_advance 正常记账。"""
    monkeypatch.setattr(tl, "agent_alive", lambda pid: False)
    st = _mkstate()
    st["entries"] = [_entry()]
    tp.slot_acquire(st, "issue_fix", "issue-5", 111, 0, "/l", "/w",
                    "t", tp.boot_id())          # 当前 boot: 非 environmental
    tp.startup_reconcile(st)
    assert tp.slot_get(st, "issue_fix", "issue-5") is None
    e = tl.StateStore.entries(st)[0]
    assert e.status == "DISPATCHED" and e.agent_pid == 111
    assert e.attempts == 2


# ============ Task 4: 槽位化派发 + worktree 隔离 ============


def _patch_dispatch_offline(monkeypatch, tmp_path):
    """dispatch_agent 离线夹具: state/clone 目录重定向 + 凭据 + 引擎缝全打桩。
    探活钉死 True(裁定#2): 槽位一旦占用保持占用;裸真探活不确定,且本机低号
    PID 是内核线程(EPERM 判活)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    monkeypatch.setenv("TASKLOOP_CLONE_DIR", str(tmp_path / "cl"))
    os.makedirs(tmp_path / "st")
    (tmp_path / "st" / "agent.env").write_text("PATH=/usr/bin\n")
    monkeypatch.setattr(tl, "agent_alive", lambda pid: True)
    monkeypatch.setattr(tl, "_worktree_prepare",
                        lambda clone_dir, key: f"/wt/{key}")
    monkeypatch.setattr(tl, "gh_label_add", lambda *a: None)
    monkeypatch.setattr(tp, "mem_gate_ok", lambda: True)


def test_dispatch_two_issue_agents_then_full(monkeypatch, tmp_path):
    """issue 泳道 2 槽: 第 3 个 QUEUED 条目被拒;内存不足也被拒。"""
    _patch_dispatch_offline(monkeypatch, tmp_path)
    # 桩 PID 固定小整数(裁定#2): brief 原文的 time 派生 PID 可能撞本机
    # 真实活 PID(低号多为内核线程),探活不确定会让账本时占时空。
    pids = iter((4242, 4243, 4244))
    monkeypatch.setattr(tl, "run_background",
                        lambda cmd, cwd, log: next(pids))

    def mk(n):
        return tl.Entry(workflow="w", run_id=n, status="QUEUED", issue_number=n,
                        fix_branch="", pr_number=None, attempts=0, rounds=0,
                        agent_pid=None, agent_started_at=None, agent_log=None,
                        last_dispatched_head=None, extra_runs=[], created_at="t",
                        updated_at="t", source="audit", kind="bug",
                        audit_hash=None, lane="", worktree=None,
                        merged_at=None, merged_sha=None)

    st = {"entries": [mk(1).to_dict(), mk(2).to_dict(), mk(3).to_dict()],
          "auth_failure_streak": 0}
    es = tl.StateStore.entries(st)
    for e in es:
        tl.dispatch_agent(e, st)
    assert sum(e.status == "DISPATCHED" for e in es) == 2   # 2 槽用满
    assert tp.free_slot_count(st, "issue_fix") == 0
    assert tp.SLOT_KINDS["issue_fix"] == 2
    # 派发即记账: lane/worktree 落条目,worktree 进槽位账本
    assert es[0].lane == "issue_fix" and es[0].worktree == "/wt/issue-1"
    assert tp.slot_get(st, "issue_fix", "issue-1")["worktree"] == "/wt/issue-1"
    # 内存闸拒绝(第 3 条目在满槽闸已被拒、零记账,无需重置字段——控制器
    # 裁定#3: 原稿两行赋值是无效死行,已删)
    monkeypatch.setattr(tp, "mem_gate_ok", lambda: False)
    assert tl.dispatch_agent(es[2], st) is None


def test_dispatch_pr_repair_uses_own_slots(monkeypatch, tmp_path):
    """pr_repair 与 issue_fix 互不占位: issue 2 槽用满后,pr 泳道照常派,
    且 rounds 记账在 pr 泳道(lane B 契约)。"""
    _patch_dispatch_offline(monkeypatch, tmp_path)
    pids = iter((4242, 4243, 4244))
    monkeypatch.setattr(tl, "run_background",
                        lambda cmd, cwd, log: next(pids))

    def mk(n, pr=None):
        return tl.Entry(workflow="w", run_id=n, status="QUEUED", issue_number=n,
                        fix_branch="", pr_number=pr, attempts=0, rounds=0,
                        agent_pid=None, agent_started_at=None, agent_log=None,
                        last_dispatched_head=None, extra_runs=[], created_at="t",
                        updated_at="t", source="audit", kind="bug",
                        audit_hash=None)

    st = {"entries": [mk(1).to_dict(), mk(2).to_dict(),
                      mk(3, pr=88).to_dict()],
          "auth_failure_streak": 0}
    es = tl.StateStore.entries(st)
    tl.dispatch_agent(es[0], st)
    tl.dispatch_agent(es[1], st)
    assert tp.free_slot_count(st, "issue_fix") == 0
    assert tl.dispatch_agent(es[2], st, increment_rounds=True) == 4244
    assert es[2].status == "BOT_FIX" and es[2].rounds == 1
    assert es[2].lane == "pr_repair" and es[2].worktree == "/wt/pr-88"
    assert tp.free_slot_count(st, "pr_repair") == 1
    assert tp.free_slot_count(st, "issue_fix") == 0   # pr 占用不影响 issue


def test_dispatch_same_key_second_refused_zero_spawn(monkeypatch, tmp_path):
    """同 key 二次派发: Task 5(控制器裁定#2)起前置早退——槽位账本已有
    同 key 记录(不判活)即直接拒派,零 spawn、零 killpg,条目记账保持
    首派(不重置 PID/状态/attempts)。旧「spawn 后 acquire 拒再 killpg
    击杀」的过渡期 churn 已消;slot_acquire 的击杀网仅剩并发竞态兜底。"""
    _patch_dispatch_offline(monkeypatch, tmp_path)
    pids = iter((4242, 4243))
    spawned = []
    monkeypatch.setattr(tl, "run_background",
                        lambda cmd, cwd, log: spawned.append(1) or next(pids))
    killed = []
    monkeypatch.setattr(os, "killpg",
                        lambda pid, sig: killed.append((pid, sig)))
    st = {"entries": [], "auth_failure_streak": 0}
    e = tl.StateStore.entries({"entries": [_entry()], "auth_failure_streak": 0})[0]
    assert tl.dispatch_agent(e, st) == 4242
    assert tl.dispatch_agent(e, st) is None        # 同 key issue-5 → 早退
    assert spawned == [1]                          # 第二次零 spawn
    assert killed == []                            # 无 killpg(不再 spawn-then-kill)
    assert e.agent_pid == 4242 and e.status == "DISPATCHED"
    assert e.attempts == 3 and e.lane == "issue_fix"
    assert len(st["slots"]["issue_fix"]) == 1


def test_dispatch_worktree_prepare_failure_refuses(monkeypatch, tmp_path):
    """worktree 准备失败(git 瞬时故障: fetch 网络抖动等)→ 拒派不炸 tick:
    返回 None、条目原状零记账、零 spawn——与 heal_clone 同款「本条拒绝、
    下轮重试」错误策略。"""
    _patch_dispatch_offline(monkeypatch, tmp_path)
    def boom(clone_dir, key):
        raise RuntimeError("git fetch origin --prune… 失败(exit 128): 网络抖动")
    monkeypatch.setattr(tl, "_worktree_prepare", boom)
    spawned = []
    monkeypatch.setattr(tl, "run_background",
                        lambda cmd, cwd, log: spawned.append(1) or 4242)
    e = tl.StateStore.entries({"entries": [_entry()],
                               "auth_failure_streak": 0})[0]
    assert tl.dispatch_agent(e, {"entries": [], "auth_failure_streak": 0}) is None
    assert e.status == "DISPATCHED" and e.attempts == 2
    assert spawned == []


def test_worktree_prepare_cleanup_roundtrip(tmp_path):
    """真实 git 集成: prepare 建于 origin/main、可重复 prepare(清残留);
    rmtree 兜底删目录后留 .git/worktrees 注册——cleanup 不 prune 该残留时
    同 key 重建必炸(exit 128「丢失但已注册」,2026-11-12 /tmp 实测)。"""
    bare = tmp_path / "origin.git"
    subprocess.run(["git", "init", "--bare", "-q", "-b", "main", str(bare)],
                   check=True)
    seed = tmp_path / "seed"
    subprocess.run(["git", "init", "-q", "-b", "main", str(seed)], check=True)
    (seed / "f.txt").write_text("x")
    subprocess.run(["git", "-C", str(seed), "add", "."], check=True)
    subprocess.run(["git", "-C", str(seed), "-c", "user.email=t@t",
                    "-c", "user.name=t", "commit", "-qm", "init"], check=True)
    subprocess.run(["git", "-C", str(seed), "push", "-q", str(bare), "main:main"],
                   check=True)
    clone = tmp_path / "clone"
    subprocess.run(["git", "clone", "-q", str(bare), str(clone)], check=True)
    wt = tl._worktree_prepare(str(clone), "issue-1")
    assert os.path.isfile(os.path.join(wt, "f.txt"))
    # 覆盖既有 worktree(重派残留): prepare 幂等
    assert tl._worktree_prepare(str(clone), "issue-1") == wt
    # rmtree 兜底现场(目录没了、注册残留): prepare 仍须能重建
    shutil.rmtree(wt)
    wt = tl._worktree_prepare(str(clone), "issue-1")
    assert os.path.isdir(wt)
    tl.worktree_cleanup(wt)
    assert not os.path.exists(wt)
    assert not os.listdir(os.path.join(clone, ".worktrees"))
    # 注册已 prune: 同 key 可再建
    assert os.path.isdir(tl._worktree_prepare(str(clone), "issue-1"))


def test_worktree_cleanup_locked_missing_zombie(tmp_path):
    """真实 git 集成(#811): killpg 击杀在途 agent 留下「丢失但锁定」
    (missing-but-locked) 僵尸——locked 的 remove --force 必拒、prune 永不
    清 locked,注册永久残留、同 key 重加炸派发(git 2.43 /tmp 实测 fatal
    'is a missing but locked worktree');cleanup 须 unlock(容错)后再清。
    与检视#8 的 no_dangling(健康树)互补: 本测试锁定的正是 unlock 前置
    ——删掉它本测试即红,健康树测试不红。"""
    repo = tmp_path / "repo"
    subprocess.run(["git", "init", "-q", "-b", "main", str(repo)], check=True)
    (repo / "f.txt").write_text("x")
    subprocess.run(["git", "-C", str(repo), "add", "."], check=True)
    subprocess.run(["git", "-C", str(repo), "-c", "user.email=t@t",
                    "-c", "user.name=t", "commit", "-qm", "init"], check=True)
    wt = os.path.join(str(repo), ".worktrees", "issue-811")
    subprocess.run(["git", "-C", str(repo), "worktree", "add", "--detach",
                    wt, "HEAD"], check=True)
    # 伪造软停现场的两半: 目录丢失 + admin 标记锁定(不可清除态)
    subprocess.run(["git", "-C", str(repo), "worktree", "lock", wt], check=True)
    shutil.rmtree(wt)
    # 修复前: 2.54 生产挂满 135s 超时 / 2.43 注册永久残留——须秒清不抛
    tl.worktree_cleanup(wt)
    # 注册清除(git 2.43 实测: 修复前 prune 跳过 locked,注册仍在)
    assert not os.path.exists(os.path.join(str(repo), ".git", "worktrees"))
    # 同 key 重加成功(修复前 fatal 'is a missing but locked worktree')
    subprocess.run(["git", "-C", str(repo), "worktree", "add", "--detach",
                    wt, "HEAD"], check=True)
    # 未锁容错: 健康路径 unlock exit 非零被忽略,cleanup 照常清干净
    tl.worktree_cleanup(wt)
    assert not os.path.exists(wt)
    assert not os.path.exists(os.path.join(str(repo), ".git", "worktrees"))


# ============ Task 5: 动态轮次 + reap 释放槽位 + 同 key 占位早退 ============


def test_dynamic_rounds_constants():
    """动态轮次常量锚(spec 决策表 #10,取代固定 MAX_ROUNDS=4):
    docs-only/简单 diff → 2,其余 → 3;简单 diff 门槛 100 行 ∧ 5 文件。"""
    assert tl.MAX_ROUNDS_SIMPLE == 2
    assert tl.MAX_ROUNDS_COMPLEX == 3
    assert tl.ROUND_SIMPLE_MAX_DIFF_LINES == 100
    assert tl.ROUND_SIMPLE_MAX_FILES == 5


def test_max_rounds_for_matrix(monkeypatch):
    from types import SimpleNamespace as NS
    monkeypatch.setattr(tl, "pr_files", lambda n: ["docs/a.md"])
    assert tl.max_rounds_for(None, NS(pr_number=1, additions=5, deletions=5)) == 2
    # Minor B(fix-round 1): docs 臂优先于复杂度——docs-only 即使增删
    # 250 行(≥100)仍按 docs-only 上限 2
    assert tl.max_rounds_for(
        None, NS(pr_number=1, additions=200, deletions=50)) == 2
    monkeypatch.setattr(tl, "pr_files",
                        lambda n: ["a.c", "b.c", "c.c", "d.c", "e.c"])
    assert tl.max_rounds_for(
        None, NS(pr_number=2, additions=60, deletions=30)) == 2   # ≤100 行 ∧ ≤5 文件
    monkeypatch.setattr(tl, "pr_files", lambda n: ["a.c"] * 6)
    assert tl.max_rounds_for(
        None, NS(pr_number=3, additions=60, deletions=30)) == 3   # 6 文件 → 复杂
    assert tl.max_rounds_for(
        None, NS(pr_number=4, additions=200, deletions=50)) == 3
    # files 显式给出时不现拉(pr_files 不被调用也能判)
    monkeypatch.setattr(tl, "pr_files", lambda n: (_ for _ in ()).throw(
        AssertionError("files 已显式给出,不应再拉")))
    assert tl.max_rounds_for(
        None, NS(pr_number=6, additions=5, deletions=5),
        files=["docs/x.md"]) == 2


def test_max_rounds_for_additions_deletions_gh_fallback(monkeypatch):
    """Important#1(fix-round 1): PRData 无 additions/deletions 字段(现行
    pr_loop.PRData 即此形态,开工前已 grep 核实)→ 经 gh pr view --json
    additions,deletions 兜底(brief Consumes 明文)——不兜底则简单 diff 臂
    生产不可达。兜底拉取失败(GhError)按复杂=3(宁严勿松)。"""
    from types import SimpleNamespace as NS
    monkeypatch.setattr(tl, "pr_files", lambda n: ["a.c"])
    # 兜底拉到 40+30=70 ≤100 行 ∧ 1 文件 → 简单=2(gh_json_idem 经 gh_json
    # 的 json.loads 成 dict——桩镜像该契约,非裸 JSON 字符串)
    monkeypatch.setattr(tl, "gh_json_idem",
                        lambda args, input_text=None:
                        {"additions": 40, "deletions": 30})
    assert tl.max_rounds_for(None, NS(pr_number=7)) == 2
    # 兜底拉取失败 → 复杂=3
    def boom(args, input_text=None):
        raise tl.GhError("gh pr view … 失败(exit 1): HTTP 404")
    monkeypatch.setattr(tl, "gh_json_idem", boom)
    assert tl.max_rounds_for(None, NS(pr_number=8)) == 3
    # gh 空输出(gh_json 退出码 0 无输出 → None)→ or {} 防空,0+0 ≤100
    # ∧ 1 文件 → 简单=2(空 PR 无增删,0 行本就是最小简单 diff)
    monkeypatch.setattr(tl, "gh_json_idem", lambda args, input_text=None: None)
    assert tl.max_rounds_for(None, NS(pr_number=9)) == 2
    # PR 号双形解析: 真实 PRData 字段名是 number(无 pr_number)——审查者
    # 片段的 pr.pr_number 对它 AttributeError,离线 lane_b 用例当场逮到
    monkeypatch.setattr(tl, "gh_json_idem",
                        lambda args, input_text=None:
                        {"additions": 40, "deletions": 30})
    assert tl.max_rounds_for(None, NS(number=12)) == 2


def test_max_rounds_for_empty_files_strict_complex(monkeypatch):
    """Minor A(fix-round 1): files 拉取失败(空清单)即使增删字段齐全也按
    复杂=3——空表会冒充「≤5 文件」误走简单臂,与「宁严勿松」相悖。"""
    from types import SimpleNamespace as NS
    assert tl.max_rounds_for(
        None, NS(pr_number=10, additions=60, deletions=30), files=[]) == 3
    monkeypatch.setattr(tl, "pr_files", lambda n: [])
    assert tl.max_rounds_for(
        None, NS(pr_number=11, additions=60, deletions=30)) == 3


def test_pr_files_parses_lines_and_degrades_on_gherror(monkeypatch):
    """pr_files: gh_json_idem 输出按行拆(去空行);GhError → [](按复杂
    处理的诚实降级,绝不炸车道 B);gh 空输出(gh_json 退出码 0 无输出
    → None,空 diff PR 的 .files[].path 即此形态)→ [](Important#2,
    fix-round 1——旧实现 out.splitlines() 裸崩 AttributeError)。"""
    monkeypatch.setattr(tl, "gh_json_idem",
                        lambda args, input_text=None:
                        "docs/a.md\nsrc/b.c\n\n")
    assert tl.pr_files(88) == ["docs/a.md", "src/b.c"]

    def boom(args, input_text=None):
        raise tl.GhError("gh pr view … 失败(exit 1): HTTP 404")
    monkeypatch.setattr(tl, "gh_json_idem", boom)
    assert tl.pr_files(88) == []

    # Important#2: gh 空输出 → gh_json 返回 None → pr_files 不得裸崩
    monkeypatch.setattr(tl, "gh_json_idem", lambda args, input_text=None: None)
    assert tl.pr_files(88) == []


def test_docs_only_requires_all_md_and_nonempty():
    assert tl._docs_only(["a.md", "docs/b.md"]) is True
    assert tl._docs_only([]) is False            # 拉不到清单不冒充 docs-only
    assert tl._docs_only(["a.md", "b.c"]) is False


def test_reap_releases_issue_slot_on_exit(monkeypatch):
    """控制器裁定#1(车道 A 臂): agent 退场判档后、归还/重派之前释放
    issue_fix 槽位 + entry.worktree 清引用;attempts 记账语义不动。"""
    monkeypatch.setattr(tl, "check_agent_outcome", lambda e: "dead")
    monkeypatch.setattr(tl, "gh_label_remove", lambda n, lb: None)
    st = _mkstate()
    st["entries"] = [_entry(attempts=0, agent_pid=999999901,   # 必死伪 PID
                            worktree="/wt/issue-5")]
    tp.slot_acquire(st, "issue_fix", "issue-5", 999999901, 0, "/l",
                    "/wt/issue-5", "t", tp.boot_id())
    tl.reap_or_advance(st, dry_run=False)
    e = tl.StateStore.entries(st)[0]
    assert e.attempts == 1 and e.agent_pid is None   # dead → +1 重派语义不动
    assert e.worktree is None                        # 条目侧清引用
    assert tp.slot_get(st, "issue_fix", "issue-5") is None   # 槽已释放
    # 幂等: 无槽 state(过渡期 Task 4 前条目)reap 零炸、照常记账
    st2 = _mkstate()
    st2["entries"] = [_entry(attempts=0, agent_pid=999999901)]
    tl.reap_or_advance(st2, dry_run=False)
    assert tl.StateStore.entries(st2)[0].attempts == 1


def test_reap_releases_pr_repair_slot_on_bot_fix_exit(monkeypatch):
    """控制器裁定#1(车道 B 臂): BOT_FIX agent auth_fail 退场(未达熔断
    阈值)→ 归还 PR_OPEN 的同时释放 pr_repair 槽位;rounds 记账不动。"""
    monkeypatch.setattr(tl, "check_agent_outcome", lambda e: "auth_fail")
    st = _mkstate()
    st["entries"] = [_entry(status="BOT_FIX", pr_number=88, rounds=2,
                            agent_pid=999999902, worktree="/wt/pr-88")]
    tp.slot_acquire(st, "pr_repair", "pr-88", 999999902, 0, "/l",
                    "/wt/pr-88", "t", tp.boot_id())
    tl.reap_or_advance(st, dry_run=False)
    e = tl.StateStore.entries(st)[0]
    assert e.status == "PR_OPEN" and e.rounds == 2   # 车道 B 记账语义不动
    assert e.agent_pid is None and e.worktree is None
    assert tp.slot_get(st, "pr_repair", "pr-88") is None


def test_dispatch_same_key_dead_slot_zero_spawn(monkeypatch, tmp_path,
                                                capsys):
    """控制器裁定#2 配套: 死槽占 key(未清死槽,过渡期形态)→ dispatch
    前置早退返回 None、零 spawn(消 spawn-then-killpg churn);早退按 key
    精确命中,不拦其他条目。"""
    _patch_dispatch_offline(monkeypatch, tmp_path)
    monkeypatch.setattr(tl, "agent_alive", lambda pid: False)  # 槽内 PID 已死
    spawned = []
    monkeypatch.setattr(tl, "run_background",
                        lambda cmd, cwd, log: spawned.append(1) or 4242)
    st = _mkstate()
    tp.slot_acquire(st, "issue_fix", "issue-5", 999999901, 0, "/l",
                    "/wt/issue-5", "t", tp.boot_id())
    e = tl.StateStore.entries({"entries": [_entry()], "auth_failure_streak": 0})[0]
    assert tl.dispatch_agent(e, st) is None          # 死槽占 key → 早退
    assert spawned == []                            # 零 spawn
    assert "已占用" in capsys.readouterr().out
    assert e.attempts == 2 and e.status == "DISPATCHED"   # 零记账
    # key 精确: 其他条目(issue-6)不受死槽影响,照常派发
    e6 = tl.StateStore.entries({"entries": [_entry(issue_number=6)],
                                "auth_failure_streak": 0})[0]
    assert tl.dispatch_agent(e6, st) == 4242
    assert spawned == [1]


# ============ Task 6: 泳道2 检视派发器 + --pr 单 PR CLI ============


def _patch_review_offline(monkeypatch, tmp_path, alive=True):
    """dispatch_review 离线夹具(控制器裁定 R3): 探活一律 monkeypatch
    tl.agent_alive,勿裸真 PID——本机低号 PID 是内核线程,agent_alive 对
    EPERM 判活,宿主相关必假活。state 目录重定向(命令串/log 纯字符串
    构造,零真实落盘);run_background 桩返回固定小整数 PID 序列(4242…)。
    T10 顺手修①起 dispatch_review 有 agent.env 存在性检查(B44 同款),
    夹具须就位真凭据文件(缺文件路径的用例自行 unlink)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    os.makedirs(tmp_path / "st")
    (tmp_path / "st" / "agent.env").write_text("PATH=/usr/bin\n")
    monkeypatch.setattr(tl, "agent_alive", lambda pid: alive)
    monkeypatch.setattr(tp, "mem_gate_ok", lambda: True)
    monkeypatch.setattr(tp, "boot_id", lambda: "b")
    pids = iter((4242, 4243, 4244, 4245))
    monkeypatch.setattr(tl, "run_background",
                        lambda cmd, cwd, log: next(pids))


def test_dispatch_review_per_night_cap_with_self_release(monkeypatch, tmp_path,
                                                          capsys):
    """R3 死槽自释放语义: agent 全死(探活恒 False)→ 同 (pr,head) 重试先
    释放自身 key 死槽,第 1、2 次可派(tries=1,2);第 3 次撞每晚上限
    REVIEW_PER_HEAD_PER_NIGHT=2 拒,且拒绝不增计数。内存闸拒(T10 顺手修③
    补 [mem-gate] 打印): 闸在计数/槽位之前,零 spawn、零记账。"""
    _patch_review_offline(monkeypatch, tmp_path, alive=False)
    st = _mkstate()
    assert tp.dispatch_review(st, 640, "h1") == 4242        # tries=1
    assert st["review_tries"]["640:h1"] == 1                 # 键 f"{pr}:{head}"
    assert tp.dispatch_review(st, 640, "h1") == 4243        # 死槽自释放, tries=2
    assert st["review_tries"]["640:h1"] == 2
    assert tp.dispatch_review(st, 640, "h1") is None        # ≥2 → 每晚拒
    assert st["review_tries"]["640:h1"] == 2                # 拒绝不增计数
    # 内存闸拒: 闸在计数/槽位之前,零 spawn、零记账
    monkeypatch.setattr(tp, "mem_gate_ok", lambda: False)
    assert tp.dispatch_review(st, 640, "h9") is None
    assert "640:h9" not in st["review_tries"]
    assert "[mem-gate]" in capsys.readouterr().out          # ③ 静默路补打印


def test_dispatch_review_live_same_key_and_full_slots(monkeypatch, tmp_path):
    """agent 全活(T10 顺手修②重写): 同 key 活占位 → **前置早退**(先于每晚
    上限计数),零 spawn 零 killpg、不烧预算——旧「spawn 后 acquire 拒再
    killpg」churn 已消(击杀网仅剩 acquire 竞态兜底,见
    test_dispatch_review_acquire_fail_killpg_fallback);两槽活满后第 3 个
    不同 PR 满槽拒(③ 补 [slots] 打印,零 spawn、零计数)。"""
    _patch_review_offline(monkeypatch, tmp_path, alive=True)
    spawned = []
    killed = []
    monkeypatch.setattr(tl, "run_background",
                        lambda cmd, cwd, log: spawned.append(1) or
                        (4242, 4243, 4244, 4245)[len(spawned) - 1])
    monkeypatch.setattr(os, "killpg",
                        lambda pid, sig: killed.append((pid, sig)))
    st = _mkstate()
    assert tp.dispatch_review(st, 640, "h1") == 4242
    assert tp.dispatch_review(st, 640, "h1") is None        # 同 key 活占位早退
    assert spawned == [1]                                  # 第二次零 spawn
    assert killed == []                                    # 零 killpg(不再 churn)
    assert st["review_tries"]["640:h1"] == 1               # 早退不烧每晚预算
    assert tp.dispatch_review(st, 641, "h2") == 4243
    assert tp.dispatch_review(st, 642, "h3") is None        # 满槽拒
    assert spawned == [1, 1]                                # 满槽拒零 spawn
    assert "642:h3" not in st["review_tries"]


def test_dispatch_review_acquire_fail_killpg_fallback(monkeypatch, tmp_path,
                                                       capsys):
    """acquire 失败(killpg 兜底): monkeypatch slot_acquire → False,断言
    os.killpg 被调(打桩记录)且返回 None、tries 不增(击杀不烧预算);
    T10 顺手修③ 补 [review] 占位失败打印(唯一保留的击杀网: 同轮并发
    竞态,活占位早退已拦常态)。"""
    _patch_review_offline(monkeypatch, tmp_path, alive=True)
    monkeypatch.setattr(tp, "slot_acquire", lambda *a, **kw: False)
    killed = []
    monkeypatch.setattr(os, "killpg",
                        lambda pid, sig: killed.append((pid, sig)))
    st = _mkstate()
    assert tp.dispatch_review(st, 640, "h1") is None
    assert killed == [(4242, 9)]
    assert st["review_tries"] == {}
    assert "[review]" in capsys.readouterr().out           # ③ 静默路补打印


def test_dispatch_review_agent_env_missing_refuses(monkeypatch, tmp_path,
                                                   capsys):
    """T10 顺手修①(B44 同款前置拒绝): agent.env 缺失 → 打印 + 返回 None,
    零 spawn、不烧每晚预算(tries 不增)、不占槽——不拦则第 5 bot 从未
    启动却烧满 2 次/晚,还把满槽让给假占位。"""
    _patch_review_offline(monkeypatch, tmp_path, alive=True)
    os.unlink(tmp_path / "st" / "agent.env")
    spawned = []
    monkeypatch.setattr(tl, "run_background",
                        lambda cmd, cwd, log: spawned.append(1) or 4242)
    st = _mkstate()
    assert tp.dispatch_review(st, 640, "h1") is None
    assert spawned == []                          # 缺凭据零 spawn
    assert st["review_tries"] == {}               # 不烧每晚预算
    assert st.get("slots", {}).get("pr_review", []) == []
    assert "agent.env" in capsys.readouterr().out


def test_reset_review_tries_nightly_boundary(monkeypatch, tmp_path):
    """每晚边界: reset_review_tries(pipeline 启动时清零)后同 (pr,head)
    立即可再派,计数从 1 起步。"""
    _patch_review_offline(monkeypatch, tmp_path, alive=False)
    st = _mkstate()
    assert tp.dispatch_review(st, 640, "h1") == 4242
    assert tp.dispatch_review(st, 640, "h1") == 4243
    assert tp.dispatch_review(st, 640, "h1") is None          # 每晚上限 2
    tp.reset_review_tries(st)
    assert st["review_tries"] == {}
    assert tp.dispatch_review(st, 640, "h1") == 4244          # 新夜可再派
    assert st["review_tries"]["640:h1"] == 1


def test_dispatch_review_spawn_command_shape(monkeypatch, tmp_path):
    """派发契约: spawn `--pr <N>`(timeout 7500s / agent.env 凭据 /
    setsid+`echo $!` 回显形态)、log = <STATE_DIR>/review-pr<N>-<ts>.log、
    cwd = 克隆目录;常量锚 REVIEW_AGENT_TIMEOUT_SEC=7500(M13,2026-11-13
    终审: 外层 5400 < 内层最坏 claude 3600 + 解析失败重试 3600 = 7200+,
    必被内层反超——检视 agent 全部死于外层,审查白跑) /
    REVIEW_PER_HEAD_PER_NIGHT=2;ts 为 UTC 带 Z 后缀(T10 顺手修④: naive
    本地时间戳 → 与 tl.dispatch_agent 的 _now_iso UTC 口径对齐,文件名
    模板 review-pr<N>-<ts>.log 不变)。"""
    _patch_review_offline(monkeypatch, tmp_path, alive=True)
    monkeypatch.setenv("TASKLOOP_CLONE_DIR", str(tmp_path / "cl"))
    seen = {}
    monkeypatch.setattr(
        tl, "run_background",
        lambda cmd, cwd, log: seen.update(cmd=cmd, cwd=cwd, log=log) or 4242)
    st = _mkstate()
    assert tp.dispatch_review(st, 640, "h1") == 4242
    assert tp.REVIEW_AGENT_TIMEOUT_SEC == 7500
    assert tp.REVIEW_PER_HEAD_PER_NIGHT == 2
    assert "--pr 640" in seen["cmd"]
    assert "timeout 7500 /usr/bin/python3" in seen["cmd"]
    assert "pr_local_review.py" in seen["cmd"]
    assert "agent.env" in seen["cmd"]
    assert "setsid" in seen["cmd"] and "echo $!" in seen["cmd"]
    assert seen["cwd"] == str(tmp_path / "cl")
    assert seen["log"].startswith(str(tmp_path / "st"))
    assert re.fullmatch(r".*review-pr640-\d{8}T\d{6}Z\.log", seen["log"])


def test_pr_local_review_cli_pr_flag_and_mutex(monkeypatch, capsys):
    """--pr 单 PR CLI: 与 --all-open 互斥(argparse error → SystemExit 2,
    任一顺序);--pr 与位置参数 <PR> 共用单 PR 路径 review_one_pr(取 head
    sha → run_review → publish_review,spec §4 泳道2「发布检视」);无参
    用法错误 exit 2。"""
    import pr_local_review as plr
    for argv in (["--pr", "640", "--all-open"],
                 ["--all-open", "--pr", "640"]):
        try:
            plr.main(argv)
            code = None
        except SystemExit as exc:
            code = exc.code
        assert code == 2
    assert "互斥" in capsys.readouterr().err
    calls = []
    monkeypatch.setattr(plr, "review_one_pr", lambda n: calls.append(n))
    assert plr.main(["--pr", "640"]) == 0
    assert plr.main(["641"]) == 0                    # 位置参数同路径
    assert calls == [640, 641]
    assert plr.main([]) == 2                          # 无参用法错误
    assert calls == [640, 641]
    assert "需指定" in capsys.readouterr().err


# ============ Task 7: merge 门 evaluate_merge_gate ============


def _pr(state_="open", draft=False, mergeable=True, title="T",
        local=True, latest=100000, opinions=0, fork=False):
    """merge 门 PR 替身(控制器裁定#3): SimpleNamespace 形状与真实 PRData
    关键字段吻合(bot_done/latest_signal/opinion_count/pending_skew_opinions/
    annotations_fetch_failed/draft/mergeable/fork/state/title/head_sha;
    PR 号双形 pr_number+number(真实 PRData 字段名是 number,T5 _pr_number
    同款双形契约);latest=最新信号距 now 秒数(默认远超静默窗),None →
    latest_signal=None(静默窗未起步臂)。"""
    return NS(state=state_, draft=draft, mergeable=mergeable, title=title,
              head_sha="abc1234", url="u", pr_number=1, number=1, fork=fork,
              bot_done={
                  "codex": "x", "coderabbit": "x", "pr-agent": "x",
                  "greptile": "x",
                  "local-claude": "x" if local else None},
              latest_signal=None if latest is None else
              datetime.now(timezone.utc) - timedelta(seconds=latest),
              opinion_count=opinions, pending_skew_opinions=0,
              annotations_fetch_failed=False)


def _gate_entry(fix_branch="fix/issue-1", pr_number=1, issue_number=1):
    return NS(pr_number=pr_number, issue_number=issue_number,
              fix_branch=fix_branch, status="PR_OPEN")


def _gh_stub(rollups, merged=None, rec=None):
    """tl.gh_json_idem 离线桩(零触网,fix-round 1 基线新形态):
    - `pr view <N> --json statusCheckRollup` → rollups[N](统一存
      (name, conclusion, workflowName) 规范形),列序按 args 的 --jq 过滤器
      忠实回放——两个生产通道列序不同(候选 checks_ok
      [.name, .conclusion, .workflowName];基线 baseline_reds
      [.workflowName, .name, .conclusion]),真 gh 正是按过滤器定列序;
      候选 PR(_pr 默认 1 号)与已合入基线 PR 按 args[2] 分派。
    - `pr list --state merged --json number` → merged 原样回放: 多行裸串
      (gh_json JSONDecodeError → 原样)/ int 单值(gh --jq 单号输出裸数字,
      json.loads 得 int)/ None(gh 空输出契约)/ Exception(抛 GhError)。
    - rec: 可选 list——pr list 分支把 args 原样记入(裁定#2: 基线
      pr list 形态断言的取证通道)。"""
    def fake(args, input_text=None):
        if args[:2] == ["pr", "list"]:
            if rec is not None:
                rec.append(list(args))
            if isinstance(merged, Exception):
                raise merged
            return merged
        if args[:2] == ["pr", "view"] and "statusCheckRollup" in args:
            n = str(args[2])
            jq = args[args.index("--jq") + 1]
            wf_first = ".workflowName, .name" in jq
            return "".join((f"{w}\t{nm}\t{c}\n" if wf_first
                            else f"{nm}\t{c}\t{w}\n")
                           for nm, c, w in rollups[n])
        raise AssertionError(f"意外 gh 调用: {args}")
    return fake


def test_required_checks_and_engine_branch_re_calibration():
    """常量锚(裁定#1 校准,2026-11-13 对现役 PR #661/#660/#643/#642
    `gh pr view --json statusCheckRollup` 实测四 PR 完全一致): rollup 的
    CheckRun 实名是 lint / git-sanity / Build binary for CPU (Linux) /
    zizmor (workflow security)——分支保护 UI 建议集写法「Security / zizmor」
    在 rollup 不出现;checks_ok 按前缀匹配,常量写主干(矩阵后缀形态由
    前缀覆盖,zizmor 状态上下文裸名「zizmor」同被主干覆盖)。"""
    assert tp.REQUIRED_CHECKS_FULL == ["lint", "git-sanity",
                                       "Build binary for CPU (Linux)", "zizmor"]
    assert tp.REQUIRED_CHECKS_DOCS == ["lint", "git-sanity"]
    assert tp.DOCS_QUIET_SEC == 300
    for branch in ("fix/issue-1", "fix/issue-123", "fix/ci-45",
                   "fix/revert-pr67", "fix/issue-1-more"):
        # 前缀匹配语义(brief 原文 re.match 无 $ 锚,spec glob fix/issue-*):
        # fix/issue-1-more 属 fix/issue-* 形,应过
        assert tp.ENGINE_BRANCH_RE.match(branch), branch
    for branch in ("fix/user-thing", "user-branch", "fix/issue-abc",
                   "", "fix/revert-42"):
        assert not tp.ENGINE_BRANCH_RE.match(branch), branch


def test_wip_gated_and_docs_only_shapes():
    assert tp.wip_gated("WIP: feat/x") is True
    assert tp.wip_gated("feat: 正常标题") is False
    assert tp.wip_gated("") is False
    assert tp.docs_only(["a.md", "docs/b.md"]) is True
    assert tp.docs_only([]) is False            # 空清单不冒充 docs-only
    assert tp.docs_only(["a.md", "b.c"]) is False
    # 与 tl._docs_only 单一真源一致(T5 已锚;同一判定喂轮次上限与快道)
    for files in (["a.md"], [], ["a.md", "b.c"], ["x.md", "y.md"]):
        assert tp.docs_only(files) == tl._docs_only(files)


def test_docs_ready_matrix():
    """docs 快道门槛 2 放宽四要素: 第5bot 唯一 + DOCS_QUIET_SEC=300s 静默 +
    零意见(含 pending 超容差/annotations 完整性,同 classify 口径)。"""
    now = datetime.now(timezone.utc)
    assert tp.docs_ready(_pr(latest=tp.DOCS_QUIET_SEC + 1), now) is True
    pr = _pr(latest=None)
    pr.latest_signal = now - timedelta(seconds=tp.DOCS_QUIET_SEC)
    assert tp.docs_ready(pr, now) is True          # 边界 ≥(恰 300s 过)
    pr = _pr(latest=None)
    pr.latest_signal = now - timedelta(seconds=tp.DOCS_QUIET_SEC - 1)
    assert tp.docs_ready(pr, now) is False         # 差 1s 未过窗
    assert tp.docs_ready(_pr(latest=None), now) is False      # 无信号
    assert tp.docs_ready(_pr(local=False), now) is False     # 第5bot 缺
    assert tp.docs_ready(_pr(opinions=1), now) is False      # 有可行动意见
    pr = _pr(); pr.pending_skew_opinions = 1
    assert tp.docs_ready(pr, now) is False                    # 超容差意见
    pr = _pr(); pr.annotations_fetch_failed = True
    assert tp.docs_ready(pr, now) is False                    # annotations 不完整


def test_baseline_reds_merged_pr_union_and_degradation(monkeypatch):
    """baseline_reds(controller 裁定 2026-11-13,fix-round 1 Important#2 重写):
    最近 BASELINE_MERGED_DEPTH=3 个已合入 PR 的 rollup 中,同 workflowName 的
    红 job 名并集——原 main-run 形态对 PR-CI workflow("Pull Request",
    lint/build 所在)结构性为空(仅 pull_request 事件跑在 PR head,main 上
    无此 workflow 的 schedule/push run),继承红永不放行、特性死亡。
    多 PR 取并集;拉不到/空 → 空集(不放行任何红,宁严勿松)。"""
    assert tp.BASELINE_MERGED_DEPTH == 3
    rollups = {
        "901": [("lint", "FAILURE", "Pull Request"),
                ("git-sanity", "SUCCESS", "Pull Request"),
                ("spell-check", "FAILURE", "Docs Lint")],
        "902": [("lint", "SUCCESS", "Pull Request"),
                ("build-arm", "FAILURE", "Pull Request")],   # 同组增并集
        "903": [("zizmor (workflow security)", "FAILURE", "Security")],
    }
    rec = []
    monkeypatch.setattr(tl, "gh_json_idem",
                        _gh_stub(rollups, merged="901\n902\n903\n", rec=rec))
    # Pull Request 组: 901 lint + 902 build-arm 并集;异 workflow 组不混入
    assert tp.baseline_reds("o/r", "Pull Request") == {"lint", "build-arm"}
    assert tp.baseline_reds("o/r", "Security") == {"zizmor (workflow security)"}
    assert tp.baseline_reds("o/r", "Docs Lint") == {"spell-check"}
    assert tp.baseline_reds("o/r", "不存在的工作流") == set()
    # 基线 pr list 形态锚(裁定#2,T7 复审遗留 Minor): --state merged +
    # --limit str(BASELINE_MERGED_DEPTH) + --repo——防漂回 main-run 旧形
    assert rec, "基线 pr list 调用未被记录"
    args = rec[0]
    assert args[:2] == ["pr", "list"]
    assert args[args.index("--state") + 1] == "merged"
    assert args[args.index("--limit") + 1] == str(tp.BASELINE_MERGED_DEPTH)
    assert args[args.index("--repo") + 1] == "o/r"
    # 单个已合入 PR: gh --jq '.[].number' 输出裸数字 → gh_json json.loads
    # 得 int → str().split() 整链可用
    monkeypatch.setattr(tl, "gh_json_idem",
                        _gh_stub({"901": rollups["901"]}, merged=901))
    assert tp.baseline_reds("o/r", "Pull Request") == {"lint"}
    # merged 清单为空(gh 空输出 None)/pr list GhError → 空集(宁严勿松)
    monkeypatch.setattr(tl, "gh_json_idem", _gh_stub({}, merged=None))
    assert tp.baseline_reds("o/r", "Pull Request") == set()
    monkeypatch.setattr(tl, "gh_json_idem",
                        _gh_stub({}, merged=tl.GhError(
                            "gh pr list … 失败(exit 1): HTTP 502")))
    assert tp.baseline_reds("o/r", "Pull Request") == set()
    # 单个基线 PR 的 rollup 拉取失败(GhError) → 跳过该 PR 不炸整链

    def flaky(args, input_text=None):
        if args[:2] == ["pr", "list"]:
            return "901\n902\n"
        if str(args[2]) == "901":
            raise tl.GhError("gh pr view 901 … 失败(exit 1): HTTP 502")
        # 基线通道列序 [.workflowName, .name, .conclusion](与 checks_ok 候选
        # 通道相反——按生产 jq 过滤器回放)
        return "Pull Request\tlint\tFAILURE\n"
    monkeypatch.setattr(tl, "gh_json_idem", flaky)
    assert tp.baseline_reds("o/r", "Pull Request") == {"lint"}


def test_checks_ok_green_prefix_and_case_forms(monkeypatch):
    """全绿 rollup → (True, []);前缀匹配覆盖矩阵后缀形态;结论大小写归一
    (实测 rollup 结论大写 SUCCESS/SKIPPED/FAILURE——brief 的
    (success, skipped, SUCCESS) 漏大写 SKIPPED,校准修正为小写归一);
    SKIPPED 不算红(docs-only 重 job 常见形态)。"""
    rows = [("lint", "SUCCESS", "Pull Request"),
            ("git-sanity", "success", "Pull Request"),   # 小写亦绿
            ("Build binary for CPU (Linux) (GCC-arm64, -arm, unittests, "
             "selftests)", "SKIPPED", "Pull Request"),    # 矩阵后缀 + skip
            ("zizmor (workflow security)", "SUCCESS", "Security"),
            ("Multi-version (${{ matrix.series }})", "SKIPPED", "Pull Request"),
            ("CodeRabbit", "SKIPPED", "")]                # 状态上下文空 workflow
    monkeypatch.setattr(tl, "gh_json_idem", _gh_stub({"1": rows}))
    ok, why = tp.checks_ok(_pr(), tp.REQUIRED_CHECKS_FULL, "o/r")
    assert ok is True and why == []


def test_checks_ok_required_missing_and_gherror(monkeypatch):
    rows = [("lint", "SUCCESS", "Pull Request"),
            ("git-sanity", "SUCCESS", "Pull Request")]   # Build binary/zizmor 缺席
    monkeypatch.setattr(tl, "gh_json_idem", _gh_stub({"1": rows}))
    ok, why = tp.checks_ok(_pr(), tp.REQUIRED_CHECKS_FULL, "o/r")
    assert ok is False and "required check 缺席" in why[0]
    # docs 集(lint+git-sanity)在 → True
    ok, _ = tp.checks_ok(_pr(), tp.REQUIRED_CHECKS_DOCS, "o/r")
    assert ok is True
    # rollup 拉取失败 → False + 证据
    def boom(args, input_text=None):
        raise tl.GhError("gh pr view … 失败(exit 1): HTTP 502")
    monkeypatch.setattr(tl, "gh_json_idem", boom)
    ok, why = tp.checks_ok(_pr(), tp.REQUIRED_CHECKS_DOCS, "o/r")
    assert ok is False and "check rollup 拉取失败" in why[0]
    # gh 空输出(gh_json None 契约)不炸 → 零 check → 缺席拒绝
    monkeypatch.setattr(tl, "gh_json_idem", lambda a, input_text=None: None)
    ok, why = tp.checks_ok(_pr(), tp.REQUIRED_CHECKS_DOCS, "o/r")
    assert ok is False and "required check 缺席" in why[0]


def test_checks_ok_inherited_red_by_workflow_groups(monkeypatch):
    """继承红(裁定#2 数据流 + fix-round 1 基线重写): 候选 PR 红 job 的
    workflowName 从 rollup 行收集,按 workflow 分组对照「最近 3 个已合入
    PR 同 workflowName 的红 job 并集」——全组 ⊆ 基线 → 放行并携带证据;
    任一组有非基线红 → 拒;空 workflowName(状态上下文)同通道对照,
    已合入对照组无该红 → 拒。"""
    cand = [("lint", "FAILURE", "Pull Request"),
            ("git-sanity", "SUCCESS", "Pull Request"),
            ("Build binary for CPU (Linux)", "SUCCESS", "Pull Request"),
            ("zizmor (workflow security)", "SUCCESS", "Security"),
            ("spell-check", "FAILURE", "Docs Lint")]
    base = {"901": [("lint", "FAILURE", "Pull Request")],   # lint 继承红
            "902": [("CodeRabbit", "SKIPPED", "")],
            "903": [("spell-check", "SUCCESS", "Docs Lint")]}   # 基线绿 → 外来红
    merged = "901\n902\n903\n"
    monkeypatch.setattr(tl, "gh_json_idem",
                        _gh_stub({"1": cand, **base}, merged=merged))
    ok, why = tp.checks_ok(_pr(), tp.REQUIRED_CHECKS_FULL, "o/r")
    assert ok is False and "非基线继承" in why[0] and "spell-check" in why[0]
    # 去掉 Docs Lint 外来红 → lint 红继承自已合入基线 → 放行 + 证据
    monkeypatch.setattr(tl, "gh_json_idem",
                        _gh_stub({"1": cand[:4], **base}, merged=merged))
    ok, why = tp.checks_ok(_pr(), tp.REQUIRED_CHECKS_FULL, "o/r")
    assert ok is True and "继承红放行" in why[0] and "lint" in why[0]
    # 空 workflowName 的红(状态上下文): 已合入对照组同组无该红 → 拒
    rows3 = [("lint", "SUCCESS", "Pull Request"),
             ("git-sanity", "SUCCESS", "Pull Request"),
             ("Build binary for CPU (Linux)", "SUCCESS", "Pull Request"),
             ("zizmor (workflow security)", "SUCCESS", "Security"),
             ("CodeRabbit", "FAILURE", "")]
    monkeypatch.setattr(tl, "gh_json_idem",
                        _gh_stub({"1": rows3, **base}, merged=merged))
    ok, why = tp.checks_ok(_pr(), tp.REQUIRED_CHECKS_FULL, "o/r")
    assert ok is False and "非基线继承" in why[0] and "CodeRabbit" in why[0]
    # merged 清单拉取失败(GhError)→ 空基线 → 红永不放行(宁严勿松)

    def boom(args, input_text=None):
        if args[:2] == ["pr", "list"]:
            raise tl.GhError("gh pr list … 失败(exit 1): HTTP 502")
        if args[:2] == ["pr", "view"] and str(args[2]) == "1":
            return "".join(f"{nm}\t{c}\t{w}\n" for nm, c, w in cand)
        raise AssertionError(f"空基线短路后不应再拉基线 PR: {args}")
    monkeypatch.setattr(tl, "gh_json_idem", boom)
    ok, why = tp.checks_ok(_pr(), tp.REQUIRED_CHECKS_FULL, "o/r")
    assert ok is False and "非基线继承" in why[0] and "lint" in why[0]


def test_gate_full_path_engine_pr_ok(monkeypatch, tmp_path):
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    st = _mkstate()
    seen = {}
    monkeypatch.setattr(tl.pr_loop, "classify_state",
                        lambda pr, q, now: "READY_TO_MERGE")
    monkeypatch.setattr(tp, "checks_ok",
                        lambda pr, req, slug: seen.update(req=req, slug=slug)
                        or (True, []))
    monkeypatch.setattr(tl, "_paused", lambda: False)
    monkeypatch.setattr(tl, "repo_slug", lambda: "o/r")
    e = _gate_entry()
    r = tp.evaluate_merge_gate(st, e, _pr(), ["a.c"],
                              datetime.now(timezone.utc), False)
    assert r.ok is True and r.fast is False
    assert seen["req"] == tp.REQUIRED_CHECKS_FULL   # 全门槛查 FULL 集
    assert seen["slug"] == "o/r"
    assert r.reasons == []


def test_gate_rejects_user_wip_and_missing_ownership(monkeypatch):
    # 熔断前置(Minor #4)后 _paused 在入口即查——无 env 重定向的用例必须桩死,
    # 否则探活真机默认 state 目录(宿主相关)
    monkeypatch.setattr(tl, "_paused", lambda: False)
    st = _mkstate()
    r = tp.evaluate_merge_gate(st, None, _pr(title="WIP: feat/x"),
                               ["a.md"], datetime.now(timezone.utc), False)
    assert r.ok is False and "WIP" in " ".join(r.reasons)   # 草稿闸
    # 非 docs 归属缺条目也拒(brief 注): 用户代码 PR 无引擎条目 → 门槛 1 拒
    r2 = tp.evaluate_merge_gate(st, None, _pr(), ["a.c"],
                                datetime.now(timezone.utc), False)
    assert r2.ok is False and "非引擎 PR" in " ".join(r2.reasons)


def test_gate_rejects_non_engine_branch(monkeypatch):
    """门槛 1 引擎分支形: fix/issue-N|fix/ci-N|fix/revert-prN 之外拒
    (用户 WIP 分支结构性排除,无白名单)。"""
    monkeypatch.setattr(tl, "_paused", lambda: False)
    r = tp.evaluate_merge_gate(_mkstate(), _gate_entry(fix_branch="fix/user-x"),
                               _pr(), ["a.c"], datetime.now(timezone.utc), False)
    assert r.ok is False and "非引擎分支" in " ".join(r.reasons)


def test_gate_rejects_closed_draft_conflict(monkeypatch):
    """门槛 4 状态拒臂: 非 open / DRAFT / fork(Critical #1,fix-round 1)/
    mergeable=false。"""
    monkeypatch.setattr(tl, "_paused", lambda: False)
    now = datetime.now(timezone.utc)
    r = tp.evaluate_merge_gate(_mkstate(), _gate_entry(),
                               _pr(state_="closed"), ["a.c"], now, False)
    assert r.ok is False and "非 open/DRAFT" in " ".join(r.reasons)
    r = tp.evaluate_merge_gate(_mkstate(), _gate_entry(),
                               _pr(draft=True), ["a.c"], now, False)
    assert r.ok is False and "非 open/DRAFT" in " ".join(r.reasons)
    r = tp.evaluate_merge_gate(_mkstate(), _gate_entry(),
                               _pr(mergeable=False), ["a.c"], now, False)
    assert r.ok is False and "冲突" in " ".join(r.reasons)


def test_gate_rejects_fork_pr(monkeypatch, tmp_path):
    """Critical #1(fix-round 1): spec 门槛 ④ = open ∧ 非 DRAFT ∧ **非 fork**
    ∧ mergeable——fork 的 docs PR 也不走快道放行,引擎臂同样拒。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    monkeypatch.setattr(tl, "_paused", lambda: False)
    monkeypatch.setattr(tp, "checks_ok", lambda pr, req, slug: (True, []))
    monkeypatch.setattr(tl, "repo_slug", lambda: "o/r")
    now = datetime.now(timezone.utc)
    # fork 的 docs PR(entry=None、全绿、无意见)→ ok False
    r = tp.evaluate_merge_gate(_mkstate(), None, _pr(fork=True), ["a.md"],
                               now, False)
    assert r.ok is False and "fork" in " ".join(r.reasons)
    # 引擎臂同样拒
    r = tp.evaluate_merge_gate(_mkstate(), _gate_entry(), _pr(fork=True),
                               ["a.c"], now, False)
    assert r.ok is False and "fork" in " ".join(r.reasons)


def test_gate_docs_fast_path(monkeypatch, tmp_path):
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    st = _mkstate()
    seen = {}
    monkeypatch.setattr(tp, "checks_ok",
                        lambda pr, req, slug: seen.update(req=req) or (True, []))
    monkeypatch.setattr(tl, "_paused", lambda: False)
    monkeypatch.setattr(tl, "repo_slug", lambda: "o/r")
    # 快道不查外部四 bot(门槛 2 放宽: 第5bot 唯一)——classify 一触即炸
    monkeypatch.setattr(tl.pr_loop, "classify_state",
                        lambda pr, q, now: (_ for _ in ()).throw(
                            AssertionError("docs 快道不应走 classify_state")))
    now = datetime.now(timezone.utc)
    pr = _pr(local=True, opinions=0)
    pr.latest_signal = now - timedelta(seconds=tp.DOCS_QUIET_SEC + 10)
    r = tp.evaluate_merge_gate(st, None, pr, ["a.md"], now, False)  # 用户 docs PR
    assert r.ok is True and r.fast is True
    assert seen["req"] == tp.REQUIRED_CHECKS_DOCS   # 快道只查快 job


def test_gate_docs_with_opinions_not_ok(monkeypatch):
    monkeypatch.setattr(tl, "_paused", lambda: False)   # 熔断入口即查(Minor #4)
    now = datetime.now(timezone.utc)
    pr = _pr(local=True, opinions=2)
    pr.latest_signal = now - timedelta(seconds=400)
    r = tp.evaluate_merge_gate(_mkstate(), None, pr, ["a.md"], now, False)
    assert r.ok is False
    assert "docs 快道未就绪" in " ".join(r.reasons)


def test_gate_engine_entry_docs_files_take_fast_path(monkeypatch, tmp_path):
    """引擎条目的 docs-only PR 同样走快道(门槛 1/2/3 docs 例外适用任意
    PR,spec §5): 引擎分支仍须匹配,检视只等第5bot+300s,CI 只查快 job。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    seen = {}
    monkeypatch.setattr(tp, "checks_ok",
                        lambda pr, req, slug: seen.update(req=req) or (True, []))
    monkeypatch.setattr(tl, "_paused", lambda: False)
    monkeypatch.setattr(tl, "repo_slug", lambda: "o/r")
    monkeypatch.setattr(tl.pr_loop, "classify_state",
                        lambda pr, q, now: (_ for _ in ()).throw(
                            AssertionError("docs 快道不应走 classify_state")))
    r = tp.evaluate_merge_gate(_mkstate(), _gate_entry(), _pr(), ["a.md"],
                               datetime.now(timezone.utc), False)
    assert r.ok is True and r.fast is True
    assert seen["req"] == tp.REQUIRED_CHECKS_DOCS


def test_gate_engine_classify_not_ready_rejects(monkeypatch, tmp_path):
    """全门槛路径检视判定委托 pr_loop.classify_state(五 bot/静默/意见),
    DEFAULT_QUIET 口径;≠READY_TO_MERGE 即拒。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    seen = {}

    def cls(pr, quiet, now_arg):
        seen["quiet"] = quiet
        return "READY_TO_FIX"
    monkeypatch.setattr(tl.pr_loop, "classify_state", cls)
    r = tp.evaluate_merge_gate(_mkstate(), _gate_entry(), _pr(), ["a.c"],
                               datetime.now(timezone.utc), False)
    assert r.ok is False and "READY_TO_FIX" in " ".join(r.reasons)
    assert seen["quiet"] == tl.pr_loop.DEFAULT_QUIET


def test_gate_rejects_when_checks_fail_and_when_paused(monkeypatch, tmp_path):
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    monkeypatch.setattr(tl.pr_loop, "classify_state",
                        lambda pr, q, now: "READY_TO_MERGE")
    monkeypatch.setattr(tl, "_paused", lambda: False)
    monkeypatch.setattr(tl, "repo_slug", lambda: "o/r")
    now = datetime.now(timezone.utc)
    monkeypatch.setattr(tp, "checks_ok",
                        lambda pr, req, slug: (False, ["required check 缺席: ['zizmor']"]))
    r = tp.evaluate_merge_gate(_mkstate(), _gate_entry(), _pr(), ["a.c"],
                               now, False)
    assert r.ok is False and "required check 缺席" in " ".join(r.reasons)
    # 熔断(Minor #4 前置到入口,零 gh 成本): PAUSED 优先于归属/状态等一切
    # 后续检查——非引擎分支条目在 PAUSED 下报 PAUSED 而非「非引擎分支」
    monkeypatch.setattr(tp, "checks_ok", lambda pr, req, slug: (True, []))
    monkeypatch.setattr(tl, "_paused", lambda: True)
    r = tp.evaluate_merge_gate(_mkstate(),
                               _gate_entry(fix_branch="fix/user-x"),
                               _pr(), ["a.c"], now, False)
    assert r.ok is False and "PAUSED" in " ".join(r.reasons)
    r = tp.evaluate_merge_gate(_mkstate(), _gate_entry(), _pr(), ["a.c"],
                               now, False)
    assert r.ok is False and "PAUSED" in " ".join(r.reasons)
    # docs 快道同样受熔断
    r = tp.evaluate_merge_gate(_mkstate(), None, _pr(latest=1000), ["a.md"],
                               now, False)
    assert r.ok is False and "PAUSED" in " ".join(r.reasons)
    # no_auto_merge 参数臂(检测函数 no_auto_merge_active 属 T8,裁定#4)
    monkeypatch.setattr(tl, "_paused", lambda: False)
    r = tp.evaluate_merge_gate(_mkstate(), _gate_entry(), _pr(), ["a.c"],
                               now, True)
    assert r.ok is False and "NO_AUTO_MERGE" in " ".join(r.reasons)


def test_gate_engine_inherited_red_passes_with_evidence(monkeypatch, tmp_path):
    """全门槛 + 继承红放行(rollup workflowName 数据流,裁定#2;fix-round 1
    基线重写): 候选 rollup lint 红 ⊆ 最近 3 个已合入 PR 同 workflow 的红 job
    并集 → 门过且 reasons 携带证据行(spec §5 门槛 6 审计评论的输入;真实
    PR #661 现况即 lint 继承红)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    monkeypatch.setattr(tl.pr_loop, "classify_state",
                        lambda pr, q, now: "READY_TO_MERGE")
    monkeypatch.setattr(tl, "_paused", lambda: False)
    monkeypatch.setattr(tl, "repo_slug", lambda: "o/r")
    rows = [("lint", "FAILURE", "Pull Request"),
            ("git-sanity", "SUCCESS", "Pull Request"),
            ("Build binary for CPU (Linux)", "SUCCESS", "Pull Request"),
            ("zizmor (workflow security)", "SUCCESS", "Security")]
    base = {"901": [("lint", "FAILURE", "Pull Request")],
            "902": [], "903": []}
    monkeypatch.setattr(tl, "gh_json_idem",
                        _gh_stub({"1": rows, **base}, merged="901\n902\n903\n"))
    r = tp.evaluate_merge_gate(_mkstate(), _gate_entry(), _pr(), ["a.c"],
                               datetime.now(timezone.utc), False)
    assert r.ok is True and r.fast is False
    assert any("继承红放行" in x and "lint" in x for x in r.reasons)


def test_gate_no_auto_merge_and_paused(monkeypatch, tmp_path):
    """门槛 5 熔断参数臂(裁定#4: NO_AUTO_MERGE 文件的检测函数
    no_auto_merge_active 属 T8,门只收 no_auto_merge: bool——文件落盘仅为
    T8 接线形态示例);存在 → 门转纯报告模式(ok=False,灰度)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    (tmp_path / "st").mkdir(parents=True)
    (tmp_path / "st" / "NO_AUTO_MERGE").write_text("")
    monkeypatch.setattr(tl, "_paused", lambda: False)
    monkeypatch.setattr(tp, "checks_ok", lambda pr, req, slug: (True, []))
    monkeypatch.setattr(tl, "repo_slug", lambda: "o/r")
    now = datetime.now(timezone.utc)
    pr = _pr(); pr.latest_signal = now - timedelta(seconds=1000)
    r = tp.evaluate_merge_gate(_mkstate(), None, pr, ["a.md"], now, True)
    assert r.ok is False and "NO_AUTO_MERGE" in " ".join(r.reasons)


def test_gate_mergeable_none_transitional_not_rejected(monkeypatch, tmp_path):
    """mergeable=None/字段缺失形态不拒(裁定#1 锚形,T7 已裁定的过渡
    语义): 真实 PRData 无 mergeable,fill_mergeable 拉取失败/gh 空输出
    时保持 None——门只把显式 False 当冲突拒;后续若把 None 也拒(如
    is not True)即接线遗漏,本测当场红。checks_ok 走真路径经
    tl.gh_json_idem 桩离线。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    monkeypatch.setattr(tl, "_paused", lambda: False)
    monkeypatch.setattr(tl, "repo_slug", lambda: "o/r")
    monkeypatch.setattr(tl.pr_loop, "classify_state",
                        lambda pr, q, now: "READY_TO_MERGE")
    rows = [("lint", "SUCCESS", "Pull Request"),
            ("git-sanity", "SUCCESS", "Pull Request"),
            ("Build binary for CPU (Linux)", "SUCCESS", "Pull Request"),
            ("zizmor (workflow security)", "SUCCESS", "Security")]
    monkeypatch.setattr(tl, "gh_json_idem", _gh_stub({"1": rows}))
    now = datetime.now(timezone.utc)
    r = tp.evaluate_merge_gate(_mkstate(), _gate_entry(), _pr(mergeable=None),
                               ["a.c"], now, False)
    assert r.ok is True
    pr = _pr()
    del pr.mergeable          # 字段缺失 = 真实 PRData 形态
    r2 = tp.evaluate_merge_gate(_mkstate(), _gate_entry(), pr, ["a.c"],
                                now, False)
    assert r2.ok is True


# ============ Task 8: merge 执行 + 记账 + 审计评论 ============


def test_execute_merge_records_sha_and_comments(monkeypatch, tmp_path):
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    calls = []
    monkeypatch.setattr(tl, "gh_json_idem",
                        lambda args, input_text=None: calls.append(args) or
                        ("abc1234def5678" if "--jq" in args and
                         "mergeCommit" in " ".join(args) else "{}"))
    monkeypatch.setattr(tl, "comment_on_issue",
                        lambda n, b: calls.append(["comment", n]))
    e = NS(pr_number=42, issue_number=7, fix_branch="fix/issue-7",
           status="PR_OPEN", merged_at=None, merged_sha=None,
           updated_at="t")
    st = {"entries": [e], "auth_failure_streak": 0}
    gr = tp.GateResult(True, False, ["继承红放行: [X] ⊆ 已合入 PR 基线红"])
    sha = tp.execute_merge(st, e, _pr(), gr)
    assert sha == "abc1234def5678" and e.merged_sha == sha and e.merged_at
    assert e.status == "DONE"
    assert any("squash" in " ".join(map(str, c)) for c in calls)
    # 三步序: PR 审计评论 → merge(squash) → 回读 mergeCommit,issue 收尾在后
    ci = next(i for i, c in enumerate(calls)
              if c[:2] == ["pr", "comment"])
    mi = next(i for i, c in enumerate(calls)
              if c[:2] == ["pr", "merge"])
    vi = next(i for i, c in enumerate(calls)
              if "--jq" in c and "mergeCommit" in " ".join(map(str, c)))
    assert ci < mi < vi
    assert ["comment", 7] in calls                # issue 收尾评论


def test_execute_merge_user_docs_pr_no_entry(monkeypatch, tmp_path):
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    monkeypatch.setattr(tl, "gh_json_idem", lambda a, input_text=None: "sha1")
    monkeypatch.setattr(tl, "comment_on_issue", lambda n, b: None)
    sha = tp.execute_merge(_mkstate(), None, _pr(),
                           tp.GateResult(True, True, []))
    assert sha == "sha1"


def test_execute_merge_dry_run_only_prints(monkeypatch, tmp_path, capsys):
    """dry_run: 只打印拟执行动作——零 gh 写、零记账、条目原样。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    calls = []
    monkeypatch.setattr(tl, "gh_json_idem",
                        lambda a, input_text=None: calls.append(a) or "x")
    monkeypatch.setattr(tl, "comment_on_issue",
                        lambda n, b: calls.append(["comment", n]))
    e = NS(pr_number=42, issue_number=7, fix_branch="fix/issue-7",
           status="PR_OPEN", merged_at=None, merged_sha=None,
           updated_at="t")
    sha = tp.execute_merge({"entries": [e], "auth_failure_streak": 0}, e,
                           _pr(), tp.GateResult(True, False, []), dry_run=True)
    assert sha is None and calls == []
    assert e.status == "PR_OPEN" and e.merged_sha is None \
        and e.merged_at is None
    out = capsys.readouterr().out
    assert "[dry-run]" in out and "PR #1" in out


def test_execute_merge_failure_keeps_entry_open(monkeypatch, tmp_path):
    """失败路径(三步序半途): 评论成功、merge GhError 原样上抛 → 条目
    不置 DONE/不记 merged_sha(下轮门复评),issue 不收尾。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    calls = []

    def boom(args, input_text=None):
        calls.append(list(args))
        if args[:2] == ["pr", "merge"]:
            raise tl.GhError("gh pr merge … 失败(exit 1): HTTP 502")
        return "{}"
    monkeypatch.setattr(tl, "gh_json_idem", boom)
    monkeypatch.setattr(tl, "comment_on_issue",
                        lambda n, b: calls.append(["comment", n]))
    e = NS(pr_number=42, issue_number=7, fix_branch="fix/issue-7",
           status="PR_OPEN", merged_at=None, merged_sha=None,
           updated_at="t")
    with pytest.raises(tl.GhError):
        tp.execute_merge({"entries": [e], "auth_failure_streak": 0}, e,
                         _pr(), tp.GateResult(True, False, []))
    assert any(c[:2] == ["pr", "comment"] for c in calls)   # 评论已贴
    # issue 未收尾——c[0] 判形(fix-round 1 Important #2: 原 c[:2]==["comment"]
    # 切片恒 ≠ 单元素列表,是恒真空锁)
    assert not any(c[0] == "comment" for c in calls)
    assert ["comment", 7] not in calls
    assert e.status == "PR_OPEN" and e.merged_sha is None \
        and e.merged_at is None


def _merge_flow_stub(view_ret, calls):
    """execute_merge 全流程桩(回读降级两臂共用,fix-round 1 Important #1):
    pr comment/merge 与 issue close 回 "{}";pr view mergeCommit 按
    view_ret 回放——None=gh 空输出,Exception=抛出;comment_on_issue 不在
    此(测试自行记录以断言 issue 收尾)。"""
    def fake(args, input_text=None):
        calls.append(list(args))
        if args[:2] == ["pr", "view"] and "mergeCommit" in " ".join(args):
            if isinstance(view_ret, Exception):
                raise view_ret
            return view_ret
        return "{}"
    return fake


def _degraded_assert(e, sha, calls, capsys):
    """回读降级两臂的共同断言: 不可逆点(merge 已执行)之后宁记账勿崩——
    DONE + merged_sha=None + [warn] 留痕,issue 仍收尾。"""
    assert sha is None and e.status == "DONE" and e.merged_sha is None \
        and e.merged_at
    assert ["comment", 7] in calls                      # issue 收尾仍走
    assert any(c[:2] == ["issue", "close"] for c in calls)
    out = capsys.readouterr().out
    assert "[warn]" in out and "merged_sha" in out


def test_execute_merge_sha_readback_none_still_done(monkeypatch, tmp_path,
                                                     capsys):
    """回读降级臂①(Important #1): squash merge 已执行后 sha 回读得 None
    (gh 空输出/字面 null)——不崩不丢账: 已合入是事实,记账反映现实
    (DONE + merged_sha=None + [warn],revert 归因缺主数据须留痕)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    calls = []
    monkeypatch.setattr(tl, "gh_json_idem", _merge_flow_stub(None, calls))
    monkeypatch.setattr(tl, "comment_on_issue",
                        lambda n, b: calls.append(["comment", n]))
    e = NS(pr_number=42, issue_number=7, fix_branch="fix/issue-7",
           status="PR_OPEN", merged_at=None, merged_sha=None,
           updated_at="t")
    sha = tp.execute_merge({"entries": [e], "auth_failure_streak": 0}, e,
                           _pr(), tp.GateResult(True, False, []))
    _degraded_assert(e, sha, calls, capsys)


def test_execute_merge_sha_readback_gherror_still_done(monkeypatch, tmp_path,
                                                        capsys):
    """回读降级臂②(Important #1): merge 已执行后回读步 GhError——同样
    降级记账,GhError 不再穿透(不可逆点后崩溃不撤销 merge,只丢账;
    AttributeError/GhError 还会穿透上层按 GhError 分类的处置)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    calls = []
    monkeypatch.setattr(tl, "gh_json_idem", _merge_flow_stub(
        tl.GhError("gh pr view … 失败(exit 1): HTTP 502"), calls))
    monkeypatch.setattr(tl, "comment_on_issue",
                        lambda n, b: calls.append(["comment", n]))
    e = NS(pr_number=42, issue_number=7, fix_branch="fix/issue-7",
           status="PR_OPEN", merged_at=None, merged_sha=None,
           updated_at="t")
    sha = tp.execute_merge({"entries": [e], "auth_failure_streak": 0}, e,
                           _pr(), tp.GateResult(True, False, []))
    _degraded_assert(e, sha, calls, capsys)


def test_no_auto_merge_active_probe(monkeypatch, tmp_path):
    """NO_AUTO_MERGE 熔断探测: STATE_DIR 下同名文件存在 → True,缺失 →
    False;纯存在性探查幂等(重复调用同结果,零 gh/零变更)。"""
    st = tmp_path / "st"
    st.mkdir()
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(st))
    assert tp.no_auto_merge_active() is False
    (st / "NO_AUTO_MERGE").write_text("")
    assert tp.no_auto_merge_active() is True
    assert tp.no_auto_merge_active() is True        # 幂等重复探测
    (st / "NO_AUTO_MERGE").unlink()
    assert tp.no_auto_merge_active() is False


def test_fill_mergeable_seam(monkeypatch, tmp_path, capsys):
    """fill_mergeable 公开接缝(裁定#1): 真实 PRData 无 mergeable 字段,
    T10 merge_sweep 接线前由调用方补——缺/None 才拉(有值不重复拉);
    GhError → 保持 None 并 [warn];gh 空输出 None → 保持 None(门按
    T7 过渡语义不拒)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    calls = []
    monkeypatch.setattr(tl, "repo_slug", lambda: "o/r")
    monkeypatch.setattr(tl, "gh_json_idem",
                        lambda a, input_text=None: calls.append(list(a))
                        or False)
    pr = _pr()
    del pr.mergeable                    # 字段缺失 = 真实 PRData 形态
    tp.fill_mergeable(pr)
    assert pr.mergeable is False        # 拉到即写回
    assert len(calls) == 1
    a = calls[0]
    assert a[:3] == ["pr", "view", "1"]            # 取号双形 pr_number|number
    assert a[a.index("--repo") + 1] == "o/r"
    assert a[a.index("--json") + 1] == "mergeable"
    assert a[a.index("--jq") + 1] == ".mergeable"
    # 已有值不重复拉
    tp.fill_mergeable(pr)
    assert len(calls) == 1
    # GhError → 保持 None + [warn](只打警告,不炸链)
    def boom(x, input_text=None):
        raise tl.GhError("gh pr view … 失败(exit 1): HTTP 502")
    monkeypatch.setattr(tl, "gh_json_idem", boom)
    pr2 = _pr(mergeable=None)
    tp.fill_mergeable(pr2)
    assert getattr(pr2, "mergeable", None) is None
    assert "[warn]" in capsys.readouterr().out
    # gh 空输出(gh_json None 契约)→ 保持 None
    monkeypatch.setattr(tl, "gh_json_idem", lambda a, input_text=None: None)
    pr3 = _pr(mergeable=None)
    tp.fill_mergeable(pr3)
    assert getattr(pr3, "mergeable", None) is None
    # 取号双形统一(fix-round 1 Minor#1,与 execute_merge 同款): number-only
    # (真实 PRData 形态)与 pr_number-only(brief 替身形态)都不得踩
    # AttributeError——同文件同一对象不得两种取号形态
    monkeypatch.setattr(tl, "gh_json_idem",
                        lambda a, input_text=None: calls.append(list(a))
                        or True)
    pr4 = NS(number=5, mergeable=None)
    tp.fill_mergeable(pr4)
    assert calls[-1][:3] == ["pr", "view", "5"] and pr4.mergeable is True
    pr5 = NS(pr_number=8, mergeable=None)
    tp.fill_mergeable(pr5)
    assert calls[-1][:3] == ["pr", "view", "8"] and pr5.mergeable is True


# ============ Task 9: 精确归因自动 revert ============


def _red(head_sha, run_id=555, workflow="Sanitizer", url="u"):
    """scan_red_runs 真实键形: gh run list --json 的 camelCase 原始 dict
    (grep 实核: databaseId/workflowName/event/conclusion/headBranch/headSha/
    createdAt/url——brief 假设的 head_sha/html_url/title 蛇形键不存在,
    控制器裁定#2 以实码为准对齐,不得造键)。"""
    return {"databaseId": run_id, "workflowName": workflow, "event": "push",
            "conclusion": "failure", "headBranch": "main",
            "headSha": head_sha, "createdAt": "2026-11-13T00:00:00Z",
            "url": url}


def _merged_entry(merged_sha="deadbeef1234", pr_number=42, issue_number=7):
    """execute_merge 记账后的引擎条目(merged_sha 非空 = revert 归因主数据,
    人工 merge 永不写)。"""
    return _entry(status="DONE", issue_number=issue_number,
                  fix_branch="fix/issue-7", pr_number=pr_number,
                  merged_at="t", merged_sha=merged_sha)


def _no_gh(monkeypatch):
    """防意外触网哨: 本用例预期零 gh 调用——任何 gh 调用当场炸(fix-round 1
    起 creation path 新增 gh_json 双态查重搜索,两个入口一并钉死)。"""
    def boom(fn):
        def f(a, input_text=None):
            raise AssertionError(f"本用例不应触 {fn}: {a}")
        return f
    monkeypatch.setattr(tl, "gh_json", boom("gh_json"))
    monkeypatch.setattr(tl, "gh_json_idem", boom("gh_json_idem"))


def test_revert_attribution_creates_issue_for_merged_sha(monkeypatch, tmp_path):
    """命中臂(brief 步骤1主测): 红 run headSha ∈ 条目 merged_sha 集 → 建
    revert issue,返回号清单;正文含 revert-for 标记 + git revert -m 1
    指令 + 结束语契约,state 记 _last_revert_body(brief 原文键,裁定#4)。
    create_task_issue 真实签名 (title, body, labels)(裁定#1 grep 实核——
    brief 的 source/kind 参数不存在): source=manual 经无 source-marker
    正文由 reconcile 归类、kind=bug 经 agent-task+agent-bug 标签编码
    (task-loop:revert marker 不入 _SOURCE_MARKER_RES,来路自然是 manual)。
    双态 marker 查重(fix-round 1 Important#1)经 gh_json 桩返回空;二阶链
    探针经 gh_json_idem 桩返回普通标题(裁定#3 零触网)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    monkeypatch.setenv("TASKLOOP_REPO", "o/r")
    st = _mkstate()
    st["entries"] = [_merged_entry()]
    monkeypatch.setattr(tl, "gh_json", lambda a, input_text=None: [])
    gh_calls = []
    monkeypatch.setattr(tl, "gh_json_idem",
                        lambda a, input_text=None: gh_calls.append(list(a))
                        or "普通修复后的标题")
    made = []
    monkeypatch.setattr(tl, "create_task_issue",
                        lambda title, body, labels:
                        made.append((title, body, labels)) or 99)
    got = tp.revert_attribution(st, [_red("deadbeef1234")])
    # I2: 返回二元组 (issue 清单, 已吸收红 run id 集)——归因命中且 revert
    # issue 已建的红 run 由 cycle_once 从 triage 前向修复里滤除
    assert got == ([99], {555}) and len(made) == 1
    # I3: 建成后 state["revert_seen"] 记账(主查重,state 侧)
    assert st["revert_seen"] == ["deadbeef1234"]
    title, body, labels = made[0]
    assert "revert" in title.lower()
    assert title == "[revert] PR #42 自动 merge 后 main 红(Sanitizer)"
    assert labels == [tl.AGENT_LABEL_TASK, tl.AGENT_LABEL_BUG]
    assert tp.REVERT_FOR_FMT == "revert-for: PR #{pr} @ {sha}"
    assert "revert-for: PR #42 @ deadbeef1234" in body
    assert "git revert -m 1 deadbeef1234" in body
    assert "[Sanitizer](u)" in body
    assert "<!-- task-loop:revert:deadbeef1234 -->" in body
    assert "task-loop: done <fix_branch>" in body
    assert "revert-for: PR #42 @ deadbeef1234" in st["_last_revert_body"]
    # 二阶链探针的 gh 形锚: issue view <N> --json title --jq .title
    assert any(a[:3] == ["issue", "view", "7"]
               and a[a.index("--json") + 1] == "title"
               and a[a.index("--jq") + 1] == ".title" for a in gh_calls)


def test_revert_attribution_ignores_unrelated_red(monkeypatch):
    """归因不到 → 空清单(intake 照常建普通红 issue 的语义在 T10,不在本
    函数);miss 臂零 gh——无探针调用(防意外触网哨锁定)。"""
    _no_gh(monkeypatch)
    st = _mkstate()
    st["entries"] = []
    assert tp.revert_attribution(st, [_red("other")]) == ([], set())


def test_revert_attribution_set_requires_sha_and_pr(monkeypatch):
    """归因集只收 merged_sha ∧ pr_number 双非空条目: execute_merge 失败/
    sha 回读空(merged_sha=None)与无 PR 条目永不命中(裁决语境: None 自然
    不命中——revert-for 正文需要 PR 号,缺任一即不可归因)。"""
    _no_gh(monkeypatch)
    st = _mkstate()
    st["entries"] = [_merged_entry(merged_sha=None),
                     _merged_entry(pr_number=None, issue_number=None)]
    assert tp.revert_attribution(st, [_red("deadbeef1234")]) == ([], set())


def test_revert_attribution_second_order_chain_escalates(monkeypatch, capsys):
    """二阶链闸(防 revert 打乒乓球,spec §6 链深 ≤1): 命中条目的 issue
    标题以 [revert] 开头 = 该 PR 本身是 revert 产物 → 打印 escalation
    转人工(夜间报告可见),不建 issue、不记 _last_revert_body。查重搜索
    (gh_json 桩空)在闸前,先证「本 merge 无既有 revert issue」;探针走
    tl.issue_title_starts(经 gh_json_idem 桩,裁定#3)。"""
    monkeypatch.setenv("TASKLOOP_REPO", "o/r")
    st = _mkstate()
    st["entries"] = [_merged_entry()]
    monkeypatch.setattr(tl, "gh_json", lambda a, input_text=None: [])
    monkeypatch.setattr(tl, "gh_json_idem",
                        lambda a, input_text=None:
                        "[revert] PR #40 自动 merge 后 main 红(Sanitizer)")
    made = []
    monkeypatch.setattr(tl, "create_task_issue",
                        lambda title, body, labels: made.append(title) or 99)
    assert tp.revert_attribution(st, [_red("deadbeef1234")]) == ([], set())
    assert made == [] and "_last_revert_body" not in st \
        and "revert_seen" not in st
    out = capsys.readouterr().out
    assert "本身是 revert 产物" in out and "转人工" in out


def test_revert_attribution_title_probe_failure_escalates(monkeypatch, capsys):
    """二阶链不可判(探针 GhError)→ 宁严勿松转人工: [warn] 留痕、不建
    issue——红 run 仍由 intake(T10)建普通红 issue 前向修复,零丢失;
    brief 的「本身是 revert 产物」提示只在确认命中时打印,不可判臂用
    诚实的 [warn],不冒充命中(placeholder honesty)。"""
    monkeypatch.setenv("TASKLOOP_REPO", "o/r")
    st = _mkstate()
    st["entries"] = [_merged_entry()]
    monkeypatch.setattr(tl, "gh_json", lambda a, input_text=None: [])

    def boom(a, input_text=None):
        raise tl.GhError("gh issue view … 失败(exit 1): HTTP 502")
    monkeypatch.setattr(tl, "gh_json_idem", boom)
    made = []
    monkeypatch.setattr(tl, "create_task_issue",
                        lambda title, body, labels: made.append(title) or 99)
    assert tp.revert_attribution(st, [_red("deadbeef1234")]) == ([], set())
    assert made == []
    out = capsys.readouterr().out
    assert "[warn]" in out and "二阶链不可判" in out
    assert "本身是 revert 产物" not in out


def test_revert_attribution_dry_run_only_prints(monkeypatch, capsys):
    """dry_run: 只打印拟建动作——零建 issue、零 _last_revert_body/revert_seen
    记账(不吸收进 state);但 absorbed_run_ids 已含该 run(拟建输出即真实
    将建集——真实跑该 run 会被吸收,cycle_once dry 预演口径一致)。"""
    monkeypatch.setenv("TASKLOOP_REPO", "o/r")
    st = _mkstate()
    st["entries"] = [_merged_entry()]
    monkeypatch.setattr(tl, "gh_json", lambda a, input_text=None: [])
    monkeypatch.setattr(tl, "gh_json_idem", lambda a, input_text=None: "普通")
    made = []
    monkeypatch.setattr(tl, "create_task_issue",
                        lambda title, body, labels: made.append(title) or 99)
    assert tp.revert_attribution(st, [_red("deadbeef1234")],
                                 dry_run=True) == ([], {555})
    assert made == [] and "_last_revert_body" not in st \
        and "revert_seen" not in st
    out = capsys.readouterr().out
    assert "[dry-run] 将建 revert issue" in out
    assert "deadbeef1234" in out


def test_revert_attribution_dedups_same_merge_within_call(monkeypatch, capsys):
    """同批内同一 merge 只建一个 revert issue: 多 workflow 同 sha 皆红是
    常态(同一坏 main commit 的 Sanitizer/Multi-OS 同时红),revert 的对象
    是坏 merge 不是红 run——不去重则双 agent 同 revert 竞态。第二个红 run
    打印去重留痕(brief 未列的补墙,报告注明),前缀 [revert-dedup] 与
    二阶链升级提示 [revert] 分离(顺手修#1——T10 夜报 grep "revert" 时
    去重噪声不得混入人工升级信号);跨调用查重见双态 marker 幂等测。"""
    monkeypatch.setenv("TASKLOOP_REPO", "o/r")
    st = _mkstate()
    st["entries"] = [_merged_entry()]
    monkeypatch.setattr(tl, "gh_json", lambda a, input_text=None: [])
    monkeypatch.setattr(tl, "gh_json_idem", lambda a, input_text=None: "普通")
    made = []
    monkeypatch.setattr(tl, "create_task_issue",
                        lambda title, body, labels: made.append(title) or 99)
    got = tp.revert_attribution(st, [
        _red("deadbeef1234", run_id=555),
        _red("deadbeef1234", run_id=556, workflow="Multi-OS Verify",
             url="u2")])
    # 同 merge 的两个红 run 都被吸收(revert issue 覆盖整个坏 merge——
    # 不吸收则 run 556 仍会走 triage 前向修复,与 revert 双 agent 互踩)
    assert got == ([99], {555, 556}) and len(made) == 1
    out = capsys.readouterr().out
    assert "[revert-dedup]" in out and "去重" in out


def test_revert_attribution_cross_call_marker_idempotent(monkeypatch, capsys):
    """跨调用 marker 幂等(fix-round 1 Important#1 + I3 裸 sha 查询词):
    LOOKBACK_DAYS=3 窗内坏 merge 未修时红 run 每轮重见,第二次调用必对
    同一 merged_sha 重复建 issue → 双 agent 同 revert 竞态/attempts 燃烧/
    假性 ESCALATED。I3 双层查重: state revert_seen 是主闸(建成即记,下一
    轮先命中,零 gh);gh 双态搜索是 state 丢失/重启后的兜底闸——查询词改
    **裸 sha**(正文 revert-for: 纯文本必被索引;HTML 注释 marker 有已知
    正文索引漏命中特征),Python 侧完整 marker 复核保留(宽松命中干扰项
    滤掉)。closed 命中兑现「人工关闭的 revert issue 不重建」的人工接管
    语义(检视#35 同一缺陷类)。"""
    monkeypatch.setenv("TASKLOOP_REPO", "o/r")
    st = _mkstate()
    st["entries"] = [_merged_entry()]
    monkeypatch.setattr(tl, "gh_json_idem", lambda a, input_text=None: "普通")
    searches = []
    hit_closed = []          # 测试在两次调用之间翻转: closed 态命中

    def fake_search(args, input_text=None):
        searches.append(list(args))
        if args[args.index("--state") + 1] == "closed" and hit_closed:
            return [{"number": 101, "body": "宽松命中但正文无完整 marker"},
                    {"number": 100,
                     "body": "x\n<!-- task-loop:revert:deadbeef1234 -->\n"}]
        return []
    monkeypatch.setattr(tl, "gh_json", fake_search)
    made = []
    monkeypatch.setattr(tl, "create_task_issue",
                        lambda title, body, labels: made.append(title) or 99)
    # 第一次调用: 双态皆空 → 建 issue + revert_seen 记账
    assert tp.revert_attribution(st, [_red("deadbeef1234")]) == ([99], {555})
    assert len(made) == 1
    assert st["revert_seen"] == ["deadbeef1234"]
    # 第二次调用前清 revert_seen(模拟重启/state 丢失)——兜底 gh 搜索闸
    # 才被真正考察;state 完好时第二轮零 gh(主闸测试另行覆盖)
    del st["revert_seen"]
    # 第二次调用(红 run 重见): open 空、closed 命中 → 零新建、run 被吸收
    hit_closed.append(1)
    assert tp.revert_attribution(st, [_red("deadbeef1234")]) == ([], {555})
    assert len(made) == 1                          # create 桩零新增调用
    out = capsys.readouterr().out
    assert "[revert-dedup]" in out and "不重建" in out
    # gh search 形锚(先例移植): 双态逐查 + --repo + --json number,body +
    # 裸 sha 查询词(I3: 不再押 HTML 注释 marker 的正文索引;完整 marker
    # 复核在 Python 侧)
    states = [a[a.index("--state") + 1] for a in searches]
    assert states[:2] == ["open", "closed"]        # 第一轮即双态
    for a in searches:
        assert a[:2] == ["search", "issues"]
        assert a[a.index("--repo") + 1] == "o/r"
        assert a[a.index("--json") + 1] == "number,body"
        assert "deadbeef1234" in a                 # 裸 sha 查询词
        assert "task-loop:revert:deadbeef1234" not in a   # 非完整 marker 形态


def test_revert_attribution_revert_seen_primary_dedup(monkeypatch, capsys):
    """I3 主查重: state["revert_seen"] 含该 sha(上一轮建成记账)→ 本轮
    查重**先看 state 再 gh 搜索**——state 命中零 gh 调用、零建 issue、
    run 被吸收(镜像 audit 的 audit_seen 双源设计,state 是第一道闸,
    gh body marker 搜索是 state 丢失后的兜底)。"""
    monkeypatch.setenv("TASKLOOP_REPO", "o/r")
    st = _mkstate()
    st["entries"] = [_merged_entry()]
    st["revert_seen"] = ["deadbeef1234"]
    _no_gh(monkeypatch)          # 任何 gh 调用当场炸——state 命中必零 gh
    made = []
    monkeypatch.setattr(tl, "create_task_issue",
                        lambda title, body, labels: made.append(title) or 99)
    assert tp.revert_attribution(st, [_red("deadbeef1234")]) == ([], {555})
    assert made == []
    out = capsys.readouterr().out
    assert "revert_seen" in out and "跳过" in out


def test_revert_attribution_marker_search_failure_escalates(monkeypatch, capsys):
    """查重搜索 GhError → 幂等不可证,宁严勿松转人工: [warn] 留痕、不建
    issue、不记 _last_revert_body、**不吸收**(红 run 仍有 T10 intake 普通
    红 issue 前向修复兜底——误建的代价(双 agent 同 revert 竞态/attempts
    燃烧)大于漏建,与二阶链不可判臂同向)。"""
    monkeypatch.setenv("TASKLOOP_REPO", "o/r")
    st = _mkstate()
    st["entries"] = [_merged_entry()]

    def boom(a, input_text=None):
        raise tl.GhError("gh search issues … 失败(exit 1): HTTP 502")
    monkeypatch.setattr(tl, "gh_json", boom)
    made = []
    monkeypatch.setattr(tl, "create_task_issue",
                        lambda title, body, labels: made.append(title) or 99)
    assert tp.revert_attribution(st, [_red("deadbeef1234")]) == ([], set())
    assert made == [] and "_last_revert_body" not in st
    out = capsys.readouterr().out
    assert "[warn]" in out and "幂等不可证" in out


def test_issue_title_starts_probe(monkeypatch):
    """issue_title_starts 助手(brief: task_loop 无现成助手,令补的小函数;
    裁定#3 经 tl.gh_json_idem 桩测试): gh issue view <N> --repo <slug>
    --json title --jq .title 幂等读,str 前缀判定;gh 空输出 None(gh_json
    契约)→ False(标题未知不可证前缀);GhError 原样上抛——降级语义归调用方
    (与 issue_state 同款契约,不掀翻整链)。"""
    monkeypatch.setenv("TASKLOOP_REPO", "o/r")
    calls = []
    monkeypatch.setattr(tl, "gh_json_idem",
                        lambda a, input_text=None: calls.append(list(a))
                        or "[revert] PR #40 自动 merge 后 main 红(Sanitizer)")
    assert tl.issue_title_starts(7, "[revert]") is True
    a = calls[0]
    assert a[:3] == ["issue", "view", "7"]
    assert a[a.index("--repo") + 1] == "o/r"
    assert a[a.index("--json") + 1] == "title"
    assert a[a.index("--jq") + 1] == ".title"
    assert tl.issue_title_starts(7, "[REVERT]") is False   # 前缀精确,不归一
    monkeypatch.setattr(tl, "gh_json_idem", lambda a, input_text=None: None)
    assert tl.issue_title_starts(7, "[revert]") is False

    def boom(a, input_text=None):
        raise tl.GhError("gh issue view … 失败(exit 1): HTTP 502")
    monkeypatch.setattr(tl, "gh_json_idem", boom)
    with pytest.raises(tl.GhError):
        tl.issue_title_starts(7, "[revert]")


# ============ Task 10: 编排器主循环 + 软停 + 报告 + CLI 接线 ============


def _patch_cycle_offline(monkeypatch, tmp_path):
    """cycle_once/run_pipeline 离线夹具(裁定#9 探活纪律): 全 tl/tp 进程与
    gh 接缝打桩,零触网零 spawn;state 目录重定向 tmp。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    monkeypatch.setattr(tl, "scan_red_runs", lambda: [])
    monkeypatch.setattr(tl, "triage_new_red_runs",
                        lambda s, r, dry_run=False: [])
    monkeypatch.setattr(tl, "reap_or_advance",
                        lambda s, dry_run=False: None)
    monkeypatch.setattr(tl, "reconcile_issues",
                        lambda s, dry_run=False: {})
    monkeypatch.setattr(tl, "heal_clone", lambda s, dry_run=False: None)
    monkeypatch.setattr(tl, "lane_b_advance", lambda s, dry_run=False: None)
    monkeypatch.setattr(tp, "needs_review_list", lambda: [])
    monkeypatch.setattr(tp, "open_docs_prs", lambda: [])
    monkeypatch.setattr(tp, "audit_line_running", lambda st: True)


def test_cycle_once_orchestration_order(monkeypatch, tmp_path):
    """全接缝打桩,断言 cycle_once 调齐 8 步且互不炸(brief 步骤1主测:
    intake→janitor→reap→reconcile→heal→泳道1→泳道2→lane_b→audit→
    merge_sweep 的顺序载荷,裁定#2 不可破坏)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    order = []
    monkeypatch.setattr(tl, "scan_red_runs", lambda: [])
    monkeypatch.setattr(tl, "triage_new_red_runs",
                        lambda s, r, dry_run=False: order.append("intake"))
    monkeypatch.setattr(tl, "reap_or_advance",
                        lambda s, dry_run=False: order.append("reap"))
    monkeypatch.setattr(tl, "reconcile_issues",
                        lambda s, dry_run=False: order.append("reconcile") or set())
    monkeypatch.setattr(tl, "heal_clone",
                        lambda s, dry_run=False: order.append("heal"))
    monkeypatch.setattr(tl, "lane_b_advance",
                        lambda s, dry_run=False: order.append("laneb"))
    monkeypatch.setattr(tp, "needs_review_list", lambda: [])
    monkeypatch.setattr(tp, "open_docs_prs", lambda: [])
    monkeypatch.setattr(tp, "audit_line_running", lambda st: True)
    st = _mkstate()
    tp.cycle_once(st, False, [], datetime.now())
    assert order == ["intake", "reap", "reconcile", "heal", "laneb"]
    assert tp.SLOT_KINDS["issue_fix"] == 2


def test_cycle_once_lane1_dispatch_writeback_and_save(monkeypatch, tmp_path):
    """泳道1 派发记账必须回写 state["entries"] 再落盘——Entry 是解析副本,
    不回写则 save 落的是旧 dict,DISPATCHED/PID/attempts 全丢 = 每轮重派
    (scan 检视#6 / Task 4 R2 同款根因,brief 的 cycle_once 漏了回写)。"""
    _patch_cycle_offline(monkeypatch, tmp_path)
    monkeypatch.setattr(tl, "_dispatch_gate_ok", lambda e, ol: True)
    dispatched = []

    def fake_dispatch(e, s, dry_run=False, **kw):
        e.status = "DISPATCHED"
        e.agent_pid = 4242
        e.attempts += 1
        tp.slot_acquire(s, "issue_fix",
                        f"issue-{e.issue_number or e.run_id}",
                        4242, 4242, "/l", None, "t", "b")
        dispatched.append(e.issue_number)
        return 4242
    monkeypatch.setattr(tl, "dispatch_agent", fake_dispatch)
    st = _mkstate()
    st["entries"] = [_entry(status="QUEUED", agent_pid=None)]
    tp.cycle_once(st, False, [], datetime.now())
    assert dispatched == [5]
    disk = tl.StateStore().load()          # 派发记账即时落盘且是新 dict
    assert disk["entries"][0]["status"] == "DISPATCHED"
    assert disk["entries"][0]["agent_pid"] == 4242
    assert disk["entries"][0]["attempts"] == 3


def test_cycle_once_lane1_includes_requeued_dispatched(monkeypatch, tmp_path):
    """泳道1 取材必须含「DISPATCHED 且无 PID」(dead/timeout 经
    _requeue_or_escalate 保持 DISPATCHED 待下一轮重派)——brief 只扫 QUEUED
    会让重派条目永久滞留(scan 旧编排的重派扫消化这一群,薄壳等价性要求
    cycle_once 覆盖同一取材)。"""
    _patch_cycle_offline(monkeypatch, tmp_path)
    monkeypatch.setattr(tl, "_dispatch_gate_ok", lambda e, ol: True)
    seen = []

    def fake_dispatch(e, s, dry_run=False, **kw):
        seen.append(e.status)
        return 4242
    monkeypatch.setattr(tl, "dispatch_agent", fake_dispatch)
    st = _mkstate()
    st["entries"] = [_entry(status="DISPATCHED", agent_pid=None, attempts=1)]
    tp.cycle_once(st, False, [], datetime.now())
    assert seen == ["DISPATCHED"]          # 重派条目被取材


def test_cycle_once_lane2_paused_blocks_and_dry_run_no_spawn(monkeypatch,
                                                             tmp_path):
    """泳道2 接线: dry-run 只打印意图零派发(dry-run 零 spawn 契约);
    PAUSED 熔断拦检视派发(紧急停机覆盖全部派发,spec §1 人工保留点——
    dispatch_agent/run_audit 自带闸,检视线在编排层拦);正常路径真派。"""
    _patch_cycle_offline(monkeypatch, tmp_path)
    monkeypatch.setattr(tp, "needs_review_list", lambda: [(640, "h1")])
    calls = []
    monkeypatch.setattr(tp, "dispatch_review",
                        lambda st, num, head: calls.append(num) or 4242)
    st = _mkstate()
    tp.cycle_once(st, True, [], datetime.now())          # dry: 零派发
    assert calls == []
    monkeypatch.setattr(tl, "_paused", lambda: True)
    tp.cycle_once(st, False, [], datetime.now())          # PAUSED: 拦派
    assert calls == []
    monkeypatch.setattr(tl, "_paused", lambda: False)
    tp.cycle_once(st, False, [], datetime.now())          # 正常: 泳道2 派发
    assert calls == [640]


def test_cycle_once_collects_revert_and_slot_events(monkeypatch, tmp_path):
    """事件收集(裁定#4): revert_attribution 返回 (issue 清单, 吸收集)
    二元组,issue 清单记 {"kind":"revert-issue"};janitor 回收清单记
    {"kind":"slot-released"}——夜间报告的数据源(revert 转人工信号本身在
    pipeline.log/stdout,正文只引路径)。"""
    _patch_cycle_offline(monkeypatch, tmp_path)
    monkeypatch.setattr(tp, "revert_attribution",
                        lambda st, runs, dry_run=False: ([99], set()))
    monkeypatch.setattr(tl, "agent_alive", lambda pid: False)
    monkeypatch.setattr(tl, "worktree_cleanup", lambda p: None)
    monkeypatch.setattr(tp, "boot_id", lambda: "new-boot")
    st = _mkstate()
    tp.slot_acquire(st, "issue_fix", "issue-1", 999999901, 0, "/l", "/w",
                    "t", "old-boot")
    events = []
    tp.cycle_once(st, False, events, datetime.now())
    assert {"kind": "revert-issue", "issues": [99]} in events
    assert any(ev["kind"] == "slot-released" for ev in events)


def test_cycle_once_absorbed_runs_skip_forward_triage(monkeypatch, tmp_path):
    """I2 双建消解(终审): 归因命中的红 run 若不被吸收,revert_attribution
    建了 revert issue 之后 triage_new_red_runs 又对同一 run 建「修复」
    issue——双 agent 对同一故障互相冲突。cycle_once 必须把 absorbed 集里
    的 run 从传给 triage 的清单里滤除(未吸收的照旧前向修复,T9 兜底)。"""
    _patch_cycle_offline(monkeypatch, tmp_path)
    monkeypatch.setattr(tl, "scan_red_runs",
                        lambda: [_red("deadbeef1234", run_id=555),
                                 _red("other", run_id=556)])
    monkeypatch.setattr(tp, "revert_attribution",
                        lambda st, runs, dry_run=False: ([99], {555}))
    seen_runs = []
    monkeypatch.setattr(
        tl, "triage_new_red_runs",
        lambda s, r, dry_run=False: seen_runs.append(list(r)) or [])
    st = _mkstate()
    events = []
    tp.cycle_once(st, False, events, datetime.now())
    assert seen_runs == [[_red("other", run_id=556)]]   # run 555 被滤除
    assert {"kind": "revert-issue", "issues": [99]} in events
    # 吸收集为空时全量透传(无 revert 命中的常态夜)
    seen_runs.clear()
    monkeypatch.setattr(tp, "revert_attribution",
                        lambda st, runs, dry_run=False: ([], set()))
    tp.cycle_once(st, False, events, datetime.now())
    assert len(seen_runs[0]) == 2


def test_write_report(tmp_path, monkeypatch):
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    st = _mkstate()
    p = tp.write_report(st, [{"kind": "merge", "pr": 42}], False)
    assert os.path.exists(p) and st["last_report"] == os.path.basename(p)
    # 裁定#4: 正文引 pipeline.log 路径(人工介入信号见 stdout 归档)
    with open(p, encoding="utf-8") as f:
        body = f.read()
    assert "pipeline.log" in body and "人工介入信号" in body
    assert "- {'kind': 'merge', 'pr': 42}" in body


def test_write_report_dry_run_zero_write(tmp_path, monkeypatch):
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    st = _mkstate()
    p = tp.write_report(st, [{"kind": "merge", "pr": 42}], True)
    assert not os.path.exists(p)          # dry: 报告不写
    assert "last_report" not in st        # state 不记账


def test_pipeline_lock_second_instance(tmp_path, monkeypatch):
    """R4(锁的意义测试,brief 原形态作废): 非 dry run_pipeline + 外部先持
    pipeline.lock → 退出码 0,且锁检查先于一切副作用(state 未读未写、
    零 gh、零迁移);dry-run 保持零本地写契约不碰锁。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    os.makedirs(tmp_path / "st")
    fd = os.open(os.path.join(tmp_path / "st", tp.PIPELINE_LOCK_NAME),
                 os.O_CREAT | os.O_RDWR)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        assert tp.run_pipeline() == 0    # 拿不到锁 → 退出码 0
        assert not os.path.exists(os.path.join(tmp_path / "st", "state.json"))
    finally:
        fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)


def test_run_pipeline_dry_run_zero_local_writes(monkeypatch, tmp_path,
                                                capsys):
    """dry-run 三处零写(裁定自查清单): 不建目录/不碰锁/state 不落盘/
    报告不写;单轮预演退出 0。"""
    _patch_cycle_offline(monkeypatch, tmp_path)
    assert tp.run_pipeline(dry_run=True) == 0
    assert not os.path.exists(tmp_path / "st")     # 连状态目录都没建
    assert "[dry-run] pipeline 全流程预演完成,零副作用" \
        in capsys.readouterr().out


def test_run_pipeline_window_loop_soft_stop_report(monkeypatch, tmp_path):
    """非 dry 全流程: 前奏(迁移/标签/清检视每晚计数/启动对账)→ 窗口循环
    (每轮 cycle+落盘)→ 出窗软停 → 报告落盘 + last_report + status 摘要
    数据。in_window 桩两态: 一次 True(单轮)一次 False(出窗)。"""
    _patch_cycle_offline(monkeypatch, tmp_path)
    monkeypatch.setattr(tl, "_migrate_legacy_state_dir", lambda: None)
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    monkeypatch.setattr(tp.time, "sleep", lambda s: None)
    flags = iter([True, False])
    monkeypatch.setattr(tp, "in_window",
                        lambda now, s=23, e=9: next(flags))
    soft = []
    monkeypatch.setattr(tp, "soft_stop",
                        lambda st, grace_sec=None: soft.append(st))
    os.makedirs(tmp_path / "st")
    tl.StateStore().save({"entries": [], "auth_failure_streak": 0,
                          "review_tries": {"640:h1": 2}})
    assert tp.run_pipeline() == 0
    assert len(soft) == 1                          # 出窗软停恰一次
    assert soft[0]["review_tries"] == {}           # 启动清零每晚检视计数
    disk = tl.StateStore().load()
    report = os.path.join(tmp_path / "st", disk["last_report"])
    assert os.path.exists(report)                  # 报告落盘
    assert disk["last_report"] == os.path.basename(report)


def test_run_pipeline_cycle_gherror_digested(monkeypatch, tmp_path):
    """窗口循环 GhError 单轮消化不炸整窗: scan_red_runs 持续 GhError →
    仍写报告/落盘/软停,退出 0。"""
    _patch_cycle_offline(monkeypatch, tmp_path)
    monkeypatch.setattr(tl, "_migrate_legacy_state_dir", lambda: None)
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    monkeypatch.setattr(tp.time, "sleep", lambda s: None)

    def boom():
        raise tl.GhError("gh run list … 失败(exit 1): HTTP 502")
    monkeypatch.setattr(tl, "scan_red_runs", boom)
    flags = iter([True, True, False])
    monkeypatch.setattr(tp, "in_window",
                        lambda now, s=23, e=9: next(flags))
    monkeypatch.setattr(tp, "soft_stop",
                        lambda st, grace_sec=None: None)
    assert tp.run_pipeline() == 0
    disk = tl.StateStore().load()
    assert os.path.exists(os.path.join(tmp_path / "st", disk["last_report"]))


def test_soft_stop_grace_then_kill_no_attempt(monkeypatch, tmp_path):
    """软停: 超窗活 agent 击杀(SIGTERM→SIGKILL 补刀两段);条目干净回队
    不计 attempts/rounds(PR #611 假性升级教训);worktree 清、槽清;
    审计线同收;记账回写 state dict(解析副本→to_dict,不回写则落旧 dict)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    monkeypatch.setattr(tp.time, "sleep", lambda s: None)
    monkeypatch.setattr(tl, "agent_alive", lambda pid: True)
    killed = []
    monkeypatch.setattr(os, "killpg",
                        lambda pid, sig: killed.append((pid, sig)))
    cleaned = []
    monkeypatch.setattr(tl, "worktree_cleanup", lambda p: cleaned.append(p))
    st = _mkstate()
    st["entries"] = [
        _entry(status="BOT_FIX", pr_number=88, rounds=2, attempts=1,
               agent_pid=4242, worktree="/wt/pr-88"),
        _entry(status="DISPATCHED", attempts=1, agent_pid=4243,
               worktree="/wt/issue-5")]
    tp.slot_acquire(st, "pr_repair", "pr-88", 4242, 4242, "/l", "/wt/pr-88",
                    "t", "b")
    tp.slot_acquire(st, "issue_fix", "issue-5", 4243, 4243, "/l",
                    "/wt/issue-5", "t", "b")
    tp.slot_acquire(st, "audit", "audit-nightly", 4244, 4244, "/l", None,
                    "t", "b")
    tp.soft_stop(st, grace_sec=0)
    es = tl.StateStore.entries(st)
    assert es[0].status == "PR_OPEN" and es[0].attempts == 1 \
        and es[0].rounds == 2 and es[0].agent_pid is None
    assert es[1].status == "QUEUED" and es[1].attempts == 1 \
        and es[1].agent_pid is None
    assert (4242, 15) in killed and (4243, 15) in killed \
        and (4244, 15) in killed          # SIGTERM 三槽(含 audit 线)
    assert (4242, 9) in killed            # 仍活者 SIGKILL 补刀
    assert sorted(cleaned) == ["/wt/issue-5", "/wt/pr-88"]
    assert not st.get("slots", {}).get("pr_repair")
    assert not st.get("slots", {}).get("issue_fix")
    assert not st.get("slots", {}).get("audit")
    assert st["entries"][0]["status"] == "PR_OPEN"   # 回写后的 dict


def test_soft_stop_all_exited_no_kill(monkeypatch, tmp_path):
    """自然退场臂: 全部 agent 已退 → 零击杀;槽/worktree 仍清(易耗品),
    条目不动(attempts 不变、状态不冒充软停语义——真实结局归下一轮/次夜
    reap 记账)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    monkeypatch.setattr(tl, "agent_alive", lambda pid: False)
    killed = []
    monkeypatch.setattr(os, "killpg",
                        lambda pid, sig: killed.append((pid, sig)))
    cleaned = []
    monkeypatch.setattr(tl, "worktree_cleanup", lambda p: cleaned.append(p))
    st = _mkstate()
    st["entries"] = [_entry(status="DISPATCHED", agent_pid=4243,
                           worktree="/wt/issue-5")]
    tp.slot_acquire(st, "issue_fix", "issue-5", 4243, 4243, "/l",
                    "/wt/issue-5", "t", "b")
    tp.soft_stop(st, grace_sec=1800)
    assert killed == []                   # 自然退场不击杀
    assert cleaned == ["/wt/issue-5"]      # worktree 照清
    assert tp.slot_get(st, "issue_fix", "issue-5") is None
    e = tl.StateStore.entries(st)[0]
    assert e.status == "DISPATCHED" and e.attempts == 2 \
        and e.agent_pid == 4243           # 条目零动,归 reap 记账


# ============ Task 10: merge 门扫接线 ============


def _patch_merge_offline(monkeypatch, tmp_path):
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    monkeypatch.setattr(tl, "repo_slug", lambda: "o/r")
    monkeypatch.setattr(tl, "agent_alive", lambda pid: False)
    monkeypatch.setattr(tp, "open_docs_prs", lambda: [])


def test_merge_sweep_engine_wiring_fill_gate_execute(monkeypatch, tmp_path):
    """引擎条目接线序(裁定#3): fetch_pr → fill_mergeable → pr_files →
    evaluate_merge_gate → execute_merge;merge 事件入 events;记账即时
    回写落盘(merged_sha 是 revert 归因主数据)。"""
    _patch_merge_offline(monkeypatch, tmp_path)
    seq = []
    monkeypatch.setattr(tl.pr_loop, "fetch_pr",
                        lambda slug, n: seq.append(("fetch", n))
                        or NS(number=n, mergeable=None))
    monkeypatch.setattr(tp, "fill_mergeable",
                        lambda pr: seq.append("fill") or
                        setattr(pr, "mergeable", True))
    monkeypatch.setattr(tl, "pr_files",
                        lambda n: seq.append("files") or ["a.c"])
    monkeypatch.setattr(tp, "evaluate_merge_gate",
                        lambda st, e, pr, files, now, no_auto_merge:
                        seq.append("gate") or tp.GateResult(True, False, ["ok"]))
    monkeypatch.setattr(tp, "execute_merge",
                        lambda st, e, pr, gr, dry_run=False:
                        seq.append("exec") or "sha42")
    st = _mkstate()
    st["entries"] = [_entry(status="PR_OPEN", pr_number=88,
                           fix_branch="fix/issue-7")]
    events = []
    tp.merge_sweep(st, events, False, datetime.now(timezone.utc))
    assert seq == [("fetch", 88), "fill", "files", "gate", "exec"]
    assert events == [{"kind": "merge", "pr": 88, "sha": "sha42"}]


def test_merge_sweep_skips_pr_with_live_repair_agent(monkeypatch, tmp_path):
    """repair 槽活 agent 在跑的 PR 跳过(brief 原文): fetch 都不发生,
    等修复 agent 退场(同轮 lane_b 已把它保持 BOT_FIX/PR_OPEN)。"""
    _patch_merge_offline(monkeypatch, tmp_path)
    monkeypatch.setattr(tl, "agent_alive", lambda pid: True)
    fetched = []
    monkeypatch.setattr(tl.pr_loop, "fetch_pr",
                        lambda slug, n: fetched.append(n) or NS(number=n))
    st = _mkstate()
    st["entries"] = [_entry(status="PR_OPEN", pr_number=88,
                           fix_branch="fix/issue-7")]
    tp.slot_acquire(st, "pr_repair", "pr-88", 4242, 4242, "/l", None,
                    "t", "b")
    events = []
    tp.merge_sweep(st, events, False, datetime.now(timezone.utc))
    assert fetched == [] and events == []


def test_merge_sweep_fetch_gherror_digested(monkeypatch, tmp_path):
    """单 PR fetch GhError 只跳过该 PR 不炸整扫(与 lane_b 单条目降级同口径;
    brief 只给 docs 循环 try,引擎循环同样需要)。"""
    _patch_merge_offline(monkeypatch, tmp_path)

    def boom(slug, n):
        raise tl.GhError("gh pr view … 失败(exit 1): HTTP 502")
    monkeypatch.setattr(tl.pr_loop, "fetch_pr", boom)
    monkeypatch.setattr(tl, "pr_files", lambda n: ["a.c"])
    monkeypatch.setattr(tp, "fill_mergeable", lambda pr: None)
    monkeypatch.setattr(tp, "evaluate_merge_gate",
                        lambda *a: (_ for _ in ()).throw(
                            AssertionError("fetch 失败不应过门")))
    st = _mkstate()
    st["entries"] = [_entry(status="PR_OPEN", pr_number=88,
                           fix_branch="fix/issue-7")]
    events = []
    tp.merge_sweep(st, events, False, datetime.now(timezone.utc))   # 不炸
    assert events == []


def test_merge_sweep_gate_reject_upsert(monkeypatch, tmp_path):
    """门拒事件同 (kind,pr) 去重-更新: merge 门每 PIPELINE_CYCLE_SEC 轮询,
    夜报数据源不做重复账——二扫同 PR 拒因演化时报告反映最新判据。"""
    _patch_merge_offline(monkeypatch, tmp_path)
    monkeypatch.setattr(tl.pr_loop, "fetch_pr",
                        lambda slug, n: NS(number=n))
    monkeypatch.setattr(tp, "fill_mergeable", lambda pr: None)
    monkeypatch.setattr(tl, "pr_files", lambda n: ["a.c"])
    verdicts = iter([tp.GateResult(False, False, ["classify=READY_TO_FIX"]),
                     tp.GateResult(False, False,
                                   ["required check 缺席: ['zizmor']"])])
    monkeypatch.setattr(tp, "evaluate_merge_gate", lambda *a: next(verdicts))
    st = _mkstate()
    st["entries"] = [_entry(status="PR_OPEN", pr_number=88,
                           fix_branch="fix/issue-7")]
    events = []
    now = datetime.now(timezone.utc)
    tp.merge_sweep(st, events, False, now)
    tp.merge_sweep(st, events, False, now)
    assert len(events) == 1
    assert events[0] == {"kind": "gate-reject", "pr": 88,
                         "reasons": ["required check 缺席: ['zizmor']"]}


def test_merge_sweep_no_auto_merge_would_merge_report(monkeypatch, tmp_path):
    """NO_AUTO_MERGE 灰度纯报告模式(裁定#3): no_auto_merge 只传参、不用它跳过 docs
    循环——熔断拒后复评一次不带熔断,复评过门记 would-merge 事件
    (spec §11.4「本应 merge+门槛证据」),绝不执行 merge。"""
    _patch_merge_offline(monkeypatch, tmp_path)
    (tmp_path / "st").mkdir(parents=True)
    (tmp_path / "st" / tp.NO_AUTO_MERGE_NAME).write_text("")
    calls = []

    def gate(st, e, pr, files, now, no_auto_merge):
        calls.append(no_auto_merge)
        if no_auto_merge:
            return tp.GateResult(
                False, True, ["NO_AUTO_MERGE 熔断(灰度: 只报告)"])
        return tp.GateResult(
            True, True, ["docs 快道: 第5bot 唯一 + 300s 静默窗过"])
    monkeypatch.setattr(tp, "evaluate_merge_gate", gate)
    monkeypatch.setattr(tp, "execute_merge",
                        lambda *a, **kw: (_ for _ in ()).throw(
                            AssertionError("熔断期不得执行 merge")))
    monkeypatch.setattr(tp, "fill_mergeable", lambda pr: None)
    monkeypatch.setattr(tl.pr_loop, "fetch_pr",
                        lambda slug, n: NS(number=n))
    monkeypatch.setattr(tl, "pr_files", lambda n: ["docs/a.md"])
    monkeypatch.setattr(tp, "open_docs_prs",
                        lambda: [{"pr_number": 700, "title": "docs: x",
                                  "head": "h"}])
    st = _mkstate()
    events = []
    tp.merge_sweep(st, events, False, datetime.now(timezone.utc))
    assert calls == [True, False]          # 先带熔断评,拒后复评不带
    assert events == [{"kind": "would-merge", "pr": 700,
                       "reasons": ["docs 快道: 第5bot 唯一 + 300s 静默窗过"]}]


def test_merge_sweep_docs_filters_and_engine_pr_no_double(monkeypatch,
                                                           tmp_path):
    """docs 循环三滤: 引擎条目 PR 不双扫(引擎循环已评恰一次,防双重评估/
    双 merge)、WIP: 标题在 fetch 前拒(用户显式不同意当夜合入)、非
    docs-only 在门评前拒;docs 引擎 PR 不因 docs-only 双进 docs 循环。"""
    _patch_merge_offline(monkeypatch, tmp_path)
    fetched = []
    monkeypatch.setattr(tl.pr_loop, "fetch_pr",
                        lambda slug, n: fetched.append(n) or NS(number=n))
    monkeypatch.setattr(tp, "fill_mergeable", lambda pr: None)
    gated = []

    def gate(st, e, pr, files, now, no_auto_merge):
        gated.append((getattr(pr, "number", None), files))
        return tp.GateResult(False, False, ["拒"])
    monkeypatch.setattr(tp, "evaluate_merge_gate", gate)
    monkeypatch.setattr(tl, "pr_files",
                        lambda n: ["a.c"] if n == 702 else ["docs/a.md"])
    monkeypatch.setattr(tp, "open_docs_prs",
                        lambda: [{"pr_number": 88, "title": "docs 引擎 PR",
                                  "head": "h"},        # 引擎条目 → 不双扫
                                 {"pr_number": 701, "title": "WIP: 草稿",
                                  "head": "h"},        # WIP → fetch 前拒
                                 {"pr_number": 702, "title": "代码 PR",
                                  "head": "h"}])      # 非 docs-only → 门评前拒
    st = _mkstate()
    st["entries"] = [_entry(status="PR_OPEN", pr_number=88,
                           fix_branch="fix/issue-7")]
    events = []
    tp.merge_sweep(st, events, False, datetime.now(timezone.utc))
    # 引擎 88 仅引擎循环评估一次;701 WIP 零 fetch;702 fetch 后被 docs-only 拒
    assert fetched == [88, 702]
    assert gated == [(88, ["docs/a.md"])]
    assert events == [{"kind": "gate-reject", "pr": 88, "reasons": ["拒"]}]


def _patch_conflict_offline(monkeypatch, tmp_path):
    """I6 冲突计数用例的共通离线桩: merge_sweep 真走 evaluate_merge_gate
    (classify/checks 打桩,门本体不打)——被测对象是 conflict_nights 计数
    与升级,不是门槛矩阵。"""
    _patch_merge_offline(monkeypatch, tmp_path)
    monkeypatch.setattr(tl, "pr_files", lambda n: ["src/a.c"])
    monkeypatch.setattr(tp, "fill_mergeable", lambda pr: None)
    monkeypatch.setattr(tl, "comment_on_issue", lambda n, b: None)
    monkeypatch.setattr(tl, "gh_label_remove", lambda n, l: None)
    monkeypatch.setattr(tl, "gh_label_add", lambda n, l: None)


def test_pipeline_night_boundary():
    """I6 fix-round 2: 夜 = pipeline 夜(23:00 起跨午夜到 09:00),不是本地
    日历日——本地日期 00:00 翻转会错误地把一夜劈成两夜。23:30 与次日
    01:30 同属一夜(同标识);09:00 前的凌晨归昨晚;窗口外 09:00–23:00
    (仅手动 scan 会发生)归即将开始的一夜。"""
    assert tp._pipeline_night(datetime(2026, 11, 13, 23, 0)) == "2026-11-13"
    assert tp._pipeline_night(datetime(2026, 11, 13, 23, 30)) == "2026-11-13"
    assert tp._pipeline_night(datetime(2026, 11, 14, 0, 30)) == "2026-11-13"
    assert tp._pipeline_night(datetime(2026, 11, 14, 1, 30)) == "2026-11-13"
    assert tp._pipeline_night(datetime(2026, 11, 14, 8, 59)) == "2026-11-13"
    assert tp._pipeline_night(datetime(2026, 11, 14, 9, 0)) == "2026-11-14"
    assert tp._pipeline_night(datetime(2026, 11, 14, 22, 30)) == "2026-11-14"
    assert tp._pipeline_night(datetime(2026, 11, 14, 23, 0)) == "2026-11-14"
    assert tp._pipeline_night(datetime(2026, 11, 15, 1, 0)) == "2026-11-14"


def _patch_conflict_gate(monkeypatch, mergeable):
    """冲突用例的门依赖桩: classify=READY_TO_MERGE + checks 绿 + 指定
    mergeable——拒因只可能是(或不可能是)「与 main 冲突」。"""
    monkeypatch.setattr(tl.pr_loop, "classify_state",
                        lambda pr, q, now: "READY_TO_MERGE")
    monkeypatch.setattr(tp, "checks_ok", lambda pr, req, slug: (True, []))
    monkeypatch.setattr(tl.pr_loop, "fetch_pr",
                        lambda slug, n: NS(number=n, state="open", draft=False,
                                          fork=False, mergeable=mergeable))


def test_merge_sweep_conflict_same_night_counts_once(monkeypatch, tmp_path):
    """I6 fix-round 2(生产真节奏回归): run_pipeline 整夜持有内存 state、
    每 300s 一轮 cycle、merge_sweep 是尾步——冲突条目当夜会被评估上百次。
    旧实现每次含「冲突」拒因的门评都 +1,当夜第 2 轮 cycle(约 10 分钟)
    即假性 2 → 假性 ESCALATED,升级文案还谎称「已持续 2 晚」。夜边界
    锚定(_pipeline_night): 同一夜(23:30 → 23:40 → 跨午夜 00:30)只 +1
    一次,条目整夜保持 PR_OPEN。"""
    _patch_conflict_offline(monkeypatch, tmp_path)
    _patch_conflict_gate(monkeypatch, mergeable=False)
    st = _mkstate()
    st["entries"] = [_entry(status="PR_OPEN", pr_number=88,
                            fix_branch="fix/issue-7", agent_pid=None,
                            agent_log=None, conflict_nights=0)]
    events = []
    # 夜 2026-11-13 第 1 轮(23:30): 冲突拒 → 0→1 + 夜锚
    tp.merge_sweep(st, events, False, datetime(2026, 11, 13, 23, 30))
    e = tl.StateStore.entries(st)[0]
    assert e.status == "PR_OPEN" and e.conflict_nights == 1 \
        and e.last_conflict_night == "2026-11-13"
    assert events[-1]["kind"] == "gate-reject"
    # 同夜第 2 轮(23:40,300s 节奏)与跨午夜段(次日 00:30): 不再 +1、
    # 不升级(旧实现在此即假性 2 → ESCALATED,本测红)
    tp.merge_sweep(st, events, False, datetime(2026, 11, 13, 23, 40))
    tp.merge_sweep(st, events, False, datetime(2026, 11, 14, 0, 30))
    e2 = tl.StateStore.entries(st)[0]
    assert e2.status == "PR_OPEN" and e2.conflict_nights == 1
    assert e2.last_conflict_night == "2026-11-13"
    # 同夜再评仍逐轮记 gate-reject 事件(报告可见),只是计数不涨
    assert events[-1]["kind"] == "gate-reject"


def test_merge_sweep_conflict_second_night_escalates(monkeypatch, tmp_path,
                                                     capsys):
    """I6 fix-round 2(跨夜再 +1,第二夜首次评估即升级): 夜 1 的凌晨段
    (00:30,h<9 归昨晚)与夜 2 的开局(23:30)是真·跨夜对——第二夜首次
    评估 1→2 即 ESCALATED,升级文案为真(「已持续 2 晚」+ pipeline 夜
    口径,不再谎称);计数/夜锚经 state 写回跨夜存续(不写回则每轮从
    dict 旧值重新解析,永远停在 1)。"""
    _patch_conflict_offline(monkeypatch, tmp_path)
    _patch_conflict_gate(monkeypatch, mergeable=False)
    st = _mkstate()
    st["entries"] = [_entry(status="PR_OPEN", pr_number=88,
                            fix_branch="fix/issue-7", agent_pid=None,
                            agent_log=None, conflict_nights=0)]
    events = []
    # 夜 1 开局(11-13 23:30)与凌晨段(11-14 00:30)同夜: 只计 1
    tp.merge_sweep(st, events, False, datetime(2026, 11, 13, 23, 30))
    tp.merge_sweep(st, events, False, datetime(2026, 11, 14, 0, 30))
    e = tl.StateStore.entries(st)[0]
    assert e.status == "PR_OPEN" and e.conflict_nights == 1
    # 夜 2 首次评估(11-14 23:30): 1→2 → ESCALATED(文案真)
    tp.merge_sweep(st, events, False, datetime(2026, 11, 14, 23, 30))
    e2 = tl.StateStore.entries(st)[0]
    assert e2.status == "ESCALATED" and e2.conflict_nights == 2
    assert e2.last_conflict_night == "2026-11-14"
    out = capsys.readouterr().out
    assert "ESCALATED" in out and "持续 2 晚" in out \
        and "pipeline 夜" in out and "spec §5" in out


def test_merge_sweep_conflict_counter_resets(monkeypatch, tmp_path):
    """I6 fix-round 2 清零双字段: 非冲突 gate-reject / 过门 → conflict_
    nights=0 ∧ last_conflict_night=None **双清**——只清计数不清夜锚,
    「清零后同夜又见冲突」会被同夜锚错误吞掉(不 +1);双清后同夜再冲突
    正确地按新连击第 1 夜起步。"""
    _patch_conflict_offline(monkeypatch, tmp_path)
    # 非冲突拒因臂: classify=READY_TO_FIX(拒因无「冲突」)
    monkeypatch.setattr(tl.pr_loop, "classify_state",
                        lambda pr, q, now: "READY_TO_FIX")
    monkeypatch.setattr(tp, "checks_ok", lambda pr, req, slug: (True, []))
    monkeypatch.setattr(tl.pr_loop, "fetch_pr",
                        lambda slug, n: NS(number=n, state="open", draft=False,
                                          fork=False, mergeable=True))
    st = _mkstate()
    st["entries"] = [_entry(status="PR_OPEN", pr_number=88,
                            fix_branch="fix/issue-7", agent_pid=None,
                            agent_log=None, conflict_nights=1,
                            last_conflict_night="2026-11-13")]
    tp.merge_sweep(st, [], False, datetime(2026, 11, 13, 23, 30))
    e = tl.StateStore.entries(st)[0]
    assert e.status == "PR_OPEN" and e.conflict_nights == 0 \
        and e.last_conflict_night is None      # 非冲突拒 → 双清

    # 过门臂: classify=READY_TO_MERGE + checks 绿 + mergeable=True → 门过
    # → 双清(execute_merge 打桩记录,门本体真实)
    monkeypatch.setattr(tl.pr_loop, "classify_state",
                        lambda pr, q, now: "READY_TO_MERGE")
    merged = []
    monkeypatch.setattr(tp, "execute_merge",
                        lambda st, e, pr, gr, dry_run=False:
                        merged.append(1) or "sha9")
    st["entries"] = [_entry(status="PR_OPEN", pr_number=88,
                            fix_branch="fix/issue-7", agent_pid=None,
                            agent_log=None, conflict_nights=1,
                            last_conflict_night="2026-11-13")]
    tp.merge_sweep(st, [], False, datetime(2026, 11, 13, 23, 30))
    assert merged == [1]                       # 门过,merge 执行被调
    e2 = tl.StateStore.entries(st)[0]
    assert e2.conflict_nights == 0 and e2.last_conflict_night is None


def test_cycle_once_ready_to_merge_reaches_merge_gate_end_to_end(
        monkeypatch, tmp_path, capsys):
    """C1 跨模块集成(终审旗舰缝: lane_b ↔ merge_sweep 的任务级盲区)——
    lane_b **不打桩**、merge_sweep **真走 evaluate_merge_gate→execute_merge**
    (不打 gate/checks_ok/execute 桩,只打 gh 层): READY_TO_MERGE 的引擎条目
    (五 bot 信号齐 + 静默窗过 + 零意见 + required checks 绿)经 cycle_once
    后由 merge 门自动合入——execute_merge 真执行(gh pr merge --squash)、
    条目 DONE + merged_sha 记账、issue 由 merge 路径收尾;非 lane_b 的
    @人工路径(零 @wangxumarshall 评论)。旧缺陷形态(lane_b 当轮置 DONE)
    下本测红: merge 门永远看不到条目、无 merge 调用、merged_sha=None。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    monkeypatch.setattr(tl, "scan_red_runs", lambda: [])
    monkeypatch.setattr(tl, "triage_new_red_runs",
                        lambda s, r, dry_run=False: [])
    monkeypatch.setattr(tl, "reap_or_advance",
                        lambda s, dry_run=False: None)
    monkeypatch.setattr(tl, "reconcile_issues",
                        lambda s, dry_run=False: {})
    monkeypatch.setattr(tl, "heal_clone", lambda s, dry_run=False: None)
    monkeypatch.setattr(tp, "needs_review_list", lambda: [])
    monkeypatch.setattr(tp, "open_docs_prs", lambda: [])
    monkeypatch.setattr(tp, "audit_line_running", lambda st: True)
    monkeypatch.setattr(tl, "_paused", lambda: False)
    monkeypatch.setattr(tl, "repo_slug", lambda: "o/r")
    monkeypatch.setattr(tl, "pr_files", lambda n: ["src/a.c"])
    monkeypatch.setattr(tl.pr_loop, "ensure_local_review",
                        lambda num, sha: None)
    pr = _pr(latest=100000)          # 五 bot 齐 + 静默窗过 + 零意见(真实 classify)
    pr.number = 88
    pr.pr_number = 88
    monkeypatch.setattr(tl.pr_loop, "fetch_pr", lambda slug, n: pr)
    gh_calls = []

    def gh_fake(args, input_text=None):
        gh_calls.append(list(args))
        if "statusCheckRollup" in args:      # required checks 全绿(真实 checks_ok)
            return "".join(f"{nm}\t{c}\tPull Request\n" for nm, c in (
                ("lint", "SUCCESS"), ("git-sanity", "SUCCESS"),
                ("Build binary for CPU (Linux)", "SUCCESS"),
                ("zizmor (workflow security)", "SUCCESS")))
        if args[:2] == ["pr", "view"] and "mergeCommit" in args:
            return "mainsha123456"
        return "{}"
    monkeypatch.setattr(tl, "gh_json_idem", gh_fake)

    # 场景①(熔断放开): cycle_once 全流程 → lane_b 只移交、merge 门真合入
    st = _mkstate()
    st["entries"] = [_entry(status="PR_OPEN", pr_number=88,
                            fix_branch="fix/issue-7")]
    events = []
    tp.cycle_once(st, False, events, datetime.now())
    e = tl.StateStore.entries(st)[0]
    assert e.status == "DONE" and e.merged_sha == "mainsha123456" \
        and e.merged_at                       # merge 门记账(非 lane_b 路径)
    assert any(c[:2] == ["pr", "merge"] and "--squash" in c
               for c in gh_calls)             # execute_merge 真执行
    assert {"kind": "merge", "pr": 88, "sha": "mainsha123456"} in events
    # 非 lane_b @人工路径: 全部评论(gh 写)不含 @wangxumarshall
    for c in gh_calls:
        assert "wangxumarshall" not in " ".join(c)
    # lane_b 真跑过(未打桩)且只移交: stdout 有移交提示、无 @人工收尾
    out = capsys.readouterr().out
    assert "READY_TO_MERGE" in out and "merge 门" in out
    # issue 收尾走 merge 路径(execute_merge 的闭环评论 + close)
    assert any(c[:2] == ["issue", "close"] and c[2] == "5"
               for c in gh_calls)
    assert any(c[:2] == ["issue", "comment"]
               and "自动 squash merge" in c[c.index("--body") + 1]
               for c in gh_calls)

    # 场景②(NO_AUTO_MERGE 灰度): 同场景条目保持 PR_OPEN、零 merge 执行,
    # 夜报记 would-merge 事件(只报告不执行)
    (tmp_path / "st").mkdir(parents=True, exist_ok=True)
    (tmp_path / "st" / tp.NO_AUTO_MERGE_NAME).write_text("")
    gh_calls.clear()
    st2 = _mkstate()
    st2["entries"] = [_entry(status="PR_OPEN", pr_number=88,
                             fix_branch="fix/issue-7")]
    events2 = []
    tp.cycle_once(st2, False, events2, datetime.now())
    e2 = tl.StateStore.entries(st2)[0]
    assert e2.status == "PR_OPEN" and e2.merged_sha is None  # 条目不动
    assert not any(c[:2] == ["pr", "merge"] for c in gh_calls)  # 零执行
    assert {"kind": "would-merge", "pr": 88, "reasons": []} in events2
    report = tp.write_report(st2, events2, False)
    with open(report, encoding="utf-8") as f:
        assert "would-merge" in f.read()      # 夜报含 would-merge 事件


def test_spawn_audit_dry_gates_and_shape(monkeypatch, tmp_path, capsys):
    """audit 后台线: dry-run 零 spawn 只打印;非 dry spawn(timeout 7500 +
    scripts/task_loop.py audit + cwd=克隆)并占 audit 槽;已占位/内存闸/
    克隆缺失三路早退。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    os.makedirs(tmp_path / "cl")
    monkeypatch.setenv("TASKLOOP_CLONE_DIR", str(tmp_path / "cl"))
    monkeypatch.setattr(tp, "mem_gate_ok", lambda: True)
    calls = []
    monkeypatch.setattr(tl, "run_background",
                        lambda cmd, cwd, log: calls.append((cmd, cwd, log))
                        or 4242)
    # 时钟钉死 23:30(窗口起点,剩 9.5h)——fix-round 1 起 spawn_audit 有窗口
    # 末端守卫,不钉死会在 06:55-09:00 时段跑套件时被守卫拦下(宿主时间炸弹)
    PIN = dict(window_end=9, now=datetime(2026, 11, 13, 23, 30))
    st = _mkstate()
    tp.spawn_audit(st, True, **PIN)        # dry: 意图打印,零 spawn
    assert calls == []
    assert "将 spawn audit" in capsys.readouterr().out
    tp.spawn_audit(st, False, **PIN)       # 真 spawn + 占槽
    assert len(calls) == 1
    assert "task_loop.py audit" in calls[0][0]
    assert "timeout 7500" in calls[0][0]
    assert calls[0][1] == str(tmp_path / "cl")
    assert tp.slot_get(st, "audit", "audit-nightly") is not None
    monkeypatch.setattr(tp, "audit_line_running", lambda s: True)
    tp.spawn_audit(st, False, **PIN)       # 已在跑 → 早退
    assert len(calls) == 1
    monkeypatch.setattr(tp, "audit_line_running", lambda s: False)
    monkeypatch.setattr(tp, "mem_gate_ok", lambda: False)
    tp.spawn_audit(st, False, **PIN)       # 内存闸 → 早退
    assert len(calls) == 1
    monkeypatch.setattr(tp, "mem_gate_ok", lambda: True)
    monkeypatch.setenv("TASKLOOP_CLONE_DIR", str(tmp_path / "nope"))
    tp.spawn_audit(st, False, **PIN)       # 克隆缺失 → 早退
    assert len(calls) == 1


def test_needs_review_list_parses_tsv(monkeypatch):
    """泳道2 取材: gh pr list TSV 解析,already_reviewed(第 5 bot 信号
    commit 锚定幂等)命中者剔除;畸形行/无 headRefOid 行宁跳过不炸扫。"""
    import pr_local_review as plr
    monkeypatch.setattr(tl, "gh_json_idem",
                        lambda a, input_text=None: "640\tabc\n641\tdef\n\n")
    monkeypatch.setattr(plr, "already_reviewed",
                        lambda num, head: num == 640)
    assert tp.needs_review_list() == [(641, "def")]
    monkeypatch.setattr(tl, "gh_json_idem",
                        lambda a, input_text=None: "junk\n642\tx\n")
    monkeypatch.setattr(plr, "already_reviewed", lambda num, head: False)
    assert tp.needs_review_list() == [(642, "x")]
    monkeypatch.setattr(tl, "gh_json_idem", lambda a, input_text=None: None)
    assert tp.needs_review_list() == []     # gh 空输出(gh_json None 契约)


def test_open_docs_prs_parses_tsv(monkeypatch):
    """docs 取材: 三元组(号/标题/head);WIP/docs-only 过滤在门扫
    (wip_gated/docs_only),本函数只取材。"""
    monkeypatch.setattr(tl, "gh_json_idem",
                        lambda a, input_text=None:
                        "700\tdocs: title\th1\n701\tWIP: x\th2\nbad\n")
    assert tp.open_docs_prs() == [
        {"pr_number": 700, "title": "docs: title", "head": "h1"},
        {"pr_number": 701, "title": "WIP: x", "head": "h2"}]
    monkeypatch.setattr(tl, "gh_json_idem", lambda a, input_text=None: None)
    assert tp.open_docs_prs() == []


# ============ Task 10: CLI 接线 + scan 薄壳 ============


def test_cmd_scan_is_pipeline_single_cycle_shell(monkeypatch, tmp_path):
    """scan 薄壳(裁定#7): cmd_scan → task_loop_pipeline 单轮非 dry 循环,
   非复制粘贴另一套编排。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    # RED 安全垫: 旧 cmd_scan 走 _run_scan_orchestration——stub 其重接缝,
    # 使 RED 失败落在断言而非真实 gh/git 副作用。
    monkeypatch.setattr(tl, "_migrate_legacy_state_dir", lambda: None)
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    monkeypatch.setattr(tl, "scan_red_runs", lambda: [])
    monkeypatch.setattr(tl, "list_open_agent_issues", lambda: [])
    monkeypatch.setattr(tl, "heal_clone", lambda s, dry_run=False: None)
    called = []
    monkeypatch.setattr(tp, "run_pipeline_single_cycle",
                        lambda: called.append(1) or 0)
    assert tl.cmd_scan(NS()) == 0
    assert called == [1]


def test_run_pipeline_single_cycle_flow(monkeypatch, tmp_path):
    """单轮循环: 共 pipeline.lock、前奏幂等子集(迁移/标签/启动对账)、
    不 reset_review_tries(手动 scan 不清检视每晚预算——夜边界是流水线
    启动对账,spec §4)、GhError → 退出 1 且已推进 state 落盘。"""
    _patch_cycle_offline(monkeypatch, tmp_path)
    monkeypatch.setattr(tl, "_migrate_legacy_state_dir", lambda: None)
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    os.makedirs(tmp_path / "st")
    tl.StateStore().save({"entries": [], "auth_failure_streak": 0,
                          "review_tries": {"640:h1": 1}})
    cycles = []
    monkeypatch.setattr(tp, "cycle_once",
                        lambda st, dr, ev, now: cycles.append(dr))
    assert tp.run_pipeline_single_cycle() == 0
    assert cycles == [False]                # 单轮非 dry
    disk = tl.StateStore().load()
    assert disk["review_tries"] == {"640:h1": 1}   # 不清每晚预算
    assert "last_report" not in disk               # 单轮不写夜报


def test_run_pipeline_single_cycle_lock_conflict(tmp_path, monkeypatch):
    """单轮薄壳与 run_pipeline 共 pipeline.lock: 外部持有 → 打印 + 退出 0。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    os.makedirs(tmp_path / "st")
    fd = os.open(os.path.join(tmp_path / "st", tp.PIPELINE_LOCK_NAME),
                 os.O_CREAT | os.O_RDWR)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX)
        assert tp.run_pipeline_single_cycle() == 0
        assert not os.path.exists(os.path.join(tmp_path / "st", "state.json"))
    finally:
        fcntl.flock(fd, fcntl.LOCK_UN)
        os.close(fd)


def test_pipeline_cli_args_and_lazy_import(monkeypatch):
    """pipeline 子命令 argparse + cmd_pipeline 懒 import 接线(防
    task_loop↔pipeline 循环依赖);--dry-run/--window-start/--window-end
    透传。"""
    parser = tl.build_parser()
    args = parser.parse_args(["pipeline", "--dry-run",
                              "--window-start", "22", "--window-end", "8"])
    assert args.dry_run is True and args.window_start == 22 \
        and args.window_end == 8
    args = parser.parse_args(["pipeline"])
    assert args.dry_run is False and args.window_start == 23 \
        and args.window_end == 9
    seen = {}
    monkeypatch.setattr(tp, "run_pipeline",
                        lambda dry_run=False, window_start=23, window_end=9:
                        seen.update(dry_run=dry_run, ws=window_start,
                                    we=window_end) or 0)
    assert tl.main(["pipeline", "--dry-run", "--window-start", "22",
                    "--window-end", "8"]) == 0
    assert seen == {"dry_run": True, "ws": 22, "we": 8}


def test_cmd_status_shows_last_report(tmp_path, monkeypatch, capsys):
    """status 顶部最近一夜摘要: state["last_report"] 存在 → 打一行
    (报告文件在 state 目录,status 只报名字)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    os.makedirs(tmp_path / "st")
    tl.StateStore().save({"entries": [], "auth_failure_streak": 0,
                          "last_report": "report-20261113.md"})
    monkeypatch.setattr(tl, "list_open_agent_issues", lambda: [])
    assert tl.cmd_status(NS()) == 0
    assert "最近夜间报告: report-20261113.md" in capsys.readouterr().out


# ============ Task 10 fix round 1: 共管键合并 / soft_stop 泳道过滤 / audit 窗口守卫 ============


def test_pipeline_save_co_managed_streak_keys_max_merge(monkeypatch, tmp_path):
    """Important#1(fix-round 1): auth_failure_streak/auth_failure_at 是**双写者
    键**——pipeline 的 reap_or_advance 每轮在内存 bump/归零,audit 子进程亦在
    scan.lock 下 merge-write(_audit_streak_add)。裸拷磁盘旧值会把 pipeline 的
    bump 在内存与磁盘同时抹掉,「连续 ≥3 → PAUSED」熔断跨轮结构性失效(凭据
    过期夜=整夜空转 ~120 轮)。合并语义(controller 裁定,熔断宁早勿晚):
    streak=max(盘,内存)、at=取两侧较新;audit_cursor/audit_seen 保持磁盘权威。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    os.makedirs(tmp_path / "st")
    store = tl.StateStore()
    # 臂①: 磁盘侧 audit 推进在先(streak=2, at=T1),内存侧 reap bump(streak=1)
    # → max=2 落盘且**内存对象同步为 2**(下轮 bump 从 2 起,不再被抹)
    store.save({"entries": [], "auth_failure_streak": 2,
                "auth_failure_at": "2026-11-13T01:00:00Z",
                "audit_cursor": {"sha": "s", "at": "t"},
                "audit_seen": ["h1"]})
    mem = _mkstate()
    mem["auth_failure_streak"] = 1
    mem["auth_failure_at"] = "2026-11-13T00:00:00Z"      # 内存侧更旧
    tp._pipeline_save(store, mem)
    assert mem["auth_failure_streak"] == 2               # max 且内存同步
    assert mem["auth_failure_at"] == "2026-11-13T01:00:00Z"   # at 取较新(盘)
    disk = store.load()
    assert disk["auth_failure_streak"] == 2
    assert disk["audit_cursor"] == {"sha": "s", "at": "t"}     # 磁盘权威
    assert disk["audit_seen"] == ["h1"]
    # 臂②(反向,RED 判别臂): 内存 3(已达熔断阈)、磁盘 1 → 3 保留;
    # at 反向同理——内存较新 T2 胜出
    store.save({"entries": [], "auth_failure_streak": 1,
                "auth_failure_at": "2026-11-13T00:00:00Z"})
    mem2 = _mkstate()
    mem2["auth_failure_streak"] = 3
    mem2["auth_failure_at"] = "2026-11-13T02:00:00Z"
    tp._pipeline_save(store, mem2)
    assert mem2["auth_failure_streak"] == 3
    assert mem2["auth_failure_at"] == "2026-11-13T02:00:00Z"   # at 取较新(内存)
    assert store.load()["auth_failure_streak"] == 3
    # 臂③(裁定明示的近似): 内存成功归零 0、磁盘仍 2 → max=2——成功归零
    # 可能被另一线未归零值掩盖,熔断宁早勿晚(fail-safe),docstring 如实注明
    store.save({"entries": [], "auth_failure_streak": 2})
    mem3 = _mkstate()
    mem3["auth_failure_streak"] = 0
    tp._pipeline_save(store, mem3)
    assert mem3["auth_failure_streak"] == 2
    # 双写者键缺侧取有: 磁盘无 at、内存有 → 内存保留
    store.save({"entries": [], "auth_failure_streak": 0})
    mem4 = _mkstate()
    mem4["auth_failure_at"] = "2026-11-13T03:00:00Z"
    tp._pipeline_save(store, mem4)
    assert mem4["auth_failure_at"] == "2026-11-13T03:00:00Z"


def test_soft_stop_pr_review_slot_does_not_demote_entry(monkeypatch, tmp_path):
    """Important#2(fix-round 1): pr_review 与 pr_repair 槽对同一 PR 用同 key
    f"pr-<N>"(泳道2+3 并行是 spec 设计流,reviewer 7500s 超时 > 300s 轮询,
    碰撞不罕见)。回队循环只认 issue_fix/pr_repair 槽——pr_review/audit 槽只
    击杀清槽不动条目;旧形态按 key 匹配任意活槽,pr_review 槽命中 BOT_FIX
    条目时 kind≠pr_repair 把它误降 QUEUED,同 key 双槽时还会把 pr_repair 臂
    刚写的 PR_OPEN 再抹掉。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    monkeypatch.setattr(tp.time, "sleep", lambda s: None)
    monkeypatch.setattr(tl, "agent_alive", lambda pid: True)
    monkeypatch.setattr(tl, "worktree_cleanup", lambda p: None)
    # 场景①: BOT_FIX 条目 + 同 PR 双活槽(修复 agent + reviewer 并行)
    st = _mkstate()
    st["entries"] = [_entry(status="BOT_FIX", pr_number=88, attempts=1,
                            agent_pid=4242, worktree="/wt/pr-88")]
    tp.slot_acquire(st, "pr_repair", "pr-88", 4242, 4242, "/l", "/wt/pr-88",
                    "t", "b")
    tp.slot_acquire(st, "pr_review", "pr-88", 4245, 4245, "/l", None,
                    "t", "b")
    tp.soft_stop(st, grace_sec=0)
    e = tl.StateStore.entries(st)[0]
    assert e.status == "PR_OPEN"          # 修复臂回队,检视臂不再抹成 QUEUED
    assert e.attempts == 1                # 击杀不计 attempts
    assert not st.get("slots", {}).get("pr_repair")
    assert not st.get("slots", {}).get("pr_review")   # 检视槽照样击杀清槽
    # 场景②: 同 PR 只有 pr_review 活槽(修复 agent 已退场的过渡形态)
    # → 条目原样,检视槽不越权处置条目
    st2 = _mkstate()
    st2["entries"] = [_entry(status="BOT_FIX", pr_number=88, attempts=1,
                             agent_pid=None)]
    tp.slot_acquire(st2, "pr_review", "pr-88", 4245, 4245, "/l", None,
                    "t", "b")
    tp.soft_stop(st2, grace_sec=0)
    e2 = tl.StateStore.entries(st2)[0]
    assert e2.status == "BOT_FIX" and e2.attempts == 1
    assert not st2.get("slots", {}).get("pr_review")


def test_spawn_audit_window_end_guard(monkeypatch, tmp_path, capsys):
    """Minor#1(fix-round 1): 距窗口结束剩余 < AUDIT_LINE_TIMEOUT_SEC(7500s,
    audit 自身 timeout 预算)不再 spawn——audit 唯一持久化是收尾一次
    merge-write,09:00 软停击杀会丢掉最长 ~2h 在审块(白审)。时钟经参数
    注入(裁定#9),与 in_window 同款本地口径(跨午夜窗)。"""
    monkeypatch.setenv("TASKLOOP_STATE_DIR", str(tmp_path / "st"))
    os.makedirs(tmp_path / "cl")
    monkeypatch.setenv("TASKLOOP_CLONE_DIR", str(tmp_path / "cl"))
    monkeypatch.setattr(tp, "mem_gate_ok", lambda: True)
    calls = []
    monkeypatch.setattr(tl, "run_background",
                        lambda cmd, cwd, log: calls.append(cmd) or 4242)
    # 07:00 → 剩 2h < 7500s → 拦(dry 预演同样只报拦因,不出现将 spawn)
    st = _mkstate()
    tp.spawn_audit(st, True, window_end=9, now=datetime(2026, 11, 13, 7, 0))
    tp.spawn_audit(st, False, window_end=9, now=datetime(2026, 11, 13, 7, 0))
    assert calls == []
    out = capsys.readouterr().out
    assert "距窗口结束" in out and "将 spawn audit" not in out
    # 23:30(窗口起点附近)→ 剩 9.5h → 正常 spawn
    tp.spawn_audit(_mkstate(), False, window_end=9,
                   now=datetime(2026, 11, 13, 23, 30))
    assert len(calls) == 1
    # 跨午夜: 01:00 → 剩 8h → spawn
    tp.spawn_audit(_mkstate(), False, window_end=9,
                   now=datetime(2026, 11, 13, 1, 0))
    assert len(calls) == 2
    # 08:59 → 剩 60s → 拦
    tp.spawn_audit(_mkstate(), False, window_end=9,
                   now=datetime(2026, 11, 13, 8, 59))
    assert len(calls) == 2


def test_run_pipeline_single_cycle_gherror_rc1_persists_progress(
        monkeypatch, tmp_path, capsys):
    """Minor#2(fix-round 1): 单轮薄壳 GhError → rc 1 且已推进 state 落盘
    (旧 scan 先例语义的覆盖锁): 预置 boot 变化死槽 → 启动对账干净回队先
    落盘,cycle 首步 GhError 后回读 state 证推进未丢。"""
    _patch_cycle_offline(monkeypatch, tmp_path)
    monkeypatch.setattr(tl, "_migrate_legacy_state_dir", lambda: None)
    monkeypatch.setattr(tl, "ensure_agent_labels", lambda: None)
    monkeypatch.setattr(tl, "agent_alive", lambda pid: False)
    monkeypatch.setattr(tl, "worktree_cleanup", lambda p: None)
    monkeypatch.setattr(tp, "boot_id", lambda: "new-boot")
    os.makedirs(tmp_path / "st")
    seeded = _mkstate()
    seeded["entries"] = [_entry(status="DISPATCHED", agent_pid=111,
                                attempts=2)]
    tp.slot_acquire(seeded, "issue_fix", "issue-5", 111, 0, "/l", "/w",
                    "t", "old-boot")
    tl.StateStore().save(seeded)

    def boom():
        raise tl.GhError("gh run list … 失败(exit 1): HTTP 502")
    monkeypatch.setattr(tl, "scan_red_runs", boom)
    assert tp.run_pipeline_single_cycle() == 1
    assert "GhError" in capsys.readouterr().err
    disk = tl.StateStore().load()
    assert disk["entries"][0]["status"] == "QUEUED"   # 启动对账推进已落盘
    assert disk["entries"][0]["agent_pid"] is None
    assert disk["entries"][0]["attempts"] == 2        # 环境性死亡豁免不动 attempts
