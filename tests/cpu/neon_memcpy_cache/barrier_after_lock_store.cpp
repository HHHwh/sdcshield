/*  barrier_after_lock_store — write-loss probe INSIDE the lock+barrier
 *  path.
 *
 *  Event-report lineage: "the code had both lock protection and memory
 *  barriers, so software concurrency is ruled out" — the fault sits
 *  somewhere inside the locked+barriered path itself.  Probe design:
 *
 *    lock    : real ARM64 ldaxr/stxr exclusive pair (inline asm — the
 *              instructions named in the spec, NOT a compiler-generated
 *              __atomic which may emit LSE casal instead).
 *    section : plain `str` of expected+1 into the protected variable.
 *    exit    : `dmb ish` then plain `str` of 0 to release — the barrier
 *              guarantees the in-section store is visible to the next
 *              lock holder BEFORE the lock reads as free.
 *    check   : every lock acquisition must observe exactly `expected`
 *              (the value the previous holder stored).  Observing
 *              expected-1 = the previous holder's write vanished despite
 *              lock + barrier -> FAIL (the reported signature).
 *
 *  Handoff is round-robin over ALL cores (any core may take the lock
 *  next), so every acquisition is potentially a cross-core handoff; the
 *  expected value lives in a std::atomic only because it is advisory
 *  bookkeeping (the actual protected variable is plain str/ldr via asm).
 *  Under -n 1 the single core hands the lock to itself: the full
 *  lock->store->dmb->release->reacquire->verify chain still runs.
 */
#include "sandstone.h"
#include <cstdint>
#include <atomic>
#include "arm_add_excite.h"

namespace { /* ODR: per-TU BALSData */
struct BALSData {
    alignas(64) uint64_t lock;        /* 0 = free, 1 = held */
    alignas(64) uint64_t protected_val;
    alignas(64) std::atomic<uint64_t> expected; /* advisory mirror */
    std::atomic<uint64_t> acquisitions;
};
}

#define BALS_SPIN_LIMIT 200000000ULL

/* ldaxr/stxr spinlock: acquire semantics via ldaxr; release below. */
static inline void bals_lock(uint64_t *lock) {
    uint64_t tmp;
    uint64_t res;
    __asm__ volatile(
        "1:  ldaxr  %0, [%2]\n\t"
        "    cbnz   %0, 1b\n\t"
        "    stxr   %w1, %3, [%2]\n\t"
        "    cbnz   %w1, 1b\n\t"
        : "=&r"(tmp), "=&r"(res)
        : "r"(lock), "r"(1UL)
        : "memory");
}

/* release: barrier THEN free — section stores become visible before the
 * lock word reads 0. */
static inline void bals_unlock(uint64_t *lock) {
    __asm__ volatile("dmb ish" ::: "memory");
    __asm__ volatile("str %0, [%1]" :: "r"(0UL), "r"(lock) : "memory");
}

static inline void bals_store(uint64_t *addr, uint64_t val) {
    __asm__ volatile("str %0, [%1]" :: "r"(val), "r"(addr) : "memory");
}

static inline uint64_t bals_load(const uint64_t *addr) {
    uint64_t v;
    __asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static int barrier_after_lock_store_init(struct test *test) {
    auto *d = new BALSData;
    if (!d) return EXIT_FAILURE;
    d->lock = 0;
    d->protected_val = 0;
    d->expected.store(0, std::memory_order_relaxed);
    d->acquisitions.store(0, std::memory_order_relaxed);
    test->data = d;
    return EXIT_SUCCESS;
}

static int barrier_after_lock_store_cleanup(struct test *test) {
    delete static_cast<BALSData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int barrier_after_lock_store_run(struct test *test, int cpu) {
    auto *d = static_cast<BALSData *>(test->data);
    (void)cpu;

    uint64_t round = 0;
    do {
        ++round;
        /* bounded spin for the lock — a permanently-held lock is a FAIL,
         * not a hang (spinlock_add_neon watchdog precedent). */
        uint64_t spin = 0;
        while (bals_load(&d->lock) != 0 && ++spin < BALS_SPIN_LIMIT) { }
        if (spin >= BALS_SPIN_LIMIT) {
            report_fail_msg("barrier_after_lock_store: lock never freed "
                            "after %lu spins", (unsigned long)BALS_SPIN_LIMIT);
        }

        bals_lock(&d->lock);

        /* the acquired view must match the expected handoff value */
        uint64_t exp = d->expected.load(std::memory_order_relaxed);
        uint64_t cur = bals_load(&d->protected_val);
        if (cur != exp) {
            bals_unlock(&d->lock);
        /* excitation filler: NEON switching power (see arm_add_excite.h) */
        if (!arm_add_excite::fill(cpu, round)) {
            report_fail_msg("excitation filler store->reload mismatch "
                            "(cpu %d round %lu)", cpu,
                            (unsigned long)round);
        }

            report_fail_msg("barrier_after_lock_store: protected value %lu, "
                            "expected %lu — previous holder's store was "
                            "lost inside the lock+barrier path",
                            (unsigned long)cur, (unsigned long)exp);
        }

        /* section: store the next handoff value, plain str */
        bals_store(&d->protected_val, exp + 1);
        d->expected.store(exp + 1, std::memory_order_relaxed);
        d->acquisitions.fetch_add(1, std::memory_order_relaxed);

        bals_unlock(&d->lock);
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(barrier_after_lock_store, "Write-loss inside the lock+barrier path: ldaxr/stxr lock, in-section str, dmb ish release — next holder must observe the stored value (ARM64)")
    .test_init = barrier_after_lock_store_init,
    .test_run = barrier_after_lock_store_run,
    .test_cleanup = barrier_after_lock_store_cleanup,
    .fracture_loop_count = -1,   /* never fracture: our semantic rounds are microseconds; auto mode (40) ends the run in ~10 ms */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
