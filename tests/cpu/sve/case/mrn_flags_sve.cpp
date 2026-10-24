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
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

#define MRN_FLAGS_SVE_COUNT 1024

/* 元素数 count — 运行时可配置, 统一优先级链:
 *   1. -O mrn_flags_sve.count=N / 配置文件同名条目
 *   2. 环境变量 SDC_MRN_FLAGS_SVE_COUNT
 *   3. 设计默认 1024
 * （core179 诊断探针: 未配置时默认 = 原硬编码设计几何, 不做内存
 *   自适应 — 缓冲几何本身是激发参数; 显式配置 clamp 到
 *   [1024, 1<<22]。非 8 的倍数 log_warning + 向下取整, 保持 u64
 *   缓冲字节数的 64B cache-line 对齐。） */
static size_t mrn_flags_sve_count(struct test *test)
{
    const int64_t dflt = MRN_FLAGS_SVE_COUNT;
    const int64_t hi = 1ll << 22;
    int64_t v = sdc_knob_int_clamped(test, "count", 0, dflt, hi);
    if (v == 0 && sdc_env_knob_int(test->id, "count", &v)
        && (v < dflt || v > hi)) {
        int64_t c = (v < dflt) ? dflt : hi;
        log_warning("%s: count=%ld 越界 (有效 %ld..%ld), 已 clamp 为 %ld",
                    test->id, (long)v, (long)dflt, (long)hi, (long)c);
        v = c;
    }
    if (v <= 0)
        v = dflt;
    if (v % 8 != 0) {
        log_warning("%s: count=%ld 非 8 的倍数, 向下取整为 %ld (保持 64B 缓冲对齐)",
                    test->id, (long)v, (long)(v - v % 8));
        v -= v % 8;
    }
    return (size_t)v;
}

struct MrnFlagsSveData {
    uint64_t *src;
    uint64_t *expected;
    size_t count;
};

static int mrn_flags_sve_init(struct test *test)
{
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: SVE capability + 128..1024-bit VL range
     * gate in one framework-layer call (device/cpu/arm64/arm64_vl.h). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif
    auto *data = static_cast<MrnFlagsSveData *>(malloc(sizeof(MrnFlagsSveData)));
    data->count = mrn_flags_sve_count(test);
    data->src = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    memset_random(data->src, data->count * sizeof(uint64_t));
    for (size_t i = 0; i < data->count; ++i) {
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
    uint64_t *dst = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    const int lanes = svcntd();
    const svuint64_t magic = svdup_u64(0xDEADBEEFDEADBEEFULL);

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < data->count; base += lanes) {
            int n = (data->count - base < (size_t)lanes) ? (int)(data->count - base) : lanes;
            svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)n);
            svuint64_t v = svld1_u64(pg, data->src + base);
            svuint64_t flipped = svnot_u64_x(pg, v);
            /* zero -> magic (谓词选择替代标量 CSEL) */
            svbool_t is_zero = svcmpeq_n_u64(pg, flipped, 0);
            svst1_u64(pg, dst + base, svsel_u64(is_zero, magic, flipped));
        }
        if (memcmp(dst, data->expected, data->count * sizeof(uint64_t)) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < data->count; ++_dump_j) {
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
                log_error("mrn_flags_sve: total mismatched elements: %zu / %zu",
                          _dump_bad, data->count);
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
