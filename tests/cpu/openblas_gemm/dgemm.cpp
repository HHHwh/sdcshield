/**
 * @file
 *
 * @copyright
 * Copyright 2026 ISCAS.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b openblas_dgemm
 * @parblock
 * Stress test for the vector FMA units using OpenBLAS's hand-written NEON
 * dgemm micro-kernel (TSV110). Literature identifies vector FMA as the #1
 * SEVI ASPLOS'26: >92% of vector SDC incidents; Veritas HPCA'25:
 * vector units orders of magnitude above scalar). Each iteration copies the
 * inputs, computes C = A*B, and byte-compares against a golden product
 * computed from freshly re-rolled operands every kRerollEvery iterations
 * (per-thread, framework RNG — B2-004; init's copy stays only as the
 * init-time finiteness canary): copy->compute->verify, the most frequent
 * CORE179 trigger structure. A third scheduling sample beside Eigen/ACL
 * GEMM.
 * The matrix dimension is runtime-configurable via the test knob
 * "-O openblas_dgemm.mdim=N" (16..4096, default 256; the test-id
 * prefix is required — a bare "mdim=N" is silently ignored), sweeping
 * the working set across L1D (64KB) / L2 (512KB) / LLC / DRAM /
 * remote-NUMA on TSV110 (64: 32KB ... 1024: 8MB, 2048: 32MB, 4096:
 * 128MB per matrix) — the CORE179 probes showed the store->reload
 * cache-domain pattern is a triggering discriminator, so each size
 * class exercises different forwarding paths.
 * Two further runtime knobs (same test-id prefix rule) sweep orthogonal
 * axes of the kernel beside the working-set size: "-O
 * openblas_dgemm.transab=0..3" selects the TransA/TransB quadrant
 * (bit1=TransA, bit0=TransB: 0=NN default, 1=NT, 2=TN, 3=TT) —
 * transposed operands walk OpenBLAS's different internal packing
 * routines, i.e. different instruction-scheduling phases; and "-O
 * openblas_dgemm.beta_permille=0..1000000" sets beta=permille/1000
 * (default 0 = historical pure store into C; 1000 = beta 1.0), which
 * exercises the kernel's C read-modify-write path (C = alpha*op(A)*op(B)
 * + beta*C reads C instead of only writing it). With beta != 0 the
 * initial content of C participates in the result, so init seeds the
 * golden buffer and every run iteration seeds the thread-local C with
 * the same deterministic non-zero pattern (0.5*(i&63)/64.0, magnitude
 * <= ~0.49) before the cblas call; the golden finiteness sentinel below
 * still holds since |C| <= ~1.7e-2 + beta*0.5 (≈ 0.52 at the beta ≤ 1
 * operating point, ≈ 500 across the full beta_permille=1000000 range —
 * still hundreds of orders of magnitude below the 1e300 cutoff).
 * @endparblock
 */

#include <sandstone.h>

#include <cblas.h>

#include <string.h>
#include <stdlib.h>

namespace {
struct gemm_test_data {
    int mdim;             /* matrix dimension, from the -O openblas_dgemm.mdim=N knob */
    int transab;          /* TransA/TransB quadrant from -O openblas_dgemm.transab=0..3
                             (bit1=TransA, bit0=TransB: 0=NN 1=NT 2=TN 3=TT) */
    double beta;          /* beta = beta_permille/1000 from -O openblas_dgemm.beta_permille
                             (0.0 = historical pure store into C; != 0 exercises the
                             C read-modify-write path) */
    double *a;
    double *b;
    double *golden;      /* C = alpha*op(A)*op(B) + beta*C computed once in init as the finiteness
                             canary for the operand distribution; not read by test_run (B2-004: run
                             keeps its own per-thread re-rolled l_a/l_b/l_golden) */
};

/* Scratch buffers are per-thread: the framework runs one worker thread per
 * core and they all share test->data, so anything written during test_run
 * must be thread-local (a_copy/b_copy are the copy-in destinations, c is
 * the product destination; all three are written every iteration). l_a/
 * l_b/l_golden are this thread's re-rolled operands and golden (B2-004):
 * the shared d->a/b/golden are never read by test_run — init's fill stays
 * only as the init-time finiteness canary, matching the 46141ed4 Eigen
 * pattern where run owns its inputs outright. All are allocated lazily
 * on first use and intentionally not
 * freed in cleanup: the worker threads are gone (or never existed) when
 * the main thread runs test_cleanup, and the whole forked child exits
 * right after, so the OS reclaims the storage. calloc zeroes c once so a
 * skipped/short first iteration can never compare garbage. */
static thread_local double *a_copy = nullptr;
static thread_local double *b_copy = nullptr;
static thread_local double *c = nullptr;
static thread_local double *l_a = nullptr;
static thread_local double *l_b = nullptr;
static thread_local double *l_golden = nullptr;
}

#define CAST(_x) static_cast<struct gemm_test_data *>(_x)

/* Map a random uint64 onto a finite, well-normalised double with a random
 * sign and a random 52-bit mantissa, magnitude in [1e-6, 2e-3]. See the
 * init comment for why the raw bit pattern cannot be used directly. */
static double random_bounded(void) {
    uint64_t r = random64();
    double frac = (double)(r >> 11) / 9007199254740992.0; /* [0, 1) */
    double sign = (r & 1) ? -1.0 : 1.0;
    return sign * (frac * 2.0e-3 + 1.0e-6);
}

static int openblas_dgemm_init(struct test *test) {
    auto d = new(gemm_test_data);
    test->data = d;
    int64_t knob = get_testspecific_knob_value_int(test, "mdim", 256);
    if (knob < 16 || knob > 4096) {
        report_fail_msg("mdim knob out of range: %ld (valid 16..4096, default 256)", (long)knob);
    }
    d->mdim = (int)knob;
    int64_t transab = get_testspecific_knob_value_int(test, "transab", 0);
    if (transab < 0 || transab > 3) {
        report_fail_msg("transab knob out of range: %ld (valid 0..3: 0=NN 1=NT 2=TN 3=TT, default 0)", (long)transab);
    }
    int64_t beta_pm = get_testspecific_knob_value_int(test, "beta_permille", 0);
    if (beta_pm < 0 || beta_pm > 1000000) {
        report_fail_msg("beta_permille knob out of range: %ld (valid 0..1000000, default 0; beta=permille/1000)", (long)beta_pm);
    }
    d->transab = (int)transab;
    d->beta    = (double)beta_pm / 1000.0;
    size_t n2 = (size_t)d->mdim * (size_t)d->mdim;
    d->a      = (double *)malloc(n2 * sizeof(double));
    d->b      = (double *)malloc(n2 * sizeof(double));
    d->golden = (double *)malloc(n2 * sizeof(double));
    if (!d->a || !d->b || !d->golden) {
        report_fail_msg("OOM allocating %zu bytes", n2 * sizeof(double) * 3);
    }
    /* high-entropy random operands (framework RNG; libc rand is trapped).
     * A raw uint64 reinterpreted as double is a NaN/Inf exponent field about
     * a quarter of the time and NaN would poison the byte-exact comparison,
     * and plain division (x / 1.0e30) does not fix it (NaN propagates; the
     * remainder collapses toward subnormals). Instead the random bits are
     * folded into a bounded magnitude: the top 52 bits become the mantissa
     * fraction of a value in [~1e-6, ~2e-3] with a random sign. The full
     * 52-bit mantissa stays random (the SDC payload), while the K=mdim
     * dot products are bounded by K * (2e-3)^2 <= ~1.7e-2 (K <= 4096) —
     * forty orders of magnitude from overflow and far from subnormal
     * rounding. */
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
     * behavior stays byte-identical). */
    for (size_t i = 0; i < n2; ++i)
        d->golden[i] = 0.5 * (double)(i & 63) / 64.0;
    cblas_dgemm(CblasRowMajor,
                (d->transab & 2) ? CblasTrans : CblasNoTrans,
                (d->transab & 1) ? CblasTrans : CblasNoTrans,
                d->mdim, d->mdim, d->mdim,
                1.0, d->a, d->mdim, d->b, d->mdim,
                d->beta, d->golden, d->mdim);
    /* reject a NaN/Inf-polluted golden at the source: if the random operands
     * produced a non-finite product the byte-exact comparison below would be
     * meaningless (NaN != NaN), so fail loudly instead of silently passing */
    for (size_t i = 0; i < n2; ++i) {
        double g = d->golden[i];
        if (g != g || g > 1.0e300 || g < -1.0e300) {
            report_fail_msg("golden product not finite at element %zu "
                            "(random operands need tighter scaling)", i);
        }
    }
    return EXIT_SUCCESS;
}

static int openblas_dgemm_run(struct test *test, int cpu) {
    auto d = CAST(test->data);
    size_t bytes = (size_t)d->mdim * d->mdim * sizeof(double);
    size_t n2 = (size_t)d->mdim * d->mdim;
    long iter = 0;
    /* B2-004 (R2b freeze blind spot): this thread's operands and golden
     * are re-rolled from the framework RNG every kRerollEvery iterations
     * instead of replaying init's frozen bytes forever; the shared
     * d->a/d->b/d->golden are never read by the reroll path (init's fill
     * stays only as the init-time finiteness canary, like the 46141ed4
     * Eigen pattern).
     * The golden is recomputed with the very same cblas call and knob set
     * the verified call below uses, on the same operand distribution, so
     * the finiteness bound argued in init carries over; -s replays stay
     * reproducible since the framework RNG is seeded per test.
     * PR #244 review (coderabbit): the reroll golden is recomputed on the
     * DUT core with the same cblas implementation, so a deterministic
     * (mercurial-core) fault poisons golden and DUT identically and the
     * memcmp passes. The shared init golden — computed on a different
     * core before the workers start — is therefore replayed at the
     * midpoint of every reroll period: cross-core byte-exact reference
     * that a single-core deterministic fault cannot poison. */
    constexpr int kRerollEvery = 16;
    int since_reroll = kRerollEvery; /* force the first-iteration fill */
    TEST_LOOP(test, 1) {
        /* lazily allocate this thread's scratch buffers */
        if (__builtin_expect(!a_copy || !b_copy || !c || !l_a || !l_b || !l_golden, 0)) {
            a_copy   = (double *)malloc(bytes);
            b_copy   = (double *)malloc(bytes);
            c        = (double *)calloc(1, bytes);
            l_a      = (double *)malloc(bytes);
            l_b      = (double *)malloc(bytes);
            l_golden = (double *)malloc(bytes);
            if (!a_copy || !b_copy || !c || !l_a || !l_b || !l_golden)
                report_fail_msg("OOM allocating thread scratch (%zu bytes)", bytes * 6);
        }

        if (++since_reroll >= kRerollEvery) {
            since_reroll = 0;
            /* re-roll: fresh high-entropy operands for this thread;
             * golden recomputed with the same cblas call, seeded with the
             * same deterministic C pattern the per-iteration seeding
             * below uses (beta semantics) */
            for (size_t i = 0; i < n2; ++i) {
                l_a[i] = random_bounded();
                l_b[i] = random_bounded();
            }
            for (size_t i = 0; i < n2; ++i)
                l_golden[i] = 0.5 * (double)(i & 63) / 64.0;
            cblas_dgemm(CblasRowMajor,
                        (d->transab & 2) ? CblasTrans : CblasNoTrans,
                        (d->transab & 1) ? CblasTrans : CblasNoTrans,
                        d->mdim, d->mdim, d->mdim,
                        1.0, l_a, d->mdim, l_b, d->mdim,
                        d->beta, l_golden, d->mdim);
        } else if (since_reroll == kRerollEvery / 2) {
            /* frozen replay (PR #244 review, coderabbit): restore init's
             * shared operands and golden — the init thread computed the
             * golden on a different core before the workers started, so
             * this round's byte-exact memcmp is a CROSS-CORE reference.
             * A deterministic (mercurial-core) fault would poison the
             * reroll golden and the DUT identically (same core, same
             * cblas call) — only the cross-core replay catches it.
             * Exactly one hit per reroll period: since_reroll takes this
             * value once between the reset at 0 and the next reroll. */
            memcpy(l_a, d->a, bytes);
            memcpy(l_b, d->b, bytes);
            memcpy(l_golden, d->golden, bytes);
        }

        /* copy-in (dirties cache lines, then reloads them in the kernel) */
        memcpy(a_copy, l_a, bytes);
        memcpy(b_copy, l_b, bytes);

        /* re-seed C's initial content every iteration: with beta != 0 the
         * kernel reads C (read-modify-write), so each iteration must start
         * from the same deterministic pattern the golden product was seeded
         * with at the reroll (recomputed inline — no extra shared array) */
        for (size_t i = 0; i < n2; ++i)
            c[i] = 0.5 * (double)(i & 63) / 64.0;

        cblas_dgemm(CblasRowMajor,
                    (d->transab & 2) ? CblasTrans : CblasNoTrans,
                    (d->transab & 1) ? CblasTrans : CblasNoTrans,
                    d->mdim, d->mdim, d->mdim,
                    1.0, a_copy, d->mdim, b_copy, d->mdim,
                    d->beta, c, d->mdim);

        ++iter;
        /* verify-out: byte-exact golden compare of inputs and product */
        if (memcmp(a_copy, l_a, bytes) != 0) {
            report_fail_msg("input A corrupted after GEMM (iteration %ld)", iter);
        }
        if (memcmp(b_copy, l_b, bytes) != 0) {
            report_fail_msg("input B corrupted after GEMM (iteration %ld)", iter);
        }
        memcmp_or_fail(c, l_golden, (size_t)d->mdim * d->mdim);
    }
    return EXIT_SUCCESS;
}

static int openblas_dgemm_cleanup(struct test *test) {
    auto d = CAST(test->data);
    free(d->a); free(d->b); free(d->golden);
    delete d;
    return EXIT_SUCCESS;
}

DECLARE_TEST(openblas_dgemm, "OpenBLAS DGEMM (NEON FMA micro-kernel, copy/compute/verify per iteration)")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = openblas_dgemm_init,
  .test_run = openblas_dgemm_run,
  .test_cleanup = openblas_dgemm_cleanup,
  .fracture_loop_count = 4,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
