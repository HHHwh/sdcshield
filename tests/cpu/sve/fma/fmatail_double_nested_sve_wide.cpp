#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <cmath>

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

static constexpr int VECTOR_SIZE = 8;              // 8 个双精度浮点数（与 fmatail_double_nested_avx512 负载一致）
static constexpr int INNER_ITERATIONS = 10;        // 内层循环每组生成的随机数据组数（与原版一致）

static int fmatail_double_nested_sve_wide_init(struct test *test) {
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
static int fmatail_double_nested_sve_wide_run(struct test *test, int cpu) {
    (void)cpu;
    /* randomization hardening H9'/P12: framework RNG (per-thread stream,
     * -s reproducible) replaces std::mt19937; range [-1e10, 1e10) mapped
     * from frandom's [0,1). */
    auto dist = []() { return frandom_scale((double)(1e10) - (double)(-1e10)) + (double)(-1e10); };
    static std::atomic<uint64_t> iter{0};

    do {
        bool all_passed = true;

        // 内层循环：生成多组随机数据（INNER_ITERATIONS=10 组，与原版一致）
        for (int group = 0; group < INNER_ITERATIONS; ++group) {
            alignas(16) double a[VECTOR_SIZE];
            alignas(16) double b[VECTOR_SIZE];
            alignas(16) double c[VECTOR_SIZE];
            alignas(16) double hw_result[VECTOR_SIZE];
            double sw_ref[VECTOR_SIZE];

            // 生成随机向量，并偶尔插入特殊值（0, 1, -1, Inf）——注入模式与原版逐字一致
            for (int i = 0; i < VECTOR_SIZE; ++i) {
                a[i] = dist();
                b[i] = dist();
                c[i] = dist();
                // 随机插入特殊值
                if (i % 3 == 0) {
                    switch ((i + group) % 4) {
                        case 0: a[i] = 0.0; break;
                        case 1: b[i] = 1.0; break;
                        case 2: c[i] = -1.0; break;
                        case 3: a[i] = INFINITY; break;
                    }
                }
            }

            // ---- 硬件 FMA 计算 (SVE) ----
            for (int base = 0; base < VECTOR_SIZE; base += svcntd()) {
                svbool_t pg = svwhilelt_b64((uint64_t)base, (uint64_t)VECTOR_SIZE);
                svfloat64_t va = svld1_f64(pg, a + base);
                svfloat64_t vb = svld1_f64(pg, b + base);
                svfloat64_t vc = svld1_f64(pg, c + base);
                svfloat64_t vd = svmla_f64_x(pg, vc, va, vb);
                svst1_f64(pg, hw_result + base, vd);
            }

            // ---- 软件参考：使用标准库 fma（正确舍入） ----
            for (int i = 0; i < VECTOR_SIZE; ++i) {
                sw_ref[i] = fma(a[i], b[i], c[i]);
            }

            // ---- 尾数精度测试：逐位比较硬件结果与参考值 ----
            bool data_ok = (memcmp(hw_result, sw_ref, sizeof(hw_result)) == 0);

            // ---- 一致性测试：存储硬件结果到内存再加载比较 ----
            double store_buf[VECTOR_SIZE];
            memcpy(store_buf, hw_result, sizeof(store_buf));
            double reload_buf[VECTOR_SIZE];
            memcpy(reload_buf, store_buf, sizeof(store_buf));
            bool consistent = (memcmp(reload_buf, hw_result, sizeof(reload_buf)) == 0);

            bool group_pass = data_ok && consistent;
            if (!group_pass) all_passed = false;

            // ---- 输出该组信息（与原 NEON 版一致） ----
            uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
            const char *color = group_pass ? "\033[32m" : "\033[31m";
            const char *result_str = group_pass ? "PASS" : "FAIL";

            fprintf(stderr, "fmatail_double_nested_sve_wide: Iter %lu, Group %d, a[0..3]=%.12e %.12e %.12e %.12e\n",
                    iteration, group, a[0], a[1], a[2], a[3]);
            fprintf(stderr, "                                             b[0..3]=%.12e %.12e %.12e %.12e\n",
                    b[0], b[1], b[2], b[3]);
            fprintf(stderr, "                                             c[0..3]=%.12e %.12e %.12e %.12e\n",
                    c[0], c[1], c[2], c[3]);
            fprintf(stderr, "  hw_result[0..3]=%.12e %.12e %.12e %.12e\n",
                    hw_result[0], hw_result[1], hw_result[2], hw_result[3]);
            fprintf(stderr, "  sw_ref[0..3]=%.12e %.12e %.12e %.12e\n",
                    sw_ref[0], sw_ref[1], sw_ref[2], sw_ref[3]);
            fprintf(stderr, "  data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
                    data_ok, consistent, color, result_str);
            fflush(stderr);
        }

        if (!all_passed) {
            report_fail_msg("fmatail_double_nested_sve_wide: Some group failed");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int fmatail_double_nested_sve_wide_run(struct test *test, int cpu) {
    (void)cpu;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM SVE FMA required");
    return EXIT_SKIP;
}
#endif

static int fmatail_double_nested_sve_wide_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(fmatail_double_nested_sve_wide, "SVE wide-vector double FMA exhaustive tail precision with double nested loops (SVE port of fmatail_double_nested_avx512)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fmatail_double_nested_sve_wide_init,
    .test_run = fmatail_double_nested_sve_wide_run,
    .test_cleanup = fmatail_double_nested_sve_wide_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
