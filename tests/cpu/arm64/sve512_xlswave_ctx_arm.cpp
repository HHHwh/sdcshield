/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve512_xlswave_ctx_arm
 * @parblock
 * XLSDFT composite-context SDC stress (Phase 3 escalation T1).
 *
 * Motivation: nine synthetic load classes and a no-MPI stencil extraction
 * all ran clean on cpu139 — the fault needs the real XLSDFT instruction
 * mix, whose known ingredients are (field campaign P5): a stencil axis
 * kernel (svdup/ld1rd coefficient broadcast + RADIUS=6 dual svmla chain),
 * SVD block-traversal addressing with SVE gather over an L2-overflowing
 * working set, and pointer/accumulator address canaries. This test
 * interleaves all of them in one per-core loop: three phases rotate per
 * sweep, so the machine sees the same instruction-class alternation as
 * the real application without reproducing its MPI/libomp scaffolding.
 *
 * Phase A — stencil axis core, verbatim from sve512_stencil_axis_arm
 *   (all-VL form): svdup_f64(coeff[r]) loads, dual-vector svmla_x chain,
 *   full unroll, memory-resident thread-local loop counter,
 *   per-iteration VA[63:48] canary on the accumulator's stack slot. The
 *   tile width is bt = 2*svcntd() f64 lanes (128..1024-bit SVE — every
 *   layout offset scales with bt; all-VL Phase 2 P3, four-tier gem5
 *   evidence in gem5-fi feat/sve512-dual stencil_axis_dual).
 * Phase B — SVD-format gather/scatter, verbatim addressing topology from
 *   sve512_gather_scatter_svd: 300x300 doubles (720 KB, L2-overflow),
 *   2-D 16x16 block permutation (clamped tail blocks), svld1_gather /
 *   svst1_scatter with svwhilelt tails, fma(v,v,1.5) in flight.
 * Phase C — verification: phase-A per-lane byte-compare against the
 *   init-frozen scalar golden (computed once in test_init from the src
 *   fill with the stencil_axis reference math — an independent golden:
 *   at-rest src corruption after init drives the hardware result away
 *   from the frozen copy, whereas a per-sweep recompute from the same
 *   corrupted src would agree with it; the pre-P3 closed-form constant
 *   96.0*(jj+1) had the same independence but bound to the fill
 *   pattern), phase-B full
 *   byte-compare against the init-time scalar golden (verified every
 *   sweep — the compare is ~0.1 ms, so no subset rotation is needed),
 *   and a pointer-table canary (workspace pointers byte-compared against
 *   their sweep-0 images, each checked for VA[63:48] == 0).
 *
 * Goldens are SVE-independent: phase A is an init-frozen scalar reference
 * over the bt-wide tile, phase B is precomputed by a scalar C loop in
 * test_init (framework-side). All-VL (128..1024-bit via arm64_require_sve_vl);
 * the pre-P3 512-bit single-value gate and its issue #182 caveat are
 * cleared by the four-tier dual-kernel evidence.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <cmath>
#include <algorithm>  // std::fill: libc++ does not pull <algorithm> transitively (libstdc++ does)
#include <memory>
#include <vector>

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

using std::size_t;
using std::ptrdiff_t;

// Phase A geometry (identical to sve512_stencil_axis_arm, all-VL form).
constexpr size_t RADIUS = 6;
constexpr size_t kTileMax = 32;   /* 1024-bit cap: bt = 2*svcntd() <= 32 */
static constexpr double AXIS_COEFF = 0.125;   /* stencil coefficient (r=1..6) */
static constexpr size_t coord = 10;
static constexpr size_t extent = 56;
static constexpr size_t stride = 64;
static constexpr size_t K_AXIS_CALLS = 64;

// Phase B geometry (identical to sve512_gather_scatter_svd).
static constexpr size_t SVD_ROWS = 300;
static constexpr size_t SVD_COLS = 300;
static constexpr size_t SVD_BLOCK_ROWS = 16;
static constexpr size_t SVD_BLOCK_COLS = 16;

// Memory-resident loop counter, as in the field recipe.
static thread_local size_t j;

struct XlswaveCtxData {
    std::vector<double> src;            // [extent][stride][bt] grid
    const double *s_base = nullptr;     // coord=10 layer's first tile
    size_t bt = 0;                      // tile width = 2*svcntd() lanes
    size_t vl_d = 0;
    std::vector<double> axis_expected;  // init-frozen Phase A golden
    std::vector<uint64_t> gather_src;
    std::vector<uint64_t> gather_idx;
    std::vector<uint64_t> scatter_golden;  // init-time scalar golden
};

#ifdef __aarch64__

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

static void gather_scatter_golden(XlswaveCtxData *d)
{
    for (size_t i = 0; i < d->gather_idx.size(); ++i) {
        double v;
        memcpy(&v, &d->gather_src[d->gather_idx[i]], 8);
        v = std::fma(v, v, 1.5);
        memcpy(&d->scatter_golden[d->gather_idx[i]], &v, 8);
    }
}

static void gather_scatter_hw(XlswaveCtxData *d, uint64_t *dst)
{
    const size_t n = d->gather_idx.size();
    const size_t lanes = d->vl_d;
    const svfloat64_t one_half = svdup_f64(1.5);
    for (size_t off = 0; off < n; off += lanes) {
        const svbool_t pg = svwhilelt_b64((uint64_t)off, (uint64_t)n);
        svuint64_t vidx = svld1_u64(pg, &d->gather_idx[off]);
        svfloat64_t v = svld1_gather_u64index_f64(
            pg, reinterpret_cast<const double *>(d->gather_src.data()), vidx);
        v = svmla_f64_x(pg, one_half, v, v);
        svst1_scatter_u64index_f64(
            pg, reinterpret_cast<double *>(dst), vidx, v);
    }
}

static int sve512_xlswave_ctx_arm_init(struct test *test)
{
    /* All-VL Phase 2 P3: capability + 128..1024-bit range check in one
       framework-layer call; the historical issue #182 VL=128 mismatch is
       cleared by the four-tier dual-kernel evidence (gem5-fi
       feat/sve512-dual). arm64_require_sve_vl returns BYTES — f64 lanes
       = svl/8, tile width bt = 2*lanes, range-checked below
       (release-effective, NDEBUG-proof). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    try {
        auto data = std::make_unique<XlswaveCtxData>();
        data->vl_d = arm64_sve_f64_lanes_checked(test, svl);
        data->bt = 2 * data->vl_d;
        /* bt = 2*lanes <= kTileMax whenever lanes is in range; the
           belt-and-braces bt guard keeps the kTileMax-sized stack
           buffers' invariant explicit (out-of-range lanes are already
           logged by the helper). */
        if (data->vl_d == 0 || data->bt > kTileMax)
            return EXIT_FAILURE;

        // Phase A workspace: [extent][stride][bt], tile values {1..bt}.
        data->src.resize(extent * stride * data->bt);
        for (size_t i = 0; i < data->src.size(); ++i)
            data->src[i] = static_cast<double>(i % data->bt + 1);
        data->s_base = data->src.data() + coord * stride * data->bt;

        /* Phase A golden, frozen here (review F1): the geometry (coord/
           extent/stride/RADIUS), the 0.125 coefficients, K_AXIS_CALLS and
           the src fill are all init-fixed, so the expected tile is
           loop-invariant — computing it once restores an independent
           golden. A per-sweep recompute reads the same src the hardware
           just consumed, so at-rest src corruption reconciles both sides
           and goes unnoticed; the frozen copy cannot. Exactness (the
           stencil_axis idiom): every term is an exact multiple of 1/8 and
           the per-call increment stays below 12 * 0.125 * bt <= 48, so
           after ×K_AXIS_CALLS both the hardware accumulation and this
           reference stay on the exact 1/8 grid below 2^53 — bit-exact
           equality, byte-compared per lane in run. */
        data->axis_expected.resize(data->bt);
        for (size_t jj = 0; jj < data->bt; ++jj)
            data->axis_expected[jj] = 0.0;
        for (size_t r = 1; r <= RADIUS; ++r) {
            const ptrdiff_t off = static_cast<ptrdiff_t>(r * stride) * data->bt;
            for (size_t jj = 0; jj < data->bt; ++jj) {
                double delta = 0.0;
                if (coord >= r)
                    delta += data->s_base[-off + static_cast<ptrdiff_t>(jj)];
                if (coord + r < extent)
                    delta += data->s_base[+off + static_cast<ptrdiff_t>(jj)];
                data->axis_expected[jj] += AXIS_COEFF * delta;
            }
        }
        for (size_t jj = 0; jj < data->bt; ++jj)
            data->axis_expected[jj] *= static_cast<double>(K_AXIS_CALLS);

        // Phase B workspace + 2-D block permutation (framework RNG, H12').
        const size_t gn = SVD_ROWS * SVD_COLS;
        data->gather_src.resize(gn);
        data->gather_idx.resize(gn);
        data->scatter_golden.assign(gn, 0);
        for (size_t i = 0; i < gn; ++i) {
            data->gather_src[i] = 0x3FF0000000000000ULL |
                (random64() & 0x000FFFFFFFFFFFFFULL);
            data->gather_idx[i] = i;
        }
        for (size_t br = 0; br < SVD_ROWS; br += SVD_BLOCK_ROWS) {
            for (size_t bc = 0; bc < SVD_COLS; bc += SVD_BLOCK_COLS) {
                const size_t rows_in =
                    (SVD_ROWS - br < SVD_BLOCK_ROWS) ? (SVD_ROWS - br)
                                                     : SVD_BLOCK_ROWS;
                const size_t cols_in =
                    (SVD_COLS - bc < SVD_BLOCK_COLS) ? (SVD_COLS - bc)
                                                     : SVD_BLOCK_COLS;
                for (size_t k = 0; k < rows_in * cols_in; ++k) {
                    size_t jj = (size_t)(random64() % (k + 1));
                    size_t row_i = br + k / cols_in;
                    size_t col_i = bc + k % cols_in;
                    size_t row_j = br + jj / cols_in;
                    size_t col_j = bc + jj % cols_in;
                    uint64_t t = data->gather_idx[row_i * SVD_COLS + col_i];
                    data->gather_idx[row_i * SVD_COLS + col_i] =
                        data->gather_idx[row_j * SVD_COLS + col_j];
                    data->gather_idx[row_j * SVD_COLS + col_j] = t;
                }
            }
        }
        gather_scatter_golden(data.get());

        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sve512_xlswave_ctx_arm init: %s", e.what());
        return EXIT_SKIP;
    }
}

static int sve512_xlswave_ctx_arm_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<XlswaveCtxData *>(test->data);

    /* All-VL policy #3: fail closed on init/run VL drift (single source,
       framework/device/cpu/arm64/arm64_vl.h). */
    ARM64_SVE_VL_DRIFT_CHECK_F64(test, d->vl_d);
    const size_t bt = d->bt;
    const size_t lanes = d->vl_d;

    // Stack-local coefficient table (the svdup_f64 / ld1rd source).
    double coeff[RADIUS + 1] = {};
    for (size_t r = 1; r <= RADIUS; ++r)
        coeff[r] = AXIS_COEFF;

    const double* s_base = d->s_base;
    const svbool_t ptrue = svptrue_b64();

    // Per-CPU-thread phase-B destination (no cross-thread sharing);
    // zero-filled at the top of every sweep below (review F2) so a
    // dropped scatter write cannot ride on the previous sweep's bytes.
    std::vector<uint64_t> dst(d->scatter_golden.size());

    // Pointer-table canary: sweep-0 images, byte-compared every sweep.
    const void *canary[7] = {
        static_cast<const void *>(d),
        static_cast<const void *>(d->src.data()),
        static_cast<const void *>(d->s_base),
        static_cast<const void *>(d->gather_src.data()),
        static_cast<const void *>(d->gather_idx.data()),
        static_cast<const void *>(d->scatter_golden.data()),
        static_cast<const void *>(dst.data()),
    };
    const void *canary_golden[7];
    memcpy(canary_golden, canary, sizeof(canary));

    do {
        /* ---- Phase A: stencil axis core (field recipe, all-VL form) ---- */
        svfloat64_t res0 = svdup_f64(0.0);
        svfloat64_t res1 = svdup_f64(0.0);
        for (j = 0; j < K_AXIS_CALLS; ++j) {
            if ((reinterpret_cast<std::uintptr_t>(&res0)) &
                    0xFFFF000000000000ull) {
                log_warning("sve512_xlswave_ctx: axis iter=%zu &res0=%p",
                            j, static_cast<void *>(&res0));
            }
            axis_generic_dual(s_base, coord, extent, stride, ptrue, coeff,
                              bt, res0, res1);
        }
        double result[kTileMax];
        svst1(ptrue, result, res0);
        svst1(ptrue, result + lanes, res1);

        /* Phase A verification: per-lane byte-exact compare against the
           init-frozen golden (the values are exact 1/8-grid quantities,
           so != and memcmp agree; memcmp keeps the byte-exact golden
           discipline of phases B/C). */
        for (size_t jj = 0; jj < bt; ++jj) {
            if (memcmp(&result[jj], &d->axis_expected[jj], sizeof(double))
                    != 0) {
                log_warning("sve512_xlswave_ctx: axis lane %2zu "
                            "result=%.1f expected=%.1f",
                            jj, result[jj], d->axis_expected[jj]);
                report_fail_msg("sve512_xlswave_ctx_arm: phase A "
                                "(stencil axis) SDC detected");
                return EXIT_FAILURE;
            }
        }

        /* ---- Phase B: SVD-format gather/scatter round trip ---- */
        /* Zero the destination at the top of every sweep (review F2, the
           gather_scatter_svd/_arm idiom): the golden fma(v,v,1.5) is never
           zero, so a dropped or misdirected scatter write leaves the zero
           fill behind and fails the phase-C compare instead of riding on
           the previous sweep's correct bytes. */
        std::fill(dst.begin(), dst.end(), 0);
        gather_scatter_hw(d, dst.data());

        /* ---- Phase C: byte-exact verification + pointer canary ---- */
        if (memcmp(dst.data(), d->scatter_golden.data(),
                   dst.size() * sizeof(uint64_t)) != 0) {
            for (size_t i = 0; i < dst.size(); ++i) {
                if (dst[i] != d->scatter_golden[i]) {
                    log_warning("sve512_xlswave_ctx: gather lane %zu "
                                "golden=0x%016" PRIx64 " actual=0x%016"
                                PRIx64 " xor=0x%016" PRIx64, i,
                                d->scatter_golden[i], dst[i],
                                d->scatter_golden[i] ^ dst[i]);
                    break;
                }
            }
            report_fail_msg("sve512_xlswave_ctx_arm: phase B "
                            "(gather/scatter) SDC detected");
            return EXIT_FAILURE;
        }
        if (memcmp(canary, canary_golden, sizeof(canary)) != 0) {
            for (size_t i = 0; i < 7; ++i) {
                if (canary[i] != canary_golden[i]) {
                    log_warning("sve512_xlswave_ctx: canary slot %zu changed "
                                "golden=%p actual=%p", i, canary_golden[i],
                                canary[i]);
                }
                if ((reinterpret_cast<std::uintptr_t>(canary[i])) &
                        0xFFFF000000000000ull) {
                    log_warning("sve512_xlswave_ctx: canary slot %zu "
                                "VA[63:48] != 0: %p", i, canary[i]);
                }
            }
            report_fail_msg("sve512_xlswave_ctx_arm: phase C "
                            "(pointer canary) SDC detected");
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int sve512_xlswave_ctx_arm_cleanup(struct test *test)
{
    delete static_cast<XlswaveCtxData *>(test->data);
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(sve512_xlswave_ctx_arm,
             "XLSDFT composite-context SDC stress (escalation T1): rotating "
             "phases — stencil axis kernel (svdup/ld1rd coefficients, "
             "RADIUS=6 dual svmla chain at bt = 2*svcntd() lanes, "
             "per-iteration VA[63:48] accumulator canary), SVD-format SVE "
             "gather/scatter over a 720 KB L2-overflow working set with "
             "2-D 16x16 block permutation, and a workspace pointer-table "
             "canary; exact-integer and byte-exact verification every "
             "sweep; all-VL 128..1024-bit via arm64_require_sve_vl")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_xlswave_ctx_arm_init,
    .test_run = sve512_xlswave_ctx_arm_run,
    .test_cleanup = sve512_xlswave_ctx_arm_cleanup,
    .fracture_loop_count = 0,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
