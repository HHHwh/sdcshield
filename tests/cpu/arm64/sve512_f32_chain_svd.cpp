/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve512_f32_chain_svd
 * @parblock
 * SVD-format f32 FMLA chain with an SVD-scale working set and nested
 * block traversal, mirroring eigen_svd_cdouble_sve's beyond-L2 memory
 * behaviour at f32 width (4..32 lanes per vector). All-VL 128..1024-bit
 * via arm64_require_sve_vl (all-VL Phase 2 P3); the pre-P3 512-bit
 * single-value gate and its issue #182 caveat are cleared by the
 * four-tier dual-kernel evidence in gem5-fi feat/sve512-dual;
 * test_run re-checks the VL and fails closed on drift.
 *
 * Differs from sve512_f32_chain_arm: working set scales with the lanes
 * (5.9..47.3 MB across m and a over the 128..1024-bit tiers) vs 64 KB,
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

// Serial FMA chain length per iteration — VL-independent (a per-lane
// dependency sequence; the lane count, not the length, scales with VL).
static constexpr size_t CHAIN_STEPS = 512;
static constexpr size_t SVD_ROWS = 300;
static constexpr size_t SVD_COLS = 300;
static constexpr size_t SVD_BLOCK_ROWS = 16;
static constexpr size_t SVD_BLOCK_COLS = 16;

static const uint32_t F32_FINITE[] = {
    0x3FAAAAABU, 0xBFAAAAABU, 0x3EAAAAABU, 0xBF2AAAABU,
    0x3F800000U, 0xBF800000U, 0x00555555U, 0x80AAAAAAU,
    0x3FCCCCCDU, 0xBFCCCCCDU, 0x3F333333U, 0xBF333333U,
    0x3E555555U, 0xBE555555U, 0x3F666666U, 0xBF666666U,
};
static constexpr size_t F32_FINITE_SIZE =
    sizeof(F32_FINITE) / sizeof(F32_FINITE[0]);

static inline uint64_t splitmix64(uint64_t x)
{
    uint64_t z = (x + 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

struct SveF32ChainSvdData {
    size_t vl_w;
    std::vector<uint32_t> seeds_f32;
    std::vector<uint32_t> m_f32;
    std::vector<uint32_t> a_f32;
};

#ifdef __aarch64__

// Block-major stream layout: [block_row][block_col][step][lane], one
// CHAIN_STEPS x VL contiguous slab per block origin (19x19 blocks).
static inline size_t block_slab_base(size_t row, size_t col, size_t lanes)
{
    const size_t n_blocks_c = (SVD_COLS + SVD_BLOCK_COLS - 1) / SVD_BLOCK_COLS;
    return ((row / SVD_BLOCK_ROWS) * n_blocks_c + (col / SVD_BLOCK_COLS)) *
           (CHAIN_STEPS * lanes);
}

static void f32_chain_golden(const SveF32ChainSvdData *d, uint32_t *out,
                             size_t row, size_t col)
{
    const size_t lanes = d->vl_w;
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t accw = d->seeds_f32[lane];
        float acc;
        memcpy(&acc, &accw, 4);
        for (size_t i = 0; i < CHAIN_STEPS; ++i) {
            float m, a;
            const size_t base = block_slab_base(row, col, lanes) + i * lanes;
            memcpy(&m, &d->m_f32[base + lane], 4);
            memcpy(&a, &d->a_f32[base + lane], 4);
            acc = std::fma(m, a, acc);
        }
        memcpy(&accw, &acc, 4);
        out[lane] = accw;
    }
}

static void f32_chain_hw(const SveF32ChainSvdData *d, uint32_t *out,
                         size_t row, size_t col)
{
    const size_t lanes = d->vl_w;
    const svbool_t pg = svptrue_b32();
    svfloat32_t acc = svld1_f32(pg,
        reinterpret_cast<const float *>(d->seeds_f32.data()));
    for (size_t i = 0; i < CHAIN_STEPS; ++i) {
        const size_t base = block_slab_base(row, col, lanes) + i * lanes;
        svfloat32_t m = svld1_f32(pg,
            reinterpret_cast<const float *>(&d->m_f32[base]));
        svfloat32_t a = svld1_f32(pg,
            reinterpret_cast<const float *>(&d->a_f32[base]));
        acc = svmla_f32_x(pg, acc, m, a);
    }
    svst1_f32(pg, reinterpret_cast<float *>(out), acc);
}

static void report_f32_lane_mismatch(const char *tag, size_t lane,
                                     uint32_t golden, uint32_t actual)
{
    log_warning("%s: lane %zu golden=0x%08" PRIx32 " "
                "actual=0x%08" PRIx32 " xor=0x%08" PRIx32,
                tag, lane, golden, actual, golden ^ actual);
}

static int sve512_f32_chain_svd_init(struct test *test)
{
    /* All-VL Phase 2 P3: capability + 128..1024-bit range check in one
       framework-layer call (the geometry has been svcnt-dynamic all
       along; only the old single-value gate pinned one tier). The
       historical VL=128 golden mismatch on a hosted CI runner
       (issue #182) is cleared by the four-tier dual-kernel evidence
       (gem5-fi feat/sve512-dual, 48 runs). arm64_require_sve_vl
       returns the VL in BYTES — f32 lanes = svl/4, range-checked
       below (release-effective, NDEBUG-proof). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    try {
        auto data = std::make_unique<SveF32ChainSvdData>();
        data->vl_w = (size_t)svl / 4;   /* f32 lanes = SVL bytes / 4 */
        if (data->vl_w < 4 || data->vl_w > 32) {
            log_error("sve512_f32_chain_svd: f32 lanes %zu outside "
                      "[4,32] — unit drift (svl %ld bytes)",
                      data->vl_w, svl);
            return EXIT_FAILURE;
        }
        /* randomization hardening H12': framework RNG per run (-s
         * reproducible) instead of fixed splitmix/table indices; the
         * |x| < 2 finite-band invariant is preserved (f32 bias-127
         * exponents 0x7E/0x7F, B1-007), and 25% of entries still draw
         * from the high-Hamming F32_FINITE table for gate-toggle density. */
        auto random_finite_f32 = []() -> uint32_t {
            if ((random32() & 3) == 0)
                return F32_FINITE[random32() % F32_FINITE_SIZE];
            uint32_t sign = (random32() & 1U) << 31;
            uint32_t mantissa = random32() & 0x007FFFFFU;
            /* 0x7E = [0.5, 1); 0x7F = [1, 2) — f32 bias-127 exponents;
             * the f64-biased 0x3FE/0x3FF built qNaN/Inf patterns (B1-007). */
            uint32_t exp = 0x7EU + (random32() & 1U);
            return sign | (exp << 23) | mantissa;
        };
        /* Value-domain knob (SEVI ASPLOS'26 Obs.19: bounded vs unbounded
         * inputs differ up to 245x in SDC frequency on the same core).
         * bounded=1 (default): the |x| <= 2 finite band above — the
         * randomization-hardened path, untouched.
         * bounded=0: full-entropy mantissas with the exponent clamped to
         * [2^-8, 1) so the chain still cannot overflow to Inf/NaN and
         * the byte-exact golden stays valid. */
        int64_t bounded = get_testspecific_knob_value_int(test, "bounded", 1);
        auto random_unbounded_f32 = []() -> uint32_t {
            uint32_t sign = (random32() & 1U) << 31;
            uint32_t mantissa = random32() & 0x007FFFFFU;
            uint32_t exp = 0x77U + ((random32() >> 29) & 7U);  /* [2^-8, 1) */
            return sign | (exp << 23) | mantissa;
        };
        data->seeds_f32.resize(data->vl_w);
        for (size_t lane = 0; lane < data->vl_w; ++lane) {
            data->seeds_f32[lane] = 0x3F800000U |
                (random32() & 0x007FFFFFU);
        }
        // Block-major streams: 361 blocks x CHAIN_STEPS x VL f32 elements
        // — the working set is proportional to the lanes: across m and a
        // the four tiers (128/256/512/1024-bit) total 5.9/11.8/23.7/47.3
        // MB, every tier far beyond L2, keeping the SVD overflow intent
        // (and staying far from the old layout's 2.7 GiB-per-stream
        // blowup).
        const size_t n_blocks_r = (SVD_ROWS + SVD_BLOCK_ROWS - 1) / SVD_BLOCK_ROWS;
        const size_t n_blocks_c = (SVD_COLS + SVD_BLOCK_COLS - 1) / SVD_BLOCK_COLS;
        const size_t total = n_blocks_r * n_blocks_c * CHAIN_STEPS * data->vl_w;
        data->m_f32.resize(total);
        data->a_f32.resize(total);
        for (size_t i = 0; i < total; ++i) {
            data->m_f32[i] = bounded ? random_finite_f32()
                                     : random_unbounded_f32();
            data->a_f32[i] = bounded ? random_finite_f32()
                                     : random_unbounded_f32();
        }
        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sve512_f32_chain_svd init: %s", e.what());
        return EXIT_SKIP;
    }
}

static int sve512_f32_chain_svd_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<SveF32ChainSvdData *>(test->data);

    /* All-VL policy #3: fail closed on init/run VL drift. */
    if (svcntw() != d->vl_w) {
        log_error("sve512_f32_chain_svd: VL drifted between init (%zu "
                  "lanes) and run (%u lanes)", d->vl_w, (unsigned)svcntw());
        report_fail_msg("sve512_f32_chain_svd: SVE VL drifted between "
                        "init and run");
    }

    std::vector<uint32_t> gold32(d->vl_w);
    std::vector<uint32_t> hw(d->vl_w);

    do {
        bool all_passed = true;
        for (size_t br = 0; br < SVD_ROWS; br += SVD_BLOCK_ROWS) {
            for (size_t bc = 0; bc < SVD_COLS; bc += SVD_BLOCK_COLS) {
                f32_chain_golden(d, gold32.data(), br, bc);
                f32_chain_hw(d, hw.data(), br, bc);
                for (size_t lane = 0; lane < d->vl_w; ++lane) {
                    if (hw[lane] != gold32[lane]) {
                        report_f32_lane_mismatch(
                            "sve512_f32_chain_svd", lane,
                            gold32[lane], hw[lane]);
                        all_passed = false;
                    }
                }
            }
        }
        if (!all_passed) {
            report_fail_msg("sve512_f32_chain_svd: SDC detected");
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));
    return EXIT_SUCCESS;
}

static int sve512_f32_chain_svd_cleanup(struct test *test)
{
    delete static_cast<SveF32ChainSvdData *>(test->data);
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(sve512_f32_chain_svd,
             "SVD-format SVE f32 FMLA chain: lanes-proportional SVD-scale "
             "working set + nested block traversal (all-VL 128..1024-bit "
             "via arm64_require_sve_vl), byte-exact vs scalar fmaf()")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_f32_chain_svd_init,
    .test_run = sve512_f32_chain_svd_run,
    .test_cleanup = sve512_f32_chain_svd_cleanup,
    .fracture_loop_count = 0,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
