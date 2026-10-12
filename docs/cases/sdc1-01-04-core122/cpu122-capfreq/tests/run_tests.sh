#!/bin/bash
# =============================================================================
# tests/run_tests.sh — cpu122-online-offline.sh v2 行为测试（fake sysfs，无真实硬件）
# 原理: CPU_SYSFS/MOD_SYSFS 重定向到临时假树 + PATH 前置假 journalctl/dmesg
#       + CPU122_CAPFREQ_TEST_MODE=1 跳过 root 检查与准备1/2（不触碰真实 grub/udev）
# 运行: bash tests/run_tests.sh     退出码 0=全过
# =============================================================================
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SCRIPT=$(cd "$HERE/.." && pwd)/../cpu122-online-offline.sh

PASS=0 FAIL=0 FAILED_NAMES=""

ok()  { PASS=$((PASS+1)); echo "  PASS: $*"; }
bad() { FAIL=$((FAIL+1)); FAILED_NAMES="$FAILED_NAMES
  FAIL: $*"; echo "  FAIL: $*"; }

ENVROOT=""

new_env() {                     # 每个 case 一个干净假树
    ENVROOT=$(mktemp -d /tmp/cpu122test.XXXXXX)
    local c=$ENVROOT/cpu
    mkdir -p "$ENVROOT/fakebin" "$ENVROOT/journal" \
            "$c/cpu121/topology" "$c/cpu122/topology" "$c/cpu122/cpufreq" "$c/cpu123/topology"
    # 全局 CPU 状态（happy 默认，个别 case 覆写）
    echo "0-127"          > "$c/present"
    echo "0-121,123-127"  > "$c/online"
    echo "122"            > "$c/offline"
    # cpu121/122/123 拓扑（同簇、core_id 递增；122 位于 121 与 123 之间）
    for n in 121 122 123; do echo 1 > "$c/cpu$n/topology/physical_package_id"; done
    echo 290 > "$c/cpu121/topology/core_id"
    echo 292 > "$c/cpu122/topology/core_id"
    echo 294 > "$c/cpu123/topology/core_id"
    echo 1 > "$c/cpu121/online"; echo 1 > "$c/cpu123/online"
    # cpu122 离线 + cpufreq 读数（模块武装后的期望值，个别 case 覆写）
    echo 0        > "$c/cpu122/online"
    echo 1450000  > "$c/cpu122/cpufreq/scaling_max_freq"
    echo 1450000  > "$c/cpu122/cpufreq/scaling_cur_freq"
    # 假 journalctl / dmesg
    printf '#!/bin/bash\ncat "%s/journal/boot.log"\n' "$ENVROOT" > "$ENVROOT/fakebin/journalctl"
    printf '#!/bin/bash\ncat "%s/journal/dmesg.log" 2>/dev/null\n' "$ENVROOT" > "$ENVROOT/fakebin/dmesg"
    chmod +x "$ENVROOT/fakebin/"*
    echo "localhost kernel: CPU122: Booted secondary processor 0x0900060200" > "$ENVROOT/journal/boot.log"
    echo "cpu122-capfreq: armed: target=CPU122 cap=1450000 kHz" > "$ENVROOT/journal/dmesg.log"
    # 模块默认已加载（cap=1450000, target=122, perf 已换算）；"未加载" case 用 mod_unload 覆写
    mkdir -p "$ENVROOT/mod/params"
    echo 1450000 > "$ENVROOT/mod/params/cap_khz"
    echo 122    > "$ENVROOT/mod/params/target_cpu"
    echo 148    > "$ENVROOT/mod/params/cap_perf"
}

mod_unload() { rm -rf "$ENVROOT/mod"; }
mod_setcap()  { echo "$1" > "$ENVROOT/mod/params/cap_khz"; }

run_script() {                 # $@ = 脚本参数
    CPU122_CAPFREQ_TEST_MODE=1 \
    CPU_SYSFS="$ENVROOT/cpu" \
    MOD_SYSFS="$ENVROOT/mod/params" \
    PATH="$ENVROOT/fakebin:$PATH" \
    bash "$SCRIPT" "$@"
}

assert_rc()   { [ "$1" -eq "$2" ] && ok "$3" || bad "$3 (rc=$1 期望 $2)"; }
assert_has()  { printf '%s' "$1" | grep -q -- "$2" && ok "$3" || bad "$3 (未命中: $2)"; }
assert_file() { [ "$(cat "$1" 2>/dev/null)" = "$2" ] && ok "$3" \
                || bad "$3 ($(cat "$1" 2>/dev/null) ≠ $2)"; }

# ============================== T2 用例 ==============================

echo "== T2.1 下线基线（验证测试基建与既有路径不回归）=="
new_env
echo 1 > "$ENVROOT/cpu/cpu122/online"          # 122 当前在线
out=$(run_script 下线); rc=$?
assert_rc $rc 0 "T2.1 下线 rc=0"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T2.1 online 文件写 0"
assert_has "$out" "下线成功" "T2.1 输出含 下线成功"
rm -rf "$ENVROOT"

echo "== T2.2 封顶·模块未加载 → 拒绝 =="
new_env; mod_unload
out=$(run_script 封顶 2000000); rc=$?
assert_rc $rc 1 "T2.2 rc=1"
assert_has "$out" "未加载" "T2.2 提示模块未加载"
rm -rf "$ENVROOT"

echo "== T2.3 封顶 2000000（122 离线）→ 写参数 + 提示下次上线生效 =="
new_env
out=$(run_script 封顶 2000000); rc=$?
assert_rc $rc 0 "T2.3 rc=0"
assert_file "$ENVROOT/mod/params/cap_khz" 2000000 "T2.3 cap_khz 写入 2000000"
assert_has "$out" "下次上线" "T2.3 提示下次上线路径内生效"
rm -rf "$ENVROOT"

echo "== T2.4 封顶 2000000（122 在线）→ 读数跟随 =="
new_env; echo 1 > "$ENVROOT/cpu/cpu122/online"
echo 2000000 > "$ENVROOT/cpu/cpu122/cpufreq/scaling_max_freq"   # 桩：模块传导后的读数
out=$(run_script 封顶 2000000); rc=$?
assert_rc $rc 0 "T2.4 rc=0"
assert_has "$out" "在线验证" "T2.4 在线验证输出"
rm -rf "$ENVROOT"

echo "== T2.5 封顶参数校验 =="
new_env
run_script 封顶 >/dev/null;         assert_rc $? 1 "T2.5a 缺 kHz → rc=1"
run_script 封顶 abc >/dev/null;     assert_rc $? 1 "T2.5b 非数字 → rc=1"
run_script 封顶 3100000 >/dev/null; assert_rc $? 1 "T2.5c 超 2.9G → rc=1"
out=$(run_script 封顶 2900000);     assert_rc $? 0 "T2.5d 2900000 高危放行 rc=0"
assert_has "$out" "高危" "T2.5d 高危警告"
out=$(run_script 封顶 0);           assert_rc $? 0 "T2.5e 0=解锁放行 rc=0"
assert_has "$out" "解锁" "T2.5e 解锁警告"
rm -rf "$ENVROOT"

# ============================== T3 用例 ==============================

echo "== T3.1 上线·模块未加载 → 拒绝且不写 online =="
new_env; mod_unload
out=$(run_script 上线); rc=$?
assert_rc $rc 1 "T3.1 rc=1"
assert_has "$out" "门0" "T3.1 门0 拦截输出"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T3.1 online 保持 0（未裸奔上线）"
rm -rf "$ENVROOT"

echo "== T3.2 上线·cap=0 → 拒绝 =="
new_env; mod_setcap 0
out=$(run_script 上线); rc=$?
assert_rc $rc 1 "T3.2 rc=1"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T3.2 online 保持 0"
rm -rf "$ENVROOT"

echo "== T3.3 上线·cap 越界与高危档 =="
new_env; mod_setcap 3100000
run_script 上线 >/dev/null; assert_rc $? 1 "T3.3a cap=3100000 拒绝"
rm -rf "$ENVROOT"
new_env; mod_setcap 2500000
echo 2500000 > "$ENVROOT/cpu/cpu122/cpufreq/scaling_max_freq"
echo 2500000 > "$ENVROOT/cpu/cpu122/cpufreq/scaling_cur_freq"
out=$(run_script 上线); rc=$?
assert_rc $rc 0 "T3.3b cap=2500000 高危警告后放行"
assert_has "$out" "高危" "T3.3b 高危警告输出"
rm -rf "$ENVROOT"

echo "== T3.4 门1·present 漂移 → 拒绝 =="
new_env; echo "0-126" > "$ENVROOT/cpu/present"
out=$(run_script 上线); rc=$?
assert_rc $rc 1 "T3.4 present=0-126 拒绝"
assert_has "$out" "门1" "T3.4 门1 拦截输出"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T3.4 online 保持 0"
rm -rf "$ENVROOT"

echo "== T3.5 门1·offline 异常 → 拒绝 =="
new_env; echo "121,122" > "$ENVROOT/cpu/offline"
run_script 上线 >/dev/null; assert_rc $? 1 "T3.5 offline≠122 拒绝"
rm -rf "$ENVROOT"

echo "== T3.6 门1·拓扑漂移 → 拒绝 =="
new_env; echo 7 > "$ENVROOT/cpu/cpu122/topology/physical_package_id"
run_script 上线 >/dev/null; assert_rc $? 1 "T3.6 拓扑不符拒绝"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T3.6 online 保持 0"
rm -rf "$ENVROOT"

echo "== T3.7 上线 happy path =="
new_env
out=$(run_script 上线); rc=$?
assert_rc $rc 0 "T3.7 rc=0"
assert_file "$ENVROOT/cpu/cpu122/online" 1 "T3.7 online 写 1"
assert_has "$out" "0x0900060200" "T3.7 MPIDR 核对输出"
assert_has "$out" "后验2" "T3.7 频率后验输出"
rm -rf "$ENVROOT"

echo "== T3.8 后验1·MPIDR 不符 → 自动下线 =="
new_env
echo "localhost kernel: CPU122: Booted secondary processor 0x0800060200" > "$ENVROOT/journal/boot.log"
out=$(run_script 上线); rc=$?
assert_rc $rc 1 "T3.8 rc=1"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T3.8 自动下线（online 回 0）"
assert_has "$out" "MPIDR" "T3.8 MPIDR 报错输出"
rm -rf "$ENVROOT"

echo "== T3.9 后验2·qos 疑失效（max 越界）→ 自动下线 =="
new_env
echo 2900000 > "$ENVROOT/cpu/cpu122/cpufreq/scaling_max_freq"
out=$(run_script 上线); rc=$?
assert_rc $rc 1 "T3.9 rc=1"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T3.9 自动下线"
assert_has "$out" "qos 疑失效" "T3.9 qos 失效提示"
rm -rf "$ENVROOT"

echo "== T3.10 后验1·journal 无 Booted 行 → 自动下线 =="
new_env
echo "unrelated line" > "$ENVROOT/journal/boot.log"
run_script 上线 >/dev/null; assert_rc $? 1 "T3.10 无 Booted 行 rc=1"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T3.10 自动下线"
rm -rf "$ENVROOT"

echo "== T3.11 门0·模块 target_cpu 演练态（123）→ 上线拒绝 =="
new_env; echo 123 > "$ENVROOT/mod/params/target_cpu"
out=$(run_script 上线); rc=$?
assert_rc $rc 1 "T3.11 rc=1"
assert_has "$out" "target_cpu" "T3.11 target_cpu 拦截输出"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T3.11 online 保持 0（未裸奔上线）"
rm -rf "$ENVROOT"

echo "== T3.12 门0·模块 target_cpu 演练态（123）→ 封顶拒绝 =="
new_env; echo 123 > "$ENVROOT/mod/params/target_cpu"
out=$(run_script 封顶 2000000); rc=$?
assert_rc $rc 1 "T3.12 rc=1"
assert_has "$out" "target_cpu" "T3.12 target_cpu 拦截输出"
rm -rf "$ENVROOT"

echo "== T3.13 门0·cap_perf 未换算（kprobe 未武装）→ 上线拒绝 =="
new_env; echo 0 > "$ENVROOT/mod/params/cap_perf"
out=$(run_script 上线); rc=$?
assert_rc $rc 1 "T3.13 rc=1"
assert_has "$out" "cap_perf" "T3.13 cap_perf 拦截输出"
assert_file "$ENVROOT/cpu/cpu122/online" 0 "T3.13 online 保持 0"
rm -rf "$ENVROOT"

echo "======================================"
echo "PASS=$PASS FAIL=$FAIL"
[ $FAIL -eq 0 ] || { echo "$FAILED_NAMES"; exit 1; }
