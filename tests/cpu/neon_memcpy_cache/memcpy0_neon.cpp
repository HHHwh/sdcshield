/*  NEON variant of tests/cpu/memory/memcpy0_neon.cpp — auto-derived by the 2026-11-02
 *  cache/memory NEON pass: load-point neon_copy() calls replaced with the explicit
 *  vld1q/vst1q stream in arm_add_neon_copy.h (bypasses the libc IFUNC dispatcher,
 *  keeps the NEON datapath attribution).  Original test untouched; identifiers
 *  carry the _neon suffix to avoid ODR clashes with the original TU.
 */
#include <sandstone.h>
#include "arm_add_neon_copy.h"
using arm_add_neon::neon_copy;
#include <cstdint>
#include <cstring>

// 测试块大小：选择典型值如 256 字节，也可调整
static constexpr size_t BLOCK_SIZE = 256;

static int memcpy0_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int memcpy0_run(struct test *test, int cpu) {
    (void)cpu;

    // 分配源和目标缓冲区（对齐可选，但 memcpy 不要求对齐）
    uint8_t *src = static_cast<uint8_t*>(malloc(BLOCK_SIZE));
    uint8_t *dst = static_cast<uint8_t*>(malloc(BLOCK_SIZE));
    if (!src || !dst) {
        free(src);
        free(dst);
        return EXIT_FAILURE;
    }

    do {
        // 每迭代重掷源数据（框架 RNG，按线程独立流）：原实现整场只复制
        // 同一份全 0 数据，值空间 = 1，任何位翻转检测力为零附近
        memset_random(src, BLOCK_SIZE);
        memset(dst, 0xFF, BLOCK_SIZE);

        // 执行内存复制
        neon_copy(dst, src, BLOCK_SIZE);

        // 验证数据是否完全一致
        bool data_ok = (memcmp(dst, src, BLOCK_SIZE) == 0);

        if (!data_ok) {
            report_fail_msg("memcpy0_neon: data mismatch after copy (src[0..7]="
                            "%02X %02X %02X %02X %02X %02X %02X %02X, dst[0..7]="
                            "%02X %02X %02X %02X %02X %02X %02X %02X)",
                            src[0], src[1], src[2], src[3],
                            src[4], src[5], src[6], src[7],
                            dst[0], dst[1], dst[2], dst[3],
                            dst[4], dst[5], dst[6], dst[7]);
            free(src);
            free(dst);
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    free(src);
    free(dst);
    return EXIT_SUCCESS;
}

static int memcpy0_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(memcpy0_neon, "Basic memory copy (all-zero data)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = memcpy0_init,
    .test_run = memcpy0_run,
    .test_cleanup = memcpy0_finish,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
