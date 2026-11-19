/*  fp64_2ldr_4ext_stp — user-specified excitation recipe (2026-11-12):
 *  2 ldr + 4 ext + 1 stp, FP64 data.
 *
 *  Hot loop (per element, 16B NEON, asm-guaranteed IO):
 *      ldr q(srcA[i]); ldr q(srcB[i])        — 2 FP64 source loads
 *      vextq_f64 x4 (graft chain)            — 4 EXT lane permutations
 *      stp q,q -> dst pair (32B)             — 1 paired store of two
 *                                              chain results
 *
 *  EXT graft chain (vextq_f64(x, y, 1) = {x.hi, y.lo}, lanes lo=0 hi=1):
 *      e1 = ext(a, b) = {a1, b0}
 *      e2 = ext(e1, a) = {b0, a0}
 *      e3 = ext(e2, b) = {a0, b0}
 *      e4 = ext(e3, e1) = {b0, a1}
 *      stp(dst[2i], dst[2i+1]) = {e4, e2}
 *
 *  Golden is init-time SCALAR lane-index recomputation (issue #373: zero
 *  vector instructions in init — the EXT chain is pure lane muxing, so
 *  the scalar golden is u64 lane selection and bit-exact).  Random
 *  bit-pattern sources reinterpreted as doubles may be NaN/Inf — EXT is
 *  a bit-level permutation, NaN payloads just move, bit-compare via
 *  memcmp.  rowmajor's per-sweep direction alternation and the 2^8
 *  sweep-per-round cadence are kept (family skeleton).
 *
 *  Window: srcA/srcB 16 KiB each + dst 32 KiB (stp doubles the store
 *  footprint) = 64 KiB — the L1D capacity edge, the family's
 *  L1D-domain trigger recipe territory.
 */
#include <sandstone.h>
#include <arm_neon.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)

#define FP64_2LDR_4EXT_STP_COUNT (16 * 1024 / 16)   /* 1024 vec x 16B srcs */

namespace {

struct Fp64TwoLdrFourExtStpData {
    uint64x2_t srcA[FP64_2LDR_4EXT_STP_COUNT];
    uint64x2_t srcB[FP64_2LDR_4EXT_STP_COUNT];
    uint64x2_t expected[FP64_2LDR_4EXT_STP_COUNT * 2];  /* stp pairs */
};

static inline uint64x2_t ld_vec(const uint64x2_t *addr) {
    uint64x2_t res;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

/* the single paired store: 1 stp q,q writing both chain results */
static inline void st_pair(uint64x2_t *addr, uint64x2_t v0, uint64x2_t v1) {
    __asm__ volatile("stp %q0, %q1, [%2]"
                     :: "w"(v0), "w"(v1), "r"(addr) : "memory");
}

} /* namespace */

static int fp64_2ldr_4ext_stp_init(struct test *test) {
    auto *d = new Fp64TwoLdrFourExtStpData;
    if (!d)
        return EXIT_FAILURE;
    memset_random(d->srcA, sizeof(d->srcA));
    memset_random(d->srcB, sizeof(d->srcB));

    /* scalar golden — pure lane-index selection, zero vector ops in init
     * (issue #373).  vextq_f64(x, y, 1) = {x.hi, y.lo}:
     *   e1 = {a1, b0}; e2 = {b0, a0}; e3 = {a0, b0}; e4 = {b0, a1}
     * stp writes {e4, e2} to the dst pair {2i, 2i+1}. */
    for (int i = 0; i < FP64_2LDR_4EXT_STP_COUNT; ++i) {
        const uint64_t a0 = vgetq_lane_u64(d->srcA[i], 0);
        const uint64_t a1 = vgetq_lane_u64(d->srcA[i], 1);
        const uint64_t b0 = vgetq_lane_u64(d->srcB[i], 0);
        d->expected[2 * i]     = (uint64x2_t){ b0, a1 };   /* e4 */
        d->expected[2 * i + 1] = (uint64x2_t){ b0, a0 };   /* e2 */
    }
    test->data = d;
    return EXIT_SUCCESS;
}

static int fp64_2ldr_4ext_stp_cleanup(struct test *test) {
    delete static_cast<Fp64TwoLdrFourExtStpData *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int fp64_2ldr_4ext_stp_run(struct test *test, int cpu) {
    auto *d = static_cast<Fp64TwoLdrFourExtStpData *>(test->data);
    (void)cpu;

    uint64x2_t *dst = static_cast<uint64x2_t *>(aligned_alloc(
            64, FP64_2LDR_4EXT_STP_COUNT * 2 * sizeof(uint64x2_t)));
    if (!dst)
        return EXIT_FAILURE;

    uint64_t sweep = 0;
    TEST_LOOP(test, 1 << 8) {
        const bool ascending = (sweep & 1) == 0;

        for (int n = 0; n < FP64_2LDR_4EXT_STP_COUNT; n++) {
            const int i = ascending
                ? n : (FP64_2LDR_4EXT_STP_COUNT - 1 - n);

            /* 2 ldr — dual FP64 source loads (asm) */
            const float64x2_t fa =
                vreinterpretq_f64_u64(ld_vec(&d->srcA[i]));
            const float64x2_t fb =
                vreinterpretq_f64_u64(ld_vec(&d->srcB[i]));

            /* 4 ext — the lane-graft chain */
            const float64x2_t e1 = vextq_f64(fa, fb, 1);
            const float64x2_t e2 = vextq_f64(e1, fa, 1);
            const float64x2_t e3 = vextq_f64(e2, fb, 1);
            const float64x2_t e4 = vextq_f64(e3, e1, 1);

            /* 1 stp — paired 32B store of both chain results */
            st_pair(&dst[2 * i],
                    vreinterpretq_u64_f64(e4),
                    vreinterpretq_u64_f64(e2));
        }
        sweep++;

        /* cold branch: full-window verify against the scalar golden */
        if (memcmp(dst, d->expected,
                   FP64_2LDR_4EXT_STP_COUNT * 2 * sizeof(uint64x2_t)) != 0) {
            const uint64_t *dd = reinterpret_cast<const uint64_t *>(dst);
            const uint64_t *ee = reinterpret_cast<const uint64_t *>(d->expected);
            for (uint32_t j = 0; j < FP64_2LDR_4EXT_STP_COUNT * 4; ++j) {
                if (dd[j] != ee[j]) {
                    log_error("fp64_2ldr_4ext_stp: miscompare vec %u lane %u "
                              "(sweep=%lu): golden=0x%016llx actual=0x%016llx "
                              "xor=0x%016llx (2ldr+4ext+stp chain)",
                              j / 4, j % 4, (unsigned long)sweep,
                              (unsigned long long)ee[j],
                              (unsigned long long)dd[j],
                              (unsigned long long)(ee[j] ^ dd[j]));
                    break;   /* <=8-line dump discipline */
                }
            }
            report_fail_msg("fp64_2ldr_4ext_stp: 2ldr+4ext+1stp chain "
                            "miscompares");
        }
    }

    free(dst);
    return EXIT_SUCCESS;
}

DECLARE_TEST(fp64_2ldr_4ext_stp, "User-recipe FP64 excitation probe: 2 asm ldr q (dual FP64 sources) + 4 vextq_f64 lane-graft chain + 1 stp q,q paired store of two chain results; bit-exact scalar lane-index golden, rowmajor sweep skeleton (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fp64_2ldr_4ext_stp_init,
    .test_run = fp64_2ldr_4ext_stp_run,
    .test_cleanup = fp64_2ldr_4ext_stp_cleanup,
    .fracture_loop_count = -1,   /* never fracture (family convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif /* __aarch64__ */
