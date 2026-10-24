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
/* 执行期勘误（B1-001 修复时发现）：原移植为 (VECTOR_SIZE * 2)，但写阶段
 * 每块只存 1 个向量（4 元素）——块距 8、半数元素从未被 SVE store 覆盖
 * （全靠 memset 的 0 补偿求和）。改回 VECTOR_SIZE（与 NEON 侧 sse_sym
 * 参照一致），全部元素进入写入路径。块数/总元素数在 init 中按目标内存
 * 动态计算(见 mesh_target_bytes)。 */
#define BLOCK_ELEMENTS VECTOR_SIZE
// 块数/总元素数在 init 中按目标内存动态计算(见 mesh_target_bytes)。

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    alignas(16) float *data;               // 16 字节对齐即可（原版 avx_sym 为 float）
    size_t num_blocks;                     // 运行时块数(按目标内存计算)
    size_t total_elements;                 // 运行时总元素数
    std::atomic<uint32_t> next_block;
    std::atomic<double> global_sum;      // 用 double 累加，减少误差
    std::atomic<uint32_t> allocated_blocks;
    std::atomic<uint32_t> thread_idx;
    /* sense-reversal barrier (B1-001 fix, ported from NEON-side 2731971d):
     * the original iter/read_done/round_done protocol deadlocked from
     * round 1 (read_done was never set to 1; iter only advanced after a
     * completed round), so the sum check never executed and the test
     * passed vacuously. */
    std::atomic<uint32_t> arrived;
    std::atomic<uint32_t> epoch;
};
}

// 目标字节数: -O mesh_upi_sve_sym.buffer_bytes=N /
// SDC_MESH_UPI_SVE_SYM_BUFFER_BYTES 环境变量 /
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

static int mesh_upi_sve_sym_init(struct test *test) {
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
    td->global_sum.store(0.0, std::memory_order_relaxed);
    td->allocated_blocks.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    td->arrived.store(0, std::memory_order_relaxed);
    td->epoch.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_sve_sym_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    // 分配唯一线程 ID
    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);

    /* 线程总数取框架权威值 thread_count()（自动尊重 --cpuset、
     * test.max_threads 与 OS 亲和性限制，每个线程取值一致）—— 与 NEON 侧
     * 2731971 的修复相同。原“3 次稳定读数”启发式在 -n 1 下会永久自旋
     * 挂死（cur > 1 永不成立，后面的线程数检查不可达），大规模并发下
     * 又严重欠计；单线程运行现在按 memcpy_rewr 先例干净跳过。 */
    uint32_t total_threads = thread_count();
    if (total_threads < 2) {
        log_skip(CpuTopologyIssueSkipCategory,
                 "mesh_upi_sve_sym requires at least 2 threads (inter-core test); "
                 "skipping on this thread count");
        return EXIT_SKIP;
    }

    /* randomization hardening (B1-002, aligned with 2731971d): framework
     * RNG (per-thread stream, -s reproducible) replaces std::mt19937.
     * 取整数值 float：大工作集下 double 求和仍精确(整数 <=2^53 无舍入)，
     * 保证 read_sum == expected_sum 在任意块序下都成立，避免放大工作集
     * 后因浮点求和次序差异产生假 Sum mismatch (与 mesh_upi_avx_sym 的
     * H14' 同修法)。 */
    auto dist = []() { return (float)(int32_t)(frandomf_scale(2000.0f) - 1000.0f); };

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    uint32_t my_epoch = 0;
    do {
        // ---------- 阶段 1：所有线程共同填充数据 ----------
        // （本轮状态已由上一轮 sense-reversal 屏障的最后到达线程重置）

        // 所有线程竞争分配块并写入随机浮点数据
        while (true) {
            uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
            if (block >= td->num_blocks) break;
            size_t offset = block * BLOCK_ELEMENTS;
            float vals[VECTOR_SIZE];
            double local_sum = 0.0;
            for (int j = 0; j < VECTOR_SIZE; ++j) {
                vals[j] = dist();
                local_sum += (double)vals[j];
            }
            // SVE 向量存储（等价原 NEON vld1q_f32/vst1q_f32）
            svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
            svfloat32_t v = svld1_f32(pg, vals);
            svst1_f32(pg, td->data + offset, v);
            __sync_synchronize();              // 等效于 _mm_sfence

            // 一致性测试：立即读回并比较
            svfloat32_t loaded = svld1_f32(pg, td->data + offset);
            svbool_t cmp = svcmpeq_f32(pg, v, loaded);
            bool ok = (svcntp_b32(pg, cmp) == VECTOR_SIZE);
            if (!ok) {
                // 不一致，但最终会在全量校验时捕获，这里不处理
            }

            // 累加全局和（使用 double 原子操作）
            double old_sum = td->global_sum.load(std::memory_order_relaxed);
            while (!td->global_sum.compare_exchange_weak(old_sum, old_sum + local_sum,
                                                         std::memory_order_seq_cst,
                                                         std::memory_order_relaxed)) {}
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

        __sync_synchronize();  // 确保写入全局可见（mfence）

        // ---------- 阶段 2：所有线程读取完整数组并验证 ----------
        double read_sum = 0.0;
        bool consistent = true;
        size_t bad_vec = (size_t)-1;
        alignas(16) float store_buf[VECTOR_SIZE];

        for (size_t i = 0; i < td->total_elements; i += VECTOR_SIZE) {
            svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
            svfloat32_t v = svld1_f32(pg, td->data + i);
            float vals[VECTOR_SIZE];
            svst1_f32(pg, vals, v);
            for (int j = 0; j < VECTOR_SIZE; ++j) read_sum += (double)vals[j];

            // 一致性测试：Store/Load 回读比较
            svst1_f32(pg, store_buf, v);
            svfloat32_t reload = svld1_f32(pg, store_buf);
            svbool_t cmp = svcmpeq_f32(pg, v, reload);
            if (svcntp_b32(pg, cmp) != VECTOR_SIZE) {
                consistent = false;
                bad_vec = i;
            }
        }

        double expected_sum = td->global_sum.load(std::memory_order_seq_cst);
        bool passed = (read_sum == expected_sum) && consistent;

        // 输出结果（每个线程输出一次）
        fprintf(stderr, "mesh_upi_sve_sym: Thread %d, data[0..3]=(%.4f,%.4f,%.4f,%.4f), read_sum=%f, expected_sum=%f, consistent=%d, result=%s%s%s\n",
                id,
                td->data[0], td->data[1], td->data[2], td->data[3],
                read_sum, expected_sum, consistent,
                passed ? GREEN : RED,
                passed ? "PASS" : "FAIL",
                RESET);
        fflush(stderr);

        if (!passed) {
            /* cold path: record bit-level evidence for the failing leg(s) */
            if (read_sum != expected_sum) {
                uint64_t _dump_es, _dump_rs;
                memcpy(&_dump_es, &expected_sum, 8);
                memcpy(&_dump_rs, &read_sum, 8);
                log_error("mesh_upi_sve_sym: sum mismatch: golden=0x%016llx (%g) actual=0x%016llx (%g) xor=0x%016llx",
                          (unsigned long long)_dump_es, expected_sum,
                          (unsigned long long)_dump_rs, read_sum,
                          (unsigned long long)(_dump_es ^ _dump_rs));
            }
            if (!consistent)
                log_error("mesh_upi_sve_sym: store/reload consistency failed at vector %zu (post-hoc data at failing vector: "
                          "0x%08x 0x%08x 0x%08x 0x%08x)",
                          bad_vec,
                          *(const uint32_t *)&td->data[bad_vec + 0],
                          *(const uint32_t *)&td->data[bad_vec + 1],
                          *(const uint32_t *)&td->data[bad_vec + 2],
                          *(const uint32_t *)&td->data[bad_vec + 3]);
            report_fail_msg("mesh_upi_sve_sym: Sum mismatch or consistency failure");
            return EXIT_FAILURE;
        }

        // ---------- 阶段 3：sense-reversal 屏障进入下一轮 ----------
        // 最后到达的线程在此重置下一轮状态（global_sum/next_block/
        // allocated_blocks）：fetch_add(acq_rel) 的到达顺序保证所有线程
        // 都已离开阶段 2 的校验，epoch 的 release 存储保证重置先于任何
        // 线程的下一轮填充可见 —— 这正是原协议想做而没做对的轮次隔离。
        uint32_t done = td->arrived.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (done == total_threads) {
            td->global_sum.store(0.0, std::memory_order_seq_cst);
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

static int mesh_upi_sve_sym_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_sve_sym_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_sve_sym");
    return EXIT_SKIP;
}
static int mesh_upi_sve_sym_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_sve_sym_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_sve_sym,
             "Symmetrical stress … do AVX loads and stores from L1D to L1D with different directions and switching cores (ARM SVE version)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sve_sym_init,
    .test_run = mesh_upi_sve_sym_run,
    .test_cleanup = mesh_upi_sve_sym_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
