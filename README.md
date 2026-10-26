# SDCShield

SDCShield is an open-source project designed to identify defects and bugs in ARM CPUs derived from Intel's OpenDCDiag (ARM64 port). It consists of a set of tests built around a sophisticated CPU testing framework.

## Origin

SDCShield is derived from OpenDCDiag (which contains the Intel `sandstone` framework), licensed under the [Apache License 2.0](LICENSE). See [NOTICE](NOTICE) for full derivation and upstream provenance.

## 一行命令检测鲲鹏 CPU SDC

**源码树（推荐）**：

```bash
bash scripts/run/sdc_detect.sh
```

**预构建二进制**（需 2026-10-09 之后由 `scripts/offline-build/package-built-artifacts.sh` 打包、随包 `sdc_detect.sh` 的版本，`detect` 子命令自动调用它；CI tarball 与旧包未含——用下方等效命令。获取与进入目录见下节「直接运行预构建二进制」）：

```bash
./run-sdcshield.sh detect
```

**冒烟自检**（约 35 秒，验证链路与优先序）：`bash scripts/run/sdc_detect.sh --smoke`

**脚本使用方法**：`bash scripts/run/sdc_detect.sh [--smoke] [透传参数…]`（`-h`/`--help` 看完整帮助）。

| 项 | 说明 |
|---|---|
| `--smoke` | 冒烟自检：每桶前 2 用例各 2 秒的单遍运行（约 35 秒），验证链路与优先序 |
| 透传参数 | 其余参数原样交给 sdcshield（如 `-t 30s`、`--cpuset 0-47`） |
| `SDC_BIN` 环境变量 | 显式指定二进制；默认依次发现 `./builddir/sdcshield`、`./builddir-gcc/sdcshield`，找不到即报错退出并给出构建指引 |
| 产物 | 当前目录留下 `sdc-detect-testlist-*.txt`（本次优先序名单，可复现）与 `sdc-detect-*.yaml`（日志） |
| 停止 | Ctrl-C 干净停止；检出 FAIL 自动停（`-F`） |

**优先序机制**：脚本从当前二进制动态生成优先序测试列表（框架 `--test-list-file`，顺序即执行序，对新用例自适应），先跑 SDC 敏感度最高的负载：

| 优先级 | 负载域 | 依据 |
|---|---|---|
| P1 | 向量 FMA/矩阵（GEMM/SVD/SLEEF/FFT/sve512-f 链） | SEVI(ASPLOS'26)：>92% SDC 事故由 FMA 指令贡献 |
| P2 | ARM64 SDC 专项（LSU/OoO/L2 跨行/di-t/sve512 上下文） | 本仓库为已知鲲鹏故障签名定向编写 |
| P3 | memcpy/store→reload 访存通路 | CPU179 实证触发判别条件 |
| P4 | 位扩散（压缩/哈希/CRC/加密） | From Gates to SDCs(DATE'25)：sha 类位掩蔽最少 |
| P5 | 标量浮点/大整数 | — |
| P6 | 一致性/原子/SIMD | 控制流类故障多 crash 化，SDC 倾向低 |
| P7 | 其余（字母序） | 完整多样性保留 |

**命令逐项解释**（脚本已内建，无需手输；其余参数原样透传，如 `-t 30s`、`--cpuset`）：

| 旗标 | 作用 |
|---|---|
| `--quality=0` | 跑 PROD+BETA 全部 446 用例（源码构建口径；预构建二进制为 225 例、一轮约 3.75 小时——少的是 vendored 计算库测试组，见下节口径说明）。**必须加**：BETA 级 `arm64_sdc`（di/dt 电压骤降专项）等 6 个用例不加此旗标会被跳过（实测 skip 并提示） |
| `--test-list-file` | 优先序测试列表：由脚本从当前二进制动态生成（顺序即执行序，对新用例自适应） |
| `-T forever` | 整套用例无限轮转。SDC 故障窗口极稀疏（CPU179 实测占空比 ~0.06%），长驻留是检出关键；一轮 ≈ 7 小时 |
| `-t 60s` | 每用例 60 秒。SEVI 实测 >80% 的首错在 10 秒内出现，60s 有 6 倍余量 |
| `-Y` | 结构化 YAML 日志（取证格式） |
| `-F` | 首个 FAIL 即停——停下就是信号。SKIP 不算失败，占位用例不会误停 |
| `-o sdc-detect-*.yaml` | 日志落盘为固定文件名（便于归档取证）。不带 `-o` 时框架会在当前目录自动生成 `sdcshield-<UTC时间戳>.yaml`，文件名不可预测；且全过退出（exit: pass）时该文件被自动删除，仅失败/中断时留存 |

**等效命令（无脚本时；注意：默认注册序无优先级）**——不带 `--test-list-file` 时框架按默认注册序执行，SDC 敏感负载不会先跑：

```bash
# 源码构建
./builddir/sdcshield --quality=0 -T forever -t 60s -Y -F -o sdc-$(date +%m%d-%H%M).yaml
# 预构建二进制（旧 tarball；clone 后进入对应 SP 的 built 目录）
./run-sdcshield.sh --quality=0 -T forever -t 60s -Y -F -o sdc-$(date +%m%d-%H%M).yaml
```

**为什么是这条命令**：0102单板故障核的实证——优先序全套件全核轮转约 100 秒出首错，而单测试单跑（含 11180 个 seed 轮换）零复现。全核并发 + 负载多样性 + 长驻留三者齐备才有检出率，这条命令一次配齐。

**结果判读**：

1. **一直跑不停** = 本轮全过；`-T forever` 自动进入下一轮，继续挂着（检出率随时间积累）。
2. **停下且有 FAIL**：先看挂在哪个测试——
   - 挂在 `eigen_svd_double` / `eigen_sparse`：大核数机器全核多线程下的已知 ULP 级良性抖动。用 `-e <测试名> -t 5000 -n 1` 单线程复跑（源码树 `./builddir/sdcshield`、预构建 `./run-sdcshield.sh`）：**单线程过 = 良性**（假阳性），单线程也挂 = 真嫌疑。
   - 挂在其他任何测试：**SDC 嫌疑**。保留 `-o` 落盘的 YAML 日志（含 seed/迭代号，可复现），进入定位流程。
3. **停止方式**：Ctrl-C 干净停止（当前测试跑完边界即停，日志完整落盘，`exit: interrupted`）。
4. **检出后的定位**：`bash scripts/run/run_sdc_campaign.sh`（24h 战役，自动失败分类 known_benign_ulp / sdc_suspect + 可复现嫌疑自动逐核二分定位）。

## 一键最大程度激发 SDC（7×24 战役总入口）

```bash
sudo bash scripts/sdc-excite-reproduce/excite.sh          # 一键上线 7×24 战役
bash  scripts/sdc-excite-reproduce/excite.sh --smoke      # 只预检+冒烟（无需 root）
bash  scripts/sdc-excite-reproduce/excite.sh --status     # 只读巡检
sudo bash scripts/sdc-excite-reproduce/excite.sh --stop   # 停止（保留断点）
```

**更激进（单模式深驻留）**：给 sdcshield 追加 `--max-test-loop-count=10`（自动模式默认40，你可以根据需求控制种子随机生成的频次）

```bash
./builddir/sdcshield --quality=0 -T forever -t 60s --max-test-loop-count=10 -Y -F -o sdc-$(date +%m%d-%H%M).yaml
```

**使用方法要点**（`excite.sh -h` 看完整帮助）：`--smoke` 模式无需 root、不触碰 systemd（预检 + 战役冒烟门）；run 模式需 root（sudo），幂等可重入——已安装/已活跃的服务自动跳过，`state.json` 断点保留，重复执行安全。环境变量：`SDC_BIN` 指定 sdcshield 二进制；`SMOKE_TIMEOUT_S` 覆盖战役冒烟门上限（默认 2400 秒；冒烟为 L1-L5 短时长单遍，实测约 23 分钟，L5 专项每次轮换一支分支——慢板机器可调大）。

一条命令完成：预检（二进制 fail-loud + L0 冒烟门）→ 按需安装 systemd 单元 →
先起 sdc-monitor 联锁眼睛 → 战役冒烟门（不过不上线）→ 上线 L1-L5 状态机战役
（冷机代表集 → 谱系扫档 → 多样性轮转 → 专项轮换 di/dt·热·NUMA → 深驻留，
事件取证 + 定向复测，断点续跑）。M2 事件栈（root-helper/eventd/controller/ring）
为单板相关人工步骤，按 [运维手册](docs/sdc-excite-reproduce/operations-runbook.md) 拉起。

**四工具路由**：

| 场景 | 命令 |
|---|---|
| 快速检测（检出即停） | `bash scripts/run/sdc_detect.sh` |
| 逐核定位扫描（core×test 矩阵 + 失败确认复测） | `bash scripts/run/sdc-run.sh scan [--cores all\|0-31\|…] [--tests …] [--t 10s] [--par 32]` |
| 24h 自包含定位（自动失败分类 + 逐核二分） | `bash scripts/run/run_sdc_campaign.sh` |
| 7×24 最大激发（工程化战役 + 事件闭环） | `sudo bash scripts/sdc-excite-reproduce/excite.sh` |

## 快速开始

两条入口，按需选一。

### 直接运行预构建二进制（不想编译）

`third-party/rpms/` 聚合了三个 git 子模块，按 openEuler 大版本分仓，覆盖各 LTS/SP 版本的预构建二进制：

| 子模块目录 | 覆盖版本 |
|---|---|
| [`third-party/rpms/openEuler-20.03/`](https://github.com/wangxumarshall/sdcshield-rpm-20.03.git) | 20.03 LTS / SP1–SP4 |
| [`third-party/rpms/openEuler-22.03/`](https://github.com/wangxumarshall/sdcshield-rpm-22.03.git) | 22.03 LTS / SP1–SP4 |
| [`third-party/rpms/openEuler-24.03/`](https://github.com/wangxumarshall/sdcshield-rpm-24.03.git) | 24.03 LTS / SP1–SP4（SP3 = 基准版本） |

预构建二进制（git 08c37be3，2026-09-12 构建）含 221 个 PROD 用例——少于源码构建（440）是因为镜像内未含全部 vendored 计算库测试组；检出命令见上节"一行命令"。

```bash
git clone --recurse-submodules <repo-url>   # 含子模块
cd third-party/rpms/openEuler-24.03/openEuler-24.03LTS_SP3/built
./run-sdcshield.sh --list-tests            # 自动设 LD_LIBRARY_PATH=./libs
./run-sdcshield.sh --quality=0 -T forever -t 60s -Y -F                # 首次检测到SDC后，停止
./run-sdcshield.sh --quality=0 -T forever -t 60s -Y -ignore-timeout   # 一起跑，就算检测到SDC后，也一直往后跑
./run-sdcshield.sh -T forever -t 600s -Y -e "eigen_svd*" -e "eigen_gemm*" -e "fma*" -e "zstd*" -e "zlib*" # 定向子集，检出率低于全套件轮转
./run-sdcshield.sh -t 60s -n 1 -e zstd19 # 单线程，规避大核数 ULP 数值 flakiness
```

检测命令的完整解释与结果判读见「一行命令检测鲲鹏 CPU SDC」节（`--quality=0` 为何必须加亦见该节）。

> **SP 必须与目标机一致**：SP3 的 `glibc-devel` 携带 `Requires: glibc = <sp3-N>`，装到 SP4 会触发受保护 `glibc` 降级死结。`install-deps.sh` 通过 `.os-version` 标记在安装前拦截错配。

**从 CI 下载预构建二进制（无需 clone 子模块）**：每次 [Multi-OS Verify](.github/workflows/multi-os-verify.yml) CI 运行（每日 cron + 手动触发）都为 15 个 SP 各产出一个自包含 tarball（`built-<series>-<sp>` artifact，保留 90 天）——仓库 → Actions → 任意运行 → Artifacts 下载，或 `gh run download <run-id> --name built-24.03-SP3`。解包后 `./run-sdcshield.sh` 用法同上。要**永久保存**某次构建：手动触发时勾选 `publish_release`，15 个 tarball 会作为 GitHub Release 资产上传（详见 [docs/build-deploy-pr-ci/multi-version-build-deploy.md §6.2.1](docs/build-deploy-pr-ci/multi-version-build-deploy.md)）。

### 从源码构建

```bash
# openEuler 24.03（基准平台）依赖；Ubuntu/Fedora 见 docs/build-deploy-pr-ci/offline-build-dependencies.md
sudo dnf install -y meson ninja-build gcc g++ cmake boost-devel zlib-devel libzstd-devel libisa-l-devel gtest-devel
# 首次构建前：依次构建 5 个 vendored 依赖库（幂等，install/ 已存在则秒过；
# pocketfft 无需此步——头文件+源码直接编入测试库）
./third-party/openssl/build.sh           # → install/lib/libcrypto.a（SSL 测试默认启用）
./third-party/openblas/build.sh          # → install/lib/libopenblas.a（openblas_{d,s,z,c}gemm + openblas_lu）
./third-party/sleef/build.sh             # → install/lib/libsleef.a（sleef_neon / sleef_sve）
./third-party/isa-l/build.sh             # → install/lib/libisal.a（isal_igzip + isal_crc*；缺省回退系统 libisal）
./third-party/acl/build.sh               # → install/lib/libarm_compute-core.a（acl_gemm NEGEMM + fisttp_arm；NEON runtime core 目标，v23.02）
PKG_CONFIG_PATH=./third-party/eigen5 meson setup builddir --buildtype=release
./scripts/build.sh
./builddir/sdcshield --list-tests        # 应列出 440 个 PROD 用例（gcc + 全部 vendored 依赖，实测于 2026-10-12）
```

> 构建统一走 `./scripts/build.sh`：默认 `-j` = `nproc`/2（共享服务器保护；ninja 上游默认是核数+2）。显式 `-j` 或 `SDC_BUILD_JOBS` 可覆盖，详见 `scripts/build.sh` 头注释。
>
> ARM64 要求 Eigen 5.0.0+（系统 Eigen 3.3.x 在 GCC 12+ 下编译失败）。仓库自带 `third-party/eigen5/`，aarch64 构建路径在 `tests/cpu/meson.build` 中直接 `include_directories` 指向它，**无需系统安装 eigen3**；`PKG_CONFIG_PATH` 仅为兼容 x86 路径而保留，带上无害。
>
> 五个 `build.sh` 均为可选步骤：任一 `install/` 缺失时 meson 打印提示并跳过对应测试组或回退系统库（不阻断构建、不影响其余用例；isa-l 缺失时回退系统 `libisal`，两者皆无才剔除 `isal_*` 测试；acl 缺失时 `acl_gemm`/`fisttp_arm` 两个 ACL 库测试不构建，SVE 变体为 svmla 重实现、不受影响）。vendored 库的选型依据（向量 FMA GEMM > 哈希/加密 > 压缩 > SVE 超越函数）见 [docs/paper/SDC_RESEARCH_for_SDCShield-负载设计.md](docs/paper/SDC_RESEARCH_for_SDCShield-负载设计.md)（31 篇 SDC 文献综合）。
>
> **clang/BiSheng 工具链（HPC 节点路径）**：GCC 缺 C++ 或需要 SME 用例时，可用 BiSheng clang 5.2.0
> （LLVM 19.1.7，HPCKit）构建：`source <HPCKit>/setvars.sh`、`export TMPDIR=/tmp` 后 meson/ninja
> 照常。clang 额外启用 SME 测试 `sme_za_tileview_arm`（独立 `tests_arm64_sme` 静态库，
> `-march=armv9-a+sve+sme+sme-f64f64`；meson `has_argument` 探测编译器支持，gcc 无该支持时
> 打印提示并剔除，不阻断构建）。运行期以 HWCAP2_SME + SVL 几何双门控，不满足则诚实 SKIP；
> 运行需 HPCKit 环境（libc++/libc++abi 动态库须在 LD_LIBRARY_PATH）。cn23154（HiSilicon 0xd22，
> 608 核）实测 2026-09-28：默认 quality 320 用例（该节点 vendored 依赖可用集下的实测值），
> `zstd19 -t 2000 -n 1` 回归 pass。

## 离线与多版本构建

离线工具在 `scripts/offline-build/`。单机原生路径（`download-deps.sh`/`install-deps.sh`/`build.sh`，共享 `_common.sh`，用 `.os-version` 标记强制版本匹配）只支持 **24.03**——`_common.sh` 基线硬编码 `24.03LTS`，`detect_os_version_full` 对 22.03/20.03 会误报。22.03/20.03 必须走容器路径（D）。

```bash
cd scripts/offline-build/
# A. 下载 RPM 树（有网机，与目标机同 SP）
./download-deps.sh                     # 当前 SP 一版 → ./sdcshield-rpms/
./download-all-versions.sh             # 全 LTS/SP，对 24.03 SP3 基准取同名包集交集
./supplement-20.03-gcc10.sh all         # 20.03 专用：补 gcc-10 工具集 + meson 0.59（20.03 自带 gcc-7）
# B. 离线装依赖（目标机，无网）→ 传对应 SP 的 RPM 目录，版本会被核对
./install-deps.sh ../../third-party/rpms/openEuler-24.03/openEuler-24.03LTS_SP3
# C. 目标机原生构建（依赖已装，仅 24.03）
./build.sh                              # meson + ninja + 冒烟（zstd19 -n 1）
# D. 按版本容器构建（免主机安装；源码只读挂载）—— 22.03/20.03 必走此路
./container-build.sh 24.03 SP3          # → build-out/openEuler-24.03LTS_SP3/sdcshield
./container-build.sh 22.03 SP3          # 注入 -DOPENEULER_22_03 + C++23 polyfill
./container-build.sh 20.03 SP4          # gcc-toolset-10 + 仓内 vendored meson 0.59.4
# E. 打包到对应版本 built/ 目录
./package-built-artifacts.sh 24.03 SP3
# F. 纯净容器验证（仅挂 built/，验证可直接运行）
./verify-built-pristine.sh 24.03 SP3        # 全量；eigen 类 -n 1
./verify-built-pristine.sh 24.03 SP3 smoke  # 快：--list-tests + zstd19
# G. 构建容器内跑全量用例
./run-full-tests.sh 24.03 SP3 [build]
```

> **22.03 / 20.03 适配仅在容器内进行**：旧工具链（gcc-10、meson 0.59/0.54、binutils 2.34）缺 C++20/23 特性。`container-build.sh` 只读挂载源码树，在可丢弃副本上注入 `framework/compat/cpp23_polyfill.h`、版本宏（`OPENEULER_22_03`/`OPENEULER_20_03`）和 `-D` meson option（`-Denable_acl=disabled`）——host 源码不动，x86/24.03 参考路径不受影响。历史上的 `sed` 补丁（`string::contains`→`.find()`、`udf`→`.inst`、ACL 置空、`meson_version` 放宽）**已全部收敛到源码/meson option**，容器内对源码副本零修改，只剩 CXXFLAGS/polyfill 注入。

完整依赖与排坑见 [docs/build-deploy-pr-ci/offline-build-dependencies.md](docs/build-deploy-pr-ci/offline-build-dependencies.md)。

### 多版本一键构建与部署（15 个 OS 版本）

单 SP 手动链（上节 D→E→F）由 `build-all.sh` 自动化封装（镜像烘焙、哈希 skip、`-P` 并行、`--since`、smoke/full 五档）：`./scripts/offline-build/build-all.sh --all --full --jobs 3` 一次构建全 15 个版本（20.03/22.03/24.03 × LTS+SP1~SP4），以纯净容器 full 验证为发布闸门（期望 `PASS: 15`）；`./scripts/offline-build/package-release.sh <series> <sp>` 产出 ~6MB 现场 tarball，内含自动检测 OS 的 `run.sh`（精确匹配版本、校验 sha256，不匹配硬停指路；`run.sh full` 为两段式全核 eigen 满载，4 个数值敏感测试 `-n 1` 补跑规避 ULP 级假 FAIL）。一键式全流程 `release-all.sh` 是 agent/CI 首选入口：base 镜像拉取 → 15 版本烘焙 → 构建 → full 验证 → 提交推送（3 个 RPM submodule + 主仓），五阶段幂等，check 阶段 **15 × `RESULT: PASS` 且 0 FAIL** 硬闸、不过绝不推送，在 `main` 上拒绝运行；`--skip-base` 跳过 base 拉取。完整设计、一键复现指南（含 15 SP 全 full 验证基线）与速查见 [docs/build-deploy-pr-ci/multi-version-build-deploy.md](docs/build-deploy-pr-ci/multi-version-build-deploy.md)（设计方案）、[docs/build-deploy-pr-ci/multi-version-build-deploy-usermanual.md](docs/build-deploy-pr-ci/multi-version-build-deploy-usermanual.md)（用户手册）与 [scripts/offline-build/README.md](scripts/offline-build/README.md)。

### GitHub Actions 每日多 OS 自动验证（`.github/workflows/multi-os-verify.yml`）

CI 哨兵（与上节 `release-all.sh` 发布链路互补）：每天 UTC 04:00（北京时间 12:00）自动触发，也可手动 `workflow_dispatch`；15 个镜像 × 15 个 job 并行（`fail-fast: false`）——openEuler 20.03/22.03/24.03 × LTS+SP1~SP4，运行在 `ubuntu-24.04-arm` 托管 aarch64 runner 的 ghcr.io 构建镜像内。每 job 原生 `meson+ninja` 构建后由 `scripts/gha/verify-params.py` 做全量用例 × 全量参数功能测试（`--quality=-1` 全覆盖、`-n 1/4/8` 三档、openblas `mdim` 扫谱、selftests 正负集），`scripts/gha/benchmark.sh` 采跨 OS 基准，`report` job 汇总 15 份结果成「用例 × 版本」矩阵。零执行与全 skip 分两层判定：任一阶段 `pass+skip` 全为 0（一条结果记录都没有）即按失败处理，不静默放过坏门；`all-quality` 阶段允许只有 skip 记录的通过（仍需 `seeds>0` 种证据），并发档与 `mdim` 档则要求至少 1 个 `pass` 结果（全 skip 判 FAIL）。镜像需先 `./scripts/offline-build/images/build-images.sh <series> <sp> --push` 推到 `ghcr.io/wangxumarshall/sdcshield-offline`。详见 [docs/build-deploy-pr-ci/multi-version-build-deploy.md](docs/build-deploy-pr-ci/multi-version-build-deploy.md) 的「GitHub Actions 每日多 OS 验证」章节与 [scripts/gha/README.md](scripts/gha/README.md)。

### PR CI 门禁、安全扫描与基准趋势

PR（`pr.yaml`，arm64-only）：lint（tabs/codespell/actionlint）→ 新增代码随机性门禁（`scripts/gha/check-randomization.py`：对 diff 新增的 `tests/**.cpp` 分级检查——新增自建不可重放 RNG `mt19937`/`random_device`/`rand()` 为 R1 `::error::` 必须改用框架 RNG `random32`/`random64`/`memset_random`/`frandom*`，种子由框架统一管理并随 `-vvv` 落盘、失败可精确重放；固定 pattern 大表为 R2/R3 `::warning::`，有意为之可在文件内加 `R3-justified: <理由>` 注释豁免；diff 扫描本身失败按 exit 3 loudly 报错，不静默放行）→ git 历史/DCO（人工提交必须 Signed-off-by，Copilot Autofix 机器提交豁免）→ `build-cpu`（GCC-arm64：构建 + unittests + selftests + quick 全量跑，结果矩阵写入 PR 页 summary）→ `multi-version` 角落三（20.03-LTS/22.03-SP3/24.03-SP3；源码/适配层改动才跑，docs-only PR 自动跳过）。安全（`security.yaml` + `codeql.yaml`，每周全量 + PR 增量）：zizmor / gitleaks / osv-scanner / CodeQL cpp。基准趋势：nightly 的 24.03-LTS-SP3 `benchmark.tsv` 经 github-action-benchmark 推 gh-pages 分支，出跨 commit 趋势图 + 150% 回归告警（观测期不硬门）。网页侧操作清单（原生失败通知、分支保护、Code scanning 确认）见 [docs/build-deploy-pr-ci/ci-web-operations.md](docs/build-deploy-pr-ci/ci-web-operations.md)。

### 代码检视机器人（PR 自动检视，聚焦 SDC 激发）

仓库接入 codex、CodeRabbit、PR-Agent、Greptile 四个代码检视机器人，在 PR 打开/更新时自动检视；四者配置（`AGENTS.md`、`.coderabbit.yaml`、`.pr_agent.toml`、`.greptile/`）均内嵌 SDC 激发专属检视要点（激发强度与覆盖、数据模式与缺陷激活、golden 字节比对、x86-64 非回归、占位诚实性、SVE/NEON VL、失败现场留证、多样性与检出率口径）。新增 `bot-coverage.yml` 做**非阻塞**覆盖度校验：有界轮询四个 bot 是否已检视，结果写入 step summary 并评论告警，**恒 exit 0、绝不阻塞合并**。四个机器人均已生效（Greptile 于 2026-10-13 完成安装授权，PR #238 首次实测产出检视）。生效核查方法（`gh` 命令与账号对照）、Greptile 安装记录与 SDC 检视要点汇总见 [docs/build-deploy-pr-ci/pr-review-bots.md](docs/build-deploy-pr-ci/pr-review-bots.md)。

### 15 镜像全严格选项稳定性验证（`scripts/lts-stability/`）

本地 podman 上的**最严格**验证入口（GHA 日报的本地超集）：15 个镜像 × 29 条目严格选项矩阵 × 全部测试用例（含 `--quality=-1` SKIP 级、三 RNG 引擎、cpuset 跨 NUMA、全部 `-O` 测试旋钮、selftests、`--on-crash=context` 等）。`run-all-15.sh` 一键全矩阵（`--smoke` 链路快检），`run-lts-stability.sh <series> <sp>` 单镜像全矩阵；每镜像容器内原生构建全功能二进制（`-Dssl_link_type=static` + vendored 库）后跑严格矩阵，全部 15 PASS 才 exit 0。2026-09-17 全链路实测 **15/15 PASS**（main `ebe0bf1`，290 测试集），过程中发现并修复 8 个跨版本软件 bug。用法与设计见 [scripts/lts-stability/README.md](scripts/lts-stability/README.md)。

## OpenSSL SHA（`openssl_sha`，默认构建）

`openssl_sha` 经 OpenSSL 计算 SHA-256/384/512 与 golden 比对。默认 `ssl_link_type=dynamic`，优先使用 vendored OpenSSL（`third-party/openssl/`，需先执行 `./third-party/openssl/build.sh` 构建），若 `install/` 缺失则回退系统 `libcrypto`，两者都不可用时 meson 打印提示并跳过 SSL 测试。

| `ssl_link_type` | 行为 |
|---|---|
| `dynamic`（默认）| 优先 vendored OpenSSL，回退系统 `libcrypto`，构建期动态链接 |
| `static` | 同上，链接静态 `libcrypto` |
| `loaded` | 运行期 `dlopen()` 加载 `libcrypto` |
| `none` | 禁用 OpenSSL；无 `openssl_sha` |

```bash
# 若 third-party/openssl/install/ 已存在则无需手动构建 vendored OpenSSL，
# 否则需先执行：
./third-party/openssl/build.sh
# 仅当回退系统库且未装 openssl-devel 时才需要：
sudo dnf install -y openssl-devel
PKG_CONFIG_PATH=./third-party/eigen5 meson setup --reconfigure builddir --buildtype=release
./scripts/build.sh && ./builddir/sdcshield --list-tests | grep openssl_sha
```

## Vendored 计算库（third-party/，SDC 负载扩展）

自 2026-09-15 起，仓库 vendor 了高优化度计算库作为 SDC 压测负载（2026-09-16 增补 isa-l）——文献结论（见 `docs/paper/SDC_RESEARCH_for_SDCShield-负载设计.md`，31 篇论文综合）：高度优化的第三方库等价于人工写好的数据流型 FU 饱和序列，其中向量 FMA/GEMM 是第一大 SDC 源（SEVI：>92% SDC 事故由 FMA 指令贡献）。每个目录保留上游官方 tag tarball 以溯源，`build.sh` 幂等构建出静态归档到 `install/`（gitignore；meson 探测 `install/`，缺失时打提示跳过对应测试或回退系统库，不阻断构建）。

| 目录 | 内容 | 产出 | 测试 |
|---|---|---|---|
| `third-party/openssl/` | OpenSSL 3.5.0 | 静态 `libcrypto.a` | `openssl_sha`（SHA-2）+ `openssl_sha3`（SHA-3/SHAKE Keccak）+ `openssl_sm3sm4`（国密 SM3 摘要 + SM4-CBC 加解密往返）+ 70 个 `ipsec_*`（46 NEON + 24 SVE 移植变体，默认启用） |
| `third-party/openblas/` | OpenBLAS 0.3.29（TSV110 内核，单线程 + USE_LOCKING=1） | 静态 `libopenblas.a` | `openblas_dgemm` / `openblas_sgemm` / `openblas_zgemm` / `openblas_cgemm`（NEON FMA 微内核，每迭代 copy/compute/verify）+ `openblas_lu`（LAPACK dgesv 主元分解） |
| `third-party/sleef/` | SLEEF 3.9.0（TLFLOAT=OFF） | 静态 `libsleef.a` | `sleef_neon`（NEON 多项式 FMA 链：sin/cos/exp/log double+float × u10/u35 双精度档 + asin/atan/cbrt/log10/sinh/tanh/pow 函数族，足迹旋钮 `-O sleef_neon.nelems=N`）；`sleef_sve`（SVE 变体，double+float × u10/u35，**需 SVE 硬件**——无 SVE 的机器如鲲鹏 920 干净 skip：`CpuNotSupported`） |
| `third-party/pocketfft/` | pocketfft C 版（BSD-3，头文件+源码直接入库，无需 build.sh） | 编入测试库 | `pocketfft_fft`（位反转抽取重排 + 旋转因子 FMA 蝶形链） |
| `third-party/isa-l/` | Intel isa-l 2.32.1（BSD-3，Makefile.unx 路径，aarch64 无需 autoconf/nasm） | 静态 `libisal.a` | `isal_igzip`（igzip deflate/inflate 往返，aarch64 汇编内核）；10 个 `isal_crc*`（NEON pmull CRC16/32/64）。`install/` 缺失时**回退系统 `libisal`**（openssl 式两级 gate） |

OpenBLAS 单线程（`USE_THREAD=0`）是刻意设计：库内多线程会引入非确定性归约顺序（假阳性）；`USE_LOCKING=1` 仅为让框架"每核一 worker 线程"并发调用 `cblas_*gemm` 时内部 packing 缓冲池不互相踩踏（实证：无锁 8 线程×10s 出 62 字节错配，加锁后 128 核（本机全核）30s 压力零错配，2026-09-15 全核压测实证），锁只保护缓冲表元数据、不改计算结果。

isa-l（`isal_igzip` + 10 个 `isal_crc*`）自 2026-09-16 起 vendor 到 `third-party/isa-l/`（v2.32.1，静态 `libisal.a`）；`install/` 缺失时回退系统 `libisal`，两者皆无才剔除 `isal_*` 测试。

```bash
# 首次构建顺序（pocketfft 无需预构建）：
./third-party/openssl/build.sh && ./third-party/openblas/build.sh && ./third-party/sleef/build.sh && ./third-party/isa-l/build.sh
PKG_CONFIG_PATH=./third-party/eigen5 meson setup builddir --buildtype=release && ./scripts/build.sh
# 全核 30s 压测终验（实测 8806/8806 迭代全 pass，2026-09-15，鲲鹏 920 128 核）：
./builddir/sdcshield -e openblas_dgemm,openblas_sgemm,openblas_zgemm,sleef_neon,isal_igzip,pocketfft_fft,openssl_sha -t 30000
```

**GEMM 矩阵尺寸扫谱（test knob）**：四个 `openblas_*gemm` 测试（double/float/complex-double/complex-float）的矩阵尺寸**默认 256**（L2 域；不传 knob 时行为与历史版本完全一致），可经 test knob 扫谱：`-O <testid>.mdim=N`，N∈[16,4096]，越界 fail-loudly 报错并提示合法域。**必须带测试 ID 前缀**——框架的 TestKeyWrapper 以 `<testid>.<key>` 查找（`framework/test_knobs.cpp`），裸 `mdim=N` 会被静默忽略、回退默认 256（用 `-v` 可在日志中核对每个 knob 的实际生效值）。

**GEMM 形态扫谱（test knob）**：`-O <testid>.transab=0..3`（0=NN 默认、1=NT、2=TN、3=TT——TransA/TransB 在 OpenBLAS 内部走不同 packing 例程）+ `-O <testid>.beta_permille=P`（P∈[0,1000000]，β=P/1000，默认 0；β≠0 命中 C 读改写路径，C 以固定非零模式预填保证确定性）。四个 gemm 测试均支持。

**每测试每档工作集足迹（每线程 scratch = 3 矩阵；矩阵字节数 = mdim² × 元素宽度）**：

| 档位 | dgemm (double) | sgemm (float) | cgemm (complex float) | zgemm (complex double) | cache 域 |
|---|---|---|---|---|---|
| `mdim=64` | 32KB/矩阵 | 16KB | 32KB | 64KB | L1D |
| `mdim=256`（默认） | 512KB | 256KB | 512KB | 1MB | L2 |
| `mdim=512` | 2MB | 1MB | 2MB | 4MB | LLC |
| `mdim=1024` | 8MB | 4MB | 8MB | 16MB | DRAM |
| `mdim=2048` | 32MB | 16MB | 32MB | 64MB | DRAM/远端 NUMA |
| `mdim=4096` | 128MB | 64MB | 128MB | 256MB | 大页/NUMA 交错 |

（cgemm 元素 = 交错 (re,im) float 对 = 8 字节/元素，是 zgemm 的 complex-float 对应物（zgemm = complex double = 16 字节/元素）；矩阵字节数与 dgemm 相同。）

**全核内存预算表（scratch 合计 = 3 矩阵 × 核数；选档前先对照主机内存）**：

| 核数 | dgemm@1024 | dgemm@4096 | sgemm@4096 | cgemm@4096 | zgemm@4096 |
|---|---|---|---|---|---|
| 128 核 | ~3GB | ~49GB | ~25GB | ~49GB | ~98GB |
| 512 核 | ~13GB | ~197GB | ~98GB | ~197GB | ~393GB |
| 2048 核 | ~50GB | ~786GB | ~393GB | ~786GB | ~1.6TB |

（zgemm 内存紧张时推荐档 **512**；1024 档 128 核实测 ~7.7GB。内存不足时 OOM 走 `report_fail_msg` fail-loudly，不是静默或崩溃。）

**推荐战役模式**：

```bash
# 模式 A：谱系扫档（cache 域逐层压测；openblas 单独跑避免混跑 cache 污染）
for m in 64 256 512 1024; do
  ./builddir/sdcshield -O openblas_dgemm.mdim=$m -O openblas_sgemm.mdim=$m \
    -O openblas_zgemm.mdim=$m -e openblas_dgemm,openblas_sgemm,openblas_zgemm -t 15m
done

# 模式 B：混合套件（多样性 = 检出率；默认 256 档，与全部其他测试同跑）
./builddir/sdcshield -e openblas_dgemm,openblas_sgemm,openblas_zgemm,zstd19,sleef_neon,isal_igzip,pocketfft_fft,openssl_sha -t 30m

# 模式 C：大矩阵深压（大内存主机；4096 档单迭代分钟级，需长窗口，必要时 -n 限并发控内存）
./builddir/sdcshield -O openblas_dgemm.mdim=4096 -e openblas_dgemm -t 2h        # 或 -n 64 限制并发

# 模式 E：单命令 knob 全谱（一条命令同时挂多个 -O，每个 -O 只作用于它前缀的测试，
#         无前缀匹配的测试按默认参数跑——多样性轮 + 参数扫谱二合一；9 个测试 12 个 knob）
./builddir/sdcshield \
    -e openblas_dgemm,openblas_sgemm,openblas_zgemm,openblas_lu,sleef_neon,pocketfft_fft,isal_igzip,eigen_svd_cdouble_sve,ipsec_aes128_cbc_hmac_sha1_sse \
    -O ipsec_aes128_cbc_hmac_sha1_sse.datasize=4194304 \
    -O openblas_dgemm.mdim=1024 -O openblas_sgemm.mdim=1024 -O openblas_zgemm.mdim=1024 \
    -O openblas_dgemm.transab=1 -O openblas_sgemm.transab=2 -O openblas_zgemm.transab=3 \
    -O openblas_dgemm.beta_permille=500 \
    -O openblas_lu.n=1024 \
    -O sleef_neon.nelems=16384 \
    -O pocketfft_fft.n=6144 \
    -O isal_igzip.level=2 \
    -t 30m
```

**模式 D：全谱战役脚本 `scripts/run/run_sdc_spectrum.sh`**——把模式 A/B/C 与全部 test knob 合并为一条命令的两阶段战役：

```bash
bash scripts/run/run_sdc_spectrum.sh                    # 默认：15m/档扫谱 + 2h 深驻留
SWEEP_TIME=30s DWELL_TIME=1m bash scripts/run/run_sdc_spectrum.sh   # 冒烟（约 10 分钟）
```

- **阶段 1 谱系广域扫**（每档独立日志，多样性 = 检出率）：GEMM 尺寸谱（mdim 64..1024）+ 形态谱（transab × beta_permille）、SLEEF 足迹谱、FFT 因子谱（pow2/Bluestein 质数/混合 radix）、压缩 level 谱、加密/哈希全家族 + 混合多样性轮。
- **阶段 2 深驻留**：`--max-test-loop-count=0` 关闭 fracturing，全绿档四负载固定 seed 持续运行——CORE179 类单模式持续暴露。
- 时长经 `SWEEP_TIME`/`DWELL_TIME` env 旋钮覆盖；日志按阶段/档位分文件存 `./sdc_spectrum_<时间戳>/`，末尾汇总 pass/fail。

模式选择依据：CORE179 探针证明 store→reload 的 cache-domain 与跨线模式是触发判别条件（模式 A 逐层覆盖）；文献共识"负载多样性即检出率"（模式 B 与模式 D 阶段 1，详见 `docs/paper/SDC_RESEARCH_for_SDCShield-负载设计.md`）；大矩阵档把分块 GEMM 的 packing/回写路径推进 DRAM 与 NUMA 远端域（模式 C）；固定 seed 长驻留提升单模式的统计采样深度（模式 D 阶段 2）。mdim>256 档在固定时间窗内迭代数按 mdim³ 骤减，是计算密度换覆盖广度的交换——统计采样请加长 `-t`。

**模式 0：单板机器扫描 `scripts/run/sdc_machine_scan.sh`**——压测战役第 0 步，新单板可直接复用：一次性采集 DMI/BMC 传感器/SEL/NUMA/RAS/软件栈，产出 `capabilities.env` 能力开关与人读摘要，自动标记硬件异常（SEL 周期故障、无本地内存 node、无 SVE/cpufreq 等），战役参数据此适配：

```bash
SUDO_PW=<密码> bash scripts/run/sdc_machine_scan.sh          # root 全量模式
bash scripts/run/sdc_machine_scan.sh                         # 无 root 降级模式
```

**模式 F：24h+ 压测执行器 `scripts/run/run_sdc_campaign.sh`**——把模式 D 的两阶段协议扩展为 24h+ 持续测试（持续高频测试比快照式多数量级地抓出缺陷）：P1 全量广域扫 → P2 文献优先级加权 soak → P3 固定 seed 深驻留 → P4×N 循环（随机序重扫 + NUMA 拓扑/并发档轮换）。全程 ipmitool/EDAC/SEL 环境监测，fail-continue + 自动失败分类（known_benign_ulp / full_core_only / sdc_suspect）+ 可复现嫌疑自动逐核二分（CORE179 式定位），`.done` 阶段标记支持断点续跑：

```bash
nohup setsid bash scripts/run/run_sdc_campaign.sh > campaign.log 2>&1 &   # 24h+ 正式
SMOKE=1 bash scripts/run/run_sdc_campaign.sh                            # 冒烟（~15min）
bash scripts/run/run_sdc_campaign.sh --selftest-classify                # 解析器自测
```

## 测试用例与检测能力

当前 ARM64 构建（Kunpeng 920 / openEuler 24.03 SP3）默认 quality 下共 **440 个用例**（gcc + 全部 vendored 依赖齐备时，`--list-tests` 实测 2026-10-12；`--quality=0` 446、`--quality=-1` 451），含 SVE 全向量长家族。许多用例沿用上游 x86 名字（如 `mesh_upi_avx2_*`、`ipsec_*_avx`、`fma_*_avx512`），但实现已落到 NEON / ARM 原生指令，命名保留是为与 x86 参考用例跨架构比对。

| 检测域 | 代表用例 | 检测能力 |
|---|---|---|
| 内存 / 拷贝 | `memcpy_l{1d,2,3}_cache_size`、`memcpy_rewr`、`mem_disambiguation`、`mmu_stress_arm`、`random_access_sweep` | 各级缓存带宽、store-to-load 转发、跨行一致性与内存序、TLB 扰动、大 mmap 随机偏移/块长扫掠（L1..DRAM 尺度 + NEON RMW 字节精确校验，knob: mode/valmode/size_mb） |
| 缓存 / 互联 | `cachebounce`、`mesh_upi_*`（45，含 18 个 SVE 变体） | cache line 弹跳、CLFLUSH 压力、MESH/UPI 多核读写协同 |
| 锁 / 原子 | `lock*`、`lockless_cmpxchg*`、`atomic_simd_*`、`spinlock_*`（31） | 锁指令、无锁 cmpxchg、128/256/512 位原子、自旋锁各类竞争（ARM64 atomic） |
| 向量 / SIMD | `swizzle`、`insert_extract`、`kreg1`–`kreg9`、`gather*` | NEON 排列/插入抽取、掩码寄存器（x86 k-reg 仿真）、gather/scatter |
| FMA / 浮点 | `fma`、`fma_patterns_*`、`fma_tail*`、`fpu_special_values` | FMA 模式与尾数精度穷举、特殊值逐字节 golden 比对 |
| 算术 / 大整数 | `adcx`、`adox`、`adcxlong`、`adcx_arm`、`bigint_mulx_arm`、`gmp_big*` | 进位/溢出链、GMP 大整数乘加、高汉明距离操作数压满加法器 |
| CRC / 校验 | `crc32`、`isal_crc{32,64}_*`、`zpclmul*` | `crc32` 指令、isa-l CRC32/CRC64 各标准、zlib PCLMUL 折叠 |
| 压缩 | `zlib*`、`zstd*`、`zfuzz`、`isal_igzip` | zlib/zstd 压缩-解压往返、各级别、fuzz、isa-l deflate/inflate 往返（`-O isal_igzip.level=0..3` 扫四套 match-finder 数据结构：静态 Huffman/基础 hash 链/深历史/hash map） |
| 线性代数（Eigen） | `eigen_gemm_*`、`eigen_sparse`、`eigen_svd*`（含 `_cdouble_sve`） | GEMM、稀疏 Cholesky、SVD（BDCSVD/Jacobi）施压 FMA/向量 |
| 线性代数（OpenBLAS） | `openblas_dgemm`、`openblas_sgemm`、`openblas_zgemm`、`openblas_cgemm`、`openblas_lu` | OpenBLAS NEON FMA 微内核 GEMM（double/float/complex-double/complex-float 四种 lane 组织），每迭代 copy/compute/verify；矩阵尺寸 `-O <testid>.mdim=N`（16..4096，默认 256）可扫 L1D→L2→LLC→DRAM→NUMA 工作集；`-O <testid>.transab=0..3` + `.beta_permille` 扫 packing/读改写相位；`openblas_lu` = LAPACK dgesv 主元分支 + 行交换 store + 三重 golden 比对（vendored，见上节） |
| 超越函数（SLEEF） | `sleef_neon`、`sleef_sve` | 多项式 FMA 依赖链 × 宽向量，逐字节 golden 比对；u10 + u35 双精度档（两套独立多项式链）+ asin/atan/cbrt/log10/sinh/tanh/pow 函数族；`sleef_neon` 足迹旋钮 `-O sleef_neon.nelems=N`（128..262144，默认 1024，4 的倍数）扫 L1/L2/LLC 工作集；`sleef_sve` 需 SVE 硬件（无则干净 skip）（vendored，见上节） |
| FFT（pocketfft） | `pocketfft_fft` | 复数 FFT + 实数 rfft 前向 golden 比对：位反转抽取重排（store→load scatter）+ 旋转因子 FMA 蝶形链；`-O pocketfft_fft.n=N` 扫因子谱系（512..16384 pow2 / 4099/8191 质数触发 Bluestein 卷积化 / 6144/10000 混合 radix）（vendored，见上节） |
| IPSec / 密码 | `ipsec_*`（70，含 24 个 SVE 移植变体） | AES-CBC/CTR/GCM、HMAC-SHA1/2、XCBC/CMAC/3DES-DOCSIS 于 NEON / SVE |
| OpenSSL SHA | `openssl_sha`、`openssl_sha3`、`openssl_sm3sm4` | SHA-256/384/512（SHA-2 加法链）、SHA-3-224/256/384/512 + SHAKE128 XOF（Keccak 置换 AND/旋转/χθ 步，与 SHA-2 正交的 FU 混合）、SM3 摘要 + SM4-CBC 加解密往返（国密整数通路）vs golden（默认构建，优先 vendored OpenSSL） |
| ARM 加密扩展 | `arm_crypto` | AES（AESE/AESMC）crypto 数据通路 |
| 虚拟化 / 系统寄存器 | `vmx_vmexit_*`、`vmxmsr` | guest 触发 vmexit 退出路径一致性 |
| ARM64 SDC 专项 | `arm64_sdc`、`power_virus_dit`、`ooo_dep_chain_arm`、`lsu_store_forward_arm`、`l2c_cross_cache_line_arm`、`mmu_split_tlb_arm`、`sve512_gather_scatter_arm`、`sve512_f64_chain_arm`、`sve512_f64_special_arm`、`sve512_f32_chain_arm`、`sve512_stencil_axis_arm`、`sve512_xlswave_ctx_arm`、`sve512_f64_chain_svd`、`sve512_f64_special_svd`、`sve512_f32_chain_svd`、`sve512_gather_scatter_svd`、`libomp_ctx_arm`、`sme_za_tileview_arm` | di/dt 电压骤降、乱序依赖链、LSU 转发、L2 跨行、MMU/TLB/页表遍历器、SVE 全向量长度 gather/scatter 间接索引数据通路、SVE 全向量长度 f64 FMLA 串行依赖链、SVE f64 特殊值链（NaN/Inf 类别比对）、SVE f32 FMLA 串行依赖链（svcntw() f32 数据通路，all-VL）、SCF/stencil 轴核触发配方复现器（svdup 系数装载 + RADIUS=6 双向 svmla 链 + VA[63:48] 累加器地址金丝雀）、XLSDFT 复合上下文触发器（同一 per-core 循环内交错 64× stencil 轴核、SVD 尺度 gather/scatter 与 7 槽指针表金丝雀 + 每轮 VA[63:48] 金丝雀，三阶段字节精确 memcmp，复现已知故障窗口配方而无 MPI/libomp 脚手架；无 SVE 诚实 SKIP，all-VL 128..1024-bit 自适应（all-VL Phase 2 P3））、SVD 尺度工作集 f64 FMLA 链（L2 溢出 + 16x16 块遍历）、SVD 尺度工作集 f64 特殊值链、SVD 尺度工作集 f32 FMLA 链（svcntw() 通路，all-VL）、SVD 尺度工作集 gather/scatter 往返（2-D 块索引置换）、libomp fork/join 上下文仿真（SVE FMLA 突发 × 时间同步集体停放窗口交替 + 函数指针表/计数器元数据金丝雀——指针比对、间接调用返回值、逐字节 memcmp 三层校验；只仿真访问模式、不复刻真实 libomp TLS/元数据布局（如实标注），无等待对端进度的设计天然无死锁；无 SVE 诚实 SKIP，all-VL 128..1024-bit 自适应（all-VL Phase 2 P3））、SME ZA tile 语义压力（h 装载 round-trip / v 列转置读出 / 向量选择 imm 旋转 / za1h 独立 round-trip + za0 隔离，逐字节 golden；全 SVL 128–1024-bit 运行时自适应，无 HWCAP2_SME 或超范围 SVL 诚实 SKIP；需 clang +sme 工具链，gcc 构建自动剔除） |
| SDC 激发增强套件（2026-09-17 研究落地，2026-10-12 合并） | `sve512_nt_reload_arm`、`sve512_fmmla_arm`、`sve2_cross_precision_arm`、`sve512_fcmla_arm`、`sve512_pred_ops_arm`、`sve_fpcr_cartesian_arm`、`fpcr_rounding_cartesian_arm`、`power_virus_dit_sve_arm`、`mesh_upi_avx512_symm_f64`、`sme_fmopa_za_arm` + 既有测试增强 | 针对已知鲲鹏故障签名与 SEVI(ASPLOS'26)/ITHICA 研究结论定向编写：**nt_reload** = NUMA3 VA 通路触发指令窗口首覆盖（ld1rd/ldnt1d/stnt1d 非时间访存 + 栈重装载距离旋钮 1..4）；**FMMLA/跨精度/FCMLA/谓词** = SVE2 矩阵外积、FP16/BF16 加宽乘加、复数 FMA 全旋转（0/90/180/270）、谓词网络（whilelt 密度扫描/谓词翻转/FADDA 串行归约/LDFF1+FFR/FMAXV）——此前零指令级覆盖的数据通路；**FPCR 笛卡尔**（NEON + SVE 伴生）= 舍入模式 RNE/RTZ/RDN/RUP × FZ 8 配档全扫（FPCR 现场保存/恢复纪律，本机 920 真机验证）；**power_virus SVE** = 全宽 SVE di/dt 病毒（突发内嵌地址生成）；**mesh f64** = 首个 FP mesh 变体（跨核共享数据 FP 生产/消费，0.5 倍数精确块校验和使总和无序可判定）；**SME FMOPA** = 纯 inline asm 的 ZA 外积链（gcc 可建，内核未暴露 HWCAP2_SME 时干净 skip——`selftest_sme_sigill_probe` 一跑定论）。既有增强：sve512 链族值域旋钮 `-O <test>.bounded=0|1`（SEVI Obs.19 有界/无界域 SDC 频率差 245×，默认 1 不改行为）；gather_scatter 读偏移显式断言（源值携带自身索引，76% 内存类 SDC 为错偏移读）+ u64base/宽步距变体。框架诊断：memcmp 失败 lane 偏移直方图（单 lane/相邻 lane = 单物理单元指纹，SEVI Obs.13）、aarch64 崩溃消息 VA[55:48] 签名判读（只报不判）。SVE/SME 类在 920 上干净 skip（编译+指令生成已验证），byte-exact golden 真机首验在 cn23154（SVE512 故障机） |
| ARM64 触发配方 | `agu_stress_2src`、`neon_rot_2src`、`neon_rot_ldr_at_top_rowmajor`、`movbe` 系列（`movbe`、`movbe_dump`、11 个 `movbe_dump_probe_*`） | AGU 吞吐施压（2 源加载 + 旋转 ALU + store/reload/store）、core-179 配方的 NEON 向量通路判别（uint64x2 旋转 ALU + 向量 store/reload/store）、ldr_at_top 扫描顺序变体（升/降序交替，区分槽位局部 vs 前进位置特征）、core-179 字节交换往返触发探针组 |
| SVE 版 avx53 套件（tests/cpu/sve/，53 个） | `fma_tail_sve{,_wide}`、`fmatail_sve{,_wide}`×4 对、`fma_patterns_sve_wide_{ps,pd}`、`mesh_upi_sve_*` 18 个、`eigen_svd_bidiag_sve`、`ipsec_*_sve{,_wide}` 24 个 | 53 个 avx 命名 NEON 测试的 SVE1 移植（负载环境不变：向量宽度/随机域/特殊值注入/块结构/原子协议/golden 模式全保留；命名 `_avx/_avx2→_sve`、`_avx512→_sve_wide`）。FMA 10 + Mesh 18 = 纯指令替换（SVE 谓词访存）；eigen 1 = 自写 Householder 双对角化 SVE 负载（替代崩溃的 Eigen-SVE 后端路径）；ipsec 17 = EVP 加密 + 多流 SVE HMAC（SHA 内核与 OpenSSL 全向量比对 5200+1360 全对，lane0=原 MAC 语义+变体填 lane），7 个 EVP-only（GCM/CMAC/XCBC）如实标注计算路径。构建于 `tests_sve_avx53`/`tests_sve_avx53_ipsec` 库（`-march=armv8.2-a+sve`，init 探 HWCAP_SVE 干净 skip） |
| IST 硬件自检 | `ist`、`ist_array`、`ist_sbaf` | ARM64 In-Silicon Test（当前 placeholder，见下表） |

### 用例质量分级

| 级别 | 名称 | 运行条件 | 实有数量 |
|---|---|---|---|
| -1 | SKIP | `quality >= -1` | 5 |
| 0 | BETA | `quality >= 0` | 6 |
| 2 | PROD（默认）| `quality >= 2` | 440（gcc + 全部 vendored 依赖，实测 2026-10-12） |
| | **合计** | | **451** |

> 上表为 gcc 参考平台（2026-10-12 实测）口径。cn23154 复现分支（clang/libc++ 工具链，含 sve512 专项家族与 SME 用例）2026-09-28 实测：默认 PROD 列出 322，`--quality=-1` 全量 333；与参考平台的差值来自构建条件与平台可用性差异，未做逐项拆解。

- **BETA（`--quality=0`）**：`arm64_sdc`、`arm64_sdc_sve`、`arm_crypto`、`ist_sbaf`、`neon_add`、`neon_add_sve`
- **SKIP（`--quality=-1`）**：`smi_count`、`eigen_svd_jacobi`、`eigen_svd_jacobi_cdouble`、`eigen_svd_jacobi_double`、`eigen_svd_jacobi_fvectors`

### 占位用例的诚实跳过

暂未实现的特性用例返回 `EXIT_SKIP` 并附理由（而非 `EXIT_SUCCESS` 伪装通过）。实测 `skip-reason`：

| 用例 | 跳过理由 |
|---|---|
| `ist` / `ist_array` / `ist_sbaf` | `to be implemented (placeholder): ARM64 In-Silicon Test (IST) backend not yet available; test reserved as the counterpart of Intel IFS` |
| `smi_count` | `to be implemented (placeholder): SMI counting requires a per-CPU firmware/RAS-interrupt counter not available on this architecture` |

> `mce_check` 是真实 EDAC 后端测试（统计 `/proc/interrupts` 的 EDAC `ce/ue_count`），实测 `exit: pass`，非 placeholder。

**能力门控的诚实跳过**（skip 不是只有"未实现"一种）：17 个多流 HMAC 的 ipsec SVE 用例（`*_hmac_sha*` / `*_sha2_*`）每条 HMAC 流占一个 SVE lane，**流数 = 运行时 lane 数（all-VL Phase 2 P4：sha1/224/256 为 `svcntw()` 4..32 流、384/512 为 `svcntd()` 2..16 流，128..1024-bit 全档自适应）**，经框架层 `arm64_require_sve_vl` 门控（HWCAP_SVE 能力 + 范围检查一行）；W 调度表按 1024-bit 上限定维（修掉了旧固定 8/4 维度在 VL≥512 硬件上的行越界隐患），`data_size` knob 上限钳制到 HMAC 组装缓冲容量。无 SVE 的硬件在框架级 `test compiled with sve` 守卫处跳过。内核流数参数化形态经 gem5-fi `sve_sha_dual` 四档 16-run 实证（128-bit 档 4/2 流全绿即"短 VL 可跑"的命题实证；K/IV 常量表同源复制，FIPS 标量参考逐流字节级比对）。

### `memcpy_rewr`：MPSC memcpy 一致性压测（使用选项与推荐参数）

GlusterFS IOT 调度器衍生的多生产者/单消费者队列 memcpy 一致性压测（ARM64 专属）：producer 经 4 优先级链表队列投递，consumer 对线程私有 `mem_info_s` 的 b1..b5 做五路 memcpy 并逐字节比对——拷贝不崩溃但不一致即为 SDC。角色分配与拷贝长度由策略配置驱动，无 conf 文件时自动退化成原版独立工具的默认模式（不 skip）。

**conf 模式**（`tests/cpu/memory/memcpy_rewr_strategies.conf`，仓库默认提供）——`SANDSTONE_STRATEGY_INDEX` 轮转选策略，`SANDSTONE_STRATEGY_CONF` 覆盖路径：

```bash
for i in 0 1 2; do SANDSTONE_STRATEGY_INDEX=$i ./builddir/sdcshield -e memcpy_rewr -t 5000; done
```

| INDEX | 策略 | 拓扑意图 | 推荐场景 |
|---|---|---|---|
| 0 | `numa_cross_node`（`numa_split`） | 跨 NUMA 节点 barrage，所有数据移动穿跨 die 一致性 Fabric | 多路/多 NUMA 机器的互联压力 |
| 1 | `same_die_l3_brawl`（`die_even_odd`） | 同 die/cluster 内 even/odd 核对打，一致性流量锁死在单 L3 slice + ring bus | 单 die 内 L3/环网压力 |
| 2 | `few_producer_many_consumer_storm`（`first_n_producers`，`producer_count=4`） | 少写多读，诱发目录控制器 Invalidate 广播风暴 | snoop/目录控制器压力 |

**默认模式**（conf 缺席，如 `SANDSTONE_STRATEGY_CONF=/nonexistent`，或部署机无源码树）——参数按机器自适应（全核并行，`test_schedule_fullsystem`）：

- `block_size = clamp(MemAvailable×50% ÷ 线程数 ÷ 6, 64 KiB, 2 MiB)`（6 = 每线程实际触碰的 buffer 前沿数保守上界；2 MiB = `mem_info_s` 字段宽，即原版 `g_size` 溢出边界的安全上限）
- `producer_count = max(1, CPU核数/12)`（48 核 → 4:44，对齐原版参考参数 p 0..3 / c 0..47）
- 忽略 `SANDSTONE_STRATEGY_INDEX`；conf 存在但损坏/不可读仍然 loud skip（不静默吞错）

**参考实测**（Kunpeng 920，128 核，MemAvailable ≈ 22.5 GB；`-vv` 日志）：

- 默认模式全核：`strategy[0]=default_original block_size=2097152 threads=128 producer_count=10`（自适应封顶 2 MiB），峰值 RSS ≈ 1.52 GiB，无 OOM
- conf 三策略全核（`-t 3000`）：`block_size=65536 threads=128 producer_count=4`，三档均 `result: pass`
- 单跑推荐：`-t 5000` 全核（约 5 s/策略）；策略覆盖推荐 `for i in 0 1 2` 轮转各一轮

## 运行测试

```console
./builddir/sdcshield --list-tests                        # 列 PROD 用例（默认）
./builddir/sdcshield -e zstd19 -t 5000                   # 单测试，5 秒，全核
./builddir/sdcshield -e zstd19 -t 5000 -n 1             # 单线程，规避 192 核 ULP flakiness
./builddir/sdcshield --quality=0 -e arm64_sdc -t 5000   # 跑 BETA 用例
./builddir/sdcshield --quality=-1 -e eigen_svd_jacobi   # 跑 SKIP 用例
./builddir/sdcshield -l                                  # 用例 + 描述 + 分组
./builddir/sdcshield --list-groups                       # @compression / @ipsec / @math
./builddir/sdcshield --dump-cpu-info                     # CPU + 特性 + 拓扑 + SVE/SME 向量长度(aarch64)
./builddir/sdcshield --on-crash=context -e selftest_sigsegv -vv   # 崩溃回溯
./builddir/sdcshield --fatal-skips ...                   # skip 也按致命处理（不静默吞掉）
```

`--dump-cpu-info` 在 aarch64 上额外输出 **SVE 向量长度**与 **SME streaming 向量长度**（有硬件时为实测 bits/bytes，无则显式 `n/a`）——二者是 per-thread 运行量，与进程级 feature 位图相互独立（`docs/architecture.md` 的 arm64_vl.h 层）。

`scripts/run/` 狩猎脚本遵循**失败留证**口径：逐测试长时窗轮跑（如 `run-sdc-hunt-30min.sh`）逐用例 `-o` 落盘 YAML 日志并传 `--fatal-skips`（skip 也算失败，诚实可见）；无论有无 SDC/失败信号，日志一律保留供事后审计，汇总 CSV 中 `pass` 与日志内 SDC 记录双保险——`exit_rc=0` 但日志含 `miscompare` 记为 `sdc_log`，不误报为 `pass`。

### Test knob（`-O`，运行期测试参数）

部分测试支持**运行期参数旋钮**——不重编译即可改变矩阵尺寸、工作集大小、压缩级别等压力参数，参数值写入 YAML 日志（可复现）。语法：`-O <测试ID>.<参数名>=<值>`（可重复传多个）。

```console
# 默认 256 档（L2 域）→ 4096 档（806MB 工作集，压到 DRAM/NUMA 远端域）
./builddir/sdcshield -e openblas_dgemm -O openblas_dgemm.mdim=4096 -t 30m

# 形态扫谱：转置组合 + β 读改写路径
./builddir/sdcshield -e openblas_zgemm -O openblas_zgemm.transab=3 -O openblas_zgemm.beta_permille=500 -t 15m

# ipsec 载荷 1024B→16MB：AES/3DES/SHA 数据路径从 L1 压到 DRAM（46 个 NEON ipsec 用例同款参数名；24 个 SVE 变体无此 knob）
./builddir/sdcshield -e ipsec_aes128_cbc_hmac_sha1_sse -O ipsec_aes128_cbc_hmac_sha1_sse.datasize=16777216 -t 5000

# SLEEF 足迹扩到 6MB（默认 1024 元素 ≈ 128KB）
./builddir/sdcshield -e sleef_neon -O sleef_neon.nelems=262144 -t 30m

# FFT 走 Bluestein 质数路径（默认 4096 是纯 radix-4/2）
./builddir/sdcshield -e pocketfft_fft -O pocketfft_fft.n=4099 -t 15m
```

**两个易错点**：① 必须带测试 ID 前缀——框架以 `<测试ID>.<参数名>` 为查找键（`framework/test_knobs.cpp`），裸 `-O mdim=4096` 会被**静默忽略**并回退默认值（`-v` 可核对每个 knob 的实际生效值）；② 越界值 fail-loudly 报错（不静默钳制），报错信息含合法范围。

当前支持 knob 的测试全集：

| 测试 | 参数 | 合法域 | 默认 | 作用（档位→压力域） |
|---|---|---|---|---|
| `openblas_{d,s,z,c}gemm` | `.mdim` | 16..4096 | 256 | 矩阵尺寸：64→L1 / 256→L2 / 1024→L3 / 2048+→DRAM |
| `openblas_{d,s,z,c}gemm` | `.transab` | 0..3 | 0 | NN/NT/TN/TT（OpenBLAS 不同 packing 例程） |
| `openblas_{d,s,z,c}gemm` | `.beta_permille` | 0..10⁶ | 0 | β=P/1000；β≠0 命中 C 读改写路径 |
| `openblas_lu` | `.n` | 16..2048 | 256 | LU 分解矩阵尺寸（64→32KB … 2048→32MB） |
| `sleef_neon` | `.nelems` | 128..262144（4 的倍数） | 1024 | 每函数族元素数（工作集 128KB→6MB） |
| `pocketfft_fft` | `.n` | 512..16384（pow2）/ 4099 / 6144 / 10000 | 4096 | FFT 点数与因数结构（pow2=radix-4/2，4099=Bluestein 质数） |
| `isal_igzip` | `.level` | 0..3 | 1 | deflate 级别（不同匹配查找器；2/3 有独立 level_buf） |
| `zstd` / `zstd1` / `zstd19` / `zfuzz` | `.level` / `.maxbuffersize` | 1..22 / 4096.. | 按变体 | 压缩级别 / 每迭代缓冲上限 |
| `eigen_svd_cdouble_sve` | `.mdim` | 300..6000 | **按内存自适应** | 强制矩阵维度（覆盖自适应选择） |
| `ipsec_*`（46 个 NEON 变体；24 个 SVE 变体无此 knob） | `.datasize` | 1024..64MB（16 的倍数） | 1024 | 加密/解密/MAC 载荷尺寸：1024→L1 / 64KB→L2 / 1MB+→L3/DRAM——AES/3DES/SHA 数据路径全谱扫（2026-09-19；默认 1024 与历史逐字节一致） |
| `memcpy_rewr` | 环境变量 `SANDSTONE_STRATEGY_INDEX` / `SANDSTONE_STRATEGY_CONF` | 0..2 / conf 路径 | — | MPSC 策略选择（env 而非 `-O` 机制） |

仍无 knob 的测试（eigen NEON 家族、isal_crc×10、openssl×3 等）参数为编译期常量——参数审查（`docs/research/third-party-sdc-param-critique.md`）记录的已知状态；ipsec×46（NEON）已于 2026-09-19 补齐 `datasize`。

Eigen SVD：`eigen_svd_cdouble` 跑在 NEON 后端；`eigen_svd_cdouble_sve` 跑在 SVE 向量后端（vendored Eigen 5.0 已补 `PacketXd`/`PacketXcd` double 与 complex<double> packet——svcmla 复数乘法、ptranspose 复数转置、gather/scatter 等全套，2026-09-19）。本机（VL=256）实测 300×300 复数 BDCSVD ~0.7 s（标量回退时代 >10 分钟）；VL=512 场景在 gem5 SE 模式功能验证（`scripts/eigen-sve-double/gem5/`）。注意：size-specific SVE 代码要求运行时向量长度等于编译期 `-msve-vector-bits`（128），本机 256 硬件上独立运行该测试需 prctl 固定任务 VL。

## 架构支持

| 架构 | 状态 | 说明 |
|---|---|---|
| ARM64（AArch64） | ✅ 全支持 | Kunpeng 920、通用 ARMv8.1+ |
| x86-64 | 参考架构 | Intel 原始代码路径，移植中不动 |

ARM64 能力：CPU 特性检测（FP/NEON/CRC32/Crypto/SVE/SVE2）、拓扑检测（ACPI PPTT/sysfs/device tree）、SDC 检测（EDAC ECC、CRC32/CRC64）、SIMD（NEON 128 位 + 256/512 仿真）、RAS/ECC（EDAC/ACPI APEI）。

## 运维入口

SDC 激励/复现战役（M0–M5）的机侧运维脚本在 `scripts/sdc-excite-reproduce/`，总入口为 excite.sh（用法与战役编排见上节「一键最大程度激发 SDC」）：

```console
sudo bash scripts/sdc-excite-reproduce/excite.sh    # 一键总入口：预检 → systemd 单元 → sdc-monitor → 冒烟门 → 上线
bash  scripts/sdc-excite-reproduce/excite.sh --smoke / --status / --stop    # 只冒烟 / 只读巡检 / 停止（留断点）
```

细分脚本（excite.sh 即其首启序列的固化）：`status.sh` 只读状态一览——9 服务 / 战役进度 / M2 事件流+controller 状态+采集器丢样 / 电压频率 / SEL / 磁盘 / 告警（即 `--status`）；`start.sh` 直接启动战役（需 root，跳过预检与冒烟门）；`stop.sh` 停止战役（保留断点，即 `--stop`）。

完整运维流程（9 服务、BLACK 恢复、演练排期、巡检模板）见 [docs/sdc-excite-reproduce/operations-runbook.md](docs/sdc-excite-reproduce/operations-runbook.md)。

## 延伸

- [PR 检视-修复闭环手册](docs/build-deploy-pr-ci/pr-loop.md) — 会话内 agent 自动化：push 自动开 PR → 四 bot 检视 → subagent 批判性处置修复 → 循环闭环 → 人工 merge（引擎 `scripts/pr_loop.py`，双端入口 `/pr-loop` 与 `.trae/rules/pr-loop.md`）
- [编写测试指南](docs/coding/writing_tests.md) — 框架处理了测试生命周期、线程模型、CPU 特性识别、RNG 等样板代码
- [离线构建依赖与排坑](docs/build-deploy-pr-ci/offline-build-dependencies.md) — 完整依赖树、版本管制、坑点
- [SDC 前沿研究综合](docs/paper/SDC_RESEARCH_for_SDCShield-负载设计.md) — 31 篇 SDC 文献（SOSP/HPCA/ISCA/MICRO/ASPLOS 等，2003–2026）系统性总结，负载设计与 vendored 依赖库选型依据
- [SDC 学术综述报告](docs/paper/SDC_ACADEMIC_SURVEY_CN.md) — 53 篇论文全文精读（笔记在 `docs/paper/ref-paper-fullread-digest/`）写成的分类学综述：核心特征/根因/故障模式/检测/处理五层分类学，学术界×产业界对比、合作生态与全栈×全生命周期启示
- [贡献指南](CONTRIBUTING.md) · [行为准则](CODE_OF_CONDUCT.md)
