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
  `EXIT_SUCCESS`). Examples: `tests/cpu/ist/ist.c`,
  `tests/common/smi_count/smi_count.cpp`.
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
- **Per-worker pin** (`framework/device/cpu/arm64/arm64_spec_prepare.h`,
  called from `sandstone_run.cpp` just before `test_run`): the worker is
  pinned to the chip's maximum tier and the value is read back. This is the
  only thread whose VL matters — `test_init` runs on a throwaway aux thread.

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
(x86-only MSR 0x34 — no ARM equivalent, returns nullopt). `mce_check` is a
special always-inserted test.

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
framework **overrides libc seeding functions** (`framework/random.cpp:781-883`
`extern "C"` block — `srand` aborts, etc.).

## Eigen 5.0 SVE double packets (vendored)

The vendored Eigen 5.0 SVE backend carries `double`/`complex<double>` packets
(`PacketXd` in `arch/SVE/PacketMath.h`, `PacketXcd` + `arch/SVE/Complex.h`),
making `eigen_svd_cdouble_sve` run vectorized SVD (~0.7 s at M_DIM 300 vs
>10 min scalar). **Runtime-VL rule**: size-specific SVE code requires the
process vector length to equal the compile-time `-msve-vector-bits`.
Whether a host has SVE at all is per-machine: probe it (CLAUDE.md rule 2)
instead of assuming — on a host without SVE the SVD SVE test cleanly skips,
and on SVE hosts with VL≠128 it needs a `prctl(PR_SVE_SET_VL, 16)` pin (see
`scripts/eigen-sve-double/` and `arm64_sve_pin_vl()`). All-VL Phase 2 P5:
the Eigen pair (`eigen_svd_cdouble_sve`, `eigen_gemm_double_max_arm`) self-pins
each worker thread inside `test_run` via `arm64_sve_pin_vl(16)` — no external
launcher needed anymore (Eigen's SVE packets are compile-time fixed-width; VLA
is un-compilable at the C++ language level per the twice-confirmed spike, so
the 128-bit lock stays; a failed pin skips honestly). `sleef_sve` and the
sve_rot probe family migrated to the true-VLA `tests_sve_vla` library (no
`-msve-vector-bits`) — all-VL 128..1024-bit.
