#!/bin/bash
# sdc_detect.sh — SDC 一键检测（SDC 敏感用例优先执行）
#
# 生成"SDC 敏感度优先序"的测试列表（框架 --test-list-file：顺序即执行序），
# 全核轮转直到检出一个 FAIL（-F 即停）或 Ctrl-C 干净停止。完整多样性保留
# （全部用例都跑，只是顺序优先）——多样性=检出率（CPU179 实证单测试单跑零复现）。
#
# 优先级分桶（首个匹配的桶获胜，未匹配 → P7 字母序兜底）：
#   P1 向量FMA/矩阵        SEVI(ASPLOS'26)：>92% SDC 事故由 FMA 指令贡献，vector≫scalar
#   P2 ARM64 SDC 专项      本仓库为已知鲲鹏故障签名（CPU179/cn23154）定向编写
#   P3 memcpy/store→reload CPU179 实证 store→reload 是触发判别条件
#   P4 位扩散(压缩/哈希/CRC) From Gates to SDCs(DATE'25)：sha 类负载位掩蔽最少
#   P5 标量浮点/大整数      高汉明距离操作数压满加法器
#   P6 一致性/原子/SIMD     文献共识：控制流/一致性故障多 crash 化，SDC 倾向低
#   P7 其余                 x86 遗留探针/IST 占位/虚拟化类
#
# 用法:
#   bash scripts/run/sdc_detect.sh                 # 一键检测（默认，长驻留）
#   bash scripts/run/sdc_detect.sh --smoke         # 冒烟（约 35s：每桶前 2 用例各 2s 的单遍）
#   SDC_BIN=<路径> bash scripts/run/sdc_detect.sh  # 显式指定二进制
#   其余参数原样透传给 sdcshield（如 -t 30s、--cpuset 0-47）
set -euo pipefail

usage() {
    sed -n '3,21p' "$0" | sed 's/^# \{0,1\}//'
}

SMOKE=0
PASSTHROUGH=()
for a in "$@"; do
    case "$a" in
        --smoke) SMOKE=1 ;;
        -h|--help) usage; exit 0 ;;
        *) PASSTHROUGH+=("$a") ;;
    esac
done

# ---------------- 二进制发现（fail-loud）----------------
if [ -n "${SDC_BIN:-}" ]; then
    BIN="$SDC_BIN"
else
    BIN=""
    for c in ./builddir/sdcshield ./builddir-gcc/sdcshield; do
        [ -x "$c" ] && BIN="$c" && break
    done
fi
if [ -z "${BIN:-}" ] || [ ! -x "$BIN" ]; then
    echo "FATAL: 未找到可执行的 sdcshield 二进制（SDC_BIN=${SDC_BIN:-未设置}；" \
         "已尝试 ./builddir/sdcshield 与 ./builddir-gcc/sdcshield）。" >&2
    echo "       请先构建（README「从源码构建」）或 export SDC_BIN=<路径>。" >&2
    exit 1
fi

QUALITY=0          # PROD+BETA：不加则 arm64_sdc 等 6 个 BETA 用例被跳过（实测）
if [ "$SMOKE" = 1 ]; then
    T_PER_TEST=2s    # 单遍：每桶前 2 用例 × 2s ≈ 35s 自然结束（不传 -T）
else
    T_PER_TEST=60s  # SEVI：>80% 首错 <10s，60s 有 6 倍余量
    TOTAL_ARGS=(-T forever)   # 故障窗口稀疏（CPU179 占空比 ~0.06%）→ 长驻留
fi
STAMP=$(date +%m%d-%H%M%S)
OUT="sdc-detect-${STAMP}.yaml"
LISTFILE="sdc-detect-testlist-${STAMP}.txt"
SMOKEFILE="sdc-detect-testlist-${STAMP}-smoke.txt"   # --smoke 短名单（每桶前 2 + P7 前 2）

# ---------------- 生成优先序测试列表 ----------------
"$BIN" --quality="$QUALITY" --list-tests > "${LISTFILE}.all" 2>/dev/null \
    || { echo "FATAL: --list-tests 执行失败（二进制异常）" >&2; exit 1; }
TOTAL_N=$(wc -l < "${LISTFILE}.all")
[ "$TOTAL_N" -gt 0 ] || { echo "FATAL: --list-tests 输出为空" >&2; exit 1; }

P1='^(openblas_(d|s|z|c)gemm|openblas_lu|eigen_gemm|eigen_svd|eigen_sparse|sleef_neon|sleef_sve|pocketfft_fft|acl_gemm|fma|fmatail|sve512_f|arm64_sdc|neon_add)'
P2='^(sve512_|power_virus_dit|ooo_dep_chain|lsu_store_forward|l2c_cross_cache_line|mmu_split_tlb|fsu_byteexact|iex_operand_combo|operand_space|ifu_branch_target|fisttp|libomp_ctx|sme_za)'
P3='^(memcpy|mem_|random_access_sweep|mmu_stress_arm|movbe|agu_|neon_rot|mrn|sve_rot|partial_store_forwarding|part_store_fwd|vmovnt)'
P4='^(zstd|zlib|zfuzz|isal_|crc32|openssl_|ipsec_|arm_crypto|zpclmul)'
P5='^(fpu_special_values|adcx|adox|bigint_mulx|gmp_big)'
P6='^(cachebounce|cache_stress|mesh_upi|lock|lockless|atomic_|spinlock|swizzle|insert_extract|kreg|gather|arm0102_kreg)'
BUCKETS=("$P1" "$P2" "$P3" "$P4" "$P5" "$P6")
NAMES=("P1 向量FMA/矩阵        " "P2 ARM64 SDC 专项     " "P3 memcpy/store→reload"
       "P4 位扩散(压缩/哈希/CRC)" "P5 标量浮点/大整数    " "P6 一致性/原子/SIMD   ")

# 分桶：顺序过滤（首个匹配获胜）；原始名单与每轮中间结果用独立文件，
# 严禁原地覆写（grep 输入与输出同文件会在读取前被截断——redirect-to-input 竞态）
: > "$LISTFILE"
if [ "$SMOKE" = 1 ]; then : > "$SMOKEFILE"; fi
REST="${LISTFILE}.all"
SUM=0
for i in "${!BUCKETS[@]}"; do
    CNT=$(grep -cE "${BUCKETS[$i]}" "$REST" || true)
    grep -E "${BUCKETS[$i]}" "$REST" >> "$LISTFILE" || true
    if [ "$SMOKE" = 1 ]; then
        # 冒烟短名单：本桶前 2 个（顺序即 .all 中的注册序）
        grep -E "${BUCKETS[$i]}" "$REST" | head -2 >> "$SMOKEFILE" || true
    fi
    grep -vE "${BUCKETS[$i]}" "$REST" > "${LISTFILE}.rest.new" || true
    mv "${LISTFILE}.rest.new" "${LISTFILE}.rest"
    REST="${LISTFILE}.rest"
    echo "  ${NAMES[$i]}: ${CNT} 个用例"
    SUM=$((SUM + CNT))
done
if [ "$SMOKE" = 1 ]; then
    LC_ALL=C sort "$REST" | head -2 >> "$SMOKEFILE" || true
fi
LC_ALL=C sort "$REST" >> "$LISTFILE"
echo "  P7 其余（字母序）      : $((TOTAL_N - SUM)) 个用例"

# 硬件能力预过滤(2026-10-18 战役修复): 无 SVE/SME 的机器剔除编译期
# CpuNotSupported 用例——否则 PR#244 fail-loud 下 --fatal-skips 把这类
# skip 记失败 + -F 首错即停(night2 实捕: eigen_gemm_double_max_arm
# 'test compiled with sve' 停机, P2-P7 全未跑)。保留 --fatal-skips 对
# 剩余 skip 的严格性(D-007)。
if ! grep -qw sve /proc/cpuinfo 2>/dev/null; then
    BEFORE=$(wc -l < "$LISTFILE")
    grep -vE '^(sve|sleef_sve|sme)' "$LISTFILE" > "${LISTFILE}.hw" && mv "${LISTFILE}.hw" "$LISTFILE"
    [ "$SMOKE" = 1 ] && grep -vE '^(sve|sleef_sve|sme)' "$SMOKEFILE" > "${SMOKEFILE}.hw" && mv "${SMOKEFILE}.hw" "$SMOKEFILE"
    AFTER=$(wc -l < "$LISTFILE")
    echo "  硬件预过滤(无SVE/SME)  : 剔除 $((BEFORE - AFTER)) 个编译期skip用例(sve*/sleef_sve*/sme*)"
    TOTAL_N=$AFTER   # 一致性基准同步为过滤后数量
fi
rm -f "${LISTFILE}.all" "${LISTFILE}.rest"

LISTED=$(wc -l < "$LISTFILE")
[ "$LISTED" -eq "$TOTAL_N" ] \
    || { echo "FATAL: 分桶后数量不一致（$LISTED != $TOTAL_N）——分桶正则有重叠或遗漏" >&2; exit 1; }

# ---------------- 运行 ----------------
echo "sdcshield : $BIN"
if [ "$SMOKE" = 1 ]; then
    echo "冒烟名单  : $SMOKEFILE（$(wc -l < "$SMOKEFILE") 个：每桶前 2 + P7 前 2，单遍）"
    sed 's/^/    /' "$SMOKEFILE"
    echo "日志      : $OUT"
    echo "停止方式  : 单遍自然结束（约 35s）；Ctrl-C 干净停止；300s 兜底；检出 FAIL 即自动停（-F）；skip 记为失败（--fatal-skips）"
    # 兜底：300s 绝对上限防意外挂死（SIGINT 干净停止已实测验证）
    # --fatal-skips：冒烟名单含恒 skip/R1/R2b/R3 用例，skip 必须显式暴露而非静默通过（D-007）
    exec timeout -s INT 300 "$BIN" --quality="$QUALITY" --test-list-file "$SMOKEFILE" \
        -t "$T_PER_TEST" -Y -F --fatal-skips -o "$OUT" \
        ${PASSTHROUGH[@]+"${PASSTHROUGH[@]}"}
else
    echo "测试列表  : $LISTFILE（$TOTAL_N 个，优先序）"
    echo "日志      : $OUT"
    echo "停止方式  : Ctrl-C（干净停止，日志完整落盘）；检出 FAIL 即自动停（-F）；skip 记为失败（--fatal-skips）"
    exec "$BIN" --quality="$QUALITY" --test-list-file "$LISTFILE" \
        "${TOTAL_ARGS[@]}" -t "$T_PER_TEST" -Y -F --fatal-skips -o "$OUT" \
        ${PASSTHROUGH[@]+"${PASSTHROUGH[@]}"}
fi
