/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0.
 *
 * @test @b arm0102_kreg_mask_vceq
 * @parblock
 * Freeze-narrowing variant of arm0102_kreg_mask (2026-09-07 freeze matrix):
 * kreg_mask FREEZES the machine at >=180 cores while its logic-ALU sibling
 * kreg_logic (identical skeleton) does not — the only variable is the
 * compare-class payload (vceq/vcgt/vtst sparse masks). This variant keeps
 * the skeleton identical but replaces the 4-way compare ROTATION with a
 * single fixed compare (vceq_s32) on all i%4 lanes:
 *   freeze  -> the compare itself (vceq producing sparse 0/-1 masks) is
 *              the necessary condition; rotation irrelevant
 *   no freeze -> the rotation/mix across different compare types is
 *              required (mirrors the scalar ALU-rotation necessity result)
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define ARM0102_KREG_MASK_VCEQ_COUNT 1024   /* uint32x4 elements -> 16KB per buffer */

struct Arm0102KregMaskVceqData {
    uint32x4_t *srcA;
    uint32x4_t *srcB;
    uint32x4_t *expected;
};

#if defined(__aarch64__)

static inline void store_vec(void *addr, uint32x4_t val) {
    vst1q_u32(static_cast<uint32_t *>(addr), val);
}

static inline uint32x4_t load_vec(const void *addr) {
    uint32x4_t res;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

#endif

static int arm0102_kreg_mask_vceq_init(struct test *test) {
    auto *data = static_cast<Arm0102KregMaskVceqData *>(malloc(sizeof(Arm0102KregMaskVceqData)));
    data->srcA = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_KREG_MASK_VCEQ_COUNT * sizeof(uint32x4_t)));
    data->srcB = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_KREG_MASK_VCEQ_COUNT * sizeof(uint32x4_t)));
    data->expected = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_KREG_MASK_VCEQ_COUNT * sizeof(uint32x4_t)));

    memset_random(data->srcA, ARM0102_KREG_MASK_VCEQ_COUNT * sizeof(uint32x4_t));
    memset_random(data->srcB, ARM0102_KREG_MASK_VCEQ_COUNT * sizeof(uint32x4_t));

    /* golden: fixed vceq_s32 on every element — same NEON units */
    for (int i = 0; i < ARM0102_KREG_MASK_VCEQ_COUNT; i++) {
        data->expected[i] = vceqq_s32(vreinterpretq_s32_u32(data->srcA[i]),
                                      vreinterpretq_s32_u32(data->srcB[i]));
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int arm0102_kreg_mask_vceq_run(struct test *test, int cpu) {
    auto *data = static_cast<Arm0102KregMaskVceqData *>(test->data);

    uint32x4_t *temp = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_KREG_MASK_VCEQ_COUNT * sizeof(uint32x4_t)));
    uint32x4_t *dst  = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_KREG_MASK_VCEQ_COUNT * sizeof(uint32x4_t)));

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < ARM0102_KREG_MASK_VCEQ_COUNT; i++) {
            uint32x4_t a = load_vec(&data->srcA[i]);   /* ldr q — source 1 */
            uint32x4_t b = load_vec(&data->srcB[i]);   /* ldr q — source 2 */
            uint32x4_t res;

            /* fixed compare on ALL lanes — no rotation; the ONLY variable
             * vs kreg_mask is compare-mix -> single compare            */
            res = vceqq_s32(vreinterpretq_s32_u32(a), vreinterpretq_s32_u32(b));

            store_vec(&temp[i], res);
            store_vec(&dst[i], load_vec(&temp[i]));     /* back-to-back str->ldr->str */
        }

        if (memcmp(dst, data->expected, ARM0102_KREG_MASK_VCEQ_COUNT * sizeof(uint32x4_t)) != 0) {
            report_fail_msg("arm0102_kreg_mask_vceq data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int arm0102_kreg_mask_vceq_cleanup(struct test *test) {
    auto *data = static_cast<Arm0102KregMaskVceqData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(arm0102_kreg_mask_vceq, "Freeze-narrowing: kreg_mask skeleton with fixed vceq compare (no rotation) — compare-itself vs compare-mix")
    .test_init    = arm0102_kreg_mask_vceq_init,
    .test_run     = arm0102_kreg_mask_vceq_run,
    .test_cleanup = arm0102_kreg_mask_vceq_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
