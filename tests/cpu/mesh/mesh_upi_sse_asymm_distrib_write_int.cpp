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
    /* sense-reversal 轮屏障（2731971 已验证模式，自 distrib 姊妹移植）：
     * 原 reset-winner 清零协议（读核心经 reset_lock CAS 选举复位）存在
     * 已实证的假 FAIL 竞态——写核心尾部 next_block 自旋的放行判据与
     * 复位时机在孵化偏斜/预算耗尽下错位，读核心可读到跨轮撕裂数组
     * （expected_sum 呈 0 或部分和）→ 假阳性 FAIL。arrived/epoch 以
     * 固定参与者计数（thread_count()，全体写核心+唯一读核心）为判据，
     * 每轮状态由屏障最后到达者统一复位，无清零竞态、无胜者选举。 */
    std::atomic<uint32_t> arrived;       // 本轮已到达屏障的线程数（全体写者+读者）
    std::atomic<uint32_t> epoch;         // 轮次代（sense 位）
};

static int mesh_upi_sse_asymm_distrib_write_int_init(struct test *test) {
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

static int mesh_upi_sse_asymm_distrib_write_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    if (thread_count() < 2) {
        log_skip(CpuTopologyIssueSkipCategory,
                 "mesh_upi_sse_asymm_distrib_write_int requires at least 2 threads (inter-core test); "
                 "skipping on this thread count");
        return EXIT_SKIP;
    }

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    bool is_reader = (id == 0);

    /* 屏障参与者总数取框架权威值 thread_count()（自动尊重 --cpuset / -n /
     * test.max_threads，每个线程取值一致；2731971 修法）＝全体写核心＋
     * 唯一读核心。 */
    uint32_t total_threads = thread_count();

    /* randomization hardening H14' (P17): framework RNG (per-thread
     * stream, -s reproducible) replaces std::mt19937; range [-1000000, 1000000). */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    uint32_t my_epoch = 0;
    do {
        if (!is_reader) {
            // 写核心：循环分配块直到所有块分配完
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
                bool ok = (vgetq_lane_u32(cmp, 0) == 0xFFFFFFFF &&
                           vgetq_lane_u32(cmp, 1) == 0xFFFFFFFF &&
                           vgetq_lane_u32(cmp, 2) == 0xFFFFFFFF &&
                           vgetq_lane_u32(cmp, 3) == 0xFFFFFFFF);
                if (!ok) {
                    // 发现不一致，但继续处理（最终会在读核心校验时失败）
                }

                td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
                td->allocated_blocks.fetch_add(1, std::memory_order_seq_cst);
            }
            /* 尾部 next_block 自旋（等读核心复位）连同 reset-winner 协议
             * 一并移除：轮次隔离由下方 sense-reversal 屏障接管——写核心
             * 填满后到达屏障，读核心读完校验后到达，最后到达者复位并
             * 翻转 epoch，next_block 越界回绕覆盖在读数组的问题不复
             * 存在。 */
        }

        if (is_reader) {
            /* 等待所有块分配完成。原实现每次自旋都调 test_time_condition：
             * 读核心（id 0，首个入场线程）先于写者线程进入 test_run，预算
             * 在写者尚未填充任何块时就被烧光，等待提前中断 → 校验被跳过
             * → vacuous pass。按同族 avx2 变体既证的节流模式：等待条件
             * 每自旋检查、ttc 预算按档节流检查、预算耗尽走 out_of_budget
             * 干净退出。档距取 131072（而非 1024）：要桥接的是写者线程的
             * 启动滞后——同机同族实测（Kunpeng 920，192 核）写者进入
             * test_run 比读者晚 2.7~6.7ms（中位 3.8ms），而自旋实测
             * ~1.7ns/次，1024 一档 × 预算只覆盖 ~70µs 差 50 倍，
             * × 131072 ≈ 8.9ms 才能盖住最大滞后。 */
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
                fprintf(stderr, "mesh_upi_sse_asymm_distrib_write_int: Thread %d (reader), data[0..3]=(%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, consistent=%d, result=%s%s%s\n",
                        id,
                        td->data[0], td->data[1], td->data[2], td->data[3],
                        read_sum, expected_sum, consistent,
                        passed ? GREEN : RED,
                        passed ? "PASS" : "FAIL",
                        RESET);
                fflush(stderr);
                report_fail_msg("mesh_upi_sse_asymm_distrib_write_int: Sum mismatch or consistency failure");
                return EXIT_FAILURE;
            }

        }

        /* ---------- 轮屏障：sense-reversal，全体写者与读者都到达 ----------
         * 最后到达者在此复位下一轮状态（global_sum/next_block/
         * allocated_blocks）后翻转 epoch：fetch_add(acq_rel) 的到达链
         * 保证复位发生时所有线程都已离开本轮的填充/校验——写者不可能
         * 在读者校验期间开写下一轮，读者也不可能读到部分填充的数组
         * （这正是原 reset-winner 清零协议想做而没做对的轮次隔离），
         * epoch 的 release 存储保证复位先于任何线程的下一轮访问可见。 */
        {
            uint32_t done = td->arrived.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (done == total_threads) {
                td->global_sum.store(0, std::memory_order_seq_cst);
                td->next_block.store(0, std::memory_order_seq_cst);
                td->allocated_blocks.store(0, std::memory_order_seq_cst);
                td->arrived.store(0, std::memory_order_relaxed);
                td->epoch.store(my_epoch + 1, std::memory_order_release);
            } else {
                /* 自旋按 131072 一档节流预算检查：该等待必须覆盖同族实测
                 * 2.7-6.7ms 的线程孵化偏斜（首轮早到者要等最晚到达者，
                 * 1024 一档在 40 次预算下只覆盖 ~70µs，会把每轮都变成
                 * 预算耗尽的 vacuous 退出），40 次 × 131072 自旋 ×
                 * ~1.7ns ≈ 8.9ms 覆盖实测最大值（8502e491 既证参数）；
                 * 等待条件本身每自旋都查，响应不受影响；到期各自走
                 * out_of_budget 有界退出（他人亦到期退出，无死锁）。 */
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

static int mesh_upi_sse_asymm_distrib_write_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_sse_asymm_distrib_write_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_sse_asymm_distrib_write_int");
    return EXIT_SKIP;
}
static int mesh_upi_sse_asymm_distrib_write_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_sse_asymm_distrib_write_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_sse_asymm_distrib_write_int,
             "Asymmetrical(1 read core from all rest distributed write cores) stress MESH and UPI or Ring Interconnect and QPI or FSB (do int32 NEON loads and stores from L1/L2 to L1/L2 with switching read core)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sse_asymm_distrib_write_int_init,
    .test_run = mesh_upi_sse_asymm_distrib_write_int_run,
    .test_cleanup = mesh_upi_sse_asymm_distrib_write_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
