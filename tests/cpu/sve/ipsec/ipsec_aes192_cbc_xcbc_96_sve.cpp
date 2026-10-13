/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ipsec_aes192_cbc_xcbc_96_sve
 * @parblock
 * This test encrypts random data using AES-192-CBC encryption and computes
 * an AES-XCBC-96 message authentication code (truncated to 12 bytes) on the
 * ciphertext via OpenSSL AES-ECB primitives using the AVX optimized code path.
 * It also decrypts the ciphertext and verifies that it matches the original
 * plaintext. The ciphertext, MAC, and decrypted data are compared against
 * pre-computed golden values on every iteration to detect silent data corruption.
 * @endparblock
 */

#include "sandstone.h"

#if SANDSTONE_SSL_BUILD

#include "sandstone_ssl.h"
#include <string.h>

#define DATA_SIZE (1024u)
#define AES_KEY_SIZE (24)
#define AES_IV_SIZE (16)
#define XCBC_KEY_SIZE (16)
#define XCBC_96_DIGEST_SIZE (12)
#define XCBC_FULL_DIGEST_SIZE (16)

struct aes_xcbc_avx_data {
    uint8_t aes_key[AES_KEY_SIZE];
    uint8_t aes_iv[AES_IV_SIZE];
    uint8_t xcbc_key[XCBC_KEY_SIZE];
    uint8_t plaintext[DATA_SIZE];
    uint8_t golden_ciphertext[DATA_SIZE];
    uint8_t golden_mac[XCBC_96_DIGEST_SIZE];
};

/* Helper function: AES-128-ECB encryption for XCBC */
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

/* Manual implementation of AES-XCBC-96 as defined in RFC 3566 */
static void aes_xcbc_96(const uint8_t *key, const uint8_t *msg, size_t len, uint8_t *out) {
    uint8_t K1[16], K2[16], K3[16];
    uint8_t const1[16] = {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1};
    uint8_t const2[16] = {2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2};
    uint8_t const3[16] = {3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3};

    /* Key Derivation */
    aes_ecb_encrypt(key, const1, K1);
    aes_ecb_encrypt(key, const2, K2);
    aes_ecb_encrypt(key, const3, K3);

    size_t n_blocks = (len + 15) / 16;
    uint8_t E[16] = {0};
    uint8_t M[16];

    /* Special case for empty message */
    if (n_blocks == 0) {
        memset(M, 0, 16);
        M[0] = 0x80;
        for (int i = 0; i < 16; i++) M[i] ^= K3[i];
        aes_ecb_encrypt(K1, M, out);
        return;
    }

    /* Process all blocks except the last one */
    for (size_t i = 0; i < n_blocks - 1; i++) {
        memcpy(M, msg + i * 16, 16);
        for (int j = 0; j < 16; j++) M[j] ^= E[j];
        aes_ecb_encrypt(K1, M, E);
    }

    /* Process the last block */
    size_t rem = len % 16;
    if (rem == 0 && len > 0) {
        /* Complete block: XOR with K2 */
        memcpy(M, msg + (n_blocks - 1) * 16, 16);
        for (int j = 0; j < 16; j++) M[j] ^= E[j] ^ K2[j];
    } else {
        /* Incomplete block: Pad with 10*, XOR with K3 */
        memset(M, 0, 16);
        memcpy(M, msg + (n_blocks - 1) * 16, rem);
        M[rem] = 0x80;
        for (int j = 0; j < 16; j++) M[j] ^= E[j] ^ K3[j];
    }

    aes_ecb_encrypt(K1, M, out);
}

static void aes_encrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t len) {
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_EncryptInit_ex(ctx, s_EVP_aes_192_cbc(), NULL, key, iv);
    s_EVP_CIPHER_CTX_set_padding(ctx, 0);
    int outlen;
    s_EVP_EncryptUpdate(ctx, out, &outlen, in, len);
    int outlen2;
    s_EVP_EncryptFinal_ex(ctx, out + outlen, &outlen2);
    s_EVP_CIPHER_CTX_free(ctx);
}

static void aes_decrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t len) {
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_DecryptInit_ex(ctx, s_EVP_aes_192_cbc(), NULL, key, iv);
    s_EVP_CIPHER_CTX_set_padding(ctx, 0);
    int outlen;
    s_EVP_DecryptUpdate(ctx, out, &outlen, in, len);
    int outlen2;
    s_EVP_DecryptFinal_ex(ctx, out + outlen, &outlen2);
    s_EVP_CIPHER_CTX_free(ctx);
}

static void aes_xcbc_avx_compute_golden(struct aes_xcbc_avx_data *d) {
    aes_encrypt(d->aes_key, d->aes_iv, d->plaintext, d->golden_ciphertext, DATA_SIZE);
    uint8_t full_mac[XCBC_FULL_DIGEST_SIZE];
    aes_xcbc_96(d->xcbc_key, d->golden_ciphertext, DATA_SIZE, full_mac);
    memcpy(d->golden_mac, full_mac, XCBC_96_DIGEST_SIZE);
}

static int aes_xcbc_avx_init(struct test *test) {
    if (s_EVP_CIPHER_CTX_new && s_EVP_EncryptInit_ex && s_EVP_DecryptInit_ex && 
        s_EVP_aes_192_cbc() && s_EVP_aes_128_ecb()) {
        struct aes_xcbc_avx_data *d = (struct aes_xcbc_avx_data *)malloc(sizeof(*d));
        if (!d) return EXIT_SKIP;
        memset_random(d->aes_key, AES_KEY_SIZE);
        memset_random(d->aes_iv, AES_IV_SIZE);
        memset_random(d->xcbc_key, XCBC_KEY_SIZE);
        memset_random(d->plaintext, DATA_SIZE);
        aes_xcbc_avx_compute_golden(d);
        test->data = d;
        return EXIT_SUCCESS;
    } else {
        log_skip(TestResourceIssueSkipCategory, "OpenSSL library is not available or the current version is not supported");
        return EXIT_SKIP;
    }
}

static int aes_xcbc_avx_run(struct test *test, int cpu) {
    struct aes_xcbc_avx_data *d = (struct aes_xcbc_avx_data *)test->data;
    uint8_t *ciphertext = (uint8_t *)malloc(DATA_SIZE);
    uint8_t *decrypted = (uint8_t *)malloc(DATA_SIZE);
    uint8_t *full_mac = (uint8_t *)malloc(XCBC_FULL_DIGEST_SIZE);

    /* R2b (B2-003): per-thread plaintext + golden, re-rolled every
     * iteration (thread-safe: shared d is only read after init; the
     * struct copy puts plaintext and the golden fields written by
     * compute_golden — golden_ciphertext/golden_mac — on this
     * thread's stack; keys/IV ride along as identical values). */
    struct aes_xcbc_avx_data local_d;
    memcpy(&local_d, d, sizeof(local_d));
    TEST_LOOP(test, 256) {
        /* re-roll: fresh plaintext + same-iteration golden (EVP encrypt +
         * XCBC-96 recomputed for this thread's data) */
        memset_random(local_d.plaintext, DATA_SIZE);
        aes_xcbc_avx_compute_golden(&local_d);
        aes_encrypt(local_d.aes_key, local_d.aes_iv, local_d.plaintext, ciphertext, DATA_SIZE);
        memcmp_or_fail(ciphertext, local_d.golden_ciphertext, DATA_SIZE, "AES-192-CBC ciphertext mismatch (AVX)");

        aes_decrypt(local_d.aes_key, local_d.aes_iv, ciphertext, decrypted, DATA_SIZE);
        memcmp_or_fail(decrypted, local_d.plaintext, DATA_SIZE, "AES-192-CBC decryption mismatch (AVX)");

        aes_xcbc_96(local_d.xcbc_key, ciphertext, DATA_SIZE, full_mac);
        memcmp_or_fail(full_mac, local_d.golden_mac, XCBC_96_DIGEST_SIZE, "XCBC-96 digest mismatch (AVX)");
    }
    free(ciphertext);
    free(decrypted);
    free(full_mac);
    return EXIT_SUCCESS;
}

static int aes_xcbc_avx_cleanup(struct test *test) {
    free(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ipsec_aes192_cbc_xcbc_96_sve, "Intel IPSEC SVE port: AES192-CBC w/ XCBC-96 (compute path: OpenSSL EVP; port of the avx-named original)")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = aes_xcbc_avx_init,
    .test_run = aes_xcbc_avx_run,
    .test_cleanup = aes_xcbc_avx_cleanup,
    .minimum_cpu = IPSEC_X86_GATE(cpu_haswell),
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#else

static int aes_xcbc_avx_init(struct test *test) {
    log_skip(OsNotSupportedSkipCategory, "OpenSSL build is not enabled");
    return EXIT_SKIP;
}

static int aes_xcbc_avx_run(struct test *test, int cpu) {
    __builtin_unreachable();
}

DECLARE_TEST(ipsec_aes192_cbc_xcbc_96_sve, "Intel IPSEC SVE port: AES192-CBC w/ XCBC-96 (compute path: OpenSSL EVP; port of the avx-named original)")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = aes_xcbc_avx_init,
    .test_run = aes_xcbc_avx_run,
    .minimum_cpu = IPSEC_X86_GATE(cpu_haswell),
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif
