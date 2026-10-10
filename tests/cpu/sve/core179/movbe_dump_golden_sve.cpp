/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movbe_dump_golden_sve
 * @parblock
 * SVE port of movbe_dump_golden (source: missing_testcases_20261009/
 * tests/cpu/misc/movbe_dump_golden.cpp).
 *
 * Cross-validation variant of the movbe round-trip: test_init (which runs
 * on the child main thread before the per-CPU workers spawn) writes the
 * input buffer to a file right after filling it. That file is the
 * independent golden: it records the exact bits written by init, which
 * never passed through a suspect core's load path. The fail-branch
 * golden/actual in the log are re-read by whichever core hit the
 * miscompare, so they are themselves potentially corrupted; comparing the
 * log's golden/actual against this file answers "did the failing core
 * even read the correct input?".
 *
 * The hot loop is identical to movbe_l1d_sve's (the scalar original's run
 * is byte-identical to movbe_dump.cpp's, which movbe_l1d's run copies):
 * in1 = svld1(input); svtbl; svst1(swapped); in2 = asm ldr z reload of
 * input; svtbl back; vector compare with a cold per-element rescan.
 *
 * Deviation from the scalar original (documented): the golden file goes
 * to /var/tmp/movbe_input_golden_sve_<pid>.bin instead of the original's
 * fixed /tmp/movbe_input_golden.bin — /tmp is tmpfs (lost on the
 * crash/hang events this family exists to investigate) and the fixed
 * path lets concurrent runs clobber each other; the dump-capable
 * siblings (neon_rot_ldr_at_top_rowmajor_dump) already chose /var/tmp
 * for exactly this reason. The pid suffix makes overlapping runs safe.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <sys/auxv.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif
#endif

#define MOVBE_DUMP_GOLDEN_SVE_BUFFER_SIZE (1 << 14)

struct MovbeDumpGoldenSveData {
    uint32_t *input;
    uint32_t *swapped;
};

static int movbe_dump_golden_sve_init(struct test *test)
{
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for movbe_dump_golden_sve");
        return EXIT_SKIP;
    }
#endif
    auto *data = static_cast<MovbeDumpGoldenSveData *>(malloc(sizeof(MovbeDumpGoldenSveData)));
    data->input = static_cast<uint32_t *>(aligned_alloc_safe(64, MOVBE_DUMP_GOLDEN_SVE_BUFFER_SIZE * sizeof(uint32_t)));
    data->swapped = static_cast<uint32_t *>(aligned_alloc_safe(64, MOVBE_DUMP_GOLDEN_SVE_BUFFER_SIZE * sizeof(uint32_t)));

    /* init 线程一次性填充; 每次迭代复用 (原版语义) */
    for (size_t i = 0; i < MOVBE_DUMP_GOLDEN_SVE_BUFFER_SIZE; ++i) {
        data->input[i] = random32();
    }

    /* 落盘独立 golden: init 写入, 未经任何 worker 核的加载路径 */
    char path[128];
    snprintf(path, sizeof(path), "/var/tmp/movbe_input_golden_sve_%d.bin", (int)getpid());
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        ssize_t total = 0;
        const char *buf = (const char *)data->input;
        size_t len = MOVBE_DUMP_GOLDEN_SVE_BUFFER_SIZE * sizeof(uint32_t);
        while ((size_t)total < len) {
            ssize_t n = write(fd, buf + total, len - (size_t)total);
            if (n <= 0) break;
            total += n;
        }
        close(fd);
    } else {
        log_error("movbe_dump_golden_sve: cannot open golden file %s (errno %d) — "
                  "continuing without cross-validation golden", path, errno);
    }

    test->data = data;
    return EXIT_SUCCESS;
}

#if defined(__aarch64__)

static inline svuint8_t load_vec_u8_sve(const void *addr)
{
    svuint8_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

static int movbe_dump_golden_sve_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *data = static_cast<MovbeDumpGoldenSveData *>(test->data);

    /* bswap32 的 svtbl 索引: 每元素 [3,2,1,0] (movbe_sve 构造) */
    static const uint8_t idx4[4] = {3, 2, 1, 0};
    const int lanes = svcntw();
    uint8_t vidx_bytes[64];
    for (int i = 0; i < lanes && i < 16; ++i)
        for (int b = 0; b < 4; ++b)
            vidx_bytes[i * 4 + b] = (uint8_t)(i * 4 + idx4[b]);
    svbool_t pg_batch = svwhilelt_b8((uint64_t)0, (uint64_t)(lanes < 16 ? lanes * 4 : 64));
    svuint8_t vidx = svld1_u8(pg_batch, vidx_bytes);

    uint8_t round_bytes[64];

    TEST_LOOP(test, 1 << 13) {
        for (size_t base = 0; base < MOVBE_DUMP_GOLDEN_SVE_BUFFER_SIZE; base += lanes) {
            int n = (MOVBE_DUMP_GOLDEN_SVE_BUFFER_SIZE - base < (size_t)lanes)
                        ? (int)(MOVBE_DUMP_GOLDEN_SVE_BUFFER_SIZE - base) : lanes;
            svbool_t pg = svwhilelt_b8((uint64_t)0, (uint64_t)(n * 4));

            svuint8_t in1 = svld1_u8(pg, (const uint8_t *)(data->input + base));
            svuint8_t vswapped = svtbl_u8(in1, vidx);
            svst1_u8(pg, (uint8_t *)(data->swapped + base), vswapped);

            /* 被测加载: input 的堆 reload (asm 保证, 与 movbe_l1d_sve 同构) */
            svuint8_t in2 = load_vec_u8_sve(data->input + base);

            svuint8_t vround = svtbl_u8(vswapped, vidx);
            if (svptest_any(pg, svcmpne_u8(pg, vround, in2))) {
                svst1_u8(pg, round_bytes, vround);
                uint32_t round[16];
                memcpy(round, round_bytes, (size_t)n * 4);
                for (int k = 0; k < n; ++k) {
                    uint32_t golden = data->input[base + k];
                    if (round[k] != golden) {
                        log_error("MovBE-dump-golden-SVE: Round-trip failed at index %u: "
                                  "input=0x%08X golden=0x%08X actual=0x%08X xor=0x%08X",
                                  (unsigned)(base + k), golden, golden, round[k],
                                  golden ^ round[k]);
                        report_fail_msg("MovBE-SVE: Round-trip failed at index %u",
                                        (unsigned)(base + k));
                    }
                }
            }
        }
    }

    return EXIT_SUCCESS;
}

#else

static int movbe_dump_golden_sve_run(struct test *test, int cpu)
{
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for movbe_dump_golden_sve");
    return EXIT_SKIP;
}

#endif

static int movbe_dump_golden_sve_cleanup(struct test *test)
{
    auto *data = static_cast<MovbeDumpGoldenSveData *>(test->data);
    if (data) {
        free(data->input);
        free(data->swapped);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(movbe_dump_golden_sve,
             "SVE port of movbe_dump_golden: movbe round-trip with init-dumps-input-buffer to "
             "/var/tmp as the independent init-thread golden (hot path identical to movbe_l1d_sve)")
    .test_init = movbe_dump_golden_sve_init,
    .test_run = movbe_dump_golden_sve_run,
    .test_cleanup = movbe_dump_golden_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
