#!/bin/bash
# build.sh — 在依赖已就绪的 openEuler 24.03 SPx aarch64 上构建 SDCShield
# 并做最小功能验证。
#
# 用法:
#   ./build.sh [源码根目录] [构建目录]
#
# **版本管控**: 检测并打印本机 openEuler 版本 (SPx), 与 download-deps.sh /
# install-deps.sh 的版本基线一致, 确保构建环境也被版本管控覆盖。
set -euo pipefail

# 加载共享的版本检测逻辑
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=_common.sh
source "$SCRIPT_DIR/_common.sh"

require_openeuler
require_min_os
echo "==> 构建 openEuler 版本: $(detect_os_version_full)"

SRC="${1:-$(git rev-parse --show-toplevel 2>/dev/null || echo .)}"
BUILD="${2:-$SRC/builddir}"

cd "$SRC"

echo "==> 源码目录: $SRC"
echo "==> 构建目录: $BUILD"

# aarch64 路径: eigen5 仓库自带, 无需系统 eigen3, 也无需 PKG_CONFIG_PATH.
# 保留 PKG_CONFIG_PATH 仅为兼容 x86 路径(若在该分支上构建). 无害.
echo "==> meson setup..."
if [ ! -d "$BUILD" ]; then
    PKG_CONFIG_PATH="$SRC/third-party/eigen5" meson setup "$BUILD" --buildtype=release
else
    PKG_CONFIG_PATH="$SRC/third-party/eigen5" meson setup --reconfigure "$BUILD" --buildtype=release
fi

echo "==> ninja..."
# 统一走受限并行度入口（默认 -j = nproc/2, 共享服务器保护; 显式 -j/SDC_BUILD_JOBS 可覆盖）
"$SCRIPT_DIR/../build.sh" -C "$BUILD"

BIN="$BUILD/sdcshield"
echo "==> 产物: $(ls -la "$BIN")"

echo "==> 功能验证: 列出测试数"
N=$("$BIN" --list-tests 2>/dev/null | wc -l)
echo "    测试数: $N"
[ "$N" -gt 100 ] || { echo "错误: 测试数异常偏少 ($N), 检查 isal/zlib/zstd 是否装上" >&2; exit 1; }

echo "==> 功能验证: zstd19 单线程"
"$BIN" -e zstd19 -t 2000 -n 1 2>&1 | grep -iE "exit|result" | head -3

echo "==> 完成。二进制: $BIN"
