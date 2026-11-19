/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b arm0102_fma_f32
 * @parblock
 * SDC trigger-format variant of tests/cpu/fma/fma.cpp (float FMA, vfmaq_f32).
 * Original computes c + a*b on freshly-generated stack arrays with printf in
 * the hot loop. This variant applies the verified core-179 trigger recipe:
 *   - 2 source loads from persistent heap buffers (asm-guaranteed ldr q)
 *   - the original's load instruction (vfmaq lane-fused-multiply-add) kept
 *     as the payload compute, ROTATED across 4 variants per i%4:
 *       fma(a,b,c) / fms / fnma / fnms  (mixed op rotation, per
 *     alu-instruction-rotation-required: rotation/mixing is REQUIRED)
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

#define ARM0102_FMA_F32_COUNT 1024   /* float32x4 elements -> 16KB per buffer */

struct Arm0102FmaF32Data {
    float32x4_t *srcA;
    float32x4_t *srcB;
    float32x4_t *srcC;
    float32x4_t *expected;
};

#if defined(__aarch64__)

/* reload via inline asm — REQUIRED: the compiler eliminates intrinsic
 * reloads (reuses the stored register, no ldr emitted). The asm boundary
 * makes the back-to-back str q -> ldr q pair guaranteed in the binary. */
static inline void store_vec(void *addr, float32x4_t val) {
    vst1q_f32(static_cast<float *>(addr), val);
}

static inline float32x4_t load_vec(const void *addr) {
    float32x4_t res;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

#endif

static int arm0102_fma_f32_init(struct test *test) {
    auto *data = static_cast<Arm0102FmaF32Data *>(malloc(sizeof(Arm0102FmaF32Data)));
    data->srcA = static_cast<float32x4_t *>(aligned_alloc(64, ARM0102_FMA_F32_COUNT * sizeof(float32x4_t)));
    data->srcB = static_cast<float32x4_t *>(aligned_alloc(64, ARM0102_FMA_F32_COUNT * sizeof(float32x4_t)));
    data->srcC = static_cast<float32x4_t *>(aligned_alloc(64, ARM0102_FMA_F32_COUNT * sizeof(float32x4_t)));
    data->expected = static_cast<float32x4_t *>(aligned_alloc(64, ARM0102_FMA_F32_COUNT * sizeof(float32x4_t)));

    memset_random(data->srcA, ARM0102_FMA_F32_COUNT * sizeof(float32x4_t));
    memset_random(data->srcB, ARM0102_FMA_F32_COUNT * sizeof(float32x4_t));
    memset_random(data->srcC, ARM0102_FMA_F32_COUNT * sizeof(float32x4_t));

    /* float random can produce NaN/Inf from raw memset_random bits; re-roll
     * into a safe finite range via the framework RNG (keeps auto-seed) */
    for (int i = 0; i < ARM0102_FMA_F32_COUNT; i++) {
        float *pa = reinterpret_cast<float *>(&data->srcA[i]);
        float *pb = reinterpret_cast<float *>(&data->srcB[i]);
        float *pc = reinterpret_cast<float *>(&data->srcC[i]);
        for (int l = 0; l < 4; l++) {
            pa[l] = frandomf_scale(100.0f);
            pb[l] = frandomf_scale(100.0f);
            pc[l] = frandomf_scale(100.0f);
        }
    }

    /* golden: same rotating composition, computed on the same NEON units */
    for (int i = 0; i < ARM0102_FMA_F32_COUNT; i++) {
        switch (i % 4) {
            case 0: data->expected[i] = vfmaq_f32(data->srcC[i], data->srcA[i], data->srcB[i]); break;
            case 1: data->expected[i] = vfmsq_f32(data->srcC[i], data->srcA[i], data->srcB[i]); break;
            case 2: data->expected[i] = vnegq_f32(vfmsq_f32(vnegq_f32(data->srcC[i]), data->srcA[i], data->srcB[i])); break;
            case 3: data->expected[i] = vnegq_f32(vfmaq_f32(vnegq_f32(data->srcC[i]), data->srcA[i], data->srcB[i])); break;
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int arm0102_fma_f32_run(struct test *test, int cpu) {
    auto *data = static_cast<Arm0102FmaF32Data *>(test->data);

    float32x4_t *temp = static_cast<float32x4_t *>(aligned_alloc(64, ARM0102_FMA_F32_COUNT * sizeof(float32x4_t)));
    float32x4_t *dst  = static_cast<float32x4_t *>(aligned_alloc(64, ARM0102_FMA_F32_COUNT * sizeof(float32x4_t)));

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < ARM0102_FMA_F32_COUNT; i++) {
            float32x4_t a = load_vec(&data->srcA[i]);   /* ldr q — source 1 */
            float32x4_t b = load_vec(&data->srcB[i]);   /* ldr q — source 2 */
            float32x4_t c = load_vec(&data->srcC[i]);   /* ldr q — addend */
            float32x4_t res;

            switch (i % 4) {                            /* payload rotation */
                case 0: res = vfmaq_f32(c, a, b); break;   /*  c + a*b  (fma.cpp) */
                case 1: res = vfmsq_f32(c, a, b); break;   /*  c - a*b  */
                case 2: res = vnegq_f32(vfmsq_f32(vnegq_f32(c), a, b)); break; /* -c + a*b */
                case 3: res = vnegq_f32(vfmaq_f32(vnegq_f32(c), a, b)); break; /* -c - a*b */
            }

            store_vec(&temp[i], res);
            store_vec(&dst[i], load_vec(&temp[i]));     /* back-to-back str->ldr->str */
        }

        if (memcmp(dst, data->expected, ARM0102_FMA_F32_COUNT * sizeof(float32x4_t)) != 0) {
            report_fail_msg("arm0102_fma_f32 data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int arm0102_fma_f32_cleanup(struct test *test) {
    auto *data = static_cast<Arm0102FmaF32Data *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->srcC);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(arm0102_fma_f32, "arm-0102 variant of fma.cpp: 2-src vec load + rotating vfmaq{,s,nma,nms}_f32 + store/reload/store")
    .test_init    = arm0102_fma_f32_init,
    .test_run     = arm0102_fma_f32_run,
    .test_cleanup = arm0102_fma_f32_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
