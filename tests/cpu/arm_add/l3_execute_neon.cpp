/*  l3_execute_neon — NEON variant of tests/cpu/cache/l3_execute.cpp
 *  (same-day NEON pass; original untouched).
 *
 *  Same fetch-path probe structure — ic ivau invalidation of the
 *  worker's code + dsb ish + isb resync forces re-fetch through
 *  L2/L3, executed results bit-compared to the INIT-TIME snapshot —
 *  but the worker's compute kernel is the NEON vector pipeline: a
 *  fixed veorq/vmulq/vaddq/vshlq chain over uint32x4, so the code
 *  being re-fetched is NEON-heavy machine code (the fetch corruption
 *  of a NEON instruction word is what this variant would surface).
 *
 *  Same-source cross-time recompute (eigen pattern), single worker
 *  function (no ICF hazard by construction), deterministic inputs —
 *  here not even FP: pure integer NEON, so the bit snapshot is exact.
 */
#include "sandstone.h"
#include <cstdint>
#include <cstring>
#include <arm_neon.h>

static constexpr int    L3EN_NUM_INPUTS = 64;
static constexpr size_t L3EN_FLUSH_BYTES = 256;
static constexpr size_t L3EN_LINE = 64;

/* The function under test: fixed integer-NEON compute chain, noinline
 * so every call is a real fetch+execute of THIS code. */
static __attribute__((noinline)) uint32x4_t l3en_worker(uint32x4_t v, uint32_t round) {
    const uint32x4_t k1 = vdupq_n_u32(0x2545F491u);
    const uint32x4_t k2 = vdupq_n_u32(0x9E3779B1u);
    uint32x4_t a = veorq_u32(v, k1);
    uint32x4_t b = vaddq_u32(v, vdupq_n_u32(round));
    for (int i = 0; i < 8; ++i) {
        a = veorq_u32(a, vmulq_u32(b, k1));
        b = vaddq_u32(b, vshlq_n_u32(a, 3));
    }
    return veorq_u32(a, vmulq_u32(b, k2));
}

namespace { /* ODR: per-TU L3ENData */
struct L3ENData {
    uint32_t in[L3EN_NUM_INPUTS][4];       /* deterministic vector inputs */
    uint32_t golden[L3EN_NUM_INPUTS][4];   /* init-time bit snapshot */
};
}

/* 2026-11-18 L3-capability rework (twin of the scalar parent's): the
 * dc civac pass (clean+invalidate to the PoC) EVICTS the code lines
 * from the private unified L2 first — bare ic ivau only reaches the
 * PoU = L2 and the refetch stayed L2-served.  After civac + ivau the
 * next fetch misses L1I AND L2 and is genuinely L3-served. */
static inline void l3en_flush_icache(const void *start, size_t len) {
    const char *base = reinterpret_cast<const char *>(
        reinterpret_cast<uintptr_t>(start) & ~(uintptr_t)(L3EN_LINE - 1));
    const char *end = reinterpret_cast<const char *>(start) + len;
    for (const char *p = base; p < end; p += L3EN_LINE)
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (const char *p = base; p < end; p += L3EN_LINE)
        __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

static int l3_execute_neon_init(struct test *test) {
    auto *d = new L3ENData;
    if (!d) return EXIT_FAILURE;
    for (int i = 0; i < L3EN_NUM_INPUTS; ++i) {
        for (int l = 0; l < 4; ++l)
            d->in[i][l] = (uint32_t)(i * 0x9E3779B1u + l * 0x85EBCA6Bu);
        uint32x4_t v = vld1q_u32(d->in[i]);
        uint32x4_t r = l3en_worker(v, (uint32_t)i);
        vst1q_u32(d->golden[i], r);
    }
    test->data = d;
    return EXIT_SUCCESS;
}

static int l3_execute_neon_cleanup(struct test *test) {
    delete static_cast<L3ENData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int l3_execute_neon_run(struct test *test, int cpu) {
    (void)cpu;
    auto *d = static_cast<L3ENData *>(test->data);

    do {
        /* force the next call to re-fetch the NEON code from L2/L3 */
        l3en_flush_icache(reinterpret_cast<const void *>(&l3en_worker),
                          L3EN_FLUSH_BYTES);

        for (int i = 0; i < L3EN_NUM_INPUTS; ++i) {
            uint32x4_t v = vld1q_u32(d->in[i]);
            uint32x4_t r = l3en_worker(v, (uint32_t)i);

            uint32x4_t g = vld1q_u32(d->golden[i]);
            uint32x4_t diff = veorq_u32(r, g);
            if (vmaxvq_u32(diff) != 0) {
                uint32_t out[4];
                vst1q_u32(out, r);
                report_fail_msg("l3_execute_neon: NEON execution differs "
                                "from init snapshot after I-cache reload: "
                                "input %d: expected {%08x,%08x,%08x,%08x} "
                                "got {%08x,%08x,%08x,%08x} (fetch-path "
                                "corruption of NEON code?)", i,
                                d->golden[i][0], d->golden[i][1],
                                d->golden[i][2], d->golden[i][3],
                                out[0], out[1], out[2], out[3]);
                return EXIT_FAILURE;
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(l3_execute_neon, "Reads and executes NEON code from L3: dc-civac evicts the code lines from L2 (to PoC) before ic-ivau + dsb/isb, so the re-fetch of the veorq/vmulq/vaddq/vshlq kernel is genuinely L3-served; results bit-compared to the init snapshot (L3->ICache fetch-path SDC on NEON code, 2026-11-18 rework) (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = l3_execute_neon_init,
    .test_run = l3_execute_neon_run,
    .test_cleanup = l3_execute_neon_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
