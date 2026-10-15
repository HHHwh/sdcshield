# SDCShield 多 openEuler 版本构建与部署方案(方案 2:Registry 镜像)

> 目标:在 openEuler 20.03 / 22.03 / 24.03 三大系列、各 LTS+SP1~SP4 共 **15 个**操作系统版本上运行 SDCShield,受限于各版本编译器(gcc-7/10/12)与依赖库版本差异,需为每个版本构建**原生二进制 + 随包运行时库**,打包后下载到指定环境运行。要求**开发、构建、部署**三端灵活且高效。
>
> 本方案采用 **A 路线(全量逐版本)** + **方案 2(镜像入 Registry,不入 git)**:15 个原生二进制逐版本纯净验证通过才发布;部署侧精确匹配、**不跨版本回退**;容器镜像以 `Containerfile` 配方入仓、镜像本体存 Registry,可复现、可追溯、不膨胀主仓。

---

## 0. 设计基线(已实测的真实数字)

| 对象 | 真实体积 | 归宿 |
|---|---|---|
| 单 SP 的 RPM 依赖树(构建输入) | 196–346 MB | RPM submodule(git,已是现状) |
| 单系列 5 SP RPM 树 | 0.98–1.7 GB | 同上,每系列独立 submodule |
| 单 SP `built/` 产物(二进制+libs) | 2.8–19 MB | 各 RPM submodule 的 `built/`(已是现状) |
| 单 SP 容器镜像(装好 deps) | 210–595 MB | **ghcr.io Registry**(本方案,不入 git) |
| `Containerfile` + 编排脚本(文本) | < 10 KB | sdcshield 主仓 git |
| 现有本地 podman 镜像存储 | 39 GB(15 镜像) | 迁移至 Registry 后本地仅作缓存 |

> 体积结论:RPM 树与镜像层均为**百 MB 级**,绝不进 git pack(2GB 上限 + 二进制不可压缩)。RPM 树作为**构建输入**需 git 追踪版本 → 已用 submodule 解决;镜像作为**构建产物**该进 Registry,不入 git。

---

## 1. 总体架构

### 1.1 三层存储分层

```
┌─────────────────────────────────────────────────────────────────┐
│ 源码层   sdcshield 主仓 (git, <100MB)                       │
│   framework/ tests/ meson.build + scripts/offline-build/ +       │
│   framework/compat/  +  Containerfile.template + 编排脚本       │
└──────────────────────────┬──────────────────────────────────────┘
                           │ git submodule pin
┌──────────────────────────▼──────────────────────────────────────┐
│ 依赖层   3 个 RPM submodule (git, 各 1~1.7GB)                   │
│   openEuler-20.03 (1.7G)  22.03 (977M)  24.03 (1.2G)            │
│   每系列 5 个 SP 子目录,各 ~200-340MB: rpms/*.rpm + built/ 产物 │
└──────────────────────────┬──────────────────────────────────────┘
                           │ Containerfile COPY (构建时)
┌──────────────────────────▼──────────────────────────────────────┐
│ 环境层   ghcr.io Registry (镜像, 15 个 × ~250MB, 不入 git)       │
│   ghcr.io/wangxumarshall/sdcshield-offline:24.03-LTS-SP3 ...   │
│   由 Containerfile.template + RPM pin 可复现构建;digest 入 manifest│
└──────────────────────────┬──────────────────────────────────────┘
                           │ podman run (构建/验证时)
┌──────────────────────────▼──────────────────────────────────────┐
│ 产物层   各 submodule/built/ + 发布 tarball                     │
│   built/: sdcshield + libs/ + run-sdcshield.sh + BUILD-HASH   │
│   dist/sdcshield-<tag>.tar.gz (~20MB, 现场用)                  │
└──────────────────────────┬──────────────────────────────────────┘
                           │ 下载
┌──────────────────────────▼──────────────────────────────────────┐
│ 部署层   目标机 run.sh → detect OS → 精确匹配 → exec            │
└─────────────────────────────────────────────────────────────────┘
```

### 1.2 数据流(改一行源码 → 发布)

```
[download-all-versions.sh] ──▶ RPM submodule (3 仓, 输入, 已有)
                                    │
[build-images.sh]           ──▶ ghcr.io 镜像 (15, 环境, 本方案新增)
        ↑ Containerfile.template      │
                                    │
[container-build.sh]         ──▶ build-out/<tag>/sdcshield (二进制, 已有/改)
                                    │
[package-built-artifacts.sh]──▶ submodule/built/ + BUILD-HASH + MANIFEST (已有/增强)
                                    │
[verify-built-pristine.sh]  ──▶ 闸门 PASS/FAIL (已有)
                                    │ (PASS)
[package-release.sh]        ──▶ dist/<tag>.tar.gz + built-index.tsv (新增)
                                    │
[ghcr.io Release / GH Release]
                                    │
[run.sh @ 目标机]          ──▶ detect_os → 精确匹配 → 解压 → exec (新增)
```

### 1.3 产物矩阵(A 路线,15 个原生二进制)

```
            20.03 (gcc-10 toolset)    22.03 (gcc-10)        24.03 (gcc-12)
LTS   openEuler-20.03LTS        openEuler-22.03LTS        openEuler-24.03LTS
SP1   openEuler-20.03LTS_SP1     openEuler-22.03LTS_SP1     openEuler-24.03LTS_SP1
SP2   ...SP2                     ...SP2                     ...SP2
SP3   ...SP3                     ...SP3                     ...SP3   ← 基准版本
SP4   ...SP4                     ...SP4                     ...SP4
```

**A 路线边界(不可逾越)**:
- 一个 SP 一个二进制,取该 SP 原生 toolchain codegen。
- 部署侧无匹配 SP → **硬停并指路重构建**,绝不静默用相邻 SP 顶替(placeholder-honesty)。
- 每个二进制自带运行时 `.so` bundle,`run-sdcshield.sh` 设 `LD_LIBRARY_PATH`,纯净容器即跑。
- `verify-built-pristine.sh`(只挂 `built/`,无 host lib64)是唯一发布闸门。

---

## 2. 镜像构建子系统(方案 2 核心)

### 2.1 Containerfile 模板设计

**文件**:`scripts/offline-build/images/Containerfile.template`(< 3KB,入主仓)

设计原则:一个模板 + `build-images.sh <series> <sp>` 参数化注入,避免 15 个重复文件。

**镜像基座策略(待定,见 §11)**:
- **策略 B1(推荐)**:`FROM quay.io/openeuler/openeuler:<series>`(公共 LTS 镜像 ~210MB),`COPY` 该 SP RPM 树,`RUN rpm -Uvh --nodeps --force` 强装覆盖到 SP 版本。完全可复现,base 来自公共 Registry。
- **策略 B2(兼容)**:`FROM` 现有手动 KIWI SP 镜像(推 Registry 后作 base),`Containerfile` 仅声明挂载点。复用现有镜像,但 base 仍需独立追溯。

模板骨架(B1):

```dockerfile
# Containerfile.template — 由 build-images.sh 参数化:
#   BUILD-arg SERIES, SP, RPMDIR(构建上下文中的 RPM 子目录)
FROM quay.io/openeuler/openeuler:${SERIES}
# 20.03 需 gcc-toolset-10,镜像层装好(在 /opt/openEuler/gcc-toolset-10/)
ARG SERIES
ARG SP

# 1) COPY 该 SP 的 RPM 树(构建上下文 = RPM 子目录本身)
COPY *.rpm /rpms/

# 2) 预装 findutils(最小镜像缺 find)
RUN rpm -Uvh --nodeps --force /rpms/findutils-*.rpm >/dev/null 2>&1 || true

# 3) 强装依赖树(等价 container-build.sh:68-97 的 EXCLUDE_RE 过滤)
#    排除 bootloader/固件/系统核心,只装构建必需包
RUN KEEP=$(find /rpms -maxdepth 1 -name '*.rpm' | \
        grep -ivE 'grub2|shim|mokutil|efivar|dracut|kpartx|fuse|glibc$|glibc-common|glibc-headers|glibc-static|systemd$|systemd-libs|systemd-udev|setup$|filesystem$|basesystem|shadow|pam|crypto-policies|openEuler-release|openEuler-gpg|openEuler-repos') && \
    rpm -Uvh --nodeps --force $KEEP >/tmp/rpm-install.log 2>&1 || true && \
    for mustpkg in glibc-devel glibc-headers binutils \
                   gcc-toolset-10-gcc gcc-toolset-10-gcc-c++ \
                   gcc-toolset-10-libstdc++-devel gcc-toolset-10-libgcc; do \
        f=$(ls /rpms/${mustpkg}-*.rpm 2>/dev/null | head -1); \
        [ -n "$f" ] && rpm -Uvh --nodeps --force "$f" >/dev/null 2>&1 || true; \
    done

# 4) 建 ld 符号链接(20.03/22.03 最小镜像缺)
RUN [ ! -e /usr/bin/ld ] && [ -e /usr/bin/ld.bfd ] && \
    ln -sf /usr/bin/ld.bfd /usr/bin/ld || true

# 5) 20.03 toolset 库已就位;构建期 env(CC/CXX/PATH)由 container-build.sh 注入
#    镜像只保证"deps 存在",不规定"怎么用 deps"
RUN gcc --version | head -1
```

**关键设计点**:
- `COPY *.rpm /rpms/` 的构建上下文 = 该 SP 的 RPM 子目录(`podman build ... <rpmdir>`),podman 发 tar 流 ~250MB,一次性,镜像层缓存后不再重发。
- 依赖安装逻辑从 `container-build.sh` 搬进 `Containerfile` 固化为层 —— 这是**杠杆 1(烘焙 deps)**的基础:镜像构建一次,之后每次源码构建直接跳过装包。
- 20.03 toolset 激活(PATH/CC/CXX)是"怎么用 deps",仍由 `container-build.sh` 注入(构建期 env),不进镜像 `RUN`。

### 2.2 build-images.sh

**文件**:`scripts/offline-build/images/build-images.sh`(< 4KB,入主仓)

```
用法:
  build-images.sh <series> <sp> [--push] [--tar] [--force]
  build-images.sh --all [--push]
  build-images.sh --list          # 打印 image-manifest.tsv
```

逻辑:
1. 算镜像 tag:`ghcr.io/wangxumarshall/sdcshield-offline:${series}-LTS-${sp}`(LTS 无后缀)
2. 算 Containerfile 输入哈希:`sha256(Containerfile.template + 该 SP RPM 树的 file list + 版本宏配置)`
3. 查 `image-manifest.tsv`:输入哈希已存在且 `skopeo inspect` 确认 digest 在 Registry → **skip**(幂等,改 RPM 才重建)
4. `podman build -f Containerfile.template -t <tag> --build-arg SERIES=.. --build-arg SP=.. <rpmdir>`
5. `podman push <tag>`(若 `--push`)
6. 算 digest:`podman inspect <tag> --format '{{.Digest}}'`
7. 追加/更新 `image-manifest.tsv`
8. `--tar`:`podman save <tag> -o dist/images/<tag>.oci.tar`(气隙用)

幂等性:RPM 树不变 + Containerfile 不变 → 输入哈希不变 → 不重建。这是镜像层的增量基础。

### 2.3 image-manifest.tsv(镜像追溯表)

**文件**:入主仓 `scripts/offline-build/images/image-manifest.tsv`(< 2KB)

```
sp-tag            containerfile-sha  rpm-pin-sha    image-digest                                                     base-image                     date         size-MB
24.03-LTS-SP3     a1b2c3d            028e9c9...     sha256:9f8a7b...                                                  quay.io/openeuler/openeuler:24.03  2026-08-24  249
22.03-LTS-SP3     a1b2c3d            7d3ce66...     sha256:e7c1d2...                                                  quay.io/openeuler/openeuler:22.03  2026-08-24  197
20.03-LTS-SP4     a1b2c3d            4a11f87...     sha256:3b9f01...                                                  quay.io/openeuler/openeuler:20.03  2026-08-24  339
...
```

`containerfile-sha` = `Containerfile.template` 内容哈希;`rpm-pin-sha` = 对应 submodule 的 git pin。两者组合可重现任意历史镜像。digest 是 Registry 里实际镜像的指纹。

### 2.4 Registry 与气隙导入

- **主路径(有网开发机)**:`build-images.sh <series> <sp> --push` → `podman push ghcr.io/wangxumarshall/sdcshield-offline:<tag>`。ghcr.io 私有仓免费,15 × 250MB ≈ 3.7GB,额度内。

  **ghcr.io 授权(首次 push 前,一次性)**:
  1. GitHub → Settings → Developer settings → Personal access tokens → Fine-grained tokens → 生成 token,勾选 `write:packages`(写 package)权限。
  2. 在构建机登录(二选一):
     ```bash
     # 用 token:
     echo "$GHCR_TOKEN" | podman login ghcr.io -u wangxumarshall --password-stdin
     # 或交互:
     podman login ghcr.io     # Username: wangxumarshall  Password: <token>
     ```
  3. 登录后 `build-images.sh 24.03 SP3 --push` 实测 push;manifest 的 `remote` 列变 `yes`、`image-digest` 写入远端 digest。

- **气隙路径**:在有网机 `build-images.sh <s> <sp> --tar` → 得 `dist/images/<tag>.oci.tar`(~250MB)→ 拷到现场 → `podman load < <tag>.oci.tar`。
  - 等价命令:`skopeo copy docker://ghcr.io/...:<tag> oci-archive:/path/<tag>.tar`
- **跨 CI runner**:GitHub Actions 直接 `podman pull ghcr.io/...:<tag>`,无需每 job 重建镜像(GHA 默认有 `packages: read` 权限)。

---

## 3. 构建编排子系统(build-all.sh)

**文件**:`scripts/offline-build/build-all.sh`(< 6KB,入主仓)。这是**开发/构建效率的核心**:把"改一行源码 → 全矩阵验证"压成一条命令。

### 3.1 接口

```
用法:
  build-all.sh [series] [sp] [options]
    series: 24.03 | 22.03 | 20.03 | all(默认 all)
    sp    : LTS | SP1..SP4 | all(默认 all)
  options:
    --since <ref>   只重建 git diff <ref>..HEAD 受影响的 SP
    --smoke         快档:list-tests>100 + zstd19 -n1 pass(默认)
    --full          全量:verify-built-pristine.sh(eigen -n1, 余 -n8)
    --jobs N        并行度(默认 4);内层 ninja -j 16
    --rebuild-images  强制重建镜像(默认按 manifest skip)
    --no-verify     只构建不验证
```

### 3.2 五个效率杠杆

**杠杆 1:deps 烘焙进镜像(消除每构建重装 300 RPM)**
- 现状 `container-build.sh:68-97` 每次构建 `rpm -Uvh --nodeps --force` ~300 包,是 15 路构建最重固定成本。
- 改 `container-build.sh`:启动时检测镜像里 `/usr/bin/gcc` 已存在 → 跳过整段依赖安装,直接进 `meson setup`。
- 一次构建从"装包+编译"降到"只编译"。

**杠杆 2:源码哈希 skip(稳态增量)**
- 构建产 `built/BUILD-HASH`,内容 = 影响输出的输入哈希:
  ```
  sha256(
    framework/ tests/ meson.build meson_options.txt framework/compat/ 全部源码
    + 该 SP 的 sed 适配清单(container-build.sh 里的 perl 命令文本)
    + Containerfile.template 的 containerfile-sha(基座变了要重建)
    + cpp_std / OPENEULER_MACRO 配置
  )
  ```
- `build-all.sh` 开头算当前哈希,与各 SP 现有 `built/BUILD-HASH` 比,**相同则 skip 复用** `built/`。
- **必须把 sed 适配命令也算进哈希**:否则改了 sed 却复用旧二进制就错。
- 稳态下:改一行 `sandstone.cpp`,只有源码确实变的 SP 重建,其余 14 个零成本复用。

**杠杆 3:并行**
- `xargs -P $JOBS`(默认 4)并行起 podman 容器。
- 每容器内 `ninja -j 16`。
- 15 串行 → -P4,墙上时间 ~1/4。`--jobs` 可按机器调(192 核箱内存够可给 6)。

**杠杆 4:--since 增量(按 git diff 选 SP)**
- `git diff --name-only <ref>..HEAD`:若只动 `tests/cpu/zstd*` → 大多数 SP 都受影响(源码共享);若动 `framework/compat/cpp23_polyfill.h` → 只 22.03/20.03 受影响(24.03 不注入)。
- 叠加杠杆 2 的哈希 skip,双重过滤。

**杠杆 5:smoke / full 两档**
- `--smoke`(默认):`--list-tests` > 100 + `zstd19 -t 1000 -n 1` → `exit: pass`。秒级。
- `--full`:`verify-built-pristine.sh` 全量。发布前必跑。
- 开发期默认 smoke,发布闸门 full。

### 3.3 build-all.sh 骨架

```bash
#!/bin/bash
set -euo pipefail
# ... 参数解析:SERIES, SP, SINCE, MODE(smoke/full), JOBS, REBUILD_IMAGES

# 杠杆 4: --since 算受影响文件集
AFFECTED_FILES=""; [ -n "$SINCE" ] && AFFECTED_FILES=$(git diff --name-only "$SINCE"..HEAD)

# 镜像就绪检查(杠杆 1 前提)
[ -z "$REBUILD_IMAGES" ] && for sp in targets; do
    build-images.sh $series $sp --push  # 幂等,已存在则 skip
done

# 杠杆 2+3: 并行构建 + 哈希 skip
build_one() {
    local series=$1 sp=$2
    local tag="openEuler-${series}${SP_DIR[$sp]}"
    local cur_hash=$(compute_input_hash "$series" "$sp")  # 含 sed 清单
    local existing=$(cat "third-party/rpms/openEuler-$series/$tag/built/BUILD-HASH" 2>/dev/null || echo "")
    if [ "$cur_hash" = "$existing" ] && [ "$MODE" != "full-force" ]; then
        echo "skip $tag (hash unchanged)"; return 0
    fi
    # 杠杆 4: --since 过滤
    [ -n "$SINCE" ] && ! affected "$series" "$sp" "$AFFECTED_FILES" && { echo "skip $tag (--since)"; return 0; }
    container-build.sh "$series" "$sp"          # 杠杆 1:镜像内 deps 已就绪,跳过装包
    package-built-artifacts.sh "$series" "$sp"  # 写 BUILD-HASH + MANIFEST
    [ "$MODE" = full ] && verify-built-pristine.sh "$series" "$sp" full
    [ "$MODE" = smoke ] && verify-built-pristine.sh "$series" "$sp" smoke
}
export -f build_one
# 杠杆 3: 并行
printf '%s\n' "${TARGETS[@]}" | xargs -P "$JOBS" -I{} bash -c '...build_one {}'
```

---

## 4. 打包发布子系统

### 4.1 package-built-artifacts.sh 增强(写元数据)

现状已产 `built/`(二进制+libs+run-sdcshield.sh)。增加三份元数据:

**`built/BUILD-HASH`**(单行):构建输入哈希(杠杆 2 用)。skip 判定依据。

**`built/MANIFEST.tsv`**:产物文件清单 + 校验和
```
file              sha256                                                       size     source
sdcshield        9f8a...                                                     5242880  built
libs/libstdc++.so.6.0.28  e7c1...                                1800000  gcc-toolset-10
libs/libatomic.so.1.2.0   3b9f...                                  50000  libatomic RPM
libs/libgcc_s.so.1        ...                                        ...
run-sdcshield.sh  ...                                        1200  generator
```

**`built/VERSION`**:人读元数据
```
sdcshield openEuler-24.03LTS_SP3
git: 028e9c9 (main, 2026-08-24)
built: 2026-08-24T10:00Z
gcc: 12.3.1   glibc: 2.38   cpp_std: gnu++23
image: ghcr.io/.../sdcshield-offline:24.03-LTS-SP3@sha256:9f8a7b...
binary-sha256: 9f8a...
build-hash: a1b2c3d...
```

这三份让任何 SP 的产物可溯源、可校验、可 skip。

### 4.2 package-release.sh(现场 tarball)

**文件**:`scripts/offline-build/package-release.sh`(新增,< 3KB)

```
用法: package-release.sh <series> <sp>
产物: dist/sdcshield-openEuler-24.03LTS_SP3-<sha8>.tar.gz (~20MB)
  含:
    sdcshield          (stripped 二进制)
    libs/               (运行时 .so)
    run-sdcshield.sh
    MANIFEST.tsv
    VERSION
    built-index.tsv     (全 15 SP 总表,供 run.sh 匹配)
```

轻量(~20MB),不含 RPM 树/镜像。现场/气隙机用。多个 SP 的 tarball 可同放一目录。

### 4.3 built-index.tsv(跨 SP 总表,部署匹配用)

**文件**:`scripts/offline-build/built-index.tsv`(随每个 release tarball 携带一份全量)

```
sp-tag              git-sha     built-date          gcc       glibc    binary-sha256                    build-hash  image-digest                  pass  skip  fail
24.03-LTS-SP3       028e9c9     2026-08-24T10:00Z   12.3.1    2.38     9f8a...                          a1b2c3d     sha256:9f8a7b...              1946  8     0
22.03-LTS-SP3       7d3ce66     2026-08-24T10:05Z   10.3.1    2.34     e7c1...                          a1b2c3d     sha256:e7c1d2...              1940  10    0
20.03-LTS-SP4       4a11f87     2026-08-24T10:10Z   10.3.0    2.28     3b9f...                          a1b2c3d     sha256:3b9f01...              1920  12    0
...
```

- 部署侧 `run.sh` 读它精确匹配 SP。
- SDC 结果异常时,凭它查回 git sha / gcc / 构建日 / image digest。

---

## 5. 部署子系统(run.sh)

**文件**:随 release tarball 携带的顶层 `run.sh`(也可独立发布)

### 5.1 流程

```
1. source _common.sh → detect_os_version_full → "openEuler-24.03LTS_SP3"
2. 读 built-index.tsv,查该精确 sp-tag 条目
3. 查本地部署目录有无对应 tarball(或已解压 built/):
   有 → 解压(若未解压)→ exec run-sdcshield.sh "$@"
   无 + 有网 → 从 GitHub Release 按版本拉对应 tarball 到本地缓存 → 解压 → exec
   无 + 气隙 → 硬停:
     "错误: 本机 openEuler-24.03LTS_SP3 无匹配二进制。
      可用版本: [built-index.tsv 列出全部]
      请在同版本机上运行:
        scripts/offline-build/build-all.sh 24.03 SP3 --full
      然后用 package-release.sh 24.03 SP3 产出 tarball 拷来。"
     (绝不静默用相邻 SP 顶替)
```

### 5.2 部署包形态

两种,各司其职:
- **现场预置目录**:运维把需要的几个 SP 的 tarball + 一份 `built-index.tsv` 放同一目录,`run.sh` 从该目录匹配。气隙场景。
- **按需拉取(有网)**:`run.sh` 从 GitHub Release `/releases` 按检测到的版本下载对应 tarball 到 `~/.cache/sdcshield/`,缓存复用。

### 5.3 run.sh 骨架

```bash
#!/bin/bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/_common.sh"   # detect_os_version_full

OS_TAG=$(detect_os_version_full)   # openEuler-24.03LTS_SP3
INDEX="$HERE/built-index.tsv"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/sdcshield"

# 精确匹配
match=$(grep -m1 "^${OS_TAG}\b" "$INDEX" || true)
[ -n "$match" ] || { echo "错误: $OS_TAG 无匹配二进制,见上"; exit 2; }

# 定位 tarball: 本地目录优先,否则缓存,否则下载
tarball=$(find_tarball "$OS_TAG" "$HERE" "$CACHE")
[ -f "$tarball" ] || { download_from_release "$OS_TAG" "$tarball" || hard_stop; }

# 解压到缓存目录
bindir="$CACHE/${OS_TAG}"
[ -x "$bindir/sdcshield" ] || { mkdir -p "$bindir"; tar xzf "$tarball" -C "$bindir"; }

# 校验 binary-sha256(防下载损坏/篡改)
expected_sha=$(echo "$match" | awk '{print $7}')
verify_sha256 "$bindir/sdcshield" "$expected_sha" || { echo "校验失败"; exit 1; }

exec "$bindir/run-sdcshield.sh" "$@"
```

`run-sdcshield.sh` 支持 `full` 首参数(2026-09-15 加入):两段式全核 eigen 满载——第一段 11 个稳定 eigen 测试不带 `-n`(默认系统全部 CPU),第二段 4 个数值敏感测试(`eigen_svd_double`/`eigen_sparse`/`eigen_svd_cdouble`/`eigen_svd_cdouble_sve`)`-n 1` 补跑(规避大规模多线程 ULP 偶发假 FAIL)。默认每测试 60s(`-t` 覆盖);`eigen_svd_cdouble_sve` 自 2026-09-19 起为真实 SVE 向量化压测(Eigen double/complex packet 已补,见 usermanual 3.5 节要点)。详见 `scripts/offline-build/package-built-artifacts.sh` 的 heredoc 与 usermanual 3.5 节。

---

## 6. CI 矩阵

### 6.1 PR CI(角落三 smoke)

`.github/workflows/pr.yaml` 的 `multi-version` job(已落地):
- 目标 SP:**20.03-LTS**(最老 toolchain,适配层回归哨兵)、**22.03-LTS-SP3**、**24.03-LTS-SP3**(基准)。
- 步骤:`podman pull ghcr.io/...:<tag>`(从 Registry,不重建)→ `container-build.sh` → `verify-built-pristine.sh smoke`。
- polyfill/sed 适配一旦被源码改动带歪,这三个先红。

### 6.2 每日全 15 全量验证(已落地:`.github/workflows/multi-os-verify.yml`)

独立 workflow,**原生容器执行模式**:作业直接 `container:` 跑在 ghcr.io 构建镜像里(代码由 `actions/checkout` 自动挂载到容器工作目录),配置最简洁、无 docker 嵌套、无 SELinux `:Z` 竞态。`schedule: '0 4 * * *'`(UTC 04:00 = 北京时间 12:00)每日触发,另可 `workflow_dispatch`(可选 `verify_depth: full|smoke`)。

- **矩阵**:`strategy.matrix.include` 展开 15 个 job(三系列 × LTS+SP1~SP4),`fail-fast: false`,互不拖累,全跑完出结论。
- **runner**:`ubuntu-24.04-arm`(GitHub hosted aarch64,GA,原生支持 `container:` 属性且需 arm64 镜像;本仓镜像即 arm64)。换自建 kunpeng920 runner 改一行 `runs-on`。
- **镜像拉取**:`container.credentials` 用 `GITHUB_TOKEN` 认证(`packages: read` 权限),公开/私有镜像皆可;`username` 固定 `wangxumarshall`(不可用 `github.actor`——schedule 触发时 actor 为空会致 docker login 失败)。前置:15 镜像先 `build-images.sh <s> <sp> --push` 推到 `ghcr.io/wangxumarshall/sdcshield-offline`。
- **每 job 流程**:checkout(`submodules: false`,镜像已烘焙依赖)→ `actions/cache` 缓存 vendored 库构建 → 镜像内 `meson+ninja` 构建 → `scripts/gha/verify-params.py` 全量参数功能测试 → `scripts/gha/benchmark.sh` 采基准 → 上传日志/基准。
- **全量参数扫描**(`verify-params.py`,纯 stdlib 适配镜像无 PyYAML):`--quality=-1`(PROD+BETA+SKIP)、`-n 1/4/8` 三档并发(多线程档 `--disable` eigen 数值类,规避已知 ULP flakiness)、openblas `mdim` 扫谱、selftests `@positive` + 逐条负面(断言非零退出且非 insn 崩溃)。
- **最终 report summary**(`report-summary.py`):`report` job 下载 15 份 `allquality.yaml`,生成一张**「用例 × 版本」结果矩阵**写入 job summary —— 行 = 全部测试用例(~290,15 版本求并集),列 = 15 个 OS 版本;每格 = `<结果态>[<耗时>s]`(`PASS[1.23s]`/`FAIL[0.10s]`/`SKIP[0.00s]`/`TIMEOUT[..]`/`CRASH[..]`/`OSERR[..]`/`INTERRUPTED[..]`/`INVALID[..]`,空 = 该版本无此用例),矩阵尾部附结果态统计。有基线时每格升级为 `<结果态>[<耗时>s (±Δs)]`——Δ = 相对**上次成功全量运行**(report job 用 gh CLI 下载其 `verify-*` artifact,7 天 retention 内,靠有无 `allquality.yaml` 区分全量/smoke)同用例同版本的耗时变化,增加为正,**仅对比耗时**(结果态不参与对比);基线缺失或基线无该用例时不显示 Δ。取代旧的跨 OS 基准墙钟对比表。
- **已知省略(诚实)**:`sleef`(需 cmake;24.03 镜像 cmake 断链缺 `libuv.so.1`、22.03/20.03 无 cmake → 优雅缺席,与 `container-build.sh` 一致)`sleef_neon`/`sleef_sve`;sleef/isal 因构建工具链差异属 24.03 独占,在矩阵中体现为对应版本的空单元格。

脚本与口径:`scripts/gha/`(README、verify-params.py、benchmark.sh、benchmark.md、report-summary.py、package-built.sh)。

### 6.2.1 下载 CI 预构建产物(built/ tarball)

每次 Multi-OS Verify 运行(cron 每日 + 手动 dispatch)都会为 15 个 SP 各产出一个
**自包含 tarball**,结构与本地 podman 链路的 `third-party/rpms/.../built/` 一致:

```
sdcshield-openEuler-<series><LTS_SPx>-<git-sha8>.tar.gz   (~4-8MB)
├── sdcshield            (stripped 二进制)
├── libs/                (非系统自带运行时 .so,详见 scripts/gha/README.md)
├── run-sdcshield.sh     (设 LD_LIBRARY_PATH=libs 后 exec; full 子命令两段式 eigen)
├── MANIFEST.tsv         (文件 sha256 + 体积清单)
├── VERSION              (git sha / series / cpp_std / binary-sha256 / build-hash)
└── BUILD-HASH           (与 build-all.sh compute_build_hash 公式一致)
```

**获取方式**(网页):仓库 → Actions → 任意一次 Multi-OS Verify 运行 → 页面底部
Artifacts 区 → `built-<series>-<sp>`(15 个,各含一个 tar.gz),点击下载 zip(解
压得 tar.gz)。保留期 90 天(公开仓默认),滚动覆盖。

**命令行**(需 `gh auth login`):

```bash
gh run list --workflow=multi-os-verify.yml --limit 5        # 找 run id
gh run download <run-id> --name built-24.03-SP3 -D ./pkg    # 下载单个
# 或一次全部:
gh run download <run-id> -p 'built-*' -D ./all-pkgs
```

**使用**(目标机为对应 openEuler 版本):

```bash
tar -xzf sdcshield-openEuler-24.03LTS_SP3-<sha8>.tar.gz -C sdcshield-pkg
cd sdcshield-pkg/openEuler-24.03LTS_SP3
./run-sdcshield.sh --list-tests          # 常规用法,参数原样透传
./run-sdcshield.sh -e zstd19 -t 5000     # 跑单个测试
./run-sdcshield.sh full                  # 全核满载 eigen(两段式,见脚本头注释)
```

**产物闸门**:workflow 的 `Package smoke-verify` step 在上传前解开 tarball、只设
`LD_LIBRARY_PATH=libs` 实跑(list-tests >100 + zstd19 `exit:pass` 双断言),失败即
红 —— 打出来的包保证能独立运行,不静默降级。

**与本地 built/ 的关系**:同 commit 时 BUILD-HASH 相等(公式与
`build-all.sh compute_build_hash` 完全一致,双向实测验证)—— GHA 产物与本地
podman 链路产物是同输入配置的等价物。binary-sha256 不保证 bit 级一致(构建路径
细节差异)。长期版本沉淀仍走本地 `package-release.sh` 的 dist/ tarball 路线
(RPM submodule built/ + git 追溯),CI artifact 是按 run 滚动的"最新版"获取通道。

**永久保存(Release,人工选择)**:artifact 90 天过期,不是版本库。需要钉住某个
版本时,workflow_dispatch 勾选 **`publish_release`** 触发——该次 run 末尾的
`release` job 把 15 个 tarball 作为 Release 资产上传(tag 形如
`build-YYYYMMDD-<sha8>`),**永久保留**(除非手动删),网页 Releases 页面或:

```bash
gh release list                        # 列出钉住的版本
gh release download build-20260920-abc12345 -D ./rel  # 取回 tarball
```

发布语义:里程碑式手动钉版(重要合入后);nightly 由 90 天滚动 artifact 天然
承担,不当 nightly 用。cron 触发时 `publish_release` 恒为否,行为不变。

### 6.3 Runner 选型

- **GitHub hosted arm64**(`ubuntu-24.04-arm`):默认,零维护,原生容器模式直接可用。
- self-hosted aarch64 runner(kunpeng920 本机/同架构机):原生速度,改 `runs-on` 一行即可;同样走原生容器模式拉 ghcr 镜像。

---

## 7. 适配层收敛(让 15 路构建可维护)

`container-build.sh` 里那串 perl sed(`.contains()`→`.find()`、`udf`→`.inst`、ACL 置空)在 15 路构建下是**脆点放大 15 倍**。收敛目标:容器内对源码副本的修改数 → 0,只剩 CXXFLAGS/polyfill 注入。

| 现状 sed | 收敛方案 | 落点 |
|---|---|---|
| `string::contains`→`.find()`(3 处) | 并进 `framework/compat/`:polyfill 头里加 `#define contains find`(粗)或 compat `<string>` shim | `framework/compat/` |
| `udf #0x1234`→`.inst 0x00001234` | 源码 `#ifdef OPENEULER_20_03` 走 `.inst` 分支(additive,合规) | `framework/selftest.cpp` |
| ACL 子集置空(sed 改 meson.build) | 改成 meson option `-Denable_acl=auto`(无 ACL 库时空集) | `tests/cpu/arithmetic_arm/meson.build` |

收敛后:`container-build.sh` 不再 `perl -pi -e` 改源码行,只剩 `CXXFLAGS_EXTRA` 注入 polyfill 头 + 版本宏。这是 15 路构建可维护性的地基。

---

## 8. 目标目录与文件总图

```
sdcshield/
├── scripts/offline-build/
│   ├── _common.sh                    (已有: 版本检测)
│   ├── download-all-versions.sh      (已有: 15 SP RPM 下载)
│   ├── container-build.sh            (已有/改: 杠杆1 跳过装包)
│   ├── package-built-artifacts.sh    (已有/增强: 写 BUILD-HASH+MANIFEST+VERSION)
│   ├── verify-built-pristine.sh      (已有: 发布闸门)
│   ├── run-full-tests.sh             (已有)
│   ├── install-deps.sh / download-deps.sh / build.sh / supplement-20.03-gcc10.sh  (已有: 单版本路径)
│   ├── build-all.sh                  ★新: 15 矩阵编排(5 杠杆)
│   ├── package-release.sh            ★新: 现场 tarball
│   ├── built-index.tsv              ★新: 跨 SP 总表(随 release 更新)
│   └── images/                      ★新: 镜像子系统
│       ├── Containerfile.template    ★新: 一模板 + SP 参数化(<3KB)
│       ├── build-images.sh           ★新: 构建/推 Registry/tar(<4KB)
│       └── image-manifest.tsv        ★新: 镜像 digest 追溯(<2KB)
├── framework/compat/
│   ├── cpp23_polyfill.h             (已有: C++23 polyfill)
│   └── (收敛后: string/contains shim, selftest udf 不再 sed)
└── third-party/rpms/                 (submodule, 已有)
    ├── openEuler-20.03/
    │   ├── openEuler-20.03LTS[_SPx]/rpms/   (该 SP 全部 *.rpm)
    │   └── openEuler-20.03LTS[_SPx]/built/  (产物沉淀)
    ├── openEuler-22.03/   (同上: 每 SP 一个 rpms/ + built/)
    └── openEuler-24.03/   (同上)
```

主仓新增总体积:`Containerfile.template` + `build-images.sh` + `build-all.sh` + `package-release.sh` + 两个 tsv ≈ **< 25KB**。永不膨胀。

---

## 9. 落地顺序(one-patch-per-unit,各自 verify→push 非 main)

| # | patch(单元) | 价值 | 关键验证 | 依赖 |
|---|---|---|---|---|
| 1 | `images/Containerfile.template` + `build-images.sh` | 镜像可复现 + 入仓 | `podman build` 成功;镜像内 `gcc --version` 正确;`podman run` 容器内 `find`/`ld` 可用 | — |
| 2 | 改 `container-build.sh`:镜像 deps 就绪则跳过装包(杠杆1) | 单构建提速最猛 | 24.03-SP3 `built/` 纯净验证仍 PASS | 1 |
| 3 | `build-all.sh` 串行版 + smoke/full(杠杆5) | 一条命令跑矩阵 | `build-all.sh 24.03 all --smoke` 5 个 SP 全 PASS | 2 |
| 4 | `build-all.sh` 加源码哈希 skip(杠杆2)+ `--since`(杠杆4) | 稳态增量 | 改无关文件→skip;改源码→只重建受影响 SP | 3 |
| 5 | `build-all.sh` 加 `-P` 并行(杠杆3) | 墙上时间 | 全 15 并行无 OOM;结果与串行一致 | 4 |
| 6 | `package-built-artifacts.sh` 写 BUILD-HASH+MANIFEST+VERSION | 溯源 + 为 skip/匹配铺路 | 产物含三份元数据,sha256 校验通过 | 3 |
| 7 | `package-release.sh` + `built-index.tsv` | 现场 tarball | tarball ~20MB;解压后纯净容器跑通 | 6 |
| 8 | `run.sh` 目标机精确匹配 + 不回退 | 部署侧落地 | 匹配 SP→exec;不匹配→硬停指路 | 7 |
| 9 | CI `multi-version` job(角落三 smoke) | 回归哨兵 | PR 上三 SP smoke 全绿 | 3 |
| 10 | 收敛 sed 进 compat/`#ifdef` | 15 路可维护性 | 22.03/20.03 构建不再 sed 改源码;验证仍 PASS | 3 |

每 patch 遵守 CLAUDE.md 自验证:build clean + 真实命令输出 + 回归(zstd19 pass)+ x86-64 非回归(改动在 `#elif __aarch64__`/per-arch meson/`scripts/` 下)。

---

## 10. 效率指标(目标)

| 操作 | 现状 | 目标(方案2 全量落地后) |
|---|---|---|
| 改一行源码→全 15 验证 | 15×(装包+编译+验证)串行,~小时级 | 杠杆1+2+3:多数 skip,受影响 SP 并行,~10 分钟级 |
| 新机器起步 | 手动构建镜像 | `build-images.sh --all --push`,幂等 skip |
| 气隙部署 | 手动拷镜像+二进制 | `run.sh` 自动匹配或 `package-release` tarball |
| 回归追溯 | 镜像不可复现 | image-manifest + built-index 全 digest 追溯 |
| PR 多版本回归 | 无(CI 只跑 debian:sid) | 角落三 smoke 哨兵 |

---

## 11. 待确认决策点

1. **镜像基座策略**:B1(`FROM quay.io/openeuler/openeuler:<series>` + COPY SP RPM 强装,完全可复现) vs B2(复用现有手动 KIWI SP 镜像作 base)。推荐 B1。
2. **Registry 可达性**:本机能否 `podman push ghcr.io/wangxumarshall/...`?能则主路径推 Registry;不能则 `build-images.sh --tar` 走 oci tar 包流转(两条路径都实现)。
3. **CI runner**:self-hosted aarch64(推荐,原生速度)vs GitHub hosted arm64。
4. **并行度默认**:`-P 4` + `ninja -j 16`(192 核箱,eigen 编译吃内存,不拉满)。可调。
5. **部署拉取源**:GitHub Release(公开)vs ghcr.io(需 token)。影响 `run.sh` 按需下载路径。

确认 1、2 后即可从 patch 1(`build-images.sh` + `Containerfile.template`)开干。
