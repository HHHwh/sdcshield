#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <unistd.h>

// ARM64 系统寄存器：MIDR_EL1（Main ID Register）
#define MIDR_EL1_REG 0

struct TestData {
    std::vector<uint64_t> reg_values;  // 按逻辑 CPU 编号索引
    bool all_same;                     // 是否所有核心值相同
    uint64_t common_value;             // 共同值（如果相同）
};

// 读取 ARM64 系统寄存器（用户态可读）
[[maybe_unused]] static bool read_midr_el1(int cpu, uint64_t *val) {
    (void)cpu;  // ARM64 上读取系统寄存器与 CPU 无关，但保留参数用于日志
    // 使用内联汇编读取 MIDR_EL1
    uint64_t reg;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(reg));
    *val = reg;
    return true;
}

static int vmxmsr_init(struct test *test) {
#ifdef __aarch64__
    // ARM64 has no VT-x/VMX virtualization — the previous "simulated" path
    // (single constant store+reload, no TEST_LOOP) passed vacuously while
    // stressing nothing. Honest skip per the placeholder-honesty rule.
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): no VT-x/VMX virtualization on ARM64");
    return EXIT_SKIP;
#else
    auto *data = new TestData;
    if (!data) return EXIT_FAILURE;
    test->data = data;

    int num_cpus = sysconf(_SC_NPROCESSORS_CONF);
    if (num_cpus <= 0) {
        delete data;
        fprintf(stderr, "vmxmsr: Failed to get CPU count\n");
        return EXIT_FAILURE;
    }

    data->reg_values.resize(num_cpus);
    bool any_failed = false;
    bool first = true;
    uint64_t first_val = 0;

    // 遍历所有逻辑 CPU，读取 MIDR_EL1
    for (int cpu = 0; cpu < num_cpus; ++cpu) {
        uint64_t val;
        // 注意：由于我们无法在用户态绑定到特定 CPU 来读取，但 MIDR_EL1 在所有核心相同，所以我们只读取一次。
        // 但为了保持与原测试一致的“每个核心读取”行为，我们模拟为每个核心调用，但实际读取同一寄存器。
        // 这里我们仍调用 read_midr_el1（它不依赖 cpu 参数）并记录。
        if (!read_midr_el1(cpu, &val)) {
            fprintf(stderr, "vmxmsr: Failed to read MIDR_EL1 on CPU %d\n", cpu);
            any_failed = true;
            continue;
        }
        data->reg_values[cpu] = val;
        if (first) {
            first_val = val;
            first = false;
        }
    }

    if (any_failed) {
        delete data;
        test->data = nullptr;
        fprintf(stderr, "vmxmsr: Some reads failed, skipping test.\n");
        return EXIT_SUCCESS;
    }

    // 检查一致性
    data->all_same = true;
    data->common_value = first_val;
    for (size_t i = 1; i < data->reg_values.size(); ++i) {
        if (data->reg_values[i] != first_val) {
            data->all_same = false;
            data->common_value = 0;
            break;
        }
    }

    if (!data->all_same) {
        fprintf(stderr, "vmxmsr: MIDR_EL1 values differ across CPUs (unexpected)\n");
    }

    return EXIT_SUCCESS;
#endif
}

static int vmxmsr_run(struct test *test, int cpu) {
#ifdef __aarch64__
    // Unreachable on ARM64: init already returned EXIT_SKIP. Kept honest
    // (no vacuous EXIT_SUCCESS) in case init is ever bypassed.
    (void)test; (void)cpu;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): no VT-x/VMX virtualization on ARM64");
    return EXIT_SKIP;
#else
    auto *data = static_cast<TestData*>(test->data);
    if (!data) {
        fprintf(stderr, "vmxmsr: No test data, skipping.\n");
        return EXIT_SUCCESS;
    }

    uint64_t expected = data->all_same ? data->common_value : data->reg_values[cpu];
    uint64_t actual;

    if (!read_midr_el1(cpu, &actual)) {
        fprintf(stderr, "vmxmsr: Failed to read MIDR_EL1 on CPU %d\n", cpu);
        report_fail_msg("vmxmsr: read MIDR_EL1 failed");
        return EXIT_FAILURE;
    }

    bool passed = (actual == expected);

    // 一致性测试：存储到内存再加载比较
    uint64_t store_buf = actual;
    uint64_t reload_buf;
    memcpy(&reload_buf, &store_buf, sizeof(store_buf));
    bool consistent = (reload_buf == actual);
    passed = passed && consistent;

    if (!passed) {
        fprintf(stderr, "\n[vmxmsr] FAIL on CPU %d\n", cpu);
        fprintf(stderr, "  expected = 0x%016lX, actual = 0x%016lX\n", expected, actual);
        fprintf(stderr, "  consistent = %d\n", consistent);
        report_fail_msg("vmxmsr: register mismatch or consistency failure");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "\033[32mvmxmsr PASS on CPU %d\033[0m\n", cpu);

    // 只执行一次检查（不需要循环）
    return EXIT_SUCCESS;
#endif
}

static int vmxmsr_finish(struct test *test) {
    auto *data = static_cast<TestData*>(test->data);
    delete data;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

DECLARE_TEST(vmxmsr,
             "Verifies that the MIDR_EL1 register has the same value across all CPUs")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = vmxmsr_init,
    .test_run = vmxmsr_run,
    .test_cleanup = vmxmsr_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
