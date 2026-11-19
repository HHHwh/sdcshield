/*  NEON variant of tests/cpu/arm_add/cache_stress_memcpy_neon.cpp — same-day NEON pass:
 *  the copy load points become the explicit vld1q/vst1q stream
 *  (arm_add_neon_copy.h); flush/verify semantics identical.  The
  *  scalar original stays; identifiers carry the _neon suffix (ODR).
 */
/*  cache_stress_memcpy_neon — pure memory copy control (633 #211 "Memory
 *  copy only (REP MOVS, no flush)").
 *
 *  The CONTROL ARM of cache_stress_clflush (633 #210): byte-for-byte the
 *  same copy + verify workload — same 64 KB buffers, same anti-fold dst
 *  pre-corruption, same independent golden — with ZERO cache-flush
 *  interference.  Any failure here that does not reproduce under
 *  cache_stress_clflush localises the fault to the flush/copy interplay
 *  rather than the copy path itself; failures appearing ONLY under
 *  clflush are the eviction-pressure signature.
 *
 *  ARM64 equivalent of REP MOVS: the plain memcpy loop (compiler emits
 *  ldp/stp pairs at -O2) — the spec explicitly names "memcpy 库调用或
 *  LDP/STP 循环" as the accepted equivalent, and the instruction under
 *  study in the #210/#211 pair is the FLUSH, not the copy.
 *
 *  x86-64 untouched: registered only under the aarch64 guard in
 *  tests/cpu/meson.build.
 */
#include "sandstone.h"
#include "arm_add_neon_copy.h"
using arm_add_neon::neon_copy;
#include <cstdint>
#include <cstring>
#include "arm_add_excite.h"

#define CSM_SIZE (64 * 1024)

namespace { /* ODR: per-TU CSMData */
struct CSMData {
    uint64_t *src;
    uint64_t *dst;
    uint64_t *golden;
};
}

static int cache_stress_memcpy_init(struct test *test) {
    (void)test;   /* buffers are per-thread (thread-safety fix, see original) */
    return EXIT_SUCCESS;
}

static int cache_stress_memcpy_cleanup(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int cache_stress_memcpy_run(struct test *test, int cpu) {
    (void)test;
    uint64_t epoch = 0;

    /* PER-THREAD buffers — thread-safety fix shared with the scalar
     * original (full-core campaign finding; -n 64 verification floor) */
    uint64_t *src    = new uint64_t[CSM_SIZE / 8];
    uint64_t *dst    = new uint64_t[CSM_SIZE / 8];
    uint64_t *golden = new uint64_t[CSM_SIZE / 8];
    if (!src || !dst || !golden) return EXIT_FAILURE;

    memset_random(src, CSM_SIZE);
    neon_copy(golden, src, CSM_SIZE);   /* independent golden snapshot */
    memset(dst, 0, CSM_SIZE);

    do {
        ++epoch;
        const uint64_t poison = ~epoch;

        /* pre-corrupt dst (anti-golden-fold, identical to clflush arm) */
        for (size_t i = 0; i < CSM_SIZE / 8; ++i)
            dst[i] = poison;

        /* pure copy — no flush interference anywhere */
        neon_copy(dst, src, CSM_SIZE);

        /* verify against the independent golden */
        if (memcmp(dst, golden, CSM_SIZE) != 0) {
            for (size_t i = 0; i < CSM_SIZE / 8; ++i) { /* full scan (was sampled i += 8: off-grid flips produced no error and the framework judged pass) */
                if (golden[i] != dst[i]) {
                    report_fail_msg("cache_stress_memcpy_neon: dst qword %zu "
                                    "differs from golden with NO flush "
                                    "present (g=%016lx a=%016lx) — pure "
                                    "copy-path failure, the control arm",
                                    i, (unsigned long)golden[i],
                                    (unsigned long)dst[i]);
                }
            }
        }

        /* excitation filler: NEON switching power (see arm_add_excite.h) */
        if (!arm_add_excite::fill(cpu, epoch)) {
            report_fail_msg("excitation filler store->reload mismatch "
                            "(cpu %d round %lu)", cpu,
                            (unsigned long)epoch);
        }
    } while (test_time_condition(test));

    delete[] src;
    delete[] dst;
    delete[] golden;
    return EXIT_SUCCESS;
}

DECLARE_TEST(cache_stress_memcpy_neon, "Pure memory copy control, no flush (633 #211 REP MOVS equivalent): the zero-interference arm of cache_stress_clflush (ARM64)")
    .test_init = cache_stress_memcpy_init,
    .test_run = cache_stress_memcpy_run,
    .test_cleanup = cache_stress_memcpy_cleanup,
    .fracture_loop_count = -1,   /* never fracture: our semantic rounds are microseconds; auto mode (40) ends the run in ~10 ms */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
