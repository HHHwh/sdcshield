"""ci_triage 状态存储（Task 1）——StateStore round-trip 与原子写。

用例口径（brief 2 例）:
  1 test_state_store_roundtrip: save 后 load 等值——完整 state(含 Entry 列表
    + auth_failure_streak)经 JSON 往返后逐字段等值,entries(state) 还原为
    类型化 Entry 对象且全部 16 字段等值;
  2 test_state_store_atomic_no_partial: 写入中断(替换 os.replace 抛异常)
    时,旧 state.json 原样可读、不残留 tmp 文件被 load 读到——load 只读
    state.json,save 是「写 tmp → os.replace」原子替换。
"""
import os, sys

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
