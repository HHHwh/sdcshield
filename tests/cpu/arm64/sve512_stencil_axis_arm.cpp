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
 * layout offsets scale at runtime; out-of-range VLs return a clean
 * EXIT_SKIP, non-SVE CPUs are skipped by the framework's
 * compiler-feature gate. Built only on aarch64.
 * @endparblock
 */

#include <sandstone.h>
#include <cinttypes>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#ifdef __aarch64__
#include <sys/auxv.h>
#include <asm/hwcap.h>
#include <arm_sve.h>
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

// Memory-resident loop counter, as in the original (file-scope, so it is
// not register-allocated). thread_local because the framework runs one
// test_run per CPU concurrently — a plain global would race across
// threads; TLS preserves the memory-resident property per thread.
static thread_local size_t j;

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

inline void axis_generic_16x2(
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

static int sve512_stencil_axis_arm_init(struct test *test)
{
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SVE not available; sve512_stencil_axis_arm requires SVE");
        return EXIT_SKIP;
    }

    /* Full-VL support: bt = 2*svcntd() scales every layout offset below.
     * The 512-bit geometry was characterized on HiSilicon 0xd22; the
     * parameterized geometry is verified at all four VLs in gem5 (the
     * historical issue #182 mismatch was the stale RADIUS=4 analytic
     * expectation, since fixed — see the @parblock). */
    const size_t bt = 2 * svcntd();
    if (bt < 4 || bt > kTileMax) {
        log_skip(CpuNotSupportedSkipCategory,
                 "sve512_stencil_axis_arm: SVE vector length %zu bytes is "
                 "outside the supported 128..1024-bit range",
                 static_cast<size_t>(svcntb()));
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
        svfloat64_t res0 = svdup_f64(0.0);
        svfloat64_t res1 = svdup_f64(0.0);

        for (j = 0; j < iterations; ++j) {
            if ((reinterpret_cast<std::uintptr_t>(&res0)) & 0xFFFF000000000000ull) {
                log_warning("sve512_stencil_axis: iteration=%zu, &res0=%p",
                            j, static_cast<void *>(&res0));
            }
            axis_generic_16x2(
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
             "with exact-integer per-lane verification")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_stencil_axis_arm_init,
    .test_run = sve512_stencil_axis_arm_run,
    .test_cleanup = sve512_stencil_axis_arm_cleanup,
    .fracture_loop_count = 0,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
