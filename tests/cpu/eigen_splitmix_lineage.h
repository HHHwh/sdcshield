/* eigen_splitmix_lineage.h — Eigen sizing 探针 splitmix 谱系的魔数/值域
 * 单一事实源 (issue #389, PR #392 检视 R1)。
 *
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 使用方(2 个家族, 填充结构不同故不共用填充模板):
 *   - eigen_gemm/gemm_solve_probe.h::fill_pair —— GEMM 联合求解探针,
 *     方阵对(a,b)标量双流/complex 四流填充;
 *   - eigen_svd/sandstone_eigen_common.h::sizing_probe —— SVD 联合求解
 *     探针, 单矩阵满秩谱系填充。
 * 谱系契约(检视 #51): 两家族的 probe 填充必须与各自 run() 的随机填充
 * (frandom_scale(2.0) - 1.0, 均匀随机 [-1,1))同谱系。收口前同一公式
 * 骨架在 Eigen dense sizing 探针内共 9 份逐字副本(GEMM 8 + SVD 1),
 * 跨家族的魔数/值域演化靠两处手工同步——正是收口要消灭的漂移模式
 * (0.0290→0.0315 静默陈旧先例)。魔数与值域档位在此单点定义后,
 * 演化只改本文件, 两家族编译期同步。
 * **范围边界(检视 #392-R3)**: 本头的单源声称仅覆盖上述 Eigen dense
 * sizing 探针谱系。kGolden/kMix 作为通用哈希常数在全仓另有大量独立
 * 使用(eigen_sparse.cpp 的非零元放置散列——其谱系契约对象是自身
 * run() 的 fill-in 结构, 仅借用同款 i*K1^j*K2 坐标混合且无 K3 终结化/
 * 值域档位; 以及 l2c/mmu/dep-chain 等测试的散列 tweak)——它们与本文
 * 件无共享谱系契约, 不随本头演化, 改本头不影响其行为(这是预期而
 * 非漂移)。勿将它们收口进本谱系(虚假耦合)。
 * 公式结构划分(检视 #392-R4): 家族无关的公式原语(初始流终结化
 * coord_finalize/单拍推进 advance/值域映射 to_range)在本文件单点,
 * 两家族逐字同构、演化只改本文件; 家族相关的流编排(GEMM 方阵对
 * 标量双流/complex 四流、SVD 单矩阵+complex (h,h2) 双值及拍数)由
 * 各使用方持有。
 * 漂移审计指引(检视 #392-R6): 演化本头(改魔数/值域档位/公式原语)后,
 * 以全仓 grep 魔数找漂移副本时, docs/superpowers/plans/ 下归档的取证
 * 快照(如 2026-11-01-gemm-probe-collapse.md, 文首勘误横幅已标注)含
 * 7 份旧实现逐字副本——取证冻结、不随本头演化, 命中属预期噪声而非
 * 漂移; 有效的审计对象是上述两家族代码路径中新增的未引用本头的
 * 逐字副本。
 * 类型分派边界(检视 #392-R8): complex 类型判定刻意不随公式原语收进
 * 本文件单点。GEMM 侧 is_std_complex 特化遵守其「文件群不引 boost」
 * 既有约束(收口时为模板化 GEMM 三变体而新写, std-only); SVD 侧
 * boost::is_complex 嵌在其共享头 6 处使用(RSS 家族分派×2/探针填充/
 * golden 构造 enable_if×2/run 侧填充), 仅换探针一处会造成同文件双
 * trait 并存, 全数替换则是对 x86 活动路径的又一次大规模非加性改写。
 * 两判定对两家族实际具现的 Eigen 标量域(double/float/std::complex,
 * Eigen 矩阵不产生非 std 复数包装)结论恒等, 分派分叉场景在具现域之
 * 外——勿为形式单源把判定收进本头(虚假耦合, 同范围边界原则)。
 * 演化复核路径(检视 #392-R8): CI 无 x86 构建目标(pr.yaml 构建矩阵仅
 * GCC-arm64), 本头演化会同步改写 SVD 探针的 x86 活动路径且无 CI 红/
 * 绿信号——等价性不随 CI 持续成立, 演化本头后须自查: aarch64 侧对
 * 全部反依赖 TU 做 objdump -d 新旧对比; x86 侧在任一 x86 主机构建
 * 新旧两版对比 svd 家族 TU(交叉汇编方法与基线取证见 PR #392 描述
 * 「x86-64 非回归」节)。
 *
 * 本头文件刻意无架构守门、不 include 任何 Eigen/boost 头: 两个使用方
 * 家族的源文件在 x86-64 参考构建中也编译, 这里只出现 std 常量——
 * constexpr 常量在两家族的 -O3 产物中折叠为同一立即数, 机器码与各自
 * 内联魔数写法逐指令一致(objdump 规范化对比实证)。 */
#ifndef EIGEN_SPLITMIX_LINEAGE_H
#define EIGEN_SPLITMIX_LINEAGE_H

#include <cstdint>

namespace sandstone_splitmix {

/* K1: splitmix64 黄金比例乘子(奇数), 亦作哈希流推进的加法常数。 */
inline constexpr std::uint64_t kGolden = 0x9E3779B97F4A7C15ull;
/* K2: 行/列坐标混合乘子(与 K1 互质, (r,c) 格点雪崩)。 */
inline constexpr std::uint64_t kMix = 0xC2B2AE3D27D4EB4Full;
/* K3: 终结化与流推进的雪崩乘子。 */
inline constexpr std::uint64_t kAvalanche = 0xD6E8FEB86659FD93ull;
/* 值域档位: 均匀随机 [-1,1) 千分之一步长——h % kRangeSteps /
 * kRangeScale - 1.0, 与 run() 期 frandom_scale(2.0) - 1.0 同谱系。
 * 不变量(检视 #392-R3): 均匀覆盖 [-1,1) 要求 steps == 2·scale
 * (h%steps/scale ∈ [0,2) 再减 1); 非对称编辑(只改其一)会静默破坏
 * 全部探针与 run() 的分布契约且无运行期信号, 以下编译期锁定。 */
inline constexpr unsigned kRangeSteps = 2000u;
inline constexpr double kRangeScale = 1000.0;
/* 精确 double 比较(检视 #392-R5): static_cast<unsigned>(kRangeScale) 截断
 * 只锁整数部分——kRangeScale 的小数编辑(1000.0→1000.25)会静默穿过整数
 * 断言而覆盖契约缩为 [-1, 0.9985)。2.0*kRangeScale(指数+1, 精确)与
 * unsigned→double(该值域精确)均无舍入, 等值比较封死小数漂移。 */
static_assert(static_cast<double>(kRangeSteps) == 2.0 * kRangeScale,
              "[-1,1) uniform coverage requires kRangeSteps == 2*kRangeScale");

/* ---- 家族无关的公式原语(检视 #392-R4): 初始流终结化/单拍推进/值域
 * 映射在两家族逐字同构, 与家族相关的填充结构(流的拍数与用法)不同——
 * 原语在此单点, 演化(如 xorshift 量、二段雪崩)只改本文件, 两家族
 * 编译期同步; 各使用方只保留家族相关的流编排。inline -O3 内联后与
 * 手写展开生成相同机器码(objdump 规范化对比实证)。 ---- */

/* 初始流终结化: (r,c) 坐标混合 → 首个哈希值。 */
inline std::uint64_t coord_finalize(std::uint64_t r, std::uint64_t c)
{
    std::uint64_t h = r * kGolden ^ c * kMix;
    h ^= h >> 32;
    h *= kAvalanche;
    h ^= h >> 32;
    return h;
}

/* 单拍推进: x = x*K3 + K1 再 xorshift32(splitmix 雪崩)。 */
inline std::uint64_t advance(std::uint64_t x)
{
    x = x * kAvalanche + kGolden;
    x ^= x >> 32;
    return x;
}

/* 值域映射: 哈希 → 均匀随机 [-1,1)。 */
inline double to_range(std::uint64_t h)
{
    return (double)(h % kRangeSteps) / kRangeScale - 1.0;
}

}  // namespace sandstone_splitmix

#endif  /* EIGEN_SPLITMIX_LINEAGE_H */
