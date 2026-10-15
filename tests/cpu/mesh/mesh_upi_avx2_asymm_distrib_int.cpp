#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
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
    std::atomic<uint32_t> allocated_blocks;
    std::atomic<uint32_t> thread_idx;
    /* sense-reversal 轮屏障（2731971 已验证模式）：原 round_done/
     * reader_count 协议的完成判据 `done == reader_count` 以孵化期间仍在
     * 单调增长的注册计数为目标——先到的读者群会把"部分 cohort"误判为
     * "全体已完成"而提前复位轮次，写者随即重填，mid-verify 读者读到撕裂
     * 数组 → 假阳性 FAIL（smoke p2_mesh 已观测同签名失败 1 例：
     * expected_sum=0 + 旧 8 元轮和；详见 mesh_bug_proof.md）。
     * reset_lock 为死字段（CAS 复位协议属 avx512 变体），一并移除。 */
    std::atomic<uint32_t> arrived;        // 本轮已到达屏障的线程数（写者+读者）
    std::atomic<uint32_t> epoch;          // 轮次代（sense 位）
};

static int mesh_upi_avx2_asymm_distrib_int_init(struct test *test) {
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
    td->allocated_blocks.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    td->arrived.store(0, std::memory_order_relaxed);
    td->epoch.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx2_asymm_distrib_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    bool is_writer = (id == 0);

    /* 屏障参与者总数取框架权威值 thread_count()（自动尊重 --cpuset / -n /
     * test.max_threads，每个线程取值一致；2731971 修法）。原协议以
     * reader_count 为完成判据目标——它是"入场时才自增"的注册计数，
     * 孵化窗口内仍在单调增长（无启动屏障），判据可被部分 cohort 提前
     * 满足 → 假阳性复位 → 撕裂读（同 sse 姊妹，见 TestData 注释与
     * mesh_bug_proof.md §2）。 */
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
             * round_done==0"的顶屏障连同缺陷判据一并移除。填满仅有的
             * 1 块（8 元，~µs），不消耗 fracture 预算。 */
            uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
            if (block < NUM_BLOCKS) {
                // 生成随机数据并写入（8个 int32，分两个 NEON 向量）
                int32_t vals[TOTAL_ELEMENTS];
                uint64_t local_sum = 0;
                for (int j = 0; j < TOTAL_ELEMENTS; ++j) {
                    vals[j] = dist();
                    local_sum += (uint64_t)vals[j];
                }
                // 前 4 个
                int32x4_t v0 = vld1q_s32(vals);
                vst1q_s32(td->data, v0);
                // 后 4 个
                int32x4_t v1 = vld1q_s32(vals + 4);
                vst1q_s32(td->data + 4, v1);
                // 内存屏障确保数据对其它核心可见
                __sync_synchronize();

                td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
                td->allocated_blocks.fetch_add(1, std::memory_order_seq_cst);
            }
        } else {
            /* 读核心：等待本轮写入完成（NUM_BLOCKS=1，等待 ~µs 级）。
             * 原实现每次自旋都调 test_time_condition（098d4aa 之前的
             * 预算烧法），改为 1024 自旋一档节流（2731971/8502e491 既证
             * 模式）：条件每自旋查，预算按档查，耗尽走 out_of_budget。 */
            uint32_t spin = 0;
            while (td->allocated_blocks.load(std::memory_order_seq_cst) < NUM_BLOCKS) {
                if ((++spin & 1023u) == 0) {
                    if (!test_time_condition(test)) goto out_of_budget;
                } else {
                    __asm__ volatile("yield");
                }
            }

            // 读取数据（两个向量）
            // Sum snapshot BEFORE the read (TOCTOU fix): a fast thread can
            // finish its read, run the reset phase and start writing the NEXT
            // iteration while this thread is still walking the buffer.
            uint64_t expected_sum = td->global_sum.load(std::memory_order_seq_cst);
            int32x4_t v0 = vld1q_s32(td->data);
            int32x4_t v1 = vld1q_s32(td->data + 4);
            int32_t vals[8];
            vst1q_s32(vals, v0);
            vst1q_s32(vals + 4, v1);
            uint64_t read_sum = 0;
            for (int j = 0; j < TOTAL_ELEMENTS; ++j) {
                read_sum += (uint64_t)vals[j];
            }

            bool passed = (read_sum == expected_sum);

            if (!passed) {
                // 首次失败证据（原先每个读迭代都从读核心 1 打印 PASS 行）
                fprintf(stderr, "mesh_upi_avx2_asymm_distrib_int: Thread %d (reader), data[0..7]=(%d,%d,%d,%d,%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, result=%s%s%s\n",
                        id,
                        td->data[0], td->data[1], td->data[2], td->data[3],
                        td->data[4], td->data[5], td->data[6], td->data[7],
                        read_sum, expected_sum,
                        passed ? GREEN : RED,
                        passed ? "PASS" : "FAIL",
                        RESET);
                fflush(stderr);
                report_fail_msg("mesh_upi_avx2_asymm_distrib_int: Sum mismatch");
                return EXIT_FAILURE;
            }
        }

        /* ---------- 轮屏障：sense-reversal，写者与全体读者都到达 ----------
         * 最后到达者复位下一轮状态后翻转 epoch：fetch_add(acq_rel) 到达链
         * 保证复位时所有线程都已离开本轮填充/校验，epoch release 存储保证
         * 复位先于任何线程下一轮访问可见（2731971 已验证模式）。 */
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
                 * 2.7-6.7ms 的线程孵化偏斜（首轮早到者要等最晚到达者；
                 * 1024 一档在 40 次预算下只覆盖 ~70µs），40 次 × 131072
                 * 自旋 × ~1.7ns ≈ 8.9ms（8502e491 既证参数）；条件本身
                 * 每自旋都查，响应不受影响。 */
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

static int mesh_upi_avx2_asymm_distrib_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx2_asymm_distrib_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_avx2_asymm_distrib_int");
    return EXIT_SKIP;
}
static int mesh_upi_avx2_asymm_distrib_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx2_asymm_distrib_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx2_asymm_distrib_int,
             "Asymmetrical(1 write core to all rest distributed read cores) stress MESH and UPI or Ring Interconnect and QPI (do int32 AVX-2(YMM) loads and stores from L1/L2 to L1/L2 with switching write core) [ARM NEON version]")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx2_asymm_distrib_int_init,
    .test_run = mesh_upi_avx2_asymm_distrib_int_run,
    .test_cleanup = mesh_upi_avx2_asymm_distrib_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
