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

./builddir/sdcshield --list-tests             # default PROD set AFTER spec filtering — host-dependent:
                                               #   SVE1 host:  565 PROD / 675 q0 / 683 q-1  (2026-11-03 merge 复核:
                                               #               main #472 迁移/增量 + feat/eigen-sve-unification
                                               #               +12 SVE eigen cases, − eigen_svd_bidiag_sve;
                                               #               full chain in README)
                                               #   14 eigen *_sve2 companions (2026-11-13, feat/eigen-sve2-companion)
                                               #     are INVISIBLE in every list 口径 on this SVE1 host — spec filter
                                               #     precedes list; explicit -e registers them and skips honestly.
                                               #     SVE2 host: default set +10 PROD, q-1 +14 (4 Jacobi are SKIP-level).
                                               #   no-SVE 920 主机: 458 PROD / 566 q0 / 570 q-1  (2026-11-17 实测于
                                               #               no-SVE 920 主机 (not THIS SVE1-256 host), vendored 5 库
                                               #               install/ 全在位口径, 依赖缺位会相应减对应测试组;
                                               #               = 迁移 111 (arm0102_* 基础 10 项 2026-11-03 退册) +
                                               #               arm_add 7 + _neon 14 + cache 新 9 PROD;
                                               #               eigen SVE 12 新/退役 1 + SVE2 配套 14 全
                                               #               ARM64_REQUIRES_SVE/SVE2 门控, 净变化 0)
                                               # full dated chain: README「测试用例与检测能力」
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
  `eigen_svd_cdouble_sve` 321s / `eigen_svd_bidiag_sve` 343s; the
  latter case — and its `SDC_RSS_BIDIAG` table entry — was retired
  2026-11-02, historical note only):
  `sdc_joint_dim_slice` = min(n_mem, n_run, clamp) — n_mem from a
  per-NUMA-node four-source budget (phys×‰ / MemAvailable×0.9 / cgroup
  v1+v2 / CommitLimit−Committed_AS×0.9 when vm.overcommit_memory=2 —
  no-op otherwise, issue #310) against measured per-family RSS
  coefficients (RSS_RATIO_TABLE),
  n_run from a probe power-law fit × the AR all-core-slowdown table
  (family×topology), refined by a VmHWM feedback loop (per-slice EMA,
  median aggregation, 0.7× table floor). Explicit `-O <test>.mdim/n`
  bypasses sizing; the explicit-knob `[内存越界]`/`[欠压力]`
  expected-memory warning (the T11 block sits inside the
  aarch64+linux guard — x86-64 prints none for explicit knobs; the
  tags themselves also fire on x86-64 from the arch-agnostic
  `sdc_auto_square_dim`/`sdc_auto_linear_count` adaptive paths)
  fires only for the ten SVD template-family users (knob routed
  through the joint layer via `calibrated_auto_dim`) —
  gemm/sparse/sve callers still short-circuit the knob in
  their own init (before the arch guard), no warning yet (T11b
  follow-up; 10/47 eigen cases covered — 32 at the 2026-11-03
  unification docs count + `eigen_gemm_double_ab_split` (migration
  #472 content, in the same-day unification merge; no -O mdim knob,
  dimension from the raw `GEMM_K_DIM` env) + 14 `_sve2` companions
  (2026-11-13, inits short-circuiting the knob like their `_sve`
  templates) — the covered ten unchanged; the fourth bidiag caller was
  retired with its case 2026-11-02). The pre-fix "ULP-level
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
  The 14 Eigen SVE cases (2026-11-03, feat/eigen-sve-unification) are all
  tier-locked by the language (fixed-width packets) and built once per tier
  with a runtime function-pointer dispatch, unified on the per-tier body +
  dispatcher framework in
  `tests/cpu/eigen_svd/sandstone_eigen_sve_dispatch.h` (2 pre-existing:
  `eigen_svd_cdouble_sve`/`eigen_gemm_double_max_arm`; 12 new: BDCSVD×4,
  JacobiSVD×4 — SKIP-level, GEMM×4; `eigen_svd_bidiag_sve` retired
  2026-11-02, its purpose eliminated by #410). Each of the 14 has an
  `_sve2` companion (2026-11-13, feat/eigen-sve2-companion, 14+14): the
  same per-tier bodies are dual-compiled per tier — `armv8.2-a+sve`
  (Eigen ns `EigenSVE<t>`) vs `armv8.2-a+sve2` (Eigen ns `EigenSVE2<t>`;
  2026-11-17 前曾用 `armv9-a+sve2`——armv9-a 架构名需 GCC 11+，曾致 Multi-OS
  Verify 的 20.03/22.03 gcc-10 镜像 Build 全红（PR #661 回归，run 44011713221）；
  +sve2 在 armv8.x-a base 上 GCC 10 即可编；双 march 40 库 objdump A/B 实测
  （2026-11-17 干净同源全量构建对比，flags 除 march 外逐项全等）：40/40 库
  原始反汇编有差异（分支偏移/地址移位 + 标量调度重排，合计 ~10.2k 行），
  32/40 库助记符多重集逐库全等，余 8/40 库另有 ±1..13 次纯标量计数微差
  （mov/ldr/str/stp/ldp/ldur/stur/nop/add/lsl/madd/mul/b.*，含一次整数
  mul↔madd 融合翻转；零 SVE 族、零 FP 族计数变化 → 数值面不变），
  WHILEWR 逐库计数 A/B 全等（合计 1549 实例分布于 35 库），诚实边界不变) —
  and each `<pfx>_sve2` dispatcher is the `_sve` template plus exactly six
  differences (id/env `_sve2`, pfx at every call site, an
  `arm64_cpu_has_sve2()` honest-SKIP gate before `arm64_require_sve_vl`,
  `ARM64_REQUIRES_SVE2`, the "SVE2 packet backend … counterpart" description
  string, header counterpart + honest-scope note). (Exception,
  disclosed in the commit: where the `_sve` template's header carried
  stale build-mechanics claims that would contradict the `_sve2` build,
  the header is factually corrected — the two known cases:
  `eigen_svd_cdouble_sve2` ×3, `eigen_gemm_double_max_arm_sve2` ×1.)
  Honesty boundary (corrected 2026-11-17): the stress kernels are an SVE1
  subset, but GCC idiom-recognizes Eigen's aliasing-checked copy plumbing
  (call_assignment/gemm_pack_rhs loops) into WHILEWR — an SVE2-only
  address-overlap instruction, 1549 instances in 35/40 libraries, present
  since #661 (armv9-a+sve2) and per-library IDENTICAL after the march
  switch (the earlier "emits ZERO SVE2-only instructions, objdump-audited
  40/40" claim missed this class; caught by the binutils SVE1-only
  assembler gate). The companions still provide an SVE2-gated entry
  point, NOT SVE2-only datapath coverage, and runtime is HWCAP2_SVE2-
  gated so WHILEWR only ever executes on SVE2 hardware. gem5 evidence:
  SE pairing matrix 10 body
  families × 8 legs, per-tier sve2↔sve checksums byte-identical, plus an
  FS real-binary leg validating the dispatcher path
  (`eigen_svd_double_sve2 -n 2`, VL=128, pass). Every payload runs all
  four tiers since the vendored-Eigen ptranspose
  scatter rewrite (issue #410, 2026-11-01: the memory-bridge form read past
  `buf[N][NC]` when NC > N — 1024-bit NC=8; the scatter form is in bounds
  for any N/NC and bit-identical in square shapes, gem5-verified at
  128/256/512/1024 with the 512 checksum reproducing the pre-fix value).
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
  auto-repair PRs, deterministic merge gate): `docs/build-deploy-pr-ci/agent-full-automation.md`.
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
- **Process artifacts**: `docs/superpowers/plans/` only — specs/ and
  inventory/ were cleaned out of the repo by 31f6a99e (2026-11-17);
  recover them from git history at `31f6a99e^`.
- **PR review-fix loop** (agent-driven: five bots (four external SaaS + the local
  headless claude adversarial reviewer) → subagent critical triage →
  fix → loop → merge): `docs/build-deploy-pr-ci/pr-loop.md`;
  engines `scripts/pr_loop.py` + `scripts/pr_local_review.py`（the 5th bot;
  unattended orchestration is task_loop's）. Merge 政策（2026-11-13 红线反转，
  spec 2026-11-12-full-auto-pipeline）：夜间 pipeline 的 merge 门对引擎 PR
  （`fix/issue-N|fix/ci-N|fix/revert-prN`）六门槛全过自动 squash merge；全
  docs-only PR 走快道（标题 `WIP:` 前缀 = 草稿闸，去掉 = 当夜可合）；用户非
  docs PR 永不自动 merge；自动 resolve review threads 仍然禁止。`/pr-loop`
  and `/dev-loop` commands (`.claude/commands/`) and Trae rules
  (`.trae/rules/`) are content-twins.
- **task-loop 无人值守任务闭环**（统一编排引擎: CI 红/main 审查/会话/手建
  issue → agent → PR 闭环;ci_triage 的泛化后继,含迁移说明。2026-11-13 起
  调度为 nightly pipeline: cron `0 23 * * *` 拉起 `task_loop.py pipeline`
  （23:00–09:00 窗口三泳道并行 + merge 门 + 09:00 软停，`@reboot` 落窗
  自愈）；旧 scan/audit/第5bot 三行旧 cron 将随 task-loop.md §3.1 runbook
  部署删除，`scan` 降级为会话手动单轮
  薄壳）: `docs/build-deploy-pr-ci/task-loop.md`;引擎 `scripts/task_loop.py`;
  会话命令 `/dev-loop`。
