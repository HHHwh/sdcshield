/*  barrier_after_lock_store_fp64 — FP64 datapath variant of
 *  tests/cpu/arm_add/barrier_after_lock_store.cpp (2026-11-10; parent
 *  untouched).
 *
 *  Same write-loss probe INSIDE the lock+barrier path, with the
 *  protected payload on the FP64 datapath:
 *    lock    : real ARM64 ldaxr/stxr exclusive pair — GPR bit-level,
 *              EXACTLY the parent's (the instructions are the test's
 *              subject; the architecture has no FP-register exclusive
 *              pair).
 *    section : plain FP-register `str` (str %d0) of expected+1.0 into
 *              the protected double — the in-section store now runs
 *              through the FP store port / FP register file.
 *    exit    : `dmb ish` then plain `str` of 0 to release (unchanged).
 *    check   : every lock acquisition must observe exactly `expected`
 *              (integer-valued doubles in the exact domain — numerical
 *              equality is the parent's bit-level check; no wrap below
 *              2^53).
 *  The advisory expected mirror is a std::atomic<double>; the
 *  excitation filler (arm_add_excite.h) is the parent's, untouched.
 *  Under -n 1 the single core hands the lock to itself: the full
 *  lock->FP store->dmb->release->reacquire->verify chain still runs.
 */
#include "sandstone.h"
#include <cstdint>
#include <atomic>
#include "../arm_add/arm_add_excite.h"

namespace { /* ODR: per-TU BALSData */
struct BALSData {
    alignas(64) uint64_t lock;        /* 0 = free, 1 = held — GPR subject */
    alignas(64) double protected_val; /* FP-datapath payload */
    alignas(64) std::atomic<double> expected; /* advisory mirror */
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

/* FP-datapath payload accesses: plain str/ldr on d registers (asm —
 * the parent's anti-forwarding discipline, re-typed to the FP port) */
static inline void bals_store_f64(double *addr, double val) {
    __asm__ volatile("str %d0, [%1]" :: "w"(val), "r"(addr) : "memory");
}

static inline double bals_load_f64(const double *addr) {
    double v;
    __asm__ volatile("ldr %d0, [%1]" : "=w"(v) : "r"(addr) : "memory");
    return v;
}

static inline uint64_t bals_load_u64(const uint64_t *addr) {
    uint64_t v;
    __asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static int barrier_after_lock_store_fp64_init(struct test *test) {
    auto *d = new BALSData;
    if (!d) return EXIT_FAILURE;
    d->lock = 0;
    d->protected_val = 0.0;
    d->expected.store(0.0, std::memory_order_relaxed);
    d->acquisitions.store(0, std::memory_order_relaxed);
    test->data = d;
    return EXIT_SUCCESS;
}

static int barrier_after_lock_store_fp64_cleanup(struct test *test) {
    delete static_cast<BALSData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int barrier_after_lock_store_fp64_run(struct test *test, int cpu) {
    auto *d = static_cast<BALSData *>(test->data);
    (void)cpu;

    uint64_t round = 0;
    do {
        ++round;
        /* bounded spin for the lock — a permanently-held lock is a FAIL,
         * not a hang (spinlock_add_neon watchdog precedent). */
        uint64_t spin = 0;
        while (bals_load_u64(&d->lock) != 0 && ++spin < BALS_SPIN_LIMIT) { }
        if (spin >= BALS_SPIN_LIMIT) {
            report_fail_msg("barrier_after_lock_store_fp64: lock never freed "
                            "after %lu spins", (unsigned long)BALS_SPIN_LIMIT);
        }

        bals_lock(&d->lock);

        /* the acquired view must match the expected handoff value
         * (integer-valued doubles: numerical == is exact) */
        double exp = d->expected.load(std::memory_order_relaxed);
        double cur = bals_load_f64(&d->protected_val);
        if (cur != exp) {
            bals_unlock(&d->lock);
        /* excitation filler: NEON switching power (see arm_add_excite.h) */
        if (!arm_add_excite::fill(cpu, round)) {
            report_fail_msg("excitation filler store->reload mismatch "
                            "(cpu %d round %lu)", cpu,
                            (unsigned long)round);
        }

            report_fail_msg("barrier_after_lock_store_fp64: protected value %f, "
                            "expected %f — previous holder's FP64 store was "
                            "lost inside the lock+barrier path",
                            cur, exp);
        }

        /* section: store the next handoff value, plain FP-register str */
        bals_store_f64(&d->protected_val, exp + 1.0);
        d->expected.store(exp + 1.0, std::memory_order_relaxed);
        d->acquisitions.fetch_add(1, std::memory_order_relaxed);

        bals_unlock(&d->lock);
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(barrier_after_lock_store_fp64, "FP64 variant of barrier_after_lock_store: write-loss inside the lock+barrier path with the payload on the FP datapath: ldaxr/stxr lock (GPR subject), in-section str %d0, dmb ish release — next holder must observe the stored value (ARM64)")
    .test_init = barrier_after_lock_store_fp64_init,
    .test_run = barrier_after_lock_store_fp64_run,
    .test_cleanup = barrier_after_lock_store_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture: our semantic rounds are microseconds; auto mode (40) ends the run in ~10 ms */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
