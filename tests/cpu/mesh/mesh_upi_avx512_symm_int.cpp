#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#ifdef __aarch64__
#include <arm_neon.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/sysinfo.h>

#define NEON_VECTOR_SIZE 4                // NEON 一次处理 4 个 int32
#define BLOCK_SIZE 16                     // 逻辑块大小（由 4 个 NEON 向量组成）
#define VECTORS_PER_BLOCK (BLOCK_SIZE / NEON_VECTOR_SIZE)  // = 4
#define NUM_BLOCKS 256                    // 块数量（≥最大线程数, 静态分区需要）
#define TOTAL_ELEMENTS (BLOCK_SIZE * NUM_BLOCKS)
#define TOTAL_BYTES (TOTAL_ELEMENTS * sizeof(int32_t))

/* 2026-10-16 协议重设计（v2）：原版（共享 next_block 计数器 + 写屏障 +
 * reset-winner 清零协议）存在竞态——簇锚定 dwell 4 核 8/8 轮假 FAIL
 * （expected_sum=0 + 跨迭代混合读，取证增强实证；对照簇同型复现 → 非硬件）。
 * v2 三项结构性消除：
 *   1. 静态块分区（线程 t 固定负责一段块）——删除 next_block 共享计数器
 *      及其每迭代复位；
 *   2. 双感知反转屏障 B1（写后）/B2（读后）替代 reset-winner 协议——无计数
 *      器清零、无胜者选举、无套圈窗口（B1 保证快照时全部写完成且无人开写
 *      下一迭代；B2 保证全部读完才允许覆写数组）；
 *   3. 累计和差分（expected = cum(K) - cum(K-1)）替代"每迭代清零 global_sum"
 *      ——global_sum 单调累加, B1 后全线程读到相同值。
 * 时间到期时各线程在自己的屏障自旋处退出（他人亦到期退出, 有界无死锁）。 */
struct TestData {
    alignas(16) int32_t *data;
    std::atomic<uint64_t> global_sum;       // 跨迭代单调累计（差分得每迭代期望）
    std::atomic<uint32_t> thread_idx;
    std::atomic<uint32_t> b1_count, b1_sense;   // 屏障 B1: 写后读前
    std::atomic<uint32_t> b2_count, b2_sense;   // 屏障 B2: 读后写前
    uint64_t expected_block_sums[NUM_BLOCKS];   // 写侧逐块真值和（仅失败取证用）
};

static int mesh_upi_avx512_symm_int_init(struct test *test) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) {
        return -255;   // 需要至少 2 个线程
    }

    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;

    td->data = (int32_t*)aligned_alloc(16, TOTAL_BYTES);
    if (!td->data) {
        delete td;
        return EXIT_FAILURE;
    }
    memset(td->data, 0, TOTAL_BYTES);
    memset(td->expected_block_sums, 0, sizeof(td->expected_block_sums));

    td->global_sum.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    td->b1_count.store(0, std::memory_order_relaxed);
    td->b1_sense.store(0, std::memory_order_relaxed);
    td->b2_count.store(0, std::memory_order_relaxed);
    td->b2_sense.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx512_symm_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    uint32_t total = thread_count();

    /* randomization hardening H14' (P17): framework RNG (per-thread
     * stream, -s reproducible) replaces std::mt19937; range [-1000000, 1000000). */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    // 感知反转屏障：最后一个到达者翻转 sense 放行所有人；其余自旋等翻转。
    // 自旋带时间条件——到期各自退出，他人也到期退出（有界，无死锁）。
    auto barrier = [&](std::atomic<uint32_t> &cnt, std::atomic<uint32_t> &sen) {
        uint32_t s = sen.load(std::memory_order_seq_cst);
        if (cnt.fetch_add(1, std::memory_order_seq_cst) + 1 == total) {
            cnt.store(0, std::memory_order_seq_cst);
            sen.store(1 - s, std::memory_order_seq_cst);
        } else {
            while (sen.load(std::memory_order_seq_cst) == s &&
                   test_time_condition(test)) {
                __asm__ volatile("yield");
            }
        }
    };

    // 静态块分区：线程 id 固定负责 [b_lo, b_hi)；末线程吃余数。total > NUM_BLOCKS
    // 时靠 NUM_BLOCKS=256 ≥ 最大线程数保证（thread_count 上限即在线 CPU 数）。
    uint32_t k = NUM_BLOCKS / total;
    uint32_t b_lo = (uint32_t)id * k;
    uint32_t b_hi = (id == (int)total - 1) ? NUM_BLOCKS : b_lo + k;

    uint64_t prev_cum = 0;    // 上一迭代 B1 快照（B1..B2 间无写 → 全线程相同值）

    do {
        // ---------- 写阶段：静态分区，逐块写+记账 ----------
        for (uint32_t block = b_lo; block < b_hi; ++block) {
            size_t offset = block * BLOCK_SIZE;
            int32_t vals[BLOCK_SIZE];
            uint64_t bsum = 0;
            for (int j = 0; j < BLOCK_SIZE; ++j) {
                vals[j] = dist();
                bsum += (uint64_t)vals[j];
            }
            // 使用 4 个 NEON 向量存储 16 个元素
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                int32x4_t vec = vld1q_s32(vals + v * NEON_VECTOR_SIZE);
                vst1q_s32(td->data + offset + v * NEON_VECTOR_SIZE, vec);
            }
            __sync_synchronize();   // 类似 _mm_sfence

            td->expected_block_sums[block] = bsum;   // 块真值（取证用，读者在 B1 后读）
            td->global_sum.fetch_add(bsum, std::memory_order_seq_cst);
        }

        // B1：全部块写完成（此后到 B2 之间数组冻结、global_sum 稳定）
        barrier(td->b1_count, td->b1_sense);
        if (!test_time_condition(test)) break;

        uint64_t cum = td->global_sum.load(std::memory_order_seq_cst);
        uint64_t expected_sum = cum - prev_cum;   // 本迭代全部块之和
        prev_cum = cum;

        // ---------- 读阶段：读取整个数组并校验 ----------
        uint64_t read_sum = 0;
        bool consistent = true;
        uint64_t block_sums[NUM_BLOCKS];          // 逐块读和（仅失败取证用）
        size_t bad_block = 0; int bad_vec = -1;   // 一致性首失配位置（仅取证）
        int32_t bad_orig[NEON_VECTOR_SIZE], bad_reload[NEON_VECTOR_SIZE];
        alignas(16) int32_t store_buf[BLOCK_SIZE];   // 用于一致性测试

        for (size_t i = 0; i < TOTAL_ELEMENTS; i += BLOCK_SIZE) {
            // 加载 16 个元素
            int32_t vals[BLOCK_SIZE];
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                int32x4_t vec = vld1q_s32(td->data + i + v * NEON_VECTOR_SIZE);
                vst1q_s32(vals + v * NEON_VECTOR_SIZE, vec);
            }
            uint64_t block_sum = 0;
            for (int j = 0; j < BLOCK_SIZE; ++j) {
                read_sum += (uint64_t)vals[j];
                block_sum += (uint64_t)vals[j];
            }
            block_sums[i / BLOCK_SIZE] = block_sum;

            // 一致性测试：Store/Load 比较
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                int32x4_t orig = vld1q_s32(vals + v * NEON_VECTOR_SIZE);
                vst1q_s32(store_buf + v * NEON_VECTOR_SIZE, orig);
            }
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                int32x4_t orig = vld1q_s32(vals + v * NEON_VECTOR_SIZE);
                int32x4_t reload = vld1q_s32(store_buf + v * NEON_VECTOR_SIZE);
                uint32x4_t cmp = vceqq_s32(orig, reload);
                if (!(vgetq_lane_u32(cmp, 0) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 1) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 2) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 3) == 0xFFFFFFFF)) {
                    consistent = false;
                    bad_block = i / BLOCK_SIZE; bad_vec = v;
                    vst1q_s32(bad_orig, orig);
                    vst1q_s32(bad_reload, reload);
                    break;
                }
            }
            if (!consistent) break;
        }

        bool sum_ok = (read_sum == expected_sum);
        bool passed = sum_ok && consistent;

        if (!passed) {
            // 首次失败证据
            fprintf(stderr, "mesh_upi_avx512_symm_int: Thread %d, data[0..15]=(%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, consistent=%d, result=%s%s%s\n",
                    id,
                    td->data[0], td->data[1], td->data[2], td->data[3],
                    td->data[4], td->data[5], td->data[6], td->data[7],
                    td->data[8], td->data[9], td->data[10], td->data[11],
                    td->data[12], td->data[13], td->data[14], td->data[15],
                    read_sum, expected_sum, consistent,
                    passed ? GREEN : RED,
                    passed ? "PASS" : "FAIL",
                    RESET);
            fflush(stderr);

            // ---- 失败取证（不改变判定语义，仅增证据）----
            if (!sum_ok && consistent) {
                // 纯求和失配。二次全量读：本线程未过 B2 → 其余线程仍阻塞在
                // B2 自旋（或已到期退出）→ 数组在本线程两次读之间必然冻结。
                uint64_t read_sum2 = 0;
                uint64_t block_sums2[NUM_BLOCKS];
                for (size_t i = 0; i < TOTAL_ELEMENTS; i += BLOCK_SIZE) {
                    int32_t v2[BLOCK_SIZE];
                    for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                        int32x4_t vec = vld1q_s32(td->data + i + v * NEON_VECTOR_SIZE);
                        vst1q_s32(v2 + v * NEON_VECTOR_SIZE, vec);
                    }
                    uint64_t bs2 = 0;
                    for (int j = 0; j < BLOCK_SIZE; ++j) bs2 += (uint64_t)v2[j];
                    block_sums2[i / BLOCK_SIZE] = bs2;
                    read_sum2 += bs2;
                }
                const char *verdict =
                    (read_sum2 == read_sum)
                        ? "STABLE-MISMATCH (re-read equals first read; array content differs from expected — check protocol/golden)"
                        : "UNSTABLE (array changed between reads despite B2 protection — investigate)";
                fprintf(stderr, "mesh_upi_avx512_symm_int: Thread %d re-read_sum=%lu (first=%lu, expected=%lu) -> %s\n",
                        id, read_sum2, read_sum, expected_sum, verdict);
                for (int b = 0; b < NUM_BLOCKS; ++b) {
                    if (block_sums[b] != td->expected_block_sums[b] ||
                        block_sums[b] != block_sums2[b]) {
                        const int32_t *p = td->data + b * BLOCK_SIZE;
                        fprintf(stderr, "mesh_upi_avx512_symm_int: Thread %d block %d: first-read=%lu re-read=%lu write-side=%lu, block re-read values=(%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d)\n",
                                id, b, block_sums[b], block_sums2[b],
                                td->expected_block_sums[b],
                                p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],
                                p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
                        break;
                    }
                }
            }
            if (!consistent && bad_vec >= 0) {
                // 一致性失配：lane 级原始值 vs 重载值（位级直接可见）
                fprintf(stderr, "mesh_upi_avx512_symm_int: Thread %d consistency first-mismatch block %zu vec %d: orig=(%d,%d,%d,%d) reload=(%d,%d,%d,%d)\n",
                        id, bad_block, bad_vec,
                        bad_orig[0], bad_orig[1], bad_orig[2], bad_orig[3],
                        bad_reload[0], bad_reload[1], bad_reload[2], bad_reload[3]);
            }
            fflush(stderr);
            report_fail_msg("mesh_upi_avx512_symm_int: Sum mismatch or consistency failure");
            return EXIT_FAILURE;
        }

        // B2：全部读完成，数组保护解除（下一迭代写可开始）
        barrier(td->b2_count, td->b2_sense);
        if (!test_time_condition(test)) break;

    } while (test_time_condition(test));

    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_avx512_symm_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx512_symm_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_avx512_symm_int");
    return EXIT_SKIP;
}
static int mesh_upi_avx512_symm_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx512_symm_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx512_symm_int,
             "Symmetrical stress MESH and UPI (do int32 NEON loads and stores from L1/L2 to L1/L2 with different directions and switching cores)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx512_symm_int_init,
    .test_run = mesh_upi_avx512_symm_int_run,
    .test_cleanup = mesh_upi_avx512_symm_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
