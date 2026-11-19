/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b cachebounce_sdc
 * @parblock
 * Variant combining cachebounce's "all cores bounce on the same cache line"
 * pressure with movbe/mrn_rmw's str→ldr store→reload-and-compare SDC probe.
 *
 * Design intent: every cpu's worker reads the SAME shared input cache lines
 * (generating cross-core MESI ping-pong pressure — one of the "system-wide
 * full-core concurrent pressure" conditions that triggers core-179 SDC, see
 * memory movbe-d-positive-control-20260824), while doing a plain ldr/str
 * read-modify-write + store→load-forwarding reload that compares the stored
 * value against a precomputed golden. This aligns with 179 SDC's trigger
 * path (ordinary ldr/str store→reload, NOT atomic fetch_add, NOT NEON).
 *
 * - input  : shared across all cores, N cache-line-aligned rows (64B each),
 *            footprint far below L1D (consistent with 179 SDC: small
 *            footprint still triggers, see footprint-independent-of-l1d-size)
 * - temp   : per-cpu local buffer (each core writes its own temp, avoiding
 *            cross-core ping-pong on the *store* side; SDC path is
 *            str→ldr forwarding within a single core)
 * - golden : precomputed ALU(input) result, used as the compare reference
 *
 * On miscompare the cold failure branch dumps input/golden/actual/xor with a
 * per-byte breakdown (mirrors movbe_dump) so the flipped bit can be located.
 * The hot loop is NOT perturbed — no extra volatile reads or local snapshots.
 * Do NOT add a recompute field: recompute's input is itself read via L1D
 * (same ldr path, same address), so it has no independence to rule out L1D
 * read faults (see memory mrn-rmw-variant-paths-and-recompute-limit).
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

static constexpr size_t CACHE_LINE_SIZE = 64;
/* 16 rows × 64B = 1KB shared input footprint (<< L1D 64KB, stays resident). */
static constexpr size_t CACHEBOUNCE_ROWS = 16;
static constexpr size_t U64_PER_ROW = CACHE_LINE_SIZE / sizeof(uint64_t);
static constexpr size_t CACHEBOUNCE_COUNT = CACHEBOUNCE_ROWS * U64_PER_ROW;

struct CachebounceSdcData {
    uint64_t *input;     /* shared, all cores read → MESI ping-pong */
    uint64_t *expected;  /* shared golden (precomputed ALU result) */
};

#if defined(__aarch64__)
static inline void store_qword(void *addr, uint64_t val) {
    __asm__ volatile ("str %1, [%0]" : : "r"(addr), "r"(val) : "memory");
}
static inline uint64_t load_qword(const void *addr) {
    uint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=r"(res) : "r"(addr) : "memory");
    return res;
}
#else
static inline void store_qword(void *addr, uint64_t val) {
    *reinterpret_cast<uint64_t*>(addr) = val;
}
static inline uint64_t load_qword(const void *addr) {
    return *reinterpret_cast<const uint64_t*>(addr);
}
#endif

static int cachebounce_sdc_init(struct test *test) {
    auto *data = new CachebounceSdcData();
    if (!data) return EXIT_FAILURE;

    /* Shared input + expected, cache-line aligned so each row is one line. */
    data->input    = static_cast<uint64_t *>(aligned_alloc(CACHE_LINE_SIZE, CACHEBOUNCE_COUNT * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(CACHE_LINE_SIZE, CACHEBOUNCE_COUNT * sizeof(uint64_t)));

    memset_random(data->input, CACHEBOUNCE_COUNT * sizeof(uint64_t));

    /* Golden: a simple xor with a per-row rotating mask. Single ALU op keeps
     * the hot loop tight (store→reload is the point, not ALU diversity). */
    for (size_t i = 0; i < CACHEBOUNCE_COUNT; ++i) {
        uint64_t mask = (0x5A5A5A5A5A5A5A5AULL << (i % 8)) | 0xAAULL;
        data->expected[i] = data->input[i] ^ mask;
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int cachebounce_sdc_run(struct test *test, int cpu) {
    auto *data = static_cast<CachebounceSdcData *>(test->data);

    /* Per-cpu local temp buffer: each core writes its own temp so the
     * store→ldr reload stays within one core (forwarding), while the
     * *input reads* bounce the shared lines across cores (ping-pong). */
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(CACHE_LINE_SIZE, CACHEBOUNCE_COUNT * sizeof(uint64_t)));
    if (!temp) return EXIT_FAILURE;

    TEST_LOOP(test, 1 << 13) {
        for (size_t i = 0; i < CACHEBOUNCE_COUNT; ++i) {
            /* Hot path: ldr shared input (ping-pong pressure) → ALU →
             * str temp → ldr temp (store→load forwarding) → implicit
             * compare via golden below. Plain ldr/str, no atomic, no NEON. */
            uint64_t a = load_qword(&data->input[i]);      /* ldr (shared) */
            uint64_t mask = (0x5A5A5A5A5A5A5A5AULL << (i % 8)) | 0xAAULL;
            uint64_t res = a ^ mask;

            store_qword(&temp[i], res);                    /* str */
            uint64_t dst = load_qword(&temp[i]);           /* ldr (forwarding) */

            if (dst != data->expected[i]) {  /* cold branch only */
                uint64_t inputv = load_qword(&data->input[i]);
                uint64_t golden = data->expected[i];
                uint64_t actual = dst;
                uint64_t xorv = golden ^ actual;

                log_error("cachebounce_sdc: miscompare at index %u (cpu=%d): "
                          "input=0x%016lX golden=0x%016lX actual=0x%016lX "
                          "xor=0x%016lX | temp=0x%016lX dst=0x%016lX | %s",
                          (unsigned)i, cpu,
                          (unsigned long)inputv, (unsigned long)golden,
                          (unsigned long)actual, (unsigned long)xorv,
                          (unsigned long)temp[i], (unsigned long)dst,
                          (temp[i] == dst) ? "[fwd-faithful]" : "[fwd-damaged]");

            }
        }
    }

    free(temp);
    return EXIT_SUCCESS;
}

static int cachebounce_sdc_cleanup(struct test *test) {
    auto *data = static_cast<CachebounceSdcData *>(test->data);
    if (data) {
        free(data->input);
        free(data->expected);
        delete data;
        test->data = nullptr;
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(cachebounce_sdc, "All-core cache-line ping-pong + str→ldr store-reload SDC probe (core-179 path: ordinary ldr/str, small footprint)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = cachebounce_sdc_init,
    .test_run = cachebounce_sdc_run,
    .test_cleanup = cachebounce_sdc_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
