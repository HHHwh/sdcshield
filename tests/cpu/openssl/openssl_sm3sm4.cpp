/**
 * @file
 *
 * @copyright
 * Copyright 2026 ISCAS.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b openssl_sm3sm4
 * @parblock
 * Stress test for the Chinese national cryptographic standards SM3
 * (GB/T 32905-2016 digest) and SM4 (GB/T 32907-2016 block cipher) via
 * OpenSSL's EVP API. SM3 is a Merkle-Damgard hash like SHA-2 but with a
 * different integer mixing structure (boolean function rounds + cyclic
 * rotates + a message-expansion permutation distinct from SHA-2's
 * mod-2^64 sigma sums), so it exercises the ALU/rotate pipes in a phase
 * the SHA-2 (additive chains) and SHA-3 (AND-XOR-rotate sponge)
 * companion tests do not. SM4 is a 32-round Feistel-like block cipher
 * with its own 8-bit S-box, linear L-transform and CK round constants —
 * a S-box/round-function structure orthogonal to AES's affine S-box +
 * ShiftRows/MixColumns. Two new integer-path phases, one test. Pairing
 * the national-crypto (国密) algorithms with a domestic ARM silicon
 * validation tool is a natural coverage fit. OpenSSL 3.5 has dedicated
 * optimized SM3/SM4 implementations. Each iteration re-encrypts the
 * plaintext with SM4-CBC (no padding: 1024 bytes = 64 SM4 blocks, so
 * ciphertext length is fixed at 1024) and byte-compares against the
 * golden ciphertext, decrypts the golden ciphertext and verifies the
 * roundtrip against the plaintext, and recomputes the SM3 digest of the
 * plaintext and byte-compares it too (compute->verify on both the cipher
 * and digest datapaths).
 * @endparblock
 */

#include "sandstone.h"

#if SANDSTONE_SSL_BUILD

#include "sandstone_ssl.h"
#include "sdc_stress_config.h"

#include <string.h>
#include <stdlib.h>

#define SM3_DIGEST_SIZE (32)
#define SM4_KEY_SIZE  (16)
#define SM4_IV_SIZE   (16)
#define SM4_BLOCK     (16)

/* 数据字节 — 统一配置链:
 *   1. -O openssl_sm3sm4.data_size=N / 配置文件同名条目
 *   2. 环境变量 SDC_OPENSSL_SM3SM4_DATA_SIZE
 *   3. 自适应: 每线程 plaintext+ciphertext+golden+decrypted ~= 4x,
 *      占物理内存 60%, clamp [1024, 16M]。线性缓冲用
 *      sdc_auto_linear_count（sqrt 缩放会欠缩放压力）。
 * SM4-CBC 无填充要求 16B 对齐: 显式/自适应值均向下对齐到 16,
 * 对齐后 <1024 取 1024。历史硬编码 1024 (= 64 SM4 块)。 */
static size_t sm_data_size(struct test *test)
{
    /* hi 动态: 大内存机器按 60% 预算放开, 小内存机器保持 16M 收口 */
    const int64_t hi = sdc_linear_hi(1, 4, 1.5, 600, thread_count(), 16 << 20);
    int64_t v = sdc_knob_int_clamped(test, "data_size", 0, 1024, hi);
    if (v == 0)
        sdc_env_knob_int_clamped("openssl_sm3sm4", "data_size", &v, 1024, hi);
    if (v > 0)
        v &= ~(int64_t)(SM4_BLOCK - 1);
    else
        v = sdc_auto_linear_count(1, 4, 1.5, 600, thread_count(),
                                  1024, hi, test->id, "data_size")
            & ~(int64_t)(SM4_BLOCK - 1);
    if (v < 1024)
        v = 1024;
    return (size_t)v;
}

struct sm_test_data {
    uint8_t sm4_key[SM4_KEY_SIZE], sm4_iv[SM4_IV_SIZE];
    uint8_t *plaintext;
    uint8_t *golden_ciphertext;           /* SM4-CBC no padding: N in = N out */
    size_t data_size;
    uint8_t golden_sm3[SM3_DIGEST_SIZE];  /* SM3 digest of plaintext */
};

static void sm4_cbc_encrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *in,
                            uint8_t *out, size_t len, int *total_outlen)
{
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_EncryptInit_ex(ctx, s_EVP_sm4_cbc(), NULL, key, iv);
    s_EVP_CIPHER_CTX_set_padding(ctx, 0);
    int outlen = 0;
    int outlen2 = 0;
    s_EVP_EncryptUpdate(ctx, out, &outlen, in, len);
    s_EVP_EncryptFinal_ex(ctx, out + outlen, &outlen2);
    s_EVP_CIPHER_CTX_free(ctx);
    *total_outlen = outlen + outlen2;
}

static void sm4_cbc_decrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *in,
                            uint8_t *out, size_t len, int *total_outlen)
{
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_DecryptInit_ex(ctx, s_EVP_sm4_cbc(), NULL, key, iv);
    s_EVP_CIPHER_CTX_set_padding(ctx, 0);
    int outlen = 0;
    int outlen2 = 0;
    s_EVP_DecryptUpdate(ctx, out, &outlen, in, len);
    s_EVP_DecryptFinal_ex(ctx, out + outlen, &outlen2);
    s_EVP_CIPHER_CTX_free(ctx);
    *total_outlen = outlen + outlen2;
}

static void sm3_digest(const uint8_t *msg, size_t len, uint8_t out[SM3_DIGEST_SIZE])
{
    EVP_MD_CTX *mdctx = s_EVP_MD_CTX_new();
    unsigned int md_len = 0;
    s_EVP_DigestInit_ex(mdctx, s_EVP_sm3(), NULL);
    s_EVP_DigestUpdate(mdctx, msg, len);
    s_EVP_DigestFinal_ex(mdctx, out, &md_len);
    s_EVP_MD_CTX_free(mdctx);
}

static int ssl_sm3sm4_init(struct test *test)
{
    if (s_EVP_sm3 && s_EVP_sm4_cbc && s_EVP_CIPHER_CTX_new && s_EVP_CIPHER_CTX_free
            && s_EVP_EncryptInit_ex && s_EVP_EncryptUpdate && s_EVP_EncryptFinal_ex
            && s_EVP_DecryptInit_ex && s_EVP_DecryptUpdate && s_EVP_DecryptFinal_ex
            && s_EVP_CIPHER_CTX_set_padding && s_EVP_MD_CTX_new && s_EVP_MD_CTX_free
            && s_EVP_DigestInit_ex && s_EVP_DigestUpdate && s_EVP_DigestFinal_ex) {
        struct sm_test_data *d = (struct sm_test_data *)malloc(sizeof(*d));
        if (!d)
            return EXIT_SKIP;

        d->data_size = sm_data_size(test);
        d->plaintext = (uint8_t *)malloc(d->data_size);
        d->golden_ciphertext = (uint8_t *)malloc(d->data_size);
        if (!d->plaintext || !d->golden_ciphertext) {
            free(d->plaintext);
            free(d->golden_ciphertext);
            free(d);
            return EXIT_SKIP;
        }

        memset_random(d->sm4_key, SM4_KEY_SIZE);
        memset_random(d->sm4_iv, SM4_IV_SIZE);
        memset_random(d->plaintext, d->data_size);

        /* Golden SM4-CBC ciphertext, padding disabled: data_size is 16B
         * aligned (see sm_data_size), an exact multiple of the 16-byte SM4
         * block, so the output must be exactly data_size bytes. Any other
         * length means padding kicked in — a surprise that would silently
         * shift the golden comparison. */
        int enc_len = 0;
        sm4_cbc_encrypt(d->sm4_key, d->sm4_iv, d->plaintext, d->golden_ciphertext,
                        d->data_size, &enc_len);
        if (enc_len != (int)d->data_size) {
            log_error("SM4-CBC encryption produced %d bytes, expected %zu (padding surprise)",
                      enc_len, d->data_size);
            free(d->plaintext);
            free(d->golden_ciphertext);
            free(d);
            return EXIT_FAILURE;
        }

        /* Golden SM3 digest of the plaintext */
        sm3_digest(d->plaintext, d->data_size, d->golden_sm3);

        test->data = d;
        return EXIT_SUCCESS;
    } else {
        log_skip(TestResourceIssueSkipCategory, "OpenSSL library is not available or the current version is not supported");
        return EXIT_SKIP;
    }
}

static int ssl_sm3sm4_run(struct test *test, int cpu)
{
    struct sm_test_data *d = (struct sm_test_data *)test->data;

    /* randomization hardening H7': per-thread copy of the (small) struct —
     * the shared d is only read; plaintext and goldens live in this
     * thread's copy and are re-rolled every iteration.  plaintext/
     * golden_ciphertext 是堆指针: 整结构体浅拷贝后必须换成线程私有
     * 缓冲, 否则 127 线程共享写同一块内存 = 假失败。 */
    struct sm_test_data local_d;
    memcpy(&local_d, d, sizeof(local_d));   /* key/iv/data_size/golden_sm3 */
    local_d.plaintext = (uint8_t *)malloc(local_d.data_size);
    local_d.golden_ciphertext = (uint8_t *)malloc(local_d.data_size);
    if (!local_d.plaintext || !local_d.golden_ciphertext) {
        free(local_d.plaintext);
        free(local_d.golden_ciphertext);
        return EXIT_SKIP;
    }
    const size_t SM_DATA_SIZE = local_d.data_size;

    /* Per-thread work buffers, allocated once outside the loop (no
     * offsets needed, the cipher/digest work itself is the stress, same
     * as the ipsec EVP tests). */
    uint8_t *ciphertext = (uint8_t *)malloc(SM_DATA_SIZE);
    uint8_t *decrypted = (uint8_t *)malloc(SM_DATA_SIZE);
    uint8_t *digest = (uint8_t *)malloc(SM3_DIGEST_SIZE);

    int outlen = 0;

    TEST_LOOP(test, 256) {
        /* Re-roll: fresh plaintext + same-iteration golden through the
         * same EVP path for this thread's data. */
        memset_random(local_d.plaintext, SM_DATA_SIZE);
        int g_len = 0;
        sm4_cbc_encrypt(local_d.sm4_key, local_d.sm4_iv, local_d.plaintext,
                        local_d.golden_ciphertext, SM_DATA_SIZE, &g_len);
        if (g_len != (int)SM_DATA_SIZE)
            report_fail_msg("golden SM4-CBC encryption produced %d bytes, expected %zu (padding surprise)",
                            g_len, SM_DATA_SIZE);
        sm3_digest(local_d.plaintext, SM_DATA_SIZE, local_d.golden_sm3);

        /* 1. Re-encrypt the plaintext and compare against golden */
        sm4_cbc_encrypt(local_d.sm4_key, local_d.sm4_iv, local_d.plaintext, ciphertext,
                        SM_DATA_SIZE, &outlen);
        if (outlen != (int)SM_DATA_SIZE)
            report_fail_msg("SM4-CBC encryption produced %d bytes, expected %zu (padding surprise)",
                            outlen, SM_DATA_SIZE);
        memcmp_or_fail(ciphertext, local_d.golden_ciphertext, SM_DATA_SIZE,
                       "sm4-cbc ciphertext mismatch");

        /* 2. Decrypt the golden ciphertext, verify roundtrip */
        sm4_cbc_decrypt(local_d.sm4_key, local_d.sm4_iv, local_d.golden_ciphertext, decrypted,
                        SM_DATA_SIZE, &outlen);
        if (outlen != (int)SM_DATA_SIZE)
            report_fail_msg("SM4-CBC decryption produced %d bytes, expected %zu (padding surprise)",
                            outlen, SM_DATA_SIZE);
        memcmp_or_fail(decrypted, local_d.plaintext, SM_DATA_SIZE,
                       "sm4-cbc roundtrip mismatch");

        /* 3. Recompute the SM3 digest and compare against golden */
        sm3_digest(local_d.plaintext, SM_DATA_SIZE, digest);
        memcmp_or_fail(digest, local_d.golden_sm3, SM3_DIGEST_SIZE,
                       "sm3 digest mismatch");
    }


    free(local_d.plaintext);
    free(local_d.golden_ciphertext);
    free(ciphertext);
    free(decrypted);
    free(digest);
    return EXIT_SUCCESS;
}

static int ssl_sm3sm4_cleanup(struct test *test)
{
    struct sm_test_data *d = (struct sm_test_data *)test->data;
    if (d) {
        free(d->plaintext);
        free(d->golden_ciphertext);
        free(d);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(openssl_sm3sm4, "Test SM3 (Chinese national digest) and SM4 (Chinese national block cipher)")
    .test_init = ssl_sm3sm4_init,
    .test_run = ssl_sm3sm4_run,
    .test_cleanup = ssl_sm3sm4_cleanup,
    .fracture_loop_count = 4,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#else // !SANDSTONE_SSL_BUILD

static int ssl_sm3sm4_init(struct test *test)
{
    log_skip(OsNotSupportedSkipCategory, "Not supported on this OS");
    return EXIT_SKIP;
}

static int ssl_sm3sm4_run(struct test *test, int cpu)
{
    __builtin_unreachable();
}

DECLARE_TEST(openssl_sm3sm4, "Test SM3 (Chinese national digest) and SM4 (Chinese national block cipher)")
    .test_init = ssl_sm3sm4_init,
    .test_run = ssl_sm3sm4_run,
    .fracture_loop_count = 4,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif
