#include <sandstone.h>
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/sysinfo.h>

#define VECTOR_SIZE 4                     // 原负载：一次处理 4 个 int32（SVE 谓词分批）
#define BLOCK_ELEMENTS (VECTOR_SIZE * 2)  // 每个块 8 个元素（两个向量）
// 块数/总元素数在 init 中按目标内存动态计算(见 mesh_target_bytes)。

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    alignas(16) int32_t *data;           // 16 字节对齐即可
    size_t num_blocks;                   // 运行时块数(按目标内存计算)
    size_t total_elements;               // 运行时总元素数
    std::atomic<uint32_t> next_block;
    std::atomic<uint64_t> global_sum;
    std::atomic<uint32_t> allocated_blocks;
    std::atomic<uint32_t> thread_idx;
    /* sense-reversal 轮屏障（B1-001 修复，移植自 NEON 侧 2731971d）：
     * 原协议结构性互等——写者外层每轮先等 `allocated_blocks == 0 &&
     * round_done == 0`（round_done 由读者复位，读者却在等写者写满
     * NUM_BLOCKS 块），且写者每轮只写 1 块即回外层，首轮即死锁，读者
     * 校验从未执行、测试 vacuous pass；reader_count（入场注册计数）作
     * 完成判据还是移动目标。arrived/epoch 以固定参与者计数（写者 +
     * 全体读者 = thread_count()）隔离轮次，无此缺陷。 */
    std::atomic<uint32_t> arrived;       // 本轮已到达屏障的线程数（写者+读者）
    std::atomic<uint32_t> epoch;         // 轮次代（sense 位）
};
}

// 目标字节数: -O mesh_upi_sve_asymm_distrib_int.buffer_bytes=N /
// SDC_MESH_UPI_SVE_ASYMM_DISTRIB_INT_BUFFER_BYTES 环境变量 /
// 自适应 = 物理内存 90% 在 2 路 mesh 战役间均摊(与 mesh_upi_avx_sym 的
// SDC_MESH_BUFFER_BYTES 语义一致, 但走统一配置链)
static size_t mesh_target_bytes(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "buffer_bytes", 0,
                                     1 << 20, 1ll << 44);
    if (v == 0)
        sdc_env_knob_int_clamped(test->id, "buffer_bytes", &v, 1 << 20, 1ll << 44);
    if (v > 0)
        return (size_t)v;
    /* 旧脚本兼容: SDC_MESH_BUFFER_BYTES 直接指定字节 */
    if (const char *e = getenv("SDC_MESH_BUFFER_BYTES")) {
        char *end = nullptr;
        unsigned long long b = strtoull(e, &end, 10);
        if (end && *end == '\0' && b > 0)
            return (size_t)b;
    }
    return sdc_target_workset_bytes(900, 2, nullptr);
}

static int mesh_upi_sve_asymm_distrib_int_init(struct test *test) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) return -255;

    /* All-VL Phase 2 P6: per-test auxv hand-probe -> framework
     * layer: arm64_require_sve_vl does the SVE capability check
     * + 128..1024-bit range gate in one call (skip reason
     * framework-issued). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;

    const size_t elem_bytes = sizeof(int32_t);
    const size_t block_bytes = BLOCK_ELEMENTS * elem_bytes;
    size_t target = mesh_target_bytes(test);
    td->num_blocks = target / block_bytes;
    if (td->num_blocks < 1) td->num_blocks = 1;
    if (td->num_blocks > 0xFFFFFFFFu) td->num_blocks = 0xFFFFFFFFu;
    td->total_elements = td->num_blocks * BLOCK_ELEMENTS;
    size_t total_bytes = td->total_elements * elem_bytes;

    td->data = (int32_t*)aligned_alloc(16, total_bytes);
    if (!td->data) {
        delete td;
        return EXIT_FAILURE;
    }
    memset(td->data, 0, total_bytes);

    td->next_block.store(0, std::memory_order_relaxed);
    td->global_sum.store(0, std::memory_order_relaxed);
    td->allocated_blocks.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    td->arrived.store(0, std::memory_order_relaxed);
    td->epoch.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_sve_asymm_distrib_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    bool is_writer = (id == 0);

    /* 屏障参与者总数取框架权威值 thread_count()（自动尊重 --cpuset / -n /
     * test.max_threads，每个线程取值一致；2731971 修法）。原协议以
     * reader_count（入场时才自增的注册计数）为完成判据目标，孵化窗口
     * 内仍在单调增长，判据可被部分 cohort 提前满足 —— 连同
     * round_done/reader_count 协议一并移除（见 TestData 注释）。
     * 另注：NEON 参照（15895cd4）无单线程防护，-n 1 时写者独跑、读者
     * 数为 0，轮次空转 vacuous；按家族政策（556d54e3：-n 1 干净跳过
     * inter-core 测试）SVE 移植补上。 */
    uint32_t total_threads = thread_count();
    if (total_threads < 2) {
        log_skip(CpuTopologyIssueSkipCategory,
                 "mesh_upi_sve_asymm_distrib_int requires at least 2 threads (inter-core test); "
                 "skipping on this thread count");
        return EXIT_SKIP;
    }

    /* randomization hardening (B1-002, aligned with 2731971d): framework
     * RNG (per-thread stream, -s reproducible) replaces std::mt19937;
     * range [-1000000, 1000000]. */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    uint32_t my_epoch = 0;
    do {
        if (is_writer) {
            /* 写者（固定 id==0）：本轮状态已由上一轮 sense-reversal 屏障的
             * 最后到达者复位（首轮由 init 复位）——原“等
             * allocated_blocks==0 且 round_done==0”的顶屏障连同
             * round_done/reader_count 协议一并移除：它等的是读者复位，
             * 而读者在等写者写满，首轮即互等死锁。 */

            /* 串行填充所有块：有限工作量（NUM_BLOCKS×BLOCK_ELEMENTS 个
             * 元素，微秒级），不逐块消耗 fracture 预算 —— 原实现每块
             * 烧 2 次 test_time_condition，一轮 128 块远超预算，写者
             * 在填充中途预算耗尽退出。同 2731971 写者的无守卫填充。 */
            while (true) {
                uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
                if (block >= td->num_blocks) {
                    break;
                }

                size_t offset = block * BLOCK_ELEMENTS;
                int32_t vals[BLOCK_ELEMENTS];
                uint64_t local_sum = 0;
                for (int j = 0; j < BLOCK_ELEMENTS; ++j) {
                    vals[j] = dist();
                    local_sum += (uint64_t)vals[j];
                }
                /* 分两个向量存储（SVE 谓词分批）。原移植缺 offset：所有
                 * 块都写 data[0..7]，128 块互相覆盖且其余 1016 个元素
                 * 恒为 0 —— 该 bug 从未暴露，因为首轮死锁使读者校验
                 * 从未执行。 */
                for (int v = 0; v < 2; ++v) {
                    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                    svint32_t vec = svld1_s32(pg, vals + v * VECTOR_SIZE);
                    svst1_s32(pg, td->data + offset + v * VECTOR_SIZE, vec);
                }
                __sync_synchronize();  // 类似 _mm_sfence

                // 一致性测试：立即读回比较
                bool block_ok = true;
                for (int v = 0; v < 2; ++v) {
                    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                    svint32_t written = svld1_s32(pg, vals + v * VECTOR_SIZE);
                    svint32_t loaded = svld1_s32(pg, td->data + offset + v * VECTOR_SIZE);
                    svbool_t cmp = svcmpeq_s32(pg, written, loaded);
                    if (svcntp_b32(pg, cmp) != VECTOR_SIZE) block_ok = false;
                }
                /* 写者的立即读回是唯一逐字节的写入内容校验——读核心的
                 * sum 校验可被保和损坏（如元素交换）绕过，所以 block_ok
                 * 为假必须直接判失败并保留证据，不能丢弃结果（原实现
                 * 此处无读回校验；同 87ebc4b / 8502e491 SVE-wide 的修法）。 */
                if (!block_ok) {
                    fprintf(stderr, "mesh_upi_sve_asymm_distrib_int: Thread %d (writer), block %u immediate read-back mismatch, written vals[0..7]=(%d,%d,%d,%d,%d,%d,%d,%d), loaded data[0..7] at offset %zu=(%d,%d,%d,%d,%d,%d,%d,%d), result=%sFAIL%s\n",
                            id, block,
                            vals[0], vals[1], vals[2], vals[3],
                            vals[4], vals[5], vals[6], vals[7],
                            offset,
                            td->data[offset], td->data[offset + 1],
                            td->data[offset + 2], td->data[offset + 3],
                            td->data[offset + 4], td->data[offset + 5],
                            td->data[offset + 6], td->data[offset + 7],
                            RED, RESET);
                    fflush(stderr);
                    report_fail_msg("mesh_upi_sve_asymm_distrib_int: writer immediate read-back mismatch (store/load corruption)");
                    return EXIT_FAILURE;
                }

                td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
                td->allocated_blocks.fetch_add(1, std::memory_order_seq_cst);
            }
        } else {
            /* 读核心：等待所有块分配完成。原实现每次自旋都调
             * test_time_condition：读者 40 次预算在微秒级就被自旋
             * 烧光，等待提前中断 → 校验被跳过 → vacuous pass。
             * 按 2731971 已验证的节流模式：等待条件每自旋检查、
             * ttc 预算每 1024 自旋检查一次，预算耗尽走 out_of_budget
             * 干净退出。 */
            uint32_t spin = 0;
            while (td->allocated_blocks.load(std::memory_order_seq_cst) < td->num_blocks) {
                if ((++spin & 1023u) == 0) {
                    if (!test_time_condition(test)) goto out_of_budget;
                } else {
                    __asm__ volatile("yield");
                }
            }

            // 全内存屏障
            __sync_synchronize();

            /* 全数组遍历校验。原移植只读 data[0..7]（首块 8 元素，无偏移
             * 遍历）却与全数组 128 块的 expected_sum 比较 —— 结构性恒
             * 失败，被首轮死锁掩盖而从未暴露。 */
            uint64_t read_sum = 0;
            bool consistent = true;
            alignas(16) int32_t store_buf[BLOCK_ELEMENTS];

            for (size_t i = 0; i < td->total_elements; i += BLOCK_ELEMENTS) {
                int32_t vals[BLOCK_ELEMENTS];
                for (int v = 0; v < 2; ++v) {
                    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                    svint32_t vec = svld1_s32(pg, td->data + i + v * VECTOR_SIZE);
                    svst1_s32(pg, vals + v * VECTOR_SIZE, vec);
                }
                for (int j = 0; j < BLOCK_ELEMENTS; ++j) {
                    read_sum += (uint64_t)vals[j];
                }

                // 一致性测试：Store/Load 回读比较
                for (int v = 0; v < 2; ++v) {
                    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                    svint32_t orig = svld1_s32(pg, vals + v * VECTOR_SIZE);
                    svst1_s32(pg, store_buf + v * VECTOR_SIZE, orig);
                    svint32_t reload = svld1_s32(pg, store_buf + v * VECTOR_SIZE);
                    svbool_t cmp = svcmpeq_s32(pg, orig, reload);
                    if (svcntp_b32(pg, cmp) != VECTOR_SIZE)
                        consistent = false;
                }
            }

            uint64_t expected_sum = td->global_sum.load(std::memory_order_seq_cst);
            bool passed = (read_sum == expected_sum) && consistent;

            if (!passed) {
                // 首次失败证据（原先每轮都打印 PASS 行；与 Task 5 家族统一移入失败分支）
                fprintf(stderr, "mesh_upi_sve_asymm_distrib_int: Thread %d (reader), data[0..7]=(%d,%d,%d,%d,%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, consistent=%d, result=%s%s%s\n",
                        id,
                        td->data[0], td->data[1], td->data[2], td->data[3],
                        td->data[4], td->data[5], td->data[6], td->data[7],
                        read_sum, expected_sum, consistent,
                        passed ? GREEN : RED,
                        passed ? "PASS" : "FAIL",
                        RESET);
                fflush(stderr);
                /* cold path: record bit-level evidence for the failing leg(s) */
                if (read_sum != expected_sum)
                    log_error("mesh_upi_sve_asymm_distrib_int: sum mismatch: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                              (unsigned long long)expected_sum, (unsigned long long)read_sum,
                              (unsigned long long)(expected_sum ^ read_sum));
                report_fail_msg("mesh_upi_sve_asymm_distrib_int: Sum mismatch or consistency failure");
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

static int mesh_upi_sve_asymm_distrib_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_sve_asymm_distrib_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_sve_asymm_distrib_int");
    return EXIT_SKIP;
}
static int mesh_upi_sve_asymm_distrib_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_sve_asymm_distrib_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_sve_asymm_distrib_int,
             "Asymmetrical(1 write core to all rest distributed read cores) stress MESH and UPI or Ring Interconnect and QPI (do int32 SVE loads and stores from L1/L2 to L1/L2 with switching write core) [ARM SVE version]")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sve_asymm_distrib_int_init,
    .test_run = mesh_upi_sve_asymm_distrib_int_run,
    .test_cleanup = mesh_upi_sve_asymm_distrib_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
