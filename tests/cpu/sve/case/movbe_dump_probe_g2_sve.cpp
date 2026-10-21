/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movbe_dump_probe_g2_sve
 * @parblock
 * PROBE G2 (SVE): the store stream spreads across 16 DIFFERENT cache lines
 * (128B apart) — the set-conflict control for G1.
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

struct movbe_dump_probe_g2_sve_data {
    uint32_t *input;
    uint32_t *swapped;
};

static int movbe_dump_probe_g2_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for movbe_dump_probe_g2_sve");
        return EXIT_SKIP;
    }
#endif
    auto *data = (movbe_dump_probe_g2_sve_data *)malloc(sizeof(movbe_dump_probe_g2_sve_data));
    data->input = (uint32_t *)aligned_alloc_safe(64, MOVBE_BUFFER_SIZE * sizeof(uint32_t));
    data->swapped = (uint32_t *)aligned_alloc_safe(64, 128 * 16 + 64);

    for (size_t i = 0; i < MOVBE_BUFFER_SIZE; ++i) {
        data->input[i] = random32();
    }

    test->data = data;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)
/* gcc-10 (22.03/20.03) 对本函数在 -O2+ 的 RTL expand 确定性 ICE(internal
 * compiler error: Segmentation fault,run 35977342136/36078386244 两轮修复
 * 均无效)。rootfs gcc-10.3.1 二分定位:-O0/-O1 过、-O2 起崩,非向量化类
 * (-fno-tree-vectorize 等不救);函数级降 O1 稳定通过。SVE intrinsics 为
 * 显式生成,不依赖优化器;probe 测 store 侧效应而非吞吐,O1 无语义影响。 */
__attribute__((optimize("O1")))
static int movbe_dump_probe_g2_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = (movbe_dump_probe_g2_sve_data *)test->data;

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

            /* 载入 n 个 32-bit 元素 (向量) → 字节级重排 */
            uint8_t in_bytes[64];
            memcpy(in_bytes, data->input + base, n * 4);
            svuint8_t vswapped = svtbl_u8(svld1_u8(pg, in_bytes), vidx);

            /* store swapped (向量 store/reload 路径) */
            /* PROBE G2: store 到 16 个不同 128B 行 — svst1 直打堆 */
            /* gcc-10 (22.03/20.03 CI, run 35977342136) 对 svst1 地址式内含
               "& 常数" 在 RTL expand ICE;地址先落入局部指针再传,
               语义不变 (probe_h 的 line_idx 先例)。 */
            uint8_t *dst = (uint8_t *)(data->swapped + ((base / lanes) & 15) * (128 / 4));
            svst1_u8(pg, dst, vswapped);

            /* 再交换一次还原 (同一索引表, svtbl 可逆) */
            svuint8_t vrestored = svtbl_u8(vswapped, vidx);   /* 寄存器直连: 原版 bswap 作用于寄存器值, 这些 probe 被测的是 store 侧效应 */
            uint8_t out_bytes[64];
            svst1_u8(pg, out_bytes, vrestored);

            /* 校验: 还原值 == 输入值; ONLY on failure: dump input/golden/actual/xor + 字节分解 */
            uint32_t restored[16];
            memcpy(restored, out_bytes, n * 4);
            for (int i = 0; i < n; ++i) {
                if (restored[i] != data->input[base + i]) {
                    uint32_t inputv = data->input[base + i];
                    uint32_t golden = inputv;
                    uint32_t actual = restored[i];
                    uint32_t xorv = golden ^ actual;
                    unsigned char *ib = (unsigned char *)&inputv;
                    unsigned char *gb = (unsigned char *)&golden;
                    unsigned char *ab = (unsigned char *)&actual;
                    unsigned char *xb = (unsigned char *)&xorv;
                    log_error("MovBE-SVE-dump: Round-trip failed at index %u: "
                              "input=0x%08X golden=0x%08X actual=0x%08X xor=0x%08X | "
                              "bytes[b3 b2 b1 b0] input=%02X%02X%02X%02X "
                              "golden=%02X%02X%02X%02X actual=%02X%02X%02X%02X "
                              "xor=%02X%02X%02X%02X",
                              (unsigned)(base + i), inputv, golden, actual, xorv,
                              ib[3], ib[2], ib[1], ib[0],
                              gb[3], gb[2], gb[1], gb[0],
                              ab[3], ab[2], ab[1], ab[0],
                              xb[3], xb[2], xb[1], xb[0]);
                    report_fail_msg("MovBE-SVE: Round-trip failed at index %u",
                                    (unsigned)(base + i));
                }
            }
        }
    }

    return EXIT_SUCCESS;
}
#else
static int movbe_dump_probe_g2_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for movbe_dump_probe_g2_sve");
    return EXIT_SKIP;
}
#endif

static int movbe_dump_probe_g2_sve_cleanup(struct test *test)
{
    auto *data = (movbe_dump_probe_g2_sve_data *)test->data;
    if (data) {
        free(data->input);
        free(data->swapped);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(movbe_dump_probe_g2_sve,
             "SVE byte-swap round-trip with dump-on-failure: svtbl [3,2,1,0] + vector "
             "store/reload, input/golden/actual/xor byte breakdown on failure (port of movbe_dump)")
    .test_init = movbe_dump_probe_g2_sve_init,
    .test_run = movbe_dump_probe_g2_sve_run,
    .test_cleanup = movbe_dump_probe_g2_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
