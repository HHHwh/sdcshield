/* gemm_solve_probe.h — GEMM 联合求解 sizing 探针的共享实现 (issue #389)。
 *
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 谱系契约(检视 #51)在 GEMM 家族承载: probe 填充必须与各测试 run() 的
 * fill_rand(frandom_scale(2.0) - 1.0, 均匀随机 [-1,1))同谱系——splitmix
 * 确定性哈希满足该契约, 且不依赖 init aux 线程上未定义的 per-thread RNG
 * 状态。GEMM 计时对数值不敏感(无 SVD 式 deflation 路径), 谱系由分布
 * 一致性满足; 填充 O(n²) 相对乘法 O(n³) 已计入探针计时(保守方向)。
 * 改填充结构必须与各 run() 的 fill_rand 同步演化; 魔数/值域档位则取自
 * ../eigen_splitmix_lineage.h 单一事实源(与 eigen_svd 家族的
 * sizing_probe 共享, 检视 #392-R1)——改值域只改该头, 勿在此内联魔数。
 *
 * 收口背景(issue #389): 本填充曾以 8 份逐字副本分散在 double14 /
 * gemm_{double,float,cdouble}_dynamic_square{,_dump} 7 个 T9 文件与
 * gemm_double_max_arm_body(T6 源头, 按 4 个 VL tier 编译), 谱系漂移
 * 靠 8 处手工同步维持——任何一处漂移即静默破坏谱系(0.0290→0.0315
 * 静默陈旧先例, framework RSS 表 T9 重标定)。设计同 eigen_svd/
 * sandstone_eigen_common.h::sizing_probe 的 PR#302 检视 #14 收口。
 *
 * 本头文件刻意不 include 任何 Eigen 头: gemm_double_max_arm_body.cpp
 * 以 -DEigen=EigenSVEn 宏改名后按 tier 编译, 这里出现 Eigen:: 符号
 * 会被宏劫持; std 头(<cassert>/<complex>/<cstdint>/<type_traits>)不受
 * 影响。include 块置于架构守门之内(检视 #392-R3): 守门内的 namespace
 * 是这些头的唯一消费者, x86-64 预处理视角下本头展开为空壳(rule 5
 * x86 untouched 构造成立, 非勘误口径); SVD 侧 sandstone_eigen_common.h
 * 的守门外 include 因其在 x86 也编译而属必需, 与此处不同。
 * 守门边界(检视 #392-R5): 只守 __aarch64__、不附 __linux__——本头零
 * Linux 依赖(纯 std 头), 而 gemm_double_max_arm_body.cpp(meson 门控
 * 仅 cpu_family=='aarch64', 无 OS 谓词)的 solve_once 转发无 #else 回退,
 * 本头必须在其整个编译域(aarch64 全 OS)可见, 否则 aarch64-非 Linux
 * 构建从可编译收窄为硬编译错误(收口前该 TU 的内联实现无 OS 守门)。
 * 7 个 T9 wrapper 自带 __aarch64__&&__linux__ 守门(其 aarch64 路径走
 * sdc_joint_dim_slice 联合求解, 依赖框架 __linux__ 实现; 非 Linux 下
 * wrapper 关闭、mdim 回退 legacy 链, 不实例化本头模板、无符号引用)。
 * 措辞勘误(检视 #392-R9): 原文「不引用本头」失准——7 个 TU 的
 * include 指令在各自守门块之外无条件生效, 非 Linux 下本头仍全量展开
 * (命名空间/std 头/lineage 头在编译面可见, 唯模板不具现), 编译暴露面
 * 以展开口径如实记。T9 wrapper 守门与头守门的包含关系不变——是本
 * 守门的真子集, 不受放宽影响。 */
#ifndef GEMM_SOLVE_PROBE_H
#define GEMM_SOLVE_PROBE_H

#if defined(__aarch64__)

#include <cassert>
#include <complex>
#include <cstdint>
#include <type_traits>

#include "../eigen_splitmix_lineage.h"

namespace gemm_probe {

/* std::complex 判定(SVD 先例用 boost::is_complex; GEMM 文件群不引
 * boost, 自写特化保持依赖面不变)。 */
template <typename T> struct is_std_complex : std::false_type {};
template <typename T> struct is_std_complex<std::complex<T>> : std::true_type {};

/* 填充一对 n×n 方阵: 家族无关的公式原语(初始流终结化 coord_finalize/
 * 单拍推进 advance/值域映射 to_range)取自 ../eigen_splitmix_lineage.h
 * (检视 #392-R4 单源化), 本侧只持家族相关的流编排——标量分支
 * a←h、b←h2(1 拍), complex 分支 a←(h,h2)、b←(h3,h4)(3 拍), 与收口前
 * 的三个变体逐位一致(float 经 (Scalar) 先算 double 再收窄)。 */
template <typename Mat>
static void fill_pair(Mat &a, Mat &b)
{
    using Scalar = typename Mat::Scalar;
    constexpr bool kComplex = is_std_complex<Scalar>::value;
    using sandstone_splitmix::advance;
    using sandstone_splitmix::coord_finalize;
    using sandstone_splitmix::to_range;
    /* 契约: a、b 为同维 n×n 方阵。本 header 是共享基础设施(8 个 wrapper
     * 使用方之外的未来调用方可直接 include), 尺寸不符时下方 b(r, col)
     * 写入即越界——release 构建(本仓 meson b_ndebug=if-release)下 Eigen
     * operator() 无边界检查, 静默堆越界写 UB。assert 在 debug 构建抓
     * 误用; release 下 NDEBUG 展开为 ((void)0), 探针机器码与计时不受
     * 影响(检视 #392-R1)。
     * 防护边界如实声明(检视 #392-R3): release 产品二进制中本断言为
     * 空, 防护等级与所依赖的 Eigen operator() 的 NDEBUG 行为对齐——
     * 未来调用方在 release 下的误用防线是 wrapper 的同文件可见性与
     * 检视流程, 非运行期检查; 这是 assert 语义的设计权衡而非遗漏。 */
    assert(a.cols() == a.rows() && b.rows() == a.rows()
           && b.cols() == a.rows());
    const int n = a.rows();
    for (int r = 0; r < n; ++r)
        for (int col = 0; col < n; ++col) {
            std::uint64_t h = coord_finalize((std::uint64_t)r,
                                             (std::uint64_t)col);
            if constexpr (kComplex) {
                std::uint64_t h2 = advance(h);
                std::uint64_t h3 = advance(h2);
                std::uint64_t h4 = advance(h3);
                a(r, col) = Scalar(to_range(h),  to_range(h2));
                b(r, col) = Scalar(to_range(h3), to_range(h4));
            } else {
                std::uint64_t h2 = advance(h);
                a(r, col) = (Scalar)to_range(h);
                b(r, col) = (Scalar)to_range(h2);
            }
        }
}

/* 联合求解探针: 构两张 Mat + 同谱系填充 + 一次乘法, 结果丢弃只为
 * 计时。调用方 sdc_joint_dim_slice 的 probe 参数是 C 函数指针
 * int solve_once(int), 各测试以一行转发 wrapper 提供该符号。 */
template <typename Mat>
static int solve_once(int n)
{
    Mat a(n, n), b(n, n), c;
    fill_pair(a, b);
    c = a * b;
    return 0;
}

}  // namespace gemm_probe

#endif  /* __aarch64__ */
#endif  /* GEMM_SOLVE_PROBE_H */
