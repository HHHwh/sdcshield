/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve512_f64_chain_svd
 * @parblock
 * SVD-format f64 FMLA dependency chain SDC stress — the SVE-512 datapath
 * with an SVD-scale working set.
 *
 * Differs from sve512_f64_chain_arm:
 *   - working set 1.44 MB (300x300 complex<double>-equivalent) vs 64 KB,
 *     so it overflows L2 (512 KB) and forces continuous L1D<->L2<->L3
 *     traffic, mirroring eigen_svd_cdouble_sve's memory footprint;
 *   - nested block traversal (BLOCK_ROWS x BLOCK_COLS) matching Eigen
 *     BDCSVD's block-of-matrix access pattern instead of a flat stream.
 *
 * Still a serial svmla_f64_x chain (512 steps), byte-exact compared to a
 * scalar std::fma reference. Compiled WITHOUT -msve-vector-bits so it uses
 * the CPU runtime vector length (512-bit on HiSilicon 0xd22). Non-SVE CPUs
 * return a clean EXIT_SKIP.
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
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SVE not available; sve512_f64_chain_svd requires SVE");
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
        auto data = std::make_unique<SveF64ChainSvdData>();
        data->vl_d = svcntd();

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
        data->seeds_f64.resize(data->vl_d);
        for (size_t lane = 0; lane < data->vl_d; ++lane) {
            data->seeds_f64[lane] = 0x3FF0000000000000ULL |
                (random64() & 0x000FFFFFFFFFFFFFULL);
        }

        // SVD-scale operand streams, block-major: one contiguous
        // CHAIN_STEPS x VL slab per 16x16 block origin, 19x19 blocks.
        // = 361 * 512 * VL f64 elements (~11.8 MB per stream at a 512-bit
        // VL, 23.7 MB across m and a — 46x L2, matching the SVD working-
        // set goal without the old layout's 2.7 GiB-per-stream blowup).
        const size_t n_blocks_r = (SVD_ROWS + SVD_BLOCK_ROWS - 1) / SVD_BLOCK_ROWS;
        const size_t n_blocks_c = (SVD_COLS + SVD_BLOCK_COLS - 1) / SVD_BLOCK_COLS;
        const size_t total = n_blocks_r * n_blocks_c * CHAIN_STEPS * data->vl_d;
        data->m_f64.resize(total);
        data->a_f64.resize(total);
        for (size_t i = 0; i < total; ++i) {
            data->m_f64[i] = random_finite_f64();
            data->a_f64[i] = random_finite_f64();
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
                "sve512_f64_chain_svd: SVE-512 f64 chain SDC detected");
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
             "SVD-format SVE-512 f64 FMLA chain: 1.44 MB working set + "
             "nested block traversal, byte-exact vs scalar fma()")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_f64_chain_svd_init,
    .test_run = sve512_f64_chain_svd_run,
    .test_cleanup = sve512_f64_chain_svd_cleanup,
    .fracture_loop_count = -1,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
