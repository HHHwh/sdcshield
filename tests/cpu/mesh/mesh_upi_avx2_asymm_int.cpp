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

#define TOTAL_ELEMENTS 1024
#define VECTOR_SIZE 4                     // NEON 一次处理 4 个 int32
#define BLOCK_ELEMENTS (VECTOR_SIZE * 2)  // 保持每个块 8 个元素（两个向量）
#define NUM_BLOCKS (TOTAL_ELEMENTS / BLOCK_ELEMENTS)  // = 128
#define TOTAL_BYTES (TOTAL_ELEMENTS * sizeof(int32_t))

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    alignas(16) int32_t *data;           // 16 字节对齐即可
    std::atomic<uint32_t> next_block;
    std::atomic<uint64_t> global_sum;
    std::atomic<uint32_t> allocated_blocks;
    std::atomic<uint32_t> thread_idx;
    /* sense-reversal barrier (G2'/G3' fix): the original iter/round_done
     * protocol deadlocked from round 1 (the writer waited for
     * iter != current_iter, but iter only advanced after a completed
     * round), so the readers' verification never executed and the test
     * passed vacuously. Pre-existing since the 098d4aa port. */
    std::atomic<uint32_t> arrived;
    std::atomic<uint32_t> epoch;
};
}

static int mesh_upi_avx2_asymm_int_init(struct test *test) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) return -255;

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
    td->allocated_blocks.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    td->arrived.store(0, std::memory_order_relaxed);
    td->epoch.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx2_asymm_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    // 分配唯一线程 ID（用于每轮输出与 asymm 写者轮转）
    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);

    /* 线程总数取框架权威值 thread_count()（自动尊重 --cpuset、
     * test.max_threads 与 OS 亲和性限制，每个线程取值一致）。
     * 原运行时“3 次稳定读数”启发式是 098d4aa 移植引入的缺陷：大规模
     * 并发下严重欠计（191 线程实测检出 2，甚至读出垃圾值），屏障参与
     * 者数目随之错误 → 轮次隔离崩溃 → 误报 sum mismatch；此前的死锁
     * 屏障恰好掩盖了它。另注：该启发式在 -n 1 下会永久自旋挂死，
     * thread_count() 则给出确定性结果。 */
    uint32_t total_threads = thread_count();
    if (total_threads < 2) {
        log_skip(CpuTopologyIssueSkipCategory,
                 "mesh_upi_avx2_asymm_int requires at least 2 threads (inter-core test); "
                 "skipping on this thread count");
        return EXIT_SKIP;
    }

    /* randomization hardening H14' (P17): framework RNG (per-thread
     * stream, -s reproducible) replaces std::mt19937; range [-1000000, 1000000). */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    uint32_t my_epoch = 0;
    do {
        int writer_id = my_epoch % total_threads;

        if (id == writer_id) {
            // ---------- 写者：写入所有块 ----------
            // （本轮状态已由上一轮 sense-reversal 屏障的最后到达线程重置；
            //   第一轮的状态由 init 重置）
            for (uint32_t block = 0; block < NUM_BLOCKS; ++block) {
                size_t offset = block * BLOCK_ELEMENTS;
                int32_t vals[BLOCK_ELEMENTS];
                uint64_t local_sum = 0;
                for (int j = 0; j < BLOCK_ELEMENTS; ++j) {
                    vals[j] = dist();
                    local_sum += (uint64_t)vals[j];
                }
                // 分两个 NEON 向量存储
                int32x4_t v0 = vld1q_s32(vals);
                int32x4_t v1 = vld1q_s32(vals + VECTOR_SIZE);
                vst1q_s32(td->data + offset, v0);
                vst1q_s32(td->data + offset + VECTOR_SIZE, v1);
                __sync_synchronize();  // 类似 _mm_sfence

                // 一致性测试（自身写入后读回）
                int32x4_t loaded0 = vld1q_s32(td->data + offset);
                int32x4_t loaded1 = vld1q_s32(td->data + offset + VECTOR_SIZE);
                [[maybe_unused]] uint32x4_t cmp0 = vceqq_s32(v0, loaded0);
                [[maybe_unused]] uint32x4_t cmp1 = vceqq_s32(v1, loaded1);
                // 检查所有元素是否相等（忽略，因为后续读者会校验）

                td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
                td->allocated_blocks.fetch_add(1, std::memory_order_seq_cst);
            }

            // 全局屏障确保所有数据已传播到所有核心
            __sync_synchronize();  // 类似 _mm_mfence

        } else {
            // ---------- 读者：等待所有块分配完成 ----------
            /* test_time_condition() 每次调用都会消耗 auto-fracture 的
             * inner_loop_count 预算（默认 40）：写者串行填充 NUM_BLOCKS
             * 需要数十微秒，若每次自旋都检查预算，读者会在写者完成前
             * 耗尽 40 次预算而提前 break（校验又被跳过）。每 1024 次
             * 自旋检查一次 —— inner_loop_count 语义是“轮次”而非
             * “自旋”，这是框架认可的用法（其余 mesh 文件同理）。 */
            uint32_t spin = 0;
            while (td->allocated_blocks.load(std::memory_order_seq_cst) < NUM_BLOCKS) {
                if ((++spin & 1023u) == 0) {
                    if (!test_time_condition(test)) goto out_of_budget;
                } else {
                    __asm__ volatile("yield");
                }
            }

            __sync_synchronize();  // 确保写者的所有存储可见

            // Sum snapshot BEFORE the read loop (TOCTOU fix): a fast thread
            // can finish its read, run the reset phase and start writing the
            // NEXT iteration while this thread is still walking the buffer.
            uint64_t expected_sum = td->global_sum.load(std::memory_order_seq_cst);
            uint64_t read_sum = 0;
            bool consistent = true;
            alignas(16) int32_t store_buf[BLOCK_ELEMENTS];

            for (size_t i = 0; i < TOTAL_ELEMENTS; i += BLOCK_ELEMENTS) {
                int32x4_t v0 = vld1q_s32(td->data + i);
                int32x4_t v1 = vld1q_s32(td->data + i + VECTOR_SIZE);
                int32_t vals[BLOCK_ELEMENTS];
                vst1q_s32(vals, v0);
                vst1q_s32(vals + VECTOR_SIZE, v1);
                for (int j = 0; j < BLOCK_ELEMENTS; ++j) read_sum += (uint64_t)vals[j];

                // 一致性测试：Store/Load 回读比较
                vst1q_s32(store_buf, v0);
                vst1q_s32(store_buf + VECTOR_SIZE, v1);
                int32x4_t reload0 = vld1q_s32(store_buf);
                int32x4_t reload1 = vld1q_s32(store_buf + VECTOR_SIZE);
                uint32x4_t cmp0 = vceqq_s32(v0, reload0);
                uint32x4_t cmp1 = vceqq_s32(v1, reload1);
                // 检查所有 8 个元素是否相等
                bool ok0 = (vgetq_lane_u32(cmp0, 0) == 0xFFFFFFFF &&
                            vgetq_lane_u32(cmp0, 1) == 0xFFFFFFFF &&
                            vgetq_lane_u32(cmp0, 2) == 0xFFFFFFFF &&
                            vgetq_lane_u32(cmp0, 3) == 0xFFFFFFFF);
                bool ok1 = (vgetq_lane_u32(cmp1, 0) == 0xFFFFFFFF &&
                            vgetq_lane_u32(cmp1, 1) == 0xFFFFFFFF &&
                            vgetq_lane_u32(cmp1, 2) == 0xFFFFFFFF &&
                            vgetq_lane_u32(cmp1, 3) == 0xFFFFFFFF);
                if (!(ok0 && ok1)) consistent = false;
            }

            bool passed = (read_sum == expected_sum) && consistent;

            // 输出结果（每个读者输出一次）
            fprintf(stderr, "mesh_upi_avx2_asymm_int: Thread %d (reader), data[0..7]=(%d,%d,%d,%d,%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, consistent=%d, result=%s%s%s\n",
                    id,
                    td->data[0], td->data[1], td->data[2], td->data[3],
                    td->data[4], td->data[5], td->data[6], td->data[7],
                    read_sum, expected_sum, consistent,
                    passed ? GREEN : RED,
                    passed ? "PASS" : "FAIL",
                    RESET);
            fflush(stderr);

            if (!passed) {
                report_fail_msg("mesh_upi_avx2_asymm_int: Sum mismatch or consistency failure");
                return EXIT_FAILURE;
            }

            // ---------- 读者到达 sense-reversal 屏障，等待写者与全部读者 ----------
        }

        // ---------- 阶段 3：sense-reversal 屏障进入下一轮（写者也到达） ----------
        // 最后到达的线程在此重置下一轮状态（global_sum/next_block/
        // allocated_blocks）：fetch_add(acq_rel) 的到达顺序保证所有线程
        // （写者 + 全部读者）都已离开本轮流量的生产/消费，epoch 的
        // release 存储保证重置先于任何线程的下一轮访问可见 —— 这正是
        // 原协议想做而没做对的轮次隔离。writer 随 my_epoch 轮转。
        // （自旋同样按 1024 次一档节流预算检查，理由同读者的等待。）
        {
            uint32_t done = td->arrived.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (done == total_threads) {
                td->global_sum.store(0, std::memory_order_seq_cst);
                td->next_block.store(0, std::memory_order_seq_cst);
                td->allocated_blocks.store(0, std::memory_order_seq_cst);
                td->arrived.store(0, std::memory_order_relaxed);
                td->epoch.store(my_epoch + 1, std::memory_order_release);
            } else {
                uint32_t spin = 0;
                while (td->epoch.load(std::memory_order_acquire) == my_epoch) {
                    if ((++spin & 1023u) == 0) {
                        if (!test_time_condition(test)) goto out_of_budget;
                    } else {
                        __asm__ volatile("yield");
                    }
                }
            }
        }
        ++my_epoch;
    } while (test_time_condition(test));

out_of_budget:
    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_avx2_asymm_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx2_asymm_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_avx2_asymm_int");
    return EXIT_SKIP;
}
static int mesh_upi_avx2_asymm_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx2_asymm_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx2_asymm_int,
             "Asymmetrical(1 write core to all rest mutual read cores) stress MESH and UPI or Ring Interconnect and QPI or FSB (do int32 AVX-2(YMM) loads and stores from L1/L2 to L1/L2 with switching write core) [ARM NEON version]")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx2_asymm_int_init,
    .test_run = mesh_upi_avx2_asymm_int_run,
    .test_cleanup = mesh_upi_avx2_asymm_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
