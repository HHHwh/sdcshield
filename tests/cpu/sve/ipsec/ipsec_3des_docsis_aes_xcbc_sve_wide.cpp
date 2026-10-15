/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ipsec_3des_docsis_aes_xcbc_sve_wide
 * @parblock
 * This test encrypts random data using 3DES-DOCSIS encryption and computes
 * an AES-XCBC-96 message authentication code (12-byte truncated AES-XCBC-MAC
 * per RFC 3566) on the ciphertext via OpenSSL AES-ECB primitives using the
 * AVX-512 optimized code path. It also decrypts the ciphertext and verifies 
 * that it matches the original plaintext. Both the ciphertext, MAC, and 
 * decrypted data are compared against pre-computed golden values on every 
 * iteration to detect silent data corruption. Iterations alternate between
 * a re-rolled input with a same-core recomputed golden and a frozen replay
 * of the init-time inputs compared byte-exact against the init thread's
 * cross-core golden, adding coverage for deterministic (mercurial-core)
 * defects.
 * @endparblock
 */

#include "sandstone.h"

#if SANDSTONE_SSL_BUILD
#include "sandstone_ssl.h"
#include "sdc_stress_config.h"
#include <string.h>

#define DES3_KEY_SIZE          (24)
#define DES3_IV_SIZE           (8)
#define AES_KEY_SIZE           (16)
#define XCBC96_DIGEST_SIZE     (12)
#define XCBC_FULL_DIGEST_SIZE  (16)

struct xcbc_avx512_data {
    uint8_t des3_key[DES3_KEY_SIZE];
    uint8_t des3_iv[DES3_IV_SIZE];
    uint8_t aes_key[AES_KEY_SIZE];
    uint8_t *plaintext;
    uint8_t *golden_ciphertext;
    uint8_t golden_mac[XCBC96_DIGEST_SIZE];
    size_t data_size;
};

static size_t ipsec_data_size(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "data_size", 0, 1024, 16 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("ipsec_3des_docsis_aes_xcbc_sve_wide", "data_size", &v, 1024, 16 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count(1, 4, 1.5, 600, thread_count(),
                                         1024, 16 << 20, test->id, "data_size");
}

static void aes_ecb_encrypt(const uint8_t *key, const uint8_t *in, uint8_t *out) {
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_EncryptInit_ex(ctx, s_EVP_aes_128_ecb(), NULL, key, NULL);
    s_EVP_CIPHER_CTX_set_padding(ctx, 0);
    int outlen;
    s_EVP_EncryptUpdate(ctx, out, &outlen, in, 16);
    int outlen2;
    s_EVP_EncryptFinal_ex(ctx, out + outlen, &outlen2);
    s_EVP_CIPHER_CTX_free(ctx);
}

static void des3_encrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t len) {
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_EncryptInit_ex(ctx, s_EVP_des_ede3_cbc(), NULL, key, iv);
    s_EVP_CIPHER_CTX_set_padding(ctx, 0);
    int outlen;
    s_EVP_EncryptUpdate(ctx, out, &outlen, in, len);
    int outlen2;
    s_EVP_EncryptFinal_ex(ctx, out + outlen, &outlen2);
    s_EVP_CIPHER_CTX_free(ctx);
}

static void des3_decrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t len) {
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_DecryptInit_ex(ctx, s_EVP_des_ede3_cbc(), NULL, key, iv);
    s_EVP_CIPHER_CTX_set_padding(ctx, 0);
    int outlen;
    s_EVP_DecryptUpdate(ctx, out, &outlen, in, len);
    int outlen2;
    s_EVP_DecryptFinal_ex(ctx, out + outlen, &outlen2);
    s_EVP_CIPHER_CTX_free(ctx);
}

static void aes_xcbc_96(const uint8_t *key, const uint8_t *msg, size_t len, uint8_t *out) {
    uint8_t K1[16], K2[16], K3[16];
    uint8_t const1[16] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
    uint8_t const2[16] = {2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2};
    uint8_t const3[16] = {3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3};

    aes_ecb_encrypt(key, const1, K1);
    aes_ecb_encrypt(key, const2, K2);
    aes_ecb_encrypt(key, const3, K3);

    size_t n_blocks = (len + 15) / 16;
    uint8_t E[16] = {0};
    uint8_t M[16];

    if (n_blocks == 0) {
        memset(M, 0, 16);
        M[0] = 0x80;
        for (int i = 0; i < 16; i++) M[i] ^= K2[i];
        aes_ecb_encrypt(K1, M, out);
        return;
    }

    for (size_t i = 0; i < n_blocks - 1; i++) {
        memcpy(M, msg + i * 16, 16);
        for (int j = 0; j < 16; j++) M[j] ^= E[j];
        aes_ecb_encrypt(K1, M, E);
    }

    size_t rem = len % 16;
    if (rem == 0 && len > 0) {
        memcpy(M, msg + (n_blocks - 1) * 16, 16);
        for (int j = 0; j < 16; j++) M[j] ^= E[j] ^ K2[j];
    } else {
        memset(M, 0, 16);
        memcpy(M, msg + (n_blocks - 1) * 16, rem);
        M[rem] = 0x80;
        for (int j = 0; j < 16; j++) M[j] ^= E[j] ^ K3[j];
    }
    aes_ecb_encrypt(K1, M, out);
}

static void xcbc_avx512_compute_golden(struct xcbc_avx512_data *d) {
    des3_encrypt(d->des3_key, d->des3_iv, d->plaintext, d->golden_ciphertext, d->data_size);
    uint8_t full_mac[XCBC_FULL_DIGEST_SIZE];
    aes_xcbc_96(d->aes_key, d->golden_ciphertext, d->data_size, full_mac);
    memcpy(d->golden_mac, full_mac, XCBC96_DIGEST_SIZE);
}

static int xcbc_avx512_init(struct test *test) {
    if (s_EVP_CIPHER_CTX_new && s_EVP_EncryptInit_ex && s_EVP_DecryptInit_ex && s_EVP_des_ede3_cbc && s_EVP_aes_128_ecb) {
        struct xcbc_avx512_data *d = (struct xcbc_avx512_data *)malloc(sizeof(*d));
        if (!d) return EXIT_SKIP;
        /* 工作集字节数: -O ipsec_3des_docsis_aes_xcbc_sve_wide.data_size=N /
         * SDC_IPSEC_3DES_DOCSIS_AES_XCBC_SVE_WIDE_DATA_SIZE 环境变量 /
         * 自适应: 每线程明文+密文+golden 密文+解密缓冲 ~= 4x, 占物理内存
         * 60%, clamp [1024, 16M]。线性缓冲用 sdc_auto_linear_count（sqrt 缩放会欠缩放压力）。历史硬编码 1024。3DES-CBC 无填充场景
         * 尺寸必须对齐到分组大小, 统一 16B 对齐 (同时满足 3DES 8B),
         * 对齐后 < 1024 取 1024。 */
        d->data_size = ipsec_data_size(test);
        d->data_size &= ~(size_t)15;
        if (d->data_size < 1024)
            d->data_size = 1024;
        d->plaintext = (uint8_t *)malloc(d->data_size);
        d->golden_ciphertext = (uint8_t *)malloc(d->data_size);
        if (!d->plaintext || !d->golden_ciphertext) {
            free(d->plaintext);
            free(d->golden_ciphertext);
            free(d);
            return EXIT_SKIP;
        }

        memset_random(d->des3_key, DES3_KEY_SIZE);
        memset_random(d->des3_iv, DES3_IV_SIZE);
        memset_random(d->aes_key, AES_KEY_SIZE);
        memset_random(d->plaintext, d->data_size);

        xcbc_avx512_compute_golden(d);

        test->data = d;
        return EXIT_SUCCESS;
    } else {
        log_skip(TestResourceIssueSkipCategory, "OpenSSL library is not available or the current version is not supported");
        return EXIT_SKIP;
    }
}

static int xcbc_avx512_run(struct test *test, int cpu) {
    struct xcbc_avx512_data *d = (struct xcbc_avx512_data *)test->data;

    uint8_t *ciphertext = (uint8_t *)malloc(d->data_size);
    uint8_t *decrypted = (uint8_t *)malloc(d->data_size);
    uint8_t *full_mac = (uint8_t *)malloc(XCBC_FULL_DIGEST_SIZE);

    /* R2b (B2-003): per-thread plaintext + golden, re-rolled every
     * iteration (thread-safe: shared d is only read after init; the
     * struct copy puts plaintext and the golden fields written by
     * compute_golden — golden_ciphertext/golden_mac — on this
     * thread's stack; keys/IV ride along as identical values). */
    struct xcbc_avx512_data local_d;
    memcpy(&local_d, d, sizeof(local_d));
    long iter = 0;
    TEST_LOOP(test, 256) {
        if (iter & 1) {
            /* frozen replay round (PR #244 review): init's shared
             * cross-core golden — the reroll path recomputes golden
             * with the same code on the same core, which cannot
             * catch deterministic (mercurial-core) defects; every
             * other round replays init's frozen inputs and compares
             * byte-exact against init's golden computed on a
             * different core. */
            des3_encrypt(d->des3_key, d->des3_iv, d->plaintext, ciphertext, d->data_size);
            memcmp_or_fail(ciphertext, d->golden_ciphertext, d->data_size, "3DES-DOCSIS ciphertext mismatch (frozen replay)");

            des3_decrypt(d->des3_key, d->des3_iv, ciphertext, decrypted, d->data_size);
            memcmp_or_fail(decrypted, d->plaintext, d->data_size, "3DES-DOCSIS decryption mismatch (frozen replay)");

            aes_xcbc_96(d->aes_key, ciphertext, d->data_size, full_mac);
            memcmp_or_fail(full_mac, d->golden_mac, XCBC96_DIGEST_SIZE, "AES-XCBC-96 digest mismatch (frozen replay)");
        } else {
            /* re-roll: fresh plaintext + same-iteration golden (EVP encrypt +
             * XCBC-96 recomputed for this thread's data) */
            memset_random(local_d.plaintext, d->data_size);
            xcbc_avx512_compute_golden(&local_d);
            des3_encrypt(local_d.des3_key, local_d.des3_iv, local_d.plaintext, ciphertext, d->data_size);
            memcmp_or_fail(ciphertext, local_d.golden_ciphertext, d->data_size, "3DES-DOCSIS ciphertext mismatch (AVX-512)");

            des3_decrypt(local_d.des3_key, local_d.des3_iv, ciphertext, decrypted, d->data_size);
            memcmp_or_fail(decrypted, local_d.plaintext, d->data_size, "3DES-DOCSIS decryption mismatch (AVX-512)");

            aes_xcbc_96(local_d.aes_key, ciphertext, d->data_size, full_mac);
            memcmp_or_fail(full_mac, local_d.golden_mac, XCBC96_DIGEST_SIZE, "AES-XCBC-96 digest mismatch (AVX-512)");
        }
        ++iter;
    }

    free(ciphertext);
    free(decrypted);
    free(full_mac);
    return EXIT_SUCCESS;
}

static int xcbc_avx512_cleanup(struct test *test) {
    struct xcbc_avx512_data *d = (struct xcbc_avx512_data *)test->data;
    if (d) {
        free(d->plaintext);
        free(d->golden_ciphertext);
    }
    free(d);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ipsec_3des_docsis_aes_xcbc_sve_wide, "Intel IPSEC SVE port: 3DES-DOCSIS w/ AES-XCBC-96 (compute path: OpenSSL EVP; port of the avx-named original)")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec), 
    .test_init = xcbc_avx512_init,
    .test_run = xcbc_avx512_run,
    .test_cleanup = xcbc_avx512_cleanup,
    .minimum_cpu = IPSEC_X86_GATE(cpu_skylake_avx512),
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#else

static int xcbc_avx512_init(struct test *test) {
    log_skip(OsNotSupportedSkipCategory, "OpenSSL build is not enabled");
    return EXIT_SKIP;
}

static int xcbc_avx512_run(struct test *test, int cpu) {
    __builtin_unreachable();
}

DECLARE_TEST(ipsec_3des_docsis_aes_xcbc_sve_wide, "Intel IPSEC SVE port: 3DES-DOCSIS w/ AES-XCBC-96 (compute path: OpenSSL EVP; port of the avx-named original)")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),     
    .test_init = xcbc_avx512_init,
    .test_run = xcbc_avx512_run,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif
