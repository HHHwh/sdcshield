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
2. **Self-verify before commit, 100% real** — (a) `./scripts/build.sh` clean,
   zero new warnings; (b) run the affected behavior and quote real output;
   (c) regression: `-e zstd19 -t 2000 -n 1` → `exit: pass`; (d) x86
   non-regression by inspection. No "should work" — fix and re-verify, never
   commit.
   **gem5-fi rule when the host chip cannot run it**: if the affected
   behavior is unrunnable on this machine because the chip lacks the
   capability (missing HWCAP/HWCAP2 bit such as SME, an unsupported VL/SVL
   tier, a newer ISA generation), then verifying only the SKIP path is NOT
   sufficient — the change MUST be validated in gem5-fi
   (`/home/sdc/wangxu/gem5-fi`, CHAOS/SE, same-source dual kernel under
   `workloads/sdcshield/`) at the affected VL/SVL tiers, quoting real run
   output (tier, checksum, `SDCSHIELD_FAILS=0`); if gem5 validation is
   genuinely impossible, state that verification boundary honestly instead.
   Never pass off "SKIP reason unchanged" as the verification of a
   functional change (PR #242 precedent: input-formula fix verified by
   `sme_za_tileview_dual.c` at SVL 128/256/512/1024-bit).
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
./scripts/build.sh      # ninja -j = nproc/2 by default (shared-server cap)
# Fresh clone: run the vendored build.sh first (openssl/openblas/sleef/isa-l/
# acl — see README). Missing install/ degrades gracefully, never hard.

./builddir/sdcshield --list-tests             # default PROD set AFTER spec filtering — host-dependent: ~478 on an SVE1 host, 297 on a no-SVE Kunpeng 920 (2026-10-27)
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

Guides: `docs/coding/writing_tests.md` (full); registration
`docs/research/adding-tests.md`; style `docs/coding/coding_style_guide.md`
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

## Known platform quirks (probe per host — this section binds no machine)

这些条目来自**多台**开发机的实测记录，单条可能只对其中一部分机器成立。
换机先按 rule 2 探测；与本机实测冲突时以本机实测为准，并顺手更新本节。

- `sysfs cluster_id` increases monotonically (138→654→1170…) and
  `physical_package_id` is large — firmware/ACPI-PPTT artifact (observed on a
  Kunpeng-family board); read as-is.
- cpufreq/thermal **因机而异**：有的板有 `cppc_cpufreq`（每核独立 policy，
  实测范围见 `cpuinfo_min_freq`/`cpuinfo_max_freq`），有的完全没有。先看
  `ls /sys/devices/system/cpu/cpu0/cpufreq` 与 `ls /sys/class/thermal/`：
  缺 `cpufreq` 时 `--vary-frequency`/`--vary-uncore-frequency` 优雅 SKIP；
  缺 CPU `thermal_zone*` 时 thermal throttle 是 no-op。都不是缺陷。
- `eigen_svd_double`/`eigen_sparse` full-core flakiness is **FIXED**
  (2026-10-24, feat/eigen-svd-multithread): both now use the per-thread
  same-source golden (each worker generates its own input and recomputes the
  golden on the same thread), so cross-thread Eigen SIMD-path divergence can
  no longer produce false mismatches. Verified on a 64-core host: full-run
  green (3x240s x 3 seeds, 10-min x 2 tiers, 300s rerolls) plus gem5-fi FS
  2-core `eigen_svd_double -n 2` pass. Run full-core; `-n 1` is no longer
  required. Adaptive dims are jointly sized (2026-10-27,
  feat/eigen-p0-p3-fix — also cured the full-core auto timeouts
  `eigen_svd_cdouble_sve` 321s / `eigen_svd_bidiag_sve` 343s):
  `sdc_joint_dim_slice` = min(n_mem, n_run, clamp) — n_mem from a
  per-NUMA-node three-source budget (phys×‰ / MemAvailable×0.9 / cgroup
  v1+v2) against measured per-family RSS coefficients (RSS_RATIO_TABLE),
  n_run from a probe power-law fit × the AR all-core-slowdown table
  (family×topology), refined by a VmHWM feedback loop (per-slice EMA,
  median aggregation, 0.7× table floor). Explicit `-O <test>.mdim/n`
  bypasses sizing with an expected-memory warning. The pre-fix "ULP-level
  sporadic failure under full-core multithreading" advice is
  obsolete.
- **Vector capability (SVE/SVE2/SME, VL/SVL tiers) is per-host — probe it,
  never assume it.** Measured lab hosts differ widely: some expose SVE with a
  pinnable VL and no SVE2/SME, some expose the full SVE2 set + SME/SME2 at a
  wider VL. Verification path follows rule 2: tiers the host supports are
  verified natively here; tiers it lacks go to gem5-fi
  (`--sve_vl/--sme_vl {1,2,4,8}` = 128/256/512/1024-bit).
- **ARM64 spec policy (2026-10-27)**: sdcshield probes the chip's spec once
  (`arm64_spec_get()`: extension bits + the MAXIMUM tier, measured by asking
  the kernel for the architectural ceiling and reading back what it granted —
  "current VL" is not "max VL", a sysctl can lower the default). The default
  test set is filtered at selection time to what the chip can execute
  (`struct test::arm64_requires` / `arm64_min_vl_bits`). The **control thread**
  is pinned to the chip maximum before `test_init` (its aux thread and the
  per-CPU workers inherit that VL, so init goldens and run lanes agree even
  when a sysctl lowered the default), and every SVE/SVE2 worker re-pins and
  verifies the readback (a refused pin is a SKIP, a readback mismatch is a
  FAIL). `-e` / `--test-list-file` bypass the filter (explicit requests still
  register and skip honestly). Override the assumed maximum with
  `SDCSHIELD_ARM64_SPEC_MAX_VL=<bits>` (only lowers; re-probed so an
  unimplemented tier such as 384 falls back to what the kernel grants).
  The Eigen SVE pair is tier-locked by the language (fixed-width packets) and
  is built once per tier with a runtime function-pointer dispatch; the
  complex SVD payload has no 1024-bit variant because Eigen's SVE complex
  `ptranspose` reads past `buf[N][NC]` when NC > N (upstream defect).
  `scripts/check-arm64-requires.py` keeps the tag coverage honest.
- **`PR_SVE_SET_VL` does not survive `execve()` on this kernel** (measured
  2026-10-27: a launcher that pins to 128-bit and `execvp`s the target shows
  `PR_SVE_GET_VL`=16 bytes in the launcher, but the exec'd program reads back
  32). So an external "pin-then-exec" wrapper CANNOT produce a lower-tier run;
  tier-specific runs must pin IN-PROCESS (the framework's worker pin, or the
  test's own `arm64_sve_pin_vl`). `SDCSHIELD_ARM64_SPEC_MAX_VL=<bits>` lowers
  the spec and therefore the framework pin without needing exec.
- **FMMLA (`svmmla_f32/f64`) semantics** (basis-probed 2026-10-27): each group
  of FOUR lanes is a 2x2 matrix product with the SECOND operand transposed —
  `out[g+2r+c] = Σ_k Zn[g+2r+k]·Zm[g+2c+k]` — and the two products are summed
  FUSED (one rounding). A scalar `std::fma` chain is therefore off by up to
  1 ULP on arbitrary operands; byte-exact goldens need exactly-representable
  (integer) operands. `svmmla_f64` is UNDEFINED below VL=256 (SIGILL).
  These were the first executions of the instructions on any available host,
  because the test's old gate required HWCAP2_SVE2 — which FMMLA does not need.
- **SME caveat**: "HWCAP2_SME=0 / `SMSTART` raises SIGILL" only proves SME is
  *not enabled by this kernel* — with `CONFIG_ARM64_SME=n` (check
  `/boot/config-$(uname -r)`; the SME sysctl
  `/proc/sys/abi/sme_default_vector_length` will also be absent) EL0 `SMSTART`
  traps regardless of silicon. A kernel-level "absent" is not a hardware
  verdict; say "software-invisible" and state the boundary.

## Documentation map — consult before guessing

## Documentation map — consult before guessing

- **Framework internals**: each test runs in a forked child whose `test_init`
  spawns per-CPU worker threads calling `test_run`; the parent collects via
  `forkfd`; tests are `struct test` instances in a special ELF `tests`
  section. **Read `docs/architecture.md` before touching `framework/`** (fork
  lifecycle, quality gates, feature detection, sysdeps, RAS, RNG).
- **Writing tests / code style**: `docs/coding/writing_tests.md`,
  `docs/research/adding-tests.md`, `docs/coding/coding_style_guide.md`.
- **CLI beyond this file**: `docs/research/usage.md` (all options).
- **Build/deploy/multi-OS**: README (quick start, 15-version build);
  `docs/build-deploy-pr-ci/` (design + usermanual + offline deps).
- **Agent full-automation design** (PR-loop lifecycle, bots, CI sentinel,
  auto-repair PRs, human merge gate): `docs/build-deploy-pr-ci/agent-full-automation.md`.
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
  `docs/coding/OPEN_SOURCE_PROVENANCE.md`.
- **Process artifacts**: `docs/superpowers/` (plans/, specs/, inventory/).
- **PR review-fix loop** (agent-driven: five bots (four external SaaS + the local
  headless claude adversarial reviewer) → subagent critical triage →
  fix → loop → human merge; never auto-merge): `docs/build-deploy-pr-ci/pr-loop.md`;
  engines `scripts/pr_loop.py` + `scripts/pr_local_review.py`（the 5th bot;
  unattended orchestration is task_loop's); `/pr-loop` and `/dev-loop` commands
  (`.claude/commands/`) and Trae rules (`.trae/rules/`) are content-twins.
- **task-loop 无人值守任务闭环**（统一编排引擎: CI 红/main 审查/会话/手建
  issue → agent → PR 闭环;ci_triage 的泛化后继,含迁移说明）:
  `docs/build-deploy-pr-ci/task-loop.md`;引擎 `scripts/task_loop.py`;
  会话命令 `/dev-loop`。
