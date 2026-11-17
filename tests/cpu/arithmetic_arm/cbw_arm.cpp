/**
 * @file cbw_arm.cpp
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test cbw_arm
 * @parblock
 * Signed load and sign-extend (LDRSB/LDRSH) combined with an Arm Optimized
 * Routines string operation.  A fixed random table of int8 / int16 values is
 * generated at init and the sign-extended int64 golden values are precomputed.
 * Each run re-issues inline-asm LDRSB/LDRSH against the table and verifies the
 * extension, then copies the source byte buffer through AOR's
 * __memcpy_aarch64 / __memcmp_aarch64 and checks the round trip against the
 * golden copy.  Pattern follows Intel OpenDCDiag's Eigen-based design.
 *
 * Logging follows OpenDCDiag convention: pass path is silent; on mismatch the
 * failing inputs and actual-vs-golden outputs are dumped via log_data() before
 * the thread is marked failed via report_fail_msg().
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#ifdef __cplusplus
extern "C" {
#endif
#include <stringlib.h>   // Arm Optimized Routines 提供的 string 库 (C 头)
#ifdef __cplusplus
}
#endif

static constexpr size_t DATA_SIZE = 256;
static constexpr size_t COPY_SIZE = 64;

struct TestData {
    std::vector<int8_t> byte_data;
    std::vector<int16_t> halfword_data;
    std::vector<int64_t> golden_byte_ext;
    std::vector<int64_t> golden_half_ext;
    std::vector<uint8_t> src_data;   // 用于库函数测试的源数据
    std::vector<uint8_t> dst_data;   // 用于库函数测试的目标数据
    std::vector<uint8_t> golden_copy; // 预计算的复制结果
};

// 初始化：生成固定随机输入，预计算黄金结果。所有线程共享同一份只读数据。
static int cbw_arm_init(struct test *test) {
    auto *data = new TestData;
    if (!data) return EXIT_FAILURE;

    data->byte_data.resize(DATA_SIZE);
    data->halfword_data.resize(DATA_SIZE);
    data->golden_byte_ext.resize(DATA_SIZE);
    data->golden_half_ext.resize(DATA_SIZE);
    data->src_data.resize(DATA_SIZE);
    data->dst_data.resize(DATA_SIZE);
    data->golden_copy.resize(DATA_SIZE);

    /* 随机输入改用框架 RNG（random64，受 -s 控制可复现）：原先
     * std::random_device 播种破坏 -s 可复现纪律（AGENTS.md 关键规则：
     * 随机输入必须全经框架 RNG）。覆盖 int8/int16 全符号域（含正负），
     * 并预计算 sign-extend 的 int64 golden。 */
    for (size_t i = 0; i < DATA_SIZE; ++i) {
        data->byte_data[i] = static_cast<int8_t>((random64() & 0xFF) - 128);
        data->halfword_data[i] = static_cast<int16_t>((random64() & 0xFFFF) - 32768);
        data->golden_byte_ext[i] = static_cast<int64_t>(data->byte_data[i]);
        data->golden_half_ext[i] = static_cast<int64_t>(data->halfword_data[i]);
        data->src_data[i] = static_cast<uint8_t>(random64() & 0xFF);
        data->golden_copy[i] = data->src_data[i];  // 预期复制结果与原数据相同
    }

    test->data = data;
    return EXIT_SUCCESS;
}

// 运行测试：每次迭代计算并比较，使用线程局部缓冲
static int cbw_arm_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<TestData*>(test->data);
    if (!data) return EXIT_FAILURE;

    do {
        // ----- 1. 使用内联汇编测试 LDRSB / LDRSH -----
        // 每轮随机索引覆盖整张表（原只校验 [0] 单元素，载荷 SDC 检测力近乎为零）。
        size_t idx = random64() % DATA_SIZE;
        int64_t ext_byte_val;
        uint64_t addr = reinterpret_cast<uint64_t>(data->byte_data.data() + idx);
        __asm__ volatile (
            "ldrsb %0, [%1]"
            : "=r"(ext_byte_val)
            : "r"(addr)
            : "memory"
        );

        int64_t ext_half_val;
        uint64_t half_addr = reinterpret_cast<uint64_t>(data->halfword_data.data() + idx);
        __asm__ volatile (
            "ldrsh %0, [%1]"
            : "=r"(ext_half_val)
            : "r"(half_addr)
            : "memory"
        );

        bool byte_ok = (ext_byte_val == data->golden_byte_ext[idx]);
        bool half_ok = (ext_half_val == data->golden_half_ext[idx]);

        // ----- 2. 调用 Arm Optimized Routines libstringlib 的 memcpy 作为真实负载 -----
        // 使用 AOR 提供的 __memcpy_aarch64 复制数据，并用 __memcmp_aarch64 验证其正确性
        void *ret = __memcpy_aarch64(data->dst_data.data(), data->src_data.data(), DATA_SIZE);
        bool lib_ok = (ret == data->dst_data.data()) &&
                      (__memcmp_aarch64(data->dst_data.data(), data->golden_copy.data(), DATA_SIZE) == 0);

        // ----- 3. 存储一致性测试 -----
        uint64_t store_buf[2] = {static_cast<uint64_t>(ext_byte_val),
                                 static_cast<uint64_t>(ext_half_val)};
        uint64_t reload_buf[2];
        memcpy(reload_buf, store_buf, sizeof(store_buf));
        bool consistent = (reload_buf[0] == store_buf[0]) && (reload_buf[1] == store_buf[1]);

        bool passed = byte_ok && half_ok && lib_ok && consistent;

        if (!passed) {
            // Fail 详查：把本次输入与输出落进 yaml 的 data: 字段，再用 report_fail_msg
            // 记录失败位置（框架自动标 thread failed）。report_fail_msg 是 noreturn。
            char ctx[256];
            snprintf(ctx, sizeof(ctx),
                     "cbw_arm: byte_ok=%d (ext=0x%016llX golden=0x%016llX); "
                     "half_ok=%d (ext=0x%016llX golden=0x%016llX); "
                     "lib_memcpy_ok=%d; consistent=%d",
                     (int)byte_ok, (unsigned long long)ext_byte_val,
                     (unsigned long long)data->golden_byte_ext[idx],
                     (int)half_ok, (unsigned long long)ext_half_val,
                     (unsigned long long)data->golden_half_ext[idx],
                     (int)lib_ok, (int)consistent);
            log_data("cbw_arm input byte_data (signed, 256B)",
                     data->byte_data.data(), DATA_SIZE * sizeof(int8_t));
            log_data("cbw_arm input halfword_data (signed, 256×16-bit)",
                     data->halfword_data.data(), DATA_SIZE * sizeof(int16_t));
            log_data("cbw_arm input src_data (for memcpy, 256B)",
                     data->src_data.data(), DATA_SIZE * sizeof(uint8_t));
            log_data("cbw_arm output dst_data (after __memcpy_aarch64, 256B)",
                     data->dst_data.data(), DATA_SIZE * sizeof(uint8_t));
            report_fail_msg("%s", ctx);
            // report_fail_msg 不返回
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int cbw_arm_finish(struct test *test) {
    auto *data = static_cast<TestData*>(test->data);
    delete data;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

DECLARE_TEST(cbw_arm,
             "Signed load and extension (LDRSB/LDRSH) with libstringlib load")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = cbw_arm_init,
    .test_run = cbw_arm_run,
    .test_cleanup = cbw_arm_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
