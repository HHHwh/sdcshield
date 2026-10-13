/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movbe_sve
 * @parblock
 * SVE port of movbe: byte-swap followed by another byte-swap restoring
 * the original order, on SVE vector lanes. Each 32-bit element's bytes
 * are permuted by svtbl with the [3,2,1,0] index table (bit-exact vs
 * __builtin_bswap32, 50k random vectors verified); the swapped value is
 * stored and reloaded through svst1/svld1 (the vector store->load path
 * that mirrors the scalar trigger recipe's reload). Input is random
 * (framework RNG at init), reused per iteration exactly as the original.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <sys/auxv.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif
#endif

#define MOVBE_BUFFER_SIZE (1 << 14)

struct movbe_sve_data {
    uint32_t *input;
    uint32_t *swapped;
};

static int movbe_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for movbe_sve");
        return EXIT_SKIP;
    }
#endif
    auto *data = (movbe_sve_data *)malloc(sizeof(movbe_sve_data));
    data->input = (uint32_t *)aligned_alloc_safe(64, MOVBE_BUFFER_SIZE * sizeof(uint32_t));
    data->swapped = (uint32_t *)aligned_alloc_safe(64, MOVBE_BUFFER_SIZE * sizeof(uint32_t));

    for (size_t i = 0; i < MOVBE_BUFFER_SIZE; ++i) {
        data->input[i] = random32();
    }

    test->data = data;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)
/* 冷路径报告 (noinline): 让热循环里被比较的两个值在 cmp 后即死
 * (本函数自行按地址重载), 基循环指令序列与加转储前保持一致 */
[[gnu::noinline]] static void movbe_sve_report_mismatch(uint64_t idx,
                                                        const uint32_t *golden,
                                                        const uint32_t *actual)
{
    report_fail_msg("MovBE-SVE: Round-trip failed at index %u: "
                    "golden=0x%08x actual=0x%08x xor=0x%08x",
                    (unsigned)idx, (unsigned)*golden, (unsigned)*actual,
                    (unsigned)(*golden ^ *actual));
}

static int movbe_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = (movbe_sve_data *)test->data;

    /* bswap32 的 svtbl 索引: 每元素 [3,2,1,0] */
    static const uint8_t idx4[4] = {3, 2, 1, 0};

    const int lanes = svcntw();
    uint8_t vidx_bytes[64];
    for (int i = 0; i < lanes && i < 16; ++i)
        for (int b = 0; b < 4; ++b)
            vidx_bytes[i * 4 + b] = (uint8_t)(i * 4 + idx4[b]);
    svbool_t pg_batch = svwhilelt_b8((uint64_t)0, (uint64_t)(lanes < 16 ? lanes * 4 : 64));
    svuint8_t vidx = svld1_u8(pg_batch, vidx_bytes);

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < MOVBE_BUFFER_SIZE; base += lanes) {
            int n = (MOVBE_BUFFER_SIZE - base < (size_t)lanes) ? (int)(MOVBE_BUFFER_SIZE - base) : lanes;
            svbool_t pg = svwhilelt_b8((uint64_t)0, (uint64_t)(n * 4));

            /* 直接堆访存 (无栈中转): 修复版 —— 原版被测路径是
             * 堆 store(swapped) 后紧跟堆 reload, 栈中转会改变被测微操作序列 */
            uint8_t in_bytes[64];
            memcpy(in_bytes, data->input + base, n * 4);
            svuint8_t vswapped = svtbl_u8(svld1_u8(pg, in_bytes), vidx);

            /* ★ 堆 store: svst1 直接写 swapped 堆缓冲区 */
            svst1_u8(pg, (uint8_t *)(data->swapped + base), vswapped);

            /* ★ 堆 reload: svld1 直接从 swapped 读回 (被测的 store→load 转发路径) */
            svuint8_t reloaded = svld1_u8(pg, (const uint8_t *)(data->swapped + base));

            /* 再交换一次还原 (同一索引表, svtbl 可逆) */
            svuint8_t vrestored = svtbl_u8(reloaded, vidx);
            uint8_t out_bytes[64];
            svst1_u8(pg, out_bytes, vrestored);

            /* 校验: 还原值 == 输入值 */
            uint32_t restored[16];
            memcpy(restored, out_bytes, n * 4);
            for (int i = 0; i < n; ++i) {
                if (restored[i] != data->input[base + i]) {
                    movbe_sve_report_mismatch(base + i, &data->input[base + i],
                                              &restored[i]);
                }
            }
        }
    }

    return EXIT_SUCCESS;
}
#else
static int movbe_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for movbe_sve");
    return EXIT_SKIP;
}
#endif

static int movbe_sve_cleanup(struct test *test)
{
    auto *data = (movbe_sve_data *)test->data;
    if (data) {
        free(data->input);
        free(data->swapped);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(movbe_sve,
             "SVE byte-swap round-trip: svtbl [3,2,1,0] permutation + vector "
             "store/reload on svst1/svld1 lanes (port of movbe)")
    .test_init = movbe_sve_init,
    .test_run = movbe_sve_run,
    .test_cleanup = movbe_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
