/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_ldr_rand_thread
 * @parblock
 * Per-thread-exclusive random-address variant of
 * neon_rot_ldr_at_top_rowmajor_k7pattern.  Renamed + fixed from the
 * board-local neon_rot_ldr_wangxu draft (issue #339): the draft carved ALL
 * buffers — init's srcA/srcB/scratch/expected AND every thread's temp/dst
 * — from one big mmap pool with INDEPENDENT random offsets and no overlap
 * detection.  124 threads x 2 + 4 blocks of 16 KiB in one pool collide
 * with probability ~38% (1 GiB pool) up to ~100% (<=64 MiB pool), and any
 * overlap between a thread's store target and expected/srcA makes the
 * test corrupt its own golden / inputs and report a FALSE SDC.
 *
 * Fix: keep the random-address goal but make the carve EXCLUSIVE by
 * construction.  The big block is partitioned into disjoint zones: zone 0
 * holds the four init buffers, zone 1+cpu is worker cpu's private
 * territory for its temp/dst.  Inside a zone the buffers sit at random
 * offsets within fixed sub-zones, so the address spread survives, but a
 * store can structurally never land in another thread's memory or in the
 * golden — no matter how unlucky the RNG gets.
 *
 * Keeps the k7pattern NEON skeleton: 3rd ldr at top + 2-src loads +
 * rotating add/eor/and/orr + back-to-back str/str + memcmp cold path,
 * alternating ascending/descending sweep direction.  Address offsets are
 * seeded from the framework RNG (random64()), so a fixed -s seed keeps
 * the whole layout reproducible.
 * @endparblock
 */

#include "sandstone.h"

#include <cstdint>
#include <cstring>

#if defined(__aarch64__)
#  include <arm_neon.h>
#endif

#include <sys/mman.h>
#include <fstream>
#include <sstream>

#define NEON_ROT_LDR_RAND_THREAD_COUNT 1024
#define NEON_ROT_LDR_RAND_THREAD_ALIGN 64

/* ---- per-test address-randomization RNG (xorshift32, seeded from framework) ---- */
struct RandThreadRng {
    uint32_t state;
    explicit RandThreadRng(uint32_t s) : state(s ? s : 0x9E3779B9u) {}
    uint32_t next() {
        uint32_t x = state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state = x;
        return x;
    }
    uint64_t next64() {
        return (uint64_t)next() | ((uint64_t)next() << 32);
    }
};

struct NeonRotLdrRandThreadData {
    /* single large block, partitioned into exclusive zones */
    void     *big_block;
    size_t    big_block_size;
    size_t    init_zone_size;   /* zone 0 = [big, big+init_zone_size) */
    size_t    thread_zone_size; /* zone 1+cpu = [big+init_zone_size+cpu*tzs, +tzs) */
    int       thread_count;
    bool      from_malloc;      /* big_block came from aligned_alloc, not mmap */
    /* carved init buffers (64-byte aligned, each in its own sub-zone) */
    uint64x2_t *srcA;
    uint64x2_t *srcB;
    uint64x2_t *scratch;
    uint64x2_t *expected;
};

static inline void store_vec(void *addr, uint64x2_t val) {
    vst1q_u64(static_cast<uint64_t *>(addr), val);
}

static inline uint64x2_t load_vec(const void *addr) {
    uint64x2_t res;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

/* Read /proc/meminfo MemAvailable (bytes). Fall back to 1 GiB on parse failure. */
static size_t get_available_memory_bytes() {
    std::ifstream f("/proc/meminfo");
    std::string line;
    size_t avail_kb = 0;
    while (std::getline(f, line)) {
        if (line.rfind("MemAvailable:", 0) == 0) {
            std::istringstream iss(line.substr(13));
            iss >> avail_kb;
            break;
        }
    }
    if (avail_kb == 0) avail_kb = 1024 * 1024;  /* 1 GiB fallback */
    return avail_kb * 1024ull;
}

/*
 * Carve a 64-byte-aligned `bytes`-sized sub-block out of sub-zone
 * `sub_index` of `sub_count` equal fixed sub-zones inside
 * [zone_base, zone_base + zone_size).  Sub-zones are disjoint by
 * construction; the block's offset within its sub-zone is random.  With
 * zone_size a multiple of 64 and sub_count in {2,4}, the sub-zone size is
 * also a multiple of 64, so the aligned-up start can never cross the
 * sub-zone's upper bound (bytes > ALIGN-1).  Overlap between buffers in
 * DIFFERENT sub-zones or zones is structurally impossible.
 */
static uint64x2_t *carve_in_sub_zone(void *zone_base, size_t zone_size,
                                     int sub_index, int sub_count,
                                     size_t bytes, RandThreadRng &rng) {
    const size_t sub = zone_size / sub_count;      /* >= bytes + ALIGN by min_block */
    const size_t sub_off = sub_index * sub;
    const size_t slack = sub - bytes;              /* >= ALIGN > 0 */
    const size_t off = sub_off + (size_t)(rng.next64() % (slack + 1));
    uintptr_t addr = reinterpret_cast<uintptr_t>(zone_base) + off;
    addr = (addr + NEON_ROT_LDR_RAND_THREAD_ALIGN - 1) &
           ~static_cast<uintptr_t>(NEON_ROT_LDR_RAND_THREAD_ALIGN - 1);
    return reinterpret_cast<uint64x2_t *>(addr);
}

static int neon_rot_ldr_rand_thread_init(struct test *test) {
    /* Seed the address-randomization RNG from the framework RNG so the
     * whole zone layout is reproducible under a fixed -s seed. */
    uint32_t seed = static_cast<uint32_t>(random64());
    RandThreadRng rng(seed);

    constexpr size_t per_buf = NEON_ROT_LDR_RAND_THREAD_COUNT * sizeof(uint64x2_t);
    /* Exclusive-zone floor sizes (both multiples of 64). */
    constexpr size_t init_zone_min   = 4 * (per_buf + NEON_ROT_LDR_RAND_THREAD_ALIGN);
    constexpr size_t thread_zone_min = 2 * (per_buf + NEON_ROT_LDR_RAND_THREAD_ALIGN);

    const int n_threads = thread_count();
    size_t min_block = init_zone_min +
                       static_cast<size_t>(n_threads) * thread_zone_min;
    min_block = (min_block + NEON_ROT_LDR_RAND_THREAD_ALIGN - 1) &
                ~static_cast<size_t>(NEON_ROT_LDR_RAND_THREAD_ALIGN - 1);

    /* Random big-block size in [min_block, max_block] to keep the
     * whole-pool VA spread random; capped at 25% of MemAvailable. */
    size_t avail = get_available_memory_bytes();
    size_t max_block = std::max(avail / 4, min_block);
    size_t big_size;
    if (max_block <= min_block) {
        big_size = min_block;
    } else {
        size_t span = max_block - min_block;
        size_t rnd = (size_t)rng.next64() % span;
        big_size = min_block + rnd;
    }
    const size_t page = 4096;
    big_size = (big_size + page - 1) & ~(page - 1);
    if (big_size < min_block) big_size = min_block;  /* page round-up can't shrink, belt+braces */

    void *big = mmap(nullptr, big_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    bool from_malloc = false;
    if (big == MAP_FAILED) {
        /* Fall back to a modest aligned_alloc big enough for the full
         * zone layout; cleanup must free() it (issue #339's munmap-UB). */
        big = aligned_alloc(NEON_ROT_LDR_RAND_THREAD_ALIGN, min_block);
        if (!big) {
            report_fail_msg("neon_rot_ldr_rand_thread: failed to allocate big block");
            return EXIT_SKIP;
        }
        from_malloc = true;
        big_size = min_block;
    }

    auto *data = static_cast<NeonRotLdrRandThreadData *>(malloc(sizeof(NeonRotLdrRandThreadData)));
    data->big_block = big;
    data->big_block_size = big_size;
    data->thread_count = n_threads;
    data->from_malloc = from_malloc;

    /* Zone 0 = init zone (fixed floor size); zones 1..n = per-thread zones
     * sharing the rest equally.  min_block guarantees every thread zone
     * >= thread_zone_min (both sides are multiples of 64, so the
     * round-down cannot undercut the floor). */
    data->init_zone_size = init_zone_min;
    data->thread_zone_size =
        ((big_size - init_zone_min) / static_cast<size_t>(n_threads)) &
        ~static_cast<size_t>(NEON_ROT_LDR_RAND_THREAD_ALIGN - 1);

    data->srcA     = carve_in_sub_zone(big, init_zone_min, 0, 4, per_buf, rng);
    data->srcB     = carve_in_sub_zone(big, init_zone_min, 1, 4, per_buf, rng);
    data->scratch  = carve_in_sub_zone(big, init_zone_min, 2, 4, per_buf, rng);
    data->expected = carve_in_sub_zone(big, init_zone_min, 3, 4, per_buf, rng);

    memset_random(data->srcA, per_buf);
    memset_random(data->srcB, per_buf);
    memset_random(data->scratch, per_buf);

    /* golden (issue #373): scalar independent recomputation — pure C
     * integer ops on the scalar pipeline, never the NEON units under
     * test, so a persistent NEON datapath fault cannot corrupt golden
     * and actual identically. Phase = vector index % 4 (2 u64 lanes per
     * vector), matching the run loop's per-vector switch. PR #412
     * leftover found by the second baton's marker sweep. */
    {
        const uint64_t *sa = reinterpret_cast<const uint64_t *>(data->srcA);
        const uint64_t *sb = reinterpret_cast<const uint64_t *>(data->srcB);
        uint64_t       *ex = reinterpret_cast<uint64_t *>(data->expected);
        for (int e = 0; e < 2 * NEON_ROT_LDR_RAND_THREAD_COUNT; e++) {
            switch ((e / 2) % 4) {
                case 0: ex[e] = sa[e] + sb[e]; break;
                case 1: ex[e] = sa[e] ^ sb[e]; break;
                case 2: ex[e] = sa[e] & sb[e]; break;
                case 3: ex[e] = sa[e] | sb[e]; break;
            }
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int neon_rot_ldr_rand_thread_run(struct test *test, int cpu) {
    auto *data = static_cast<NeonRotLdrRandThreadData *>(test->data);
    constexpr size_t per_buf = NEON_ROT_LDR_RAND_THREAD_COUNT * sizeof(uint64x2_t);

    /* Per-thread RNG seed: reproducible under a fixed -s seed (the
     * framework seeds each worker's random64() stream deterministically),
     * fresh and private per thread. */
    uint32_t seed = static_cast<uint32_t>(random64());
    RandThreadRng rng(seed);

    /* This worker's EXCLUSIVE zone: no other thread's store can reach it,
     * and it cannot reach the init zone or any other thread's zone. */
    char *zone_base = static_cast<char *>(data->big_block) +
                      data->init_zone_size +
                      static_cast<size_t>(cpu) * data->thread_zone_size;

    uint64x2_t *temp = carve_in_sub_zone(zone_base, data->thread_zone_size, 0, 2, per_buf, rng);
    uint64x2_t *dst  = carve_in_sub_zone(zone_base, data->thread_zone_size, 1, 2, per_buf, rng);

    uint64_t sweep = 0;
    TEST_LOOP(test, 1 << 13) {
        const bool ascending = (sweep & 1) == 0;
        for (int n = 0; n < NEON_ROT_LDR_RAND_THREAD_COUNT; n++) {
            const int i = ascending ? n : (NEON_ROT_LDR_RAND_THREAD_COUNT - 1 - n);

            uint64x2_t unused_x = load_vec(&data->scratch[i]);
            (void)unused_x;

            uint64x2_t a = load_vec(&data->srcA[i]);
            uint64x2_t b = load_vec(&data->srcB[i]);
            uint64x2_t res;

            switch (i % 4) {
                case 0: res = vaddq_u64(a, b); break;
                case 1: res = veorq_u64(a, b); break;
                case 2: res = vandq_u64(a, b); break;
                case 3: res = vorrq_u64(a, b); break;
            }

            store_vec(&temp[i], res);
            store_vec(&dst[i], res);
        }
        sweep++;

        if (memcmp(dst, data->expected, per_buf) != 0) {
            report_fail_msg("neon_rot_ldr_rand_thread data miscompare");
        }
    }

    /* temp/dst live in this thread's exclusive zone of the big block;
     * the block itself is released in cleanup. */
    return EXIT_SUCCESS;
}

static int neon_rot_ldr_rand_thread_cleanup(struct test *test) {
    auto *data = static_cast<NeonRotLdrRandThreadData *>(test->data);
    if (data) {
        if (data->big_block) {
            /* Free by allocation origin — munmap on an aligned_alloc
             * pointer is UB (issue #339 secondary bug). */
            if (data->from_malloc)
                free(data->big_block);
            else
                munmap(data->big_block, data->big_block_size);
        }
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_ldr_rand_thread, "Per-thread-exclusive random-address ldr_at_top: big mmap block partitioned into disjoint init/thread zones, random carve inside each — address spread kept, golden/inter-thread overlap structurally impossible")
    .test_init    = neon_rot_ldr_rand_thread_init,
    .test_run     = neon_rot_ldr_rand_thread_run,
    .test_cleanup = neon_rot_ldr_rand_thread_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
