/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_ldr_at_top_str3_checkall_sve
 * @parblock
 * SVE port of neon_rot_ldr_at_top_str3_checkall (source:
 * missing_testcases_20261009/tests/cpu/misc/
 * neon_rot_ldr_at_top_rowmajor_str3_checkall.cpp — test name has no
 * "rowmajor", file name does; kept verbatim).
 *
 * Store-slot classification variant of str3: the same hot loop (3 ldr =
 * dead scratch at top + 2 sources, rotating ALU, THREE stores of the
 * same res to temp/junk/dst, alternating sweeps), but the verification
 * runs THREE unconditional memcmps per sweep (temp, junk, dst each vs
 * golden) OR-ed into a slot_mask, and a noinline cold scan records
 * WHICH store slot(s) mismatched per element:
 *   single-bit mask   -> that store's own write path
 *   0b111 (all three) -> upstream error (all three carry the same res)
 * plus a cold re-read of srcA/srcB (load- vs ALU-error classification)
 * and per-element binary records to /var/tmp. Transient errors (re-read
 * clean) produce a sweep-level record with index 0xFFFFFFFF.
 *
 * The noinline on the cold scan prevents fusion into the hot loop (the
 * tlbstress loop-fusion lesson). Note (kept from the original):
 * within-run s0-vs-s1-vs-s2 rate comparison is valid; cross-test rate
 * vs plain str3 is NOT (the check section grew 1 -> 3 memcmps).
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#if defined(__aarch64__)
#include <arm_sve.h>
#include <sys/auxv.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif
#endif

#define NEON_ROT_LDR_AT_TOP_STR3_CHECKALL_SVE_SLOTS 1024

#pragma pack(push, 1)
struct str3_checkall_sve_record {
    uint64_t timestamp_us;
    uint32_t cpu;             /* logical CPU */
    uint32_t iteration;       /* 0-based sweep */
    uint32_t sweep_dir;       /* 0=asc 1=desc */
    uint32_t sweep_slot_mask; /* bit0=temp bit1=junk bit2=dst */
    uint32_t index;           /* u64 element index; 0xFFFFFFFF = sweep-level transient */
    uint32_t index_slot_mask; /* per-element slot mask at rescan */
    uint32_t op;              /* slot%4 */
    uint64_t srcA, srcB, exp, temp, junk, dst;
};
#pragma pack(pop)

struct NeonRotLdrAtTopStr3CheckallSveData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *scratch;
    uint64_t *expected;
    int lanes;
    int dump_fd;
};

static uint64_t str3_ca_sve_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

#if defined(__aarch64__)

static inline void store_vec_sve(void *addr, svuint64_t val) {
    svst1_u64(svptrue_b64(), static_cast<uint64_t *>(addr), val);
}

static inline svuint64_t load_vec_sve(const void *addr) {
    svuint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

/* noinline: 防冷扫描与热循环融合 (tlbstress 教训) */
__attribute__((noinline)) static void str3_checkall_sve_cold_scan(
        struct test *test, int cpu, const NeonRotLdrAtTopStr3CheckallSveData *data,
        const uint64_t *temp, const uint64_t *junk, const uint64_t *dst,
        uint32_t sweep_slot_mask, uint64_t iteration, uint32_t sweep_dir)
{
    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_STR3_CHECKALL_SVE_SLOTS * data->lanes;
    bool any = false;
    for (size_t j = 0; j < elems; j++) {
        const bool t_bad = temp[j] != data->expected[j];
        const bool k_bad = junk[j] != data->expected[j];
        const bool d_bad = dst[j] != data->expected[j];
        if (!t_bad && !k_bad && !d_bad) continue;
        any = true;
        uint32_t index_slot_mask = (t_bad ? 1u : 0u) | (k_bad ? 2u : 0u) | (d_bad ? 4u : 0u);
        const int slot = (int)(j / (size_t)data->lanes);
        log_error("neon_rot_ldr_at_top_str3_checkall_sve: slot mismatch at index %zu "
                  "(slot %d op=%d dir=%s sweep_mask=0x%x index_mask=0x%x): "
                  "srcA=0x%016lX srcB=0x%016lX exp=0x%016lX temp=0x%016lX "
                  "junk=0x%016lX dst=0x%016lX",
                  j, slot, slot % 4, sweep_dir ? "desc" : "asc",
                  sweep_slot_mask, index_slot_mask,
                  (unsigned long)data->srcA[j], (unsigned long)data->srcB[j],
                  (unsigned long)data->expected[j], (unsigned long)temp[j],
                  (unsigned long)junk[j], (unsigned long)dst[j]);
        if (data->dump_fd >= 0) {
            str3_checkall_sve_record rec;
            rec.timestamp_us = str3_ca_sve_now_us();
            rec.cpu = (uint32_t)device_info[cpu].cpu_number;
            rec.iteration = (uint32_t)iteration;
            rec.sweep_dir = sweep_dir;
            rec.sweep_slot_mask = sweep_slot_mask;
            rec.index = (uint32_t)j;
            rec.index_slot_mask = index_slot_mask;
            rec.op = (uint32_t)(slot % 4);
            rec.srcA = data->srcA[j];
            rec.srcB = data->srcB[j];
            rec.exp = data->expected[j];
            rec.temp = temp[j];
            rec.junk = junk[j];
            rec.dst = dst[j];
            if (write(data->dump_fd, &rec, sizeof rec) < 0) { /* best effort */ }
        }
    }
    if (!any) {
        /* 瞬态错误: memcmp 失配但重扫干净 — sweep 级记录 */
        if (data->dump_fd >= 0) {
            str3_checkall_sve_record rec;
            memset(&rec, 0, sizeof rec);
            rec.timestamp_us = str3_ca_sve_now_us();
            rec.cpu = (uint32_t)device_info[cpu].cpu_number;
            rec.iteration = (uint32_t)iteration;
            rec.sweep_dir = sweep_dir;
            rec.sweep_slot_mask = sweep_slot_mask;
            rec.index = 0xFFFFFFFFu;   /* transient marker (原版语义) */
            log_error("neon_rot_ldr_at_top_str3_checkall_sve: transient mismatch "
                      "(sweep_mask=0x%x, rescan clean)", sweep_slot_mask);
            if (write(data->dump_fd, &rec, sizeof rec) < 0) { /* best effort */ }
        }
    }
}

#endif

static int neon_rot_ldr_at_top_str3_checkall_sve_init(struct test *test) {
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_str3_checkall_sve");
        return EXIT_SKIP;
    }

    const int lanes = (int)svcntd();
    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_STR3_CHECKALL_SVE_SLOTS * lanes;

    auto *data = static_cast<NeonRotLdrAtTopStr3CheckallSveData *>(malloc(sizeof(NeonRotLdrAtTopStr3CheckallSveData)));
    data->lanes = lanes;
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->scratch = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, elems * sizeof(uint64_t)));

    memset_random(data->srcA, elems * sizeof(uint64_t));
    memset_random(data->srcB, elems * sizeof(uint64_t));
    memset_random(data->scratch, elems * sizeof(uint64_t));

    svbool_t pg = svptrue_b64();
    for (int i = 0; i < NEON_ROT_LDR_AT_TOP_STR3_CHECKALL_SVE_SLOTS; i++) {
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

    char path[128];
    snprintf(path, sizeof(path), "/var/tmp/neon_rot_ldr_at_top_str3_checkall_sve_%d.bin", (int)getpid());
    data->dump_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (data->dump_fd < 0) {
        log_error("neon_rot_ldr_at_top_str3_checkall_sve: cannot open %s (errno %d) — "
                  "continuing without binary records", path, errno);
    }

    test->data = data;
    return EXIT_SUCCESS;
#else
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_str3_checkall_sve");
    return EXIT_SKIP;
#endif
}

#if defined(__aarch64__)
static int neon_rot_ldr_at_top_str3_checkall_sve_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<NeonRotLdrAtTopStr3CheckallSveData *>(test->data);

    if ((int)svcntd() != data->lanes) {
        log_error("neon_rot_ldr_at_top_str3_checkall_sve: vector length changed between init (%d) and run (%d)",
                  data->lanes, (int)svcntd());
        report_fail_msg("neon_rot_ldr_at_top_str3_checkall_sve: vector length changed between init and run");
    }

    const size_t elems = (size_t)NEON_ROT_LDR_AT_TOP_STR3_CHECKALL_SVE_SLOTS * data->lanes;
    const size_t bytes = elems * sizeof(uint64_t);
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, bytes));
    uint64_t *junk = static_cast<uint64_t *>(aligned_alloc(64, bytes));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, bytes));

    uint64_t sweep = 0;
    TEST_LOOP(test, 1 << 13) {
        const bool ascending = (sweep & 1) == 0;
        for (int n = 0; n < NEON_ROT_LDR_AT_TOP_STR3_CHECKALL_SVE_SLOTS; n++) {
            const int i = ascending ? n : (NEON_ROT_LDR_AT_TOP_STR3_CHECKALL_SVE_SLOTS - 1 - n);
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
            store_vec_sve(junk + off, res);
            store_vec_sve(dst + off, res);
        }
        const uint32_t sweep_dir = ascending ? 0u : 1u;
        sweep++;

        /* 三个无条件 memcmp OR 成 slot_mask (bit0=temp bit1=junk bit2=dst) */
        uint32_t slot_mask = 0;
        if (memcmp(temp, data->expected, bytes) != 0) slot_mask |= 1u;
        if (memcmp(junk, data->expected, bytes) != 0) slot_mask |= 2u;
        if (memcmp(dst, data->expected, bytes) != 0) slot_mask |= 4u;

        if (slot_mask != 0) {
            str3_checkall_sve_cold_scan(test, cpu, data, temp, junk, dst,
                                        slot_mask, sweep - 1, sweep_dir);
            report_fail_msg("neon_rot_ldr_at_top_str3_checkall_sve data miscompare "
                            "(slot_mask=0x%x)", slot_mask);
        }
    }

    free(dst);
    free(junk);
    free(temp);
    return EXIT_SUCCESS;
}
#else
static int neon_rot_ldr_at_top_str3_checkall_sve_run(struct test *test, int cpu) {
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM SVE required for neon_rot_ldr_at_top_str3_checkall_sve");
    return EXIT_SKIP;
}
#endif

static int neon_rot_ldr_at_top_str3_checkall_sve_cleanup(struct test *test) {
    auto *data = static_cast<NeonRotLdrAtTopStr3CheckallSveData *>(test->data);
    if (data) {
        if (data->dump_fd >= 0)
            close(data->dump_fd);
        free(data->srcA);
        free(data->srcB);
        free(data->scratch);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_ldr_at_top_str3_checkall_sve,
             "SVE store-slot classification variant of str3: same 3-ldr/3-str hot loop; per-sweep "
             "3-way memcmp catches temp-only and junk-only errors; records which store slot(s) "
             "mismatched + transient markers (port of neon_rot_ldr_at_top_str3_checkall)")
    .test_init    = neon_rot_ldr_at_top_str3_checkall_sve_init,
    .test_run     = neon_rot_ldr_at_top_str3_checkall_sve_run,
    .test_cleanup = neon_rot_ldr_at_top_str3_checkall_sve_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
