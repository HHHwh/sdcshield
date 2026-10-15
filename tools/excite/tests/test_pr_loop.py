"""pr_loop 第 5 bot(本地 claude code)判据测试(Task 2)。

用例口径(Task 2 brief 2 例,纯函数、全离线):
  1 test_bot_order_has_five: BOT_ORDER 扩为五元组——第 5 位置 local-claude,
    order 即 status/wait/collect 展示顺序;
  2 test_local_claude_requires_author_and_marker: 第 5 bot 判据是「作者
    wangxumarshall 且 正文含 LOCAL_CLAUDE_MARKER」双条件——wangxumarshall
    无标记评论不算、非 wangxumarshall 有标记不算、两者齐备才算(避免把
    用户本人普通评论误判为第 5 bot,设计文档反目标 2)。
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "..", "scripts"))
import pr_loop


def test_bot_order_has_five():
    assert pr_loop.BOT_ORDER == ("codex", "coderabbit", "pr-agent",
                                 "greptile", "local-claude")


def test_local_claude_requires_author_and_marker():
    assert pr_loop.LOCAL_CLAUDE_LOGIN == "wangxumarshall"
    assert pr_loop.LOCAL_CLAUDE_MARKER == "<!-- local-claude-review -->"

    # wangxumarshall 无标记评论 → 不算第 5 bot(用户本人普通评论)
    assert pr_loop.local_claude_comment("wangxumarshall", "普通人工评论") is False
    # 非 wangxumarshall 有标记 → 不算(双条件,只认作者+标记齐备)
    assert pr_loop.local_claude_comment(
        "someone[bot]", pr_loop.LOCAL_CLAUDE_MARKER) is False
    # wangxumarshall 有标记 → 算第 5 bot
    assert pr_loop.local_claude_comment(
        "wangxumarshall", f"{pr_loop.LOCAL_CLAUDE_MARKER}\n检视意见…") is True


# ---------------------------------------------------------------------------
# Task 3: wait 内主动触发第 5 bot 审查(事件线)+ 同 head 幂等。
# 全部离线: fake resolve_slug/fetch_pr/classify_state/run_review/publish_review/
# already_reviewed——绝不触网、绝不真实跑 claude。

def _open_pr(number=9, head_sha="d" * 40):
    """cmd_wait 轮询所需的最小 PRData(open、非 draft/fork,首轮即 READY_TO_MERGE)。"""
    return pr_loop.PRData(
        number=number, title="t", state="open", draft=False, fork=False,
        head_sha=head_sha, head_ref="feat/x", base_ref="main",
        head_time=pr_loop.now_utc(), url=f"https://example.invalid/{number}",
        author="wangxumarshall",
        opinion_breakdown={"review": 0, "review_comment": 0,
                           "issue_comment": 0, "annotations": 0},
    )


def test_wait_triggers_local_review_once(monkeypatch):
    pr = _open_pr()
    monkeypatch.setattr(pr_loop, "resolve_slug", lambda explicit=None: "test/repo")
    monkeypatch.setattr(pr_loop, "fetch_pr", lambda slug, num: pr)
    # 首轮即完成 → wait 立即返回,不 sleep,触网外零副作用
    monkeypatch.setattr(pr_loop, "classify_state",
                        lambda p, quiet, now: "READY_TO_MERGE")

    calls = {"run": 0, "publish": 0}
    reviewed = {"flag": False}
    monkeypatch.setattr(pr_loop, "already_reviewed",
                        lambda num, sha: reviewed["flag"])

    def fake_run(num, sha):
        calls["run"] += 1
        return []

    def fake_publish(num, sha, findings):
        calls["publish"] += 1

    monkeypatch.setattr(pr_loop, "run_review", fake_run)
    monkeypatch.setattr(pr_loop, "publish_review", fake_publish)

    args = pr_loop.build_parser().parse_args(
        ["wait", "9", "--interval", "0", "--timeout", "1"])

    # 未审(head 尚未发过第 5 bot 信号)→ wait 主动触发一次审查并发布
    assert pr_loop.cmd_wait(args) == 0
    assert calls == {"run": 1, "publish": 1}

    # 已审(already_reviewed 真)→ wait 不再触发(同 head 幂等)
    calls["run"] = calls["publish"] = 0
    reviewed["flag"] = True
    assert pr_loop.cmd_wait(args) == 0
    assert calls == {"run": 0, "publish": 0}


def test_ensure_local_review_idempotent_same_head(monkeypatch):
    calls = {"run": 0, "publish": 0}
    state = {"reviewed": False}

    def fake_already_reviewed(num, sha):
        return state["reviewed"]

    def fake_run(num, sha):
        calls["run"] += 1
        return []

    def fake_publish(num, sha, findings):
        calls["publish"] += 1
        state["reviewed"] = True   # 发布后同 head 即视为已审

    monkeypatch.setattr(pr_loop, "already_reviewed", fake_already_reviewed)
    monkeypatch.setattr(pr_loop, "run_review", fake_run)
    monkeypatch.setattr(pr_loop, "publish_review", fake_publish)

    head = "d" * 40
    pr_loop.ensure_local_review(9, head)
    pr_loop.ensure_local_review(9, head)

    # 同 head 二次调用不触发(幂等去重——事件线 + 时间线撞车只审一次)
    assert calls == {"run": 1, "publish": 1}


def test_wait_ensure_local_review_failure_degrades(monkeypatch):
    """第 5 bot 触发失败(GhError)仅告警降级,wait 仍继续四 bot 轮询(不 exit 2)。"""
    pr = _open_pr()
    monkeypatch.setattr(pr_loop, "resolve_slug", lambda explicit=None: "test/repo")
    monkeypatch.setattr(pr_loop, "fetch_pr", lambda slug, num: pr)
    # 首轮即完成 → wait 立即返回,不 sleep
    monkeypatch.setattr(pr_loop, "classify_state",
                        lambda p, quiet, now: "READY_TO_MERGE")

    def boom(num, sha):
        raise pr_loop.GhError("gh pr view --json reviews 失败(exit 1): 未认证")

    monkeypatch.setattr(pr_loop, "ensure_local_review", boom)

    args = pr_loop.build_parser().parse_args(
        ["wait", "9", "--interval", "0", "--timeout", "1"])
    # 第 5 bot 触发失败不硬中止 wait——四 bot 轮询照常进入并完成,exit 0
    assert pr_loop.cmd_wait(args) == 0
