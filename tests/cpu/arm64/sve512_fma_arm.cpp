/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sve512_fma_arm
 * @parblock
 * SVE full-vector-length FMA SDC stress — the SVE-512 datapath that no
 * existing test exercises. The only SVE test in the suite
 * (eigen_svd_cdouble_sve) is built with -msve-vector-bits=128, so on a
 * CPU with a 512-bit SVE vector length (HiSilicon 0xd22,
 * sve_default_vector_length=64) it still only uses 128-bit vectors: the
 * remaining 384 bits of every FMA/predicate/gather pipeline stage stay
 * dark. NEON tests (neon_test, fsu_byteexact_arm) cover the 128-bit NEON
 * pipes; nothing covers SVE at full width.
 *
 * This test is compiled WITHOUT -msve-vector-bits so the GCC sizeless
 * svfloat64_t / svfloat32_t types adopt the CPU runtime vector length —
 * the full 512-bit datapath on 0xd22 (and a proportional width on any
 * other SVE CPU). test_init gates via the framework layer
 * (arm64_require_sve_vl: HWCAP_SVE capability + 128..1024-bit range in
 * one call, no SVE instruction executes before the gate, all-VL Phase 2
 * P6), returning a clean EXIT_SKIP on SVE-less CPUs (e.g. the Kunpeng
 * 920 login node); test_run re-checks the VL and fails closed on drift.
 *
 * Four byte-exact-checked workloads run in every iteration:
 *
 *   1. **f64 FMLA dependency chain**: a serial chain of svmla_f64_x over
 *      high-Hamming finite f64 lanes (|x| <= 2, so 512 steps of products
 *      can never overflow to Inf/NaN — every lane stays byte-exact
 *      comparable), each step feeding the next — a real serial dependency
 *      through the wide FMA pipe, not ILP. The golden is recomputed in
 *      scalar C++ with the identical operation order, so any lane flip
 *      in the wide datapath is a byte-exact mismatch.
 *   2. **f64 special-value chain**: the same chain structure over the
 *      IEEE-754 special-value table (NaN/sNaN/±Inf/±0/±1), compared by
 *      category (fsu_byteexact_arm precedent): a category change is
 *      always a failure.
 *   3. **f32 FMLA dependency chain**: the same structure at f32 width
 *      (svmla_f32_x), exercising the 16-lane f32 FMA/predicate datapath
 *      at full 512-bit width.
 *   4. **gather/scatter round-trip**: svld1_gather_u64index_f64 loads
 *      f64 elements at element-index granularity from a permuted index
 *      table, FMA-transforms them, and svst1_scatter_u64index_f64 writes
 *      them to a scratch region; the region is byte-compared against the
 *      scalar-computed golden. This drives the SVE memory pipeline
 *      (gather/scatter AGU + lane reordering) that per-lane FMA loops
 *      never touch.
 *
 * SDC detection: byte-exact lane compares against deterministic software
 * references, with per-lane golden/actual/XOR logging on mismatch.
 * report_fail_msg on any mismatch. ARM64-native; non-SVE CPUs return a
 * clean EXIT_SKIP from init. Built only on aarch64.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <cmath>
#include <vector>

#ifdef __aarch64__
#include <arm_sve.h>
#include <device/cpu/arm64/arm64_vl.h>
#endif

// Serial FMA chain length per iteration.
static constexpr size_t CHAIN_STEPS = 512;

// Special-value (category-checked) chain length.
static constexpr size_t SPECIAL_CHAIN_STEPS = 64;

// Finite high-Hamming f64 patterns: |x| <= 2, so products stay <= 4 and
// 512-step sums stay < 2048 — the chain can never overflow or NaN, so
// every lane of every step is byte-exact comparable. Successive entries
// toggle many mantissa/sign bits (high gate-toggle activity).
static const uint64_t F64_FINITE[] = {
    0x3FF5555555555555ULL,   //  1.3333...
    0xBFFAAAAAAAAAAAAAULL,   // -1.6667...
    0x3FD5555555555555ULL,   //  0.3333...
    0xBFEAAAAAAAAAAAAAULL,   // -0.8333...
    0x0015555555555555ULL,   //  denormal +
    0x801AAAAAAAAAAAAAULL,   //  denormal -
    0x3FF0000000000000ULL,   //  1.0
    0xBFF0000000000000ULL,   // -1.0
    0x0005555555555555ULL,   //  denormal +
    0x800AAAAAAAAAAAAAULL,   //  denormal -
    0x3FF999999999999AULL,   //  1.6
    0xBFF6666666666667ULL,   // -1.4
    0x3FE5555555555555ULL,   //  0.6667...
    0xBFE6666666666667ULL,   // -0.7
    0x3FF3333333333333ULL,   //  1.2
    0xBFFCCCCCCCCCCCCDULL,   // -1.8
};
static constexpr size_t F64_FINITE_SIZE =
    sizeof(F64_FINITE) / sizeof(F64_FINITE[0]);

// Special-value table for the category-checked chain.
static const uint64_t F64_SPECIAL[] = {
    0x0000000000000000ULL,   // +0.0
    0x8000000000000000ULL,   // -0.0
    0x7FF8000000000000ULL,   // qNaN
    0x7FF0000000000001ULL,   // sNaN
    0x7FF0000000000000ULL,   // +Inf
    0xFFF0000000000000ULL,   // -Inf
    0x3FF0000000000000ULL,   // 1.0
    0xBFF0000000000000ULL,   // -1.0
};
static constexpr size_t F64_SPECIAL_SIZE =
    sizeof(F64_SPECIAL) / sizeof(F64_SPECIAL[0]);

// f32 finite patterns (|x| <= 2).
static const uint32_t F32_FINITE[] = {
    0x3FAAAAABU, 0xBFAAAAABU, 0x3EAAAAABU, 0xBF2AAAABU,
    0x3F800000U, 0xBF800000U, 0x00555555U, 0x80AAAAAAU,
    0x3FCCCCCDU, 0xBFCCCCCDU, 0x3F333333U, 0xBF333333U,
    0x3E555555U, 0xBE555555U, 0x3F666666U, 0xBF666666U,
};
static constexpr size_t F32_FINITE_SIZE =
    sizeof(F32_FINITE) / sizeof(F32_FINITE[0]);

// Deterministic seed generator (same family as other tests).
static inline uint64_t splitmix64(uint64_t x)
{
    uint64_t z = (x + 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

struct SveFmaData {
    // Runtime vector geometry.
    size_t vl_d;   // svcntd() — f64 lanes per full vector
    size_t vl_w;   // svcntw() — f32 lanes per full vector

    // Per-lane seed accumulators (finite, [1.0, 2.0)).
    std::vector<uint64_t> seeds_f64;
    std::vector<uint32_t> seeds_f32;

    // f64 finite-chain operand streams, layout [step * vl_d + lane].
    std::vector<uint64_t> m_f64;
    std::vector<uint64_t> a_f64;

    // f64 special-chain operand streams, same layout.
    std::vector<uint64_t> sm_f64;
    std::vector<uint64_t> sa_f64;

    // f32 finite-chain operand streams, layout [step * vl_w + lane].
    std::vector<uint32_t> m_f32;
    std::vector<uint32_t> a_f32;

    // Gather/scatter round-trip buffers (f64 workspace).
    std::vector<uint64_t> gather_src;     // source pool
    std::vector<uint64_t> gather_idx;     // u64 element indices (permuted)
    std::vector<uint64_t> scatter_dst;    // hardware scatter destination
    std::vector<uint64_t> scatter_golden; // scalar golden destination
};

#ifdef __aarch64__

// ---------------------------------------------------------------------------
// Scalar goldens: identical operation order and indexing to the SVE chains
// (stream layout is [step * lanes + lane]).
// ---------------------------------------------------------------------------
static void f64_chain_golden(const SveFmaData *d, uint64_t *out)
{
    const size_t lanes = d->vl_d;
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint64_t accw = d->seeds_f64[lane];
        double acc;
        memcpy(&acc, &accw, 8);
        for (size_t i = 0; i < CHAIN_STEPS; ++i) {
            double m, a;
            memcpy(&m, &d->m_f64[i * lanes + lane], 8);
            memcpy(&a, &d->a_f64[i * lanes + lane], 8);
            acc = std::fma(m, a, acc);
        }
        memcpy(&accw, &acc, 8);
        out[lane] = accw;
    }
}

static void f64_special_golden(const SveFmaData *d, uint64_t *out)
{
    const size_t lanes = d->vl_d;
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint64_t accw = d->seeds_f64[lane];
        double acc;
        memcpy(&acc, &accw, 8);
        for (size_t i = 0; i < SPECIAL_CHAIN_STEPS; ++i) {
            double m, a;
            memcpy(&m, &d->sm_f64[i * lanes + lane], 8);
            memcpy(&a, &d->sa_f64[i * lanes + lane], 8);
            acc = std::fma(m, a, acc);
        }
        memcpy(&accw, &acc, 8);
        out[lane] = accw;
    }
}

static void f32_chain_golden(const SveFmaData *d, uint32_t *out)
{
    const size_t lanes = d->vl_w;
    for (size_t lane = 0; lane < lanes; ++lane) {
        uint32_t accw = d->seeds_f32[lane];
        float acc;
        memcpy(&acc, &accw, 4);
        for (size_t i = 0; i < CHAIN_STEPS; ++i) {
            float m, a;
            memcpy(&m, &d->m_f32[i * lanes + lane], 4);
            memcpy(&a, &d->a_f32[i * lanes + lane], 4);
            acc = std::fma(m, a, acc);
        }
        memcpy(&accw, &acc, 4);
        out[lane] = accw;
    }
}

// Category compare (fsu_byteexact_arm precedent): NaN/±Inf stay in
// category; finite values byte-exact.
static inline bool f64_lane_ok(uint64_t golden, uint64_t actual)
{
    double g, a;
    memcpy(&g, &golden, 8);
    memcpy(&a, &actual, 8);
    if (std::isnan(g)) return std::isnan(a);
    if (std::isinf(g)) return std::isinf(a) && ((g > 0) == (a > 0));
    if (std::isnan(a) || std::isinf(a)) return false;
    return golden == actual;
}

static inline bool f32_lane_ok(uint32_t golden, uint32_t actual)
{
    float g, a;
    memcpy(&g, &golden, 4);
    memcpy(&a, &actual, 4);
    if (std::isnan(g)) return std::isnan(a);
    if (std::isinf(g)) return std::isinf(a) && ((g > 0) == (a > 0));
    if (std::isnan(a) || std::isinf(a)) return false;
    return golden == actual;
}

// Finite high-Hamming f64 patterns: |x| <= 2, so products stay <= 4 and
// 512-step sums stay < 2048 — the chain can never overflow or NaN, so

// ---------------------------------------------------------------------------
// Hardware chains (SVE, full runtime VL).
// ---------------------------------------------------------------------------
static void f64_chain_hw(const SveFmaData *d, uint64_t *out)
{
    const size_t lanes = d->vl_d;
    const svbool_t pg = svptrue_b64();
    svfloat64_t acc = svld1_f64(pg, reinterpret_cast<const double *>(d->seeds_f64.data()));
    for (size_t i = 0; i < CHAIN_STEPS; ++i) {
        svfloat64_t m = svld1_f64(pg, reinterpret_cast<const double *>(&d->m_f64[i * lanes]));
        svfloat64_t a = svld1_f64(pg, reinterpret_cast<const double *>(&d->a_f64[i * lanes]));
        acc = svmla_f64_x(pg, acc, m, a);
    }
    svst1_f64(pg, reinterpret_cast<double *>(out), acc);
}

static void f64_special_chain_hw(const SveFmaData *d, uint64_t *out)
{
    const size_t lanes = d->vl_d;
    const svbool_t pg = svptrue_b64();
    svfloat64_t acc = svld1_f64(pg, reinterpret_cast<const double *>(d->seeds_f64.data()));
    for (size_t i = 0; i < SPECIAL_CHAIN_STEPS; ++i) {
        svfloat64_t m = svld1_f64(pg, reinterpret_cast<const double *>(&d->sm_f64[i * lanes]));
        svfloat64_t a = svld1_f64(pg, reinterpret_cast<const double *>(&d->sa_f64[i * lanes]));
        acc = svmla_f64_x(pg, acc, m, a);
    }
    svst1_f64(pg, reinterpret_cast<double *>(out), acc);
}

static void f32_chain_hw(const SveFmaData *d, uint32_t *out)
{
    const size_t lanes = d->vl_w;
    const svbool_t pg = svptrue_b32();
    svfloat32_t acc = svld1_f32(pg, reinterpret_cast<const float *>(d->seeds_f32.data()));
    for (size_t i = 0; i < CHAIN_STEPS; ++i) {
        svfloat32_t m = svld1_f32(pg, reinterpret_cast<const float *>(&d->m_f32[i * lanes]));
        svfloat32_t a = svld1_f32(pg, reinterpret_cast<const float *>(&d->a_f32[i * lanes]));
        acc = svmla_f32_x(pg, acc, m, a);
    }
    svst1_f32(pg, reinterpret_cast<float *>(out), acc);
}

// Gather/scatter round-trip: gather from pool at permuted element indices,
// FMA-transform, scatter to dst at the same indices.

// Scalar golden for the gather/scatter round-trip (identical per-element
// op: v = v * v + 1.5, matching svmla(v, v, 1.5)).
static void gather_scatter_golden(SveFmaData *d)
{
    // The hardware scatter writes dst[idx[off + lane]] for each vector
    // lane — the golden must index through the same indirection, not
    // linearly (a permutation does NOT commute with the scatter).
    for (size_t i = 0; i < d->gather_idx.size(); ++i) {
        double v;
        memcpy(&v, &d->gather_src[d->gather_idx[i]], 8);
        v = std::fma(v, v, 1.5);
        memcpy(&d->scatter_golden[d->gather_idx[i]], &v, 8);
    }
}

static void gather_scatter_hw(SveFmaData *d)
{
    const size_t n = d->gather_idx.size();
    const size_t lanes = d->vl_d;
    const svbool_t pg = svptrue_b64();
    const svfloat64_t one_half = svdup_f64(1.5);
    for (size_t off = 0; off < n; off += lanes) {
        svuint64_t vidx = svld1_u64(pg, &d->gather_idx[off]);
        svfloat64_t v = svld1_gather_u64index_f64(pg, reinterpret_cast<const double *>(d->gather_src.data()),
                                                  vidx);
        v = svmla_f64_x(pg, one_half, v, v);
        svst1_scatter_u64index_f64(pg, reinterpret_cast<double *>(d->scatter_dst.data()), vidx, v);
    }
}

// ---------------------------------------------------------------------------
// Per-lane failure logging (fsu_byteexact_arm style).
// ---------------------------------------------------------------------------
static void report_f64_lane_mismatch(const char *tag, size_t lane,
                                     uint64_t golden, uint64_t actual)
{
    log_warning("%s: lane %zu golden=0x%016" PRIx64 " "
                "actual=0x%016" PRIx64 " xor=0x%016" PRIx64,
                tag, lane, golden, actual, golden ^ actual);
}

// ---------------------------------------------------------------------------
// test_init
// ---------------------------------------------------------------------------
static int sve512_fma_arm_init(struct test *test)
{
#ifndef __aarch64__
    (void)test;
    return EXIT_SUCCESS;
#else
    /* All-VL Phase 2 P6: capability + 128..1024-bit range check in one
       framework-layer call, replacing the getauxval hand-probe (no SVE
       instruction executes before the gate). arm64_require_sve_vl
       returns the VL in BYTES — f64 lanes = svl/8, f32 lanes = svl/4,
       both range-checked below (release-effective, NDEBUG-proof). */
    const long svl = arm64_require_sve_vl(test, 128, 1024);
    if (svl < 0)
        return EXIT_SKIP;

    try {
        auto data = std::make_unique<SveFmaData>();
        data->vl_d = (size_t)svl / 8;   /* f64 lanes = SVL bytes / 8 */
        data->vl_w = (size_t)svl / 4;   /* f32 lanes = SVL bytes / 4 */
        if (data->vl_d < 2 || data->vl_d > 16) {
            log_error("sve512_fma_arm: f64 lanes %zu outside [2,16] "
                      "— unit drift (svl %ld bytes)", data->vl_d, svl);
            return EXIT_FAILURE;
        }
        if (data->vl_w < 4 || data->vl_w > 32) {
            log_error("sve512_fma_arm: f32 lanes %zu outside [4,32] "
                      "— unit drift (svl %ld bytes)", data->vl_w, svl);
            return EXIT_FAILURE;
        }

        // Per-lane seed accumulators: finite in [1.0, 2.0).
        data->seeds_f64.resize(data->vl_d);
        for (size_t lane = 0; lane < data->vl_d; ++lane) {
            data->seeds_f64[lane] = 0x3FF0000000000000ULL |
                (splitmix64(0xC0FFEE00ULL + lane) & 0x000FFFFFFFFFFFFFULL);
        }
        data->seeds_f32.resize(data->vl_w);
        for (size_t lane = 0; lane < data->vl_w; ++lane) {
            data->seeds_f32[lane] = 0x3F800000U |
                ((uint32_t)splitmix64(0xC0FFEE00ULL + lane) & 0x007FFFFFU);
        }

        // f64 finite chain: CHAIN_STEPS full vectors of operands.
        const size_t n64 = CHAIN_STEPS * data->vl_d;
        data->m_f64.resize(n64);
        data->a_f64.resize(n64);
        for (size_t i = 0; i < n64; ++i) {
            data->m_f64[i] = F64_FINITE[(i * 7 + 1) % F64_FINITE_SIZE];
            data->a_f64[i] = F64_FINITE[(i * 5 + 3) % F64_FINITE_SIZE];
        }

        // f64 special chain: SPECIAL_CHAIN_STEPS vectors.
        const size_t ns64 = SPECIAL_CHAIN_STEPS * data->vl_d;
        data->sm_f64.resize(ns64);
        data->sa_f64.resize(ns64);
        for (size_t i = 0; i < ns64; ++i) {
            data->sm_f64[i] = F64_SPECIAL[(i * 3 + 1) % F64_SPECIAL_SIZE];
            data->sa_f64[i] = F64_SPECIAL[(i * 5 + 2) % F64_SPECIAL_SIZE];
        }

        // f32 finite chain: CHAIN_STEPS full vectors.
        const size_t n32 = CHAIN_STEPS * data->vl_w;
        data->m_f32.resize(n32);
        data->a_f32.resize(n32);
        for (size_t i = 0; i < n32; ++i) {
            data->m_f32[i] = F32_FINITE[(i * 7 + 1) % F32_FINITE_SIZE];
            data->a_f32[i] = F32_FINITE[(i * 5 + 3) % F32_FINITE_SIZE];
        }

        // Gather/scatter workspace: 64 vectors' worth of elements.
        const size_t gn = 64 * data->vl_d;
        data->gather_src.resize(gn);
        data->gather_idx.resize(gn);
        data->scatter_dst.assign(gn, 0);
        data->scatter_golden.resize(gn);
        for (size_t i = 0; i < gn; ++i) {
            data->gather_src[i] = data->seeds_f64[i % data->vl_d];
            data->gather_idx[i] = i;
        }
        // Permute indices (deterministic Fisher-Yates via splitmix64).
        for (size_t i = gn - 1; i > 0; --i) {
            size_t j = (size_t)(splitmix64(0xBEEF0000ULL + i) % (i + 1));
            uint64_t t = data->gather_idx[i];
            data->gather_idx[i] = data->gather_idx[j];
            data->gather_idx[j] = t;
        }

        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory, "sve512_fma_arm init: %s",
                 e.what());
        return EXIT_SKIP;
    }
#endif
}

// f32 variant of the mismatch logger.
static void report_f32_lane_mismatch(const char *tag, size_t lane,
                                     uint32_t golden, uint32_t actual)
{
    log_warning("%s: lane %zu golden=0x%08" PRIx32 " "
                "actual=0x%08" PRIx32 " xor=0x%08" PRIx32,
                tag, lane, golden, actual, golden ^ actual);
}

// ---------------------------------------------------------------------------
// test_run: four byte-exact-checked workloads per iteration.
// ---------------------------------------------------------------------------
static int sve512_fma_arm_run(struct test *test, int cpu)
{
    (void)cpu;
#ifndef __aarch64__
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 SVE-512 FMA chains "
             "required");
    return EXIT_SKIP;
#else
    auto *d = static_cast<SveFmaData *>(test->data);

    /* All-VL policy #3: fail closed on init/run VL drift (the operand
       streams and goldens are sized in init at its lanes). */
    if (svcntd() != d->vl_d || svcntw() != d->vl_w) {
        log_error("sve512_fma_arm: VL drifted between init (f64 %zu, "
                  "f32 %zu lanes) and run (f64 %u, f32 %u lanes)",
                  d->vl_d, d->vl_w, (unsigned)svcntd(),
                  (unsigned)svcntw());
        report_fail_msg("sve512_fma_arm: SVE VL drifted between init "
                        "and run");
    }

    // Goldens, recomputed each iteration (deterministic, same op order).
    std::vector<uint64_t> gold64(d->vl_d);
    std::vector<uint64_t> golds64(d->vl_d);
    std::vector<uint32_t> gold32(d->vl_w);
    f64_chain_golden(d, gold64.data());
    f64_special_golden(d, golds64.data());
    f32_chain_golden(d, gold32.data());

    do {
        bool all_passed = true;

        // ---- (1) f64 finite FMLA chain.
        {
            std::vector<uint64_t> hw(d->vl_d);
            f64_chain_hw(d, hw.data());
            for (size_t lane = 0; lane < d->vl_d; ++lane) {
                if (hw[lane] != gold64[lane]) {
                    report_f64_lane_mismatch("sve512_f64_chain", lane,
                                             gold64[lane], hw[lane]);
                    all_passed = false;
                }
            }
        }

        // ---- (2) f64 special-value chain (category compare).
        {
            std::vector<uint64_t> hw(d->vl_d);
            f64_special_chain_hw(d, hw.data());
            for (size_t lane = 0; lane < d->vl_d; ++lane) {
                if (!f64_lane_ok(golds64[lane], hw[lane])) {
                    report_f64_lane_mismatch("sve512_f64_special", lane,
                                             golds64[lane], hw[lane]);
                    all_passed = false;
                }
            }
        }

        // ---- (3) f32 finite FMLA chain.
        {
            std::vector<uint32_t> hw(d->vl_w);
            f32_chain_hw(d, hw.data());
            for (size_t lane = 0; lane < d->vl_w; ++lane) {
                if (hw[lane] != gold32[lane]) {
                    report_f32_lane_mismatch("sve512_f32_chain", lane,
                                             gold32[lane], hw[lane]);
                    all_passed = false;
                }
            }
        }

        // ---- (4) gather/scatter round-trip.
        {
            gather_scatter_hw(d);
            gather_scatter_golden(d);
            for (size_t i = 0; i < d->scatter_dst.size(); ++i) {
                if (d->scatter_dst[i] != d->scatter_golden[i]) {
                    report_f64_lane_mismatch("sve512_gather_scatter", i,
                                             d->scatter_golden[i],
                                             d->scatter_dst[i]);
                    all_passed = false;
                }
            }
            // Reset the destination for the next iteration.
            std::fill(d->scatter_dst.begin(), d->scatter_dst.end(), 0);
        }

        if (!all_passed) {
            report_fail_msg("sve512_fma_arm: SVE-512 FMA chain SDC detected");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#endif
}

// ---------------------------------------------------------------------------
// test_cleanup
// ---------------------------------------------------------------------------
static int sve512_fma_arm_cleanup(struct test *test)
{
#ifdef __aarch64__
    delete static_cast<SveFmaData *>(test->data);
#else
    (void)test;
#endif
    return EXIT_SUCCESS;
}

#endif // __aarch64__

DECLARE_TEST(sve512_fma_arm,
             "SVE full-vector-length FMA SDC stress: f64/f32 FMLA dependency "
             "chains + special-value category chain + gather/scatter "
             "round-trip at the CPU's runtime SVE vector length (all-VL "
             "128..1024-bit via arm64_require_sve_vl), byte-exact compared "
             "against deterministic software references")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sve512_fma_arm_init,
    .test_run = sve512_fma_arm_run,
    .test_cleanup = sve512_fma_arm_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
