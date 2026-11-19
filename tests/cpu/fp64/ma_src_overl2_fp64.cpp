/*  ma_src_overl2_fp64 — 3-ldr+FP64-ALU+2-str trigger recipe, sources beyond L2.
 *
 *  FP64 variant of ma_src_overl2 (parent: tests/cpu/memory-access/
 *  ma_src_overl2.cpp — UNCOMMITTED co-worker WIP on disk at adaptation
 *  time; the frozen disk state is the reference, a fresh clone may not
 *  carry it yet): the neon_rot_ldr_at_top_rowmajor skeleton (dead
 *  scratch ldr at top + 2 source loads + i%4 rotating vALU + back-to-back
 *  double store + cold-branch memcmp) is kept instruction-shaped, the
 *  footprint experiment (4 MiB per source, 8 MiB live, 16 KiB
 *  L1D-resident windows) is kept identical — ONLY the rotating vALU
 *  consumer is re-typed from u64 integer ops (add/eor/and/orr) to FP64
 *  arithmetic (add/sub/mul/swapped-sub), moving the consumer to the FP
 *  pipe / FP register file.
 *
 *  Slot-4 operator note (same as ma_l2evict_2src_fp64): fsub(b,a)
 *  instead of the rowmajor_fp64 precedent's fmax — std::fmax has
 *  implementation freedom that can break bit-exactness of a scalar
 *  golden on double-NaN payloads; +,-,* compile to FADD/FSUB/FMUL which
 *  share the vector forms' pseudocode (bit-identical by architecture).
 *  The mixed 4-way switch blocks auto-vectorization of the golden loop.
 *
 *  Golden is init-time SCALAR double recomputation over the full 4 MiB
 *  (issue #373 discipline preserved from the parent: zero vector ALU in
 *  init, independent recomputation path).  NaN/Inf propagation on random
 *  bit patterns is deterministic and bit-compared.  fracture disabled:
 *  cache fracturing would perturb the footprint experiment.
 */
#include <sandstone.h>
#include <arm_neon.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#if defined(__aarch64__)

#define MA_SRC_OVERL2_SRC_VECS  (262144)   /* x 16B = 4 MiB per source  */
#define MA_SRC_OVERL2_WIN_VECS  (1024)     /* x 16B = 16 KiB window     */
#define MA_SRC_OVERL2_BLOCKS    (MA_SRC_OVERL2_SRC_VECS / MA_SRC_OVERL2_WIN_VECS)

namespace {

struct MaSrcOverL2Fp64Data {
    uint64x2_t srcA[MA_SRC_OVERL2_SRC_VECS];     /* 4 MiB, L3/DRAM-served */
    uint64x2_t srcB[MA_SRC_OVERL2_SRC_VECS];     /* 4 MiB, L3/DRAM-served */
    uint64x2_t expected[MA_SRC_OVERL2_SRC_VECS]; /* 4 MiB scalar double golden */
    uint64x2_t scratch[MA_SRC_OVERL2_WIN_VECS];  /* 16 KiB, L1D-resident  */
};

static inline uint64x2_t ma_overl2_load(const uint64x2_t *addr) {
    uint64x2_t res;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}

} /* namespace */

static int ma_src_overl2_fp64_init(struct test *test) {
    auto *d = new MaSrcOverL2Fp64Data;
    if (!d)
        return EXIT_FAILURE;
    memset_random(d->srcA, sizeof(d->srcA));
    memset_random(d->srcB, sizeof(d->srcB));
    memset_random(d->scratch, sizeof(d->scratch));
    /* scalar double golden over the full 4 MiB — same rotating arithmetic
     * on scalar FP lanes, zero vector ALU in init (issue #373).  Rotation
     * uses the WINDOW index parity (g % 4), matching the hot loop's i % 4. */
    const double *a = reinterpret_cast<const double *>(d->srcA);
    const double *b = reinterpret_cast<const double *>(d->srcB);
    double *e = reinterpret_cast<double *>(d->expected);
    for (uint32_t g = 0; g < MA_SRC_OVERL2_SRC_VECS * 2; ++g) {
        switch ((g / 2) % 4) {
            case 0:  e[g] = a[g] + b[g]; break;
            case 1:  e[g] = a[g] - b[g]; break;
            case 2:  e[g] = a[g] * b[g]; break;
            default: e[g] = b[g] - a[g]; break;
        }
    }
    test->data = d;
    return EXIT_SUCCESS;
}

static int ma_src_overl2_fp64_cleanup(struct test *test) {
    delete static_cast<MaSrcOverL2Fp64Data *>(test->data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

static int ma_src_overl2_fp64_run(struct test *test, int cpu) {
    auto *d = static_cast<MaSrcOverL2Fp64Data *>(test->data);
    (void)cpu;

    /* per-thread L1D-resident window (rowmajor idiom) */
    uint64x2_t *temp = static_cast<uint64x2_t *>(
            aligned_alloc(64, MA_SRC_OVERL2_WIN_VECS * sizeof(uint64x2_t)));
    uint64x2_t *dst = static_cast<uint64x2_t *>(
            aligned_alloc(64, MA_SRC_OVERL2_WIN_VECS * sizeof(uint64x2_t)));
    if (!temp || !dst) {
        free(temp);
        free(dst);
        return EXIT_FAILURE;
    }

    uint64_t round = 0;
    do {
        /* rowmajor's per-sweep direction alternation */
        const bool ascending = (round & 1) == 0;
        ++round;

        for (uint32_t blk = 0; blk < MA_SRC_OVERL2_BLOCKS; ++blk) {
            const uint64x2_t *sa = &d->srcA[blk * MA_SRC_OVERL2_WIN_VECS];
            const uint64x2_t *sb = &d->srcB[blk * MA_SRC_OVERL2_WIN_VECS];

            /* hot window — instruction-shaped like rowmajor's sweep:
             * dead scratch ldr + 2 source ldr + rotating FP64 vALU +
             * double str; only the source base walks across the 8 MiB range */
            for (uint32_t n = 0; n < MA_SRC_OVERL2_WIN_VECS; ++n) {
                const uint32_t i = ascending ? n : (MA_SRC_OVERL2_WIN_VECS - 1 - n);

                uint64x2_t unused_x = ma_overl2_load(&d->scratch[i]);
                (void)unused_x;

                uint64x2_t a = ma_overl2_load(&sa[i]);
                uint64x2_t b = ma_overl2_load(&sb[i]);
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

                vst1q_u64(reinterpret_cast<uint64_t *>(&temp[i]), res);
                vst1q_u64(reinterpret_cast<uint64_t *>(&dst[i]), res);
            }

            /* cold branch per block: verify the window against the scalar
             * double golden for this block */
            if (memcmp(dst, &d->expected[blk * MA_SRC_OVERL2_WIN_VECS],
                       MA_SRC_OVERL2_WIN_VECS * sizeof(uint64x2_t)) != 0) {
                const uint64_t *dd = reinterpret_cast<const uint64_t *>(dst);
                const uint64_t *ee = reinterpret_cast<const uint64_t *>(
                        &d->expected[blk * MA_SRC_OVERL2_WIN_VECS]);
                for (uint32_t i = 0; i < MA_SRC_OVERL2_WIN_VECS * 2; ++i) {
                    if (dd[i] != ee[i]) {
                        log_error("ma_src_overl2_fp64: miscompare block %u vec %u "
                                  "lane %u (round=%lu): golden=0x%016llx "
                                  "actual=0x%016llx xor=0x%016llx "
                                  "(FP64 sources beyond L2)",
                                  blk, i / 2, i % 2, (unsigned long)round,
                                  (unsigned long long)ee[i],
                                  (unsigned long long)dd[i],
                                  (unsigned long long)(ee[i] ^ dd[i]));
                        break;   /* ≤8-line dump discipline */
                    }
                }
                report_fail_msg("ma_src_overl2_fp64: FP64 2src recipe miscompares "
                                "with L3/DRAM-served sources");
            }
        }
    } while (test_time_condition(test));

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ma_src_overl2_fp64, "FP64 variant of ma_src_overl2: 3-ldr+rotating-FP64-vALU+2-str rowmajor recipe with 4 MiB-per-source arrays (8 MiB live sources, 8x per-core L2): source loads L3/DRAM-served while scratch/temp/dst stay L1D-resident (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = ma_src_overl2_fp64_init,
    .test_run = ma_src_overl2_fp64_run,
    .test_cleanup = ma_src_overl2_fp64_cleanup,
    .fracture_loop_count = -1,   /* never fracture — cache placement is the variable */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif /* __aarch64__ */
