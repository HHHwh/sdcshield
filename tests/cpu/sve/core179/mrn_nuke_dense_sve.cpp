/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_nuke_dense_sve
 * @parblock
 * SVE port of mrn_nuke_dense (source: missing_testcases_20261009/
 * tests/cpu/misc/mrn_nuke_dense.cpp).
 *
 * Load-density axis of the mrn_nuke probe: the skeleton (svst1 temp ->
 * ldr z -> svst1 dst, no ALU, golden = src itself) with the element
 * count raised 16x to 16384 (128 KB per buffer) — ~3x the vector
 * load density of the 1024-element scalar original per unit time on
 * SVE lanes. Tests the "absolute load count" threshold hypothesis.
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

#define MRN_NUKE_DENSE_SVE_COUNT 16384   /* 16x the 1024-element family default */

struct MrnNukeDenseSveData {
    uint64_t *src;
};

static int mrn_nuke_dense_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for mrn_nuke_dense_sve");
        return EXIT_SKIP;
    }
#endif
    auto *data = static_cast<MrnNukeDenseSveData *>(malloc(sizeof(MrnNukeDenseSveData)));
    data->src = static_cast<uint64_t *>(aligned_alloc(64, MRN_NUKE_DENSE_SVE_COUNT * sizeof(uint64_t)));
    memset_random(data->src, MRN_NUKE_DENSE_SVE_COUNT * sizeof(uint64_t));
    test->data = data;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)

static inline svuint64_t load_vec_sve(const void *addr)
{
    svuint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

static int mrn_nuke_dense_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<MrnNukeDenseSveData *>(test->data);
    const int lanes = svcntd();

    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, MRN_NUKE_DENSE_SVE_COUNT * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, MRN_NUKE_DENSE_SVE_COUNT * sizeof(uint64_t)));

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < MRN_NUKE_DENSE_SVE_COUNT; base += lanes) {
            int n = (MRN_NUKE_DENSE_SVE_COUNT - base < (size_t)lanes)
                        ? (int)(MRN_NUKE_DENSE_SVE_COUNT - base) : lanes;
            svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)n);
            /* mrn_nuke 骨架不变: 向量 store temp, 转发 ldr z, store dst; 仅元素数 16x */
            svst1_u64(pg, temp + base, svld1_u64(pg, data->src + base));
            svst1_u64(pg, dst + base, load_vec_sve(temp + base));
        }
        if (memcmp(dst, data->src, MRN_NUKE_DENSE_SVE_COUNT * sizeof(uint64_t)) != 0) {
            report_fail_msg("mrn_nuke_dense_sve data miscompare");
        }
    }
    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

#else

static int mrn_nuke_dense_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for mrn_nuke_dense_sve");
    return EXIT_SKIP;
}

#endif

static int mrn_nuke_dense_sve_cleanup(struct test *test)
{
    auto *data = static_cast<MrnNukeDenseSveData *>(test->data);
    if (data) {
        free(data->src);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mrn_nuke_dense_sve,
             "SVE mrn_nuke variant: 16x element count (16384) to test the load-density threshold "
             "hypothesis (port of mrn_nuke_dense)")
    .test_init = mrn_nuke_dense_sve_init,
    .test_run = mrn_nuke_dense_sve_run,
    .test_cleanup = mrn_nuke_dense_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
