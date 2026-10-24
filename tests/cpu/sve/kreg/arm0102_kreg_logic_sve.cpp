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
 * semantics; svcntw() elements (runtime VL) of buffer slack cover the
 * asm full-VL ldr z at the final sub-block (documented deviation from
 * the mask_sve template). Back-to-back str z -> ldr z -> str z
 * amplifier.
 * The element count (originally ARM0102_KREG_LOGIC_SVE_COUNT=1024) is
 * runtime configurable via -O arm0102_kreg_logic_sve.count=N /
 * SDC_ARM0102_KREG_LOGIC_SVE_COUNT, with a memory-adaptive default
 * when unconfigured.
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

#define ARM0102_KREG_LOGIC_SVE_COUNT 1024
/* 尾部 asm 全宽读松弛 = 运行时 svcntw(): 最末子块 ldr z 读到
 * (COUNT-1)+svcntw() 个元素, slack=svcntw() 严格充分 (任意 VL,
 * movbe_slf_sweep VL-portable 范式) */

struct Arm0102KregLogicSveData {
    uint32_t *srcA;
    uint32_t *srcB;
    uint32_t *expected;
    int count;                /* 运行时元素数 (≤64M, int 足够) */
};

#if defined(__aarch64__)

/* 元素数 ARM0102_KREG_LOGIC_SVE_COUNT(1024) — 运行时可配置, 统一优先级链:
 *   1. -O arm0102_kreg_logic_sve.count=N / 配置文件同名条目
 *   2. 环境变量 SDC_ARM0102_KREG_LOGIC_SVE_COUNT
 *   3. 自适应: srcA/srcB/expected(共享)+每线程 temp/dst, 5×4B,
 *      占物理内存 60%, clamp [1024, 64M]。pg4 子块/相位/尾部松弛不变。 */
static size_t arm0102_kreg_logic_sve_count(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "count", 0, ARM0102_KREG_LOGIC_SVE_COUNT, 64 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("arm0102_kreg_logic_sve", "count", &v, ARM0102_KREG_LOGIC_SVE_COUNT, 64 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count(4, 5, 1.0, 600, thread_count(),
                                         ARM0102_KREG_LOGIC_SVE_COUNT, 64 << 20, test->id, "count");
}

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
    const int count = (int)arm0102_kreg_logic_sve_count(test);
    const size_t total = (size_t)(count + slack) * sizeof(uint32_t);
    auto *data = static_cast<Arm0102KregLogicSveData *>(malloc(sizeof(Arm0102KregLogicSveData)));
    data->count = count;
    data->srcA = static_cast<uint32_t *>(aligned_alloc_safe(64, total));
    data->srcB = static_cast<uint32_t *>(aligned_alloc_safe(64, total));
    data->expected = static_cast<uint32_t *>(aligned_alloc_safe(64, total));

    memset_random(data->srcA, total);
    memset_random(data->srcB, total);

    /* lanes 已由上方门控 svl 换算 (f32 lanes = SVL bytes / 4) */
    svbool_t pg4 = svwhilelt_b32((uint64_t)0, (uint64_t)KLOGIC_SVE_SUBLANES);
    for (int base = 0; base < count; base += lanes) {
        const int n = (count - base < lanes)
                          ? count - base : lanes;
        for (int sub = 0; sub < n; sub += KLOGIC_SVE_SUBLANES) {
            /* 相位 = 子块序号 (base+sub)/KLOGIC_SVE_SUBLANES, 按子块旋转;
             * 原式 (base+sub)%4 恒为 0 (base 是 svcntw() 倍数、sub 是 4 的倍数) */
            const int phase = (base + sub) / KLOGIC_SVE_SUBLANES;
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
    const int count = data->count;

    uint32_t *temp = static_cast<uint32_t *>(aligned_alloc_safe(64, (size_t)(count + svcntw()) * sizeof(uint32_t)));
    uint32_t *dst  = static_cast<uint32_t *>(aligned_alloc_safe(64, (size_t)(count + svcntw()) * sizeof(uint32_t)));

    TEST_LOOP(test, 1 << 13) {
        for (int base = 0; base < count; base += svcntw()) {
            const int n = (count - base < svcntw())
                              ? count - base : svcntw();
            for (int sub = 0; sub < n; sub += KLOGIC_SVE_SUBLANES) {
                svuint32_t a = load_vec_sve(data->srcA + base + sub);
                svuint32_t b = load_vec_sve(data->srcB + base + sub);
                svbool_t pg4 = svwhilelt_b32((uint64_t)0, (uint64_t)KLOGIC_SVE_SUBLANES);

                svuint32_t res = logic_rotating_sve(pg4, a, b, (base + sub) / KLOGIC_SVE_SUBLANES);

                store_vec_sve(temp + base + sub, res);
                store_vec_sve(dst + base + sub, load_vec_sve(temp + base + sub));
            }
        }

        if (memcmp(dst, data->expected, (size_t)count * sizeof(uint32_t)) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < (size_t)(count); ++_dump_j) {
                    if (dst[_dump_j] != data->expected[_dump_j]) {
                        ++_dump_bad;
                        if (_dump_shown < 8) {
                            log_error("arm0102_kreg_logic_sve: mismatch at index %zu: golden=0x%08x actual=0x%08x xor=0x%08x",
                                      _dump_j, (unsigned)data->expected[_dump_j], (unsigned)dst[_dump_j],
                                      (unsigned)(data->expected[_dump_j] ^ dst[_dump_j]));
                            ++_dump_shown;
                        }
                    }
                }
                log_error("arm0102_kreg_logic_sve: total mismatched elements: %zu / %d", _dump_bad,
                          count);
            }
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
