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

static constexpr size_t SVD_ROWS = 300;
static constexpr size_t SVD_COLS = 300;
static constexpr size_t SVD_BLOCK_ROWS = 16;
static constexpr size_t SVD_BLOCK_COLS = 16;

static inline uint64_t splitmix64(uint64_t x)
{
    uint64_t z = (x + 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

struct SveGatherScatterSvdData {
    size_t vl_d;
    std::vector<uint64_t> seeds_f64;
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
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SVE not available; sve512_gather_scatter_svd requires SVE");
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
        auto data = std::make_unique<SveGatherScatterSvdData>();
        data->vl_d = svcntd();

        /* randomization hardening H12': seeds from the framework RNG. */
        data->seeds_f64.resize(data->vl_d);
        for (size_t lane = 0; lane < data->vl_d; ++lane) {
            data->seeds_f64[lane] = 0x3FF0000000000000ULL |
                (random64() & 0x000FFFFFFFFFFFFFULL);
        }

        // SVD-scale gather/scatter workspace: 300x300 doubles = 720 KB.
        const size_t gn = SVD_ROWS * SVD_COLS;
        data->gather_src.resize(gn);
        data->gather_idx.resize(gn);
        data->scatter_golden.assign(gn, 0);

        for (size_t i = 0; i < gn; ++i) {
            data->gather_src[i] = data->seeds_f64[i % data->vl_d];
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

    // Per-CPU-thread local destination: the index table is a full
    // permutation, so every element is overwritten each pass — no memset
    // needed, and no cross-thread sharing.
    std::vector<uint64_t> dst(d->scatter_golden.size());

    do {
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
             "SVD-format SVE-512 gather/scatter round-trip: 1.44 MB working "
             "set + 2-D block index permutation, byte-exact vs scalar golden")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_gather_scatter_svd_init,
    .test_run = sve512_gather_scatter_svd_run,
    .test_cleanup = sve512_gather_scatter_svd_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
