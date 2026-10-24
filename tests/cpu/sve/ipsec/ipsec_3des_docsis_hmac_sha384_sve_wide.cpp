/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ipsec_3des_docsis_hmac_sha384_sve_wide
 * @parblock
 * SVE port of ipsec_3des_docsis_hmac_sha384_avx512: AES/3DES encryption stays on OpenSSL EVP while
 * the HMAC-SHA384 MAC runs on the multi-stream SVE HMAC implementation
 * (verified bit-for-bit against OpenSSL; see scripts/sve-sha-verify).
 * N = svcntd() (64-bit family) independent HMAC lanes run in parallel on
 * SVE vector lanes (all-VL 128..1024-bit, one message per 64-bit lane): lane 0
 * carries the original ciphertext message (MAC semantics identical to the
 * original test), lanes 1..N-1 carry deterministic key-stream variant
 * messages that fill the remaining lanes — same computation family,
 * golden values precomputed for all N. Ciphertext, MACs and decrypted
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
#include <device/cpu/arm64/arm64_vl.h>
#include <string.h>
#include <stdlib.h>

#define HMAC_KEY_SIZE_V (48)
#define SHA384_DIGEST_SIZE (48)
#define AES_KEY_SIZE_V (24)
#define AES_IV_SIZE_V (8)
#define NSTREAM (SVE_SHA_STREAMS_MAX_64)   /* All-VL: 数组按 1024-bit 上限定维; 运行时流数 = svcntd() */

struct ipsec_3des_docsis_hmac_sha384_sve_wide_data {
    uint8_t aes_key[AES_KEY_SIZE_V];
    uint8_t aes_iv[AES_IV_SIZE_V];
    uint8_t hmac_key[48];
    uint8_t *plaintext;
    uint8_t *golden_ciphertext;
    uint8_t golden_mac[NSTREAM][48];
    /* N-1 条变体消息: 确定性派生(异或字节掩码), 填 SVE lane */
    uint8_t *variant_msg[NSTREAM-1];
    size_t data_size;
    int nstreams;    /* All-VL: init 期流数 = svl/8 (svcntd()), run 漂移守卫用 */
};

static size_t ipsec_data_size(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "data_size", 0, 1024, 1830);
    if (v == 0)
        sdc_env_knob_int_clamped("ipsec_3des_docsis_hmac_sha384_sve_wide", "data_size", &v, 1024, 1830);
    if (v > 0)
        return (size_t)v;
    return (size_t)sdc_auto_linear_count(1, 4, 1.5, 600, thread_count(),
                                         1024, 1830, test->id, "data_size");
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

static void ipsec_3des_docsis_hmac_sha384_sve_wide_compute_golden(struct test *test, struct ipsec_3des_docsis_hmac_sha384_sve_wide_data *d) {
    des3_encrypt(d->aes_key, d->aes_iv, d->plaintext, d->golden_ciphertext, d->data_size);
    /* SVE 多流 HMAC: lane0=密文(主语义), lane1..N-1=变体 */
    const int n = (int)svcntd();   /* 与 kernel 内部推导一致 */
    const uint8_t *keys[NSTREAM], *msgs[NSTREAM];
    uint8_t variant_keys[NSTREAM][48];
    for (int s = 0; s < n; ++s) {
        memcpy(variant_keys[s], d->hmac_key, 48);
        if (s > 0)
            variant_keys[s][0] ^= (uint8_t)s;   /* 变体密钥: 确定性微扰 */
        keys[s] = variant_keys[s];
    }
    msgs[0] = d->golden_ciphertext;
    for (int s = 1; s < n; ++s) {
        /* 变体消息: 密文异或流掩码(确定性) */
        for (size_t i = 0; i < d->data_size; ++i)
            d->variant_msg[s-1][i] = d->golden_ciphertext[i] ^ (uint8_t)(s * 0x5A + (i & 0x0F));
        msgs[s] = d->variant_msg[s-1];
    }
    if (hmac_sha384_x4(d->golden_mac, keys, 48, msgs, d->data_size) != 0) {
        log_error("%s: HMAC capacity overflow (data_size too large)", test->id);
        report_fail_msg("%s: hmac assembly over capacity", test->id);
    }
}

static int ipsec_3des_docsis_hmac_sha384_sve_wide_init(struct test *test) {
    if (s_EVP_CIPHER_CTX_new && s_EVP_EncryptInit_ex && s_EVP_DecryptInit_ex && s_EVP_CIPHER_CTX_new && s_EVP_des_ede3_cbc()) {
        /* All-VL Phase 2 P4: capability + 128..1024-bit range check in
         * one framework-layer call (replaces the old lower-bound-only
         * svcntw()>=8/svcntd()>=4 gate -- the 8.2 defect: at VL>=512 the
         * fixed 8/4-wide W rows overflowed; the STREAMS_MAX-sized rows +
         * this range gate close both ends). arm64_require_sve_vl returns
         * the VL in BYTES. */
        const long svl = arm64_require_sve_vl(test, 128, 1024);
        if (svl < 0)
            return EXIT_SKIP;
        const int nstreams = (int)svl / 8;   /* one stream per 64-bit lane (svcntd) */
        if (nstreams < 2 || nstreams > 16) { /* [2,16], release-effective, NDEBUG-proof */
            log_error("%s: streams %d outside range — unit drift", test->id, nstreams);
            return EXIT_FAILURE;
        }
        struct ipsec_3des_docsis_hmac_sha384_sve_wide_data *d = (struct ipsec_3des_docsis_hmac_sha384_sve_wide_data *)malloc(sizeof(*d));
        if (!d) return EXIT_SKIP;
        d->nstreams = nstreams;
        /* 工作集字节数: -O ipsec_3des_docsis_hmac_sha384_sve_wide.data_size=N /
         * SDC_IPSEC_3DES_DOCSIS_HMAC_SHA384_SVE_WIDE_DATA_SIZE 环境变量 / 自适应:
         * 每线程明文+密文+golden 密文+解密缓冲+变体消息 ~= (3+nstreams-1)x,
         * 占物理内存 60%, clamp [1024, 1830]——上限闭环 8.4: HMAC 组装
         * 缓冲 full[N][2048], 2048-128-17=1903B 安全边界。线性缓冲用 sdc_auto_linear_count（sqrt 缩放会欠缩放压力）。历史硬编码 1024。3DES-CBC
         * 无填充分组 8B, 统一 16B 对齐(天然满足 8B), 对齐后 < 1024 取 1024。 */
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
        for (int s = 0; s < d->nstreams - 1; ++s) {
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
        memset_random(d->hmac_key, 48);
        memset_random(d->plaintext, d->data_size);
        ipsec_3des_docsis_hmac_sha384_sve_wide_compute_golden(test, d);
        test->data = d;
        return EXIT_SUCCESS;
    } else {
        log_skip(TestResourceIssueSkipCategory, "OpenSSL library is not available or the current version is not supported");
        return EXIT_SKIP;
    }
}

static int ipsec_3des_docsis_hmac_sha384_sve_wide_run(struct test *test, int cpu) {
    (void)cpu;
    struct ipsec_3des_docsis_hmac_sha384_sve_wide_data *d = (struct ipsec_3des_docsis_hmac_sha384_sve_wide_data *)test->data;
    /* All-VL policy #3: fail closed on init/run VL drift. */
    if ((int)svcntd() != d->nstreams) {
        log_error("%s: SVE VL drifted between init (%d streams) and run (%d streams)",
                  test->id, d->nstreams, (int)svcntd());
        report_fail_msg("%s: SVE VL drifted between init and run", test->id);
    }
    uint8_t *ciphertext = (uint8_t *)malloc(d->data_size);
    uint8_t *decrypted = (uint8_t *)malloc(d->data_size);
    uint8_t mac[NSTREAM][48];
    uint8_t *variant_msg = (uint8_t *)malloc((d->nstreams-1) * d->data_size);

    /* R2b (B2-003): per-thread plaintext + golden, re-rolled every
     * iteration (thread-safe: shared d is only read after init; the
     * struct copy puts plaintext and every golden field written by
     * compute_golden — golden_ciphertext/golden_mac/variant_msg — on
     * this thread's stack). */
    struct ipsec_3des_docsis_hmac_sha384_sve_wide_data local_d;
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
    for (int s = 0; s < d->nstreams - 1; ++s) {
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
            des3_encrypt(d->aes_key, d->aes_iv, d->plaintext, ciphertext, d->data_size);
            memcmp_or_fail(ciphertext, d->golden_ciphertext, d->data_size, "ciphertext mismatch (frozen replay)");
            des3_decrypt(d->aes_key, d->aes_iv, ciphertext, decrypted, d->data_size);
            memcmp_or_fail(decrypted, d->plaintext, d->data_size, "decryption mismatch (frozen replay)");

            /* SVE 多流 HMAC (主消息 + 变体) */
            {
                const uint8_t *keys[NSTREAM], *msgs[NSTREAM];
                const int n = (int)svcntd();   /* 与 kernel 内部推导一致 */
                uint8_t variant_keys[NSTREAM][48];
                for (int s = 0; s < n; ++s) {
                    memcpy(variant_keys[s], d->hmac_key, 48);
                    if (s > 0) variant_keys[s][0] ^= (uint8_t)s;
                    keys[s] = variant_keys[s];
                }
                msgs[0] = ciphertext;
                for (int s = 1; s < n; ++s) {
                    for (size_t i = 0; i < d->data_size; ++i)
                        variant_msg[(s-1)*d->data_size + i] = ciphertext[i] ^ (uint8_t)(s * 0x5A + (i & 0x0F));
                    msgs[s] = variant_msg + (s-1)*d->data_size;
                }
                if (hmac_sha384_x4(mac, keys, 48, msgs, d->data_size) != 0) {
                    log_error("%s: HMAC capacity overflow (data_size too large)", test->id);
                    report_fail_msg("%s: hmac assembly over capacity", test->id);
                }
            }
            for (int s = 0; s < d->nstreams; ++s)
                memcmp_or_fail(mac[s], d->golden_mac[s], 48, "HMAC-SHA384 digest mismatch (frozen replay)");
        } else {
            /* re-roll: fresh plaintext + same-iteration golden (EVP encrypt +
             * SVE HMAC recomputed for this thread's data) */
            memset_random(local_d.plaintext, d->data_size);
            ipsec_3des_docsis_hmac_sha384_sve_wide_compute_golden(test, &local_d);
            des3_encrypt(local_d.aes_key, local_d.aes_iv, local_d.plaintext, ciphertext, d->data_size);
            memcmp_or_fail(ciphertext, local_d.golden_ciphertext, d->data_size, "ciphertext mismatch (SVE)");
            des3_decrypt(local_d.aes_key, local_d.aes_iv, ciphertext, decrypted, d->data_size);
            memcmp_or_fail(decrypted, local_d.plaintext, d->data_size, "decryption mismatch (SVE)");

            /* SVE 多流 HMAC (主消息 + 变体) */
            {
                const uint8_t *keys[NSTREAM], *msgs[NSTREAM];
                const int n = (int)svcntd();   /* 与 kernel 内部推导一致 */
                uint8_t variant_keys[NSTREAM][48];
                for (int s = 0; s < n; ++s) {
                    memcpy(variant_keys[s], local_d.hmac_key, 48);
                    if (s > 0) variant_keys[s][0] ^= (uint8_t)s;
                    keys[s] = variant_keys[s];
                }
                msgs[0] = ciphertext;
                for (int s = 1; s < n; ++s) {
                    for (size_t i = 0; i < d->data_size; ++i)
                        variant_msg[(s-1)*d->data_size + i] = ciphertext[i] ^ (uint8_t)(s * 0x5A + (i & 0x0F));
                    msgs[s] = variant_msg + (s-1)*d->data_size;
                }
                if (hmac_sha384_x4(mac, keys, 48, msgs, d->data_size) != 0) {
                    log_error("%s: HMAC capacity overflow (data_size too large)", test->id);
                    report_fail_msg("%s: hmac assembly over capacity", test->id);
                }
            }
            for (int s = 0; s < d->nstreams; ++s)
                memcmp_or_fail(mac[s], local_d.golden_mac[s], 48, "HMAC-SHA384 digest mismatch (SVE)");
        }
        ++iter;
    }
    free(ciphertext);
    free(decrypted);
    free(variant_msg);
    for (int s = 0; s < d->nstreams - 1; ++s)
        free(local_d.variant_msg[s]);
    free(local_d.plaintext);
    free(local_d.golden_ciphertext);
    return EXIT_SUCCESS;
}

static int ipsec_3des_docsis_hmac_sha384_sve_wide_cleanup(struct test *test) {
    struct ipsec_3des_docsis_hmac_sha384_sve_wide_data *d = (struct ipsec_3des_docsis_hmac_sha384_sve_wide_data *)test->data;
    if (d) {
        free(d->plaintext);
        free(d->golden_ciphertext);
        for (int s = 0; s < d->nstreams - 1; ++s)
            free(d->variant_msg[s]);
    }
    free(d);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ipsec_3des_docsis_hmac_sha384_sve_wide, "Intel IPSEC SVE port: encryption via OpenSSL EVP + multi-stream SVE HMAC-SHA384 (port of ipsec_3des_docsis_hmac_sha384_avx512)")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = ipsec_3des_docsis_hmac_sha384_sve_wide_init,
    .test_run = ipsec_3des_docsis_hmac_sha384_sve_wide_run,
    .test_cleanup = ipsec_3des_docsis_hmac_sha384_sve_wide_cleanup,
    .minimum_cpu = IPSEC_X86_GATE(cpu_haswell),
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#else

static int ipsec_3des_docsis_hmac_sha384_sve_wide_init(struct test *test) {
    log_skip(OsNotSupportedSkipCategory, "OpenSSL build is not enabled");
    return EXIT_SKIP;
}

static int ipsec_3des_docsis_hmac_sha384_sve_wide_run(struct test *test, int cpu) {
    __builtin_unreachable();
}

DECLARE_TEST(ipsec_3des_docsis_hmac_sha384_sve_wide, "Intel IPSEC SVE port: encryption via OpenSSL EVP + multi-stream SVE HMAC-SHA384 (port of ipsec_3des_docsis_hmac_sha384_avx512)")
    .groups = DECLARE_TEST_GROUPS(&group_ipsec),
    .test_init = ipsec_3des_docsis_hmac_sha384_sve_wide_init,
    .test_run = ipsec_3des_docsis_hmac_sha384_sve_wide_run,
    .minimum_cpu = IPSEC_X86_GATE(cpu_haswell),
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif
