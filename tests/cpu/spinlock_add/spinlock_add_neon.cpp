/**
 * @copyright
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b spinlock_add_neon
 * @parblock
 * Spinlock-liveness + coherence SDC probe (qspinlock hard-lockup phenomenon).
 *
 * 现象针对性设计（qiling-vmcore 五案诊断 → sdcshield 侧检测缺口闭合）：
 * 中行/中央国债两例 NMI hard lockup 的根因是锁 cacheline 一致性元数据瞬态
 * 故障（解锁写丢失 / 锁值读冻结），内核侧表现为关中断自旋 >10s 被 watchdog
 * 咬停。现有 lock/spinlock 用例遇同类故障只会无声挂死（超时/墙收，不算
 * 检出）。本测试补三条检测通道：
 *   通道A（数据SDC）  ：NEON vfma 计算结果 vs init 同源 golden（逐字节 memcmp）
 *   通道B（数据行W丢）：全局锁保护下的共享计数器，cleanup 时
 *                        Σ 逐线程获锁数 == 共享计数器（add-only，无抵消方向）
 *                        + 锁行元数据对账：epoch==共享计数器（同临界区构造
 *                        等值）、magic/pad==init 指纹——锁行元数据写丢失而
 *                        lock_word 完好（非挂死形态）时通道A/C 全盲，此处补检
 *   通道C（锁字挂死，核心增量）：获锁等待 >10s（对齐 sbsa-gwdt）后进入
 *                        1s 确认窗二次判定——epoch 停走且 CAS 持续失败 =
 *                        确认冻结（真冻结时 epoch 必停走：epoch 唯一递增
 *                        路径在持锁者临界区内，lock_word 冻结=无人能再持
 *                        锁）→ report_fail + dump 锁所在 64B cacheline 全部
 *                        8 个 u64（对齐五案"8 字节窗口撕裂、同行其余字段
 *                        完好"的现场录像格式；magic 字段对账爆炸半径）；
 *                        确认窗内 epoch 增长或 CAS 成功 = 锁在正常流转、
 *                        本线程被调度饥饿——log 告警并重置等待预算继续争
 *                        用（不 FAIL，防高负载共享机假阳性），跨 3 个窗口
 *                        仍无获锁才以进展异常 FAIL（消息与冻结论区分）；
 *                        确认窗另设观察者停摆防护（PR#328 检视）——窗内
 *                        相邻采样间隔 >100ms = 本线程自身未被调度（cgroup
 *                        freezer/CRIU/VM 迁移暂停），冻结时长不得计入窗内
 *                        墙钟（重启确认窗），反复停摆以区分性签名 FAIL、
 *                        不以 hard-lockup CONFIRMED 定罪
 *
 * 检出时延边界（PR#341 检视提出、后续轮依框架代码修正——初版误采
 * 「-t 到期即处决」的时序断言，实为虚构盲区）：通道C
 * 的 FROZEN 判定自身需 ~11s（10s 看门狗 + 1s 确认窗 + dump 落盘）、
 * NO_PROGRESS 判定需 ~40s（3 次预算重置 + 第 4 窗口），而 acquire 循环
 * 只认自身预算、不查 test_time_condition（有意取舍：等待者查时间条件并
 * 提前放弃 = 把检出机会让渡给超时，判定完整性优先）——判定完成时刻可溢
 * 出 -t 名义窗口（onset + 11s/40s > -t）。溢出部分由框架父进程的收集墙
 * 容忍：test_timeout = max(5×-t + 30s, 300s)（framework/sandstone.cpp），
 * worker 全部 join 后子进程正常退出，FAIL 消息与 64B dump 照常产出、结果
 * 收集为 Failed 而非 TimedOut——onset+11s/40s ≤ -t+11s/40s <
 * max(5×-t+30s, 300s) 对一切 -t 恒成立，尾部检出不会丢失。真实边界只有
 * 两条，在此如实披露：① 显式 --max-test-time 设低于 onset+判定时延时，
 * 父进程墙先到，子进程被处决（SIGQUIT→20s→SIGKILL 阶梯，
 * terminate_children），FAIL 与 dump 丢失、以 TimedOut 收场；② 真实
 * TimedOut（子进程挂满 max(5×-t+30s, 300s) 收集墙）是比通道C 所能报告
 * 的更强异常形态——单次冻结即产出 FROZEN FAIL、不可能跑到墙，能挂满墙
 * 的只剩框架/系统级停摆或不可中断 I/O 睡眠等，应按更强信号处置、不应
 * 按良性超时轻放。
 *
 * 三通道自检（部署到新机器后必跑，coherence_probe SELFTEST_W 同款纪律——
 * 阳性通过后阴性结果才有意义；错值 fail loudly 而非静默 off，防 typo 无声
 * 击穿部署纪律）：
 *   -O spinlock_add_neon.selftest=stall  首个进临界区线程持锁 15s →
 *                                        通道C 应触发（前置条件由 init 强制：
 *                                        ≥2 线程（否则唯一线程自睡自放、绿
 *                                        pass 假绿）、-t ≥16000ms 派生下限
 *                                        （推荐 25000——阳性 FAIL(~11s) 与排空
 *                                        (~15s) 须落标称窗口内），不满足 fail
 *                                        loudly；15s > 看门狗 10s + 确认窗 1s
 *                                        + 裕量——模拟冻结须长于两段式全检出
 *                                        时延，否则确认窗内 CAS 成功会被判
 *                                        为饥饿、阳性击穿）
 *   -O spinlock_add_neon.selftest=data   init 毒化各 slot golden →
 *                                        通道A 应触发（单线程即可，cpu 无关）
 *
 * 负载形态：每线程独立 NEON vfma 链（64 floats/slot，输入 ∈ [-1,1]，
 * 4 组 × 4 向量 × 4 轮 fused multiply-add，输出理论上界 ~3e20——4 轮交叉
 * vfma 指数增长所致，远小于 FLT_MAX，无溢出无 NaN 路径；本机探针实测
 * 10^6 slot max|out|=1.36e20、无非有限值），
 * 随后在全局自旋锁（CAS-acquire / release-store，qspinlock 同指令谱系）
 * 临界区内递增共享计数器——深争用 + 实算负载。
 *
 * 多核逻辑安全（结果与核数/线程数无关）：slot=cpu%SLOTS 输入只读共享、
 * 内核纯函数、out 缓冲线程栈私有、计数对（shared/local）同在临界区内
 * 更新（不一致窗口小于 locks.cpp 先例）、日志 O_APPEND 单次 write()。
 *
 * 逐次验证日志（-O spinlock_add_neon.logverif=1，默认 0；logverif/
 * logverif_retain 错值（非 0/1）及无效组合（logverif_retain=1 而未开
 * logverif——retain 只豁免绿跑删除、文件本身由 logverif 创建，单开
 * retain 静默无效）fail loudly 见 init，键名 typo 为框架级
 * 已知缺口、在此披露）：
 * /var/tmp/spinlock_add_neon_<pid>.log，每行 = 迭代号/线程/输入前 4 元素/
 * 输出前 4 元素/验证结果，PASS 绿（\033[32m）、FAIL 红（\033[31m）；
 * 每线程 10000 行上限后转仅记失败（防日志爆炸），失败行永不受上限约束。
 * 失败行与绿行同格式（同样仅前 4 元素）；全 64 float in/out/golden 对照
 * dump 经 log_error 落框架日志、不写入本文件——取证需全量 dump 时到框架
 * 日志找（PR#341 检视）。logverif=0 时完全不触碰 /var/tmp；
 * logverif=1 但打开失败（环境不可用或后缀重试耗尽）时 log_warning 降级
 * 禁用日志、测试照跑（诊断附属不升格为检测失败；'W' 级告警默认
 * verbosity 不显示、须 -v，见 init 降级注释）。文件口径与生命周期
 * （PR#341 检视）：fracture 架构下每切片各建一个
 * <pid>.log、行预算随切片重置（fork 起新子进程、-f no 同进程重跑
 * init+run——run 局部变量每切片归零），fork 模式长跑呈每秒数百个碎片
 * 文件而非单一连续日志；-f no 并非「单文件 10000 行/线程」的例外
 * （PR#341 检视）——retain=0 时同名文件每绿切片 unlink 后
 * 重建（内容不跨切片累积，绿跑终态存活 0），retain=1 时同 pid 每切片
 * 撞名走后缀（见 retain 段）；10000 行/线程上限的可达路径是单切片大
 * 循环（--max-test-loop-count 自带禁用 fracture，实测 20000 循环恰
 * 10000 行）。绿跑（通道B 对账全绿 ∧ 零红 FAIL 行 ∧ 非 selftest ∧ 未开
 * logverif_retain）cleanup 时删除本轮文件，长期战役绿跑稳态存活文件数
 * = O(当前并发子进程数)；任何检出迹象（数据 SDC 红行、通道C FAIL、通道B
 * 对账失败）或 selftest 阳性自检保留文件作取证。-O
 * spinlock_add_neon.logverif_retain=1（默认 0，须与 logverif=1 同开、
 * 组合校验 fail loudly 见 init——单开不建任何文件）保留绿跑文件供人工审计
 * （PR#341 检视：golden 为 init 同源预计算，init/run 一致同错
 * 时三通道全绿，logverif 行是绿跑期唯一可人工核对的计算值迹）——代价
 * 是回到无界堆积，长战役慎用；撞名遗留文件有两形态（PR#341 检视）：
 * fork 模式 pid 复用/回绕（本机 pid_max=4194304、实测 ~240 切片/s ≈
 * 4.8h/圈，上限 64 ≈ 13 天连续战役才耗尽）
 * 与 -f no 同 pid 每切片必撞（无需回绕，烧穿速率 = fracture 率，实测
 * 本机 36-65 切片/s 随载、65 格 1-3s 烧穿，耗尽后逐切片降级、
 * logverif 死亡直至 rm 旧文件），init 以 <pid>-<n>.log 后缀退格重试，
 * 审计文件名因而不是恒定 <pid>.log。
 * 红跑文件为取证设计、无上限无轮转（PR#341 检视裁定）：嘈杂共享机上
 * 的饥饿假阳性 FAIL 亦逐切片留档（实测速率 ~840MB/天量级），长战役
 * 操作员须自行巡检清理 /var/tmp——上限/轮转与取证完整性冲突，按取证
 * 优先保留此边界并在此声明。
 *
 * Deviation note (documented): 内核为自包含 NEON intrinsics（本仓 neon_rot
 * 家族同范式）；第三方库（openblas/sleef）计算负载变体为后续独立单元。
 * 循环条件直连 test_time_condition（非 TEST_LOOP，locks/spinlock 家族习语）
 * ——--inject-idle 的占空比注入对本测试不生效（时长仍被同选项折算），依据
 * 与边界见 run 循环尾部注释（PR#328 检视）。
 * @endparblock
 */

#include "sandstone.h"
#include "sandstone_p.h" /* selftest=stall 前置条件校验须读生效测试时长
                          * sApp->current_test_duration（公开 API 无时长访
                          * 问器）。耦合诚实声明（PR#341 检视）：本报
                          * 告文件是全仓唯一解引用 sApp 私有单例的测试
                          * （grep 'sApp->' tests/ 仅命中此处；mce_check/
                          * smi_count include sandstone_p.h 仅为传递可见
                          * interrupt_monitor.hpp，不读其成员）——框架重构
                          * 该私有成员（sandstone_p.h）时恰好只破坏本测试；
                          * 日后出现公开时长访问器时应改走之 */
#include <cerrno>   /* errno：logverif 打开失败降级路径使用；不依赖框架头链
                     * 传递包含（PR#328 检视——上游 include 整理时
                     * 偶然可见性会静默破碎） */
#include <chrono>   /* std::chrono::milliseconds（selftest=stall 前置条件校验读
                     * 生效测试时长）：直连纪律同 <cerrno>——此前仅经
                     * sandstone_p.h:30 → sandstone_chrono.h:24 偶然可见
                     * （PR#328 检视） */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include <arm_neon.h> /* 本文件仅经 tests/cpu/meson.build 的 aarch64 门控编译
                       *（spinlock_unaligned 同款先例）；非 aarch64 构建不含
                       * 本用例——误接线时 arm_neon.h/NEON intrinsics 即编译
                       * 失败，显式暴露而非无声死代码 */

#define SPINLOCK_ADD_NEON_SLOTS 128
#define SPINLOCK_ADD_NEON_FLOATS 64          /* 16 x float32x4 */
#define SPINLOCK_ADD_NEON_ROUNDS 4           /* |x|<=1 → 输出理论上界 ~3e20（<<FLT_MAX；实测 max ~1.4e20） */
#define SPINLOCK_ADD_NEON_STALL_SECONDS 10.0 /* sbsa-gwdt 同阈值 */
#define SPINLOCK_ADD_NEON_CONFIRM_SECONDS 1.0 /* 超时后的冻结确认窗（两段式判定，见 acquire） */
#define SPINLOCK_ADD_NEON_OBSERVER_GAP_SECONDS 0.1 /* 确认窗内观察者停摆判定阈值（见 acquire），远低于
                                                    * 确认窗、远高于正常抢占间隔（本测试 worker 每核
                                                    * 单线程钉核自旋，毫秒级间隙不触发） */
#define SPINLOCK_ADD_NEON_GAP_RESTARTS_MAX 3 /* 确认窗因观察者停摆重启的上限：超出即放弃冻结判定 */
#define SPINLOCK_ADD_NEON_STALL_SLEEP_SECONDS 15 /* 自检持锁秒数：须 > 看门狗+确认窗+裕量 */
#define SPINLOCK_ADD_NEON_STALL_MIN_TEST_MS ((SPINLOCK_ADD_NEON_STALL_SLEEP_SECONDS + 1) * 1000)
                                                 /* selftest=stall 的 -t 下限：阳性 FAIL(~11s)
                                                  * 与持锁排空(~15s) 须落在标称窗口内 */
#define SPINLOCK_ADD_NEON_STARVATION_RESETS_MAX 3 /* 饥饿重置上限：跨 3+1 窗口无获锁 → 进展异常 FAIL */
#define SPINLOCK_ADD_NEON_LOG_MAX 10000      /* 每线程逐次日志行上限 */
#define SPINLOCK_ADD_NEON_LOG_OPEN_TRIES 64 /* <pid>.log 撞 EEXIST 时 <pid>-<n>.log 后缀重试上限（见 init） */
#define SPINLOCK_ADD_NEON_MAGIC 0x5A5AC0DE5A5AC0DEull

/* 恰一条 cacheline：lock_word 是 8 字节窗口的靶心，epoch/holder/magic 是
 * "同行其余字段完好"的对照（五案指纹格式） */
struct alignas(64) SpinlockAddNeonLockLine {
    volatile uint64_t lock_word;  /* +0  : 0=free 1=held */
    volatile uint64_t epoch;      /* +8  : 累计获锁次数（持锁者更新） */
    volatile uint64_t holder;     /* +16 : 当前持锁者 cpu */
    volatile uint64_t magic;      /* +24 : 固定指纹 */
    volatile uint64_t pad[4];     /* +32..+56 */
};
static_assert(sizeof(SpinlockAddNeonLockLine) == 64, "lock line must be one cacheline");

struct SpinlockAddNeonData {
    float *inputs;   /* SLOTS x FLOATS，init 生成，所有线程只读 */
    float *golden;   /* SLOTS x FLOATS，init 同源内核预计算，只读 */
    SpinlockAddNeonLockLine *line;
    uint64_t *shared_counter; /* 锁内 add-only（通道B 靶） */
    uint64_t *local_counts;   /* SLOTS，锁内与 shared 同窗递增 */
    int log_fd;
    int logverif;             /* knob: 0/1 */
    int logverif_retain;      /* knob: 0/1——绿跑保留 logverif 文件（审计用） */
    int selftest;             /* knob: 0=off 1=stall 2=data */
    volatile int stall_armed;
    char log_path[128];       /* logverif 文件路径：绿跑 cleanup unlink 用 */
    uint64_t log_fail_lines;  /* 红 FAIL 行 / FAIL 迹象计数（RELAXED 原子递增，
                               * cleanup join 后 quiescent 读）——绿跑 unlink
                               * 的保守条件：任何检出迹象都保留文件取证 */
};

static uint64_t spinlock_add_neon_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* 任何 FAIL 迹象的统一记账点（绿跑 unlink 的保守条件，见 cleanup）：
 * 新增检测通道 / FAIL 分支时必须调用本函数——漏调用 = 该路径红跑的
 * logverif 取证文件被当绿跑误删且无告警（PR#341 检视：递增点
 * 分散的演进风险，集中到单一命名函数使契约可见）。
 * 契约边界（PR#341 检视）：本契约是注释级约定、无结构性强制——框架侧
 * 存在更强的失败事实（framework/sandstone_run.cpp run_one_test_inner
 * 以 thr->has_failed() 判 Failed，任何 'E' 前缀日志经
 * framework/logging.cpp message_common_check 置位）但测试侧 cleanup
 * 不可见不可查询，绿跑 unlink 判定只消费本函数记账与通道B 本地 ret
 * 两个信号；结构性闭合（框架向 cleanup 暴露可查询的 per-test 失败位，
 * 或本函数与 report_fail 家族在宏层绑定）超出本测试文件范围，作为
 * 框架级跟进项遗留——本文件以显式契约注释为边界，当前全部 FAIL
 * 路径已核对均有覆盖（检视实证）。RELAXED 原子（多线程
 * 并发递增，#16 同款纪律；cleanup 在 join 后 quiescent 读） */
static inline void spinlock_add_neon_note_fail(struct SpinlockAddNeonData *d)
{
    __atomic_fetch_add(&d->log_fail_lines, 1, __ATOMIC_RELAXED);
}

/* 纯函数 NEON 内核：4 组 x (a,b,c,d) 向量链 x 4 轮 vfma。确定性（IEEE 单
 * 舍入 FMA），无共享状态。
 * g 是向量槽基（每步处理 4 个 float32x4 = 16 float）：循环上界派生自
 * SPINLOCK_ADD_NEON_FLOATS/4 而非字面量——若以 16 写死，将来调大 FLOATS
 * 会让 kernel 只写每 slot out[] 前 64 float，而 run 的 memcmp 按 FLOATS
 * 比对到 kernel 从未写入的未初始化字节，静默变成假 FAIL 机器；
 * static_assert 锁住 16-float 步进对 FLOATS 可整除 */
static_assert(SPINLOCK_ADD_NEON_FLOATS % 16 == 0,
              "kernel group step covers 4 x float32x4 = 16 floats");
static void spinlock_add_neon_kernel(const float *in, float *out)
{
    for (int g = 0; g < SPINLOCK_ADD_NEON_FLOATS / 4; g += 4) {
        float32x4_t a = vld1q_f32(in + g * 4 + 0);
        float32x4_t b = vld1q_f32(in + g * 4 + 4);
        float32x4_t c = vld1q_f32(in + g * 4 + 8);
        float32x4_t d = vld1q_f32(in + g * 4 + 12);
        for (int r = 0; r < SPINLOCK_ADD_NEON_ROUNDS; ++r) {
            a = vfmaq_f32(a, b, c);
            b = vfmaq_f32(b, c, d);
            c = vfmaq_f32(c, d, a);
            d = vfmaq_f32(d, a, b);
        }
        vst1q_f32(out + g * 4 + 0, a);
        vst1q_f32(out + g * 4 + 4, b);
        vst1q_f32(out + g * 4 + 8, c);
        vst1q_f32(out + g * 4 + 12, d);
    }
}

/* CAS-acquire 自旋（qspinlock 同谱系）；等待超阈值返回 false（看门狗触发）。
 * 时钟每 1024 次重试采样一次（PR#328 检视）：逐次 clock_gettime
 * 走 vDSO、触碰 vvar 页另一共享行，深争用下约半数自旋预算为非锁流量，
 * 稀释锁行争用激发形态；1024 次自旋（CAS+yield，量级 ~0.1ms）<< 10s
 * 看门狗阈值，检出延迟增量可忽略 */
#define SPINLOCK_ADD_NEON_SPIN_CLOCK_PERIOD 1024
static_assert((SPINLOCK_ADD_NEON_SPIN_CLOCK_PERIOD &
               (SPINLOCK_ADD_NEON_SPIN_CLOCK_PERIOD - 1)) == 0,
              "spin clock sampling period must be a power of two");

enum SpinlockAddNeonAcquire {
    SPINLOCK_ACQUIRE_OK,
    SPINLOCK_ACQUIRE_FROZEN,      /* 确认冻结：确认窗内 epoch 停走 + CAS 持续失败 */
    SPINLOCK_ACQUIRE_NO_PROGRESS, /* 锁行持续流动但本线程跨窗口无法获锁 */
    SPINLOCK_ACQUIRE_OBSERVER_STALLED, /* 确认窗反复被本线程自身的停摆打断，无法给出冻结判定 */
};

/* 两段式看门狗判定（PR#328 检视）：等待超时 ≠ 锁冻结——等待者被
 * 调度饥饿 >10s（高负载共享机/cgroup 限流抢占本线程，此时他线程正常获锁）
 * 时墙钟同样超时，单段墙钟判定会把该形态误报为 hard-lockup。超时后进入
 * 确认窗（SPINLOCK_ADD_NEON_CONFIRM_SECONDS）二次采样：
 *   - CAS 成功，或锁行 epoch 增长 → 锁在正常流转，超时源于本线程饥饿/
 *     瞬态 → log 告警、重置等待预算继续争用，不 FAIL；
 *   - epoch 停走且 CAS 持续失败满窗 → 确认冻结 → FAIL。
 * 真冻结时 epoch 必停走：epoch 唯一递增路径是持锁者在临界区内的自增
 * （run 的 C 段），lock_word 冻结 = 无人再能成为持锁者 = 递增路径已死，
 * 故"停走才 FAIL"不引入新假阴性；非对称读冻结（本核观测冻住、他核正常
 * 流转）同样走 FAIL：本核 CAS 持续失败且（本核读到的）epoch 停走，按
 * 冻结论处。饥饿重置有上限（SPINLOCK_ADD_NEON_STARVATION_RESETS_MAX）：
 * 连续 3 次重置后仍超时且锁行在流动 = 持续饥饿/进展异常，以区分性消息
 * FAIL——既防假阳性也防线程在无限重置中永不返回。FAIL 报文/dump 的等待
 * 时长取进入 acquire 起的累计（t_entry 基线，跨窗求和），看门狗预算基线
 * t0 则每窗重置——两基线分工见函数内注释（PR#328 检视）。
 *
 * 观察者停摆防护（PR#328 检视）：确认窗的墙钟证据只在本线程
 * 实际运行时才成立。cgroup freezer / CRIU checkpoint / VM live-migration
 * 等全局进程冻结若落在确认窗内，冻结时长被原样计入 now-c0——恢复瞬间
 * 持锁者同样刚解冻、尚未完成临界区，epoch 静止 + CAS 失败 + now-c0 超
 * 窗三条件在解冻后第一个采样点同时成立，原判定以最高置信度签名
 * "hard-lockup signature CONFIRMED" 误报（本机已实证复现：selftest=stall
 * 下 SIGSTOP 全进程 12s 冻结落在窗内，恢复瞬间 FAIL，报文等待时长含冻
 * 结 12s）。判别手段：冻结必然恢复、真冻结永不恢复——停摆签名（相邻两
 * 次采样间隔 > SPINLOCK_ADD_NEON_OBSERVER_GAP_SECONDS，远超窗内单次迭代
 * 成本）检出即重启确认窗（重取 epoch0/c0，冻结时长不再计入）；重启后的
 * 窗内锁行流动/获锁成功 → 按饥饿处理（全局冻结恢复后持锁者微秒级完成临
 * 界区并释放），真冻结 epoch 永不恢复 → 重启后的窗照常确认。反复停摆
 * （一窗内 > SPINLOCK_ADD_NEON_GAP_RESTARTS_MAX 次）= 观察者自身被深度
 * 限流/反复冻结，无法给出冻结判定，以区分性签名 FAIL（不以 CONFIRMED
 * 定罪）。固有边界（与 PR#328 检视同类裁定，保持既有处置）：解冻后持锁者
 * 被持续抢占 >1s 且观察者运行正常时，"持锁者未运行"与"释放写丢失"在
 * 等待者视角不可区分，仍按冻结判决——该形态无冻结时同样存在。 */
static enum SpinlockAddNeonAcquire spinlock_add_neon_acquire(struct SpinlockAddNeonData *d,
                                                             int cpu, uint64_t *stall_ns_out)
{
    const uint64_t limit = (uint64_t)(SPINLOCK_ADD_NEON_STALL_SECONDS * 1e9);
    const uint64_t confirm_limit = (uint64_t)(SPINLOCK_ADD_NEON_CONFIRM_SECONDS * 1e9);
    /* 两条时间基线（PR#328 检视）：
     *   t_entry —— 本次获锁尝试的累计等待基线：自进入 acquire 起永不重置。
     *             报告口径：FROZEN/NO_PROGRESS 的 stall_ns 与 dump_line 的
     *             "LOCK STALL %.3fs" 统一取 now-t_entry（跨饥饿重置窗口求和），
     *             "across %d wait windows (%.1fs total)" 才名实相符。
     *   t0 —— 本窗看门狗预算基线：饥饿重置时重置（判定口径：确认窗判"锁在
     *         流动"后须重算 10s 预算）。原实现报告口径也误取 now-t0，4 窗各
     *         >10s 的 NO_PROGRESS 把 ~11s 的末窗时长谎报成了 total。 */
    const uint64_t t_entry = spinlock_add_neon_now_ns();
    uint64_t t0 = t_entry;
    uint64_t expected = 0;
    uint32_t spins = 0;
    int starvation_resets = 0;
    for (;;) {
        if (__atomic_compare_exchange_n(&d->line->lock_word, &expected, 1,
                                        false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return SPINLOCK_ACQUIRE_OK;
        expected = 0;
        if ((++spins & (SPINLOCK_ADD_NEON_SPIN_CLOCK_PERIOD - 1)) == 0) {
            const uint64_t waited = spinlock_add_neon_now_ns() - t0;
            if (waited > limit) {
                /* epoch/holder 是跨锁共享字段：等待者无锁读并发于持锁者
                 * 临界区写——C++20 中 volatile 无跨线程语义，plain 读-写对
                 * 构成 data race（形式 UB）；RELAXED 原子读消除之，ARM64
                 * 对齐 8B 单拷贝原子、与 plain ldr 同码（行为不变、语义有
                 * 定义，PR#341 检视） */
                uint64_t epoch0 = __atomic_load_n(&d->line->epoch, __ATOMIC_RELAXED);
                uint64_t c0 = spinlock_add_neon_now_ns();
                uint64_t c_prev = c0;
                const uint64_t gap_limit =
                    (uint64_t)(SPINLOCK_ADD_NEON_OBSERVER_GAP_SECONDS * 1e9);
                int gap_restarts = 0;
                bool acquired = false, flowed = false, observer_stalled = false;
                for (;;) {
                    if (__atomic_compare_exchange_n(&d->line->lock_word, &expected, 1,
                                                    false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                        acquired = true;
                        break;
                    }
                    expected = 0;
                    if (__atomic_load_n(&d->line->epoch, __ATOMIC_RELAXED) != epoch0) {
                        flowed = true;
                        break;
                    }
                    const uint64_t now = spinlock_add_neon_now_ns();
                    /* 观察者停摆（PR#328 检视，设计见函数头注释）：
                     * 相邻采样间隔远超窗内单次迭代成本 = 本线程自身未被调度
                     * （全局冻结/CPU 限流），停摆期间的墙钟不得计入确认窗 */
                    if (now - c_prev > gap_limit) {
                        if (gap_restarts >= SPINLOCK_ADD_NEON_GAP_RESTARTS_MAX) {
                            observer_stalled = true;
                            break;
                        }
                        gap_restarts++;
                        epoch0 = __atomic_load_n(&d->line->epoch, __ATOMIC_RELAXED);
                        c0 = now;
                        c_prev = now;
                        continue;
                    }
                    c_prev = now;
                    if (now - c0 > confirm_limit)
                        break; /* epoch 停走 + CAS 失败满确认窗 = 确认冻结 */
                    __asm__ volatile("yield");
                }
                if (acquired) {
                    /* 确认窗内获锁：锁健康，超时是本线程被饿/被冻结后恢复
                     * ——告警后正常继续 */
                    log_warning("spinlock_add_neon: cpu %d waited %.1fs for the lock but "
                                "acquired it during the confirm window — scheduling "
                                "starvation or a process freeze/thaw (not a lock "
                                "freeze); continuing",
                                cpu, (double)waited / 1e9);
                    return SPINLOCK_ACQUIRE_OK;
                }
                if (observer_stalled) {
                    *stall_ns_out = spinlock_add_neon_now_ns() - t_entry;
                    return SPINLOCK_ACQUIRE_OBSERVER_STALLED;
                }
                if (flowed) {
                    if (starvation_resets >= SPINLOCK_ADD_NEON_STARVATION_RESETS_MAX) {
                        *stall_ns_out = spinlock_add_neon_now_ns() - t_entry;
                        return SPINLOCK_ACQUIRE_NO_PROGRESS;
                    }
                    starvation_resets++;
                    log_warning("spinlock_add_neon: cpu %d waited %.1fs but the lock line "
                                "kept flowing (epoch advanced) — scheduling starvation, "
                                "not a freeze; wait budget reset (%d/%d)",
                                cpu, (double)waited / 1e9, starvation_resets,
                                SPINLOCK_ADD_NEON_STARVATION_RESETS_MAX);
                    t0 = spinlock_add_neon_now_ns();
                    spins = 0;
                    continue;
                }
                *stall_ns_out = spinlock_add_neon_now_ns() - t_entry;
                return SPINLOCK_ACQUIRE_FROZEN;
            }
        }
        __asm__ volatile("yield");
    }
}

static void spinlock_add_neon_release(struct SpinlockAddNeonData *d)
{
    __atomic_store_n(&d->line->lock_word, 0, __ATOMIC_RELEASE);
}

/* 锁行 64B 全量 dump（五案现场录像格式）；verdict 由两段式判定给出（冻结
 * /进展异常的区分性签名） */
static void spinlock_add_neon_dump_line(struct SpinlockAddNeonData *d, int cpu,
                                        uint64_t stall_ns, const char *verdict)
{
    /* 逐元素 RELAXED 原子读（PR#341 检视）：dump 时其余 worker
     * 仍在自旋/CAS/临界区——lock_word/epoch/holder 与本读并发，plain
     * volatile 读是 data race 的一半（形式 UB）；ARM64 对齐 8B 单拷贝原子，
     * RELAXED 与 plain ldr 同码 */
    const volatile uint64_t *q = reinterpret_cast<const volatile uint64_t *>(d->line);
    uint64_t v[8];
    for (int i = 0; i < 8; ++i)
        v[i] = __atomic_load_n(&q[i], __ATOMIC_RELAXED);
    log_error("spinlock_add_neon: LOCK STALL %.3fs on cpu %d — %s; lock cacheline 64B dump: "
              "lock_word=0x%016lX epoch=%lu holder=%lu magic=0x%016lX "
              "pad=[0x%016lX,0x%016lX,0x%016lX,0x%016lX]%s",
              (double)stall_ns / 1e9, cpu, verdict,
              (unsigned long)v[0], (unsigned long)v[1], (unsigned long)v[2],
              (unsigned long)v[3], (unsigned long)v[4], (unsigned long)v[5],
              (unsigned long)v[6], (unsigned long)v[7],
              v[3] == SPINLOCK_ADD_NEON_MAGIC
                  ? " (magic intact — 8-byte window signature)"
                  : " (MAGIC ALSO CORRUPTED — wider blast radius)");
}

static int spinlock_add_neon_run(struct test *test, int cpu)
{
    auto *d = static_cast<SpinlockAddNeonData *>(test->data);
    const int slot = cpu % SPINLOCK_ADD_NEON_SLOTS;
    const float *in = d->inputs + (size_t)slot * SPINLOCK_ADD_NEON_FLOATS;
    const float *gold = d->golden + (size_t)slot * SPINLOCK_ADD_NEON_FLOATS;
    float out[SPINLOCK_ADD_NEON_FLOATS];
    uint64_t logged = 0, iter = 0;

    do {
        /* A: NEON 实算负载（锁外，本线程私有 out） */
        spinlock_add_neon_kernel(in, out);

        /* B: 获锁——两段式看门狗（通道C：10s 超时 + 1s 确认窗，冻结/饥饿区分） */
        uint64_t stall_ns = 0;
        const enum SpinlockAddNeonAcquire acq = spinlock_add_neon_acquire(d, cpu, &stall_ns);
        if (acq != SPINLOCK_ACQUIRE_OK) {
            const char *verdict;
            /* 通道C FAIL = 保留 logverif 文件的迹象（供回看失败前的逐次
             * 迭代记录）；经 note_fail 统一记账（契约见其注释） */
            spinlock_add_neon_note_fail(d);
            /* verdict/report 只断言证据支持的结论（PR#341 检视）：
             * 确认窗能证明的是签名（epoch 静止 + CAS 持续失败），不能证明
             * 根因——「释放写丢失」与「持锁者整窗未运行（被抢占/限流，如
             * selftest=stall 的合法 sleep 持锁者）」在等待者视角不可区分
             * （PR#328 检视裁定该形态仍按冻结判决），报文按
             * 签名 + 候选因陈述，不断言硬件级根因 */
            if (acq == SPINLOCK_ACQUIRE_FROZEN)
                verdict = "hard-lockup signature CONFIRMED (epoch static across the "
                          "confirm window, CAS failing — lock word stuck at 1; "
                          "root cause not asserted: lost release write vs holder "
                          "descheduled/preempted across the window are "
                          "indistinguishable from a waiter's view)";
            else if (acq == SPINLOCK_ACQUIRE_OBSERVER_STALLED)
                verdict = "freeze verdict withheld (this thread could not observe "
                          "continuously — confirm window repeatedly invalidated by "
                          "observer stall gaps)";
            else
                verdict = "progress anomaly (lock line kept flowing but this thread "
                          "never acquired — sustained starvation?)";
            spinlock_add_neon_dump_line(d, cpu, stall_ns, verdict);
            if (acq == SPINLOCK_ACQUIRE_FROZEN)
                report_fail_msg("spinlock_add_neon: lock acquisition stalled %.1fs (>%.0fs) "
                                "on cpu %d, frozen CONFIRMED (lock line static for >=%.0fs, "
                                "CAS failing) — hard-lockup signature; root cause not "
                                "asserted (lost release write vs holder descheduled/"
                                "preempted across the window are indistinguishable from "
                                "a waiter's view)",
                                (double)stall_ns / 1e9, SPINLOCK_ADD_NEON_STALL_SECONDS, cpu,
                                SPINLOCK_ADD_NEON_CONFIRM_SECONDS);
            else if (acq == SPINLOCK_ACQUIRE_OBSERVER_STALLED)
                report_fail_msg("spinlock_add_neon: lock acquisition stalled %.1fs on "
                                "cpu %d with >=%d observer stall gaps (>=%.0fms each) "
                                "invalidating the confirm window — cannot confirm a "
                                "hard lockup from interrupted observation (cgroup "
                                "freezer/CRIU checkpoint/VM live-migration pause or "
                                "severe CPU throttling produce this signature)",
                                (double)stall_ns / 1e9, cpu,
                                SPINLOCK_ADD_NEON_GAP_RESTARTS_MAX + 1,
                                SPINLOCK_ADD_NEON_OBSERVER_GAP_SECONDS * 1e3);
            else
                report_fail_msg("spinlock_add_neon: lock line kept flowing but this thread "
                                "failed to acquire across %d wait windows (%.1fs total) on "
                                "cpu %d — sustained starvation or progress anomaly, not a "
                                "lock freeze",
                                SPINLOCK_ADD_NEON_STARVATION_RESETS_MAX + 1,
                                (double)stall_ns / 1e9, cpu);
            return EXIT_FAILURE;
        }

        /* C: 临界区——锁字同行元数据 + 计数对（通道B 靶，同窗更新）。
         * epoch/holder 写走 RELAXED 原子（PR#341 检视）：锁内单
         * 写者，但等待者确认窗/dump 的无锁读并发于本写——C++20 下 plain
         * volatile 写+他线程读是 data race（形式 UB），原子化写侧消除之；
         * RELAXED 保持锁内单写者语义（同步已由 lock_word 的
         * acquire/release 承担），ARM64 对齐 8B 与原 plain str 同码。
         * shared_counter/local_counts 无锁外读者（cleanup 在 join 后
         * quiescent 读），plain 计数保持。epoch 经显式原子读-改-写（不用
         * volatile ++，C++20 弃用复合操作） */
        d->shared_counter[0]++;
        d->local_counts[slot]++;
        __atomic_store_n(&d->line->holder, (uint64_t)cpu, __ATOMIC_RELAXED);
        __atomic_store_n(&d->line->epoch,
                         __atomic_load_n(&d->line->epoch, __ATOMIC_RELAXED) + 1,
                         __ATOMIC_RELAXED);

        /* 自检 stall：首个进临界区的线程持锁 15s（> 看门狗 10s + 确认窗 1s
         * + 裕量——模拟冻结须长于两段式全检出时延，否则确认窗内其他线程
         * CAS 成功会被判为饥饿、阳性击穿），其他线程看门狗应触发。
         * sleep 返回值不可弃（PR#341 检视）：POSIX sleep 被任意
         * 已捕获信号打断即返回未睡秒数——持锁提前释放会让确认窗内 CAS
         * 成功、通道 C 阳性丢失，部署自检静默假绿（与 init 的 fail-loudly
         * 门槛不对等）。循环睡满标称秒数 */
        if (d->selftest == 1 && d->stall_armed == 0) {
            d->stall_armed = 1;
            unsigned remain = SPINLOCK_ADD_NEON_STALL_SLEEP_SECONDS;
            while (remain > 0)
                remain = sleep(remain);
        }

        spinlock_add_neon_release(d);

        /* D: 验证（通道A） */
        const bool ok = memcmp(out, gold, sizeof(out)) == 0;

        /* E: 逐次验证日志（绿 PASS / 红 FAIL；每线程上限，失败行永不受限） */
        if (d->log_fd >= 0 && (ok ? logged < SPINLOCK_ADD_NEON_LOG_MAX : true)) {
            char buf[512];
            int n = snprintf(buf, sizeof buf,
                    "iter=%lu cpu=%d in=[%+.6f,%+.6f,%+.6f,%+.6f] "
                    "out=[%+.6f,%+.6f,%+.6f,%+.6f] result=\033[%dm%s\033[0m\n",
                    (unsigned long)iter, cpu,
                    in[0], in[1], in[2], in[3], out[0], out[1], out[2], out[3],
                    ok ? 32 : 31, ok ? "PASS" : "FAIL");
            /* snprintf 截断时返回完整写入所需长度（可 > sizeof buf）——钳制
             * 到 buf 内实际内容，防 write 越界读栈缓冲（护栏防御未来调大
             * 字段/格式串）。buf=512 的依据（PR#328 检视）：健康
             * 输出 |out|≤~3e20 → %+.6f 每元素 ≈29 字符、行长 ~230B；但
             * FAIL 行正是 SDC 场景——位型可落 FLT_MAX 级（%+.6f ≈45 字符/
             * 元素），out 4 元素全落该域行长 ~300B、in+out 8 元素全落（共
             * 享输入行被同击）绝对最坏 457B（本机探针 2026-10-30 实测）。
             * 256 会截断砍掉尾部 result=FAIL 标记、破「绿 PASS 红 FAIL」日
             * 志契约；512 覆盖绝对最坏并留余量——调整格式串时须重算此界 */
            if (n < 0)
                n = 0;
            else if (n > (int)sizeof buf - 1)
                n = (int)sizeof buf - 1;
            if (n > 0 && write(d->log_fd, buf, (size_t)n) < 0) { /* best effort */ }
            if (ok)
                logged++;
        }

        if (!ok) {
            /* 数据 miscompare = 保留文件的迹象（红行是否实际写出无关——
             * logverif 降级禁用时同样计数；经 note_fail 统一记账） */
            spinlock_add_neon_note_fail(d);
            log_error("spinlock_add_neon: data miscompare cpu=%d iter=%lu slot=%d — "
                      "full 64-float in/out/golden dump:",
                      cpu, (unsigned long)iter, slot);
            for (int v = 0; v < SPINLOCK_ADD_NEON_FLOATS; v += 4)
                log_error("  [%02d] in=[%+.6e,%+.6e,%+.6e,%+.6e] out=[%+.6e,%+.6e,%+.6e,%+.6e] "
                          "golden=[%+.6e,%+.6e,%+.6e,%+.6e]",
                          v, in[v], in[v+1], in[v+2], in[v+3],
                          out[v], out[v+1], out[v+2], out[v+3],
                          gold[v], gold[v+1], gold[v+2], gold[v+3]);
            report_fail_msg("spinlock_add_neon: NEON result != golden (cpu %d iter %lu)",
                            cpu, (unsigned long)iter);
            return EXIT_FAILURE;
        }
        iter++;
        /* 循环条件直连 test_time_condition 而非 TEST_LOOP/test_loop_condition
         * ——locks/spinlock 家族统一习语（locks.cpp:68、spinlock_cross_socket.cpp:78
         * 同款 do-while；全仓 248 文件直连 vs 218 文件 TEST_LOOP）。后果须知情
         * （PR#328 检视）：--inject-idle 的 usleep 注入只存在于
         * test_loop_condition（上游 8d510c9c 有意设计："existing tests that
         * directly use test_time_condition() won't be affected by the idle
         * injection"），但时长折算是框架全局行为——本测试在 inject-idle 实验
         * 中为 (1-idle_rate)×时长 × 100% 占空比，与 TEST_LOOP 用例不一致。
         * 不改为 test_loop_condition 的依据：① 该函数每 N 次迭代才被调用一次
         * （TEST_LOOP 粒度），inner_loop_count/骨折切片随之为 ×N（本测试切片
         * 行为——含 logverif 每切片文件与行预算——经多轮检视校准，重构
         * 即重开已收敛议题）；② 独行修复无战役收益：直连条件是 248 文件的
         * 框架级语义，占空比一致性须框架层统一解决。此边界在此声明而非
         * 静默存在 */
    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int spinlock_add_neon_init(struct test *test)
{
    auto *d = static_cast<SpinlockAddNeonData *>(malloc(sizeof(SpinlockAddNeonData)));
    if (!d)
        return EXIT_FAILURE;
    memset(d, 0, sizeof *d);

    /* knob 读取（init/main 线程，random_access_sweep 纪律：缓存进 test->data，
     * run 线程禁止直接读）；必须用 testspecific 变体——-O testid.key=value
     * 的键按测试 id 定域，NULL-test 裸键查不到 */
    /* logverif / logverif_retain 是 0/1 布尔旋钮：全值域白名单 fail
     * loudly 而非静默钳 0/1（PR#341 检视，selftest 旋钮同段纪律）——
     * retain 静默钳 1 会「以为没开实际开了」（无
     * 界堆积照发），反向错值静默钳 0 则审计文件照删。必须走 string
     * 变体 + strcmp 白名单而非 int 变体 + 整数域守卫：非数值串
     * （=on/=true…）经 knob_value_integer 的 strtoull 解析失败后无
     * 告警返回 value_if_not_present=0（framework/test_knobs.cpp:113-129
     * Defaulted 路径），整数域守卫（(unsigned)raw > 1）拦不住——恰是
     * 「反向错值静默钳 0 → 审计文件照删」路径（PR#341 检视揭示
     * int 版守卫只闭合整数子域）。键名 typo（logverif_rettain）框架
     * 侧静默忽略未知键（test_knobs 无 set-vs-used 交叉核对——框架级
     * 已知缺口）测试侧不可见，不在此防御范围 */
    const char *logverif_s = get_testspecific_knob_value_string(test, "logverif", "0");
    /* logverif_retain（0/1，默认 0）：绿跑是否保留逐次验证日志。默认删
     * （修 #9/#14 的长战役绿跑堆积写满 /var/tmp）；=1 时绿跑也保留——
     * golden 为 init 同源内核预计算，init/run 一致同错的系统性错误三通
     * 道全绿，logverif 行是绿跑期唯一可人工审计的计算值迹（PR#341
     * 检视），需要审计时显式打开；代价是回到无界堆积（见 header
     * 生命周期披露），长战役慎用 */
    const char *logverif_retain_s = get_testspecific_knob_value_string(test, "logverif_retain", "0");
    if (strcmp(logverif_s, "0") != 0 && strcmp(logverif_s, "1") != 0) {
        log_error("spinlock_add_neon: invalid logverif knob value \"%s\" "
                  "(valid: 0 or 1, default 0) — failing loudly so a wrong "
                  "value cannot silently flip the verification-log setting "
                  "(selftest knob discipline)", logverif_s);
        /* 与 selftest 错值路径同约束：仅 d 本身已分配，就地释放返回，
         * 不能 goto fail——会跨越下方 floats_total 的初始化（C++ 禁止） */
        free(d);
        return EXIT_FAILURE;
    }
    if (strcmp(logverif_retain_s, "0") != 0 && strcmp(logverif_retain_s, "1") != 0) {
        log_error("spinlock_add_neon: invalid logverif_retain knob value "
                  "\"%s\" (valid: 0 or 1, default 0) — failing loudly so a "
                  "wrong value cannot silently flip the audit-retention "
                  "setting (selftest knob discipline)", logverif_retain_s);
        free(d);
        return EXIT_FAILURE;
    }
    /* 组合交叉校验（PR#341 检视）：logverif_retain=1 而 logverif=0 时
     * retain 静默无效——retain 的唯一消费点是 cleanup 的绿跑豁免条件，
     * 该条件处于 if (d->log_fd >= 0) 块内，而 log_fd 仅在下方
     * if (d->logverif) 才打开：logverif=0 时恒 -1、豁免永不可达。实测
     * 复现（52ed7ab9 基线）：单开 retain=1 绿跑 exit: pass 且 /var/tmp
     * 零文件——操作员按 header「需要审计时显式打开」的说明以为审计迹
     * 在留档而实际一个文件都不存在，与上方两旋钮各自的错值 fail
     * loudly 同一 typo/误解危害类；init 手中同时持有两值，交叉校验
     * 闭合。就地 free 返回不 goto（同上方错值路径约束） */
    if (strcmp(logverif_retain_s, "1") == 0 && strcmp(logverif_s, "1") != 0) {
        log_error("spinlock_add_neon: logverif_retain=1 requires logverif=1 "
                  "(retention only exempts the per-slice verification log "
                  "from green-run cleanup; without logverif the log and its "
                  "files are never created) — failing loudly so the "
                  "combination cannot silently no-op");
        free(d);
        return EXIT_FAILURE;
    }
    d->logverif = strcmp(logverif_s, "1") == 0;
    d->logverif_retain = strcmp(logverif_retain_s, "1") == 0;
    const char *st = get_testspecific_knob_value_string(test, "selftest", "off");
    /* 未知取值 fail loudly 而非静默映射 0（igzip level knob 先例：静默钳
     * 0 会让部署自检纪律被一个 typo 无声击穿——selftest=Stall 之类错值
     * 跑满 -t 时长绿 pass，与阳性自检是否真正执行在时耗上无法区分，
     * PR#328 检视） */
    if (strcmp(st, "off") == 0 || strcmp(st, "0") == 0)
        d->selftest = 0;
    else if (strcmp(st, "stall") == 0 || strcmp(st, "1") == 0)
        d->selftest = 1;
    else if (strcmp(st, "data") == 0 || strcmp(st, "2") == 0)
        d->selftest = 2;
    else {
        log_error("spinlock_add_neon: invalid selftest knob value \"%s\" "
                  "(valid: off|stall|data or 0|1|2, default off) — failing "
                  "loudly so a typo cannot silently skip the deployment "
                  "self-test", st);
        /* 此处仅 d 本身已分配（memset 后各指针为 NULL），就地释放返回；
         * 不能 goto fail——会跨越下方 floats_total 的初始化（C++ 禁止） */
        free(d);
        return EXIT_FAILURE;
    }
    d->stall_armed = 0;
    d->log_fd = -1;

    /* selftest=stall 前置条件 fail loudly（PR#328 检视）：通道 C
     * 阳性需要第二个线程在 10s 看门狗 + 1s 确认窗下等待——单线程时唯一
     * 线程自己持锁睡 15s 后自行 release、循环至 -t 耗尽绿 pass，通道 C
     * 永不触发（假绿，与 selftest 错值静默 off 同一纪律缺口：前置条件
     * 只写在注释里 = 本文件 :288 自己批判的「纪律被静默击穿」形态）；
     * 时长不足时阳性 FAIL(~11s) 与持锁排空(~15s) 落在标称窗口之外，
     * 外部超时包装下证据丢失。线程数取 thread_count()（公开 API，
     * mce_check 先例；fork/slice 子进程 init 前 sApp->thread_count 已
     * 归一为本 slice 实际 worker 数），时长取 sApp->current_test_duration
     * （run_one_test 在 init_internal 前设置，含 -t/每测试覆盖/上下限裁剪
     * 与 --inject-idle 折算后的生效值）。 */
    if (d->selftest == 1) {
        if (thread_count() < 2) {
            log_error("spinlock_add_neon: selftest=stall needs >=2 test threads to "
                      "watch the lock (channel C: a second thread must spin under the "
                      "10s watchdog while the holder sleeps %ds) — got %d thread(s); "
                      "a single thread would sleep, self-release, and green-pass to "
                      "-t with channel C never firing; failing loudly instead of "
                      "producing that false green (use -n >=2 / --cpuset with >=2 "
                      "cpus, or selftest=data which is single-thread safe)",
                      SPINLOCK_ADD_NEON_STALL_SLEEP_SECONDS, thread_count());
            free(d);
            return EXIT_FAILURE;
        }
        if (sApp->current_test_duration
                < std::chrono::milliseconds(SPINLOCK_ADD_NEON_STALL_MIN_TEST_MS)) {
            log_error("spinlock_add_neon: selftest=stall needs a test duration >= "
                      "%dms so the positive FAIL (~%ds: watchdog + confirm window) "
                      "and the holder drain (~%ds) land inside the nominal window — "
                      "got %dms; failing loudly instead of producing a late FAIL "
                      "that external timeout wrappers may kill (recommended: "
                      "-t 25000)",
                      SPINLOCK_ADD_NEON_STALL_MIN_TEST_MS,
                      (int)(SPINLOCK_ADD_NEON_STALL_SECONDS
                            + SPINLOCK_ADD_NEON_CONFIRM_SECONDS),
                      SPINLOCK_ADD_NEON_STALL_SLEEP_SECONDS,
                      (int)sApp->current_test_duration.count());
            free(d);
            return EXIT_FAILURE;
        }
    }

    const size_t floats_total = (size_t)SPINLOCK_ADD_NEON_SLOTS * SPINLOCK_ADD_NEON_FLOATS;
    d->inputs = static_cast<float *>(aligned_alloc(64, floats_total * sizeof(float)));
    d->golden = static_cast<float *>(aligned_alloc(64, floats_total * sizeof(float)));
    d->line = static_cast<SpinlockAddNeonLockLine *>(aligned_alloc(64, sizeof(SpinlockAddNeonLockLine)));
    /* C11 7.22.3.1：aligned_alloc 的 size 须为 alignment 整数倍——
     * sizeof(uint64_t)=8 违反（glibc 不检查故可跑，属潜伏 UB，严格分配器
     * 将返回 NULL 触发假 FAIL）；本行 cacheline 对齐故取 64 */
    d->shared_counter = static_cast<uint64_t *>(aligned_alloc(64, 64));
    d->local_counts = static_cast<uint64_t *>(calloc(SPINLOCK_ADD_NEON_SLOTS, sizeof(uint64_t)));
    if (!d->inputs || !d->golden || !d->line || !d->shared_counter || !d->local_counts) {
        log_error("spinlock_add_neon: allocation failure");
        goto fail;
    }

    /* 输入：init 一次生成，所有线程共享只读（框架 RNG，-s 可复现） */
    for (int s = 0; s < SPINLOCK_ADD_NEON_SLOTS; ++s) {
        float *dst = d->inputs + (size_t)s * SPINLOCK_ADD_NEON_FLOATS;
        for (int i = 0; i < SPINLOCK_ADD_NEON_FLOATS; ++i)
            /* 值域实为 [-1, +1]：double→float 就近舍入使商 ∈ (1-2^-25, 1) 的
             * 64 个整数（x ∈ [2147483584, 2147483647]，含 2^31-64 平局点
             * ties-to-even）产生 +1.0f，概率 2^-26；-1 仅 x=INT32_MIN 精确
             * 取得。本机全量探针实证（正 int32 域逐值扫描，2026-10-31）。
             * golden 同源预计算（同 kernel 同输入）与输出界分析（按 |x|<=1，
             * :35/:78）均按闭区间陈述、不受影响（PR#328 检视）。 */
            dst[i] = (float)((int32_t)random32() / 2147483648.0); /* [-1, +1] */
        /* golden：同源内核预计算（同机字节确定） */
        spinlock_add_neon_kernel(dst, d->golden + (size_t)s * SPINLOCK_ADD_NEON_FLOATS);
    }

    /* 自检 data：毒化每个 slot 的 golden 首元素（cpu 无关——任意 cpuset 布局
     * 都会命中）。位级 XOR 最低尾数位：对任意位型（含 0/±Inf/NaN 及
     * |x|>=2^24 的 ulp>1 值域）无条件改变，memcmp 必检出——+1.0f 在
     * |x|>=2^24 时舍入后位型不变（本机探针：10^6 slot 有 164 个的
     * out[0] 落在该域，0.016%），毒化会静默失效 */
    if (d->selftest == 2)
        for (int s = 0; s < SPINLOCK_ADD_NEON_SLOTS; ++s) {
            float *g = &d->golden[(size_t)s * SPINLOCK_ADD_NEON_FLOATS];
            uint32_t bits;
            memcpy(&bits, g, sizeof bits);
            bits ^= 1;
            memcpy(g, &bits, sizeof bits);
        }

    /* 锁行初始化 */
    d->line->lock_word = 0;
    d->line->epoch = 0;
    d->line->holder = ~0ull;
    d->line->magic = SPINLOCK_ADD_NEON_MAGIC;
    for (int i = 0; i < 4; ++i)
        d->line->pad[i] = SPINLOCK_ADD_NEON_MAGIC ^ (uint64_t)(i + 1);
    d->shared_counter[0] = 0;

    if (d->logverif) {
        /* 首选 /var/tmp/spinlock_add_neon_<pid>.log；EEXIST 时以
         * <pid>-<n>.log 递增后缀重试（PR#341 检视）——重试仍全
         * O_EXCL，「绝不写已存在文件」的防预植语义不变（注入伪 PASS/
         * 稀释 FAIL 行需向已存在文件追加，O_EXCL 恒拒绝；后缀名可预测
         * 不构成新攻击面：防御目标是拒绝写已存在文件而非隐藏文件名，
         * 预植 <pid>-1.log 与预植 <pid>.log 同等难度且同等被拒）。
         * 撞名走后缀的场景（retain=1 及红跑取证文件无上限留档时）有二：
         * fork 模式长战役 pid 空间回绕（本机 pid_max=4194304，实测节奏
         * ~240 切片/s ≈ 4.8h/圈）后每圈同 pid 切片命中上一圈保留文件，
         * 若直接降级则该 pid 的 logverif 在整个战役期持续死亡，后缀重试
         * 使每圈退一格，上限 SPINLOCK_ADD_NEON_LOG_OPEN_TRIES（~4.8h/圈
         * × 64 ≈ 13 天连续战役）耗尽才降级；-f no（nofork 调试模式，
         * PR#341 检视）同 pid 每切片必撞——框架对每切片同进程
         * 重跑本 init、无需 pid 回绕，烧穿速率 = fracture 率（实测本机
         * 36-65 切片/s 随载，65 格 1-3s 烧穿），耗尽后逐切片降级、rm
         * 旧文件即可恢复。cleanup 对应：unlink 的是本轮实际打开的
         * log_path（含后缀），不动上一圈遗留。 */
        for (int attempt = 0; attempt <= SPINLOCK_ADD_NEON_LOG_OPEN_TRIES; ++attempt) {
            if (attempt == 0)
                snprintf(d->log_path, sizeof d->log_path,
                         "/var/tmp/spinlock_add_neon_%d.log", (int)getpid());
            else
                snprintf(d->log_path, sizeof d->log_path,
                         "/var/tmp/spinlock_add_neon_%d-%d.log", (int)getpid(), attempt);
            d->log_fd = open(d->log_path, O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_NOFOLLOW, 0644);
            if (d->log_fd >= 0 || errno != EEXIST)
                break; /* 成功；或非「文件已存在」形态（只读 fs、SELinux、
                        * 预植 symlink 的 ELOOP…）——后缀重试无意义 */
        }
        if (d->log_fd < 0) {
            /* logverif 是 opt-in 诊断附属、不是检测契约：环境不可用（只读
             * /var/tmp、SELinux 拒绝、O_NOFOLLOW 命中预植 symlink 的
             * ELOOP，或后缀重试耗尽——同 pid 遗留文件数已超
             * SPINLOCK_ADD_NEON_LOG_OPEN_TRIES，rm 旧文件即可恢复；
             * -f no 同 pid 每切片撞名即此形态，见上）时
             * 降级为禁用日志继续跑——检测通道 A/B/C 不受影响，环境问题
             * 不与 SDC 检出共享退出码、不污染战役自动分诊（cpufreq/thermal
             * 缺失优雅降级同哲学，PR#328 检视）。用 log_warning
             * 而非 log_error：后者会把本用例标 Failed（message_common_check
             * 对 'E' 前缀调 logging_mark_thread_failed）。可见性边界
             * （PR#341 检视）：'W' 级告警在默认 verbosity 不
             * 显示（logging status_level('W')=VERBOSE(1)），须 -v——
             * nofork+retain 长跑烧穿后逐切片降级在默认输出下静默，操作
             * 员要看到降级须 -v（实测同场景默认 0 条、-v 6s 348 条）。 */
            log_warning("spinlock_add_neon: cannot open %s (errno %d: %s) — "
                        "per-verification log disabled, test continues "
                        "(logverif is a diagnostic aid, not a detection "
                        "contract)",
                        d->log_path, errno, strerror(errno));
            d->log_fd = -1;
        }
    }

    test->data = d;
    return EXIT_SUCCESS;

fail:
    /* init 失败时框架不调用 test_cleanup（sandstone_run.cpp 的 init 失败
     * 分支提前 return）——此处自行释放，失败经返回值传播（ret > 0 →
     * Failed）。不用 report_fail_msg：它经 report_fail_common 异步取消
     * 调用线程且 noreturn，而 test_init/cleanup 都在控制线程执行。 */
    free(d->inputs);
    free(d->golden);
    free(d->line);
    free(d->shared_counter);
    free(d->local_counts);
    free(d);
    return EXIT_FAILURE;
}

static int spinlock_add_neon_cleanup(struct test *test)
{
    auto *d = static_cast<SpinlockAddNeonData *>(test->data);
    int ret = EXIT_SUCCESS;
    if (d) {
        /* 通道B：Σ 逐线程获锁数 == 共享计数器（数据行写丢失检测；
         * add-only 无抵消方向；优雅收尾下计数对同窗更新，无逻辑假阳性） */
        uint64_t total = 0;
        for (int i = 0; i < SPINLOCK_ADD_NEON_SLOTS; ++i)
            total += d->local_counts[i];
        if (total != d->shared_counter[0]) {
            log_error("spinlock_add_neon: shared counter %lu != sum of per-thread "
                      "acquisitions %lu (diff %ld) — lost/frozen update on the "
                      "lock-protected data line",
                      (unsigned long)d->shared_counter[0], (unsigned long)total,
                      (long)d->shared_counter[0] - (long)total);
            /* 失败经返回值传播（框架: cleanup 返回 > EXIT_SUCCESS → Failed）。
             * 不用 report_fail_msg：它经 report_fail_common 异步取消调用
             * 线程且 noreturn，而 cleanup 在控制线程执行——其后的 close/
             * free 与 finish 收尾路径将不可达（memcpy_rewr 先例同样是
             * 先释放后报告；这里直接以返回值收尾，退出码路径完整）。 */
            ret = EXIT_FAILURE;
        }

        /* 通道B 补充：锁行元数据主动对账（PR#328 检视）。epoch 与
         * shared_counter 构造上相等——同一临界区内各增一、持锁单写者、
         * cleanup 前框架已 join 全部 worker（run_threads_in_parallel），
         * quiescent 无撕裂窗口，零假阳性。magic/pad init 后恒定。瞬态一致
         * 性故障只丢锁行元数据写而 lock_word 完好时（非挂死形态：无挂死
         * →通道C 盲、计数对在数据行→通道B 平衡、NEON 数据完好→通道A
         * 通过）此前三通道全绿漏检——本对账闭合该盲区。holder 为最后
         * 持锁者 cpu（值不确定），仍仅作 dump 现场字段。 */
        if (d->line->epoch != d->shared_counter[0]) {
            log_error("spinlock_add_neon: lock-line epoch %lu != shared counter "
                      "%lu (diff %ld) — lost update on the lock cacheline "
                      "metadata (8-byte window, lock_word intact)",
                      (unsigned long)d->line->epoch,
                      (unsigned long)d->shared_counter[0],
                      (long)d->line->epoch - (long)d->shared_counter[0]);
            ret = EXIT_FAILURE;
        }
        if (d->line->magic != SPINLOCK_ADD_NEON_MAGIC) {
            log_error("spinlock_add_neon: lock-line magic 0x%016lX != expected "
                      "0x%016lX — lock cacheline fingerprint corrupted",
                      (unsigned long)d->line->magic,
                      (unsigned long)SPINLOCK_ADD_NEON_MAGIC);
            ret = EXIT_FAILURE;
        }
        for (int i = 0; i < 4; ++i) {
            if (d->line->pad[i] !=
                    (SPINLOCK_ADD_NEON_MAGIC ^ (uint64_t)(i + 1))) {
                log_error("spinlock_add_neon: lock-line pad[%d] 0x%016lX != "
                          "expected 0x%016lX — lock cacheline fingerprint "
                          "corrupted",
                          i, (unsigned long)d->line->pad[i],
                          (unsigned long)(SPINLOCK_ADD_NEON_MAGIC ^ (uint64_t)(i + 1)));
                ret = EXIT_FAILURE;
            }
        }
        if (d->log_fd >= 0) {
            close(d->log_fd);
            /* 绿跑 unlink（PR#341 检视）：fracture-
             * per-fork 架构下每子进程各建一个 <pid>.log、行预算随 fork 重
             * 置，长战役绿跑文件线性堆积直至写满 /var/tmp（实测 3s 单
             * 线程 718 文件/29.2MB）。保守删除条件：通道B 对账全绿
             * （ret==EXIT_SUCCESS）∧ 全程零红 FAIL 行/FAIL 迹象
             * （log_fail_lines==0——数据 miscompare 与通道C FAIL 经
             * note_fail 统一计入，新增 FAIL 路径的记账契约见其注释）∧
             * 非 selftest（阳性自检必 FAIL，取证文件保留）∧ 非
             * logverif_retain（审计旋钮，PR#341 检视——绿跑的
             * 人工审计迹需求与堆积治理由显式旋钮分交操作员）——任何检出
             * 迹象都保留文件；log_fd>=0 保证只删本轮自有文件（pid 复用
             * EEXIST 降级时不动上一轮遗留）。unlink 失败 best-effort
             * log_warning 不升格为检测失败（诊断附属哲学同 init 打开降
             * 级路径） */
            if (ret == EXIT_SUCCESS && d->selftest == 0 && d->logverif_retain == 0 &&
                    __atomic_load_n(&d->log_fail_lines, __ATOMIC_RELAXED) == 0 &&
                    unlink(d->log_path) != 0)
                log_warning("spinlock_add_neon: cannot unlink green-run log %s "
                            "(errno %d: %s) — best effort, test result unaffected",
                            d->log_path, errno, strerror(errno));
        }
        free(d->inputs);
        free(d->golden);
        free(d->line);
        free(d->shared_counter);
        free(d->local_counts);
        free(d);
        test->data = nullptr;
    }
    return ret;
}

DECLARE_TEST(spinlock_add_neon, "Spinlock liveness + coherence SDC probe: NEON vfma compute "
             "under a global CAS spinlock with a 10s acquisition-stall watchdog (hard-lockup "
             "signature, 64B lock-line dump), shared-counter sum check, and byte-exact golden "
             "verification; per-verification log with green PASS / red FAIL")
    .test_init    = spinlock_add_neon_init,
    .test_run     = spinlock_add_neon_run,
    .test_cleanup = spinlock_add_neon_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
