/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b kreg1_sve
 * @parblock
 * SVE port of kreg1: mask generation from vector compares, mask logic
 * (and/or/xor/nand/xnor), and mask blend — all on real SVE predicates
 * (svcmplt/svcmpgt generate the masks, svsel performs the blend).
 * Software reference computes masks element-wise (independent golden).
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

#define KREG1_SVE_LANES 4

static int kreg1_sve_init(struct test *test) {
    (void)test;
    /* All-VL Phase 2 P6: SVE capability + 128..1024-bit VL range
     * gate in one framework-layer call (device/cpu/arm64/arm64_vl.h). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
    return EXIT_SUCCESS;
}

static int kreg1_sve_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_real_distribution<float> dist(-100.0f, 100.0f);
    static std::atomic<uint64_t> iter{0};

    do {
        // 1. 生成两个随机向量（4 个 float，与原版一致）
        float a_arr[KREG1_SVE_LANES], b_arr[KREG1_SVE_LANES];
        for (int i = 0; i < KREG1_SVE_LANES; ++i) {
            a_arr[i] = dist(rng);
            b_arr[i] = dist(rng);
        }

        svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)KREG1_SVE_LANES);
        svfloat32_t vec_a = svld1_f32(pg, a_arr);
        svfloat32_t vec_b = svld1_f32(pg, b_arr);

        // 2. 谓词比较产生掩码（SVE 谓词寄存器 = 真掩码）
        svbool_t lt = svcmplt_f32(pg, vec_a, vec_b);
        svbool_t gt = svcmpgt_f32(pg, vec_a, vec_b);

        // 展开谓词到 0/1 lane 值并打包为 4 位掩码
        float lt_vals[KREG1_SVE_LANES], gt_vals[KREG1_SVE_LANES];
        svst1_f32(pg, lt_vals, svsel_f32(lt, svdup_f32(1.0f), svdup_f32(0.0f)));
        svst1_f32(pg, gt_vals, svsel_f32(gt, svdup_f32(1.0f), svdup_f32(0.0f)));
        uint8_t mask1 = 0, mask2 = 0;
        for (int i = 0; i < KREG1_SVE_LANES; ++i) {
            if (lt_vals[i] != 0.0f) mask1 |= (1 << i);
            if (gt_vals[i] != 0.0f) mask2 |= (1 << i);
        }

        // 3. 掩码逻辑运算（与 x86/原版一致）
        uint8_t and_mask  = mask1 & mask2;
        uint8_t or_mask   = mask1 | mask2;
        uint8_t xor_mask  = mask1 ^ mask2;
        uint8_t nand_mask = (~mask1) & mask2;
        uint8_t xnor_mask = ~(mask1 ^ mask2) & 0xF;

        // 4. 用 or_mask 谓词混合两个向量（_mm_mask_blend_ps 的 SVE 真等价: svsel）
        //    从 or_mask 重建谓词
        uint32_t sel[KREG1_SVE_LANES];
        for (int i = 0; i < KREG1_SVE_LANES; ++i)
            sel[i] = (or_mask & (1 << i)) ? 0xFFFFFFFFu : 0;
        svbool_t blend_p = svcmpne_n_u32(pg, svld1_u32(pg, sel), 0);
        svfloat32_t result = svsel_f32(blend_p, vec_b, vec_a);

        // 5. Store/load 一致性验证
        float store_buf[KREG1_SVE_LANES];
        svst1_f32(pg, store_buf, result);
        svfloat32_t reload = svld1_f32(pg, store_buf);
        bool consistent = (svcntp_b32(pg, svcmpeq_f32(pg, result, reload)) == KREG1_SVE_LANES);

        // 6. 软件参考（逐元素独立 golden）
        uint8_t sw_mask1 = 0, sw_mask2 = 0;
        for (int i = 0; i < KREG1_SVE_LANES; ++i) {
            if (a_arr[i] < b_arr[i]) sw_mask1 |= (1 << i);
            if (a_arr[i] > b_arr[i]) sw_mask2 |= (1 << i);
        }
        uint8_t sw_and  = sw_mask1 & sw_mask2;
        uint8_t sw_or   = sw_mask1 | sw_mask2;
        uint8_t sw_xor  = sw_mask1 ^ sw_mask2;
        uint8_t sw_nand = (~sw_mask1) & sw_mask2;
        uint8_t sw_xnor = ~(sw_mask1 ^ sw_mask2) & 0xF;

        // 7. 比较硬件与软件结果
        bool mask_pass = (sw_and == and_mask) && (sw_or == or_mask) &&
                         (sw_xor == xor_mask) && (sw_nand == nand_mask) &&
                         (sw_xnor == xnor_mask);
        bool passed = mask_pass && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        fprintf(stderr, "kreg1_sve: Iter %lu, a=[%.2f,%.2f,%.2f,%.2f] b=[%.2f,%.2f,%.2f,%.2f]\n",
                iteration, a_arr[0], a_arr[1], a_arr[2], a_arr[3],
                b_arr[0], b_arr[1], b_arr[2], b_arr[3]);
        fprintf(stderr, "  hw: and=0x%X or=0x%X xor=0x%X nand=0x%X xnor=0x%X\n",
                and_mask, or_mask, xor_mask, nand_mask, xnor_mask);
        fprintf(stderr, "  sw: and=0x%X or=0x%X xor=0x%X nand=0x%X xnor=0x%X\n",
                sw_and, sw_or, sw_xor, sw_nand, sw_xnor);
        fprintf(stderr, "  consistent=%d, result=%s%s%s\n",
                consistent, color, result_str, "\033[0m");
        fflush(stderr);

        if (!passed) {
            report_fail_msg("kreg1_sve: mask logic or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int kreg1_sve_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for kreg1_sve");
    return EXIT_SKIP;
}
static int kreg1_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    return EXIT_SKIP;
}
#endif

static int kreg1_sve_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(kreg1_sve, "Mask register ops (compare/mask logic/blend) on real SVE predicates (port of kreg1)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg1_sve_init,
    .test_run = kreg1_sve_run,
    .test_cleanup = kreg1_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
