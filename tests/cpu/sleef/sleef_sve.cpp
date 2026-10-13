/**
 * @file
 *
 * @copyright
 * Copyright 2026 ISCAS.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sleef_sve
 * @parblock
 * Stress test for SVE data paths via SLEEF's SVE transcendental
 * functions: the original sin/cos/exp/log double families plus the
 * float32 tier (svfloat32_t, 4 lanes at fixed VL=128) and the u35
 * second-accuracy chains (sin/cos/log/exp2 — u35 SVE has no exp, exp2
 * is its sibling with an independent polynomial chain). Long polynomial
 * FMA dependency chains; results only feed a byte-exact golden compare
 * (data-flow payload: faults surface as wrong values, not crashes —
 * From Gates to SDCs DATE'25). Pure functions => deterministic golden
 * values, recomputed from freshly re-rolled inputs every kRerollEvery
 * iterations with the same kernels (per-thread, framework RNG — B2-006;
 * init's pass stays only as the finiteness canary). u10 and u35 are
 * SLEEF's two INDEPENDENT implementations of the same mathematics
 * (different coefficients, different FMA-chain schedules), so the
 * second tier is a free second-phase sample of the same silicon for
 * the same math. Covers the newest-instruction-generation risk area
 * (PinDrop Obs12: instructions fail most in their first arch gen) —
 * SVE is the newest-generation vector datapath on ARM64. Compiled in
 * the dedicated tests_sve static library (-march=armv8.2-a+sve
 * -msve-vector-bits=128) so the SVE instructions never leak into the
 * NEON baseline build.
 *
 * Runtime gate: silicon without SVE (e.g. Kunpeng 920) skips cleanly
 * (CpuNotSupported / EXIT_SKIP). Real execution requires SVE hardware
 * (Kunpeng 930 / Neoverse) — documented limitation of this development
 * host; on SVE silicon the test runs the full payload.
 * @endparblock
 */

#include <sandstone.h>

#include <sleef.h>

#include <arm_sve.h>
#include <sys/auxv.h>
#include <asm/hwcap.h>
#include <string.h>
#include <stdlib.h>

/* Fixed 128-bit SVE vector length (-msve-vector-bits=128, matching the
 * tests_sve build block): svfloat64_t is exactly 2 lanes and svfloat32_t
 * exactly 4, so ELEMS (divisible by 4) processes in ELEMS/2 svld1_f64/
 * svst1_f64 pairs (svptrue_b64()) plus ELEMS/4 svld1_f32/svst1_f32
 * quadruples (svptrue_b32()) with fully-active predicates — each
 * predicate is correct because every load/store touches exactly the
 * lanes the fixed VL provides. */
#define ELEMS 1024   /* elements per family: 512 svfloat64_t vectors (2 lanes) + 256 svfloat32_t vectors (4 lanes) */

namespace {
struct sleef_test_data {
    /* per-domain inputs; the run loop keeps its OWN per-thread re-rolled
     * copy (B2-006) — the shared arrays are filled and golden-checked
     * once in init as the finiteness canary, then never read again */
    double *xd_trig, *xd_exp, *xd_log;
    float  *xf_trig, *xf_exp, *xf_log;
    /* golden outputs, u10 tier, double & float (same canary role) */
    double *gd_sin, *gd_cos, *gd_exp, *gd_log;
    float  *gf_sin, *gf_cos, *gf_exp, *gf_log;
    /* u35 second-accuracy tier golden outputs (SLEEF's other polynomial
     * implementation of the same math; u35 SVE has no exp — exp2 stands
     * in, itself an independent FMA chain) */
    double *gd_sin35, *gd_cos35, *gd_log35, *gd_exp2_35;
};
}

#define CAST(_x) static_cast<struct sleef_test_data *>(_x)

/* Scratch output buffers are per-thread: the framework runs one worker
 * thread per core and they all share test->data, so anything written during
 * test_run must be thread-local (od/of are the kernel destinations, written
 * every iteration). Allocated lazily on first use and intentionally not
 * freed in cleanup, exactly like the sleef_neon / openblas_* scratch
 * buffers: the worker threads are gone (or never existed) when the main
 * thread runs test_cleanup, and the forked child exits right after, so
 * the OS reclaims the storage. calloc zeroes them once so a short first
 * iteration can never compare garbage.
 *
 * B2-006 (R2b freeze blind spot): l is this thread's own sleef_test_data
 * — inputs re-rolled from the framework RNG and goldens recomputed with
 * the very same SLEEF kernels every kRerollEvery iterations; the shared
 * test->data inputs/goldens are never read by test_run (init's fill
 * stays only as the init-time finiteness canary, matching the 46141ed4
 * Eigen pattern where run owns its inputs outright). */
static thread_local double *od = nullptr;
static thread_local float  *of = nullptr;
static thread_local struct sleef_test_data *l = nullptr;

/* Uniform double in [0,1) from the framework RNG (top 53 bits; libc rand is
 * trapped by the framework). */
static double uniform01(void)
{
    return (double)(random64() >> 11) / 9007199254740992.0;
}

/* Domain-boundary analysis — identical construction to sleef_neon (see
 * there for the full write-up):
 *  - trig : u in [0,1) tops out at 1-2^-53, so u*2-1 (exact in binary)
 *           lies in [-1, 1-2^-52]; times pi the magnitude stays strictly
 *           inside [-pi, pi] after the roundings. sin/cos of that is
 *           guaranteed finite and well inside [-1, 1].
 *  - exp  : same construction over [-20, 20-2^-48]; exp of that is in
 *           [~2e-9, ~4.85e8], forty orders from overflow and far above
 *           subnormal float (float min normal ~1.2e-38).
 *  - log  : u in [0,1) plus 1e-300 keeps the argument strictly positive
 *           (u==0 gives 1e-300, a normal double), scaled by 1e10: log
 *           arguments span [1e-290, 1e10), all finite, all inside SLEEF's
 *           u10 accuracy domain (positive reals).
 *  - float narrowing CAUTION: the [0,1) double does NOT stay below 1
 *           after narrowing — doubles in (1-2^-25, 1) round UP to exactly
 *           1.0f — so xf_trig/xf_exp can reach exactly +-pi_f / +-20_f
 *           (the domain edge). Harmless: the kernels carry their accuracy
 *           guarantee over the whole input domain, determinism never
 *           depends on accuracy (the golden values come from the very
 *           same kernel), and the init finiteness tripwire below
 *           backstops the rest. The float log variant uses a 1e-30f
 *           offset so it survives the narrowing: arguments span
 *           [~1e-20, ~1e10).
 *  - u35 tier: identical input arrays and identical domain analysis as
 *    the u10 tier above (trig for sin/cos, log for log, exp for exp2 —
 *    exp2's domain is all reals and exp2 of [-20,20) is [2^-20, 2^20) =
 *    [~9.5e-7, ~1.05e6], comfortably finite). The u35 kernels are a
 *    DIFFERENT polynomial implementation of the same functions, so the
 *    outputs land on different bits than u10 — that is the point
 *    (second-phase sample), and determinism is per-kernel so the
 *    byte-exact golden compare stays valid.
 * Every golden value is additionally checked for finiteness in init: a
 * NaN/Inf would make the byte-exact comparison meaningless (NaN != NaN). */

/* Fill one sleef_test_data's input arrays with fresh domain-mapped
 * framework-RNG randoms. Shared by init and the run-loop reroll (B2-006),
 * so the mapping and the boundary analysis above are guaranteed identical
 * in both places. */
static void sleef_fill_inputs(struct sleef_test_data *p)
{
    /* double inputs, per domain (same mappings and boundary analysis as sleef_neon) */
    for (int i = 0; i < ELEMS; ++i) {
        double u = uniform01();
        p->xd_trig[i] = (u * 2.0 - 1.0) * 3.141592653589793;   /* [-pi, pi] */
    }
    for (int i = 0; i < ELEMS; ++i) {
        double u = uniform01();
        p->xd_exp[i] = (u * 2.0 - 1.0) * 20.0;                 /* [-20, 20) */
    }
    for (int i = 0; i < ELEMS; ++i) {
        double u = uniform01();
        p->xd_log[i] = (u + 1.0e-300) * 1.0e10;                /* positive */
    }

    /* float inputs, per domain. CAUTION: the [0,1) double does NOT stay
     * below 1 after narrowing — doubles in (1-2^-25, 1) round UP to exactly
     * 1.0f — so xf_trig/xf_exp can reach exactly +-pi_f / +-20_f (the domain
     * edge). Harmless: the u10 kernels carry their accuracy guarantee over
     * the whole input domain, determinism never depends on accuracy (the
     * golden values come from the very same kernel), and the init finiteness
     * tripwire backstops the rest. */
    for (int i = 0; i < ELEMS; ++i) {
        float u = (float)uniform01();
        p->xf_trig[i] = (u * 2.0f - 1.0f) * 3.14159265f;       /* [-pi, pi] */
    }
    for (int i = 0; i < ELEMS; ++i) {
        float u = (float)uniform01();
        p->xf_exp[i] = (u * 2.0f - 1.0f) * 20.0f;              /* [-20, 20) */
    }
    for (int i = 0; i < ELEMS; ++i) {
        float u = (float)uniform01();
        p->xf_log[i] = (u + 1.0e-30f) * 1.0e10f;               /* positive */
    }
}

/* Compute the golden outputs from p's inputs with the very same SVE
 * kernels the run loop verifies against — the shared-helper structure is
 * what keeps the byte-exact comparison valid across rerolls. */
static void sleef_compute_goldens(struct sleef_test_data *p)
{
    const svbool_t pg = svptrue_b64();
    for (int i = 0; i < ELEMS; i += 2) {
        svfloat64_t x = svld1_f64(pg, &p->xd_trig[i]);
        svst1_f64(pg, &p->gd_sin[i], Sleef_sindx_u10sve(x));
        svst1_f64(pg, &p->gd_cos[i], Sleef_cosdx_u10sve(x));
        x = svld1_f64(pg, &p->xd_exp[i]);
        svst1_f64(pg, &p->gd_exp[i], Sleef_expdx_u10sve(x));
        x = svld1_f64(pg, &p->xd_log[i]);
        svst1_f64(pg, &p->gd_log[i], Sleef_logdx_u10sve(x));
        /* u35 second tier: same inputs, the OTHER polynomial implementation */
        svst1_f64(pg, &p->gd_sin35[i], Sleef_sindx_u35sve(svld1_f64(pg, &p->xd_trig[i])));
        svst1_f64(pg, &p->gd_cos35[i], Sleef_cosdx_u35sve(svld1_f64(pg, &p->xd_trig[i])));
        svst1_f64(pg, &p->gd_log35[i], Sleef_logdx_u35sve(svld1_f64(pg, &p->xd_log[i])));
        /* u35 has NO exp — exp2 is the independent-chain stand-in */
        svst1_f64(pg, &p->gd_exp2_35[i], Sleef_exp2dx_u35sve(svld1_f64(pg, &p->xd_exp[i])));
    }
    const svbool_t pgf = svptrue_b32();
    for (int i = 0; i < ELEMS; i += 4) {
        svfloat32_t x = svld1_f32(pgf, &p->xf_trig[i]);
        svst1_f32(pgf, &p->gf_sin[i], Sleef_sinfx_u10sve(x));
        svst1_f32(pgf, &p->gf_cos[i], Sleef_cosfx_u10sve(x));
        x = svld1_f32(pgf, &p->xf_exp[i]);
        svst1_f32(pgf, &p->gf_exp[i], Sleef_expfx_u10sve(x));
        x = svld1_f32(pgf, &p->xf_log[i]);
        svst1_f32(pgf, &p->gf_log[i], Sleef_logfx_u10sve(x));
    }
}

static int sleef_sve_init(struct test *test)
{
    /* Runtime gate. NOTE: device_has_feature(cpu_feature_sve) is useless
     * here — it is defined as (device_compiler_features & f) || (device_features
     * & f) and this TU is compiled with +sve, so the compiler half would make
     * it always-true on any CPU. Probe HWCAP_SVE directly via getauxval,
     * exactly like eigen_svd_cdouble_sve: the vDSO call executes no SVE
     * instructions, so it is safe to run before any SVE code path. The
     * framework-level compiler_minimum_device gate (the "test compiled with
     * sve" skip) already catches non-SVE hosts before init; this explicit
     * probe keeps a sleef_sve-specific skip reason even if that framework
     * gate ordering ever changes. */
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SVE not available on this CPU; "
                 "sleef_sve requires SVE (e.g. Kunpeng 930)");
        return EXIT_SKIP;
    }

    auto d = new(sleef_test_data);
    test->data = d;
    d->xd_trig = (double *)malloc(ELEMS * sizeof(double));
    d->xd_exp  = (double *)malloc(ELEMS * sizeof(double));
    d->xd_log  = (double *)malloc(ELEMS * sizeof(double));
    d->xf_trig = (float  *)malloc(ELEMS * sizeof(float));
    d->xf_exp  = (float  *)malloc(ELEMS * sizeof(float));
    d->xf_log  = (float  *)malloc(ELEMS * sizeof(float));
    d->gd_sin = (double *)malloc(ELEMS * sizeof(double));
    d->gd_cos = (double *)malloc(ELEMS * sizeof(double));
    d->gd_exp = (double *)malloc(ELEMS * sizeof(double));
    d->gd_log = (double *)malloc(ELEMS * sizeof(double));
    d->gf_sin = (float  *)malloc(ELEMS * sizeof(float));
    d->gf_cos = (float  *)malloc(ELEMS * sizeof(float));
    d->gf_exp = (float  *)malloc(ELEMS * sizeof(float));
    d->gf_log = (float  *)malloc(ELEMS * sizeof(float));
    /* u35 tier golden outputs */
    d->gd_sin35 = (double *)malloc(ELEMS * sizeof(double));
    d->gd_cos35 = (double *)malloc(ELEMS * sizeof(double));
    d->gd_log35 = (double *)malloc(ELEMS * sizeof(double));
    d->gd_exp2_35 = (double *)malloc(ELEMS * sizeof(double));
    if (!d->xd_trig || !d->xd_exp || !d->xd_log ||
        !d->xf_trig || !d->xf_exp || !d->xf_log ||
        !d->gd_sin || !d->gd_cos || !d->gd_exp || !d->gd_log ||
        !d->gf_sin || !d->gf_cos || !d->gf_exp || !d->gf_log ||
        !d->gd_sin35 || !d->gd_cos35 || !d->gd_log35 || !d->gd_exp2_35) {
        report_fail_msg("OOM in sleef_sve init");
    }

    /* fill inputs through the shared helper — init's pass stays only as
     * the init-time finiteness canary (B2-006); the run loop re-rolls its
     * own per-thread copy every kRerollEvery iterations */
    sleef_fill_inputs(d);

    /* golden outputs from the very same SVE kernels, once; any non-finite
     * result would poison the byte-exact comparison (NaN != NaN), so fail
     * loudly at the source instead (same tripwire as sleef_neon) */
    sleef_compute_goldens(d);
    for (int i = 0; i < ELEMS; ++i) {
        double gs = d->gd_sin[i], gc = d->gd_cos[i];
        double ge = d->gd_exp[i], gl = d->gd_log[i];
        if (gs != gs || gs > 1.0 || gs < -1.0 || gc != gc || gc > 1.0 || gc < -1.0 ||
            ge != ge || ge > 1.0e300 || ge < 0.0 ||
            gl != gl || gl > 1.0e300 || gl < -1.0e300) {
            report_fail_msg("golden value not finite/in-range at element %d "
                            "(domain mapping needs tighter scaling)", i);
        }
    }
    for (int i = 0; i < ELEMS; ++i) {
        float gs = d->gf_sin[i], gc = d->gf_cos[i];
        float ge = d->gf_exp[i], gl = d->gf_log[i];
        if (gs != gs || gs > 1.0f || gs < -1.0f || gc != gc || gc > 1.0f || gc < -1.0f ||
            ge != ge || ge > 1.0e30f || ge < 0.0f ||
            gl != gl || gl > 1.0e30f || gl < -1.0e30f) {
            report_fail_msg("float golden value not finite/in-range at element %d "
                            "(domain mapping needs tighter scaling)", i);
        }
    }
    /* finiteness sentinels, u35 tier. exp2(20) ~ 1.05e6 so the bound is
     * 1e7 (an order of headroom); sin/cos stay in [-1,1]; log shares the
     * original log range. */
    for (int i = 0; i < ELEMS; ++i) {
        double gs = d->gd_sin35[i], gc = d->gd_cos35[i];
        double gl = d->gd_log35[i], ge = d->gd_exp2_35[i];
        if (gs != gs || gs > 1.0 || gs < -1.0 || gc != gc || gc > 1.0 || gc < -1.0 ||
            gl != gl || gl > 1.0e300 || gl < -1.0e300 ||
            ge != ge || ge > 1.0e7 || ge < 0.0) {
            report_fail_msg("double u35 golden value not finite/in-range at element %d "
                            "(domain mapping needs tighter scaling)", i);
        }
    }
    return EXIT_SUCCESS;
}

static int sleef_sve_run(struct test *test, int cpu)
{
    constexpr int kRerollEvery = 16;
    int since_reroll = kRerollEvery; /* force the first-iteration fill */
    TEST_LOOP(test, 1) {
        /* lazily allocate this thread's scratch output buffers and its own
         * sleef_test_data (B2-006: run never reads the shared test->data) */
        if (__builtin_expect(!od || !of || !l, 0)) {
            od = (double *)calloc(1, ELEMS * sizeof(double));
            of = (float  *)calloc(1, ELEMS * sizeof(float));
            l = (struct sleef_test_data *)malloc(sizeof(*l));
            if (l) {
                l->xd_trig = (double *)malloc(ELEMS * sizeof(double));
                l->xd_exp  = (double *)malloc(ELEMS * sizeof(double));
                l->xd_log  = (double *)malloc(ELEMS * sizeof(double));
                l->xf_trig = (float  *)malloc(ELEMS * sizeof(float));
                l->xf_exp  = (float  *)malloc(ELEMS * sizeof(float));
                l->xf_log  = (float  *)malloc(ELEMS * sizeof(float));
                l->gd_sin = (double *)malloc(ELEMS * sizeof(double));
                l->gd_cos = (double *)malloc(ELEMS * sizeof(double));
                l->gd_exp = (double *)malloc(ELEMS * sizeof(double));
                l->gd_log = (double *)malloc(ELEMS * sizeof(double));
                l->gf_sin = (float  *)malloc(ELEMS * sizeof(float));
                l->gf_cos = (float  *)malloc(ELEMS * sizeof(float));
                l->gf_exp = (float  *)malloc(ELEMS * sizeof(float));
                l->gf_log = (float  *)malloc(ELEMS * sizeof(float));
                l->gd_sin35 = (double *)malloc(ELEMS * sizeof(double));
                l->gd_cos35 = (double *)malloc(ELEMS * sizeof(double));
                l->gd_log35 = (double *)malloc(ELEMS * sizeof(double));
                l->gd_exp2_35 = (double *)malloc(ELEMS * sizeof(double));
            }
            /* short-circuit || leaves the l-> member checks unevaluated
             * when l itself failed to allocate; report_fail_msg is
             * noreturn so the buffers are valid past this point */
            if (!od || !of || !l ||
                !l->xd_trig || !l->xd_exp || !l->xd_log ||
                !l->xf_trig || !l->xf_exp || !l->xf_log ||
                !l->gd_sin || !l->gd_cos || !l->gd_exp || !l->gd_log ||
                !l->gf_sin || !l->gf_cos || !l->gf_exp || !l->gf_log ||
                !l->gd_sin35 || !l->gd_cos35 || !l->gd_log35 || !l->gd_exp2_35) {
                report_fail_msg("OOM allocating thread data (%zu bytes)",
                                (size_t)ELEMS * (11 * sizeof(double) + 7 * sizeof(float) +
                                                 sizeof(double) + sizeof(float)) +
                                sizeof(*l));
            }
        }

        /* B2-006 reroll: fresh framework-RNG inputs and goldens recomputed
         * with the very same kernels every kRerollEvery iterations (46141ed4
         * Eigen pattern) — the init-frozen shared arrays are never replayed */
        if (++since_reroll >= kRerollEvery) {
            since_reroll = 0;
            sleef_fill_inputs(l);
            sleef_compute_goldens(l);
        }

        /* compute-out: each family replays this thread's re-rolled domain
         * inputs through the same SVE kernels and lands in the
         * thread-private scratch */
        const svbool_t pg = svptrue_b64();
        for (int i = 0; i < ELEMS; i += 2) {
            svfloat64_t x = svld1_f64(pg, &l->xd_trig[i]);
            svst1_f64(pg, &od[i], Sleef_sindx_u10sve(x));
        }
        memcmp_or_fail(od, l->gd_sin, ELEMS, "sin double sve");

        for (int i = 0; i < ELEMS; i += 2) {
            svfloat64_t x = svld1_f64(pg, &l->xd_trig[i]);
            svst1_f64(pg, &od[i], Sleef_cosdx_u10sve(x));
        }
        memcmp_or_fail(od, l->gd_cos, ELEMS, "cos double sve");

        for (int i = 0; i < ELEMS; i += 2) {
            svfloat64_t x = svld1_f64(pg, &l->xd_exp[i]);
            svst1_f64(pg, &od[i], Sleef_expdx_u10sve(x));
        }
        memcmp_or_fail(od, l->gd_exp, ELEMS, "exp double sve");

        for (int i = 0; i < ELEMS; i += 2) {
            svfloat64_t x = svld1_f64(pg, &l->xd_log[i]);
            svst1_f64(pg, &od[i], Sleef_logdx_u10sve(x));
        }
        memcmp_or_fail(od, l->gd_log, ELEMS, "log double sve");

        const svbool_t pgf = svptrue_b32();
        for (int i = 0; i < ELEMS; i += 4) {
            svfloat32_t x = svld1_f32(pgf, &l->xf_trig[i]);
            svst1_f32(pgf, &of[i], Sleef_sinfx_u10sve(x));
        }
        memcmp_or_fail(of, l->gf_sin, ELEMS, "sin float sve");

        for (int i = 0; i < ELEMS; i += 4) {
            svfloat32_t x = svld1_f32(pgf, &l->xf_trig[i]);
            svst1_f32(pgf, &of[i], Sleef_cosfx_u10sve(x));
        }
        memcmp_or_fail(of, l->gf_cos, ELEMS, "cos float sve");

        for (int i = 0; i < ELEMS; i += 4) {
            svfloat32_t x = svld1_f32(pgf, &l->xf_exp[i]);
            svst1_f32(pgf, &of[i], Sleef_expfx_u10sve(x));
        }
        memcmp_or_fail(of, l->gf_exp, ELEMS, "exp float sve");

        for (int i = 0; i < ELEMS; i += 4) {
            svfloat32_t x = svld1_f32(pgf, &l->xf_log[i]);
            svst1_f32(pgf, &of[i], Sleef_logfx_u10sve(x));
        }
        memcmp_or_fail(of, l->gf_log, ELEMS, "log float sve");

        /* u35 second-accuracy tier: same inputs, other polynomial chain */
        for (int i = 0; i < ELEMS; i += 2) {
            svfloat64_t x = svld1_f64(pg, &l->xd_trig[i]);
            svst1_f64(pg, &od[i], Sleef_sindx_u35sve(x));
        }
        memcmp_or_fail(od, l->gd_sin35, ELEMS, "sin35 double sve");

        for (int i = 0; i < ELEMS; i += 2) {
            svfloat64_t x = svld1_f64(pg, &l->xd_trig[i]);
            svst1_f64(pg, &od[i], Sleef_cosdx_u35sve(x));
        }
        memcmp_or_fail(od, l->gd_cos35, ELEMS, "cos35 double sve");

        for (int i = 0; i < ELEMS; i += 2) {
            svfloat64_t x = svld1_f64(pg, &l->xd_log[i]);
            svst1_f64(pg, &od[i], Sleef_logdx_u35sve(x));
        }
        memcmp_or_fail(od, l->gd_log35, ELEMS, "log35 double sve");

        for (int i = 0; i < ELEMS; i += 2) {
            svfloat64_t x = svld1_f64(pg, &l->xd_exp[i]);
            svst1_f64(pg, &od[i], Sleef_exp2dx_u35sve(x));
        }
        memcmp_or_fail(od, l->gd_exp2_35, ELEMS, "exp2_35 double sve");
    }
    return EXIT_SUCCESS;
}

static int sleef_sve_cleanup(struct test *test)
{
    auto d = CAST(test->data);
    free(d->xd_trig); free(d->xd_exp); free(d->xd_log);
    free(d->xf_trig); free(d->xf_exp); free(d->xf_log);
    free(d->gd_sin); free(d->gd_cos); free(d->gd_exp); free(d->gd_log);
    free(d->gf_sin); free(d->gf_cos); free(d->gf_exp); free(d->gf_log);
    free(d->gd_sin35); free(d->gd_cos35); free(d->gd_log35); free(d->gd_exp2_35);
    delete d;
    return EXIT_SUCCESS;
}

DECLARE_TEST(sleef_sve, "SLEEF vectorized transcendentals (SVE polynomial FMA chains, sin/cos/exp/log double+float + u35 tier (exp2) double)")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = sleef_sve_init,
  .test_run = sleef_sve_run,
  .test_cleanup = sleef_sve_cleanup,
  .minimum_cpu = 0,
  .fracture_loop_count = 4,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
