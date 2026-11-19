/*  str_instruction_stress_neon — NEON variant of
 *  tests/cpu/arm_add/str_instruction_stress.cpp (original untouched).
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
    uint32_t (*slots)[SISN_NSLOTS][4];          /* [threads][slots][lanes] */
    alignas(64) std::atomic<uint64_t> ver[SISN_MAX_THREADS]; /* 0=down, odd=writing, even=done(E=v/2) */
};
}

static inline uint32x4_t ld_vec(const uint32_t *a) {
    uint32x4_t v;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(v) : "r"(a) : "memory");
    return v;
}
static inline void st_vec(uint32_t *a, uint32x4_t v) {
    __asm__ volatile("str %q0, [%1]" :: "w"(v), "r"(a) : "memory");
}

static int str_instruction_stress_neon_init(struct test *test) {
    auto *d = new SISNData;
    if (!d) return EXIT_FAILURE;
    d->slots = new uint32_t[SISN_MAX_THREADS][SISN_NSLOTS][4];
    if (!d->slots) { delete d; return EXIT_FAILURE; }
    for (int i = 0; i < SISN_MAX_THREADS; ++i)
        d->ver[i].store(0, std::memory_order_relaxed);
    test->data = d;
    return EXIT_SUCCESS;
}

static int str_instruction_stress_neon_cleanup(struct test *test) {
    auto *d = static_cast<SISNData *>(test->data);
    delete[] d->slots;
    delete d;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int str_instruction_stress_neon_run(struct test *test, int cpu) {
    auto *d = static_cast<SISNData *>(test->data);
    const int partner = cpu ^ 1;
    uint64_t epoch = 0;

    do {
        ++epoch;
        auto *mine = d->slots[cpu];
        uint32x4_t ev = vdupq_n_u32((uint32_t)epoch);

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
            uint32x4_t v = ld_vec(&mine[s][0]);
            uint32x4_t diff = veorq_u32(v, ev);
            if (vmaxvq_u32(diff) != 0) {
                report_fail_msg("str_instruction_stress_neon: cpu %d slot "
                                "%d self read-back {%u,%u,%u,%u}, wrote "
                                "epoch %lu (NEON store did not land)", cpu,
                                s, vgetq_lane_u32(v, 0), vgetq_lane_u32(v, 1),
                                vgetq_lane_u32(v, 2), vgetq_lane_u32(v, 3),
                                (unsigned long)epoch);
            }
        }

        /* verify 2: partner completed-state snapshot */
        uint64_t v1 = d->ver[partner].load(std::memory_order_acquire);
        if ((v1 & 1) == 0 && v1 != 0) {
            const uint64_t pe = v1 / 2;
            auto *theirs = d->slots[partner];
            uint32x4_t pev = vdupq_n_u32((uint32_t)pe);
            int bad_slot = -1;
            uint32x4_t bad{};
            /* ONE-EPOCH FORESIGHT TOLERATED (see scalar original's note):
             * slot = pe+1 with the marker still in flight is a legal
             * cross-PE propagation race; older-than-pe or partial-lane
             * mismatches still fail */
            uint32x4_t pev1 = vdupq_n_u32((uint32_t)(pe + 1));
            for (int s = 0; s < SISN_NSLOTS; ++s) {
                uint32x4_t v = ld_vec(&theirs[s][0]);
                uint32x4_t diff = veorq_u32(v, pev);
                uint32x4_t diff1 = veorq_u32(v, pev1);
                if (vmaxvq_u32(diff) != 0 && vmaxvq_u32(diff1) != 0) {
                    bad_slot = s; bad = v; break;
                }
            }
            uint64_t v2 = d->ver[partner].load(std::memory_order_acquire);
            if (v1 == v2 && bad_slot >= 0) {
                report_fail_msg("str_instruction_stress_neon: cpu %d sees "
                                "partner %d completed epoch %lu slot %d "
                                "{%u,%u,%u,%u} — some NEON store of the "
                                "batch did not land", cpu, partner,
                                (unsigned long)pe, bad_slot,
                                vgetq_lane_u32(bad, 0), vgetq_lane_u32(bad, 1),
                                vgetq_lane_u32(bad, 2), vgetq_lane_u32(bad, 3));
            }
            /* v1 != v2: torn snapshot round, discarded and retried */
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(str_instruction_stress_neon, "Dense NEON vector store batch (asm st_vec) + dsb ish, same-core and cross-core completed-state verification — any lane not landing fails (ARM64)")
    .test_init = str_instruction_stress_neon_init,
    .test_run = str_instruction_stress_neon_run,
    .test_cleanup = str_instruction_stress_neon_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
