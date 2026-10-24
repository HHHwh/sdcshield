/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b power_virus_dit_sve_arm
 * @parblock
 * Full-width SVE di/dt voltage-transient power virus — the SVE port of
 * power_virus_dit (NEON128). At a 512-bit vector length the same burst
 * drives 4x the datapath width: larger transient current swings and 4x
 * the per-cycle bit-toggle density in the bypass network — the physical
 * mechanism behind SEVI (ASPLOS'26) Obs.4 (wider vectors carry
 * additional low-frequency SDC cases: more transistors active + 2x power
 * causing voltage-margin excursions).
 *
 * Structure per cycle (power_virus_dit skeleton, widened):
 *   BURST: a dependent svmla_f64_x chain at full runtime VL, operands
 *          alternating between two high-Hamming-distance f64 patterns
 *          every step (max multiplier-input toggling). Every 64 steps
 *          the burst performs an svld1_f64 load with the base pointer
 *          re-read from a stack slot — address generation inside the
 *          burst current window (the NUMA3 fault's address-path x
 *          burst compound). No time check inside the burst.
 *   STALL: yield-hint loop draining the vector units (unchanged from
 *          the NEON version).
 *
 * SDC detection: each burst's final accumulator is compared byte-exact
 * against a scalar fma reference repeating the identical dependent
 * sequence (the load contribution is folded into the chain: every 64th
 * step's multiplier operand comes from the loaded vector, mirrored in
 * the golden in the same order).
 *
 * Cross-core barrier: NOT used — the framework has no barrier helper
 * (known gap), so cores burst on statistically-random phases; cluster
 * saturation comes from the framework's one-thread-per-CPU execution
 * itself (design tradeoff recorded here; lockstep would be an
 * enhancement, not a requirement — the NEON version ships the same
 * way).
 *
 * test_init gates via the framework layer (arm64_require_sve_vl:
 * HWCAP_SVE capability + 128..1024-bit range in one call, all-VL
 * Phase 2 P6) and returns a clean EXIT_SKIP on SVE-less CPUs. The run
 * side reads the VL fresh (svcntd) and derives the golden from the
 * same lanes, so no init/run VL state exists to drift. Built in
 * tests_arm64_sve.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <cmath>
#include <vector>

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

// Burst/stall sizes (matched duty cycle, power_virus_dit precedent).
static constexpr int BURST_INNER = 2048;
static constexpr int STALL_INNER = 2048;

// Load-in-burst period: base pointer re-read from a stack slot and used
// for an SVE load every LOAD_PERIOD steps of the burst.
static constexpr int LOAD_PERIOD = 64;

#ifdef __aarch64__

// High-Hamming alternating operand pairs (|x| <= 2, exact).
static constexpr double A0 = 1.3333333333333333;   // 0x3FF5555555555555
static constexpr double A1 = -1.6666666666666667;  // 0xBFFAAAAAAAAAAAAA
static constexpr double B0 = 0.3333333333333333;   // 0x3FD5555555555555
static constexpr double B1 = -0.8333333333333333;  // 0xBFEAAAAAAAAAAAAA

static int power_virus_dit_sve_arm_init(struct test *test)
{
    /* All-VL Phase 2 P6: capability + 128..1024-bit range check in one
       framework-layer call, replacing the getauxval hand-probe. Gate
       only: the run side re-reads svcntd() and sizes its buffers and
       golden from the same lanes (no init/run VL state to drift). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
    (void)svl;
    return EXIT_SUCCESS;
}

static inline void stall_yield(int n)
{
    for (int i = 0; i < n; ++i)
        __asm__ volatile("yield" ::: "memory");
}

static int power_virus_dit_sve_arm_run(struct test *test, int cpu)
{
    (void)cpu;
    const size_t vl_d = svcntd();
    const svbool_t pg = svptrue_b64();

    // Load-in-burst operands (address-generation x burst compound).
    std::vector<double> loadbuf(vl_d * 4);
    for (size_t i = 0; i < loadbuf.size(); ++i)
        loadbuf[i] = (i & 1) ? B0 : B1;
    // Stack slot holding the base pointer — re-read every LOAD_PERIOD.
    const double *base_slot = loadbuf.data();
    size_t load_idx = 0;

    // Scalar golden: full-lane re-computation (the loaded vector is NOT
    // lane-uniform — even/odd lanes differ — so every lane gets its own
    // chain; svmla(pg, op1, op2, op3) = op1 + op2*op3 with per-step (m,a)
    // alternating and every LOAD_PERIOD-th step taking m from the
    // loadbuf value the hardware loaded at that lane).
    std::vector<double> golden(vl_d);
    {
        size_t li = 0;
        for (size_t lane = 0; lane < vl_d; ++lane) {
            double acc = 0.0;
            for (int k = 0; k < BURST_INNER; ++k) {
                double m = (k & 1) ? A1 : A0;
                double a = (k & 1) ? B1 : B0;
                if (k % LOAD_PERIOD == 0) {
                    m = loadbuf[(li * vl_d + lane) % loadbuf.size()];
                }
                acc = std::fma(m, a, acc);
            }
            golden[lane] = acc;
        }
        (void)li;
    }

    do {
        bool all_passed = true;
        for (int cycle = 0; cycle < 8; ++cycle) {
            // ---- BURST: dependent full-width SVE FMA chain with
            // alternating operands and periodic stack-reload loads ----
            load_idx = 0;
            svfloat64_t acc = svdup_f64(0.0);
            svfloat64_t a0v = svdup_f64(A0);
            svfloat64_t a1v = svdup_f64(A1);
            svfloat64_t b0v = svdup_f64(B0);
            svfloat64_t b1v = svdup_f64(B1);
            for (int k = 0; k < BURST_INNER; ++k) {
                if (k % LOAD_PERIOD == 0) {
                    // base pointer re-read from the stack slot — the
                    // address-generation op inside the burst window
                    const double *b = base_slot;
                    size_t off = (load_idx * vl_d) % loadbuf.size();
                    svfloat64_t lv = svld1_f64(pg, b + off);
                    acc = svmla_f64_x(pg, acc, lv, (k & 1) ? b1v : b0v);
                    ++load_idx;
                } else if (k & 1) {
                    acc = svmla_f64_x(pg, acc, a1v, b1v);
                } else {
                    acc = svmla_f64_x(pg, acc, a0v, b0v);
                }
            }
            std::vector<double> hw(vl_d);
            svst1_f64(pg, hw.data(), acc);
            for (size_t lane = 0; lane < vl_d; ++lane) {
                if (hw[lane] != golden[lane]) {
                    uint64_t g, a;
                    memcpy(&g, &golden[lane], 8);
                    memcpy(&a, &hw[lane], 8);
                    log_warning("power_virus_dit_sve: burst %d lane %zu "
                                "golden=0x%016" PRIx64 " actual=0x%016"
                                PRIx64, cycle, lane, g, a);
                    all_passed = false;
                }
            }
            if (!all_passed)
                break;

            // ---- STALL ----
            stall_yield(STALL_INNER);
        }

        if (!all_passed) {
            report_fail_msg("power_virus_dit_sve_arm: di/dt transient SDC "
                            "detected (SVE FMA corrupted mid burst/stall)");
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

#else

static int power_virus_dit_sve_arm_init(struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;
}
static int power_virus_dit_sve_arm_run(struct test *test, int cpu)
{
    (void)test;
    (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for the "
             "SVE di/dt power virus");
    return EXIT_SKIP;
}

#endif

static int power_virus_dit_sve_arm_cleanup(struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(power_virus_dit_sve_arm,
             "Full-width SVE di/dt voltage-transient power virus: dependent "
             "svmla_f64_x burst chains with alternating high-Hamming "
             "operands and periodic stack-reload SVE loads (address "
             "generation inside the burst window), alternating with yield "
             "stalls — the full-width SVE port of power_virus_dit, "
             "byte-exact vs a per-lane scalar fma golden")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = power_virus_dit_sve_arm_init,
    .test_run = power_virus_dit_sve_arm_run,
    .test_cleanup = power_virus_dit_sve_arm_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
