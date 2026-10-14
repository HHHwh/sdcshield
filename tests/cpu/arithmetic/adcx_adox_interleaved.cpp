/**
 * @file
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test adcx_adox_interleaved
 * @parblock
 * Interleaved unsigned add with overflow (adox) and unsigned add with carry (adcx).
 * This test validates two multi-precision addition chains: one using carry flag (CF)
 * and another using overflow flag (OF) as carry input. Since ARM64 lacks native
 * ADCX/ADOX instructions, both chains are software-simulated using 64-bit arithmetic.
 * Three genuinely independent implementations of the same addition -- a 32-bit
 * limb-wise golden, an overflow-flag ADCX-style chain, and a 128-bit ADOX-style
 * chain -- cross-validate each other on inputs freshly randomized with the
 * framework RNG every round.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// ============================================================================
// 黄金参考实现（独立算法：32 位分段累加，进位按半字传播）
// 与两条 DUT 链（溢出标志链、128 位加法链）在进位粒度与计算路径上均不同
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
// 软件模拟 ADCX 链（DUT：64 位溢出标志进位链，对应 CF 语义）
// ============================================================================

static void adcx_chain_sw(const uint64_t *lhs, const uint64_t *rhs,
                          uint64_t *result, size_t n, uint64_t carry_in) {
    uint64_t carry = carry_in & 1;
    for (size_t i = 0; i < n; ++i) {
        uint64_t sum;
        bool c1 = __builtin_add_overflow(lhs[i], rhs[i], &sum);
        bool c2 = __builtin_add_overflow(sum, carry, &sum);
        result[i] = sum;
        carry = (c1 || c2);
    }
}

// ============================================================================
// 软件模拟 ADOX 链（DUT：__int128 加法，对应 OF 语义进位链）
// 与 adcx_chain_sw 计算路径不同，二者交叉验证
// ============================================================================

static void adox_chain_sw(const uint64_t *lhs, const uint64_t *rhs,
                          uint64_t *result, size_t n, uint64_t carry_in) {
    uint64_t carry = carry_in & 1;
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
    /* carry_in = 0:
     *  w0: FFFFFFFFFFFFFFFF + 1 + 0 -> 0000000000000000, carry 1
     *  w1: 0 + FFFFFFFFFFFFFFFF + 1 -> 0000000000000000, carry 1
     *  w2: FFFFFFFFFFFFFFFF + 1 + 1 -> 0000000000000001, carry 1
     *  w3: 1234567890ABCDEF + FEDCBA0987654321 + 1 -> 1111108218111111, carry 1
     * carry_in = 1: 仅 w0 变为 0000000000000001，其余不变 */
    static const uint64_t kat_expected_c0[4] = {
        0x0000000000000000ull, 0x0000000000000000ull,
        0x0000000000000001ull, 0x1111108218111111ull,
    };
    static const uint64_t kat_expected_c1[4] = {
        0x0000000000000001ull, 0x0000000000000000ull,
        0x0000000000000001ull, 0x1111108218111111ull,
    };
    uint64_t gold[4], res[4];

    for (uint64_t carry_in : { 0ull, 1ull }) {
        const uint64_t *expected = (carry_in == 0) ? kat_expected_c0 : kat_expected_c1;

        compute_golden(kat_lhs, kat_rhs, carry_in, gold, 4);
        if (memcmp(gold, expected, sizeof(kat_expected_c0)) != 0)
            return false;

        adcx_chain_sw(kat_lhs, kat_rhs, res, 4, carry_in);
        if (memcmp(res, expected, sizeof(kat_expected_c0)) != 0)
            return false;

        adox_chain_sw(kat_lhs, kat_rhs, res, 4, carry_in);
        if (memcmp(res, expected, sizeof(kat_expected_c0)) != 0)
            return false;
    }

    return true;
}

// ============================================================================
// 带颜色的日志输出
// ============================================================================

static void print_colored_result(const char *label, bool passed,
                                 const uint64_t *lhs, const uint64_t *rhs,
                                 const uint64_t *result_cf, const uint64_t *result_of,
                                 const uint64_t *gold, size_t n) {
    const char *color = passed ? "\033[32m" : "\033[31m";
    const char *result_str = passed ? "PASS" : "FAIL";

    fprintf(stderr, "%s: %s%s\033[0m\n", label, color, result_str);
    fprintf(stderr, "  lhs[0..3] = 0x%016lx 0x%016lx 0x%016lx 0x%016lx\n",
            lhs[0], (n > 1 ? lhs[1] : 0), (n > 2 ? lhs[2] : 0), (n > 3 ? lhs[3] : 0));
    fprintf(stderr, "  rhs[0..3] = 0x%016lx 0x%016lx 0x%016lx 0x%016lx\n",
            rhs[0], (n > 1 ? rhs[1] : 0), (n > 2 ? rhs[2] : 0), (n > 3 ? rhs[3] : 0));
    fprintf(stderr, "  result_cf[0..3]=0x%016lx 0x%016lx 0x%016lx 0x%016lx\n",
            result_cf[0], (n > 1 ? result_cf[1] : 0), (n > 2 ? result_cf[2] : 0), (n > 3 ? result_cf[3] : 0));
    fprintf(stderr, "  result_of[0..3]=0x%016lx 0x%016lx 0x%016lx 0x%016lx\n",
            result_of[0], (n > 1 ? result_of[1] : 0), (n > 2 ? result_of[2] : 0), (n > 3 ? result_of[3] : 0));
    fprintf(stderr, "  gold[0..3]   =0x%016lx 0x%016lx 0x%016lx 0x%016lx\n",
            gold[0], (n > 1 ? gold[1] : 0), (n > 2 ? gold[2] : 0), (n > 3 ? gold[3] : 0));
    fflush(stderr);
}

// ============================================================================
// 测试生命周期函数
// ============================================================================

static int adcx_adox_interleaved_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int adcx_adox_interleaved_run(struct test *test, int cpu) {
    (void)cpu;
    constexpr size_t N = 1024;

    if (!kat_selfcheck()) {
        report_fail_msg("adcx_adox_interleaved: known-answer self-check failed");
        return EXIT_FAILURE;
    }

    std::vector<uint64_t> lhs(N);
    std::vector<uint64_t> rhs(N);
    std::vector<uint64_t> gold(N);
    std::vector<uint64_t> result_cf(N);
    std::vector<uint64_t> result_of(N);
    std::vector<uint64_t> store_buf(N);

    do {
        /* 每轮用框架 RNG 重掷输入（可通过 --random-seed 复现） */
        const uint64_t carry_in = random64() & 1;
        for (size_t i = 0; i < N; ++i) {
            lhs[i] = random64();
            rhs[i] = random64();
        }

        compute_golden(lhs.data(), rhs.data(), carry_in, gold.data(), N);

        // 1. 模拟 ADCX 链（64 位溢出标志进位链）
        adcx_chain_sw(lhs.data(), rhs.data(), result_cf.data(), N, carry_in);

        // 2. 模拟 ADOX 链（128 位加法链，与 ADCX 链计算路径不同）
        adox_chain_sw(lhs.data(), rhs.data(), result_of.data(), N, carry_in);

        // 3. 验证：两条独立实现链各自与 golden 比对，且彼此交叉比对
        //    （三条比较均非冗余：任一实现单独损坏都会被至少两条比较捕获）
        bool cf_ok = (memcmp(result_cf.data(), gold.data(), N * sizeof(uint64_t)) == 0);
        bool of_ok = (memcmp(result_of.data(), gold.data(), N * sizeof(uint64_t)) == 0);
        bool both_ok = (memcmp(result_cf.data(), result_of.data(), N * sizeof(uint64_t)) == 0);
        bool data_ok = cf_ok && of_ok && both_ok;

        // 4. 一致性测试
        memcpy(store_buf.data(), result_cf.data(), N * sizeof(uint64_t));
        bool consistent = (memcmp(store_buf.data(), result_cf.data(), N * sizeof(uint64_t)) == 0);

        if (!(data_ok && consistent)) {
            /* 首失配下标定位（CodeRabbit 意见）：三条比较中任一失配的首个 i */
            int bad_i = -1;
            for (size_t i = 0; i < N; ++i) {
                if (result_cf[i] != gold[i] || result_of[i] != gold[i] ||
                    result_cf[i] != result_of[i]) {
                    bad_i = (int)i;
                    break;
                }
            }
            if (bad_i >= 0) {
                report_fail_msg("adcx_adox_interleaved: first mismatch at i=%d, carry_in=%lu, "
                                "lhs=0x%016lx, rhs=0x%016lx, result_cf=0x%016lx, result_of=0x%016lx, "
                                "gold=0x%016lx, xor_cf=0x%016lx, xor_of=0x%016lx",
                                bad_i, (unsigned long)carry_in,
                                (unsigned long)lhs[bad_i], (unsigned long)rhs[bad_i],
                                (unsigned long)result_cf[bad_i], (unsigned long)result_of[bad_i],
                                (unsigned long)gold[bad_i],
                                (unsigned long)(result_cf[bad_i] ^ gold[bad_i]),
                                (unsigned long)(result_of[bad_i] ^ gold[bad_i]));
            }
            print_colored_result("adcx_adox_interleaved", false,
                                 lhs.data(), rhs.data(),
                                 result_cf.data(), result_of.data(),
                                 gold.data(), N);
            report_fail_msg("adcx_adox_interleaved: cf_ok=%d, of_ok=%d, both_ok=%d, consistent=%d",
                            cf_ok, of_ok, both_ok, consistent);
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int adcx_adox_interleaved_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

// ============================================================================
// 测试声明（确保测试名称唯一，与 adcx 区分）
// ============================================================================

DECLARE_TEST(adcx_adox_interleaved,
             "Interleaved unsigned add with overflow (adox) and unsigned add with carry (adcx) on ARM64")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = adcx_adox_interleaved_init,
    .test_run = adcx_adox_interleaved_run,
    .test_cleanup = adcx_adox_interleaved_finish,
    .fracture_loop_count = 4,
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
