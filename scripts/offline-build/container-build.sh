#!/bin/bash
# container-build.sh — 在 podman openEuler 容器内离线构建 SDCShield-arm 并验证。
#
# 用法:
#   ./container-build.sh <series> <sp> [extra-meson-args...]
#     series: 24.03 | 22.03 | 20.03
#     sp   : LTS | SP1 | SP2 | SP3 | SP4
#
# 示例:
#   ./container-build.sh 24.03 SP3
#   ./container-build.sh 22.03 SP3
#   ./container-build.sh 20.03 SP4
#
# 设计要点(满足 CLAUDE.md 约束):
#   - 源码树只读挂载 (/src),容器内不可写 → 24.03 源码不可能被污染。
#   - 22.03/20.03 的全部适配通过 CXXFLAGS 注入:
#       -DOPENEULER_22.03 (或 OPENEULER_20.03)
#       -include /src/framework/compat/cpp23_polyfill.h   # polyfill,不改既有源码
#       (meson 的 -Dcpp_std=gnu++20 命令行覆盖,不改 meson.build)
#       vendored ACL 容器内原生重建(third-party/acl, 22.03/20.03 需 supplement-cmake.sh)
#   - meson_version:源码已声明 >=0.56(22.03 0.59 与 20.03 vendored 0.59.4 均满足),
#     无需 sed 放宽。
#   - 产物拷出到 host: build-out/openEuler-XX.03LTS_SPx/ (bin + log + ldd清单)。
#
# SPDX-License-Identifier-Identifier: Apache-2.0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# SDCSRC_ROOT:允许调用方传入一份源码副本的根目录(与真实仓同构),用于
# 多系列并行构建场景 — 每系列一份副本挂 /src,避免共享 /src 挂载被多个
# 容器并发 :Z relabel 产生 Permission denied 竞态(relabel 期间 cp -a 会
# 读到临时不可读的文件,残缺源码 → meson 缺文件错误)。
# 副本必须包含:framework/ tests/ bats/ scripts/ meson.build meson_options.txt
# third-party/(含 eigen5 + vendored install/,rpms 子目录可选 — RPM 另行挂载)。
SDCSRC_ROOT="${SDCSRC_ROOT:-$SRC_ROOT}"

SERIES="${1:?usage: $0 <series> <sp> [meson-args]}"
SP="${2:?usage: $0 <series> <sp> [meson-args]}"
shift 2 || true
EXTRA_MESON=("$@")

# 规范化 SP 标签(镜像 tag 用 LTS-SPx,目录名用 LTS_SPx)
case "$SP" in
    LTS)   SP_LABEL="LTS";    SP_DIR="LTS" ;;
    SP[1-4]) SP_LABEL="LTS-$SP"; SP_DIR="LTS_$SP" ;;
    *) echo "bad SP: $SP" >&2; exit 1 ;;
esac

OS_TAG="openEuler-${SERIES}${SP_DIR}"
IMG="localhost/openeuler-offline:${SERIES}-${SP_LABEL}"
RPMDIR_HOST="$SRC_ROOT/third-party/rpms/openEuler-${SERIES}/${OS_TAG}/rpms"
OUTDIR_HOST="$SRC_ROOT/build-out/${OS_TAG}"

[ -d "$RPMDIR_HOST" ] || { echo "RPM dir missing: $RPMDIR_HOST" >&2; exit 1; }
ls "$RPMDIR_HOST"/*.rpm >/dev/null 2>&1 || { echo "no rpms in $RPMDIR_HOST" >&2; exit 1; }

echo "==> 镜像: $IMG"
echo "==> RPM 目录(host): $RPMDIR_HOST"
echo "==> 产物输出(host): $OUTDIR_HOST"

# host 端准备输出目录
mkdir -p "$OUTDIR_HOST"

# 容器内入口脚本:装依赖 + 构建 + 测试,所有产物落 /out (映射到 host OUTDIR)
# inner-build.sh 用 SP-specific 路径(并发 worker 各自一份,避免覆盖竞态)。
INNER=/tmp/inner-build.sh
INNER_HOST="$SRC_ROOT/build-out/inner-build-${OS_TAG}.sh"
cat > "$INNER_HOST" <<'INNER_EOF'
#!/bin/bash
set -euo pipefail

# 从容器 env 读参数(OS_TAG, EXTRA_MESON 数组, OS_SERIES)
RPMDIR="/rpms"
SRC="/src"
BUILD="/build"
OUT="/out"

# 创建构建工作目录 (容器内可写) + /var/tmp (最小镜像缺, rpm %trigger 脚本需要)
mkdir -p "$BUILD" "$OUT" /var/tmp /tmp

# ===== 杠杆 1: 镜像已烘焙依赖则跳过安装 =====
# 由 build-images.sh 构建的镜像(localhost/openeuler-offline:...)在镜像层里已
# rpm -Uvh --nodeps --force 装完整棵依赖树 + 建 ld 链接(见 Containerfile.template)。
# 检测信号: 镜像自带 find + gcc → deps 已就绪, 跳过 [1/5] 整段安装, 直接进 toolset 激活。
# (20.03 的 /usr/bin/gcc 可能是 gcc-7 或缺失, 但 toolset 激活块会用 /opt/.../gcc-10
#  覆盖 PATH/CC/CXX; 那是"怎么用 deps"不是"装 deps", 仍要跑。故此处只判 find。)
if command -v find >/dev/null 2>&1; then
    echo "===== [1/5] 依赖已就绪(镜像烘焙) — 跳过安装 ====="
    echo "  gcc: $(gcc --version 2>/dev/null | head -1 || echo 'toolset 激活后见下')"
else
    echo "===== [1/5] 离线安装依赖 ($RPMDIR) ====="
    # 最小镜像缺 find, 先用 rpm 直装 findutils (--nodeps, 依赖已在镜像)。
    FINDUTILS_RPM=$(ls "$RPMDIR"/findutils-*.rpm 2>/dev/null | head -1 || true)
    if [ -n "$FINDUTILS_RPM" ] && ! command -v find >/dev/null 2>&1; then
        echo "  预装 findutils (镜像缺 find)..."
        rpm -Uvh --nodeps --force "$FINDUTILS_RPM" >/dev/null 2>&1 || true
    fi
    command -v find >/dev/null 2>&1 || { echo "ERROR: find 仍不可用" >&2; exit 1; }

    # 容器是最小化 KIWI 镜像, 系统包(util-linux/libuuid 等)版本与 RPM 树来源子版本号
    # 略有差异(如 -31 vs -39), dnf 的精确版本依赖会触发 "protected dnf" 死结。
    # 容器是可丢弃环境, 用 rpm --nodeps --force 直接强装整棵依赖树(等价于
    # install-deps.sh 的兜底方案 4), 依赖物理上都在 RPM 树里, 强装后能正常工作。
    EXCLUDE_RE='grub2|shim|mokutil|efivar|dracut|kpartx|fuse|glibc$|glibc-common|glibc-headers|glibc-static|systemd$|systemd-libs|systemd-udev|setup$|filesystem$|basesystem|shadow|pam|crypto-policies|openEuler-release|openEuler-gpg|openEuler-repos'
    KEEP=$(find "$RPMDIR" -maxdepth 1 -name '*.rpm' | grep -ivE "$EXCLUDE_RE")
    echo "  强装 $(echo "$KEEP" | wc -l) 个 RPM (--nodeps --force)..."
    rpm -Uvh --nodeps --force $KEEP >/tmp/rpm-install.log 2>&1 || true

    # 大批量 --nodeps --force 会因文件冲突静默跳过部分关键包(如 glibc-devel 提供
    # crt1.o, binutils 提供 ld)。显式重装这几个关键包, 保证 toolset gcc 能链接。
    # 注: 某些包(如 glibc-headers)在 20.03 不存在, ls 无匹配会非零退出 → 用 || true
    # 兜底, 否则 set -e 会中断脚本。
    for mustpkg in glibc-devel glibc-headers binutils gcc-toolset-10-gcc gcc-toolset-10-gcc-c++ gcc-toolset-10-libstdc++-devel gcc-toolset-10-libgcc; do
        f=$(ls "$RPMDIR"/${mustpkg}-*.rpm 2>/dev/null | head -1) || true
        [ -n "$f" ] && rpm -Uvh --nodeps --force "$f" >/dev/null 2>&1 || true
    done
    echo "  安装完成; gcc 版本: $(gcc --version 2>/dev/null | head -1 || echo MISSING)"

    # 20.03 + 22.03 的最小镜像缺 ld 符号链接: binutils 装了 ld.bfd 但
    # /usr/bin/ld → /etc/alternatives/ld 的 alternatives 链未建(--nodeps 跳了
    # %post 脚本依赖的 chkconfig/alternatives)。手动建直链, 让 collect2 找到 ld。
    if [ ! -e /usr/bin/ld ] && [ -e /usr/bin/ld.bfd ]; then
        ln -sf /usr/bin/ld.bfd /usr/bin/ld
        echo "  建 /usr/bin/ld → ld.bfd 符号链接"
    fi
fi

# ld 符号链接兜底对"镜像烘焙分支"同样必要: 烘焙镜像(依赖已就绪跳过安装)也可能缺
# /usr/bin/ld(尤其 20.03-SP4 这种 toolset 靠自愈强装的场景, collect2 需要系统 ld)。
if [ ! -e /usr/bin/ld ] && [ -e /usr/bin/ld.bfd ]; then
    ln -sf /usr/bin/ld.bfd /usr/bin/ld
    echo "  建 /usr/bin/ld → ld.bfd 符号链接(烘焙分支兜底)"
fi

# 20.03: gcc-10 以 SCL gcc-toolset-10 形式安装(在 /opt/openEuler/gcc-toolset-10/root/)。
# 系统默认 gcc 是 7.3(无 C++20/23)。激活 toolset: 把其 bin 加 PATH, 设 CC/CXX,
# 设 lib/include 搜索路径(LDFLAGS/CPPFLAGS/-isystem), 让 meson 用 gcc-10 而非 gcc-7。
TOOLSET_ROOT="/opt/openEuler/gcc-toolset-10/root"
if [ -n "${OPENEULER_MACRO:-}" ] && [ ! -d "$TOOLSET_ROOT/usr/bin" ]; then
    # 部分镜像(实测 20.03-LTS-SP4)烘焙层缺 toolset。RPM 树里有全套 gcc-toolset-10-*,
    # 此处从 /rpms 强装(镜像层"依赖已就绪跳过安装"分支不覆盖这种缺包,兜底在此)。
    echo "  toolset 缺失于镜像层,从 RPM 树强装 gcc-toolset-10..."
    for tspkg in gcc-toolset-10-runtime gcc-toolset-10-gcc gcc-toolset-10-cpp gcc-toolset-10-gcc-c++ \
                 gcc-toolset-10-binutils gcc-toolset-10-libstdc++-devel \
                 gcc-toolset-10-libgcc gcc-toolset-10-libatomic gcc-toolset-10-libgomp \
                 libmpc mpfr gmp binutils glibc-devel gcc-toolset-10-libstdc++; do
        f=$(ls "$RPMDIR"/${tspkg}-*.rpm 2>/dev/null | head -1) || true
        [ -n "$f" ] && rpm -Uvh --nodeps --force "$f" >/dev/null 2>&1 || true
    done
fi
if [ -n "${OPENEULER_MACRO:-}" ] && [ -d "$TOOLSET_ROOT/usr/bin" ]; then
    TS_BIN="$TOOLSET_ROOT/usr/bin"
    # toolset 的 gcc/g++ 二进制名为 gcc/g++(非 gcc-10); 加 PATH 优先于系统 gcc-7
    export PATH="$TS_BIN:$PATH"
    export CC="$TS_BIN/gcc"
    export CXX="$TS_BIN/g++"
    # toolset 的 libstdc++ 头/库在 root/usr/include 与 root/usr/lib64
    TS_LIB="$TOOLSET_ROOT/usr/lib64"
    export LDFLAGS="-L$TS_LIB -Wl,-rpath,$TS_LIB ${LDFLAGS:-}"
    # 让 meson 的 cpp.check_header / dependency 能找到 toolset 的 boost/gmp 等
    export PKG_CONFIG_PATH="$TOOLSET_ROOT/usr/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
    echo "  激活 gcc-toolset-10: $(gcc --version 2>/dev/null | head -1)"
    echo "  CC=$CC CXX=$CXX"
fi

echo "===== [2/5] 工具链自检 ====="
for cmd in gcc g++ meson ninja perl python3 pkg-config ar; do
    printf "  %-9s %s\n" "$cmd" "$($cmd --version 2>/dev/null | head -1 || echo MISSING)"
done

echo "===== [3/5] meson setup ====="
# 把只读 /src 拷一份到可写 /build/src(供 meson setup 用,不再 sed 改源码)。
cp -a "$SRC" "$BUILD/src"
SRCW="$BUILD/src"

# ---- vendored 第三方库容器内原生重建(仅当宿主产物与本容器 glibc 不兼容) ----
# third-party/{openssl,openblas,sleef}/install/ 是构建宿主(24.03, glibc 2.38)
# 上 build.sh 的产物。静态 .a 里可能引用宿主特有符号(实测 __isoc23_strtol,
# glibc 2.38 的 C23 变体),在老容器(22.03 glibc 2.34 / 20.03 glibc 2.28)链接
# 失败:undefined reference to `__isoc23_strtol'(openblas_env.c / e_afalg.c /
# rand_unix.c)。检测:install/.glibc-build-tag(宿主 build.sh 写入或此处写入)
# 与当前容器 ldd --version 不一致 → 删掉 install/,在容器内用该系列原生工具链重建。
# 注意:不能用 `ldd --version | head -1 | grep ...` 链 — inner 脚本 set -o pipefail,
# head 提前关闭管道使 ldd 收 SIGPIPE(141),|| echo unknown 兜底会把 "unknown" 追加在
# grep 已输出的版本号后面,VENDORED_GLIBC 变成两行 "2.38\nunknown" → tag 比较永假 →
# 每次都重建(2026-09-16 24.03-SP3 重验实测踩坑)。getconf 是单命令,无管道无 SIGPIPE。
VENDORED_GLIBC="$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}' || true)"
VENDORED_GLIBC="${VENDORED_GLIBC:-unknown}"
# 20.03 最小 KIWI 镜像无 GNU tar,只有 bsdtar(libarchive);兼容两者。
TAR_BIN="$(command -v tar || command -v bsdtar || echo tar)"
rebuild_vendored() {
    local name="$1"   # openssl | openblas
    local dir="$SRCW/third-party/$name"
    local tagfile="$dir/install/.glibc-build-tag"
    local built_on
    built_on=$(cat "$tagfile" 2>/dev/null || echo host-unknown)
    if [ "$built_on" = "$VENDORED_GLIBC" ] && [ -d "$dir/install/lib" ]; then
        echo "  vendored $name: install/ 是本容器 glibc $VENDORED_GLIBC 产物,复用"
        return 0
    fi
    echo "  vendored $name: install/ 构建于 glibc $built_on != $VENDORED_GLIBC,容器内重建..."
    rm -rf "$dir/install"
    case "$name" in
        openssl)
            rm -rf "$dir/openssl-3.5.0"
            mkdir -p "$dir/openssl-3.5.0"
            "$TAR_BIN" xzf "$dir/openssl-3.5.0.tar.gz" -C "$dir/openssl-3.5.0" --strip-components=1
            # no-asm:openEuler 22.03 LTS 基线镜像的 binutils 2.37-25 汇编 OpenSSL 的
            # SM4 ARMv8-CE 汇编(vpaes-armv8.pl 生成的 vpsm4 路径)产出错误机器码 —
            # SM4-CBC 解密自第二块起确定性错位(实测 -s LCG:614153748 100% 复现,
            # openssl_sm3sm4 roundtrip mismatch @offset16;22.03-SP1..SP4 的 binutils
            # 2.37-23 与 24.03 host binutils 2.41 均正常;同容器裸 EVP harness 5 万次
            # 随机 + no-asm 对照组 0 失败)。容器内重建统一 no-asm:SM3/SM4 走纯 C,
            # 性能略降换正确性(稳定性验证语境优先正确性)。
            ( cd "$dir/openssl-3.5.0" \
              && ./Configure linux-aarch64 --prefix="$dir/install" \
                   no-shared no-tests no-docs no-apps --release no-asm \
              && make -j"${VENDORED_JOBS:-16}" build_sw \
              && (make install_sw 2>/dev/null || make install_dev) )
            ;;
        openblas)
            rm -rf "$dir/OpenBLAS-0.3.29"
            "$TAR_BIN" xzf "$dir/OpenBLAS-0.3.29.tar.gz" -C "$dir"
            ( cd "$dir/OpenBLAS-0.3.29" \
              && make -j"${VENDORED_JOBS:-16}" TARGET=TSV110 USE_THREAD=0 USE_LOCKING=1 \
                   NO_SHARED=1 NOFORTRAN=1 NO_AFFINITY=1 \
              && make PREFIX="$dir/install" NO_SHARED=1 USE_LOCKING=1 install )
            [ -f "$dir/install/lib/libopenblas_tsv110-r0.3.29.a" ] && \
                ln -sf libopenblas_tsv110-r0.3.29.a "$dir/install/lib/libopenblas.a"
            ;;
    esac
    # tag 含 "-noasm" 后缀:asm 时代的 tag(裸 glibc 版本号)不匹配 → 迁移一次重建;
    # 之后同 glibc + no-asm 的产物复用。
    echo "$VENDORED_GLIBC-noasm" > "$dir/install/.glibc-build-tag"
}
if [ -d "$SRCW/third-party/openssl/install" ]; then
    rebuild_vendored openssl || echo "  WARNING: openssl 容器内重建失败,回退宿主产物" >&2
fi
if [ -d "$SRCW/third-party/openblas/install" ]; then
    rebuild_vendored openblas || echo "  WARNING: openblas 容器内重建失败,回退宿主产物" >&2
fi
# sleef 需要 cmake;22.03/20.03 离线容器无 cmake(镜像和 RPM 树都没有完整 cmake 包)。
# 宿主 libsleef.a 同样有 __isoc23_strtol 风险 → 在无 cmake 的容器里删掉 install/,
# meson 探测不到即优雅缺席 sleef_neon/sleef_sve(与 isal 在无 libisal 镜像的缺席同模式)。
if ! command -v cmake >/dev/null 2>&1; then
    if [ -d "$SRCW/third-party/sleef/install" ]; then
        echo "  vendored sleef: 容器无 cmake,移除宿主产物(sleef 测试本镜像缺席)"
        rm -rf "$SRCW/third-party/sleef/install"
    fi
else
    # 容器有 cmake(如 24.03):宿主产物 glibc 相同(镜像==host 同 SP)时
    # rebuild_vendored 的 tag 检查自然跳过;不同则下述重建(sleef 用 cmake)。
    if [ -d "$SRCW/third-party/sleef/install" ]; then
        sleef_tag=$(cat "$SRCW/third-party/sleef/install/.glibc-build-tag" 2>/dev/null || echo host-unknown)
        if [ "$sleef_tag" != "$VENDORED_GLIBC" ]; then
            echo "  vendored sleef: install/ 构建于 glibc $sleef_tag != $VENDORED_GLIBC,容器内重建..."
            rm -rf "$SRCW/third-party/sleef/install" "$SRCW/third-party/sleef/build"
            ( cd "$SRCW/third-party/sleef" \
              && "$TAR_BIN" xzf sleef-3.9.0.tar.gz \
              && cmake -S sleef-3.9.0 -B build \
                   -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
                   -DSLEEF_ENABLE_TLFLOAT=OFF \
                   -DCMAKE_INSTALL_PREFIX="$PWD/install" \
                   -DCMAKE_INSTALL_LIBDIR=lib \
              && cmake --build build -j"${VENDORED_JOBS:-16}" \
              && cmake --install build )
            echo "$VENDORED_GLIBC" > "$SRCW/third-party/sleef/install/.glibc-build-tag"
        fi
    fi
fi

# meson_version:源码已声明 >=0.56(project_source_root 引入版,22.03 的 0.59 与
# 20.03 vendored 0.59.4 均满足),无需再 sed 放宽。

# udf 助记符:源码已用 .inst 0x00001234(.inst 伪指令,新旧 binutils 统一支持,
# 同 UDF #0x1234 编码,同 SIGILL)。无需再 sed-patch。见 selftest.cpp 注释。

# std::string/string_view::contains (C++23, GCC10 缺):源码 3 个调用点已改为
# 可移植的 .find()==npos 比较(C++17, GCC10/12 通用)。无需再 sed-patch。
# 注: std::map::contains (C++20) GCC10 已支持, 不在涉及范围内。

# ACL (Arm Compute Library): vendored 源码构建(third-party/acl, v23.02),
# 与 openssl/openblas/sleef 同一模式: install/ 的 glibc-build-tag 与本容器
# 不同则容器内原生重建(ACL CMakeLists 要求 gcc>=10.2: 24.03 系统 gcc 12.3,
# 22.03 系统 gcc 10.3, 20.03 用已激活的 gcc-toolset-10, 三者均满足)。
# ACL 需要 cmake>=3.13: 24.03 镜像自带;22.03/20.03 镜像无 cmake 时尝试
# /rpms 树里的 cmake RPM(supplement-cmake.sh 预下);仍无则删除宿主 install/
# 让 meson 优雅缺席 2 个 ACL 测试(与 sleef 无 cmake 时同模式)。
ACL_OPT=""
if [ -d "$SRCW/third-party/acl/install" ]; then
    acl_tag=$(cat "$SRCW/third-party/acl/install/.glibc-build-tag" 2>/dev/null || echo host-unknown)
    if [ "$acl_tag" = "$VENDORED_GLIBC" ] && [ -d "$SRCW/third-party/acl/install/lib" ]; then
        echo "  vendored ACL: install/ 是本容器 glibc $VENDORED_GLIBC 产物,复用"
    elif command -v cmake >/dev/null 2>&1; then
        echo "  vendored ACL: install/ 构建于 glibc $acl_tag != $VENDORED_GLIBC,容器内重建..."
        rm -rf "$SRCW/third-party/acl/install" "$SRCW/third-party/acl/build"
        ( cd "$SRCW/third-party/acl" \
          && "$TAR_BIN" xzf ComputeLibrary-23.02.tar.gz \
          && bash build.sh )
        echo "$VENDORED_GLIBC" > "$SRCW/third-party/acl/install/.glibc-build-tag"
    else
        echo "  vendored ACL: 容器无 cmake(22.03/20.03 需先在有网机跑 supplement-cmake.sh 下 RPM 到 rpms 树顶层),移除宿主产物(ACL 测试本镜像缺席)"
        rm -rf "$SRCW/third-party/acl/install"
    fi
fi

# CXXFLAGS 注入 polyfill 头 + 版本宏 + compat/ 系统头路径 (只对 22.03/20.03;24.03 不注入)
# -isystem compat: 让源码里的 #include <barrier> 命中 compat/barrier shim(GCC10 无该头)
# -include unistd.h: GCC10 头文件卫生比 GCC12 严, 部分 ARM64 测试源码
#   (vector/kreg1.cpp/kreg2.cpp/insert_extract.cpp) 调 getpid() 但未 #include <unistd.h>;
#   GCC12 靠传递包含放过, GCC10 报 "not declared"。用 -include unistd.h 无害兜底
#   (不改动任何既有源码行, 仅在 22.03/20.03 构建注入)。
POLYFILL_HDR="$SRCW/framework/compat/cpp23_polyfill.h"
COMPAT_DIR="$SRCW/framework/compat"
CXXFLAGS_EXTRA=""
if [ -n "${OPENEULER_MACRO:-}" ] && [ -f "$POLYFILL_HDR" ]; then
    CXXFLAGS_EXTRA="-D${OPENEULER_MACRO} -include $POLYFILL_HDR -include unistd.h -isystem $COMPAT_DIR"
    echo "  注入: $CXXFLAGS_EXTRA"
fi

export PKG_CONFIG_PATH="$SRCW/third-party/eigen5"
# CXXFLAGS 让 meson 合并 -D/-include 到 cpp_args
export CXXFLAGS="${CXXFLAGS_EXTRA} ${CXXFLAGS:-}"
export CPPSTD="${CPPSTD:-gnu++23}"

rm -rf "$BUILD/builddir"
meson_setup() { ${MESON_BIN:-meson} "$@"; }
meson_setup setup "$BUILD/builddir" "$SRCW" --buildtype=release -Dcpp_std="$CPPSTD" $ACL_OPT ${EXTRA_MESON_ARGS:-}

echo "===== [4/5] ninja ====="
# 统一走受限并行度入口（默认 -j = nproc/2; 容器内 nproc 即容器可见核数）
"$SRCW/scripts/build.sh" -C "$BUILD/builddir"

BIN="$BUILD/builddir/sdcshield"
echo "===== [5/5] 功能验证 ====="
ls -la "$BIN"
N=$("$BIN" --list-tests 2>/dev/null | wc -l)
echo "list-tests: $N"
[ "$N" -gt 100 ] || { echo "ERROR: too few tests ($N)" >&2; exit 1; }

echo "--- zstd19 -n 1 ---"
"$BIN" -e zstd19 -t 2000 -n 1 2>&1 | grep -iE "exit|result" | head -3 || true

# 拷产物到 /out
cp "$BIN" "$OUT/sdcshield"
ldd "$BIN" 2>/dev/null | awk '{print $1}' | sort -u > "$OUT/ldd-libs.txt" || true
echo "===== 容器内构建完成 ====="
INNER_EOF
chmod +x "$INNER_HOST"

# 决定注入宏与 cpp_std
# 注: 宏名用 OPENEULER_22_03 / OPENEULER_20_03 (点号→下划线),因 C 预处理器宏标识符
# 不允许含点号 ('.')。语义与目标意图一致,仅隔离 22.03/20.03 适配,不触碰 24.03。
# 20.03 特殊: 系统自带 meson 0.54 + python3.7 太旧(meson 0.59 RPM 也跑不动,需
# importlib.metadata / py3.8+)。故 20.03 用 meson 0.59.4 源码包(入仓
# third-party/meson/meson-0.59.4, 纯 Python, 兼容 py3.7)直接 python3 meson.py 运行。
case "$SERIES" in
    24.03) OEU_MACRO="";        CPPSTD="gnu++23"; MESON_BIN="meson" ;;
    22.03) OEU_MACRO="OPENEULER_22_03"; CPPSTD="gnu++20"; MESON_BIN="meson" ;;
    20.03) OEU_MACRO="OPENEULER_20_03"; CPPSTD="gnu++20"; MESON_BIN="python3 /meson-src/meson.py" ;;
    *) echo "bad series"; exit 1 ;;
esac

# extra meson args (数组转空格串)
echo "==> 启动 podman 容器构建..."
# ACL 已 vendor 到 third-party/acl(随 /src 挂载进容器, 容器内原生重建,
# 见 inner 脚本的 vendored ACL 段)。不再挂载 host 的 arm-opt-install 头树或
# /usr/lib64/libarm_compute.so, 也不再传 -Dacl_incdir。
# clang-rt builtins: host 路径(bind 挂到同名位置; 24.03 容器镜像==host 同 SP 有效)。
CLANG_RT="/usr/lib/clang/17"
EXTRA_MESON_STR="${EXTRA_MESON[*]:-}"
# :Z 让 podman 给挂载点打 SELinux 私有标签(容器可 exec 挂载的脚本/二进制),
# 否则 SELinux enforcing 系统会 "Permission denied"(与 verify-built-pristine.sh 一致)。
MOUNTS=(-v "$SDCSRC_ROOT:/src:ro,Z" -v "$RPMDIR_HOST:/rpms:ro,Z" -v "$OUTDIR_HOST:/out:Z" -v "$INNER_HOST:$INNER:ro,Z")
# 20.03 用 meson 源码包 (RPM 版的 meson 0.59 跑不动于 python3.7)。
# 源码包入仓 third-party/meson/meson-0.59.4(11M, 纯源码, 可复现)。
[ "$SERIES" = "20.03" ] && [ -d "$SDCSRC_ROOT/third-party/meson/meson-0.59.4" ] && \
    MOUNTS+=(-v "$SDCSRC_ROOT/third-party/meson/meson-0.59.4:/meson-src:ro")
# ACL: 无 host 挂载(third-party/acl 随 /src 进容器, 容器内重建)。
# 22.03/20.03 的 cmake RPM 由 supplement-cmake.sh 预下到 rpms 树顶层(容器内
# 自愈安装);rpms 树整体已挂载为 /rpms, 无需单独挂载。
[ "$SERIES" = "24.03" ] && [ -d "$CLANG_RT" ] && MOUNTS+=(-v "$CLANG_RT:$CLANG_RT:ro")

timeout 1200 podman run --rm --user=0 \
    "${MOUNTS[@]}" \
    -e OPENEULER_MACRO="$OEU_MACRO" \
    -e CPPSTD="$CPPSTD" \
    -e EXTRA_MESON_ARGS="$EXTRA_MESON_STR" \
    -e MESON_BIN="$MESON_BIN" \
    "$IMG" \
    bash "$INNER"

echo "==> 产物在: $OUTDIR_HOST"
ls -la "$OUTDIR_HOST"
