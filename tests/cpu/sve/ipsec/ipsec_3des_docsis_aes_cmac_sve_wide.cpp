/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ipsec_3des_docsis_aes_cmac_sve_wide
 * @parblock
 * This test encrypts random data using 3DES-DOCSIS encryption and computes
 * an AES-CMAC message authentication code on the ciphertext via OpenSSL
 * using the AVX-512 optimized code path. It also decrypts the ciphertext 
 * and verifies that it matches the original plaintext. Both the ciphertext, 
 * MAC, and decrypted data are compared against pre-computed golden values 
 * on every iteration to detect silent data corruption. Iterations alternate
 * between a re-rolled input with a same-core recomputed golden and a frozen
 * replay of the init-time inputs compared byte-exact against the init
 * thread's cross-core golden, adding coverage for deterministic
 * (mercurial-core) defects.
 * @endparblock
 */

#include "sandstone.h"

#if SANDSTONE_SSL_BUILD
#include "sandstone_ssl.h"
#include <string.h>

#define DATA_SIZE             (1024u)
#define DES3_KEY_SIZE         (24)
#define DES3_IV_SIZE          (8)
#define AES_KEY_SIZE          (16)
#define CMAC_DIGEST_SIZE      (16)

struct cmac_avx512_data {
    uint8_t des3_key[DES3_KEY_SIZE];
    uint8_t des3_iv[DES3_IV_SIZE];
    uint8_t aes_key[AES_KEY_SIZE];
    uint8_t plaintext[DATA_SIZE];
    uint8_t golden_ciphertext[DATA_SIZE];
    uint8_t golden_mac[CMAC_DIGEST_SIZE];
};

static void aes_cmac(const uint8_t *key, const uint8_t *msg, size_t len, uint8_t *out) {
    CMAC_CTX *ctx = s_CMAC_CTX_new();
    s_CMAC_Init(ctx, key, AES_KEY_SIZE, s_EVP_aes_128_cbc(), NULL);
    s_CMAC_Update(ctx, msg, len);
    size_t outlen;
    s_CMAC_Final(ctx, out, &outlen);
    s_CMAC_CTX_free(ctx);
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

static void cmac_avx512_compute_golden(struct cmac_avx512_data *d) {
    des3_encrypt(d->des3_key, d->des3_iv, d->plaintext, d->golden_ciphertext, DATA_SIZE);
    aes_cmac(d->aes_key, d->golden_ciphertext, DATA_SIZE, d->golden_mac);
}

static int cmac_avx512_init(struct test *test) {
    if (s_EVP_CIPHER_CTX_new && s_EVP_EncryptInit_ex && s_EVP_DecryptInit_ex && s_EVP_des_ede3_cbc && s_CMAC_CTX_new && s_CMAC_Init) {
        struct cmac_avx512_data *d = (struct cmac_avx512_data *)malloc(sizeof(*d));
        if (!d) return EXIT_SKIP;

        memset_random(d->des3_key, DES3_KEY_SIZE);
        memset_random(d->des3_iv, DES3_IV_SIZE);
        memset_random(d->aes_key, AES_KEY_SIZE);
        memset_random(d->plaintext, DATA_SIZE);

        cmac_avx512_compute_golden(d);

        test->data = d;
        return EXIT_SUCCESS;
    } else {
        log_skip(TestResourceIssueSkipCategory, "OpenSSL library is not available or the current version is not supported");
        return EXIT_SKIP;
    }
}

static int cmac_avx512_run(struct test *test, int cpu) {
    struct cmac_avx512_data *d = (struct cmac_avx512_data *)test->data;

    uint8_t *ciphertext = (uint8_t *)malloc(DATA_SIZE);
    uint8_t *decrypted = (uint8_t *)malloc(DATA_SIZE);
    uint8_t *mac = (uint8_t *)malloc(CMAC_DIGEST_SIZE);

    /* R2b (B2-003): per-thread plaintext + golden, re-rolled every
     * iteration (thread-safe: shared d is only read after init; the
     * struct copy puts plaintext and the golden fields written by
     * compute_golden — golden_ciphertext/golden_mac — on this
     * thread's stack; keys/IV ride along as identical values). */
    struct cmac_avx512_data local_d;
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
            des3_encrypt(d->des3_key, d->des3_iv, d->plaintext, ciphertext, DATA_SIZE);
            memcmp_or_fail(ciphertext, d->golden_ciphertext, DATA_SIZE, "3DES-DOCSIS ciphertext mismatch (frozen replay)");

            des3_decrypt(d->des3_key, d->des3_iv, ciphertext, decrypted, DATA_SIZE);
            memcmp_or_fail(decrypted, d->plaintext, DATA_SIZE, "3DES-DOCSIS decryption mismatch (frozen replay)");

            aes_cmac(d->aes_key, ciphertext, DATA_SIZE, mac);
            memcmp_or_fail(mac, d->golden_mac, CMAC_DIGEST_SIZE, "AES-CMAC digest mismatch (frozen replay)");
        } else {
            /* re-roll: fresh plaintext + same-iteration golden (EVP encrypt +
             * CMAC recomputed for this thread's data) */
            memset_random(local_d.plaintext, DATA_SIZE);
            cmac_avx512_compute_golden(&local_d);
            des3_encrypt(local_d.des3_key, local_d.des3_iv, local_d.plaintext, ciphertext, DATA_SIZE);
            memcmp_or_fail(ciphertext, local_d.golden_ciphertext, DATA_SIZE, "3DES-DOCSIS ciphertext mismatch (AVX-512)");

            des3_decrypt(local_d.des3_key, local_d.des3_iv, ciphertext, decrypted, DATA_SIZE);
            memcmp_or_fail(decrypted, local_d.plaintext, DATA_SIZE, "3DES-DOCSIS decryption mismatch (AVX-512)");

            aes_cmac(local_d.aes_key, ciphertext, DATA_SIZE, mac);
            memcmp_or_fail(mac, local_d.golden_mac, CMAC_DIGEST_SIZE, "AES-CMAC digest mismatch (AVX-512)");
        }
        ++iter;
    }

    free(ciphertext);
    free(decrypted);
    free(mac);
    return EXIT_SUCCESS;
}

static int cmac_avx512_cleanup(struct test *test) {
    free(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ipsec_3des_docsis_aes_cmac_sve_wide, "Intel IPSEC SVE port: 3DES-DOCSIS w/ AES-CMAC (compute path: OpenSSL EVP; port of the avx-named original)")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = cmac_avx512_init,
    .test_run = cmac_avx512_run,
    .test_cleanup = cmac_avx512_cleanup,
    .minimum_cpu = IPSEC_X86_GATE(cpu_skylake_avx512),
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#else

static int cmac_avx512_init(struct test *test) {
    log_skip(OsNotSupportedSkipCategory, "OpenSSL build is not enabled");
    return EXIT_SKIP;
}

static int cmac_avx512_run(struct test *test, int cpu) {
    __builtin_unreachable();
}

DECLARE_TEST(ipsec_3des_docsis_aes_cmac_sve_wide, "Intel IPSEC SVE port: 3DES-DOCSIS w/ AES-CMAC (compute path: OpenSSL EVP; port of the avx-named original)")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = cmac_avx512_init,
    .test_run = cmac_avx512_run,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif
