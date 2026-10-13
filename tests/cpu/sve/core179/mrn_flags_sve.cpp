/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_flags_sve
 * @parblock
 * SVE port of mrn_flags: bitwise NOT + zero-conditional magic-select on
 * vector lanes (svnot + svcmpqe/svsel replacing the scalar MVN/CMP/CSEL),
 * with the same golden rule (result of ~x, or the magic constant when ~x==0).
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

#define MRN_FLAGS_SVE_COUNT 1024

struct MrnFlagsSveData {
    uint64_t *src;
    uint64_t *expected;
};

static int mrn_flags_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for mrn_flags_sve");
        return EXIT_SKIP;
    }
#endif
    auto *data = static_cast<MrnFlagsSveData *>(malloc(sizeof(MrnFlagsSveData)));
    data->src = static_cast<uint64_t *>(aligned_alloc(64, MRN_FLAGS_SVE_COUNT * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, MRN_FLAGS_SVE_COUNT * sizeof(uint64_t)));
    memset_random(data->src, MRN_FLAGS_SVE_COUNT * sizeof(uint64_t));
    for (int i = 0; i < MRN_FLAGS_SVE_COUNT; ++i) {
        uint64_t flipped = ~data->src[i];
        data->expected[i] = (flipped == 0) ? 0xDEADBEEFDEADBEEFULL : flipped;
    }
    test->data = data;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)
static int mrn_flags_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<MrnFlagsSveData *>(test->data);
    uint64_t *dst = static_cast<uint64_t *>(aligned_alloc(64, MRN_FLAGS_SVE_COUNT * sizeof(uint64_t)));
    const int lanes = svcntd();
    const svuint64_t magic = svdup_u64(0xDEADBEEFDEADBEEFULL);

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < MRN_FLAGS_SVE_COUNT; base += lanes) {
            int n = (MRN_FLAGS_SVE_COUNT - base < (size_t)lanes) ? (int)(MRN_FLAGS_SVE_COUNT - base) : lanes;
            svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)n);
            svuint64_t v = svld1_u64(pg, data->src + base);
            svuint64_t flipped = svnot_u64_x(pg, v);
            /* zero -> magic (谓词选择替代标量 CSEL) */
            svbool_t is_zero = svcmpeq_n_u64(pg, flipped, 0);
            svst1_u64(pg, dst + base, svsel_u64(is_zero, magic, flipped));
        }
        if (memcmp(dst, data->expected, MRN_FLAGS_SVE_COUNT * sizeof(uint64_t)) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < (size_t)(MRN_FLAGS_SVE_COUNT); ++_dump_j) {
                    if (dst[_dump_j] != data->expected[_dump_j]) {
                        ++_dump_bad;
                        if (_dump_shown < 8) {
                            log_error("mrn_flags_sve: mismatch at index %zu: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                                      _dump_j,
                                      (unsigned long long)data->expected[_dump_j],
                                      (unsigned long long)dst[_dump_j],
                                      (unsigned long long)(data->expected[_dump_j] ^ dst[_dump_j]));
                            ++_dump_shown;
                        }
                    }
                }
                log_error("mrn_flags_sve: total mismatched elements: %zu / %d",
                          _dump_bad, MRN_FLAGS_SVE_COUNT);
            }
            report_fail_msg("mrn_flags_sve data miscompare");
        }
    }
    free(dst);
    return EXIT_SUCCESS;
}
#else
static int mrn_flags_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for mrn_flags_sve");
    return EXIT_SKIP;
}
#endif

static int mrn_flags_sve_cleanup(struct test *test)
{
    auto *data = static_cast<MrnFlagsSveData *>(test->data);
    if (data) {
        free(data->src);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mrn_flags_sve, "SVE bitwise-not + zero-conditional magic select on vector lanes (port of mrn_flags)")
    .test_init = mrn_flags_sve_init,
    .test_run = mrn_flags_sve_run,
    .test_cleanup = mrn_flags_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
