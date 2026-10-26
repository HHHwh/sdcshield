# all-VL Phase 2 收尾硬化单元（终审递延清单）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> （as-built 注记，PR #293 R2 检视 local-claude(L3)：本计划实际以 Task 标题段落执行完毕，未启用 checkbox 勾销——执行跟踪由 #294 提交链承载；保留模板原文以存出处，不做回填勾选。）

**Goal:** 清零 P6 终审递延清单：六类先存 VL>512（部分 VL>128）越界缺陷 + 3 文件缺漂移守卫 + fmopa assert NDEBUG 失效 + x8/x4 命名残留 + sve512_fma_arm 孤儿接线。

**Architecture:** 全部为缺陷修复/机械变换，不新增功能面。gem5 侧（as-built 修正）：本机 0xd01 无 SVE——SKIP 路径不构成功能验证（CLAUDE.md 规则 2），受影响测试以**同源 dual kernel 四档实证**收口（sve512_fma_dual / sve_arith_bounds_dual，#294 处置落地）；原计划「既有 kernel 家族证据覆盖」的表述被 PR #293 检视推翻（家族模式不含 gather/scatter 负载，也不含本组修复后的测试几何），按 PR #242 同源实证标准执行。movmskpspd 修复属行为收缩（谓词钳制到原语义宽度），代码论证之外 as-built 另补 gem5 四档真实执行（sve_arith_bounds_dual D 节）。

**Spec:** `docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md` §9/§12（SKIP 语义与验收）；递延清单出处 = P5+P6 终审报告 + P6 批 B 审计。

## Global Constraints

- 修复模式（**as-built 光谱**，PR #293 R2 勘误 local-claude(L13)：原写「统一」不实——同一战役三种加固深度并存）：①**16-lane 门内闭包扩容**（vsum[16]/chunk[128]/agu phase[16]，f7716fcf）——由 init 层 `arm64_require_sve_vl(test, 128, 1024)` 一次性门控背书（**13 文件基线 7e464454 即有**，P6 批 C 迁移，非本轮新增——模式①全部 12 文件（10 个 vsum + arm64_sdc_sve + agu_stress_2src）+ Task 5 fsu_byteexact；#294 head 97cd08a1 逐文件实证 init 均为 `const long svl = arm64_require_sve_vl(test, 128, 1024); if (svl < 0) return EXIT_SKIP;`：adcx_sve:125 等 10 个 vsum 文件、arm64_sdc_sve:110、fsu_byteexact_sve:135、agu_stress_2src_sve:77；main 上计数为 0 系基线落差而非缺失——PR #293 R3 检视 coderabbit(L14)「三测试只查 HWCAP_SVE、需补门控/改 SVE_MAX_VECTOR_BYTES」前提驳回：其所审 PR #293 树（main 基座,落后基线恰 79 commit）上三文件确为旧式 `getauxval(AT_HWCAP)` 手探,与 R2 L22 同型 main 树误读;其 VL=2048 算术本身成立（svcntb()=256 > chunk[128]、fsu cross 末偏移 56+256=312 > 256、svcntd()=32 > phase[16]）但该档被门控排除——门内 svcntb()≤128/svcntd()≤16 恰好闭包 chunk[128]（:85 注释明言）、fsu cross（56+128=184 ≤ 256,:150-153 注释明言）、agu phase;agu 另经 ee6a4a7d 升级为 `SVE_MAX_VECTOR_BYTES/8` + run 漂移 fail-closed 守卫,双保险见 Task 3）；②**`SVE_MAX_VECTOR_BYTES` 架构上限范式**（neon_rot phase 74c618d7、u8x16 基线即有、#288 movbe 全族）——任意 VL 无条件安全；③**run 侧 fresh-VL 复读**（sme_fmopa_stress `arm64_sme_svl_bytes()/4 != n`，policy #3）。①的诚实边界：init 后 run 前 VL 上漂超 1024-bit 不在其保护内（[16] 数组无 run 侧再校验，会静默溢出）——威胁模型升级时以②/③为准。谓词钳制保留原语宽度（movmskpspd）；注释引用门控不变式。
- 每缺陷一 commit（一单元一 patch）；构建零告警 + `-e <test>` skip + zstd19 回归；**行为修复另需 gem5-fi 受影响档位同源实证**（本机无 SVE，SKIP 之外唯一功能验证路径；as-built：sve512_fma_dual 4-run（四档各一）+ sve_arith_bounds_dual 16-run（有效维度 4 档 × 3 输入集 = 12 组）,全部 SDCSHIELD_FAILS=0——run 数与 poison 分母的统一口径矩阵见 As-built 收口记录「gem5 证据统一矩阵」（PR #293 R4 检视 coderabbit(L94) 勘误：原「4 档 × 3 seed」与「16-run」两种口径未加说明）。
- 分支：`feat/allvl2-hardening` 从 `feat/p6-allvl`（`7e464454`）切出。
- gem5 边界（as-built 修正）：原计划的「既有 kernel 家族（chain/gather/stencil）四档实证覆盖受影响模式」**不成立**——sve512_chain_dual 仅含 chain_f64/chain_f32/special_f64 三模式，不含受影响测试的 gather/scatter 负载与修复后几何（PR #293 检视 coderabbit/local-claude 同源指出；PR #242 先例为同源实证）。实际收口：新建同源 kernel **sve512_fma_dual**（sve512_fma_arm 四负载含 gather-scatter 往返，四档 SDCSHIELD_FAILS=0）与 **sve_arith_bounds_dual**（mul512 分块/operand_space 谓词/phase 派发/movmsk 钳制四节，16-run 全 0，poison 阴性对照 **FAILS=5**——四节 golden 各毒化一处、D 节 ps/pd 两形态各一，5/5 全检出；PR #293 R4 勘误 coderabbit(L94)：原写「FAILS=4/5」系把两 kernel 的 poison 值连写压缩、失实——sve512_fma_dual 的 poison 阴性对照为 FAILS=4（Task 10），两 kernel 分母与逐档矩阵见 As-built「gem5 证据统一矩阵」）。movmskpspd 修复使行为**更窄**（只读 FPS_LANES=4/FPD_LANES=2，与 x86 原版语义逐位一致）。诚实边界记录于 commit 正文。

## 任务

### Task 1: vsum[8]→[16] 家族（10 文件，一 commit）

adcx_sve/adox_sve/adcx_arm_sve/adox_arm_sve/adcxlong_sve/adcxlong_arm_sve/adcx_adox_interleaved_sve/adcx_adox_interleaved_arm_sve（各 :76-77 附近）+ gmp_bigadd_sve + operand_space_sve（基线 7e464454 均 :110；原写 :113/:114 系 main 行号——PR #293 R2 勘误 local-claude(L22)）共 **10 个文件**（检视勘误：原写 11 文件；`git grep -l "uint64_t vsum\[8\]"` 在基线 7e464454 与 main 上均恰 10 个命中，且 gmp_bigadd/operand_space 的变量名同为 `vsum`，原文「同型变量名按实际」的对冲表述不实——PR #293 检视 coderabbit #6 / local-claude #17）：`uint64_t vsum[8];` → `uint64_t vsum[16];   /* 1024-bit gate: svcntd() <= 16 */`。

**门控前提**（PR #293 R2 检视 coderabbit(L22) 处置）：10 文件 init 均已有 `const long svl = arm64_require_sve_vl(test, 128, 1024); if (svl < 0) return EXIT_SKIP;`（基线 7e464454 实测 10/10，f7716fcf 之前即存在，adcx_sve:125 等）——`/* 1024-bit gate */` 注释的指称对象即该 init 门控（门内 svcntd() ≤ 16）；框架层（arm64_vl.h）在 VL 超出 128..1024 时 log_skip（理由含实测 VL 值）并返回 EXIT_SKIP，run 不可达。意见前提「Task 1 的 10 个文件目前都没有调用 arm64_require_sve_vl」系在 main 树（PR head）上的误读——main 落后基线 79 commit、P6 批 C 未入 main（tests/ 门控计数 main=0 / 基线=174），与 R1 #15/#12 子项同型；其「VL=2048（svcntd()=32）全宽写越过数组末端」的算术本身成立，但该档位被 init 门控排除——[16] 在门内闭包安全，门后上漂的残留边界见 Global Constraints ①。

### Task 2: ai[8]/lo_vec[8]→[16]（2 文件，一 commit）

bigint_mulx_sve.cpp + gmp_bignum_sve.cpp（两文件为逐字节重复的同一函数 `sve_bigint_mul512`）。原处方（`ai[8]`/`lo_vec[8]` 扩 `[16]`，谓词形态「按实际 svld1 谓词形态定」）**不完整且留有未决占位**（PR #293 检视 local-claude #13）：谓词形态是写作时即可读出的既成事实 `svptrue_b64()`，且同函数 `svld1_u64(pg, b)` 对 8 元素操作数 b 的越界读（VL>512 时 OOB 64B）不在扩容处方内。**as-built 处方**（#294 处置 7685cb73，取代原文）：删除 svptrue，按 `svcntd()` 分块 + `svwhilelt_b64(j0, NUM_WORDS)` 谓词限定 + `svdup_n_u64` 广播——ai/lo_vec 中转数组整体消除，b 读越界与乘法器盲区（原 `(void)lo` 丢弃 SVE 低半积）一并修复；gem5-fi sve_arith_bounds_dual A 节 4 档 × 3 输入集（有效 12 组,口径见 As-built「gem5 证据统一矩阵」）全 0。

### Task 3: phase[8]→[16]（agu_stress_2src，一 commit）

:123 `phase[8]` → `[16]` + 注释（填充循环 k<n 已动态，n≤lanes≤16）。

**边界登记（PR #293 检视 local-claude #12，清单外开放缺陷）**：本处方不触及该 run 循环的尾批几何——`for (base=0; base<count; base+=lanes)` 的 count 来自 knob/auto **未取整**（对照 neon_rot_2src_sve init 的「向下取整到 svcntd() 整倍数」，agu 无此步），`load_vec_sve` 为 asm ldr 全 VL 读、`store_vec_sve` 为 `svwhilelt(0, svcntd())` 全 VL 写，`aligned_alloc_safe` 仅向上取整到 64B 对齐（pad < 64B）。count 为 8 的倍数时 VL≤512 档无尾批；**VL=1024（lanes=16）且 count%16≠0 时尾批堆越界**（默认 count=600：600%16=8、4800B 已 64B 对齐 pad=0 → 尾批越界 8 元素 64B；#294 head 7fe83a97 实证仍无取整）。修复属实施链自有文件（f7716fcf 触碰过），登记为 #294 下轮处置项；修复样板现成——同链 f7716fcf 已施于 neon_rot 兄弟文件的 init 向下取整 + run 整除守卫两件套可直接移植（PR #293 R2 检视 local-claude(L32)：「有模式在手仍留默认参数即可触发的堆写」成立，归属与时机仍为 #294 下轮，本 PR 仅计划文档）。**已修（as-built 更新，PR #293 R3）**：#294 round 3 **ee6a4a7d** 落地——尾批 slack（init 侧 srcA/srcB/expected 与 run 侧 temp/dst 均按 count+lanes 元素定容,松弛区不进 golden/memcmp）+ run 侧 init/run VL 漂移 fail-closed 守卫（struct 增 `lanes` 字段保存 init 期 svcntd(),失配 log_error + report_fail_msg）+ phase 升级 `SVE_MAX_VECTOR_BYTES/8`（97cd08a1 :154,「门控+漂移守卫双保险」,74c618d7 家族范式同款）;同 commit 另为 fma_f32 补漂移守卫。gem5 实证（PR #293 R4 补引）：**agu_2src_dual** 新 kernel（aad8f5b2,feat/pr294-r3-dual 分支）四档 × 3 seed 12-run 全绿——count 故意取 4*lanes+(lanes+1)/2（非 lanes 整倍数）,每档过尾批全宽 ldr z/str z;poison 阴性对照 FAILS=3（3 轮 × golden[1] 毒化 1 元素,冷路径 dump 镜像测试形态）。

### Task 4: chunk[64]→[128]（arm64_sdc_sve，一 commit）

:85 `chunk[64]` → `chunk[128];  /* 1024-bit gate: svcntb() <= 128 */`。

### Task 5: fsu_byteexact 跨行写扩容（一 commit）

:150 `vbuf = alloc(CACHE_LINE, CACHE_LINE * 2)` → `CACHE_LINE * 2 + 128`（`cross = vbuf + CACHE_LINE - 8` 起 svptrue 写 svcntb() 字节，1024-bit=128B，56+128=184 > 128 → 需 64+128+16 余量；读回同范围，`mask != svcntw()` 全宽比较保持）。

### Task 6: movmskpspd 谓词钳制（一 commit）

:123 `svwhilelt_b32(0, lanes < 32 ? lanes : 32)` → `svwhilelt_b32(0, 4)`（FPS_LANES=4——x86 MOVMSKPS 原语义 4-lane，out[32] 的 32 上限是为宽向量写的但源行只有 4 元素）；:141 同理 → `svwhilelt_b64(0, 2)`（FPD_LANES=2）。**紧邻的 `const int lanes = svcntw()/svcntd();` 两行必须随之删除**（PR #293 检视 local-claude #19：钳制后 lanes 零引用，-Wunused 告警撞自设零告警门）。掩码打包循环 `i<4`/`i<2` 不变——语义与原版逐位一致，且消除末行 OOB 读（VL>128 即触发的那个）。as-built（f7716fcf + 7fe83a97）：lanes 两行已删；原计划「`vals[32]`/`out[32]`/`out[16]` 保留（防御）」进一步收敛——`vals[32]` 死数组删除、out 容量对齐行宽 `out[FPS_LANES]`/`out[FPD_LANES]`（谓词与打包循环零变化），gem5 四档真实执行补证（sve_arith_bounds_dual D 节）。

### Task 7: neon_rot 3 文件补 run 漂移守卫（一 commit）

neon_rot_2src_sve / neon_rot_store_only_sve / neon_rot_u8x16_sve 补 run 漂移守卫。原处方 `svcntb() != data->vl_bytes` **引用了三个目标 struct 都不存在的字段**（NeonRot2srcSveData / NeonRotStoreOnlySveData = {srcA, srcB, expected, count}，NeonRotU8x16SveData = {srcA, srcB, expected, buffer_bytes——knob 缓冲尺寸,非 VL}），且「ldr_at_top 家族同款」指认有误——ldr_at_top 家族的守卫是 `svcntd() != data->lanes`（init 保存 `data->lanes = svcntd()`），`vl_bytes` 是 sleef/sve_rot 家族（`arm64_sve_vl_bytes()` 口径）的惯用法（PR #293 检视 coderabbit #8 / local-claude #14）。**as-built 处方**（f7716fcf，无需新增字段）：2src/store_only 用 init 取整不变式 `data->count % (size_t)svcntd() != 0 → log_error + report_fail_msg`（init 已把 count 向下取整到 svcntd() 整倍数,失配即漂移）;u8x16 用 `buffer_bytes % lcm(svcntb(), 64) != 0` 同型——run 守卫失配时 `log_error + report_fail_msg`（**fail-closed,与 2src/store_only 完全一致**;f7716fcf as-built :147-157 即如此——原文「失配时 log_warning」**失实**,log_warning 属 init 期 knob 取整事件（buffer_bytes 非公倍数时向下取整并告警,非漂移守卫、不终止任何路径）,PR #293 R3 检视 coderabbit(L53) 勘误;对齐基数原文 max 亦已过时——#294 round 2 **dc2dc471** 改 `std::lcm(svcntb(), 64)`,非 2 幂 VL（如 96B）下 max=96 非公倍数而真公倍数 192,原 max 基数在非 2 幂 VL 下 aligned_alloc 尺寸违约）。另注：漂移守卫防不住 phase[8] 的 VL=1024 栈越界（init/run 同档时守卫恒过）——neon_rot_2src 的 phase 修复由 #294 处置 74c618d7 承载（`phase[SVE_MAX_VECTOR_BYTES/8]`）。

**守卫检测边界**（PR #293 R2 检视 coderabbit(L48) 处置）：整除性保持的漂移（count=8 同被 svcntd()=4/8 整除、buffer_bytes=16384 同被 64/128B 整除）确实通过守卫——但那不是「错误的运行状态」：三测试 golden 均为逐元素 i%4 / j%4 标量循环（2src/store_only 逐 u64、u8x16 逐 byte），计算与 VL/分块宽度无关；整除成立时步长整除定尺寸缓冲（全 VL ldr/str 无尾批）、phase 定容 `SVE_MAX_VECTOR_BYTES` 覆盖架构最大 2048-bit、u8x16 存储谓词化且 ldr 尾读有 256B slack——内存安全与逐位正确性均成立，漂移前后结果逐位一致。守卫检验的是**危险条件**（步长失除 → 全 VL 尾批越过定尺寸缓冲）而非漂移事件本身，失除即 fail-closed；ldr_at_top 家族的 `svcntd() != data->lanes` 精确比较是该家族的惯用法（其 golden 与 init VL 耦合），对 neon_rot 的逐元素几何属过度约束（良性漂移会误报 SDC）——按计算几何选型，不采用。

### Task 8: fmopa_stress assert → release 生效检查（一 commit）

:243 `assert(n >= 4 && n <= kTileNMax);` → 显式 `if (!(n >= 4 && n <= kTileNMax)) { log_error(...); report_fail_msg(...); }`（注释更新：NDEBUG 下 assert 蒸发——P2 已实证该类；release 生效检查替代）。

### Task 9: x8/x4 命名 → xN（机械改名，一 commit）

sve_sha_kernels.h 3 函数（sha256_compress_x8→sha256_compress_xn、sha512_compress_x4→sha512_compress_xn、sha1_compress_x8→sha1_compress_xn）+ sve_hmac.h 5 函数（hmac_sha256_x8→hmac_sha256_xn 等）+ 18 个调用文件 sed。**改名安全约束（PR #293 检视 local-claude #16）**：`hmac_sha{1,224,256}_x8` 是 `hmac_sha*_x86_64_*` 的**字面前缀**——按名字 grep 选文件会带入 tests/cpu/ipsec/3des_docsis/ 下 3 个 x86 文件，未锚定的 sed 一致重命名后构建不红、静默违反 x86-64 untouched 铁律；必须排除 x86 文件（按 SVE 调用方枚举或词边界锚定）。18 个调用文件含 **scripts/sve-sha-verify/verify.cpp**（scripts/ 下而非 tests/,漏改即编译断——grep 实证 5 处 x8/x4 调用）。注释中 "8 条/4 条" 措辞同步 N 化。构建+17 测试 skip+zstd19+verify.cpp 独立编译。as-built（2ea9069c）：8 函数 × 20 文件（2 头 + 17 SVE 测试 + verify.cpp），x86 侧零触碰（commit 正文「x86_64 文件的 hmac_sha1_x8 命中系前缀子串,已核非调用方」）。

### Task 10: sve512_fma_arm 接线（一 commit + gem5 同源实证）

meson `arm64_sve_lib` sources 加 `'sve512_fma_arm.cpp'`（P6 批 C 已迁 require 门控——计划基线 7e464454 实证 :343-348 为 `arm64_require_sve_vl(test, 128, 1024)` 框架层调用,注释明言取代 getauxval 手探;PR #293 检视 local-claude #15 称「实为手工 getauxval」系在 main 树上的误读——main 落后基线 79 个 commit、P6 批 C 未入 main,基线上 tests/ 有 174 文件调用 arm64_require_sve_vl,该子项驳回）；`--list-tests` 出现新 ID；`-e sve512_fma_arm` skip 干净；docblock/meson 注释更新（孤儿注释转为已接线）。**gem5 证据（as-built 修正）**：该测试 4 工作负载中第 4 个是 **gather/scatter 往返**（`svld1_gather_u64index_f64`/`svst1_scatter_u64index_f64`,docblock :42-48）——原计划「f64/f32 FMLA 链 + 特殊值形态」漏记该负载,且 sve512_chain_dual 三模式（chain_f64/chain_f32/special_f64）**不含此路径**,「家族级 scoping」不成立（PR #293 检视 coderabbit #9 / local-claude #15;PR #242 同源实证先例）。实际收口（#294 处置）：新建同源 kernel **sve512_fma_dual**——init（splitmix64 种子+模式表流+Fisher-Yates 置换）→ 三标量 golden → 四负载（含 gather-scatter 输出指针往返+fill 复位）→ 逐 lane 比较,四档（128/256/512/1024-bit）全部 SDCSHIELD_FAILS=0,poison 阴性对照 FAILS=4 全活——该文件自身逻辑首次获得执行实证（另 d609b4fc 修复 scatter 缓冲 -n>1 竞争）。

### Task 11: 收尾

progress-log 硬化单元条目（逐缺陷修复实录 + 谓词钳制语义论证）；README/meson 若有相关表述同步；push + PR（标题含"终审递延清零"）。

## Self-Review

- 覆盖：终审递延清单全部 5 项（**6 类越界**=Task 1-6；3 守卫=Task 7；assert=Task 8；命名=Task 9；孤儿=Task 10）。检视勘误：原写「7 类越界」与 Goal「六类」自相矛盾（PR #293 检视 coderabbit #6 / local-claude #18）——Task 1-6 恰为 6 类（vsum / ai_lo_vec / phase / chunk / fsu_cross / movmskpspd）。
- 原文自称「无占位符」不实（PR #293 检视 local-claude #13）：Task 2 的「按实际 svld1 谓词形态定」即未决占位（谓词形态基线可读）。处置后每处修复给具体行号、目标形态与 as-built 归属。
- Review Focus：①缓冲上限与门控闭包（16 lanes=1024-bit）②谓词钳制不改变前 4/2 lane 语义（movmskpspd 与 x86 原版逐位一致）③扩容后跨界点不变（fsu cross 仍在 line0-8）④改名 18 文件调用点零遗漏（grep 复核,含 verify.cpp、排除 x86 前缀撞名）⑤接线后孤儿注释与 meson 一致。

## 范围边界（PR #293 检视处置登记）

本计划目标是**清零 P6 终审递延清单**，不等于「仓库内全部 VL>512 越界清零」。清单外已知同型缺陷（检视 local-claude #12 指出；R2 再补登 local-claude(L74) iex 一项——教训：按文件枚举而非按类（svptrue 全宽打小栈数组）grep 的清单不完备；处置时逐项核实）：

| 缺陷 | 现状（实证） | 归属路由 |
|---|---|---|
| neon_rot_2src_sve phase[8]（:133,与 agu 同型的逐字复制块） | 已修——#294 74c618d7（`phase[SVE_MAX_VECTOR_BYTES/8]`） | — |
| movbe 族 16 文件 in/out_bytes[64]/vidx_bytes[64] 等（c4eb1546 只修了 golden） | 已修——#288 R2（52c2d755 实测全族 16 文件均 `SVE_MAX_VECTOR_BYTES` 化） | — |
| mrn_rmw_sve:115 / mrn_rmw_dump_sve:114 phase[8]；mrn_reloaded_sve got[64]/[16]/[8]（VL=1024 写 128B 入 64B 数组） | 已修——#288 检视 round 3 **17f1d032**（三文件 spill/phase 按架构最大 VL 定容：phase[SVE_MAX_VECTOR_BYTES/8]（mrn_rmw:124 / mrn_rmw_dump:123）、got[SVE_MAX_VECTOR_BYTES]（mrn_reloaded:111）;gem5 侧 09873de0 mrn_reload_rmw_dual 四档 × 3 seed 12-run 全绿,poison FAILS=4）——PR #293 R4 同步销账 | — |
| agu_stress_2src 尾批（count 未取整,VL=1024 堆越界,见 Task 3 边界登记） | 已修——#294 round 3 ee6a4a7d（尾批 slack + run 漂移 fail-closed + phase 升级 SVE_MAX_VECTOR_BYTES/8） | — |
| sve_fpcr_cartesian tie 块 ta/tb/tc/res[2]（任意 VL 越界,先存 main 原生缺陷 b2370da4） | 已修——#288 检视 round 2 **62850974**（tie 块操作数/结果迁 vl_w 向量,消任意档位全宽越界）+ round 3 b8b16015（补 init/run VL 漂移 fail-closed 守卫）——PR #293 R4 同步销账 | — |
| iex_operand_combo_sve sve_rev/sve_clz 用未钳制 `svptrue` 读写 8B/32B 定容栈数组（:88-95 `uint8_t in[8], out[8]` + `svptrue_b8()`、:110-114 `uint64_t in[4], out[4]` + `svptrue_b64()`；d2b2ece9 引入,main 原生——与 45459bfa 修复的 operand_space sve_mul_words 完全同型） | **开放**——7fe83a97 与 main 均在；**128-bit 最低档即触发**（svcntb()=16 → svst1_u8 向 out[8] 越界写 8B、svld1_u8 对 in[8]/idx[8] 越界读；sve_clz VL≥512 越界读写 64B 入 32B 数组）；门控 :255 允许 128..1024 全档 | 独立单元（修复模式 = 45459bfa 谓词钳制同型）（PR #293 R2 检视 local-claude(L74) 补登） |

## As-built 收口记录（PR #293 检视处置）

- 实施全程由 #294（feat/allvl2-hardening）承载：f7716fcf（Task 1-8,20 文件）/ 2ea9069c（Task 9）/ 3b293d9b + 6c4dc121（Task 10/11）;#294 检视处置再增 5 修复提交（d609b4fc / 7685cb73 / 45459bfa / 74c618d7 / 7fe83a97）;round 2-5 续增（**head 97cd08a1**,PR #293 R4 同步——13 门控文件行号与 agu :154 在 97cd08a1 复核仍精确）：79542f9d（mul512 低半积 per-lane 逐项比对）/ a9a4fdd8（sve_hmac 流数维度容量守卫）/ dc2dc471（u8x16 对齐基数 max→lcm,见 Task 7）/ 4ebf2199（mul512 冷路径诊断）/ 975ce16d（stencil_axis 守卫 release 化+漂移 fail-closed）/ ee6a4a7d（agu 尾批+漂移守卫,见 Task 3 与范围边界表）/ 615288c2（fma_arm docblock gem5 简表,见下条）/ d1d709c0（kreg 四兄弟漂移守卫）/ dac72d0e（k7p GPS tag 槽内偏移编码;round 5 **b3749e7e** 修订——32 位拆分恢复槽身份,round-4 槽内偏移形态的跨槽误读盲区经 gem5 intra/swap 对照实证后收回）/ e3a05d1d（kreg mask 尾批全宽 ldr 越界读补 slack,round 5）/ b3749e7e（k7p 32 位拆分恢复,round 5）/ 97cd08a1（progress-log round 5 入档;gem5 侧 8e4041a0：kreg_mask_dual 四档 9-run + sve_rot_vla_dual intra/swap 增量 12-run）。
- **提交粒度偏离登记**（PR #293 R2 检视 coderabbit(L86)）：Global Constraints「每缺陷一 commit」未被满足——f7716fcf 将 Task 1-8（六类越界 + 3 守卫 + assert 转换,20 文件）并为单一提交「终审递延清零 1/2」;Task 9/10/11 各自成 commit（2ea9069c / 3b293d9b / 6c4dc121）。无逐 Task 独立 SHA 可列,亦非 squash（从未存在过拆分提交）——如实登记为流程偏离：动机是六类修复共享同一验证管线（构建 + 9 测试 skip + zstd19 + gem5 同源）,拆分不产生独立可验证单元;教训 = commit 粒度约束应与验证管线对齐,或实施时显式登记豁免。
- **gem5 证据可核载体**（PR #293 R2 检视 local-claude(L16) 处置）：checkout 实际位于 `/home/sdc/wangxu/operator/gem5-fi`——CLAUDE.md 所写 `/home/sdc/wangxu/code-review/gem5-fi` 路径已失效（该子项成立）;但意见核心「两个 dual kernel / 所引分支与提交 / 运行记录 / checksum 表全部不存在」**驳回**：其所称「唯一 checkout /home/sdc/wangxu/gem5-fi」并不存在,系在错误路径上审计。实证：分支 **feat/pr294-hardening-dual**（本地 + origin）——136c8b95（sve512_fma_dual 四档）+ 90c25429（sve_arith_bounds_dual 16-run,7685cb73 正文所引 SHA 有效,findings.md +36 行）;findings.md 两节含逐档 checksum 表,与 7685cb73 正文、#294 处置报告三方对账一致（如 128-bit 87accea6e20b7e99）;gem5.opt 二进制在 build/ARM/。spec 三处落盘现状（R2 时点缺两处,**#294 round 2-3 已补齐**,PR #293 R3 复核）：findings.md ✓；测试 docblock 简表已落——**615288c2**（sve512_fma_arm.cpp :59-73,四档 checksum 表 a21f58a169e67cd5/5d99e539781bfd32/7053636920f1b997/153eb39bbb6c0d30 + poison 阴性对照 FAILS=4 + 无 SVE 诚实边界）;progress-log 两 kernel 条目已落——round 3 勘误入档（progress-log :875-880 sve512_fma_dual/sve_arith_bounds_dual 逐 kernel 条目）。
- **gem5 证据统一矩阵**（PR #293 R4 检视 coderabbit(L94) 处置——勘误：原 :15「4 档 × 3 seed」与 :17/:94「16-run」系同一实证的两种口径（有效维度 vs 名义 run 数）未加说明;原 :17「FAILS=4/5」系两 kernel poison 值连写、失实。poison 阴性对照性质 = **J1' 工具有效性检查**——毒化一处验证比较通道存活,分母即毒化注入数,全检出即通道全活;非 SDC 检出率统计,不据此声明检出率下界）：

  | kernel | 验证对象（section） | VL 档位 | 输入集（seed） | 名义 run 数 | 健康路径 | poison 阴性对照（J1'） |
  |---|---|---|---|---|---|---|
  | sve512_fma_dual（136c8b95） | sve512_fma_arm 四负载：f64 FMLA 链 / f64 特殊值链 / f32 链 / gather-scatter 往返 | --sve_vl 1/2/4/8（128/256/512/1024-bit） | 确定性,无随机维 | 4（每档 1） | SDCSHIELD_FAILS=0 ×4（checksum 表 docblock :64-67 与 findings.md :356-361） | 四负载 golden 各毒化一处 → **FAILS=4**（4/4 全检出） |
  | sve_arith_bounds_dual（90c25429） | A mul512 分块 / B operand_space 谓词 / C phase 派发 / D movmsk 钳制 | 同上四档 | seed 1/2/3（seed-1 行 fixed+random 双跑同输入同 checksum） | **16**（有效维度 4 档 × 3 输入集 = 12 组） | SDCSHIELD_FAILS=0 ×16（逐格 checksum 表 findings.md :393-398,如 128-bit 87accea6e20b7e99/a9843eb6328f291a/23e868549b5c5760） | 四节 golden 各毒化一处、D 节 ps/pd 两形态各一 → **FAILS=5**（5/5 全检出） |

  A 节 head 形态同步（#294 round 2 per-lane 比对 + round 3 冷路径诊断）后重跑（aad8f5b2）：12 格 checksum 与 round 1 表**逐字一致**（回归锚——per-lane 通道/诊断不改计算输出）,poison 升为 **FAILS=9**（A-memcmp 1 + A-per-lane 4 + B 1 + C 1 + D 2——per-lane 通道对 memcmp 不可见的毒化词直接检出,乘法器盲区消除由对照证明）。agu 行为修复（ee6a4a7d）另有 agu_2src_dual 12-run（见 Task 3）。
- **progress-log 残留矛盾登记**（PR #293 R2 检视 local-claude(L86)）：6c4dc121 的 progress-log 条目仍写「四工作负载 kernel 形态已由 chain_dual/gather_dual 四档实证覆盖(家族级 scoping)」「累计 gem5 四档实证 ~200 run 全绿」（7fe83a97 progress-log :758-763）——该结论已被本文 Task 10 勘误推翻（家族模式不含 gather/scatter 负载）,仓库持久记录与计划自述矛盾;勘误归属 #294 下轮（progress-log 在 #294 分支）,与本 PR 无涉（本文所在分支不含该文件版本）。**已修（as-built 更新,PR #293 R3 复核）**：#294 round 3 勘误入档——progress-log :760 现为「〔勘误 2026-10-26 round 3: '家族级 scoping' 已被 round 1 检视 #31 推翻……round 1 处置已建同源 kernel sve512_fma_dual 四档实证闭合〕」,矛盾消除。
- PR #293 本轮 13 条检视意见（行级 12 + PR 评论 1）处置：**采纳 12 / 无需处置 1**（pr-agent "No major issues" 摘要）——其中 local-claude #15 的「手工 getauxval 门控」子项与 #12 的「全仓零 require 门控」子项按基线证据（7e464454 vs main）驳回,其余子项全部成立;逐条结论与实证见处置报告 /tmp/pr293-resolution.md。
- 本轮回填本文的勘误：Task 1 文件数（11→10）与变量名表述 / Task 2 谓词占位与 b 读越界 / Task 3 尾批边界 / Task 6 lanes 删除与 vals/out 收敛 / Task 7 守卫形态（vl_bytes→取整不变式）/ Task 9 改名安全（x86 前缀撞名 + verify.cpp）/ Task 10 gem5 证据（家族类比→sve512_fma_dual 同源四档）与「已迁 require 门控」基线实证 / 全局 gem5 边界与验收 / Self-Review 类数（7→6）与「无占位符」校正。
- PR #293 **第二轮**检视处置（当前 head d7cab730；coderabbit 3 行级 + local-claude 8 行级——后者于检视数据导出后 2026-10-26 01:44 UTC 才触发,一并纳入本轮）：**采纳 7 / 部分采纳·核心驳回 3 / 驳回 1**。采纳：提交粒度偏离登记（coderabbit L86）/ iex_operand_combo 补登范围边界表（local-claude L74,Critical）/ progress-log 矛盾登记（local-claude L86）/ agu 修复样板注记（local-claude L32）/ 修复模式光谱勘误（local-claude L13）/ checkbox as-built 注记（local-claude L3）/ Task 1 行号勘误（local-claude L22,基线 :110 非 :113/:114）。部分采纳·核心驳回：coderabbit L22（「10 文件无 require 门控」系 main 树误读,门控基线即有——门控前提补登记）/ coderabbit L48（整除性算术成立但「放行错误运行状态」结论被逐元素几何证伪——检测边界补登记）/ local-claude L16（「gem5 证据无载体」被三方对账证伪——CLAUDE.md 路径失效与三处落盘缺两处两子项成立）。驳回：local-claude L87（「/tmp/pr293-resolution.md 已不存在」失实——本文件现存且为本轮处置报告载体;其「易失路径」流程观察如实记录于处置报告）。逐条结论与实证见处置报告 /tmp/pr293-resolution.md 第二轮章节。
- PR #293 **第三轮**检视处置（当前 head 67b72102;coderabbit 2 行级 + 容器 review 1,local-claude/codex/greptile 配额缺席）：**采纳 1 / 部分采纳·核心驳回 1 / 无需处置 1（容器）**。部分采纳·核心驳回：coderabbit(L14)——「arm64_sdc_sve/fsu_byteexact_sve/agu_stress_2src_sve 只检查 HWCAP_SVE,需补 arm64_require_sve_vl 门控或 SVE_MAX_VECTOR_BYTES 化」前提**被逐文件实证驳回**（三文件在计划基线 7e464454 即有 require 门控,P6 批 C 迁移;#294 head c62861e8 实测 :110/:135/:77;coderabbit 所审 PR #293 树为 main 基座、落后基线恰 79 commit,三文件确为旧式 getauxval 手探——与 R2 L22 同型误读;其 VL=2048 越界算术成立但该档被门控排除,门内闭包成立,agu 另有 ee6a4a7d 双保险）,采纳其「逐文件列出门控与 EXIT_SKIP 路径」的登记要求（Global Constraints ① 扩为 13 文件逐文件证据）;采纳：coderabbit(L53)——Task 7「u8x16 失配时 log_warning」文本失实（run 守卫 as-built 即 log_error + report_fail_msg,fail-closed;log_warning 属 init 期 knob 取整）,对齐基数 max→lcm（#294 dc2dc471）一并勘误;容器 review（#37）无独立内容。同轮 as-built 同步（保持本文与 #294 现状一致）：head 7fe83a97→c62861e8、agu 尾批已修（ee6a4a7d,范围边界表 开放→已修）、docblock 简表已落（615288c2）、progress-log 双勘误入档（round 3）。逐条结论与实证见处置报告 /tmp/pr293-resolution.md 第三轮章节。
- PR #293 **第四轮**检视处置（当前 head adeb1e70;coderabbit 1 行级（L94,id=4374296606）+ 容器 review 1,local-claude/codex/greptile 配额缺席）：**采纳 1 / 无需处置 1（容器）**。采纳：coderabbit(L94)——gem5 证据运行计数与 poison 指标口径不一致（:15「4 档 × 3 seed」= 有效 12 组 vs :17/:94「16-run」= 名义 run 数,两种口径未加说明;:17「FAILS=4/5」系两 kernel poison 值连写、失实——实际 sve512_fma_dual FAILS=4 / sve_arith_bounds_dual FAILS=5）——处置：As-built 新增「gem5 证据统一矩阵」（kernel/节/VL/输入集/名义 run 数/健康路径/poison 分母与含义）,:15/:17/:29 口径同步,poison 性质锚定 J1' 工具有效性（比较通道存活）而非 SDC 检出率统计。同轮 as-built 同步：#294 head c62861e8→97cd08a1（round 5：e3a05d1d/b3749e7e/97cd08a1 + gem5 8e4041a0）;范围边界表 mrn 行、fpcr 行 开放→已修（#288 round 2-3：17f1d032/62850974/b8b16015,gem5 09873de0 12-run;表内仅余 iex 一行开放）;Task 3 补 agu_2src_dual gem5 实证（aad8f5b2 12-run,poison FAILS=3）;13 门控行号与 agu :154 在 97cd08a1 复核仍精确。容器 review（#40）无独立内容。逐条结论与实证见处置报告 /tmp/pr293-resolution.md 第四轮章节。
