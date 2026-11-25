/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ldp_stp_cpy_p13
 * @parblock
 * Dual-source EXT cross-lane probe — port of the external 346-suite
 * case SIMDFPLoadStoreMemLoopCpyP13 (2026-11-24, user directive).
 *
 * 3x128KB buffers (L2-resident footprint): srcA and srcB are shared
 * read-only sources filled with random bits; dst is per-thread; the
 * golden is a shared expected buffer computed in init by PURE SCALAR
 * word copies (issue #373 paradigm, scripts/check-scalar-golden.sh
 * gated — no vector instruction executes in the golden path).
 *
 * Hot loop (all instruction forms asm-pinned):
 *
 *   ldp  q0, q1, [srcA], #32      (source pair A: elements 2j, 2j+1)
 *   ldp  q2, q3, [srcB], #32      (source pair B)
 *   ext  v4.16B, v0.16B, v2.16B, #8   (cross-buffer extract)
 *   ext  v5.16B, v1.16B, v3.16B, #8
 *   stp  q4, q5, [dst], #32
 *
 * Discriminator (neon_rot lineage): the 179 recipe's discriminating
 * feature was "2 source loads + data ALU before the store" — this case
 * is its cross-lane-network variant: EXT consumes BOTH LDP results
 * (v0 with v2, v1 with v3, one from each source buffer) so both load
 * results flow through the vector permute/cross-lane network before
 * the store, at an L2-resident footprint the rowmajor family never
 * swept (3x128KB vs 2x64KB).
 *
 * Scalar golden derivation (byte-exact): ext Vd.16B, Vn.16B, Vm.16B,
 * #8 = bytes 8..23 of concat(Vn:Vm) = (hi half of Vn) ++ (lo half of
 * Vm).  Per pair-step j (u64 words 4j..4j+3):
 *   dst[4j]   = srcA[4j+1];  dst[4j+1] = srcB[4j];
 *   dst[4j+2] = srcA[4j+3];  dst[4j+3] = srcB[4j+2];
 * a pure word-copy permutation — no arithmetic, no vector unit.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define P13_COUNT 8192                 /* uint64x2_t elements -> 128KB/buffer */
#define P13_BYTES (P13_COUNT * 16)

namespace { /* ODR: per-TU P13Data */
struct P13Data {
    uint64_t *srcA;                    /* shared, read-only after init */
    uint64_t *srcB;                    /* shared, read-only after init */
    uint64_t *expected;                /* shared scalar golden */
};
}

static int ldp_stp_cpy_p13_init(struct test *test)
{
    auto *d = new P13Data;
    if (!d)
        return EXIT_FAILURE;

    d->srcA = static_cast<uint64_t *>(aligned_alloc(32, P13_BYTES));
    d->srcB = static_cast<uint64_t *>(aligned_alloc(32, P13_BYTES));
    d->expected = static_cast<uint64_t *>(aligned_alloc(32, P13_BYTES));
    if (!d->srcA || !d->srcB || !d->expected) {
        free(d->srcA); free(d->srcB); free(d->expected);
        delete d;
        return EXIT_FAILURE;
    }

    memset_random(d->srcA, P13_BYTES);
    memset_random(d->srcB, P13_BYTES);

    /* scalar golden: the EXT #8 permutation as pure word copies
     * (see header derivation — no vector instruction on this path) */
    for (size_t j = 0; j < P13_COUNT / 2; ++j) {
        const size_t w = j * 4;
        d->expected[w + 0] = d->srcA[w + 1];
        d->expected[w + 1] = d->srcB[w + 0];
        d->expected[w + 2] = d->srcA[w + 3];
        d->expected[w + 3] = d->srcB[w + 2];
    }

    test->data = d;
    return EXIT_SUCCESS;
}

static int ldp_stp_cpy_p13_cleanup(struct test *test)
{
    auto *d = static_cast<P13Data *>(test->data);
    if (d) {
        free(d->srcA);
        free(d->srcB);
        free(d->expected);
        delete d;
    }
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int ldp_stp_cpy_p13_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<P13Data *>(test->data);

    /* per-thread destination; sources and golden stay shared read-only */
    auto *dst = static_cast<uint64_t *>(aligned_alloc(32, P13_BYTES));
    if (!dst) {
        report_fail_msg("ldp_stp_cpy_p13: buffer allocation failed");
        return EXIT_FAILURE;
    }

    TEST_LOOP(test, 1 << 10) {
        /* one sweep: 4096 dual-source EXT pair-steps, full 128KB */
        const uint64_t *pa = d->srcA;
        const uint64_t *pb = d->srcB;
        uint64_t *pd = dst;
        for (size_t p = 0; p < P13_COUNT / 2; ++p) {
            __asm__ volatile(
                "ldp  q0, q1, [%[pa]], #32\n\t"
                "ldp  q2, q3, [%[pb]], #32\n\t"
                "ext  v4.16B, v0.16B, v2.16B, #8\n\t"
                "ext  v5.16B, v1.16B, v3.16B, #8\n\t"
                "stp  q4, q5, [%[pd]], #32\n\t"
                : [pa] "+r" (pa), [pb] "+r" (pb), [pd] "+r" (pd)
                :
                : "v0", "v1", "v2", "v3", "v4", "v5", "memory");
        }

        /* cold path: scalar golden comparison */
        if (memcmp(dst, d->expected, P13_BYTES) != 0) {
            for (size_t i = 0; i < P13_COUNT * 2; ++i) {
                if (dst[i] != d->expected[i]) {
                    report_fail_msg("ldp_stp_cpy_p13: dst mismatch at u64 "
                                    "%zu: expected 0x%016lx got 0x%016lx "
                                    "(dual-source LDP + EXT #8 cross-lane)",
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

DECLARE_TEST(ldp_stp_cpy_p13, "Dual-source EXT cross-lane probe (346-suite SIMDFPLoadStoreMemLoopCpyP13): post-indexed LDP Q-pairs from two 128KB L2-resident random sources + EXT #8 byte-extract across both + pair store, scalar word-copy golden (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = ldp_stp_cpy_p13_init,
    .test_run = ldp_stp_cpy_p13_run,
    .test_cleanup = ldp_stp_cpy_p13_cleanup,
    .fracture_loop_count = -1,   /* never fracture: sweep loops are fast */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
