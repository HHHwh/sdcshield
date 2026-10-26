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

#define VECTOR_SIZE 4                  // 原负载：每次存储 4 个 int32（SVE 谓词分批）
// 每线程元素数在 run 中按目标内存动态计算(见 mesh_target_bytes)。

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    std::atomic<uint64_t> thread_idx;
    size_t per_thread_elements;         // 运行时每线程元素数(按目标内存计算)
};
}

// 每线程目标字节数: -O mesh_upi_sve_write_only_int.buffer_bytes=N /
// SDC_MESH_UPI_SVE_WRITE_ONLY_INT_BUFFER_BYTES 环境变量 /
// 自适应 = 物理内存 90% 在 2 路 mesh 战役间均摊后, 再按线程数均摊
// (每线程独立缓冲, 总占用 = buffer_bytes * nthreads)
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
    int threads = thread_count();
    if (threads < 1) threads = 1;
    return sdc_target_workset_bytes(900, 2, nullptr) / (size_t)threads;
}

static int mesh_upi_sve_write_only_int_init(struct test *test) {
    /* All-VL Phase 2 P6: per-test auxv hand-probe -> framework
     * layer: arm64_require_sve_vl does the SVE capability check
     * + 128..1024-bit range gate in one call (skip reason
     * framework-issued). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;
    td->thread_idx.store(0, std::memory_order_relaxed);

    const size_t elem_bytes = sizeof(int32_t);
    size_t target = mesh_target_bytes(test);
    td->per_thread_elements = target / elem_bytes;
    /* 向下对齐到 VECTOR_SIZE 的整数倍, 保证写循环步进不越界 */
    td->per_thread_elements = td->per_thread_elements / VECTOR_SIZE * VECTOR_SIZE;
    if (td->per_thread_elements < VECTOR_SIZE) td->per_thread_elements = VECTOR_SIZE;
    test->data = td;
    return EXIT_SUCCESS;
}

static int mesh_upi_sve_write_only_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);

    // 每个线程独立分配自己的缓冲区（对齐到 16 字节）
    const size_t per_thread_bytes = td->per_thread_elements * sizeof(int32_t);
    alignas(16) int32_t *buf = (int32_t*)aligned_alloc(16, per_thread_bytes);
    if (!buf) {
        report_fail_msg("Failed to allocate buffer");
        return EXIT_FAILURE;
    }

    /* randomization hardening (B1-002, aligned with 2731971d): framework
     * RNG (per-thread stream, -s reproducible) replaces std::mt19937;
     * range [-1000000, 1000000]. */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        // 逐块生成随机数据、写入、立即读回验证
        bool passed = true;
        int32_t first_vals[VECTOR_SIZE]; // 用于输出本次的输入

        for (size_t i = 0; i < td->per_thread_elements; i += VECTOR_SIZE) {
            // 生成 VECTOR_SIZE 个随机数
            int32_t vals[VECTOR_SIZE];
            for (int j = 0; j < VECTOR_SIZE; ++j) {
                vals[j] = dist();
                if (i == 0) first_vals[j] = vals[j]; // 保存前几个用于输出
            }

            // 使用 SVE 存储
            svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
            svint32_t v = svld1_s32(pg, vals);
            svst1_s32(pg, buf + i, v);

            // 立即读回验证
            svint32_t loaded = svld1_s32(pg, buf + i);
            int32_t loaded_vals[VECTOR_SIZE];
            svst1_s32(pg, loaded_vals, loaded);
            for (int j = 0; j < VECTOR_SIZE; ++j) {
                if (loaded_vals[j] != vals[j]) {
                    passed = false;
                    break;
                }
            }
            if (!passed) break;
        }

        // 输出本次的输入（前 VECTOR_SIZE 个随机数）和结果
        fprintf(stderr, "mesh_upi_sve_write_only_int: Thread %d, input[0..3]=(%d,%d,%d,%d), result=%s%s%s\n",
                id,
                first_vals[0], first_vals[1], first_vals[2], first_vals[3],
                passed ? GREEN : RED,
                passed ? "PASS" : "FAIL",
                RESET);
        fflush(stderr);

        if (!passed) {
            report_fail_msg("mesh_upi_sve_write_only_int: Data mismatch");
            free(buf);
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    free(buf);
    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_sve_write_only_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_sve_write_only_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_sve_write_only_int");
    return EXIT_SKIP;
}
static int mesh_upi_sve_write_only_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_sve_write_only_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_sve_write_only_int, "Mutual write stress MESH and UPI or Ring Interconnect and QPI (do int32 SVE stores to L1/L2/L3)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sve_write_only_int_init,
    .test_run = mesh_upi_sve_write_only_int_run,
    .test_cleanup = mesh_upi_sve_write_only_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
