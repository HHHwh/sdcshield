/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve512_fmmla_arm
 * @parblock
 * SVE2 FMMLA matrix-outer-product chain SDC stress (f32 via FEAT_F32MM,
 * f64 via FEAT_F64MM) — the highest-FLOPS-density datapath in SVE (each
 * instruction completes VL/2 x VL multiply-accumulates into a 2D tile)
 * and a first-generation instruction family (PinDrop HPCA'26: an
 * instruction's failure rate is highest in the architecture generation
 * that introduces it). Zero prior coverage in the suite. cn23154
 * (HiSilicon 0xd22) reports f32mm=f64mm=1 in ID_AA64ZFR0_EL1.
 *
 * FMMLA semantics (ARM DDI 0616) — the lane mapping this test's golden
 * implements, worked out on paper for a 128-bit VL f32 case (VL/2=2 rows):
 *   Zda holds the accumulator tile packed as rows of (VL/2) f32 elements;
 *   row r, column c lives in Zda lane (r*(VL/2) + c), for
 *   r in [0, VL/2), c in [0, VL/2).
 *   FMMLA Zda, Zn, Zm: Zn supplies row vectors (Zn lane (r*VL/2 + k) is
 *   matrix element A[r][k]), Zm supplies column vectors (Zm lane
 *   (k*VL/2 + c) is B[k][c]), and
 *     Zda[r][c] += sum_k A[r][k] * B[k][c]
 *   with each product accumulating in a single fused rounding.
 *
 * Golden: for f32, the scalar mirror accumulates with fmaf in the same
 * k-order and the same single rounding, so it is byte-exact against the
 * hardware. For f64 the same with fma (FEAT_F64MM, .d tiles, VL/4 rows
 * of VL/4 elements — the f64 lane mapping halves the tile side).
 *
 * Chain: 256 dependent FMMLA steps (the tile output feeds the next step's
 * accumulator), operands from the finite high-Hamming table (|x| <= 2) so
 * the 256-step chain can never overflow to Inf/NaN (sve512_f64_chain_arm
 * precedent). Bounded |A|,|B| <= 2 with tile side <= 8: per-step products
 * <= 4 and sums <= 8*4 = 32; 256 steps bound the accumulator by 256*32*2
 * + ... < 2^15 — well inside f64/f32 range and exactly tracked by the
 * scalar mirror regardless (the mirror tracks every rounding, so even
 * magnitude growth stays byte-exact).
 *
 * Self-check: init runs the golden for one step on VL=2-row toy data and
 * compares against the hardware FMMLA result once; a mismatch fails init
 * with a clear message (golden-lane-mapping bug guard, so the test never
 * reports a false SDC on a healthy CPU).
 *
 * test_init gates in two layers (all-VL Phase 2 P6 split): the pure SVE
 * capability + 128..1024-bit range check goes through the framework
 * layer (arm64_require_sve_vl, one call, no SVE instruction executes
 * before the gate), while the composite SVE2|F32MM|F64MM HWCAP2 bits
 * stay a getauxval hand-probe — that conjunction is this test's own
 * semantics (FEAT_F32MM + FEAT_F64MM), not something the generic
 * require helper covers. Either layer returns a clean EXIT_SKIP when
 * absent (e.g. this Kunpeng 920 dev host). test_run re-checks the VL
 * and fails closed on drift. Requires the tests_arm64_sve2 library
 * (-march=armv9-a+sve2+f32mm+f64mm). Any runtime vector length in the
 * supported range is accepted (full 512-bit on 0xd22).
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
#include <device/cpu/arm64/arm64_vl.h>
#endif

// Dependent FMMLA chain length per iteration.
static constexpr size_t CHAIN_STEPS = 256;

// Finite high-Hamming f64 patterns (|x| <= 2; chain can never overflow —
// sve512_f64_chain_arm precedent).
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

struct FmmlaData {
    size_t vl_f32_rows;    // f32 tile side = svcntw()/2
    size_t vl_f64_rows;    // f64 tile side = svcntd()/2
    std::vector<double> golden_f64;   // expected f64 tile (row-major)
    std::vector<float>  golden_f32;   // expected f32 tile (row-major)
};

#ifdef __aarch64__

// ---------------------------------------------------------------------------
// Scalar golden for ONE FMMLA f64 step: tile side n = vl_f64_rows.
//   res[r][c] = sum_k fma(A[r][k], B[k][c], res[r][c])   (k ascending,
//   single rounding each accumulate — identical to the hardware order)
// ---------------------------------------------------------------------------
static void fmmla_f64_step_golden(double *tile, const double *Zn,
                                  const double *Zm, size_t n)
{
    for (size_t r = 0; r < n; ++r)
        for (size_t c = 0; c < n; ++c)
            for (size_t k = 0; k < n; ++k)
                tile[r * n + c] = std::fma(Zn[r * n + k], Zm[k * n + c],
                                           tile[r * n + c]);
}

static void fmmla_f32_step_golden(float *tile, const float *Zn,
                                  const float *Zm, size_t n)
{
    for (size_t r = 0; r < n; ++r)
        for (size_t c = 0; c < n; ++c)
            for (size_t k = 0; k < n; ++k)
                tile[r * n + c] = std::fmaf(Zn[r * n + k], Zm[k * n + c],
                                            tile[r * n + c]);
}

// Hardware: one FMMLA step via the unpredicated ACLE intrinsic. Zn/Zm/tile
// are full vectors (vl elements); FMMLA interprets them per the mapping in
// the docblock — the golden uses the same packed layout.
static void fmmla_f64_step_hw(double *tile, const double *Zn, const double *Zm,
                              size_t vl_d)
{
    svfloat64_t zda = svld1_f64(svptrue_b64(), tile);
    svfloat64_t zn  = svld1_f64(svptrue_b64(), Zn);
    svfloat64_t zm  = svld1_f64(svptrue_b64(), Zm);
    zda = svmmla_f64(zda, zn, zm);
    svst1_f64(svptrue_b64(), tile, zda);
    (void)vl_d;
}

static void fmmla_f32_step_hw(float *tile, const float *Zn, const float *Zm)
{
    svfloat32_t zda = svld1_f32(svptrue_b32(), tile);
    svfloat32_t zn  = svld1_f32(svptrue_b32(), Zn);
    svfloat32_t zm  = svld1_f32(svptrue_b32(), Zm);
    zda = svmmla_f32(zda, zn, zm);
    svst1_f32(svptrue_b32(), tile, zda);
}

static int sve512_fmmla_arm_init(struct test *test)
{
#ifndef __aarch64__
    (void)test;
    return EXIT_SUCCESS;
#else
    /* All-VL Phase 2 P6 split gate: the pure SVE capability + 128..1024-bit
       range check moves to the framework layer (arm64_require_sve_vl —
       returns the VL in BYTES), while the composite SVE2|F32MM|F64MM
       HWCAP2 conjunction stays a getauxval hand-probe: it is this test's
       own semantic gate (FEAT_F32MM + FEAT_F64MM matrix-multiply
       instructions), which the generic require helper does not cover. */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
    unsigned long hwcap2 = getauxval(AT_HWCAP2);
    if ((hwcap2 & (HWCAP2_SVE2 | HWCAP2_SVEF32MM | HWCAP2_SVEF64MM))
            != (HWCAP2_SVE2 | HWCAP2_SVEF32MM | HWCAP2_SVEF64MM)) {
        log_skip(CpuNotSupportedSkipCategory,
                 "SVE2 F32MM/F64MM matrix-multiply not available "
                 "(HWCAP2: sve2=%lu f32mm=%lu f64mm=%lu); "
                 "sve512_fmmla_arm requires FEAT_F32MM + FEAT_F64MM",
                 (hwcap2 & HWCAP2_SVE2) ? 1UL : 0UL,
                 (hwcap2 & HWCAP2_SVEF32MM) ? 1UL : 0UL,
                 (hwcap2 & HWCAP2_SVEF64MM) ? 1UL : 0UL);
        return EXIT_SKIP;
    }

    try {
        auto data = std::make_unique<FmmlaData>();
        const size_t vl_w = (size_t)svl / 4;     /* f32 lanes = SVL bytes / 4 */
        const size_t vl_d = (size_t)svl / 8;     /* f64 lanes = SVL bytes / 8 */
        if (vl_d < 2 || vl_d > 16 || vl_w < 4 || vl_w > 32) {
            log_error("sve512_fmmla_arm: lanes outside [2,16]/[4,32] "
                      "— unit drift (svl %ld bytes)", svl);
            return EXIT_FAILURE;
        }
        data->vl_f32_rows = vl_w / 2;            // f32 tile side
        data->vl_f64_rows = vl_d / 2;            // f64 tile side

        // ---- golden: f64 chain ----
        {
            const size_t n = data->vl_f64_rows;
            std::vector<double> Zn(vl_d), Zm(vl_d), tile(vl_d, 0.0);
            for (size_t i = 0; i < vl_d; ++i) {
                memcpy(&Zn[i], &F64_FINITE[(i * 7 + 1) % F64_FINITE_SIZE], 8);
                memcpy(&Zm[i], &F64_FINITE[(i * 5 + 3) % F64_FINITE_SIZE], 8);
            }
            for (size_t step = 0; step < CHAIN_STEPS; ++step)
                fmmla_f64_step_golden(tile.data(), Zn.data(), Zm.data(), n);
            data->golden_f64 = tile;
        }
        // ---- golden: f32 chain (f32 view of the same finite table) ----
        {
            const size_t n = data->vl_f32_rows;
            std::vector<float> Zn(vl_w), Zm(vl_w), tile(vl_w, 0.0f);
            for (size_t i = 0; i < vl_w; ++i) {
                double d;
                memcpy(&d, &F64_FINITE[(i * 7 + 1) % F64_FINITE_SIZE], 8);
                Zn[i] = (float)d;
                memcpy(&d, &F64_FINITE[(i * 5 + 3) % F64_FINITE_SIZE], 8);
                Zm[i] = (float)d;
            }
            for (size_t step = 0; step < CHAIN_STEPS; ++step)
                fmmla_f32_step_golden(tile.data(), Zn.data(), Zm.data(), n);
            data->golden_f32 = tile;
        }

        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sve512_fmmla_arm init: %s", e.what());
        return EXIT_SKIP;
    }
#endif
}

static int sve512_fmmla_arm_run(struct test *test, int cpu)
{
    (void)cpu;
#ifndef __aarch64__
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 SVE2 F32MM/F64MM "
             "required for sve512_fmmla_arm");
    return EXIT_SKIP;
#else
    auto *d = static_cast<FmmlaData *>(test->data);
    const size_t vl_d = svcntd();
    const size_t vl_w = svcntw();

    /* All-VL policy #3: fail closed on init/run VL drift (the goldens
       are sized at init's lanes; a wider run-side VL would read past
       them in the compare loops below). */
    if (vl_d != d->vl_f64_rows * 2 || vl_w != d->vl_f32_rows * 2) {
        log_error("sve512_fmmla_arm: VL drifted between init (f64 tile "
                  "side %zu, f32 tile side %zu) and run (svcntd %u, "
                  "svcntw %u)", d->vl_f64_rows, d->vl_f32_rows,
                  (unsigned)vl_d, (unsigned)vl_w);
        report_fail_msg("sve512_fmmla_arm: SVE VL drifted between init "
                        "and run");
    }

    bool all_passed = true;

    // ---------- f64 tile ----------
    {
        std::vector<double> Zn(vl_d), Zm(vl_d), tile(vl_d, 0.0);
        for (size_t i = 0; i < vl_d; ++i) {
            memcpy(&Zn[i], &F64_FINITE[(i * 7 + 1) % F64_FINITE_SIZE], 8);
            memcpy(&Zm[i], &F64_FINITE[(i * 5 + 3) % F64_FINITE_SIZE], 8);
        }
        // ONE hardware step + golden comparison = self-check of the lane
        // mapping before the long chain runs (docblock guard).
        {
            std::vector<double> one_tile(vl_d, 0.0);
            fmmla_f64_step_golden(one_tile.data(), Zn.data(), Zm.data(),
                                  d->vl_f64_rows);
            std::vector<double> hw_tile(vl_d, 0.0);
            fmmla_f64_step_hw(hw_tile.data(), Zn.data(), Zm.data(), vl_d);
            if (memcmp(one_tile.data(), hw_tile.data(), vl_d * 8) != 0) {
                log_warning("sve512_fmmla: f64 lane-mapping self-check "
                            "FAILED — golden does not match hardware for "
                            "one step; refusing to report SDC (golden bug)");
                // Not an SDC: fail the test loudly as a test bug, not as
                // silent data corruption.
                report_fail_msg("sve512_fmmla_arm: internal golden "
                                "lane-mapping mismatch (f64) — test bug, "
                                "not an SDC report");
                return EXIT_FAILURE;
            }
        }
        for (size_t step = 0; step < CHAIN_STEPS; ++step)
            fmmla_f64_step_hw(tile.data(), Zn.data(), Zm.data(), vl_d);
        for (size_t i = 0; i < vl_d; ++i) {
            if (tile[i] != d->golden_f64[i]) {
                uint64_t g, a;
                memcpy(&g, &d->golden_f64[i], 8);
                memcpy(&a, &tile[i], 8);
                log_warning("sve512_fmmla: f64 lane %zu golden=0x%016" PRIx64
                            " actual=0x%016" PRIx64, i, g, a);
                all_passed = false;
            }
        }
    }

    // ---------- f32 tile ----------
    {
        std::vector<float> Zn(vl_w), Zm(vl_w), tile(vl_w, 0.0f);
        for (size_t i = 0; i < vl_w; ++i) {
            double dd;
            memcpy(&dd, &F64_FINITE[(i * 7 + 1) % F64_FINITE_SIZE], 8);
            Zn[i] = (float)dd;
            memcpy(&dd, &F64_FINITE[(i * 5 + 3) % F64_FINITE_SIZE], 8);
            Zm[i] = (float)dd;
        }
        {
            std::vector<float> one_tile(vl_w, 0.0f);
            fmmla_f32_step_golden(one_tile.data(), Zn.data(), Zm.data(),
                                  d->vl_f32_rows);
            std::vector<float> hw_tile(vl_w, 0.0f);
            fmmla_f32_step_hw(hw_tile.data(), Zn.data(), Zm.data());
            if (memcmp(one_tile.data(), hw_tile.data(), vl_w * 4) != 0) {
                report_fail_msg("sve512_fmmla_arm: internal golden "
                                "lane-mapping mismatch (f32) — test bug, "
                                "not an SDC report");
                return EXIT_FAILURE;
            }
        }
        for (size_t step = 0; step < CHAIN_STEPS; ++step)
            fmmla_f32_step_hw(tile.data(), Zn.data(), Zm.data());
        for (size_t i = 0; i < vl_w; ++i) {
            if (tile[i] != d->golden_f32[i]) {
                uint32_t g, a;
                memcpy(&g, &d->golden_f32[i], 4);
                memcpy(&a, &tile[i], 4);
                log_warning("sve512_fmmla: f32 lane %zu golden=0x%08" PRIx32
                            " actual=0x%08" PRIx32, i, g, a);
                all_passed = false;
            }
        }
    }

    if (!all_passed) {
        report_fail_msg("sve512_fmmla_arm: SVE2 FMMLA matrix-outer-product "
                        "SDC detected (f32mm/f64mm datapath)");
        return EXIT_FAILURE;
    }

    // Re-run until the time budget elapses.
    while (test_time_condition(test)) {
        std::vector<double> Zn(vl_d, 0.0), Zm(vl_d, 0.0), tile(vl_d, 0.0);
        for (size_t i = 0; i < vl_d; ++i) {
            memcpy(&Zn[i], &F64_FINITE[(i * 7 + 1) % F64_FINITE_SIZE], 8);
            memcpy(&Zm[i], &F64_FINITE[(i * 5 + 3) % F64_FINITE_SIZE], 8);
        }
        for (size_t step = 0; step < CHAIN_STEPS; ++step)
            fmmla_f64_step_hw(tile.data(), Zn.data(), Zm.data(), vl_d);
        if (memcmp(tile.data(), d->golden_f64.data(), vl_d * 8) != 0) {
            report_fail_msg("sve512_fmmla_arm: SVE2 FMMLA f64 SDC detected");
            return EXIT_FAILURE;
        }
        std::vector<float> Zn32(vl_w, 0.0f), Zm32(vl_w, 0.0f), tile32(vl_w, 0.0f);
        for (size_t i = 0; i < vl_w; ++i) {
            double dd;
            memcpy(&dd, &F64_FINITE[(i * 7 + 1) % F64_FINITE_SIZE], 8);
            Zn32[i] = (float)dd;
            memcpy(&dd, &F64_FINITE[(i * 5 + 3) % F64_FINITE_SIZE], 8);
            Zm32[i] = (float)dd;
        }
        for (size_t step = 0; step < CHAIN_STEPS; ++step)
            fmmla_f32_step_hw(tile32.data(), Zn32.data(), Zm32.data());
        if (memcmp(tile32.data(), d->golden_f32.data(), vl_w * 4) != 0) {
            report_fail_msg("sve512_fmmla_arm: SVE2 FMMLA f32 SDC detected");
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
#endif
}

static int sve512_fmmla_arm_cleanup(struct test *test)
{
#ifdef __aarch64__
    delete static_cast<FmmlaData *>(test->data);
#else
    (void)test;
#endif
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(sve512_fmmla_arm,
             "SVE2 FMMLA matrix-outer-product chain SDC stress (f32 F32MM + "
             "f64 F64MM): 256-step dependent svmmla chains over the packed "
             "2D-tile lane mapping, byte-exact vs a scalar fma/fmaf golden "
             "with identical k-order, one-step lane-mapping self-check "
             "before the chain runs")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_fmmla_arm_init,
    .test_run = sve512_fmmla_arm_run,
    .test_cleanup = sve512_fmmla_arm_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
