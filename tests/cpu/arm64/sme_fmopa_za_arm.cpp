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
 *   smstart sm + smstart za — enter streaming mode AND enable ZA. The
 *                            double-switch is mandatory: FMOPA requires
 *                            SVCR.SM=1, and a za-only window traps on
 *                            the first FMOPA (gem5 SME model
 *                            smEnCheckCode; empirically settled at all
 *                            four tiers by the dual kernel, gem5-fi
 *                            feat/fmopa-za-dual @ ebe08632). Inside the
 *                            streaming window only the streaming-
 *                            compatible subset may run (the fa64=0
 *                            constraint on 0xd22) — the block contains
 *                            nothing but SVE loads, FMOPA, MOVA, stores
 *                            and the mode switches.
 *   ptrue p0.s
 *   zero {za}             — deterministic tile start state
 *   N x [ ld1w z0/z1 <- mem;  a/b += stride;  fmopa za0.s, p0/m, p0/m ]
 *   mova z2.s, p0/m, za0v.s[w12, 0]   — extract a ZA vector slice
 *   st1w z2.s -> memory
 *   smstop za + smstop sm — leave both (ZA inaccessible, streaming off)
 * w12 is zeroed (slice selector 0); z0/z1/z2/p0/w12 are clobbers.
 *
 * All-VL Phase 2 P2 rework (spec
 * docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md §6): the
 * pre-P2 form had never executed on any hardware and carried four
 * independent defects, fixed here:
 *   1. `rdsvl %0, 0` (imm 0) always returned 0 -> tile_cols 0 -> empty
 *      golden and a heap overflow on real SME hardware (RDSVL multiplies
 *      SVL_bytes by the immediate);
 *   2. the asm advanced the operand pointers by #16 BYTES while the
 *      golden indexed 16 FLOATS per step — inconsistent at every SVL;
 *   3. the golden modelled ZA row 0 while `mova ..., za0v.s[w12, 0]`
 *      reads the COLUMN view (the za_tileview probe, gem5-verified at
 *      four tiers in PR #242, established za0v = column/transpose);
 *   4. `smstart za` alone cannot execute FMOPA (SVCR.SM=1 required) —
 *      the sm+za double-switch is mandatory (four-tier gem5 verdict,
 *      dual kernel ebe08632).
 * The rework: SVL via the framework layer (`arm64_require_sme_svl`,
 * 128..1024-bit range check — the helper returns the SVL in BYTES),
 * per-step operand WINDOWS of cols = SVL_bits/32 = SVL_bytes/4 f32
 * lanes, the window stride registerized (cols * sizeof(float) as an
 * asm operand — element size stays fixed), and the golden follows the
 * gem5-verified axis verdict below. This is the test's FIRST FUNCTIONAL
 * FORM; there is no regression anchor (the pre-P2 form produced no
 * valid output anywhere).
 *
 * Golden: FMOPA za0.S with full predicates computes
 *   ZA[r][c] += Zn[r] * Zm[c]   (outer product, single rounding)
 * Operands are exact small integers in window layout [s*cols + i]
 * (a: (i%7)+1, b: (i%5)+1 over the absolute index), so every product
 * and accumulate is exactly representable — zero rounding ambiguity,
 * and gem5's fplib double-rounding (F4) coincides with hardware.
 * The readout vector golden_readout[r] follows the axis verdict of the
 * same-source dual kernel (gem5-fi workloads/sdcshield/
 * sme_fmopa_za_dual.c, four tiers x {fixed, random x3}):
 *   COLUMN: mova za0v.s[w12,0] w12=0 reads column 0 —
 *           readout[r] = sum_s a[s*cols+r] * b[s*cols]
 *
 * A one-step self-check runs before the chain (golden-vs-hardware on a
 * single FMOPA — no independent oracle: the check rides the same
 * FMOPA/ZA datapath as the main chain, so a self-check mismatch could be
 * a test-model bug or real SDC and must be investigated, not presumed).
 * test_run re-checks the SVL against the value recorded at init and
 * fails closed on drift (never continues with stale geometry).
 *
 * test_init gates via arm64_require_sme_svl() (framework arm64_vl.h
 * layer: HWCAP2_SME capability + 128..1024-bit SVL range, honest
 * runtime probing immune to the device_has_feature compile-time
 * short-circuit). If a kernel ever exposes the bit without truly
 * enabling SME, the SMSTART traps (SIGILL) — the fork-isolated child
 * crashes and the framework reports it; that outcome is itself
 * diagnostic (documented expectation, settled definitively by the
 * selftest_sme_sigill_probe).
 * Requires the tests_arm64_sme library (-march=armv9-a+sme).
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <memory>
#include <vector>

#ifdef __aarch64__
#include <device/cpu/arm64/arm64_vl.h>
#endif

// FMOPA accumulation steps per iteration (SVL-independent).
static constexpr size_t MOPA_STEPS = 256;

struct SmeFmopaData {
    size_t tile_cols;               // SVL/32 f32 columns per tile row
    std::vector<float> a_stream;    // operand windows, [s*cols + i]
    std::vector<float> b_stream;
    std::vector<float> golden_readout; // expected mova readout after chain
};

#ifdef __aarch64__

// ---------------------------------------------------------------------------
// One FMOPA accumulation round (N steps) with operands staged in memory.
// All SVE state is local to the asm block; results leave via memory.
// The window stride (cols * sizeof(float)) is a REGISTER operand — the
// PR #242 invariant: geometry scales with SVL, element size stays fixed.
// ---------------------------------------------------------------------------
static void fmopa_chain_asm(const float *a_stream, const float *b_stream,
                            size_t steps, float *out_vec, size_t cols)
{
    const size_t stride = cols * sizeof(float);
    __asm__ volatile(
        "smstart sm\n\t"
        "smstart za\n\t"
        "ptrue p0.s\n\t"
        "zero {za}\n\t"
        "mov w12, wzr\n\t"
        "1:\n\t"
        "ld1w {z0.s}, p0/z, [%[a]]\n\t"
        "ld1w {z1.s}, p0/z, [%[b]]\n\t"
        "add %[a], %[a], %[s]\n\t"
        "add %[b], %[b], %[s]\n\t"
        "fmopa za0.s, p0/m, p0/m, z0.s, z1.s\n\t"
        "subs %[n], %[n], #1\n\t"
        "b.ne 1b\n\t"
        "mova z2.s, p0/m, za0v.s[w12, 0]\n\t"
        "st1w {z2.s}, p0, [%[o]]\n\t"
        "smstop za\n\t"
        "smstop sm\n\t"
        : [a] "+r"(a_stream), [b] "+r"(b_stream), [n] "+r"(steps)
        : [o] "r"(out_vec), [s] "r"(stride)
        : "z0", "z1", "z2", "p0", "w12", "memory", "cc");
}

// Scalar golden over the operand windows, exact integer accumulation.
// Axis verdict (gem5-fi sme_fmopa_za_dual @ ebe08632, four tiers x
// {fixed, random x3} = 16 runs, SDCSHIELD_FAILS=0): mova
// za0v.s[w12,0] with w12=0 reads COLUMN 0 —
//   readout[r] = ZA[r][0] = sum_s z0[r] * z1[0] = sum_s a[s*cols+r] * b[s*cols]
static std::vector<float> fmopa_golden(const std::vector<float> &a_stream,
                                       const std::vector<float> &b_stream,
                                       size_t steps, size_t cols)
{
    std::vector<float> out(cols, 0.0f);
    for (size_t s = 0; s < steps; ++s) {
        const size_t s0 = s * cols;
        for (size_t r = 0; r < cols; ++r)
            out[r] += a_stream[s0 + r] * b_stream[s0];  /* exact integers */
    }
    return out;
}

// Exact-integer operand windows, [s*cols + i] over the absolute index.
static void fill_operand_windows(std::vector<float> &a_stream,
                                 std::vector<float> &b_stream, size_t steps,
                                 size_t cols)
{
    a_stream.resize(steps * cols);
    b_stream.resize(steps * cols);
    for (size_t si = 0; si < a_stream.size(); ++si) {
        a_stream[si] = (float)((si % 7) + 1);
        b_stream[si] = (float)((si % 5) + 1);
    }
}

static int sme_fmopa_za_arm_init(struct test *test)
{
#ifndef __aarch64__
    (void)test;
    return EXIT_SUCCESS;
#else
    /* Framework layer: HWCAP2_SME capability + 128..1024-bit SVL range
       check in one call; -1 means the skip was already logged. The
       return value is the SVL in BYTES; cols = SVL_bits/32 = svl_bytes/4
       (f32 lanes per tile row) — the exact mapping the dual kernel uses
       (sme_fmopa_za_dual.c: cols = svl_bytes/4). The explicit range
       check below catches any unit drift on the first SME host to run
       this, release builds included (assert() is compiled out under
       NDEBUG; adversarial review Critical-1: the pre-fix form divided
       bytes by 32 — an 8x error no local run could expose). */
    const long svl = arm64_require_sme_svl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    try {
        auto data = std::make_unique<SmeFmopaData>();
        data->tile_cols = (size_t)svl / 4;
        if (data->tile_cols < 4 || data->tile_cols > 32) {
            log_error("sme_fmopa_za_arm: cols %zu outside [4,32] — unit "
                      "drift (svl %ld bytes)", data->tile_cols, svl);
            return EXIT_FAILURE;
        }

        fill_operand_windows(data->a_stream, data->b_stream, MOPA_STEPS,
                             data->tile_cols);
        data->golden_readout = fmopa_golden(data->a_stream, data->b_stream,
                                            MOPA_STEPS, data->tile_cols);
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

    /* Fail-closed: never continue with stale geometry if the SVL drifted
       between init and run (PR #242 review invariant). arm64_sme_svl_bytes
       returns BYTES — same /4 mapping as init. */
    if ((size_t)arm64_sme_svl_bytes() / 4 != cols) {
        log_error("sme_fmopa_za_arm: SVL drifted between init (%zu cols) "
                  "and run (%ld cols)", cols, arm64_sme_svl_bytes() / 4);
        report_fail_msg("sme_fmopa_za_arm: SVL drifted between init and run");
    }

    /* One-step self-check (golden-vs-hardware on a single FMOPA — no
       independent oracle, same FMOPA/ZA datapath: a mismatch could be a
       test-model bug or real SDC, investigate before concluding). */
    {
        std::vector<float> one_a(cols, 2.0f), one_b(cols);
        for (size_t j = 0; j < cols; ++j)
            one_b[j] = (j & 1) ? 1.0f : 3.0f;
        std::vector<float> expect = fmopa_golden(one_a, one_b, 1, cols);
        std::vector<float> got(cols, -1.0f);
        fmopa_chain_asm(one_a.data(), one_b.data(), 1, got.data(), cols);
        if (memcmp(expect.data(), got.data(), cols * sizeof(float)) != 0) {
            report_fail_msg("sme_fmopa_za_arm: tile-mapping self-check "
                            "mismatch — no independent oracle (same "
                            "FMOPA/ZA datapath): test-model bug or real "
                            "SDC, investigate");
        }
    }

    do {
        std::vector<float> out(cols, -1.0f);
        fmopa_chain_asm(d->a_stream.data(), d->b_stream.data(), MOPA_STEPS,
                        out.data(), cols);
        /* Byte-exact per-lane comparison (spec §8 invariant: no cross-lane
           aggregation); log the first mismatch for diagnosis. */
        if (memcmp(out.data(), d->golden_readout.data(),
                   cols * sizeof(float)) != 0) {
            for (size_t c = 0; c < cols; ++c)
                if (out[c] != d->golden_readout[c]) {
                    uint32_t g, a;
                    memcpy(&g, &d->golden_readout[c], 4);
                    memcpy(&a, &out[c], 4);
                    log_warning("sme_fmopa_za: lane %zu golden=0x%08" PRIx32
                                " actual=0x%08" PRIx32, c, g, a);
                    break;
                }
            report_fail_msg("sme_fmopa_za_arm: SME FMOPA/ZA outer-"
                            "product SDC detected");
            return EXIT_FAILURE;
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
             "memory — GCC's arm_sme.h is an empty shell), ZA readout "
             "via mova, byte-exact vs an exact-integer scalar golden "
             "with a one-step tile-mapping self-check; all-VL "
             "(128..1024-bit via arm64_require_sme_svl); clean-skip on "
             "kernels without HWCAP2_SME")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sme_fmopa_za_arm_init,
    .test_run = sme_fmopa_za_arm_run,
    .test_cleanup = sme_fmopa_za_arm_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
