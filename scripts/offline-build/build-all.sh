#!/bin/bash
# build-all.sh — 多 openEuler 版本矩阵构建编排器(方案 2)。
#
# 把"改一行源码 → 全 15 SP 验证"压成一条命令。本 patch 叠加杠杆 2(源码哈希
# skip)+ 杠杆 4(--since 按 git diff 选 SP);杠杆 3(-P 并行)由后续 patch 启用。
#
# 杠杆 2:构建产 built/BUILD-HASH,内容 = 影响二进制输出的输入哈希。
#   稳态下改一行 sandstone.cpp,只有源码确实变的 SP 重建,其余复用 built/。
#   哈希输入:源码树(framework/ tests/ meson.build meson_options.txt) +
#   container-build.sh(改适配逻辑也触发重建;sed 已收敛到源码/meson option) +
#   cpp_std/macro + 镜像 input-hash(基座变也重建)。全,不漏。
#
# 杠杆 4:--since <ref> 用 git diff 选受影响 SP(与哈希 skip 叠加,双重过滤)。
#
# 用法:
#   build-all.sh [series] [sp] [options]
#     series: 24.03 | 22.03 | 20.03 | all  (默认 all)
#     sp    : LTS | SP1..SP4 | all          (默认 all)
#   options:
#     --smoke   快档:verify-built-pristine.sh smoke (默认)
#     --full    全量:verify-built-pristine.sh full (发布闸门)
#     --no-verify  只构建 + 打包,不验证
#     --jobs N  并行度(本 patch 串行,占位;后续 patch 启用)
#     --since <ref>  只重建 git diff <ref>..HEAD 受影响的 SP
#     --force   忽略哈希 skip,强制重建
#
# 流程(每个目标 SP):
#   1) build-images.sh <s> <sp>     镜像就绪(幂等 skip)
#   2) 哈希 skip 判定:输入哈希 == built/BUILD-HASH 且 --since 未命中 → 复用,跳 3-4
#   3) container-build.sh <s> <sp>  源码构建(杠杆1:镜像已烘焙 deps)
#   4) package-built-artifacts.sh   打进 built/ + 写 BUILD-HASH
#   5) verify-built-pristine.sh smoke|full  闸门
#
# SPDX-License-Identifier-Identifier: Apache-2.0
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"  # scripts/offline-build/ → 仓根

# ── SP 矩阵 ──
ALL_SERIES="24.03 22.03 20.03"
ALL_SP="LTS SP1 SP2 SP3 SP4"

# ── 参数 ──
SERIES_ARG="all"; SP_ARG="all"; MODE="smoke"; JOBS=1; NOVERIFY=0; SINCE=""; FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --smoke)    MODE="smoke"; shift ;;
        --full)     MODE="full";  shift ;;
        --all)      SERIES_ARG="all"; SP_ARG="all"; shift ;;   # 全 15 SP
        --no-verify) NOVERIFY=1;  shift ;;
        --jobs)     JOBS="$2"; shift 2 ;;
        --since)    SINCE="$2"; shift 2 ;;
        --force)    FORCE=1; shift ;;
        --help|-h) sed -n '2,/^$/p' "$0" | sed 's/^# //;s/^#//'; exit 0 ;;
        -*) echo "unknown: $1" >&2; exit 1 ;;
        *)  if [ "$SERIES_ARG" = "all" ] && [[ "$1" != all ]]; then SERIES_ARG="$1"
            elif [ "$SP_ARG" = "all" ] && [[ "$1" != all ]]; then SP_ARG="$1"
            fi; shift ;;
    esac
done

if [ "$SERIES_ARG" = "all" ]; then SERIES_LIST="$ALL_SERIES"; else SERIES_LIST="$SERIES_ARG"; fi
if [ "$SP_ARG" = "all" ]; then SP_LIST="$ALL_SP"; else SP_LIST="$SP_ARG"; fi

# ── 杠杆 4:--since 受影响文件集 ──
AFFECTED=""
if [ -n "$SINCE" ]; then
    AFFECTED=$(git -C "$SRC_ROOT" diff --name-only "$SINCE"..HEAD 2>/dev/null || true)
    echo "[--since $SINCE] 受影响文件 $(echo "$AFFECTED" | grep -c . 2>/dev/null || echo 0) 个"
fi

# ┠─ 杠杆 4:该 SP 是否被 --since 命中 ──
# 22.03/20.03 受 sed 适配影响(OPENEULER_MACRO 触发 .contains/udf/ACL sed),
# 故 compat/ 或 container-build.sh 动了 → 这两个系列全 SP 都算受影响。
hit_by_since() {
    local series="$1"
    [ -z "$SINCE" ] && return 0   # 无 --since → 全命中(再由哈希 skip 过滤)
    # 任何源码/脚本变动都影响所有 SP(源码共享);compat 适配只影响 22.03/20.03
    local any_src=$(echo "$AFFECTED" | grep -cE '^(framework/|tests/|meson\.build|meson_options\.txt|scripts/offline-build/container-build\.sh|scripts/offline-build/images/)' || echo 0)
    [ "$any_src" -gt 0 ] && return 0
    # 仅 22.03/20.03 专属变动?
    local compat=$(echo "$AFFECTED" | grep -cE '^framework/compat/' || echo 0)
    [ "$compat" -gt 0 ] && { [[ "$series" == 22.03 || "$series" == 20.03 ]] && return 0 || return 1; }
    return 1
}

# ── 杠杆 2:计算构建输入哈希 ──
# 输入:源码树 + container-build.sh(含 sed 适配)+ cpp_std/macro + 镜像 input-hash
compute_build_hash() {
    local series="$1" sp="$2"
    local macro=""
    case "$series" in
        24.03) macro=""; cpp_std="gnu++23" ;;
        22.03) macro="OPENEULER_22_03"; cpp_std="gnu++20" ;;
        20.03) macro="OPENEULER_20_03"; cpp_std="gnu++20" ;;
    esac
    # 源码树哈希(framework tests meson.build meson_options.txt 的 git ls-tree)
    local src_hash
    src_hash=$(git -C "$SRC_ROOT" ls-tree -r HEAD -- framework tests meson.build meson_options.txt 2>/dev/null | sha256sum | awk '{print $1}')
    # container-build.sh 哈希(含全部 sed 适配)
    local cb_hash
    cb_hash=$(sha256sum "$SCRIPT_DIR/container-build.sh" | awk '{print $1}')
    # 镜像 input-hash(从 image-manifest.tsv 取;基座变也重建)
    local tag="${series}-$(case "$sp" in LTS) echo LTS;; SP[1-4]) echo LTS-$sp;; esac)"
    local img_hash
    img_hash=$(grep -P "^${tag}\t" "$SCRIPT_DIR/images/image-manifest.tsv" 2>/dev/null | awk -F'\t' '{print $2}' || echo "no-image")
    printf '%s|%s|%s|%s|%s|%s\n' "$src_hash" "$cb_hash" "$cpp_std" "${macro:-none}" "$img_hash" "$tag" | sha256sum | awk '{print $1}'
}

echo "############ build-all: series=[$SERIES_LIST] sp=[$SP_LIST] mode=$MODE force=$FORCE since=${SINCE:-no} ############"

# ── 单 SP 流程 ──────────────────────────────────────────────────────
build_one() {
    local series="$1" sp="$2"
    local tag="openEuler-${series}$(case "$sp" in LTS) echo LTS;; SP[1-4]) echo LTS_$sp;; esac)"
    local spdir="$SRC_ROOT/third-party/rpms/openEuler-${series}/${tag}"
    local rpmdir="$spdir/rpms"
    local builtdir="$spdir/built"
    echo ""
    echo "==================== $tag ===================="

    # 杠杆 4:--since 过滤(先于镜像构建,避免无谓的 quay pull/image 检查)
    if ! hit_by_since "$series"; then
        echo "skip $tag (--since 未命中)"; echo "RESULT: SKIP $tag (--since)"; return 0
    fi

    # 1) 镜像就绪(幂等:已烘焙则 skip)
    if ! "$SCRIPT_DIR/images/build-images.sh" "$series" "$sp" 2>&1; then
        echo "RESULT: FAIL $tag (image build)" >&2; return 1
    fi

    # 杠杆 2:哈希 skip 判定
    if [ "$FORCE" != 1 ]; then
        local cur_hash existing=""
        cur_hash=$(compute_build_hash "$series" "$sp")
        [ -f "$builtdir/BUILD-HASH" ] && existing=$(cat "$builtdir/BUILD-HASH" 2>/dev/null || echo "")
        if [ -n "$existing" ] && [ "$cur_hash" = "$existing" ] && [ -x "$builtdir/sdcshield" ]; then
            echo "skip $tag (BUILD-HASH 未变,复用 built/)"
            # 复用时仍可跑验证(若要求)
            if [ "$NOVERIFY" != 1 ] && [ "$MODE" = "full" ]; then
                if ! "$SCRIPT_DIR/verify-built-pristine.sh" "$series" "$sp" "$MODE" 2>&1; then
                    echo "RESULT: FAIL $tag (verify full on reuse)" >&2; return 1
                fi
            fi
            echo "RESULT: PASS $tag (reused)"; return 0
        fi
        echo "[hash] cur=${cur_hash:0:12} existing=${existing:0:12}${existing:+...} → $([ "$cur_hash" = "$existing" ] && echo same || echo diff)"
    fi

    # 3) 源码构建(杠杆1:镜像已烘焙 deps)
    if ! "$SCRIPT_DIR/container-build.sh" "$series" "$sp" 2>&1; then
        echo "RESULT: FAIL $tag (source build)" >&2; return 1
    fi
    # 4) 打包(含写 BUILD-HASH + MANIFEST + VERSION,见 package-built-artifacts.sh)
    if ! "$SCRIPT_DIR/package-built-artifacts.sh" "$series" "$sp" 2>&1; then
        echo "RESULT: FAIL $tag (package)" >&2; return 1
    fi
    # 5) 闸门
    if [ "$NOVERIFY" != 1 ]; then
        if ! "$SCRIPT_DIR/verify-built-pristine.sh" "$series" "$sp" "$MODE" 2>&1; then
            echo "RESULT: FAIL $tag (verify $MODE)" >&2; return 1
        fi
    fi
    echo "RESULT: PASS $tag"
}

# ── 执行矩阵 ────────────────────────────────────────────────────────
# 杠杆 3:-P 并行。每个 SP 独立,用 xargs -P 并发。JOBS=1 退化为串行(等价 patch 前)。
# 每个 worker 把结果写一行到结果文件 + stdout;主进程从结果文件汇总
# (避免管道子 shell 变量隔离)。
PASS_N=0; FAIL_N=0; SKIP_N=0; FAIL_TAGS=""
RESULTS_DIR="$(mktemp -d)"
trap 'rm -rf "$RESULTS_DIR"' EXIT

# worker 脚本(被 BASH_ENV 注入):各函数定义 + main 调 build_one,结果写文件+stdout。
WORKER_SCRIPT="$(mktemp)"
{
    declare -f build_one compute_build_hash hit_by_since
    cat <<'WORKER_EOF'
_worker_main() {
    local series="$1" sp="$2" results_dir="$3"
    local out result tag
    tag="openEuler-${series}$(case "$sp" in LTS) echo LTS;; SP[1-4]) echo LTS_$sp;; esac)"
    out=$(build_one "$series" "$sp" 2>&1) || true
    result=$(echo "$out" | grep -oE 'RESULT: [A-Z]+ [^ ]+( [^ ]+)*' | tail -1)
    [ -z "$result" ] && result="RESULT: FAIL ${tag} (no-result)"
    # 失败时把完整构建日志透出：build_one 在 $() 里执行，容器/编译器的
    # 报错原本全部被吞掉，CI 只剩一行 RESULT: FAIL，无法定位。
    case "$result" in
        "RESULT: FAIL"*) echo "$out" ;;
    esac
    echo "$result"                                    # stdout(实时可见)
    echo "$result" > "${results_dir}/${tag}.result"  # 文件(汇总用)
}
WORKER_EOF
} > "$WORKER_SCRIPT"
export BASH_ENV="$WORKER_SCRIPT"
export SCRIPT_DIR SRC_ROOT MODE NOVERIFY FORCE SINCE AFFECTED SERIES_LIST SP_LIST RESULTS_DIR

# 展开目标列表为 "series sp" 对,用 xargs -P 并发
for series in $SERIES_LIST; do
    for sp in $SP_LIST; do
        echo "${series} ${sp}"
    done
done | xargs -P "$JOBS" -L1 bash -c '
    _worker_main "$1" "$2" "$RESULTS_DIR" || echo "RESULT: FAIL openEuler-${1} (worker-exit)"
' _ 2>&1 | grep -E "RESULT:|====|error:|Error |ERROR|FAILED|fatal|ninja: build stopped|Traceback|Permission denied" || true

unset BASH_ENV; rm -f "$WORKER_SCRIPT"

# 从结果文件汇总(不依赖管道子 shell)
for rf in "$RESULTS_DIR"/*.result; do
    [ -f "$rf" ] || continue
    line=$(cat "$rf")
    case "$(echo "$line" | grep -oE "RESULT: [A-Z]+ ")" in
        "RESULT: PASS ") PASS_N=$((PASS_N+1)) ;;
        "RESULT: SKIP ") SKIP_N=$((SKIP_N+1)) ;;
        *) FAIL_N=$((FAIL_N+1)); FAIL_TAGS="$FAIL_TAGS $(echo "$line" | awk '{print $3}')" ;;
    esac
done

echo ""
echo "############ 汇总 ############"
echo "PASS: $PASS_N   SKIP: $SKIP_N   FAIL: $FAIL_N"
[ -n "$FAIL_TAGS" ] && echo "失败:$FAIL_TAGS"
[ "$FAIL_N" = 0 ]

