/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * SVE/SVE2 engine for adaptive_mem_sdc (2026-11-13, Task 3).
 * Spec: docs/ADAPTIVE_MEM_SDC_DESIGN_20261113.md §3/§5.
 *
 * Compiled in its own translation unit with -march=armv9-a+sve2 (probe
 * fallback armv8.2-a+sve2) so it builds on non-SVE hosts; at RUNTIME it
 * is selected only when HWCAP advertises SVE/SVE2 (Kunpeng 920B/950).
 * SVE and SVE2 share this code — the ACLE sv* forms use the full
 * runtime vector length; SVE2-specific rotation slots (sqdmulh etc.)
 * are a future extension.
 *
 * ⚠️ Runtime validation on SVE hardware is a FOLLOW-UP campaign (spec
 * §8): this host (Kunpeng 920, ARMv8.1-A) cannot execute these paths.
 * The compile gate + objdump gate prove encoding-level correctness
 * only.
 *
 * SVE idiom note: sizeless types forbid pointer arithmetic, so buffers
 * are PLAIN element arrays; vector i of a u64 buffer lives at
 * &arr[i * svcntd()].  The DCE guard for the empty-load slots is an
 * empty volatile asm with "w" operand + "memory" clobber (same
 * anti-DCE/anti-reorder semantics as the NEON family's asm-volatile
 * ldr, without betting the never-executed ld1d asm operand syntax).
 */

#include "sandstone.h"
#include "adaptive_mem_sdc.h"

#include <arm_sve.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

#if defined(__aarch64__)

/* Per-call context (mirror of the NEON engine's, VL-scaled byte sizes) */
struct SveCtx {
    struct AdaptiveShm *shm;
    int combo;
    int isa;                             /* 1=SVE 2=SVE2 (report tag) */
    int test_mode;                       /* survey mode: per-child exit, not global flag */
    uint64_t sweep;
    void *srcA, *srcB, *srcC, *scratch, *expected;
    void *temp, *dst, *junk1, *junk2;
};

/* vector geometry, computed once per run_combo.
 * (svbool_t is sizeless and cannot be a C++ struct member — predicates
 * are created at function entry instead.) */
struct SveGeom {
    uint64_t lanes64;                    /* svcntd(): u64 lanes per vector */
    uint64_t lanes32;                    /* svcntw(): u32 lanes per vector */
    size_t vl_bytes;                     /* svcntb() */
};

/* DCE-guarded vector load: intrinsic + empty volatile asm ("w" operand,
 * "memory" clobber) — the load cannot be eliminated nor reordered. */
static inline svuint64_t svld_guard64(svbool_t pg, const uint64_t *addr)
{
    svuint64_t v = svld1_u64(pg, addr);
    __asm__ volatile ("" : : "w"(v) : "memory");
    return v;
}
static inline svuint32_t svld_guard32(svbool_t pg, const uint32_t *addr)
{
    svuint32_t v = svld1_u32(pg, addr);
    __asm__ volatile ("" : : "w"(v) : "memory");
    return v;
}

static inline void svst64(svbool_t pg, uint64_t *addr, svuint64_t v)
{
    svst1_u64(pg, addr, v);
}
static inline void svst32(svbool_t pg, uint32_t *addr, svuint32_t v)
{
    svst1_u32(pg, addr, v);
}

/* ---- slot ops (i%4), _x (unpredicated) forms, formulas per spec §3 ---- */

static inline svuint64_t fp64_rot(svbool_t pg, int slot, svuint64_t a, svuint64_t b)
{
    const svfloat64_t fa = svreinterpret_f64_u64(a);
    const svfloat64_t fb = svreinterpret_f64_u64(b);
    svfloat64_t fr;
    switch (slot) {
        case 0: fr = svadd_f64_x(pg, fa, fb); break;
        case 1: fr = svsub_f64_x(pg, fa, fb); break;
        case 2: fr = svmul_f64_x(pg, fa, fb); break;
        default: fr = svmax_f64_x(pg, fa, fb); break;
    }
    return svreinterpret_u64_f64(fr);
}

static inline svuint64_t int_rot(svbool_t pg, int slot, svuint64_t a, svuint64_t b)
{
    switch (slot) {
        case 0: return svadd_u64_x(pg, a, b);
        case 1: return sveor_u64_x(pg, a, b);
        case 2: return svand_u64_x(pg, a, b);
        default: return svorr_u64_x(pg, a, b);
    }
}

static inline svuint32_t mla_rot(svbool_t pg, int slot, svuint32_t a, svuint32_t b)
{
    const svint32_t sa = svreinterpret_s32_u32(a);
    const svint32_t sb = svreinterpret_s32_u32(b);
    switch (slot) {
        case 0: return svreinterpret_u32_s32(svmul_s32_x(pg, sa, sb));
        case 1: return svreinterpret_u32_s32(svadd_s32_x(pg, svmul_s32_x(pg, sa, sb), sa));
        case 2: return svreinterpret_u32_s32(svsub_s32_x(pg, svmul_s32_x(pg, sa, sb), sb));
        default: return sveor_u32_x(pg, a, b);
    }
}

static inline svuint64_t sub_rot(svbool_t pg, int slot, svuint64_t a, svuint64_t b)
{
    switch (slot) {
        case 0: return svsub_u64_x(pg, a, b);
        case 1: return sveor_u64_x(pg, a, b);
        case 2: return svbic_u64_x(pg, a, b);
        default: return svorr_u64_x(pg, a, svnot_u64_x(pg, b));   /* ORN per spec */
    }
}

/* ---- 3-source slot ops (pinned formulas, no (a×b)+c form anywhere) ---- */

static inline svuint64_t fp64_rot3(svbool_t pg, int slot, svuint64_t a, svuint64_t b, svuint64_t c)
{
    const svfloat64_t fa = svreinterpret_f64_u64(a);
    const svfloat64_t fb = svreinterpret_f64_u64(b);
    const svfloat64_t fc = svreinterpret_f64_u64(c);
    svfloat64_t fr;
    switch (slot) {
        case 0: fr = svadd_f64_x(pg, svadd_f64_x(pg, fa, fb), fc); break;
        case 1: fr = svsub_f64_x(pg, svsub_f64_x(pg, fa, fb), fc); break;
        case 2: fr = svmul_f64_x(pg, svmul_f64_x(pg, fa, fb), fc); break;
        default: fr = svmax_f64_x(pg, svmax_f64_x(pg, fa, fb), fc); break;
    }
    return svreinterpret_u64_f64(fr);
}

static inline svuint64_t int_rot3(svbool_t pg, int slot, svuint64_t a, svuint64_t b, svuint64_t c)
{
    switch (slot) {
        case 0: return sveor_u64_x(pg, sveor_u64_x(pg, a, b), c);
        case 1: return sveor_u64_x(pg, svadd_u64_x(pg, a, b), c);
        case 2: return sveor_u64_x(pg, a, svadd_u64_x(pg, b, c));
        default: return svadd_u64_x(pg, sveor_u64_x(pg, a, b), c);
    }
}

static inline svuint32_t mla_rot3(svbool_t pg, int slot, svuint32_t a, svuint32_t b, svuint32_t c)
{
    const svint32_t sa = svreinterpret_s32_u32(a);
    const svint32_t sb = svreinterpret_s32_u32(b);
    const svint32_t sc = svreinterpret_s32_u32(c);
    const svint32_t t = svmul_s32_x(pg, sa, sb);
    switch (slot) {
        case 0: return svreinterpret_u32_s32(t);
        case 1: return svreinterpret_u32_s32(svadd_s32_x(pg, t, sc));
        case 2: return svreinterpret_u32_s32(svsub_s32_x(pg, sc, t));
        default: return sveor_u32_x(pg, sveor_u32_x(pg, a, b), c);
    }
}

static inline svuint64_t sub_rot3(svbool_t pg, int slot, svuint64_t a, svuint64_t b, svuint64_t c)
{
    switch (slot) {
        case 0: return svsub_u64_x(pg, svsub_u64_x(pg, a, b), c);
        case 1: return sveor_u64_x(pg, sveor_u64_x(pg, a, b), c);
        case 2: return svbic_u64_x(pg, a, svorr_u64_x(pg, b, c));                   /* a & ~(b|c) */
        default: return svorr_u64_x(pg, a, svnot_u64_x(pg, svand_u64_x(pg, b, c))); /* a | ~(b&c) */
    }
}

/* ---- hot sweep: one full sweep (ADAPT_COUNT vectors), noinline ----
 * u64-path: vector i at &buf[i * lanes64]; u32-path: &buf[i * lanes32] */
template <AdaptLoad LOAD, int LDR, int STR, AdaptSweep SWEEP>
static bool __attribute__((noinline)) sve_hot_sweep(struct SveCtx *ctx, const struct SveGeom *g)
{
    constexpr bool is_u32 = (LOAD == LOAD_MLA);
    const size_t bytes = ADAPT_COUNT * g->vl_bytes;
    bool clean = true;

    if constexpr (is_u32) {
        auto *srcA = static_cast<uint32_t *>(ctx->srcA);
        auto *srcB = static_cast<uint32_t *>(ctx->srcB);
        auto *srcC = static_cast<uint32_t *>(ctx->srcC);
        auto *scratch = static_cast<uint32_t *>(ctx->scratch);
        auto *temp = static_cast<uint32_t *>(ctx->temp);
        auto *dst = static_cast<uint32_t *>(ctx->dst);
        auto *junk1 = static_cast<uint32_t *>(ctx->junk1);
        auto *junk2 = static_cast<uint32_t *>(ctx->junk2);
        const svbool_t pg = svptrue_b32();
        const uint64_t lanes = g->lanes32;

        const bool descending = (SWEEP == SWEEP_ALT) && ((ctx->sweep & 1) != 0);
        for (int n = 0; n < ADAPT_COUNT; n++) {
            const int i = descending ? (ADAPT_COUNT - 1 - n) : n;
            const uint64_t off = i * lanes;
            svuint32_t unused_x = svld_guard32(pg, scratch + off);
            (void)unused_x;
            svuint32_t a = svld_guard32(pg, srcA + off);
            svuint32_t b = svld_guard32(pg, srcB + off);
            svuint32_t res;
            if constexpr (LOAD == LOAD_EMPTY) {
                if constexpr (LDR == 3) { svuint32_t c = svld_guard32(pg, srcC + off); (void)c; }
                res = a;
            } else if constexpr (LDR == 3) {
                svuint32_t c = svld_guard32(pg, srcC + off);
                res = mla_rot3(pg, i % 4, a, b, c);
            } else {
                res = mla_rot(pg, i % 4, a, b);
            }
            svst32(pg, temp + off, res);
            if constexpr (STR >= 3) svst32(pg, junk1 + off, res);
            if constexpr (STR >= 4) svst32(pg, junk2 + off, res);
            svst32(pg, dst + off, res);
        }
    } else {
        auto *srcA = static_cast<uint64_t *>(ctx->srcA);
        auto *srcB = static_cast<uint64_t *>(ctx->srcB);
        auto *srcC = static_cast<uint64_t *>(ctx->srcC);
        auto *scratch = static_cast<uint64_t *>(ctx->scratch);
        auto *temp = static_cast<uint64_t *>(ctx->temp);
        auto *dst = static_cast<uint64_t *>(ctx->dst);
        auto *junk1 = static_cast<uint64_t *>(ctx->junk1);
        auto *junk2 = static_cast<uint64_t *>(ctx->junk2);
        const svbool_t pg = svptrue_b64();
        const uint64_t lanes = g->lanes64;

        const bool descending = (SWEEP == SWEEP_ALT) && ((ctx->sweep & 1) != 0);
        for (int n = 0; n < ADAPT_COUNT; n++) {
            const int i = descending ? (ADAPT_COUNT - 1 - n) : n;
            const uint64_t off = i * lanes;
            svuint64_t unused_x = svld_guard64(pg, scratch + off);
            (void)unused_x;
            svuint64_t a = svld_guard64(pg, srcA + off);
            svuint64_t b = svld_guard64(pg, srcB + off);
            svuint64_t res;
            if constexpr (LOAD == LOAD_EMPTY) {
                if constexpr (LDR == 3) { svuint64_t c = svld_guard64(pg, srcC + off); (void)c; }
                res = a;
            } else if constexpr (LDR == 3) {
                svuint64_t c = svld_guard64(pg, srcC + off);
                if constexpr (LOAD == LOAD_FP64ROT)     res = fp64_rot3(pg, i % 4, a, b, c);
                else if constexpr (LOAD == LOAD_INTROT)  res = int_rot3(pg, i % 4, a, b, c);
                else                                     res = sub_rot3(pg, i % 4, a, b, c);
            } else {
                if constexpr (LOAD == LOAD_FP64ROT)     res = fp64_rot(pg, i % 4, a, b);
                else if constexpr (LOAD == LOAD_INTROT)  res = int_rot(pg, i % 4, a, b);
                else                                     res = sub_rot(pg, i % 4, a, b);
            }
            svst64(pg, temp + off, res);
            if constexpr (STR >= 3) svst64(pg, junk1 + off, res);
            if constexpr (STR >= 4) svst64(pg, junk2 + off, res);
            svst64(pg, dst + off, res);
        }
    }
    ctx->sweep++;

    auto *expected = static_cast<uint8_t *>(ctx->expected);
    auto *dstb = static_cast<uint8_t *>(ctx->dst);
    if (memcmp(dstb, expected, bytes) != 0)
        clean = false;

    if (!clean) {
        const struct ComboDef &cd = kComboTable[ctx->combo];
        /* set the cross-child flag BEFORE the noreturn report */
        __atomic_store_n(&ctx->shm->sdc_flag, 1u, __ATOMIC_RELEASE);
        /* vl in the tag: on-machine trio diagnostics key field */
        report_fail_msg("adaptive_mem_sdc combo#%d [%s/vl%dB/%s/ldr%d/str%d/%s] data miscompare",
                        ctx->combo, ctx->isa == 2 ? "SVE2" : "SVE",
                        (int)g->vl_bytes,
                        cd.dt == DT_FP64 ? "fp64" : "int",
                        cd.ldr, cd.str, cd.label);
    }

    /* Exit condition.  Combat: the global sdc_flag stops this child once
     * ANY child has detected (later children never reach the engine —
     * pick_combo idles them — so this mostly caps THIS child's reports).
     * Test mode: the flag is ignored (the survey must keep detecting
     * across children); exit only after THIS child's own detection —
     * the global-flag check here would neuter every post-detection
     * child to a single silent sweep on SVE machines. */
    if (ctx->test_mode)
        return clean;
    return __atomic_load_n(&ctx->shm->sdc_flag, __ATOMIC_ACQUIRE) == 0u;
}

using SveSweepFn = bool (*)(struct SveCtx *, const struct SveGeom *);

static constexpr AdaptSweep sve_sweep_for(AdaptLoad load)
{
    return (load == LOAD_MLA || load == LOAD_SUB) ? SWEEP_MONO : SWEEP_ALT;
}

#define ADAPT_SVE_ROW(load)                                                   \
    { { &sve_hot_sweep<load, 2, 2, sve_sweep_for(load)>,                      \
        &sve_hot_sweep<load, 2, 3, sve_sweep_for(load)>,                      \
        &sve_hot_sweep<load, 2, 4, sve_sweep_for(load)> },                    \
      { &sve_hot_sweep<load, 3, 2, sve_sweep_for(load)>,                      \
        &sve_hot_sweep<load, 3, 3, sve_sweep_for(load)>,                      \
        &sve_hot_sweep<load, 3, 4, sve_sweep_for(load)> } }

static const SveSweepFn sve_by_lts[5][2][3] = {
    ADAPT_SVE_ROW(LOAD_FP64ROT),
    ADAPT_SVE_ROW(LOAD_INTROT),
    ADAPT_SVE_ROW(LOAD_EMPTY),
    ADAPT_SVE_ROW(LOAD_MLA),
    ADAPT_SVE_ROW(LOAD_SUB),
};
#undef ADAPT_SVE_ROW

/* ---- cold path: fill + golden (same intrinsics as the hot loop) ---- */

static void __attribute__((noinline)) sve_fill_golden(struct SveCtx *ctx, const struct SveGeom *g)
{
    const struct ComboDef &cd = kComboTable[ctx->combo];
    const size_t bytes = ADAPT_COUNT * g->vl_bytes;

    memset_random(ctx->srcA, bytes);
    memset_random(ctx->srcB, bytes);
    memset_random(ctx->srcC, bytes);
    memset_random(ctx->scratch, bytes);

    if (cd.load == LOAD_MLA) {
        /* narrow sources to 16-bit magnitude and persist (parent-mla pattern) */
        auto *A = static_cast<uint32_t *>(ctx->srcA);
        auto *B = static_cast<uint32_t *>(ctx->srcB);
        auto *C = static_cast<uint32_t *>(ctx->srcC);
        for (size_t k = 0; k < bytes / 4; k++) {
            A[k] &= 0x0000ffffu;
            B[k] &= 0x0000ffffu;
            C[k] &= 0x0000ffffu;
        }
    } else if (cd.load == LOAD_FP64ROT) {
        /* NaN-free sanitize (see NEON engine / spec appendix v2.1):
         * dual inlined copies of commutative FP ops pick different
         * operand orders; both-sNaN lanes would diverge. */
        for (int b = 0; b < 3; b++) {
            auto *buf = b == 0 ? static_cast<uint64_t *>(ctx->srcA)
                      : b == 1 ? static_cast<uint64_t *>(ctx->srcB)
                               : static_cast<uint64_t *>(ctx->srcC);
            for (size_t k = 0; k < bytes / 8; k++)
                if ((buf[k] >> 52 & 0x7ff) == 0x7ff)
                    buf[k] &= ~(1ULL << 62);
        }
    }

    if (cd.load == LOAD_MLA) {
        auto *A = static_cast<uint32_t *>(ctx->srcA);
        auto *B = static_cast<uint32_t *>(ctx->srcB);
        auto *C = static_cast<uint32_t *>(ctx->srcC);
        auto *E = static_cast<uint32_t *>(ctx->expected);
        const svbool_t pg = svptrue_b32();
        const uint64_t lanes = g->lanes32;
        for (int i = 0; i < ADAPT_COUNT; i++) {
            const uint64_t off = i * lanes;
            svuint32_t a = svld_guard32(pg, A + off);
            svuint32_t b = svld_guard32(pg, B + off);
            svuint32_t e;
            if (cd.ldr == 3) e = mla_rot3(pg, i % 4, a, b, svld_guard32(pg, C + off));
            else             e = mla_rot(pg, i % 4, a, b);
            svst32(pg, E + off, e);
        }
    } else {
        auto *A = static_cast<uint64_t *>(ctx->srcA);
        auto *B = static_cast<uint64_t *>(ctx->srcB);
        auto *C = static_cast<uint64_t *>(ctx->srcC);
        auto *E = static_cast<uint64_t *>(ctx->expected);
        const svbool_t pg = svptrue_b64();
        const uint64_t lanes = g->lanes64;
        for (int i = 0; i < ADAPT_COUNT; i++) {
            const uint64_t off = i * lanes;
            svuint64_t a = svld_guard64(pg, A + off);
            svuint64_t b = svld_guard64(pg, B + off);
            svuint64_t e;
            switch (cd.load) {
                case LOAD_FP64ROT:
                    e = (cd.ldr == 3) ? fp64_rot3(pg, i % 4, a, b, svld_guard64(pg, C + off))
                                      : fp64_rot(pg, i % 4, a, b);
                    break;
                case LOAD_INTROT:
                    e = (cd.ldr == 3) ? int_rot3(pg, i % 4, a, b, svld_guard64(pg, C + off))
                                      : int_rot(pg, i % 4, a, b);
                    break;
                case LOAD_EMPTY:
                    e = a;
                    break;
                case LOAD_XFMOV:
                    /* unreachable in the SVE engine: XFMOV combos are
                     * routed to the NEON engine unconditionally (the
                     * cross-file FMOV chain is NEON-encoded by design;
                     * NEON executes natively on SVE hosts) */
                    e = a;
                    break;
                default:
                    e = (cd.ldr == 3) ? sub_rot3(pg, i % 4, a, b, svld_guard64(pg, C + off))
                                      : sub_rot(pg, i % 4, a, b);
                    break;
            }
            svst64(pg, E + off, e);
        }
    }
}

extern "C" int adaptive_sve_run_combo(struct test *test, int cpu, int combo)
{
    (void)cpu;
    if (combo < 0 || combo >= ADAPT_COMBO_COUNT)
        return EXIT_SUCCESS;

    auto *mdata = static_cast<struct AdaptiveMemSdcData *>(test->data);
    const struct ComboDef &cd = kComboTable[combo];

    struct SveCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.shm = mdata ? mdata->shm : nullptr;
    ctx.combo = combo;
    ctx.isa = mdata ? mdata->isa : 1;
    ctx.test_mode = mdata ? mdata->test_mode : 0;

    struct SveGeom g;
    g.lanes64 = svcntd();
    g.lanes32 = svcntw();
    g.vl_bytes = svcntb();

    const size_t bytes = ADAPT_COUNT * g.vl_bytes;
    ctx.srcA     = aligned_alloc(64, bytes);
    ctx.srcB     = aligned_alloc(64, bytes);
    ctx.srcC     = aligned_alloc(64, bytes);
    ctx.scratch  = aligned_alloc(64, bytes);
    ctx.expected = aligned_alloc(64, bytes);
    ctx.temp     = aligned_alloc(64, bytes);
    ctx.dst      = aligned_alloc(64, bytes);
    ctx.junk1    = aligned_alloc(64, bytes);
    ctx.junk2    = aligned_alloc(64, bytes);

    sve_fill_golden(&ctx, &g);

    SveSweepFn fn = sve_by_lts[cd.load][cd.ldr - 2][cd.str - 2];

    TEST_LOOP(test, 1 << 8) {
        if (!fn(&ctx, &g))
            break;
    }

    free(ctx.junk2);
    free(ctx.junk1);
    free(ctx.dst);
    free(ctx.temp);
    free(ctx.expected);
    free(ctx.scratch);
    free(ctx.srcC);
    free(ctx.srcB);
    free(ctx.srcA);
    return EXIT_SUCCESS;
}

#else  /* !__aarch64__ */

extern "C" int adaptive_sve_run_combo(struct test *test, int cpu, int combo)
{
    (void)test; (void)cpu; (void)combo;
    return EXIT_SUCCESS;
}

#endif
