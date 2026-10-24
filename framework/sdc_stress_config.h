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

/* ------------------------------------------------------------------ */
/* 内存自适应默认值                                                    */
/* ------------------------------------------------------------------ */

/*
 * 自适应方阵维度: 求最大的 N 使
 *   bytes_per_elem * matrices_per_thread * rss_ratio * N^2 * nthreads
 *     <= phys_mem * mem_fraction
 * 再 clamp 到 [lo, hi]。
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
 *   k clamp [1.5, 4.0] 抑制小档计时噪声; 试解总成本仅两小档(秒级)。
 *   显式 knob/env 指定维度的路径不得调用本函数(用户自担)。
 * 调用契约:
 *   int dim = sdc_auto_square_dim(...);        // 内存上限
 *   if (dim > lo)
 *       dim = sdc_calibrated_dim(lo, dim, max_single_ms,
 *                                test_id, knob, k_floor, solve_once);
 */
template <typename F>
static inline int sdc_calibrated_dim(int lo, int hi, long max_single_ms,
                                     const char *test_id, const char *knob,
                                     double k_floor, F solve_once)
{
    if (hi <= lo)
        return lo;
    auto probe = [&](int n) -> double {
        auto t0 = std::chrono::steady_clock::now();
        solve_once(n);
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - t0).count();
    };
    /* probe 档自适应抬升: 小档求解太快(<1ms)时计时噪声淹没幂律拟合,
     * 翻倍 probe 档直到可测量(>=1ms)或触及 hi——否则外推基数失真
     * (实测稀疏 Cholesky 规则带状 probe 在 64 档仅 0.1ms, 外推失控)。 */
    int p1 = lo;
    double t1 = probe(p1);
    while (t1 < 1.0 && p1 * 2 <= hi) {
        p1 *= 2;
        t1 = probe(p1);
    }
    int mid = (p1 * 2 <= hi) ? p1 * 2 : hi;
    double t2 = probe(mid);
    if (t1 <= 0.0 || t2 <= 0.0) {
        /* 计时分辨率不足(极快求解)——时间不是约束, 交还内存上限 */
        log_warning("[时间校准] %s.%s: 试解计时不足(%.2f/%.2fms) → 维持内存上限 %ld",
                    test_id, knob, t1, t2, (long)hi);
        return hi;
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
    log_warning("[时间校准] %s.%s: 单次求解预算 %ldms (试解 %d→%.1fms, %d→%.1fms, k=%.2f) → %d",
                test_id, knob, max_single_ms, p1, t1, mid, t2, k, dim);
    return dim;
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

/* 环境变量兜底: SDC_<TESTID>_<KNOB> —— 配置文件未提供时的轻量通道。
 * 返回值: 找到并解析成功返回 1 且 *out 填充; 否则返回 0。 */
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

} // extern "C++"
#endif /* __cplusplus */

#endif /* SDC_STRESS_CONFIG_H */
