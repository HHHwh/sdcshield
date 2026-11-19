/*  lockless_cmpxchg16b_neon — NEON variant of tests/cpu/lock/lockless_cmpxchg16b_neon.cpp (lock_arm pass,
 *  2026-11-03; original untouched).
 *
 *  The lock-free / wide-CAS protocol is EXACTLY the original's (it is
 *  the test's subject).  ADDED alongside the atomic update: a
 *  per-thread uint32x4 vector slot RMW'd through asm ld_vec/st_vec
 *  (intrinsics get GCC-forwarded) — the NEON datapath payload raises
 *  switching power while every original check stays verbatim.  New
 *  check: the slot must equal the low 32 bits of the thread's own
 *  local_sum (mod-2^32 arithmetic is closed on both sides).
 */
#include <sandstone.h>
#include <arm_neon.h>
/* asm vector IO — intrinsics get GCC store-transformed / load-forwarded */
static inline uint32x4_t la_ld_vec(const uint32_t *a) {
    uint32x4_t v;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(v) : "r"(a) : "memory");
    return v;
}
static inline void la_st_vec(uint32_t *a, uint32x4_t v) {
    __asm__ volatile("str %q0, [%1]" :: "w"(v), "r"(a) : "memory");
}

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#include <unistd.h>

#define MAX_THREADS 128

// 使用 unsigned __int128 表示 16 字节原子数据
// 低 64 位：counter，高 64 位：version
namespace { /* ODR: per-TU SharedData (29-file same-name hazard) */
struct SharedData {
    alignas(16) std::atomic<unsigned __int128> data;  // 16 字节原子对象
    std::atomic<uint64_t> thread_idx;                 // 线程 ID 分配器
    uint64_t local_sums[MAX_THREADS];
    uint32_t (*vec_slot)[4];   /* per-thread NEON payload slot */                 // 每个线程的本地增量总和
};
}

// 辅助函数：从 128 位中提取 counter 和 version
static inline void unpack_128(unsigned __int128 val, uint64_t &counter, uint64_t &version) {
    counter = static_cast<uint64_t>(val);
    version = static_cast<uint64_t>(val >> 64);
}

// 辅助函数：打包 counter 和 version 为 128 位
static inline unsigned __int128 pack_128(uint64_t counter, uint64_t version) {
    return (static_cast<unsigned __int128>(version) << 64) | counter;
}

static int lockless_cmpxchg16b_init(struct test *test) {
    auto *sd = new SharedData;
    if (!sd) return EXIT_FAILURE;
    sd->data.store(0, std::memory_order_relaxed);
    sd->thread_idx.store(0, std::memory_order_relaxed);
    memset(sd->local_sums, 0, sizeof(sd->local_sums));
    sd->vec_slot = new uint32_t[MAX_THREADS][4];
    memset(sd->vec_slot, 0, MAX_THREADS * 16);
    test->data = sd;
    return EXIT_SUCCESS;
}

static int lockless_cmpxchg16b_run(struct test *test, int cpu) {
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
        uint64_t inc = dist();   // 本次的输入

        // CAS 循环更新 16 字节数据
        uint64_t old_counter, old_version, new_counter, new_version;
        unsigned __int128 old_val, new_val;
        do {
            old_val = sd->data.load(std::memory_order_acquire);
            unpack_128(old_val, old_counter, old_version);
            new_counter = old_counter + inc;
            new_version = old_version + 1;   // 每次更新增加版本号
            new_val = pack_128(new_counter, new_version);
        } while (!sd->data.compare_exchange_strong(old_val, new_val,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire));

        local_sum += inc;
        /* NEON payload alongside the atomic update: per-thread vector RMW */
        {
            uint32_t *slot = sd->vec_slot[id];
            uint32x4_t v = la_ld_vec(slot);
            v = vaddq_u32(v, vdupq_n_u32((uint32_t)inc));
            la_st_vec(slot, v);
        }

    } while (test_time_condition(test));

    sd->local_sums[id] = local_sum;
    return EXIT_SUCCESS;
}

static int lockless_cmpxchg16b_finish(struct test *test) {
    auto *sd = static_cast<SharedData*>(test->data);

    uint64_t total_local = 0;
    uint64_t thread_count = sd->thread_idx.load(std::memory_order_relaxed);
    for (uint64_t i = 0; i < thread_count; ++i) {
        total_local += sd->local_sums[i];
    }

    // 读取最终计数器值
    unsigned __int128 final_val = sd->data.load(std::memory_order_acquire);
    uint64_t counter_value;
    unpack_128(final_val, counter_value, *((uint64_t*)&final_val + 1)); // 只需 counter

    // 一致性测试：存储计数器值到内存，再加载比较
    uint64_t store_buf = counter_value;
    uint64_t reload_buf;
    memcpy(&reload_buf, &store_buf, sizeof(store_buf));
    bool consistent = (reload_buf == counter_value);

    bool data_ok = (counter_value == total_local);
    bool passed = data_ok && consistent;

    const char *color = passed ? "\033[32m" : "\033[31m";
    const char *result_str = passed ? "PASS" : "FAIL";

    fprintf(stderr, "lockless_cmpxchg16b_neon: Counter=%lu, Total local sums=%lu, Threads=%lu, data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
            counter_value, total_local, thread_count, data_ok, consistent, color, result_str);
    fflush(stderr);

    if (!passed) {
        report_fail_msg("lockless_cmpxchg16b_neon: Counter mismatch or consistency failure");
        return EXIT_FAILURE;
    }

    /* NEON slot check: slot == low 32 bits of local_sum */
    for (uint64_t i = 0; i < thread_count; ++i) {
        uint32_t want = (uint32_t)sd->local_sums[i];
        uint32x4_t got = la_ld_vec(sd->vec_slot[i]);
        uint32x4_t diff = veorq_u32(got, vdupq_n_u32(want));
        if (vmaxvq_u32(diff) != 0) {
            report_fail_msg("lockless_cmpxchg16b_neon: vector slot %lu mismatch: got "
                            "{%u,%u,%u,%u} want %u (NEON store lost)", i,
                            vgetq_lane_u32(got, 0), vgetq_lane_u32(got, 1),
                            vgetq_lane_u32(got, 2), vgetq_lane_u32(got, 3), want);
            delete[] sd->vec_slot;
            delete sd;
            return EXIT_FAILURE;
        }
    }
    delete[] sd->vec_slot;
    delete sd;
    return EXIT_SUCCESS;
}

DECLARE_TEST(lockless_cmpxchg16b_neon, "Testing lockless cmpxchg16b")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = lockless_cmpxchg16b_init,
    .test_run = lockless_cmpxchg16b_run,
    .test_cleanup = lockless_cmpxchg16b_finish,
    .fracture_loop_count = -1,   /* never fracture (lock_arm convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
