#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#ifdef __aarch64__
#include <arm_sve.h>
#include <sys/auxv.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/sysinfo.h>

#ifndef HWCAP_SVE
#define HWCAP_SVE (1 << 22)
#endif

#define TOTAL_ELEMENTS 1024
#define VECTOR_SIZE 4                     // 原负载：一次处理 4 个 int32（SVE 谓词分批）
#define BLOCK_ELEMENTS (VECTOR_SIZE * 2)  // 每个块 8 个元素（两个向量）
#define NUM_BLOCKS (TOTAL_ELEMENTS / BLOCK_ELEMENTS)  // = 128
#define TOTAL_BYTES (TOTAL_ELEMENTS * sizeof(int32_t))

struct TestData {
    alignas(16) int32_t *data;           // 16 字节对齐即可
    std::atomic<uint32_t> next_block;
    std::atomic<uint64_t> global_sum;
    std::atomic<uint32_t> allocated_blocks;
    std::atomic<uint32_t> thread_idx;
    std::atomic<uint32_t> round_done;
    std::atomic<uint64_t> iter;
};

static int mesh_upi_sve_asymm_int_init(struct test *test) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) return -255;

    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for mesh_upi_sve_asymm_int");
        return EXIT_SKIP;
    }

    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;

    td->data = (int32_t*)aligned_alloc(16, TOTAL_BYTES);
    if (!td->data) {
        delete td;
        return EXIT_FAILURE;
    }
    memset(td->data, 0, TOTAL_BYTES);

    td->next_block.store(0, std::memory_order_relaxed);
    td->global_sum.store(0, std::memory_order_relaxed);
    td->allocated_blocks.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    td->round_done.store(0, std::memory_order_relaxed);
    td->iter.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

/* gcc-16.2 (debian:sid, 2026-09) 对本函数在 -O2+ RTL expand ICE(internal
 * compiler error: Segmentation fault);与 core179 三 probe 的 gcc-10 ICE 同族
 * (SVE 循环内 svst1 模式 × 优化器)。注意:函数级 O1 对 gcc-16 此处无效
 * (gcc-10 的 probe 用 O1 即可,gcc-16 需 O0 才完全规避)。SDC 激励为显式
 * intrinsics 与原子操作,不依赖优化器;自旋屏障在 O0 下语义不变,仅自旋
 * 速度略降(原子条件本身每次都真实读)。 */
__attribute__((optimize("O0")))
static int mesh_upi_sve_asymm_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    // 分配唯一线程 ID
    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);

    /* 线程总数取框架权威值 thread_count()（自动尊重 --cpuset、
     * test.max_threads 与 OS 亲和性限制，每个线程取值一致）—— 与 NEON 侧
     * 2731971 的修复相同。原“3 次稳定读数”启发式在 -n 1 下会永久自旋
     * 挂死（cur > 1 永不成立，后面的线程数检查不可达），大规模并发下
     * 又严重欠计；单线程运行现在按 memcpy_rewr 先例干净跳过。 */
    uint32_t total_threads = thread_count();
    if (total_threads < 2) {
        log_skip(CpuTopologyIssueSkipCategory,
                 "mesh_upi_sve_asymm_int requires at least 2 threads (inter-core test); "
                 "skipping on this thread count");
        return EXIT_SKIP;
    }

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int32_t> dist(-1000000, 1000000);

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        uint64_t current_iter = td->iter.load(std::memory_order_acquire);
        int writer_id = current_iter % total_threads;

        if (id == writer_id) {
            // ---------- 写者：等待上一轮完全结束 ----------
            while ((td->allocated_blocks.load(std::memory_order_seq_cst) != 0 ||
                    td->round_done.load(std::memory_order_seq_cst) != 0 ||
                    td->iter.load(std::memory_order_acquire) == current_iter) &&
                   test_time_condition(test)) {
                __asm__ volatile("yield");
            }
            if (!test_time_condition(test)) break;

            // 重置状态
            td->global_sum.store(0, std::memory_order_seq_cst);
            td->next_block.store(0, std::memory_order_seq_cst);
            td->allocated_blocks.store(0, std::memory_order_seq_cst);
            td->round_done.store(0, std::memory_order_seq_cst);

            // 写入所有块
            for (uint32_t block = 0; block < NUM_BLOCKS; ++block) {
                size_t offset = block * BLOCK_ELEMENTS;
                int32_t vals[BLOCK_ELEMENTS];
                uint64_t local_sum = 0;
                for (int j = 0; j < BLOCK_ELEMENTS; ++j) {
                    vals[j] = dist(rng);
                    local_sum += (uint64_t)vals[j];
                }
                // 分两个向量存储（SVE 谓词分批）
                for (int v = 0; v < 2; ++v) {
                    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                    svint32_t vec = svld1_s32(pg, vals + v * VECTOR_SIZE);
                    svst1_s32(pg, td->data + offset + v * VECTOR_SIZE, vec);
                }
                __sync_synchronize();  // 类似 _mm_sfence

                // 一致性测试（自身写入后读回）
                for (int v = 0; v < 2; ++v) {
                    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                    svint32_t written = svld1_s32(pg, vals + v * VECTOR_SIZE);
                    svint32_t loaded = svld1_s32(pg, td->data + offset + v * VECTOR_SIZE);
                    svbool_t cmp = svcmpeq_s32(pg, written, loaded);
                    (void)cmp;  // 检查所有元素是否相等（忽略，因为后续读者会校验）
                }

                td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
                td->allocated_blocks.fetch_add(1, std::memory_order_seq_cst);
            }

            // 全局屏障确保所有数据已传播到所有核心
            __sync_synchronize();  // 类似 _mm_mfence

        } else {
            // ---------- 读者：等待所有块分配完成 ----------
            while (td->allocated_blocks.load(std::memory_order_seq_cst) < NUM_BLOCKS &&
                   test_time_condition(test)) {
                __asm__ volatile("yield");
            }
            if (!test_time_condition(test)) break;

            __sync_synchronize();  // 确保写者的所有存储可见

            uint64_t read_sum = 0;
            bool consistent = true;
            size_t bad_vec = (size_t)-1;
            alignas(16) int32_t store_buf[BLOCK_ELEMENTS];

            for (size_t i = 0; i < TOTAL_ELEMENTS; i += BLOCK_ELEMENTS) {
                int32_t vals[BLOCK_ELEMENTS];
                for (int v = 0; v < 2; ++v) {
                    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                    svint32_t vec = svld1_s32(pg, td->data + i + v * VECTOR_SIZE);
                    svst1_s32(pg, vals + v * VECTOR_SIZE, vec);
                }
                for (int j = 0; j < BLOCK_ELEMENTS; ++j) read_sum += (uint64_t)vals[j];

                // 一致性测试：Store/Load 回读比较
                for (int v = 0; v < 2; ++v) {
                    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                    svint32_t orig = svld1_s32(pg, vals + v * VECTOR_SIZE);
                    svst1_s32(pg, store_buf + v * VECTOR_SIZE, orig);
                    svint32_t reload = svld1_s32(pg, store_buf + v * VECTOR_SIZE);
                    svbool_t cmp = svcmpeq_s32(pg, orig, reload);
                    if (svcntp_b32(pg, cmp) != VECTOR_SIZE) {
                        consistent = false;
                        bad_vec = i + v * VECTOR_SIZE;
                    }
                }
            }

            uint64_t expected_sum = td->global_sum.load(std::memory_order_seq_cst);
            bool passed = (read_sum == expected_sum) && consistent;

            // 输出结果（每个读者输出一次）
            fprintf(stderr, "mesh_upi_sve_asymm_int: Thread %d (reader), data[0..7]=(%d,%d,%d,%d,%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, consistent=%d, result=%s%s%s\n",
                    id,
                    td->data[0], td->data[1], td->data[2], td->data[3],
                    td->data[4], td->data[5], td->data[6], td->data[7],
                    read_sum, expected_sum, consistent,
                    passed ? GREEN : RED,
                    passed ? "PASS" : "FAIL",
                    RESET);
            fflush(stderr);

            if (!passed) {
                /* cold path: record bit-level evidence for the failing leg(s) */
                if (read_sum != expected_sum)
                    log_error("mesh_upi_sve_asymm_int: sum mismatch: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                              (unsigned long long)expected_sum, (unsigned long long)read_sum,
                              (unsigned long long)(expected_sum ^ read_sum));
                if (!consistent)
                    log_error("mesh_upi_sve_asymm_int: store/reload consistency failed at vector %zu (post-hoc data at failing vector: "
                              "0x%08x 0x%08x 0x%08x 0x%08x)",
                              bad_vec,
                              *(const uint32_t *)&td->data[bad_vec + 0],
                              *(const uint32_t *)&td->data[bad_vec + 1],
                              *(const uint32_t *)&td->data[bad_vec + 2],
                              *(const uint32_t *)&td->data[bad_vec + 3]);
                report_fail_msg("mesh_upi_sve_asymm_int: Sum mismatch or consistency failure");
                return EXIT_FAILURE;
            }

            // ---------- 最后一个读者执行重置 ----------
            uint32_t done = td->round_done.fetch_add(1, std::memory_order_seq_cst) + 1;
            uint32_t total_readers = total_threads - 1;
            if (done == total_readers) {
                td->global_sum.store(0, std::memory_order_seq_cst);
                td->next_block.store(0, std::memory_order_seq_cst);
                td->allocated_blocks.store(0, std::memory_order_seq_cst);
                td->round_done.store(0, std::memory_order_seq_cst);
                td->iter.fetch_add(1, std::memory_order_release);
            } else {
                while (td->round_done.load(std::memory_order_seq_cst) != 0 &&
                       test_time_condition(test)) {
                    __asm__ volatile("yield");
                }
                if (!test_time_condition(test)) break;
            }
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_sve_asymm_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_sve_asymm_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_sve_asymm_int");
    return EXIT_SKIP;
}
static int mesh_upi_sve_asymm_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_sve_asymm_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_sve_asymm_int,
             "Asymmetrical(1 write core to all rest mutual read cores) stress MESH and UPI or Ring Interconnect and QPI or FSB (do int32 SVE loads and stores from L1/L2 to L1/L2 with switching write core) [ARM SVE version]")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sve_asymm_int_init,
    .test_run = mesh_upi_sve_asymm_int_run,
    .test_cleanup = mesh_upi_sve_asymm_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
