/*  sfence_neon — NEON variant of tests/cpu/cache/sfence.cpp (same-day
 *  NEON pass; original untouched).
 *
 *  Same barrier structure, mixed-store workload widened on the
 *  regular side:
 *   1. NON-TEMPORAL group: STNP pair-writes (the scalar NT store —
 *      ARM64 has no VECTOR non-temporal store; STNP stays scalar by
 *      necessity), half the pairs, rotating;
 *   2. DMB ISHST — the SFENCE: NT stores ordered before everything
 *      the regular group is about to issue;
 *   3. REGULAR group: the other half written as 16-byte vst1q_u32
 *      VECTOR stores (the NEON datapath this variant adds);
 *   4. DSB SY — completion drain (STNP weak-completion fence);
 *   5. verify: every vector read back through inline asm "ldr q"
 *      (anti-forwarding) and bit-compared against golden.
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

/* scalar non-temporal pair store (no vector NT exists on ARM64) */
static inline void stnp_pair(uint32_t *addr, uint32_t w0, uint32_t w1) {
    __asm__ volatile("stnp %w0, %w1, [%2]"
                     :: "r"(w0), "r"(w1), "r"(addr) : "memory");
}

/* anti-forwarding VECTOR load */
static inline uint32x4_t ldr_vec(const uint32_t *addr) {
    uint32x4_t v;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(v) : "r"(addr) : "memory");
    return v;
}

static int sfence_neon_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int sfence_neon_cleanup(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int sfence_neon_run(struct test *test, int cpu) {
    (void)cpu;

    std::vector<uint32_t> buf(SFN_NUM_VECS * 4);
    std::vector<uint32_t> golden(SFN_NUM_VECS * 4);

    uint64_t rounds = 0;

    do {
        /* fresh independent golden */
        for (size_t i = 0; i < SFN_NUM_VECS * 4; ++i)
            golden[i] = (uint32_t)random64();

        /* which half of the 8B pairs goes non-temporal rotates */
        const unsigned phase = (unsigned)(rounds & 1);

        /* 1. non-temporal group: scalar STNP pair-writes, TWO pairs
         * per vector (each 16B vector is written entirely by STNP —
         * full-coverage model, half the vectors, rotating) */
        for (size_t v = 0; v < SFN_NUM_VECS; ++v) {
            if (((v + phase) & 1) == 0) {
                stnp_pair(&buf[v * 4],     golden[v * 4],     golden[v * 4 + 1]);
                stnp_pair(&buf[v * 4 + 2], golden[v * 4 + 2], golden[v * 4 + 3]);
            }
        }

        /* 2. the SFENCE */
        dmb_ishst();

        /* 3. regular group: 16B NEON vector stores for the other half */
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
                report_fail_msg("sfence_neon: vector read-back mismatch "
                                "at vec %zu (round %lu, %s lane): "
                                "expected {%08x,%08x,%08x,%08x} got "
                                "{%08x,%08x,%08x,%08x}",
                                v, (unsigned long)rounds,
                                ((v + phase) & 1) ? "regular" :
                                "non-temporal",
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

DECLARE_TEST(sfence_neon, "SFENCE via dmb-ishst between scalar STNP non-temporal pairs and vst1q regular vector stores; dsb-sy drain; full asm-ldr-q vector bit verification (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = sfence_neon_init,
    .test_run = sfence_neon_run,
    .test_cleanup = sfence_neon_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
