/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ipsec_aes192_gcm_sve
 * @parblock
 * This test encrypts random data using AES-192-GCM encryption via OpenSSL
 * using the AVX optimized code path. It generates an authentication tag,
 * decrypts the ciphertext, and verifies the tag. The ciphertext, tag, and
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
#include <string.h>

#define DATA_SIZE (1024u)
#define AES_KEY_SIZE (24)
#define AES_IV_SIZE (12)
#define GCM_TAG_SIZE (16)

struct aes_gcm_avx_data {
    uint8_t aes_key[AES_KEY_SIZE];
    uint8_t aes_iv[AES_IV_SIZE];
    uint8_t plaintext[DATA_SIZE];
    uint8_t golden_ciphertext[DATA_SIZE];
    uint8_t golden_tag[GCM_TAG_SIZE];
};

static void aes_gcm_encrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *in, uint8_t *out, uint8_t *tag, size_t len) {
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_EncryptInit_ex(ctx, s_EVP_aes_192_gcm(), NULL, NULL, NULL);
    s_EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, AES_IV_SIZE, NULL);
    s_EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv);
    
    int outlen;
    s_EVP_EncryptUpdate(ctx, out, &outlen, in, len);
    int outlen2;
    s_EVP_EncryptFinal_ex(ctx, out + outlen, &outlen2);
    
    s_EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, GCM_TAG_SIZE, tag);
    s_EVP_CIPHER_CTX_free(ctx);
}

static int aes_gcm_decrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *in, uint8_t *out, const uint8_t *tag, size_t len) {
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_DecryptInit_ex(ctx, s_EVP_aes_192_gcm(), NULL, NULL, NULL);
    s_EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, AES_IV_SIZE, NULL);
    s_EVP_DecryptInit_ex(ctx, NULL, NULL, key, iv);
    
    int outlen;
    s_EVP_DecryptUpdate(ctx, out, &outlen, in, len);
    s_EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, GCM_TAG_SIZE, (void *)tag);
    
    int outlen2;
    int ret = s_EVP_DecryptFinal_ex(ctx, out + outlen, &outlen2);
    s_EVP_CIPHER_CTX_free(ctx);
    
    return ret;
}

static void aes_gcm_avx_compute_golden(struct aes_gcm_avx_data *d) {
    aes_gcm_encrypt(d->aes_key, d->aes_iv, d->plaintext, d->golden_ciphertext, d->golden_tag, DATA_SIZE);
}

static int aes_gcm_avx_init(struct test *test) {
    if (s_EVP_CIPHER_CTX_new && s_EVP_EncryptInit_ex && s_EVP_DecryptInit_ex && s_EVP_aes_192_gcm && s_EVP_CIPHER_CTX_ctrl) {
        struct aes_gcm_avx_data *d = (struct aes_gcm_avx_data *)malloc(sizeof(*d));
        if (!d) return EXIT_SKIP;
        memset_random(d->aes_key, AES_KEY_SIZE);
        memset_random(d->aes_iv, AES_IV_SIZE);
        memset_random(d->plaintext, DATA_SIZE);
        aes_gcm_avx_compute_golden(d);
        test->data = d;
        return EXIT_SUCCESS;
    } else {
        log_skip(TestResourceIssueSkipCategory, "OpenSSL library is not available or the current version is not supported");
        return EXIT_SKIP;
    }
}

static int aes_gcm_avx_run(struct test *test, int cpu) {
    struct aes_gcm_avx_data *d = (struct aes_gcm_avx_data *)test->data;
    uint8_t *ciphertext = (uint8_t *)malloc(DATA_SIZE);
    uint8_t *decrypted = (uint8_t *)malloc(DATA_SIZE);
    uint8_t *tag = (uint8_t *)malloc(GCM_TAG_SIZE);

    /* R2b (B2-003): per-thread plaintext + golden, re-rolled every
     * iteration (thread-safe: shared d is only read after init; the
     * struct copy puts plaintext/golden_ciphertext on this thread's
     * stack and compute_golden writes only into the copy — keys/IV/
     * golden_tag ride along as identical values). */
    struct aes_gcm_avx_data local_d;
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
            aes_gcm_encrypt(d->aes_key, d->aes_iv, d->plaintext, ciphertext, tag, DATA_SIZE);
            memcmp_or_fail(ciphertext, d->golden_ciphertext, DATA_SIZE, "AES-192-GCM ciphertext mismatch (frozen replay)");
            memcmp_or_fail(tag, d->golden_tag, GCM_TAG_SIZE, "AES-192-GCM tag mismatch (frozen replay)");

            int ret = aes_gcm_decrypt(d->aes_key, d->aes_iv, ciphertext, decrypted, tag, DATA_SIZE);
            if (ret != 1) {
                report_fail_msg("AES-192-GCM decryption failed (tag verification failed) (frozen replay)");
            }
            memcmp_or_fail(decrypted, d->plaintext, DATA_SIZE, "AES-192-GCM decryption mismatch (frozen replay)");
        } else {
            /* re-roll: fresh plaintext + same-iteration golden (EVP path
             * recomputed for this thread's data) */
            memset_random(local_d.plaintext, DATA_SIZE);
            aes_gcm_avx_compute_golden(&local_d);
            aes_gcm_encrypt(local_d.aes_key, local_d.aes_iv, local_d.plaintext, ciphertext, tag, DATA_SIZE);
            memcmp_or_fail(ciphertext, local_d.golden_ciphertext, DATA_SIZE, "AES-192-GCM ciphertext mismatch (AVX)");
            memcmp_or_fail(tag, local_d.golden_tag, GCM_TAG_SIZE, "AES-192-GCM tag mismatch (AVX)");

            int ret = aes_gcm_decrypt(local_d.aes_key, local_d.aes_iv, ciphertext, decrypted, tag, DATA_SIZE);
            if (ret != 1) {
                report_fail_msg("AES-192-GCM decryption failed (tag verification failed) (AVX)");
            }
            memcmp_or_fail(decrypted, local_d.plaintext, DATA_SIZE, "AES-192-GCM decryption mismatch (AVX)");
        }
        ++iter;
    }
    free(ciphertext);
    free(decrypted);
    free(tag);
    return EXIT_SUCCESS;
}

static int aes_gcm_avx_cleanup(struct test *test) {
    free(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ipsec_aes192_gcm_sve, "Intel IPSEC SVE port: AES192-GCM (compute path: OpenSSL EVP; port of the avx-named original)")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = aes_gcm_avx_init,
    .test_run = aes_gcm_avx_run,
    .test_cleanup = aes_gcm_avx_cleanup,
    .minimum_cpu = IPSEC_X86_GATE(cpu_haswell),
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#else

static int aes_gcm_avx_init(struct test *test) {
    log_skip(OsNotSupportedSkipCategory, "OpenSSL build is not enabled");
    return EXIT_SKIP;
}

static int aes_gcm_avx_run(struct test *test, int cpu) {
    __builtin_unreachable();
}

DECLARE_TEST(ipsec_aes192_gcm_sve, "Intel IPSEC SVE port: AES192-GCM (compute path: OpenSSL EVP; port of the avx-named original)")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = aes_gcm_avx_init,
    .test_run = aes_gcm_avx_run,
    .minimum_cpu = IPSEC_X86_GATE(cpu_haswell),
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif
