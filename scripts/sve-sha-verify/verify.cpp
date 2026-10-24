/**
 * Standalone verifier: SVE multi-stream SHA kernels vs OpenSSL EVP digests.
 * Part of the avx53 SVE port plan Task 5 correctness gate — the kernels
 * must match OpenSSL bit-for-bit on a large vector set before being used
 * as ipsec SVE test golden bases.
 *
 * All-VL Phase 2 P4: the stream count is the runtime lane count
 * (svcntw()/svcntd(), one message per lane, 128..1024-bit VL), matching
 * the parameterized kernels — the vector-set size per round scales with
 * the host VL (5200 vectors at VL=256, more at wider VL).
 *
 * Build: g++ -O2 -march=armv8.2-a+sve -I<openssl install include> \
 *            this_file.c -o verifier -L<lib> -l:libcrypto.a
 */
#include "../../tests/cpu/sve/ipsec/sve_sha_kernels.h"
#include <openssl/evp.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static unsigned g_seed = 0x12345678u;
static unsigned lcg(void) { return g_seed = g_seed * 1103515245u + 12345u; }

/* SHA-256 padding for a message of len bytes (len < 2^32/8 bytes), total
 * padded length multiple of 64. Returns padded buffer, sets *padded_len. */
static std::vector<uint8_t> pad_sha256(const uint8_t *msg, size_t len) {
    size_t total = ((len + 9 + 63) / 64) * 64;
    std::vector<uint8_t> p(msg, msg + len);
    p.resize(total, 0);
    p[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i)
        p[total - 1 - i] = (uint8_t)(bits >> (8 * i));
    return p;
}

static std::vector<uint8_t> pad_sha512(const uint8_t *msg, size_t len) {
    size_t total = ((len + 17 + 127) / 128) * 128;
    std::vector<uint8_t> p(msg, msg + len);
    p.resize(total, 0);
    p[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;   /* 测试消息 < 2^32 字节, 高 64 位为 0 */
    for (int i = 0; i < 8; ++i) p[total - 8 + i] = 0;
    for (int i = 0; i < 8; ++i) p[total - 1 - i] = (uint8_t)(bits >> (8 * i));
    return p;
}

int main() {
    int fails = 0, total = 0;

    /* All-VL gate (adversarial review Important-1): arrays are sized to
       the 1024-bit cap; a wider runtime VL would overflow them — mirror
       the gem5 kernel's exit-3 guard. */
    if ((int)svcntw() < 4 || (int)svcntw() > SVE_SHA_STREAMS_MAX_32 ||
        (int)svcntd() < 2 || (int)svcntd() > SVE_SHA_STREAMS_MAX_64) {
        fprintf(stderr, "unsupported VL (svcntw=%d svcntd=%d; arrays "
                        "sized for 128..1024-bit)\n",
                (int)svcntw(), (int)svcntd());
        return 3;
    }

    /* ---------- SHA-256 (streams = svcntw()) ---------- */
    for (int round = 0; round < 200; ++round) {
        const int n = (int)svcntw();              /* all-VL: runtime lane count */
        size_t len = lcg() % 512;                 /* 0..511 字节 */
        size_t nblocks = ((len + 9 + 63) / 64);
        uint8_t msgs[SVE_SHA_STREAMS_MAX_32][512];
        for (int s = 0; s < n; ++s)
            for (size_t i = 0; i < len; ++i)
                msgs[s][i] = (uint8_t)lcg();

        const uint8_t *mp[SVE_SHA_STREAMS_MAX_32];
        std::vector<uint8_t> padded[SVE_SHA_STREAMS_MAX_32];
        for (int s = 0; s < n; ++s) {
            padded[s] = pad_sha256(msgs[s], len);
            mp[s] = padded[s].data();
        }
        uint32_t st[8][SVE_SHA_STREAMS_MAX_32];
        sha256_compress_x8(st, mp, (uint32_t)nblocks, sha256_iv);

        for (int s = 0; s < n; ++s) {
            uint8_t digest[32];
            unsigned dlen = 0;
            EVP_Digest(msgs[s], len, digest, &dlen, EVP_sha256(), nullptr);
            uint8_t mine[32];
            for (int w = 0; w < 8; ++w) {
                mine[w*4+0] = (uint8_t)(st[w][s] >> 24);
                mine[w*4+1] = (uint8_t)(st[w][s] >> 16);
                mine[w*4+2] = (uint8_t)(st[w][s] >> 8);
                mine[w*4+3] = (uint8_t)(st[w][s]);
            }
            ++total;
            if (memcmp(digest, mine, 32) != 0) {
                ++fails;
                if (fails <= 3)
                    printf("SHA256 mismatch: round=%d stream=%d len=%zu\n", round, s, len);
            }
        }
    }

    /* ---------- SHA-224 (IV 不同 + 截断 7 字; streams = svcntw()) ---------- */
    for (int round = 0; round < 100; ++round) {
        const int n = (int)svcntw();
        size_t len = lcg() % 256;
        size_t nblocks = ((len + 9 + 63) / 64);
        uint8_t msgs[SVE_SHA_STREAMS_MAX_32][256];
        for (int s = 0; s < n; ++s)
            for (size_t i = 0; i < len; ++i)
                msgs[s][i] = (uint8_t)lcg();
        const uint8_t *mp[SVE_SHA_STREAMS_MAX_32];
        std::vector<uint8_t> padded[SVE_SHA_STREAMS_MAX_32];
        for (int s = 0; s < n; ++s) {
            padded[s] = pad_sha256(msgs[s], len);
            mp[s] = padded[s].data();
        }
        uint32_t st[8][SVE_SHA_STREAMS_MAX_32];
        sha256_compress_x8(st, mp, (uint32_t)nblocks, sha224_iv);
        for (int s = 0; s < n; ++s) {
            uint8_t digest[28];
            unsigned dlen = 0;
            EVP_Digest(msgs[s], len, digest, &dlen, EVP_sha224(), nullptr);
            uint8_t mine[28];
            for (int w = 0; w < 7; ++w) {
                mine[w*4+0] = (uint8_t)(st[w][s] >> 24);
                mine[w*4+1] = (uint8_t)(st[w][s] >> 16);
                mine[w*4+2] = (uint8_t)(st[w][s] >> 8);
                mine[w*4+3] = (uint8_t)(st[w][s]);
            }
            ++total;
            if (memcmp(digest, mine, 28) != 0) { ++fails;
                if (fails <= 3) printf("SHA224 mismatch: round=%d stream=%d len=%zu\n", round, s, len); }
        }
    }

    /* ---------- SHA-512 (streams = svcntd()) ---------- */
    for (int round = 0; round < 200; ++round) {
        const int n = (int)svcntd();
        size_t len = lcg() % 1024;
        size_t nblocks = ((len + 17 + 127) / 128);
        uint8_t msgs[SVE_SHA_STREAMS_MAX_64][1024];
        for (int s = 0; s < n; ++s)
            for (size_t i = 0; i < len; ++i)
                msgs[s][i] = (uint8_t)lcg();
        const uint8_t *mp[SVE_SHA_STREAMS_MAX_64];
        std::vector<uint8_t> padded[SVE_SHA_STREAMS_MAX_64];
        for (int s = 0; s < n; ++s) {
            padded[s] = pad_sha512(msgs[s], len);
            mp[s] = padded[s].data();
        }
        uint64_t st[8][SVE_SHA_STREAMS_MAX_64];
        sha512_compress_x4(st, mp, (uint32_t)nblocks, sha512_iv);
        for (int s = 0; s < n; ++s) {
            uint8_t digest[64];
            unsigned dlen = 0;
            EVP_Digest(msgs[s], len, digest, &dlen, EVP_sha512(), nullptr);
            uint8_t mine[64];
            for (int w = 0; w < 8; ++w)
                for (int k = 0; k < 8; ++k)
                    mine[w*8+k] = (uint8_t)(st[w][s] >> (56 - 8*k));
            ++total;
            if (memcmp(digest, mine, 64) != 0) { ++fails;
                if (fails <= 3) printf("SHA512 mismatch: round=%d stream=%d len=%zu\n", round, s, len); }
        }
    }

    /* ---------- SHA-384 (streams = svcntd()) ---------- */
    for (int round = 0; round < 100; ++round) {
        const int n = (int)svcntd();
        size_t len = lcg() % 512;
        size_t nblocks = ((len + 17 + 127) / 128);
        uint8_t msgs[SVE_SHA_STREAMS_MAX_64][512];
        for (int s = 0; s < n; ++s)
            for (size_t i = 0; i < len; ++i)
                msgs[s][i] = (uint8_t)lcg();
        const uint8_t *mp[SVE_SHA_STREAMS_MAX_64];
        std::vector<uint8_t> padded[SVE_SHA_STREAMS_MAX_64];
        for (int s = 0; s < n; ++s) {
            padded[s] = pad_sha512(msgs[s], len);
            mp[s] = padded[s].data();
        }
        uint64_t st[8][SVE_SHA_STREAMS_MAX_64];
        sha512_compress_x4(st, mp, (uint32_t)nblocks, sha384_iv);
        for (int s = 0; s < n; ++s) {
            uint8_t digest[48];
            unsigned dlen = 0;
            EVP_Digest(msgs[s], len, digest, &dlen, EVP_sha384(), nullptr);
            uint8_t mine[48];
            for (int w = 0; w < 6; ++w)
                for (int k = 0; k < 8; ++k)
                    mine[w*8+k] = (uint8_t)(st[w][s] >> (56 - 8*k));
            ++total;
            if (memcmp(digest, mine, 48) != 0) { ++fails;
                if (fails <= 3) printf("SHA384 mismatch: round=%d stream=%d len=%zu\n", round, s, len); }
        }
    }

    /* ---------- SHA-1 (streams = svcntw()) ---------- */
    for (int round = 0; round < 200; ++round) {
        const int n = (int)svcntw();
        size_t len = lcg() % 512;
        size_t nblocks = ((len + 9 + 63) / 64);
        uint8_t msgs[SVE_SHA_STREAMS_MAX_32][512];
        for (int s = 0; s < n; ++s)
            for (size_t i = 0; i < len; ++i)
                msgs[s][i] = (uint8_t)lcg();
        const uint8_t *mp[SVE_SHA_STREAMS_MAX_32];
        std::vector<uint8_t> padded[SVE_SHA_STREAMS_MAX_32];
        for (int s = 0; s < n; ++s) {
            padded[s] = pad_sha256(msgs[s], len);
            mp[s] = padded[s].data();
        }
        uint32_t st[5][SVE_SHA_STREAMS_MAX_32];
        sha1_compress_x8(st, mp, (uint32_t)nblocks);
        for (int s = 0; s < n; ++s) {
            uint8_t digest[20];
            unsigned dlen = 0;
            EVP_Digest(msgs[s], len, digest, &dlen, EVP_sha1(), nullptr);
            uint8_t mine[20];
            for (int w = 0; w < 5; ++w) {
                mine[w*4+0] = (uint8_t)(st[w][s] >> 24);
                mine[w*4+1] = (uint8_t)(st[w][s] >> 16);
                mine[w*4+2] = (uint8_t)(st[w][s] >> 8);
                mine[w*4+3] = (uint8_t)(st[w][s]);
            }
            ++total;
            if (memcmp(digest, mine, 20) != 0) { ++fails;
                if (fails <= 3) printf("SHA1 mismatch: round=%d stream=%d len=%zu\n", round, s, len); }
        }
    }

    printf("total=%d fails=%d (svcntw=%zu svcntd=%zu) → %s\n", total, fails,
           (size_t)svcntw(), (size_t)svcntd(),
           fails == 0 ? "ALL MATCH ✓ (kernels verified against OpenSSL)" : "MISMATCH ✗");
    return fails != 0;
}
