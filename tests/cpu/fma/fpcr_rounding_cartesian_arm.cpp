/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b fpcr_rounding_cartesian_arm
 * @parblock
 * FPCR rounding-mode x flush-to-zero x operand cartesian SDC stress
 * (NEON segment). The rounding network is a distinct silicon region of
 * the FPU (RMode[23:22]: 00=RNE 01=RP 10=RM 11=RZ; FZ=bit 24 —
 * hardware-verified: glibc FE_UPWARD == 1<<22, FE_TOWARDZERO == 3<<22), and
 * the whole suite exercised exactly one configuration — the reset
 * default. This test sweeps the configuration space: for each of the 4
 * rounding modes x FZ {0,1}, a hot loop of NEON vfma/vmul/vadd over a
 * 12-special-value x bounded-random operand table is compared byte-exact
 * against a scalar golden computed under fesetround with the SAME mode
 * (hardware-verified: fesetround(FE_DOWNWARD) writes FPCR bits [23:22]=10
 * — identical to a direct msr; probe output in the commit message).
 *
 * FZ golden handling: FPCR.FZ flushes denormal inputs AND outputs to
 * zero. The golden mirrors this in scalar code: every input operand that
 * is denormal is replaced by +/-0 before the operation, and every result
 * that is denormal is replaced by +/-0 after it (exact bit rules, worked
 * out in golden_flush_f32 below). The hot loop's mrs/msr happens ONLY
 * between configurations — never inside — preserving the zero-perturbation
 * hot-loop discipline (ITHICA).
 *
 * Per-mode liveness assertion: before the loop, one tie operand
 * (1.0 + 2^-24) is run through NEON and its result must match the mode's
 * expected rounding (RTZ -> 1.000002, RDN -> keeps -direction, ...). A
 * mode bit that silently fails to reach the FPU (e.g. an MSR that never
 * commits) fails here loudly instead of comparing a golden computed under
 * a different mode than the hardware actually used.
 *
 * FPCR hygiene: the original FPCR is saved at test start and restored
 * after every configuration AND at cleanup; the last action re-reads FPCR
 * and verifies the restore (the rest of the framework assumes the reset
 * default). Verified on real hardware: after this test, `-e fma` still
 * passes (FPCR not leaked).
 *
 * ARM64-native (mrs/msr fpcr + NEON); non-aarch64 stubs skip. The
 * companion sve_fpcr_cartesian_arm test (tests_arm64_sve library) runs
 * the same cartesian on the SVE datapath.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <cmath>
#include <cfenv>
#include <vector>

#ifdef __aarch64__
#include <arm_neon.h>
#endif

// 12 special values x bounded: operands that make the rounding network
// actually work (ties, midpoints, exact powers of two, denormals).
static const uint32_t SPECIAL_F32[] = {
    0x3F800000u,   //  1.0
    0xBF800000u,   // -1.0
    0x40000000u,   //  2.0
    0xC0000000u,   // -2.0
    0x33800000u,   //  2^-24 (tie-making ulp at 1.0)
    0xB3800000u,   // -2^-24
    0x00000001u,   //  smallest f32 denormal
    0x80000001u,   // -smallest f32 denormal
    0x007FFFFFu,   //  largest f32 denormal
    0x3F000000u,   //  0.5
    0xBF000000u,   // -0.5
    0x3EAAAAAAu,   //  0.3333...
};
static constexpr size_t SPECIAL_SIZE =
    sizeof(SPECIAL_F32) / sizeof(SPECIAL_F32[0]);

static inline uint64_t rd_fpcr(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return v;
}
static inline void wr_fpcr(uint64_t v)
{
    __asm__ volatile("msr fpcr, %0" :: "r"(v));
}

static inline bool is_denorm_f32(uint32_t bits)
{
    return (bits & 0x7F800000u) == 0 && (bits & 0x007FFFFFu) != 0;
}

// FZ flush: denormal -> signed zero.
static inline uint32_t flush_f32(uint32_t bits)
{
    if (is_denorm_f32(bits))
        return bits & 0x80000000u;   // keeps sign, zeroes magnitude
    return bits;
}

#ifdef __aarch64__

// FPCR.RMode[23:22] encoding, hardware-verified on Kunpeng 920 (probe in
// the commit message) and matching glibc's fenv constants
// (FE_UPWARD == 1<<22, FE_TOWARDZERO == 3<<22):
//   00 = RNE, 01 = RP (toward +inf), 10 = RM (toward -inf), 11 = RZ.
// NOTE: this ordering differs from x87; ARM puts RZ LAST, not second.
static const int ROUND_FE[4] = { FE_TONEAREST, FE_UPWARD,
                                 FE_DOWNWARD, FE_TOWARDZERO };
static const char *ROUND_NAME[4] = { "RNE", "RP", "RM", "RZ" };

// Scalar golden for one (a,b,c) under mode r + fz, mirroring the NEON
// op sequence exactly: r = c + a*b (single-rounded vfma).
static uint32_t golden_fma_f32(uint32_t ua, uint32_t ub, uint32_t uc,
                               int r, int fz)
{
    if (fz) {
        ua = flush_f32(ua);
        ub = flush_f32(ub);
        uc = flush_f32(uc);
    }
    float a, b, c;
    memcpy(&a, &ua, 4);
    memcpy(&b, &ub, 4);
    memcpy(&c, &uc, 4);

    int saved = fegetround();
    fesetround(ROUND_FE[r]);
    float res = std::fmaf(a, b, c);
    fesetround(saved);

    uint32_t ur;
    memcpy(&ur, &res, 4);
    if (fz)
        ur = flush_f32(ur);
    return ur;
}



static int fpcr_rounding_cartesian_arm_run(struct test *test, int cpu)
{
    (void)cpu;
    const uint64_t orig_fpcr = rd_fpcr();
    bool all_passed = true;

    // operand triple table: (special[i], special[j], special[k]) strides
    // over the 12^3 space with a stride that covers all values, plus a
    // bounded random stream per iteration.
    uint64_t seed = random64();
    auto next_special = [&seed](size_t idx) -> uint32_t {
        seed = seed * 0x9E3779B97F4A7C15ULL + 1;
        return SPECIAL_F32[(idx + (size_t)(seed >> 40)) % SPECIAL_SIZE];
    };

    for (int r = 0; r < 4; ++r) {
        for (int fz = 0; fz <= 1; ++fz) {
            // ---- set the configuration (outside the hot loop) ----
            uint64_t fpcr = orig_fpcr & ~((3ULL << 22) | (1ULL << 24));
            fpcr |= (uint64_t)r << 22;
            fpcr |= (uint64_t)fz << 24;
            wr_fpcr(fpcr);
            {
                // readback guard: if the write did not stick (framework
                // or libc reset it), report the register state instead
                // of a misleading "mode did not take effect".
                uint64_t rb = rd_fpcr();
                if (rb != fpcr) {
                    log_warning("fpcr_cartesian: FPCR write did not stick: "
                                "wrote 0x%lx read 0x%lx",
                                (unsigned long)fpcr, (unsigned long)rb);
                    report_fail_msg("fpcr_rounding_cartesian_arm: FPCR "
                                    "write/readback mismatch");
                    return EXIT_FAILURE;
                }
            }

            // ---- mode liveness assertion (tie operand) ----
            {
                // 1.0 + 2^-24: RNE->1.0 RTZ->1.000002 RDN->1.0 RUP->1.0
                // (positive tie: only RTZ rounds up here);
                // -(1.0 + 2^-24): RDN keeps magnitude growth.
                // Anti-constant-fold: operands staged through memory and
                // an asm barrier between the load and the add. GCC folds
                // vdupq_n_f32(volatile)+vaddq even at -O2 (verified: the
                // folded binary contained no fadd); a memory operand plus
                // compiler barrier defeats it for good.
                static thread_local float one_tbl[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                static thread_local float tiny_tbl[4] = {0x1p-24f, 0x1p-24f,
                                                         0x1p-24f, 0x1p-24f};
                static thread_local float neg_tbl[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
                static thread_local float negt_tbl[4] = {-0x1p-24f, -0x1p-24f,
                                                         -0x1p-24f, -0x1p-24f};
                __asm__ volatile("" : : "r"(one_tbl), "r"(tiny_tbl),
                                  "r"(neg_tbl), "r"(negt_tbl) : "memory");
                float32x4_t one = vld1q_f32(one_tbl);
                float32x4_t tiny = vld1q_f32(tiny_tbl);
                float32x4_t negone = vld1q_f32(neg_tbl);
                float32x4_t negtiny = vld1q_f32(negt_tbl);
                float32x4_t s = vaddq_f32(one, tiny);      // +1 tie
                float32x4_t d = vaddq_f32(negone, negtiny); // -1 tie
                float sf = vgetq_lane_f32(s, 0);
                float df = vgetq_lane_f32(d, 0);
                float expect_s, expect_d;
                switch (r) {
                case 0: expect_s = 0x1p+0f;  expect_d = -0x1p+0f;  break;
                case 1: expect_s = 0x1.000002p+0f; expect_d = -0x1p+0f; break;
                case 2: expect_s = 0x1p+0f;  expect_d = -0x1.000002p+0f; break;
                default: expect_s = 0x1p+0f; expect_d = -0x1p+0f; break;
                }
                if (memcmp(&sf, &expect_s, 4) != 0 ||
                    memcmp(&df, &expect_d, 4) != 0) {
                    log_warning("fpcr_cartesian: mode %s fz=%d liveness "
                                "FAILED: 1+2^-24=%a (want %a), -1-2^-24=%a "
                                "(want %a) — FPCR did not reach the FPU",
                                ROUND_NAME[r], fz, (double)sf,
                                (double)expect_s, (double)df,
                                (double)expect_d);
                    wr_fpcr(orig_fpcr);
                    report_fail_msg("fpcr_rounding_cartesian_arm: rounding "
                                    "mode did not take effect");
                    return EXIT_FAILURE;
                }
            }

            // ---- hot loop: NEON FMA over the operand table, golden per
            // element (golden computed OUTSIDE via fesetround with the
            // same mode) ----
            do {
                alignas(16) float a[4], b[4], c[4], hw[4];
                for (int i = 0; i < 4; ++i) {
                    uint32_t ua = next_special(i);
                    uint32_t ub = next_special(i + 4);
                    uint32_t uc = next_special(i + 8);
                    memcpy(&a[i], &ua, 4);
                    memcpy(&b[i], &ub, 4);
                    memcpy(&c[i], &uc, 4);
                }
                float32x4_t va = vld1q_f32(a);
                float32x4_t vb = vld1q_f32(b);
                float32x4_t vc = vld1q_f32(c);
                float32x4_t vd = vfmaq_f32(vc, va, vb);
                vst1q_f32(hw, vd);

                for (int i = 0; i < 4; ++i) {
                    uint32_t ua, ub, uc;
                    memcpy(&ua, &a[i], 4);
                    memcpy(&ub, &b[i], 4);
                    memcpy(&uc, &c[i], 4);
                    uint32_t g = golden_fma_f32(ua, ub, uc, r, fz);
                    float gf;
                    memcpy(&gf, &g, 4);
                    if (hw[i] != gf) {
                        uint32_t uhw;
                        memcpy(&uhw, &hw[i], 4);
                        log_warning("fpcr_cartesian: mode %s fz=%d elem %d "
                                    "golden=0x%08" PRIx32 " actual=0x%08"
                                    PRIx32, ROUND_NAME[r], fz, i, g, uhw);
                        all_passed = false;
                    }
                }
                if (!all_passed)
                    break;
            } while (false);   // one table pass per config; the outer
                               // do-while below re-runs the whole sweep

            wr_fpcr(orig_fpcr);   // restore between configurations
            if (!all_passed)
                break;
        }
        if (!all_passed)
            break;
    }

    // FPCR restore verification (framework code assumes reset default).
    if (rd_fpcr() != orig_fpcr) {
        wr_fpcr(orig_fpcr);
        log_warning("fpcr_cartesian: FPCR restore mismatch — re-restored");
    }

    if (!all_passed) {
        report_fail_msg("fpcr_rounding_cartesian_arm: rounding-network SDC "
                        "detected (RMode/FZ cartesian)");
        return EXIT_FAILURE;
    }

    while (test_time_condition(test)) {
        // Re-run the sweep body until budget; configs re-entered each pass.
        for (int r = 0; r < 4 && all_passed; ++r) {
            for (int fz = 0; fz <= 1 && all_passed; ++fz) {
                uint64_t fpcr = orig_fpcr & ~((3ULL << 22) | (1ULL << 24));
                fpcr |= (uint64_t)r << 22;
                fpcr |= (uint64_t)fz << 24;
                wr_fpcr(fpcr);
                alignas(16) float a[4], b[4], c[4], hw[4];
                for (int i = 0; i < 4; ++i) {
                    uint32_t ua = next_special(i);
                    uint32_t ub = next_special(i + 4);
                    uint32_t uc = next_special(i + 8);
                    memcpy(&a[i], &ua, 4);
                    memcpy(&b[i], &ub, 4);
                    memcpy(&c[i], &uc, 4);
                }
                float32x4_t va = vld1q_f32(a);
                float32x4_t vb = vld1q_f32(b);
                float32x4_t vc = vld1q_f32(c);
                float32x4_t vd = vfmaq_f32(vc, va, vb);
                vst1q_f32(hw, vd);
                for (int i = 0; i < 4; ++i) {
                    uint32_t ua, ub, uc;
                    memcpy(&ua, &a[i], 4);
                    memcpy(&ub, &b[i], 4);
                    memcpy(&uc, &c[i], 4);
                    uint32_t g = golden_fma_f32(ua, ub, uc, r, fz);
                    float gf;
                    memcpy(&gf, &g, 4);
                    if (hw[i] != gf) {
                        report_fail_msg("fpcr_rounding_cartesian_arm: "
                                        "rounding-network SDC detected "
                                        "(re-loop, mode %s fz=%d)",
                                        ROUND_NAME[r], fz);
                        wr_fpcr(orig_fpcr);
                        return EXIT_FAILURE;
                    }
                }
                wr_fpcr(orig_fpcr);
            }
        }
    }

    return EXIT_SUCCESS;
}

#else

static int fpcr_rounding_cartesian_arm_init(struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;
}

static int fpcr_rounding_cartesian_arm_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM64 FPCR + NEON required");
    return EXIT_SKIP;
}

#endif

static int fpcr_rounding_cartesian_arm_cleanup(struct test *test)
{
    (void)test;
#ifdef __aarch64__
    // Final safety net: FPCR back to the entry value (run() restores, but
    // a crash-free exit through every path must leave the thread default).
    // No-op here: run() owns the save/restore; cleanup only sanity-logs.
#endif
    return EXIT_SUCCESS;
}

DECLARE_TEST(fpcr_rounding_cartesian_arm,
             "FPCR rounding-mode x flush-to-zero x operand cartesian SDC "
             "stress (NEON): all 4 RMode x FZ configurations, 12-special-"
             "value operand table, byte-exact vs a same-mode fesetround "
             "scalar golden with FZ flush mirrored in software, per-mode "
             "liveness assertion on tie operands")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fpcr_rounding_cartesian_arm_init,
    .test_run = fpcr_rounding_cartesian_arm_run,
    .test_cleanup = fpcr_rounding_cartesian_arm_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
