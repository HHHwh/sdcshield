#!/bin/bash
# SDC 狩猎 v3（all-128 法）— 2026-09-24 用户指定（选项 2）：
#   全部 128 核（含故障核 122 = MPIDR 0x0900060200，已降频封顶 1.45 GHz）逐个运行
#   sve 清单测试各 30 分钟；fail → 保留日志；pass → 日志同样保留
#   （2026-10-13 审查修复 D-009 同改：原"确认 pass 后删除日志"改为全保留，
#    与 --fatal-skips 配合——skip/异常不可静默吞掉，事后可审计）。
#
# 相比 v2（scripts/run/run-sdc-hunt-30min-cpu10-127.sh，10-127 法，未运行）的改动：
#   1. --cpuset 0..127 全核（122 降频后全核 60s 冒烟已通过，0-9 闲置不再必要；
#      框架不支持范围语法，故枚举 128 个单值；cpu0 无 online 文件属正常——不可下线）
#   2. 新增预检 + 每测试复查：cpu122 必须处于 1.45 GHz 封顶——封顶丢失即中止
#      （2.9 GHz 负载下 122 两度带崩整机：9-17 panic 环、9-22 22:29 未起跑即关机；
#       降频后同种子同负载 60s×2 + 全核 60s 全 pass，见 smoke-*-20260924-* 日志）
#   3. 其余机制（MPIDR 核对 / START-END fsync 死亡取证 / 断点续跑 / 死亡嫌疑隔离 /
#      40min wall 兜底 / 每轮磁盘复查 / fail 先提取详情再谈日志去留）全部沿用 v2
#
# 归属背景：9-22 长测（v1，33+1 项）全程在 127-CPU 启动（logical 122=旧123 健康核）上
#   跑——本狩猎是故障核旧122 首次真正参与长时窗检测，故从头跑全部 116 项（不跳过）。
#
# ⚠️ 机器死亡后恢复流程（grub 带 maxcpus=122，重启后 122-127 默认 offline）：
#     root 逐个上线：for c in $(seq 122 127); do echo 1 > /sys/devices/system/cpu$c/online; done
#     root 重新封顶：echo 1450000 > /sys/devices/system/cpu/cpu122/cpufreq/scaling_max_freq
#     然后重新运行本脚本——已完成测试自动跳过；死亡窗口测试已在隔离名单，需人工决定是否重跑。
#
# 用法: mkdir -p sdc_hunt_logs/2026-09-24-hunt-all128-122cap1450
#       setsid nohup bash scripts/run/run-sdc-hunt-30min-all128.sh \
#            >> sdc_hunt_logs/2026-09-24-hunt-all128-122cap1450/console.log 2>&1 &

set -u

REPO=/home/sdc/root-xupeng/sdcshield
BIN=$REPO/builddir/sdcshield
LIST_FILE=${1:-$REPO/docs/docs_xu/sve-test-inventory.txt}
OUT_DIR=$REPO/sdc_hunt_logs/2026-09-24-hunt-all128-122cap1450
PER_TEST_MS=1800000          # 30 分钟
GUARD_SECS=2400              # 40 分钟 wall-clock 兜底
MIN_FREE_GB=50
TARGET_CPU=122
TARGET_MPIDR=0x0900060200    # SDC 故障核（2026-09-23 用户确认原始情报指旧122）
CAP_KHZ=1450000              # 2026-09-24 用户降频封顶值
CAP_TOLERANCE=1500000        # 封顶判定上限（≥此值视为封顶丢失）

mkdir -p "$OUT_DIR"
STATUS=$OUT_DIR/STATUS.txt
RESULTS_CSV=$OUT_DIR/summary.csv
SUSPECTS=$OUT_DIR/death-suspects.txt
[ -f "$RESULTS_CSV" ] || echo "test_id,start_time,end_time,result,has_sdc_log,log_path" > "$RESULTS_CSV"

log_sync() {   # 立即落盘：死亡窗口取证依赖此函数
    echo "$*" >> "$STATUS"
    sync "$STATUS"
}

# ---------------- 预检 ----------------
fail_preflight() {
    echo "PREFLIGHT FAIL: $*" >> "$STATUS"
    echo "PREFLIGHT FAIL: $*"
    exit 1
}

# 0) 故障核 122 必须处于降频封顶（本狩猎的安全前提）
read_cap() { cat /sys/devices/system/cpu/cpu$TARGET_CPU/cpufreq/scaling_max_freq 2>/dev/null; }
CUR_CAP=$(read_cap)
[ -n "$CUR_CAP" ] && [ "$CUR_CAP" -le "$CAP_TOLERANCE" ] \
    || fail_preflight "cpu$TARGET_CPU 未处于降频封顶（scaling_max_freq=${CUR_CAP:-unset}，需 root: echo $CAP_KHZ > /sys/devices/system/cpu/cpu$TARGET_CPU/cpufreq/scaling_max_freq）——2.9 GHz 负载两度带崩整机，拒绝开跑"

# 1) 故障核 122 必须在线（本狩猎就是冲它来的）
[ "$(cat /sys/devices/system/cpu/cpu$TARGET_CPU/online 2>/dev/null)" = "1" ] \
    || fail_preflight "cpu$TARGET_CPU 不在线——先上线它"

# 2) 0-127 必须全部在线（cpu0 无 online 文件 = 不可下线，视为在线）
OFFLINE_CORES=""
for c in $(seq 0 127); do
    v=$(cat /sys/devices/system/cpu/cpu$c/online 2>/dev/null)
    [ -z "$v" ] && v=1
    [ "$v" = "1" ] || OFFLINE_CORES="$OFFLINE_CORES $c"
done
[ -z "$OFFLINE_CORES" ] || fail_preflight "以下核不在线:$OFFLINE_CORES（重启后 maxcpus=122 生效时需 root 手动上线 122-127）"

# 3) MPIDR 核对（任务规则：跨启动编号必以 MPIDR 为准）
MPIDR_LINE=$(journalctl -k -b 0 --no-pager 2>/dev/null | grep -E "CPU$TARGET_CPU: Booted secondary processor" | tail -1)
echo "$MPIDR_LINE" | grep -q "$TARGET_MPIDR" \
    || fail_preflight "cpu$TARGET_CPU 的 MPIDR 不是 $TARGET_MPIDR（$MPIDR_LINE）——编号漂移，中止"

# 4) 二进制与清单
[ -x "$BIN" ] || fail_preflight "缺少 $BIN"
[ -r "$LIST_FILE" ] || fail_preflight "缺少清单 $LIST_FILE"
TOTAL=$(grep -c . "$LIST_FILE")
[ "$TOTAL" -gt 0 ] || fail_preflight "清单为空"

# 5) 磁盘余量
FREE_GB=$(df -BG --output=avail /home | tail -1 | tr -dc '0-9')
[ "$FREE_GB" -ge "$MIN_FREE_GB" ] || fail_preflight "/home 仅剩 ${FREE_GB}G (< ${MIN_FREE_GB}G)"

# ---------------- 断点续跑 + 死亡嫌疑隔离 ----------------
DONE_TESTS=",$(tail -n +2 "$RESULTS_CSV" | cut -d, -f1 | paste -sd, -),"
is_done() { case "$DONE_TESTS" in *",$1,"*) return 0 ;; *) return 1 ;; esac; }

if [ -f "$STATUS" ]; then
    DANGLING=$(awk '/^START /{t=$2} /^(END|QUARANTINED) /{t=""} END{print t}' "$STATUS")
    if [ -n "$DANGLING" ] && ! is_done "$DANGLING"; then
        {
            echo "$(date '+%Y-%m-%d %H:%M:%S')  死亡嫌疑: $DANGLING"
            echo "  依据: STATUS.txt 中 START 无 END 且 summary 无结果——机器可能死在该测试运行窗口"
            echo "  处置: 本轮跳过；残留日志保留在 $OUT_DIR/${DANGLING}.yaml（未删除）"
            ls -la "$OUT_DIR/${DANGLING}."* 2>/dev/null | sed 's/^/  /'
        } >> "$SUSPECTS"
        log_sync "QUARANTINED $DANGLING $(date '+%F %T') → death-suspects.txt（需人工决定是否单独重跑）"
        echo "$DANGLING,$(date '+%F %T'),,quarantined(death-suspect),,kept" >> "$RESULTS_CSV"
        sync "$RESULTS_CSV"
        DONE_TESTS="$DONE_TESTS$DANGLING,"
    fi
fi

# ---------------- 头部 ----------------
{
    echo "# SDC hunt v3 — all 128 cores, incl. faulty cpu$TARGET_CPU capped at ${CAP_KHZ}kHz, 30 min per test, keep-on-fail"
    echo "# start: $(date '+%F %T')   kernel: $(uname -r)   online: $(cat /sys/devices/system/cpu/online)"
    echo "# target: cpu$TARGET_CPU = $TARGET_MPIDR  [$MPIDR_LINE]"
    echo "# cpu$TARGET_CPU freq: max=$(read_cap) cur=$(cat /sys/devices/system/cpu/cpu$TARGET_CPU/cpufreq/scaling_cur_freq 2>/dev/null) governor=$(cat /sys/devices/system/cpu/cpu$TARGET_CPU/cpufreq/scaling_governor 2>/dev/null)"
    echo "# binary: $BIN ($(date -r "$BIN" '+%F %T'))   inventory: $LIST_FILE ($TOTAL tests)"
    echo "# resume: $(tail -n +2 "$RESULTS_CSV" | wc -l) done   free: ${FREE_GB}G   guard: ${GUARD_SECS}s"
    echo "# markers are fsync'd — a START without END = machine-death window (that test goes to death-suspects.txt on relaunch)"
    echo "# cap re-checked before EVERY test: lost cap (>= ${CAP_TOLERANCE}kHz) = abort"
} >> "$STATUS"
log_sync "# cpu list: $(seq 0 127 | paste -sd, -)"

CPU_LIST=$(seq 0 127 | paste -sd, -)

# ---------------- 主循环 ----------------
IDX=0
for T in $(grep . "$LIST_FILE"); do
    IDX=$((IDX+1))
    is_done "$T" && { echo "[$IDX/$TOTAL] $T 已完成（resume 跳过）"; continue; }

    # 封顶复查（防中途解封/状态漂移：2.9 GHz 负载两度带崩整机）
    CUR_CAP=$(read_cap)
    if [ -z "$CUR_CAP" ] || [ "$CUR_CAP" -gt "$CAP_TOLERANCE" ]; then
        log_sync "ABORT cap-lost cpu$TARGET_CPU max_freq=${CUR_CAP:-unset} before $T $(date '+%F %T')"
        echo "cpu$TARGET_CPU 封顶丢失（max_freq=${CUR_CAP:-unset}），中止；root 恢复封顶后 resume 继续"
        exit 1
    fi

    START=$(date '+%Y-%m-%d %H:%M:%S')
    log_sync "START $T $START"

    LOG=$OUT_DIR/${T}.yaml
    CONSOLE=$OUT_DIR/${T}.console
    rm -f "$LOG" "$CONSOLE"   # 仅在真正要跑时清残留（被隔离测试的残留不删——那是死亡窗口证据）

    # 30 分钟，全核 0-127（含降频封顶的 122），yaml 显式命名；40 分钟 wall-clock 兜底；行缓冲 console
    # --fatal-skips：skip 也算失败（D-009 同改——狩猎窗口内 skip 不可静默通过）
    timeout -k 60 "$GUARD_SECS" stdbuf -oL -eL "$BIN" \
        -e "$T" -t "$PER_TEST_MS" --fatal-skips --cpuset="$CPU_LIST" -o "$LOG" > "$CONSOLE" 2>&1
    RC=$?
    END=$(date '+%Y-%m-%d %H:%M:%S')

    # ---- SDC 双重判定（退出码 + 日志内容） ----
    RESULT="pass"
    [ $RC -ne 0 ] && RESULT="nonzero_exit($RC)"
    HAS_SDC=0
    if [ -f "$LOG" ]; then
        grep -qE "result: (fail|crash)" "$LOG" && HAS_SDC=1
        grep -qE "data-miscompare|E> Failed|miscompare" "$LOG" && HAS_SDC=1
        grep -qE "^exit: (fail|crash)" "$LOG" && HAS_SDC=1
    fi

    # 红队修复: RC=0 但日志含 SDC 记录时 result 列不可写 pass——否则同一行
    # "pass,1" 自相矛盾, "grep ,pass," 统计会误判; 记为 sdc_log。
    # RC!=0 时保留 nonzero_exit(rc) 不覆盖(退出码信息更有价值)
    [ "$HAS_SDC" -eq 1 ] && [ $RC -eq 0 ] && RESULT="sdc_log"

    if [ "$HAS_SDC" -eq 1 ] || [ $RC -ne 0 ]; then
        # 立即提取关键归因信息（批次 2 教训：先提取，再谈日志去留）
        {
            echo "=== $T  rc=$RC  $START → $END ==="
            grep -nE "result: (fail|crash)|miscompare|E> Failed" "$LOG" 2>/dev/null | head -20
        } >> "$OUT_DIR/fail-details.txt"
        log_sync "END $T SIGNAL rc=$RC sdc=$HAS_SDC kept $END"
        echo "$T,$START,$END,$RESULT,$HAS_SDC,$LOG" >> "$RESULTS_CSV"
        sync "$RESULTS_CSV"
        echo "  [$IDX/$TOTAL] ★★ $T: 信号 (rc=$RC, sdc=$HAS_SDC) → 日志保留 $LOG"
    else
        # 双保险均无 → pass；日志一律保留（D-009 同改：不删，供事后审计）
        log_sync "END $T pass kept $END"
        echo "$T,$START,$END,pass,0,$LOG" >> "$RESULTS_CSV"
        sync "$RESULTS_CSV"
        echo "  [$IDX/$TOTAL] ✓ $T: 无 SDC → 日志保留 $LOG"
    fi

    # 磁盘余量复查
    FREE_GB=$(df -BG --output=avail /home | tail -1 | tr -dc '0-9')
    if [ "$FREE_GB" -lt "$MIN_FREE_GB" ]; then
        log_sync "ABORT lowdisk ${FREE_GB}G after $T $(date '+%F %T')"
        echo "磁盘不足（${FREE_GB}G），中止；resume 后继续"
        exit 1
    fi
done

log_sync "ALLDONE $(date '+%F %T')"
echo "=============================================="
echo " SDC 狩猎 v3（all-128）全部完成  $(date '+%F %T')"
echo " 汇总: $RESULTS_CSV"
echo " 有信号条目:"
# tail rc 恒 0 使 || 永不触发——补 grep . 让"全 pass 无信号"时也能打出 (无)
grep -vE ",pass,0," "$RESULTS_CSV" | tail -n +2 | grep . || echo "  (无)"
echo " 死亡嫌疑: $(grep -c . "$SUSPECTS" 2>/dev/null || echo 0) 条（$SUSPECTS）"
echo "=============================================="
