/**
 * @file adcx_adox_interleaved_arm.cpp
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test adcx_adox_interleaved_arm
 * @parblock
 * Testing interleaved ADC/ADCS chains via GMP library (simulating dual carry chains).
 * Each iteration processes two independent large integer additions (a0+b0 and a1+b1)
 * to simulate the interleaved execution of ADCX/ADOX on x86. Golden results are
 * precomputed in init using pure C++ (__int128) simulation. Each run computes
 * both additions using GMP (which uses ADC/ADCS) and compares against the
 * precomputed goldens. Pattern follows Intel OpenDCDiag's Eigen-based design.
 *
 * Logging follows SDCShield convention: pass path is silent; on mismatch the
 * failing inputs (a0,b0,a1,b1) and actual-vs-golden outputs are dumped via
 * log_data() before the thread is marked failed via report_fail_msg().
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

// 测试配置：每个大整数由 4 个 64-bit 字组成（256 位）
static constexpr size_t NUM_WORDS = 4;
static constexpr size_t NUM_GROUPS = 512;   // 每组包含两个独立加法

// 测试数据封装：所有线程只读共享
struct AdcxAdoxInterleavedTestData {
    std::vector<uint64_t> a0_words;      // 第一对加法 操作数1
    std::vector<uint64_t> b0_words;      // 第一对加法 操作数2
    std::vector<uint64_t> a1_words;      // 第二对加法 操作数1
    std::vector<uint64_t> b1_words;      // 第二对加法 操作数2
    std::vector<uint64_t> golden0_words; // 第一对加法黄金结果（含进位）
    std::vector<uint64_t> golden1_words; // 第二对加法黄金结果（含进位）
    size_t num_groups;
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
    res_words[NUM_WORDS] = (uint64_t)carry;
    carry_out = (uint64_t)carry;
}

// ============================================================================
// 初始化测试数据（生成随机 256-bit 大整数，并预计算两组黄金结果）
// 所有线程共享同一份只读数据。
// ============================================================================

static int adcx_adox_interleaved_arm_init(struct test *test) {
    try {
        auto data = std::make_unique<AdcxAdoxInterleavedTestData>();
        data->num_groups = NUM_GROUPS;

        // 分配存储空间（每组两个加法，每个加法两个操作数，每个操作数 NUM_WORDS 个字）
        data->a0_words.resize(NUM_GROUPS * NUM_WORDS);
        data->b0_words.resize(NUM_GROUPS * NUM_WORDS);
        data->a1_words.resize(NUM_GROUPS * NUM_WORDS);
        data->b1_words.resize(NUM_GROUPS * NUM_WORDS);
        data->golden0_words.resize(NUM_GROUPS * (NUM_WORDS + 1));
        data->golden1_words.resize(NUM_GROUPS * (NUM_WORDS + 1));

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

        for (size_t i = 0; i < NUM_GROUPS; ++i) {
            // ---- 第一对加法 (a0 + b0) ----
            mpz_urandomb(a_tmp, rng_state, NUM_WORDS * 64);
            mpz_urandomb(b_tmp, rng_state, NUM_WORDS * 64);
            mpz_export(&data->a0_words[i * NUM_WORDS], nullptr, -1, sizeof(uint64_t), 0, 0, a_tmp);
            mpz_export(&data->b0_words[i * NUM_WORDS], nullptr, -1, sizeof(uint64_t), 0, 0, b_tmp);

            // 计算黄金结果（软件模拟）
            uint64_t sw_words[NUM_WORDS + 1];
            uint64_t carry;
            software_add_words(&data->a0_words[i * NUM_WORDS],
                               &data->b0_words[i * NUM_WORDS],
                               sw_words, carry);
            memcpy(&data->golden0_words[i * (NUM_WORDS + 1)], sw_words, (NUM_WORDS + 1) * sizeof(uint64_t));

            // ---- 第二对加法 (a1 + b1) ----
            mpz_urandomb(a_tmp, rng_state, NUM_WORDS * 64);
            mpz_urandomb(b_tmp, rng_state, NUM_WORDS * 64);
            mpz_export(&data->a1_words[i * NUM_WORDS], nullptr, -1, sizeof(uint64_t), 0, 0, a_tmp);
            mpz_export(&data->b1_words[i * NUM_WORDS], nullptr, -1, sizeof(uint64_t), 0, 0, b_tmp);

            software_add_words(&data->a1_words[i * NUM_WORDS],
                               &data->b1_words[i * NUM_WORDS],
                               sw_words, carry);
            memcpy(&data->golden1_words[i * (NUM_WORDS + 1)], sw_words, (NUM_WORDS + 1) * sizeof(uint64_t));
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

static int adcx_adox_interleaved_arm_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<AdcxAdoxInterleavedTestData*>(test->data);

    // 每个线程独立缓冲区
    uint64_t a0_words[NUM_WORDS], b0_words[NUM_WORDS];
    uint64_t a1_words[NUM_WORDS], b1_words[NUM_WORDS];
    uint64_t hw0_words[NUM_WORDS + 1], hw1_words[NUM_WORDS + 1];
    mpz_t a_mpz, b_mpz, result_hw;
    mpz_init(a_mpz);
    mpz_init(b_mpz);
    mpz_init(result_hw);

    // 用于一致性测试的 store/load 缓冲区
    uint64_t store_buf[NUM_WORDS + 1];
    uint64_t reload_buf[NUM_WORDS + 1];

    do {
        // 遍历所有组
        for (size_t i = 0; i < td->num_groups; ++i) {
            // 1. 加载第一对输入
            memcpy(a0_words, &td->a0_words[i * NUM_WORDS], NUM_WORDS * sizeof(uint64_t));
            memcpy(b0_words, &td->b0_words[i * NUM_WORDS], NUM_WORDS * sizeof(uint64_t));
            // 加载第二对输入
            memcpy(a1_words, &td->a1_words[i * NUM_WORDS], NUM_WORDS * sizeof(uint64_t));
            memcpy(b1_words, &td->b1_words[i * NUM_WORDS], NUM_WORDS * sizeof(uint64_t));

            // 2. 第一对加法：硬件执行 (GMP)
            mpz_import(a_mpz, NUM_WORDS, -1, sizeof(uint64_t), 0, 0, a0_words);
            mpz_import(b_mpz, NUM_WORDS, -1, sizeof(uint64_t), 0, 0, b0_words);
            mpz_add(result_hw, a_mpz, b_mpz);
            memset(hw0_words, 0, (NUM_WORDS + 1) * sizeof(uint64_t));
            size_t count;
            mpz_export(hw0_words, &count, -1, sizeof(uint64_t), 0, 0, result_hw);

            // 3. 第二对加法：硬件执行 (GMP)
            mpz_import(a_mpz, NUM_WORDS, -1, sizeof(uint64_t), 0, 0, a1_words);
            mpz_import(b_mpz, NUM_WORDS, -1, sizeof(uint64_t), 0, 0, b1_words);
            mpz_add(result_hw, a_mpz, b_mpz);
            memset(hw1_words, 0, (NUM_WORDS + 1) * sizeof(uint64_t));
            mpz_export(hw1_words, &count, -1, sizeof(uint64_t), 0, 0, result_hw);

            // 4. 与预存黄金结果比较
            const uint64_t *golden0 = &td->golden0_words[i * (NUM_WORDS + 1)];
            const uint64_t *golden1 = &td->golden1_words[i * (NUM_WORDS + 1)];
            bool data0_ok = true, data1_ok = true;
            size_t mw0 = 0, mw1 = 0;
            for (size_t j = 0; j < NUM_WORDS + 1; ++j) {
                if (hw0_words[j] != golden0[j]) { data0_ok = false; mw0 = j; }
                if (hw1_words[j] != golden1[j]) { data1_ok = false; mw1 = j; }
            }
            bool data_ok = data0_ok && data1_ok;

            // 5. 存储一致性测试（对两对结果都做）
            memcpy(store_buf, hw0_words, (NUM_WORDS + 1) * sizeof(uint64_t));
            memcpy(reload_buf, store_buf, (NUM_WORDS + 1) * sizeof(uint64_t));
            bool consistent = true;
            for (size_t j = 0; j < NUM_WORDS + 1; ++j) {
                if (hw0_words[j] != reload_buf[j]) { consistent = false; break; }
            }
            memcpy(store_buf, hw1_words, (NUM_WORDS + 1) * sizeof(uint64_t));
            memcpy(reload_buf, store_buf, (NUM_WORDS + 1) * sizeof(uint64_t));
            bool consistent1 = true;
            for (size_t j = 0; j < NUM_WORDS + 1; ++j) {
                if (hw1_words[j] != reload_buf[j]) { consistent1 = false; break; }
            }
            consistent = consistent && consistent1;

            if (!data_ok || !consistent) {
                // Fail 详查：把两组输入(a0,b0,a1,b1)与输出(hw0,hw1,golden0,golden1)落进 yaml，
                // 再用 report_fail_msg 记录失败位置（框架自动标 thread failed）。report_fail_msg 是
                // noreturn，调用前先释放 GMP 资源。只报真正 mismatch 的 pair，避免误报。
                char ctx[240];
                if (!data0_ok && !data1_ok) {
                    snprintf(ctx, sizeof(ctx),
                             "adcx_adox_interleaved_arm group %zu: pair0 word %zu mismatch "
                             "(hw0=0x%016" PRIx64 " gold0=0x%016" PRIx64 "); pair1 word %zu mismatch "
                             "(hw1=0x%016" PRIx64 " gold1=0x%016" PRIx64 "); consistent=%d",
                             i, mw0, hw0_words[mw0], golden0[mw0],
                             mw1, hw1_words[mw1], golden1[mw1], (int)consistent);
                } else if (!data0_ok) {
                    snprintf(ctx, sizeof(ctx),
                             "adcx_adox_interleaved_arm group %zu: pair0 word %zu mismatch "
                             "(hw0=0x%016" PRIx64 " gold0=0x%016" PRIx64 "); pair1 ok; consistent=%d",
                             i, mw0, hw0_words[mw0], golden0[mw0], (int)consistent);
                } else if (!data1_ok) {
                    snprintf(ctx, sizeof(ctx),
                             "adcx_adox_interleaved_arm group %zu: pair0 ok; pair1 word %zu mismatch "
                             "(hw1=0x%016" PRIx64 " gold1=0x%016" PRIx64 "); consistent=%d",
                             i, mw1, hw1_words[mw1], golden1[mw1], (int)consistent);
                } else {
                    snprintf(ctx, sizeof(ctx),
                             "adcx_adox_interleaved_arm group %zu: data ok but consistency failed; consistent=%d",
                             i, (int)consistent);
                }
                log_data("adcx_adox_interleaved input a0 (256-bit LE limbs)",
                         a0_words, NUM_WORDS * sizeof(uint64_t));
                log_data("adcx_adox_interleaved input b0 (256-bit LE limbs)",
                         b0_words, NUM_WORDS * sizeof(uint64_t));
                log_data("adcx_adox_interleaved input a1 (256-bit LE limbs)",
                         a1_words, NUM_WORDS * sizeof(uint64_t));
                log_data("adcx_adox_interleaved input b1 (256-bit LE limbs)",
                         b1_words, NUM_WORDS * sizeof(uint64_t));
                log_data("adcx_adox_interleaved output hw0 (mpz_add result)",
                         hw0_words, (NUM_WORDS + 1) * sizeof(uint64_t));
                log_data("adcx_adox_interleaved golden0 (__int128 result)",
                         golden0, (NUM_WORDS + 1) * sizeof(uint64_t));
                log_data("adcx_adox_interleaved output hw1 (mpz_add result)",
                         hw1_words, (NUM_WORDS + 1) * sizeof(uint64_t));
                log_data("adcx_adox_interleaved golden1 (__int128 result)",
                         golden1, (NUM_WORDS + 1) * sizeof(uint64_t));
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

static int adcx_adox_interleaved_arm_finish(struct test *test) {
    delete static_cast<AdcxAdoxInterleavedTestData*>(test->data);
    return EXIT_SUCCESS;
}

// ============================================================================
// 测试注册
// ============================================================================

DECLARE_TEST(adcx_adox_interleaved_arm, "Interleaved ADC/ADCS chains via GMP (simulating dual carry chains)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = adcx_adox_interleaved_arm_init,
    .test_run = adcx_adox_interleaved_arm_run,
    .test_cleanup = adcx_adox_interleaved_arm_finish,
    .fracture_loop_count = 4,
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
