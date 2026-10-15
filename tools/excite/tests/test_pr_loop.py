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