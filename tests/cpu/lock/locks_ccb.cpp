#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#include <unistd.h>
#include <new>   // for placement new

#define MAX_THREADS 128
#define CACHE_LINE_SIZE 64

// 使用 std::atomic_flag 实现自旋锁（可放置于任意地址，包括跨缓存行）
struct SpinLock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;

    void lock() {
        while (flag.test_and_set(std::memory_order_acquire)) {
            // 自旋等待（可添加 yield，非必需）
        }
    }

    void unlock() {
        flag.clear(std::memory_order_release);
    }
};

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    uint8_t *base;                 // 分配的内存基址
    SpinLock *lock;                // 指向跨缓存行的锁对象的指针
    alignas(64) uint64_t counter; // 共享计数器，单独缓存行
    std::atomic<uint64_t> thread_idx;
    uint64_t local_sums[MAX_THREADS];
};
}

static int locks_ccb_init(struct test *test) {
    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;

    // 分配足够大的内存（256 字节）
    td->base = new uint8_t[256];

    // 寻找一个地址，使得锁对象（至少1字节）跨越缓存行边界
    uintptr_t addr = reinterpret_cast<uintptr_t>(td->base);
    int offset = 0;
    for (int i = 0; i < 128; ++i) {
        if (( (addr + i) % CACHE_LINE_SIZE ) >= (CACHE_LINE_SIZE - 1)) {
            offset = i;
            break;
        }
    }
    // 若未找到，强制使用偏移 63（使得地址在缓存行最后1字节）
    if (offset == 0 && ((addr % CACHE_LINE_SIZE) < 63)) {
        offset = 63;
    }
    // 在跨缓存行地址处 placement new 构造 SpinLock 对象
    td->lock = new (td->base + offset) SpinLock();
    // 初始化锁（无需额外操作，构造时已初始化 flag）

    td->counter = 0;
    td->thread_idx.store(0, std::memory_order_relaxed);
    memset(td->local_sums, 0, sizeof(td->local_sums));
    test->data = td;

    // 可选：打印锁地址和缓存行偏移，便于调试
    // fprintf(stderr, "Lock at %p, offset within cache line: %lu\n", td->lock, (uintptr_t)td->lock % 64);

    return EXIT_SUCCESS;
}

static int locks_ccb_run(struct test *test, int cpu) {
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
        uint64_t inc = dist();   // 本次的输入

        td->lock->lock();
        td->counter += inc;
        td->lock->unlock();

        local_sum += inc;

    } while (test_time_condition(test));

    td->local_sums[id] = local_sum;
    return EXIT_SUCCESS;
}

static int locks_ccb_finish(struct test *test) {
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

    fprintf(stderr, "locks_ccb: Shared counter=%lu, Total local sums=%lu, Threads=%lu, data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
            counter_value, total_local, thread_count, data_ok, consistent, color, result_str);
    fflush(stderr);

    if (!passed) {
        report_fail_msg("locks_ccb: Counter mismatch or consistency failure");
        return EXIT_FAILURE;
    }

    // 手动调用析构函数，释放 placement new 构造的对象
    td->lock->~SpinLock();
    delete[] td->base;
    delete td;
    return EXIT_SUCCESS;
}

DECLARE_TEST(locks_ccb, "Testing locks - read, modify, write locked data. Locked data crosses cache line bdry")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = locks_ccb_init,
    .test_run = locks_ccb_run,
    .test_cleanup = locks_ccb_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
