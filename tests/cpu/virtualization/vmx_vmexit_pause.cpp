#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

// 读取 ARM64 虚拟计数器
static inline uint64_t read_cntvct_el0() {
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

static int vmx_vmexit_pause_init(struct test *test) {
#ifdef __aarch64__
    // ARM64 has no VT-x/VMX virtualization — the previous "simulated" path
    // (single constant store+reload, no TEST_LOOP) passed vacuously while
    // stressing nothing. Honest skip per the placeholder-honesty rule.
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): no VT-x/VMX virtualization on ARM64");
    return EXIT_SKIP;
#else
    return EXIT_SUCCESS;
#endif
}

static int vmx_vmexit_pause_run(struct test *test, int cpu) {
#ifdef __aarch64__
    // Unreachable on ARM64: init already returned EXIT_SKIP. Kept honest
    // (no vacuous EXIT_SUCCESS) in case init is ever bypassed.
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): no VT-x/VMX virtualization on ARM64");
    return EXIT_SKIP;
#else

    const int iterations = 100000;  // 执行足够多的 yield 确保时间变化
    uint64_t before, after;
    uint64_t store_buf, reload_buf;
    bool consistent = true;

    before = read_cntvct_el0();

    // 循环执行 yield 指令，确保有一定耗时
    for (int i = 0; i < iterations; ++i) {
        __asm__ volatile("yield");
    }

    after = read_cntvct_el0();

    // 一致性测试：存储 → 加载
    store_buf = after;
    memcpy(&reload_buf, &store_buf, sizeof(store_buf));
    consistent = (reload_buf == after);

    // 判定：执行后计数器应该有增长
    bool passed = (after > before) && consistent;

    if (!passed) {
        fprintf(stderr, "\n[vmx_vmexit_pause] FAIL on CPU %d\n", cpu);
        fprintf(stderr, "  before = 0x%016lX, after = 0x%016lX\n", before, after);
        fprintf(stderr, "  consistent = %d\n", consistent);
        report_fail_msg("vmx_vmexit_pause: YIELD execution or consistency failure");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "\033[32mvmx_vmexit_pause PASS on CPU %d\033[0m\n", cpu);
    return EXIT_SUCCESS;
#endif
}

static int vmx_vmexit_pause_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(vmx_vmexit_pause,
             "Have a guest trigger vmexits by calling YIELD (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = vmx_vmexit_pause_init,
    .test_run = vmx_vmexit_pause_run,
    .test_cleanup = vmx_vmexit_pause_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
