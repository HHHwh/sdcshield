/*  llcstress_neon_fp64 — FP64 data-path variant of tests/cpu/L3_cache_920/llcstress_neon.cpp
 *  (2026-11-10; parent untouched).
 *
 *  The random LLC roaming chassis is EXACTLY the parent's (64 MB shared
 *  window, band roaming, per-thread interleave slots, asm ldr/str, the
 *  GCC store-transform trap comment) — re-typed ONLY in the datapath:
 *  each slot is a float64x2 (2 f64 lanes in the same 16 bytes) and the
 *  RMW add is vaddq_f64(+1.0) — FP pipe / FP register file.  The slot
 *  itself IS the persistent golden (only its owner writes it); the
 *  per-band counts table is the independent second copy, compared in
 *  the immediate read-back bit-exact (veorq_u64 on the f64 bits).
 *  Band visit counts are integers far below 2^53, so f64 +1.0
 *  accumulation is exact (parent's mod-2^32 closure -> exact-domain
 *  equality; no wrap below 2^53).  Same-slot races stay impossible by
 *  the interleave; adjacent lines stay cross-thread.
 */
#include "sandstone.h"
#include <cstdint>
#include <vector>
#include <arm_neon.h>

static inline float64x2_t ld_vec(const double *a) {
    float64x2_t v;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(v) : "r"(a) : "memory");
    return v;
}
static inline void st_vec(double *a, float64x2_t v) {
    __asm__ volatile("str %q0, [%1]" :: "w"(v), "r"(a) : "memory");
}

static constexpr size_t LLSN_MEM_SIZE  = 64ULL * 1024 * 1024;  /* 64 MB  */
/* 2026-11-18 L3-capability rework (twin of the scalar parent's):
 * STRIDE_V 1024 -> 512 so a thread's own-slot footprint is 8192 lines
 * (512 KB = the private L2 boundary, parent parity), plus a DELAYED
 * RE-VERIFICATION sweep below. */
static constexpr size_t LLSN_STRIDE_V  = 512;    /* f64x2 slots per band  */
static constexpr size_t LLSN_BANDS     = LLSN_MEM_SIZE / 16 / LLSN_STRIDE_V;
static constexpr uint64_t LLSN_SWEEP_EVERY = 65536;

static int llcstress_neon_fp64_init(struct test *test) {
    /* throwing-new OOM follows the repo T10 convention (cf.
     * gemm_cdouble_dynamic_square): bad_alloc -> clean SKIP. */
    std::vector<uint8_t> *mem;
    try {
        mem = new std::vector<uint8_t>;
        mem->resize(LLSN_MEM_SIZE);
    } catch (const std::bad_alloc &) {
        delete mem;
        log_skip(RuntimeSkipCategory, "memory: %s 64MB window", test->id);
        return EXIT_SKIP;
    }
    test->data = mem;
    return EXIT_SUCCESS;
}

static int llcstress_neon_fp64_cleanup(struct test *test) {
    delete static_cast<std::vector<uint8_t> *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int llcstress_neon_fp64_run(struct test *test, int cpu) {
    auto *mem = static_cast<std::vector<uint8_t> *>(test->data);
    double *base = reinterpret_cast<double *>(mem->data());

    /* per-band count table: each band's slot holds THAT band's visit
     * count, so the golden must be per-band too (parent's structure,
     * kept verbatim — just re-typed to f64 lanes). */
    std::vector<double> counts(LLSN_BANDS * 2, 0.0);

    do {
        for (uint64_t batch = 0; batch < LLSN_SWEEP_EVERY; ++batch) {
            /* random band; our vector slot inside it — full-window
             * roaming, zero same-slot races across threads */
            size_t band = random64() % LLSN_BANDS;
            double *slot = base + (band * LLSN_STRIDE_V + (size_t)cpu) * 2;

            /* FP64 16B load-modify-store on the roamed slot */
            /* ALL slot accesses via asm: an intrinsic vst1q->vld1q pair
             * on the same address gets store-transformed + load-forwarded
             * by GCC (parent's measured trap) — asm cannot be. */
            float64x2_t v = ld_vec(slot);
            v = vaddq_f64(v, vdupq_n_f64(1.0));
            st_vec(slot, v);

            /* per-band golden (counts is private verification state —
             * intrinsic access is fine, it is not the path under test) */
            double *cnt = counts.data() + band * 2;
            float64x2_t c = vaddq_f64(vld1q_f64(cnt), vdupq_n_f64(1.0));
            vst1q_f64(cnt, c);

            /* read-back: the slot must hold exactly this band's count
             * (bit-exact: integer-valued f64 lanes) */
            float64x2_t mem_v = ld_vec(slot);
            uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(mem_v),
                                        vreinterpretq_u64_f64(c));
            if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                report_fail_msg("llcstress_neon_fp64: slot read-back mismatch "
                                "(band %zu): mem-bits {%016llx,%016llx} vs expected-bits "
                                "{%016llx,%016llx} (lost/corrupted FP64 store)",
                                band,
                                (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(mem_v), 0),
                                (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(mem_v), 1),
                                (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(c), 0),
                                (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(c), 1));
                return EXIT_FAILURE;
            }
        }

        /* L3 verification sweep (2026-11-18 rework): re-read ALL own
         * slots against the per-band counts shadow — delayed by a
         * full interval of random-band roaming, so everything except
         * the just-re-stored tail is L2-evicted and L3/DRAM-served. */
        for (size_t b = 0; b < LLSN_BANDS; ++b) {
            double *slot = base + (b * LLSN_STRIDE_V + (size_t)cpu) * 2;
            float64x2_t mem_v = ld_vec(slot);
            float64x2_t c = vld1q_f64(counts.data() + b * 2);
            uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(mem_v),
                                        vreinterpretq_u64_f64(c));
            if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                report_fail_msg("llcstress_neon_fp64: delayed re-verification "
                                "mismatch (band %zu): mem-bits {%016llx,%016llx} "
                                "vs expected-bits {%016llx,%016llx} (L3-resident "
                                "corruption)", b,
                                (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(mem_v), 0),
                                (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(mem_v), 1),
                                (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(c), 0),
                                (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(c), 1));
                return EXIT_FAILURE;
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(llcstress_neon_fp64, "FP64 variant of llcstress_neon: high-frequency random LLC roaming over 64MB with per-thread f64x2 interleave slots: asm ldr/vaddq_f64/str RMW + immediate read-back verify PLUS delayed re-verification sweep against the per-band counts shadow (L3-path, 2026-11-18 rework) (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = llcstress_neon_fp64_init,
    .test_run = llcstress_neon_fp64_run,
    .test_cleanup = llcstress_neon_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
