#include <sandstone.h>
#include <cstdint>
#include <cmath>
#include <random>
#include <cstring>

#ifdef __aarch64__
#include <arm_sve.h>
#include <sys/auxv.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif
#endif

static constexpr size_t VECTOR_SIZE = 8;   // 8 个单精度浮点数（与原版一致）

// 软件参考：使用 fmaf 逐元素计算（单次舍入）
static void software_fma(const float *a, const float *b, const float *c, float *ref) {
    for (int i = 0; i < (int)VECTOR_SIZE; ++i) {
        ref[i] = fmaf(a[i], b[i], c[i]);
    }
}

// 比较两个浮点数数组是否近似相等（允许 1e-6 相对误差，与原版一致）
static bool approx_equal(const float *x, const float *y) {
    for (int i = 0; i < (int)VECTOR_SIZE; ++i) {
        float diff = fabsf(x[i] - y[i]);
        float tol = 1e-6f * fmaxf(fabsf(x[i]), fabsf(y[i]));
        if (diff > tol && diff > 1e-7f) {
            return false;
        }
    }
    return true;
}

struct TestData {};

static int fma_sve_init(struct test *test) {
    (void)test;
#ifdef __aarch64__
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for fma_sve");
        return EXIT_SKIP;
    }
#endif
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int fma_sve_run(struct test *test, int cpu) {
    (void)cpu;
    // 每个线程独立分配数据（栈上）
    float a[VECTOR_SIZE];
    float b[VECTOR_SIZE];
    float c[VECTOR_SIZE];
    float result[VECTOR_SIZE];

    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dist(-100.0f, 100.0f);

    do {
        // 生成随机向量 a, b, c
        for (int i = 0; i < (int)VECTOR_SIZE; ++i) {
            a[i] = dist(rng);
            b[i] = dist(rng);
            c[i] = dist(rng);
        }

        // ---- 硬件 FMA 计算 (SVE): svmla_f32_x 即 c + a*b, VL 无关分批 ----
        for (size_t base = 0; base < VECTOR_SIZE; base += svcntw()) {
            svbool_t pg = svwhilelt_b32((uint64_t)base, (uint64_t)VECTOR_SIZE);
            svfloat32_t va = svld1_f32(pg, a + base);
            svfloat32_t vb = svld1_f32(pg, b + base);
            svfloat32_t vc = svld1_f32(pg, c + base);
            svst1_f32(pg, result + base, svmla_f32_x(pg, vc, va, vb));
        }

        // ---- 软件参考计算 ----
        float ref[VECTOR_SIZE];
        software_fma(a, b, c, ref);

        // 比较硬件结果与参考
        bool data_ok = approx_equal(result, ref);

        // 一致性测试：存储硬件结果到内存再加载比较
        float store_buf[VECTOR_SIZE];
        memcpy(store_buf, result, sizeof(store_buf));
        float reload_buf[VECTOR_SIZE];
        memcpy(reload_buf, store_buf, sizeof(store_buf));
        bool consistent = (memcmp(reload_buf, result, sizeof(reload_buf)) == 0);

        if (!(data_ok && consistent)) {
            report_fail_msg("fma_sve: FMA result mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int fma_sve_run(struct test *test, int cpu) {
    (void)cpu;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM SVE FMA required");
    return EXIT_SKIP;
}
#endif

static int fma_sve_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(fma_sve, "FMA instruction basic test (SVE single-precision, port of fma)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fma_sve_init,
    .test_run = fma_sve_run,
    .test_cleanup = fma_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
