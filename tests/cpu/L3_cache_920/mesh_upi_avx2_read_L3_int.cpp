#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#ifdef __aarch64__
#include <arm_neon.h>
#include <unistd.h>

#define ARRAY_SIZE (1 << 20)           // 1M 个 int32 = 4MB（略大于 L2，小于 L3）
#define VECTOR_SIZE 4                  // NEON 每次加载 4 个 int32（128 位）

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    alignas(16) int32_t *data;         // 对齐到 16 字节，便于 NEON 加载
    uint64_t golden_sum;               // 预先计算的参考累加和
    std::atomic<uint64_t> thread_idx;  // 用于线程 ID 分配
};
}

static int mesh_upi_avx2_read_L3_int_init(struct test *test) {
    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;

    // 分配对齐内存（16 字节对齐）
    td->data = (int32_t*)aligned_alloc(16, ARRAY_SIZE * sizeof(int32_t));
    if (!td->data) {
        delete td;
        return EXIT_FAILURE;
    }

    // 生成随机 int32 数据并计算参考累加和
    /* randomization hardening H14' (P17): framework RNG (per-thread
     * stream, -s reproducible) replaces std::mt19937; range [-1000000, 1000000). */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };
    uint64_t sum = 0;
    for (size_t i = 0; i < ARRAY_SIZE; ++i) {
        td->data[i] = dist();
        sum += (uint64_t)td->data[i];   // 无符号扩展，避免溢出
    }
    td->golden_sum = sum;
    td->thread_idx.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx2_read_L3_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    // 分配线程 ID（用于输出）
    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        uint64_t local_sum = 0;

        // 顺序读取整个数组，使用 NEON 加载（每次 4 个 int32）
        for (size_t i = 0; i < ARRAY_SIZE; i += VECTOR_SIZE) {
            int32x4_t v = vld1q_s32(td->data + i);
            // 将向量中 4 个 int32 累加到 local_sum（标量累加）
            int32_t vals[VECTOR_SIZE];
            vst1q_s32(vals, v);
            for (int j = 0; j < VECTOR_SIZE; ++j) {
                local_sum += (uint64_t)vals[j];
            }
        }

        bool passed = (local_sum == td->golden_sum);

        if (!passed) {
            // 首次失败证据（原先每个工作迭代都打印 PASS 行）
            fprintf(stderr, "mesh_upi_avx2_read_L3_int: Thread %d, data[0..3]=(%d,%d,%d,%d), local_sum=%lu, golden_sum=%lu, result=%s%s%s\n",
                    id,
                    td->data[0], td->data[1], td->data[2], td->data[3],
                    local_sum, td->golden_sum,
                    passed ? GREEN : RED,
                    passed ? "PASS" : "FAIL",
                    RESET);
            fflush(stderr);
            report_fail_msg("mesh_upi_avx2_read_L3_int: Sum mismatch");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_avx2_read_L3_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx2_read_L3_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_avx2_read_L3_int");
    return EXIT_SKIP;
}
static int mesh_upi_avx2_read_L3_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx2_read_L3_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx2_read_L3_int, "Doing 100% sequential int32 NEON reads (aim for 100% L3 hits)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx2_read_L3_int_init,
    .test_run = mesh_upi_avx2_read_L3_int_run,
    .test_cleanup = mesh_upi_avx2_read_L3_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
