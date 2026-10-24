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
 * pg4 sub-block predicates preserve the 4-lane NEON semantics;
 * svcntw() elements (runtime VL) of buffer slack cover the asm full-VL
 * ldr z at the final sub-block (documented deviation from the mask_sve
 * template). Back-to-back str z -> ldr z -> str z amplifier.
 * The element count (originally ARM0102_FMA_F32_SVE_COUNT=1024) is
 * runtime configurable via -O arm0102_fma_f32_sve.count=N /
 * SDC_ARM0102_FMA_F32_SVE_COUNT, with a memory-adaptive default when
 * unconfigured.
 * @endparblock
 */

#include "sandstone.h"
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

#define ARM0102_FMA_F32_SVE_COUNT 1024
/* 尾部 asm 全宽读松弛 = 运行时 svcntw(): 最末子块 ldr z 读到
 * (COUNT-1)+svcntw() 个元素, slack=svcntw() 严格充分 (任意 VL,
 * movbe_slf_sweep VL-portable 范式) */

struct Arm0102FmaF32SveData {
    float *srcA;
    float *srcB;
    float *srcC;
    float *expected;
    int count;               /* 运行时元素数 (≤64M, int 足够) */
};

#if defined(__aarch64__)

/* 元素数 ARM0102_FMA_F32_SVE_COUNT(1024) — 运行时可配置, 统一优先级链:
 *   1. -O arm0102_fma_f32_sve.count=N / 配置文件同名条目
 *   2. 环境变量 SDC_ARM0102_FMA_F32_SVE_COUNT
 *   3. 自适应: srcA/srcB/srcC/expected(共享)+每线程 temp/dst, 6×4B,
 *      占物理内存 60%, clamp [1024, 64M]。pg4 子块/相位/尾部松弛不变。 */
static size_t arm0102_fma_f32_sve_count(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "count", 0, ARM0102_FMA_F32_SVE_COUNT, 64 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("arm0102_fma_f32_sve", "count", &v, ARM0102_FMA_F32_SVE_COUNT, 64 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count(4, 6, 1.0, 600, thread_count(),
                                         ARM0102_FMA_F32_SVE_COUNT, 64 << 20, test->id, "count");
}

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
    /* All-VL Phase 2 P6: SVE capability + 128..1024-bit VL range
     * gate in one framework-layer call (device/cpu/arm64/arm64_vl.h). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    const int lanes = (int)(svl / 4);   /* f32 lanes = SVL bytes / 4 */
    if (lanes < 4 || lanes > 32) {
        log_error("%s: f32 lanes %d outside [4,32] — unit drift (svl %ld bytes)",
                  test->id, lanes, svl);
        return EXIT_FAILURE;
    }
    const int slack = lanes;   /* asm ldr z 尾批松弛, 见文件头注释 */
    const int count = (int)arm0102_fma_f32_sve_count(test);
    const size_t total = (size_t)(count + slack) * sizeof(float);
    auto *data = static_cast<Arm0102FmaF32SveData *>(malloc(sizeof(Arm0102FmaF32SveData)));
    data->count = count;
    data->srcA = static_cast<float *>(aligned_alloc_safe(64, total));
    data->srcB = static_cast<float *>(aligned_alloc_safe(64, total));
    data->srcC = static_cast<float *>(aligned_alloc_safe(64, total));
    data->expected = static_cast<float *>(aligned_alloc_safe(64, total));

    /* 逐 lane 缩放随机 (raw bits 会出 NaN/Inf — 原版语义) */
    for (int i = 0; i < count; i++) {
        data->srcA[i] = frandomf_scale(100.0f);
        data->srcB[i] = frandomf_scale(100.0f);
        data->srcC[i] = frandomf_scale(100.0f);
    }

    /* golden: 与 run 相同的子块结构 + 相位 = 子块序号
     * (base+sub)/FMA_SVE_SUBLANES, 同 SVE 单元; 原式 (base+sub)%4 恒为 0
     * (base 是 svcntw() 倍数、sub 是 4 的倍数), 旋转失效 */
    /* lanes 已由上方门控 svl 换算 (f32 lanes = SVL bytes / 4) */
    svbool_t pg4 = svwhilelt_b32((uint64_t)0, (uint64_t)FMA_SVE_SUBLANES);
    for (int base = 0; base < count; base += lanes) {
        const int n = (count - base < lanes)
                          ? count - base : lanes;
        for (int sub = 0; sub < n; sub += FMA_SVE_SUBLANES) {
            const int phase = (base + sub) / FMA_SVE_SUBLANES;
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
    const int count = data->count;

    float *temp = static_cast<float *>(aligned_alloc_safe(64, (size_t)(count + svcntw()) * sizeof(float)));
    float *dst  = static_cast<float *>(aligned_alloc_safe(64, (size_t)(count + svcntw()) * sizeof(float)));

    TEST_LOOP(test, 1 << 13) {
        for (int base = 0; base < count; base += svcntw()) {
            const int n = (count - base < svcntw())
                              ? count - base : svcntw();
            for (int sub = 0; sub < n; sub += FMA_SVE_SUBLANES) {
                /* 3 源加载 (a, b, 加数 c): asm 保证的 ldr z */
                svfloat32_t a = load_vec_sve(data->srcA + base + sub);
                svfloat32_t b = load_vec_sve(data->srcB + base + sub);
                svfloat32_t c = load_vec_sve(data->srcC + base + sub);
                svbool_t pg4 = svwhilelt_b32((uint64_t)0, (uint64_t)FMA_SVE_SUBLANES);

                svfloat32_t res = fma_rotating_sve(pg4, a, b, c, (base + sub) / FMA_SVE_SUBLANES);

                store_vec_sve(temp + base + sub, res);
                store_vec_sve(dst + base + sub, load_vec_sve(temp + base + sub));
            }
        }

        if (memcmp(dst, data->expected, (size_t)count * sizeof(float)) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (bit pattern + float) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < (size_t)(count); ++_dump_j) {
                    uint32_t _dump_g, _dump_a;
                    memcpy(&_dump_g, &data->expected[_dump_j], 4);
                    memcpy(&_dump_a, &dst[_dump_j], 4);
                    if (_dump_g != _dump_a) {
                        ++_dump_bad;
                        if (_dump_shown < 8) {
                            log_error("arm0102_fma_f32_sve: mismatch at index %zu: golden=0x%08x (%g) actual=0x%08x (%g) xor=0x%08x",
                                      _dump_j, _dump_g, data->expected[_dump_j], _dump_a, dst[_dump_j],
                                      _dump_g ^ _dump_a);
                            ++_dump_shown;
                        }
                    }
                }
                log_error("arm0102_fma_f32_sve: total mismatched elements: %zu / %d", _dump_bad,
                          count);
            }
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
