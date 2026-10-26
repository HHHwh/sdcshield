/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b swizzle_sve
 * @parblock
 * SVE port of swizzle: the pairwise-swap permutation (0↔1, 2↔3, ...) via
 * svtbl byte-level table lookup on an SVE vector — the SVE1 general
 * permute primitive (NEON vrev64q has no direct SVE1 counterpart).
 * Reference is the same scalar gather with the same perm table.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <random>
#include <cstring>
#include <atomic>

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

// 标量参考实现（与原版一致）
static void swizzle_reference(const float *src, float *dst, const int *perm) {
    for (int i = 0; i < 8; ++i) {
        dst[i] = src[perm[i]];
    }
}

static int swizzle_sve_init(struct test *test) {
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
/* 冷路径转储 (noinline): 让热循环里 memcmp 比较后的两数组值即死
 * (本函数按地址自行重载), do-while 主体指令序列与加转储前保持一致 */
[[gnu::noinline]] static void swizzle_sve_dump_mismatch(const float *golden,
                                                        const float *actual)
{
    /* cold path: locate mismatches, record bit-level evidence (bit pattern + float) */
    size_t _dump_bad = 0;
    int _dump_shown = 0;
    for (size_t _dump_j = 0; _dump_j < 8; ++_dump_j) {
        uint32_t _dump_g, _dump_a;
        memcpy(&_dump_g, &golden[_dump_j], 4);
        memcpy(&_dump_a, &actual[_dump_j], 4);
        if (_dump_g != _dump_a) {
            ++_dump_bad;
            if (_dump_shown < 8) {
                log_error("swizzle_sve: mismatch at index %zu: golden=0x%08x (%g) actual=0x%08x (%g) xor=0x%08x",
                          _dump_j, _dump_g, golden[_dump_j], _dump_a, actual[_dump_j],
                          _dump_g ^ _dump_a);
                ++_dump_shown;
            }
        }
    }
    log_error("swizzle_sve: total mismatched elements: %zu / %d", _dump_bad, 8);
}

static int swizzle_sve_run(struct test *test, int cpu) {
    (void)cpu;

    std::mt19937 rng(std::random_device{}());
    // 置换模式：交换每对相邻元素 (0↔1, 2↔3, 4↔5, 6↔7) —— 与原版一致
    int perm[8] = {1, 0, 3, 2, 5, 4, 7, 6};

    float src[8];
    float dst_ref[8];
    float dst_hw[8];

    static std::atomic<uint64_t> iter{0};

    do {
        for (int i = 0; i < 8; ++i) src[i] = (float)((int)(rng() % 20000) - 10000) / 100.0f;
        swizzle_reference(src, dst_ref, perm);

        /* === SVE 实现: svtbl 字节表查找做 32-bit lane 置换 ===
         * 每 float 4 字节: lane i 的字节来自 lane perm[i]。
         * 索引表按字节粒度: byte j of lane i ← byte j of lane perm[i]. */
        const int lanes = svcntw();          /* 谓词宽度, ≥ 8 时一批完成 */
        uint8_t srcbytes[32];
        uint8_t outbytes[32];
        uint8_t idx[32];
        int covered = (lanes >= 8) ? 8 : lanes;
        memcpy(srcbytes, src, 8 * sizeof(float));
        for (int i = 0; i < covered; ++i)
            for (int b = 0; b < 4; ++b)
                idx[i * 4 + b] = (uint8_t)(perm[i] * 4 + b);

        svbool_t pg = svwhilelt_b8((uint64_t)0, (uint64_t)(covered * 4));
        svuint8_t vidx = svld1_u8(pg, idx);
        svuint8_t vsrc = svld1_u8(pg, srcbytes);
        svuint8_t vout = svtbl_u8(vsrc, vidx);
        svst1_u8(pg, outbytes, vout);
        memcpy(dst_hw, outbytes, covered * sizeof(float));

        /* covered < 8 时(小 VL)剩余 lane 用标量补齐 —— 保持 8 元素完整比对 */
        for (int i = covered; i < 8; ++i)
            dst_hw[i] = src[perm[i]];

        bool passed = (memcmp(dst_hw, dst_ref, sizeof(dst_hw)) == 0);
        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);

        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";
        fprintf(stderr, "swizzle_sve: Iteration %lu, input[0..3] = %.2f, %.2f, %.2f, %.2f, result = %s%s\033[0m\n",
                iteration, src[0], src[1], src[2], src[3], color, result_str);

        if (!passed) {
            swizzle_sve_dump_mismatch(dst_ref, dst_hw);
            report_fail_msg("swizzle_sve: data mismatch");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int swizzle_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    __builtin_unreachable();
}
#endif

static int swizzle_sve_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(swizzle_sve, "Vector swizzle (pairwise-swap permute) via SVE svtbl byte-table lookup (port of swizzle)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = swizzle_sve_init,
    .test_run = swizzle_sve_run,
    .test_cleanup = swizzle_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
