# CLAUDE.md

Guidance for Claude Code in this repository. SDCShield detects **silent data
corruption (SDC)** — computations that don't crash but produce wrong bits — by
stressing compute units (FMA, SIMD, compression, SVD, crypto) and
byte-comparing against golden values. Forked from Intel's OpenDCDiag; ported
to ARM64 (ARMv8.1+; lab hosts differ in part number, core count and vector
capability — probe each machine, see rule 2); x86-64 is the untouched
reference architecture. Deep-dives live under `docs/` — the Documentation map
below routes you; consult it before guessing.

## Non-negotiable rules

1. **One patch per unit** — each feature/bug/porting point is its own commit;
   never bundle. Work multi-item lists sequentially: verify → commit → push.
2. **Self-verify before commit, 100% real** — (a) `./scripts/build.sh` clean,
   zero new warnings; (b) run the affected behavior and quote real output;
   (c) regression: `-e zstd19 -t 2000 -n 1` → `exit: pass`; (d) x86
   non-regression by inspection. No "should work" — fix and re-verify, never
   commit.
   **Probe the host before assuming anything** — the lab runs several ARM64
   hosts that differ in part number, core count and vector capability
   (SVE/SVE2/SME, VL/SVL tiers). Never quote a part number, a core count or
   "this machine has/hasn't X" from memory or from another host; measure
   THIS machine first (MIDR = implementer/part/rev, HWCAP-derived features
   incl. `sve`/`sve2`/`sme`, online CPUs, and the *real* per-thread vector
   length — HWCAP presence is not proof an instruction executes, so probe
   the instruction itself where it matters; an unsupported one raises
   SIGILL):
   `cat /sys/devices/system/cpu/cpu0/regs/identification/midr_el1`,
   `grep -m1 Features /proc/cpuinfo`, `nproc` + `lscpu | grep On-line`,
   `prctl(PR_SVE_GET_VL)` / `rdvl` for VL, `mrs smidr_el1` / `SMSTART` for
   SME, and `./builddir/sdcshield --dump-cpu-info` (CPU + feature dump; a
   current build also prints the SVE vector length).
   - **Capability present on this host → verify natively here** with a real
     run and real output. SKIP-only is never acceptable evidence for a
     functional change.
   - **Capability absent on this host → gem5-fi** (`/home/sdc/wangxu/code-review/gem5-fi`,
     CHAOS/SE, same-source dual kernel under `workloads/sdcshield/`) at the
     affected VL/SVL tiers, quoting real run output (tier, checksum,
     `SDCSHIELD_FAILS=0`); if gem5 validation is genuinely impossible, state
     that verification boundary honestly instead.
   - Never pass off "SKIP reason unchanged" as the verification of a
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
- `eigen_svd_double`/`eigen_sparse` fail sporadically under full-system
  multithreading (ULP-level diffs vs strict memcmp; the larger the core
  count the more likely — machine-independent) — `-n 1` always passes.
  Eigen/large-core-count limitation, not a port defect.
- **Vector capability (SVE/SVE2/SME, VL/SVL tiers) is per-host — probe it,
  never assume it.** Measured lab hosts differ widely: some expose SVE with a
  pinnable VL and no SVE2/SME, some expose the full SVE2 set + SME/SME2 at a
  wider VL. Verification path follows rule 2: tiers the host supports are
  verified natively here; tiers it lacks go to gem5-fi
  (`--sve_vl/--sme_vl {1,2,4,8}` = 128/256/512/1024-bit). All-VL Phase 2 P5:
  `tests_sve` (eigen×2 only) still compiles at fixed 128-bit — those tests
  self-pin each worker thread to the compile-time VL inside `test_run`
  (`arm64_sve_pin_vl(16)`, no external launcher; a failed pin skips honestly).
  `sleef_sve` + sve_rot×5 migrated to the true-VLA `tests_sve_vla` library
  (no `-msve-vector-bits`, all-VL 128..1024-bit).
- **SME caveat**: "HWCAP2_SME=0 / `SMSTART` raises SIGILL" only proves SME is
  *not enabled by this kernel* — with `CONFIG_ARM64_SME=n` (check
  `/boot/config-$(uname -r)`; the SME sysctl
  `/proc/sys/abi/sme_default_vector_length` will also be absent) EL0 `SMSTART`
  traps regardless of silicon. A kernel-level "absent" is not a hardware
  verdict; say "software-invisible" and state the boundary.

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
  engines `scripts/pr_loop.py` + `scripts/pr_local_review.py`（the 5th bot）; `/pr-loop` command
  (`.claude/commands/`) and Trae rule (`.trae/rules/`) are content-twins.
- **CI 无人值守分诊**（cron/双车道/熔断/kill switch 运维手册）:
  `docs/build-deploy-pr-ci/ci-triage.md`; engine `scripts/ci_triage.py`（pr_loop 姊妹件,
  main CI 红 → issue → headless agent 修复 → fix/ci-* PR）。
