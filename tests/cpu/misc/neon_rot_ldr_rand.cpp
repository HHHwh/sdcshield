/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b neon_rot_ldr_rand
 * @parblock
 * Address-space-randomized variant of neon_rot_ldr_at_top_rowmajor_k7pattern.
 *
 * The k7pattern variant alternates ascending/descending sweeps to distinguish
 * "address-local" vs "forward-progress-position" corruption signatures. This
 * variant keeps the same NEON skeleton (3rd ldr at top + 2-src loads +
 * rotating ALU add/eor/and/orr + back-to-back str/str + memcmp cold path) and
 * the same alternating sweep direction, but randomizes WHERE in the process
 * address space each buffer lives.
 *
 * At init time we read /proc/meminfo (MemAvailable), mmap a single large
 * anonymous block BlockA whose size is a random fraction of the available
 * memory (capped at 25% to avoid OOM), then carve SIX buffers plus TWO write
 * buffers per worker thread out of BlockA at 64-byte-aligned offsets. All
 * buffers thus sit at randomized offsets inside one large mapping, exercising
 * a much larger slice of the address space than the original fixed-position
 * aligned_alloc buffers.
 *
 * CARVE EXCLUSIVITY (issue #339 fix). The original address-randomized case
 * carved every region independently, so temp/dst could overlap another
 * thread's dst, srcA/srcB or expected. Because all regions are 64-byte
 * aligned, such an overlap is at a non-zero element shift, and each thread
 * then stores its own index's result into a different buffer's element: the
 * owner of that dst reports a data miscompare that is pure software. With
 * -n 8 on this host it reproduced as 1-2 failing CPUs (the owner of the
 * clobbered dst); a write block overlapping srcA/srcB/expected fails the
 * whole slice. This version allocates every region from a disjoint set of
 * 16 KiB slots chosen by rejection from one seeded RNG stream at init (the
 * init-side stream is deterministic under -s), so no two regions can
 * overlap by construction. A layout audit runs at init and fails the test
 * loudly if that invariant is ever broken, instead of emitting a
 * hardware-looking data miscompare.
 *
 * The address-randomization RNG is seeded from the framework RNG (random64())
 * at init, so the layout is reproducible under a fixed -s seed. Note that the
 * per-thread run-time stream would NOT be a safe seed source: under the
 * AESSequence engine the generic seedThread() XORs the mixin into both AES
 * state words, so AESE's AddRoundKey cancels it on the first output and every
 * thread's first random64() is identical. Carving happens once at init, on
 * the single init-side stream, precisely to avoid that and to make the layout
 * independent of thread scheduling.
 *
 * If mmap cannot provide the requested block, the test falls back to an
 * aligned_alloc block just large enough for the disjoint layout (reduced
 * address spread, logged), and skips only if even that allocation fails. It
 * never runs a layout that cannot hold its regions.
 *
 * Diagnostic only: SDCSHIELD_NEON_ROT_LDR_RAND_BLOCK_MB=<n> pins the block
 * size to n MiB (used by the issue #339 verification to reproduce the old
 * defect at a small block; not used by normal runs).
 * @endparblock
 */

#include "sandstone.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(__aarch64__)
#  include <arm_neon.h>
#endif

#include <pthread.h>
#include <sys/mman.h>

#include <fstream>
#include <sstream>

#define NEON_ROT_LDR_RAND_COUNT 1024
#define NEON_ROT_LDR_RAND_ALIGN 64

/* ---- per-test address-randomization RNG (xorshift32, seeded from framework) ---- */
struct RandRng {
    uint32_t state;
    explicit RandRng(uint32_t s) : state(s ? s : 0x9E3779B9u) {}
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

enum RandRegionKind {
    rand_src_a,
    rand_src_b,
    rand_scratch,
    rand_expected,
    rand_temp,
    rand_dst,
};

struct RandRegion {
    uintptr_t start;
    size_t len;
    int owner;               /* thread number / cpu, -1 for init-time regions */
    RandRegionKind kind;
};

struct NeonRotLdrRandData {
    /* single large block from which all sub-blocks are carved */
    void     *big_block;
    size_t    big_block_size;
    bool      fallback_alloc;   /* big_block came from aligned_alloc(), not mmap() */

    /* carved sub-blocks (64-byte aligned, pairwise disjoint) */
    uint64x2_t *srcA;
    uint64x2_t *srcB;
    uint64x2_t *scratch;
    uint64x2_t *expected;
    uint64x2_t **thread_temp;   /* [nthreads] */
    uint64x2_t **thread_dst;    /* [nthreads] */
    int         nthreads;

    /* layout audit (invariant check; issue #339) */
    pthread_mutex_t lock;
    RandRegion     *regions;
    int             nregions;
    int             region_cap;
};

#if defined(__aarch64__)

static inline void store_vec(void *addr, uint64x2_t val) {
    vst1q_u64(static_cast<uint64_t *>(addr), val);
}

static inline uint64x2_t load_vec(const void *addr) {
    uint64x2_t res;
    __asm__ volatile ("ldr %q0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

#endif

static const char *region_kind_name(RandRegionKind kind) {
    switch (kind) {
        case rand_src_a:    return "srcA";
        case rand_src_b:    return "srcB";
        case rand_scratch:  return "scratch";
        case rand_expected: return "expected";
        case rand_temp:     return "temp";
        case rand_dst:      return "dst";
    }
    return "?";
}

/* A pair of overlapping regions is harmful when at least one of them is
 * written at run time and the other holds a value that must stay intact
 * (inputs / golden), or when two differently-indexed write regions alias
 * (each store then lands on the other buffer's wrong element). */
static bool region_pair_is_harmful(RandRegionKind a, RandRegionKind b, ptrdiff_t shift)
{
    const bool a_write = (a == rand_temp || a == rand_dst);
    const bool b_write = (b == rand_temp || b == rand_dst);

    if (a_write && (b == rand_src_a || b == rand_src_b || b == rand_expected))
        return true;
    if (b_write && (a == rand_src_a || a == rand_src_b || a == rand_expected))
        return true;
    if (a == rand_expected && (b == rand_src_a || b == rand_src_b))
        return true;
    if (b == rand_expected && (a == rand_src_a || a == rand_src_b))
        return true;
    if (a == rand_temp && b == rand_temp)
        return false;           /* temp is written but never read or compared */
    if (a_write && b_write)
        return shift != 0;      /* same address -> identical stores, otherwise wrong element */
    return false;               /* scratch is read-only and never compared */
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
 * Carve `count` pairwise-disjoint 16 KiB slot-aligned sub-blocks out of
 * [base, base+total) using rejection from a single seeded stream. Returns
 * false if the block cannot hold the requested number of slots.
 * This exclusivity is the issue #339 fix: independent carving let temp/dst
 * land inside another region and turned a layout collision into a
 * hardware-looking data miscompare.
 */
static bool carve_disjoint_slots(void *base, size_t total, size_t slot_size, int count,
                                 RandRng &rng, uint64x2_t **out, size_t *used)
{
    const size_t slot_count = total / slot_size;
    if (slot_count < (size_t)count)
        return false;

    const uint64_t attempt_cap = (uint64_t)slot_count * 64 + 65536;
    for (int i = 0; i < count; i++) {
        uint64_t idx = 0;
        bool found = false;
        for (uint64_t attempt = 0; attempt < attempt_cap; attempt++) {
            idx = rng.next64() % slot_count;
            bool duplicate = false;
            for (int j = 0; j < i; j++) {
                if (used[j] == idx) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
        used[i] = idx;
        out[i] = reinterpret_cast<uint64x2_t *>(static_cast<uint8_t *>(base) + idx * slot_size);
    }
    return true;
}

/*
 * Register a carved region and audit it against every region registered
 * before it. With exclusive slot allocation this must never report an
 * overlap; if it does, the test fails the layout as a software defect
 * (issue #339) instead of letting a data miscompare masquerade as SDC.
 * Returns true if this region forms a harmful overlap.
 */
static bool remember_region(NeonRotLdrRandData *data, const void *addr, size_t len,
                            int owner, RandRegionKind kind)
{
    RandRegion rec = { reinterpret_cast<uintptr_t>(addr), len, owner, kind };

    pthread_mutex_lock(&data->lock);
    if (data->nregions >= data->region_cap) {
        pthread_mutex_unlock(&data->lock);
        log_error("neon_rot_ldr_rand: region registry full (%d), layout audit incomplete",
                  data->region_cap);
        return false;
    }
    const int previous = data->nregions;
    data->regions[previous] = rec;      /* written under the lock; entries never move */
    data->nregions = previous + 1;
    pthread_mutex_unlock(&data->lock);

    bool harmful_overlap = false;
    for (int i = 0; i < previous; i++) {
        const RandRegion &other = data->regions[i];
        const uintptr_t lo = rec.start > other.start ? rec.start : other.start;
        const uintptr_t hi_end1 = rec.start + rec.len;
        const uintptr_t hi_end2 = other.start + other.len;
        const uintptr_t hi = hi_end1 < hi_end2 ? hi_end1 : hi_end2;
        if (lo >= hi)
            continue;

        const ptrdiff_t shift = (ptrdiff_t)rec.start - (ptrdiff_t)other.start;
        const bool harmful = region_pair_is_harmful(kind, other.kind, shift);
        log_warning("neon_rot_ldr_rand: REGION OVERLAP %s[%d]@%p vs %s[%d]@%p: %zu bytes, "
                    "shift=%td B (%td x 16 B) -> %s",
                    region_kind_name(kind), owner, addr,
                    region_kind_name(other.kind), other.owner, (void *)other.start,
                    (size_t)(hi - lo), shift, shift / (ptrdiff_t)sizeof(uint64x2_t),
                    harmful ? "HARMFUL (layout invariant violated)" : "harmless");
        harmful_overlap |= harmful;
    }
    return harmful_overlap;
}

/* Diagnostic block-size override, see the file comment. */
static size_t block_size_override_bytes() {
    const char *env = getenv("SDCSHIELD_NEON_ROT_LDR_RAND_BLOCK_MB");
    if (!env)
        return 0;
    char *end = nullptr;
    unsigned long long mb = strtoull(env, &end, 10);
    if (end == env || *end != '\0' || mb == 0)
        return 0;
    return (size_t)mb << 20;
}

static int neon_rot_ldr_rand_init(struct test *test) {
    /* Seed the address-randomization RNG from the framework RNG so the
     * layout is reproducible under a fixed -s seed. */
    uint32_t seed = static_cast<uint32_t>(random64());
    RandRng rng(seed);

    constexpr size_t per_buf = NEON_ROT_LDR_RAND_COUNT * sizeof(uint64x2_t);
    const int nthreads = thread_count();
    const int nregions = 4 + 2 * nthreads;

    /* The block must hold every region with room to spread the offsets. */
    const size_t required = (size_t)nregions * per_buf;
    const size_t min_block = required + (1u << 20);
    size_t avail = get_available_memory_bytes();
    size_t max_block = avail / 4;
    if (max_block < min_block) max_block = min_block;

    size_t big_size;
    const size_t override = block_size_override_bytes();
    if (override) {
        if (override < required) {
            log_skip(RuntimeSkipCategory,
                     "neon_rot_ldr_rand: block override %zu MiB cannot hold %d disjoint 16 KiB regions",
                     override >> 20, nregions);
            return EXIT_SKIP;
        }
        big_size = override;
        log_warning("neon_rot_ldr_rand: diagnostic override: big_block pinned to %zu MiB",
                    big_size >> 20);
    } else if (max_block <= min_block) {
        big_size = min_block;
    } else {
        size_t span = max_block - min_block;
        big_size = min_block + (size_t)rng.next64() % span;
    }
    const size_t page = 4096;
    big_size = (big_size + page - 1) & ~(page - 1);

    void *big = mmap(nullptr, big_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    bool fallback = false;
    if (big == MAP_FAILED) {
        /* Reduced address spread, but still a valid disjoint layout. */
        big = aligned_alloc(NEON_ROT_LDR_RAND_ALIGN, min_block);
        if (!big) {
            log_skip(RuntimeSkipCategory, "neon_rot_ldr_rand: failed to allocate big block");
            return EXIT_SKIP;
        }
        big_size = min_block;
        fallback = true;
        log_warning("neon_rot_ldr_rand: mmap failed, fell back to aligned_alloc(%zu B = %.1f MiB); "
                    "reduced address spread, layout still disjoint",
                    min_block, min_block / 1048576.0);
    }

    auto *data = static_cast<NeonRotLdrRandData *>(calloc(1, sizeof(NeonRotLdrRandData)));
    uint64x2_t **slots = static_cast<uint64x2_t **>(calloc((size_t)nregions, sizeof(uint64x2_t *)));
    size_t *used = static_cast<size_t *>(calloc((size_t)nregions, sizeof(size_t)));
    if (data) {
        data->thread_temp = static_cast<uint64x2_t **>(calloc((size_t)nthreads, sizeof(uint64x2_t *)));
        data->thread_dst = static_cast<uint64x2_t **>(calloc((size_t)nthreads, sizeof(uint64x2_t *)));
        data->regions = static_cast<RandRegion *>(calloc((size_t)nregions, sizeof(RandRegion)));
    }
    if (!data || !slots || !used || !data->thread_temp || !data->thread_dst || !data->regions) {
        if (fallback) free(big); else munmap(big, big_size);
        if (data) {
            free(data->thread_temp);
            free(data->thread_dst);
            free(data->regions);
        }
        free(data); free(slots); free(used);
        log_skip(RuntimeSkipCategory, "neon_rot_ldr_rand: failed to allocate test data");
        return EXIT_SKIP;
    }

    data->big_block = big;
    data->big_block_size = big_size;
    data->fallback_alloc = fallback;
    data->nthreads = nthreads;
    data->nregions = 0;
    data->region_cap = nregions;
    pthread_mutex_init(&data->lock, nullptr);

    log_info("neon_rot_ldr_rand: big_block=%p size=%zu MiB (%s), threads=%d, regions=%d",
             big, big_size >> 20, fallback ? "aligned_alloc fallback" : "mmap",
             nthreads, nregions);

    if (!carve_disjoint_slots(big, big_size, per_buf, nregions, rng, slots, used)) {
        log_skip(RuntimeSkipCategory,
                 "neon_rot_ldr_rand: block of %zu B cannot hold %d disjoint 16 KiB regions",
                 big_size, nregions);
        if (fallback) free(big); else munmap(big, big_size);
        free(slots); free(used); free(data->thread_temp); free(data->thread_dst);
        free(data->regions); pthread_mutex_destroy(&data->lock); free(data);
        return EXIT_SKIP;
    }

    data->srcA     = slots[0];
    data->srcB     = slots[1];
    data->scratch  = slots[2];
    data->expected = slots[3];
    for (int i = 0; i < nthreads; i++) {
        data->thread_temp[i] = slots[4 + 2 * i];
        data->thread_dst[i]  = slots[5 + 2 * i];
    }

    /* Layout invariant: with exclusive slots nothing may overlap. */
    bool layout_bug = false;
    layout_bug |= remember_region(data, data->srcA, per_buf, -1, rand_src_a);
    layout_bug |= remember_region(data, data->srcB, per_buf, -1, rand_src_b);
    layout_bug |= remember_region(data, data->scratch, per_buf, -1, rand_scratch);
    layout_bug |= remember_region(data, data->expected, per_buf, -1, rand_expected);
    for (int i = 0; i < nthreads; i++) {
        layout_bug |= remember_region(data, data->thread_temp[i], per_buf, i, rand_temp);
        layout_bug |= remember_region(data, data->thread_dst[i], per_buf, i, rand_dst);
    }
    if (layout_bug) {
        log_error("neon_rot_ldr_rand: carve produced overlapping regions despite exclusive slots");
        if (fallback) free(big); else munmap(big, big_size);
        free(slots); free(used); free(data->thread_temp); free(data->thread_dst);
        free(data->regions); pthread_mutex_destroy(&data->lock); free(data);
        return EXIT_FAILURE;
    }
    free(slots);
    free(used);

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
        for (int e = 0; e < 2 * NEON_ROT_LDR_RAND_COUNT; e++) {
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

static int neon_rot_ldr_rand_run(struct test *test, int cpu) {
    auto *data = static_cast<NeonRotLdrRandData *>(test->data);
    constexpr size_t per_buf = NEON_ROT_LDR_RAND_COUNT * sizeof(uint64x2_t);

    if (cpu < 0 || cpu >= data->nthreads) {
        report_fail_msg("neon_rot_ldr_rand: cpu index %d outside the %d pre-assigned slots",
                        cpu, data->nthreads);
    }
    uint64x2_t *temp = data->thread_temp[cpu];
    uint64x2_t *dst  = data->thread_dst[cpu];
    log_info("neon_rot_ldr_rand: thread %d temp=%p dst=%p", cpu, (void *)temp, (void *)dst);

    uint64_t sweep = 0;
    TEST_LOOP(test, 1 << 13) {
        const bool ascending = (sweep & 1) == 0;
        for (int n = 0; n < NEON_ROT_LDR_RAND_COUNT; n++) {
            const int i = ascending ? n : (NEON_ROT_LDR_RAND_COUNT - 1 - n);

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

        /* memcmp_or_fail reports the first offset, its address and the lane
         * histogram -- the data needed to attribute a failure. */
        memcmp_or_fail(reinterpret_cast<const uint64_t *>(dst),
                       reinterpret_cast<const uint64_t *>(data->expected),
                       per_buf / sizeof(uint64_t),
                       "neon_rot_ldr_rand dst vs expected");
    }

    return EXIT_SUCCESS;
}

static int neon_rot_ldr_rand_cleanup(struct test *test) {
    auto *data = static_cast<NeonRotLdrRandData *>(test->data);
    if (data) {
        if (data->big_block) {
            if (data->fallback_alloc)
                free(data->big_block);
            else
                munmap(data->big_block, data->big_block_size);
        }
        free(data->thread_temp);
        free(data->thread_dst);
        free(data->regions);
        pthread_mutex_destroy(&data->lock);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(neon_rot_ldr_rand, "Address-space-randomized ldr_at_top: big block with exclusive random 16 KiB slots per buffer (issue #339 fix: no carve overlap)")
    .test_init    = neon_rot_ldr_rand_init,
    .test_run     = neon_rot_ldr_rand_run,
    .test_cleanup = neon_rot_ldr_rand_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
