#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

// 打印整数数组（最多 8 个元素）
static void print_vector(const char* label, const int32_t* v, int len) {
    fprintf(stderr, "%s: [", label);
    for (int i = 0; i < len && i < 8; ++i) {
        fprintf(stderr, " %d", v[i]);
        if (i < len-1 && i < 7) fprintf(stderr, ",");
    }
    if (len > 8) fprintf(stderr, ", ...");
    fprintf(stderr, " ]");
}
static void print_vector(const char* label, const int64_t* v, int len) {
    fprintf(stderr, "%s: [", label);
    for (int i = 0; i < len && i < 8; ++i) {
        fprintf(stderr, " %ld", v[i]);
        if (i < len-1 && i < 7) fprintf(stderr, ",");
    }
    if (len > 8) fprintf(stderr, ", ...");
    fprintf(stderr, " ]");
}

static int vmovnt3_vex_init(struct test *test) {
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

static int vmovnt3_vex_run(struct test *test, int cpu) {
    (void)cpu;
#ifdef __aarch64__
    // ARM64 上 NEON 默认可用，无需检测

    // 线程局部数据（对齐至 16 字节，NEON 加载/存储对齐要求）
    alignas(16) int32_t src_i32[8];
    alignas(16) int32_t loaded_i32[8];
    alignas(16) int32_t store_i32[8];
    alignas(16) int32_t reload_i32[8];
    alignas(16) int64_t src_i64[4];
    alignas(16) int64_t loaded_i64[4];
    alignas(16) int64_t store_i64[4];
    alignas(16) int64_t reload_i64[4];

    /* randomization hardening (B3-007): framework RNG (per-thread stream,
     * -s reproducible) replaces std::mt19937; ranges [-10000, 10000] and
     * [-1000000, 1000000]. */
    auto i32dist = []() { return (int32_t)((-10000) + (int64_t)(random64() % (uint64_t)((10000) - (-10000) + 1))); };
    auto i64dist = []() { return (int64_t)((-1000000LL) + (int64_t)(random64() % (uint64_t)((1000000LL) - (-1000000LL) + 1))); };

    do {
        // 生成随机数据
        for (int i = 0; i < 8; ++i) src_i32[i] = i32dist();
        for (int i = 0; i < 4; ++i) src_i64[i] = i64dist();

        // ---- 测试 vmovntdqa (VEX, int32) ----
        // 模拟非临时加载：使用 NEON 加载 + 预取
        __builtin_prefetch(src_i32, 0, 3);  // 预取到缓存

        // 加载两个 128 位向量（共 8 个 int32）
        int32x4_t v0_i32 = vld1q_s32(src_i32);
        int32x4_t v1_i32 = vld1q_s32(src_i32 + 4);
        vst1q_s32(loaded_i32, v0_i32);
        vst1q_s32(loaded_i32 + 4, v1_i32);

        bool data_ok_i32 = (memcmp(src_i32, loaded_i32, 8 * sizeof(int32_t)) == 0);

        // 一致性测试：存储加载结果到另一位置再加载比较
        vst1q_s32(store_i32, v0_i32);
        vst1q_s32(store_i32 + 4, v1_i32);
        __sync_synchronize();  // 确保存储完成

        int32x4_t v_reload0_i32 = vld1q_s32(store_i32);
        int32x4_t v_reload1_i32 = vld1q_s32(store_i32 + 4);
        vst1q_s32(reload_i32, v_reload0_i32);
        vst1q_s32(reload_i32 + 4, v_reload1_i32);
        bool consistent_i32 = (memcmp(loaded_i32, reload_i32, 8 * sizeof(int32_t)) == 0);

        if (!(data_ok_i32 && consistent_i32)) {
            fprintf(stderr, "\n[vmovntdqa (VEX, int32)] FAIL (CPU %d)\n", cpu);
            print_vector("  src", src_i32, 8);
            print_vector("  loaded", loaded_i32, 8);
            print_vector("  reloaded", reload_i32, 8);
            fprintf(stderr, "  data_ok=%d consistent=%d\n", data_ok_i32, consistent_i32);
            report_fail_msg("vmovntdqa (VEX, int32) mismatch");
            return EXIT_FAILURE;
        }
        fprintf(stderr, "\033[32mvmovntdqa (VEX, int32) PASS\033[0m\n");

        // ---- 测试 vmovntdqa (VEX, int64) ----
        __builtin_prefetch(src_i64, 0, 3);

        int64x2_t v0_i64 = vld1q_s64(src_i64);
        int64x2_t v1_i64 = vld1q_s64(src_i64 + 2);
        vst1q_s64(loaded_i64, v0_i64);
        vst1q_s64(loaded_i64 + 2, v1_i64);

        bool data_ok_i64 = (memcmp(src_i64, loaded_i64, 4 * sizeof(int64_t)) == 0);

        vst1q_s64(store_i64, v0_i64);
        vst1q_s64(store_i64 + 2, v1_i64);
        __sync_synchronize();

        int64x2_t v_reload0_i64 = vld1q_s64(store_i64);
        int64x2_t v_reload1_i64 = vld1q_s64(store_i64 + 2);
        vst1q_s64(reload_i64, v_reload0_i64);
        vst1q_s64(reload_i64 + 2, v_reload1_i64);
        bool consistent_i64 = (memcmp(loaded_i64, reload_i64, 4 * sizeof(int64_t)) == 0);

        if (!(data_ok_i64 && consistent_i64)) {
            fprintf(stderr, "\n[vmovntdqa (VEX, int64)] FAIL (CPU %d)\n", cpu);
            print_vector("  src", src_i64, 4);
            print_vector("  loaded", loaded_i64, 4);
            print_vector("  reloaded", reload_i64, 4);
            fprintf(stderr, "  data_ok=%d consistent=%d\n", data_ok_i64, consistent_i64);
            report_fail_msg("vmovntdqa (VEX, int64) mismatch");
            return EXIT_FAILURE;
        }
        fprintf(stderr, "\033[32mvmovntdqa (VEX, int64) PASS\033[0m\n");

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for this memory test");
    return EXIT_SKIP;
#endif
}

static int vmovnt3_vex_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(vmovnt3_vex,
             "Simple test for vmovnt instructions - vmovntdqa (vex version, ARM64 NEON)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = vmovnt3_vex_init,
    .test_run = vmovnt3_vex_run,
    .test_cleanup = vmovnt3_vex_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
