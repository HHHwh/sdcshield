#!/bin/bash
# build.sh — SDCShield 规范构建入口（并行度上限保护）
#
# 为什么存在: 上游 ninja 默认并行度 = 逻辑 CPU 数 + 2（2011-02-01 ninja commit
# 0d0f186 的 GuessParallelism(): 0/1 核→2、2 核→3、≥3 核→N+2, 沿用至今, 源码无
# 注释解释动机）。本仓库主要在共享服务器（鲲鹏 920, 96 逻辑核）上构建与压测,
# 98 路并行 g++（Eigen 重模板, 单进程峰值数百 MB）内存/CPU 双峰值会打满机器,
# 拖垮同机在跑的压测。故项目内构建统一走本入口, 默认并行度 = nproc × 50%。
#
# 用法（在仓库根目录执行; builddir 需先 meson setup, 见 README）:
#   ./scripts/build.sh                 # 等价 ninja -C builddir -j $(( $(nproc)/2 ))
#   ./scripts/build.sh unittests       # 追加 ninja 目标/参数
#   ./scripts/build.sh -C other-dir    # 自定义构建目录（显式 -C 时不重复注入）
#   ./scripts/build.sh -j 8            # 显式 -j 优先
#
# 并行度优先级: 显式 -j 参数 > SDC_BUILD_JOBS 环境变量 > nproc/2（下限 1;
# -j 0 在 ninja 里 = 无限并行, 故必须钳位）。make jobserver 场景（MAKEFLAGS 含
# jobserver, 如 make -jN 内嵌调用）让路给 jobserver 池, 不注入 -j。
# CI（GitHub Actions 隔离 runner）与历史归档文档不经本入口, 不受影响。

set -euo pipefail

inject_C=1
inject_j=1
for a in "$@"; do
    case "$a" in
    -C|-C*) inject_C=0 ;;
    -j*|--jobs=*) inject_j=0 ;;
    esac
done

# ninja >= 1.12 检测到 MAKEFLAGS 带 jobserver 时自动用其池; 显式 -j 会禁用
case "${MAKEFLAGS:-}" in
*jobserver*) inject_j=0 ;;
esac

if [ "$inject_j" = 1 ]; then
    j="${SDC_BUILD_JOBS:-}"
    if [ -z "$j" ]; then
        j=$(( $(nproc 2>/dev/null || echo 4) / 2 ))
    fi
    case "$j" in
    ''|*[!0-9]*) echo "scripts/build.sh: 无效并行度 '$j'（SDC_BUILD_JOBS 需为正整数）" >&2; exit 2 ;;
    esac
    [ "$j" -ge 1 ] || j=1
    set -- -j "$j" "$@"
fi

if [ "$inject_C" = 1 ]; then
    set -- -C builddir "$@"
fi

exec ninja "$@"
