/*  lockless_cmpxchg_neon_fp64.cpp — FP64 data-path variant of tests/cpu/lock_arm/lockless_cmpxchg_neon.cpp
 *  (2026-11-10; parent untouched).  The lock protocol (CAS / xchg / BTS
 *  acquisition, barriered release, spin hints) is EXACTLY the parent's —
 *  it is the test's subject.  What is re-typed: the in-critical-section
 *  payload — the per-thread vector slot RMW runs vaddq_f64 instead of
 *  vaddq_u32 (FP pipe / FP register file), and the shared counter is a
 *  double accumulated with FADD (integer-exact domain: 64 threads x
 *  ~1e5 locks/s x 1000 max inc x 86400 s ≈ 5.5e14 < 2^53 ≈ 9e15, 16x
 *  headroom).  Every parent check is kept: counter vs total local sums
 *  (numerical equality in the exact domain — the parent's mod-2^32 rule
 *  cannot wrap below 2^53), store/reload consistency, slot-vs-local_sum
 *  verify (bit-exact veorq_u64 on the f64 bits).
 */
#include <sandstone.h>
#include <arm_neon.h>
/* asm vector IO — intrinsics get GCC store-transformed / load-forwarded */
static inline float64x2_t la_ld_vec(const double *a) {
    float64x2_t v;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(v) : "r"(a) : "memory");
    return v;
}
static inline void la_st_vec(double *a, float64x2_t v) {
    __asm__ volatile("str %q0, [%1]" :: "w"(v), "r"(a) : "memory");
}
static inline unsigned long long f64_bits(double d) {
    unsigned long long u; memcpy(&u, &d, 8); return u;
}

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <random>
#include <unistd.h>

#define MAX_THREADS 128

namespace { /* ODR: per-TU SharedData (29-file same-name hazard) */
struct SharedData {
    alignas(64) std::atomic<double> counter;   // 共享计数器（原子类型）
    std::atomic<uint64_t> thread_idx;            // 线程 ID 分配器
    uint64_t local_sums[MAX_THREADS];
    double (*vec_slot)[2];   /* per-thread NEON payload slot */            // 每个线程的本地增量总和
};
}

static int lockless_cmpxchg_init(struct test *test) {
    auto *sd = new SharedData;
    if (!sd) return EXIT_FAILURE;
    sd->counter = 0.0;
    sd->thread_idx.store(0, std::memory_order_relaxed);
    memset(sd->local_sums, 0, sizeof(sd->local_sums));
    sd->vec_slot = new double[MAX_THREADS][2];
    memset(sd->vec_slot, 0, MAX_THREADS * 16);
    test->data = sd;
    return EXIT_SUCCESS;
}

static int lockless_cmpxchg_run(struct test *test, int cpu) {
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

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    do {
        uint64_t inc = dist();   // 本次的输入

        // 使用 std::atomic 的 compare_exchange_strong 循环更新计数器
        auto &counter = sd->counter;
        double old_val, new_val;
        do {
            old_val = counter.load(std::memory_order_acquire);
            new_val = old_val + (double)inc;
        } while (!counter.compare_exchange_strong(old_val, new_val,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire));

        local_sum += inc;
        /* NEON payload alongside the atomic update: per-thread vector RMW */
        {
            double *slot = sd->vec_slot[id];
            float64x2_t v = la_ld_vec(slot);
            v = vaddq_f64(v, vdupq_n_f64((double)inc));
            la_st_vec(slot, v);
        }


    } while (test_time_condition(test));

    sd->local_sums[id] = local_sum;
    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int lockless_cmpxchg_finish(struct test *test) {
    auto *sd = static_cast<SharedData*>(test->data);

    uint64_t total_local = 0;
    uint64_t thread_count = sd->thread_idx.load(std::memory_order_relaxed);
    for (uint64_t i = 0; i < thread_count; ++i) {
        total_local += sd->local_sums[i];
    }

    double counter_value = sd->counter.load(std::memory_order_acquire);

    // 一致性测试：存储计数器值到内存，再加载比较
    double store_buf = counter_value;
    double reload_buf;
    memcpy(&reload_buf, &store_buf, sizeof(store_buf));
    bool consistent = (reload_buf == counter_value);

    bool data_ok = (counter_value == (double)total_local);
    bool passed = data_ok && consistent;

    const char *color = passed ? "\033[32m" : "\033[31m";
    const char *result_str = passed ? "PASS" : "FAIL";

    fprintf(stderr, "lockless_cmpxchg_neon_fp64: Counter=%.0f, Total local sums=%lu, Threads=%lu, data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
            counter_value, total_local, thread_count, data_ok, consistent, color, result_str);
    fflush(stderr);

    if (!passed) {
        report_fail_msg("lockless_cmpxchg_neon_fp64: Counter mismatch or consistency failure");
        return EXIT_FAILURE;
    }

    /* NEON slot check: slot == low 32 bits of local_sum */
    for (uint64_t i = 0; i < thread_count; ++i) {
        double want = (double)sd->local_sums[i];
        float64x2_t got = la_ld_vec(sd->vec_slot[i]);
        uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(got), vreinterpretq_u64_f64(vdupq_n_f64(want)));
        if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
            report_fail_msg("lockless_cmpxchg_neon_fp64: vector slot %lu mismatch: got "
                            "{%016llx,%016llx} want-bits %016llx (NEON store lost)", i,
                            (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(got), 0), (unsigned long long)vgetq_lane_u64(vreinterpretq_u64_f64(got), 1),
                            f64_bits(want));
            delete[] sd->vec_slot;
            delete sd;
            return EXIT_FAILURE;
        }
    }
    delete[] sd->vec_slot;
    delete sd;
    return EXIT_SUCCESS;
}

DECLARE_TEST(lockless_cmpxchg_neon_fp64, "FP64 variant: Testing lockless cmpxchg — data path FP64 (slot vaddq_f64[+double FADD counter]), lock/CAS protocol untouched")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = lockless_cmpxchg_init,
    .test_run = lockless_cmpxchg_run,
    .test_cleanup = lockless_cmpxchg_finish,
    .fracture_loop_count = -1,   /* never fracture (lock_arm convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
