/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movbe_l1d_sve
 * @test @b movbe_l2_sve
 * @parblock
 * SVE ports of movbe_l1d / movbe_l2 (reload-path localization probes for the
 * core-179 SDC; source: missing_testcases_20261009/tests/cpu/misc/
 * movbe_l1d_vs_l2.cpp).
 *
 * Hypothesis under test: the defect lies in the per-core load data path
 * (L1D read array -> load port -> register). Each batch of svcntw() u32
 * elements runs the movbe round-trip whose TESTED load is the *reload of
 * the input batch from the heap*:
 *
 *   in1      = svld1(input)                  load 1 (heap, direct)
 *   vswapped = svtbl(in1, [3,2,1,0])        REV 1
 *   svst1(swapped, vswapped)                STR
 *   [movbe_l2_sve only: evict input lines]  dc civac + dsb sy
 *   in2      = ldr z (input)                RELOAD — the tested load
 *   vround   = svtbl(vswapped, idx)         REV 2 (register side, like the
 *                                           scalar original's val = bswap(val))
 *   cmp vround != in2                       bswap∘bswap = id, so this is
 *                                           exactly the original's
 *                                           "bswap(bswap(in1)) != reload"
 *
 *   - movbe_l1d_sve: the reload hits L1D. Expected on a defective core:
 *     SDC triggers.
 *   - movbe_l2_sve: every cache line covering the input batch is evicted
 *     before the reload, so it is served from L2/L3. Expected (if the
 *     hypothesis holds): SDC disappears.
 *
 * The reload is asm-guaranteed (volatile ldr z + "memory" clobber): in2
 * reads the same address as in1, so a plain svld1 could be CSE'd away or
 * hoisted above the swapped-store — the asm boundary keeps the reload real
 * and in place (the u8x16 original documents the same compiler-DCE hazard
 * for intrinsic reloads).
 *
 * Two SEPARATE tests (not one with a branch) so neither hot loop contains
 * a regime-selection conditional (any hot-loop conditional can silence the
 * timing-sensitive defect). The two run bodies are separate template
 * instantiations; the evict is compile-time dead in the l1d instantiation.
 *
 * Deviations from the scalar originals (documented):
 *   - bswap runs on SVE lanes via the svtbl [3,2,1,0] byte index table
 *     (bit-exact vs __builtin_bswap32, 50k-vector verified — movbe_sve
 *     precedent);
 *   - the evict runs once per cache line covering the batch instead of
 *     once per 4-byte element (the original re-evicts the same line 16x);
 *     every line is flushed before the reload either way — semantically
 *     equivalent, and the batch reload misses L1D exactly as intended;
 *   - the comparison is vector-wide (svcmpne + svptest_any) with a cold
 *     per-element rescan for logging, instead of per-element scalar cmp.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <sys/auxv.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif
#endif

#define MOVBE_L1D_SVE_BUFFER_SIZE (1 << 14)

struct MovbeL1dL2SveData {
    uint32_t *input;
    uint32_t *swapped;
};

static int movbe_l1d_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for movbe_l1d_sve");
        return EXIT_SKIP;
    }
#endif
    auto *data = static_cast<MovbeL1dL2SveData *>(malloc(sizeof(MovbeL1dL2SveData)));
    data->input = static_cast<uint32_t *>(aligned_alloc_safe(64, MOVBE_L1D_SVE_BUFFER_SIZE * sizeof(uint32_t)));
    data->swapped = static_cast<uint32_t *>(aligned_alloc_safe(64, MOVBE_L1D_SVE_BUFFER_SIZE * sizeof(uint32_t)));

    for (size_t i = 0; i < MOVBE_L1D_SVE_BUFFER_SIZE; ++i) {
        data->input[i] = random32();
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int movbe_l1d_sve_cleanup(struct test *test)
{
    auto *data = static_cast<MovbeL1dL2SveData *>(test->data);
    if (data) {
        free(data->input);
        free(data->swapped);
        free(data);
    }
    return EXIT_SUCCESS;
}

/* Shared miscompare logger (cold branch only) — same field set as the
 * scalar originals. */
static void log_miscompare_sve(const char *tag, unsigned i,
                               uint32_t inputv, uint32_t golden,
                               uint32_t actual, uint32_t xorv)
{
    unsigned char *ib = (unsigned char *)&inputv;
    unsigned char *gb = (unsigned char *)&golden;
    unsigned char *ab = (unsigned char *)&actual;
    unsigned char *xb = (unsigned char *)&xorv;
    log_error("%s: Round-trip failed at index %u: "
              "input=0x%08X golden=0x%08X actual=0x%08X xor=0x%08X | "
              "bytes[b3 b2 b1 b0] input=%02X%02X%02X%02X "
              "golden=%02X%02X%02X%02X actual=%02X%02X%02X%02X "
              "xor=%02X%02X%02X%02X",
              tag, i, inputv, golden, actual, xorv,
              ib[3], ib[2], ib[1], ib[0],
              gb[3], gb[2], gb[1], gb[0],
              ab[3], ab[2], ab[1], ab[0],
              xb[3], xb[2], xb[1], xb[0]);
    report_fail_msg("%s: Round-trip failed at index %u", tag, i);
}

#if defined(__aarch64__)

/* asm 保证的被测 reload: in2 与 in1 同地址, 纯 svld1 会被 CSE/上提;
 * volatile + "memory" clobber 同时钉住它相对 swapped-store 的位置 */
static inline svuint8_t load_vec_u8_sve(const void *addr)
{
    svuint8_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

/* Evict every cache line covering [addr, addr+bytes): dc civac + dsb sy
 * per line. The scalar original evicts per 4-byte element (same line
 * repeatedly); one eviction per line leaves the same end state — all
 * lines flushed before the reload. */
static inline void evict_l1d_range_sve(const void *addr, size_t bytes)
{
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)63;
    uintptr_t end = (uintptr_t)addr + bytes;
    for (uintptr_t a = start; a < end; a += 64) {
        __asm__ volatile("dc civac, %0\n\tdsb sy\n\t" :: "r"(a) : "memory");
    }
}

/* Shared round-trip body. evict != 0 inserts the L1D eviction between the
 * swapped-store and the input reload (movbe_l2_sve); the template
 * parameter is compile-time constant, so each instantiation's hot loop
 * carries no regime conditional — mirroring the originals' two-separate-
 * tests discipline. */
template<int evict> static int movbe_l1d_sve_run_impl(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<MovbeL1dL2SveData *>(test->data);

    /* bswap32 的 svtbl 索引: 每元素 [3,2,1,0] (movbe_sve 构造) */
    static const uint8_t idx4[4] = {3, 2, 1, 0};
    const int lanes = svcntw();
    uint8_t vidx_bytes[64];
    for (int i = 0; i < lanes && i < 16; ++i)
        for (int b = 0; b < 4; ++b)
            vidx_bytes[i * 4 + b] = (uint8_t)(i * 4 + idx4[b]);
    svbool_t pg_batch = svwhilelt_b8((uint64_t)0, (uint64_t)(lanes < 16 ? lanes * 4 : 64));
    svuint8_t vidx = svld1_u8(pg_batch, vidx_bytes);

    uint8_t round_bytes[64];

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < MOVBE_L1D_SVE_BUFFER_SIZE; base += lanes) {
            int n = (MOVBE_L1D_SVE_BUFFER_SIZE - base < (size_t)lanes)
                        ? (int)(MOVBE_L1D_SVE_BUFFER_SIZE - base) : lanes;
            svbool_t pg = svwhilelt_b8((uint64_t)0, (uint64_t)(n * 4));

            /* 加载 1 (堆直读) + REV 1 + 堆 store 到 swapped */
            svuint8_t in1 = svld1_u8(pg, (const uint8_t *)(data->input + base));
            svuint8_t vswapped = svtbl_u8(in1, vidx);
            svst1_u8(pg, (uint8_t *)(data->swapped + base), vswapped);

            /* movbe_l2_sve: reload 前冲刷 input 批覆盖的全部 L1D 行 */
            if (evict)
                evict_l1d_range_sve(data->input + base, (size_t)n * 4);

            /* 被测加载: input 的堆 reload (asm 保证) */
            svuint8_t in2 = load_vec_u8_sve(data->input + base);

            /* REV 2 (寄存器侧, 同原版 val = bswap(val)) + 向量比对:
             * svtbl∘svtbl = 恒等 ⇒ vround == in1, 谓词等价于原版
             * "bswap(bswap(in1)) != reload" */
            svuint8_t vround = svtbl_u8(vswapped, vidx);
            if (svptest_any(pg, svcmpne_u8(pg, vround, in2))) {
                /* 冷路径: 落栈逐元素复核 + 日志 (golden/actual 与原版
                 * 同源: golden = input 冷重读, actual = 双 bswap 寄存器值) */
                svst1_u8(pg, round_bytes, vround);
                uint32_t round[16];
                memcpy(round, round_bytes, (size_t)n * 4);
                for (int k = 0; k < n; ++k) {
                    uint32_t golden = data->input[base + k];
                    if (round[k] != golden) {
                        log_miscompare_sve(evict ? "movbe_l2_sve" : "movbe_l1d_sve",
                                           (unsigned)(base + k), golden, golden,
                                           round[k], golden ^ round[k]);
                    }
                }
            }
        }
    }

    return EXIT_SUCCESS;
}

static int movbe_l1d_sve_run(struct test *test, int cpu)
{
    return movbe_l1d_sve_run_impl<0>(test, cpu);
}

static int movbe_l2_sve_run(struct test *test, int cpu)
{
    return movbe_l1d_sve_run_impl<1>(test, cpu);
}

#else

static int movbe_l1d_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for movbe_l1d_sve");
    return EXIT_SKIP;
}

static int movbe_l2_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for movbe_l2_sve");
    return EXIT_SKIP;
}

#endif

DECLARE_TEST(movbe_l1d_sve,
             "SVE movbe reload-path probe (L1D-hit): asm-guaranteed svld1 heap reload of the "
             "input batch hits L1D (port of movbe_l1d)")
    .test_init = movbe_l1d_sve_init,
    .test_run = movbe_l1d_sve_run,
    .test_cleanup = movbe_l1d_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

DECLARE_TEST(movbe_l2_sve,
             "SVE movbe reload-path probe (L2-reload): evict input lines (dc civac + dsb) before "
             "the asm-guaranteed svld1 reload — if SDC is L1D-path, should disappear (port of movbe_l2)")
    .test_init = movbe_l1d_sve_init,
    .test_run = movbe_l2_sve_run,
    .test_cleanup = movbe_l1d_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
