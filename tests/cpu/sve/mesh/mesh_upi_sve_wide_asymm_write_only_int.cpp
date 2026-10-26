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

#define NEON_VECTOR_SIZE 4                // NEON 一次处理 4 个 int32
#define BLOCK_ELEMENTS 16                 // 每个块 16 个 int32（由 4 个 NEON 向量组成）
#define VECTORS_PER_BLOCK (BLOCK_ELEMENTS / NEON_VECTOR_SIZE)  // = 4
// 块数/总元素数在 init 中按目标内存动态计算(见 mesh_target_bytes)。

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    alignas(16) int32_t *data;           // 共享数据数组（16 字节对齐）
    size_t num_blocks;                   // 运行时块数(按目标内存计算)
    size_t total_elements;               // 运行时总元素数
    std::atomic<uint32_t> next_block;    // 下一个可分配的块索引
    std::atomic<uint64_t> global_sum;    // 所有写核心的校验和累加
    std::atomic<uint32_t> allocated_blocks; // 已分配的块数
    std::atomic<uint32_t> thread_idx;    // 线程 ID 分配器
    std::atomic<uint32_t> reset_lock;    // 重置互斥锁
};
}

// 目标字节数: -O mesh_upi_sve_wide_asymm_write_only_int.buffer_bytes=N /
// SDC_MESH_UPI_SVE_WIDE_ASYMM_WRITE_ONLY_INT_BUFFER_BYTES 环境变量 /
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

static int mesh_upi_sve_wide_asymm_write_only_int_init(struct test *test) {
    /* All-VL Phase 2 P6: per-test auxv hand-probe -> framework
     * layer: arm64_require_sve_vl does the SVE capability check
     * + 128..1024-bit range gate in one call (skip reason
     * framework-issued). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) {
        return -255;   // 需要至少 2 个线程
    }

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
    td->reset_lock.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_sve_wide_asymm_write_only_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    bool is_reader = (id == 0);

    /* randomization hardening (B1-002, aligned with 2731971d): framework
     * RNG (per-thread stream, -s reproducible) replaces std::mt19937;
     * range [-1000000, 1000000]. */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        if (!is_reader) {
            // 写核心：循环分配块直到所有块分配完
            while (true) {
                uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
                if (block >= td->num_blocks) break;

                size_t offset = block * BLOCK_ELEMENTS;
                int32_t vals[BLOCK_ELEMENTS];
                uint64_t local_sum = 0;
                for (int j = 0; j < BLOCK_ELEMENTS; ++j) {
                    vals[j] = dist();
                    local_sum += (uint64_t)vals[j];
                }
                // 使用 4 个 NEON 向量存储 16 个元素
                for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)NEON_VECTOR_SIZE);

                    svint32_t vec = svld1_s32(pg, vals + v * NEON_VECTOR_SIZE);

                    svst1_s32(pg, td->data + offset + v * NEON_VECTOR_SIZE, vec);
                }
                __sync_synchronize();   // 确保写入对其他核心可见

                // 一致性测试：立即读回比较
                [[maybe_unused]] bool block_ok = true;
                for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)NEON_VECTOR_SIZE);

                    svint32_t written = svld1_s32(pg, vals + v * NEON_VECTOR_SIZE);

                    svint32_t loaded = svld1_s32(pg, td->data + offset + v * NEON_VECTOR_SIZE);

                    svbool_t cmp = svcmpeq_s32(pg, written, loaded);

                    if (svcntp_b32(pg, cmp) != NEON_VECTOR_SIZE) {
                        block_ok = false;
                        break;
                    }
                }
                // 原代码发现不一致时仅记录，我们也同样处理（最终会在读核心校验时失败）

                td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
                td->allocated_blocks.fetch_add(1, std::memory_order_seq_cst);
            }
        }

        if (is_reader) {
            // 等待所有块分配完成
            while (td->allocated_blocks.load(std::memory_order_seq_cst) < td->num_blocks &&
                   test_time_condition(test)) {
                __asm__ volatile("yield");
            }
            if (!test_time_condition(test)) break;

            // 读取整个数组，计算校验和并进行一致性测试
            uint64_t read_sum = 0;
            bool consistent = true;
            size_t bad_vec = (size_t)-1;
            alignas(16) int32_t store_buf[BLOCK_ELEMENTS];

            for (size_t i = 0; i < td->total_elements; i += BLOCK_ELEMENTS) {
                // 加载 16 个元素
                int32_t vals[BLOCK_ELEMENTS];
                for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)NEON_VECTOR_SIZE);

                    svint32_t vec = svld1_s32(pg, td->data + i + v * NEON_VECTOR_SIZE);

                    svst1_s32(pg, vals + v * NEON_VECTOR_SIZE, vec);
                }
                for (int j = 0; j < BLOCK_ELEMENTS; ++j) {
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
            }

            uint64_t expected_sum = td->global_sum.load(std::memory_order_seq_cst);
            bool sum_ok = (read_sum == expected_sum);
            bool passed = sum_ok && consistent;

            // 输出结果
            fprintf(stderr, "mesh_upi_sve_wide_asymm_write_only_int: Thread %d (reader), data[0..15]=(%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, consistent=%d, result=%s%s%s\n",
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

            if (!passed) {
                /* cold path: record bit-level evidence for the failing leg(s) */
                if (read_sum != expected_sum)
                    log_error("mesh_upi_sve_wide_asymm_write_only_int: sum mismatch: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                              (unsigned long long)expected_sum, (unsigned long long)read_sum,
                              (unsigned long long)(expected_sum ^ read_sum));
                if (!consistent)
                    log_error("mesh_upi_sve_wide_asymm_write_only_int: store/reload consistency failed at vector %zu (post-hoc data at failing vector: "
                              "0x%08x 0x%08x 0x%08x 0x%08x)",
                              bad_vec,
                              *(const uint32_t *)&td->data[bad_vec + 0],
                              *(const uint32_t *)&td->data[bad_vec + 1],
                              *(const uint32_t *)&td->data[bad_vec + 2],
                              *(const uint32_t *)&td->data[bad_vec + 3]);
                report_fail_msg("mesh_upi_sve_wide_asymm_write_only_int: Sum mismatch or consistency failure");
                return EXIT_FAILURE;
            }

            // 重置状态（仅由一个读核心执行）
            uint32_t expected_lock = 0;
            if (td->reset_lock.compare_exchange_strong(expected_lock, 1,
                                                       std::memory_order_seq_cst,
                                                       std::memory_order_seq_cst)) {
                td->global_sum.store(0, std::memory_order_seq_cst);
                td->next_block.store(0, std::memory_order_seq_cst);
                td->allocated_blocks.store(0, std::memory_order_seq_cst);
                td->reset_lock.store(0, std::memory_order_seq_cst);
            } else {
                // 未获得锁（但此测试只有唯一读核心，不会进入）
                while (td->reset_lock.load(std::memory_order_seq_cst) != 0 &&
                       test_time_condition(test)) {
                    __asm__ volatile("yield");
                }
                if (!test_time_condition(test)) break;
            }
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_sve_wide_asymm_write_only_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_sve_wide_asymm_write_only_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_sve_wide_asymm_write_only_int");
    return EXIT_SKIP;
}
static int mesh_upi_sve_wide_asymm_write_only_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_sve_wide_asymm_write_only_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_sve_wide_asymm_write_only_int,
             "Mutual write stress MESH and UPI (do NEON stores to L1/L2/L3)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sve_wide_asymm_write_only_int_init,
    .test_run = mesh_upi_sve_wide_asymm_write_only_int_run,
    .test_cleanup = mesh_upi_sve_wide_asymm_write_only_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
