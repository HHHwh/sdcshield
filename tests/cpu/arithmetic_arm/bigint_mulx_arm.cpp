// 文件：tests/cpu/arithmetic_arm/bigint_mulx_arm.cpp
/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test bigint_mulx_arm
 * @parblock
 * 512-bit large integer multiplication via the GMP library (ARM64).
 * Operands are re-rolled every iteration from the framework RNG; the golden
 * low-half product is computed by an independent scalar schoolbook multiply
 * (unsigned __int128), while the DUT path is GMP's mpz_mul (UMULL inside
 * mpn_mul_basecase). Pattern follows Intel OpenDCDiag's Eigen-based design.
 *
 * Logging follows SDCShield convention: pass path is silent; on mismatch the
 * failing inputs (a, b) and actual-vs-golden outputs are dumped via log_data()
 * before the thread is marked failed via report_fail_msg().
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <gmp.h>

static constexpr int NUM_WORDS = 8;          // 512 位 = 8 × 64 位
static constexpr int BITS = 512;

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    // Operands and golden are per-thread stack buffers re-rolled every
    // iteration in run (framework RNG); nothing shared is needed anymore.
};
}

// 辅助：GMP -> uint64_t 数组（小端序），只取低 low_words 个字
static void mpz_to_words_trunc(uint64_t *words, const mpz_t src, int num_words) {
    // 截断 src 到 num_words * 64 位
    mpz_t tmp;
    mpz_init(tmp);
    mpz_mod_2exp(tmp, src, num_words * 64);
    memset(words, 0, num_words * sizeof(uint64_t));
    mpz_export(words, nullptr, -1, sizeof(uint64_t), 0, 0, tmp);
    mpz_clear(tmp);
}

// 辅助：uint64_t 数组 -> GMP
static void words_to_mpz(mpz_t dst, const uint64_t *words, int num_words) {
    mpz_import(dst, num_words, -1, sizeof(uint64_t), 0, 0, words);
}

// 独立标量黄金：__int128 schoolbook 乘法（与 GMP 的 mpn/UMULL 路径不同实现，
// 参考 adcx.cpp 的 __int128 golden vs ADCS 汇编 DUT 模式）。已在本机用 10000
// 组随机 512 位输入验证与 GMP 逐位一致（低 512 位）。
static void schoolbook_mul_golden(const uint64_t *a, const uint64_t *b, uint64_t *out) {
    uint64_t acc[2 * NUM_WORDS] = {0};
    for (int i = 0; i < NUM_WORDS; ++i) {
        unsigned __int128 carry = 0;
        for (int j = 0; j < NUM_WORDS; ++j) {
            unsigned __int128 cur = (unsigned __int128)a[i] * b[j] + acc[i + j] + carry;
            acc[i + j] = (uint64_t)cur;
            carry = cur >> 64;
        }
        acc[i + NUM_WORDS] += (uint64_t)carry;
    }
    memcpy(out, acc, NUM_WORDS * sizeof(uint64_t));
}

// 初始化：操作数已改为 run 内每迭代重掷（框架 RNG 按线程独立流），init 只
// 分配共享的向量长度。原实现用固定 mt19937_64 种子，所有机器所有运行字节相同。
static int bigint_mulx_arm_init(struct test *test) {
    auto *data = new TestData;
    if (!data) return EXIT_FAILURE;

    test->data = data;
    return EXIT_SUCCESS;
}

// 运行测试：每次迭代重掷操作数 + GMP 重算 golden 并比较，使用线程局部缓冲
static int bigint_mulx_arm_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<TestData*>(test->data);
    if (!data) return EXIT_FAILURE;

    // 每线程局部操作数与 golden（NUM_WORDS=8，栈上即可；避免共享 TestData
    // 的并发写竞争）
    uint64_t a[NUM_WORDS];
    uint64_t b[NUM_WORDS];
    uint64_t golden[NUM_WORDS];

    mpz_t a_mpz, b_mpz, product;
    mpz_init(a_mpz);
    mpz_init(b_mpz);
    mpz_init(product);

    do {
        // 每迭代重掷操作数（random64 按线程独立流）；golden 同迭代由独立的
        // 标量 schoolbook 路径算出，受测路径用 GMP mpz_mul 复算比对
        for (int i = 0; i < NUM_WORDS; ++i) {
            a[i] = random64();
            b[i] = random64();
        }

        // 独立标量黄金（__int128 schoolbook，非 GMP 路径）
        schoolbook_mul_golden(a, b, golden);

        // 受测路径：GMP mpz_mul（内部 mpn_mul_basecase 的 UMULL 64x64->128）
        words_to_mpz(a_mpz, a, NUM_WORDS);
        words_to_mpz(b_mpz, b, NUM_WORDS);
        mpz_mul(product, a_mpz, b_mpz);

        uint64_t result[NUM_WORDS];
        mpz_to_words_trunc(result, product, NUM_WORDS);

        // 与黄金结果比较
        bool data_ok = true;
        int mismatch_word = 0;
        for (int i = 0; i < NUM_WORDS; ++i) {
            if (result[i] != golden[i]) {
                data_ok = false;
                mismatch_word = i;
                break;
            }
        }

        // 存储一致性测试
        uint64_t store_buf[NUM_WORDS];
        memcpy(store_buf, result, sizeof(store_buf));
        uint64_t reload_buf[NUM_WORDS];
        memcpy(reload_buf, store_buf, sizeof(store_buf));
        bool consistent = (memcmp(reload_buf, result, sizeof(result)) == 0);

        bool passed = data_ok && consistent;

        if (!passed) {
            // Fail 详查：把本次输入(a,b)与输出(result,golden)落进 yaml 的 data: 字段，
            // 再用 report_fail_msg 记录失败位置（框架自动标 thread failed）。
            // report_fail_msg 是 noreturn，调用前先释放 GMP 资源。
            char ctx[160];
            snprintf(ctx, sizeof(ctx),
                     "bigint_mulx_arm: word %d mismatch (result=0x%016llX "
                     "golden=0x%016llX) consistent=%d",
                     mismatch_word,
                     (unsigned long long)result[mismatch_word],
                     (unsigned long long)golden[mismatch_word],
                     (int)consistent);
            log_data("bigint_mulx input a (512-bit LE limbs)", a, NUM_WORDS * sizeof(uint64_t));
            log_data("bigint_mulx input b (512-bit LE limbs)", b, NUM_WORDS * sizeof(uint64_t));
            log_data("bigint_mulx output result (mpz_mul low 512-bit)", result, NUM_WORDS * sizeof(uint64_t));
            log_data("bigint_mulx golden (__int128 schoolbook low 512-bit)",
                     golden, NUM_WORDS * sizeof(uint64_t));
            mpz_clear(a_mpz);
            mpz_clear(b_mpz);
            mpz_clear(product);
            report_fail_msg("%s", ctx);
            // report_fail_msg 不返回
        }

    } while (test_time_condition(test));

    mpz_clear(a_mpz);
    mpz_clear(b_mpz);
    mpz_clear(product);
    return EXIT_SUCCESS;
}

static int bigint_mulx_arm_finish(struct test *test) {
    auto *data = static_cast<TestData*>(test->data);
    delete data;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

DECLARE_TEST(bigint_mulx_arm,
             "512b integer multiplication using GMP (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = bigint_mulx_arm_init,
    .test_run = bigint_mulx_arm_run,
    .test_cleanup = bigint_mulx_arm_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
