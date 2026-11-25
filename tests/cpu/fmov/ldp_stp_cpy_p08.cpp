/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ldp_stp_cpy_p08
 * @parblock
 * Pure pair-form copy probe — port of the external 346-suite case
 * SIMDFPLoadStoreMemLoopCpyP08 (2026-11-24, user directive).
 *
 * Hot loop is a bare LDP/STP post-indexed train over 2x16KB L1-resident
 * buffers with NO data transformation:
 *
 *   ldp  q0, q1, [src], #32
 *   stp  q0, q1, [dst], #32        (x512 per sweep, 32B per pair)
 *
 * Discriminator (neon_rot lineage): this is the no-ALU existence
 * baseline of the fmov copy family — does the LDP/STP post-indexed
 * pair pressure by itself constitute a trigger carrier, with no
 * consumer between the load and the store pair (cf. neon_rot_store_only,
 * which established the scalar-side store-only existence proof)?
 * P05/P06/P13 add consumers (rotating ALU / EXT) on top of this
 * chassis; adaptive_ldp_stp sweeps the count axes around it.
 *
 * Data: random bits (memset_random) — 2026-11-24 user directive.  The
 * original suite's 5a_a5 fill is carried instead by the p05/p06
 * data-plane contrast pair, not here.
 *
 * Golden: the source buffer itself — the hot loop never writes src, so
 * the whole check is the sweep-end cold-path memcmp(dst, src) (2-buffer
 * footprint, faithful to the original 2x16KB spec).  src is shared
 * read-only after init; dst is per-thread (no cross-thread races).
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define P08_COUNT 1024                 /* uint64x2_t elements -> 16KB/buffer */
#define P08_BYTES (P08_COUNT * 16)

namespace { /* ODR: per-TU P08Data */
struct P08Data {
    uint64_t *src;                     /* shared, read-only after init */
};
}

static int ldp_stp_cpy_p08_init(struct test *test)
{
    auto *d = new P08Data;
    if (!d)
        return EXIT_FAILURE;

    /* 32B-aligned: LDP/STP Q-pair access size (fmov_train_ldp_stp idiom) */
    d->src = static_cast<uint64_t *>(aligned_alloc(32, P08_BYTES));
    if (!d->src) {
        delete d;
        return EXIT_FAILURE;
    }
    memset_random(d->src, P08_BYTES);

    test->data = d;
    return EXIT_SUCCESS;
}

static int ldp_stp_cpy_p08_cleanup(struct test *test)
{
    auto *d = static_cast<P08Data *>(test->data);
    if (d) {
        free(d->src);
        delete d;
    }
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int ldp_stp_cpy_p08_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<P08Data *>(test->data);

    /* per-thread destination; the source stays shared read-only */
    auto *dst = static_cast<uint64_t *>(aligned_alloc(32, P08_BYTES));
    if (!dst) {
        report_fail_msg("ldp_stp_cpy_p08: buffer allocation failed");
        return EXIT_FAILURE;
    }

    TEST_LOOP(test, 1 << 12) {
        /* one sweep: 512 post-indexed LDP/STP pairs, full 16KB copy */
        const uint64_t *ps = d->src;
        uint64_t *pd = dst;
        for (size_t p = 0; p < P08_COUNT / 2; ++p) {
            __asm__ volatile(
                "ldp  q0, q1, [%[src]], #32\n\t"
                "stp  q0, q1, [%[dst]], #32\n\t"
                : [src] "+r" (ps), [dst] "+r" (pd)
                :
                : "v0", "v1", "memory");
        }

        /* cold path: the source is the golden (never written by the
         * hot loop — pure copy, no transformation) */
        if (memcmp(dst, d->src, P08_BYTES) != 0) {
            for (size_t i = 0; i < P08_COUNT * 2; ++i) {
                if (dst[i] != d->src[i]) {
                    report_fail_msg("ldp_stp_cpy_p08: dst mismatch at u64 "
                                    "%zu: expected 0x%016lx got 0x%016lx "
                                    "(pure post-indexed LDP/STP pair copy)",
                                    i, (unsigned long)d->src[i],
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

DECLARE_TEST(ldp_stp_cpy_p08, "Pure pair-form copy probe (346-suite SIMDFPLoadStoreMemLoopCpyP08): post-indexed LDP/STP Q-pair train over 2x16KB L1-resident random data, no consumer — the no-ALU existence baseline of the fmov copy family (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = ldp_stp_cpy_p08_init,
    .test_run = ldp_stp_cpy_p08_run,
    .test_cleanup = ldp_stp_cpy_p08_cleanup,
    .fracture_loop_count = -1,   /* never fracture: sweep loops are fast */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
