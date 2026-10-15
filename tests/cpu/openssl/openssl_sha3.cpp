/**
 * @file
 *
 * @copyright
 * Copyright 2026 ISCAS.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b openssl_sha3
 * @parblock
 * Stress test for SHA-3 (FIPS 202) and SHAKE128 via OpenSSL's EVP digest
 * API. SHA-3 is the Keccak-f[1600] sponge permutation: theta (column
 * parity), rho (lane rotation), pi (lane transposition), chi (the only
 * nonlinear step, an AND-NOT), iota (round-constant XOR). That
 * AND-XOR-rotate datapath is orthogonal to SHA-2's mod-2^64 additive
 * chains exercised by the companion openssl_sha test, so the two tests
 * together cover distinct functional-unit mixes on the crypto pipes.
 * SHAKE128 adds the extendable-output-function (XOF) squeeze path with a
 * caller-chosen 32-byte output on top of the fixed-length
 * sha3-224/256/384/512 digests. The SDC literature synthesis
 * (docs/paper/SDC_RESEARCH_SYNTHESIS_CN.md §7.2, priority 2) explicitly
 * names SHA-2/3 hashing among the highest-value coverage targets.
 * OpenSSL 3.5 runtime-dispatches to its optimized Keccak implementation
 * on aarch64. Each iteration copies a randomly chosen golden plaintext
 * into an mmap'ed arena at a random offset, recomputes all five digests,
 * and byte-compares against golden values calculated once in init
 * (copy->compute->verify).
 * @endparblock
 */

#include "sandstone.h"

#if SANDSTONE_SSL_BUILD

#include "sandstone_ssl.h"
#include "sdc_stress_config.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/mman.h>

#define PLAINTEXT_SIZE              (512UL)
#define SHA3_GOLDEN_ELEMS           (256UL)
#define SHA3_MAX_OFFSET             (512UL)

#define SHA3_224_DIGEST_LENGTH      (28U)
#define SHA3_256_DIGEST_LENGTH      (32U)
#define SHA3_384_DIGEST_LENGTH      (48U)
#define SHA3_512_DIGEST_LENGTH      (64U)
#define SHA3_DIGEST_MAX             (64U)   /* sha3-512 = 64B; shake128 XOF output is 32B */
#define SHAKE128_OUT_LENGTH         (32U)

struct sha3_elem
{
    uint8_t plain_text[PLAINTEXT_SIZE];
    uint8_t sha3_224sum[SHA3_224_DIGEST_LENGTH];
    uint8_t sha3_256sum[SHA3_256_DIGEST_LENGTH];
    uint8_t sha3_384sum[SHA3_384_DIGEST_LENGTH];
    uint8_t sha3_512sum[SHA3_512_DIGEST_LENGTH];
    uint8_t shake128sum[SHAKE128_OUT_LENGTH];
};

struct sha3_test
{
    uint8_t *arena;
    size_t arena_size;   /* was uint8_t — truncated every value > 255
                          * (randomization hardening H7') */
    sha3_elem golden_elements[SHA3_GOLDEN_ELEMS];
};

static void ssl_sha3_224(sha3_elem *target)
{
    /* Create Context */
    EVP_MD_CTX *mdctx = s_EVP_MD_CTX_new();

    /* Fetch algorithm */
    const EVP_MD *md = s_EVP_get_digestbyname("sha3-224");

    /* Digest */
    unsigned int md_len = 0;
    s_EVP_DigestInit_ex(mdctx, md, NULL);
    s_EVP_DigestUpdate(mdctx, &target->plain_text[0], PLAINTEXT_SIZE);
    s_EVP_DigestFinal_ex(mdctx, &target->sha3_224sum[0], &md_len);
    s_EVP_MD_CTX_free(mdctx);
}

static void ssl_sha3_256(sha3_elem *target)
{
    /* Create Context */
    EVP_MD_CTX *mdctx = s_EVP_MD_CTX_new();

    /* Fetch algorithm */
    const EVP_MD *md = s_EVP_get_digestbyname("sha3-256");

    /* Digest */
    unsigned int md_len = 0;
    s_EVP_DigestInit_ex(mdctx, md, NULL);
    s_EVP_DigestUpdate(mdctx, &target->plain_text[0], PLAINTEXT_SIZE);
    s_EVP_DigestFinal_ex(mdctx, &target->sha3_256sum[0], &md_len);
    s_EVP_MD_CTX_free(mdctx);
}

static void ssl_sha3_384(sha3_elem *target)
{
    /* Create Context */
    EVP_MD_CTX *mdctx = s_EVP_MD_CTX_new();

    /* Fetch algorithm */
    const EVP_MD *md = s_EVP_get_digestbyname("sha3-384");

    /* Digest */
    unsigned int md_len = 0;
    s_EVP_DigestInit_ex(mdctx, md, NULL);
    s_EVP_DigestUpdate(mdctx, &target->plain_text[0], PLAINTEXT_SIZE);
    s_EVP_DigestFinal_ex(mdctx, &target->sha3_384sum[0], &md_len);
    s_EVP_MD_CTX_free(mdctx);
}

static void ssl_sha3_512(sha3_elem *target)
{
    /* Create Context */
    EVP_MD_CTX *mdctx = s_EVP_MD_CTX_new();

    /* Fetch algorithm */
    const EVP_MD *md = s_EVP_get_digestbyname("sha3-512");

    /* Digest */
    unsigned int md_len = 0;
    s_EVP_DigestInit_ex(mdctx, md, NULL);
    s_EVP_DigestUpdate(mdctx, &target->plain_text[0], PLAINTEXT_SIZE);
    s_EVP_DigestFinal_ex(mdctx, &target->sha3_512sum[0], &md_len);
    s_EVP_MD_CTX_free(mdctx);
}

static void ssl_shake128(sha3_elem *target)
{
    /* Create Context */
    EVP_MD_CTX *mdctx = s_EVP_MD_CTX_new();

    /* Fetch algorithm */
    const EVP_MD *md = s_EVP_get_digestbyname("shake128");

    /* Digest: XOF — caller-chosen output length, so finalize with
     * DigestFinalXOF (DigestFinal_ex would fail on an XOF). */
    s_EVP_DigestInit_ex(mdctx, md, NULL);
    s_EVP_DigestUpdate(mdctx, &target->plain_text[0], PLAINTEXT_SIZE);
    s_EVP_DigestFinalXOF(mdctx, &target->shake128sum[0], SHAKE128_OUT_LENGTH);
    s_EVP_MD_CTX_free(mdctx);
}

static size_t sha3_arena_bytes(struct test *test);

static int ssl_sha3_init(struct test* test)
{
    /* 预热 knob 缓存: arena_bytes 在 run(worker线程)里经 sdc_knob_int_clamped
     * 的缓存读取, 必须在 init(主线程)查询一次填充缓存, 否则 debug 构建下
     * worker 线程查询 knob 触发 abort, 且显式配置会失效。 */
    (void)sha3_arena_bytes(test);
    if (s_EVP_MD_CTX_new && s_EVP_get_digestbyname && s_EVP_DigestInit_ex && s_EVP_DigestUpdate && s_EVP_DigestFinal_ex && s_EVP_DigestFinalXOF) {
        const size_t sha3_offset = (random64() & 0x1ff) | 1;
        const size_t sha3_arena_size = sha3_offset + sizeof(sha3_test);
        uint8_t *sha3_arena = (uint8_t *) mmap(NULL, sha3_arena_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);

        sha3_test *sha3_test_ptr = (sha3_test *)(&sha3_arena[sha3_offset]);
        test->data = sha3_test_ptr;

        sha3_test_ptr->arena = sha3_arena;
        sha3_test_ptr->arena_size = sha3_arena_size;

        for (size_t i=0; i<SHA3_GOLDEN_ELEMS; i++)
        {
            sha3_elem *cursor = &sha3_test_ptr->golden_elements[i];
            memset_random(&cursor->plain_text[0], PLAINTEXT_SIZE);

            /* Calculate sha3/shake checksums */
            ssl_sha3_224(cursor);
            ssl_sha3_256(cursor);
            ssl_sha3_384(cursor);
            ssl_sha3_512(cursor);
            ssl_shake128(cursor);
        }

        return EXIT_SUCCESS;
    }
    else {
        log_skip(TestResourceIssueSkipCategory, "OpenSSL library is not available or the current version is not supported");
        return EXIT_SKIP;
    }
}

/* Per-thread MB-scale work arena with guaranteed-disjoint halves (see
 * openssl_sha.cpp): each thread re-rolls ITS OWN golden elem every
 * iteration — fresh plaintext + digests recomputed through the EVP
 * path — then recomputes the same plaintext at a different random
 * address and compares. No shared state is written after init
 * (randomization hardening H7').
 *
 * Arena 尺寸 — 统一配置链:
 *   1. -O openssl_sha3.arena_bytes=N / 配置文件同名条目
 *   2. 环境变量 SDC_OPENSSL_SHA3_ARENA_BYTES
 *   3. 自适应: 物理内存 60% 按线程数均摊, clamp [1MiB, 4GiB]。
 * 历史硬编码 4MiB。 */
static size_t sha3_arena_bytes(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "arena_bytes", 0,
                                     1 << 20, 4ll << 30);
    if (v == 0)
        sdc_env_knob_int_clamped("openssl_sha3", "arena_bytes", &v, 1 << 20, 4ll << 30);
    if (v > 0)
        return (size_t)v;
    int nthreads = thread_count();
    if (nthreads < 1)
        nthreads = 1;
    uint64_t per = sdc_phys_mem_bytes() / 1000 * 600 / (uint64_t)nthreads;
    if (per < (1u << 20))
        per = 1u << 20;
    if (per > (4ull << 30))
        per = 4ull << 30;
    return (size_t)per;
}

static int ssl_sha3_run(struct test* test, int cpu)
{
    const size_t our_arena_size = sha3_arena_bytes(test);
    uint8_t *our_arena = (uint8_t *) mmap(NULL, our_arena_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
    if (our_arena == MAP_FAILED) {
        log_skip(TestResourceIssueSkipCategory, "ssl_sha3: work-arena mmap failed");
        return EXIT_SKIP;
    }

    TEST_LOOP(test, 128) {
        const size_t elem_sz = sizeof(sha3_elem);
        const size_t half = (our_arena_size - 2 * elem_sz) / 2;
        const size_t golden_offset = (random64() % half) + 1;
        const size_t our_offset = half + elem_sz + (random64() % half) + 1;

        sha3_elem *golden_elem = (sha3_elem *) (&our_arena[golden_offset]);
        sha3_elem *our_elem = (sha3_elem *) (&our_arena[our_offset]);

        /* Re-roll this thread's golden elem (same-iteration golden) */
        memset_random(&golden_elem->plain_text[0], PLAINTEXT_SIZE);
        ssl_sha3_224(golden_elem);
        ssl_sha3_256(golden_elem);
        ssl_sha3_384(golden_elem);
        ssl_sha3_512(golden_elem);
        ssl_shake128(golden_elem);

        /* Same plaintext at a DIFFERENT random address */
        memcpy(&our_elem->plain_text, &golden_elem->plain_text[0], PLAINTEXT_SIZE);
        ssl_sha3_224(our_elem);
        ssl_sha3_256(our_elem);
        ssl_sha3_384(our_elem);
        ssl_sha3_512(our_elem);
        ssl_shake128(our_elem);

        /* Check result against golden values */
        memcmp_or_fail(&our_elem->sha3_224sum[0], &golden_elem->sha3_224sum[0], SHA3_224_DIGEST_LENGTH,
                "sha3-224sum values does not match.");
        memcmp_or_fail(&our_elem->sha3_256sum[0], &golden_elem->sha3_256sum[0], SHA3_256_DIGEST_LENGTH,
                "sha3-256sum values does not match.");
        memcmp_or_fail(&our_elem->sha3_384sum[0], &golden_elem->sha3_384sum[0], SHA3_384_DIGEST_LENGTH,
                "sha3-384sum values does not match.");
        memcmp_or_fail(&our_elem->sha3_512sum[0], &golden_elem->sha3_512sum[0], SHA3_512_DIGEST_LENGTH,
                "sha3-512sum values does not match.");
        memcmp_or_fail(&our_elem->shake128sum[0], &golden_elem->shake128sum[0], SHAKE128_OUT_LENGTH,
                "shake128sum values does not match.");
    }

    munmap(our_arena, our_arena_size);
    return EXIT_SUCCESS;
}

#else // !SANDSTONE_SSL_BUILD

static int ssl_sha3_init(struct test *test)
{
    log_skip(OsNotSupportedSkipCategory, "Not supported on this OS");
    return EXIT_SKIP;
}

static int ssl_sha3_run(struct test *test, int cpu)
{
    __builtin_unreachable();
}

#endif

DECLARE_TEST(openssl_sha3, "Test calculating different SHA-3/SHAKE128 checksums (Keccak)")
    .test_init = ssl_sha3_init,
    .test_run = ssl_sha3_run,
    .fracture_loop_count = 4,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
