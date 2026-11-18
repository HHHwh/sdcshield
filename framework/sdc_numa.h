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
 * 按 node_MemFree / ΣMemFree 比例分摊全局四源预算 sdc_budget_bytes
 * (permille)(其内已含 MemAvailable×0.9、cgroup 上界与 strict
 * overcommit 承诺余量[#310], 节点合计不越全局)。退化规则(C1): 无内存
 * 节点(MemTotal=0)的线程需求并入 fallback
 * 节点(有内存节点中 MemFree 最大者) —— node0-only 机器上等价于全局模型。 */
uint64_t sdc_node_budget_bytes(int permille, int cpu);

/* 该 cpu 所在节点(含并入的无内存节点)的线程数 —— 节点预算分摊的分母。
 * 探测失败(无节点内存信息)退化为全局在线 CPU 数。 */
int sdc_numa_effective_threads(int cpu);

/* sdc_numa_effective_threads 的选中设备集口径(issue #515): 折算域从
 * 「全机在线 CPU」(sysfs, --cpuset/taskset 无关)收缩为「框架实际选中的
 * 设备集」——启动亲和(taskset 生效, ambient_logical_processor_set)与
 * --cpuset/--deviceset 过滤后的 shmem->device_info[], 即 worker 的真实
 * 分布域。返回: effective 节点(含 C1 并入, 与上函数同一归属口径)命中
 * 的选中设备数; *total_selected 非空时写入选中设备总数(折算分母)。
 * 选中集不可读(shmem 未建/设备数为 0)或 NUMA 探测失败返回 -1 且
 * *total_selected 置 0 —— 调用方退回全机口径(修复前行为)。全核时
 * 返回值与 sdc_numa_effective_threads(cpu) 恒等, *total_selected 与
 * sdc_numa_probe()->total_cpus 恒等(默认路径零变化); 跨节点均匀的
 * 受限集下新旧折算同值(均匀假设仅对非均匀集失真)。 */
int sdc_numa_effective_threads_selected(int cpu, int *total_selected);

/* /proc/self/status 的 VmHWM(峰值 RSS, kB→字节); 读不到返回 0。
 * T7 反馈闭环专用 —— 刻意不用 memfpt_current_high_water_mark(): 它在
 * release 构建返回 0, 且把它转正会激活 x86 诊断路径(spec §4.4 修正 #2)。
 * 口径注意: fork child 的 VmHWM 起点 = 父进程当时 RSS(实测 8.3MB)。
 * 反馈写入点自 issue #385 起在 run_one_test_inner 入口采样该起点并在
 * 折算 eff_ratio 时扣除(见 sandstone_run.cpp sdc_feedback_write); 联合
 * 求解探针的高水位仍计入(探针远小于 n_mem 路径的正式工作集, 小 n 路径
 * 由读侧 4×静态锚上钳兜底)。 */
uint64_t sdc_read_vm_hwm(void);

/* 联合口径查询回调的注册点(issue #747 依赖倒置修复): 配置层
 * (sdc_stress_config.h)在登记口径时把查询函数注册给框架, 框架的
 * VmHWM 反馈写入点经回调取 per-test 联合求解口径(n/slice 线程数/
 * 元素字节)——调度 TU 不再 include 配置层头、不直接查其跨 TU 账本,
 * 框架与测试 sizing 机制解耦。回调契约: test_id 无登记条目或口径无效
 * 返回 false; 三个出参仅在返回 true 时写入。注册须先于读取: 配置层与
 * 反馈写入点同在 child 进程内(init 登记在先、run 尾写入在后), child
 * 从 parent fork 时本指针为 null(parent 不跑 test_init)——未走联合
 * 求解的 child 无注册/无条目, 写入点不写(与修复前无条目口径一致)。 */
typedef bool (*sdc_joint_ctx_lookup_fn)(const char *test_id, int *n,
                                        int *threads, int *elem_bytes);
void sdc_register_joint_ctx_lookup(sdc_joint_ctx_lookup_fn fn);

#endif /* __aarch64__ && __linux__ */
#endif
