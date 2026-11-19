/*  store_lost_write_detect_fp64 — FP64 datapath variant of
 *  tests/cpu/arm_add/store_lost_write_detect.cpp (2026-11-10; parent
 *  untouched).  Sentinel ladder chassis (same-core read-back,
 *  release/acquire publication, dsb re-read) kept verbatim; the
 *  sentinel payload runs the FP-register datapath (str/ldr %d0,
 *  integer-valued epoch doubles in the exact domain).
 *  lost-write sentinel with a three-layer
 *  read-back ladder.
 *
 *  Event-report lineage: "the str instruction failed, the assignment never
 *  landed; the OLD value survives intact" — a lost write, not a torn one.
 *  Probe design (per core, symmetric; partner = cpu ^ 1):
 *
 *    write   : plain `str` of the monotonically increasing epoch into the
 *              core's own sentinel slot (fresh value each round so a
 *              lost write cannot hide behind a golden fold — issue #374).
 *    layer 1 : same-core immediate `ldr` read-back — must equal the epoch
 *              just stored (a miss here is a locally lost write).
 *    layer 2 : cross-core `ldr` of the PARTNER's slot — the spec's
 *              remote read with no barrier in between.
 *    layer 3 : partner-slot re-read after `dsb ish` (completes all
 *              memory access before the load) — still-old here means the
 *              write is lost even past a full barrier.
 *
 *  Ordering rationale (wait-free, deadlock-free): the writer publishes
 *  its epoch with written.store(epoch, release) AFTER the asm str of the
 *  sentinel; a reader that acquires the published epoch is therefore
 *  guaranteed the sentinel store has already happened — an ldr still
 *  returning an older value is a genuine visibility failure, not a race
 *  window.  The first draft used a bidirectional ack handshake (writer
 *  waits for the partner to ack before doing its own read) — a textbook
 *  deadlock: both cores' reads sat behind their own waits.  The release/
 *  acquire publication removes every wait while keeping the fail
 *  criteria exactly as strong.
 *
 *  Under -n 1 no partner ever publishes (written[partner] stays 0), so
 *  layers 2/3 run same-core in sequence — the whole ladder still runs.
 */
#include "sandstone.h"
#include <cstdint>
#include <atomic>
#include "../arm_add/arm_add_excite.h"

#define SLD_MAX_THREADS 1024

namespace { /* ODR: per-TU SLDData */
struct SLDData {
    /* per-core sentinel slot: plain str/ldr via asm (NOT std::atomic, so
     * the compiler cannot substitute ldar/stlr and reorder the ladder) */
    alignas(64) double sentinel[SLD_MAX_THREADS];
    alignas(64) std::atomic<uint64_t> written[SLD_MAX_THREADS]; /* epoch published (release) */
    alignas(64) std::atomic<uint64_t> started[SLD_MAX_THREADS]; /* worker up */
};
}

static int store_lost_write_detect_fp64_init(struct test *test) {
    auto *d = new SLDData;
    if (!d) return EXIT_FAILURE;
    for (int i = 0; i < SLD_MAX_THREADS; ++i) {
        d->sentinel[i] = 0;
        d->written[i].store(0, std::memory_order_relaxed);
        d->started[i].store(0, std::memory_order_relaxed);
    }
    test->data = d;
    return EXIT_SUCCESS;
}

static int store_lost_write_detect_fp64_cleanup(struct test *test) {
    delete static_cast<SLDData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static inline void sld_str(double *addr, double val) {
    __asm__ volatile("str %d0, [%1]" :: "w"(val), "r"(addr) : "memory");
}

static inline double sld_ldr(const double *addr) {
    double v;
    __asm__ volatile("ldr %d0, [%1]" : "=w"(v) : "r"(addr) : "memory");
    return v;
}

static int store_lost_write_detect_fp64_run(struct test *test, int cpu) {
    auto *d = static_cast<SLDData *>(test->data);
    const int partner = cpu ^ 1;
    uint64_t epoch = 0;

    d->started[cpu].store(1, std::memory_order_relaxed);

    do {
        ++epoch;
        double *slot = &d->sentinel[cpu];

        /* store + layer 1: same-core immediate read-back */
        sld_str(slot, (double)epoch);
        double l1 = sld_ldr(slot);
        if (l1 != (double)epoch) {
            report_fail_msg("store_lost_write_detect_fp64: cpu %d layer-1 "
                            "same-core read-back %.0f, wrote %.0f (locally "
                            "lost FP64 write)", cpu, l1,
                            (double)epoch);
        }

        /* publish: release pairs with the reader's acquire below */
        d->written[cpu].store(epoch, std::memory_order_release);

        /* cross-core side: the partner's freshly published slot, if any */
        uint64_t published = d->written[partner].load(std::memory_order_acquire);
        if (published != 0) {
            const double *pslot = &d->sentinel[partner];
            double p1 = sld_ldr(pslot);              /* layer 2 */
            __asm__ volatile("dsb ish" ::: "memory");
            double p2 = sld_ldr(pslot);              /* layer 3 */
            if (p1 < (double)published || p2 < (double)published) {
                report_fail_msg("store_lost_write_detect_fp64: cpu %d read "
                                "partner %d slot %.0f/%.0f, partner published "
                                "%.0f (lost write: precisely-old FP64 value)",
                                cpu, partner, p1,
                                p2, (double)published);
            }
        } else if (d->started[partner].load(std::memory_order_relaxed) == 0) {
            /* no partner in this run (-n 1): ladder continues same-core */
            double l2 = sld_ldr(slot);
            __asm__ volatile("dsb ish" ::: "memory");
            double l3 = sld_ldr(slot);
            if (l2 != (double)epoch || l3 != (double)epoch) {
                report_fail_msg("store_lost_write_detect_fp64: cpu %d solo "
                                "ladder l2=%.0f l3=%.0f, wrote %.0f",
                                cpu, l2, l3,
                                (double)epoch);
            }
        }
        /* if the partner HAS started but not published yet, skip this
         * round's cross-read — no waiting, no deadlock, next round will
         * see the publication */
        /* excitation filler: NEON switching power (see arm_add_excite.h) */
        if (!arm_add_excite::fill(cpu, epoch)) {
            report_fail_msg("excitation filler store->reload mismatch "
                            "(cpu %d round %lu)", cpu,
                            (unsigned long)epoch);
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(store_lost_write_detect_fp64, "FP64 variant: lost-write sentinel ladder: str epoch, same-core read-back, cross-core read, then dsb ish re-read — any layer holding the old value is a lost write (ARM64)")
    .test_init = store_lost_write_detect_fp64_init,
    .test_run = store_lost_write_detect_fp64_run,
    .test_cleanup = store_lost_write_detect_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture: our semantic rounds are microseconds; auto mode (40) ends the run in ~10 ms */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
