#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#ifdef __aarch64__
#include <arm_neon.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/sysinfo.h>

#define NEON_VECTOR_SIZE 4                // NEON 一次处理 4 个 int32
#define BLOCK_SIZE 16                     // 逻辑块大小（由 4 个 NEON 向量组成）
#define VECTORS_PER_BLOCK (BLOCK_SIZE / NEON_VECTOR_SIZE)  // = 4
#define NUM_BLOCKS 64                     // 块数量
#define TOTAL_ELEMENTS (BLOCK_SIZE * NUM_BLOCKS)  // = 1024
#define TOTAL_BYTES (TOTAL_ELEMENTS * sizeof(int32_t))

struct TestData {
    alignas(16) int32_t *data;          // 16 字节对齐即可
    std::atomic<uint32_t> next_block;
    std::atomic<uint64_t> global_sum;
    std::atomic<uint32_t> write_done;
    std::atomic<uint32_t> read_done;
    std::atomic<uint32_t> reset_lock;
    std::atomic<uint32_t> thread_idx;
    uint64_t expected_block_sums[NUM_BLOCKS];   // 写侧逐块真值和（仅失败取证用）
};

static int mesh_upi_avx512_symm_int_init(struct test *test) {
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
    memset(td->expected_block_sums, 0, sizeof(td->expected_block_sums));

    td->next_block.store(0, std::memory_order_relaxed);
    td->global_sum.store(0, std::memory_order_relaxed);
    td->write_done.store(0, std::memory_order_relaxed);
    td->read_done.store(0, std::memory_order_relaxed);
    td->reset_lock.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx512_symm_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    uint32_t total = thread_count();

    /* randomization hardening H14' (P17): framework RNG (per-thread
     * stream, -s reproducible) replaces std::mt19937; range [-1000000, 1000000). */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        // ---------- 写阶段：分配块并写入 ----------
        while (true) {
            uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
            if (block >= NUM_BLOCKS) break;

            size_t offset = block * BLOCK_SIZE;
            int32_t vals[BLOCK_SIZE];
            uint64_t local_sum = 0;
            for (int j = 0; j < BLOCK_SIZE; ++j) {
                vals[j] = dist();
                local_sum += (uint64_t)vals[j];
            }
            // 使用 4 个 NEON 向量存储 16 个元素
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                int32x4_t vec = vld1q_s32(vals + v * NEON_VECTOR_SIZE);
                vst1q_s32(td->data + offset + v * NEON_VECTOR_SIZE, vec);
            }
            __sync_synchronize();   // 类似 _mm_sfence

            td->expected_block_sums[block] = local_sum;   // 写侧块真值（读者在屏障后读，序由 write_done RMW 保证）
            td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
            td->write_done.fetch_add(1, std::memory_order_seq_cst);
        }

        // 等待所有块写完成
        while (td->write_done.load(std::memory_order_seq_cst) < NUM_BLOCKS &&
               test_time_condition(test)) {
            __asm__ volatile("yield");
        }
        if (!test_time_condition(test)) break;

        // Sum snapshot BEFORE the read loop: a fast thread can finish its
        // read, run the reset phase and start the NEXT iteration's writes
        // while this thread is still walking the buffer. Comparing against a
        // snapshot taken after the read would race the reset/next-write and
        // produce a false mismatch (expected_sum cleared to 0 or partially
        // overwritten by the next iteration).
        uint64_t expected_sum = td->global_sum.load(std::memory_order_seq_cst);

        // ---------- 读阶段：读取整个数组并校验 ----------
        uint64_t read_sum = 0;
        bool consistent = true;
        uint64_t block_sums[NUM_BLOCKS];          // 逐块读和（仅失败取证用）
        size_t bad_block = 0; int bad_vec = -1;   // 一致性首失配位置（仅取证）
        int32_t bad_orig[NEON_VECTOR_SIZE], bad_reload[NEON_VECTOR_SIZE];
        alignas(16) int32_t store_buf[BLOCK_SIZE];   // 用于一致性测试

        for (size_t i = 0; i < TOTAL_ELEMENTS; i += BLOCK_SIZE) {
            // 加载 16 个元素
            int32_t vals[BLOCK_SIZE];
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                int32x4_t vec = vld1q_s32(td->data + i + v * NEON_VECTOR_SIZE);
                vst1q_s32(vals + v * NEON_VECTOR_SIZE, vec);
            }
            uint64_t block_sum = 0;
            for (int j = 0; j < BLOCK_SIZE; ++j) {
                read_sum += (uint64_t)vals[j];
                block_sum += (uint64_t)vals[j];
            }
            block_sums[i / BLOCK_SIZE] = block_sum;

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
                    bad_block = i / BLOCK_SIZE; bad_vec = v;
                    vst1q_s32(bad_orig, orig);
                    vst1q_s32(bad_reload, reload);
                    break;
                }
            }
            if (!consistent) break;
        }

        bool sum_ok = (read_sum == expected_sum);
        bool passed = sum_ok && consistent;

        if (!passed) {
            // 首次失败证据（原先每个工作迭代都从线程 0 打印 PASS 行）
            fprintf(stderr, "mesh_upi_avx512_symm_int: Thread %d, data[0..15]=(%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, consistent=%d, result=%s%s%s\n",
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

            // ---- 失败取证增强（不改变判定语义，仅增证据；2026-10-16 CPU27 事件
            //      只剩聚合和、位级不可恢复——本段补齐块级定位与瞬态性自证）----
            if (!sum_ok && consistent) {
                // 纯求和失配（CPU27 事件形态）。二次全量读：本线程未 read_done++，
                // reset 门控未过 → 数组在两读之间必然冻结。
                uint64_t read_sum2 = 0;
                uint64_t block_sums2[NUM_BLOCKS];
                for (size_t i = 0; i < TOTAL_ELEMENTS; i += BLOCK_SIZE) {
                    int32_t v2[BLOCK_SIZE];
                    for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                        int32x4_t vec = vld1q_s32(td->data + i + v * NEON_VECTOR_SIZE);
                        vst1q_s32(v2 + v * NEON_VECTOR_SIZE, vec);
                    }
                    uint64_t bs2 = 0;
                    for (int j = 0; j < BLOCK_SIZE; ++j) bs2 += (uint64_t)v2[j];
                    block_sums2[i / BLOCK_SIZE] = bs2;
                    read_sum2 += bs2;
                }
                const char *verdict =
                    (read_sum2 == expected_sum)
                        ? "TRANSIENT-DATAPATH (array exonerated by re-read; fault in this thread's load/ALU path)"
                        : "ARRAY-SIDE (re-read still mismatches; suspect stored data/memory)";
                fprintf(stderr, "mesh_upi_avx512_symm_int: Thread %d re-read_sum=%lu (first=%lu, expected=%lu) -> %s\n",
                        id, read_sum2, read_sum, expected_sum, verdict);
                for (int b = 0; b < NUM_BLOCKS; ++b) {
                    if (block_sums[b] != block_sums2[b] ||
                        block_sums2[b] != td->expected_block_sums[b]) {
                        const int32_t *p = td->data + b * BLOCK_SIZE;
                        fprintf(stderr, "mesh_upi_avx512_symm_int: Thread %d block %d: first-read=%lu re-read=%lu write-side=%lu delta(first-re-read)=%ld, block re-read values=(%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d)\n",
                                id, b, block_sums[b], block_sums2[b],
                                td->expected_block_sums[b],
                                (int64_t)(block_sums[b] - block_sums2[b]),
                                p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],
                                p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
                        break;
                    }
                }
            }
            if (!consistent && bad_vec >= 0) {
                // 一致性失配：lane 级原始值 vs 重载值（位级直接可见）
                fprintf(stderr, "mesh_upi_avx512_symm_int: Thread %d consistency first-mismatch block %zu vec %d: orig=(%d,%d,%d,%d) reload=(%d,%d,%d,%d)\n",
                        id, bad_block, bad_vec,
                        bad_orig[0], bad_orig[1], bad_orig[2], bad_orig[3],
                        bad_reload[0], bad_reload[1], bad_reload[2], bad_reload[3]);
            }
            fflush(stderr);
            report_fail_msg("mesh_upi_avx512_symm_int: Sum mismatch or consistency failure");
            return EXIT_FAILURE;
        }

        // 注册读完成
        td->read_done.fetch_add(1, std::memory_order_seq_cst);

        // ---------- 重置阶段（由最后一个读完成的线程执行） ----------
        uint32_t expected_lock = 0;
        if (td->reset_lock.compare_exchange_strong(expected_lock, 1,
                                                   std::memory_order_seq_cst,
                                                   std::memory_order_seq_cst)) {
            // 等待所有线程完成读取
            while (td->read_done.load(std::memory_order_seq_cst) < total &&
                   test_time_condition(test)) {
                __asm__ volatile("yield");
            }
            if (!test_time_condition(test)) {
                td->reset_lock.store(0, std::memory_order_seq_cst);
                break;
            }

            // 重置共享状态
            td->global_sum.store(0, std::memory_order_seq_cst);
            td->next_block.store(0, std::memory_order_seq_cst);
            td->write_done.store(0, std::memory_order_seq_cst);
            td->read_done.store(0, std::memory_order_seq_cst);
            td->reset_lock.store(0, std::memory_order_seq_cst);
        } else {
            // 未获得锁，等待重置完成（即 reset_lock 变为 0）
            while (td->reset_lock.load(std::memory_order_seq_cst) != 0 &&
                   test_time_condition(test)) {
                __asm__ volatile("yield");
            }
            if (!test_time_condition(test)) break;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_avx512_symm_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx512_symm_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_avx512_symm_int");
    return EXIT_SKIP;
}
static int mesh_upi_avx512_symm_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx512_symm_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx512_symm_int,
             "Symmetrical stress MESH and UPI (do int32 NEON loads and stores from L1/L2 to L1/L2 with different directions and switching cores)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx512_symm_int_init,
    .test_run = mesh_upi_avx512_symm_int_run,
    .test_cleanup = mesh_upi_avx512_symm_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
