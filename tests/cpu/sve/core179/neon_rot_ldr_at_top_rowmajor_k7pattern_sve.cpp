/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_ldr_at_top_rowmajor_k7p_zero_sve
 * @test @b neon_rot_ldr_at_top_rowmajor_k7p_one_sve
 * @test @b neon_rot_ldr_at_top_rowmajor_k7p_rand_sve
 * @test @b neon_rot_ldr_at_top_rowmajor_k7p_hiham_sve
 * @parblock
 * SVE port of the k7pattern battery (source:
 * missing_testcases_20261009/tests/cpu/misc/
 * neon_rot_ldr_at_top_rowmajor_k7pattern.cpp — one file, four tests
 * sharing one implementation).
 *
 * Pattern-activation axis of the GPS chassis: identical rowmajor
 * skeleton, only the operand FILL varies (pattern_word(which, byte_off)):
 *   k7p_zero  — all-zero operands (activation floor)
 *   k7p_one   — all-one operands (complementary floor)
 *   k7p_rand  — GPS-tagged random operands (B-track baseline; NOTE: the
 *               name says rand but the fill is the DETERMINISTIC GPS tag
 *               generator, not RNG — kept from the scalar original)
 *   k7p_hiham — 0x55/0xAA alternating bytes by (byte_off+byte) parity —
 *               maximum transition density; byte_off parity recoverable
 *               offline
 *
 * Pre-registered readouts (scalar parent lane curve: 2x64b = 223
 * fail/30min, 4x32b = 0, 16x8b = 2; op bias orr 71%/and 23%/eor 5%/add
 * 1%, damage avoids all-0/all-1 segments): zero/one ~0 while rand/hiham
 * fail -> pattern gating; all similar -> lane shape; hiham >> rand ->
 * transition density matters. All four share init/run/cleanup with the
 * pattern enum and the name string (dump filenames, fail messages) as
 * the only parameters.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <sys/auxv.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif
#endif

#define NEON_ROT_LDR_AT_TOP_ROWMAJOR_K7P_SVE_SLOTS 1024
#define K7P_SVE_DUMP_DIR "movbe_log/gps_rowmajor"

#define K7P_SVE_MAGIC_A   0xA5A5ULL
#define K7P_SVE_MAGIC_B   0x5A5AULL
#define K7P_SVE_MAGIC_SCR 0x0F0FULL

enum K7pSvePattern {
    K7P_SVE_ZERO,
    K7P_SVE_ONE,
    K7P_SVE_RAND,     /* GPS-tagged (deterministic), NOT memset_random */
    K7P_SVE_HIHAM,
};

/* which: 0=srcA 1=srcB 2=scratch; byte_off: 该字在其缓冲内的字节偏移 */
static uint64_t k7p_sve_pattern_word(K7pSvePattern p, int which, uint64_t byte_off)
{
    switch (p) {
        case K7P_SVE_ZERO:
            return 0x0000000000000000ULL;
        case K7P_SVE_ONE:
            return 0xFFFFFFFFFFFFFFFFULL;
        case K7P_SVE_RAND: {
            const uint64_t magic = which == 0 ? K7P_SVE_MAGIC_A
                                 : which == 1 ? K7P_SVE_MAGIC_B
                                              : K7P_SVE_MAGIC_SCR;
            const uint64_t off = byte_off & 0xFFFFULL;
            return (magic << 48) | (off << 32) | ((~off & 0xFFFFULL) << 16) | 0ULL;
        }
        case K7P_SVE_HIHAM: {
            /* 逐字节: (byte_off + b) 奇偶 -> 0xAA/0x55, 相邻字节对恒差 8 位 */
            uint64_t w = 0;
            for (int b = 0; b < 8; b++) {
                const uint8_t v = ((byte_off + (uint64_t)b) & 1ULL) ? 0xAAu : 0x55u;
                w |= (uint64_t)v << (8 * b);
            }
            return w;
        }
    }
    return 0;
}

struct NeonRotLdrAtTopRowmajorK7pSveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *scratch;
    uint64_t *expected;
    int lanes;
};

#if defined(__aarch64__)

static inline void store_vec_sve(void *addr, svuint64_t val) {
    svst1_u64(svptrue_b64(), static_cast<uint64_t *>(addr), val);
}

static inline svuint64_t load_vec_sve(const void *addr) {
    svuint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

#endif

static int k7p_sve_init_common(struct test *test, K7pSvePattern pattern)
{
    const int lanes = (int)svcntd();
    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_ROWMAJOR_K7P_SVE_SLOTS * lanes;

    auto *data = static_cast<NeonRotLdrAtTopRowmajorK7pSveData *>(malloc(sizeof(NeonRotLdrAtTopRowmajorK7pSveData)));
    data->lanes = lanes;
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->scratch = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    for (size_t j = 0; j < elems; j++) {
        const uint64_t boff = (uint64_t)j * 8;
        data->srcA[j]    = k7p_sve_pattern_word(pattern, 0, boff);
        data->srcB[j]    = k7p_sve_pattern_word(pattern, 1, boff);
        data->scratch[j] = k7p_sve_pattern_word(pattern, 2, boff);
    }

    svbool_t pg = svptrue_b64();
    for (int i = 0; i < NEON_ROT_LDR_AT_TOP_ROWMAJOR_K7P_SVE_SLOTS; i++) {
        svuint64_t a = svld1_u64(pg, data->srcA + (size_t)i * lanes);
        svuint64_t b = svld1_u64(pg, data->srcB + (size_t)i * lanes);
        svuint64_t res;
        switch (i % 4) {
            case 0: res = svadd_u64_x(pg, a, b); break;
            case 1: res = sveor_u64_x(pg, a, b); break;
            case 2: res = svand_u64_x(pg, a, b); break;
            case 3: res = svorr_u64_x(pg, a, b); break;
        }
        svst1_u64(pg, data->expected + (size_t)i * lanes, res);
    }

    mkdir(K7P_SVE_DUMP_DIR, 0755);

    test->data = data;
    return EXIT_SUCCESS;
}

static int k7p_zero_sve_init(struct test *test)
{
#if defined(__aarch64__)
    if ((getauxval(AT_HWCAP) & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for k7p_zero_sve");
        return EXIT_SKIP;
    }
    return k7p_sve_init_common(test, K7P_SVE_ZERO);
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for k7p_zero_sve");
    return EXIT_SKIP;
#endif
}

static int k7p_one_sve_init(struct test *test)
{
#if defined(__aarch64__)
    if ((getauxval(AT_HWCAP) & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for k7p_one_sve");
        return EXIT_SKIP;
    }
    return k7p_sve_init_common(test, K7P_SVE_ONE);
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for k7p_one_sve");
    return EXIT_SKIP;
#endif
}

static int k7p_rand_sve_init(struct test *test)
{
#if defined(__aarch64__)
    if ((getauxval(AT_HWCAP) & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for k7p_rand_sve");
        return EXIT_SKIP;
    }
    return k7p_sve_init_common(test, K7P_SVE_RAND);
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for k7p_rand_sve");
    return EXIT_SKIP;
#endif
}

static int k7p_hiham_sve_init(struct test *test)
{
#if defined(__aarch64__)
    if ((getauxval(AT_HWCAP) & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for k7p_hiham_sve");
        return EXIT_SKIP;
    }
    return k7p_sve_init_common(test, K7P_SVE_HIHAM);
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for k7p_hiham_sve");
    return EXIT_SKIP;
#endif
}

#if defined(__aarch64__)

/* 共享 run: name 用于转储文件名/失败消息 (原版结构) */
static int k7p_sve_run_common(struct test *test, int cpu, const char *name)
{
    (void)cpu;
    auto *data = static_cast<NeonRotLdrAtTopRowmajorK7pSveData *>(test->data);

    if ((int)svcntd() != data->lanes) {
        log_error("%s: vector length changed between init (%d) and run (%d)",
                  name, data->lanes, (int)svcntd());
        report_fail_msg("%s: vector length changed between init and run", name);
    }

    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_ROWMAJOR_K7P_SVE_SLOTS * data->lanes;
    const size_t bytes = elems * sizeof(uint64_t);
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, bytes));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, bytes));

    uint64_t sweep = 0;
    TEST_LOOP(test, 1 << 13) {
        const bool ascending = (sweep & 1) == 0;
        for (int n = 0; n < NEON_ROT_LDR_AT_TOP_ROWMAJOR_K7P_SVE_SLOTS; n++) {
            const int i = ascending ? n : (NEON_ROT_LDR_AT_TOP_ROWMAJOR_K7P_SVE_SLOTS - 1 - n);
            const size_t off = (size_t)i * data->lanes;

            svuint64_t unused_x = load_vec_sve(data->scratch + off);
            (void)unused_x;

            svuint64_t a = load_vec_sve(data->srcA + off);
            svuint64_t b = load_vec_sve(data->srcB + off);
            svuint64_t res;

            switch (i % 4) {
                case 0: res = svadd_u64_x(svptrue_b64(), a, b); break;
                case 1: res = sveor_u64_x(svptrue_b64(), a, b); break;
                case 2: res = svand_u64_x(svptrue_b64(), a, b); break;
                case 3: res = svorr_u64_x(svptrue_b64(), a, b); break;
            }

            store_vec_sve(temp + off, res);
            store_vec_sve(dst + off, res);
        }
        sweep++;

        if (memcmp(dst, data->expected, bytes) != 0) {
            char path[256];
            snprintf(path, sizeof(path), "%s/%s_fail_cpu%d_sweep%lu.bin",
                     K7P_SVE_DUMP_DIR, name, device_info[cpu].cpu_number, sweep - 1);
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                const char *bufs[5] = {
                    (const char *)dst, (const char *)data->expected,
                    (const char *)data->srcA, (const char *)data->srcB,
                    (const char *)data->scratch,
                };
                for (int k = 0; k < 5; ++k) {
                    ssize_t total = 0;
                    while ((size_t)total < bytes) {
                        ssize_t w = write(fd, bufs[k] + total, bytes - (size_t)total);
                        if (w <= 0) break;
                        total += w;
                    }
                }
                char tail[96];
                int len = snprintf(tail, sizeof(tail), "%s sweep=%lu\n", name, sweep - 1);
                if (write(fd, tail, (size_t)len) < 0) { /* best effort */ }
                close(fd);
            }
            report_fail_msg("%s data miscompare", name);
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int k7p_zero_sve_run(struct test *test, int cpu)
{ return k7p_sve_run_common(test, cpu, "k7p_zero_sve"); }
static int k7p_one_sve_run(struct test *test, int cpu)
{ return k7p_sve_run_common(test, cpu, "k7p_one_sve"); }
static int k7p_rand_sve_run(struct test *test, int cpu)
{ return k7p_sve_run_common(test, cpu, "k7p_rand_sve"); }
static int k7p_hiham_sve_run(struct test *test, int cpu)
{ return k7p_sve_run_common(test, cpu, "k7p_hiham_sve"); }

#else

static int k7p_zero_sve_run(struct test *test, int cpu)
{ (void)test; (void)cpu; return EXIT_SKIP; }
static int k7p_one_sve_run(struct test *test, int cpu)
{ (void)test; (void)cpu; return EXIT_SKIP; }
static int k7p_rand_sve_run(struct test *test, int cpu)
{ (void)test; (void)cpu; return EXIT_SKIP; }
static int k7p_hiham_sve_run(struct test *test, int cpu)
{ (void)test; (void)cpu; return EXIT_SKIP; }

#endif

static int k7p_sve_cleanup(struct test *test)
{
    auto *data = static_cast<NeonRotLdrAtTopRowmajorK7pSveData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_ldr_at_top_rowmajor_k7p_zero_sve,
             "SVE K7 pattern cell: all-zero operands (activation floor of the 2x64b path; "
             "port of neon_rot_ldr_at_top_rowmajor_k7p_zero)")
    .test_init    = k7p_zero_sve_init,
    .test_run     = k7p_zero_sve_run,
    .test_cleanup = k7p_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

DECLARE_TEST(neon_rot_ldr_at_top_rowmajor_k7p_one_sve,
             "SVE K7 pattern cell: all-one operands (complementary activation floor; "
             "port of neon_rot_ldr_at_top_rowmajor_k7p_one)")
    .test_init    = k7p_one_sve_init,
    .test_run     = k7p_one_sve_run,
    .test_cleanup = k7p_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

DECLARE_TEST(neon_rot_ldr_at_top_rowmajor_k7p_rand_sve,
             "SVE K7 pattern cell: GPS-tagged operands (B-track baseline; deterministic tags, "
             "NOT memset_random — name kept from the scalar original; doubles as the "
             "dump-capable A-track; port of neon_rot_ldr_at_top_rowmajor_k7p_rand)")
    .test_init    = k7p_rand_sve_init,
    .test_run     = k7p_rand_sve_run,
    .test_cleanup = k7p_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

DECLARE_TEST(neon_rot_ldr_at_top_rowmajor_k7p_hiham_sve,
             "SVE K7 pattern cell: 0x55/0xAA alternating high-Hamming operands (maximum "
             "transition density; port of neon_rot_ldr_at_top_rowmajor_k7p_hiham)")
    .test_init    = k7p_hiham_sve_init,
    .test_run     = k7p_hiham_sve_run,
    .test_cleanup = k7p_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
