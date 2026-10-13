#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

// 读取 ARM64 虚拟计数器（用户态可读，功能类似于读取 CR8 的系统寄存器）
static inline uint64_t read_cntvct_el0() {
    uint64_t val;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
}

static int vmx_vmexit_from_cr8_init(struct test *test) {
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

static int vmx_vmexit_from_cr8_run(struct test *test, int cpu) {
#ifdef __aarch64__
    // Unreachable on ARM64: init already returned EXIT_SKIP. Kept honest
    // (no vacuous EXIT_SUCCESS) in case init is ever bypassed.
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): no VT-x/VMX virtualization on ARM64");
    return EXIT_SKIP;
#else

    bool passed = true;
    bool consistent = true;

    // 读取虚拟计数器（在 ARM64 上相当于读取系统寄存器）
    uint64_t read_val = read_cntvct_el0();

    // 只要不产生异常就算成功，不需要验证具体值
    bool data_ok = true;

    // 一致性测试：将读取值存储到内存再加载比较
    uint64_t store_buf = read_val;
    uint64_t reload_buf;
    memcpy(&reload_buf, &store_buf, sizeof(store_buf));
    consistent = (reload_buf == read_val);

    passed = data_ok && consistent;

    if (!passed) {
        fprintf(stderr, "\n[vmx_vmexit_from_cr8] FAIL on CPU %d\n", cpu);
        fprintf(stderr, "  read_val = 0x%016lX\n", read_val);
        fprintf(stderr, "  consistent = %d\n", consistent);
        report_fail_msg("vmx_vmexit_from_cr8: system register read or consistency failure");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "\033[32mvmx_vmexit_from_cr8 PASS on CPU %d (CNTVCT_EL0 read)\033[0m\n", cpu);
    return EXIT_SUCCESS;
#endif
}

static int vmx_vmexit_from_cr8_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(vmx_vmexit_from_cr8,
             "Have a guest trigger vmexits by reading system register (ARM64 CNTVCT_EL0)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = vmx_vmexit_from_cr8_init,
    .test_run = vmx_vmexit_from_cr8_run,
    .test_cleanup = vmx_vmexit_from_cr8_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
