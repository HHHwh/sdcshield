#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#ifdef __aarch64__
#include <arm_neon.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/sysinfo.h>

#define TOTAL_ELEMENTS 1024               // 总元素数（小工作集，保证快速完成）
#define NEON_VECTOR_SIZE 4                // NEON 一次处理 4 个 int32
#define BLOCK_ELEMENTS 16                 // 每个块 16 个 int32（由 4 个 NEON 向量组成）
#define VECTORS_PER_BLOCK (BLOCK_ELEMENTS / NEON_VECTOR_SIZE)  // = 4
#define NUM_BLOCKS (TOTAL_ELEMENTS / BLOCK_ELEMENTS)           // = 64
#define TOTAL_BYTES (TOTAL_ELEMENTS * sizeof(int32_t))

struct TestData {
    alignas(16) int32_t *data;           // 共享数据数组（16 字节对齐）
    std::atomic<uint32_t> next_block;    // 下一个可分配的块索引
    std::atomic<uint64_t> global_sum;    // 所有写核心的校验和累加
    std::atomic<uint32_t> write_done;       // 已完成写入并提交校验和的块数
    std::atomic<uint32_t> thread_idx;    // 线程 ID 分配器
    std::atomic<uint32_t> reset_lock;    // 重置互斥锁
};

static int mesh_upi_avx512_asymm_write_only_int_init(struct test *test) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) {
        return -255;   // 需要至少 2 个线程
    }

    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;

    td->data = (int32_t*)aligned_alloc(16, TOTAL_BYTES);
    if (!td->data) {
        delete td;
        return EXIT_FAILURE;
    }
    memset(td->data, 0, TOTAL_BYTES);

    td->next_block.store(0, std::memory_order_relaxed);
    td->global_sum.store(0, std::memory_order_relaxed);
    td->write_done.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    td->reset_lock.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx512_asymm_write_only_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    bool is_reader = (id == 0);

    /* randomization hardening H14' (P17): framework RNG (per-thread
     * stream, -s reproducible) replaces std::mt19937; range [-1000000, 1000000). */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        if (!is_reader) {
            // 写核心：循环分配块直到所有块分配完
            while (true) {
                uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
                if (block >= NUM_BLOCKS) break;

                size_t offset = block * BLOCK_ELEMENTS;
                int32_t vals[BLOCK_ELEMENTS];
                uint64_t local_sum = 0;
                for (int j = 0; j < BLOCK_ELEMENTS; ++j) {
                    vals[j] = dist();
                    local_sum += (uint64_t)vals[j];
                }
                // 使用 4 个 NEON 向量存储 16 个元素
                for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                    int32x4_t vec = vld1q_s32(vals + v * NEON_VECTOR_SIZE);
                    vst1q_s32(td->data + offset + v * NEON_VECTOR_SIZE, vec);
                }
                __sync_synchronize();   // 确保写入对其他核心可见

                // 一致性测试：立即读回比较
                bool block_ok = true;
                for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                    int32x4_t written = vld1q_s32(vals + v * NEON_VECTOR_SIZE);
                    int32x4_t loaded = vld1q_s32(td->data + offset + v * NEON_VECTOR_SIZE);
                    uint32x4_t cmp = vceqq_s32(written, loaded);
                    // 检查所有元素相等
                    if (!(vgetq_lane_u32(cmp, 0) == 0xFFFFFFFF &&
                          vgetq_lane_u32(cmp, 1) == 0xFFFFFFFF &&
                          vgetq_lane_u32(cmp, 2) == 0xFFFFFFFF &&
                          vgetq_lane_u32(cmp, 3) == 0xFFFFFFFF)) {
                        block_ok = false;
                        break;
                    }
                }
                /* 写核心的立即读回是唯一逐字节的写入内容校验——读核心的
                 * sum 校验可被保和损坏（如元素交换）绕过，所以 block_ok
                 * 为假必须直接判失败，不能只记录。 */
                if (!block_ok) {
                    fprintf(stderr, "mesh_upi_avx512_asymm_write_only_int: Thread %d (writer), block %u immediate read-back mismatch, written vals[0..3]=(%d,%d,%d,%d), loaded data[0..3] at offset %zu=(%d,%d,%d,%d), result=%sFAIL%s\n",
                            id, block,
                            vals[0], vals[1], vals[2], vals[3],
                            offset,
                            td->data[offset], td->data[offset + 1],
                            td->data[offset + 2], td->data[offset + 3],
                            RED, RESET);
                    fflush(stderr);
                    report_fail_msg("mesh_upi_avx512_asymm_write_only_int: writer immediate read-back mismatch (store/load corruption)");
                    return EXIT_FAILURE;
                }

                td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
                td->write_done.fetch_add(1, std::memory_order_seq_cst);
            }
            // 等待读核心完成本轮读取并重置 next_block。若不等待，写核心会在
            // 读核心读取期间越界重入 next_block.fetch_add，使 32 位 next_block
            // 回绕后覆盖读核心正在读取的数据，造成 sum 校验假失败。
            while (td->next_block.load(std::memory_order_seq_cst) != 0 &&
                   test_time_condition(test)) {
                __asm__ volatile("yield");
            }
        }

        if (is_reader) {
            // 等待所有块分配完成
            while (td->write_done.load(std::memory_order_seq_cst) < NUM_BLOCKS &&
                   test_time_condition(test)) {
                __asm__ volatile("yield");
            }
            if (!test_time_condition(test)) break;

            // Sum snapshot BEFORE the read loop (see symm_f64/int): a reader
            // snapshotting global_sum after the walk races the reset-phase
            // writer restart and the next iteration's writes.
            uint64_t expected_sum = td->global_sum.load(std::memory_order_seq_cst);

            // 读取整个数组，计算校验和并进行一致性测试
            uint64_t read_sum = 0;
            bool consistent = true;
            alignas(16) int32_t store_buf[BLOCK_ELEMENTS];

            for (size_t i = 0; i < TOTAL_ELEMENTS; i += BLOCK_ELEMENTS) {
                // 加载 16 个元素
                int32_t vals[BLOCK_ELEMENTS];
                for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                    int32x4_t vec = vld1q_s32(td->data + i + v * NEON_VECTOR_SIZE);
                    vst1q_s32(vals + v * NEON_VECTOR_SIZE, vec);
                }
                for (int j = 0; j < BLOCK_ELEMENTS; ++j) {
                    read_sum += (uint64_t)vals[j];
                }

                // 一致性测试：Store/Load 比较
                for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                    int32x4_t orig = vld1q_s32(vals + v * NEON_VECTOR_SIZE);
                    vst1q_s32(store_buf + v * NEON_VECTOR_SIZE, orig);
                }
                for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                    int32x4_t orig = vld1q_s32(vals + v * NEON_VECTOR_SIZE);
                    int32x4_t reload = vld1q_s32(store_buf + v * NEON_VECTOR_SIZE);
                    uint32x4_t cmp = vceqq_s32(orig, reload);
                    if (!(vgetq_lane_u32(cmp, 0) == 0xFFFFFFFF &&
                          vgetq_lane_u32(cmp, 1) == 0xFFFFFFFF &&
                          vgetq_lane_u32(cmp, 2) == 0xFFFFFFFF &&
                          vgetq_lane_u32(cmp, 3) == 0xFFFFFFFF)) {
                        consistent = false;
                        break;
                    }
                }
            }

            bool sum_ok = (read_sum == expected_sum);
            bool passed = sum_ok && consistent;

            if (!passed) {
                // 首次失败证据（原先每轮都从读核心打印 PASS 行）
                fprintf(stderr, "mesh_upi_avx512_asymm_write_only_int: Thread %d (reader), data[0..15]=(%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, consistent=%d, result=%s%s%s\n",
                        id,
                        td->data[0], td->data[1], td->data[2], td->data[3],
                        td->data[4], td->data[5], td->data[6], td->data[7],
                        td->data[8], td->data[9], td->data[10], td->data[11],
                        td->data[12], td->data[13], td->data[14], td->data[15],
                        read_sum, expected_sum, consistent,
                        passed ? GREEN : RED,
                        passed ? "PASS" : "FAIL",
                        RESET);
                fflush(stderr);
                report_fail_msg("mesh_upi_avx512_asymm_write_only_int: Sum mismatch or consistency failure");
                return EXIT_FAILURE;
            }

            // 重置状态（仅由一个读核心执行）
            uint32_t expected_lock = 0;
            if (td->reset_lock.compare_exchange_strong(expected_lock, 1,
                                                       std::memory_order_seq_cst,
                                                       std::memory_order_seq_cst)) {
                // next_block 必须最后归零，作为写核心重新入场的释放栅栏；
                // 否则写核心会在 write_done 归零前重入，造成计数丢失。
                td->global_sum.store(0, std::memory_order_seq_cst);
                td->write_done.store(0, std::memory_order_seq_cst);
                td->next_block.store(0, std::memory_order_seq_cst);
                td->reset_lock.store(0, std::memory_order_seq_cst);
            } else {
                // 未获得锁（但此测试只有唯一读核心，不会进入）
                while (td->reset_lock.load(std::memory_order_seq_cst) != 0 &&
                       test_time_condition(test)) {
                    __asm__ volatile("yield");
                }
                if (!test_time_condition(test)) break;
            }
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_avx512_asymm_write_only_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx512_asymm_write_only_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_avx512_asymm_write_only_int");
    return EXIT_SKIP;
}
static int mesh_upi_avx512_asymm_write_only_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx512_asymm_write_only_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx512_asymm_write_only_int,
             "Mutual write stress MESH and UPI (do NEON stores to L1/L2/L3)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx512_asymm_write_only_int_init,
    .test_run = mesh_upi_avx512_asymm_write_only_int_run,
    .test_cleanup = mesh_upi_avx512_asymm_write_only_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
