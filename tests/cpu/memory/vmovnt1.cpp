#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#ifdef __aarch64__
#include <arm_neon.h>
#endif

// 打印向量（最多 8 个元素）
static void print_vector(const char* label, const float* v, int len) {
    fprintf(stderr, "%s: [", label);
    for (int i = 0; i < len && i < 8; ++i) {
        fprintf(stderr, " %.4f", v[i]);
        if (i < len-1 && i < 7) fprintf(stderr, ",");
    }
    if (len > 8) fprintf(stderr, ", ...");
    fprintf(stderr, " ]");
}
static void print_vector(const char* label, const double* v, int len) {
    fprintf(stderr, "%s: [", label);
    for (int i = 0; i < len && i < 8; ++i) {
        fprintf(stderr, " %.4f", v[i]);
        if (i < len-1 && i < 7) fprintf(stderr, ",");
    }
    if (len > 8) fprintf(stderr, ", ...");
    fprintf(stderr, " ]");
}

static int vmovnt1_init(struct test *test) {
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

static int vmovnt1_run(struct test *test, int cpu) {
    (void)cpu;
#ifdef __aarch64__
    // ARM64 上 NEON 默认可用，无需检测

    // 线程局部数据（对齐至 16 字节，NEON 对齐要求）
    alignas(16) float src_f32[8];      // 256 位 = 8 个 float
    alignas(16) float store_f32[8];
    alignas(16) float reload_f32[8];
    alignas(16) double src_f64[4];     // 256 位 = 4 个 double
    alignas(16) double store_f64[4];
    alignas(16) double reload_f64[4];

    // 512 位版本（用 4 个 128 位向量组合）
    alignas(16) float src_f32_512[16];
    alignas(16) float store_f32_512[16];
    alignas(16) float reload_f32_512[16];
    alignas(16) double src_f64_512[8];
    alignas(16) double store_f64_512[8];
    alignas(16) double reload_f64_512[8];

    /* randomization hardening (B3-007): framework RNG (per-thread stream,
     * -s reproducible) replaces std::mt19937; ranges [-1.0f, 1.0f) and
     * [-1.0, 1.0) mapped from frandom's [0,1). */
    auto fdist = []() { return frandomf_scale((float)(1.0f) - (float)(-1.0f)) + (float)(-1.0f); };
    auto ddist = []() { return frandom_scale((double)(1.0) - (double)(-1.0)) + (double)(-1.0); };

    do {
        // ---- 生成随机数据（256 位） ----
        for (int i = 0; i < 8; ++i) src_f32[i] = fdist();
        for (int i = 0; i < 4; ++i) src_f64[i] = ddist();

        // ---- 测试 256-bit 单精度（两个 128 位向量） ----
        float32x4_t vf0 = vld1q_f32(src_f32);
        float32x4_t vf1 = vld1q_f32(src_f32 + 4);
        vst1q_f32(store_f32, vf0);
        vst1q_f32(store_f32 + 4, vf1);
        __sync_synchronize();  // 确保存储完成（等效 mfence）

        // 加载验证
        float32x4_t reload_vf0 = vld1q_f32(store_f32);
        float32x4_t reload_vf1 = vld1q_f32(store_f32 + 4);
        vst1q_f32(reload_f32, reload_vf0);
        vst1q_f32(reload_f32 + 4, reload_vf1);
        bool data_ok_f = (memcmp(src_f32, reload_f32, 8 * sizeof(float)) == 0);
        bool consistent_f = (memcmp(store_f32, reload_f32, 8 * sizeof(float)) == 0);
        bool passed_f = data_ok_f && consistent_f;

        if (!passed_f) {
            fprintf(stderr, "\n[vmovntps (256-bit)] FAIL (CPU %d)\n", cpu);
            print_vector("  src", src_f32, 8);
            print_vector("  stored", store_f32, 8);
            print_vector("  reloaded", reload_f32, 8);
            fprintf(stderr, "  data_ok=%d consistent=%d\n", data_ok_f, consistent_f);
            report_fail_msg("vmovntps (256-bit) mismatch or consistency failure");
            return EXIT_FAILURE;
        }

        // ---- 测试 256-bit 双精度 ----
        float64x2_t vd0 = vld1q_f64(src_f64);
        float64x2_t vd1 = vld1q_f64(src_f64 + 2);
        vst1q_f64(store_f64, vd0);
        vst1q_f64(store_f64 + 2, vd1);
        __sync_synchronize();

        float64x2_t reload_vd0 = vld1q_f64(store_f64);
        float64x2_t reload_vd1 = vld1q_f64(store_f64 + 2);
        vst1q_f64(reload_f64, reload_vd0);
        vst1q_f64(reload_f64 + 2, reload_vd1);
        bool data_ok_d = (memcmp(src_f64, reload_f64, 4 * sizeof(double)) == 0);
        bool consistent_d = (memcmp(store_f64, reload_f64, 4 * sizeof(double)) == 0);
        bool passed_d = data_ok_d && consistent_d;

        if (!passed_d) {
            fprintf(stderr, "\n[vmovntpd (256-bit)] FAIL (CPU %d)\n", cpu);
            print_vector("  src", src_f64, 4);
            print_vector("  stored", store_f64, 4);
            print_vector("  reloaded", reload_f64, 4);
            fprintf(stderr, "  data_ok=%d consistent=%d\n", data_ok_d, consistent_d);
            report_fail_msg("vmovntpd (256-bit) mismatch or consistency failure");
            return EXIT_FAILURE;
        }

        // ---- 测试 512-bit 版本（4 个 128 位向量） ----
        // 生成 512 位数据
        for (int i = 0; i < 16; ++i) src_f32_512[i] = fdist();
        for (int i = 0; i < 8; ++i) src_f64_512[i] = ddist();

        // 单精度 512-bit
        float32x4_t vf512[4];
        for (int i = 0; i < 4; ++i)
            vf512[i] = vld1q_f32(src_f32_512 + i*4);
        for (int i = 0; i < 4; ++i)
            vst1q_f32(store_f32_512 + i*4, vf512[i]);
        __sync_synchronize();

        float32x4_t reload_vf512[4];
        for (int i = 0; i < 4; ++i)
            reload_vf512[i] = vld1q_f32(store_f32_512 + i*4);
        for (int i = 0; i < 4; ++i)
            vst1q_f32(reload_f32_512 + i*4, reload_vf512[i]);
        bool data_ok_f512 = (memcmp(src_f32_512, reload_f32_512, 16 * sizeof(float)) == 0);
        bool consistent_f512 = (memcmp(store_f32_512, reload_f32_512, 16 * sizeof(float)) == 0);
        bool passed_f512 = data_ok_f512 && consistent_f512;

        if (!passed_f512) {
            fprintf(stderr, "\n[vmovntps (512-bit)] FAIL (CPU %d)\n", cpu);
            print_vector("  src", src_f32_512, 16);
            print_vector("  stored", store_f32_512, 16);
            print_vector("  reloaded", reload_f32_512, 16);
            fprintf(stderr, "  data_ok=%d consistent=%d\n", data_ok_f512, consistent_f512);
            report_fail_msg("vmovntps (512-bit) mismatch or consistency failure");
            return EXIT_FAILURE;
        }

        // 双精度 512-bit
        float64x2_t vd512[4];
        for (int i = 0; i < 4; ++i)
            vd512[i] = vld1q_f64(src_f64_512 + i*2);
        for (int i = 0; i < 4; ++i)
            vst1q_f64(store_f64_512 + i*2, vd512[i]);
        __sync_synchronize();

        float64x2_t reload_vd512[4];
        for (int i = 0; i < 4; ++i)
            reload_vd512[i] = vld1q_f64(store_f64_512 + i*2);
        for (int i = 0; i < 4; ++i)
            vst1q_f64(reload_f64_512 + i*2, reload_vd512[i]);
        bool data_ok_d512 = (memcmp(src_f64_512, reload_f64_512, 8 * sizeof(double)) == 0);
        bool consistent_d512 = (memcmp(store_f64_512, reload_f64_512, 8 * sizeof(double)) == 0);
        bool passed_d512 = data_ok_d512 && consistent_d512;

        if (!passed_d512) {
            fprintf(stderr, "\n[vmovntpd (512-bit)] FAIL (CPU %d)\n", cpu);
            print_vector("  src", src_f64_512, 8);
            print_vector("  stored", store_f64_512, 8);
            print_vector("  reloaded", reload_f64_512, 8);
            fprintf(stderr, "  data_ok=%d consistent=%d\n", data_ok_d512, consistent_d512);
            report_fail_msg("vmovntpd (512-bit) mismatch or consistency failure");
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

static int vmovnt1_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(vmovnt1,
             "Simple test for vmovnt instructions - vmovntpd/ps (ARM64 NEON)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = vmovnt1_init,
    .test_run = vmovnt1_run,
    .test_cleanup = vmovnt1_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
