/**
 * @file adcxlong_arm.cpp
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test adcxlong_arm
 * @parblock
 * Testing long ADC/ADCS chains via GMP library.
 * Uses 1024-bit integers (16 words) to create long carry chains.
 * Golden result is precomputed in init using pure C++ (__int128) simulation.
 * Each run computes the result using GMP (which uses ADC/ADCS) and compares
 * against the precomputed golden. Pattern follows Intel OpenDCDiag's Eigen-based design.
 *
 * Logging follows the SDCShield convention: the pass path is silent; on a
 * mismatch the failing inputs (a, b) and the actual-vs-golden outputs are
 * dumped to the log via log_data() before the thread is marked failed via
 * report_fail_msg(), which records the failure location in the YAML.
 * @endparblock
 */

#include <sandstone.h>
#include <gmp.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cinttypes>
#include <memory>

// 测试配置：每个大整数由 16 个 64-bit 字组成（1024 位）
static constexpr size_t NUM_WORDS = 16;
static constexpr size_t NUM_PAIRS = 512;   // 每批测试的数据对数量（减少以保持总时间可控）

// 测试数据封装（类似 EigenSparseTestData）：所有线程只读共享
struct AdcxlongTestData {
    std::vector<uint64_t> a_words;      // 所有 a 的单词展开存储
    std::vector<uint64_t> b_words;      // 所有 b 的单词展开存储
    std::vector<uint64_t> golden_words; // 所有黄金结果的单词展开存储（包含进位）
    size_t num_pairs;
};

// ============================================================================
// 黄金参考：软件模拟大整数加法（使用 __int128 模拟进位链）
// 返回结果 + 进位（扩展为 NUM_WORDS+1 个单词）
// ============================================================================

static void software_add_words(const uint64_t *a_words, const uint64_t *b_words,
                               uint64_t *res_words, uint64_t &carry_out) {
    unsigned __int128 carry = 0;
    for (size_t i = 0; i < NUM_WORDS; ++i) {
        unsigned __int128 sum = (unsigned __int128)a_words[i] + b_words[i] + carry;
        res_words[i] = (uint64_t)sum;
        carry = sum >> 64;
    }
    res_words[NUM_WORDS] = (uint64_t)carry;  // 保存进位到扩展单词
    carry_out = (uint64_t)carry;
}

// ============================================================================
// 初始化测试数据（生成随机 1024-bit 大整数，并预计算黄金结果）
// 所有线程共享同一份只读数据。
// ============================================================================

static int adcxlong_arm_init(struct test *test) {
    try {
        auto data = std::make_unique<AdcxlongTestData>();
        data->num_pairs = NUM_PAIRS;

        // 分配存储空间
        data->a_words.resize(NUM_PAIRS * NUM_WORDS);
        data->b_words.resize(NUM_PAIRS * NUM_WORDS);
        data->golden_words.resize(NUM_PAIRS * (NUM_WORDS + 1)); // 加一个进位位

        // 生成随机数据（使用 GMP 生成大整数，然后导出为单词）
        // B1-006: seed GMP's randstate from the framework RNG (per-thread
        // stream, reproducible via the reported seed / -s) instead of
        // std::random_device, so the operand batch can be replayed.
        gmp_randstate_t rng_state;
        gmp_randinit_default(rng_state);
        gmp_randseed_ui(rng_state, static_cast<unsigned long>(random64()));

        mpz_t a_tmp, b_tmp;
        mpz_init(a_tmp);
        mpz_init(b_tmp);

        for (size_t i = 0; i < NUM_PAIRS; ++i) {
            // 生成随机 1024 位数
            mpz_urandomb(a_tmp, rng_state, NUM_WORDS * 64);
            mpz_urandomb(b_tmp, rng_state, NUM_WORDS * 64);

            // 导出 a 和 b 的单词
            size_t count_a, count_b;
            mpz_export(&data->a_words[i * NUM_WORDS], &count_a, -1, sizeof(uint64_t), 0, 0, a_tmp);
            mpz_export(&data->b_words[i * NUM_WORDS], &count_b, -1, sizeof(uint64_t), 0, 0, b_tmp);

            // 计算黄金结果（软件模拟）
            uint64_t sw_words[NUM_WORDS + 1];
            uint64_t carry;
            software_add_words(&data->a_words[i * NUM_WORDS],
                               &data->b_words[i * NUM_WORDS],
                               sw_words, carry);
            // 存储黄金结果
            memcpy(&data->golden_words[i * (NUM_WORDS + 1)], sw_words, (NUM_WORDS + 1) * sizeof(uint64_t));
        }

        mpz_clear(a_tmp);
        mpz_clear(b_tmp);
        gmp_randclear(rng_state);

        test->data = data.release();
        return EXIT_SUCCESS;

    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory, "Exception during init: %s", e.what());
        return EXIT_SKIP;
    }
}

// ============================================================================
// 运行测试（每个核心独立执行，使用线程局部缓冲）
// ============================================================================

static int adcxlong_arm_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<AdcxlongTestData*>(test->data);

    // 每个线程独立缓冲区，避免竞争
    uint64_t a_words[NUM_WORDS];
    uint64_t b_words[NUM_WORDS];
    uint64_t hw_words[NUM_WORDS + 1];
    mpz_t a_mpz, b_mpz, result_hw;
    mpz_init(a_mpz);
    mpz_init(b_mpz);
    mpz_init(result_hw);

    // 用于一致性测试的 store/load 缓冲区
    uint64_t store_buf[NUM_WORDS + 1];
    uint64_t reload_buf[NUM_WORDS + 1];

    do {
        // 遍历所有数据对
        for (size_t i = 0; i < td->num_pairs; ++i) {
            // 1. 从共享数据中加载输入
            memcpy(a_words, &td->a_words[i * NUM_WORDS], NUM_WORDS * sizeof(uint64_t));
            memcpy(b_words, &td->b_words[i * NUM_WORDS], NUM_WORDS * sizeof(uint64_t));

            // 2. 构建 GMP 大整数（从单词导入）
            mpz_import(a_mpz, NUM_WORDS, -1, sizeof(uint64_t), 0, 0, a_words);
            mpz_import(b_mpz, NUM_WORDS, -1, sizeof(uint64_t), 0, 0, b_words);

            // 3. 硬件执行：使用 GMP 的 mpz_add（内部会调用 ADC/ADCS）
            mpz_add(result_hw, a_mpz, b_mpz);

            // 4. 将硬件结果导出为字数组（包括可能的进位）
            memset(hw_words, 0, (NUM_WORDS + 1) * sizeof(uint64_t));
            size_t count;
            mpz_export(hw_words, &count, -1, sizeof(uint64_t), 0, 0, result_hw);
            // 如果 count > NUM_WORDS，则高位有进位；否则高位为0

            // 5. 与预存黄金结果比较（包括进位）
            const uint64_t *golden = &td->golden_words[i * (NUM_WORDS + 1)];
            bool data_ok = true;
            size_t mismatch_word = 0;
            for (size_t j = 0; j < NUM_WORDS + 1; ++j) {
                if (hw_words[j] != golden[j]) {
                    data_ok = false;
                    mismatch_word = j;
                    break;
                }
            }

            // 6. 存储一致性测试（Store/Load）
            memcpy(store_buf, hw_words, (NUM_WORDS + 1) * sizeof(uint64_t));
            memcpy(reload_buf, store_buf, (NUM_WORDS + 1) * sizeof(uint64_t));
            bool consistent = true;
            for (size_t j = 0; j < NUM_WORDS + 1; ++j) {
                if (hw_words[j] != reload_buf[j]) {
                    consistent = false;
                    break;
                }
            }

            if (!data_ok || !consistent) {
                // Fail 详查：把本次输入(a,b)与输出(hw,golden)落进 yaml 的 data: 字段，
                // 再用 report_fail_msg 记录失败位置（框架自动标 thread failed）。
                // report_fail_msg 是 noreturn，调用前先释放 GMP 资源。
                char ctx[160];
                snprintf(ctx, sizeof(ctx),
                         "adcxlong_arm pair %zu: word %zu mismatch (hw=0x%016" PRIx64
                         " golden=0x%016" PRIx64 ") consistent=%d",
                         i, mismatch_word, hw_words[mismatch_word],
                         golden[mismatch_word], (int)consistent);
                log_data("adcxlong_arm input a (1024-bit, little-endian limbs)",
                         a_words, NUM_WORDS * sizeof(uint64_t));
                log_data("adcxlong_arm input b (1024-bit, little-endian limbs)",
                         b_words, NUM_WORDS * sizeof(uint64_t));
                log_data("adcxlong_arm output hw (mpz_add result)",
                         hw_words, (NUM_WORDS + 1) * sizeof(uint64_t));
                log_data("adcxlong_arm golden (software __int128 result)",
                         golden, (NUM_WORDS + 1) * sizeof(uint64_t));
                mpz_clear(a_mpz);
                mpz_clear(b_mpz);
                mpz_clear(result_hw);
                report_fail_msg("%s", ctx);
                // report_fail_msg 不返回
            }
        }

    } while (test_time_condition(test));

    mpz_clear(a_mpz);
    mpz_clear(b_mpz);
    mpz_clear(result_hw);
    return EXIT_SUCCESS;
}

// ============================================================================
// 清理资源
// ============================================================================

static int adcxlong_arm_finish(struct test *test) {
    delete static_cast<AdcxlongTestData*>(test->data);
    return EXIT_SUCCESS;
}

// ============================================================================
// 测试注册
// ============================================================================

DECLARE_TEST(adcxlong_arm, "Testing long ADC/ADCS chains via GMP with precomputed golden (1024-bit)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = adcxlong_arm_init,
    .test_run = adcxlong_arm_run,
    .test_cleanup = adcxlong_arm_finish,
    .fracture_loop_count = 4,
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
