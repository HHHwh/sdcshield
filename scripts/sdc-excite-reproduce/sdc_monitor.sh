#!/bin/bash
# sdc_monitor.sh — SDC 7×24 战役监控（以 root 运行：sdc-monitor.service）
#
# 职责（plan §6/§8）:
#   每 60s: IPMI + OS 采样入 CSV 时序（事件回溯依据）
#   安全联锁: 温度(滞回)/风扇/内存/磁盘水位 → PAUSE 标志；SEL Critical → 粘性 PAUSE
#   每 5min: SEL 增量检查
#   cmd/ 命令代理: governor.request（L5 di/dt 阶跃）、snapshot.request（事件 root 取证）
# 与战役驱动互不阻断（独立 systemd 单元，重启各自独立）。
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/sdc_common.sh"

# 目录权限模型：EXCITE_REPRODUCE_DIR 树归运行用户（sdc）所有（install.sh/驱动创建）。
# 本脚本以 root 运行，只补建监控自身目录，并把属主还给 EXCITE_REPRODUCE_DIR 的属主，
# 避免抢建驱动目录（logs/events/stressng）导致 sdc 侧不可写。
# alerts.log 为双进程共写（root 监控 + sdc 驱动），必须 666。
mkdir -p "$EXCITE_REPRODUCE_DIR" "$MON_DIR" "$CMD_DIR"
CSV="$MON_DIR/monitor.csv"
SNAP="$MON_DIR/snapshots"
SEL_EV="$MON_DIR/sel_events"
mkdir -p "$SNAP" "$SEL_EV"
DIR_OWNER=$(stat -c %U "$EXCITE_REPRODUCE_DIR" 2>/dev/null || echo root)
DIR_GROUP=$(stat -c %G "$EXCITE_REPRODUCE_DIR" 2>/dev/null || echo root)
if [ "$DIR_OWNER" != root ]; then
    chown -R "${DIR_OWNER}:${DIR_GROUP}" "$EXCITE_REPRODUCE_DIR" 2>/dev/null
fi
touch "$MON_DIR/alerts.log" 2>/dev/null
chmod 666 "$MON_DIR/alerts.log" 2>/dev/null

# campaign.env 由 sdc-excite-reproduce.sh 首次运行生成；监控可先于驱动启动（用默认阈值）
[ -f "$EXCITE_REPRODUCE_DIR/campaign.env" ] && source "$EXCITE_REPRODUCE_DIR/campaign.env"
PAUSE_C="${THERMAL_PAUSE_C:-95}"
RESUME_C="${THERMAL_RESUME_C:-90}"
DISK_WARN="${DISK_WARN_PCT:-85}"
DISK_STOP="${DISK_STOP_PCT:-95}"
# 板侧联锁传感器适配（默认=参考机 TaiShan 2280 SDR 命名；其他单板用 systemd
# drop-in Environment= 或 campaign.env 覆盖；冒号分隔以容纳含空格的传感器名，
# 名字按 sdr_raw 的正则语义匹配）：
#   THERMAL_SENSORS — 额外 CPU 温度传感器名，取值并入 maxt 参与热联锁
#                     （空=不新增，参考板 c1/c2 路径不变）
#   FAN_SENSORS    — 风扇传感器名，任一缺失/0rpm 视为异常（保持"缺失即异常"）；
#                    置空=显式关闭风扇联锁（无风扇读数板卡记录在案的选择）
THERMAL_SENSORS="${THERMAL_SENSORS:-}"
FAN_SENSORS="${FAN_SENSORS:-FAN2 Speed:FAN3 Speed}"

# ---- monitor.csv v3 表头（v5 §6.4/§6.6/§6.7 单元 4：SDR 发现式列集 + RAS/OS 固定尾列）----
# 列 = ts + 发现式模拟量列（sdr_analog_columns 归一化名，列序持久化于 sensors_v3.json，
#       重启列集一致）+ 固定尾列 mc_ce_total..bmc_ok（EDAC CE/UE、vmstat oom/pgmajfault、
#       NUMA MemAvailable、磁盘水位、SEL 5min 窗口、BMC 通道）。
# 测试钩子（生产不设即走 ipmitool）：MON_SDR_FILE=非空文件 → SDR 从该文件读，不调 ipmitool；
#                                   MON_SELFTEST=1 → 一个完整采样周期后退出（周期末 break）。
fetch_sdr() {   # 每周期单轮询（联锁段仍用同一 $SDR 取值）
    if [ -n "${MON_SDR_FILE:-}" ] && [ -s "$MON_SDR_FILE" ]; then
        cat "$MON_SDR_FILE"
    else
        timeout 20 ipmitool sdr list 2>/dev/null
    fi
}
_edac_sum() {   # $1=ce_count|ue_count → EDAC 全 mc 求和；无 EDAC（无 mc*/）→ 空（缺失=空，不伪造 0）
    local f v t=0 n=0
    for f in /sys/devices/system/edac/mc/mc*/"$1"; do
        [ -r "$f" ] || continue
        v=$(cat "$f" 2>/dev/null)
        case "$v" in ''|*[!0-9]*) continue ;; esac
        t=$((t + v)); n=$((n + 1))
    done
    [ "$n" -gt 0 ] && echo "$t"
    return 0
}
SENSORS_JSON="$MON_DIR/sensors_v3.json"
DISCRETE_MAP="$MON_DIR/discrete_state.map"
DISCRETE_LOG="$MON_DIR/discrete_events.log"
touch "$DISCRETE_LOG" 2>/dev/null    # 离散态转移事件流（Task 3：diff 结果逐行追加 TRANSITION）
V3_TAIL="mc_ce_total,mc_ue_total,oom_kill,pgmajfault,numa0_memavail_kb,numa1_memavail_kb,numa2_memavail_kb,numa3_memavail_kb,disk_pct,sel5m,bmc_ok"
# 启动列集装配（v5 §6.4 discovery）：首次全量发现并持久化；重启重新发现与持久序比对——
# 一致→沿用；漂移（固件升级/换板致 SDR 列集变化）→ 归档旧 CSV（_v3drift_）+ alert +
# 以新列集开新文件；发现为空（BMC 暂不可达）→ 沿用持久序（空发现不构成漂移证据）。
BOOT_SDR=$(fetch_sdr)
NEW_COLS=$(sdr_analog_columns <<<"$BOOT_SDR" | awk -F'|' -v fixed="ts,$V3_TAIL" '
    BEGIN { split(fixed, F, ","); for (i in F) skip[F[i]] = 1 }
    !($1 in skip) { print $1 }' | paste -sd, -)   # 与 ts/尾列重名的发现列剔除（防表头列名冲突）
OLD_COLS=""
[ -f "$SENSORS_JSON" ] && OLD_COLS=$(python3 -c "import json,sys; print(','.join(json.load(open(sys.argv[1])).get('columns', [])))" "$SENSORS_JSON" 2>/dev/null)
V3_COLS="$OLD_COLS"
if [ -n "$NEW_COLS" ] && [ "$NEW_COLS" != "$OLD_COLS" ]; then
    if [ -n "$OLD_COLS" ]; then
        alert "monitor v3 列集漂移（SDR 模拟量列集变化——固件升级/换板？）: 旧[${OLD_COLS}] → 新[${NEW_COLS}]，旧 CSV 归档 _v3drift_"
        [ -f "$CSV" ] && mv "$CSV" "$MON_DIR/monitor_v3drift_$(date +%Y%m%d_%H%M%S).csv"
    fi
    V3_COLS="$NEW_COLS"
    python3 -c "import json,sys,time; json.dump({'columns': [c for c in sys.argv[1].split(',') if c], 'updated': time.strftime('%F %T')}, open(sys.argv[2], 'w'), indent=1)" "$V3_COLS" "$SENSORS_JSON"
fi
CSV_HDR="ts${V3_COLS:+,$V3_COLS},$V3_TAIL"
if [ -f "$CSV" ] && [ "$(head -1 "$CSV")" != "$CSV_HDR" ]; then
    mv "$CSV" "$MON_DIR/monitor_v1_$(date +%Y%m%d_%H%M%S).csv"   # 旧表头数据归档保留（v1/v2 → v3 升级）
    gzip -q "$MON_DIR"/monitor_v1_*.csv 2>/dev/null &
fi
[ -f "$CSV" ] || echo "$CSV_HDR" > "$CSV"

# ipmitool 输出有两种格式（实测）：'sdr list' 3 字段（name|reading|status）、
# 'sdr type X' 5 字段（name|id|status|x|reading）。取 $5，为空则退 $2，两格式通吃。
sdr_raw() { awk -F'|' -v s="$1" '$1 ~ s {v=$5; if (v ~ /^[ ]*$/) v=$2; gsub(/^ +| +$/,"",v); print v}' <<<"$SDR" | head -1; }
sdr_val() { sdr_raw "$1" | grep -oE '[0-9]+(\.[0-9]+)?' | head -1; }
last_sel_id=""
SEL_BASELINE_FILE="$MON_DIR/sel_baseline.txt"
[ -f "$SEL_BASELINE_FILE" ] && last_sel_id=$(cat "$SEL_BASELINE_FILE")   # 跨重启保留 diff 基线
sel_tick=0
sel_fail=0
bmc_fail=0
thermal_paused=0
thermal_hot=0
fan_bad=0
SLEEP_NEXT=60
SEL5M_CARRY=0
PREVSTAT=""
LAST_SNAP=0   # 10 分钟工况快照（用户指令 2026-09-24：每 10min 记录全量工况 + 偏移标记）

paused_for() { [ -f "$PAUSE_FLAG" ] && grep -q "$1" "$PAUSE_FLAG" 2>/dev/null; }
set_pause() { echo "$1: $2 ($(date '+%F %T'))" > "$PAUSE_FLAG"; alert "PAUSE: $2"; }
clr_pause_if() { if paused_for "$1"; then rm -f "$PAUSE_FLAG"; alert "RESUME: $1 已恢复"; fi; }

# ---- monitor v3 周期状态（UE 告警去重 / collector 看门狗计数）----
ue_alerted=0                        # UE>0 每周期至多一条告警；UE 清零后重置
declare -A collector_fail=()
declare -A collector_last_alert=()

# ---- known_faults 白名单（v5 §6.4，Task 3：断言告警的用户定案豁免表）----
# 读取：$EXCITE_REPRODUCE_DIR/known_faults.csv（数据根，每周期重读=热更新），缺失回退
# repo configs/sdc-excite-reproduce/known_faults.csv。行格式 source,type,description,whitelist_action；
# source 与离散传感器名同口径规范化后匹配（PSU Redundancy ↔ psu_redundancy）。
# action=ignore|log_only → 都只记录不告警（控制器 2026-09-26 裁定：log_only 与 ignore 的
# 差异仅在 TRANSITION 行尾加 [known_fault:log_only] 标注——修正 plan 字面的误读）。
declare -A KNOWN_FAULTS=()
load_known_faults() {
    KNOWN_FAULTS=()
    local kf="$EXCITE_REPRODUCE_DIR/known_faults.csv" line s act
    [ -f "$kf" ] || kf="$REPO_DIR/configs/sdc-excite-reproduce/known_faults.csv"
    [ -f "$kf" ] || return 0
    while IFS= read -r line; do
        case "$line" in '#'*|'') continue ;; esac
        s=$(_sdr_norm "$(cut -d',' -f1 <<<"$line")")
        act=$(awk -F',' '{gsub(/\r/,""); gsub(/^ +| +$/,"",$NF); print $NF}' <<<"$line")
        case "$act" in ignore|log_only) KNOWN_FAULTS[$s]=$act ;; esac
    done < "$kf"
    return 0
}

# ---- MON_SELFTEST 测试钩子（生产不设；联锁段 :184-293 逐字保留，跳过动作全部在本段完成）----
# SEL 段在 sel_tick%5==1 的周期调真 ipmitool sel list——测试下置初值 -1 使唯一周期落在
# %5==0，SEL 采集整体安全跳过（不打真 BMC）；cmd 代理段监听的 cmd/ 重定向到监控目录下
# 空目录——governor/snapshot 代理自然空转（不写真 sysfs / ipmitool）。
# exposure 段钩子：MON_ONLINE_FILE=非空文件 → online 核集从该文件读（生产不设 →
# /sys/devices/system/cpu/online）——与 MON_SDR_FILE 同模式。
if [ "${MON_SELFTEST:-0}" = 1 ]; then
    sel_tick=-1
    CMD_DIR="$MON_DIR/cmd_selftest"
    mkdir -p "$CMD_DIR"
fi

# ---- exposure.json 暴露量快照（M5 T1，v5 §9.4 有效分母——M4 移交项）----
# 10min 周期与 condition_10m 对齐：聚合 driver.log 今日日汇总行（loop-count 累计 +
# cycle）+ online 核数积分 → spool/exposure.json **追加** JSONL 快照行（与
# events.jsonl 同风格——追加不覆盖）。字段不可得 → null + source 注记 unavailable
# （不伪造 0）。核时积分状态存 monitor/exposure_state.json：跨重启沿 last_epoch
# 锚点续积分（dt=实际间隔；首启无锚点按名义 600s）、跨日归零、dt 有界 24h。
# T2 起 daily_summary() 在 driver.log 日汇总行补记「loop-count 日累计: N（YAML 文件数 M）」
# token（N=今日 logs/YYYYMMDD/ 逐文件 'loop-count:' 行数之和）→ valid_iterations_today 得真值；
# 旧档/as-built 无 token 的行仍如实降级 null（M4 rate_table 的"不可得"路径保留）。
write_exposure() {
    local dlog="$EXCITE_REPRODUCE_DIR/driver.log" today summary=""
    today=$(date '+%F')
    [ -r "$dlog" ] && summary=$(grep "$today" "$dlog" 2>/dev/null | grep '日汇总' | tail -1)
    EXPOSURE_TODAY="$today" EXPOSURE_SUMMARY="$summary" \
    python3 - "$EXCITE_REPRODUCE_DIR" <<'PYEOF' 2>/dev/null || :
import json, os, re, sys, time

root = sys.argv[1]
today = os.environ["EXPOSURE_TODAY"]
summary = os.environ.get("EXPOSURE_SUMMARY", "")

def _online_count():
    src = os.environ.get("MON_ONLINE_FILE") or "/sys/devices/system/cpu/online"
    try:
        text = open(src).read().strip()
    except OSError:
        return None
    n = 0
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        try:
            if "-" in part:
                a, b = part.split("-")
                n += int(b) - int(a) + 1
            else:
                n += 1
        except ValueError:
            return None
    return n or None

# driver 今日日汇总行 → loop-count 累计 + cycle（token 提取；行在而无 token → None）
# 兼容两种 token 形：`loop-count=123456`（旧档）与 `loop-count 日累计: 123456`（T2 起）
iters = cyc = None
m = re.search(r"loop[-_]count(?:\s*日累计)?\s*[:=：]?\s*(\d+)", summary)
if m:
    iters = int(m.group(1))
m = re.search(r"cycle\s*[:=]?\s*(\d+)", summary)
if m:
    cyc = int(m.group(1))

# online 核数积分（core_hours_today）= Σ dt × online 核数 / 3600
now = time.time()
state_p = os.path.join(root, "monitor", "exposure_state.json")
state = {}
try:
    state = json.load(open(state_p))
except Exception:
    pass
n_online = _online_count()
core_hours = None
if n_online is not None:
    base = float(state.get("core_hours") or 0.0) if state.get("date") == today else 0.0
    try:
        dt = now - float(state.get("last_epoch")) if state.get("last_epoch") is not None else 600.0
    except (TypeError, ValueError):
        dt = 600.0                     # 首启/坏锚点：按名义 10min 周期计
    dt = min(max(dt, 0.0), 86400.0)    # 时钟回拨归零、长停机以 24h 封顶
    core_hours = base + dt * n_online / 3600.0

# source_files：今日 logs/YYYYMMDD/ 的 yaml / yaml.gz 计数（目录列举，恒便宜）
sf = {"yaml_today": 0, "yaml_gz_today": 0}
day_dir = os.path.join(root, "logs", today.replace("-", ""))
if os.path.isdir(day_dir):
    for name in os.listdir(day_dir):
        if name.endswith(".yaml"):
            sf["yaml_today"] += 1
        elif name.endswith(".yaml.gz"):
            sf["yaml_gz_today"] += 1

if iters is None and core_hours is None:
    src = "unavailable"
elif iters is None or core_hours is None:
    miss = []
    if iters is None:
        miss.append("valid_iterations_today:driver.log今日日汇总行无loop-count累计")
    if core_hours is None:
        miss.append("core_hours_today:online核数不可读")
    src = "unavailable(" + ";".join(miss) + ")"
else:
    src = "driver.log日汇总loop-count + online核数积分"

snap = {"ts": time.strftime("%F %T"), "date": today,
        "valid_iterations_today": iters, "core_hours_today": core_hours,
        "online_cpus": n_online, "cycle": cyc, "source_files": sf, "source": src}
spool = os.path.join(root, "spool")
os.makedirs(spool, exist_ok=True)
with open(os.path.join(spool, "exposure.json"), "a", encoding="utf-8") as f:
    f.write(json.dumps(snap, ensure_ascii=False) + "\n")
if core_hours is not None:   # online 不可读时不推锚点——恢复后把未观测间隔计入积分
    json.dump({"date": today, "core_hours": core_hours, "last_epoch": now},
              open(state_p, "w"))
PYEOF
}

while :; do
    ts=$(date '+%F %T')
    SDR=$(fetch_sdr)     # 单轮询（MON_SDR_FILE 测试注入 / ipmitool；联锁段仍用同一 $SDR）
    bmc=1; [ -z "$SDR" ] && bmc=0
    if [ "$bmc" = 0 ]; then
        bmc_fail=$((bmc_fail+1))
        [ $bmc_fail -eq 10 ] && alert "BMC 通道连续失败 10 次，降级 OS 侧采样（acpitz/meminfo/df 不断链）"
    else
        bmc_fail=0
    fi

    c1=$(sdr_val 'CPU1 Core Rem');  c2=$(sdr_val 'CPU2 Core Rem')
    m1=$(sdr_val 'CPU1 MEM Temp');  m2=$(sdr_val 'CPU2 MEM Temp')
    ot=$(sdr_val 'Outlet Temp');    in_=$(sdr_val 'Inlet Temp')
    pw=$(sdr_val '^Power ');        v1=$(sdr_val 'CPU1 VDDAVS'); v2=$(sdr_val 'CPU2 VDDAVS')
    f2=$(sdr_val 'FAN2 Speed');     f3=$(sdr_val 'FAN3 Speed')
    p1=$(sdr_raw 'CPU1 Prochot')
    p2=$(sdr_raw 'CPU2 Prochot')
    tz0=$(awk '{printf "%.1f", $1/1000}' /sys/class/thermal/thermal_zone0/temp 2>/dev/null)
    tz1=$(awk '{printf "%.1f", $1/1000}' /sys/class/thermal/thermal_zone1/temp 2>/dev/null)
    # ---- 电压全轨（每 socket 6 路关键供电）----
    nv1=$(sdr_val 'CPU1 N_VDDAVS'); nv2=$(sdr_val 'CPU2 N_VDDAVS')
    fx1=$(sdr_val 'CPU1 VDDFIX');   fx2=$(sdr_val 'CPU2 VDDFIX')
    h1=$(sdr_val 'CPU1 HVCC');      h2=$(sdr_val 'CPU2 HVCC')
    qa1=$(sdr_val 'CPU1 VDDQ_AB');  qc1=$(sdr_val 'CPU1 VDDQ_CD')
    qa2=$(sdr_val 'CPU2 VDDQ_AB');  qc2=$(sdr_val 'CPU2 VDDQ_CD')
    # ---- CPU 占用率（/proc/stat tick 增量，总 + 分 socket；socket0=cpu0-63）----
    NEWSTAT=$(awk '/^cpu[0-9]/{n=substr($1,4)+0; idle=$5+$6; tot=0; for(i=2;i<=8;i++) tot+=$i; s=(n<64)?0:1; T[s]+=tot; I[s]+=idle} END{print T[0],I[0],T[1],I[1]}' /proc/stat 2>/dev/null)
    ua=""; us0=""; us1=""
    if [ -n "$NEWSTAT" ] && [ -n "${PREVSTAT:-}" ]; then
        read -r PT0 PI0 PT1 PI1 <<< "$PREVSTAT"
        read -r NT0 NI0 NT1 NI1 <<< "$NEWSTAT"
        # busy_delta = (T-I) 的增量；util = busy_delta/total_delta（tick 归一，与采样间隔无关）
        ua=$(awk -v nt0=$NT0 -v ni0=$NI0 -v pt0=$PT0 -v pi0=$PI0 -v nt1=$NT1 -v ni1=$NI1 -v pt1=$PT1 -v pi1=$PI1 \
            'BEGIN{b0=(nt0-ni0)-(pt0-pi0); d0=nt0-pt0; b1=(nt1-ni1)-(pt1-pi1); d1=nt1-pt1;
                  if(d0+d1>0) printf "%.1f", 100*(b0+b1)/(d0+d1); else print ""}')
        us0=$(awk -v nt0=$NT0 -v ni0=$NI0 -v pt0=$PT0 -v pi0=$PI0 'BEGIN{b=(nt0-ni0)-(pt0-pi0); d=nt0-pt0; if(d>0) printf "%.1f", 100*b/d; else print ""}')
        us1=$(awk -v nt1=$NT1 -v ni1=$NI1 -v pt1=$PT1 -v pi1=$PI1 'BEGIN{b=(nt1-ni1)-(pt1-pi1); d=nt1-pt1; if(d>0) printf "%.1f", 100*b/d; else print ""}')
    fi
    PREVSTAT=$NEWSTAT
    # ---- CPU 频率（128 核 min/avg/max kHz——固件热节流会在此显形）----
    FREQ=$(awk 'NR==FNR{next}{f+=$1; if(min==0||$1<min)min=$1; if($1>max)max=$1; n++} END{if(n>0) printf "%d,%d,%d", min, f/n, max; else print ",,"}' /dev/null /sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq 2>/dev/null)
    fmin=$(cut -d, -f1 <<<"$FREQ"); favg=$(cut -d, -f2 <<<"$FREQ"); fmax=$(cut -d, -f3 <<<"$FREQ")
    ma=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
    sw=$(awk '/^SwapTotal/{t=$2}/^SwapFree/{f=$2}END{print t-f}' /proc/meminfo)
    l1=$(awk '{print $1}' /proc/loadavg)
    dp=$(df -P "$EXCITE_REPRODUCE_DIR" | awk 'NR==2{gsub(/%/,"");print $5}')
    # ---- v3 行装配：模拟量按 sensors_v3.json 持久列序取值（缺失列留空——"传感器在但无读数"）----
    arow=$(sdr_analog_columns <<<"$SDR" | awk -F'|' -v cols="$V3_COLS" '
        BEGIN { n = split(cols, C, ","); for (i = 1; i <= n; i++) idx[C[i]] = i }
        ($1 in idx) { V[idx[$1]] = $2 }
        END { s = ""; for (i = 1; i <= n; i++) s = s V[i] ","; printf "%s", s }')
    # ---- v3 固定尾列：RAS（EDAC CE/UE）+ OS（vmstat oom_kill/pgmajfault）+ NUMA MemAvailable（缺则 MemFree 近似，节点缺失=空）----
    mc_ce=$(_edac_sum ce_count); mc_ue=$(_edac_sum ue_count)
    oomk=$(awk '/^oom_kill /{print $2}' /proc/vmstat 2>/dev/null)
    pgmj=$(awk '/^pgmajfault /{print $2}' /proc/vmstat 2>/dev/null)
    # numa 口径（T2 裁定）：MemAvailable，缺则 MemFree 近似——本机 node meminfo 无 MemAvailable 行
    n0=$(awk '/MemAvailable:/{v=$4} /MemFree:/{f=$4} END{print (v!="")?v:f}' /sys/devices/system/node/node0/meminfo 2>/dev/null)
    n1=$(awk '/MemAvailable:/{v=$4} /MemFree:/{f=$4} END{print (v!="")?v:f}' /sys/devices/system/node/node1/meminfo 2>/dev/null)
    n2=$(awk '/MemAvailable:/{v=$4} /MemFree:/{f=$4} END{print (v!="")?v:f}' /sys/devices/system/node/node2/meminfo 2>/dev/null)
    n3=$(awk '/MemAvailable:/{v=$4} /MemFree:/{f=$4} END{print (v!="")?v:f}' /sys/devices/system/node/node3/meminfo 2>/dev/null)
    echo "$ts,${arow}${mc_ce:-},${mc_ue:-},${oomk:-},${pgmj:-},${n0:-},${n1:-},${n2:-},${n3:-},${dp:-},${SEL5M_CARRY:-},$bmc" >> "$CSV"

    # ---- UE 即时告警（v5 §8.6：即时告警不自动 PAUSE——用户决策点；每周期至多一条，UE 清零后重置）----
    if [ -n "$mc_ue" ] && [ "$mc_ue" -gt 0 ] 2>/dev/null; then
        if [ "$ue_alerted" = 0 ]; then
            alert "EDAC UE>0 total=${mc_ue}（v5 §8.6：即时告警不自动 PAUSE——用户决策点）"
            ue_alerted=1
        fi
    else
        ue_alerted=0
    fi

    # ---- collector 看门狗（v5 §16.4 采集断链可见化：连续 3 周期 fail → alert，30min 内不重复）----
    for c in percore pmu ras; do
        if systemctl is-active "sdc-collector@$c" >/dev/null 2>&1; then
            collector_fail[$c]=0
        else
            collector_fail[$c]=$(( ${collector_fail[$c]:-0} + 1 ))
            if [ "${collector_fail[$c]}" -ge 3 ]; then
                _wd_now=$(date +%s)
                if [ $(( _wd_now - ${collector_last_alert[$c]:-0} )) -ge 1800 ]; then
                    collector_last_alert[$c]=$_wd_now
                    alert "collector 看门狗：sdc-collector@$c 连续 ${collector_fail[$c]} 周期 inactive（采集断链，v5 §16.4）"
                fi
            fi
        fi
    done

    # ---- 离散态全量 diff（v5 §6.4：转移即事件；断言（新值非 0x00/ok）即时告警；known_faults 白名单）----
    if [ -n "$SDR" ]; then
        load_known_faults     # 每周期重读（热更新：数据根 known_faults.csv 追加行无需重启监控）
        sdr_discrete_map <<<"$SDR" > "$DISCRETE_MAP.new"
        dtrans=""; drc=0
        dtrans=$(discrete_diff "$DISCRETE_MAP" "$DISCRETE_MAP.new") || drc=$?   # 退出码=变化数，调用处守卫
        if [ -n "$dtrans" ]; then
            while IFS= read -r tline; do
                [ -z "$tline" ] && continue
                tname=${tline%%:*}          # "name: 旧 → 新" → name（规范化名，与 kf source 同口径）
                tnew=${tline##* → }         # 行尾新值（取最后一个 → 之后，INIT 行同构）
                kact="${KNOWN_FAULTS[$tname]:-}"
                tsuf=""; [ "$kact" = log_only ] && tsuf=" [known_fault:log_only]"
                echo "[$ts] TRANSITION ${tline}${tsuf}" >> "$DISCRETE_LOG"
                # 断言告警判定（2026-09-26 评审裁定，三重豁免）：
                #   1) INIT（首启全量快照）不是断言——坏值 INIT 也只记录（消除首启假告警）
                #   2) 新值为正常态（0x00/ok/OK）非断言
                #   3) 白名单 ignore|log_only 都只记录（log_only 差异=行尾标注）
                case "$tline" in
                    *": INIT → "*) ;;
                    *) case "$tnew" in
                           0x00|ok|OK) ;;
                           *) case "$kact" in
                                  ignore|log_only) ;;
                                  *) alert "离散态断言 ${tline}" ;;
                              esac ;;
                       esac ;;
                esac
            done <<< "$dtrans"
        fi
        mv -f "$DISCRETE_MAP.new" "$DISCRETE_MAP"
    fi

    # ---- 10 分钟工况快照 + 偏移标记（用户指令 2026-09-24）----
    # 全量工况人读快照入 condition_10m.log；与上次快照比较，电压/频率/温度/SEL 等
    # 偏移超阈值的字段以 ⚠ 标注（旧值→新值）。状态存 snap_state.json。
    now_s=$(date +%s)
    if [ $((now_s - LAST_SNAP)) -ge 600 ]; then
        LAST_SNAP=$now_s
        sel_total=$(cat "$SEL_EV"/*.txt 2>/dev/null | wc -l)
        c1="$c1" c2="$c2" m1="$m1" m2="$m2" ot="$ot" in_="$in_" \
        v1="$v1" v2="$v2" nv1="$nv1" nv2="$nv2" fx1="$fx1" fx2="$fx2" \
        h1="$h1" h2="$h2" qa1="$qa1" qc1="$qc1" qa2="$qa2" qc2="$qc2" \
        fmin="$fmin" favg="$favg" fmax="$fmax" f2="$f2" f3="$f3" pw="$pw" \
        p1="$p1" p2="$p2" ma="$ma" sw="$sw" dp="$dp" ua="$ua" us0="$us0" us1="$us1" \
        l1="$l1" sel_total="$sel_total" SEL5M_CARRY="$SEL5M_CARRY" \
        SNAP_TS="$(date '+%F %T')" \
        python3 - "$MON_DIR" <<'PYEOF' >> "$MON_DIR/condition_10m.log" 2>/dev/null
import json, os, sys
mon = sys.argv[1]
env = os.environ
def g(k): return env.get(k, "")
TH = {  # 偏移阈值（数值字段）；不在表内 = 只记录不比较
    "c1": 3, "c2": 3, "m1": 3, "m2": 3, "ot": 3, "in_": 3,
    "v1": .01, "v2": .01, "nv1": .01, "nv2": .01, "fx1": .01, "fx2": .01,
    "h1": .01, "h2": .01, "qa1": .01, "qc1": .01, "qa2": .01, "qc2": .01,
    "fmin": 1, "favg": 1, "fmax": 1, "f2": 300, "f3": 300, "pw": 40,
    "ma": 1048576, "sw": 1048576, "dp": 1,
}
LABEL = {"c1": "cpu1_°C", "c2": "cpu2_°C", "m1": "mem1_°C", "m2": "mem2_°C",
         "ot": "outlet_°C", "in_": "inlet_°C", "v1": "VDDAVS_s0_V", "v2": "VDDAVS_s1_V",
         "nv1": "N_VDDAVS_s0_V", "nv2": "N_VDDAVS_s1_V", "fx1": "VDDFIX_s0_V", "fx2": "VDDFIX_s1_V",
         "h1": "HVCC_s0_V", "h2": "HVCC_s1_V", "qa1": "VDDQ_AB_s0_V", "qc1": "VDDQ_CD_s0_V",
         "qa2": "VDDQ_AB_s1_V", "qc2": "VDDQ_CD_s1_V", "fmin": "freq_min_kHz", "favg": "freq_avg_kHz",
         "fmax": "freq_max_kHz", "f2": "FAN2_rpm", "f3": "FAN3_rpm", "pw": "power_W",
         "ma": "memavail_kB", "sw": "swap_used_kB", "dp": "disk_pct"}
prev = {}
try: prev = json.load(open(f"{mon}/snap_state.json"))
except Exception: pass
print(f"===== 工况快照 {os.environ.get('SNAP_TS', __import__('time').strftime('%F %T'))} =====")
print(f"[温度°C] cpu1={g('c1')} cpu2={g('c2')} mem1={g('m1')} mem2={g('m2')} outlet={g('ot')} inlet={g('in_')}")
print(f"[电压V] VDDAVS={g('v1')}/{g('v2')} N_VDDAVS={g('nv1')}/{g('nv2')} VDDFIX={g('fx1')}/{g('fx2')} "
      f"HVCC={g('h1')}/{g('h2')} VDDQ_AB={g('qa1')}/{g('qa2')} VDDQ_CD={g('qc1')}/{g('qc2')}")
print(f"[频率kHz] min={g('fmin')} avg={g('favg')} max={g('fmax')}（低于 2600000 = 节流）")
print(f"[占用] util={g('ua')}% s0={g('us0')}% s1={g('us1')}% load1={g('l1')}")
print(f"[风扇rpm] FAN2={g('f2')} FAN3={g('f3')}  [功耗] {g('pw')}W  prochot={g('p1')}/{g('p2')}")
print(f"[内存] avail={g('ma')}kB swap={g('sw')}kB  [磁盘] {g('dp')}%")
print(f"[SEL] 战役期累计={g('sel_total')} 条，近5min窗口={g('SEL5M_CARRY')}")
dev = []
for k, th in TH.items():
    cur, old = g(k), str(prev.get(k, ""))
    if not cur or not old: continue
    try:
        d = float(cur) - float(old)
        if abs(d) >= th:
            dev.append(f"⚠ {LABEL[k]}: {old} → {cur} ({'+' if d>0 else ''}{d:g})")
    except ValueError: pass
for k, lab in (("p1", "prochot_s0"), ("p2", "prochot_s1")):
    cur = g(k)
    if cur and cur != "0x00":
        dev.append(f"⚠ {lab}: {prev.get(k,'')} → {cur}（断言！）")
if prev and int(g("sel_total") or 0) > int(prev.get("sel_total", 0) or 0):
    dev.append(f"⚠ SEL 新增 {int(g('sel_total')) - int(prev.get('sel_total', 0))} 条")
print("----- 偏移标记（vs 上次快照）-----")
print("\n".join(dev) if dev else "（无偏移）")
state = {k: g(k) for k in list(TH.keys()) + ["p1", "p2", "sel_total", "ua", "us0", "us1", "l1"]}
json.dump(state, open(f"{mon}/snap_state.json", "w"))
PYEOF
        write_exposure   # 暴露量快照（M5 T1）：driver 日汇总聚合 + online 核时积分 → spool/exposure.json
    fi

    # ---- 温度联锁（2026-09-24 08:21 实战事件后收紧：
    #      60s 采样 + 5 分钟升级曾让峰值持续 5 分钟 @105-106C（=Tjmax+1）。
    #      新规则：maxt>=88 时 20s 快采样；PAUSE 95 保持；KILL 条件 =
    #      绝对线 >=100 立即杀 或 连续 2 个热采样；连带杀 stress-ng。
    #      Prochot 传感器本板全程 0x00 无用（节流只见于 SEL Processor State 事件））----
    maxt=0
    for t in "$c1" "$c2"; do [ -n "$t" ] && [ "$t" -gt "$maxt" ] 2>/dev/null && maxt=$t; done
    # 板侧适配：THERMAL_SENSORS（冒号分隔）——参考板外的 CPU 温度命名
    # （如 R240K V2 的 CPU1_TEMP..CPU4_TEMP）经此并入 maxt
    IFS=: read -ra _ts_arr <<< "$THERMAL_SENSORS"
    for s in "${_ts_arr[@]}"; do
        [ -n "$s" ] || continue
        t=$(sdr_val "$s")
        [ -n "$t" ] && [ "$t" -gt "$maxt" ] 2>/dev/null && maxt=$t
    done
    if [ "$maxt" -ge 88 ] 2>/dev/null; then SLEEP_NEXT=20; else SLEEP_NEXT=60; fi
    if [ "$maxt" -ge "$PAUSE_C" ] 2>/dev/null; then
        if ! paused_for thermal; then set_pause thermal "CPU ${maxt}C >= ${PAUSE_C}C"; fi
        thermal_paused=1
        thermal_hot=$((thermal_hot + 1))
        if [ "$maxt" -ge 100 ] 2>/dev/null || [ "$thermal_hot" -ge 2 ]; then
            if pgrep -x sdcshield >/dev/null 2>&1 || pgrep -x stress-ng >/dev/null 2>&1; then
                alert "热升级：${maxt}C（连续${thermal_hot}个热采样）→ KILL 全部负载（绝对线100C/连续2采样）"
                pkill -KILL -x sdcshield 2>/dev/null
                pkill -KILL -x stress-ng 2>/dev/null
                thermal_hot=0
            fi
        fi
    elif [ "$thermal_paused" = 1 ] && [ "$maxt" -le "$RESUME_C" ] 2>/dev/null; then
        clr_pause_if thermal; thermal_paused=0; thermal_hot=0
    else
        thermal_hot=0
    fi

    # ---- 风扇联锁（FAN_SENSORS 任一缺失/0rpm；连续 3 个采样异常才暂停，恢复即放行）----
    #      默认 FAN2/FAN3 Speed（参考板）；板侧经 FAN_SENSORS 覆盖；置空=显式关闭。
    if [ "$bmc" = 1 ] && [ -n "$FAN_SENSORS" ]; then
        fan_bad_names=""; fan_seen=0
        IFS=: read -ra _fs_arr <<< "$FAN_SENSORS"
        for s in "${_fs_arr[@]}"; do
            [ -n "$s" ] || continue
            fan_seen=$((fan_seen+1))
            fv=$(sdr_val "$s")
            if [ -z "$fv" ] || [ "$fv" = 0 ]; then fan_bad_names="${fan_bad_names}${fan_bad_names:+ }$s=${fv:-na}"; fi
        done
        if [ "$fan_seen" -gt 0 ] && [ -n "$fan_bad_names" ]; then
            if [ "$fan_bad" -lt 3 ]; then
                fan_bad=$((fan_bad+1))
                if [ "$fan_bad" = 3 ] && ! paused_for fan; then set_pause fan "$fan_bad_names"; fi
            fi
        else
            fan_bad=0; clr_pause_if fan
        fi
    fi

    # ---- 内存联锁（防 OOM；与背景服务共存保护）----
    if [ -n "$ma" ] && [ "$ma" -lt 2097152 ] 2>/dev/null; then
        ! paused_for mem && set_pause mem "MemAvailable ${ma}kB < 2GB"
    elif [ -n "$ma" ] && [ "$ma" -gt 4194304 ] 2>/dev/null; then
        clr_pause_if mem
    fi

    # ---- 磁盘水位 ----
    if [ -n "$dp" ] && [ "$dp" -ge "$DISK_STOP" ] 2>/dev/null; then
        ! paused_for disk && set_pause disk "disk ${dp}% >= ${DISK_STOP}%（停新日志）"
    elif [ -n "$dp" ] && [ "$dp" -ge "$DISK_WARN" ] 2>/dev/null; then
        [ $(( $(date +%s) % 3600 )) -lt 60 ] && alert "磁盘水位 ${dp}%（告警线 ${DISK_WARN}%）"
    fi

    # ---- SEL 增量（每 5min；事件率风暴告警 + Critical Asserted 粘性暂停）----
    # 2026-09-24 修复：基线持久化跨重启（08:31 重启曾吞掉整个风暴窗口的 diff）+
    # 超时 20s→40s（风暴时 BMC 忙 + CPU 过载会拖慢 ipmitool）+ 采集失败可见化
    sel_tick=$((sel_tick+1))
    if [ $((sel_tick % 5)) -eq 1 ]; then
        selout=$(timeout 40 ipmitool sel list 2>/dev/null)
        if [ -n "$selout" ]; then
            sel_fail=0
            # 修复（2026-09-24）：原 awk 只 gsub 无 print → lastid 恒空 → 增量捕获
            # 从未生效（sel_events 空目录的真正根因）；同时过滤尾部空行
            lastid=$(grep -v '^[[:space:]]*$' <<<"$selout" | tail -1 | awk -F'|' '{gsub(/ /,"",$1); print $1}')
            if [ -n "$last_sel_id" ] && [ "$lastid" != "$last_sel_id" ]; then
                evf="$SEL_EV/$(date +%Y%m%d-%H%M%S).txt"
                awk -F'|' -v a="$last_sel_id" '{gsub(/ /,"",$1); if (strtonum("0x"$1) > strtonum("0x"a)) print}' <<<"$selout" > "$evf"
                nnew=$(grep -c . "$evf" 2>/dev/null || echo 0)
                SEL5M_CARRY=$nnew
                # 事件率异常（如固件节流振荡 5 分钟 60+ 条）——告警不暂停（温度联锁管根因）
                [ "$nnew" -ge 20 ] && alert "SEL 事件风暴：5 分钟新增 ${nnew} 条（$(awk -F'|' '{print $3}' "$evf" | sort | uniq -c | sort -rn | head -2 | tr '\n' ' ')）——典型为热节流振荡，见 $evf"
                if grep -qE 'Critical.*(Asserted|asserted)' "$evf"; then
                    ! paused_for sel && set_pause sel "SEL Critical 事件（粘性：需人工调查后 rm PAUSE）: $(head -1 "$evf")"
                fi
            fi
            last_sel_id=$lastid
            echo "$lastid" > "$SEL_BASELINE_FILE" 2>/dev/null
        else
            sel_fail=$((sel_fail + 1))
            [ $((sel_fail % 6)) -eq 1 ] && alert "SEL 采集连续失败 ${sel_fail} 次（ipmitool 超时？）——事件窗口可能丢失，风暴后应人工核对 ipmitool sel list"
        fi
    fi

    # ---- 命令代理 1: governor（用户指令 2026-09-24：所有 CPU 恒 performance/最高频率
    #      ——只接受 performance 请求，其余拒绝并告警；驱动侧 di/dt 已改为纯负载阶跃）----
    if [ -f "$CMD_DIR/governor.request" ]; then
        g=$(cat "$CMD_DIR/governor.request")
        if [ "$g" = "performance" ]; then
            for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
                echo "$g" > "$c" 2>/dev/null
            done
            mv "$CMD_DIR/governor.request" "$CMD_DIR/governor.done.$(date +%s)"
        else
            mv "$CMD_DIR/governor.request" "$CMD_DIR/governor.rejected.$(date +%s)"
            alert "governor 请求 '$g' 被拒绝（用户指令：全核恒 performance）"
        fi
    fi

    # ---- 命令代理 2: root 取证快照（事件流水线；dmesg/SEL/SDR/DCMI）----
    if [ -f "$CMD_DIR/snapshot.request" ]; then
        sd="$SNAP/$(date +%Y%m%d-%H%M%S)"
        mkdir -p "$sd"; chmod 755 "$SNAP" "$sd"
        timeout 30 ipmitool sdr list > "$sd/sdr.txt" 2>&1
        timeout 30 ipmitool sel list > "$sd/sel.txt" 2>&1
        timeout 30 ipmitool dcmi power reading > "$sd/dcmi.txt" 2>&1
        dmesg > "$sd/dmesg.txt" 2>&1
        mv "$CMD_DIR/snapshot.request" "$CMD_DIR/snapshot.done.$(date +%s)"
    fi

    # MON_SELFTEST 测试钩子：周期末退出——保证一个完整采样周期落盘（生产不设 → 永续循环）
    [ "${MON_SELFTEST:-0}" = 1 ] && break
    sleep "$SLEEP_NEXT"
done
