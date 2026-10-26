/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b kreg4_sve
 * @parblock
 * SVE port of kreg4: VPTESTM/VPTESTNM mask generation (bit = (a&b) != 0 /
 * == 0) on real SVE predicates — svand + svcmpne/cmpeq produce the
 * 16-lane mask natively; the predicate is the mask. Independent scalar
 * golden + store/load consistency as the original.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <random>
#include <cstring>
#include <atomic>
#include <ctime>
#include <unistd.h>

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

#define KREG4_ELEMS 16

static int kreg4_sve_init(struct test *test) {
    (void)test;
#ifdef __aarch64__
    /* All-VL Phase 2 P6: SVE capability + 128..1024-bit VL range
     * gate in one framework-layer call (device/cpu/arm64/arm64_vl.h). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int kreg4_sve_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_int_distribution<uint32_t> val_dist(0, 0xFFFFFFFF);
    static std::atomic<uint64_t> iter{0};

    do {
        // 1. 生成两个随机 16 元素 u32 向量（与原版一致）
        uint32_t a_vals[KREG4_ELEMS], b_vals[KREG4_ELEMS];
        for (int i = 0; i < KREG4_ELEMS; ++i) {
            a_vals[i] = val_dist(rng);
            b_vals[i] = val_dist(rng);
        }

        /* 2. SVE 谓词产生掩码: VPTESTM = (a&b)!=0, VPTESTNM = (a&b)==0
         * 16 元素按 svcntw() 分批; 谓词即掩码。 */
        uint16_t hw_mask_m = 0, hw_mask_nm = 0;
        {
            const int lanes = svcntw();
            for (int base = 0; base < KREG4_ELEMS; base += lanes) {
                int n = (KREG4_ELEMS - base < lanes) ? (KREG4_ELEMS - base) : lanes;
                svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)n);
                svuint32_t va = svld1_u32(pg, a_vals + base);
                svuint32_t vb = svld1_u32(pg, b_vals + base);
                svuint32_t vand = svand_u32_x(pg, va, vb);
                svbool_t pm = svcmpne_n_u32(pg, vand, 0);   /* != 0 → mask */
                svbool_t pnm = svcmpeq_n_u32(pg, vand, 0);  /* == 0 → nmask */
                /* 谓词展开到 0/1 lane 值打包为位掩码 */
                uint32_t mv[16], nmv[16];
                svst1_u32(pg, mv, svsel_u32(pm, svdup_u32(1u), svdup_u32(0u)));
                svst1_u32(pg, nmv, svsel_u32(pnm, svdup_u32(1u), svdup_u32(0u)));
                for (int i = 0; i < n; ++i) {
                    if (mv[i])  hw_mask_m  |= (1u << (base + i));
                    if (nmv[i]) hw_mask_nm |= (1u << (base + i));
                }
            }
        }

        // 3. 软件参考（逐元素, 独立 golden）
        uint16_t sw_mask_m = 0, sw_mask_nm = 0;
        for (int i = 0; i < KREG4_ELEMS; ++i) {
            uint32_t and_val = a_vals[i] & b_vals[i];
            if (and_val != 0) sw_mask_m |= (1u << i);
            else              sw_mask_nm |= (1u << i);
        }

        // 4. 存储一致性
        uint32_t reload_a[KREG4_ELEMS], reload_b[KREG4_ELEMS];
        memcpy(reload_a, a_vals, sizeof(a_vals));
        memcpy(reload_b, b_vals, sizeof(b_vals));
        bool consistent = (memcmp(reload_a, a_vals, sizeof(a_vals)) == 0) &&
                          (memcmp(reload_b, b_vals, sizeof(b_vals)) == 0);

        // 5. 比较硬件与软件
        bool passed = (hw_mask_m == sw_mask_m) && (hw_mask_nm == sw_mask_nm) && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        fprintf(stderr, "kreg4_sve: Iter %lu, a[0..3]=0x%08X,0x%08X,0x%08X,0x%08X\n",
                iteration, a_vals[0], a_vals[1], a_vals[2], a_vals[3]);
        fprintf(stderr, "          b[0..3]=0x%08X,0x%08X,0x%08X,0x%08X\n",
                b_vals[0], b_vals[1], b_vals[2], b_vals[3]);
        fprintf(stderr, "  sw: VPTESTM=0x%04X, VPTESTNM=0x%04X\n", sw_mask_m, sw_mask_nm);
        fprintf(stderr, "  hw: VPTESTM=0x%04X, VPTESTNM=0x%04X\n", hw_mask_m, hw_mask_nm);
        fprintf(stderr, "  consistent=%d, result=%s%s\033[0m\n",
                consistent, color, result_str);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("kreg4_sve: mismatch in SVE predicate masks or consistency");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int kreg4_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    __builtin_unreachable();
}
#endif

static int kreg4_sve_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(kreg4_sve, "Vector test mask generation (VPTESTM, VPTESTNM) on real SVE predicates (port of kreg4)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg4_sve_init,
    .test_run = kreg4_sve_run,
    .test_cleanup = kreg4_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
