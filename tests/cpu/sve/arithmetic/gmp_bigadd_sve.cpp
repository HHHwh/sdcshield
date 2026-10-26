/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b gmp_bigadd_sve
 * @parblock
 * SVE port of gmp_bigadd: large-integer addition via the SVE carry chain
 * (svadd lanes + serial carry fold, verified equivalent to __int128 on 20k
 * random chains) replacing the GMP mpz_add run path; golden stays the
 * __int128 software reference. Random operands per init via framework RNG.
 * The operand width in 64-bit limbs (originally GMP_SVE_LIMBS=256,
 * i.e. 2048-bit) is runtime configurable via -O gmp_bigadd_sve.limbs=N /
 * SDC_GMP_BIGADD_SVE_LIMBS, with a memory-adaptive default when
 * unconfigured.
 * @endparblock
 */

#include "sandstone.h"
#include "sdc_stress_config.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

static constexpr size_t GMP_SVE_LIMBS = 256;   /* 2048-bit operands */

struct GmpBigaddSveData {
    size_t limbs;                   /* 运行时 64-bit limb 数 */
    std::vector<uint64_t> a;
    std::vector<uint64_t> b;
    std::vector<uint64_t> golden;
};

static void compute_golden(const GmpBigaddSveData *d, uint64_t *out, size_t n)
{
    uint64_t carry = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned __int128 s = (unsigned __int128)d->a[i] + d->b[i] + carry;
        out[i] = (uint64_t)s;
        carry = (uint64_t)(s >> 64);
    }
}

/* 操作数宽度 GMP_SVE_LIMBS(256, 即 2048-bit) — 运行时可配置, 统一优先级链:
 *   1. -O gmp_bigadd_sve.limbs=N / 配置文件同名条目
 *   2. 环境变量 SDC_GMP_BIGADD_SVE_LIMBS
 *   3. 自适应: a/b/golden 共享 3 份 + 每线程 result 1 份, 4×8B,
 *      rss_ratio 1.0, 占物理内存 60%, clamp [256, 1M]。
 *      原 256 为下限。 */
static size_t gmp_bigadd_sve_limbs(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "limbs", 0, GMP_SVE_LIMBS,
                                     1 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("gmp_bigadd_sve", "limbs", &v,
                                 GMP_SVE_LIMBS, 1 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count(8, 4, 1.0, 600, thread_count(),
                                         GMP_SVE_LIMBS, 1 << 20, test->id, "limbs");
}

static int gmp_bigadd_sve_init(struct test *test)
{
#if defined(__aarch64__)
    /* All-VL Phase 2 P6: per-test auxv hand-probe -> framework
     * layer: arm64_require_sve_vl does the SVE capability check
     * + 128..1024-bit range gate in one call (skip reason
     * framework-issued). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
#endif
    auto *d = new GmpBigaddSveData;
    const size_t LIMBS = gmp_bigadd_sve_limbs(test);
    d->limbs = LIMBS;
    d->a.resize(LIMBS);
    d->b.resize(LIMBS);
    d->golden.resize(LIMBS);
    memset_random(d->a.data(), LIMBS * sizeof(uint64_t));
    memset_random(d->b.data(), LIMBS * sizeof(uint64_t));
    compute_golden(d, d->golden.data(), LIMBS);
    test->data = d;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)
static int gmp_bigadd_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<GmpBigaddSveData *>(test->data);
    const size_t LIMBS = d->limbs;
    std::vector<uint64_t> result(LIMBS);
    const int lanes = svcntd();

    TEST_LOOP(test, 1 << 13) {
        /* SVE 进位链 (GMP mpz_add 的 SVE 替代): svadd 批 + 串行进位折叠 */
        uint64_t carry = 0;
        for (size_t base = 0; base < LIMBS; base += lanes) {
            int n = (LIMBS - base < (size_t)lanes) ? (int)(LIMBS - base) : lanes;
            svbool_t pg = svwhilelt_b64((uint64_t)0, (uint64_t)n);
            svuint64_t va = svld1_u64(pg, d->a.data() + base);
            svuint64_t vb = svld1_u64(pg, d->b.data() + base);
            uint64_t vsum[8];
            svst1_u64(pg, vsum, svadd_u64_x(pg, va, vb));
            for (int i = 0; i < n; ++i) {
                unsigned __int128 s = (unsigned __int128)vsum[i] + carry;
                result[base + i] = (uint64_t)s;
                uint64_t cin_ab = (vsum[i] < d->a[base + i]) ? 1u : 0u;
                carry = cin_ab + (uint64_t)(s >> 64);
            }
        }
        if (memcmp(result.data(), d->golden.data(), LIMBS * sizeof(uint64_t)) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < LIMBS; ++_dump_j) {
                    if (result[_dump_j] != d->golden[_dump_j]) {
                        ++_dump_bad;
                        if (_dump_shown < 8) {
                            log_error("gmp_bigadd_sve: mismatch at limb %zu: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                                      _dump_j, (unsigned long long)d->golden[_dump_j],
                                      (unsigned long long)result[_dump_j],
                                      (unsigned long long)(d->golden[_dump_j] ^ result[_dump_j]));
                            ++_dump_shown;
                        }
                    }
                }
                log_error("gmp_bigadd_sve: total mismatched elements: %zu / %d",
                          _dump_bad, (int)LIMBS);
            }
            report_fail_msg("gmp_bigadd_sve: big-integer add miscompare (SVE carry chain)");
        }
    }
    return EXIT_SUCCESS;
}
#else
static int gmp_bigadd_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    return EXIT_SKIP;
}
#endif

static int gmp_bigadd_sve_finish(struct test *test)
{
    delete static_cast<GmpBigaddSveData *>(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(gmp_bigadd_sve, "Large-integer addition on the SVE carry chain (2048-bit, port of gmp_bigadd)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = gmp_bigadd_sve_init,
    .test_run = gmp_bigadd_sve_run,
    .test_cleanup = gmp_bigadd_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
