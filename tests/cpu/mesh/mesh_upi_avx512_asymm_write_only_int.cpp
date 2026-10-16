#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#ifdef __aarch64__
#include <arm_neon.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/sysinfo.h>

#define TOTAL_ELEMENTS 1024               // 总元素数（小工作集，保证快速完成）
#define NEON_VECTOR_SIZE 4                // NEON 一次处理 4 个 int32
#define BLOCK_ELEMENTS 16                 // 每个块 16 个 int32（由 4 个 NEON 向量组成）
#define VECTORS_PER_BLOCK (BLOCK_ELEMENTS / NEON_VECTOR_SIZE)  // = 4
#define NUM_BLOCKS (TOTAL_ELEMENTS / BLOCK_ELEMENTS)           // = 64
#define TOTAL_BYTES (TOTAL_ELEMENTS * sizeof(int32_t))

/* 2026-10-16 协议重设计（参照 mesh_upi_avx512_symm_int 的 v2 范式
 * 13b13850 与 mesh_upi_avx2_asymm_int 的 G2'/G3' 修复）：原版 reset-winner
 * 清零协议（读者 CAS reset_lock、清零 global_sum/write_done/next_block，
 * 写者自旋等 next_block==0 重新入场）存在假 FAIL 竞态——清零/写者重入
 * 与读者的读取、快照之间无屏障隔离（asymm 家族已实证的跨迭代混合读 /
 * expected_sum=0）。新版结构性消除：
 *   1. 双感知反转屏障 B1（写后读前）/B2（读后写前）替代 reset-winner
 *      协议——无业务计数器清零、无胜者选举、无套圈窗口（B1 保证读者
 *      快照时全部写完成且无人开写下一迭代；B2 保证读者读完才允许
 *      覆写数组）；
 *   2. 累计和差分（expected = cum(K) - cum(K-1)）替代"每迭代清零
 *      global_sum"——global_sum 单调累加，B1 后读者读到稳定值；
 *   3. 写核心静态分区（写者 wid 固定负责 [b_lo, b_hi)）替代 next_block
 *      动态分配——每块恰一个写者，无分配竞争、无每迭代复位（保持
 *      "全部非读核共写数组、唯一读核校验"的写压力语义；W > NUM_BLOCKS
 *      时多余写者本轮空转，所有块仍恰好被写一次，sum 语义不变）。
 * 时间到期时各线程在自己的屏障自旋处退出（他人亦到期退出，有界无死锁）。 */
struct TestData {
    alignas(16) int32_t *data;           // 共享数据数组（16 字节对齐）
    std::atomic<uint64_t> global_sum;    // 跨迭代单调累计（差分得每迭代期望）
    std::atomic<uint32_t> thread_idx;    // 线程 ID 分配器
    std::atomic<uint32_t> b1_count, b1_sense;   // 屏障 B1: 写后读前
    std::atomic<uint32_t> b2_count, b2_sense;   // 屏障 B2: 读后写前
};

static int mesh_upi_avx512_asymm_write_only_int_init(struct test *test) {
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

    td->global_sum.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    td->b1_count.store(0, std::memory_order_relaxed);
    td->b1_sense.store(0, std::memory_order_relaxed);
    td->b2_count.store(0, std::memory_order_relaxed);
    td->b2_sense.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_avx512_asymm_write_only_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    bool is_reader = (id == 0);   // 一个读核心，其余为写核心（角色固定）

    /* 线程总数取框架权威值 thread_count()（自动尊重 --cpuset、
     * test.max_threads 与 OS 亲和性限制，每个线程取值一致）——屏障
     * 参与者数目必须全体一致，否则轮次隔离崩溃；单线程无法做跨核
     * 互写测试（原版单线程下空转 SUCCESS，属 vacuous pass）。 */
    uint32_t total = thread_count();
    if (total < 2) {
        log_skip(CpuTopologyIssueSkipCategory,
                 "mesh_upi_avx512_asymm_write_only_int requires at least 2 threads (inter-core test); "
                 "skipping on this thread count");
        return EXIT_SKIP;
    }

    /* randomization hardening H14' (P17): framework RNG (per-thread
     * stream, -s reproducible) replaces std::mt19937; range [-1000000, 1000000). */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    /* 感知反转屏障（v2 范式，参照 symm_int 13b13850 + avx2_asymm_int
     * G2'/G3'）：最后一个到达者清 cnt、翻转 sense 放行所有人；其余自旋
     * 等翻转。自旋按 1024 次一档节流检查时间预算——test_time_condition
     * 每次调用消耗 auto-fracture 的 inner_loop_count 预算（默认 40），
     * 写者们填满数组需数十微秒，逐自旋检查会在写完成前耗尽预算而全员
     * 提前退出。到期时各自从屏障退出，他人亦到期退出（有界、无死锁）。
     * 返回 false = 时间预算耗尽，调用方退出主循环。 */
    auto barrier = [&](std::atomic<uint32_t> &cnt, std::atomic<uint32_t> &sen) -> bool {
        uint32_t s = sen.load(std::memory_order_seq_cst);
        if (cnt.fetch_add(1, std::memory_order_seq_cst) + 1 == total) {
            cnt.store(0, std::memory_order_seq_cst);
            sen.store(1 - s, std::memory_order_seq_cst);
            return true;
        }
        uint32_t spin = 0;
        while (sen.load(std::memory_order_seq_cst) == s) {
            if ((++spin & 1023u) == 0) {
                if (!test_time_condition(test)) return false;
            } else {
                __asm__ volatile("yield");
            }
        }
        return true;
    };

    /* 写核心集合：id != 0 的 total-1 个线程（wid ∈ [0, W)）。静态分区：
     * 写者 wid 固定负责 [b_lo, b_hi)；末写者吃余数。W > NUM_BLOCKS 时
     * per=0，多余写者本轮空转（其 B1 到达即"完成"）——所有块仍恰好
     * 被写一次，sum 校验语义不变。 */
    uint32_t W = total - 1;
    uint32_t per = NUM_BLOCKS / W;
    uint32_t b_lo = 0, b_hi = 0;
    if (!is_reader) {
        uint32_t wid = (uint32_t)(id - 1);
        b_lo = wid * per;
        b_hi = (wid == W - 1) ? NUM_BLOCKS : b_lo + per;
    }

    uint64_t prev_cum = 0;    // 读者上一迭代 B1 快照（角色固定，写者无需差分）

    do {
        if (!is_reader) {
            // ---------- 写核心：写入自己的静态分区 ----------
            // （本轮状态无需清零——global_sum 单调累计，读者差分取值。）
            for (uint32_t block = b_lo; block < b_hi; ++block) {
                size_t offset = block * BLOCK_ELEMENTS;
                int32_t vals[BLOCK_ELEMENTS];
                uint64_t local_sum = 0;
                for (int j = 0; j < BLOCK_ELEMENTS; ++j) {
                    vals[j] = dist();
                    local_sum += (uint64_t)vals[j];
                }
                // 使用 4 个 NEON 向量存储 16 个元素
                for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                    int32x4_t vec = vld1q_s32(vals + v * NEON_VECTOR_SIZE);
                    vst1q_s32(td->data + offset + v * NEON_VECTOR_SIZE, vec);
                }
                __sync_synchronize();   // 确保写入对其他核心可见

                // 一致性测试：立即读回比较
                bool block_ok = true;
                for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                    int32x4_t written = vld1q_s32(vals + v * NEON_VECTOR_SIZE);
                    int32x4_t loaded = vld1q_s32(td->data + offset + v * NEON_VECTOR_SIZE);
                    uint32x4_t cmp = vceqq_s32(written, loaded);
                    // 检查所有元素相等
                    if (!(vgetq_lane_u32(cmp, 0) == 0xFFFFFFFF &&
                          vgetq_lane_u32(cmp, 1) == 0xFFFFFFFF &&
                          vgetq_lane_u32(cmp, 2) == 0xFFFFFFFF &&
                          vgetq_lane_u32(cmp, 3) == 0xFFFFFFFF)) {
                        block_ok = false;
                        break;
                    }
                }
                /* 写核心的立即读回是唯一逐字节的写入内容校验——读核心的
                 * sum 校验可被保和损坏（如元素交换）绕过，所以 block_ok
                 * 为假必须直接判失败，不能只记录。 */
                if (!block_ok) {
                    fprintf(stderr, "mesh_upi_avx512_asymm_write_only_int: Thread %d (writer), block %u immediate read-back mismatch, written vals[0..3]=(%d,%d,%d,%d), loaded data[0..3] at offset %zu=(%d,%d,%d,%d), result=%sFAIL%s\n",
                            id, block,
                            vals[0], vals[1], vals[2], vals[3],
                            offset,
                            td->data[offset], td->data[offset + 1],
                            td->data[offset + 2], td->data[offset + 3],
                            RED, RESET);
                    fflush(stderr);
                    report_fail_msg("mesh_upi_avx512_asymm_write_only_int: writer immediate read-back mismatch (store/load corruption)");
                    return EXIT_FAILURE;
                }

                td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
            }
        }

        // B1：本轮全部块写完成（此后到 B2 之间数组冻结、global_sum 稳定）
        if (!barrier(td->b1_count, td->b1_sense)) break;

        if (is_reader) {
            // 累计和差分快照：expected = cum(K) - cum(K-1)（B1 后无写 → 值稳定）
            uint64_t cum = td->global_sum.load(std::memory_order_seq_cst);
            uint64_t expected_sum = cum - prev_cum;   // 本迭代全部块之和
            prev_cum = cum;

            // 读取整个数组，计算校验和并进行一致性测试
            uint64_t read_sum = 0;
            bool consistent = true;
            alignas(16) int32_t store_buf[BLOCK_ELEMENTS];

            for (size_t i = 0; i < TOTAL_ELEMENTS; i += BLOCK_ELEMENTS) {
                // 加载 16 个元素
                int32_t vals[BLOCK_ELEMENTS];
                for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                    int32x4_t vec = vld1q_s32(td->data + i + v * NEON_VECTOR_SIZE);
                    vst1q_s32(vals + v * NEON_VECTOR_SIZE, vec);
                }
                for (int j = 0; j < BLOCK_ELEMENTS; ++j) {
                    read_sum += (uint64_t)vals[j];
                }

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
                        break;
                    }
                }
            }

            bool sum_ok = (read_sum == expected_sum);
            bool passed = sum_ok && consistent;

            if (!passed) {
                // 首次失败证据（原先每轮都从读核心打印 PASS 行）
                fprintf(stderr, "mesh_upi_avx512_asymm_write_only_int: Thread %d (reader), data[0..15]=(%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, consistent=%d, result=%s%s%s\n",
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
                report_fail_msg("mesh_upi_avx512_asymm_write_only_int: Sum mismatch or consistency failure");
                return EXIT_FAILURE;
            }
        }

        // B2：读者读完成，数组保护解除（下一迭代写可开始）
        if (!barrier(td->b2_count, td->b2_sense)) break;

    } while (test_time_condition(test));

    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_avx512_asymm_write_only_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx512_asymm_write_only_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_avx512_asymm_write_only_int");
    return EXIT_SKIP;
}
static int mesh_upi_avx512_asymm_write_only_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx512_asymm_write_only_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx512_asymm_write_only_int,
             "Mutual write stress MESH and UPI (do NEON stores to L1/L2/L3)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx512_asymm_write_only_int_init,
    .test_run = mesh_upi_avx512_asymm_write_only_int_run,
    .test_cleanup = mesh_upi_avx512_asymm_write_only_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
