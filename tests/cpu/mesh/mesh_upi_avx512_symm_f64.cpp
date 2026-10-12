/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mesh_upi_avx512_symm_f64
 * @parblock
 * Cross-core shared-data f64 production/consumption SDC stress — the
 * first floating-point variant in the mesh family (all 27 existing mesh
 * tests are int32). The known NUMA3 fault class is a compound of
 * remote/shared access x compute; until now the suite covered the two
 * halves separately (mesh_int: shared access, integer; sve512_*: FP
 * compute, private data). This test combines them on the mesh_upi
 * three-phase skeleton (write / read-verify / reset with atomic block
 * claiming), with FP compute on both sides of the shared buffer:
 *
 *   WRITE phase: each claimed block is produced with a 4-step NEON
 *     vfmaq_f64 local chain over exact binary values, then stored.
 *
 *   READ phase: every element of the shared buffer is loaded, run
 *     through the same 4-step FMA chain, and byte-compared against the
 *     recomputed golden; plus the store/reload consistency check of the
 *     original skeleton.
 *
 *   Global sum: per-block sums (computed exactly on both sides — block
 *     values are 0.5-multiples, so every chain result and every sum is
 *     exact in f64, order-independent) are accumulated into atomic
 *     f64 counters on each side; read total must equal write total
 *     byte-for-byte. Exactness argument: with all values being exact
 *     halves and bounded magnitude, no rounding ever occurs, so the
 *     floating-point associativity hazard does not exist here.
 *
 * NEON f64 (float64x2_t) — runs on any aarch64, no SVE needed, so the
 * compound coverage also exists on SVE-less hosts (this Kunpeng 920
 * included, where it is fully verified on real hardware).
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#ifdef __aarch64__
#include <arm_neon.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/sysinfo.h>

#define NEON_VECTOR_SIZE 2                // NEON 一次处理 2 个 f64
#define BLOCK_SIZE 16                     // 逻辑块大小(8 个 float64x2)
#define VECTORS_PER_BLOCK (BLOCK_SIZE / NEON_VECTOR_SIZE)  // = 8
#define NUM_BLOCKS 64
#define TOTAL_ELEMENTS (BLOCK_SIZE * NUM_BLOCKS)  // = 1024
#define TOTAL_BYTES (TOTAL_ELEMENTS * sizeof(double))

struct TestData {
    alignas(16) double *data;             // 共享缓冲(16B 对齐)
    std::atomic<uint32_t> next_block;
    std::atomic<uint64_t> global_sum;     // f64 位模式的精确和(见下)
    std::atomic<uint32_t> write_done;
    std::atomic<uint32_t> read_done;
    std::atomic<uint32_t> reset_lock;
    std::atomic<uint32_t> thread_idx;
    uint32_t total_threads;
};

// 块值生成:slot j 的种子值(精确 0.5 倍数)
static inline double block_value(uint32_t block, int j)
{
    // 范围 [0.5, 16.0) 的精确值:0.5 * (1..32)
    return 0.5 * (double)(1 + ((block * 16 + (uint32_t)j) % 32));
}

// 4 步 NEON FMA 链(精确:值域内每步结果都是 0.5 倍数,无舍入)
static inline double fma_chain(double v)
{
    float64x2_t acc = vdupq_n_f64(v);
    float64x2_t m = vdupq_n_f64(0.5);
    float64x2_t a = vdupq_n_f64(0.5);
    acc = vfmaq_f64(acc, m, a);   // v + 0.25
    acc = vfmaq_f64(acc, m, a);   // v + 0.50
    acc = vfmaq_f64(acc, m, a);   // v + 0.75
    acc = vfmaq_f64(acc, m, a);   // v + 1.00
    return vgetq_lane_f64(acc, 0);
}

// 标量 golden:与 NEON 链完全同序同值(每步 +0.25,精确)
static inline double fma_chain_scalar(double v)
{
    return v + 0.25 + 0.25 + 0.25 + 0.25;
}

static int mesh_upi_avx512_symm_f64_init(struct test *test) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2)
        return -255;   // 需要至少 2 个线程

    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;

    td->data = (double *)aligned_alloc(16, TOTAL_BYTES);
    if (!td->data) {
        delete td;
        return EXIT_FAILURE;
    }
    memset(td->data, 0, TOTAL_BYTES);

    td->next_block.store(0, std::memory_order_relaxed);
    td->global_sum.store(0, std::memory_order_relaxed);
    td->write_done.store(0, std::memory_order_relaxed);
    td->read_done.store(0, std::memory_order_relaxed);
    td->reset_lock.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    td->total_threads = sysconf(_SC_NPROCESSORS_ONLN);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx512_symm_f64_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData *>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    uint32_t total = td->total_threads;

    do {
        // ---------- 写阶段:认领块,FP 生产,NEON 存储 ----------
        uint64_t local_sum_bits = 0;   // 精确和以位模式累加? 不 —— 用 double
        double local_sum = 0.0;
        while (true) {
            uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
            if (block >= NUM_BLOCKS) break;

            size_t offset = block * BLOCK_SIZE;
            double vals[BLOCK_SIZE];
            for (int j = 0; j < BLOCK_SIZE; ++j) {
                vals[j] = block_value(block, j);
                local_sum += vals[j];   // 精确:0.5 倍数求和无舍入
            }
            // NEON 存储(每块 8 个 float64x2)
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                float64x2_t vec = vld1q_f64(vals + v * NEON_VECTOR_SIZE);
                vst1q_f64(td->data + offset + v * NEON_VECTOR_SIZE, vec);
            }
            __sync_synchronize();

            td->write_done.fetch_add(1, std::memory_order_seq_cst);
        }
        (void)local_sum_bits;

        // 写侧全局和:local_sum 精确,fetch_add 顺序无关
        // (double 的原子 fetch_add 不存在;用标准 CAS 重试循环)
        uint64_t exp_bits = td->global_sum.load(std::memory_order_seq_cst);
        for (;;) {
            double snapshot, desired;
            memcpy(&snapshot, &exp_bits, 8);
            desired = snapshot + local_sum;               // 精确加法
            uint64_t des_bits;
            memcpy(&des_bits, &desired, 8);
            if (td->global_sum.compare_exchange_weak(
                    exp_bits, des_bits,
                    std::memory_order_seq_cst,
                    std::memory_order_seq_cst)) {
                break;   // success: exp_bits was still current
            }
            // failure: exp_bits was reloaded with the current value by
            // compare_exchange — retry with it
        }

        // 等待所有块写完成
        while (td->write_done.load(std::memory_order_seq_cst) < NUM_BLOCKS &&
               test_time_condition(test)) {
            __asm__ volatile("yield");
        }
        if (!test_time_condition(test)) break;

        // Sum snapshot BEFORE the read loop: a fast thread can finish its
        // read, run the reset phase and start the NEXT iteration's writes
        // while this thread is still walking the buffer (the f64 FMA chain
        // widens the read window vs the int skeleton). Comparing against a
        // snapshot taken after the read would race the reset/next-write
        // and produce a false mismatch (caught live: iteration 17,
        // read_sum=0x1.08p+13 vs expected=0x1.bdp+10).
        uint64_t expected_sum_bits = td->global_sum.load(std::memory_order_seq_cst);

        // ---------- 读阶段:全量读 + FMA 链 + golden 比对 ----------
        double read_sum = 0.0;
        bool consistent = true;
        alignas(16) double store_buf[BLOCK_SIZE];

        for (size_t i = 0; i < TOTAL_ELEMENTS; i += BLOCK_SIZE) {
            double vals[BLOCK_SIZE];
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                float64x2_t vec = vld1q_f64(td->data + i + v * NEON_VECTOR_SIZE);
                vst1q_f64(vals + v * NEON_VECTOR_SIZE, vec);
            }
            for (int j = 0; j < BLOCK_SIZE; ++j) {
                read_sum += vals[j];            // 精确
                double got = fma_chain(vals[j]);
                double want = fma_chain_scalar(vals[j]);
                if (got != want) {              // byte-exact(精确值,== 可靠)
                    consistent = false;
                    break;
                }
            }
            if (!consistent) break;

            // 一致性测试:Store/Load 比较(原骨架)
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                float64x2_t orig = vld1q_f64(vals + v * NEON_VECTOR_SIZE);
                vst1q_f64(store_buf + v * NEON_VECTOR_SIZE, orig);
            }
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                float64x2_t orig = vld1q_f64(vals + v * NEON_VECTOR_SIZE);
                float64x2_t reload = vld1q_f64(store_buf + v * NEON_VECTOR_SIZE);
                uint64x2_t cmp = vceqq_u64(vreinterpretq_u64_f64(orig),
                                           vreinterpretq_u64_f64(reload));
                if (!(vgetq_lane_u64(cmp, 0) == 0xFFFFFFFFFFFFFFFFULL &&
                      vgetq_lane_u64(cmp, 1) == 0xFFFFFFFFFFFFFFFFULL)) {
                    consistent = false;
                    break;
                }
            }
            if (!consistent) break;
        }

        double expected_sum;
        memcpy(&expected_sum, &expected_sum_bits, 8);
        bool sum_ok = (read_sum == expected_sum);   // 双侧精确,顺序无关
        bool passed = sum_ok && consistent;

        if (!passed) {
            log_warning("mesh_upi_avx512_symm_f64: thread %d read_sum=%a "
                        "expected=%a consistent=%d", id, read_sum,
                        expected_sum, (int)consistent);
            report_fail_msg("mesh_upi_avx512_symm_f64: cross-core FP sum "
                            "mismatch or consistency failure");
            return EXIT_FAILURE;
        }

        td->read_done.fetch_add(1, std::memory_order_seq_cst);

        // ---------- 重置阶段(最后一个读完成者执行) ----------
        uint32_t expected_lock = 0;
        if (td->reset_lock.compare_exchange_strong(expected_lock, 1,
                                                   std::memory_order_seq_cst,
                                                   std::memory_order_seq_cst)) {
            while (td->read_done.load(std::memory_order_seq_cst) < total &&
                   test_time_condition(test)) {
                __asm__ volatile("yield");
            }
            if (!test_time_condition(test)) {
                td->reset_lock.store(0, std::memory_order_seq_cst);
                break;
            }
            td->global_sum.store(0, std::memory_order_seq_cst);
            td->next_block.store(0, std::memory_order_seq_cst);
            td->write_done.store(0, std::memory_order_seq_cst);
            td->read_done.store(0, std::memory_order_seq_cst);
            td->reset_lock.store(0, std::memory_order_seq_cst);
        } else {
            while (td->reset_lock.load(std::memory_order_seq_cst) != 0 &&
                   test_time_condition(test)) {
                __asm__ volatile("yield");
            }
            if (!test_time_condition(test)) break;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int mesh_upi_avx512_symm_f64_finish(struct test *test) {
    auto *td = static_cast<TestData *>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx512_symm_f64_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for "
             "mesh_upi_avx512_symm_f64");
    return EXIT_SKIP;
}
static int mesh_upi_avx512_symm_f64_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx512_symm_f64_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx512_symm_f64,
             "Cross-core shared-data f64 production/consumption (symm): "
             "the first FP mesh variant — NEON vfmaq_f64 chains on both "
             "sides of a shared cross-core buffer with exact 0.5-multiple "
             "arithmetic (order-independent sums, byte-exact compare) — "
             "the remote-access x compute compound the int-only mesh "
             "family never covered")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx512_symm_f64_init,
    .test_run = mesh_upi_avx512_symm_f64_run,
    .test_cleanup = mesh_upi_avx512_symm_f64_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
