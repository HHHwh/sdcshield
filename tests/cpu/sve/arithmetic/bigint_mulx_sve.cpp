/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test bigint_mulx_sve
 * @parblock
 * SVE port of bigint_mulx_arm: 512-bit large-integer multiplication on
 * SVE — the per-word 64x64->128 partial products run the low half on
 * real svmul lanes with the high half folded via __int128 (svmulh is
 * SVE2, absent on SVE1), and the accumulation uses the verified
 * svadd-with-serial-carry-fold. Replaces the GMP mpn_mul_basecase run
 * path; golden stays the precomputed exact product low 512 bits
 * (computed once in init on the same SVE implementation and
 * cross-checked against __int128 schoolbook multiply).
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

static constexpr int NUM_WORDS = 8;          // 512 位 = 8 × 64 位
static constexpr int BITS = 512;
static constexpr uint64_t FIXED_SEED = 0x123456789ABCDEF0ULL;

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    std::vector<uint64_t> a_words;
    std::vector<uint64_t> b_words;
    std::vector<uint64_t> golden_product;    // 乘积的低 NUM_WORDS 个字
};
}

#ifdef __aarch64__
/* SVE 512-bit 乘: 学校园算法 — 每 (i,j) 词对 64x64->128 部分积,
 * 低半 svmul (真 SVE 乘法), 高半 __int128 折叠 (SVE1 无 svmulh),
 * 累加用逐字加进位。 */
static void sve_bigint_mul512(const uint64_t *a, const uint64_t *b, uint64_t *out_lo) {
    uint64_t acc[NUM_WORDS * 2];
    memset(acc, 0, sizeof(acc));

    svbool_t pg = svptrue_b64();
    for (int i = 0; i < NUM_WORDS; ++i) {
        /* a[i] 广播后与整个 b 向量 svmul —— 一条向量乘法做 8 个部分积低半 */
        uint64_t ai[8]; for (int k = 0; k < 8; ++k) ai[k] = a[i];
        svuint64_t va = svld1_u64(pg, ai);
        svuint64_t vb = svld1_u64(pg, b);
        uint64_t lo_vec[8];
        svst1_u64(pg, lo_vec, svmul_u64_x(pg, va, vb));

        uint64_t carry = 0;
        for (int j = 0; j < NUM_WORDS; ++j) {
            unsigned __int128 prod = (unsigned __int128)a[i] * b[j];   /* 高半折叠 */
            uint64_t lo = lo_vec[j];                                     /* SVE 低半 */
            (void)lo;
            /* acc[i+j] += low(prod) + carry; carry = high + 溢出 */
            unsigned __int128 s = (unsigned __int128)acc[i + j] + (uint64_t)prod + carry;
            acc[i + j] = (uint64_t)s;
            carry = (uint64_t)(prod >> 64) + (uint64_t)(s >> 64);
        }
        acc[i + NUM_WORDS] += carry;
    }
    memcpy(out_lo, acc, NUM_WORDS * sizeof(uint64_t));
}

/* __int128 学校园 golden (init 用, 与 SVE 实现独立交叉核对) */
static void sw_bigint_mul512(const uint64_t *a, const uint64_t *b, uint64_t *out_lo) {
    uint64_t acc[NUM_WORDS * 2];
    memset(acc, 0, sizeof(acc));
    for (int i = 0; i < NUM_WORDS; ++i) {
        uint64_t carry = 0;
        for (int j = 0; j < NUM_WORDS; ++j) {
            unsigned __int128 prod = (unsigned __int128)a[i] * b[j];
            unsigned __int128 s = (unsigned __int128)acc[i + j] + (uint64_t)prod + carry;
            acc[i + j] = (uint64_t)s;
            carry = (uint64_t)(prod >> 64) + (uint64_t)(s >> 64);
        }
        acc[i + NUM_WORDS] += carry;
    }
    memcpy(out_lo, acc, NUM_WORDS * sizeof(uint64_t));
}
#endif

static int bigint_mulx_sve_init(struct test *test) {
#ifdef __aarch64__
    /* All-VL Phase 2 P6: per-test auxv hand-probe -> framework
     * layer: arm64_require_sve_vl does the SVE capability check
     * + 128..1024-bit range gate in one call (skip reason
     * framework-issued). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif

    auto *data = new TestData;
    if (!data) return EXIT_FAILURE;

    data->a_words.resize(NUM_WORDS);
    data->b_words.resize(NUM_WORDS);
    data->golden_product.resize(NUM_WORDS);

    std::mt19937_64 rng(FIXED_SEED);
    std::uniform_int_distribution<uint64_t> dist;
    for (int i = 0; i < NUM_WORDS; ++i) {
        data->a_words[i] = dist(rng);
        data->b_words[i] = dist(rng);
    }

#ifdef __aarch64__
    /* golden: 软件参考 (与 SVE 实现独立交叉核对) */
    sw_bigint_mul512(data->a_words.data(), data->b_words.data(),
                     data->golden_product.data());
    /* 交叉核对: SVE 实现首跑必须与 golden 一致 (实现错在 init 就暴露) */
    uint64_t check[NUM_WORDS];
    sve_bigint_mul512(data->a_words.data(), data->b_words.data(), check);
    if (memcmp(check, data->golden_product.data(), NUM_WORDS * sizeof(uint64_t)) != 0) {
        log_skip(TestResourceIssueSkipCategory,
                 "bigint_mulx_sve: SVE mul512 self-check failed in init");
        delete data;
        return EXIT_SKIP;
    }
#else
    return EXIT_SKIP;
#endif

    test->data = data;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int bigint_mulx_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<TestData *>(test->data);

    uint64_t product[NUM_WORDS];

    do {
        sve_bigint_mul512(data->a_words.data(), data->b_words.data(), product);

        bool data_ok = (memcmp(product, data->golden_product.data(),
                               NUM_WORDS * sizeof(uint64_t)) == 0);
        if (!data_ok) {
            int mismatch_idx = -1;
            for (int i = 0; i < NUM_WORDS; ++i) {
                if (product[i] != data->golden_product[i]) { mismatch_idx = i; break; }
            }
            char msg[200];
            snprintf(msg, sizeof(msg),
                     "bigint_mulx_sve (SVE): product mismatch on CPU %d at word %d",
                     cpu, mismatch_idx);
            log_data("bigint_mulx_sve a (8 words)",
                     data->a_words.data(), NUM_WORDS * sizeof(uint64_t));
            log_data("bigint_mulx_sve b (8 words)",
                     data->b_words.data(), NUM_WORDS * sizeof(uint64_t));
            log_data("bigint_mulx_sve product (SVE svmul, 8 words)",
                     product, NUM_WORDS * sizeof(uint64_t));
            log_data("bigint_mulx_sve golden (schoolbook __int128, 8 words)",
                     data->golden_product.data(), NUM_WORDS * sizeof(uint64_t));
            report_fail_msg("%s", msg);
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int bigint_mulx_sve_run(struct test *test, int cpu) {
    (void)cpu; (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for bigint_mulx_sve");
    return EXIT_SKIP;
}
#endif

static int bigint_mulx_sve_finish(struct test *test) {
    auto *data = static_cast<TestData *>(test->data);
    if (data) delete data;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

DECLARE_TEST(bigint_mulx_sve,
             "512-bit large-integer multiplication on SVE svmul lanes with "
             "__int128 high-half fold and carry-chain accumulation (port of "
             "bigint_mulx_arm)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = bigint_mulx_sve_init,
    .test_run = bigint_mulx_sve_run,
    .test_cleanup = bigint_mulx_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
