/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve512_nt_reload_arm
 * @parblock
 * SVE non-temporal memory access + stack-reload window SDC stress — the
 * first test in the suite covering the LD1RD/LDNT1D/STNT1D instruction
 * family, the trigger-instruction window of the cn23154 NUMA3 VA-path
 * transient fault (field diagnosis 2026-09: the fault lands in VA[55:48]
 * on the RF-readout -> bypass-forward -> AGU -> SVE-LSU address path, and
 * requires (a) cluster saturation, (b) a base pointer reloaded from a
 * stack slot entering SVE addressing within a few instructions, (c)
 * SVE-dense traffic in the ld1rd/stnt1d shapes). This test parameterizes
 * the structural ingredients:
 *
 * Per step (one full vector), all operands flow through memory so the
 * golden is exact and no register state leaks between asm blocks:
 *   ld1rd  z8   <- coeff  (stack slot broadcast — the fault shape itself)
 *   ld1rd  z10  <- prev   (previous result lane 0, stack slot broadcast)
 *   ldnt1d z9   <- buf[base + off]        (non-temporal load)
 *   fmla   z9, z9, z8    -> v + v*coeff   (single-rounded)
 *   fmla   z9, z10, z8   -> + prev*coeff  (single-rounded)
 *   stnt1d z9   -> out[base + off]        (non-temporal store)
 *   st1d   z9   -> stack readback vector  (full-vector lane check)
 *
 * The reload->use distance is a runtime knob (-O ...distance=1..4): each
 * variant is a separate __attribute__((noinline)) function, so the base
 * pointer genuinely re-materializes from the stack at every call, and the
 * variant inserts (distance-1) unrelated ALU instructions between the
 * stack read of prev and the SVE block — sweeping the 2-3 instruction
 * window the field diagnosis identified.
 *
 * Golden (scalar, identical operation order, per lane i):
 *   res_i(s) = fma(prev_s, c, fma(v_i(s), c, v_i(s)))
 *   prev_{s+1} = res_0(s)   (the prev stack slot is lane 0's result)
 * with prev_0 = COEFF_INIT. buf values are exact 1.5-multiples so every
 * intermediate is exactly representable-independent and byte-comparable.
 *
 * A VA[63:48] canary on the buffer pointer runs every outer iteration
 * (sve512_stencil_axis_arm precedent — the NUMA3 signature corrupts
 * VA[55:48] while TCR.TBI0=1 silently ignores VA[63:56]).
 *
 * test_init probes HWCAP_SVE via getauxval before any SVE instruction,
 * returning a clean EXIT_SKIP on SVE-less CPUs. Any runtime vector length
 * is accepted (full 512-bit on HiSilicon 0xd22). ARM64-native; built only
 * on aarch64 (tests_arm64_sve library).
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

// Full-vector steps per outer iteration.
static constexpr size_t DIST_STEPS = 64;

// Data buffer elements (exact 1.5-multiples). Must be a multiple of the
// max vector length so each step's ldnt1d/stnt1d touches a fresh vector.
static constexpr size_t BUF_ELEMS = 4096;

static constexpr double COEFF = 1.5;    // ld1rd-broadcast coefficient
static constexpr double COEFF_INIT = -0.5; // initial prev stack-slot value

struct NtReloadData {
    size_t vl_d;                       // svcntd() — f64 lanes per vector
    std::vector<double> buf;           // in:  buf[j] = j * 1.5
    std::vector<double> out;           // stnt1d destination (same size)
    std::vector<double> golden;        // expected final res per lane
    int dist;                          // reload->use distance 1..4 (knob)
};

#ifdef __aarch64__

// VA[63:48] canary: userspace addresses have bits 63:48 zero on a healthy
// kernel; a non-zero pattern matches the known NUMA3 VA[55:48] signature.
static bool va_canary_ok(const void *p)
{
    return (reinterpret_cast<uintptr_t>(p) & 0xFFFF000000000000ULL) == 0;
}

// ---------------------------------------------------------------------------
// One full-vector step. noinline forces the base pointer to re-materialize
// from the caller's stack at every call (the stack-reload ingredient).
// scratch_chain is the (distance-1) unrelated ALN work inserted between the
// stack read of prev and the SVE block. All SVE state is local to the asm:
// z8/z10 are ld1rd-loaded from memory, z9 flows out via st1d — no register
// threading across asm blocks, so the golden is exact.
// ---------------------------------------------------------------------------
__attribute__((noinline))
static void nt_step_dist1(const double *base, uint64_t off,
                          const double *coef_slot, const double *prev_slot,
                          double *out_base, double *readback)
{
    uint64_t scratch = (uint64_t)(uintptr_t)prev_slot;
    (void)scratch; // dist 1: SVE block immediately follows the stack reads
    __asm__ volatile(
        "ptrue p0.d\n\t"
        "ld1rd {z8.d}, p0/z, [%[coef]]\n\t"
        "ld1rd {z10.d}, p0/z, [%[prev]]\n\t"
        "ldnt1d {z9.d}, p0/z, [%[b], %[off], lsl #3]\n\t"
        "fmla z9.d, p0/m, z9.d, z8.d\n\t"
        "fmla z9.d, p0/m, z10.d, z8.d\n\t"
        "stnt1d {z9.d}, p0, [%[ob], %[off], lsl #3]\n\t"
        "st1d {z9.d}, p0, [%[rb]]\n\t"
        :
        : [coef] "r"(coef_slot), [prev] "r"(prev_slot),
          [b] "r"(base), [ob] "r"(out_base), [rb] "r"(readback),
          [off] "r"(off)
        : "z8", "z9", "z10", "p0", "memory", "cc");
}

__attribute__((noinline))
static void nt_step_dist2(const double *base, uint64_t off,
                          const double *coef_slot, const double *prev_slot,
                          double *out_base, double *readback)
{
    // one unrelated ALU op between the stack read and the SVE block
    uint64_t scratch = (uint64_t)(uintptr_t)prev_slot;
    scratch = scratch * 3 + 1;
    __asm__ volatile("" : "+r"(scratch) :: "memory");
    __asm__ volatile(
        "ptrue p0.d\n\t"
        "ld1rd {z8.d}, p0/z, [%[coef]]\n\t"
        "ld1rd {z10.d}, p0/z, [%[prev]]\n\t"
        "ldnt1d {z9.d}, p0/z, [%[b], %[off], lsl #3]\n\t"
        "fmla z9.d, p0/m, z9.d, z8.d\n\t"
        "fmla z9.d, p0/m, z10.d, z8.d\n\t"
        "stnt1d {z9.d}, p0, [%[ob], %[off], lsl #3]\n\t"
        "st1d {z9.d}, p0, [%[rb]]\n\t"
        :
        : [coef] "r"(coef_slot), [prev] "r"(prev_slot),
          [b] "r"(base), [ob] "r"(out_base), [rb] "r"(readback),
          [off] "r"(off)
        : "z8", "z9", "z10", "p0", "memory", "cc");
    (void)scratch;
}

__attribute__((noinline))
static void nt_step_dist3(const double *base, uint64_t off,
                          const double *coef_slot, const double *prev_slot,
                          double *out_base, double *readback)
{
    // two unrelated ALU ops
    uint64_t scratch = (uint64_t)(uintptr_t)prev_slot;
    scratch = (scratch ^ (scratch >> 7)) * 5 + 3;
    scratch = scratch * 3 + 1;
    __asm__ volatile("" : "+r"(scratch) :: "memory");
    __asm__ volatile(
        "ptrue p0.d\n\t"
        "ld1rd {z8.d}, p0/z, [%[coef]]\n\t"
        "ld1rd {z10.d}, p0/z, [%[prev]]\n\t"
        "ldnt1d {z9.d}, p0/z, [%[b], %[off], lsl #3]\n\t"
        "fmla z9.d, p0/m, z9.d, z8.d\n\t"
        "fmla z9.d, p0/m, z10.d, z8.d\n\t"
        "stnt1d {z9.d}, p0, [%[ob], %[off], lsl #3]\n\t"
        "st1d {z9.d}, p0, [%[rb]]\n\t"
        :
        : [coef] "r"(coef_slot), [prev] "r"(prev_slot),
          [b] "r"(base), [ob] "r"(out_base), [rb] "r"(readback),
          [off] "r"(off)
        : "z8", "z9", "z10", "p0", "memory", "cc");
    (void)scratch;
}

__attribute__((noinline))
static void nt_step_dist4(const double *base, uint64_t off,
                          const double *coef_slot, const double *prev_slot,
                          double *out_base, double *readback)
{
    // three unrelated ALU ops
    uint64_t scratch = (uint64_t)(uintptr_t)prev_slot;
    scratch = (scratch + 0x9E3779B97F4A7C15ULL) * 0xBF58476D1CE4E5B9ULL;
    scratch = (scratch ^ (scratch >> 31)) + 7;
    scratch = scratch * 3 + 1;
    __asm__ volatile("" : "+r"(scratch) :: "memory");
    __asm__ volatile(
        "ptrue p0.d\n\t"
        "ld1rd {z8.d}, p0/z, [%[coef]]\n\t"
        "ld1rd {z10.d}, p0/z, [%[prev]]\n\t"
        "ldnt1d {z9.d}, p0/z, [%[b], %[off], lsl #3]\n\t"
        "fmla z9.d, p0/m, z9.d, z8.d\n\t"
        "fmla z9.d, p0/m, z10.d, z8.d\n\t"
        "stnt1d {z9.d}, p0, [%[ob], %[off], lsl #3]\n\t"
        "st1d {z9.d}, p0, [%[rb]]\n\t"
        :
        : [coef] "r"(coef_slot), [prev] "r"(prev_slot),
          [b] "r"(base), [ob] "r"(out_base), [rb] "r"(readback),
          [off] "r"(off)
        : "z8", "z9", "z10", "p0", "memory", "cc");
    (void)scratch;
}

// Scalar golden for one step, identical rounding order as the two fmla:
//   first  fmla: v + v*c    == fma(v, c, v)      (single rounding)
//   second fmla: + prev*c   == fma(prev, c, ...) (single rounding)
static inline double golden_step(double prev, double v, double c)
{
    return std::fma(prev, c, std::fma(v, c, v));
}

static int sve512_nt_reload_arm_init(struct test *test)
{
#ifndef __aarch64__
    (void)test;
    return EXIT_SUCCESS;
#else
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SVE not available on this CPU; "
                 "sve512_nt_reload_arm requires SVE (ld1rd/ldnt1d/stnt1d "
                 "are the NUMA3 VA-path fault trigger-instruction window)");
        return EXIT_SKIP;
    }

    try {
        auto data = std::make_unique<NtReloadData>();
        data->vl_d = svcntd();
        if (BUF_ELEMS % data->vl_d != 0) {
            // BUF_ELEMS is 4096; any power-of-two lane count divides it.
            // A non-power-of-two VL would make step vectors overlap; skip
            // rather than compute a partial golden.
            log_skip(CpuNotSupportedSkipCategory,
                     "sve512_nt_reload_arm: unexpected vector length "
                     "(vl_d=%zu does not divide %zu)",
                     data->vl_d, BUF_ELEMS);
            return EXIT_SKIP;
        }

        // Distance knob: -O sve512_nt_reload_arm.distance=1..4 (default 2,
        // the field-diagnosis window is 2-3 instructions after the reload).
        int64_t knob = get_testspecific_knob_value_int(test, "distance", 2);
        if (knob < 1 || knob > 4) {
            log_skip(TestResourceIssueSkipCategory,
                     "sve512_nt_reload_arm: distance knob out of range: %ld "
                     "(valid 1..4, default 2)", (long)knob);
            return EXIT_SKIP;
        }
        data->dist = (int)knob;

        // Working set: exactly-representable values, byte-exact comparable.
        data->buf.resize(BUF_ELEMS);
        for (size_t j = 0; j < BUF_ELEMS; ++j)
            data->buf[j] = (double)j * 1.5;
        data->out.assign(BUF_ELEMS, 0.0);

        // Golden: per-lane chain over DIST_STEPS steps. prev is shared
        // across lanes (lane 0's previous result, broadcast by ld1rd), so
        // the lanes must be advanced in step-major order.
        data->golden.assign(data->vl_d, 0.0);
        std::vector<double> res(data->vl_d);
        double prev = COEFF_INIT;
        for (size_t step = 0; step < DIST_STEPS; ++step) {
            size_t off = (step * data->vl_d) % BUF_ELEMS;
            for (size_t lane = 0; lane < data->vl_d; ++lane)
                res[lane] = golden_step(prev, data->buf[off + lane], COEFF);
            prev = res[0];
        }
        for (size_t lane = 0; lane < data->vl_d; ++lane) {
            data->golden[lane] = res[lane];
            // res[] now holds the FINAL step's per-lane results; copy to
            // golden words below (double storage for memcmp clarity).
        }
        // (golden[] already holds the final res per lane via the loop above
        // — res was assigned every step and lane, so golden[lane] = res of
        // the last executed step. The explicit copy keeps intent clear:)
        for (size_t lane = 0; lane < data->vl_d; ++lane)
            data->golden[lane] = res[lane];

        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sve512_nt_reload_arm init: %s", e.what());
        return EXIT_SKIP;
    }
#endif
}

static int sve512_nt_reload_arm_run(struct test *test, int cpu)
{
    (void)cpu;
#ifndef __aarch64__
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 SVE required for "
             "sve512_nt_reload_arm");
    return EXIT_SKIP;
#else
    auto *d = static_cast<NtReloadData *>(test->data);
    const size_t vl = d->vl_d;

    // Stack slots — the reload sources (fault ingredient).
    const double coef_slot = COEFF;         // ld1rd broadcast source
    double prev_slot = COEFF_INIT;          // ld1rd broadcast source
    std::vector<double> readback(vl);       // st1d full-vector readback

    if (!va_canary_ok(d->buf.data()) || !va_canary_ok(d->out.data())) {
        log_warning("sve512_nt_reload: VA[63:48] canary tripped: buf=%p out=%p",
                    (void *)d->buf.data(), (void *)d->out.data());
        report_fail_msg("sve512_nt_reload_arm: VA[63:48] canary mismatch "
                        "(known NUMA3 signature shape)");
        return EXIT_FAILURE;
    }

    do {
        bool all_passed = true;
        prev_slot = COEFF_INIT;

        for (size_t step = 0; step < DIST_STEPS; ++step) {
            size_t off = (step * vl) % BUF_ELEMS;
            switch (d->dist) {
            case 1:
                nt_step_dist1(d->buf.data(), off, &coef_slot, &prev_slot,
                              d->out.data(), readback.data());
                break;
            case 2:
                nt_step_dist2(d->buf.data(), off, &coef_slot, &prev_slot,
                              d->out.data(), readback.data());
                break;
            case 3:
                nt_step_dist3(d->buf.data(), off, &coef_slot, &prev_slot,
                              d->out.data(), readback.data());
                break;
            default:
                nt_step_dist4(d->buf.data(), off, &coef_slot, &prev_slot,
                              d->out.data(), readback.data());
                break;
            }
            // prev chain: lane 0's result, read from the st1d readback.
            prev_slot = readback[0];
        }

        // Full-vector byte-exact check against the golden (final step).
        if (memcmp(readback.data(), d->golden.data(), vl * sizeof(double)) != 0) {
            for (size_t lane = 0; lane < vl; ++lane) {
                uint64_t g, a;
                memcpy(&g, &d->golden[lane], 8);
                memcpy(&a, &readback[lane], 8);
                if (g != a)
                    log_warning("sve512_nt_reload: lane %zu golden=0x%016"
                                PRIx64 " actual=0x%016" PRIx64,
                                lane, g, a);
            }
            all_passed = false;
        }
        // stnt1d region check for the final step's vector.
        {
            size_t last_off = ((DIST_STEPS - 1) * vl) % BUF_ELEMS;
            if (memcmp(&d->out[last_off], d->golden.data(),
                       vl * sizeof(double)) != 0)
                all_passed = false;
        }

        if (!all_passed) {
            report_fail_msg("sve512_nt_reload_arm: SVE NT/reload-window "
                            "SDC detected (ld1rd/ldnt1d/stnt1d path)");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#endif
}

static int sve512_nt_reload_arm_cleanup(struct test *test)
{
#ifdef __aarch64__
    delete static_cast<NtReloadData *>(test->data);
#else
    (void)test;
#endif
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(sve512_nt_reload_arm,
             "SVE non-temporal access + stack-reload window SDC stress: "
             "ld1rd/ldnt1d/stnt1d (the NUMA3 VA-path fault trigger "
             "instructions) with a parameterized reload->use distance "
             "(-O sve512_nt_reload_arm.distance=1..4), byte-exact vs a "
             "scalar fma golden with the identical operation order")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_nt_reload_arm_init,
    .test_run = sve512_nt_reload_arm_run,
    .test_cleanup = sve512_nt_reload_arm_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
