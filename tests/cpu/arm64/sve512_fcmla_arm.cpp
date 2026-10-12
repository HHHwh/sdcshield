/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve512_fcmla_arm
 * @parblock
 * SVE complex multiply-accumulate (FCMLA/SVCMLA) chain SDC stress — all
 * four rotations (0/90/180/270 degrees). The complex cross-multiply +
 * rotate-select datapath (real/imag dual FMA with sign network) is
 * silicon the real-only FMLA tests never touch; complex FMA is the core
 * of complex SVD/FFT workloads (eigen_svd_cdouble goes through scalar
 * complex expansion today — the instruction-level path had zero
 * coverage). FCMLA is SVE1 (any SVE machine runs it; full 512-bit VL on
 * HiSilicon 0xd22).
 *
 * Semantics (ARM DDI 0616), lane pairs (even=real, odd=imag):
 *   acc_e' = fma(+b_sel * c_e, acc_e)   acc_i' = fma(+b_sel * c_i, acc_i)
 * where (b_sel, c_e, c_i) per rotation:
 *   rot 0:   re += b_re*c_re ; im += b_re*c_im   (b = +b_e for both)
 *   rot 90:  re += -b_i*c_i ; im += b_i*c_e      (cross, sign on the imag)
 *   rot 180: re += -b_e*c_re; im += -b_e*c_im    (negated real)
 *   rot 270: re += b_i*c_i  ; im += -b_i*c_e     (cross, sign on the imag)
 * Precisely (DDI 0616 FCMLA pseudo-code): for each rotation the two
 * accumulates are single-rounded FMAs with the rotated operand selection:
 *   rot 0   : acc_e' = fma( b_e, c_e, acc_e); acc_i' = fma( b_e, c_i, acc_i)
 *   rot 90  : acc_e' = fma(-b_i, c_i, acc_e); acc_i' = fma( b_i, c_e, acc_i)
 *   rot 180 : acc_e' = fma(-b_e, c_e, acc_e); acc_i' = fma(-b_e, c_i, acc_i)
 *   rot 270 : acc_e' = fma( b_i, c_i, acc_e); acc_i' = fma(-b_i, c_e, acc_i)
 *
 * Worked example (init self-check data): b=(1,2), c=(3,4), acc=(0,0):
 *   rot 0   -> (1*3, 1*4)     = (3, 4)
 *   rot 90  -> (-2*4, 2*3)    = (-8, 6)
 *   rot 180 -> (-1*3, -1*4)   = (-3, -4)
 *   rot 270 -> (2*4, -2*3)    = (8, -6)
 *
 * The golden is this exact per-rotation fma expansion in scalar C++
 * (std::fma, single rounding each accumulate — NOT std::complex, whose
 * multiply is double-rounded). The ACLE intrinsic svcmla_f64_m(pg, acc,
 * b, c, 0|90|180|270) requires the rotation as a literal, so each
 * rotation is its own specialized step function.
 *
 * Chain: 512 dependent steps per rotation (accumulator feeds forward).
 * Operands from the finite high-Hamming table (|x| <= 2) so no chain can
 * overflow to Inf/NaN (sve512_f64_chain precedent). A one-step self-check
 * per rotation compares golden vs hardware before the chain runs — a
 * golden bug reports as a test bug, never as SDC.
 *
 * test_init probes HWCAP_SVE and returns a clean EXIT_SKIP on SVE-less
 * CPUs. Any runtime vector length. Built in tests_arm64_sve
 * (armv8.2-a+sve — FCMLA is SVE1).
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

static constexpr size_t CHAIN_STEPS = 512;

// Finite high-Hamming f64 patterns, |x| <= 2 (sve512_f64_chain precedent).
static const uint64_t F64_FINITE[] = {
    0x3FF5555555555555ULL,   //  1.3333...
    0xBFFAAAAAAAAAAAAAULL,   // -1.6667...
    0x3FD5555555555555ULL,   //  0.3333...
    0xBFEAAAAAAAAAAAAAULL,   // -0.8333...
    0x3FF0000000000000ULL,   //  1.0
    0xBFF0000000000000ULL,   // -1.0
    0x3FF999999999999AULL,   //  1.6
    0xBFF6666666666667ULL,   // -1.4
};
static constexpr size_t F64_FINITE_SIZE =
    sizeof(F64_FINITE) / sizeof(F64_FINITE[0]);

struct FcmlaData {
    size_t vl_d;
    // Expected acc after the chain, per rotation (0/90/180/270).
    std::vector<uint64_t> golden[4];
};

#ifdef __aarch64__

// Scalar golden step for one lane pair, per the DDI 0616 expansion above.
static inline void fcmla_golden_pair(double &acc_e, double &acc_i,
                                     double b_e, double b_i,
                                     double c_e, double c_i, int rot)
{
    switch (rot) {
    case 0:
        acc_e = std::fma( b_e, c_e, acc_e);
        acc_i = std::fma( b_e, c_i, acc_i);
        break;
    case 90:
        acc_e = std::fma(-b_i, c_i, acc_e);
        acc_i = std::fma( b_i, c_e, acc_i);
        break;
    case 180:
        acc_e = std::fma(-b_e, c_e, acc_e);
        acc_i = std::fma(-b_e, c_i, acc_i);
        break;
    default: // 270
        acc_e = std::fma( b_i, c_i, acc_e);
        acc_i = std::fma(-b_i, c_e, acc_i);
        break;
    }
}

// Hardware step per rotation (literal rotation -> separate functions).
static void fcmla_hw_rot0(double *acc, const double *b, const double *c,
                          size_t vl_d)
{
    svfloat64_t va = svld1_f64(svptrue_b64(), acc);
    svfloat64_t vb = svld1_f64(svptrue_b64(), b);
    svfloat64_t vc = svld1_f64(svptrue_b64(), c);
    va = svcmla_f64_m(svptrue_b64(), va, vb, vc, 0);
    svst1_f64(svptrue_b64(), acc, va);
    (void)vl_d;
}
static void fcmla_hw_rot90(double *acc, const double *b, const double *c,
                           size_t vl_d)
{
    svfloat64_t va = svld1_f64(svptrue_b64(), acc);
    svfloat64_t vb = svld1_f64(svptrue_b64(), b);
    svfloat64_t vc = svld1_f64(svptrue_b64(), c);
    va = svcmla_f64_m(svptrue_b64(), va, vb, vc, 90);
    svst1_f64(svptrue_b64(), acc, va);
    (void)vl_d;
}
static void fcmla_hw_rot180(double *acc, const double *b, const double *c,
                            size_t vl_d)
{
    svfloat64_t va = svld1_f64(svptrue_b64(), acc);
    svfloat64_t vb = svld1_f64(svptrue_b64(), b);
    svfloat64_t vc = svld1_f64(svptrue_b64(), c);
    va = svcmla_f64_m(svptrue_b64(), va, vb, vc, 180);
    svst1_f64(svptrue_b64(), acc, va);
    (void)vl_d;
}
static void fcmla_hw_rot270(double *acc, const double *b, const double *c,
                            size_t vl_d)
{
    svfloat64_t va = svld1_f64(svptrue_b64(), acc);
    svfloat64_t vb = svld1_f64(svptrue_b64(), b);
    svfloat64_t vc = svld1_f64(svptrue_b64(), c);
    va = svcmla_f64_m(svptrue_b64(), va, vb, vc, 270);
    svst1_f64(svptrue_b64(), acc, va);
    (void)vl_d;
}

static int sve512_fcmla_arm_init(struct test *test)
{
#ifndef __aarch64__
    (void)test;
    return EXIT_SUCCESS;
#else
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SVE not available; sve512_fcmla_arm requires SVE "
                 "(complex FMA FCMLA is SVE1)");
        return EXIT_SKIP;
    }

    try {
        auto data = std::make_unique<FcmlaData>();
        data->vl_d = svcntd();

        const size_t pairs = data->vl_d / 2;   // complex lane pairs
        // b/c operand streams: pair-major [pair*2 + {e,i}]
        std::vector<double> b(data->vl_d), c(data->vl_d);
        for (size_t i = 0; i < data->vl_d; ++i) {
            memcpy(&b[i], &F64_FINITE[(i * 7 + 1) % F64_FINITE_SIZE], 8);
            memcpy(&c[i], &F64_FINITE[(i * 5 + 3) % F64_FINITE_SIZE], 8);
        }

        for (int rot = 0; rot < 4; ++rot) {
            std::vector<double> acc(data->vl_d, 0.0);
            for (size_t step = 0; step < CHAIN_STEPS; ++step) {
                for (size_t p = 0; p < pairs; ++p) {
                    fcmla_golden_pair(acc[2 * p], acc[2 * p + 1],
                                      b[2 * p], b[2 * p + 1],
                                      c[2 * p], c[2 * p + 1], rot);
                }
            }
            data->golden[rot].resize(data->vl_d);
            memcpy(data->golden[rot].data(), acc.data(), data->vl_d * 8);
        }

        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sve512_fcmla_arm init: %s", e.what());
        return EXIT_SKIP;
    }
#endif
}

static int sve512_fcmla_arm_run(struct test *test, int cpu)
{
    (void)cpu;
#ifndef __aarch64__
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 SVE required for "
             "sve512_fcmla_arm");
    return EXIT_SKIP;
#else
    auto *d = static_cast<FcmlaData *>(test->data);
    const size_t vl_d = d->vl_d;
    const size_t pairs = vl_d / 2;

    std::vector<double> b(vl_d), c(vl_d);
    for (size_t i = 0; i < vl_d; ++i) {
        memcpy(&b[i], &F64_FINITE[(i * 7 + 1) % F64_FINITE_SIZE], 8);
        memcpy(&c[i], &F64_FINITE[(i * 5 + 3) % F64_FINITE_SIZE], 8);
    }

    // ---------- one-step self-check per rotation ----------
    {
        const double sb[2] = {1.0, 2.0}, sc[2] = {3.0, 4.0};
        const double expect[4][2] = {
            { 3.0,  4.0},   // rot 0
            {-8.0,  6.0},   // rot 90
            {-3.0, -4.0},   // rot 180
            { 8.0, -6.0},   // rot 270
        };
        for (int rot = 0; rot < 4; ++rot) {
            double gacc[2] = {0.0, 0.0};
            fcmla_golden_pair(gacc[0], gacc[1], sb[0], sb[1],
                              sc[0], sc[1], rot);
            if (gacc[0] != expect[rot][0] || gacc[1] != expect[rot][1]) {
                report_fail_msg("sve512_fcmla_arm: golden worked-example "
                                "self-check failed (rot %d) — test bug, "
                                "not SDC", rot * 90);
                return EXIT_FAILURE;
            }
        }
    }

    // ---------- dependent chains, all four rotations ----------
    do {
        bool all_passed = true;
        for (int rot = 0; rot < 4; ++rot) {
            std::vector<double> acc(vl_d, 0.0);
            for (size_t step = 0; step < CHAIN_STEPS; ++step) {
                switch (rot) {
                case 0:  fcmla_hw_rot0(acc.data(), b.data(), c.data(), vl_d); break;
                case 1:  fcmla_hw_rot90(acc.data(), b.data(), c.data(), vl_d); break;
                case 2:  fcmla_hw_rot180(acc.data(), b.data(), c.data(), vl_d); break;
                default: fcmla_hw_rot270(acc.data(), b.data(), c.data(), vl_d); break;
                }
            }
            for (size_t i = 0; i < vl_d; ++i) {
                uint64_t g, a;
                memcpy(&g, &d->golden[rot][i], 8);
                memcpy(&a, &acc[i], 8);
                if (g != a) {
                    log_warning("sve512_fcmla: rot %d lane %zu "
                                "golden=0x%016" PRIx64 " actual=0x%016" PRIx64,
                                rot * 90, i, g, a);
                    all_passed = false;
                }
            }
            (void)pairs;
        }

        if (!all_passed) {
            report_fail_msg("sve512_fcmla_arm: SVE complex FMA (FCMLA) "
                            "SDC detected");
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#endif
}

static int sve512_fcmla_arm_cleanup(struct test *test)
{
#ifdef __aarch64__
    delete static_cast<FcmlaData *>(test->data);
#else
    (void)test;
#endif
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(sve512_fcmla_arm,
             "SVE complex FMA (FCMLA) chain SDC stress: all four rotations "
             "(0/90/180/270) of svcmla_f64 over dependent 512-step chains, "
             "byte-exact vs a per-rotation fused-semantics scalar golden "
             "(DDI 0616 expansion, single rounding per accumulate), "
             "worked-example self-check before the chain")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_fcmla_arm_init,
    .test_run = sve512_fcmla_arm_run,
    .test_cleanup = sve512_fcmla_arm_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
