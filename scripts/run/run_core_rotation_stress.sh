#!/bin/bash
# ============================================================
# 轮流测试每个核心：当前核心运行 sdcshield，其余核心运行 stress-ng
# 测试后删除 PASS 日志，只保留 FAIL 日志
# ============================================================

OPENDCDIAG="./builddir/sdcshield"          # 可执行文件路径
STRESS_NG="stress-ng"                       # stress-ng 命令
TIMEOUT="60s"                               # 每个核心的测试时长（可调整）
TOTAL_CORES=128                             # 鲲鹏920 核心总数
LOG_DIR="./core_rotation_logs"              # 日志存放目录

mkdir -p "$LOG_DIR"

echo "========== 开始轮转核心测试 =========="
echo "总核心数: $TOTAL_CORES"
echo "每个核心测试时间: $TIMEOUT"
echo "日志目录: $LOG_DIR"
echo "开始时间: $(date)"
echo ""

for ((core=83; core<TOTAL_CORES; core++)); do
    LOG_FILE="$LOG_DIR/core_${core}.log"
    echo -n "核心 $core 测试中 ... "

    # 构造除当前核心外的 CPU 列表（用于 stress-ng）
    if [ $core -eq 0 ]; then
        other_cores="1-127"
    elif [ $core -eq $((TOTAL_CORES - 1)) ]; then
        other_cores="0-126"
    else
        other_cores="0-$((core-1)),$((core+1))-127"
    fi

    # 启动 sdcshield 在当前核心上（后台）
    taskset -c $core $OPENDCDIAG -T $TIMEOUT --strict-runtime -o "$LOG_FILE" &
    PID_OPENDCDIAG=$!

    # 启动 stress-ng 在其余核心上（后台），线程数 = 核心数-1
    taskset -c "$other_cores" $STRESS_NG --cpu $((TOTAL_CORES - 1)) --timeout $TIMEOUT --metrics-brief &
    PID_STRESS=$!

    # 等待两个任务完成
    wait $PID_OPENDCDIAG; TEST_RC=$?
    wait $PID_STRESS; STRESS_RC=$?

    # 完整判定：进程退出码非 0 / fail/crash / 零 result / pass 之外的软失败结果均不通过
    # （fail 块内部含独立 "seed:" 行，只统计 "result:" 行，避免误计）
    RESULT_CNT=$(grep -Eci "result: *(pass|fail|crash|skip|timed out|interrupted|operating system error)" "$LOG_FILE")
    PASS_CNT=$(grep -c "result: pass" "$LOG_FILE")
    if grep -Eqi "result: *(fail|crash)" "$LOG_FILE"; then
        echo -e "\033[31m❌ FAIL\033[0m (日志已保留)"
    elif [ $TEST_RC -ne 0 ] || [ $STRESS_RC -ne 0 ]; then
        echo -e "\033[31m❌ FAIL（sdcshield rc=$TEST_RC, stress-ng rc=$STRESS_RC）——日志保留: $LOG_FILE\033[0m"
    elif [ "$RESULT_CNT" -eq 0 ] || [ "$PASS_CNT" -ne "$RESULT_CNT" ]; then
        # 零 result 行（崩溃/被杀/零执行）或存在 pass 之外的软失败结果——一律保留
        echo -e "\033[31m⚠️ 结果不完整或异常（pass=$PASS_CNT/结果数=$RESULT_CNT）——日志保留: $LOG_FILE\033[0m"
    else
        echo -e "\033[32m✅ PASS\033[0m (日志已删除)"
        rm -f "$LOG_FILE"   # 删除确认通过的日志
    fi
done

echo ""
echo "========== 所有核心测试完成 =========="
echo "结束时间: $(date)"
echo "FAIL 日志保存在: $LOG_DIR"
