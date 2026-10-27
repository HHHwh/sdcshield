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
    int lanes;                /* init 期 f32 lanes (SVL bytes/4): run 侧以
                               * svcntw() 复读核对, 漂移则 fail-closed
                               * (PR #294 检视 round 4 发现 #49) */
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

    /* golden: 同一旋转组合, 在相同 SVE 单元上预计算 */
    [[maybe_unused]] svbool_t pg = svptrue_b32();
    const int lanes = (int)(svl / 4);   /* f32 lanes = SVL bytes / 4 */
    /* arm64_vl.h unit convention: release-effective range check catches
     * a wrong-divisor drift (e.g. svl/8 here would compile silently). */
    if (lanes < 4 || lanes > 32) {
        log_error("%s: f32 lanes %d outside [4,32] — unit drift (svl %ld bytes)",
                  test->id, lanes, svl);
        return EXIT_FAILURE;
    }
    data->lanes = lanes;   /* run 侧以 svcntw() 复读核对 (漂移守卫) */
    /* 尾批 asm 全宽读松弛 = lanes (kreg_logic 同款, a6f753ce): run 侧
     * ldr z 按 svcntb() 全宽载, 最末子块最多读到 (count-1)+svcntw()
     * 个元素, slack=lanes 严格充分 (任意 VL)。aligned_alloc_safe 为
     * 795e0d55 家族契约修法 (total 未必是 64 的倍数)。 */
    const int slack = lanes;
    const size_t total = (size_t)(count + slack) * sizeof(uint32_t);
    data->srcA = static_cast<uint32_t *>(aligned_alloc_safe(64, total));
    data->srcB = static_cast<uint32_t *>(aligned_alloc_safe(64, total));
    data->expected = static_cast<uint32_t *>(aligned_alloc_safe(64, total));

    memset_random(data->srcA, total);
    memset_random(data->srcB, total);

    for (int base = 0; base < count; base += lanes) {
        int n = (count - base < lanes) ? (count - base) : lanes;
        [[maybe_unused]] svbool_t pgn = svwhilelt_b32((uint64_t)0, (uint64_t)n);
        /* 与 run 完全相同的 4 元素子块结构 + 相位按子块旋转 */
        for (int sub = 0; sub < n; sub += 4) {
            /* 相位 = 子块序号 (base+sub)/KREG_SVE_SUBLANES, 按子块旋转;
             * 原式 (base+sub)%4 恒为 0 (base 是 svcntw() 倍数、sub 是 4
             * 的倍数) —— 4 谓词路径只覆盖 eq, gt/eq-u/tst 全部死代码
             * (426dc5c6 家族同款修法, kreg_logic/not/select/fma 均已改)。 */
            const int phase = (base + sub) / KREG_SVE_SUBLANES;
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

    /* 漂移 fail-closed (all-VL policy #3; PR #294 检视 round 4 发现 #49):
     * golden 的旋转相位按 init lanes 预计算 ((base+sub)/KREG_SVE_SUBLANES),
     * 下面的批循环步进却用运行时 svcntw() —— 两者在 VL 漂移时不再同构。
     * 以 svcntw() 复读核对, 不一致即报错退出而非静默比错。 */
    if ((int)svcntw() != data->lanes) {
        log_error("%s: SVE VL drifted between init (lanes=%d) and run "
                  "(svcntw()=%d)", test->id, data->lanes, (int)svcntw());
        report_fail_msg("%s: SVE VL drifted between init and run", test->id);
    }

    /* run 侧 temp/dst 同享尾批松弛: load_vec_sve 的 asm ldr z 全宽读
     * 最末子块, (count + svcntw()) 与 init 的 total 同界 (kreg_logic 同款)。 */
    uint32_t *temp = static_cast<uint32_t *>(aligned_alloc_safe(64, (size_t)(count + svcntw()) * sizeof(uint32_t)));
    uint32_t *dst  = static_cast<uint32_t *>(aligned_alloc_safe(64, (size_t)(count + svcntw()) * sizeof(uint32_t)));

    TEST_LOOP(test, 1 << 13) {
        /* 与 golden 完全相同的 4 元素子块结构: 每 SVE 向量批内按 4 元素子块,
         * 相位 = 子块序号 (base+sub)/KREG_SVE_SUBLANES (与 init 一致) */
        for (int base = 0; base < count; base += svcntw()) {
            int n = (count - base < svcntw()) ? (count - base) : svcntw();
            for (int sub = 0; sub < n; sub += 4) {
                /* 2 源加载: asm 保证的 ldr z (SVE 版配方核心) */
                svuint32_t a = load_vec_sve(data->srcA + base + sub);   /* ldr z — source 1 */
                svuint32_t b = load_vec_sve(data->srcB + base + sub);   /* ldr z — source 2 */
                svbool_t pg4 = svwhilelt_b32((uint64_t)0, (uint64_t)4);

                /* 载荷: 谓词比较掩码扩展 + svsel (相位与 golden 一致) */
                svuint32_t res = kreg_expand_rotating(pg4, a, b,
                                                      (base + sub) / KREG_SVE_SUBLANES);

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
    .arm64_requires = ARM64_REQUIRES_SVE,
END_DECLARE_TEST
