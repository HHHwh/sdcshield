# SDCShield 在 openEuler 24.03 (LTS-SP3) aarch64 上的构建依赖与离线构建指南

> 目标：在一个**最小安全安装**的 openEuler 24.03 SP3 aarch64 上，从零编译构建本仓库（默认 CPU device、ARM64 路径），并支持在**无网络环境**下快速复刻。
>
> 以下结论全部基于对 `meson.build` / `meson_options.txt` / 各子目录 `meson.build` 的逐文件分析，并在一台干净 openEuler 24.03 SP3 (Kunpeng 920, aarch64) 上以一次真实 `meson setup --reconfigure` + `ninja` + 功能验证（278 个 PROD 测试，实测于 2026-09-15，`zstd19` 单线程 `exit: pass`）背书。

---

## 1. 依赖总览（分层）

本项目是**纯本地编译**的 C/C++ 项目，依赖可分为四层：

| 层 | 内容 | 是否必须 |
|---|---|---|
| A. 构建工具链 | gcc / g++ / meson / ninja / perl / python3 / pkg-config / binutils | **必须** |
| B. 编译期库（头文件 + 静态/动态库） | boost（仅头文件）、eigen5（**仓库自带**，无需系统装）、zlib、libzstd、isa-l（**仓库自带** vendored，系统包仅作回退） | **必须**（eigen5 与 isa-l 除外，因仓库自带） |
| C. 可选功能库（源码自带，`build.sh` 一次构建） | openssl（third-party/openssl）、openblas（third-party/openblas）、sleef（third-party/sleef）、isa-l（third-party/isa-l）、pocketfft（third-party/pocketfft，**无需 build.sh**，头文件+源码直接编入）；gtest（仅单元测试 `unittests` 目标，默认不构建） | 可选 |
| D. 运行期 / 链接选项相关 | libatomic（aarch64 128 位原子运行时）、glibc 静态库（仅全静态链接 `-static` 时） | 视构建模式 |

关键事实（来自源码与实测）：

1. **Eigen 5.0.0+ 仓库自带**于 `third-party/eigen5/`。aarch64 路径在 `tests/cpu/meson.build` 里直接 `include_directories('../../third-party/eigen5')`，**不依赖系统 eigen3**，也**不需要**再设 `PKG_CONFIG_PATH`（该 env 仅在你想让系统 pkg-config 看到 eigen5.pc 时用，但 aarch64 构建并不查询它）。`CLAUDE.md` 顶部那段 `PKG_CONFIG_PATH=./third-party/eigen5` 是历史遗留/兼容写法，aarch64 实际构建可省略——但**带上无害**，故离线脚本里保留以兼容未来 x86 路径。
2. **无 nasm 依赖**。`meson.build` 里虽定义了 `nasm_system_flags`，但**没有任何 `find_program('nasm')` 调用**，也没有 `.asm` 源文件被编译。aarch64 上不需要 nasm/yasm。
3. **boost 只用头文件**。`framework/meson.build` 检查 `boost/algorithm/string.hpp` 与 `boost/type_traits/is_complex.hpp` 两个头；实测 `pkg-config --libs boost` 为空（header-only）。因此 `boost-devel` 足够，不需要 boost 的 `.so` 组件。
4. **isa-l 仓库自带 vendored（2026-09-16 起）**：`third-party/isa-l/`（Intel isa-l v2.32.1 tarball + `build.sh` → `install/lib/libisal.a` + `install/include/isa-l/*.h`，Makefile.unx 路径，aarch64 无需 autoconf/nasm，构建约 10 秒）。11 个 `isal_*` 测试（`isal_igzip` + 10 个 CRC）默认链接 vendored 静态归档；`install/` 缺失时回退系统 `libisal`（此时才需要系统包），两者皆无才剔除 `isal_*` 测试。openEuler 系统包两个等价：`libisa-l-devel`（EPOL 仓，2.30.0）与 `libisal-devel`（everything 仓，2.29.0），都装 `/usr/include/isa-l/*.h` + `libisal.a/.so`。
5. **libatomic**：aarch64 上 `std::atomic<__int128>`（128 位 CAS）会 lower 成对 `__atomic_*_16` 运行时符号的调用，需要 `libatomic`。meson 以 `required:false` 查找，但实际 `spinlock_stress_cmpxchg16b` 等测试会用到。`libatomic` 包默认随 gcc 装上。
6. **gtest**：仅 `unittests` 目标（`build_by_default:false`）需要，默认 `ninja` 不构建它。若不跑单元测试，**可省** `gtest-devel`。
7. **openssl**：**源码自带**（`third-party/openssl/openssl-3.5.0.tar.gz`，`./third-party/openssl/build.sh` 一次构建出 `install/lib/libcrypto.a` + `install/include/openssl/`）。自 2026-09-15 起 `ssl_link_type` 默认 `dynamic`，`openssl_sha` + ipsec 全系列测试默认构建并链接 vendored 静态归档（二进制自包含，无 libcrypto.so 运行时依赖，但 ipsec/openssl 头文件与符号来自 vendored 3.5.0）。`install/` 缺失时 meson 回退系统 `libcrypto`（此时才需要 `openssl-devel`），再找不到则禁用 SSL 测试并打印提示。
8. **openblas**：**源码自带**（`third-party/openblas/OpenBLAS-0.3.29.tar.gz`，`./third-party/openblas/build.sh` 一次构建出 `install/lib/libopenblas.a`；TARGET=TSV110、单线程 `USE_THREAD=0` + `USE_LOCKING=1`，构建约 2 分钟）。`openblas_dgemm`/`openblas_sgemm`/`openblas_zgemm` 三个测试依赖它；`install/` 缺失时 meson 打印提示、这三个测试不构建（其余不受影响）。
9. **sleef**：**源码自带**（`third-party/sleef/sleef-3.9.0.tar.gz`，`./third-party/sleef/build.sh` 一次构建出 `install/lib/libsleef.a`；TLFLOAT=OFF，构建约 15 秒，需要 cmake）。`sleef_neon`（任意 NEON 主机可跑）与 `sleef_sve`（需 SVE 硬件，无 SVE 机器干净 skip）依赖它；`install/` 缺失时同样打提示跳过。
10. **pocketfft**：**源码自带且无需预构建**（`third-party/pocketfft/pocketfft-master/` 的 `pocketfft.h` + `pocketfft.c` 直接编入测试库，BSD-3）。`pocketfft_fft` 测试开箱即得，没有 build.sh。
11. **git**：仅 `framework/scripts/make-gitid.pl` 生成 `gitid.h` 时调用 `git describe`；无 git 仓库时脚本会优雅退化（写入占位版本号），不阻断构建。但为得到正确版本号，建议装 `git`。
12. **gcc 版本**：`cpp_std=gnu++23`。实测 **gcc 12.3.1**（openEuler 24.03 SP3 自带）即可编译通过，无需 gcc 13+。
13. **meson 版本**：要求 `>=1.3`。openEuler 官方 `meson-1.3.1` 满足。（注：当前开发机 `meson --version` 显示 1.11.2 来自用户 `pip` 安装 `~/.local`，**不是**系统包；离线最小化环境应使用系统 RPM 的 1.3.1，已足够。）
14. **vendored 依赖库的构建顺序**：首次 `meson setup` 前依次跑四个 `build.sh`（openssl → openblas → sleef → isa-l，顺序无关但都幂等；pocketfft 无需预构建）。任一 `install/` 缺失时 meson 只打 message 提示并跳过对应测试组或回退系统库（isa-l），**不阻断构建**——`--list-tests` 数量会相应减少。库选型依据（向量 FMA GEMM > 哈希/加密 > 压缩 > SVE 超越函数）见 `docs/paper/SDC_RESEARCH_SYNTHESIS_CN.md`（31 篇 SDC 文献综合）。

---

## 2. 最小软件包清单（openEuler 24.03 SP3 aarch64，默认 CPU 构建）

### 2.1 必装包（离线构建核心集）

```text
gcc                         # 12.3.1 — C 编译器
gcc-c++                     # 12.3.1 — C++ 编译器（自动拉入 libstdc++-devel）
meson                       # 1.3.1   — 构建系统（>=1.3，满足 meson_version 要求）
ninja-build                 # 1.11.1  — 后端构建器
perl                        # 5.38.0  — 跑 generate-short-ids.pl / make-gitid.pl / armsimd_generate.pl
python3                     # 3.11.6  — 跑 generate_test_list.py
pkgconf                     # 1.9.5   — pkg-config，查 zlib/zstd/openssl/boost
binutils                    # 2.41    — ar/objcopy（仅 -static 路径用到，但 ld.bfd 是推荐链接器）
boost-devel                 # 1.83.0  — 仅头文件依赖
zlib-devel                  # 1.2.13  — zlib 测试
zstd-devel                  # 1.5.5   — zstd19 测试
libatomic                   # 12.3.1  — aarch64 128 位原子运行时
git                         # 2.43.0  — 生成 gitid.h 版本号（无则退化，非阻断）
```

### 2.2 可选包

```text
openssl-devel               # 3.0.12  — 系统包仅作回退：vendored OpenSSL 默认启用
                                      #   （third-party/openssl，build.sh 一次构建；
                                      #    install/ 缺失时 meson 回退系统 libcrypto）
libisa-l-devel              # 2.30.0  — 系统包仅作回退：vendored isa-l 默认启用
                                      #   （third-party/isa-l，build.sh 一次构建；
                                      #    install/ 缺失时 meson 回退系统 libisal，
                                      #    此时才需要本包。EPOL 仓；或用 everything
                                      #    仓的 libisal-devel 2.29.0）
cmake                       # 3.27+   — 仅 vendored SLEEF 的 build.sh 需要
                                      #   （不构建 sleef 测试则可省）
gtest-devel                 # 1.14.0  — 仅构建 unittests 目标时
glibc-static                # 全静态链接(-static)时需要 libc.a（默认动态链接不需要）
```

> 注：openEuler 24.03 SP3 的 `glibc-devel` 已包含 `/usr/lib64/libc.a`，`-static-libstdc++`（`-Dstatic_libstdcpp=true`）需要的 `libstdc++.a` 由 `libstdc++-devel`（随 gcc-c++ 拉入）提供。仅当要**全静态**链接整个二进制（`-Ddependency_link=static` + `cpp_link_args=-static`）时才需要额外的 `glibc-static`。

---

## 3. 离线构建操作流程

### 3.1 在一台有网的 openEuler 24.03 SP3 上下载 RPM

```bash
# 1. 创建离线包目录
mkdir -p ~/sdcshield-offline && cd ~/sdcshield-offline

# 2. 下载必装包及其完整依赖树
#    用 `dnf download` 子命令（不是 `dnf install --downloadonly`）：
#    --resolve 解析依赖, --alldeps 连同本机已满足的依赖也一并下载（对最小化
#    目标机是必需的, 否则依赖不全）。
dnf download --resolve --alldeps --destdir=$PWD \
    gcc gcc-c++ meson ninja-build perl python3 pkgconf binutils \
    boost-devel zlib-devel zstd-devel libatomic git

# 3.（可选）下载 OpenSSL/gtest/isa-l 系统回退包
dnf download --resolve --alldeps --destdir=$PWD openssl-devel gtest-devel libisa-l-devel
```

> `dnf download` 子命令由 `dnf-plugins-core` 提供。openEuler 24.03 SP3 最小安装默认不含它，需先在有网机上 `dnf install -y dnf-plugins-core`。
>
> `libisa-l-devel` 在 **EPOL** 仓。若最小安装的机器未启用 EPOL，需先 `sudo dnf config-manager --set-enabled EPOL`（或在有网机上下载时确保 EPOL 已启用，RPM 拷过去即可直接 `rpm -ivh`，不依赖仓库元数据）。
>
> 若 EPOL 不可用，改用 `libisal-devel`（everything 仓，2.29.0），头文件路径一致。

### 3.2 在无网络的目标机上安装

```bash
# 拷贝整个目录到目标机后：
cd ~/sdcshield-offline
# 直接用 install-deps.sh（它会在 shell 层面过滤掉受保护/无关系统包）：
./scripts/offline-build/install-deps.sh .
# 或手动等价命令（先过滤出构建必需的 RPM，再传给 dnf）：
EXCLUDE_RE='^(grub2|grubby|shim|mokutil|efivar|efibootmgr|os-prober|dracut|kpartx|device-mapper|fuse|glibc|glibc-common|glibc-devel|glibc-headers|systemd|systemd-libs|systemd-udev|setup|filesystem|basesystem|shadow|pam|crypto-policies|openEuler-release|openEuler-gpg-keys|openEuler-repos)'
sudo dnf install -y --disablerepo=* \
    $(find . -maxdepth 1 -name '*.rpm' -printf '%p\n' | grep -ivE "$EXCLUDE_RE" | sort)
# 或纯 rpm 方式（需手动保证顺序）：
# sudo rpm -Uvh --nodeps ./*.rpm
```

### 3.3 构建本项目

```bash
cd /path/to/sdcshield-arm-test2

# 首次构建前：依次构建 vendored 依赖库（幂等，install/ 已存在则秒过；
# pocketfft 无需此步）。任一步跳过 → meson 打提示、对应测试不构建
# （isa-l 则回退系统 libisal）。
./third-party/openssl/build.sh     # ~30s：install/lib/libcrypto.a
./third-party/openblas/build.sh    # ~2min：install/lib/libopenblas.a
./third-party/sleef/build.sh       # ~15s：install/lib/libsleef.a（需 cmake）
./third-party/isa-l/build.sh       # ~10s：install/lib/libisal.a

# aarch64 路径：eigen5 仓库自带，无需系统 eigen3
# PKG_CONFIG_PATH 仅为兼容 x86 路径而保留，aarch64 可省
meson setup builddir --buildtype=release
ninja -C builddir

# 验证（278 = vendored 依赖齐备时的 PROD 实测值，2026-09-15；
# 缺某个 install/ 时数量相应减少，meson 配置日志有对应 message）
./builddir/sdcshield --list-tests          # 应列出 278 个测试
./builddir/sdcshield -e zstd19 -t 2000 -n 1 # 应 exit: pass
```

### 3.4 OpenSSL SSL 测试（默认启用，vendored）

```bash
# 一次构建 vendored OpenSSL（若 install/ 已存在则直接跳过）：
./third-party/openssl/build.sh

# 默认构建即包含 SSL 测试（ssl_link_type 默认 dynamic，优先 vendored 静态归档）：
meson setup builddir --buildtype=release
ninja -C builddir
./builddir/sdcshield --list-tests | grep openssl_sha    # 1 行
./builddir/sdcshield --list-tests | grep -c ipsec       # 46 行
```

---

## 4. 依赖→包→文件 三对应表（便于排障）

| 源码里的依赖查询 | pkg-config / find_library 名 | openEuler 包 | 提供的关键文件 |
|---|---|---|---|
| `dependency('boost')` + `check_header('boost/algorithm/string.hpp')` | `boost` | `boost-devel` | `/usr/include/boost/algorithm/string.hpp` |
| `dependency('eigen3')`（aarch64 改用自带） | `eigen3` | **仓库自带** `third-party/eigen5/` | `third-party/eigen5/Eigen/*` |
| `dependency('zlib')` | `zlib` | `zlib-devel` | `/usr/include/zlib.h`, `/usr/lib64/libz.{a,so}` |
| `dependency('libzstd')` | `libzstd` | `zstd-devel` | `/usr/include/zstd.h`, `/usr/lib64/libzstd.{a,so}` |
| vendored isa-l（默认优先） | `test -f third-party/isa-l/install/lib/libisal.a` | **仓库自带** `third-party/isa-l/`（build.sh） | `install/lib/libisal.a`, `install/include/isa-l/*.h` |
| `cpp.find_library('isal', static:true)`（仅回退，vendored install/ 缺失时） | （无 .pc，直接 find_library） | `libisa-l-devel` 或 `libisal-devel` | `/usr/include/isa-l/*.h`, `/usr/lib64/libisal.{a,so}` |
| `cc.find_library('atomic')` | （无 .pc） | `libatomic` | `/usr/lib64/libatomic.so.1`（静态 `libatomic.a` 随 `libatomic` 装） |
| `cc.find_library('dl')` | — | glibc 自带 | `/usr/lib64/libdl.so` |
| `dependency('threads')` | — | glibc 自带 | libpthread |
| vendored libcrypto（默认优先） | `test -f third-party/openssl/install/lib/libcrypto.a` | **仓库自带** `third-party/openssl/`（build.sh） | `install/lib/libcrypto.a`, `install/include/openssl/sha.h` |
| vendored OpenBLAS | `test -f third-party/openblas/install/lib/libopenblas.a` | **仓库自带** `third-party/openblas/`（build.sh） | `install/lib/libopenblas.a`, `install/include/cblas.h` |
| vendored SLEEF | `test -f third-party/sleef/install/lib/libsleef.a` | **仓库自带** `third-party/sleef/`（build.sh） | `install/lib/libsleef.a`, `install/include/sleef.h` |
| vendored pocketfft | `test -f third-party/pocketfft/pocketfft-master/pocketfft.h` | **仓库自带** `third-party/pocketfft/`（无需预构建） | `pocketfft-master/pocketfft.{h,c}` |
| `dependency('libcrypto')`（仅回退，install/ 缺失时） | `libcrypto` | `openssl-devel` | `/usr/include/openssl/sha.h`, `/usr/lib64/libcrypto.{a,so}` |
| `dependency('gtest_main')`（可选） | `gtest_main` | `gtest-devel` | `/usr/lib64/libgtest*.a`, `.pc` |
| `find_program('perl')` | — | `perl` | `/usr/bin/perl` |
| `find_program('python3')` | — | `python3` | `/usr/bin/python3` |
| `find_program('bash')` / `('sh')` | — | glibc-minimal 自带 | `/usr/bin/bash`, `/usr/bin/sh` |
| `make-gitid.pl` 调 `git describe` | — | `git`（可选，退化容错） | `/usr/bin/git` |

---

## 5. 常见离线构建坑点

1. **isal 找不到**：先跑 `./third-party/isa-l/build.sh`（离线场景仓库自带 tarball，无需网络），meson 配置日志应出现 `Using vendored isa-l`。若既无 vendored install/ 又无系统包，症状：`Library isal found: NO` + `No vendored or system libisal found` 提示，11 个 `isal_*` 测试被静默剔除（构建仍成功，但测试数变少）。此时才需要装系统包：`libisa-l-devel`（EPOL）或 `libisal-devel`（everything）。
2. **vendored 库 install/ 缺失**：症状：meson 配置日志出现 `OpenBLAS install/ not found ... run its build.sh` / `SLEEF install/ not found ...` / `pocketfft.h not found ...`（isa-l 则是回退系统库或上述提示），`openblas_*`/`sleef_*`/`pocketfft_fft`/`isal_*` 测试不构建或回退，`--list-tests` 少于 278。解决：跑对应的 `third-party/*/build.sh`（pocketfft 则确认 tarball 已解压出 `pocketfft-master/`——仓库已直接提交这三个文件，正常 clone 即有）。
3. **eigen 版本错**：若误装系统 `eigen3-devel`（Eigen 3.3.x），aarch64 + GCC 12 会因 `deprecated-enum-enum` 转换在 `-Wextra` 下变硬错误而编译失败。**务必用仓库自带的 `third-party/eigen5`**；aarch64 路径已硬编码指向它，勿覆盖。
4. **meson 版本低**：openEuler 自带 `meson-1.3.1` 恰好满足 `>=1.3`；但若目标机装了更老的 meson（如 22.03 LTS 带的），需升级。离线包里带的 1.3.1 RPM 可直接用。
5. **EPOL 仓未启用**：`libisa-l-devel` 只在 EPOL。**默认不需要它**（vendored isa-l 优先）；仅当走系统回退路径时，离线场景把 RPM 拷过去 `rpm -ivh` 即可绕过仓库检查，或改用 everything 仓的 `libisal-devel`，或直接跑 `./third-party/isa-l/build.sh`。
6. **无 git**：无 git 仓库或无 git 命令时，`gitid.h` 仍会生成（脚本 fallback 到占位串），构建不阻断，只是版本号是占位。
7. **磁盘空间**：完整构建（含 SVE/NEON 多后端）产物约 256 MB 单二进制 + 中间 `.a`，建议 builddir 所在盘预留 ≥ 2 GB。
8. **离线安装报"删除受保护包 grub2-efi-aa64"**：`dnf download --resolve --alldeps` 会把整棵依赖树拉全，其中混入 bootloader/固件（`grub2-*`、`shim`、`mokutil`、`efivar`、`efibootmgr`）、initramfs 链（`dracut`、`os-prober`、`kpartx`、`device-mapper`、`fuse`）及系统核心（`glibc`、`systemd`、`pam`、`setup`、`filesystem`、`basesystem`、`shadow`、`openEuler-release` 等）。它们在 openEuler 上受 dnf `protected_packages` 保护，离线 `dnf install ./*.rpm` 时版本若有细微差异，dnf 会视"升级"为"删除受保护包"而拒绝安装。SDCShield 构建完全不依赖这些包。
   **关键坑**：`dnf install --exclude=grub2* ./*.rpm` **无效**——`--exclude` 对命令行显式指定的本地 `.rpm` 文件参数不生效，dnf 会把匹配的 `.rpm` 从候选移除后仍为这些"参数"去仓库找匹配，在 `--disablerepo=*` 下报 `No match for argument: grub2-...rpm`。正确做法是**在 shell 层面过滤文件列表**，只把构建必需的 `.rpm` 传给 dnf。`install-deps.sh` 已内置：按前缀静态排除受保护/无关系统包（`EXCLUDE_RE`），再跳过目标机已装同版本包（`skip_if_installed`），最后拓扑排序（见坑点 8）后传给 dnf。

9. **OS 版本管控 + 依赖拓扑排序**：离线构建的根因性坑是**下载机与目标机 openEuler 版本不一致**（如 SP3 下载、SP4 目标）。这会引发两类无法兜底的冲突：降级冲突（`audit-libs`/`openssl-libs`/`rpm` 等旧版替换新版）和 `glibc-devel` 精确版本依赖死结（SP3 `glibc-devel` `Requires: glibc = 2.38-84.sp3`，装到 SP4 要求降级受保护的 glibc，不装则 gcc 缺依赖）。
   - **严格版本匹配（正解）**：`download-deps.sh` 检测下载机 OS 版本，在输出目录写 `.os-version` 标记文件（内容如 `openEuler-24.03LTS_SP3`）；`install-deps.sh` 读标记并与目标机版本严格比对，**不一致则拒绝安装**并指引到同版本机重下。版本一致时 SPx glibc-devel 对应 SPx glibc，精确版本依赖自然满足，死结消失。三个脚本共享 `scripts/offline-build/_common.sh` 的 `detect_os_sp`/`detect_os_version_full`/`require_openeuler`。
   - **依赖拓扑排序**：`install-deps.sh` 对每个候选 RPM 用 `rpm -qp --provides`/`--requires` 提取依赖符号，建依赖图（若 B 的 require 由候选 RPM A 提供且未被目标机已装包满足，则建边 A→B），`tsort` 拓扑排序后按序传给 dnf。这保证 `libgcc`/`glibc-devel`/`gmp`/`mpfr`/`binutils` 等基础层先于 `gcc` 安装，避免"依赖未就绪"。目标机已装包提供的符号不建边（否则会把系统已满足的依赖当卡点）。版本匹配前提下，排序后整批传 dnf 一次装通；dnf 内部也会再排序，脚本层的显式排序提供确定性 + 可读性。
   - 兜底：dnf 仍失败时，按脚本提示的 `--allowerasing` 或 `rpm -Uvh --nodeps --force` 逐个强装（仅版本不匹配且无法重下时）。

---

## 6. 一键离线脚本（可在目标机直接跑）

见仓库 `scripts/offline-build/` 下的：
- `_common.sh` — 共享的 openEuler 版本检测逻辑（被下面三个脚本 source）
- `download-deps.sh` — 在有网机上拉取全部 RPM（含 OS 版本标记写入）
- `install-deps.sh` — 在无网机上离线安装（含版本核对 + 依赖拓扑排序）
- `build.sh` — 标准构建 + 验证（含版本检测）

（如该目录不存在，按本文件第 3 节手写即可，步骤已最小化。）
