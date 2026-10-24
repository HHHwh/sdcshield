/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_nuke_dense_sve
 * @parblock
 * SVE port of mrn_nuke_dense (source: missing_testcases_20261009/
 * tests/cpu/misc/mrn_nuke_dense.cpp).
 *
 * Load-density axis of the mrn_nuke probe: the skeleton (svst1 temp ->
 * ldr z -> svst1 dst, no ALU, golden = src itself) with the element
 * count raised 16x to 16384 (128 KB per buffer) — ~3x the vector
 * load density of the 1024-element scalar original per unit time on
 * SVE lanes. Tests the "absolute load count" threshold hypothesis.
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

#define MRN_NUKE_DENSE_SVE_COUNT 16384   /* 16x the 1024-element family default */

/* 元素数 count — 运行时可配置, 统一优先级链:
 *   1. -O mrn_nuke_dense_sve.count=N / 配置文件同名条目
 *   2. 环境变量 SDC_MRN_NUKE_DENSE_SVE_COUNT
 *   3. 设计默认 16384 (16x 家族默认 — 负载密度假设的本体, 不可缩小)
 * （core179 诊断探针: 未配置时默认 = 原硬编码设计几何, 不做内存
 *   自适应 — 缓冲几何本身是激发参数; 显式配置 clamp 到
 *   [16384, 1<<22]。非 8 的倍数 log_warning + 向下取整, 保持 u64
 *   缓冲字节数的 64B cache-line 对齐。） */
static size_t mrn_nuke_dense_sve_count(struct test *test)
{
    const int64_t dflt = MRN_NUKE_DENSE_SVE_COUNT;
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

struct MrnNukeDenseSveData {
    uint64_t *src;
    size_t count;
};

static int mrn_nuke_dense_sve_init(struct test *test)
{
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: SVE capability + 128..1024-bit VL range
     * gate in one framework-layer call (device/cpu/arm64/arm64_vl.h). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif
    auto *data = static_cast<MrnNukeDenseSveData *>(malloc(sizeof(MrnNukeDenseSveData)));
    data->count = mrn_nuke_dense_sve_count(test);
    data->src = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    memset_random(data->src, data->count * sizeof(uint64_t));
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

static int mrn_nuke_dense_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<MrnNukeDenseSveData *>(test->data);
    const int lanes = svcntd();

    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < data->count; base += lanes) {
            int n = (data->count - base < (size_t)lanes)
                        ? (int)(data->count - base) : lanes;
            svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)n);
            /* mrn_nuke 骨架不变: 向量 store temp, 转发 ldr z, store dst; 仅元素数 16x */
            svst1_u64(pg, temp + base, svld1_u64(pg, data->src + base));
            svst1_u64(pg, dst + base, load_vec_sve(temp + base));
        }
        if (memcmp(dst, data->src, data->count * sizeof(uint64_t)) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < data->count; ++_dump_j) {
                    if (dst[_dump_j] != data->src[_dump_j]) {
                        ++_dump_bad;
                        if (_dump_shown < 8) {
                            log_error("mrn_nuke_dense_sve: mismatch at index %zu: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                                      _dump_j,
                                      (unsigned long long)data->src[_dump_j],
                                      (unsigned long long)dst[_dump_j],
                                      (unsigned long long)(data->src[_dump_j] ^ dst[_dump_j]));
                            ++_dump_shown;
                        }
                    }
                }
                log_error("mrn_nuke_dense_sve: total mismatched elements: %zu / %zu",
                          _dump_bad, data->count);
            }
            report_fail_msg("mrn_nuke_dense_sve data miscompare");
        }
    }
    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

#else

static int mrn_nuke_dense_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for mrn_nuke_dense_sve");
    return EXIT_SKIP;
}

#endif

static int mrn_nuke_dense_sve_cleanup(struct test *test)
{
    auto *data = static_cast<MrnNukeDenseSveData *>(test->data);
    if (data) {
        free(data->src);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mrn_nuke_dense_sve,
             "SVE mrn_nuke variant: 16x element count (16384) to test the load-density threshold "
             "hypothesis (port of mrn_nuke_dense)")
    .test_init = mrn_nuke_dense_sve_init,
    .test_run = mrn_nuke_dense_sve_run,
    .test_cleanup = mrn_nuke_dense_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
