#!/usr/bin/env python3
# gitleaks_allowlist_probe.py — gitleaks 豁免回归探针。
#
# 定位: 把 .gitleaks.toml 的安全不变式从「注释约束」升级为「CI 可证伪断言」
# (PR #292 三轮检视实证: 未锚定的 d1f0be94 曾全绿通过 PR 门禁, run 40101478911
# success 与修复后 run 40258687491 success 不可区分; PR 口径 --first-parent
# <base>..<head> 只扫 PR 自身 commits, 历史/逃逸形态不在扫描集, 写错的豁免
# 会绿着合并、最迟 7 天后 schedule cron 才红)。
#
# 两层校验:
# 1. 形态(静态, 无需 gitleaks 二进制):
#    - [extend] useDefault 必须保留(缺它 = 静默关闭全部默认规则);
#    - [allowlist].regexes 每条必须 `(?i)^…$` 整值锚定且无分支 `|`(半锚定
#      分支 `^abc|def$` 与未锚定同害: 子串命中即豁免)。
# 2. 行为(动态, 需与 CI 同版本 gitleaks): 固定样本集双态扫描 —
#    baseline(gitleaks 内置默认规则, cwd 置于无配置的样本目录触发)下
#    5 样本必须全检出(阳性对照: 证明样本形态真实触发规则, 断言非空转);
#    加载仓库 .gitleaks.toml 后, golden 整值(含 hex 大小写两形)必须被豁免,
#    前拼/内嵌公开片段的逃逸样本与无关对照必须仍被检出。
#
# 判别实证(gitleaks 8.24.3 故障注入, 2026-10-26):
#   正则回归未锚定(d1f0be94 形态) → 逃逸样本静默消失 → 探针红
#   豁免值笔误(改 1 位 hex)      → golden 整值检出       → 探针红
#   删 [extend] useDefault        → 0 findings(检测整体
#                                  静默失效)             → 探针红
#   去 (?i)                       → 大写公开向量复报     → 探针红
#
# 用法:
#   scripts/gitleaks_allowlist_probe.py [--gitleaks PATH] [--config PATH]
#   gitleaks 缺省取 env GITLEAKS_BIN 或 PATH 上的 gitleaks(版本应与 CI
#   security.yaml 的 GITLEAKS_VERSION pin 一致, 脚本仅打印不强制);
#   config 缺省取本脚本所在仓库根的 .gitleaks.toml。
# 退出码: 0 = 全部断言通过; 1 = 任一断言失败(FAIL 行 + 汇总)。
#
# SPDX-License-Identifier: Apache-2.0
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 固定样本集。形态锚定(勿随意改写, 断言语义建立在这些形态上):
#   golden_vector.c  = run 38240202283 误报现场 crypto_test.cpp:28 的注释
#                      形态(FIPS-197 C.1 AES-128 key, 误报根因);
#   golden_upper.c   = 同一公开向量的 hex 大写等价写法(大小写复燃路径);
#   escape_prefix/escape_embed = PR #292 检视逃逸实证形态: 真实凭据
#                      前拼/内嵌公开向量片段, 未锚定正则会把它们静默豁免;
#   control.txt      = 无关 40 位 hex(阴性对照: useDefault 被删或默认规则
#                      被削弱时它将不再检出)。
# 「期望被检出」样本必须运行时拼接构造: 这些形态若以完整字面量写入本
# 文件, PR 口径扫描(扫 PR 自身提交, run 40276158101 实证)会把本文件当真
# 泄漏报红(github-pat/generic-api-key 命中本文件 61-63 行)。拼接后源文件
# 不含完整 secret 形态, 写盘样本与首版逐字节一致, 探针断言不变; 亦不得
# 为此加路径豁免(不变式 #2 禁止按路径豁免)。golden 两形为整值公开向量,
# 本仓豁免对其生效, 保留字面量即被 CI 扫描豁免。
_PUBLIC_VECTOR = "2b7e151628aed2a6abf7158809cf4f3c"  # FIPS-197 C.1, 已豁免
_CONTROL_SECRET = "8f1a7c93" + "e5b24d06" + "af31c8e5" + "7b02d941a7e6c820"
SAMPLES: dict[str, str] = {
    "golden_vector.c": f" *   key        = {_PUBLIC_VECTOR}\n",
    "golden_upper.c": f" *   key        = {_PUBLIC_VECTOR.upper()}\n",
    "escape_prefix.txt": f'aws_secret_access_key = "{_PUBLIC_VECTOR}Wq7zXj2L"\n',
    "escape_embed.txt": f'token = "ghp_{_PUBLIC_VECTOR}ABCD"\n',
    "control.txt": f'password = "{_CONTROL_SECRET}"\n',
}
EXPECT_EXEMPT = frozenset({"golden_vector.c", "golden_upper.c"})
EXPECT_DETECTED = frozenset(SAMPLES) - EXPECT_EXEMPT

# 豁免正则合规形态: `(?i)^` 开头、`$` 结尾、条目内无分支(有 `|` 时 RE2 按
# 分支求值, 半锚定分支等价于未锚定子串豁免)。
ANCHORED_SHAPE = re.compile(r"^\(\?i\)\^[^|]*\$$")


class ProbeFailure(Exception):
    pass


def check_shape(config_path: str) -> list[str]:
    """静态形态校验(不变式 #1/#4), 返回断言描述列表(异常即失败)。"""
    with open(config_path, "rb") as f:
        cfg = tomllib.load(f)
    if cfg.get("extend", {}).get("useDefault") is not True:
        raise ProbeFailure(
            "shape: [extend] useDefault must be true — without it this file "
            "replaces the whole default ruleset (= silently disabling all "
            "leak detection)"
        )
    regexes = cfg.get("allowlist", {}).get("regexes", [])
    if not regexes:
        raise ProbeFailure("shape: [allowlist].regexes is empty — nothing to probe")
    for i, entry in enumerate(regexes):
        if not ANCHORED_SHAPE.match(entry):
            raise ProbeFailure(
                f"shape: allowlist regexes[{i}] is not whole-value anchored "
                f"'(?i)^…$' without alternation: {entry!r} — unanchored/half-"
                "anchored entries silently exempt any real credential "
                "containing the fragment (PR #292 escape precedent)"
            )
    return [
        "shape: [extend] useDefault retained",
        *(f"shape: allowlist regexes[{i}] anchored (?i)^…$, no alternation"
          for i in range(len(regexes))),
    ]


def run_gitleaks(gitleaks: str, source: str, report: str,
                 config: str | None, cwd: str) -> set[str]:
    """跑一次 --no-git 扫描, 返回检出样本文件名集合。

    baseline 态(config=None)必须以无 .gitleaks.toml 的目录为 cwd, 否则
    gitleaks 会自动加载 cwd 上的仓库配置, 阳性对照失效。
    """
    if os.path.exists(report):
        os.unlink(report)
    cmd = [gitleaks, "detect", "--no-git", "--source", source,
           "--exit-code", "0", "--report-format", "json",
           "--report-path", report]
    if config:
        cmd += ["--config", config]
    proc = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd)
    if not os.path.exists(report):
        raise ProbeFailure(
            f"gitleaks produced no report (exit={proc.returncode}); "
            f"stderr tail: {proc.stderr.strip()[-400:]!r}"
        )
    try:
        with open(report, encoding="utf-8") as f:
            findings = json.load(f)
    except json.JSONDecodeError as e:
        raise ProbeFailure(f"gitleaks report is not valid JSON: {e}") from e
    # 报告 File 字段是相对 --source 的路径; 样本全在根, 取 basename 归一。
    return {os.path.basename(f.get("File", "")) for f in findings}


def main() -> int:
    parser = argparse.ArgumentParser(
        description="gitleaks allowlist invariants probe "
                    "(shape + behaviour, see module docstring)")
    parser.add_argument("--gitleaks",
                        default=os.environ.get("GITLEAKS_BIN", "gitleaks"),
                        help="gitleaks binary (default: $GITLEAKS_BIN or PATH)")
    parser.add_argument("--config",
                        default=os.path.join(REPO_ROOT, ".gitleaks.toml"),
                        help="gitleaks config to probe")
    args = parser.parse_args()
    if not os.path.isfile(args.config):
        print(f"probe: FAIL — config not found: {args.config}", file=sys.stderr)
        return 1
    if not shutil.which(args.gitleaks) and not os.path.isfile(args.gitleaks):
        print(f"probe: FAIL — gitleaks binary not found: {args.gitleaks}",
              file=sys.stderr)
        return 1

    ver = subprocess.run([args.gitleaks, "--version"], capture_output=True,
                         text=True)
    print(f"probe: gitleaks {ver.stdout.strip() or ver.stderr.strip()}"
          " (CI pins GITLEAKS_VERSION in security.yaml — keep them in sync)")

    results: list[tuple[bool, str]] = []
    try:
        passed = check_shape(args.config)
        results += [(True, d) for d in passed]

        with tempfile.TemporaryDirectory(prefix="gitleaks-probe-") as scratch:
            for name, body in SAMPLES.items():
                with open(os.path.join(scratch, name), "w",
                          encoding="utf-8") as f:
                    f.write(body)
            report = os.path.join(tempfile.gettempdir(),
                                  "gitleaks-probe-report.json")

            base = run_gitleaks(args.gitleaks, scratch, report,
                                config=None, cwd=scratch)
            if base != set(SAMPLES):
                missing = sorted(set(SAMPLES) - base)
                raise ProbeFailure(
                    f"baseline positive control: expected all {len(SAMPLES)} "
                    f"samples detected by built-in defaults, missing: "
                    f"{missing} — sample forms no longer trigger rules, "
                    "probe assertions would be vacuous; update SAMPLES "
                    "against the pinned gitleaks version"
                )
            results.append((True, "baseline: all 5 samples detected by "
                                  "built-in defaults (positive control)"))

            repo = run_gitleaks(args.gitleaks, scratch, report,
                                config=args.config, cwd=scratch)
            leaked_exempt = sorted(EXPECT_EXEMPT & repo)
            missed_detect = sorted(EXPECT_DETECTED - repo)
            if leaked_exempt:
                raise ProbeFailure(
                    f"exemption ineffective: {leaked_exempt} still detected "
                    "with repo config — allowlist typo/regex drift (would "
                    "merge green and re-red the weekly schedule scan)"
                )
            if missed_detect:
                raise ProbeFailure(
                    f"detection weakened: {missed_detect} NOT detected with "
                    "repo config — unanchored/alternation regression or "
                    "useDefault removed (real credentials embedding the "
                    "public vector would silently escape all rules)"
                )
            results += [
                (True, "exempt: golden_vector.c (exact public vector, "
                       "golden-baseline comment form)"),
                (True, "exempt: golden_upper.c (hex case-equivalent form)"),
                (True, "detect: escape_prefix.txt (prefixed credential)"),
                (True, "detect: escape_embed.txt (ghp_-embedded credential)"),
                (True, "detect: control.txt (default rules unweakened)"),
            ]
    except ProbeFailure as e:
        results.append((False, str(e)))

    for ok, desc in results:
        print(f"[{'PASS' if ok else 'FAIL'}] {desc}")
    failed = [d for ok, d in results if not ok]
    print(f"probe: {len(results) - len(failed)}/{len(results)} PASS")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
