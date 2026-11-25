/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ldp_stp_cpy_p05
 * @parblock
 * Bidirectional ping-pong rotation probe — port of the external
 * 346-suite case SIMDFPLoadStoreMemLoopCpyP05 (2026-11-24, user
 * directive).
 *
 * 2x64KB buffers, 100 ping-pong rounds per sweep: odd rounds copy
 * workA -> workB, even rounds workB -> workA, every copy passing the
 * pair through the rotating integer ALU:
 *
 *   ldp  q0, q1, [src], #32         (element pair 2j, 2j+1)
 *   r0 = rot4[j%4](v0, v1)          (add/eor/and/orr, uint64x2 lanes)
 *   r1 = rot4[(j+2)%4](v0, v1)      (second slot, offset by 2)
 *   stp  q2, q3, [dst], #32         (stores the (r0, r1) pair)
 *
 * Discriminator (neon_rot lineage): the closest chassis to the
 * rowmajor trigger recipe in the fmov copy family — rotating ALU
 * consumer + sweep DIRECTION axis (alternating read/write buffer
 * roles flip the store->load adjacency at every round seam), built on
 * the LDP/STP post-indexed pair form instead of single ldr/str.
 *
 * Golden (scalar, issue #373 paradigm — no vector instruction on the
 * golden path, check-scalar-golden.sh gated): masterA is a shared
 * read-only random master filled once in init; goldenA/goldenB are the
 * exact 100-round scalar fold of masterA (after round 100: workA =
 * fold100, workB = fold99 — odd rounds write B, even rounds write A).
 * Per pair-step j over source words w0..w3 = in[4j..4j+3]:
 *   out[4j+0] = rot64(slot1, w0, w2);  out[4j+1] = rot64(slot1, w1, w3);
 *   out[4j+2] = rot64(slot2, w0, w2);  out[4j+3] = rot64(slot2, w1, w3);
 * (r0/r1 are lane-wise u64 ops, so the scalar replica is exact.)
 *
 * Per sweep, workA is memcpy-reset from masterA at the sweep boundary
 * (cold path — never inside the hot round loop); workB needs no reset
 * (round 1 fully overwrites it).  workA/workB are per-thread.
 *
 * Known boundary (recorded deliberately): the check point is the
 * sweep-end memcmp of both buffers only — a per-round check would
 * perturb the round-seam store->load adjacency the direction axis is
 * probing.  A single-bit error that passes through the and/orr slots
 * can in principle be absorbed by the fold chain; the add/eor slots
 * propagate it.  100 rounds x 2048 pair-steps of exposure per sweep.
 * @endparblock
 */

#include "sandstone.h"
#include <arm_neon.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define P05_COUNT 4096                 /* uint64x2_t elements -> 64KB/buffer */
#define P05_BYTES (P05_COUNT * 16)
#define P05_ROUNDS 100                  /* ping-pong rounds per sweep */

namespace { /* ODR: per-TU P05Data */
struct P05Data {
    uint64_t *masterA;                  /* shared read-only random master */
    uint64_t *goldenA;                  /* shared scalar fold100 */
    uint64_t *goldenB;                  /* shared scalar fold99 */
};
}

/* scalar golden replica: lane-wise u64 rotation (see header derivation) */
static inline uint64_t p05_rot64(int slot, uint64_t a, uint64_t b)
{
    switch (slot) {
        case 0:  return a + b;
        case 1:  return a ^ b;
        case 2:  return a & b;
        default: return a | b;
    }
}

static void p05_fold_round(const uint64_t *in, uint64_t *out)
{
    for (size_t j = 0; j < P05_COUNT / 2; ++j) {
        const int s1 = (int)(j % 4);
        const int s2 = (int)((j + 2) % 4);
        const uint64_t w0 = in[4 * j + 0];
        const uint64_t w1 = in[4 * j + 1];
        const uint64_t w2 = in[4 * j + 2];
        const uint64_t w3 = in[4 * j + 3];
        out[4 * j + 0] = p05_rot64(s1, w0, w2);
        out[4 * j + 1] = p05_rot64(s1, w1, w3);
        out[4 * j + 2] = p05_rot64(s2, w0, w2);
        out[4 * j + 3] = p05_rot64(s2, w1, w3);
    }
}

/* hot-path consumer: the same rotation on the vector units */
static inline uint64x2_t p05_rot4(int slot, uint64x2_t a, uint64x2_t b)
{
    switch (slot) {
        case 0:  return vaddq_u64(a, b);
        case 1:  return veorq_u64(a, b);
        case 2:  return vandq_u64(a, b);
        default: return vorrq_u64(a, b);
    }
}

static int ldp_stp_cpy_p05_init(struct test *test)
{
    auto *d = new P05Data;
    if (!d)
        return EXIT_FAILURE;

    d->masterA = static_cast<uint64_t *>(aligned_alloc(32, P05_BYTES));
    d->goldenA = static_cast<uint64_t *>(aligned_alloc(32, P05_BYTES));
    d->goldenB = static_cast<uint64_t *>(aligned_alloc(32, P05_BYTES));
    if (!d->masterA || !d->goldenA || !d->goldenB) {
        free(d->masterA); free(d->goldenA); free(d->goldenB);
        delete d;
        return EXIT_FAILURE;
    }

    memset_random(d->masterA, P05_BYTES);

    /* scalar fold: goldenA starts as masterA, then 100 rounds of
     * ping-pong between goldenA/goldenB — after round 100 goldenA
     * holds fold100, goldenB holds fold99 (odd rounds write B) */
    memcpy(d->goldenA, d->masterA, P05_BYTES);
    for (int r = 1; r <= P05_ROUNDS; ++r) {
        if (r & 1)
            p05_fold_round(d->goldenA, d->goldenB);
        else
            p05_fold_round(d->goldenB, d->goldenA);
    }

    test->data = d;
    return EXIT_SUCCESS;
}

static int ldp_stp_cpy_p05_cleanup(struct test *test)
{
    auto *d = static_cast<P05Data *>(test->data);
    if (d) {
        free(d->masterA);
        free(d->goldenA);
        free(d->goldenB);
        delete d;
    }
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int ldp_stp_cpy_p05_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<P05Data *>(test->data);

    /* per-thread ping-pong work buffers */
    auto *workA = static_cast<uint64_t *>(aligned_alloc(32, P05_BYTES));
    auto *workB = static_cast<uint64_t *>(aligned_alloc(32, P05_BYTES));
    if (!workA || !workB) {
        free(workA); free(workB);
        report_fail_msg("ldp_stp_cpy_p05: buffer allocation failed");
        return EXIT_FAILURE;
    }

    TEST_LOOP(test, 1 << 8) {
        /* cold reset from the master at the sweep boundary; workB needs
         * none (round 1 fully overwrites it) */
        memcpy(workA, d->masterA, P05_BYTES);

        uint64_t *cur = workA, *nxt = workB;
        for (int r = 1; r <= P05_ROUNDS; ++r) {
            const uint64_t *ps = cur;
            uint64_t *pd = nxt;
            for (size_t j = 0; j < P05_COUNT / 2; ++j) {
                uint64x2_t a, b;
                __asm__ volatile(
                    "ldp  %q0, %q1, [%2], #32\n\t"
                    : "=w" (a), "=w" (b), "+r" (ps)
                    :
                    : "memory");
                const uint64x2_t r0 = p05_rot4((int)(j % 4), a, b);
                const uint64x2_t r1 = p05_rot4((int)((j + 2) % 4), a, b);
                __asm__ volatile(
                    "stp  %q1, %q2, [%0], #32\n\t"
                    : "+r" (pd)
                    : "w" (r0), "w" (r1)
                    : "memory");
            }
            uint64_t *swp = cur; cur = nxt; nxt = swp;
        }

        /* cold path: both buffers against the scalar fold goldens */
        if (memcmp(workA, d->goldenA, P05_BYTES) != 0 ||
            memcmp(workB, d->goldenB, P05_BYTES) != 0) {
            const uint64_t *gold[2] = { d->goldenA, d->goldenB };
            const uint64_t *work[2] = { workA, workB };
            static const char *names[2] = { "workA(fold100)", "workB(fold99)" };
            for (int k = 0; k < 2; ++k) {
                for (size_t i = 0; i < P05_COUNT * 2; ++i) {
                    if (work[k][i] != gold[k][i]) {
                        report_fail_msg("ldp_stp_cpy_p05: %s mismatch at u64 "
                                        "%zu: expected 0x%016lx got 0x%016lx "
                                        "(100-round ping-pong rot4 fold)",
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

DECLARE_TEST(ldp_stp_cpy_p05, "Bidirectional ping-pong rotation probe (346-suite SIMDFPLoadStoreMemLoopCpyP05): post-indexed LDP/STP Q-pairs over 2x64KB random buffers, 100 A<->B rounds per sweep through the rotating add/eor/and/orr consumer, scalar fold100/fold99 goldens (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = ldp_stp_cpy_p05_init,
    .test_run = ldp_stp_cpy_p05_run,
    .test_cleanup = ldp_stp_cpy_p05_cleanup,
    .fracture_loop_count = -1,   /* never fracture: sweep loops are fast */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
