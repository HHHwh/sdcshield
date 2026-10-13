/**
 * @copyright
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b zpclmul
 * @parblock
 * Zlib PCLMUL CRC32 calculation test - crc folding (ARM64 NEON PMULL version)
 *
 * The hardware path folds the IEEE 802.3 CRC-32 (reflected polynomial
 * 0xEDB88320) over 128-bit vectors using PMULL/PMULL2 carry-less
 * multiplication: a 4-accumulator main loop, a 3-level fold-down, a
 * partial-vector tail fold and a Barrett reduction down to 32 bits
 * (structure after libdeflate's crc32_arm_pmullx4; folding constants
 * are the precomputed x^N mod G(x) / Barrett constants from its
 * gen-crc32-consts.py). The golden is zlib's independent byte-sliced
 * software crc32(). A known-answer self-check ("123456789" ->
 * 0xCBF43926, plus vectors covering the tail-folding path) gates the
 * random loop. Input data is random bytes with a random length in
 * [16, BLOCK_SIZE] so both folding branches are exercised; each
 * iteration also validates store-to-load consistency.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstring>
#include <random>
#include <zlib.h>

#ifdef __aarch64__
#include <arm_neon.h>

static constexpr size_t BLOCK_SIZE = 4096;

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
static uint32_t crc32_slice1(uint32_t crc, const uint8_t *p, size_t len)
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
 * (pre-complement); the zlib-API wrapper below does the two
 * complementations. Handles any length:
 *   len < 16        -> bitwise fallback
 *   len < 64 + 15   -> single accumulator, one fold per 16 bytes
 *   len >= 64 + 15  -> 4 accumulators, then a 3-level fold-down
 * followed by a partial-vector tail fold and Barrett reduction.
 */
__attribute__((target("+crypto")))
static uint32_t crc32_pmull(uint32_t crc, const uint8_t *p, size_t len)
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
        const uint8x16_t *vp;

        v0 = veorq_u8(vld1q_u8(p), u32_to_bytevec(crc));
        p += 16;
        vp = (const uint8x16_t *)p;
        v1 = *vp++;
        v2 = *vp++;
        v3 = *vp++;
        while (len >= 64 + 64) {
            v0 = fold_vec(v0, *vp++, multipliers_4);
            v1 = fold_vec(v1, *vp++, multipliers_4);
            v2 = fold_vec(v2, *vp++, multipliers_4);
            v3 = fold_vec(v3, *vp++, multipliers_4);
            len -= 64;
        }
        v0 = fold_vec(v0, v2, multipliers_2);
        v1 = fold_vec(v1, v3, multipliers_2);
        if (len & 32) {
            v0 = fold_vec(v0, *vp++, multipliers_2);
            v1 = fold_vec(v1, *vp++, multipliers_2);
        }
        v0 = fold_vec(v0, v1, multipliers_1);
        if (len & 16)
            v0 = fold_vec(v0, *vp++, multipliers_1);
        p = (const uint8_t *)vp;
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

/* DUT: NEON PMULL carry-less-multiplication CRC-32 folding, zlib-API
 * semantics. Independent of the zlib golden below in both algorithm
 * and code path. */
static uint32_t crc32_hardware(const uint8_t *data, size_t len, uint32_t crc)
{
    /* zlib API <-> raw state domain: result = ~raw(~crc) */
    return ~crc32_pmull(~crc, data, len);
}

/* Golden: zlib software CRC-32 (byte-sliced tables, no NEON) */
static uint32_t crc32_software(const uint8_t *data, size_t len, uint32_t crc)
{
    return crc32(crc, data, len);
}

/* Known-answer self-check gating the random loop: the standard
 * "123456789" -> 0xCBF43926 vector exercises the len < 16 branch, and
 * "123456789" x15 (135 bytes, 135 % 16 = 7 tail) exercises the 4-
 * accumulator main loop plus the partial-vector tail fold. Both are
 * additionally cross-checked against the zlib golden. */
static bool kat_selfcheck()
{
    static const char kat9[] = "123456789";
    static const size_t kat9_len = 9;
    uint8_t kat135[135];

    for (int i = 0; i < 15; i++)
        memcpy(kat135 + i * 9, kat9, kat9_len);

    if (crc32_hardware(reinterpret_cast<const uint8_t *>(kat9), kat9_len, 0)
            != 0xCBF43926)
        return false;
    for (size_t len : { kat9_len, sizeof(kat135) }) {
        const uint8_t *p = (len == kat9_len)
                        ? reinterpret_cast<const uint8_t *>(kat9) : kat135;
        if (crc32_hardware(p, len, 0) != crc32_software(p, len, 0))
            return false;
    }
    return true;
}

static int zpclmul_run(struct test *test, int cpu) {
    (void)cpu;

    if (!kat_selfcheck()) {
        report_fail_msg("zpclmul: known-answer self-check failed");
        return EXIT_FAILURE;
    }

    alignas(16) uint8_t input[BLOCK_SIZE];
    alignas(16) uint8_t store_buf[BLOCK_SIZE];
    alignas(16) uint8_t reload_buf[BLOCK_SIZE];

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint8_t> byte_dist(0, 255);
    /* Random length >= 16 keeps both folding branches and the tail
     * fold reachable; the len < 16 branch is covered by the KAT. */
    std::uniform_int_distribution<int> len_dist(16, BLOCK_SIZE);

    do {
        size_t len = static_cast<size_t>(len_dist(rng));
        for (size_t i = 0; i < len; ++i)
            input[i] = byte_dist(rng);

        uint32_t hw_crc = crc32_hardware(input, len, 0);
        uint32_t sw_crc = crc32_software(input, len, 0);
        bool crc_ok = (hw_crc == sw_crc);

        memcpy(store_buf, input, len);
        memcpy(reload_buf, store_buf, len);
        bool consistent = (memcmp(input, reload_buf, len) == 0);

        if (!(crc_ok && consistent)) {
            report_fail_msg("zpclmul: crc_ok=%d, consistent=%d, len=%zu, "
                            "hw_crc=0x%08X, sw_crc=0x%08X",
                            crc_ok, consistent, len, hw_crc, sw_crc);
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

#else /* !__aarch64__ */

static int zpclmul_run(struct test *test, int cpu) {
    (void)cpu;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM64 NEON PMULL required");
    return EXIT_SKIP;
}

#endif /* __aarch64__ */

static int zpclmul_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int zpclmul_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(zpclmul, "Zlib PCLMUL CRC32 calculation test - crc folding (ARM64 NEON PMULL)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = zpclmul_init,
    .test_run = zpclmul_run,
    .test_cleanup = zpclmul_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
