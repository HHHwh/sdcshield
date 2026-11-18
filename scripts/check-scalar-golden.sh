#!/bin/sh
# check-scalar-golden.sh — mechanical gate for the issue #373 paradigm.
#
# The scalar-independent-golden paradigm (docs/coding/writing_tests.md,
# "Golden must be independent of the datapath under test") requires that the
# init-phase golden computation NEVER executes on the vector units under
# test. The switch-phase dispatch in the golden loop is a natural barrier
# against GCC auto-vectorization, but that is a property of the current
# compiler, not a guarantee — this script pins it against the built binary.
#
# For each requested symbol (a test's *_init function, or a run-side golden
# helper such as tlbstress_compute_golden) it disassembles the EXACT symbol
# extent and fails if any vector ALU instruction appears there. Pure data
# movement (ldr/str q, mov v) is allowed: it is not golden computation, and
# a load/store-path fault hits golden bytes and the run-side fresh recompute
# differently, so the miscompare still fires.
#
# Symbol names are C++-mangled in the symtab: the script resolves them by
# substring through nm. A symbol that cannot be resolved, or resolves to an
# empty disassembly, is a FAILURE, not a pass — an empty objdump output
# grep'd for vector instructions is a hollow PASS (the exact trap this
# gate exists to prevent; hit in practice on 2026-11-01).
#
# Usage:
#   scripts/check-scalar-golden.sh <binary> <symbol-substring>...
# Example:
#   scripts/check-scalar-golden.sh builddir/sdcshield \
#       neon_rot_ldr_at_top_init neon_rot_u8x16_init tlbstress_compute_golden
# Exit: 0 = all symbols scalar-clean, 1 = any violation or resolution failure.

set -u

if [ $# -lt 2 ]; then
    echo "usage: $0 <binary> <symbol-substring>..." >&2
    exit 1
fi

BIN=$1; shift
[ -x "$BIN" ] || { echo "error: '$BIN' is not an executable" >&2; exit 1; }

# Vector ALU mnemonics (A64 SIMD + SVE): op whose FIRST operand is a NEON
# vN.<shape>, an SVE zN.<shape>, or an SVE predicate pN.<shape> register
# (SVE compares write pN directly; fmla/fmad/etc. are predicated with p0/m
# but lead with zN). SVE additions (issue #373 second baton): fmla/fmad/
# fmls/fmsb/fnma/fns-family FMA, fneg/neg, not, sel, and the cmp* family —
# verified against real disassembly of the pre-conversion golden symbols
# (e.g. `fmad z1.s, p0/m, z3.s, z0.s`, `cmpeq p1.s, p0/z, z0.s, z1.s`).
# Data movement (ldr/str q, ld1w/st1w, mov/ins/dup v, mov z, p1/z, ptrue,
# whilelt) is deliberately excluded: it is not golden computation, and the
# scalar FP pipe (fmadd s0, ... — the fmaf-based independent golden for the
# arm0102_fma family) is likewise not a vector unit and stays allowed.
VALU_RE='[[:space:]](add|sub|eor|and|orr|bic|orn|mul|mla|mls|neg|abs|shl|sshll|ushll|sri|sli|ext|trn|zip|uzp|tbl|tbx|rev|fadd|fsub|fmul|fdiv|fmla|fmls|fmad|fmsb|fnma|fnms|fnmul|fneg|fabs|not|sel|cmpeq|cmpne|cmpgt|cmpge|cmplt|cmple)[[:space:]]+(v[0-9]+\.|z[0-9]+\.|p[0-9]+\.)'

fail=0
for sub in "$@"; do
    # resolve mangled symbol(s) + size via nm; local (t) and global (T) both fine.
    # A substring can match MANY distinct static symbols (per-TU same-named
    # inits) -- ALL of them are checked; a violation in any one is a failure
    # (all-match semantics backported from check-bswap-golden.sh, where the
    # movbe family's 15 same-named movbe_init instances require it).
    # nm -S fields: $1=addr $2=size $3=type $4=mangled-name
    nmatch=0
    for line in $(nm -S "$BIN" 2>/dev/null | awk -v s="$sub" '$4 ~ s && ($3 == "t" || $3 == "T") {print $1 ":" $2 ":" $4}'); do
        nmatch=$((nmatch + 1))
        addr=${line%%:*}
        rest=${line#*:}
        size=${rest%%:*}
        mangled=${rest#*:}
        end=$((0x$addr + 0x$size))
        out=$(objdump -d "$BIN" --start-address=0x$addr --stop-address=$end 2>/dev/null)
        insns=$(printf '%s\n' "$out" | grep -cE '^[[:space:]]+[0-9a-f]+:')
        hits=$(printf '%s\n' "$out" | grep -cE "$VALU_RE")
        if [ "$insns" -eq 0 ]; then
            echo "FAIL  $sub [$mangled @0x$addr]: empty disassembly (hollow-pass guard)"
            fail=1
        elif [ "$hits" -gt 0 ]; then
            echo "FAIL  $sub [$mangled @0x$addr]: $hits vector-ALU instruction(s) in golden path:"
            printf '%s\n' "$out" | grep -E "$VALU_RE" | sed 's/^/        /' | head -5
            fail=1
        else
            echo "PASS  $sub [$mangled @0x$addr]: $insns insns, 0 vector-ALU"
        fi
    done
    if [ "$nmatch" -eq 0 ]; then
        echo "FAIL  $sub: symbol not found in $BIN (wrong name, or inlined away?)"
        fail=1
    fi
done

exit $fail
