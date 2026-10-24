/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0.
 *
 * @test @b arm0102_kreg_mask_sve
 * @parblock
 * SVE port of arm0102_kreg_mask (SDC trigger-format variant of kreg7/8:
 * 2-source vector loads + rotating compare-mask expansion
 * eq/gt/eq-u/tst + back-to-back store→reload→store amplifier).
 * The SVE version keeps the verified core-179 trigger recipe structure
 * but moves the data path to SVE: asm-guaranteed ldr z for the two source
 * loads, predicate compares (svcmpeq/svcmpgt) + svsel for the mask
 * expansion (the real SVE equivalent of the kreg semantics), and
 * back-to-back str z → ldr z → str z as the amplifier.
 * Golden precomputed in init on the same SVE units; memcmp in cold path.
 * The element count (originally ARM0102_KREG_MASK_SVE_COUNT=1024) is
 * runtime configurable via -O arm0102_kreg_mask_sve.count=N /
 * SDC_ARM0102_KREG_MASK_SVE_COUNT, with a memory-adaptive default
 * when unconfigured.
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

#define ARM0102_KREG_MASK_SVE_COUNT 1024   /* elements per buffer (uint32) */

struct Arm0102KregMaskSveData {
    uint32_t *srcA;
    uint32_t *srcB;
    uint32_t *expected;
    int count;                /* 运行时元素数 (≤64M, int 足够) */
};

#if defined(__aarch64__)

/* 元素数 ARM0102_KREG_MASK_SVE_COUNT(1024) — 运行时可配置, 统一优先级链:
 *   1. -O arm0102_kreg_mask_sve.count=N / 配置文件同名条目
 *   2. 环境变量 SDC_ARM0102_KREG_MASK_SVE_COUNT
 *   3. 自适应: srcA/srcB/expected(共享)+每线程 temp/dst, 5×4B,
 *      占物理内存 60%, clamp [1024, 64M]。pg4 子块/相位不变。 */
static size_t arm0102_kreg_mask_sve_count(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "count", 0, ARM0102_KREG_MASK_SVE_COUNT, 64 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("arm0102_kreg_mask_sve", "count", &v, ARM0102_KREG_MASK_SVE_COUNT, 64 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count(4, 5, 1.0, 600, thread_count(),
                                         ARM0102_KREG_MASK_SVE_COUNT, 64 << 20, test->id, "count");
}

/* 4-lane (16B) 语义的存/取 —— 与子块谓词 pg4 严格一致。
 * 注意不能用 svptrue_b32(): VL=256 时它是 8 lane/32B, 会越界写
 * 4 元素子块后面的内存 (851 次循环后堆元数据被砸穿 → double free,
 * 批次 1 调试时抓到的 bug)。 */
#define KREG_SVE_SUBLANES 4

static inline void store_vec_sve(void *addr, svuint32_t val) {
    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)KREG_SVE_SUBLANES);
    svst1_u32(pg, static_cast<uint32_t *>(addr), val);
}

static inline svuint32_t load_vec_sve(const void *addr) {
    svuint32_t res;
    /* asm 保证的加载 (core-179 配方要求, 与原版 ldr q 对应);
     * SVE ldr z 按 VL 全宽载, 后续所有计算/存储都用 pg4 截断 */
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

/* 旋转比较掩码扩展: i%4 → eq/gt(signed)/eq-u/tst —— 谓词 + svsel (真 kreg 语义) */
static inline svuint32_t kreg_expand_rotating(svbool_t pg, svuint32_t a, svuint32_t b, int i) {
    switch (i % 4) {
        case 0: {  /* eq (signed reinterpret) */
            svint32_t sa = svreinterpret_s32_u32(a), sb = svreinterpret_s32_u32(b);
            svbool_t p = svcmpeq_s32(pg, sa, sb);
            return svsel_u32(p, svdup_u32(0xFFFFFFFFu), svdup_u32(0));
        }
        case 1: {  /* gt (signed) */
            svint32_t sa = svreinterpret_s32_u32(a), sb = svreinterpret_s32_u32(b);
            svbool_t p = svcmpgt_s32(pg, sa, sb);
            return svsel_u32(p, svdup_u32(0xFFFFFFFFu), svdup_u32(0));
        }
        case 2: {  /* eq-u */
            svbool_t p = svcmpeq_u32(pg, a, b);
            return svsel_u32(p, svdup_u32(0xFFFFFFFFu), svdup_u32(0));
        }
        default: { /* tst: (a & b) != 0 */
            svbool_t p = svcmpne_n_u32(pg, svand_u32_x(pg, a, b), 0);
            return svsel_u32(p, svdup_u32(0xFFFFFFFFu), svdup_u32(0));
        }
    }
}

static int arm0102_kreg_mask_sve_init(struct test *test) {
    /* All-VL Phase 2 P6: SVE capability + 128..1024-bit VL range
     * gate in one framework-layer call (device/cpu/arm64/arm64_vl.h). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    auto *data = static_cast<Arm0102KregMaskSveData *>(malloc(sizeof(Arm0102KregMaskSveData)));
    const int count = (int)arm0102_kreg_mask_sve_count(test);
    data->count = count;
    data->srcA = static_cast<uint32_t *>(aligned_alloc(64, (size_t)count * sizeof(uint32_t)));
    data->srcB = static_cast<uint32_t *>(aligned_alloc(64, (size_t)count * sizeof(uint32_t)));
    data->expected = static_cast<uint32_t *>(aligned_alloc(64, (size_t)count * sizeof(uint32_t)));

    memset_random(data->srcA, (size_t)count * sizeof(uint32_t));
    memset_random(data->srcB, (size_t)count * sizeof(uint32_t));

    /* golden: 同一旋转组合, 在相同 SVE 单元上预计算 */
    [[maybe_unused]] svbool_t pg = svptrue_b32();
    const int lanes = (int)(svl / 4);   /* f32 lanes = SVL bytes / 4 */
    for (int base = 0; base < count; base += lanes) {
        int n = (count - base < lanes) ? (count - base) : lanes;
        [[maybe_unused]] svbool_t pgn = svwhilelt_b32((uint64_t)0, (uint64_t)n);
        /* 与 run 完全相同的 4 元素子块结构 + 相位 (base+sub)%4 */
        for (int sub = 0; sub < n; sub += 4) {
            int phase = (base + sub) % 4;
            /* 单 lane 组: 用标量嵌入向量 */
            uint32_t av[8], bv[8], ov[8];
            for (int k = 0; k < 4 && sub + k < n; ++k) {
                av[k] = data->srcA[base + sub + k];
                bv[k] = data->srcB[base + sub + k];
            }
            svbool_t pg4 = svwhilelt_b32((uint64_t)0, (uint64_t)4);
            svuint32_t va = svld1_u32(pg4, av), vb = svld1_u32(pg4, bv);
            svuint32_t res = kreg_expand_rotating(pg4, va, vb, phase);
            svst1_u32(pg4, ov, res);
            for (int k = 0; k < 4 && sub + k < n; ++k)
                data->expected[base + sub + k] = ov[k];
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int arm0102_kreg_mask_sve_run(struct test *test, int cpu) {
    auto *data = static_cast<Arm0102KregMaskSveData *>(test->data);
    const int count = data->count;

    uint32_t *temp = static_cast<uint32_t *>(aligned_alloc(64, (size_t)count * sizeof(uint32_t)));
    uint32_t *dst  = static_cast<uint32_t *>(aligned_alloc(64, (size_t)count * sizeof(uint32_t)));

    TEST_LOOP(test, 1 << 13) {
        /* 与 golden 完全相同的 4 元素子块结构: 每 SVE 向量批内按 4 元素子块,
         * 相位 = (base+sub) % 4 (与 init 的 golden 循环逐字一致) */
        for (int base = 0; base < count; base += svcntw()) {
            int n = (count - base < svcntw()) ? (count - base) : svcntw();
            for (int sub = 0; sub < n; sub += 4) {
                /* 2 源加载: asm 保证的 ldr z (SVE 版配方核心) */
                svuint32_t a = load_vec_sve(data->srcA + base + sub);   /* ldr z — source 1 */
                svuint32_t b = load_vec_sve(data->srcB + base + sub);   /* ldr z — source 2 */
                svbool_t pg4 = svwhilelt_b32((uint64_t)0, (uint64_t)4);

                /* 载荷: 谓词比较掩码扩展 + svsel (相位与 golden 一致) */
                svuint32_t res = kreg_expand_rotating(pg4, a, b, base + sub);

                /* 放大器: str z -> ldr z -> str z */
                store_vec_sve(temp + base + sub, res);
                store_vec_sve(dst + base + sub, load_vec_sve(temp + base + sub));
            }
        }

        if (memcmp(dst, data->expected, (size_t)count * sizeof(uint32_t)) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < (size_t)(count); ++_dump_j) {
                    if (dst[_dump_j] != data->expected[_dump_j]) {
                        ++_dump_bad;
                        if (_dump_shown < 8) {
                            log_error("arm0102_kreg_mask_sve: mismatch at index %zu: golden=0x%08x actual=0x%08x xor=0x%08x",
                                      _dump_j, (unsigned)data->expected[_dump_j], (unsigned)dst[_dump_j],
                                      (unsigned)(data->expected[_dump_j] ^ dst[_dump_j]));
                            ++_dump_shown;
                        }
                    }
                }
                log_error("arm0102_kreg_mask_sve: total mismatched elements: %zu / %d", _dump_bad,
                          count);
            }
            report_fail_msg("arm0102_kreg_mask_sve data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int arm0102_kreg_mask_sve_cleanup(struct test *test) {
    auto *data = static_cast<Arm0102KregMaskSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

#else

static int arm0102_kreg_mask_sve_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for arm0102_kreg_mask_sve");
    return EXIT_SKIP;
}
static int arm0102_kreg_mask_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    return EXIT_SKIP;
}
static int arm0102_kreg_mask_sve_cleanup(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#endif

DECLARE_TEST(arm0102_kreg_mask_sve,
             "SVE port of arm-0102 kreg mask variant: 2-src ldr z + rotating predicate compare-mask expand (eq/gt/eq-u/tst via svsel) + str z/ldr z/str z amplifier")
    .test_init    = arm0102_kreg_mask_sve_init,
    .test_run     = arm0102_kreg_mask_sve_run,
    .test_cleanup = arm0102_kreg_mask_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
