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
 * float32 tier and the u35
 * second-accuracy chains (sin/cos/log/exp2 — u35 SVE has no exp, exp2
 * is its sibling with an independent polynomial chain). Long polynomial
 * FMA dependency chains; results only feed a byte-exact golden compare
 * (data-flow payload: faults surface as wrong values, not crashes —
 * From Gates to SDCs DATE'25). Pure functions => deterministic golden
 * values, recomputed from freshly re-rolled inputs every kRerollEvery
 * iterations with the same kernels (per-thread, framework RNG — B2-006);
 * once per fracture (round 3 of every 4) a frozen-replay round runs on
 * init's shared inputs/goldens, so the init-time golden is actually
 * re-compared instead of staying only the finiteness canary. Every SVE
 * loop steps
 * by svcntd()/svcntw() with svwhilelt tail truncation, which makes the
 * loop indices and tails exact at EVERY runtime vector length — the TU
 * is true VLA (all-VL Phase 2 P5: compiled in the dedicated tests_sve_vla
 * library with -march=armv8.2-a+sve and NO -msve-vector-bits, so the
 * sizeless svfloat64_t/svfloat32_t types adopt the CPU's runtime VL; the
 * pre-P5 128-bit lock and its ACLE intersection caveat are retired).
 * Spike evidence: three-state compile probe (128 / 256 / no flag) with
 * zero warnings, and the vendored SLEEF 3.9.0 static library is pure VLA
 * machine code (disassembly: cntd/cntp/rdvl, no ptrue_pat fixed-width
 * patterns); the historical "VL=256 compile regression" is most
 * plausibly experiment contamination (the whole-library flag flip
 * recompiled Eigen's fixed-width TUs, whose heap corruption got
 * attributed to sleef). Cross-tier byte-identity of the per-lane-pure
 * kernels is judged by the gem5-fi sleef_vla_dual 16-run matrix (four
 * tiers x {fixed, random x3}; kernel in place, matrix pending at this
 * commit — to be recorded on the gem5-fi side when run). u10 and u35
 * are
 * SLEEF's two INDEPENDENT implementations of the same mathematics
 * (different coefficients, different FMA-chain schedules), so the
 * second tier is a free second-phase sample of the same silicon for
 * the same math. Covers the newest-instruction-generation risk area
 * (PinDrop Obs12: instructions fail most in their first arch gen) —
 * SVE is the newest-generation vector datapath on ARM64. Compiled in
 * the dedicated tests_sve_vla true-VLA static library
 * (-march=armv8.2-a+sve, no -msve-vector-bits) so the SVE instructions
 * never leak into the NEON baseline build.
 *
 * The per-family element count is runtime-configurable through the
 * unified stress-config chain: "-O sleef_sve.nelems=N" or a config-file
 * entry (128..64M, multiples of 4 — out-of-range values warn and clamp,
 * non-multiples of 4 warn and round down), the SDC_SLEEF_SVE_NELEMS
 * environment variable, or a memory-adaptive default (18 shared arrays
 * + 2 per-thread scratch buffers, modeled as ~20 double-sized linear
 * buffers per thread at 60% of physical memory) when neither is set.
 * Configuring nelems=1024 reproduces the historical compile-time
 * ELEMS=1024 behavior exactly (same RNG consumption order,
 * byte-identical inputs/goldens).
 *
 * Runtime gate: silicon without SVE (e.g. Kunpeng 920) skips cleanly
 * (CpuNotSupported / EXIT_SKIP) via the framework-layer
 * arm64_require_sve_vl helper (HWCAP_SVE capability + 128..1024-bit
 * range check in one call, all-VL Phase 2 P5). Real execution requires
 * SVE hardware (Kunpeng 930 / Neoverse) — documented limitation of this
 * development host; on SVE silicon the test runs the full payload at
 * the hardware's own vector length.
 * @endparblock
 */

#include <sandstone.h>
#include "sdc_stress_config.h"

#include <sleef.h>

#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#include <string.h>
#include <stdlib.h>

/* Elements per family — a RUNTIME knob resolved through the unified
 * stress-config chain (see sleef_sve_nelems below): every loop bound
 * and buffer size in this TU derives from it. Every SVE loop steps by
 * svcntd()/svcntw() and builds its load/store predicate with
 * svwhilelt_b64/b32(i, nelems) inside the loop, so the loop indices and
 * tails are exact at EVERY runtime vector length (all-VL Phase 2 P5:
 * this TU compiles in the true-VLA tests_sve_vla library with no
 * -msve-vector-bits; a longer VL just takes longer vectors per
 * iteration, a shorter one takes more of them — same bytes either way,
 * the kernels are per-lane pure). Spike evidence: three-state compile
 * probe (128/256/no flag) zero warnings; the vendored SLEEF library is
 * pure VLA machine code (disassembly: cntd/cntp/rdvl, no ptrue_pat). */
#define ELEMS_MIN 128        /* multiple of 4 — an efficiency nicety at the 128-bit tier (f32 tier steps 4 lanes), NOT a correctness requirement: svwhilelt tails keep any count exact at every VL */
#define ELEMS_MAX (64 << 20) /* per-family element ceiling of the nelems knob */

namespace {
struct sleef_test_data {
    /* elements per family, from the unified config chain (see
     * sleef_sve_nelems): every loop bound and buffer size below derives
     * from it, so the whole working set (the 18 shared arrays + the
     * per-thread od/of scratch) scales with the knob — run threads share
     * test->data, so the value is resolved once in init, never per
     * thread */
    int nelems;
    /* init-time SVE VL in bytes (the arm64_require_sve_vl return) — the
     * run-side drift-guard anchor (all-VL policy #3). The per-lane-pure
     * kernels make the goldens VL-independent, so this is a fail-closed
     * environment invariant, not geometry. Only test->data (init's
     * shared copy) ever reads it. */
    size_t vl_bytes;
    /* per-domain inputs; the run loop keeps its OWN per-thread re-rolled
     * copy (B2-006) — the shared arrays are filled and golden-checked
     * once in init as the finiteness canary, then replayed once per
     * fracture by the run loop's frozen-replay round (each family keeps
     * its own array so the run loop can replay exactly the inputs the
     * golden values were computed from) */
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

/* 每家族元素数 nelems — 统一配置链:
 *   1. -O sleef_sve.nelems=N / 配置文件同名条目 (128..64M, 越界告警并 clamp)
 *   2. 环境变量 SDC_SLEEF_SVE_NELEMS
 *   3. 自适应默认: 18 个全局共享数组 + 每线程 od/of 2 个 scratch, 按
 *      double 8B/元素 × 20 个线性缓冲/线程计, 占物理内存 60%,
 *      clamp [128, 64M]。线性缓冲用 sdc_auto_linear_count (误用方阵版
 *      会把压力欠缩放 sqrt(n) 倍)。
 * 任何显式值必须是 4 的倍数 (尾谓词安全表述: svwhilelt 使任意计数在
 * 所有档位精确, 4 倍数只是让 128-bit 档的 f32 阶 (每步 4 lane) 无尾——
 * 效率修饰而非正确性要求, 见上), 非 4 倍数 log_warning 并向下对齐;
 * 自适应值静默对齐 (其下限 128 本身是 4 的倍数, 对齐不会跌破)。 */
static size_t sleef_sve_nelems(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "nelems", 0, ELEMS_MIN, ELEMS_MAX);
    if (v == 0)
        sdc_env_knob_int_clamped("sleef_sve", "nelems", &v, ELEMS_MIN, ELEMS_MAX);
    if (v > 0) {
        if (v % 4 != 0) {
            log_warning("sleef_sve: nelems=%ld is not a multiple of 4 "
                        "(alignment nicety; svwhilelt tails keep any "
                        "count exact at every VL) — "
                        "rounding down to %ld",
                        (long)v, (long)(v - v % 4));
            v -= v % 4;
        }
        if (v < ELEMS_MIN)
            v = ELEMS_MIN;
        return (size_t)v;
    }
    return (size_t)(sdc_auto_linear_count(8, 20, 1.0, 600, thread_count(),
                                          ELEMS_MIN, ELEMS_MAX, test->id, "nelems") & ~(int64_t)3);
}

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
 * test->data inputs/goldens are otherwise only the init-time finiteness
 * canary, so the run loop's once-per-fracture frozen-replay round
 * switches the kernel input/golden source to them (the SLEEF SVE
 * kernels only READ their inputs — svld1 loads — and write the
 * per-thread od/of scratch, so the shared snapshot can be replayed
 * without copying it into l — the reroll state is never touched) to
 * actually re-verify the frozen golden (the same-kernel reroll alone
 * cannot catch a mercurial core defect). */
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
 * in both places. Element count from the struct's nelems knob. */
static void sleef_fill_inputs(struct sleef_test_data *p)
{
    /* double inputs, per domain (same mappings and boundary analysis as sleef_neon) */
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
}

/* Compute the golden outputs from p's inputs with the very same SVE
 * kernels the run loop verifies against — the shared-helper structure is
 * what keeps the byte-exact comparison valid across rerolls. Steps by
 * svcntd()/svcntw() with svwhilelt tail-truncated predicates, exact at
 * every runtime VL (see the nelems comment above). */
static void sleef_compute_goldens(struct sleef_test_data *p)
{
    for (int i = 0; i < p->nelems; i += svcntd()) {
        const svbool_t pg = svwhilelt_b64((uint64_t)i, (uint64_t)p->nelems);
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
    for (int i = 0; i < p->nelems; i += svcntw()) {
        const svbool_t pgf = svwhilelt_b32((uint64_t)i, (uint64_t)p->nelems);
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
    /* All-VL Phase 2 P5: framework-layer capability + 128..1024-bit range
     * gate in one call (framework/device/cpu/arm64/arm64_vl.h), replacing
     * the getauxval hand-probe. NOTE: device_has_feature(cpu_feature_sve)
     * is useless here — it is defined as (device_compiler_features & f) ||
     * (device_features & f) and this TU is compiled with +sve, so the
     * compiler half would make it always-true on any CPU;
     * arm64_require_sve_vl probes HWCAP_SVE directly and reads rdvl — no
     * SVE instruction executes on the no-SVE path, so it is safe before
     * any SVE code. The framework-level compiler_minimum_device gate (the
     * "test compiled with sve" skip) already catches non-SVE hosts before
     * init; this explicit gate keeps a sleef_sve-specific skip reason even
     * if that framework gate ordering ever changes. Any tier in range is
     * in-domain: the TU and the vendored SLEEF library are both pure VLA
     * (spike evidence in the file header). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    auto d = new(sleef_test_data);
    test->data = d;
    d->vl_bytes = (size_t)svl;   /* run-side drift-guard anchor */
    d->nelems = (int)sleef_sve_nelems(test);
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
    if (!d->xd_trig || !d->xd_exp || !d->xd_log ||
        !d->xf_trig || !d->xf_exp || !d->xf_log ||
        !d->gd_sin || !d->gd_cos || !d->gd_exp || !d->gd_log ||
        !d->gf_sin || !d->gf_cos || !d->gf_exp || !d->gf_log ||
        !d->gd_sin35 || !d->gd_cos35 || !d->gd_log35 || !d->gd_exp2_35) {
        report_fail_msg("OOM in sleef_sve init");
    }

    /* fill inputs through the shared helper (bounded by d->nelems) —
     * init's pass stays only as the init-time finiteness canary
     * (B2-006); the run loop re-rolls its own per-thread copy every
     * kRerollEvery iterations */
    sleef_fill_inputs(d);

    /* golden outputs from the very same SVE kernels, once; any non-finite
     * result would poison the byte-exact comparison (NaN != NaN), so fail
     * loudly at the source instead (same tripwire as sleef_neon) */
    sleef_compute_goldens(d);
    for (int i = 0; i < d->nelems; ++i) {
        double gs = d->gd_sin[i], gc = d->gd_cos[i];
        double ge = d->gd_exp[i], gl = d->gd_log[i];
        if (gs != gs || gs > 1.0 || gs < -1.0 || gc != gc || gc > 1.0 || gc < -1.0 ||
            ge != ge || ge > 1.0e300 || ge < 0.0 ||
            gl != gl || gl > 1.0e300 || gl < -1.0e300) {
            report_fail_msg("golden value not finite/in-range at element %d "
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
    return EXIT_SUCCESS;
}

static int sleef_sve_run(struct test *test, int cpu)
{
    auto d = CAST(test->data);       /* read-only, for the frozen replay round */

    /* All-VL policy #3: fail closed on init/run VL drift. The kernels are
     * per-lane pure so the goldens are VL-independent, but a VL change on
     * this thread between init and run would be an environment anomaly
     * worth failing loudly on. */
    if (svcntb() != d->vl_bytes) {
        log_error("sleef_sve: SVE VL drifted between init (%zu bytes) and "
                  "run (%u bytes)", d->vl_bytes, (unsigned)svcntb());
        report_fail_msg("sleef_sve: SVE VL drifted between init and run");
    }

    constexpr int kRerollEvery = 16;
    int since_reroll = kRerollEvery; /* force the first-iteration fill */
    long iter = 0;
    TEST_LOOP(test, 1) {
        /* lazily allocate this thread's scratch output buffers and its own
         * sleef_test_data (B2-006: run owns its re-rolled copy; the shared
         * test->data is read only by the once-per-fracture frozen-replay
         * round); sizes come from the shared d->nelems knob */
        if (__builtin_expect(!od || !of || !l, 0)) {
            od = (double *)calloc(1, (size_t)d->nelems * sizeof(double));
            of = (float  *)calloc(1, (size_t)d->nelems * sizeof(float));
            l = (struct sleef_test_data *)malloc(sizeof(*l));
            if (l) {
                l->nelems = d->nelems;
                l->xd_trig = (double *)malloc((size_t)d->nelems * sizeof(double));
                l->xd_exp  = (double *)malloc((size_t)d->nelems * sizeof(double));
                l->xd_log  = (double *)malloc((size_t)d->nelems * sizeof(double));
                l->xf_trig = (float  *)malloc((size_t)d->nelems * sizeof(float));
                l->xf_exp  = (float  *)malloc((size_t)d->nelems * sizeof(float));
                l->xf_log  = (float  *)malloc((size_t)d->nelems * sizeof(float));
                l->gd_sin = (double *)malloc((size_t)d->nelems * sizeof(double));
                l->gd_cos = (double *)malloc((size_t)d->nelems * sizeof(double));
                l->gd_exp = (double *)malloc((size_t)d->nelems * sizeof(double));
                l->gd_log = (double *)malloc((size_t)d->nelems * sizeof(double));
                l->gf_sin = (float  *)malloc((size_t)d->nelems * sizeof(float));
                l->gf_cos = (float  *)malloc((size_t)d->nelems * sizeof(float));
                l->gf_exp = (float  *)malloc((size_t)d->nelems * sizeof(float));
                l->gf_log = (float  *)malloc((size_t)d->nelems * sizeof(float));
                l->gd_sin35 = (double *)malloc((size_t)d->nelems * sizeof(double));
                l->gd_cos35 = (double *)malloc((size_t)d->nelems * sizeof(double));
                l->gd_log35 = (double *)malloc((size_t)d->nelems * sizeof(double));
                l->gd_exp2_35 = (double *)malloc((size_t)d->nelems * sizeof(double));
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
                                (size_t)d->nelems * (11 * sizeof(double) + 7 * sizeof(float) +
                                                 sizeof(double) + sizeof(float)) +
                                sizeof(*l));
            }
        }

        /* B2-006 reroll: fresh framework-RNG inputs and goldens recomputed
         * with the very same kernels every kRerollEvery iterations (46141ed4
         * Eigen pattern). PR #244 review round 2 (greptile+coderabbit):
         * the old "midpoint == 8" replay never fired — `since_reroll` is
         * a run() local and this test sets fracture_loop_count=4, so the
         * framework re-enters test_run in a FRESH child process every 4
         * TEST_LOOP iterations, resetting all locals (and when it did
         * fire, copying d into l poisoned the reroll state for the next
         * non-replay round). The frozen replay now triggers on an
         * in-fracture round position (round 3 of every 4) and does NOT
         * touch l at all: the SLEEF SVE kernels only READ their inputs
         * (svld1 loads) and write the per-thread od/of scratch, so the
         * replay round simply switches the input/golden source to init's
         * shared snapshot — the reroll state survives the replay round
         * intact, and replay rounds do not advance the reroll counter
         * either. The replay verifies init's frozen goldens, which init
         * computed on a different core before the workers started: the
         * same-kernel reroll recompute runs on this very core, so a
         * mercurial deterministic core defect would reproduce
         * identically on both sides and never trip the compare — the
         * cross-core frozen replay closes that blind spot. */
        const bool frozen_replay = (iter & 3) == 2;
        if (!frozen_replay && ++since_reroll >= kRerollEvery) {
            since_reroll = 0;
            sleef_fill_inputs(l);
            sleef_compute_goldens(l);
        }
        /* the kernel input/golden source of this round: init's frozen
         * shared snapshot on a replay round, this thread's re-rolled
         * struct otherwise */
        const struct sleef_test_data *src = frozen_replay ? d : l;

        /* compute-out: each family replays this round's domain inputs
         * (re-rolled or the frozen-replay snapshot) through the same SVE
         * kernels and lands in the thread-private scratch; the loops
         * step by svcntd()/svcntw() with svwhilelt predicates (see the
         * nelems comment), so the tail never walks past d->nelems at
         * any runtime VL */
        for (int i = 0; i < d->nelems; i += svcntd()) {
            const svbool_t pg = svwhilelt_b64((uint64_t)i, (uint64_t)d->nelems);
            svfloat64_t x = svld1_f64(pg, &src->xd_trig[i]);
            svst1_f64(pg, &od[i], Sleef_sindx_u10sve(x));
        }
        memcmp_or_fail(od, src->gd_sin, d->nelems, "sin double sve");

        for (int i = 0; i < d->nelems; i += svcntd()) {
            const svbool_t pg = svwhilelt_b64((uint64_t)i, (uint64_t)d->nelems);
            svfloat64_t x = svld1_f64(pg, &src->xd_trig[i]);
            svst1_f64(pg, &od[i], Sleef_cosdx_u10sve(x));
        }
        memcmp_or_fail(od, src->gd_cos, d->nelems, "cos double sve");

        for (int i = 0; i < d->nelems; i += svcntd()) {
            const svbool_t pg = svwhilelt_b64((uint64_t)i, (uint64_t)d->nelems);
            svfloat64_t x = svld1_f64(pg, &src->xd_exp[i]);
            svst1_f64(pg, &od[i], Sleef_expdx_u10sve(x));
        }
        memcmp_or_fail(od, src->gd_exp, d->nelems, "exp double sve");

        for (int i = 0; i < d->nelems; i += svcntd()) {
            const svbool_t pg = svwhilelt_b64((uint64_t)i, (uint64_t)d->nelems);
            svfloat64_t x = svld1_f64(pg, &src->xd_log[i]);
            svst1_f64(pg, &od[i], Sleef_logdx_u10sve(x));
        }
        memcmp_or_fail(od, src->gd_log, d->nelems, "log double sve");

        for (int i = 0; i < d->nelems; i += svcntw()) {
            const svbool_t pgf = svwhilelt_b32((uint64_t)i, (uint64_t)d->nelems);
            svfloat32_t x = svld1_f32(pgf, &src->xf_trig[i]);
            svst1_f32(pgf, &of[i], Sleef_sinfx_u10sve(x));
        }
        memcmp_or_fail(of, src->gf_sin, d->nelems, "sin float sve");

        for (int i = 0; i < d->nelems; i += svcntw()) {
            const svbool_t pgf = svwhilelt_b32((uint64_t)i, (uint64_t)d->nelems);
            svfloat32_t x = svld1_f32(pgf, &src->xf_trig[i]);
            svst1_f32(pgf, &of[i], Sleef_cosfx_u10sve(x));
        }
        memcmp_or_fail(of, src->gf_cos, d->nelems, "cos float sve");

        for (int i = 0; i < d->nelems; i += svcntw()) {
            const svbool_t pgf = svwhilelt_b32((uint64_t)i, (uint64_t)d->nelems);
            svfloat32_t x = svld1_f32(pgf, &src->xf_exp[i]);
            svst1_f32(pgf, &of[i], Sleef_expfx_u10sve(x));
        }
        memcmp_or_fail(of, src->gf_exp, d->nelems, "exp float sve");

        for (int i = 0; i < d->nelems; i += svcntw()) {
            const svbool_t pgf = svwhilelt_b32((uint64_t)i, (uint64_t)d->nelems);
            svfloat32_t x = svld1_f32(pgf, &src->xf_log[i]);
            svst1_f32(pgf, &of[i], Sleef_logfx_u10sve(x));
        }
        memcmp_or_fail(of, src->gf_log, d->nelems, "log float sve");

        /* u35 second-accuracy tier: same inputs, other polynomial chain */
        for (int i = 0; i < d->nelems; i += svcntd()) {
            const svbool_t pg = svwhilelt_b64((uint64_t)i, (uint64_t)d->nelems);
            svfloat64_t x = svld1_f64(pg, &src->xd_trig[i]);
            svst1_f64(pg, &od[i], Sleef_sindx_u35sve(x));
        }
        memcmp_or_fail(od, src->gd_sin35, d->nelems, "sin35 double sve");

        for (int i = 0; i < d->nelems; i += svcntd()) {
            const svbool_t pg = svwhilelt_b64((uint64_t)i, (uint64_t)d->nelems);
            svfloat64_t x = svld1_f64(pg, &src->xd_trig[i]);
            svst1_f64(pg, &od[i], Sleef_cosdx_u35sve(x));
        }
        memcmp_or_fail(od, src->gd_cos35, d->nelems, "cos35 double sve");

        for (int i = 0; i < d->nelems; i += svcntd()) {
            const svbool_t pg = svwhilelt_b64((uint64_t)i, (uint64_t)d->nelems);
            svfloat64_t x = svld1_f64(pg, &src->xd_log[i]);
            svst1_f64(pg, &od[i], Sleef_logdx_u35sve(x));
        }
        memcmp_or_fail(od, src->gd_log35, d->nelems, "log35 double sve");

        for (int i = 0; i < d->nelems; i += svcntd()) {
            const svbool_t pg = svwhilelt_b64((uint64_t)i, (uint64_t)d->nelems);
            svfloat64_t x = svld1_f64(pg, &src->xd_exp[i]);
            svst1_f64(pg, &od[i], Sleef_exp2dx_u35sve(x));
        }
        memcmp_or_fail(od, src->gd_exp2_35, d->nelems, "exp2_35 double sve");
        ++iter;
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

DECLARE_TEST(sleef_sve, "SLEEF vectorized transcendentals (SVE polynomial FMA chains, all-VL 128..1024-bit via arm64_require_sve_vl, sin/cos/exp/log double+float + u35 tier (exp2) double)")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = sleef_sve_init,
  .test_run = sleef_sve_run,
  .test_cleanup = sleef_sve_cleanup,
  .minimum_cpu = 0,
  .fracture_loop_count = 4,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
