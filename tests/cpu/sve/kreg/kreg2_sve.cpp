#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <random>
#include <cstring>
#include <atomic>
#include <ctime>
#include <unistd.h>

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

static int kreg2_sve_init(struct test *test) {
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
static int kreg2_sve_run(struct test *test, int cpu) {
    (void)cpu;
    std::mt19937 rng(static_cast<unsigned>(time(nullptr)) + getpid());
    std::uniform_int_distribution<uint16_t> mask_dist(0, 0xFFFF);
    std::uniform_int_distribution<int> shift_dist(0, 15);
    static std::atomic<uint64_t> iter{0};

    do {
        uint16_t src_mask_val = mask_dist(rng);
        int shift_amount = shift_dist(rng);

        /* SVE 硬件: 向量移位指令 svlsl/svlsr (真指令, 非整数标量仿真)。
         * VL 无关: 谓词按 svcnth 分批, 数组按最大 16 lane 准备。 */
        uint16_t left_shifted = 0, right_shifted = 0;
        {
            uint16_t in[16];
            const int lanes = svcnth();
            for (int i = 0; i < lanes && i < 16; ++i) in[i] = src_mask_val;
            svbool_t pg = svwhilelt_b16((uint64_t)0, (uint64_t)lanes < 16 ? (uint64_t)lanes : 16);
            svuint16_t v = svld1_u16(pg, in);
            uint16_t out_l[16], out_r[16];
            svst1_u16(pg, out_l, svlsl_n_u16_x(pg, v, shift_amount));
            svst1_u16(pg, out_r, svlsr_n_u16_x(pg, v, shift_amount));
            left_shifted = out_l[0];
            right_shifted = out_r[0];
        }

        /* 独立标量软件参考 (原版 sw==hw 同义反复; SVE 版 sw 是独立语义,
         * 谓词/向量指令必须真正算对才能 pass —— 这是本转换的核心价值) */
        uint16_t sw_left  = static_cast<uint16_t>(src_mask_val << shift_amount);
        uint16_t sw_right = static_cast<uint16_t>(src_mask_val >> shift_amount);

        /* 存储一致性测试：将结果写入内存并重新加载 */
        uint16_t store_buf[2] = {left_shifted, right_shifted};
        uint16_t reload_buf[2];
        memcpy(reload_buf, store_buf, sizeof(store_buf));
        bool consistent = (reload_buf[0] == left_shifted) && (reload_buf[1] == right_shifted);

        /* 比较硬件（SVE 向量移位结果）与软件参考 */
        bool left_pass  = (sw_left == left_shifted);
        bool right_pass = (sw_right == right_shifted);
        bool passed = left_pass && right_pass && consistent;

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = passed ? "\033[32m" : "\033[31m";
        const char *result_str = passed ? "PASS" : "FAIL";

        fprintf(stderr, "kreg2_sve: Iter %lu, src=0x%04X, shift=%d\n",
                iteration, src_mask_val, shift_amount);
        fprintf(stderr, "  sw: left=0x%04X, right=0x%04X\n", sw_left, sw_right);
        fprintf(stderr, "  hw: left=0x%04X, right=0x%04X\n", left_shifted, right_shifted);
        fprintf(stderr, "  consistent=%d, result=%s%s\033[0m\n",
                consistent, color, result_str);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("kreg2_sve: mismatch in SVE shift or consistency");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int kreg2_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for kreg2_sve");
    return EXIT_SKIP;
}
#endif

static int kreg2_sve_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(kreg2_sve, "Mask register shift operations (KSHIFTL, KSHIFTR) on real SVE vector shift instructions")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg2_sve_init,
    .test_run = kreg2_sve_run,
    .test_cleanup = kreg2_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
