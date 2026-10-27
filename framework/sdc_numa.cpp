/* SPDX-License-Identifier: Apache-2.0 */
#include "sdc_numa.h"
#if defined(__aarch64__) && defined(__linux__)

#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

/* 解析 "0-31,64,96-121" 形式的 CPU 区间串; 命中 cpu 返回 true */
static bool cpu_list_contains(const char *list, int cpu)
{
    const char *p = list;
    while (*p) {
        char *end;
        long a = strtol(p, &end, 10);
        if (end == p) break;
        long b = a;
        if (*end == '-') b = strtol(end + 1, &end, 10);
        if (cpu >= a && cpu <= b) return true;
        if (*end != ',') break;
        p = end + 1;
    }
    return false;
}

/* 统计 CPU 区间串覆盖的 CPU 数 */
static int count_cpu_list(const char *list)
{
    int n = 0;
    const char *p = list;
    while (*p) {
        char *end;
        long a = strtol(p, &end, 10);
        if (end == p) break;
        long b = a;
        if (*end == '-') b = strtol(end + 1, &end, 10);
        n += (int)(b - a + 1);
        if (*end != ',') break;
        p = end + 1;
    }
    return n;
}

/* 读 nodeN/meminfo 的 MemTotal(kB); 解析失败 -1 */
static long node_memtotal_kb(int node)
{
    char path[128], line[256];
    snprintf(path, sizeof path, "/sys/devices/system/node/node%d/meminfo", node);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long kb = -1;
    while (fgets(line, sizeof line, f)) {
        const char *p = strstr(line, "MemTotal:");
        if (!p) continue;
        if (sscanf(p + strlen("MemTotal:"), "%ld", &kb) != 1) kb = -1;
        break;
    }
    fclose(f);
    return kb;
}

/* 一次性探测; 由 sdc_numa_probe 的静态初始化保证只跑一遍 */
static struct sdc_numa_topology probe_uncached(void)
{
    struct sdc_numa_topology topo = {};
    DIR *d = opendir("/sys/devices/system/node");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != nullptr) {
            int node;
            if (!parse_node_dir(de->d_name, &node)) continue;
            if (node_memtotal_kb(node) > 0)
                ++topo.nodes_with_mem;
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
     * 探测失败(nodes_with_mem==0)与未知档一律最差档。 */
    static const struct { int nodes; double mem, comp; } tbl[] = {
        { 1, 25.0, 5.0 },   /* 单节点内存: SVD 实测带 18-25 取上界; GEMM 4.4+余量 */
        { 2, 10.0, 4.0 },   /* 保守估计, TODO(实测): 无真 2 节点内存机器 */
        { 4,  5.0, 3.0 },   /* 同上 */
        { 8,  4.0, 2.5 },   /* >=8 节点 */
    };
    int n = t->nodes_with_mem;
    double worst = fam == SDC_FAM_MEM_BOUND ? 25.0 : 5.0;
    if (n <= 0) return worst;
    for (const auto &r : tbl)
        if (n <= r.nodes) return fam == SDC_FAM_MEM_BOUND ? r.mem : r.comp;
    return fam == SDC_FAM_MEM_BOUND ? 4.0 : 2.5;
}

int sdc_numa_node_of_cpu(int cpu)
{
    if (cpu < 0) return -1;
    DIR *d = opendir("/sys/devices/system/node");
    if (!d) return -1;
    struct dirent *de;
    int found = -1;
    while ((de = readdir(d)) != nullptr) {
        int node;
        if (!parse_node_dir(de->d_name, &node)) continue;
        char path[128], buf[1024];
        snprintf(path, sizeof path, "/sys/devices/system/node/node%d/cpulist", node);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(buf, sizeof buf, f) && cpu_list_contains(buf, cpu))
            found = node;
        fclose(f);
        if (found >= 0) break;
    }
    closedir(d);
    return found;
}

#endif /* __aarch64__ && __linux__ */
