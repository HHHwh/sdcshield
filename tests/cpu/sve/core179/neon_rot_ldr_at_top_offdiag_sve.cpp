/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_ldr_at_top_offdiag_sve
 * @parblock
 * SVE port of neon_rot_ldr_at_top_offdiag (source:
 * missing_testcases_20261009/tests/cpu/misc/neon_rot_ldr_at_top_offdiag.cpp).
 *
 * Address-layout variant of the position probe (original-parent lineage,
 * forward-only sweep): the two source vectors are loaded from ONE
 * interleaved buffer at vector indices 3i+1 and 3i+2 — adjacent
 * addresses (one vector apart), vs the parent's two parallel buffers
 * whose slots are a full buffer apart. Same total load footprint as the
 * parent (3 vectors' worth per iteration including the dead scratch
 * load); tests whether source-load address separation (different cache
 * sets/lines) is a necessary trigger condition. Rotation reverts to the
 * parent's add/eor/and/orr.
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

#define NEON_ROT_LDR_AT_TOP_OFFDIAG_SVE_SLOTS 1024

struct NeonRotLdrAtTopOffdiagSveData {
    uint64_t *interleaved;   /* 3 x SLOTS vectors; src pair at [3i+1],[3i+2] */
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

static int neon_rot_ldr_at_top_offdiag_sve_init(struct test *test) {
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_offdiag_sve");
        return EXIT_SKIP;
    }

    const int lanes = (int)svcntd();
    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_OFFDIAG_SVE_SLOTS * lanes;

    auto *data = static_cast<NeonRotLdrAtTopOffdiagSveData *>(malloc(sizeof(NeonRotLdrAtTopOffdiagSveData)));
    data->lanes = lanes;
    /* 交错缓冲: 3 x SLOTS 向量 (与父系等量加载足迹) */
    data->interleaved = static_cast<uint64_t *>(aligned_alloc(64, 3 * elems * sizeof(uint64_t)));
    data->scratch = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    memset_random(data->interleaved, 3 * elems * sizeof(uint64_t));
    memset_random(data->scratch, elems * sizeof(uint64_t));

    /* golden: 源对 [3i+1]/[3i+2], 父系旋转 add/eor/and/orr, 同 SVE 单元 */
    svbool_t pg = svptrue_b64();
    for (int i = 0; i < NEON_ROT_LDR_AT_TOP_OFFDIAG_SVE_SLOTS; i++) {
        svuint64_t a = svld1_u64(pg, data->interleaved + (size_t)(3 * i + 1) * lanes);
        svuint64_t b = svld1_u64(pg, data->interleaved + (size_t)(3 * i + 2) * lanes);
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
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_offdiag_sve");
    return EXIT_SKIP;
#endif
}

#if defined(__aarch64__)
static int neon_rot_ldr_at_top_offdiag_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<NeonRotLdrAtTopOffdiagSveData *>(test->data);

    if ((int)svcntd() != data->lanes) {
        log_error("neon_rot_ldr_at_top_offdiag_sve: vector length changed between init (%d) and run (%d)",
                  data->lanes, (int)svcntd());
        report_fail_msg("neon_rot_ldr_at_top_offdiag_sve: vector length changed between init and run");
    }

    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_OFFDIAG_SVE_SLOTS * data->lanes;
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < NEON_ROT_LDR_AT_TOP_OFFDIAG_SVE_SLOTS; i++) {
            const size_t off = (size_t)i * data->lanes;

            svuint64_t unused_x = load_vec_sve(data->scratch + off);
            (void)unused_x;

            /* 两个源载相邻地址 (相差一个向量) — 地址间隔必要性探针 */
            svuint64_t a = load_vec_sve(data->interleaved + (size_t)(3 * i + 1) * data->lanes);
            svuint64_t b = load_vec_sve(data->interleaved + (size_t)(3 * i + 2) * data->lanes);
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

        if (memcmp(dst, data->expected, elems * sizeof(uint64_t)) != 0) {
            report_fail_msg("neon_rot_ldr_at_top_offdiag_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int neon_rot_ldr_at_top_offdiag_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_offdiag_sve");
    return EXIT_SKIP;
}
#endif

static int neon_rot_ldr_at_top_offdiag_sve_cleanup(struct test *test) {
    auto *data = static_cast<NeonRotLdrAtTopOffdiagSveData *>(test->data);
    if (data) {
        free(data->interleaved);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_ldr_at_top_offdiag_sve,
             "SVE interleave variant of the position probe: source pairs adjacent in ONE "
             "interleaved buffer (address-separation necessity test) at forward-only sweep with "
             "3rd ldr z at top (port of neon_rot_ldr_at_top_offdiag)")
    .test_init    = neon_rot_ldr_at_top_offdiag_sve_init,
    .test_run     = neon_rot_ldr_at_top_offdiag_sve_run,
    .test_cleanup = neon_rot_ldr_at_top_offdiag_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
