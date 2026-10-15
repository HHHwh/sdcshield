/**
 * @copyright
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b vmovnt3
 * @parblock
 * Simple test for vmovnt instructions - vmovntdqa (ARM64 NEON version)
 *
 * This test simulates non-temporal load and prefetch into cache using NEON
 * loads and software prefetch. It validates that data loaded from memory
 * is correctly prefetched and stored back with consistency.
 * Input data are random floats in [-1, 1].
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

// ============================================================================
// 测试配置
// ============================================================================

static constexpr size_t VECTOR_SIZE = 4;      // float32x4_t 含 4 个元素
static constexpr size_t NUM_VECTORS = 4;      // 总共 16 个 float（128 位 × 4 = 512 位）
static constexpr size_t TOTAL_FLOATS = NUM_VECTORS * VECTOR_SIZE;  // 16

// ============================================================================
// 辅助函数：打印浮点向量（带颜色）
// ============================================================================

static void print_vector(const char* label, const float* v, size_t len) {
    fprintf(stderr, "%s: [", label);
    for (size_t i = 0; i < len; ++i) {
        fprintf(stderr, " %.4f", v[i]);
        if (i < len - 1) fprintf(stderr, ",");
    }
    fprintf(stderr, " ]");
}

static void print_colored_result(const char* test_name, bool passed,
                                 const float* input, const float* output,
                                 size_t len) {
    const char *color = passed ? "\033[32m" : "\033[31m";
    const char *result_str = passed ? "PASS" : "FAIL";

    fprintf(stderr, "%s: %s%s\033[0m\n", test_name, color, result_str);
    print_vector("  input", input, len);
    fprintf(stderr, "\n");
    print_vector("  output", output, len);
    fprintf(stderr, "\n");
    fflush(stderr);
}

// ============================================================================
// 测试生命周期函数
// ============================================================================

static int vmovnt3_init(struct test *test) {
#ifndef __aarch64__
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for this memory test");
    return EXIT_SKIP;
#else
    (void)test;
    return EXIT_SUCCESS;
#endif
}

static int vmovnt3_run(struct test *test, int cpu) {
    (void)cpu;
#ifdef __aarch64__
    // 线程局部数据，对齐到 16 字节（NEON 加载/存储对齐要求）
    alignas(16) float src[TOTAL_FLOATS];
    alignas(16) float store_buf[TOTAL_FLOATS];
    alignas(16) float reload_buf[TOTAL_FLOATS];

    /* randomization hardening (B3-007): framework RNG (per-thread stream,
     * -s reproducible) replaces std::mt19937; range [-1.0f, 1.0f) mapped
     * from frandomf's [0,1). */
    auto dist = []() { return frandomf_scale((float)(1.0f) - (float)(-1.0f)) + (float)(-1.0f); };

    do {
        // ---------- 1. 生成随机浮点数 ----------
        for (size_t i = 0; i < TOTAL_FLOATS; ++i) {
            src[i] = dist();
        }

        // ---------- 2. 模拟非临时加载 + 预取 ----------
        // 将 src 拆分为 4 个 NEON 向量并加载
        float32x4_t v0 = vld1q_f32(src);
        float32x4_t v1 = vld1q_f32(src + 4);
        float32x4_t v2 = vld1q_f32(src + 8);
        float32x4_t v3 = vld1q_f32(src + 12);

        // 软件预取：提示 CPU 预取数据到缓存（模拟 vmovntdqa 的预取效果）
        // 这里对 src 的起始地址进行预取，实际应用中预取指令通常在加载前使用。
        __builtin_prefetch(src, 0, 3);  // 读预取，高局部性

        // 为了模拟 "非临时" 特性，我们可以添加内存屏障确保顺序，但此处重点在于数据一致性

        // ---------- 3. 存储向量到 store_buf ----------
        vst1q_f32(store_buf, v0);
        vst1q_f32(store_buf + 4, v1);
        vst1q_f32(store_buf + 8, v2);
        vst1q_f32(store_buf + 12, v3);

        // 内存屏障，确保存储完成（模拟非临时存储的序列化效果）
        __sync_synchronize();

        // ---------- 4. 从 store_buf 重新加载 ----------
        float32x4_t reload0 = vld1q_f32(store_buf);
        float32x4_t reload1 = vld1q_f32(store_buf + 4);
        float32x4_t reload2 = vld1q_f32(store_buf + 8);
        float32x4_t reload3 = vld1q_f32(store_buf + 12);
        vst1q_f32(reload_buf, reload0);
        vst1q_f32(reload_buf + 4, reload1);
        vst1q_f32(reload_buf + 8, reload2);
        vst1q_f32(reload_buf + 12, reload3);

        // ---------- 5. 一致性测试：比较原始数据与重新加载的数据 ----------
        bool data_ok = (memcmp(src, reload_buf, sizeof(src)) == 0);

        // 存储一致性测试：比较存储缓冲区与重新加载的数据（应完全一致）
        bool consistent = (memcmp(store_buf, reload_buf, sizeof(store_buf)) == 0);

        bool passed = data_ok && consistent;

        // ---------- 6. 输出结果 ----------
        // 这里我们将 reload_buf 视为 "输出"（即从内存读取回来的数据）
        // 仅在失败时打印，避免每轮 PASS 导致日志膨胀。
        if (!passed) {
            print_colored_result("vmovnt3", passed, src, reload_buf, TOTAL_FLOATS);
            report_fail_msg("vmovnt3: data mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for this memory test");
    return EXIT_SKIP;
#endif
}

static int vmovnt3_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

// ============================================================================
// 测试注册
// ============================================================================

DECLARE_TEST(vmovnt3,
             "Simple test for vmovnt instructions - vmovntdqa (ARM64 NEON)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = vmovnt3_init,
    .test_run = vmovnt3_run,
    .test_cleanup = vmovnt3_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
