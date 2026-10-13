#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>

#ifdef __aarch64__
#include <arm_neon.h>

static int kreg2_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int kreg2_run(struct test *test, int cpu) {
    (void)cpu;
    auto mask_dist = []() { return (uint16_t)((0) + (int64_t)(random64() % (uint64_t)((0xFFFF) - (0) + 1))); };
    auto shift_dist = []() { return (int)((0) + (int64_t)(random64() % (uint64_t)((15) - (0) + 1))); };
    static std::atomic<uint64_t> iter{0};

    do {
        uint16_t src_mask_val = mask_dist();
        int shift_amount = shift_dist();

        /* 硬件: NEON 变量移位指令 vshlq_u16 —— 第二操作数为有符号移位
         * 计数向量: 正数 = 左移, 负数 = 逻辑右移 (x86 KSHIFTL/KSHIFTR
         * 的 NEON 等价物), 源掩码广播到全部 8 lane, 结果经真实内存
         * (vst1q/vld1q) 往返 (原版 sw 与 hw 是同一 C 标量表达式, 结构
         * 上恒 pass —— B1-004)。 */
        uint16_t out_l[8] __attribute__((aligned(16)));
        uint16_t out_r[8] __attribute__((aligned(16)));
        {
            uint16x8_t v   = vdupq_n_u16(src_mask_val);
            int16x8_t  lsh = vdupq_n_s16(static_cast<int16_t>(shift_amount));
            int16x8_t  rsh = vdupq_n_s16(static_cast<int16_t>(-shift_amount));
            vst1q_u16(out_l, vshlq_u16(v, lsh));   /* KSHIFTL 等价 */
            vst1q_u16(out_r, vshlq_u16(v, rsh));   /* KSHIFTR 等价 */
        }

        /* 软件参考: 独立标量移位。 */
        uint16_t sw_left  = static_cast<uint16_t>(src_mask_val << shift_amount);
        uint16_t sw_right = static_cast<uint16_t>(src_mask_val >> shift_amount);

        /* 交叉验证: 读回全部 8 lane 与标量 golden 的广播向量比对
         * (vceqq + vminvq), 任一 lane 被 SDC 破坏都会被捕获。 */
        bool left_pass =
            (vminvq_u16(vceqq_u16(vld1q_u16(out_l), vdupq_n_u16(sw_left))) != 0);
        bool right_pass =
            (vminvq_u16(vceqq_u16(vld1q_u16(out_r), vdupq_n_u16(sw_right))) != 0);
        bool passed = left_pass && right_pass;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);

        if (!passed) {
            const char *color = passed ? "\033[32m" : "\033[31m";
            const char *result_str = passed ? "PASS" : "FAIL";
            fprintf(stderr, "kreg2: Iter %lu, src=0x%04X, shift=%d\n",
                    iteration, src_mask_val, shift_amount);
            fprintf(stderr, "  sw: left=0x%04X, right=0x%04X\n", sw_left, sw_right);
            fprintf(stderr, "  hw: left=0x%04X, right=0x%04X\n",
                    out_l[0], out_r[0]);
            fprintf(stderr, "  result=%s%s\033[0m\n", color, result_str);
            report_fail_msg("kreg2: mismatch in NEON vector shift");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int kreg2_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#else // !__aarch64__

// On non-AArch64 (e.g. x86-64) there is no <arm_neon.h>; the test self-skips
// in test_init so the result is marked ignored rather than spuriously
// passing (kreg1.cpp precedent).
static int kreg2_init(struct test *test) {
    (void)test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON required for "
             "mask register shift test (vshlq variable shift)");
    return EXIT_SKIP;
}

static int kreg2_run(struct test *test, int cpu) {
    (void)test;
    (void)cpu;
    __builtin_unreachable();    // init reports EXIT_SKIP, run never executes
}

static int kreg2_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(kreg2, "Mask register shift operations (KSHIFTL, KSHIFTR) on real NEON variable shifts")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg2_init,
    .test_run = kreg2_run,
    .test_cleanup = kreg2_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
