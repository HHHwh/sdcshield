/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0.
 *
 * @test @b arm0102_fma_f64
 * @parblock
 * SDC trigger-format variant of the fmatail_avx2/fmatail_avx512 family
 * (double FMA, vfmaq_f64). Original computes c + a*b on stack arrays with
 * printf in the hot loop. This variant applies the verified core-179
 * trigger recipe:
 *   - 2 source loads from persistent heap buffers (asm-guaranteed ldr q)
   - the original's load instruction (vfmaq lane-fused-multiply-add, f64)
 *     kept as the payload compute, ROTATED across 4 variants per i%4:
 *       fma / fms / fnma / fnms
 *   - back-to-back vector store -> reload -> store (amplifier)
 * Golden precomputed in init on the same NEON units; memcmp in cold path.
 * Original files untouched.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define ARM0102_FMA_F64_COUNT 1024   /* float64x2 elements -> 16KB per buffer */

struct Arm0102FmaF64Data {
    float64x2_t *srcA;
    float64x2_t *srcB;
    float64x2_t *srcC;
    float64x2_t *expected;
};

#if defined(__aarch64__)

static inline void store_vec(void *addr, float64x2_t val) {
    vst1q_f64(static_cast<double *>(addr), val);
}

static inline float64x2_t load_vec(const void *addr) {
    float64x2_t res;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

#endif

static int arm0102_fma_f64_init(struct test *test) {
    auto *data = static_cast<Arm0102FmaF64Data *>(malloc(sizeof(Arm0102FmaF64Data)));
    data->srcA = static_cast<float64x2_t *>(aligned_alloc(64, ARM0102_FMA_F64_COUNT * sizeof(float64x2_t)));
    data->srcB = static_cast<float64x2_t *>(aligned_alloc(64, ARM0102_FMA_F64_COUNT * sizeof(float64x2_t)));
    data->srcC = static_cast<float64x2_t *>(aligned_alloc(64, ARM0102_FMA_F64_COUNT * sizeof(float64x2_t)));
    data->expected = static_cast<float64x2_t *>(aligned_alloc(64, ARM0102_FMA_F64_COUNT * sizeof(float64x2_t)));

    /* double random: re-roll raw bits into a safe finite range via the
     * framework RNG (keeps auto-seed) */
    for (int i = 0; i < ARM0102_FMA_F64_COUNT; i++) {
        double *pa = reinterpret_cast<double *>(&data->srcA[i]);
        double *pb = reinterpret_cast<double *>(&data->srcB[i]);
        double *pc = reinterpret_cast<double *>(&data->srcC[i]);
        for (int l = 0; l < 2; l++) {
            pa[l] = frandom_scale(1e10);
            pb[l] = frandom_scale(1e10);
            pc[l] = frandom_scale(1e10);
        }
    }

    /* golden: same rotating composition, computed on the same NEON units */
    for (int i = 0; i < ARM0102_FMA_F64_COUNT; i++) {
        switch (i % 4) {
            case 0: data->expected[i] = vfmaq_f64(data->srcC[i], data->srcA[i], data->srcB[i]); break;
            case 1: data->expected[i] = vfmsq_f64(data->srcC[i], data->srcA[i], data->srcB[i]); break;
            case 2: data->expected[i] = vnegq_f64(vfmsq_f64(vnegq_f64(data->srcC[i]), data->srcA[i], data->srcB[i])); break;
            case 3: data->expected[i] = vnegq_f64(vfmaq_f64(vnegq_f64(data->srcC[i]), data->srcA[i], data->srcB[i])); break;
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int arm0102_fma_f64_run(struct test *test, int cpu) {
    auto *data = static_cast<Arm0102FmaF64Data *>(test->data);

    float64x2_t *temp = static_cast<float64x2_t *>(aligned_alloc(64, ARM0102_FMA_F64_COUNT * sizeof(float64x2_t)));
    float64x2_t *dst  = static_cast<float64x2_t *>(aligned_alloc(64, ARM0102_FMA_F64_COUNT * sizeof(float64x2_t)));

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < ARM0102_FMA_F64_COUNT; i++) {
            float64x2_t a = load_vec(&data->srcA[i]);   /* ldr q — source 1 */
            float64x2_t b = load_vec(&data->srcB[i]);   /* ldr q — source 2 */
            float64x2_t c = load_vec(&data->srcC[i]);   /* ldr q — addend */
            float64x2_t res;

            switch (i % 4) {                            /* payload rotation */
                case 0: res = vfmaq_f64(c, a, b); break;   /*  c + a*b  (fmatail*) */
                case 1: res = vfmsq_f64(c, a, b); break;   /*  c - a*b  */
                case 2: res = vnegq_f64(vfmsq_f64(vnegq_f64(c), a, b)); break; /* -c + a*b */
                case 3: res = vnegq_f64(vfmaq_f64(vnegq_f64(c), a, b)); break; /* -c - a*b */
            }

            store_vec(&temp[i], res);
            store_vec(&dst[i], load_vec(&temp[i]));     /* back-to-back str->ldr->str */
        }

        if (memcmp(dst, data->expected, ARM0102_FMA_F64_COUNT * sizeof(float64x2_t)) != 0) {
            report_fail_msg("arm0102_fma_f64 data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int arm0102_fma_f64_cleanup(struct test *test) {
    auto *data = static_cast<Arm0102FmaF64Data *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->srcC);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(arm0102_fma_f64, "arm-0102 variant of fmatail family: 2-src vec load + rotating vfmaq{,s,nma,nms}_f64 + store/reload/store")
    .test_init    = arm0102_fma_f64_init,
    .test_run     = arm0102_fma_f64_run,
    .test_cleanup = arm0102_fma_f64_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
