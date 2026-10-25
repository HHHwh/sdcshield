# SVE/SME 全 VL 二期 P5（sleef/eigen×8）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 spike 证据分流：**sleef_sve + sve_rot×5 真迁移**（TU/库双 VLA 就绪、三态探针零告警）到真 VLA 库；**eigen×2（gemm/svd）自 pin 回退**（Eigen packet 定长、VLA 语言级不可编译——两次 spike 独立确证）。

**Architecture:** gem5-fi sleef dual kernel（链 vendored SLEEF 静态库，same-kernel golden + 编排层跨档 checksum 比对 + 128 回归锚）四档 16-run → sdcshield 迁移（meson 库移动 + 注释重写）+ eigen×2 自 pin 接入 → 文档收尾。**16-run 全绿是 Task 2/3 合并的前置硬门，不是可后补的记录项**。as-built 收口注记（2026-10-25 二轮检视回写，偏差详录见文末「As-built 收口记录」）：实现 2026-10-24 先行入库 → sleef 16-run 同日全绿（gem5-fi feat/sleef-vla-dual @ 6d740525；merge blocker 登记后由 11cdd2c5 解除）→ sve_rot 家族 16-run 2026-10-25 全绿（feat/sve-rot-vla-dual @ faae123f）——**两矩阵均全绿于合并前，硬门实际成立**；中间态（matrix 曾 pending、家族 kernel 曾暂定 compile-probe-only，progress-log 2026-10-24）已被上述收口取代。as-built kernel 规格强于 Task 1 原文（12 函数矩阵、NELEMS=1028、双 noinline golden 抗 CSE、保真闭环——按原文规格实现反而会造出检测链更弱的 kernel）。

**Spec:** `docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md` §6 P5（"据证据定深度"）+ §11.2（sleef VL=256 编译回归根因 spike，裁定实验污染）——sleef 真迁移的规范锚点；Eigen 回退 = §11.1 (c)（Eigen 第三方墙三选一的撞墙回退路径，spike 两次独立确证）。**sleef 不适用 §11.1**——三选一全部针对 Eigen 第三方墙（(a) patch vendored Eigen 与 sleef 无关）。

**Spike 证据基线**（2026-10-24 双 spike，报告全文见执行会话账本与 gem5-fi 侧记录）：
- sleef：TU 零 128-ism 构造（sizeless + svcnt 步进 + whilelt 尾）；vendored SLEEF 3.9.0 纯 VLA（反汇编实证 cntd/cntp/rdvl、无 ptrue_pat）；三态编译（128/256/无 flag）全零告警；历史 256 回归最可能系实验污染（H1：整体翻 flag 连 Eigen 定长 TU 重编的堆损坏被归因 sleef）。
- Eigen：`arm_sve_vector_bits(EIGEN_ARM64_SVE_VL)` 定长 + PacketXcd struct 成员/NumTraits sizeof 语言级死结；七类编译期消费点；自 pin 回退有完整基础设施（arm64_sve_pin_vl + require）。

## Global Constraints

- 分支：`feat/sleef-eigen-allvl` 堆叠 `feat/ipsec-allvl`；gem5-fi `feat/sleef-dual` 堆叠 `feat/sha-dual`。
- gem5 sleef kernel 依赖 vendored SLEEF 静态库（主 checkout `third-party/sleef/install/lib/libsleef.a`，gitignored——kernel Makefile 用绝对路径引用 + 注释声明依赖，不把二进制入仓）。**基线钉版**：Task 1 首轮运行前记录 `sha256sum third-party/sleef/install/lib/libsleef.a`，随 16-run 实录一并写入 gem5-fi findings 表与 progress-log P5 条目——.a 是 gitignored 可再生产物（build.sh 幂等短路一旦被打破重编即静默换血），无哈希钉版则 128 回归锚与跨档比对全部悬空（spike H1 "实验污染" 正是同类环境漂移前科）。
- **Task 1 前置（二轮检视补）**：先 `test -f third-party/sleef/install/lib/libsleef.a`——缺失即运行 `./third-party/sleef/build.sh` 重建或立即停止（fresh clone 无此 gitignored 产物，kernel 链接即失败，16-run 无从建立证据）。**as-built 钉版登记（如实）**：首轮运行前 sha256 **未**按上条执行记录（计划步骤未兑现，偏差登记见文末）；事后补记 `1a56c914ce1c4ebb3b52f55685de5335dd35e8af051f4448f78445eb85d04a28`——档位未变由文件 mtime 2026-09-15（早于 16-run 一个多月，未被重建）与 gem5-fi findings 入库前反汇编复验计数（13 cntd / 21 cntp / 6 rdvl / 86 ptrue，零 pattern 立即数 ptrue）双重背书；该哈希自此为前向钉版基线。
- 单位军规 / 验证 subagent 隔离 / 一文件一 commit / DCO / x86 零改动，同前各阶段。

## Review Focus

1. **sleef 跨档逐位一致**：sin/cos/exp/log 逐 lane 纯函数 ⇒ 四档 golden 逐字节一致——判据核心在**编排层**（比对四档同 mode+seed 的 checksum；kernel 每次 gem5 run 只见单档，进程内拿不到其余三档的 golden 缓冲。若不一致即历史回归真身，单变量定位）。
2. **128 回归锚**：原判据「迁移后 128 档 checksum 与迁移前（128 编译产物）一致」**as-built 修正（二轮检视）**——该字面锚无基线可执行：迁移前定宽产物从未在 gem5 运行过（gem5-fi 全史 sleef kernel 仅 6d740525 一个新增），原计划两侧（基线侧运行、比对侧重跑）均无执行步骤，属无主判据。实际执行的锚定 = **三重保真闭环**（gem5-fi findings @ 6d740525）：① 跨档逐位一致——128 档 checksum 与 256/512/1024 档相同（四档同值，128 档即锚点值）；② gem5 向量 vs 标量 0.00 ulp 标定（12 家族 × 1028 lane × 四档，worst 0.00 ulp）；③ **真硬件标量锚**——同一 libsleef.a 标量函数在宿主 Kunpeng 920 按同 fill/同 FNV 序重算，4 输入集 checksum 与 gem5 向量路径逐位一致。③ 严格强于原字面锚：闭合的是 gem5==真硬件，而非新代码==旧代码（后者两侧共享同一模拟器，共模不可检）。
3. **Eigen 自 pin 语义**：test_run 每 worker 线程首行 pin(16) + 返回值核对 + run 侧 fail-closed 漂移守卫（`arm64_sve_vl_bytes()`，非 svcntb()，依据见 Task 3）；pin 失败（内核限制）诚实 SKIP 文案。
4. **meson 库移动的依赖链**：sleef_dep 跟随新库；tests_set_sve 剩余成员（eigen×2）不破坏。
5. **sve_rot×5 同批**：探针已证 VLA 三态全净——迁移低风险项，但四档实证仍按家族 kernel 16-run 走（Task 2），"探针全净"不替代运行验证。

---

### Task 1: gem5-fi `sleef_vla_dual.c` + 16 run

- [x] kernel：链 `libsleef.a`（sha256 基线钉版见 Global Constraints）；四函数组（Sleef_sindx_u10sve/cosdx/expdx/logdx + u35 二档）；输入 nelems=1024；**same-kernel golden**（init 期同函数算）+ 逐字节比对 + FNV。kernel 输出 per-run 字段 `mode=sleef_vla lanes=N seed=S checksum=<fnv-16hex> SDCSHIELD_FAILS=<n>`——**每次 gem5 run 只见单档，判据 1（四档 golden 逐位一致）由编排层完成**：run 循环外比对四档同 mode+seed 的 checksum；kernel 内不做跨档断言、不输出 cross-tier-match 类字段（单档进程拿不到其余三档的 golden 缓冲，按字面实现只能伪造比对或空断言）。Makefile 目标注明 libsleef.a 绝对路径依赖。**退出码契约（二轮检视补钉）**：kernel 必须 `return g_fails ? 1 : 0`（dual-kernel 家族惯例，stencil_axis_dual.c:147；as-built sleef_vla_dual.c:383 实装）——且 gem5 SE **不传播 guest 退出码**（exit(7) 实证：gem5 进程退 0，findings 诚实边界条目），故编排层对 FAILS 的门禁只能解析 `SDCSHIELD_FAILS=` 输出行（下方脚本的门禁 1），rc 检查对 kernel FAILS 结构性失明。as-built 规格强于本条：12 函数矩阵（double u10 ×4 + float u10 ×4 + double u35 ×4，u35 无 exp、exp2 为独立链替补）；NELEMS=1028（≡4 mod 32，使 256/512/1024 档尾真正截断）；golden_pass/compute_out 双 noinline 规避 SLEEF_CONST 单函数内 CSE（反汇编实证两函数各含 12 个 SLEEF bl）；另加有限性哨兵 + 阴性对照（golden 1-ulp 毒化→FAILS=1；输入 1-ulp 毒化→FAILS=4 + checksum 变化）与保真闭环（见 Review Focus 2）。
- [x] 16 run（四档 × {fixed 1, random 1/2/3}）隔离代理；**四档同 seed**——每个 random case 的 xs32 seed 钉定为 {1,2,3}，四档复用同一输入序列，seed 随输出行记录（失败可重放）；命令形态沿 P2 计划闭环版（`tee` 留全档日志、`rc=${PIPESTATUS[0]}` 取 gem5 真实退出码而非管道末 grep 的、overall 汇总）——**初版漏带了 P2 闭环版的 fails_ok 程序化门禁，二轮检视补齐并加两条**（门禁 1 FAILS 行——gem5 SE 不传播 guest 退出码，rc 对 kernel FAILS 失明；门禁 2 记录在场——缺日志/缺记录不得经 `wc -l` 的退出状态静默通过；门禁 3 random 互异——防恒定输出 kernel 的假一致）：

```bash
G5=/home/sdc/wangxu/operator/gem5-fi/build/ARM/gem5.opt
overall=0
for T in 1 2 4 8; do
  for ARGS in "fixed 1" "random 1" "random 2" "random 3"; do
    LOG=/tmp/sleef_sve${T}_$(echo $ARGS | tr ' ' '_').log
    timeout 600 $G5 configs/se/arm_chaos.py --cpu O3 \
      --cmd workloads/sdcshield/sleef_vla_dual --sve_vl $T \
      --workload_args "$ARGS" 2>&1 | tee "$LOG" \
      | grep -E 'sleef_vla_dual|SDCSHIELD_FAILS|checksum'
    rc=${PIPESTATUS[0]}      # timeout/gem5 的真实退出码,不是 grep 的
    echo "tier=$T args='$ARGS' exit=$rc"
    [ "$rc" -ne 0 ] && overall=1
    # 门禁 1(FAILS 行): gem5 SE 不传播 guest 退出码, kernel FAILS 只能由此行判定
    grep -q '^SDCSHIELD_FAILS=0$' "$LOG" || { overall=1; echo "tier=$T args='$ARGS' FAILS-gate: 行缺失或非 0"; }
    # 门禁 2(记录在场): 每 run 必须产出一条 checksum 记录
    grep -q 'checksum=[0-9a-f]' "$LOG" || { overall=1; echo "tier=$T args='$ARGS' checksum-gate: 记录缺失"; }
  done
done
# 判据 1 = 编排层跨档比对(kernel 内不做——单档进程只见单档):
for A in fixed_1 random_1 random_2 random_3; do
  for T in 1 2 4 8; do
    [ -s /tmp/sleef_sve${T}_${A}.log ] || { overall=1; echo "log-gate: /tmp/sleef_sve${T}_${A}.log 缺失或为空"; }
  done
  n=$(grep -h -o 'checksum=[0-9a-f]*' /tmp/sleef_sve{1,2,4,8}_${A}.log | sort -u | wc -l)
  echo "$A unique-checksums=$n"        # 期望 1: 四档同 seed 逐位一致
  [ "$n" -ne 1 ] && overall=1
done
# 判据 3 = random 互异(防恒定输出 kernel 假一致): 三个 random case 的唯一 checksum 恰为 3
nu=$(grep -h -o 'checksum=[0-9a-f]*' /tmp/sleef_sve{1,2,4,8}_random_{1,2,3}.log | sort -u | wc -l)
echo "random-unique-checksums=$nu"     # 期望 3
[ "$nu" -ne 3 ] && overall=1
exit $overall
```

判据（全部程序化，无悬挂文本判据）：每 run `SDCSHIELD_FAILS=0`（门禁 1）且 gem5 exit=0 且 checksum 记录在场（门禁 2）；四档同 mode+seed 的 checksum 逐位一致（判据 1，编排层比对 + 日志在场门禁）；random 各 seed checksum 互异（判据 3，门禁 3）；128 档为回归锚基准（锚定形态见 Review Focus 2 的 as-built 修正）。**Task 1 全绿是 Task 2/3 合并的前置硬门。** as-built 对账（6d740525，16/16）：gem5 进程退出码全 0、`SDCSHIELD_FAILS=0` 全 16 run；fixed_1 四档同值 `0e7d323699e93b79`，random_1/2/3 四档各同值 `d57c395eaedf525e` / `93386fc81537269b` / `b44f6cd4c1e83633`（互异成立）——三条门禁所断言的事实已在执行记录中逐项成立。本轮补强的脚本是**未来重跑**的口径：修脚本不追认旧证据、亦不必为补门禁重跑 16 run（权威证据 = kernel stdout 的 FAILS/checksum 列，P2 先例 Erratum 3 同款）；/tmp 日志易失，持久记录 = gem5-fi findings.md 16-run 表。

### Task 2: sdcshield sleef_sve + sve_rot×5 迁移（每文件一 commit）

- [x] `tests/cpu/meson.build`：`sleef/sleef_sve.cpp` 移出 tests_set_sve → 新真 VLA 静态库 `tests_sve_vla`，参数清单：`-march=armv8.2-a+sve`、**无 `-msve-vector-bits=128`**（真 sizeless VLA——sizeless 类型采纳运行时 VL 是本库存在的全部意义；"移出 sourceset"本身不保证参数不继承，须显式不带）、sleef_dep、Eigen 宏**不带**、`sve_sanitize_exempt`（`-fno-sanitize=all`——GCC 12.3 SVE intrinsics + sanitizer 插桩 codegen ICE 纪律，tests/cpu/meson.build:42-51，sanitizer 夜巡拦截面内）；sve_rot×5 同批移入；tests_set_sve 仅剩 eigen×2（注释更新）。构建后核对 compile_commands.json：新库每个 TU 的编译命令无任何固定 VL flag（防参数继承回归）。as-built 核对基准：meson 源参数清单穷举（新库 c_args/cpp_args 全集 = tests_common_* + `-march=armv8.2-a+sve`（cpp 另加 sve_sanitize_exempt），tests_common_* 为全测试共享不可能含 SVE 定宽 flag）+ 全库 grep 仅 tests_set_sve（eigen×2）残留 `-msve-vector-bits=128`（meson:1020，既有设计）；compile_commands 逐 TU 复核在合并前树重建后可复跑（本机 builddir 现反映迁移前树，登记）。
- [x] sleef_sve.cpp 注释重写（128-lock 段落 → VLA 语义 + 四档实证引用）；门控换 `arm64_require_sve_vl(test, 128, 1024)`（现行 HWCAP 手探 → 框架层）+ run 侧漂移守卫。范围依据：(128, 1024) = **gem5 验证阶梯上界**（128/256/512/1024 四档实证口径），2048-bit 未在本战役验证、不宣称（框架 selftest 的 (128, 2048) 是探针语义，非功能覆盖宣称）。as-built：门控 :325 + vl_bytes 锚 :331 + `svcntb() != vl_bytes` drift guard → log_error :414-416；docblock 已引 16-run 收口（"matrix pending" 表述由 11cdd2c5 消除）。
- [x] sve_rot×5：五段式逐文件（P3/P4 批量代理同款，#286 范本 a96fff38〔rebase 前 8bb2904c〕）：① init 首行 `arm64_require_sve_vl(test, 128, 1024)` 门控；② `vl_bytes` 改取 require 返回值（单一来源，替代 svcntb() 直读）；③ run 侧漂移守卫 `svcntb() != vl_bytes` → log_error + report_fail_msg（真 VLA 库无 -msve-vector-bits，svcntb() 不被折叠——与 eigen×2 的 rdvl 守卫差异各有依据）；④ docblock 128-lock NOTE → 迁移完成表述 + 三态编译探针 spike 证据（兑现其 P5 迁移预排注释）；⑤ meson 移动（上一条）。as-built 五 commit：a96fff38 / c3ac2eeb / 40dec21e / 0416929f / 0c94a06a。
- [x] **sve_rot×5 家族 gem5 验证**：增补家族 dual kernel + 16 run（P3 聚类 scoping 先例：同 kernel 形态 = 同验证目标——rot ALU 链 + ldr/str asm 全向量访存是五文件共享的 kernel 形态，一个家族 kernel 四档实证覆盖全族；spec §12.1"受影响用例四档 16 run"以此落实）。as-built 收口（2026-10-25，gem5-fi feat/sve-rot-vla-dual @ faae123f，#286 检视 round 1 补齐）：`sve_rot_vla_dual`（镜像 k7pattern 默认几何 1024 slots 全链：四 cell fill → init 形 golden（asm z-ldr + rot_alu i%4）→ 升/降双 sweep → memcmp；通道 B FNV-1a；通道 C tag 唯一性）16 run 全绿——四档 × 四 cell 全部 SDCSHIELD_FAILS=0；fixed 32 位拆分编码四档 tag dups=0；legacy 阴性对照 1024-bit 档 dups=8192（= 512 slots × 16 lanes，k7pattern 混叠数学在真实执行中逐位实证）、512-bit 档 65528 < 65536 与原头注 "fits up to 512b inclusive" 边界精确吻合；zero/one/hiham 十二 run 全绿（不经 tag，纯回归佐证）。中间态（progress-log 2026-10-24 曾裁定维持 compile-probe-only、登记为后续可选工作项）已被本收口取代——"全 VL 达标"宣称的必要条件已补齐，无未验证边界残留。
- [x] 每文件构建零告警 + skip + zstd19（as-built：#286 分支全量构建零新警告；本机无 SVE 全部干净 SKIP（CpuNotSupported, 'test compiled with sve'）；`zstd19 -t 2000 -n 1` → exit: pass——#286 检视实录）。

### Task 3: eigen×2 自 pin 接入（每文件一 commit）

- [x] `gemm_double_max_arm.cpp` / `svd_cdouble_sve.cpp` 四要点（与 #286 实现对齐：fd3e258e / 3b82b533〔rebase 前 0e7b69a4 / 1ec37655〕；as-built 代码位点——require 门控 :157/:189、`arm64_sve_pin_vl(16)` + 返回值核对 :172/:226、`arm64_sve_vl_bytes() != 16` fail-closed 守卫 :188 起）：
    - **init 门控与返回值契约**：`arm64_require_sve_vl(test, 128, 1024)` 返回 `<0 = EXIT_SKIP(-255) 且 helper 内已 log_skip——非裸 -1`（arm64_vl.h 头注：框架对非 EXIT_SKIP 的负返回值按 strerror(-ret) 记 "Unexpected OS error"，裸 -1 会在真实 skip 理由上叠加误导性 "Operation not permitted"）——`if (svl < 0) return EXIT_SKIP;` 直接返回，**不得**继续走 pin（否则准确的范围 SKIP 理由被 "cannot pin..." 覆盖、或产生重复 skip 记录）。范围 (128, 1024) 依据：pin 只需向下调（任意更高 VL → 128），上限 = gem5 验证阶梯口径（128..1024 四档实证）；放宽 2048（框架 selftest 探针口径）为可选项，非本阶段必需。
    - **pin 位置 = test_run 每 worker 线程首行，不是 test_init**：arm64_vl.h 硬边界——test_init 跑在一次性 aux 线程（init_in_aux_thread），其 prctl 到不了 per-CPU worker（worker 由控制线程创建、继承控制线程的 VL；内核只向 pin 线程自建的线程传导 pin）；init 里 pin 是静默无效 pin，失配 VL 裸跑的堆损坏前科 = validate4b（SVE packet 尾溢出 → free()/munmap_chunk aborts）。`arm64_sve_pin_vl(16)`（编译档 128）返回值 ≠16 → 诚实 log_skip（"cannot pin to the compile-time VL (kernel restriction?)"）+ EXIT_SKIP。
    - **run 侧 fail-closed 漂移守卫用 `arm64_sve_vl_bytes() != 16`（.inst 编码 rdvl），不用 `svcntb()`**：-msve-vector-bits=128 -O2 下 GCC 把 `svcntb() != 16` 常量折叠为 `mov w0, 0`——守卫静默蒸发且零告警（实证）；rdvl 读真实 per-thread 运行时 VL。守卫触发 → log_error + report_fail_msg（fail，非 skip）。
    - docblock 记 spike 裁定与语言级死结引用。**消灭对已丢失的外部 /tmp/vlrun launcher 的依赖**。
    - **gem5 验证诚实边界**（P0-P1 计划 :759 先例——"gem5 SE 对 prctl 的支持是诚实边界，不做判定"）：gem5 SE 将 prctl 映射 -ENOSYS（ignoreFunc）→ gem5 四档跑 eigen×2 实际行使 pin 失败的诚实 SKIP 路径（真实输出：log_skip "cannot pin..." + skip）；pin 成功路径（prctl 成功 + rdvl=16 + 守卫通过）gem5 内不可达——以 objdump 机器码核对（run 包装器内 prctl 调用 + 返回值核对 + 真实 rdvl 守卫三段在位，fd3e258e/3b82b533 实测）为等效验证，真机 SVE 行为如实记为未验证边界，不宣称 gem5 已验 pin 成功。
- [x] 每文件构建零告警 + skip + zstd19（as-built 同 Task 2 末条口径）。

### Task 4: 文档收尾 + PR

- [x] 文档同步（spec §10 六处表口径）：meson 注释、README（sleef 行 + Eigen SVD 段 launcher 表述）、**CLAUDE.md**（Known platform quirks 两处被本计划推翻的陈述："eigen SVD test needs a VL-pinning launcher（`/tmp/vlrun`）"与 "`sleef_sve` stays at 128 by design"）、**docs/architecture.md**（Eigen 5.0 SVE packets 节 runtime-VL 规则段：process→thread VL 语义、删 "sleef_sve compiles at 128 by design"）、docs/research/adding-tests.md（sourceset 表补 `tests_sve_vla` 行）、progress-log（P5 条目：双 spike 结论、分流裁定、16-run 实录、Eigen 死结档案引用）。与 #286 的实际同步范围（88d42767〔rebase 前 4991fc03〕：README/architecture.md/adding-tests.md/CLAUDE.md 四处 + progress-log 11cdd2c5/679c4610 P5 条目）对齐。PR 正文（含分流归属表：sleef 行锚点 = §6 P5 + §11.2、**不适用 §11.1**；eigen 行 = §11.1 (c) 撞墙回退被行使）。
- [x] **spec 自身回写（二轮检视补）**：spec §6 P5 交付物「spike 报告（spec 附录）」与 §8 golden 表 sleef/eigen×8 行「结论写入本 spec 附录」在原计划中无任何步骤兑现（Task 4 原清单止于六处同步，Self-Review 原文「Spec §6 P5 全覆盖」因此不实）——增补：spec 新增附录 B（双 spike 结论 + 分流裁定 + golden 策略结论 + 验证口径），§8 表行与 §6 P5 行同步指向。已由本 PR 增补 commit 兑现。

## Self-Review

- Spec §6 P5 全覆盖（sleef 迁移/回退分流——按 spike 证据而非原设想的统一路径；原"真 VLA 重构"对 sleef 成立对 Eigen 不可达）**含 spec 自身回写**（原清单遗漏，二轮检视补——附录 B，见 Task 4 增补项）。锚点归位：sleef 真迁移 = §6 P5 + §11.2（256 回归根因 spike 裁定），Eigen 回退 = §11.1 (c) 撞墙路径被行使；sleef 不适用 §11.1。Review Focus 归属：1→Task 1；2→Task 1（as-built 以三重保真闭环取代原字面 128 锚，见 Review Focus 2 修正）；3→Task 3；4→Task 2；5→Task 2（含家族 kernel 16-run，已收口 faae123f）。无占位符；kernel 的 libsleef.a 依赖是环境事实非 TBD（sha256 基线钉版见 Global Constraints——as-built 为事后补记，诚实登记）。正文实现 commit 引用一律 on-branch（rebase 后）SHA，rebase 前对象以〔〕注记。

## As-built 收口记录（2026-10-25，PR #285 二轮检视回写）

- **时间线与硬门成立性**：实现（sve_rot×5 + eigen×2 + meson 迁移 + 文档同步）2026-10-24 入库 → sleef 16-run 同日全绿（6d740525）→ 本计划终版 bf1700be（同日 21:30，晚于实现——本文件是事后追认的执行口径记录，复选框按 as-built 勾选）→ sve_rot 家族 16-run 2026-10-25 全绿（faae123f，#286 检视 round 1）。两矩阵均全绿于合并前，"矩阵补齐全绿前 P5 不得视为完成（spec §12.1）"的硬门实际成立（11cdd2c5 解除 merge blocker 后才具备合并条件）。
- **偏差登记（as-built 与计划原文的差异，如实）**：
  1. sha256 钉版未在首轮运行前执行——事后补记 `1a56c914…85d04a28`（mtime 2026-09-15 早于 16-run、未被重建 + findings 反汇编复验计数双重背书档位未变），见 Global Constraints。
  2. Review Focus 2 原字面锚（迁移前 128 编译产物 checksum 比对）无基线可执行——迁移前产物从未在 gem5 运行（gem5-fi 全史 sleef kernel 仅 6d740525 一个新增），以三重保真闭环取代（严格更强：闭合 gem5==真硬件，而非新代码==旧代码的模拟器共模比对），见 Review Focus 2。
  3. Task 1 kernel 规格按强于计划的形态实现（12 函数矩阵、NELEMS=1028 尾截断、双 noinline golden 抗 CSE、有限性哨兵、阴性对照、保真闭环）——按原文规格实现会造出检测链更弱的 kernel，见 Task 1。
  4. 16-run 门禁脚本初版漏带 P2 闭环版已有的 fails_ok 门禁、且缺日志在场与 random 互异两条——二轮补齐三条程序化门禁；已执行证据（6d740525）在记录列上逐项满足三条门禁所断言的事实，修脚本不追认旧证据、不必为补门禁重跑（权威证据 = kernel stdout FAILS/checksum 列，P2 先例 Erratum 3 同款）。
  5. 正文原引用 8bb2904c / 0e7b69a4 / 1ec37655 / 4991fc03 为 rebase 前对象（不在 #286 分支祖先链）——已改引 on-branch 等价 SHA（a96fff38 / fd3e258e / 3b82b533 / 88d42767）。
  6. Task 4 原六处同步清单遗漏 spec 自身回写（spec §6 P5 交付物「spike 报告（spec 附录）」）——二轮检视补，已由本 PR 增补 commit 兑现（附录 B）。
- **G5 路径勘误**：Task 1 脚本原写 `/home/sdc/wangxu/code-review/gem5-fi/...`，本机实际为 `/home/sdc/wangxu/operator/gem5-fi/...`（gem5.opt 在位实证）——已改。
