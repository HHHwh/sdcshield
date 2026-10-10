/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_reloaded_2src_alu_sve
 * @parblock
 * SVE port of mrn_reloaded_2src_alu (source: missing_testcases_20261009/
 * tests/cpu/misc/mrn_reloaded_2src_alu.cpp).
 *
 * qword-segment-only variant of the mrn_reloaded lineage: 2-source load
 * + rotating {+,-,^,&} ALU, then svst1(res -> temp) immediately followed
 * by the asm-guaranteed ldr z of temp (back-to-back forwarding), verified
 * IMMEDIATELY per batch (vector compare + cold per-element rescan) —
 * no dst array, TEST_LOOP quantum 1<<8 like the scalar original (the
 * 179-machine all-time highest-rate case: 126 fail/36min on the scalar
 * path).
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

#define MRN_RELOADED_2SRC_ALU_SVE_COUNT 1024

struct MrnReloaded2srcAluSveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *expected;
};

static int mrn_reloaded_2src_alu_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for mrn_reloaded_2src_alu_sve");
        return EXIT_SKIP;
    }
#endif
    auto *data = static_cast<MrnReloaded2srcAluSveData *>(malloc(sizeof(MrnReloaded2srcAluSveData)));
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, MRN_RELOADED_2SRC_ALU_SVE_COUNT * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, MRN_RELOADED_2SRC_ALU_SVE_COUNT * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, MRN_RELOADED_2SRC_ALU_SVE_COUNT * sizeof(uint64_t)));

    memset_random(data->srcA, MRN_RELOADED_2SRC_ALU_SVE_COUNT * sizeof(uint64_t));
    memset_random(data->srcB, MRN_RELOADED_2SRC_ALU_SVE_COUNT * sizeof(uint64_t));

    for (int i = 0; i < MRN_RELOADED_2SRC_ALU_SVE_COUNT; i++) {
        switch (i % 4) {
            case 0: data->expected[i] = data->srcA[i] + data->srcB[i]; break;
            case 1: data->expected[i] = data->srcA[i] - data->srcB[i]; break;
            case 2: data->expected[i] = data->srcA[i] ^ data->srcB[i]; break;
            case 3: data->expected[i] = data->srcA[i] & data->srcB[i]; break;
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

static int mrn_reloaded_2src_alu_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<MrnReloaded2srcAluSveData *>(test->data);

    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, MRN_RELOADED_2SRC_ALU_SVE_COUNT * sizeof(uint64_t)));
    const int lanes = svcntd();
    uint64_t got_bytes[64];

    TEST_LOOP(test, 1 << 8) {
        for (size_t base = 0; base < MRN_RELOADED_2SRC_ALU_SVE_COUNT; base += lanes) {
            int n = (MRN_RELOADED_2SRC_ALU_SVE_COUNT - base < (size_t)lanes)
                        ? (int)(MRN_RELOADED_2SRC_ALU_SVE_COUNT - base) : lanes;
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

            /* str temp -> 立即 ldr z (背靠背转发); 无 dst — 立即验证 */
            svst1_u64(pg, temp + base, res);
            svuint64_t r = load_vec_sve(temp + base);

            svuint64_t vexp = svld1_u64(pg, data->expected + base);
            if (svptest_any(pg, svcmpne_u64(pg, r, vexp))) {
                svst1_u64(pg, got_bytes, r);
                for (int k = 0; k < n; ++k) {
                    if (got_bytes[k] != data->expected[base + k]) {
                        report_fail_msg("mrn_reloaded_2src_alu_sve qword miscompare at %zu: "
                                        "expected 0x%llx got 0x%llx",
                                        (size_t)(base + k),
                                        (unsigned long long)data->expected[base + k],
                                        (unsigned long long)got_bytes[k]);
                    }
                }
            }
        }
    }

    free(temp);
    return EXIT_SUCCESS;
}

#else

static int mrn_reloaded_2src_alu_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for mrn_reloaded_2src_alu_sve");
    return EXIT_SKIP;
}

#endif

static int mrn_reloaded_2src_alu_sve_cleanup(struct test *test)
{
    auto *data = static_cast<MrnReloaded2srcAluSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mrn_reloaded_2src_alu_sve,
             "SVE mrn_reloaded variant: 2-source load + data-ALU on the qword segment, "
             "back-to-back str/ldr with immediate verify (port of mrn_reloaded_2src_alu)")
    .test_init = mrn_reloaded_2src_alu_sve_init,
    .test_run = mrn_reloaded_2src_alu_sve_run,
    .test_cleanup = mrn_reloaded_2src_alu_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
