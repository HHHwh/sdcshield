#!/bin/bash
# sdc_common.sh — SDC 7×24 战役共享助手。被 sdc-excite-reproduce.sh 与 sdc_monitor.sh source。
# 所有路径可由 env 覆盖（SDC_BIN / SDC_EXCITE_REPRODUCE_DIR，兼容回退旧名 SDC_CAMPAIGN_DIR），新单板零修改。
# 依据 docs/superpowers/plans/2026-09-23-2102312YVY10M6000038-sdc-7x24-stress-plan.md

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="${SDC_BIN:-$REPO_DIR/builddir/sdcshield}"
EXCITE_REPRODUCE_DIR="${SDC_EXCITE_REPRODUCE_DIR:-${SDC_CAMPAIGN_DIR:-$HOME/sdc-excite-reproduce}}"
STATE_FILE="$EXCITE_REPRODUCE_DIR/state.json"
PAUSE_FLAG="$EXCITE_REPRODUCE_DIR/PAUSE"
CMD_DIR="$EXCITE_REPRODUCE_DIR/cmd"
LOG_ROOT="$EXCITE_REPRODUCE_DIR/logs"
MON_DIR="$EXCITE_REPRODUCE_DIR/monitor"
EVENTS_DIR="$EXCITE_REPRODUCE_DIR/events"
STRESSNG_DIR="$EXCITE_REPRODUCE_DIR/stressng"

ensure_dirs() {
    mkdir -p "$LOG_ROOT" "$MON_DIR" "$EVENTS_DIR" "$CMD_DIR" "$STRESSNG_DIR" \
             "$EXCITE_REPRODUCE_DIR/inventory"
}

# fail-loud：BIN 缺失曾导致驱动静默空转（--list-tests 空输出 → 全部阶段"跳过"）。
# 仅由真正执行 $BIN 的消费者显式调用（驱动/excite.sh）；纯函数消费者（pytest/
# drill_lib/sdc_profile）与 sdc_monitor.sh（眼睛：负载坏时仍须存活观测）不调用。
ensure_bin() {
    if [ ! -x "$BIN" ]; then
        echo "FATAL: sdcshield 二进制不存在或不可执行: $BIN" >&2
        echo "       构建后重试（README「从源码构建」），或 export SDC_BIN=<路径>" >&2
        exit 1
    fi
}

log() {
    echo "[$(date '+%F %T')] $*" >> "$EXCITE_REPRODUCE_DIR/driver.log"
    echo "[driver] $*"
}

alert() {
    echo "[$(date '+%F %T')] ALERT $*" >> "$MON_DIR/alerts.log"
    echo "[ALERT] $*" >&2
}

state_get() { # state_get <key> → stdout
    python3 -c "
import json,sys
try: d=json.load(open(sys.argv[2]))
except Exception: sys.exit(0)
v=d.get(sys.argv[1],'')
print(v if not isinstance(v,(dict,list)) else json.dumps(v))" "$1" "$STATE_FILE" 2>/dev/null
}

state_set() { # state_set <key> <value>
    python3 -c "
import json,sys
p=sys.argv[3]
try: d=json.load(open(p))
except Exception: d={}
d[sys.argv[1]]=sys.argv[2]
json.dump(d,open(p,'w'),indent=1)" "$1" "$2" "$STATE_FILE"
}

check_pause() { # 驱动侧：联锁暂停等待（PAUSE 内容为原因文本）
    while [ -f "$PAUSE_FLAG" ]; do
        echo "[$(date '+%F %T')] PAUSED: $(cat "$PAUSE_FLAG" 2>/dev/null)"
        sleep 30
    done
}

mem_avail_kb() { awk '/MemAvailable/{print $2}' /proc/meminfo; }

max_mdim_for_threads() { # $1=线程数 → 全核 GEMM mdim 上限
    # scratch ≈ 3*N^2*8B*threads（dgemm 双精度三矩阵），预算 ≤ MemAvailable/2
    python3 -c "
import sys, math
avail = int(open('/proc/meminfo').read().split('MemAvailable:')[1].split()[0]) * 1024
t = int(sys.argv[1]); n = int(math.sqrt(avail * 0.5 / (3 * 8 * t)))
p = 16
while p * 2 <= n: p *= 2
print(min(p, 4096))" "$1"
}

# ---------------- 事件提取（handle_failure 定向复测依据）----------------
# 输入: sdcshield stdout_summary（框架退出时打印的精简失败报告，含 - test: / result: / fail: 块）
# 注意 result 必须同时认 fail 与 crash——RCA 附3 根因：旧实现只匹配 'result: *fail'，
# 2026-09-25 cold_c4 事件（result: crash）提取为空 → 落入 wholecmd 重放（900s×3 重放泄漏命令）
# 而非 120s 定向复测；crash 事件的 fail: 块同样带 AES 种子，故两函数口径一致。
extract_failed_test() {  # $1=stdout_summary 路径 → 首个 fail/crash 事件的测试名（无则空）
    awk '/^- test:/{t=$3} /^  result: *(fail|crash)/{print t; exit}' "$1" 2>/dev/null
}
extract_fail_seed() {    # $1=stdout_summary 路径 → 首个 fail 块内的 AES 种子（无则空）
    grep -m1 '^  fail: {' "$1" 2>/dev/null | grep -oE "AES:[0-9a-f]+"
}

# ---------------- 复现队列产出 + verify.request 消费（M3 T5 驱动集成）----------------
# M2 controller 的 enqueue_reproduction 动作（账本行）在驱动侧的落点：handle_failure
# 台账行后入队 spool/repro_queue/，由 tools/excite/sdc_reproducer.py --from-queue
# 消费（from_event→真实性门禁→capsule+概率复测×3→spool/repro_done，闭环 M3）。
enqueue_repro() {  # enqueue_repro <event_dir> [test] [seed] → spool/repro_queue/<epoch-ns>.json
    local evdir="$1" t="${2:-?}" s="${3:-none}" q
    mkdir -p "$EXCITE_REPRODUCE_DIR/spool/repro_queue"
    q="$EXCITE_REPRODUCE_DIR/spool/repro_queue/$(date +%s%N).json"
    printf '{"event_dir": "%s", "test": "%s", "seed": "%s", "label": "%s", "queued_at": "%s"}\n' \
        "$evdir" "$t" "$s" "$(basename "$evdir")" "$(date -Is)" > "$q.tmp" \
        && mv "$q.tmp" "$q"   # tmp+mv 原子落位（controller 同纪律）：消费者不读半截
}

# cmd/verify.request 消费（M2 T5 移交③闭环）：monitor v3 联锁 PAUSE 时 controller
# 写的只读校验请求（interlock→BLACK）。最小消费者：记台账一行 + touch
# snapshot.request（root monitor 现有 60s 代理吃）+ 按 monitor 命令代理惯例终态化
# verify.done.<epoch>（mv 保留原件内容供审计）。幂等：无请求即 no-op。
consume_verify_request() {
    if [ -f "$CMD_DIR/verify.request" ]; then
        alert "interlock verify 触发（M2 controller）: $(cat "$CMD_DIR/verify.request" 2>/dev/null)"
        touch "$CMD_DIR/snapshot.request"   # root 监控 60s 内补 dmesg/SEL/SDR 快照
        mv "$CMD_DIR/verify.request" "$CMD_DIR/verify.done.$(date +%s)"
    fi
}

# ---------------- 复测内存护栏 + control 孤儿清理（Task 5，RCA 附3 建议 1/2/3）----------------
# v5 §8.6 分工：主负载的内存防线是 monitor 联锁——护栏只针对 handle_failure 的复测，
# 主阶段路径零内存检查（test_no_mem_checks_in_main_phase_paths 守护此边界）。
# RCA：cold_c4 的 wholecmd 重放（900s×3）在内存紧张期推进 OOM，且泄漏命令留下
# comm=control 的孤儿切片（sandstone_run.cpp child_run 以 prctl 改名，pkill -x sdcshield 够不着）。
mem_below() {  # 纯判定（测试直测）：$1=当前 MemAvailable kB，$2=阈值 kB；低于→0；空/非数值→1（不拦）
    [ -n "$1" ] && [ -n "$2" ] || return 1
    [ "$1" -lt "$2" ] 2>/dev/null
}

cleanup_orphans() { # 清本用户 control 孤儿切片；-u 限定当前用户防误杀他人进程
    # 测试沙盒：SDC_ORPHAN_PIDS（逗号分隔 pid 表）设置时只杀这些 pid（pytest 用），
    # 不设走生产 pkill——战役运行期跑测试必须经测试助手设此变量（test 文件头红线：
    # 2026-09-26 无沙盒时期实测污染战役，pkill 与运行中切片 comm=control 不可区分）
    if [ -n "${SDC_ORPHAN_PIDS:-}" ]; then
        local p; for p in ${SDC_ORPHAN_PIDS//,/ }; do kill -KILL "$p" 2>/dev/null; done
        return 0
    fi
    # 只清「真孤儿」control 切片（父进程已死、被 init 收养 ppid==1），避免误杀仍在运行的
    # 战役子进程（comm 同为 control 但 ppid 指向存活 sdcshield 主进程）。
    # 2026-10-13 0202 板实测：pkill -x control 跨战役误杀四路 mesh/eigen/arm0102
    # 定向战役子进程，产生伪 crash（code 9 Killed）——改 ppid==1 过滤后不再误伤。
    local pid ppid
    for pid in $(pgrep -x control -u "$(id -un)" 2>/dev/null); do
        ppid=$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d ' ')
        [ "$ppid" = "1" ] && kill -KILL "$pid" 2>/dev/null
    done
    return 0        # 无孤儿是常态，归零
}

_cur_memavail_kb() { mem_avail_kb; }   # 取数钩子：测试 monkeypatch 覆写以注入内存值

retest_guarded() { # <label> <retests.txt> <timeout时长> <命令...> → 命令 rc；250=内存门拦截
    # 前置门：MemAvailable < 6GB → 等 MEMGUARD_WAIT_S(默认60s) 重取，仍低 → 记 skipped 返回 250
    # 看门狗：运行期每 MEMWATCH_POLL_S(默认5s) 查 < 2.5GB → KILL 复测进程组并记 killed
    # kill 语义（2026-09-26 两轮实证）：本机 coreutils timeout 自成进程组长（pgid=
    # timeout pid，sdcshield main 亦在其组）→ kill -KILL $rpid + kill -KILL -- -$rpid
    # 收 timeout+main；但框架切片 signals_init_child() setsid() 自成会话/组（真机
    # 实测 control 切片 pgid=sess=自pid）——组杀够不着，须 cleanup_orphans 按
    # comm=control 收（sdcshield 对 TERM 无响应前科 → 直接 KILL 不做 TERM 阶梯）；
    # -$cpid 兜底兼容"子进程自成组"形态；wait $rpid 收尸（timeout 被 KILL → 137）
    # killed(memwatch) 记录取击杀即复核（kill -0）：pid 已被 bash 收尸 = ma 读取
    # 期间自然退出（假阳性窗口），不记；zombie 仍在表则记（实证 5/5 不漏）
    local label="$1" rtxt="$2" tmo="$3"; shift 3
    local ma cpid rpid hit
    ma=$(_cur_memavail_kb)
    if mem_below "$ma" 6291456; then            # < 6GB：等一手再判（可能只是事件余波）
        sleep "${MEMGUARD_WAIT_S:-60}"
        ma=$(_cur_memavail_kb)
        if mem_below "$ma" 6291456; then
            echo "$label skipped(memguard ma=${ma}kB)" >> "$rtxt"
            return 250
        fi
    fi
    timeout --signal=TERM --kill-after=60s "$tmo" "$@" &
    rpid=$!
    while kill -0 "$rpid" 2>/dev/null; do
        sleep "${MEMWATCH_POLL_S:-5}"
        kill -0 "$rpid" 2>/dev/null || break
        ma=$(_cur_memavail_kb)
        if mem_below "$ma" 2621440; then        # < 2.5GB：灭组，防复测把机器推进 OOM
            cpid=$(ps --ppid "$rpid" -o pid= 2>/dev/null | head -1 | tr -d ' ')
            kill -KILL "$rpid" 2>/dev/null
            hit=0; kill -0 "$rpid" 2>/dev/null && hit=1
            kill -KILL -- "-$rpid" 2>/dev/null
            [ -n "$cpid" ] && kill -KILL -- "-$cpid" 2>/dev/null
            cleanup_orphans   # 收 setsid 自成组的 control 切片（组杀够不着）
            [ "$hit" -eq 1 ] && echo "$label killed(memwatch ma=${ma}kB)" >> "$rtxt"
            break
        fi
    done
    wait "$rpid"
    return $?
}

# ---------------- monitor v3: SDR 发现式解析（v5 §6.4 归一化 + 单元 4）----------------
# 输入: stdin = 一次 ipmitool sdr list 全量输出（两种实测格式通吃: 3 字段 name|reading|status
#       / 5 字段 name|id|status|x|reading——取 $5 空则退 $2，与 sdc_monitor.sh 既有 sdr_raw 同口径）
# 分类三态（2026-09-26 评审根修：真机读数带单位——实证 inventory 03_ipmitool_sdr_list.txt
#   "35 degrees C"/"276 Watts"/"0.88 Volts"，旧"纯数值=模拟量"判定使真机模拟量恒 0 列、
#   离散图混入带单位读数与 no reading）：
#   读数 0x 开头 → discrete（十六进制状态码）
#   非 0x 且含数字 → analog（提取首个数值："276 Watts"→276、"20.52 Amps"→20.52）
#   no reading/Not Readable/空/纯文字无数值 → skip（两边都不入，缺失不伪造）
#   两函数消费同一 _sdr_classify 三态输出，分类互补不重不漏。
_sdr_norm() { tr -c 'a-zA-Z0-9\n' '_' <<<"$1" | tr 'A-Z' 'a-z' | sed 's/_*$//;s/^_*//'; }
_sdr_classify() {    # stdin: sdr list 全量 → stdout: 每行 规范化名|原始读数|analog|discrete|skip
    awk -F'|' 'NF>=3 {
        v=$5; if (v ~ /^[ ]*$/) v=$2; gsub(/^ +| +$/,"",v); gsub(/^ +| +$/,"",$1)
        cl = "skip"
        if (v ~ /^0x/) cl = "discrete"
        else if (v ~ /[0-9]/) cl = "analog"
        if ($1 == "") cl = "skip"
        print $1 "|" v "|" cl
    }' | while IFS='|' read -r name val cl; do
        printf "%s|%s|%s\n" "$(_sdr_norm "$name")" "$val" "$cl"
    done
}
sdr_analog_columns() {   # stdin: sdr list 全量 → stdout: 每行 规范化名|提取数值（仅模拟量）
    _sdr_classify | awk -F'|' '$3=="analog" && match($2, /[0-9]+(\.[0-9]+)?/) { print $1 "|" substr($2, RSTART, RLENGTH) }'
}
sdr_discrete_map() {     # stdin: sdr list 全量 → stdout: 每行 规范化名|原始读数（仅离散量）
    _sdr_classify | awk -F'|' '$3=="discrete" { print $1 "|" $2 }'
}
# discrete_diff <旧map文件> <新map文件>: 输出变化行（旧无=INIT），退出码=变化数（0=无变化）
discrete_diff() {
    local old="$1" new="$2" rc=0 line name val
    [ -f "$old" ] || touch "$old"
    while IFS='|' read -r name val; do
        [ -z "$name" ] && continue
        [ -z "$val" ] && continue   # 空读数传感器不参与 diff（防"每周期 INIT"误报，T1 评审 2b）
        prev=$(grep -m1 "^${name}|" "$old" | cut -d'|' -f2-)
        if [ -z "$prev" ]; then echo "${name}: INIT → ${val}"; rc=$((rc+1))
        elif [ "$prev" != "$val" ]; then echo "${name}: ${prev} → ${val}"; rc=$((rc+1)); fi
    done < "$new"
    return $rc
}
