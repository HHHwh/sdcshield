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

#define NEON_VECTOR_SIZE 4                // 原负载：一次处理 4 个 int32（SVE 谓词分批）
#define BLOCK_SIZE 16                     // 逻辑块大小（原由 4 个 NEON 向量组成）
#define VECTORS_PER_BLOCK (BLOCK_SIZE / NEON_VECTOR_SIZE)  // = 4
// 块数/总元素数在 init 中按目标内存动态计算(见 mesh_target_bytes)。

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    alignas(16) int32_t *data;          // 16 字节对齐即可
    size_t num_blocks;                   // 运行时块数(按目标内存计算)
    size_t total_elements;               // 运行时总元素数
    std::atomic<uint32_t> next_block;
    std::atomic<uint64_t> global_sum;
    std::atomic<uint32_t> write_done;
    std::atomic<uint32_t> read_done;
    std::atomic<uint32_t> reset_lock;
    std::atomic<uint32_t> thread_idx;
    uint32_t total_threads;
};
}

// 目标字节数: -O mesh_upi_sve_wide_symm_int.buffer_bytes=N /
// SDC_MESH_UPI_SVE_WIDE_SYMM_INT_BUFFER_BYTES 环境变量 /
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

static int mesh_upi_sve_wide_symm_int_init(struct test *test) {
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
    const size_t block_bytes = BLOCK_SIZE * elem_bytes;
    size_t target = mesh_target_bytes(test);
    td->num_blocks = target / block_bytes;
    if (td->num_blocks < 1) td->num_blocks = 1;
    if (td->num_blocks > 0xFFFFFFFFu) td->num_blocks = 0xFFFFFFFFu;
    td->total_elements = td->num_blocks * BLOCK_SIZE;
    size_t total_bytes = td->total_elements * elem_bytes;

    td->data = (int32_t*)aligned_alloc(16, total_bytes);
    if (!td->data) {
        delete td;
        return EXIT_FAILURE;
    }
    memset(td->data, 0, total_bytes);

    td->next_block.store(0, std::memory_order_relaxed);
    td->global_sum.store(0, std::memory_order_relaxed);
    td->write_done.store(0, std::memory_order_relaxed);
    td->read_done.store(0, std::memory_order_relaxed);
    td->reset_lock.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    td->total_threads = sysconf(_SC_NPROCESSORS_ONLN);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_sve_wide_symm_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    uint32_t total = td->total_threads;

    /* randomization hardening (B1-002, aligned with 2731971d): framework
     * RNG (per-thread stream, -s reproducible) replaces std::mt19937;
     * range [-1000000, 1000000]. */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        // ---------- 写阶段：分配块并写入 ----------
        while (true) {
            uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
            if (block >= td->num_blocks) break;

            size_t offset = block * BLOCK_SIZE;
            int32_t vals[BLOCK_SIZE];
            uint64_t local_sum = 0;
            for (int j = 0; j < BLOCK_SIZE; ++j) {
                vals[j] = dist();
                local_sum += (uint64_t)vals[j];
            }
            // 使用 4 个向量存储 16 个元素（SVE 谓词分批）
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)NEON_VECTOR_SIZE);
                svint32_t vec = svld1_s32(pg, vals + v * NEON_VECTOR_SIZE);
                svst1_s32(pg, td->data + offset + v * NEON_VECTOR_SIZE, vec);
            }
            __sync_synchronize();   // 类似 _mm_sfence

            td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
            td->write_done.fetch_add(1, std::memory_order_seq_cst);
        }

        // 等待所有块写完成
        while (td->write_done.load(std::memory_order_seq_cst) < td->num_blocks &&
               test_time_condition(test)) {
            __asm__ volatile("yield");
        }
        if (!test_time_condition(test)) break;

        // ---------- 读阶段：读取整个数组并校验 ----------
        uint64_t read_sum = 0;
        bool consistent = true;
        size_t bad_vec = (size_t)-1;
        alignas(16) int32_t store_buf[BLOCK_SIZE];   // 用于一致性测试

        for (size_t i = 0; i < td->total_elements; i += BLOCK_SIZE) {
            // 加载 16 个元素
            int32_t vals[BLOCK_SIZE];
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)NEON_VECTOR_SIZE);
                svint32_t vec = svld1_s32(pg, td->data + i + v * NEON_VECTOR_SIZE);
                svst1_s32(pg, vals + v * NEON_VECTOR_SIZE, vec);
            }
            for (int j = 0; j < BLOCK_SIZE; ++j) {
                read_sum += (uint64_t)vals[j];
            }

            // 一致性测试：Store/Load 比较
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)NEON_VECTOR_SIZE);
                svint32_t orig = svld1_s32(pg, vals + v * NEON_VECTOR_SIZE);
                svst1_s32(pg, store_buf + v * NEON_VECTOR_SIZE, orig);
            }
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)NEON_VECTOR_SIZE);
                svint32_t orig = svld1_s32(pg, vals + v * NEON_VECTOR_SIZE);
                svint32_t reload = svld1_s32(pg, store_buf + v * NEON_VECTOR_SIZE);
                svbool_t cmp = svcmpeq_s32(pg, orig, reload);
                if (svcntp_b32(pg, cmp) != NEON_VECTOR_SIZE) {
                    consistent = false;
                    bad_vec = i + v * NEON_VECTOR_SIZE;
                    break;
                }
            }
            if (!consistent) break;
        }

        uint64_t expected_sum = td->global_sum.load(std::memory_order_seq_cst);
        bool sum_ok = (read_sum == expected_sum);
        bool passed = sum_ok && consistent;

        // 输出结果（仅由线程 0 输出，避免重复）
        if (id == 0) {
            fprintf(stderr, "mesh_upi_sve_wide_symm_int: Thread %d, data[0..15]=(%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, consistent=%d, result=%s%s%s\n",
                    id,
                    td->data[0], td->data[1], td->data[2], td->data[3],
                    td->data[4], td->data[5], td->data[6], td->data[7],
                    td->data[8], td->data[9], td->data[10], td->data[11],
                    td->data[12], td->data[13], td->data[14], td->data[15],
                    read_sum, expected_sum, consistent,
                    passed ? GREEN : RED,
                    passed ? "PASS" : "FAIL",
                    RESET);
            fflush(stderr);
        }

        if (!passed) {
            /* cold path: record bit-level evidence for the failing leg(s) */
            if (read_sum != expected_sum)
                log_error("mesh_upi_sve_wide_symm_int: sum mismatch: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                          (unsigned long long)expected_sum, (unsigned long long)read_sum,
                          (unsigned long long)(expected_sum ^ read_sum));
            if (!consistent)
                log_error("mesh_upi_sve_wide_symm_int: store/reload consistency failed at vector %zu (post-hoc data at failing vector: "
                          "0x%08x 0x%08x 0x%08x 0x%08x)",
                          bad_vec,
                          *(const uint32_t *)&td->data[bad_vec + 0],
                          *(const uint32_t *)&td->data[bad_vec + 1],
                          *(const uint32_t *)&td->data[bad_vec + 2],
                          *(const uint32_t *)&td->data[bad_vec + 3]);
            report_fail_msg("mesh_upi_sve_wide_symm_int: Sum mismatch or consistency failure");
            return EXIT_FAILURE;
        }

        // 注册读完成
        td->read_done.fetch_add(1, std::memory_order_seq_cst);

        // ---------- 重置阶段（由最后一个读完成的线程执行） ----------
        uint32_t expected_lock = 0;
        if (td->reset_lock.compare_exchange_strong(expected_lock, 1,
                                                   std::memory_order_seq_cst,
                                                   std::memory_order_seq_cst)) {
            // 等待所有线程完成读取
            while (td->read_done.load(std::memory_order_seq_cst) < total &&
                   test_time_condition(test)) {
                __asm__ volatile("yield");
            }
            if (!test_time_condition(test)) {
                td->reset_lock.store(0, std::memory_order_seq_cst);
                break;
            }

            // 重置共享状态
            td->global_sum.store(0, std::memory_order_seq_cst);
            td->next_block.store(0, std::memory_order_seq_cst);
            td->write_done.store(0, std::memory_order_seq_cst);
            td->read_done.store(0, std::memory_order_seq_cst);
            td->reset_lock.store(0, std::memory_order_seq_cst);
        } else {
            // 未获得锁，等待重置完成（即 reset_lock 变为 0）
            while (td->reset_lock.load(std::memory_order_seq_cst) != 0 &&
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

static int mesh_upi_sve_wide_symm_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_sve_wide_symm_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_sve_wide_symm_int");
    return EXIT_SKIP;
}
static int mesh_upi_sve_wide_symm_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_sve_wide_symm_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_sve_wide_symm_int,
             "Symmetrical stress … do int32 SVE loads and stores from L1/L2 to L1/L2 with different directions and switching cores (ARM SVE wide-vector version)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sve_wide_symm_int_init,
    .test_run = mesh_upi_sve_wide_symm_int_run,
    .test_cleanup = mesh_upi_sve_wide_symm_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
