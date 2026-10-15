#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#ifdef __aarch64__
#include <arm_sve.h>
#include <sys/auxv.h>
#include <unistd.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif

#define PER_THREAD_SIZE (1 << 20)      // 每个线程 1M 个 int32 = 4MB
#define VECTOR_SIZE 4                  // 原负载：每次存储 4 个 int32（SVE 谓词分批）

struct TestData {
    std::atomic<uint64_t> thread_idx;
};

static int mesh_upi_sve_write_only_int_init(struct test *test) {
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for mesh_upi_sve_write_only_int");
        return EXIT_SKIP;
    }

    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;
    td->thread_idx.store(0, std::memory_order_relaxed);
    test->data = td;
    return EXIT_SUCCESS;
}

static int mesh_upi_sve_write_only_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);

    // 每个线程独立分配自己的缓冲区（对齐到 16 字节）
    alignas(16) int32_t *buf = (int32_t*)aligned_alloc(16, PER_THREAD_SIZE * sizeof(int32_t));
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

        for (size_t i = 0; i < PER_THREAD_SIZE; i += VECTOR_SIZE) {
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
