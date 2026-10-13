# Fix: two PR #232 SVE arm64 tests fail their own golden on SVE hardware

## Context

After the `log_yaml` fix (`ffcc7a35`) unblocked the `run selftests` bats
step, the CI job **Build binary for CPU** advanced to `run tests quickly`
and exposed two **newly added** PR #232 tests failing (job
`114901375828`, run `38275921824`):

- `sve512_fcmla_arm` → `Failed at .../sve512_fcmla_arm.cpp:243: golden
  worked-example self-check failed (rot 90) ??? test bug, not SDC`
- `sve512_pred_ops_arm` → `Failed at .../sve512_pred_ops_arm.cpp:296: SVE
  predicate-network SDC detected (whilelt/svnot/FADDA/FFR/FMAXV)`

Both report on every thread with a deterministic seed → not intermittent
hardware SDC. Local Kunpeng 920 has **no SVE**, so both tests `skip`
(`test compiled with sve`) and cannot reproduce locally; the defects are
proven by code semantics + the ARM FCMLA / ACLE `svmla` definitions.

Both are **golden bugs in the tests**, not SDC and not port defects. They
were masked earlier because CI never reached `run tests quickly`.

## Root cause 1 — `sve512_fcmla_arm.cpp` (rotation dispatch key mismatch)

`fcmla_golden_pair(..., int rot)` dispatches with `switch (rot)` on
**angles** (`case 0/90/180/270`), but every caller passes the loop
**index** `rot = 0..3`:

- self-check (line 238) and chain-golden in `test_init` (line 186) both do
  `for (int rot = 0; rot < 4; ++rot) ... fcmla_golden_pair(..., rot)`.

So index `0` hits `case 0` (correct), but indices `1/2/3` all fall into
`default` (the 270° formula → `(8,-6)`). Expected `expect[1] = (-8,6)`
(90°) → self-check fails and reports `rot * 90 = 90`, exactly the CI
message. The hardware dispatch (`fcmla_hw_rot0/90/180/270`, lines 258–261)
is index-keyed, so the chain golden is also computed with the wrong
rotation.

The per-rotation **formulas are correct** (verified against ARM DDI 0616
FCMLA pseudocode: rot 90 → `re += -b_im*c_im; im += b_im*c_re`). Only
the dispatch key is wrong.

## Root cause 2 — `sve512_pred_ops_arm.cpp` (svmla operand order + `_x`)

ACLE: `svmla_f64_x(pg, op1, op2, op3)` = `op1 + op2 * op3` (op1 is the
addend/accumulator). Repo precedent: `svmla_f32_x(pg, vc, va, vb)` = `c +
a*b` ([fma_tail_sve.cpp:64]); `svcmla_f64_m(pg, acc, b, c, rot)`.

Every call here passes `vsrc` as **op1**: `svmla_f64_x(pg, vsrc, vsrc,
vdst)` = `vsrc + vsrc*vdst`, not the documented `fma(src, src, dst)`:

- §1 exact variant: `svmla_f64_x(pg, vsrc, vsrc, vz)` with `vz=0` gives
  `vsrc + vsrc*0 = src`; golden is `fma(src,src,0)=src²` → mismatch.
- §1 sentinel check: `_x` is "inactive lanes don't care"; the SVE FMLA
  merges with the destination op1 (`vsrc`), so masked-out lanes become
  `src`, not `SENTINEL` → "sentinel clobbered".
- §2 and the timed re-loop share the same operand order bug.

So checking inactive-lane preservation with the `_x` form is unsound by
construction; the fix uses the merging `_m` form with the accumulator as
op1.

## Fix

Two independent, minimal units (one patch each, per repo rule 1).

### Unit A — `tests/cpu/arm64/sve512_fcmla_arm.cpp`

Key the golden `switch` on the rotation **index** `0..3` to match every
caller and the hardware dispatch (`case 90→1`, `case 180→2`, default→3),
and document that `rot` is the index (0/90/180/270).

### Unit B — `tests/cpu/arm64/sve512_pred_ops_arm.cpp`

Rewrite §1/§2 and the re-loop to the correct form:

- accumulate into the destination: `svmla_f64_m(pg, dst, src, src)` =
  `dst + src*src` (op1 = accumulator);
- use the **merging** `_m` form so masked-out lanes are architecturally
  preserved and the sentinel check is sound;
- active lanes carry a finite, exactly-representable addend (`BASE=1.5`)
  so the computed lanes have a deterministic byte-exact golden; inactive
  lanes keep the NaN `SENTINEL`;
- golden = `fma(src, src, BASE)` for active lanes, `SENTINEL` bit-exact
  for inactive ones.

§3 (FADDA) verified correct by inspection and left unchanged. §4 was
initially believed correct too — that proved wrong on the next CI run, see
Root cause 3.

## Root cause 3 — `sve512_pred_ops_arm.cpp` §4 (FFR readback without SETFFR)

After unit A (commit `cf63210a`) and unit B (commit `f335b564`) landed,
CI run `38277871409` still failed `run tests quickly`, but the failure
moved from line 296 to line **264** (the aggregate report), i.e. the only
remaining sub-check failing was §4. The warning is suppressed in the CI
console by the default verbosity, but the run-log artifact
(`test-linux-GCC-arm64-logs`, `quick.yaml`) preserves it:

> `W> sve512_pred_ops: FFR not all-true after mapped LDFF1 (count=0, expected 2)`

The test read FFR with `svrdffr()` **without priming it first**. Per the
ARM SVE first-fault model (confirmed against the ARM first-fault-load
flow: *"SETFFR — reset the FFR to all-ones; LDFF1 … if any subsequent
element faults, the hardware silently suppresses that fault and clears
FFR from that lane onward"*, and the FFR reference: *"可以使用 SETFFR
指令初始化该值"*), an `LDFF1` only **clears** the FFR bit of a faulting
element — it never **sets** a bit. So without `SETFFR` the readback is the
previous, undefined FFR state; on the CI runner that state was 0, giving
`count=0` instead of `2` (VL=128 → 2 doublewords).

Empirically verified on the local GCC 12 aarch64 toolchain: for
`svldff1_f64(all, p)` GCC emits a real `ldff1d` (no relaxation to `ld1`),
and emits `setffr` **only** when `svsetffr()` is present in the source.

### Unit C — `tests/cpu/arm64/sve512_pred_ops_arm.cpp`

Call `svsetffr()` immediately before the §4 `svldff1_f64`, so the FFR bit
for each non-faulting lane is the architecturally defined 1, and the
`svrdffr()`/`svcntp_b64` comparison against the all-true operating
predicate is meaningful. Update the §4 doc comment accordingly.


## Verification (100% real)

- `ninja -C builddir` clean, zero new warnings.
- Both tests on non-SVE Kunpeng 920 → `result: skip`, `skip-reason: 'test
  compiled with sve'`, `exit: pass` (no crash / no assert).
- Regression `./builddir/sdcshield -e zstd19 -t 2000 -n 1` → `exit: pass`.
- Correctness of the rewritten golden proven by hand: for `src ∈
  {-1.5,-0.5,0.5,1.5}`, `BASE=1.5`, active-lane golden `src²+1.5 ∈
  {1.75,3.75}` is exact; FADDA sum over VL (multiple of 4) is exactly 0.
- x86-64 untouched (files are `#ifdef __aarch64__` only).
- Unit C: `objdump -d` of the rebuilt
  `builddir/tests/cpu/arm64/libtests_arm64_sve.a.p/sve512_pred_ops_arm.cpp.o`
  shows `setffr` immediately followed by `ldff1d {z0.d}, p0/z, …` in the §4
  path (grep `setffr` → 2 hits, `ldff1` → 2 hits).
- Final oracle: CI `Build binary for CPU` → `run tests quickly` green.
