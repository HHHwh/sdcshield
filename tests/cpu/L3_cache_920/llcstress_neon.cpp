/*  llcstress_neon — NEON variant of tests/cpu/cache/llcstress.cpp
 *  (same-day NEON pass; original untouched).
 *
 *  The random LLC roaming is unchanged (64 MB shared window, larger
 *  than LLC — low hit rates, replacement-policy pressure), but the
 *  stress pair is now a 16-byte NEON load-modify-store: each thread
 *  owns every uint32x4 slot congruent to cpu (mod 1024) — one vector
 *  per 16 KB band — and roams random bands doing vld1q_u32 /
 *  vaddq_u32(+1) / vst1q_u32 on its own slot.  The slot itself IS
 *  the persistent golden (only its owner writes it); the register
 *  local is the independent second copy, compared in the read-back —
 *  a lost or corrupted vector store shows exactly like the scalar
 *  original's read-back mismatch.  Same-slot races are impossible by
 *  the interleave; adjacent lines stay cross-thread (false-sharing
 *  cadence preserved).
 */
#include "sandstone.h"
#include <cstdint>
#include <vector>
#include <arm_neon.h>

static inline uint32x4_t ld_vec(const uint32_t *a) {
    uint32x4_t v;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(v) : "r"(a) : "memory");
    return v;
}
static inline void st_vec(uint32_t *a, uint32x4_t v) {
    __asm__ volatile("str %q0, [%1]" :: "w"(v), "r"(a) : "memory");
}

static constexpr size_t LLSN_MEM_SIZE  = 64ULL * 1024 * 1024;  /* 64 MB  */
/* 2026-11-18 L3-capability rework (twin of the scalar parent's):
 * STRIDE_V 1024 -> 512 so a thread's own-slot footprint is 8192 lines
 * (512 KB = the private L2 boundary, parent parity), plus a DELAYED
 * RE-VERIFICATION sweep below. */
static constexpr size_t LLSN_STRIDE_V  = 512;    /* uint32x4 slots per band */
static constexpr size_t LLSN_BANDS     = LLSN_MEM_SIZE / 16 / LLSN_STRIDE_V;
static constexpr uint64_t LLSN_SWEEP_EVERY = 65536;

static int llcstress_neon_init(struct test *test) {
    auto *mem = new std::vector<uint8_t>;
    if (!mem) return EXIT_FAILURE;
    mem->resize(LLSN_MEM_SIZE);
    test->data = mem;
    return EXIT_SUCCESS;
}

static int llcstress_neon_cleanup(struct test *test) {
    delete static_cast<std::vector<uint8_t> *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int llcstress_neon_run(struct test *test, int cpu) {
    auto *mem = static_cast<std::vector<uint8_t> *>(test->data);
    uint32_t *base = reinterpret_cast<uint32_t *>(mem->data());

    /* per-band count table: each band's slot holds THAT band's visit
     * count, so the golden must be per-band too — a single global
     * counter mismatches the moment any band is revisited (the first
     * draft's bug: mem=band-count vs local=total-count). */
    std::vector<uint32_t> counts(LLSN_BANDS * 4, 0);

    do {
        for (uint64_t batch = 0; batch < LLSN_SWEEP_EVERY; ++batch) {
            /* random band; our vector slot inside it — full-window
             * roaming, zero same-slot races across threads */
            size_t band = random64() % LLSN_BANDS;
            uint32_t *slot = base + (band * LLSN_STRIDE_V + (size_t)cpu) * 4;

            /* NEON 16B load-modify-store on the roamed slot */
            /* ALL slot accesses via asm: an intrinsic vst1q->vld1q pair
             * on the same address gets store-transformed + load-forwarded
             * by GCC (measured: the emitted store wrote the OLD value and
             * the verify folded to a register compare) — asm cannot be. */
            uint32x4_t v = ld_vec(slot);
            v = vaddq_u32(v, vdupq_n_u32(1));
            st_vec(slot, v);

            /* per-band golden (counts is private verification state —
             * intrinsic access is fine, it is not the path under test) */
            uint32_t *cnt = counts.data() + band * 4;
            uint32x4_t c = vaddq_u32(vld1q_u32(cnt), vdupq_n_u32(1));
            vst1q_u32(cnt, c);

            /* read-back: the slot must hold exactly this band's count */
            uint32x4_t mem_v = ld_vec(slot);
            uint32x4_t diff = veorq_u32(mem_v, c);
            if (vmaxvq_u32(diff) != 0) {
                report_fail_msg("llcstress_neon: slot read-back mismatch "
                                "(band %zu): mem {%u,%u,%u,%u} vs expected "
                                "{%u,%u,%u,%u} (lost/corrupted NEON store)",
                                band,
                                vgetq_lane_u32(mem_v, 0), vgetq_lane_u32(mem_v, 1),
                                vgetq_lane_u32(mem_v, 2), vgetq_lane_u32(mem_v, 3),
                                vgetq_lane_u32(c, 0), vgetq_lane_u32(c, 1),
                                vgetq_lane_u32(c, 2), vgetq_lane_u32(c, 3));
                return EXIT_FAILURE;
            }
        }

        /* L3 verification sweep (2026-11-18 rework): re-read ALL own
         * slots against the per-band counts shadow — delayed by a
         * full interval of random-band roaming, so everything except
         * the just-re-stored tail is L2-evicted and L3/DRAM-served. */
        for (size_t b = 0; b < LLSN_BANDS; ++b) {
            uint32_t *slot = base + (b * LLSN_STRIDE_V + (size_t)cpu) * 4;
            uint32x4_t mem_v = ld_vec(slot);
            uint32x4_t c = vld1q_u32(counts.data() + b * 4);
            uint32x4_t diff = veorq_u32(mem_v, c);
            if (vmaxvq_u32(diff) != 0) {
                report_fail_msg("llcstress_neon: delayed re-verification "
                                "mismatch (band %zu): mem {%u,%u,%u,%u} vs "
                                "expected {%u,%u,%u,%u} (L3-resident "
                                "corruption)", b,
                                vgetq_lane_u32(mem_v, 0), vgetq_lane_u32(mem_v, 1),
                                vgetq_lane_u32(mem_v, 2), vgetq_lane_u32(mem_v, 3),
                                vgetq_lane_u32(c, 0), vgetq_lane_u32(c, 1),
                                vgetq_lane_u32(c, 2), vgetq_lane_u32(c, 3));
                return EXIT_FAILURE;
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(llcstress_neon, "High-frequency random LLC roaming over 64MB with per-thread uint32x4 interleave slots: NEON vld1q/vaddq/vst1q RMW + immediate read-back verify PLUS delayed re-verification sweep against the per-band counts shadow (L3-path, 2026-11-18 rework) (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = llcstress_neon_init,
    .test_run = llcstress_neon_run,
    .test_cleanup = llcstress_neon_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
