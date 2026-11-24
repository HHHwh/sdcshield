#!/bin/bash
#=====================================================
# cpu_toggle.sh — CPU 动态调频脚本（通用）
#
# 功能：每隔 INTERVAL 秒切换一次 CPU 频率，每次切换到频率列表中的下一档
# 原理：通过 cpufreq 的 sysfs 接口把各核心切到 userspace governor，
#       然后按可用频率列表循环写 scaling_setspeed（userspace 不可用时，
#       如 intel_pstate，回退为循环写 scaling_max_freq 限频）；
#       退出时自动恢复原始 governor 与频率上下限
#
# 使用说明：
#   sudo bash cpu_toggle.sh          # 默认每 15 秒切换一档
#   sudo bash cpu_toggle.sh 5        # 每 5 秒切换一档
#   sudo bash cpu_toggle.sh help     # 显示本帮助
#
# 退出：Ctrl+C（或 SIGTERM），脚本会自动恢复原始 governor/频率上限
# 注意：需要 root；无 cpufreq（虚拟机/容器常见）时会如实说明并退出。
#=====================================================

CPU_SYSFS="${CPU_SYSFS:-/sys/devices/system/cpu}"

usage() {
    cat <<'EOF'
用法: sudo bash cpu_toggle.sh [间隔秒数]
      bash cpu_toggle.sh help

说明:
  每隔「间隔秒数」（默认 15）秒把 CPU 频率切换到频率列表的下一档，
  列表取自 scaling_available_frequencies（若存在），否则取
  cpuinfo_min_freq 与 cpuinfo_max_freq 两档，循环往复。

示例:
  sudo bash cpu_toggle.sh        # 每 15 秒切换一次
  sudo bash cpu_toggle.sh 5      # 每 5 秒切换一次
  bash cpu_toggle.sh help        # 显示本帮助

注意:
  - 需要 root 权限（sudo）。
  - 依赖 cpufreq sysfs；无 cpufreq（虚拟机/容器）时直接说明并退出。
  - userspace governor 不可用（如 intel_pstate）时自动回退为
    循环限制 scaling_max_freq 的等效调频方式。
  - Ctrl+C 退出时自动恢复运行前的 governor 与频率上限。
EOF
}

write_sysfs() {
    { echo "$2" > "$1"; } 2>/dev/null
}

case "${1:-}" in
    help|-h|--help)
        usage
        exit 0
        ;;
esac

INTERVAL=${1:-15}          # 调频间隔，默认 15 秒
if ! [[ $INTERVAL =~ ^[0-9]+$ ]] || [ "$INTERVAL" -le 0 ]; then
    echo "错误：间隔秒数应为正整数，收到 '$INTERVAL'。" >&2
    usage >&2
    exit 1
fi

#---------- 前置检查 ----------
if [ "$EUID" -ne 0 ]; then
    echo "错误：需要 root 权限运行，请使用 sudo 或 root 用户。"
    exit 1
fi

if [ ! -d "$CPU_SYSFS/cpu0/cpufreq" ]; then
    echo "错误：系统不支持 cpufreq（可能运行在虚拟机或容器中，CPU 频率由宿主机管理）。"
    exit 1
fi

#---------- 获取可用频率列表（单位 kHz）----------
MIN=$(cat "$CPU_SYSFS/cpu0/cpufreq/cpuinfo_min_freq")
MAX=$(cat "$CPU_SYSFS/cpu0/cpufreq/cpuinfo_max_freq")
FREQ_LIST=()
if [ -r "$CPU_SYSFS/cpu0/cpufreq/scaling_available_frequencies" ]; then
    while read -r f; do
        [[ $f =~ ^[0-9]+$ ]] && FREQ_LIST+=("$f")
    done < <(tr ' ' '\n' < "$CPU_SYSFS/cpu0/cpufreq/scaling_available_frequencies" |
             sort -n -u)
fi
if [ ${#FREQ_LIST[@]} -eq 0 ]; then
    FREQ_LIST=("$MIN" "$MAX")
fi

echo "可用频率档位（MHz）："
for f in "${FREQ_LIST[@]}"; do
    printf "  %d\n" $((f / 1000))
done

#---------- 保存各核心原始 governor/频率上限，退出时恢复 ----------
declare -A ORIG_GOV ORIG_MIN ORIG_MAX
for d in "$CPU_SYSFS"/cpu[0-9]*/cpufreq; do
    [ -d "$d" ] || continue
    cpu=$(basename "$(dirname "$d")")
    ORIG_GOV[$cpu]=$(cat "$d/scaling_governor" 2>/dev/null)
    ORIG_MIN[$cpu]=$(cat "$d/scaling_min_freq" 2>/dev/null)
    ORIG_MAX[$cpu]=$(cat "$d/scaling_max_freq" 2>/dev/null)
done

cleanup() {
    echo
    echo "正在恢复原始 governor 与频率上下限..."
    for d in "$CPU_SYSFS"/cpu[0-9]*/cpufreq; do
        [ -d "$d" ] || continue
        cpu=$(basename "$(dirname "$d")")
        # 先恢复 min 再恢复 max，避免瞬间出现 min > max
        write_sysfs "$d/scaling_min_freq" "${ORIG_MIN[$cpu]}"
        write_sysfs "$d/scaling_max_freq" "${ORIG_MAX[$cpu]}"
        write_sysfs "$d/scaling_governor" "${ORIG_GOV[$cpu]}"
    done
    echo "已恢复，退出。"
    exit 0
}
trap cleanup INT TERM

#---------- 设置 userspace governor（允许手动指定频率）----------
GOV_OK=0
GOV_TOTAL=0
for d in "$CPU_SYSFS"/cpu[0-9]*/cpufreq; do
    [ -d "$d" ] || continue
    GOV_TOTAL=$((GOV_TOTAL + 1))
    if write_sysfs "$d/scaling_governor" userspace; then
        GOV_OK=$((GOV_OK + 1))
    fi
done

if [ "$GOV_OK" -eq 0 ]; then
    # userspace 不可用时（如 intel_pstate），回退到
    # “限制 scaling_max_freq”的方式实现等效调频
    echo "提示：userspace governor 不可用，改用 scaling_max_freq 限频方式。"
    MODE="cap"
else
    if [ "$GOV_OK" -lt "$GOV_TOTAL" ]; then
        echo "警告：仅 $GOV_OK/$GOV_TOTAL 个 cpufreq 域切到 userspace，其余域写入会被内核忽略。"
    fi
    MODE="set"
fi

#---------- 主循环：每隔 INTERVAL 秒切换到下一档频率 ----------
echo "开始动态调频，每 ${INTERVAL} 秒切换一次，按 Ctrl+C 停止。"
echo "-----------------------------------------------------"

idx=0
while true; do
    freq=${FREQ_LIST[$((idx % ${#FREQ_LIST[@]}))]}
    for d in "$CPU_SYSFS"/cpu[0-9]*/cpufreq; do
        [ -d "$d" ] || continue
        if [ "$MODE" = "set" ]; then
            write_sysfs "$d/scaling_setspeed" "$freq"
        else
            # scaling_max_freq 不能低于该核当前 scaling_min_freq，否则内核拒绝写入
            cap=$freq
            curmin=$(cat "$d/scaling_min_freq" 2>/dev/null)
            if [[ $curmin =~ ^[0-9]+$ ]] && [ "$cap" -lt "$curmin" ]; then
                cap=$curmin
            fi
            write_sysfs "$d/scaling_max_freq" "$cap"
        fi
    done

    cur=$(cat "$CPU_SYSFS/cpu0/cpufreq/scaling_cur_freq" 2>/dev/null)
    if [[ $cur =~ ^[0-9]+$ ]]; then
        cur_mhz="$((cur / 1000)) MHz"
    else
        cur_mhz="读取失败"
    fi
    echo "$(date '+%Y-%m-%d %H:%M:%S')  目标频率: $((freq / 1000)) MHz   当前实测: $cur_mhz"

    idx=$((idx + 1))
    sleep "$INTERVAL"
done
