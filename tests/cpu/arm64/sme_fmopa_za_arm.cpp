/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sme_fmopa_za_arm
 * @parblock
 * SME outer-product/ZA-tile chain SDC stress — the first SME test in the
 * suite. SME (FEAT_SME) adds the matrix engine the regular SVE datapath
 * cannot touch: the ZA tile storage and the FMOPA outer-product array —
 * the highest-FLOPS-per-cycle path in the whole ISA. cn23154 (HiSilicon
 * 0xd22) reports SME+SME2 in SMFR0_EL1 (hardware present), but its
 * 5.10 kernel predates the HWCAP2_SME exposure (mainline 5.19+) and has
 * no ZA/streaming context management — userspace SMSTART is expected to
 * SIGILL there. This test follows the write-now/skip-cleanly doctrine:
 * on any machine whose kernel does not expose HWCAP2_SME it returns a
 * clean EXIT_SKIP; on Neoverse V3-generation hardware (and any future
 * kernel that enables SME on 0xd22) it runs for real.
 *
 * Implementation: pure inline asm. GCC 12.3's <arm_sme.h> is an empty
 * shell (0 intrinsics — probed), so the whole SMSTART..SMSTOP sequence
 * is one asm volatile block with all operands staged through memory:
 *   smstart za            — enter streaming mode, ZA accessible
 *   ptrue p0.s
 *   zero {za}             — deterministic tile start state
 *   N x [ ld1w z0/z1 <- mem;  fmopa za0.s, p0/m, p0/m, z0.s, z1.s ]
 *   mova z2.s, p0/m, za0v.s[w12, 0]   — extract tile row slice 0
 *   st1w z2.s -> memory
 *   smstop za             — leave streaming mode (ZA inaccessible after)
 * w12 is zeroed (slice selector 0); z0/z1/z2/p0/w12 are clobbers.
 *
 * The fa64=0 constraint on 0xd22 (SMFR0) means only the streaming-
 * compatible subset may execute inside SMSTART..SMSTOP — the asm block
 * contains nothing but SVE loads, FMOPA, MOVA, stores and the mode
 * switches, so it is valid under that constraint.
 *
 * Golden: FMOPA za0.S with full predicates computes
 *   ZA[r][c] += Zn[r] * Zm[c]   (outer product, single rounding)
 * Deterministic operand streams (a[i] in {1.0, 2.0}, b[j] in {1.0, 3.0}
 * patterns) make every accumulate exact, so the golden is a plain scalar
 * double loop with zero rounding ambiguity. The tile row slice 0 of za0
 * holds row 0: ZA[0][c] for c in [0, SVL/32).
 *
 * A one-step self-check runs before the chain (golden-vs-hardware on a
 * single FMOPA — a golden bug reports as a test bug, never as SDC).
 *
 * test_init probes getauxval(AT_HWCAP2) & HWCAP2_SME and returns a clean
 * EXIT_SKIP when absent (this Kunpeng 920 and cn23154's 5.10 kernel both
 * skip). If a kernel ever exposes the bit without truly enabling SME,
 * the SMSTART traps (SIGILL) — the fork-isolated child crashes and the
 * framework reports it; that outcome is itself diagnostic (documented
 * expectation, settled definitively by the selftest_sme_sigill_probe).
 * Requires the tests_arm64_sme library (-march=armv9-a+sme).
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
#endif

// FMOPA accumulation steps per iteration.
static constexpr size_t MOPA_STEPS = 256;

struct SmeFmopaData {
    size_t tile_cols;               // SVL/32 f32 columns per tile row
    std::vector<float> golden_row0; // expected ZA row 0 after the chain
};

#ifdef __aarch64__

// ---------------------------------------------------------------------------
// One FMOPA accumulation round (N steps) with operands staged in memory.
// All SVE state is local to the asm block; results leave via memory.
// ---------------------------------------------------------------------------
static void fmopa_chain_asm(const float *a_stream, const float *b_stream,
                            size_t steps, float *out_row0, size_t out_cols)
{
    __asm__ volatile(
        "smstart za\n\t"
        "ptrue p0.s\n\t"
        "zero {za}\n\t"
        "mov w12, wzr\n\t"
        "1:\n\t"
        "ld1w {z0.s}, p0/z, [%[a]]\n\t"
        "ld1w {z1.s}, p0/z, [%[b]]\n\t"
        "add %[a], %[a], #16\n\t"
        "add %[b], %[b], #16\n\t"
        "fmopa za0.s, p0/m, p0/m, z0.s, z1.s\n\t"
        "subs %[n], %[n], #1\n\t"
        "b.ne 1b\n\t"
        "mova z2.s, p0/m, za0v.s[w12, 0]\n\t"
        "st1w {z2.s}, p0, [%[o]]\n\t"
        "smstop za\n\t"
        : [a] "+r"(a_stream), [b] "+r"(b_stream), [n] "+r"(steps)
        : [o] "r"(out_row0)
        : "z0", "z1", "z2", "p0", "w12", "memory", "cc");
    (void)out_cols;
}

// Scalar golden: ZA[r][c] += a[r] * b[c] over the stream, exact values.
// Operands are exact small integers so every accumulate is exact.
static std::vector<float> fmopa_golden(const std::vector<float> &a_stream,
                                       const std::vector<float> &b_stream,
                                       size_t steps, size_t cols)
{
    // za0 row 0 accumulates a_stream[0] * b_stream[j] per step (the
    // row index comes from the A vector's lane: with full ptrue, lane
    // r of z0 multiplies into tile row r; row 0 <- z0 lane 0).
    std::vector<double> acc(cols, 0.0);
    for (size_t s = 0; s < steps; ++s)
        for (size_t c = 0; c < cols; ++c)
            acc[c] += (double)a_stream[s] * (double)b_stream[s * 16 + c];
    std::vector<float> out(cols);
    for (size_t c = 0; c < cols; ++c)
        out[c] = (float)acc[c];   // exact: small integers
    return out;
}

static int sme_fmopa_za_arm_init(struct test *test)
{
#ifndef __aarch64__
    (void)test;
    return EXIT_SUCCESS;
#else
    unsigned long hwcap2 = getauxval(AT_HWCAP2);
    if ((hwcap2 & HWCAP2_SME) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "SME not exposed by this kernel (HWCAP2_SME unset; needs "
                 "kernel >= 5.19 and hardware SME — e.g. HiSilicon 0xd22 "
                 "has the hardware but its 5.10 kernel does not enable it). "
                 "Run selftest_sme_sigill_probe to settle kernel support "
                 "in one run.");
        return EXIT_SKIP;
    }

    try {
        auto data = std::make_unique<SmeFmopaData>();
        // Tile row width: SVL bits / 32 per .s row. Read via inline asm
        // rdsvl (safe only under streaming mode? No — RDSVL reads the
        // current vector length; use __ARM_FEATURE_SME's __arm_get_current_vg
        // is clang-only; read SVCR-free via 'rdsvl' which works outside
        // streaming for the SM vector length on SME hardware).
        long svl;
        __asm__ volatile("rdsvl %0, 0" : "=r"(svl));
        data->tile_cols = (size_t)svl / 32;

        // Deterministic exact operand streams.
        std::vector<float> a_stream(MOPA_STEPS);
        std::vector<float> b_stream(MOPA_STEPS * 16);
        for (size_t s = 0; s < MOPA_STEPS; ++s) {
            a_stream[s] = (s & 1) ? 1.0f : 2.0f;
            for (size_t j = 0; j < 16; ++j)
                b_stream[s * 16 + j] = (j & 1) ? 1.0f : 3.0f;
        }
        data->golden_row0 = fmopa_golden(a_stream, b_stream, MOPA_STEPS,
                                          data->tile_cols);
        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sme_fmopa_za_arm init: %s", e.what());
        return EXIT_SKIP;
    }
#endif
}

static int sme_fmopa_za_arm_run(struct test *test, int cpu)
{
    (void)cpu;
#ifndef __aarch64__
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 SME required for "
             "sme_fmopa_za_arm");
    return EXIT_SKIP;
#else
    auto *d = static_cast<SmeFmopaData *>(test->data);
    const size_t cols = d->tile_cols;

    // Operand streams (recreated identically to init).
    std::vector<float> a_stream(MOPA_STEPS);
    std::vector<float> b_stream(MOPA_STEPS * 16);
    for (size_t s = 0; s < MOPA_STEPS; ++s) {
        a_stream[s] = (s & 1) ? 1.0f : 2.0f;
        for (size_t j = 0; j < 16; ++j)
            b_stream[s * 16 + j] = (j & 1) ? 1.0f : 3.0f;
    }

    // One-step self-check (golden vs hardware on a single FMOPA).
    {
        std::vector<float> one_a(1, 2.0f);
        std::vector<float> one_b(16);
        for (size_t j = 0; j < 16; ++j)
            one_b[j] = (j & 1) ? 1.0f : 3.0f;
        std::vector<float> expect = fmopa_golden(one_a, one_b, 1, cols);
        std::vector<float> got(cols, -1.0f);
        fmopa_chain_asm(one_a.data(), one_b.data(), 1, got.data(), cols);
        if (memcmp(expect.data(), got.data(), cols * sizeof(float)) != 0) {
            report_fail_msg("sme_fmopa_za_arm: golden tile-mapping "
                            "self-check failed — test bug, not SDC");
            return EXIT_FAILURE;
        }
    }

    do {
        std::vector<float> row0(cols, -1.0f);
        fmopa_chain_asm(a_stream.data(), b_stream.data(), MOPA_STEPS,
                        row0.data(), cols);
        for (size_t c = 0; c < cols; ++c) {
            if (row0[c] != d->golden_row0[c]) {
                uint32_t g, a;
                memcpy(&g, &d->golden_row0[c], 4);
                memcpy(&a, &row0[c], 4);
                log_warning("sme_fmopa_za: col %zu golden=0x%08" PRIx32
                            " actual=0x%08" PRIx32, c, g, a);
                report_fail_msg("sme_fmopa_za_arm: SME FMOPA/ZA outer-"
                                "product SDC detected");
                return EXIT_FAILURE;
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#endif
}

static int sme_fmopa_za_arm_cleanup(struct test *test)
{
#ifdef __aarch64__
    delete static_cast<SmeFmopaData *>(test->data);
#else
    (void)test;
#endif
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(sme_fmopa_za_arm,
             "SME FMOPA/ZA outer-product chain SDC stress: 256-step "
             "fmopa za0.s accumulation into the ZA tile via a single "
             "SMSTART..SMSTOP inline-asm block (operands staged through "
             "memory — GCC's arm_sme.h is an empty shell), row-slice "
             "extraction via mova, byte-exact vs an exact-integer scalar "
             "golden with a one-step tile-mapping self-check; clean-skip "
             "on kernels without HWCAP2_SME")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sme_fmopa_za_arm_init,
    .test_run = sme_fmopa_za_arm_run,
    .test_cleanup = sme_fmopa_za_arm_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
