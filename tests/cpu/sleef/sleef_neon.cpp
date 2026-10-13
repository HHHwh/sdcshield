/**
 * @file
 *
 * @copyright
 * Copyright 2026 ISCAS.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sleef_neon
 * @parblock
 * Stress test for NEON data paths via SLEEF's vectorized transcendental
 * functions: the original sin/cos/exp/log families plus the u35
 * second-accuracy tier (sin/cos/log/exp2 — u35 has NO exp, exp2 is its
 * sibling with an independent polynomial chain) and the new asin/atan/
 * cbrt/log10/sinh/tanh/pow families, double & float, u10 and u35
 * accuracy, advsimd kernels. Long polynomial FMA dependency chains;
 * results only feed a byte-exact golden compare (data-flow payload:
 * faults surface as wrong values, not crashes — From Gates to SDCs
 * DATE'25). Pure functions => deterministic golden values, recomputed
 * from freshly re-rolled inputs every kRerollEvery iterations with the
 * same kernels (per-thread, framework RNG — B2-006; init's pass stays
 * only as the finiteness canary). u10 and u35 are SLEEF's two INDEPENDENT
 * implementations of the same mathematics (different coefficients,
 * different FMA-chain schedules), so the second tier is a free
 * second-phase sample of the same silicon for the same math.
 * Covers the newest-instruction-generation risk area (PinDrop Obs12:
 * instructions fail most in their first arch gen) — the advsimd u10
 * codepaths are SLEEF's newest NEON implementations.
 *
 * The per-family element count is a runtime footprint knob:
 * "-O sleef_neon.nelems=N" (128..262144 in multiples of 4, default 1024;
 * the test-id prefix is required — a bare "nelems=N" is silently
 * ignored). It sweeps the replay working set across the cache hierarchy
 * (default 1024: a double segment's input+golden+scratch = 24KB stays
 * L1D-resident; 16384: ~384KB -> L2; 262144: 2MB per double buffer ->
 * LLC/DRAM; see the ELEMS_DEFAULT comment for the numbers), because
 * different cache structures hold different ACE residency. Omitting the
 * knob keeps the historical default element count (the init canary pass
 * consumes RNG in the historical order; the run loop's per-thread rerolls
 * draw fresh values from the framework RNG every kRerollEvery iterations
 * — B2-006).
 * @endparblock
 */

#include <sandstone.h>

#include <sleef.h>

#include <arm_neon.h>
#include <string.h>
#include <stdlib.h>

/* Elements per family — a RUNTIME knob, not a compile-time constant:
 * "-O sleef_neon.nelems=N". The default reproduces the historical
 * ELEMS=1024 exactly (identical RNG consumption order, byte-identical
 * inputs/goldens). Footprint tiers on Kunpeng 920 (TSV110 core):
 *   1024   — a double replay segment touches input+golden+scratch
 *            3 x 8KB = 24KB, resident in L1D (64KB)
 *   16384  — 3 x 128KB = 384KB per double segment -> L2 (512KB)
 *   262144 — 2MB per double buffer (1MB float) x several buffers
 *            -> LLC / DRAM
 * (different cache structures hold different ACE residency; the
 * openblas mdim knob is the same lesson's GEMM version). ELEMS_MIN and
 * every accepted value must be a multiple of 4: the float halves advance
 * in float32x4_t stride-4 steps (double stride 2 divides 4), and a
 * non-multiple would make the last vector iteration overrun its buffer
 * (validated loudly in init). Per-thread data ceiling at ELEMS_MAX
 * (B2-006: inputs/goldens are per-thread now, not shared): 22 double +
 * 22 float arrays plus od/of scratch = 276 B/element, ~72MB per worker
 * at the knob maximum; 192-core worst case ~ 14GB — the server-class
 * hosts that carry that many cores carry the RAM, and the default 1024
 * stays ~276KB per worker. */
#define ELEMS_DEFAULT 1024  /* elements per family: 512 float64x2 vectors (double half) + 256 float32x4 vectors (float half) */
#define ELEMS_MIN 128       /* multiple of 4 — float32x4_t stride (see above) */
#define ELEMS_MAX 262144    /* per-thread scratch ceiling ~3MB/worker (see above) */

namespace {
struct sleef_test_data {
    /* elements per family, from the -O sleef_neon.nelems=N knob
     * (ELEMS_MIN..ELEMS_MAX, default ELEMS_DEFAULT): every loop bound and
     * buffer size below derives from it, so the whole working set (inputs
     * + goldens + per-thread scratch) scales with the knob — see the
     * ELEMS_DEFAULT comment above for the L1/L2/LLC/DRAM tier rationale */
    int nelems;
    /* per-domain inputs; the run loop keeps its OWN per-thread re-rolled
     * copy (B2-006) — the shared arrays are filled and golden-checked
     * once in init as the finiteness canary, then never read again */
    double *xd_trig, *xd_exp, *xd_log;
    float  *xf_trig, *xf_exp, *xf_log;
    /* golden outputs (filled in init as the finiteness canary; the run
     * loop recomputes its own per-thread copy every kRerollEvery
     * iterations — B2-006) */
    double *gd_sin, *gd_cos, *gd_exp, *gd_log;
    float  *gf_sin, *gf_cos, *gf_exp, *gf_log;
    /* u35 second-accuracy tier golden outputs (SLEEF's other polynomial
     * implementation of the same math; u35 NEON has no exp — exp2 stands
     * in, itself an independent FMA chain) */
    double *gd_sin35, *gd_cos35, *gd_log35, *gd_exp2_35;
    float  *gf_sin35, *gf_cos35, *gf_log35, *gf_exp2_35;
    /* new function families: inputs, then golden outputs. asin/atan share
     * the [-1,1) input array (xd_inv/xf_inv); cbrt/log10/sinh/tanh reuse
     * the cbrt/log/exp domain arrays respectively (cbrt gets its own wide
     * [-1000,1000) array; log10 reuses xd_log/xf_log, sinh/tanh reuse
     * xd_exp/xf_exp). pow is the only two-argument kernel: xd_pow2/xf_pow2
     * hold the bases, xd_pexp/xf_pexp the exponents. */
    double *xd_inv,  *xd_cbrt, *gd_asin, *gd_atan, *gd_cbrt, *gd_log10, *gd_sinh, *gd_tanh;
    double *xd_pow2, *xd_pexp, *gd_pow;
    float  *xf_inv,  *xf_cbrt, *gf_asin, *gf_atan, *gf_cbrt, *gf_log10, *gf_sinh, *gf_tanh;
    float  *xf_pow2, *xf_pexp, *gf_pow;
};
}

#define CAST(_x) static_cast<struct sleef_test_data *>(_x)

/* Scratch output buffers are per-thread: the framework runs one worker
 * thread per core and they all share test->data, so anything written during
 * test_run must be thread-local (od/of are the kernel destinations, written
 * every iteration). Allocated lazily on first use and intentionally not
 * freed in cleanup, exactly like the openblas_* scratch buffers: the worker
 * threads are gone (or never existed) when the main thread runs
 * test_cleanup, and the forked child exits right after, so the OS reclaims
 * the storage. calloc zeroes them once so a short first iteration can never
 * compare garbage.
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

/* Domain-boundary analysis for the mappings below (the naive u*span+lo
 * write-up can round to the boundary itself):
 *  - trig : u in [0,1) tops out at 1-2^-53, so u*2-1 (exact in binary)
 *           lies in [-1, 1-2^-52]; times pi the magnitude stays
 *           <= pi*(1-2^-52)*(1+2eps) — strictly inside [-pi, pi] after the
 *           two roundings. sin/cos of that is also guaranteed finite and
 *           well inside [-1, 1].
 *  - exp  : same construction over [-20, 20-2^-48]; exp of that is in
 *           [~2e-9, ~4.85e8], forty orders from overflow and far above
 *           subnormal double/float (float min normal ~1.2e-38).
 *  - log  : u in [0,1) plus 1e-300 keeps the argument strictly positive
 *           (u==0 gives 1e-300, a normal double; the float variant uses
 *           1e-30 so the offset survives the float narrowing), scaled by
 *           1e10: log arguments span [1e-290, 1e10) for the double variant
 *           ([~1e-20, ~1e10) once narrowed to float), all finite, all
 *           inside SLEEF's u10 accuracy domain (positive reals).
 *
 * Domain-boundary analysis for the u35 tier and the new families:
 *  - u35 tier: identical input arrays and identical domain analysis as the
 *    u10 tier above (trig for sin/cos, log for log, exp for exp2 — exp2's
 *    domain is all reals and exp2 of [-20,20) is [2^-20, 2^20) = [~9.5e-7,
 *    ~1.05e6], comfortably finite in both widths). The u35 kernels are a
 *    DIFFERENT polynomial implementation of the same functions, so the
 *    outputs land on different bits than u10 — that is the point (second
 *    phase sample), and determinism is per-kernel so the byte-exact golden
 *    compare stays valid.
 *  - asin/atan: input u*2-1 in [-1, 1-2^-52] (exact in binary, same
 *    construction as trig without the pi scaling). asin's domain [-1,1]
 *    includes the endpoint -1 (u==0 gives exactly -1, asin(-1) = -pi/2,
 *    finite); atan is defined on all reals so [-1,1) is trivially safe.
 *    Output ranges: asin in [-pi/2, pi/2], atan in (-pi/2, pi/2) — the
 *    init sentinel uses the generous bound 1.6 (> pi/2 ~ 1.5708).
 *  - cbrt: input u*2000-1000 in [-1000, 1000-2^-43]. cbrt is defined on
 *    all reals (odd function, exact at 0, negative inputs give negative
 *    outputs); cbrt of that range is within [-10, 10) (cbrt(1000) = 10
 *    not included since the top is 1000-eps). Exercises the three states
 *    negative / zero / positive-plus-large in one family.
 *  - log10: reuses the log positive mapping, i.e. arguments in
 *    [1e-290, 1e10) double / [~1e-20, ~1e10) float. log10 of that is in
 *    (-290, 10] double / (-20, 10] float — the init sentinel uses
 *    (-700, 25] / (-30, 25] with generous headroom on both sides.
 *  - sinh/tanh: reuse the exp mapping [-20, 20). sinh(20) ~ 2.42e8 and
 *    sinh(-20) ~ -2.42e8 — far from double overflow (1.8e308) and float
 *    overflow (3.4e38); the init sentinel bound is 1e9. tanh of [-20,20)
 *    is in (-1, 1) mathematically, BUT SLEEF's tanh kernels (both u10
 *    xtanh and u35 xtanh_u35, verified in sleefsimddp.c) clamp to +-1.0
 *    for |x| > 18.714973875, and inputs in (18.715, 20) DO produce
 *    exactly +-1.0 by design. The sentinel therefore checks |g| <= 1
 *    (not < 1): the clamp value 1.0 is a legitimate, deterministic
 *    kernel output, and clamped-to-1.0 is still byte-comparable (same
 *    clamp fires on replay). The float kernel clamps at a different
 *    threshold — 8.664339742f (sleefsimdsp.c), not the double 18.714973875
 *    (the two thresholds are per-element-type constants) — but the same
 *    |g| <= 1 sentinel conclusion holds for both: inputs above the float
 *    threshold still clamp to exactly +-1.0f by design.
 *  - pow : base 1+u*9 in [1, 10-2^-49] (u*9 scaling, +1 keeps it >= 1:
 *    avoids 0^0, 0^negative and negative-base branch complexity
 *    entirely — every base is a positive normal number). Exponent
 *    u*8-4 in [-4, 4-2^-50]. Extreme products: pow(10, 4) = 1e4 (not
 *    reached, top exponent < 4) and pow(1, -4) = 1 — the result range is
 *    (0, 1e4] in both widths; float powf(10,4) = 1e4f exactly
 *    representable. No subnormal/overflow corners.
 * Every golden value is additionally checked for finiteness in init: a
 * NaN/Inf would make the byte-exact comparison meaningless (NaN != NaN). */

/* Fill one sleef_test_data's input arrays with fresh domain-mapped
 * framework-RNG randoms. Shared by init and the run-loop reroll (B2-006),
 * so the mapping and the boundary analysis above are guaranteed identical
 * in both places. */
static void sleef_fill_inputs(struct sleef_test_data *p)
{
    /* double inputs, per domain */
    for (int i = 0; i < p->nelems; ++i) {
        double u = uniform01();
        p->xd_trig[i] = (u * 2.0 - 1.0) * 3.141592653589793;   /* [-pi, pi] */
    }
    for (int i = 0; i < p->nelems; ++i) {
        double u = uniform01();
        p->xd_exp[i] = (u * 2.0 - 1.0) * 20.0;                 /* [-20, 20) */
    }
    for (int i = 0; i < p->nelems; ++i) {
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
    for (int i = 0; i < p->nelems; ++i) {
        float u = (float)uniform01();
        p->xf_trig[i] = (u * 2.0f - 1.0f) * 3.14159265f;       /* [-pi, pi] */
    }
    for (int i = 0; i < p->nelems; ++i) {
        float u = (float)uniform01();
        p->xf_exp[i] = (u * 2.0f - 1.0f) * 20.0f;              /* [-20, 20) */
    }
    for (int i = 0; i < p->nelems; ++i) {
        float u = (float)uniform01();
        p->xf_log[i] = (u + 1.0e-30f) * 1.0e10f;               /* positive */
    }

    /* new-family inputs, double. log10 shares xd_log (positive mapping),
     * sinh/tanh share xd_exp ([-20,20) — see the boundary analysis above
     * for why the tanh clamp at |x|>18.715 makes |tanh| <= 1 inclusive). */
    for (int i = 0; i < p->nelems; ++i) {
        double u = uniform01();
        p->xd_inv[i] = u * 2.0 - 1.0;                          /* [-1, 1) */
    }
    for (int i = 0; i < p->nelems; ++i) {
        double u = uniform01();
        p->xd_cbrt[i] = u * 2000.0 - 1000.0;                   /* [-1000, 1000) */
    }
    for (int i = 0; i < p->nelems; ++i) {
        double u = uniform01();
        p->xd_pow2[i] = 1.0 + u * 9.0;                         /* [1, 10) base */
    }
    for (int i = 0; i < p->nelems; ++i) {
        double u = uniform01();
        p->xd_pexp[i] = u * 8.0 - 4.0;                         /* [-4, 4) exponent */
    }

    /* new-family inputs, float (same narrowing caveat as xf_trig/xf_exp
     * above: e.g. 1+u*9 can round up to exactly 10.0f and the asin input
     * can reach exactly +-1.0f — both still inside/at the documented
     * domain edge, and determinism never depends on accuracy) */
    for (int i = 0; i < p->nelems; ++i) {
        float u = (float)uniform01();
        p->xf_inv[i] = u * 2.0f - 1.0f;                        /* [-1, 1) */
    }
    for (int i = 0; i < p->nelems; ++i) {
        float u = (float)uniform01();
        p->xf_cbrt[i] = u * 2000.0f - 1000.0f;                 /* [-1000, 1000) */
    }
    for (int i = 0; i < p->nelems; ++i) {
        float u = (float)uniform01();
        p->xf_pow2[i] = 1.0f + u * 9.0f;                       /* [1, 10) base */
    }
    for (int i = 0; i < p->nelems; ++i) {
        float u = (float)uniform01();
        p->xf_pexp[i] = u * 8.0f - 4.0f;                        /* [-4, 4) exponent */
    }
}

/* Compute one sleef_test_data's golden outputs with the very same SLEEF
 * kernels the run loop replays them with. Shared by init and the run-loop
 * reroll (B2-006) so the kernel call sequence is bit-identical in both
 * places — that identity is what makes the run loop's cross-reroll
 * byte-exact memcmp meaningful. */
static void sleef_compute_goldens(struct sleef_test_data *p)
{
    for (int i = 0; i < p->nelems; i += 2) {
        float64x2_t x = vld1q_f64(&p->xd_trig[i]);
        vst1q_f64(&p->gd_sin[i], Sleef_sind2_u10advsimd(x));
        vst1q_f64(&p->gd_cos[i], Sleef_cosd2_u10advsimd(x));
        x = vld1q_f64(&p->xd_exp[i]);
        vst1q_f64(&p->gd_exp[i], Sleef_expd2_u10advsimd(x));
        x = vld1q_f64(&p->xd_log[i]);
        vst1q_f64(&p->gd_log[i], Sleef_logd2_u10advsimd(x));
        /* u35 second tier: same inputs, the OTHER polynomial implementation */
        vst1q_f64(&p->gd_sin35[i], Sleef_sind2_u35advsimd(vld1q_f64(&p->xd_trig[i])));
        vst1q_f64(&p->gd_cos35[i], Sleef_cosd2_u35advsimd(vld1q_f64(&p->xd_trig[i])));
        vst1q_f64(&p->gd_log35[i], Sleef_logd2_u35advsimd(vld1q_f64(&p->xd_log[i])));
        /* u35 has NO exp — exp2 is the independent-chain stand-in */
        vst1q_f64(&p->gd_exp2_35[i], Sleef_exp2d2_u35advsimd(vld1q_f64(&p->xd_exp[i])));
        /* new families: asin/atan ([-1,1) inputs), cbrt ([-1000,1000)),
         * log10 (positive log inputs), sinh/tanh ([-20,20) exp inputs),
         * pow (base [1,10) x exponent [-4,4), the only 2-arg kernel) */
        vst1q_f64(&p->gd_asin[i], Sleef_asind2_u35advsimd(vld1q_f64(&p->xd_inv[i])));
        vst1q_f64(&p->gd_atan[i], Sleef_atand2_u35advsimd(vld1q_f64(&p->xd_inv[i])));
        vst1q_f64(&p->gd_cbrt[i], Sleef_cbrtd2_u35advsimd(vld1q_f64(&p->xd_cbrt[i])));
        vst1q_f64(&p->gd_log10[i], Sleef_log10d2_u10advsimd(vld1q_f64(&p->xd_log[i])));
        vst1q_f64(&p->gd_sinh[i], Sleef_sinhd2_u35advsimd(vld1q_f64(&p->xd_exp[i])));
        vst1q_f64(&p->gd_tanh[i], Sleef_tanhd2_u35advsimd(vld1q_f64(&p->xd_exp[i])));
        vst1q_f64(&p->gd_pow[i], Sleef_powd2_u10advsimd(vld1q_f64(&p->xd_pow2[i]),
                                                        vld1q_f64(&p->xd_pexp[i])));
    }
    for (int i = 0; i < p->nelems; i += 4) {
        float32x4_t x = vld1q_f32(&p->xf_trig[i]);
        vst1q_f32(&p->gf_sin[i], Sleef_sinf4_u10advsimd(x));
        vst1q_f32(&p->gf_cos[i], Sleef_cosf4_u10advsimd(x));
        x = vld1q_f32(&p->xf_exp[i]);
        vst1q_f32(&p->gf_exp[i], Sleef_expf4_u10advsimd(x));
        x = vld1q_f32(&p->xf_log[i]);
        vst1q_f32(&p->gf_log[i], Sleef_logf4_u10advsimd(x));
        /* u35 second tier */
        vst1q_f32(&p->gf_sin35[i], Sleef_sinf4_u35advsimd(vld1q_f32(&p->xf_trig[i])));
        vst1q_f32(&p->gf_cos35[i], Sleef_cosf4_u35advsimd(vld1q_f32(&p->xf_trig[i])));
        vst1q_f32(&p->gf_log35[i], Sleef_logf4_u35advsimd(vld1q_f32(&p->xf_log[i])));
        vst1q_f32(&p->gf_exp2_35[i], Sleef_exp2f4_u35advsimd(vld1q_f32(&p->xf_exp[i])));
        /* new families (asin/atan/cbrt/sinh/tanh at u35, log10/pow at u10) */
        vst1q_f32(&p->gf_asin[i], Sleef_asinf4_u35advsimd(vld1q_f32(&p->xf_inv[i])));
        vst1q_f32(&p->gf_atan[i], Sleef_atanf4_u35advsimd(vld1q_f32(&p->xf_inv[i])));
        vst1q_f32(&p->gf_cbrt[i], Sleef_cbrtf4_u35advsimd(vld1q_f32(&p->xf_cbrt[i])));
        vst1q_f32(&p->gf_log10[i], Sleef_log10f4_u10advsimd(vld1q_f32(&p->xf_log[i])));
        vst1q_f32(&p->gf_sinh[i], Sleef_sinhf4_u35advsimd(vld1q_f32(&p->xf_exp[i])));
        vst1q_f32(&p->gf_tanh[i], Sleef_tanhf4_u35advsimd(vld1q_f32(&p->xf_exp[i])));
        vst1q_f32(&p->gf_pow[i], Sleef_powf4_u10advsimd(vld1q_f32(&p->xf_pow2[i]),
                                                        vld1q_f32(&p->xf_pexp[i])));
    }
}

static int sleef_neon_init(struct test *test)
{
    auto d = new(sleef_test_data);
    test->data = d;
    /* footprint knob, mdim precedent: read once, fail loudly on bad
     * values — a silent clamp would run a different campaign than the
     * operator asked for, and a non-multiple-of-4 would overrun the
     * float32x4 stride-4 vector loops (last iteration past the buffer) */
    int64_t knob = get_testspecific_knob_value_int(test, "nelems", ELEMS_DEFAULT);
    if (knob < ELEMS_MIN || knob > ELEMS_MAX) {
        report_fail_msg("nelems knob out of range: %ld (valid %d..%d, default %d)",
                        (long)knob, ELEMS_MIN, ELEMS_MAX, ELEMS_DEFAULT);
    }
    if (knob % 4 != 0) {
        report_fail_msg("nelems knob must be a multiple of 4 (float32x4_t stride): %ld",
                        (long)knob);
    }
    d->nelems = (int)knob;
    d->xd_trig = (double *)malloc(d->nelems * sizeof(double));
    d->xd_exp  = (double *)malloc(d->nelems * sizeof(double));
    d->xd_log  = (double *)malloc(d->nelems * sizeof(double));
    d->xf_trig = (float  *)malloc(d->nelems * sizeof(float));
    d->xf_exp  = (float  *)malloc(d->nelems * sizeof(float));
    d->xf_log  = (float  *)malloc(d->nelems * sizeof(float));
    d->gd_sin = (double *)malloc(d->nelems * sizeof(double));
    d->gd_cos = (double *)malloc(d->nelems * sizeof(double));
    d->gd_exp = (double *)malloc(d->nelems * sizeof(double));
    d->gd_log = (double *)malloc(d->nelems * sizeof(double));
    d->gf_sin = (float  *)malloc(d->nelems * sizeof(float));
    d->gf_cos = (float  *)malloc(d->nelems * sizeof(float));
    d->gf_exp = (float  *)malloc(d->nelems * sizeof(float));
    d->gf_log = (float  *)malloc(d->nelems * sizeof(float));
    /* u35 tier golden outputs */
    d->gd_sin35 = (double *)malloc(d->nelems * sizeof(double));
    d->gd_cos35 = (double *)malloc(d->nelems * sizeof(double));
    d->gd_log35 = (double *)malloc(d->nelems * sizeof(double));
    d->gd_exp2_35 = (double *)malloc(d->nelems * sizeof(double));
    d->gf_sin35 = (float  *)malloc(d->nelems * sizeof(float));
    d->gf_cos35 = (float  *)malloc(d->nelems * sizeof(float));
    d->gf_log35 = (float  *)malloc(d->nelems * sizeof(float));
    d->gf_exp2_35 = (float  *)malloc(d->nelems * sizeof(float));
    /* new families: inputs */
    d->xd_inv  = (double *)malloc(d->nelems * sizeof(double));
    d->xd_cbrt = (double *)malloc(d->nelems * sizeof(double));
    d->xd_pow2 = (double *)malloc(d->nelems * sizeof(double));
    d->xd_pexp = (double *)malloc(d->nelems * sizeof(double));
    d->xf_inv  = (float  *)malloc(d->nelems * sizeof(float));
    d->xf_cbrt = (float  *)malloc(d->nelems * sizeof(float));
    d->xf_pow2 = (float  *)malloc(d->nelems * sizeof(float));
    d->xf_pexp = (float  *)malloc(d->nelems * sizeof(float));
    /* new families: golden outputs */
    d->gd_asin = (double *)malloc(d->nelems * sizeof(double));
    d->gd_atan = (double *)malloc(d->nelems * sizeof(double));
    d->gd_cbrt = (double *)malloc(d->nelems * sizeof(double));
    d->gd_log10 = (double *)malloc(d->nelems * sizeof(double));
    d->gd_sinh = (double *)malloc(d->nelems * sizeof(double));
    d->gd_tanh = (double *)malloc(d->nelems * sizeof(double));
    d->gd_pow = (double *)malloc(d->nelems * sizeof(double));
    d->gf_asin = (float  *)malloc(d->nelems * sizeof(float));
    d->gf_atan = (float  *)malloc(d->nelems * sizeof(float));
    d->gf_cbrt = (float  *)malloc(d->nelems * sizeof(float));
    d->gf_log10 = (float  *)malloc(d->nelems * sizeof(float));
    d->gf_sinh = (float  *)malloc(d->nelems * sizeof(float));
    d->gf_tanh = (float  *)malloc(d->nelems * sizeof(float));
    d->gf_pow = (float  *)malloc(d->nelems * sizeof(float));
    if (!d->xd_trig || !d->xd_exp || !d->xd_log ||
        !d->xf_trig || !d->xf_exp || !d->xf_log ||
        !d->gd_sin || !d->gd_cos || !d->gd_exp || !d->gd_log ||
        !d->gf_sin || !d->gf_cos || !d->gf_exp || !d->gf_log ||
        !d->gd_sin35 || !d->gd_cos35 || !d->gd_log35 || !d->gd_exp2_35 ||
        !d->gf_sin35 || !d->gf_cos35 || !d->gf_log35 || !d->gf_exp2_35 ||
        !d->xd_inv || !d->xd_cbrt || !d->xd_pow2 || !d->xd_pexp ||
        !d->xf_inv || !d->xf_cbrt || !d->xf_pow2 || !d->xf_pexp ||
        !d->gd_asin || !d->gd_atan || !d->gd_cbrt || !d->gd_log10 ||
        !d->gd_sinh || !d->gd_tanh || !d->gd_pow ||
        !d->gf_asin || !d->gf_atan || !d->gf_cbrt || !d->gf_log10 ||
        !d->gf_sinh || !d->gf_tanh || !d->gf_pow) {
        report_fail_msg("OOM in sleef_neon init");
    }

    /* fill inputs through the shared helper — init's pass stays only as
     * the init-time finiteness canary (B2-006); the run loop re-rolls its
     * own per-thread copy every kRerollEvery iterations */
    sleef_fill_inputs(d);

    /* golden outputs from the very same kernels, once; any non-finite
     * result would poison the byte-exact comparison (NaN != NaN), so fail
     * loudly at the source instead (same tripwire as openblas_dgemm) */
    sleef_compute_goldens(d);
    for (int i = 0; i < d->nelems; ++i) {
        double gs = d->gd_sin[i], gc = d->gd_cos[i];
        double ge = d->gd_exp[i], gl = d->gd_log[i];
        if (gs != gs || gs > 1.0 || gs < -1.0 || gc != gc || gc > 1.0 || gc < -1.0 ||
            ge != ge || ge > 1.0e300 || ge < 0.0 ||
            gl != gl || gl > 1.0e300 || gl < -1.0e300) {
            report_fail_msg("double golden value not finite/in-range at element %d "
                            "(domain mapping needs tighter scaling)", i);
        }
    }
    for (int i = 0; i < d->nelems; ++i) {
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
    for (int i = 0; i < d->nelems; ++i) {
        double gs = d->gd_sin35[i], gc = d->gd_cos35[i];
        double gl = d->gd_log35[i], ge = d->gd_exp2_35[i];
        if (gs != gs || gs > 1.0 || gs < -1.0 || gc != gc || gc > 1.0 || gc < -1.0 ||
            gl != gl || gl > 1.0e300 || gl < -1.0e300 ||
            ge != ge || ge > 1.0e7 || ge < 0.0) {
            report_fail_msg("double u35 golden value not finite/in-range at element %d "
                            "(domain mapping needs tighter scaling)", i);
        }
    }
    for (int i = 0; i < d->nelems; ++i) {
        float gs = d->gf_sin35[i], gc = d->gf_cos35[i];
        float gl = d->gf_log35[i], ge = d->gf_exp2_35[i];
        if (gs != gs || gs > 1.0f || gs < -1.0f || gc != gc || gc > 1.0f || gc < -1.0f ||
            gl != gl || gl > 1.0e30f || gl < -1.0e30f ||
            ge != ge || ge > 1.0e7f || ge < 0.0f) {
            report_fail_msg("float u35 golden value not finite/in-range at element %d "
                            "(domain mapping needs tighter scaling)", i);
        }
    }
    /* finiteness sentinels, new families:
     *  - asin/atan: |g| <= 1.6 (pi/2 ~ 1.5708 plus headroom; atan can only
     *    approach pi/2 but the shared bound keeps it simple)
     *  - cbrt: |g| < 10 (cbrt(1000)=10, not included), sentinel 1.0e3
     *  - log10: (-700, 25] double / (-30, 25] float (domain is
     *    (-290, 10] / (-20, 10], generous headroom)
     *  - sinh: |g| < 1e9 (sinh(20) ~ 2.42e8)
     *  - tanh: |g| <= 1 INCLUSIVE — SLEEF clamps to exactly 1.0 for
     *    |x| > 18.714973875 (see boundary analysis; deterministic)
     *  - pow: g in (0, 1e4] (pow(10,4)=1e4 is the theoretical max, not
     *    reached; strictly positive since base >= 1 > 0) */
    for (int i = 0; i < d->nelems; ++i) {
        double ga = d->gd_asin[i], gt = d->gd_atan[i], gc = d->gd_cbrt[i];
        double gl10 = d->gd_log10[i], gsh = d->gd_sinh[i], gth = d->gd_tanh[i], gp = d->gd_pow[i];
        if (ga != ga || ga > 1.6 || ga < -1.6 ||
            gt != gt || gt > 1.6 || gt < -1.6 ||
            gc != gc || gc > 1.0e3 || gc < -1.0e3 ||
            gl10 != gl10 || gl10 > 25.0 || gl10 < -700.0 ||
            gsh != gsh || gsh > 1.0e9 || gsh < -1.0e9 ||
            gth != gth || gth > 1.0 || gth < -1.0 ||
            gp != gp || gp > 1.0e4 || gp <= 0.0) {
            report_fail_msg("double new-family golden value not finite/in-range at element %d "
                            "(domain mapping needs tighter scaling)", i);
        }
    }
    for (int i = 0; i < d->nelems; ++i) {
        float ga = d->gf_asin[i], gt = d->gf_atan[i], gc = d->gf_cbrt[i];
        float gl10 = d->gf_log10[i], gsh = d->gf_sinh[i], gth = d->gf_tanh[i], gp = d->gf_pow[i];
        if (ga != ga || ga > 1.6f || ga < -1.6f ||
            gt != gt || gt > 1.6f || gt < -1.6f ||
            gc != gc || gc > 1.0e3f || gc < -1.0e3f ||
            gl10 != gl10 || gl10 > 25.0f || gl10 < -30.0f ||
            gsh != gsh || gsh > 1.0e9f || gsh < -1.0e9f ||
            gth != gth || gth > 1.0f || gth < -1.0f ||
            gp != gp || gp > 1.0e4f || gp <= 0.0f) {
            report_fail_msg("float new-family golden value not finite/in-range at element %d "
                            "(domain mapping needs tighter scaling)", i);
        }
    }
    return EXIT_SUCCESS;
}

static int sleef_neon_run(struct test *test, int cpu)
{
    auto d = CAST(test->data);
    constexpr int kRerollEvery = 16;
    int since_reroll = kRerollEvery; /* force the first-iteration fill */
    TEST_LOOP(test, 1) {
        /* lazily allocate this thread's scratch output buffers and its own
         * sleef_test_data (B2-006: run never reads the shared test->data
         * arrays — the nelems knob scalar alone is copied once from the
         * shared struct, mdim/beta precedent) */
        if (__builtin_expect(!od || !of || !l, 0)) {
            od = (double *)calloc(1, d->nelems * sizeof(double));
            of = (float  *)calloc(1, d->nelems * sizeof(float));
            l = (struct sleef_test_data *)malloc(sizeof(*l));
            if (l) {
                l->nelems = d->nelems;
                l->xd_trig = (double *)malloc(l->nelems * sizeof(double));
                l->xd_exp  = (double *)malloc(l->nelems * sizeof(double));
                l->xd_log  = (double *)malloc(l->nelems * sizeof(double));
                l->xf_trig = (float  *)malloc(l->nelems * sizeof(float));
                l->xf_exp  = (float  *)malloc(l->nelems * sizeof(float));
                l->xf_log  = (float  *)malloc(l->nelems * sizeof(float));
                l->gd_sin = (double *)malloc(l->nelems * sizeof(double));
                l->gd_cos = (double *)malloc(l->nelems * sizeof(double));
                l->gd_exp = (double *)malloc(l->nelems * sizeof(double));
                l->gd_log = (double *)malloc(l->nelems * sizeof(double));
                l->gf_sin = (float  *)malloc(l->nelems * sizeof(float));
                l->gf_cos = (float  *)malloc(l->nelems * sizeof(float));
                l->gf_exp = (float  *)malloc(l->nelems * sizeof(float));
                l->gf_log = (float  *)malloc(l->nelems * sizeof(float));
                l->gd_sin35 = (double *)malloc(l->nelems * sizeof(double));
                l->gd_cos35 = (double *)malloc(l->nelems * sizeof(double));
                l->gd_log35 = (double *)malloc(l->nelems * sizeof(double));
                l->gd_exp2_35 = (double *)malloc(l->nelems * sizeof(double));
                l->gf_sin35 = (float  *)malloc(l->nelems * sizeof(float));
                l->gf_cos35 = (float  *)malloc(l->nelems * sizeof(float));
                l->gf_log35 = (float  *)malloc(l->nelems * sizeof(float));
                l->gf_exp2_35 = (float  *)malloc(l->nelems * sizeof(float));
                l->xd_inv  = (double *)malloc(l->nelems * sizeof(double));
                l->xd_cbrt = (double *)malloc(l->nelems * sizeof(double));
                l->xd_pow2 = (double *)malloc(l->nelems * sizeof(double));
                l->xd_pexp = (double *)malloc(l->nelems * sizeof(double));
                l->xf_inv  = (float  *)malloc(l->nelems * sizeof(float));
                l->xf_cbrt = (float  *)malloc(l->nelems * sizeof(float));
                l->xf_pow2 = (float  *)malloc(l->nelems * sizeof(float));
                l->xf_pexp = (float  *)malloc(l->nelems * sizeof(float));
                l->gd_asin = (double *)malloc(l->nelems * sizeof(double));
                l->gd_atan = (double *)malloc(l->nelems * sizeof(double));
                l->gd_cbrt = (double *)malloc(l->nelems * sizeof(double));
                l->gd_log10 = (double *)malloc(l->nelems * sizeof(double));
                l->gd_sinh = (double *)malloc(l->nelems * sizeof(double));
                l->gd_tanh = (double *)malloc(l->nelems * sizeof(double));
                l->gd_pow = (double *)malloc(l->nelems * sizeof(double));
                l->gf_asin = (float  *)malloc(l->nelems * sizeof(float));
                l->gf_atan = (float  *)malloc(l->nelems * sizeof(float));
                l->gf_cbrt = (float  *)malloc(l->nelems * sizeof(float));
                l->gf_log10 = (float  *)malloc(l->nelems * sizeof(float));
                l->gf_sinh = (float  *)malloc(l->nelems * sizeof(float));
                l->gf_tanh = (float  *)malloc(l->nelems * sizeof(float));
                l->gf_pow = (float  *)malloc(l->nelems * sizeof(float));
            }
            /* short-circuit || leaves the l-> member checks unevaluated
             * when l itself failed to allocate; report_fail_msg is
             * noreturn so the buffers are valid past this point */
            if (!od || !of || !l ||
                !l->xd_trig || !l->xd_exp || !l->xd_log ||
                !l->xf_trig || !l->xf_exp || !l->xf_log ||
                !l->gd_sin || !l->gd_cos || !l->gd_exp || !l->gd_log ||
                !l->gf_sin || !l->gf_cos || !l->gf_exp || !l->gf_log ||
                !l->gd_sin35 || !l->gd_cos35 || !l->gd_log35 || !l->gd_exp2_35 ||
                !l->gf_sin35 || !l->gf_cos35 || !l->gf_log35 || !l->gf_exp2_35 ||
                !l->xd_inv || !l->xd_cbrt || !l->xd_pow2 || !l->xd_pexp ||
                !l->xf_inv || !l->xf_cbrt || !l->xf_pow2 || !l->xf_pexp ||
                !l->gd_asin || !l->gd_atan || !l->gd_cbrt || !l->gd_log10 ||
                !l->gd_sinh || !l->gd_tanh || !l->gd_pow ||
                !l->gf_asin || !l->gf_atan || !l->gf_cbrt || !l->gf_log10 ||
                !l->gf_sinh || !l->gf_tanh || !l->gf_pow) {
                report_fail_msg("OOM allocating thread data (%zu bytes)",
                                (size_t)d->nelems * (22 * sizeof(double) +
                                                     22 * sizeof(float) +
                                                     sizeof(double) + sizeof(float)) +
                                sizeof(*l));
            }
        }

        /* B2-006 reroll: fresh framework-RNG inputs and goldens recomputed
         * with the very same kernels every kRerollEvery iterations
         * (46141ed4 Eigen pattern) — the init-frozen shared arrays are
         * never replayed */
        if (++since_reroll >= kRerollEvery) {
            since_reroll = 0;
            sleef_fill_inputs(l);
            sleef_compute_goldens(l);
        }

        /* compute-out: each family replays its own domain inputs through
         * the same kernels and lands in the thread-private scratch */
        for (int i = 0; i < l->nelems; i += 2) {
            float64x2_t x = vld1q_f64(&l->xd_trig[i]);
            vst1q_f64(&od[i], Sleef_sind2_u10advsimd(x));
        }
        memcmp_or_fail(od, l->gd_sin, l->nelems, "sin double");

        for (int i = 0; i < l->nelems; i += 2) {
            float64x2_t x = vld1q_f64(&l->xd_trig[i]);
            vst1q_f64(&od[i], Sleef_cosd2_u10advsimd(x));
        }
        memcmp_or_fail(od, l->gd_cos, l->nelems, "cos double");

        for (int i = 0; i < l->nelems; i += 2) {
            float64x2_t x = vld1q_f64(&l->xd_exp[i]);
            vst1q_f64(&od[i], Sleef_expd2_u10advsimd(x));
        }
        memcmp_or_fail(od, l->gd_exp, l->nelems, "exp double");

        for (int i = 0; i < l->nelems; i += 2) {
            float64x2_t x = vld1q_f64(&l->xd_log[i]);
            vst1q_f64(&od[i], Sleef_logd2_u10advsimd(x));
        }
        memcmp_or_fail(od, l->gd_log, l->nelems, "log double");

        for (int i = 0; i < l->nelems; i += 4) {
            float32x4_t x = vld1q_f32(&l->xf_trig[i]);
            vst1q_f32(&of[i], Sleef_sinf4_u10advsimd(x));
        }
        memcmp_or_fail(of, l->gf_sin, l->nelems, "sin float");

        for (int i = 0; i < l->nelems; i += 4) {
            float32x4_t x = vld1q_f32(&l->xf_trig[i]);
            vst1q_f32(&of[i], Sleef_cosf4_u10advsimd(x));
        }
        memcmp_or_fail(of, l->gf_cos, l->nelems, "cos float");

        for (int i = 0; i < l->nelems; i += 4) {
            float32x4_t x = vld1q_f32(&l->xf_exp[i]);
            vst1q_f32(&of[i], Sleef_expf4_u10advsimd(x));
        }
        memcmp_or_fail(of, l->gf_exp, l->nelems, "exp float");

        for (int i = 0; i < l->nelems; i += 4) {
            float32x4_t x = vld1q_f32(&l->xf_log[i]);
            vst1q_f32(&of[i], Sleef_logf4_u10advsimd(x));
        }
        memcmp_or_fail(of, l->gf_log, l->nelems, "log float");

        /* u35 second-accuracy tier: same inputs, other polynomial chain */
        for (int i = 0; i < l->nelems; i += 2) {
            float64x2_t x = vld1q_f64(&l->xd_trig[i]);
            vst1q_f64(&od[i], Sleef_sind2_u35advsimd(x));
        }
        memcmp_or_fail(od, l->gd_sin35, l->nelems, "sin35 double");

        for (int i = 0; i < l->nelems; i += 2) {
            float64x2_t x = vld1q_f64(&l->xd_trig[i]);
            vst1q_f64(&od[i], Sleef_cosd2_u35advsimd(x));
        }
        memcmp_or_fail(od, l->gd_cos35, l->nelems, "cos35 double");

        for (int i = 0; i < l->nelems; i += 2) {
            float64x2_t x = vld1q_f64(&l->xd_log[i]);
            vst1q_f64(&od[i], Sleef_logd2_u35advsimd(x));
        }
        memcmp_or_fail(od, l->gd_log35, l->nelems, "log35 double");

        for (int i = 0; i < l->nelems; i += 2) {
            float64x2_t x = vld1q_f64(&l->xd_exp[i]);
            vst1q_f64(&od[i], Sleef_exp2d2_u35advsimd(x));
        }
        memcmp_or_fail(od, l->gd_exp2_35, l->nelems, "exp2_35 double");

        for (int i = 0; i < l->nelems; i += 4) {
            float32x4_t x = vld1q_f32(&l->xf_trig[i]);
            vst1q_f32(&of[i], Sleef_sinf4_u35advsimd(x));
        }
        memcmp_or_fail(of, l->gf_sin35, l->nelems, "sin35 float");

        for (int i = 0; i < l->nelems; i += 4) {
            float32x4_t x = vld1q_f32(&l->xf_trig[i]);
            vst1q_f32(&of[i], Sleef_cosf4_u35advsimd(x));
        }
        memcmp_or_fail(of, l->gf_cos35, l->nelems, "cos35 float");

        for (int i = 0; i < l->nelems; i += 4) {
            float32x4_t x = vld1q_f32(&l->xf_log[i]);
            vst1q_f32(&of[i], Sleef_logf4_u35advsimd(x));
        }
        memcmp_or_fail(of, l->gf_log35, l->nelems, "log35 float");

        for (int i = 0; i < l->nelems; i += 4) {
            float32x4_t x = vld1q_f32(&l->xf_exp[i]);
            vst1q_f32(&of[i], Sleef_exp2f4_u35advsimd(x));
        }
        memcmp_or_fail(of, l->gf_exp2_35, l->nelems, "exp2_35 float");

        /* new families: asin/atan/cbrt/log10/sinh/tanh/pow */
        for (int i = 0; i < l->nelems; i += 2) {
            float64x2_t x = vld1q_f64(&l->xd_inv[i]);
            vst1q_f64(&od[i], Sleef_asind2_u35advsimd(x));
        }
        memcmp_or_fail(od, l->gd_asin, l->nelems, "asin double");

        for (int i = 0; i < l->nelems; i += 2) {
            float64x2_t x = vld1q_f64(&l->xd_inv[i]);
            vst1q_f64(&od[i], Sleef_atand2_u35advsimd(x));
        }
        memcmp_or_fail(od, l->gd_atan, l->nelems, "atan double");

        for (int i = 0; i < l->nelems; i += 2) {
            float64x2_t x = vld1q_f64(&l->xd_cbrt[i]);
            vst1q_f64(&od[i], Sleef_cbrtd2_u35advsimd(x));
        }
        memcmp_or_fail(od, l->gd_cbrt, l->nelems, "cbrt double");

        for (int i = 0; i < l->nelems; i += 2) {
            float64x2_t x = vld1q_f64(&l->xd_log[i]);
            vst1q_f64(&od[i], Sleef_log10d2_u10advsimd(x));
        }
        memcmp_or_fail(od, l->gd_log10, l->nelems, "log10 double");

        for (int i = 0; i < l->nelems; i += 2) {
            float64x2_t x = vld1q_f64(&l->xd_exp[i]);
            vst1q_f64(&od[i], Sleef_sinhd2_u35advsimd(x));
        }
        memcmp_or_fail(od, l->gd_sinh, l->nelems, "sinh double");

        for (int i = 0; i < l->nelems; i += 2) {
            float64x2_t x = vld1q_f64(&l->xd_exp[i]);
            vst1q_f64(&od[i], Sleef_tanhd2_u35advsimd(x));
        }
        memcmp_or_fail(od, l->gd_tanh, l->nelems, "tanh double");

        for (int i = 0; i < l->nelems; i += 2) {
            float64x2_t b = vld1q_f64(&l->xd_pow2[i]);
            float64x2_t e = vld1q_f64(&l->xd_pexp[i]);
            vst1q_f64(&od[i], Sleef_powd2_u10advsimd(b, e));
        }
        memcmp_or_fail(od, l->gd_pow, l->nelems, "pow double");

        for (int i = 0; i < l->nelems; i += 4) {
            float32x4_t x = vld1q_f32(&l->xf_inv[i]);
            vst1q_f32(&of[i], Sleef_asinf4_u35advsimd(x));
        }
        memcmp_or_fail(of, l->gf_asin, l->nelems, "asin float");

        for (int i = 0; i < l->nelems; i += 4) {
            float32x4_t x = vld1q_f32(&l->xf_inv[i]);
            vst1q_f32(&of[i], Sleef_atanf4_u35advsimd(x));
        }
        memcmp_or_fail(of, l->gf_atan, l->nelems, "atan float");

        for (int i = 0; i < l->nelems; i += 4) {
            float32x4_t x = vld1q_f32(&l->xf_cbrt[i]);
            vst1q_f32(&of[i], Sleef_cbrtf4_u35advsimd(x));
        }
        memcmp_or_fail(of, l->gf_cbrt, l->nelems, "cbrt float");

        for (int i = 0; i < l->nelems; i += 4) {
            float32x4_t x = vld1q_f32(&l->xf_log[i]);
            vst1q_f32(&of[i], Sleef_log10f4_u10advsimd(x));
        }
        memcmp_or_fail(of, l->gf_log10, l->nelems, "log10 float");

        for (int i = 0; i < l->nelems; i += 4) {
            float32x4_t x = vld1q_f32(&l->xf_exp[i]);
            vst1q_f32(&of[i], Sleef_sinhf4_u35advsimd(x));
        }
        memcmp_or_fail(of, l->gf_sinh, l->nelems, "sinh float");

        for (int i = 0; i < l->nelems; i += 4) {
            float32x4_t x = vld1q_f32(&l->xf_exp[i]);
            vst1q_f32(&of[i], Sleef_tanhf4_u35advsimd(x));
        }
        memcmp_or_fail(of, l->gf_tanh, l->nelems, "tanh float");

        for (int i = 0; i < l->nelems; i += 4) {
            float32x4_t b = vld1q_f32(&l->xf_pow2[i]);
            float32x4_t e = vld1q_f32(&l->xf_pexp[i]);
            vst1q_f32(&of[i], Sleef_powf4_u10advsimd(b, e));
        }
        memcmp_or_fail(of, l->gf_pow, l->nelems, "pow float");
    }
    return EXIT_SUCCESS;
}

static int sleef_neon_cleanup(struct test *test)
{
    auto d = CAST(test->data);
    free(d->xd_trig); free(d->xd_exp); free(d->xd_log);
    free(d->xf_trig); free(d->xf_exp); free(d->xf_log);
    free(d->gd_sin); free(d->gd_cos); free(d->gd_exp); free(d->gd_log);
    free(d->gf_sin); free(d->gf_cos); free(d->gf_exp); free(d->gf_log);
    free(d->gd_sin35); free(d->gd_cos35); free(d->gd_log35); free(d->gd_exp2_35);
    free(d->gf_sin35); free(d->gf_cos35); free(d->gf_log35); free(d->gf_exp2_35);
    free(d->xd_inv); free(d->xd_cbrt); free(d->xd_pow2); free(d->xd_pexp);
    free(d->xf_inv); free(d->xf_cbrt); free(d->xf_pow2); free(d->xf_pexp);
    free(d->gd_asin); free(d->gd_atan); free(d->gd_cbrt); free(d->gd_log10);
    free(d->gd_sinh); free(d->gd_tanh); free(d->gd_pow);
    free(d->gf_asin); free(d->gf_atan); free(d->gf_cbrt); free(d->gf_log10);
    free(d->gf_sinh); free(d->gf_tanh); free(d->gf_pow);
    delete d;
    return EXIT_SUCCESS;
}

DECLARE_TEST(sleef_neon, "SLEEF vectorized transcendentals (NEON polynomial FMA chains, sin/cos/exp/log + u35 tier (exp2) + asin/atan/cbrt/log10/sinh/tanh/pow, double+float; footprint knob -O sleef_neon.nelems=N, 128..262144, default 1024)")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = sleef_neon_init,
  .test_run = sleef_neon_run,
  .test_cleanup = sleef_neon_cleanup,
  .fracture_loop_count = 4,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
