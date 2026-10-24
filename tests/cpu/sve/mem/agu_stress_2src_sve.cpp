/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b agu_stress_2src_sve
 * @parblock
 * SVE port of agu_stress_2src: 2-source vector loads (asm-guaranteed ldr z
 * — the SVE form of the scalar 2-src ldr AGU pattern), rotating
 * add/eor/and/or composition, back-to-back str z -> ldr z -> str z
 * store/reload/store chain maximizing address-generation pressure on
 * vector lanes.
 * @endparblock
 */

#include "sandstone.h"
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

/* 每缓冲 u64 元素数(原 1024) — 统一优先级链:
 *   1. -O agu_stress_2src_sve.count=N / 配置文件同名条目
 *   2. 环境变量 SDC_AGU_STRESS_2SRC_SVE_COUNT
 *   3. 自适应: 共享 srcA/srcB/expected 3 + 每线程 temp/dst 2, 共 5 个
 *      8B/元素线性缓冲, 占物理内存 60%, clamp [1024, 32M]
 *      (线性缓冲用 sdc_auto_linear_count, 方阵模型会欠缩放 sqrt 倍)。 */
static size_t agu_2src_sve_count(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "count", 0, 1024, 32 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("agu_stress_2src_sve", "count", &v, 1024, 32 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count(8, 5, 1.0, 600, thread_count(),
                                         1024, 32 << 20, test->id, "count");
}

struct AguStress2srcSveData {
    size_t    count;      /* 每缓冲 u64 元素数(init 定容, 线程共享) */
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *expected;
};

#if defined(__aarch64__)

static inline void store_vec_sve(void *addr, svuint64_t val) {
    svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)svcntd());
    svst1_u64(pg, static_cast<uint64_t *>(addr), val);
}

static inline svuint64_t load_vec_sve(const void *addr) {
    svuint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

#endif

static int agu_stress_2src_sve_init(struct test *test) {
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: per-test auxv hand-probe -> framework
     * layer: arm64_require_sve_vl does the SVE capability check
     * + 128..1024-bit range gate in one call (skip reason
     * framework-issued). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif
    auto *data = static_cast<AguStress2srcSveData *>(malloc(sizeof(AguStress2srcSveData)));
    data->count = agu_2src_sve_count(test);
    data->srcA = static_cast<uint64_t *>(aligned_alloc_safe(64, data->count * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc_safe(64, data->count * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc_safe(64, data->count * sizeof(uint64_t)));

    memset_random(data->srcA, data->count * sizeof(uint64_t));
    memset_random(data->srcB, data->count * sizeof(uint64_t));

    /* golden: same rotating composition, computed on the same SVE units */
    for (size_t i = 0; i < data->count; i++) {
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
static int agu_stress_2src_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<AguStress2srcSveData *>(test->data);
    const size_t count = data->count;

    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc_safe(64, count * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc_safe(64, count * sizeof(uint64_t)));
    const int lanes = svcntd();
    svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)lanes);

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < count; base += lanes) {
            /* 2 源加载: asm 保证的 ldr z */
            svuint64_t a = load_vec_sve(data->srcA + base);
            svuint64_t b = load_vec_sve(data->srcB + base);
            svuint64_t res;

            /* 旋转 ALU 组合: 逐 lane 相位 (base+k)%4, 相位谓词分派 (与 golden i%4 一致) */
            {
                svuint64_t r_add = svadd_u64_x(pg, a, b);
                svuint64_t r_eor = sveor_u64_x(pg, a, b);
                svuint64_t r_and = svand_u64_x(pg, a, b);
                svuint64_t r_orr = svorr_u64_x(pg, a, b);
                uint64_t phase[8];
                int n = (count - base < (size_t)lanes) ? (int)(count - base) : lanes;
                for (int k = 0; k < n; ++k) phase[k] = (base + k) % 4;
                svbool_t pgn = svwhilelt_b64((uint64_t)0, (uint64_t)n);
                svuint64_t vphase = svld1_u64(pgn, phase);
                res = svsel_u64(svcmpeq_n_u64(pgn, vphase, 0), r_add,
                     svsel_u64(svcmpeq_n_u64(pgn, vphase, 1), r_eor,
                     svsel_u64(svcmpeq_n_u64(pgn, vphase, 2), r_and, r_orr)));
            }

            /* 放大器: str z -> ldr z -> str z */
            store_vec_sve(temp + base, res);
            store_vec_sve(dst + base, load_vec_sve(temp + base));
        }

        if (memcmp(dst, data->expected, count * sizeof(uint64_t)) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < count; ++_dump_j) {
                    if (dst[_dump_j] != data->expected[_dump_j]) {
                        ++_dump_bad;
                        if (_dump_shown < 8) {
                            log_error("agu_stress_2src_sve: mismatch at index %zu: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                                      _dump_j, (unsigned long long)data->expected[_dump_j],
                                      (unsigned long long)dst[_dump_j],
                                      (unsigned long long)(data->expected[_dump_j] ^ dst[_dump_j]));
                            ++_dump_shown;
                        }
                    }
                }
                log_error("agu_stress_2src_sve: total mismatched elements: %zu / %zu", _dump_bad,
                          count);
            }
            report_fail_msg("agu_stress_2src_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int agu_stress_2src_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for agu_stress_2src_sve");
    return EXIT_SKIP;
}
#endif

static int agu_stress_2src_sve_cleanup(struct test *test) {
    auto *data = static_cast<AguStress2srcSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(agu_stress_2src_sve,
             "SVE AGU throughput stress: 2-src ldr z + rotating svadd/sveor/svand/svorr "
             "+ back-to-back str z/ldr z/str z (port of agu_stress_2src)")
    .test_init = agu_stress_2src_sve_init,
    .test_run = agu_stress_2src_sve_run,
    .test_cleanup = agu_stress_2src_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
