#!/bin/bash
# ============================================================
# 依次对每对相邻核心进行测试：
#   当前核心对 (i, i+1) 运行 sdcshield
#   其余核心运行 stress-ng 复合压力（矩阵+内存+缓存）
# 测试后删除 PASS 日志，只保留 FAIL 日志
# ============================================================

# ---------- 可配置参数（宏） ----------
OPENDCDIAG="./builddir/sdcshield"          # sdcshield 可执行文件路径
STRESS_NG="stress-ng"                       # stress-ng 命令

# stress-ng 负载配置
STRESS_CMD_TEMPLATE='stress-ng --matrix $other_count --matrix-method prod --vm 2 --vm-bytes 80% --timeout $TIMEOUT --metrics-brief'

# 其他参数
TIMEOUT="180s"                               # 每个核心对的测试时长（可调整 s/m/h）
TOTAL_CORES=$(nproc)                        # 系统总核心数（自动检测）
LOG_DIR="./adjacent_cores_logs"             # 日志存放目录

# ---------- 脚本主体 ----------
mkdir -p "$LOG_DIR"

echo "========== 开始相邻核心对测试 =========="
echo "总核心数: $TOTAL_CORES"
echo "每个核心对测试时间: $TIMEOUT"
echo "日志目录: $LOG_DIR"
echo "stress-ng 命令模板: $STRESS_CMD_TEMPLATE"
echo "开始时间: $(date)"
echo ""

# 遍历所有相邻核心对 (0-1, 1-2, ..., N-2 - N-1)
for ((i=0; i<TOTAL_CORES-1; i++)); do
    core1=$i
    core2=$((i+1))
    LOG_FILE="$LOG_DIR/core_${core1}_${core2}.log"
    echo -n "核心对 $core1-$core2 测试中 ... "

    # 构造除当前核心对外的 CPU 列表
    other_cores_list=()
    for ((c=0; c<TOTAL_CORES; c++)); do
        if [ $c -ne $core1 ] && [ $c -ne $core2 ]; then
            other_cores_list+=($c)
        fi
    done
    other_cores=$(IFS=,; echo "${other_cores_list[*]}")
    other_count=${#other_cores_list[@]}

    # 启动 sdcshield（当前核心对）
    taskset -c $core1,$core2 $OPENDCDIAG -T $TIMEOUT --strict-runtime -o "$LOG_FILE" &
    PID_OPENDCDIAG=$!

    # 启动 stress-ng 复合压力（其他核心），使用 eval 解析模板中的变量
    eval "taskset -c \"$other_cores\" $STRESS_CMD_TEMPLATE" &
    PID_STRESS=$!

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
        rm -f "$LOG_FILE"
    fi
done

echo ""
echo "========== 所有相邻核心对测试完成 =========="
echo "结束时间: $(date)"
echo "FAIL 日志保存在: $LOG_DIR"
