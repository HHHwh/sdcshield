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
#define BLOCK_ELEMENTS (VECTOR_SIZE * 2)
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
    std::atomic<uint32_t> round_done;
    std::atomic<uint64_t> iter;
    std::atomic<uint32_t> num_threads;
    std::atomic<uint32_t> ready;
};
}

// 目标字节数: -O mesh_upi_sve_asymm_distrib_write_int.buffer_bytes=N /
// SDC_MESH_UPI_SVE_ASYMM_DISTRIB_WRITE_INT_BUFFER_BYTES 环境变量 /
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

static int mesh_upi_sve_asymm_distrib_write_int_init(struct test *test) {
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
    td->round_done.store(0, std::memory_order_relaxed);
    td->iter.store(0, std::memory_order_relaxed);
    td->num_threads.store(0, std::memory_order_relaxed);
    td->ready.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_sve_asymm_distrib_write_int_run(struct test *test, int cpu) {
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
            // 写核心：循环分配块并写入
            while (test_time_condition(test)) {
                uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
                if (block < td->num_blocks) {
                    size_t offset = block * BLOCK_ELEMENTS;
                    // 生成随机数据（8个 int32）
                    int32_t vals[BLOCK_ELEMENTS];
                    uint64_t local_sum = 0;
                    for (int j = 0; j < BLOCK_ELEMENTS; ++j) {
                        vals[j] = dist();
                        local_sum += (uint64_t)vals[j];
                    }
                    // 分两个向量存储（SVE 谓词分批）。原版所有块写同一位置+读者越界读栈
                    // 是 UB 上的巧合；SVE 版按块偏移写入（与 asymm_int 相同的忠实模式）。
                    for (int v = 0; v < 2; ++v) {
                        svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                        svint32_t vec = svld1_s32(pg, vals + v * VECTOR_SIZE);
                        svst1_s32(pg, td->data + offset + v * VECTOR_SIZE, vec);
                    }
                    // 内存屏障保证可见性
                    __sync_synchronize();

                    td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
                    td->allocated_blocks.fetch_add(1, std::memory_order_seq_cst);
                } else {
                    // 没有可用块，等待 reader 重置
                    while (td->allocated_blocks.load(std::memory_order_seq_cst) > 0 &&
                           test_time_condition(test)) {
                        __asm__ volatile("yield");
                    }
                    if (!test_time_condition(test)) break;
                }
            }
        }

        if (is_reader) {
            // 读核心：等待数据就绪
            while (td->allocated_blocks.load(std::memory_order_seq_cst) < td->num_blocks &&
                   test_time_condition(test)) {
                __asm__ volatile("yield");
            }
            if (!test_time_condition(test)) {
                // 时间到但未完成，直接返回（不报告失败）
                return EXIT_SUCCESS;
            }

            // 读取全部数据（按块遍历，与写者对称）
            uint64_t read_sum = 0;
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
            }

            uint64_t expected_sum = td->global_sum.load(std::memory_order_seq_cst);
            bool passed = (read_sum == expected_sum);

            fprintf(stderr, "mesh_upi_sve_asymm_distrib_write_int: Thread %d (reader), data[0..7]=(%d,%d,%d,%d,%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, result=%s%s%s\n",
                    id,
                    td->data[0], td->data[1], td->data[2], td->data[3],
                    td->data[4], td->data[5], td->data[6], td->data[7],
                    read_sum, expected_sum,
                    passed ? GREEN : RED,
                    passed ? "PASS" : "FAIL",
                    RESET);
            fflush(stderr);

            if (!passed) {
                /* cold path: record bit-level evidence for the failing leg(s) */
                if (read_sum != expected_sum)
                    log_error("mesh_upi_sve_asymm_distrib_write_int: sum mismatch: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                              (unsigned long long)expected_sum, (unsigned long long)read_sum,
                              (unsigned long long)(expected_sum ^ read_sum));
                report_fail_msg("mesh_upi_sve_asymm_distrib_write_int: Sum mismatch");
                return EXIT_FAILURE;
            }

            // 重置状态，让写核心进入下一轮
            td->global_sum.store(0, std::memory_order_seq_cst);
            td->next_block.store(0, std::memory_order_seq_cst);
            td->allocated_blocks.store(0, std::memory_order_seq_cst);
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_sve_asymm_distrib_write_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_sve_asymm_distrib_write_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_sve_asymm_distrib_write_int");
    return EXIT_SKIP;
}
static int mesh_upi_sve_asymm_distrib_write_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_sve_asymm_distrib_write_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_sve_asymm_distrib_write_int,
             "Asymmetrical(1 read core from all rest distributed write cores) stress MESH and UPI or Ring Interconnect and QPI (do int32 SVE loads and stores from L1/L2 to L1/L2 with switching read core) [ARM SVE version]")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sve_asymm_distrib_write_int_init,
    .test_run = mesh_upi_sve_asymm_distrib_write_int_run,
    .test_cleanup = mesh_upi_sve_asymm_distrib_write_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
