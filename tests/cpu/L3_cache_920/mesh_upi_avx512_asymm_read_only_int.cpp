#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#ifdef __aarch64__
#include <arm_neon.h>
#include <unistd.h>

#define ARRAY_ELEMS (1 << 20)           // 1M 个 int32 = 4MB（大于 L2，小于典型 L3）
#define NEON_VECTOR_SIZE 4              // NEON 一次处理 4 个 int32
#define VECTOR_SIZE 16                  // 逻辑块大小 16（由 4 个 NEON 向量组成）
#define VECTORS_PER_BLOCK (VECTOR_SIZE / NEON_VECTOR_SIZE)  // = 4
#define TOTAL_BYTES (ARRAY_ELEMS * sizeof(int32_t))

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    alignas(16) int32_t *data;          // 共享只读数据，16 字节对齐
    uint64_t golden_sum;                // 预先计算的参考累加和
    std::atomic<uint32_t> thread_idx;   // 用于线程 ID 分配
};
}

static int mesh_upi_avx512_asymm_read_only_int_init(struct test *test) {
    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;

    td->data = (int32_t*)aligned_alloc(16, TOTAL_BYTES);
    if (!td->data) {
        delete td;
        return EXIT_FAILURE;
    }

    /* randomization hardening H14' (P17): framework RNG (per-thread
     * stream, -s reproducible) replaces std::mt19937; range [-1000000, 1000000). */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };
    uint64_t sum = 0;
    for (size_t i = 0; i < ARRAY_ELEMS; ++i) {
        td->data[i] = dist();
        sum += (uint64_t)td->data[i];
    }
    td->golden_sum = sum;
    td->thread_idx.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx512_asymm_read_only_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);

    // 每个线程独立的临时缓冲区，用于一致性测试（Store/Load）
    alignas(16) int32_t store_buf[VECTOR_SIZE];

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        uint64_t local_sum = 0;
        bool consistent = true;

        // 顺序读取整个数组，每次处理 16 个 int32（4 个 NEON 向量）
        for (size_t i = 0; i < ARRAY_ELEMS; i += VECTOR_SIZE) {
            // 加载 16 个元素到临时数组
            int32_t vals[VECTOR_SIZE];
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                int32x4_t vec = vld1q_s32(td->data + i + v * NEON_VECTOR_SIZE);
                vst1q_s32(vals + v * NEON_VECTOR_SIZE, vec);
            }
            // 累加
            for (int j = 0; j < VECTOR_SIZE; ++j) {
                local_sum += (uint64_t)vals[j];
            }

            // ----- 一致性测试：Store/Load 比较 -----
            // 将 16 个元素存储到临时缓冲区
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                int32x4_t orig = vld1q_s32(vals + v * NEON_VECTOR_SIZE);
                vst1q_s32(store_buf + v * NEON_VECTOR_SIZE, orig);
            }
            // 重新加载并比较
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                int32x4_t orig = vld1q_s32(vals + v * NEON_VECTOR_SIZE);
                int32x4_t reload = vld1q_s32(store_buf + v * NEON_VECTOR_SIZE);
                uint32x4_t cmp = vceqq_s32(orig, reload);
                // 检查所有 4 个元素是否相等
                if (!(vgetq_lane_u32(cmp, 0) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 1) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 2) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 3) == 0xFFFFFFFF)) {
                    consistent = false;
                    break;
                }
            }
            if (!consistent) break;
        }

        bool sum_ok = (local_sum == td->golden_sum);
        bool passed = sum_ok && consistent;

        if (!passed) {
            // 首次失败证据（原先每个工作迭代都从线程 0 打印 PASS 行）
            fprintf(stderr, "mesh_upi_avx512_asymm_read_only_int: Thread %d, data[0..15]=(%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d), local_sum=%lu, golden_sum=%lu, consistent=%d, result=%s%s%s\n",
                    id,
                    td->data[0], td->data[1], td->data[2], td->data[3],
                    td->data[4], td->data[5], td->data[6], td->data[7],
                    td->data[8], td->data[9], td->data[10], td->data[11],
                    td->data[12], td->data[13], td->data[14], td->data[15],
                    local_sum, td->golden_sum, consistent,
                    passed ? GREEN : RED,
                    passed ? "PASS" : "FAIL",
                    RESET);
            fflush(stderr);
            report_fail_msg("mesh_upi_avx512_asymm_read_only_int: Sum mismatch or consistency failure");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_avx512_asymm_read_only_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx512_asymm_read_only_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_avx512_asymm_read_only_int");
    return EXIT_SKIP;
}
static int mesh_upi_avx512_asymm_read_only_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx512_asymm_read_only_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx512_asymm_read_only_int,
             "Mutual read stress MESH and UPI (do NEON loads from L1/L2/L3)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx512_asymm_read_only_int_init,
    .test_run = mesh_upi_avx512_asymm_read_only_int_run,
    .test_cleanup = mesh_upi_avx512_asymm_read_only_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
