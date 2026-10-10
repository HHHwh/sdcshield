/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_store_only_sve
 * @parblock
 * SVE port of neon_rot_store_only (source: missing_testcases_20261009/
 * tests/cpu/misc/neon_rot_store_only.cpp).
 *
 * Reload-necessity probe on the vector datapath: the neon_rot_2src
 * trigger recipe (asm-guaranteed ldr z source loads + rotating
 * add/eor/and/orr composition per element phase + back-to-back stores)
 * with the reload REMOVED — the result is stored to temp and dst with
 * no load in between (the scalar NEON original's baseline:
 * neon_rot_2src fires ~33 fail/30min; this cell asks whether the
 * back-to-back reload is a necessary condition).
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

#define NEON_ROT_STORE_ONLY_SVE_COUNT 1024   /* uint64 elements per buffer */

struct NeonRotStoreOnlySveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *expected;
};

static int neon_rot_store_only_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for neon_rot_store_only_sve");
        return EXIT_SKIP;
    }
#endif
    auto *data = static_cast<NeonRotStoreOnlySveData *>(malloc(sizeof(NeonRotStoreOnlySveData)));
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, NEON_ROT_STORE_ONLY_SVE_COUNT * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, NEON_ROT_STORE_ONLY_SVE_COUNT * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, NEON_ROT_STORE_ONLY_SVE_COUNT * sizeof(uint64_t)));

    memset_random(data->srcA, NEON_ROT_STORE_ONLY_SVE_COUNT * sizeof(uint64_t));
    memset_random(data->srcB, NEON_ROT_STORE_ONLY_SVE_COUNT * sizeof(uint64_t));

    for (int i = 0; i < NEON_ROT_STORE_ONLY_SVE_COUNT; i++) {
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

static inline svuint64_t load_vec_sve(const void *addr)
{
    svuint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

static int neon_rot_store_only_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<NeonRotStoreOnlySveData *>(test->data);

    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, NEON_ROT_STORE_ONLY_SVE_COUNT * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, NEON_ROT_STORE_ONLY_SVE_COUNT * sizeof(uint64_t)));
    const int lanes = svcntd();
    svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)lanes);

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < NEON_ROT_STORE_ONLY_SVE_COUNT; base += lanes) {
            /* 2 源加载: asm 保证的 ldr z */
            svuint64_t a = load_vec_sve(data->srcA + base);
            svuint64_t b = load_vec_sve(data->srcB + base);

            /* 旋转 ALU: 逐 lane 相位 (base+k)%4, 相位谓词分派 */
            svuint64_t r_add = svadd_u64_x(pg, a, b);
            svuint64_t r_eor = sveor_u64_x(pg, a, b);
            svuint64_t r_and = svand_u64_x(pg, a, b);
            svuint64_t r_orr = svorr_u64_x(pg, a, b);
            int n = (NEON_ROT_STORE_ONLY_SVE_COUNT - base < (size_t)lanes)
                        ? (int)(NEON_ROT_STORE_ONLY_SVE_COUNT - base) : lanes;
            uint64_t phase[8];
            for (int k = 0; k < n; ++k) phase[k] = (base + k) % 4;
            svbool_t pgn = svwhilelt_b64((uint64_t)0, (uint64_t)n);
            svuint64_t vphase = svld1_u64(pgn, phase);
            svuint64_t res = svsel_u64(svcmpeq_n_u64(pgn, vphase, 0), r_add,
                             svsel_u64(svcmpeq_n_u64(pgn, vphase, 1), r_eor,
                             svsel_u64(svcmpeq_n_u64(pgn, vphase, 2), r_and, r_orr)));

            /* 两存无读: 移除 reload (reload-必要性探针的核心) */
            svst1_u64(pgn, temp + base, res);
            svst1_u64(pgn, dst + base, res);
        }

        if (memcmp(dst, data->expected, NEON_ROT_STORE_ONLY_SVE_COUNT * sizeof(uint64_t)) != 0) {
            report_fail_msg("neon_rot_store_only_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

#else

static int neon_rot_store_only_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_store_only_sve");
    return EXIT_SKIP;
}

#endif

static int neon_rot_store_only_sve_cleanup(struct test *test)
{
    auto *data = static_cast<NeonRotStoreOnlySveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_store_only_sve,
             "SVE datapath variant: 2-src ldr z + rotating ALU + 2 stores, NO reload — the "
             "reload-necessity probe on SVE lanes (port of neon_rot_store_only)")
    .test_init = neon_rot_store_only_sve_init,
    .test_run = neon_rot_store_only_sve_run,
    .test_cleanup = neon_rot_store_only_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
