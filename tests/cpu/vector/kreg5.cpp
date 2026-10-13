#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>

#ifdef __aarch64__
#include <arm_neon.h>

static int kreg5_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int kreg5_run(struct test *test, int cpu) {
    (void)cpu;
    auto mask16_dist = []() { return (uint16_t)((0) + (int64_t)(random64() % (uint64_t)((0xFFFF) - (0) + 1))); };
    static std::atomic<uint64_t> iter{0};

    do {
        uint16_t a_val = mask16_dist();
        uint16_t b_val = mask16_dist();

        /* 硬件: KUNPCK 用真交织指令 vzip1q (lane 交替 [a,b,a,b,...]);
         * KNOT 用按位取反指令 vmvnq。数据全程驻留向量通路。 */
        uint16_t zo[8], no[8];
        uint32_t hw_c;
        {
            uint16x8_t va = vdupq_n_u16(a_val);
            uint16x8_t vb = vdupq_n_u16(b_val);
            uint16x8_t z = vzip1q_u16(va, vb);
            uint16x8_t n = vmvnq_u16(va);
            vst1q_u16(zo, z);
            vst1q_u16(no, n);
            /* 小端 32 位视图: 低 16 位 = a, 高 16 位 = b */
            hw_c = ((uint32_t)zo[1] << 16) | zo[0];
        }

        /* 软件参考: 独立标量 (原版 sw 直接赋值自 hw, 结构上恒 pass —— B1-004)。 */
        uint32_t sw_c = ((uint32_t)b_val << 16) | a_val;
        uint16_t sw_not = (uint16_t)~a_val;

        /* 交叉验证: 读回数组的全部 8 lane 与独立标量预期比对
         * (交织: 偶 lane = a, 奇 lane = b; 取反: 每 lane = ~a),
         * 任一 lane 被 SDC 破坏都会被捕获。 */
        bool zip_pass = true;
        for (int i = 0; i < 8; ++i)
            zip_pass = zip_pass && (zo[i] == ((i & 1) ? b_val : a_val));
        bool not_pass = true;
        for (int i = 0; i < 8; ++i)
            not_pass = not_pass && (no[i] == sw_not);
        bool passed = zip_pass && not_pass && (hw_c == sw_c);

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);

        if (!passed) {
            const char *color = passed ? "\033[32m" : "\033[31m";
            const char *result_str = passed ? "PASS" : "FAIL";
            fprintf(stderr, "kreg5: Iter %lu, a=0x%04X, b=0x%04X\n",
                    iteration, a_val, b_val);
            fprintf(stderr, "  KUNPCK: sw=0x%08X, hw=0x%08X\n", sw_c, hw_c);
            fprintf(stderr, "  KNOT:   sw=0x%04X, hw=0x%04X\n", sw_not, no[0]);
            fprintf(stderr, "  result=%s%s\033[0m\n", color, result_str);
            report_fail_msg("kreg5: mismatch in NEON interleave/not");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int kreg5_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#else // !__aarch64__

// On non-AArch64 (e.g. x86-64) there is no <arm_neon.h>; the test self-skips
// in test_init so the result is marked ignored rather than spuriously
// passing (kreg1.cpp precedent).
static int kreg5_init(struct test *test) {
    (void)test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON required for "
             "mask register unpack and not (interleave/not)");
    return EXIT_SKIP;
}

static int kreg5_run(struct test *test, int cpu) {
    (void)test;
    (void)cpu;
    __builtin_unreachable();    // init reports EXIT_SKIP, run never executes
}

static int kreg5_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(kreg5, "Mask unpack and not (KUNPCK, KNOT) on real NEON interleave instruction (vzip1q)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg5_init,
    .test_run = kreg5_run,
    .test_cleanup = kreg5_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
