/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b fpu_special_values_sve
 * @parblock
 * SVE port of fpu_special_values: full IEEE-754 special-value cartesian
 * product (12^3 per width) swept through SVE svmla_f32_x / svmla_f64_x.
 * Every SVE lane of the broadcast vector is checked byte-for-byte against
 * the libm single-rounding reference (fmaf/fma) — a per-lane SDC signature
 * in any lane is caught. Framework SNaN quieting applies uniformly so the
 * byte-exact compare stays sound. NaN results compare NaN-class-only:
 * IEEE-754 leaves NaN payload propagation implementation-defined (the
 * hardware svmla ORs input NaN payloads, libm fma forwards the first
 * operand's), so only NaN-ness is checked; all non-NaN results (incl.
 * -0.0 sign flips, the SDC signal) remain byte-exact.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cinttypes>
#include <limits>

#ifdef __aarch64__
#include <arm_sve.h>
#include <sys/auxv.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif
#endif

// Type-punning helper (mirrors the original).
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

// Number of special values in the sweep table (per type).
static constexpr int NUM_SPECIALS = 12;

static void build_specials_f32(float t[NUM_SPECIALS])
{
    t[0]  = NAN;
    t[1]  = __builtin_nansf("");
    t[2]  = INFINITY;
    t[3]  = -INFINITY;
    t[4]  = 0.0f;
    t[5]  = -0.0f;
    t[6]  =  std::numeric_limits<float>::denorm_min();
    t[7]  = -std::numeric_limits<float>::denorm_min();
    t[8]  =  std::numeric_limits<float>::max();
    t[9]  = -std::numeric_limits<float>::max();
    t[10] =  std::numeric_limits<float>::min();
    t[11] = -std::numeric_limits<float>::min();
}

static void build_specials_f64(double t[NUM_SPECIALS])
{
    t[0]  = NAN;
    t[1]  = __builtin_nans("");
    t[2]  = INFINITY;
    t[3]  = -INFINITY;
    t[4]  = 0.0;
    t[5]  = -0.0;
    t[6]  =  std::numeric_limits<double>::denorm_min();
    t[7]  = -std::numeric_limits<double>::denorm_min();
    t[8]  =  std::numeric_limits<double>::max();
    t[9]  = -std::numeric_limits<double>::max();
    t[10] =  std::numeric_limits<double>::min();
    t[11] = -std::numeric_limits<double>::min();
}

static int fpu_special_values_sve_init(struct test *test)
{
    (void)test;
#ifdef __aarch64__
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for fpu_special_values_sve");
        return EXIT_SKIP;
    }
#endif
    return EXIT_SUCCESS;
}

#ifdef __aarch64__

// Sweep the float32 FMA over the full special-value cartesian product on
// SVE lanes; every lane of the broadcast vector is byte-compared.
static int sweep_f32_sve(const float a_t[NUM_SPECIALS],
                         const float b_t[NUM_SPECIALS],
                         const float c_t[NUM_SPECIALS],
                         float &out_a, float &out_b, float &out_c,
                         float &out_hw, float &out_sw)
{
    int mismatches = 0;
    float hw_lane = 0, sw_lane = 0;
    const int lanes32 = svcntw();
    float hwv[32];   /* 最大支持 VL 1024bit (32 lanes) */

    for (int i = 0; i < NUM_SPECIALS; ++i) {
        for (int j = 0; j < NUM_SPECIALS; ++j) {
            for (int k = 0; k < NUM_SPECIALS; ++k) {
                float a = a_t[i], b = b_t[j], c = c_t[k];

                /* Hardware: svmla_f32_x (c + a*b, single rounding). Broadcast
                 * into a full SVE vector; ALL lanes checked (per-lane SDC). */
                svbool_t pg = svptrue_b32();
                svfloat32_t va = svdup_f32(a);
                svfloat32_t vb = svdup_f32(b);
                svfloat32_t vc = svdup_f32(c);
                svfloat32_t vd = svmla_f32_x(pg, vc, va, vb);
                svst1_f32(pg, hwv, vd);

                /* Software reference: fmaf (single rounding, IEEE-754). */
                sw_lane = fmaf(a, b, c);
                uint32_t sw_bits = bits_of(sw_lane);

                /* Per-lane compare: NaN class-only for NaN results,
                 * byte-exact otherwise (1-bit ULP flip is caught). */
                for (int lane = 0; lane < lanes32 && lane < 32; ++lane) {
                    hw_lane = hwv[lane];
                    bool hw_nan = std::isnan(hw_lane);
                    bool sw_nan = std::isnan(sw_lane);
                    /* NaN class-only: IEEE-754 leaves NaN payload
                     * propagation implementation-defined (hardware svmla
                     * ORs the input NaN payloads, libm fma forwards the
                     * first operand's) - only NaN-ness is checked when
                     * either side is NaN; all non-NaN results (incl. -0.0
                     * sign flips, the SDC signal) stay byte-exact. */
                    bool lane_ok = (hw_nan || sw_nan) ? (hw_nan && sw_nan)
                                                      : (bits_of(hw_lane) == sw_bits);
                    if (!lane_ok) {
                        if (mismatches == 0) {
                            out_a = a; out_b = b; out_c = c;
                            out_hw = hw_lane; out_sw = sw_lane;
                        }
                        ++mismatches;
                    }
                }
            }
        }
    }
    return mismatches;
}

// Sweep the float64 FMA similarly.
static int sweep_f64_sve(const double a_t[NUM_SPECIALS],
                         const double b_t[NUM_SPECIALS],
                         const double c_t[NUM_SPECIALS],
                         double &out_a, double &out_b, double &out_c,
                         double &out_hw, double &out_sw)
{
    int mismatches = 0;
    double hw_lane = 0, sw_lane = 0;
    const int lanes64 = svcntd();
    double hwv[16];   /* 最大支持 VL 1024bit (16 lanes) */

    for (int i = 0; i < NUM_SPECIALS; ++i) {
        for (int j = 0; j < NUM_SPECIALS; ++j) {
            for (int k = 0; k < NUM_SPECIALS; ++k) {
                double a = a_t[i], b = b_t[j], c = c_t[k];

                svbool_t pg = svptrue_b64();
                svfloat64_t va = svdup_f64(a);
                svfloat64_t vb = svdup_f64(b);
                svfloat64_t vc = svdup_f64(c);
                svfloat64_t vd = svmla_f64_x(pg, vc, va, vb);
                svst1_f64(pg, hwv, vd);

                sw_lane = fma(a, b, c);
                uint64_t sw_bits = bits_of(sw_lane);

                for (int lane = 0; lane < lanes64 && lane < 16; ++lane) {
                    hw_lane = hwv[lane];
                    bool hw_nan = std::isnan(hw_lane);
                    bool sw_nan = std::isnan(sw_lane);
                    /* NaN class-only: IEEE-754 leaves NaN payload
                     * propagation implementation-defined (hardware svmla
                     * ORs the input NaN payloads, libm fma forwards the
                     * first operand's) - only NaN-ness is checked when
                     * either side is NaN; all non-NaN results (incl. -0.0
                     * sign flips, the SDC signal) stay byte-exact. */
                    bool lane_ok = (hw_nan || sw_nan) ? (hw_nan && sw_nan)
                                                      : (bits_of(hw_lane) == sw_bits);
                    if (!lane_ok) {
                        if (mismatches == 0) {
                            out_a = a; out_b = b; out_c = c;
                            out_hw = hw_lane; out_sw = sw_lane;
                        }
                        ++mismatches;
                    }
                }
            }
        }
    }
    return mismatches;
}

static int fpu_special_values_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    float a32[NUM_SPECIALS], b32[NUM_SPECIALS], c32[NUM_SPECIALS];
    build_specials_f32(a32);
    build_specials_f32(b32);
    build_specials_f32(c32);

    double a64[NUM_SPECIALS], b64[NUM_SPECIALS], c64[NUM_SPECIALS];
    build_specials_f64(a64);
    build_specials_f64(b64);
    build_specials_f64(c64);

    do {
        float  fa = 0, fb = 0, fc = 0, fhw = 0, fsw = 0;
        double da = 0, db = 0, dc = 0, dhw = 0, dsw = 0;

        int mm32 = sweep_f32_sve(a32, b32, c32, fa, fb, fc, fhw, fsw);
        int mm64 = sweep_f64_sve(a64, b64, c64, da, db, dc, dhw, dsw);

        if (mm32 != 0 || mm64 != 0) {
            if (mm32 != 0) {
                log_warning("fpu_special_values_sve: f32 mismatch (count=%d): "
                            "fma(a,b,c) a=0x%08" PRIx32 " b=0x%08" PRIx32 " "
                            "c=0x%08" PRIx32 " hw=0x%08" PRIx32 " sw=0x%08" PRIx32,
                            mm32,
                            bits_of(fa), bits_of(fb), bits_of(fc),
                            bits_of(fhw), bits_of(fsw));
            }
            if (mm64 != 0) {
                log_warning("fpu_special_values_sve: f64 mismatch (count=%d): "
                            "fma(a,b,c) a=0x%016" PRIx64 " b=0x%016" PRIx64 " "
                            "c=0x%016" PRIx64 " hw=0x%016" PRIx64 " sw=0x%016" PRIx64,
                            mm64,
                            bits_of(da), bits_of(db), bits_of(dc),
                            bits_of(dhw), bits_of(dsw));
            }
            report_fail_msg("fpu_special_values_sve: special-value FMA mismatch "
                            "(f32 count=%d, f64 count=%d)", mm32, mm64);
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}
#else
static int fpu_special_values_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM SVE required for fpu_special_values_sve");
    return EXIT_SKIP;
}
#endif

static int fpu_special_values_sve_finish(struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(fpu_special_values_sve,
             "FPU special-value sweep on SVE svmla lanes (full IEEE-754 special cartesian product, per-lane byte-exact vs libm)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fpu_special_values_sve_init,
    .test_run = fpu_special_values_sve_run,
    .test_cleanup = fpu_special_values_sve_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
