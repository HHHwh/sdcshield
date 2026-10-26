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

#define VECTOR_SIZE 4                  // 原负载：一次处理 4 个 int32（SVE 谓词分批）
// 总元素数在 init 中按目标内存动态计算(见 mesh_target_bytes)。

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    alignas(16) int32_t *data;         // 共享只读数据（16 字节对齐）
    size_t total_elements;             // 运行时总元素数(按目标内存计算)
    uint64_t golden_sum;               // 预先计算的参考累加和
    std::atomic<uint32_t> thread_idx;
};
}

// 目标字节数: -O mesh_upi_sve_read_only_int.buffer_bytes=N /
// SDC_MESH_UPI_SVE_READ_ONLY_INT_BUFFER_BYTES 环境变量 /
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

static int mesh_upi_sve_read_only_int_init(struct test *test) {
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
    size_t target = mesh_target_bytes(test);
    td->total_elements = target / elem_bytes;
    /* 向下对齐到 VECTOR_SIZE 的整数倍, 保证读循环步进不越界 */
    td->total_elements = td->total_elements / VECTOR_SIZE * VECTOR_SIZE;
    if (td->total_elements < VECTOR_SIZE) td->total_elements = VECTOR_SIZE;
    size_t total_bytes = td->total_elements * elem_bytes;

    td->data = (int32_t*)aligned_alloc(16, total_bytes);
    if (!td->data) {
        delete td;
        return EXIT_FAILURE;
    }

    // 生成随机 int32 数据并计算参考累加和
    /* randomization hardening (B1-002, aligned with 2731971d): framework
     * RNG (init-time stream, -s reproducible) replaces std::mt19937;
     * range [-1000000, 1000000]. */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };
    uint64_t sum = 0;
    for (size_t i = 0; i < td->total_elements; ++i) {
        td->data[i] = dist();
        sum += (uint64_t)td->data[i];
    }
    td->golden_sum = sum;
    td->thread_idx.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_sve_read_only_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        uint64_t local_sum = 0;

        // 顺序读取整个数组（SVE 谓词分批，每次 VECTOR_SIZE 个 int32）
        for (size_t i = 0; i < td->total_elements; i += VECTOR_SIZE) {
            svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
            svint32_t v = svld1_s32(pg, td->data + i);
            int32_t vals[VECTOR_SIZE];
            svst1_s32(pg, vals, v);
            for (int j = 0; j < VECTOR_SIZE; ++j) {
                local_sum += (uint64_t)vals[j];
            }
        }

        bool passed = (local_sum == td->golden_sum);

        // 输出本次的输入（数组前 4 个元素）和结果
        fprintf(stderr, "mesh_upi_sve_read_only_int: Thread %d, data[0..3]=(%d,%d,%d,%d), local_sum=%lu, golden_sum=%lu, result=%s%s%s\n",
                id,
                td->data[0], td->data[1], td->data[2], td->data[3],
                local_sum, td->golden_sum,
                passed ? GREEN : RED,
                passed ? "PASS" : "FAIL",
                RESET);
        fflush(stderr);

        if (!passed) {
            /* cold path: record bit-level evidence for the failing leg(s) */
            if (local_sum != td->golden_sum)
                log_error("mesh_upi_sve_read_only_int: sum mismatch: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                          (unsigned long long)td->golden_sum, (unsigned long long)local_sum,
                          (unsigned long long)(td->golden_sum ^ local_sum));
            report_fail_msg("mesh_upi_sve_read_only_int: Sum mismatch");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_sve_read_only_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_sve_read_only_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_sve_read_only_int");
    return EXIT_SKIP;
}
static int mesh_upi_sve_read_only_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_sve_read_only_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_sve_read_only_int, "Mutual read stress MESH and UPI or Ring Interconnect and QPI Mutual read stress MESH and UPI or Ring Interconnect and QPI (do int32 SVE loads from L1/L2/L3)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sve_read_only_int_init,
    .test_run = mesh_upi_sve_read_only_int_run,
    .test_cleanup = mesh_upi_sve_read_only_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
