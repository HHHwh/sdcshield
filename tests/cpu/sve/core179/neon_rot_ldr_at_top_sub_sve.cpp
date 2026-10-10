/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_ldr_at_top_sub_sve
 * @parblock
 * SVE port of neon_rot_ldr_at_top_sub (source:
 * missing_testcases_20261009/tests/cpu/misc/neon_rot_ldr_at_top_sub.cpp).
 *
 * ALU-dimension variant of the position probe (original-parent lineage,
 * forward-only sweep): the rotating composition is sub/eor/bic/orn —
 * carry-propagating sub vs combinational eor/bic/orn — to test whether
 * the corruption xor-signature follows the ALU class (the dump campaign
 * showed add = carry-chain look, eor = few low-bit flips). Same slot
 * geometry as neon_rot_ldr_at_top_sve otherwise: dead 3rd ldr z
 * (scratch) at iteration top + 2 source loads + back-to-back store
 * pair, 1024 slots.
 *
 * Instruction note (documented): GCC 12's SVE1 ACLE has no svorn —
 * ORN(a,b) = a | ~b is composed as svorr(a, svnot(b)); BIC maps
 * directly to svbic. Everything else maps 1:1.
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

#define NEON_ROT_LDR_AT_TOP_SUB_SVE_SLOTS 1024

struct NeonRotLdrAtTopSubSveData {
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

static int neon_rot_ldr_at_top_sub_sve_init(struct test *test) {
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_sub_sve");
        return EXIT_SKIP;
    }

    const int lanes = (int)svcntd();
    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_SUB_SVE_SLOTS * lanes;

    auto *data = static_cast<NeonRotLdrAtTopSubSveData *>(malloc(sizeof(NeonRotLdrAtTopSubSveData)));
    data->lanes = lanes;
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->scratch = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    memset_random(data->srcA, elems * sizeof(uint64_t));
    memset_random(data->srcB, elems * sizeof(uint64_t));
    memset_random(data->scratch, elems * sizeof(uint64_t));

    /* golden: 同一旋转组合 (sub/eor/bic/orn), 在相同 SVE 单元上预计算 */
    svbool_t pg = svptrue_b64();
    for (int i = 0; i < NEON_ROT_LDR_AT_TOP_SUB_SVE_SLOTS; i++) {
        svuint64_t a = svld1_u64(pg, data->srcA + (size_t)i * lanes);
        svuint64_t b = svld1_u64(pg, data->srcB + (size_t)i * lanes);
        svuint64_t res;
        switch (i % 4) {
            case 0: res = svsub_u64_x(pg, a, b); break;
            case 1: res = sveor_u64_x(pg, a, b); break;
            case 2: res = svbic_u64_x(pg, a, b); break;
            case 3: res = svorr_u64_x(pg, a, svnot_u64_x(pg, b)); break;  /* ORN */
        }
        svst1_u64(pg, data->expected + (size_t)i * lanes, res);
    }

    test->data = data;
    return EXIT_SUCCESS;
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_sub_sve");
    return EXIT_SKIP;
#endif
}

#if defined(__aarch64__)
static int neon_rot_ldr_at_top_sub_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<NeonRotLdrAtTopSubSveData *>(test->data);

    if ((int)svcntd() != data->lanes) {
        log_error("neon_rot_ldr_at_top_sub_sve: vector length changed between init (%d) and run (%d)",
                  data->lanes, (int)svcntd());
        report_fail_msg("neon_rot_ldr_at_top_sub_sve: vector length changed between init and run");
    }

    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_SUB_SVE_SLOTS * data->lanes;
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < NEON_ROT_LDR_AT_TOP_SUB_SVE_SLOTS; i++) {
            const size_t off = (size_t)i * data->lanes;

            /* 3rd load at the TOP (value discarded — position is the probe) */
            svuint64_t unused_x = load_vec_sve(data->scratch + off);
            (void)unused_x;

            svuint64_t a = load_vec_sve(data->srcA + off);
            svuint64_t b = load_vec_sve(data->srcB + off);
            svuint64_t res;

            switch (i % 4) {
                case 0: res = svsub_u64_x(svptrue_b64(), a, b); break;
                case 1: res = sveor_u64_x(svptrue_b64(), a, b); break;
                case 2: res = svbic_u64_x(svptrue_b64(), a, b); break;
                case 3: res = svorr_u64_x(svptrue_b64(), a, svnot_u64_x(svptrue_b64(), b)); break;
            }

            store_vec_sve(temp + off, res);
            store_vec_sve(dst + off, res);
        }

        if (memcmp(dst, data->expected, elems * sizeof(uint64_t)) != 0) {
            report_fail_msg("neon_rot_ldr_at_top_sub_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int neon_rot_ldr_at_top_sub_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_sub_sve");
    return EXIT_SKIP;
}
#endif

static int neon_rot_ldr_at_top_sub_sve_cleanup(struct test *test) {
    auto *data = static_cast<NeonRotLdrAtTopSubSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_ldr_at_top_sub_sve,
             "SVE ALU-dimension variant of the position probe: rotation sub/eor/bic/orn "
             "(carry vs combinational signature test; ORN composed as orr+not) at forward-only "
             "sweep with 3rd ldr z at top (port of neon_rot_ldr_at_top_sub)")
    .test_init    = neon_rot_ldr_at_top_sub_sve_init,
    .test_run     = neon_rot_ldr_at_top_sub_sve_run,
    .test_cleanup = neon_rot_ldr_at_top_sub_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
