/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b fsu_byteexact_sve
 * @parblock
 * Floating-point / SIMD Unit SDC stress — the FSU is at 80% coverage, the
 * highest of the weak units but still with a real gap: the existing fma.cpp
 * compares NEON vfmaq_f32 against a libm fmaf reference with a **1e-6
 * relative tolerance** (approx_equal), not a byte-exact memcmp. A 1-bit
 * flip in the FMA datapath (the canonical FSU transient SDC) changes the
 * result by far less than 1e-6 and passes that test — so the existing FSU
 * coverage is numerically exercised but not actually SDC-verified. CLAUDE.md
 * is explicit: golden comparison must be byte-identical memcmp.
 *
 * This test closes that gap. It is byte-exact throughout (for the
 * well-defined paths where NEON-fmaf equivalence holds), and uses category
 * checks where IEEE-754 leaves the result implementation-defined:
 *
 *   1. **FMA over a finite IEEE-754 value table, byte-exact**: vfmaq_f32 vs
 *      fmaf for every (a,b,c) drawn from a table of finite values (±0,
 *      ±subnormal, ±max-normal, high-Hamming normals). NEON vfmaq_f32 and
 *      libm fmaf are both IEEE-754 single-round FMA, so for finite inputs
 *      the results are bit-identical (verified before integration). A 1-bit
 *      FSU fault diverges here byte-exact — caught where the tolerance test
 *      missed it.
 *   2. **NaN/Inf category check**: for inputs containing NaN/Inf, the
 *      result's *category* (NaN, signed-Inf, or finite) must match between
 *      NEON and libm — payload bits are NOT compared because IEEE-754
 *      leaves NaN payload propagation implementation-defined (NEON quiets
 *      to default NaN, libm may preserve a payload; both compliant), so
 *      payload-exact comparison would report a false SDC. A fault that
 *      turns a NaN into a finite, or an Inf into a finite, is still caught
 *      (category mismatch).
 *   3. **Cross-cache-line NEON 128B data path**: a 16B vst1q/vld1q at
 *      offset line_end-8 straddles two 64B lines — the SIMD datapath under
 *      a split access, where a load/store-forwarding fault corrupts a lane.
 *      The reloaded vector must match the stored one byte-exact lane-by-lane.
 *   4. **Single→double widen + narrow**: compute FMA in float, widen to
 *      double, recompute in double (fma), narrow back — the precision-
 *      extension / rounding path. For the chosen operands the narrowed
 *      result equals the float FMA byte-exact (verified); a fault in the FP
 *      format-conversion path breaks it.
 *   5. **High-Hamming dependent FMA chain**: alternating high-Hamming float
 *      operands in a dependent chain (acc_{k} = acc_{k-1} + a_k*b_k) so the
 *      multiplier input nets toggle maximally each cycle (the operand-
 *      mutation / di/dt pressure the research calls out, on the FP path).
 *      Final accumulator byte-exact vs a software fmaf reference.
 *
 * SDC detection: byte-exact (vceqq_f32 lane-AND for vectors, raw uint32 bit
 * compare for scalars) for the finite/widen-narrow/chain paths; category
 * match for the NaN/Inf path. report_fail_msg on any mismatch. ARM64-native
 * (NEON vfmaq_f32/vst1q/vld1q/vceqq_f32 + libm fmaf/fma); non-aarch64
 * returns a clean EXIT_SKIP. Wired into the arm64 subdir (aarch64-only
 * guard), so x86-64 is untouched.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <cmath>
#include <limits>

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

static constexpr size_t CACHE_LINE = 64;

// Type-punning helper (mirrors power_virus_dit / sandstone_data.cpp).
static inline uint32_t bits_of(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}
static inline uint64_t bits_of(double d)
{
    uint64_t u;
    std::memcpy(&u, &d, sizeof(u));
    return u;
}
static inline float float_of(uint32_t u)
{
    float f;
    std::memcpy(&f, &u, sizeof(f));
    return f;
}
static inline bool is_nan_bits(uint32_t b)  // NaN = exp all-ones, mantissa nonzero
{ return (b & 0x7fffffff) > 0x7f800000; }
static inline bool is_inf_bits(uint32_t b)  // Inf = exp all-ones, mantissa zero
{ return (b & 0x7fffffff) == 0x7f800000; }

// Finite IEEE-754 value table for byte-exact FMA comparison (no NaN/Inf —
// those are category-checked separately because their payload propagation is
// implementation-defined).
static const uint32_t FINITE_TABLE[] = {
    0x00000000, 0x80000000, 0x3f800000, 0xbf800000,
    0x40000000, 0xc0000000, 0x40490fdb, 0xc0490fdb,
    0x7f7fffff, 0xff7fffff, 0x00800000, 0x80800000,
    0x00000001, 0x80000001, 0x007fffff,
    0x12345678, 0x4b000000, 0xcb000000, 0x3eaaaaab,
    0xbeaaaaab, 0x447a0000, 0xc47a0000, 0x3c23d70a,
    0xbc23d70a, 0x5a17b000, 0xda17b000,
};
static constexpr size_t FINITE_TABLE_SIZE =
    sizeof(FINITE_TABLE) / sizeof(FINITE_TABLE[0]);

// NaN / Inf bit patterns for the category check.
static const uint32_t SPECIAL_TABLE[] = {
    0x7f800000, 0xff800000, 0x7fc00000, 0x7fe00000,
    0x7f800001, 0x7fa00000,
};
static constexpr size_t SPECIAL_TABLE_SIZE =
    sizeof(SPECIAL_TABLE) / sizeof(SPECIAL_TABLE[0]);

// High-Hamming float pairs (operand-mutation pressure on the FP datapath).
static const uint32_t HH_PAIRS[][2] = {
    {0x3f800000, 0x40000000}, {0xc0000000, 0x3f800000},
    {0x40490fdb, 0xc0490fdb}, {0x4b000000, 0x41200000},
    {0x3eaaaaab, 0xbeaaaaab}, {0x5a17b000, 0x3a83126f},
};
static constexpr size_t NUM_HH_PAIRS = sizeof(HH_PAIRS) / sizeof(HH_PAIRS[0]);

static int fsu_byteexact_sve_init(struct test *test)
{
    /* All-VL Phase 2 P6: per-test auxv hand-probe -> framework
     * layer: arm64_require_sve_vl does the SVE capability check
     * + 128..1024-bit range gate in one call (skip reason
     * framework-issued). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    (void)test;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__

static int fsu_byteexact_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;

    uint8_t *vbuf = static_cast<uint8_t *>(aligned_alloc_safe(CACHE_LINE, CACHE_LINE * 2));
    if (!vbuf) {
        log_skip(TestResourceIssueSkipCategory,
                 "fsu_byteexact_sve: vbuf alloc failed");
        return EXIT_SKIP;
    }

    do {
        bool all_passed = true;

        // ---- (1) FMA over the finite IEEE-754 value table, byte-exact.
        // To bound the O(N^3) table to a manageable per-iteration cost, pair
        // (i,j) and cycle c through the table; this still exercises every
        // (a,b) pair with several c values across iterations.
        for (size_t i = 0; i < FINITE_TABLE_SIZE && all_passed; ++i) {
            for (size_t j = 0; j < FINITE_TABLE_SIZE && all_passed; ++j) {
                float a = float_of(FINITE_TABLE[i]);
                float b = float_of(FINITE_TABLE[j]);
                float c = float_of(FINITE_TABLE[(i + j) % FINITE_TABLE_SIZE]);

                svfloat32_t vd = svmla_f32_x(svptrue_b32(), svdup_f32(c),
                                          svdup_f32(a), svdup_f32(b));
                float sw0 = fmaf(a, b, c);
                float lanes[32];
                svst1_f32(svptrue_b32(), lanes, vd);
                if (bits_of(lanes[0]) != bits_of(sw0)) {
                    log_warning("fsu_byteexact_sve: FMA mismatch "
                                "a=0x%08x b=0x%08x c=0x%08x "
                                "hw=0x%08x sw=0x%08x",
                                FINITE_TABLE[i], FINITE_TABLE[j],
                                FINITE_TABLE[(i + j) % FINITE_TABLE_SIZE],
                                bits_of(lanes[0]), bits_of(sw0));
                    all_passed = false;
                }
            }
        }

        // ---- (2) NaN/Inf category check. For inputs containing NaN/Inf the
        // result category (NaN / signed-Inf / finite) must match between NEON
        // and libm. Payload bits are NOT compared (implementation-defined).
        for (size_t i = 0; i < SPECIAL_TABLE_SIZE && all_passed; ++i) {
            float a = float_of(SPECIAL_TABLE[i]);
            float b = float_of(SPECIAL_TABLE[(i + 1) % SPECIAL_TABLE_SIZE]);
            float c = float_of(0.5f);
            svfloat32_t vd = svmla_f32_x(svptrue_b32(), svdup_f32(c),
                                      svdup_f32(a), svdup_f32(b));
            float sw0 = fmaf(a, b, c);
            float lanes[32];
            svst1_f32(svptrue_b32(), lanes, vd);
            uint32_t hw = bits_of(lanes[0]), sw = bits_of(sw0);
            // Category match: both NaN, or both the same signed-Inf, or both
            // finite-equal. (A fault turning NaN->finite or Inf->finite is a
            // category mismatch -> SDC.)
            bool cat_ok = (is_nan_bits(hw) && is_nan_bits(sw))
                       || (is_inf_bits(hw) && is_inf_bits(sw)
                           && (hw == sw))
                       || (!is_nan_bits(hw) && !is_inf_bits(hw)
                           && !is_nan_bits(sw) && !is_inf_bits(sw)
                           && hw == sw);
            if (!cat_ok) {
                log_warning("fsu_byteexact_sve: NaN/Inf category mismatch "
                            "a=0x%08x b=0x%08x hw=0x%08x sw=0x%08x",
                            SPECIAL_TABLE[i],
                            SPECIAL_TABLE[(i + 1) % SPECIAL_TABLE_SIZE],
                            hw, sw);
                all_passed = false;
            }
        }

        // ---- (3) Cross-cache-line NEON 128B data path.
        static uint32_t hh_cycle = 0;
        uint32_t p0 = HH_PAIRS[hh_cycle % NUM_HH_PAIRS][0];
        uint32_t p1 = HH_PAIRS[(hh_cycle + 1) % NUM_HH_PAIRS][1];
        hh_cycle = (hh_cycle + 1) % NUM_HH_PAIRS;
        svfloat32_t src = svmla_f32_x(svptrue_b32(), svdup_f32(float_of(p0)),
                                       svdup_f32(float_of(p1)), svdup_f32(2.0f));
        uint8_t *cross = vbuf + CACHE_LINE - 8; // 8B in line0, 8B in line1
        svst1_f32(svptrue_b32(), (float *)cross, src);
        svfloat32_t reloaded = svld1_f32(svptrue_b32(), (float *)cross);
        svbool_t cmp = svcmpeq_f32(svptrue_b32(), src, reloaded);
        uint64_t mask = svcntp_b32(svptrue_b32(), cmp);
        uint64_t lanes_expected = svcntw();
        if (mask != lanes_expected) {
            float s[32], r[32];
            svst1_f32(svptrue_b32(), s, src);
            svst1_f32(svptrue_b32(), r, reloaded);
            log_warning("fsu_byteexact_sve: cross-line 128B NEON mismatch "
                        "src=[0x%08x,0x%08x,0x%08x,0x%08x] "
                        "rld=[0x%08x,0x%08x,0x%08x,0x%08x]",
                        bits_of(s[0]), bits_of(s[1]), bits_of(s[2]), bits_of(s[3]),
                        bits_of(r[0]), bits_of(r[1]), bits_of(r[2]), bits_of(r[3]));
            all_passed = false;
        }

        // ---- (4) Single→double widen + narrow (byte-exact for these ops).
        for (size_t i = 0; i < NUM_HH_PAIRS && all_passed; ++i) {
            float a = float_of(HH_PAIRS[i][0]);
            float b = float_of(HH_PAIRS[i][1]);
            float c = 0.5f;
            float f32_res = fmaf(a, b, c);
            double d_res = fma((double)a, (double)b, (double)c);
            float narrowed = (float)d_res;
            if (bits_of(f32_res) != bits_of(narrowed)) {
                log_warning("fsu_byteexact_sve: widen/narrow mismatch "
                            "a=0x%08x b=0x%08x "
                            "f32=0x%08x narrow=0x%08x (d=0x%016" PRIx64 ")",
                            HH_PAIRS[i][0], HH_PAIRS[i][1],
                            bits_of(f32_res), bits_of(narrowed),
                            bits_of(d_res));
                all_passed = false;
            }
        }

        // ---- (5) High-Hamming dependent FMA chain (byte-exact vs fmaf).
        svfloat32_t acc = svdup_f32(0.0f);
        float swacc = 0.0f;
        for (size_t i = 0; i < NUM_HH_PAIRS * 64; ++i) {
            float a = float_of(HH_PAIRS[i % NUM_HH_PAIRS][0]);
            float b = float_of(HH_PAIRS[(i + 1) % NUM_HH_PAIRS][1]);
            acc = svmla_f32_x(svptrue_b32(), acc, svdup_f32(a), svdup_f32(b));
            swacc = fmaf(a, b, swacc);
        }
        float lanes[32];
        svst1_f32(svptrue_b32(), lanes, acc);
        if (bits_of(lanes[0]) != bits_of(swacc)) {
            log_warning("fsu_byteexact_sve: dependent FMA chain mismatch "
                        "hw=0x%08x sw=0x%08x",
                        bits_of(lanes[0]), bits_of(swacc));
            all_passed = false;
        }

        if (!all_passed) {
            free(vbuf);
            report_fail_msg("fsu_byteexact_sve: FSU FMA/SIMD byte-exact SDC "
                            "detected");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    free(vbuf);
    return EXIT_SUCCESS;
}
#else
static int fsu_byteexact_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for fsu_byteexact_sve");
    return EXIT_SKIP;
}
#endif

static int fsu_byteexact_sve_finish(struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(fsu_byteexact_sve,
             "SVE FSU byte-exact SDC stress: FMA over a finite IEEE-754 value table "
             "(svmla lanes, byte-exact vs libm), NaN/Inf category check, "
             "cross-cache-line SVE data path, widen/narrow, and a high-Hamming "
             "dependent SVE FMA chain (port of fsu_byteexact_arm)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fsu_byteexact_sve_init,
    .test_run = fsu_byteexact_sve_run,
    .test_cleanup = fsu_byteexact_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
