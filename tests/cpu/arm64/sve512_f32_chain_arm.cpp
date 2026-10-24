/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve512_f32_chain_arm
 * @parblock
 * SVE full-vector-length f32 FMLA dependency chain SDC stress — standalone
 * extraction of workload 3 from sve512_fma_arm.
 *
 * The same serial FMLA chain structure as sve512_f64_chain_arm but at f32
 * width (svmla_f32_x), exercising the f32 FMA/predicate datapath at the
 * CPU's full runtime vector width (4..32 f32 lanes).
 *
 * The golden is recomputed in scalar C++ with the identical operation
 * order, so any lane flip in the wide f32 datapath is a byte-exact
 * mismatch.
 *
 * Compiled WITHOUT -msve-vector-bits so the sizeless svfloat32_t type
 * adopts the CPU runtime vector length — all-VL (128..1024-bit, all-VL
 * Phase 2 P3; the pre-P3 512-bit single-value gate and its issue #182
 * caveat are cleared by the four-tier dual-kernel evidence in gem5-fi
 * feat/sve512-dual). test_init gates via the framework layer
 * (arm64_require_sve_vl: HWCAP_SVE capability + range check in one
 * call, no SVE instruction executes before the probe), returning a
 * clean EXIT_SKIP on SVE-less CPUs; test_run re-checks the VL and
 * fails closed on drift.
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
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

// Serial FMA chain length per iteration — VL-independent (a per-lane
// dependency sequence; the lane count, not the length, scales with VL).
static constexpr size_t CHAIN_STEPS = 512;

// f32 finite patterns (|x| <= 2).
static const uint32_t F32_FINITE[] = {
    0x3FAAAAABU, 0xBFAAAAABU, 0x3EAAAAABU, 0xBF2AAAABU,
    0x3F800000U, 0xBF800000U, 0x00555555U, 0x80AAAAAAU,
    0x3FCCCCCDU, 0xBFCCCCCDU, 0x3F333333U, 0xBF333333U,
    0x3E555555U, 0xBE555555U, 0x3F666666U, 0xBF666666U,
};
static constexpr size_t F32_FINITE_SIZE =
    sizeof(F32_FINITE) / sizeof(F32_FINITE[0]);

// Deterministic seed generator (same family as other tests).
static inline uint64_t splitmix64(uint64_t x)
{
    uint64_t z = (x + 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

struct SveF32ChainData {
    size_t vl_w;   // svcntw() — f32 lanes per full vector

    // Per-lane seed accumulators (finite, [1.0, 2.0)).
    std::vector<uint32_t> seeds_f32;

    // f32 finite-chain operand streams, layout [step * vl_w + lane].
    std::vector<uint32_t> m_f32;
    std::vector<uint32_t> a_f32;
};

#ifdef __aarch64__

// ---------------------------------------------------------------------------
// Scalar golden: identical operation order and indexing to the SVE chain
// (stream layout is [step * lanes + lane]).
// ---------------------------------------------------------------------------
static void f32_chain_golden(const SveF32ChainData *d, uint32_t *out)
{
    const size_t lanes = d->vl_w;
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t accw = d->seeds_f32[lane];
        float acc;
        memcpy(&acc, &accw, 4);
        for (size_t i = 0; i < CHAIN_STEPS; ++i) {
            float m, a;
            memcpy(&m, &d->m_f32[i * lanes + lane], 4);
            memcpy(&a, &d->a_f32[i * lanes + lane], 4);
            acc = std::fma(m, a, acc);
        }
        memcpy(&accw, &acc, 4);
        out[lane] = accw;
    }
}

// ---------------------------------------------------------------------------
// Hardware chain (SVE, full runtime VL).
// ---------------------------------------------------------------------------
static void f32_chain_hw(const SveF32ChainData *d, uint32_t *out)
{
    const size_t lanes = d->vl_w;
    const svbool_t pg = svptrue_b32();
    svfloat32_t acc = svld1_f32(pg, reinterpret_cast<const float *>(d->seeds_f32.data()));
    for (size_t i = 0; i < CHAIN_STEPS; ++i) {
        svfloat32_t m = svld1_f32(pg, reinterpret_cast<const float *>(&d->m_f32[i * lanes]));
        svfloat32_t a = svld1_f32(pg, reinterpret_cast<const float *>(&d->a_f32[i * lanes]));
        acc = svmla_f32_x(pg, acc, m, a);
    }
    svst1_f32(pg, reinterpret_cast<float *>(out), acc);
}

// ---------------------------------------------------------------------------
// Per-lane failure logging (fsu_byteexact_arm style).
// ---------------------------------------------------------------------------
static void report_f32_lane_mismatch(const char *tag, size_t lane,
                                     uint32_t golden, uint32_t actual)
{
    log_warning("%s: lane %zu golden=0x%08" PRIx32 " "
                "actual=0x%08" PRIx32 " xor=0x%08" PRIx32,
                tag, lane, golden, actual, golden ^ actual);
}

// ---------------------------------------------------------------------------
// test_init
// ---------------------------------------------------------------------------
static int sve512_f32_chain_arm_init(struct test *test)
{
#ifndef __aarch64__
    (void)test;
    return EXIT_SUCCESS;
#else
    /* All-VL Phase 2 P3: capability + 128..1024-bit range check in one
       framework-layer call (the geometry has been svcnt-dynamic all
       along; only the old single-value gate pinned one tier). The
       historical VL=128 golden mismatch on a hosted CI runner
       (issue #182) is cleared by the four-tier dual-kernel evidence
       (gem5-fi feat/sve512-dual, 48 runs). arm64_require_sve_vl
       returns the VL in BYTES — f32 lanes = svl/4, range-checked
       below (release-effective, NDEBUG-proof). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    try {
        auto data = std::make_unique<SveF32ChainData>();
        data->vl_w = (size_t)svl / 4;   /* f32 lanes = SVL bytes / 4 */
        if (data->vl_w < 4 || data->vl_w > 32) {
            log_error("sve512_f32_chain_arm: f32 lanes %zu outside "
                      "[4,32] — unit drift (svl %ld bytes)",
                      data->vl_w, svl);
            return EXIT_FAILURE;
        }

        /* randomization hardening H12': framework RNG per run (-s
         * reproducible) instead of fixed splitmix/table indices; the
         * |x| <= 2 finite-band invariant is preserved (f32 bias-127
         * exponents 0x7E/0x7F, B1-007), and 25% of entries still draw
         * from the high-Hamming F32_FINITE table for gate-toggle density. */
        auto random_finite_f32 = []() -> uint32_t {
            if ((random32() & 3) == 0)
                return F32_FINITE[random32() % F32_FINITE_SIZE];
            uint32_t sign = (random32() & 1U) << 31;
            uint32_t mantissa = random32() & 0x007FFFFFU;
            /* 0x7E = [0.5, 1); 0x7F = [1, 2) — f32 bias-127 exponents;
             * the f64-biased 0x3FE/0x3FF built qNaN/Inf patterns (B1-007). */
            uint32_t exp = 0x7EU + (random32() & 1U);
            return sign | (exp << 23) | mantissa;
        };

        /* Value-domain knob (SEVI ASPLOS'26 Obs.19: bounded vs unbounded
         * inputs differ up to 245x in SDC frequency on the same core).
         * bounded=1 (default): the |x| <= 2 finite band above — the
         * randomization-hardened path, untouched.
         * bounded=0: full-entropy mantissas with the exponent clamped to
         * [2^-8, 1) so the chain still cannot overflow to Inf/NaN and
         * the byte-exact golden stays valid. */
        int64_t bounded = get_testspecific_knob_value_int(test, "bounded", 1);
        auto random_unbounded_f32 = []() -> uint32_t {
            uint32_t sign = (random32() & 1U) << 31;
            uint32_t mantissa = random32() & 0x007FFFFFU;
            uint32_t exp = 0x77U + ((random32() >> 29) & 7U);  /* [2^-8, 1) */
            return sign | (exp << 23) | mantissa;
        };

        // Per-lane seed accumulators: finite in [1.0, 2.0).
        data->seeds_f32.resize(data->vl_w);
        for (size_t lane = 0; lane < data->vl_w; ++lane) {
            data->seeds_f32[lane] = 0x3F800000U |
                (random32() & 0x007FFFFFU);
        }

        // f32 finite chain: CHAIN_STEPS full vectors.
        const size_t n32 = CHAIN_STEPS * data->vl_w;
        data->m_f32.resize(n32);
        data->a_f32.resize(n32);
        for (size_t i = 0; i < n32; ++i) {
            data->m_f32[i] = bounded ? random_finite_f32()
                                     : random_unbounded_f32();
            data->a_f32[i] = bounded ? random_finite_f32()
                                     : random_unbounded_f32();
        }

        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sve512_f32_chain_arm init: %s", e.what());
        return EXIT_SKIP;
    }
#endif
}

// ---------------------------------------------------------------------------
// test_run: f32 finite FMLA chain per iteration.
// ---------------------------------------------------------------------------
static int sve512_f32_chain_arm_run(struct test *test, int cpu)
{
    (void)cpu;
#ifndef __aarch64__
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 SVE required for "
             "sve512_f32_chain_arm");
    return EXIT_SKIP;
#else
    auto *d = static_cast<SveF32ChainData *>(test->data);

    /* All-VL policy #3: fail closed on init/run VL drift. */
    if (svcntw() != d->vl_w) {
        log_error("sve512_f32_chain_arm: VL drifted between init (%zu "
                  "lanes) and run (%u lanes)", d->vl_w, (unsigned)svcntw());
        report_fail_msg("sve512_f32_chain_arm: SVE VL drifted between "
                        "init and run");
    }

    // Golden, recomputed each iteration (deterministic, same op order).
    std::vector<uint32_t> gold32(d->vl_w);
    f32_chain_golden(d, gold32.data());

    do {
        bool all_passed = true;

        std::vector<uint32_t> hw(d->vl_w);
        f32_chain_hw(d, hw.data());
        for (size_t lane = 0; lane < d->vl_w; ++lane) {
            if (hw[lane] != gold32[lane]) {
                report_f32_lane_mismatch("sve512_f32_chain", lane,
                                         gold32[lane], hw[lane]);
                all_passed = false;
            }
        }

        if (!all_passed) {
            report_fail_msg(
                "sve512_f32_chain_arm: SVE f32 FMLA chain SDC detected");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#endif
}

// ---------------------------------------------------------------------------
// test_cleanup
// ---------------------------------------------------------------------------
static int sve512_f32_chain_arm_cleanup(struct test *test)
{
#ifdef __aarch64__
    delete static_cast<SveF32ChainData *>(test->data);
#else
    (void)test;
#endif
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(sve512_f32_chain_arm,
             "SVE full-vector-length f32 FMLA dependency chain SDC stress: "
             "512-step serial svmla_f32_x chain at the CPU's runtime SVE "
             "vector length (all-VL 128..1024-bit via "
             "arm64_require_sve_vl), byte-exact compared against a "
             "deterministic scalar fmaf() reference")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_f32_chain_arm_init,
    .test_run = sve512_f32_chain_arm_run,
    .test_cleanup = sve512_f32_chain_arm_cleanup,
    .fracture_loop_count = 0,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
