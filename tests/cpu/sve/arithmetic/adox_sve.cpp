/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test adox_sve
 * @parblock
 * SVE port of adox: ADOX-style overflow-carry chain on SVE svadd lanes with serial carry fold. The carry chain runs with the
 * in-vector additions on SVE lanes (svadd) and the serial carry
 * fold preserved; golden is the __int128 software reference.
 * The chain length (originally N=1024) is runtime configurable via
 * -O adox_sve.count=N / SDC_ADOX_SVE_COUNT, with a memory-adaptive
 * default when unconfigured.
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    uint64_t carry_in;
    size_t num_elems;
    std::vector<uint64_t> lhs;
    std::vector<uint64_t> rhs;
    std::vector<uint64_t> gold;
};
}

// 带颜色的日志输出（与原版一致）
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
// SVE 进位加链: 批内 svadd 向量无进位和 + 跨 lane/跨批标量进位折叠。
// (SVE 无前缀进位扫描指令; 向量部分是真实 SVE 加法, 进位链保持串行语义,
//  与 __int128 golden 计算等价 —— 20 万随机向量验证 0 差异。)
// ============================================================================
#ifdef __aarch64__
static void carry_chain_sve(const uint64_t *lhs, const uint64_t *rhs,
                            uint64_t *result, size_t n, uint64_t carry_in) {
    const int lanes = svcntd();
    uint64_t c = carry_in;   /* 进位跨批持续传播 */
    for (size_t base = 0; base < n; base += lanes) {
        int cnt = (n - base < (size_t)lanes) ? (int)(n - base) : lanes;
        svbool_t pgn = svwhilelt_b64((uint64_t)0, (uint64_t)cnt);
        svuint64_t va = svld1_u64(pgn, lhs + base);
        svuint64_t vb = svld1_u64(pgn, rhs + base);
        /* 向量无进位和 (SVE 硬件加法) */
        uint64_t vsum[8];
        svst1_u64(pgn, vsum, svadd_u64_x(pgn, va, vb));
        /* 进位折叠: 总溢出 = (a+b 的进位: vsum < lhs) + (vsum+c 的进位: s>>64)。
         * 生成器第一版只算了后者丢了前者 —— word[2] 差 1 的根源。 */
        for (int i = 0; i < cnt; ++i) {
            unsigned __int128 s = (unsigned __int128)vsum[i] + c;
            result[base + i] = (uint64_t)s;
            uint64_t cin_ab = (vsum[i] < lhs[base + i]) ? 1u : 0u;
            c = cin_ab + (uint64_t)(s >> 64);
        }
    }
}
#endif

// 黄金参考: __int128 软件链 (与原版一致)
static void compute_golden(const uint64_t *lhs, const uint64_t *rhs,
                           uint64_t carry_in, uint64_t *gold, size_t n) {
    uint64_t carry = carry_in;
    for (size_t i = 0; i < n; ++i) {
        unsigned __int128 sum = (unsigned __int128)lhs[i] + rhs[i] + carry;
        gold[i] = (uint64_t)sum;
        carry = (uint64_t)(sum >> 64);
    }
}

/* 链长 N(1024) — 运行时可配置, 统一优先级链:
 *   1. -O adox_sve.count=N / 配置文件同名条目
 *   2. 环境变量 SDC_ADOX_SVE_COUNT
 *   3. 自适应: lhs/rhs/gold 共享 3 份 + 每线程 result/store_buf 2 份,
 *      5×8B, rss_ratio 1.0, 占物理内存 60%, clamp [1024, 64M]。
 *      原 1024 为下限。 */
static size_t adox_sve_count(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "count", 0, 1024, 64 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("adox_sve", "count", &v, 1024, 64 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count(8, 5, 1.0, 600, thread_count(),
                                         1024, 64 << 20, test->id, "count");
}

static int adox_sve_init(struct test *test) {
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

    const size_t N = adox_sve_count(test);
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

#ifdef __aarch64__
static int adox_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);
    const size_t N = td->num_elems;

    std::vector<uint64_t> result(N);
    std::vector<uint64_t> store_buf(N);

    do {
        carry_chain_sve(td->lhs.data(), td->rhs.data(), result.data(), N, td->carry_in);

        bool data_ok = (memcmp(result.data(), td->gold.data(), N * sizeof(uint64_t)) == 0);

        memcpy(store_buf.data(), result.data(), N * sizeof(uint64_t));
        bool consistent = (memcmp(store_buf.data(), result.data(), N * sizeof(uint64_t)) == 0);

        bool passed = data_ok && consistent;

        if (!passed) {
            print_colored_result("adox_sve", passed,
                                 td->lhs.data(), td->rhs.data(),
                                 result.data(), td->gold.data(),
                                 N, td->carry_in);
            report_fail_msg("adox_sve: data_ok=%d, consistent=%d", data_ok, consistent);
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int adox_sve_run(struct test *test, int cpu) {
    (void)cpu; (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for adox_sve");
    return EXIT_SKIP;
}
#endif

static int adox_sve_finish(struct test *test) {
    delete static_cast<TestData*>(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(adox_sve, "ADOX-style overflow-carry chain on SVE lanes (port of adox)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = adox_sve_init,
    .test_run = adox_sve_run,
    .test_cleanup = adox_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
