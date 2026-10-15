#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>

#ifdef __aarch64__
#include <arm_neon.h>

static int kreg6_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int kreg6_run(struct test *test, int cpu) {
    (void)cpu;
    auto mask16_dist = []() { return (uint16_t)((0) + (int64_t)(random64() % (uint64_t)((0xFFFF) - (0) + 1))); };
    static std::atomic<uint64_t> iter{0};

    static const uint32_t iota_init[4] = {0, 1, 2, 3};

    do {
        uint16_t mask_val = mask16_dist();

        /* 硬件: NEON 掩码展开 —— 每 lane 用 vshlq_u32 变量移位提取掩码
         * 的对应 bit (lane 索引取负 -> 逻辑右移), 再用 vsubq (0 - bit)
         * 展开为全 1/全 0 (x86 VPMOVM2D 风格), 4 个 128-bit 段共 16 lane
         * (原版为纯标量循环且 sw_vals[i] 直接复制 hw_vals[i], 结构上恒
         * pass —— B1-004)。 */
        uint32_t hw_vals[16] __attribute__((aligned(16)));
        {
            const uint32x4_t iota = vld1q_u32(iota_init);
            for (int q = 0; q < 4; ++q) {
                uint32x4_t idx  = vaddq_u32(vdupq_n_u32(q * 4), iota);
                int32x4_t  nidx = vnegq_s32(vreinterpretq_s32_u32(idx));
                uint32x4_t bit  = vandq_u32(vshlq_u32(vdupq_n_u32(mask_val), nidx),
                                            vdupq_n_u32(1));
                vst1q_u32(&hw_vals[q * 4], vsubq_u32(vdupq_n_u32(0), bit));
            }
        }

        /* 软件参考: 独立标量展开。 */
        uint32_t sw_vals[16];
        for (int i = 0; i < 16; ++i)
            sw_vals[i] = (mask_val & (1u << i)) ? 0xFFFFFFFFu : 0u;

        /* 交叉验证: 硬件向量展开 vs 标量 golden 全 16 lane 比对。 */
        bool passed = (memcmp(hw_vals, sw_vals, sizeof(hw_vals)) == 0);

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);

        if (!passed) {
            const char *color = passed ? "\033[32m" : "\033[31m";
            const char *result_str = passed ? "PASS" : "FAIL";
            fprintf(stderr, "kreg6: Iter %lu, mask=0x%04X\n", iteration, mask_val);
            fprintf(stderr, "  sw: ");
            for (int i = 0; i < 16; ++i) fprintf(stderr, "%08X ", sw_vals[i]);
            fprintf(stderr, "\n  hw: ");
            for (int i = 0; i < 16; ++i) fprintf(stderr, "%08X ", hw_vals[i]);
            fprintf(stderr, "\n  result=%s%s\033[0m\n", color, result_str);
            fflush(stderr);
            report_fail_msg("kreg6: mismatch in NEON mask broadcast");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int kreg6_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#else // !__aarch64__

// On non-AArch64 (e.g. x86-64) there is no <arm_neon.h>; the test self-skips
// in test_init so the result is marked ignored rather than spuriously
// passing (kreg1.cpp precedent).
static int kreg6_init(struct test *test) {
    (void)test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON required for "
             "mask broadcast test (vshlq bit extract + vsubq expansion)");
    return EXIT_SKIP;
}

static int kreg6_run(struct test *test, int cpu) {
    (void)test;
    (void)cpu;
    __builtin_unreachable();    // init reports EXIT_SKIP, run never executes
}

static int kreg6_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(kreg6, "Mask broadcast to vector on real NEON (per-lane bit extract + expand)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg6_init,
    .test_run = kreg6_run,
    .test_cleanup = kreg6_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
