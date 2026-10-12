#!/bin/bash
# =============================================================================
# cpu122-online-offline.sh — CPU122（坏核）上线/下线/封顶控制 v2
#
# 用法:
#   sudo ./cpu122-online-offline.sh {上线|on|1|下线|off|0}
#   sudo ./cpu122-online-offline.sh {封顶|cap} <kHz>     # 0=解锁（高危）
#
# v2 安全模型（2026-10-10, 上线即封顶体系）:
#   前提    内核模块 cpu122_capfreq 已加载（kprobe cppc_set_perf 钳位 +
#           freq_qos MAX 挂 policy 跨热插拔存活）——满频 slam 在写硬件前即被
#           钳位, 封顶不再依赖用户态"看到 online=1 再降频"（那条链路架构性
#           必输: uevent 在 write() 返回前才广播, 实测上线后 ~10ms 即死）。
#   门0     模块未加载/cap 异常 → 拒绝上线/封顶（绝不裸奔上线）;
#           cap=0 拒绝上线; cap>2.9G 拒绝; cap>2.0G 高危警告放行。
#   门1     present≠0-127 / offline≠122 / cpu121-123 拓扑不符同簇序
#           → 编号漂移嫌疑, 拒绝上线（跨启动以 MPIDR 为准, 见配置总账 §6.4）。
#   后验1   journalctl 核对 CPU122 MPIDR == 0x0900060200, 不符自动下线。
#   后验2   scaling_max_freq/scaling_cur_freq ≤ cap, 越界（qos 疑失效）自动下线。
#   封顶    写模块 cap_khz, qos 自动传导, 在线切档免上下线（1.45G↔2.0G）。
#
# 每次执行前自动完成三项准备工作（均幂等, 已就绪则跳过）:
#   准备1  检索并注释 udev 的 CPU 自动上线规则（防 maxcpus 失效;
#          /usr/lib 出厂文件的注释会被 udev 包升级还原, 升级后重跑核查）
#   准备2  GRUB 确保 maxcpus=122 panic=30（含 grubby 生效层同步,
#          清除历史残留 nr_cpus= ——09-24 曾发生生效层残留 nr_cpus=12）
#   准备3  上线除 CPU122 外的全部核（0..121, 123..最大核号）
#
# 设计文档: docs/superpowers/specs/2026-10-10-cpu122-online-atomic-freqcap-design.md
# 模块正本: docs/cases/sdc1-01-04-core122/cpu122-capfreq/（构建/SOP/验证手册）
# =============================================================================

set -u

BAD_CORE=122
# 测试钩子: TEST_MODE 下 CPU_SYSFS/MOD_SYSFS 可指向假树（仅测试使用，板上勿设）
CPU_SYSFS=${CPU_SYSFS:-/sys/devices/system/cpu}
MOD_SYSFS=${MOD_SYSFS:-/sys/module/cpu122_capfreq/parameters}
GRUB_DEFAULT=/etc/default/grub
CAP_HARD_MAX=2900000     # 板上物理最高频（kHz）
CAP_WARN_MAX=2000000     # 超出即高危警告（2.0G 实验档以上）
# 激活状态 CPU 自动上线规则的匹配模式（以 # 开头的注释行天然不匹配，天然幂等；
# 覆盖 SUBSYSTEM 在行首 与 ACTION=="add" 在行首两种写法）
UDEV_CPU_ONLINE_RE='^[[:space:]]*(ACTION=="add",[[:space:]]*)?SUBSYSTEM=="cpu".*ATTR\{online\}="1"'

log() { echo "[$(date '+%F %T')] $*"; }
die() { log "错误: $*"; exit 1; }

usage() {
    cat <<EOF
用法: $0 {上线|on|1|下线|off|0}          坏核上下线
      $0 {封顶|cap} <kHz>               在线改频率上限（免上下线, 0=解锁）
  上线|on|1   -> 门0(模块就绪)+门1(编号预检) -> echo 1 -> 后验(MPIDR+频率)
  下线|off|0  -> echo 0 -> cpu122/online
  封顶|cap    -> 写 /sys/module/cpu122_capfreq/parameters/cap_khz
参数强制显式指定，没有默认值。
示例:
  sudo $0 上线            # 三门齐过后上线（封顶由模块在上线路径内完成）
  sudo $0 下线            # CPU122 下线
  sudo $0 封顶 2000000    # 切 2.0G 档（在线生效, 无需上下线）
  sudo $0 封顶 0          # 解锁（仅限满频 A/B, 高危）
EOF
}

# ---------- 参数解析（先于 root 检查，无参数/错误参数直接报用法） ----------
if [ $# -lt 1 ] || [ $# -gt 2 ]; then
    usage
    exit 1
fi
case "$1" in
    上线|on|1)  [ $# -eq 1 ] || { usage; exit 1; }
                ACT=1; ACT_TXT="上线" ;;
    下线|off|0) [ $# -eq 1 ] || { usage; exit 1; }
                ACT=0; ACT_TXT="下线" ;;
    封顶|cap)   ACT=2; ACT_TXT="封顶"
                [ $# -eq 2 ] || { usage; exit 1; }
                case "$2" in ''|*[!0-9]*) usage; exit 1 ;; esac
                CAP_ARG=$2 ;;
    -h|--help)  usage; exit 0 ;;
    *)          usage; exit 1 ;;
esac

# ---------- root 权限（sysfs / udev / grub 写操作均需要） ----------
if [ "${CPU122_CAPFREQ_TEST_MODE:-}" = "1" ]; then
    log "TEST MODE: 跳过 root 检查与准备1/2（仅 fake sysfs 测试用）"
elif [ "$(id -u)" -ne 0 ]; then
    log "需要 root 权限，通过 sudo 重新执行..."
    exec sudo bash "$0" "$@"
    die "sudo 不可用，请以 root 手动运行"
fi

[ -d "$CPU_SYSFS/cpu$BAD_CORE" ] \
    || die "CPU$BAD_CORE 不存在（未被内核 present），无法操作"

# ---------- 准备1: 检索并注释 udev 的 CPU 自动上线规则 ----------
# 不复制出厂规则做 /etc 整文件覆盖（udev 升级后快照会过时、遮蔽新版出厂规则），
# 而是每次运行动态检索全部规则目录，将激活的 CPU 自动上线规则原地注释。
find_active_cpu_online_rules() {
    # 输出所有激活的 CPU 自动上线规则（格式: 文件:行号:内容），无则输出空
    local dir f
    for dir in /etc/udev/rules.d /run/udev/rules.d /usr/lib/udev/rules.d; do
        [ -d "$dir" ] || continue
        for f in "$dir"/*.rules; do
            [ -f "$f" ] || continue
            grep -En "$UDEV_CPU_ONLINE_RE" "$f" 2>/dev/null || true
        done
    done
}

block_udev_autoonline() {
    local found n=0 f l
    local -a hit_files
    found=$(find_active_cpu_online_rules)
    if [ -n "$found" ]; then
        mapfile -t hit_files < <(printf '%s\n' "$found" | cut -d: -f1 | sort -u)
        for f in "${hit_files[@]}"; do
            # 命中行前置 # 原地注释（原行内容保留），并追加标注
            sed -i -E "s@${UDEV_CPU_ONLINE_RE}@#& # [cpu122-online-offline.sh] 坏核 CPU$BAD_CORE 禁止 udev 自动上线@" "$f" \
                || die "注释 $f 中的 CPU 自动上线规则失败（文件只读?）"
            n=$((n + 1))
        done
        while IFS= read -r l; do
            log "准备1: 已注释 -> $l"
        done <<< "$found"
        log "准备1: 共注释 $n 个规则文件，复核后 reload udevd"
        log "提示: /usr/lib 出厂文件中的注释会在 udev 包升级时被还原，升级后请重跑本脚本核查"
    else
        log "准备1: 检索 /etc /run /usr/lib 全部 udev 规则，未发现激活的 CPU 自动上线规则"
    fi
    # 复核: 不允许残留任何激活的 CPU 自动上线规则
    if [ -n "$(find_active_cpu_online_rules)" ]; then
        die "复核失败: 仍存在未注释的 CPU 自动上线规则"
    fi
    # 旧方案残留检测: /etc 同名整文件覆盖会遮蔽 udev 升级后的新出厂规则
    if [ -f /etc/udev/rules.d/40-openEuler.rules ]; then
        log "警告: 存在 /etc/udev/rules.d/40-openEuler.rules 整文件覆盖（旧方案残留），会遮蔽升级后的新出厂规则，建议确认后删除"
    fi
    udevadm control --reload 2>/dev/null || true
}

# ---------- 准备2: GRUB 确保 maxcpus=122 panic=30 ----------
ensure_grub_params() {
    [ -f "$GRUB_DEFAULT" ] || die "未找到 $GRUB_DEFAULT"
    local line val newval changed=0 _w
    line=$(grep -E '^GRUB_CMDLINE_LINUX=' "$GRUB_DEFAULT" | head -n1)
    [ -n "$line" ] || die "$GRUB_DEFAULT 中未找到 GRUB_CMDLINE_LINUX="
    val=$(printf '%s\n' "$line" | sed -E 's/^GRUB_CMDLINE_LINUX="(.*)"$/\1/')
    [ "$val" != "$line" ] || die "解析 GRUB_CMDLINE_LINUX 失败（非标准双引号格式）"

    newval=$val
    # 先清除旧 maxcpus=/panic= 值（如历史实验残留的 maxcpus=1）及 nr_cpus=
    # （nr_cpus 硬截断 possible，会破坏 128 核可见的设计，09-24 本机曾在生效层残留 nr_cpus=12），
    # 再统一追加目标值
    newval=$(printf '%s\n' "$newval" \
        | sed -E 's/(^| )maxcpus=[0-9]+//g; s/(^| )panic=[0-9]+//g; s/(^| )nr_cpus=[0-9]+//g')
    newval="${newval:+$newval }maxcpus=$BAD_CORE panic=30"
    read -ra _w <<< "$newval"; newval="${_w[*]}"    # 规整空白

    if [ "$newval" != "$val" ]; then
        sed -i -E "s@^GRUB_CMDLINE_LINUX=.*@GRUB_CMDLINE_LINUX=\"$newval\"@" "$GRUB_DEFAULT" \
            || die "写入 $GRUB_DEFAULT 失败"
        changed=1
        log "准备2: GRUB_CMDLINE_LINUX 已更新为 \"$newval\""
    else
        log "准备2: maxcpus=$BAD_CORE panic=30 已存在，无需修改"
    fi

    if [ "$changed" -eq 1 ]; then
        # 仅在修改过后重新生成 grub.cfg；UEFI 优先（鲲鹏服务器通常为 UEFI）
        local grub_cfg="" d
        for d in /boot/efi/EFI/openEuler /boot/efi/EFI/openeuler /boot/grub2; do
            [ -d "$d" ] && { grub_cfg="$d/grub.cfg"; break; }
        done
        if [ -n "$grub_cfg" ]; then
            grub2-mkconfig -o "$grub_cfg" || die "grub2-mkconfig -o $grub_cfg 失败"
            log "准备2: 已重新生成 $grub_cfg"
        else
            log "警告: 未找到 UEFI/Legacy 的 grub.cfg 目录，跳过重新生成（生效层随后由 grubby 同步）"
        fi
    else
        log "准备2: grub.cfg 未修改，不重新生成"
    fi

    # 生效层同步（BLS entries）: 确保 maxcpus/panic 真正写进启动项，避免死层
    if command -v grubby >/dev/null 2>&1; then
        local args_line n=0 ok=1
        while IFS= read -r args_line; do
            [ -n "$args_line" ] || continue
            n=$((n + 1))
            printf '%s' "$args_line" | grep -qE "(^| )maxcpus=$BAD_CORE( |$)" || ok=0
            printf '%s' "$args_line" | grep -qE '(^| )panic=30( |$)'          || ok=0
            printf '%s' "$args_line" | grep -qE '(^| )nr_cpus='               && ok=0
        done < <(grubby --info=ALL 2>/dev/null | awk -F'"' '/^args=/{print $2}')
        if [ "$n" -eq 0 ]; then
            log "警告: grubby 未返回任何启动项 args，跳过生效层检查"
        elif [ "$ok" -eq 1 ]; then
            log "准备2: 生效层（BLS entries）已含 maxcpus=$BAD_CORE panic=30 且无 nr_cpus，无需同步"
        else
            grubby --update-kernel=ALL --remove-args="nr_cpus" \
                   --args="maxcpus=$BAD_CORE panic=30" \
                || die "grubby 同步生效层失败"
            log "准备2: 已通过 grubby 同步生效层（maxcpus=$BAD_CORE panic=30，并移除 nr_cpus）"
        fi
    else
        log "警告: grubby 缺失，无法校验/同步 BLS 生效层，请人工确认"
    fi

    # 当前运行内核的一致性提醒
    grep -qE "(^| )maxcpus=$BAD_CORE( |$)" /proc/cmdline 2>/dev/null \
        || log "提示: 当前运行内核 cmdline 尚无 maxcpus=$BAD_CORE，需重启后生效"
    grep -q 'nr_cpus=' /proc/cmdline 2>/dev/null \
        && log "警告: 当前 cmdline 含 nr_cpus=（硬截断 possible），请核查引导层是否残留"
    return 0
}

# ---------- 准备3: 上线除 CPU122 外的全部核 ----------
online_all_except_bad() {
    local max i onfile cnt=0 failed=0
    max=$(ls -d "$CPU_SYSFS"/cpu[0-9]* 2>/dev/null | sed 's@.*/cpu@@' | sort -n | tail -n1)
    case "$max" in ''|*[!0-9]*) die "无法确定最大 CPU 核号" ;; esac
    for i in $(seq 0 "$max"); do
        [ "$i" -eq "$BAD_CORE" ] && continue
        onfile="$CPU_SYSFS/cpu$i/online"
        if [ -f "$onfile" ] && [ "$(cat "$onfile" 2>/dev/null)" = "0" ]; then
            if echo 1 > "$onfile" 2>/dev/null; then
                log "准备3: CPU$i 上线"
                cnt=$((cnt + 1))
            else
                log "警告: CPU$i 上线失败"
                failed=$((failed + 1))
            fi
        fi
    done
    if [ "$cnt" -eq 0 ] && [ "$failed" -eq 0 ]; then
        log "准备3: 除 CPU$BAD_CORE 外全部核均已在线，无需处理（最大核号 $max）"
    else
        log "准备3: 本次新上线 $cnt 个核，失败 $failed 个（最大核号 $max）"
    fi
    [ "$failed" -eq 0 ] || die "存在上线失败的核，中止后续动作"
}

# ============================== 主流程 ==============================
log "===== 准备工作 ====="
if [ "${CPU122_CAPFREQ_TEST_MODE:-}" != "1" ]; then
    block_udev_autoonline
    ensure_grub_params
else
    log "TEST MODE: 准备1/2 已跳过"
fi
online_all_except_bad

# ---------- 门0: 模块就绪检查（上线/封顶共用；成功时置全局 MOD_CAP） ----------
module_ready() {
    local p="$MOD_SYSFS/cap_khz" t
    if [ ! -d "${MOD_SYSFS%/*}" ]; then
        log "门0: 模块 cpu122_capfreq 未加载 — 拒绝（裸奔上线封顶无从谈起）"
        log "门0: 排查: lsmod | grep cpu122_capfreq; dmesg | grep cpu122-capfreq; 内核升级后需按模块 README 重编"
        return 1
    fi
    if [ ! -r "$p" ]; then
        log "门0: 参数文件 $p 不可读 — 拒绝"
        return 1
    fi
    MOD_CAP=$(cat "$p" 2>/dev/null)
    case "$MOD_CAP" in ''|*[!0-9]*)
        log "门0: cap_khz 非法值 '$MOD_CAP' — 拒绝"; return 1 ;;
    esac
    t=$(cat "$MOD_SYSFS/target_cpu" 2>/dev/null)
    if [ "$t" != "$BAD_CORE" ]; then
        log "门0: 模块 target_cpu='$t' ≠ $BAD_CORE（V1 演练态/参数异常, 钳位与 qos 不在坏核上）— 拒绝"
        return 1
    fi
    return 0
}

# ---------- 门0 附加上线边界: cap 必须 >0 且 ≤ 物理上限; >2.0G 高危警告 ----------
cap_sanity_for_online() {
    if [ "$MOD_CAP" -eq 0 ]; then
        log "门0: cap_khz=0（模块停用态）— 拒绝上线（如需满频 A/B, 先执行: $0 封顶 2900000 显式解锁）"
        return 1
    fi
    if [ "$(cat "$MOD_SYSFS/cap_perf" 2>/dev/null)" = "0" ]; then
        log "门0: 模块 cap_perf=0（kHz→perf 换算未就绪, kprobe 钳位未武装, 首次 slam 将直通）— 拒绝上线"
        log "门0: 排查: dmesg | grep 'cpu122-capfreq'（cppc_get_perf_caps 失败记录）"
        return 1
    fi
    if [ "$MOD_CAP" -gt "$CAP_HARD_MAX" ]; then
        log "门0: cap_khz=$MOD_CAP 超出物理最高频 $CAP_HARD_MAX — 拒绝"
        return 1
    fi
    [ "$MOD_CAP" -gt "$CAP_WARN_MAX" ] \
        && log "门0: 高危警告: cap_khz=$MOD_CAP 超出 2.0G 实验档（坏核满频曾 <4s 致死）"
    return 0
}

# ---------- 门1: 编号漂移预检（防固件重屏蔽后 122 不再是坏核） ----------
drift_preflight() {
    local present offline pkg121 pkg122 pkg123 core121 core122 core123
    present=$(cat "$CPU_SYSFS/present" 2>/dev/null)
    offline=$(cat "$CPU_SYSFS/offline" 2>/dev/null)
    if [ "$present" != "0-127" ]; then
        log "门1: present='$present' ≠ 0-127（固件呈现变化, 逻辑编号可能漂移）— 拒绝上线"
        return 1
    fi
    if [ "$offline" != "122" ]; then
        log "门1: offline='$offline' ≠ 122 — 拒绝上线"
        return 1
    fi
    pkg121=$(cat "$CPU_SYSFS/cpu121/topology/physical_package_id" 2>/dev/null)
    pkg122=$(cat "$CPU_SYSFS/cpu122/topology/physical_package_id" 2>/dev/null)
    pkg123=$(cat "$CPU_SYSFS/cpu123/topology/physical_package_id" 2>/dev/null)
    core121=$(cat "$CPU_SYSFS/cpu121/topology/core_id" 2>/dev/null)
    core122=$(cat "$CPU_SYSFS/cpu122/topology/core_id" 2>/dev/null)
    core123=$(cat "$CPU_SYSFS/cpu123/topology/core_id" 2>/dev/null)
    if [ -z "$pkg121" ] || [ -z "$pkg122" ] || [ -z "$pkg123" ] \
       || [ -z "$core121" ] || [ -z "$core122" ] || [ -z "$core123" ]; then
        log "门1: 拓扑文件缺失 — 拒绝上线"
        return 1
    fi
    if [ "$pkg121" != "$pkg122" ] || [ "$pkg122" != "$pkg123" ] \
       || [ "$core121" -ge "$core122" ] || [ "$core122" -ge "$core123" ]; then
        log "门1: cpu121/122/123 拓扑不符同簇序（pkg $pkg121/$pkg122/$pkg123, core $core121/$core122/$core123）— 编号漂移嫌疑, 拒绝上线"
        return 1
    fi
    log "门1: present/offline/拓扑预检通过（121-123 同簇, core_id $core121 < $core122 < $core123）"
    return 0
}

# ---------- 后验: MPIDR 核对 + 频率读数（不符自动下线） ----------
BAD_MPIDR=0x0900060200
postonline_verify() {
    local mpidr fmax fcur try
    # 后验1: 本启动日志中 CPU122 的 MPIDR 必须是坏核物理 ID（空结果小重试, journald 摄录延迟）
    mpidr=""
    for try in 1 2 3; do
        mpidr=$(journalctl -k -b 0 --no-pager 2>/dev/null \
                | grep "CPU$BAD_CORE: Booted secondary processor" | tail -n1)
        [ -n "$mpidr" ] && break
        sleep 0.3
    done
    if [ -z "$mpidr" ]; then
        log "后验1: journalctl 未见 'CPU$BAD_CORE: Booted' — 自动下线并中止"
        echo 0 > "$CPU_SYSFS/cpu$BAD_CORE/online" 2>/dev/null
        return 1
    fi
    if ! printf '%s' "$mpidr" | grep -q "$BAD_MPIDR"; then
        log "后验1: MPIDR 不符（期望 $BAD_MPIDR, 实际: $mpidr）— 编号漂移! 自动下线并中止"
        echo 0 > "$CPU_SYSFS/cpu$BAD_CORE/online" 2>/dev/null
        return 1
    fi
    log "后验1: MPIDR 核对通过（$mpidr）"
    # 后验2: 频率读数必须 ≤ cap（max 恰等 qos 值; cur 允许取整/过渡容差）
    fmax=$(cat "$CPU_SYSFS/cpu$BAD_CORE/cpufreq/scaling_max_freq" 2>/dev/null)
    fcur=$(cat "$CPU_SYSFS/cpu$BAD_CORE/cpufreq/scaling_cur_freq" 2>/dev/null)
    if [ -z "$fmax" ] || [ "$fmax" -gt $((MOD_CAP + 1000)) ]; then
        log "后验2: scaling_max_freq='$fmax' 超出 cap=$MOD_CAP — qos 疑失效, 自动下线并中止"
        echo 0 > "$CPU_SYSFS/cpu$BAD_CORE/online" 2>/dev/null
        return 1
    fi
    if [ -n "$fcur" ] && [ "$fcur" -gt $((MOD_CAP + 50000)) ]; then
        log "后验2: scaling_cur_freq='$fcur' 超出 cap+50MHz — 当前频率越界, 自动下线并中止"
        echo 0 > "$CPU_SYSFS/cpu$BAD_CORE/online" 2>/dev/null
        return 1
    fi
    log "后验2: 频率读数通过（max=$fmax cur=$fcur ≤ cap=$MOD_CAP）"
    log "后验2(信息): $(dmesg 2>/dev/null | grep 'cpu122-capfreq' | tail -n2 | tr '\n' ' ')"
    return 0
}

case "$ACT" in
2)  # ---- 封顶: 在线改模块频率上限（qos 自动传导, 无需上下线） ----
    log "===== 主功能: 封顶 CPU$BAD_CORE @ ${CAP_ARG}kHz ====="
    module_ready || die "封顶需要模块就绪"
    if [ "$CAP_ARG" -gt "$CAP_HARD_MAX" ]; then
        die "封顶值 $CAP_ARG 超出物理最高频 $CAP_HARD_MAX"
    fi
    [ "$CAP_ARG" -gt "$CAP_WARN_MAX" ] \
        && log "高危警告: 封顶 $CAP_ARG 超出 2.0G 实验档（坏核满频曾 <4s 致死）"
    [ "$CAP_ARG" -eq 0 ] \
        && log "警告: 封顶 0 = 解锁（解除限制: kprobe 直通 + qos 失效）— 仅限满频 A/B 且风险自担"
    err=$( { echo "$CAP_ARG" > "$MOD_SYSFS/cap_khz"; } 2>&1 ) \
        || die "写 cap_khz 失败: $err"
    MOD_CAP=$(cat "$MOD_SYSFS/cap_khz")
    [ "$MOD_CAP" = "$CAP_ARG" ] || die "cap_khz 回读不符: $MOD_CAP ≠ $CAP_ARG"
    log "封顶 $CAP_ARG kHz 已生效"
    if [ "$(cat "$CPU_SYSFS/cpu$BAD_CORE/online" 2>/dev/null)" = "1" ]; then
        sleep 0.2    # qos notifier 经 schedule_work 异步生效
        fmax=$(cat "$CPU_SYSFS/cpu$BAD_CORE/cpufreq/scaling_max_freq" 2>/dev/null)
        if [ -n "$fmax" ] && [ "$fmax" -gt $((MOD_CAP + 1000)) ]; then
            log "警告: 在线读数 scaling_max_freq=$fmax 未跟随 cap=$MOD_CAP（qos 疑未挂载, 检查 dmesg 'cpu122-capfreq'）"
        else
            log "在线验证: scaling_max_freq=$fmax ≤ cap=$MOD_CAP ✓"
        fi
    else
        log "CPU$BAD_CORE 当前离线 — cap 将在下次上线路径内生效"
    fi
    ;;
1)  # ---- 上线: 门0+门1 → 上线（封顶由模块在上线路径内完成）→ 后验 ----
    module_ready            || die "模块未就绪, 拒绝上线（绝不裸奔）"
    cap_sanity_for_online   || die "cap 参数不合法, 拒绝上线"
    drift_preflight         || die "编号漂移预检未过, 拒绝上线"
    log "===== 主功能: CPU$BAD_CORE 【上线】 (online=1, cap=$MOD_CAP kHz 由模块在上线路径内实施) ====="
    err=$( { echo 1 > "$CPU_SYSFS/cpu$BAD_CORE/online"; } 2>&1 ) \
        || die "CPU$BAD_CORE 上线失败: $err"
    cur=$(cat "$CPU_SYSFS/cpu$BAD_CORE/online")
    [ "$cur" = "1" ] || die "CPU$BAD_CORE 状态验证失败: online=$cur（期望 1）"
    log "CPU$BAD_CORE 上线成功, online=1"
    postonline_verify       || die "后验未过, CPU$BAD_CORE 已自动下线, 中止"
    ;;
0)  # ---- 下线 ----
    log "===== 主功能: CPU$BAD_CORE 【下线】 (online=0) ====="
    err=$( { echo 0 > "$CPU_SYSFS/cpu$BAD_CORE/online"; } 2>&1 ) \
        || die "CPU$BAD_CORE 下线失败: $err"
    cur=$(cat "$CPU_SYSFS/cpu$BAD_CORE/online")
    [ "$cur" = "0" ] || die "CPU$BAD_CORE 状态验证失败: online=$cur（期望 0）"
    log "CPU$BAD_CORE 下线成功，当前 online=$cur"
    log "提示: 模块封顶仍在值守, 下次上线自动生效（重启后由 modules-load.d 自动加载）"
    ;;
esac

# ---------- 最终状态 ----------
off=$(cat "$CPU_SYSFS/offline" 2>/dev/null)
log "online : $(cat "$CPU_SYSFS/online")"
log "offline: ${off:-（空）}"
log "完成。"
