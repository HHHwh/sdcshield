/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b agu_stress_2src_sve
 * @parblock
 * SVE port of agu_stress_2src: 2-source vector loads (asm-guaranteed ldr z
 * — the SVE form of the scalar 2-src ldr AGU pattern), rotating
 * add/eor/and/or composition, back-to-back str z -> ldr z -> str z
 * store/reload/store chain maximizing address-generation pressure on
 * vector lanes.
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

#define AGU_STRESS_2SRC_SVE_COUNT 1024   /* uint64 elements per buffer */

struct AguStress2srcSveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *expected;
};

#if defined(__aarch64__)

static inline void store_vec_sve(void *addr, svuint64_t val) {
    svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)svcntd());
    svst1_u64(pg, static_cast<uint64_t *>(addr), val);
}

static inline svuint64_t load_vec_sve(const void *addr) {
    svuint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

#endif

static int agu_stress_2src_sve_init(struct test *test) {
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for agu_stress_2src_sve");
        return EXIT_SKIP;
    }
#endif
    auto *data = static_cast<AguStress2srcSveData *>(malloc(sizeof(AguStress2srcSveData)));
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, AGU_STRESS_2SRC_SVE_COUNT * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, AGU_STRESS_2SRC_SVE_COUNT * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, AGU_STRESS_2SRC_SVE_COUNT * sizeof(uint64_t)));

    memset_random(data->srcA, AGU_STRESS_2SRC_SVE_COUNT * sizeof(uint64_t));
    memset_random(data->srcB, AGU_STRESS_2SRC_SVE_COUNT * sizeof(uint64_t));

    /* golden: same rotating composition, computed on the same SVE units */
    for (int i = 0; i < AGU_STRESS_2SRC_SVE_COUNT; i++) {
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
static int agu_stress_2src_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<AguStress2srcSveData *>(test->data);

    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, AGU_STRESS_2SRC_SVE_COUNT * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, AGU_STRESS_2SRC_SVE_COUNT * sizeof(uint64_t)));
    const int lanes = svcntd();
    svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)lanes);

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < AGU_STRESS_2SRC_SVE_COUNT; base += lanes) {
            /* 2 源加载: asm 保证的 ldr z */
            svuint64_t a = load_vec_sve(data->srcA + base);
            svuint64_t b = load_vec_sve(data->srcB + base);
            svuint64_t res;

            /* 旋转 ALU 组合: 逐 lane 相位 (base+k)%4, 相位谓词分派 (与 golden i%4 一致) */
            {
                svuint64_t r_add = svadd_u64_x(pg, a, b);
                svuint64_t r_eor = sveor_u64_x(pg, a, b);
                svuint64_t r_and = svand_u64_x(pg, a, b);
                svuint64_t r_orr = svorr_u64_x(pg, a, b);
                uint64_t phase[8];
                int n = (AGU_STRESS_2SRC_SVE_COUNT - base < (size_t)lanes) ? (int)(AGU_STRESS_2SRC_SVE_COUNT - base) : lanes;
                for (int k = 0; k < n; ++k) phase[k] = (base + k) % 4;
                svbool_t pgn = svwhilelt_b64((uint64_t)0, (uint64_t)n);
                svuint64_t vphase = svld1_u64(pgn, phase);
                res = svsel_u64(svcmpeq_n_u64(pgn, vphase, 0), r_add,
                     svsel_u64(svcmpeq_n_u64(pgn, vphase, 1), r_eor,
                     svsel_u64(svcmpeq_n_u64(pgn, vphase, 2), r_and, r_orr)));
            }

            /* 放大器: str z -> ldr z -> str z */
            store_vec_sve(temp + base, res);
            store_vec_sve(dst + base, load_vec_sve(temp + base));
        }

        if (memcmp(dst, data->expected, AGU_STRESS_2SRC_SVE_COUNT * sizeof(uint64_t)) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < (size_t)(AGU_STRESS_2SRC_SVE_COUNT); ++_dump_j) {
                    if (dst[_dump_j] != data->expected[_dump_j]) {
                        ++_dump_bad;
                        if (_dump_shown < 8) {
                            log_error("agu_stress_2src_sve: mismatch at index %zu: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                                      _dump_j, (unsigned long long)data->expected[_dump_j],
                                      (unsigned long long)dst[_dump_j],
                                      (unsigned long long)(data->expected[_dump_j] ^ dst[_dump_j]));
                            ++_dump_shown;
                        }
                    }
                }
                log_error("agu_stress_2src_sve: total mismatched elements: %zu / %d", _dump_bad,
                          AGU_STRESS_2SRC_SVE_COUNT);
            }
            report_fail_msg("agu_stress_2src_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int agu_stress_2src_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for agu_stress_2src_sve");
    return EXIT_SKIP;
}
#endif

static int agu_stress_2src_sve_cleanup(struct test *test) {
    auto *data = static_cast<AguStress2srcSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(agu_stress_2src_sve,
             "SVE AGU throughput stress: 2-src ldr z + rotating svadd/sveor/svand/svorr "
             "+ back-to-back str z/ldr z/str z (port of agu_stress_2src)")
    .test_init = agu_stress_2src_sve_init,
    .test_run = agu_stress_2src_sve_run,
    .test_cleanup = agu_stress_2src_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
