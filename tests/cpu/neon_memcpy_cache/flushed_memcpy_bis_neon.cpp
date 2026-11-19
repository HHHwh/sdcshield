/*  NEON variant of tests/cpu/cache/flushed_memcpy_bis_neon.cpp — same-day NEON pass:
 *  load-point memcpy() replaced with the explicit vld1q/vst1q stream
 *  (arm_add_neon_copy.h); flush/verify semantics identical.  Original
  *  untouched; identifiers carry the _neon suffix (ODR).
 */
/**
 * @file
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test flushed_memcpy_bis_neon
 * @parblock
 * Improved version of flushed_memcpy that performs more copies
 * (633 #29 lineage, user spec 2026-11-02; "bis" = the heavier arm).
 *
 * What "more copies" means here (and did NOT mean in the draft): a
 * FOUR-BUFFER ROTATING RELAY — buf0 -> buf1 -> buf2 -> buf3 -> buf0,
 * four real copies per round where each copy's SOURCE is the previous
 * copy's DESTINATION, so every hop moves live data along a fresh
 * path and any lost write or bit flip PROPAGATES to the final state.
 * The draft copied the SAME src to the SAME dst four times — hops
 * 2..4 had no new data path and added duration, not detection surface.
 *
 * After every hop the destination's full 4 KiB is dc-civac'd and
 * drained (heavier than flushed_memcpy's interleaved frontier
 * evictions — the "increasing cache stress" the spec asks for), then
 * the hop is verified against its source; at the end of the round
 * buf0 is verified against an independent golden snapshot of the
 * initial fill (the closed loop: four hops must return the original
 * data bit-exactly).  Hop checks localise WHICH hop failed; the loop
 * check catches anything that propagated.
 *
 * Design traps found in the user's draft (fixed, same family set):
 * pure-repetition copies replaced by the relay above; the local
 * store->load consistency pass (the compiler-forwarding fake, fifth
 * sighting today) removed; alignas(64) on std::vector (decorates the
 * stack object, not the heap data) removed; std::mt19937/
 * random_device floats -> framework random64 words with bitwise
 * compare; per-fail fprintf removed; .fracture_loop_count = -1;
 * _cleanup naming.
 * @endparblock
 */

#include "sandstone.h"
#include "arm_add_neon_copy.h"
using arm_add_neon::neon_copy;
#include <cstdint>
#include <cstring>
#include <vector>

static constexpr size_t FMB_BLOCK_SIZE = 4 * 1024;          /* 4 KiB      */
static constexpr size_t FMB_NUM_WORDS  = FMB_BLOCK_SIZE / 4; /* 1024       */
static constexpr size_t FMB_LINE       = 64;                /* bytes      */
static constexpr int    FMB_NUM_HOPS   = 4;                 /* relay ring */

/* ARM64 cache-line clean+invalidate (CLFLUSH equivalent) */
static inline void dc_civac(void *addr) {
    __asm__ volatile("dc civac, %0" :: "r"(addr) : "memory");
}

static inline void dsb_sy() {
    __asm__ volatile("dsb sy" ::: "memory");
}

/* full-buffer line eviction, drained */
static inline void flush_buffer(const void *buf, size_t size) {
    uintptr_t a = reinterpret_cast<uintptr_t>(buf) & ~(uintptr_t)(FMB_LINE - 1);
    const uintptr_t end = reinterpret_cast<uintptr_t>(buf) + size;
    for (; a < end; a += FMB_LINE)
        dc_civac(reinterpret_cast<void *>(a));
    dsb_sy();
}

static int flushed_memcpy_bis_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int flushed_memcpy_bis_cleanup(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int flushed_memcpy_bis_run(struct test *test, int cpu) {
    (void)cpu;

    std::vector<uint32_t> buf[FMB_NUM_HOPS];
    for (auto &b : buf)
        b.resize(FMB_NUM_WORDS);
    std::vector<uint32_t> golden(FMB_NUM_WORDS);   /* init-fill snapshot */

    uint64_t rounds = 0;

    do {
        /* 1. fresh random fill of buf0; golden = independent snapshot */
        for (size_t i = 0; i < FMB_NUM_WORDS; ++i) {
            buf[0][i] = (uint32_t)random64();
            golden[i] = buf[0][i];
        }

        /* 2. the relay: hop j copies buf[j] -> buf[(j+1) mod 4], with
         * the heavier bis cadence — FULL destination eviction + drain
         * after every hop, then hop-local verification */
        for (int hop = 0; hop < FMB_NUM_HOPS; ++hop) {
            const std::vector<uint32_t> &from = buf[hop];
            std::vector<uint32_t> &to = buf[(hop + 1) % FMB_NUM_HOPS];

            neon_copy(to.data(), from.data(), FMB_BLOCK_SIZE);
            flush_buffer(to.data(), FMB_BLOCK_SIZE);

            if (memcmp(to.data(), from.data(), FMB_BLOCK_SIZE) != 0) {
                for (size_t i = 0; i < FMB_NUM_WORDS; ++i) {
                    if (to[i] != from[i]) {
                        report_fail_msg("flushed_memcpy_bis_neon: hop %d "
                                        "mismatch at word %zu (round %lu): "
                                        "expected 0x%08x got 0x%08x",
                                        hop, i, (unsigned long)rounds,
                                        from[i], to[i]);
                        return EXIT_FAILURE;
                    }
                }
            }
        }

        /* 3. closed loop: four hops must return the original data */
        if (memcmp(buf[0].data(), golden.data(), FMB_BLOCK_SIZE) != 0) {
            for (size_t i = 0; i < FMB_NUM_WORDS; ++i) {
                if (buf[0][i] != golden[i]) {
                    report_fail_msg("flushed_memcpy_bis_neon: relay loop "
                                    "corruption at word %zu (round %lu): "
                                    "golden 0x%08x final 0x%08x — an error "
                                    "propagated through the hops",
                                    i, (unsigned long)rounds,
                                    golden[i], buf[0][i]);
                    return EXIT_FAILURE;
                }
            }
        }

        ++rounds;
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(flushed_memcpy_bis_neon, "Four-buffer rotating relay (4 real copies per round, each hop's destination fully dc-civac'd and drained) with hop-local and closed-loop bit verification (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = flushed_memcpy_bis_init,
    .test_run = flushed_memcpy_bis_run,
    .test_cleanup = flushed_memcpy_bis_cleanup,
    .fracture_loop_count = -1,   /* never fracture: fast semantic rounds */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
