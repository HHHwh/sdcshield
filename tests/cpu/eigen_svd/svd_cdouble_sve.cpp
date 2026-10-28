/**
 * @file
 *
 * @copyright
 * Copyright 2026 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_svd_cdouble_sve
 * @parblock
 * This is the ARM64 SVE-backed counterpart of @ref eigen_svd_cdouble.
 *
 * It runs the same divide-and-conquer bi-diagonalization SVD
 * (Eigen::BDCSVD) on a complex double input matrix, but the Eigen code is
 * instantiated against the SVE packet backend by compiling this translation
 * unit with `-DEIGEN_ARM64_USE_SVE` and a `-march=...+sve` target, and the
 * Eigen namespace is renamed (`-DEigen=EigenSVE`) so it links cleanly
 * beside the NEON baseline build.
 *
 * The matrix dimension is chosen in the CHILD test_init (P0-1 fix,
 * 2026-10-27): an explicit knob/env override wins, else the joint solver
 * sdc_joint_dim_slice returns min(n_mem, n_run) — n_mem sizes the
 * per-NUMA-node memory budget against the measured complex-BDCSVD RSS
 * coefficient (RSS_RATIO_TABLE SDC_RSS_BDCSVD_C128), n_run caps the
 * full-core single iteration (prime BDCSVD + recomputation, both in run)
 * at a 140 s table-AR budget via the measured power-law fit and the
 * memory-bound all-core slowdown factor (140 s at the table AR=25 works
 * out to ~218 s at this host class' measured worst-child AR≈39 — see the
 * run_budget_ms note in sve_probe_and_init) — clamped to [300, 6000]. The
 * preinit no
 * longer sizes the matrix at all: it only writes a desired_duration
 * UPPER bound from the same memory ceiling, and sizing in the child also
 * fixes exec_each_test children, whose freshly-exec'd image never ran the
 * preinit and previously sized every run at the un-initialised 300.
 * An explicit override is available via
 * `-O eigen_svd_cdouble_sve.mdim=N` (300..6000).
 *
 * Because SVE instructions are baked into the object, the test gates at
 * init time via the framework-layer arm64_require_sve_vl helper (the
 * HWCAP/rdvl probe executes no SVE instructions — all-VL Phase 2 P5)
 * and reports a clean CpuNotSupported skip when the running CPU has no
 * SVE (e.g. Kunpeng 920). On SVE-capable hardware (e.g. Kunpeng 930)
 * the test runs and stresses the SVE FMA/vector units for SDC detection.
 *
 * All-VL Phase 2 (design 2026-10-27 §4.7): Eigen's SVE packet backend is
 * compile-time fixed-width (arm_sve_vector_bits(EIGEN_ARM64_SVE_VL));
 * VLA is un-compilable at the C++ language level (sizeless struct
 * members / NumTraits sizeof — twice-confirmed spike). The payload is
 * therefore compiled once per tier under its own Eigen namespace and the
 * vector-length-agnostic dispatch TU here selects the variant matching
 * the framework-pinned worker VL (128/256/512; the 1024-bit complex
 * variant is not built because Eigen's SVE complex ptranspose reads past
 * buf[N][NC] when NC > N — upstream defect, GCC -Warray-bounds). test_init
 * allocates for the tier it observes and test_run re-checks the worker's
 * readback against that tier; a refused pin skips, a mismatch fails
 * closed.
 *
 * @note This test requires at least 2 threads to run.
 * @endparblock
 */

#include "sandstone_eigen_common.h"
#include "sdc_stress_config.h"

#if defined(__aarch64__)

#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#include <cmath>

/* Per-tier payload namespaces (svd_cdouble_sve_body.cpp). The EigenSVDTest
 * data type is itself Eigen-parameterised, so allocation/cleanup/run all live
 * in the variant TU; this dispatcher only sees void* — design §4.7. */
namespace svd_vl128  { void *make_data(int); void cleanup(struct test *); int run_body(struct test *, int); int solve_once(int); }
namespace svd_vl256  { void *make_data(int); void cleanup(struct test *); int run_body(struct test *, int); int solve_once(int); }
namespace svd_vl512  { void *make_data(int); void cleanup(struct test *); int run_body(struct test *, int); int solve_once(int); }

/* Tier chosen at init (from the chip maximum) and re-checked at run (from the
 * worker's pinned VL). They must agree: switching variants mid-test would
 * reinterpret the Eigen objects allocated by a different instantiation. */
static long eigen_svd_cdouble_sve_tier_bits;

/* Joint-solver probe dispatch (P0-1): route the sizing probe into the
 * per-tier namespace recorded above. The probe executes that tier's
 * fixed-width Eigen packets on the init thread, whose VL is the
 * framework-pinned control-thread tier (inherited before this aux thread
 * was created) — the same tier make_data allocates for and test_run
 * re-checks, so packet width and executing VL always match. */
static int solve_once_cdouble_sve(int n)
{
    switch (eigen_svd_cdouble_sve_tier_bits) {
    case 128:  return svd_vl128::solve_once(n);
    case 256:  return svd_vl256::solve_once(n);
    case 512:  return svd_vl512::solve_once(n);
    default:   return -1;   /* unreachable: tier validated before probing */
    }
}

/* Runtime dimension mode: the template allocates and compares against
 * d->dim, which the CHILD test_init derives (knob/env > joint solver). */

/* Dimension bounds (measurement history: cortex x3b, VL=128 pinned, same
 * compile flags as this TU, 2026-09-19 — plan file
 * docs/superpowers/plans/2026-09-19-svd-cdouble-sve-mdim-5800.md):
 *
 *   MDIM_MIN  = 300: the NEON counterpart's parity point; the floor and
 *                    the joint-solver lo (quick mode collapses here).
 *   MDIM_MAX  = 6000: knob ceiling AND joint-solver hi (6000 alone
 *                    already busts the time budget; it exists for opt-in
 *                    experiments on faster hosts).
 *
 * Measured single-BDCSVD times on that host: 2400→40s, 3600→134s,
 * 4400→243s, 4800→322s, 5600→511s, 5800→563s, 6000→622s (~N³ scaling);
 * the old 4400 time cap and the 12x-of-16·N²-per-worker memory model were
 * superseded by the joint solver (child side: RSS_RATIO_TABLE
 * SDC_RSS_BDCSVD_C128 = 0.2055 KB/n²/thread measured differential
 * coefficient + the table-AR iteration budget; preinit side: the
 * t(N) = 243.4s·(N/4400)³ + 1s fit of those same points, still used for
 * the desired_duration upper bound below).
 */
#define MDIM_MIN 300
#define MDIM_MAX 6000

/*
 * preinit (runs once in the PARENT, before run_one_test computes
 * test_duration() — so writing test->desired_duration here is effective
 * for the very first run, unlike in test_init; same pattern as
 * ist_skip_preinit setting test->minimum_duration).
 *
 * P0-1 (spec §4.5, q2 invariant): the dimension itself is no longer sized
 * here — the child's test_init does that per NUMA node with the joint
 * solver, which also fixes exec_each_test children (their freshly-exec'd
 * image never ran this preinit, so the old parent-sized static arrived
 * un-initialised at 300). The preinit only writes an UPPER bound:
 *
 *   n_hi  = sqrt(budget650 / (workers × c_BDCSVD_C128)), clamped to
 *           [MDIM_MIN, MDIM_MAX] — the same three-source memory budget
 *           and RSS-table coefficient (sdc_rss_table_c(), not a literal —
 *           the table was re-calibrated once already: GEMM_F64
 *           0.0290→0.0315) the child's n_mem uses, with workers = the
 *           run's actual whole-machine worker count
 *           (sdc_total_threads(), available in preinit: commit_shmem()
 *           runs before test_preinit). The child's n_mem denominator is
 *           the same worker count scaled to its node
 *           (sdc_joint_dim_slice folds workers/total_cpus into
 *           node_cpus), so n_hi tracks it on every host class — not
 *           just the 127-worker reference the value was historically
 *           hard-coded to.
 *   dur   = t(n_hi) × 0.95,  t(N) = 243.4s·(N/4400)³ + 1s  (x3b fit,
 *           within ±2% of the measured points at N>=2400).
 *
 * Child n <= n_hi: the child returns min(n_mem, n_run, hi), n_mem uses
 * the same budget/c/worker-scaled threads, and its most-conservative
 * AR(25) keeps n_run below the memory ceiling on this host class.
 * (Correction of an earlier comment that claimed a host with fewer
 * threads "only shrinks n_hi — the safe direction": that was backwards.
 * A hard-coded 127 makes the child's n_mem denominator SMALLER than
 * n_hi's on sub-127-worker hosts, so child n can EXCEED n_hi by up to
 * sqrt(127/workers) — dur then UNDER-estimates the real iteration and
 * the derived timeout 5×duration+30 s (300 s floor) loses its headroom,
 * which is the UNSAFE direction, the very P0 timeout mode this PR
 * fixes. Using the actual worker count restores the invariant on every
 * host; the child's own n_run still bounds one iteration at ~140 s
 * (模型, run_budget_ms=140000) / ~213 s (实测最坏) as a second belt.)
 */
static int size_matrix_preinit(struct test *test)
{
    /* 最保守 AR(25) + RSS 表 c 推 n 上界, 再推 duration 上界:
     * child 精算 n 恒 <= 此上界(spec §4.5)。c 引用表函数而非字面量
     * (0.2055)——GEMM_F64 表值已实测重标定过一次(0.0290→0.0315),
     * 字面量会静默陈旧; 分母用实际全机 worker 数而非参考机 127,
     * 子进程 n_mem 的分母是同一 worker 口径(折算到节点), 硬编码 127
     * 在 worker<127 的主机上使 child n 可超 n_hi 达 √(127/workers)。 */
    double budget_kb = (double)sdc_budget_bytes(650) / 1024.0;
    double c_kb = sdc_rss_table_c(SDC_RSS_BDCSVD_C128);
    int workers = sdc_total_threads();
    if (workers < 1)
        workers = 1;
    if (c_kb <= 0.0)
        c_kb = 0.2055;   /* 防御: 表缺失时退回实测字面量口径 */
    int n_hi = (int)std::sqrt(budget_kb / ((double)workers * c_kb));
    if (n_hi > MDIM_MAX) n_hi = MDIM_MAX;
    if (n_hi < MDIM_MIN) n_hi = MDIM_MIN;
    double iter_s = 243.4 * std::pow((double)n_hi / 4400.0, 3.0) + 1.0;
    test->desired_duration = (int)(iter_s * 950.0);   /* ms, -5% margin */
    return EXIT_SUCCESS;
}

/*
 * Gate SVE availability via the framework layer (all-VL Phase 2 P5):
 * arm64_require_sve_vl probes HWCAP_SVE and reads rdvl — no SVE
 * instructions execute on the no-SVE path. We must not let any
 * SVE-instrumented Eigen code run on a CPU without SVE (it would
 * SIGILL), so this stays before the Eigen-backed init's allocations;
 * the framework's compiler_minimum_device gate already skips non-SVE
 * hosts before init — this keeps a test-specific reason if that
 * ordering ever changes.
 */
static int sve_probe_and_init(struct test *test)
{
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    /* The framework pinned the control thread before this aux thread was
     * created, so `svl` is the tier the workers will run at. VALIDATE the
     * tier before anything else: the joint solver's probe below executes
     * that tier's fixed-width Eigen packets on THIS thread, and make_data
     * allocates the variant for it; test_run compares the worker's
     * readback against this value and fails closed on any divergence
     * instead of reinterpreting objects built for another tier. */
    const long tier = svl * 8;
    switch (tier) {
    case 128:
    case 256:
    case 512:
        eigen_svd_cdouble_sve_tier_bits = tier;
        break;
    case 1024:
        /* Eigen's SVE complex ptranspose indexes buf[N][NC] with i < NC,
           which is only in bounds when NC <= N (=4 here). At 1024-bit
           NC = 8, so the complex payload has a genuine out-of-bounds read
           upstream; we deliberately do not build that variant. */
        log_skip(CpuNotSupportedSkipCategory,
                 "%s: 1024-bit Eigen complex packets are unsound upstream "
                 "(SVE ptranspose reads past buf[N][NC] when NC > N); "
                 "no variant built for this tier", test->id);
        return EXIT_SKIP;
    default:
        log_skip(CpuNotSupportedSkipCategory,
                 "%s: no Eigen SVE variant compiled for the %ld-bit tier",
                 test->id, tier);
        return EXIT_SKIP;
    }

    /* Dimension (P0-1, spec §4.5): knob > env > joint-solver auto, all in
     * the CHILD so exec_each_test images size themselves. The auto branch
     * probes solve_once on THIS thread (its VL is the tier just recorded)
     * and returns min(n_mem, n_run): per-NUMA-node memory budget against
     * the measured complex-BDCSVD RSS coefficient, and a full-core
     * single-iteration budget via the power-law fit and the memory-bound
     * AR factor. quick mode collapses to MDIM_MIN.
     *
     * run_budget_ms deviates from the joint contract value 200000 (= 300 s
     * hard grace × 0.67): the measured WORST-child slowdown on a
     * single-memory-node host exceeds the AR table's 1-node mem-bound
     * entry (25). Measured 2026-10-27, 127 threads, n=1100 (~31 GiB
     * footprint): the node-0 (local-DRAM) children finish while the
     * children on the memory-less nodes (remote DRAM) see an effective AR
     * of ~39 — 1.55×, just past the 1.5× margin the 0.67 factor embeds.
     * With 200000 the solver picked n≈1229 and the remote children could
     * not finish one iteration inside the 300 s grace (SIGQUIT at
     * 304-314 s, reproduced twice). 140000 lands n_run ≈ 1050-1095
     * (n=1100 measured 228 s wall including init), i.e. ~225 s total —
     * inside the grace with ~25 % margin, and within the plan's expected
     * [750, 1100]. The principled fix is the AR table entry itself
     * (framework/sdc_numa.cpp — Task 1 scope, flagged to the
     * coordinator); this per-test budget is the in-scope compensation
     * and should be revisited when the table is re-calibrated.
     * (注: n=1100 是定点实验档; auto 档在同一 run_budget_ms 下实测最坏
     * ~213 s, 见上方 size_matrix_preinit 注释, 两个口径不矛盾.) */
    int dim;
    int64_t knob = sdc_knob_int_clamped(test, "mdim", 0, MDIM_MIN, MDIM_MAX);
    if (knob == 0)
        sdc_env_knob_int_clamped("eigen_svd_cdouble_sve", "mdim", &knob,
                                 MDIM_MIN, MDIM_MAX);
    if (knob > 0) {
        dim = (int)knob;
    } else {
        struct sdc_joint_args ja = {
            .lo = MDIM_MIN, .hi = MDIM_MAX,
            .fam = SDC_RSS_BDCSVD_C128,    /* 0.2055 KB/n²/thread, measured */
            .bytes_per_elem = 16, .matrices_per_thread = 1, .rss_ratio = 12.0,
                    /* legacy triple (pre-table model), ignored while fam
                     * selects the RSS table */
            .shared_bytes_per_slice = 0,   /* make_data holds no matrices */
            .mem_fraction_permille = 650,
            .ar_fam = SDC_FAM_MEM_BOUND,
            .run_budget_ms = 140000,       /* worst-child iteration budget;
                                            * 140 s at the table AR=25 ≈
                                            * 218 s at the measured
                                            * worst-child AR≈39 (see above) */
        };
        try {
            dim = sdc_joint_dim_slice(test, "mdim", &ja,
                                      solve_once_cdouble_sve);
        } catch (...) {
            /* probe allocation failure (transient OOM &c): fall back to
             * the floor — same doctrine as the SVD template's calibrated
             * path ("上限宁小勿大"): a sizing probe must never terminate
             * the child. */
            log_warning("[联合求解] %s: mdim 探针异常 → 退守下限 %d",
                        test->id, MDIM_MIN);
            dim = MDIM_MIN;
        }
    }

    log_info("M_DIM %d (sized in child init: knob/env, else 联合求解 "
             "min(n_mem, n_run); preinit desired_duration %d ms is the "
             "upper-bound model; override with -O "
             "eigen_svd_cdouble_sve.mdim=N)",
             dim, test->desired_duration);

    /* Allocate the test data (the runtime-dimension template's run()
     * primes its own per-thread matrices; the init-time golden BDCSVD
     * became dead computation once run() stopped reading eigen_test_data's
     * matrices — removed. Timing note: desired_duration now covers exactly
     * the two run-side decompositions (prime + DUT); init no longer
     * contributes a third.) */
    void *data = nullptr;
    switch (eigen_svd_cdouble_sve_tier_bits) {
    case 128:  data = svd_vl128::make_data(dim);  break;
    case 256:  data = svd_vl256::make_data(dim);  break;
    case 512:  data = svd_vl512::make_data(dim);  break;
    default:
        return EXIT_SKIP;   /* unreachable: tier validated above */
    }
    test->data = data;
    return EXIT_SUCCESS;
}

/*
 * Per-tier run wrapper (design §4.7): re-read and re-pin THIS worker to
 * the framework-chosen tier, verify the readback equals both the pin and
 * the tier test_init allocated for, then dispatch to that tier's payload.
 * The framework pinned the control thread before init (so init saw the
 * same tier); the re-pin here is an idempotent second check that a silent
 * environment downgrade cannot slip a fixed-width packet onto the wrong
 * VL. The readback uses arm64_sve_vl_bytes() (.inst-encoded rdvl), NOT
 * svcntb(): in a -msve-vector-bits TU GCC constant-folds svcntb(), while
 * the raw rdvl reads the actual per-thread runtime VL.
 */
static int eigen_svd_cdouble_sve_run(struct test *test, int cpu)
{
    const long want_bytes = arm64_sve_vl_bytes();
    if (want_bytes <= 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "%s: SVE disappeared between pin and run", test->id);
        return EXIT_SKIP;
    }
    const long pinned = arm64_sve_pin_vl(want_bytes);
    if (pinned != want_bytes) {
        log_skip(CpuNotSupportedSkipCategory,
                 "%s: cannot pin the worker to the %ld-bit VL (prctl rc=%ld)",
                 test->id, want_bytes * 8, pinned);
        return EXIT_SKIP;
    }
    if (arm64_sve_vl_bytes() != want_bytes) {
        log_error("%s: pinned %ld bytes but read back %ld",
                  test->id, want_bytes, arm64_sve_vl_bytes());
        report_fail_msg("%s: SVE VL readback mismatch after pin", test->id);
    }
    if (want_bytes * 8 != eigen_svd_cdouble_sve_tier_bits) {
        log_error("%s: run tier %ld-bit differs from the %ld-bit variant the "
                  "data was allocated for", test->id, want_bytes * 8,
                  eigen_svd_cdouble_sve_tier_bits);
        report_fail_msg("%s: Eigen SVE variant/tier mismatch", test->id);
    }
    /* Design §4.9: record which instantiation actually ran. */
    log_info("%s: Eigen SVE svd variant = %ld-bit", test->id, want_bytes * 8);
    switch (want_bytes * 8) {
    case 128:  return svd_vl128::run_body(test, cpu);
    case 256:  return svd_vl256::run_body(test, cpu);
    case 512:  return svd_vl512::run_body(test, cpu);
    case 1024:
        log_skip(CpuNotSupportedSkipCategory,
                 "%s: 1024-bit Eigen complex packets are unsound upstream "
                 "(SVE ptranspose reads past buf[N][NC] when NC > N); "
                 "no variant built for this tier", test->id);
        return EXIT_SKIP;
    default:
        log_skip(CpuNotSupportedSkipCategory,
                 "%s: no Eigen SVE variant compiled for the %ld-bit tier",
                 test->id, want_bytes * 8);
        return EXIT_SKIP;
    }
}

static int eigen_svd_cdouble_sve_cleanup(struct test *test)
{
    switch (eigen_svd_cdouble_sve_tier_bits) {
    case 128:  svd_vl128::cleanup(test);  break;
    case 256:  svd_vl256::cleanup(test);  break;
    case 512:  svd_vl512::cleanup(test);  break;
    default: break;
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_svd_cdouble_sve, "Eigen SVD complex<double> (ARM64 SVE vector backend with per-tier 128/256/512-bit variants selected at the framework-pinned VL, dimension sized in child init by the joint memory/time solver); counterpart of eigen_svd_cdouble")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_preinit = size_matrix_preinit,
  .test_init = sve_probe_and_init,
  .test_run = eigen_svd_cdouble_sve_run,
  .test_cleanup = eigen_svd_cdouble_sve_cleanup,
  .minimum_cpu = 0,
  /* desired_duration is written by test_preinit (parent, before the
   * framework computes test_duration for the first run) as an UPPER-BOUND
   * estimate from the memory ceiling (P0-1: the child sizes the dimension
   * itself via the joint solver; this only keeps the parent's timing
   * expectation at or above the child's choice, so exactly one run
   * iteration executes and the derived timeout (5*duration+30s, 300s
   * floor) scales along). */
  .desired_duration = 0,
  .fracture_loop_count = 5,
  .quality_level = TEST_QUALITY_PROD,
    .arm64_requires = ARM64_REQUIRES_SVE,
END_DECLARE_TEST

#endif // __aarch64__
