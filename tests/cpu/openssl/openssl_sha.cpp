/**
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test ssl_sha
 *
 * The test calculates 3 different checksums (sha256, sha384 and sha512) for a
 * given random-generated buffer and compares the results against
 * pre-calculated golden values.
 *
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
#define SHA_GOLDEN_ELEMS            (1024UL)
#define SHA_MAX_OFFSET              (512UL)

struct sha_elem
{
    uint8_t plain_text[PLAINTEXT_SIZE];
    uint8_t sha256sum[SHA256_DIGEST_LENGTH];
    uint8_t sha384sum[SHA384_DIGEST_LENGTH];
    uint8_t sha512sum[SHA512_DIGEST_LENGTH];
};

struct sha_test
{
    uint8_t *arena;
    size_t arena_size;   /* was uint8_t — silently truncated every value
                          * above 255 (randomization hardening H7') */
    sha_elem golden_elements[SHA_GOLDEN_ELEMS];
};

static void ssl_sha256(sha_elem *target)
{
    /* Create Context */
    EVP_MD_CTX *mdctx = s_EVP_MD_CTX_new();

    /* Fetch algorithm */
    const EVP_MD *md = s_EVP_get_digestbyname("sha256");

    /* Digest */
    unsigned int md_len = 0;
    s_EVP_DigestInit_ex(mdctx, md, NULL);
    s_EVP_DigestUpdate(mdctx, &target->plain_text[0], PLAINTEXT_SIZE);
    s_EVP_DigestFinal_ex(mdctx, &target->sha256sum[0], &md_len);
    s_EVP_MD_CTX_free(mdctx);
}

static void ssl_sha384(sha_elem *target)
{
    /* Create Context */
    EVP_MD_CTX *mdctx = s_EVP_MD_CTX_new();

    /* Fetch algorithm */
    const EVP_MD *md = s_EVP_get_digestbyname("sha384");

    /* Digest */
    unsigned int md_len = 0;
    s_EVP_DigestInit_ex(mdctx, md, NULL);
    s_EVP_DigestUpdate(mdctx, &target->plain_text[0], PLAINTEXT_SIZE);
    s_EVP_DigestFinal_ex(mdctx, &target->sha384sum[0], &md_len);
    s_EVP_MD_CTX_free(mdctx);
}

static void ssl_sha512(sha_elem *target)
{
    /* Create Context */
    EVP_MD_CTX *mdctx = s_EVP_MD_CTX_new();

    /* Fetch algorithm */
    const EVP_MD *md = s_EVP_get_digestbyname("sha512");

    /* Digest */
    unsigned int md_len = 0;
    s_EVP_DigestInit_ex(mdctx, md, NULL);
    s_EVP_DigestUpdate(mdctx, &target->plain_text[0], PLAINTEXT_SIZE);
    s_EVP_DigestFinal_ex(mdctx, &target->sha512sum[0], &md_len);
    s_EVP_MD_CTX_free(mdctx);
}

static int ssl_sha_init(struct test* test)
{
    if (s_EVP_DigestInit_ex && s_EVP_DigestUpdate && s_EVP_DigestFinal_ex && s_EVP_get_digestbyname) {
        const size_t sha_offset = (random64() & 0x1ff) | 1;
        const size_t sha_arena_size = sha_offset + sizeof(sha_test);
        uint8_t *sha_arena = (uint8_t *) mmap(NULL, sha_arena_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);

        sha_test *sha_test_ptr = (sha_test *)(&sha_arena[sha_offset]);
        test->data = sha_test_ptr;

        sha_test_ptr->arena = sha_arena;
        sha_test_ptr->arena_size = sha_arena_size;

        for (size_t i=0; i<SHA_GOLDEN_ELEMS; i++)
        {
            sha_elem *cursor = &sha_test_ptr->golden_elements[i];
            memset_random(&cursor->plain_text[0], PLAINTEXT_SIZE);

            /* Calculate sha checksums */
            ssl_sha256(cursor);
            ssl_sha384(cursor);
            ssl_sha512(cursor);
        }

        return EXIT_SUCCESS;
    }
    else {
        log_skip(TestResourceIssueSkipCategory, "OpenSSL library is not available or the current version is not supported");
        return EXIT_SKIP;
    }
}

/* Per-thread MB-scale work arena: the elem pair lands at random offsets
 * anywhere inside the arena (was fixed 4 MiB), so the digest inputs/outputs
 * touch different cache sets, pages and (with THP) huge-page offsets every
 * iteration. Each thread re-rolls ITS OWN golden elem every iteration
 * (fresh plaintext + freshly computed digests through the same EVP
 * path), so no shared state is written after init — thread-safe by
 * construction (randomization hardening H7').
 *
 * Arena 尺寸 — 统一配置链:
 *   1. -O openssl_sha.arena_bytes=N / 配置文件同名条目
 *   2. 环境变量 SDC_OPENSSL_SHA_ARENA_BYTES
 *   3. 自适应: 物理内存 60% 按线程数均摊, clamp [1MiB, 4GiB]。
 * arena 越大, golden/our elem 随机落点跨度越大, 覆盖越多 cache set /
 * 页 / THP 偏移。历史硬编码 4MiB。 */
static size_t sha_arena_bytes(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "arena_bytes", 0,
                                     1 << 20, 4ll << 30);
    if (v == 0)
        sdc_env_knob_int_clamped("openssl_sha", "arena_bytes", &v, 1 << 20, 4ll << 30);
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

static int ssl_sha_run(struct test* test, int cpu)
{
    const size_t our_arena_size = sha_arena_bytes(test);
    uint8_t *our_arena = (uint8_t *) mmap(NULL, our_arena_size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
    if (our_arena == MAP_FAILED) {
        log_skip(TestResourceIssueSkipCategory, "ssl_sha: work-arena mmap failed");
        return EXIT_SKIP;
    }

    TEST_LOOP(test, 256) {
        const size_t elem_sz = sizeof(sha_elem);
        /* guaranteed-disjoint placement: split the arena into two halves;
         * golden elem lands anywhere in the first half, our elem anywhere
         * in the second — the two 656-byte ranges can never overlap. */
        const size_t half = (our_arena_size - 2 * elem_sz) / 2;
        const size_t golden_offset = (random64() % half) + 1;
        const size_t our_offset = half + elem_sz + (random64() % half) + 1;

        sha_elem *golden_elem = (sha_elem *) (&our_arena[golden_offset]);
        sha_elem *our_elem = (sha_elem *) (&our_arena[our_offset]);

        /* Re-roll this thread's golden elem: fresh random plaintext +
         * digests computed through the EVP path (same-iteration golden). */
        memset_random(&golden_elem->plain_text[0], PLAINTEXT_SIZE);
        ssl_sha256(golden_elem);
        ssl_sha384(golden_elem);
        ssl_sha512(golden_elem);

        /* Same plaintext into OUR elem at a DIFFERENT random address,
         * then recompute: byte-identical digests expected at any address. */
        memcpy(&our_elem->plain_text, &golden_elem->plain_text[0], PLAINTEXT_SIZE);
        ssl_sha256(our_elem);
        ssl_sha384(our_elem);
        ssl_sha512(our_elem);

        /* Check result against golden values */
        memcmp_or_fail(&our_elem->sha256sum[0], &golden_elem->sha256sum[0], SHA256_DIGEST_LENGTH,
                "sha256sum values does not match.");
        memcmp_or_fail(&our_elem->sha384sum[0], &golden_elem->sha384sum[0], SHA384_DIGEST_LENGTH,
                "sha384sum values does not match.");
        memcmp_or_fail(&our_elem->sha512sum[0], &golden_elem->sha512sum[0], SHA512_DIGEST_LENGTH,
                "sha512sum values does not match.");
    }

    munmap(our_arena, our_arena_size);
    return EXIT_SUCCESS;
}

#else // !SANDSTONE_SSL_BUILD

static int ssl_sha_init(struct test *test)
{
    log_skip(OsNotSupportedSkipCategory, "Not supported on this OS");
    return EXIT_SKIP;
}

static int ssl_sha_run(struct test *test, int cpu)
{
    __builtin_unreachable();
}

#endif

DECLARE_TEST(openssl_sha, "Test calculating different sha checksums")
    .test_init = ssl_sha_init,
    .test_run = ssl_sha_run,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
