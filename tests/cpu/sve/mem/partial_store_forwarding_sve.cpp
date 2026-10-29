/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b partial_store_forwarding_sve
 * @parblock
 * SVE port of partial_store_forwarding: partial-width stores (1/2/4/8
 * bytes) expressed as SVE predicated stores (svst1 with a whilelt
 * predicate of exactly the store width — the native SVE form of partial
 * stores, more precise than the scalar memcpy-style writes), followed
 * by partial-width loads that may hit the store-buffer entry. Shadow
 * buffer comparison exactly as the original.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

static int partial_store_forwarding_sve_init(struct test *test)
{
    (void)test;
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: per-test auxv hand-probe -> framework
     * layer: arm64_require_sve_vl does the SVE capability check
     * + 128..1024-bit range gate in one call (skip reason
     * framework-issued). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)
/* SVE 谓词控部分写: 宽度 sz 字节 (1/2/4/8), 值从 64-bit 广播向量取低字节 */
static inline void sve_write_partial(uint8_t *addr, int sz, uint64_t val) {
    svbool_t pg = svwhilelt_b8((uint64_t)0, (uint64_t)sz);
    uint8_t bytes[8];
    memcpy(bytes, &val, 8);
    /* #313: broadcast load reuses the store predicate — svptrue_b8 read
     * svcntb() bytes through this 8-byte array (OOB read at every tier;
     * inactive lanes are irrelevant: the predicated store below only
     * takes lanes 0..sz-1, which is exactly val's low sz bytes). */
    svuint8_t v = svld1_u8(pg, bytes);
    svst1_u8(pg, addr, v);
}
/* SVE 谓词控部分读: 读回 sz 字节零扩展 */
static inline uint64_t sve_read_partial(const uint8_t *addr, int sz) {
    svbool_t pg = svwhilelt_b8((uint64_t)0, (uint64_t)sz);
    uint8_t bytes[8] = {0};
    svst1_u8(pg, bytes, svld1_u8(pg, addr));
    uint64_t val;
    memcpy(&val, bytes, 8);
    return val;
}
#endif

static int partial_store_forwarding_sve_run(struct test *test, int cpu) {
#if !defined(__aarch64__)
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for partial_store_forwarding_sve");
    return EXIT_SKIP;
#else
    // 线程局部缓冲（与原版一致）
    alignas(64) uint8_t real_buf[256];
    alignas(64) uint8_t shadow_buf[256];

    memset_random(real_buf, sizeof(real_buf));
    memcpy(shadow_buf, real_buf, sizeof(shadow_buf));

    TEST_LOOP(test, 1 << 16) {
        // 随机偏移 + 宽度 (1/2/4/8) + 值 —— 与原版同分布
        uintptr_t s_off = random32() % (sizeof(real_buf) - 8);
        int s_sz = 1 << (random32() % 4);
        uint64_t s_val = random64();

        // SVE 谓词控部分写 (真硬件谓词部分 store)
        sve_write_partial(real_buf + s_off, s_sz, s_val);
        sve_write_partial(shadow_buf + s_off, s_sz, s_val);

        __asm__ volatile("" ::: "memory");

        // 随机读 (可能命中 store buffer 的部分数据)
        uintptr_t l_off = random32() % (sizeof(real_buf) - 8);
        int l_sz = 1 << (random32() % 4);

        uint64_t real_val = sve_read_partial(real_buf + l_off, l_sz);
        uint64_t shadow_val = sve_read_partial(shadow_buf + l_off, l_sz);

        if (real_val != shadow_val) {
            report_fail_msg("partial_store_forwarding_sve failed at offset %lu, size %d: expected 0x%016lx, got 0x%016lx",
                            l_off, l_sz, shadow_val, real_val);
        }
    }
    return EXIT_SUCCESS;
#endif
}

DECLARE_TEST(partial_store_forwarding_sve,
             "Partial-width predicated stores (svst1 whilelt 1/2/4/8B) with "
             "store-buffer partial-hit loads vs shadow (port of partial_store_forwarding)")
    .test_init    = partial_store_forwarding_sve_init,
    .test_run     = partial_store_forwarding_sve_run,
    .quality_level = TEST_QUALITY_PROD,
    .arm64_requires = ARM64_REQUIRES_SVE,
END_DECLARE_TEST
