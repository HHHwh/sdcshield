# 如何给 SDCShield 添加新测试用例（研究整理）

> 来源：`docs/writing_tests.md`（官方指南）+ `tests/cpu/meson.build` + `tests/cpu/arm64/meson.build`
> + `framework/sandstone.h`（权威 API）+ 仓库 CLAUDE.md 规则。核对日期 2026-09-18。

## 0. 最小可用模板（C/C++ 皆可）

```c
/**
 * @copyright
 * Copyright 2026 <owner>.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b my_test
 * @parblock
 * <一段描述：这个测试做什么、怎么压测、期望抓什么 SDC>
 * @endparblock
 */

#include "sandstone.h"

static int my_init(struct test *test)
{
    /* 生成 golden 数据（可用 random32/random64/memset_random） */
    return EXIT_SUCCESS;
}

static int my_run(struct test *test, int cpu)
{
    TEST_LOOP(test, 1 << 13) {
        /* 计算 + 与 golden 比对 */
        memcmp_or_fail(actual, golden, nelems, "描述字符串");
    }
    return EXIT_SUCCESS;
}

static int my_cleanup(struct test *test)
{
    /* 可选：释放 test->data（非 slicing 测试可不 free，进程退出即回收） */
    return EXIT_SUCCESS;
}

DECLARE_TEST(my_test, "一行描述")
        .test_init = my_init,
        .test_run = my_run,            /* 唯一必须非 NULL 的字段 */
        .test_cleanup = my_cleanup,
        .quality_level = TEST_QUALITY_BETA,   /* 新测试一律先 BETA，跑稳后升 PROD */
END_DECLARE_TEST
```

注册进构建（`tests/cpu/meson.build`，放对集合）：
```meson
tests_set_base.add( files( 'mydir/my_test.cpp' ) )
```
然后 `meson setup --reconfigure builddir ... && ./scripts/build.sh`（改 meson 必须
reconfigure，plain ninja 感知不到），`./builddir/sdcshield --quality=0 --list-tests | grep my_test`。

## 1. 测试生命周期（必须理解）

| 回调 | 运行处 | 时机 | NULL 允许 |
|---|---|---|---|
| `test_preinit` | **父进程** | 每应用运行一次（之后被置 NULL，不随 fracturing 重跑） | ✅ |
| `test_init` | 子进程主线程 | 每次测试调用（fracturing/slicing 会多次，但各自独立或同进程!见下） | ✅ |
| `test_run` | 子进程每 CPU 线程 | 压测主体 | ❌ **必须非 NULL** |
| `test_cleanup` | 子进程主线程 | 测试结束（含干净失败后） | ✅ |
| `test_postcleanup` | 子进程主线程 | 每应用运行一次 | ✅ |

- **fracturing**：同一测试以不同种子在**新子进程**里反复跑 → `test_init` 多次但进程独立，
  不 free 内存也无所谓。
- **slicing**（声明了 `max_threads`）：`test_init` 在**同一进程**里被多次调用 → 必须
  在 cleanup 里 free，否则真泄漏。slicing 适合每线程内存开销大的测试。
- 返回值：`EXIT_SUCCESS`(0) 通过；`EXIT_SKIP`(-255) 或任意负值 = 跳过（先 `log_skip(类别, "理由")`）；
  `-errno` 表示因系统调用失败而跳过。

## 2. `struct test` 常用字段（sandstone.h，权威）

| 字段 | 作用 |
|---|---|
| `.test_run` | 必填 |
| `.quality_level` | `TEST_QUALITY_BETA`(0) 新测试默认 / `TEST_QUALITY_PROD`(2) / `TEST_QUALITY_SKIP`(-1) |
| `.minimum_cpu` | 特性位掩码（`cpu_features.h`，构建期生成勿手编）；运行时 `device_has_feature()` 检测，不满足自动 skip |
| `.groups` | `DECLARE_TEST_GROUPS(&group_math)` 等，组定义在 `framework/sandstone_test_groups.h`（现有：compression/ipsec/math/fuzzing/special；x86 另有 kvm） |
| `.desired_duration` / `.minimum_duration` / `.maximum_duration` | 时长控制（ms）；desired<0 = 不循环只跑一次 |
| `.fracture_loop_count` | <0 永不分裂；0 自动；>0 固定内层计数 |
| `.max_threads` | 启用 slicing 的每片线程上限 |
| `.flags` | 见 test_flags 枚举（`test_is_optional`、`test_init_in_parent`、`test_requires_smt` 等） |
| `.data` | 测试私有指针（init 存 / run 取 / cleanup 释放）——**推荐替代全局 static** |

## 3. 框架 API 速查（都在 sandstone.h）

- **循环**：`TEST_LOOP(test, N)`——N 是内层粒度（约定 2 的幂）；框架到点叫停。debugoptimized
  构建 + `--test-tests` 可让框架推荐 N 的合理值（内层太短会报 "Inner loop is too short"）。
- **比对**：`memcmp_or_fail(actual, expected, nelems, "desc")`——**类型感知，第三参是元素个数
  不是字节数**；失配自动 dump actual/expected 十六进制 + offset + mask 并终止线程。
- **失败**：`report_fail()` / `report_fail_msg(fmt, ...)`（`[[noreturn]]`）。
- **日志**：`log_skip/log_debug/log_info/log_warning/log_error`（printf 风格）、`log_data`（二进制）。
  默认每线程 5 条/128B 上限。**成功路径禁打日志**（log_debug 除外）。
- **随机**：`random32/random64/frandomf/memset_random` 等；每线程独立状态；libc rand/random
  被框架接管。失败复现：日志 `state: { seed: 'AES:...' }` → `-s 'AES:...'` 重放。
- **内存**：`malloc/aligned_alloc` 被框架接管（失败即退线程，返回内存已清零）；
  `aligned_alloc_safe(align, size)` 自动把 size 补齐到 align 的倍数。
- **CPU 信息**：`num_cpus()`；`test_run` 的 `cpu` 参数是索引，查 `device_info[cpu]`
  （core_id/numa_id/die_id/L3 cache 等）。

## 4. 本仓库的构建分区（tests/cpu/meson.build sourceset 机制）

四个 sourceset，决定源文件用什么编译选项：

| 集合 | 编译选项 | 用途 |
|---|---|---|
| `tests_set_base` | 基线 march | 通用测试（绝大多数） |
| `tests_set_hsw`（仅x86） | `-march=haswell` + `-DEigen=EigenAVX2` | x86 AVX2 |
| `tests_set_skx`（仅x86） | `-march=skylake-avx512` + `-DEigen=EigenAVX512` | x86 AVX512 |
| `tests_set_sve`（仅aarch64） | `-march=armv8.2-a+sve -msve-vector-bits=128` + `-DEIGEN_ARM64_USE_SVE -DEigen=EigenSVE` | ARM SVE（Eigen 命名空间改名避免与 NEON 基线符号冲突） |

另外几个独立 static_library（带专属 march）：
- `tests_crc`（aarch64）：`-march=armv8.1-a+crc`——arm_acle CRC intrinsics
- `tests_arm64`（`tests/cpu/arm64/meson.build`）：`-march=armv8.1-a+crc+crypto`——NEON+AES
- `tests_arm64_sve`（同文件）：`-march=armv8.2-a+sve`——SVE 全长向量族（每测试 init 里
  `#include <device/cpu/arm64/arm64_vl.h>` 后调
  `arm64_require_sve_vl(test, min_bits, max_bits)` 一行完成能力+范围探测，跳过时它返回
  `EXIT_SKIP`(-255)——用 `if (vl < 0) return EXIT_SKIP;` 承接（成功侧 vl 是字节数，需保存）；
  无 SVE 硬件干净 skip）
- vendored 库探测（openblas/sleef/isa-l/openssl）：`install/` 存在才加，**缺失给 message
  + 空列表，绝不用 disabler()**（会静默杀掉整个二进制的依赖链）

**条件加入模式**：
```meson
# 依赖 gate 型：
zstd_dep = dependency('libzstd', static : dep_static)
tests_set_base.add(when: zstd_dep, if_true: files('zstd/test.c'))

# arch gate 型：
if host_machine.cpu_family() == 'aarch64'
    tests_set_base.add(when: memcpy_rewr_dep, if_true: files('memory/memcpy_rewr.cpp'))
endif

# 文件存在探测型（vendored）：
sleef_install = meson.current_source_dir() / '../../third-party/sleef/install'
if host_machine.cpu_family() == 'aarch64' and run_command('test', '-f',
        sleef_install / 'lib/libsleef.a', check: false).returncode() == 0
    ...declare_dependency + tests_set_base.add(...)
endif
```

## 5. 新测试放置位置决策树

```
测试与架构无关（纯 C/C++/std::atomic）？
  → tests/cpu/<域>/ + tests_set_base
ARM NEON/CRC/Crypto/inline-asm 专属？
  → tests/cpu/arm64/ + tests/arm64 的 meson（自带 +crc+crypto march）
     （整个子目录只在 aarch64 进构建，x86 完全不编）
需要 SVE（<arm_sve.h> / svmla_*）？
  → tests/cpu/arm64/ 源文件 + 加进 tests_arm64_sve 库（+sve march）
     且 init 里必须 arm64_require_sve_vl()，负返回值即 EXIT_SKIP（本机 0xd01 无 SVE）
依赖 Eigen 且要 SVE 后端？
  → tests_set_sve（EigenSVE 命名空间隔离）
系统级/架构无关跨设备（mce_check, smi_count 类）
  → tests/common/ + tests/common/meson.build
要 vendored 第三方库？
  → 仿 openblas/sleef 块：third-party/<lib>/{tarball,build.sh,install/}，
     meson 两级 gate（vendored 优先，回退系统库，再无则 message 跳过）
```

## 6. 本仓库特有纪律（CLAUDE.md，必须遵守）

1. **x86-64 不动规则**：ARM64 移植是增量。优先 `#elif defined(__aarch64__)` 分支和
   per-arch meson gate，不改 x86 逻辑。真共享的代码才把 `#ifdef __x86_64__` 拓宽为
   `#if defined(__x86_64__) || defined(__aarch64__)`。
2. **占位测试诚实原则**：暂未实现的功能测试必须 `log_skip(类别, "to be implemented
   (placeholder): <缺什么>")` + `return EXIT_SKIP`——**禁止** no-op 返回 SUCCESS 伪装通过。
   真缺的硬件（ARM 无 microcode/PPIN/MSR）保持 documented stub。
3. **golden 比对是逐字节 memcmp**：浮点必须可复现（框架在 sandstone_data.cpp 统一静默
   SNaN 正是为此）；NaN 类别比对（非逐字节 payload）见 `fsu_byteexact_arm.cpp` 先例。
4. **一个单元一个 patch**：一个特性/bug/适配点一个 commit，不捆绑。非平凡改动必须先写
   计划到 `docs/superpowers/plans/YYYY-MM-DD-<feature>.md`（用 superpowers:writing-plans
   skill），按计划逐项 commit。
5. **提交前自验证（100% 真实命令）**：
   - `./scripts/build.sh` 零新错误（既有良性警告可接受）
   - 跑真实命令并引用真实输出（`--list-tests` 出现新测试、`-e <newtest>` `exit: pass`、
     skip 测试要看到 skip-reason）
   - 回归：至少一个无关测试（如 zstd19）`exit: pass`
   - 检查 x86 不受影响
6. **验证过自动 push 到特性分支**（不推 main；在 main 上先 `git checkout -b <branch>`）。
   注意：commit message 不得以 `Co-Authored-By: Claude` 结尾。
7. 大改动同步更新 README.md 和 docs/ 对应文档。

## 7. 现成参考实现（按场景找模板）

| 场景 | 参考文件 |
|---|---|
| 最简单入门 | `tests/examples/simple_add.c`（不构建，仅参考） |
| SIMD + golden 比对 + test->data | `tests/examples/vector_add.c` |
| NEON 专属 + 特性探测 skip | `tests/cpu/arm64/neon_test.cpp`、`load_port/load_port.cpp` |
| SVE + HWCAP 探测 + 双精度链 | `tests/cpu/arm64/sve512_f64_chain_arm.cpp` |
| 特殊值类别比对（NaN/Inf） | `tests/cpu/arm64/fsu_byteexact_arm.cpp`、`sve512_f64_special_arm.cpp` |
| test knob（-O 旋钮） | `tests/cpu/openblas_gemm/*.cpp`（mdim/transab/beta_permille）、`pocketfft/fft.cpp`、`sleef/sleef_neon.cpp` |
| vendored 库接入 | `tests/cpu/sleef/`、`openblas_gemm/`、`isa-l/` + 对应 third-party/*/build.sh |
| 依赖系统状态的 init skip | `tests/common/smi_count/smi_count.cpp`（placeholder 模式） |
| slicing（max_threads）大内存测试 | 见 writing_tests.md「Slicing」节 |
| 拓扑感知（NUMA/die/core） | `tests/cpu/memory/memcpy_rewr.cpp`（device_info[cpu].numa_id） |
