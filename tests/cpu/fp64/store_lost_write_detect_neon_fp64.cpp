/*  store_lost_write_detect_neon_fp64 — FP64 datapath variant of
 *  tests/cpu/arm_add/store_lost_write_detect_neon.cpp (2026-11-10;
 *  parent untouched).  Sentinel-vector ladder chassis kept verbatim;
 *  the vector payload is re-typed to f64x2 (integer-valued epoch
 *  doubles, bit-exact veorq_u64 verify, vcltq_f64 ordering).
 *  NEON variant of
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
    alignas(64) double sentinel[SLDN_MAX_THREADS][2];
    alignas(64) std::atomic<uint64_t> written[SLDN_MAX_THREADS];
    alignas(64) std::atomic<uint64_t> started[SLDN_MAX_THREADS];
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

static int store_lost_write_detect_neon_fp64_init(struct test *test) {
    auto *d = new SLDNData;
    if (!d) return EXIT_FAILURE;
    for (int i = 0; i < SLDN_MAX_THREADS; ++i) {
        for (int l = 0; l < 2; ++l) d->sentinel[i][l] = 0.0;
        d->written[i].store(0, std::memory_order_relaxed);
        d->started[i].store(0, std::memory_order_relaxed);
    }
    test->data = d;
    return EXIT_SUCCESS;
}

static int store_lost_write_detect_neon_fp64_cleanup(struct test *test) {
    delete static_cast<SLDNData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int store_lost_write_detect_neon_fp64_run(struct test *test, int cpu) {
    auto *d = static_cast<SLDNData *>(test->data);
    const int partner = cpu ^ 1;
    uint64_t epoch = 0;

    d->started[cpu].store(1, std::memory_order_relaxed);

    do {
        ++epoch;
        double *slot = d->sentinel[cpu];
        float64x2_t ev = vdupq_n_f64((double)epoch);

        /* store + layer 1: same-core read-back */
        st_vec(slot, ev);
        float64x2_t l1 = ld_vec(slot);
        uint64x2_t d1 = veorq_u64(vreinterpretq_u64_f64(l1), vreinterpretq_u64_f64(ev));
        if (vmaxvq_u32(vreinterpretq_u32_u64(d1)) != 0) {
            report_fail_msg("store_lost_write_detect_neon_fp64: cpu %d layer-1 "
                            "read-back differs (epoch %lu)", cpu,
                            (unsigned long)epoch);
        }

        /* publish (scalar channel, release) */
        d->written[cpu].store(epoch, std::memory_order_release);

        /* cross-core: partner's freshly published vector, if any */
        uint64_t published = d->written[partner].load(std::memory_order_acquire);
        if (published != 0) {
            double *pslot = d->sentinel[partner];
            float64x2_t pe = vdupq_n_f64((double)published);
            float64x2_t p1 = ld_vec(pslot);              /* layer 2 */
            __asm__ volatile("dsb ish" ::: "memory");
            float64x2_t p2 = ld_vec(pslot);              /* layer 3 */
            /* OLDER-than-published is the failure: the partner legally
             * publishes newer epochs between our acquire and our read,
             * so p1 > published is progress, NOT a failure (the first
             * draft failed on != and broke on healthy cores) */
            uint64x2_t d2 = vcltq_f64(p1, pe);
            uint64x2_t d3 = vcltq_f64(p2, pe);
            if (vmaxvq_u32(vreinterpretq_u32_u64(d2)) != 0 || vmaxvq_u32(vreinterpretq_u32_u64(d3)) != 0) {
                report_fail_msg("store_lost_write_detect_neon_fp64: cpu %d read "
                                "partner %d vector as {old} vs published %lu "
                                "(lost FP64 store: precisely-old FP64 vector)",
                                cpu, partner, (unsigned long)published);
            }
        } else if (d->started[partner].load(std::memory_order_relaxed) == 0) {
            /* -n 1: solo ladder continues same-core */
            float64x2_t l2 = ld_vec(slot);
            __asm__ volatile("dsb ish" ::: "memory");
            float64x2_t l3 = ld_vec(slot);
            uint64x2_t d2 = veorq_u64(vreinterpretq_u64_f64(l2), vreinterpretq_u64_f64(ev));
            uint64x2_t d3 = veorq_u64(vreinterpretq_u64_f64(l3), vreinterpretq_u64_f64(ev));
            if (vmaxvq_u32(vreinterpretq_u32_u64(d2)) != 0 || vmaxvq_u32(vreinterpretq_u32_u64(d3)) != 0) {
                report_fail_msg("store_lost_write_detect_neon_fp64: cpu %d solo "
                                "ladder differs (epoch %lu)", cpu,
                                (unsigned long)epoch);
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(store_lost_write_detect_neon_fp64, "FP64 variant: lost-write f64x2 sentinel ladder: asm vst epoch vector, same-core read-back, cross-core read, dsb ish re-read — any layer holding the old vector is a lost NEON store (ARM64)")
    .test_init = store_lost_write_detect_neon_fp64_init,
    .test_run = store_lost_write_detect_neon_fp64_run,
    .test_cleanup = store_lost_write_detect_neon_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
