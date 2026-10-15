"""pr_local_review 对抗性检视 prompt 引擎(Task 1)测试。

用例口径(Task 1 brief 2 例,全离线、不触网、不碰生产状态):
  1 test_build_adversarial_prompt_has_five_elements: 对一段含 memcmp 的 diff
    生成 prompt,断言含「pentester/攻击者」framing、「Skeptic/Architect/
    Minimalist」多镜头、「file:line + 证据」锚定要求、「意图/scope」镜、
    「先看删除再查新增」差分顺序——对抗性五要素全落地;
  2 test_prompt_contains_sdc_checks: 断言 prompt 含 AGENTS.md「SDC 激发能力」
    关键词 memcmp / per-lane / TEST_LOOP / FP 可复现。用不含这些关键词的
    diff 作输入,确保关键词只能来自 prompt 自身的 SDC 核查段,而非 diff。
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "..", "scripts"))
import pr_local_review as plr


def _memcmp_diff():
    """含 memcmp 且带删除/新增行的样例 diff(五要素用例输入)。"""
    return (
        "diff --git a/tests/sve/fma_sve.c b/tests/sve/fma_sve.c\n"
        "--- a/tests/sve/fma_sve.c\n"
        "+++ b/tests/sve/fma_sve.c\n"
        "@@ -12,6 +12,5 @@\n"
        "-    int rc = memcmp(actual, golden, sizeof(actual));\n"
        "-    if (rc) report_fail();\n"
        "+    memcmp_or_fail(actual, golden, sizeof(actual), \"fma_sve\");\n"
    )


def _plain_diff():
    """不含任何 SDC 关键词的样例 diff(SDC 核查用例输入)。"""
    return (
        "diff --git a/scripts/pr_loop.py b/scripts/pr_loop.py\n"
        "--- a/scripts/pr_loop.py\n"
        "+++ b/scripts/pr_loop.py\n"
        "@@ -70,7 +70,7 @@\n"
        "-def classify_state(pr, quiet, now):\n"
        "+def classify_state(pr, quiet, now, local_bot=None):\n"
    )


def test_build_adversarial_prompt_has_five_elements():
    ctx = {"title": "FMA SVE 测试字节级比对",
           "body": "修复 golden 比对,防止误报",
           "issue": "关联 issue #123"}
    prompt = plr.build_adversarial_prompt(_memcmp_diff(), ctx)

    # 1 pentester framing(对抗性渗透测试者,而非礼貌建议)
    assert "pentester" in prompt
    assert "攻击者" in prompt
    # 2 多批判镜头(Skeptic / Architect / Minimalist 须全在场)
    assert "Skeptic" in prompt
    assert "Architect" in prompt
    assert "Minimalist" in prompt
    # 3 结构化证据锚定(file:line + 证据,禁空泛)
    assert "file:line" in prompt
    assert "证据" in prompt
    # 4 意图镜(scope creep / drift)
    assert "意图" in prompt
    assert "scope" in prompt
    # 5 差分审查顺序(先看删除、再查新增)
    assert "删除" in prompt
    assert "新增" in prompt


def test_prompt_contains_sdc_checks():
    prompt = plr.build_adversarial_prompt(_plain_diff(), {"title": "x", "body": ""})

    # SDC 激发能力关键词必须来自 prompt 自身(输入 diff 不含它们)
    assert "memcmp" in prompt
    assert "per-lane" in prompt
    assert "TEST_LOOP" in prompt
    assert "可复现" in prompt