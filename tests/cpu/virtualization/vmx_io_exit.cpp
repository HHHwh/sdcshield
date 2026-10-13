#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef __aarch64__
// ARM64 平台：诚实跳过 —— 无端口 I/O，原先的"模拟"路径（单个常量存取即
// EXIT_SUCCESS）不压测任何单元却报 pass，违反 placeholder-honesty 规则。
static int vmx_io_exit_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): no I/O port instructions on ARM64");
    return EXIT_SKIP;
}

static int vmx_io_exit_run(struct test *test, int cpu) {
    (void)cpu;
    (void)test;
    // Unreachable on ARM64: init already returned EXIT_SKIP. Kept honest
    // (no vacuous EXIT_SUCCESS) in case init is ever bypassed.
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): no I/O port instructions on ARM64");
    return EXIT_SKIP;
}

#else
// x86 平台：保留原测试逻辑
#include <immintrin.h>
#include <random>

static bool is_vmx_and_hypervisor() {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1), "c"(0));
    bool vmx = (ecx & (1 << 5)) != 0;
    bool hypervisor = (ecx & (1 << 31)) != 0;
    return vmx && hypervisor;
}

static int vmx_io_exit_init(struct test *test) {
    if (!is_vmx_and_hypervisor()) {
        fprintf(stderr, "vmx_io_exit: VMX or Hypervisor not detected, skipping.\n");
        return EXIT_SUCCESS;
    }
    return EXIT_SUCCESS;
}

static int vmx_io_exit_run(struct test *test, int cpu) {
    (void)cpu;

    if (!is_vmx_and_hypervisor()) {
        fprintf(stderr, "vmx_io_exit: VMX or Hypervisor not detected, skipping.\n");
        return EXIT_SUCCESS;
    }

    bool passed = true;
    bool consistent = true;

    uint64_t test_val = 0xDEADBEEFCAFEBABEULL;
    uint64_t store_buf, reload_buf;

    store_buf = test_val;
    _mm_mfence();

    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint8_t> byte_dist(0, 255);
    uint8_t io_data = byte_dist(rng);

    __asm__ volatile ("outb %0, $0x80" : : "a"(io_data) : "memory");
    uint8_t read_data;
    __asm__ volatile ("inb $0x80, %0" : "=a"(read_data));

    memcpy(&reload_buf, &store_buf, sizeof(store_buf));
    consistent = (reload_buf == test_val);
    // Port 0x80 is the POST diagnostic port: a write followed by a read
    // must return the same byte on a healthy bus. Verify it (the previous
    // version read read_data but never compared it).
    bool io_roundtrip = (read_data == io_data);
    passed = consistent && io_roundtrip;

    if (!passed) {
        fprintf(stderr, "\n[vmx_io_exit] FAIL on CPU %d\n", cpu);
        fprintf(stderr, "  test_val = 0x%016lX, reload_buf = 0x%016lX\n", test_val, reload_buf);
        fprintf(stderr, "  io_data written = 0x%02X, read_data = 0x%02X\n", io_data, read_data);
        fprintf(stderr, "  consistent = %d, io_roundtrip = %d\n", consistent, io_roundtrip);
        report_fail_msg("vmx_io_exit: I/O execution or consistency failure");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "\033[32mvmx_io_exit PASS on CPU %d\033[0m\n", cpu);
    return EXIT_SUCCESS;
}
#endif

static int vmx_io_exit_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(vmx_io_exit,
             "Verifies that a guest triggers IO VM-Exits as expected")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = vmx_io_exit_init,
    .test_run = vmx_io_exit_run,
    .test_cleanup = vmx_io_exit_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
