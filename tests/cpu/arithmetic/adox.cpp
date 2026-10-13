/**
 * @file
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test adox
 * @parblock
 * Testing unsigned add with overflow (adox) on ARM64 via software simulation.
 * Validates multi-precision addition using overflow-as-carry semantics.
 * The device-under-test chain (__int128 adds) is cross-validated against an
 * independent golden implementation (32-bit limb-wise addition) on inputs
 * freshly randomized with the framework RNG every round.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// ============================================================================
// 黄金参考实现（独立算法：32 位分段累加，进位按半字传播）
// 与 DUT 的 __int128 单次加法在进位粒度与计算路径上均不同，
// 两者交叉验证可暴露任一路径上的数据损坏。
// ============================================================================

static void compute_golden(const uint64_t *lhs, const uint64_t *rhs,
                           uint64_t carry_in, uint64_t *gold, size_t n) {
    uint64_t carry = carry_in & 1;
    for (size_t i = 0; i < n; ++i) {
        /* low 32-bit limbs */
        uint64_t t = (uint64_t)(uint32_t)lhs[i] + (uint32_t)rhs[i] + carry;
        uint64_t lo = t & 0xffffffffu;
        carry = t >> 32;

        /* high 32-bit limbs */
        t = (uint64_t)(uint32_t)(lhs[i] >> 32) + (uint32_t)(rhs[i] >> 32) + carry;
        gold[i] = lo | ((t & 0xffffffffu) << 32);
        carry = t >> 32;
    }
}

// ============================================================================
// 软件模拟 ADOX 链（DUT：__int128 加法，OF 语义进位链）
// 在 ARM64 上无法直接使用 OF，因此用 128 位加法模拟
// ============================================================================

static void adox_chain_sw(const uint64_t *lhs, const uint64_t *rhs,
                          uint64_t *result, size_t n, uint64_t carry_in) {
    uint64_t carry = carry_in;
    for (size_t i = 0; i < n; ++i) {
        unsigned __int128 sum = (unsigned __int128)lhs[i] + rhs[i] + carry;
        result[i] = (uint64_t)sum;
        carry = (uint64_t)(sum >> 64);
    }
}

// ============================================================================
// 已知答案自检（期望值为手算常量，独立于任一实现）
// 覆盖：进位生成、进位跨字传播、进位吸收、混合模式
// ============================================================================

static bool kat_selfcheck() {
    static const uint64_t kat_lhs[4] = {
        0xFFFFFFFFFFFFFFFFull, 0x0000000000000000ull,
        0xFFFFFFFFFFFFFFFFull, 0x1234567890ABCDEFull,
    };
    static const uint64_t kat_rhs[4] = {
        0x0000000000000001ull, 0xFFFFFFFFFFFFFFFFull,
        0x0000000000000001ull, 0xFEDCBA0987654321ull,
    };
    /* carry_in = 1:
     *  w0: FFFFFFFFFFFFFFFF + 1 + 1 -> 0000000000000001, carry 1
     *  w1: 0 + FFFFFFFFFFFFFFFF + 1 -> 0000000000000000, carry 1
     *  w2: FFFFFFFFFFFFFFFF + 1 + 1 -> 0000000000000001, carry 1
     *  w3: 1234567890ABCDEF + FEDCBA0987654321 + 1 -> 1111108218111111, carry 1 */
    static const uint64_t kat_expected[4] = {
        0x0000000000000001ull, 0x0000000000000000ull,
        0x0000000000000001ull, 0x1111108218111111ull,
    };
    uint64_t gold[4], dut[4];

    compute_golden(kat_lhs, kat_rhs, 1, gold, 4);
    if (memcmp(gold, kat_expected, sizeof(kat_expected)) != 0)
        return false;

    adox_chain_sw(kat_lhs, kat_rhs, dut, 4, 1);
    if (memcmp(dut, kat_expected, sizeof(kat_expected)) != 0)
        return false;

    return true;
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
// 测试生命周期函数
// ============================================================================

static int adox_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int adox_run(struct test *test, int cpu) {
    (void)cpu;
    constexpr size_t N = 1024;

    if (!kat_selfcheck()) {
        report_fail_msg("adox: known-answer self-check failed");
        return EXIT_FAILURE;
    }

    std::vector<uint64_t> lhs(N);
    std::vector<uint64_t> rhs(N);
    std::vector<uint64_t> gold(N);
    std::vector<uint64_t> result(N);
    std::vector<uint64_t> store_buf(N);

    do {
        /* 每轮用框架 RNG 重掷输入（可通过 --random-seed 复现） */
        const uint64_t carry_in = random64() & 1;
        for (size_t i = 0; i < N; ++i) {
            lhs[i] = random64();
            rhs[i] = random64();
        }

        compute_golden(lhs.data(), rhs.data(), carry_in, gold.data(), N);
        adox_chain_sw(lhs.data(), rhs.data(), result.data(), N, carry_in);

        bool data_ok = (memcmp(result.data(), gold.data(), N * sizeof(uint64_t)) == 0);

        memcpy(store_buf.data(), result.data(), N * sizeof(uint64_t));
        bool consistent = (memcmp(store_buf.data(), result.data(), N * sizeof(uint64_t)) == 0);

        if (!(data_ok && consistent)) {
            print_colored_result("adox", false,
                                 lhs.data(), rhs.data(),
                                 result.data(), gold.data(),
                                 N, carry_in);
            report_fail_msg("adox: data_ok=%d, consistent=%d", data_ok, consistent);
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int adox_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

// ============================================================================
// 测试声明
// ============================================================================

DECLARE_TEST(adox, "Testing unsigned add with overflow (adox) on ARM64 (simulated)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = adox_init,
    .test_run = adox_run,
    .test_cleanup = adox_finish,
    .fracture_loop_count = 4,
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
