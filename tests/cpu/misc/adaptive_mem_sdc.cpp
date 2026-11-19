/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b adaptive_mem_sdc
 * @parblock
 * Adaptive memory-access SDC test (2026-11-13).
 * Spec: docs/ADAPTIVE_MEM_SDC_DESIGN_20261113.md (v2, approved);
 * plan: docs/superpowers/plans/2026-11-13-adaptive-mem-sdc.md.
 *
 * Cross-machine axiom (user-provided): the machine's widest SIMD ISA is
 * the most SDC-prone — NEON(128b) on Kunpeng 920, SVE(256b) on 920B,
 * SVE2 on 950.  This test probes HWCAP once per child, then every
 * short-lived fork_each_test child picks one combo from the 30-entry
 * rotation sequence (kPriorityOrder: measured-TTF order, fastest #12
 * first — 2026-11-14 locked sweep) and runs it for its whole lifetime:
 * normal mode by wall-clock slice (30min per combo, anchor in shm),
 * verify mode by a round-robin counter (each child takes the next combo
 * so a short -T run covers all 30 goldens).
 *
 * On SDC the failing child tags its report_fail_msg with the combo
 * identity and sets a shm flag that makes later children idle; the
 * campaign script SIGKILLs the main process on the first fail block
 * (the framework itself never exits on fail).
 *
 * Env contract (campaign script): ADAPTIVE_MEM_SDC_KEY (shm name
 * suffix; fresh key = fresh anchor), ADAPTIVE_MEM_SDC_VERIFY=1 (verify
 * mode), ADAPTIVE_MEM_SDC_COVFILE (per-child combo trace, one line per
 * init: "<iso> <pid> <isa> <mode> <combo>"), ADAPTIVE_MEM_SDC_COMBO
 * (lock every child to this combo regardless of slice — the
 * confirmation-arm hook for combos beyond #0), ADAPTIVE_MEM_SDC_ORDER
 * (full permutation of 0..35, rotation slot -> combo ID; default =
 * measured-TTF priority order), ADAPTIVE_MEM_SDC_ISA (auto|neon|sve|sve2
 * research override; forcing an ISA wider than HWCAP advertises is
 * refused with a stderr warning), ADAPTIVE_MEM_SDC_MODE (combat|test;
 * combat = default production behavior: priority rotation, first SDC
 * freezes later children to idle; test = survey: rotate through EVERY
 * combo in rotation order, one ADAPTIVE_MEM_SDC_TEST_SLICE window each,
 * detections reported but NOT frozen past — which combos detect SDC is
 * read off the fail-block combo tags at harvest; after the last slot
 * children idle = survey complete), ADAPTIVE_MEM_SDC_TEST_SLICE (test
 * -mode slice seconds, default 600 — the 2026-11-14 locked-sweep
 * window, results directly comparable).
 * @endparblock
 */

#include "sandstone.h"
#include "adaptive_mem_sdc.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <type_traits>
#include <fcntl.h>
#include <unistd.h>

#if defined(__aarch64__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#include <sys/mman.h>

/* HWCAP2 fallback, for build environments whose uapi asm/hwcap.h predates
 * the feature (openEuler 20.03-LTS ships 4.19-era headers with no HWCAP2_*
 * definitions at all — SVE2 landed in uapi 5.10). Value per uapi
 * asm/hwcap.h; same fallback pattern as framework arm64_vl.h. The fallback
 * carries the exact uapi bit, so probe_isa() behaves identically whether
 * the macro comes from the header or here (CI 20.03 containers run on a
 * modern runner kernel where AT_HWCAP2 can legitimately carry the bit).
 * HWCAP_SVE (AT_HWCAP family, uapi 4.15) is present in 4.19-era headers
 * and needs no fallback. */
#ifndef HWCAP2_SVE2
#define HWCAP2_SVE2 (1UL << 1)
#endif
#endif

/* ------------------------------------------------------------------ */
/* Combo priority table (spec §2 rows #0-29; #30-35 = 2026-11-18       */
/* XFMOV extension, user directive — same int-segment grid)           */
/* ------------------------------------------------------------------ */

extern "C" const struct ComboDef kComboTable[ADAPT_COMBO_COUNT] = {
    /* #0-5  FP64 segment: ldr2 str2/3/4, ldr3 str2/3/4 — all FP64 rotation */
    { DT_FP64, 2, 2, LOAD_FP64ROT, "fp64rot" },
    { DT_FP64, 2, 3, LOAD_FP64ROT, "fp64rot" },
    { DT_FP64, 2, 4, LOAD_FP64ROT, "fp64rot" },
    { DT_FP64, 3, 2, LOAD_FP64ROT, "fp64rot" },
    { DT_FP64, 3, 3, LOAD_FP64ROT, "fp64rot" },
    { DT_FP64, 3, 4, LOAD_FP64ROT, "fp64rot" },
    /* #6-29 int segment: ldr 2 then 3, str 2/3/4, loads in priority order */
    { DT_INT, 2, 2, LOAD_INTROT, "introt" },
    { DT_INT, 2, 2, LOAD_EMPTY,  "empty"  },
    { DT_INT, 2, 2, LOAD_MLA,    "mla"    },
    { DT_INT, 2, 2, LOAD_SUB,    "sub"    },
    { DT_INT, 2, 3, LOAD_INTROT, "introt" },
    { DT_INT, 2, 3, LOAD_EMPTY,  "empty"  },
    { DT_INT, 2, 3, LOAD_MLA,    "mla"    },
    { DT_INT, 2, 3, LOAD_SUB,    "sub"    },
    { DT_INT, 2, 4, LOAD_INTROT, "introt" },
    { DT_INT, 2, 4, LOAD_EMPTY,  "empty"  },
    { DT_INT, 2, 4, LOAD_MLA,    "mla"    },
    { DT_INT, 2, 4, LOAD_SUB,    "sub"    },
    { DT_INT, 3, 2, LOAD_INTROT, "introt" },
    { DT_INT, 3, 2, LOAD_EMPTY,  "empty"  },
    { DT_INT, 3, 2, LOAD_MLA,    "mla"    },
    { DT_INT, 3, 2, LOAD_SUB,    "sub"    },
    { DT_INT, 3, 3, LOAD_INTROT, "introt" },
    { DT_INT, 3, 3, LOAD_EMPTY,  "empty"  },
    { DT_INT, 3, 3, LOAD_MLA,    "mla"    },
    { DT_INT, 3, 3, LOAD_SUB,    "sub"    },
    { DT_INT, 3, 4, LOAD_INTROT, "introt" },
    { DT_INT, 3, 4, LOAD_EMPTY,  "empty"  },
    { DT_INT, 3, 4, LOAD_MLA,    "mla"    },
    { DT_INT, 3, 4, LOAD_SUB,    "sub"    },
    /* #30-35 XFMOV extension (2026-11-18, user directive): the cross-file
     * FMOV consumer chain as a load-family choice, same int-segment grid.
     * Standalone validation pair: fmov_train_ldr_str / fmov_train_ldp_stp
     * (10min full-core arms, 0 fail on this host). */
    { DT_INT, 2, 2, LOAD_XFMOV, "xfmov" },
    { DT_INT, 2, 3, LOAD_XFMOV, "xfmov" },
    { DT_INT, 2, 4, LOAD_XFMOV, "xfmov" },
    { DT_INT, 3, 2, LOAD_XFMOV, "xfmov" },
    { DT_INT, 3, 3, LOAD_XFMOV, "xfmov" },
    { DT_INT, 3, 4, LOAD_XFMOV, "xfmov" },
};

/* ------------------------------------------------------------------ */
/* Rotation order: slice slot -> combo ID (combo# stays the stable      */
/* recipe-ID/index into kComboTable everywhere: tags, COMBO lock,      */
/* verify round-robin).  Measured first-detection TTF from the          */
/* 2026-11-14 30-combo locked sweep (10min arms, mask 96-189, 179      */
/* present): 15 active arms ascending by TTF, then the 15 zero arms    */
/* as the reserve layer in table order.                                */
/* Source: docs/20261114/adaptive_combo_sweep/RESULTS.md               */
/* ------------------------------------------------------------------ */
extern "C" const int kPriorityOrder[ADAPT_COMBO_COUNT] = {
    12, 0, 7, 13, 21, 9, 25, 8, 14, 1, 3, 6, 10, 18, 26,     /* active: TTF 6.45s .. 58.4s */
    2, 4, 5, 11, 15, 16, 17, 19, 20, 22, 23, 24, 27, 28, 29, /* reserve: 10min zero arms */
    30, 31, 32, 33, 34, 35,                                  /* XFMOV: uncalibrated (11-18, negative on this host) */
};

/* ------------------------------------------------------------------ */
/* Engine entry points.  The SVE/SVE2 body lives in                    */
/* adaptive_mem_sdc_sve.cpp (own TU, +sve2 march); the NEON body is    */
/* below.                                                              */
/* ------------------------------------------------------------------ */

#if defined(__aarch64__)

/* ------------------------------------------------------------------ */
/* NEON engine (Task 2): 30 hot-loop instances + fill + golden          */
/* ------------------------------------------------------------------ */

#include <arm_neon.h>

/* Per-call context.  Every test_run invocation owns its own buffers:
 * verify mode picks a fresh combo per call, so fill+golden+alloc are
 * per-call by design (cold path, noinline helpers below). */
struct NeonCtx {
    /* buffers FIRST at the parent chassis's exact field offsets
     * (srcA=+0, srcB=+8, scratch=+16, expected=+24): the per-element
     * pointer-reload stream then matches the parent encoding exactly;
     * destinations are cached locals like the parent's temp/dst */
    void *srcA, *srcB, *scratch, *expected, *srcC;
    void *temp, *dst, *junk1, *junk2;
    struct AdaptiveShm *shm;             /* plumbing, cold-path only */
    int combo;
};

/* DCE-guarded loads (family pattern: intrinsic loads are NOT volatile —
 * the empty-load slots must keep their unused b/c loads emitted). */
static inline uint64x2_t load_vec(const void *addr) {
    uint64x2_t res;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}
static inline uint32x4_t load_vec_u32(const void *addr) {
    uint32x4_t res;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}
static inline void store_vec(void *addr, uint64x2_t val) {
    vst1q_u64(static_cast<uint64_t *>(addr), val);
}
static inline void store_vec_u32(void *addr, uint32x4_t val) {
    vst1q_u32(static_cast<uint32_t *>(addr), val);
}

/* ---- 2-source slot ops (i%4) — exact family formulas ---- */

/* FP64 rotation: fadd/fsub/fmul/fmax, slot order mirrors the FP64 parent */
static inline uint64x2_t fp64_rot(int slot, uint64x2_t a, uint64x2_t b) {
    const float64x2_t fa = vreinterpretq_f64_u64(a);
    const float64x2_t fb = vreinterpretq_f64_u64(b);
    float64x2_t fres;
    switch (slot) {
        case 0: fres = vaddq_f64(fa, fb); break;
        case 1: fres = vsubq_f64(fa, fb); break;
        case 2: fres = vmulq_f64(fa, fb); break;
        default: fres = vmaxq_f64(fa, fb); break;
    }
    return vreinterpretq_u64_f64(fres);
}

/* int rotation: add/eor/and/orr — rowmajor parent chassis */
static inline uint64x2_t int_rot(int slot, uint64x2_t a, uint64x2_t b) {
    switch (slot) {
        case 0: return vaddq_u64(a, b);
        case 1: return veorq_u64(a, b);
        case 2: return vandq_u64(a, b);
        default: return vorrq_u64(a, b);
    }
}

/* mla rotation (s32, decomposed — never the fused 3-operand form):
 * mul / mla-style (t+a) / mls-style (b−t) / eor control */
static inline uint32x4_t mla_rot(int slot, uint32x4_t a, uint32x4_t b) {
    const int32x4_t sa = vreinterpretq_s32_u32(a);
    const int32x4_t sb = vreinterpretq_s32_u32(b);
    switch (slot) {
        case 0: return vreinterpretq_u32_s32(vmulq_s32(sa, sb));
        case 1: return vreinterpretq_u32_s32(vaddq_s32(vmulq_s32(sa, sb), sa));
        case 2: return vreinterpretq_u32_s32(vsubq_s32(vmulq_s32(sa, sb), sb));
        default: return veorq_u32(a, b);
    }
}

/* sub rotation: sub (borrow chain) / eor / bic / orn */
static inline uint64x2_t sub_rot(int slot, uint64x2_t a, uint64x2_t b) {
    switch (slot) {
        case 0: return vsubq_u64(a, b);
        case 1: return veorq_u64(a, b);
        case 2: return vbicq_u64(a, b);
        default: return vornq_u64(a, b);
    }
}

/* ---- 3-source slot ops (i%4) — pinned formulas ----
 * FP64: no (a×b)+c form anywhere — fma contraction would rewrite it */
static inline uint64x2_t fp64_rot3(int slot, uint64x2_t a, uint64x2_t b, uint64x2_t c) {
    const float64x2_t fa = vreinterpretq_f64_u64(a);
    const float64x2_t fb = vreinterpretq_f64_u64(b);
    const float64x2_t fc = vreinterpretq_f64_u64(c);
    float64x2_t fres;
    switch (slot) {
        case 0: fres = vaddq_f64(vaddq_f64(fa, fb), fc); break;
        case 1: fres = vsubq_f64(vsubq_f64(fa, fb), fc); break;
        case 2: fres = vmulq_f64(vmulq_f64(fa, fb), fc); break;
        default: fres = vmaxq_f64(vmaxq_f64(fa, fb), fc); break;
    }
    return vreinterpretq_u64_f64(fres);
}

/* int 3src: the historical mrn_nuke_3src_alu mixing pattern, verbatim */
static inline uint64x2_t int_rot3(int slot, uint64x2_t a, uint64x2_t b, uint64x2_t c) {
    switch (slot) {
        case 0: return veorq_u64(veorq_u64(a, b), c);
        case 1: return veorq_u64(vaddq_u64(a, b), c);
        case 2: return veorq_u64(a, vaddq_u64(b, c));
        default: return vaddq_u64(veorq_u64(a, b), c);
    }
}

/* mla 3src: c is the accumulator-side operand */
static inline uint32x4_t mla_rot3(int slot, uint32x4_t a, uint32x4_t b, uint32x4_t c) {
    const int32x4_t sa = vreinterpretq_s32_u32(a);
    const int32x4_t sb = vreinterpretq_s32_u32(b);
    const int32x4_t sc = vreinterpretq_s32_u32(c);
    const int32x4_t t = vmulq_s32(sa, sb);
    switch (slot) {
        case 0: return vreinterpretq_u32_s32(t);
        case 1: return vreinterpretq_u32_s32(vaddq_s32(t, sc));
        case 2: return vreinterpretq_u32_s32(vsubq_s32(sc, t));
        default: return veorq_u32(veorq_u32(a, b), c);
    }
}

/* sub 3src: borrow chain / eor / bic chain / orn chain */
static inline uint64x2_t sub_rot3(int slot, uint64x2_t a, uint64x2_t b, uint64x2_t c) {
    switch (slot) {
        case 0: return vsubq_u64(vsubq_u64(a, b), c);
        case 1: return veorq_u64(veorq_u64(a, b), c);
        case 2: return vbicq_u64(a, vorrq_u64(b, c));       /* a & ~(b|c) */
        default: return vornq_u64(a, vandq_u64(b, c));      /* a | ~(b&c) */
    }
}

/* ---- XFMOV 中间负载 (2026-11-18, 用户指令: 跨寄存器堆 FMOV 消费链) ----
 * 指令序列取自用户提供的 SIMDFPLoadStoreMemLoopCpyP08 反汇编; 独立验证对
 * fmov_train_ldr_str / fmov_train_ldp_stp 已全核 10min 零错。语义:
 *   a 双 lane 跨堆出 (fmov x,d 平面形 + fmov x,v.d[1] 索引形——索引形
 *   d[0] 不可编码, ARM 只允许高半索引) → GPR add rot → 跨堆回 →
 *   b <OP> (a+rot); ldr3 形再 c <OP> 一次。OP 槽轮转 add/eor(.16b)/sub
 *   (i%3)。显式寄存器绑定保 FMOV 精确编码 (G2 编码保真门以 objdump 验)。
 * golden = neon_fill_golden 的 LOAD_XFMOV case 标量位级复算。 */
static constexpr uint64_t XFMOV_ROT = 0x9E3779B97F4A7C15ULL;

static inline __attribute__((always_inline)) uint64x2_t xfmov_rot(int slot, uint64x2_t a, uint64x2_t b)
{
    register uint64x2_t va __asm__("v16") = a;
    register uint64x2_t vb __asm__("v17") = b;
    register uint64x2_t vr __asm__("v18");
    uint64_t r = XFMOV_ROT;
    if (slot == 0)
        __asm__ volatile(
            "fmov x9,  d16\n\t"
            "fmov x10, v16.d[1]\n\t"
            "add  x11, x9,  %[rot]\n\t"
            "add  x12, x10, %[rot]\n\t"
            "fmov d19,  x11\n\t"
            "fmov v19.d[1], x12\n\t"
            "add  v18.2d, v17.2d, v19.2d"
            : "=w" (vr)
            : "w" (va), "w" (vb), [rot] "r" (r)
            : "x9", "x10", "x11", "x12", "v19");
    else if (slot == 1)
        __asm__ volatile(
            "fmov x9,  d16\n\t"
            "fmov x10, v16.d[1]\n\t"
            "add  x11, x9,  %[rot]\n\t"
            "add  x12, x10, %[rot]\n\t"
            "fmov d19,  x11\n\t"
            "fmov v19.d[1], x12\n\t"
            "eor  v18.16b, v17.16b, v19.16b"
            : "=w" (vr)
            : "w" (va), "w" (vb), [rot] "r" (r)
            : "x9", "x10", "x11", "x12", "v19");
    else
        __asm__ volatile(
            "fmov x9,  d16\n\t"
            "fmov x10, v16.d[1]\n\t"
            "add  x11, x9,  %[rot]\n\t"
            "add  x12, x10, %[rot]\n\t"
            "fmov d19,  x11\n\t"
            "fmov v19.d[1], x12\n\t"
            "sub  v18.2d, v17.2d, v19.2d"
            : "=w" (vr)
            : "w" (va), "w" (vb), [rot] "r" (r)
            : "x9", "x10", "x11", "x12", "v19");
    return vr;
}

static inline __attribute__((always_inline)) uint64x2_t xfmov_rot3(int slot, uint64x2_t a, uint64x2_t b, uint64x2_t c)
{
    register uint64x2_t va __asm__("v16") = a;
    register uint64x2_t vb __asm__("v17") = b;
    register uint64x2_t vc __asm__("v20") = c;
    register uint64x2_t vr __asm__("v18");
    uint64_t r = XFMOV_ROT;
    if (slot == 0)
        __asm__ volatile(
            "fmov x9,  d16\n\t"
            "fmov x10, v16.d[1]\n\t"
            "add  x11, x9,  %[rot]\n\t"
            "add  x12, x10, %[rot]\n\t"
            "fmov d19,  x11\n\t"
            "fmov v19.d[1], x12\n\t"
            "add  v18.2d, v17.2d, v19.2d\n\t"
            "add  v18.2d, v20.2d, v18.2d"
            : "=w" (vr)
            : "w" (va), "w" (vb), "w" (vc), [rot] "r" (r)
            : "x9", "x10", "x11", "x12", "v19");
    else if (slot == 1)
        __asm__ volatile(
            "fmov x9,  d16\n\t"
            "fmov x10, v16.d[1]\n\t"
            "add  x11, x9,  %[rot]\n\t"
            "add  x12, x10, %[rot]\n\t"
            "fmov d19,  x11\n\t"
            "fmov v19.d[1], x12\n\t"
            "eor  v18.16b, v17.16b, v19.16b\n\t"
            "eor  v18.16b, v20.16b, v18.16b"
            : "=w" (vr)
            : "w" (va), "w" (vb), "w" (vc), [rot] "r" (r)
            : "x9", "x10", "x11", "x12", "v19");
    else
        __asm__ volatile(
            "fmov x9,  d16\n\t"
            "fmov x10, v16.d[1]\n\t"
            "add  x11, x9,  %[rot]\n\t"
            "add  x12, x10, %[rot]\n\t"
            "fmov d19,  x11\n\t"
            "fmov v19.d[1], x12\n\t"
            "sub  v18.2d, v17.2d, v19.2d\n\t"
            "sub  v18.2d, v20.2d, v18.2d"
            : "=w" (vr)
            : "w" (va), "w" (vb), "w" (vc), [rot] "r" (r)
            : "x9", "x10", "x11", "x12", "v19");
    return vr;
}

/* =====================================================================
 * 模块库 (2026-11-14 用户架构: 按分类写功能块, 按优先级拼接)
 * 每个模块的文本 = 家族父版对应段落的逐字形式; 底盘宏拼接后, 每个组合
 * 的热循环文本 = 底盘文本 + 模块替换 → 编码保真由构造保证 (本家族物理:
 * "功能等价改写即灭活", 保真不能靠模板 if-constexpr 的运气).
 *   DECL 模块: M_D0/M_D1/M_D2 (junk 局部声明档位, 与 STR 档配套, 父版函数头形态)
 *   LDR  模块: M_LDR2 / M_LDR3 (scratch 顶置护栏由底盘持有)
 *   LOAD 模块: M_FP64ROT/M_INTROT/M_EMPTY/M_MLA/M_SUB × {2,3} 源形
 *   STR  模块: M_STR2 / M_STR3 / M_STR4 (temp→junk*→dst 背靠背, dst 最后)
 *   底盘扫序: ALT 型 (fp64/introt/empty 父版升降交替, 文本逐字对齐父版)
 *             / MONO 型 (mla/sub 父版单调扫, 无 sweep)
 * ===================================================================== */

static inline uint64x2_t *S64(void *p) { return static_cast<uint64x2_t *>(p); }
static inline uint32x4_t *S32(void *p) { return static_cast<uint32x4_t *>(p); }

/* ---- LDR 模块 (u64; 文本 = rowmajor 父版源装载段) ---- */
#define M_LDR2_U64()                                                      \
    uint64x2_t a = load_vec(&S64(ctx->srcA)[i]);                          \
    uint64x2_t b = load_vec(&S64(ctx->srcB)[i]);
#define M_LDR3_U64()                                                      \
    M_LDR2_U64()                                                          \
    uint64x2_t c = load_vec(&S64(ctx->srcC)[i]);
#define M_LDR2_U32()                                                      \
    uint32x4_t a = load_vec_u32(&S32(ctx->srcA)[i]);                      \
    uint32x4_t b = load_vec_u32(&S32(ctx->srcB)[i]);
#define M_LDR3_U32()                                                      \
    M_LDR2_U32()                                                          \
    uint32x4_t c = load_vec_u32(&S32(ctx->srcC)[i]);

/* ---- LOAD 模块 (u64 轮换段文本 = 各自父版; u32 仅 mla) ---- */
#define M_FP64ROT2()  uint64x2_t res = fp64_rot(i % 4, a, b);
#define M_FP64ROT3()  uint64x2_t res = fp64_rot3(i % 4, a, b, c);
#define M_INTROT2()   uint64x2_t res = int_rot(i % 4, a, b);
#define M_INTROT3()   uint64x2_t res = int_rot3(i % 4, a, b, c);
#define M_EMPTY2()    uint64x2_t res = a; (void)b;
#define M_EMPTY3()    uint64x2_t res = a; (void)b; (void)c;
#define M_SUB2()      uint64x2_t res = sub_rot(i % 4, a, b);
#define M_SUB3()      uint64x2_t res = sub_rot3(i % 4, a, b, c);
#define M_XFMOV2()    uint64x2_t res = xfmov_rot(i % 3, a, b);
#define M_XFMOV3()    uint64x2_t res = xfmov_rot3(i % 3, a, b, c);
#define M_MLA2()      uint32x4_t res = mla_rot(i % 4, a, b);
#define M_MLA3()      uint32x4_t res = mla_rot3(i % 4, a, b, c);

/* ---- STR 模块 (背靠背; 文本 = str4 父版追加模式) ---- */
#define M_STR2_U64()                                                       \
    store_vec(&temp[i], res);                                              \
    store_vec(&dst[i], res);
#define M_STR3_U64()                                                       \
    store_vec(&temp[i], res);                                              \
    store_vec(&junk1[i], res);                                             \
    store_vec(&dst[i], res);
#define M_STR4_U64()                                                       \
    store_vec(&temp[i], res);                                              \
    store_vec(&junk1[i], res);                                             \
    store_vec(&junk2[i], res);                                             \
    store_vec(&dst[i], res);
#define M_STR2_U32()                                                       \
    store_vec_u32(&temp[i], res);                                          \
    store_vec_u32(&dst[i], res);
#define M_STR3_U32()                                                       \
    store_vec_u32(&temp[i], res);                                          \
    store_vec_u32(&junk1[i], res);                                         \
    store_vec_u32(&dst[i], res);
#define M_STR4_U32()                                                       \
    store_vec_u32(&temp[i], res);                                          \
    store_vec_u32(&junk1[i], res);                                         \
    store_vec_u32(&junk2[i], res);                                         \
    store_vec_u32(&dst[i], res);

/* ---- DECL 模块 (junk 局部声明档位; 文本 = 各父版函数头的局部声明段,
 *      str2 父版只有 temp/dst 由底盘自带) ---- */
#define M_D0_U64()   /* str2: 无 junk 局部 */
#define M_D1_U64()   uint64x2_t *junk1 = S64(ctx->junk1);
#define M_D2_U64()   uint64x2_t *junk1 = S64(ctx->junk1);                  \
                     uint64x2_t *junk2 = S64(ctx->junk2);
#define M_D0_U32()   /* str2: 无 junk 局部 */
#define M_D1_U32()   uint32x4_t *junk1 = S32(ctx->junk1);
#define M_D2_U32()   uint32x4_t *junk1 = S32(ctx->junk1);                  \
                     uint32x4_t *junk2 = S32(ctx->junk2);

/* ---- 底盘宏: 父版热循环骨架 + 模块孔 (u64 两型 / u32 一型) ----
 * ALT 型 sweep/ascending/i 三行 = fp64 父版逐字 (编码保真);
 * MONO 型无 sweep (mla/sub 父版单调扫, 亦免 set-but-unused 警告). */
#define ADAPT_CHASSIS_ALT_U64(FN, DECLB, LDRB, LOADB, STRB)                \
static void __attribute__((noinline)) FN(struct test *test, struct NeonCtx *ctx) \
{                                                                          \
    uint64x2_t *temp  = S64(ctx->temp);                                    \
    uint64x2_t *dst   = S64(ctx->dst);                                     \
    DECLB()                                                                \
    uint64_t sweep = 0;                                                    \
    TEST_LOOP(test, 1 << 8) {                                              \
        const bool ascending = (sweep & 1) == 0;                           \
        for (int n = 0; n < ADAPT_COUNT; n++) {                            \
            const int i = ascending ? n : (ADAPT_COUNT - 1 - n);           \
            uint64x2_t unused_x = load_vec(&S64(ctx->scratch)[i]);         \
            (void)unused_x;                                                \
            LDRB()                                                          \
            LOADB()                                                         \
            STRB()                                                          \
        }                                                                  \
        sweep++;                                                           \
        if (memcmp(dst, ctx->expected, ADAPT_COUNT * 16) != 0) {           \
            const struct ComboDef &cd = kComboTable[ctx->combo];           \
            __atomic_store_n(&ctx->shm->sdc_flag, 1u, __ATOMIC_RELEASE);   \
            report_fail_msg("adaptive_mem_sdc combo#%d [NEON/%s/ldr%d/str%d/%s] data miscompare", \
                            ctx->combo, cd.dt == DT_FP64 ? "fp64" : "int",  \
                            cd.ldr, cd.str, cd.label);                      \
        }                                                                  \
    }                                                                      \
}

#define ADAPT_CHASSIS_MONO_U64(FN, DECLB, LDRB, LOADB, STRB)               \
static void __attribute__((noinline)) FN(struct test *test, struct NeonCtx *ctx) \
{                                                                          \
    uint64x2_t *temp  = S64(ctx->temp);                                    \
    uint64x2_t *dst   = S64(ctx->dst);                                     \
    DECLB()                                                                \
    TEST_LOOP(test, 1 << 8) {                                              \
        for (int n = 0; n < ADAPT_COUNT; n++) {                            \
            const int i = n;                                               \
            uint64x2_t unused_x = load_vec(&S64(ctx->scratch)[i]);         \
            (void)unused_x;                                                \
            LDRB()                                                          \
            LOADB()                                                         \
            STRB()                                                          \
        }                                                                  \
        if (memcmp(dst, ctx->expected, ADAPT_COUNT * 16) != 0) {           \
            const struct ComboDef &cd = kComboTable[ctx->combo];           \
            __atomic_store_n(&ctx->shm->sdc_flag, 1u, __ATOMIC_RELEASE);   \
            report_fail_msg("adaptive_mem_sdc combo#%d [NEON/%s/ldr%d/str%d/%s] data miscompare", \
                            ctx->combo, cd.dt == DT_FP64 ? "fp64" : "int",  \
                            cd.ldr, cd.str, cd.label);                      \
        }                                                                  \
    }                                                                      \
}

#define ADAPT_CHASSIS_MONO_U32(FN, DECLB, LDRB, LOADB, STRB)               \
static void __attribute__((noinline)) FN(struct test *test, struct NeonCtx *ctx) \
{                                                                          \
    uint32x4_t *temp  = S32(ctx->temp);                                    \
    uint32x4_t *dst   = S32(ctx->dst);                                     \
    DECLB()                                                                \
    TEST_LOOP(test, 1 << 8) {                                              \
        for (int n = 0; n < ADAPT_COUNT; n++) {                            \
            const int i = n;   /* mla 父版单调扫 */                         \
            uint32x4_t unused_x = load_vec_u32(&S32(ctx->scratch)[i]);     \
            (void)unused_x;                                                \
            LDRB()                                                          \
            LOADB()                                                         \
            STRB()                                                          \
        }                                                                  \
        if (memcmp(dst, ctx->expected, ADAPT_COUNT * 16) != 0) {           \
            const struct ComboDef &cd = kComboTable[ctx->combo];           \
            __atomic_store_n(&ctx->shm->sdc_flag, 1u, __ATOMIC_RELEASE);   \
            report_fail_msg("adaptive_mem_sdc combo#%d [NEON/%s/ldr%d/str%d/%s] data miscompare", \
                            ctx->combo, cd.dt == DT_FP64 ? "fp64" : "int",  \
                            cd.ldr, cd.str, cd.label);                      \
        }                                                                  \
    }                                                                      \
}

/* ---- 30 个组合装配 (优先级序 = kComboTable 序; 与表逐行并排核对;
 *      DECL 档与 STR 档配套, #3-5 修复: FP64 ldr3 = M_FP64ROT3 与 golden 对齐) ---- */
ADAPT_CHASSIS_ALT_U64 (nc_00, M_D0_U64, M_LDR2_U64, M_FP64ROT2, M_STR2_U64)  /* #0  fp64/ldr2/str2 */
ADAPT_CHASSIS_ALT_U64 (nc_01, M_D1_U64, M_LDR2_U64, M_FP64ROT2, M_STR3_U64)  /* #1  fp64/ldr2/str3 */
ADAPT_CHASSIS_ALT_U64 (nc_02, M_D2_U64, M_LDR2_U64, M_FP64ROT2, M_STR4_U64)  /* #2  fp64/ldr2/str4 */
ADAPT_CHASSIS_ALT_U64 (nc_03, M_D0_U64, M_LDR3_U64, M_FP64ROT3, M_STR2_U64)  /* #3  fp64/ldr3/str2 */
ADAPT_CHASSIS_ALT_U64 (nc_04, M_D1_U64, M_LDR3_U64, M_FP64ROT3, M_STR3_U64)  /* #4  fp64/ldr3/str3 */
ADAPT_CHASSIS_ALT_U64 (nc_05, M_D2_U64, M_LDR3_U64, M_FP64ROT3, M_STR4_U64)  /* #5  fp64/ldr3/str4 */
ADAPT_CHASSIS_ALT_U64 (nc_06, M_D0_U64, M_LDR2_U64, M_INTROT2,  M_STR2_U64)  /* #6  introt/ldr2/str2 */
ADAPT_CHASSIS_ALT_U64 (nc_07, M_D0_U64, M_LDR2_U64, M_EMPTY2,   M_STR2_U64)  /* #7  empty/ldr2/str2 */
ADAPT_CHASSIS_MONO_U32(nc_08, M_D0_U32, M_LDR2_U32, M_MLA2,     M_STR2_U32)  /* #8  mla/ldr2/str2 */
ADAPT_CHASSIS_MONO_U64(nc_09, M_D0_U64, M_LDR2_U64, M_SUB2,     M_STR2_U64)  /* #9  sub/ldr2/str2 */
ADAPT_CHASSIS_ALT_U64 (nc_10, M_D1_U64, M_LDR2_U64, M_INTROT2,  M_STR3_U64)  /* #10 introt/ldr2/str3 */
ADAPT_CHASSIS_ALT_U64 (nc_11, M_D1_U64, M_LDR2_U64, M_EMPTY2,   M_STR3_U64)  /* #11 empty/ldr2/str3 */
ADAPT_CHASSIS_MONO_U32(nc_12, M_D1_U32, M_LDR2_U32, M_MLA2,     M_STR3_U32)  /* #12 mla/ldr2/str3 */
ADAPT_CHASSIS_MONO_U64(nc_13, M_D1_U64, M_LDR2_U64, M_SUB2,     M_STR3_U64)  /* #13 sub/ldr2/str3 */
ADAPT_CHASSIS_ALT_U64 (nc_14, M_D2_U64, M_LDR2_U64, M_INTROT2,  M_STR4_U64)  /* #14 introt/ldr2/str4 */
ADAPT_CHASSIS_ALT_U64 (nc_15, M_D2_U64, M_LDR2_U64, M_EMPTY2,   M_STR4_U64)  /* #15 empty/ldr2/str4 */
ADAPT_CHASSIS_MONO_U32(nc_16, M_D2_U32, M_LDR2_U32, M_MLA2,     M_STR4_U32)  /* #16 mla/ldr2/str4 */
ADAPT_CHASSIS_MONO_U64(nc_17, M_D2_U64, M_LDR2_U64, M_SUB2,     M_STR4_U64)  /* #17 sub/ldr2/str4 */
ADAPT_CHASSIS_ALT_U64 (nc_18, M_D0_U64, M_LDR3_U64, M_INTROT3,  M_STR2_U64)  /* #18 introt/ldr3/str2 */
ADAPT_CHASSIS_ALT_U64 (nc_19, M_D0_U64, M_LDR3_U64, M_EMPTY3,   M_STR2_U64)  /* #19 empty/ldr3/str2 */
ADAPT_CHASSIS_MONO_U32(nc_20, M_D0_U32, M_LDR3_U32, M_MLA3,     M_STR2_U32)  /* #20 mla/ldr3/str2 */
ADAPT_CHASSIS_MONO_U64(nc_21, M_D0_U64, M_LDR3_U64, M_SUB3,     M_STR2_U64)  /* #21 sub/ldr3/str2 */
ADAPT_CHASSIS_ALT_U64 (nc_22, M_D1_U64, M_LDR3_U64, M_INTROT3,  M_STR3_U64)  /* #22 introt/ldr3/str3 */
ADAPT_CHASSIS_ALT_U64 (nc_23, M_D1_U64, M_LDR3_U64, M_EMPTY3,   M_STR3_U64)  /* #23 empty/ldr3/str3 */
ADAPT_CHASSIS_MONO_U32(nc_24, M_D1_U32, M_LDR3_U32, M_MLA3,     M_STR3_U32)  /* #24 mla/ldr3/str3 */
ADAPT_CHASSIS_MONO_U64(nc_25, M_D1_U64, M_LDR3_U64, M_SUB3,     M_STR3_U64)  /* #25 sub/ldr3/str3 */
ADAPT_CHASSIS_ALT_U64 (nc_26, M_D2_U64, M_LDR3_U64, M_INTROT3,  M_STR4_U64)  /* #26 introt/ldr3/str4 */
ADAPT_CHASSIS_ALT_U64 (nc_27, M_D2_U64, M_LDR3_U64, M_EMPTY3,   M_STR4_U64)  /* #27 empty/ldr3/str4 */
ADAPT_CHASSIS_MONO_U32(nc_28, M_D2_U32, M_LDR3_U32, M_MLA3,     M_STR4_U32)  /* #28 mla/ldr3/str4 */
ADAPT_CHASSIS_MONO_U64(nc_29, M_D2_U64, M_LDR3_U64, M_SUB3,     M_STR4_U64)  /* #29 sub/ldr3/str4 */
/* ---- XFMOV 扩展 6 组合 (2026-11-18): ALT 底盘 (introt 母型) + FMOV 跨堆链 ---- */
ADAPT_CHASSIS_ALT_U64 (nc_30, M_D0_U64, M_LDR2_U64, M_XFMOV2,   M_STR2_U64)  /* #30 xfmov/ldr2/str2 */
ADAPT_CHASSIS_ALT_U64 (nc_31, M_D1_U64, M_LDR2_U64, M_XFMOV2,   M_STR3_U64)  /* #31 xfmov/ldr2/str3 */
ADAPT_CHASSIS_ALT_U64 (nc_32, M_D2_U64, M_LDR2_U64, M_XFMOV2,   M_STR4_U64)  /* #32 xfmov/ldr2/str4 */
ADAPT_CHASSIS_ALT_U64 (nc_33, M_D0_U64, M_LDR3_U64, M_XFMOV3,   M_STR2_U64)  /* #33 xfmov/ldr3/str2 */
ADAPT_CHASSIS_ALT_U64 (nc_34, M_D1_U64, M_LDR3_U64, M_XFMOV3,   M_STR3_U64)  /* #34 xfmov/ldr3/str3 */
ADAPT_CHASSIS_ALT_U64 (nc_35, M_D2_U64, M_LDR3_U64, M_XFMOV3,   M_STR4_U64)  /* #35 xfmov/ldr3/str4 */

/* 分发表: 优先级序直排 (combo# = 下标), 与 kComboTable 逐行对应 */
using SweepFn = void (*)(struct test *, struct NeonCtx *);
static const SweepFn neon_combo_fns[ADAPT_COMBO_COUNT] = {
    &nc_00, &nc_01, &nc_02, &nc_03, &nc_04, &nc_05,
    &nc_06, &nc_07, &nc_08, &nc_09, &nc_10, &nc_11,
    &nc_12, &nc_13, &nc_14, &nc_15, &nc_16, &nc_17,
    &nc_18, &nc_19, &nc_20, &nc_21, &nc_22, &nc_23,
    &nc_24, &nc_25, &nc_26, &nc_27, &nc_28, &nc_29,
    &nc_30, &nc_31, &nc_32, &nc_33, &nc_34, &nc_35,
};

/* ---- cold path: fill + golden (same intrinsics as the hot loop) ---- */

static void __attribute__((noinline)) neon_fill_golden(struct NeonCtx *ctx)
{
    const struct ComboDef &cd = kComboTable[ctx->combo];
    const size_t bytes = ADAPT_COUNT * 16;

    memset_random(ctx->srcA, bytes);
    memset_random(ctx->srcB, bytes);
    memset_random(ctx->srcC, bytes);
    memset_random(ctx->scratch, bytes);

    if (cd.load == LOAD_FP64ROT) {
        /* NaN-free inputs.  GCC freely reorders operands of the
         * commutative FP ops (fadd/fmul/fmax), and for a BOTH-sNaN
         * lane AArch64 returns the quieted *first* operand — so two
         * inlined copies of the "same" expression (golden loop vs hot
         * loop) can disagree bit-exactly on sNaN-pair lanes.  Clearing
         * the all-ones exponent at fill time (NaN/Inf -> finite, bit 62
         * cleared) removes the payload ambiguity entirely; finite
         * arithmetic is bit-deterministic under operand reordering.
         * Integer loads are exact and need no sanitize.  (Root cause:
         * 2026-11-13 verify fail, deterministic repro at ~1e4 rounds,
         * 300M rounds clean post-fix.) */
        auto sanitize = [](void *buf) {
            auto *w = static_cast<uint64_t *>(buf);
            for (int k = 0; k < ADAPT_COUNT * 2; k++)
                if ((w[k] >> 52 & 0x7ff) == 0x7ff)
                    w[k] &= ~(1ULL << 62);
        };
        sanitize(ctx->srcA);
        sanitize(ctx->srcB);
        sanitize(ctx->srcC);
    }

    if (cd.load == LOAD_MLA) {
        /* parent-mla pattern: narrow sources to 16-bit magnitude and
         * PERSIST them so golden recomputation sees identical inputs */
        auto *A = static_cast<uint32x4_t *>(ctx->srcA);
        auto *B = static_cast<uint32x4_t *>(ctx->srcB);
        auto *C = static_cast<uint32x4_t *>(ctx->srcC);
        const uint32x4_t mask = vdupq_n_u32(0x0000ffff);
        for (int i = 0; i < ADAPT_COUNT; i++) {
            A[i] = vandq_u32(A[i], mask);
            B[i] = vandq_u32(B[i], mask);
            C[i] = vandq_u32(C[i], mask);
        }
    }

    if (cd.load == LOAD_XFMOV) {
        /* scalar bit-exact golden mirroring the asm chain:
         * r = op(b, a + XFMOV_ROT); ldr3: r = op(c, r);
         * op rotates add/eor/sub by element index (i % 3) */
        auto op = [](int s, uint64_t x, uint64_t y) {
            return s == 0 ? (x + y) : (s == 1 ? (x ^ y) : (x - y));
        };
        auto *aw = static_cast<const uint64_t *>(ctx->srcA);
        auto *bw = static_cast<const uint64_t *>(ctx->srcB);
        auto *cw = static_cast<const uint64_t *>(ctx->srcC);
        auto *ew = static_cast<uint64_t *>(ctx->expected);
        for (int i = 0; i < ADAPT_COUNT; ++i) {
            const int slot = i % 3;
            uint64_t r0 = op(slot, bw[2 * i],     aw[2 * i]     + XFMOV_ROT);
            uint64_t r1 = op(slot, bw[2 * i + 1], aw[2 * i + 1] + XFMOV_ROT);
            if (cd.ldr == 3) {
                r0 = op(slot, cw[2 * i],     r0);
                r1 = op(slot, cw[2 * i + 1], r1);
            }
            ew[2 * i] = r0;
            ew[2 * i + 1] = r1;
        }
    } else if (cd.load == LOAD_MLA) {
        auto *A = static_cast<uint32x4_t *>(ctx->srcA);
        auto *B = static_cast<uint32x4_t *>(ctx->srcB);
        auto *C = static_cast<uint32x4_t *>(ctx->srcC);
        auto *E = static_cast<uint32x4_t *>(ctx->expected);
        for (int i = 0; i < ADAPT_COUNT; i++) {
            if (cd.ldr == 3) E[i] = mla_rot3(i % 4, A[i], B[i], C[i]);
            else             E[i] = mla_rot(i % 4, A[i], B[i]);
        }
    } else {
        auto *A = static_cast<uint64x2_t *>(ctx->srcA);
        auto *B = static_cast<uint64x2_t *>(ctx->srcB);
        auto *C = static_cast<uint64x2_t *>(ctx->srcC);
        auto *E = static_cast<uint64x2_t *>(ctx->expected);
        for (int i = 0; i < ADAPT_COUNT; i++) {
            switch (cd.load) {
                case LOAD_FP64ROT:
                    E[i] = (cd.ldr == 3) ? fp64_rot3(i % 4, A[i], B[i], C[i])
                                         : fp64_rot(i % 4, A[i], B[i]);
                    break;
                case LOAD_INTROT:
                    E[i] = (cd.ldr == 3) ? int_rot3(i % 4, A[i], B[i], C[i])
                                         : int_rot(i % 4, A[i], B[i]);
                    break;
                case LOAD_EMPTY:
                    E[i] = A[i];         /* res = a regardless of ldr */
                    break;
                default: /* LOAD_SUB */
                    E[i] = (cd.ldr == 3) ? sub_rot3(i % 4, A[i], B[i], C[i])
                                         : sub_rot(i % 4, A[i], B[i]);
                    break;
            }
        }
    }
}

extern "C" int adaptive_neon_run_combo(struct test *test, int cpu, int combo)
{
    (void)cpu;
    if (combo < 0 || combo >= ADAPT_COMBO_COUNT)
        return EXIT_SUCCESS;

    auto *mdata = static_cast<struct AdaptiveMemSdcData *>(test->data);

    struct NeonCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.shm = mdata ? mdata->shm : nullptr;
    ctx.combo = combo;

    const size_t bytes = ADAPT_COUNT * 16;
    ctx.srcA     = aligned_alloc(64, bytes);
    ctx.srcB     = aligned_alloc(64, bytes);
    ctx.srcC     = aligned_alloc(64, bytes);
    ctx.scratch  = aligned_alloc(64, bytes);
    ctx.expected = aligned_alloc(64, bytes);
    ctx.temp     = aligned_alloc(64, bytes);
    ctx.dst      = aligned_alloc(64, bytes);
    ctx.junk1    = aligned_alloc(64, bytes);
    ctx.junk2    = aligned_alloc(64, bytes);

    neon_fill_golden(&ctx);

    /* 模块装配实例 (combo# = 优先级序), 实例自带 TEST_LOOP */
    neon_combo_fns[combo](test, &ctx);

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

extern "C" int adaptive_neon_run_combo(struct test *test, int cpu, int combo)
{
    (void)test; (void)cpu; (void)combo;
    return EXIT_SUCCESS;
}

#endif

#if defined(__aarch64__)

/* ------------------------------------------------------------------ */
/* Per-child plumbing: shm, ISA probe, combo pick, covfile trace       */
/* ------------------------------------------------------------------ */

static const char *isa_tag(int isa)
{
    switch (isa) {
        case 2:  return "SVE2";
        case 1:  return "SVE";
        default: return "NEON";
    }
}

/* Map (or create) the named shm.  O_EXCL decides the creator; the
 * creator publishes the anchor then sets ready (release).  Latecomers
 * spin on ready (acquire) with a bounded wait — on timeout the mapping
 * is returned anyway and pick_combo() falls back to idle. */
static struct AdaptiveShm *shm_setup(void)
{
    const char *key = getenv("ADAPTIVE_MEM_SDC_KEY");
    char name[128];
    if (key) {
        snprintf(name, sizeof(name), "/adaptive_mem_sdc_%s", key);
    } else {
        /* no key: derive from the main process pid (children fork from it,
         * so every child of one run shares the name; a later ad-hoc run
         * gets a fresh object instead of reusing a stale anchor/sdc_flag) */
        snprintf(name, sizeof(name), "/adaptive_mem_sdc_ppid%ld", (long)getppid());
    }

    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    bool creator = (fd >= 0);
    if (!creator) {
        if (errno != EEXIST)
            return nullptr;
        fd = shm_open(name, O_RDWR, 0600);
        if (fd < 0)
            return nullptr;
    }

    if (creator && ftruncate(fd, sizeof(struct AdaptiveShm)) != 0) {
        close(fd);
        return nullptr;
    }

    auto *shm = static_cast<struct AdaptiveShm *>(
            mmap(nullptr, sizeof(struct AdaptiveShm), PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd, 0));
    close(fd);
    if (shm == MAP_FAILED)
        return nullptr;

    if (creator) {
        /* zero the fresh object (ftruncate gives zeros, be explicit anyway) */
        memset(shm, 0, sizeof(*shm));
        clock_gettime(CLOCK_MONOTONIC, &shm->anchor);
        __atomic_store_n(&shm->ready, 1u, __ATOMIC_RELEASE);
    } else {
        /* bounded spin: creator publishes within microseconds of our
         * EEXIST; 2s covers any pathological scheduling */
        for (int i = 0; i < 200000; i++) {
            if (__atomic_load_n(&shm->ready, __ATOMIC_ACQUIRE) == 1u)
                break;
            usleep(10);
        }
    }
    return shm;
}

static int probe_isa(void)
{
    /* SVE2 lives in AT_HWCAP2, SVE/ASIMD in AT_HWCAP — do not mix them up */
    int probed;
    unsigned long hwc2 = getauxval(AT_HWCAP2);
    if (hwc2 & HWCAP2_SVE2)
        probed = 2;
    else if (getauxval(AT_HWCAP) & HWCAP_SVE)
        probed = 1;
    else
        probed = 0;                     /* ASIMD/NEON: base aarch64 */

    /* Research override: pin the ISA for same-machine A/B.  Forcing a
     * WIDER ISA than the silicon advertises is refused — executing SVE
     * code on a non-SVE core is guaranteed SIGILL, not a fallback. */
    const char *ov = getenv("ADAPTIVE_MEM_SDC_ISA");
    if (ov == nullptr || strcmp(ov, "auto") == 0)
        return probed;

    int want;
    if (strcmp(ov, "neon") == 0)        want = 0;
    else if (strcmp(ov, "sve") == 0)    want = 1;
    else if (strcmp(ov, "sve2") == 0)   want = 2;
    else {
        fprintf(stderr, "adaptive_mem_sdc: ADAPTIVE_MEM_SDC_ISA must be auto|neon|sve|sve2, ignoring '%s'\n", ov);
        return probed;
    }

    if (want > probed) {
        fprintf(stderr, "adaptive_mem_sdc: ADAPTIVE_MEM_SDC_ISA=%s requested but HWCAP does not advertise it; falling back to %s\n",
                ov, probed == 0 ? "neon" : probed == 1 ? "sve" : "sve2");
        return probed;
    }
    return want;                        /* narrower pin is always safe */
}

/* Rotation order actually used by this child: kPriorityOrder (measured-
 * TTF default) unless ADAPTIVE_MEM_SDC_ORDER carries a valid full
 * permutation of 0..35.  Parsed once per child init (cold path); any
 * malformed or non-permutation value: stderr warning + default order. */
static int g_rot_order[ADAPT_COMBO_COUNT];

static void order_setup(void)
{
    memcpy(g_rot_order, kPriorityOrder, sizeof(g_rot_order));

    const char *spec = getenv("ADAPTIVE_MEM_SDC_ORDER");
    if (spec == nullptr)
        return;

    int tmp[ADAPT_COMBO_COUNT];
    bool seen[ADAPT_COMBO_COUNT] = { false };
    int n = 0;
    const char *p = spec;
    while (*p != '\0') {
        char *end = nullptr;
        long v = strtol(p, &end, 10);
        if (end == p || v < 0 || v >= ADAPT_COMBO_COUNT || seen[v])
            goto bad;
        seen[v] = true;
        if (n < ADAPT_COMBO_COUNT)
            tmp[n++] = (int)v;
        if (*end == '\0')
            break;
        if (*end != ',')
            goto bad;
        p = end + 1;
    }
    if (n != ADAPT_COMBO_COUNT)
        goto bad;
    memcpy(g_rot_order, tmp, sizeof(g_rot_order));
    return;
bad:
    fprintf(stderr, "adaptive_mem_sdc: ADAPTIVE_MEM_SDC_ORDER must be a permutation of 0..%d, keeping default priority order (ignoring '%s')\n",
            ADAPT_COMBO_COUNT - 1, spec);
}

/* Seconds elapsed since the shm anchor, floored at 0 (cold helper). */
static int64_t seconds_since(const struct timespec *anchor, struct timespec *now)
{
    clock_gettime(CLOCK_MONOTONIC, now);
    int64_t s = (int64_t)(now->tv_sec - anchor->tv_sec);
    if (now->tv_nsec < anchor->tv_nsec)
        s -= 1;
    return s < 0 ? 0 : s;
}

/* Pick this child's combo.  Returns -1 when the child should idle
 * (sdc_flag set in combat mode, survey complete in test mode, or shm
 * unusable). */
static int pick_combo(struct AdaptiveShm *shm, int test_mode, int test_slice)
{
    if (!shm)
        return -1;
    /* combat mode only: first SDC freezes every later child to idle.
     * test mode ignores the flag — the survey must keep switching
     * combos after detections (per-combo verdicts come from the
     * fail-block combo tags at harvest time). */
    if (!test_mode &&
        __atomic_load_n(&shm->sdc_flag, __ATOMIC_ACQUIRE) != 0)
        return -1;
    /* creator never published the anchor (pathological 2s spin timeout):
     * idle — reading a zero anchor would compute a huge elapsed and jump
     * straight to the last combo instead of the documented idle fallback */
    if (__atomic_load_n(&shm->ready, __ATOMIC_ACQUIRE) != 1u)
        return -1;

    /* confirmation/campaign hook: lock every child to one combo (e.g.
     * ADAPTIVE_MEM_SDC_COMBO=1 arms the #2 priority recipe; fresh KEY
     * alone can only ever reach combo 0 in slice 0) */
    const char *force = getenv("ADAPTIVE_MEM_SDC_COMBO");
    if (force != nullptr) {
        int c = atoi(force);
        if (c >= 0 && c < ADAPT_COMBO_COUNT)
            return c;
    }

    if (getenv("ADAPTIVE_MEM_SDC_VERIFY") != nullptr) {
        uint32_t prev = __atomic_fetch_add(&shm->verify_ctr, 1u, __ATOMIC_RELAXED);
        return (int)(prev % ADAPT_COMBO_COUNT);
    }

    struct timespec now;
    if (test_mode) {
        /* survey: every combo gets exactly one test_slice window, in
         * rotation order; after the last slot the survey is complete
         * and later children idle */
        int64_t elapsed = seconds_since(&shm->anchor, &now);
        int64_t idx = elapsed / test_slice;
        if (idx >= ADAPT_COMBO_COUNT)
            return -1;
        return g_rot_order[idx];
    }

    int64_t elapsed = seconds_since(&shm->anchor, &now);
    int64_t idx = elapsed / ADAPT_SLICE_SECONDS;
    if (idx > ADAPT_COMBO_COUNT - 1)
        idx = ADAPT_COMBO_COUNT - 1;
    return g_rot_order[idx];            /* slot -> combo: TTF priority */
}

/* one line per child init: "<iso> <pid> <isa> <mode> <combo>" */
static void covfile_trace(int isa, int test_mode, int combo)
{
    const char *path = getenv("ADAPTIVE_MEM_SDC_COVFILE");
    if (!path)
        return;
    char line[160];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char iso[32];
    strftime(iso, sizeof(iso), "%FT%T", &tm);
    snprintf(line, sizeof(line), "%s %d %s %s %d\n", iso, (int)getpid(),
             isa_tag(isa),
             getenv("ADAPTIVE_MEM_SDC_VERIFY") ? "verify"
                 : test_mode ? "test" : "normal",
             combo);
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0)
        return;
    (void)!write(fd, line, strlen(line));
    close(fd);
}

static int adaptive_mem_sdc_init(struct test *test)
{
    auto *data = static_cast<struct AdaptiveMemSdcData *>(malloc(sizeof(struct AdaptiveMemSdcData)));
    data->shm = shm_setup();
    data->isa = probe_isa();
    data->test_mode = 0;
    data->test_slice = 600;             /* the 2026-11-14 locked-sweep window */
    const char *mode = getenv("ADAPTIVE_MEM_SDC_MODE");
    if (mode != nullptr) {
        if (strcmp(mode, "test") == 0)
            data->test_mode = 1;
        else if (strcmp(mode, "combat") != 0)
            fprintf(stderr, "adaptive_mem_sdc: ADAPTIVE_MEM_SDC_MODE must be combat|test, ignoring '%s'\n", mode);
    }
    const char *ts = getenv("ADAPTIVE_MEM_SDC_TEST_SLICE");
    if (ts != nullptr) {
        int v = atoi(ts);
        if (v >= 1)
            data->test_slice = v;
        else
            fprintf(stderr, "adaptive_mem_sdc: ADAPTIVE_MEM_SDC_TEST_SLICE must be >=1 second, keeping default 600\n");
    }
    order_setup();
    test->data = data;
    return EXIT_SUCCESS;
}

static int adaptive_mem_sdc_run(struct test *test, int cpu)
{
    auto *data = static_cast<struct AdaptiveMemSdcData *>(test->data);
    int combo = pick_combo(data->shm, data->test_mode, data->test_slice);
    covfile_trace(data->isa, data->test_mode, combo);

    if (combo < 0)
        return EXIT_SUCCESS;             /* post-SDC or shm unusable: idle */

    /* XFMOV combos always take the NEON engine: the cross-file FMOV
     * chain is NEON-encoded by design (fmov D-plane / V.D[1] forms)
     * and NEON executes natively on SVE hosts — real execution on
     * every ISA, no placeholder skip. */
    if (combo >= 0 && kComboTable[combo].load == LOAD_XFMOV)
        return adaptive_neon_run_combo(test, cpu, combo);

    switch (data->isa) {
        case 1:
        case 2:
            return adaptive_sve_run_combo(test, cpu, combo);
        default:
            return adaptive_neon_run_combo(test, cpu, combo);
    }
}

static int adaptive_mem_sdc_cleanup(struct test *test)
{
    auto *data = static_cast<struct AdaptiveMemSdcData *>(test->data);
    /* shm mapping dies with the child process; the named object is
     * removed by the campaign script at campaign end */
    free(data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

#else  /* !__aarch64__ */

static int adaptive_mem_sdc_init(struct test *test)  { (void)test; return EXIT_SUCCESS; }
static int adaptive_mem_sdc_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SUCCESS; }
static int adaptive_mem_sdc_cleanup(struct test *test) { (void)test; return EXIT_SUCCESS; }

#endif

DECLARE_TEST(adaptive_mem_sdc, "Adaptive memory-access SDC test: probes widest SIMD (SVE2>SVE>NEON, ADAPTIVE_MEM_SDC_ISA override), sweeps 36-combo rotation sequence (30 original + 6 XFMOV cross-file FMOV middle-load rows, 2026-11-18) (kPriorityOrder: measured-TTF order, fastest first, ADAPTIVE_MEM_SDC_ORDER override, 30min wall-clock slice per combo, verify-mode round-robin for golden smoke); MODE=combat (default: first SDC freezes the rotation) or MODE=test (survey: every combo runs one TEST_SLICE window, detections reported but not frozen, idle after the last slot); SDC report carries the combo tag")
    .test_init    = adaptive_mem_sdc_init,
    .test_run     = adaptive_mem_sdc_run,
    .test_cleanup = adaptive_mem_sdc_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
