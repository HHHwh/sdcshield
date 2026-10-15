#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <isa-l/crc.h>

extern "C" {
    uint32_t crc32_gzip_refl(uint32_t init, const unsigned char *buf, uint64_t len);
}

static constexpr size_t BLOCK_SIZE = 1024;

// 软件 CRC32 参考实现（GZIP 标准，反射位序）
static uint32_t crc32_gzip_software(const uint8_t *buf, size_t len) {
    // CRC-32 参数：多项式 0xEDB88320，初始值 0xFFFFFFFF，最终异或 0xFFFFFFFF，输入/输出反射
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= buf[i];
        for (int j = 0; j < 8; ++j) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return ~crc; // 最终异或
}

static int isal_crc32_gzip_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int isal_crc32_gzip_run(struct test *test, int cpu) {
    (void)cpu;
    std::vector<uint8_t> local_data(BLOCK_SIZE);
    /* randomization hardening (B3-010): framework RNG (per-thread stream,
     * -s reproducible) replaces std::mt19937; bytes [0, 255]. */
    auto byte_dist = []() { return (uint8_t)((0) + (int64_t)(random64() % (uint64_t)((255) - (0) + 1))); };

    do {
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            local_data[i] = byte_dist();
        }

        // 软件计算黄金 CRC 值
        uint32_t crc_ref = crc32_gzip_software(local_data.data(), BLOCK_SIZE);

        // 硬件计算：使用 ISA-L 的 crc32_gzip_refl 函数
        uint32_t crc_hw = crc32_gzip_refl(0, local_data.data(), BLOCK_SIZE);

        // 内存屏障（ARM64 兼容）
        __sync_synchronize();

        bool data_ok = (crc_hw == crc_ref);

        // 一致性测试：存储 CRC 值到内存再加载比较
        uint32_t store_buf = crc_hw;
        uint32_t reload_buf;
        memcpy(&reload_buf, &store_buf, sizeof(store_buf));
        bool consistent = (reload_buf == crc_hw);

        bool passed = data_ok && consistent;

        if (!passed) {
            report_fail_msg("isal_crc32_gzip: CRC mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int isal_crc32_gzip_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(isal_crc32_gzip, "ISA-L CRC32 GZIP (random data)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = isal_crc32_gzip_init,
    .test_run = isal_crc32_gzip_run,
    .test_cleanup = isal_crc32_gzip_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
