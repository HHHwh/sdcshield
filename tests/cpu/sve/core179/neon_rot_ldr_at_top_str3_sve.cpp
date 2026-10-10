/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_ldr_at_top_str3_sve
 * @parblock
 * SVE port of neon_rot_ldr_at_top_str3 (source:
 * missing_testcases_20261009/tests/cpu/misc/neon_rot_ldr_at_top_str3.cpp).
 *
 * Store-count variant of the rowmajor position probe: THREE stores per
 * slot (temp, junk, dst — in that order, all writing the same res; junk
 * never read) at the constant 3-load (dead scratch at top + 2 sources)
 * rotating-ALU skeleton. Store-side gradient: parent 2 str / str3 3 str /
 * str4 4 str — a 0-fail here is ambiguous between "store pressure kills
 * the trigger" and "instruction-count/layout shift kills it"; the
 * gradient SHAPE is the signal (pre-registered in the scalar original).
 *
 * Blind spot kept from the original (by design): only dst is verified —
 * the neon_rot_ldr_at_top_str3_checkall sibling closes it.
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

#define NEON_ROT_LDR_AT_TOP_STR3_SVE_SLOTS 1024

struct NeonRotLdrAtTopStr3SveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *scratch;
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

static int neon_rot_ldr_at_top_str3_sve_init(struct test *test) {
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_str3_sve");
        return EXIT_SKIP;
    }

    const int lanes = (int)svcntd();
    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_STR3_SVE_SLOTS * lanes;

    auto *data = static_cast<NeonRotLdrAtTopStr3SveData *>(malloc(sizeof(NeonRotLdrAtTopStr3SveData)));
    data->lanes = lanes;
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->scratch = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    memset_random(data->srcA, elems * sizeof(uint64_t));
    memset_random(data->srcB, elems * sizeof(uint64_t));
    memset_random(data->scratch, elems * sizeof(uint64_t));

    svbool_t pg = svptrue_b64();
    for (int i = 0; i < NEON_ROT_LDR_AT_TOP_STR3_SVE_SLOTS; i++) {
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
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_str3_sve");
    return EXIT_SKIP;
#endif
}

#if defined(__aarch64__)
static int neon_rot_ldr_at_top_str3_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<NeonRotLdrAtTopStr3SveData *>(test->data);

    if ((int)svcntd() != data->lanes) {
        log_error("neon_rot_ldr_at_top_str3_sve: vector length changed between init (%d) and run (%d)",
                  data->lanes, (int)svcntd());
        report_fail_msg("neon_rot_ldr_at_top_str3_sve: vector length changed between init and run");
    }

    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_STR3_SVE_SLOTS * data->lanes;
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    uint64_t *junk = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    uint64_t sweep = 0;
    TEST_LOOP(test, 1 << 13) {
        const bool ascending = (sweep & 1) == 0;
        for (int n = 0; n < NEON_ROT_LDR_AT_TOP_STR3_SVE_SLOTS; n++) {
            const int i = ascending ? n : (NEON_ROT_LDR_AT_TOP_STR3_SVE_SLOTS - 1 - n);
            const size_t off = (size_t)i * data->lanes;

            svuint64_t unused_x = load_vec_sve(data->scratch + off);
            (void)unused_x;

            svuint64_t a = load_vec_sve(data->srcA + off);
            svuint64_t b = load_vec_sve(data->srcB + off);
            svuint64_t res;

            switch (i % 4) {
                case 0: res = svadd_u64_x(svptrue_b64(), a, b); break;
                case 1: res = sveor_u64_x(svptrue_b64(), a, b); break;
                case 2: res = svand_u64_x(svptrue_b64(), a, b); break;
                case 3: res = svorr_u64_x(svptrue_b64(), a, b); break;
            }

            /* 三存同值: temp -> junk -> dst (junk 只写不读, 原版顺序) */
            store_vec_sve(temp + off, res);
            store_vec_sve(junk + off, res);
            store_vec_sve(dst + off, res);
        }
        sweep++;

        /* 原版盲区保留: 仅校验 dst (str3_checkall 才补全三槽) */
        if (memcmp(dst, data->expected, elems * sizeof(uint64_t)) != 0) {
            report_fail_msg("neon_rot_ldr_at_top_str3_sve data miscompare");
        }
    }

    free(dst);
    free(junk);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int neon_rot_ldr_at_top_str3_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_str3_sve");
    return EXIT_SKIP;
}
#endif

static int neon_rot_ldr_at_top_str3_sve_cleanup(struct test *test) {
    auto *data = static_cast<NeonRotLdrAtTopStr3SveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_ldr_at_top_str3_sve,
             "SVE store-count variant of the rowmajor position probe: 3 str per slot (2 useful + "
             "1 dead junk str) at the constant 3-ldr/rot-ALU skeleton; alternating sweep order "
             "(port of neon_rot_ldr_at_top_str3)")
    .test_init    = neon_rot_ldr_at_top_str3_sve_init,
    .test_run     = neon_rot_ldr_at_top_str3_sve_run,
    .test_cleanup = neon_rot_ldr_at_top_str3_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
