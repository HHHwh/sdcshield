/*  spinlock_stress_cmpxchg16b_neon_fp64.cpp — FP64 data-path variant of tests/cpu/lock_arm/spinlock_stress_cmpxchg16b_neon.cpp
 *  (2026-11-10; parent untouched).  The lock-free / wide-CAS pack protocol
 *  (counter+version double-field packing, unpack/pack arithmetic, the CAS
 *  itself) is EXACTLY the parent's — bit-level, the test's subject.
 *  What is re-typed: the per-thread vector slot payload RMW runs
 *  vaddq_f64 instead of vaddq_u32 (FP pipe / FP register file, asm
 *  ld_vec/st_vec preserved).  Every parent check is kept: packed-counter
 *  vs total local sums (parent's mod-2^32 rule preserved), store/reload
 *  consistency, slot-vs-local_sum verify (bit-exact veorq_u64 on the f64
 *  bits; integer sums stay far below the 2^53 exact domain).
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

// 16 字节对齐的结构体，使用 std::atomic<__int128> 管理
namespace { /* ODR: per-TU SharedData (29-file same-name hazard) */
struct alignas(16) SharedData {
    std::atomic<__int128> value;               // 原子128位整数
    std::atomic<uint64_t> thread_idx;          // 线程 ID 分配器
    uint64_t local_sums[MAX_THREADS];
    double (*vec_slot)[2];   /* per-thread NEON payload slot */          // 每个线程的本地增量总和
};
}

static int spinlock_stress_cmpxchg16b_init(struct test *test) {
    auto *sd = new SharedData;
    if (!sd) return EXIT_FAILURE;
    sd->value = 0;
    sd->thread_idx.store(0, std::memory_order_relaxed);
    memset(sd->local_sums, 0, sizeof(sd->local_sums));
    sd->vec_slot = new double[MAX_THREADS][2];
    memset(sd->vec_slot, 0, MAX_THREADS * 16);
    test->data = sd;
    return EXIT_SUCCESS;
}

static int spinlock_stress_cmpxchg16b_run(struct test *test, int cpu) {
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

        bool success = false;
        while (!success) {
            __int128 old_val = sd->value.load(std::memory_order_acquire);
            uint64_t old_counter = (uint64_t)old_val;
            uint64_t old_pad = (uint64_t)(old_val >> 64);
            uint64_t new_counter = old_counter + inc;
            __int128 new_val = ((__int128)old_pad << 64) | new_counter;
            // 使用 compare_exchange_weak（等价于 cmpxchg16b）
            success = sd->value.compare_exchange_weak(old_val, new_val,
                                                      std::memory_order_acq_rel,
                                                      std::memory_order_acquire);
            if (!success) {
                // 自旋等待，加入 yield 提示
                #if defined(__aarch64__)
                    __asm__ volatile("yield");
                #elif defined(__x86_64__)
                    __asm__ volatile("pause");
                #else
                    /* no arch-specific pause hint; fall through */
                #endif
            }
        }

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
}

static int spinlock_stress_cmpxchg16b_finish(struct test *test) {
    auto *sd = static_cast<SharedData*>(test->data);

    uint64_t total_local = 0;
    uint64_t thread_count = 0;
    for (int i = 0; i < MAX_THREADS; ++i) {
        if (sd->local_sums[i] != 0) {
            total_local += sd->local_sums[i];
            thread_count++;
        }
    }

    __int128 final_val = sd->value.load(std::memory_order_acquire);
    uint64_t final_counter = (uint64_t)final_val;

    // 一致性测试：存储到内存再加载比较
    __int128 store_buf = final_val;
    __int128 reload_buf;
    memcpy(&reload_buf, &store_buf, sizeof(store_buf));
    bool consistent = (reload_buf == final_val);

    bool data_ok = (final_counter == total_local);
    bool passed = data_ok && consistent;

    const char *color = passed ? "\033[32m" : "\033[31m";
    const char *result_str = passed ? "PASS" : "FAIL";

    fprintf(stderr, "spinlock_stress_cmpxchg16b_neon_fp64: Final counter=%lu, Total local sums=%lu, Threads=%lu, data_ok=%d, consistent=%d, result=%s%s\033[0m\n",
            final_counter, total_local, thread_count, data_ok, consistent, color, result_str);
    fflush(stderr);

    if (!passed) {
        report_fail_msg("spinlock_stress_cmpxchg16b_neon_fp64: Counter mismatch or consistency failure");
        return EXIT_FAILURE;
    }

    /* NEON slot check: slot == low 32 bits of local_sum */
    for (uint64_t i = 0; i < thread_count; ++i) {
        double want = (double)sd->local_sums[i];
        float64x2_t got = la_ld_vec(sd->vec_slot[i]);
        uint64x2_t diff = veorq_u64(vreinterpretq_u64_f64(got), vreinterpretq_u64_f64(vdupq_n_f64(want)));
        if (vmaxvq_u32(vreinterpretq_u32_u64(diff)) != 0) {
            report_fail_msg("spinlock_stress_cmpxchg16b_neon_fp64: vector slot %lu mismatch: got "
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

DECLARE_TEST(spinlock_stress_cmpxchg16b_neon_fp64, "FP64 variant: Stress test LOCK CMPXCHG16B (ARM64 atomic 128-bit) — data path FP64 (slot vaddq_f64; the packed counter/version stays the parent's integer bit-level protocol), lock/CAS protocol untouched")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = spinlock_stress_cmpxchg16b_init,
    .test_run = spinlock_stress_cmpxchg16b_run,
    .test_cleanup = spinlock_stress_cmpxchg16b_finish,
    .fracture_loop_count = -1,   /* never fracture (lock_arm convention) */
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
