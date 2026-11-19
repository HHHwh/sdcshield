/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0.
 *
 * @test @b arm0102_ins_ext
 * @parblock
 * SDC trigger-format variant of tests/cpu/vector/insert_extract.cpp
 * (SIMD lane insert/extract). Original builds one vector on the stack
 * and extracts a lane, with printf in the hot loop. This variant applies
 * the verified core-179 trigger recipe:
 *   - 2 source loads from persistent heap buffers (asm-guaranteed ldr q)
 *   - payload = insert/extract semantics, ROTATED across 4 per i%4:
 *       ins lane0(scratch,B) / ins lane2 / extract-compare via vceq
 *       (insert of a rotated source lane) / ins of B into A lane1
 *   - back-to-back vector store -> reload -> store (amplifier)
 * Golden precomputed in init on the same NEON units; memcmp in cold path.
 * Original file untouched.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define ARM0102_INS_EXT_COUNT 1024   /* uint32x4 elements -> 16KB per buffer */

struct Arm0102InsExtData {
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

static int arm0102_ins_ext_init(struct test *test) {
    auto *data = static_cast<Arm0102InsExtData *>(malloc(sizeof(Arm0102InsExtData)));
    data->srcA = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_INS_EXT_COUNT * sizeof(uint32x4_t)));
    data->srcB = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_INS_EXT_COUNT * sizeof(uint32x4_t)));
    data->expected = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_INS_EXT_COUNT * sizeof(uint32x4_t)));

    memset_random(data->srcA, ARM0102_INS_EXT_COUNT * sizeof(uint32x4_t));
    memset_random(data->srcB, ARM0102_INS_EXT_COUNT * sizeof(uint32x4_t));

    /* golden: same rotating composition, computed on the same NEON units.
     * vsetq_lane with a runtime lane is not allowed, so inserts use fixed
     * lanes; the "extract" variant uses vaddv-style lane reduce via vceq. */
    for (int i = 0; i < ARM0102_INS_EXT_COUNT; i++) {
        uint32x4_t a = data->srcA[i];
        uint32x4_t b = data->srcB[i];
        uint32_t bl0 = vgetq_lane_u32(b, 0);
        uint32_t bl2 = vgetq_lane_u32(b, 2);
        uint32_t al3 = vgetq_lane_u32(a, 3);
        switch (i % 4) {
            case 0: data->expected[i] = vsetq_lane_u32(bl0, a, 0); break;  /* ins B[0] -> A lane0 */
            case 1: data->expected[i] = vsetq_lane_u32(bl2, a, 2); break;  /* ins B[2] -> A lane2 */
            case 2: data->expected[i] = vsetq_lane_u32(al3, a, 1); break;  /* self-extract-reinsert */
            case 3: data->expected[i] = vsetq_lane_u32(bl0 ^ bl2, a, 3); break; /* mixed insert */
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int arm0102_ins_ext_run(struct test *test, int cpu) {
    auto *data = static_cast<Arm0102InsExtData *>(test->data);

    uint32x4_t *temp = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_INS_EXT_COUNT * sizeof(uint32x4_t)));
    uint32x4_t *dst  = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_INS_EXT_COUNT * sizeof(uint32x4_t)));

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < ARM0102_INS_EXT_COUNT; i++) {
            uint32x4_t a = load_vec(&data->srcA[i]);   /* ldr q — source 1 */
            uint32x4_t b = load_vec(&data->srcB[i]);   /* ldr q — source 2 */
            uint32x4_t res;

            uint32_t bl0 = vgetq_lane_u32(b, 0);       /* extract (insert_extract.cpp) */
            uint32_t bl2 = vgetq_lane_u32(b, 2);
            uint32_t al3 = vgetq_lane_u32(a, 3);
            switch (i % 4) {                            /* payload rotation */
                case 0: res = vsetq_lane_u32(bl0, a, 0); break;
                case 1: res = vsetq_lane_u32(bl2, a, 2); break;
                case 2: res = vsetq_lane_u32(al3, a, 1); break;
                case 3: res = vsetq_lane_u32(bl0 ^ bl2, a, 3); break;
            }

            store_vec(&temp[i], res);
            store_vec(&dst[i], load_vec(&temp[i]));     /* back-to-back str->ldr->str */
        }

        if (memcmp(dst, data->expected, ARM0102_INS_EXT_COUNT * sizeof(uint32x4_t)) != 0) {
            report_fail_msg("arm0102_ins_ext data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int arm0102_ins_ext_cleanup(struct test *test) {
    auto *data = static_cast<Arm0102InsExtData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(arm0102_ins_ext, "arm-0102 variant of insert_extract.cpp: 2-src vec load + rotating lane insert/extract + store/reload/store")
    .test_init    = arm0102_ins_ext_init,
    .test_run     = arm0102_ins_ext_run,
    .test_cleanup = arm0102_ins_ext_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
