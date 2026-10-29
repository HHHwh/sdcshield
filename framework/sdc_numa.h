/* SPDX-License-Identifier: Apache-2.0 */
/* NUMA 拓扑探测与全核争抢慢化(AR)分档 —— eigen P0-P3 修复 spec §4.1。
 * AR(全核慢化系数)是 负载族×拓扑 的函数, 不是拓扑属性: 同一台
 * node0-only 机器实测 GEMM 4.4x vs SVD 18-25x(2026-10-27, Phase C)。
 * 表值宁可高估(时间预算收紧, 保 P0 不 timeout), P1 靠反馈闭环兜底。 */
#ifndef SDC_NUMA_H
#define SDC_NUMA_H

#if defined(__aarch64__) && defined(__linux__)

#include <cstdint>

enum sdc_mem_family { SDC_FAM_MEM_BOUND, SDC_FAM_COMPUTE_BOUND };

/* sysfs 节点容量上限: 节点 id >= 此值的 nodeN 目录被忽略(实测服务器拓扑
 * 远低于此; 记录在案的有界性妥协, 超出节点按"不存在"处理)。 */
#define SDC_NUMA_MAX_NODES 64

/* 每 NUMA 节点的内存/CPU 概要(probe 一次性缓存)。 */
struct sdc_numa_node {
    uint64_t memtotal_bytes;    /* nodeN/meminfo MemTotal; 0 = 无内存/不可读 */
    uint64_t memfree_bytes;     /* nodeN/meminfo MemFree; 0 = 不可读/无 */
    int ncpus;                  /* nodeN/cpulist 覆盖的 CPU 数(在线); 0 = 无 cpulist */
};

struct sdc_numa_topology {
    int nodes_with_mem;              /* MemTotal>0 的节点数; 探测失败为 0 */
    int total_cpus;                  /* cpu/online 覆盖数; 不可读为 0 */
    struct sdc_numa_node per_node[SDC_NUMA_MAX_NODES];   /* 按节点 id 索引 */
};

const struct sdc_numa_topology *sdc_numa_probe(void);
double sdc_ar_factor(enum sdc_mem_family fam, const struct sdc_numa_topology *t);
int sdc_numa_node_of_cpu(int cpu);

/* 节点合成预算(spec §4.2): 节点 meminfo 无 MemAvailable 字段(实测),
 * 按 node_MemFree / ΣMemFree 比例分摊全局三源预算 sdc_budget_bytes
 * (permille)(其内已含 MemAvailable×0.9 与 cgroup 上界, 节点合计不越
 * 全局)。退化规则(C1): 无内存节点(MemTotal=0)的线程需求并入 fallback
 * 节点(有内存节点中 MemFree 最大者) —— node0-only 机器上等价于全局模型。 */
uint64_t sdc_node_budget_bytes(int permille, int cpu);

/* 该 cpu 所在节点(含并入的无内存节点)的线程数 —— 节点预算分摊的分母。
 * 探测失败(无节点内存信息)退化为全局在线 CPU 数。 */
int sdc_numa_effective_threads(int cpu);

/* /proc/self/status 的 VmHWM(峰值 RSS, kB→字节); 读不到返回 0。
 * T7 反馈闭环专用 —— 刻意不用 memfpt_current_high_water_mark(): 它在
 * release 构建返回 0, 且把它转正会激活 x86 诊断路径(spec §4.4 修正 #2)。
 * 口径注意: fork child 的 VmHWM 起点 = 父进程当时 RSS(实测 8.3MB,
 * eigen 家族 <1%, 记录为已知口径); 联合求解探针的高水位同样计入
 * (探针远小于正式工作集)。 */
uint64_t sdc_read_vm_hwm(void);

#endif /* __aarch64__ && __linux__ */
#endif
