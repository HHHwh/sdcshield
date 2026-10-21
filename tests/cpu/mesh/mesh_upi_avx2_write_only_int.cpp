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
        // 逐块生成随机数据、写入、立即读回验证
        bool passed = true;
        int32_t first_vals[VECTOR_SIZE]; // 用于输出本次的输入

        for (size_t i = 0; i < PER_THREAD_SIZE; i += VECTOR_SIZE) {
            // 生成 VECTOR_SIZE 个随机数
            int32_t vals[VECTOR_SIZE];
            for (int j = 0; j < VECTOR_SIZE; ++j) {
                vals[j] = dist();
                if (i == 0) first_vals[j] = vals[j]; // 保存前几个用于输出
            }

            // 使用 NEON 存储
            int32x4_t v = vld1q_s32(vals);
            vst1q_s32(buf + i, v);

            // 立即读回验证
            int32x4_t loaded = vld1q_s32(buf + i);
            int32_t loaded_vals[VECTOR_SIZE];
            vst1q_s32(loaded_vals, loaded);
            for (int j = 0; j < VECTOR_SIZE; ++j) {
                if (loaded_vals[j] != vals[j]) {
                    passed = false;
                    break;
                }
            }
            if (!passed) break;
        }

        if (!passed) {
            // 首次失败证据（原先每个工作迭代都打印 PASS 行）
            fprintf(stderr, "mesh_upi_avx2_write_only_int: Thread %d, input[0..3]=(%d,%d,%d,%d), result=%s%s%s\n",
                    id,
                    first_vals[0], first_vals[1], first_vals[2], first_vals[3],
                    passed ? GREEN : RED,
                    passed ? "PASS" : "FAIL",
                    RESET);
            fflush(stderr);
            report_fail_msg("mesh_upi_avx2_write_only_int: Data mismatch");
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
DECLARE_TEST(mesh_upi_avx2_write_only_int, "Mutual write stress MESH and UPI or Ring Interconnect and QPI (do int32 NEON stores to L1/L2/L3)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx2_write_only_int_init,
    .test_run = mesh_upi_avx2_write_only_int_run,
    .test_cleanup = mesh_upi_avx2_write_only_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
