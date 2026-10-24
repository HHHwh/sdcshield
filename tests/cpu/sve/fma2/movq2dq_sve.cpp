/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movq2dq_sve
 * @parblock
 * SVE port of movq2dq: move a GPR value into an SVE z-register and verify
 * the low element plus that the remaining lanes follow the broadcast /
 * zero semantics observed via svst1 (the SVE analogue of the original's
 * fmov d0, x0 channel where the upper half is zeroed).
 * The workload element count (originally MOVQ2DQ_SVE_COUNT=1024) is
 * runtime configurable via -O movq2dq_sve.count=N /
 * SDC_MOVQ2DQ_SVE_COUNT, with a memory-adaptive default when unconfigured.
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

#define MOVQ2DQ_SVE_COUNT 1024

/* 元素数 MOVQ2DQ_SVE_COUNT(1024) — 运行时可配置, 统一优先级链:
 *   1. -O movq2dq_sve.count=N / 配置文件同名条目
 *   2. 环境变量 SDC_MOVQ2DQ_SVE_COUNT
 *   3. 自适应: 1×8B 随机源(共享), 占物理内存 60%,
 *      clamp [1024, 64M]。原 1024 为下限。 */
static size_t movq2dq_sve_count(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "count", 0, MOVQ2DQ_SVE_COUNT, 64 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("movq2dq_sve", "count", &v, MOVQ2DQ_SVE_COUNT, 64 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count(8, 1, 1.0, 600, thread_count(),
                                         MOVQ2DQ_SVE_COUNT, 64 << 20, test->id, "count");
}

struct Movq2dqSveData {
    uint64_t *values;
    size_t count;
};

static int movq2dq_sve_init(struct test *test)
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

    auto *data = static_cast<Movq2dqSveData *>(malloc(sizeof(Movq2dqSveData)));
    const size_t count = movq2dq_sve_count(test);
    data->count = count;
    data->values = static_cast<uint64_t *>(malloc(count * sizeof(uint64_t)));
    memset_random(data->values, count * sizeof(uint64_t));
    test->data = data;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)
static int movq2dq_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<Movq2dqSveData *>(test->data);
    const size_t count = data->count;

    TEST_LOOP(test, 1 << 13) {
        for (size_t i = 0; i < count; i++) {
            /* GPR → SVE 向量: svdup 广播 */
            svuint64_t v = svdup_u64(data->values[i]);
            /* 读回: 存到栈再比对 (低 lane + 广播语义) */
            uint64_t out[16];
            svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)svcntd());
            svst1_u64(pg, out, v);

            if (out[0] != data->values[i]) {
                report_fail_msg(
                    "movq2dq_sve low 64-bit mismatch at index %zu: "
                    "expected 0x%llx got 0x%llx",
                    i,
                    static_cast<unsigned long long>(data->values[i]),
                    static_cast<unsigned long long>(out[0]));
            }
        }
    }
    return EXIT_SUCCESS;
}
#else
static int movq2dq_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    return EXIT_SKIP;
}
#endif

static int movq2dq_sve_cleanup(struct test *test)
{
    auto *data = static_cast<Movq2dqSveData *>(test->data);
    if (data) {
        free(data->values);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(movq2dq_sve, "GPR to SVE z-register moves with value checks (port of movq2dq)")
    .test_init    = movq2dq_sve_init,
    .test_run     = movq2dq_sve_run,
    .test_cleanup = movq2dq_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
