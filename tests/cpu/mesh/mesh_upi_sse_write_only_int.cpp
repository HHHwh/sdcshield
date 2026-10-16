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

#define TOTAL_ELEMENTS 1024               // 总元素数（小工作集，保证快速完成）
#define VECTOR_SIZE 4                     // NEON 每次处理 4 个 int32
#define BLOCK_ELEMENTS VECTOR_SIZE
#define NUM_BLOCKS (TOTAL_ELEMENTS / BLOCK_ELEMENTS)  // = 256
#define TOTAL_BYTES (TOTAL_ELEMENTS * sizeof(int32_t))

struct TestData {
    alignas(16) int32_t *data;           // 共享数据数组，16 字节对齐
    std::atomic<uint32_t> next_block;    // 下一个可分配的块索引
    std::atomic<uint64_t> global_sum;    // 所有写核心的校验和累加
    std::atomic<uint32_t> allocated_blocks; // 已分配的块数
    std::atomic<uint32_t> thread_idx;    // 线程 ID 分配器
    /* sense-reversal 轮屏障（2731971 已验证模式）：原 reset-winner 协议
     * 由唯一读者经 reset_lock CAS 单方清零每轮计数器，存在两个缺陷：
     * (1) 复位序把 next_block 排在 allocated_blocks 之前清零——写者以
     *     next_block==0 为重新入场判据，观察到复位即可认领并提交
     *     allocated_blocks.fetch_add；若读者恰在两条 store 之间被抢占
     *     （192 核常规抢占），该增量被随后的 store(0) 覆盖 → 本轮
     *     allocated 永不达 NUM_BLOCKS → 读者等到超时，该轮校验丢失。
     *     （姊妹文件 avx2_asymm_write_int 把 next_block 排最后自愈了
     *     此窗口，本文件没有。）
     * (2) 读者等待与写者收尾自旋每次迭代都调 test_time_condition：
     *     读者是最先入场的 id 0，须桥接写者线程毫秒级孵化偏斜，40 次
     *     预算在 µs 级烧光 → 等待提前中断 → 校验被跳过 → vacuous pass
     *     （8502e491 在同族实测 0 行读端验证输出）。
     * CAS 的 else 分支在唯一读者下是不可达死代码。屏障最后到达者复位
     * ——无每迭代清零窗口、无胜者选举、写者不再自旋等复位。 */
    std::atomic<uint32_t> arrived;       // 本轮已到达屏障的线程数（写者+读者）
    std::atomic<uint32_t> epoch;         // 轮次代（sense 位）
};

static int mesh_upi_sse_write_only_int_init(struct test *test) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) {
        return -255;   // SKIP
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
    td->allocated_blocks.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    td->arrived.store(0, std::memory_order_relaxed);
    td->epoch.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_sse_write_only_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    bool is_reader = (id == 0);

    /* 屏障参与者总数取框架权威值 thread_count()（自动尊重 --cpuset、
     * test.max_threads 与 OS 亲和性限制，每个线程取值一致；2731971
     * 修法）。原文件无此检查：-n 1 下唯一线程是读者，allocated_blocks
     * 永不达 NUM_BLOCKS → 自旋到超时 → vacuous pass；现按家族先例
     * 干净跳过（556d54e3）。 */
    uint32_t total_threads = thread_count();
    if (total_threads < 2) {
        log_skip(CpuTopologyIssueSkipCategory,
                 "mesh_upi_sse_write_only_int requires at least 2 threads (inter-core test); "
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
        if (!is_reader) {
            /* 写核心：本轮竞争分配块直到所有块分配完（有限工作量，
             * 不逐块消耗 fracture 预算）。认领语义不变：fetch_add 竞争，
             * 认到 >= NUM_BLOCKS 即本轮退出；每块写入与原实现逐句相同。
             * 原实现认领后还自旋等待读者把 next_block 清零（reset-winner
             * 协议的收尾等待）——由下方轮屏障取代。 */
            while (true) {
                uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
                if (block >= NUM_BLOCKS) break;

                size_t offset = block * BLOCK_ELEMENTS;
                int32_t vals[VECTOR_SIZE];
                uint64_t local_sum = 0;
                for (int j = 0; j < VECTOR_SIZE; ++j) {
                    vals[j] = dist();
                    local_sum += (uint64_t)vals[j];
                }
                // NEON 加载/存储
                int32x4_t v = vld1q_s32(vals);
                vst1q_s32(td->data + offset, v);
                __sync_synchronize();   // 类似 _mm_sfence

                // 一致性测试：立即读回比较
                int32x4_t loaded = vld1q_s32(td->data + offset);
                uint32x4_t cmp = vceqq_s32(v, loaded);
                // 检查所有 4 个元素是否相等
                if (!(vgetq_lane_u32(cmp, 0) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 1) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 2) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 3) == 0xFFFFFFFF)) {
                    // 发现不一致，但继续处理（最终会在读核心校验时失败）
                }

                td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
                td->allocated_blocks.fetch_add(1, std::memory_order_seq_cst);
            }
        } else {
            /* 读核心：等待所有块分配完成。等待条件每自旋检查、ttc 预算
             * 按档节流检查、预算耗尽走 out_of_budget 干净退出。档距
             * 131072（8502e491 实测参数）：读者是最先入场的 id 0，须
             * 桥接写者线程 2.7~6.7ms 的孵化偏斜，1024 一档下 40 次预算
             * 仅覆盖 ~70µs（原实现每次自旋都调 ttc，正是 vacuous 根因
             * 之一，见 TestData 注释）。 */
            uint32_t spin = 0;
            while (td->allocated_blocks.load(std::memory_order_seq_cst) < NUM_BLOCKS) {
                if ((++spin & 131071u) == 0) {
                    if (!test_time_condition(test)) goto out_of_budget;
                } else {
                    __asm__ volatile("yield");
                }
            }

            // 读取整个数组，计算校验和并进行一致性测试
            // Sum snapshot BEFORE the read loop (TOCTOU fix): a fast thread
            // can finish its read, run the reset phase and start writing the
            // NEXT iteration while this thread is still walking the buffer.
            uint64_t expected_sum = td->global_sum.load(std::memory_order_seq_cst);
            uint64_t read_sum = 0;
            bool consistent = true;
            alignas(16) int32_t store_buf[VECTOR_SIZE];

            for (size_t i = 0; i < TOTAL_ELEMENTS; i += VECTOR_SIZE) {
                int32x4_t v = vld1q_s32(td->data + i);
                int32_t vals[VECTOR_SIZE];
                vst1q_s32(vals, v);
                for (int j = 0; j < VECTOR_SIZE; ++j) {
                    read_sum += (uint64_t)vals[j];
                }

                // 一致性测试：Store/Load 比较
                vst1q_s32(store_buf, v);
                int32x4_t reload = vld1q_s32(store_buf);
                uint32x4_t cmp = vceqq_s32(v, reload);
                if (!(vgetq_lane_u32(cmp, 0) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 1) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 2) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 3) == 0xFFFFFFFF)) {
                    consistent = false;
                }
            }

            bool sum_ok = (read_sum == expected_sum);
            bool passed = sum_ok && consistent;

            if (!passed) {
                // 首次失败证据（原先每轮都从读核心打印 PASS 行）
                fprintf(stderr, "mesh_upi_sse_write_only_int: Thread %d (reader), data[0..3]=(%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, consistent=%d, result=%s%s%s\n",
                        id,
                        td->data[0], td->data[1], td->data[2], td->data[3],
                        read_sum, expected_sum, consistent,
                        passed ? GREEN : RED,
                        passed ? "PASS" : "FAIL",
                        RESET);
                fflush(stderr);
                report_fail_msg("mesh_upi_sse_write_only_int: Sum mismatch or consistency failure");
                return EXIT_FAILURE;
            }

            /* 原重置段（reset_lock CAS 选举 + 逐条清零 + 不可达 else
             * 分支）由下方轮屏障的最后到达者复位取代。 */
        }

        /* ---------- 轮屏障：sense-reversal，读者与全体写者都到达 ----------
         * 最后到达者在此复位下一轮状态（global_sum/next_block/
         * allocated_blocks）后翻转 epoch：fetch_add(acq_rel) 的到达链
         * 保证复位发生时所有线程都已离开本轮的填充/校验（写者不再需要
         * 自旋等 next_block==0），epoch 的 release 存储保证复位先于任何
         * 线程的下一轮访问可见 —— 这正是原协议想做而没做对的轮次隔离。
         * 复位在屏障内进行，写者不可能观察到半复位状态，计数覆盖窗口
         * 结构性消除。自旋按 131072 一档节流预算检查（档距依据同读者
         * 等待：首轮早到者须等最晚孵化者）。 */
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
                    if ((++spin & 131071u) == 0) {
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

static int mesh_upi_sse_write_only_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_sse_write_only_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_sse_write_only_int");
    return EXIT_SKIP;
}
static int mesh_upi_sse_write_only_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_sse_write_only_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_sse_write_only_int,
             "Mutual write stress MESH and UPI or Ring Interconnect and QPI or FSB (do int32 NEON stores to L1/L2/L3)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sse_write_only_int_init,
    .test_run = mesh_upi_sse_write_only_int_run,
    .test_cleanup = mesh_upi_sse_write_only_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
