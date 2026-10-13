#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#ifdef __aarch64__
#include <arm_sve.h>
#include <sys/auxv.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/sysinfo.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif

#define TOTAL_ELEMENTS 1024
#define VECTOR_SIZE 4                     // 原负载：一次处理 4 个 int32（SVE 谓词分批）
#define BLOCK_ELEMENTS (VECTOR_SIZE * 2)  // 每个块 8 个元素（两个向量）
#define NUM_BLOCKS (TOTAL_ELEMENTS / BLOCK_ELEMENTS)  // = 128
#define TOTAL_BYTES (TOTAL_ELEMENTS * sizeof(float))

struct TestData {
    alignas(16) float *data;               // 16 字节对齐即可（原版 avx_sym 为 float）
    std::atomic<uint32_t> next_block;
    std::atomic<double> global_sum;      // 用 double 累加，减少误差
    std::atomic<uint32_t> allocated_blocks;
    std::atomic<uint32_t> thread_idx;
    std::atomic<uint32_t> round_done;
    std::atomic<uint64_t> iter;
    std::atomic<uint32_t> read_done;
};

static int mesh_upi_sve_sym_init(struct test *test) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) return -255;

    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for mesh_upi_sve_sym");
        return EXIT_SKIP;
    }

    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;

    td->data = (float*)aligned_alloc(16, TOTAL_BYTES);
    if (!td->data) {
        delete td;
        return EXIT_FAILURE;
    }
    memset(td->data, 0, TOTAL_BYTES);

    td->next_block.store(0, std::memory_order_relaxed);
    td->global_sum.store(0.0, std::memory_order_relaxed);
    td->allocated_blocks.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    td->round_done.store(0, std::memory_order_relaxed);
    td->iter.store(0, std::memory_order_relaxed);
    td->read_done.store(0, std::memory_order_relaxed);
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

    std::mt19937 rng(std::random_device{}());
    std::uniform_real_distribution<float> dist(-1000.0f, 1000.0f);

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        // ---------- 阶段 1：所有线程共同填充数据 ----------
        uint64_t current_iter = td->iter.load(std::memory_order_acquire);
        while ((td->read_done.load(std::memory_order_seq_cst) != 0 ||
                td->iter.load(std::memory_order_acquire) == current_iter) &&
               test_time_condition(test)) {
            __asm__ volatile("yield");
        }
        if (!test_time_condition(test)) break;

        // 重置状态
        td->global_sum.store(0.0, std::memory_order_seq_cst);
        td->next_block.store(0, std::memory_order_seq_cst);
        td->allocated_blocks.store(0, std::memory_order_seq_cst);
        td->round_done.store(0, std::memory_order_seq_cst);

        // 所有线程竞争分配块并写入随机浮点数据
        while (true) {
            uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
            if (block >= NUM_BLOCKS) break;
            size_t offset = block * BLOCK_ELEMENTS;
            float vals[VECTOR_SIZE];
            double local_sum = 0.0;
            for (int j = 0; j < VECTOR_SIZE; ++j) {
                vals[j] = dist(rng);
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

        // 等待所有线程完成写入
        while (td->allocated_blocks.load(std::memory_order_seq_cst) < NUM_BLOCKS &&
               test_time_condition(test)) {
            __asm__ volatile("yield");
        }
        if (!test_time_condition(test)) break;

        __sync_synchronize();  // 确保写入全局可见（mfence）

        // ---------- 阶段 2：所有线程读取完整数组并验证 ----------
        double read_sum = 0.0;
        bool consistent = true;
        size_t bad_vec = (size_t)-1;
        alignas(16) float store_buf[VECTOR_SIZE];

        for (size_t i = 0; i < TOTAL_ELEMENTS; i += VECTOR_SIZE) {
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

        // ---------- 阶段 3：最后一个完成读取的线程重置状态并增加轮次 ----------
        uint32_t done = td->round_done.fetch_add(1, std::memory_order_seq_cst) + 1;
        if (done == total_threads) {
            td->read_done.store(0, std::memory_order_seq_cst);
            td->iter.fetch_add(1, std::memory_order_release);
        } else {
            while (td->read_done.load(std::memory_order_seq_cst) != 0 &&
                   test_time_condition(test)) {
                __asm__ volatile("yield");
            }
            if (!test_time_condition(test)) break;
        }

    } while (test_time_condition(test));

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
