/*  NEON variant of tests/cpu/memory/memcpy_shuffle_v2_neon.cpp — auto-derived by the 2026-11-02
 *  cache/memory NEON pass: load-point neon_copy() calls replaced with the explicit
 *  vld1q/vst1q stream in arm_add_neon_copy.h (bypasses the libc IFUNC dispatcher,
 *  keeps the NEON datapath attribution).  Original test untouched; identifiers
 *  carry the _neon suffix to avoid ODR clashes with the original TU.
 */
#include <sandstone.h>
#include "arm_add_neon_copy.h"
using arm_add_neon::neon_copy;
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

static constexpr size_t BLOCK_SIZE = 4096;  // 4KB，包含多个缓存行
static constexpr size_t CACHE_LINE = 64;

/* ARM64 数据缓存行 clean+invalidate（CLFLUSH 等价），配合 dsb sy 冲刷。
 * 注意：__builtin___clear_cache 是 I-cache 维护原语（ic ivau+isb），不是
 * 数据缓存冲刷，不能用来制造本测试宣称的 dc-civac 干扰。 */
static inline void dc_civac(void *addr) {
    __asm__ volatile("dc civac, %0" :: "r"(addr) : "memory");
}
static inline void dsb_sy() {
    __asm__ volatile("dsb sy" ::: "memory");
}

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

        // ARM64 数据缓存行冲刷：dc civac + dsb sy（真正刷数据缓存）
        dc_civac(src.data() + src_off);
        dc_civac(dst.data() + dst_off);
        dsb_sy();

        neon_copy(dst.data(), src.data(), BLOCK_SIZE);

        bool data_ok = (memcmp(dst.data(), src.data(), BLOCK_SIZE) == 0);

        neon_copy(store_buf.data(), dst.data(), BLOCK_SIZE);
        bool consistent = (memcmp(store_buf.data(), dst.data(), BLOCK_SIZE) == 0);

        if (!(data_ok && consistent)) {
            report_fail_msg("memcpy_shuffle_v2_neon: data mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int memcpy_shuffle_v2_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(memcpy_shuffle_v2_neon, "Memory copy with dc-civac cache flush interference (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = memcpy_shuffle_v2_init,
    .test_run = memcpy_shuffle_v2_run,
    .test_cleanup = memcpy_shuffle_v2_finish,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
