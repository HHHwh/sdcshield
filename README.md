# SDCShield

SDCShield is an open-source project designed to identify defects and bugs in ARM CPUs derived from Intel's OpenDCDiag (ARM64 port). It consists of a set of tests built around a sophisticated CPU testing framework, licensed under the [Apache License 2.0](LICENSE). See [NOTICE](NOTICE) for full derivation and upstream provenance.

SDCShield 专攻**静默数据损坏（SDC, Silent Data Corruption）**——计算不崩溃、但产出错误比特的缺陷。检测原理：向目标计算单元（FMA、SIMD、压缩、SVD、crypto）施加高强压测，再与预计算的 golden 值做**字节级比对**（错一比特即 FAIL，无容差）。本项目由 agent 全流程自动化开发与检视（见下节）。

## 质量保障：Agent 全流程自动化

SDCShield 的检测结论必须可信，因此仓库把「质量保障」做成贯穿开发—检视—CI—发布的全流程自动化体系，总体架构：

![Agent 全流程自动化总体架构](docs/build-deploy-pr-ci/agent-full-automation.png)

四层机制（工程细节见「质量保障与 CI 基础设施」一节）：

1. **PR 检视-修复闭环（六阶段，绝不自动 merge）**——push 到特性分支自动开 PR；五路检视：四个外部 SaaS bot（Codex / CodeRabbit / PR-Agent / Greptile）+ 本地无头 claude 对抗检视；`scripts/pr_loop.py` 驱动 Status → Wait → Collect → subagent 批判性处置（逐条拒绝或修复并说明）→ 修复提交触发新一轮检视 → 循环至 READY_TO_MERGE → **人工 merge**。合入权永远在维护者手中。
2. **无人值守任务闭环（task-loop）**——main CI 红 / main 增量审查 / 会话 / issue 统一进入 `agent-task` 队列，headless agent 修复后开 PR，走同一条检视闭环，最终仍由人工 merge（`scripts/task_loop.py`）。
3. **CI 哨兵与发布闸门**——每日 cron 在 15 个 openEuler 版本容器内全量构建 + 全用例 × 全参数验证（`multi-os-verify.yml`）；PR 级 lint / DCO / 随机性门禁 / 多版本构建；发布链 `release-all.sh` 以 15 × `RESULT: PASS` 为硬闸。
4. **安全与稳定性扫描**——CodeQL / Gitleaks / osv-scanner / zizmor（每周全量 + PR 增量）；ASan+UBSan 夜巡；本地 15 镜像 × 29 条严格选项矩阵（`scripts/lts-stability/`）。

架构设计、PR 状态机与分阶段路线图见 [agent-full-automation.md](docs/build-deploy-pr-ci/agent-full-automation.md)、[PR 检视-修复闭环手册](docs/build-deploy-pr-ci/pr-loop.md)、[task-loop 手册](docs/build-deploy-pr-ci/task-loop.md)。

## 一行命令检测 CPU SDC

**源码树（推荐）**：

```bash
bash scripts/run/sdc_detect.sh
```

**预构建二进制**（仅限 2026-10-09 后由 `scripts/offline-build/package-built-artifacts.sh` 打包、随包 `sdc_detect.sh` 的版本——`detect` 子命令自动调用；CI tarball 与旧包用下方等效命令，进入目录见下节「直接运行预构建二进制」）：

```bash
./run-sdcshield.sh detect
```

`bash scripts/run/sdc_detect.sh [--smoke] [透传参数…]`（`-h` 看完整帮助）：

| 项 | 说明 |
|---|---|
| `--smoke` | 冒烟自检：每桶前 2 用例各 2 秒单遍（约 35 秒），验证链路与优先序 |
| 透传参数 | 原样交给 sdcshield（如 `-t 30s`、`--cpuset 0-47`） |
| `SDC_BIN` | 显式指定二进制；默认依次发现 `./builddir/sdcshield`、`./builddir-gcc/sdcshield`，找不到即报错并给构建指引 |
| 产物 | `sdc-detect-testlist-*.txt`（优先序名单，可复现）+ `sdc-detect-*.yaml`（日志） |
| 停止 | Ctrl-C 干净停止；FAIL 或显式计失败的 SKIP 即停（`-F` + `--fatal-skips`） |

**优先序机制**：脚本从当前二进制动态生成优先序测试列表（框架 `--test-list-file`，顺序即执行序，对新用例自适应），先跑 SDC 敏感度最高的负载：

| 优先级 | 负载域（分桶正则实测口径） | 依据 |
|---|---|---|
| P1 | 向量 FMA/矩阵（`openblas_*gemm`/`eigen_gemm`/`eigen_svd`/`eigen_sparse`/`sleef_*`/`pocketfft_fft`/`fma*`/`sve512_f*`）+ `arm64_sdc`、`acl_gemm`、`neon_add` | SEVI(ASPLOS'26)：>92% SDC 事故由 FMA 指令贡献 |
| P2 | ARM64 SDC 专项（`sve512_*`、`power_virus_dit`、`lsu_store_forward`、`l2c_cross_cache_line`、`mmu_split_tlb`、`fisttp`、`libomp_ctx`、`sme_za` 等） | 本仓库为已知鲲鹏故障签名定向编写 |
| P3 | memcpy/store→reload 访存通路 | CPU179 实证触发判别条件 |
| P4 | 位扩散（压缩/哈希/CRC/加密） | From Gates to SDCs(DATE'25)：sha 类位掩蔽最少 |
| P5 | 标量浮点/大整数 | — |
| P6 | 一致性/原子/SIMD | 控制流类故障多 crash 化，SDC 倾向低 |
| P7 | 其余（字母序） | 完整多样性保留 |

**等效命令**（无脚本时；默认注册序无优先级——SDC 敏感负载不会先跑）：

```bash
# 源码构建 / 预构建二进制（后者进入对应 SP 的 built 目录；旧二进制判读警示见「直接运行预构建二进制」节）
./builddir/sdcshield --quality=0 -T forever -t 60s -Y -F -o sdc-$(date +%m%d-%H%M).yaml
./run-sdcshield.sh --quality=0 -T forever -t 60s -Y -F -o sdc-$(date +%m%d-%H%M).yaml
```

**为什么是这条命令**：根据我们的经验，全核并发 + 负载多样性 + 长驻留三者齐备才有检出率，这条命令一次配齐。

**命令逐项解释**（脚本已内建，无需手输）：

| 旗标 | 作用 |
|---|---|
| `--quality=0` | 跑 PROD+BETA。**必须加**：`arm64_sdc`（di/dt 电压骤降专项）等 10 个 BETA 用例不加会被跳过（分级与 `sched_storm×4` 降级历史见「用例质量分级」） |
| `-T forever` | 整套用例无限轮转。SDC 故障窗口极稀疏（CPU179 实测占空比 ~0.06%），长驻留是检出关键 |
| `-t 60s` | 每用例 60 秒。SEVI 实测 >80% 的首错在 10 秒内出现，60s 有 6 倍余量 |
| `-Y` | 结构化 YAML 日志（取证格式） |
| `-F` | 首个 FAIL 即停——停下就是信号；同时传 `--fatal-skips`：SKIP 也计失败（占位/能力缺失不被静默吞过） |
| `-o sdc-detect-*.yaml` | 固定文件名落盘。不带 `-o` 时框架生成时间戳文件、全过时自动删除，仅失败/中断留存 |

**结果判读**：

1. **一直跑不停** = 本轮全过；`-T forever` 自动进入下一轮，检出率随时间积累。
2. **停下且有 FAIL** = **SDC 嫌疑**：保留 `-o` 落盘的 YAML 日志（含 seed/迭代号，可复现）。含 2026-10-24 修复的构建中满核 FAIL 与其他测试同等进入定位流程，`-e <测试名> -t 5000 -n 1` 单线程复跑可作交叉对照；旧二进制的 eigen 满核 FAIL 判读口径见「直接运行预构建二进制」节的警示块。eigen 族以同源重算比对，检出**非确定性缺陷**（同输入不同位）；对**确定性核缺陷**存在结构性盲区（已披露的覆盖空洞）。
3. **停止方式**：Ctrl-C 干净停止（当前测试跑完边界即停，日志完整落盘，`exit: interrupted`）。
4. **检出后的定位**：`bash scripts/run/run_sdc_campaign.sh`（24h 战役，自动失败分类 + 可复现嫌疑自动逐核二分定位）。

## 一键最大程度激发 SDC（7×24 战役总入口）

```bash
sudo bash scripts/sdc-excite-reproduce/excite.sh          # 一键上线 7×24 战役
bash  scripts/sdc-excite-reproduce/excite.sh --smoke      # 只预检+冒烟（无需 root）
bash  scripts/sdc-excite-reproduce/excite.sh --status     # 只读巡检
sudo bash scripts/sdc-excite-reproduce/excite.sh --stop   # 停止（保留断点）
```

**更激进（单模式深驻留）**：追加 `--max-test-loop-count=10`（自动模式默认 40，控制种子随机生成的频次）：

```bash
./builddir/sdcshield --quality=0 -T forever -t 60s --max-test-loop-count=10 -Y -F -o sdc-$(date +%m%d-%H%M).yaml
```

run 模式需 root（sudo）、幂等可重入（已安装/活跃服务自动跳过，`state.json` 断点保留）；`SDC_BIN` 指定二进制，`SMOKE_TIMEOUT_S` 覆盖冒烟门上限（默认 2400 秒，慢板机器可调大）。链路：预检（fail-loud + L0 冒烟门）→ systemd 单元 → sdc-monitor 联锁 → 战役冒烟门（不过不上线）→ L1-L5 状态机战役（冷机代表集 → 谱系扫档 → 多样性轮转 → 专项轮换 di/dt·热·NUMA → 深驻留，事件取证 + 断点续跑）；M2 事件栈按 [运维手册](docs/sdc-excite-reproduce/operations-runbook.md) 拉起（`-h` 看完整帮助）。

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

预构建二进制（git 08c37be3，2026-09-12 构建）含 221 个 PROD 用例——少于源码构建（487 全量上界），少的是镜像内未含的 vendored 计算库测试组；用例计数口径见「用例质量分级」。

> **旧二进制判读警示**：2026-09-12 预构建二进制是 eigen 满核假阳修复（2026-10-24）**之前**的构建——其上 `eigen_svd_double`/`eigen_sparse` 的满核 FAIL 按旧口径判读为已知 ULP 级良性抖动（`-n 1` 复跑通过即可），不升级为 SDC 嫌疑；"满核挂 = SDC 嫌疑"的判读规则仅适用于含修复的构建。用旧二进制跑 24h 战役脚本须显式恢复旧白名单（含修复的源码构建不需要、也不应设置）：
>
> ```bash
> SDC=third-party/rpms/openEuler-24.03/openEuler-24.03LTS_SP3/built/run-sdcshield.sh \
> KNOWN_FLAKY="eigen_svd_double eigen_sparse" bash scripts/run/run_sdc_campaign.sh
> ```

```bash
git clone --recurse-submodules <repo-url>   # 含子模块
cd third-party/rpms/openEuler-24.03/openEuler-24.03LTS_SP3/built
./run-sdcshield.sh --list-tests            # 自动设 LD_LIBRARY_PATH=./libs
./run-sdcshield.sh --quality=0 -T forever -t 60s -Y -F                # 检出 SDC 后停止
./run-sdcshield.sh --quality=0 -T forever -t 60s -Y -ignore-timeout   # 检出后继续跑
./run-sdcshield.sh -T forever -t 600s -Y -e "eigen_svd*" -e "eigen_gemm*" -e "fma*" -e "zstd*" -e "zlib*" # 定向子集，检出率低于全套件轮转
./run-sdcshield.sh -t 60s -n 1 -e zstd19 # 单线程确定性口径（见「运行测试」）
```

> **SP 必须与目标机一致**：SP3 的 `glibc-devel` 携带 `Requires: glibc = <sp3-N>`，装错 SP 触发受保护 `glibc` 降级死结；`install-deps.sh` 以 `.os-version` 标记在安装前拦截。

**从 CI 下载**：每次 [Multi-OS Verify](.github/workflows/multi-os-verify.yml) 运行（每日 cron + 手动触发）为 15 个 SP 各产出自包含 tarball（`built-<series>-<sp>` artifact，保留 90 天；`gh run download <run-id> --name built-24.03-LTS-SP3`），解包后用法同上。要**永久保存**某次构建：手动触发时勾选 `publish_release`，15 个 tarball 上传为 GitHub Release 资产（详见 [multi-version-build-deploy.md §6.2.1](docs/build-deploy-pr-ci/multi-version-build-deploy.md)）。

### 从源码构建

```bash
# openEuler 24.03（基准平台）依赖；Ubuntu/Fedora 见 docs/build-deploy-pr-ci/offline-build-dependencies.md
sudo dnf install -y meson ninja-build gcc g++ cmake boost-devel zlib-devel libzstd-devel libisa-l-devel gtest-devel
# 首次构建前：依次构建 5 个 vendored 依赖库（幂等，install/ 已存在则秒过；pocketfft 无需此步）
./third-party/openssl/build.sh           # → install/lib/libcrypto.a（SSL 测试默认启用）
./third-party/openblas/build.sh          # → install/lib/libopenblas.a（openblas_{d,s,z,c}gemm + openblas_lu）
./third-party/sleef/build.sh             # → install/lib/libsleef.a（sleef_neon / sleef_sve）
./third-party/isa-l/build.sh             # → install/lib/libisal.a（isal_igzip + isal_crc*；缺省回退系统 libisal）
./third-party/acl/build.sh               # → install/lib/libarm_compute-core.a（acl_gemm NEGEMM + fisttp_arm；NEON runtime core 目标，v23.02）
# third-party/arm-opt/（Arm Optimized Routines）无需构建：随仓库直接携带
#   include/mathlib.h + stringlib.h 与 lib/libmathlib.a + libstringlib.a
#   （静态链入，aor_math / aor_string / cbw_arm 三测试；不 vendored .so——
#   22.03 SP4 无 libmathlib.so 且动态链入会带进 GLIBC_2.38 版本需求）
PKG_CONFIG_PATH=./third-party/eigen5 meson setup builddir --buildtype=release
./scripts/build.sh
./builddir/sdcshield --list-tests        # 列出默认 PROD 用例——数量随芯片能力过滤（见「测试用例与检测能力」）
```

> 构建统一走 `./scripts/build.sh`：默认 `-j` = `nproc`/2（共享服务器保护），`SDC_BUILD_JOBS` 可覆盖（见 `scripts/build.sh` 头注释）。ARM64 要求 Eigen 5.0+（系统 3.3.x 在 GCC 12+ 编译失败）；仓库自带 `third-party/eigen5/`（5.0.1），aarch64 直接 `include_directories` 指向它、无需系统安装，`PKG_CONFIG_PATH` 仅为兼容 x86 路径保留。
>
> 五个 `build.sh` 均可选：任一 `install/` 缺失时 meson 打提示并跳过对应测试组或回退系统库（isa-l 缺失回退系统 `libisal`，两者皆无才剔除 `isal_*`；acl 缺失时 `acl_gemm`/`fisttp_arm` 不构建，SVE 变体不受影响）——不阻断构建。选型依据（向量 FMA GEMM > 哈希/加密 > 压缩 > SVE 超越函数）见 [SDC_RESEARCH_for_SDCShield-负载设计.md](docs/paper/SDC_RESEARCH_for_SDCShield-负载设计.md)（31 篇文献综合）。
>
> **clang/BiSheng 工具链（HPC 节点）**：BiSheng clang 5.2.0（LLVM 19.1.7，HPCKit）`source <HPCKit>/setvars.sh`、`export TMPDIR=/tmp` 后 meson/ninja 照常；额外启用 SME 测试 `sme_za_tileview_arm`（`-march=armv9-a+sve+sme+sme-f64f64`，gcc 无该支持时探测剔除、不阻断），运行期 HWCAP2_SME + SVL 几何双门控、不满足诚实 SKIP，运行需 HPCKit 环境（libc++ 在 LD_LIBRARY_PATH）。cn23154（HiSilicon 0xd22，608 核）实测 2026-09-28：默认 quality 320 用例，`zstd19 -t 2000 -n 1` 回归 pass。

## 离线与多版本构建

离线工具在 `scripts/offline-build/`。单机原生路径（`download-deps.sh`/`install-deps.sh`/`build.sh`，`.os-version` 强制版本匹配）只支持 **24.03**（`_common.sh` 基线硬编码，对 22.03/20.03 误报）；22.03/20.03 必须走容器路径（D）。

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

> **22.03 / 20.03 适配仅在容器内**：旧工具链（gcc-10、meson 0.59/0.54、binutils 2.34）缺 C++20/23 特性。`container-build.sh` 只读挂载源码树，在可丢弃副本上注入 polyfill、版本宏与 CXXFLAGS（历史上的 sed 补丁已全部收敛到源码/meson option，容器内对源码副本零修改）；ACL 按容器 glibc 自动处置。CI workflow 对 22.03 另传 `-Denable_acl=disabled`——host 源码不动，x86/24.03 参考路径不受影响。

完整依赖与排坑见 [offline-build-dependencies.md](docs/build-deploy-pr-ci/offline-build-dependencies.md)。

### 多版本一键构建与部署（15 个 OS 版本）

`./scripts/offline-build/build-all.sh --all --full --jobs 3` 一次构建全 15 版本（20.03/22.03/24.03 × LTS+SP1~SP4），以纯净容器 full 验证为发布闸门（期望 `PASS: 15`）；`package-release.sh <series> <sp>` 产出 ~20MB 现场 tarball（`run.sh` 自动检测 OS、校验 sha256，`run.sh full` 两段式全核满载、4 个数值敏感测试 `-n 1` 补跑）。一键全流程 `release-all.sh` 是 agent/CI 首选入口：五阶段幂等，check 阶段 **15 × `RESULT: PASS` 且 0 FAIL** 硬闸、不过绝不推送、在 `main` 上拒绝运行；`--skip-base` 跳过 base 拉取。设计与手册见 [multi-version-build-deploy.md](docs/build-deploy-pr-ci/multi-version-build-deploy.md)（设计）、[multi-version-build-deploy-usermanual.md](docs/build-deploy-pr-ci/multi-version-build-deploy-usermanual.md)（用户手册）、[scripts/offline-build/README.md](scripts/offline-build/README.md)。

## 质量保障与 CI 基础设施

### GitHub Actions 每日多 OS 自动验证（`.github/workflows/multi-os-verify.yml`）

CI 哨兵（与上节 `release-all.sh` 发布链路互补）：每天 UTC 04:00（北京时间 12:00）自动触发 + 手动 `workflow_dispatch`；15 个镜像 × 15 job 并行（openEuler 20.03/22.03/24.03 × LTS+SP1~SP4，运行在 `ubuntu-24.04-arm` 托管 aarch64 runner 的 ghcr.io 构建镜像内）。每 job 构建后由 `scripts/gha/verify-params.py` 做全量用例 × 全量参数功能测试（`--quality=-1` 全覆盖、`-n 1/4/8` 三档、openblas `mdim` 扫谱、selftests 正负集），`benchmark.sh` 采跨 OS 基准，`report` job 汇总 15 份结果成「用例 × 版本」矩阵。零执行与全 skip 两层判定：任一阶段连一条结果记录都没有即按失败处理，不静默放过坏门；`all-quality` 阶段允许只有 skip 记录的通过（仍需 `seeds>0` 证据），并发档与 `mdim` 档要求至少 1 个 `pass`（全 skip 判 FAIL）。镜像需先推到 `ghcr.io/wangxumarshall/sdcshield-offline`。详见 [multi-version-build-deploy.md](docs/build-deploy-pr-ci/multi-version-build-deploy.md) 与 [scripts/gha/README.md](scripts/gha/README.md)。

### PR CI 门禁、安全扫描与基准趋势

PR（`pr.yaml`，arm64-only）：lint（tabs/codespell/actionlint）→ 新增代码随机性门禁（`scripts/gha/check-randomization.py`：diff 新增的 `tests/**.cpp` 中自建不可重放 RNG（`mt19937`/`random_device`/`rand()`）为 R1 error，必须改用框架 RNG（种子统一管理、失败可精确重放）；固定 pattern 大表为 R2/R3 warning，`R3-justified:` 注释豁免；diff 扫描本身失败按 exit 3 loudly 报错）→ git 历史/DCO（机器提交豁免）→ `build-cpu`（GCC-arm64：构建 + unittests + selftests + quick 全量跑，结果矩阵入 PR summary）与 `multi-version` 角落三（20.03-LTS/22.03-SP3/24.03-SP3）并行。安全（`security.yaml` + `codeql.yaml`，每周全量 + PR 增量）：zizmor / gitleaks / osv-scanner / CodeQL cpp；`sanitizer.yaml` 每日 UTC 18:43 做 ASan+UBSan 夜巡；`auto-pr.yml` 特性分支首次 push 自动开 PR。基准趋势：每日 cron 的 `benchmark.tsv` 经 github-action-benchmark 推 gh-pages 分支，出跨 commit 趋势图 + 150% 回归告警（观测期不硬门）。网页侧操作清单见 [ci-web-operations.md](docs/build-deploy-pr-ci/ci-web-operations.md)。

### 代码检视机器人（PR 自动检视，聚焦 SDC 激发）

五路检视：四个外部 SaaS（codex、CodeRabbit、PR-Agent、Greptile——配置 `AGENTS.md`、`.coderabbit.yaml`、`.pr_agent.toml`、`.greptile/`，均内嵌 SDC 激发专属检视要点）+ 第 5 个**本地无头 claude 对抗检视**（`scripts/pr_local_review.py`，无需 GitHub 使能）。`bot-coverage.yml` 做非阻塞覆盖度校验：有界轮询四个外部 bot，结果写入 step summary 并评论告警，**恒 exit 0、绝不阻塞合并**。四个外部 bot 均已生效（Greptile 2026-10-13 授权，PR #238 首次实测产出检视）。生效核查方法、Greptile 安装记录与 SDC 检视要点汇总见 [pr-review-bots.md](docs/build-deploy-pr-ci/pr-review-bots.md)；五 bot 检视-修复闭环全流程见 [PR 检视-修复闭环手册](docs/build-deploy-pr-ci/pr-loop.md)。

### task-loop：无人值守任务闭环（issue 驱动开发闭环）

`scripts/task_loop.py`（统一任务编排引擎，`ci_triage.py` 的泛化后继；cron 每 30 分钟驱动 `scan`、每 6 小时驱动 `audit`）：四类触发源——main 分支 CI 红 run、main 增量对抗审查、会话 enqueue（`/dev-loop`）、GitHub 手建 issue——统一进入 `agent-task` 队列，单飞 headless agent 在独立克隆里修复/实现 → push `fix|feat/issue-<N>` 分支 → 自开 PR（`Closes #N`）→ 复用 pr-loop 五 bot 检视闭环 → READY_TO_MERGE 时 @人工 merge。**merge 永远在人**；标签协议/单飞/熔断/kill switch 等运维细节见 [task-loop.md](docs/build-deploy-pr-ci/task-loop.md)（前代手册见 [ci-triage.md](docs/build-deploy-pr-ci/ci-triage.md)，历史保留）。

### 15 镜像全严格选项稳定性验证（`scripts/lts-stability/`）

本地 podman 上的最严格验证入口（GHA 日报的本地超集）：15 镜像 × 29 条严格选项矩阵 × 全部测试用例（含 `--quality=-1` SKIP 级、三 RNG 引擎、cpuset 跨 NUMA、全部 `-O` 旋钮、selftests、`--on-crash=context` 等）。每镜像容器内原生构建全功能二进制后跑严格矩阵，全部 15 PASS 才 exit 0——2026-09-17 全链路实测 **15/15 PASS**（过程中发现并修复 8 个跨版本软件 bug）。用法与设计见 [scripts/lts-stability/README.md](scripts/lts-stability/README.md)。

## OpenSSL SHA（`openssl_sha`，默认构建）

`openssl_sha` 经 OpenSSL 计算 SHA-256/384/512 与 golden 比对。默认 `ssl_link_type=dynamic`，优先 vendored OpenSSL（`third-party/openssl/`），`install/` 缺失回退系统 `libcrypto`，两者皆无则跳过 SSL 测试；构建方法见「从源码构建」。

| `ssl_link_type` | 行为 |
|---|---|
| `dynamic`（默认）| 优先 vendored OpenSSL，回退系统 `libcrypto`，构建期动态链接 |
| `static` | 同上，链接静态 `libcrypto` |
| `loaded` | 运行期 `dlopen()` 加载 `libcrypto` |
| `none` | 禁用 OpenSSL；无 `openssl_sha` |

## Vendored 计算库（third-party/，SDC 负载扩展）

仓库 vendor 了高优化度计算库作为 SDC 压测负载（2026-09-15 起）——文献结论（见 [SDC_RESEARCH_for_SDCShield-负载设计.md](docs/paper/SDC_RESEARCH_for_SDCShield-负载设计.md)，31 篇论文综合）：高度优化的第三方库等价于人工写好的数据流型 FU 饱和序列，向量 FMA/GEMM 是第一大 SDC 源。每个目录保留上游官方 tag tarball 溯源，`build.sh` 幂等构建到 `install/`；缺席时的优雅降级见「从源码构建」。

| 目录 | 内容 | 产出 | 测试 |
|---|---|---|---|
| `third-party/openssl/` | OpenSSL 3.5.0 | 静态 `libcrypto.a` | `openssl_sha`（SHA-2）+ `openssl_sha3`（SHA-3/SHAKE Keccak）+ `openssl_sm3sm4`（国密 SM3 摘要 + SM4-CBC 加解密往返）+ 70 个 `ipsec_*`（46 NEON + 24 SVE 移植变体，默认启用） |
| `third-party/openblas/` | OpenBLAS 0.3.29（TSV110 内核，单线程 + USE_LOCKING=1） | 静态 `libopenblas.a` | `openblas_dgemm` / `openblas_sgemm` / `openblas_zgemm` / `openblas_cgemm`（NEON FMA 微内核，每迭代 copy/compute/verify）+ `openblas_lu`（LAPACK dgesv 主元分解） |
| `third-party/sleef/` | SLEEF 3.9.0（TLFLOAT=OFF） | 静态 `libsleef.a` | `sleef_neon`（NEON 多项式 FMA 链：sin/cos/exp/log double+float × u10/u35 双精度档 + asin/atan/cbrt/log10/sinh/tanh/pow 函数族，足迹旋钮 `-O sleef_neon.nelems=N`）；`sleef_sve`（SVE 变体，double+float × u10/u35，**需 SVE 硬件**——无 SVE 的机器如鲲鹏 920 干净 skip：`CpuNotSupported`） |
| `third-party/pocketfft/` | pocketfft C 版（BSD-3，头文件+源码直接入库，无需 build.sh） | 编入测试库 | `pocketfft_fft`（位反转抽取重排 + 旋转因子 FMA 蝶形链） |
| `third-party/isa-l/` | Intel isa-l 2.32.1（BSD-3，Makefile.unx 路径，aarch64 无需 autoconf/nasm） | 静态 `libisal.a` | `isal_igzip`（igzip deflate/inflate 往返，aarch64 汇编内核）；10 个 `isal_crc*`（NEON pmull CRC16/32/64）。`install/` 缺失时**回退系统 `libisal`**（openssl 式两级 gate） |

OpenBLAS 单线程（`USE_THREAD=0`）是刻意设计：库内多线程引入非确定性归约顺序（假阳性）；`USE_LOCKING=1` 让"每核一 worker"并发调 `cblas_*gemm` 时内部 packing 缓冲池不互踩（实测无锁 8 线程即出错配、加锁后 128 核 30s 零错配），锁只保护缓冲表元数据、不改计算结果。

**全核压测终验**（实测 8806/8806 迭代全 pass，2026-09-15，鲲鹏 920 128 核）：

```bash
./builddir/sdcshield -e openblas_dgemm,openblas_sgemm,openblas_zgemm,sleef_neon,isal_igzip,pocketfft_fft,openssl_sha -t 30000
```

**GEMM 尺寸/形态扫谱**：`-O <testid>.mdim=N`（默认内存自适应，可扫 L1D→L2→LLC→DRAM→NUMA 工作集）+ `.transab=0..3`（NN/NT/TN/TT packing 相位）+ `.beta_permille`（β≠0 命中 C 读改写路径）——参数语义、合法域与两个易错点见「Test knob」总表。

**每测试每档工作集足迹**（每线程 scratch = 3 矩阵；矩阵字节数 = mdim² × 元素宽度）：

| 档位 | dgemm (double) | sgemm (float) | cgemm (complex float) | zgemm (complex double) | cache 域 |
|---|---|---|---|---|---|
| `mdim=64` | 32KB/矩阵 | 16KB | 32KB | 64KB | L1D |
| `mdim=256` | 512KB | 256KB | 512KB | 1MB | L2 |
| `mdim=512` | 2MB | 1MB | 2MB | 4MB | LLC |
| `mdim=1024` | 8MB | 4MB | 8MB | 16MB | DRAM |
| `mdim=2048` | 32MB | 16MB | 32MB | 64MB | DRAM/远端 NUMA |
| `mdim=4096` | 128MB | 64MB | 128MB | 256MB | 大页/NUMA 交错 |

（cgemm 元素 = 交错 (re,im) float 对 = 8 字节；zgemm = complex double = 16 字节。）

**全核内存预算表**（scratch 合计 = 3 矩阵 × 核数；选档前先对照主机内存）：

| 核数 | dgemm@1024 | dgemm@4096 | sgemm@4096 | cgemm@4096 | zgemm@4096 |
|---|---|---|---|---|---|
| 128 核 | ~3GB | ~49GB | ~25GB | ~49GB | ~98GB |
| 512 核 | ~13GB | ~197GB | ~98GB | ~197GB | ~393GB |
| 2048 核 | ~50GB | ~786GB | ~393GB | ~786GB | ~1.6TB |

（zgemm 内存紧张时推荐档 **512**；内存不足时 OOM 走 `report_fail_msg` fail-loudly，不是静默或崩溃。）

**推荐战役模式**：

```bash
# 模式 A：谱系扫档（cache 域逐层压测；openblas 单独跑避免混跑 cache 污染）
for m in 64 256 512 1024; do
  ./builddir/sdcshield -O openblas_dgemm.mdim=$m -O openblas_sgemm.mdim=$m \
    -O openblas_zgemm.mdim=$m -e openblas_dgemm,openblas_sgemm,openblas_zgemm -t 15m
done

# 模式 B：混合套件（多样性 = 检出率；默认内存自适应档，与全部其他测试同跑）
./builddir/sdcshield -e openblas_dgemm,openblas_sgemm,openblas_zgemm,zstd19,sleef_neon,isal_igzip,pocketfft_fft,openssl_sha -t 30m

# 模式 C：大矩阵深压（大内存主机；4096 档单迭代分钟级，必要时 -n 限并发控内存）
./builddir/sdcshield -O openblas_dgemm.mdim=4096 -e openblas_dgemm -t 2h

# 模式 E：单命令 knob 全谱（每个 -O 只作用于它前缀的测试，无前缀匹配的按默认参数跑
#         ——多样性轮 + 参数扫谱二合一）
./builddir/sdcshield \
    -e openblas_dgemm,openblas_sgemm,openblas_zgemm,openblas_lu,sleef_neon,pocketfft_fft,isal_igzip,eigen_svd_cdouble_sve,ipsec_aes128_cbc_hmac_sha1_sse \
    -O openblas_dgemm.mdim=1024 -O openblas_dgemm.transab=1 -O openblas_dgemm.beta_permille=500 \
    -O openblas_lu.n=1024 -O sleef_neon.nelems=16384 -O pocketfft_fft.n=6144 -O isal_igzip.level=2 \
    -t 30m
```

**模式 D：全谱战役脚本 `scripts/run/run_sdc_spectrum.sh`**——把 A/B/C 与全部 test knob 合并为一条命令的两阶段战役：阶段 1 谱系广域扫（GEMM 尺寸谱 + 形态谱、SLEEF 足迹谱、FFT 因子谱（pow2/Bluestein 质数/混合 radix）、压缩 level 谱、加密/哈希全家族 + 混合多样性轮）；阶段 2 深驻留（`--max-test-loop-count=0` 关闭 fracturing，固定 seed 持续运行）。时长经 `SWEEP_TIME`/`DWELL_TIME` 覆盖，日志按阶段/档位存 `./sdc_spectrum_<时间戳>/`。

```bash
bash scripts/run/run_sdc_spectrum.sh                    # 默认：15m/档扫谱 + 2h 深驻留
SWEEP_TIME=30s DWELL_TIME=1m bash scripts/run/run_sdc_spectrum.sh   # 冒烟（约 10 分钟）
```

**模式 F：24h+ 压测执行器 `scripts/run/run_sdc_campaign.sh`**——把模式 D 扩展为 24h+ 持续测试（持续高频测试比快照式多数量级地抓出缺陷）：P1 全量广域扫 → P2 文献优先级加权 soak → P3 固定 seed 深驻留 → P4×N 循环（随机序重扫 + NUMA 拓扑/并发档轮换）。全程 ipmitool/EDAC/SEL 环境监测，fail-continue + 自动失败分类（known_benign_ulp / full_core_only / transient / sdc_suspect）+ 可复现嫌疑自动逐核二分（CORE179 式定位），`.done` 阶段标记断点续跑：

```bash
nohup setsid bash scripts/run/run_sdc_campaign.sh > campaign.log 2>&1 &   # 24h+ 正式
SMOKE=1 bash scripts/run/run_sdc_campaign.sh                            # 冒烟（~10min）
```

**模式 0：单板机器扫描 `scripts/run/sdc_machine_scan.sh`**——压测战役第 0 步，新单板可直接复用：一次性采集 DMI/BMC 传感器/SEL/NUMA/RAS/软件栈，产出 `capabilities.env` 能力开关，自动标记硬件异常（SEL 周期故障、无本地内存 node、无 SVE/cpufreq 等），战役参数据此适配：

```bash
SUDO_PW=<密码> bash scripts/run/sdc_machine_scan.sh          # root 全量模式
bash scripts/run/sdc_machine_scan.sh                         # 无 root 降级模式
```

模式选择依据：CORE179 探针证明 store→reload 的 cache-domain 与跨线模式是触发判别条件（模式 A 逐层覆盖）；文献共识"负载多样性即检出率"（模式 B/D）；大矩阵档把分块 GEMM 推进 DRAM 与 NUMA 远端域（模式 C）；固定 seed 长驻留提升单模式统计采样深度（模式 F）。mdim>256 档在固定时间窗内迭代数按 mdim³ 骤减——统计采样请加长 `-t`。

## 测试用例与检测能力

### ARM64 spec 策略与默认用例集（2026-10-27 起）

sdcshield 启动时探测一次芯片规格（`arm64_spec_get()`：SVE/SVE2/SME/SME2 扩展位 + **架构最大** VL/SVL 档位——向内核请求天花板并读回实际授予值；"当前 VL" ≠ "最大 VL"，sysctl 可能压低默认值）。默认测试集在**选择期**按每个用例声明的 `arm64_requires` 与 `arm64_min_vl_bits`（`framework/sandstone.h`）过滤，只保留芯片能执行的用例——默认用例数 **host-dependent**：SVE1 主机实测 486 PROD（`--quality=0` 496、`--quality=-1` 500；2026-11-02 迁移 +21 PROD 后待重测，推算 507/517/521）；无 SVE 鲲鹏 920 **本机 2026-11-18 实测 534 PROD（`--quality=0` 643、`--quality=-1` 647；= 2026-11-10 口径 519/628/632 + 11-10 后累积 +15：`tests/cpu/L3_cache_920/` FP64 扩展 12（L3 域 f64x2 孪生）+ `adaptive_l3_sdc` 1（L3 域自适应 21 组合，含本机 21 臂标定全零负空间专档 `docs/20261118/adaptive_l3_sweep21/RESULTS.md`）+ 各会话 2；实测为准）；完整时序链见「用例质量分级」footnote）**。

- `-e` / `--test-list-file` **旁路过滤**：显式请求的用例仍会注册，能力不足时诚实 SKIP（附理由）。
- **控制线程**在 `test_init` 前 pin 到芯片最大档（aux 线程与 worker 继承，init golden 与 run lane 档位一致）；每个 SVE/SVE2 worker 重新 pin 并校验读回——pin 被拒 = SKIP，读回不符 = FAIL。
- `SDCSHIELD_ARM64_SPEC_MAX_VL=<bits>` 压低假定的最大档（只降不升，未实现档回退内核授予值）。
- 用例规格声明覆盖由 `scripts/check-arm64-requires.py` 保证诚实。

### SVE 用例全 VL 验证矩阵（2026-10-30 收口）

190 个 SVE 用例（tag∪name 枚举口径）× 128/256/512-bit 三档真实证据矩阵（570 格）：**560 pass（469 直绿 + 91 有界几何）+ 1 合法 SKIP + 9 gem5 边界格 + 0 FAIL**，本轮运动零测试代码缺陷（冻结基线上另有 5 个测试携带 PR #316 检视事后修复的越界缺陷，其 pass 是弱证据，见 matrix.md §4 † 脚注）。三档采集：128 = `SDCSHIELD_ARM64_SPEC_MAX_VL=128` 进程内降档、256 = 原生（各 2 seeds）、512 = gem5 SE 跑真二进制（功能正确性语义，压力维度由 native 腿承担）。边界注记：① 唯一 SKIP = `sve512_fmmla_arm@128`——F64MM 在 VL<256 架构性 UNDEFINED（实测 SIGILL）；② 9 个 gem5 边界格（2×PMULL64 GCM + 3×DC-CIVAC + 4×仿真速超时）native 两档全绿。证据钉在冻结基线 `12ccda540fd4`：[matrix.md](docs/superpowers/inventory/sve-all-vl-campaign/matrix.md)（机械生成，重跑即复现）+ 逐腿分诊 `triage-{128,256,512}.md`（同目录）。

| 检测域 | 代表用例 | 检测能力 |
|---|---|---|
| 内存 / 拷贝 | `memcpy_l{1d,2,3}_cache_size`、`memcpy_rewr`、`mem_disambiguation`、`mmu_stress_arm`、`random_access_sweep` | 各级缓存带宽、store-to-load 转发、跨行一致性与内存序、TLB 扰动、大 mmap 随机偏移/块长扫掠（NEON RMW 字节精确校验，knob: mode/valmode/size_mb） |
| 缓存 / 互联 | `cachebounce`、`mesh_upi_*`（46，含 18 个 SVE 变体） | cache line 弹跳、CLFLUSH 压力、MESH/UPI 多核读写协同 |
| 锁 / 原子 | `lock*`、`lockless_cmpxchg*`、`atomic_simd_*`、`spinlock_*`（31） | 锁指令、无锁 cmpxchg、128/256/512 位原子、自旋锁各类竞争（ARM64 atomic） |
| 向量 / SIMD | `swizzle`、`insert_extract`、`kreg1`–`kreg9`、`gather*` | NEON 排列/插入抽取、掩码寄存器（x86 k-reg 仿真）、gather/scatter |
| FMA / 浮点 | `fma`、`fma_patterns_*`、`fma_tail*`、`fpu_special_values` | FMA 模式与尾数精度穷举、特殊值逐字节 golden 比对 |
| 算术 / 大整数 | `adcx`、`adox`、`adcxlong`、`adcx_arm`、`bigint_mulx_arm`、`gmp_big*` | 进位/溢出链、GMP 大整数乘加、高汉明距离操作数压满加法器 |
| CRC / 校验 | `crc32`、`isal_crc{32,64}_*`、`zpclmul*` | `crc32` 指令、isa-l CRC32/CRC64 各标准、zlib PCLMUL 折叠 |
| 压缩 | `zlib*`、`zstd*`、`zfuzz`、`isal_igzip` | zlib/zstd 压缩-解压往返、各级别、fuzz、isa-l deflate/inflate 往返（`-O isal_igzip.level=0..3` 扫四套 match-finder 数据结构） |
| 线性代数（Eigen） | `eigen_gemm_*`、`eigen_sparse`、`eigen_svd*`（含 `_cdouble_sve`） | GEMM、稀疏 Cholesky、SVD（BDCSVD/Jacobi）施压 FMA/向量 |
| 线性代数（OpenBLAS） | `openblas_dgemm`、`openblas_sgemm`、`openblas_zgemm`、`openblas_cgemm`、`openblas_lu` | NEON FMA 微内核 GEMM 四种 lane 组织，每迭代 copy/compute/verify；`openblas_lu` = LAPACK dgesv 主元分支 + 三重 golden 比对；knob 语义见「Test knob」总表 |
| 超越函数（SLEEF） | `sleef_neon`、`sleef_sve` | 多项式 FMA 依赖链 × 宽向量，逐字节 golden 比对；u10 + u35 双精度档 + asin/atan/cbrt/log10/sinh/tanh/pow 函数族；`sleef_sve` 需 SVE 硬件（无则干净 skip），all-VL 128..1024-bit 真 VLA；足迹旋钮见「Test knob」总表 |
| FFT（pocketfft） | `pocketfft_fft` | 复数 FFT + 实数 rfft 前向 golden 比对：位反转抽取重排 + 旋转因子 FMA 蝶形链；`-O pocketfft_fft.n=N` 扫因子谱系（pow2 / Bluestein 质数 / 混合 radix） |
| IPSec / 密码 | `ipsec_*`（70，含 24 个 SVE 移植变体） | AES-CBC/CTR/GCM、HMAC-SHA1/2、XCBC/CMAC/3DES-DOCSIS 于 NEON / SVE |
| OpenSSL SHA | `openssl_sha`、`openssl_sha3`、`openssl_sm3sm4` | SHA-2 加法链、SHA-3/SHAKE Keccak 置换、SM3 摘要 + SM4-CBC 加解密往返（国密整数通路）vs golden |
| ARM 加密扩展 | `arm_crypto` | AES（AESE/AESMC）crypto 数据通路 |
| 虚拟化 / 系统寄存器 | `vmx_vmexit_*`、`vmxmsr` | guest 触发 vmexit 退出路径一致性 |
| ARM64 SDC 专项 | `arm64_sdc`、`power_virus_dit`、`ooo_dep_chain_arm`、`lsu_store_forward_arm`、`l2c_cross_cache_line_arm`、`mmu_split_tlb_arm`、`sve512_gather_scatter_arm`、`sve512_f64_chain_arm`、`sve512_f64_special_arm`、`sve512_f32_chain_arm`、`sve512_stencil_axis_arm`、`sve512_xlswave_ctx_arm`、`sve512_f64_chain_svd`、`sve512_f64_special_svd`、`sve512_f32_chain_svd`、`sve512_gather_scatter_svd`、`libomp_ctx_arm`、`sme_za_tileview_arm` | di/dt 电压骤降、乱序依赖链、LSU 转发、L2 跨行、MMU/TLB/页表遍历器、SVE 全向量长 f64/f32 FMLA 串行链与特殊值链（NaN/Inf 类别比对）、SVE gather/scatter 间接索引数据通路、stencil 轴核与 XLSDFT 复合上下文触发配方复现器（三阶段字节精确 memcmp + VA[63:48] 地址金丝雀）、SVD 尺度工作集变体、libomp fork/join 上下文仿真（三层校验）、SME ZA tile 语义压力（需 clang +sme 工具链）。SVE/SME 类按真实能力门控：无 SVE 诚实 SKIP，all-VL 128..1024-bit 自适应；无 HWCAP2_SME 或超范围 SVL 诚实 SKIP |
| SDC 激发增强套件（2026-09-17 研究落地，2026-10-12 合并） | `sve512_nt_reload_arm`、`sve512_fmmla_arm`、`sve2_cross_precision_arm`、`sve512_fcmla_arm`、`sve512_pred_ops_arm`、`sve_fpcr_cartesian_arm`、`fpcr_rounding_cartesian_arm`、`power_virus_dit_sve_arm`、`mesh_upi_avx512_symm_f64`、`sme_fmopa_za_arm` + 既有测试增强 | 针对已知鲲鹏故障签名与 SEVI(ASPLOS'26)/ITHICA 研究结论定向编写：**nt_reload** = NUMA3 VA 通路非时间访存（ld1rd/ldnt1d/stnt1d）；**FMMLA/跨精度/FCMLA/谓词** = 此前零指令级覆盖的 SVE2 数据通路（矩阵外积、FP16/BF16 加宽乘加、复数 FMA 全旋转、谓词网络）；**FPCR 笛卡尔** = 舍入模式 × FZ 8 配档全扫；**power_virus SVE** = 全宽 di/dt 病毒；**mesh f64** = 首个 FP mesh 变体；**SME FMOPA** = 纯 inline asm 的 ZA 外积链。既有增强：sve512 链族值域旋钮 `-O <test>.bounded=0\|1`（SEVI Obs.19 有界/无界域 SDC 频率差 245×）。SVE/SME 类在 920 上干净 skip，byte-exact golden 真机首验在 cn23154（SVE512 故障机） |
| ARM64 触发配方 | `agu_stress_2src`、`neon_rot_2src`、`neon_rot_ldr_at_top_rowmajor`、`movbe` 系列（`movbe`、`movbe_dump`、11 个 `movbe_dump_probe_*`） | AGU 吞吐施压（2 源加载 + 旋转 ALU + store/reload/store）、core-179 配方的 NEON 向量通路判别、ldr_at_top 扫描顺序变体（升/降序交替）、core-179 字节交换往返触发探针组 |
| opendcdiag 研究仓迁移组（2026-11-02，121 用例） | `spinlock_stale_read{,_xchg}`、`locv_a{1,2,3}_mrn_*`、`locv_b{0,1,2}_mrn_pair_*`、`arm0102_*`（10）、`eigen_gemm_double_ab_split`、`aor_{math,string}`、`cbw_arm`、`sdc_fuzz_000`–`sdc_fuzz_099` | core179 读路径缺陷类差分探针（CAS vs xchg 臂定位故障层）；LOCv 战役 A/B 六臂（bypass-gap / split-consumer / pair 假共享）；arm-0102 触发配方变体（fma/ins_ext/kreg×6/swizzle）；Eigen GEMM 内外校验判别；Arm Optimized Routines sincospi/memcpy vs golden（vendored `third-party/arm-opt` 静态链入）；L1 fuzzing 生成的 core179 复现器方阵（Eigen 稀疏 Cholesky，BETA 级 + 配套生成器 `scripts/generate_sdc_fuzz_cases.py`） |
| arm_add 写可见性探针组（2026-11-02，7 用例） | `store_visibility_cross_class`、`store_lost_write_detect`、`barrier_after_lock_store`、`low_load_cache_pressure`、`str_instruction_stress`、`cache_stress_clflush`、`cache_stress_memcpy` | str 失效故障报告五维探针：跨一致性域写可见性（屏障前后双读）、哨兵三层回读丢写阶梯（同核→跨核→dsb 后）、锁+屏障路径丢写（真 ldaxr/stxr 接力）、低占空比跨核扫（5-30% knob `duty_us`，补 633 无低负载覆盖的空缺）、四宽度 str 密写+完成态快照校验（seqlock 式防撕裂）；633 #210/#211 ARM64 等价对照对（DC CIVAC 冲刷干扰复制 vs 纯复制）。全部注入反验实证（模拟丢写 7/7 FAIL 检出） |
| SVE 版 avx53 套件（`tests/cpu/sve/` 中 53 个 avx 命名移植；该目录共 172 个 SVE 用例） | `fma_tail_sve{,_wide}`、`fmatail_sve{,_wide}`×4 对、`fma_patterns_sve_wide_{ps,pd}`、`mesh_upi_sve_*` 18 个、`eigen_svd_bidiag_sve`、`ipsec_*_sve{,_wide}` 24 个 | 53 个 avx 命名 NEON 测试的 SVE1 移植（负载环境与 golden 模式全保留；命名 `_avx/_avx2→_sve`、`_avx512→_sve_wide`）。FMA 10 + Mesh 18 = 纯指令替换（SVE 谓词访存）；eigen 1 = 自写 Householder 双对角化 SVE 负载；ipsec 17 = EVP 加密 + 多流 SVE HMAC（SHA 内核与 OpenSSL 全向量比对）+ 7 个 EVP-only（GCM/CMAC/XCBC）如实标注。构建于 `tests_sve_avx53*` 库（`-march=armv8.2-a+sve`，init 探 HWCAP_SVE 干净 skip） |
| IST 硬件自检 | `ist`、`ist_array`、`ist_sbaf` | ARM64 In-Silicon Test（当前 placeholder，见下表） |

### 用例质量分级

| 级别 | 名称 | 运行条件 | 实有数量（源码全量，选择期过滤前） |
|---|---|---|---|
| -1 | SKIP | `quality >= -1` | 4 |
| 0 | BETA | `quality >= 0` | 110 |
| 2 | PROD（默认）| `quality >= 2` | 538 |
| | **合计** | | **652** |

> 上表为 ARM64 gcc 构建的源码全量口径，含 2026-11-02 opendcdiag 研究仓迁移增量 +121（21 PROD + 100 BETA `sdc_fuzz_*`）+ 同日 arm_add 新探针 +7 PROD + _neon 变体 +14 PROD + 633 cache 族新测试 +9 PROD（2026-11-01 复核口径 501 = 487 PROD + 10 BETA + 4 SKIP 基础上叠加）。运行时实际数量随芯片 spec 过滤：SVE1 主机 2026-11-01 实测 490 PROD（q0/q-1 = 496/500），同日 issue #374 将 `sched_storm×4`（内核调度刺激/负对照，无 SDC 检出语义）自 PROD 降级 BETA 后复核 **486 PROD**（q0/q-1 集合不变；迁移后推算 507/517/521，待该主机重测）；无 SVE 鲲鹏 920 **2026-11-02 本机实测 439 PROD / 547 q0 / 551 q-1**（= 同日迁移后 409/517/521 + arm_add 7 + _neon 14 + cache 族 9 PROD；旧推算 299 已过时——2026-10-31 bulk 注册 +71 等未计入，这正是此前"待无 SVE 主机实测复核"的兑现）。cn23154 复现分支（clang/libc++，含 SME 用例）2026-09-28 实测：默认 322，`--quality=-1` 全量 333。

- **BETA（`--quality=0`）**：`arm64_sdc`、`arm64_sdc_sve`、`arm_crypto`、`ist_sbaf`、`neon_add`、`neon_add_sve`、`sched_storm×4`、`sdc_fuzz_000`–`sdc_fuzz_099`（100 个 core179 复现器，2026-11-02 迁移）
- **SKIP（`--quality=-1`）**：`eigen_svd_jacobi`、`eigen_svd_jacobi_cdouble`、`eigen_svd_jacobi_double`、`eigen_svd_jacobi_fvectors`

### 占位用例的诚实跳过

暂未实现的特性用例返回 `EXIT_SKIP` 并附理由（而非 `EXIT_SUCCESS` 伪装通过）。实测 `skip-reason`：

| 用例 | 跳过理由 |
|---|---|
| `ist` / `ist_array` / `ist_sbaf` | `to be implemented (placeholder): ARM64 In-Silicon Test (IST) backend not yet available; test reserved as the counterpart of Intel IFS` |

> `mce_check` 是真实 EDAC 后端测试（统计 `/proc/interrupts` 的 EDAC `ce/ue_count`），实测 `exit: pass`，非 placeholder。

### `smi_count`（ARM64）：CNTVCT 时序毛刺探针

`smi_count` 是真实探针（2026-10-31，issue #7）：每个 CPU 一 worker 在测试窗口内连续读架构计数器 `CNTVCT_EL0`（用户态 `mrs`，~25ns/读），统计相邻读间隔 >100µs 的"被偷走时间"事件（次数、>1ms 严重数、最大值、累计值），非零才经 `log_platform_message` 报告（INFO 级，`-vv` 可见），永不失败。**语义边界**：探针无法区分毛刺来源（固件 SMC/EL3、虚拟化偷取、中断风暴、内核调度抢占），与 x86 的 MSR 0x34（仅计 SMM、覆盖整个运行期）语义不同；ARM64 版观察窗口是本测试自身运行窗口（fork 模型下 preinit 起的后台监控线程无法存活到子进程）。裸机 ARM64 不存在内核可见的固件抢占计数器——本探针是可用的最接近等价物，定位是**解释其它测试时序抖动的逐 CPU 噪声地图**，不是固件事件计数器。若内核禁用 EL0 计数器访问（`CNTKCTL_EL1.EL0VCTEN=0`），init 的 SIGILL 守卫会诚实 SKIP；**`--inject-idle` 下本用例计数无效**（框架注入的 sleep 会被计为毛刺）。x86-64 版仍走 MSR 0x34（零改动）。

**能力门控的诚实跳过**：17 个多流 HMAC 的 ipsec SVE 用例（`*_hmac_sha*` / `*_sha2_*`）每条 HMAC 流占一个 SVE lane，**流数 = 运行时 lane 数**（sha1/224/256 为 `svcntw()` 4..32 流、384/512 为 `svcntd()` 2..16 流，128..1024-bit 全档自适应），经框架层 `arm64_require_sve_vl` 门控；`data_size` knob 上限钳制到 HMAC 组装缓冲容量。无 SVE 的硬件在框架级守卫处跳过；流数参数化形态经 gem5-fi `sve_sha_dual` 四档 16-run 实证（FIPS 标量参考逐流字节级比对）。

### `memcpy_rewr`：MPSC memcpy 一致性压测

GlusterFS IOT 调度器衍生的多生产者/单消费者队列 memcpy 一致性压测（ARM64 专属）：producer 经 4 优先级链表队列投递，consumer 对线程私有 `mem_info_s` 的 b1..b5 做五路 memcpy 并逐字节比对——拷贝不崩溃但不一致即为 SDC。无 conf 文件时自动退化成原版独立工具的默认模式（不 skip）。

**conf 模式**（`tests/cpu/memory/memcpy_rewr_strategies.conf`，仓库默认提供）——`SANDSTONE_STRATEGY_INDEX` 轮转选策略，`SANDSTONE_STRATEGY_CONF` 覆盖路径：

```bash
for i in 0 1 2; do SANDSTONE_STRATEGY_INDEX=$i ./builddir/sdcshield -e memcpy_rewr -t 5000; done
```

| INDEX | 策略 | 拓扑意图 | 推荐场景 |
|---|---|---|---|
| 0 | `numa_cross_node`（`numa_split`） | 跨 NUMA 节点 barrage，数据移动穿跨 die 一致性 Fabric | 多路/多 NUMA 机器的互联压力 |
| 1 | `same_die_l3_brawl`（`die_even_odd`） | 同 die/cluster 内 even/odd 核对打，流量锁死单 L3 slice + ring bus | 单 die 内 L3/环网压力 |
| 2 | `few_producer_many_consumer_storm`（`first_n_producers`，`producer_count=4`） | 少写多读，诱发目录控制器 Invalidate 广播风暴 | snoop/目录控制器压力 |

**默认模式**（conf 缺席或部署机无源码树）——全核并行自适应：`block_size = clamp(MemAvailable×50% ÷ 线程数 ÷ 6, 64 KiB, 2 MiB)`、`producer_count = max(1, 核数/12)`（对齐原版参考参数 p 0..3 / c 0..47）；conf 存在但损坏/不可读仍然 loud skip。参考实测（Kunpeng 920 128 核，MemAvailable ≈ 22.5 GB）：默认模式 block_size 封顶 2 MiB、峰值 RSS ≈ 1.52 GiB 无 OOM；conf 三策略全核（`-t 3000`）三档均 `result: pass`。

## 运行测试

```console
./builddir/sdcshield --list-tests                        # 列 PROD 用例（默认）
./builddir/sdcshield -e zstd19 -t 5000                   # 单测试，5 秒，全核
./builddir/sdcshield -e zstd19 -t 5000 -n 1             # 单线程，确定性口径（回归标准命令）
./builddir/sdcshield --quality=0 -e arm64_sdc -t 5000   # 跑 BETA 用例
./builddir/sdcshield --quality=-1 -e eigen_svd_jacobi   # 跑 SKIP 用例
./builddir/sdcshield -l                                  # 用例 + 描述 + 分组
./builddir/sdcshield --list-groups                       # @compression / @ipsec / @math
./builddir/sdcshield --dump-cpu-info                     # CPU + 特性 + 拓扑 + SVE/SME 向量长度(aarch64)
./builddir/sdcshield --on-crash=context -e selftest_sigsegv -vv   # 崩溃回溯
./builddir/sdcshield --fatal-skips ...                   # skip 也按致命处理（不静默吞掉）
```

`--dump-cpu-info` 在 aarch64 额外输出 **SVE/SME 向量长度**（per-thread 运行量，与进程级 feature 位图独立，见 `docs/architecture.md`）。`scripts/run/` 狩猎脚本遵循**失败留证**口径：逐用例 `-o` 落盘 YAML + `--fatal-skips`，日志一律保留供事后审计——`exit_rc=0` 但日志含 `miscompare` 记为 `sdc_log`，不误报为 `pass`。

### Test knob（`-O`，运行期测试参数）

部分测试支持**运行期参数旋钮**——不重编译即可改变矩阵尺寸、工作集大小、压缩级别等压力参数，参数值写入 YAML 日志（可复现）。语法：`-O <测试ID>.<参数名>=<值>`（可重复传多个）。

```console
# 默认内存自适应档 → 4096 档（806MB 工作集，压到 DRAM/NUMA 远端域）
./builddir/sdcshield -e openblas_dgemm -O openblas_dgemm.mdim=4096 -t 30m

# 形态扫谱：转置组合 + β 读改写路径
./builddir/sdcshield -e openblas_zgemm -O openblas_zgemm.transab=3 -O openblas_zgemm.beta_permille=500 -t 15m

# ipsec 载荷 1024B→16MB：AES/3DES/SHA 数据路径从 L1 压到 DRAM（46 个 NEON ipsec 用例同款参数名 .datasize；24 个 SVE 变体为 .data_size，域 1024..约 1900 受 HMAC 组装缓冲约束）
./builddir/sdcshield -e ipsec_aes128_cbc_hmac_sha1_sse -O ipsec_aes128_cbc_hmac_sha1_sse.datasize=16777216 -t 5000

# SLEEF 足迹扩到 6MB（默认内存自适应；显式 262144 元素 ≈ 6MB）
./builddir/sdcshield -e sleef_neon -O sleef_neon.nelems=262144 -t 30m

# FFT 走 Bluestein 质数路径（默认 4096 是纯 radix-4/2）
./builddir/sdcshield -e pocketfft_fft -O pocketfft_fft.n=4099 -t 15m
```

**两个易错点**：① 必须带测试 ID 前缀——框架以 `<测试ID>.<参数名>` 为查找键（`framework/test_knobs.cpp`），裸 `-O mdim=4096` 会被**静默忽略**并回退默认值（`-v` 可核对实际生效值）；② 越界行为按测试分两种：openblas/sleef/eigen SVE 族**告警并 clamp**（非致命），`isal_igzip.level`、`pocketfft_fft.n` 等仍 **fail-loudly**（静默 clamp 会跑出操作员没要的 match-finder 相位）。

当前支持 knob 的测试全集（越界行为：标注 clamp 的为告警取整/钳制，其余 fail-loudly）：

| 测试 | 参数 | 合法域 | 默认 | 作用（档位→压力域） |
|---|---|---|---|---|
| `openblas_{d,s,z,c}gemm` | `.mdim` | 16..16384（越界告警并 clamp） | **内存自适应**（[128,8192]，85% 物理内存预算） | 矩阵尺寸：64→L1 / 256→L2 / 1024→L3 / 2048+→DRAM |
| `openblas_{d,s,z,c}gemm` | `.transab` | 0..3 | 0 | NN/NT/TN/TT（OpenBLAS 不同 packing 例程） |
| `openblas_{d,s,z,c}gemm` | `.beta_permille` | 0..10⁶ | 0 | β=P/1000；β≠0 命中 C 读改写路径 |
| `openblas_lu` | `.n` | 16..16384（clamp） | 内存自适应（[128,8192]） | LU 分解矩阵尺寸（64→32KB … 2048→32MB） |
| `sleef_neon` / `sleef_sve` | `.nelems` | 128..67108864（4 的倍数，非倍数告警取整） | 内存自适应 | 每函数族元素数（工作集 128KB→GB 级） |
| `pocketfft_fft` | `.n` | 512/1024/2048/4096/8192/16384（pow2）/ 4099 / **8191**（质数→Bluestein）/ 6144 / 10000（白名单外 fail-loudly） | 4096 | FFT 点数与因数结构（pow2=radix-4/2，4099/8191=Bluestein 质数） |
| `isal_igzip` | `.level` | 0..3（fail-loudly） | 1 | deflate 级别（不同匹配查找器；2/3 有独立 level_buf） |
| `zstd` / `zstd_aaa` | `.level` / `.maxbuffersize` | 1..22 / 4096.. | 按变体 | 压缩级别 / 每迭代缓冲上限 |
| `zstd1` / `zstd19` | `.maxbuffersize` | 4096.. | 按变体 | 每迭代缓冲上限（level 固定 1 / 19，无 level knob） |
| `zlib*`（NEON 家族） | `.level` / `.maxbuffersize` | 1..9 / 按变体 | 按变体 | zlib 压缩级别 / 缓冲上限（`zfuzz` 为随机 level 的 fuzz 往返、无 knob） |
| `eigen_gemm_*`（NEON 家族） | `.mdim` | 16..4096（按变体，clamp） | 内存自适应 | 矩阵维度（覆盖自适应选择） |
| `eigen_gemm_double_max_arm` | `.mdim` | 1024..4096（clamp） | **按内存×时间联合自适应** | SVE GEMM 逐档变体的矩阵维度 |
| `eigen_svd_cdouble_sve` | `.mdim` | 300..6000（clamp） | **按内存×时间联合自适应** | 强制矩阵维度（预期内存警告目前覆盖 SVD 模板族，本用例显式路径暂无警告——T11b 将统一路由） |
| `eigen_svd_bidiag_sve` | `.mdim` | 64..2048（clamp） | 自适应 | 自写 Householder 双对角化负载维度 |
| `eigen_sparse` | `.n` | 64..8192（clamp） | 自适应 | 稀疏 Cholesky 规模 |
| `openssl_sha` / `openssl_sha3` | `.arena_bytes` | 1MiB..4GiB（clamp） | 内存自适应 | 哈希数据竞技场字节数（SHA-2 加法链 / Keccak 置换的工作集） |
| `openssl_sm3sm4` | `.data_size` | 1024..（自适应上限，clamp） | 自适应 | SM3/SM4 载荷尺寸 |
| `gather*` / `gatherscatter*` | `.data_size` | 按变体 | 自适应 | gather/scatter 工作集 |
| `ipsec_*`（46 个 NEON 变体） | `.datasize` | 1024..64MB（16 的倍数） | 1024 | 加密/解密/MAC 载荷尺寸：1024→L1 / 64KB→L2 / 1MB+→L3/DRAM |
| `ipsec_*_sve*`（24 个 SVE 变体） | `.data_size`（snake_case） | 1024..约 1900（HMAC 组装缓冲上限，clamp） | 内存自适应（流数=运行时 lane 数） | 多流 HMAC 载荷尺寸 |
| `sve512_{f32,f64}_chain_{arm,svd}` | `.bounded` | 0\|1 | 1 | 有界/无界操作数值域（SEVI Obs.19：SDC 频率差 245×） |
| `sve512_nt_reload_arm` | `.distance` | 1..4 | 2 | 栈重装载距离（NUMA3 VA 通路触发指令窗口） |
| `memcpy_rewr` | 环境变量 `SANDSTONE_STRATEGY_INDEX` / `SANDSTONE_STRATEGY_CONF` | 0..2 / conf 路径 | — | MPSC 策略选择（env 而非 `-O` 机制） |

仍无 knob 的测试（`isal_crc*`×10 等）参数为编译期常量——参数审查（`docs/research/third-party-sdc-param-critique.md`）记录的已知状态。

**Eigen SVD**：`eigen_svd_cdouble` 跑在 NEON 后端；`eigen_svd_cdouble_sve` 跑在 SVE 向量后端（vendored Eigen 5.0.1 已补 `PacketXd`/`PacketXcd` double 与 complex<double> packet 全套）。Eigen packet 定宽、VLA 语言级不可编译，故两个走 Eigen 库的 SVE 用例——`eigen_gemm_double_max_arm`（GEMM）与 `eigen_svd_cdouble_sve`（SVD）——改为**逐档各编一份 payload**（均 128/256/512/1024 四档，独立 Eigen 命名空间 + `-msve-vector-bits=n`），运行时按框架 pin 后的**实际**档位函数指针分派；pin 不可用诚实 SKIP，读回不符 FAIL（`eigen_svd_bidiag_sve` 为自写 Householder 真 VLA 负载，不属此分派）。复数 SVD 的 1024 档自 2026-11-01（issue #410）启用：vendored Eigen 的 SVE 复数 `ptranspose` 原实现（memory-bridge）在 NC>N 时越界读，已改写为 scatter 形状（任意 N/NC 界内、方阵形状与原置换位级相同）；gem5-fi 四档（128/256/512/1024）同源双 kernel 实测 `SDCSHIELD_FAILS=0`，512 档 checksum 与修复前逐字节一致（位级非回归），1024 档为首次真实执行（证据链 `docs/superpowers/inventory/issue410-eigen-complex-1024/`）。

## 架构支持

| 架构 | 状态 | 说明 |
|---|---|---|
| ARM64（AArch64） | ✅ 全支持 | Kunpeng 920、通用 ARMv8.1+ |
| x86-64 | 参考架构 | Intel 原始代码路径，移植中不动 |

ARM64 能力：CPU 特性检测（FP/NEON/CRC32/Crypto/SVE/SVE2）、拓扑检测（ACPI PPTT/sysfs/device tree）、SDC 检测（EDAC ECC、CRC32/CRC64）、SIMD（NEON 128 位 + 256/512 仿真）、RAS/ECC（EDAC/ACPI APEI）。

## 运维入口

SDC 激励/复现战役（M0–M5）的机侧运维脚本在 `scripts/sdc-excite-reproduce/`，总入口用法见上节「一键最大程度激发 SDC」。细分脚本：`status.sh` 只读巡检（9 服务 / 战役进度 / M2 事件流 / 电压频率 / SEL / 磁盘 / 告警，即 `--status`）、`start.sh` 直接启动（跳过预检与冒烟门）、`stop.sh` 停止（保留断点，即 `--stop`）。完整运维流程（9 服务、BLACK 恢复、演练排期、巡检模板）见 [运维手册](docs/sdc-excite-reproduce/operations-runbook.md)。

## 延伸

- [PR 检视-修复闭环手册](docs/build-deploy-pr-ci/pr-loop.md) — 会话内 agent 自动化：push 自动开 PR → 五 bot 检视（四外部 SaaS + 本地无头 claude）→ subagent 批判性处置修复 → 循环闭环 → 人工 merge（引擎 `scripts/pr_loop.py` + `scripts/pr_local_review.py`，双端入口 `/pr-loop` 与 `.trae/rules/pr-loop.md`）
- [task-loop 无人值守任务闭环手册](docs/build-deploy-pr-ci/task-loop.md) — CI 红/main 审查/会话/手建 issue → agent-task 队列 → headless agent 修复/实现 → PR → 检视闭环 → 人工 merge（引擎 `scripts/task_loop.py`，cron scan */30 + audit 每 6h，标签协议/单飞/熔断/kill switch；会话命令 `/dev-loop`；`ci_triage.py` 为过渡薄壳，历史手册见 [ci-triage.md](docs/build-deploy-pr-ci/ci-triage.md)）
- [编写测试指南](docs/coding/writing_tests.md) — 框架处理了测试生命周期、线程模型、CPU 特性识别、RNG 等样板代码
- [离线构建依赖与排坑](docs/build-deploy-pr-ci/offline-build-dependencies.md) — 完整依赖树、版本管制、坑点
- [SDC 前沿研究综合](docs/paper/SDC_RESEARCH_for_SDCShield-负载设计.md) — 31 篇 SDC 文献（SOSP/HPCA/ISCA/MICRO/ASPLOS 等，2003–2026）系统性总结，负载设计与 vendored 依赖库选型依据
- [SDC 学术综述报告](docs/paper/SDC_ACADEMIC_SURVEY_CN.md) — 53 篇论文全文精读（笔记在 `docs/paper/ref-paper-fullread-digest/`）写成的分类学综述：核心特征/根因/故障模式/检测/处理五层分类学，学术界×产业界对比、合作生态与全栈×全生命周期启示
- [贡献指南](CONTRIBUTING.md) · [行为准则](CODE_OF_CONDUCT.md)
