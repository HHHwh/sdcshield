#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>

#ifdef __aarch64__
#include <arm_neon.h>

static int kreg9_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int kreg9_run(struct test *test, int cpu) {
    (void)cpu;
    auto type_dist = []() { return (int)((0) + (int64_t)(random64() % (uint64_t)((3) - (0) + 1))); };
    static std::atomic<uint64_t> iter{0};

    do {
        int type = type_dist();
        bool passed = false;
        char type_name[16] = "UNKNOWN";

        switch (type) {
            case 0: { // KMOVB (8-bit)
                uint8_t orig = static_cast<uint8_t>(random32() & 0xFF);
                /* 硬件: vdupq 广播 -> vst1q/vld1q 真实内存往返 (掩码
                 * 寄存器 <-> GPR 通路), vmaxvq/vminvq 横向归约校验全部
                 * 16 lane 都等于 orig (max == min == orig; 原版 gpr =
                 * orig 后自比, 结构上恒 pass —— B1-004)。 */
                uint8_t buf[16] __attribute__((aligned(16)));
                uint8x16_t stored = vdupq_n_u8(orig);
                vst1q_u8(buf, stored);
                /* 编译器屏障（CodeRabbit 意见）：buf 是不逃逸的局部数组，
                 * 单独的 asm volatile "" ::: "memory" 只被当作改写 "asm 可
                 * 能访问到的内存"，未逃逸局部对象不在其中 —— GCC/Clang 仍
                 * 可把 vld1q(buf) 转发为 vdupq 结果，st1/ld1 甚至归约全被
                 * 删除（-O3 反汇编已证实）。改用两次 asm 屏障：
                 *  - "r"(buf)：buf 地址作为 asm 输入操作数逃逸到编译器不
                 *    可见代码，vst1q 不能删，后续 vld1q(buf) 也不能被证
                 *    明读回 stored（阻断 store-to-load forwarding）；
                 *  - "+w"(loaded)：加载结果作为 asm 的读改写操作数，阻断
                 *    asm 前后对 loaded 的常量传播，归约必须真实发射。
                 * 先例：vector/insert_extract.cpp 的 store/load 屏障。 */
                uint8x16_t loaded = stored;          /* 占位，asm 写入真实值 */
                __asm__ volatile("" : "+w"(loaded) : "r"(buf) : "memory");
                loaded = vld1q_u8(buf);              /* asm 后的真实加载 */
                __asm__ volatile("" : "+w"(loaded) : "r"(buf) : "memory");
                passed = (vmaxvq_u8(loaded) == orig) && (vminvq_u8(loaded) == orig);
                strcpy(type_name, "KMOVB");
                if (!passed) {
                    fprintf(stderr, "kreg9: Iter %lu, type=%s, orig=0x%02X, "
                            "lanes(min/max)=0x%02X/0x%02X\n",
                            iter.load(), type_name, orig, vminvq_u8(loaded), vmaxvq_u8(loaded));
                }
                break;
            }
            case 1: { // KMOVW (16-bit)
                uint16_t orig = static_cast<uint16_t>(random32() & 0xFFFF);
                uint16_t buf[8] __attribute__((aligned(16)));
                uint16x8_t stored = vdupq_n_u16(orig);
                vst1q_u16(buf, stored);
                /* buf 逃逸 + "+w" 两次 asm 屏障（同 KMOVB case） */
                uint16x8_t loaded = stored;
                __asm__ volatile("" : "+w"(loaded) : "r"(buf) : "memory");
                loaded = vld1q_u16(buf);
                __asm__ volatile("" : "+w"(loaded) : "r"(buf) : "memory");
                passed = (vmaxvq_u16(loaded) == orig) && (vminvq_u16(loaded) == orig);
                strcpy(type_name, "KMOVW");
                if (!passed) {
                    fprintf(stderr, "kreg9: Iter %lu, type=%s, orig=0x%04X, "
                            "lanes(min/max)=0x%04X/0x%04X\n",
                            iter.load(), type_name, orig, vminvq_u16(loaded), vmaxvq_u16(loaded));
                }
                break;
            }
            case 2: { // KMOVD (32-bit)
                uint32_t orig = random32();
                uint32_t buf[4] __attribute__((aligned(16)));
                uint32x4_t stored = vdupq_n_u32(orig);
                vst1q_u32(buf, stored);
                /* buf 逃逸 + "+w" 两次 asm 屏障（同 KMOVB case） */
                uint32x4_t loaded = stored;
                __asm__ volatile("" : "+w"(loaded) : "r"(buf) : "memory");
                loaded = vld1q_u32(buf);
                __asm__ volatile("" : "+w"(loaded) : "r"(buf) : "memory");
                passed = (vmaxvq_u32(loaded) == orig) && (vminvq_u32(loaded) == orig);
                strcpy(type_name, "KMOVD");
                if (!passed) {
                    fprintf(stderr, "kreg9: Iter %lu, type=%s, orig=0x%08X, "
                            "lanes(min/max)=0x%08X/0x%08X\n",
                            iter.load(), type_name, orig, vminvq_u32(loaded), vmaxvq_u32(loaded));
                }
                break;
            }
            case 3: { // KMOVQ (64-bit)
                uint64_t orig = ((uint64_t)random32() << 32) | random32();
                uint64_t buf[2] __attribute__((aligned(16)));
                uint64x2_t stored = vdupq_n_u64(orig);
                vst1q_u64(buf, stored);
                /* buf 逃逸 + "+w" 两次 asm 屏障（同 KMOVB case） */
                uint64x2_t loaded = stored;
                __asm__ volatile("" : "+w"(loaded) : "r"(buf) : "memory");
                loaded = vld1q_u64(buf);
                __asm__ volatile("" : "+w"(loaded) : "r"(buf) : "memory");
                /* u64 无横向 max/min 归约指令: vceqq 等于比较后以
                 * u32 位视图归约 (全 lane 相等 -> 全 0xFFFFFFFF ->
                 * vminvq != 0)。 */
                uint32x4_t eq = vreinterpretq_u32_u64(vceqq_u64(loaded, vdupq_n_u64(orig)));
                passed = (vminvq_u32(eq) != 0);
                strcpy(type_name, "KMOVQ");
                if (!passed) {
                    fprintf(stderr, "kreg9: Iter %lu, type=%s, orig=0x%016lX, "
                            "lane0=0x%016lX lane1=0x%016lX\n",
                            iter.load(), type_name, orig,
                            vgetq_lane_u64(loaded, 0), vgetq_lane_u64(loaded, 1));
                }
                break;
            }
        }

        if (!passed) {
            const char *color = passed ? "\033[32m" : "\033[31m";
            const char *result_str = passed ? "PASS" : "FAIL";
            fprintf(stderr, "result=%s%s\033[0m\n", color, result_str);
            fflush(stderr);
            report_fail_msg("kreg9: mismatch in NEON broadcast/roundtrip");
            return EXIT_FAILURE;
        }

        iter.fetch_add(1, std::memory_order_relaxed);

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int kreg9_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#else // !__aarch64__

// On non-AArch64 (e.g. x86-64) there is no <arm_neon.h>; the test self-skips
// in test_init so the result is marked ignored rather than spuriously
// passing (kreg1.cpp precedent).
static int kreg9_init(struct test *test) {
    (void)test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM NEON required for "
             "mask to/from GPR move test (broadcast + roundtrip)");
    return EXIT_SKIP;
}

static int kreg9_run(struct test *test, int cpu) {
    (void)test;
    (void)cpu;
    __builtin_unreachable();    // init reports EXIT_SKIP, run never executes
}

static int kreg9_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(kreg9, "Mask to/from GPR move on real NEON (broadcast, memory roundtrip, cross-lane reduction)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = kreg9_init,
    .test_run = kreg9_run,
    .test_cleanup = kreg9_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
