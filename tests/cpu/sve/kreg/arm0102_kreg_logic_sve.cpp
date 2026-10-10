/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b arm0102_kreg_logic_sve
 * @parblock
 * SVE port of arm0102_kreg_logic (source: missing_testcases_20261009/
 * tests/cpu/arm-0102/arm0102_kreg_logic.cpp; the kreg1 mask-logic
 * branch of the core-179 trigger recipe).
 *
 * Rotating mask logic on 32-bit lanes (phase i%4): and / or / xor /
 * andnot (bic) — the direct SVE 1:1 intrinsic mapping (kreg1_sve
 * precedent). pg4 sub-block predicates preserve the 4-lane NEON
 * semantics; +8-element buffer slack covers the asm full-VL ldr z at
 * the final sub-block (documented deviation from the mask_sve
 * template). Back-to-back str z -> ldr z -> str z amplifier.
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

#define ARM0102_KREG_LOGIC_SVE_COUNT 1024
#define ARM0102_KREG_LOGIC_SVE_SLACK 8

struct Arm0102KregLogicSveData {
    uint32_t *srcA;
    uint32_t *srcB;
    uint32_t *expected;
};

#if defined(__aarch64__)

#define KLOGIC_SVE_SUBLANES 4

static inline void store_vec_sve(void *addr, svuint32_t val) {
    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)KLOGIC_SVE_SUBLANES);
    svst1_u32(pg, static_cast<uint32_t *>(addr), val);
}

static inline svuint32_t load_vec_sve(const void *addr) {
    svuint32_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

static inline svuint32_t logic_rotating_sve(svbool_t pg, svuint32_t a, svuint32_t b, int phase)
{
    switch (phase % 4) {
        case 0: return svand_u32_x(pg, a, b);
        case 1: return svorr_u32_x(pg, a, b);
        case 2: return sveor_u32_x(pg, a, b);
        default: return svbic_u32_x(pg, a, b);
    }
}

#endif

static int arm0102_kreg_logic_sve_init(struct test *test) {
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for arm0102_kreg_logic_sve");
        return EXIT_SKIP;
    }

    const size_t total = (size_t)(ARM0102_KREG_LOGIC_SVE_COUNT + ARM0102_KREG_LOGIC_SVE_SLACK) * sizeof(uint32_t);
    auto *data = static_cast<Arm0102KregLogicSveData *>(malloc(sizeof(Arm0102KregLogicSveData)));
    data->srcA = static_cast<uint32_t *>(aligned_alloc(64, total));
    data->srcB = static_cast<uint32_t *>(aligned_alloc(64, total));
    data->expected = static_cast<uint32_t *>(aligned_alloc(64, total));

    memset_random(data->srcA, total);
    memset_random(data->srcB, total);

    const int lanes = svcntw();
    svbool_t pg4 = svwhilelt_b32((uint64_t)0, (uint64_t)KLOGIC_SVE_SUBLANES);
    for (int base = 0; base < ARM0102_KREG_LOGIC_SVE_COUNT; base += lanes) {
        const int n = (ARM0102_KREG_LOGIC_SVE_COUNT - base < lanes)
                          ? ARM0102_KREG_LOGIC_SVE_COUNT - base : lanes;
        for (int sub = 0; sub < n; sub += KLOGIC_SVE_SUBLANES) {
            const int phase = (base + sub) % 4;
            uint32_t av[8], bv[8], ov[8];
            for (int k = 0; k < KLOGIC_SVE_SUBLANES && sub + k < n; ++k) {
                av[k] = data->srcA[base + sub + k];
                bv[k] = data->srcB[base + sub + k];
            }
            svuint32_t va = svld1_u32(pg4, av), vb = svld1_u32(pg4, bv);
            svuint32_t res = logic_rotating_sve(pg4, va, vb, phase);
            svst1_u32(pg4, ov, res);
            for (int k = 0; k < KLOGIC_SVE_SUBLANES && sub + k < n; ++k)
                data->expected[base + sub + k] = ov[k];
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for arm0102_kreg_logic_sve");
    return EXIT_SKIP;
#endif
}

#if defined(__aarch64__)
static int arm0102_kreg_logic_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<Arm0102KregLogicSveData *>(test->data);

    uint32_t *temp = static_cast<uint32_t *>(aligned_alloc(64, (size_t)(ARM0102_KREG_LOGIC_SVE_COUNT + ARM0102_KREG_LOGIC_SVE_SLACK) * sizeof(uint32_t)));
    uint32_t *dst  = static_cast<uint32_t *>(aligned_alloc(64, (size_t)(ARM0102_KREG_LOGIC_SVE_COUNT + ARM0102_KREG_LOGIC_SVE_SLACK) * sizeof(uint32_t)));

    TEST_LOOP(test, 1 << 13) {
        for (int base = 0; base < ARM0102_KREG_LOGIC_SVE_COUNT; base += svcntw()) {
            const int n = (ARM0102_KREG_LOGIC_SVE_COUNT - base < svcntw())
                              ? ARM0102_KREG_LOGIC_SVE_COUNT - base : svcntw();
            for (int sub = 0; sub < n; sub += KLOGIC_SVE_SUBLANES) {
                svuint32_t a = load_vec_sve(data->srcA + base + sub);
                svuint32_t b = load_vec_sve(data->srcB + base + sub);
                svbool_t pg4 = svwhilelt_b32((uint64_t)0, (uint64_t)KLOGIC_SVE_SUBLANES);

                svuint32_t res = logic_rotating_sve(pg4, a, b, base + sub);

                store_vec_sve(temp + base + sub, res);
                store_vec_sve(dst + base + sub, load_vec_sve(temp + base + sub));
            }
        }

        if (memcmp(dst, data->expected, (size_t)ARM0102_KREG_LOGIC_SVE_COUNT * sizeof(uint32_t)) != 0) {
            report_fail_msg("arm0102_kreg_logic_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int arm0102_kreg_logic_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for arm0102_kreg_logic_sve");
    return EXIT_SKIP;
}
#endif

static int arm0102_kreg_logic_sve_cleanup(struct test *test) {
    auto *data = static_cast<Arm0102KregLogicSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(arm0102_kreg_logic_sve,
             "SVE port of arm-0102 kreg logic variant: 2-src ldr z + rotating and/or/xor/andnot "
             "+ str z/ldr z/str z amplifier on pg4 sub-blocks")
    .test_init    = arm0102_kreg_logic_sve_init,
    .test_run     = arm0102_kreg_logic_sve_run,
    .test_cleanup = arm0102_kreg_logic_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
