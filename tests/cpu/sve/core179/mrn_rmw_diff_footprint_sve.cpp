/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_rmw_diff_64k_sve
 * @test @b mrn_rmw_diff_1m_sve
 * @parblock
 * SVE ports of mrn_rmw_diff_64k / mrn_rmw_diff_1m (footprint probes;
 * source: missing_testcases_20261009/tests/cpu/misc/mrn_rmw_diff_footprint.cpp).
 *
 * Same diff-address str->ldr skeleton as mrn_rmw_diff_sve, with only the
 * tempB footprint varying (mrn_rmw_diff_sve = 8 KB):
 *
 *   - mrn_rmw_diff_64k_sve: tempB batch set = 64 KB == L1D — does the
 *     L1D-array-read trigger rate change at the capacity/thrash edge?
 *   - mrn_rmw_diff_1m_sve: 1 MB = 16x L1D — every reload misses L1D and
 *     is served from L2. If the SDC needs an L1D hit, it should vanish.
 *
 * The two tests share one parameterized implementation (count from the
 * data struct, log tag from test->id) — the structure of the scalar
 * original, whose shared run also carries no regime branch: the count
 * and tag are read once before the hot loop.
 *
 * Race note: tempA is a same-value shared write target (deterministic
 * function of read-only srcA/srcB, never read back) — benign by the
 * same argument as mrn_rmw_diff_sve. Verification reads only
 * tempB/expected (read-only after init).
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

#define MRN_RMW_DIFF_64K_SVE_COUNT 8192    /* 64 KB == L1D (thrash edge) */
#define MRN_RMW_DIFF_1M_SVE_COUNT 131072   /* 1 MB == 16x L1D */

struct MrnRmwDiffFpSveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *expected;
    uint64_t *tempA;
    uint64_t *tempB;
    int count;
};

static int mrn_rmw_diff_fp_sve_init_common(struct test *test, int count)
{
    auto *data = static_cast<MrnRmwDiffFpSveData *>(malloc(sizeof(MrnRmwDiffFpSveData)));
    data->count = count;
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, (size_t)count * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, (size_t)count * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, (size_t)count * sizeof(uint64_t)));
    data->tempA = static_cast<uint64_t *>(aligned_alloc(64, (size_t)count * sizeof(uint64_t)));
    data->tempB = static_cast<uint64_t *>(aligned_alloc(64, (size_t)count * sizeof(uint64_t)));

    memset_random(data->srcA, (size_t)count * sizeof(uint64_t));
    memset_random(data->srcB, (size_t)count * sizeof(uint64_t));

    for (int i = 0; i < count; i++) {
        switch (i % 4) {
            case 0: data->expected[i] = data->srcA[i] + data->srcB[i]; break;
            case 1: data->expected[i] = data->srcA[i] - data->srcB[i]; break;
            case 2: data->expected[i] = data->srcA[i] ^ data->srcB[i]; break;
            case 3: data->expected[i] = data->srcA[i] & data->srcB[i]; break;
        }
        data->tempB[i] = data->expected[i];
        data->tempA[i] = 0;
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int mrn_rmw_diff_64k_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for mrn_rmw_diff_64k_sve");
        return EXIT_SKIP;
    }
#endif
    return mrn_rmw_diff_fp_sve_init_common(test, MRN_RMW_DIFF_64K_SVE_COUNT);
}

static int mrn_rmw_diff_1m_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for mrn_rmw_diff_1m_sve");
        return EXIT_SKIP;
    }
#endif
    return mrn_rmw_diff_fp_sve_init_common(test, MRN_RMW_DIFF_1M_SVE_COUNT);
}

static int mrn_rmw_diff_fp_sve_cleanup(struct test *test)
{
    auto *data = static_cast<MrnRmwDiffFpSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data->tempA);
        free(data->tempB);
        free(data);
    }
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)

static inline svuint64_t load_vec_sve(const void *addr)
{
    svuint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

/* 共享 run: count/tag 循环外一次读取, 热循环无 regime 分支 (原版结构) */
static int mrn_rmw_diff_fp_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<MrnRmwDiffFpSveData *>(test->data);
    const char *tag = test->id;
    const int count = data->count;
    const int lanes = svcntd();
    uint64_t got_bytes[64];

    TEST_LOOP(test, 1 << 13) {
        for (int base = 0; base < count; base += lanes) {
            int n = (count - base < lanes) ? (count - base) : lanes;
            svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)n);

            svuint64_t va = svld1_u64(pg, data->srcA + base);
            svuint64_t vb = svld1_u64(pg, data->srcB + base);
            svuint64_t r_add = svadd_u64_x(pg, va, vb);
            svuint64_t r_sub = svsub_u64_x(pg, va, vb);
            svuint64_t r_eor = sveor_u64_x(pg, va, vb);
            svuint64_t r_and = svand_u64_x(pg, va, vb);
            uint64_t phase[8];
            for (int k = 0; k < n; ++k) phase[k] = (base + k) % 4;
            svuint64_t vphase = svld1_u64(pg, phase);
            svuint64_t res = svsel_u64(svcmpeq_n_u64(pg, vphase, 0), r_add,
                             svsel_u64(svcmpeq_n_u64(pg, vphase, 1), r_sub,
                             svsel_u64(svcmpeq_n_u64(pg, vphase, 2), r_eor, r_and)));

            svst1_u64(pg, data->tempA + base, res);
            svuint64_t got = load_vec_sve(data->tempB + base);

            svuint64_t vexp = svld1_u64(pg, data->expected + base);
            if (svptest_any(pg, svcmpne_u64(pg, got, vexp))) {
                svst1_u64(pg, got_bytes, got);
                for (int k = 0; k < n; ++k) {
                    if (got_bytes[k] != data->expected[base + k]) {
                        unsigned i = (unsigned)(base + k);
                        const char *op = (i % 4 == 0) ? "add"
                                       : (i % 4 == 1) ? "sub"
                                       : (i % 4 == 2) ? "xor" : "and";
                        uint64_t golden = data->expected[base + k];
                        uint64_t actual = got_bytes[k];
                        log_error("%s: miscompare at index %u (op=%s): "
                                  "golden=0x%016lX actual=0x%016lX xor=0x%016lX",
                                  tag, i, op, (unsigned long)golden,
                                  (unsigned long)actual,
                                  (unsigned long)(golden ^ actual));
                        report_fail_msg("%s: data miscompare at index %u (op=%s)",
                                        tag, i, op);
                    }
                }
            }
        }
    }

    return EXIT_SUCCESS;
}

#else

static int mrn_rmw_diff_fp_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for mrn_rmw_diff footprint tests");
    return EXIT_SKIP;
}

#endif

DECLARE_TEST(mrn_rmw_diff_64k_sve,
             "SVE mrn_rmw footprint probe (L1D thrash edge): diff-addr str->ldr with tempB=64KB==L1D "
             "— does the L1D-array read trigger at the thrash edge? (port of mrn_rmw_diff_64k)")
    .test_init = mrn_rmw_diff_64k_sve_init,
    .test_run = mrn_rmw_diff_fp_sve_run,
    .test_cleanup = mrn_rmw_diff_fp_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

DECLARE_TEST(mrn_rmw_diff_1m_sve,
             "SVE mrn_rmw footprint probe (L2-reload): diff-addr str->ldr with tempB=1MB=16xL1D — "
             "every reload misses L1D (served from L2); if the SDC needs an L1D hit, it should "
             "vanish (port of mrn_rmw_diff_1m)")
    .test_init = mrn_rmw_diff_1m_sve_init,
    .test_run = mrn_rmw_diff_fp_sve_run,
    .test_cleanup = mrn_rmw_diff_fp_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
