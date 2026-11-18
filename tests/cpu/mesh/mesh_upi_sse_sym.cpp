#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#ifdef __aarch64__
#include <arm_neon.h>
#include <unistd.h>
#include <cstdlib>
#include <sys/sysinfo.h>

#define VECTOR_SIZE 4                     // NEON 可容纳 4 个 float
#define BLOCK_ELEMENTS 8192               // 每块元素数(加大块以降低原子争用)
#define VECTORS_PER_BLOCK (BLOCK_ELEMENTS / VECTOR_SIZE)  // = 2048
// 块数/总元素数在 init 中按目标内存动态计算(见 mesh_target_bytes)。

namespace { /* ODR: per-TU TestData (66-file same-name hazard) */
struct TestData {
    alignas(16) float *data;             // 共享数据数组，16 字节对齐
    size_t num_blocks;                   // 运行时块数(按目标内存计算)
    size_t total_elements;               // 运行时总元素数
    std::atomic<uint64_t> next_block;
    std::atomic<double> global_sum;      // 用 double 累加，减少误差
    std::atomic<uint64_t> allocated_blocks;
    std::atomic<uint32_t> thread_idx;
    /* sense-reversal barrier (G2'/G3' fix): the original iter/read_done/
     * round_done protocol deadlocked from round 1 (read_done was never
     * set to 1; iter only advanced after a completed round), so the sum
     * check never executed and the test passed vacuously. Pre-existing
     * since the 098d4aa port. */
    std::atomic<uint32_t> arrived;
    std::atomic<uint32_t> epoch;
};
}

// 内存消耗目标: 环境变量 SDC_MESH_BUFFER_BYTES 显式指定(字节)；
// 未指定时按物理内存的 ~90% 在 2 路 mesh 战役间均摊(FP 用例是唯一大内存消费者)。
// 边界声明(R5,#308 意见20/21/22 实测推翻 R4 版本;根治跟踪 #357,空过
// PASS 记账语义另见 #358):本目标默认取物理内存 45%(或 env 显式指定)。
// 本用例全线程并行认领填充(while(true),循环体无任何
// test_time_condition)——整轮时长既不受 -t 约束也不受迭代预算约束;
// 唯一硬界是父进程宽限 max(5×-t+30s, 300s)。R6 勘误(意见25):R5 版
// 「(block_ok/ok → FAIL)是唯一的真检出」与彼时本分支代码不符(写侧
// 立即回读的 ok 曾在空体 if 中被丢弃)——该修复已随 PR #331(b8149b84)
// 并入 main:写侧立即读回逐 lane 失配 → report_fail 硬 FAIL(同线程紧随
// own store 的字节级内容校验)。#324 勘误:读阶段校验能力记账为「聚合和
// read_sum == expected_sum(写侧累加 vs 读侧全量重读——非逐字节,补偿性
// 双错等和保持型损坏不可见)+ store_ok 私有通路自检(读路径 store/reload
// 自比较,不对照写入值,对共享缓冲内容零检出;旧名 consistent 恒真且
// 误导为数据校验,本 issue 改名)」——族级 per-element golden 由 #332
// 跟踪;等待每自旋查预算(耐心 ~40 自旋)→ 预算耗尽即 break 跳过读
// 阶段,该轮零校验。默认 ≈27.7 GB(61 GiB 主机)本机实测首轮 26.7 s
// (超 -t 3 s 预算 ~9 倍;R4 检视轮同配置 53.9 s、18 倍)后空转 pass
// (修复前口径;#331 的 validated_any 并入后,未完成任何校验轮的结局
// 已改为诚实 SKIP,不再空转 pass);更慢负载下撞宽限悬崖 → timed out/
// exit: invalid——两态结局的该轮均零校验。
// merge-base 既有(旧 0xFFFFFFFF 块封顶同样不按耐心收口),非本 PR
// 引入。
static size_t mesh_target_bytes() {
    const char *e = getenv("SDC_MESH_BUFFER_BYTES");
    if (e) {
        char *end = nullptr;
        unsigned long long v = strtoull(e, &end, 10);
        if (end && *end == '\0' && v > 0)
            return (size_t)v;
    }
    struct sysinfo si;
    if (sysinfo(&si) == 0 && si.totalram > 0) {
        size_t total = (size_t)si.totalram * si.mem_unit;
        return total / 10 * 9 / 2;
    }
    return (size_t)1 << 30;   // 1 GiB 兜底
}

static int mesh_upi_sse_sym_init(struct test *test) {
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) return -255;

    auto *td = new TestData;
    if (!td) return EXIT_FAILURE;

    const size_t elem_bytes = sizeof(float);
    const size_t block_bytes = BLOCK_ELEMENTS * elem_bytes;
    size_t target = mesh_target_bytes();
    td->num_blocks = target / block_bytes;
    if (td->num_blocks < 1) td->num_blocks = 1;
    /* 块索引空间 64 位(R2):num_blocks 不再封顶(target 经 floor 除法而来,
     * num_blocks × block_bytes ≤ target ≤ SIZE_MAX,乘法无 size_t 溢出)。
     * 去封顶必须配套 64 位块索引:next_block 若仍是 uint32,块数 ≥ 2^32 时
     * fetch_add 越过 0xFFFFFFFF 回绕到 0,低块被重复认领重写——求和族
     * global_sum 重复累计→假 FAIL,break 式认领循环每 2^32 次认领仅一张
     * 退出票。该回绕在旧封顶下仅剩一个角落可触发:计算块数 ≥ 2^32 时
     * 封顶恰把 num_blocks 钉在 0xFFFFFFFF,拿到 0xFFFFFFFF 的线程 break
     * 后计数器回绕到 0,其余写者重新认领低块重写,退出票每 2^32 次认领
     * 仅一张、N 线程需 N 张,写阶段无法在测试时限内自然终止;但本用例
     * 2^32 块 = 128 TiB,init 分配必败,该角落实际不可达(计算块数 ≤
     * 2^32 − 线程数的常态亦不可达);
     * 历史假 FAIL 的真因是首轮已修的偏移乘法回绕:旧 size_t offset =
     * block * BLOCK_ELEMENTS 是 32 位乘法,元素偏移 ≥ 2^32(缓冲 ≥ 16 GiB)时
     * 低偏移被回绕索引重复写入,缓冲上半段地址从未被写。 */
    td->total_elements = td->num_blocks * BLOCK_ELEMENTS;
    size_t total_bytes = td->total_elements * elem_bytes;

    td->data = (float*)aligned_alloc(16, total_bytes);
    if (!td->data) {
        delete td;
        return EXIT_FAILURE;
    }
    memset(td->data, 0, total_bytes);

    td->next_block.store(0, std::memory_order_relaxed);
    td->global_sum.store(0.0, std::memory_order_relaxed);
    td->allocated_blocks.store(0, std::memory_order_relaxed);
    td->thread_idx.store(0, std::memory_order_relaxed);
    td->arrived.store(0, std::memory_order_relaxed);
    td->epoch.store(0, std::memory_order_relaxed);
    test->data = td;

    return EXIT_SUCCESS;
}

static int mesh_upi_sse_sym_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);

    // 分配唯一线程 ID（用于每轮输出与 asymm 写者轮转）
    int id = td->thread_idx.fetch_add(1, std::memory_order_relaxed);

    /* 线程总数取框架权威值 thread_count()（自动尊重 --cpuset、
     * test.max_threads 与 OS 亲和性限制，每个线程取值一致）。
     * 原运行时“3 次稳定读数”启发式是 098d4aa 移植引入的缺陷：大规模
     * 并发下严重欠计（191 线程实测检出 2，甚至读出垃圾值），屏障参与
     * 者数目随之错误 → 轮次隔离崩溃 → 误报 sum mismatch；此前的死锁
     * 屏障恰好掩盖了它。另注：该启发式在 -n 1 下会永久自旋挂死，
     * thread_count() 则给出确定性结果。 */
    uint32_t total_threads = thread_count();
    if (total_threads < 2) {
        log_skip(CpuTopologyIssueSkipCategory,
                 "mesh_upi_sse_sym requires at least 2 threads (inter-core test); "
                 "skipping on this thread count");
        return EXIT_SKIP;
    }

    /* randomization hardening H14' (P17): framework RNG。
     * 取整数值 float：大 N 下 double 求和仍精确(整数 <=2^53 无舍入)，
     * 保证 read_sum == expected_sum 在任意块序下都成立。 */
    auto dist = []() { return (float)(int32_t)(frandomf_scale(2000.0f) - 1000.0f); };

    #define GREEN "\033[32m"
    #define RED   "\033[31m"
    #define RESET "\033[0m"

    uint32_t my_epoch = 0;
    /* 本线程是否完成过至少一整轮「填充→全量读取校验」。-t 在相位 1/屏障处
     * 到期时循环 break 落到函数尾——若从未完成任何校验轮即返回 EXIT_SUCCESS，
     * 就是「零校验 PASS」空过（issue #358，违反 CLAUDE.md 规则 6 精神：
     * no-op 测试报 pass 是 bug）。改为诚实 SKIP：复测台账 skip=1 即暴露
     * 「本轮未验证任何东西」（PR #331 二轮意见 #23）。 */
    bool validated_any = false;
    do {
        // ---------- 阶段 1：所有线程共同填充数据 ----------
        // （本轮状态已由上一轮 sense-reversal 屏障的最后到达线程重置）

        // 所有线程竞争分配块并写入随机浮点数据（每块 BLOCK_ELEMENTS 个元素）
        while (true) {
            uint64_t block = td->next_block.fetch_add(1, std::memory_order_seq_cst);
            if (block >= td->num_blocks) break;
            // 块号 64 位,与偏移乘法同宽:首轮修 32 位乘法偏移回绕(元素偏移
            // ≥ 2^32 时低偏移被回绕索引重复写入,缓冲上半段从未被写);R2 去封顶后
            // 块号本身也须 64 位,否则 fetch_add 在块数 ≥ 2^32 时回绕到 0,
            // 低块被重复认领重写。
            size_t offset = (size_t)block * BLOCK_ELEMENTS;
            float vals[BLOCK_ELEMENTS];
            double local_sum = 0.0;
            for (int j = 0; j < BLOCK_ELEMENTS; ++j) {
                vals[j] = dist();
                local_sum += (double)vals[j];
            }
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                float32x4_t vec = vld1q_f32(vals + v * VECTOR_SIZE);
                vst1q_f32(td->data + offset + v * VECTOR_SIZE, vec);  // 对齐存储
            }
            __sync_synchronize();   // 类似 _mm_sfence

            // 一致性测试：立即读回并比较（整块逐向量）
            bool ok = true;
            for (int v = 0; v < VECTORS_PER_BLOCK; ++v) {
                float32x4_t written = vld1q_f32(vals + v * VECTOR_SIZE);
                float32x4_t loaded = vld1q_f32(td->data + offset + v * VECTOR_SIZE);
                uint32x4_t cmp = vceqq_f32(written, loaded);
                if (!(vgetq_lane_u32(cmp, 0) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 1) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 2) == 0xFFFFFFFF &&
                      vgetq_lane_u32(cmp, 3) == 0xFFFFFFFF)) {
                    ok = false;
                    break;
                }
            }
            /* 写相位立即读回是逐 lane 字节级比对，其结果必须落地为失败：
             * 读相位的全量校验只有聚合和（和保持型损坏——如补偿性双错、
             * lane 互换——不可见）与 store_buf 自拷贝比较（不对照写入值），
             * 旧注释「最终会在全量校验时捕获」对这类损坏不成立（PR #331
             * 二轮意见 #22）。avx512_sym 同款语义先例（其 135-148 行注释）。
             * 块为本线程独占认领，写后立即回读失配 = 真实 store/load 损坏。 */
            if (!ok) {
                fprintf(stderr, "mesh_upi_sse_sym: Thread %d, block %llu immediate read-back mismatch, written vals[0..3]=(%.6f,%.6f,%.6f,%.6f), loaded data[0..3] at offset %zu=(%.6f,%.6f,%.6f,%.6f), result=%sFAIL%s\n",
                        id, (unsigned long long)block,
                        vals[0], vals[1], vals[2], vals[3],
                        offset,
                        td->data[offset], td->data[offset + 1],
                        td->data[offset + 2], td->data[offset + 3],
                        RED, RESET);
                fflush(stderr);
                report_fail_msg("mesh_upi_sse_sym: write-phase immediate read-back mismatch (store/load corruption)");
                return EXIT_FAILURE;
            }

            // 累加全局和（使用 double 原子操作）
            double old_sum = td->global_sum.load(std::memory_order_relaxed);
            while (!td->global_sum.compare_exchange_weak(old_sum, old_sum + local_sum,
                                                         std::memory_order_seq_cst,
                                                         std::memory_order_relaxed)) {}
            td->allocated_blocks.fetch_add(1, std::memory_order_seq_cst);
        }

        // 等待所有线程完成写入
        while (td->allocated_blocks.load(std::memory_order_seq_cst) < td->num_blocks &&
               test_time_condition(test)) {
            __asm__ volatile("yield");
        }
        if (!test_time_condition(test)) break;

        __sync_synchronize();  // 确保写入全局可见

        // ---------- 阶段 2：所有线程读取完整数组并验证 ----------
        // Sum snapshot BEFORE the read loop (TOCTOU fix): a fast thread can
        // finish its read, run the reset phase and start writing the NEXT
        // iteration while this thread is still walking the buffer. Sampling
        // global_sum after the read would race that reset/next-write and
        // produce a false mismatch (expected_sum cleared to 0 / partially
        // overwritten by the next iteration's writes).
        double expected_sum = td->global_sum.load(std::memory_order_seq_cst);
        double read_sum = 0.0;
        /* store_ok 检私有 store/reload 通路自一致(store_buf 是本线程栈上
         * 缓冲,store 后立即 reload 自比较)——两臂消费同一份刚载入的
         * 数据,不对照写入值,对共享缓冲区内容损坏零检出(作为数据校验
         * 恒真),不是数据校验(#324:旧名 consistent 的恒真记账正是本
         * issue 立案项;#323 在 avx512_symm_f64 已把同款检查改名去误导)。
         * 读阶段真正的数据校验只有聚合和 read_sum == expected_sum——族级
         * per-element golden 由 #332 立案重构,勿以为本阶段有逐字节内容
         * 校验(avx_sym/avx512_sym 读阶段头注同款声明)。 */
        bool store_ok = true;
        alignas(16) float store_buf[VECTOR_SIZE];

        for (size_t i = 0; i < td->total_elements; i += VECTOR_SIZE) {
            float32x4_t v = vld1q_f32(td->data + i);
            float vals[VECTOR_SIZE];
            vst1q_f32(vals, v);
            for (int j = 0; j < VECTOR_SIZE; ++j) read_sum += (double)vals[j];

            // 私有通路自检：Store/Load 回读自比较（#324 改名，检出边界见声明处注释）
            vst1q_f32(store_buf, v);
            float32x4_t reload = vld1q_f32(store_buf);
            uint32x4_t cmp = vceqq_f32(v, reload);
            // 检查所有 4 个元素是否相等
            if (!(vgetq_lane_u32(cmp, 0) == 0xFFFFFFFF &&
                  vgetq_lane_u32(cmp, 1) == 0xFFFFFFFF &&
                  vgetq_lane_u32(cmp, 2) == 0xFFFFFFFF &&
                  vgetq_lane_u32(cmp, 3) == 0xFFFFFFFF)) {
                store_ok = false;
            }
        }

        bool passed = (read_sum == expected_sum) && store_ok;

        if (!passed) {
            /* 失败证据(原先每轮每线程都打印 PASS 行——子进程 stderr 被框架
             * 重定向进 memfd(logging.cpp open_memfd),轮次 × 全线程的输出
             * 全程驻留内存,额外增加 memfd 压力;#324 立案项,收口为仅
             * 失败时输出——avx_sym/avx512_sym 先例(PR #414,#325)。 */
            fprintf(stderr, "mesh_upi_sse_sym: Thread %d, data[0..3]=(%.4f,%.4f,%.4f,%.4f), read_sum=%f, expected_sum=%f, store_ok=%d, result=%s%s%s\n",
                    id,
                    td->data[0], td->data[1], td->data[2], td->data[3],
                    read_sum, expected_sum, store_ok,
                    passed ? GREEN : RED,
                    passed ? "PASS" : "FAIL",
                    RESET);
            fflush(stderr);
            report_fail_msg("mesh_upi_sse_sym: Sum mismatch or private store/reload self-consistency failure");
            return EXIT_FAILURE;
        }
        validated_any = true;   // 本轮全量读取校验已通过

        // ---------- 阶段 3：sense-reversal 屏障进入下一轮 ----------
        // 最后到达的线程在此重置下一轮状态（global_sum/next_block/
        // allocated_blocks）：fetch_add(acq_rel) 的到达顺序保证所有线程
        // 都已离开阶段 2 的校验，epoch 的 release 存储保证重置先于任何
        // 线程的下一轮填充可见 —— 这正是原协议想做而没做对的轮次隔离。
        uint32_t done = td->arrived.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (done == total_threads) {
            td->global_sum.store(0.0, std::memory_order_seq_cst);
            td->next_block.store(0, std::memory_order_seq_cst);
            td->allocated_blocks.store(0, std::memory_order_seq_cst);
            td->arrived.store(0, std::memory_order_relaxed);
            td->epoch.store(my_epoch + 1, std::memory_order_release);
        } else {
            while (td->epoch.load(std::memory_order_acquire) == my_epoch &&
                   test_time_condition(test)) {
                __asm__ volatile("yield");
            }
            if (!test_time_condition(test)) break;
        }
        ++my_epoch;

    } while (test_time_condition(test));

    if (!validated_any) {
        log_skip(TestResourceIssueSkipCategory,
                 "mesh_upi_sse_sym: test time expired before the first full "
                 "validation round completed (runtime-sized buffer fill "
                 "outlives -t); nothing was verified in this run");
        return EXIT_SKIP;
    }
    return EXIT_SUCCESS;

    #undef GREEN
    #undef RED
    #undef RESET
}

static int mesh_upi_sse_sym_finish(struct test *test) {
    auto *td = static_cast<TestData*>(test->data);
    free(td->data);
    delete td;
    return EXIT_SUCCESS;
}

#else
static int mesh_upi_sse_sym_init(struct test *test) {
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): ARM NEON required for mesh_upi_sse_sym");
    return EXIT_SKIP;
}
static int mesh_upi_sse_sym_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SKIP; }
static int mesh_upi_sse_sym_finish(struct test *test) { (void)test; return EXIT_SUCCESS; }
#endif
DECLARE_TEST(mesh_upi_sse_sym,
             "Symmetrical stress … do NEON loads and stores from L1D to L1D with different directions and switching cores")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = mesh_upi_sse_sym_init,
    .test_run = mesh_upi_sse_sym_run,
    .test_cleanup = mesh_upi_sse_sym_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
