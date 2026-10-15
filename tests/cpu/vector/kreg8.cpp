#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>

#ifdef __aarch64__
#include <arm_neon.h>

static int kreg8_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int kreg8_run(struct test *test, int cpu) {
    (void)cpu;
    auto type_dist = []() { return (int)((0) + (int64_t)(random64() % (uint64_t)((3) - (0) + 1))); };
    static std::atomic<uint64_t> iter{0};

    static const uint8_t  iota8_init[8]  = {0, 1, 2, 3, 4, 5, 6, 7};
    static const uint16_t iota16_init[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    static const uint32_t iota32_init[4] = {0, 1, 2, 3};
    static const uint64_t iota64_init[2] = {0, 1};

    do {
        int type = type_dist();
        bool passed = false;
        char type_name[16] = "UNKNOWN";

        switch (type) {
            case 0: { // VPMOVM2B (8-bit, 64 elements)
                /* H9'/P12: framework RNG */
                uint64_t mask_val = random64();

                /* 硬件: 每 lane 用 vshl_u8 提取掩码对应 bit (组 q 取掩码
                 * 的第 q 个字节), vsub (0 - bit) 展开 1 -> 0xFF,
                 * 8 组共 64 lane (原版 sw_vals[i] 与 hw_vals[i] 同值双写,
                 * 结构上恒 pass —— B1-004)。 */
                uint8_t hw_vals[64] __attribute__((aligned(16)));
                {
                    const uint8x8_t iota8 = vld1_u8(iota8_init);
                    const int8x8_t  nidx8 = vneg_s8(vreinterpret_s8_u8(iota8));
                    for (int q = 0; q < 8; ++q) {
                        uint8x8_t bit = vand_u8(
                            vshl_u8(vdup_n_u8(static_cast<uint8_t>(mask_val >> (q * 8))), nidx8),
                            vdup_n_u8(1));
                        vst1_u8(&hw_vals[q * 8], vsub_u8(vdup_n_u8(0), bit));
                    }
                }

                /* 软件参考: 独立标量展开。 */
                uint8_t sw_vals[64];
                for (int i = 0; i < 64; ++i)
                    sw_vals[i] = (mask_val & (1ULL << i)) ? 0xFF : 0x00;

                passed = (memcmp(hw_vals, sw_vals, sizeof(hw_vals)) == 0);
                strcpy(type_name, "VPMOVM2B");

                if (!passed) {
                    fprintf(stderr, "kreg8: Iter %lu, type=%s, mask=0x%016lX\n",
                            iter.load(), type_name, mask_val);
                    fprintf(stderr, "  sw[0..3]=%02X %02X %02X %02X  hw[0..3]=%02X %02X %02X %02X\n",
                            sw_vals[0], sw_vals[1], sw_vals[2], sw_vals[3],
                            hw_vals[0], hw_vals[1], hw_vals[2], hw_vals[3]);
                }
                break;
            }
            case 1: { // VPMOVM2W (16-bit, 32 elements)
                /* H9'/P12: framework RNG */
                uint32_t mask_val = random32();

                uint16_t hw_vals[32] __attribute__((aligned(16)));
                {
                    const uint16x8_t iota16 = vld1q_u16(iota16_init);
                    const int16x8_t  nidx16 = vnegq_s16(vreinterpretq_s16_u16(iota16));
                    for (int q = 0; q < 4; ++q) {
                        uint16x8_t bit = vandq_u16(
                            vshlq_u16(vdupq_n_u16(static_cast<uint16_t>(mask_val >> (q * 8))), nidx16),
                            vdupq_n_u16(1));
                        vst1q_u16(&hw_vals[q * 8], vsubq_u16(vdupq_n_u16(0), bit));
                    }
                }

                uint16_t sw_vals[32];
                for (int i = 0; i < 32; ++i)
                    sw_vals[i] = (mask_val & (1U << i)) ? 0xFFFF : 0x0000;

                passed = (memcmp(hw_vals, sw_vals, sizeof(hw_vals)) == 0);
                strcpy(type_name, "VPMOVM2W");

                if (!passed) {
                    fprintf(stderr, "kreg8: Iter %lu, type=%s, mask=0x%08X\n",
                            iter.load(), type_name, mask_val);
                    fprintf(stderr, "  sw[0..3]=%04X %04X %04X %04X  hw[0..3]=%04X %04X %04X %04X\n",
                            sw_vals[0], sw_vals[1], sw_vals[2], sw_vals[3],
                            hw_vals[0], hw_vals[1], hw_vals[2], hw_vals[3]);
                }
                break;
            }
            case 2: { // VPMOVM2D (32-bit, 16 elements)
                /* H9'/P12: framework RNG */
                uint16_t mask_val = (uint16_t)(random32() & 0xFFFF);

                uint32_t hw_vals[16] __attribute__((aligned(16)));
                {
                    const uint32x4_t iota32 = vld1q_u32(iota32_init);
                    for (int q = 0; q < 4; ++q) {
                        uint32x4_t idx  = vaddq_u32(vdupq_n_u32(q * 4), iota32);
                        int32x4_t  nidx = vnegq_s32(vreinterpretq_s32_u32(idx));
                        uint32x4_t bit  = vandq_u32(vshlq_u32(vdupq_n_u32(mask_val), nidx),
                                                    vdupq_n_u32(1));
                        vst1q_u32(&hw_vals[q * 4], vsubq_u32(vdupq_n_u32(0), bit));
                    }
                }

                uint32_t sw_vals[16];
                for (int i = 0; i < 16; ++i)
                    sw_vals[i] = (mask_val & (1U << i)) ? 0xFFFFFFFFu : 0u;

                passed = (memcmp(hw_vals, sw_vals, sizeof(hw_vals)) == 0);
                strcpy(type_name, "VPMOVM2D");

                if (!passed) {
                    fprintf(stderr, "kreg8: Iter %lu, type=%s, mask=0x%04X\n",
                            iter.load(), type_name, mask_val);
                    fprintf(stderr, "  sw[0..3]=%08X %08X %08X %08X  hw[0..3]=%08X %08X %08X %08X\n",
                            sw_vals[0], sw_vals[1], sw_vals[2], sw_vals[3],
                            hw_vals[0], hw_vals[1], hw_vals[2], hw_vals[3]);
                }
                break;
            }
            case 3: { // VPMOVM2Q (64-bit, 8 elements)
                uint8_t mask_val = (uint8_t)(random32() & 0xFF);

                uint64_t hw_vals[8] __attribute__((aligned(16)));
                {
                    const uint64x2_t iota64 = vld1q_u64(iota64_init);
                    for (int q = 0; q < 4; ++q) {
                        uint64x2_t idx  = vaddq_u64(vdupq_n_u64(q * 2), iota64);
                        int64x2_t  nidx = vnegq_s64(vreinterpretq_s64_u64(idx));
                        uint64x2_t bit  = vandq_u64(vshlq_u64(vdupq_n_u64(mask_val), nidx),
                                                    vdupq_n_u64(1));
                        vst1q_u64(&hw_vals[q * 2], vsubq_u64(vdupq_n_u64(0), bit));
                    }
                }

                uint64_t sw_vals[8];
                for (int i = 0; i < 8; ++i)
                    sw_vals[i] = (mask_val & (1U << i)) ? 0xFFFFFFFFFFFFFFFFULL : 0ULL;

                passed = (memcmp(hw_vals, sw_vals, sizeof(hw_vals)) == 0);
                strcpy(type_name, "VPMOVM2Q");

                if (!passed) {
                    fprintf(stderr, "kreg8: Iter %lu, type=%s, mask=0x%02X\n",
                            iter.load(), type_name, mask_val);
                    fprintf(stderr, "  sw[0..1]=%016lX %016lX  hw[0..1]=%016lX %016lX\n",
                            sw_vals[0], sw_vals[1], hw_vals[0], hw_vals[1]);
                }
                break;
            }
        }

        if (!passed) {
            const char *color = passed ? "\033[32m" : "\033[31m";
            const char *result_str = passed ? "PASS" : "FAIL";
            fprintf(stderr, "result=%s%s\033[0m\n", color, result_str);
            fflush(stderr);
            report_fail_msg("kreg8: mismatch in NEON mask-to-vector conversion");
            return EXIT_FAILURE;
        }

        iter.fetch_add(1, std::memory_order_relaxed);

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int kreg8_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#else // !__aarch64__

// On non-AArch64 (e.g. x86-64) there is no <arm_neon.h>; the test self-skips
// in test_init so the result is marked ignored rather than spuriously
// passing (kreg1.cpp precedent).
static int kreg8_init(struct test *test) {
    (void)test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON required for "
             "mask-to-vector conversion test (vshlq/vsubq expansion)");
    return EXIT_SKIP;
}

static int kreg8_run(struct test *test, int cpu) {
    (void)test;
    (void)cpu;
    __builtin_unreachable();    // init reports EXIT_SKIP, run never executes
}

static int kreg8_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(kreg8, "Mask to vector conversion on real NEON (per-lane bit extract + expand)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg8_init,
    .test_run = kreg8_run,
    .test_cleanup = kreg8_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
