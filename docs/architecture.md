# SDCShield Architecture Deep-Dive

> Companion to `CLAUDE.md` — read this **before touching `framework/`**.
> All file/line references verified against main @ b85da2ed on 2026-09-24.

## Test lifecycle (fork model)

Each test runs in a **forked child process**:

```
parent: run_one_test_inner (framework/sandstone_run.cpp)
  → child_run (framework/sandstone_run.cpp:1232)
      → test_init                     (child main thread)
      → per-CPU worker threads        (test_run via test_run_wrapper_function)
      → test_cleanup
```

The parent collects results via `forkfd`. A test that crashes crashes the
*child*, which reports back through `CrashContext`
(`framework/sysdeps/unix/child_debug.cpp`).

Fork modes (`SandstoneApplication::ForkMode`): `no_fork`, `fork_each_test`
(default for non-debug builds), `exec_each_test` (default for debug builds).

`test_preinit` runs **once in the parent** and is nulled after first run, so it
does not re-run across iterations — use `test_init` for per-iteration skipping.

## Test declaration & the "tests" section

Tests are `struct test` instances placed in a special ELF section
(`section("tests")`) via the `DECLARE_TEST` macro; the framework iterates
`__start_tests`..`__stop_tests` to find them. `DECLARE_MANUAL_TEST` omits the
section attribute — used for special/injected tests like `mce_check`, which is
always appended last regardless of selection.

`struct test` fields: `test_preinit` / `test_init` / `test_run` /
`test_cleanup` (function pointers; NULL is allowed and skipped — **except
`test_run`, which must be non-NULL**), `minimum_cpu` (feature gate),
`quality_level`, `groups`, `flags`.

## Test knobs (test options)

Test options (`-O <test>.<key>=<value>`, `sdc_stress.conf`, …) are read via
`get_testspecific_knob_value_{int,uint,double,string}()` and are **init-only**:
the getters may only run from `test_init` (control thread, `thread_num == -1`).
`logging_mark_knob_used()` enforces this with a `thread_num != -1` check that
aborts — since issue #417 the check is **not** NDEBUG-gated (release builds
included) and sits ahead of the `no_output` short-circuit, so a quiet output
format cannot mask the bug; `selftest_knob_from_run` locks the negative path
in `bats/sanity-check/20-selftests.bats` and
`scripts/gha/verify-params.py`. A payload that needs a knob inside `test_run`
must query it in `test_init` and cache it (the gather/openssl prewarm pattern).
The sizing helper `sdc_knob_int_clamped()` is equally fail-loud on a worker
cache miss — its former silent `def` fallback bypassed the getter check
entirely and ignored `-O` in every build type (issue #417 gap 3).

## Quality levels

`TEST_QUALITY_SKIP`(-1) < `BETA`(0) < `PROD`(2, default `--quality=2`). A test
runs when `quality_level >= requested_quality`. The SKIP-skip guard
(`framework/sandstone.cpp:546` and `:559`) drops SKIP-level tests unless
`requested_quality < 0` — so a SKIP-level test with a NULL `test_run` will
**crash** under `--quality=-1` unless it self-skips in init (see `mce_check`).

## Cross-architecture portability model

The x86-64 implementation is the reference; ARM64 is a parallel port. Gating is
`#ifdef __x86_64__` with ARM64 `#elif`/stubs. Rules:

- Placeholder tests for unimplemented-on-ARM features **must skip** with reason
  `"to be implemented (placeholder)"` (`EXIT_SKIP` from `test_init`, never
  `EXIT_SUCCESS`). Example: `tests/cpu/ist/ist.c`.
  (`tests/common/smi_count/smi_count.cpp` was one until the ARM64
  CNTVCT_EL0 gap probe landed — 2026-10-31, issue #7.)
- Real features get real implementations; truly-absent hardware (ARM has no
  CPU microcode/PPIN/MSR) stays a documented stub, not faked.
  `tests/common/mce_check/mce_check.cpp` is a *real* EDAC-backed test on ARM64.

## CPU feature detection (generated pipeline)

```
framework/device/cpu/simd.conf      (x86 keys: CPUID leaves)  ─┐
framework/device/cpu/simd-arm.conf  (ARM keys: HWCAP bits)   ─┴→ {x86simd,armsimd}_generate.pl
                                                              (Perl, at meson-configure time)
                                                              → builddir/.../cpu_features.h
                                                              (GENERATED — do not edit)
```

The generated header emits `device_features_t` (128-bit), `cpu_feature_*`
constants, `cpu_*` arch macros, `features_string`/`features_indices` tables,
and `x86_locators`/`x86_architectures` tables. x86 keys on CPUID leaves; ARM
keys on `getauxval(AT_HWCAP/HWCAP2)`. **Both generators emit identical symbol
names**, so `cpu_device.cpp` / `cpuid_internal.h` work unchanged across
arches. `detect_cpu()` (`cpuid_internal.h`) fills the global `device_features`;
`device_has_feature(cpu_feature_X)` / `minimum_cpu` gating consume it;
`dump_device_info()` (`cpu_device.cpp`) and `device_features_to_string()`
consume the generated tables.

## SVE/SME vector-length layer (arm64_vl.h)

Two consumers sit on top of this layer (design
`docs/superpowers/specs/2026-10-27-arm64-spec-policy-and-vl-tier-cases-design.md`):

- **Selection** (`framework/sandstone_tests.cpp`, `SandstoneTestSet` ctor):
  tests declaring `arm64_requires` / `arm64_min_vl_bits` are dropped from the
  default set when the probed chip spec cannot run them, so `--list-tests`
  shows what this machine can actually execute. Explicit `-e` /
  `--test-list-file` selections bypass the filter and skip at run time.
- **Control-thread pin before `test_init`** and **per-worker verification**
  (`framework/device/cpu/arm64/arm64_spec_prepare.h`, called from
  `sandstone_run.cpp`): both `test_init`'s aux thread and the per-CPU workers
  are created by the control thread and inherit its VL, so the control thread
  is raised to the chip maximum first — init's goldens/buffers and the run-side
  lane count must agree even when the kernel default VL was lowered by a
  sysctl. Each worker then re-pins and reads the value back: a refused pin is
  a SKIP, a readback mismatch is a FAIL.

`framework/device/cpu/arm64/arm64_vl.h` is the single entry point for
per-thread SVE vector length (VL) and SME streaming vector length (SVL):

- capability probes (`arm64_cpu_has_sve/sve2/sme`) — honest runtime
  `getauxval(AT_HWCAP/HWCAP2)` checks that, unlike `device_has_feature()`,
  are not short-circuited by compile-time `__ARM_FEATURE_*` macros in
  +sve/+sme-compiled TUs;
- current-thread VL/SVL queries (`arm64_sve_vl_bytes` /
  `arm64_sme_svl_bytes`) — rdvl/rdsvl via `.inst`, so any aarch64 TU can
  use them without a +sve march;
- self-pinning (`arm64_sve_pin_vl` / `arm64_sme_pin_vl`, prctl) for
  size-specific TUs. Pin on the thread that needs the VL — inside
  `test_run` workers. Pinning from `test_init` does **not** reach the
  workers: ordinary tests run `test_init` on a throwaway aux thread
  (`sandstone_run.cpp`, `init_in_aux_thread=true`), and the per-CPU
  workers are created by the control thread, inheriting *its* (unpinned)
  default VL. The kernel's rule that a newly created thread inherits its
  *creator's* VL still holds — the framework's thread structure is simply
  not "test_init creates the workers" (only `test_in_parent` /
  `test_init_in_parent` flagged tests differ);
- test-flow helpers (`arm64_require_sve_vl` / `arm64_require_sme_svl`)
  folding capability check + range check + log_skip into one call.

VL/SVL are deliberately **not** part of `device_features`: they are
per-thread runtime quantities, while feature bits are process-wide.
Verification: the `.inst` encodings (rdvl/rdsvl) are derived via
`as`+`objdump` (recorded in the header comments) and included in every
aarch64 build — but the instructions only *execute* where the capability
probes pass, so their runtime verification runs wherever the host has
SVE/SME (probe the host — CLAUDE.md rule 2; the probes and SKIP paths run
everywhere).
Verification boundary, stated honestly: an earlier "validated at all four
tiers in gem5-fi" claim could not be verified on audit (the referenced
probe/branch no longer exists locally and no gem5 build is present on
this machine), and gem5 SE maps prctl to ignoreFunc, so the pin success
path (SET_VL taking effect) is kernel-documented but not yet empirically
executed anywhere — `selftest_arm64_vl_require_sve` pins an alternate VL
on the worker thread and reads it back via rdvl, so the first SVE-host
selftest run *attempts* the round-trip proof; where prctl is blocked
(-EPERM), ignored (-ENOSYS), or no alternate VL exists, the test degrades
to a logged warning rather than claiming the proof.

## Sysdeps layering

```
framework/sysdeps/<os>/                 linux | darwin | freebsd | windows
framework/sysdeps/unix/                 shared unix (incl. child_debug.cpp)
framework/device/cpu/sysdeps/<os>/      CPU-specific
framework/sysdeps/generic/              non-x86 fallbacks (e.g. kvm.c skip stub)
```

Arch gating is **inside files** (`#ifdef __x86_64__` / `__aarch64__`) and
**inside meson** (`host_machine.cpu_family()`): e.g. `msr.c` is x86-only,
`interrupt_monitor.cpp` is x86+aarch64.

## InterruptMonitor / RAS

`framework/interrupt_monitor.hpp` + `sysdeps/linux/interrupt_monitor.cpp`.
`InterruptMonitorWorks` is `true` on linux x86-64 + aarch64. x86 counts MCE/TRM
lines from `/proc/interrupts`; aarch64 counts EDAC `ce_count` + `ue_count`
(controller-wide, placed at index 0). `count_smi_events()` uses `read_msr`
(x86-only MSR 0x34 — no ARM equivalent, returns nullopt). The ARM64
`smi_count` test therefore bypasses InterruptMonitor entirely — it is a
CNTVCT_EL0 read-gap probe implemented inside the test itself, because no
sampleable firmware-preemption counter exists on bare-metal ARM64 (see
`tests/common/smi_count/smi_count.cpp` for the measured basis and the
semantic boundary: the probe reports a per-CPU timing-noise map and cannot
attribute gaps to firmware vs. hypervisor vs. kernel scheduling).
`mce_check` is a special always-inserted test.

## Tests directory layout

- `tests/common/` — arch-agnostic (mce_check, smi_count)
- `tests/cpu/` — compute tests (eigen_*, zlib, zstd, ifs, ist, openssl,
  openblas_gemm, sleef, pocketfft, isa-l, mesh_*)
- `tests/{gpu,idxd}/` — only built for `-Ddevice_type=gpu/idxd`
- `tests/examples/` — never built; reference only

`tests/cpu/meson.build` uses meson **sourcesets**: `tests_set_base` (all
arches), `tests_set_hsw`/`tests_set_skx` (x86 AVX2/AVX512, compiled with
`-DEigen=EigenAVX2`/`EigenAVX512` namespace renames so multiple SIMD backends
link without symbol clash), `tests_set_sve` (aarch64 SVE, `-DEigen=EigenSVE`).

## RNG

`framework/random.cpp`. Engines: Constant, LCG, AES (auto-picked when
`haveAes()`; list via `-s help`). The AES engine uses `#pragma GCC target`
per-arch: x86 `_mm_aesenc_si128`, aarch64 `vaesmcq_u8(vaeseq_u8(...))` via
`+crypto`. RNG state is per-thread (`thread_rng` union, 64-byte aligned). The
framework **overrides libc seeding functions** (`framework/random.cpp:850-952`
`extern "C"` block — `srand` aborts, etc.).

Threads spawned by test code with a raw `std::thread` never run the
framework's init paths; their `thread_num` TLS is zero-initialized to 0,
which used to alias worker 0's slot (`rngs[1]`) — a data race when several
such threads call the RNG (issue #348, proven by ThreadSanitizer on
`cache_stress_aggressor`). Since 2026-10-31 such threads get a private,
lazily-seeded `thread_local` engine instead (`tls_engine` /
`raw_thread_engine()`): the mixin is a fixed salt ^ a process-wide atomic
counter and the seed material is the global engine's current state, so raw
streams still depend on `-s`. Framework threads bind the exact slots they
always used (`random_init_global` / `random_init_thread` / the
`thread_num == -1` first-use fallback), so their streams stay bit-exact
under `-s`. Caveat: which raw thread gets which counter value depends on
scheduling, so per-raw-thread replay across runs is not guaranteed; and a
detached raw thread whose *first* RNG call happens after the test finished
could race the scheduler's `random_advance_seed()` on the global slot —
join raw threads before `test_run`/`test_cleanup` returns.

## Eigen 5.0 SVE double packets (vendored)

The vendored Eigen 5.0 SVE backend carries `double`/`complex<double>` packets
(`PacketXd` in `arch/SVE/PacketMath.h`, `PacketXcd` + `arch/SVE/Complex.h`),
making `eigen_svd_cdouble_sve` run vectorized SVD (~0.7 s at M_DIM 300 vs
>10 min scalar). **Runtime-VL rule**: size-specific SVE code requires the
process vector length to equal the compile-time `-msve-vector-bits`.
Whether a host has SVE at all is per-machine: probe it (CLAUDE.md rule 2)
instead of assuming — on a host without SVE the SVD SVE test cleanly skips.
Tier dispatch (2026-10-27, replaces the earlier 128-bit lock): Eigen packets
are compile-time fixed-width (VLA is un-compilable at the C++ language level,
twice-confirmed spike), so the Eigen SVE pair is built **once per tier** under
its own Eigen namespace and a vector-length-agnostic dispatcher picks the
variant matching the framework-pinned worker VL (complex SVD runs all four
tiers since the vendored-Eigen ptranspose scatter rewrite, issue #410
2026-11-01: the memory-bridge form read past `buf[N][NC]` when NC > N at
1024-bit; the scatter form is in bounds for any N/NC and bit-identical in
square shapes, gem5-verified at 128/256/512/1024). Each per-tier run wrapper
re-pins the worker to the
framework-chosen
tier and verifies the readback (a refused pin skips, a mismatch fails).
`sleef_sve` and the sve_rot probe family live in the true-VLA `tests_sve_vla`
library (no `-msve-vector-bits`) — all-VL 128..1024-bit.

## Eigen sizing: NUMA-aware joint solve (arm64)

All 21 eigen tests size their matrix dimension through `sdc_joint_dim_slice()`
(`framework/sdc_stress_config.h`, design
`docs/superpowers/specs/2026-10-27-eigen-p0-p3-fix-design.md`):
`n = min(n_mem, n_run, clamp)`. Data flow, in order:

```
sdc_numa_probe()   nodes-with-memory + per-node MemFree + cpulist
      ↓
sdc_budget_bytes() four-source min: phys×‰ / MemAvailable×0.9 /
                   cgroup v1+v2 headroom / CommitLimit−Committed_AS×0.9
                   (commit source only when vm.overcommit_memory=2,
                   no-op otherwise — issue #310); per-node share by MemFree
      ↓            (zero-memory nodes fold into the fallback node —
                    on a node0-only host this degrades exactly to
                    the global model)
sdc_joint_dim_slice()  n_mem: node budget vs RSS_RATIO_TABLE
                       (measured per-family c, KB/n²/thread)
                       n_run: probe power-law fit (fixed overhead
                       subtracted, 100 ms probe threshold) × AR
                       all-core-slowdown table (family×topology)
      ↓
VmHWM feedback     per-slice MemFeedback slots in shmem (written at
                   run_one_test_inner tail, no_fork excluded):
                   eff_ratio=(VmHWM − fork baseline)/(n²·elem·threads)
                   (baseline sampled before any test allocation, #385),
                   EMA α=0.5, median aggregation, 0.7× table floor and
                   4× table-anchor upward cap — c corrections feed the
                   *next* fracture/test
```

`eigen_svd_cdouble_sve` / `eigen_gemm_double_max_arm` moved sizing from
preinit into the child `test_init` (also fixing an exec-mode divergence
where the exec'd image never saw the preinit-computed dim). The other 12
Eigen SVE cases (2026-11-03 unification — per-tier body + dispatcher
framework, `tests/cpu/eigen_svd/sandstone_eigen_sve_dispatch.h`)
likewise size in the child `test_init` via the dispatcher
(`eigen_svd_bidiag_sve` did too — retired 2026-11-02, historical note).
Explicit
`-O <test>.mdim` bypasses sizing; an `[内存越界]`/`[欠压力]`
expected-memory warning fires only for callers whose knob query routes
through the joint layer's early-exit (the ten SVD template-family users
via `calibrated_auto_dim`). The GEMM/sparse/SVE callers still
short-circuit the knob in their own init (`if (v > 0) return v;` before
the arch guard) — their explicit paths carry no warning yet (T11b
follow-up; 10/47 eigen cases covered — 32 at the 2026-11-03
unification docs count + `eigen_gemm_double_ab_split` (migration
#472 content, in the same-day unification merge; no -O mdim knob,
dimension from the raw `GEMM_K_DIM` env) + 14 `_sve2` companions
(2026-11-13, inits short-circuiting the knob like their `_sve`
templates) — the covered ten unchanged; the fourth bidiag caller was
retired with its case).

x86-64 keeps the old sizing behavior via the **callers'** `#else` chains
(e.g. `calibrated_auto_dim`'s legacy two-step memory+2 s-calibration
chain, `gemm_mdim`'s `sdc_auto_square_dim` path) — rule 5. The `#else`
branch inside `sdc_joint_dim_slice` itself is the global-arch fallback
(#482): four-source global budget + power-law time cap via
`sdc_calibrated_dim` (AR=1, no per-node/AR/VmHWM feedback — x86 has no
`sdc_numa.h`). No live x86 caller reaches it today (all call sites are
`#if defined(__aarch64__) && defined(__linux__)`-gated); it exists so a
future cross-arch caller that drops its guard gets the same memory×time
dual cap as ARM instead of silently losing the single-solve time bound.
Migrating an x86 caller still changes its sizing numbers (RSS table /
four-source budget / run_budget_ms semantics), which rule 5 keeps to the
callers' own `#else` chains.

The one rule-5 exception formerly disclosed here (the `sdc_calibrated_dim`
probe threshold raised 1 ms → 100 ms as a global shared constant) is
retired (issue #351): the threshold is now a parameter, `probe_floor_ms`.
Its default 1.0 restores the historical noise-floor semantics for every
existing 7-argument caller — the x86-64 `#else` legacy chains
(`eigen_sparse`, the SVD template family) and aarch64-non-Linux — so their
calibration is semantically equivalent to the pre-#302 behavior: identical
1 ms probe stopping threshold and code path, with calibrated operating
points and init probe cost reproducing within timing jitter (calibration
is wall-clock-dependent and thus not byte-reproducible; the PR-measured
`mdim` 473 vs the 476 pre-#302 baseline is such jitter). The 100 ms M3
floor (fixed-overhead share < 10%, needed
by the difference-based `sdc_fit` `a_ms` extraction; bound to the named
constant `sdc_m3_probe_floor_ms`) is carried explicitly
by the joint solver on its ARM64-only path; no non-migrated consumer sees
it. Two runtime `log_warning`s guard the extraction on that path, both
emitted only for fit-consuming callers (the legacy 7-argument chains pass
no `fit` and never warn): one when the probe tier's measured duration
falls short of the floor, and one when the two-point difference itself
degenerates — tier collision under `hi` truncation, or flat/inverted
timings — and the b=0 fallback `a = t1/p1^k` leaves the full fixed
overhead inside `a_ms` (a_ms inflated → n_run undersized → under-excited
SDC coverage).
