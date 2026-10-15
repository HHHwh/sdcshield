/**
 * @file
 *
 * @copyright
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared NEON PMULL CRC-32 folding implementation for the zpclmul and
 * zpclmul_rep tests (ARM64). Folds the IEEE 802.3 CRC-32 (reflected
 * polynomial 0xEDB88320) over 128-bit vectors using PMULL/PMULL2
 * carry-less multiplication: a 4-accumulator main loop, a 3-level
 * fold-down, a partial-vector tail fold and a Barrett reduction down
 * to 32 bits (structure after libdeflate's crc32_arm_pmullx4; folding
 * constants are the precomputed x^N mod G(x) / Barrett constants from
 * its gen-crc32-consts.py).
 *
 * All vector loads go through vld1q_u8(), which is defined for any
 * alignment - the folding operates on plain uint8_t buffers with no
 * 16-byte alignment guarantee, so a uint8x16_t-typed pointer must
 * never be dereferenced directly (libdeflate aligns its input first;
 * here the buffers may be unaligned).
 */

#ifndef CRC32_PMULL_FOLD_H
#define CRC32_PMULL_FOLD_H

#include <sandstone.h>
#include <cstdint>

#ifdef __aarch64__
#include <arm_neon.h>

/* ---- folding constants (libdeflate lib/crc32_multipliers.h) ---- */
static constexpr uint64_t CRC32_X159_MODG = 0xae689191ULL; /* x^159 mod G(x) */
static constexpr uint64_t CRC32_X95_MODG = 0xccaa009eULL;  /* x^95 mod G(x) */
static constexpr uint64_t CRC32_X287_MODG = 0xf1da05aaULL; /* x^287 mod G(x) */
static constexpr uint64_t CRC32_X223_MODG = 0x81256527ULL; /* x^223 mod G(x) */
static constexpr uint64_t CRC32_X543_MODG = 0x8f352d95ULL; /* x^543 mod G(x) */
static constexpr uint64_t CRC32_X479_MODG = 0x1d9513d7ULL; /* x^479 mod G(x) */
static constexpr uint64_t CRC32_BARRETT_1 = 0xb4e5b025f7011641ULL; /* floor(x^95/G) */
static constexpr uint64_t CRC32_BARRETT_2 = 0x00000001db710641ULL; /* G(x) */

/* len < 16 fallback: bitwise reflected CRC-32 (raw state domain,
 * i.e. pre-complement - same domain zlib uses internally). */
static inline uint32_t crc32_slice1(uint32_t crc, const uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return crc;
}

/* ---- PMULL helpers (libdeflate lib/arm/crc32_pmull_helpers.h) ---- */

/* crc into the low 4 bytes (little-endian lane 0), rest zeroed */
static inline uint8x16_t u32_to_bytevec(uint32_t a)
{
    return vreinterpretq_u8_u32(vsetq_lane_u32(a, vdupq_n_u32(0), 0));
}

/* two u64s loaded as poly64x2 (lane 0 = low 64 bits) */
static inline poly64x2_t load_multipliers(const uint64_t p[2])
{
    return vreinterpretq_p64_u64(vld1q_u64(p));
}

/* low 64 bits x low 64 bits, carry-less */
__attribute__((target("+crypto")))
static inline uint8x16_t clmul_low(uint8x16_t a, poly64x2_t b)
{
    return vreinterpretq_u8_p128(
            vmull_p64((poly64_t)vgetq_lane_u64(vreinterpretq_u64_u8(a), 0),
                      (poly64_t)vgetq_lane_u64(vreinterpretq_u64_p64(b), 0)));
}

/* high 64 bits x high 64 bits, carry-less */
__attribute__((target("+crypto")))
static inline uint8x16_t clmul_high(uint8x16_t a, poly64x2_t b)
{
    return vreinterpretq_u8_p128(
            vmull_high_p64(vreinterpretq_p64_u8(a), b));
}

/* one folding step: src.lo*m.lo ^ src.hi*m.hi ^ dst */
__attribute__((target("+crypto")))
static inline uint8x16_t fold_vec(uint8x16_t src, uint8x16_t dst,
                                  poly64x2_t multipliers)
{
    return veorq_u8(veorq_u8(clmul_low(src, multipliers),
                             clmul_high(src, multipliers)), dst);
}

/*
 * v holds a 16-byte polynomial; p points at the 1..15 bytes of data
 * that follow it (a full 16 bytes are readable from p+len-16). The
 * partial data is bit-shifted in via a byte-shuffle and the merged
 * vector is folded by multipliers_1.
 */
__attribute__((target("+crypto")))
static inline uint8x16_t fold_partial_vec(uint8x16_t v, const uint8_t *p,
                                          size_t len, poly64x2_t multipliers_1)
{
    /* Number of bytes needed from the next vector; lshift/rshift are
     * the per-lane byte-shuffle indices implementing << and >> by
     * that many bytes. */
    static const uint8_t shift_tab[48] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    const uint8x16_t lshift = vld1q_u8(&shift_tab[len]);
    const uint8x16_t rshift = vld1q_u8(&shift_tab[len + 16]);
    const uint8x16_t mask =
        vreinterpretq_u8_s8(vshrq_n_s8(vreinterpretq_s8_u8(rshift), 7));
    uint8x16_t x0, x1;

    /* x0 = v truncated to 'len' bytes and left-justified */
    x0 = vqtbl1q_u8(v, lshift);

    /* x1 = the last '16 - len' bytes of v, followed by 'len' bytes
     * of new data (mask 0 selects the v lookup, 1 selects the load) */
    x1 = vbslq_u8(mask, vld1q_u8(p + len - 16), vqtbl1q_u8(v, rshift));

    return fold_vec(x0, x1, multipliers_1);
}

/*
 * CRC-32 polynomial folding over PMULL/PMULL2 (structure after
 * libdeflate crc32_arm_pmullx4). Operates in the raw state domain
 * (pre-complement); the callers do the two complementations.
 * Handles any length:
 *   len < 16        -> bitwise fallback
 *   len < 64 + 15   -> single accumulator, one fold per 16 bytes
 *   len >= 64 + 15  -> 4 accumulators, then a 3-level fold-down
 * followed by a partial-vector tail fold and Barrett reduction.
 */
__attribute__((target("+crypto")))
static inline uint32_t crc32_pmull(uint32_t crc, const uint8_t *p, size_t len)
{
    static const uint64_t mults[3][2] __attribute__((aligned(16))) = {
        { CRC32_X159_MODG, CRC32_X95_MODG },   /* 1 vec  */
        { CRC32_X543_MODG, CRC32_X479_MODG },  /* 4 vecs */
        { CRC32_X287_MODG, CRC32_X223_MODG },  /* 2 vecs */
    };
    static const uint64_t barrett_consts[3][2] __attribute__((aligned(16))) = {
        { CRC32_X95_MODG, },
        { CRC32_BARRETT_1, },
        { CRC32_BARRETT_2, },
    };
    const poly64x2_t multipliers_1 = load_multipliers(mults[0]);
    uint8x16_t v0, v1, v2, v3;

    if (len < 64 + 15) {
        if (len < 16)
            return crc32_slice1(crc, p, len);
        v0 = veorq_u8(vld1q_u8(p), u32_to_bytevec(crc));
        p += 16;
        len -= 16;
        while (len >= 16) {
            v0 = fold_vec(v0, vld1q_u8(p), multipliers_1);
            p += 16;
            len -= 16;
        }
    } else {
        const poly64x2_t multipliers_4 = load_multipliers(mults[1]);
        const poly64x2_t multipliers_2 = load_multipliers(mults[2]);

        v0 = veorq_u8(vld1q_u8(p), u32_to_bytevec(crc));
        p += 16;
        v1 = vld1q_u8(p); p += 16;
        v2 = vld1q_u8(p); p += 16;
        v3 = vld1q_u8(p); p += 16;
        while (len >= 64 + 64) {
            v0 = fold_vec(v0, vld1q_u8(p), multipliers_4); p += 16;
            v1 = fold_vec(v1, vld1q_u8(p), multipliers_4); p += 16;
            v2 = fold_vec(v2, vld1q_u8(p), multipliers_4); p += 16;
            v3 = fold_vec(v3, vld1q_u8(p), multipliers_4); p += 16;
            len -= 64;
        }
        v0 = fold_vec(v0, v2, multipliers_2);
        v1 = fold_vec(v1, v3, multipliers_2);
        if (len & 32) {
            v0 = fold_vec(v0, vld1q_u8(p), multipliers_2); p += 16;
            v1 = fold_vec(v1, vld1q_u8(p), multipliers_2); p += 16;
        }
        v0 = fold_vec(v0, v1, multipliers_1);
        if (len & 16) {
            v0 = fold_vec(v0, vld1q_u8(p), multipliers_1); p += 16;
        }
        len &= 15;
    }

    /* Fold in the trailing 1..15 bytes before coming down to 32 bits */
    if (len)
        v0 = fold_partial_vec(v0, p, len, multipliers_1);

    /* Barrett reduction to a 32-bit remainder */
    v0 = veorq_u8(clmul_low(v0, load_multipliers(barrett_consts[0])),
                  vextq_u8(v0, vdupq_n_u8(0), 8));
    v1 = clmul_low(v0, load_multipliers(barrett_consts[1]));
    v1 = clmul_low(v1, load_multipliers(barrett_consts[2]));
    v0 = veorq_u8(v0, v1);
    return vgetq_lane_u32(vreinterpretq_u32_u8(v0), 2);
}

#endif /* __aarch64__ */

/*
 * Runtime gating for the PMULL folding tests: target("+crypto") only
 * governs codegen, not the executing CPU, so on a part lacking the
 * crypto extension the PMULL instructions would fault (SIGILL). The
 * framework checks .minimum_cpu before test_init and skips cleanly.
 * Mirrors framework/sandstone_ssl.h's IPSEC_X86_GATE pattern: the
 * cpu_feature_pmull symbol only exists in the AArch64 feature
 * contract, so on other architectures the gate is dropped (0).
 */
#ifdef __aarch64__
#define ZPCLMUL_MIN_CPU cpu_feature_pmull
#else
#define ZPCLMUL_MIN_CPU 0
#endif

#endif /* CRC32_PMULL_FOLD_H */
