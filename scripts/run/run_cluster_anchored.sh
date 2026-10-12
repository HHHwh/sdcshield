#!/bin/bash
# ============================================================
# 簇锚定 SDC campaign 编排脚本(cluster-anchored campaign)
#
# 用途:对指定簇(目标簇)运行 sdcshield 测试,同时在另一簇
# (健康对照簇)运行相同测试与时长,输出双簇对照结果。这是针对
# "簇锚定型瞬态故障"(如 cn23154 NUMA3 VA[55:48] 通路故障,
# ~19min 节律)的编排:单轮 5s 默认测试对这类故障完全盲,
# 需要 簇内满饱和 × 长驻留 × 健康簇对照 三者齐备。
#
# 用法:
#   ./run_cluster_anchored.sh -c <簇号>            # sysfs cluster_cpus_list 解析
#   ./run_cluster_anchored.sh -C <cpu列表>          # 直接给 cpuset(如 114-151)
# 可选:
#   -t <秒>   每轮时长(默认 1800 = 30min,匹配 ~19min 故障节律)
#   -r <轮数> 轮数(默认 4)
#   -e <测试> 测试列表(默认 sve512 全家族 + nt_reload)
#   -q        快速轮换模式(60s × N 轮测试轮换 — Ripple 7% 类
#             "只有频繁切换才触发"的缺陷需要此节奏)
#
# 依赖:builddir/sdcshield;sysfs topology(cluster_cpus_list,
# 缺失时回退 die_cpus_list,再回退均匀切分)。
#
# 崩溃判读提示(自动打印):si_addr 低 48 位完好 + VA[55:48] 非零
# = 已知 NUMA3 签名形状(详见仓库 memory/诊断记录)。
# ============================================================

SDCSHIELD="./builddir/sdcshield"
TIMEOUT=1800
ROUNDS=4
TESTS="sve512_nt_reload_arm,sve512_f64_chain_arm,sve512_f32_chain_arm,sve512_gather_scatter_arm,sve512_stencil_axis_arm,power_virus_dit"
QUICK=0
CLUSTER_ID=""
CPUSET=""
LOG_DIR="./cluster_anchored_logs"

while getopts "c:C:t:r:e:q" opt; do
    case $opt in
        c) CLUSTER_ID="$OPTARG" ;;
        C) CPUSET="$OPTARG" ;;
        t) TIMEOUT="$OPTARG" ;;
        r) ROUNDS="$OPTARG" ;;
        e) TESTS="$OPTARG" ;;
        q) QUICK=1; TIMEOUT=60; ROUNDS=20 ;;
        *) echo "用法: $0 [-c 簇号|-C cpu列表] [-t 秒] [-r 轮数] [-e 测试列表] [-q]"; exit 1 ;;
    esac
done

# ---------- 解析目标簇 ----------
expand_cpuset() {
    # "0-3,8" -> 逐 CPU 空格列表
    local spec="$1" out=""
    IFS=',' read -ra parts <<< "$spec"
    for p in "${parts[@]}"; do
        if [[ "$p" == *-* ]]; then
            lo="${p%-*}"; hi="${p#*-}"
            for ((i=lo; i<=hi; i++)); do out+="$i "; done
        else
            out+="$p "
        fi
    done
    echo "$out"
}

# --cpuset only accepts a comma-separated plain-number list (verified:
# "0-3" -> rc=64 EX_USAGE, "0,1,2,3" -> rc=0).
to_comma_list() {
    local spec="$1" out="" first=1
    for c in $(expand_cpuset "$spec"); do
        if [ $first -eq 1 ]; then out="$c"; first=0; else out="$out,$c"; fi
    done
    echo "$out"
}

if [ -z "$CPUSET" ]; then
    if [ -n "$CLUSTER_ID" ]; then
        # 任一 CPU 的 cluster_cpus_list 给出簇边界;轮询所有 CPU 找簇号 CLUSTER_ID 所在的
        for c in /sys/devices/system/cpu/cpu[0-9]*; do
            cid=$(cat "$c/topology/cluster_cpus_list" 2>/dev/null)
            if [ -n "$cid" ]; then
                cpus=$(expand_cpuset "$cid")
                # 该簇的"编号"取其最小 CPU 号的序位 —— 简化:用簇首 CPU 列表去重
                echo "$cid" >> /tmp/.clusters_seen.$$
            fi
        done
        mapfile -t uniq_clusters < <(sort -u /tmp/.clusters_seen.$$ 2>/dev/null)
        rm -f /tmp/.clusters_seen.$$
        if [ "${#uniq_clusters[@]}" -ge $((CLUSTER_ID + 1)) ]; then
            CPUSET="${uniq_clusters[$CLUSTER_ID]}"
            echo "簇 #$CLUSTER_ID => cpus $CPUSET"
        else
            echo "错误: 找到 ${#uniq_clusters[@]} 个簇,簇号 $CLUSTER_ID 越界" >&2
            exit 1
        fi
    else
        echo "错误: 需要 -c <簇号> 或 -C <cpu列表>" >&2
        exit 1
    fi
fi

# ---------- 选健康对照簇(排除目标簇) ----------
# 从 /sys/devices/system/cpu/online 取实际存在的 CPU(机器可能有
# offline CPU,编号不连续 — nproc 数量不可当最大编号用,实测踩坑:
# nproc=126 但 CPU 122+ 不存在,--cpuset 直接 EX_USAGE)。
TARGET_CPUS=$(expand_cpuset "$CPUSET")
ONLINE_CPUS=$(expand_cpuset "$(cat /sys/devices/system/cpu/online)")
# 过滤掉目标簇,取剩余里与目标簇规模最接近的前 N 个(N=目标簇大小)
TARGET_COUNT=$(echo "$TARGET_CPUS" | wc -w)
CONTROL_LIST=""
for c in $ONLINE_CPUS; do
    skip=0
    for t in $TARGET_CPUS; do [ "$c" = "$t" ] && skip=1 && break; done
    [ $skip -eq 0 ] && CONTROL_LIST+="$c "
done
CONTROL_COUNT=$(echo $CONTROL_LIST | wc -w)
if [ "$CONTROL_COUNT" -lt "$TARGET_COUNT" ]; then
    echo "警告: 可用对照 CPU 不足(需 $TARGET_COUNT 有 $CONTROL_COUNT),退化为无对照" >&2
    CONTROL_CPUSET=""
else
    CONTROL_CPUSET=$(echo $CONTROL_LIST | tr ' ' '\n' | head -n "$TARGET_COUNT" | paste -sd, -)
fi

mkdir -p "$LOG_DIR"
STAMP=$(date +%Y%m%d-%H%M%S)
echo "========== 簇锚定 campaign =========="
echo "目标簇 cpus: $CPUSET"
echo "对照簇 cpus: ${CONTROL_CPUSET:-无(退化为单簇)}"
echo "每轮 ${TIMEOUT}s × ${ROUNDS} 轮"
echo "测试: $TESTS"
echo "日志: $LOG_DIR/"
echo "开始: $(date)"
echo ""

PASS_T=0; FAIL_T=0; PASS_C=0; FAIL_C=0
for ((r=1; r<=ROUNDS; r++)); do
    echo "---- 第 $r/$ROUNDS 轮 ($(date +%H:%M:%S)) ----"
    TLOG="$LOG_DIR/round${r}_target.yaml"
    "$SDCSHIELD" --cpuset="$(to_comma_list "$CPUSET")" -t "$TIMEOUT" -e "$TESTS" -o "$TLOG" -q >/dev/null 2>&1
    TRC=$?
    if [ $TRC -eq 0 ]; then PASS_T=$((PASS_T+1)); else FAIL_T=$((FAIL_T+1)); fi
    echo "  目标簇: exit=$TRC (pass轮 $PASS_T / fail轮 $FAIL_T)"

    if [ -n "$CONTROL_CPUSET" ]; then
        CLOG="$LOG_DIR/round${r}_control.yaml"
        "$SDCSHIELD" --cpuset="$(to_comma_list "$CONTROL_CPUSET")" -t "$TIMEOUT" -e "$TESTS" -o "$CLOG" -q >/dev/null 2>&1
        CRC=$?
        if [ $CRC -eq 0 ]; then PASS_C=$((PASS_C+1)); else FAIL_C=$((FAIL_C+1)); fi
        echo "  对照簇: exit=$CRC (pass轮 $PASS_C / fail轮 $FAIL_C)"
    fi

    # 崩溃判读:目标簇 yaml 里出现 crash/Received signal 即提示签名判据
    if grep -lq "Received signal\|result: crash" "$TLOG" 2>/dev/null; then
        echo "  ⚠ 目标簇本轮出现崩溃 —— 判读提示:"
        echo "    si_addr 低 48 位完好且 VA[55:48] 非零(如 0x00XX_0000_....)="
        echo "    已知 NUMA3 VA[55:48] 瞬态故障签名;低位损坏则按新证据分析。"
        grep -h "FAR\|Received signal\|si_addr" "$TLOG" | head -5 | sed 's/^/    /'
    fi
done

echo ""
echo "========== 汇总 =========="
echo "目标簇: $PASS_T pass / $FAIL_T fail (共 $ROUNDS 轮)"
if [ -n "$CONTROL_CPUSET" ]; then
    echo "对照簇: $PASS_C pass / $FAIL_C fail"
    if [ $FAIL_T -gt 0 ] && [ $FAIL_C -eq 0 ]; then
        echo ">>> 目标簇失败而对照簇全过 —— 簇锚定差异成立,保留全部日志。"
    fi
fi
echo "结束: $(date)"
