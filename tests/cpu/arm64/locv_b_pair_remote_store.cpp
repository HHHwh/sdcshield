/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b locv_b0_mrn_pair_base
 * @test @b locv_b1_mrn_pair_poke
 * @test @b locv_b2_mrn_pair_interleave
 * @parblock
 * Family B of the localization-verify campaign (2026-11-01,
 * docs/CORE179_LOCALIZATION_VERIFY_CAMPAIGN_P1_P6_PLAN_20261101.md §4).
 *
 * Question (P1): does the same-LLC-store effect act via (a) generic
 * snoop/backpressure on the forwarding race (candidate 1), or (b)
 * same-line invalidation -> refill of the recipe's lines (candidate 2)?
 *
 * All three tests share the mrn_reloaded_2src_alu recipe (str→ldr
 * back-to-back, 2src+ALU, qword).  Threads pair up as (cpu, cpu^1) —
 * adjacent core, same cluster, same LLC domain guaranteed.
 *
 *   B0 base       : private temp buffer, partnership scaffolding but no
 *                   cross-core traffic.  Same-day baseline.
 *   B1 poke       : private temp buffer + every 64 elements each thread
 *                   stores a marker into the PAIR-SHARED poke line.
 *                   Both partners hit the same line → mutual snoop/tag
 *                   pressure on each other's L1, recipe lines never
 *                   touched.  = snoop pressure WITHOUT invalidation.
 *   B2 interleave : the pair shares ONE buffer with qwords interleaved
 *                   (even = partner A, odd = partner B) — every recipe
 *                   store false-shares a line → invalidation + refill +
 *                   snoop at full cadence.  Semantics preserved (each
 *                   thread reads back only its own qwords).
 *
 * Pre-registered readouts:
 *   - B1 ≈ B0 and B2 > both     → invalidation/refill implicated (cand. 2)
 *   - B1 > B0 and B2 ≈ B1       → snoop pressure suffices (cand. 1)
 *   - B2 < B0                   → same-line interference DESTROYS the
 *                                 window (race is snoop-sensitive; also
 *                                 informative)
 *   - all ≈ B0                  → cadence too low, denser rerun
 *
 * Declared confounds: B2's interference density is every-element vs
 * B1's 1/64 (directional readout only); B2's per-element str becomes
 * miss+fill (loop cadence shifts, loops count not comparable); B0/B1
 * carry the k&63 poke-check branch vs plain A0 (B-family compares
 * among themselves; A0 cross-comparison is secondary).
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

#define LOCV_B_COUNT 1024
#define LOCV_B_POKE_EVERY 64

enum LocvBMode { B_BASE = 0, B_POKE = 1, B_INTERLEAVE = 2 };

struct LocvBData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *expected;
    uint64_t *poke;      /* one 64B line per pair (8 qwords) */
    uint64_t *pairbuf;   /* 2*COUNT qwords per pair, interleaved */
    std::atomic<int> *ready; /* per-cpu liveness flag (relaxed cross-thread) */
};

#if defined(__x86_64__) || defined(__i386__)

static inline void store_qword(void *addr, uint64_t val) {
    __asm__ volatile ("movq %1, %0" : "=m"(*reinterpret_cast<uint64_t*>(addr)) : "r"(val) : "memory");
}

static inline uint64_t load_qword(const void *addr) {
    uint64_t res;
    __asm__ volatile ("movq %1, %0" : "=r"(res) : "m"(*reinterpret_cast<const uint64_t*>(addr)) : "memory");
    return res;
}

#elif defined(__aarch64__)

static inline void store_qword(void *addr, uint64_t val) {
    __asm__ volatile ("str %1, [%0]" : : "r"(addr), "r"(val) : "memory");
}

static inline uint64_t load_qword(const void *addr) {
    uint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=r"(res) : "r"(addr) : "memory");
    return res;
}

#endif

static int locv_b_init(struct test *test) {
    auto *data = static_cast<LocvBData *>(malloc(sizeof(LocvBData)));
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, LOCV_B_COUNT * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, LOCV_B_COUNT * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, LOCV_B_COUNT * sizeof(uint64_t)));
    /* per-cpu 就绪表与 pair 缓冲按实际线程数定尺寸：原硬编码 192 线程
     * (LOCV_B_PAIRS=96) 在 >192 核主机上 ready[cpu]/qbase 会越界（潜伏 UB）。
     * cpu 是框架的稠密线程下标 0..thread_count()-1；pair = cpu>>1，partner
     * = cpu^1（奇线程数时末线程的 partner 落到 2*pairs，故就绪表多留一格）。 */
    const int pairs = (thread_count() + 1) / 2;
    data->poke = static_cast<uint64_t *>(aligned_alloc(64, pairs * 8 * sizeof(uint64_t)));
    data->pairbuf = static_cast<uint64_t *>(aligned_alloc(64, pairs * 2 * LOCV_B_COUNT * sizeof(uint64_t)));
    data->ready = new std::atomic<int>[2 * pairs]();  /* value-initialized to 0 */

    memset_random(data->srcA, LOCV_B_COUNT * sizeof(uint64_t));
    memset_random(data->srcB, LOCV_B_COUNT * sizeof(uint64_t));

    for (int i = 0; i < LOCV_B_COUNT; i++) {
        switch (i % 4) {
            case 0: data->expected[i] = data->srcA[i] + data->srcB[i]; break;
            case 1: data->expected[i] = data->srcA[i] - data->srcB[i]; break;
            case 2: data->expected[i] = data->srcA[i] ^ data->srcB[i]; break;
            case 3: data->expected[i] = data->srcA[i] & data->srcB[i]; break;
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int locv_b_run_common(struct test *test, int cpu, LocvBMode mode) {
    auto *data = static_cast<LocvBData *>(test->data);

    const int partner = cpu ^ 1;
    const int pair = cpu >> 1;
    const int odd = cpu & 1;
    uint64_t *qbase = data->pairbuf + (size_t)pair * 2 * LOCV_B_COUNT;

    uint64_t *temp = nullptr;
    if (mode != B_INTERLEAVE)
        temp = static_cast<uint64_t *>(aligned_alloc(64, LOCV_B_COUNT * sizeof(uint64_t)));

    data->ready[cpu].store(1, std::memory_order_release);
    uint64_t marker = (uint64_t)cpu << 32;

    TEST_LOOP(test, 1 << 8) {
        for (int k = 0; k < LOCV_B_COUNT; k++) {
            uint64_t a = data->srcA[k];
            uint64_t b = data->srcB[k];
            uint64_t res;

            switch (k % 4) {
                case 0: res = a + b; break;
                case 1: res = a - b; break;
                case 2: res = a ^ b; break;
                case 3: res = a & b; break;
            }

            uint64_t *slot = (mode == B_INTERLEAVE) ? &qbase[2 * k + odd] : &temp[k];
            store_qword(slot, res);
            uint64_t r = load_qword(slot);
            if (r != data->expected[k]) {
                report_fail_msg("locv_b (mode=%d) qword miscompare at %d: "
                                "expected 0x%llx got 0x%llx", (int)mode, k,
                                static_cast<unsigned long long>(data->expected[k]),
                                static_cast<unsigned long long>(r));
            }
            if (mode == B_POKE && (k & (LOCV_B_POKE_EVERY - 1)) == 0
                && data->ready[partner].load(std::memory_order_acquire)) {
                store_qword(&data->poke[pair * 8], ++marker);
            }
        }
    }

    if (temp) free(temp);
    return EXIT_SUCCESS;
}

static int locv_b0_run(struct test *test, int cpu) { return locv_b_run_common(test, cpu, B_BASE); }
static int locv_b1_run(struct test *test, int cpu) { return locv_b_run_common(test, cpu, B_POKE); }
static int locv_b2_run(struct test *test, int cpu) { return locv_b_run_common(test, cpu, B_INTERLEAVE); }

static int locv_b_cleanup(struct test *test) {
    auto *data = static_cast<LocvBData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data->poke);
        free(data->pairbuf);
        delete[] data->ready;
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(locv_b0_mrn_pair_base, "locv B0: pair scaffolding baseline (no cross-core traffic)")
    .test_init    = locv_b_init,
    .test_run     = locv_b0_run,
    .test_cleanup = locv_b_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

DECLARE_TEST(locv_b1_mrn_pair_poke, "locv B1: partner pokes pair-shared line every 64 elem (snoop pressure, no recipe invalidation)")
    .test_init    = locv_b_init,
    .test_run     = locv_b1_run,
    .test_cleanup = locv_b_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

DECLARE_TEST(locv_b2_mrn_pair_interleave, "locv B2: pair-interleaved temp buffer (full-cadence false sharing: invalidation+refill)")
    .test_init    = locv_b_init,
    .test_run     = locv_b2_run,
    .test_cleanup = locv_b_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
