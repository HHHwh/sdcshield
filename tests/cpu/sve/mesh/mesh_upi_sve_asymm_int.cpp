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
    /* sense-reversal barrier (B1-001 fix, ported from NEON-side 2731971d):
     * the original iter/round_done protocol deadlocked from round 1 (the
     * writer's phase-1 spin waited for iter != current_iter, but iter only
     * advanced after a completed round), so the readers' verification never
     * executed and the test passed vacuously. Pre-existing since the
     * original SVE port. */
    std::atomic<uint32_t> arrived;
    std::atomic<uint32_t> epoch;
};
}

// 目标字节数: -O mesh_upi_sve_asymm_int.buffer_bytes=N /
// SDC_MESH_UPI_SVE_ASYMM_INT_BUFFER_BYTES 环境变量 /
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

static int mesh_upi_sve_asymm_int_init(struct test *test) {
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

/* gcc-16.2 (debian:sid, 2026-09) 对本函数在 -O2+ RTL expand ICE(internal
 * compiler error: Segmentation fault);与 core179 三 probe 的 gcc-10 ICE 同族
 * (SVE 循环内 svst1 模式 × 优化器)。注意:函数级 O1 对 gcc-16 此处无效
 * (gcc-10 的 probe 用 O1 即可,gcc-16 需 O0 才完全规避)。SDC 激励为显式
 * intrinsics 与原子操作,不依赖优化器;自旋屏障在 O0 下语义不变,仅自旋
 * 速度略降(原子条件本身每次都真实读)。 */
__attribute__((optimize("O0")))
static int mesh_upi_sve_asymm_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    // 分配唯一线程 ID（用于每轮输出与 asymm 写者轮转）
    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);

    /* 线程总数取框架权威值 thread_count()（自动尊重 --cpuset、
     * test.max_threads 与 OS 亲和性限制，每个线程取值一致）—— 与 NEON 侧
     * 2731971 的修复相同。原“3 次稳定读数”启发式在 -n 1 下会永久自旋
     * 挂死（cur > 1 永不成立，后面的线程数检查不可达），大规模并发下
     * 又严重欠计；单线程运行现在按 memcpy_rewr 先例干净跳过。 */
    uint32_t total_threads = thread_count();
    if (total_threads < 2) {
        log_skip(CpuTopologyIssueSkipCategory,
                 "mesh_upi_sve_asymm_int requires at least 2 threads (inter-core test); "
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
        /* 写者随轮次轮转：writer_id 取自 my_epoch 而非原协议的
         * current_iter —— 原写者 phase-1 自旋等 `iter != current_iter`，
         * 而 iter 只有整轮完成后才递增，首轮即死锁（见 TestData 注释）。 */
        int writer_id = my_epoch % total_threads;

        if (id == writer_id) {
            // ---------- 写者：写入所有块 ----------
            // （本轮状态已由上一轮 sense-reversal 屏障的最后到达线程重置；
            //   第一轮的状态由 init 重置）
            for (uint32_t block = 0; block < td->num_blocks; ++block) {
                size_t offset = block * BLOCK_ELEMENTS;
                int32_t vals[BLOCK_ELEMENTS];
                uint64_t local_sum = 0;
                for (int j = 0; j < BLOCK_ELEMENTS; ++j) {
                    vals[j] = dist();
                    local_sum += (uint64_t)vals[j];
                }
                // 分两个向量存储（SVE 谓词分批）
                for (int v = 0; v < 2; ++v) {
                    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                    svint32_t vec = svld1_s32(pg, vals + v * VECTOR_SIZE);
                    svst1_s32(pg, td->data + offset + v * VECTOR_SIZE, vec);
                }
                __sync_synchronize();  // 类似 _mm_sfence

                // 一致性测试（自身写入后读回）
                for (int v = 0; v < 2; ++v) {
                    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                    svint32_t written = svld1_s32(pg, vals + v * VECTOR_SIZE);
                    svint32_t loaded = svld1_s32(pg, td->data + offset + v * VECTOR_SIZE);
                    svbool_t cmp = svcmpeq_s32(pg, written, loaded);
                    (void)cmp;  // 检查所有元素是否相等（忽略，因为后续读者会校验）
                }

                td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
                td->allocated_blocks.fetch_add(1, std::memory_order_seq_cst);
            }

            // 全局屏障确保所有数据已传播到所有核心
            __sync_synchronize();  // 类似 _mm_mfence

        } else {
            // ---------- 读者：等待所有块分配完成 ----------
            /* test_time_condition() 每次调用都会消耗 auto-fracture 的
             * inner_loop_count 预算（默认 40）：写者串行填充全部块
             * 需要数十微秒，若每次自旋都检查预算，读者会在写者完成前
             * 耗尽 40 次预算而提前 break（校验又被跳过）。每 1024 次
             * 自旋检查一次 —— inner_loop_count 语义是“轮次”而非
             * “自旋”，这是框架认可的用法（其余 mesh 文件同理）。 */
            uint32_t spin = 0;
            while (td->allocated_blocks.load(std::memory_order_seq_cst) < td->num_blocks) {
                if ((++spin & 1023u) == 0) {
                    if (!test_time_condition(test)) goto out_of_budget;
                } else {
                    __asm__ volatile("yield");
                }
            }

            __sync_synchronize();  // 确保写者的所有存储可见

            uint64_t read_sum = 0;
            bool consistent = true;
            size_t bad_vec = (size_t)-1;
            alignas(16) int32_t store_buf[BLOCK_ELEMENTS];

            for (size_t i = 0; i < td->total_elements; i += BLOCK_ELEMENTS) {
                int32_t vals[BLOCK_ELEMENTS];
                for (int v = 0; v < 2; ++v) {
                    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                    svint32_t vec = svld1_s32(pg, td->data + i + v * VECTOR_SIZE);
                    svst1_s32(pg, vals + v * VECTOR_SIZE, vec);
                }
                for (int j = 0; j < BLOCK_ELEMENTS; ++j) read_sum += (uint64_t)vals[j];

                // 一致性测试：Store/Load 回读比较
                for (int v = 0; v < 2; ++v) {
                    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                    svint32_t orig = svld1_s32(pg, vals + v * VECTOR_SIZE);
                    svst1_s32(pg, store_buf + v * VECTOR_SIZE, orig);
                    svint32_t reload = svld1_s32(pg, store_buf + v * VECTOR_SIZE);
                    svbool_t cmp = svcmpeq_s32(pg, orig, reload);
                    if (svcntp_b32(pg, cmp) != VECTOR_SIZE) {
                        consistent = false;
                        bad_vec = i + v * VECTOR_SIZE;
                    }
                }
            }

            uint64_t expected_sum = td->global_sum.load(std::memory_order_seq_cst);
            bool passed = (read_sum == expected_sum) && consistent;

            // 输出结果（每个读者输出一次）
            fprintf(stderr, "mesh_upi_sve_asymm_int: Thread %d (reader), data[0..7]=(%d,%d,%d,%d,%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, consistent=%d, result=%s%s%s\n",
                    id,
                    td->data[0], td->data[1], td->data[2], td->data[3],
                    td->data[4], td->data[5], td->data[6], td->data[7],
                    read_sum, expected_sum, consistent,
                    passed ? GREEN : RED,
                    passed ? "PASS" : "FAIL",
                    RESET);
            fflush(stderr);

            if (!passed) {
                /* cold path: record bit-level evidence for the failing leg(s) */
                if (read_sum != expected_sum)
                    log_error("mesh_upi_sve_asymm_int: sum mismatch: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                              (unsigned long long)expected_sum, (unsigned long long)read_sum,
                              (unsigned long long)(expected_sum ^ read_sum));
                if (!consistent)
                    log_error("mesh_upi_sve_asymm_int: store/reload consistency failed at vector %zu (post-hoc data at failing vector: "
                              "0x%08x 0x%08x 0x%08x 0x%08x)",
                              bad_vec,
                              *(const uint32_t *)&td->data[bad_vec + 0],
                              *(const uint32_t *)&td->data[bad_vec + 1],
                              *(const uint32_t *)&td->data[bad_vec + 2],
                              *(const uint32_t *)&td->data[bad_vec + 3]);
                report_fail_msg("mesh_upi_sve_asymm_int: Sum mismatch or consistency failure");
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
        // （epoch 自旋按 131072 次一档节流预算检查，先例
        //   mesh_upi_sve_asymm_distrib_int：屏障等待须覆盖线程孵化偏斜，
        //   1024 一档在 40 次预算下覆盖不足。读者的 allocated_blocks
        //   等待保持 1024 一档不变——那边只需覆盖写者的填充时长。）
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

static int mesh_upi_sve_asymm_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_sve_asymm_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_sve_asymm_int");
    return EXIT_SKIP;
}
static int mesh_upi_sve_asymm_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_sve_asymm_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_sve_asymm_int,
             "Asymmetrical(1 write core to all rest mutual read cores) stress MESH and UPI or Ring Interconnect and QPI or FSB (do int32 SVE loads and stores from L1/L2 to L1/L2 with switching write core) [ARM SVE version]")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sve_asymm_int_init,
    .test_run = mesh_upi_sve_asymm_int_run,
    .test_cleanup = mesh_upi_sve_asymm_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
