/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_nuke_4src_alu_sve
 * @parblock
 * SVE port of mrn_nuke_4src_alu (source: missing_testcases_20261009/
 * tests/cpu/misc/mrn_nuke_4src_alu.cpp).
 *
 * Dependency-chain depth axis, depth = 3: 4-source load + data-ALU
 * feeding the svst1 -> ldr z -> svst1 memory-renaming skeleton. Rotating
 * chains per element phase: a^b^c^d / ((a+b)^c)+d / (a^b)+(c^d) /
 * a^(b+c)^d. Golden precomputed in init with the same phase math.
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

#define MRN_NUKE_4SRC_ALU_SVE_COUNT 1024

/* 元素数 count — 运行时可配置, 统一优先级链:
 *   1. -O mrn_nuke_4src_alu_sve.count=N / 配置文件同名条目
 *   2. 环境变量 SDC_MRN_NUKE_4SRC_ALU_SVE_COUNT
 *   3. 设计默认 1024
 * （core179 诊断探针: 未配置时默认 = 原硬编码设计几何, 不做内存
 *   自适应 — 缓冲几何本身是激发参数; 显式配置 clamp 到
 *   [1024, 1<<22]。非 8 的倍数 log_warning + 向下取整, 保持 u64
 *   缓冲字节数的 64B cache-line 对齐。） */
static size_t mrn_nuke_4src_alu_sve_count(struct test *test)
{
    const int64_t dflt = MRN_NUKE_4SRC_ALU_SVE_COUNT;
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

struct MrnNuke4srcAluSveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *srcC;
    uint64_t *srcD;
    uint64_t *expected;
    size_t count;
};

static int mrn_nuke_4src_alu_sve_init(struct test *test)
{
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: SVE capability + 128..1024-bit VL range
     * gate in one framework-layer call (device/cpu/arm64/arm64_vl.h). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif
    auto *data = static_cast<MrnNuke4srcAluSveData *>(malloc(sizeof(MrnNuke4srcAluSveData)));
    data->count = mrn_nuke_4src_alu_sve_count(test);
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    data->srcC = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    data->srcD = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));

    memset_random(data->srcA, data->count * sizeof(uint64_t));
    memset_random(data->srcB, data->count * sizeof(uint64_t));
    memset_random(data->srcC, data->count * sizeof(uint64_t));
    memset_random(data->srcD, data->count * sizeof(uint64_t));

    for (size_t i = 0; i < data->count; i++) {
        switch (i % 4) {
            case 0: data->expected[i] = data->srcA[i] ^ data->srcB[i] ^ data->srcC[i] ^ data->srcD[i]; break;
            case 1: data->expected[i] = ((data->srcA[i] + data->srcB[i]) ^ data->srcC[i]) + data->srcD[i]; break;
            case 2: data->expected[i] = (data->srcA[i] ^ data->srcB[i]) + (data->srcC[i] ^ data->srcD[i]); break;
            case 3: data->expected[i] = data->srcA[i] ^ (data->srcB[i] + data->srcC[i]) ^ data->srcD[i]; break;
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)

static inline svuint64_t load_vec_sve(const void *addr)
{
    svuint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

static int mrn_nuke_4src_alu_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<MrnNuke4srcAluSveData *>(test->data);
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    const int lanes = svcntd();

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < data->count; base += lanes) {
            int n = (data->count - base < (size_t)lanes)
                        ? (int)(data->count - base) : lanes;
            svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)n);

            svuint64_t va = svld1_u64(pg, data->srcA + base);
            svuint64_t vb = svld1_u64(pg, data->srcB + base);
            svuint64_t vc = svld1_u64(pg, data->srcC + base);
            svuint64_t vd = svld1_u64(pg, data->srcD + base);
            svuint64_t r0 = sveor_u64_x(pg, sveor_u64_x(pg, sveor_u64_x(pg, va, vb), vc), vd);
            svuint64_t r1 = svadd_u64_x(pg, sveor_u64_x(pg, svadd_u64_x(pg, va, vb), vc), vd);
            svuint64_t r2 = svadd_u64_x(pg, sveor_u64_x(pg, va, vb), sveor_u64_x(pg, vc, vd));
            svuint64_t r3 = sveor_u64_x(pg, sveor_u64_x(pg, va, svadd_u64_x(pg, vb, vc)), vd);
            /* SVE 架构最大 VL 2048bit → svcntd() ≤ 32 (movbe_slf_sweep
             * VL-portable 范式): n = min(count-base, svcntd()) 全 VL 覆盖 */
            uint64_t phase[SVE_MAX_VECTOR_BYTES / 8];
            for (int k = 0; k < n; ++k) phase[k] = (base + k) % 4;
            svuint64_t vphase = svld1_u64(pg, phase);
            svuint64_t res = svsel_u64(svcmpeq_n_u64(pg, vphase, 0), r0,
                             svsel_u64(svcmpeq_n_u64(pg, vphase, 1), r1,
                             svsel_u64(svcmpeq_n_u64(pg, vphase, 2), r2, r3)));

            svst1_u64(pg, temp + base, res);
            svst1_u64(pg, dst + base, load_vec_sve(temp + base));
        }

        if (memcmp(dst, data->expected, data->count * sizeof(uint64_t)) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < data->count; ++_dump_j) {
                    if (dst[_dump_j] != data->expected[_dump_j]) {
                        ++_dump_bad;
                        if (_dump_shown < 8) {
                            log_error("mrn_nuke_4src_alu_sve: mismatch at index %zu: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                                      _dump_j,
                                      (unsigned long long)data->expected[_dump_j],
                                      (unsigned long long)dst[_dump_j],
                                      (unsigned long long)(data->expected[_dump_j] ^ dst[_dump_j]));
                            ++_dump_shown;
                        }
                    }
                }
                log_error("mrn_nuke_4src_alu_sve: total mismatched elements: %zu / %zu",
                          _dump_bad, data->count);
            }
            report_fail_msg("mrn_nuke_4src_alu_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

#else

static int mrn_nuke_4src_alu_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for mrn_nuke_4src_alu_sve");
    return EXIT_SKIP;
}

#endif

static int mrn_nuke_4src_alu_sve_cleanup(struct test *test)
{
    auto *data = static_cast<MrnNuke4srcAluSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->srcC);
        free(data->srcD);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mrn_nuke_4src_alu_sve,
             "SVE mrn_nuke variant: 4-source load + data-ALU (dependency chain depth = 3) on the "
             "str/ldr/str skeleton (port of mrn_nuke_4src_alu)")
    .test_init = mrn_nuke_4src_alu_sve_init,
    .test_run = mrn_nuke_4src_alu_sve_run,
    .test_cleanup = mrn_nuke_4src_alu_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
