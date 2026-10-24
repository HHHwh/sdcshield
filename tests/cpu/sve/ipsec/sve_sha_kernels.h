/**
 * @file
 *
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Multi-stream SVE SHA-1/SHA-224/SHA-256/SHA-384/SHA-512 kernels for the
 * ipsec SVE ports (Task 5 of the avx53 SVE port plan).
 *
 * Design: SHA compression is serial *within* one message (block i+1 chains
 * on block i's state) but independent *across* messages. The kernels
 * process N independent messages in parallel — one per 32-bit SVE lane
 * (SHA-256 family) or one per 64-bit lane (SHA-512 family), N =
 * svcntw()/svcntd() at the caller's runtime VL (all-VL Phase 2 P4) — with
 * message schedules and round state entirely in SVE registers.
 *
 * Correctness gate: verified against OpenSSL EVP digests on a large
 * vector set (see the standalone verifier) before use as a golden base.
 */
#ifndef SVE_SHA_KERNELS_H
#define SVE_SHA_KERNELS_H

#include <arm_sve.h>
#include <cstdint>
#include <cstddef>

/* GCC 12 的 arm_sve.h 没有旋转 intrinsic (svror/svrol) —— 用移位组合实现。
 * ROTR32(x,n) = (x>>n)|(x<<(32-n)); ROTL32 同理; 64-bit 版本宽度换 64。 */
#define SVE_ROTR32(pg, x, n) \
    svorr_u32_x(pg, svlsr_n_u32_x(pg, (x), (n)), svlsl_n_u32_x(pg, (x), 32 - (n)))
#define SVE_ROTL32(pg, x, n) \
    svorr_u32_x(pg, svlsl_n_u32_x(pg, (x), (n)), svlsr_n_u32_x(pg, (x), 32 - (n)))
#define SVE_ROTR64(pg, x, n) \
    svorr_u64_x(pg, svlsr_n_u64_x(pg, (x), (n)), svlsl_n_u64_x(pg, (x), 64 - (n)))

/* All-VL Phase 2 P4: stream count = runtime lane count (one message per
   lane); arrays sized to the 1024-bit cap. The vector code itself is
   lane-count-agnostic (ptrue auto-scales; the schedule is per-column). */
#define SVE_SHA_STREAMS_MAX_32 32   /* svcntw() at VL=1024-bit */
#define SVE_SHA_STREAMS_MAX_64 16   /* svcntd() at VL=1024-bit */

/* ================= SHA-256 family (SHA-224/SHA-256), 8×32-bit lanes ================= */

static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

static const uint32_t sha256_iv[8] = {
    0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
};
static const uint32_t sha224_iv[8] = {
    0xc1059ed8,0x367cd507,0x3070dd17,0xf70e5939,0xffc00b31,0x68581511,0x64f98fa7,0xbefa4fa4
};

/* N 条等长消息并行压缩 (N = svcntw(), 32-bit lane 每条一流)。msgs: N 条消息
 * 指针(各 nblocks*64B, 已含 padding 与长度); out: out[w][s] 写第 s 条消息的
 * 8 个状态字(小端 uint32)。streams = svcntw() at the caller's runtime VL
 * (all-VL Phase 2: 调用方 128..1024-bit 门控, 此处 n∈[4,32] 由 VL 决定)。 */
static inline void sha256_compress_x8(uint32_t out[8][SVE_SHA_STREAMS_MAX_32],   /* [word][stream] */
                                      const uint8_t *const msgs[SVE_SHA_STREAMS_MAX_32],
                                      uint32_t nblocks,
                                      const uint32_t iv[8])
{
    /* 流数 = 运行时 lane 数(内部推导): 向量通路按 VL 动作, n≡lane 数。 */
    const int n = (int)svcntw();
    svbool_t pg = svptrue_b32();

    svuint32_t a = svdup_u32(iv[0]), b = svdup_u32(iv[1]),
               c = svdup_u32(iv[2]), d = svdup_u32(iv[3]),
               e = svdup_u32(iv[4]), f = svdup_u32(iv[5]),
               g = svdup_u32(iv[6]), h = svdup_u32(iv[7]);

    for (uint32_t blk = 0; blk < nblocks; ++blk) {
        /* 保存链接值 (滚动) */
        svuint32_t a0 = a, b0 = b, c0 = c, d0 = d, e0 = e, f0 = f, g0 = g, h0 = h;

        /* SVE 类型不能做数组元素(可变长度类型禁入聚合) —— 消息调度字表
         * 驻留内存 uint32_t[64][SVE_SHA_STREAMS_MAX_32] (lane=流, 行宽按
         * 1024-bit 上限定维——修复旧 [8] 定维在 VL>=512 时的越界栈写),
         * 每步用 svld1/svst1 往返。调度展开与轮函数仍在 SVE 向量里算。 */
        uint32_t W[64][SVE_SHA_STREAMS_MAX_32];
        for (int t = 0; t < 16; ++t) {
            for (int s = 0; s < n; ++s) {
                const uint8_t *p = msgs[s] + (size_t)blk * 64 + (size_t)t * 4;
                W[t][s] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                          ((uint32_t)p[2] << 8) | (uint32_t)p[3];
            }
        }
        for (int t = 16; t < 64; ++t) {
            svuint32_t m15 = svld1_u32(pg, W[t-15]);
            svuint32_t m2  = svld1_u32(pg, W[t-2]);
            svuint32_t s0 = sveor_u32_x(pg, SVE_ROTR32(pg, m15, 7),
                                         SVE_ROTR32(pg, m15, 18));
            s0 = sveor_u32_x(pg, s0, svlsr_n_u32_x(pg, m15, 3));
            svuint32_t s1 = sveor_u32_x(pg, SVE_ROTR32(pg, m2, 17),
                                         SVE_ROTR32(pg, m2, 19));
            s1 = sveor_u32_x(pg, s1, svlsr_n_u32_x(pg, m2, 10));
            svuint32_t wt = svadd_u32_x(pg,
                               svadd_u32_x(pg, svld1_u32(pg, W[t-16]), s0),
                               svadd_u32_x(pg, svld1_u32(pg, W[t-7]), s1));
            svst1_u32(pg, W[t], wt);
        }
        for (int t = 0; t < 64; ++t) {
            svuint32_t S1 = sveor_u32_x(pg, SVE_ROTR32(pg, e, 6),
                                         SVE_ROTR32(pg, e, 11));
            S1 = sveor_u32_x(pg, S1, SVE_ROTR32(pg, e, 25));
            svuint32_t ch = sveor_u32_x(pg, svand_u32_x(pg, e, f),
                                         svand_u32_x(pg, svnot_u32_x(pg, e), g));
            svuint32_t t1 = svadd_u32_x(pg, svadd_u32_x(pg, h, S1),
                           svadd_u32_x(pg, ch,
                           svadd_u32_x(pg, svdup_u32(sha256_k[t]),
                                       svld1_u32(pg, W[t]))));
            svuint32_t S0 = sveor_u32_x(pg, SVE_ROTR32(pg, a, 2),
                                         SVE_ROTR32(pg, a, 13));
            S0 = sveor_u32_x(pg, S0, SVE_ROTR32(pg, a, 22));
            svuint32_t maj = sveor_u32_x(pg, svand_u32_x(pg, a, b),
                                          svand_u32_x(pg, sveor_u32_x(pg, a, b), c));
            svuint32_t t2 = svadd_u32_x(pg, S0, maj);
            h = g; g = f; f = e;
            e = svadd_u32_x(pg, d, t1);
            d = c; c = b; b = a;
            a = svadd_u32_x(pg, t1, t2);
        }
        /* 滚动链接: state += block 前的链接值 */
        a = svadd_u32_x(pg, a, a0);
        b = svadd_u32_x(pg, b, b0);
        c = svadd_u32_x(pg, c, c0);
        d = svadd_u32_x(pg, d, d0);
        e = svadd_u32_x(pg, e, e0);
        f = svadd_u32_x(pg, f, f0);
        g = svadd_u32_x(pg, g, g0);
        h = svadd_u32_x(pg, h, h0);
    }

    svst1_u32(pg, out[0], a);
    svst1_u32(pg, out[1], b);
    svst1_u32(pg, out[2], c);
    svst1_u32(pg, out[3], d);
    svst1_u32(pg, out[4], e);
    svst1_u32(pg, out[5], f);
    svst1_u32(pg, out[6], g);
    svst1_u32(pg, out[7], h);
}

/* ================= SHA-512 family (SHA-384/SHA-512), 4×64-bit lanes ================= */

static const uint64_t sha512_k[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};
static const uint64_t sha512_iv[8] = {
    0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL
};
static const uint64_t sha384_iv[8] = {
    0xcbbb9d5dc1059ed8ULL,0x629a292a367cd507ULL,0x9159015a3070dd17ULL,0x152fecd8f70e5939ULL,
    0x67332667ffc00b31ULL,0x8eb44a8768581511ULL,0xdb0c2e0d64f98fa7ULL,0x47b5481dbefa4fa4ULL
};

/* N 条等长消息并行压缩 (64-bit lane, N = svcntd())。msgs: N 条消息指针(各
 * nblocks*128B); out[word][stream]: 8 个字 × N 条流。streams = svcntd() at
 * the caller's runtime VL (all-VL Phase 2: 调用方 128..1024-bit 门控, 此处
 * n∈[2,16] 由 VL 决定)。 */
static inline void sha512_compress_x4(uint64_t out[8][SVE_SHA_STREAMS_MAX_64],   /* [word][stream] */
                                      const uint8_t *const msgs[SVE_SHA_STREAMS_MAX_64],
                                      uint32_t nblocks,
                                      const uint64_t iv[8])
{
    /* 流数 = 运行时 lane 数(内部推导): 向量通路按 VL 动作, n≡lane 数。 */
    const int n = (int)svcntd();
    svbool_t pg = svptrue_b64();

    svuint64_t a = svdup_u64(iv[0]), b = svdup_u64(iv[1]),
               c = svdup_u64(iv[2]), d = svdup_u64(iv[3]),
               e = svdup_u64(iv[4]), f = svdup_u64(iv[5]),
               g = svdup_u64(iv[6]), h = svdup_u64(iv[7]);

    for (uint32_t blk = 0; blk < nblocks; ++blk) {
        svuint64_t a0 = a, b0 = b, c0 = c, d0 = d, e0 = e, f0 = f, g0 = g, h0 = h;

        uint64_t W[80][SVE_SHA_STREAMS_MAX_64];
        for (int t = 0; t < 16; ++t) {
            for (int s = 0; s < n; ++s) {
                const uint8_t *p = msgs[s] + (size_t)blk * 128 + (size_t)t * 8;
                uint64_t w = 0;
                for (int k = 0; k < 8; ++k)
                    w = (w << 8) | p[k];
                W[t][s] = w;
            }
        }
        for (int t = 16; t < 80; ++t) {
            svuint64_t m15 = svld1_u64(pg, W[t-15]);
            svuint64_t m2  = svld1_u64(pg, W[t-2]);
            svuint64_t s0 = sveor_u64_x(pg, SVE_ROTR64(pg, m15, 1),
                                         SVE_ROTR64(pg, m15, 8));
            s0 = sveor_u64_x(pg, s0, svlsr_n_u64_x(pg, m15, 7));
            svuint64_t s1 = sveor_u64_x(pg, SVE_ROTR64(pg, m2, 19),
                                         SVE_ROTR64(pg, m2, 61));
            s1 = sveor_u64_x(pg, s1, svlsr_n_u64_x(pg, m2, 6));
            svuint64_t wt = svadd_u64_x(pg,
                               svadd_u64_x(pg, svld1_u64(pg, W[t-16]), s0),
                               svadd_u64_x(pg, svld1_u64(pg, W[t-7]), s1));
            svst1_u64(pg, W[t], wt);
        }
        for (int t = 0; t < 80; ++t) {
            svuint64_t S1 = sveor_u64_x(pg, SVE_ROTR64(pg, e, 14),
                                         SVE_ROTR64(pg, e, 18));
            S1 = sveor_u64_x(pg, S1, SVE_ROTR64(pg, e, 41));
            svuint64_t ch = sveor_u64_x(pg, svand_u64_x(pg, e, f),
                                         svand_u64_x(pg, svnot_u64_x(pg, e), g));
            svuint64_t t1 = svadd_u64_x(pg, svadd_u64_x(pg, h, S1),
                           svadd_u64_x(pg, ch,
                           svadd_u64_x(pg, svdup_u64(sha512_k[t]),
                                       svld1_u64(pg, W[t]))));
            svuint64_t S0 = sveor_u64_x(pg, SVE_ROTR64(pg, a, 28),
                                         SVE_ROTR64(pg, a, 34));
            S0 = sveor_u64_x(pg, S0, SVE_ROTR64(pg, a, 39));
            svuint64_t maj = sveor_u64_x(pg, svand_u64_x(pg, a, b),
                                          svand_u64_x(pg, sveor_u64_x(pg, a, b), c));
            svuint64_t t2 = svadd_u64_x(pg, S0, maj);
            h = g; g = f; f = e;
            e = svadd_u64_x(pg, d, t1);
            d = c; c = b; b = a;
            a = svadd_u64_x(pg, t1, t2);
        }
        a = svadd_u64_x(pg, a, a0);
        b = svadd_u64_x(pg, b, b0);
        c = svadd_u64_x(pg, c, c0);
        d = svadd_u64_x(pg, d, d0);
        e = svadd_u64_x(pg, e, e0);
        f = svadd_u64_x(pg, f, f0);
        g = svadd_u64_x(pg, g, g0);
        h = svadd_u64_x(pg, h, h0);
    }

    svst1_u64(pg, out[0], a);
    svst1_u64(pg, out[1], b);
    svst1_u64(pg, out[2], c);
    svst1_u64(pg, out[3], d);
    svst1_u64(pg, out[4], e);
    svst1_u64(pg, out[5], f);
    svst1_u64(pg, out[6], g);
    svst1_u64(pg, out[7], h);
}

/* ================= SHA-1 (HMAC-SHA1-96 用), N×32-bit lanes ================= */

/* N 条等长消息并行压缩 (32-bit lane, N = svcntw())。msgs: N 条消息指针(各
 * nblocks*64B); out[w][s]: 5 个状态字 × N 条流。streams = svcntw() at the
 * caller's runtime VL (all-VL Phase 2: 调用方 128..1024-bit 门控, 此处
 * n∈[4,32] 由 VL 决定)。(旧注释 "4 条 64-bit lane" 系 sha512 段复制残留,
 * 实况一直是 32-bit lane。) */

static inline void sha1_compress_x8(uint32_t out[5][SVE_SHA_STREAMS_MAX_32],   /* [word][stream] */
                                    const uint8_t *const msgs[SVE_SHA_STREAMS_MAX_32],
                                    uint32_t nblocks)
{
    /* 流数 = 运行时 lane 数(内部推导): 向量通路按 VL 动作, n≡lane 数。 */
    const int n = (int)svcntw();
    static const uint32_t iv[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    svbool_t pg = svptrue_b32();

    svuint32_t a = svdup_u32(iv[0]), b = svdup_u32(iv[1]),
               c = svdup_u32(iv[2]), d = svdup_u32(iv[3]),
               e = svdup_u32(iv[4]);

    for (uint32_t blk = 0; blk < nblocks; ++blk) {
        svuint32_t a0 = a, b0 = b, c0 = c, d0 = d, e0 = e;

        uint32_t W[80][SVE_SHA_STREAMS_MAX_32];
        for (int t = 0; t < 16; ++t) {
            for (int s = 0; s < n; ++s) {
                const uint8_t *p = msgs[s] + (size_t)blk * 64 + (size_t)t * 4;
                W[t][s] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                          ((uint32_t)p[2] << 8) | (uint32_t)p[3];
            }
        }
        for (int t = 16; t < 80; ++t) {
            svuint32_t x = sveor_u32_x(pg, svld1_u32(pg, W[t-3]),
                                       svld1_u32(pg, W[t-8]));
            x = sveor_u32_x(pg, x, svld1_u32(pg, W[t-14]));
            x = sveor_u32_x(pg, x, svld1_u32(pg, W[t-16]));
            svst1_u32(pg, W[t], SVE_ROTL32(pg, x, 1));   /* SHA-1 调度是左旋 1 (ROTL, 不是 ROTR —— 移植 bug 已修) */
        }
        for (int t = 0; t < 80; ++t) {
            svuint32_t k, f;
            if (t < 20) {
                f = svorr_u32_x(pg, svand_u32_x(pg, b, c),
                               svand_u32_x(pg, svnot_u32_x(pg, b), d));
                k = svdup_u32(0x5a827999u);
            } else if (t < 40) {
                f = sveor_u32_x(pg, sveor_u32_x(pg, b, c), d);
                k = svdup_u32(0x6ed9eba1u);
            } else if (t < 60) {
                /* majority = (b&c)|(b&d)|(c&d) */
                f = svorr_u32_x(pg, svorr_u32_x(pg, svand_u32_x(pg, b, c),
                                               svand_u32_x(pg, b, d)),
                               svand_u32_x(pg, c, d));
                k = svdup_u32(0x8f1bbcdcu);
            } else {
                f = sveor_u32_x(pg, sveor_u32_x(pg, b, c), d);
                k = svdup_u32(0xca62c1d6u);
            }
            svuint32_t tmp = svadd_u32_x(pg, SVE_ROTL32(pg, a, 5),
                              svadd_u32_x(pg, f, svadd_u32_x(pg, e,
                              svadd_u32_x(pg, k, svld1_u32(pg, W[t])))));
            e = d;
            d = c;
            c = SVE_ROTL32(pg, b, 30);
            b = a;
            a = tmp;
        }
        a = svadd_u32_x(pg, a, a0);
        b = svadd_u32_x(pg, b, b0);
        c = svadd_u32_x(pg, c, c0);
        d = svadd_u32_x(pg, d, d0);
        e = svadd_u32_x(pg, e, e0);
    }

    svst1_u32(pg, out[0], a);
    svst1_u32(pg, out[1], b);
    svst1_u32(pg, out[2], c);
    svst1_u32(pg, out[3], d);
    svst1_u32(pg, out[4], e);
}

#endif /* SVE_SHA_KERNELS_H */
