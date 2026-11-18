/*  low_load_cache_pressure — low-duty-cycle cross-die store -> remote
 *  load, sustained.
 *
 *  Event-report lineage: "the problem occurs at LOW load (5%-30%)" — the
 *  existing 633-suite coverage is all high-pressure, nothing sweeps the
 *  low-load regime where the failure actually shows.  Probe design:
 *
 *    period  : 10 ms wall clock (nanosleep for the idle window — sleep
 *              IS the design here, the low duty cycle is the payload,
 *              not a violation of the no-sleep hot-loop convention).
 *    active  : knob-controlled active window (duty_us, default 1000 =
 *              10%; accepted range 500..3000 = 5%..30% of the period).
 *              Window boundaries are read from the architectural timer
 *              (cntvct_el0 — one register read, no syscall).
 *    in-window work:
 *      - high-density `str` of a monotonically increasing seq into the
 *        core's OWN slot, followed by `dmb ish` and a same-core ldr
 *        read-back (layer 1: locally lost write check);
 *      - sweep `ldr` over every started core's slot (remote loads —
 *        cross-cluster/die by construction, whoever is up) and verify
 *        NO slot ever goes backwards vs. this core's last sweep.
 *
 *  Fail criteria: (1) own-slot immediate read-back != just-stored epoch;
 *  (2) any remote slot's observed value regresses (a precisely-old value
 *  observed at low load — the reported failure regime).
 *
 *  -n 1: the sweep reads only the core's own slot; the store/read-back
 *  ladder plus the low-duty rhythm are fully exercised.
 */
#include "sandstone.h"
#include <cstdint>
#include <ctime>
#include <atomic>
#include "arm_add_excite.h"

#define LLP_MAX_THREADS 1024
#define LLP_PERIOD_NS   10000000LL   /* 10 ms */
#define LLP_PERIOD_US   10000

namespace { /* ODR: per-TU LLPData */
struct LLPData {
    alignas(64) uint64_t slot[LLP_MAX_THREADS];  /* plain str/ldr via asm */
    alignas(64) std::atomic<uint64_t> started[LLP_MAX_THREADS];
    uint64_t duty_us;                /* duty knob, clamped once in test_init */
};
}

static inline void llp_str(uint64_t *addr, uint64_t val) {
    __asm__ volatile("str %0, [%1]" :: "r"(val), "r"(addr) : "memory");
}

static inline uint64_t llp_ldr(const uint64_t *addr) {
    uint64_t v;
    __asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static inline uint64_t llp_cntvct(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static inline uint64_t llp_timer_freq(void) {
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
}

static int low_load_cache_pressure_init(struct test *test) {
    auto *d = new LLPData;
    if (!d) return EXIT_FAILURE;
    for (int i = 0; i < LLP_MAX_THREADS; ++i) {
        d->slot[i] = 0;
        d->started[i].store(0, std::memory_order_relaxed);
    }
    /* duty knob: active-window length in us (5%..30% of the 10 ms period).
     * Test options are init-only (the framework's fail-loud check aborts on
     * any worker-thread query — main #580 / issue #417), so clamp here once
     * and let run() read the cached value. */
    uint64_t duty_us = get_testspecific_knob_value_uint(test, "duty_us", 1000);
    if (duty_us < 500)  duty_us = 500;
    if (duty_us > 3000) duty_us = 3000;
    d->duty_us = duty_us;
    test->data = d;
    return EXIT_SUCCESS;
}

static int low_load_cache_pressure_cleanup(struct test *test) {
    delete static_cast<LLPData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int low_load_cache_pressure_run(struct test *test, int cpu) {
    auto *d = static_cast<LLPData *>(test->data);

    /* duty knob: active-window length in us (5%..30% of the 10 ms period),
     * clamped and cached in test_init (knobs are init-only). */
    const uint64_t duty_us = d->duty_us;

    const uint64_t freq = llp_timer_freq();          /* ticks per second */
    const uint64_t active_ticks = duty_us * freq / 1000000ULL;
    const int64_t idle_ns = (int64_t)(LLP_PERIOD_US - duty_us) * 1000LL;

    uint64_t last_seen[LLP_MAX_THREADS];
    for (int i = 0; i < LLP_MAX_THREADS; ++i) last_seen[i] = 0;

    uint64_t epoch = 0;
    d->started[cpu].store(1, std::memory_order_relaxed);

    do {
        const uint64_t window_end = llp_cntvct() + active_ticks;

        /* ---- active window: dense store + remote-load sweep ---- */
        do {
            ++epoch;
            uint64_t *mine = &d->slot[cpu];
            llp_str(mine, epoch);
            __asm__ volatile("dmb ish" ::: "memory");
            uint64_t back = llp_ldr(mine);
            if (back != epoch) {
                report_fail_msg("low_load_cache_pressure: cpu %d own-slot "
                                "read-back %lu, wrote %lu (low-load lost "
                                "write)", cpu, (unsigned long)back,
                                (unsigned long)epoch);
            }

            for (int r = 0; r < LLP_MAX_THREADS; ++r) {
                if (r != cpu
                    && d->started[r].load(std::memory_order_relaxed) == 0)
                    continue;
                uint64_t v = llp_ldr(&d->slot[r]);
                if (v < last_seen[r]) {
                    report_fail_msg("low_load_cache_pressure: cpu %d saw "
                                    "slot %d regress: %lu after %lu "
                                    "(precisely-old value at low load)",
                                    cpu, r, (unsigned long)v,
                                    (unsigned long)last_seen[r]);
                }
                if (v > last_seen[r]) last_seen[r] = v;
            }
        } while (llp_cntvct() < window_end);
            /* excitation filler: NEON switching power (see arm_add_excite.h) */
            if (!arm_add_excite::fill(cpu, epoch)) {
                report_fail_msg("excitation filler store->reload mismatch "
                                "(cpu %d round %lu)", cpu,
                                (unsigned long)epoch);
            }


        /* ---- idle window: the 5%-30% duty cycle ---- */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = idle_ns };
        nanosleep(&ts, nullptr);
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(low_load_cache_pressure, "Low-duty-cycle (5%-30%, knob duty_us) cross-die store -> remote load sweep: precisely-old values at low load fail (ARM64)")
    .test_init = low_load_cache_pressure_init,
    .test_run = low_load_cache_pressure_run,
    .test_cleanup = low_load_cache_pressure_cleanup,
    .fracture_loop_count = -1,   /* never fracture: our semantic rounds are microseconds; auto mode (40) ends the run in ~10 ms */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
