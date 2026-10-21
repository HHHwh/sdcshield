#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#include <unistd.h>

#define MAX_THREADS 128

// 自旋锁：使用 GCC 内置原子操作，直接作用于任意地址（支持非对齐）
static inline void spin_lock(uint64_t *lock) {
    uint64_t expected = 0;
    uint64_t desired = 1;
    while (!__atomic_compare_exchange_n(lock, &expected, desired,
                                        false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        expected = 0;
        // ARM64 暂停提示
        #if defined(__aarch64__)
            __asm__ volatile("yield");
        #elif defined(__x86_64__)
            __asm__ volatile("pause");
        #else
            /* no arch-specific pause hint; fall through */
        #endif
    }
}

static inline void spin_unlock(uint64_t *lock) {
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    uint8_t *base;              // 分配的内存基址
    uint64_t *lock;             // 非对齐锁指针（原子操作直接作用于该地址）
    alignas(64) uint64_t counter;
    std::atomic<uint64_t> thread_idx;
    uint64_t local_sums[MAX_THREADS];
};
}

static int spinlock_unaligned_init(struct test *test) {
    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;
    // 分配 64 字节，确保有足够空间
    td->base = new uint8_t[64];
    // 选择偏移 1 字节的地址作为锁（非 8 字节对齐）
    td->lock = reinterpret_cast<uint64_t*>(td->base + 1);
    // 初始化为 0
    __atomic_store_n(td->lock, 0, __ATOMIC_RELAXED);
    td->counter = 0;
    td->thread_idx.store(0, std::memory_order_relaxed);
    memset(td->local_sums, 0, sizeof(td->local_sums));
    test->data = td;
    return EXIT_SUCCESS;
}

static int spinlock_unaligned_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);
    if (id >= MAX_THREADS) {
        report_fail_msg("Too many threads");
        return EXIT_FAILURE;
    }

    /* randomization hardening H14' (P16): framework RNG (per-thread
     * stream, -s reproducible) replaces std::mt19937; range [1, 1000). */
    auto dist = []() { return (1) + random64() % (uint64_t)((1000) - (1) + 1); };
    uint64_t local_sum = 0;

    do {
        uint64_t inc = dist();

        spin_lock(td->lock);
        td->counter += inc;
        spin_unlock(td->lock);

        local_sum += inc;

    } while (test_time_condition(test));

    td->local_sums[id] = local_sum;
    return EXIT_SUCCESS;
}

static int spinlock_unaligned_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);

    uint64_t total_local = 0;
    uint64_t thread_count = 0;
    for (int i = 0; i < MAX_THREADS; ++i) {
        if (td->local_sums[i] != 0) {
            total_local += td->local_sums[i];
            thread_count++;
        }
    }

    uint64_t counter_value = td->counter;

    // 一致性测试：存储计数器值到内存，再加载比较
    uint64_t store_buf = counter_value;
    uint64_t reload_buf;
    memcpy(&reload_buf, &store_buf, sizeof(store_buf));
    bool consistent = (reload_buf == counter_value);

    bool data_ok = (counter_value == total_local);
    bool passed = data_ok && consistent;

    const char *color = passed ? "\033[32m" : "\033[31m";
    const char *result_str = passed ? "PASS" : "FAIL";

    fprintf(stderr, "spinlock_unaligned: Shared counter=%lu, Total local sums=%lu, Threads=%lu, data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
            counter_value, total_local, thread_count, data_ok, consistent, color, result_str);
    fflush(stderr);

    if (!passed) {
        report_fail_msg("spinlock_unaligned: Counter mismatch or consistency failure");
        return EXIT_FAILURE;
    }

    delete[] td->base;
    delete td;
    return EXIT_SUCCESS;
}

DECLARE_TEST(spinlock_unaligned, "Spinlock on unaligned memory address (ARM64 atomic builtins)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = spinlock_unaligned_init,
    .test_run = spinlock_unaligned_run,
    .test_cleanup = spinlock_unaligned_finish,
    .minimum_cpu = cpu_feature_uscat,   // unaligned single-copy atomic store (ARMv8.4 USCAT); without it the misaligned CAS traps (SIGBUS)
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
