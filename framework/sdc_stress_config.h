/*
 * Copyright 2026 ISCAS.
 * SPDX-License-Identifier: Apache-2.0
 *
 * sdc_stress_config.h — 统一的 FP/向量/矩阵业务用例压力配置层。
 *
 * 目标: 把 eigen/openblas/sleef/pocketfft/gather/fma/sve 等目录下用例的
 * 尺寸参数统一成「一条解析链 + 一套环境自适应默认值 + 一套校验规则」，
 * 无需重新编译即可调整测试压力。
 *
 * 优先级链(高 → 低):
 *   1. 命令行 -O testid.knob=value       (框架 test_knobs 机制, 最高优先)
 *   2. 配置文件 (SDC_STRESS_CONFIG 环境变量指定的路径, 缺省
 *      ./sdc_stress.conf)                 (在 -O 解析前预载, -O 覆盖之)
 *   3. 环境变量 SDC_<TESTID>_<KNOB>=value (缺省配置文件不存在时的兜底)
 *   4. 用例内置默认值 (可用环境自适应函数动态计算)
 *
 * 用例侧的使用方式(全部只依赖本头文件 + test_knobs.h):
 *
 *   #include "sdc_stress_config.h"
 *   ...
 *   // 尺寸类旋钮: 默认按物理内存自适应, 显式配置则 clamp 到安全范围
 *   int mdim = sdc_knob_int_clamped(test, "mdim", 0, 16, 8192);
 *   if (mdim == 0)
 *       mdim = sdc_auto_square_dim(8, 4, 6.0, 1024, 4096);
 *
 * 内存自适应模型 (sdc_auto_square_dim):
 *   N = sqrt(phys_mem * mem_fraction / (bytes_per_elem * matrices_per_thread
 *                                        * rss_ratio * nthreads))
 *   —— 与 eigen_gemm_double_max_arm.choose_dim / eigen_svd_cdouble_sve.
 *   size_matrix 同一套模型, 各用例通过参数表达自己的 RSS 结构。
 *
 * 校验:
 *   - sdc_knob_int_clamped: 无效值(非数字/越界)时 report_fail_msg 报出
 *     期望范围并 clamp, 杜绝静默吞掉错误配置。
 *   - 配置文件格式错误在加载阶段(sandstone_opts.cpp)以 EX_USAGE 退出。
 */

#ifndef SDC_STRESS_CONFIG_H
#define SDC_STRESS_CONFIG_H

#include <sandstone.h>
#include <test_knobs.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <map>
#include <string>
#include <unistd.h>

/* NUMA 探测/AR 与联合求解的 per-node 分支仅 ARM64-Linux(spec §5 rule 5)。
 * x86 侧 sdc_mem_family 仅作 sdc_joint_args 的占位成员(无 AR 语义,
 * 与 sdc_numa.h 的定义互斥不冲突)。 */
#if defined(__aarch64__) && defined(__linux__)
# include "sdc_numa.h"
# include <sched.h>
#else
enum sdc_mem_family { SDC_FAM_MEM_BOUND, SDC_FAM_COMPUTE_BOUND };
#endif

#ifdef __cplusplus
extern "C++" {

/*
 * quick 模式(--quick: max_test_loop_count==1) 检测, 供三个自适应函数把
 * 规模收缩到 lo 下限(CI 冒烟用, 内存自适应的大工作集在 CI 限时内跑不完)。
 * 公共 API quick_run() 由框架提供(sandstone.h)。
 */
static inline bool sdc_quick_run(void)
{
    return quick_run();
}

/* ------------------------------------------------------------------ */
/* 环境探测: 物理内存 / CPU 数                                         */
/* ------------------------------------------------------------------ */

/* 物理内存总字节数(带 64MB 兜底, 供 sysconf 失败的小内存环境)。 */
static inline uint64_t sdc_phys_mem_bytes(void)
{
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || page_size <= 0)
        return 64ull << 20;
    return (uint64_t)pages * (uint64_t)page_size;
}

/* ------------------------------------------------------------------ */
/* 可用内存预算: 四源取 min(spec §4.2 + issue #310 承诺源)              */
/* ------------------------------------------------------------------ */

/* [可用内存预算] 四源取 min(spec §4.2 + issue #310): 物理×‰ /
 * MemAvailable×0.9 / cgroup 限额−已用 / strict overcommit 承诺余量。
 * 任一来源读失败或无限即跳过该来源(MemAvailable 源失败返 0、cgroup 源
 * 与承诺源返 UINT64_MAX 作跳过哨兵); cgroup 限额已耗尽(lim≤usage)与
 * 承诺已满是【真实 0】, 诚实参与 min 而非跳过(#465)。
 * 动机: 旧口径只读 _SC_PHYS_PAGES, 批处理/容器限额下超分——
 * "job oom" 实测前科(svd_cdouble_sve.cpp:114-120 注释); 承诺源补
 * overcommit_memory=2 内核上"物理可用 ≠ 可承诺"的盲区(#310)。 */
static inline uint64_t sdc_memavailable_bytes(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[256];
    uint64_t kb = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %lu kB", &kb) == 1) break;
    fclose(f);
    return kb * 1024;
}

/* 解析 /proc/self/cgroup(PR#302 检视 #8): 定位本进程所在的 cgroup 层级,
 * 而非固定读根路径——嵌套 cgroup(systemd slice/scope, 容器)里根的
 * "无限/更宽"限制不代表本进程的约束, 旧口径会跳过该来源或用过宽预算,
 * 削弱 OOM 防护("job oom" 前科场景之一)。
 *   v2: 单行 "0::/rel/path"   → base=/sys/fs/cgroup;
 *   v1: 多行 "ID:ctrls:/rel"  → controllers 含 memory 的行,
 *                              base=/sys/fs/cgroup/memory(沿用旧挂载点
 *                              假设; 控制器重挂非标准位置属边界外)。
 * 两种行都记录(memory 控制器同一时刻只在 v1/v2 之一挂载——另一侧的
 * 层级文件不存在, walk 自然得 0); 无匹配行/不可读 → rel="/" 即旧根
 * 路径行为。写回 base/rel(含结尾 '\0')。 */
static inline void sdc_cgroup_self(char *v1base, size_t v1bsz, char *v1rel, size_t v1rsz,
                                   char *v2base, size_t v2bsz, char *v2rel, size_t v2rsz)
{
    snprintf(v1base, v1bsz, "/sys/fs/cgroup/memory");
    snprintf(v1rel, v1rsz, "/");
    snprintf(v2base, v2bsz, "/sys/fs/cgroup");
    snprintf(v2rel, v2rsz, "/");
    FILE *f = fopen("/proc/self/cgroup", "r");
    if (!f)
        return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "0::", 3) == 0) {                 /* v2 */
            /* rel 超长装不下 → 保持 "/"(根路径=旧口径): 静默截断会
             * 产生一个"合法前缀目录"的错位 walk, 比退根更糟。 */
            if (strlen(line + 3) < v2rsz)
                snprintf(v2rel, v2rsz, "%s", line + 3);
            continue;
        }
        const char *c1 = strchr(line, ':');
        const char *c2 = c1 ? strchr(c1 + 1, ':') : nullptr;
        if (!c1 || !c2 || c2 <= c1 + 1)
            continue;
        size_t ctrl_len = (size_t)(c2 - c1 - 1);
        char ctrls[128];
        if (ctrl_len >= sizeof ctrls)
            continue;
        memcpy(ctrls, c1 + 1, ctrl_len);
        ctrls[ctrl_len] = '\0';
        /* 逗号边界的 token 精确匹配 "memory"(非子串; 手写遍历避免
         * strtok 的静态状态)。 */
        const char *p = ctrls;
        while (*p) {
            const char *q = strchr(p, ',');
            size_t tl = q ? (size_t)(q - p) : strlen(p);
            if (tl == 6 && strncmp(p, "memory", 6) == 0) {
                if (strlen(c2 + 1) < v1rsz)   /* 同 v2: 超长退根 */
                    snprintf(v1rel, v1rsz, "%s", c2 + 1);
                break;
            }
            if (!q)
                break;
            p = q + 1;
        }
    }
    fclose(f);
}

/* 单级可用量: v2 读 memory.max("max"=无限→该级不约束)/memory.current;
 * v1 读 memory.limit_in_bytes(≥物理内存=无限)/memory.usage_in_bytes。
 * 哨兵契约(issue #465, 与第四源 sdc_commit_headroom_bytes 同约定):
 *   UINT64_MAX = 该级不约束/不可知(文件缺失/不可读/"max"/无限/usage
 *                读失败)——跳过, 不放大也不缩小预算;
 *   真实值(含 0) = 该级约束本进程, 0 = 限额已耗尽(limit≤usage)——
 *                诚实参与 min, 把预算压到零。
 * 旧口径把耗尽也折叠成 0=跳过: 消费侧 `cg > 0` 把耗尽的限额当无限,
 * 预算退化为 min(物理×‰, MemAvail×0.9) 完全无视 cgroup("job oom"
 * 前科场景), 随后大维度分配 bad_alloc → std::terminate/SIGABRT。 */
static inline uint64_t sdc_cgroup_level_avail(const char *dir, bool v2)
{
    /* 320: dir 最长 255(walk 的 256 缓冲) + 最长文件名 23, 防截断告警 */
    char path[320];
    uint64_t lim, cur;
    FILE *f;
    if (v2) {
        snprintf(path, sizeof path, "%s/memory.max", dir);
        f = fopen(path, "r");
        if (!f) return UINT64_MAX;
        char buf[64];
        bool ok = fgets(buf, sizeof buf, f) != nullptr;
        fclose(f);
        if (!ok || strncmp(buf, "max", 3) == 0) return UINT64_MAX;
        char *end;
        lim = strtoull(buf, &end, 10);
        /* 0=耗尽是强判决, 来源必须可信: 垃圾/空内容不得经 strtoull 的
         * 0 返回伪造"耗尽"(v1 侧 fscanf("%lu")==1 已有同等校验)。 */
        if (end == buf) return UINT64_MAX;
        snprintf(path, sizeof path, "%s/memory.current", dir);
        f = fopen(path, "r");
    } else {
        snprintf(path, sizeof path, "%s/memory.limit_in_bytes", dir);
        f = fopen(path, "r");
        if (!f) return UINT64_MAX;
        bool ok = fscanf(f, "%lu", &lim) == 1;
        fclose(f);
        if (!ok || lim >= sdc_phys_mem_bytes()) return UINT64_MAX;  /* 无限 */
        snprintf(path, sizeof path, "%s/memory.usage_in_bytes", dir);
        f = fopen(path, "r");
    }
    if (!f) return UINT64_MAX;
    bool ok2 = fscanf(f, "%lu", &cur) == 1;
    fclose(f);
    if (!ok2) return UINT64_MAX;      /* usage 不可读: 不可知 ≠ 耗尽 */
    return lim > cur ? lim - cur : 0; /* 耗尽 = 真实 0, 参与 min (#465) */
}

/* 自叶子(dir=base+rel)向根(base)逐级取 avail 的最小值——祖先与子孙
 * 的限制都约束本进程, 各级 usage 含下级, limit−usage 是该级对本进程
 * 的可用上界, 取 min 即保守合成。同 level_avail 哨兵契约(#465):
 * UINT64_MAX = 全程未见任何约束; 耗尽级 0 恰是 min 的胜者, 逐级传播。 */
static inline uint64_t sdc_cgroup_walk(const char *base, const char *rel, bool v2)
{
    char dir[256];
    if (snprintf(dir, sizeof dir, "%s%s", base, rel) >= (int)sizeof dir)
        return UINT64_MAX;    /* 路径超长: 不可知 ≠ 耗尽 (#465) */
    size_t blen = strlen(base);
    uint64_t best = UINT64_MAX;   /* min 恒等元 = "未见任何约束" */
    for (;;) {
        uint64_t av = sdc_cgroup_level_avail(dir, v2);
        if (av < best)
            best = av;
        char *slash = strrchr(dir, '/');
        if (!slash || (size_t)(slash - dir) + 1 <= blen)
            break;                     /* 已处理到 base 本级 */
        *slash = '\0';
    }
    return best;
}

static inline uint64_t sdc_cgroup_limit_bytes(void)
{
    /* v2: memory.max - memory.current ("max" = 无限 → 不约束)
     * v1: memory/memory.limit_in_bytes - memory.usage_in_bytes
     *      (limit >= 物理内存视为无限 → 不约束)。本机(openEuler)实测为 v1、
     *      且位于嵌套 /system.slice/sshd.service(根无限制≠本进程无限)。
     * 哨兵(#465): UINT64_MAX = 两侧都读不到约束(未挂载/无限/读失败);
     * 真实值(含 0 = 耗尽) = 约束生效。UINT64_MAX 是 min 恒等元, 旧
     * "0=未挂载"特判溶解——memory 控制器只在一侧挂载时另一侧 walk 返回
     * UINT64_MAX, 直接取 min 即活侧的值。(v2 memory.max 写成 ≥2^64−cur
     * 的荒谬大数值会与哨兵别名——其语义本就是"实际无限", 别名到跳过
     * 无害, 与 #384 R8 的哨兵别名记录同型。) */
    char v1base[64], v1rel[192], v2base[48], v2rel[192];
    sdc_cgroup_self(v1base, sizeof v1base, v1rel, sizeof v1rel,
                    v2base, sizeof v2base, v2rel, sizeof v2rel);
    uint64_t a1 = sdc_cgroup_walk(v1base, v1rel, false);
    uint64_t a2 = sdc_cgroup_walk(v2base, v2rel, true);
    return a1 < a2 ? a1 : a2;
}

/* [承诺预算] 第四源(issue #310): 仅 vm.overcommit_memory=2(strict) 内核
 * 生效——匿名私有承诺(含 fork 的 COW 记账)受 CommitLimit =
 * RAM×ratio/100 + swap 硬限制, 超限是 mmap/brk ENOMEM(bad_alloc 降档/
 * 线程级 SKIP), 不是 OOM-kill。前三源全物理口径, 而 MemAvailable(物理
 * 空闲)与承诺余量独立: ratio=50 的 60GB+4GB swap 机器上 MemAvailable×0.9
 * 可给出 ~40GB 预算, 承诺余量却只有 CommitLimit−Committed_AS——sizing
 * 会系统性越界(#310 的 Y002 部署形态)。
 * VmHWM 反馈回路替代不了本源: =2 下分配在 RSS 增长之前就被拒,
 * bad_alloc 降级路径的样本到不了反馈写入点(幸存者偏差), c 被低估 →
 * n 膨胀 → 更多 ENOMEM——反馈闭环在承诺受限机器上是盲的。
 * 口径: 0.9×(CommitLimit−Committed_AS)(/proc/meminfo, 全系统现读,
 * 与 MemAvailable 源同"现读+折扣"哲学——承诺记账本身精确, 折扣留给
 * 并行 slice 各自 sizing 与实际分配之间的时间窗 + 他进程并发抢承诺;
 * sqrt(0.9)≈0.95 的维度代价可忽略)。承诺已满 → 0(诚实: 一分不能再
 * 承诺)。非 =2(0=heuristic/1=always, CommitLimit 无强制力)、sysctl
 * 读失败、meminfo 字段缺失 → UINT64_MAX "不约束"(与既有"读失败跳过
 * 该来源"约定一致, 不放大也不缩小预算)。 */
static inline uint64_t sdc_commit_headroom_bytes(void)
{
    FILE *f = fopen("/proc/sys/vm/overcommit_memory", "r");
    if (!f)
        return UINT64_MAX;
    int policy;
    bool ok = fscanf(f, "%d", &policy) == 1;
    fclose(f);
    if (!ok || policy != 2)
        return UINT64_MAX;
    uint64_t limit = 0, committed = 0;
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            unsigned long long kb;
            if (sscanf(line, "CommitLimit: %llu kB", &kb) == 1)
                limit = kb * 1024ull;
            else if (sscanf(line, "Committed_AS: %llu kB", &kb) == 1)
                committed = kb * 1024ull;
        }
        fclose(f);
    }
    if (limit == 0 || committed == 0)
        return UINT64_MAX;      /* 字段缺失/不可读 → 跳过该来源 */
    if (limit <= committed)
        return 0;               /* 承诺已满 */
    uint64_t head = limit - committed;
    return head - head / 10;    /* ×0.9, 与 MemAvailable 源对称 */
}

static inline uint64_t sdc_budget_bytes(int permille)
{
    uint64_t b = sdc_phys_mem_bytes() * (uint64_t)permille / 1000;
    uint64_t ma = sdc_memavailable_bytes();
    if (ma > 0 && ma * 9 / 10 < b) b = ma * 9 / 10;
    uint64_t cg = sdc_cgroup_limit_bytes();
    /* #465: UINT64_MAX = 不约束(未挂载/无限/读失败)→ 跳过; 真实值(含
     * 0 = 限额耗尽)参与 min——下游契约早已按此口径写就(mesh_upi 系
     * "budget==0(限额/承诺耗尽)→ init 诚实 SKIP"; 第四源同约定)。 */
    if (cg != UINT64_MAX && cg < b) {
        b = cg;
        if (cg == 0) {
            static bool logged = false;   /* 每 TU 一次(static inline 是内部
                                          * 链接, static local 随 TU 各一份;
                                          * 每 slice child 是独立进程, 实测
                                          * 每 child 一条, 与下方承诺源同式) */
            if (!logged) {
                logged = true;
                log_warning("[cgroup 预算] cgroup 内存限额已耗尽 "
                            "(limit ≤ usage): 预算压至 0 — 自适应规模将"
                            "退守用例规模下限");
            }
        }
    }
    /* #310 第四源: strict overcommit 下的承诺余量(非 =2 时为
     * UINT64_MAX, 本块无操作)。成为最小来源时打一次诊断——该部署形态
     * 的用户直接看到预算被 CommitLimit 钳制, 而非对着 ENOMEM/降档猜测。 */
    uint64_t head = sdc_commit_headroom_bytes();
    if (head != UINT64_MAX && head < b) {
        b = head;
        static bool logged = false;   /* 每 TU 一次(static inline 是内部
                                      * 链接, static local 随 TU 各一份);
                                      * 每 slice child 是独立进程, 实测
                                      * 每 child 一条(仅 node-budget 路
                                      * 径触发), 诊断足够且不刷屏 */
        if (!logged) {
            logged = true;
            log_warning("[承诺预算] overcommit_memory=2: 预算被 CommitLimit "
                        "钳制为 %.1f GiB (0.9×(CommitLimit−Committed_AS))",
                        (double)b / 1073741824.0);
        }
    }
    return b;
}

/* 在线 CPU 数(带 1 兜底)。 */
static inline int sdc_online_cpus(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

/*
 * 全机并发线程数 —— 压力预算分摊的基准。
 *
 * 框架会把多 socket / 大核数机器切成多个 slice 并「并行 fork」子进程
 * (每 slice 一个 child), child_run() 里 sApp->thread_count 被改写为
 * slice 内线程数。用例在 test_init(子进程内)读 thread_count() 拿到的
 * 只是本 slice 的线程数, 若直接用它分摊全机内存预算, N 个并行 slice
 * 各自分摊一次 => 实际内存 = N × 预算 (实测 2 slice 时 gather_f64 峰值
 * 达物理内存 84% 而模型只有 60%)。
 *
 * 本函数返回预算分摊应使用的全机总线程数:
 *   - total_thread_count() (公共 API, child 里读 shmem->total_thread_count);
 *   - 不可见(独立编译/单元测试): thread_count() > 0 则用之, 否则在线 CPU 数。
 */
static inline int sdc_total_threads(void)
{
    int n = total_thread_count();
    if (n > 0)
        return n;
    n = thread_count();
    if (n > 0)
        return n;
    return sdc_online_cpus();
}

/* ------------------------------------------------------------------ */
/* 通用配置文件解析: KEY=VALUE 行, '#' 注释, 空行忽略                   */
/* ------------------------------------------------------------------ */

/*
 * 读取 path 指向的配置文件, 把每个 KEY=VALUE 行注册成全局 knob。
 * 与框架 -O 相同的名字空间, 因此 testid.knob=value 行的语义和命令行
 * 完全一致, 且命令行 -O (后加载) 天然覆盖配置文件。
 * 返回: 0 成功(含文件不存在), -1 格式错误(errmsg 填充)。
 */
static inline int sdc_load_config_file(const char *path, char *errmsg, size_t errsz)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;                         /* 不存在 => 没有配置文件, 正常 */

    char line[512];
    int lineno = 0;
    int bad = 0;
    while (fgets(line, sizeof(line), f)) {
        ++lineno;
        /* strip trailing newline / CR */
        size_t len = strlen(line);
        while (len && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';                /* '#' 之后为注释 */
        /* 去首尾空白 */
        char *s = line;
        while (*s == ' ' || *s == '\t') ++s;
        char *e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) --e;
        *e = '\0';
        if (*s == '\0')
            continue;                     /* 空行/纯注释 */

        char *eq = strchr(s, '=');
        if (!eq || eq == s || eq[1] == '\0') {
            if (!bad && errmsg)
                snprintf(errmsg, errsz,
                         "sdc_stress.conf:%d: 需要 KEY=VALUE 形式, 得到 \"%s\"",
                         lineno, s);
            bad = 1;
            continue;
        }
        /* set_knob_from_key_value_string 按 '=' 原样切分且不清理空白,
         * 因此这里把 KEY 右侧、VALUE 左侧的空白剥掉再重组, 允许用户
         * 写 "KEY = VALUE" 的自然格式。 */
        {
            char *key_end = eq;
            while (key_end > s && (key_end[-1] == ' ' || key_end[-1] == '\t'))
                --key_end;
            char *val = eq + 1;
            while (*val == ' ' || *val == '\t')
                ++val;
            if (key_end == s || *val == '\0') {
                if (!bad && errmsg)
                    snprintf(errmsg, errsz,
                             "sdc_stress.conf:%d: KEY 或 VALUE 为空: \"%s\"",
                             lineno, s);
                bad = 1;
                continue;
            }
            size_t klen = (size_t)(key_end - s);
            char pair[512];
            snprintf(pair, sizeof(pair), "%.*s=%s", (int)klen, s, val);
            if (!set_knob_from_key_value_string(pair)) {
                if (!bad && errmsg)
                    snprintf(errmsg, errsz,
                             "sdc_stress.conf:%d: 非法条目 \"%s\"",
                             lineno, s);
                bad = 1;
            }
        }
    }
    fclose(f);
    return bad ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* 旋钮读取 + 校验                                                     */
/* ------------------------------------------------------------------ */

/*
 * knob 值缓存: 框架约定 test option 只能在 test_init()(主线程,
 * thread_num == -1)查询——在 worker 线程(test_run)调用
 * get_testspecific_knob_value_int() 会触发 logging_mark_knob_used 的
 * abort(该检查自 #417 起对所有构建类型生效, 不再受 NDEBUG 门控)。
 * 故 init 查询时把最终生效值缓存, run 里只读缓存; worker 侧 cache miss
 * 同样 fail loudly(见 sdc_knob_int_clamped)。fork 模式下子进程写时
 * 复制继承主进程已填充的缓存。
 */
static inline std::string sdc_knob_cache_key(const struct test *test, const char *key)
{
    return std::string(test ? test->id : "(global)") + "." + key;
}

static inline std::map<std::string, int64_t> &sdc_knob_cache()
{
    static std::map<std::string, int64_t> cache;
    return cache;
}

/*
 * 读取整型旋钮, clamp 到 [lo, hi]:
 *   - 未配置 => 返回 def;
 *   - 配置但越界 => report_fail_msg 报警(非致命, 便于 capture 战役不因
 *     单个坏配置中断)并 clamp 到边界;
 *   - 配置且合法 => 原值返回。
 * def 传 0 表示"调用方希望自适应"——本函数原样返回 0, 由调用方走
 * sdc_auto_* 系列。
 */
static inline int64_t sdc_knob_int_clamped(struct test *test, const char *key,
                                           int64_t def, int64_t lo, int64_t hi)
{
    const std::string cache_key = sdc_knob_cache_key(test, key);

    /* worker 线程(test_run): 不查 knob, 直接读 init 预热过的缓存。
     * 未命中说明该测试在 init 里没有查询过此 knob(测试 bug)。旧行为是
     * 静默返回 def——release 与 debug 下同样发生, 会让
     * `-O <test>.<knob>=N` 被无声无视(#417 缺口 3)。现在 fail loudly:
     * log_error + abort, 与 logging_mark_knob_used 的 worker 线程检查
     * 同口径。 */
    if (thread_num != -1) {
        auto it = sdc_knob_cache().find(cache_key);
        if (it != sdc_knob_cache().end())
            return it->second;
        log_error("%s: worker 线程读取 knob \"%s\" 但 init 未预热缓存——"
                  "测试 bug: 请在 test_init 里无条件调用本 helper(或读取同 "
                  "knob 的 sdc_knob_int_clamped)预填缓存, 否则 -O 显式配置"
                  "会被无视",
                  test ? test->id : "(global)", key);
        abort();
    }

    /* 主线程(test_init): 正常查询 + clamp + 缓存 */
    /* 只在显式配置时才需要校验; 用一个不可能的哨兵探测配置存在性 */
    int64_t sentinel = hi + 1;            /* sentinel 一定在 [lo,hi] 之外 */
    int64_t v = get_testspecific_knob_value_int(test, key, sentinel);
    int64_t result;
    if (v == sentinel) {
        if (def == 0)                      /* 哨兵模式: 值由下级通道决定 */
            log_warning("[压力映射] %s.%s: 未显式配置 → 环境变量/内存自适应通道"
                        " (core179 诊断探针则为设计默认几何)",
                        test ? test->id : "(global)", key);
        result = def;
    } else if (v < lo || v > hi) {
        int64_t clamped = v < lo ? lo : hi;
        log_warning("%s: %s=%ld 越界 (有效 %ld..%ld), 已 clamp 为 %ld",
                    test ? test->id : "(global)", key, (long)v,
                    (long)lo, (long)hi, (long)clamped);
        log_warning("[压力映射] %s.%s = %ld (来源: 命令行/配置文件, 已越界 clamp)",
                    test ? test->id : "(global)", key, (long)clamped);
        result = clamped;
    } else {
        log_warning("[压力映射] %s.%s = %ld (来源: 命令行/配置文件, 有效 %ld..%ld)",
                    test ? test->id : "(global)", key, (long)v,
                    (long)lo, (long)hi);
        result = v;
    }
    sdc_knob_cache()[cache_key] = result;
    return result;
}

/* 环境变量兜底: SDC_<TESTID>_<KNOB> —— 配置文件未提供时的轻量通道。
 * 返回值: 找到且解析成功(可表示, 无 ERANGE 溢出)返回 1 且 *out 填充;
 * 否则返回 0(含形态非法/溢出饱和——饱和值不是用户真实输入, 不得当
 * "解析成功"送进 clamp 通道, issue #490)。
 * (位于联合求解段之前——T11 的 knob 早退块补查 env 通道要用。) */
static inline int sdc_env_knob_int(const char *test_id, const char *key,
                                   int64_t *out)
{
    if (!test_id || !key)
        return 0;
    char name[256];
    int n = snprintf(name, sizeof(name), "SDC_%s_%s", test_id, key);
    if (n <= 0 || (size_t)n >= sizeof(name))
        return 0;
    for (char *p = name; *p; ++p) {
        if (*p == '.')
            *p = '_';                     /* testid 里可能带点 */
        else if (*p >= 'a' && *p <= 'z')
            *p = (char)(*p - 'a' + 'A');  /* 转大写 */
    }
    const char *v = getenv(name);
    if (!v || !*v)
        return 0;
    char *end = nullptr;
    errno = 0;
    long long parsed = strtoll(v, &end, 0);
    if (end == v || (end && *end != '\0'))
        return 0;
    /* ERANGE: strtoll 饱和到 LLONG_MAX/LLONG_MIN, parsed 不是用户输入。
     * 若放行, 越界告警打出的会是饱和值而非用户真实输入(实测 23 位输入
     * 打出 9223372036854775807), 且与 -O 通道的溢出行为不可比(该通道
     * strtoull 饱和回绕成 -1 反向 clamp, 上游事故不在本处修)——按契约
     * "解析成功才返回 1"拒绝, 告警真实值后回退未配置路径(默认/自适应),
     * 与 -O 整数选项的 ERANGE 口径同构(也查 errno==ERANGE、越界错误打
     * 原始入参串, sandstone_opts.cpp ParseIntArgument); 位置在形态检查
     * 之后, 溢出且带尾随垃圾的输入保持原有静默拒绝不变。 */
    if (errno == ERANGE) {
        log_warning("[压力映射] %s.%s: 环境变量值 '%s' 超出 long long 可表示"
                    "范围 (strtoll 溢出饱和), 按未配置处理 — 回退默认/自适应通道",
                    test_id, key, v);
        return 0;
    }
    if (out)
        *out = (int64_t)parsed;
    return 1;
}

/* 环境变量兜底(clamped 版): 与 sdc_env_knob_int 相同通道, 但配置越界时
 * log_warning 并 clamp 到 [lo, hi] —— 与 -O/配置文件通道的校验语义一致,
 * 杜绝 SDC_<TESTID>_<KNOB> 直通道的无效参数(防 OOM/防欠压力)。
 * 返回值: 环境变量存在且被采用(含 clamp)返回 1; 未设置返回 0。 */
static inline int sdc_env_knob_int_clamped(const char *test_id, const char *key,
                                           int64_t *out, int64_t lo, int64_t hi)
{
    int64_t v;
    if (!sdc_env_knob_int(test_id, key, &v))
        return 0;
    if (v < lo || v > hi) {
        int64_t c = (v < lo) ? lo : hi;
        log_warning("%s: %s=%ld 越界 (有效 %ld..%ld), 已 clamp 为 %ld",
                    test_id ? test_id : "(global)", key, (long)v,
                    (long)lo, (long)hi, (long)c);
        v = c;
    }
    log_warning("[压力映射] %s.%s = %ld (来源: 环境变量, 有效 %ld..%ld)",
                test_id ? test_id : "(global)", key, (long)v,
                (long)lo, (long)hi);
    if (out)
        *out = v;
    return 1;
}

/* 显式 knob→env 兜底读取的单一真源【PR#384 检视, 意见 4432271425】:
 * 该序列(knob 通道 sdc_knob_int_clamped(def=0) → 未显式配置时 env 通道
 * sdc_env_knob_int_clamped 兜底)此前在 eigen_gemm/gemm_double_max_arm 与
 * eigen_svd/svd_cdouble_sve 两文件的 preinit + child test_init 四处逐字
 * 复制, "两处取值恒相同"的不变式纯靠手工同步——任一侧单独改动(clamp
 * 范围/env 串/回退序)即静默重开 issue #352 的误杀窗口, 而两文件头注释
 * 明文邀请的 MDIM_MAX opt-in 实验恰是最可能触发单侧改动的场景。调用方
 * (preinit 与 child)必须经由本函数 + 同一 (test, lo, hi) 读取同一旋钮,
 * 读取通道的不变式由单一实现钉住(lo/hi 仍由调用方传参——同文件同一宏
 * 两处传参, 值一致由宏单一真源承担)。
 * 【PR#384 检视, 意见 4439402793】env 通道前缀不再由调用方传字面量:
 * 从入参 test->id 推导(与 sdc_knob_cache_key 同法), 拼错字面量即单侧
 * env 兜底失效的手工同步点彻底消除——四处调用点此前各手写一次
 * "eigen_svd_cdouble_sve"/"eigen_gemm_double_max_arm" 恰与 test->id
 * 相同, 推导后行为逐字不变(test 为空则跳过 env 通道, 与传空串同义;
 * "逐字不变"仅对默认构建成立, NO_TEST_NAMES 边界见下段)。
 * 【PR#384 检视 R4, 意见 4441045420】SANDSTONE_NO_TEST_NAMES 构建
 * (framework_options 含 no-test-names, 或 logging_format=no_output,
 * 见 framework/meson.build)下 DECLARE_TEST_INNER 令
 * .id = STRINGIFY(TEST_ID_<test>) = shortid 十六进制串(如 "cd14b7")
 * ——env 名随之从 SDC_EIGEN_SVD_CDOUBLE_SVE_MDIM 漂移为
 * SDC_CD14B7_MDIM, 已部署旧名静默失配(实测 2026-11-04: 旧名 →
 * "未显式配置"回退 auto 档; 新名 → "来源: 环境变量"命中显式档。
 * 回退的 auto 档有 AR 保护, 不重开 #352)。该漂移系框架 test->id
 * 派既有属性(联合求解 T11 块/eigen common 头 x86 分支/mesh_upi 系
 * 同此), 且与该构建 -e/-O 的 shortid 寻址一致(-e
 * eigen_svd_cdouble_sve 同样 Cannot find)——字面量时代的 env 旧名
 * 反而与寻址体系脱节成两套命名。恢复字面量重开 4439402793 的同步
 * 点, 单侧剥 shortid 又与全部 test->id 派不一致, 均不取; 稳定短名
 * 寻址属框架级设计项, 边界在此如实记录。
 * 【PR#384 检视 R5, 意见 4463650392】「返回 0 = 未显式配置」的哨兵
 * 契约隐含 **lo > 0** 前提, 在此显式声明: sdc_knob_int_clamped 对
 * 「已配置且合法」的值原样返回——lo ≤ 0 的调用方会让用户显式
 * -O <test>.<key>=0 以合法值 0 命中下方 knob == 0 判定, 被当未配置
 * 走 env 兜底(env 已设则 -O/env 优先级倒置, 未设则显式 0 被静默
 * 丢弃当 auto)。当前四调用方(gemm/svd 各 preinit+child)lo =
 * MDIM_MIN(1024/300) > 0 安全; 值域天然含 0 的旋钮(slots/count 类)
 * 不得经由本函数, 须改用显式哨兵/三态协议。
 * 【PR#384 检视 R8, 意见 4464388834】哨兵探测还有 **hi + 1** 端别名:
 * sdc_knob_int_clamped 用 sentinel = hi+1 探测「是否显式配置」, 用户
 * 显式配置值恰为 hi+1 时(如 -O …mdim=6001, SVD MDIM_MAX=6000; gemm
 * 同构 4097/4096)以合法路径返回 hi+1 == sentinel, 被当未配置走 env
 * 兜底——与 lo≤0 端同型失效(env 已设则 -O/env 优先级倒置, 未设则
 * 静默降级 auto 且日志「未显式配置」与事实矛盾)。实测(2026-11-04):
 * -O mdim=6001 → 「未显式配置 → 环境变量/内存自适应通道」回退 auto
 * 档; 对照 -O mdim=9999 走 clamp 分支截到 6000 并告警——全值域唯独
 * hi+1 一个值被静默吞掉。回退的 auto 档有 AR 保护, 不重开 #352
 * kill 窗。改双向哨兵(独立 bool 出参等)属框架级接口变更, 超出本 PR
 * 边界; 契约在此补全: 调用方旋钮值域须排除 {0(lo≤0 时的别名),
 * hi+1(探测哨兵)} 两个值。
 * 返回 0 = 未显式配置(调用方走自适应; 前提 lo > 0 且值域避让 hi+1,
 * 见上两段)。内部
 * 序列与原四份拷贝逐字一致, 日志/缓存语义零变化(默认构建)。 */
static inline int64_t sdc_knob_or_env_clamped(struct test *test,
                                              const char *key,
                                              int64_t lo, int64_t hi)
{
    int64_t knob = sdc_knob_int_clamped(test, key, 0, lo, hi);
    if (knob == 0)
        sdc_env_knob_int_clamped(test ? test->id : nullptr, key, &knob,
                                 lo, hi);
    return knob;
}

/* ------------------------------------------------------------------ */
/* 内存自适应默认值                                                    */
/* ------------------------------------------------------------------ */

/*
 * 自适应方阵维度: 求最大的 N 使
 *   bytes_per_elem * matrices_per_thread * rss_ratio * N^2 * nthreads
 *     <= phys_mem * mem_fraction
 * 再 clamp 到 [lo, hi]。
 *
 * Deprecated(spec §4.3): arm64 新代码请用 sdc_joint_dim_slice()——
 * 后者按内存×时间×NUMA 联合求解; 本函数是纯内存全局模型(无时间轴、
 * 无 per-node 预算), x86 既有调用方保留(rule 5 原样)。
 *
 * 参数语义:
 *   bytes_per_elem          — 单个矩阵元素字节数 (double=8, float=4,
 *                             complex<double>=16 ...)
 *   matrices_per_thread     — 每线程同时驻留的矩阵数(含 golden/scratch)
 *   rss_ratio               — 算法峰值 RSS 相对矩阵体积的倍数
 *                             (GEMM ~6, BDCSVD ~12, 见各用例注释)
 *   mem_fraction_permille   — 允许本用例(全部线程合计)占用的物理内存,
 *                             千分数: 900 = 90%
 *   nthreads                — 并发线程数(传 thread_count(); 在 preinit
 *                             (父进程) 调用时它是请求的总线程数)
 *   lo / hi                 — 安全区间(参考各用例实测时间上限)
 */
static inline int sdc_auto_square_dim(int bytes_per_elem,
                                      int matrices_per_thread,
                                      double rss_ratio,
                                      int mem_fraction_permille,
                                      int nthreads,
                                      int lo, int hi,
                                      const char *test_id = nullptr,
                                      const char *knob = nullptr)
{
    /* quick 模式(CI 冒烟): 1 轮迭代要在 CI 限时内跑完, 直接用安全下限 */
    if (sdc_quick_run()) {
        if (test_id && knob)
            log_warning("[压力映射] %s.%s = %ld (来源: quick 模式收缩, 取 lo 下限)",
                        test_id, knob, (long)lo);
        return lo;
    }
    /* 预算按全机并发线程分摊: nthreads 入参只是本 slice 的线程数,
     * 框架并行 fork 的 N 个 slice 会各自调用本函数, 若按 slice 数分摊
     * 全机预算, 合计足迹 = N × 预算。 */
    int total = sdc_total_threads();
    if (total > nthreads)
        nthreads = total;
    if (nthreads < 1)
        nthreads = 1;
    if (mem_fraction_permille < 1)
        mem_fraction_permille = 500;      /* 缺省保守 50% */
    if (mem_fraction_permille > 1000)
        mem_fraction_permille = 1000;

    double budget = (double)sdc_phys_mem_bytes()
                    * (double)mem_fraction_permille / 1000.0;
    double per_elem = (double)bytes_per_elem
                      * (double)matrices_per_thread * rss_ratio
                      * (double)nthreads;
    if (per_elem <= 0.0)
        return lo;
    int raw = (int)std::sqrt(budget / per_elem);
    int n = raw;
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    if (test_id && knob) {
        const double gib = 1073741824.0;
        double expect = (double)n * (double)n * per_elem;
        double phys = (double)sdc_phys_mem_bytes();
        if (raw > hi)
            log_warning("[欠压力] %s.%s: 自适应需 %ld 触安全上限 %ld, "
                        "预期合计仅 %.1f GiB (%d‰ 预算的 %.0f%%) — 可提高上限或减线程",
                        test_id, knob, (long)raw, (long)hi,
                        expect / gib, mem_fraction_permille,
                        100.0 * expect / budget);
        else if (raw < lo)
            log_warning("[内存越界] %s.%s: 安全下限 %ld 使预期合计 %.1f GiB "
                        "超出 %d‰ 预算 %.1f GiB — 线程过多或内存过小",
                        test_id, knob, (long)lo, expect / gib,
                        mem_fraction_permille, budget / gib);
        log_warning("[压力映射] %s.%s = %ld (来源: 内存自适应 %d‰, 全机 %d 线程) → "
                    "预期合计 ~%.1f GiB = 物理内存的 %.1f%%",
                    test_id, knob, (long)n, mem_fraction_permille, nthreads,
                    expect / gib, 100.0 * expect / phys);
    }
    return n;
}

/*
 * 时间校准模型 (sdc_calibrated_dim):
 *   内存自适应(sdc_auto_square_dim)只约束 RSS, 不约束单次求解时长——
 *   大维度下单次 SVD/Cholesky 求解可达分钟级, -t 到时后 TERM 无法打断
 *   迭代内计算, 框架只能 SIGQUIT 判 timed out(实测 eigen_sparse 自适应
 *   档满核 600s 仍无法退出; eigen SVD mdim=2048 单次数分钟)。
 *   本函数在自适应路径上叠加时间上限: 在 lo 与 2*lo 两档各完整求解一次
 *   (结果丢弃, 只为计时), 幂律 t(n)=c*n^k 拟合指数 k, 外推使单次求解
 *   <= max_single_ms 的维度, 再乘安全系数 0.5 吸收外推偏差。
 *   外推后再实测选定维度一次, 超预算按实测幂律收缩并复验——构造性
 *   保证 one solve <= max_single_ms(外推/安全系数失准的兜底)。
 *   口径(检视 #44/#45): (1) probe 的计时单位 = 构造 + 一次完整求解,
 *   与 run 期迭代同构(保守方向——构造被计入预算); (2) 该保证是
 *   单线程探针口径。run 期满核并发时 memory-bound 求解受带宽/缓存
 *   竞争显著放大——2026-10-25 鲲鹏 920(128 CPUs)实测: n=1191 稀疏
 *   Cholesky 单解 574ms(单线程) → 14268ms(128 线程, ~24.9×), 含
 *   reroll 的最坏迭代 ~29s; -t 的 TERM 兑现最终由框架硬宽限保证
 *   (sandstone.cpp test_timeout = max(5×-t + 30s, 300s) 后才
 *   SIGQUIT), 校准的作用是把未校准档的分钟级(600s)压到 O(10s)
 *   量级、远低于该宽限。
 *   k clamp [1.5, 4.0] 抑制小档计时噪声; 试解总成本两小档+一验证档。
 *   注意: 验证探针本身**不受 max_single_ms 约束**——它是预算的执行者,
 *   初选落点在幂律/k_floor 失准时可超预算数倍(本文件 :434 段自引的实测
 *   落点 8.3s vs 2s 预算即此情形), 只有其后的收缩轮才按预算收敛。
 *   显式 knob/env 指定维度的路径不得调用本函数(用户自担)。
 * 拟合出参 fit(spec §4.3 M3): 成功路径填充 {a_ms, p} —— 单解模型
 * t(n)=a_ms·n^p, a_ms 已按两点差分扣除固定开销 b(见函数尾); 早退
 * 路径保持 {0,0}(无效, 消费方跳过 n_run)。
 * fit/probe_floor_ms 无缺省(主模板 10 参, 见下方转发重载): 既有 7 实参
 * 调用(跨 arch 存量链, 含 x86-64 #else 分支)经 7 参转发重载——等价于
 * 旧缺省 {nullptr, 1.0}, 签名与行为不变(rule 5, issue #351——转发
 * 重载的 1.0 即 f84966bc 之前的历史噪声底语义, 不得随 ARM 侧 M3 口径
 * 联动); 需要 M3 固定开销口径的调用方(联合求解两分支——ARM 分支与
 * x86 fallback(#482))显式传
 * sdc_m3_probe_floor_ms(100.0)。fit 保持自 f84966bc 引入起(至本
 * PR 参数化前)的第 8 参位次、probe_floor_ms 殿后: 字面量 0/NULL 作第 8
 * 实参(『显式不要 fit』的旧写法)经 std::nullptr_t 重载绑 fit 而非经
 * int→double 隐式转换静默重绑 probe_floor_ms=0.0——后者使 probe 抬档
 * 循环永假、外推基数退回微档噪声且无告警; double 字面量误入第 8 位
 * 编译报错(响亮); 传非空 fit 而省略第 9 参(8 参形态)亦编译报错
 * (R10 检视 id=4463831067: 该形态会以缺省 1.0 停点静默运行——
 * 固定开销污染差分, 仅剩运行期告警兜底, 与本签名杜绝『经签名
 * 静默误用』的设计目标不符; fit 消费方必须显式选择 M3 口径)。
 * fit_only(issue #487, 第 10 参缺省 false): 跳过外推后的验证档与收缩
 *   轮, 返回未验证的外推档——供只消费 fit 的调用方(联合求解)使用:
 *   其返回值被弃用, 验证/收缩的全部成本只为被弃用的返回值(2026-11-04
 *   实测 128 CPU 主机 GEMM 族每 init 验证档单解 11.2s 纯浪费)。fit 的
 *   两点差分只用验证前的 (p1, mid) 探针, 跳过验证档不改变拟合值。
 * 调用契约:
 *   int dim = sdc_auto_square_dim(...);        // 内存上限
 *   if (dim > lo)
 *       dim = sdc_calibrated_dim(lo, dim, max_single_ms,
 *                                test_id, knob, k_floor, solve_once);
 */
struct sdc_fit {
    double a_ms;    /* t(n)=a_ms·n^p 单解幂律系数(ms, 已扣固定开销) */
    double p;       /* 幂指数(k_floor 修正后) */
};

/* M3 固定开销口径的 probe 档下限(ms), spec §4.3 M3: 差分扣 b 要求
 * probe 档**实测时长**不低于该值——固定开销(构造/预处理)在计时中
 * 占比 <10%, 幂律系数不被抬高。消费方共三处(fit 差分守卫 / 联合求解
 * 显式实参 / 告警文案), 由本常量绑定, M3 重标定只改一处。 */
static constexpr double sdc_m3_probe_floor_ms = 100.0;

/* M3 前提告警公共段: 分流文案(传参达标/未达标)共用的病理与后果表述。
 * 宏参与字面量拼接(format 检查仍生效), 单点维护防双口径漂移——
 * 分支各自维护曾致 byte-identical 双口径并存(见 8d0dc47e 裁定)。 */
#define SDC_M3_FIT_WARN_BODY                                          \
    "fit 差分扣 b 的固定开销占比<10%%前提未达成(spec §4.3 M3), "       \
    "a_ms 将被固定开销与计时噪声污染(a_ms 虚高→n_run 偏小→激发欠压); "

template <typename F>
static inline int sdc_calibrated_dim(int lo, int hi, long max_single_ms,
                                     const char *test_id, const char *knob,
                                     double k_floor, F solve_once,
                                     struct sdc_fit *fit,
                                     double probe_floor_ms,
                                     bool fit_only = false)
{
    if (fit) { fit->a_ms = 0.0; fit->p = 0.0; }  /* 早退路径保持无效 */
    if (hi <= lo)
        return lo;
    auto probe = [&](int n) -> double {
        auto t0 = std::chrono::steady_clock::now();
        if (solve_once(n) != 0)
            return -1.0;   /* issue #420: 探针自报失败(非零返回码)→ 负哨兵
                              计时, 与"计时恰好为 0"的不可测语义区分。失败
                              分支的计时是 μs 级噪声而非求解耗时, 绝不能进
                              幂律拟合。(历史触发者 bidiag run_bidiag_once
                              已随用例 2026-11-02 退役; 现役探针全为异常
                              上抛式, 本谓词对其恒假——纯契约防御) */
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - t0).count();
    };
    /* probe 档自适应抬升: 小档求解太快(< probe_floor_ms)时翻倍 probe 档
     * 直到可测量或触及 hi——否则外推基数失真。两级语义(issue #351):
     *   - 缺省 1.0(历史噪声底): 求解快到 1ms 以下即被计时噪声淹没
     *     (实测稀疏 Cholesky 规则带状 probe 在 64 档仅 0.1ms, 外推
     *     失控)——跨 arch 存量 7 参调用(x86-64 #else 链)语义不变沿用
     *     此口径(同 1ms 停点与代码路径, 操作点在计时抖动内一致),
     *     不得随 ARM 侧 M3 口径联动改动(rule 5)。
     *   - sdc_m3_probe_floor_ms(100.0, spec §4.3 M3): 固定开销(构造/
     *     预处理)在计时中占比 <10%, 幂律系数不被抬高(实测 sve 模型
     *     t(N)=243.4·(N/4400)³+1 的 +1 项在 N=300 档占 93%, 系数虚高
     *     ~14×, 立方根收缩 2.4×)——仅由联合求解(ARM 分支与 x86
     *     fallback(#482))显式传入,
     *     与其 fit 差分扣 b 配套。 */
    int p1 = lo;
    double t1 = probe(p1);
    /* t1>=0.0(issue #420): 失败哨兵即刻停止抬升——失败探针恒"快",
     * 不拦会一路把 p1 无谓探到 hi。 */
    while (t1 >= 0.0 && t1 < probe_floor_ms && p1 * 2 <= hi) {
        p1 *= 2;
        t1 = probe(p1);
    }
    int mid = (p1 * 2 <= hi) ? p1 * 2 : hi;
    double t2 = probe(mid);
    if (t1 < 0.0 || t2 < 0.0) {
        /* issue #420: 探针自报失败——失败分支的 μs 级计时噪声曾被当作
         * 超快求解外推(k 钳制后 dim=hi), 联合层 n_run 失控 → mdim 落
         * n_mem/hi 上限, 即 P0-2 超时复发路径。失败 ≠ 『时间不是约束』
         * (那是两档计时皆恰为 0 的语义, 仍交还 hi), 与异常式探针的
         * catch(...) → lo 教义同向: 退守校准下限。fit 保持无效({0,0}),
         * 消费方(联合层)据此跳过 n_run、另行整体退守。 */
        log_warning("[时间校准] %s.%s: 试解探针自报失败(试解 %d→%.2fms, "
                    "%d→%.2fms) → 退守校准下限 %ld",
                    test_id, knob, p1, t1, mid, t2, (long)lo);
        return lo;
    }
    if (t1 <= 0.0 || t2 <= 0.0) {
        /* 任一档计时不可测即无法拟合 k。仅当两档皆不可测(t1 = t2 = 0)时,
         * 才能断定"时间不是约束"(求解快到连时钟都读不出), 此时交还内存
         * 上限是安全的。若只是一档读数为 0, 按『上限宁小勿大』退守校准
         * 下限 lo——与两个调用方 catch (...) → lo 的方向一致。绝不在此
         * 交还未校准的 hi: 那会静默复活分钟级、TERM 打不断的单次求解,
         * 正是本函数要消除的故障模式。 */
        const bool both_unmeasurable = (t1 <= 0.0 && t2 <= 0.0);
        log_warning("[时间校准] %s.%s: 试解计时不足(%.2f/%.2fms) → %s %ld",
                    test_id, knob, t1, t2,
                    both_unmeasurable ? "维持内存上限" : "退守校准下限",
                    both_unmeasurable ? (long)hi : (long)lo);
        return both_unmeasurable ? hi : lo;
    }
    double k = std::log2(t2 / t1);
    if (k < 1.5) k = 1.5;
    if (k > 4.0) k = 4.0;
    /* k_floor: 求解器复杂度下限(稠密 SVD ~3.0; 随机 ~10% 密度稀疏
     * Cholesky 的 fill-in 近稠密, 理论同 ~3.0——随机谱系实测
     * 3.3~3.5, 而旧口径误写 2.0)。小档 probe 的实测 k 偏低(BDCSVD
     * 64→128 实测 1.8——预处理开销主导的小尺度行为), 直接外推会把
     * 大维度单次求解低估几个数量级(实测外推出 2048 维"1 秒内",
     * 实际数分钟)。取 max(实测, 理论)。 */
    if (k < k_floor) k = k_floor;
    double budget = (double)max_single_ms * 0.5;   /* 安全系数 0.5 */
    int dim = (int)((double)p1 * std::pow(budget / t1, 1.0 / k));
    if (dim < lo) dim = lo;
    if (dim > hi) dim = hi;
    /* 外推后验证探针(2026-10-24 检视 #11): 幂律外推与 k_floor 都是
     * 启发式, 失准时单解可远超预算(检视实证: 大内存主机旧口径外推
     * 落点单解 8.3s >> 2s)。在选定 dim 再实测一次, 超预算按实测幂律
     * 收缩并复验——构造性保证 one solve ≤ max_single_ms; 收缩到 lo
     * 仍超限则如实记录(机器极慢/lo 过大, 后果与显式 knob 同级)。
     * newdim 强制单调递减吸收计时噪声, 轮数上限 4 防抖动。
     * fit_only(issue #487)整档跳过: 该保证守护的是返回值, 而联合
     * 求解弃用返回值只取 fit——跳过后返回未验证外推档。 */
    /* 显示值按断言方向选截断口径(PR #377 检视 id=4441216644/
     * 4463472452/4463472461/4463831060): round 渲染会把窗口内的
     * 实测值渲染到断言对面——下界断言(t1 < sdc_m3_probe_floor_ms)
     * 在 t1∈[99.95,100) 渲染成 100.0(printf("%.1f", 99.97)="100.0"),
     * 上界断言(td0/td > max_single_ms)在 (max, max+0.05) 渲染成
     * max(printf("%.1f", 2000.04)="2000.0"), 打出『实测 2000.0ms
     * 超预算』的显示值=预算却断言超——同一误导形态的两个方向。
     * 修法对称: 下界断言配 floor(显示值 ≤ 实测值 ⇒ 显示<阈值恒真),
     * 上界断言配 ceil(显示值 ≥ 实测值 ⇒ 显示>阈值恒真, 阈值取整十
     * 值时严格 >)。t1/t2 另有全流一致与单调保持要求(汇总行/M3
     * 前提告警/差分退化告警同一测量同一数字; t2≤t1 的倒挂不被
     * 渲染成正挂——round 无此保证)——fit 消费链(联合求解)向下
     * 截断; 存量 7 参链(fit=nullptr, x86-64 #else)不经 M3/退化
     * 告警, 无矛盾可消, 保持 f84966bc~1 的 %.1f 原渲染基线零漂移
     * (rule 5: 不随 ARM 侧口径联动, 显示亦然)。代价均为偏离至多
     * 一个显示精度(0.1ms), 对诊断处方无损。td0 告警行(断言为真才
     * 打印)与汇总行超预算后缀为上界断言语境, 两条链都会出现且矛盾
     * 形态相同——td0 一律 ceil, td 仅在超预算(断言为真)时 ceil,
     * 正常路径保持原渲染零漂移。上提至验证 wrap 之外: wrap 之下的
     * M3 前提告警/差分退化告警仍引用 t1_disp/t2_disp, fit-only 路径
     * 的告警与汇总行亦须同一测量同一数字(issue #487)。 */
    const double t1_disp = fit ? std::floor(t1 * 10.0) / 10.0 : t1;
    const double t2_disp = fit ? std::floor(t2 * 10.0) / 10.0 : t2;
    if (!fit_only) {
        int xdim = dim;
        double td = probe(dim);
        double td0 = td;
        int shrinks = 0;
        while (td > (double)max_single_ms && dim > lo && shrinks < 4) {
            int newdim = (int)((double)dim * std::pow(budget / td, 1.0 / k));
            if (newdim >= dim) newdim = dim - 1;
            if (newdim < lo) newdim = lo;
            dim = newdim;
            ++shrinks;
            td = probe(dim);
        }
        if (td0 > (double)max_single_ms)
            log_warning("[时间校准] %s.%s: 初选 %d 实测 %.1fms 超预算 → 按实测幂律收缩 %d 轮",
                        test_id, knob, xdim,
                        std::ceil(td0 * 10.0) / 10.0, shrinks);
        /* td 仅在超预算(断言为真)时 ceil——正常路径保持原渲染, 存量链
         * 基线零漂移; 断言为假时无『仍超预算』后缀, 无矛盾语境。 */
        const double td_disp =
            (td > (double)max_single_ms) ? std::ceil(td * 10.0) / 10.0 : td;
        log_warning("[时间校准] %s.%s: 单次求解预算 %ldms (试解 %d→%.1fms, %d→%.1fms, "
                    "k=%.2f) → %d (验证 %.1fms%s)",
                    test_id, knob, max_single_ms, p1, t1_disp, mid, t2_disp, k, dim,
                    td_disp,
                    td_disp < 0.0
                        ? ", 验证探针失败(维度未经复核)"
                        : td > (double)max_single_ms
                        ? (dim <= lo ? ", lo 档仍超预算" : ", 收缩轮数上限后仍超预算")
                        : "");
    } else {
        log_warning("[时间校准] %s.%s: 试解 %d→%.1fms, %d→%.1fms, k=%.2f → %d "
                    "(fit-only: 跳过验证档, 未验证外推值)",
                    test_id, knob, p1, t1_disp, mid, t2_disp, k, dim);
    }
    if (fit) {
        /* M3 有效性告警: 差分扣 b 的前提是 probe 档**实测时长** t1 ≥
         * sdc_m3_probe_floor_ms(固定开销占比 <10%, spec §4.3 M3)——
         * 判据取实测 t1 而非入参 probe_floor_ms(PR #377 检视: 只查入参
         * 两个方向都判错——floor=100.0 但 hi 截断致档位抬不上去同样是
         * 致噪源; 反之慢求解器在缺省 1.0 下 lo 档实测已 ≥100ms, 差分
         * 本干净, 不应告警)。实测 t1 不足时 a_ms 虚高 → n_run 偏小 →
         * 激发维度静默欠压, 在此从静默变有声。
         * 阈值锚定 spec §4.3 M3 方法口径而非调用方实参(R4 检视答疑):
         * 100ms 是差分扣 b 方法本身的档位判据, 非逐求解器可调——显式
         * 传 sub-100 实参仍告警是有意设计(偏离 spec 的差分口径应被看
         * 见; 固定开销 b 显著更小的求解器应先修订 spec M3 标定, 常量
         * 已单点绑定)。文案按传参是否已达 M3 口径分流: 已达标者唯一
         * 残余致因是 knob hi 截断, 不再误指传参。 */
        if (t1 < sdc_m3_probe_floor_ms) {
            /* t1 显示用上方汇总行同源的 t1_disp(fit 链恒为向下截断
             * 口径, 见其定义处注释)——告警行断言与汇总行数字恒一致。 */
            if (probe_floor_ms >= sdc_m3_probe_floor_ms)
                log_warning("[时间校准] %s.%s: probe 档实测 %.1fms < %.0fms——"
                            "knob hi 截断致档位抬不上去, " SDC_M3_FIT_WARN_BODY
                            "检查 knob hi 是否足以抬升 probe 档", test_id,
                            knob, t1_disp, sdc_m3_probe_floor_ms);
            else
                log_warning("[时间校准] %s.%s: probe 档实测 %.1fms < %.0fms——"
                            SDC_M3_FIT_WARN_BODY
                            "检查 probe_floor_ms 传参(M3 消费方应显式传 %.0f)"
                            "与 knob hi 是否足以抬升 probe 档", test_id, knob,
                            t1_disp, sdc_m3_probe_floor_ms, sdc_m3_probe_floor_ms);
        }
        /* 两点差分扣除固定开销 b(M3): t = a·n^p + b ⇒
         *   a = (t2−t1)/(mid^p − p1^p),   b = t1 − a·p1^p
         * b≈0 时严格退化为 a = t1/p1^p。差分在 b 为常数的模型下精确,
         * 常规档(b 占比 <10%, 与联合求解显式传入的 sdc_m3_probe_floor_ms
         * 配套)与 b 主导的对抗档都成立; 代价是对差分 (t2−t1) 的测量
         * 噪声敏感(p1/mid 档位拉得越开越稳)。退化保护(档位重合/计时
         * 持平/非有限/非正)回退
         * b=0 口径 a = t1/p1^p —— t1 含全部固定开销, 系数只高不低、
         * 消费方 n_run 只小不大: 时间预算侧保守, SDC 激发强度侧欠压
         * (与上方 t1<floor 告警同型污染), 故回退从静默变有声。 */
        double pn = std::pow((double)p1, k);
        double mn = std::pow((double)mid, k);
        double a = 0.0;
        if (mid > p1 && t2 > t1 && mn > pn && std::isfinite(mn - pn))
            a = (t2 - t1) / (mn - pn);
        if (!(a > 0.0) && pn > 0.0) {
            /* 差分退化告警(PR #377 检视): 非正差分此前零告警——t1 ≥
             * floor 时 M3 前提告警不触发, 而档位重合(hi 截断使
             * mid==p1)或计时持平/倒挂(系统抖动、非单调求解器)令差分
             * 退化为 0/负, b=0 口径把固定开销全数计入 a_ms(a_ms 虚高
             * →n_run 偏小→激发欠压)。两档实测随告警留档供定位; 处方
             * 按两类致因: 拉大 knob hi 档距(重合)或复跑(持平)。
             * 已知边界(R6 检视裁定): 噪声偏置的弱正差分(t2>t1 但
             * (t2−t1) 信噪比不足)不在本告警与 M3 前提告警覆盖内——
             * 差分对测量噪声固有敏感(见上方代价声明), M3 口径
             * (floor≥100ms+档距 2×, k_floor=3.0 下健康比值 ~8×)把
             * 该场景压在 ±10-30% 固有抖动量级(反向膨胀被消费端
             * min(n_mem, n_run, hi) 封顶; 128 核高载机实测比值仍
             * 7.1×/8.3×)。收口须先按 spec §4.3 M3 标定差分信噪比
             * 判据(如实测 k 距 k_floor 的带阈), 不在此增设无 spec
             * 依据的阈值——告警疲劳稀释真事件信号(R4/#23 同裁定)。 */
            log_warning("[时间校准] %s.%s: fit 两点差分退化(档 %d→%d 实测 "
                        "%.1fms→%.1fms, 档位重合或计时持平/倒挂)——回退 "
                        "b=0 口径 a=t1/p1^k, 固定开销未扣除, a_ms 虚高→"
                        "n_run 偏小→激发欠压; 检查 knob hi 拉大档距或复跑"
                        "消除计时抖动", test_id, knob, p1, mid, t1_disp,
                        t2_disp);
            a = t1 / pn;             /* t1>0 由上方早退保证 */
        }
        if (a > 0.0 && std::isfinite(a)) {
            fit->a_ms = a;
            fit->p = k;
        }
    }
    return dim;
}

/* 7 参存量转发重载(等价旧缺省 {nullptr, 1.0}): 跨 arch 存量链(含
 * x86-64 #else 分支)调用点源码零改动(rule 5)——1.0 即 f84966bc 之前
 * 的历史噪声底语义。 */
template <typename F>
static inline int sdc_calibrated_dim(int lo, int hi, long max_single_ms,
                                     const char *test_id, const char *knob,
                                     double k_floor, F solve_once)
{
    return sdc_calibrated_dim(lo, hi, max_single_ms, test_id, knob, k_floor,
                              solve_once, nullptr, 1.0);
}

/* 第 8 参 std::nullptr_t 重载: 『显式不要 fit』的字面量 0/NULL 旧写法
 * 绑定于此(而非经 int→double 隐式转换静默重绑 probe_floor_ms=0.0),
 * 语义 = 7 参链。 */
template <typename F>
static inline int sdc_calibrated_dim(int lo, int hi, long max_single_ms,
                                     const char *test_id, const char *knob,
                                     double k_floor, F solve_once,
                                     std::nullptr_t)
{
    return sdc_calibrated_dim(lo, hi, max_single_ms, test_id, knob, k_floor,
                              solve_once, nullptr, 1.0);
}

/* ------------------------------------------------------------------ */
/* 联合求解: 时间×内存双上限维度(spec §4.3)                              */
/* ------------------------------------------------------------------ */

/* RSS_RATIO_TABLE 键: 算法族。 */
enum sdc_rss_family {
    SDC_RSS_BDCSVD_F32, SDC_RSS_BDCSVD_F64, SDC_RSS_BDCSVD_C128,
    SDC_RSS_JACOBI_F32, SDC_RSS_JACOBI_F64, SDC_RSS_JACOBI_C128,
    SDC_RSS_GEMM_F32, SDC_RSS_GEMM_F64, SDC_RSS_GEMM_C128,
    SDC_RSS_SPARSE, SDC_RSS_INVALID
};

/* 族 → 实测 c(KB/n²/线程)。Phase A 2026-10-27, (child@T4−child@T1)/3
 * 差分增量口径——与 svd_cdouble_sve.cpp:92-95 的"单进程 9.7-10× 输入
 * 体积"(含基线, ~155B/n²)口径不同, 差异 ~35% 属口径差非矛盾: 联合
 * 求解的增量模型取差分口径。
 * c 的可移植性(issue #511 勘误, 2026-11-08): 主体(≈矩阵驻留数×元素
 * 大小, 如 GEMM_F64 4 矩阵×8B=0.03125)是算法内在、一阶跨机近似; 但
 * 差分口径只测 n² 斜率项, 漏计非 n² 缩放驻留(Eigen packing buffer、
 * 联合求解探针高水位、分配器 slack)——VmHWM 实测隐含 c 同机即偏高:
 * GEMM_F64 重标定前 +8.8%(T6), 重标定后 61G 单节点机 +1.3%(max_arm)、
 * 128C/30G 四节点机 +3.4~4.1%(NEON dynamic_square, 2026-11-08 实测),
 * 残差随机器/用例漂移, 不构成"逐机精确"。表值是初始锚: 精度由 T7
 * VmHWM 反馈闭环同机收敛(向上修正无死区, spec §4.4), 跨机首跑的
 * ±5% 级残差同样由反馈吸收。Eigen 版本升级需重标定本表。 */
static const double s_rss_ratio_table[SDC_RSS_INVALID] = {
    0.0669,   /* SDC_RSS_BDCSVD_F32  */
    0.1352,   /* SDC_RSS_BDCSVD_F64  */
    0.2055,   /* SDC_RSS_BDCSVD_C128 */
    0.0313,   /* SDC_RSS_JACOBI_F32  */
    0.0626,   /* SDC_RSS_JACOBI_F64  */
    0.1237,   /* SDC_RSS_JACOBI_C128 */
    0.0159,   /* SDC_RSS_GEMM_F32    */
    /* GEMM_F64 0.0290 → 0.0315 (T9, T6 裁定的重标定): 差分口径曾漏计
     * 第 4 个驻留矩阵(run 期 reroll 乘积 x)——解析值 4 矩阵×8B =
     * 0.03125 KB/n²/线程; T6 实测隐含 0.0316(N=3364 峰值子进程 RSS
     * 43.31 GiB vs 模型 39.8 GiB, +8.8%), T7 反馈 eff 4.104~4.116 →
     * c_fb≈32.8 B(+10.5%, 含探针高水位 ~4.7% 口径)。取 0.0315。
     * 同源核对(只动 F64): F32 0.0159 vs 4×4B=0.015625 偏大 1.8%、
     * C128 0.0631 vs 4×16B=0.0625 偏大 1.0%, 均在 +2% 内不动。 */
    0.0315,   /* SDC_RSS_GEMM_F64    */
    0.0631,   /* SDC_RSS_GEMM_C128   */
    0.0091,   /* SDC_RSS_SPARSE      */
};

static inline double sdc_rss_table_c(enum sdc_rss_family f)
{
    if ((int)f < 0 || (int)f >= (int)SDC_RSS_INVALID)
        return 0.0;
    return s_rss_ratio_table[f];
}

/* ------------------------------------------------------------------ */
/* VmHWM 反馈闭环的记账缓存 (T7, spec §4.4)                              */
/* ------------------------------------------------------------------ */

/* sdc_joint_dim_slice 求解成功后把本次口径登记于此(键 = test->id), 供
 * run_one_test_inner 尾部的反馈写入点折算 eff_ratio =
 * VmHWM/(n²·elem·threads)。进程内静态 map: 联合求解与写入点同在 child
 * 进程内(test_init 的 aux 线程 → run 结束), 同一份数据; 跨 fracture 的
 * 持久化靠 shmem 的 MemFeedback 槽, 不靠本 map(每 fracture 的 child 都是
 * 从 parent 重新 fork/exec 的, map 不继承 —— preinit(parent)侧求解不会
 * 把口径带给 exec child, 已知边界)。未走联合求解(显式 knob / 未迁移
 * 用例)无条目 ⇒ 无反馈写入。 */
struct sdc_joint_ctx {
    int n;              /* 求解出的最终维度 */
    int threads;        /* 本 slice worker 数 */
    int elem_bytes;     /* a->bytes_per_elem 记账口径(写读同源, 相消) */
    double c;           /* 最终生效 c(字节/n²/线程, 含反馈修正) */
};

/* 注意链接: 这里必须用【外部链接的 inline】(不能学 sdc_knob_cache 的
 * static inline) —— 写入方是测试 TU(test_init 里调 sdc_joint_dim_slice),
 * 读取方是框架 TU(sandstone_run.cpp 的写入点), static inline 的内部链接
 * 会让两个 TU 各持一份 map, 闭环静默断裂(实测首版即栽在此)。 */
inline std::map<std::string, struct sdc_joint_ctx> &sdc_joint_cache()
{
    static std::map<std::string, struct sdc_joint_ctx> cache;
    return cache;
}

/* T10 降档路径的反馈口径失效(issue #545; 取代 PR#302 检视 #25 的
 * 『同步分母』修复): init 期 bad_alloc 降档到 kInitRetryLo 重试成功后,
 * run_one_test_inner 尾部的反馈写入点按 ctx.n 折算 eff_ratio
 * = VmHWM/(n²·elem·threads)。分子 VmHWM 是本 child 进程的峰值, 包含
 * 降档前部分成功的 O(n²) 分配(d->lhs 已建成、d->rhs 才抛 bad_alloc);
 * 只把分母 ctx.n 改成 retry 值会把 eff 高估 (n_solved/retry)² 倍
 * (n=3500/retry=16 约 48000×)。读侧向上修正无死区, 一次瞬时 OOM 会被
 * 直接采纳为 c, 后续 fracture 的 n 被压向 lo、压力静默塌缩(EMA α=0.5
 * 下还要 ~18 个 fracture 才衰减回 ±15% 死区)。
 * 分子无法按 retry 口径折算: issue #385 的基线扣除只减去 fork 起点
 * (父进程 RSS), 降档前那次部分成功的 O(n²) 分配发生在基线**之后**,
 * 仍留在分子里; 4×锚上钳只把污染上限压到 4×(不锁死), 不能恢复真实
 * 工作集。因此降档 child 诚实的选择是不发布样本: 注销本 child 的联合
 * 求解登记, sdc_feedback_write 见无条目即不写。下一 fracture 重新求解,
 * 不受本次瞬态事件污染。显式 knob/
 * 未迁移用例无登记条目时为 no-op。init 双重失败(EXIT_SKIP)不需要:
 * run_one_test_inner 对 init SKIP 提前返回, 到不了反馈写入点。已知
 * 残差不变(如实记录): worker 期 bad_alloc 的线程级 SKIP 仍按求解 n
 * 记账(VmHWM 是不完整分配的峰值)——低估方向受读侧 0.7× 锚下限钳制
 * (n 至多膨胀 1/√0.7≈1.2×), 且生产构建 malloc 插桩拦截空分配(仅
 * ASAN 可达), 不单独设通道。 */
static inline void sdc_joint_cache_drop(struct test *test)
{
    if (!test || !test->id)
        return;
    auto it = sdc_joint_cache().find(test->id);
    if (it != sdc_joint_cache().end())
        sdc_joint_cache().erase(it);
}

/* framework/sandstone_run.cpp 提供(arm64+linux 编译): 遍历全部 slice 的
 * PerThreadData::Main.mem_feedback 槽(MAP_SHARED shmem, 子进程经
 * raw_main_thread_data 读兄弟槽), 取同 test_id 哈希槽 eff_ratio 的
 * median(q4: max 取最噪样本, median 防抖)。无有效槽(首 fracture/无写入
 * 者)返回 <0。仅 arm64 联合求解分支调用 —— x86 无定义亦无调用者。 */
double sdc_feedback_eff_median(const char *test_id);

struct sdc_joint_args {
    int lo, hi;                     /* knob clamp */
    enum sdc_rss_family fam;        /* SDC_RSS_INVALID=用传统三元组 */
    int bytes_per_elem, matrices_per_thread; double rss_ratio; /* 传统路径 */
    double shared_bytes_per_slice;  /* init 共享矩阵, 0 无 */
    int mem_fraction_permille;
    enum sdc_mem_family ar_fam;
    long run_budget_ms;             /* 全核单迭代(2解×AR)预算 ms(AR:
                                       workers==1 取 1, 见下方闸门; issue #563),
                                       调用方传 200000(spec: 300s 宽限×0.67≈200s);
                                       0=不限时间 */
};

/*
 * 联合层的 knob 读取(T11, P2b): cache 优先——常规调用方在自身 init 里已
 * 用 sdc_knob_int_clamped 查询过同一 (test, knob), 值已在 cache 且
 * [压力映射] 日志已打过, 重查会重复打日志; cache 未命中(调用方没经
 * sdc_knob_int_clamped 的非常规路径)才完整查询+缓存。返回语义与
 * sdc_knob_int_clamped(def=0) 一致: 0 = 未显式配置(-O/配置文件通道)。
 * env 兜底通道不经过本函数(sdc_env_knob_int_clamped 不写 cache), 由
 * sdc_joint_dim_slice 的 knob 早退块自行补查。
 *
 * issue #692: cache 键(test->id+"."+key)不含边界——命中值是按首次查询
 * 调用点的边界 clamp 的结果, 不保证在本次调用的 [lo, hi] 内; 边界不同
 * 的二次查询会静默拿到按旧边界 clamp 的越界值。故命中后按本次边界重
 * clamp(0 = 未显式配置哨兵, 原样透传, 不参与 clamp), 越界时有声告警
 * ——调用方边界分歧是缺陷信号, 不静默截断; 重 clamp 结果不回写 cache
 * (缓存的是首次查询口径, 回写会让另一边界口径的读者失真)。现状核实
 * (2026-11-17): 全部调用方预热边界与 ja.lo/hi 一致, 且显式值都在调用
 * 方 init 短路——命中路径今天只返回 0, 本重 clamp 是对未来调用方形态
 * 的防御, 既有行为逐位不变。
 */
static inline int64_t sdc_joint_knob_value(struct test *test, const char *key,
                                           int64_t lo, int64_t hi)
{
    auto it = sdc_knob_cache().find(sdc_knob_cache_key(test, key));
    if (it != sdc_knob_cache().end()) {
        int64_t v = it->second;
        if (v != 0 && (v < lo || v > hi)) {
            int64_t clamped = v < lo ? lo : hi;
            log_warning("%s: %s=%ld 越界 (有效 %ld..%ld; cache 键不含边界, "
                        "与首次查询调用点的边界分歧, issue #692), "
                        "已重 clamp 为 %ld",
                        test ? test->id : "(global)", key, (long)v,
                        (long)lo, (long)hi, (long)clamped);
            return clamped;
        }
        return v;
    }
    return sdc_knob_int_clamped(test, key, 0, lo, hi);
}

#if defined(__aarch64__) && defined(__linux__)
/* issue #385: 反馈向上采纳的上钳倍数(相对静态锚 c_anchor)。
 *
 * 背景: 写侧基线扣除(sandstone_run.cpp sdc_feedback_write)已消掉 fork RSS
 * 这个最大污染源, 但读侧「向上无上钳」仍是一条开放通道——任何残余异常
 * 样本(seqlock 混合态、探针高水位主导的小 n 记账、未来新增的污染源)都
 * 会被全额采纳并进入 EMA, 而向下修正受 15% 死区 + 0.7×锚下限保护, 膨胀
 * 值只能靠后续样本半衰, 期间 n 被压向 lo(与 09c9c4fc「堵反馈通道污染」
 * 的意图直接矛盾)。
 *
 * 阈值选择依据(为何是 4×):
 *   - 正常路径实测相对静态表的偏差 ≤ ~10.5%(T6/T9 GEMM_F64 重标定: 表
 *     0.0290→0.0315 KB, 实测隐含 +10.5%); 4× = 400% 留出 ~38× 余量, 正常
 *     大 n×多线程路径永不触发(基线占比 <1%, 探针占比 <1%)。
 *   - 扣除基线后分子残余的合法成分是联合求解探针高水位: n_mem 受限路径
 *     里探针 ≪ 正式工作集; 只有 n_run 受限的小 n 路径可能让探针占比升到
 *     静态锚的 ~2-3×, 4× 仍不误伤。
 *   - 上钳要挡的污染量级: #385 复算的 n=16×1 线程基线污染 = 32422 B vs
 *     GEMM_F64 锚 32.256 B ≈ 1000×; 4× 把单样本放大从 1000× 压到 ≤4×,
 *     配合 EMA α=0.5(4 个正常样本后残余 4×/16=0.25×)与 15% 死区, 不再
 *     永久锁死 c。
 *   - 锚 = 表值时它本身就是「矩阵数×elem」的内存模型上限附近; 反馈要求
 *     超过 4 倍驻留矩阵只能说明模型或样本之一错了, 应当有声地拒绝而非
 *     静默采纳(见下方 clamped 告警)。 */
static constexpr double sdc_feedback_up_cap_factor = 4.0;

struct sdc_feedback_decision {
    double c;          /* 采纳后的 c; 未采纳时 = 入参 c_bytes */
    double cand_raw;   /* 死区/0.7×锚下限处理后的候选(上钳前) */
    bool adopted;
    bool upward;
    bool clamped;
};

/* 反馈采纳决策(纯函数, 便于机制级验证): 与 #385 之前的差异仅在
 * 「向上采纳前先钳到 up_cap」; 死区/下限/采纳判据逐字保留。 */
static inline struct sdc_feedback_decision
sdc_feedback_decide_c(double c_bytes, double c_fb, double anchor)
{
    struct sdc_feedback_decision d = { c_bytes, 0.0, false, false, false };
    double cand = std::max(c_fb, 0.7 * anchor);
    d.cand_raw = cand;
    double up_cap = sdc_feedback_up_cap_factor * anchor;
    if (up_cap > 0.0 && cand > up_cap) {
        cand = up_cap;
        d.clamped = true;
    }
    d.upward = cand >= c_bytes;
    bool outside_deadzone = cand < 0.85 * c_bytes;
    d.adopted = d.upward || outside_deadzone;
    if (d.adopted)
        d.c = cand;
    return d;
}
#endif /* __aarch64__ && __linux__ */

/*
 * 联合求解 n_opt = min(n_mem, n_run, hi), lo 兜底:
 *   n_mem = sqrt((node_budget − node_slices·shared) / (node_threads · c))
 *   n_run = (run_budget_ms / (AR(ar_fam) × 2 × fit.a_ms))^(1/fit.p)
 * 时间侧: run_budget_ms 即全核单迭代预算(×2 = init 填充+一次求解,
 * ×AR = 全核争抢慢化(族×拓扑, spec M2); AR 在 workers==1 时取 1——单
 * worker 自争抢为 1, 表值是全核口径, issue #563); 调用方传 200000 = 300s
 * 硬宽限 ×0.67(spec M1), 本函数不再二次折算——200s 已是折算后的值);
 * fit 来自 sdc_calibrated_dim 的两点差分(已扣固定开销, M3), 无效时跳过
 * n_run。
 * 内存侧 c 优先级链(T7 起): VmHWM 反馈 median(跨 fracture 的 Main 槽
 * EMA, 详见 sdc_feedback_eff_median; 向上无死区但 4×静态锚上钳、向下
 * 15% 死区 + 0.7×静态锚下限) → RSS_RATIO_TABLE(族) → 调用方传统三元组
 * (bytes_per_elem×matrices_per_thread×rss_ratio)。
 * knob 三层 fallback(T11 起): arm64 分支持有 knob 早退——显式 -O/env 值
 * 在此早退并打预期内存档位警告(见函数内 T11 块); 调用方迁移后
 * (calibrated_auto_dim 模式)显式值也路由进本函数。未迁移调用方仍在
 * 自身 init 里短路(无警告, 见 T11 块尾注)。quick 模式直接返回 lo
 * (m3, 与 sdc_auto_square_dim 同捷径; 但位于 knob 早退之后——显式值
 * 优先, 与迁移前调用方短路语义一致)。
 * solve_once 可为 NULL(不校时间, 只算 n_mem); run_budget_ms=0 同理。
 * x86-64(#482): 全局四源预算 + sdc_calibrated_dim 时间校准(AR=1),
 * 无 per-node/AR/反馈——见函数内 #else 分支; 现行 x86 调用点均 guard
 * 挡在旧链上, 该分支是未来跨 arch 迁移的安全落点(行为边界详见 #else
 * 注释)。
 */
static inline int sdc_joint_dim_slice(struct test *test, const char *knob,
                                      const struct sdc_joint_args *a,
                                      int solve_once(int))
{
    if (!a)
        return 0;
    /* permille/c 提前到 knob/quick 早退之前(T11): knob 早退的预期内存
     * 警告需要 c; 纯计算无副作用, 相对退化区间/quick 检查的顺序提升
     * 零行为差(两条早退路径原本就不读这两个值)。 */
    int permille = a->mem_fraction_permille;
    if (permille < 1) permille = 500;
    if (permille > 1000) permille = 1000;
    /* c(KB/n²/线程 → 字节): 表优先, 未迁移用例走传统三元组 */
    double c_table = sdc_rss_table_c(a->fam) * 1024.0;   /* 0 = 无表 */
    double c_bytes = (a->fam != SDC_RSS_INVALID)
            ? c_table
            : (double)a->bytes_per_elem * (double)a->matrices_per_thread
              * a->rss_ratio;

#if defined(__aarch64__) && defined(__linux__)
    /* ---- T11 (P2b): 显式 knob 早退——预期合计内存档位警告 ----
     * 显式 -O/配置文件/env 值完全旁路联合求解(时间校准/自适应都不跑,
     * 用户自担), 此前无任何内存预期提示。早退前按本函数的模型口径算
     *   预期合计 = 全机线程 × c × n² + slices × shared
     * 与全局四源预算 sdc_budget_bytes(permille) 比对: 超出 → [内存越界],
     * 不足一半 → [欠压力], 中间档安静。口径:
     *   - threads 用 sdc_total_threads()(全局): 显式 knob 在 init 查询
     *     时 thread_count() 是 slice 口径, 预算按全机分摊(与
     *     sdc_auto_square_dim 同基准);
     *   - c 即上方 c_bytes(表优先, fam 无效时调用方三元组换算——统一算);
     *   - budget 用全局口径而非节点预算: 显式值不参与节点分摊, 全机
     *     合计对全机预算才是用户该看到的数;
     *   - knob 读取走 sdc_joint_knob_value(cache 优先, 不重复查询/打
     *     日志), env 兜底在此补查(-O 优先, 与调用方原 fallback 序一致)。
     * 位于退化区间/quick 早退**之前**: 显式值优先于 quick 收缩, 与迁移
     * 前调用方短路语义一致(用户显式指定 > 一切自动收缩)。警告不打断
     * 执行——显式值仍按用户指定跑。
     * 边界: 未迁移调用方(gemm/sparse/sve 族的调用方短路)不经此处,
     * 显式路径仍无警告; 采纳本机制只需删掉调用方短路、显式值照常
     * 路由进本函数(SVD 模板族 calibrated_auto_dim 即此模式)。 */
    {
        int64_t kv = sdc_joint_knob_value(test, knob, a->lo, a->hi);
        if (kv == 0 && test && test->id)
            sdc_env_knob_int_clamped(test->id, knob, &kv, a->lo, a->hi);
        if (kv > 0) {
            int total = sdc_total_threads();
            if (total < 1)
                total = 1;
            int cur = thread_count();
            int slices = (cur > 0 && total > cur) ? (total + cur - 1) / cur : 1;
            double expect = (double)total * c_bytes * (double)kv * (double)kv
                          + (double)slices * (double)a->shared_bytes_per_slice;
            double budget = (double)sdc_budget_bytes(permille);
            const double gib = 1073741824.0;
            if (c_bytes > 0.0 && budget > 0.0 && std::isfinite(expect)) {
                if (expect > budget)
                    log_warning("[内存越界] %s.%s: 显式 %ld 预期合计 %.1f GiB 超出 "
                                "%d‰ 预算 %.1f GiB — 时间校准/自适应均已旁路(用户自担)",
                                test ? test->id : "(global)", knob ? knob : "?",
                                (long)kv, expect / gib, permille, budget / gib);
                else if (expect < budget * 0.5)
                    log_warning("[欠压力] %s.%s: 显式 %ld 预期合计仅 %.1f GiB "
                                "(%d‰ 预算的 %.0f%%) — 可上调或走自适应",
                                test ? test->id : "(global)", knob ? knob : "?",
                                (long)kv, expect / gib, permille,
                                100.0 * expect / budget);
            }
            return (int)kv;
        }
    }
#endif

    if (a->hi <= a->lo)
        return a->lo;                      /* 退化 clamp 区间 */
    if (sdc_quick_run()) {                 /* CI 冒烟: 与 auto_* 同捷径 */
        log_warning("[联合求解] %s.%s = %ld (来源: quick 模式收缩, 取 lo 下限)",
                    test ? test->id : "(global)", knob ? knob : "?",
                    (long)a->lo);
        return a->lo;
    }

#if defined(__aarch64__) && defined(__linux__)
    /* ---- VmHWM 反馈读侧 (T7, spec §4.4): c 优先级 反馈median → RSS 表
     * → 传统三元组。槽内 eff_ratio = VmHWM/(n²·elem·slice线程)(EMA α=0.5,
     * 写入点在 run_one_test_inner 尾部), ×elem 还原字节口径; 写/读用同一
     * a->bytes_per_elem, 记账口径相消(调用方 elem 前后不一致也不失真)。
     * 采纳规则:
     *   - 向上修正(c_fb ≥ 模型): 无死区(表系统性偏小时靠它收敛——T6 实测
     *     GEMM_F64 表偏小 ~8%; 压 n 是安全方向), 但自 issue #385 起有
     *     4×静态锚上钳: 单次异常样本不能把 c 永久锁在污染值上(阈值依据见
     *     sdc_feedback_up_cap_factor 注释)。上钳生效时单独告警, 原始候选
     *     一并打印, 不静默截断;
     *   - 向下修正: 需越出 15% 死区才采用(反馈噪声/m1 选择偏差——超时
     *     child 到不了写入点, 样本偏乐观——都朝压小 c 的方向, 小幅下压
     *     一律不信), 且下限 = 静态锚 × 0.7(锚 = 表值, 无表用三元组),
     *     防反馈把 c 压小导致 n 膨胀(OOM 方向)。 */
    {
        double eff_med = sdc_feedback_eff_median(test ? test->id : nullptr);
        if (eff_med > 0.0 && a->bytes_per_elem > 0) {
            double c_fb = eff_med * (double)a->bytes_per_elem;
            double anchor = c_table > 0.0 ? c_table : c_bytes;  /* 静态锚 */
            struct sdc_feedback_decision d =
                    sdc_feedback_decide_c(c_bytes, c_fb, anchor);
            if (d.clamped) {
                log_warning("[联合求解] %s.%s: 反馈样本 c_fb=%.1f 超静态锚 %.1f "
                            "的 %.0f× → 上钳至 %.1f B/n²/线程 (原始候选 %.1f; "
                            "issue #385)",
                            test ? test->id : "(global)", knob ? knob : "?",
                            c_fb, anchor, sdc_feedback_up_cap_factor, d.c,
                            d.cand_raw);
            }
            if (d.adopted) {
                log_warning("[联合求解] %s.%s: VmHWM 反馈生效 c=%.1f→%.1f B/n²/线程 "
                            "(eff=%.3f, %s, 下限 %.1f)",
                            test ? test->id : "(global)", knob ? knob : "?",
                            c_bytes, d.c, eff_med,
                            d.upward ? "向上修正, 4×锚上钳" : "越出15%死区",
                            0.7 * anchor);
                c_bytes = d.c;
            } else {
                log_warning("[联合求解] %s.%s: VmHWM 反馈 c=%.1f 落入 ±15%% 死区, "
                            "维持静态值 %.1f",
                            test ? test->id : "(global)", knob ? knob : "?",
                            c_fb, c_bytes);
            }
        }
    }
    /* 本 slice 归属 cpu: 亲和集最小 cpu(spec m4 近似——slice 跨节点时
     * 取首 cpu 的节点); 亲和不可读退当前 cpu。 */
    int cpu = -1;
    {
        cpu_set_t set;
        if (sched_getaffinity(0, sizeof set, &set) == 0)
            for (int i = 0; i < CPU_SETSIZE; ++i)
                if (CPU_ISSET(i, &set)) { cpu = i; break; }
        if (cpu < 0)
            cpu = sched_getcpu();
    }
    uint64_t node_budget = sdc_node_budget_bytes(permille, cpu);
    int node_cpus = sdc_numa_effective_threads(cpu);
    if (node_cpus < 1)
        node_cpus = 1;                     /* T2 保证 ≥1, 双保险 */
    /* node_threads 用 worker 口径(spec §4.3 "该节点全部 worker 数"):
     * 折算域 = 框架选中设备集(issue #515)——启动亲和(taskset)与
     * --cpuset/--deviceset 过滤后的 shmem->device_info[], 即 worker 真实
     * 分布域; 旧「全机在线 CPU 均匀分布」假设在非均匀 --cpuset 下把
     * node_cpus 虚折(本机 192CPU/8节点: 24 worker 全落 node0 时
     * 24×24/192=3, 节点足迹超分 8×, 非保守方向)。全核时选中集 ==
     * 全机集, 折算输入恒等(零变化); 跨节点均匀受限集下两模型同值
     * (均匀假设仅对非均匀集失真); 选中集不可读(-1)退回全机口径
     * (修复前行为)。
     * worker ≠ 域内 CPU 数时仍按 workers/域 折算(全核时恰等)——
     * worker 少于 CPU 时不折算则 -n 1 把节点预算按全机线程分摊,
     * mdim 欠压 √(全机/1) 量级(127 CPU 机实测 ~11×; 旧全局模型
     * total_thread_count 补偿的同型问题); worker 多于 CPU(超额订阅)
     * 时不折算则 node_cpus 停在物理 CPU 数, 节点聚合 RSS =
     * (workers/cpus)×预算, 击穿 OOM 不变量(issue #423; x86 fallback
     * 与旧全局模型均按真实 worker 数分摊, 双向正确, 此处与它们对齐)。
     * 当前 -n 在解析层 Saturate 到亲和集 CPU 数(sandstone_opts.cpp),
     * 合法 CLI 无法超订——超订分支是防御性封堵(模型不变量在超订
     * 输入域的完整性), 非当前可达的行为。 */
    int workers = sdc_total_threads();
    {
        int cpus_all = sdc_numa_probe()->total_cpus;
        int sel_total = 0;
        int sel_node = sdc_numa_effective_threads_selected(cpu, &sel_total);
        if (sel_node > 0 && sel_total > 0) {
            if (sel_node != node_cpus || sel_total != cpus_all)
                log_warning("[联合求解] %s.%s: 折算域改选中设备集 "
                            "node_cpus %d→%d, 域 %d→%d (issue #515)",
                            test ? test->id : "(global)",
                            knob ? knob : "?",
                            node_cpus, sel_node, cpus_all, sel_total);
            node_cpus = sel_node;
            cpus_all = sel_total;
        }
        if (workers > 0 && cpus_all > 0 && workers != cpus_all)
            node_cpus = node_cpus * workers / cpus_all;   /* 向下取整 */
        if (node_cpus < 1)
            node_cpus = 1;
    }
    int node_threads = node_cpus;
    int slice_thr = thread_count();
    if (slice_thr < 1)
        slice_thr = 1;
    int node_slices = (node_threads + slice_thr - 1) / slice_thr;
    double avail = (double)node_budget
                 - a->shared_bytes_per_slice * (double)node_slices;
    int n_mem = (c_bytes > 0.0 && avail > 0.0)
            ? (int)std::sqrt(avail / ((double)node_threads * c_bytes)) : 0;

    /* issue #563/#509: AR 表值是全核争抢口径——workers==1 时单 worker 自
     * 争抢为 1(物理定义), 取 1 而非表值; 不折算则 -n 1 档(CLAUDE.md 标准
     * 回归档) n_run 被虚缩 AR^(1/fit.p)(mem 族单节点表值 25, p≈3 →
     * ~2.9×), 确定性单线程档激发面积静默回退。与 preinit 显式档同构
     * (svd_cdouble_sve f23e2fd9 / gemm 检视意见 4439402781 的
     * workers > 1 闸门), 亦是 svd_cdouble_sve.cpp 头注对联合求解记载的
     * 契约(PR#384 引入, "workers == 1 takes ar = 1")——此处实现对齐。
     * 中间档
     * (1 < workers < 全核)维持表值不插值: 无标定数据, 与 minimum_duration
     * 侧二值闸门裁定(意见 4439402800/4440012653)同向(时间预算侧保守);
     * 全核(workers == 全机 CPU)表达式不变, 行为零变化。per_solve 同用本
     * ar(模型自洽): fit_only 下 per_solve 只影响被联合层弃用的返回值,
     * 探针档位由 probe_floor_ms 决定——拟合值与探针成本不随本闸门变化。 */
    double ar = workers > 1
            ? sdc_ar_factor(a->ar_fam, sdc_numa_probe()) : 1.0;
    int n_run = 0;
    bool probe_failed = false;     /* issue #420: 任一抬升/mid 档探针自报失败 */
    if (solve_once && a->run_budget_ms > 0 && n_mem > a->lo) {
        /* k_floor=3.0: 现役/规划消费方(稠密 SVD/GEMM/稀疏
         * Cholesky)的复杂度下限同为 ~3.0(各自旧调用方同值;第四个
         * bidiag 消费方已随用例 2026-11-02 退役);
         * sdc_joint_args 不带此参(签名契约)。 */
        double per_solve_ms = (double)a->run_budget_ms / (ar * 2.0);
        long per_solve = (long)per_solve_ms;
        if (per_solve < 1)
            per_solve = 1;
        /* 探针上限 = knob hi(不用 n_mem 截断): 差分基线 (p1, mid) 拉得
         * 越开, 固定开销扣除对计时噪声越稳; 探针成本由 100ms 探针档
         * 下限(下方显式实参 sdc_m3_probe_floor_ms, spec §4.3 M3——模板
         * 缺省 1.0 是存量 7 参链的历史语义, 不得联动改动, issue #351)
         * 约束(终点档起每档 ≤ ~8×t1), 与 n_mem 无关。
         * 返回值弃用(其内向外推是未扣 b 的保守口径), 只取 fit——
         * fit_only=true 跳过验证档/收缩轮(issue #487: 验证的全部成本
         * 只为被弃用的返回值, 实测 GEMM 族每 init 白烧 ~per_solve/2
         * 单线程时间)。 */
        /* issue #420: 包装探针记录自报失败——fit_only(#517)下联合层弃用
         * 返回值, 模板内早退只落到 fit={0,0}(n_run=0), n 仍会落 n_mem;
         * 瞬态内存压力下(探针期失败、n_mem 充裕)全核单次迭代分钟级、
         * TERM 打不断——probe_failed→lo 退守是 fit-only 时代该路径的
         * 安全机制本体(与模板内哨兵互补, 缺一复活半个 P0-2 路径)。 */
        auto solve_probe = [&](int n) -> int {
            int rc = solve_once(n);
            if (rc != 0)
                probe_failed = true;
            return rc;
        };
        struct sdc_fit fit;
        sdc_calibrated_dim(a->lo, a->hi, per_solve,
                           test ? test->id : "(global)", knob, 3.0,
                           solve_probe, &fit, sdc_m3_probe_floor_ms,
                           /*fit_only=*/true);
        if (fit.a_ms > 0.0 && fit.p > 1.0 && std::isfinite(fit.a_ms)) {
            double nr = std::pow((double)a->run_budget_ms
                                 / (ar * 2.0 * fit.a_ms), 1.0 / fit.p);
            if (nr >= 2147483647.0)
                n_run = 2147483647;        /* 防溢出截断 */
            else if (nr > 0.0)
                n_run = (int)nr;
        }
    }
    int n = n_mem;
    if (probe_failed) {
        /* issue #420: 时间模型不可信 → 退守 lo; n_mem 不可交还——瞬态
         * 内存压力下按 n_mem 选出的全核单次迭代可达分钟级、TERM 打
         * 不断(P0-2 复发路径)。fit_only(#517)下验证档不存在, 失败
         * 检测覆盖抬升/mid 两档。 */
        log_warning("[联合求解] %s.%s: 校准探针自报失败 → 退守 lo=%d "
                    "(时间模型不可信, n_mem=%d 弃用; issue #420)",
                    test ? test->id : "(global)", knob ? knob : "?",
                    a->lo, n_mem);
        n = a->lo;
    } else {
        if (n_run > 0 && n_run < n)
            n = n_run;
        if (a->hi < n)
            n = a->hi;
        if (n < a->lo)
            n = a->lo;
    }
    log_warning("[联合求解] %s.%s: n_mem=%d n_run=%d AR=%.1f c=%.1f → %d (预算 %.1fGiB)",
                test ? test->id : "(global)", knob ? knob : "?",
                n_mem, n_run, ar, c_bytes, n, (double)node_budget / 1073741824.0);
    /* 口径登记(T7): run_one_test_inner 尾部的反馈写入点据此折算
     * eff_ratio = VmHWM/(n²·elem·threads)。threads 用本 slice 的
     * thread_count()(child_run 已改写为 slice 口径)。 */
    if (test && test->id) {
        struct sdc_joint_ctx ctx = { n, slice_thr, a->bytes_per_elem, c_bytes };
        sdc_joint_cache()[test->id] = ctx;
    }
    return n;
#else
    /* x86-64 fallback(#482): 四源全局预算 + 时间校准——能力面与 ARM
     * 分支同构, 减去 per-node/AR/VmHWM 反馈(x86 无 sdc_numa.h 基础设施):
     *   - 内存: sdc_budget_bytes(四源取 min) 按全机线程×c 分摊 + slices×
     *     shared。旧口径仅物理×‰——批处理/容器限额下超分("job oom"
     *     实测前科, svd_cdouble_sve.cpp 注释; #310 的四源函数本就
     *     arch-independent, 读不到的源自动跳过, 退化回物理×‰)。
     *   - 时间: solve_once 非空且 run_budget_ms>0 时经 sdc_calibrated_dim
     *     幂律拟合求 n_run(AR=1: x86 无 AR 表, 与调用方旧链同为单线程
     *     探针口径; run_budget_ms 语义与 ARM 一致 = 全核单迭代(2 解)
     *     预算)。旧 fallback `(void)solve_once` 静默丢弃时间校准——
     *     跨 arch 调用方拆 guard 迁移时会失去单解时间上限,
     *     eigen_sparse 注释自述的『自适应档满核 600s SIGQUIT』故障类
     *     随之复活(#482 的 armed trap), 本补丁解除之。探针自报失败的
     *     probe_failed→lo 退守(issue #420)与 fit_only 验证档跳过
     *     (issue #487)同 ARM 分支口径——安全机制本体, 不随 fallback
     *     缺省。
     * 行为边界(如实): 当前 x86 无调用方经此分支(调用点均 aarch64
     *     guard, 见各调用方 #else 注释), 本补丁不改变任何现行 x86
     *     行为; 改变的是未来迁移者得到的契约。rule 5 依归: Intel 原代码
     *     零触碰, 调用方 #else 旧链原样保留。 */
    int total = sdc_total_threads();
    if (total < 1)
        total = 1;
    int cur = thread_count();
    int slices = (cur > 0 && total > cur) ? (total + cur - 1) / cur : 1;
    double budget = (double)sdc_budget_bytes(permille);
    double avail = budget - a->shared_bytes_per_slice * (double)slices;
    int n_mem = (c_bytes > 0.0 && avail > 0.0)
            ? (int)std::sqrt(avail / ((double)total * c_bytes)) : 0;
    int n_run = 0;
    bool probe_failed = false;     /* issue #420: 任一抬升/mid 档探针自报失败 */
    if (solve_once && a->run_budget_ms > 0 && n_mem > a->lo) {
        /* 与 ARM 分支同构: 只取 fit(两点差分已扣固定开销 b), 返回值
         * 弃用(其内向外推是未扣 b 的保守口径); k_floor=3.0 与 ARM 分支
         * 同值(现役/规划消费方复杂度下限, sdc_joint_args 不带此参的
         * 签名契约); 探针上限 = knob hi(同 ARM: 差分基线拉得越开,
         * 固定开销扣除对计时噪声越稳)。AR=1 → per_solve=run_budget/2。
         * probe_floor_ms=sdc_m3_probe_floor_ms 与 fit_only=true 亦同
         * ARM 显式实参: 前者是 fit 差分扣 b 的配套 M3 口径(省略第 9 参
         * 的 8 参形态无匹配重载——PR #503 合入版即此形态, x86 构建全
         * 断, 本修复补齐); 后者跳过只为被弃用返回值服务的验证档/收缩
         * 轮(issue #487, ARM 同款实测浪费), 跳过不改变拟合值。 */
        long per_solve = (long)((double)a->run_budget_ms / 2.0);
        if (per_solve < 1)
            per_solve = 1;
        /* issue #420: 包装探针记录自报失败——fit_only 下联合层弃用
         * 返回值, 模板内早退只落到 fit={0,0}(n_run=0), n 仍会落
         * n_mem; probe_failed→lo 退守是该路径的安全机制本体(与模板
         * 内哨兵互补, 缺一复活半个 P0-2 路径, 同 ARM 分支)。 */
        auto solve_probe = [&](int n) -> int {
            int rc = solve_once(n);
            if (rc != 0)
                probe_failed = true;
            return rc;
        };
        struct sdc_fit fit;
        sdc_calibrated_dim(a->lo, a->hi, per_solve,
                           test ? test->id : "(global)", knob, 3.0,
                           solve_probe, &fit, sdc_m3_probe_floor_ms,
                           /*fit_only=*/true);
        if (fit.a_ms > 0.0 && fit.p > 1.0 && std::isfinite(fit.a_ms)) {
            double nr = std::pow((double)a->run_budget_ms
                                 / (2.0 * fit.a_ms), 1.0 / fit.p);
            if (nr >= 2147483647.0)
                n_run = 2147483647;        /* 防溢出截断 */
            else if (nr > 0.0)
                n_run = (int)nr;
        }
    }
    int n = n_mem;
    if (probe_failed) {
        /* issue #420: 时间模型不可信 → 退守 lo; n_mem 不可交还(同 ARM
         * 分支——瞬态内存压力下按 n_mem 选出的全核单次迭代可达分钟级、
         * TERM 打不断, P0-2 复发路径)。fit_only 下验证档不存在, 失败
         * 检测覆盖抬升/mid 两档。 */
        log_warning("[联合求解] %s.%s: 校准探针自报失败 → 退守 lo=%d "
                    "(时间模型不可信, n_mem=%d 弃用; issue #420)",
                    test ? test->id : "(global)", knob ? knob : "?",
                    a->lo, n_mem);
        n = a->lo;
    } else {
        if (n_run > 0 && n_run < n)
            n = n_run;
        if (a->hi < n)
            n = a->hi;
        if (n < a->lo)
            n = a->lo;
    }
    log_warning("[联合求解] %s.%s: n_mem=%d n_run=%d → %d (全局四源口径 %.1fGiB; "
                "x86: 无 per-node/AR/反馈)",
                test ? test->id : "(global)", knob ? knob : "?",
                n_mem, n_run, n, budget / 1073741824.0);
    return n;
#endif
}

/*
 * 自适应线性元素数: gather/fisttp/ipsec/sm3sm4 等"每线程 k 个长度为 n
 * 的线性缓冲"用例专用。与方阵不同, 总内存对 n 是线性的, 直接除法即可
 * (误用 sdc_auto_square_dim 会把压力欠缩放 sqrt(n) 倍):
 *   n = phys_mem * mem_fraction / (bytes_per_elem * arrays_per_thread
 *                                  * rss_ratio * nthreads
 *                                  + global_bytes_per_elem * slices)
 * clamp 到 [lo, hi]。hi 即安全上限, 供战役侧多实例并发时收口。
 *
 * global_bytes_per_elem (缺省 0): test_init 在共享 test->data 里按每元素
 * 分配的全局数组字节数(sleef 类"init 期共享 + run 期每线程副本"结构)。
 * 全局数组每个并行 slice 各实例化一份, 因此按 slice 数(total/nthreads,
 * 向上取整)放大后并入分母; 不传即无全局项, 语义与旧版完全一致。
 */
static inline int64_t sdc_auto_linear_count(int64_t bytes_per_elem,
                                            int arrays_per_thread,
                                            double rss_ratio,
                                            int mem_fraction_permille,
                                            int nthreads,
                                            int64_t lo, int64_t hi,
                                            const char *test_id = nullptr,
                                            const char *knob = nullptr,
                                            int64_t global_bytes_per_elem = 0)
{
    /* quick 模式(CI 冒烟): 同 sdc_auto_square_dim */
    if (sdc_quick_run()) {
        if (test_id && knob)
            log_warning("[压力映射] %s.%s = %ld (来源: quick 模式收缩, 取 lo 下限)",
                        test_id, knob, (long)lo);
        return lo;
    }
    /* 预算按全机并发线程分摊(同 sdc_auto_square_dim, 防 N slice 叠加越界) */
    int total = sdc_total_threads();
    if (total > nthreads)
        nthreads = total;
    if (nthreads < 1)
        nthreads = 1;
    if (mem_fraction_permille < 1)
        mem_fraction_permille = 500;
    if (mem_fraction_permille > 1000)
        mem_fraction_permille = 1000;

    /* 全局共享数组: 每 slice 实例化一份, 每元素 global_bytes_per_elem 字节,
     * 与每线程部分(同样按元素计)一起构成总分母 (单位均为 B/元素) */
    int slices = 1;
    if (global_bytes_per_elem > 0) {
        slices = (total + nthreads - 1) / nthreads;
        if (slices < 1)
            slices = 1;
    }
    double budget = (double)sdc_phys_mem_bytes()
                    * (double)mem_fraction_permille / 1000.0;
    double per_elem = (double)bytes_per_elem
                      * (double)arrays_per_thread * rss_ratio
                      * (double)nthreads
                      + (double)global_bytes_per_elem * slices;
    if (per_elem <= 0.0)
        return lo;
    int64_t raw = (int64_t)(budget / per_elem);
    int64_t n = raw;
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    if (test_id && knob) {
        const double gib = 1073741824.0;
        double expect = (double)n * per_elem;
        double phys = (double)sdc_phys_mem_bytes();
        if (raw > hi)
            log_warning("[欠压力] %s.%s: 自适应需 %ld 触安全上限 %ld, "
                        "预期合计仅 %.1f GiB (%d‰ 预算的 %.0f%%) — 可提高上限或减线程",
                        test_id, knob, (long)raw, (long)hi,
                        expect / gib, mem_fraction_permille,
                        100.0 * expect / budget);
        else if (raw < lo)
            log_warning("[内存越界] %s.%s: 安全下限 %ld 使预期合计 %.1f GiB "
                        "超出 %d‰ 预算 %.1f GiB — 线程过多或内存过小",
                        test_id, knob, (long)lo, expect / gib,
                        mem_fraction_permille, budget / gib);
        log_warning("[压力映射] %s.%s = %ld (来源: 内存自适应 %d‰, 全机 %d 线程%s) → "
                    "预期合计 ~%.1f GiB = 物理内存的 %.1f%%",
                    test_id, knob, (long)n, mem_fraction_permille, nthreads,
                    global_bytes_per_elem > 0 ? ", 含全局共享数组" : "",
                    expect / gib, 100.0 * expect / phys);
    }
    return n;
}

/*
 * 自适应工作集字节数: 求目标字节数使
 *   bytes <= phys_mem * mem_fraction_permille / 1000 / consumers
 * consumers 是"同时跑的大内存用例数"(如 2 路 mesh 战役), 用于在多个
 * 大内存消费者之间均摊。env_override 非空且可解析时直接采用(字节)。
 */
static inline uint64_t sdc_target_workset_bytes(int mem_fraction_permille,
                                                int consumers,
                                                const char *env_override,
                                                const char *test_id = nullptr,
                                                const char *knob = nullptr)
{
    const double gib = 1073741824.0;
    /* quick 模式(CI 冒烟): 工作集收缩到 16 MiB, 1 轮迭代快速完成 */
    if (sdc_quick_run()) {
        if (test_id && knob)
            log_warning("[压力映射] %s.%s = 16 MiB (来源: quick 模式收缩工作集)",
                        test_id, knob);
        return 16u << 20;
    }
    if (env_override) {
        char *end = nullptr;
        unsigned long long v = strtoull(env_override, &end, 10);
        if (end && *end == '\0' && v > 0) {
            if (test_id && knob)
                log_warning("[压力映射] %s.%s = %lu 字节 (来源: 环境变量覆盖) = %.1f GiB",
                            test_id, knob, (unsigned long)v, v / gib);
            return (uint64_t)v;
        }
    }
    if (consumers < 1)
        consumers = 1;
    if (mem_fraction_permille < 1)
        mem_fraction_permille = 500;
    if (mem_fraction_permille > 1000)
        mem_fraction_permille = 1000;
    /* 工作集类(mesh 等)是每 slice 一份还是全机一份由用例结构决定:
     * consumers 语义是「同时跑的大内存消费者数」, 并行 slice 各自调
     * 用本函数时也各自实例化一份 => 并发 slice 数同样要计入分摊,
     * 否则 N slice 合计 = N × 预算。 */
    {
        int total = sdc_total_threads();
        int cur = thread_count();
        int slices = (cur > 0 && total > cur) ? (total + cur - 1) / cur : 1;
        consumers *= slices;
    }
    uint64_t bytes = sdc_phys_mem_bytes() / 1000ull
             * (uint64_t)mem_fraction_permille / (uint64_t)consumers;
    if (test_id && knob)
        log_warning("[压力映射] %s.%s = %lu 字节 (来源: 工作集自适应 %d‰÷%d) = "
                    "%.1f GiB = 物理内存的 %.1f%%",
                    test_id, knob, (unsigned long)bytes, mem_fraction_permille,
                    consumers, bytes / gib,
                    100.0 * (double)bytes / (double)sdc_phys_mem_bytes());
    return bytes;
}

/*
 * 动态安全上限: 线性缓冲族的 hi 不再是固定值, 而是按同一预算模型算出的
 * "目标值本身允许到多大"。公式与 sdc_auto_linear_count 相同但取 hi 侧:
 *   hi = max(floor_hi, phys_mem * mem_fraction / (per_elem_bytes * nthreads))
 * 即: 小内存机器保持原 floor_hi 收口(防 OOM), 大内存机器按比例放开,
 * 使"自适应默认值"与"显式配置的上限"不再互相矛盾——配置越大越接近
 * 预算, 不存在 60% 预算需要 2.4G 元素但上限只有 16M 的欠压力收口。
 */
static inline int64_t sdc_linear_hi(int64_t per_elem_bytes,
                                    int arrays_per_thread,
                                    double rss_ratio,
                                    int mem_fraction_permille,
                                    int nthreads,
                                    int64_t floor_hi)
{
    /* 预算按全机并发线程分摊(同 sdc_auto_linear_count, 防 N slice 叠加越界) */
    int total = sdc_total_threads();
    if (total > nthreads)
        nthreads = total;
    if (nthreads < 1)
        nthreads = 1;
    if (mem_fraction_permille < 1)
        mem_fraction_permille = 500;
    if (mem_fraction_permille > 1000)
        mem_fraction_permille = 1000;
    double per_elem = (double)per_elem_bytes
                      * (double)arrays_per_thread * rss_ratio
                      * (double)nthreads;
    if (per_elem <= 0.0)
        return floor_hi;
    int64_t scaled = (int64_t)((double)sdc_phys_mem_bytes()
                    * (double)mem_fraction_permille / 1000.0 / per_elem);
    return scaled > floor_hi ? scaled : floor_hi;
}

} // extern "C++"
#endif /* __cplusplus */

#endif /* SDC_STRESS_CONFIG_H */
