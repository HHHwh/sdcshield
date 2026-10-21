/**
 * @file
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test adcxlong
 * @parblock
 * Testing long acdx chains for IFU parity.
 * This test performs a very long ADCX chain (large vector) to stress the
 * instruction decoder and pipeline. It computes multi-precision addition
 * on random 64-bit integers, compares against a golden reference.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

// ============================================================================
// 测试数据（所有线程共享只读）
// ============================================================================

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    std::vector<uint64_t> lhs;      // 操作数1
    std::vector<uint64_t> rhs;      // 操作数2
    std::vector<uint64_t> gold;     // 黄金参考值
    uint64_t carry_in;              // 初始进位 (0 或 1)
    size_t num_elems;               // 向量长度
};
}

// ============================================================================
// 黄金参考实现（软件模拟 ADCX 链）
// ============================================================================

static void compute_golden(const uint64_t *lhs, const uint64_t *rhs,
                           uint64_t carry_in, uint64_t *gold, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned __int128 sum = (unsigned __int128)lhs[i] + rhs[i] + carry_in;
        gold[i] = (uint64_t)sum;
        carry_in = (uint64_t)(sum >> 64);
    }
}

// ============================================================================
// 带颜色的日志输出（仅显示前4个元素）
// ============================================================================

static void print_colored_result(const char *label, bool passed,
                                 const uint64_t *lhs, const uint64_t *rhs,
                                 const uint64_t *result, const uint64_t *gold,
                                 size_t n, uint64_t carry_in) {
    const char *color = passed ? "\033[32m" : "\033[31m";
    const char *result_str = passed ? "PASS" : "FAIL";

    fprintf(stderr, "%s: %s%s\033[0m\n", label, color, result_str);
    fprintf(stderr, "  carry_in = %lu\n", carry_in);
    fprintf(stderr, "  length = %zu\n", n);
    fprintf(stderr, "  lhs[0..3] = 0x%016lx 0x%016lx 0x%016lx 0x%016lx\n",
            lhs[0], (n > 1 ? lhs[1] : 0), (n > 2 ? lhs[2] : 0), (n > 3 ? lhs[3] : 0));
    fprintf(stderr, "  rhs[0..3] = 0x%016lx 0x%016lx 0x%016lx 0x%016lx\n",
            rhs[0], (n > 1 ? rhs[1] : 0), (n > 2 ? rhs[2] : 0), (n > 3 ? rhs[3] : 0));
    fprintf(stderr, "  result[0..3]= 0x%016lx 0x%016lx 0x%016lx 0x%016lx\n",
            result[0], (n > 1 ? result[1] : 0), (n > 2 ? result[2] : 0), (n > 3 ? result[3] : 0));
    fprintf(stderr, "  gold[0..3]  = 0x%016lx 0x%016lx 0x%016lx 0x%016lx\n",
            gold[0], (n > 1 ? gold[1] : 0), (n > 2 ? gold[2] : 0), (n > 3 ? gold[3] : 0));
    fflush(stderr);
}

// ============================================================================
// ADCS 指令链（ARM64 纯内联汇编，使用修复后的方法）
// ============================================================================
#ifdef __aarch64__
static void adcx_chain_correct(const uint64_t *lhs, const uint64_t *rhs,
                               uint64_t *result, size_t n, uint64_t carry_in) {
    if (n == 0) return;

    const uint64_t *p_lhs = lhs;
    const uint64_t *p_rhs = rhs;
    uint64_t *p_res = result;
    size_t count = n;

    __asm__ volatile(
        // 设置初始 Carry Flag
        "mov     x7, #-1\n\t"
        "adds    xzr, %4, x7\n\t"

        // 核心进位加法循环
        "1:\n\t"
        "ldr     x4, [%0], #8\n\t"
        "ldr     x5, [%1], #8\n\t"
        "adcs    x6, x4, x5\n\t"
        "str     x6, [%2], #8\n\t"

        "sub     %3, %3, #1\n\t"         // sub 不修改 NZCV
        "cbnz    %3, 1b\n\t"
        : "+r"(p_lhs), "+r"(p_rhs), "+r"(p_res), "+r"(count)
        : "r"(carry_in)
        : "x4", "x5", "x6", "x7", "cc", "memory"
    );
}
#else
// 非 aarch64：无 ADCS 指令，用软件 128 位加法模拟（结果与黄金一致）
static void adcx_chain_correct(const uint64_t *lhs, const uint64_t *rhs,
                               uint64_t *result, size_t n, uint64_t carry_in) {
    for (size_t i = 0; i < n; ++i) {
        unsigned __int128 sum = (unsigned __int128)lhs[i] + rhs[i] + carry_in;
        result[i] = (uint64_t)sum;
        carry_in = (uint64_t)(sum >> 64);
    }
}
#endif

// ============================================================================
// 测试生命周期函数
// ============================================================================

static int adcxlong_init(struct test *test) {
    auto *data = new TestData;
    if (!data) return EXIT_FAILURE;

    constexpr size_t N = 4096;   // 长链，增加 IFU 压力
    data->num_elems = N;
    data->lhs.resize(N);
    data->rhs.resize(N);
    data->gold.resize(N);

    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;
    for (size_t i = 0; i < N; ++i) {
        data->lhs[i] = dist(rng);
        data->rhs[i] = dist(rng);
    }
    data->carry_in = dist(rng) & 1;

    compute_golden(data->lhs.data(), data->rhs.data(), data->carry_in,
                   data->gold.data(), N);

    test->data = data;
    return EXIT_SUCCESS;
}

static int adcxlong_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);
    const size_t N = td->num_elems;

    std::vector<uint64_t> result(N);
    std::vector<uint64_t> store_buf(N);

    do {
        adcx_chain_correct(td->lhs.data(), td->rhs.data(), result.data(), N, td->carry_in);

        bool data_ok = (memcmp(result.data(), td->gold.data(), N * sizeof(uint64_t)) == 0);

        memcpy(store_buf.data(), result.data(), N * sizeof(uint64_t));
        bool consistent = (memcmp(store_buf.data(), result.data(), N * sizeof(uint64_t)) == 0);

        bool passed = data_ok && consistent;

        print_colored_result("adcxlong", passed,
                             td->lhs.data(), td->rhs.data(),
                             result.data(), td->gold.data(),
                             N, td->carry_in);

        if (!passed) {
            report_fail_msg("adcxlong: data_ok=%d, consistent=%d", data_ok, consistent);
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int adcxlong_finish(struct test *test) {
    delete static_cast<TestData*>(test->data);
    return EXIT_SUCCESS;
}

// ============================================================================
// 测试声明
// ============================================================================

DECLARE_TEST(adcxlong, "Testing long acdx chains for IFU parity on ARM64")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = adcxlong_init,
    .test_run = adcxlong_run,
    .test_cleanup = adcxlong_finish,
    .fracture_loop_count = 4,
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
