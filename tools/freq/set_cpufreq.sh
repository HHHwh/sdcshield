#!/bin/bash
# set_cpufreq.sh — 固定 CPU 频率（SDC 压测时消除 DVFS 频率漂移噪声）
#
# 为什么存在: SDC 检出依赖逐位比对, CPU 频率/调频策略在压测中变化会引入
# 计时与热状态差异; 把 scaling_governor 钉到 userspace 并显式设频, 可让
# 同一轮压测运行在固定频率上。注意 cpufreq 因机而异（见 CLAUDE.md
# "Known platform quirks"）: 有的板有 cppc_cpufreq 且支持 userspace
# governor, 有的板根本没有 cpufreq —— 后者本脚本会如实说明并退出。
#
# 使用说明（需要 root, 否则写入会失败并提示）:
#   修改CPU当前频率:
#     bash set_cpufreq.sh cur 2600000
#   修改CPU的最小频率:
#     bash set_cpufreq.sh min 1000000
#   查看帮助:
#     bash set_cpufreq.sh help
#
# 参数单位均为 kHz（与 sysfs 一致）; 省略频率时 cur 默认 2600000 (2.6GHz),
# min 默认 200000 (200MHz)。取值范围被硬件钳位, 越界值会被内核收窄到
# [cpuinfo_min_freq, cpuinfo_max_freq]。

CPU_SYSFS="/sys/devices/system/cpu"

usage() {
    cat <<'EOF'
用法: bash set_cpufreq.sh <命令> [频率(kHz)]

命令:
  cur  [freq]   把 CPU 当前频率固定为 freq（切到 userspace governor 后
                写 scaling_setspeed）, 默认 2600000 = 2.6GHz
  min  [freq]   把 CPU 最小频率下限设为 freq（写 scaling_min_freq）,
                默认 200000 = 200MHz
  help          显示本帮助

示例:
  bash set_cpufreq.sh cur 2600000   # 修改CPU当前频率为 2.6GHz
  bash set_cpufreq.sh min 1000000   # 修改CPU的最小频率为 1.0GHz

注意:
  - 需要 root 权限, 否则所有写入会失败（脚本会提示实际生效的 CPU 数）。
  - 频率单位 kHz, 越界值由内核钳位到硬件支持范围。
  - 本机无 cpufreq（无 /sys/devices/system/cpu/cpuN/cpufreq）时直接说明并退出。
  - 恢复动态调频: 设回原 governor, 例如
      for d in /sys/devices/system/cpu/cpu[0-9]*/cpufreq; do
          echo schedutil > "$d/scaling_governor";   # 或 ondemand/performance
      done
EOF
}

check_cpufreq() {
    for d in "$CPU_SYSFS"/cpu[0-9]*/cpufreq; do
        [ -d "$d" ] && return 0
    done
    echo "本机无 cpufreq sysfs（$CPU_SYSFS/cpuN/cpufreq 不存在）, 无法设置频率。"
    echo "（缺少 cpufreq 的板子属正常平台差异, 不是缺陷。）"
    exit 0
}

check_freq() {
    local freq=$1
    if ! [[ $freq =~ ^[0-9]+$ ]] || [ "$freq" -le 0 ]; then
        echo "无效频率: '$freq'（应为正整数, 单位 kHz）" >&2
        usage >&2
        exit 1
    fi
}

report() {
    local ok=$1 total=$2 what=$3
    if [ "$ok" -eq 0 ]; then
        echo "警告: $what 在 $total 个 cpufreq 域上全部写入失败——请用 root/sudo 运行。" >&2
        return 1
    elif [ "$ok" -lt "$total" ]; then
        echo "警告: $what 仅 $ok/$total 个 cpufreq 域写入成功。" >&2
    fi
    return 0
}

# 组重定向才能在文件本身不可写时也吞掉 shell 的重定向报错（逐条 2>/dev/null 不行）
write_sysfs() {
    { echo "$2" > "$1"; } 2>/dev/null
}

function set_min_freq() {
    freq=${1:-200000}  # 默认设置CPU最小频率为200MHz
    check_freq "$freq"
    local ok=0 total=0 d
    for d in "$CPU_SYSFS"/cpu[0-9]*/cpufreq; do
        [ -d "$d" ] || continue
        total=$((total + 1))
        if write_sysfs "$d/scaling_min_freq" "$freq"; then
            ok=$((ok + 1))
        fi
    done
    local changed=1
    report "$ok" "$total" "最小频率写入" || changed=0

    min_freq=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq)
    if [ "$changed" -eq 1 ]; then
        echo "当前CPU最小频率已修改为: $min_freq"
    else
        echo "当前CPU最小频率未修改（当前仍为: $min_freq）"
    fi
}

function set_cur_freq() {
    freq=${1:-2600000}  # 默认设置CPU频率为2.6GHz
    check_freq "$freq"
    local ok=0 total=0 d
    for d in "$CPU_SYSFS"/cpu[0-9]*/cpufreq; do
        [ -d "$d" ] || continue
        total=$((total + 1))
        if write_sysfs "$d/scaling_governor" userspace &&
           write_sysfs "$d/scaling_setspeed" "$freq"; then
            ok=$((ok + 1))
        fi
    done
    local changed=1
    report "$ok" "$total" "当前频率写入" || changed=0

    cur_freq=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq)
    if [ "$changed" -eq 1 ]; then
        echo "当前CPU频率已修改为: $cur_freq"
    else
        echo "当前CPU频率未修改（当前仍为: $cur_freq）"
    fi
}

action=$1
freq=$2
case "$action" in
    min)
        check_cpufreq
        set_min_freq "$freq"
        ;;
    cur)
        check_cpufreq
        set_cur_freq "$freq"
        ;;
    help|-h|--help)
        usage
        ;;
    "")
        echo "无效选项,请输入传参 min/cur！" >&2
        usage >&2
        exit 1
        ;;
    *)
        echo "无效选项,请输入传参 min/cur！" >&2
        usage >&2
        exit 1
        ;;
esac
