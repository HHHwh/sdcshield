/*  NEON variant of tests/cpu/memory/memcpy_sem_neon.cpp — auto-derived by the 2026-11-02
 *  cache/memory NEON pass: load-point neon_copy() calls replaced with the explicit
 *  vld1q/vst1q stream in arm_add_neon_copy.h (bypasses the libc IFUNC dispatcher,
 *  keeps the NEON datapath attribution).  Original test untouched; identifiers
 *  carry the _neon suffix to avoid ODR clashes with the original TU.
 */
#include <sandstone.h>
#include "arm_add_neon_copy.h"
using arm_add_neon::neon_copy;
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <vector>
#include <thread>
#include <semaphore.h>
#include <unistd.h>

static constexpr size_t BLOCK_SIZE = 256;          // 每线程数据块大小
static constexpr int NUM_THREADS = 4;              // 并发线程数

// 每个线程的私有数据
struct ThreadData {
    std::vector<uint8_t> src;
    std::vector<uint8_t> dst;
    bool data_ok;
    bool consistent;
};

// 线程函数：等待信号量，执行复制，验证
static void worker_thread(ThreadData *data, sem_t *sem) {
    sem_wait(sem);   // 等待开始信号

    // 执行内存复制
    neon_copy(data->dst.data(), data->src.data(), BLOCK_SIZE);

    // 验证数据是否一致
    data->data_ok = (memcmp(data->dst.data(), data->src.data(), BLOCK_SIZE) == 0);

    // 一致性测试：存储到临时缓冲区再加载比较
    std::vector<uint8_t> store_buf(BLOCK_SIZE);
    neon_copy(store_buf.data(), data->dst.data(), BLOCK_SIZE);
    data->consistent = (memcmp(store_buf.data(), data->dst.data(), BLOCK_SIZE) == 0);
}

static int memcpy_sem_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int memcpy_sem_run(struct test *test, int cpu) {
    (void)cpu;

    // 随机数生成器
    auto dist = []() { return (uint8_t)((0) + (int64_t)(random64() % (uint64_t)((255) - (0) + 1))); };

    // 信号量：用于同步所有线程同时开始
    sem_t sem;
    sem_init(&sem, 0, 0);   // 初始值为 0

    // 线程数据数组
    std::vector<ThreadData> thread_data(NUM_THREADS);
    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);

    static std::atomic<uint64_t> iter{0};

    do {
        // 1. 为每个线程准备随机数据
        for (int i = 0; i < NUM_THREADS; ++i) {
            thread_data[i].src.resize(BLOCK_SIZE);
            thread_data[i].dst.resize(BLOCK_SIZE);
            for (size_t j = 0; j < BLOCK_SIZE; ++j) {
                thread_data[i].src[j] = dist();
            }
            // 目标缓冲区初始化为随机值（避免未初始化干扰）
            for (size_t j = 0; j < BLOCK_SIZE; ++j) {
                thread_data[i].dst[j] = dist();
            }
        }

        // 2. 创建线程并等待信号
        for (int i = 0; i < NUM_THREADS; ++i) {
            threads.emplace_back(worker_thread, &thread_data[i], &sem);
        }

        // 3. 释放信号，让所有线程同时开始复制
        for (int i = 0; i < NUM_THREADS; ++i) {
            sem_post(&sem);
        }

        // 4. 等待所有线程完成
        for (auto &t : threads) {
            t.join();
        }
        threads.clear();   // 清空以便下一轮复用

        // 5. 检查所有线程的结果
        bool all_passed = true;
        for (int i = 0; i < NUM_THREADS; ++i) {
            if (!thread_data[i].data_ok || !thread_data[i].consistent) {
                all_passed = false;
                break;
            }
        }

        uint64_t iteration = iter.fetch_add(1, std::memory_order_relaxed);
        const char *color = all_passed ? "\033[32m" : "\033[31m";
        const char *result_str = all_passed ? "PASS" : "FAIL";

        // 输出第一个线程的输入摘要（代表整体）
        fprintf(stderr, "memcpy_sem_neon: Iter %lu, src[0..7]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                iteration,
                thread_data[0].src[0], thread_data[0].src[1],
                thread_data[0].src[2], thread_data[0].src[3],
                thread_data[0].src[4], thread_data[0].src[5],
                thread_data[0].src[6], thread_data[0].src[7]);
        fprintf(stderr, "  all_passed=%d, result=%s%s\033[0m\n",
                all_passed, color, result_str);
        fflush(stderr);

        if (!all_passed) {
            report_fail_msg("memcpy_sem_neon: data mismatch or consistency failure in one or more threads");
            sem_destroy(&sem);
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    sem_destroy(&sem);
    return EXIT_SUCCESS;
}

static int memcpy_sem_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(memcpy_sem_neon, "Concurrent memory copy with semaphore synchronization")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = memcpy_sem_init,
    .test_run = memcpy_sem_run,
    .test_cleanup = memcpy_sem_finish,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
