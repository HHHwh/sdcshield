/**
 * @file
 *
 * @copyright
 * Copyright 2026 ISCAS.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b openblas_lu
 * @parblock
 * Stress test for the data-dependent control flow inside OpenBLAS's LAPACK
 * dgesv path (LU factorization with partial pivoting + triangular solve,
 * entered through the LAPACKE C interface — 2605 exported LAPACKE symbols
 * with zero prior coverage in this suite; the openblas_* tests so far only
 * exercised the cblas GEMM surface). Where GEMM is a straight-line vector
 * FMA pipeline over static loop bounds, LU with partial pivoting is built
 * from microstructures GEMM never touches:
 *   - a per-column data-dependent branch: the pivot search compares every
 *     candidate row's magnitude and picks the maximum (idamax), so every
 *     column of the matrix drives a compare/select decision — a bit flip
 *     in a comparator, a flag register, or a mispredicted branch recovery
 *     path lands directly in the permutation;
 *   - a row-swap store pattern: the chosen rows are exchanged with strided
 *     gather/scatter stores before elimination, a store addressing pattern
 *     disjoint from GEMM's blocked streaming stores;
 *   - triangular back-substitution: sequential dependence along the
 *     diagonal (column k's elimination feeds column k+1's operands), so a
 *     corrupted intermediate propagates forward instead of being masked.
 * The pivot sequence is compared as a first-class result: a wrong pivot
 * choice is direct evidence of a branch/compare-path SDC, visible in the
 * ipiv permutation array before it corrupts any floating-point factor.
 * Each iteration copies the inputs, runs LAPACKE_dgesv, and byte-compares
 * three arrays against golden values from init (copy->compute->verify, the
 * most frequent CORE179 trigger structure): the packed L\U factors (n^2
 * doubles), the ipiv pivot sequence (n ints), and the solution x
 * (n doubles, nrhs=1). All three are deterministic: the same input bits
 * through the same binary always produce the same pivot choices, so a
 * byte-exact memcmp is valid.
 * The matrix dimension is runtime-configurable through the unified
 * stress-config chain (priority order): "-O openblas_lu.n=N" or a
 * config-file entry (16..16384; out-of-range values warn and clamp
 * instead of failing; the test-id prefix is required — a bare "n=N"
 * is silently ignored), the SDC_OPENBLAS_LU_N environment variable,
 * or a memory-adaptive default when neither is set (4 double matrices
 * per thread — the global a/lu and per-thread ac buffers are n^2 each
 * and b/ipiv/x/ipivc/xc are O(n) — at a 6.0x (eigen-GEMM) RSS ratio and 85% of
 * physical memory, clamped to 128..8192; explicit knob range 16..16384). The knob sweeps the
 * working set across L1D (64KB) / L2 (512KB) / LLC / DRAM on TSV110
 * (64: 32KB, 256: 512KB, 1024: 8MB, 2048: 32MB per matrix) at an O(n^3)
 * flop budget per iteration (2048^3 = 8.6e9 flops, matching the mdim 2048
 * GEMM class).
 * Note on the ROW_MAJOR layout: LAPACKE_dgesv internally transposes to
 * column-major, calls LAPACK_dgesv, and transposes back, allocating and
 * freeing its transposition work buffers with plain malloc/free inside
 * each call (lapacke_dgesv_work.c). Those buffers are call-local, and the
 * underlying OpenBLAS build is USE_LOCKING=1 (see tests/cpu/meson.build),
 * so concurrent calls from the framework's one-worker-per-core threads are
 * safe — the same reasoning that makes concurrent cblas_dgemm safe. The
 * buffers this test itself writes (ac/ipivc/xc) are thread-local.
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"

#include <lapacke.h>

#include <string.h>
#include <stdlib.h>

namespace {
struct lu_test_data {
    int n;               /* matrix dimension, from the unified config chain (-O
                            openblas_lu.n / config file / SDC_OPENBLAS_LU_N /
                            memory-adaptive default; see openblas_lu_n) */
    double *a;           /* original matrix, n^2, read-only (input to the dgesv copy) */
    double *lu;          /* golden packed L\U factors (dgesv overwrites its A arg in-place) */
    int *ipiv;           /* golden pivot sequence, n ints */
    double *b;           /* right-hand side, n doubles (nrhs=1) */
    double *x;           /* golden solution (dgesv overwrites its B arg in-place) */
};

/* Scratch buffers are per-thread: the framework runs one worker thread per
 * core and they all share test->data, so anything written during test_run
 * must be thread-local (ac is the copy-in matrix that dgesv factors in
 * place, ipivc receives the pivot sequence, xc is the right-hand side that
 * dgesv overwrites with the solution; all three are written every
 * iteration). They are allocated lazily on first use and intentionally not
 * freed in cleanup: the worker threads are gone (or never existed) when the
 * main thread runs test_cleanup, and the whole forked child exits right
 * after, so the OS reclaims the storage. */
static thread_local double *ac = nullptr;
static thread_local int *ipivc = nullptr;
static thread_local double *xc = nullptr;
}

#define CAST(_x) static_cast<struct lu_test_data *>(_x)

/* Map a random uint64 onto a finite, well-normalised double with a random
 * sign and a random 52-bit mantissa, magnitude in [1e-6, 2e-3]. See the
 * init comment for why the raw bit pattern cannot be used directly. */
static double random_bounded(void) {
    uint64_t r = random64();
    double frac = (double)(r >> 11) / 9007199254740992.0; /* [0, 1) */
    double sign = (r & 1) ? -1.0 : 1.0;
    return sign * (frac * 2.0e-3 + 1.0e-6);
}

/* 方阵维度 n — 统一配置链 (旧式 get_testspecific_knob_value_int + 越界
 * 致命 report_fail_msg 的迁移版, knob 名不变):
 *   1. -O openblas_lu.n=N / 配置文件同名条目 (16..16384, 越界告警并
 *      clamp — 旧版致命失败改为非致命)
 *   2. 环境变量 SDC_OPENBLAS_LU_N
 *   3. 自适应默认: 全局 a/lu (n²) + b/ipiv/x (O(n)) + 每线程 ac (n²) +
 *      ipivc/xc (O(n)) ≈ 4 个 double 方阵/线程, RSS 峰值 ≈ 矩阵体积
 *      6.0 倍(eigen 同模型), 合计占物理内存 85%, clamp [128, 8192] (显式配置下限
 *      16 兼容旧范围, 自适应下限 128 免得默认工作集缩进 L1)。 */
static int openblas_lu_n(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "n", 0, 16, 16384);
    if (v == 0)
        sdc_env_knob_int_clamped("openblas_lu", "n", &v, 16, 16384);
    if (v > 0)
        return (int)v;
    return sdc_auto_square_dim(8, 4, 6.0, 850, thread_count(), 128, 8192, test->id, "n");
}

static int openblas_lu_init(struct test *test) {
    auto d = new(lu_test_data);
    test->data = d;
    d->n = openblas_lu_n(test);
    size_t n2 = (size_t)d->n * (size_t)d->n;
    d->a    = (double *)malloc(n2 * sizeof(double));
    d->b    = (double *)malloc((size_t)d->n * sizeof(double));
    d->lu   = (double *)malloc(n2 * sizeof(double));
    d->ipiv = (int *)malloc((size_t)d->n * sizeof(int));
    d->x    = (double *)malloc((size_t)d->n * sizeof(double));
    if (!d->a || !d->b || !d->lu || !d->ipiv || !d->x) {
        report_fail_msg("OOM allocating %zu bytes", n2 * sizeof(double) * 3);
    }
    /* high-entropy random operands (framework RNG; libc rand is trapped).
     * A raw uint64 reinterpreted as double is a NaN/Inf exponent field about
     * a quarter of the time and NaN would poison the byte-exact comparison,
     * so the random bits are folded into a bounded magnitude instead: the
     * top 52 bits become the mantissa fraction of a value in [~1e-6, ~2e-3]
     * with a random sign. The full 52-bit mantissa stays random (the SDC
     * payload). A dense random matrix with independent continuous entries
     * is nonsingular with probability 1 (singularity is a measure-zero
     * event in the space of matrices); the [1e-6, 2e-3] magnitude band
     * additionally keeps every elimination intermediate far from overflow
     * and subnormal rounding. So a singular draw is not expected — if
     * dgesv reports one, fail loudly and ask for a rerun rather than
     * silently retrying (a hidden retry loop would re-roll the RNG and
     * mask exactly the nondeterminism this test hunts). */
    for (size_t i = 0; i < n2; ++i)
        d->a[i] = random_bounded();
    for (int i = 0; i < d->n; ++i)
        d->b[i] = random_bounded();
    /* golden run: dgesv overwrites its A and B arguments in place, so copy
     * the operands into the golden buffers and factor the copies — after
     * the call d->lu holds the packed L\U factors and d->x the solution.
     * Leading dimensions in row-major layout are ROW STRIDES: A is n×n
     * stored compactly so lda = n, while B is a compact n×1 vector so its
     * row stride is ldb = 1 (= nrhs, the LAPACKE minimum). Passing ldb = n
     * instead would make LAPACKE's transpose/NaN-check helpers stride n
     * doubles between consecutive rows of B and read/write far past the
     * n-element buffer (empirically: SEGV in LAPACKE_dge_nancheck). */
    memcpy(d->lu, d->a, n2 * sizeof(double));
    memcpy(d->x, d->b, (size_t)d->n * sizeof(double));
    int rc = LAPACKE_dgesv(LAPACK_ROW_MAJOR, d->n, 1, d->lu, d->n, d->ipiv,
                           d->x, 1);
    if (rc != 0) {
        report_fail_msg("LAPACKE_dgesv failed with info %d (random operand set singular — rerun; probability ~0)",
                        rc);
    }
    /* reject a NaN/Inf-polluted golden at the source: if the random operands
     * produced a non-finite solution the byte-exact comparison below would be
     * meaningless (NaN != NaN), so fail loudly instead of silently passing.
     * The bound is a loose tripwire (1e30), not a tight envelope: x's
     * components are bounded by ~cond(A)·|b|, and for these operands that is
     * nowhere near 1e30 — anything past it means the solve already went
     * catastrophically wrong, which is precisely what should fail. */
    for (int i = 0; i < d->n; ++i) {
        double g = d->x[i];
        if (g != g || g > 1.0e30 || g < -1.0e30) {
            report_fail_msg("golden solution not finite at element %d "
                            "(random operands need tighter scaling)", i);
        }
    }
    return EXIT_SUCCESS;
}

static int openblas_lu_run(struct test *test, int cpu) {
    auto d = CAST(test->data);
    size_t bytes = (size_t)d->n * d->n * sizeof(double);
    size_t n2 = (size_t)d->n * d->n;
    long iter = 0;
    TEST_LOOP(test, 1) {
        /* lazily allocate this thread's scratch buffers */
        if (__builtin_expect(!ac || !ipivc || !xc, 0)) {
            ac    = (double *)malloc(bytes);
            ipivc = (int *)malloc((size_t)d->n * sizeof(int));
            xc    = (double *)malloc((size_t)d->n * sizeof(double));
            if (!ac || !ipivc || !xc)
                report_fail_msg("OOM allocating thread scratch (%zu bytes)",
                                bytes + (size_t)d->n * (sizeof(int) + sizeof(double)));
        }

        /* copy-in (dirties cache lines, then reloads them in the kernel) */
        memcpy(ac, d->a, bytes);
        memcpy(xc, d->b, (size_t)d->n * sizeof(double));

        int rc = LAPACKE_dgesv(LAPACK_ROW_MAJOR, d->n, 1, ac, d->n, ipivc,
                               xc, 1);
        ++iter;
        if (rc != 0) {
            report_fail_msg("LAPACKE_dgesv failed with info %d (iteration %ld; "
                            "golden run succeeded — rerun)", rc, iter);
        }

        /* verify-out: three byte-exact golden compares. Order matters: the
         * factors first (the bulk payload), then the pivot sequence (the
         * branch/compare-path evidence — a wrong pivot choice shows up here
         * even when the floating-point arithmetic itself was flawless), then
         * the solution (the back-substitution result). */
        memcmp_or_fail(ac, d->lu, n2, "LU factors");
        memcmp_or_fail(ipivc, d->ipiv, (size_t)d->n, "pivot sequence");
        memcmp_or_fail(xc, d->x, (size_t)d->n, "solution");
    }
    return EXIT_SUCCESS;
}

static int openblas_lu_cleanup(struct test *test) {
    auto d = CAST(test->data);
    free(d->a); free(d->lu); free(d->ipiv); free(d->b); free(d->x);
    delete d;
    return EXIT_SUCCESS;
}

DECLARE_TEST(openblas_lu, "OpenBLAS LAPACKE dgesv (LU partial pivoting: pivot-branch/row-swap/back-substitution, copy/compute/verify per iteration)")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = openblas_lu_init,
  .test_run = openblas_lu_run,
  .test_cleanup = openblas_lu_cleanup,
  .fracture_loop_count = 4,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
