#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#ifdef __aarch64__
#include <arm_neon.h>
#include <unistd.h>

#define PER_THREAD_SIZE (1 << 20)      // 每个线程 1M 个 int32 = 4MB
#define VECTOR_SIZE 4                  // NEON 每次存储 4 个 int32（128 位）

// L3 几何注记（2026-11-17 L3 审计）：4MB/线程私有窗。鲲鹏 920 分布式
// L3 = 24MB/24 核簇——簇内并发 ≤6 线程时窗驻留 L3；全簇 24 线程时聚合
// 96MB ≫ 24MB 切片，写以流式+驱逐为主（L3 压力在、容量驻留不在）。
// 仓库单测试独占跑法处于驻留区。
// 2026-11-17 能力改造（用户指令）：两遍式——写满+golden sum → 延迟
// 全量重读复算比对。重读时首块早已被 4MB 流量逐出 512KB 私有 L2，
// 校验点真穿存储层级（L3/DRAM 供数），不再依赖立即回读转发。

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    std::atomic<uint64_t> thread_idx;
};
}

static int mesh_upi_avx2_write_only_int_init(struct test *test) {
    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;
    td->thread_idx.store(0, std::memory_order_relaxed);
    test->data = td;
    return EXIT_SUCCESS;
}

static int mesh_upi_avx2_write_only_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);

    // 每个线程独立分配自己的缓冲区（对齐到 16 字节，满足 NEON 要求）
    alignas(16) int32_t *buf = (int32_t*)aligned_alloc(16, PER_THREAD_SIZE * sizeof(int32_t));
    if (!buf) {
        report_fail_msg("Failed to allocate buffer");
        return EXIT_FAILURE;
    }

    /* randomization hardening H14' (P17): framework RNG (per-thread
     * stream, -s reproducible) replaces std::mt19937; range [-1000000, 1000000). */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        /* Pass 1 — write fill: fresh randoms through NEON stores,
         * accumulating the golden sum.  (2026-11-17 L3-capability
         * rework, user directive: the old immediate per-block
         * read-back was served by store-buffer/L1 forwarding and
         * never saw L2/L3 — the audit finding.) */
        int32_t first_vals[VECTOR_SIZE];
        uint64_t golden_sum = 0;
        for (size_t i = 0; i < PER_THREAD_SIZE; i += VECTOR_SIZE) {
            int32_t vals[VECTOR_SIZE];
            for (int j = 0; j < VECTOR_SIZE; ++j) {
                vals[j] = dist();
                if (i == 0) first_vals[j] = vals[j];
            }
            int32x4_t v = vld1q_s32(vals);
            vst1q_s32(buf + i, v);
            for (int j = 0; j < VECTOR_SIZE; ++j)
                golden_sum += (uint64_t)vals[j];
        }

        /* Pass 2 — delayed full re-read: the 4 MB buffer was just
         * streamed, so early lines are long evicted from the 512 KB
         * private L2 — this re-read is L3/DRAM-served and the sum
         * compare is a genuine hierarchy verification point (the
         * read family's proven pattern).  L3-resident flip -> sum
         * mismatch; blind spot: compensating double errors (same as
         * read family, documented). */
        uint64_t reread_sum = 0;
        for (size_t i = 0; i < PER_THREAD_SIZE; i += VECTOR_SIZE) {
            int32x4_t loaded = vld1q_s32(buf + i);
            int32_t loaded_vals[VECTOR_SIZE];
            vst1q_s32(loaded_vals, loaded);
            for (int j = 0; j < VECTOR_SIZE; ++j)
                reread_sum += (uint64_t)loaded_vals[j];
        }

        if (reread_sum != golden_sum) {
            fprintf(stderr, "mesh_upi_avx2_write_only_int: Thread %d, input[0..3]=(%d,%d,%d,%d), golden_sum=%lu, reread_sum=%lu, result=%sFAIL%s\n",
                    id,
                    first_vals[0], first_vals[1], first_vals[2], first_vals[3],
                    (unsigned long)golden_sum, (unsigned long)reread_sum,
                    RED, RESET);
            fflush(stderr);
            report_fail_msg("mesh_upi_avx2_write_only_int: write->delayed-re-read sum mismatch (L3 write path)");
            free(buf);
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    free(buf);
    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_avx2_write_only_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx2_write_only_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_avx2_write_only_int");
    return EXIT_SKIP;
}
static int mesh_upi_avx2_write_only_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx2_write_only_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx2_write_only_int, "Mutual write stress MESH and UPI or Ring Interconnect and QPI (do int32 NEON stores to L1/L2/L3; write-fill then delayed full re-read with sum/golden verify — genuine L3-path verification, 2026-11-17 rework)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx2_write_only_int_init,
    .test_run = mesh_upi_avx2_write_only_int_run,
    .test_cleanup = mesh_upi_avx2_write_only_int_finish,
    .fracture_loop_count = -1,   /* never fracture: rework rounds carry 8MB traffic, run the full -t */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
