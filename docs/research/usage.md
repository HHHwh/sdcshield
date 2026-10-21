# SDCShield 使用指南（研究整理）

> 来源：`sdcshield --help` 实际输出 + `framework/sandstone_opts.cpp` 源码核对 + 实际运行验证。
> 核对日期：2026-09-18（二进制版本 `sdcshield-06f0ef541a61`，main @ 06f0ef54）。

## 1. 构建与运行

```bash
# 首次构建（vendored 依赖已在 third-party/*/install 齐备时可跳过 build.sh）：
PKG_CONFIG_PATH=./third-party/eigen5 meson setup builddir --buildtype=release
./scripts/build.sh

# 修改 meson sources/options 之后必须 reconfigure（plain ninja 不会感知配置变化）：
PKG_CONFIG_PATH=./third-party/eigen5 meson setup --reconfigure builddir --buildtype=release
./scripts/build.sh
```

vendored 库构建（可选，缺失时对应测试优雅降级不阻断构建）：
```bash
./third-party/openssl/build.sh     # → install/lib/libcrypto.a（SSL 测试）
./third-party/openblas/build.sh    # → install/lib/libopenblas.a（openblas_*gemm/lu）
./third-party/sleef/build.sh       # → install/lib/libsleef.a（sleef_neon/sve）
./third-party/isa-l/build.sh       # → install/lib/libisal.a（isal_igzip/isal_crc*）
# pocketfft 无需预构建（头文件+源码直接编入）
```

## 2. 日常运行命令（全部实测过）

```bash
./builddir/sdcshield --list-tests                 # 列 PROD 用例（默认质量；实测 289 个）
./builddir/sdcshield -l                          # 用例 + 描述 + 分组（比 --list-tests 详细）
./builddir/sdcshield --list-groups               # 实测：@compression @ipsec @math
./builddir/sdcshield --list-test-ids             # 纯 ID 列表（脚本友好）
./builddir/sdcshield --list-group-members @math  # 列某组成员
./builddir/sdcshield -e zstd19 -t 5000           # 单测试 5 秒，全核
./builddir/sdcshield -e zstd19 -t 5000 -n 1      # 单线程（确定性，规避大核数 ULP flakiness）
./builddir/sdcshield -e "@compression" -t 10s    # 按组跑
./builddir/sdcshield -e "zstd*"                  # 通配符
./builddir/sdcshield --quality=0 -e arm64_sdc    # 跑 BETA 用例
./builddir/sdcshield --quality=-1 -e eigen_svd_jacobi   # 跑 SKIP 级用例（实测 298 个全量）
./builddir/sdcshield --dump-cpu-info             # CPU 特性 + 拓扑，然后退出
./builddir/sdcshield -s help                     # 列 RNG 引擎（Constant/LCG/AES）
./builddir/sdcshield --selftests --list-tests    # 框架自测（实测 142 个）
./builddir/sdcshield --on-crash=context -e selftest_sigsegv -vv  # 崩溃回溯 dump
./builddir/sdcshield --version                   # 实测输出 sdcshield-06f0ef541a61
```

结果判定：看最后 `exit: pass` / `exit: failure`；YAML 日志里 `result: pass`。
回归基线实测（2026-09-18）：`-e zstd19 -t 2000 -n 1` → `exit: pass`。

## 3. `--help` 里列出的选项（原文摘要）

| 选项 | 作用 |
|---|---|
| `-F, --fatal-errors` | 首个失败后停止 |
| `-T <time>, --total-time` | 总运行时长（`forever`=无限循环；s/m/h 单位） |
| `--strict-runtime` | 配合 -T，到时强制停 |
| `-t <time>` | 每测试时长（ms，可带 s/m/h） |
| `--max-test-count <N>` | 最多跑 N 个测试 |
| `--max-test-loop-count <N>` | 每测试主循环次数上限（0=不限=关闭 fracturing） |
| `--cpuset=<set>, --deviceset` | 选 CPU（裸编号或 p包裹/c核/t线程 拓扑字母） |
| `--dump-cpu-info` | 打印检测到的 CPU 信息后退出 |
| `-e <test>, --enable / --disable` | 选择性启用/禁用（test ID、通配符、@组） |
| `--ignore-os-error, --ignore-timeout` | OS 错误/超时不中断整体运行 |
| `--ignore-unknown-tests` | 忽略不认识的测试名（默认报错） |
| `-h, --help` / `--version` | 帮助 / 版本 |
| `-l, --list` / `--list-tests` / `--list-groups` | 列表类 |
| `--max-messages <N>` | 每线程每测试最大日志条数（默认 5；<=0 不限） |
| `--max-logdata <N>` | 每线程每测试最大二进制日志字节数（默认 128） |
| `-n <N>, --threads` | 线程数（0/缺省=全部 CPU） |
| `-o, --output-log <FILE>` | 日志文件（默认自动生成，全过则删除；`-o -` 输出 stdout；`/dev/null` 关闭） |
| `-s <STATE>, --rng-state` | RNG 状态（`Engine:data`，用于复现失败） |
| `-v/-vv, -q, --verbose/--quiet` | 冗长度（-vv 才有崩溃上下文和每线程频率） |
| `--1sec/--30sec/--2min/--5min` | 按优先级驱动覆盖率的定时运行 |
| `--test-list-file <file>` | 文本文件指定测试及各自时长 |
| `--test-list-randomize` | 随机化执行顺序 |
| `--test-delay <ms>` | 测试间延迟 |
| `-Y, --yaml` | YAML 日志格式 |

## 4. `--help` **未列出**但存在的选项（源码 `framework/sandstone_opts.cpp` 核对）

### 高频使用（CLAUDE.md/README 有记载）

| 选项 | 作用 | 备注 |
|---|---|---|
| `--quality=<level>` | 质量级别过滤。默认 2(PROD)；`--quality=0` 加上 BETA；`--quality=-1` 加上 SKIP | 实测计数 289/293/298 |
| `--selftests` | 运行框架自测而非产品测试 | 142 个；`@positive`/负向 selftest 配合断言退出码 |
| `--on-crash=<mode>` | 崩溃处理：`context`（回溯）等 | `-vv` 配合 |
| `--beta` / `--alpha` | 等价质量级别切换（历史兼容） | |
| `-O <testid>.<key>=<val>, --test-option` | 测试旋钮（test knob），如 `-O openblas_dgemm.mdim=1024`、`-O sleef_neon.nelems=4096`、`-O pocketfft_fft.n=4099`、`-O isal_igzip.level=2` | **必须带测试 ID 前缀**，裸 `mdim=N` 被静默忽略（TestKeyWrapper 按 `<testid>.<key>` 查找，`framework/test_knobs.cpp`） |
| `-f <mode>, --fork-mode` | fork 模式：`no`（调试用，不 fork）/ 默认非debug `fork_each_test` / debug 默认 `exec_each_test` | |
| `--on-hang=<mode>` | 挂起处理（默认 5 分钟杀；`gdb` 可自动起 gdb） | |

### 调度/运行控制

| 选项 | 作用 |
|---|---|
| `--quick` | 快速运行 |
| `--service` | 服务模式 |
| `--schedule-by=<t>` | 调度方式 |
| `--reschedule=<n>` | 重调度 |
| `--no-slicing` | 关闭 slicing（见 writing_tests.md） |
| `--max-concurrent-threads <n>` | 最大并发线程 |
| `--max-cores-per-slice <n>` | 每 slice 最大核数 |
| `--thread-ratio <r>` | 线程比例 |
| `--inject-idle <t>` | 注入空闲 |
| `--include-optional` | 跑标记了 `test_is_optional` 的测试 |
| `--triage / --no-triage` | 失败分诊（默认失败会自动重跑定位 core/thread） |
| `--retest-on-failure <n>` / `--total-retest-on-failure <n>` | 失败重跑次数 |
| `--ud-on-failure` | 失败时触发 UD |
| `--timeout <t>` / `--timeout-kill <t>` | 超时控制 |
| `--force-test-time` | 强制 -t 时长 |
| `--shorten-runtime <x>` / `--longer-runtime <x>` | 缩放运行时长 |
| `--weighted-testrun-type <t>` | 加权测试运行类型 |
| `--use-builtin-test-list[=x]` | 用内建测试列表 |
| `--mem-sample-time` / `--mem-samples-per-log` / `--no-memory-sampling` | 内存采样 |
| `--syslog` | 日志进 syslog |
| `--ulog <x>` | ulog（需 SANDSTONE_ULOG） |
| `--fatal-skips` | skip 也算致命 |
| `--ignore-mce-errors` | 忽略 MCE 错误 |
| `--temperature-threshold <t>` | 温度阈值 |
| `--vary-frequency` / `--vary-uncore-frequency` | 变频（鲲鹏 920 无 cpufreq，会打印 skipping 继续；需 SANDSTONE_FREQUENCY_MANAGER） |
| `--dump-device-info` | = `--dump-cpu-info` 别名 |
| `--is-asan-build` | 仅 ASAN 构建 |
| `--gdb-server` / `--is-debug-build` / `--test-tests` | 仅 debug 构建（`--test-tests` 用于 TEST_LOOP 粒度调参） |

## 5. 关键运行语义（源码 + 实测理解）

### 质量级别（`framework/sandstone.h` test_quality）
```
TEST_QUALITY_SKIP = -1 < TEST_QUALITY_BETA = 0 < TEST_QUALITY_PROD = 2
```
测试运行条件：`test.quality_level >= requested_quality`。默认 `--quality=2`。
SKIP 级测试仅在 `--quality=-1` 时运行（sandstone.cpp:546/559 的 SKIP-skip guard）。

### Fork 模型
每个测试在 fork 的子进程中运行：parent `run_one_test_inner` → `child_run` →
`test_init`（子进程主线程）→ 每核一 worker 线程跑 `test_run` → `test_cleanup`。
`test_preinit` 在**父进程**只跑一次（之后置 NULL，跨迭代不重跑）。
崩溃崩的是子进程，通过 CrashContext 上报（`framework/sysdeps/unix/child_debug.cpp`）。

### Fracturing（测试分裂）
默认每测试时长片（默认 1s，可 `desired_duration` 覆盖）内，框架反复以不同随机种子
重新运行同一测试（各自独立子进程）→ 这是日志里同一测试出现多次的原因。
`--max-test-loop-count=0` 关闭 fracturing；测试内 `fracture_loop_count < 0` 同理。

### RNG
引擎：Constant / LCG / AES（默认 `haveAes()` 时自动 AES）。状态**每线程独立**
（`thread_rng`，64 字节对齐）。框架**接管了 libc 的 rand/random/srand**（seed 类直接
abort），第三方库里的随机数也因此可复现。复现失败：`-s 'AES:<seed串>'`（日志
`state: { seed: ... }` 里拷贝）。

### 日志
- 默认 YAML（meson `-Dlogging_format=yaml|tap|no_output`）；`-o -` 到 stdout；`-o /dev/null` 关闭
- 全部通过 → 自动生成的日志文件被删除；`-o` 指定的不删
- 每线程每测试默认最多 5 条日志 / 128 字节二进制数据（`--max-messages 0 --max-logdata 0` 解除）
- 测试**只允许在错误路径打日志**（log_debug 除外，release 编译掉）
- stdout 直接丢弃（printf 无效）；stderr 会进日志但请用框架 log_* 函数

### Test knob（`-O`）
`-O <testid>.<key>=<value>`。现有旋钮：
- `openblas_{d,s,z,c}gemm.mdim=N`（16..4096，默认 256）+ `.transab=0..3` + `.beta_permille=0..1000000`
- `sleef_neon.nelems=N`（128..262144，默认 1024，4 的倍数）
- `pocketfft_fft.n=N`（512..16384，含质数触发 Bluestein）
- `isal_igzip.level=0..3`
- 详见 `framework/test_knobs.cpp`（TestKeyWrapper 按 `<testid>.<key>` 精确匹配）

## 6. 退出码

`exit: pass` = 0；测试失败非零；`EXIT_SKIP`(-255) 是测试内部返回值不是进程退出码。
`--ignore-os-error/--ignore-timeout` 让超时/OS 错误不终止全局。
