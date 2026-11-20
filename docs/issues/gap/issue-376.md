# Issue #376 解决方案文档：sve512_*(15)/power_virus 链终态-only 校验漏检盲区

- Issue：[#376](https://github.com/wangxumarshall/sdcshield/issues/376)（2026-11-17 立案）
- 本文核查日期：2026-11-18（核查基线 = main `727cc8d3`；行号为当日实查）

## 一、结论先行

**issue 所列三处现场（sve512_f32_chain_arm、sve512_fmmla_arm、power_virus_dit）已全部在
main 落地分段中间检查点修复；族级普查后确认剩余 1 个真缺口：`power_virus_dit_sve`
（`tests/cpu/sve/fma2/power_virus_dit_sve.cpp`）仍是终态-only，且属 issue 自己分级中风险
最高的「累积型」链。** 另 3 个未分段文件（gather_scatter×2、pred_ops、xlswave）经核查不在
盲区内（每次迭代的每次写入在同迭代内即被逐字节校验，不存在「后续运算吸收中途翻转」的路径）。

## 二、现状核查（逐文件实测）

### 2.1 issue 点名三处——均已修复

| issue 引用（立案时行号） | 当前状态 | 修复后落点（当前行号） |
|---|---|---|
| `sve512_f32_chain_arm.cpp:248-253`（f32 链终态） | **已修** | golden 按 SEG_LEN=64 步边界记录全段（`:119-136`，注释明引 issue #376）；硬件链每边界 `svst1` 溢出（`:143-155`，`:152` "issue #376 checkpoint spill"）；run 期分段比对（`:270-292`）。末段即链终态，旧终态-only 比较被包含 |
| `sve512_fmmla_arm.cpp:322-331`（FMMLA tile 终态） | **已修** | `:88-89` SEG_LEN=64/SEG_COUNT；golden 每 step 边界记 f64/f32 双 tile（`:255-276`）；硬件快照 `:349`（"issue #376 ckpt snap"）；另有单步 golden 自检（lane-mapping fail-closed） |
| `power_virus_dit.cpp:155,169-186`（2048 步 FMA burst 终点累加器） | **已修** | `:74-84` BURST_INNER=2048/SEG_LEN=64/SEG_COUNT=32；`burst_fma_chain_seg` 每 32 步（SEG_LEN/2）vst1q 溢出 4 lane 累加器（`:115-133`，`:128` "issue #376 checkpoint spill"）；`burst_fma_reference_seg` 软件参考同步分段（`:136-146`）；run 期全边界逐 lane 字节比对（`:165-196`，注释："a transient fault anywhere in the burst corrupts the next snapshot while it is still supra-ulp, before the growth grinds it away"） |

### 2.2 族级普查（17 个相关文件全查）

**已分段（12）**：sve512_f32_chain_arm、f32_chain_svd、f64_chain_arm、f64_chain_svd、
f64_special_arm、f64_special_svd、fcmla_arm、fma_arm、fmmla_arm、nt_reload_arm（改为
逐步 golden：`golden_step` 记录每一步 stnt1d 目标向量，`:82-86` 注明 "the stnt1d region
of intermediate steps was previously never verified"）、stencil_axis_arm（形态 B 128 个
kRandSeg=64 边界 + 边界比例化容差上界，`:134-144`、`:462-496`）、power_virus_dit、
power_virus_dit_sve_arm（f64 链 per-lane 分段 golden，`:129-184`）。以上文件源码内均含
"issue #376" 注释锚点，可 grep 复核。

**不在盲区（4，如实分类）**：

- `sve512_gather_scatter_{arm,svd}.cpp`：gather/scatter 是单发置换写，golden 按
  `idx[i]→dst` 映射（`:95-115`），每轮全量字节比对——每个元素的写入就是它的终值，无
  后续累加可吸收中途翻转。
- `sve512_pred_ops_arm.cpp`：四个 section 各自的精确 golden（谓词/FMA 互补集/规约/FFR
  位图），单节内无长累积链。
- `sve512_xlswave_ctx_arm.cpp`：每个 sweep（~0.1 ms）全量字节比对 init 冻结 golden
  （`:31-41`），检查点密度远高于问题阈值。

**剩余真缺口（1）**：`tests/cpu/sve/fma2/power_virus_dit_sve.cpp`（测试 id
`power_virus_dit_sve`，PROD）：

- 链本体 `:96-108`：`acc = svmla(acc, a0, b0); acc = svmla(acc, a1, b1)` 交替，常量
  `:89-93`（A0·B0=4.0，A1·B1=−0.25 → 每对净增 +3.75）——纯**累积型**（issue 分级中
  风险最高类：增长把低位翻转磨掉）。
- 校验 `:141-156`：run 函数（`:134` 起）内，整条 2048 步（×scale 1..256，`:49-56`
  knob）链跑完后只比**终点** `acc` vs `burst_fma_reference` 的单值 `sw_ref`
  （`:113-121` 只返回终值；逐 lane 字节比对在 `:148-156`）。
- 中途单 bit 翻转的吸收路径：步 k 累加器 ≈3.75k/2，f32 ulp 随幅值增长（终值 3840 处
  ulp=2⁻¹²）；早步低位翻转（当时 ulp=2⁻²² 量级）在后续 +3.75 累加中被舍入吸收，
  终态可不变 → 漏检。与已修的 `power_virus_dit.cpp` 修复前形态完全同构。

注意与 `power_virus_dit_sve_arm`（tests/cpu/arm64/，已修）是**两个不同测试**：前者是
fma2 目录的 f32 版（本缺口），后者是 arm64 目录的 f64 版（已修）。

## 三、根因

1. **剩余缺口**：`tests/cpu/sve/fma2/power_virus_dit_sve.cpp:134-156` —— burst 链终点
   单值比对；`:113-121` 软件参考只产出终值；无任何中间检查点。链型为累积型
   （常量 `:89-93`）。
2. **已修三处的原根因**（留档）：链/tile 校验只比最终状态，中途翻转被加法淹没或
   f32 舍入对齐吸收（issue 原文机理，已被分段检查点消除）。

## 四、建议修复路径

对剩余的 `power_virus_dit_sve` 移植同族已三处落地的模式（**不新设计**）：

1. 参照 `power_virus_dit.cpp:115-146` 的 `burst_fma_chain_seg`/`burst_fma_reference_seg`：
   链内每 `SEG_LEN/2 = 32` 步把 `acc` `svst1_f32` 溢出到 `seg_out`（fma2 版是全 lane
   同链、lane-uniform 常量，比 NEON 版更简单——每段一个向量，不需 [4] 数组）；
   软件参考同步分段记录。
2. run 期（`:134-156` 处）改为全 SEG_COUNT 段逐 lane 字节比对（先例
   `power_virus_dit.cpp:184-196` 的告警格式含 seg/lane/golden/actual）。
3. **分段粒度红线**（issue 明确要求遵守）：检查点不能破坏 di/dt 压力形态——溢出仅是
   每 32 步一次的 `svst1`，burst 链仍是不可断的寄存器依赖链（f32_chain 先例
   `:147-149` "the FMA dependency chain runs on unbroken"）；参照 #360/`97c88275` 的
   TEST_LOOP 粒度教训，段数 32/2048 步对 `-t` 预算无感。
4. scale knob（`:49-56`，burst=stall=2048×scale，clamp [1,256]）与分段正交：SEG_LEN
   取常数 64、SEG_COUNT = burst_inner/SEG_LEN 随 scale 缩放；注意 `burst_inner` 为奇数
   边界（scale 产生非 64 倍数时）取 floor 段 + 末尾余段直接并入最后一段比对。
5. 可选加值项（issue 修法方向 3）：评估把 BURST_C/操作数整数值化以消除舍入吸收路径
   本身——但 fma2 版与已修三处保持同构优先，整数值化是独立决策，不阻塞本修复。

## 五、验证边界

1. **本机（SVE1-256）真实跑**：`./builddir/sdcshield -e power_virus_dit_sve -t 2000 -n 1`
   → `exit: pass`（回归红线：`-e zstd19 -t 2000 -n 1` → pass）。构建零新告警
   （`./scripts/build.sh`）。
2. **漏检盲区的正向证明（issue 验收方向）**：按 CLAUDE.md rule 2 的 gem5-fi 规则——
   本机无法做链中途单 bit 注入，须在 gem5-fi（CHAOS/SE）对修复后的分段校验做注入
   演示：同一翻转注入点在「终态-only（旧形态，可用 git 历史版本对照跑）」下漏检、
   在分段检查点下 FAIL，引真实 run 输出（tier、checksum、`SDCSHIELD_FAILS` 计数）。
   已修三处的同款验收（`power_virus_dit.cpp` 等）可复用其 gem5 腿配置。
3. **VL 档位**：fma2 版运行时 VL 自适应（`svcntw()`），本机 SVE-256 可直接验 256 档；
   128/512/1024 档按 gem5-fi `--sve_vl {1,4,8}` 补验（All-VL 政策）。
4. **超时红线**：分段后 `-t 2000` 不超时（对照 #360）；`-O power_virus_dit_sve.scale=256`
   最大档同样不超时（溢出开销 O(SEG_COUNT×VL)，256×2048 步下仍远小于 burst 本体）。

诚实边界：本文「已修 12 文件」的判定基于源码结构核查（分段逻辑 + 比对路径 + "issue #376"
注释锚点），其 gem5 注入验收是否全部留档未逐一复核；剩余缺口的修复方案未实现代码，仅为
方案文档（mission 边界：只出文档）。
