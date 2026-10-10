/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_nuke_4src_alu_sve
 * @parblock
 * SVE port of mrn_nuke_4src_alu (source: missing_testcases_20261009/
 * tests/cpu/misc/mrn_nuke_4src_alu.cpp).
 *
 * Dependency-chain depth axis, depth = 3: 4-source load + data-ALU
 * feeding the svst1 -> ldr z -> svst1 memory-renaming skeleton. Rotating
 * chains per element phase: a^b^c^d / ((a+b)^c)+d / (a^b)+(c^d) /
 * a^(b+c)^d. Golden precomputed in init with the same phase math.
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

#define MRN_NUKE_4SRC_ALU_SVE_COUNT 1024

struct MrnNuke4srcAluSveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *srcC;
    uint64_t *srcD;
    uint64_t *expected;
};

static int mrn_nuke_4src_alu_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for mrn_nuke_4src_alu_sve");
        return EXIT_SKIP;
    }
#endif
    auto *data = static_cast<MrnNuke4srcAluSveData *>(malloc(sizeof(MrnNuke4srcAluSveData)));
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, MRN_NUKE_4SRC_ALU_SVE_COUNT * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, MRN_NUKE_4SRC_ALU_SVE_COUNT * sizeof(uint64_t)));
    data->srcC = static_cast<uint64_t *>(aligned_alloc(64, MRN_NUKE_4SRC_ALU_SVE_COUNT * sizeof(uint64_t)));
    data->srcD = static_cast<uint64_t *>(aligned_alloc(64, MRN_NUKE_4SRC_ALU_SVE_COUNT * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, MRN_NUKE_4SRC_ALU_SVE_COUNT * sizeof(uint64_t)));

    memset_random(data->srcA, MRN_NUKE_4SRC_ALU_SVE_COUNT * sizeof(uint64_t));
    memset_random(data->srcB, MRN_NUKE_4SRC_ALU_SVE_COUNT * sizeof(uint64_t));
    memset_random(data->srcC, MRN_NUKE_4SRC_ALU_SVE_COUNT * sizeof(uint64_t));
    memset_random(data->srcD, MRN_NUKE_4SRC_ALU_SVE_COUNT * sizeof(uint64_t));

    for (int i = 0; i < MRN_NUKE_4SRC_ALU_SVE_COUNT; i++) {
        switch (i % 4) {
            case 0: data->expected[i] = data->srcA[i] ^ data->srcB[i] ^ data->srcC[i] ^ data->srcD[i]; break;
            case 1: data->expected[i] = ((data->srcA[i] + data->srcB[i]) ^ data->srcC[i]) + data->srcD[i]; break;
            case 2: data->expected[i] = (data->srcA[i] ^ data->srcB[i]) + (data->srcC[i] ^ data->srcD[i]); break;
            case 3: data->expected[i] = data->srcA[i] ^ (data->srcB[i] + data->srcC[i]) ^ data->srcD[i]; break;
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

static int mrn_nuke_4src_alu_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<MrnNuke4srcAluSveData *>(test->data);
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, MRN_NUKE_4SRC_ALU_SVE_COUNT * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, MRN_NUKE_4SRC_ALU_SVE_COUNT * sizeof(uint64_t)));
    const int lanes = svcntd();

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < MRN_NUKE_4SRC_ALU_SVE_COUNT; base += lanes) {
            int n = (MRN_NUKE_4SRC_ALU_SVE_COUNT - base < (size_t)lanes)
                        ? (int)(MRN_NUKE_4SRC_ALU_SVE_COUNT - base) : lanes;
            svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)n);

            svuint64_t va = svld1_u64(pg, data->srcA + base);
            svuint64_t vb = svld1_u64(pg, data->srcB + base);
            svuint64_t vc = svld1_u64(pg, data->srcC + base);
            svuint64_t vd = svld1_u64(pg, data->srcD + base);
            svuint64_t r0 = sveor_u64_x(pg, sveor_u64_x(pg, sveor_u64_x(pg, va, vb), vc), vd);
            svuint64_t r1 = svadd_u64_x(pg, sveor_u64_x(pg, svadd_u64_x(pg, va, vb), vc), vd);
            svuint64_t r2 = svadd_u64_x(pg, sveor_u64_x(pg, va, vb), sveor_u64_x(pg, vc, vd));
            svuint64_t r3 = sveor_u64_x(pg, sveor_u64_x(pg, va, svadd_u64_x(pg, vb, vc)), vd);
            uint64_t phase[8];
            for (int k = 0; k < n; ++k) phase[k] = (base + k) % 4;
            svuint64_t vphase = svld1_u64(pg, phase);
            svuint64_t res = svsel_u64(svcmpeq_n_u64(pg, vphase, 0), r0,
                             svsel_u64(svcmpeq_n_u64(pg, vphase, 1), r1,
                             svsel_u64(svcmpeq_n_u64(pg, vphase, 2), r2, r3)));

            svst1_u64(pg, temp + base, res);
            svst1_u64(pg, dst + base, load_vec_sve(temp + base));
        }

        if (memcmp(dst, data->expected, MRN_NUKE_4SRC_ALU_SVE_COUNT * sizeof(uint64_t)) != 0) {
            report_fail_msg("mrn_nuke_4src_alu_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

#else

static int mrn_nuke_4src_alu_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for mrn_nuke_4src_alu_sve");
    return EXIT_SKIP;
}

#endif

static int mrn_nuke_4src_alu_sve_cleanup(struct test *test)
{
    auto *data = static_cast<MrnNuke4srcAluSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->srcC);
        free(data->srcD);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mrn_nuke_4src_alu_sve,
             "SVE mrn_nuke variant: 4-source load + data-ALU (dependency chain depth = 3) on the "
             "str/ldr/str skeleton (port of mrn_nuke_4src_alu)")
    .test_init = mrn_nuke_4src_alu_sve_init,
    .test_run = mrn_nuke_4src_alu_sve_run,
    .test_cleanup = mrn_nuke_4src_alu_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
