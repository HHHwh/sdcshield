/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_gemm_double_max_arm
 * @parblock
 * ARM64-only maximum-matrix GEMM SDC stress (request 2026-10-09: eigen +
 * SVE, largest matrix parameters, maximum pressure). Eigen dynamic-square
 * GEMM whose dimension is chosen at init as the largest memory-safe square:
 *
 *   N = clamp(min(sqrt(ram_per_worker / (RSS_MATRIX_RATIO * 8)),
 *             n_cap(workers)), 1024, 4096)
 *
 * RSS_MATRIX_RATIO = 6.0 covers the four per-thread matrices (l_lhs,
 * l_rhs, l_prod and the reroll-time product x) plus headroom for Eigen's
 * GEMM packing buffers. A worker-count cap then bounds the aggregate
 * footprint: 4096 up to 8 workers, then 4096/cbrt(workers/8) — 608
 * workers land at N=1024 (4 x 8 MB per thread, ~20 GB node-wide);
 * N=4096 x 608 workers wedged init for 17+ min. The
 * working set overflows L2 and L3 and pressures the TLB — the
 * cache-overflow trigger path validated by the sve512_*_svd family —
 * while the loop keeps the all-core FMA datapath saturated for the whole
 * budget.
 *
 * SDC-excitation ingredients (docs/sdc-excite-reproduce/ matrix):
 *  - cache/TLB overflow: N*N*8 B per matrix (tens of GB node-wide);
 *  - access-pattern switching: every kRerollEvery iterations the loop
 *    flips GEMM compute -> random fill (store storm) -> golden recompute
 *    -> memcmp (streaming read) — di/dt and bus-turnaround transients;
 *  - per-thread operand re-rolls (H11'/P14 hardening) defeat error
 *    hiding in cached operands;
 *  - byte-exact memcmp vs the recomputed golden on every iteration.
 *
 * Compiled only on aarch64 in the tests_sve set (-march=armv8.2-a+sve),
 * so Eigen's ARM SVE packet backend drives the GEMM kernels. x86-64
 * untouched (stock gemm_double_dynamic_square stays at M_DIM 256).
 * Non-aarch64 hosts: the file is not even in the build (meson guard).
 *
 * Run requirement: this TU compiles with -msve-vector-bits=128 like the
 * rest of tests_sve, so on a host whose runtime vector length differs
 * (Kunpeng 920 runs VL=256) it MUST run under the VL-pinning launcher
 * (prctl PR_SVE_SET_VL 16 | PR_SVE_VL_INHERIT before exec, the
 * scripts/eigen-sve-double/ precedent). Bare runs at mismatched VL
 * overflow SVE packet tails and heap-corrupt the process (observed as
 * free()/munmap_chunk aborts in validate4b; clean under pinning per
 * validate4c3).
 * @endparblock
 */

#include <sandstone.h>

#include <Eigen/Core>

#include <cmath>
#include <cstdint>
#include <unistd.h>

using namespace Eigen;

typedef Matrix<double, Dynamic, Dynamic> Mat;

/* Memory-safe dimension bounds. N=4096 -> 4 matrices x 128 MB per thread. */
#define MDIM_MIN 1024
#define MDIM_MAX 4096
#define RSS_MATRIX_RATIO 6.0

namespace {
constexpr int kRerollEvery = 16;

int g_dim = MDIM_MIN;

int choose_dim(struct test *test)
{
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    int nthreads = thread_count();
    double ram_per_worker = (pages > 0 && page_size > 0 && nthreads > 0)
                                ? (double)pages * (double)page_size / nthreads
                                : 0.0;

    int64_t knob = get_testspecific_knob_value_int(test, "mdim", 0);
    if (knob > 0)
        return (int)(knob < MDIM_MIN ? MDIM_MIN : (knob > MDIM_MAX ? MDIM_MAX : knob));

    if (ram_per_worker <= 0.0)
        return MDIM_MIN;    /* introspection unavailable: conservative floor */
    int n_mem = (int)std::sqrt(ram_per_worker / (RSS_MATRIX_RATIO * 8.0));
    /* Concurrency cap: at high worker counts the node-wide footprint
     * (4 * N*N * 8 B per worker) thrashes shared DRAM bandwidth and
     * first-touch page faults; N=4096 x 608 workers (311 GB) wedged
     * init for 17+ min on a full node. Keep MDIM_MAX up to 8 workers,
     * then roll off as 1/cbrt(workers/8): 64 -> 2048, 608 -> 967 ->
     * floored at MDIM_MIN (1024, ~20 GB node-wide). */
    int n_cap = (nthreads <= 8) ? MDIM_MAX
                              : (int)(MDIM_MAX / std::cbrt(nthreads / 8.0));
    if (n_cap < MDIM_MIN)
        n_cap = MDIM_MIN;
    if (n_mem > n_cap)
        n_mem = n_cap;
    return (n_mem < MDIM_MIN) ? MDIM_MIN : (n_mem > MDIM_MAX) ? MDIM_MAX : n_mem;
}

void fill_rand(Mat &m)
{
    for (int r = 0; r < m.rows(); ++r)
        for (int c = 0; c < m.cols(); ++c)
            m(r, c) = frandom_scale(2.0) - 1.0;
}
}

/* Runs as .test_preinit in the parent process: thread_count() there is the
 * requested total (-n / all cores).  In the forked child the framework
 * rewrites thread_count() to the slice range (e.g. 38), which made choose_dim
 * size N for the wrong worker count (-n 64 logged "38 threads", N=2436). */
static int eigen_gemm_double_max_arm_preinit(struct test *test)
{
    g_dim = choose_dim(test);
    double gb = (double)g_dim * g_dim * 8.0 * 4.0 / (1 << 30);
    log_info("eigen_gemm_double_max_arm: N=%d (~%.2f GB per thread, %d threads)",
             g_dim, gb, thread_count());
    return EXIT_SUCCESS;
}

static int eigen_gemm_double_max_arm_run(struct test *test, int cpu)
{
    const int dim = g_dim;
    Mat l_lhs, l_rhs, l_prod;
    l_lhs.resize(dim, dim);
    l_rhs.resize(dim, dim);
    l_prod.resize(dim, dim);
    fill_rand(l_lhs);
    fill_rand(l_rhs);
    l_prod = l_lhs * l_rhs;
    int since_reroll = 0;

    do {
        Mat x;
        x = l_lhs * l_rhs;

        memcmp_or_fail(x.data(), l_prod.data(), dim * dim);

        if (++since_reroll >= kRerollEvery) {
            since_reroll = 0;
            fill_rand(l_lhs);
            fill_rand(l_rhs);
            l_prod = l_lhs * l_rhs;
        }
    } while (test_time_condition(test));
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_gemm_double_max_arm, "Eigen max-matrix GEMM payload (double, dynamic, memory-safe max square, ARM64+SVE packet backend)")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_preinit = eigen_gemm_double_max_arm_preinit,
  .test_run = eigen_gemm_double_max_arm_run,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
