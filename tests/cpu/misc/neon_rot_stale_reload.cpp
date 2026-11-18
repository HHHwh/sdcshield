/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_stale_reload
 * @parblock
 * Window time-distance probe (plan ① of the 2026-09-04 attribution split):
 *   baseline:      ALU -> str temp[i]   -> ldr temp[i]   -> str dst[i]
 *   stale_reload:  ALU -> str temp[i]   -> ldr temp[i-1] -> str dst[i]
 * The load reads the PREVIOUS element of the same temp buffer (same cache
 * line for i%4!=0), so the load's matching store is ~6 instructions back
 * instead of back-to-back. Everything else is preserved vs baseline:
 * instruction count, str+ldr+str mixed AGU pressure, address set (temp is
 * touched twice), and — per the new skeleton spec — the 4-step dependency
 * chain: str dst[i] consumes the ldr's OUTPUT register (unlike fwd_probe,
 * which discarded the loaded value and broke the chain).
 * Single variable: load-to-matching-store time distance.
 *   fires  -> same-address property is NOT the amplifier; time distance is
 *   0-fail -> the back-to-back tightness is the amplifier
 * Golden: dst[i] = expected[i-1] (rotated by one); dst[0] = preload value
 * (temp is prefilled with the ALU golden so first-pass loads read valid
 * data; every later pass rewrites identical values).
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define NEON_ROT_STALE_RELOAD_COUNT 1024

struct NeonRotStaleReloadData {
    uint64x2_t *srcA;
    uint64x2_t *srcB;
    uint64x2_t *srcref;    /* ALU golden, used to prefill temp */
    uint64x2_t *expected;  /* dst golden: rotated by one */
};

#if defined(__aarch64__)

static inline uint64x2_t load_vec(const void *addr) {
    uint64x2_t res;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

#endif

static int neon_rot_stale_reload_init(struct test *test) {
    auto *data = static_cast<NeonRotStaleReloadData *>(malloc(sizeof(NeonRotStaleReloadData)));
    if (!data) {
        log_skip(TestResourceIssueSkipCategory,
                 "neon_rot_stale_reload: data alloc failed");
        return EXIT_SKIP;
    }
    data->srcA = static_cast<uint64x2_t *>(aligned_alloc_safe(64, NEON_ROT_STALE_RELOAD_COUNT * sizeof(uint64x2_t)));
    data->srcB = static_cast<uint64x2_t *>(aligned_alloc_safe(64, NEON_ROT_STALE_RELOAD_COUNT * sizeof(uint64x2_t)));
    data->srcref = static_cast<uint64x2_t *>(aligned_alloc_safe(64, NEON_ROT_STALE_RELOAD_COUNT * sizeof(uint64x2_t)));
    data->expected = static_cast<uint64x2_t *>(aligned_alloc_safe(64, NEON_ROT_STALE_RELOAD_COUNT * sizeof(uint64x2_t)));
    if (!data->srcA || !data->srcB || !data->srcref || !data->expected) {
        free(data->srcA);
        free(data->srcB);
        free(data->srcref);
        free(data->expected);
        free(data);
        log_skip(TestResourceIssueSkipCategory,
                 "neon_rot_stale_reload: buffer alloc failed");
        return EXIT_SKIP;
    }

    memset_random(data->srcA, NEON_ROT_STALE_RELOAD_COUNT * sizeof(uint64x2_t));
    memset_random(data->srcB, NEON_ROT_STALE_RELOAD_COUNT * sizeof(uint64x2_t));

    /* golden (issue #373): scalar independent recomputation — pure C integer
     * ops on the scalar pipeline, never the NEON units under test. A persistent
     * NEON datapath fault (stuck lane / unit computing deterministic wrong
     * bits) can no longer corrupt golden and actual identically; it now
     * surfaces as a golden-vs-actual byte difference at the memcmp.
     * Boundary note: the rotation copy below (ex[e]=rf[e-2]) is pure data
     * movement — GCC may emit ldr/str q for it (verified in the binary).
     * That is not golden COMPUTATION: a load/store-path fault hits it and
     * the run-side fresh ALU recompute differently, so the miscompare still
     * fires; only the ALU op semantics needed to be scalar. */
    {
        const uint64_t *sa = reinterpret_cast<const uint64_t *>(data->srcA);
        const uint64_t *sb = reinterpret_cast<const uint64_t *>(data->srcB);
        uint64_t *rf = reinterpret_cast<uint64_t *>(data->srcref);
        uint64_t *ex = reinterpret_cast<uint64_t *>(data->expected);
        for (int e = 0; e < 2 * NEON_ROT_STALE_RELOAD_COUNT; e++) {
            switch ((e / 2) % 4) {   /* op phase: vector index % 4 */
                case 0: rf[e] = sa[e] + sb[e]; break;
                case 1: rf[e] = sa[e] ^ sb[e]; break;
                case 2: rf[e] = sa[e] & sb[e]; break;
                case 3: rf[e] = sa[e] | sb[e]; break;
            }
        }

        /* dst golden: rotated by one; dst[0] = preload (= srcref[0], arbitrary
         * but consistent with temp_buf[0] prefill in run). One vector = two
         * scalar elements. */
        ex[0] = rf[0];
        ex[1] = rf[1];
        for (int e = 2; e < 2 * NEON_ROT_STALE_RELOAD_COUNT; e++)
            ex[e] = rf[e - 2];
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int neon_rot_stale_reload_run(struct test *test, int cpu) {
    auto *data = static_cast<NeonRotStaleReloadData *>(test->data);

    /* one leading element so &temp[-1] is valid for i=0; aligned_alloc_safe
     * rounds the (COUNT+1)*16 = 16400-byte request up to a 64-byte multiple —
     * C11 requires size to be a multiple of alignment (glibc tolerates a
     * violation, ASan aborts on it) */
    uint64x2_t *temp_buf = static_cast<uint64x2_t *>(aligned_alloc_safe(64, (NEON_ROT_STALE_RELOAD_COUNT + 1) * sizeof(uint64x2_t)));
    uint64x2_t *dst = static_cast<uint64x2_t *>(aligned_alloc_safe(64, NEON_ROT_STALE_RELOAD_COUNT * sizeof(uint64x2_t)));
    if (!temp_buf || !dst) {
        free(temp_buf);
        free(dst);
        log_skip(TestResourceIssueSkipCategory,
                 "neon_rot_stale_reload: run buffer alloc failed");
        return EXIT_SKIP;
    }
    uint64x2_t *temp = temp_buf + 1;

    /* prefill: first-pass loads read valid data; res always rewrites the
     * same values, so every pass is self-consistent */
    temp_buf[0] = data->expected[0];
    memcpy(temp, data->srcref, NEON_ROT_STALE_RELOAD_COUNT * sizeof(uint64x2_t));

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < NEON_ROT_STALE_RELOAD_COUNT; i++) {
            uint64x2_t a = load_vec(&data->srcA[i]);
            uint64x2_t b = load_vec(&data->srcB[i]);
            uint64x2_t res;

            switch (i % 4) {
                case 0: res = vaddq_u64(a, b); break;
                case 1: res = veorq_u64(a, b); break;
                case 2: res = vandq_u64(a, b); break;
                case 3: res = vorrq_u64(a, b); break;
            }

            /* single asm block, exact order:
             *   str temp[i] = res        (same as baseline)
             *   ldr loaded = temp[i-1]   (baseline reads temp[i]: the ONLY
             *                             variable = time distance)
             *   str dst[i] = loaded      (final store EATS the ldr output:
             *                             4-step chain preserved — verified
             *                             via register identity in objdump) */
            uint64x2_t loaded;
            __asm__ volatile (
                "str %q1, [%2]  \n\t"
                "ldr %q0, [%3]  \n\t"
                "str %q0, [%4]  \n\t"
                : "=&w"(loaded)
                : "w"(res), "r"(&temp[i]), "r"(&temp[i-1]), "r"(&dst[i])
                : "memory");
        }

        if (memcmp(dst, data->expected, NEON_ROT_STALE_RELOAD_COUNT * sizeof(uint64x2_t)) != 0) {
            report_fail_msg("neon_rot_stale_reload data miscompare");
        }
    }

    free(dst);
    free(temp_buf);
    return EXIT_SUCCESS;
}

static int neon_rot_stale_reload_cleanup(struct test *test) {
    auto *data = static_cast<NeonRotStaleReloadData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->srcref);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_stale_reload, "NEON window-distance probe: ldr reads temp[i-1] (chain/mix/count kept, only load-to-store distance varies)")
    .test_init    = neon_rot_stale_reload_init,
    .test_run     = neon_rot_stale_reload_run,
    .test_cleanup = neon_rot_stale_reload_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
