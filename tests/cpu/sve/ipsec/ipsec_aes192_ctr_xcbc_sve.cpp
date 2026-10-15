/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ipsec_aes192_ctr_xcbc_sve
 * @parblock
 * This test encrypts random data using AES-192-CTR encryption and computes
 * an AES-XCBC-96 message authentication code (truncated to 12 bytes) on the
 * ciphertext via OpenSSL AES-ECB primitives using the AVX optimized code path.
 * It also decrypts the ciphertext and verifies that it matches the original
 * plaintext. The ciphertext, MAC, and decrypted data are compared against
 * pre-computed golden values on every iteration to detect silent data
 * corruption. Iterations alternate between a re-rolled input with a
 * same-core recomputed golden and a frozen replay of the init-time inputs
 * compared byte-exact against the init thread's cross-core golden, adding
 * coverage for deterministic (mercurial-core) defects.
 * @endparblock
 */

#include "sandstone.h"

#if SANDSTONE_SSL_BUILD

#include "sandstone_ssl.h"
#include "sdc_stress_config.h"
#include <string.h>

#define AES_KEY_SIZE (24)
#define AES_IV_SIZE (16)
#define XCBC_KEY_SIZE (16)
#define XCBC_96_DIGEST_SIZE (12)
#define XCBC_FULL_DIGEST_SIZE (16)

struct aes_xcbc_avx_data {
    uint8_t aes_key[AES_KEY_SIZE];
    uint8_t aes_iv[AES_IV_SIZE];
    uint8_t xcbc_key[XCBC_KEY_SIZE];
    uint8_t *plaintext;
    uint8_t *golden_ciphertext;
    uint8_t golden_mac[XCBC_96_DIGEST_SIZE];
    size_t data_size;
};

static size_t ipsec_data_size(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "data_size", 0, 1024, 16 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("ipsec_aes192_ctr_xcbc_sve", "data_size", &v, 1024, 16 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count(1, 4, 1.5, 600, thread_count(),
                                         1024, 16 << 20, test->id, "data_size");
}

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

    aes_ecb_encrypt(key, const1, K1);
    aes_ecb_encrypt(key, const2, K2);
    aes_ecb_encrypt(key, const3, K3);

    size_t n_blocks = (len + 15) / 16;
    uint8_t E[16] = {0};
    uint8_t M[16];

    if (n_blocks == 0) {
        memset(M, 0, 16);
        M[0] = 0x80;
        for (int i = 0; i < 16; i++) M[i] ^= K3[i]; // Fixed: Use K3 for empty message as per RFC 3566
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

static void aes_xcbc_avx_compute_golden(struct aes_xcbc_avx_data *d) {
    aes_ctr_encrypt(d->aes_key, d->aes_iv, d->plaintext, d->golden_ciphertext, d->data_size);
    uint8_t full_mac[XCBC_FULL_DIGEST_SIZE];
    aes_xcbc_96(d->xcbc_key, d->golden_ciphertext, d->data_size, full_mac);
    memcpy(d->golden_mac, full_mac, XCBC_96_DIGEST_SIZE);
}

static int aes_xcbc_avx_init(struct test *test) {
    if (s_EVP_CIPHER_CTX_new && s_EVP_EncryptInit_ex && s_EVP_DecryptInit_ex &&
        s_EVP_aes_192_ctr() && s_EVP_aes_128_ecb()) {
        struct aes_xcbc_avx_data *d = (struct aes_xcbc_avx_data *)malloc(sizeof(*d));
        if (!d) return EXIT_SKIP;
        /* 工作集字节数: -O ipsec_aes192_ctr_xcbc_sve.data_size=N /
         * SDC_IPSEC_AES192_CTR_XCBC_SVE_DATA_SIZE 环境变量 / 自适应:
         * 每线程明文+密文+golden 密文+解密缓冲 ~= 4x, 占物理内存 60%,
         * clamp [1024, 16M]。线性缓冲用 sdc_auto_linear_count（sqrt 缩放会欠缩放压力）。历史硬编码 1024。CTR 为流式无需分组对齐,
         * 但统一 16B 对齐无害。 */
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
        memset_random(d->aes_key, AES_KEY_SIZE);
        memset_random(d->aes_iv, AES_IV_SIZE);
        memset_random(d->xcbc_key, XCBC_KEY_SIZE);
        memset_random(d->plaintext, d->data_size);
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
    uint8_t *ciphertext = (uint8_t *)malloc(d->data_size);
    uint8_t *decrypted = (uint8_t *)malloc(d->data_size);
    uint8_t *full_mac = (uint8_t *)malloc(XCBC_FULL_DIGEST_SIZE);

    /* R2b (B2-003): per-thread plaintext + golden, re-rolled every
     * iteration (thread-safe: shared d is only read after init; the
     * struct copy puts plaintext and the golden fields written by
     * compute_golden — golden_ciphertext/golden_mac — on this
     * thread's stack; keys/IV ride along as identical values). */
    struct aes_xcbc_avx_data local_d;
    memcpy(&local_d, d, sizeof(local_d));
    /* plaintext/golden_ciphertext 是指针, 浅拷贝会让 local_d 与共享 d
     * 指向同一缓冲; re-roll 的 memset_random/compute_golden 会破坏 d 的
     * 冻结输入与 golden, 使下一轮 frozen replay 误比较。独立分配隔离。 */
    local_d.plaintext = (uint8_t *)malloc(d->data_size);
    local_d.golden_ciphertext = (uint8_t *)malloc(d->data_size);
    if (!local_d.plaintext || !local_d.golden_ciphertext) {
        free(local_d.plaintext);
        free(local_d.golden_ciphertext);
        free(ciphertext);
        free(decrypted);
        free(full_mac);
        log_skip(TestResourceIssueSkipCategory,
                 "out of memory for per-thread buffers");
        return EXIT_SKIP;
    }
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
            aes_ctr_encrypt(d->aes_key, d->aes_iv, d->plaintext, ciphertext, d->data_size);
            memcmp_or_fail(ciphertext, d->golden_ciphertext, d->data_size, "AES-192-CTR ciphertext mismatch (frozen replay)");

            aes_ctr_decrypt(d->aes_key, d->aes_iv, ciphertext, decrypted, d->data_size);
            memcmp_or_fail(decrypted, d->plaintext, d->data_size, "AES-192-CTR decryption mismatch (frozen replay)");

            aes_xcbc_96(d->xcbc_key, ciphertext, d->data_size, full_mac);
            memcmp_or_fail(full_mac, d->golden_mac, XCBC_96_DIGEST_SIZE, "XCBC-96 digest mismatch (frozen replay)");
        } else {
            /* re-roll: fresh plaintext + same-iteration golden (EVP encrypt +
             * XCBC-96 recomputed for this thread's data) */
            memset_random(local_d.plaintext, d->data_size);
            aes_xcbc_avx_compute_golden(&local_d);
            aes_ctr_encrypt(local_d.aes_key, local_d.aes_iv, local_d.plaintext, ciphertext, d->data_size);
            memcmp_or_fail(ciphertext, local_d.golden_ciphertext, d->data_size, "AES-192-CTR ciphertext mismatch (AVX)");

            aes_ctr_decrypt(local_d.aes_key, local_d.aes_iv, ciphertext, decrypted, d->data_size);
            memcmp_or_fail(decrypted, local_d.plaintext, d->data_size, "AES-192-CTR decryption mismatch (AVX)");

            aes_xcbc_96(local_d.xcbc_key, ciphertext, d->data_size, full_mac);
            memcmp_or_fail(full_mac, local_d.golden_mac, XCBC_96_DIGEST_SIZE, "XCBC-96 digest mismatch (AVX)");
        }
        ++iter;
    }
    free(ciphertext);
    free(decrypted);
    free(full_mac);
    free(local_d.plaintext);
    free(local_d.golden_ciphertext);
    return EXIT_SUCCESS;
}

static int aes_xcbc_avx_cleanup(struct test *test) {
    struct aes_xcbc_avx_data *d = (struct aes_xcbc_avx_data *)test->data;
    if (d) {
        free(d->plaintext);
        free(d->golden_ciphertext);
    }
    free(d);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ipsec_aes192_ctr_xcbc_sve, "Intel IPSEC SVE port: AES192-CTR w/ XCBC-96 (compute path: OpenSSL EVP; port of the avx-named original)")
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

DECLARE_TEST(ipsec_aes192_ctr_xcbc_sve, "Intel IPSEC SVE port: AES192-CTR w/ XCBC-96 (compute path: OpenSSL EVP; port of the avx-named original)")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = aes_xcbc_avx_init,
    .test_run = aes_xcbc_avx_run,
    .minimum_cpu = IPSEC_X86_GATE(cpu_haswell),
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif
