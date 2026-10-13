/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_nuke_sve
 * @parblock
 * SVE port of mrn_nuke: array copy through a temp buffer on vector lanes —
 * svst1-to-temp then svld1-from-temp store->load forwarding dependency
 * stressing the memory-renaming logic.
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

#define MRN_NUKE_SVE_COUNT 1024

struct MrnNukeSveData {
    uint64_t *src;
};

static int mrn_nuke_sve_init(struct test *test)
{{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {{
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for mrn_nuke_sve");
        return EXIT_SKIP;
    }}
#endif
    auto *data = static_cast<MrnNukeSveData *>(malloc(sizeof(MrnNukeSveData)));
    data->src = static_cast<uint64_t *>(aligned_alloc(64, MRN_NUKE_SVE_COUNT * sizeof(uint64_t)));
    memset_random(data->src, MRN_NUKE_SVE_COUNT * sizeof(uint64_t));
    test->data = data;
    return EXIT_SUCCESS;
}}

#if defined(__aarch64__)
static int mrn_nuke_sve_run(struct test *test, int cpu)
{{
    (void)cpu;
    auto *data = static_cast<MrnNukeSveData *>(test->data);
    const int lanes = svcntd();

    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, MRN_NUKE_SVE_COUNT * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, MRN_NUKE_SVE_COUNT * sizeof(uint64_t)));

    TEST_LOOP(test, 1 << 13) {{
        for (size_t base = 0; base < MRN_NUKE_SVE_COUNT; base += lanes) {{
            int n = (MRN_NUKE_SVE_COUNT - base < (size_t)lanes) ? (int)(MRN_NUKE_SVE_COUNT - base) : lanes;
            svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)n);
            /* 向量 store 到 temp, 立即向量 load (store->load 转发依赖) */
            svst1_u64(pg, temp + base, svld1_u64(pg, data->src + base));
            svst1_u64(pg, dst + base, svld1_u64(pg, temp + base));
        }}
        if (memcmp(dst, data->src, MRN_NUKE_SVE_COUNT * sizeof(uint64_t)) != 0) {{
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < (size_t)(MRN_NUKE_SVE_COUNT); ++_dump_j) {
                    if (dst[_dump_j] != data->src[_dump_j]) {
                        ++_dump_bad;
                        if (_dump_shown < 8) {
                            log_error("mrn_nuke_sve: mismatch at index %zu: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                                      _dump_j,
                                      (unsigned long long)data->src[_dump_j],
                                      (unsigned long long)dst[_dump_j],
                                      (unsigned long long)(data->src[_dump_j] ^ dst[_dump_j]));
                            ++_dump_shown;
                        }
                    }
                }
                log_error("mrn_nuke_sve: total mismatched elements: %zu / %d",
                          _dump_bad, MRN_NUKE_SVE_COUNT);
            }
            report_fail_msg("mrn_nuke_sve data miscompare");
        }}
    }}
    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}}
#else
static int mrn_nuke_sve_run(struct test *test, int cpu)
{{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for mrn_nuke_sve");
    return EXIT_SKIP;
}}
#endif

static int mrn_nuke_sve_cleanup(struct test *test)
{{
    auto *data = static_cast<MrnNukeSveData *>(test->data);
    if (data) {{
        free(data->src);
        free(data);
    }}
    return EXIT_SUCCESS;
}}

DECLARE_TEST(mrn_nuke_sve, "SVE array copy via temp buffer stressing memory renaming (port of mrn_nuke)")
    .test_init = mrn_nuke_sve_init,
    .test_run = mrn_nuke_sve_run,
    .test_cleanup = mrn_nuke_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
