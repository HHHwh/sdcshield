/*
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef INC_TEST_DATA_H
#define INC_TEST_DATA_H

#include "sandstone_chrono.h"

#include <assert.h>
#include <atomic>
#include "gettid.h"

struct DeviceRange
{
    // a contiguous range
    int starting_device;
    int device_count;
};

struct ThreadRange
{
    // a contiguous range
    int starting_thread;
    int thread_count;
};

enum class LogicalProcessor : int { None = -1 };

enum ThreadState : int {
    thread_not_started = 0,
    thread_running = 1,
    thread_failed = 2,
    thread_debugged = 3,
    thread_succeeded = -1,
    thread_skipped = -2,
};

namespace PerThreadData {
struct Common
{
    std::atomic<ThreadState> thread_state;

    /* file descriptor for logging */
    int log_fd;

    /* Records number of messages logged per thread of each test */
    std::atomic<int> messages_logged;

    /* Records the number of bytes log_data'ed per thread */
    std::atomic<unsigned> data_bytes_logged;

    enum class Flag {
        CallbackCalled = 0x0001,
    };
    uint32_t thread_flags;
    friend constexpr inline uint32_t operator|(Flag f1, Flag f2)
    {
        return unsigned(f1) | unsigned(f2);
    }
    friend constexpr inline uint32_t operator&(uint32_t flags, Flag f2)
    {
        return flags & unsigned(f2);
    }

    LogicalProcessor failing_cpu;
    MonotonicTimePoint fail_time;
    bool has_failed() const
    {
        assert((fail_time > MonotonicTimePoint{}) == (int(failing_cpu) >= 0));
        return fail_time > MonotonicTimePoint{};
    }
    bool has_skipped() const
    {
        return fail_time < MonotonicTimePoint{};
    }

    void init()
    {
        thread_state.store(thread_not_started, std::memory_order_relaxed);
        messages_logged.store(0, std::memory_order_relaxed);
        data_bytes_logged.store(0, std::memory_order_relaxed);
        failing_cpu = LogicalProcessor::None;
        fail_time = MonotonicTimePoint{};
        thread_flags = {};
    }
};

/* VmHWM 反馈槽 (T7, spec §4.4): 每 slice 一个实例, 随 PerThreadData::Main
 * 驻留在 MAP_SHARED shmem 的可写区(protect_shmem 只读保护的是 shmem 头,
 * 不含本区), 上代 child 的写下代 child/parent 可见 —— 闭环的跨 fracture
 * 载体。字段语义:
 *   - test_id_hash: test->id 的 FNV-1a 64 位哈希; 0 = 空槽(fresh memfd
 *     零页)。槽被不同测试复用时按哈希失配重置(防读到陈旧值, spec §8)。
 *   - eff_ratio:   VmHWM/(n²·elem·本slice线程数) 的 EMA(α=0.5)。记账
 *     口径由联合求解侧缓存(sdc_joint_cache), 未走联合求解的测试无写入。
 *   - samples:     EMA 样本数(该槽该测试的写入次数), 0 = 无有效反馈。
 * 生命周期 (M5, 防"闭环静默死亡"): 本结构【不进 Main::init()】——
 * init_per_thread_data() 在每 fracture/test 前清全部槽的 init() 字段,
 * 反馈挂进去会被逐 fracture 抹掉; 只在 parent 侧 commit_shmem() 首次
 * 映射后清零一次, 语义 = 跨 test/fracture 持久(per-invocation shmem,
 * memfd 每次运行新建 ⇒ 无跨运行记忆)。
 * x86-64 (rule 5): 本字段在 x86 同样编译(避免 Main 布局分叉), 但写入/
 * 读取调用点全部 arm64 门控 ⇒ x86 无写入者, 恒为零值, 行为不变。 */
struct MemFeedback
{
    uint64_t test_id_hash;
    double eff_ratio;
    uint32_t samples;
};

struct alignas(64) Main : Common
{
    DeviceRange device_range;
    ThreadRange thread_range;
    MemFeedback mem_feedback;       /* 48B body + 24B = 72B → sizeof 128 */
};

struct alignas(64) TestCommon : Common
{
    /* Number of iterations of the inner loop (aka #times test_time_condition called) */
    uint32_t inner_loop_count;
    uint32_t inner_loop_count_at_fail;
    LogicalProcessor previous_cpu;  // used by reschedule

    /* Thread ID */
    std::atomic<tid_t> tid;

    void init()
    {
        Common::init();
        inner_loop_count = inner_loop_count_at_fail = 0;
        previous_cpu = LogicalProcessor::None;
    }
};
} // namespace PerThreadData

#endif /* INC_TEST_DATA_H */
