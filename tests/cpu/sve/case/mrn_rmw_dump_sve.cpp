/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_rmw_dump_sve
 * @parblock
 * SVE port of mrn_rmw_dump: identical to mrn_rmw_sve plus dump-on-failure\n * (input/golden/actual/xor at the first mismatch).
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

#define MRN_RMW_SVE_COUNT 1024

/* 元素数 count — 运行时可配置, 统一优先级链:
 *   1. -O mrn_rmw_dump_sve.count=N / 配置文件同名条目
 *   2. 环境变量 SDC_MRN_RMW_DUMP_SVE_COUNT
 *   3. 设计默认 1024
 * （core179 诊断探针: 未配置时默认 = 原硬编码设计几何, 不做内存
 *   自适应 — 缓冲几何本身是激发参数; 显式配置 clamp 到
 *   [1024, 1<<22]。非 8 的倍数 log_warning + 向下取整, 保持 u64
 *   缓冲字节数的 64B cache-line 对齐。dump 窗口/格式契约不变。） */
static size_t mrn_rmw_dump_sve_count(struct test *test)
{
    const int64_t dflt = MRN_RMW_SVE_COUNT;
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

struct MrnRmwSveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *expected;
    size_t count;
};

static int mrn_rmw_dump_sve_init(struct test *test)
{
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: SVE capability + 128..1024-bit VL range
     * gate in one framework-layer call (device/cpu/arm64/arm64_vl.h). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif
    auto *data = static_cast<MrnRmwSveData *>(malloc(sizeof(MrnRmwSveData)));
    data->count = mrn_rmw_dump_sve_count(test);
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    memset_random(data->srcA, data->count * sizeof(uint64_t));
    memset_random(data->srcB, data->count * sizeof(uint64_t));
    for (size_t i = 0; i < data->count; ++i) {
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
static int mrn_rmw_dump_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<MrnRmwSveData *>(test->data);
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    const int lanes = svcntd();

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < data->count; base += lanes) {
            /* i%4 旋转: add/sub/xor/and —— 逐 lane 相位 (i%4), 按位谓词分派:
             * 对整批分别计算 4 个操作的结果, 用相位谓词逐 lane 选择。
             * (批步进 lanes 时 base%4 不恒 0, 相位谓词按 (base+k)%4 生成) */
            int n = (data->count - base < (size_t)lanes) ? (int)(data->count - base) : lanes;
            svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)n);
            svuint64_t va = svld1_u64(pg, data->srcA + base);
            svuint64_t vb = svld1_u64(pg, data->srcB + base);
            /* 4 个操作的结果 */
            svuint64_t r_add = svadd_u64_x(pg, va, vb);
            svuint64_t r_sub = svsub_u64_x(pg, va, vb);
            svuint64_t r_eor = sveor_u64_x(pg, va, vb);
            svuint64_t r_and = svand_u64_x(pg, va, vb);
            /* 相位谓词: lane k → (base+k)%4 */
            uint64_t phase[8];
            for (int k = 0; k < n; ++k) phase[k] = (base + k) % 4;
            svuint64_t vphase = svld1_u64(pg, phase);
            svuint64_t res = svsel_u64(svcmpeq_n_u64(pg, vphase, 0), r_add,
                          svsel_u64(svcmpeq_n_u64(pg, vphase, 1), r_sub,
                          svsel_u64(svcmpeq_n_u64(pg, vphase, 2), r_eor, r_and)));
            /* RMW: store 到 temp, load 到 dst (向量) */
            svst1_u64(pg, temp + base, res);
            svst1_u64(pg, dst + base, svld1_u64(pg, temp + base));
        }
        if (memcmp(dst, data->expected, data->count * sizeof(uint64_t)) != 0) {
            /* dump 版: 定位首错 + input/golden/actual/xor 字节分解 */
            for (size_t i = 0; i < data->count; ++i) {
                if (dst[i] != data->expected[i]) {
                    uint64_t inputv = data->srcA[i];
                    uint64_t golden = data->expected[i];
                    uint64_t actual = dst[i];
                    uint64_t xorv = golden ^ actual;
                    log_error("mrn_rmw_dump_sve: mismatch at index %zu: "
                              "a=0x%016llX b=0x%016llX golden=0x%016llX "
                              "actual=0x%016llX xor=0x%016llX",
                              i, (unsigned long long)data->srcA[i],
                              (unsigned long long)data->srcB[i],
                              (unsigned long long)golden,
                              (unsigned long long)actual,
                              (unsigned long long)xorv);
                    (void)inputv;
                    break;
                }
            }
            report_fail_msg("mrn_rmw_dump_sve data miscompare");
        }
    }
    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int mrn_rmw_dump_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for mrn_rmw_dump_sve");
    return EXIT_SKIP;
}
#endif

static int mrn_rmw_dump_sve_cleanup(struct test *test)
{
    auto *data = static_cast<MrnRmwSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mrn_rmw_dump_sve, "SVE rotating-op RMW with dump-on-failure (port of mrn_rmw_dump)")
    .test_init = mrn_rmw_dump_sve_init,
    .test_run = mrn_rmw_dump_sve_run,
    .test_cleanup = mrn_rmw_dump_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
