#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#ifdef __aarch64__
#include <arm_neon.h>          // NEON intrinsics (aarch64 only)
#endif

#define DATA_SIZE 1024          // 数据大小（元素数）
#define CACHE_LINE 64

static int load_port_init(struct test *test) {
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int load_port_run(struct test *test, int cpu) {
    (void)cpu;
    /* randomization hardening (B3-008): framework RNG (per-thread stream,
     * -s reproducible) replaces std::mt19937. */
    auto dist = []() { return random32(); };  /* full [0, 2^32) */

    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        alignas(16) uint32_t src[DATA_SIZE];
        alignas(16) uint32_t dst[DATA_SIZE];
        for (int i = 0; i < DATA_SIZE; ++i) {
            src[i] = dist();
        }

        // 使用 NEON 加载指令（128 位）和标量加载混合，模拟多端口压力
        int i = 0;
        // NEON 128-bit 加载 (vld1q_u32) 每次 4 个元素
        for (; i + 4 <= DATA_SIZE; i += 4) {
            uint32x4_t v = vld1q_u32(&src[i]);
            vst1q_u32(&dst[i], v);
        }
        // 标量加载处理剩余
        for (; i < DATA_SIZE; ++i) {
            dst[i] = src[i];
        }

        bool data_ok = (memcmp(src, dst, sizeof(src)) == 0);

        alignas(16) uint32_t store_buf[DATA_SIZE];
        memcpy(store_buf, dst, sizeof(dst));
        bool consistent = (memcmp(store_buf, dst, sizeof(dst)) == 0);

        bool passed = data_ok && consistent;

        if (!passed) {
            fprintf(stderr, "load_port: src[0..7]=%08X %08X %08X %08X %08X %08X %08X %08X, data_ok=%d, consistent=%d, result=%sFAIL%s\n",
                    src[0], src[1], src[2], src[3], src[4], src[5], src[6], src[7],
                    data_ok, consistent, RED, RESET);
            fflush(stderr);
            report_fail_msg("load_port: Data mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int load_port_run(struct test *test, int cpu) {
    (void)cpu;
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for load_port");
    return EXIT_SKIP;
}
#endif

static int load_port_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(load_port, "Pressure on load ports")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = load_port_init,
    .test_run = load_port_run,
    .test_cleanup = load_port_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
