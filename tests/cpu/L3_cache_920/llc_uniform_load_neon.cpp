/*  llc_uniform_load_neon — NEON variant of tests/cpu/L3_cache_920/
 *  llc_uniform_load.cpp (2026-11-18 capability-rework propagation).
 *
 *  The uniform stride becomes a VECTOR stride: one uint32x4 load
 *  every 128 bytes across the per-thread window sized to the PROBED
 *  per-cluster L3 (the stride that trains the prefetcher — the #32
 *  stress), walk start rotating by one vector per pass.  Verification:
 *  the loaded vector is stored at the same index and read back through
 *  inline asm "ldr q" (anti-forwarding — intrinsic store->load pairs
 *  can still be register-folded), bit-compared with veorq/vmaxvq.
 *
 *  2026-11-18 L3-capability rework (twin of the scalar parent's):
 *  data[] gets an init-time golden COPY and a per-pass DENSE sweep
 *  (the stride walk covers only 1/8 of the window — one 16 B vector
 *  per 128 B stride; the sweep covers all of it) — window-sized
 *  traffic evicts the private L2, so the compare is L3/DRAM-served.
 *  The sweep is ELEMENT-EXACT per lane (2026-11-18 element-exact
 *  pass, same as the scalar parent): a sum-only check is blind to
 *  permutation/swap-class corruption; per-lane compare is not.
 */
#include "sandstone.h"
#include "sandstone_p.h"   /* device_count/device_info: L3 size probe */
#include <cstdint>
#include <cstring>
#include <vector>
#include <arm_neon.h>

/* Per-cluster L3 size (same 3-step probe as memcpy_l3_cache_size;
 * MUST be called from test_init — see the memcpy H8' lesson). */
static size_t lluln_l3_size(void) {
    int ndev = device_count();
    for (int i = 0; i < ndev; ++i) {
        int sz = device_info[i].cache[2].cache_data;   /* cache[2] = L3 */
        if (sz > 0)
            return static_cast<size_t>(sz);
    }
    long size = sysconf(_SC_LEVEL3_CACHE_SIZE);
    if (size > 0) return static_cast<size_t>(size);
    return 8 * 1024 * 1024;
}

static constexpr size_t LLULN_STRIDE_VECS = 128 / 16;   /* one vec per 128B */

static inline uint32x4_t ldr_vec(const uint32_t *addr) {
    uint32x4_t v;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(v) : "r"(addr) : "memory");
    return v;
}

static int llc_uniform_load_neon_init(struct test *test) {
    /* probe HERE — topology is filled by now (see lluln_l3_size).
     * throwing-new OOM path follows the repo's T10 convention
     * (cf. gemm_cdouble_dynamic_square): bad_alloc -> clean SKIP,
     * resource exhaustion is not an SDC red. */
    try {
        test->data = new size_t(lluln_l3_size());
    } catch (const std::bad_alloc &) {
        log_skip(RuntimeSkipCategory, "memory: %s l3-size word", test->id);
        return EXIT_SKIP;
    }
    return EXIT_SUCCESS;
}

static int llc_uniform_load_neon_cleanup(struct test *test) {
    delete static_cast<size_t *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int llc_uniform_load_neon_run(struct test *test, int cpu) {
    (void)cpu;

    const size_t buffer_bytes = *static_cast<size_t *>(test->data);
    const size_t num_vecs = buffer_bytes / 16;

    std::vector<uint32_t> data(num_vecs * 4);
    std::vector<uint32_t> golden(num_vecs * 4);
    std::vector<uint32_t> check(num_vecs * 4);

    memset_random(data.data(), buffer_bytes);
    memcpy(golden.data(), data.data(), buffer_bytes);
    memset(check.data(), 0, buffer_bytes);

    size_t offset = random64() % LLULN_STRIDE_VECS;

    do {
        for (size_t v = offset; v < num_vecs; v += LLULN_STRIDE_VECS) {
            uint32_t *dp = data.data() + v * 4;
            uint32_t *cp = check.data() + v * 4;

            /* uniform strided vector load — prefetcher stress */
            uint32x4_t original = vld1q_u32(dp);

            /* store -> REAL vector load-back through the LSU */
            vst1q_u32(cp, original);
            uint32x4_t loaded = ldr_vec(cp);

            uint32x4_t diff = veorq_u32(original, loaded);
            if (vmaxvq_u32(diff) != 0) {
                report_fail_msg("llc_uniform_load_neon: store->load "
                                "mismatch at vec %zu: stored {%08x,"
                                "%08x,%08x,%08x} loaded {%08x,%08x,"
                                "%08x,%08x} (LSU forwarding path under "
                                "strided access)",
                                v,
                                vgetq_lane_u32(original, 0), vgetq_lane_u32(original, 1),
                                vgetq_lane_u32(original, 2), vgetq_lane_u32(original, 3),
                                vgetq_lane_u32(loaded, 0), vgetq_lane_u32(loaded, 1),
                                vgetq_lane_u32(loaded, 2), vgetq_lane_u32(loaded, 3));
                return EXIT_FAILURE;
            }
        }
        offset = (offset + 1) % LLULN_STRIDE_VECS;

        /* L3 verification sweep (2026-11-18 element-exact rework): the
         * stride walk covers only 1/8 of the window (one 16 B vector
         * per 128 B stride); this DENSE pass re-reads every vector and
         * compares EACH LANE bit-exact against the init-time golden
         * copy — window-sized traffic evicts the private L2, so the
         * compare is L3/DRAM-served. Element-exact (not a sum):
         * permutation/swap-class L3 corruption fails here where a
         * sum-only check is order-blind. */
        for (size_t v = 0; v < num_vecs; ++v) {
            uint32x4_t x = vld1q_u32(data.data() + v * 4);
            uint32x4_t g = vld1q_u32(golden.data() + v * 4);
            uint32x4_t diff = veorq_u32(x, g);
            if (vmaxvq_u32(diff) != 0) {
                report_fail_msg("llc_uniform_load_neon: golden sweep "
                                "mismatch at vec %zu: golden {%08x,"
                                "%08x,%08x,%08x} got {%08x,%08x,%08x,"
                                "%08x} (L3-resident data corruption, "
                                "element-exact per-lane compare)",
                                v,
                                vgetq_lane_u32(g, 0), vgetq_lane_u32(g, 1),
                                vgetq_lane_u32(g, 2), vgetq_lane_u32(g, 3),
                                vgetq_lane_u32(x, 0), vgetq_lane_u32(x, 1),
                                vgetq_lane_u32(x, 2), vgetq_lane_u32(x, 3));
                return EXIT_FAILURE;
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(llc_uniform_load_neon, "Uniform 128B-strided vld1q vector loads over per-thread windows sized to the probed per-cluster L3 (prefetcher stress); store->asm-ldr-q vector load-back PLUS per-pass dense element-exact per-lane golden sweep (L3-path verification, 2026-11-18 rework) (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = llc_uniform_load_neon_init,
    .test_run = llc_uniform_load_neon_run,
    .test_cleanup = llc_uniform_load_neon_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
