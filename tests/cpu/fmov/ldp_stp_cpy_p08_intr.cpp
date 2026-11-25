/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ldp_stp_cpy_p08_intr
 * @parblock
 * Intrinsics-built twin of ldp_stp_cpy_p08 (2026-11-25, user directive:
 * one C++-built extension per fmov case).
 *
 * Same logical workload as the asm twin — 2x16KB L1-resident random
 * buffers, pure copy with NO data transformation, the family's no-ALU
 * existence baseline — but ZERO inline assembly: the hot loop is a
 * plain vld1q_u64 / vst1q_u64 element copy and the compiler is free to
 * schedule and encode it however it chooses (post-indexed pairs, base
 * plus immediate, whatever GCC emits).  The emitted encoding is an
 * OBSERVATION recorded in the session ledger, not a gate: this arm
 * answers "does the naturally-compiled copy workload detect SDC",
 * while the asm twin answers "does the pinned post-indexed LDP/STP
 * encoding".  Together they are an encoding-discrimination pair
 * (fmov_train_gdroop_ldp_stp <-> _intr lineage).
 *
 * Paradigm note (2026-11-25 SDC-understanding correction): the
 * field-relevant face of SDC detection is compiler-generated code —
 * the highest-rate 0102 probe (neon_rot_ldr_at_top_rowmajor_fp64_dump)
 * uses exactly one asm line (an anti-DCE dead-load pin) and eigen /
 * O3-compiled workloads trigger real defects; hand-frozen encodings
 * are the research arm, not the detection default.
 *
 * Zero asm is possible here because every loaded value is consumed
 * (copied out) — the fp64_dump champion keeps its single asm line to
 * keep an architecturally-dead load from being DCE'd; this loop has no
 * dead loads to protect.
 *
 * Construction note (observed 2026-11-25): a MONOTONE vld1q/vst1q copy
 * loop is idiom-recognized by GCC into a plain memcpy() call (objdump
 * showed `bl memcpy`), which would duplicate this repo's existing
 * 28-test memcpy fleet.  This twin therefore carries the champion's
 * ALTERNATING-SWEEP skeleton (even sweeps ascending, odd descending —
 * the rowmajor direction axis, a proven SDC-relevant structural
 * variable): the runtime direction select keeps the element train a
 * real per-element vector-copy loop in the binary while leaving every
 * encoding choice (single vs paired, addressing mode, scheduling) to
 * the compiler.  This is the nearest C++-buildable form of the pure
 * copy train that is NOT a libc call.
 *
 * Golden: the source buffer itself (the loop never writes src), same
 * as the asm twin; scalar-clean by construction (no golden arithmetic
 * at all).
 * @endparblock
 */

#include "sandstone.h"
#include <arm_neon.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define P08I_COUNT 1024                /* uint64x2_t elements -> 16KB/buffer */
#define P08I_BYTES (P08I_COUNT * 16)

namespace { /* ODR: per-TU P08IData */
struct P08IData {
    uint64_t *src;                     /* shared, read-only after init */
};
}

static int ldp_stp_cpy_p08_intr_init(struct test *test)
{
    auto *d = new P08IData;
    if (!d)
        return EXIT_FAILURE;

    d->src = static_cast<uint64_t *>(aligned_alloc(32, P08I_BYTES));
    if (!d->src) {
        delete d;
        return EXIT_FAILURE;
    }
    memset_random(d->src, P08I_BYTES);

    test->data = d;
    return EXIT_SUCCESS;
}

static int ldp_stp_cpy_p08_intr_cleanup(struct test *test)
{
    auto *d = static_cast<P08IData *>(test->data);
    if (d) {
        free(d->src);
        delete d;
    }
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int ldp_stp_cpy_p08_intr_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<P08IData *>(test->data);

    /* per-thread destination; the source stays shared read-only */
    auto *dst = static_cast<uint64_t *>(aligned_alloc(32, P08I_BYTES));
    if (!dst) {
        report_fail_msg("ldp_stp_cpy_p08_intr: buffer allocation failed");
        return EXIT_FAILURE;
    }

    uint64_t sweep = 0;
    TEST_LOOP(test, 1 << 12) {
        /* one sweep: full 16KB pure copy, compiler-built (no asm).
         * Alternating direction (champion skeleton): keeps the copy a
         * real per-element vector loop instead of a folded memcpy(). */
        const bool ascending = (sweep & 1) == 0;
        for (int n = 0; n < P08I_COUNT; ++n) {
            const int i = ascending ? n : (P08I_COUNT - 1 - n);
            const uint64x2_t v = vld1q_u64(&d->src[i * 2]);
            vst1q_u64(&dst[i * 2], v);
        }
        sweep++;

        /* cold path: the source is the golden (never written) */
        if (memcmp(dst, d->src, P08I_BYTES) != 0) {
            for (size_t i = 0; i < P08I_COUNT * 2; ++i) {
                if (dst[i] != d->src[i]) {
                    report_fail_msg("ldp_stp_cpy_p08_intr: dst mismatch at u64 "
                                    "%zu: expected 0x%016lx got 0x%016lx "
                                    "(compiler-built pure copy, no asm)",
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

DECLARE_TEST(ldp_stp_cpy_p08_intr, "Intrinsics-built twin of ldp_stp_cpy_p08: same 2x16KB L1-resident pure-copy workload (346-suite SIMDFPLoadStoreMemLoopCpyP08), zero asm — vld1q/vst1q element copy with compiler-chosen encodings in the champion's alternating-sweep skeleton (a monotone copy loop folds to memcpy()), the field-relevant arm of the encoding-discrimination pair (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = ldp_stp_cpy_p08_intr_init,
    .test_run = ldp_stp_cpy_p08_intr_run,
    .test_cleanup = ldp_stp_cpy_p08_intr_cleanup,
    .fracture_loop_count = -1,   /* never fracture: sweep loops are fast */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
