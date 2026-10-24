/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_rmw_diff_sve
 * @parblock
 * SVE port of mrn_rmw_diff (store-to-load FORWARDING-path isolation probe;
 * source: missing_testcases_20261009/tests/cpu/misc/mrn_rmw_fwd_vs_diff.cpp).
 *
 * Control cell for mrn_rmw_fwd_sve: keeps the tight str->ldr pair (same
 * instruction structure, same time gap) but breaks FORWARDING by using a
 * DIFFERENT load address — svst1(res -> tempA batch) then an
 * asm-guaranteed ldr z of the tempB batch. tempB is pre-filled in init
 * with the golden value that tempA receives, so the reload "should" read
 * res but is served from the L1D array (tempB resident since init), not
 * from store-buffer forwarding (the store to tempA cannot forward to a
 * load of tempB).
 *
 *   fwd triggers but this does not -> the defect is specifically in the
 *     FORWARDING path;
 *   this also triggers -> not forwarding-specific; str->ldr proximity /
 *     timing / load-port contention.
 *
 * The tempB load is asm-guaranteed: the compiler could otherwise hoist it
 * above the tempA store (distinct malloc results are disambiguatable),
 * destroying the probed str->ldr adjacency.
 *
 * Race note (kept from the scalar original by design): tempA is shared
 * across worker threads and every thread stores the same deterministic
 * value (pure function of the read-only srcA/srcB) to the same aligned
 * addresses; no thread ever READS tempA (verification reads tempB and
 * expected only). Aligned 64-bit stores are single-copy atomic on ARMv8,
 * so concurrent same-value stores are indistinguishable — no
 * false-positive mechanism exists. tempB/expected/srcA/srcB are strictly
 * read-only after init (the insert_extract_sve 1023c841 lesson does not
 * apply: nothing here reads what other threads write).
 *
 * Verification is immediate per batch (vector compare + cold per-element
 * rescan), mirroring the original's per-element in-loop compare.
 * @endparblock
 */

#include "sandstone.h"
#include "sdc_stress_config.h"
#include <cstdint>
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

#define MRN_RMW_DIFF_SVE_COUNT 1024

/* 元素数 count — 运行时可配置, 统一优先级链:
 *   1. -O mrn_rmw_diff_sve.count=N / 配置文件同名条目
 *   2. 环境变量 SDC_MRN_RMW_DIFF_SVE_COUNT
 *   3. 设计默认 1024
 * （core179 诊断探针: 未配置时默认 = 原硬编码设计几何, 不做内存
 *   自适应 — 缓冲几何本身是激发参数; 显式配置 clamp 到
 *   [1024, 1<<22]。非 8 的倍数 log_warning + 向下取整, 保持 u64
 *   缓冲字节数的 64B cache-line 对齐。） */
static size_t mrn_rmw_diff_sve_count(struct test *test)
{
    const int64_t dflt = MRN_RMW_DIFF_SVE_COUNT;
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

struct MrnRmwDiffSveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *expected;   /* golden = srcA op srcB */
    uint64_t *tempA;      /* str target (same-value shared writes) */
    uint64_t *tempB;      /* ldr source (pre-filled with expected; different addr) */
    size_t count;
};

static int mrn_rmw_diff_sve_init(struct test *test)
{
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: SVE capability + 128..1024-bit VL range
     * gate in one framework-layer call (device/cpu/arm64/arm64_vl.h). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif
    auto *data = static_cast<MrnRmwDiffSveData *>(malloc(sizeof(MrnRmwDiffSveData)));
    data->count = mrn_rmw_diff_sve_count(test);
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    data->tempA = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    data->tempB = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));

    memset_random(data->srcA, data->count * sizeof(uint64_t));
    memset_random(data->srcB, data->count * sizeof(uint64_t));

    for (size_t i = 0; i < data->count; i++) {
        switch (i % 4) {
            case 0: data->expected[i] = data->srcA[i] + data->srcB[i]; break;
            case 1: data->expected[i] = data->srcA[i] - data->srcB[i]; break;
            case 2: data->expected[i] = data->srcA[i] ^ data->srcB[i]; break;
            case 3: data->expected[i] = data->srcA[i] & data->srcB[i]; break;
        }
        /* tempB 预填 golden: L1D 阵列 reload "应当" 读到 expected[i] */
        data->tempB[i] = data->expected[i];
        data->tempA[i] = 0;
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int mrn_rmw_diff_sve_cleanup(struct test *test)
{
    auto *data = static_cast<MrnRmwDiffSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data->tempA);
        free(data->tempB);
        free(data);
    }
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)

/* asm 钉住 tempB 加载相对 tempA store 的位置 (被测 str->ldr 配对的 ldr 侧) */
static inline svuint64_t load_vec_sve(const void *addr)
{
    svuint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

static int mrn_rmw_diff_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<MrnRmwDiffSveData *>(test->data);
    const int lanes = svcntd();
    uint64_t got_bytes[64];

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < data->count; base += lanes) {
            int n = (data->count - base < (size_t)lanes)
                        ? (int)(data->count - base) : lanes;
            svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)n);

            svuint64_t va = svld1_u64(pg, data->srcA + base);
            svuint64_t vb = svld1_u64(pg, data->srcB + base);
            svuint64_t r_add = svadd_u64_x(pg, va, vb);
            svuint64_t r_sub = svsub_u64_x(pg, va, vb);
            svuint64_t r_eor = sveor_u64_x(pg, va, vb);
            svuint64_t r_and = svand_u64_x(pg, va, vb);
            /* SVE 架构最大 VL 2048bit → svcntd() ≤ 32 (movbe_slf_sweep
             * VL-portable 范式): n = min(count-base, svcntd()) 全 VL 覆盖 */
            uint64_t phase[SVE_MAX_VECTOR_BYTES / 8];
            for (int k = 0; k < n; ++k) phase[k] = (base + k) % 4;
            svuint64_t vphase = svld1_u64(pg, phase);
            svuint64_t res = svsel_u64(svcmpeq_n_u64(pg, vphase, 0), r_add,
                             svsel_u64(svcmpeq_n_u64(pg, vphase, 1), r_sub,
                             svsel_u64(svcmpeq_n_u64(pg, vphase, 2), r_eor, r_and)));

            /* str res -> tempA 批; ldr 自 tempB 批 (不同地址, L1D 阵列读) */
            svst1_u64(pg, data->tempA + base, res);
            svuint64_t got = load_vec_sve(data->tempB + base);

            /* 立即验证: reload 应读回 expected (向量比对, 冷路径逐元素) */
            svuint64_t vexp = svld1_u64(pg, data->expected + base);
            if (svptest_any(pg, svcmpne_u64(pg, got, vexp))) {
                svst1_u64(pg, got_bytes, got);
                for (int k = 0; k < n; ++k) {
                    if (got_bytes[k] != data->expected[base + k]) {
                        unsigned i = (unsigned)(base + k);
                        const char *op = (i % 4 == 0) ? "add"
                                       : (i % 4 == 1) ? "sub"
                                       : (i % 4 == 2) ? "xor" : "and";
                        uint64_t golden = data->expected[base + k];
                        uint64_t actual = got_bytes[k];
                        uint64_t xorv = golden ^ actual;
                        log_error("mrn_rmw_diff_sve: miscompare at index %u (op=%s): "
                                  "golden=0x%016lX actual=0x%016lX xor=0x%016lX",
                                  i, op, (unsigned long)golden,
                                  (unsigned long)actual, (unsigned long)xorv);
                        report_fail_msg("mrn_rmw_diff_sve: data miscompare at index %u (op=%s)",
                                        i, op);
                    }
                }
            }
        }
    }

    return EXIT_SUCCESS;
}

#else

static int mrn_rmw_diff_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for mrn_rmw_diff_sve");
    return EXIT_SKIP;
}

#endif

DECLARE_TEST(mrn_rmw_diff_sve,
             "SVE mrn_rmw forward-isolation probe: svst1 to tempA then asm ldr z from DIFFERENT "
             "addr tempB (L1D-array, not forwarding) — isolates the forwarding path "
             "(port of mrn_rmw_diff)")
    .test_init = mrn_rmw_diff_sve_init,
    .test_run = mrn_rmw_diff_sve_run,
    .test_cleanup = mrn_rmw_diff_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
