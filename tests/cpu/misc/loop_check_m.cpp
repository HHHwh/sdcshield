/**
 * @copyright
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b loop_check_m
 * @parblock
 * Loop and Cumulative Checksum (LoopCheck family, group M). The only family
 * that runs a compute pass and an independent verify pass over the same
 * buffers, then repeats the pair crc_M_OUTER times. A transient fault during
 * the compute pass leaves a wrong value in memory that the verify pass sees,
 * and a transient fault during verify shows up as a mismatch. The golden
 * captured at init adds a second reference: it was computed from the pristine
 * sources, so a fault that corrupts src1 or src2 in memory is caught even
 * though recompute and the stored result would then agree with each other.
 * (A permanent datapath fault is not caught, since it is already baked into
 * the golden.)
 * @endparblock
 *
 * @brief Shared kernel for Loop and Cumulative Checksum ports.
 *
 * Loop and Cumulative Checksum is the "LoopCheck" family: the only family that runs a
 * compute pass and an independent verify pass over the same buffers, then
 * repeats the pair crc_M_OUTER times. Every case has this shape:
 *
 * fill src1 / src2 with random data
 * repeat 100 times:
 *   compute pass: for each pair { load src1, load src2, op, store dst }
 *   DSB SY
 *   verify pass:  for each pair { load src1, load src2, recompute op,
 *                                 load dst, compare }
 *
 * The recompute-and-compare is what gives this family its L3 precision: a
 * transient fault during the compute pass leaves a wrong value in memory
 * that the verify pass sees, and a transient fault during verify shows up
 * as a mismatch. The golden captured at init adds a second reference: it
 * was computed from the pristine sources, so a fault that corrupts src1 or
 * src2 in memory is caught even though recompute and the stored result
 * would then agree with each other. (A permanent datapath fault is not
 * caught, since it is already baked into the golden.)
 *
 * Knobs:
 * crc_M_OP       datapath op (crc_M_OP_*)
 * crc_M_ELEM     16 = scalar LDP/STP X pair (Base*), 32 = NEON LD2/STP Q
 * crc_M_UNROLL   pairs processed per loop iteration (1, 3 or 5)
 * crc_M_ITERS    inner loop iterations
 * crc_M_OUTER    outer repetitions (100 for every case)
 * crc_M_SHIFT    SHL immediate / FMUL lane index
 * crc_M_MULFIXED FMUL multiplier comes from a fixed vector (V0) instead of
 *                the second source
 * crc_M_CROSS    operand pairing of the P03/P04-style variants: the two
 *                ops read src1[0] and src2[0] and use a fixed vector as the
 *                second operand (the .S uses V0/V1 or V30/V31)
 * crc_M_FIXSAME  both fixed operands are the same vector (V0 both times)
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

/* ---- datapath ops ------------------------------------------------------ */
#define crc_M_OP_ADD_EXT  0   /* ADD Wd, Wn, Wm, UXTB #0 */
#define crc_M_OP_SUB_EXT  1   /* SUB Xd, Xn, Wm, UXTB #0 */
#define crc_M_OP_MADD     2   /* MADD Xd, Xn, Xm, Xa */
#define crc_M_OP_SDIV     3   /* SDIV Xd, Xn, Xm */
#define crc_M_OP_ABS      4   /* ABS Vd.2D, Vn.2D */
#define crc_M_OP_FABS     5   /* FABS Vd.2D, Vn.2D */
#define crc_M_OP_FDIV     6   /* FDIV Vd.2D, Vn.2D, Vm.2D */
#define crc_M_OP_FMUL     7   /* FMUL Vd.2D, Vn.2D, Vm.D[lane] */
#define crc_M_OP_MLA      8   /* MLA Vd.16B, Vn.16B, Vm.16B (acc preloaded) */
#define crc_M_OP_ORR      9   /* ORR Vd.16B, Vn.16B, Vm.16B */
#define crc_M_OP_PMULL    10  /* PMULL Vd.1Q, Vn.1D, Vm.1D (acc preloaded) */
#define crc_M_OP_SHL      11  /* SHL Vd.2D, Vn.2D, #imm */
#define crc_M_OP_SMAX     12  /* SMAX Vd.16B, Vn.16B, Vm.16B */
#define crc_M_OP_SMLAL    13  /* SMLAL Vd.2D, Vn.2S, Vm.2S (acc preloaded) */

#ifndef crc_M_OP
#define crc_M_OP crc_M_OP_ADD_EXT
#endif
#ifndef crc_M_ELEM
#define crc_M_ELEM 16
#endif
#ifndef crc_M_UNROLL
#define crc_M_UNROLL 1
#endif
#ifndef crc_M_ITERS
#define crc_M_ITERS 8192
#endif
#ifndef crc_M_OUTER
#define crc_M_OUTER 100
#endif
#ifndef crc_M_SHIFT
#define crc_M_SHIFT 0
#endif
#ifndef crc_M_MULFIXED
#define crc_M_MULFIXED 0
#endif
#ifndef crc_M_CROSS
#define crc_M_CROSS 0
#endif
#ifndef crc_M_FIXSAME
#define crc_M_FIXSAME 0
#endif

/* ABS/FABS/SHL are unary: the .S applies them to the first vector of each
 * LD2 stream (ABS V6,V2 / ABS V7,V4; 5-pair: ABS V20,V0 / ABS V21,V10), so
 * the two results come from src1 and src2 respectively. */

#if crc_M_OP == crc_M_OP_ABS || crc_M_OP == crc_M_OP_FABS || \
    crc_M_OP == crc_M_OP_SHL
#define crc_M_UNARY 1
#else
#define crc_M_UNARY 0
#endif

#if crc_M_ELEM != 16 && crc_M_ELEM != 32
#error "crc_M_ELEM must be 16 (scalar) or 32 (NEON)"
#endif
#if crc_M_UNROLL < 1 || crc_M_UNROLL > 8
#error "crc_M_UNROLL must be between 1 and 8"
#endif
#if crc_M_SHIFT < 0 || crc_M_SHIFT > 63
#error "crc_M_SHIFT must be a valid shift amount / lane index"
#endif

#define crc_M_ALIGN 64
#define crc_M_BUF_SIZE ((size_t)crc_M_ITERS * crc_M_UNROLL * crc_M_ELEM)

struct crcGroupMData {
    uint8_t *src1;
    uint8_t *src2;
    uint8_t *golden;   /* op(src1, src2) computed once at init */
    uint8_t *dst;      /* compute-pass output, reused every outer iteration */
    uint8x16_t mul;    /* fixed FMUL multiplier for the P05..P08 variants */
    uint8x16_t fix0;   /* fixed second operand (V0/V30 in the .S) */
    uint8x16_t fix1;   /* fixed second operand (V1/V31 in the .S) */
    uint64_t sfix0;    /* scalar fixed operand for the Base P03/P04 variants */
    uint64_t sfix1;    /* scalar fixed addend of MADD P03/P04 */
};

#if defined(__aarch64__)

/* --- scalar datapath ---------------------------------------------------- */

/* ADD/SUB with a zero-extended byte operand, exactly as the .S writes it. */
static inline uint64_t crc_m_add_ext(uint64_t a, uint64_t b)
{
    uint64_t r;
    __asm__ volatile ("add %w0, %w1, %w2, uxtb #0"
                      : "=r"(r) : "r"(a), "r"(b));
    return r;
}

static inline uint64_t crc_m_sub_ext(uint64_t a, uint64_t b)
{
    uint64_t r;
    __asm__ volatile ("sub %0, %1, %w2, uxtb #0"
                      : "=r"(r) : "r"(a), "r"(b));
    return r;
}

static inline uint64_t crc_m_madd(uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t r;
    __asm__ volatile ("madd %0, %1, %2, %3"
                      : "=r"(r) : "r"(a), "r"(b), "r"(c));
    return r;
}

static inline uint64_t crc_m_sdiv(uint64_t a, uint64_t b)
{
    uint64_t r;
    __asm__ volatile ("sdiv %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    return r;
}

/* --- NEON datapath ------------------------------------------------------ */

static inline uint8x16_t crc_m_v(uint64_t lo, uint64_t hi)
{
    return vcombine_u8(vcreate_u8(lo), vcreate_u8(hi));
}

/* LD2/STP/LDP need a consecutive register pair, which the generic "=w"
 * constraint does not guarantee, so pin the pair explicitly and copy it out
 * through v0/v1 (which the clobbers keep out of the operand allocation). */
static inline void crc_m_ld2(uint8x16_t &x0, uint8x16_t &x1, const void *p)
{
 __asm__ volatile ("ld2 {v0.2d, v1.2d}, [%2]\n\t"
               "mov %0.16b, v0.16b\n\t"
               "mov %1.16b, v1.16b"
               : "=w"(x0), "=w"(x1) : "r"(p)
               : "v0", "v1", "memory");

}

static inline void crc_m_ldp(uint8x16_t &x0, uint8x16_t &x1, const void *p)
{
    __asm__ volatile ("ldp q0, q1, [%2]\n\t"
                      "mov %0.16b, v0.16b\n\t"
                      "mov %1.16b, v1.16b"
                      : "=w"(x0), "=w"(x1) : "r"(p)
                      : "v0", "v1", "memory");
}

static inline void crc_m_stp(void *p, uint8x16_t x0, uint8x16_t x1)
{
    __asm__ volatile ("mov v0.16b, %1.16b\n\t"
                      "mov v1.16b, %2.16b\n\t"
                      "stp q0, q1, [%0]"
                      :: "r"(p), "w"(x0), "w"(x1)
                      : "v0", "v1", "memory");
}

/* `hi` selects the accumulator constant of the accumulate ops: the .S
 * preloads the first destination with 0x5a5a.. and the second with 0xa5a5.. */
static inline uint8x16_t crc_m_vec(uint8x16_t a, uint8x16_t b,
                            uint8x16_t mul, bool hi)
{
    (void)hi;   /* only the accumulate ops below use it */
#if crc_M_OP == crc_M_OP_ADD_EXT
    /* SimdFpLoopCheckAdd*: ADD Vd.2D, Vn.2D, Vm.2D */
    (void)mul;
    return vreinterpretq_u8_u64(vaddq_u64(vreinterpretq_u64_u8(a),
                                          vreinterpretq_u64_u8(b)));
#elif crc_M_OP == crc_M_OP_ABS
    (void)b; (void)mul;
    return vreinterpretq_u8_s64(vabsq_s64(vreinterpretq_s64_u8(a)));
#elif crc_M_OP == crc_M_OP_FABS
    (void)b; (void)mul;
    return vreinterpretq_u8_f64(vabsq_f64(vreinterpretq_f64_u8(a)));
#elif crc_M_OP == crc_M_OP_FDIV
    (void)mul;
    return vreinterpretq_u8_f64(vdivq_f64(vreinterpretq_f64_u8(a),
                                          vreinterpretq_f64_u8(b)));
#elif crc_M_OP == crc_M_OP_FMUL
    /* the multiplier vector is either the second source or a fixed one */
    {
        const uint8x16_t m = crc_M_MULFIXED ? mul : b;
        if (crc_M_SHIFT == 1)
            return vreinterpretq_u8_f64(
                vmulq_laneq_f64(vreinterpretq_f64_u8(a),
                                vreinterpretq_f64_u8(m), 1));
        return vreinterpretq_u8_f64(
            vmulq_laneq_f64(vreinterpretq_f64_u8(a),
                            vreinterpretq_f64_u8(m), 0));
    }
#elif crc_M_OP == crc_M_OP_MLA
    (void)mul;
    /* the .S preloads the accumulator with 0x5a5a../0xa5a5.. */
    return vmlaq_u8(vdupq_n_u8(hi ? 0xa5 : 0x5a), a, b);
#elif crc_M_OP == crc_M_OP_ORR
    (void)mul;
    return vorrq_u8(a, b);
#elif crc_M_OP == crc_M_OP_PMULL
    (void)mul;
    {
        uint8x16_t r;
        __asm__ volatile ("pmull %0.1q, %1.1d, %2.1d"
                          : "=w"(r) : "w"(a), "w"(b));
        return r;
    }
#elif crc_M_OP == crc_M_OP_SHL
    (void)b; (void)mul;
    if (crc_M_SHIFT == 1)
        return vreinterpretq_u8_u64(
            vshlq_n_u64(vreinterpretq_u64_u8(a), 1));
    return a;
#elif crc_M_OP == crc_M_OP_SMAX
    (void)mul;
    return vreinterpretq_u8_s8(vmaxq_s8(vreinterpretq_s8_u8(a),
                                        vreinterpretq_s8_u8(b)));
#elif crc_M_OP == crc_M_OP_SMLAL
    (void)mul;
    return vreinterpretq_u8_s64(
        vmlal_s32(vdupq_n_s64(hi ? 0xa5a5a5a5a5a5a5a5LL
                                 : 0x5a5a5a5a5a5a5a5aLL),
                  vget_low_s32(vreinterpretq_s32_u8(a)),
                  vget_low_s32(vreinterpretq_s32_u8(b))));
#else
    (void)a; (void)b; (void)mul;
    return a;
#endif
}

#endif /* __aarch64__ */

/* Scalar pair: a0/a1 are the two X registers of the src1 LDP, b0/b1 the
 * src2 LDP.
 *
 * crc_M_CROSS (Base P03/P04): the .S pairs src1[0] with a fixed register and
 * src2[0] with the same fixed register, dropping the second element of each
 * LDP. MADD additionally takes a second fixed register as its addend. */
static inline void crc_m_apply_x(uint64_t a0, uint64_t a1,
                          uint64_t b0, uint64_t b1,
                          uint64_t fix0, uint64_t fix1,
                          uint64_t &r0, uint64_t &r1)
{
    r0 = a0;
    r1 = a1;
#if defined(__aarch64__)
#if crc_M_CROSS
    /* one fixed register is the second operand of both ops; MADD takes a
     * second fixed register (X13/X29 in the .S) as its addend */
    (void)a1; (void)b1;
    switch (crc_M_OP) {
    case crc_M_OP_ADD_EXT: r0 = crc_m_add_ext(a0, fix0);
                           r1 = crc_m_add_ext(b0, fix0); break;
    case crc_M_OP_SUB_EXT: r0 = crc_m_sub_ext(a0, fix0);
                           r1 = crc_m_sub_ext(b0, fix0); break;
    case crc_M_OP_MADD:    r0 = crc_m_madd(a0, fix0, fix1);
                           r1 = crc_m_madd(b0, fix0, fix1); break;
    case crc_M_OP_SDIV:    r0 = crc_m_sdiv(a0, fix0);
                           r1 = crc_m_sdiv(b0, fix0); break;
    default: break;
    }
#else
    (void)fix0; (void)fix1;
    switch (crc_M_OP) {
    case crc_M_OP_ADD_EXT: r0 = crc_m_add_ext(a0, b0);
                           r1 = crc_m_add_ext(a1, b1); break;
    case crc_M_OP_SUB_EXT: r0 = crc_m_sub_ext(a0, b0);
                           r1 = crc_m_sub_ext(a1, b1); break;
    case crc_M_OP_MADD:
        /* the addend is the sibling element of the other pair: P01 uses the
         * second src1 register (X6), P02 the second src2 register (X18) */
        if (crc_M_UNROLL > 1)
            r0 = crc_m_madd(a0, b0, b1);
        else
            r0 = crc_m_madd(a0, b0, a1);
        r1 = crc_m_madd(a1, b1, b0); break;
    case crc_M_OP_SDIV:    r0 = crc_m_sdiv(a0, b0);
                           r1 = crc_m_sdiv(a1, b1); break;
    default: break;
    }
#endif
#else
    (void)b0; (void)b1; (void)fix0; (void)fix1;
#endif
}

#if defined(__aarch64__)
/* Vector pair. One 32-byte element is two 16-byte vectors (LD2 loads both).
 *
 * Three operand pairings appear in the .S:
 * default (P01/P02): out0 = op(a0, b0), out1 = op(a1, b1)
 * crc_M_UNARY      : out0 = op(a0),   out1 = op(b0)
 * crc_M_CROSS      : out0 = op(a0, fix0), out1 = op(b0, fix1)
 * The P03/P04 variants drop the second vector of each LD2 and use a fixed
 * vector (V0/V1 or V30/V31) as the second operand instead.
 */
static inline void crc_m_apply_v(uint8x16_t a0, uint8x16_t a1,
                          uint8x16_t b0, uint8x16_t b1,
                          uint8x16_t mul,
                          uint8x16_t fix0, uint8x16_t fix1,
                          uint8x16_t &r0, uint8x16_t &r1)
{
#if crc_M_UNARY
    /* the .S applies the op to the first vector of each LD2 stream and leaves
     * the second one unused, in both the 1-pair (ABS V6,V2 / ABS V7,V4) and
     * the 5-pair variants (ABS V20,V0 / ABS V21,V10 / ABS V22,V2 / ...) */
    (void)a1; (void)b1; (void)fix0; (void)fix1;
    r0 = crc_m_vec(a0, a0, mul, false);
    r1 = crc_m_vec(b0, b0, mul, true);
#elif crc_M_CROSS
    /* P03/P04-style: op(src1[0], fixed) and op(src2[0], fixed). For FMUL the
     * fixed vector is the multiplier (V0/V31 in the .S), so it is passed as
     * `mul` as well; crc_M_MULFIXED makes crc_m_vec pick it up. */
    (void)a1; (void)b1; (void)mul;
    r0 = crc_m_vec(a0, fix0, fix0, false);
    r1 = crc_m_vec(b0, fix1, fix1, true);
#else
    (void)fix0; (void)fix1;
    r0 = crc_m_vec(a0, b0, mul, false);
    r1 = crc_m_vec(a1, b1, mul, true);
#endif
}
#endif

/* Fill src1/src2 with random data. */
static void crc_m_fill(struct crcGroupMData *data)
{
    for (size_t off = 0; off < crc_M_BUF_SIZE; off += 16) {
        memset_random(data->src1 + off, 16);
        memset_random(data->src2 + off, 16);
    }
}

/* Compute pass: dst = op(src1, src2) over the whole buffer. */
static void crc_m_compute(const struct crcGroupMData *data)
{
    const size_t step = crc_M_ELEM;
    for (size_t off = 0; off < crc_M_BUF_SIZE; off += step * crc_M_UNROLL) {
        for (int u = 0; u < crc_M_UNROLL; u++) {
            const size_t o = off + (size_t)u * step;
#if defined(__aarch64__)
#if crc_M_ELEM == 32
            /* one 32-byte element = two Q registers loaded by LD2 */
            uint8x16_t a0, a1, b0, b1, r0, r1;
            crc_m_ld2(a0, a1, data->src1 + o);
            crc_m_ld2(b0, b1, data->src2 + o);
            crc_m_apply_v(a0, a1, b0, b1, data->mul, data->fix0,
                           data->fix1, r0, r1);
            crc_m_stp(data->dst + o, r0, r1);
#else
            uint64_t a0, a1, b0, b1, r0, r1;
            __asm__ volatile ("ldp %0, %1, [%2]"
                              : "=r"(a0), "=r"(a1) : "r"(data->src1 + o)
                              : "memory");
            __asm__ volatile ("ldp %0, %1, [%2]"
                              : "=r"(b0), "=r"(b1) : "r"(data->src2 + o)
                              : "memory");
            crc_m_apply_x(a0, a1, b0, b1, data->sfix0, data->sfix1,
                           r0, r1);
            __asm__ volatile ("stp %0, %1, [%2]"
                              :: "r"(r0), "r"(r1),
                                 "r"(data->dst + o) : "memory");
#endif
#else
            uint64_t a0 = 0, a1 = 0, b0 = 0, b1 = 0, r0, r1;
            memcpy(&a0, data->src1 + o, 8);
            memcpy(&b0, data->src2 + o, 8);
            crc_m_apply_x(a0, a1, b0, b1, data->sfix0, data->sfix1,
                           r0, r1);
            memcpy(data->dst + o, &r0, 8);
#endif
        }
    }
}

/* Verify pass: recompute, load the stored result, and compare both against
 * the golden. Returns false on the first mismatch. */
static bool crc_m_verify(const struct crcGroupMData *data)
{
    const size_t step = crc_M_ELEM;
    for (size_t off = 0; off < crc_M_BUF_SIZE; off += step * crc_M_UNROLL) {
        for (int u = 0; u < crc_M_UNROLL; u++) {
            const size_t o = off + (size_t)u * step;

#if defined(__aarch64__)
#if crc_M_ELEM == 32
            uint8x16_t a0, a1, b0, b1, r0, r1, d0, d1, g0, g1;
            crc_m_ld2(a0, a1, data->src1 + o);
            crc_m_ld2(b0, b1, data->src2 + o);
            crc_m_ldp(d0, d1, data->dst + o);
            crc_m_ldp(g0, g1, data->golden + o);
            crc_m_apply_v(a0, a1, b0, b1, data->mul, data->fix0,
                           data->fix1, r0, r1);
            if (memcmp(&r0, &d0, 16) != 0 || memcmp(&r1, &d1, 16) != 0 ||
                memcmp(&r0, &g0, 16) != 0 || memcmp(&r1, &g1, 16) != 0)
                return false;
#else
            uint64_t a0, a1, b0, b1, r0, r1, d0, d1, g0, g1;
            __asm__ volatile ("ldp %0, %1, [%2]"
                              : "=r"(a0), "=r"(a1) : "r"(data->src1 + o)
                              : "memory");
            __asm__ volatile ("ldp %0, %1, [%2]"
                              : "=r"(b0), "=r"(b1) : "r"(data->src2 + o)
                              : "memory");
            __asm__ volatile ("ldp %0, %1, [%2]"
                              : "=r"(d0), "=r"(d1) : "r"(data->dst + o)
                              : "memory");
            __asm__ volatile ("ldp %0, %1, [%2]"
                              : "=r"(g0), "=r"(g1) : "r"(data->golden + o)
                              : "memory");
            crc_m_apply_x(a0, a1, b0, b1, data->sfix0, data->sfix1,
                           r0, r1);
            if (r0 != d0 || r1 != d1 || r0 != g0 || r1 != g1)
                return false;
#endif
#else
            uint64_t a0 = 0, a1 = 0, b0 = 0, b1 = 0, r0, r1;
            uint64_t d0 = 0, d1 = 0, g0 = 0, g1 = 0;
            memcpy(&a0, data->src1 + o, 8);
            memcpy(&b0, data->src2 + o, 8);
            memcpy(&d0, data->dst + o, 8);
            memcpy(&g0, data->golden + o, 8);
            crc_m_apply_x(a0, a1, b0, b1, data->sfix0, data->sfix1,
                           r0, r1);
            if (r0 != d0 || r1 != d1 || r0 != g0 || r1 != g1)
                return false;
#endif   /* __aarch64__ */
        }
    }
    return true;
}

static int crc_m_init(struct test *test)
{
#if defined(__aarch64__)
    auto *data = static_cast<crcGroupMData *>(malloc(sizeof(crcGroupMData)));
    if (!data)
        return EXIT_FAILURE;
    memset(data, 0, sizeof(*data));

    const size_t alloc_size =
        (crc_M_BUF_SIZE + crc_M_ALIGN - 1) & ~(size_t)(crc_M_ALIGN - 1);
    data->src1   = static_cast<uint8_t *>(aligned_alloc(crc_M_ALIGN, alloc_size));
    data->src2   = static_cast<uint8_t *>(aligned_alloc(crc_M_ALIGN, alloc_size));
    data->dst    = static_cast<uint8_t *>(aligned_alloc(crc_M_ALIGN, alloc_size));
    data->golden = static_cast<uint8_t *>(aligned_alloc(crc_M_ALIGN, alloc_size));
    if (!data->src1 || !data->src2 || !data->dst || !data->golden) {
        free(data->src1); free(data->src2);
        free(data->dst);  free(data->golden);
        free(data);
        return EXIT_FAILURE;
    }

    crc_m_fill(data);

    /* fixed operands: the FMUL multiplier of the P05..P08 variants and the
     * V0/V1 (or V30/V31) second operands of the P03/P04 cross variants */
    {
        uint64_t lo, hi;
        memset_random(&lo, 8);
        memset_random(&hi, 8);
        data->mul = crc_m_v(lo, hi);
        memset_random(&lo, 8);
        memset_random(&hi, 8);
        data->fix0 = crc_m_v(lo, hi);
        if (crc_M_FIXSAME) {
            data->fix1 = data->fix0;
        } else {
            memset_random(&lo, 8);
            memset_random(&hi, 8);
            data->fix1 = crc_m_v(lo, hi);
        }
        memset_random(&data->sfix0, 8);
        memset_random(&data->sfix1, 8);
    }

    /* golden = one compute pass over the pristine sources */
    crc_m_compute(data);
    memcpy(data->golden, data->dst, crc_M_BUF_SIZE);

    test->data = data;
    return EXIT_SUCCESS;

#else
    (void)test;
    return EXIT_SUCCESS;
#endif
}

static int crc_m_run(struct test *test, int cpu)
{
#if defined(__aarch64__)
    (void)cpu;
    auto *data = static_cast<crcGroupMData *>(test->data);

    /* TEST_LOOP repeats the whole 100-iteration outer loop 16 times, the
     * same amplification the other crc ports use to fill a test slot. */
    TEST_LOOP(test, 1 << 4) {
        for (int outer = 0; outer < crc_M_OUTER; outer++) {
            crc_m_compute(data);
#if defined(__aarch64__)
            __asm__ volatile ("dsb sy" ::: "memory");
#endif
            if (!crc_m_verify(data))
                report_fail_msg("Loop and Cumulative Checksum loop-check miscompare");
        }
    }
    return EXIT_SUCCESS;
#else
    (void)cpu;
    return EXIT_SUCCESS;
#endif
}

static int crc_m_cleanup(struct test *test)
{
    auto *data = static_cast<crcGroupMData *>(test->data);
    if (data) {
        free(data->src1);
        free(data->src2);
        free(data->dst);
        free(data->golden);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(loop_check_m, "Loop and Cumulative Checksum (LoopCheck group M): compute + DSB SY + recompute dual-reference verify")
    .test_init    = crc_m_init,
    .test_run     = crc_m_run,
    .test_cleanup = crc_m_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
