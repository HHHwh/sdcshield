/*  spinlock_rmw_cmpxchg_neon — NEON variant of tests/cpu/spinlock/spinlock_rmw_cmpxchg_neon.cpp (lock_arm pass,
 *  2026-11-03; original untouched).
 *
 *  The lock protocol (its CAS / ldaxr-stxr / xchg acquisition and the
 *  barriered release) is EXACTLY the original's — it is the test's
 *  subject and has no NEON equivalent.  What is ADDED under the same
 *  lock: a per-thread uint32x4 vector slot, read-modified-written
 *  through asm ld_vec/st_vec (intrinsics get GCC-forwarded) — the
 *  NEON datapath payload raises in-critical-section switching power
 *  (the arm_add excitation lineage) while every original check
 *  (shared counter vs total local sums) is kept verbatim.  New check:
 *  each thread's vector slot must equal the low 32 bits of its own
 *  local_sum (mod-2^32 arithmetic is closed, so wraparound is
 *  consistent on both sides).
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

namespace { /* ODR: per-TU SharedData (29-file same-name hazard) */
struct SharedData {
    alignas(64) std::atomic<uint64_t> lock;   // 自旋锁变量（原子类型）
    alignas(64) uint64_t counter;             // 共享计数器
    std::atomic<uint64_t> thread_idx;         // 线程 ID 分配器
    uint64_t local_sums[MAX_THREADS];         // 每个线程的本地增量总和
    uint32_t (*vec_slot)[4];                  // per-thread NEON payload slot
};
}

static int spinlock_rmw_cmpxchg_init(struct test *test) {
    auto *sd = new SharedData;
    if (!sd) return EXIT_FAILURE;
    sd->lock = 0;
    sd->counter = 0;
    sd->thread_idx.store(0, std::memory_order_relaxed);
    memset(sd->local_sums, 0, sizeof(sd->local_sums));
    sd->vec_slot = new uint32_t[MAX_THREADS][4];
    memset(sd->vec_slot, 0, MAX_THREADS * 16);
    test->data = sd;
    return EXIT_SUCCESS;
}

static int spinlock_rmw_cmpxchg_run(struct test *test, int cpu) {
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
        uint64_t inc = dist();

        spin_lock(sd->lock);
        sd->counter += inc;
        /* NEON payload under the same lock: per-thread vector slot RMW */
        {
            uint32_t *slot = sd->vec_slot[id];
            uint32x4_t v = la_ld_vec(slot);
            v = vaddq_u32(v, vdupq_n_u32((uint32_t)inc));
            la_st_vec(slot, v);
        }
        spin_unlock(sd->lock);

        local_sum += inc;

    } while (test_time_condition(test));

    sd->local_sums[id] = local_sum;
    return EXIT_SUCCESS;
}

static int spinlock_rmw_cmpxchg_finish(struct test *test) {
    auto *sd = static_cast<SharedData*>(test->data);

    uint64_t total_local = 0;
    uint64_t thread_count = sd->thread_idx.load(std::memory_order_relaxed);
    for (uint64_t i = 0; i < thread_count; ++i) {
        total_local += sd->local_sums[i];
    }

    uint64_t counter_value = sd->counter;

    // 一致性测试：存储计数器值到内存，再加载比较
    uint64_t store_buf = counter_value;
    uint64_t reload_buf;
    memcpy(&reload_buf, &store_buf, sizeof(store_buf));
    bool consistent = (reload_buf == counter_value);

    bool data_ok = (counter_value == total_local);
    bool passed = data_ok && consistent;

    const char *color = passed ? "\033[32m" : "\033[31m";
    const char *result_str = passed ? "PASS" : "FAIL";

    fprintf(stderr, "spinlock_rmw_cmpxchg_neon: Shared counter=%lu, Total local sums=%lu, Threads=%lu, data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
            counter_value, total_local, thread_count, data_ok, consistent, color, result_str);
    fflush(stderr);

    if (!passed) {
        report_fail_msg("spinlock_rmw_cmpxchg_neon: Counter mismatch or consistency failure");
        return EXIT_FAILURE;
    }

    /* NEON slot check: slot must equal the low 32 bits of local_sum */
    for (uint64_t i = 0; i < thread_count; ++i) {
        uint32_t want = (uint32_t)sd->local_sums[i];
        uint32x4_t got = la_ld_vec(sd->vec_slot[i]);
        uint32x4_t wantv = vdupq_n_u32(want);
        uint32x4_t diff = veorq_u32(got, wantv);
        if (vmaxvq_u32(diff) != 0) {
            report_fail_msg("spinlock_rmw_cmpxchg_neon: vector slot %lu mismatch: "
                            "got {%u,%u,%u,%u} want %u (NEON store lost "
                            "inside the lock path)", i,
                            vgetq_lane_u32(got, 0), vgetq_lane_u32(got, 1),
                            vgetq_lane_u32(got, 2), vgetq_lane_u32(got, 3),
                            want);
            delete[] sd->vec_slot;
            delete sd;
            return EXIT_FAILURE;
        }
    }

    delete[] sd->vec_slot;
    delete sd;
    return EXIT_SUCCESS;
}

DECLARE_TEST(spinlock_rmw_cmpxchg_neon, "Spinlock with CMPXCHG (RMW) test (ARM64 atomic)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = spinlock_rmw_cmpxchg_init,
    .test_run = spinlock_rmw_cmpxchg_run,
    .test_cleanup = spinlock_rmw_cmpxchg_finish,
    .fracture_loop_count = -1,   /* never fracture (lock_arm convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
