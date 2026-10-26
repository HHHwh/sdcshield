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
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

#define MRN_NUKE_SVE_COUNT 1024

/* 元素数 count — 运行时可配置, 统一优先级链:
 *   1. -O mrn_nuke_sve.count=N / 配置文件同名条目
 *   2. 环境变量 SDC_MRN_NUKE_SVE_COUNT
 *   3. 设计默认 1024
 * （core179 诊断探针: 未配置时默认 = 原硬编码设计几何, 不做内存
 *   自适应 — 缓冲几何本身是激发参数; 显式配置 clamp 到
 *   [1024, 1<<22]。非 8 的倍数 log_warning + 向下取整, 保持 u64
 *   缓冲字节数的 64B cache-line 对齐。） */
static size_t mrn_nuke_sve_count(struct test *test)
{{
    const int64_t dflt = MRN_NUKE_SVE_COUNT;
    const int64_t hi = 1ll << 22;
    int64_t v = sdc_knob_int_clamped(test, "count", 0, dflt, hi);
    if (v == 0 && sdc_env_knob_int(test->id, "count", &v)
        && (v < dflt || v > hi)) {{
        int64_t c = (v < dflt) ? dflt : hi;
        log_warning("%s: count=%ld 越界 (有效 %ld..%ld), 已 clamp 为 %ld",
                    test->id, (long)v, (long)dflt, (long)hi, (long)c);
        v = c;
    }}
    if (v <= 0)
        v = dflt;
    if (v % 8 != 0) {{
        log_warning("%s: count=%ld 非 8 的倍数, 向下取整为 %ld (保持 64B 缓冲对齐)",
                    test->id, (long)v, (long)(v - v % 8));
        v -= v % 8;
    }}
    return (size_t)v;
}}

struct MrnNukeSveData {
    uint64_t *src;
    size_t count;
};

static int mrn_nuke_sve_init(struct test *test)
{{
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: SVE capability + 128..1024-bit VL range
     * gate in one framework-layer call (device/cpu/arm64/arm64_vl.h). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif
    auto *data = static_cast<MrnNukeSveData *>(malloc(sizeof(MrnNukeSveData)));
    data->count = mrn_nuke_sve_count(test);
    data->src = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    memset_random(data->src, data->count * sizeof(uint64_t));
    test->data = data;
    return EXIT_SUCCESS;
}}

#if defined(__aarch64__)
static int mrn_nuke_sve_run(struct test *test, int cpu)
{{
    (void)cpu;
    auto *data = static_cast<MrnNukeSveData *>(test->data);
    const int lanes = svcntd();

    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));

    TEST_LOOP(test, 1 << 13) {{
        for (size_t base = 0; base < data->count; base += lanes) {{
            int n = (data->count - base < (size_t)lanes) ? (int)(data->count - base) : lanes;
            svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)n);
            /* 向量 store 到 temp, 立即向量 load (store->load 转发依赖) */
            svst1_u64(pg, temp + base, svld1_u64(pg, data->src + base));
            svst1_u64(pg, dst + base, svld1_u64(pg, temp + base));
        }}
        if (memcmp(dst, data->src, data->count * sizeof(uint64_t)) != 0) {{
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < data->count; ++_dump_j) {
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
                log_error("mrn_nuke_sve: total mismatched elements: %zu / %zu",
                          _dump_bad, data->count);
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
