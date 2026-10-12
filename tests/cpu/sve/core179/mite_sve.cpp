/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mite_sve
 * @parblock
 * SVE port of mite: the add/eor/sub constant chain and the parity/bit16
 * branch logic run on vector lanes (svadd/sveor/svsub + predicate selects
 * replacing the scalar branches), golden per the scalar reference.
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

#define MITE_SVE_ITERATIONS 1024

static constexpr uint32_t MITE_CONST1 = 0x12345678u;
static constexpr uint32_t MITE_CONST2 = 0x9ABCDEF0u;
static constexpr uint32_t MITE_CONST3 = 0x55555555u;

struct MiteSveData {
    uint32_t *input;
    uint32_t *expected;
};

/* 标量参考 (golden, 与原版 process_value 同语义) */
static inline uint32_t process_value_ref(uint32_t w0_val)
{
    w0_val = w0_val + MITE_CONST1;
    w0_val = w0_val ^ MITE_CONST2;
    w0_val = w0_val - MITE_CONST3;
    if (w0_val & 1) w0_val = w0_val * 13;
    else            w0_val = w0_val * 17;
    if ((w0_val >> 16) & 1) w0_val += 1;
    return w0_val;
}

static int mite_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for mite_sve");
        return EXIT_SKIP;
    }
#endif
    auto *data = (MiteSveData *)malloc(sizeof(MiteSveData));
    data->input = (uint32_t *)aligned_alloc_safe(64, MITE_SVE_ITERATIONS * sizeof(uint32_t));
    data->expected = (uint32_t *)aligned_alloc_safe(64, MITE_SVE_ITERATIONS * sizeof(uint32_t));
    memset_random(data->input, MITE_SVE_ITERATIONS * sizeof(uint32_t));
    for (int i = 0; i < MITE_SVE_ITERATIONS; ++i)
        data->expected[i] = process_value_ref(data->input[i]);
    test->data = data;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)
static int mite_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = (MiteSveData *)test->data;
    uint32_t *dst = (uint32_t *)aligned_alloc(64, MITE_SVE_ITERATIONS * sizeof(uint32_t));
    const int lanes = svcntw();

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < MITE_SVE_ITERATIONS; base += lanes) {
            int n = (MITE_SVE_ITERATIONS - base < (size_t)lanes) ? (int)(MITE_SVE_ITERATIONS - base) : lanes;
            svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)n);
            svuint32_t v = svld1_u32(pg, data->input + base);

            /* 常量链 (向量): +C1 ^C2 -C3 */
            v = svadd_n_u32_x(pg, v, MITE_CONST1);
            v = sveor_n_u32_x(pg, v, MITE_CONST2);
            v = svsub_n_u32_x(pg, v, MITE_CONST3);

            /* 奇偶分支: 奇数 ×13, 偶数 ×17 (谓词选择) */
            svbool_t odd = svcmpne_n_u32(pg, svand_n_u32_x(pg, v, 1u), 0u);
            v = svsel_u32(odd, svmul_n_u32_x(pg, v, 13u), svmul_n_u32_x(pg, v, 17u));

            /* bit16 分支: +1 (谓词选择) */
            svbool_t bit16 = svcmpne_n_u32(pg, svand_n_u32_x(pg, svlsr_n_u32_x(pg, v, 16), 1u), 0u);
            v = svsel_u32(bit16, svadd_n_u32_x(pg, v, 1u), v);

            svst1_u32(pg, dst + base, v);
        }
        if (memcmp(dst, data->expected, MITE_SVE_ITERATIONS * sizeof(uint32_t)) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < (size_t)(MITE_SVE_ITERATIONS); ++_dump_j) {
                    if (dst[_dump_j] != data->expected[_dump_j]) {
                        ++_dump_bad;
                        if (_dump_shown < 8) {
                            log_error("mite_sve: mismatch at index %zu: golden=0x%08x actual=0x%08x xor=0x%08x",
                                      _dump_j,
                                      (unsigned)data->expected[_dump_j],
                                      (unsigned)dst[_dump_j],
                                      (unsigned)(data->expected[_dump_j] ^ dst[_dump_j]));
                            ++_dump_shown;
                        }
                    }
                }
                log_error("mite_sve: total mismatched elements: %zu / %d",
                          _dump_bad, MITE_SVE_ITERATIONS);
            }
            report_fail_msg("mite_sve data miscompare");
        }
    }
    free(dst);
    return EXIT_SUCCESS;
}
#else
static int mite_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for mite_sve");
    return EXIT_SKIP;
}
#endif

static int mite_sve_cleanup(struct test *test)
{
    auto *data = (MiteSveData *)test->data;
    if (data) {
        free(data->input);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mite_sve, "SVE add/eor/sub constant chain with predicate-based branch logic (port of mite)")
    .test_init = mite_sve_init,
    .test_run = mite_sve_run,
    .test_cleanup = mite_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
