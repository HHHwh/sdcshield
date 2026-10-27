#!/usr/bin/env python3
"""Ensure every vector test declares its ARM64 spec requirement.

A test source that contains SVE/SME intrinsics (or the raw instructions the
suite uses) MUST carry `.arm64_requires = ARM64_REQUIRES_{SVE,SVE2,SME,SME2}`
in every DECLARE_TEST block. Without it the selection-time filter
(framework/sandstone_tests.cpp) cannot know the test's extension, and the
framework would keep showing it on hosts that can never run it.

Usage:
    scripts/check-arm64-requires.py            # report violations, exit 1 if any
    scripts/check-arm64-requires.py --fix      # insert the tag, then report

Design: docs/superpowers/specs/2026-10-27-arm64-spec-policy-and-vl-tier-cases-design.md §4.5
"""
from __future__ import annotations   # tuple[str, int] on Python 3.7/3.8 (openEuler 20.03)

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCAN_DIRS = (
    ROOT / "tests/cpu/sve",
    ROOT / "tests/cpu/arm64",
    ROOT / "tests/cpu/eigen_gemm",   # Eigen SVE packets are generated in the
    ROOT / "tests/cpu/eigen_svd",    # vendored library, not by our own body
    ROOT / "tests/cpu/sleef",        # sleef_sve: true-VLA SVE, lives outside
                                     # the sve/ tree (tests_sve_vla library)
)

# Raw vector instructions this suite uses, plus the intrinsic prefixes.
INTRINSIC = re.compile(
    r"\b(sv[a-z][a-z0-9_]*\(|svcnt[bdhw]\(|"
    r"smstart\b|smstop\b|fmopa\b|mova\b|rdsvl\b|rdvl\b)"
)

# A TU that includes arm_sve.h (or the framework VL layer) has a vector
# datapath even when its own body contains no intrinsic call — e.g. the Eigen
# payload TUs, whose SVE code is generated inside the vendored library.
SVE_HEADER = re.compile(r"#\s*include\s*[<\"](?:arm_sve\.h|device/cpu/arm64/arm64_vl\.h)[>\"]")
DECLARE = re.compile(r"^\s*DECLARE_(?:MANUAL_)?TEST\(", re.M)
END = re.compile(r"^\s*END_DECLARE_TEST\b", re.M)
HAS_TAG = re.compile(r"^\s*\.arm64_requires\s*=", re.M)


def required_bits(path: pathlib.Path) -> str:
    name = path.name
    # svmmla_f32/f64 are FEAT_F32MM/F64MM, not SVE2 — a host without SVE2
    # can still execute them (measured 2026-10-27). Only the genuinely
    # SVE2-instruction test gets the SVE2 tag.
    if "sve2_cross_precision" in name:
        return "ARM64_REQUIRES_SVE2"
    if name.startswith("sme_"):
        return "ARM64_REQUIRES_SME"
    return "ARM64_REQUIRES_SVE"


def blocks(src: str):
    """Yield (start, end) offsets of each DECLARE_TEST..END_DECLARE_TEST block."""
    for d in DECLARE.finditer(src):
        e = END.search(src, d.end())
        if e:
            yield d.start(), e.end()


def fix_file(path: pathlib.Path, src: str) -> tuple[str, int]:
    bits = required_bits(path)
    out, last, patched = [], 0, 0
    for start, end in blocks(src):
        block = src[start:end]
        if HAS_TAG.search(block):
            continue
        # Insert immediately before END_DECLARE_TEST; the previous field ends
        # with a comma in this code base (verified across tests/cpu/**). If it
        # does not, the source is already malformed -- leave it for the compiler.
        m = END.search(src, start)
        prev = src[last:m.start()].rstrip()
        if not prev.endswith(","):
            continue
        out.append(src[last:m.start()])
        out.append(f"    .arm64_requires = {bits},\n")
        last = m.start()
        patched += 1
    out.append(src[last:])
    return "".join(out), patched


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fix", action="store_true")
    args = ap.parse_args()

    missing, patched_files = [], 0
    for d in SCAN_DIRS:
        for path in sorted(d.rglob("*.cpp")):
            src = path.read_text(encoding="utf-8", errors="replace")
            if not (INTRINSIC.search(src) or SVE_HEADER.search(src)):
                continue                       # no vector datapath: tag not required
            if not list(blocks(src)):
                continue                       # no DECLARE_TEST (helper/support file)
            if args.fix:
                new, n = fix_file(path, src)
                if n:
                    path.write_text(new, encoding="utf-8")
                    patched_files += 1
                    src = new
            for start, end in blocks(src):
                if not HAS_TAG.search(src[start:end]):
                    missing.append(str(path.relative_to(ROOT)))

    if args.fix:
        print(f"patched {patched_files} file(s)")
    if missing:
        print("MISSING .arm64_requires:", file=sys.stderr)
        for m in sorted(set(missing)):
            print(f"  {m}", file=sys.stderr)
        return 1
    print("ok: every vector test declares arm64_requires")
    return 0


if __name__ == "__main__":
    sys.exit(main())
