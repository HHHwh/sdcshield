/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movmskpspd_sve
 * @parblock
 * SVE port of movmskpspd: sign-bit mask extraction from float/double
 * vectors. The SVE version uses predicate compares (svcmplt on the
 * integer reinterpretation against zero) + svcntp/svbrkb-style per-lane
 * packing — the predicate register IS the sign mask, the native SVE
 * expression of MOVMSKPS/PD. Goldens are computed from raw sign bits
 * exactly as the original.
 * The operand-pair count (originally MOVMSK_COUNT=1024) is runtime
 * configurable via -O movmskpspd_sve.count=N / SDC_MOVMSKPSPD_SVE_COUNT,
 * with a memory-adaptive default when unconfigured (FPS_LANES/FPD_LANES
 * width semantics unchanged).
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

#define MOVMSK_COUNT 1024
#define FPS_LANES 4
#define FPD_LANES 2

/* 操作数对数 MOVMSK_COUNT(1024) — 运行时可配置, 统一优先级链:
 *   1. -O movmskpspd_sve.count=N / 配置文件同名条目
 *   2. 环境变量 SDC_MOVMSKPSPD_SVE_COUNT
 *   3. 自适应: fvalues(4×4B)+dvalues(2×8B)+fexpected(4B)+dexpected(4B)
 *      ≈ 40B/对(模型 8B×4), 占物理内存 60%, clamp [1024, 64M]。
 *      FPS_LANES/FPD_LANES 宽度语义不动。 */
static size_t movmskpspd_sve_count(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "count", 0, MOVMSK_COUNT, 64 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("movmskpspd_sve", "count", &v, MOVMSK_COUNT, 64 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count(8, 4, 1.0, 600, thread_count(),
                                         MOVMSK_COUNT, 64 << 20, test->id, "count");
}

struct MovmskpspdSveData {
    float *fvalues;
    double *dvalues;
    uint32_t *fexpected;
    uint32_t *dexpected;
    size_t count;
};

static int movmskpspd_sve_init(struct test *test)
{
#ifdef __aarch64__
    /* All-VL Phase 2 P6: per-test auxv hand-probe -> framework
     * layer: arm64_require_sve_vl does the SVE capability check
     * + 128..1024-bit range gate in one call (skip reason
     * framework-issued). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif

    auto *data = static_cast<MovmskpspdSveData *>(malloc(sizeof(MovmskpspdSveData)));
    const size_t count = movmskpspd_sve_count(test);
    data->count = count;

    data->fvalues  = static_cast<float *>(aligned_alloc(16, count * FPS_LANES * sizeof(float)));
    data->dvalues  = static_cast<double *>(aligned_alloc(16, count * FPD_LANES * sizeof(double)));
    data->fexpected = static_cast<uint32_t *>(malloc(count * sizeof(uint32_t)));
    data->dexpected = static_cast<uint32_t *>(malloc(count * sizeof(uint32_t)));

    memset_random(data->fvalues, count * FPS_LANES * sizeof(float));
    memset_random(data->dvalues, count * FPD_LANES * sizeof(double));

    /* Golden masks from raw sign bits (independent of the instruction under test) */
    for (size_t i = 0; i < count; i++) {
        uint32_t mask = 0;
        for (int j = 0; j < FPS_LANES; j++) {
            uint32_t bits;
            std::memcpy(&bits, &data->fvalues[i * FPS_LANES + j], sizeof(bits));
            if (bits & 0x80000000u)
                mask |= (1u << j);
        }
        data->fexpected[i] = mask;
    }
    for (size_t i = 0; i < count; i++) {
        uint32_t mask = 0;
        for (int j = 0; j < FPD_LANES; j++) {
            uint64_t bits;
            std::memcpy(&bits, &data->dvalues[i * FPD_LANES + j], sizeof(bits));
            if (bits & 0x8000000000000000ull)
                mask |= (1u << j);
        }
        data->dexpected[i] = mask;
    }

    test->data = data;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)

/*
 * SVE MOVMSKPS equivalent: predicate compare (negative lanes) is the
 * native sign mask; pack the first 4 predicate lanes into a 4-bit mask
 * by selecting sign bits with svsel and reading them back.
 */
static inline uint32_t do_movmskps_sve(const float *p)
{
    const int lanes = svcntw();
    float vals[32];
    uint32_t out[32];
    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)(lanes < 32 ? lanes : 32));
    svfloat32_t v = svld1_f32(pg, p);
    /* 谓词 = 符号位置位 lane。注意不能用浮点比较 svcmplt_f32(v, 0):
     * -0.0 < +0.0 为 false 会漏检负零; 按原版语义用符号位(整数比较)。 */
    svbool_t neg = svcmpge_n_u32(pg, svreinterpret_u32_f32(v), 0x80000000u);
    /* 展开谓词到 0/1 lane 值并读回 */
    svuint32_t bits = svsel_u32(neg, svdup_u32(1u), svdup_u32(0u));
    svst1_u32(pg, out, bits);
    (void)vals;
    uint32_t mask = 0;
    for (int i = 0; i < 4; ++i) mask |= (out[i] & 1u) << i;
    return mask;
}

static inline uint32_t do_movmskpd_sve(const double *p)
{
    const int lanes = svcntd();
    uint64_t out[16];
    svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)(lanes < 16 ? lanes : 16));
    svfloat64_t v = svld1_f64(pg, p);
    svbool_t neg = svcmpge_n_u64(pg, svreinterpret_u64_f64(v), 0x8000000000000000ull);
    svuint64_t bits = svsel_u64(neg, svdup_u64(1ull), svdup_u64(0));
    svst1_u64(pg, out, bits);
    uint32_t mask = 0;
    /* double: 64-bit lane 的 1 已在低位 */
    for (int i = 0; i < 2; ++i) mask |= (out[i] & 1u) << i;
    return mask;
}

static int movmskpspd_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<MovmskpspdSveData *>(test->data);
    const size_t count = data->count;

    TEST_LOOP(test, 1 << 13) {
        for (size_t i = 0; i < count; i++) {
            uint32_t fmask = do_movmskps_sve(&data->fvalues[i * FPS_LANES]);
            if (fmask != data->fexpected[i]) {
                report_fail_msg(
                    "movmskpspd_sve: MOVMSKPS mismatch at index %zu: "
                    "expected 0x%x got 0x%x",
                    i, data->fexpected[i], fmask);
            }
        }
        for (size_t i = 0; i < count; i++) {
            uint32_t dmask = do_movmskpd_sve(&data->dvalues[i * FPD_LANES]);
            if (dmask != data->dexpected[i]) {
                report_fail_msg(
                    "movmskpspd_sve: MOVMSKPD mismatch at index %zu: "
                    "expected 0x%x got 0x%x",
                    i, data->dexpected[i], dmask);
            }
        }
    }
    return EXIT_SUCCESS;
}
#else
static int movmskpspd_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    return EXIT_SKIP;
}
#endif

static int movmskpspd_sve_cleanup(struct test *test)
{
    auto *data = static_cast<MovmskpspdSveData *>(test->data);
    if (data) {
        free(data->fvalues);
        free(data->dvalues);
        free(data->fexpected);
        free(data->dexpected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(movmskpspd_sve, "Sign-bit mask extraction on SVE predicate compares (svcmplt+svsel, port of movmskpspd)")
    .test_init    = movmskpspd_sve_init,
    .test_run     = movmskpspd_sve_run,
    .test_cleanup = movmskpspd_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
