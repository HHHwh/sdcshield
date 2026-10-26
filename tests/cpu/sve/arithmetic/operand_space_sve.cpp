/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test operand_space_sve
 * @parblock
 * SVE port of operand_space_arm: operand-space SDC stressor feeding the
 * adder / carry-chain / multiplier with high-Hamming replicated words.
 * The 256-bit adds run on SVE svadd lanes with the serial carry fold
 * (GMP replaced); the 64x64->128 multiplies run on svmul + svmulh
 * (high-half via __int128-fold — svmulh is SVE2, absent on SVE1, so the
 * high half folds scalar __int128 on the same operand stream); the
 * store-to-load-forwarding probe stays the original inline-asm str/ldr
 * (a scalar datapath by design). Goldens are the __int128 software
 * references exactly as the original.
 * The operand-pair count (originally NUM_PAIRS=80) is runtime
 * configurable via -O operand_space_sve.num_pairs=N /
 * SDC_OPERAND_SPACE_SVE_NUM_PAIRS, with a memory-adaptive default
 * when unconfigured. NUM_WORDS=4 (the 256-bit operand width) stays
 * fixed.
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

static constexpr size_t NUM_WORDS = 4;
static constexpr size_t NUM_PAIRS = 80;

static const uint64_t HAMMING_TABLE[] = {
    0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
    0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL,
    0xCCCCCCCCCCCCCCCCULL, 0x3333333333333333ULL,
    0xF0F0F0F0F0F0F0F0ULL, 0x0F0F0F0F0F0F0F0FULL,
    0xFFFF0000FFFF0000ULL, 0x0000FFFF0000FFFFULL,
    0xFFFFFF00FFFFFF00ULL, 0x00FFFFFF00FFFFFFULL,
    0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFULL,
    0x0000000000000001ULL, 0xFFFFFFFFFFFFFFFEULL,
    0x123456789ABCDEF0ULL,
};
static constexpr size_t HAMMING_TABLE_SIZE =
    sizeof(HAMMING_TABLE) / sizeof(HAMMING_TABLE[0]);

struct OperandSpaceSveData {
    std::vector<uint64_t> a_words;
    std::vector<uint64_t> b_words;
    std::vector<uint64_t> golden_add;
    std::vector<uint64_t> golden_mul_lo;
    std::vector<uint64_t> golden_mul_hi;
    size_t num_pairs;
};

/* 操作数对数 NUM_PAIRS(80) — 运行时可配置, 统一优先级链:
 *   1. -O operand_space_sve.num_pairs=N / 配置文件同名条目
 *   2. 环境变量 SDC_OPERAND_SPACE_SVE_NUM_PAIRS
 *   3. 自适应: a_words/b_words/golden_add/golden_mul_lo/golden_mul_hi
 *      共 5 份共享向量 (实际 15×8B=120B/对, 按规格 bytes_per_elem=8×
 *      arrays=5 均摊), rss_ratio 1.0, 占物理内存 60%, clamp [80, 1M]。
 *      原 80 为下限。NUM_WORDS=4 (256-bit 操作数宽度) 固定不动。 */
static size_t operand_space_sve_num_pairs(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "num_pairs", 0, NUM_PAIRS,
                                     1 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("operand_space_sve", "num_pairs", &v,
                                 NUM_PAIRS, 1 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count(8, 5, 1.0, 600, thread_count(),
                                         NUM_PAIRS, 1 << 20, test->id, "num_pairs");
}

static void software_add_words(const uint64_t *a, const uint64_t *b, uint64_t *res)
{
    unsigned __int128 carry = 0;
    for (size_t i = 0; i < NUM_WORDS; ++i) {
        unsigned __int128 sum = (unsigned __int128)a[i] + b[i] + carry;
        res[i] = (uint64_t)sum;
        carry = sum >> 64;
    }
    res[NUM_WORDS] = (uint64_t)carry;
}

static inline void fill_operand(uint64_t *dst, uint64_t base_word)
{
    for (size_t i = 0; i < NUM_WORDS; ++i)
        dst[i] = base_word;
}

#if defined(__aarch64__)
/* SVE 256-bit add: svadd lanes + serial carry fold (equivalence verified) */
static void sve_add_words(const uint64_t *a, const uint64_t *b, uint64_t *res)
{
    uint64_t c = 0;
    const int lanes = svcntd();
    for (size_t base = 0; base < NUM_WORDS; base += lanes) {
        int cnt = (NUM_WORDS - base < (size_t)lanes) ? (int)(NUM_WORDS - base) : lanes;
        svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)cnt);
        svuint64_t va = svld1_u64(pg, a + base);
        svuint64_t vb = svld1_u64(pg, b + base);
        uint64_t vsum[8];
        svst1_u64(pg, vsum, svadd_u64_x(pg, va, vb));
        for (int i = 0; i < cnt; ++i) {
            unsigned __int128 s = (unsigned __int128)vsum[i] + c;
            res[base + i] = (uint64_t)s;
            uint64_t cin_ab = (vsum[i] < a[base + i]) ? 1u : 0u;
            c = cin_ab + (uint64_t)(s >> 64);
        }
    }
    res[NUM_WORDS] = c;
}

/* 64x64->128 乘: svmul 低半 (真 SVE 乘法) + __int128 高半折叠
 * (svmulh 是 SVE2 指令, 本机 SVE1 无) */
static void sve_mul_words(uint64_t a, uint64_t b, uint64_t *lo, uint64_t *hi)
{
    svbool_t pg = svptrue_b64();
    uint64_t ai[4] = {a, a, a, a}, bi[4] = {b, b, b, b}, out[4];
    svst1_u64(pg, out, svmul_u64_x(pg, svld1_u64(pg, ai), svld1_u64(pg, bi)));
    *lo = out[0];
    unsigned __int128 prod = (unsigned __int128)a * b;
    *hi = (uint64_t)(prod >> 64);
    (void)prod;
}

/* slf 探针保持原版 inline-asm str/ldr (标量转发通路) */
static inline uint64_t slf_probe(const uint64_t *src, uint64_t *buf, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        __asm__ volatile("str %1, [%0, %2]"
                         : : "r"(buf), "r"(src[i]), "r"(i * sizeof(uint64_t))
                         : "memory");
    }
    uint64_t acc = 0;
    for (size_t i = 0; i < n; ++i) {
        uint64_t v;
        __asm__ volatile("ldr %0, [%1, %2]"
                         : "=r"(v)
                         : "r"(buf), "r"(i * sizeof(uint64_t))
                         : "memory");
        acc += v;
    }
    return acc;
}
#endif

static int operand_space_sve_init(struct test *test)
{
#ifdef __aarch64__
    /* All-VL Phase 2 P6: per-test auxv hand-probe -> framework
     * layer: arm64_require_sve_vl does the SVE capability check
     * + 128..1024-bit range gate in one call (skip reason
     * framework-issued). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif
    const size_t NUM_P = operand_space_sve_num_pairs(test);
    try {
        auto data = std::make_unique<OperandSpaceSveData>();
        data->num_pairs = NUM_P;
        data->a_words.resize(NUM_P * NUM_WORDS);
        data->b_words.resize(NUM_P * NUM_WORDS);
        data->golden_add.resize(NUM_P * (NUM_WORDS + 1));
        data->golden_mul_lo.resize(NUM_P);
        data->golden_mul_hi.resize(NUM_P);

        for (size_t i = 0; i < NUM_P; ++i) {
            uint64_t a_base = HAMMING_TABLE[i % HAMMING_TABLE_SIZE];
            uint64_t b_base = HAMMING_TABLE[(i + 1) % HAMMING_TABLE_SIZE];

            uint64_t a[NUM_WORDS], b[NUM_WORDS];
            fill_operand(a, a_base);
            fill_operand(b, b_base);

            memcpy(&data->a_words[i * NUM_WORDS], a, NUM_WORDS * sizeof(uint64_t));
            memcpy(&data->b_words[i * NUM_WORDS], b, NUM_WORDS * sizeof(uint64_t));

            uint64_t sum[NUM_WORDS + 1];
            software_add_words(a, b, sum);
            memcpy(&data->golden_add[i * (NUM_WORDS + 1)], sum,
                   (NUM_WORDS + 1) * sizeof(uint64_t));

            unsigned __int128 prod = (unsigned __int128)a[0] * b[0];
            data->golden_mul_lo[i] = (uint64_t)prod;
            data->golden_mul_hi[i] = (uint64_t)(prod >> 64);
        }

        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory, "operand_space_sve init exception: %s", e.what());
        return EXIT_SKIP;
    }
}

static int operand_space_sve_run(struct test *test, int cpu)
{
    (void)cpu;
#ifndef __aarch64__
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for operand_space_sve");
    return EXIT_SKIP;
#else
    auto *td = static_cast<OperandSpaceSveData *>(test->data);

    uint64_t a_words[NUM_WORDS];
    uint64_t b_words[NUM_WORDS];
    uint64_t hw_words[NUM_WORDS + 1];

    do {
        bool all_passed = true;

        for (size_t i = 0; i < td->num_pairs; ++i) {
            memcpy(a_words, &td->a_words[i * NUM_WORDS], NUM_WORDS * sizeof(uint64_t));
            memcpy(b_words, &td->b_words[i * NUM_WORDS], NUM_WORDS * sizeof(uint64_t));

            /* SVE 256-bit 加 (svadd + carry fold) */
            sve_add_words(a_words, b_words, hw_words);

            const uint64_t *golden = &td->golden_add[i * (NUM_WORDS + 1)];
            bool data_ok = (memcmp(hw_words, golden, (NUM_WORDS + 1) * sizeof(uint64_t)) == 0);

            /* store-to-load-forwarding 探针 (原版 asm) */
            uint64_t slf_buf[NUM_WORDS + 1];
            uint64_t slf_golden = 0;
            for (size_t w = 0; w < NUM_WORDS + 1; ++w)
                slf_golden += hw_words[w];
            uint64_t slf_got = slf_probe(hw_words, slf_buf, NUM_WORDS + 1);
            bool consistent = (slf_got == slf_golden);

            /* SVE 64x64->128 乘 (svmul 低半 + 高半折叠) */
            uint64_t mul_lo, mul_hi;
            sve_mul_words(a_words[0], b_words[0], &mul_lo, &mul_hi);
            bool mul_ok = (mul_lo == td->golden_mul_lo[i]) &&
                          (mul_hi == td->golden_mul_hi[i]);

            if (!(data_ok && consistent && mul_ok)) {
                log_warning("operand_space_sve: pair %zu mismatch "
                            "add_ok=%d slf_ok=%d mul_ok=%d", i,
                            (int)data_ok, (int)consistent, (int)mul_ok);
                all_passed = false;
            }
        }

        if (!all_passed) {
            report_fail_msg("operand_space_sve: operand-space SDC detected (SVE path)");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#endif
}

static int operand_space_sve_finish(struct test *test)
{
    delete static_cast<OperandSpaceSveData *>(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(operand_space_sve,
             "Operand-space SDC stressor on SVE: high-Hamming replicated words "
             "through svadd carry chains, svmul multiplies, and the original "
             "inline-asm store-to-load-forwarding probe (port of operand_space_arm)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = operand_space_sve_init,
    .test_run = operand_space_sve_run,
    .test_cleanup = operand_space_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
