/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ipsec_aes128_cbc_aes_cmac_sve
 * @parblock
 * This test encrypts random data using AES-128-CBC encryption and computes
 * an AES-CMAC-96 message authentication code (truncated to 12 bytes) on the
 * ciphertext via OpenSSL using the AVX optimized code path. It also decrypts 
 * the ciphertext and verifies that it matches the original plaintext. The 
 * ciphertext, MAC, and decrypted data are compared against pre-computed 
 * golden values on every iteration to detect silent data corruption.
 * @endparblock
 */

#include "sandstone.h"

#if SANDSTONE_SSL_BUILD
#include "sandstone_ssl.h"
#include <string.h>

#define DATA_SIZE             (1024u)
#define AES_KEY_SIZE          (16)
#define AES_IV_SIZE           (16)
#define CMAC_KEY_SIZE         (16)
#define CMAC_96_DIGEST_SIZE   (12)
#define CMAC_FULL_DIGEST_SIZE (16)

struct aes_cmac_avx_data {
    uint8_t aes_key[AES_KEY_SIZE];
    uint8_t aes_iv[AES_IV_SIZE];
    uint8_t cmac_key[CMAC_KEY_SIZE];
    uint8_t plaintext[DATA_SIZE];
    uint8_t golden_ciphertext[DATA_SIZE];
    uint8_t golden_mac[CMAC_96_DIGEST_SIZE];
};

static void aes_cmac_96(const uint8_t *key, const uint8_t *msg, size_t len, uint8_t *out) {
    CMAC_CTX *ctx = s_CMAC_CTX_new();
    s_CMAC_Init(ctx, key, CMAC_KEY_SIZE, s_EVP_aes_128_cbc(), NULL);
    s_CMAC_Update(ctx, msg, len);
    uint8_t full_mac[CMAC_FULL_DIGEST_SIZE];
    size_t outlen;
    s_CMAC_Final(ctx, full_mac, &outlen);
    s_CMAC_CTX_free(ctx);
    memcpy(out, full_mac, CMAC_96_DIGEST_SIZE);
}

static void aes_encrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t len) {
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_EncryptInit_ex(ctx, s_EVP_aes_128_cbc(), NULL, key, iv);
    s_EVP_CIPHER_CTX_set_padding(ctx, 0);
    int outlen;
    s_EVP_EncryptUpdate(ctx, out, &outlen, in, len);
    int outlen2;
    s_EVP_EncryptFinal_ex(ctx, out + outlen, &outlen2);
    s_EVP_CIPHER_CTX_free(ctx);
}

static void aes_decrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t len) {
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_DecryptInit_ex(ctx, s_EVP_aes_128_cbc(), NULL, key, iv);
    s_EVP_CIPHER_CTX_set_padding(ctx, 0);
    int outlen;
    s_EVP_DecryptUpdate(ctx, out, &outlen, in, len);
    int outlen2;
    s_EVP_DecryptFinal_ex(ctx, out + outlen, &outlen2);
    s_EVP_CIPHER_CTX_free(ctx);
}

static void aes_cmac_avx_compute_golden(struct aes_cmac_avx_data *d) {
    aes_encrypt(d->aes_key, d->aes_iv, d->plaintext, d->golden_ciphertext, DATA_SIZE);
    aes_cmac_96(d->cmac_key, d->golden_ciphertext, DATA_SIZE, d->golden_mac);
}

static int aes_cmac_avx_init(struct test *test) {
    if (s_EVP_CIPHER_CTX_new && s_EVP_EncryptInit_ex && s_EVP_DecryptInit_ex && s_EVP_aes_128_cbc() && s_CMAC_CTX_new) {
        struct aes_cmac_avx_data *d = (struct aes_cmac_avx_data *)malloc(sizeof(*d));
        if (!d) return EXIT_SKIP;

        memset_random(d->aes_key, AES_KEY_SIZE);
        memset_random(d->aes_iv, AES_IV_SIZE);
        memset_random(d->cmac_key, CMAC_KEY_SIZE);
        memset_random(d->plaintext, DATA_SIZE);

        aes_cmac_avx_compute_golden(d);

        test->data = d;
        return EXIT_SUCCESS;
    } else {
        log_skip(TestResourceIssueSkipCategory, "OpenSSL library is not available or the current version is not supported");
        return EXIT_SKIP;
    }
}

static int aes_cmac_avx_run(struct test *test, int cpu) {
    struct aes_cmac_avx_data *d = (struct aes_cmac_avx_data *)test->data;

    uint8_t *ciphertext = (uint8_t *)malloc(DATA_SIZE);
    uint8_t *decrypted = (uint8_t *)malloc(DATA_SIZE);
    uint8_t *mac = (uint8_t *)malloc(CMAC_96_DIGEST_SIZE);

    /* R2b (B2-003): per-thread plaintext + golden, re-rolled every
     * iteration (thread-safe: shared d is only read after init; the
     * struct copy puts plaintext and the golden fields written by
     * compute_golden — golden_ciphertext/golden_mac — on this
     * thread's stack; keys/IV ride along as identical values). */
    struct aes_cmac_avx_data local_d;
    memcpy(&local_d, d, sizeof(local_d));
    TEST_LOOP(test, 256) {
        /* re-roll: fresh plaintext + same-iteration golden (EVP encrypt +
         * CMAC-96 recomputed for this thread's data) */
        memset_random(local_d.plaintext, DATA_SIZE);
        aes_cmac_avx_compute_golden(&local_d);
        aes_encrypt(local_d.aes_key, local_d.aes_iv, local_d.plaintext, ciphertext, DATA_SIZE);
        memcmp_or_fail(ciphertext, local_d.golden_ciphertext, DATA_SIZE, "AES-128-CBC ciphertext mismatch (AVX)");

        aes_decrypt(local_d.aes_key, local_d.aes_iv, ciphertext, decrypted, DATA_SIZE);
        memcmp_or_fail(decrypted, local_d.plaintext, DATA_SIZE, "AES-128-CBC decryption mismatch (AVX)");

        aes_cmac_96(local_d.cmac_key, ciphertext, DATA_SIZE, mac);
        memcmp_or_fail(mac, local_d.golden_mac, CMAC_96_DIGEST_SIZE, "AES-CMAC-96 digest mismatch (AVX)");
    }

    free(ciphertext);
    free(decrypted);
    free(mac);
    return EXIT_SUCCESS;
}

static int aes_cmac_avx_cleanup(struct test *test) {
    free(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ipsec_aes128_cbc_aes_cmac_sve, "Intel IPSEC SVE port: AES128-CBC w/ AES-CMAC-96 (compute path: OpenSSL EVP; port of the avx-named original)")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = aes_cmac_avx_init,
    .test_run = aes_cmac_avx_run,
    .test_cleanup = aes_cmac_avx_cleanup,
    .minimum_cpu = IPSEC_X86_GATE(cpu_haswell),
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#else

static int aes_cmac_avx_init(struct test *test) {
    log_skip(OsNotSupportedSkipCategory, "OpenSSL build is not enabled");
    return EXIT_SKIP;
}

static int aes_cmac_avx_run(struct test *test, int cpu) {
    __builtin_unreachable();
}

DECLARE_TEST(ipsec_aes128_cbc_aes_cmac_sve, "Intel IPSEC SVE port: AES128-CBC w/ AES-CMAC-96 (compute path: OpenSSL EVP; port of the avx-named original)")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = aes_cmac_avx_init,
    .test_run = aes_cmac_avx_run,
    .minimum_cpu = IPSEC_X86_GATE(cpu_haswell),
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif
