#!/bin/bash
# ============================================================
# 逐个核心运行 15 个测试用例，每个用例运行指定时间（默认 20 秒）
# 用法： ./run_all_cores_fixed.sh [时间]  例如：./run_all_cores_fixed.sh 30s
# ============================================================

# 清理可能存在的默认日志文件（避免占用磁盘空间）
echo "清理旧日志文件..."
rm -f sdcshield-*.yaml

TEST_TIME="${1:-10s}"
NUM=$(echo "$TEST_TIME" | grep -o '^[0-9]*')
UNIT=$(echo "$TEST_TIME" | grep -o '[a-zA-Z]*$')
if [ -z "$NUM" ]; then NUM=1; UNIT="s"; fi
case "$UNIT" in
    s) TOTAL_SEC=$((NUM * 2)) ;;
    m) TOTAL_SEC=$((NUM * 2 * 60)) ;;
    h) TOTAL_SEC=$((NUM * 2 * 3600)) ;;
    *) TOTAL_SEC=$((NUM * 2)) ;;
esac
TOTAL_TIME="${TOTAL_SEC}s"

TOTAL_CORES=128
LOG_DIR="fail_all_core"
tests=(
    memcpy_shuffle_v2
    memcpy_shuffle
    isal_crc_iscsi
    isal_crc_ieee
    isal_crc64_jones_norm
    isal_crc64_iso_refl
    isal_crc64_iso_norm
    isal_crc64_ecma182_refl
    isal_crc64_ecma182_norm
    isal_crc32_gzip
    isal_crc_t10dif
    isal_crc64_jones_refl
    crc32_fixed_shuffled
    crc32_fixed
    crc32
)

mkdir -p "$LOG_DIR"
echo "开始时间: $(date)"

for ((core=3; core<TOTAL_CORES; core++)); do
    echo -n "核心 $core 测试中 ... "
    LOG_FILE="$LOG_DIR/core_${core}.log"
    > "$LOG_FILE"
    RC_ALL=0
    for test in "${tests[@]}"; do
        echo "  [$(date +%H:%M:%S)] 运行 $test" >> "$LOG_FILE"
        # 使用 timeout，强制将输出重定向到日志文件，并丢弃任何可能生成默认文件的途径
        timeout -k 5 "$TOTAL_SEC" ./builddir/sdcshield \
            --enable="$test" \
            -t "$TEST_TIME" \
            -T "$TOTAL_TIME" \
            --strict-runtime \
            --cpuset="$core" \
            -o "$LOG_FILE" >> "$LOG_FILE" 2>&1
        # 记录每条命令的真实退出码（非 0 = 被杀/超时/崩溃，禁止报通过）
        rc=$?
        # 仅保留首个非零退出码：后面的 rc=1 不覆盖前面的 rc=124（timeout），超时信息不丢失
        [ $rc -ne 0 ] && [ $RC_ALL -eq 0 ] && RC_ALL=$rc
        # 注意：-o 已指定，再次重定向确保所有输出都进入日志
    done
    # 完整判定：先核对 15 个预期用例是否都有 result 行，再看有无 fail/crash/软失败
    PASS_CNT=$(grep -c "result: pass" "$LOG_FILE")
    TOTAL_CNT=$(grep -Ec "result: *(pass|fail|crash|skip|timed out|interrupted|operating system error)" "$LOG_FILE")
    if grep -Eqi "result: *(fail|crash)" "$LOG_FILE"; then
        echo -e "\033[31m❌ FAIL\033[0m"
        # 保留失败日志，不删除
    elif [ $RC_ALL -ne 0 ]; then
        echo -e "\033[31m❌ FAIL（存在非零退出码 $RC_ALL）——日志保留: $LOG_FILE\033[0m"
    elif [ "$TOTAL_CNT" -lt ${#tests[@]} ] || [ "$PASS_CNT" -lt ${#tests[@]} ]; then
        # 预期 15 个用例未全部出结果或未全部 pass（未执行/被中断/零结果）——一律保留
        echo -e "\033[31m⚠️ 结果不完整（pass=$PASS_CNT/结果数=$TOTAL_CNT，预期 ${#tests[@]}）——日志保留: $LOG_FILE\033[0m"
    else
        echo -e "\033[32m✅ PASS\033[0m"
        rm -f "$LOG_FILE"
    fi
done

echo "结束时间: $(date)"
# 最后再次清理可能新生成的默认日志（如果有）
rm -f sdcshield-*.yaml
