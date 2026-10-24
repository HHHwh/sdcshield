/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_reloaded_sve
 * @parblock
 * SVE port of mrn_reloaded: store->immediate-reload at three widths
 * (8/32/64-bit) on vector lanes (svst1/svld1 batches at matching widths),
 * per-element verification exactly as the original.
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

#define MRN_RELOADED_SVE_COUNT 256

/* 元素数 count — 运行时可配置, 统一优先级链:
 *   1. -O mrn_reloaded_sve.count=N / 配置文件同名条目
 *   2. 环境变量 SDC_MRN_RELOADED_SVE_COUNT
 *   3. 设计默认 256
 * （core179 诊断探针: 未配置时默认 = 原硬编码设计几何, 不做内存
 *   自适应 — 缓冲几何本身是激发参数; 显式配置 clamp 到
 *   [256, 1<<22]。非 8 的倍数 log_warning + 向下取整, 保持 u64
 *   缓冲字节数的 64B cache-line 对齐。count 同时约束 8/32/64-bit
 *   三条宽度批 — 与原版共享同一元素数设计。） */
static size_t mrn_reloaded_sve_count(struct test *test)
{
    const int64_t dflt = MRN_RELOADED_SVE_COUNT;
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

struct MrnReloadedSveData {
    uint8_t *src8;
    uint32_t *src32;
    uint64_t *src64;
    size_t count;
};

static int mrn_reloaded_sve_init(struct test *test)
{
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: SVE capability + 128..1024-bit VL range
     * gate in one framework-layer call (device/cpu/arm64/arm64_vl.h). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif
    auto *data = static_cast<MrnReloadedSveData *>(malloc(sizeof(MrnReloadedSveData)));
    data->count = mrn_reloaded_sve_count(test);
    data->src8  = static_cast<uint8_t  *>(aligned_alloc(64, data->count * sizeof(uint8_t)));
    data->src32 = static_cast<uint32_t *>(aligned_alloc(64, data->count * sizeof(uint32_t)));
    data->src64 = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    memset_random(data->src8,  data->count * sizeof(uint8_t));
    memset_random(data->src32, data->count * sizeof(uint32_t));
    memset_random(data->src64, data->count * sizeof(uint64_t));
    test->data = data;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)
static int mrn_reloaded_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<MrnReloadedSveData *>(test->data);

    uint8_t  *temp8  = static_cast<uint8_t  *>(aligned_alloc(64, data->count * sizeof(uint8_t)));
    uint32_t *temp32 = static_cast<uint32_t *>(aligned_alloc(64, data->count * sizeof(uint32_t)));
    uint64_t *temp64 = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));

    TEST_LOOP(test, 1 << 8) {
        /* 8-bit 批 */
        for (size_t base = 0; base < data->count; base += svcntb()) {
            int n = (data->count - base < (size_t)svcntb()) ? (int)(data->count - base) : svcntb();
            svbool_t pg = svwhilelt_b8((uint64_t)0, (uint64_t)n);
            svst1_u8(pg, temp8 + base, svld1_u8(pg, data->src8 + base));
            uint8_t got[64];
            svst1_u8(pg, got, svld1_u8(pg, temp8 + base));
            for (int i = 0; i < n; ++i)
                if (got[i] != data->src8[base + i])
                    report_fail_msg("mrn_reloaded_sve 1-byte miscompare at %d: "
                                    "golden=0x%02x actual=0x%02x xor=0x%02x",
                                    (int)(base + i), (unsigned)data->src8[base + i],
                                    (unsigned)got[i], (unsigned)(data->src8[base + i] ^ got[i]));
        }
        /* 32-bit 批 */
        for (size_t base = 0; base < data->count; base += svcntw()) {
            int n = (data->count - base < (size_t)svcntw()) ? (int)(data->count - base) : svcntw();
            svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)n);
            svst1_u32(pg, temp32 + base, svld1_u32(pg, data->src32 + base));
            uint32_t got[16];
            svst1_u32(pg, got, svld1_u32(pg, temp32 + base));
            for (int i = 0; i < n; ++i)
                if (got[i] != data->src32[base + i])
                    report_fail_msg("mrn_reloaded_sve 4-byte miscompare at %d: "
                                    "golden=0x%08x actual=0x%08x xor=0x%08x",
                                    (int)(base + i), (unsigned)data->src32[base + i],
                                    (unsigned)got[i], (unsigned)(data->src32[base + i] ^ got[i]));
        }
        /* 64-bit 批 */
        for (size_t base = 0; base < data->count; base += svcntd()) {
            int n = (data->count - base < (size_t)svcntd()) ? (int)(data->count - base) : svcntd();
            svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)n);
            svst1_u64(pg, temp64 + base, svld1_u64(pg, data->src64 + base));
            uint64_t got[8];
            svst1_u64(pg, got, svld1_u64(pg, temp64 + base));
            for (int i = 0; i < n; ++i)
                if (got[i] != data->src64[base + i])
                    report_fail_msg("mrn_reloaded_sve 8-byte miscompare at %d: "
                                    "golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                                    (int)(base + i), (unsigned long long)data->src64[base + i],
                                    (unsigned long long)got[i],
                                    (unsigned long long)(data->src64[base + i] ^ got[i]));
        }
    }

    free(temp8);
    free(temp32);
    free(temp64);
    return EXIT_SUCCESS;
}
#else
static int mrn_reloaded_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for mrn_reloaded_sve");
    return EXIT_SKIP;
}
#endif

static int mrn_reloaded_sve_cleanup(struct test *test)
{
    auto *data = static_cast<MrnReloadedSveData *>(test->data);
    if (data) {
        free(data->src8);
        free(data->src32);
        free(data->src64);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mrn_reloaded_sve, "SVE store->immediate-reload at 8/32/64-bit vector widths (port of mrn_reloaded)")
    .test_init = mrn_reloaded_sve_init,
    .test_run = mrn_reloaded_sve_run,
    .test_cleanup = mrn_reloaded_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
