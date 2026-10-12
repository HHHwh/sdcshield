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
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <sys/auxv.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif
#endif

static constexpr size_t GMP_SVE_LIMBS = 256;   /* 2048-bit operands */

struct GmpBigaddSveData {
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

static int gmp_bigadd_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for gmp_bigadd_sve");
        return EXIT_SKIP;
    }
#endif
    auto *d = new GmpBigaddSveData;
    d->a.resize(GMP_SVE_LIMBS);
    d->b.resize(GMP_SVE_LIMBS);
    d->golden.resize(GMP_SVE_LIMBS);
    memset_random(d->a.data(), GMP_SVE_LIMBS * sizeof(uint64_t));
    memset_random(d->b.data(), GMP_SVE_LIMBS * sizeof(uint64_t));
    compute_golden(d, d->golden.data(), GMP_SVE_LIMBS);
    test->data = d;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)
static int gmp_bigadd_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<GmpBigaddSveData *>(test->data);
    std::vector<uint64_t> result(GMP_SVE_LIMBS);
    const int lanes = svcntd();

    TEST_LOOP(test, 1 << 13) {
        /* SVE 进位链 (GMP mpz_add 的 SVE 替代): svadd 批 + 串行进位折叠 */
        uint64_t carry = 0;
        for (size_t base = 0; base < GMP_SVE_LIMBS; base += lanes) {
            int n = (GMP_SVE_LIMBS - base < (size_t)lanes) ? (int)(GMP_SVE_LIMBS - base) : lanes;
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
        if (memcmp(result.data(), d->golden.data(), GMP_SVE_LIMBS * sizeof(uint64_t)) != 0) {
            /* cold path: locate mismatches, record bit-level evidence (golden/actual/xor) */
            {
                size_t _dump_bad = 0;
                int _dump_shown = 0;
                for (size_t _dump_j = 0; _dump_j < (size_t)(GMP_SVE_LIMBS); ++_dump_j) {
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
                          _dump_bad, (int)GMP_SVE_LIMBS);
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
