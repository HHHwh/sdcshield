/* SPDX-License-Identifier: Apache-2.0 */
#include "sdc_numa.h"
#if defined(__aarch64__) && defined(__linux__)

#include "sdc_stress_config.h"   /* sdc_budget_bytes(三源 min, spec §4.2) */

#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

/* sysfs 目录项名过滤: nodeN 且 N 全数字 */
static int parse_node_dir(const char *name, int *out)
{
    if (strncmp(name, "node", 4) != 0) return 0;
    char *end;
    long n = strtol(name + 4, &end, 10);
    if (*end != '\0' || n < 0) return 0;
    *out = (int)n;
    return 1;
}

/* cpu→node 归属查表(Task 2 起 node_of_cpu 走此表, 替代 Task 1 的逐文件
 * 扫描): 一次性由 probe_uncached 填充, 之后只读。容量 4096 = 内核
 * NR_CPUS 实际上限; 更大 CPU 编号查表得 -1(与"不在任何节点"同路径)。 */
#define SDC_NUMA_MAX_CPUS 4096
static int s_cpu_node[SDC_NUMA_MAX_CPUS];    /* -1 = 未归属; 0 是合法节点号 */

/* 遍历 CPU 区间串 "0-31,64,96-121,123-127" 的每个 CPU, 逐个回调 visit。
 * 内核 cpulist 语义; 非数字处停止; 倒挂段 "5-3" 按单 CPU 计。 */
template <typename Visit>
static void for_each_cpu_in_list(const char *list, Visit visit)
{
    const char *p = list;
    while (*p) {
        char *end;
        long a = strtol(p, &end, 10);
        if (end == p) break;
        long b = a;
        if (*end == '-') b = strtol(end + 1, &end, 10);
        if (b < a) b = a;
        for (long c = a; c <= b; ++c)
            visit((int)c);
        if (*end != ',') break;
        p = end + 1;
    }
}

/* 统计 CPU 区间串覆盖的 CPU 数 */
static int count_cpu_list(const char *list)
{
    int n = 0;
    for_each_cpu_in_list(list, [&](int) { ++n; });
    return n;
}

/* 读 nodeN/meminfo 的 MemTotal/MemFree(→字节)。行形如 "Node 0 MemTotal:
 * 64138080 kB"(字段名前有节点号前缀), 用 strstr 定位; 缺失/不可读置 0
 * —— 与"无内存节点"同路径(按 C1 并入 fallback, 不虚报)。 */
static void node_mem_read(int node, uint64_t *total_bytes, uint64_t *free_bytes)
{
    char path[128], line[256];
    *total_bytes = *free_bytes = 0;
    snprintf(path, sizeof path, "/sys/devices/system/node/node%d/meminfo", node);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        unsigned long long kb;
        const char *p = strstr(line, "MemTotal:");
        if (p) {
            if (sscanf(p + 9, "%llu", &kb) == 1)
                *total_bytes = kb * 1024;
            continue;
        }
        p = strstr(line, "MemFree:");
        if (p && sscanf(p + 8, "%llu", &kb) == 1)
            *free_bytes = kb * 1024;
    }
    fclose(f);
}

/* 一次性探测; 由 sdc_numa_probe 的静态初始化保证只跑一遍 */
static struct sdc_numa_topology probe_uncached(void)
{
    struct sdc_numa_topology topo = {};
    /* 0 是合法节点号, 归属表必须显式置 -1(BSS 零值会被误读为 node0) */
    for (int i = 0; i < SDC_NUMA_MAX_CPUS; ++i)
        s_cpu_node[i] = -1;
    DIR *d = opendir("/sys/devices/system/node");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != nullptr) {
            int node;
            if (!parse_node_dir(de->d_name, &node)) continue;
            if (node >= SDC_NUMA_MAX_NODES) continue;   /* 超容量节点忽略 */
            struct sdc_numa_node *pn = &topo.per_node[node];
            node_mem_read(node, &pn->memtotal_bytes, &pn->memfree_bytes);
            if (pn->memtotal_bytes > 0)
                ++topo.nodes_with_mem;
            /* cpulist: 计数 + 登记 cpu→node 归属表(一次解析, 之后查表) */
            char path[128], buf[1024];
            snprintf(path, sizeof path, "/sys/devices/system/node/node%d/cpulist", node);
            FILE *f = fopen(path, "r");
            if (f) {
                if (fgets(buf, sizeof buf, f)) {
                    int n = 0;
                    for_each_cpu_in_list(buf, [&](int c) {
                        ++n;
                        if (c >= 0 && c < SDC_NUMA_MAX_CPUS)
                            s_cpu_node[c] = node;
                    });
                    pn->ncpus = n;
                }
                fclose(f);
            }
        }
        closedir(d);
    }
    FILE *onl = fopen("/sys/devices/system/cpu/online", "r");
    if (onl) {
        char buf[256] = "";
        if (fgets(buf, sizeof buf, onl))
            topo.total_cpus = count_cpu_list(buf);
        fclose(onl);
    }
    return topo;
}

const struct sdc_numa_topology *sdc_numa_probe(void)
{
    /* 函数内静态初始化由 C++11 保证线程安全: 并发首调只探测一次 */
    static const struct sdc_numa_topology topo = probe_uncached();
    return &topo;
}

double sdc_ar_factor(enum sdc_mem_family fam, const struct sdc_numa_topology *t)
{
    if (!t) t = sdc_numa_probe();
    /* 分档表(spec §4.1): 行=有内存节点数, 列=负载族。
     * 探测失败(nodes_with_mem==0)一律最差档; 中间档向下取整(取
     * nodes<=n 的最大行)——表值宁高估, 永不向上猜。 */
    static const struct { int nodes; double mem, comp; } tbl[] = {
        { 1, 25.0, 5.0 },   /* 单节点内存: SVD 实测带 18-25 取上界; GEMM 4.4+余量 */
        { 2, 10.0, 4.0 },   /* 保守估计, TODO(实测): 无真 2 节点内存机器 */
        { 4,  5.0, 3.0 },   /* 同上 */
        { 8,  4.0, 2.5 },   /* >=8 节点 */
    };
    int n = t->nodes_with_mem;
    if (n <= 0) return fam == SDC_FAM_MEM_BOUND ? 25.0 : 5.0;
    /* 表按 nodes 升序; 取最大的 r.nodes <= n 的行。
     * n>=1 必命中首行; n 超过所有行仍取 >=8 档(最后一行)。 */
    const auto *row = &tbl[0];
    for (const auto &r : tbl)
        if (r.nodes <= n) row = &r;
    return fam == SDC_FAM_MEM_BOUND ? row->mem : row->comp;
}

int sdc_numa_node_of_cpu(int cpu)
{
    if (cpu < 0 || cpu >= SDC_NUMA_MAX_CPUS) return -1;
    sdc_numa_probe();          /* 保证归属表已构建(magic static 一次性) */
    return s_cpu_node[cpu];
}

/* C1 fallback 节点: 有内存节点中 MemFree 最大者; 平手取 MemTotal 大者,
 * 再平手取最小节点号。无有内存节点(探测失败)返回 -1。 */
static int sdc_fallback_node(const struct sdc_numa_topology *t)
{
    int fb = -1;
    uint64_t best_free = 0, best_total = 0;
    for (int i = 0; i < SDC_NUMA_MAX_NODES; ++i) {
        const struct sdc_numa_node *n = &t->per_node[i];
        if (n->memtotal_bytes == 0) continue;
        if (n->memfree_bytes > best_free
            || (n->memfree_bytes == best_free && n->memtotal_bytes > best_total)) {
            best_free = n->memfree_bytes;
            best_total = n->memtotal_bytes;
            fb = i;
        }
    }
    return fb;
}

/* total_cpus 无失败哨兵(不可读为 0), 防 0 除的在线 CPU 兜底 */
static int sdc_online_cpus_fallback(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

/* 该 cpu 的需求最终落在哪个节点: 本节点有内存用本节点, 否则并入
 * fallback(C1)。返回 -1 表示无任何节点内存信息(探测失败)。 */
static int sdc_effective_node(const struct sdc_numa_topology *t, int cpu)
{
    int node = sdc_numa_node_of_cpu(cpu);
    if (node < 0 || t->per_node[node].memtotal_bytes == 0)
        node = sdc_fallback_node(t);
    return node;
}

uint64_t sdc_node_budget_bytes(int permille, int cpu)
{
    /* 数据新鲜度口径(PR#302 检视 #21, 与 spec §8 对齐):
     *   - 全局预算 sdc_budget_bytes() 每次【现读】/proc/meminfo 与 cgroup
     *     文件——MemAvailable 波动(他进程)即时反映(spec §8 承诺的
     *     "每次 fracture 重读" 指的就是这一层);
     *   - 节点分摊比例 MemFree/ΣMemFree 取自 sdc_numa_probe() 的探测期
     *     快照(进程生命期 magic static)——聚合预算逐次新鲜, 仅真多节点
     *     机上的【节点间分摊】是慢漂移近似; 单内存节点比例恒 1, 精确
     *     还原全局模型(本机即此形态)。
     *   不逐次重探节点 MemFree 的理由: fallback 节点选择(C1)与 cpu→node
     *   归属表都建立在同一快照上, 中途分摊漂移会使无内存节点的线程
     *   归属与 fallback 选择失配(比陈旧分摊更糟的不一致); 多节点分摊
     *   漂移为已披露边界(spec §8, 本机 node0-only 不可验)。 */
    const struct sdc_numa_topology *t = sdc_numa_probe();
    /* 探测失败(无节点内存信息): 诚实退化全局模型 —— 全局预算本身已含
     * 三源 min 上界, 退化不会放大可用量。 */
    if (t->nodes_with_mem <= 0)
        return sdc_budget_bytes(permille);
    int node = sdc_effective_node(t, cpu);
    if (node < 0)
        return sdc_budget_bytes(permille);   /* 防御: 上面已保证不可达 */
    uint64_t sum_free = 0;
    for (int i = 0; i < SDC_NUMA_MAX_NODES; ++i)
        if (t->per_node[i].memtotal_bytes > 0)
            sum_free += t->per_node[i].memfree_bytes;
    uint64_t budget = sdc_budget_bytes(permille);
    if (sum_free == 0)
        /* 极端: 有内存节点全部 MemFree=0 —— 按有内存节点数均摊, 不除零 */
        return budget / (uint64_t)t->nodes_with_mem;
    /* MemFree=0 的节点分摊为 0(spec §4.2)。__int128 防乘法溢出: 预算与
     * MemFree 都是 1e10..1e13 量级, 直乘超 uint64。单内存节点时
     * MemFree/ΣMemFree=1, 精确还原全局预算(node0-only 退化)。 */
    return (uint64_t)((unsigned __int128)budget * t->per_node[node].memfree_bytes
                      / sum_free);
}

int sdc_numa_effective_threads(int cpu)
{
    const struct sdc_numa_topology *t = sdc_numa_probe();
    if (t->nodes_with_mem <= 0)
        return t->total_cpus > 0 ? t->total_cpus : sdc_online_cpus_fallback();
    int fb = sdc_fallback_node(t);
    int node = sdc_effective_node(t, cpu);
    if (node < 0)
        return sdc_online_cpus_fallback();
    int n = t->per_node[node].ncpus;
    if (node == fb) {
        /* C1: 无内存节点的线程需求全部记到 fallback 节点头上; 有内存
         * 但非 fallback 的节点只计自身线程(并入是单向的)。 */
        for (int i = 0; i < SDC_NUMA_MAX_NODES; ++i)
            if (t->per_node[i].memtotal_bytes == 0)
                n += t->per_node[i].ncpus;
    }
    return n > 0 ? n : (t->total_cpus > 0 ? t->total_cpus
                                          : sdc_online_cpus_fallback());
}

/* T7 (spec §4.4): VmHWM(kB→字节), 读不到 0。行形如 "VmHWM:  123456 kB"。 */
uint64_t sdc_read_vm_hwm(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return 0;
    char line[256];
    unsigned long long kb = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "VmHWM: %llu kB", &kb) == 1)
            break;
    fclose(f);
    return kb * 1024ull;
}

#endif /* __aarch64__ && __linux__ */
