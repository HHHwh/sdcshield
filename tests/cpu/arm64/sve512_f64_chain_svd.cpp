/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve512_f64_chain_svd
 * @parblock
 * SVD-format f64 FMLA dependency chain SDC stress — the SVE datapath
 * with an SVD-scale working set.
 *
 * Differs from sve512_f64_chain_arm:
 *   - working set scales with the lanes (5.9..47.3 MB across m and a
 *     over the 128..1024-bit tiers) vs 64 KB, so it overflows L2
 *     (512 KB) at every tier and forces continuous L1D<->L2<->L3
 *     traffic, mirroring eigen_svd_cdouble_sve's complex<double>
 *     300x300 footprint;
 *   - nested block traversal (BLOCK_ROWS x BLOCK_COLS) matching Eigen
 *     BDCSVD's block-of-matrix access pattern instead of a flat stream.
 *
 * Still a serial svmla_f64_x chain (512 steps), byte-exact compared to a
 * scalar std::fma reference. Compiled WITHOUT -msve-vector-bits so it uses
 * the CPU runtime vector length — all-VL (128..1024-bit, all-VL Phase 2
 * P3; the pre-P3 512-bit single-value gate and its issue #182 caveat are
 * cleared by the four-tier dual-kernel evidence in gem5-fi
 * feat/sve512-dual). test_init gates via the framework layer
 * (arm64_require_sve_vl: HWCAP_SVE capability + range check in one call,
 * no SVE instruction executes before the probe), returning a clean
 * EXIT_SKIP on SVE-less CPUs; test_run re-checks the VL and fails closed
 * on drift.
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

// SVD-scale working set: 300x300 doubles = 720 KB. With two operand
// streams (m and a) this is 1.44 MB total — matches eigen_svd_cdouble_sve's
// complex<double> 300x300 footprint (1.44 MB) that overflows L2.
static constexpr size_t SVD_ROWS = 300;
static constexpr size_t SVD_COLS = 300;
static constexpr size_t SVD_BLOCK_ROWS = 16;   // outer block size
static constexpr size_t SVD_BLOCK_COLS = 16;   // inner block size

static const uint64_t F64_FINITE[] = {
    0x3FF5555555555555ULL, 0xBFFAAAAAAAAAAAAAULL, 0x3FD5555555555555ULL,
    0xBFFAAAAAAAAAAAAAULL, 0x0015555555555555ULL, 0x801AAAAAAAAAAAAAULL,
    0x3FF0000000000000ULL, 0xBFF0000000000000ULL, 0x0005555555555555ULL,
    0x800AAAAAAAAAAAAAULL, 0x3FF999999999999AULL, 0xBFF6666666666667ULL,
    0x3FE5555555555555ULL, 0xBFE6666666666667ULL, 0x3FF3333333333333ULL,
    0xBFFCCCCCCCCCCCCDULL,
};
static constexpr size_t F64_FINITE_SIZE =
    sizeof(F64_FINITE) / sizeof(F64_FINITE[0]);

static inline uint64_t splitmix64(uint64_t x)
{
    uint64_t z = (x + 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

struct SveF64ChainSvdData {
    size_t vl_d;

    // Per-lane seeds.
    std::vector<uint64_t> seeds_f64;

    // SVD-scale operand streams, layout: m[block_row][block_col][step][lane]
    // Total elements: SVD_ROWS * SVD_COLS for each of m and a.
    std::vector<uint64_t> m_f64;
    std::vector<uint64_t> a_f64;
};

#ifdef __aarch64__

// Block-major stream layout: [block_row][block_col][step][lane], one
// CHAIN_STEPS x VL contiguous slab per block origin (19x19 blocks).
// Block (br, bc) slab starts at ((br/16)*19 + bc/16) * (STEPS*VL).
static inline size_t block_slab_base(size_t row, size_t col, size_t lanes)
{
    const size_t n_blocks_c = (SVD_COLS + SVD_BLOCK_COLS - 1) / SVD_BLOCK_COLS;
    return ((row / SVD_BLOCK_ROWS) * n_blocks_c + (col / SVD_BLOCK_COLS)) *
           (CHAIN_STEPS * lanes);
}

static void f64_chain_golden(const SveF64ChainSvdData *d, uint64_t *out,
                             size_t row, size_t col)
{
    const size_t lanes = d->vl_d;
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint64_t accw = d->seeds_f64[lane];
        double acc;
        memcpy(&acc, &accw, 8);
        for (size_t i = 0; i < CHAIN_STEPS; ++i) {
            double m, a;
            const size_t base = block_slab_base(row, col, lanes) + i * lanes;
            memcpy(&m, &d->m_f64[base + lane], 8);
            memcpy(&a, &d->a_f64[base + lane], 8);
            acc = std::fma(m, a, acc);
        }
        memcpy(&accw, &acc, 8);
        out[lane] = accw;
    }
}

static void f64_chain_hw(const SveF64ChainSvdData *d, uint64_t *out,
                         size_t row, size_t col)
{
    const size_t lanes = d->vl_d;
    const svbool_t pg = svptrue_b64();
    svfloat64_t acc = svld1_f64(pg,
        reinterpret_cast<const double *>(d->seeds_f64.data()));
    for (size_t i = 0; i < CHAIN_STEPS; ++i) {
        const size_t base = block_slab_base(row, col, lanes) + i * lanes;
        svfloat64_t m = svld1_f64(pg,
            reinterpret_cast<const double *>(&d->m_f64[base]));
        svfloat64_t a = svld1_f64(pg,
            reinterpret_cast<const double *>(&d->a_f64[base]));
        acc = svmla_f64_x(pg, acc, m, a);
    }
    svst1_f64(pg, reinterpret_cast<double *>(out), acc);
}

static void report_f64_lane_mismatch(const char *tag, size_t lane,
                                     uint64_t golden, uint64_t actual)
{
    log_warning("%s: lane %zu golden=0x%016" PRIx64 " "
                "actual=0x%016" PRIx64 " xor=0x%016" PRIx64,
                tag, lane, golden, actual, golden ^ actual);
}

static int sve512_f64_chain_svd_init(struct test *test)
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
        auto data = std::make_unique<SveF64ChainSvdData>();
        data->vl_d = (size_t)svl / 8;   /* f64 lanes = SVL bytes / 8 */
        if (data->vl_d < 2 || data->vl_d > 16) {
            log_error("sve512_f64_chain_svd: f64 lanes %zu outside "
                      "[2,16] — unit drift (svl %ld bytes)",
                      data->vl_d, svl);
            return EXIT_FAILURE;
        }

        /* randomization hardening H12': framework RNG per run (-s
         * reproducible) instead of fixed splitmix/table indices; the
         * |x| <= 2 finite-band invariant is preserved (exponent capped
         * at 0x3FF), and 25% of entries still draw from the high-Hamming
         * F64_FINITE table for gate-toggle density. */
        auto random_finite_f64 = []() -> uint64_t {
            if ((random32() & 3) == 0)
                return F64_FINITE[random32() % F64_FINITE_SIZE];
            uint64_t sign = (random64() & 1ULL) << 63;
            uint64_t mantissa = random64() & 0x000FFFFFFFFFFFFFULL;
            uint64_t exp = 0x3FEU + (random64() & 1ULL);  /* [0.5, 2) */
            return sign | (exp << 52) | mantissa;
        };

        /* Value-domain knob (SEVI ASPLOS'26 Obs.19: bounded vs unbounded
         * inputs differ up to 245x in SDC frequency on the same core).
         * bounded=1 (default): the |x| <= 2 finite band above — the
         * randomization-hardened path, untouched.
         * bounded=0: full-entropy mantissas with the exponent clamped to
         * [2^-8, 1) so the chain still cannot overflow to Inf/NaN and
         * the byte-exact golden stays valid. */
        int64_t bounded = get_testspecific_knob_value_int(test, "bounded", 1);
        auto random_unbounded_f64 = []() -> uint64_t {
            uint64_t sign = (random64() & 1ULL) << 63;
            uint64_t mantissa = random64() & 0x000FFFFFFFFFFFFFULL;
            uint64_t exp = 0x3F7ULL + ((random64() >> 61) & 7ULL);
            return sign | (exp << 52) | mantissa;
        };
        data->seeds_f64.resize(data->vl_d);
        for (size_t lane = 0; lane < data->vl_d; ++lane) {
            data->seeds_f64[lane] = 0x3FF0000000000000ULL |
                (random64() & 0x000FFFFFFFFFFFFFULL);
        }

        // SVD-scale operand streams, block-major: one contiguous
        // CHAIN_STEPS x VL slab per 16x16 block origin, 19x19 blocks.
        // = 361 * 512 * VL f64 elements — the working set is proportional
        // to the lanes: across m and a the four tiers (128/256/512/1024-
        // bit) total 5.9/11.8/23.7/47.3 MB, every tier far beyond L2,
        // keeping the SVD overflow intent (and staying far from the old
        // layout's 2.7 GiB-per-stream blowup).
        const size_t n_blocks_r = (SVD_ROWS + SVD_BLOCK_ROWS - 1) / SVD_BLOCK_ROWS;
        const size_t n_blocks_c = (SVD_COLS + SVD_BLOCK_COLS - 1) / SVD_BLOCK_COLS;
        const size_t total = n_blocks_r * n_blocks_c * CHAIN_STEPS * data->vl_d;
        data->m_f64.resize(total);
        data->a_f64.resize(total);
        for (size_t i = 0; i < total; ++i) {
            data->m_f64[i] = bounded ? random_finite_f64()
                                     : random_unbounded_f64();
            data->a_f64[i] = bounded ? random_finite_f64()
                                     : random_unbounded_f64();
        }

        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory,
                 "sve512_f64_chain_svd init: %s", e.what());
        return EXIT_SKIP;
    }
}

static int sve512_f64_chain_svd_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<SveF64ChainSvdData *>(test->data);

    /* All-VL policy #3: fail closed on init/run VL drift. */
    if (svcntd() != d->vl_d) {
        log_error("sve512_f64_chain_svd: VL drifted between init (%zu "
                  "lanes) and run (%u lanes)", d->vl_d, (unsigned)svcntd());
        report_fail_msg("sve512_f64_chain_svd: SVE VL drifted between "
                        "init and run");
    }

    std::vector<uint64_t> gold64(d->vl_d);
    std::vector<uint64_t> hw(d->vl_d);

    do {
        bool all_passed = true;

        // SVD-format nested block traversal.
        for (size_t br = 0; br < SVD_ROWS; br += SVD_BLOCK_ROWS) {
            for (size_t bc = 0; bc < SVD_COLS; bc += SVD_BLOCK_COLS) {
                f64_chain_golden(d, gold64.data(), br, bc);
                f64_chain_hw(d, hw.data(), br, bc);
                for (size_t lane = 0; lane < d->vl_d; ++lane) {
                    if (hw[lane] != gold64[lane]) {
                        report_f64_lane_mismatch(
                            "sve512_f64_chain_svd", lane,
                            gold64[lane], hw[lane]);
                        all_passed = false;
                    }
                }
            }
        }

        if (!all_passed) {
            report_fail_msg(
                "sve512_f64_chain_svd: SVE f64 chain SDC detected");
            return EXIT_FAILURE;
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int sve512_f64_chain_svd_cleanup(struct test *test)
{
    delete static_cast<SveF64ChainSvdData *>(test->data);
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(sve512_f64_chain_svd,
             "SVD-format SVE f64 FMLA chain: lanes-proportional SVD-scale "
             "working set + nested block traversal (all-VL 128..1024-bit "
             "via arm64_require_sve_vl), byte-exact vs scalar fma()")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_f64_chain_svd_init,
    .test_run = sve512_f64_chain_svd_run,
    .test_cleanup = sve512_f64_chain_svd_cleanup,
    .fracture_loop_count = 0,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
