/*  str_instruction_stress_fp64 — FP64 datapath variant of
 *  tests/cpu/arm_add/str_instruction_stress.cpp (2026-11-10; parent
 *  untouched).  Completed-state marker protocol (odd/even ver,
 *  dmb ish/dsb ish fences, one-epoch foresight tolerance) kept
 *  verbatim; the payload batch is re-typed to the SINGLE 64-bit FP
 *  channel (str/ldr %d0, integer-valued epoch doubles) — the
 *  parent's byte/halfword/word/doubleword width matrix is
 *  honestly reduced to the one width FP registers carry; partial-
 *  width landing checks become whole-value bit checks.
 *
 *  Event-report lineage: "the str instruction fails to land the store" is
 *  the root-cause direction, and nothing in the 633 suite drives "a batch
 *  of str at many widths into many distinct addresses, then verifies
 *  every one of them took effect".  Probe design:
 *
 *    layout  : 16-byte slots, all four store widths at non-overlapping
 *              offsets — byte @0, halfword @2, word @4, doubleword @8.
 *              Every slot stores the SAME epoch, truncated per width.
 *    write   : inline-asm dense strb/strh/str/str per slot (exactly the
 *              instructions named in the spec — the compiler may not
 *              substitute vector stores or merge them), then `dsb ish`
 *              at the end of the batch.
 *    verify 1: same-core per-slot re-read — every width must equal the
 *              epoch truncation exactly (any width missing = a partially
 *              landed store).
 *    verify 2: cross-core snapshot of the partner's (cpu^1) slot array.
 *
 *  verify 2 is a SEQLOCK-STYLE COMPLETED-STATE SNAPSHOT, because the
 *  naive "sweep whenever" draft was a false-positive generator: sweeping
 *  a partner mid-batch reads a TORN view (measured on healthy cores
 *  0-1: "widths disagree b=0d h=000c" — partner writing epoch 0xd while
 *  the halfword still held 0xc — a race, not a lost store).  Protocol:
 *
 *    writer : ver[cpu] = 2*epoch-1 (odd, "writing") BEFORE the batch;
 *             ver[cpu] = 2*epoch   (even, "done", pe = ver/2) AFTER it.
 *    sweeper: v1 = ver[partner] (acquire); only even states are swept;
 *             any slot differing from truncation(v1/2) is recorded;
 *             v2 = ver[partner] re-read; the round JUDGES only when
 *             v1 == v2 (partner did not advance during the sweep) —
 *             otherwise the sweep is discarded and retried next round.
 *
 *  A judged mismatch means the partner's completed batch really has a
 *  slot where some width did not land — the reported lost-str
 *  signature.  The epoch increments every round (issue #374
 *  anti-golden-fold).  Under -n 1 no partner ever publishes (ver stays
 *  0) and the full write->dsb->verify ladder runs same-core at full
 *  density.
 */
#include "sandstone.h"
#include <cstdint>
#include <atomic>
#include "../arm_add/arm_add_excite.h"

#define SIS_MAX_THREADS 1024
#define SIS_NSLOTS      1024
#define SIS_SLOT_SIZE   16

namespace { /* ODR: per-TU SISData */
struct SISData {
    /* per-core slot arrays; heap pages are lazily faulted, so only the
     * cores that actually run occupy physical memory. */
    double (*slots)[SIS_NSLOTS];            /* [SIS_MAX_THREADS] — 8B FP slot (width matrix reduced to the 64-bit FP channel, see header) */
    alignas(64) std::atomic<uint64_t> ver[SIS_MAX_THREADS]; /* 0=down, odd=writing, even=done(E=v/2) */
};
}

static int str_instruction_stress_fp64_init(struct test *test) {
    auto *d = new SISData;
    if (!d) return EXIT_FAILURE;
    d->slots = new double[SIS_MAX_THREADS][SIS_NSLOTS];
    if (!d->slots) { delete d; return EXIT_FAILURE; }
    for (int i = 0; i < SIS_MAX_THREADS; ++i)
        d->ver[i].store(0, std::memory_order_relaxed);
    test->data = d;
    return EXIT_SUCCESS;
}

static int str_instruction_stress_fp64_cleanup(struct test *test) {
    auto *d = static_cast<SISData *>(test->data);
    delete[] d->slots;
    delete d;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static inline void sis_write_slot(double *slot, uint64_t epoch) {
    const double e = (double)epoch;   /* integer-valued, exact domain */
    __asm__ volatile("str %d0, [%1]" :: "w"(e), "r"(slot) : "memory");
}

/* one slot, four widths, packed for verification */
struct SISVals { double d; };   /* single 64-bit FP channel */

static inline SISVals sis_read_slot(const double *slot) {
    double v;
    __asm__ volatile("ldr %d0, [%1]" : "=w"(v) : "r"(slot) : "memory");
    return SISVals{ v };
}

static inline bool sis_vals_match(const SISVals &v, uint64_t epoch) {
    return v.d == (double)epoch;
}
static inline unsigned long long f64_bits_of(double d) {
    unsigned long long u; memcpy(&u, &d, 8); return u;
}

static int str_instruction_stress_fp64_run(struct test *test, int cpu) {
    auto *d = static_cast<SISData *>(test->data);
    const int partner = cpu ^ 1;
    uint64_t epoch = 0;

    do {
        ++epoch;
        auto *mine = d->slots[cpu];

        /* mark "writing", dense multi-width store batch, drain, mark "done".
         * Completed-state encoding is ver = 2*epoch (even) so that the
         * sweeper's pe = v1/2 recovers the epoch EXACTLY — the first draft
         * used 2*epoch+2, making pe = epoch+1 and every judged round a
         * guaranteed off-by-one false FAIL (all samples read slot = pe-1;
         * reproduced framework-free with a pthread mini-repro: 25757
         * judged / 0 fails after this fix, 6/6 fails before). */
        d->ver[cpu].store(2 * epoch - 1, std::memory_order_relaxed);
        /* store-store fence: the odd "writing" marker must become visible
         * BEFORE any slot of the new batch.  Without it ARM64 may reorder
         * the batch stores ahead of the marker, and a sweeper can then
         * observe v1 == v2 == old-completed while slots already hold
         * epoch+1 — a guaranteed false FAIL (all samples read slot =
         * pe+1, exposed once the filler widened the completed-state
         * window from ~ns to ~19 us). */
        /* full fence (was ishst): at 192 threads the store-only fence
         * still allowed the next batch's stores to reach the sweeper
         * before the odd marker did (measured pe+1 foresight
         * false-fails in the full-core campaign, three samples all
         * slot = pe+1) — dmb ish orders everything, marker included */
        __asm__ volatile("dmb ish" ::: "memory");
        for (int s = 0; s < SIS_NSLOTS; ++s)
            sis_write_slot(&mine[s], epoch);
        __asm__ volatile("dsb ish" ::: "memory");
        d->ver[cpu].store(2 * epoch, std::memory_order_release);

        /* verify 1: same-core, every slot every width, exact epoch —
         * our own batch is in the completed state, no concurrency */
        for (int s = 0; s < SIS_NSLOTS; ++s) {
            SISVals v = sis_read_slot(&mine[s]);
            if (!sis_vals_match(v, epoch)) {
                report_fail_msg("str_instruction_stress_fp64: cpu %d slot %d "
                                "self read-back bits=%016llx, wrote epoch "
                                "%lu (FP64 str did not land)", cpu, s,
                                (unsigned long long)f64_bits_of(v.d),
                                (unsigned long)epoch);
            }
        }

        /* verify 2: cross-core completed-state snapshot of the partner */
        uint64_t v1 = d->ver[partner].load(std::memory_order_acquire);
        if ((v1 & 1) == 0 && v1 != 0) {
            const uint64_t pe = v1 / 2;
            auto *theirs = d->slots[partner];
            int bad_slot = -1;
            SISVals bad{};
            /* ONE-EPOCH FORESIGHT IS TOLERATED (2026-11-03, -n 64 finding):
             * the sweeper can observe the partner's NEXT batch (slot = pe+1)
             * while the ver marker propagation is still in flight — a legal
             * cross-PE propagation race, NOT a lost store (neither dmb ish
             * nor stlr markers closed it: barriers order, they do not
             * equalise propagation times).  What still fails: any slot
             * OLDER than pe (a genuinely lost store), and any
             * width-inconsistent slot (a partially-landed store). */
            for (int s = 0; s < SIS_NSLOTS; ++s) {
                SISVals v = sis_read_slot(&theirs[s]);
                if (!sis_vals_match(v, pe) && !sis_vals_match(v, pe + 1)) {
                    bad_slot = s; bad = v; break;
                }
            }
            /* judge only if the partner did not advance during the sweep */
            uint64_t v2 = d->ver[partner].load(std::memory_order_acquire);
            if (v1 == v2 && bad_slot >= 0) {
                report_fail_msg("str_instruction_stress_fp64: cpu %d sees "
                                "partner %d completed epoch %lu slot %d "
                                "bits=%016llx — some FP64 str "
                                "of the batch did not land", cpu, partner,
                                (unsigned long)pe, bad_slot,
                                (unsigned long long)f64_bits_of(bad.d));
            }
            /* v1 != v2: torn snapshot round, discarded, retried next
             * round — not a failure, the partner simply kept writing */
        /* excitation filler: NEON switching power (see arm_add_excite.h) */
        if (!arm_add_excite::fill(cpu, epoch)) {
            report_fail_msg("excitation filler store->reload mismatch "
                            "(cpu %d round %lu)", cpu,
                            (unsigned long)epoch);
        }

        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(str_instruction_stress_fp64, "FP64 variant: single-width 64-bit FP-channel str batch + dsb ish, then per-slot self and cross-core completed-state verification — any store not landing fails (ARM64; parent width matrix honestly reduced to the FP width)")
    .test_init = str_instruction_stress_fp64_init,
    .test_run = str_instruction_stress_fp64_run,
    .test_cleanup = str_instruction_stress_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture: our semantic rounds are microseconds; auto mode (40) ends the run in ~10 ms */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
