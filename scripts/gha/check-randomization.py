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
#   注释与字符串字面量内容不判定(按完整源文件的注释/字符串状态机
#   strip_code_lines 精确剥离,跨行块注释状态正确);存量代码不在 diff
#   新增行内,不追溯。
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

TESTS_PATHSPEC = ["tests/*.cpp"]


def strip_code_lines(src):
    """按完整源文件做注释/字符串状态机,返回每行的纯代码部分(含换行符计数)。

    PR #244 review (greptile): 纯行级正则无法裁决跨行块注释状态——
    新增行 "/* Do not use std::mt19937 here"(未闭合的块注释首行)会被
    当真实代码误报 CI 失败;反之 "*p = std::mt19937(seed)();"(行首 *
    是解引用,代码)会被当块注释续行整行漏检。改为逐字符扫描整个文件:
    跟踪块注释 / 行注释 / 字符串字面量 / 字符字面量 / 原始字符串
    (C++ raw string R"(...)") / pp-number 六种状态,行内状态切换正确
    处理(如 code /* comment */ code),产出每行剥除注释与字符串内容
    后的代码。字符串内容替换为空格(保留长度),避免字符串字面量里的
    "rand()" 命中 R1。

    PR #244 review round 4 (greptile) 修正:
    - 字符串/字符字面量的反斜杠行续接必须保留换行符("aaa\\<newline>
      bbb" 的换行被替换为空格会让 code_lines 行号与原文件错位,后续
      行全部检查错行)。
    - C++ 数字分隔符 1'000 的 ' 不是字符字面量开头——pp-number 状态
      连续消费数字记号,否则 constexpr int count = 1'000; 之后的 RNG
      代码会被当字符内容整段删除,CI 漏报。
    - 原始字符串在引号处回看 R/uR/u8R/UR/LR 前缀(前缀字符本身已按
      代码追加,此处一并清空)——原实现 re.match(r'R"...', src[i:])
      在引号位置永远不命中,raw string 分支是死分支,R"(...)" 内的
      mt19937 会被当代码误报。
    """
    n = len(src)
    code = []           # 与 src 等长的字符列表,注释/字符串位置换成空格
    i = 0
    in_block = False    # /* ... */
    in_line = False     # // ...
    in_str = False      # "..."
    in_char = False     # '...'
    raw_end = None      # R"delim( ... )delim" 的闭合标记
    while i < n:
        c = src[i]
        nxt = src[i + 1] if i + 1 < n else ""
        if in_line:
            if c == "\n":
                in_line = False
                code.append(c)
            else:
                code.append(" ")
            i += 1
        elif in_block:
            if c == "*" and nxt == "/":
                code.extend("  ")
                in_block = False
                i += 2
            else:
                code.append("\n" if c == "\n" else " ")
                i += 1
        elif in_str:
            if c == "\\":
                if nxt == "\n":
                    # 行续接:反斜杠清空,换行保留(行号对齐)
                    code.append(" ")
                    code.append("\n")
                    i += 2
                elif nxt:
                    code.extend("  ")
                    i += 2
                else:
                    code.append(" ")
                    i += 1
            elif c == '"':
                in_str = False
                code.append(" ")
                i += 1
            else:
                code.append("\n" if c == "\n" else " ")
                i += 1
        elif in_char:
            if c == "\\":
                if nxt == "\n":
                    code.append(" ")
                    code.append("\n")
                    i += 2
                elif nxt:
                    code.extend("  ")
                    i += 2
                else:
                    code.append(" ")
                    i += 1
            elif c == "'":
                in_char = False
                code.append(" ")
                i += 1
            else:
                code.append("\n" if c == "\n" else " ")
                i += 1
        else:
            if raw_end is not None:
                if src.startswith(raw_end, i):
                    for _ in raw_end:
                        code.append(" ")
                    i += len(raw_end)
                    raw_end = None
                else:
                    code.append("\n" if c == "\n" else " ")
                    i += 1
            elif c == "/" and nxt == "*":
                code.extend("  ")
                in_block = True
                i += 2
            elif c == "/" and nxt == "/":
                code.extend("  ")
                in_line = True
                i += 2
            elif c == '"':
                # C++ 原始字符串:R"delim( ... )delim" — delim 可为空或
                # ≤16 个非括号字符。greptile: 在引号处回看前缀字符
                # (R 本身已被当代码追加,这里连同前缀一并清空),再匹配
                # "delim( 开头;不匹配则按普通字符串处理。
                prefix = None
                if i > 0 and src[i - 1] == "R":
                    ps = i - 1
                    k = i - 2
                    while k >= 0 and (src[k].isalnum() or src[k] == "_"):
                        ps = k
                        k -= 1
                    if src[ps:i - 1] in ("", "u", "u8", "U", "L"):
                        prefix = ps
                m = re.match(r'"([^()\\\s]{0,16})\(', src[i:]) if prefix is not None else None
                if m:
                    for t in range(prefix, i):   # 清空前缀(含 R)
                        code[t] = " "
                    raw_end = ")" + m.group(1) + '"'
                    for _ in m.group(0):        # "delim( 整段清空
                        code.append(" ")
                    i += len(m.group(0))
                else:
                    in_str = True
                    code.append(" ")
                    i += 1
            elif c == "'":
                in_char = True
                code.append(" ")
                i += 1
            elif c.isdigit() and (i == 0 or not (src[i - 1].isalnum() or src[i - 1] == "_")) or \
                    (c == "." and nxt.isdigit() and (i == 0 or not (src[i - 1].isalnum() or src[i - 1] == "_"))):
                # pp-number: 数字(或 .数字)开头的记号连续消费
                # [0-9A-Za-z_.'] 与 e/E/p/P 后紧跟的 +/-,使数字分隔符
                # 1'000'000 / 0x1'F / 1e-5 中的 ' 不被误当字符字面量
                # 开头(greptile: 否则其后的 RNG 代码被当字符内容删除)。
                # 记号本身是代码,原样保留。
                j = i + 1
                while j < n:
                    cj = src[j]
                    if cj.isalnum() or cj in "._'":
                        j += 1
                    elif cj in "+-" and src[j - 1] in "eEpP":
                        j += 1
                    else:
                        break
                code.extend(src[i:j])
                i = j
            else:
                code.append(c)
                i += 1
    return "".join(code).splitlines(keepends=True)


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
        code_lines = strip_code_lines(src)

        # R1: 新增行引入自建 RNG → error(注释/字符串内容不算;R3-justified 不豁免)。
        # PR #244 review (greptile): 判定基于完整文件的注释/字符串状态机产出
        # (strip_code_lines),行号与 src 一一对应——跨行块注释的正确性由状态机
        # 保证,而非行首启发式。
        for lineno, text in adds:
            code = code_lines[lineno - 1].rstrip("\r\n") if lineno - 1 < len(code_lines) else ""
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
