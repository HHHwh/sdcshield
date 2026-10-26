/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve512_gather_scatter_svd
 * @parblock
 * SVD-format gather/scatter round-trip with SVD-scale working set and 2-D
 * block index pattern matching Eigen BDCSVD's block-of-matrix access.
 *
 * Differs from sve512_gather_scatter_arm: working set ~2.9 MB (90000 f64
 * elements across src/dst/golden) vs 4 KB, 2-D block index table vs 1-D
 * linear permutation.
 *
 * Fixes over the initial integration (which crashed on the SVE target):
 *   - tail-block permutation now clamps k to the actual in-bounds block
 *     extents (300 is not a multiple of 16; the old code indexed up to
 *     row/col 303, out of bounds of the 90000-element vectors);
 *   - the golden is precomputed once in test_init (read-only in run);
 *   - the scatter destination is a per-CPU-thread local buffer (the old
 *     shared dst + per-thread std::fill(0) was a data race under
 *     multi-threading);
 *   - the hot loop uses an svwhilelt predicate: 90000 is not guaranteed
 *     divisible by every runtime VL, so a full ptrue could over-read the
 *     final partial vector.
 *
 * Review hardening (PR #283, F2 — mirrors the _arm variant's v2
 * precedent): the gather_src fill is index-revealing v2 — sign |
 * exponent [0.5,2) | 32-bit entropy in mantissa [51:20] | 20-bit
 * element index tag (90000 < 2^20, so every element is distinct; the
 * old seeds_f64[i % vl_d] fill had only vl_d distinct values and could
 * not reveal a wrong-offset gather landing in the same residue class),
 * and the run-side destination is zero-filled at the top of every round
 * (the golden 1.5 + v*v is never zero, so a dropped scatter write
 * cannot ride on the previous round's correct bytes).
 *
 * All-VL 128..1024-bit via arm64_require_sve_vl (all-VL Phase 2 P3;
 * the pre-P3 512-bit single-value gate and its issue #182 caveat are
 * cleared by the four-tier dual-kernel evidence in gem5-fi
 * feat/sve512-dual); test_run re-checks the VL and fails closed on
 * drift. The 90000-element workspace itself is VL-independent — the
 * svwhilelt tail predicate above already handles every runtime VL.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <memory>
#include <vector>

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

static constexpr size_t SVD_ROWS = 300;
static constexpr size_t SVD_COLS = 300;
static constexpr size_t SVD_BLOCK_ROWS = 16;
static constexpr size_t SVD_BLOCK_COLS = 16;

// Index-revealing encoding v2 (review F2, _arm variant precedent): gn =
// 90000 < 2^20, so the low 20 mantissa bits uniquely tag every element.
static constexpr uint64_t IDX_TAG_MASK = 0xFFFFFULL;

static inline uint64_t splitmix64(uint64_t x)
{
    uint64_t z = (x + 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

struct SveGatherScatterSvdData {
    size_t vl_d;
    std::vector<uint64_t> gather_src;
    std::vector<uint64_t> gather_idx;
    // Precomputed once in init; read-only in run (shared across threads).
    std::vector<uint64_t> scatter_golden;
};

#ifdef __aarch64__

static void gather_scatter_golden(SveGatherScatterSvdData *d)
{
    for (size_t i = 0; i < d->gather_idx.size(); ++i) {
        double v;
        memcpy(&v, &d->gather_src[d->gather_idx[i]], 8);
        v = std::fma(v, v, 1.5);
        memcpy(&d->scatter_golden[d->gather_idx[i]], &v, 8);
    }
}

static void gather_scatter_hw(SveGatherScatterSvdData *d, uint64_t *dst)
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

static void report_f64_lane_mismatch(const char *tag, size_t lane,
                                     uint64_t golden, uint64_t actual)
{
    log_warning("%s: lane %zu golden=0x%016" PRIx64 " "
                "actual=0x%016" PRIx64 " xor=0x%016" PRIx64,
                tag, lane, golden, actual, golden ^ actual);
}

static int sve512_gather_scatter_svd_init(struct test *test)
{
    /* All-VL Phase 2 P3: capability + 128..1024-bit range check in one
       framework-layer call (the geometry has been svcnt-dynamic all
       along — the svwhilelt tail predicate was designed for arbitrary
       runtime VLs from day one; only the old single-value gate pinned
       one tier). The historical VL=128 golden mismatch on a hosted CI
       runner (issue #182) is cleared by the four-tier dual-kernel
       evidence (gem5-fi feat/sve512-dual, 48 runs). arm64_require_sve_vl
       returns the VL in BYTES — f64 lanes = svl/8, range-checked
       below (release-effective, NDEBUG-proof). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    try {
        auto data = std::make_unique<SveGatherScatterSvdData>();
        data->vl_d = arm64_sve_f64_lanes_checked(test, svl);
        if (data->vl_d == 0)
            return EXIT_FAILURE;   /* lanes out of range — reason logged */

        // SVD-scale gather/scatter workspace: 300x300 doubles = 720 KB.
        const size_t gn = SVD_ROWS * SVD_COLS;
        data->gather_src.resize(gn);
        data->gather_idx.resize(gn);
        data->scatter_golden.assign(gn, 0);

        /* Index-revealing encoding v2 (review F2, the _arm variant's
         * precedent): the low 20 mantissa bits (IDX_TAG_MASK) carry the
         * element index, while sign, exponent and the upper mantissa
         * carry per-run fresh entropy from the framework RNG (H12': -s
         * reproducible). gn = 90000 < 2^20, so every element's tag is
         * distinct — the pre-fix seeds_f64[i % vl_d] periodic fill (only
         * vl_d distinct values) could not reveal a wrong-offset gather
         * landing in the same residue class. The exponent stays in the
         * [0.5, 2) band so the FMA transform v = 1.5 + v*v stays in
         * [1.75, 5.5]: never zero (a dropped scatter write then shows
         * up as the round's zero fill) and never overflow. */
        for (size_t i = 0; i < gn; ++i) {
            uint64_t sign = (random64() & 1ULL) << 63;
            uint64_t exp = 0x3FEULL + (random64() & 1ULL);          // [0.5, 2)
            uint64_t entropy = (random64() & 0xFFFFFFFFULL) << 20;  // mant [51:20]
            data->gather_src[i] = sign | (exp << 52) | entropy |
                                  (uint64_t)(i & IDX_TAG_MASK);
            data->gather_idx[i] = i;
        }

        // SVD-format block permutation: shuffle within each block, keeping
        // the 2-D locality of BDCSVD's block traversal. 300 is NOT a
        // multiple of 16, so the tail blocks (br/bc = 288) are 12x12 /
        // 16x12 / 12x16 — clamp k to the actual in-bounds extents or the
        // flat indices run off the end of the 90000-element vectors.
        for (size_t br = 0; br < SVD_ROWS; br += SVD_BLOCK_ROWS) {
            for (size_t bc = 0; bc < SVD_COLS; bc += SVD_BLOCK_COLS) {
                const size_t rows_in =
                    (SVD_ROWS - br < SVD_BLOCK_ROWS) ? (SVD_ROWS - br)
                                                     : SVD_BLOCK_ROWS;
                const size_t cols_in =
                    (SVD_COLS - bc < SVD_BLOCK_COLS) ? (SVD_COLS - bc)
                                                     : SVD_BLOCK_COLS;
                /* block-local shuffle driven by the framework RNG
                 * (randomization hardening H12': per-run fresh
                 * permutation; was a fixed splitmix permutation). */
                for (size_t k = 0; k < rows_in * cols_in; ++k) {
                    size_t j = (size_t)(random64() % (k + 1));
                    size_t row_i = br + k / cols_in;
                    size_t col_i = bc + k % cols_in;
                    size_t row_j = br + j / cols_in;
                    size_t col_j = bc + j % cols_in;
                    uint64_t t = data->gather_idx[row_i * SVD_COLS + col_i];
                    data->gather_idx[row_i * SVD_COLS + col_i] =
                        data->gather_idx[row_j * SVD_COLS + col_j];
                    data->gather_idx[row_j * SVD_COLS + col_j] = t;
                }
            }
        }

        // Golden precomputed once (read-only afterwards, thread-safe).
        gather_scatter_golden(data.get());

        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sve512_gather_scatter_svd init: %s", e.what());
        return EXIT_SKIP;
    }
}

static int sve512_gather_scatter_svd_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<SveGatherScatterSvdData *>(test->data);

    /* All-VL policy #3: fail closed on init/run VL drift (single source,
       framework/device/cpu/arm64/arm64_vl.h). */
    ARM64_SVE_VL_DRIFT_CHECK_F64(test, d->vl_d);

    // Per-CPU-thread local destination (no cross-thread sharing),
    // zero-filled at the top of every round (review F2): the golden
    // 1.5 + v*v is never zero, so a dropped scatter write leaves the
    // round's zero fill behind instead of riding on the previous
    // round's correct bytes.
    std::vector<uint64_t> dst(d->scatter_golden.size());

    do {
        std::fill(dst.begin(), dst.end(), 0);
        bool all_passed = true;
        gather_scatter_hw(d, dst.data());
        for (size_t i = 0; i < dst.size(); ++i) {
            if (dst[i] != d->scatter_golden[i]) {
                report_f64_lane_mismatch("sve512_gather_scatter_svd", i,
                                         d->scatter_golden[i], dst[i]);
                all_passed = false;
            }
        }
        if (!all_passed) {
            report_fail_msg("sve512_gather_scatter_svd: SDC detected");
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));
    return EXIT_SUCCESS;
}

static int sve512_gather_scatter_svd_cleanup(struct test *test)
{
    delete static_cast<SveGatherScatterSvdData *>(test->data);
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(sve512_gather_scatter_svd,
             "SVD-format SVE gather/scatter round-trip: 90000-element "
             "VL-independent working set + 2-D block index permutation "
             "(all-VL 128..1024-bit via arm64_require_sve_vl), byte-exact "
             "vs scalar golden")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_gather_scatter_svd_init,
    .test_run = sve512_gather_scatter_svd_run,
    .test_cleanup = sve512_gather_scatter_svd_cleanup,
    .fracture_loop_count = 0,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
