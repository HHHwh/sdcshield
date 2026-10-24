/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve512_f64_special_svd
 * @parblock
 * SVD-format f64 special-value chain — same SVD-scale, lanes-proportional
 * working set and nested block traversal as sve512_f64_chain_svd, but over
 * the IEEE-754 special-value table (NaN/sNaN/±Inf/±0/±1), compared by
 * category. All-VL 128..1024-bit via arm64_require_sve_vl (all-VL
 * Phase 2 P3); the pre-P3 512-bit single-value gate and its issue #182
 * caveat are cleared by the four-tier dual-kernel evidence in gem5-fi
 * feat/sve512-dual; test_run re-checks the VL and fails closed on drift.
 *
 * Differs from sve512_f64_special_arm: working set scales with the lanes
 * (0.7..5.9 MB across sm and sa over the 128..1024-bit tiers) vs 8 KB,
 * nested block traversal vs flat stream.
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
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

// Special-value (category-checked) chain length — VL-independent (a
// per-lane dependency sequence; the lane count, not the length, scales
// with VL).
static constexpr size_t SPECIAL_CHAIN_STEPS = 64;
static constexpr size_t SVD_ROWS = 300;
static constexpr size_t SVD_COLS = 300;
static constexpr size_t SVD_BLOCK_ROWS = 16;
static constexpr size_t SVD_BLOCK_COLS = 16;

static const uint64_t F64_SPECIAL[] = {
    0x0000000000000000ULL, 0x8000000000000000ULL, 0x7FF8000000000000ULL,
    0x7FF0000000000001ULL, 0x7FF0000000000000ULL, 0xFFF0000000000000ULL,
    0x3FF0000000000000ULL, 0xBFF0000000000000ULL,
};
static constexpr size_t F64_SPECIAL_SIZE =
    sizeof(F64_SPECIAL) / sizeof(F64_SPECIAL[0]);

static inline uint64_t splitmix64(uint64_t x)
{
    uint64_t z = (x + 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

struct SveF64SpecialSvdData {
    size_t vl_d;
    std::vector<uint64_t> seeds_f64;
    std::vector<uint64_t> sm_f64;
    std::vector<uint64_t> sa_f64;
};

#ifdef __aarch64__

// Block-major stream layout: [block_row][block_col][step][lane], one
// SPECIAL_CHAIN_STEPS x VL contiguous slab per block origin (19x19 blocks).
static inline size_t block_slab_base(size_t row, size_t col, size_t lanes)
{
    const size_t n_blocks_c = (SVD_COLS + SVD_BLOCK_COLS - 1) / SVD_BLOCK_COLS;
    return ((row / SVD_BLOCK_ROWS) * n_blocks_c + (col / SVD_BLOCK_COLS)) *
           (SPECIAL_CHAIN_STEPS * lanes);
}

static void f64_special_golden(const SveF64SpecialSvdData *d, uint64_t *out,
                               size_t row, size_t col)
{
    const size_t lanes = d->vl_d;
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint64_t accw = d->seeds_f64[lane];
        double acc;
        memcpy(&acc, &accw, 8);
        for (size_t i = 0; i < SPECIAL_CHAIN_STEPS; ++i) {
            double m, a;
            const size_t base = block_slab_base(row, col, lanes) + i * lanes;
            memcpy(&m, &d->sm_f64[base + lane], 8);
            memcpy(&a, &d->sa_f64[base + lane], 8);
            acc = std::fma(m, a, acc);
        }
        memcpy(&accw, &acc, 8);
        out[lane] = accw;
    }
}

static void f64_special_chain_hw(const SveF64SpecialSvdData *d, uint64_t *out,
                                 size_t row, size_t col)
{
    const size_t lanes = d->vl_d;
    const svbool_t pg = svptrue_b64();
    svfloat64_t acc = svld1_f64(pg,
        reinterpret_cast<const double *>(d->seeds_f64.data()));
    for (size_t i = 0; i < SPECIAL_CHAIN_STEPS; ++i) {
        const size_t base = block_slab_base(row, col, lanes) + i * lanes;
        svfloat64_t m = svld1_f64(pg,
            reinterpret_cast<const double *>(&d->sm_f64[base]));
        svfloat64_t a = svld1_f64(pg,
            reinterpret_cast<const double *>(&d->sa_f64[base]));
        acc = svmla_f64_x(pg, acc, m, a);
    }
    svst1_f64(pg, reinterpret_cast<double *>(out), acc);
}

static inline bool f64_lane_ok(uint64_t golden, uint64_t actual)
{
    double g, a;
    memcpy(&g, &golden, 8); memcpy(&a, &actual, 8);
    if (std::isnan(g)) return std::isnan(a);
    if (std::isinf(g)) return std::isinf(a) && ((g > 0) == (a > 0));
    if (std::isnan(a) || std::isinf(a)) return false;
    return golden == actual;
}

static void report_f64_lane_mismatch(const char *tag, size_t lane,
                                     uint64_t golden, uint64_t actual)
{
    log_warning("%s: lane %zu golden=0x%016" PRIx64 " "
                "actual=0x%016" PRIx64 " xor=0x%016" PRIx64,
                tag, lane, golden, actual, golden ^ actual);
}

static int sve512_f64_special_svd_init(struct test *test)
{
    /* All-VL Phase 2 P3: capability + 128..1024-bit range check in one
       framework-layer call (the geometry has been svcnt-dynamic all
       along; only the old single-value gate pinned one tier). The
       historical VL=128 golden mismatch on a hosted CI runner
       (issue #182) is cleared by the four-tier dual-kernel evidence
       (gem5-fi feat/sve512-dual, 48 runs). arm64_require_sve_vl
       returns the VL in BYTES — f64 lanes = svl/8, range-checked
       below (release-effective, NDEBUG-proof). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    try {
        auto data = std::make_unique<SveF64SpecialSvdData>();
        data->vl_d = (size_t)svl / 8;   /* f64 lanes = SVL bytes / 8 */
        if (data->vl_d < 2 || data->vl_d > 16) {
            log_error("sve512_f64_special_svd: f64 lanes %zu outside "
                      "[2,16] — unit drift (svl %ld bytes)",
                      data->vl_d, svl);
            return EXIT_FAILURE;
        }
        /* randomization hardening H12': seeds from the framework RNG;
         * the special VALUE table stays by design, but which special each
         * step draws is now a random index. */
        data->seeds_f64.resize(data->vl_d);
        for (size_t lane = 0; lane < data->vl_d; ++lane) {
            data->seeds_f64[lane] = 0x3FF0000000000000ULL |
                (random64() & 0x000FFFFFFFFFFFFFULL);
        }
        // Block-major streams: 361 blocks x SPECIAL_CHAIN_STEPS x VL
        // — the working set is proportional to the lanes: across sm and
        // sa the four tiers (128/256/512/1024-bit) total 0.7/1.5/3.0/
        // 5.9 MB (the 512-bit tier keeps the intended ~1.4 MB-per-stream
        // SVD-scale footprint; the old [row][col][step][lane] layout
        // multiplied out to ~368 MB per stream).
        const size_t n_blocks_r = (SVD_ROWS + SVD_BLOCK_ROWS - 1) / SVD_BLOCK_ROWS;
        const size_t n_blocks_c = (SVD_COLS + SVD_BLOCK_COLS - 1) / SVD_BLOCK_COLS;
        const size_t total = n_blocks_r * n_blocks_c * SPECIAL_CHAIN_STEPS * data->vl_d;
        data->sm_f64.resize(total);
        data->sa_f64.resize(total);
        for (size_t i = 0; i < total; ++i) {
            data->sm_f64[i] = F64_SPECIAL[random32() % F64_SPECIAL_SIZE];
            data->sa_f64[i] = F64_SPECIAL[random32() % F64_SPECIAL_SIZE];
        }
        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sve512_f64_special_svd init: %s", e.what());
        return EXIT_SKIP;
    }
}

static int sve512_f64_special_svd_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<SveF64SpecialSvdData *>(test->data);

    /* All-VL policy #3: fail closed on init/run VL drift. */
    if (svcntd() != d->vl_d) {
        log_error("sve512_f64_special_svd: VL drifted between init (%zu "
                  "lanes) and run (%u lanes)", d->vl_d, (unsigned)svcntd());
        report_fail_msg("sve512_f64_special_svd: SVE VL drifted between "
                        "init and run");
    }

    std::vector<uint64_t> golds64(d->vl_d);
    std::vector<uint64_t> hw(d->vl_d);

    do {
        bool all_passed = true;
        for (size_t br = 0; br < SVD_ROWS; br += SVD_BLOCK_ROWS) {
            for (size_t bc = 0; bc < SVD_COLS; bc += SVD_BLOCK_COLS) {
                f64_special_golden(d, golds64.data(), br, bc);
                f64_special_chain_hw(d, hw.data(), br, bc);
                for (size_t lane = 0; lane < d->vl_d; ++lane) {
                    if (!f64_lane_ok(golds64[lane], hw[lane])) {
                        report_f64_lane_mismatch(
                            "sve512_f64_special_svd", lane,
                            golds64[lane], hw[lane]);
                        all_passed = false;
                    }
                }
            }
        }
        if (!all_passed) {
            report_fail_msg("sve512_f64_special_svd: SDC detected");
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));
    return EXIT_SUCCESS;
}

static int sve512_f64_special_svd_cleanup(struct test *test)
{
    delete static_cast<SveF64SpecialSvdData *>(test->data);
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(sve512_f64_special_svd,
             "SVD-format SVE f64 special-value chain: lanes-proportional "
             "SVD-scale working set + nested block traversal (all-VL "
             "128..1024-bit via arm64_require_sve_vl), category compare")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_f64_special_svd_init,
    .test_run = sve512_f64_special_svd_run,
    .test_cleanup = sve512_f64_special_svd_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
