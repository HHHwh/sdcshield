#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#include <unistd.h>

#define MAX_THREADS 128

// 使用 uint64_t 表示 8 字节原子数据
// 低 32 位：counter，高 32 位：version
struct SharedData {
    alignas(8) std::atomic<uint64_t> data;   // 8 字节原子对象
    std::atomic<uint64_t> thread_idx;        // 线程 ID 分配器
    uint64_t local_sums[MAX_THREADS];        // 每个线程的本地增量总和
};

// 辅助函数：从 64 位中提取 counter 和 version
static inline void unpack_64(uint64_t val, uint32_t &counter, uint32_t &version) {
    counter = static_cast<uint32_t>(val);
    version = static_cast<uint32_t>(val >> 32);
}

// 辅助函数：打包 counter 和 version 为 64 位
static inline uint64_t pack_64(uint32_t counter, uint32_t version) {
    return (static_cast<uint64_t>(version) << 32) | counter;
}

static int lockless_cmpxchg8b_init(struct test *test) {
    auto *sd = new SharedData;
    if (!sd) return EXIT_FAILURE;
    sd->data.store(0, std::memory_order_relaxed);
    sd->thread_idx.store(0, std::memory_order_relaxed);
    memset(sd->local_sums, 0, sizeof(sd->local_sums));
    test->data = sd;
    return EXIT_SUCCESS;
}

static int lockless_cmpxchg8b_run(struct test *test, int cpu) {
    (void)cpu;
    auto *sd = static_cast<SharedData*>(test->data);

    int id = sd->thread_idx.fetch_add(1, std::memory_order_relaxed);
    if (id >= MAX_THREADS) {
        report_fail_msg("Too many threads");
        return EXIT_FAILURE;
    }

    /* randomization hardening H14' (P16): framework RNG (per-thread
     * stream, -s reproducible) replaces std::mt19937; range [1, 1000). */
    auto dist = []() { return (1) + random64() % (uint64_t)((1000) - (1) + 1); };
    uint64_t local_sum = 0;

    do {
        uint32_t inc = dist();   // 本次的输入

        // CAS 循环更新 8 字节数据
        uint32_t old_counter, old_version, new_counter, new_version;
        uint64_t old_val, new_val;
        do {
            old_val = sd->data.load(std::memory_order_acquire);
            unpack_64(old_val, old_counter, old_version);
            new_counter = old_counter + inc;
            new_version = old_version + 1;   // 版本号加1
            new_val = pack_64(new_counter, new_version);
        } while (!sd->data.compare_exchange_strong(old_val, new_val,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire));

        local_sum += inc;

    } while (test_time_condition(test));

    sd->local_sums[id] = local_sum;
    return EXIT_SUCCESS;
}

static int lockless_cmpxchg8b_finish(struct test *test) {
    auto *sd = static_cast<SharedData*>(test->data);

    uint64_t total_local = 0;
    uint64_t thread_count = sd->thread_idx.load(std::memory_order_relaxed);
    for (uint64_t i = 0; i < thread_count; ++i) {
        total_local += sd->local_sums[i];
    }

    // 读取最终计数器值（低32位）
    uint64_t final_val = sd->data.load(std::memory_order_acquire);
    uint32_t counter_value = static_cast<uint32_t>(final_val);

    // 一致性测试：存储计数器值到内存，再加载比较
    uint64_t store_buf = counter_value;
    uint64_t reload_buf;
    memcpy(&reload_buf, &store_buf, sizeof(store_buf));
    bool consistent = (reload_buf == counter_value);

    // counter 是 8 字节原子对象里的低 32 位，随 CAS 按 2^32 回绕；
    // total_local 是各线程 64 位本地增量之和，不会回绕。这里必须按
    // 模 2^32 比较，否则 total_local 超过 4G 时必然误报 data_ok=0。
    bool data_ok = (counter_value == static_cast<uint32_t>(total_local));
    bool passed = data_ok && consistent;

    const char *color = passed ? "\033[32m" : "\033[31m";
    const char *result_str = passed ? "PASS" : "FAIL";

    fprintf(stderr, "lockless_cmpxchg8b: Counter=%lu, Total local sums=%lu, Threads=%lu, data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
            (unsigned long)counter_value, (unsigned long)total_local,
            (unsigned long)thread_count, data_ok, consistent, color, result_str);
    fflush(stderr);

    if (!passed) {
        report_fail_msg("lockless_cmpxchg8b: Counter mismatch or consistency failure");
        return EXIT_FAILURE;
    }

    delete sd;
    return EXIT_SUCCESS;
}

DECLARE_TEST(lockless_cmpxchg8b, "Testing lockless cmpxchg8b")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = lockless_cmpxchg8b_init,
    .test_run = lockless_cmpxchg8b_run,
    .test_cleanup = lockless_cmpxchg8b_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
