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

#define VECTOR_SIZE 4                     // 原负载：一次处理 4 个元素（SVE 谓词分批）
#define BLOCK_ELEMENTS 16                 // 每个块 16 个元素（原由 4 个 NEON 向量组成）
#define VECTORS_PER_BLOCK (BLOCK_ELEMENTS / VECTOR_SIZE)  // = 4
// 块数/总元素数在 init 中按目标内存动态计算(见 mesh_target_bytes)。

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    alignas(16) float *data;           // 16 字节对齐即可
    size_t num_blocks;                 // 运行时块数(按目标内存计算)
    size_t total_elements;             // 运行时总元素数
    std::atomic<uint32_t> next_block;
    std::atomic<uint64_t> global_sum;   // 未使用，但保留（与原版一致）
    std::atomic<uint32_t> allocated_blocks;
    std::atomic<uint32_t> thread_idx;
    /* sense-reversal barrier (B1-001 fix, ported from NEON-side 2731971d):
     * the original iter/round_done protocol deadlocked from round 1
     * (round_done was only reset, never used as a release signal that any
     * thread could observe before phase 3; iter only advanced after a
     * completed round), so the verification never executed and the test
     * passed vacuously. */
    std::atomic<uint32_t> arrived;
    std::atomic<uint32_t> epoch;
};
}

// 目标字节数: -O mesh_upi_sve_wide_sym.buffer_bytes=N /
// SDC_MESH_UPI_SVE_WIDE_SYM_BUFFER_BYTES 环境变量 /
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

static int mesh_upi_sve_wide_sym_init(struct test *test) {
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

    const size_t elem_bytes = sizeof(float);
    const size_t block_bytes = BLOCK_ELEMENTS * elem_bytes;
    size_t target = mesh_target_bytes(test);
    td->num_blocks = target / block_bytes;
    if (td->num_blocks < 1) td->num_blocks = 1;
    if (td->num_blocks > 0xFFFFFFFFu) td->num_blocks = 0xFFFFFFFFu;
    td->total_elements = td->num_blocks * BLOCK_ELEMENTS;
    size_t total_bytes = td->total_elements * elem_bytes;

    td->data = (float*)aligned_alloc(16, total_bytes);
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

static int mesh_upi_sve_wide_sym_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);

    /* 线程总数取框架权威值 thread_count()（自动尊重 --cpuset、
     * test.max_threads 与 OS 亲和性限制，每个线程取值一致）—— 与 NEON 侧
     * 2731971 的修复相同。原“3 次稳定读数”启发式在 -n 1 下会永久自旋
     * 挂死（cur > 1 永不成立，后面的线程数检查不可达），大规模并发下
     * 又严重欠计；单线程运行现在按 memcpy_rewr 先例干净跳过。 */
    uint32_t total_threads = thread_count();
    if (total_threads < 2) {
        log_skip(CpuTopologyIssueSkipCategory,
                 "mesh_upi_sve_wide_sym requires at least 2 threads (inter-core test); "
                 "skipping on this thread count");
        return EXIT_SKIP;
    }

    /* randomization hardening (B1-002, aligned with 2731971d): framework
     * RNG (per-thread stream, -s reproducible) replaces std::mt19937;
     * range [-1000, 1000). */
    auto dist = []() { return frandomf_scale(2000.0f) - 1000.0f; };

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    uint32_t my_epoch = 0;
    do {
        // ---------- 阶段 1：所有线程共同填充数据 ----------
        // （本轮状态已由上一轮 sense-reversal 屏障的最后到达线程重置）

        // 所有线程竞争分配块并写入
        while (true) {
            uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
            if (block >= td->num_blocks) break;
            size_t offset = block * BLOCK_ELEMENTS;
            float vals[BLOCK_ELEMENTS];
            double local_sum = 0.0;
            for (int j = 0; j < BLOCK_ELEMENTS; ++j) {
                vals[j] = dist();
                local_sum += (double)vals[j];
            }
            // 使用 4 个向量存储 16 个元素（SVE 谓词分批）
                    {
                        svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                        svfloat32_t vec = svld1_f32(pg, vals + 0 * VECTOR_SIZE);
                        svst1_f32(pg, td->data + offset + 0 * VECTOR_SIZE, vec);
                    }
                    {
                        svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                        svfloat32_t vec = svld1_f32(pg, vals + 1 * VECTOR_SIZE);
                        svst1_f32(pg, td->data + offset + 1 * VECTOR_SIZE, vec);
                    }
                    {
                        svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                        svfloat32_t vec = svld1_f32(pg, vals + 2 * VECTOR_SIZE);
                        svst1_f32(pg, td->data + offset + 2 * VECTOR_SIZE, vec);
                    }
                    {
                        svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                        svfloat32_t vec = svld1_f32(pg, vals + 3 * VECTOR_SIZE);
                        svst1_f32(pg, td->data + offset + 3 * VECTOR_SIZE, vec);
                    }

            __sync_synchronize();   // 确保写入对其他核心可见

            // 一致性测试：立即读回比较
            bool block_ok = true;
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                svfloat32_t written = svld1_f32(pg, vals + v * VECTOR_SIZE);
                svfloat32_t loaded = svld1_f32(pg, td->data + offset + v * VECTOR_SIZE);
                svbool_t cmp = svcmpeq_f32(pg, written, loaded);
                if (svcntp_b32(pg, cmp) != VECTOR_SIZE) {
                    block_ok = false;
                    break;
                }
            }
            /* 写阶段的立即读回是本测试唯一逐字节的写入内容校验——读阶段
             * 只做 store_buf 自一致性比较，不对照写入值，所以 block_ok
             * 为假必须直接判失败，不能只记录（同 87ebc4b 的修法）。 */
            if (!block_ok) {
                fprintf(stderr, "mesh_upi_sve_wide_sym: Thread %d, block %u immediate read-back mismatch, written vals[0..3]=(%.6f,%.6f,%.6f,%.6f), loaded data[0..3] at offset %zu=(%.6f,%.6f,%.6f,%.6f), result=%sFAIL%s\n",
                        id, block,
                        vals[0], vals[1], vals[2], vals[3],
                        offset,
                        td->data[offset], td->data[offset + 1],
                        td->data[offset + 2], td->data[offset + 3],
                        RED, RESET);
                fflush(stderr);
                report_fail_msg("mesh_upi_sve_wide_sym: write-phase immediate read-back mismatch (store/load corruption)");
                return EXIT_FAILURE;
            }

            (void)local_sum;  // 原版算了 local_sum 但未累加到 global_sum（字段保留未用）
            td->allocated_blocks.fetch_add(1, std::memory_order_seq_cst);
        }

        /* 等待所有线程完成写入：与阶段 3 屏障等待同款 131072 一档节流
         * 预算检查（CodeRabbit 意见，先例 mesh_upi_sve_asymm_distrib_int
         * 的 epoch 等待）：每次自旋都调 test_time_condition 会迅速耗尽
         * inner_loop_count 预算（默认 40 次），把等待压成提前退出、
         * 跳过阶段 2 校验及屏障；预算耗尽走 out_of_budget 干净退出，
         * 不再在循环外重复消耗一次预算。等待条件本身每自旋都查，
         * 响应不受影响。 */
        {
            uint32_t spin = 0;
            while (td->allocated_blocks.load(std::memory_order_seq_cst) < td->num_blocks) {
                if ((++spin & 131071u) == 0) {
                    if (!test_time_condition(test)) goto out_of_budget;
                } else {
                    __asm__ volatile("yield");
                }
            }
        }

        __sync_synchronize();

        // 阶段 2：读取并验证
        bool consistent = true;
        size_t bad_vec = (size_t)-1;
        alignas(16) float store_buf[BLOCK_ELEMENTS];

        for (size_t i = 0; i < td->total_elements; i += BLOCK_ELEMENTS) {
            float vals[BLOCK_ELEMENTS];
                    {
                        svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                        svfloat32_t vec = svld1_f32(pg, td->data + i + 0 * VECTOR_SIZE);
                        svst1_f32(pg, vals + 0 * VECTOR_SIZE, vec);
                    }
                    {
                        svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                        svfloat32_t vec = svld1_f32(pg, td->data + i + 1 * VECTOR_SIZE);
                        svst1_f32(pg, vals + 1 * VECTOR_SIZE, vec);
                    }
                    {
                        svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                        svfloat32_t vec = svld1_f32(pg, td->data + i + 2 * VECTOR_SIZE);
                        svst1_f32(pg, vals + 2 * VECTOR_SIZE, vec);
                    }
                    {
                        svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                        svfloat32_t vec = svld1_f32(pg, td->data + i + 3 * VECTOR_SIZE);
                        svst1_f32(pg, vals + 3 * VECTOR_SIZE, vec);
                    }


            // 一致性测试：Store/Load 比较
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                svfloat32_t orig = svld1_f32(pg, vals + v * VECTOR_SIZE);
                svst1_f32(pg, store_buf + v * VECTOR_SIZE, orig);
            }
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                svfloat32_t orig = svld1_f32(pg, vals + v * VECTOR_SIZE);
                svfloat32_t reload = svld1_f32(pg, store_buf + v * VECTOR_SIZE);
                svbool_t cmp = svcmpeq_f32(pg, orig, reload);
                if (svcntp_b32(pg, cmp) != VECTOR_SIZE) {
                    consistent = false;
                    bad_vec = i + v * VECTOR_SIZE;
                    break;
                }
            }
        }

        bool passed = consistent;  // 原版 avx512_sym 仅校验一致性（store/reload），不比对求和

        fprintf(stderr, "mesh_upi_sve_wide_sym: Thread %d, data[0..15]=(%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f), consistent=%d, result=%s%s%s\n",
                id,
                td->data[0], td->data[1], td->data[2], td->data[3],
                td->data[4], td->data[5], td->data[6], td->data[7],
                td->data[8], td->data[9], td->data[10], td->data[11],
                td->data[12], td->data[13], td->data[14], td->data[15],
                consistent,
                passed ? GREEN : RED,
                passed ? "PASS" : "FAIL",
                RESET);
        fflush(stderr);

        if (!passed) {
            /* cold path: record bit-level evidence for the failing leg(s) */
            if (!consistent)
                log_error("mesh_upi_sve_wide_sym: store/reload consistency failed at vector %zu (post-hoc data at failing vector: "
                          "0x%08x 0x%08x 0x%08x 0x%08x)",
                          bad_vec,
                          *(const uint32_t *)&td->data[bad_vec + 0],
                          *(const uint32_t *)&td->data[bad_vec + 1],
                          *(const uint32_t *)&td->data[bad_vec + 2],
                          *(const uint32_t *)&td->data[bad_vec + 3]);
            report_fail_msg("mesh_upi_sve_wide_sym: Sum mismatch or consistency failure");
            return EXIT_FAILURE;
        }

        // ---------- 阶段 3：sense-reversal 屏障进入下一轮 ----------
        // 最后到达的线程在此重置下一轮状态（global_sum/next_block/
        // allocated_blocks）：fetch_add(acq_rel) 的到达顺序保证所有线程
        // 都已离开阶段 2 的校验，epoch 的 release 存储保证重置先于任何
        // 线程的下一轮填充可见 —— 这正是原协议想做而没做对的轮次隔离。
        uint32_t done = td->arrived.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (done == total_threads) {
            td->global_sum.store(0, std::memory_order_seq_cst);
            td->next_block.store(0, std::memory_order_seq_cst);
            td->allocated_blocks.store(0, std::memory_order_seq_cst);
            td->arrived.store(0, std::memory_order_relaxed);
            td->epoch.store(my_epoch + 1, std::memory_order_release);
        } else {
            /* 自旋按 131072 一档节流预算检查（CodeRabbit 意见，先例
             * mesh_upi_sve_asymm_distrib_int）：每次自旋都调
             * test_time_condition 会迅速耗尽 inner_loop_count 预算
             * （默认 40 次），把屏障等待压成 vacuous 退出；等待条件
             * 本身每自旋都查，响应不受影响。 */
            uint32_t spin = 0;
            while (td->epoch.load(std::memory_order_acquire) == my_epoch) {
                if ((++spin & 131071u) == 0) {
                    if (!test_time_condition(test)) goto out_of_budget;
                } else {
                    __asm__ volatile("yield");
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

static int mesh_upi_sve_wide_sym_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_sve_wide_sym_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_sve_wide_sym");
    return EXIT_SKIP;
}
static int mesh_upi_sve_wide_sym_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_sve_wide_sym_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_sve_wide_sym,
             "Symmetrical stress … do SVE loads and stores from L1D to L1D with different directions and #cores (ARM SVE wide-vector version)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sve_wide_sym_init,
    .test_run = mesh_upi_sve_wide_sym_run,
    .test_cleanup = mesh_upi_sve_wide_sym_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
