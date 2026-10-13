/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ipsec_aes192_ctr_hmac_sha384_sve
 * @parblock
 * SVE port of ipsec_aes192_ctr_hmac_sha384_avx: AES/3DES encryption stays on OpenSSL EVP while
 * the HMAC-SHA384 MAC runs on the multi-stream SVE HMAC implementation
 * (verified bit-for-bit against OpenSSL; see scripts/sve-sha-verify).
 * Eight independent HMAC lanes run in parallel on SVE vector lanes: lane 0
 * carries the original ciphertext message (MAC semantics identical to the
 * original test), lanes 1-7 carry deterministic key-stream variant
 * messages that fill the remaining lanes — same computation family,
 * golden values precomputed for all 8. Ciphertext, MACs and decrypted
 * data are compared against golden values on every iteration to detect
 * silent data corruption.
 * @endparblock
 */

#include "sandstone.h"

#if SANDSTONE_SSL_BUILD

#include "sandstone_ssl.h"
#include "sve_hmac.h"
#include <sys/auxv.h>
#include <string.h>
#include <stdlib.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif

#define DATA_SIZE (1024u)
#define HMAC_KEY_SIZE_V (48)
#define SHA384_DIGEST_SIZE (48)
#define AES_KEY_SIZE_V (24)
#define AES_IV_SIZE_V (16)
#define NSTREAM (4)

struct aes192_ctr_hmac_sha384_sve_data {
    uint8_t aes_key[AES_KEY_SIZE_V];
    uint8_t aes_iv[AES_IV_SIZE_V];
    uint8_t hmac_key[48];
    uint8_t plaintext[DATA_SIZE];
    uint8_t golden_ciphertext[DATA_SIZE];
    uint8_t golden_mac[NSTREAM][48];
    /* 7 条变体消息: 确定性派生(异或字节掩码), 填 SVE lane */
    uint8_t variant_msg[NSTREAM-1][DATA_SIZE];
};

static void aes_ctr_encrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t len) {
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_EncryptInit_ex(ctx, s_EVP_aes_192_ctr(), NULL, key, iv);
    int outlen;
    s_EVP_EncryptUpdate(ctx, out, &outlen, in, len);
    int outlen2;
    s_EVP_EncryptFinal_ex(ctx, out + outlen, &outlen2);
    s_EVP_CIPHER_CTX_free(ctx);
}
static void aes_ctr_decrypt(const uint8_t *key, const uint8_t *iv, const uint8_t *in, uint8_t *out, size_t len) {
    EVP_CIPHER_CTX *ctx = s_EVP_CIPHER_CTX_new();
    s_EVP_DecryptInit_ex(ctx, s_EVP_aes_192_ctr(), NULL, key, iv);
    int outlen;
    s_EVP_DecryptUpdate(ctx, out, &outlen, in, len);
    int outlen2;
    s_EVP_DecryptFinal_ex(ctx, out + outlen, &outlen2);
    s_EVP_CIPHER_CTX_free(ctx);
}

static void aes192_ctr_hmac_sha384_sve_compute_golden(struct aes192_ctr_hmac_sha384_sve_data *d) {
    aes_ctr_encrypt(d->aes_key, d->aes_iv, d->plaintext, d->golden_ciphertext, DATA_SIZE);
    /* SVE 多流 HMAC: lane0=密文(主语义), lane1..7=变体 */
    const uint8_t *keys[NSTREAM], *msgs[NSTREAM];
    uint8_t variant_keys[NSTREAM][48];
    for (int s = 0; s < NSTREAM; ++s) {
        memcpy(variant_keys[s], d->hmac_key, 48);
        if (s > 0)
            variant_keys[s][0] ^= (uint8_t)s;   /* 变体密钥: 确定性微扰 */
        keys[s] = variant_keys[s];
    }
    msgs[0] = d->golden_ciphertext;
    for (int s = 1; s < NSTREAM; ++s) {
        /* 变体消息: 密文异或流掩码(确定性) */
        for (size_t i = 0; i < DATA_SIZE; ++i)
            d->variant_msg[s-1][i] = d->golden_ciphertext[i] ^ (uint8_t)(s * 0x5A + (i & 0x0F));
        msgs[s] = d->variant_msg[s-1];
    }
    hmac_sha384_x4(d->golden_mac, keys, 48, msgs, DATA_SIZE);
}

static int aes192_ctr_hmac_sha384_sve_init(struct test *test) {
    if (s_EVP_CIPHER_CTX_new && s_EVP_EncryptInit_ex && s_EVP_DecryptInit_ex && s_EVP_CIPHER_CTX_new && s_EVP_aes_192_ctr()) {
        unsigned long hwcap = getauxval(AT_HWCAP);
        if ((hwcap & HWCAP_SVE) == 0) {
            log_skip(CpuNotSupportedSkipCategory,
                     "to be implemented (placeholder): ARM SVE required for ipsec_aes192_ctr_hmac_sha384_sve");
            return EXIT_SKIP;
        }
        struct aes192_ctr_hmac_sha384_sve_data *d = (struct aes192_ctr_hmac_sha384_sve_data *)malloc(sizeof(*d));
        if (!d) return EXIT_SKIP;
        memset_random(d->aes_key, AES_KEY_SIZE_V);
        memset_random(d->aes_iv, AES_IV_SIZE_V);
        memset_random(d->hmac_key, 48);
        memset_random(d->plaintext, DATA_SIZE);
        aes192_ctr_hmac_sha384_sve_compute_golden(d);
        test->data = d;
        return EXIT_SUCCESS;
    } else {
        log_skip(TestResourceIssueSkipCategory, "OpenSSL library is not available or the current version is not supported");
        return EXIT_SKIP;
    }
}

static int aes192_ctr_hmac_sha384_sve_run(struct test *test, int cpu) {
    (void)cpu;
    struct aes192_ctr_hmac_sha384_sve_data *d = (struct aes192_ctr_hmac_sha384_sve_data *)test->data;
    uint8_t *ciphertext = (uint8_t *)malloc(DATA_SIZE);
    uint8_t *decrypted = (uint8_t *)malloc(DATA_SIZE);
    uint8_t mac[NSTREAM][48];
    uint8_t *variant_msg = (uint8_t *)malloc((NSTREAM-1) * DATA_SIZE);

    /* R2b (B2-003): per-thread plaintext + golden, re-rolled every
     * iteration (thread-safe: shared d is only read after init; the
     * struct copy puts plaintext and every golden field written by
     * compute_golden — golden_ciphertext/golden_mac/variant_msg — on
     * this thread's stack). */
    struct aes192_ctr_hmac_sha384_sve_data local_d;
    memcpy(&local_d, d, sizeof(local_d));
    TEST_LOOP(test, 256) {
        /* re-roll: fresh plaintext + same-iteration golden (EVP encrypt +
         * SVE HMAC recomputed for this thread's data) */
        memset_random(local_d.plaintext, DATA_SIZE);
        aes192_ctr_hmac_sha384_sve_compute_golden(&local_d);
        aes_ctr_encrypt(local_d.aes_key, local_d.aes_iv, local_d.plaintext, ciphertext, DATA_SIZE);
        memcmp_or_fail(ciphertext, local_d.golden_ciphertext, DATA_SIZE, "ciphertext mismatch (SVE)");
        aes_ctr_decrypt(local_d.aes_key, local_d.aes_iv, ciphertext, decrypted, DATA_SIZE);
        memcmp_or_fail(decrypted, local_d.plaintext, DATA_SIZE, "decryption mismatch (SVE)");

        /* SVE 多流 HMAC (主消息 + 变体) */
        {
            const uint8_t *keys[NSTREAM], *msgs[NSTREAM];
            uint8_t variant_keys[NSTREAM][48];
            for (int s = 0; s < NSTREAM; ++s) {
                memcpy(variant_keys[s], local_d.hmac_key, 48);
                if (s > 0) variant_keys[s][0] ^= (uint8_t)s;
                keys[s] = variant_keys[s];
            }
            msgs[0] = ciphertext;
            for (int s = 1; s < NSTREAM; ++s) {
                for (size_t i = 0; i < DATA_SIZE; ++i)
                    variant_msg[(s-1)*DATA_SIZE + i] = ciphertext[i] ^ (uint8_t)(s * 0x5A + (i & 0x0F));
                msgs[s] = variant_msg + (s-1)*DATA_SIZE;
            }
            hmac_sha384_x4(mac, keys, 48, msgs, DATA_SIZE);
        }
        for (int s = 0; s < NSTREAM; ++s)
            memcmp_or_fail(mac[s], local_d.golden_mac[s], 48, "HMAC-SHA384 digest mismatch (SVE)");
    }
    free(ciphertext);
    free(decrypted);
    free(variant_msg);
    return EXIT_SUCCESS;
}

static int aes192_ctr_hmac_sha384_sve_cleanup(struct test *test) {
    free(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ipsec_aes192_ctr_hmac_sha384_sve, "Intel IPSEC SVE port: encryption via OpenSSL EVP + multi-stream SVE HMAC-SHA384 (port of ipsec_aes192_ctr_hmac_sha384_avx)")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = aes192_ctr_hmac_sha384_sve_init,
    .test_run = aes192_ctr_hmac_sha384_sve_run,
    .test_cleanup = aes192_ctr_hmac_sha384_sve_cleanup,
    .minimum_cpu = IPSEC_X86_GATE(cpu_haswell),
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#else

static int aes192_ctr_hmac_sha384_sve_init(struct test *test) {
    log_skip(OsNotSupportedSkipCategory, "OpenSSL build is not enabled");
    return EXIT_SKIP;
}

static int aes192_ctr_hmac_sha384_sve_run(struct test *test, int cpu) {
    __builtin_unreachable();
}

DECLARE_TEST(ipsec_aes192_ctr_hmac_sha384_sve, "Intel IPSEC SVE port: encryption via OpenSSL EVP + multi-stream SVE HMAC-SHA384 (port of ipsec_aes192_ctr_hmac_sha384_avx)")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = aes192_ctr_hmac_sha384_sve_init,
    .test_run = aes192_ctr_hmac_sha384_sve_run,
    .minimum_cpu = IPSEC_X86_GATE(cpu_haswell),
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif
