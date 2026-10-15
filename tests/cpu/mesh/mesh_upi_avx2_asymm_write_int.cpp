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
    std::atomic<uint32_t> reset_lock;
};

static int mesh_upi_avx2_asymm_write_int_init(struct test *test) {
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
    td->reset_lock.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx2_asymm_write_int_run(struct test *test, int cpu) {
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
            /* 等待读核心完成本轮读取并重置 next_block。若不等待，写核心会在
             * 读核心读取期间越界重入 next_block.fetch_add，使 32 位 next_block
             * 回绕后覆盖读核心正在读取的数据，造成 sum 校验假失败。 */
            while (td->next_block.load(std::memory_order_seq_cst) != 0 &&
                   test_time_condition(test)) {
                __asm__ volatile("yield");
            }
        } else {
            // 读核心：等待数据就绪，校验，并重置
            while (test_time_condition(test)) {
                /* 等待写核心完成写入。原实现每次自旋都调
                 * test_time_condition：读核心（id 0）先于写者线程进入
                 * test_run，40 次预算在写者尚未填充任何块时就被自旋
                 * 烧光，等待提前中断 → 校验被跳过 → vacuous pass
                 * （实测 -f no -t 3000 -n 2 下 0 行读端验证输出）。
                 * 按 2731971 已验证的节流模式：等待条件每自旋检查、
                 * ttc 预算按档节流检查、预算耗尽走 out_of_budget
                 * 干净退出。
                 *
                 * 档距取 131072（而非参考实现的 1024）：这里要桥接的
                 * 是第二个工作线程的启动滞后 —— 实测本机（Kunpeng 920，
                 * 192 核，-f no -n 2）写者线程进入 test_run 比读者晚
                 * 2.7~6.7ms（中位 3.8ms，n=251），而本自旋实测
                 * ~1.7ns/次（无 SMT，yield 为空提示；共享行 L1 命中）。
                 * 40 次预算 × 1024 × 1.7ns ≈ 70µs，差 50 倍；
                 * × 131072 ≈ 8.9ms 才能盖住最大滞后。2731971 的 1024
                 * 面向微秒级等待（其写者是先进入的线程，数据早已
                 * 就绪），本文件角色相反，读者的首次等待是毫秒级。 */
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
                    fprintf(stderr, "mesh_upi_avx2_asymm_write_int: Thread %d (reader), data[0..7]=(%d,%d,%d,%d,%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, result=%s%s%s\n",
                            id,
                            td->data[0], td->data[1], td->data[2], td->data[3],
                            td->data[4], td->data[5], td->data[6], td->data[7],
                            read_sum, expected_sum,
                            passed ? GREEN : RED,
                            passed ? "PASS" : "FAIL",
                            RESET);
                    fflush(stderr);
                    report_fail_msg("mesh_upi_avx2_asymm_write_int: Sum mismatch");
                    return EXIT_FAILURE;
                }

                // ---------- 重置状态（仅由一个读核心执行） ----------
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
                    /* 未获得锁，等待重置完成（同样按 1024 自旋一档节流
                     * ttc 预算检查）。 */
                    uint32_t spin = 0;
                    while (td->write_done.load(std::memory_order_seq_cst) > 0) {
                        if ((++spin & 1023u) == 0) {
                            if (!test_time_condition(test)) goto out_of_budget;
                        } else {
                            __asm__ volatile("yield");
                        }
                    }
                }
            }
        }
    } while (test_time_condition(test));

out_of_budget:
    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_avx2_asymm_write_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx2_asymm_write_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_avx2_asymm_write_int");
    return EXIT_SKIP;
}
static int mesh_upi_avx2_asymm_write_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx2_asymm_write_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx2_asymm_write_int,
             "Asymmetrical(1 read core from all rest mutual write cores) stress MESH and UPI or Ring Interconnect and QPI (do int32 AVX-2(YMM) loads and stores from L1/L2 to L1/L2 with switching read core) [ARM NEON version]")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx2_asymm_write_int_init,
    .test_run = mesh_upi_avx2_asymm_write_int_run,
    .test_cleanup = mesh_upi_avx2_asymm_write_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
