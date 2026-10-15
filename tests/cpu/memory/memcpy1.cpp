#include <sandstone.h>          // 提供 DECLARE_TEST, report_fail_msg, test_time_condition 等
#include <cstring>              // memcpy, memcmp
#include <cstdint>              // uint8_t
#include <cstddef>              // size_t
#include <cstdio>               // snprintf, fprintf

static constexpr size_t BLOCK_SIZE = 256;

static int memcpy1_init(struct test *test) {
    (void)test;   // 消除未使用参数警告
    return EXIT_SUCCESS;
}

static int memcpy1_run(struct test *test, int cpu) {
    uint8_t src[BLOCK_SIZE];
    uint8_t dst[BLOCK_SIZE];
    /* randomization hardening (B3-007): framework RNG (per-thread stream,
     * -s reproducible) replaces std::mt19937; range [0, 255]. */
    auto dist = []() { return (uint8_t)((0) + (int64_t)(random64() % (uint64_t)((255) - (0) + 1))); };
    unsigned long iteration = 0;

    do {
        // 生成随机数据
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            src[i] = dist();
        }

        // 执行复制
        memcpy(dst, src, BLOCK_SIZE);

        // 校验
        bool passed = (memcmp(dst, src, BLOCK_SIZE) == 0);

        // 输出本轮输入（前8字节十六进制）和验证结果
        char hex_str[32];
        snprintf(hex_str, sizeof(hex_str), "%02x%02x%02x%02x%02x%02x%02x%02x",
                 src[0], src[1], src[2], src[3], src[4], src[5], src[6], src[7]);
        fprintf(stderr, "memcpy1: Iteration %lu, src first 8 bytes = %s, result = %s\n",
                iteration++, hex_str, passed ? "PASS" : "FAIL");

        if (!passed) {
            report_fail_msg("memcpy1: data mismatch after copy");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int memcpy1_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

// 使用框架已有的 group_math 组（避免自定义组的不完整类型问题）
DECLARE_TEST(memcpy1, "Copy small memory blocks with random data")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = memcpy1_init,
    .test_run = memcpy1_run,
    .test_cleanup = memcpy1_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
