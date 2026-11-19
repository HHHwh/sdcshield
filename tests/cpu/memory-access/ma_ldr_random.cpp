/*  ma_ldr_random — memory-access tail-probe family, load head: random-index ldr scan
 *  (2026-11-04).  The swept variable is the LOAD HEAD; the tail access
 *  pair (str->str  /  ldr->str->str, back-to-back asm, <=2-cycle
 *  store->load forwarding window) and the golden mechanism (init
 *  snapshot + rotating 16-word segment anti-fold) are the shared
 *  ma_tail_probe.h constants.  Fault signature being probed: after a
 *  load burst, the second of two adjacent stores (or the pair after an
 *  adjacent ldr) mis-places data under pressure.
 */
#include "ma_tail_probe.h"
#include <arm_neon.h>

namespace { /* ODR: per-TU MA_LDR_RANDOM_T */
struct MA_LDR_RANDOM_T {
    ma_probe::MAData probe;
    uint32_t scratch[MA_WORDS];
};
}

static int ma_ldr_random_init(struct test *test) {
    auto *d = new MA_LDR_RANDOM_T;
    if (!d) return EXIT_FAILURE;
    ma_probe::ma_init(&d->probe);
    memset(d->scratch, 0, sizeof(d->scratch));
    test->data = d;
    return EXIT_SUCCESS;
}

static int ma_ldr_random_cleanup(struct test *test) {
    delete static_cast<MA_LDR_RANDOM_T *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int ma_ldr_random_run(struct test *test, int cpu) {
    auto *d = static_cast<MA_LDR_RANDOM_T *>(test->data);
    (void)cpu;
    uint64_t round = 0;

    do {
        ++round;
        /* LOAD HEAD: random-index scalar ldr scan over the 16KiB source */
        uint32_t sink = 0;
        for (int k = 0; k < 256; ++k) {
            const uint32_t idx = (uint32_t)(random64() % MA_WORDS);
            sink ^= ma_probe::ma_ldr(&d->probe.src[idx]);
        }
        __asm__ volatile("" :: "r"(sink) : "memory");   /* keep the loads live */

        /* TAIL PROBE: 16-word segment, rotating every round; the two
         * modes alternate by round parity */
        const uint32_t base =
            (uint32_t)((round * MA_SEG) % (MA_WORDS - MA_SEG));
        const bool mode_b = (round & 1) != 0;
        for (int i = 0; i < MA_SEG; ++i) {
            const bool ok = mode_b
                ? ma_probe::probe_ldr_str_str(&d->probe, base, i)
                : ma_probe::probe_str_str(&d->probe, base, i);
            if (!ok) {
                report_fail_msg("ma_ldr_random: tail access-pair mismatch at "
                                "word %u (round %lu, mode %s): temp=0x%08x "
                                "dst=0x%08x golden=0x%08x (mis-placed "
                                "store after load burst)",
                                base + (uint32_t)i, (unsigned long)round,
                                mode_b ? "ldr+str+str" : "str+str",
                                d->probe.temp[base + i], d->probe.dst[base + i],
                                d->probe.golden[base + i]);
                return EXIT_FAILURE;
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(ma_ldr_random, "Tail access-pair probe after random-index ldr scan load: str->str / ldr->str->str back-to-back asm with init-snapshot golden (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = ma_ldr_random_init,
    .test_run = ma_ldr_random_run,
    .test_cleanup = ma_ldr_random_cleanup,
    .fracture_loop_count = -1,   /* never fracture (family convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
