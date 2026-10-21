/**
 * @file adcx.cpp
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test adcx
 * @parblock
 * Testing unsigned add with carry on ARM64 using ADCS instruction sequence.
 * Validates multi-precision addition and carry propagation on ARMv8/v9 processors.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// ============================================================================
// 测试数据（所有线程共享只读）
// ============================================================================

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    size_t num_elems;               // 向量长度
};
}

// ============================================================================
// 黄金参考实现（软件模拟 ADC 链）
// ============================================================================

static void compute_golden(const uint64_t *lhs, const uint64_t *rhs,
                           uint64_t carry_in, uint64_t *gold, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        unsigned __int128 sum = (unsigned __int128)lhs[i] + rhs[i] + carry_in;
        gold[i] = (uint64_t)sum;
        carry_in = (uint64_t)(sum >> 64);   // 传递 Carry
    }
}

// ============================================================================
// 带颜色的日志输出
// ============================================================================

static void print_colored_result(const char *label, bool passed,
                                 const uint64_t *lhs, const uint64_t *rhs,
                                 const uint64_t *result, const uint64_t *gold,
                                 size_t n, uint64_t carry_in) {
    const char *color = passed ? "\033[32m" : "\033[31m";
    const char *result_str = passed ? "PASS" : "FAIL";

    fprintf(stderr, "%s: %s%s\033[0m\n", label, color, result_str);
    fprintf(stderr, "  carry_in = %lu\n", carry_in);
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
// ADCS 指令链（ARM64 纯内联汇编无损进位传递实现）
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
        // 1. 设置初始 Carry Flag
        // carry_in 为 1 时：1 + 0xFFFFFFFFFFFFFFFF 产生溢出 -> PSTATE.C = 1
        // carry_in 为 0 时：0 + 0xFFFFFFFFFFFFFFFF 无溢出   -> PSTATE.C = 0
        "mov     x7, #-1\n\t"
        "adds    xzr, %4, x7\n\t"

        // 2. 核心进位加法循环
        "1:\n\t"
        "ldr     x4, [%0], #8\n\t"       // 读取 lhs[i]，指针 +8
        "ldr     x5, [%1], #8\n\t"       // 读取 rhs[i]，指针 +8
        "adcs    x6, x4, x5\n\t"         // 带 C 标志加法，同时产生新的 C 标志
        "str     x6, [%2], #8\n\t"       // 写入 result[i]，指针 +8

        "sub     %3, %3, #1\n\t"         // sub（非 subs）不会修改 NZCV 标志位
        "cbnz    %3, 1b\n\t"             // cbnz 不依赖/不触碰 NZCV 标志位
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
// 测试生命周期
// ============================================================================

static int adcx_init(struct test *test) {
    auto *data = new TestData;
    if (!data) return EXIT_FAILURE;

    // Only the vector length is shared; operands are generated per-thread in
    // adcx_run via the framework RNG (per-thread streams, -s reproducible).
    constexpr size_t N = 1024;
    data->num_elems = N;

    test->data = data;
    return EXIT_SUCCESS;
}

static int adcx_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);
    const size_t N = td->num_elems;

    // Per-thread operands + golden: re-rolled every iteration from the
    // framework's per-thread RNG so the ADCS chain sees fresh data every
    // loop (random64() is per-thread, so no sharing race).
    std::vector<uint64_t> lhs(N);
    std::vector<uint64_t> rhs(N);
    std::vector<uint64_t> gold(N);
    std::vector<uint64_t> result(N);
    std::vector<uint64_t> store_buf(N);

    do {
        uint64_t carry_in = random64() & 1;
        for (size_t i = 0; i < N; ++i) {
            lhs[i] = random64();
            rhs[i] = random64();
        }
        compute_golden(lhs.data(), rhs.data(), carry_in, gold.data(), N);

        adcx_chain_correct(lhs.data(), rhs.data(), result.data(), N, carry_in);

        bool data_ok = (memcmp(result.data(), gold.data(), N * sizeof(uint64_t)) == 0);

        memcpy(store_buf.data(), result.data(), N * sizeof(uint64_t));
        bool consistent = (memcmp(store_buf.data(), result.data(), N * sizeof(uint64_t)) == 0);

        bool passed = data_ok && consistent;

        if (!passed) {
            print_colored_result("adcx", passed,
                                 lhs.data(), rhs.data(),
                                 result.data(), gold.data(),
                                 N, carry_in);
            report_fail_msg("adcx: data_ok=%d, consistent=%d", data_ok, consistent);
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int adcx_finish(struct test *test) {
    delete static_cast<TestData*>(test->data);
    return EXIT_SUCCESS;
}

// ============================================================================
// 测试声明
// ============================================================================

DECLARE_TEST(adcx, "Testing unsigned add with carry (adcx) on ARM64")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = adcx_init,
    .test_run = adcx_run,
    .test_cleanup = adcx_finish,
    .fracture_loop_count = 4,
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
