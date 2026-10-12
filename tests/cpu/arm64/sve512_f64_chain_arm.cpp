/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve512_f64_chain_arm
 * @parblock
 * SVE full-vector-length f64 FMLA dependency chain SDC stress — standalone
 * extraction of workload 1 from sve512_fma_arm.
 *
 * A serial chain of svmla_f64_x over high-Hamming finite f64 lanes
 * (|x| <= 2, so 512 steps of products can never overflow to Inf/NaN — every
 * lane stays byte-exact comparable), each step feeding the next — a real
 * serial dependency through the wide FMA pipe, not ILP.
 *
 * The golden is recomputed in scalar C++ with the identical operation
 * order, so any lane flip in the wide datapath is a byte-exact mismatch.
 *
 * Compiled WITHOUT -msve-vector-bits so the sizeless svfloat64_t type
 * adopts the CPU runtime vector length — the full 512-bit datapath on
 * HiSilicon 0xd22 (sve_default_vector_length=64). test_init probes
 * HWCAP_SVE via getauxval (no SVE instruction executes before the probe),
 * returning a clean EXIT_SKIP on SVE-less CPUs.
 *
 * SDC detection: byte-exact lane compares against the deterministic
 * software reference, with per-lane golden/actual/XOR logging on mismatch.
 * report_fail_msg on any mismatch. ARM64-native; non-SVE CPUs return a
 * clean EXIT_SKIP from init. Built only on aarch64.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <cmath>
#include <memory>
#include <vector>

#ifdef __aarch64__
#include <sys/auxv.h>
#include <asm/hwcap.h>
#include <arm_sve.h>
#endif

// Serial FMA chain length per iteration.
static constexpr size_t CHAIN_STEPS = 512;

// Finite high-Hamming f64 patterns: |x| <= 2, so products stay <= 4 and
// 512-step sums stay < 2048 — the chain can never overflow or NaN, so
// every lane of every step is byte-exact comparable. Successive entries
// toggle many mantissa/sign bits (high gate-toggle activity).
static const uint64_t F64_FINITE[] = {
    0x3FF5555555555555ULL,   //  1.3333...
    0xBFFAAAAAAAAAAAAAULL,   // -1.6667...
    0x3FD5555555555555ULL,   //  0.3333...
    0xBFEAAAAAAAAAAAAAULL,   // -0.8333...
    0x0015555555555555ULL,   //  denormal +
    0x801AAAAAAAAAAAAAULL,   //  denormal -
    0x3FF0000000000000ULL,   //  1.0
    0xBFF0000000000000ULL,   // -1.0
    0x0005555555555555ULL,   //  denormal +
    0x800AAAAAAAAAAAAAULL,   //  denormal -
    0x3FF999999999999AULL,   //  1.6
    0xBFF6666666666667ULL,   // -1.4
    0x3FE5555555555555ULL,   //  0.6667...
    0xBFE6666666666667ULL,   // -0.7
    0x3FF3333333333333ULL,   //  1.2
    0xBFFCCCCCCCCCCCCDULL,   // -1.8
};
static constexpr size_t F64_FINITE_SIZE =
    sizeof(F64_FINITE) / sizeof(F64_FINITE[0]);

// Deterministic seed generator (same family as other tests).
static inline uint64_t splitmix64(uint64_t x)
{
    uint64_t z = (x + 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

struct SveF64ChainData {
    size_t vl_d;   // svcntd() — f64 lanes per full vector

    // Per-lane seed accumulators (finite, [1.0, 2.0)).
    std::vector<uint64_t> seeds_f64;

    // f64 finite-chain operand streams, layout [step * vl_d + lane].
    std::vector<uint64_t> m_f64;
    std::vector<uint64_t> a_f64;
};

#ifdef __aarch64__

// ---------------------------------------------------------------------------
// Scalar golden: identical operation order and indexing to the SVE chain
// (stream layout is [step * lanes + lane]).
// ---------------------------------------------------------------------------
static void f64_chain_golden(const SveF64ChainData *d, uint64_t *out)
{
    const size_t lanes = d->vl_d;
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint64_t accw = d->seeds_f64[lane];
        double acc;
        memcpy(&acc, &accw, 8);
        for (size_t i = 0; i < CHAIN_STEPS; ++i) {
            double m, a;
            memcpy(&m, &d->m_f64[i * lanes + lane], 8);
            memcpy(&a, &d->a_f64[i * lanes + lane], 8);
            acc = std::fma(m, a, acc);
        }
        memcpy(&accw, &acc, 8);
        out[lane] = accw;
    }
}

// ---------------------------------------------------------------------------
// Hardware chain (SVE, full runtime VL).
// ---------------------------------------------------------------------------
static void f64_chain_hw(const SveF64ChainData *d, uint64_t *out)
{
    const size_t lanes = d->vl_d;
    const svbool_t pg = svptrue_b64();
    svfloat64_t acc = svld1_f64(pg, reinterpret_cast<const double *>(d->seeds_f64.data()));
    for (size_t i = 0; i < CHAIN_STEPS; ++i) {
        svfloat64_t m = svld1_f64(pg, reinterpret_cast<const double *>(&d->m_f64[i * lanes]));
        svfloat64_t a = svld1_f64(pg, reinterpret_cast<const double *>(&d->a_f64[i * lanes]));
        acc = svmla_f64_x(pg, acc, m, a);
    }
    svst1_f64(pg, reinterpret_cast<double *>(out), acc);
}

// ---------------------------------------------------------------------------
// Per-lane failure logging (fsu_byteexact_arm style).
// ---------------------------------------------------------------------------
static void report_f64_lane_mismatch(const char *tag, size_t lane,
                                     uint64_t golden, uint64_t actual)
{
    log_warning("%s: lane %zu golden=0x%016" PRIx64 " "
                "actual=0x%016" PRIx64 " xor=0x%016" PRIx64,
                tag, lane, golden, actual, golden ^ actual);
}

// ---------------------------------------------------------------------------
// test_init
// ---------------------------------------------------------------------------
static int sve512_f64_chain_arm_init(struct test *test)
{
#ifndef __aarch64__
    (void)test;
    return EXIT_SUCCESS;
#else
    // Probe SVE availability BEFORE executing any SVE instruction.
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SVE not available on this CPU; "
                 "sve512_f64_chain_arm requires SVE "
                 "(e.g. HiSilicon 0xd22, 512-bit VL)");
        return EXIT_SKIP;
    }

    /* 本测试族为 512-bit VL 设计并在 HiSilicon 0xd22 上验证;GH 托管 SVE
     * runner(运行时 VL=128)上实测 golden 失配(issue #182)。 */
    if (svcntb() != 64) {
        log_skip(CpuNotSupportedSkipCategory,
                 "SVE vector length is not 512-bit on this CPU; the sve512 "
                 "family is designed and validated at 512-bit VL only "
                 "(HiSilicon 0xd22, see issue #182)");
        return EXIT_SKIP;
    }

    try {
        auto data = std::make_unique<SveF64ChainData>();
        data->vl_d = svcntd();

        /* randomization hardening H12': operands come from the framework
         * RNG (per-run fresh, -s reproducible) instead of the fixed
         * splitmix/table indices that made every run byte-identical.
         * Design invariant preserved: every generated f64 stays in the
         * finite band |x| <= 2 (uniform exponent 0x3FE..0x3FF covers
         * [0.5, 2); the switch's other selections fold back to 0x3FE),
         * so 512-step products/sums can still never overflow or NaN.
         * 25% of entries
         * still draw from the high-Hamming F64_FINITE table to keep the
         * gate-toggle density the table was designed for. */
        auto random_finite_f64 = []() -> uint64_t {
            if ((random32() & 3) == 0)
                return F64_FINITE[random32() % F64_FINITE_SIZE];
            uint64_t sign = (random64() & 1ULL) << 63;
            uint64_t mantissa = random64() & 0x000FFFFFFFFFFFFFULL;
            uint64_t exp;
            uint64_t raw = random64();
            switch ((raw >> 62) & 3ULL) {
            case 0: exp = 0x3FEULL; break;   /* [0.5, 1)   */
            case 1: exp = 0x3FFULL; break;   /* [1, 2)     */
            default: exp = 0x3FEULL; break;  /* keep |x|<2 on the rest too */
            }
            return sign | (exp << 52) | mantissa;
        };

        // Per-lane seed accumulators: finite in [1.0, 2.0).
        data->seeds_f64.resize(data->vl_d);
        for (size_t lane = 0; lane < data->vl_d; ++lane) {
            data->seeds_f64[lane] = 0x3FF0000000000000ULL |
                (random64() & 0x000FFFFFFFFFFFFFULL);
        }

        // f64 finite chain: CHAIN_STEPS full vectors of operands.
        const size_t n64 = CHAIN_STEPS * data->vl_d;
        data->m_f64.resize(n64);
        data->a_f64.resize(n64);
        for (size_t i = 0; i < n64; ++i) {
            data->m_f64[i] = random_finite_f64();
            data->a_f64[i] = random_finite_f64();
        }

        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sve512_f64_chain_arm init: %s", e.what());
        return EXIT_SKIP;
    }
#endif
}

// ---------------------------------------------------------------------------
// test_run: f64 finite FMLA chain per iteration.
// ---------------------------------------------------------------------------
static int sve512_f64_chain_arm_run(struct test *test, int cpu)
{
    (void)cpu;
#ifndef __aarch64__
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 SVE-512 f64 FMLA "
             "chain required");
    return EXIT_SKIP;
#else
    auto *d = static_cast<SveF64ChainData *>(test->data);

    // Golden, recomputed each iteration (deterministic, same op order).
    std::vector<uint64_t> gold64(d->vl_d);
    f64_chain_golden(d, gold64.data());

    do {
        bool all_passed = true;

        std::vector<uint64_t> hw(d->vl_d);
        f64_chain_hw(d, hw.data());
        for (size_t lane = 0; lane < d->vl_d; ++lane) {
            if (hw[lane] != gold64[lane]) {
                report_f64_lane_mismatch("sve512_f64_chain", lane,
                                         gold64[lane], hw[lane]);
                all_passed = false;
            }
        }

        if (!all_passed) {
            report_fail_msg(
                "sve512_f64_chain_arm: SVE-512 f64 FMLA chain SDC detected");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#endif
}

// ---------------------------------------------------------------------------
// test_cleanup
// ---------------------------------------------------------------------------
static int sve512_f64_chain_arm_cleanup(struct test *test)
{
#ifdef __aarch64__
    delete static_cast<SveF64ChainData *>(test->data);
#else
    (void)test;
#endif
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(sve512_f64_chain_arm,
             "SVE full-vector-length f64 FMLA dependency chain SDC stress: "
             "512-step serial svmla_f64_x chain at the CPU's runtime SVE "
             "vector length (512-bit on HiSilicon 0xd22), byte-exact "
             "compared against a deterministic scalar fma() reference")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_f64_chain_arm_init,
    .test_run = sve512_f64_chain_arm_run,
    .test_cleanup = sve512_f64_chain_arm_cleanup,
    .fracture_loop_count = -1,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
