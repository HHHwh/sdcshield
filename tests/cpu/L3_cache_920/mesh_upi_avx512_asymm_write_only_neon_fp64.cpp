/*  mesh_upi_avx512_asymm_write_only_neon_fp64 — FP64 datapath variant
 *  of tests/cpu/L3_cache_920/mesh_upi_avx512_asymm_write_only_int.cpp
 *  (2026-11-18 NEON/FP64 expansion completion, user directive).  The
 *  x86 AVX-512 name is inherited lineage; the datapath is f64x2 NEON.
 *
 *  The 2026-11-17 capability-rework geometry, born here from the
 *  start: 4 MB SHARED double array with the v2 double-barrier
 *  protocol (B1 write->read, B2 read->write), STATIC writer
 *  partitions (every block written exactly once per round), and
 *  cumulative differential k-space sums (global_ksum monotonic;
 *  reader takes expected = cum(K) - cum(K-1) after B1).  k = (int64)
 *  (v*2) over exact 0.5-multiples makes every accumulator
 *  exact-integer math (order-independent, bit-exact).
 */
#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <unistd.h>
#include <arm_neon.h>

#define ARRAY_DOUBLES (1 << 19)          /* 512K doubles = 4MB shared */
#define BLOCK_DOUBLES 16
#define VEC_LANES 2
#define VECS_PER_BLOCK (BLOCK_DOUBLES / VEC_LANES)
#define NUM_BLOCKS (ARRAY_DOUBLES / BLOCK_DOUBLES)

/* anti-forwarding f64x2 load (asm discipline: intrinsic store->load
 * pairs can be register-folded by GCC — asm cannot) */
static inline float64x2_t ma_ldr_vec_f64(const double *addr) {
    float64x2_t v;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(v) : "r"(addr) : "memory");
    return v;
}

namespace { /* ODR: per-TU TestData */
struct TestData {
    alignas(16) double *data;            /* shared, 16B aligned */
    std::atomic<uint64_t> global_ksum;   /* monotonic k-space accumulator */
    std::atomic<uint32_t> thread_idx;
    std::atomic<uint32_t> b1_count, b1_sense;   /* B1: write->read */
    std::atomic<uint32_t> b2_count, b2_sense;   /* B2: read->write */
};
}

/* exact 0.5-multiple in [-2^19, 2^19); k = v*2 is an exact integer */
static inline double mawol_dist(void) {
    return (double)((int64_t)(random64() % (1ULL << 21)) - (1 << 20)) * 0.5;
}

static int mesh_upi_avx512_asymm_write_only_neon_fp64_init(struct test *test) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) {
        return -255;   /* SKIP */
    }

    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;

    td->data = (double *)aligned_alloc(16, ARRAY_DOUBLES * sizeof(double));
    if (!td->data) {
        delete td;
        return EXIT_FAILURE;
    }
    memset(td->data, 0, ARRAY_DOUBLES * sizeof(double));

    td->global_ksum.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    td->b1_count.store(0, std::memory_order_relaxed);
    td->b1_sense.store(0, std::memory_order_relaxed);
    td->b2_count.store(0, std::memory_order_relaxed);
    td->b2_sense.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx512_asymm_write_only_neon_fp64_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData *>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    bool is_reader = (id == 0);

    uint32_t total = thread_count();
    if (total < 2) {
        log_skip(CpuTopologyIssueSkipCategory,
                 "mesh_upi_avx512_asymm_write_only_neon_fp64 requires at "
                 "least 2 threads (inter-core test); skipping on this "
                 "thread count");
        return EXIT_SKIP;
    }

    /* sense-reversal barrier lambda (throttled for the 4MB fill) */
    auto barrier = [&](std::atomic<uint32_t> &cnt, std::atomic<uint32_t> &sen) -> bool {
        uint32_t s = sen.load(std::memory_order_seq_cst);
        if (cnt.fetch_add(1, std::memory_order_seq_cst) + 1 == total) {
            cnt.store(0, std::memory_order_seq_cst);
            sen.store(1 - s, std::memory_order_seq_cst);
            return true;
        }
        uint32_t spin = 0;
        while (sen.load(std::memory_order_seq_cst) == s) {
            if ((++spin & 131071u) == 0) {
                if (!test_time_condition(test)) return false;
            } else {
                __asm__ volatile("yield");
            }
        }
        return true;
    };

    /* static writer partition: writer wid owns [b_lo, b_hi) */
    uint32_t W = total - 1;
    uint32_t per = NUM_BLOCKS / W;
    uint32_t b_lo = 0, b_hi = 0;
    if (!is_reader) {
        uint32_t wid = (uint32_t)(id - 1);
        b_lo = wid * per;
        b_hi = (wid == W - 1) ? NUM_BLOCKS : b_lo + per;
    }

    uint64_t prev_cum = 0;

    do {
        if (!is_reader) {
            for (uint32_t block = b_lo; block < b_hi; ++block) {
                size_t offset = block * BLOCK_DOUBLES;
                double vals[BLOCK_DOUBLES];
                uint64_t ksum = 0;
                for (int j = 0; j < BLOCK_DOUBLES; ++j) {
                    vals[j] = mawol_dist();
                    ksum += (uint64_t)(int64_t)(vals[j] * 2.0);
                }
                for (int v = 0; v < VECS_PER_BLOCK; ++v)
                    vst1q_f64(td->data + offset + v * VEC_LANES,
                              vld1q_f64(vals + v * VEC_LANES));
                __sync_synchronize();

                /* immediate read-back via asm (per-byte write check) */
                bool block_ok = true;
                for (int v = 0; v < VECS_PER_BLOCK; ++v) {
                    float64x2_t written = vld1q_f64(vals + v * VEC_LANES);
                    float64x2_t loaded = ma_ldr_vec_f64(td->data + offset + v * VEC_LANES);
                    uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(written),
                                                 vreinterpretq_u64_f64(loaded));
                    if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                        block_ok = false;
                        break;
                    }
                }
                if (!block_ok) {
                    report_fail_msg("mesh_upi_avx512_asymm_write_only_neon_"
                                    "fp64: writer immediate read-back mismatch "
                                    "(block %u, store/load corruption)",
                                    block);
                    return EXIT_FAILURE;
                }

                td->global_ksum.fetch_add(ksum, std::memory_order_seq_cst);
            }
        }

        /* B1: all blocks written; array frozen until B2 */
        if (!barrier(td->b1_count, td->b1_sense)) break;

        if (is_reader) {
            uint64_t cum = td->global_ksum.load(std::memory_order_seq_cst);
            uint64_t expected_ksum = cum - prev_cum;
            prev_cum = cum;

            uint64_t read_ksum = 0;
            bool consistent = true;
            alignas(16) double store_buf[BLOCK_DOUBLES];

            for (size_t i = 0; i < ARRAY_DOUBLES; i += BLOCK_DOUBLES) {
                float64x2_t vecs[VECS_PER_BLOCK];
                for (int v = 0; v < VECS_PER_BLOCK; ++v) {
                    vecs[v] = vld1q_f64(td->data + i + v * VEC_LANES);
                    read_ksum += (uint64_t)(int64_t)(vgetq_lane_f64(vecs[v], 0) * 2.0);
                    read_ksum += (uint64_t)(int64_t)(vgetq_lane_f64(vecs[v], 1) * 2.0);
                }

                for (int v = 0; v < VECS_PER_BLOCK; ++v)
                    vst1q_f64(store_buf + v * VEC_LANES, vecs[v]);
                for (int v = 0; v < VECS_PER_BLOCK; ++v) {
                    float64x2_t orig = vld1q_f64(td->data + i + v * VEC_LANES);
                    float64x2_t reload = vld1q_f64(store_buf + v * VEC_LANES);
                    uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(orig),
                                                 vreinterpretq_u64_f64(reload));
                    if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
                        consistent = false;
                        break;
                    }
                }
                if (!consistent) break;
            }

            if (read_ksum != expected_ksum || !consistent) {
                report_fail_msg("mesh_upi_avx512_asymm_write_only_neon_"
                                "fp64: reader k-sum mismatch or block "
                                "inconsistency (L3 write path): expected "
                                "ksum %lu got %lu consistent=%d",
                                (unsigned long)expected_ksum,
                                (unsigned long)read_ksum, (int)consistent);
                return EXIT_FAILURE;
            }
        }

        /* B2: reader done; next round's writes may start */
        if (!barrier(td->b2_count, td->b2_sense)) break;

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int mesh_upi_avx512_asymm_write_only_neon_fp64_finish(struct test *test) {
    auto *td = static_cast<TestData *>(test->data);
    free(td->data);
    delete td;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

DECLARE_TEST(mesh_upi_avx512_asymm_write_only_neon_fp64, "FP64 variant of mesh_upi_avx512_asymm_write_only_int: static-partition writers fill a 4MB shared double array via f64x2 stores under double sense-reversal barriers; the reader differential-verifies with exact k-space (0.5-multiple) sums (L3 write-path SDC) (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx512_asymm_write_only_neon_fp64_init,
    .test_run = mesh_upi_avx512_asymm_write_only_neon_fp64_run,
    .test_cleanup = mesh_upi_avx512_asymm_write_only_neon_fp64_finish,
    .fracture_loop_count = -1,   /* never fracture: 4MB-fill rounds */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
