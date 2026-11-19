/*  mesh_upi_sse_write_only_neon_fp64 — FP64 datapath variant of
 *  tests/cpu/L3_cache_920/mesh_upi_sse_write_only_int.cpp (2026-11-18
 *  NEON/FP64 expansion completion, user directive).  The x86 SSE
 *  name is inherited lineage; the datapath is f64x2 NEON.
 *
 *  The 2026-11-17 capability-rework geometry, born here from the
 *  start: 4 MB SHARED double array (∈ (L2 512KB, 簇 L3 24MB)) with
 *  the sense-reversal round-barrier protocol (block-race writers,
 *  one full-array reader with sum verification).  Sums run in
 *  k-space: every element is an exact 0.5-multiple, so k = (int64)(v
 *  * 2) is exact and the atomic<uint64_t> accumulators and the
 *  reader's walk are exact-integer math (order-independent,
 *  bit-exact — the mesh f64 family discipline).  Spin throttles are
 *  scaled for the 4 MB fill (1048575 per budget check).
 */
#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <unistd.h>
#include <arm_neon.h>

#define ARRAY_DOUBLES (1 << 19)          /* 512K doubles = 4MB shared */
#define BLOCK_DOUBLES 16                 /* block = 8 f64x2 vectors */
#define VEC_LANES 2
#define VECS_PER_BLOCK (BLOCK_DOUBLES / VEC_LANES)
#define NUM_BLOCKS (ARRAY_DOUBLES / BLOCK_DOUBLES)

namespace { /* ODR: per-TU TestData */
struct TestData {
    alignas(16) double *data;              /* shared, 16B aligned */
    alignas(16) double *golden;            /* write-point double-written golden copy */
    std::atomic<uint32_t> next_block;      /* block race allocator */
    std::atomic<uint32_t> allocated_blocks;
    std::atomic<uint32_t> thread_idx;
    std::atomic<uint32_t> arrived;         /* sense-reversal barrier */
    std::atomic<uint32_t> epoch;
};
}

/* exact 0.5-multiple in [-2^19, 2^19); k = v*2 is an exact integer */
static inline double mswol_dist(void) {
    return (double)((int64_t)(random64() % (1ULL << 21)) - (1 << 20)) * 0.5;
}

static int mesh_upi_sse_write_only_neon_fp64_init(struct test *test) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) {
        return -255;   /* SKIP */
    }

    /* throwing-new OOM follows the repo T10 convention (cf.
     * gemm_cdouble_dynamic_square): bad_alloc -> clean SKIP. */
    TestData *td;
    try {
        td = new TestData;
    } catch (const std::bad_alloc &) {
        log_skip(RuntimeSkipCategory, "memory: %s TestData", test->id);
        return EXIT_SKIP;
    }

    td->data = (double *)aligned_alloc(16, ARRAY_DOUBLES * sizeof(double));
    td->golden = (double *)aligned_alloc(16, ARRAY_DOUBLES * sizeof(double));
    if (!td->data || !td->golden) {
        free(td->data); free(td->golden);
        delete td;
        return EXIT_FAILURE;
    }
    memset(td->data, 0, ARRAY_DOUBLES * sizeof(double));
    memset(td->golden, 0, ARRAY_DOUBLES * sizeof(double));

    td->next_block.store(0, std::memory_order_relaxed);
    td->allocated_blocks.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    td->arrived.store(0, std::memory_order_relaxed);
    td->epoch.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_sse_write_only_neon_fp64_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData *>(test->data);

    const int id = (int)td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    bool is_reader = (id == 0);

    uint32_t total_threads = thread_count();
    if (total_threads < 2) {
        log_skip(CpuTopologyIssueSkipCategory,
                 "mesh_upi_sse_write_only_neon_fp64 requires at least 2 "
                 "threads (inter-core test); skipping on this thread count");
        return EXIT_SKIP;
    }

    uint32_t my_epoch = 0;
    do {
        if (!is_reader) {
            /* writer: race-claim blocks until exhausted; per block:
             * fill 16 fresh 0.5-multiples via f64x2 stores, double-
             * written into the golden copy (element-exact reference,
             * 2026-11-18 rework) */
            while (true) {
                uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
                if (block >= NUM_BLOCKS) break;

                size_t offset = block * BLOCK_DOUBLES;
                double vals[BLOCK_DOUBLES];
                for (int j = 0; j < BLOCK_DOUBLES; ++j) {
                    vals[j] = mswol_dist();
                }
                for (int v = 0; v < VECS_PER_BLOCK; ++v) {
                    float64x2_t vec = vld1q_f64(vals + v * VEC_LANES);
                    vst1q_f64(td->data + offset + v * VEC_LANES, vec);
                    vst1q_f64(td->golden + offset + v * VEC_LANES, vec);
                }
                __sync_synchronize();

                td->allocated_blocks.fetch_add(1, std::memory_order_seq_cst);
            }
        } else {
            /* reader: wait for full allocation (throttled budget) */
            uint32_t spin = 0;
            while (td->allocated_blocks.load(std::memory_order_seq_cst) < NUM_BLOCKS) {
                if ((++spin & 1048575u) == 0) {
                    if (!test_time_condition(test)) goto out_of_budget;
                } else {
                    __asm__ volatile("yield");
                }
            }

            /* element-exact verification AFTER allocation completes
             * (2026-11-18 rework: the previous k-space sum compare was
             * blind to permutation/swap-class write-path damage; the
             * per-lane compare against the write-point golden copy is
             * not). Array and golden are both frozen post-allocation. */
            bool consistent = true;
            alignas(16) double store_buf[BLOCK_DOUBLES];

            for (size_t i = 0; i < ARRAY_DOUBLES; i += BLOCK_DOUBLES) {
                float64x2_t vecs[VECS_PER_BLOCK];
                for (int v = 0; v < VECS_PER_BLOCK; ++v) {
                    vecs[v] = vld1q_f64(td->data + i + v * VEC_LANES);
                    float64x2_t gv = vld1q_f64(td->golden + i + v * VEC_LANES);
                    uint64x2_t gdiff = veorq_u64(vreinterpretq_u64_f64(vecs[v]),
                                                 vreinterpretq_u64_f64(gv));
                    if (vmaxvq_u32(vreinterpretq_u32_u64(gdiff)) != 0) {
                        report_fail_msg("%s: golden mismatch at elem %zu: "
                                        "golden (%.17g, %.17g) got (%.17g, "
                                        "%.17g) (L3 write path, element-exact "
                                        "compare)",
                                        test->id, i + v * VEC_LANES,
                                        vgetq_lane_f64(gv, 0), vgetq_lane_f64(gv, 1),
                                        vgetq_lane_f64(vecs[v], 0), vgetq_lane_f64(vecs[v], 1));
                        return EXIT_FAILURE;
                    }
                }

                /* block store->reload consistency sub-check */
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

            if (!consistent) {
                report_fail_msg("%s: store->reload consistency failure "
                                "(LSU forwarding path)", test->id);
                return EXIT_FAILURE;
            }
        }

        /* sense-reversal round barrier; last arrival resets the round */
        {
            uint32_t done = td->arrived.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (done == total_threads) {
                td->next_block.store(0, std::memory_order_seq_cst);
                td->allocated_blocks.store(0, std::memory_order_seq_cst);
                td->arrived.store(0, std::memory_order_relaxed);
                td->epoch.store(my_epoch + 1, std::memory_order_release);
            } else {
                uint32_t spin = 0;
                while (td->epoch.load(std::memory_order_acquire) == my_epoch) {
                    if ((++spin & 1048575u) == 0) {
                        if (!test_time_condition(test)) goto out_of_budget;
                    } else {
                        __asm__ volatile("yield");
                    }
                }
            }
        }
        ++my_epoch;
    } while (test_time_condition(test));

out_of_budget:
    return EXIT_SUCCESS;
}

static int mesh_upi_sse_write_only_neon_fp64_finish(struct test *test) {
    auto *td = static_cast<TestData *>(test->data);
    free(td->data);
    free(td->golden);
    delete td;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

DECLARE_TEST(mesh_upi_sse_write_only_neon_fp64, "FP64 variant of mesh_upi_sse_write_only_int: cooperative writers fill a 4MB shared double array via f64x2 stores under a sense-reversal round barrier; the reader re-reads the whole array with exact k-space (0.5-multiple) sum verification (L3 write-path SDC) (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sse_write_only_neon_fp64_init,
    .test_run = mesh_upi_sse_write_only_neon_fp64_run,
    .test_cleanup = mesh_upi_sse_write_only_neon_fp64_finish,
    .fracture_loop_count = -1,   /* never fracture: 4MB-fill rounds */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
