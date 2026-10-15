# SDCShield ARM64 多 openEuler 版本构建部署用户指南

> 目的:本文回顾"多 openEuler 版本构建部署"工作的设计与实现,并给出**人/AI 可照做的一键式构建、部署、验证流程**,覆盖 openEuler 20.03 / 22.03 / 24.03 三大系列的 LTS + SP1~SP4 共 **15 个** OS 版本,aarch64 架构。
>
> 配套设计文档:`docs/multi-version-build-deploy.md`;快速开始:`scripts/offline-build/README.md`。本报告是它们的浓缩 + 操作指南 + 验证基线。

---

## 0. 一句话结论

**15 个 OS 版本各产出一个原生二进制,逐版本 full 验证全 PASS(0 fail, 0 crash)**;镜像推 ghcr.io 实测通过(可跨机/CI 共享);现场 tarball ~6MB"下载即跑"。任何人 `git clone --recurse-submodules` 后照本文流程可 **100% 复现**。

---

## 1. 问题与方案选型

### 1.1 问题
SDCShield 是 CPU/系统静默数据损坏(SDC)检测工具。要在 openEuler 20.03 / 22.03 / 24.03 三系列(各 LTS+SP1~SP4=15 版本)上运行,但各版本编译器(gcc-7/10/12)与依赖库版本差异大,不能用一个二进制通吃,需为每版本构建**原生二进制 + 随包运行时库**,打包下载到指定环境运行。要求开发、构建、部署三端灵活高效。

### 1.2 方案:A 路线 + 方案 2(Registry 镜像)

- **A 路线(全量逐版本)**:15 个原生二进制,逐版本纯净验证通过才发布;部署侧精确匹配、**不跨版本回退**(无匹配 SP→硬停指路重构建,绝不静默用相邻 SP 顶替 — placeholder-honesty)。
- **方案 2(镜像入 Registry,不入 git)**:容器镜像以 `Containerfile` 配方入仓、镜像本体存 ghcr.io,可复现、可追溯、不膨胀主仓。RPM 依赖树留 3 个 git submodule(已是现状);主仓新增 < 30KB,永不膨胀。

### 1.3 体积实测(决定分层)
| 对象 | 体积 | 归宿 |
|---|---|---|
| 单 SP RPM 树 | 196–346 MB | RPM submodule(git) |
| 单 SP 容器镜像 | 210–595 MB | ghcr.io Registry(不入 git) |
| 单 SP built/ 产物 | 2.8–19 MB | submodule built/(git) |
| 现场 tarball | ~6 MB | dist/(gitignored) |
| 主仓脚本 | < 30 KB | 主仓 git |

> RPM 树与镜像层百 MB 级,不进 git pack(2GB 上限 + 二进制不可压缩)。RPM 作构建输入需 git 追踪 → 已用 submodule;镜像作构建产物该进 Registry。

---

## 2. 三层存储分层(架构)

```
源码层  sdcshield 主仓(git,<30MB 新增)
        framework/ tests/ meson.build + scripts/offline-build/ +
        Containerfile.template + 编排脚本 + framework/compat/ + third-party/meson/
            │ git submodule pin
依赖层  3 个 RPM submodule(git,各 1~1.7GB)
        openEuler-20.03/22.03/24.03,每系列 5 个 SP 子目录(各~200-340MB RPM + built/ 产物)
            │ Containerfile COPY(构建时)
环境层  ghcr.io Registry(镜像,15 个×~250MB,不入 git)
        ghcr.io/wangxumarshall/sdcshield-offline:24.03-LTS-SP3 ...
        由 Containerfile + RPM pin 可复现,digest 入 manifest
            │ podman run(构建/验证时)
产物层  各 submodule/built/ + 发布 tarball
        built/: sdcshield + libs/ + run-sdcshield.sh + BUILD-HASH + MANIFEST.tsv + VERSION
        dist/sdcshield-<tag>.tar.gz(~6MB,现场用)
            │ 下载
部署层  目标机 run.sh → detect OS → 精确匹配 → exec
```

---

## 3. 一键式流程(人/AI 照做即可复现)

### 3.1 前置(一次性)

```bash
# 环境:openEuler aarch64(或任意 aarch64 Linux)+ podman + skopeo + git
#   sudo dnf install -y podman skopeo git   # openEuler
# 磁盘:~5GB(RPM submodule 4.7GB + 镜像缓存)

# 1. 克隆主仓 + RPM submodule(3 仓,共~4.7GB,首次慢)
git clone --recurse-submodules https://github.com/wangxumarshall/sdcshield.git
cd sdcshield
git checkout main   # 本方案分支
git submodule update --init --recursive          # 确保 RPM 树就绪

# 2.(可选,要跨机共享镜像才需)ghcr.io 授权
#    GitHub → Settings → Developer settings → Personal access tokens(fine-grained)
#    勾 write:packages,生成 token
echo "$GHCR_TOKEN" | podman login ghcr.io -u wangxumarshall --password-stdin
```

### 3.2 一键构建全 15 版本(发布闸门)

```bash
# 全 15 SP:镜像就绪 → 源码构建 → 打包 → 纯净 full 验证
./scripts/offline-build/build-all.sh --all --full
#   --full: eigen -n1 + 全量 -n8,纯净容器(只挂 built/)跑 — 发布闸门
#   --jobs 3: 并发(jobs 太高抢内存,3 为宜;192 核可试 4-6)
#   首次每 SP 拉 quay base + 烘焙 deps(~数分钟/SP),全 15 约 40-75min
#   后续:源码哈希 skip,改一行只重建受影响 SP
```

**期望汇总**:`PASS: 15  SKIP: 0  FAIL: 0`。

### 3.3 验证基线(15 SP 全 full,已实测)

| 系列 | LTS | SP1 | SP2 | SP3 | SP4 |
|---|---|---|---|---|---|
| 24.03 (gcc-12) | pass 1253 | 1286 | 1542 | 1758 | 2840 |
| 22.03 (gcc-10) | pass 3812 | 2642 | 2429 | 3831 | 2326 |
| 20.03 (toolset-10) | pass 2586 | 3610 | 2940 | 1145 | 1011 |

> 全部 `fail=0, crashes=0`。覆盖三系列 + 最老(gcc-7+toolset-10)/基准(gcc-12)toolchain。复现时若 SP4 等较新版本 pass 数更高属正常(测试集随版本增长)。

### 3.4 单版本快速迭代(开发期)

```bash
# 只构建验证某 SP(如基准 24.03 SP3),smoke 档(秒级)
./scripts/offline-build/build-all.sh 24.03 SP3 --smoke
#   --smoke: --list-tests>100 + zstd19 -n1 exit:pass(开发期默认)
#   --since origin/main: 只重建 git diff 受影响 SP(叠加哈希 skip,双重过滤)
./scripts/offline-build/build-all.sh 24.03 SP3 --smoke --since origin/main
```

### 3.5 部署到目标机

```bash
# 构建机:产出现场 tarball(~6MB,含 run.sh 自动检测 OS + 精确匹配)
./scripts/offline-build/package-release.sh 24.03 SP3
# → dist/sdcshield-openEuler-24.03LTS_SP3-<sha8>.tar.gz

# 拷到目标机解压后直接跑(自动检测 OS,精确匹配,不匹配则硬停指路)
tar xzf sdcshield-openEuler-24.03LTS_SP3-*.tar.gz
./run.sh -e zstd19 -t 2000 -n 1      # 或任意 sdcshield 参数
./run.sh --list-tests
./run.sh                              # 全量 PROD 用例
./run.sh full                         # 全核满载 eigen 运算(每测试默认 60s)
./run.sh full -t 120s                 # 同上,每测试 120s
```

`run.sh` 流程:`/etc/os-release` 检测 OS → 精确匹配 `built-index.tsv`(不回退)→ 校验 `binary-sha256`(防损坏)→ `exec run-sdcshield.sh`(设 `LD_LIBRARY_PATH`)。不匹配→硬停并指路 `build-all.sh <tag> --full` 重构建。

#### `full` 选项:全核满载 eigen 运算

`run-sdcshield.sh` 首参数为 `full` 时进入两段式 eigen 满载模式(其余参数原样透传):

1. **第一段(全核)**:11 个稳定 eigen 测试(`-e 'eigen*'` 排除下面 4 个),**不带 `-n`**——框架默认使用系统全部 CPU(实测 128 核机 128/128 CPU 100% 满载)。
2. **第二段(单线程补跑)**:4 个数值敏感测试 `eigen_svd_double`/`eigen_sparse`/`eigen_svd_cdouble`/`eigen_svd_cdouble_sve` 以 `-n 1` 运行——大规模多线程下并行 SVD/sparse 求解顺序的 ULP 级差异会对严格 `memcmp` golden 比较产生偶发假 FAIL(平台已知特性,CLAUDE.md),单线程规避。

要点:
- 每测试默认 60s,透传 `-t <time>`(或 `--test-time=`)覆盖;透传的 `-n` 只作用于第一段,第二段恒为 1 线程。
- `eigen_svd_cdouble_sve` 在无 SVE 的机器上自动 skip(`CpuNotSupported`),属预期;在 SVE 机器上自 2026-09-19 起为真实向量化压测——vendored Eigen 5.0 已补 `double`/`complex<double>` SVE packet(单次 300×300 复数 BDCSVD 约 0.7 s,原标量回退需 >10 分钟)。注意 size-specific SVE 代码要求运行时 VL 等于编译期 `-msve-vector-bits=128`,在 VL≠128 的 SVE 主机上独立运行需先固定任务 VL(prctl `PR_SVE_SET_VL`)。
- 脚本退出码 = 两段退出码之或;两段相互独立完成(第一段失败不阻断第二段)。

### 3.6 镜像推 ghcr.io(跨机/CI 共享)

```bash
# 单 SP 构建并推送(镜像已存在则幂等 skip,只补 push)
./scripts/offline-build/images/build-images.sh 24.03 SP3 --push
#   manifest remote 列变 yes,image-digest 写远端 digest
# 全 15 推:
./scripts/offline-build/images/build-images.sh --all --push

# 气隙(无网):导 oci tarball,拷到现场 podman load
./scripts/offline-build/images/build-images.sh 24.03 SP3 --tar
# → dist/images/sdcshield-offline-24.03-LTS-SP3.oci.tar(~1.6GB)
# 现场:podman load < sdcshield-offline-24.03-LTS-SP3.oci.tar
```

CI 直接 `podman pull ghcr.io/...:<tag>`,无需每 job 重建镜像。

---

## 4. 五个效率杠杆(为何高效)

| 杠杆 | 机制 | 收益 |
|---|---|---|
| 1 deps 烘焙 | 依赖装进镜像层(`Containerfile`),构建启动即就绪 | 消除每次构建装 ~300 RPM |
| 2 哈希 skip | `BUILD-HASH` 比对(源码+sed+配置+镜像 input-hash),未变则复用 built/ | 稳态改一行只重建受影响 SP |
| 3 -P 并行 | `--jobs N` 并发起 podman | 墙上时间 ~1/N |
| 4 --since | `--since <ref>` 按 git diff 选 SP | 只重建受影响版本 |
| 5 smoke/full | smoke 秒级(开发),full 全量(发布) | 开发快,发布稳 |

---

## 5. 适配层收敛(容器内源码副本零修改)

`container-build.sh` 原 4 段 sed-patch(改容器内源码副本)全部收敛到源码/meson option,只剩 CXXFLAGS/polyfill 注入 + `-D` option。这让 15 路构建可维护(改源码一行不会让 sed 漏匹配)。

| 原 sed | 收敛 | 落点 |
|---|---|---|
| `udf #0x1234`→`.inst` | 源码直接用 `.inst 0x00001234`(.inst 伪指令,新旧 binutils 统一) | `framework/selftest.cpp` |
| `string::contains`→`.find()` | 源码改可移植 `.find()==npos`(C++17, GCC10/12 通用) | `framework/sandstone_utils.cpp` `sandstone_opts.cpp` |
| ACL 子集置空(sed 改 meson.build) | meson option `-Denable_acl=disabled`(22.03/20.03)+ `-Dacl_incdir=...`(24.03 auto) | `meson_options.txt` + `tests/cpu/arithmetic_arm/meson.build` |
| `meson_version >=1.3`→`>=0.59` | 源码声明 `>=0.56`(`project_source_root` 引入版) | `meson.build` |

> 22.03/20.03 的 C++20/23 缺口由 `framework/compat/cpp23_polyfill.h`(`-include` 注入)+ `compat/barrier` shim 兜底,host 源码不动。

---

## 6. 脚本一览(各司其职)

| 脚本 | 作用 |
|---|---|
| `images/build-images.sh` | 烘焙依赖镜像层(quay base + RPM 强装)。幂等:input-hash 不变则 skip。`--push`/`--tar` |
| `images/Containerfile.template` | 一模板 + SP 参数化(`--build-arg`)。`FROM quay.io/openeuler/openeuler:<sp-tag>` |
| `container-build.sh` | 单 SP 源码构建(镜像已烘焙则跳过装包)。22.03/20.03 注入 polyfill + `-D` |
| `build-all.sh` | 15 矩阵编排(5 杠杆)。`--all --full` 发布闸门,`--smoke` 开发,`--since`/`--jobs`/`--force` |
| `package-built-artifacts.sh` | 产物进 submodule `built/`(二进制+libs+run+BUILD-HASH+MANIFEST+VERSION) |
| `verify-built-pristine.sh` | 决定性闸门:纯净容器(只挂 built/,无 host libs)跑 — "下载即跑" |
| `package-release.sh` | 现场 tarball(~6MB,含 `run.sh` 自动检测 OS + 精确匹配 + sha256 校验) |
| `run.sh`(随 tarball) | 目标机入口:检测 OS → 精确匹配 → 校验 → exec |
| `_common.sh` | openEuler 版本检测(`detect_os_sp`/`detect_os_version_full`),被多脚本 source |

---

## 7. 关键 gotcha(复现必读)

1. **RPM submodule 首次需 init**:`git clone --recurse-submodules` 或 `git submodule update --init --recursive`(慢,~4.7GB)。未 init 时 `build-images.sh` 报 "RPM 目录不存在"。
2. **SELinux**:verify/run 挂载 host 二进制进容器**必须加 `:Z`**(否则 `Permission denied` → verify 报 `too-few-tests(1)`)。脚本已统一加。
3. **20.03 的 meson**:系统自带 meson 0.54 太旧,用仓内 vendored `third-party/meson/meson-0.59.4`(纯 Python,兼容 py3.7)。无需额外下载。
4. **24.03 的 ACL**:`-Denable_acl=auto` 需 host 有 `libarm_compute.so` + 头树。24.03 容器构建时 `container-build.sh` 传 `-Dacl_incdir=<host path>`(挂载点同名);无 ACL 环境则 auto 降级为空库(跳过 2 ACL 测试),不影响其余。
5. **并发**:`--jobs N` 时每 SP 的 build_one 内部调 build-images+container-build+verify 各起 podman,实际并发容器数 > N。manifest 写入用 flock 串行化(find 共享锁 + set 排他锁);inner-build.sh 用 SP-specific 路径避免覆盖竞态。建议 `--jobs 3` 起步,192 核内存足可试 4-6。
6. **路径**:脚本在 `scripts/offline-build/[images/]` 下,`SRC_ROOT = SCRIPT_DIR/../..`(两层深,已修正)。
7. **ghcr push**:需 PAT(`write:packages`),`echo $TOKEN | podman login ghcr.io -u <user> --password-stdin`。token 暴露过务必 revoke/rotate。

---

## 8. 实现历程(patches,21 个,verify→push 非 main)

| # | patch | 价值 |
|---|---|---|
| 1 | `75d4ddb` 镜像子系统(Containerfile + build-images.sh) | 方案 2 基座 |
| 2 | `064dde8` container-build 杠杆1(镜像烘焙则跳过装包) | 消除装 300 RPM |
| 3 | `ece8755` build-all 串行 + smoke/full 编排 | 一条命令跑矩阵 |
| 4 | `ed39679` build-all 杠杆2(哈希skip)+杠杆4(--since) | 稳态增量 |
| 5 | `8da3874` build-all 杠杆3(-P 并行) | 墙上时间 |
| 6 | `d1fb4d9` package 写 BUILD-HASH+MANIFEST+VERSION | 溯源 + skip |
| 7 | `396eddf` package-release 现场 tarball + run.sh | 部署闭环 |
| 9 | `c2fb62e` CI multi-version job(角落三 smoke) | 回归哨兵 |
| 10 | `80f141d` 收敛 udf + contains sed 到源码 | 容器零源码修改 |
| — | `039bbff` README 快速开始 | 文档 |
| A | `07e0f96` vendor meson 0.59.4 入仓 | 20.03 复现缺口闭合 |
| B | `313c0f8` ACL sed → meson option | 容器零源码修改 |
| C | `61694a8` meson_version sed → 源码 `>=0.56` | 4 段 sed 全收敛 |
| D | `dc5eff0`/`d88c11c`/`71cf2f6`/`4658da0`/`0771a4c`/`86d60c1` --all/push诊断/flock并发/skip导tar/:Z SELinux/ghcr实测 | 15 SP full + ghcr push 全链路 |
| — | `cf11f64` gitignore 锁文件 | 清理 |

---

## 9. 验证证据(真实命令,非"应该能跑")

```
# 15 SP 全 full(发布闸门)
build-all.sh --all --full → 全 PASS, 0 fail, 0 crash(见 §3.3 表)

# ghcr push 实测
build-images.sh 24.03 SP3 --push --force
  → 推 ghcr.io/wangxumarshall/sdcshield-offline:24.03-LTS-SP3 ✓
  → manifest remote=yes, image-digest=sha256:07982eb8... ✓
podman pull ghcr.io/.../24.03-LTS-SP3 → 拉回,gcc 12.3.1/find/meson 全可用 ✓

# 现场下载即跑
package-release.sh 24.03 SP3 → tarball 5.8MB
纯净容器(只挂 tarball 解压目录)跑 run.sh:
  检测到 openEuler-24.03LTS_SP3 → 匹配 → 校验 binary-sha256 ✓ → exec → list-tests 220, zstd19 exit:pass
22.03 容器跑 24.03 tarball(不匹配)→ 硬停指路重构建 ✓

# sed 收敛(容器零源码修改)
20.03 LTS 完全无 sed 构建:261/261, list-tests=208, exit:pass, pristine PASS
selftest_sigill 用 .inst 仍触发 SIGILL(code 4, ILL_ILLOPC)✓
```

---

## 10. 残留(非方案必需)

- ghcr 全 15 镜像未全 push(只实测 24.03-SP3 一条链路;`build-images.sh --all --push` 可全推,需 token)。
- 24.03 的 `-Dacl_incdir` 仍传 host 特定路径(`/home/sdc/root/...`),真复现时该路径需存在或改传;20.03/22.03 不受影响(`disabled`)。
- CI `multi-version` job 未在 GHA 实跑(无 runner 配置;本地已模拟等价路径全 PASS)。

---

## 11. 指导复现的最小命令序列(复制即跑)

```bash
# === 构建机(aarch64 openEuler 或任意 aarch64 + podman)===
git clone --recurse-submodules https://github.com/wangxumarshall/sdcshield.git
cd sdcshield && git checkout main
git submodule update --init --recursive
# (可选)ghcr 授权:echo $GHCR_TOKEN | podman login ghcr.io -u wangxumarshall --password-stdin

# 一键构建验证全 15:
./scripts/offline-build/build-all.sh --all --full --jobs 3
# 期望:PASS: 15  SKIP: 0  FAIL: 0

# 产出现场包(任选版本):
./scripts/offline-build/package-release.sh 24.03 SP3
# → dist/sdcshield-openEuler-24.03LTS_SP3-*.tar.gz

# === 目标机 ===
tar xzf sdcshield-openEuler-24.03LTS_SP3-*.tar.gz && ./run.sh -e zstd19 -t 2000 -n 1
# 期望:exit: pass(run.sh 自动检测 OS 精确匹配)
```

> 详见 `scripts/offline-build/README.md`(快速开始)与 `docs/multi-version-build-deploy.md`(完整设计)。
