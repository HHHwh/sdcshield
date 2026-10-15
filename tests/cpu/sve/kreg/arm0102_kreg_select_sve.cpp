/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b arm0102_kreg_select_sve
 * @parblock
 * SVE port of arm0102_kreg_select (source: missing_testcases_20261009/
 * tests/cpu/arm-0102/arm0102_kreg_select.cpp; the kreg1 vbsl branch of
 * the core-179 trigger recipe).
 *
 * Rotating bitwise-SELECT on 32-bit lanes (phase i%4):
 *   sel by A    — BSL(a, b, b^a)
 *   bit insert  — BSL(a, a, b)
 *   bif keep    — BSL(a, b, a)
 *   sel mixed   — BSL(a, a&b, a^b)
 * followed by the back-to-back str z -> ldr z -> str z amplifier.
 *
 * Instruction note (documented): GCC 12's SVE1 ACLE has no svbsl —
 * BSL(a,b,c) = (a&b) | (~a&c) is composed with svand/svnot/svorr; the
 * 4-lane NEON semantics are preserved via pg4 sub-block predicates
 * (arm0102_kreg_mask_sve discipline: svptrue at VL=256 is 8 lanes and
 * would overrun the 4-element sub-block — the batch-1 heap-corruption
 * bug). Buffers carry svcntw() elements (runtime VL) of slack so the asm
 * full-VL ldr z at the final sub-block cannot overread (deviation from
 * the mask_sve template, documented).
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

#define ARM0102_KREG_SELECT_SVE_COUNT 1024        /* u32 elements */
/* 尾部 asm 全宽读松弛 = 运行时 svcntw(): 最末子块 ldr z 读到
 * (COUNT-1)+svcntw() 个元素, slack=svcntw() 严格充分 (任意 VL,
 * movbe_slf_sweep VL-portable 范式) */

struct Arm0102KregSelectSveData {
    uint32_t *srcA;
    uint32_t *srcB;
    uint32_t *expected;
};

#if defined(__aarch64__)

#define KSEL_SVE_SUBLANES 4

static inline void store_vec_sve(void *addr, svuint32_t val) {
    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)KSEL_SVE_SUBLANES);
    svst1_u32(pg, static_cast<uint32_t *>(addr), val);
}

static inline svuint32_t load_vec_sve(const void *addr) {
    svuint32_t res;
    /* asm 保证的加载 (core-179 配方要求; 全 VL 读, 存储侧用 pg4 截断) */
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

/* BSL(a, b, c) = (a & b) | (~a & c) — svbsl 不在 GCC 12 SVE1 ACLE (组合式) */
static inline svuint32_t bsl_sve(svbool_t pg, svuint32_t a, svuint32_t b, svuint32_t c)
{
    return svorr_u32_x(pg, svand_u32_x(pg, a, b),
                       svand_u32_x(pg, svnot_u32_x(pg, a), c));
}

/* 旋转选择载荷 (相位 i%4, 与原版逐案对应) */
static inline svuint32_t select_rotating_sve(svbool_t pg, svuint32_t a, svuint32_t b, int phase)
{
    switch (phase % 4) {
        case 0: return bsl_sve(pg, a, b, sveor_u32_x(pg, b, a));           /* sel by A */
        case 1: return bsl_sve(pg, a, a, b);                               /* bit insert */
        case 2: return bsl_sve(pg, a, b, a);                               /* bif keep */
        default: return bsl_sve(pg, a, svand_u32_x(pg, a, b),
                                sveor_u32_x(pg, a, b));                     /* sel mixed */
    }
}

#endif

static int arm0102_kreg_select_sve_init(struct test *test) {
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for arm0102_kreg_select_sve");
        return EXIT_SKIP;
    }

    const int slack = svcntw();   /* asm ldr z 尾批松弛, 见文件头注释 */
    const size_t total = (size_t)(ARM0102_KREG_SELECT_SVE_COUNT + slack) * sizeof(uint32_t);
    auto *data = static_cast<Arm0102KregSelectSveData *>(malloc(sizeof(Arm0102KregSelectSveData)));
    /* safe: total = (1024 + svcntw())*4 在部分 VL 下不是 64 的倍数
     * (VL=128 -> 4112, VL=256 -> 4128 违规; VL=512 -> 4160 恰为倍数),
     * 违规时原生 aligned_alloc 违反 C11 size 倍数前提, 新 libasan abort (issue #252) */
    data->srcA = static_cast<uint32_t *>(aligned_alloc_safe(64, total));
    data->srcB = static_cast<uint32_t *>(aligned_alloc_safe(64, total));
    data->expected = static_cast<uint32_t *>(aligned_alloc_safe(64, total));

    memset_random(data->srcA, total);
    memset_random(data->srcB, total);

    /* golden: 与 run 完全相同的 4 元素子块结构 + 相位 = 子块序号
     * (base+sub)/KSEL_SVE_SUBLANES, 按子块旋转 op; 原式 (base+sub)%4
     * 恒为 0 (base 是 svcntw() 倍数、sub 是 4 的倍数), 旋转失效 */
    const int lanes = svcntw();
    svbool_t pg4 = svwhilelt_b32((uint64_t)0, (uint64_t)KSEL_SVE_SUBLANES);
    for (int base = 0; base < ARM0102_KREG_SELECT_SVE_COUNT; base += lanes) {
        const int n = (ARM0102_KREG_SELECT_SVE_COUNT - base < lanes)
                          ? ARM0102_KREG_SELECT_SVE_COUNT - base : lanes;
        for (int sub = 0; sub < n; sub += KSEL_SVE_SUBLANES) {
            const int phase = (base + sub) / KSEL_SVE_SUBLANES;
            uint32_t av[8], bv[8], ov[8];
            for (int k = 0; k < KSEL_SVE_SUBLANES && sub + k < n; ++k) {
                av[k] = data->srcA[base + sub + k];
                bv[k] = data->srcB[base + sub + k];
            }
            svuint32_t va = svld1_u32(pg4, av), vb = svld1_u32(pg4, bv);
            svuint32_t res = select_rotating_sve(pg4, va, vb, phase);
            svst1_u32(pg4, ov, res);
            for (int k = 0; k < KSEL_SVE_SUBLANES && sub + k < n; ++k)
                data->expected[base + sub + k] = ov[k];
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for arm0102_kreg_select_sve");
    return EXIT_SKIP;
#endif
}

#if defined(__aarch64__)
static int arm0102_kreg_select_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<Arm0102KregSelectSveData *>(test->data);

    uint32_t *temp = static_cast<uint32_t *>(aligned_alloc_safe(64, (size_t)(ARM0102_KREG_SELECT_SVE_COUNT + svcntw()) * sizeof(uint32_t)));
    uint32_t *dst  = static_cast<uint32_t *>(aligned_alloc_safe(64, (size_t)(ARM0102_KREG_SELECT_SVE_COUNT + svcntw()) * sizeof(uint32_t)));

    TEST_LOOP(test, 1 << 13) {
        /* 与 golden 逐字一致的子块结构: 相位 = 子块序号 (base+sub)/KSEL_SVE_SUBLANES */
        for (int base = 0; base < ARM0102_KREG_SELECT_SVE_COUNT; base += svcntw()) {
            const int n = (ARM0102_KREG_SELECT_SVE_COUNT - base < svcntw())
                              ? ARM0102_KREG_SELECT_SVE_COUNT - base : svcntw();
            for (int sub = 0; sub < n; sub += KSEL_SVE_SUBLANES) {
                /* 2 源加载: asm 保证的 ldr z */
                svuint32_t a = load_vec_sve(data->srcA + base + sub);
                svuint32_t b = load_vec_sve(data->srcB + base + sub);
                svbool_t pg4 = svwhilelt_b32((uint64_t)0, (uint64_t)KSEL_SVE_SUBLANES);

                svuint32_t res = select_rotating_sve(pg4, a, b, (base + sub) / KSEL_SVE_SUBLANES);

                /* 放大器: str z -> ldr z -> str z */
                store_vec_sve(temp + base + sub, res);
                store_vec_sve(dst + base + sub, load_vec_sve(temp + base + sub));
            }
        }

        if (memcmp(dst, data->expected, (size_t)ARM0102_KREG_SELECT_SVE_COUNT * sizeof(uint32_t)) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < (size_t)(ARM0102_KREG_SELECT_SVE_COUNT); ++_dump_j) {
                    if (dst[_dump_j] != data->expected[_dump_j]) {
                        ++_dump_bad;
                        if (_dump_shown < 8) {
                            log_error("arm0102_kreg_select_sve: mismatch at index %zu: golden=0x%08x actual=0x%08x xor=0x%08x",
                                      _dump_j, (unsigned)data->expected[_dump_j], (unsigned)dst[_dump_j],
                                      (unsigned)(data->expected[_dump_j] ^ dst[_dump_j]));
                            ++_dump_shown;
                        }
                    }
                }
                log_error("arm0102_kreg_select_sve: total mismatched elements: %zu / %d", _dump_bad,
                          ARM0102_KREG_SELECT_SVE_COUNT);
            }
            report_fail_msg("arm0102_kreg_select_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int arm0102_kreg_select_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for arm0102_kreg_select_sve");
    return EXIT_SKIP;
}
#endif

static int arm0102_kreg_select_sve_cleanup(struct test *test) {
    auto *data = static_cast<Arm0102KregSelectSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(arm0102_kreg_select_sve,
             "SVE port of arm-0102 kreg select variant: 2-src ldr z + rotating bitwise-SELECT "
             "BSL(a,b,b^a)/BSL(a,a,b)/BSL(a,b,a)/BSL(a,a&b,a^b) composed from and/not/or + "
             "str z/ldr z/str z amplifier on pg4 sub-blocks")
    .test_init    = arm0102_kreg_select_sve_init,
    .test_run     = arm0102_kreg_select_sve_run,
    .test_cleanup = arm0102_kreg_select_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
