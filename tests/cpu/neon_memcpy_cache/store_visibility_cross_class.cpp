/*  store_visibility_cross_class — cross-coherency-domain store visibility
 *  probe.
 *
 *  Event-report lineage (user spec 2026-11-02): "rq->curr holds a precisely
 *  OLD value, other CPUs never observe the newest lock state" — a cross-
 *  domain write-visibility failure signature.  Probe design:
 *
 *    writer (cpu 0) : every round bumps each reader slot's version with a
 *                     plain `str`, then `dmb ish` (inner-shareable-domain
 *                     barrier) so the store is visible to every PE in the
 *                     shareability domain.
 *    readers        : after a nop-based delay (no clock, no I/O in the hot
 *                     path), read their slot TWICE — first WITHOUT any
 *                     barrier (r1), then after `dmb ish` (r2).
 *
 *  Fail criteria (either means the store never became visible in order):
 *    (1) r2 < r1  — the barrier-ordered read went BACKWARDS;
 *    (2) r1 or r2 < last_seen — the slot version regressed vs. the last
 *        round this reader observed (a precisely-old value surviving the
 *        barrier is exactly the reported signature).
 *
 *  -n 1: only the writer runs; it self-reads every slot right after the
 *  barrier (same-core layer must see the fresh value) so the single-thread
 *  path still exercises the store->barrier->load chain.
 */
#include "sandstone.h"
#include <cstdint>
#include <atomic>
#include "arm_add_excite.h"

#define SVC_MAX_THREADS 1024

namespace { /* ODR: per-TU SVCData (same-name struct hazard across TUs) */
struct SVCData {
    /* per-reader version slots; cacheline-aligned so writers on different
     * slots do not false-share the probe itself. */
    alignas(64) std::atomic<uint64_t> seq[SVC_MAX_THREADS];
};
}

static int store_visibility_cross_class_init(struct test *test) {
    auto *d = new SVCData;
    if (!d) return EXIT_FAILURE;
    for (int i = 0; i < SVC_MAX_THREADS; ++i)
        d->seq[i].store(0, std::memory_order_relaxed);
    test->data = d;
    return EXIT_SUCCESS;
}

static int store_visibility_cross_class_cleanup(struct test *test) {
    delete static_cast<SVCData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

/* Plain str + dmb ish: the instruction pair named in the spec.  The slot
 * store must be a plain `str` (not stlr) so visibility ordering rests
 * entirely on the explicit `dmb ish`. */
static inline void svc_store_seq(uint64_t *addr, uint64_t val) {
    __asm__ volatile("str %0, [%1]" :: "r"(val), "r"(addr) : "memory");
    __asm__ volatile("dmb ish" ::: "memory");
}

static inline uint64_t svc_load_seq(const uint64_t *addr) {
    uint64_t v;
    __asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static int store_visibility_cross_class_run(struct test *test, int cpu) {
    auto *d = static_cast<SVCData *>(test->data);

    if (cpu == 0) {
        /* writer: bump every slot, barrier, same-core sanity read-back */
        uint64_t round = 0;
        do {
            ++round;
            for (int r = 0; r < SVC_MAX_THREADS; ++r) {
                uint64_t v = d->seq[r].load(std::memory_order_relaxed) + 1;
                uint64_t *slot = reinterpret_cast<uint64_t *>(&d->seq[r]);
                svc_store_seq(slot, v);
                /* layer-1 (same-core) read-back: must observe v, not an
                 * older version — else the store was lost even locally. */
                uint64_t back = svc_load_seq(slot);
                if (back != v) {
                    report_fail_msg("store_visibility_cross_class: slot %d "
                                    "same-core read-back %lu, wrote %lu "
                                    "(lost local write)", r,
                                    (unsigned long)back, (unsigned long)v);
                }
            }
        } while (test_time_condition(test));
        /* excitation filler: NEON switching power (see arm_add_excite.h) */
        if (!arm_add_excite::fill(cpu, round)) {
            report_fail_msg("excitation filler store->reload mismatch (cpu %d round %lu)", cpu,
                            (unsigned long)round);
        }

    } else {
        /* reader: nop delay, unbarriered read, dmb ish, barriered read */
        uint64_t last_seen = 0;
        uint64_t round = 0;
        do {
            ++round;
            /* delay: pure nop spin, no clock / no syscall (C++20 forbids
             * volatile compound ops, so use a compiler barrier instead) */
            for (int i = 0; i < 200; ++i) { __asm__ volatile("" ::: "memory"); }

            uint64_t *slot = reinterpret_cast<uint64_t *>(&d->seq[cpu]);
            uint64_t r1 = svc_load_seq(slot);            /* no barrier */
            __asm__ volatile("dmb ish" ::: "memory");
            uint64_t r2 = svc_load_seq(slot);            /* after barrier */

            if (r2 < r1) {
                report_fail_msg("store_visibility_cross_class: cpu %d "
                                "barriered read %lu < unbarriered %lu "
                                "(visibility went backwards)",
                                cpu, (unsigned long)r2, (unsigned long)r1);
            }
            if (r1 < last_seen || r2 < last_seen) {
                report_fail_msg("store_visibility_cross_class: cpu %d "
                                "slot regressed: last_seen %lu, r1 %lu, "
                                "r2 %lu (precisely-old value survived)",
                                cpu, (unsigned long)last_seen,
                                (unsigned long)r1, (unsigned long)r2);
            }
            if (r2 > last_seen) last_seen = r2;
        } while (test_time_condition(test));
        /* excitation filler: NEON switching power (see arm_add_excite.h) */
        if (!arm_add_excite::fill(cpu, round)) {
            report_fail_msg("excitation filler store->reload mismatch (cpu %d round %lu)", cpu,
                            (unsigned long)round);
        }

    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(store_visibility_cross_class, "Cross-coherency-domain store visibility: writer str + dmb ish, remote readers check pre/post-barrier reads for precisely-old values (ARM64)")
    .test_init = store_visibility_cross_class_init,
    .test_run = store_visibility_cross_class_run,
    .test_cleanup = store_visibility_cross_class_cleanup,
    .fracture_loop_count = -1,   /* never fracture: our semantic rounds are microseconds; auto mode (40) ends the run in ~10 ms */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
