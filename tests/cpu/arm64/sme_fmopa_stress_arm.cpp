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
 *    below 21 - no Inf/NaN; the byte-exact memcmp catches every
 *    difference that survives the fmaf rounding chain to the final tile
 *    (a flip below half a ULP of the accumulator can be rounded away);
 *  - all-core concurrency: 608 threads each in their own streaming
 *    window (per-thread ZA, no sharing).
 *
 * Gates: the framework layer's arm64_require_sme_svl (HWCAP2_SME
 * capability + 128..1024-bit SVL range in one call, all-VL Phase 2
 * P6), then the characterized-geometry guard (n = SVL/4 f32 lanes per
 * slice within [4, kTileNMax]; the geometry was characterized at
 * 512-bit on cn23154 and the parameterized asm verified at all four
 * SVLs in gem5). Non-aarch64 hosts, non-clang compilers, and
 * out-of-range SVLs skip with an honest reason; test_run re-checks the
 * SVL and fails closed on drift. x86-64 untouched.
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

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#ifdef __aarch64__
#include <device/cpu/arm64/arm64_vl.h>
#endif

/* Full-SVL support (128..1024-bit): n = SVL/4 f32 lanes per slice is
 * read at runtime (rdsvl); the ZA0.s tile is n x n. Originally
 * characterized at 512-bit on cn23154 (16x16); the parameterized asm has
 * been verified at SVL 128/256/512/1024 in gem5 (gem5-fi feat/sve-sme-all-vl,
 * kernel sme_fmopa_dual, 16-run all-green incl. the 512-bit regression
 * anchor).
 *
 * gem5-fi no-injection control, real run output (random seed=1, one run
 * per SVL tier, gem5.opt configs/se/arm_chaos.py --cpu O3 --sme_vl N):
 *   svl=1 (128-bit)  SDCSHIELD_FAILS=0  checksum 226bb7c9de343799
 *   svl=2 (256-bit)  SDCSHIELD_FAILS=0  checksum f9159f9ee5b58081
 *   svl=4 (512-bit)  SDCSHIELD_FAILS=0  checksum 892e4ba85fa50c39
 *                    (bit-identical to the pre-parameterization 512-bit
 *                    kernel — regression anchor)
 *   svl=8 (1024-bit) SDCSHIELD_FAILS=0  checksum 2856f882eb57bc99
 * Honest boundary: gem5 validates the same-source dual kernel's asm
 * semantics and numerical contract per tier; the host lacks SME, so the
 * C++ test itself runs only its build and skip paths here. gem5 FMOPA
 * is fplib mul+add double-rounded (F4 deviation, -ffp-contract=off in the
 * dual kernel build), which the gem5-fi findings register; real-SME
 * hardware runs remain future hardware validation. */
static constexpr long kTileNMax = 32;   /* SVL=1024bit upper bound */
/* Outer-product rounds per TEST_LOOP iteration (serial chain depth). */
static constexpr long kFmopaRounds = 64;

struct SmeFmopaData {
    /* f32 lanes per slice at the runtime SVL (4..32). */
    long n;
    /* Shared read-only operand streams, layout [k * n + lane]. */
    std::vector<float> a, b;
    /* Shared byte-exact golden tile, layout [row * n + col]. */
    std::vector<float> golden;
};

#if defined(__aarch64__) && defined(__clang__)
struct SmeFmopaParams {
    const float *a;
    const float *b;
    float *out;     /* n*n f32, rewritten every pass */
    long n;         /* f32 lanes per slice (runtime SVL) */
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
        "ldr x15, [x0, #24]\n\t"     /* n lanes    */
        "smstart sm\n\t"
        "smstart za\n\t"
        "zero {za}\n\t"
        "ptrue p0.s\n\t"
        "ptrue p1.s\n\t"
        "mov x9, #0\n"               /* k = 0      */
        "1:\n\t"
        "cmp x9, #64\n\t"
        "b.ge 2f\n\t"
        "mul x14, x9, x15\n\t"           /* k*n        */
        "add x10, x1, x14, lsl #2\n\t"   /* &a[k*n]    */
        "add x11, x2, x14, lsl #2\n\t"   /* &b[k*n]    */
        "ld1w {z0.s}, p0/z, [x10]\n\t"
        "ld1w {z1.s}, p0/z, [x11]\n\t"
        "fmopa za0.s, p0/m, p1/m, z0.s, z1.s\n\t"
        "add x9, x9, #1\n\t"
        "b 1b\n"
        "2:\n\t"
        "mov x9, #0\n"               /* row = 0    */
        "3:\n\t"
        "cmp x9, x15\n\t"
        "b.ge 4f\n\t"
        "mov w12, w9\n\t"
        "mul x14, x9, x15\n\t"
        "add x13, x3, x14, lsl #2\n\t"   /* &out[r*n]  */
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
    /* All-VL Phase 2 P6: HWCAP2_SME capability + 128..1024-bit SVL range
     * in one framework-layer call, replacing the getauxval hand-probe and
     * the inline rdsvl (the helper reads rdsvl #1 internally — SME
     * implies SVE, so it cannot SIGILL here). Returns the SVL in BYTES;
     * the n = SVL/4 geometry and the [4, kTileNMax] stack-buffer guard
     * below are the gem5-verified parameterized geometry and stay
     * untouched. */
    const long svl_bytes = arm64_require_sme_svl(test, 128, 1024);
    if (svl_bytes < 0)
        return EXIT_SKIP;
    const long n = svl_bytes / 4;
    if (n < 4 || n > kTileNMax) {
        log_skip(CpuNotSupportedSkipCategory,
                 "sme_fmopa_stress_arm: SME streaming vector length %ld "
                 "bytes is outside the supported 128..1024-bit range",
                 svl_bytes);
        return EXIT_SKIP;
    }

    try {
        auto data = std::make_unique<SmeFmopaData>();
        const long k_rounds = kFmopaRounds;
        const size_t vec_count = static_cast<size_t>(k_rounds) * n;
        const size_t nn = static_cast<size_t>(n) * n;

        data->n = n;
        data->a.resize(vec_count);
        data->b.resize(vec_count);
        data->golden.assign(nn, 0.0f);

        /* Bit-dense finite operands drawn from the framework RNG:
         * |a|, |b| < 0.5625, so the 64-step f32 sums stay below 21 - no
         * overflow, no NaN. The byte-exact memcmp detects any
         * difference that survives to the final tile; an input-bit flip
         * whose product differs by less than half a ULP of the
         * accumulator can be rounded away by a later fmaf round (PR
         * #237 review, coderabbit). The RNG is re-seeded per fracture
         * fragment, so every fragment stresses fresh operand
         * combinations instead of replaying one fixed formula (PR #234
         * review, greptile 4254737399). */
        for (long k = 0; k < k_rounds; ++k)
            for (long i = 0; i < n; ++i) {
                const size_t ki = static_cast<size_t>(k) * n + i;
                data->a[ki] = frandomf_scale(1.125f) - 0.5625f;
                data->b[ki] = frandomf_scale(1.125f) - 0.5625f;
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
    const long n = d->n;
    /* init and run are separate functions: the stack drain buffer below
     * is sized kTileNMax*kTileNMax and the kernel writes n*n into it, so
     * pin init's invariant (n within [4, kTileNMax]) before any buffer
     * math (PR #242 review, coderabbit). */
    assert(n >= 4 && n <= kTileNMax);
    /* All-VL policy #3: fail closed on init/run SVL drift — the shared
     * golden is sized at init's n, and the assert above only pins the
     * stored invariant (NDEBUG-sensitive); this check is
     * release-effective and compares a fresh rdsvl read. */
    if (arm64_sme_svl_bytes() / 4 != n) {
        log_error("sme_fmopa_stress_arm: SVL drifted between init (n=%ld) "
                  "and run (n=%ld)", n, arm64_sme_svl_bytes() / 4);
        report_fail_msg("sme_fmopa_stress_arm: SME SVL drifted between "
                        "init and run");
    }
    const size_t count = static_cast<size_t>(n) * n;
    const size_t bytes = count * sizeof(float);

    /* Per-thread drain buffer: 608 CPUs run concurrently, a shared out
     * would be a cross-thread race. Rewritten every pass, no memset. */
    float out[kTileNMax * kTileNMax];

    SmeFmopaParams params = {
        d->a.data(), d->b.data(), out, n,
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
    .fracture_loop_count = 0,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
