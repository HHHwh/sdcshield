/*  ma_l2evict_2src_fp64 — L1D-evict 2src-ALU reload probe, FP64 data path.
 *  FP64 variant of ma_l2evict_2src (parent: tests/cpu/memory-access/
 *  ma_l2evict_2src.cpp — UNCOMMITTED co-worker WIP on disk at adaptation
 *  time; the frozen disk state is the reference, a fresh clone may not
 *  carry it yet): same hot-loop skeleton, same loads/stores, same
 *  dc civac + dsb sy eviction insertion, same cold-branch memcmp — ONLY
 *  the rotating vALU consumer is re-typed from u64 integer ops
 *  (add/eor/and/orr) to FP64 arithmetic (add/sub/mul/swapped-sub), so the
 *  consumer runs on the FP pipe / FP register file instead of the integer
 *  vector pipe.
 *
 *  Hot loop (per element, 128-bit NEON, instruction shapes preserved):
 *      ldr q(srcA[i]); ldr q(srcB[i])          — 2 source loads (asm)
 *      vaddq/vsubq/vmulq/vsubq(rev) f64 by i%4 — rotating FP64 vALU
 *      vst1q(temp[i])                          — str 1
 *      dc civac + dsb sy on temp[i]            — the eviction insertion
 *      ldr q(temp[i])                          — RELOAD (asm): L2/L3
 *      vst1q(dst[i])                           — str 2, back-to-back
 *
 *  Slot-4 operator note: the FP64 rotation deliberately uses
 *  fsub(b,a) (swapped-operand sub) instead of the research-repo
 *  rowmajor_fp64 precedent's fmax — std::fmax has implementation freedom
 *  (instruction vs glibc branch) that can break bit-exactness of a
 *  SCALAR golden on double-NaN payloads, while +,-,* compile to
 *  FADD/FSUB/FMUL which share the exact FPAdd/FPSub/FPMul pseudocode
 *  with the vector forms (bit-identical by architecture).  The mixed
 *  4-way switch also blocks auto-vectorization of the golden loop
 *  (issue #373: zero vector ALU in init).
 *
 *  Golden is init-time SCALAR double recomputation of the same rotating
 *  arithmetic (issue #373 discipline preserved from the parent).
 *  NaN/Inf propagation on random bit patterns is deterministic
 *  (single IEEE754 ops, same FPCR) and bit-compared via memcmp, as in
 *  the parent.  Pre-registered expectation inherited: 0 fail.
 */
#include <sandstone.h>
#include <arm_neon.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)

#define MA_L2EVICT_2SRC_VECS (16 * 1024 / 16)   /* 1024 vec x 16B = 16 KiB window */

namespace {

struct MaL2Evict2SrcFp64Data {
    uint64x2_t srcA[MA_L2EVICT_2SRC_VECS];
    uint64x2_t srcB[MA_L2EVICT_2SRC_VECS];
    uint64x2_t expected[MA_L2EVICT_2SRC_VECS];  /* scalar double golden */
};

static inline uint64x2_t ma_2src_load(const uint64x2_t *addr) {
    uint64x2_t res;
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

static int ma_l2evict_2src_fp64_init(struct test *test) {
    auto *d = new MaL2Evict2SrcFp64Data;
    if (!d)
        return EXIT_FAILURE;
    memset_random(d->srcA, sizeof(d->srcA));
    memset_random(d->srcB, sizeof(d->srcB));
    /* scalar double golden — the same rotating arithmetic on scalar FP
     * lanes, zero vector ALU in init (issue #373).  The pure C +,-,*
     * operators compile to FADD/FSUB/FMUL D which share the vector ops'
     * pseudocode, so the golden is bit-identical to the NEON consumer. */
    const double *a = reinterpret_cast<const double *>(d->srcA);
    const double *b = reinterpret_cast<const double *>(d->srcB);
    double *e = reinterpret_cast<double *>(d->expected);
    for (uint32_t i = 0; i < MA_L2EVICT_2SRC_VECS * 2; ++i) {
        switch ((i / 2) % 4) {
            case 0:  e[i] = a[i] + b[i]; break;
            case 1:  e[i] = a[i] - b[i]; break;
            case 2:  e[i] = a[i] * b[i]; break;
            default: e[i] = b[i] - a[i]; break;
        }
    }
    test->data = d;
    return EXIT_SUCCESS;
}

static int ma_l2evict_2src_fp64_cleanup(struct test *test) {
    delete static_cast<MaL2Evict2SrcFp64Data *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int ma_l2evict_2src_fp64_run(struct test *test, int cpu) {
    auto *d = static_cast<MaL2Evict2SrcFp64Data *>(test->data);
    (void)cpu;

    uint64x2_t *temp = static_cast<uint64x2_t *>(
            aligned_alloc(64, MA_L2EVICT_2SRC_VECS * sizeof(uint64x2_t)));
    uint64x2_t *dst = static_cast<uint64x2_t *>(
            aligned_alloc(64, MA_L2EVICT_2SRC_VECS * sizeof(uint64x2_t)));
    if (!temp || !dst) {
        free(temp);
        free(dst);
        return EXIT_FAILURE;
    }

    uint64_t round = 0;
    do {
        ++round;
        for (uint32_t i = 0; i < MA_L2EVICT_2SRC_VECS; ++i) {
            uint64x2_t a = ma_2src_load(&d->srcA[i]);
            uint64x2_t b = ma_2src_load(&d->srcB[i]);
            const float64x2_t fa = vreinterpretq_f64_u64(a);
            const float64x2_t fb = vreinterpretq_f64_u64(b);
            float64x2_t fres;

            switch (i % 4) {
                case 0:  fres = vaddq_f64(fa, fb); break;
                case 1:  fres = vsubq_f64(fa, fb); break;
                case 2:  fres = vmulq_f64(fa, fb); break;
                default: fres = vsubq_f64(fb, fa); break;  /* swapped-operand sub */
            }
            uint64x2_t res = vreinterpretq_u64_f64(fres);

            vst1q_u64(reinterpret_cast<uint64_t *>(&temp[i]), res); /* str 1 */

            /* mrn_rmw_l1d's insertion point: evict the just-stored line
             * so the reload below is L2/L3-served */
            evict_l1d_line(&temp[i]);

            uint64x2_t r = ma_2src_load(&temp[i]); /* RELOAD (asm) */

            vst1q_u64(reinterpret_cast<uint64_t *>(&dst[i]), r);   /* str 2, back-to-back */
        }

        /* cold branch: full-window verify against the scalar double golden */
        if (memcmp(dst, d->expected, MA_L2EVICT_2SRC_VECS * sizeof(uint64x2_t)) != 0) {
            const uint64_t *dd = reinterpret_cast<const uint64_t *>(dst);
            const uint64_t *ee = reinterpret_cast<const uint64_t *>(d->expected);
            for (uint32_t i = 0; i < MA_L2EVICT_2SRC_VECS * 2; ++i) {
                if (dd[i] != ee[i]) {
                    log_error("ma_l2evict_2src_fp64: miscompare vec %u lane %u "
                              "(round=%lu): golden=0x%016llx actual=0x%016llx "
                              "xor=0x%016llx (FP64 reload after civac evict)",
                              i / 2, i % 2, (unsigned long)round,
                              (unsigned long long)ee[i],
                              (unsigned long long)dd[i],
                              (unsigned long long)(ee[i] ^ dd[i]));
                    break;   /* ≤8-line dump discipline */
                }
            }
            report_fail_msg("ma_l2evict_2src_fp64: FP64 2src reload after civac "
                            "evict miscompares");
        }
    } while (test_time_condition(test));

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ma_l2evict_2src_fp64, "FP64 variant of ma_l2evict_2src: 2src loads + rotating FP64 vALU (add/sub/mul/rev-sub) + str, dc civac+dsb evicts the stored line before the asm reload (L2/L3-served), reload feeds the back-to-back second store (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = ma_l2evict_2src_fp64_init,
    .test_run = ma_l2evict_2src_fp64_run,
    .test_cleanup = ma_l2evict_2src_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture (family convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif /* __aarch64__ */
