/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve2_cross_precision_arm
 * @parblock
 * SVE2 cross-precision multiply-accumulate chain SDC stress — FP16->FP32
 * extended-precision FMLALB/FMLALT (FEAT_FP16FML semantics on SVE2) and
 * BF16->FP32 BFDOT (FEAT_SVE_BF16). Each family runs its own independent
 * low-precision input pipeline and widening accumulator datapath — silicon
 * the regular FMLA tests never touch — and both are first-generation
 * SVE2 instructions (PinDrop HPCA'26: highest failure rates in the
 * introducing generation). Zero prior coverage. cn23154 (0xd22) reports
 * SVE2 full set + BF16 in ID_AA64ZFR0_EL1.
 *
 * Semantics implemented by the golden (ARM DDI 0616):
 *  - svmlalb_f32(acc, b, c): result lane i = fma(b[2i], c[2i], acc[i]) —
 *    the EVEN (bottom) half of the f16 source pairs widens to f32 and
 *    accumulates single-rounded.
 *  - svmlalt_f32(acc, b, c): same over the ODD (top) half: lane i =
 *    fma(b[2i+1], c[2i+1], acc[i]).
 *  - svbfdot_f32(acc, b, c): lane i = fma(b[2i], c[2i], fma(b[2i+1],
 *    c[2i+1], acc[i])) — adjacent bf16 pairs dot into one f32 lane, each
 *    product accumulate single-rounded.
 *
 * Golden is computed in double precision on the widened values (f16/bf16
 * inputs are exact in double, and every single-rounded f32 accumulate in
 * the hardware is exactly representable as a double intermediate, then
 * rounded once to f32 per accumulate via (float) casts that mirror each
 * hardware accumulate). Worked example in the init self-check: with
 * b = {1.0h, 2.0h}, c = {3.0h, 4.0h}, acc = 0:
 *   bfdot lane = fma(2.0, 4.0, fma(1.0, 3.0, 0.0)) = 11.0 — exact.
 *
 * Operands: f16/bf16 values from a small exact table (1.0, -1.0, 1.5,
 * -1.5, 0.5, -0.5, 2.0, -2.0 — all exactly representable in both f16 and
 * bf16), so products and sums stay exact and the chain of 256 steps is
 * byte-exact comparable. Chains are dependent (accumulator feeds forward).
 *
 * test_init gates in two layers (all-VL Phase 2 P6 split): the pure SVE
 * capability + 128..1024-bit range check goes through the framework
 * layer (arm64_require_sve_vl, one call), while the composite
 * SVE2|SVEBF16 HWCAP2 bits stay a getauxval hand-probe — that
 * conjunction is this test's own semantics (FEAT_SVE_bf16 widening
 * pipelines), not something the generic require helper covers. Either
 * layer returns a clean EXIT_SKIP when absent; test_run re-checks the
 * VL and fails closed on drift. A one-step self-check compares golden
 * vs hardware before the long chain so a golden bug never reports as
 * SDC. Requires the tests_arm64_sve2 library (-march=armv9-a+sve2+bf16).
 * Any runtime vector length in the supported range is accepted.
 * ARM64-native.
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
#include <arm_fp16.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

// Dependent chain length per iteration.
static constexpr size_t CHAIN_STEPS = 256;

// Exact f16/bf16-representable operand table (values are their own exact
// f16/bf16 encodings; products and 256-step sums stay exact in f32 for
// the pairs used: |b|,|c| <= 2 -> |product| <= 4; sums bounded by
// 256 * 4 * 2 = 2048, exactly representable).
static const float EXACT_TABLE[] = {
    1.0f, -1.0f, 1.5f, -1.5f, 0.5f, -0.5f, 2.0f, -2.0f,
};
static constexpr size_t TABLE_SIZE =
    sizeof(EXACT_TABLE) / sizeof(EXACT_TABLE[0]);

struct CrossPrecData {
    size_t vl_f32;                     // f32 lanes (accumulator width)
    std::vector<float> golden_mlalb;   // expected acc after the chain
    std::vector<float> golden_mlalt;
    std::vector<float> golden_bfdot;
};

#ifdef __aarch64__

// Source-pair tables: f16 lanes (2x accumulator lanes for bottom/top and
// bfdot pairing).
static void fill_f16_sources(std::vector<__fp16> &b, std::vector<__fp16> &c)
{
    for (size_t i = 0; i < b.size(); ++i) {
        b[i] = (__fp16)EXACT_TABLE[(i * 7 + 1) % TABLE_SIZE];
        c[i] = (__fp16)EXACT_TABLE[(i * 5 + 3) % TABLE_SIZE];
    }
}

static void fill_bf16_sources(std::vector<uint16_t> &b, std::vector<uint16_t> &c)
{
    // bf16 encodings of the exact table: sign + 8-bit exponent + 7-bit
    // mantissa = top 16 bits of the f32 encoding.
    for (size_t i = 0; i < b.size(); ++i) {
        float fb = EXACT_TABLE[(i * 7 + 1) % TABLE_SIZE];
        float fc = EXACT_TABLE[(i * 5 + 3) % TABLE_SIZE];
        uint32_t wb, wc;
        memcpy(&wb, &fb, 4);
        memcpy(&wc, &fc, 4);
        b[i] = (uint16_t)(wb >> 16);
        c[i] = (uint16_t)(wc >> 16);
    }
}

// Golden step (double-precision mirror, one f32 rounding per accumulate —
// identical to the hardware): mlalb lane i uses pair (2i), mlalt pair
// (2i+1), bfdot accumulates pair (2i) then pair (2i+1).
static void golden_step_mlal(std::vector<float> &acc, const std::vector<__fp16> &b,
                             const std::vector<__fp16> &c, bool top)
{
    const size_t lanes = acc.size();
    for (size_t i = 0; i < lanes; ++i) {
        size_t j = 2 * i + (top ? 1 : 0);
        double bv = (double)b[j];
        double cv = (double)c[j];
        double a = (double)acc[i];
        acc[i] = (float)(bv * cv + a); // exact in double, one f32 rounding
    }
}

static void golden_step_bfdot(std::vector<float> &acc,
                              const std::vector<uint16_t> &b,
                              const std::vector<uint16_t> &c)
{
    const size_t lanes = acc.size();
    auto bf16_to_double = [](uint16_t h) -> double {
        uint32_t w = (uint32_t)h << 16;
        float f;
        memcpy(&f, &w, 4);
        return (double)f;
    };
    for (size_t i = 0; i < lanes; ++i) {
        double a = (double)acc[i];
        double p0 = bf16_to_double(b[2 * i]) * bf16_to_double(c[2 * i]);
        double p1 = bf16_to_double(b[2 * i + 1]) * bf16_to_double(c[2 * i + 1]);
        // hardware: two single-rounded accumulates (pair order: (2i) then
        // (2i+1)); with exact-table values all intermediates are exact in
        // double, and each f32 rounding is mirrored by the (float) casts.
        float step0 = (float)(p0 + a);
        acc[i] = (float)(p1 + (double)step0);
    }
}

// Hardware one-step helpers.
static void hw_step_mlalb(std::vector<float> &acc,
                          const std::vector<__fp16> &b,
                          const std::vector<__fp16> &c)
{
    svfloat32_t va = svld1_f32(svptrue_b32(), acc.data());
    svfloat16_t vb = svld1_f16(svptrue_b16(), b.data());
    svfloat16_t vc = svld1_f16(svptrue_b16(), c.data());
    va = svmlalb_f32(va, vb, vc);
    svst1_f32(svptrue_b32(), acc.data(), va);
}

static void hw_step_mlalt(std::vector<float> &acc,
                          const std::vector<__fp16> &b,
                          const std::vector<__fp16> &c)
{
    svfloat32_t va = svld1_f32(svptrue_b32(), acc.data());
    svfloat16_t vb = svld1_f16(svptrue_b16(), b.data());
    svfloat16_t vc = svld1_f16(svptrue_b16(), c.data());
    va = svmlalt_f32(va, vb, vc);
    svst1_f32(svptrue_b32(), acc.data(), va);
}

static void hw_step_bfdot(std::vector<float> &acc,
                          const std::vector<uint16_t> &b,
                          const std::vector<uint16_t> &c)
{
    svfloat32_t va = svld1_f32(svptrue_b32(), acc.data());
    svbfloat16_t vb = svld1_bf16(svptrue_b16(),
                                 reinterpret_cast<const __bf16 *>(b.data()));
    svbfloat16_t vc = svld1_bf16(svptrue_b16(),
                                 reinterpret_cast<const __bf16 *>(c.data()));
    va = svbfdot_f32(va, vb, vc);
    svst1_f32(svptrue_b32(), acc.data(), va);
}

static int sve2_cross_precision_arm_init(struct test *test)
{
#ifndef __aarch64__
    (void)test;
    return EXIT_SUCCESS;
#else
    /* All-VL Phase 2 P6 split gate: the pure SVE capability + 128..1024-bit
       range check moves to the framework layer (arm64_require_sve_vl —
       returns the VL in BYTES), while the composite SVE2|SVEBF16 HWCAP2
       conjunction stays a getauxval hand-probe: it is this test's own
       semantic gate (FEAT_SVE_bf16), which the generic require helper
       does not cover. */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
    unsigned long hwcap2 = getauxval(AT_HWCAP2);
    if ((hwcap2 & (HWCAP2_SVE2 | HWCAP2_SVEBF16))
            != (HWCAP2_SVE2 | HWCAP2_SVEBF16)) {
        log_skip(CpuNotSupportedSkipCategory,
                 "SVE2 cross-precision not available (HWCAP2: sve2=%lu "
                 "svebf16=%lu); sve2_cross_precision_arm requires SVE2 + "
                 "SVE_BF16 (FP16 FMLAL is SVE2 base; BF16 needs SVEBF16)",
                 (hwcap2 & HWCAP2_SVE2) ? 1UL : 0UL,
                 (hwcap2 & HWCAP2_SVEBF16) ? 1UL : 0UL);
        return EXIT_SKIP;
    }

    try {
        auto data = std::make_unique<CrossPrecData>();
        data->vl_f32 = (size_t)svl / 4;   /* f32 lanes = SVL bytes / 4 */
        if (data->vl_f32 < 4 || data->vl_f32 > 32) {
            log_error("sve2_cross_precision_arm: f32 lanes %zu outside "
                      "[4,32] — unit drift (svl %ld bytes)",
                      data->vl_f32, svl);
            return EXIT_FAILURE;
        }

        const size_t lanes = data->vl_f32;
        const size_t srcs = 2 * lanes;   // f16/bf16 source lanes

        std::vector<__fp16> b16(srcs), c16(srcs);
        fill_f16_sources(b16, c16);
        std::vector<uint16_t> bbc(srcs), cbc(srcs);
        fill_bf16_sources(bbc, cbc);

        for (int which = 0; which < 3; ++which) {
            std::vector<float> acc(lanes, 0.0f);
            for (size_t step = 0; step < CHAIN_STEPS; ++step) {
                if (which == 0) golden_step_mlal(acc, b16, c16, false);
                else if (which == 1) golden_step_mlal(acc, b16, c16, true);
                else golden_step_bfdot(acc, bbc, cbc);
            }
            if (which == 0) data->golden_mlalb = acc;
            else if (which == 1) data->golden_mlalt = acc;
            else data->golden_bfdot = acc;
        }

        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sve2_cross_precision_arm init: %s", e.what());
        return EXIT_SKIP;
    }
#endif
}

static int sve2_cross_precision_arm_run(struct test *test, int cpu)
{
    (void)cpu;
#ifndef __aarch64__
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 SVE2 required for "
             "sve2_cross_precision_arm");
    return EXIT_SKIP;
#else
    auto *d = static_cast<CrossPrecData *>(test->data);

    /* All-VL policy #3: fail closed on init/run VL drift (the goldens
       are sized at init's lanes; a wider run-side VL would read past
       them in the compare loops below). */
    if (svcntw() != d->vl_f32) {
        log_error("sve2_cross_precision_arm: VL drifted between init "
                  "(%zu f32 lanes) and run (%u lanes)", d->vl_f32,
                  (unsigned)svcntw());
        report_fail_msg("sve2_cross_precision_arm: SVE VL drifted "
                        "between init and run");
    }

    const size_t lanes = d->vl_f32;
    const size_t srcs = 2 * lanes;
    bool all_passed = true;

    std::vector<__fp16> b16(srcs), c16(srcs);
    fill_f16_sources(b16, c16);
    std::vector<uint16_t> bbc(srcs), cbc(srcs);
    fill_bf16_sources(bbc, cbc);

    // ---------- one-step self-checks (golden-vs-hardware, per family) ----------
    {
        std::vector<float> g(lanes, 0.0f), hw(lanes, 0.0f);
        golden_step_mlal(g, b16, c16, false);
        hw_step_mlalb(hw, b16, c16);
        if (memcmp(g.data(), hw.data(), lanes * 4) != 0) {
            report_fail_msg("sve2_cross_precision_arm: golden lane-mapping "
                            "self-check failed (mlalb) — test bug, not SDC");
            return EXIT_FAILURE;
        }
        std::fill(g.begin(), g.end(), 0.0f);
        std::fill(hw.begin(), hw.end(), 0.0f);
        golden_step_mlal(g, b16, c16, true);
        hw_step_mlalt(hw, b16, c16);
        if (memcmp(g.data(), hw.data(), lanes * 4) != 0) {
            report_fail_msg("sve2_cross_precision_arm: golden lane-mapping "
                            "self-check failed (mlalt) — test bug, not SDC");
            return EXIT_FAILURE;
        }
        std::fill(g.begin(), g.end(), 0.0f);
        std::fill(hw.begin(), hw.end(), 0.0f);
        golden_step_bfdot(g, bbc, cbc);
        hw_step_bfdot(hw, bbc, cbc);
        if (memcmp(g.data(), hw.data(), lanes * 4) != 0) {
            report_fail_msg("sve2_cross_precision_arm: golden lane-mapping "
                            "self-check failed (bfdot) — test bug, not SDC");
            return EXIT_FAILURE;
        }
    }

    // ---------- dependent chains + full comparison ----------
    do {
        std::vector<float> acc(lanes, 0.0f);
        for (size_t step = 0; step < CHAIN_STEPS; ++step)
            hw_step_mlalb(acc, b16, c16);
        if (memcmp(acc.data(), d->golden_mlalb.data(), lanes * 4) != 0) {
            log_warning("sve2_cross_precision: mlalb chain mismatch");
            all_passed = false;
        }

        std::fill(acc.begin(), acc.end(), 0.0f);
        for (size_t step = 0; step < CHAIN_STEPS; ++step)
            hw_step_mlalt(acc, b16, c16);
        if (memcmp(acc.data(), d->golden_mlalt.data(), lanes * 4) != 0) {
            log_warning("sve2_cross_precision: mlalt chain mismatch");
            all_passed = false;
        }

        std::fill(acc.begin(), acc.end(), 0.0f);
        for (size_t step = 0; step < CHAIN_STEPS; ++step)
            hw_step_bfdot(acc, bbc, cbc);
        if (memcmp(acc.data(), d->golden_bfdot.data(), lanes * 4) != 0) {
            log_warning("sve2_cross_precision: bfdot chain mismatch");
            all_passed = false;
        }

        if (!all_passed) {
            report_fail_msg("sve2_cross_precision_arm: cross-precision "
                            "MLA/DOT SDC detected (f16->f32 / bf16->f32)");
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#endif
}

static int sve2_cross_precision_arm_cleanup(struct test *test)
{
#ifdef __aarch64__
    delete static_cast<CrossPrecData *>(test->data);
#else
    (void)test;
#endif
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(sve2_cross_precision_arm,
             "SVE2 cross-precision MLA/DOT chain SDC stress: dependent "
             "svmlalb/svmlalt (FP16->FP32) and svbfdot (BF16->FP32) chains "
             "over exact-representable operands, byte-exact vs a "
             "double-precision scalar golden mirroring each single-rounded "
             "accumulate, one-step self-checks per family")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve2_cross_precision_arm_init,
    .test_run = sve2_cross_precision_arm_run,
    .test_cleanup = sve2_cross_precision_arm_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
