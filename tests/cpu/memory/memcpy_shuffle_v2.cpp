#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static constexpr size_t BLOCK_SIZE = 4096;  // 4KB，包含多个缓存行
static constexpr size_t CACHE_LINE = 64;

static int memcpy_shuffle_v2_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int memcpy_shuffle_v2_run(struct test *test, int cpu) {
    (void)cpu;
    std::vector<uint8_t> src(BLOCK_SIZE);
    std::vector<uint8_t> dst(BLOCK_SIZE);
    std::vector<uint8_t> store_buf(BLOCK_SIZE);
    /* randomization hardening (B3-007): framework RNG (per-thread stream,
     * -s reproducible) replaces std::mt19937; bytes [0, 255], offsets
     * [0, BLOCK_SIZE - CACHE_LINE]. */
    auto byte_dist = []() { return (uint8_t)((0) + (int64_t)(random64() % (uint64_t)((255) - (0) + 1))); };
    auto offset_dist = []() { return (size_t)((0) + (int64_t)(random64() % (uint64_t)((BLOCK_SIZE - CACHE_LINE) - (0) + 1))); };

    do {
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            src[i] = byte_dist();
        }
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            dst[i] = byte_dist();
        }

        size_t src_off = offset_dist();
        size_t dst_off = offset_dist();

        // ARM64 兼容的缓存行冲刷（使用 __builtin___clear_cache）
        __builtin___clear_cache((char*)src.data() + src_off,
                                (char*)src.data() + src_off + CACHE_LINE);
        __builtin___clear_cache((char*)dst.data() + dst_off,
                                (char*)dst.data() + dst_off + CACHE_LINE);
        // 内存屏障，确保冲刷完成
        __sync_synchronize();

        memcpy(dst.data(), src.data(), BLOCK_SIZE);

        bool data_ok = (memcmp(dst.data(), src.data(), BLOCK_SIZE) == 0);

        memcpy(store_buf.data(), dst.data(), BLOCK_SIZE);
        bool consistent = (memcmp(store_buf.data(), dst.data(), BLOCK_SIZE) == 0);

        if (!(data_ok && consistent)) {
            report_fail_msg("memcpy_shuffle_v2: data mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int memcpy_shuffle_v2_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(memcpy_shuffle_v2, "Memory copy with cache flush interference (using __clear_cache)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = memcpy_shuffle_v2_init,
    .test_run = memcpy_shuffle_v2_run,
    .test_cleanup = memcpy_shuffle_v2_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
