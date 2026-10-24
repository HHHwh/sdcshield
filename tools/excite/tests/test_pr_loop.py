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
from datetime import timedelta

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
    replies = [{"conclusion": "采纳", "reason": "r",
                "evidence": f"已修复于 commit {'f' * 40}"}
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


def test_local_claude_body_findings_tolerates_triple_backtick():
    """evidence 含三反引号代码块时,结束围栏不应被其提前截断——body JSON 段
    仍能完整解析(findings 一条不少),修复 Important 3(首个 ``` 提前截断)。"""
    finding = {"path": "tests/sve/fma_sve.c", "line": None,
               "severity": "Critical", "category": "正确性",
               "message": "近似比对是 SDC 盲区",
               "evidence": "证据含代码块:\n```\nint x = 1;\n```\n"}
    body = (pr_loop.LOCAL_CLAUDE_MARKER
            + "\n\n```json\n"
            + json.dumps([finding], ensure_ascii=False, indent=2)
            + "\n```\n")
    parsed = pr_loop._local_claude_body_findings(body)
    assert len(parsed) == 1
    assert parsed[0]["message"] == "近似比对是 SDC 盲区"
    assert "```" in parsed[0]["evidence"]
    assert "\n" in parsed[0]["evidence"]


def test_format_reply_rejects_accept_without_sha_and_reject_without_evidence():
    # 采纳缺 commit SHA → 抛错(证据须含 7-40 位十六进制 SHA)
    with pytest.raises(pr_loop.GhError):
        pr_loop._format_reply({"conclusion": "采纳", "reason": "r",
                               "evidence": "已修复,但没写 SHA"})
    # 驳回缺实证 → 抛错(证据不能为空)
    with pytest.raises(pr_loop.GhError):
        pr_loop._format_reply({"conclusion": "驳回", "reason": "误报",
                               "evidence": ""})
    # 合法: 采纳带 SHA / 驳回带实证
    sha = "a" * 40
    ok_accept = pr_loop._format_reply({"conclusion": "采纳", "reason": "r",
                                       "evidence": f"已修复于 commit {sha}"})
    assert "采纳" in ok_accept and sha in ok_accept
    ok_reject = pr_loop._format_reply({"conclusion": "驳回", "reason": "误报",
                                       "evidence": "实证: ninja -C builddir 零新警告"})
    assert "驳回" in ok_reject and "实证" in ok_reject


def test_reply_subcommand_has_production_entry(monkeypatch, tmp_path):
    """reply_to_findings 现在有生产入口: CLI 可解析 reply 命令,cmd_reply 读
    处置 JSON 后先 fetch_pr + verify_reply_completeness 再调 reply_to_findings
    (阶段 4 处置后闭环段)。"""
    disp = tmp_path / "dispositions.json"
    disp.write_text(json.dumps([
        {"path": "a.c", "line": 1, "id": 101, "conclusion": "采纳",
         "reason": "r", "evidence": "已修复于 commit " + "f" * 40},
        {"path": "d.c", "line": None, "conclusion": "驳回",
         "reason": "误报", "evidence": "实证: ninja 零警告"},
    ]))
    captured = {}

    def fake_reply(pr, findings, replies):
        captured["pr"] = pr
        captured["findings"] = findings
        captured["replies"] = replies

    def fake_verify(pr, findings, replies):
        captured["verified"] = (pr, findings, replies)

    monkeypatch.setattr(pr_loop, "reply_to_findings", fake_reply)
    monkeypatch.setattr(pr_loop, "verify_reply_completeness", fake_verify)
    monkeypatch.setattr(pr_loop, "resolve_slug", lambda explicit=None: "test/repo")
    fake_pr = object()
    monkeypatch.setattr(pr_loop, "fetch_pr", lambda slug, num: fake_pr)

    args = pr_loop.build_parser().parse_args(["reply", "9", "-f", str(disp)])
    assert pr_loop.cmd_reply(args) == 0
    assert captured["pr"] == 9
    # 答复前先 fetch 真实 PR 并做完整性校验(不再只比自产数组长度,C1)
    assert captured["verified"][0] is fake_pr
    assert captured["findings"] == [{"path": "a.c", "line": 1, "id": 101},
                                    {"path": "d.c", "line": None, "id": None}]
    assert captured["replies"][0]["conclusion"] == "采纳"
    assert captured["replies"][1]["conclusion"] == "驳回"


def test_reply_verifies_completeness_against_real_opinions(monkeypatch, tmp_path):
    """C1: 闭环完整性校验必须对照 GitHub 真实待处置意见集,而非同一份 JSON 平行
    拆出的两个数组长度(恒等)。真实意见 3 条(2 行级 + 1 body JSON 段)、处置
    JSON 只 2 条(漏 1 条 body)→ 抛 GhError,不结案。"""
    head = "d" * 40
    marker = pr_loop.LOCAL_CLAUDE_MARKER
    pr = pr_loop.PRData(
        number=9, title="t", state="open", draft=False, fork=False,
        head_sha=head, head_ref="feat/x", base_ref="main",
        head_time=pr_loop.parse_ts("2026-10-15T00:00:00Z"),
        url="https://example.invalid/9", author="wangxumarshall",
        reviews=[
            # 1 条 body JSON 段 finding(line=None)
            {"id": 3, "user": {"login": "wangxumarshall"},
             "state": "COMMENTED", "commit_id": head,
             "submitted_at": "2026-10-15T00:01:00Z",
             "body": _local_review_body(1)},
        ],
        review_comments=[
            {"id": 101, "user": {"login": "wangxumarshall"},
             "body": f"{marker}\n**[Critical] 正确性**: x",
             "created_at": "2026-10-15T00:02:00Z",
             "original_commit_id": head, "commit_id": head,
             "path": "tests/a.c", "line": 10},
            {"id": 102, "user": {"login": "wangxumarshall"},
             "body": f"{marker}\n**[Important] 性能**: y",
             "created_at": "2026-10-15T00:02:00Z",
             "original_commit_id": head, "commit_id": head,
             "path": "tests/b.c", "line": 20},
        ],
        issue_comments=[],
    )
    monkeypatch.setattr(pr_loop, "resolve_slug", lambda explicit=None: "test/repo")
    monkeypatch.setattr(pr_loop, "fetch_pr", lambda slug, num: pr)
    monkeypatch.setattr(pr_loop, "reply_to_findings",
                        lambda pr_num, findings, replies: None)

    # 处置 JSON 只含 2 条行级,漏掉 body JSON 段的那 1 条
    disp = tmp_path / "disp.json"
    disp.write_text(json.dumps([
        {"path": "tests/a.c", "line": 10, "id": 101, "conclusion": "采纳",
         "reason": "r", "evidence": "已修复于 commit " + "f" * 40},
        {"path": "tests/b.c", "line": 20, "id": 102, "conclusion": "采纳",
         "reason": "r", "evidence": "已修复于 commit " + "f" * 40},
    ]))
    args = pr_loop.build_parser().parse_args(["reply", "9", "-f", str(disp)])
    with pytest.raises(pr_loop.GhError):
        pr_loop.cmd_reply(args)


def test_reply_verifies_completeness_multi_bot_mixed(monkeypatch, tmp_path):
    """C1 修残余(Important): body 口径对齐 fetch_pr 的 opinion_count——覆盖五 bot
    全部非行级待处置意见。真实意见 = local-claude body finding(1)+ coderabbit
    行级(1)+ pr-agent ACTIVE issue 评论(1);处置 JSON 全答 3 条 → 通过;漏 1 条
    body → 抛 GhError。"""
    head = "d" * 40
    pragent_body = (
        "<!-- pr-agent:review:full -->\n"
        "检视意见\n"
        "<!-- pr-agent-review-state:v1 "
        '{"last_run": {"head_sha": "' + head + '"}, '
        '"findings": [{"state": "ACTIVE", "description": "outside-diff"}]}'
        " -->"
    )
    pr = pr_loop.PRData(
        number=9, title="t", state="open", draft=False, fork=False,
        head_sha=head, head_ref="feat/x", base_ref="main",
        head_time=pr_loop.parse_ts("2026-10-15T00:00:00Z"),
        url="https://example.invalid/9", author="wangxumarshall",
        reviews=[
            {"id": 3, "user": {"login": "wangxumarshall"},
             "state": "COMMENTED", "commit_id": head,
             "submitted_at": "2026-10-15T00:01:00Z",
             "body": _local_review_body(1)},
        ],
        review_comments=[
            {"id": 201, "user": {"login": "coderabbitai[bot]"},
             "body": "行级意见",
             "created_at": "2026-10-15T00:02:00Z",
             "original_commit_id": head, "commit_id": head,
             "path": "tests/a.c", "line": 10},
        ],
        issue_comments=[
            {"id": 999, "user": {"login": "github-actions[bot]"},
             "body": pragent_body,
             "created_at": "2026-10-15T00:03:00Z"},
        ],
    )
    monkeypatch.setattr(pr_loop, "resolve_slug", lambda explicit=None: "test/repo")
    monkeypatch.setattr(pr_loop, "fetch_pr", lambda slug, num: pr)
    monkeypatch.setattr(pr_loop, "reply_to_findings",
                        lambda pr_num, findings, replies: None)

    # 全答: 1 行级(coderabbit)+ 2 body(local-claude / pr-agent ACTIVE)
    sha = "f" * 40
    disp = tmp_path / "disp.json"
    disp.write_text(json.dumps([
        {"path": "tests/a.c", "line": 10, "id": 201, "conclusion": "采纳",
         "reason": "r", "evidence": "已修复于 commit " + sha},
        {"path": "tests/sve/fma_sve.c", "line": None, "conclusion": "驳回",
         "reason": "误报", "evidence": "实证: ninja 零警告"},
        {"path": "tests/pr-agent.c", "line": None, "conclusion": "驳回",
         "reason": "误报", "evidence": "实证: ninja 零警告"},
    ]))
    args = pr_loop.build_parser().parse_args(["reply", "9", "-f", str(disp)])
    assert pr_loop.cmd_reply(args) == 0

    # 漏 1 条 body(pr-agent ACTIVE 未答)→ body 条数不符,抛错
    disp2 = tmp_path / "disp2.json"
    disp2.write_text(json.dumps([
        {"path": "tests/a.c", "line": 10, "id": 201, "conclusion": "采纳",
         "reason": "r", "evidence": "已修复于 commit " + sha},
        {"path": "tests/sve/fma_sve.c", "line": None, "conclusion": "驳回",
         "reason": "误报", "evidence": "实证: ninja 零警告"},
    ]))
    args2 = pr_loop.build_parser().parse_args(["reply", "9", "-f", str(disp2)])
    with pytest.raises(pr_loop.GhError):
        pr_loop.cmd_reply(args2)


def test_reply_verifies_completeness_same_line_two_bots(monkeypatch):
    """修残余(Low): 同 (path,line) 两条不同 bot 的行级评论,处置 JSON 精确覆盖
    两条 id 时不误判「id 不匹配」(旧 real_by_loc 只保留一行一 id);错绑 id 仍被
    三元组集合差拒绝。"""
    head = "d" * 40
    pr = pr_loop.PRData(
        number=9, title="t", state="open", draft=False, fork=False,
        head_sha=head, head_ref="feat/x", base_ref="main",
        head_time=pr_loop.parse_ts("2026-10-15T00:00:00Z"),
        url="https://example.invalid/9", author="wangxumarshall",
        review_comments=[
            {"id": 301, "user": {"login": "coderabbitai[bot]"},
             "body": "coderabbit 意见",
             "created_at": "2026-10-15T00:02:00Z",
             "original_commit_id": head, "commit_id": head,
             "path": "tests/a.c", "line": 10},
            {"id": 302, "user": {"login": "chatgpt-codex-connector[bot]"},
             "body": "codex 意见",
             "created_at": "2026-10-15T00:02:00Z",
             "original_commit_id": head, "commit_id": head,
             "path": "tests/a.c", "line": 10},
        ],
    )
    sha = "f" * 40
    findings = [
        {"path": "tests/a.c", "line": 10, "id": 301},
        {"path": "tests/a.c", "line": 10, "id": 302},
    ]
    replies = [{"conclusion": "采纳", "reason": "r",
                "evidence": f"已修复于 commit {sha}"} for _ in range(2)]
    # 精确覆盖两条 id → 不抛错(旧 real_by_loc 会误判 id 不匹配)
    pr_loop.verify_reply_completeness(pr, findings, replies)

    # 错绑: 第二条 id 写成不存在的 303 → 三元组集合差仍拒绝
    bad = [
        {"path": "tests/a.c", "line": 10, "id": 301},
        {"path": "tests/a.c", "line": 10, "id": 303},
    ]
    with pytest.raises(pr_loop.GhError):
        pr_loop.verify_reply_completeness(pr, bad, replies)


def test_collect_entries_include_local_claude_and_id():
    """collect 导出不认 local-claude 的缺陷(Important 2): reviews/行级评论/
    issue 评论三处改用与计数一致的 local_claude_comment 双条件;行级评论附 id
    供 reply_to_findings 逐条 reply 定位。"""
    head = "a" * 40
    marker = pr_loop.LOCAL_CLAUDE_MARKER
    pr = pr_loop.PRData(
        number=9, title="t", state="open", draft=False, fork=False,
        head_sha=head, head_ref="feat/x", base_ref="main",
        head_time=pr_loop.parse_ts("2026-10-15T00:00:00Z"),
        url="https://example.invalid/9", author="wangxumarshall",
        reviews=[
            {"id": 1, "user": {"login": "wangxumarshall"},
             "state": "COMMENTED", "commit_id": head,
             "submitted_at": "2026-10-15T00:01:00Z",
             "body": _local_review_body(2)},
        ],
        review_comments=[
            {"id": 12345, "user": {"login": "wangxumarshall"},
             "body": f"{marker}\n**[Critical] 正确性**: x",
             "created_at": "2026-10-15T00:02:00Z",
             "original_commit_id": head, "commit_id": head,
             "path": "tests/sve/fma_sve.c", "line": 42},
        ],
        issue_comments=[
            {"id": 999, "user": {"login": "wangxumarshall"},
             "body": f"{marker}\n检视意见",
             "created_at": "2026-10-15T00:03:00Z"},
        ],
    )
    entries = pr_loop._collect_entries(pr)
    titles = " | ".join(t for _, t, _ in entries)
    assert "[local-claude] Review" in titles
    assert "[local-claude] 行级评论" in titles and "id=12345" in titles
    assert "[local-claude] PR 评论" in titles


# ---------------------------------------------------------------------------
# PR #289 检视闭环: 时钟偏差容差收窄(仅回退锚)+ pr-agent issue 评论严格锚
# 容差 + era_mark 口径对齐。

_ANCHOR = pr_loop.parse_ts("2026-10-24T12:00:00Z")   # 锚定下界
_TOL = pr_loop.CLOCK_SKEW_TOLERANCE                  # timedelta(seconds=300)


def test_commit_strict_match_three_states():
    head = "a" * 40
    # 1) 锚定字段存在且 == head → True
    assert pr_loop.commit_strict_match({"commit_id": head}, ("commit_id",), head)
    # 2) 字段存在但 != head → False
    assert not pr_loop.commit_strict_match(
        {"commit_id": "b" * 40}, ("original_commit_id", "commit_id"), head)
    # 3) 字段缺失 → False(严格锚不认缺失——区别于 commit_anchored 的回退 True)
    assert not pr_loop.commit_strict_match({}, ("commit_id",), head)


def test_head_anchored_skew_zero_is_strict():
    """意见 1: push_time 锚(服务器钟)skew=0——锚前 1s 的严格匹配条目也排除,
    防同 SHA 强推回退;锚后照常计入。"""
    head = "a" * 40
    before = {"commit_id": head, "submitted_at": "2026-10-24T11:59:59Z"}
    assert not pr_loop.head_anchored(before, "submitted_at", _ANCHOR,
                                     ("commit_id",), head, timedelta(0))
    after = {"commit_id": head, "submitted_at": "2026-10-24T12:00:01Z"}
    assert pr_loop.head_anchored(after, "submitted_at", _ANCHOR,
                                 ("commit_id",), head, timedelta(0))


def test_head_anchored_skew_300s_boundaries():
    """回退锚容差边界: -300s(含)计入 / -301s 排除;窗口内字段缺失 False、
    锚后字段缺失 True(commit_anchored 兜底);ts None False。"""
    head = "a" * 40
    # 锚前整 300s → 窗口下界含端点,计入(严格匹配)
    at_bound = {"commit_id": head, "submitted_at": "2026-10-24T11:55:00Z"}
    assert pr_loop.head_anchored(at_bound, "submitted_at", _ANCHOR,
                                 ("commit_id",), head, _TOL)
    # 锚前 301s → 排除
    past_bound = {"commit_id": head, "submitted_at": "2026-10-24T11:54:59Z"}
    assert not pr_loop.head_anchored(past_bound, "submitted_at", _ANCHOR,
                                     ("commit_id",), head, _TOL)
    # 窗口内但锚定字段缺失 → False(放宽以严格锚为前提,缺失无证据)
    no_field = {"submitted_at": "2026-10-24T11:59:00Z"}
    assert not pr_loop.head_anchored(no_field, "submitted_at", _ANCHOR,
                                     ("commit_id",), head, _TOL)
    # 锚后字段缺失 → True(常规路径维持 commit_anchored 的缺失回退)
    no_field_after = {"submitted_at": "2026-10-24T12:01:00Z"}
    assert pr_loop.head_anchored(no_field_after, "submitted_at", _ANCHOR,
                                 ("commit_id",), head, _TOL)
    # ts 缺失/畸形 → False
    assert not pr_loop.head_anchored({"commit_id": head}, "submitted_at",
                                     _ANCHOR, ("commit_id",), head, _TOL)


def _pragent_issue_comment(head, comment_head, ts):
    """构造一条 pr-agent 检视 issue 评论(标记 + 尾部 review-state JSON)。"""
    return {"id": 999, "user": {"login": "github-actions[bot]"},
            "body": "<!-- pr-agent:review:full -->\n检视意见\n"
                    "<!-- pr-agent-review-state:v1 "
                    '{"last_run": {"head_sha": "' + comment_head + '"}}'
                    " -->",
            "created_at": ts}


def test_issue_comment_anchored_four_states():
    """意见 2: issue 评论计入判定四态——锚后计入;窗口内 pr-agent 严格锚命中
    计入;窗口内无绑定评论不放宽;窗口内 pr-agent 锚不命中不计入。"""
    head = "a" * 40
    # 1) ts ≥ anchor → 计入(任意 bot,无绑定字段的 coderabbit 也计入)
    after = {"user": {"login": "coderabbitai[bot]"},
             "body": "x", "created_at": "2026-10-24T12:00:00Z"}
    assert pr_loop.issue_comment_anchored(after, _ANCHOR, head, _TOL)
    # 2) 窗口内(锚前 120s)pr-agent 且 last_run.head_sha == head → 计入
    pragent_ok = _pragent_issue_comment(head, head, "2026-10-24T11:58:00Z")
    assert pr_loop.issue_comment_anchored(pragent_ok, _ANCHOR, head, _TOL)
    # 3) 窗口内无 commit 绑定字段的评论 → 不计入(放宽须以可证属于当前 head 为前提)
    unbound = {"user": {"login": "coderabbitai[bot]"},
               "body": "x", "created_at": "2026-10-24T11:58:00Z"}
    assert not pr_loop.issue_comment_anchored(unbound, _ANCHOR, head, _TOL)
    # 4) 窗口内 pr-agent 但 last_run.head_sha != head(旧 head 检视)→ 不计入
    pragent_old = _pragent_issue_comment(head, "b" * 40, "2026-10-24T11:58:00Z")
    assert not pr_loop.issue_comment_anchored(pragent_old, _ANCHOR, head, _TOL)
    # 补: push_time 锚 skew=0 → 窗口为空,pr-agent 严格命中也排除(同意见 1 语义)
    assert not pr_loop.issue_comment_anchored(pragent_ok, _ANCHOR, head,
                                              timedelta(0))
    # 补: ts 缺失 → False
    assert not pr_loop.issue_comment_anchored(
        {"user": {"login": "coderabbitai[bot]"}, "body": "x"},
        _ANCHOR, head, _TOL)


def test_fetch_pr_skew_derived_from_push_time(monkeypatch):
    """意见 1(端到端): 容差由锚来源推导——push_time 可得(服务器钟)时 skew=0,
    锚前条目严格排除;events 翻不到回退 committer date(本机钟盖戳)时放宽
    300s,严格 commit 锚命中的条目在锚前窗口内计入。"""
    head = "e" * 40
    # review 落在 committer date 前 60s(本机钟快于服务器的实证形态)、
    # commit_id 严格匹配 head
    review = {"id": 1, "user": {"login": "greptile-apps[bot]"},
              "state": "COMMENTED", "commit_id": head,
              "submitted_at": "2026-10-24T11:54:00Z", "body": "意见"}
    pr_detail = {"number": 9, "title": "t", "state": "open", "draft": False,
                 "head": {"sha": head, "ref": "feat/x",
                          "repo": {"full_name": "test/repo"}},
                 "base": {"ref": "main", "repo": {"full_name": "test/repo"}},
                 "user": {"login": "wangxumarshall"},
                 "html_url": "https://example.invalid/9"}
    commit_detail = {"commit": {
        "committer": {"date": "2026-10-24T11:55:00Z"},
        "author": {"date": "2026-10-24T11:55:00Z"}}}
    push_event = {"type": "PushEvent",
                  "created_at": "2026-10-24T12:00:00Z",
                  "payload": {"head": head, "ref": "refs/heads/feat/x"}}

    def run_fetch(events):
        def fake_gh_api(path):
            if path == "repos/test/repo/pulls/9":
                return pr_detail
            if path == f"repos/test/repo/commits/{head}":
                return commit_detail
            if path.startswith("repos/test/repo/events"):
                return events
            raise AssertionError(f"意外 gh_api 调用: {path}")

        def fake_paged(path, list_key=None):
            if "check-runs" in path:
                return []
            if "/reviews" in path:
                return [review]
            if "/issues/9/comments" in path:
                return []
            if "/comments" in path:
                return []
            raise AssertionError(f"意外 gh_api_paged 调用: {path}")

        monkeypatch.setattr(pr_loop, "gh_api", fake_gh_api)
        monkeypatch.setattr(pr_loop, "gh_api_paged", fake_paged)
        pr_loop._EVENTS_CACHE.clear()   # events 进程内缓存,防两轮互相污染
        return pr_loop.fetch_pr("test/repo", 9)

    # A) push_time 可得(锚 12:00,skew=0): review 落锚前 6 分钟 → 严格排除
    pr = run_fetch([push_event])
    assert pr.push_time == pr_loop.parse_ts("2026-10-24T12:00:00Z")
    assert pr.opinion_breakdown["review"] == 0
    # B) events 翻不到 → 回退 committer date 锚(11:55,skew=300s):
    #    review 落锚前 60s,严格 commit 锚命中 → 计入
    pr2 = run_fetch([])
    assert pr2.push_time is None
    assert pr2.opinion_breakdown["review"] == 1


def test_collect_era_mark_aligned_with_counting():
    """era_mark 阈值与计数同口径: 回退锚容差窗口内(锚前 ≤300s)计入的条目
    标「当前 head」;锚前 >300s 标「历史轮次」;push_time 锚(严格)下锚前
    条目即使 <300s 也标「历史轮次」——与 head_anchored(skew=0)判定一致。"""
    head = "a" * 40
    pr = pr_loop.PRData(
        number=9, title="t", state="open", draft=False, fork=False,
        head_sha=head, head_ref="feat/x", base_ref="main",
        head_time=pr_loop.parse_ts("2026-10-24T11:55:00Z"),
        url="https://example.invalid/9", author="wangxumarshall",
        reviews=[
            {"id": 1, "user": {"login": "greptile-apps[bot]"},
             "state": "COMMENTED", "commit_id": head,
             "submitted_at": "2026-10-24T11:54:00Z", "body": "意见"},
        ],
    )
    # push_time 缺失 → 回退锚 11:55 放宽 300s: 锚前 60s 计入 → 标「当前 head」
    entries = pr_loop._collect_entries(pr)
    assert len(entries) == 1
    assert "当前 head" in entries[0][1]
    assert "历史轮次" not in entries[0][1]
    # 锚前 45 分钟(>300s)→ 排除且标「历史轮次」
    pr.reviews[0]["submitted_at"] = "2026-10-24T11:10:00Z"
    entries2 = pr_loop._collect_entries(pr)
    assert "历史轮次" in entries2[0][1]
    # push_time 可得(锚 12:00,skew=0): 锚前 60s 排除 → 也标「历史轮次」
    pr.push_time = pr_loop.parse_ts("2026-10-24T12:00:00Z")
    pr.reviews[0]["submitted_at"] = "2026-10-24T11:59:00Z"
    entries3 = pr_loop._collect_entries(pr)
    assert "历史轮次" in entries3[0][1]


def test_fetch_pr_records_skew_suspects(monkeypatch, capsys):
    """意见 5(超带诊断): commit 锚严格匹配却落在锚前 >容差(回退锚 300s)的
    条目记入 skew_suspects(kind, ts, 偏差秒);status 单 PR 详情与 collect
    元信息均给出告警(条数 + 最远偏差秒);无超带时无告警。"""
    head = "e" * 40
    # 回退锚 = committer date 11:55(本机钟盖戳):
    # - 超带条目: 11:48:20(锚前 400s,> 300s 容差,commit_id 严格匹配)→ 记诊断;
    # - 正常计入条目: 11:54:00(锚前 60s,容差窗口内)→ 不记。
    suspect = {"id": 1, "user": {"login": "greptile-apps[bot]"},
               "state": "COMMENTED", "commit_id": head,
               "submitted_at": "2026-10-24T11:48:20Z", "body": "超带意见"}
    counted = {"id": 2, "user": {"login": "coderabbitai[bot]"},
               "state": "COMMENTED", "commit_id": head,
               "submitted_at": "2026-10-24T11:54:00Z", "body": "窗口内意见"}
    pr_detail = {"number": 9, "title": "t", "state": "open", "draft": False,
                 "head": {"sha": head, "ref": "feat/x",
                          "repo": {"full_name": "test/repo"}},
                 "base": {"ref": "main", "repo": {"full_name": "test/repo"}},
                 "user": {"login": "wangxumarshall"},
                 "html_url": "https://example.invalid/9"}
    commit_detail = {"commit": {
        "committer": {"date": "2026-10-24T11:55:00Z"},
        "author": {"date": "2026-10-24T11:55:00Z"}}}

    def fake_gh_api(path):
        if path == "repos/test/repo/pulls/9":
            return pr_detail
        if path == f"repos/test/repo/commits/{head}":
            return commit_detail
        if path.startswith("repos/test/repo/events"):
            return []   # events 翻不到 → 回退锚(committer date),skew=300s
        raise AssertionError(f"意外 gh_api 调用: {path}")

    def fake_paged(path, list_key=None):
        if "check-runs" in path:
            return []
        if "/reviews" in path:
            return [suspect, counted]
        if "/issues/9/comments" in path:
            return []
        if "/comments" in path:
            return []
        raise AssertionError(f"意外 gh_api_paged 调用: {path}")

    monkeypatch.setattr(pr_loop, "gh_api", fake_gh_api)
    monkeypatch.setattr(pr_loop, "gh_api_paged", fake_paged)
    pr_loop._EVENTS_CACHE.clear()
    pr = pr_loop.fetch_pr("test/repo", 9)

    # 仅超带条目记入(窗口内计入的不记),偏差 = 锚 - ts = 400s
    assert len(pr.skew_suspects) == 1
    kind, ts, delta = pr.skew_suspects[0]
    assert kind == "review"
    assert ts == pr_loop.parse_ts("2026-10-24T11:48:20Z")
    assert delta == 400.0
    # 超带条目被时间锚排除(不计入意见),诊断如实告警而非静默丢弃
    assert pr.opinion_breakdown["review"] == 1

    warn = pr_loop.format_skew_warning(pr)
    assert warn is not None
    assert "疑似时钟超带或同 SHA 回退" in warn
    assert "1 条" in warn and ">300s" in warn and "400s" in warn

    # status 单 PR 详情打印告警
    pr_loop.print_detail(pr, "test/repo", 900)
    out = capsys.readouterr().out
    assert "疑似时钟超带或同 SHA 回退" in out

    # collect 元信息同样给出告警行
    md = pr_loop.render_collect(pr, "test/repo", 900)
    assert "疑似时钟超带或同 SHA 回退" in md

    # 无超带(全部条目锚后/窗口内)→ 无告警
    pr.skew_suspects.clear()
    assert pr_loop.format_skew_warning(pr) is None
