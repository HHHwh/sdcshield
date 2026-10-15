/**
 * @file
 *
 * @copyright
 * Copyright 2026 ISCAS.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b openblas_sgemm
 * @parblock
 * Stress test for the vector FMA units using OpenBLAS's hand-written NEON
 * sgemm micro-kernel (TSV110). Literature identifies vector FMA as the #1
 * SDC source (SEVI ASPLOS'26: >92% of vector SDC incidents; Veritas HPCA'25:
 * vector units orders of magnitude above scalar). Each iteration copies the
 * inputs, computes C = A*B, and byte-compares against the golden product
 * from init (copy->compute->verify, the most frequent CORE179 trigger
 * structure). Single precision: the narrower lanes double the per-line
 * vector throughput of the fmla pipeline, and float and double exercise
 * different corner bits of the multiply-add datapath (SEVI: single-lane
 * failures dominate). A scheduling sample beside dgemm/Eigen/ACL GEMM.
 * The matrix dimension is runtime-configurable through the unified
 * stress-config chain (priority order): "-O openblas_sgemm.mdim=N" or
 * a config-file entry (16..16384; out-of-range values warn and clamp
 * instead of failing; the test-id prefix is required — a bare
 * "mdim=N" is silently ignored), the SDC_OPENBLAS_SGEMM_MDIM
 * environment variable, or a memory-adaptive default when neither is
 * set (6 float matrices per thread — 3 shared (a/b/golden) + 3
 * per-thread scratch (a_copy/b_copy/c) — at a 6.0x (eigen-GEMM) RSS ratio and 85%
 * of physical memory, clamped to 128..8192; explicit knob range 16..16384). It sweeps the working
 * set across L1D (mdim=64: 16KB) / L2 (256: 256KB) / LLC (512: 1MB) /
 * DRAM (1024: 4MB, 2048: 16MB, 4096: 64MB) — the CORE179 probes
 * showed the store->reload cache-domain pattern is a triggering
 * discriminator, so each size class exercises different forwarding
 * paths.
 * Two further runtime knobs (same test-id prefix rule) sweep orthogonal
 * axes of the kernel beside the working-set size: "-O
 * openblas_sgemm.transab=0..3" selects the TransA/TransB quadrant
 * (bit1=TransA, bit0=TransB: 0=NN default, 1=NT, 2=TN, 3=TT) —
 * transposed operands walk OpenBLAS's different internal packing
 * routines, i.e. different instruction-scheduling phases; and "-O
 * openblas_sgemm.beta_permille=0..1000000" sets beta=permille/1000
 * (default 0 = historical pure store into C; 1000 = beta 1.0), which
 * exercises the kernel's C read-modify-write path (C = alpha*op(A)*op(B)
 * + beta*C reads C instead of only writing it). With beta != 0 the
 * initial content of C participates in the result, so init seeds the
 * golden buffer and every run iteration seeds the thread-local C with
 * the same deterministic non-zero pattern (0.5*(i&63)/64.0, magnitude
 * <= ~0.49) before the cblas call; the golden finiteness sentinel below
 * still holds since |C| <= ~1.7e-2 + beta*0.5 (≈ 0.52 at the beta ≤ 1
 * operating point, ≈ 500 across the full beta_permille=1000000 range —
 * still tens of orders of magnitude below the 1e30f cutoff).
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"

#include <cblas.h>

#include <string.h>
#include <stdlib.h>

namespace {
struct sgemm_test_data {
    int mdim;             /* matrix dimension, from the unified config chain (-O
                             openblas_sgemm.mdim / config file /
                             SDC_OPENBLAS_SGEMM_MDIM / memory-adaptive default;
                             see openblas_sgemm_mdim) */
    int transab;          /* TransA/TransB quadrant from -O openblas_sgemm.transab=0..3
                             (bit1=TransA, bit0=TransB: 0=NN 1=NT 2=TN 3=TT) */
    float beta;           /* beta = beta_permille/1000 from -O openblas_sgemm.beta_permille
                             (0.0 = historical pure store into C; != 0 exercises the
                             C read-modify-write path) */
    float *a;
    float *b;
    float *golden;      /* C = alpha*op(A)*op(B) + beta*C computed once in init; read-only after */
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
static thread_local float *a_copy = nullptr;
static thread_local float *b_copy = nullptr;
static thread_local float *c = nullptr;
}

#define CAST(_x) static_cast<struct sgemm_test_data *>(_x)

/* Map a random uint64 onto a finite, well-normalised float with a random
 * sign and a random mantissa fraction, magnitude in [~1e-6, ~2e-3]. Same
 * construction and rationale as openblas_dgemm's random_bounded: a raw
 * uint64 reinterpreted as float would be NaN/Inf about a quarter of the
 * time and NaN would poison the byte-exact comparison. The [0,1) fraction
 * comes from the top 53 bits as a double and narrows to float exactly
 * (round-to-nearest of a [0,1) double stays within [0,1]), so the value
 * stays finite and well-scaled in the float domain. */
static float random_bounded(void) {
    uint64_t r = random64();
    double frac = (double)(r >> 11) / 9007199254740992.0; /* [0, 1) */
    float sign = (r & 1) ? -1.0f : 1.0f;
    return sign * ((float)frac * 2.0e-3f + 1.0e-6f);
}

/* 方阵维度 mdim — 统一配置链 (旧式 get_testspecific_knob_value_int + 越界
 * 致命 report_fail_msg 的迁移版, knob 名不变):
 *   1. -O openblas_sgemm.mdim=N / 配置文件同名条目 (16..16384, 越界
 *      告警并 clamp — 旧版致命失败改为非致命)
 *   2. 环境变量 SDC_OPENBLAS_SGEMM_MDIM
 *   3. 自适应默认: 全局 a/b/golden 3 个 + 每线程 a_copy/b_copy/c 3 个
 *      = 6 个 float 方阵/线程, RSS 峰值 ≈ 矩阵体积 6.0 倍(与 eigen GEMM 同模型), 合计占
 *      物理内存 85%, clamp [128, 8192] (显式配置范围 16..16384 兼容旧范围,
 *      自适应下限 128 免得默认工作集缩进 L1)。 */
static int openblas_sgemm_mdim(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "mdim", 0, 16, 16384);
    if (v == 0)
        sdc_env_knob_int_clamped("openblas_sgemm", "mdim", &v, 16, 16384);
    if (v > 0)
        return (int)v;
    return sdc_auto_square_dim(4, 6, 6.0, 850, thread_count(), 128, 8192, test->id, "mdim");
}

static int openblas_sgemm_init(struct test *test) {
    auto d = new(sgemm_test_data);
    test->data = d;
    d->mdim = openblas_sgemm_mdim(test);
    /* transab / beta_permille: same knob namespace, migrated to the clamped
     * reader — out-of-range values now warn and clamp instead of failing
     * fatally (defaults unchanged: 0 = NN, beta 0.0) */
    int64_t transab = sdc_knob_int_clamped(test, "transab", 0, 0, 3);
    int64_t beta_pm = sdc_knob_int_clamped(test, "beta_permille", 0, 0, 1000000);
    d->transab = (int)transab;
    d->beta    = (float)((double)beta_pm / 1000.0);
    size_t n2 = (size_t)d->mdim * (size_t)d->mdim;
    d->a      = (float *)malloc(n2 * sizeof(float));
    d->b      = (float *)malloc(n2 * sizeof(float));
    d->golden = (float *)malloc(n2 * sizeof(float));
    if (!d->a || !d->b || !d->golden) {
        report_fail_msg("OOM allocating %zu bytes", n2 * sizeof(float) * 3);
    }
    /* high-entropy random operands (framework RNG; libc rand is trapped).
     * Bounded magnitude as in openblas_dgemm: the K=mdim dot products are
     * bounded by K * (2e-3)^2 <= ~1.7e-2 (K <= 4096) — nowhere near float
     * overflow and far from subnormal rounding, while the random mantissa
     * bits remain the SDC payload. */
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
     * behavior stays byte-identical). The pattern is dyadic
     * ((i&63)/128), so narrowing the double computation to float is
     * exact. */
    for (size_t i = 0; i < n2; ++i)
        d->golden[i] = (float)(0.5 * (double)(i & 63) / 64.0);
    cblas_sgemm(CblasRowMajor,
                (d->transab & 2) ? CblasTrans : CblasNoTrans,
                (d->transab & 1) ? CblasTrans : CblasNoTrans,
                d->mdim, d->mdim, d->mdim,
                1.0f, d->a, d->mdim, d->b, d->mdim,
                d->beta, d->golden, d->mdim);
    /* reject a NaN/Inf-polluted golden at the source: if the random operands
     * produced a non-finite product the byte-exact comparison below would be
     * meaningless (NaN != NaN), so fail loudly instead of silently passing */
    for (size_t i = 0; i < n2; ++i) {
        float g = d->golden[i];
        if (g != g || g > 1.0e30f || g < -1.0e30f) {
            report_fail_msg("golden product not finite at element %zu "
                            "(random operands need tighter scaling)", i);
        }
    }
    return EXIT_SUCCESS;
}

static int openblas_sgemm_run(struct test *test, int cpu) {
    auto d = CAST(test->data);
    size_t bytes = (size_t)d->mdim * d->mdim * sizeof(float);
    size_t n2 = (size_t)d->mdim * d->mdim;
    long iter = 0;
    TEST_LOOP(test, 1) {
        /* lazily allocate this thread's scratch buffers */
        if (__builtin_expect(!a_copy || !b_copy || !c, 0)) {
            a_copy = (float *)malloc(bytes);
            b_copy = (float *)malloc(bytes);
            c      = (float *)calloc(1, bytes);
            if (!a_copy || !b_copy || !c)
                report_fail_msg("OOM allocating thread scratch (%zu bytes)", bytes * 3);
        }

        /* copy-in (dirties cache lines, then reloads them in the kernel) */
        memcpy(a_copy, d->a, bytes);
        memcpy(b_copy, d->b, bytes);

        /* re-seed C's initial content every iteration: with beta != 0 the
         * kernel reads C (read-modify-write), so each iteration must start
         * from the same deterministic pattern the golden product was seeded
         * with in init (recomputed inline — no extra shared array) */
        for (size_t i = 0; i < n2; ++i)
            c[i] = (float)(0.5 * (double)(i & 63) / 64.0);

        cblas_sgemm(CblasRowMajor,
                    (d->transab & 2) ? CblasTrans : CblasNoTrans,
                    (d->transab & 1) ? CblasTrans : CblasNoTrans,
                    d->mdim, d->mdim, d->mdim,
                    1.0f, a_copy, d->mdim, b_copy, d->mdim,
                    d->beta, c, d->mdim);

        ++iter;
        /* verify-out: byte-exact golden compare of inputs and product */
        if (memcmp(a_copy, d->a, bytes) != 0) {
            report_fail_msg("input A corrupted after GEMM (iteration %ld)", iter);
        }
        if (memcmp(b_copy, d->b, bytes) != 0) {
            report_fail_msg("input B corrupted after GEMM (iteration %ld)", iter);
        }
        memcmp_or_fail(c, d->golden, (size_t)d->mdim * d->mdim);
    }
    return EXIT_SUCCESS;
}

static int openblas_sgemm_cleanup(struct test *test) {
    auto d = CAST(test->data);
    free(d->a); free(d->b); free(d->golden);
    delete d;
    return EXIT_SUCCESS;
}

DECLARE_TEST(openblas_sgemm, "OpenBLAS SGEMM (NEON FMA micro-kernel, single precision, copy/compute/verify per iteration)")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = openblas_sgemm_init,
  .test_run = openblas_sgemm_run,
  .test_cleanup = openblas_sgemm_cleanup,
  .fracture_loop_count = 4,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
