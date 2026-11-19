/*  low_load_cache_pressure_neon — NEON variant of
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
    alignas(64) uint32_t slot[LLPN_MAX_THREADS][4];   /* NEON epoch vectors */
    alignas(64) std::atomic<uint64_t> started[LLPN_MAX_THREADS];
    uint64_t duty_us;                /* duty knob, clamped once in test_init */
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

static int low_load_cache_pressure_neon_init(struct test *test) {
    auto *d = new LLPNData;
    if (!d) return EXIT_FAILURE;
    for (int i = 0; i < LLPN_MAX_THREADS; ++i) {
        for (int l = 0; l < 4; ++l) d->slot[i][l] = 0;
        d->started[i].store(0, std::memory_order_relaxed);
    }
    /* Test options are init-only (the framework's fail-loud check aborts on
     * any worker-thread query — main #580 / issue #417): clamp duty_us once
     * here and let run() lanes read the cached value. */
    uint64_t duty_us = get_testspecific_knob_value_uint(test, "duty_us", 1000);
    if (duty_us < 500)  duty_us = 500;
    if (duty_us > 3000) duty_us = 3000;
    d->duty_us = duty_us;
    test->data = d;
    return EXIT_SUCCESS;
}

static int low_load_cache_pressure_neon_cleanup(struct test *test) {
    delete static_cast<LLPNData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int low_load_cache_pressure_neon_run(struct test *test, int cpu) {
    auto *d = static_cast<LLPNData *>(test->data);

    /* duty knob: clamped and cached in test_init (knobs are init-only). */
    const uint64_t duty_us = d->duty_us;

    const uint64_t freq = llpn_timer_freq();
    const uint64_t active_ticks = duty_us * freq / 1000000ULL;
    const int64_t idle_ns = (int64_t)(LLPN_PERIOD_US - duty_us) * 1000LL;

    /* per-slot vector golden for the regression sweep */
    std::vector<uint32_t> last_seen_mem((size_t)LLPN_MAX_THREADS * 4, 0);
    uint32_t (*last_seen)[4] =
        reinterpret_cast<uint32_t (*)[4]>(last_seen_mem.data());

    uint64_t epoch = 0;
    d->started[cpu].store(1, std::memory_order_relaxed);

    do {
        const uint64_t window_end = llpn_cntvct() + active_ticks;

        /* ---- active window: NEON vector store + remote-load sweep ---- */
        do {
            ++epoch;
            uint32_t *mine = d->slot[cpu];
            uint32x4_t ev = vdupq_n_u32((uint32_t)epoch);
            st_vec(mine, ev);
            __asm__ volatile("dmb ish" ::: "memory");
            uint32x4_t back = ld_vec(mine);
            uint32x4_t diff = veorq_u32(back, ev);
            if (vmaxvq_u32(diff) != 0) {
                report_fail_msg("low_load_cache_pressure_neon: cpu %d "
                                "own-slot read-back differs (epoch %lu)",
                                cpu, (unsigned long)epoch);
            }

            for (int r = 0; r < LLPN_MAX_THREADS; ++r) {
                if (r != cpu
                    && d->started[r].load(std::memory_order_relaxed) == 0)
                    continue;
                uint32x4_t v = ld_vec(d->slot[r]);
                uint32x4_t ls = vld1q_u32(last_seen[r]);
                uint32x4_t reg = vcltq_u32(v, ls);   /* lane-wise regression */
                if (vmaxvq_u32(reg) != 0) {
                    report_fail_msg("low_load_cache_pressure_neon: cpu %d "
                                    "saw slot %d vector regress: now "
                                    "{%u,%u,%u,%u} after {%u,%u,%u,%u} "
                                    "(precisely-old vector at low load)",
                                    cpu, r,
                                    vgetq_lane_u32(v, 0), vgetq_lane_u32(v, 1),
                                    vgetq_lane_u32(v, 2), vgetq_lane_u32(v, 3),
                                    last_seen[r][0], last_seen[r][1],
                                    last_seen[r][2], last_seen[r][3]);
                }
                vst1q_u32(last_seen[r], vmaxq_u32(v, ls));   /* max update */
            }
        } while (llpn_cntvct() < window_end);

        /* ---- idle window: the 5%-30% duty cycle ---- */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = idle_ns };
        nanosleep(&ts, nullptr);
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(low_load_cache_pressure_neon, "Low-duty-cycle (duty_us knob, 5%-30%) cross-die NEON epoch-vector store -> remote vector-load sweep: precisely-old vectors at low load fail (ARM64)")
    .test_init = low_load_cache_pressure_neon_init,
    .test_run = low_load_cache_pressure_neon_run,
    .test_cleanup = low_load_cache_pressure_neon_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
