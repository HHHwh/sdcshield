/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test gmp_bignum_sve
 * @parblock
 * SVE port of gmp_bignum (multiply path): 512-bit large-integer multiplication on
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
/* per-lane 失配冷路径诊断 (PR #294 检视 round 3): 高位词对 (i+j >=
 * NUM_WORDS) 出错时 out_lo 的 8 字 memcmp 仍可能全对 —— 首个失配词对
 * + 出错 svmul lane 直方图把乘法器 SDC 的现场留下来 (健康硬件零进入,
 * 零成本)。 */
struct Mul512LaneDiag {
    bool     any;                 /* 是否出现过失配 */
    int      first_i, first_j;    /* 首个失配 (i,j) 词对 */
    uint64_t first_actual;        /* lo_row[j] — SVE svmul 低半积 */
    uint64_t first_expected;      /* (uint64_t)prod — __int128 参考低半 */
    uint64_t first_xor;           /* 位差 */
    uint32_t lane_hist[16];       /* 出错 lane (j % svcntd()) 直方图 — 门内 svcntd() <= 16 */
    uint32_t total;               /* 失配词对总数 (<= 64) */
};

/* SVE 512-bit 乘: 学校园算法 — 每 (i,j) 词对 64x64->128 部分积,
 * 低半 svmul (真 SVE 乘法), 高半 __int128 折叠 (SVE1 无 svmulh),
 * 累加用逐字加进位。返回 true = 每个 SVE 低半积都与 __int128 参考的
 * 低半逐项一致 (per-lane 校验, PR #294 检视 round 2)。 */
static bool sve_bigint_mul512(const uint64_t *a, const uint64_t *b, uint64_t *out_lo,
                              struct Mul512LaneDiag *diag) {
    uint64_t acc[NUM_WORDS * 2];
    memset(acc, 0, sizeof(acc));
    bool sve_ok = true;

    for (int i = 0; i < NUM_WORDS; ++i) {
        /* a[i] 广播后与 b 分块 svmul —— 谓词 svwhilelt 限定在 NUM_WORDS
         * 个字内: 128-bit 档 svcntd()=2 分 4 块, 512-bit 档单块 8 lane,
         * 1024-bit 档单块前 8 lane, 任意档不越界 —— 原版 svptrue 全宽
         * svld1_u64(pg, b) 在 VL>512 时越界读 8 元素堆缓冲 64 字节。 */
        uint64_t lo_row[NUM_WORDS];
        for (uint64_t j0 = 0; j0 < (uint64_t)NUM_WORDS; j0 += (uint64_t)svcntd()) {
            svbool_t pgj = svwhilelt_b64(j0, (uint64_t)NUM_WORDS);
            svuint64_t va = svdup_n_u64(a[i]);
            svuint64_t vb = svld1_u64(pgj, b + j0);
            svst1_u64(pgj, lo_row + j0, svmul_u64_x(pgj, va, vb));
        }

        uint64_t carry = 0;
        for (int j = 0; j < NUM_WORDS; ++j) {
            unsigned __int128 prod = (unsigned __int128)a[i] * b[j];   /* 高半折叠 */
            /* per-lane 独立比对 (PR #294 检视 round 2): SVE 积进入累加器
             * 前逐项对 __int128 低半积校验 —— i+j >= NUM_WORDS 的积
             * (28/64 词对) 只进高位结果字, out_lo 的 8 字 memcmp 观察
             * 不到这些乘法的错误 (乘法器盲区); 健康硬件上
             * lo_row[j] == (uint64_t)prod 恒成立, 累加结果逐位不变。 */
            if (lo_row[j] != (uint64_t)prod) {
                sve_ok = false;
                if (diag) {   /* 冷路径: 健康硬件不进入 */
                    if (!diag->any) {
                        diag->any = true;
                        diag->first_i = i;
                        diag->first_j = j;
                        diag->first_actual = lo_row[j];
                        diag->first_expected = (uint64_t)prod;
                        diag->first_xor = lo_row[j] ^ (uint64_t)prod;
                    }
                    const int lane = (int)((unsigned)j % (unsigned)svcntd());
                    if (lane < 16)
                        diag->lane_hist[lane]++;
                    diag->total++;
                }
            }
            /* acc[i+j] += low(SVE 积) + carry; carry = high + 溢出。 */
            unsigned __int128 s = (unsigned __int128)acc[i + j] + lo_row[j] + carry;
            acc[i + j] = (uint64_t)s;
            carry = (uint64_t)(prod >> 64) + (uint64_t)(s >> 64);
        }
        acc[i + NUM_WORDS] += carry;
    }
    memcpy(out_lo, acc, NUM_WORDS * sizeof(uint64_t));
    return sve_ok;
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

static int gmp_bignum_sve_init(struct test *test) {
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
    /* 交叉核对: SVE 实现首跑必须与 golden 一致 (实现错在 init 就暴露)。
     * per-lane 校验一并纳入。乘积/逐项不符 = 该硬件乘法器可疑 —— 报告
     * 为检出于 init 的失败而非跳过: 持续性乘法器错误若只记 skip 会被
     * 熔断/汇总口径吞掉 (PR #294 检视 round 3); init 返回 > 0 →
     * TestResult::Failed (framework/sandstone_run.cpp)。仅能力/资源
     * 不足才走 skip。 */
    uint64_t check[NUM_WORDS];
    Mul512LaneDiag diag = {};
    const bool lanes_ok =
        sve_bigint_mul512(data->a_words.data(), data->b_words.data(), check, &diag);
    const bool prod_ok = (memcmp(check, data->golden_product.data(),
                                 NUM_WORDS * sizeof(uint64_t)) == 0);
    if (!lanes_ok || !prod_ok) {
        log_error("gmp_bignum_sve: SVE mul512 self-check FAILED in init "
                  "(per-lane %s, product %s) — multiplier suspect on this CPU",
                  lanes_ok ? "ok" : "MISMATCH", prod_ok ? "ok" : "MISMATCH");
        if (diag.any)
            log_error("gmp_bignum_sve: first mismatch at (i=%d, j=%d): "
                      "sve=0x%016llx ref=0x%016llx xor=0x%016llx (%u word-pairs)",
                      diag.first_i, diag.first_j,
                      (unsigned long long)diag.first_actual,
                      (unsigned long long)diag.first_expected,
                      (unsigned long long)diag.first_xor, diag.total);
        delete data;
        return EXIT_FAILURE;
    }
#else
    return EXIT_SKIP;
#endif

    test->data = data;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__
static int gmp_bignum_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<TestData *>(test->data);

    uint64_t product[NUM_WORDS];

    do {
        /* per-lane 通道先行 (PR #294 检视 round 2): 每个乘法在进入
         * 累加器前已对 __int128 参考逐项校验, 覆盖 memcmp 观察不到的
         * i+j >= NUM_WORDS 词对; report_fail 中止线程。冷路径把首个
         * 失配词对 + 出错 lane 直方图留下 (PR #294 检视 round 3) —
         * 高位词对出错时 product 的 8 字可能全对, 没有现场无法定位。 */
        Mul512LaneDiag diag = {};
        const bool sve_lanes_ok =
            sve_bigint_mul512(data->a_words.data(), data->b_words.data(),
                              product, &diag);
        if (!sve_lanes_ok) {
            if (diag.any) {
                log_error("gmp_bignum_sve (SVE): per-lane mismatch first at "
                          "(i=%d, j=%d): sve=0x%016llx ref=0x%016llx "
                          "xor=0x%016llx (%u word-pairs) on CPU %d",
                          diag.first_i, diag.first_j,
                          (unsigned long long)diag.first_actual,
                          (unsigned long long)diag.first_expected,
                          (unsigned long long)diag.first_xor, diag.total, cpu);
                char hist[160];
                int off = 0;
                for (int l = 0; l < 16 && off < (int)sizeof(hist) - 12; ++l)
                    if (diag.lane_hist[l])
                        off += snprintf(hist + off, sizeof(hist) - off,
                                        "lane%d=%u ", l, diag.lane_hist[l]);
                log_error("gmp_bignum_sve (SVE): failing svmul lane "
                          "histogram: %s", hist);
            }
            log_data("gmp_bignum_sve a (8 words)",
                     data->a_words.data(), NUM_WORDS * sizeof(uint64_t));
            log_data("gmp_bignum_sve b (8 words)",
                     data->b_words.data(), NUM_WORDS * sizeof(uint64_t));
            log_data("gmp_bignum_sve product (SVE svmul, 8 words)",
                     product, NUM_WORDS * sizeof(uint64_t));
            report_fail_msg("gmp_bignum_sve (SVE): svmul low-half lane "
                            "mismatch vs __int128 reference on CPU %d "
                            "(per-lane check)", cpu);
        }

        bool data_ok = (memcmp(product, data->golden_product.data(),
                               NUM_WORDS * sizeof(uint64_t)) == 0);
        if (!data_ok) {
            int mismatch_idx = -1;
            for (int i = 0; i < NUM_WORDS; ++i) {
                if (product[i] != data->golden_product[i]) { mismatch_idx = i; break; }
            }
            char msg[200];
            snprintf(msg, sizeof(msg),
                     "gmp_bignum_sve (SVE): product mismatch on CPU %d at word %d",
                     cpu, mismatch_idx);
            log_data("gmp_bignum_sve a (8 words)",
                     data->a_words.data(), NUM_WORDS * sizeof(uint64_t));
            log_data("gmp_bignum_sve b (8 words)",
                     data->b_words.data(), NUM_WORDS * sizeof(uint64_t));
            log_data("gmp_bignum_sve product (SVE svmul, 8 words)",
                     product, NUM_WORDS * sizeof(uint64_t));
            log_data("gmp_bignum_sve golden (schoolbook __int128, 8 words)",
                     data->golden_product.data(), NUM_WORDS * sizeof(uint64_t));
            report_fail_msg("%s", msg);
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int gmp_bignum_sve_run(struct test *test, int cpu) {
    (void)cpu; (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for gmp_bignum_sve");
    return EXIT_SKIP;
}
#endif

static int gmp_bignum_sve_finish(struct test *test) {
    auto *data = static_cast<TestData *>(test->data);
    if (data) delete data;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

DECLARE_TEST(gmp_bignum_sve,
             "Large-integer multiply (512-bit low half) on SVE svmul lanes with "
             "__int128 high-half fold and carry-chain accumulation (port of "
             "bigint_mulx_arm)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = gmp_bignum_sve_init,
    .test_run = gmp_bignum_sve_run,
    .test_cleanup = gmp_bignum_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
    .arm64_requires = ARM64_REQUIRES_SVE,
END_DECLARE_TEST
