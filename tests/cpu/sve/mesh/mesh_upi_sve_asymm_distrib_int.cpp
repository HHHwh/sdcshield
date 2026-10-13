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
#define BLOCK_ELEMENTS (VECTOR_SIZE * 2)
#define NUM_BLOCKS (TOTAL_ELEMENTS / BLOCK_ELEMENTS)
#define TOTAL_BYTES (TOTAL_ELEMENTS * sizeof(int32_t))

struct TestData {
    alignas(16) int32_t *data;           // 16 字节对齐即可
    std::atomic<uint32_t> next_block;
    std::atomic<uint64_t> global_sum;
    std::atomic<uint32_t> allocated_blocks;
    std::atomic<uint32_t> thread_idx;
    std::atomic<uint32_t> round_done;
    std::atomic<uint64_t> iter;
    std::atomic<uint32_t> num_threads;
    std::atomic<uint32_t> ready;
    std::atomic<uint32_t> reader_count;   // 读核心数量
};

static int mesh_upi_sve_asymm_distrib_int_init(struct test *test) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) return -255;

    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for mesh_upi_sve_asymm_distrib_int");
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
    td->num_threads.store(0, std::memory_order_relaxed);
    td->ready.store(0, std::memory_order_relaxed);
    td->reader_count.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_sve_asymm_distrib_int_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    bool is_writer = (id == 0);

    if (!is_writer) {
        td->reader_count.fetch_add(1, std::memory_order_relaxed);
    }

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int32_t> dist(-1000000, 1000000);

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        if (is_writer) {
            while (test_time_condition(test)) {
                // 等待读核心完成上一轮（allocated_blocks == 0 且 round_done == 0）
                while (td->allocated_blocks.load(std::memory_order_seq_cst) > 0 ||
                       td->round_done.load(std::memory_order_seq_cst) > 0) {
                    if (!test_time_condition(test)) break;
                    __asm__ volatile("yield");
                }
                if (!test_time_condition(test)) break;

                uint32_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
                if (block < NUM_BLOCKS) {
                    // 生成随机数据并写入（8个 int32，分两个向量）
                    int32_t vals[BLOCK_ELEMENTS];
                    uint64_t local_sum = 0;
                    for (int j = 0; j < BLOCK_ELEMENTS; ++j) {
                        vals[j] = dist(rng);
                        local_sum += (uint64_t)vals[j];
                    }
                    // 前 4 个 + 后 4 个（SVE 谓词分批）
                    for (int v = 0; v < 2; ++v) {
                        svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                        svint32_t vec = svld1_s32(pg, vals + v * VECTOR_SIZE);
                        svst1_s32(pg, td->data + v * VECTOR_SIZE, vec);
                    }
                    // 内存屏障确保数据对其它核心可见
                    __sync_synchronize();

                    td->global_sum.fetch_add(local_sum, std::memory_order_seq_cst);
                    td->allocated_blocks.fetch_add(1, std::memory_order_seq_cst);
                }
            }
        } else {
            // 读核心
            while (test_time_condition(test)) {
                // 等待写核心完成写入
                while (td->allocated_blocks.load(std::memory_order_seq_cst) < NUM_BLOCKS &&
                       test_time_condition(test)) {
                    __asm__ volatile("yield");
                }
                if (!test_time_condition(test)) break;

                // 读取数据（两个向量）
                int32_t vals[BLOCK_ELEMENTS];
                for (int v = 0; v < 2; ++v) {
                    svbool_t pg = svwhilelt_b32((uint64_t)0, (uint64_t)VECTOR_SIZE);
                    svint32_t vec = svld1_s32(pg, td->data + v * VECTOR_SIZE);
                    svst1_s32(pg, vals + v * VECTOR_SIZE, vec);
                }
                uint64_t read_sum = 0;
                for (int j = 0; j < BLOCK_ELEMENTS; ++j) {
                    read_sum += (uint64_t)vals[j];
                }

                uint64_t expected_sum = td->global_sum.load(std::memory_order_seq_cst);
                bool passed = (read_sum == expected_sum);

                // 输出结果（仅第一个读核心输出以节省日志）
                if (id == 1) {
                    fprintf(stderr, "mesh_upi_sve_asymm_distrib_int: Thread %d (reader), data[0..7]=(%d,%d,%d,%d,%d,%d,%d,%d), read_sum=%lu, expected_sum=%lu, result=%s%s%s\n",
                            id,
                            td->data[0], td->data[1], td->data[2], td->data[3],
                            td->data[4], td->data[5], td->data[6], td->data[7],
                            read_sum, expected_sum,
                            passed ? GREEN : RED,
                            passed ? "PASS" : "FAIL",
                            RESET);
                    fflush(stderr);
                }

                if (!passed) {
                    /* cold path: record bit-level evidence for the failing leg(s) */
                    if (read_sum != expected_sum)
                        log_error("mesh_upi_sve_asymm_distrib_int: sum mismatch: golden=0x%016llx actual=0x%016llx xor=0x%016llx",
                                  (unsigned long long)expected_sum, (unsigned long long)read_sum,
                                  (unsigned long long)(expected_sum ^ read_sum));
                    report_fail_msg("mesh_upi_sve_asymm_distrib_int: Sum mismatch");
                    return EXIT_FAILURE;
                }

                // 同步重置：最后一个读核心重置状态
                uint32_t done = td->round_done.fetch_add(1, std::memory_order_seq_cst) + 1;
                uint32_t total_readers = td->reader_count.load(std::memory_order_acquire);
                if (done == total_readers) {
                    td->allocated_blocks.store(0, std::memory_order_seq_cst);
                    td->next_block.store(0, std::memory_order_seq_cst);
                    td->global_sum.store(0, std::memory_order_seq_cst);
                    td->round_done.store(0, std::memory_order_seq_cst);
                } else {
                    while (td->round_done.load(std::memory_order_seq_cst) != 0 &&
                           test_time_condition(test)) {
                        __asm__ volatile("yield");
                    }
                    if (!test_time_condition(test)) break;
                }
            }
        }
    } while (test_time_condition(test));

    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_sve_asymm_distrib_int_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_sve_asymm_distrib_int_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_sve_asymm_distrib_int");
    return EXIT_SKIP;
}
static int mesh_upi_sve_asymm_distrib_int_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_sve_asymm_distrib_int_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_sve_asymm_distrib_int,
             "Asymmetrical(1 write core to all rest distributed read cores) stress MESH and UPI or Ring Interconnect and QPI (do int32 SVE loads and stores from L1/L2 to L1/L2 with switching write core) [ARM SVE version]")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sve_asymm_distrib_int_init,
    .test_run = mesh_upi_sve_asymm_distrib_int_run,
    .test_cleanup = mesh_upi_sve_asymm_distrib_int_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
