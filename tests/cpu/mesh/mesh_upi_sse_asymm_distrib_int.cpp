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
    /* sense-reversal 轮屏障（2731971 已验证模式）：原 round_done/reader_count
     * 协议的完成判据 `done == reader_count` 以孵化期间仍在单调增长的注册
     * 计数为目标——先到的读者群会把"部分 cohort"误判为"全体已完成"而提前
     * 复位轮次，写者随即重填，mid-verify 读者读到旧轮残余+新轮前缀的撕裂
     * 数组，expected_sum 呈 0 或"重填前 k 块部分和"→ 假阳性 FAIL（战役
     * 49 条失败全部此签名；5 例 expected_sum 恰等于 data[0..3] 之和的算术
     * 铁证）。arrived/epoch 固定参与者计数无此缺陷。 */
    std::atomic<uint32_t> arrived;       // 本轮已到达屏障的线程数（写者+读者）
    std::atomic<uint32_t> epoch;         // 轮次代（sense 位）
};

static int mesh_upi_sse_asymm_distrib_int_init(struct test *test) {
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

static int mesh_upi_sse_asymm_distrib_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    bool is_writer = (id == 0);

    /* 屏障参与者总数取框架权威值 thread_count()（自动尊重 --cpuset / -n /
     * test.max_threads，每个线程取值一致；2731971 修法）。原协议以
     * reader_count 为完成判据目标——它是"入场时才自增"的注册计数，
     * 孵化窗口内仍在单调增长（无启动屏障），判据可被部分 cohort 提前
     * 满足，这就是假阳性复位的根因（见 TestData 注释与
     * mesh_bug_proof.md §2 的逐行 walkthrough）。 */
    uint32_t total_threads = thread_count();

    /* randomization hardening H14' (P17): framework RNG (per-thread
     * stream, -s reproducible) replaces std::mt19937; range [-1000000, 1000000). */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    uint32_t my_epoch = 0;
    do {
        if (is_writer) {
            /* 写者：本轮状态已由上一轮 sense-reversal 屏障的最后到达者
             * 复位（首轮由 init 复位）——原"等 allocated_blocks==0 且
             * round_done==0"的顶屏障连同 round_done/reader_count 协议
             * 一并移除：它等待的正是那个会提前复位的缺陷判据。 */

            /* 串行填充所有块：有限工作量（NUM_BLOCKS×VECTOR_SIZE 个
             * 元素，微秒级），不逐块消耗 fracture 预算 —— 原实现每块
             * 烧 2 次 test_time_condition，一轮 256 块需 ~513 次而
             * 预算（自动翻倍窗口关闭后停留在 ~160）永远达不到，写者
             * 在填充中途预算耗尽退出。同 2731971 写者的无守卫填充。 */
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
                /* 写者的立即读回是唯一逐字节的写入内容校验——读核心的
                 * sum 校验可被保和损坏（如元素交换）绕过，所以 ok 为假
                 * 必须直接判失败并保留证据，不能丢弃结果（原实现此处为
                 * 空分支即丢弃；同 87ebc4b / 8502e491 SVE-wide 的修法）。 */
                if (!ok) {
                    fprintf(stderr, "mesh_upi_sse_asymm_distrib_int: Thread %d (writer), block %u immediate read-back mismatch, written vals[0..3]=(%d,%d,%d,%d), loaded data[0..3] at offset %zu=(%d,%d,%d,%d), result=%sFAIL%s\n",
                            id, block,
                            vals[0], vals[1], vals[2], vals[3],
                            offset,
                            td->data[offset], td->data[offset + 1],
                            td->data[offset + 2], td->data[offset + 3],
                            RED, RESET);
                    fflush(stderr);
                    report_fail_msg("mesh_upi_sse_asymm_distrib_int: writer immediate read-back mismatch (store/load corruption)");
                    return EXIT_FAILURE;
                }

                td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
                td->allocated_blocks.fetch_add(1, std::memory_order_seq_cst);
            }
        } else {
            /* 读核心：等待所有块分配完成。原实现每次自旋都调
             * test_time_condition：读者 40 次预算在微秒级就被自旋
             * 烧光（实测读者放弃时写者仅填到第 1 块；而旧写者的
             * 逐块预算烧法本身也到不了 256 块），等待提前中断 →
             * 校验被跳过 → vacuous pass（实测 -f no -t 3000 -n 2
             * 下 0 行读端验证输出）。按 2731971 已验证的节流模式：
             * 等待条件每自旋检查、ttc 预算每 1024 自旋检查一次，
             * 预算耗尽走 out_of_budget 干净退出。 */
            uint32_t spin = 0;
            while (td->allocated_blocks.load(std::memory_order_seq_cst) < NUM_BLOCKS) {
                if ((++spin & 1023u) == 0) {
                    if (!test_time_condition(test)) goto out_of_budget;
                } else {
                    __asm__ volatile("yield");
                }
            }

            // 全内存屏障
            __sync_synchronize();

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
                // 首次失败证据（原先每轮都打印 PASS 行；与 Task 5 家族统一移入失败分支）
                fprintf(stderr, "mesh_upi_sse_asymm_distrib_int: Thread %d (reader), data[0..3]=(%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, consistent=%d, result=%s%s%s\n",
                        id,
                        td->data[0], td->data[1], td->data[2], td->data[3],
                        read_sum, expected_sum, consistent,
                        passed ? GREEN : RED,
                        passed ? "PASS" : "FAIL",
                        RESET);
                fflush(stderr);
                report_fail_msg("mesh_upi_sse_asymm_distrib_int: Sum mismatch or consistency failure");
                return EXIT_FAILURE;
            }

        }

        /* ---------- 轮屏障：sense-reversal，写者与全体读者都到达 ----------
         * 最后到达者在此复位下一轮状态（global_sum/next_block/
         * allocated_blocks）后翻转 epoch：fetch_add(acq_rel) 的到达链
         * 保证复位发生时所有线程都已离开本轮的填充/校验（这正是原
         * reader_count 移动目标判据想做而没做对的轮次隔离），epoch 的
         * release 存储保证复位先于任何线程的下一轮访问可见。 */
        {
            uint32_t done = td->arrived.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (done == total_threads) {
                td->global_sum.store(0, std::memory_order_seq_cst);
                td->next_block.store(0, std::memory_order_seq_cst);
                td->allocated_blocks.store(0, std::memory_order_seq_cst);
                td->arrived.store(0, std::memory_order_relaxed);
                td->epoch.store(my_epoch + 1, std::memory_order_release);
            } else {
                /* 自旋按 131072 一档节流预算检查：该等待必须覆盖实测
                 * 2.7-6.7ms 的线程孵化偏斜（首轮早到者要等最晚到达者，
                 * 1024 一档在 40 次预算下只覆盖 ~70µs，会把每轮都变成
                 * 预算耗尽的 vacuous 退出），40 次 × 131072 自旋 ×
                 * ~1.7ns ≈ 8.9ms 覆盖实测最大值（8502e491 既证参数）；
                 * 等待条件本身每自旋都查，响应不受影响。 */
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

static int mesh_upi_sse_asymm_distrib_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_sse_asymm_distrib_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_sse_asymm_distrib_int");
    return EXIT_SKIP;
}
static int mesh_upi_sse_asymm_distrib_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_sse_asymm_distrib_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_sse_asymm_distrib_int,
             "Asymmetrical(1 write core to all rest distributed read cores) stress MESH and UPI or Ring Interconnect and QPI or FSB (do int32 NEON loads and stores from L1/L2 to L1/L2 with switching write core)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sse_asymm_distrib_int_init,
    .test_run = mesh_upi_sse_asymm_distrib_int_run,
    .test_cleanup = mesh_upi_sse_asymm_distrib_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
