/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b arm0102_fma_f32_sve
 * @parblock
 * SVE port of arm0102_fma_f32 (source: missing_testcases_20261009/
 * tests/cpu/arm-0102/arm0102_fma_f32.cpp; the fma.cpp branch of the
 * core-179 trigger recipe).
 *
 * Rotating fused multiply-add on f32 lanes with THREE source loads
 * (a, b, addend c — all asm-guaranteed ldr z; frandomf_scale(100.0f)
 * lane fill so no NaN/Inf):
 *   fma  — vfmaq(c, a, b)          = c + a*b   -> svmla_f32_x(c, a, b)
 *   fms  — vfmsq(c, a, b)          = c - a*b   -> svmsb_f32_x(c, a, b)
 *   fnma — vneg(vfms(vneg(c),a,b)) = c + a*b   (nested, kept verbatim)
 *   fnms — vneg(vfma(vneg(c),a,b)) = c - a*b   (nested, kept verbatim)
 * The fnma/fnms cases are numerically equal to fma/fms but traverse a
 * different instruction sequence (neg + fma + neg) — that is the
 * original's datapath-probing design, preserved.
 *
 * pg4 sub-block predicates preserve the 4-lane NEON semantics; +8-
 * element buffer slack covers the asm full-VL ldr z at the final
 * sub-block (documented deviation from the mask_sve template).
 * Back-to-back str z -> ldr z -> str z amplifier.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <sys/auxv.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif
#endif

#define ARM0102_FMA_F32_SVE_COUNT 1024
#define ARM0102_FMA_F32_SVE_SLACK 8

struct Arm0102FmaF32SveData {
    float *srcA;
    float *srcB;
    float *srcC;
    float *expected;
};

#if defined(__aarch64__)

#define FMA_SVE_SUBLANES 4

static inline void store_vec_sve(void *addr, svfloat32_t val) {
    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)FMA_SVE_SUBLANES);
    svst1_f32(pg, static_cast<float *>(addr), val);
}

static inline svfloat32_t load_vec_sve(const void *addr) {
    svfloat32_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

/* 旋转 FMA 载荷 (相位 i%4); vfmaq(d,a,b)=d+a*b, vfmsq=d-a*b, vnegq 取反 */
static inline svfloat32_t fma_rotating_sve(svbool_t pg, svfloat32_t a, svfloat32_t b,
                                           svfloat32_t c, int phase)
{
    switch (phase % 4) {
        case 0: return svmla_f32_x(pg, c, a, b);                                   /* fma  */
        case 1: return svmsb_f32_x(pg, c, a, b);                                   /* fms  */
        case 2: return svneg_f32_x(pg, svmsb_f32_x(pg, svneg_f32_x(pg, c), a, b)); /* fnma */
        default: return svneg_f32_x(pg, svmla_f32_x(pg, svneg_f32_x(pg, c), a, b));/* fnms */
    }
}

#endif

static int arm0102_fma_f32_sve_init(struct test *test) {
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for arm0102_fma_f32_sve");
        return EXIT_SKIP;
    }

    const size_t total = (size_t)(ARM0102_FMA_F32_SVE_COUNT + ARM0102_FMA_F32_SVE_SLACK) * sizeof(float);
    auto *data = static_cast<Arm0102FmaF32SveData *>(malloc(sizeof(Arm0102FmaF32SveData)));
    data->srcA = static_cast<float *>(aligned_alloc(64, total));
    data->srcB = static_cast<float *>(aligned_alloc(64, total));
    data->srcC = static_cast<float *>(aligned_alloc(64, total));
    data->expected = static_cast<float *>(aligned_alloc(64, total));

    /* 逐 lane 缩放随机 (raw bits 会出 NaN/Inf — 原版语义) */
    for (int i = 0; i < ARM0102_FMA_F32_SVE_COUNT; i++) {
        data->srcA[i] = frandomf_scale(100.0f);
        data->srcB[i] = frandomf_scale(100.0f);
        data->srcC[i] = frandomf_scale(100.0f);
    }

    /* golden: 与 run 相同的子块结构 + 相位 (base+sub)%4, 同 SVE 单元 */
    const int lanes = svcntw();
    svbool_t pg4 = svwhilelt_b32((uint64_t)0, (uint64_t)FMA_SVE_SUBLANES);
    for (int base = 0; base < ARM0102_FMA_F32_SVE_COUNT; base += lanes) {
        const int n = (ARM0102_FMA_F32_SVE_COUNT - base < lanes)
                          ? ARM0102_FMA_F32_SVE_COUNT - base : lanes;
        for (int sub = 0; sub < n; sub += FMA_SVE_SUBLANES) {
            const int phase = (base + sub) % 4;
            float av[8], bv[8], cv[8], ov[8];
            for (int k = 0; k < FMA_SVE_SUBLANES && sub + k < n; ++k) {
                av[k] = data->srcA[base + sub + k];
                bv[k] = data->srcB[base + sub + k];
                cv[k] = data->srcC[base + sub + k];
            }
            svfloat32_t va = svld1_f32(pg4, av), vb = svld1_f32(pg4, bv), vc = svld1_f32(pg4, cv);
            svfloat32_t res = fma_rotating_sve(pg4, va, vb, vc, phase);
            svst1_f32(pg4, ov, res);
            for (int k = 0; k < FMA_SVE_SUBLANES && sub + k < n; ++k)
                data->expected[base + sub + k] = ov[k];
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for arm0102_fma_f32_sve");
    return EXIT_SKIP;
#endif
}

#if defined(__aarch64__)
static int arm0102_fma_f32_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<Arm0102FmaF32SveData *>(test->data);

    float *temp = static_cast<float *>(aligned_alloc(64, (size_t)(ARM0102_FMA_F32_SVE_COUNT + ARM0102_FMA_F32_SVE_SLACK) * sizeof(float)));
    float *dst  = static_cast<float *>(aligned_alloc(64, (size_t)(ARM0102_FMA_F32_SVE_COUNT + ARM0102_FMA_F32_SVE_SLACK) * sizeof(float)));

    TEST_LOOP(test, 1 << 13) {
        for (int base = 0; base < ARM0102_FMA_F32_SVE_COUNT; base += svcntw()) {
            const int n = (ARM0102_FMA_F32_SVE_COUNT - base < svcntw())
                              ? ARM0102_FMA_F32_SVE_COUNT - base : svcntw();
            for (int sub = 0; sub < n; sub += FMA_SVE_SUBLANES) {
                /* 3 源加载 (a, b, 加数 c): asm 保证的 ldr z */
                svfloat32_t a = load_vec_sve(data->srcA + base + sub);
                svfloat32_t b = load_vec_sve(data->srcB + base + sub);
                svfloat32_t c = load_vec_sve(data->srcC + base + sub);
                svbool_t pg4 = svwhilelt_b32((uint64_t)0, (uint64_t)FMA_SVE_SUBLANES);

                svfloat32_t res = fma_rotating_sve(pg4, a, b, c, base + sub);

                store_vec_sve(temp + base + sub, res);
                store_vec_sve(dst + base + sub, load_vec_sve(temp + base + sub));
            }
        }

        if (memcmp(dst, data->expected, (size_t)ARM0102_FMA_F32_SVE_COUNT * sizeof(float)) != 0) {
            report_fail_msg("arm0102_fma_f32_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int arm0102_fma_f32_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for arm0102_fma_f32_sve");
    return EXIT_SKIP;
}
#endif

static int arm0102_fma_f32_sve_cleanup(struct test *test) {
    auto *data = static_cast<Arm0102FmaF32SveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->srcC);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(arm0102_fma_f32_sve,
             "SVE port of arm-0102 fma variant: 3-src ldr z (a, b, addend c) + rotating "
             "fma/fms/fnma/fnms on f32 lanes (neg-nested sequences kept verbatim) + "
             "str z/ldr z/str z amplifier on pg4 sub-blocks")
    .test_init    = arm0102_fma_f32_sve_init,
    .test_run     = arm0102_fma_f32_sve_run,
    .test_cleanup = arm0102_fma_f32_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
