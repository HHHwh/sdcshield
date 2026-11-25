/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ldp_stp_cpy_p13_intr
 * @parblock
 * Intrinsics-built twin of ldp_stp_cpy_p13 (2026-11-25, user directive:
 * one C++-built extension per fmov case).
 *
 * Same logical workload as the asm twin — 3x128KB L2-resident buffers,
 * dual-source load + EXT #8 cross-buffer byte extract + pair store,
 * the "2 source loads + data ALU" cross-lane variant — but ZERO
 * inline assembly: the loads/stores are vld1q_u64/vst1q_u64 and the
 * extract is the vextq_u64(x, y, 1) intrinsic (result = (x.hi, y.lo),
 * lane-rotate-by-one u64 == ext #8 on 16B vectors).  The compiler is
 * free to schedule, pair, and encode however it chooses — this TU
 * compiles with -fno-tree-loop-distribute-patterns so the element
 * train can never collapse into a libc call (see fmov/meson.build).
 *
 * Golden: the SAME scalar word permutation as the asm twin (pure
 * word copies, issue #373 paradigm, check-scalar-golden.sh gated):
 *   dst[4j]   = srcA[4j+1];  dst[4j+1] = srcB[4j];
 *   dst[4j+2] = srcA[4j+3];  dst[4j+3] = srcB[4j+2];
 * identical expected bytes — the two twins are bit-comparable on the
 * same source data by construction.
 * @endparblock
 */

#include "sandstone.h"
#include <arm_neon.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define P13I_COUNT 8192                /* uint64x2_t elements -> 128KB/buffer */
#define P13I_BYTES (P13I_COUNT * 16)

namespace { /* ODR: per-TU P13IData */
struct P13IData {
    uint64_t *srcA;                    /* shared, read-only after init */
    uint64_t *srcB;                    /* shared, read-only after init */
    uint64_t *expected;                /* shared scalar golden */
};
}

static int ldp_stp_cpy_p13_intr_init(struct test *test)
{
    auto *d = new P13IData;
    if (!d)
        return EXIT_FAILURE;

    d->srcA = static_cast<uint64_t *>(aligned_alloc(32, P13I_BYTES));
    d->srcB = static_cast<uint64_t *>(aligned_alloc(32, P13I_BYTES));
    d->expected = static_cast<uint64_t *>(aligned_alloc(32, P13I_BYTES));
    if (!d->srcA || !d->srcB || !d->expected) {
        free(d->srcA); free(d->srcB); free(d->expected);
        delete d;
        return EXIT_FAILURE;
    }

    memset_random(d->srcA, P13I_BYTES);
    memset_random(d->srcB, P13I_BYTES);

    /* scalar golden: the EXT #8 permutation as pure word copies
     * (same derivation as the asm twin — no vector instruction here) */
    for (size_t j = 0; j < P13I_COUNT / 2; ++j) {
        const size_t w = j * 4;
        d->expected[w + 0] = d->srcA[w + 1];
        d->expected[w + 1] = d->srcB[w + 0];
        d->expected[w + 2] = d->srcA[w + 3];
        d->expected[w + 3] = d->srcB[w + 2];
    }

    test->data = d;
    return EXIT_SUCCESS;
}

static int ldp_stp_cpy_p13_intr_cleanup(struct test *test)
{
    auto *d = static_cast<P13IData *>(test->data);
    if (d) {
        free(d->srcA);
        free(d->srcB);
        free(d->expected);
        delete d;
    }
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int ldp_stp_cpy_p13_intr_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<P13IData *>(test->data);

    /* per-thread destination; sources and golden stay shared read-only */
    auto *dst = static_cast<uint64_t *>(aligned_alloc(32, P13I_BYTES));
    if (!dst) {
        report_fail_msg("ldp_stp_cpy_p13_intr: buffer allocation failed");
        return EXIT_FAILURE;
    }

    TEST_LOOP(test, 1 << 10) {
        /* one sweep: 4096 dual-source EXT pair-steps, full 128KB,
         * compiler-built (no asm) */
        for (size_t j = 0; j < P13I_COUNT / 2; ++j) {
            const size_t w = j * 4;
            const uint64x2_t a0 = vld1q_u64(&d->srcA[w + 0]);   /* elem 2j   */
            const uint64x2_t a1 = vld1q_u64(&d->srcA[w + 2]);   /* elem 2j+1 */
            const uint64x2_t b0 = vld1q_u64(&d->srcB[w + 0]);
            const uint64x2_t b1 = vld1q_u64(&d->srcB[w + 2]);
            /* (x.hi, y.lo) == ext #8 across the two sources */
            vst1q_u64(&dst[w + 0], vextq_u64(a0, b0, 1));
            vst1q_u64(&dst[w + 2], vextq_u64(a1, b1, 1));
        }

        /* cold path: scalar golden comparison */
        if (memcmp(dst, d->expected, P13I_BYTES) != 0) {
            for (size_t i = 0; i < P13I_COUNT * 2; ++i) {
                if (dst[i] != d->expected[i]) {
                    report_fail_msg("ldp_stp_cpy_p13_intr: dst mismatch at "
                                    "u64 %zu: expected 0x%016lx got "
                                    "0x%016lx (compiler-built dual-source "
                                    "EXT extract, no asm)",
                                    i, (unsigned long)d->expected[i],
                                    (unsigned long)dst[i]);
                    free(dst);
                    return EXIT_FAILURE;
                }
            }
        }
    }

    free(dst);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ldp_stp_cpy_p13_intr, "Intrinsics-built twin of ldp_stp_cpy_p13: same dual-source EXT #8 cross-lane workload over 3x128KB L2-resident random buffers (346-suite SIMDFPLoadStoreMemLoopCpyP13), zero asm — vld1q/vextq_u64/vst1q with compiler-chosen encodings, scalar word-permutation golden identical to the asm twin (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = ldp_stp_cpy_p13_intr_init,
    .test_run = ldp_stp_cpy_p13_intr_run,
    .test_cleanup = ldp_stp_cpy_p13_intr_cleanup,
    .fracture_loop_count = -1,   /* never fracture: sweep loops are fast */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
