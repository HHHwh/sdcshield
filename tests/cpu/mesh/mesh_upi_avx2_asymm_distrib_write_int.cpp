#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#ifdef __aarch64__
#include <arm_neon.h>
#include <unistd.h>
#include <sys/sysinfo.h>

#define TOTAL_ELEMENTS 8
#define VECTOR_SIZE 4                     // NEON 一次处理 4 个 int32
#define BLOCK_ELEMENTS TOTAL_ELEMENTS     // 块大小仍为 8（两个向量）
#define NUM_BLOCKS (TOTAL_ELEMENTS / BLOCK_ELEMENTS)   // =1
#define TOTAL_BYTES (TOTAL_ELEMENTS * sizeof(int32_t))

struct TestData {
    alignas(16) int32_t *data;            // 16 字节对齐即可
    std::atomic<uint32_t> next_block;
    std::atomic<uint64_t> global_sum;
    std::atomic<uint32_t> write_done;      // 已完成写入并提交校验和的块数
    std::atomic<uint32_t> thread_idx;
    /* sense-reversal 轮屏障（2731971 已验证模式，自 distrib 姊妹移植）：
     * 原 reset-winner 清零协议（读核心经 reset_lock CAS 复位）存在已
     * 实证的假 FAIL 竞态——复位时机与写者重入在孵化偏斜/预算耗尽下
     * 错位，读核心可读到跨轮撕裂数组（expected_sum 呈 0 或部分和）→
     * 假阳性 FAIL。arrived/epoch 以固定参与者计数（thread_count()，
     * 全体写核心+唯一读核心）为判据，每轮状态由屏障最后到达者统一
     * 复位，无清零竞态、无胜者选举。 */
    std::atomic<uint32_t> arrived;         // 本轮已到达屏障的线程数（全体写者+读者）
    std::atomic<uint32_t> epoch;           // 轮次代（sense 位）
};

static int mesh_upi_avx2_asymm_distrib_write_int_init(struct test *test) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) {
        return -255;
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
    td->arrived.store(0, std::memory_order_relaxed);
    td->epoch.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx2_asymm_distrib_write_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    if (thread_count() < 2) {
        log_skip(CpuTopologyIssueSkipCategory,
                 "mesh_upi_avx2_asymm_distrib_write_int requires at least 2 threads (inter-core test); "
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
            // 写核心：竞争分配块并写入
            while (true) {
                uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
                if (block >= NUM_BLOCKS) break;

                // 生成随机数据（8个 int32）
                int32_t vals[TOTAL_ELEMENTS];
                uint64_t local_sum = 0;
                for (int j = 0; j < TOTAL_ELEMENTS; ++j) {
                    vals[j] = dist();
                    local_sum += (uint64_t)vals[j];
                }
                // 分两个 NEON 向量存储
                int32x4_t v0 = vld1q_s32(vals);
                int32x4_t v1 = vld1q_s32(vals + 4);
                vst1q_s32(td->data, v0);
                vst1q_s32(td->data + 4, v1);
                __sync_synchronize();   // 确保数据对其它核心可见

                td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
                td->write_done.fetch_add(1, std::memory_order_seq_cst);
            }
            /* 尾部 next_block 自旋（等读核心复位）连同 reset-winner 协议
             * 一并移除：轮次隔离由下方 sense-reversal 屏障接管——写核心
             * 填完后到达屏障，读核心读完校验后到达，最后到达者复位并
             * 翻转 epoch，写者在读者读取期间越界重入 next_block 的窗口
             * 不复存在。 */
        } else {
            /* 读核心：等待数据就绪并校验。原内层 while(test_time_condition)
             * 循环连同 reset-winner 复位协议由"每轮一次校验 + 下方轮
             * 屏障"替代。 */
            /* 等待写核心完成写入。原实现每次自旋都调
             * test_time_condition：读核心（id 0）先于写者线程进入
             * test_run，40 次预算在写者尚未填充任何块时就被自旋
             * 烧光，等待提前中断 → 校验被跳过 → vacuous pass。
             * 按已验证的节流模式：等待条件每自旋检查、ttc 预算按档
             * 节流检查、预算耗尽走 out_of_budget 干净退出。
             *
             * 档距取 131072（而非 1024）：这里要桥接的是第二个工作
             * 线程的启动滞后 —— 实测本机（Kunpeng 920，192 核，
             * -f no -n 2）写者线程进入 test_run 比读者晚
             * 2.7~6.7ms（中位 3.8ms），而本自旋实测 ~1.7ns/次。
             * 40 次预算 × 1024 × 1.7ns ≈ 70µs，差 50 倍；
             * × 131072 ≈ 8.9ms 才能盖住最大滞后。 */
            uint32_t spin = 0;
            while (td->write_done.load(std::memory_order_seq_cst) < NUM_BLOCKS) {
                if ((++spin & 131071u) == 0) {
                    if (!test_time_condition(test)) goto out_of_budget;
                } else {
                    __asm__ volatile("yield");
                }
            }

            // 读取数据（两个向量）
            // Sum snapshot BEFORE the read (TOCTOU fix): a fast thread
            // can finish its read, run the reset phase and start writing
            // the NEXT iteration while this thread is still walking the
            // buffer. Sampling global_sum after the read would race that
            // reset/next-write and produce a false mismatch.
            uint64_t expected_sum = td->global_sum.load(std::memory_order_seq_cst);
            int32x4_t v0 = vld1q_s32(td->data);
            int32x4_t v1 = vld1q_s32(td->data + 4);
            int32_t vals[TOTAL_ELEMENTS];
            vst1q_s32(vals, v0);
            vst1q_s32(vals + 4, v1);
            uint64_t read_sum = 0;
            for (int j = 0; j < TOTAL_ELEMENTS; ++j) {
                read_sum += (uint64_t)vals[j];
            }

            bool passed = (read_sum == expected_sum);

            if (!passed) {
                // 首次失败证据（原先每轮都打印 PASS 行；与 Task 5 家族统一移入失败分支）
                fprintf(stderr, "mesh_upi_avx2_asymm_distrib_write_int: Thread %d (reader), data[0..7]=(%d,%d,%d,%d,%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, result=%s%s%s\n",
                        id,
                        td->data[0], td->data[1], td->data[2], td->data[3],
                        td->data[4], td->data[5], td->data[6], td->data[7],
                        read_sum, expected_sum,
                        passed ? GREEN : RED,
                        passed ? "PASS" : "FAIL",
                        RESET);
                fflush(stderr);
                report_fail_msg("mesh_upi_avx2_asymm_distrib_write_int: Sum mismatch");
                return EXIT_FAILURE;
            }
        }

        /* ---------- 轮屏障：sense-reversal，全体写者与读者都到达 ----------
         * 最后到达者在此复位下一轮状态（global_sum/write_done/
         * next_block）后翻转 epoch：fetch_add(acq_rel) 的到达链保证
         * 复位发生时所有线程都已离开本轮的写入/校验——写者不可能在
         * 读者校验期间开写下一轮（原"next_block 最后归零"释放栅栏的
         * 意图由屏障整体接管），epoch 的 release 存储保证复位先于任何
         * 线程的下一轮访问可见。 */
        {
            uint32_t done = td->arrived.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (done == total_threads) {
                td->global_sum.store(0, std::memory_order_seq_cst);
                td->write_done.store(0, std::memory_order_seq_cst);
                td->next_block.store(0, std::memory_order_seq_cst);
                td->arrived.store(0, std::memory_order_relaxed);
                td->epoch.store(my_epoch + 1, std::memory_order_release);
            } else {
                /* 自旋按 131072 一档节流预算检查：该等待必须覆盖上方注释
                 * 实测 2.7-6.7ms 的线程孵化偏斜（首轮早到者要等最晚到达
                 * 者，1024 一档在 40 次预算下只覆盖 ~70µs，会把每轮都变
                 * 成预算耗尽的 vacuous 退出）；等待条件本身每自旋都查，
                 * 响应不受影响；到期各自走 out_of_budget 有界退出（他人
                 * 亦到期退出，无死锁）。 */
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

static int mesh_upi_avx2_asymm_distrib_write_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx2_asymm_distrib_write_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_avx2_asymm_distrib_write_int");
    return EXIT_SKIP;
}
static int mesh_upi_avx2_asymm_distrib_write_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx2_asymm_distrib_write_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx2_asymm_distrib_write_int,
             "Asymmetrical(1 read core from all rest distributed write cores) stress MESH and UPI or Ring Interconnect and QPI (do int32 AVX-2(YMM) loads and stores from L1/L2 to L1/L2 with switching read core) [ARM NEON version]")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx2_asymm_distrib_write_int_init,
    .test_run = mesh_upi_avx2_asymm_distrib_write_int_run,
    .test_cleanup = mesh_upi_avx2_asymm_distrib_write_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
