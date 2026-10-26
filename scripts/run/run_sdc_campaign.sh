#!/bin/bash
# ============================================================
# SDC 压测战役执行器 —— 24h+ 持续测试（文献激发规律驱动，新单板可复用）
#
# 依据：docs/paper/SDC_RESEARCH_SYNTHESIS_CN.md §7.4 战役协议
#      docs/superpowers/plans/2026-09-23-sdc-campaign.md（方案与参数依据）
# 阶段：
#   P1 全量广域扫   --quality=$QUALITY 全测试 × 全核 × fracturing（seed 自动轮换）
#   P2 加权 soak    GEMM 尺寸/形态谱（×三调度）→ crypto → 压缩 level 谱 →
#                   SLEEF 足迹谱 → FFT 因子谱 → mesh 一致性 → 混合 × RNG 引擎
#   P3 固定 seed 驻留 --max-test-loop-count=0（关 fracturing，CORE179 单模式持续暴露）
#   P4 持续循环×N   { 广域扫重跑(--test-list-randomize，执行上下文多样性) +
#                    拓扑/并发档（全核/各 NUMA node/半 node：不同电流拉载）+
#                    轮换驻留 }
# 全程环境监测     ipmitool 传感器 + EDAC + SEL 计数 → monitor.csv（30s 采样）
# 失败处理         fail-continue：fail → 全核复跑 → -n 1 复跑 → 分类
#                  known_benign_ulp / full_core_only / sdc_suspect；
#                  可复现 suspect 自动逐核二分（CORE179 式定位）
# 断点续跑         每阶段 .done 标记；重启自动跳过已完成阶段
# 用法：
#   bash scripts/run/run_sdc_campaign.sh                     # 24h+ 正式战役
#   SMOKE=1 bash scripts/run/run_sdc_campaign.sh             # 冒烟（~10min）
#   bash scripts/run/run_sdc_campaign.sh --selftest-classify # 解析器自测
# 退出码：0 全绿 / 1 有失败 / 5 有 sdc_suspect / 2 磁盘守卫
# ============================================================
set -u

# ---------------- 参数（env 可覆盖） ----------------
SDC="${SDC:-./builddir/sdcshield}"
CAMPAIGN_DIR="${CAMPAIGN_DIR:-$(pwd)/campaign_$(date +%Y%m%d_%H%M%S)}"
SWEEP_TIME="${SWEEP_TIME:-20s}"        # P1/P4 每测试时长（SEVI: >80% 首错<10s → 2x 余量）
QUALITY="${QUALITY:-0}"                # 0 = BETA+PROD（纳入审计 D7 的 BETA 单元测试）
# 注意 -t 是每测试语义：多测试档位的时长 = 测试数 × 下面各值
GEMM_SIZE_T="${GEMM_SIZE_T:-7m}"       # 2a OpenBLAS 尺寸谱（4 测试 × 4 尺寸 ≈ 112min）
GEMM_OTHER_T="${GEMM_OTHER_T:-2m}"     # 2a2 Eigen/ACL GEMM（8 测试 ≈ 16min）
SOAK_TIME_S="${SOAK_TIME_S:-4m}"       # 2b 形态谱/2d igzip/2e sleef/2f fft（单测试档）
CRYPTO_T="${CRYPTO_T:-5m}"             # 2c crypto（6 测试 ≈ 30min）
COMPRESS_T="${COMPRESS_T:-3m}"         # 2d 压缩族（8 测试 ≈ 24min）
MESH_T="${MESH_T:-2m}"                 # 2g mesh asymm_distrib 聚焦子集（≈14min）
MIXED_T="${MIXED_T:-2m}"               # 2g 混合 ×3 引擎（6 测试 × 3 ≈ 36min）
DWELL_TIME="${DWELL_TIME:-30m}"        # P3 每负载驻留（4 负载 ≈ 2h）
CYCLE_DWELL_TIME="${CYCLE_DWELL_TIME:-1h}"
TOPO_TIME="${TOPO_TIME:-2m}"           # P4 拓扑档每测试（5 测试 × 4 档 ≈ 40min）
CYCLES="${CYCLES:-4}"                  # 总时长 ≈ 1+1.9+4.5+2+4×3.8 ≈ 24.6h
MONITOR_INTERVAL="${MONITOR_INTERVAL:-30}"
RERUN_TIME="${RERUN_TIME:-60s}"
BISECT_TIME="${BISECT_TIME:-30s}"
PHASES="${PHASES:-p1,p2,p3,p4,summary}"
TEST_FILTER="${TEST_FILTER:-}"         # 逗号分隔，支持通配（冒烟/定向用）
SMOKE="${SMOKE:-0}"
SKIP_MONITOR="${SKIP_MONITOR:-0}"
MIN_DISK_KB=$((5*1024*1024))           # 磁盘守卫阈值 5G
# 2026-10-24 修复(feat/eigen-svd-multithread): eigen_svd_double/eigen_sparse 改用
# per-thread 同源 golden, 跨线程 Eigen SIMD 路径分歧的满核假阳消除, 白名单清空——
# 满核挂 = SDC 嫌疑, 与其它测试同等进入定位流程(README 结果判读口径一致)。
# 旧预构建二进制(2026-09-12 构建, 修复前行为)如需旧口径可
# KNOWN_FLAKY="eigen_svd_double eigen_sparse" 环境变量恢复。
KNOWN_FLAKY="${KNOWN_FLAKY:-}"
FAKE_PASS_RE='^(vmx_|isal_crc|crc32|fma$)'    # 审计 D1/D6/D11: 装饰性 pass（无检出意义）

if [ "$SMOKE" = 1 ]; then
    SWEEP_TIME=3s; GEMM_SIZE_T=3s; GEMM_OTHER_T=3s; SOAK_TIME_S=3s
    CRYPTO_T=3s; COMPRESS_T=3s; MESH_T=3s; MIXED_T=3s
    DWELL_TIME=5s; CYCLE_DWELL_TIME=5s; TOPO_TIME=5s; CYCLES=1; MONITOR_INTERVAL=5
    TEST_FILTER="${TEST_FILTER:-zstd19,openssl_sha,openblas_dgemm,sleef_neon,isal_igzip,pocketfft_fft,crc32,mesh_*}"
fi

# ---------------- 小工具 ----------------
NCORES=$(nproc)
tsec() { local v="${1%?}" u="${1: -1}"; case "$u" in
    s) echo $((v));; m) echo $((v*60));; h) echo $((v*3600));;
    *) echo $(( ${1:-0} / 1000 ));; esac; }
banner() { echo; echo "============================================================"; \
    echo "$(date '+%F %T')  $*"; echo "============================================================"; }

expand() { local out="" seg a b; IFS=',' read -ra segs <<< "$1"
    for seg in "${segs[@]}"; do
        if [[ "$seg" == *-* ]]; then a=${seg%-*}; b=${seg#*-}; out+="${out:+,}$(seq -s, "$a" "$b")"
        else out+="${out:+,}$seg"; fi
    done; printf '%s' "$out"; }

half_of() { [[ "$1" =~ ^[0-9]+-[0-9]+$ ]] || return 1
    local a=${1%-*} b=${1#*-}; echo "$a-$(( (a+b)/2 ))"; }

# ---------------- 目录与日志 ----------------
mkdir -p "$CAMPAIGN_DIR"
LOG="$CAMPAIGN_DIR/logs"; RERUN_DIR="$CAMPAIGN_DIR/rerun"; BISECT_DIR="$CAMPAIGN_DIR/bisect"
mkdir -p "$LOG" "$RERUN_DIR" "$BISECT_DIR"
FAILS_LOG="$CAMPAIGN_DIR/fails.log"; CMDLOG="$CAMPAIGN_DIR/commands.log"
STDOUT_LOG="$CAMPAIGN_DIR/sdcshield_stdout.log"
: > "$CMDLOG"; : > "$FAILS_LOG"; : > "$STDOUT_LOG"
: > "$CAMPAIGN_DIR/classifications.txt"
RERUN_SEQ=0

# ---------------- YAML 结果解析 ----------------
parse_results() { # parse_results <yaml> → 每行 "<testid> <result>"
    awk '
        /^[[:space:]]*(-[[:space:]]*)?(id|test|test_id):/ {
            line=$0; sub(/^[[:space:]]*(-[[:space:]]*)?(id|test|test_id):[[:space:]]*/,"",line)
            sub(/[[:space:]]*$/,"",line); tid=line }
        /^[[:space:]]*result:/ { if (tid!="") { print tid, $2; tid="" } }
    ' "$1"
}
has_fail() { parse_results "$1" | awk '$2=="fail"{f=1} END{exit !f}'; }
record() { echo "$(date '+%F %T') [CLASS] $1 → $2：$3" >> "$FAILS_LOG"
           echo "$1 $2" >> "$CAMPAIGN_DIR/classifications.txt"; }

# ---------------- 运行与失败分类 ----------------
ncount() { local s="$1"; [ -n "$s" ] || { echo 0; return; }; awk -F',' '{print NF}' <<< "$s"; }
run_sdc() { # run_sdc <yaml> <time> <ntests> [args...]  返回 sdcshield 退出码
    local yaml="$1" t="$2" n="$3"; shift 3
    # -t 为每测试语义：一次调用的真实时长 ≈ ntests × t + 启动/收尾开销（+600s 兜 mesh 失败后 ~10min 挂起形态）
    local slack=$(( $(tsec "$t") * n + n * 3 + 600 ))
    echo "$(date '+%F %T') [cmd] $SDC $* -t $t -o $yaml (ntests=$n, slack=${slack}s)" >> "$CMDLOG"
    timeout -k 60 "$slack" "$SDC" "$@" -t "$t" -o "$yaml" >> "$STDOUT_LOG" 2>&1
    local rc=$?
    [ $rc -ne 0 ] && echo "$(date '+%F %T') [exit=$rc] $*" >> "$FAILS_LOG"
    return $rc
}
go() { # go <yaml> <time> <ntests> [args...] = run + 分类 + 磁盘守卫
    local yaml="$1" t="$2" n="$3"; shift 3
    run_sdc "$yaml" "$t" "$n" "$@"
    classify_run "$yaml" "$(basename "$yaml" .yaml)"
    disk_guard
}
classify_run() { local yaml="$1" label="$2" tid res
    while read -r tid res; do
        [ "$res" = "fail" ] && classify_fail "$tid" "$label"
    done < <(parse_results "$yaml" | sort -u)
}
classify_fail() { # classify_fail <test> <label>
    local t="$1" label="$2" y y1
    RERUN_SEQ=$((RERUN_SEQ+1))
    y="$RERUN_DIR/r${RERUN_SEQ}_${t}_all.yaml"
    echo "$(date '+%F %T') [FAIL] $t（来自 $label）" >> "$FAILS_LOG"
    run_sdc "$y" "$RERUN_TIME" 1 -e "$t" -n "$NCORES" --ignore-unknown-tests --on-crash=context -vv
    if ! has_fail "$y"; then
        record "$t" transient "全核复跑 pass（单次失败；SEVI 长尾数据点，继续观察后续 cycle）"; return; fi
    y1="$RERUN_DIR/r${RERUN_SEQ}_${t}_n1.yaml"
    run_sdc "$y1" "$RERUN_TIME" 1 -e "$t" -n 1 --ignore-unknown-tests --on-crash=context -vv
    if ! has_fail "$y1"; then
        case " $KNOWN_FLAKY " in *" $t "*)
            record "$t" known_benign_ulp "CLAUDE.md 已知全核 ULP 抖动，-n 1 复现通过";;
        *) record "$t" full_core_only "仅全核失败、单线程通过（并发路径相关，持续观察）";; esac
        return; fi
    record "$t" sdc_suspect "全核与单线程均复现失败 → 启动逐核二分"
    bisect_per_core "$t"
}
bisect_per_core() { # CORE179 式逐核定位
    local t="$1" c y bad=""
    echo "$(date '+%F %T') [BISECT] $t 开始（${NCORES} 核 × ${BISECT_TIME}）" >> "$FAILS_LOG"
    for c in $(seq 0 $((NCORES-1))); do
        y="$BISECT_DIR/${t}_core${c}.yaml"
        run_sdc "$y" "$BISECT_TIME" 1 -e "$t" --cpuset="$c" --ignore-unknown-tests
        has_fail "$y" && bad="$bad $c"
    done
    echo "$(date '+%F %T') [BISECT] $t 失败核:${bad:-无}" >> "$FAILS_LOG"
    [ -n "$bad" ] && echo "$t 失败核:$bad" >> "$CAMPAIGN_DIR/suspect_cores.txt"
}
disk_guard() { local kb; kb=$(df -P "$CAMPAIGN_DIR" | awk 'NR==2{print $4}')
    if [ "${kb:-0}" -lt "$MIN_DISK_KB" ]; then
        echo "$(date '+%F %T') [GUARD] 磁盘剩余 ${kb}KB < ${MIN_DISK_KB}KB，战役中止" >> "$FAILS_LOG"
        monitor_stop; exit 2; fi
}

# ---------------- 环境监测 ----------------
MONITOR_PIDFILE="$CAMPAIGN_DIR/.monitor_running"
monitor_start() {
    [ "$SKIP_MONITOR" = 1 ] && return 0
    touch "$MONITOR_PIDFILE"
    MONITOR_CSV="$CAMPAIGN_DIR/monitor.csv"
    echo "timestamp;CPU1CoreRem;CPU2CoreRem;CPU1MEMTemp;CPU2MEMTemp;OutletTemp;InletTemp;Power;CPUPower;MEMPower;FAN2;FAN3;VDDAVS1;VDDAVS2;SEL_Entries;EDAC_CE;EDAC_UE;acpitz" > "$MONITOR_CSV"
    (
      while [ -e "$MONITOR_PIDFILE" ]; do
        ts=$(date '+%F %T'); row=""
        if command -v ipmitool >/dev/null 2>&1 && timeout 8 ipmitool sel info >/dev/null 2>&1; then
            s=$(timeout 8 ipmitool sensor list 2>/dev/null)
            g() { echo "$s" | awk -F'|' -v pat="^$1" '$1 ~ pat {gsub(/ /,"",$2); print $2; exit}'; }
            sel=$(timeout 8 ipmitool sel info 2>/dev/null | awk '/^Entries/{print $3}')
            row="$(g 'CPU1 Core Rem');$(g 'CPU2 Core Rem');$(g 'CPU1 MEM Temp');$(g 'CPU2 MEM Temp');$(g 'Outlet Temp');$(g 'Inlet Temp');$(g '^Power ');$(g 'CPU Power');$(g 'MEM Power');$(g 'FAN2 Speed');$(g 'FAN3 Speed');$(g 'CPU1 VDDAVS');$(g 'CPU2 VDDAVS');${sel:-na}"
        else
            row="na;na;na;na;na;na;na;na;na;na;na;na;na;na"
        fi
        ce=$(cat /sys/devices/system/edac/mc/mc0/ce_count 2>/dev/null || echo na)
        ue=$(cat /sys/devices/system/edac/mc/mc0/ue_count 2>/dev/null || echo na)
        tz=$(cat /sys/class/hwmon/hwmon*/temp*_input 2>/dev/null | paste -sd/ -)
        echo "$ts;$row;$ce;$ue;${tz:-na}" >> "$MONITOR_CSV"
        sleep "$MONITOR_INTERVAL"
      done
    ) &
    echo $! > "$CAMPAIGN_DIR/.monitor_pid"
}
monitor_stop() { rm -f "$MONITOR_PIDFILE"
    if [ -f "$CAMPAIGN_DIR/.monitor_pid" ]; then
        kill "$(cat "$CAMPAIGN_DIR/.monitor_pid")" 2>/dev/null
        rm -f "$CAMPAIGN_DIR/.monitor_pid"
    fi
    return 0; }

# ---------------- 测试集动态发现 ----------------
discover() {
    # --list-test-ids 每行为 "id shortid" 两列 → 只取第一列
    ALL_TESTS="$("$SDC" --list-test-ids 2>/dev/null | awk 'NF{print $1}')"
    [ -n "$ALL_TESTS" ] || ALL_TESTS="$("$SDC" --list-tests 2>/dev/null | awk 'NF{print $1}')"
    [ -n "$ALL_TESTS" ] || { echo "FATAL: 无法从 $SDC 获取测试清单" >&2; exit 1; }
    tids() { echo "$ALL_TESTS" | grep -E "$1" | paste -sd, -; }
    GEMM_OBLAS=$(tids '^openblas_(d|s|z|c)gemm$')            # 调度样本1: OpenBLAS TSV110
    GEMM_OTHER=$(tids '^(acl_gemm|eigen_gemm_[a-z0-9_]+)$')  # 样本2/3: Eigen NEON ×7 + ACL NEGEMM
    CRYPTO=$(tids '^openssl_(sha|sha3|sm3sm4)$')
    IPSEC_SAMPLE=$(echo "$ALL_TESTS" | grep -E '^ipsec' | head -3 | paste -sd, -)
    COMPRESS=$(tids '^(zstd[0-9a-z]*|zlib[a-z0-9_]*)$')
    MESH=$(tids '^mesh_')
    MESH_FOCUS=$(tids '^mesh_.*asymm_distrib')   # 聚焦子集：冒烟实证的失败族 + 全谱已在广域扫覆盖
    SLEEF=$(tids '^sleef_')
    # 引擎名保留原大小写（-s help 输出: Constant/LCG/AES；默认引擎为 LCG）
    RNG_ENGINES="${RNG_ENGINES:-$( { "$SDC" -s help 2>&1 || true; } \
        | grep -oE 'Constant|LCG|AES' | awk '!s[$0]++' | tr '\n' ' ')}"
    N_ALL=$(echo "$ALL_TESTS" | wc -l)
    echo "[discover] 测试总数 $N_ALL"
    echo "[discover] GEMM(OpenBLAS)=${GEMM_OBLAS:-无} GEMM(其他)=${GEMM_OTHER:-无} CRYPTO=$CRYPTO"
    echo "[discover] COMPRESS=${COMPRESS:-无} MESH=$(echo "$ALL_TESTS" | grep -c '^mesh_')个(聚焦$(echo "$ALL_TESTS" | grep -c 'asymm_distrib')) SLEEF=${SLEEF:-无} RNG=${RNG_ENGINES:-无}"
}

# ---------------- 拓扑档（新板自适应） ----------------
NODE_LISTS=()
for n in /sys/devices/system/node/node[0-9]*; do
    [ -d "$n" ] && NODE_LISTS+=("$(cat "$n/cpulist")")
done
TOPO_SETS=("")                                             # 档0: 全核（不传 --cpuset）
for nl in "${NODE_LISTS[@]}"; do TOPO_SETS+=("$(expand "$nl")"); done
if [ ${#NODE_LISTS[@]} -ge 1 ]; then
    h=$(half_of "${NODE_LISTS[0]}") && TOPO_SETS+=("$(expand "$h")")
fi

# ---------------- -e 过滤（冒烟/定向） ----------------
ENABLE_ARGS=()
if [ -n "$TEST_FILTER" ]; then
    IFS=',' read -ra _tf <<< "$TEST_FILTER"
    for t in "${_tf[@]}"; do ENABLE_ARGS+=(-e "$t"); done
fi

# ---------------- 各阶段 ----------------
phase_p1() { [ -f "$CAMPAIGN_DIR/.done_p1" ] && return 0
    banner "P1 全量广域扫（--quality=$QUALITY 全核 -t $SWEEP_TIME×${N_ALL}测试，fracturing=seed 自动轮换）"
    go "$LOG/p1_sweep.yaml" "$SWEEP_TIME" "$N_ALL" --quality="$QUALITY" \
       --on-crash=context --ignore-os-errors --ignore-timeout \
       --ignore-unknown-tests ${ENABLE_ARGS[@]+"${ENABLE_ARGS[@]}"}
    touch "$CAMPAIGN_DIR/.done_p1"
}
phase_p2() { [ -f "$CAMPAIGN_DIR/.done_p2" ] && return 0
    banner "P2 文献优先级加权 soak（-t 为每测试语义，各档时长=测试数×档时长）"
    local m tb L e n eng
    for m in 64 256 512 1024; do   # 2a 尺寸谱（L1→LLC/DRAM 足迹，Biswas 驻留）
        [ -n "$GEMM_OBLAS" ] && go "$LOG/p2_gemm_m${m}.yaml" "$GEMM_SIZE_T" "$(ncount "$GEMM_OBLAS")" -e "$GEMM_OBLAS" \
            -O openblas_dgemm.mdim=$m -O openblas_sgemm.mdim=$m \
            -O openblas_zgemm.mdim=$m -O openblas_cgemm.mdim=$m --ignore-unknown-tests
    done
    [ -n "$GEMM_OTHER" ] && go "$LOG/p2_gemm_others.yaml" "$GEMM_OTHER_T" "$(ncount "$GEMM_OTHER")" -e "$GEMM_OTHER" --ignore-unknown-tests
    for tb in 0 1 2 3; do          # 2b 形态谱（转置×β 相位）
        go "$LOG/p2_gemm_tb${tb}.yaml" "$SOAK_TIME_S" 1 -e openblas_dgemm \
            -O openblas_dgemm.transab=$tb -O openblas_dgemm.beta_permille=500 --ignore-unknown-tests
    done
    go "$LOG/p2_crypto.yaml" "$CRYPTO_T" "$(ncount "${CRYPTO}${IPSEC_SAMPLE:+,$IPSEC_SAMPLE}")" -e "${CRYPTO}${IPSEC_SAMPLE:+,$IPSEC_SAMPLE}" --ignore-unknown-tests
    for L in 0 1 2 3; do           # 2d 压缩 level 谱（四套 match-finder 数据结构）
        go "$LOG/p2_igzip_L${L}.yaml" "$SOAK_TIME_S" 1 -e isal_igzip -O isal_igzip.level=$L --ignore-unknown-tests
    done
    [ -n "$COMPRESS" ] && go "$LOG/p2_compress.yaml" "$COMPRESS_T" "$(ncount "$COMPRESS")" -e "$COMPRESS" --ignore-unknown-tests
    for e in 1024 16384 262144; do # 2e SLEEF 足迹谱
        go "$LOG/p2_sleef_e${e}.yaml" "$SOAK_TIME_S" 1 -e sleef_neon -O sleef_neon.nelems=$e --ignore-unknown-tests
    done
    [ -n "$SLEEF" ] && go "$LOG/p2_sleef_all.yaml" "$SOAK_TIME_S" "$(ncount "$SLEEF")" -e "$SLEEF" --ignore-unknown-tests
    for n in 4096 4099 6144 10000; do # 2f FFT 因子谱（pow2/质数/混合 radix）
        go "$LOG/p2_fft_n${n}.yaml" "$SOAK_TIME_S" 1 -e pocketfft_fft -O pocketfft_fft.n=$n --ignore-unknown-tests
    done
    # 2g 一致性聚焦：asymm_distrib 子集（冒烟实证失败族；全 mesh 家族已在广域扫覆盖）
    [ -n "$MESH_FOCUS" ] && go "$LOG/p2_mesh.yaml" "$MESH_T" "$(ncount "$MESH_FOCUS")" -e "$MESH_FOCUS" --ignore-unknown-tests
    for eng in $RNG_ENGINES; do    # 2g 混合多样性轮 × RNG 引擎（MeRLiN 故障等价类）
        go "$LOG/p2_mixed_${eng}.yaml" "$MIXED_T" 6 \
            -e openblas_dgemm,sleef_neon,pocketfft_fft,isal_igzip,openssl_sha3,zstd19 \
            -s "${eng}:$(( (RANDOM<<15) ^ RANDOM ^ $$ ))" --ignore-unknown-tests
    done
    touch "$CAMPAIGN_DIR/.done_p2"
}
phase_p3() { [ -f "$CAMPAIGN_DIR/.done_p3" ] && return 0
    banner "P3 固定 seed 深驻留（--max-test-loop-count=0 关 fracturing，每负载 $DWELL_TIME）"
    local L i=0
    for L in openblas_dgemm sleef_neon isal_igzip pocketfft_fft; do
        i=$((i+1))
        go "$LOG/p3_dwell_${i}_${L}.yaml" "$DWELL_TIME" 1 -e "$L" \
           --max-test-loop-count=0 --on-crash=context -vv --ignore-unknown-tests
    done
    touch "$CAMPAIGN_DIR/.done_p3"
}
phase_p4() { local c
    local loads=(openblas_dgemm sleef_neon isal_igzip pocketfft_fft openssl_sha3 zstd19)
    for c in $(seq 1 "$CYCLES"); do
        [ -f "$CAMPAIGN_DIR/.done_p4c${c}" ] && continue
        banner "P4 cycle $c/$CYCLES：随机序广域扫 + 拓扑/并发档 + 轮换驻留"
        go "$LOG/p4c${c}_sweep.yaml" "$SWEEP_TIME" "$N_ALL" --quality="$QUALITY" --test-list-randomize \
           --on-crash=context --ignore-os-errors --ignore-timeout \
           --ignore-unknown-tests ${ENABLE_ARGS[@]+"${ENABLE_ARGS[@]}"}
        local i=0 t cs
        for t in "${TOPO_SETS[@]}"; do   # 不同并发=不同电流拉载（无 cpufreq 板的 V/F 代理）
            i=$((i+1)); cs=""; [ -n "$t" ] && cs="--cpuset=$t"
            go "$LOG/p4c${c}_topo${i}.yaml" "$TOPO_TIME" 5 \
               -e openblas_dgemm,sleef_neon,isal_igzip,openssl_sha3,zstd19 $cs --ignore-unknown-tests
        done
        local L=${loads[$(( (c-1) % ${#loads[@]} ))]}
        go "$LOG/p4c${c}_dwell_${L}.yaml" "$CYCLE_DWELL_TIME" 1 -e "$L" \
           --max-test-loop-count=0 --on-crash=context -vv --ignore-unknown-tests
        touch "$CAMPAIGN_DIR/.done_p4c${c}"
    done
}
phase_summary() {
    banner "战役汇总"
    local y p f s selcol sel_start sel_end
    {
        echo "# 战役汇总（$(date '+%F %T')）"
        echo
        echo "## 各运行结果"
        echo "| 运行 | pass | fail | skip |"
        echo "|---|---|---|---|"
        for y in "$LOG"/*.yaml; do
            [ -e "$y" ] || continue
            p=$(parse_results "$y" | awk '$2=="pass"' | wc -l)
            f=$(parse_results "$y" | awk '$2=="fail"' | wc -l)
            s=$(parse_results "$y" | awk '$2=="skip"' | wc -l)
            echo "| $(basename "$y") | $p | $f | $s |"
        done
        echo
        echo "## 失败分类（明细见 fails.log）"
        if [ -s "$CAMPAIGN_DIR/classifications.txt" ]; then
            awk '{print $2}' "$CAMPAIGN_DIR/classifications.txt" | sort | uniq -c
            echo; cat "$CAMPAIGN_DIR/classifications.txt"
            if [ -f "$CAMPAIGN_DIR/suspect_cores.txt" ]; then
                echo "## 失败核定位"; cat "$CAMPAIGN_DIR/suspect_cores.txt"
            fi
        else echo "（无失败）"; fi
        echo
        echo "## 假通过（装饰性 pass，审计 D1/D6/D11，无检出意义）"
        echo "$ALL_TESTS" | grep -E "$FAKE_PASS_RE" | paste -sd, -
        echo
        echo "## 环境监测统计（monitor.csv）"
        if [ -s "$CAMPAIGN_DIR/monitor.csv" ]; then
            awk -F';' 'NR>1{n++
                for(i=2;i<=NF;i++){ v=$i+0
                    if(i==15||i==NF) continue          # SEL 计数与 acpitz 合成列不进 min/max
                    if(v>mx[i])mx[i]=v; if(mn[i]==""||v<mn[i])mn[i]=v }
                if(NR==2){for(i=2;i<=NF;i++) if(i!=15&&i!=NF) mn[i]=$i+0}}
                END{printf "样本数=%d\n",n
                    for(i=2;i<NF;i++) if(i!=15) printf "col%d min=%s max=%s\n",i,mn[i],mx[i]}' "$CAMPAIGN_DIR/monitor.csv"
            selcol=$(head -1 "$CAMPAIGN_DIR/monitor.csv" | tr ';' '\n' | grep -n '^SEL_Entries$' | cut -d: -f1)
            sel_start=$(sed -n '2p' "$CAMPAIGN_DIR/monitor.csv" | cut -d';' -f"$selcol")
            sel_end=$(tail -1 "$CAMPAIGN_DIR/monitor.csv" | cut -d';' -f"$selcol")
            echo "SEL 条目: $sel_start → $sel_end（增量 $(( ${sel_end:-0} - ${sel_start:-0} ))）"
        else echo "（监测未启用）"; fi
    } > "$CAMPAIGN_DIR/campaign_summary.md"
    cat "$CAMPAIGN_DIR/campaign_summary.md"
}

# ---------------- 解析器自测 ----------------
selftest_classify() {
    local T; T=$(mktemp -d); local rc=0
    printf -- '- id: ta\n  result: pass\n- id: tb\n  result: fail\n- id: tc\n  result: skip\n' > "$T/mix.yaml"
    printf -- 'results:\n- test: td\n  result: fail\n- test: te\n  result: pass\n' > "$T/mix2.yaml"
    echo "[selftest] mix.yaml →（期望 ta pass / tb fail / tc skip）"; parse_results "$T/mix.yaml"
    [ "$(parse_results "$T/mix.yaml" | wc -l)" = 3 ] || rc=1
    echo "[selftest] mix2.yaml →（期望 td fail / te pass）"; parse_results "$T/mix2.yaml"
    [ "$(parse_results "$T/mix2.yaml" | wc -l)" = 2 ] || rc=1
    if has_fail "$T/mix.yaml"; then echo "[selftest] has_fail(mix)=YES ✓"; else echo "[selftest] has_fail(mix)=NO ✗"; rc=1; fi
    if has_fail "$T/mix2.yaml"; then echo "[selftest] has_fail(mix2)=YES ✓"; else echo "[selftest] has_fail(mix2)=NO ✗"; rc=1; fi
    printf -- '- id: tf\n  result: pass\n' > "$T/ok.yaml"
    if has_fail "$T/ok.yaml"; then echo "[selftest] has_fail(ok)=YES ✗"; rc=1; else echo "[selftest] has_fail(ok)=NO ✓"; fi
    rm -rf "$T"
    [ $rc = 0 ] && echo "[selftest] ALL PASS" || echo "[selftest] FAILED"
    exit $rc
}
[ "${1:-}" = "--selftest-classify" ] && selftest_classify

# ---------------- main ----------------
[ -x "$SDC" ] || { echo "FATAL: $SDC 不存在（先完成构建）" >&2; exit 1; }
banner "SDC 压测战役启动：$CAMPAIGN_DIR（cores=$NCORES cycles=$CYCLES）"
discover
monitor_start
trap 'monitor_stop' EXIT
for ph in ${PHASES//,/ }; do
    case "$ph" in
        p1) phase_p1;; p2) phase_p2;; p3) phase_p3;; p4) phase_p4;;
        summary) phase_summary;; *) echo "未知阶段 $ph" >&2;;
    esac
done
monitor_stop; trap - EXIT
banner "战役结束（总时长见 commands.log 首尾时间戳）"
grep -q 'sdc_suspect' "$CAMPAIGN_DIR/classifications.txt" 2>/dev/null && exit 5
[ -s "$CAMPAIGN_DIR/classifications.txt" ] && exit 1
exit 0
