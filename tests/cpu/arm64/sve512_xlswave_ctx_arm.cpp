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
 * Phase A — stencil axis core, verbatim from sve512_stencil_axis_arm:
 *   svdup_f64(coeff[r]) loads, dual-vector svmla_x chain, full unroll,
 *   memory-resident thread-local loop counter, per-iteration VA[63:48]
 *   canary on the accumulator's stack slot. K_AXIS_CALLS calls per sweep
 *   add exactly 1.5*(lane+1)*K_AXIS_CALLS per lane (0.125 is exact).
 * Phase B — SVD-format gather/scatter, verbatim addressing topology from
 *   sve512_gather_scatter_svd: 300x300 doubles (720 KB, L2-overflow),
 *   2-D 16x16 block permutation (clamped tail blocks), svld1_gather /
 *   svst1_scatter with svwhilelt tails, fma(v,v,1.5) in flight.
 * Phase C — verification: phase-A exact-integer lane check, phase-B full
 *   byte-compare against the init-time scalar golden (verified every
 *   sweep — the compare is ~0.1 ms, so no subset rotation is needed),
 *   and a pointer-table canary (workspace pointers byte-compared against
 *   their sweep-0 images, each checked for VA[63:48] == 0).
 *
 * Goldens are SVE-independent: phase A is a closed-form scalar constant,
 * phase B is precomputed by a scalar C loop in test_init (framework-side).
 * Requires 512-bit SVE like the rest of the sve512 family (issue #182).
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
#endif

using std::size_t;
using std::ptrdiff_t;

// Phase A geometry (identical to sve512_stencil_axis_arm).
constexpr size_t RADIUS = 6;
constexpr size_t B_TILE = 16;
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
    std::vector<double> src;            // [extent][stride][B_TILE] grid
    const double *s_base = nullptr;     // coord=10 layer's first tile
    size_t vl_d = 0;
    std::vector<uint64_t> gather_src;
    std::vector<uint64_t> gather_idx;
    std::vector<uint64_t> scatter_golden;  // init-time scalar golden
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
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SVE not available; sve512_xlswave_ctx_arm requires SVE");
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

    try {
        auto data = std::make_unique<XlswaveCtxData>();
        data->vl_d = svcntd();

        // Phase A workspace: [extent][stride][B_TILE], tile values {1..16}.
        data->src.resize(extent * stride * B_TILE);
        for (size_t i = 0; i < data->src.size(); ++i)
            data->src[i] = static_cast<double>(i % B_TILE + 1);
        data->s_base = data->src.data() + coord * stride * B_TILE;

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

    // Stack-local coefficient table (the svdup_f64 / ld1rd source).
    double coeff[RADIUS + 1] = {};
    for (size_t r = 1; r <= RADIUS; ++r)
        coeff[r] = 0.125;

    const double* s_base = d->s_base;
    const svbool_t ptrue = svptrue_b64();

    // Per-CPU-thread phase-B destination (full permutation overwrites
    // every element each pass — no memset, no cross-thread sharing).
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
        /* ---- Phase A: stencil axis core (field recipe verbatim) ---- */
        svfloat64_t res0 = svdup_f64(0.0);
        svfloat64_t res1 = svdup_f64(0.0);
        for (j = 0; j < K_AXIS_CALLS; ++j) {
            if ((reinterpret_cast<std::uintptr_t>(&res0)) &
                    0xFFFF000000000000ull) {
                log_warning("sve512_xlswave_ctx: axis iter=%zu &res0=%p",
                            j, static_cast<void *>(&res0));
            }
            axis_generic_16x2(s_base, coord, extent, stride, ptrue, coeff,
                              res0, res1);
        }
        double result[B_TILE];
        svst1(ptrue, result, res0);
        svst1(ptrue, result + 8, res1);
        for (size_t jj = 0; jj < B_TILE; ++jj) {
            const double expected = 96.0 * (jj + 1);  // 1.5*K_AXIS_CALLS*(jj+1)
            if (result[jj] != expected) {
                log_warning("sve512_xlswave_ctx: axis lane %2zu "
                            "result=%.1f expected=%.1f",
                            jj, result[jj], expected);
                report_fail_msg("sve512_xlswave_ctx_arm: phase A "
                                "(stencil axis) SDC detected");
                return EXIT_FAILURE;
            }
        }

        /* ---- Phase B: SVD-format gather/scatter round trip ---- */
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
             "RADIUS=6 dual svmla chain, per-iteration VA[63:48] accumulator "
             "canary), SVD-format SVE-512 gather/scatter over a 720 KB "
             "L2-overflow working set with 2-D 16x16 block permutation, and "
             "a workspace pointer-table canary; exact-integer and byte-exact "
             "verification every sweep")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_xlswave_ctx_arm_init,
    .test_run = sve512_xlswave_ctx_arm_run,
    .test_cleanup = sve512_xlswave_ctx_arm_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
