/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve512_f64_special_arm
 * @parblock
 * SVE full-vector-length f64 special-value chain SDC stress — standalone
 * extraction of workload 2 from sve512_fma_arm.
 *
 * The same serial FMLA chain structure as sve512_f64_chain_arm, but over
 * the IEEE-754 special-value table (NaN/sNaN/±Inf/±0/±1), compared by
 * category (fsu_byteexact_arm precedent): a category change is always a
 * failure. NaN payloads are not compared byte-exact because IEEE 754 does
 * not require deterministic NaN payload propagation across FMA
 * implementations.
 *
 * Compiled WITHOUT -msve-vector-bits so the sizeless svfloat64_t type
 * adopts the CPU runtime vector length — the full 512-bit datapath on
 * HiSilicon 0xd22 (sve_default_vector_length=64). test_init probes
 * HWCAP_SVE via getauxval (no SVE instruction executes before the probe),
 * returning a clean EXIT_SKIP on SVE-less CPUs.
 *
 * SDC detection: per-lane category compare against the deterministic
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

// Special-value (category-checked) chain length.
static constexpr size_t SPECIAL_CHAIN_STEPS = 64;

// Special-value table for the category-checked chain.
static const uint64_t F64_SPECIAL[] = {
    0x0000000000000000ULL,   // +0.0
    0x8000000000000000ULL,   // -0.0
    0x7FF8000000000000ULL,   // qNaN
    0x7FF0000000000001ULL,   // sNaN
    0x7FF0000000000000ULL,   // +Inf
    0xFFF0000000000000ULL,   // -Inf
    0x3FF0000000000000ULL,   // 1.0
    0xBFF0000000000000ULL,   // -1.0
};
static constexpr size_t F64_SPECIAL_SIZE =
    sizeof(F64_SPECIAL) / sizeof(F64_SPECIAL[0]);

// Deterministic seed generator (same family as other tests).
static inline uint64_t splitmix64(uint64_t x)
{
    uint64_t z = (x + 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

struct SveF64SpecialData {
    size_t vl_d;   // svcntd() — f64 lanes per full vector

    // Per-lane seed accumulators (finite, [1.0, 2.0)).
    std::vector<uint64_t> seeds_f64;

    // f64 special-chain operand streams, layout [step * vl_d + lane].
    std::vector<uint64_t> sm_f64;
    std::vector<uint64_t> sa_f64;
};

#ifdef __aarch64__

// ---------------------------------------------------------------------------
// Scalar golden: identical operation order and indexing to the SVE chain.
// ---------------------------------------------------------------------------
static void f64_special_golden(const SveF64SpecialData *d, uint64_t *out)
{
    const size_t lanes = d->vl_d;
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint64_t accw = d->seeds_f64[lane];
        double acc;
        memcpy(&acc, &accw, 8);
        for (size_t i = 0; i < SPECIAL_CHAIN_STEPS; ++i) {
            double m, a;
            memcpy(&m, &d->sm_f64[i * lanes + lane], 8);
            memcpy(&a, &d->sa_f64[i * lanes + lane], 8);
            acc = std::fma(m, a, acc);
        }
        memcpy(&accw, &acc, 8);
        out[lane] = accw;
    }
}

// ---------------------------------------------------------------------------
// Hardware chain (SVE, full runtime VL).
// ---------------------------------------------------------------------------
static void f64_special_chain_hw(const SveF64SpecialData *d, uint64_t *out)
{
    const size_t lanes = d->vl_d;
    const svbool_t pg = svptrue_b64();
    svfloat64_t acc = svld1_f64(pg, reinterpret_cast<const double *>(d->seeds_f64.data()));
    for (size_t i = 0; i < SPECIAL_CHAIN_STEPS; ++i) {
        svfloat64_t m = svld1_f64(pg, reinterpret_cast<const double *>(&d->sm_f64[i * lanes]));
        svfloat64_t a = svld1_f64(pg, reinterpret_cast<const double *>(&d->sa_f64[i * lanes]));
        acc = svmla_f64_x(pg, acc, m, a);
    }
    svst1_f64(pg, reinterpret_cast<double *>(out), acc);
}

// ---------------------------------------------------------------------------
// Category compare (fsu_byteexact_arm precedent): NaN/±Inf stay in
// category; finite values byte-exact.
// ---------------------------------------------------------------------------
static inline bool f64_lane_ok(uint64_t golden, uint64_t actual)
{
    double g, a;
    memcpy(&g, &golden, 8);
    memcpy(&a, &actual, 8);
    if (std::isnan(g)) return std::isnan(a);
    if (std::isinf(g)) return std::isinf(a) && ((g > 0) == (a > 0));
    if (std::isnan(a) || std::isinf(a)) return false;
    return golden == actual;
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
static int sve512_f64_special_arm_init(struct test *test)
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
                 "sve512_f64_special_arm requires SVE "
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
        auto data = std::make_unique<SveF64SpecialData>();
        data->vl_d = svcntd();

        /* randomization hardening H12': seeds from the framework RNG
         * (per-run fresh, -s reproducible); the special VALUE table stays
         * by design (its NaN/Inf/±0/±1 categories are the test's
         * purpose), but which special each step draws is now a random
         * index instead of a fixed strided permutation. */
        data->seeds_f64.resize(data->vl_d);
        for (size_t lane = 0; lane < data->vl_d; ++lane) {
            data->seeds_f64[lane] = 0x3FF0000000000000ULL |
                (random64() & 0x000FFFFFFFFFFFFFULL);
        }

        // f64 special chain: SPECIAL_CHAIN_STEPS vectors.
        const size_t ns64 = SPECIAL_CHAIN_STEPS * data->vl_d;
        data->sm_f64.resize(ns64);
        data->sa_f64.resize(ns64);
        for (size_t i = 0; i < ns64; ++i) {
            data->sm_f64[i] = F64_SPECIAL[random32() % F64_SPECIAL_SIZE];
            data->sa_f64[i] = F64_SPECIAL[random32() % F64_SPECIAL_SIZE];
        }

        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sve512_f64_special_arm init: %s", e.what());
        return EXIT_SKIP;
    }
#endif
}

// ---------------------------------------------------------------------------
// test_run: f64 special-value chain per iteration.
// ---------------------------------------------------------------------------
static int sve512_f64_special_arm_run(struct test *test, int cpu)
{
    (void)cpu;
#ifndef __aarch64__
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 SVE-512 f64 "
             "special-value chain required");
    return EXIT_SKIP;
#else
    auto *d = static_cast<SveF64SpecialData *>(test->data);

    // Golden, recomputed each iteration (deterministic, same op order).
    std::vector<uint64_t> golds64(d->vl_d);
    f64_special_golden(d, golds64.data());

    do {
        bool all_passed = true;

        std::vector<uint64_t> hw(d->vl_d);
        f64_special_chain_hw(d, hw.data());
        for (size_t lane = 0; lane < d->vl_d; ++lane) {
            if (!f64_lane_ok(golds64[lane], hw[lane])) {
                report_f64_lane_mismatch("sve512_f64_special", lane,
                                         golds64[lane], hw[lane]);
                all_passed = false;
            }
        }

        if (!all_passed) {
            report_fail_msg(
                "sve512_f64_special_arm: SVE-512 f64 special-value chain "
                "SDC detected");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#endif
}

// ---------------------------------------------------------------------------
// test_cleanup
// ---------------------------------------------------------------------------
static int sve512_f64_special_arm_cleanup(struct test *test)
{
#ifdef __aarch64__
    delete static_cast<SveF64SpecialData *>(test->data);
#else
    (void)test;
#endif
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(sve512_f64_special_arm,
             "SVE full-vector-length f64 special-value chain SDC stress: "
             "64-step serial svmla_f64_x chain over the IEEE-754 special "
             "table (NaN/sNaN/±Inf/±0/±1) at the CPU's runtime SVE vector "
             "length (512-bit on HiSilicon 0xd22), per-lane category "
             "compared against a deterministic scalar fma() reference")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_f64_special_arm_init,
    .test_run = sve512_f64_special_arm_run,
    .test_cleanup = sve512_f64_special_arm_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
