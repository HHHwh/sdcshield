/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b insert_extract_sve
 * @parblock
 * SVE port of insert_extract: random lane insert + extract round-trip on
 * an SVE vector. Insert = predicate-gated single-lane write (svsel with a
 * single-lane predicate); extract = svlastb on the shifted vector or a
 * predicate-gated read. Store/load consistency via svst1/svld1 + svcmpeq.
 * Values come from the per-thread framework RNG (reproducible via -s); the
 * store/load sub-check uses a function-local stack buffer (thread-private,
 * no shared state) with an asm memory barrier forcing a real reload. On
 * failure the full lane/value state is dumped via log_error before
 * report_fail_msg.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

#define IE_LANES 4

static int insert_extract_sve_init(struct test *test) {
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
static int insert_extract_sve_run(struct test *test, int cpu) {
    (void)cpu;

    do {
        // 1. 生成原始向量 (4 lane, 与原版一致) — 框架每线程 RNG, 种子可复现
        float orig_vals[IE_LANES];
        for (int i = 0; i < IE_LANES; ++i)
            orig_vals[i] = frandomf_scale(200.0f) - 100.0f;
        svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)IE_LANES);
        svfloat32_t orig_vec = svld1_f32(pg, orig_vals);

        // 2. 随机选择 lane 和插入值
        int lane = (int)(random32() % IE_LANES);
        float insert_val = frandomf_scale(200.0f) - 100.0f;

        // 3. SVE 插入: 单 lane 谓词选择 (svsel) —— vsetq_lane 的谓词等价
        uint32_t lane_sel[IE_LANES];
        for (int i = 0; i < IE_LANES; ++i) lane_sel[i] = (i == lane) ? 1u : 0u;
        svbool_t lane_p = svcmpne_n_u32(pg, svld1_u32(pg, lane_sel), 0);
        svfloat32_t ins_vec = svdup_f32(insert_val);
        svfloat32_t new_vec = svsel_f32(lane_p, ins_vec, orig_vec);

        // SVE 提取: 谓词控读回该 lane (vgetq_lane 的等价读)
        float new_vals[IE_LANES];
        svst1_f32(pg, new_vals, new_vec);
        float extracted = new_vals[lane];

        // 4. 验证插入/提取一致性
        bool insert_extract_ok = (extracted == insert_val);

        // 5. Store/Load 一致性测试 — 函数局部栈缓冲, 线程私有, 消除多写者竞态;
        //    asm 内存屏障阻断编译器消除真实 reload (先例 movbe_dump_probe_c_sve)
        float sl_buf[IE_LANES];
        svst1_f32(pg, sl_buf, orig_vec);
        __asm__ volatile("" ::: "memory");
        svfloat32_t loaded_vec = svld1_f32(pg, sl_buf);
        bool store_load_ok = (svcntp_b32(pg, svcmpeq_f32(pg, orig_vec, loaded_vec)) == IE_LANES);

        // 失败时 dump 全量现场 (两类子检查分别标注), report_fail_msg 为 noreturn
        if (!insert_extract_ok || !store_load_ok) {
            log_error("insert_extract_sve: sub-check failure: insert/extract=%s store/load=%s | "
                      "lane=%d insert_val=%.9g extracted=%.9g | "
                      "orig_vals=[%.9g %.9g %.9g %.9g] new_vals=[%.9g %.9g %.9g %.9g] sl_buf=[%.9g %.9g %.9g %.9g]",
                      insert_extract_ok ? "ok" : "FAIL", store_load_ok ? "ok" : "FAIL",
                      lane, insert_val, extracted,
                      orig_vals[0], orig_vals[1], orig_vals[2], orig_vals[3],
                      new_vals[0], new_vals[1], new_vals[2], new_vals[3],
                      sl_buf[0], sl_buf[1], sl_buf[2], sl_buf[3]);
            report_fail_msg("insert_extract_sve: insert/extract or store/load consistency failure");
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int insert_extract_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for insert_extract_sve");
    return EXIT_SKIP;
}
#endif

DECLARE_TEST(insert_extract_sve, "SVE lane insert/extract round-trip via predicate-gated select (port of insert_extract)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = insert_extract_sve_init,
    .test_run = insert_extract_sve_run,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
