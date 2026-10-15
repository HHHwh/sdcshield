/**
 * @file
 *
 * @copyright
 * Copyright 2026 ISCAS.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b openblas_zgemm
 * @parblock
 * Stress test for the vector FMA units using OpenBLAS's hand-written NEON
 * zgemm micro-kernel (TSV110). Literature identifies vector FMA as the #1
 * SDC source (SEVI ASPLOS'26: >92% of vector SDC incidents; Veritas HPCA'25:
 * vector units orders of magnitude above scalar). Each iteration copies the
 * inputs, computes C = A*B, and byte-compares against the golden product
 * from init (copy->compute->verify, the most frequent CORE179 trigger
 * structure). Complex double: the kernel decomposes each complex GEMM into
 * four real GEMMs (CORE179 §3.7: cdouble is the lowest-rate but
 * confirmed-triggering eigen path; this widens complex-path coverage with a
 * different scheduling sample).
 * The matrix dimension is runtime-configurable through the unified
 * stress-config chain (priority order): "-O openblas_zgemm.mdim=N" or
 * a config-file entry (16..16384; out-of-range values warn and clamp
 * instead of failing; the test-id prefix is required — a bare
 * "mdim=N" is silently ignored), the SDC_OPENBLAS_ZGEMM_MDIM
 * environment variable, or a memory-adaptive default when neither is
 * set (6 complex-double matrices per thread — 3 shared (a/b/golden) +
 * 3 per-thread scratch (a_copy/b_copy/c), 16 bytes per element — at a
 * 6.0x RSS ratio (eigen-GEMM model) and 85% of physical memory, clamped to 128..8192).
 * It sweeps the working set across L1D (mdim=64: 64KB) / L2 (256:
 * 1MB) / LLC (512: 4MB) / DRAM (1024: 16MB, 2048: 64MB, 4096: 256MB)
 * per complex-double matrix — the CORE179 probes showed the
 * store->reload cache-domain pattern is a triggering discriminator,
 * so each size class exercises different forwarding paths.
 * Two further runtime knobs (same test-id prefix rule) sweep orthogonal
 * axes of the kernel beside the working-set size: "-O
 * openblas_zgemm.transab=0..3" selects the TransA/TransB quadrant
 * (bit1=TransA, bit0=TransB: 0=NN default, 1=NT, 2=TN, 3=TT) —
 * transposed operands walk OpenBLAS's different internal packing
 * routines, i.e. different instruction-scheduling phases; and "-O
 * openblas_zgemm.beta_permille=0..1000000" sets beta=permille/1000
 * (default 0 = historical pure store into C; 1000 = beta 1.0), which
 * exercises the kernel's C read-modify-write path (C = alpha*op(A)*op(B)
 * + beta*C reads C instead of only writing it). With beta != 0 the
 * initial content of C participates in the result, so init seeds the
 * golden buffer and every run iteration seeds the thread-local C with
 * the same deterministic non-zero pattern (0.5*(i&63)/64.0, magnitude
 * <= ~0.49, filling the interleaved (re, im) buffer alike) before the
 * cblas call; the golden finiteness sentinel below still holds since
 * |C| <= ~1.7e-2 + beta*0.5 (≈ 0.52 at the beta ≤ 1 operating point,
 * ≈ 500 across the full beta_permille=1000000 range — still hundreds of
 * orders of magnitude below the 1e300 cutoff) per component.
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"

#include <cblas.h>

#include <string.h>
#include <stdlib.h>

namespace {
struct zgemm_test_data {
    int mdim;            /* matrix dimension, from the unified config chain (-O
                            openblas_zgemm.mdim / config file /
                            SDC_OPENBLAS_ZGEMM_MDIM / memory-adaptive default;
                            see openblas_zgemm_mdim) */
    int transab;         /* TransA/TransB quadrant from -O openblas_zgemm.transab=0..3
                            (bit1=TransA, bit0=TransB: 0=NN 1=NT 2=TN 3=TT) */
    double beta;         /* real part of the beta scalar, = beta_permille/1000 from
                            -O openblas_zgemm.beta_permille (0.0 = historical pure
                            store into C; != 0 exercises the C read-modify-write path);
                            passed to cblas as the complex pair {beta, 0.0} */
    double *a;           /* interleaved (re, im) pairs: n2 doubles, n2 = 2*mdim*mdim */
    double *b;
    double *golden;      /* C = alpha*op(A)*op(B) + beta*C computed once in init; read-only after */
};

/* Scratch buffers are per-thread: the framework runs one worker thread per
 * core and they all share test->data, so anything written during test_run
 * must be thread-local (a_copy/b_copy are the copy-in destinations, c is
 * the product destination; all three are written every iteration). They are
 * allocated lazily on first use and intentionally not freed in cleanup:
 * the worker threads are gone (or never existed) when the main thread runs
 * test_cleanup, and the whole forked child exits right after, so the OS
 * reclaims the storage. calloc zeroes c once so a skipped/short first
 * iteration can never compare garbage. */
static thread_local double *a_copy = nullptr;
static thread_local double *b_copy = nullptr;
static thread_local double *c = nullptr;
}

#define CAST(_x) static_cast<struct zgemm_test_data *>(_x)

/* cblas_zgemm takes the scalars as const void* pointing at an interleaved
 * (re, im) double pair; cblas.h's openblas_complex_double is a C99
 * double _Complex in C++ too, so address the constants through the
 * OpenBLAS-free struct layout (double[2]) to keep it plain. alpha stays
 * the literal ONE; beta is knob-controlled, so it is composed as a local
 * {beta, 0.0} pair from the parsed struct value at each call site. */
static const double ONE[2]  = {1.0, 0.0};

/* Map a random uint64 onto a finite, well-normalised double with a random
 * sign and a random 52-bit mantissa, magnitude in [1e-6, 2e-3]. Same
 * construction and rationale as openblas_dgemm's random_bounded: a raw
 * uint64 reinterpreted as double is a NaN/Inf exponent field about a
 * quarter of the time and NaN would poison the byte-exact comparison. */
static double random_bounded(void) {
    uint64_t r = random64();
    double frac = (double)(r >> 11) / 9007199254740992.0; /* [0, 1) */
    double sign = (r & 1) ? -1.0 : 1.0;
    return sign * (frac * 2.0e-3 + 1.0e-6);
}

/* 方阵维度 mdim — 统一配置链 (旧式 get_testspecific_knob_value_int + 越界
 * 致命 report_fail_msg 的迁移版, knob 名不变):
 *   1. -O openblas_zgemm.mdim=N / 配置文件同名条目 (16..16384, 越界
 *      告警并 clamp — 旧版致命失败改为非致命)
 *   2. 环境变量 SDC_OPENBLAS_ZGEMM_MDIM
 *   3. 自适应默认: 全局 a/b/golden 3 个 + 每线程 a_copy/b_copy/c 3 个
 *      = 6 个 complex double 方阵/线程 (每元素 16B, 即 2*mdim^2 个
 *      double), RSS 峰值 ≈ 矩阵体积 6.0 倍(与 eigen GEMM 同模型),
 *      合计占物理内存 85%, clamp [128, 8192] (显式配置下限 16 兼容旧范围,
 *      自适应下限 128 免得默认工作集缩进 L1)。 */
static int openblas_zgemm_mdim(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "mdim", 0, 16, 16384);
    if (v == 0)
        sdc_env_knob_int_clamped("openblas_zgemm", "mdim", &v, 16, 16384);
    if (v > 0)
        return (int)v;
    return sdc_auto_square_dim(16, 6, 6.0, 850, thread_count(), 128, 8192, test->id, "mdim");
}

static int openblas_zgemm_init(struct test *test) {
    auto d = new(zgemm_test_data);
    test->data = d;
    d->mdim = openblas_zgemm_mdim(test);
    /* transab / beta_permille: same knob namespace, migrated to the clamped
     * reader — out-of-range values now warn and clamp instead of failing
     * fatally (defaults unchanged: 0 = NN, beta 0.0) */
    int64_t transab = sdc_knob_int_clamped(test, "transab", 0, 0, 3);
    int64_t beta_pm = sdc_knob_int_clamped(test, "beta_permille", 0, 0, 1000000);
    d->transab = (int)transab;
    d->beta    = (double)beta_pm / 1000.0;
    size_t n2 = 2 * (size_t)d->mdim * (size_t)d->mdim;   /* interleaved (re, im) double pairs */
    d->a      = (double *)malloc(n2 * sizeof(double));
    d->b      = (double *)malloc(n2 * sizeof(double));
    d->golden = (double *)malloc(n2 * sizeof(double));
    if (!d->a || !d->b || !d->golden) {
        report_fail_msg("OOM allocating %zu bytes", n2 * sizeof(double) * 3);
    }
    /* high-entropy random operands (framework RNG; libc rand is trapped).
     * Bounded magnitude as in openblas_dgemm: real and imaginary parts are
     * filled independently from random_bounded, so the K=mdim complex dot
     * products are bounded by K * (2e-3)^2 <= ~1.7e-2 per component
     * (K <= 4096) — forty orders of magnitude from overflow and far from
     * subnormal rounding, while the full 52-bit mantissas stay random (the
     * SDC payload). */
    for (size_t i = 0; i < n2; ++i) {
        d->a[i] = random_bounded();
        d->b[i] = random_bounded();
    }
    /* beta != 0 makes C's initial content part of the result
     * (C = alpha*op(A)*op(B) + beta*C), so seed the golden buffer with a
     * deterministic non-zero pattern before the reference call — test_run
     * seeds its thread-local C with the very same pattern before every
     * cblas call, keeping the byte-exact comparison valid (with the
     * default beta=0 the seed is fully overwritten, so the historical
     * behavior stays byte-identical). The pattern fills the interleaved
     * (re, im) buffer element-wise, i.e. real and imaginary parts alike. */
    for (size_t i = 0; i < n2; ++i)
        d->golden[i] = 0.5 * (double)(i & 63) / 64.0;
    const double beta_scalar[2] = {d->beta, 0.0};
    cblas_zgemm(CblasRowMajor,
                (d->transab & 2) ? CblasTrans : CblasNoTrans,
                (d->transab & 1) ? CblasTrans : CblasNoTrans,
                d->mdim, d->mdim, d->mdim,
                ONE, d->a, d->mdim, d->b, d->mdim,
                beta_scalar, d->golden, d->mdim);
    /* reject a NaN/Inf-polluted golden at the source: if the random operands
     * produced a non-finite product the byte-exact comparison below would be
     * meaningless (NaN != NaN), so fail loudly instead of silently passing.
     * The buffer is n2 doubles and n2 already includes the x2 for the
     * interleaved (re, im) components of every element. */
    for (size_t i = 0; i < n2; ++i) {
        double g = d->golden[i];
        if (g != g || g > 1.0e300 || g < -1.0e300) {
            report_fail_msg("golden product not finite at element %zu "
                            "(random operands need tighter scaling)", i);
        }
    }
    return EXIT_SUCCESS;
}

static int openblas_zgemm_run(struct test *test, int cpu) {
    auto d = CAST(test->data);
    size_t bytes = 2 * (size_t)d->mdim * d->mdim * sizeof(double);
    size_t n2 = 2 * (size_t)d->mdim * d->mdim;   /* interleaved (re, im) double pairs */
    long iter = 0;
    TEST_LOOP(test, 1) {
        /* lazily allocate this thread's scratch buffers */
        if (__builtin_expect(!a_copy || !b_copy || !c, 0)) {
            a_copy = (double *)malloc(bytes);
            b_copy = (double *)malloc(bytes);
            c      = (double *)calloc(1, bytes);
            if (!a_copy || !b_copy || !c)
                report_fail_msg("OOM allocating thread scratch (%zu bytes)", bytes * 3);
        }

        /* copy-in (dirties cache lines, then reloads them in the kernel) */
        memcpy(a_copy, d->a, bytes);
        memcpy(b_copy, d->b, bytes);

        /* re-seed C's initial content every iteration: with beta != 0 the
         * kernel reads C (read-modify-write), so each iteration must start
         * from the same deterministic pattern the golden product was seeded
         * with in init (recomputed inline — no extra shared array; the
         * interleaved (re, im) buffer is filled element-wise, real and
         * imaginary parts alike) */
        for (size_t i = 0; i < n2; ++i)
            c[i] = 0.5 * (double)(i & 63) / 64.0;

        const double beta_scalar[2] = {d->beta, 0.0};
        cblas_zgemm(CblasRowMajor,
                    (d->transab & 2) ? CblasTrans : CblasNoTrans,
                    (d->transab & 1) ? CblasTrans : CblasNoTrans,
                    d->mdim, d->mdim, d->mdim,
                    ONE, a_copy, d->mdim, b_copy, d->mdim,
                    beta_scalar, c, d->mdim);

        ++iter;
        /* verify-out: byte-exact golden compare of inputs and product */
        if (memcmp(a_copy, d->a, bytes) != 0) {
            report_fail_msg("input A corrupted after GEMM (iteration %ld)", iter);
        }
        if (memcmp(b_copy, d->b, bytes) != 0) {
            report_fail_msg("input B corrupted after GEMM (iteration %ld)", iter);
        }
        memcmp_or_fail(c, d->golden, 2 * (size_t)d->mdim * d->mdim);
    }
    return EXIT_SUCCESS;
}

static int openblas_zgemm_cleanup(struct test *test) {
    auto d = CAST(test->data);
    free(d->a); free(d->b); free(d->golden);
    delete d;
    return EXIT_SUCCESS;
}

DECLARE_TEST(openblas_zgemm, "OpenBLAS ZGEMM (complex double NEON FMA, copy/compute/verify per iteration)")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = openblas_zgemm_init,
  .test_run = openblas_zgemm_run,
  .test_cleanup = openblas_zgemm_cleanup,
  .fracture_loop_count = 4,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
