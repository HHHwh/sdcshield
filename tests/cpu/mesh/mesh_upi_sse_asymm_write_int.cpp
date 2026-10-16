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

#define TOTAL_ELEMENTS 1024               // 总元素数
#define VECTOR_SIZE 4                     // NEON 每次处理 4 个 int32
#define BLOCK_ELEMENTS VECTOR_SIZE
#define NUM_BLOCKS (TOTAL_ELEMENTS / BLOCK_ELEMENTS)  // = 256
#define TOTAL_BYTES (TOTAL_ELEMENTS * sizeof(int32_t))

struct TestData {
    alignas(16) int32_t *data;           // 共享数据数组，16 字节对齐
    std::atomic<uint32_t> next_block;    // 下一个可分配的块索引
    std::atomic<uint64_t> global_sum;    // 写核心的校验和累加
    std::atomic<uint32_t> allocated_blocks; // 已分配的块数
    std::atomic<uint32_t> thread_idx;    // 线程 ID 分配器
    /* sense-reversal 轮屏障（2731971 已验证模式）：原 iter 轮转协议的
     * 写者分支是除超时外无可行出口的无限循环（外层 while(ttc) 内自旋
     * 等 allocated<NUM_BLOCKS 再认领写块）——写者一旦进入就永远回不到
     * do-while 顶端重读 iter，"轮流当读者"的轮转从未发生：第 0 轮由
     * 最先入场的 id 0 校验一次并单方复位后，全体线程（含原读者）被钉
     * 死在写者自旋里空转到超时，后续轮次的校验全部缺失（vacuous）。
     * 原读者复位（global_sum/next_block/allocated_blocks 逐条清零）也
     * 是本族 13b13850 结构性消除的"每迭代清零计数器 + 复位者单方写"
     * 模式。round_done/reader_count 自始未被读写（死状态，移植残留）。
     * arrived/epoch 固定参与者屏障 + 本地 my_epoch 轮转读者让轮转真正
     * 发生；轮次状态由屏障最后到达者复位，无清零竞态窗口。 */
    std::atomic<uint32_t> arrived;       // 本轮已到达屏障的线程数（写者+读者）
    std::atomic<uint32_t> epoch;         // 轮次代（sense 位）
};

static int mesh_upi_sse_asymm_write_int_init(struct test *test) {
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

static int mesh_upi_sse_asymm_write_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    // 每个线程分配唯一 ID
    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);

    /* 线程总数取框架权威值 thread_count()（自动尊重 --cpuset、
     * test.max_threads 与 OS 亲和性限制，每个线程取值一致）—— 与
     * 2731971 对 sym/symm/asymm 六文件的修复相同。原“3 次稳定读数”
     * 启发式在 -n 1 下会永久自旋挂死（cur > 1 永不成立，后面的
     * 线程数检查不可达），大规模并发下又严重欠计；单线程运行现在按
     * memcpy_rewr 先例干净跳过（log_skip + EXIT_SKIP）。 */
    uint32_t total_threads = thread_count();
    if (total_threads < 2) {
        log_skip(CpuTopologyIssueSkipCategory,
                 "mesh_upi_sse_asymm_write_int requires at least 2 threads (inter-core test); "
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
        int reader_id = my_epoch % total_threads;  // 轮流当读者（原 iter 轮转语义）

        if (id == reader_id) {
            // ---------- 读者：等待所有块分配完成 ----------
            /* 等待条件每自旋检查、ttc 预算按档节流检查、预算耗尽走
             * out_of_budget 干净退出（2731971/8502e491 模式）。档距
             * 131072（8502e491 实测参数）：首轮读者是最先入场的 id 0，
             * 要桥接写者线程 2.7~6.7ms 的孵化偏斜，而 1024 一档下
             * 40 次预算仅覆盖 ~70µs，会把首轮变成预算耗尽的 vacuous
             * 退出（40 × 131072 × ~1.7ns ≈ 8.9ms 覆盖实测最大值）。 */
            uint32_t spin = 0;
            while (td->allocated_blocks.load(std::memory_order_seq_cst) < NUM_BLOCKS) {
                if ((++spin & 131071u) == 0) {
                    if (!test_time_condition(test)) goto out_of_budget;
                } else {
                    __asm__ volatile("yield");
                }
            }

            // 全内存屏障
            __sync_synchronize();

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
                // 检查所有 4 个元素是否相等
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
                fprintf(stderr, "mesh_upi_sse_asymm_write_int: Thread %d (reader), data[0..3]=(%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, consistent=%d, result=%s%s%s\n",
                        id,
                        td->data[0], td->data[1], td->data[2], td->data[3],
                        read_sum, expected_sum, consistent,
                        passed ? GREEN : RED,
                        passed ? "PASS" : "FAIL",
                        RESET);
                fflush(stderr);
                report_fail_msg("mesh_upi_sse_asymm_write_int: Sum mismatch or consistency failure");
                return EXIT_FAILURE;
            }

        } else {
            // ---------- 写者：竞争分配块并写入随机数据（本轮有限工作量） ----------
            /* 原实现这里是无限外层循环 + allocated_blocks 自旋等待——
             * 写者永不回到轮转判定，是协议失效的根因（见 TestData 注释）。
             * 认领语义不变：fetch_add 竞争，认到 >= NUM_BLOCKS 即本轮
             * 退出；每块写入（随机数生成、NEON 存储、sfence、立即读回、
             * 记账提交）与原实现逐句相同。 */
            while (true) {
                uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
                if (block >= NUM_BLOCKS) {
                    break;
                }

                size_t offset = block * BLOCK_ELEMENTS;
                int32_t vals[VECTOR_SIZE];
                uint64_t local_sum = 0;
                for (int j = 0; j < VECTOR_SIZE; ++j) {
                    vals[j] = dist();
                    local_sum += (uint64_t)vals[j];
                }
                // NEON 存储
                int32x4_t v = vld1q_s32(vals);
                vst1q_s32(td->data + offset, v);
                __sync_synchronize();   // 类似 _mm_sfence

                // 一致性测试：立即读回比较（保留原逻辑）
                int32x4_t loaded = vld1q_s32(td->data + offset);
                uint32x4_t cmp = vceqq_s32(v, loaded);
                if (!(vgetq_lane_u32(cmp, 0) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 1) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 2) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 3) == 0xFFFFFFFF)) {
                    // 发现不一致，但继续处理（最终会在读者校验时失败）
                }

                td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
                td->allocated_blocks.fetch_add(1, std::memory_order_seq_cst);
            }
        }

        /* ---------- 轮屏障：sense-reversal，读者与全体写者都到达 ----------
         * 最后到达者在此复位下一轮状态（global_sum/next_block/
         * allocated_blocks）后翻转 epoch：fetch_add(acq_rel) 的到达链
         * 保证复位发生时所有线程都已离开本轮的认领/校验，epoch 的
         * release 存储保证复位先于任何线程的下一轮访问可见 —— 这正是
         * 原协议想做而没做对的轮次隔离（原版由读者在写者仍被困于
         * 无限循环时单方清零，轮转从未发生）。reader 随 my_epoch 轮转。
         * 自旋按 131072 一档节流预算检查：首轮早到者须等最晚孵化者
         * （同读者等待的档距依据）。 */
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

static int mesh_upi_sse_asymm_write_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_sse_asymm_write_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_sse_asymm_write_int");
    return EXIT_SKIP;
}
static int mesh_upi_sse_asymm_write_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_sse_asymm_write_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_sse_asymm_write_int,
             "Asymmetrical write: 1 read core from all rest mutual write cores (NEON, shared buffer)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sse_asymm_write_int_init,
    .test_run = mesh_upi_sse_asymm_write_int_run,
    .test_cleanup = mesh_upi_sse_asymm_write_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
