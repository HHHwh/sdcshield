/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ldp_stp_cpy_p05_intr
 * @parblock
 * Intrinsics-built twin of ldp_stp_cpy_p05 (2026-11-25, user directive:
 * one C++-built extension per fmov case).
 *
 * Same logical workload as the asm twin — 2x64KB random buffers, 100
 * ping-pong rounds per sweep (odd A->B, even B->A) through the
 * rotating add/eor/and/orr consumer with the j%4 / (j+2)%4 slot
 * schedule — but ZERO inline assembly: the pair loads are two
 * vld1q_u64, the stores two vst1q_u64, and the compiler schedules and
 * encodes freely (this TU compiles with -fno-tree-loop-distribute-
 * patterns; the consumer between load and store already blocks any
 * libc-call folding, the flag is belt-and-suspenders).
 *
 * Golden (scalar, issue #373 paradigm, check-scalar-golden.sh gated):
 * the SAME fold semantics as the asm twin — masterA is a shared
 * read-only random master; goldenA = fold100, goldenB = fold99 (odd
 * rounds write B); per sweep workA is memcpy-reset from masterA at the
 * sweep boundary (workB needs none — round 1 fully overwrites it).
 *
 * Known boundary (same as the asm twin, recorded deliberately): the
 * check point is the sweep-end memcmp of both buffers only; a
 * single-bit error through the and/orr slots can in principle be
 * absorbed by the fold chain (add/eor propagate it).  The and/orr
 * word degeneration toward 0x00/0xff words over ~40 rounds is also
 * inherited (only bus switching activity decays; every divergence is
 * still a bit-exact miscompare).
 * @endparblock
 */

#include "sandstone.h"
#include <arm_neon.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define P05I_COUNT 4096                /* uint64x2_t elements -> 64KB/buffer */
#define P05I_BYTES (P05I_COUNT * 16)
#define P05I_ROUNDS 100                 /* ping-pong rounds per sweep */

namespace { /* ODR: per-TU P05IData */
struct P05IData {
    uint64_t *masterA;                  /* shared read-only random master */
    uint64_t *goldenA;                  /* shared scalar fold100 */
    uint64_t *goldenB;                  /* shared scalar fold99 */
};
}

/* scalar golden replica: lane-wise u64 rotation (identical semantics to
 * the asm twin's fold — bit-comparable goldens by construction) */
static inline uint64_t p05i_rot64(int slot, uint64_t a, uint64_t b)
{
    switch (slot) {
        case 0:  return a + b;
        case 1:  return a ^ b;
        case 2:  return a & b;
        default: return a | b;
    }
}

static void p05i_fold_round(const uint64_t *in, uint64_t *out)
{
    for (size_t j = 0; j < P05I_COUNT / 2; ++j) {
        const int s1 = (int)(j % 4);
        const int s2 = (int)((j + 2) % 4);
        const uint64_t w0 = in[4 * j + 0];
        const uint64_t w1 = in[4 * j + 1];
        const uint64_t w2 = in[4 * j + 2];
        const uint64_t w3 = in[4 * j + 3];
        out[4 * j + 0] = p05i_rot64(s1, w0, w2);
        out[4 * j + 1] = p05i_rot64(s1, w1, w3);
        out[4 * j + 2] = p05i_rot64(s2, w0, w2);
        out[4 * j + 3] = p05i_rot64(s2, w1, w3);
    }
}

/* hot-path consumer: the same rotation on the vector units */
static inline uint64x2_t p05i_rot4(int slot, uint64x2_t a, uint64x2_t b)
{
    switch (slot) {
        case 0:  return vaddq_u64(a, b);
        case 1:  return veorq_u64(a, b);
        case 2:  return vandq_u64(a, b);
        default: return vorrq_u64(a, b);
    }
}

static int ldp_stp_cpy_p05_intr_init(struct test *test)
{
    auto *d = new P05IData;
    if (!d)
        return EXIT_FAILURE;

    d->masterA = static_cast<uint64_t *>(aligned_alloc(32, P05I_BYTES));
    d->goldenA = static_cast<uint64_t *>(aligned_alloc(32, P05I_BYTES));
    d->goldenB = static_cast<uint64_t *>(aligned_alloc(32, P05I_BYTES));
    if (!d->masterA || !d->goldenA || !d->goldenB) {
        free(d->masterA); free(d->goldenA); free(d->goldenB);
        delete d;
        return EXIT_FAILURE;
    }

    memset_random(d->masterA, P05I_BYTES);

    /* scalar fold: goldenA starts as masterA, then 100 rounds of
     * ping-pong between goldenA/goldenB — after round 100 goldenA
     * holds fold100, goldenB holds fold99 (odd rounds write B) */
    memcpy(d->goldenA, d->masterA, P05I_BYTES);
    for (int r = 1; r <= P05I_ROUNDS; ++r) {
        if (r & 1)
            p05i_fold_round(d->goldenA, d->goldenB);
        else
            p05i_fold_round(d->goldenB, d->goldenA);
    }

    test->data = d;
    return EXIT_SUCCESS;
}

static int ldp_stp_cpy_p05_intr_cleanup(struct test *test)
{
    auto *d = static_cast<P05IData *>(test->data);
    if (d) {
        free(d->masterA);
        free(d->goldenA);
        free(d->goldenB);
        delete d;
    }
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int ldp_stp_cpy_p05_intr_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<P05IData *>(test->data);

    /* per-thread ping-pong work buffers */
    auto *workA = static_cast<uint64_t *>(aligned_alloc(32, P05I_BYTES));
    auto *workB = static_cast<uint64_t *>(aligned_alloc(32, P05I_BYTES));
    if (!workA || !workB) {
        free(workA); free(workB);
        report_fail_msg("ldp_stp_cpy_p05_intr: buffer allocation failed");
        return EXIT_FAILURE;
    }

    TEST_LOOP(test, 1 << 8) {
        /* cold reset from the master at the sweep boundary; workB needs
         * none (round 1 fully overwrites it) */
        memcpy(workA, d->masterA, P05I_BYTES);

        uint64_t *cur = workA, *nxt = workB;
        for (int r = 1; r <= P05I_ROUNDS; ++r) {
            const uint64_t *ps = cur;
            uint64_t *pd = nxt;
            for (size_t j = 0; j < P05I_COUNT / 2; ++j) {
                const size_t w = j * 4;
                const uint64x2_t a = vld1q_u64(&ps[w + 0]);   /* elem 2j   */
                const uint64x2_t b = vld1q_u64(&ps[w + 2]);   /* elem 2j+1 */
                const uint64x2_t r0 = p05i_rot4((int)(j % 4), a, b);
                const uint64x2_t r1 = p05i_rot4((int)((j + 2) % 4), a, b);
                vst1q_u64(&pd[w + 0], r0);
                vst1q_u64(&pd[w + 2], r1);
            }
            uint64_t *swp = cur; cur = nxt; nxt = swp;
        }

        /* cold path: both buffers against the scalar fold goldens */
        if (memcmp(workA, d->goldenA, P05I_BYTES) != 0 ||
            memcmp(workB, d->goldenB, P05I_BYTES) != 0) {
            const uint64_t *gold[2] = { d->goldenA, d->goldenB };
            const uint64_t *work[2] = { workA, workB };
            static const char *names[2] = { "workA(fold100)", "workB(fold99)" };
            for (int k = 0; k < 2; ++k) {
                for (size_t i = 0; i < P05I_COUNT * 2; ++i) {
                    if (work[k][i] != gold[k][i]) {
                        report_fail_msg("ldp_stp_cpy_p05_intr: %s mismatch at "
                                        "u64 %zu: expected 0x%016lx got "
                                        "0x%016lx (compiler-built 100-round "
                                        "ping-pong rot4 fold, no asm)",
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

DECLARE_TEST(ldp_stp_cpy_p05_intr, "Intrinsics-built twin of ldp_stp_cpy_p05: same 100-round bidirectional ping-pong rot4 workload over 2x64KB random buffers (346-suite SIMDFPLoadStoreMemLoopCpyP05), zero asm — vld1q/vst1q with compiler-chosen encodings, scalar fold100/fold99 goldens identical to the asm twin (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = ldp_stp_cpy_p05_intr_init,
    .test_run = ldp_stp_cpy_p05_intr_run,
    .test_cleanup = ldp_stp_cpy_p05_intr_cleanup,
    .fracture_loop_count = -1,   /* never fracture: sweep loops are fast */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
