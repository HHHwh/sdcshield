/*  store_lost_write_detect_neon — NEON variant of
 *  tests/cpu/arm_add/store_lost_write_detect.cpp (original untouched).
 *
 *  Same three-layer lost-write ladder on the 16-byte NEON datapath:
 *  the sentinel is a uint32x4 epoch vector (all lanes carry the epoch,
 *  so any partial-lane corruption is visible); the publication channel
 *  stays a scalar std::atomic (release/acquire ordering is a lock-
 *  free protocol, orthogonal to the datapath under test):
 *    layer 1 : same-core asm-ld read-back of the just-stored vector;
 *    layer 2 : cross-core read of the partner's published vector;
 *    layer 3 : partner re-read after dsb ish.
 *  Vector store/load go through asm (ld_vec/st_vec) — intrinsic
 *  same-address pairs get GCC-forwarded (measured today).
 *
 *  -n 1: solo ladder runs same-core (layers 2/3 in sequence).
 */
#include "sandstone.h"
#include <cstdint>
#include <atomic>
#include <arm_neon.h>

#define SLDN_MAX_THREADS 1024

namespace { /* ODR: per-TU SLDNData */
struct SLDNData {
    alignas(64) uint32_t sentinel[SLDN_MAX_THREADS][4];
    alignas(64) std::atomic<uint64_t> written[SLDN_MAX_THREADS];
    alignas(64) std::atomic<uint64_t> started[SLDN_MAX_THREADS];
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

static int store_lost_write_detect_neon_init(struct test *test) {
    auto *d = new SLDNData;
    if (!d) return EXIT_FAILURE;
    for (int i = 0; i < SLDN_MAX_THREADS; ++i) {
        for (int l = 0; l < 4; ++l) d->sentinel[i][l] = 0;
        d->written[i].store(0, std::memory_order_relaxed);
        d->started[i].store(0, std::memory_order_relaxed);
    }
    test->data = d;
    return EXIT_SUCCESS;
}

static int store_lost_write_detect_neon_cleanup(struct test *test) {
    delete static_cast<SLDNData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int store_lost_write_detect_neon_run(struct test *test, int cpu) {
    auto *d = static_cast<SLDNData *>(test->data);
    const int partner = cpu ^ 1;
    uint64_t epoch = 0;

    d->started[cpu].store(1, std::memory_order_relaxed);

    do {
        ++epoch;
        uint32_t *slot = d->sentinel[cpu];
        uint32x4_t ev = vdupq_n_u32((uint32_t)epoch);

        /* store + layer 1: same-core read-back */
        st_vec(slot, ev);
        uint32x4_t l1 = ld_vec(slot);
        uint32x4_t d1 = veorq_u32(l1, ev);
        if (vmaxvq_u32(d1) != 0) {
            report_fail_msg("store_lost_write_detect_neon: cpu %d layer-1 "
                            "read-back differs (epoch %lu)", cpu,
                            (unsigned long)epoch);
        }

        /* publish (scalar channel, release) */
        d->written[cpu].store(epoch, std::memory_order_release);

        /* cross-core: partner's freshly published vector, if any */
        uint64_t published = d->written[partner].load(std::memory_order_acquire);
        if (published != 0) {
            uint32_t *pslot = d->sentinel[partner];
            uint32x4_t pe = vdupq_n_u32((uint32_t)published);
            uint32x4_t p1 = ld_vec(pslot);              /* layer 2 */
            __asm__ volatile("dsb ish" ::: "memory");
            uint32x4_t p2 = ld_vec(pslot);              /* layer 3 */
            /* OLDER-than-published is the failure: the partner legally
             * publishes newer epochs between our acquire and our read,
             * so p1 > published is progress, NOT a failure (the first
             * draft failed on != and broke on healthy cores) */
            uint32x4_t d2 = vcltq_u32(p1, pe);
            uint32x4_t d3 = vcltq_u32(p2, pe);
            if (vmaxvq_u32(d2) != 0 || vmaxvq_u32(d3) != 0) {
                report_fail_msg("store_lost_write_detect_neon: cpu %d read "
                                "partner %d vector as {old} vs published %lu "
                                "(lost NEON store: precisely-old vector)",
                                cpu, partner, (unsigned long)published);
            }
        } else if (d->started[partner].load(std::memory_order_relaxed) == 0) {
            /* -n 1: solo ladder continues same-core */
            uint32x4_t l2 = ld_vec(slot);
            __asm__ volatile("dsb ish" ::: "memory");
            uint32x4_t l3 = ld_vec(slot);
            uint32x4_t d2 = veorq_u32(l2, ev);
            uint32x4_t d3 = veorq_u32(l3, ev);
            if (vmaxvq_u32(d2) != 0 || vmaxvq_u32(d3) != 0) {
                report_fail_msg("store_lost_write_detect_neon: cpu %d solo "
                                "ladder differs (epoch %lu)", cpu,
                                (unsigned long)epoch);
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(store_lost_write_detect_neon, "Lost-write NEON sentinel ladder: asm vst epoch vector, same-core read-back, cross-core read, dsb ish re-read — any layer holding the old vector is a lost NEON store (ARM64)")
    .test_init = store_lost_write_detect_neon_init,
    .test_run = store_lost_write_detect_neon_run,
    .test_cleanup = store_lost_write_detect_neon_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
