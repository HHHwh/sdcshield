/*  sfence_neon_fp64 — FP64 datapath variant of tests/cpu/arm_add/sfence_neon.cpp
 *  (2026-11-10; parent untouched).
 *
 *  Same barrier structure, with the payload re-typed where the FP
 *  register file genuinely changes the physical path:
 *   1. NON-TEMPORAL group: the scalar STNP pair-writes move from the
 *      GPR (stnp %w0,%w1) to the FP register file (stnp %d0,%d1 —
 *      d-register NT pair; one pair covers a full 16B vector).  This
 *      is the real datapath change of this variant: NT stores issued
 *      from FP registers through the same STNP completion rules.
 *   2. DMB ISHST — the SFENCE (unchanged);
 *   3. REGULAR group: kept as the parent's 16B vst1q_u32/vld1q_u32 —
 *      the q-register path is ALREADY the vector/FP store port, a
 *      u32->f64 lane re-view adds no physical resource (the honest
 *      same-reason the pure q-pass-through variants were skipped);
 *   4. DSB SY drain (unchanged);
 *   5. verify: asm "ldr q" anti-forwarding sweep, bit-compared against
 *      the fresh per-round golden (unchanged).
 */
#include "sandstone.h"
#include <cstdint>
#include <vector>
#include <arm_neon.h>

static constexpr size_t SFN_NUM_VECS = 512;   /* 512 x 16B = 8 KiB */

static inline void dmb_ishst() {
    __asm__ volatile("dmb ishst" ::: "memory");
}

static inline void dsb_sy() {
    __asm__ volatile("dsb sy" ::: "memory");
}

/* FP-register non-temporal pair store — the NT datapath change:
 * d registers instead of the parent's w registers */
static inline void stnp_pair_f64(double *addr, double d0, double d1) {
    __asm__ volatile("stnp %d0, %d1, [%2]"
                     :: "w"(d0), "w"(d1), "r"(addr) : "memory");
}

/* anti-forwarding VECTOR load */
static inline uint32x4_t ldr_vec(const uint32_t *addr) {
    uint32x4_t v;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(v) : "r"(addr) : "memory");
    return v;
}

static int sfence_neon_fp64_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int sfence_neon_fp64_cleanup(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int sfence_neon_fp64_run(struct test *test, int cpu) {
    (void)cpu;

    std::vector<uint32_t> buf(SFN_NUM_VECS * 4);
    std::vector<uint32_t> golden(SFN_NUM_VECS * 4);

    uint64_t rounds = 0;

    do {
        /* fresh independent golden */
        for (size_t i = 0; i < SFN_NUM_VECS * 4; ++i)
            golden[i] = (uint32_t)random64();

        /* which half of the vectors goes non-temporal rotates */
        const unsigned phase = (unsigned)(rounds & 1);

        /* 1. non-temporal group: FP-REGISTER stnp d,d pair-writes —
         * one 16B pair per vector, half the vectors, rotating */
        for (size_t v = 0; v < SFN_NUM_VECS; ++v) {
            if (((v + phase) & 1) == 0) {
                const double *g = reinterpret_cast<const double *>(&golden[v * 4]);
                double *b      = reinterpret_cast<double *>(&buf[v * 4]);
                stnp_pair_f64(b, g[0], g[1]);
            }
        }

        /* 2. the SFENCE */
        dmb_ishst();

        /* 3. regular group: the parent's 16B vector stores (q path
         * kept — see header: no physical resource change in re-view) */
        for (size_t v = 0; v < SFN_NUM_VECS; ++v) {
            if (((v + phase) & 1) != 0)
                vst1q_u32(&buf[v * 4], vld1q_u32(&golden[v * 4]));
        }

        /* 4. completion drain before any read-back */
        dsb_sy();

        /* 5. full asm-ldr vector sweep vs golden */
        for (size_t v = 0; v < SFN_NUM_VECS; ++v) {
            uint32x4_t got = ldr_vec(&buf[v * 4]);
            uint32x4_t want = vld1q_u32(&golden[v * 4]);
            uint32x4_t diff = veorq_u32(got, want);
            if (vmaxvq_u32(diff) != 0) {
                uint32_t o[4];
                vst1q_u32(o, got);
                report_fail_msg("sfence_neon_fp64: vector read-back mismatch "
                                "at vec %zu (round %lu, %s lane): "
                                "expected {%08x,%08x,%08x,%08x} got "
                                "{%08x,%08x,%08x,%08x}",
                                v, (unsigned long)rounds,
                                ((v + phase) & 1) ? "regular" :
                                "non-temporal-FP64",
                                golden[v * 4], golden[v * 4 + 1],
                                golden[v * 4 + 2], golden[v * 4 + 3],
                                o[0], o[1], o[2], o[3]);
                return EXIT_FAILURE;
            }
        }

        ++rounds;
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(sfence_neon_fp64, "FP64 variant of sfence_neon: dmb-ishst between FP-register STNP d,d non-temporal pairs and vst1q regular vector stores; dsb-sy drain; full asm-ldr-q vector bit verification (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sfence_neon_fp64_init,
    .test_run = sfence_neon_fp64_run,
    .test_cleanup = sfence_neon_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
