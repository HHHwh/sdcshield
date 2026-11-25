/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ldp_stp_cpy_p06
 * @parblock
 * 5a_a5 data-plane contrast twin of ldp_stp_cpy_p05 — port of the
 * external 346-suite case SIMDFPLoadStoreMemLoopCpyP06 (2026-11-24,
 * user directive).
 *
 * The hot loop, the consumer rotation, the ping-pong direction axis,
 * the buffer sizes and the scalar golden machinery are instruction-for-
 * instruction IDENTICAL to ldp_stp_cpy_p05; the ONLY difference is the
 * master fill: instead of random bits, the master is filled with the
 * 5a_a5 alternating byte pattern (0x5a/0xa5 per byte = 01011010
 * 10100101 — every bit toggles between consecutive bytes, the
 * switching-activity maximum for a fixed pattern).  Per the 2026-11-24
 * user decision this is the family's ONLY 5a_a5 carrier: P08 and P13
 * run random data, so this pair is the family's data-plane
 * discriminant (k7pattern lineage: whether the trigger depends on data
 * content is a proven open axis — the all-zero fill killed the 179
 * trigger entirely in the k7p_zero experiment).
 *
 * Degeneration note (deliberate, faithful to the 346 original): after
 * the first ping-pong round the and/orr rotation slots start
 * collapsing the 2-byte-periodic pattern toward 0x00/0xff words; the
 * scalar golden covers the exact fold, so any divergence is still a
 * bit-exact miscompare — only the bus switching activity decays from
 * its 5a_a5 maximum as the rounds proceed.
 *
 * Discriminator: same code, different data — a machine that detects
 * SDC under p05 (random) but not under p06 (5a_a5), or vice versa,
 * exhibits data-plane gating of the trigger.
 * @endparblock
 */

#include "sandstone.h"
#include <arm_neon.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define P06_COUNT 4096                 /* uint64x2_t elements -> 64KB/buffer */
#define P06_BYTES (P06_COUNT * 16)
#define P06_ROUNDS 100                  /* ping-pong rounds per sweep */

namespace { /* ODR: per-TU P06Data */
struct P06Data {
    uint64_t *masterA;                  /* shared read-only 5a_a5 master */
    uint64_t *goldenA;                  /* shared scalar fold100 */
    uint64_t *goldenB;                  /* shared scalar fold99 */
};
}

/* scalar golden replica: lane-wise u64 rotation (identical to p05) */
static inline uint64_t p06_rot64(int slot, uint64_t a, uint64_t b)
{
    switch (slot) {
        case 0:  return a + b;
        case 1:  return a ^ b;
        case 2:  return a & b;
        default: return a | b;
    }
}

static void p06_fold_round(const uint64_t *in, uint64_t *out)
{
    for (size_t j = 0; j < P06_COUNT / 2; ++j) {
        const int s1 = (int)(j % 4);
        const int s2 = (int)((j + 2) % 4);
        const uint64_t w0 = in[4 * j + 0];
        const uint64_t w1 = in[4 * j + 1];
        const uint64_t w2 = in[4 * j + 2];
        const uint64_t w3 = in[4 * j + 3];
        out[4 * j + 0] = p06_rot64(s1, w0, w2);
        out[4 * j + 1] = p06_rot64(s1, w1, w3);
        out[4 * j + 2] = p06_rot64(s2, w0, w2);
        out[4 * j + 3] = p06_rot64(s2, w1, w3);
    }
}

/* hot-path consumer: the same rotation on the vector units */
static inline uint64x2_t p06_rot4(int slot, uint64x2_t a, uint64x2_t b)
{
    switch (slot) {
        case 0:  return vaddq_u64(a, b);
        case 1:  return veorq_u64(a, b);
        case 2:  return vandq_u64(a, b);
        default: return vorrq_u64(a, b);
    }
}

static int ldp_stp_cpy_p06_init(struct test *test)
{
    auto *d = new P06Data;
    if (!d)
        return EXIT_FAILURE;

    d->masterA = static_cast<uint64_t *>(aligned_alloc(32, P06_BYTES));
    d->goldenA = static_cast<uint64_t *>(aligned_alloc(32, P06_BYTES));
    d->goldenB = static_cast<uint64_t *>(aligned_alloc(32, P06_BYTES));
    if (!d->masterA || !d->goldenA || !d->goldenB) {
        free(d->masterA); free(d->goldenA); free(d->goldenB);
        delete d;
        return EXIT_FAILURE;
    }

    /* THE difference vs p05: 5a_a5 alternating byte fill instead of
     * random bits (0x5a at even byte offsets, 0xa5 at odd).  Filled as
     * a word constant — byte j of every word is 0x5a/a5 by j's parity
     * (8 | word size, so every word is identical 0xa55aa55aa55aa55a) —
     * a constant-store loop compiles to pure data movement; a per-byte
     * select loop gets GCC-auto-vectorized into vector ALU and fails
     * the check-scalar-golden gate on this init (caught 2026-11-24). */
    for (size_t k = 0; k < P06_COUNT * 2; ++k)
        d->masterA[k] = 0xa55aa55aa55aa55aULL;

    /* scalar fold: goldenA starts as masterA, then 100 rounds of
     * ping-pong between goldenA/goldenB — after round 100 goldenA
     * holds fold100, goldenB holds fold99 (odd rounds write B) */
    memcpy(d->goldenA, d->masterA, P06_BYTES);
    for (int r = 1; r <= P06_ROUNDS; ++r) {
        if (r & 1)
            p06_fold_round(d->goldenA, d->goldenB);
        else
            p06_fold_round(d->goldenB, d->goldenA);
    }

    test->data = d;
    return EXIT_SUCCESS;
}

static int ldp_stp_cpy_p06_cleanup(struct test *test)
{
    auto *d = static_cast<P06Data *>(test->data);
    if (d) {
        free(d->masterA);
        free(d->goldenA);
        free(d->goldenB);
        delete d;
    }
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int ldp_stp_cpy_p06_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<P06Data *>(test->data);

    /* per-thread ping-pong work buffers */
    auto *workA = static_cast<uint64_t *>(aligned_alloc(32, P06_BYTES));
    auto *workB = static_cast<uint64_t *>(aligned_alloc(32, P06_BYTES));
    if (!workA || !workB) {
        free(workA); free(workB);
        report_fail_msg("ldp_stp_cpy_p06: buffer allocation failed");
        return EXIT_FAILURE;
    }

    TEST_LOOP(test, 1 << 8) {
        /* cold reset from the master at the sweep boundary; workB needs
         * none (round 1 fully overwrites it) */
        memcpy(workA, d->masterA, P06_BYTES);

        uint64_t *cur = workA, *nxt = workB;
        for (int r = 1; r <= P06_ROUNDS; ++r) {
            const uint64_t *ps = cur;
            uint64_t *pd = nxt;
            for (size_t j = 0; j < P06_COUNT / 2; ++j) {
                uint64x2_t a, b;
                __asm__ volatile(
                    "ldp  %q0, %q1, [%2], #32\n\t"
                    : "=w" (a), "=w" (b), "+r" (ps)
                    :
                    : "memory");
                const uint64x2_t r0 = p06_rot4((int)(j % 4), a, b);
                const uint64x2_t r1 = p06_rot4((int)((j + 2) % 4), a, b);
                __asm__ volatile(
                    "stp  %q1, %q2, [%0], #32\n\t"
                    : "+r" (pd)
                    : "w" (r0), "w" (r1)
                    : "memory");
            }
            uint64_t *swp = cur; cur = nxt; nxt = swp;
        }

        /* cold path: both buffers against the scalar fold goldens */
        if (memcmp(workA, d->goldenA, P06_BYTES) != 0 ||
            memcmp(workB, d->goldenB, P06_BYTES) != 0) {
            const uint64_t *gold[2] = { d->goldenA, d->goldenB };
            const uint64_t *work[2] = { workA, workB };
            static const char *names[2] = { "workA(fold100)", "workB(fold99)" };
            for (int k = 0; k < 2; ++k) {
                for (size_t i = 0; i < P06_COUNT * 2; ++i) {
                    if (work[k][i] != gold[k][i]) {
                        report_fail_msg("ldp_stp_cpy_p06: %s mismatch at u64 "
                                        "%zu: expected 0x%016lx got 0x%016lx "
                                        "(100-round ping-pong rot4 fold, "
                                        "5a_a5 master)",
                                        names[k], i,
                                        (unsigned long)gold[k][i],
                                        (unsigned long)work[k][i]);
                        free(workA); free(workB);
                        return EXIT_FAILURE;
                    }
                }
            }
        }
    }

    free(workA);
    free(workB);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ldp_stp_cpy_p06, "5a_a5 data-plane contrast twin of ldp_stp_cpy_p05 (346-suite SIMDFPLoadStoreMemLoopCpyP06): instruction-identical ping-pong rot4 chassis, master filled with the 5a/a5 alternating byte pattern instead of random — the family's only fixed-pattern data discriminant (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = ldp_stp_cpy_p06_init,
    .test_run = ldp_stp_cpy_p06_run,
    .test_cleanup = ldp_stp_cpy_p06_cleanup,
    .fracture_loop_count = -1,   /* never fracture: sweep loops are fast */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
