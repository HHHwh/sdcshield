/*  ma_l2evict_rev_fp64 — L1D-evict reload-path probe, FP64 consumer lane.
 *  FP64 variant of ma_l2evict_rev (parent: tests/cpu/memory-access/
 *  ma_l2evict_rev.cpp — UNCOMMITTED co-worker WIP on disk at adaptation
 *  time; the frozen disk state is the reference, a fresh clone may not
 *  carry it yet): same asm-load round-trip with `dc civac + dsb sy`
 *  evicting the input line before the reload, same cold-branch compare
 *  against the independent init-time input snapshot — ONLY the consumer
 *  between the first load and the far store is re-typed: the NEON
 *  vrev32 bswap permutation becomes a single vaddq_f64(v, v) FP64 op,
 *  so the consumer runs on the FP pipe / FP register file instead of the
 *  NEON permute path.
 *
 *  The consumer's output (swapped[]) is liveness pressure only — the
 *  detection compares the RELOADED input against the golden snapshot,
 *  which is a memcpy of the untouched input (issue #373/374 preserved:
 *  a self-inverse transform must not verify against itself — and the
 *  FP64 consumer is NOT self-inverse, so the independent snapshot is
 *  mandatory).  No FP golden computation is involved: golden stays the
 *  raw input bits, NaN/Inf payload of the consumer output is irrelevant
 *  to detection.
 *
 *  Purpose inherited from the parent cell: evict-path (reload served
 *  from L2/L3) crossed with the 128-bit NEON lane structure, now with
 *  the FP64 data path as the consumer.  Pre-registered expectation:
 *  0 fail.  Known confound kept deliberately: civac+dsb both evicts AND
 *  stretches str→ldr timing.
 */
#include <sandstone.h>
#include <arm_neon.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)

#define MA_L2EVICT_REV_WORDS (16 * 1024 / 4)   /* 4096 words = 16 KiB window */

namespace {

struct MaL2EvictRevFp64Data {
    uint32_t input[MA_L2EVICT_REV_WORDS];     /* random source          */
    uint32_t swapped[MA_L2EVICT_REV_WORDS];   /* FP64-consumer store target */
    uint32_t golden[MA_L2EVICT_REV_WORDS];    /* init input snapshot    */
};

static inline uint32x4_t ma_rev_load(const uint32_t *addr) {
    uint32x4_t res;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

static inline void evict_l1d_line(const void *addr) {
    __asm__ volatile(
        "dc civac, %0\n\t"
        "dsb sy\n\t"
        :: "r"(addr) : "memory"
    );
}

} /* namespace */

static int ma_l2evict_rev_fp64_init(struct test *test) {
    auto *d = new MaL2EvictRevFp64Data;
    if (!d)
        return EXIT_FAILURE;
    memset_random(d->input, sizeof(d->input));
    /* independent golden snapshot of the raw input (issue #373/374) */
    memcpy(d->golden, d->input, sizeof(d->input));
    memset(d->swapped, 0, sizeof(d->swapped));
    test->data = d;
    return EXIT_SUCCESS;
}

static int ma_l2evict_rev_fp64_cleanup(struct test *test) {
    delete static_cast<MaL2EvictRevFp64Data *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int ma_l2evict_rev_fp64_run(struct test *test, int cpu) {
    auto *d = static_cast<MaL2EvictRevFp64Data *>(test->data);
    uint64_t round = 0;

    do {
        ++round;
        for (uint32_t w = 0; w < MA_L2EVICT_REV_WORDS; w += 4) {
            uint32x4_t v = ma_rev_load(&d->input[w]);    /* 1st read (asm)  */
            /* FP64 consumer: one vaddq_f64(v, v) — the bswap permutation's
             * slot taken by the FP add pipe.  Output is liveness-only
             * (swapped is never compared); bit patterns may go NaN/Inf. */
            float64x2_t fv = vreinterpretq_f64_u32(v);
            float64x2_t fs = vaddq_f64(fv, fv);
            vst1q_u32(&d->swapped[w], vreinterpretq_u32_f64(fs)); /* store, other line */

            /* evict input's line so the reload below is L2/L3-served */
            evict_l1d_line(&d->input[w]);

            uint32x4_t r = ma_rev_load(&d->input[w]);    /* RELOAD (asm)    */

            /* cold branch: reload vs the independent golden snapshot */
            if (vmaxvq_u32(veorq_u32(r, vld1q_u32(&d->golden[w]))) != 0) {
                uint32_t lr[4];
                vst1q_u32(lr, r);
                for (int l = 0; l < 4; ++l) {
                    if (lr[l] != d->golden[w + l]) {
                        log_error("ma_l2evict_rev_fp64: reload mismatch word %u "
                                  "(cpu=%d round=%lu): golden=0x%08x "
                                  "actual=0x%08x xor=0x%08x "
                                  "(L2/L3-served NEON reload, FP64 consumer)",
                                  w + (uint32_t)l, cpu, (unsigned long)round,
                                  d->golden[w + l], lr[l],
                                  d->golden[w + l] ^ lr[l]);
                    }
                }
                report_fail_msg("ma_l2evict_rev_fp64: NEON reload after civac "
                                "evict returned wrong data (FP64 consumer)");
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

DECLARE_TEST(ma_l2evict_rev_fp64, "FP64 variant of ma_l2evict_rev: vaddq_f64 consumer round-trip, dc civac+dsb evicts the input line before the asm reload (L2/L3-served) — L1D-domain bypass with 128-bit lane structure and FP64 data path (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = ma_l2evict_rev_fp64_init,
    .test_run = ma_l2evict_rev_fp64_run,
    .test_cleanup = ma_l2evict_rev_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture (family convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif /* __aarch64__ */
