/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve512_stencil_axis_arm
 * @parblock
 * SCF/stencil axis-kernel SDC trigger replicator — verbatim port of the
 * standalone example.cpp reproducer for the cn23154 NUMA3 virtual-address
 * path fault. Hot-loop structure preserved exactly, including the
 * svdup_f64(coeff[r]) coefficient loads (ld1rd replicate-load codegen
 * under clang — one of the fault-window instructions), the dual-vector
 * svmla_x chain, the temp/res0 spacing assignments, the full unroll
 * pragma, the memory-resident file-scope loop counter and the
 * per-iteration VA[63:48] canary on the accumulator's stack slot.
 *
 * Math (per the field diagnosis, production stencil_kernel.h radius=6):
 * src holds per-element random small integers (1..256) drawn from the
 * framework RNG — re-seeded per fracture fragment, so every fragment
 * gets fresh inputs — and coeff[r] = 0.125. Per call per lane j the
 * increment is sum over r of 0.125 * (src[coord-r][j] +
 * src[coord+r][j]): every term is an exact multiple of 1/8 and the
 * per-call increment stays below 768, so the expected result — the
 * scalar reference in test_run scaled by iterations — stays on the
 * exact 1/8 grid below 2^53: order-independent, bit-exact equality.
 * (The original standalone used the fixed src[i] = i%16+1, where the
 * expectation reduced to 1.5 * iterations * (j+1); its stale RADIUS=4
 * expectation iterations*(j+1) was a software false positive — 5
 * bit-identical reruns, identical results on healthy cores — fixed per
 * the report's follow-up item 7.1.)
 *
 * Two shapes per TEST_LOOP round (B6 / R2b freeze blind spot):
 *   - Shape A (randomized integers): per-fragment random small
 *     integers with the fixed 0.125 coefficients, verified bit-exactly
 *     against the scalar reference — breaks the frozen per-round replay
 *     while keeping exact-1/8-grid verification (PR #234 review,
 *     greptile 4254737399; PR #237 review, coderabbit).
 *   - Shape B (fully randomized): per-thread grid and coefficients
 *     re-rolled from the framework RNG every round, run through the
 *     very same axis_generic_dual hot path, then compared against an
 *     INDEPENDENT scalar std::fma chain that mirrors the kernel's
 *     r=1..6 up-then-down order. Shape A varies only the grid per
 *     fragment; shape B re-rolls both the grid and the coefficients
 *     every round, making every round a fresh sample even within one
 *     fragment. The scalar reference and the SVE svmla are both
 *     single-rounded IEEE fma on bitwise-identical operands, so the two
 *     are expected to agree bit-exactly; kTolRel is a safety margin for
 *     implementation-level micro-differences only (worst 1-ulp-per-step
 *     accumulation over kRandIters is ~8e-13 relative, an order inside
 *     the tolerance).
 *
 * Framework adaptations (each forced by the harness, structure-neutral):
 *   - main() -> test_init/test_run/test_cleanup; src and s_base are set
 *     up once in init and shared read-only across CPU threads;
 *   - the file-scope loop counter j is thread_local: sdcshield runs one
 *     test_run per CPU concurrently and a plain global would be a
 *     cross-thread data race (false FAILs); TLS keeps it memory-resident
 *     exactly as the original intended;
 *   - coeff stays a stack-local table in test_run (the ld1rd source);
 *   - the canary's printf -> log_warning;
 *   - #pragma clang loop unroll(full) guarded by __clang__ (GCC -Wall
 *     warns on the unknown pragma; clang builds — the field toolchain —
 *     see it unchanged);
 *   - per-lane results are logged only on mismatch; chrono timing
 *     dropped (the framework times tests itself).
 *
 * Full-VL support: bt = 2*svcntd() f64 lanes (128..1024-bit SVE), all
 * layout offsets scale at runtime; test_init gates the SVE capability
 * and the 128..1024-bit range via the framework layer
 * (arm64_require_sve_vl, all-VL Phase 2 P6) and out-of-range tile
 * widths return a clean EXIT_SKIP, non-SVE CPUs are skipped by the
 * framework's compiler-feature gate. Built only on aarch64.
 *
 * RUNTIME-UNVERIFIABLE (512-bit SVE): developed on a host without SVE
 * (HWCAP_SVE clear); compile-verified and the skip path executed for
 * real, but the SVE datapath itself has not run on hardware
 * (cf. 8502e491 / 73ebbd3f for the precedent).
 * @endparblock
 */

#include <sandstone.h>
#include <cassert>
#include <cinttypes>
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

using std::size_t;
using std::ptrdiff_t;

constexpr size_t RADIUS = 6;
/* Full-VL support (128..1024-bit): the kernel writes two result vectors
 * (res0+res1), so the tile width is bt = 2*svcntd() f64 lanes, read at
 * runtime (4..32). Originally 512-bit-only (B_TILE=16); the parameterized
 * geometry has been verified at VL 128/256/512/1024 in gem5 (gem5-fi
 * feat/sve-sme-all-vl, kernel stencil_axis_dual, 16-run all-green incl.
 * the 512-bit regression anchor). */
constexpr size_t kTileMax = 32;   /* VL=1024bit upper bound */

// Grid geometry (shared between init and run).
static constexpr size_t coord = 10;
static constexpr size_t extent = 56;
static constexpr size_t stride = 64;
static constexpr size_t iterations = 100'000'000;

// Shape B (B6): random-shape iteration depth and comparison tolerance.
// kRandIters keeps the scalar reference chain cheap (~1.6M fma, ~1ms per
// reroll — negligible next to shape A's 100M-iteration round); kTolRel
// only guards implementation-level micro-differences (see header).
static constexpr size_t kRandIters = 8192;
static constexpr double kTolRel = 1e-11;

// Memory-resident loop counter, as in the original (file-scope, so it is
// not register-allocated). thread_local because the framework runs one
// test_run per CPU concurrently — a plain global would race across
// threads; TLS preserves the memory-resident property per thread.
static thread_local size_t j;

// Shape B per-thread state (B6): this thread's re-rolled random grid and
// coefficients plus the scalar-reference expectation/error bound. The
// grid is lazily allocated and intentionally not freed in cleanup: the
// worker threads are gone when the main thread runs test_cleanup and the
// forked child exits right after (dgemm-style argument).
static thread_local double *r_grid = nullptr;
static thread_local double r_coeff[RADIUS + 1];
static thread_local double r_expected[kTileMax];
static thread_local double r_bound[kTileMax];

struct SveStencilAxisData {
    // Tile width in f64 lanes = 2*svcntd() at the runtime VL (4..32).
    size_t bt;
    // [extent][stride][bt] grid of random small integers.
    std::vector<double> src;
    // Points at the coord=10 layer's first tile (negative offsets legal).
    const double *s_base = nullptr;
};

#ifdef __aarch64__

// Type-punning helper (mirrors fsu_byteexact_arm / fpu_special_values_sve).
static inline uint64_t bits_of(double d)
{
    uint64_t u;
    std::memcpy(&u, &d, sizeof(u));
    return u;
}

/* (was axis_generic_16x2 — named for the retired 512-bit B_TILE=16 ×
 * 2-vector geometry; renamed for the bt-parameterized all-VL kernel.
 * The gem5-fi dual-kernel counterpart is stencil_axis_dual.) */
inline void axis_generic_dual(
    const double* s_base, const size_t coord,
    const size_t extent, const size_t stride,
    const svbool_t ptrue, const double* coeff, const size_t bt,
    svfloat64_t& res0, svfloat64_t& res1)
{
    const size_t lanes = bt / 2;   /* f64 lanes per result vector */
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (size_t r = 1; r <= RADIUS; ++r) {
        const ptrdiff_t off = static_cast<ptrdiff_t>(r * stride) * bt;
        const svfloat64_t c = svdup_f64(coeff[r]);

        if (coord >= r) {
            const double* n = s_base - off;
            res0 = svmla_x(ptrue, res0, svld1(ptrue, n), c);
            res1 = svmla_x(ptrue, res1, svld1(ptrue, n + lanes), c);
        }
        if (coord + r < extent) {
            const double* n = s_base + off;
            res0 = svmla_x(ptrue, res0, svld1(ptrue, n), c);
            res1 = svmla_x(ptrue, res1, svld1(ptrue, n + lanes), c);
        }
    }
}

// Framework-RNG helpers (B6): fold a random uint64 into a bounded
// fraction (a raw reinterpret would hit the NaN/Inf exponent field about
// a quarter of the time).
static double stencil_random_frac()
{
    uint64_t r = random64();
    return static_cast<double>(r >> 11) / 9007199254740992.0; /* [0, 1) */
}
static double stencil_random_pm_one()
{
    uint64_t r = random64();
    double frac = static_cast<double>(r >> 11) / 9007199254740992.0;
    return (r & 1) ? (frac - 1.0) : frac; /* [-1, 1) */
}

static int sve512_stencil_axis_arm_init(struct test *test)
{
    /* All-VL Phase 2 P6: probe-only swap — the SVE capability gate moves
     * to the framework layer (arm64_require_sve_vl: HWCAP_SVE + the
     * 128..1024-bit range in one call), replacing the getauxval
     * hand-probe. The bt = 2*svcntd() geometry below and its [4,
     * kTileMax] stack-buffer guard are the P1-242-verified parameterized
     * geometry and stay untouched; the run-side assert() that pins
     * init's bt invariant stays as the existing drift guard. */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;
    (void)svl;   /* gate only: bt is read via svcntd() as before */

    /* Full-VL support: bt = 2*svcntd() scales every layout offset below.
     * The 512-bit geometry was characterized on HiSilicon 0xd22; the
     * parameterized geometry is verified at all four VLs in gem5 (the
     * historical issue #182 mismatch was the stale RADIUS=4 analytic
     * expectation, since fixed — see the @parblock). svcntd() is >= 2 on
     * any legal SVE (VL >= 128-bit), so bt < 4 is a defensive dead branch
     * kept as a stack-buffer guard, not a reachable skip.
     *
     * gem5-fi no-injection control, real run output (random seed=1, one
     * run per VL tier, gem5.opt configs/se/arm_chaos.py --cpu O3
     * --sve_vl N, kernel stencil_axis_dual):
     *   vl=1 (128-bit)  SDCSHIELD_FAILS=0  checksum 6a6e2ba55a1f0240
     *   vl=2 (256-bit)  SDCSHIELD_FAILS=0  checksum 8b7dcecd82a0e212
     *   vl=4 (512-bit)  SDCSHIELD_FAILS=0  checksum 610e3ce5b02e1ace
     *                   (bit-identical to the pre-parameterization
     *                   512-bit kernel — regression anchor)
     *   vl=8 (1024-bit) SDCSHIELD_FAILS=0  checksum 8ea5a0fb4dfb840c
     * Honest boundary: gem5 validates the same-source dual kernel per
     * tier; this host (Kunpeng 920, VL=256) can run the C++ test at its
     * native VL only — the 512/1024-bit tiers are gem5-backed, real-SVE
     * runs at those VLs remain future hardware validation. */
    const size_t bt = 2 * svcntd();
    if (bt < 4 || bt > kTileMax) {
        log_skip(CpuNotSupportedSkipCategory,
                 "sve512_stencil_axis_arm: SVE vector length %zu bytes "
                 "(bt=%zu f64 tile lanes) is outside the supported "
                 "128..1024-bit range",
                 static_cast<size_t>(svcntb()), bt);
        return EXIT_SKIP;
    }

    try {
        auto data = std::make_unique<SveStencilAxisData>();

        // Layout: [extent][stride][bt]; per-element random small
        // integers from the framework RNG keep every lane independently
        // checkable while varying the input combinations across fracture
        // fragments (the RNG is re-seeded per fragment; PR #234 review,
        // greptile 4254737399). Exact integers keep the accumulation
        // bit-exact — see the scalar reference in _run.
        data->bt = bt;
        data->src.resize(extent * stride * bt);
        for (size_t i = 0; i < data->src.size(); ++i)
            data->src[i] = static_cast<double>(1 + random32() % 256);

        data->s_base = data->src.data() + coord * stride * bt;

        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sve512_stencil_axis_arm init: %s", e.what());
        return EXIT_SKIP;
    }
}

static int sve512_stencil_axis_arm_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<SveStencilAxisData *>(test->data);

    // Stack-local coefficient table (the svdup_f64 / ld1rd source), as in
    // the original main(). 0.125 is exactly representable.
    double coeff[RADIUS + 1] = {};
    for (size_t r = 1; r <= RADIUS; ++r)
        coeff[r] = 0.125;

    const double* s_base = d->s_base;
    const size_t bt = d->bt;
    /* init and run are separate functions: the expected/result stack
     * arrays below are sized kTileMax, so pin init's invariant (bt within
     * [4, kTileMax]) before any buffer math (PR #242 review, coderabbit
     * Learnings). */
    assert(bt >= 4 && bt <= kTileMax);
    const size_t lanes = bt / 2;
    const svbool_t ptrue = svptrue_b64();

    /* Scalar reference for the randomized src: one kernel call adds, per
     * lane j, sum over r of coeff[r] * (src[coord-r][j] + src[coord+r][j])
     * — coord=10 and RADIUS=6 make both directions valid for every r.
     * Every term is an exact multiple of 1/8 and the per-call increment
     * stays below 12 * 0.125 * 512 = 768, so both the hardware
     * accumulation over 1e8 calls and this reference stay on the exact
     * 1/8 grid below 2^53: order-independent, bit-exact equality. */
    double expected[kTileMax];
    for (size_t j = 0; j < bt; ++j)
        expected[j] = 0.0;
    for (size_t r = 1; r <= RADIUS; ++r) {
        const ptrdiff_t off =
            static_cast<ptrdiff_t>(r * stride) * bt;
        for (size_t j = 0; j < bt; ++j) {
            double delta = 0.0;
            if (coord >= r)
                delta += s_base[-off + static_cast<ptrdiff_t>(j)];
            if (coord + r < extent)
                delta += s_base[+off + static_cast<ptrdiff_t>(j)];
            expected[j] += 0.125 * delta;
        }
    }
    for (size_t j = 0; j < bt; ++j)
        expected[j] *= static_cast<double>(iterations);

    do {
        /* ---- 形态 A: 随机整数格点 (main, per-fragment) ----
         * 固定 0.125 系数 + init 期随机小整数 src, 对照 run 顶部的标量
         * 参考, 位模式精确比较。 */
        svfloat64_t res0 = svdup_f64(0.0);
        svfloat64_t res1 = svdup_f64(0.0);

        for (j = 0; j < iterations; ++j) {
            if ((reinterpret_cast<std::uintptr_t>(&res0)) & 0xFFFF000000000000ull) {
                log_warning("sve512_stencil_axis: iteration=%zu, &res0=%p",
                            j, static_cast<void *>(&res0));
            }
            axis_generic_dual(
                s_base, coord, extent, stride, ptrue, coeff, bt, res0, res1);
        }

        double result[kTileMax];
        svst1(ptrue, result, res0);
        svst1(ptrue, result + lanes, res1);

        /* Per-lane bit-exact comparison: compare the raw IEEE-754 bit
         * patterns instead of C++ == so the check is byte-exact like the
         * framework's memcmp_or_fail (PR #237 review, coderabbit), and log
         * both patterns — %.0f would round away the sub-1.0 1/8-grid
         * fraction and the failing bits. */
        bool ok = true;
        for (size_t j = 0; j < bt; ++j) {
            if (bits_of(result[j]) != bits_of(expected[j])) {
                log_warning("sve512_stencil_axis: lane %2zu "
                            "result=0x%016" PRIx64 " "
                            "expected=0x%016" PRIx64,
                            j, bits_of(result[j]), bits_of(expected[j]));
                ok = false;
            }
        }

        if (!ok) {
            report_fail_msg(
                "sve512_stencil_axis_arm: stencil axis kernel SDC detected");
            return EXIT_FAILURE;
        }

        /* ---- 形态 B: 全随机第二形态 (B6 / R2b 冻结盲区) ----
         * 同一热路径 (axis_generic_dual 原样), 输入换成框架 RNG 每轮重掷
         * 的随机格点/系数 (per-thread); 期望由独立标量 std::fma 链式参考
         * 按内核顺序 (r=1..6, 每半径先上后下) 计算 —— 形态 A 仅按
         * fragment 变化格点, 形态 B 每轮连系数一起重掷。 */
        {
            if (__builtin_expect(!r_grid, 0)) {
                r_grid = new (std::nothrow) double[extent * stride * bt];
                if (!r_grid) {
                    report_fail_msg("sve512_stencil_axis_arm: per-thread "
                                    "random grid alloc failed (%zu bytes)",
                                    extent * stride * bt *
                                        sizeof(double));
                }
            }

            for (size_t i = 0; i < extent * stride * bt; ++i)
                r_grid[i] = stencil_random_pm_one();
            for (size_t r = 1; r <= RADIUS; ++r)
                r_coeff[r] = 0.25 + 0.75 * stencil_random_frac();

            const double *r_base = r_grid + coord * stride * bt;

            /* 独立标量参考。几何常量 (coord=10, extent=56, RADIUS=6) 下
             * 双向条件恒真, 与内核的 if 分支一一对应。r_bound 是逐步增量
             * 绝对值之和 (舍入误差幅度的上界基准): |acc_final| <= bound,
             * 最坏逐步 1-ulp 舍入累积 <= kRandIters * eps * bound ——
             * kTolRel 余量 10 倍以上。 */
            {
                const double *up[RADIUS + 1], *dn[RADIUS + 1];
                for (size_t r = 1; r <= RADIUS; ++r) {
                    up[r] = r_base - static_cast<ptrdiff_t>(r * stride) * bt;
                    dn[r] = r_base + static_cast<ptrdiff_t>(r * stride) * bt;
                }
                for (size_t l = 0; l < bt; ++l) {
                    r_expected[l] = 0.0;
                    double inc = 0.0;
                    for (size_t r = 1; r <= RADIUS; ++r)
                        inc += std::fabs(r_coeff[r]) *
                               (std::fabs(up[r][l]) + std::fabs(dn[r][l]));
                    r_bound[l] = static_cast<double>(kRandIters) * inc;
                }
                for (size_t it = 0; it < kRandIters; ++it)
                    for (size_t r = 1; r <= RADIUS; ++r)
                        for (size_t l = 0; l < bt; ++l) {
                            r_expected[l] = std::fma(up[r][l], r_coeff[r],
                                                     r_expected[l]);
                            r_expected[l] = std::fma(dn[r][l], r_coeff[r],
                                                     r_expected[l]);
                        }
            }

            svfloat64_t b_res0 = svdup_f64(0.0);
            svfloat64_t b_res1 = svdup_f64(0.0);

            for (j = 0; j < kRandIters; ++j) {
                if ((reinterpret_cast<std::uintptr_t>(&b_res0)) & 0xFFFF000000000000ull) {
                    log_warning("sve512_stencil_axis (random shape): "
                                "iteration=%zu, &b_res0=%p",
                                j, static_cast<void *>(&b_res0));
                }
                axis_generic_dual(
                    r_base, coord, extent, stride, ptrue, r_coeff,
                    bt, b_res0, b_res1);
            }

            double b_result[kTileMax];
            svst1(ptrue, b_result, b_res0);
            svst1(ptrue, b_result + lanes, b_res1);

            for (size_t l = 0; l < bt; ++l) {
                double diff = std::fabs(b_result[l] - r_expected[l]);
                double tol = kTolRel * (r_bound[l] + 1e-300);
                /* !(<=) 形式一并捕获 NaN */
                if (!(diff <= tol)) {
                    report_fail_msg(
                        "sve512_stencil_axis_arm: random shape diverged at "
                        "lane %2zu: |%.17g - %.17g| = %.3e > tol %.3e",
                        l, b_result[l], r_expected[l], diff, tol);
                }
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int sve512_stencil_axis_arm_cleanup(struct test *test)
{
    delete static_cast<SveStencilAxisData *>(test->data);
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(sve512_stencil_axis_arm,
             "SCF/stencil axis-kernel SDC trigger replicator (cn23154 "
             "NUMA3 field recipe): VL-scaled dual-vector tile (128-1024-bit), RADIUS=6 dual-direction "
             "svmla_x chain with svdup_f64 coefficients (ld1rd codegen), "
             "memory-resident loop counter and a per-iteration VA[63:48] "
             "accumulator-address canary; per-fragment randomized inputs "
             "with exact-integer per-lane bit-pattern verification, plus "
             "a framework-RNG random second shape (re-rolled grid and "
             "coefficients every round) against an independent scalar "
             "fma reference (B6)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_stencil_axis_arm_init,
    .test_run = sve512_stencil_axis_arm_run,
    .test_cleanup = sve512_stencil_axis_arm_cleanup,
    .fracture_loop_count = 0,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
