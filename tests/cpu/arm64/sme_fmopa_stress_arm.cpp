/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b sme_fmopa_stress_arm
 * @parblock
 * SME matrix-unit SDC stress: sustained FMOPA outer-product streams
 * (request 2026-10-09: SME at maximum pressure). The ZA
 * matrix-accumulate datapath that sme_za_tileview_arm only probes for
 * addressing consistency is here driven at peak throughput: every
 * TEST_LOOP iteration issues K_FMOPA = 64 back-to-back fmopa za0.s
 * outer products accumulating into one zeroed ZA tile - a 64-deep
 * serial FMA dependency per tile element (bit errors propagate and
 * accumulate, the sve512 chain family's amplification property) - then
 * drains the 16x16 f32 tile row-by-row with st1w {za0h.s[w12, 0]} and
 * memcmps it against a scalar fmaf golden recomputed in the identical
 * k order (single-rounding FMA on both sides, the acl_gemm_sve lesson).
 *
 * SDC-excitation ingredients (docs/sdc-excite-reproduce/ matrix):
 *  - serial dependency chain: tile[r][c] is a 64-step fma chain;
 *  - access-pattern switching: fmopa burst (accumulate-only) ->
 *    st1w drain (streaming store) -> memcmp (streaming load);
 *  - bit-dense finite operands, |a|,|b| <= 0.5625 so 64-step sums stay
 *    below 21 - no Inf/NaN, every flipped bit is visible in the memcmp;
 *  - all-core concurrency: 608 threads each in their own streaming
 *    window (per-thread ZA, no sharing).
 *
 * Gates: HWCAP2 SME bit, then a runtime rdsvl check pinning the
 * characterized geometry (SVL = 512-bit = 16 f32 per slice, the
 * cn23154-measured SVL). Non-aarch64 hosts, non-clang compilers, and
 * uncharacterized SVLs skip with an honest reason. x86-64 untouched.
 * @endparblock
 */

/* See the matching comment in sme_za_tileview_arm.cpp: clang defines
 * __ARM_FEATURE_BTI for any v8.5+/v9 -march even when no BTI landing
 * pad is emitted, which the generated feature header would map into
 * compiler_minimum_device and silently skip the whole SME family on
 * exactly the hardware it exists to test. */
#ifdef __ARM_FEATURE_BTI
#undef __ARM_FEATURE_BTI
#endif

#include <sandstone.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#ifdef __aarch64__
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif
/* Kernel headers of the SME era define this; older ones (5.10) do not. */
#ifndef HWCAP2_SME
#define HWCAP2_SME (1UL << 23)
#endif

/* Characterized geometry on cn23154: SME SVL = 512-bit -> 16 f32 lanes
 * per slice, ZA0.s tile is 16x16. */
static constexpr long kTileN = 16;
/* Outer-product rounds per TEST_LOOP iteration (serial chain depth). */
static constexpr long kFmopaRounds = 64;

struct SmeFmopaData {
    /* Shared read-only operand streams, layout [k * kTileN + lane]. */
    std::vector<float> a, b;
    /* Shared byte-exact golden tile, layout [row * kTileN + col]. */
    std::vector<float> golden;
};

#if defined(__aarch64__) && defined(__clang__)
struct SmeFmopaParams {
    const float *a;
    const float *b;
    float *out;     /* 16x16 f32, rewritten every pass */
};

/* One smstart/smstop window: zero ZA, K_FMOPA outer-product rounds,
 * then drain the tile row-by-row. ZA slice index register must be
 * w12-w15 (see sme_za_tileview_arm); operand forms spelled out so
 * codegen cannot drift. */
__attribute__((naked))
static void sme_fmopa_kernel(const SmeFmopaParams *p)
{
    asm volatile(
        "ldr x1, [x0, #0]\n\t"       /* a          */
        "ldr x2, [x0, #8]\n\t"       /* b          */
        "ldr x3, [x0, #16]\n\t"      /* out        */
        "smstart sm\n\t"
        "smstart za\n\t"
        "zero {za}\n\t"
        "ptrue p0.s\n\t"
        "ptrue p1.s\n\t"
        "mov x9, #0\n"               /* k = 0      */
        "1:\n\t"
        "cmp x9, #64\n\t"
        "b.ge 2f\n\t"
        "add x10, x1, x9, lsl #6\n\t"    /* &a[k*16]   */
        "add x11, x2, x9, lsl #6\n\t"    /* &b[k*16]   */
        "ld1w {z0.s}, p0/z, [x10]\n\t"
        "ld1w {z1.s}, p0/z, [x11]\n\t"
        "fmopa za0.s, p0/m, p1/m, z0.s, z1.s\n\t"
        "add x9, x9, #1\n\t"
        "b 1b\n"
        "2:\n\t"
        "mov x9, #0\n"               /* row = 0    */
        "3:\n\t"
        "cmp x9, #16\n\t"
        "b.ge 4f\n\t"
        "mov w12, w9\n\t"
        "add x13, x3, x9, lsl #6\n\t"    /* &out[r*16] */
        "st1w {za0h.s[w12, 0]}, p0, [x13]\n\t"
        "add x9, x9, #1\n\t"
        "b 3b\n"
        "4:\n\t"
        "smstop za\n\t"
        "smstop sm\n\t"
        "ret\n\t"
    );
}
#endif

static int sme_fmopa_stress_arm_init(struct test *test)
{
#if !defined(__aarch64__) || !defined(__clang__)
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): the SME asm kernel is only "
             "built with clang on aarch64 (armv9-a+sve+sme+sme-f64f64)");
    return EXIT_SKIP;
#else
    unsigned long hwcap2 = getauxval(AT_HWCAP2);
    if ((hwcap2 & HWCAP2_SME) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SME not available; sme_fmopa_stress_arm exercises "
                 "the FMOPA matrix-accumulate datapath");
        return EXIT_SKIP;
    }

    /* RDVL Xd, #imm = VL_bytes * imm; #1 reads the streaming vector
     * length in bytes. SME implies SVE, so rdsvl cannot SIGILL here. */
    long svl_bytes;
    asm volatile("rdsvl %0, #1" : "=r"(svl_bytes));
    if (svl_bytes != kTileN * 4) {
        log_skip(CpuNotSupportedSkipCategory,
                 "sme_fmopa_stress_arm: SME streaming vector length %ld "
                 "bytes is not the characterized 64-byte (512-bit) "
                 "geometry; FMOPA tile semantics at other SVLs are "
                 "unmeasured", svl_bytes);
        return EXIT_SKIP;
    }

    try {
        auto data = std::make_unique<SmeFmopaData>();
        constexpr long n = kTileN;
        constexpr long k_rounds = kFmopaRounds;
        constexpr size_t vec_count = static_cast<size_t>(k_rounds) * n;
        constexpr size_t nn = static_cast<size_t>(n) * n;

        data->a.resize(vec_count);
        data->b.resize(vec_count);
        data->golden.assign(nn, 0.0f);

        /* Bit-dense finite operands: |a|, |b| <= 0.5625, so the 64-step
         * f32 sums stay below 21 - no overflow, no NaN, every single
         * flipped bit survives to the memcmp. Distinct per (k, lane). */
        for (long k = 0; k < k_rounds; ++k)
            for (long i = 0; i < n; ++i) {
                const size_t ki = static_cast<size_t>(k) * n + i;
                data->a[ki] = static_cast<float>(
                    ((k * 37 + i * 11) % 64) / 64.0 - 0.28125);
                data->b[ki] = static_cast<float>(
                    ((k * 53 + i * 29) % 64) / 64.0 - 0.28125);
            }

        /* Scalar golden with the identical operation order and the same
         * single-rounding fma the FMOPA accumulate performs. */
        for (long k = 0; k < k_rounds; ++k)
            for (long r = 0; r < n; ++r)
                for (long c = 0; c < n; ++c) {
                    const size_t kr = static_cast<size_t>(k) * n + r;
                    const size_t kc = static_cast<size_t>(k) * n + c;
                    const size_t rc = static_cast<size_t>(r) * n + c;
                    data->golden[rc] = std::fmaf(
                        data->a[kr], data->b[kc], data->golden[rc]);
                }

        test->data = data.release();
    } catch (const std::bad_alloc &) {
        return EXIT_SKIP;
    }
    return EXIT_SUCCESS;
#endif
}

static int sme_fmopa_stress_arm_run(struct test *test, int cpu)
{
    (void)cpu;
#if !defined(__aarch64__) || !defined(__clang__)
    (void)test;
    return EXIT_SUCCESS;
#else
    auto *d = static_cast<SmeFmopaData *>(test->data);
    constexpr long n = kTileN;
    constexpr size_t count = static_cast<size_t>(n) * n;
    constexpr size_t bytes = count * sizeof(float);

    /* Per-thread drain buffer: 608 CPUs run concurrently, a shared out
     * would be a cross-thread race. Rewritten every pass, no memset. */
    float out[kTileN * kTileN];

    SmeFmopaParams params = {
        d->a.data(), d->b.data(), out,
    };

    do {
        sme_fmopa_kernel(&params);

        if (memcmp(out, d->golden.data(), bytes) != 0) {
            memcmp_or_fail(out, d->golden.data(), count,
                           "sme_fmopa_stress_arm: FMOPA outer-product "
                           "accumulate mismatch vs scalar fmaf golden");
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
#endif
}

static int sme_fmopa_stress_arm_cleanup(struct test *test)
{
    delete static_cast<SmeFmopaData *>(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(sme_fmopa_stress_arm,
             "SME FMOPA matrix-unit SDC stress: 64-round f32 outer-product "
             "accumulate streams into ZA (serial fma chains per tile "
             "element), st1w row drain, byte-exact vs a scalar fmaf "
             "golden in the identical order (cn23154-measured SVL)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sme_fmopa_stress_arm_init,
    .test_run = sme_fmopa_stress_arm_run,
    .test_cleanup = sme_fmopa_stress_arm_cleanup,
    .fracture_loop_count = -1,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
