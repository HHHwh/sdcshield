/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_ldr_at_top_ldr4_sve
 * @parblock
 * SVE port of neon_rot_ldr_at_top_ldr4 (source:
 * missing_testcases_20261009/tests/cpu/misc/neon_rot_ldr_at_top_ldr4.cpp).
 *
 * Dead-load-count variant of the rowmajor position probe: TWO dead
 * loads at the top of the loop body (scratch1 + scratch2, both values
 * discarded) before the 2 useful source loads — 4 ldr per slot at the
 * constant 2-str-back-to-back / rotating-ALU skeleton. Load-side
 * gradient (scalar baselines): parent 3 ldr ~244 fail/30min -> ldr4
 * 4 ldr (this test) -> lsqfill 11 ldr 0 fail (large layout confound);
 * the minimal single-step dead-load probe. A 0-fail here carries the
 * same layout-confound caveat as the store-side gradient — the gradient
 * SHAPE is the signal.
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

#define NEON_ROT_LDR_AT_TOP_LDR4_SVE_SLOTS 1024

struct NeonRotLdrAtTopLdr4SveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *scratch1;
    uint64_t *scratch2;
    uint64_t *expected;
    int lanes;
};

#if defined(__aarch64__)

static inline void store_vec_sve(void *addr, svuint64_t val) {
    svst1_u64(svptrue_b64(), static_cast<uint64_t *>(addr), val);
}

static inline svuint64_t load_vec_sve(const void *addr) {
    svuint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

#endif

static int neon_rot_ldr_at_top_ldr4_sve_init(struct test *test) {
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_ldr4_sve");
        return EXIT_SKIP;
    }

    const int lanes = (int)svcntd();
    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_LDR4_SVE_SLOTS * lanes;

    auto *data = static_cast<NeonRotLdrAtTopLdr4SveData *>(malloc(sizeof(NeonRotLdrAtTopLdr4SveData)));
    data->lanes = lanes;
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->scratch1 = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->scratch2 = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    memset_random(data->srcA, elems * sizeof(uint64_t));
    memset_random(data->srcB, elems * sizeof(uint64_t));
    memset_random(data->scratch1, elems * sizeof(uint64_t));
    memset_random(data->scratch2, elems * sizeof(uint64_t));

    svbool_t pg = svptrue_b64();
    for (int i = 0; i < NEON_ROT_LDR_AT_TOP_LDR4_SVE_SLOTS; i++) {
        svuint64_t a = svld1_u64(pg, data->srcA + (size_t)i * lanes);
        svuint64_t b = svld1_u64(pg, data->srcB + (size_t)i * lanes);
        svuint64_t res;
        switch (i % 4) {
            case 0: res = svadd_u64_x(pg, a, b); break;
            case 1: res = sveor_u64_x(pg, a, b); break;
            case 2: res = svand_u64_x(pg, a, b); break;
            case 3: res = svorr_u64_x(pg, a, b); break;
        }
        svst1_u64(pg, data->expected + (size_t)i * lanes, res);
    }

    test->data = data;
    return EXIT_SUCCESS;
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_ldr4_sve");
    return EXIT_SKIP;
#endif
}

#if defined(__aarch64__)
static int neon_rot_ldr_at_top_ldr4_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<NeonRotLdrAtTopLdr4SveData *>(test->data);

    if ((int)svcntd() != data->lanes) {
        log_error("neon_rot_ldr_at_top_ldr4_sve: vector length changed between init (%d) and run (%d)",
                  data->lanes, (int)svcntd());
        report_fail_msg("neon_rot_ldr_at_top_ldr4_sve: vector length changed between init and run");
    }

    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_LDR4_SVE_SLOTS * data->lanes;
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    uint64_t sweep = 0;
    TEST_LOOP(test, 1 << 13) {
        const bool ascending = (sweep & 1) == 0;
        for (int n = 0; n < NEON_ROT_LDR_AT_TOP_LDR4_SVE_SLOTS; n++) {
            const int i = ascending ? n : (NEON_ROT_LDR_AT_TOP_LDR4_SVE_SLOTS - 1 - n);
            const size_t off = (size_t)i * data->lanes;

            /* 顶部两个死载 (scratch1 + scratch2, 值丢弃) — 4 ldr = 2 死 + 2 源 */
            svuint64_t unused_x1 = load_vec_sve(data->scratch1 + off);
            (void)unused_x1;
            svuint64_t unused_x2 = load_vec_sve(data->scratch2 + off);
            (void)unused_x2;

            svuint64_t a = load_vec_sve(data->srcA + off);
            svuint64_t b = load_vec_sve(data->srcB + off);
            svuint64_t res;

            switch (i % 4) {
                case 0: res = svadd_u64_x(svptrue_b64(), a, b); break;
                case 1: res = sveor_u64_x(svptrue_b64(), a, b); break;
                case 2: res = svand_u64_x(svptrue_b64(), a, b); break;
                case 3: res = svorr_u64_x(svptrue_b64(), a, b); break;
            }

            store_vec_sve(temp + off, res);
            store_vec_sve(dst + off, res);
        }
        sweep++;

        if (memcmp(dst, data->expected, elems * sizeof(uint64_t)) != 0) {
            report_fail_msg("neon_rot_ldr_at_top_ldr4_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int neon_rot_ldr_at_top_ldr4_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_ldr4_sve");
    return EXIT_SKIP;
}
#endif

static int neon_rot_ldr_at_top_ldr4_sve_cleanup(struct test *test) {
    auto *data = static_cast<NeonRotLdrAtTopLdr4SveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->scratch1);
        free(data->scratch2);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_ldr_at_top_ldr4_sve,
             "SVE dead-load-count variant of the rowmajor position probe: 4 ldr per slot "
             "(2 dead scratch + 2 useful) at the constant 2-str-back-to-back/rot-ALU skeleton "
             "(port of neon_rot_ldr_at_top_ldr4)")
    .test_init    = neon_rot_ldr_at_top_ldr4_sve_init,
    .test_run     = neon_rot_ldr_at_top_ldr4_sve_run,
    .test_cleanup = neon_rot_ldr_at_top_ldr4_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
