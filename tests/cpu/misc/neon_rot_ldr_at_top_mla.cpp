/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_ldr_at_top_mla
 * @parblock
 * Algebraic-dimension variant of neon_rot_ldr_at_top (2026-09-08).
 *
 * The parent's add/eor/and/orr are all simple 2-input ops. This variant
 * rotates through the multiply-accumulate family (i%4: mul / mla / mlsl /
 * eor-as-control) to test whether the SDC trigger tolerates the different
 * ALU latency/throughput structure of the Kunpeng 920 SIMD MAC pipe, and
 * whether corruption signatures change under accumulation chains:
 *   case 0: mulq then reinterpret         (pure multiply, long latency)
 *   case 1: mla: acc = vmlaq(a, a, b)   (single mla v.4s — accumulate
 *                                      chain pinned against refolding)
 *   case 2: mls-style: acc = sub(mul)
 *   case 3: veor (parent control op, unchanged semantics)
 * 32-bit lanes (the mul/mla/mls family is 32-bit-lane-only on this ISA;
 * historically spelled s32, now u32 — see below); sources
 * are masked to 16-bit magnitudes (products can still reach the sign
 * bit — bit-exactness comes from uint32_t mod-2^32 arithmetic), golden
 * precomputed in init via scalar independent recomputation (issue #373).
 * Run-side builtins are u32-typed (bit-identical mul/add/sub v.4s
 * instructions, well-defined wrapping) so the deliberate sign-bit-setting
 * products stay UBSan-clean — the s32-typed spelling is signed-overflow
 * UB under GCC's elementwise builtin lowering (issue #390).
 * Same skeleton otherwise: 3rd ldr at top + 2-src loads + rotating ALU +
 * back-to-back str/str; memcmp in cold path.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#define NEON_ROT_LDR_AT_TOP_MLA_COUNT 1024

struct NeonRotLdrAtTopMlaData {
    uint32x4_t *srcA;
    uint32x4_t *srcB;
    uint32x4_t *scratch;
    uint32x4_t *expected;
};

#if defined(__aarch64__)

static inline void store_vec(void *addr, uint32x4_t val) {
    vst1q_u32(static_cast<uint32_t *>(addr), val);
}

static inline uint32x4_t load_vec(const void *addr) {
    uint32x4_t res;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

/* keep every lane a 16-bit magnitude (0x0000ffff mask) so products stay
 * small — note they CAN still set the sign bit (0xffff*0xffff =
 * 0xFFFE0001); the golden below is bit-exact via uint32_t mod-2^32
 * arithmetic, not via no-overflow. Narrowing lives in the scalar golden
 * loop (issue #373) — masked values are bit-identical either way. */
#endif

static int neon_rot_ldr_at_top_mla_init(struct test *test) {
    auto *data = static_cast<NeonRotLdrAtTopMlaData *>(malloc(sizeof(NeonRotLdrAtTopMlaData)));
    if (!data) {
        log_skip(TestResourceIssueSkipCategory,
                 "neon_rot_ldr_at_top_mla: data alloc failed");
        return EXIT_SKIP;
    }
    data->srcA = static_cast<uint32x4_t *>(aligned_alloc_safe(64, NEON_ROT_LDR_AT_TOP_MLA_COUNT * sizeof(uint32x4_t)));
    data->srcB = static_cast<uint32x4_t *>(aligned_alloc_safe(64, NEON_ROT_LDR_AT_TOP_MLA_COUNT * sizeof(uint32x4_t)));
    data->scratch = static_cast<uint32x4_t *>(aligned_alloc_safe(64, NEON_ROT_LDR_AT_TOP_MLA_COUNT * sizeof(uint32x4_t)));
    data->expected = static_cast<uint32x4_t *>(aligned_alloc_safe(64, NEON_ROT_LDR_AT_TOP_MLA_COUNT * sizeof(uint32x4_t)));
    if (!data->srcA || !data->srcB || !data->scratch || !data->expected) {
        free(data->srcA);
        free(data->srcB);
        free(data->scratch);
        free(data->expected);
        free(data);
        log_skip(TestResourceIssueSkipCategory,
                 "neon_rot_ldr_at_top_mla: buffer alloc failed");
        return EXIT_SKIP;
    }

    memset_random(data->srcA, NEON_ROT_LDR_AT_TOP_MLA_COUNT * sizeof(uint32x4_t));
    memset_random(data->srcB, NEON_ROT_LDR_AT_TOP_MLA_COUNT * sizeof(uint32x4_t));
    memset_random(data->scratch, NEON_ROT_LDR_AT_TOP_MLA_COUNT * sizeof(uint32x4_t));

    /* golden (issue #373): scalar independent recomputation — narrowing +
     * mul/add/sub/eor in pure C integer ops on the scalar pipeline, never the
     * NEON units under test. uint32_t arithmetic is bit-exact with the
     * u32 vector ops because both reduce mod 2^32 — NOT because products stay in
     * s32 range (0xffff*0xffff = 0xFFFE0001 does set the sign bit; ~600
     * lanes per run overflow). Keep uint32_t here: an int32_t rewrite of
     * a*b+a would be signed-overflow UB.
     * A persistent NEON datapath fault can no longer corrupt golden and
     * actual identically; it now surfaces as a byte difference at the memcmp. */
    {
        uint32_t *sa = reinterpret_cast<uint32_t *>(data->srcA);
        uint32_t *sb = reinterpret_cast<uint32_t *>(data->srcB);
        uint32_t *ex = reinterpret_cast<uint32_t *>(data->expected);
        for (int e = 0; e < 4 * NEON_ROT_LDR_AT_TOP_MLA_COUNT; e++) {
            const uint32_t a = sa[e] & 0x0000ffffu;   /* narrow_for_mul, scalar */
            const uint32_t b = sb[e] & 0x0000ffffu;
            sa[e] = a;   /* persist the narrowed sources so run-side
                          * recomputation sees identical inputs */
            sb[e] = b;
            switch ((e / 4) % 4) {   /* op phase: vector index % 4 */
                case 0: ex[e] = a * b;     break;
                case 1: ex[e] = a * b + a; break;
                case 2: ex[e] = a * b - b; break;
                case 3: ex[e] = a ^ b;     break;
            }
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int neon_rot_ldr_at_top_mla_run(struct test *test, int cpu) {
    auto *data = static_cast<NeonRotLdrAtTopMlaData *>(test->data);

    uint32x4_t *temp = static_cast<uint32x4_t *>(aligned_alloc_safe(64, NEON_ROT_LDR_AT_TOP_MLA_COUNT * sizeof(uint32x4_t)));
    uint32x4_t *dst  = static_cast<uint32x4_t *>(aligned_alloc_safe(64, NEON_ROT_LDR_AT_TOP_MLA_COUNT * sizeof(uint32x4_t)));
    if (!temp || !dst) {
        free(temp);
        free(dst);
        log_skip(TestResourceIssueSkipCategory,
                 "neon_rot_ldr_at_top_mla: run buffer alloc failed");
        return EXIT_SKIP;
    }

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < NEON_ROT_LDR_AT_TOP_MLA_COUNT; i++) {
            uint32x4_t unused_x = load_vec(&data->scratch[i]);
            (void)unused_x;

            uint32x4_t a = load_vec(&data->srcA[i]);
            uint32x4_t b = load_vec(&data->srcB[i]);
            uint32x4_t res;

            /* u32-typed builtins: bit-identical to the s32 spelling (two's
             * complement mul/add/sub — same mul/mla/sub v.4s instructions),
             * but wrapping is well-defined. GCC lowers these vector
             * builtins elementwise, so under -fsanitize=undefined the s32
             * spelling turns every sign-bit-setting product (~600
             * lanes/run, the deliberate 16-bit operand domain of issue
             * #373) into scalar signed-overflow UB that halts the nightly
             * sanitizer CI (issue #390: '60669 * 52334 cannot be
             * represented in type int'). The uint32_t scalar golden
             * (issue #373) matches the u32 vector ops mod 2^32 exactly.
             * case 2's empty asm forces the product to materialize so GCC
             * cannot refold a*b-b into (a-1)*b — a rewrite GCC 12 applies
             * only to the u32 spelling: without the barrier the sub, this
             * case's whole "mls-style" identity, vanishes from the -O2/-O3
             * binary (verified: with it, the per-case mul/mla/mul+sub/eor
             * sequence matches the s32 original).
             * case 1 spells vmlaq_u32 directly instead of
             * vaddq_u32(vmulq_u32(a,b),a). The bare chain's mla is a
             * compiler fusion choice, not a guarantee: GCC 12.3 fuses
             * it to the same single mla at -O2/-O3 and under UBSan
             * (verified — the u32 chain carries no overflow checks to
             * inhibit fusion), but nothing pins that, and the refold
             * class is real on this compiler — it rewrites unbarriered
             * case 2's a*b-b into (a-1)*b, sub gone. a*b+a -> a*(b+1)
             * would likewise be bit-identical mod 2^32 (golden green,
             * chain gone), though not observed for the add form. vmlaq
             * is one intrinsic with no expression surface to refold: a
             * single mla v.4s in every build config (PR #393 r2/r4). */
            switch (i % 4) {
                case 0: res = vmulq_u32(a, b); break;
                case 1: res = vmlaq_u32(a, a, b); break;
                case 2: {
                    uint32x4_t p = vmulq_u32(a, b);
                    __asm__ volatile ("" : "+w" (p));
                    res = vsubq_u32(p, b);
                } break;
                case 3: res = veorq_u32(a, b); break;
            }

            store_vec(&temp[i], res);
            store_vec(&dst[i], res);
        }

        if (memcmp(dst, data->expected, NEON_ROT_LDR_AT_TOP_MLA_COUNT * sizeof(uint32x4_t)) != 0) {
            report_fail_msg("neon_rot_ldr_at_top_mla data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int neon_rot_ldr_at_top_mla_cleanup(struct test *test) {
    auto *data = static_cast<NeonRotLdrAtTopMlaData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_ldr_at_top_mla, "Algebraic variant of ldr_at_top: rotation mul/mla-style/mls-style/eor in 32-bit lanes, u32-typed (MAC pipe + accumulate chain signature test)")
    .test_init    = neon_rot_ldr_at_top_mla_init,
    .test_run     = neon_rot_ldr_at_top_mla_run,
    .test_cleanup = neon_rot_ldr_at_top_mla_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
