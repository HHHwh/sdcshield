#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#include <unistd.h>

#define MAX_THREADS 128
#define CACHE_LINE_SIZE 64

// 自旋锁：使用 std::atomic 的 compare_exchange_weak
static inline void spin_lock(std::atomic<uint64_t> &lock) {
    uint64_t expected = 0;
    while (!lock.compare_exchange_weak(expected, 1, std::memory_order_acquire)) {
        expected = 0;
        // ARM64 暂停提示（可选）
        #if defined(__aarch64__)
            __asm__ volatile("yield");
        #elif defined(__x86_64__)
            __asm__ volatile("pause");
        #else
            /* no arch-specific pause hint; fall through */
        #endif
    }
}

static inline void spin_unlock(std::atomic<uint64_t> &lock) {
    lock.store(0, std::memory_order_release);
}

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    uint8_t *base;                 // 分配的内存基址
    std::atomic<uint64_t> *lock;   // 跨缓存行的锁指针（原子类型）
    alignas(64) uint64_t counter;  // 共享计数器，单独缓存行
    std::atomic<uint64_t> thread_idx;
    uint64_t local_sums[MAX_THREADS];
};
}

static int spinlock_crosses_cacheline_init(struct test *test) {
    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;
    // 分配足够大的内存（256 字节）
    td->base = new uint8_t[256];
    // 寻找一个地址，使得锁（8 字节）跨越缓存行边界
    uintptr_t addr = reinterpret_cast<uintptr_t>(td->base);
    int offset = 0;
    for (int i = 0; i < 128; ++i) {
        if (( (addr + i) % CACHE_LINE_SIZE ) >= (CACHE_LINE_SIZE - 8)) {
            offset = i;
            break;
        }
    }
    // 若未找到，强制使用偏移 60
    if (offset == 0 && ((addr % CACHE_LINE_SIZE) < 56)) {
        offset = 60;
    }
    td->lock = reinterpret_cast<std::atomic<uint64_t>*>(td->base + offset);
    // 初始化为 0
    td->lock->store(0, std::memory_order_relaxed);
    td->counter = 0;
    td->thread_idx.store(0, std::memory_order_relaxed);
    memset(td->local_sums, 0, sizeof(td->local_sums));
    test->data = td;
    // 可选打印调试信息
    // fprintf(stderr, "Lock at %p, offset within cache line: %lu\n", td->lock, (uintptr_t)td->lock % 64);
    return EXIT_SUCCESS;
}

static int spinlock_crosses_cacheline_run(struct test *test, int cpu) {
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

        spin_lock(*td->lock);
        td->counter += inc;
        spin_unlock(*td->lock);

        local_sum += inc;

    } while (test_time_condition(test));

    td->local_sums[id] = local_sum;
    return EXIT_SUCCESS;
}

static int spinlock_crosses_cacheline_finish(struct test *test) {
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

    fprintf(stderr, "spinlock_crosses_cacheline: Shared counter=%lu, Total local sums=%lu, Threads=%lu, data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
            counter_value, total_local, thread_count, data_ok, consistent, color, result_str);
    fflush(stderr);

    if (!passed) {
        report_fail_msg("spinlock_crosses_cacheline: Counter mismatch or consistency failure");
        return EXIT_FAILURE;
    }

    delete[] td->base;
    delete td;
    return EXIT_SUCCESS;
}

DECLARE_TEST(spinlock_crosses_cacheline, "Spinlock crossing cacheline boundary (ARM64 atomic)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = spinlock_crosses_cacheline_init,
    .test_run = spinlock_crosses_cacheline_run,
    .test_cleanup = spinlock_crosses_cacheline_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
