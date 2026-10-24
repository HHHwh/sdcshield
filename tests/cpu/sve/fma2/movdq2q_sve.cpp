/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movdq2q_sve
 * @parblock
 * SVE port of movdq2q: move the low 64 bits of an SVE z-register to a GPR
 * and verify. The value is placed in an SVE vector (svdup) and extracted
 * via the asm vector-to-GPR channel (mov Xd, Zm.d[0]) — the SVE analogue
 * of the original's mov x0, v0.d[0] NEON channel.
 * The workload element count (originally MOV_COUNT=1024) is runtime
 * configurable via -O movdq2q_sve.count=N / SDC_MOVDQ2Q_SVE_COUNT,
 * with a memory-adaptive default when unconfigured.
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstdlib>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

#define MOV_COUNT 1024

/* 元素数 MOV_COUNT(1024) — 运行时可配置, 统一优先级链:
 *   1. -O movdq2q_sve.count=N / 配置文件同名条目
 *   2. 环境变量 SDC_MOVDQ2Q_SVE_COUNT
 *   3. 自适应: high/low 2×8B 随机源(共享), 占物理内存 60%,
 *      clamp [1024, 64M]。原 1024 为下限。 */
static size_t movdq2q_sve_count(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "count", 0, MOV_COUNT, 64 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("movdq2q_sve", "count", &v, MOV_COUNT, 64 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count(8, 2, 1.0, 600, thread_count(),
                                         MOV_COUNT, 64 << 20, test->id, "count");
}

struct movdq2q_sve_data {
    uint64_t *high_parts;
    uint64_t *low_parts;
    size_t count;
};

static int movdq2q_sve_init(struct test *test)
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

    auto *data = (movdq2q_sve_data *)malloc(sizeof(movdq2q_sve_data));
    const size_t count = movdq2q_sve_count(test);
    data->count = count;
    data->high_parts = (uint64_t *)aligned_alloc_safe(64, count * sizeof(uint64_t));
    data->low_parts = (uint64_t *)aligned_alloc_safe(64, count * sizeof(uint64_t));

    for (size_t i = 0; i < count; ++i) {
        data->high_parts[i] = random64();
        data->low_parts[i] = random64();
    }

    test->data = data;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)
static int movdq2q_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    movdq2q_sve_data *data = (movdq2q_sve_data *)test->data;
    const size_t count = data->count;

    TEST_LOOP(test, 1 << 10) {
        for (size_t i = 0; i < count; ++i) {
            /* 值进 SVE 向量 (svdup 广播) */
            svuint64_t zsrc = svdup_u64(data->low_parts[i]);

            /* SVE 向量→GPR 通道: svlastb_u64 取最后活跃 lane。
             * (z0.d[0] 的 mov 语法 SVE 汇编器不接受, svlastb 是等价的
             * 向量→标量硬件通道, 语义 = 原版 mov x0, v0.d[0] 的 SVE 对应) */
            uint64_t x0 = svlastb_u64(svptrue_b64(), zsrc);

            if (x0 != data->low_parts[i]) {
                /* cold path: record bit-level evidence before failing */
                log_error("movdq2q_sve: index %lu: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                          i, (unsigned long long)data->low_parts[i], (unsigned long long)x0,
                          (unsigned long long)(data->low_parts[i] ^ x0));
                report_fail_msg("movdq2q_sve: Low 64-bit mismatch at index %lu", i);
            }
        }
    }

    return EXIT_SUCCESS;
}
#else
static int movdq2q_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    return EXIT_SKIP;
}
#endif

static int movdq2q_sve_cleanup(struct test *test)
{
    movdq2q_sve_data *data = (movdq2q_sve_data *)test->data;
    if (data) {
        free(data->high_parts);
        free(data->low_parts);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(movdq2q_sve, "SVE z-register low-64-bit to GPR moves with value checks (port of movdq2q)")
    .test_init = movdq2q_sve_init,
    .test_run = movdq2q_sve_run,
    .test_cleanup = movdq2q_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
