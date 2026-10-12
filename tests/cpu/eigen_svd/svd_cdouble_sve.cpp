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
 * The matrix dimension is chosen at init time from the RAM available per
 * worker (see size_matrix() below): the largest N whose peak RSS fits the
 * per-worker memory share, capped at the largest N that finishes the whole
 * test (prime BDCSVD + recomputation, both in run) inside a 10-minute
 * budget, and floored at 300. An explicit override is available via
 * `-O eigen_svd_cdouble_sve.mdim=N` (300..6000).
 *
 * Because SVE instructions are baked into the object, the test probes for
 * SVE availability at init time using plain C (no SVE instructions) and
 * reports a clean CpuNotSupported skip when the running CPU has no SVE
 * (e.g. Kunpeng 920). On SVE-capable hardware (e.g. Kunpeng 930) the test
 * runs and stresses the SVE FMA/vector units for SDC detection.
 *
 * @note This test requires at least 2 threads to run.
 * @endparblock
 */

#include "sandstone_eigen_common.h"

#if defined(__aarch64__)

#include <sys/auxv.h>
#include <asm/hwcap.h>
#include <unistd.h>
#include <cmath>

using namespace Eigen;        /* renamed to EigenSVE via -DEigen=EigenSVE */
typedef Matrix<std::complex<double>, Dynamic, Dynamic> Mat;
typedef Eigen::BDCSVD<Mat> SVD;

/* Runtime dimension mode: the template allocates and compares against
 * d->dim, which size_matrix() derives from available memory. */
using eigen_svd_cdouble_sve_test = EigenSVDTest<SVD, Eigen::Dynamic>;

/* Dimension bounds, all measured on cortex x3b (VL=128 pinned, same
 * compile flags as this TU, 2026-09-19 — see the plan file
 * docs/superpowers/plans/2026-09-19-svd-cdouble-sve-mdim-5800.md):
 *
 *   MDIM_MIN  = 300: the NEON counterpart's parity point; the floor.
 *   MDIM_TIME = 4400: the largest N whose FULL test time fits a 10-minute
 *                     budget. The framework counts the prime BDCSVD plus
 *                     the DUT recomputation, both in test_run — two
 *                     complete decompositions. Measured single-BDCSVD
 *                     times: 2400→40s, 3600→134s, 4400→243s, 4800→322s,
 *                     5600→511s, 5800→563s, 6000→622s (~N³ scaling);
 *                     4400×2 = 487 s total (19% margin; 4600×2 = 583 s
 *                     leaves only 3% for load variance, 5800×2 = 1124 s
 *                     measured over budget).
 *   MDIM_MAX  = 6000: knob ceiling (6000 alone already busts the budget;
 *                     it exists for opt-in experiments on faster hosts).
 *
 * Memory model: measured peak RSS is a stable ~9.7-10x the input matrix
 * volume (BDCSVD's real-typed m_naiveU/m_naiveV/m_computed plus U/V
 * copies and workspace — N=2400→0.86GB, 4400→2.82GB, 6000→5.22GB).
 * The sizing uses 12x (20% headroom) of 16*N² bytes per worker.
 */
#define MDIM_MIN 300
#define MDIM_TIME_CAP 4400
#define MDIM_MAX 6000
#define RSS_MATRIX_RATIO 12.0

/*
 * Choose the matrix dimension:
 *   mdim knob (if set)  -> clamped to [MDIM_MIN, MDIM_MAX]
 *   else                -> min(memory-limited N, MDIM_TIME_CAP), floored
 *                          at MDIM_MIN; never below the floor even on
 *                          tiny-memory hosts (300 needs only ~0.2 GB).
 */
static int size_matrix(struct test *test, int *out_nthreads, double *out_gb_per_worker)
{
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    int nthreads = thread_count();
    /* 0.65: reserve 35% of physical DRAM for kernel/pagecache/framework.
     * The un-reserved sizing let 608 workers hit peak RSS simultaneously
     * at exactly 100% of node DRAM and the batch system OOM-killed whole
     * jobs ("reason: job oom", 2026-10-09, four nodes in lockstep on the
     * 17th test of the campaign). 0.85 itself still sized 608 workers
     * at ~480 GB, brushing the dynamic batch mem limit (node free RAM
     * hovers at 450-530 GB), so the reserve was widened to 35%. */
    if (out_nthreads) *out_nthreads = nthreads;
    double ram_per_worker = (pages > 0 && page_size > 0 && nthreads > 0)
                                ? (double)pages * (double)page_size * 0.65 / nthreads
                                : 0.0;
    if (out_gb_per_worker) *out_gb_per_worker = ram_per_worker / (1 << 30);

    int64_t knob = get_testspecific_knob_value_int(test, "mdim", 0);
    if (knob > 0)
        return (int)(knob < MDIM_MIN ? MDIM_MIN : (knob > MDIM_MAX ? MDIM_MAX : knob));

    /* memory-limited N: 12 * 16 * N^2 <= per-worker RAM  =>
     * N <= sqrt(ram_bytes_per_worker / 192) */
    if (ram_per_worker <= 0.0)
        return MDIM_MIN;   /* introspection unavailable: conservative floor */
    int n_mem = (int)std::sqrt(ram_per_worker / (RSS_MATRIX_RATIO * 16.0));

    int n = n_mem < MDIM_TIME_CAP ? n_mem : MDIM_TIME_CAP;
    return n < MDIM_MIN ? MDIM_MIN : n;
}

/* Chosen dimension, decided ONCE in the preinit (parent process, before
 * the framework computes test durations) so that both the matrix size and
 * the test timing follow it. Forked children inherit it. */
static int g_dim = MDIM_MIN;

/*
 * preinit (runs once in the PARENT, before run_one_test computes
 * test_duration() — so writing test->desired_duration here is effective
 * for the very first run, unlike in test_init; same pattern as
 * ist_skip_preinit setting test->minimum_duration).
 *
 * Scales the framework's timing with the chosen dimension so neither the
 * run loop nor the ±25% overall-time check misfires:
 * - desired_duration = expected single-iteration time minus 5%: the
 *   do-while time condition is then already expired when iteration #1
 *   finishes -> exactly one iteration at EVERY dimension (a fixed value
 *   would make small dimensions loop for the whole budget and large ones
 *   start an un-budgeted second iteration).
 * - the derived timeout (test_timeout(): 5*duration+30s, 300s floor)
 *   scales along, covering both run-side decompositions (prime + DUT).
 * Model (measured, cortex x3b VL=128; plan 2026-09-19-...md):
 * t(N) ~= 243.4s * (N/4400)^3 + 1s fixed overhead — within ±2% of the
 * measured points at N>=2400 and conservative (over-estimates) below.
 */
static int size_matrix_preinit(struct test *test)
{
    int nthreads = 1;
    double gb_per_worker = 0.0;
    g_dim = size_matrix(test, &nthreads, &gb_per_worker);

    double iter_s = 243.4 * std::pow((double)g_dim / 4400.0, 3.0) + 1.0;
    test->desired_duration = (int)(iter_s * 950.0);   /* ms, -5% margin */
    return EXIT_SUCCESS;
}

/*
 * Probe SVE availability using only the vDSO / getauxval, which executes no
 * SVE instructions itself. We must not let any SVE-instrumented Eigen code
 * run on a CPU without SVE (it would SIGILL), so this is done before
 * delegating to the Eigen-backed init.
 */
static int sve_probe_and_init(struct test *test)
{
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SVE not available on this CPU; "
                 "eigen_svd_cdouble_sve requires SVE (e.g. Kunpeng 930)");
        return EXIT_SKIP;
    }
    log_info("M_DIM %d (sized in preinit from memory; expected iteration "
             "%.1f s, desired_duration %d ms; override with -O "
             "eigen_svd_cdouble_sve.mdim=N)",
             g_dim,
             243.4 * std::pow((double)g_dim / 4400.0, 3.0) + 1.0,
             test->desired_duration);

    /* Allocate the test data (the runtime-dimension template's run()
     * primes its own per-thread matrices; the init-time golden BDCSVD
     * became dead computation once run() stopped reading eigen_test_data's
     * matrices — removed. Timing note: desired_duration now covers exactly
     * the two run-side decompositions (prime + DUT); init no longer
     * contributes a third. */
    auto d = new eigen_svd_cdouble_sve_test::eigen_test_data;
    d->dim = g_dim;
    test->data = d;
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_svd_cdouble_sve, "Eigen SVD complex<double> (ARM64 SVE vector backend, dimension sized from available memory); counterpart of eigen_svd_cdouble")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_preinit = size_matrix_preinit,
  .test_init = sve_probe_and_init,
  .test_run = eigen_svd_cdouble_sve_test::run,
  .test_cleanup = eigen_svd_cdouble_sve_test::cleanup,
  .minimum_cpu = 0,
  /* desired_duration is written by test_preinit (parent, before the
   * framework computes test_duration for the first run): expected single-
   * iteration time from the measured t(N) ~ 243.4s*(N/4400)^3 + 1s model,
   * minus 5%, so exactly one run iteration executes at every auto-sized
   * dimension and the derived timeout (5*duration+30s, 300s floor) scales
   * with the dimension — no under/overtime misreports at small dims. */
  .desired_duration = 0,
  .fracture_loop_count = 5,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif // __aarch64__
