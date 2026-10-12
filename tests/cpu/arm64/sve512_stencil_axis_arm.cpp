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
 * with src[i] = i%16+1 and coeff[r] = 0.125, every lane accumulates exact
 * integers — per call per lane j the increment is
 *   2 directions x 6 radii x (j+1) x 0.125 = 1.5 x (j+1),
 * so the exact expected result is 1.5 * iterations * (j+1). The original
 * standalone carried the stale RADIUS=4 expectation iterations*(j+1);
 * the diagnosis (5 bit-identical reruns, identical results on healthy
 * cores) proved that was a software false positive, fixed here per the
 * report's follow-up item 7.1.
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
 * Requires 512-bit SVE (svcntd() == 8); other SVE vector lengths return
 * a clean EXIT_SKIP, non-SVE CPUs are skipped by the framework's
 * compiler-feature gate. Built only on aarch64.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstddef>
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
constexpr size_t B_TILE = 16;

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
    // [extent][stride][B_TILE] grid, filled with (i % 16) + 1 per tile.
    std::vector<double> src;
    // Points at the coord=10 layer's first tile (negative offsets legal).
    const double *s_base = nullptr;
};

#ifdef __aarch64__

inline void axis_generic_16x2(
    const double* s_base, const size_t coord,
    const size_t extent, const size_t stride,
    const svbool_t ptrue, const double* coeff,
    svfloat64_t& res0, svfloat64_t& res1)
{
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
    for (size_t r = 1; r <= RADIUS; ++r) {
        const ptrdiff_t off = static_cast<ptrdiff_t>(r * stride) * B_TILE;
        const svfloat64_t c = svdup_f64(coeff[r]);

        if (coord >= r) {
            const double* n = s_base - off;
            res0 = svmla_x(ptrue, res0, svld1(ptrue, n), c);
            res1 = svmla_x(ptrue, res1, svld1(ptrue, n + 8), c);
        }
        if (coord + r < extent) {
            const double* n = s_base + off;
            res0 = svmla_x(ptrue, res0, svld1(ptrue, n), c);
            res1 = svmla_x(ptrue, res1, svld1(ptrue, n + 8), c);
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

    /* 本测试族为 512-bit VL 设计并在 HiSilicon 0xd22 上验证;GH 托管 SVE
     * runner(运行时 VL=128)上实测 golden 失配(issue #182)。 */
    if (svcntb() != 64) {
        log_skip(CpuNotSupportedSkipCategory,
                 "SVE vector length is not 512-bit on this CPU; the sve512 "
                 "family is designed and validated at 512-bit VL only "
                 "(HiSilicon 0xd22, see issue #182)");
        return EXIT_SKIP;
    }
    if (svcntd() != 8) {
        log_skip(CpuNotSupportedSkipCategory,
                 "sve512_stencil_axis_arm requires a 512-bit SVE vector "
                 "length (16 lanes across two vectors); current f64 lanes "
                 "= %zu", static_cast<size_t>(svcntd()));
        return EXIT_SKIP;
    }

    try {
        auto data = std::make_unique<SveStencilAxisData>();

        // Layout: [extent][stride][B_TILE]; every tile is {1..16} so each
        // lane is independently checkable.
        data->src.resize(extent * stride * B_TILE);
        for (size_t i = 0; i < data->src.size(); ++i)
            data->src[i] = static_cast<double>(i % B_TILE + 1);

        data->s_base = data->src.data() + coord * stride * B_TILE;

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
    const svbool_t ptrue = svptrue_b64();

    do {
        svfloat64_t res0 = svdup_f64(0.0);
        svfloat64_t res1 = svdup_f64(0.0);

        for (j = 0; j < iterations; ++j) {
            if ((reinterpret_cast<std::uintptr_t>(&res0)) & 0xFFFF000000000000ull) {
                log_warning("sve512_stencil_axis: iteration=%zu, &res0=%p",
                            j, static_cast<void *>(&res0));
            }
            axis_generic_16x2(
                s_base, coord, extent, stride, ptrue, coeff, res0, res1);
        }

        double result[B_TILE];
        svst1(ptrue, result, res0);
        svst1(ptrue, result + 8, res1);

        // coord=10, RADIUS=6: both directions valid for every r, so each
        // call adds 2 * 6 * 0.125 * (j+1) = 1.5*(j+1) per lane — exact
        // integers throughout, equality is bit-exact.
        bool ok = true;
        for (size_t j = 0; j < B_TILE; ++j) {
            const double expected =
                1.5 * static_cast<double>(iterations) * (j + 1);
            if (result[j] != expected) {
                log_warning("sve512_stencil_axis: lane %2zu result=%.0f "
                            "expected=%.0f", j, result[j], expected);
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
             "NUMA3 field recipe): 16-wide tile, RADIUS=6 dual-direction "
             "svmla_x chain with svdup_f64 coefficients (ld1rd codegen), "
             "memory-resident loop counter and a per-iteration VA[63:48] "
             "accumulator-address canary; exact-integer per-lane "
             "verification against 1.5*iterations*(j+1)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_stencil_axis_arm_init,
    .test_run = sve512_stencil_axis_arm_run,
    .test_cleanup = sve512_stencil_axis_arm_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
