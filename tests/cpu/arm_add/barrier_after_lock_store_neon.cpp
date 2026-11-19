/*  barrier_after_lock_store_neon — NEON variant of
 *  tests/cpu/arm_add/barrier_after_lock_store.cpp (original
 *  untouched).
 *
 *  The lock stays the SPEC'S ldaxr/stxr exclusive pair (acquire/
 *  release semantics have no NEON equivalent and ARE the protocol);
 *  what moves to the NEON datapath is the value UNDER the lock: a
 *  uint32x4 handoff vector written with asm st_vec (intrinsics get
 *  GCC-forwarded) inside the critical section, read back with ld_vec
 *  by the next holder.  Every acquisition must observe exactly the
 *  expected epoch in ALL FOUR LANES — a partial-lane mismatch is a
 *  partially-landed NEON store inside the lock+barrier path, the
 *  reported signature.
 *
 *  -n 1: the single core hands the lock to itself — the full
 *  lock -> st_vec -> dmb ish -> release -> reacquire -> ld_vec chain.
 */
#include "sandstone.h"
#include <cstdint>
#include <atomic>
#include <arm_neon.h>

namespace { /* ODR: per-TU BALSNData */
struct BALSNData {
    alignas(64) uint64_t lock;              /* 0 = free, 1 = held (scalar protocol) */
    alignas(64) uint32_t protected_val[4];  /* the NEON handoff vector */
    alignas(64) std::atomic<uint64_t> expected;   /* advisory epoch mirror */
    std::atomic<uint64_t> acquisitions;
};
}

#define BALSN_SPIN_LIMIT 200000000ULL

static inline uint32x4_t ld_vec(const uint32_t *a) {
    uint32x4_t v;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(v) : "r"(a) : "memory");
    return v;
}
static inline void st_vec(uint32_t *a, uint32x4_t v) {
    __asm__ volatile("str %q0, [%1]" :: "w"(v), "r"(a) : "memory");
}

/* ldaxr/stxr spinlock — the spec's instructions, unchanged */
static inline void balsn_lock(uint64_t *lock) {
    uint64_t tmp, res;
    __asm__ volatile(
        "1:  ldaxr  %0, [%2]\n\t"
        "    cbnz   %0, 1b\n\t"
        "    stxr   %w1, %3, [%2]\n\t"
        "    cbnz   %w1, 1b\n\t"
        : "=&r"(tmp), "=&r"(res)
        : "r"(lock), "r"(1UL)
        : "memory");
}

/* release: barrier THEN free (section stores visible before lock=0) */
static inline void balsn_unlock(uint64_t *lock) {
    __asm__ volatile("dmb ish" ::: "memory");
    __asm__ volatile("str %0, [%1]" :: "r"(0UL), "r"(lock) : "memory");
}

static inline uint64_t balsn_load_qword(const uint64_t *addr) {
    uint64_t v;
    __asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static int barrier_after_lock_store_neon_init(struct test *test) {
    auto *d = new BALSNData;
    if (!d) return EXIT_FAILURE;
    d->lock = 0;
    for (int l = 0; l < 4; ++l) d->protected_val[l] = 0;
    d->expected.store(0, std::memory_order_relaxed);
    d->acquisitions.store(0, std::memory_order_relaxed);
    test->data = d;
    return EXIT_SUCCESS;
}

static int barrier_after_lock_store_neon_cleanup(struct test *test) {
    delete static_cast<BALSNData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int barrier_after_lock_store_neon_run(struct test *test, int cpu) {
    auto *d = static_cast<BALSNData *>(test->data);
    (void)cpu;

    do {
        uint64_t spin = 0;
        while (balsn_load_qword(&d->lock) != 0 && ++spin < BALSN_SPIN_LIMIT) { }
        if (spin >= BALSN_SPIN_LIMIT) {
            report_fail_msg("barrier_after_lock_store_neon: lock never "
                            "freed after %lu spins",
                            (unsigned long)BALSN_SPIN_LIMIT);
        }

        balsn_lock(&d->lock);

        uint64_t exp = d->expected.load(std::memory_order_relaxed);
        uint32x4_t cur = ld_vec(d->protected_val);
        uint32x4_t want = vdupq_n_u32((uint32_t)exp);
        uint32x4_t diff = veorq_u32(cur, want);
        if (vmaxvq_u32(diff) != 0) {
            balsn_unlock(&d->lock);
            report_fail_msg("barrier_after_lock_store_neon: handoff vector "
                            "mismatch: expected epoch %lu in all lanes, got "
                            "{%u,%u,%u,%u} — NEON store lost inside the "
                            "lock+barrier path", (unsigned long)exp,
                            vgetq_lane_u32(cur, 0), vgetq_lane_u32(cur, 1),
                            vgetq_lane_u32(cur, 2), vgetq_lane_u32(cur, 3));
        }

        /* section: store the next handoff epoch as a NEON vector */
        st_vec(d->protected_val, vdupq_n_u32((uint32_t)(exp + 1)));
        d->expected.store(exp + 1, std::memory_order_relaxed);
        d->acquisitions.fetch_add(1, std::memory_order_relaxed);

        balsn_unlock(&d->lock);
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(barrier_after_lock_store_neon, "Write-loss inside the lock+barrier path on the NEON datapath: ldaxr/stxr lock, in-section asm vst epoch vector, dmb ish release — next holder must observe the epoch in all four lanes (ARM64)")
    .test_init = barrier_after_lock_store_neon_init,
    .test_run = barrier_after_lock_store_neon_run,
    .test_cleanup = barrier_after_lock_store_neon_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
