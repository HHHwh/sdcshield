/* SPDX-License-Identifier: Apache-2.0 */
/* NUMA 拓扑探测与全核争抢慢化(AR)分档 —— eigen P0-P3 修复 spec §4.1。
 * AR(全核慢化系数)是 负载族×拓扑 的函数, 不是拓扑属性: 同一台
 * node0-only 机器实测 GEMM 4.4x vs SVD 18-25x(2026-10-27, Phase C)。
 * 表值宁可高估(时间预算收紧, 保 P0 不 timeout), P1 靠反馈闭环兜底。 */
#ifndef SDC_NUMA_H
#define SDC_NUMA_H

#if defined(__aarch64__) && defined(__linux__)

enum sdc_mem_family { SDC_FAM_MEM_BOUND, SDC_FAM_COMPUTE_BOUND };

struct sdc_numa_topology {
    int nodes_with_mem;              /* MemTotal>0 的节点数; 探测失败为 0 */
    int total_cpus;
};

const struct sdc_numa_topology *sdc_numa_probe(void);
double sdc_ar_factor(enum sdc_mem_family fam, const struct sdc_numa_topology *t);
int sdc_numa_node_of_cpu(int cpu);

#endif /* __aarch64__ && __linux__ */
#endif
