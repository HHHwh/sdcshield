/*  cachebounce_neon — NEON variant of tests/cpu/cache/cachebounce.cpp.
 *
 *  Original: all cores fetch_add a single shared atomic counter — the
 *  cacheline bounces between cores, integer RMW path.
 *  This variant: the bouncing payload is a 64-byte NEON line holding one
 *  uint32x4 slot per core (up to 16 in the first line; the array extends
 *  by lines).  Each core vld1q's ITS OWN slot, adds a lane-uniform +1
 *  with vaddq, vst1q's it back — a NEON load-modify-store whose line is
 *  shared by every worker (full-cadence false sharing, the locv-B2
 *  excitation shape) instead of a coherent atomic RMW.
 *
 *  Golden independence: the shared slot IS the golden (each core's slot
 *  counts that core's own iterations; nothing else writes it), and the
 *  per-core register local is the second, independent copy — cleanup
 *  compares slot == local, so a lost or corrupted NEON store shows as a
 *  mismatch exactly like the original's counter==total check.
 *
 *  Original test untouched; x86-64 untouched (aarch64-only registration).
 */
#include "sandstone.h"
#include <cstdint>
#include <arm_neon.h>

#define CBN_MAX_THREADS 1024
#define CBN_LINE_U32X4  4        /* uint32x4 slots per 64B line */

namespace { /* ODR: per-TU CBNData */
struct CBNData {
    /* shared bouncing window: slot i belongs to core i */
    alignas(64) uint32_t slots[CBN_MAX_THREADS * 4];
};
}

static inline uint32x4_t cbn_ld_vec(const uint32_t *a) {
    uint32x4_t v;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(v) : "r"(a) : "memory");
    return v;
}
static inline void cbn_st_vec(uint32_t *a, uint32x4_t v) {
    __asm__ volatile("str %q0, [%1]" :: "w"(v), "r"(a) : "memory");
}

static int cachebounce_neon_init(struct test *test) {
    auto *d = new CBNData;
    if (!d) return EXIT_FAILURE;
    for (int i = 0; i < CBN_MAX_THREADS * 4; ++i)
        d->slots[i] = 0;
    test->data = d;
    return EXIT_SUCCESS;
}

static int cachebounce_neon_cleanup(struct test *test) {
    auto *d = static_cast<CBNData *>(test->data);
    delete d;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int cachebounce_neon_run(struct test *test, int cpu) {
    auto *d = static_cast<CBNData *>(test->data);
    uint32_t *slot = &d->slots[cpu * 4];       /* this core's uint32x4 */

    uint32x4_t local = vdupq_n_u32(0);         /* register golden */

    do {
        /* NEON RMW on a line shared by every core — slot accesses
         * via asm (intrinsic store->load pairs get GCC-forwarded,
         * silently degrading the verify to constant-true) */
        uint32x4_t v = cbn_ld_vec(slot);
        v = vaddq_u32(v, vdupq_n_u32(1));
        cbn_st_vec(slot, v);
        local = vaddq_u32(local, vdupq_n_u32(1));
    } while (test_time_condition(test));

    /* the shared slot must hold exactly this core's own iterations */
    uint32x4_t mem = cbn_ld_vec(slot);
    uint32x4_t diff = veorq_u32(mem, local);
    if (vmaxvq_u32(diff) != 0) {
        report_fail_msg("cachebounce_neon: cpu %d slot mismatch: "
                        "mem {%u,%u,%u,%u} vs local {%u,%u,%u,%u} "
                        "(lost or corrupted NEON store on the shared "
                        "bouncing line)", cpu,
                        vgetq_lane_u32(mem, 0), vgetq_lane_u32(mem, 1),
                        vgetq_lane_u32(mem, 2), vgetq_lane_u32(mem, 3),
                        vgetq_lane_u32(local, 0), vgetq_lane_u32(local, 1),
                        vgetq_lane_u32(local, 2), vgetq_lane_u32(local, 3));
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(cachebounce_neon, "NEON variant of cachebounce: per-core uint32x4 slots in a shared 64B bouncing line, vld1q/vaddq/vst1q RMW with register golden (ARM64)")
    .test_init = cachebounce_neon_init,
    .test_run = cachebounce_neon_run,
    .test_cleanup = cachebounce_neon_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
