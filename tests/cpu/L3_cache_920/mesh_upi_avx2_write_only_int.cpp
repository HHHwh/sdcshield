#include <sandstone.h>
#include <cstdint>
#include <cstring>
#include <atomic>
#ifdef __aarch64__
#include <arm_neon.h>
#include <unistd.h>

#define PER_THREAD_SIZE (1 << 20)      // 每个线程 1M 个 int32 = 4MB
#define VECTOR_SIZE 4                  // NEON 每次存储 4 个 int32（128 位）

// L3 几何注记（2026-11-17 L3 审计）：4MB/线程私有窗。鲲鹏 920 分布式
// L3 = 24MB/24 核簇——簇内并发 ≤6 线程时窗驻留 L3；全簇 24 线程时聚合
// 96MB ≫ 24MB 切片，写以流式+驱逐为主（L3 压力在、容量驻留不在）。
// 仓库单测试独占跑法处于驻留区。
// 2026-11-17 能力改造（用户指令）：两遍式——写满+golden sum → 延迟
// 全量重读复算比对。重读时首块早已被 4MB 流量逐出 512KB 私有 L2，
// 校验点真穿存储层级（L3/DRAM 供数），不再依赖立即回读转发。
// 2026-11-18 element-exact 收口（检视裁决）：pass 2 的校验由 sum 复算
// 改为对 init-time golden 副本逐元素比对——sum 对保和类损坏（块/行
// 置换、steering 对换）失明，逐元素比对不失明。golden 与 data 同点
// 双写、同序冻结，比较语义等价于「重读值 == 写入值」的逐元素形式。

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    std::atomic<uint64_t> thread_idx;
};
}

static int mesh_upi_avx2_write_only_int_init(struct test *test) {
    /* throwing-new OOM follows the repo T10 convention (cf.
     * gemm_cdouble_dynamic_square): bad_alloc -> clean SKIP. */
    TestData *td;
    try {
        td = new TestData;
    } catch (const std::bad_alloc &) {
        log_skip(RuntimeSkipCategory, "memory: %s TestData", test->id);
        return EXIT_SKIP;
    }
    td->thread_idx.store(0, std::memory_order_relaxed);
    test->data = td;
    return EXIT_SUCCESS;
}

static int mesh_upi_avx2_write_only_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    (void)td->thread_idx.fetch_add(1, std::memory_order_relaxed);   /* diagnostics only */

    // 每个线程独立分配自己的缓冲区（对齐到 16 字节，满足 NEON 要求）
    alignas(16) int32_t *buf = (int32_t*)aligned_alloc(16, PER_THREAD_SIZE * sizeof(int32_t));
    alignas(16) int32_t *golden = (int32_t*)aligned_alloc(16, PER_THREAD_SIZE * sizeof(int32_t));
    if (!buf || !golden) {
        free(buf); free(golden);
        report_fail_msg("Failed to allocate buffer");
        return EXIT_FAILURE;
    }

    /* randomization hardening H14' (P17): framework RNG (per-thread
     * stream, -s reproducible) replaces std::mt19937; range [-1000000, 1000000). */
    auto dist = []() { return (-1000000) + (int32_t)(random64() % (uint64_t)((1000000) - (-1000000) + 1)); };

    do {
        /* Pass 1 — write fill: fresh randoms through NEON stores,
         * doubled into the golden copy.  (2026-11-17 L3-capability
         * rework, user directive: the old immediate per-block
         * read-back was served by store-buffer/L1 forwarding and
         * never saw L2/L3 — the audit finding.) */
        for (size_t i = 0; i < PER_THREAD_SIZE; i += VECTOR_SIZE) {
            int32_t vals[VECTOR_SIZE];
            for (int j = 0; j < VECTOR_SIZE; ++j)
                vals[j] = dist();
            int32x4_t v = vld1q_s32(vals);
            vst1q_s32(buf + i, v);
            vst1q_s32(golden + i, v);
        }

        /* Pass 2 — delayed full re-read: the 4 MB buffer was just
         * streamed, so early lines are long evicted from the 512 KB
         * private L2 — this re-read is L3/DRAM-served and the
         * element-exact compare against the golden copy is a genuine
         * hierarchy verification point (2026-11-18 element-exact
         * rework: the previous sum compare was blind to
         * permutation/swap-class write-path damage). */
        for (size_t i = 0; i < PER_THREAD_SIZE; i += VECTOR_SIZE) {
            int32x4_t loaded = vld1q_s32(buf + i);
            int32x4_t g = vld1q_s32(golden + i);
            uint32x4_t diff = veorq_u32(vreinterpretq_u32_s32(loaded),
                                        vreinterpretq_u32_s32(g));
            if (vmaxvq_u32(diff) != 0) {
                report_fail_msg("%s: write->delayed-re-read golden "
                                "mismatch at vec %zu: golden {%d,%d,%d,%d} "
                                "got {%d,%d,%d,%d} (L3 write path, "
                                "element-exact compare)",
                                test->id, i / VECTOR_SIZE,
                                vgetq_lane_s32(g, 0), vgetq_lane_s32(g, 1),
                                vgetq_lane_s32(g, 2), vgetq_lane_s32(g, 3),
                                vgetq_lane_s32(loaded, 0), vgetq_lane_s32(loaded, 1),
                                vgetq_lane_s32(loaded, 2), vgetq_lane_s32(loaded, 3));
                free(buf); free(golden);
                return EXIT_FAILURE;
            }
        }

    } while (test_time_condition(test));

    free(buf);
    free(golden);
    return EXIT_SUCCESS;
}

static int mesh_upi_avx2_write_only_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_avx2_write_only_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_avx2_write_only_int");
    return EXIT_SKIP;
}
static int mesh_upi_avx2_write_only_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_avx2_write_only_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_avx2_write_only_int, "Mutual write stress MESH and UPI or Ring Interconnect and QPI (do int32 NEON stores to L1/L2/L3; write-fill then delayed full re-read with element-exact golden verify — genuine L3-path verification, 2026-11-18 rework)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_avx2_write_only_int_init,
    .test_run = mesh_upi_avx2_write_only_int_run,
    .test_cleanup = mesh_upi_avx2_write_only_int_finish,
    .fracture_loop_count = -1,   /* never fracture: rework rounds carry 8MB traffic, run the full -t */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
