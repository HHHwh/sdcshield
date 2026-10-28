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
/* 可用内存预算: 三源取 min(spec §4.2)                                  */
/* ------------------------------------------------------------------ */

/* [可用内存预算] 三源取 min(spec §4.2): 物理×‰ / MemAvailable×0.9 /
 * cgroup 限额−已用。任一来源读失败(返回 0)即跳过该来源。
 * 动机: 旧口径只读 _SC_PHYS_PAGES, 批处理/容器限额下超分——
 * "job oom" 实测前科(svd_cdouble_sve.cpp:114-120 注释)。 */
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
        size_t clen = (size_t)(c2 - c1 - 1);
        char ctrls[128];
        if (clen >= sizeof ctrls)
            continue;
        memcpy(ctrls, c1 + 1, clen);
        ctrls[clen] = '\0';
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
 * 文件缺失/不可读/limit≤usage → 0(该级不约束)。 */
static inline uint64_t sdc_cgroup_level_avail(const char *dir, bool v2)
{
    /* 320: dir 最长 255(walk 的 256 缓冲) + 最长文件名 23, 防截断告警 */
    char path[320];
    uint64_t lim, cur;
    FILE *f;
    if (v2) {
        snprintf(path, sizeof path, "%s/memory.max", dir);
        f = fopen(path, "r");
        if (!f) return 0;
        char buf[64];
        bool ok = fgets(buf, sizeof buf, f) != nullptr;
        fclose(f);
        if (!ok || strncmp(buf, "max", 3) == 0) return 0;
        lim = strtoull(buf, nullptr, 10);
        snprintf(path, sizeof path, "%s/memory.current", dir);
        f = fopen(path, "r");
    } else {
        snprintf(path, sizeof path, "%s/memory.limit_in_bytes", dir);
        f = fopen(path, "r");
        if (!f) return 0;
        bool ok = fscanf(f, "%lu", &lim) == 1;
        fclose(f);
        if (!ok || lim >= sdc_phys_mem_bytes()) return 0;   /* 无限 */
        snprintf(path, sizeof path, "%s/memory.usage_in_bytes", dir);
        f = fopen(path, "r");
    }
    if (!f) return 0;
    bool ok2 = fscanf(f, "%lu", &cur) == 1 && lim > cur;
    fclose(f);
    return ok2 ? lim - cur : 0;
}

/* 自叶子(dir=base+rel)向根(base)逐级取 avail 的最小值——祖先与子孙
 * 的限制都约束本进程, 各级 usage 含下级, limit−usage 是该级对本进程
 * 的可用上界, 取 min 即保守合成。 */
static inline uint64_t sdc_cgroup_walk(const char *base, const char *rel, bool v2)
{
    char dir[256];
    if (snprintf(dir, sizeof dir, "%s%s", base, rel) >= (int)sizeof dir)
        return 0;
    size_t blen = strlen(base);
    uint64_t best = 0;
    for (;;) {
        uint64_t av = sdc_cgroup_level_avail(dir, v2);
        if (av > 0 && (best == 0 || av < best))
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
    /* v2: memory.max - memory.current ("max" = 无限 → 0)
     * v1: memory/memory.limit_in_bytes - memory.usage_in_bytes
     *      (limit >= 物理内存视为无限 → 0)。本机(openEuler)实测为 v1、
     *      且位于嵌套 /system.slice/sshd.service(根无限制≠本进程无限)。 */
    char v1base[64], v1rel[192], v2base[48], v2rel[192];
    sdc_cgroup_self(v1base, sizeof v1base, v1rel, sizeof v1rel,
                    v2base, sizeof v2base, v2rel, sizeof v2rel);
    uint64_t a1 = sdc_cgroup_walk(v1base, v1rel, false);
    uint64_t a2 = sdc_cgroup_walk(v2base, v2rel, true);
    if (a1 == 0 || a2 == 0)
        return a1 > 0 ? a1 : a2;      /* memory 控制器只在一侧挂载 */
    return a1 < a2 ? a1 : a2;
}

static inline uint64_t sdc_budget_bytes(int permille)
{
    uint64_t b = sdc_phys_mem_bytes() * (uint64_t)permille / 1000;
    uint64_t ma = sdc_memavailable_bytes();
    if (ma > 0 && ma * 9 / 10 < b) b = ma * 9 / 10;
    uint64_t cg = sdc_cgroup_limit_bytes();
    if (cg > 0 && cg < b) b = cg;
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
 * abort(debug 构建)。故 init 查询时把最终生效值缓存, run 里只读缓存。
 * fork 模式下子进程写时复制继承主进程已填充的缓存。
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
     * 未命中说明该测试在 init 里没有查询过此 knob(测试 bug)——回退 def
     * 走自适应, 与 init 侧的 def 路径语义一致, 避免 debug 构建 abort。 */
    if (thread_num != -1) {
        auto it = sdc_knob_cache().find(cache_key);
        return it != sdc_knob_cache().end() ? it->second : def;
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
 * 返回值: 找到并解析成功返回 1 且 *out 填充; 否则返回 0。
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
 * 路径保持 {0,0}(无效, 消费方跳过 n_run)。默认 nullptr, 既有调用
 * (7 实参)签名不变。
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

template <typename F>
static inline int sdc_calibrated_dim(int lo, int hi, long max_single_ms,
                                     const char *test_id, const char *knob,
                                     double k_floor, F solve_once,
                                     struct sdc_fit *fit = nullptr)
{
    if (fit) { fit->a_ms = 0.0; fit->p = 0.0; }  /* 早退路径保持无效 */
    if (hi <= lo)
        return lo;
    auto probe = [&](int n) -> double {
        auto t0 = std::chrono::steady_clock::now();
        solve_once(n);
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - t0).count();
    };
    /* probe 档自适应抬升: 小档求解太快(<100ms)时固定开销(构造/预处理)
     * 在计时中占比过高——幂律系数被抬高, 外推维度被压低(实测 sve 模型
     * t(N)=243.4·(N/4400)³+1 的 +1 项在 N=300 档占 93%, 系数虚高 ~14×,
     * 立方根收缩 2.4×)。翻倍 probe 档直到固定开销占比 <10%(>=100ms,
     * spec §4.3 M3; 1ms→100ms)或触及 hi——否则外推基数失真。 */
    int p1 = lo;
    double t1 = probe(p1);
    while (t1 < 100.0 && p1 * 2 <= hi) {
        p1 *= 2;
        t1 = probe(p1);
    }
    int mid = (p1 * 2 <= hi) ? p1 * 2 : hi;
    double t2 = probe(mid);
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
     * newdim 强制单调递减吸收计时噪声, 轮数上限 4 防抖动。 */
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
                    test_id, knob, xdim, td0, shrinks);
    log_warning("[时间校准] %s.%s: 单次求解预算 %ldms (试解 %d→%.1fms, %d→%.1fms, "
                "k=%.2f) → %d (验证 %.1fms%s)",
                test_id, knob, max_single_ms, p1, t1, mid, t2, k, dim, td,
                td > (double)max_single_ms
                    ? (dim <= lo ? ", lo 档仍超预算" : ", 收缩轮数上限后仍超预算")
                    : "");
    if (fit) {
        /* 两点差分扣除固定开销 b(M3): t = a·n^p + b ⇒
         *   a = (t2−t1)/(mid^p − p1^p),   b = t1 − a·p1^p
         * b≈0 时严格退化为 a = t1/p1^p。差分在 b 为常数的模型下精确,
         * 常规档(b 占比 <10%, 与 100ms 阈值配套)与 b 主导的对抗档都
         * 成立; 代价是对差分 (t2−t1) 的测量噪声敏感(p1/mid 档位拉得
         * 越开越稳)。退化保护(档位重合/计时持平/非有限/非正)回退
         * b=0 口径 a = t1/p1^p —— 系数只高不低, 消费方 n_run 只小
         * 不大(保守方向)。 */
        double pn = std::pow((double)p1, k);
        double mn = std::pow((double)mid, k);
        double a = 0.0;
        if (mid > p1 && t2 > t1 && mn > pn && std::isfinite(mn - pn))
            a = (t2 - t1) / (mn - pn);
        if (!(a > 0.0) && pn > 0.0)
            a = t1 / pn;                 /* t1>0 由上方早退保证 */
        if (a > 0.0 && std::isfinite(a)) {
            fit->a_ms = a;
            fit->p = k;
        }
    }
    return dim;
}

/* ------------------------------------------------------------------ */
/* 联合求解: 时间×内存双上限维度(spec §4.3)                              */
/* ------------------------------------------------------------------ */

/* RSS_RATIO_TABLE 键: 算法族。 */
enum sdc_rss_family {
    SDC_RSS_BDCSVD_F32, SDC_RSS_BDCSVD_F64, SDC_RSS_BDCSVD_C128,
    SDC_RSS_JACOBI_F32, SDC_RSS_JACOBI_F64, SDC_RSS_JACOBI_C128,
    SDC_RSS_GEMM_F32, SDC_RSS_GEMM_F64, SDC_RSS_GEMM_C128,
    SDC_RSS_SPARSE, SDC_RSS_BIDIAG, SDC_RSS_INVALID
};

/* 族 → 实测 c(KB/n²/线程)。Phase A 2026-10-27, (child@T4−child@T1)/3
 * 差分增量口径——与 svd_cdouble_sve.cpp:92-95 的"单进程 9.7-10× 输入
 * 体积"(含基线, ~155B/n²)口径不同, 差异 ~35% 属口径差非矛盾: 联合
 * 求解的增量模型取差分口径。c 是算法内在属性(矩阵驻留数×元素大小),
 * 跨机可移植; Eigen 版本升级需重标定本表。 */
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
    0.0313,   /* SDC_RSS_BIDIAG      */
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

/* T10 降档路径的口径同步(PR#302 检视 #25): init 期 bad_alloc/分配失败
 * 降档到 kInitRetryLo 重试成功后, 联合求解登记的 sdc_joint_ctx.n 仍是
 * 求解值——run_one_test_inner 尾部的反馈写入点按 ctx.n 折算 eff_ratio
 * = VmHWM/(n²·elem·threads), 实际工作集只有 retry 值会使 eff 被低估
 * (n/retry)² 倍, 反馈通道被污染、c 被压向 0.7× 静态锚下限, 后续
 * fracture 的 n 向 OOM 方向膨胀。降档处调用本函数把登记口径同步为
 * 实际值; 显式 knob/未迁移用例无登记条目时为 no-op。init 双重失败
 * (EXIT_SKIP)不需要: run_one_test_inner 对 init SKIP 提前返回, 到不了
 * 反馈写入点。已知残差(如实记录): worker 期 bad_alloc 的线程级 SKIP
 * 仍按求解 n 记账(VmHWM 是不完整分配的峰值)——低估方向受读侧 0.7×
 * 锚下限钳制(n 至多膨胀 1/√0.7≈1.2×), 且生产构建 malloc 插桩拦截空
 * 分配(仅 ASAN 可达), 不单独设通道。 */
static inline void sdc_joint_cache_note_dim(struct test *test, int actual_n)
{
    if (!test || !test->id)
        return;
    auto it = sdc_joint_cache().find(test->id);
    if (it == sdc_joint_cache().end())
        return;
    if (actual_n > 0)
        it->second.n = actual_n;
    else
        sdc_joint_cache().erase(it);   /* 0 = 口径无效 → 本 child 不写反馈 */
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
    long run_budget_ms;             /* 全核单迭代(2解×AR)预算 ms, 调用方传
                                       200000(spec: 300s 宽限×0.67≈200s);
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
 */
static inline int64_t sdc_joint_knob_value(struct test *test, const char *key,
                                           int64_t lo, int64_t hi)
{
    auto it = sdc_knob_cache().find(sdc_knob_cache_key(test, key));
    if (it != sdc_knob_cache().end())
        return it->second;
    return sdc_knob_int_clamped(test, key, 0, lo, hi);
}

/*
 * 联合求解 n_opt = min(n_mem, n_run, hi), lo 兜底:
 *   n_mem = sqrt((node_budget − node_slices·shared) / (node_threads · c))
 *   n_run = (run_budget_ms / (AR(ar_fam) × 2 × fit.a_ms))^(1/fit.p)
 * 时间侧: run_budget_ms 即全核单迭代预算(×2 = init 填充+一次求解,
 * ×AR = 全核争抢慢化(族×拓扑, spec M2); 调用方传 200000 = 300s 硬宽限
 * ×0.67(spec M1), 本函数不再二次折算——200s 已是折算后的值); fit 来自
 * sdc_calibrated_dim 的两点差分(已扣固定开销, M3), 无效时跳过 n_run。
 * 内存侧 c 优先级链(T7 起): VmHWM 反馈 median(跨 fracture 的 Main 槽
 * EMA, 详见 sdc_feedback_eff_median; 向上无钳、向下 15% 死区 + 0.7×静态
 * 锚下限) → RSS_RATIO_TABLE(族) → 调用方传统三元组
 * (bytes_per_elem×matrices_per_thread×rss_ratio)。
 * knob 三层 fallback(T11 起): arm64 分支持有 knob 早退——显式 -O/env 值
 * 在此早退并打预期内存档位警告(见函数内 T11 块); 调用方迁移后
 * (calibrated_auto_dim 模式)显式值也路由进本函数。未迁移调用方仍在
 * 自身 init 里短路(无警告, 见 T11 块尾注)。quick 模式直接返回 lo
 * (m3, 与 sdc_auto_square_dim 同捷径; 但位于 knob 早退之后——显式值
 * 优先, 与迁移前调用方短路语义一致)。
 * solve_once 可为 NULL(不校时间, 只算 n_mem); run_budget_ms=0 同理。
 * x86-64(rule 5: byte-identical): 仅 n_mem 全局口径(等价旧行为),
 * 见函数内 #else 分支。
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
     * 与全局三源预算 sdc_budget_bytes(permille) 比对: 超出 → [内存越界],
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
     *   - 向上修正(c_fb ≥ 模型): 无死区无上钳 —— 表系统性偏小时靠它收敛
     *     (T6 实测 GEMM_F64 表偏小 ~8%, progress 裁定"反馈无上钳可收敛"),
     *     压 n 是安全方向;
     *   - 向下修正: 需越出 15% 死区才采用(反馈噪声/m1 选择偏差——超时
     *     child 到不了写入点, 样本偏乐观——都朝压小 c 的方向, 小幅下压
     *     一律不信), 且下限 = 静态锚 × 0.7(锚 = 表值, 无表用三元组),
     *     防反馈把 c 压小导致 n 膨胀(OOM 方向)。 */
    {
        double eff_med = sdc_feedback_eff_median(test ? test->id : nullptr);
        if (eff_med > 0.0 && a->bytes_per_elem > 0) {
            double c_fb = eff_med * (double)a->bytes_per_elem;
            double anchor = c_table > 0.0 ? c_table : c_bytes;  /* 静态锚 */
            double cand = std::max(c_fb, 0.7 * anchor);
            bool upward = cand >= c_bytes;
            bool outside_deadzone = cand < 0.85 * c_bytes;
            if (upward || outside_deadzone) {
                log_warning("[联合求解] %s.%s: VmHWM 反馈生效 c=%.1f→%.1f B/n²/线程 "
                            "(eff=%.3f, %s, 下限 %.1f)",
                            test ? test->id : "(global)", knob ? knob : "?",
                            c_bytes, cand, eff_med,
                            upward ? "向上修正, 无上钳" : "越出15%死区",
                            0.7 * anchor);
                c_bytes = cand;
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
     * -n/--cpuset 下 worker 少于 CPU, 按 全机worker/全机CPU 折算(全核
     * 时恰等)——不折算则 -n 1 把节点预算按 127 线程分摊, mdim 欠压
     * ~11×(旧全局模型 total_thread_count 补偿的同型问题)。 */
    {
        int workers = sdc_total_threads();
        int cpus_all = sdc_numa_probe()->total_cpus;
        if (workers > 0 && cpus_all > 0 && workers < cpus_all)
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

    double ar = sdc_ar_factor(a->ar_fam, sdc_numa_probe());
    int n_run = 0;
    if (solve_once && a->run_budget_ms > 0 && n_mem > a->lo) {
        /* k_floor=3.0: 现役/规划消费方(稠密 SVD/GEMM/稀疏 Cholesky/
         * bidiag)的复杂度下限同为 ~3.0(各自旧调用方同值);
         * sdc_joint_args 不带此参(签名契约)。 */
        double per_solve_ms = (double)a->run_budget_ms / (ar * 2.0);
        long per_solve = (long)per_solve_ms;
        if (per_solve < 1)
            per_solve = 1;
        /* 探针上限 = knob hi(不用 n_mem 截断): 差分基线 (p1, mid) 拉得
         * 越开, 固定开销扣除对计时噪声越稳; 探针成本由 100ms 阈值
         * 机制约束(终点档起每档 ≤ ~8×t1, 验证档 ≤ 预算), 与 n_mem
         * 无关。返回值弃用(其内向外推是未扣 b 的保守口径), 只取 fit。 */
        struct sdc_fit fit;
        sdc_calibrated_dim(a->lo, a->hi, per_solve,
                           test ? test->id : "(global)", knob, 3.0,
                           solve_once, &fit);
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
    if (n_run > 0 && n_run < n)
        n = n_run;
    if (a->hi < n)
        n = a->hi;
    if (n < a->lo)
        n = a->lo;
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
    /* x86-64 fallback(rule 5: 行为不变): 仅 n_mem 全局口径——物理×‰ 按
     * 全机线程×c 分摊(等价 sdc_auto_square_dim 的内存模型, 含 slice
     * 数×shared), 无 per-node/AR/时间外推; solve_once/run_budget_ms
     * 在 x86 被忽略, 时间校准维持调用方既有的 sdc_calibrated_dim 链。 */
    (void)solve_once;
    int total = sdc_total_threads();
    if (total < 1)
        total = 1;
    int cur = thread_count();
    int slices = (cur > 0 && total > cur) ? (total + cur - 1) / cur : 1;
    double budget = (double)sdc_phys_mem_bytes() * (double)permille / 1000.0;
    double avail = budget - a->shared_bytes_per_slice * (double)slices;
    int n_mem = (c_bytes > 0.0 && avail > 0.0)
            ? (int)std::sqrt(avail / ((double)total * c_bytes)) : 0;
    int n = n_mem;
    if (a->hi < n)
        n = a->hi;
    if (n < a->lo)
        n = a->lo;
    log_warning("[联合求解] %s.%s: n_mem=%d → %d (全局口径 %.1fGiB; rule 5: "
                "x86 原路径, 无 per-node/AR/时间外推)",
                test ? test->id : "(global)", knob ? knob : "?", n_mem, n,
                budget / 1073741824.0);
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
