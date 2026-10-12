# CLAUDE.md

Guidance for Claude Code in this repository. SDCShield detects **silent data
corruption (SDC)** — computations that don't crash but produce wrong bits — by
stressing compute units (FMA, SIMD, compression, SVD, crypto) and
byte-comparing against golden values. Forked from Intel's OpenDCDiag; ported
to ARM64 (Kunpeng 920 / ARMv8.1+); x86-64 is the untouched reference
architecture. Deep-dives live under `docs/` — the Documentation map below
routes you; consult it before guessing.

## Non-negotiable rules

1. **One patch per unit** — each feature/bug/porting point is its own commit;
   never bundle. Work multi-item lists sequentially: verify → commit → push.
2. **Self-verify before commit, 100% real** — (a) `ninja -C builddir` clean,
   zero new warnings; (b) run the affected behavior and quote real output;
   (c) regression: `-e zstd19 -t 2000 -n 1` → `exit: pass`; (d) x86
   non-regression by inspection. No "should work" — fix and re-verify, never
   commit.
3. **DCO + auto-push** — `git commit -s` (last line `Signed-off-by: wangxu
   <wangxumarshall@qq.com>`, nothing after, no Co-Authored-By). Push each
   verified commit to the feature branch, never `main`; run
   `git branch --show-current` before committing (sessions switch branches).
4. **Plan-driven** — anything beyond a single obvious line: invoke
   `superpowers:writing-plans` first, plan under
   `docs/superpowers/plans/YYYY-MM-DD-<feature>.md`. No plan, no code.
5. **x86-64 untouched** — the port is additive (`#elif __aarch64__`, per-arch
   meson guards); widen `#ifdef __x86_64__` to include `__aarch64__` only
   where genuinely shared.
6. **Placeholder honesty** — unimplemented-on-ARM features must skip with
   reason `"to be implemented (placeholder): <what's missing>"` (`EXIT_SKIP`
   from `test_init`, never `EXIT_SUCCESS`). A no-op test reporting pass is a
   bug. `mce_check` is a real EDAC-backed test on ARM64 and must pass.
7. **Docs sync** — 大颗粒度修改必须同步更新 README.md 与 docs/ 下对应文档，
   确保 100% 准确。

## Always

- Reply in Chinese.
- Be concise.
- State result before cause when practical.
- 必须诚实、不能说谎、必须100%服从事实、所有工作和结果必须基于事实并且经过严格的逻辑推理或实证，永远尊重事实、永远真诚。
- 所有回答必须通俗易懂却不失深刻，必须言简意赅，深刻准确。

## Build & run

```console
# ARM64 needs vendored Eigen 5.0.0+ (system 3.3.x breaks on GCC 12+):
PKG_CONFIG_PATH=./third-party/eigen5 meson setup builddir --buildtype=release
ninja -C builddir
# Fresh clone: run the vendored build.sh first (openssl/openblas/sleef/isa-l/
# acl — see README). Missing install/ degrades gracefully, never hard.

./builddir/sdcshield --list-tests             # 440 tests at default PROD quality (gcc + vendored, 2026-10-12; excitation-enhancement suite merged)
./builddir/sdcshield -e zstd19 -t 5000        # one test, 5 s, all CPUs
./builddir/sdcshield -e zstd19 -t 5000 -n 1   # single-threaded (deterministic)
./builddir/sdcshield --quality=-1 -e <test>   # include SKIP-level tests
./builddir/sdcshield --dump-cpu-info          # CPU/features/topology dump
./builddir/sdcshield -s help                  # RNG engines: Constant/LCG/AES
```

Meson changes need `meson setup --reconfigure builddir` before `ninja`.
Verbose `-v`/`-vv`; logging `-Dlogging_format=yaml|tap|no_output`; full CLI
incl. undocumented options: `docs/research/usage.md`.

## Writing tests

Guides: `docs/misc/writing_tests.md` (full); registration
`docs/research/adding-tests.md`; style `docs/misc/coding_style_guide.md`
(C++20/C17, GCC-10, no warnings, naming, layout). Minimal pattern:

```c
#include "sandstone.h"
static int my_init(struct test *test) { /* golden data */ return EXIT_SUCCESS; }
static int my_run(struct test *test, int cpu) {
    TEST_LOOP(test, N) { memcmp_or_fail(actual, golden, len, "..."); }
    return EXIT_SUCCESS;
}
DECLARE_TEST(my_test, "description")
    .test_init = my_init, .test_run = my_run, .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
```

- `EXIT_SKIP` (-255) → skipped (log the reason first via `log_skip`);
  `report_fail()`/`memcmp_or_fail()` abort the thread; `TEST_LOOP` is the
  standard timed loop.
- Golden comparison is **byte-identical `memcmp`** — FP must be reproducible
  (the framework quiets SNaN uniformly in `sandstone_data.cpp` so FP16/BF16
  NaN bit patterns match cross-arch).

## Known platform quirks (Kunpeng 920, openEuler 24.03 SP3)

- `sysfs cluster_id` increases monotonically (138→654→1170…) and
  `physical_package_id` is large — firmware/ACPI-PPTT artifact, read as-is.
- No `cpufreq` → `--vary-frequency`/`--vary-uncore-frequency` skip gracefully;
  no CPU `thermal_zone*` zones → thermal throttle is a no-op here.
- `eigen_svd_double`/`eigen_sparse` fail sporadically under full-system
  multithreading (192 CPUs; ULP-level diffs vs strict memcmp) — `-n 1` always
  passes. Eigen/large-core-count limitation, not a port defect.
- SVE runtime-VL rule: host is VL=256 while `tests_sve` compiles at 128 → the
  eigen SVD SVE test needs the VL-pinning launcher in
  `scripts/eigen-sve-double/`; `sleef_sve` stays at 128 by design.

## Documentation map — consult before guessing

- **Framework internals**: each test runs in a forked child whose `test_init`
  spawns per-CPU worker threads calling `test_run`; the parent collects via
  `forkfd`; tests are `struct test` instances in a special ELF `tests`
  section. **Read `docs/architecture.md` before touching `framework/`** (fork
  lifecycle, quality gates, feature detection, sysdeps, RAS, RNG).
- **Writing tests / code style**: `docs/misc/writing_tests.md`,
  `docs/research/adding-tests.md`, `docs/misc/coding_style_guide.md`.
- **CLI beyond this file**: `docs/research/usage.md` (all options).
- **Build/deploy/multi-OS**: README (quick start, 15-version build);
  `docs/build-deploy/` (design + usermanual + offline deps).
- **Session recovery** (machine may die anytime; persist conclusions to
  disk): `docs/research/progress-log.md`; fact card in
  `docs/research/README.md`.
- **SDC case history**: `docs/cases/` — CPU179 has TWO competing root-cause
  lineages (OoO register-liveness vs LSU load-return); state which you cite.
  Also CPU122 (mercurial core), cn23154 (P0→P6 core localization), hpc
  (counter-example methodology). Excitation/reproduction methodology:
  `docs/sdc-excite-reproduce/`.
- **Microarch/hardware reference**: `docs/cpu/arm64-microarchtecture-internals/`
  (canonical start; Ch.9 SDC-sensitive units, Ch.10 CPU179 case),
  `docs/cpu/arm64/kungpeng/` (measured 920 + PMU events),
  `docs/cpu/arm64/armv8-isa/` (per-instruction), `neoverse-*-trm/`;
  gem5+CHAOS fault injection: `docs/gem5-doc/`.
- **Literature rationale** (why these workloads/libs — 31-paper synthesis):
  `docs/paper/SDC_RESEARCH_for_SDCShield-负载设计.md`; fault-injection experiment
  plan: `docs/paper/SDC_FAULT_INJECTION_EXPERIMENT_PLAN_CN.md`.
- **Academic survey** (53-paper full-read taxonomy report, 2026-09):
  `docs/paper/SDC_ACADEMIC_SURVEY_CN.md` — SDC core-characteristics /
  root-cause / fault-mode / detection / handling taxonomies,
  academia-vs-industry comparison, collab ecosystem, full-stack ×
  full-lifecycle implications; per-paper digests:
  `docs/paper/ref-paper-fullread-digest/` (INDEX.md = NN↔PDF map; note
  the 26/27 filename swap).
- **Theory**: `docs/hypothesis/ARM64-SDC-uArch.md`; **output schema**:
  `docs/sdcshield-cpu.schema.json`; **provenance/compliance**:
  `docs/misc/OPEN_SOURCE_PROVENANCE.md`.
- **Process artifacts**: `docs/superpowers/` (plans/, specs/, inventory/).
