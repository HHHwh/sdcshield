/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_store_only_sve
 * @parblock
 * SVE port of neon_rot_store_only (source: missing_testcases_20261009/
 * tests/cpu/misc/neon_rot_store_only.cpp).
 *
 * Reload-necessity probe on the vector datapath: the neon_rot_2src
 * trigger recipe (asm-guaranteed ldr z source loads + rotating
 * add/eor/and/orr composition per element phase + back-to-back stores)
 * with the reload REMOVED — the result is stored to temp and dst with
 * no load in between (the scalar NEON original's baseline:
 * neon_rot_2src fires ~33 fail/30min; this cell asks whether the
 * back-to-back reload is a necessary condition).
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

#define NEON_ROT_STORE_ONLY_SVE_COUNT 1024   /* uint64 elements per buffer */

struct NeonRotStoreOnlySveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *expected;
    size_t count;   /* 运行时元素数 (knob `count`, 默认设计值) */
};

#if defined(__aarch64__)

/* 优先级: 1. -O neon_rot_store_only_sve.count=N  2. SDC_NEON_ROT_STORE_ONLY_SVE_COUNT
 *   3. 设计默认 1024
 * （诊断探针: 默认保持设计几何, 显式配置才生效 — 槽距/行距模式是激发参数不动） */
static size_t neon_rot_store_only_sve_count(struct test *test)
{
    const int64_t dflt = NEON_ROT_STORE_ONLY_SVE_COUNT;
    int64_t v = sdc_knob_int_clamped(test, "count", 0, NEON_ROT_STORE_ONLY_SVE_COUNT, 1 << 22);
    if (v == 0)
        sdc_env_knob_int_clamped("neon_rot_store_only_sve", "count", &v, NEON_ROT_STORE_ONLY_SVE_COUNT, 1 << 22);
    return v > 0 ? (size_t)v : (size_t)dflt;
}

#endif

static int neon_rot_store_only_sve_init(struct test *test)
{
    size_t count = NEON_ROT_STORE_ONLY_SVE_COUNT;
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: SVE capability + 128..1024-bit VL range
     * gate in one framework-layer call (device/cpu/arm64/arm64_vl.h). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
    count = neon_rot_store_only_sve_count(test);
    /* asm ldr z 全向量源加载: count 向下取整到 svcntd() 的整倍数 (倍数约束保留) */
    const size_t lanes = (size_t)(svl / 8);   /* f64 lanes = SVL bytes / 8 */
    const size_t rem = count - count / lanes * lanes;   /* % 表达式触发 GCC SVE ICE, 改用除法组合 */
    if (rem != 0) {
        const size_t rounded = count - rem;
        log_warning("neon_rot_store_only_sve: count=%zu 不是 svcntd()=%zu 的整倍数, 向下取整为 %zu",
                    count, lanes, rounded);
        count = rounded;
    }
#endif
    auto *data = static_cast<NeonRotStoreOnlySveData *>(malloc(sizeof(NeonRotStoreOnlySveData)));
    data->count = count;
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, count * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, count * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, count * sizeof(uint64_t)));

    memset_random(data->srcA, count * sizeof(uint64_t));
    memset_random(data->srcB, count * sizeof(uint64_t));

    for (size_t i = 0; i < count; i++) {
        switch (i % 4) {
            case 0: data->expected[i] = data->srcA[i] + data->srcB[i]; break;
            case 1: data->expected[i] = data->srcA[i] ^ data->srcB[i]; break;
            case 2: data->expected[i] = data->srcA[i] & data->srcB[i]; break;
            case 3: data->expected[i] = data->srcA[i] | data->srcB[i]; break;
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

static int neon_rot_store_only_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<NeonRotStoreOnlySveData *>(test->data);

    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, data->count * sizeof(uint64_t)));
    const int lanes = svcntd();
    svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)lanes);

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < data->count; base += lanes) {
            /* 2 源加载: asm 保证的 ldr z */
            svuint64_t a = load_vec_sve(data->srcA + base);
            svuint64_t b = load_vec_sve(data->srcB + base);

            /* 旋转 ALU: 逐 lane 相位 (base+k)%4, 相位谓词分派 */
            svuint64_t r_add = svadd_u64_x(pg, a, b);
            svuint64_t r_eor = sveor_u64_x(pg, a, b);
            svuint64_t r_and = svand_u64_x(pg, a, b);
            svuint64_t r_orr = svorr_u64_x(pg, a, b);
            int n = (data->count - base < (size_t)lanes)
                        ? (int)(data->count - base) : lanes;
            /* SVE 架构最大 VL 2048bit → svcntd() ≤ 32 (movbe_slf_sweep
             * VL-portable 范式): n = min(count-base, svcntd()) 全 VL 覆盖 */
            uint64_t phase[SVE_MAX_VECTOR_BYTES / 8];
            for (int k = 0; k < n; ++k) phase[k] = (base + k) % 4;
            svbool_t pgn = svwhilelt_b64((uint64_t)0, (uint64_t)n);
            svuint64_t vphase = svld1_u64(pgn, phase);
            svuint64_t res = svsel_u64(svcmpeq_n_u64(pgn, vphase, 0), r_add,
                             svsel_u64(svcmpeq_n_u64(pgn, vphase, 1), r_eor,
                             svsel_u64(svcmpeq_n_u64(pgn, vphase, 2), r_and, r_orr)));

            /* 两存无读: 移除 reload (reload-必要性探针的核心) */
            svst1_u64(pgn, temp + base, res);
            svst1_u64(pgn, dst + base, res);
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
                            log_error("neon_rot_store_only_sve: mismatch at index %zu: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                                      _dump_j,
                                      (unsigned long long)data->expected[_dump_j],
                                      (unsigned long long)dst[_dump_j],
                                      (unsigned long long)(data->expected[_dump_j] ^ dst[_dump_j]));
                            ++_dump_shown;
                        }
                    }
                }
                log_error("neon_rot_store_only_sve: total mismatched elements: %zu / %zu",
                          _dump_bad, data->count);
            }
            report_fail_msg("neon_rot_store_only_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

#else

static int neon_rot_store_only_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_store_only_sve");
    return EXIT_SKIP;
}

#endif

static int neon_rot_store_only_sve_cleanup(struct test *test)
{
    auto *data = static_cast<NeonRotStoreOnlySveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_store_only_sve,
             "SVE datapath variant: 2-src ldr z + rotating ALU + 2 stores, NO reload — the "
             "reload-necessity probe on SVE lanes (port of neon_rot_store_only)")
    .test_init = neon_rot_store_only_sve_init,
    .test_run = neon_rot_store_only_sve_run,
    .test_cleanup = neon_rot_store_only_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
