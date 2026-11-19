/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0.
 *
 * @test @b arm0102_kreg_not
 * @parblock
 * SDC trigger-format variant of tests/cpu/vector/kreg4.cpp (SIMD mask
 * operations with NOT / mvn semantics: and + bitwise-negate). Original
 * computes on stack arrays with printf in the hot loop. This variant
 * applies the verified core-179 trigger recipe:
 *   - 2 source loads from persistent heap buffers (asm-guaranteed ldr q)
 *   - payload = the original's mvn-based ops, ROTATED across 4 variants
 *     per i%4:  and / or / eor / bic each followed by vmvnq (the NEON
 *     bitwise-NOT the original relies on)
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

#define ARM0102_KREG_NOT_COUNT 1024   /* uint32x4 elements -> 16KB per buffer */

struct Arm0102KregNotData {
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

static int arm0102_kreg_not_init(struct test *test) {
    auto *data = static_cast<Arm0102KregNotData *>(malloc(sizeof(Arm0102KregNotData)));
    data->srcA = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_KREG_NOT_COUNT * sizeof(uint32x4_t)));
    data->srcB = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_KREG_NOT_COUNT * sizeof(uint32x4_t)));
    data->expected = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_KREG_NOT_COUNT * sizeof(uint32x4_t)));

    memset_random(data->srcA, ARM0102_KREG_NOT_COUNT * sizeof(uint32x4_t));
    memset_random(data->srcB, ARM0102_KREG_NOT_COUNT * sizeof(uint32x4_t));

    /* golden: same rotating composition, computed on the same NEON units */
    for (int i = 0; i < ARM0102_KREG_NOT_COUNT; i++) {
        switch (i % 4) {
            case 0: data->expected[i] = vmvnq_u32(vandq_u32(data->srcA[i], data->srcB[i])); break; /* nand */
            case 1: data->expected[i] = vmvnq_u32(vorrq_u32(data->srcA[i], data->srcB[i])); break; /* nor  */
            case 2: data->expected[i] = vmvnq_u32(veorq_u32(data->srcA[i], data->srcB[i])); break; /* xnor */
            case 3: data->expected[i] = vmvnq_u32(vbicq_u32(data->srcA[i], data->srcB[i])); break; /* nandnot */
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int arm0102_kreg_not_run(struct test *test, int cpu) {
    auto *data = static_cast<Arm0102KregNotData *>(test->data);

    uint32x4_t *temp = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_KREG_NOT_COUNT * sizeof(uint32x4_t)));
    uint32x4_t *dst  = static_cast<uint32x4_t *>(aligned_alloc(64, ARM0102_KREG_NOT_COUNT * sizeof(uint32x4_t)));

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < ARM0102_KREG_NOT_COUNT; i++) {
            uint32x4_t a = load_vec(&data->srcA[i]);   /* ldr q — source 1 */
            uint32x4_t b = load_vec(&data->srcB[i]);   /* ldr q — source 2 */
            uint32x4_t res;

            switch (i % 4) {                            /* payload rotation */
                case 0: res = vmvnq_u32(vandq_u32(a, b)); break;  /* nand (kreg4) */
                case 1: res = vmvnq_u32(vorrq_u32(a, b)); break;  /* nor         */
                case 2: res = vmvnq_u32(veorq_u32(a, b)); break;  /* xnor        */
                case 3: res = vmvnq_u32(vbicq_u32(a, b)); break;  /* nandnot     */
            }

            store_vec(&temp[i], res);
            store_vec(&dst[i], load_vec(&temp[i]));     /* back-to-back str->ldr->str */
        }

        if (memcmp(dst, data->expected, ARM0102_KREG_NOT_COUNT * sizeof(uint32x4_t)) != 0) {
            report_fail_msg("arm0102_kreg_not data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int arm0102_kreg_not_cleanup(struct test *test) {
    auto *data = static_cast<Arm0102KregNotData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(arm0102_kreg_not, "arm-0102 variant of kreg4.cpp: 2-src vec load + rotating vmvn(and/or/eor/bic) + store/reload/store")
    .test_init    = arm0102_kreg_not_init,
    .test_run     = arm0102_kreg_not_run,
    .test_cleanup = arm0102_kreg_not_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
