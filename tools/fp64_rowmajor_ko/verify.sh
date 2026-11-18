#!/usr/bin/env bash
# verify.sh — fp64_rowmajor_ldp_stp 内核模块完整验证流程
#
# 阶段: [1] 用户态参考编译/运行 (原始 ldr q/str q 路径)
#       [2] 内核模块编译 (kbuild, 无报错生成 .ko)
#       [3] insmod 加载 (无内核报错/oops)
#       [4] 计算结果一致性 (内核 vs 用户态参考, 六缓冲 FNV 哈希)
#       [5] rmmod 安全卸载
#
# 需 root 运行: sudo bash verify.sh

set -u

SEED=0x12345678DEADBEEF
MOD=fp64_rowmajor_ldp_stp
DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$DIR/verify.log"

PASS=0; FAIL=0
log()   { printf '%s\n' "$*" | tee -a "$LOG"; }
stage() { if [ "$1" -eq 0 ]; then log "[PASS] $2"; PASS=$((PASS+1));
          else log "[FAIL] $2"; FAIL=$((FAIL+1)); fi; }

: > "$LOG"
log "================================================================"
log " fp64 row-major ldp/stp 内核模块 — 完整验证流程"
log " kernel=$(uname -r)  arch=$(uname -m)  gcc=$(gcc -dumpversion)"
log " seed=$SEED  time=$(date '+%F %T')  user=$(id -un)"
log "================================================================"

# ---------- [1] 用户态参考 (原始 ldr q / str q 路径) ----------
log ""
log "---- [1/5] 用户态参考实现 (原始 ldr q / str q 路径) ----"
gcc -O2 -Wall -Wextra -o "$DIR/userspace_ref" "$DIR/userspace_ref.c" >>"$LOG" 2>&1
stage $? "userspace_ref 编译 (gcc -O2 -Wall -Wextra, 无告警无报错)"
"$DIR/userspace_ref" "$SEED" > "$DIR/ref.out" 2> "$DIR/ref.err"
stage $? "userspace_ref 运行 (exit 0 = 256 sweep 自检通过)"
log "用户态参考输出:"
sed 's/^/    /' "$DIR/ref.out" | tee -a "$LOG"
REF_HASH="$(grep '^HASH ' "$DIR/ref.out")"
[ -n "$REF_HASH" ]; stage $? "获取用户态 HASH 行 (六个缓冲)"

# ---------- [2] 内核模块编译 ----------
log ""
log "---- [2/5] 内核模块编译 (kbuild 外部模块) ----"
make -C "$DIR" clean >>"$LOG" 2>&1
make -C "$DIR" -j"$(nproc)" >>"$LOG" 2>&1
stage $? "内核模块编译 (make, 无报错)"
[ -f "$DIR/$MOD.ko" ]; stage $? "生成 $MOD.ko"
log "模块元数据 (modinfo):"
modinfo "$DIR/$MOD.ko" 2>/dev/null | sed 's/^/    /' | tee -a "$LOG"

# 反汇编验证: 数据通路中 ldr q / str q 应为 0, ldp/stp 存在
QLDR=$(objdump -d "$DIR/$MOD.o" 2>/dev/null | grep -cE 'ldr[[:space:]]+q[0-9]+')
QSTR=$(objdump -d "$DIR/$MOD.o" 2>/dev/null | grep -cE 'str[[:space:]]+q[0-9]+')
LDPN=$(objdump -d "$DIR/$MOD.o" 2>/dev/null | grep -cE 'ldp[[:space:]]+x[0-9]+')
STPN=$(objdump -d "$DIR/$MOD.o" 2>/dev/null | grep -cE 'stp[[:space:]]+x[0-9]+')
log "反汇编统计: ldr q=$QLDR  str q=$QSTR  ldp x=$LDPN  stp x=$STPN"
[ "$QLDR" -eq 0 ] && [ "$QSTR" -eq 0 ] && [ "$LDPN" -gt 0 ] && [ "$STPN" -gt 0 ]
stage $? "指令替换完整 (数据通路无 ldr q/str q, 已用 ldp/stp)"

# ---------- [3] insmod 加载 ----------
log ""
log "---- [3/5] insmod 加载 ----"
DMESG_N0=$(dmesg | wc -l)
insmod "$DIR/$MOD.ko" seed="$SEED" >>"$LOG" 2>&1
stage $? "insmod 加载成功 (无报错)"
lsmod | grep -q "^$MOD "; stage $? "lsmod 确认模块在内核中"
KLOG="$(dmesg | tail -n +$((DMESG_N0 + 1)))"
log "模块 dmesg 输出:"
printf '%s\n' "$KLOG" | grep "$MOD:" | sed 's/^/    /' | tee -a "$LOG"
printf '%s\n' "$KLOG" | grep -qE 'Oops|BUG:|Call trace|kernel fault'
[ $? -ne 0 ]; stage $? "无内核 Oops/BUG/Call trace"

# ---------- [4] 计算结果一致性 ----------
log ""
log "---- [4/5] 计算结果一致性 (内核 ldp/stp vs 用户态 ldr/str) ----"
KERN_HASH="$(printf '%s\n' "$KLOG" | grep -o 'HASH srcA=.*')"
[ -n "$KERN_HASH" ]; stage $? "获取内核 HASH 行 (六个缓冲)"
if [ "$REF_HASH" = "$KERN_HASH" ]; then
    stage 0 "六缓冲 FNV-1a 哈希完全一致 (srcA/srcB/scratch/expected/temp/dst)"
else
    stage 1 "哈希不一致 — 结果比对失败"
    log "  REF : $REF_HASH"
    log "  KERN: $KERN_HASH"
fi
printf '%s\n' "$KLOG" | grep -q "$MOD: PASS"
stage $? "模块内自检: 256 sweep dst==expected (PASS)"

# ---------- [5] rmmod 卸载 ----------
log ""
log "---- [5/5] rmmod 卸载 ----"
rmmod "$MOD" >>"$LOG" 2>&1
stage $? "rmmod 卸载成功 (无报错)"
lsmod | grep -q "^$MOD "; [ $? -ne 0 ]; stage $? "模块已从内核移除"
printf '%s\n' "$(dmesg | tail -n +$((DMESG_N0 + 1)))" | grep -E 'Oops|BUG:|Call trace' >/dev/null
[ $? -ne 0 ]; stage $? "全程 (加载-运行-卸载) 无内核异常"
dmesg | tail -n +$((DMESG_N0 + 1)) | grep "$MOD: unloaded" >/dev/null
stage $? "卸载日志确认 (fp64_rowmajor_ldp_stp: unloaded)"

log ""
log "================================================================"
log " 验证汇总: PASS=$PASS  FAIL=$FAIL   (详细日志: $LOG)"
log "================================================================"
[ "$FAIL" -eq 0 ]
