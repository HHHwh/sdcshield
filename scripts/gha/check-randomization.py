#!/usr/bin/env python3
# check-randomization.py — PR 随机性门禁(审查发现 F-009 / 随机性分级协议 §1b)。
#
# 背景:框架 RNG(random32/random64/memset_random/frandom*)的种子由框架统一管理并
# 随 -vvv 落盘(logging.cpp random_generator_state),失败可精确重放;而自建 RNG
# (mt19937/random_device/rand())种子不受框架控制,失败后无法复现——生产复现断链。
#
# 对 diff(target..head) 中新增/修改的 tests/**.cpp 做分级检查:
#   R1(::error::, exit 1): 新增代码引入自建不可重放 RNG(mt19937/random_device/
#       rand()/srand())。必须改用框架 RNG;R3-justified: 不豁免 R1。
#   R2/R3(::warning::): 新增固定 pattern 大表(static const ... [] = {)且文件
#       全文无任何 RNG 符号。固定 pattern 对 SDC 激发覆盖有限;若属有意为之
#       (确定性回归向量等),在文件内加 "R3-justified: <理由>" 注释豁免。
#   注释行(// /* *)不判定;存量代码不在 diff 新增行内,不追溯。
#
# 与 .github/scripts/check-no-tabs.sh 同接口,用于 GitHub Actions lint job:
#   python3 scripts/gha/check-randomization.py origin/<base-ref> HEAD
# 本地: python3 scripts/gha/check-randomization.py origin/main HEAD
# SPDX-License-Identifier: Apache-2.0
import re
import subprocess
import sys

# 框架 RNG 符号(sandstone.h random32/random64/memset_random 及模板重载、frandom*)
FRAMEWORK_RNG_RE = re.compile(r"\brandom32\s*\(|\brandom64\s*\(|\bmemset_random\b|\bfrandom[a-z_]*\s*\(")
# 自建不可重放 RNG(分级协议 R1)。行首或非标识符字符后的 rand(/srand( 才算裸调用,
# 排除 myrand(/_srand( 等无关标识符;mt19937 同时覆盖 mt19937_64;minstd_rand 与
# default_random_engine 同为不受框架管理的自建引擎(含 minstd_rand0/ranlux/knuth_b
# 等别名前缀),一并拦截。
SELF_RNG_RE = re.compile(
    r"mt19937|random_device|minstd_rand|default_random_engine"
    r"|(?:\A|[^_a-zA-Z0-9])s?rand\s*\(")
# 固定 pattern 大表形态(R2/R3 提示对象):static const/constexpr 数组初始化
STATIC_TABLE_RE = re.compile(r"\bstatic\s+(?:const|constexpr)\s+\w[^;=]*\[[^\]]*\]\s*=")
# 注释行(说明文本里的 mt19937 等不算违规):行首 //、块注释开始 /*、块注释续行 *
COMMENT_RE = re.compile(r"^\s*(//|/\*|\*)")
# 行内注释剥离(R1 前置处理):整行 //、/* ... */ 块、块注释续行(* 开头,吃到
# 闭合 */ 或行尾)、空白后跟 // 的行尾注释——剥完只对剩余代码部分执行 R1,
# 否则 "/* note */ std::mt19937 rng;" 这类前缀注释行会被当纯注释整行跳过。
# // 的空白保护与 TRAILING_COMMENT_RE 一致,避免误伤 "http://" 等字符串字面量。
INLINE_COMMENT_RE = re.compile(r"^\s*//.*$|/\*.*?\*/|^\s*\*.*?(?:\*/|$)|\s//.*$")
# 行尾注释剥离(仅"空白后跟 // 或 /*"才视为注释起,避免误伤 "http://" 等字符串字面量)
TRAILING_COMMENT_RE = re.compile(r"\s(//|/\*).*$")

TESTS_PATHSPEC = ["tests/*.cpp"]


def git(*args):
    p = subprocess.run(["git"] + list(args), stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, text=True)
    return p.returncode, p.stdout


def changed_test_files(target, head):
    """diff 中新增/修改/改名后命中 tests/**.cpp 的文件(删除的跳过)。"""
    rc, out = git("diff", "--name-only", "--diff-filter=AMR", f"{target}..{head}",
                  "--", *TESTS_PATHSPEC)
    if rc != 0:
        sys.stderr.write(out)
        sys.exit(2)
    return [f for f in out.splitlines() if f.strip()]


def added_lines(target, head, path):
    """该文件 diff 中新增的行,返回 (head 侧行号, 行内容) 列表(--unified=0 解析 @@)。"""
    rc, out = git("diff", f"{target}..{head}", "--unified=0", "--", path)
    if rc != 0:
        return []
    adds = []
    lineno = 0
    for ln in out.splitlines():
        if ln.startswith("@@"):
            m = re.search(r"\+(\d+)", ln)
            if m:
                lineno = int(m.group(1))
            continue
        if ln.startswith("+++") or not ln.startswith("+"):
            continue
        adds.append((lineno, ln[1:]))
        lineno += 1
    return adds


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: check-randomization.py <target-ref> <head>\n")
        return 2
    target, head = sys.argv[1], sys.argv[2]

    errors = []
    warnings = []
    for path in changed_test_files(target, head):
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                src = f.read()
        except OSError:
            continue  # head 侧不存在的残留路径,--diff-filter 已滤大部分
        justified = "R3-justified:" in src
        has_framework_rng = bool(FRAMEWORK_RNG_RE.search(src))
        has_self_rng = bool(SELF_RNG_RE.search(src))
        adds = added_lines(target, head, path)

        # R1: 新增行引入自建 RNG → error(注释行除外;R3-justified 不豁免)
        for lineno, text in adds:
            if COMMENT_RE.match(text):
                # 前缀块注释(/* note */)后可能还有代码,先剥离行内注释再判一次,
                # 防止 "/* note */ std::mt19937 rng;" 这类行被当纯注释整行跳过;
                # // 行与块注释续行(* / *)剥离后无代码,维持原来的跳过行为
                stripped = INLINE_COMMENT_RE.sub("", text)
                if not stripped.strip():
                    continue
                text = stripped
            code = TRAILING_COMMENT_RE.sub("", text)
            if SELF_RNG_RE.search(code):
                errors.append(
                    f"{path}:{lineno}: R1 自建不可重放 RNG(失败无法复现): {text.strip()} "
                    f"→ 改用框架 RNG(random32/random64/memset_random/frandom*)")

        # R2/R3: 新增固定 pattern 大表且文件全文无任何 RNG(框架或自建) → warning。
        # 有自建 RNG 的存量文件不属"纯固定 pattern"场景,不在此提示(存量 R1 由
        # PR 模板自验项覆盖,门禁只拦 diff 新增行)。
        if not justified and not has_framework_rng and not has_self_rng:
            for lineno, text in adds:
                if STATIC_TABLE_RE.search(text) and "{" in text:
                    warnings.append(
                        f"{path}:{lineno}: R2/R3 固定 pattern 大表且文件无框架 RNG"
                        f"(SDC 激发覆盖有限);有意为之请加 R3-justified: 注释")
                    break  # 每文件提示一次

    for w in warnings:
        print(f"::warning:: {w}")
    for e in errors:
        print(f"::error:: {e}")
    if errors:
        print("::error:: 新测试禁止自建 RNG(mt19937/random_device/rand/srand):"
              "种子不受框架管理,失败不可重放。改用框架 RNG 后随机种子会随 -vvv "
              "落盘(random_generator_state)供复现。")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
