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
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)
#include <arm_sve.h>

#ifndef SVE_MAX_VECTOR_BYTES
/* GCC 12 的 arm_sve.h 不提供该 ACLE 宏 (movbe_slf_sweep 同款自定义):
 * SVE 架构最大 VL 2048bit = 256 字节 */
#define SVE_MAX_VECTOR_BYTES 256
#endif
#include <device/cpu/arm64/arm64_vl.h>
#endif

#define MOVBE_L1D_SVE_BUFFER_SIZE (1 << 14)

/* 元素数 count (u32 元素) — 运行时可配置, 统一优先级链 (init 由
 * movbe_l1d_sve / movbe_l2_sve 两测试共享, 经 test->id 各自独立):
 *   1. -O movbe_l1d_sve.count=N / movbe_l2_sve.count=N (或配置文件同名条目)
 *   2. 环境变量 SDC_MOVBE_L1D_SVE_COUNT / SDC_MOVBE_L2_SVE_COUNT
 *   3. 设计默认 1<<14 (16384 元素 = 64KB/缓冲)
 * （core179 诊断探针: 未配置时默认 = 原硬编码设计几何, 不做内存
 *   自适应 — 缓冲几何本身是激发参数; 显式配置 clamp 到
 *   [1<<14, 1<<22]。非 16 的倍数 log_warning + 向下取整, 保持 u32
 *   缓冲字节数的 64B cache-line 对齐。） */
static size_t movbe_l1d_sve_count(struct test *test)
{
    const int64_t dflt = MOVBE_L1D_SVE_BUFFER_SIZE;
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
    if (v % 16 != 0) {
        log_warning("%s: count=%ld 非 16 的倍数, 向下取整为 %ld (保持 64B 缓冲对齐)",
                    test->id, (long)v, (long)(v - v % 16));
        v -= v % 16;
    }
    return (size_t)v;
}

struct MovbeL1dL2SveData {
    uint32_t *input;
    uint32_t *swapped;
    size_t count;
};

static int movbe_l1d_sve_init(struct test *test)
{
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: SVE capability + 128..1024-bit VL range
     * gate in one framework-layer call (device/cpu/arm64/arm64_vl.h). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif
    auto *data = static_cast<MovbeL1dL2SveData *>(malloc(sizeof(MovbeL1dL2SveData)));
    data->count = movbe_l1d_sve_count(test);
    data->input = static_cast<uint32_t *>(aligned_alloc_safe(64, data->count * sizeof(uint32_t)));
    data->swapped = static_cast<uint32_t *>(aligned_alloc_safe(64, data->count * sizeof(uint32_t)));

    for (size_t i = 0; i < data->count; ++i) {
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
    /* 索引按运行时 VL 定容: svcntw()*4 = svcntb() ≤ 256 = 架构最大 VL
     * (movbe_slf_sweep VL-portable 范式); 原 64 字节 cap 在 lanes>16 时
     * 使尾部 lane 索引为垃圾 → 确定性误报 + 失败路径栈越界 */
    uint8_t vidx_bytes[SVE_MAX_VECTOR_BYTES];
    for (int i = 0; i < lanes; ++i)
        for (int b = 0; b < 4; ++b)
            vidx_bytes[i * 4 + b] = (uint8_t)(i * 4 + idx4[b]);
    svbool_t pg_batch = svwhilelt_b8((uint64_t)0, (uint64_t)lanes * 4);
    svuint8_t vidx = svld1_u8(pg_batch, vidx_bytes);

    uint8_t round_bytes[SVE_MAX_VECTOR_BYTES];

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < data->count; base += lanes) {
            int n = (data->count - base < (size_t)lanes)
                        ? (int)(data->count - base) : lanes;
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
                /* 冷路径: 落栈逐元素复核 + 日志。复核必须用被测 reload(in2)
                 * 而非 input 的又一次重读: 向量级 svcmpne 检出的正是
                 * "寄存器结果 != 被测 reload", 若冷路径重读 input 会引入
                 * 第三个值, 可能漏报被检出的 lane 或错报索引。
                 * golden = 被测 reload(in2), actual = 双 bswap 寄存器值
                 * (与原版 scalars 字段同源)。 */
                svst1_u8(pg, round_bytes, vround);
                uint8_t in2_bytes[SVE_MAX_VECTOR_BYTES];
                svst1_u8(pg, in2_bytes, in2);
                uint32_t round[SVE_MAX_VECTOR_BYTES / 4];
                uint32_t reload[SVE_MAX_VECTOR_BYTES / 4];
                memcpy(round, round_bytes, (size_t)n * 4);
                memcpy(reload, in2_bytes, (size_t)n * 4);
                for (int k = 0; k < n; ++k) {
                    if (round[k] != reload[k]) {
                        log_miscompare_sve(evict ? "movbe_l2_sve" : "movbe_l1d_sve",
                                           (unsigned)(base + k), reload[k], reload[k],
                                           round[k], reload[k] ^ round[k]);
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
