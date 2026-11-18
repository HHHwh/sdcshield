// SPDX-License-Identifier: Apache-2.0
//
// cpp23_polyfill.h — C++23/C++20 标准库缺失符号的 polyfill,仅用于在 GCC<12 的旧
// openEuler(22.03 / 20.03,GCC 10.x)上构建 SDCShield (derived from OpenDCDiag) 的 ARM64 CPU 路径。
//
// 设计要点(满足 CLAUDE.md "宏隔离 / 24.03 源码字节一致" 约束):
//   * 本文件是新增文件,不改任何既有头/源码。
//   * 通过 CXXFLAGS 注入 `-DOPENEULER_22_03 (或 OPENEULER_20_03)
//     -include .../cpp23_polyfill.h` 在编译期前置包含;既有源码行无需任何 #include 改动。
//   * 所有补丁受 `__cpp_lib_*` 特性宏守卫:只在编译器/库确实缺失该符号时才生效。
//     → 在 GCC 12+(24.03,gnu++23 下这些宏全部定义)上,下面的 #if 分支全部不进入,
//       预处理器输出与未 -include 本文件时逐字节一致 → 24.03 源码字节一致得以保证。
//   * 守卫还加了 OPENEULER_22_03 / OPENEULER_20_03 双保险:即使误在 24.03 构建里
//     -include 了本文件,只要没定义这两个宏,polyfill 体仍不生效。
//   * 宿主机(非容器)构建时,meson.build 用 `cpp.has_header('barrier')` 探测旧 libstdc++,
//     命中即以 -DSDCSHIELD_COMPAT_LIBSTDCXX 注入同一组标志(见 meson.build 顶部);
//     容器路径仍用 OPENEULER_* 宏,两者等价且可叠加。
//
// 宏命名说明:
//   目标要求适配宏名为 "OPENEULER_22.03" / "OPENEULER_20.03",但 C 预处理器的
//   宏标识符不允许含点号('.'),#define OPENEULER_22.03 非法(点号被丢弃,实际定义成
//   OPENEULER_22)。故采用合法等价名 OPENEULER_22_03 / OPENEULER_20_03(点号→下划线,
//   工程惯例)。语义与原意图一致:仅用于隔离 22.03/20.03 的适配代码,不触碰 24.03。
//
// 覆盖范围(经实测扫描 ARM64 CPU 构建路径):
//   - std::to_underlying  (C++23, <utility>, GCC>=12)  ← 22.03/20.03 GCC10 缺
//   - std::bit_floor/ceil (C++20, <bit>,    GCC>=12 完整) ← GCC10 <bit> 不完整
//   - std::barrier        (C++20, <barrier>,GCC>=11 libstdc++) ← GCC10 缺头文件
//     仅 framework/device/cpu/topology_cpu.h 的 BarrierDeviceScheduler 用,
//     API 面: 构造(count, completion_fn) + arrive_and_wait + arrive_and_drop。
//   注: std::span(GCC10 已支持)、operator<=>(GCC10 已支持)无需 polyfill。
//   注: std::format 仅用于 x86_64 路径(#ifdef __x86_64__),ARM64 不编,无需 polyfill。
//   注: std::string/string_view::contains (C++23, P1679R3, libstdc++ GCC>=12) 是
//     成员函数,本 polyfill 无法补(只能补命名空间层自由符号)——全仓禁用该 API,
//     一律用 find() != npos 等价替代(否则 debug 构建在 gnu++20/旧 libstdc++ 上必现
//     编译失败, release 被 NDEBUG 掩盖; issue #327 条目 2)。
#pragma once

// 只在确实命中旧编译器 + 适配宏时才动 std 命名空间。
#if (defined(OPENEULER_22_03) || defined(OPENEULER_20_03) || \
     defined(SDCSHIELD_COMPAT_LIBSTDCXX)) && defined(__cplusplus)

#include <version>      // __cpp_lib_* 特性宏( libstdc++ 10 起提供)
#include <type_traits>   // underlying_type_t
#include <cstddef>       // size_t
#include <climits>       // CHAR_BIT
#include <functional>     // std::function
#include <pthread.h>      // pthread_cond_t / pthread_mutex_t / clockid_t
#include <time.h>         // struct timespec

// ---------------------------------------------------------------------------
// pthread_cond_clockwait 兜底实现 (C++ <condition_variable> 依赖)
// gcc-toolset-10 的 <condition_variable> 头调用 pthread_cond_clockwait
// (glibc 2.30 引入)。20.03 SP4 的 glibc 2.28-129 回填了该符号(头+库都有),
// 但 20.03 LTS/SP1/SP2/SP3 的 glibc 2.28-36/63/79/97 既未声明也无符号
// → <condition_variable> 编译报 "declaration must be available" + 链接缺符号。
// 本处在 glibc < 2.30 且未声明时提供一个 static inline 退化实现, 并在 #include
// <condition_variable> 之前用宏重定向调用点(关键: 宏必须在 <condition_variable>
// 被首次包含前定义, 否则头内的调用不会被重写)。
//   退化语义: 忽略 clockid 参数, 直接用 pthread_cond_timedwait (默认 CLOCK_REALTIME)。
// 对 SDCShield 的 BarrierDeviceScheduler (std::barrier 底层 condition_variable) 足够。
// glibc >= 2.30 或 SP4 的回填版本(头已声明) → __GLIBC_PREREQ(2,30) 为真, 本块不生效。
// ---------------------------------------------------------------------------
#if defined(__GLIBC__) && !__GLIBC_PREREQ(2, 30)
__BEGIN_DECLS
static inline int __attribute__((unused))
__sdcshield_pthread_cond_clockwait(pthread_cond_t *__restrict __cond,
                                    pthread_mutex_t *__restrict __mutex,
                                    clockid_t __clk, const struct timespec *__abstime)
{
    (void)__clk;  // 退化: 忽略 clockid, 用默认 CLOCK_REALTIME
    return pthread_cond_timedwait(__cond, __mutex, __abstime);
}
__END_DECLS
// 必须在 <condition_variable> 被包含前定义此宏, 才能重写头里的调用。
#  ifndef pthread_cond_clockwait
#    define pthread_cond_clockwait __sdcshield_pthread_cond_clockwait
#  endif
#endif /* glibc < 2.30 */

#include <mutex>          // std::mutex, lock_guard
#include <condition_variable>
#include <cstddef>        // ptrdiff_t / max_align_t

// ---------------------------------------------------------------------------
// std::to_underlying  (C++23, P1682, __cpp_lib_to_underlying)
// GCC 10 ~ 11 缺;GCC 12+ 有。ARM64 CPU 路径 5 个文件用(LogLevelVerbosity 等)。
// ---------------------------------------------------------------------------
#if !defined(__cpp_lib_to_underlying)
namespace std {
    template <typename _Enum>
    constexpr typename underlying_type<_Enum>::type
    to_underlying(_Enum __e) noexcept {
        return static_cast<typename underlying_type<_Enum>::type>(__e);
    }
} // namespace std
#endif // !__cpp_lib_to_underlying

// ---------------------------------------------------------------------------
// std::bit_floor / std::bit_ceil  (C++20, <bit>, __cpp_lib_bit_ops)
// GCC 10 的 <bit> 已提供 bit_floor/ceil(实测可用),只是未定义 __cpp_lib_bit_ops
// 特性宏。故这里不补 bit_floor/ceil —— 补了会与 libstdc++ 既有的重载冲突
// (ambiguous overload)。GCC12+ 同样无需补。本段保留说明,不注入任何定义。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// std::barrier  (C++20, <barrier>, __cpp_lib_barrier)
// libstdc++ 11+ 才提供 <barrier>;GCC 10 缺头文件。
// 本实现用 mutex + condition_variable 模拟阶段屏障(phase barrier):
//   - 构造(count, completion_fn): count 个参与方,每阶段结束调 completion_fn
//   - arrive_and_wait(): 计数 -1, 到 0 时触发 completion + 重置, 全部唤醒
//   - arrive_and_drop(): 永久退出(count 永久 -1, 不等待)
// 语义与 std::barrier<CompletionFunction> 在 SDCShield 的用法上一致
// (拓扑 reschedule 的成对/分组同步)。completion_fn 在阶段边界被恰好一次调用。
// 仅当库未提供时补;GCC11+ 不生效。
// ---------------------------------------------------------------------------
#if !defined(__cpp_lib_barrier)
namespace std {

// 需要默认构造的 completion function 包装(与 std::barrier<Function> 的 Function 兼容)。
// SDCShield 用 std::function<void()>, 故 completion 包装存 std::function。
template <typename _CompletionFunction = std::function<void()>>
class barrier
{
public:
    barrier(ptrdiff_t __count, _CompletionFunction __completion = _CompletionFunction())
        : _M_expected(__count), _M_waiting(__count), _M_generation(0),
          _M_completion(std::move(__completion)) {}

    // arrive_and_wait: 到达并等待本阶段全部到达
    void arrive_and_wait()
    {
        std::unique_lock<std::mutex> __lk(_M_mtx);
        ptrdiff_t __gen = _M_generation;
        if (--_M_waiting == 0) {
            // 本阶段最后一个: 触发 completion, 重置计数, 唤醒所有等待者
            if (_M_completion) _M_completion();
            _M_waiting = _M_expected;
            ++_M_generation;
            _M_cond.notify_all();
        } else {
            _M_cond.wait(__lk, [this, __gen] { return __gen != _M_generation; });
        }
    }

    // arrive_and_drop: 永久退出(预期参与方 -1, 不等待)
    void arrive_and_drop()
    {
        std::unique_lock<std::mutex> __lk(_M_mtx);
        if (--_M_waiting == 0) {
            if (_M_completion) _M_completion();
            --_M_expected;                 // 下一阶段永久少一个
            _M_waiting = _M_expected;
            ++_M_generation;
            _M_cond.notify_all();
        } else {
            --_M_expected;                 // 下一阶段少一个,本阶段继续等剩余
        }
    }

    barrier(const barrier&) = delete;
    barrier& operator=(const barrier&) = delete;

private:
    std::mutex _M_mtx;
    std::condition_variable _M_cond;
    ptrdiff_t _M_expected;
    ptrdiff_t _M_waiting;
    ptrdiff_t _M_generation;
    _CompletionFunction _M_completion;
};

} // namespace std
#endif // !__cpp_lib_barrier

#endif // (OPENEULER_22_03 || OPENEULER_20_03) && __cplusplus

