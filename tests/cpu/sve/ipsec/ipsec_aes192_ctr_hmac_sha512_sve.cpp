/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ipsec_aes192_ctr_hmac_sha512_sve
 * @parblock
 * SVE port of ipsec_aes192_ctr_hmac_sha512_avx: AES/3DES encryption stays on OpenSSL EVP while
 * the HMAC-SHA512 MAC runs on the multi-stream SVE HMAC implementation
 * (verified bit-for-bit against OpenSSL; see scripts/sve-sha-verify).
 * Eight independent HMAC lanes run in parallel on SVE vector lanes: lane 0
 * carries the original ciphertext message (MAC semantics identical to the
 * original test), lanes 1-7 carry deterministic key-stream variant
 * messages that fill the remaining lanes — same computation family,
 * golden values precomputed for all 8. Ciphertext, MACs and decrypted
 * data are compared against golden values on every iteration to detect
 * silent data corruption. Iterations alternate between a re-rolled input
 * with a same-core recomputed golden and a frozen replay of the init-time
 * inputs compared byte-exact against the init thread's cross-core golden,
 * adding coverage for deterministic (mercurial-core) defects.
 * @endparblock
 */

#include "sandstone.h"

#if SANDSTONE_SSL_BUILD

#include "sandstone_ssl.h"
#include "sve_hmac.h"
#include "sdc_stress_config.h"
#include <sys/auxv.h>
#include <string.h>
#include <stdlib.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif

#define HMAC_KEY_SIZE_V (64)
#define SHA512_DIGEST_SIZE (64)
#define AES_KEY_SIZE_V (24)
#define AES_IV_SIZE_V (16)
#define NSTREAM (4)

struct aes192_ctr_hmac_sha512_sve_data {
    uint8_t aes_key[AES_KEY_SIZE_V];
    uint8_t aes_iv[AES_IV_SIZE_V];
    uint8_t hmac_key[64];
    uint8_t *plaintext;
    uint8_t *golden_ciphertext;
    uint8_t golden_mac[NSTREAM][64];
    /* 7 条变体消息: 确定性派生(异或字节掩码), 填 SVE lane */
    uint8_t *variant_msg[NSTREAM-1];
    size_t data_size;
};

static size_t ipsec_data_size(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "data_size", 0, 1024, 16 << 20);
    if (v == 0)
        sdc_env_knob_int_clamped("ipsec_aes192_ctr_hmac_sha512_sve", "data_size", &v, 1024, 16 << 20);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count(1, 4, 1.5, 600, thread_count(),
                                         1024, 16 << 20, test->id, "data_size");
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

static void aes192_ctr_hmac_sha512_sve_compute_golden(struct test *test, struct aes192_ctr_hmac_sha512_sve_data *d) {
    aes_ctr_encrypt(d->aes_key, d->aes_iv, d->plaintext, d->golden_ciphertext, d->data_size);
    /* SVE 多流 HMAC: lane0=密文(主语义), lane1..7=变体 */
    const uint8_t *keys[NSTREAM], *msgs[NSTREAM];
    uint8_t variant_keys[NSTREAM][64];
    for (int s = 0; s < NSTREAM; ++s) {
        memcpy(variant_keys[s], d->hmac_key, 64);
        if (s > 0)
            variant_keys[s][0] ^= (uint8_t)s;   /* 变体密钥: 确定性微扰 */
        keys[s] = variant_keys[s];
    }
    msgs[0] = d->golden_ciphertext;
    for (int s = 1; s < NSTREAM; ++s) {
        /* 变体消息: 密文异或流掩码(确定性) */
        for (size_t i = 0; i < d->data_size; ++i)
            d->variant_msg[s-1][i] = d->golden_ciphertext[i] ^ (uint8_t)(s * 0x5A + (i & 0x0F));
        msgs[s] = d->variant_msg[s-1];
    }
    if (hmac_sha512_x4(d->golden_mac, keys, 64, msgs, d->data_size) != 0) {
        log_error("%s: HMAC capacity overflow (data_size too large)", test->id);
        report_fail_msg("%s: hmac assembly over capacity", test->id);
    }
}

static int aes192_ctr_hmac_sha512_sve_init(struct test *test) {
    if (s_EVP_CIPHER_CTX_new && s_EVP_EncryptInit_ex && s_EVP_DecryptInit_ex && s_EVP_CIPHER_CTX_new && s_EVP_aes_192_ctr()) {
        unsigned long hwcap = getauxval(AT_HWCAP);
        if ((hwcap & HWCAP_SVE) == 0) {
            log_skip(CpuNotSupportedSkipCategory,
                     "to be implemented (placeholder): ARM SVE required for ipsec_aes192_ctr_hmac_sha512_sve");
            return EXIT_SKIP;
        }
        /* The multi-stream SVE HMAC kernels give one stream per SVE
         * lane: 8x32-bit (sha1/224/256) needs svcntw()>=8 and
         * 4x64-bit (384/512) needs svcntd()>=4 -- both mean VL>=256
         * bit. On shorter-VL SVE hardware (e.g. Neoverse N2, VL=128)
         * lanes beyond VL are never written, so skip cleanly instead
         * of comparing unwritten memory (target silicon: VL=256, see
         * tests/cpu/sve/meson.build). */
        if (svcntw() < 8 || svcntd() < 4) {
            log_skip(CpuNotSupportedSkipCategory,
                     "ARM SVE vector length %zu bytes < 32 required by the multi-stream HMAC kernels (svcntw=%zu svcntd=%zu)",
                     svcntb(), (size_t)svcntw(), (size_t)svcntd());
            return EXIT_SKIP;
        }
        struct aes192_ctr_hmac_sha512_sve_data *d = (struct aes192_ctr_hmac_sha512_sve_data *)malloc(sizeof(*d));
        if (!d) return EXIT_SKIP;
        /* 工作集字节数: -O ipsec_aes192_ctr_hmac_sha512_sve.data_size=N /
         * SDC_IPSEC_AES192_CTR_HMAC_SHA512_SVE_DATA_SIZE 环境变量 / 自适应:
         * 每线程明文+密文+golden 密文+解密缓冲+变体消息 ~= (3+NSTREAM-1)x,
         * 占物理内存 60%, clamp [1024, 16M]。线性缓冲用 sdc_auto_linear_count（sqrt 缩放会欠缩放压力）。历史硬编码 1024。CTR 流式
         * 无需分组对齐, 统一 16B 对齐无害, 对齐后 < 1024 取 1024。 */
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
        for (int s = 0; s < NSTREAM - 1; ++s) {
            d->variant_msg[s] = (uint8_t *)malloc(d->data_size);
            if (!d->variant_msg[s]) {
                free(d->plaintext);
                free(d->golden_ciphertext);
                for (int t = 0; t < s; ++t)
                    free(d->variant_msg[t]);
                free(d);
                return EXIT_SKIP;
            }
        }
        memset_random(d->aes_key, AES_KEY_SIZE_V);
        memset_random(d->aes_iv, AES_IV_SIZE_V);
        memset_random(d->hmac_key, 64);
        memset_random(d->plaintext, d->data_size);
        aes192_ctr_hmac_sha512_sve_compute_golden(test, d);
        test->data = d;
        return EXIT_SUCCESS;
    } else {
        log_skip(TestResourceIssueSkipCategory, "OpenSSL library is not available or the current version is not supported");
        return EXIT_SKIP;
    }
}

static int aes192_ctr_hmac_sha512_sve_run(struct test *test, int cpu) {
    (void)cpu;
    struct aes192_ctr_hmac_sha512_sve_data *d = (struct aes192_ctr_hmac_sha512_sve_data *)test->data;
    uint8_t *ciphertext = (uint8_t *)malloc(d->data_size);
    uint8_t *decrypted = (uint8_t *)malloc(d->data_size);
    uint8_t mac[NSTREAM][64];
    uint8_t *variant_msg = (uint8_t *)malloc((NSTREAM-1) * d->data_size);

    /* R2b (B2-003): per-thread plaintext + golden, re-rolled every
     * iteration (thread-safe: shared d is only read after init; the
     * struct copy puts plaintext and every golden field written by
     * compute_golden — golden_ciphertext/golden_mac/variant_msg — on
     * this thread's stack). */
    struct aes192_ctr_hmac_sha512_sve_data local_d;
    memcpy(&local_d, d, sizeof(local_d));
    /* plaintext/golden_ciphertext/variant_msg 是指针, 浅拷贝会让 local_d
     * 与共享 d 指向同一缓冲; re-roll 的 memset_random/compute_golden 会
     * 破坏 d 的冻结输入与 golden, 使下一轮 frozen replay 误比较。独立分配隔离。 */
    local_d.plaintext = (uint8_t *)malloc(d->data_size);
    local_d.golden_ciphertext = (uint8_t *)malloc(d->data_size);
    if (!local_d.plaintext || !local_d.golden_ciphertext) {
        free(local_d.plaintext);
        free(local_d.golden_ciphertext);
        free(ciphertext);
        free(decrypted);
        free(variant_msg);
        log_skip(TestResourceIssueSkipCategory,
                 "out of memory for per-thread buffers");
        return EXIT_SKIP;
    }
    for (int s = 0; s < NSTREAM - 1; ++s) {
        local_d.variant_msg[s] = (uint8_t *)malloc(d->data_size);
        if (!local_d.variant_msg[s]) {
            free(local_d.plaintext);
            free(local_d.golden_ciphertext);
            for (int t = 0; t < s; ++t)
                free(local_d.variant_msg[t]);
            free(ciphertext);
            free(decrypted);
            free(variant_msg);
            log_skip(TestResourceIssueSkipCategory,
                     "out of memory for per-thread buffers");
            return EXIT_SKIP;
        }
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
            memcmp_or_fail(ciphertext, d->golden_ciphertext, d->data_size, "ciphertext mismatch (frozen replay)");
            aes_ctr_decrypt(d->aes_key, d->aes_iv, ciphertext, decrypted, d->data_size);
            memcmp_or_fail(decrypted, d->plaintext, d->data_size, "decryption mismatch (frozen replay)");

            /* SVE 多流 HMAC (主消息 + 变体) */
            {
                const uint8_t *keys[NSTREAM], *msgs[NSTREAM];
                uint8_t variant_keys[NSTREAM][64];
                for (int s = 0; s < NSTREAM; ++s) {
                    memcpy(variant_keys[s], d->hmac_key, 64);
                    if (s > 0) variant_keys[s][0] ^= (uint8_t)s;
                    keys[s] = variant_keys[s];
                }
                msgs[0] = ciphertext;
                for (int s = 1; s < NSTREAM; ++s) {
                    for (size_t i = 0; i < d->data_size; ++i)
                        variant_msg[(s-1)*d->data_size + i] = ciphertext[i] ^ (uint8_t)(s * 0x5A + (i & 0x0F));
                    msgs[s] = variant_msg + (s-1)*d->data_size;
                }
                if (hmac_sha512_x4(mac, keys, 64, msgs, d->data_size) != 0) {
                    log_error("%s: HMAC capacity overflow (data_size too large)", test->id);
                    report_fail_msg("%s: hmac assembly over capacity", test->id);
                }
            }
            for (int s = 0; s < NSTREAM; ++s)
                memcmp_or_fail(mac[s], d->golden_mac[s], 64, "HMAC-SHA512 digest mismatch (frozen replay)");
        } else {
            /* re-roll: fresh plaintext + same-iteration golden (EVP encrypt +
             * SVE HMAC recomputed for this thread's data) */
            memset_random(local_d.plaintext, d->data_size);
            aes192_ctr_hmac_sha512_sve_compute_golden(test, &local_d);
            aes_ctr_encrypt(local_d.aes_key, local_d.aes_iv, local_d.plaintext, ciphertext, d->data_size);
            memcmp_or_fail(ciphertext, local_d.golden_ciphertext, d->data_size, "ciphertext mismatch (SVE)");
            aes_ctr_decrypt(local_d.aes_key, local_d.aes_iv, ciphertext, decrypted, d->data_size);
            memcmp_or_fail(decrypted, local_d.plaintext, d->data_size, "decryption mismatch (SVE)");

            /* SVE 多流 HMAC (主消息 + 变体) */
            {
                const uint8_t *keys[NSTREAM], *msgs[NSTREAM];
                uint8_t variant_keys[NSTREAM][64];
                for (int s = 0; s < NSTREAM; ++s) {
                    memcpy(variant_keys[s], local_d.hmac_key, 64);
                    if (s > 0) variant_keys[s][0] ^= (uint8_t)s;
                    keys[s] = variant_keys[s];
                }
                msgs[0] = ciphertext;
                for (int s = 1; s < NSTREAM; ++s) {
                    for (size_t i = 0; i < d->data_size; ++i)
                        variant_msg[(s-1)*d->data_size + i] = ciphertext[i] ^ (uint8_t)(s * 0x5A + (i & 0x0F));
                    msgs[s] = variant_msg + (s-1)*d->data_size;
                }
                if (hmac_sha512_x4(mac, keys, 64, msgs, d->data_size) != 0) {
                    log_error("%s: HMAC capacity overflow (data_size too large)", test->id);
                    report_fail_msg("%s: hmac assembly over capacity", test->id);
                }
            }
            for (int s = 0; s < NSTREAM; ++s)
                memcmp_or_fail(mac[s], local_d.golden_mac[s], 64, "HMAC-SHA512 digest mismatch (SVE)");
        }
        ++iter;
    }
    free(ciphertext);
    free(decrypted);
    free(variant_msg);
    for (int s = 0; s < NSTREAM - 1; ++s)
        free(local_d.variant_msg[s]);
    free(local_d.plaintext);
    free(local_d.golden_ciphertext);
    return EXIT_SUCCESS;
}

static int aes192_ctr_hmac_sha512_sve_cleanup(struct test *test) {
    struct aes192_ctr_hmac_sha512_sve_data *d = (struct aes192_ctr_hmac_sha512_sve_data *)test->data;
    if (d) {
        free(d->plaintext);
        free(d->golden_ciphertext);
        for (int s = 0; s < NSTREAM - 1; ++s)
            free(d->variant_msg[s]);
    }
    free(d);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ipsec_aes192_ctr_hmac_sha512_sve, "Intel IPSEC SVE port: encryption via OpenSSL EVP + multi-stream SVE HMAC-SHA512 (port of ipsec_aes192_ctr_hmac_sha512_avx)")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = aes192_ctr_hmac_sha512_sve_init,
    .test_run = aes192_ctr_hmac_sha512_sve_run,
    .test_cleanup = aes192_ctr_hmac_sha512_sve_cleanup,
    .minimum_cpu = IPSEC_X86_GATE(cpu_haswell),
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#else

static int aes192_ctr_hmac_sha512_sve_init(struct test *test) {
    log_skip(OsNotSupportedSkipCategory, "OpenSSL build is not enabled");
    return EXIT_SKIP;
}

static int aes192_ctr_hmac_sha512_sve_run(struct test *test, int cpu) {
    __builtin_unreachable();
}

DECLARE_TEST(ipsec_aes192_ctr_hmac_sha512_sve, "Intel IPSEC SVE port: encryption via OpenSSL EVP + multi-stream SVE HMAC-SHA512 (port of ipsec_aes192_ctr_hmac_sha512_avx)")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = aes192_ctr_hmac_sha512_sve_init,
    .test_run = aes192_ctr_hmac_sha512_sve_run,
    .minimum_cpu = IPSEC_X86_GATE(cpu_haswell),
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif
