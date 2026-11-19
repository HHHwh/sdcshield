/*  str_instruction_stress_neon_fp64 — FP64 datapath variant of
 *  tests/cpu/arm_add/str_instruction_stress_neon.cpp (2026-11-10;
 *  parent untouched).  Completed-state marker protocol (odd/even ver,
 *  dmb/dsb fences, one-epoch foresight tolerance) kept verbatim; the
 *  vector batch is re-typed to f64x2 (integer-valued epoch doubles,
 *  bit-exact veorq_u64 lane verify, asm st_vec/ld_vec kept).
 *
 *  (was: NEON variant of
 *  tests/cpu/arm_add/str_instruction_stress.cpp)
 *
 *  The scalar four-width str batch becomes a DENSE NEON VECTOR store
 *  batch: every slot is one uint32x4 (all lanes carry the epoch), the
 *  batch is emitted with asm st_vec (intrinsics get GCC-forwarded),
 *  then dsb ish, then the completed-state seqlock publication —
 *  including the store-store fence after the odd "writing" marker,
 *  the fix the scalar original carries (marker must be visible
 *  BEFORE any slot of the new batch).  Verification:
 *    verify 1 : same-core ld_vec sweep — every slot's every lane
 *               must equal the epoch exactly;
 *    verify 2 : partner completed-state snapshot (v1 == v2 judged),
 *               full-vector compare.
 *  A partial-lane mismatch is a partially-landed NEON store — the
 *  lost-store signature on the vector path.
 *
 *  -n 1: no partner publishes; the full batch->dsb->verify ladder
 *  runs same-core at full density.
 */
#include "sandstone.h"
#include <cstdint>
#include <atomic>
#include <arm_neon.h>

#define SISN_MAX_THREADS 1024
#define SISN_NSLOTS      1024

namespace { /* ODR: per-TU SISNData */
struct SISNData {
    double (*slots)[SISN_NSLOTS][2];            /* [threads][slots][lanes] — f64x2 */
    alignas(64) std::atomic<uint64_t> ver[SISN_MAX_THREADS]; /* 0=down, odd=writing, even=done(E=v/2) */
};
}

static inline float64x2_t ld_vec(const double *a) {
    float64x2_t v;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(v) : "r"(a) : "memory");
    return v;
}
static inline void st_vec(double *a, float64x2_t v) {
    __asm__ volatile("str %q0, [%1]" :: "w"(v), "r"(a) : "memory");
}

static int str_instruction_stress_neon_fp64_init(struct test *test) {
    auto *d = new SISNData;
    if (!d) return EXIT_FAILURE;
    d->slots = new double[SISN_MAX_THREADS][SISN_NSLOTS][2];
    if (!d->slots) { delete d; return EXIT_FAILURE; }
    for (int i = 0; i < SISN_MAX_THREADS; ++i)
        d->ver[i].store(0, std::memory_order_relaxed);
    test->data = d;
    return EXIT_SUCCESS;
}

static int str_instruction_stress_neon_fp64_cleanup(struct test *test) {
    auto *d = static_cast<SISNData *>(test->data);
    delete[] d->slots;
    delete d;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int str_instruction_stress_neon_fp64_run(struct test *test, int cpu) {
    auto *d = static_cast<SISNData *>(test->data);
    const int partner = cpu ^ 1;
    uint64_t epoch = 0;

    do {
        ++epoch;
        auto *mine = d->slots[cpu];
        float64x2_t ev = vdupq_n_f64((double)epoch);

        /* odd marker, then store-store fence (marker BEFORE batch —
         * the fix carried over from the scalar original), then the
         * dense NEON vector store batch, drain, even marker */
        d->ver[cpu].store(2 * epoch - 1, std::memory_order_relaxed);
        /* full fence (was ishst) — 192-thread foresight fix shared with the
         * scalar original (measured pe+1 false-fails in the campaign) */
        __asm__ volatile("dmb ish" ::: "memory");
        for (int s = 0; s < SISN_NSLOTS; ++s)
            st_vec(&mine[s][0], ev);
        __asm__ volatile("dsb ish" ::: "memory");
        d->ver[cpu].store(2 * epoch, std::memory_order_release);

        /* verify 1: same-core, every slot every lane, exact epoch */
        for (int s = 0; s < SISN_NSLOTS; ++s) {
            float64x2_t v = ld_vec(&mine[s][0]);
            uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(v), vreinterpretq_u64_f64(ev));
            if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                report_fail_msg("str_instruction_stress_neon_fp64: cpu %d slot "
                                "%d self read-back bits {%016llx,%016llx}, wrote "
                                "epoch %lu (FP64 store did not land)", cpu,
                                s, (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(v), 0),
                                (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(v), 1),
                                (unsigned long)epoch);
            }
        }

        /* verify 2: partner completed-state snapshot */
        uint64_t v1 = d->ver[partner].load(std::memory_order_acquire);
        if ((v1 & 1) == 0 && v1 != 0) {
            const uint64_t pe = v1 / 2;
            auto *theirs = d->slots[partner];
            float64x2_t pev = vdupq_n_f64((double)pe);
            int bad_slot = -1;
            float64x2_t bad{};
            /* ONE-EPOCH FORESIGHT TOLERATED (see scalar original's note):
             * slot = pe+1 with the marker still in flight is a legal
             * cross-PE propagation race; older-than-pe or partial-lane
             * mismatches still fail */
            float64x2_t pev1 = vdupq_n_f64((double)(pe + 1));
            for (int s = 0; s < SISN_NSLOTS; ++s) {
                float64x2_t v = ld_vec(&theirs[s][0]);
                uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(v), vreinterpretq_u64_f64(pev));
                uint64x2_t diff1 = veorq_u64(vreinterpretq_u64_f64(v), vreinterpretq_u64_f64(pev1));
                if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0
                    && vmaxvq_u32(vreinterpretq_u32_u64(diff1)) != 0) {
                    bad_slot = s; bad = v; break;
                }
            }
            uint64_t v2 = d->ver[partner].load(std::memory_order_acquire);
            if (v1 == v2 && bad_slot >= 0) {
                report_fail_msg("str_instruction_stress_neon_fp64: cpu %d sees "
                                "partner %d completed epoch %lu slot %d "
                                "bits {%016llx,%016llx} — some FP64 store of the "
                                "batch did not land", cpu, partner,
                                (unsigned long)pe, bad_slot,
                                (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(bad), 0),
                                (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(bad), 1));
            }
            /* v1 != v2: torn snapshot round, discarded and retried */
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(str_instruction_stress_neon_fp64, "FP64 variant: dense f64x2 store batch (asm st_vec) + dsb ish, same-core and cross-core completed-state verification — any lane not landing fails (ARM64)")
    .test_init = str_instruction_stress_neon_fp64_init,
    .test_run = str_instruction_stress_neon_fp64_run,
    .test_cleanup = str_instruction_stress_neon_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
