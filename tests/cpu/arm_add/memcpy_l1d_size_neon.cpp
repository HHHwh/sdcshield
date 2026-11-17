/*  NEON variant of tests/cpu/memory/memcpy_l1d_size_neon.cpp — auto-derived by the 2026-11-02
 *  cache/memory NEON pass: load-point neon_copy() calls replaced with the explicit
 *  vld1q/vst1q stream in arm_add_neon_copy.h (bypasses the libc IFUNC dispatcher,
 *  keeps the NEON datapath attribution).  Original test untouched; identifiers
 *  carry the _neon suffix to avoid ODR clashes with the original TU.
 */
#include <sandstone.h>
#include "arm_add_neon_copy.h"
using arm_add_neon::neon_copy;
#include "sandstone_p.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// 获取 L1d 缓存大小（字节）。探测顺序（randomization hardening H8'）：
//   1. 框架 device_info[].cache[]（topology.cpp 启动时从 sysfs 填好，
//      本机实测 64KB——零测试使用过的现成数据源）
//   2. sysconf(_SC_LEVEL1_DCACHE_SIZE)（本机 openEuler 24.03 返回 0）
//   3. 32KB 兜底
static size_t get_l1d_size() {
    int ndev = device_count();
    for (int i = 0; i < ndev; ++i) {
        int sz = device_info[i].cache[0].cache_data;   /* cache[0] = L1d */
        if (sz > 0)
            return static_cast<size_t>(sz);
    }
    long size = sysconf(_SC_LEVEL1_DCACHE_SIZE);
    if (size > 0) return static_cast<size_t>(size);
    return 32 * 1024;
}

static int memcpy_l1d_size_init(struct test *test) {
    /* 在 test_init 里探测（此刻框架 topology 已填充 device_info）——
     * 静态全局初始化发生在 main 之前，device_count() 尚为 0，探测会
     * 静默落回兜底值（H8' 实测踩过这个坑）。 */
    auto *block = new size_t(get_l1d_size());
    if (!block)
        return EXIT_FAILURE;
    test->data = block;
    return EXIT_SUCCESS;
}

static int memcpy_l1d_size_run(struct test *test, int cpu) {
    (void)cpu;
    const size_t BLOCK_SIZE = *static_cast<size_t *>(test->data);

    // 缓冲区取 2 倍 L1d：留出随机偏移滑窗空间
    const size_t window = BLOCK_SIZE * 2;
    std::vector<uint8_t> src(window);
    std::vector<uint8_t> dst(window);

    do {
        // 每迭代：随机 src/dst 偏移（64B 对齐）+ 随机块长（L1d 的 1..全尺寸，
        // 以 64B 为粒度）——每次复制触碰的缓存组/偏移都不同
        size_t len = 64 * (1 + random32() % (BLOCK_SIZE / 64));
        size_t src_off = 64 * (random32() % ((window - len) / 64 + 1));
        size_t dst_off = 64 * (random32() % ((window - len) / 64 + 1));

        memset_random(src.data() + src_off, len);
        memset(dst.data() + dst_off, 0x5A, len);   // 先污染目标，懒惰复制无法通过

        neon_copy(dst.data() + dst_off, src.data() + src_off, len);

        bool data_ok = (memcmp(dst.data() + dst_off, src.data() + src_off, len) == 0);

        if (!data_ok) {
            report_fail_msg("memcpy_l1d_size_neon: copy mismatch (len %zu, "
                            "src_off %zu, dst_off %zu, L1d block %zu)",
                            len, src_off, dst_off, BLOCK_SIZE);
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int memcpy_l1d_size_finish(struct test *test) {
    delete static_cast<size_t *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

DECLARE_TEST(memcpy_l1d_size_neon, "Memory copy with L1D-sized block (random data, random offsets)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = memcpy_l1d_size_init,
    .test_run = memcpy_l1d_size_run,
    .test_cleanup = memcpy_l1d_size_finish,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
