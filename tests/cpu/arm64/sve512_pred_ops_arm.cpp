/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve512_pred_ops_arm
 * @parblock
 * SVE predicate-dimension SDC stress — the predicate machinery every
 * other SVE test leaves at full-true (all 11 existing SVE tests use
 * svptrue exclusively). The per-lane predicate network (whilelt
 * generation, partial activity, predicate inversion, FFR) is new silicon
 * SVE introduced relative to NEON, i.e. first-generation risk (PinDrop
 * HPCA'26), and a predicate bit corruption is silent by construction:
 * the wrong lanes compute or fail to compute, and the wrong result is
 * still a bit-legal value — exactly an SDC shape.
 *
 * Four sections, each with an exact golden:
 *
 * 1. Predicate-density sweep: for active-lane counts k = 1..VL, whilelt
 *    masks the first k lanes; an svmla_f64_m under that predicate must
 *    update exactly lanes [0, k) and leave lanes [k, VL) untouched. The
 *    active lanes carry a finite addend (so the computed result has a
 *    deterministic golden), the untouched lanes a sentinel (0xdead...);
 *    the test verifies BOTH the computed lanes (byte-exact golden) and
 *    the untouched lanes (sentinel preserved) — a predicate that fails to
 *    mask, or masks the wrong lanes, fails either half.
 *
 * 2. Predicate inversion: svnot_b_z of a whilelt predicate drives a
 *    second FMA whose active set is the complement; golden computed per
 *    lane by the same conditional rule. Together with section 1 this
 *    exercises predicate NOT + both polarity paths.
 *
 * 3. FADDA chain: the architecturally-sequential predicate-driven
 *    horizontal add (svadda_f64) over 256 steps — a serial dependency
 *    through the reduction adder, golden in the same lane order
 *    (FADDA is defined left-to-right, so the scalar mirror is exact).
 *
 * 4. LDFF1 + FFR: first-fault load from valid mapped memory must leave
 *    FFR all-true; svrdffr() is compared against the operating predicate
 *    bit-for-bit. FFR is primed with svsetffr() first — an LDFF1 only
 *    clears the bit of a faulting element, so the FFR is only meaningful
 *    against an all-ones starting state. (A faulting LDFF1 is not
 *    exercised — that is kernel territory; the mapped case checks the FFR
 *    datapath itself.) FMAXV tree reduction (order-independent, exactly
 *    checkable) over the whilelt-masked prefix.
 *
 * test_init gates via the framework layer (arm64_require_sve_vl:
 * HWCAP_SVE capability + 128..1024-bit range in one call, all-VL
 * Phase 2 P6) and returns a clean EXIT_SKIP on SVE-less CPUs; test_run
 * re-checks the VL and fails closed on drift. Built in tests_arm64_sve.
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
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

static constexpr size_t FADDA_STEPS = 256;

// Sentinel for untouched-lane detection (a quiet NaN payload; any
// untouched lane keeps these exact bits).
static constexpr uint64_t SENTINEL = 0x7FF8DEADBEEF0000ULL;

struct PredOpsData {
    size_t vl_d;
    std::vector<double> src;            // exact operands: src[j] = (j%4)-1.5
};

#ifdef __aarch64__

static int sve512_pred_ops_arm_init(struct test *test)
{
#ifndef __aarch64__
    (void)test;
    return EXIT_SUCCESS;
#else
    /* All-VL Phase 2 P6: capability + 128..1024-bit range check in one
       framework-layer call, replacing the getauxval hand-probe.
       arm64_require_sve_vl returns the VL in BYTES — f64 lanes =
       svl/8, range-checked below (release-effective, NDEBUG-proof). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    try {
        auto data = std::make_unique<PredOpsData>();
        data->vl_d = (size_t)svl / 8;   /* f64 lanes = SVL bytes / 8 */
        if (data->vl_d < 2 || data->vl_d > 16) {
            log_error("sve512_pred_ops_arm: f64 lanes %zu outside "
                      "[2,16] — unit drift (svl %ld bytes)",
                      data->vl_d, svl);
            return EXIT_FAILURE;
        }
        data->src.resize(data->vl_d);
        for (size_t j = 0; j < data->vl_d; ++j)
            data->src[j] = (double)(j % 4) - 1.5;   // exact values
        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sve512_pred_ops_arm init: %s", e.what());
        return EXIT_SKIP;
    }
#endif
}

static int sve512_pred_ops_arm_run(struct test *test, int cpu)
{
    (void)cpu;
#ifndef __aarch64__
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 SVE required for "
             "sve512_pred_ops_arm");
    return EXIT_SKIP;
#else
    auto *d = static_cast<PredOpsData *>(test->data);

    /* All-VL policy #3: fail closed on init/run VL drift (src and the
       density-sweep buffers are sized in init at its VL). */
    if (svcntd() != d->vl_d) {
        log_error("sve512_pred_ops_arm: VL drifted between init (%zu "
                  "lanes) and run (%u lanes)", d->vl_d,
                  (unsigned)svcntd());
        report_fail_msg("sve512_pred_ops_arm: SVE VL drifted between "
                        "init and run");
    }

    const size_t vl = d->vl_d;
    const svbool_t all = svptrue_b64();
    bool all_passed = true;

    // Finite FMA addend for the active lanes: a distinctive, exactly
    // representable value so the computed lanes have a deterministic
    // golden. The masked-out lanes keep the NaN SENTINEL, which the
    // merging (_m) form must preserve bit-for-bit.
    const double BASE = 1.5;
    uint64_t base_bits;
    memcpy(&base_bits, &BASE, 8);

    // ---------------- Section 1+2: density sweep + inversion ----------------
    for (size_t k = 1; k <= vl; ++k) {
        // ---- section 1: whilelt(k) partial FMA ----
        svbool_t pg = svwhilelt_b64_u64((uint64_t)0, (uint64_t)k);
        // dst: active lanes carry BASE (the addend), the rest the SENTINEL.
        std::vector<uint64_t> dst(vl, SENTINEL);
        for (size_t i = 0; i < k; ++i)
            dst[i] = base_bits;
        svfloat64_t vdst = svld1_f64(all, reinterpret_cast<const double *>(dst.data()));
        svfloat64_t vsrc = svld1_f64(all, d->src.data());
        // ACLE _m: op1 + op2*op3, merging — acc = fma(src, src, BASE) on
        // the active lanes; inactive lanes retain the SENTINEL.
        vdst = svmla_f64_m(pg, vdst, vsrc, vsrc);
        svst1_f64(all, reinterpret_cast<double *>(dst.data()), vdst);
        for (size_t i = 0; i < vl; ++i) {
            if (i < k) {
                // golden: fma(src_i, src_i, BASE)
                double g = std::fma(d->src[i], d->src[i], BASE);
                uint64_t gw, aw;
                memcpy(&gw, &g, 8);
                memcpy(&aw, &dst[i], 8);
                if (gw != aw) {
                    log_warning("sve512_pred_ops: density %zu active lane "
                                "%zu golden=0x%016" PRIx64 " actual=0x%016"
                                PRIx64, k, i, gw, aw);
                    all_passed = false;
                }
            } else if (dst[i] != SENTINEL) {
                log_warning("sve512_pred_ops: density %zu INACTIVE lane "
                            "%zu sentinel clobbered: 0x%016" PRIx64,
                            k, i, dst[i]);
                all_passed = false;
            }
        }

        // ---- section 2: inverted predicate ----
        svbool_t pg_inv = svnot_b_z(all, pg);
        // active set is the complement [k, vl); inactive (i < k) = SENTINEL.
        std::vector<uint64_t> dst2(vl, SENTINEL);
        for (size_t i = k; i < vl; ++i)
            dst2[i] = base_bits;
        svfloat64_t w = svld1_f64(all, reinterpret_cast<const double *>(dst2.data()));
        w = svmla_f64_m(pg_inv, w, vsrc, vsrc);
        svst1_f64(all, reinterpret_cast<double *>(dst2.data()), w);
        for (size_t i = 0; i < vl; ++i) {
            if (i >= k) {   // complement: active iff NOT (i < k)
                double g = std::fma(d->src[i], d->src[i], BASE);
                uint64_t gw, aw;
                memcpy(&gw, &g, 8);
                memcpy(&aw, &dst2[i], 8);
                if (gw != aw) {
                    log_warning("sve512_pred_ops: inverted density %zu lane "
                                "%zu golden=0x%016" PRIx64 " actual=0x%016"
                                PRIx64, k, i, gw, aw);
                    all_passed = false;
                }
            } else if (dst2[i] != SENTINEL) {
                log_warning("sve512_pred_ops: inverted density %zu inactive "
                            "lane %zu written: 0x%016" PRIx64,
                            k, i, dst2[i]);
                all_passed = false;
            }
        }
    }

    // ---------------- Section 3: FADDA chain ----------------
    {
        double acc = 0.5;   // exact initial scalar
        svfloat64_t vsrc = svld1_f64(all, d->src.data());
        double hw = acc;
        for (size_t step = 0; step < FADDA_STEPS; ++step) {
            hw = svadda_f64(all, hw, vsrc);
        }
        // golden: FADDA adds active lanes left-to-right onto the scalar;
        // mirror exactly.
        double g = acc;
        for (size_t step = 0; step < FADDA_STEPS; ++step) {
            for (size_t i = 0; i < vl; ++i)
                g = g + d->src[i];   // values are exact halves: every sum
                                     // exact in f64, order fully defined
        }
        uint64_t gw, aw;
        memcpy(&gw, &g, 8);
        memcpy(&aw, &hw, 8);
        if (gw != aw) {
            log_warning("sve512_pred_ops: FADDA chain golden=0x%016" PRIx64
                        " actual=0x%016" PRIx64, gw, aw);
            all_passed = false;
        }
    }

    // ---------------- Section 4: LDFF1 + FFR + FMAXV ----------------
    {
        // valid mapped memory -> first-fault must not fault, FFR all-true
        std::vector<double> mem(d->vl_d);
        for (size_t i = 0; i < d->vl_d; ++i)
            mem[i] = d->src[i];
        // Prime FFR to all-ones before the first-fault load. LDFF1 only
        // *clears* the FFR bit of each faulting element — it never sets a
        // bit — so without SETFFR the readback below is the previous
        // (undefined) FFR state, not the set of successfully-loaded lanes.
        svsetffr();
        svfloat64_t v = svldff1_f64(all, mem.data());
        std::vector<double> back(d->vl_d);
        svst1_f64(all, back.data(), v);
        if (memcmp(back.data(), mem.data(), d->vl_d * 8) != 0) {
            log_warning("sve512_pred_ops: LDFF1 readback mismatch");
            all_passed = false;
        }
        // FFR must equal the governing predicate (all-true here)
        svbool_t ffr = svrdffr();
        if (svcntp_b64(all, ffr) != (long)vl) {
            log_warning("sve512_pred_ops: FFR not all-true after mapped "
                        "LDFF1 (count=%ld, expected %zu)",
                        svcntp_b64(all, ffr), vl);
            all_passed = false;
        }
        // FMAXV over the whilelt(k=vl/2) masked prefix: exact max
        {
            size_t k = vl / 2;
            svbool_t pg = svwhilelt_b64_u64(0, k);
            svfloat64_t vs = svld1_f64(all, d->src.data());
            double hw_max = svmaxv_f64(pg, vs);
            double g_max = -INFINITY;
            for (size_t i = 0; i < k; ++i)
                if (d->src[i] > g_max)
                    g_max = d->src[i];
            uint64_t gw, aw;
            memcpy(&gw, &g_max, 8);
            memcpy(&aw, &hw_max, 8);
            if (gw != aw) {
                log_warning("sve512_pred_ops: FMAXV golden=0x%016" PRIx64
                            " actual=0x%016" PRIx64, gw, aw);
                all_passed = false;
            }
        }
    }

    if (!all_passed) {
        report_fail_msg("sve512_pred_ops_arm: SVE predicate-network SDC "
                        "detected (whilelt/svnot/FADDA/FFR/FMAXV)");
        return EXIT_FAILURE;
    }

    while (test_time_condition(test)) {
        // light re-loop of the density sweep (the full sweep already ran)
        svbool_t pg = svwhilelt_b64_u64(0, vl - 1);
        svfloat64_t vsrc = svld1_f64(all, d->src.data());
        std::vector<uint64_t> z(vl, 0);
        svfloat64_t w = svld1_f64(all, reinterpret_cast<const double *>(z.data()));
        w = svmla_f64_m(pg, w, vsrc, vsrc);   // 0 + src*src on active lanes
        std::vector<uint64_t> out(vl, 0);
        svst1_f64(all, reinterpret_cast<double *>(out.data()), w);
        for (size_t i = 0; i + 1 < vl; ++i) {
            double g = std::fma(d->src[i], d->src[i], 0.0);
            uint64_t gw, aw;
            memcpy(&gw, &g, 8);
            memcpy(&aw, &out[i], 8);
            if (gw != aw) {
                report_fail_msg("sve512_pred_ops_arm: predicate FMA SDC "
                                "detected (re-loop)");
                return EXIT_FAILURE;
            }
        }
    }

    return EXIT_SUCCESS;
#endif
}

static int sve512_pred_ops_arm_cleanup(struct test *test)
{
#ifdef __aarch64__
    delete static_cast<PredOpsData *>(test->data);
#else
    (void)test;
#endif
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(sve512_pred_ops_arm,
             "SVE predicate-dimension SDC stress: whilelt density sweep "
             "(1..VL active lanes, sentinel-guarded inactive lanes), "
             "predicate inversion (svnot_b_z complement FMA), FADDA "
             "sequential reduction chain, LDFF1+FFR first-fault state, "
             "FMAXV masked-tree reduction — the predicate network every "
             "full-true SVE test leaves untested")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_pred_ops_arm_init,
    .test_run = sve512_pred_ops_arm_run,
    .test_cleanup = sve512_pred_ops_arm_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
