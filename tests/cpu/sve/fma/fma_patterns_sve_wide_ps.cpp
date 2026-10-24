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

static constexpr int VECTOR_SIZE = 16; // 16 个单精度浮点数（与 fma_patterns_avx512_ps 负载一致）

static int fma_patterns_sve_wide_ps_init(struct test *test) {
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
static int fma_patterns_sve_wide_ps_run(struct test *test, int cpu) {
    (void)cpu;
    /* randomization hardening H9'/P12: framework RNG (per-thread stream,
     * -s reproducible) replaces std::mt19937; range [-1e6f, 1e6f) mapped
     * from frandom's [0,1). */
    auto dist = []() { return frandomf_scale((float)(1e6f) - (float)(-1e6f)) + (float)(-1e6f); };
    static std::atomic<uint64_t> iter{0};

    do {
        alignas(16) float a[VECTOR_SIZE];
        alignas(16) float b[VECTOR_SIZE];
        alignas(16) float c[VECTOR_SIZE];
        alignas(16) float hw_result[VECTOR_SIZE];
        float sw_ref[VECTOR_SIZE];

        for (int i = 0; i < VECTOR_SIZE; ++i) {
            a[i] = dist();
            b[i] = dist();
            c[i] = dist();
        }

        // ---- 硬件 FMA 计算 (SVE)：等价于原 NEON 版 4 × float32x4_t ----
        for (int base = 0; base < VECTOR_SIZE; base += svcntw()) {
            svbool_t pg = svwhilelt_b32((uint64_t)base, (uint64_t)VECTOR_SIZE);
            svfloat32_t va = svld1_f32(pg, a + base);
            svfloat32_t vb = svld1_f32(pg, b + base);
            svfloat32_t vc = svld1_f32(pg, c + base);
            svfloat32_t vd = svmla_f32_x(pg, vc, va, vb);
            svst1_f32(pg, hw_result + base, vd);
        }

        // ---- 软件参考：使用标准库 fmaf（正确舍入） ----
        for (int i = 0; i < VECTOR_SIZE; ++i) {
            sw_ref[i] = fmaf(a[i], b[i], c[i]);
        }

        // ---- 尾数精度测试：逐位比较硬件结果与参考值 ----
        bool data_ok = (memcmp(hw_result, sw_ref, sizeof(hw_result)) == 0);

        // ---- 一致性测试：存储硬件结果到内存再加载比较 ----
        float store_buf[VECTOR_SIZE];
        memcpy(store_buf, hw_result, sizeof(store_buf));
        float reload_buf[VECTOR_SIZE];
        memcpy(reload_buf, store_buf, sizeof(store_buf));
        bool consistent = (memcmp(reload_buf, hw_result, sizeof(reload_buf)) == 0);

        bool passed = data_ok && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        // ---- 输出日志（与原 NEON 版一致） ----
        fprintf(stderr, "fma_patterns_sve_wide_ps: Iter %lu, a[0..3]=%.12e %.12e %.12e %.12e\n",
                iteration, a[0], a[1], a[2], a[3]);
        fprintf(stderr, "                          b[0..3]=%.12e %.12e %.12e %.12e\n",
                b[0], b[1], b[2], b[3]);
        fprintf(stderr, "                          c[0..3]=%.12e %.12e %.12e %.12e\n",
                c[0], c[1], c[2], c[3]);
        fprintf(stderr, "  hw_result[0..3]=%.12e %.12e %.12e %.12e\n",
                hw_result[0], hw_result[1], hw_result[2], hw_result[3]);
        fprintf(stderr, "  sw_ref[0..3]=%.12e %.12e %.12e %.12e\n",
                sw_ref[0], sw_ref[1], sw_ref[2], sw_ref[3]);
        fprintf(stderr, "  data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
                data_ok, consistent, color, result_str);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("fma_patterns_sve_wide_ps: FMA pattern mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int fma_patterns_sve_wide_ps_run(struct test *test, int cpu) {
    (void)cpu;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM SVE FMA required");
    return EXIT_SKIP;
}
#endif

static int fma_patterns_sve_wide_ps_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(fma_patterns_sve_wide_ps, "SVE wide-vector single FMA pattern stress test (SVE port of fma_patterns_avx512_ps)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fma_patterns_sve_wide_ps_init,
    .test_run = fma_patterns_sve_wide_ps_run,
    .test_cleanup = fma_patterns_sve_wide_ps_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
