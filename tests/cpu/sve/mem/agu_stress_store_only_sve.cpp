/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b agu_stress_store_only_sve
 * @parblock
 * SVE port of agu_stress_store_only (source: missing_testcases_20261009/
 * tests/cpu/misc/agu_stress_store_only.cpp).
 *
 * Store-only probe: the 2-source load + rotating data-ALU is retained
 * but the result is stored TWICE with NO reload in between
 * (svst1 temp; svst1 dst) — the store->load forwarding window is
 * removed entirely. Adjudicates whether the back-to-back reload is a
 * NECESSARY condition for the core-179 trigger (the scalar original
 * showed 1 fail/30min single sample, so the store-only path is not
 * entirely clean either).
 *
 * Note the rotation is {add, xor, and, OR} — OR, not sub (the scalar
 * original's agu-family rotation; kept verbatim).
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

#define AGU_STORE_ONLY_SVE_COUNT 1024

struct AguStoreOnlySveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *expected;
};

static int agu_stress_store_only_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for agu_stress_store_only_sve");
        return EXIT_SKIP;
    }
#endif
    auto *data = static_cast<AguStoreOnlySveData *>(malloc(sizeof(AguStoreOnlySveData)));
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, AGU_STORE_ONLY_SVE_COUNT * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, AGU_STORE_ONLY_SVE_COUNT * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, AGU_STORE_ONLY_SVE_COUNT * sizeof(uint64_t)));

    memset_random(data->srcA, AGU_STORE_ONLY_SVE_COUNT * sizeof(uint64_t));
    memset_random(data->srcB, AGU_STORE_ONLY_SVE_COUNT * sizeof(uint64_t));

    for (int i = 0; i < AGU_STORE_ONLY_SVE_COUNT; i++) {
        switch (i % 4) {
            case 0: data->expected[i] = data->srcA[i] + data->srcB[i]; break;
            case 1: data->expected[i] = data->srcA[i] ^ data->srcB[i]; break;
            case 2: data->expected[i] = data->srcA[i] & data->srcB[i]; break;
            case 3: data->expected[i] = data->srcA[i] | data->srcB[i]; break;
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)

static int agu_stress_store_only_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<AguStoreOnlySveData *>(test->data);
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, AGU_STORE_ONLY_SVE_COUNT * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, AGU_STORE_ONLY_SVE_COUNT * sizeof(uint64_t)));
    const int lanes = svcntd();

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < AGU_STORE_ONLY_SVE_COUNT; base += lanes) {
            int n = (AGU_STORE_ONLY_SVE_COUNT - base < (size_t)lanes)
                        ? (int)(AGU_STORE_ONLY_SVE_COUNT - base) : lanes;
            svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)n);

            svuint64_t va = svld1_u64(pg, data->srcA + base);
            svuint64_t vb = svld1_u64(pg, data->srcB + base);
            svuint64_t r_add = svadd_u64_x(pg, va, vb);
            svuint64_t r_eor = sveor_u64_x(pg, va, vb);
            svuint64_t r_and = svand_u64_x(pg, va, vb);
            svuint64_t r_orr = svorr_u64_x(pg, va, vb);
            uint64_t phase[8];
            for (int k = 0; k < n; ++k) phase[k] = (base + k) % 4;
            svuint64_t vphase = svld1_u64(pg, phase);
            svuint64_t res = svsel_u64(svcmpeq_n_u64(pg, vphase, 0), r_add,
                             svsel_u64(svcmpeq_n_u64(pg, vphase, 1), r_eor,
                             svsel_u64(svcmpeq_n_u64(pg, vphase, 2), r_and, r_orr)));

            /* 两存无读: 移除 store->load 转发窗口 (temp 只写不读) */
            svst1_u64(pg, temp + base, res);
            svst1_u64(pg, dst + base, res);
        }

        if (memcmp(dst, data->expected, AGU_STORE_ONLY_SVE_COUNT * sizeof(uint64_t)) != 0) {
            report_fail_msg("agu_stress_store_only_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

#else

static int agu_stress_store_only_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for agu_stress_store_only_sve");
    return EXIT_SKIP;
}

#endif

static int agu_stress_store_only_sve_cleanup(struct test *test)
{
    auto *data = static_cast<AguStoreOnlySveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(agu_stress_store_only_sve,
             "SVE AGU stress store-only probe: 2-src + ALU rotation, two stores with NO reload — "
             "removes the store->load forwarding window (port of agu_stress_store_only)")
    .test_init = agu_stress_store_only_sve_init,
    .test_run = agu_stress_store_only_sve_run,
    .test_cleanup = agu_stress_store_only_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
