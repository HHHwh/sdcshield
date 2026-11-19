/*  low_load_cache_pressure_neon_fp64 — FP64 datapath variant of
 *  tests/cpu/arm_add/low_load_cache_pressure_neon.cpp (2026-11-10;
 *  parent untouched).  Duty-cycle chassis, dmb ish cadence and
 *  remote-load sweep kept verbatim; the epoch vector slot is re-typed
 *  to f64x2 (integer-valued epoch doubles, bit-exact verify, vcltq_f64
 *  ordering == parent u32 ordering in the exact domain).
 *  NEON variant of
 *  tests/cpu/arm_add/low_load_cache_pressure.cpp (original untouched).
 *
 *  Same low-duty-cycle cross-die probe (10 ms period, duty_us knob,
 *  cntvct window timing, sleep as payload) — the slot payload moves
 *  to the 16-byte NEON datapath: each core's slot is a uint32x4 epoch
 *  vector written with asm st_vec, read back with ld_vec, and swept
 *  remotely with ld_vec.  Fail criteria: own-slot read-back differing
 *  from the just-stored epoch, or any remote slot's epoch regressing
 *  lane-wise (a precisely-old vector at low load).
 *
 *  -n 1: the sweep covers only the core's own slot; the full low-duty
 *  rhythm plus the asm-vector store/read-back ladder still run.
 */
#include "sandstone.h"
#include <cstdint>
#include <ctime>
#include <atomic>
#include <arm_neon.h>

#define LLPN_MAX_THREADS 1024
#define LLPN_PERIOD_NS   10000000LL   /* 10 ms */
#define LLPN_PERIOD_US   10000

namespace { /* ODR: per-TU LLPNData */
struct LLPNData {
    alignas(64) double slot[LLPN_MAX_THREADS][2];   /* FP64 epoch vectors */
    alignas(64) std::atomic<uint64_t> started[LLPN_MAX_THREADS];
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
static inline unsigned long long f64_bits_of(double d) {
    unsigned long long u; memcpy(&u, &d, 8); return u;
}

static inline uint64_t llpn_cntvct(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline uint64_t llpn_timer_freq(void) {
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
}

static int low_load_cache_pressure_neon_fp64_init(struct test *test) {
    auto *d = new LLPNData;
    if (!d) return EXIT_FAILURE;
    for (int i = 0; i < LLPN_MAX_THREADS; ++i) {
        for (int l = 0; l < 2; ++l) d->slot[i][l] = 0.0;
        d->started[i].store(0, std::memory_order_relaxed);
    }
    test->data = d;
    return EXIT_SUCCESS;
}

static int low_load_cache_pressure_neon_fp64_cleanup(struct test *test) {
    delete static_cast<LLPNData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int low_load_cache_pressure_neon_fp64_run(struct test *test, int cpu) {
    auto *d = static_cast<LLPNData *>(test->data);

    uint64_t duty_us = get_testspecific_knob_value_uint(test, "duty_us", 1000);
    if (duty_us < 500)  duty_us = 500;
    if (duty_us > 3000) duty_us = 3000;

    const uint64_t freq = llpn_timer_freq();
    const uint64_t active_ticks = duty_us * freq / 1000000ULL;
    const int64_t idle_ns = (int64_t)(LLPN_PERIOD_US - duty_us) * 1000LL;

    /* per-slot vector golden for the regression sweep */
    std::vector<double> last_seen_mem((size_t)LLPN_MAX_THREADS * 2, 0.0);
    double (*last_seen)[2] =
        reinterpret_cast<double (*)[2]>(last_seen_mem.data());

    uint64_t epoch = 0;
    d->started[cpu].store(1, std::memory_order_relaxed);

    do {
        const uint64_t window_end = llpn_cntvct() + active_ticks;

        /* ---- active window: NEON vector store + remote-load sweep ---- */
        do {
            ++epoch;
            double *mine = d->slot[cpu];
            float64x2_t ev = vdupq_n_f64((double)epoch);
            st_vec(mine, ev);
            __asm__ volatile("dmb ish" ::: "memory");
            float64x2_t back = ld_vec(mine);
            uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(back), vreinterpretq_u64_f64(ev));
            if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                report_fail_msg("low_load_cache_pressure_neon_fp64: cpu %d "
                                "own-slot read-back differs (epoch %lu)",
                                cpu, (unsigned long)epoch);
            }

            for (int r = 0; r < LLPN_MAX_THREADS; ++r) {
                if (r != cpu
                    && d->started[r].load(std::memory_order_relaxed) == 0)
                    continue;
                float64x2_t v = ld_vec(d->slot[r]);
                float64x2_t ls = vld1q_f64(last_seen[r]);
                uint64x2_t reg = vcltq_f64(v, ls);   /* lane-wise regression *
                 * (integer-valued doubles: == parent u32 ordering) */
                if (vmaxvq_u32(vreinterpretq_u32_u64(reg)) != 0) {
                    report_fail_msg("low_load_cache_pressure_neon_fp64: cpu %d "
                                    "saw slot %d vector regress: now-bits "
                                    "{%016llx,%016llx} after-bits {%016llx,%016llx} "
                                    "(precisely-old FP64 vector at low load)",
                                    cpu, r,
                                    (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(v), 0),
                                    (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(v), 1),
                                    (unsigned long long)f64_bits_of(last_seen[r][0]),
                                    (unsigned long long)f64_bits_of(last_seen[r][1]));
                }
                vst1q_f64(last_seen[r], vmaxq_f64(v, ls));   /* max update */
            }
        } while (llpn_cntvct() < window_end);

        /* ---- idle window: the 5%-30% duty cycle ---- */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = idle_ns };
        nanosleep(&ts, nullptr);
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(low_load_cache_pressure_neon_fp64, "FP64 variant: low-duty-cycle (duty_us knob, 5%-30%) cross-die NEON epoch-vector store -> remote vector-load sweep: precisely-old vectors at low load fail (ARM64)")
    .test_init = low_load_cache_pressure_neon_fp64_init,
    .test_run = low_load_cache_pressure_neon_fp64_run,
    .test_cleanup = low_load_cache_pressure_neon_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
