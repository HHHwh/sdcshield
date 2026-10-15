"""pr_loop 第 5 bot(本地 claude code)判据测试(Task 2)。

用例口径(Task 2 brief 2 例,纯函数、全离线):
  1 test_bot_order_has_five: BOT_ORDER 扩为五元组——第 5 位置 local-claude,
    order 即 status/wait/collect 展示顺序;
  2 test_local_claude_requires_author_and_marker: 第 5 bot 判据是「作者
    wangxumarshall 且 正文含 LOCAL_CLAUDE_MARKER」双条件——wangxumarshall
    无标记评论不算、非 wangxumarshall 有标记不算、两者齐备才算(避免把
    用户本人普通评论误判为第 5 bot,设计文档反目标 2)。
"""
import json
import os
import sys

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "..", "scripts"))
import pr_loop
import pr_local_review


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
    """第 5 bot 触发失败(真实 pr_local_review.GhError)仅告警降级,wait 仍继续
    四 bot 轮询(不 exit 2)。

    走真实 ensure_local_review 代码路径:already_reviewed 返回 False →
    run_review 抛 pr_local_review.GhError——断言 cmd_wait 的 except GhError
    捕获的正是这个异常类(全仓唯一 GhError)。此前 pr_loop 自定义同名 GhError、
    cmd_wait 只捕获它,导致真实失败穿透为 traceback(本用例旧版 monkeypatch 抛
    pr_loop.GhError 恰好被捕获,是假阳性)。
    """
    # 全仓唯一异常类:pr_loop.GhError 与 pr_local_review.GhError 必须是同一类
    assert pr_loop.GhError is pr_local_review.GhError

    pr = _open_pr()
    monkeypatch.setattr(pr_loop, "resolve_slug", lambda explicit=None: "test/repo")
    monkeypatch.setattr(pr_loop, "fetch_pr", lambda slug, num: pr)
    # 首轮即完成 → wait 立即返回,不 sleep
    monkeypatch.setattr(pr_loop, "classify_state",
                        lambda p, quiet, now: "READY_TO_MERGE")
    # 未审 → 进入 run_review,由其抛真实 pr_local_review.GhError
    monkeypatch.setattr(pr_loop, "already_reviewed", lambda num, sha: False)

    def boom(num, sha):
        raise pr_local_review.GhError("gh pr view --json reviews 失败(exit 1): 未认证")

    monkeypatch.setattr(pr_loop, "run_review", boom)

    args = pr_loop.build_parser().parse_args(
        ["wait", "9", "--interval", "0", "--timeout", "1"])
    # 第 5 bot 触发失败不硬中止 wait——四 bot 轮询照常进入并完成,exit 0
    assert pr_loop.cmd_wait(args) == 0


# ---------------------------------------------------------------------------
# Task 4: 意见计数(body JSON 段 findings + 行级评论)+ 闭环逐条答复 + 意见数
# 计入(第 5 bot 意见不再让 status 误判 READY_TO_MERGE)。

def _local_review_body(num_findings):
    """构造一条 local-claude review body: 首行标记 + ```json 段收 n 条 finding。"""
    body_findings = [
        {"path": "tests/sve/fma_sve.c", "line": None, "severity": "Critical",
         "category": "正确性", "message": f"近似比对是 SDC 盲区 {i}",
         "evidence": "容差而非 memcmp"}
        for i in range(num_findings)
    ]
    return (pr_loop.LOCAL_CLAUDE_MARKER
            + "\n\n## 挂不上 diff 行的 findings(JSON)\n\n```json\n"
            + json.dumps(body_findings, ensure_ascii=False, indent=2)
            + "\n```\n")


def test_count_local_claude_findings_parses_body_json():
    # body JSON 段 3 条 findings + 2 行级评论(review.comments)= 计数 5
    review = {"body": _local_review_body(3),
              "comments": [{"id": 1}, {"id": 2}]}
    assert pr_loop.count_local_claude_findings(review) == 5


def test_reply_all_findings_no_miss(monkeypatch):
    posted = []
    monkeypatch.setattr(pr_loop, "resolve_slug", lambda explicit=None: "test/repo")
    monkeypatch.setattr(pr_loop, "_gh_api_post",
                        lambda path, payload: posted.append((path, payload)))

    findings = [
        {"path": "a.c", "line": 1, "id": 101},
        {"path": "b.c", "line": 2, "id": 102},
        {"path": "c.c", "line": 3, "id": 103},
        # 挂不上 diff 行(line=None)的 2 条 → 收进 issue comment
        {"path": "d.c", "line": None, "severity": "Important",
         "category": "性能", "message": "m4", "evidence": "e4"},
        {"path": "e.c", "line": None, "severity": "Minor",
         "category": "可维护性", "message": "m5", "evidence": "e5"},
    ]
    replies = [{"conclusion": "采纳", "reason": "r", "evidence": "e"}
               for _ in range(5)]

    pr_loop.reply_to_findings(9, findings, replies)

    # 3 条行级 reply + 1 条 issue comment(承载 2 条 body finding 答复)= 5 条答复
    inline = [p for p in posted if "/replies" in p[0]]
    issue = [p for p in posted if "/issues/9/comments" in p[0]]
    assert len(inline) == 3
    assert len(issue) == 1
    assert {p[0] for p in inline} == {
        "repos/test/repo/pulls/9/comments/101/replies",
        "repos/test/repo/pulls/9/comments/102/replies",
        "repos/test/repo/pulls/9/comments/103/replies",
    }
    body = issue[0][1]["body"]
    # issue comment 内逐条独立成段(2 条 body finding → 2 个 ### 段)
    assert body.count("### ") == 2
    assert "d.c" in body and "e.c" in body

    # 完整性校验: 漏 1 条(replies 只有 4 条)→ 抛错/非零退出,漏答不结案
    with pytest.raises(pr_loop.GhError):
        pr_loop.reply_to_findings(9, findings, replies[:4])


def test_reply_accept_has_sha_reject_has_evidence(monkeypatch):
    posted = []
    monkeypatch.setattr(pr_loop, "resolve_slug", lambda explicit=None: "test/repo")
    monkeypatch.setattr(pr_loop, "_gh_api_post",
                        lambda path, payload: posted.append((path, payload)))

    sha = "f" * 40
    findings = [
        {"path": "tests/sve/fma_sve.c", "line": 42, "id": 201},
        {"path": "tests/sve/fma_sve.c", "line": 43, "id": 202},
    ]
    replies = [
        {"conclusion": "采纳", "reason": "golden 比对必须字节级 memcmp",
         "evidence": f"已修复于 commit {sha}"},
        {"conclusion": "驳回", "reason": "误报,非本 PR 引入",
         "evidence": "实证: ninja -C builddir 零新警告;"
                     "./builddir/sdcshield -e zstd19 -t 2000 -n 1 → exit: pass"},
    ]

    pr_loop.reply_to_findings(9, findings, replies)
    assert len(posted) == 2

    accept_body = posted[0][1]["body"]
    reject_body = posted[1][1]["body"]
    # 接纳含 commit SHA、驳回含证据字段(统一结构: 结论 + 理由 + 证据)
    assert "采纳" in accept_body and sha in accept_body
    assert "驳回" in reject_body and "实证" in reject_body


def test_local_claude_opinion_counted_not_ready_to_merge(monkeypatch):
    """第 5 bot 意见须计入 opinion_count: 发 1 条 Critical 行级评论 + 2 条 body
    finding 后,status 不再给 READY_TO_MERGE(n_review/n_rcomment 均须纳入)。"""
    head = "e" * 40
    marker = pr_loop.LOCAL_CLAUDE_MARKER

    reviews = [
        {"id": 1, "user": {"login": "chatgpt-codex-connector[bot]"},
         "state": "APPROVED", "commit_id": head,
         "submitted_at": "2026-10-15T00:01:00Z", "body": ""},
        {"id": 2, "user": {"login": "coderabbitai[bot]"},
         "state": "APPROVED", "commit_id": head,
         "submitted_at": "2026-10-15T00:01:00Z", "body": ""},
        # local-claude review: state COMMENTED + body JSON 段 2 条 finding
        {"id": 3, "user": {"login": "wangxumarshall"},
         "state": "COMMENTED", "commit_id": head,
         "submitted_at": "2026-10-15T00:01:00Z",
         "body": _local_review_body(2)},
    ]
    inline_comments = [
        {"id": 100, "user": {"login": "wangxumarshall"},
         "body": f"{marker}\n**[Critical] 正确性**: 近似比对是 SDC 盲区",
         "created_at": "2026-10-15T00:02:00Z",
         "original_commit_id": head, "commit_id": head,
         "path": "tests/sve/fma_sve.c", "line": 42},
    ]
    checks = [
        {"name": "pr-agent", "status": "completed", "conclusion": "success",
         "head_sha": head, "completed_at": "2026-10-15T00:01:00Z"},
        {"name": "greptile-scan", "app": {"slug": "greptile-apps"},
         "status": "completed", "conclusion": "success",
         "head_sha": head, "completed_at": "2026-10-15T00:01:00Z"},
    ]

    def fake_gh_api(path):
        if path == "repos/test/repo/pulls/9":
            return {"number": 9, "title": "t", "state": "open", "draft": False,
                    "head": {"sha": head, "ref": "feat/x",
                             "repo": {"full_name": "test/repo"}},
                    "base": {"ref": "main", "repo": {"full_name": "test/repo"}},
                    "user": {"login": "wangxumarshall"},
                    "html_url": "https://example.invalid/9"}
        if path == f"repos/test/repo/commits/{head}":
            return {"commit": {"committer": {"date": "2026-10-15T00:00:00Z"},
                               "author": {"date": "2026-10-15T00:00:00Z"}}}
        if path.startswith("repos/test/repo/events"):
            return []
        raise AssertionError(f"意外 gh_api 调用: {path}")

    def fake_paged(path, list_key=None):
        # 模仿真实 gh_api_paged 的返回约定: 已提取 list_key,直接返回条目列表
        if "check-runs" in path:
            return checks
        if "/reviews" in path:
            return reviews
        if "/issues/9/comments" in path:
            return []
        if "/comments" in path:
            return inline_comments
        raise AssertionError(f"意外 gh_api_paged 调用: {path}")

    monkeypatch.setattr(pr_loop, "gh_api", fake_gh_api)
    monkeypatch.setattr(pr_loop, "gh_api_paged", fake_paged)

    pr = pr_loop.fetch_pr("test/repo", 9)

    # 五 bot 全有信号 + local-claude 实质意见(2 body finding + 1 行级 Critical)
    assert all(pr.bot_done.get(b) is not None for b in pr_loop.BOT_ORDER)
    assert pr.opinion_count == 3
    assert pr.opinion_breakdown["review"] == 2
    assert pr.opinion_breakdown["review_comment"] == 1

    state = pr_loop.classify_state(pr, 0, pr_loop.now_utc())
    # 有意见 → READY_TO_FIX,绝不 READY_TO_MERGE
    assert state == "READY_TO_FIX"
    assert state != "READY_TO_MERGE"
