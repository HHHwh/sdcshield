# all-VL Phase 2 收尾硬化单元（终审递延清单）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 清零 P6 终审递延清单：六类先存 VL>512（部分 VL>128）越界缺陷 + 3 文件缺漂移守卫 + fmopa assert NDEBUG 失效 + x8/x4 命名残留 + sve512_fma_arm 孤儿接线。

**Architecture:** 全部为缺陷修复/机械变换，不新增功能面；gem5 侧以既有 kernel 家族证据覆盖（chain 家族模式已四档实证），movmskpspd 修复属行为收缩（谓词钳制到原语义宽度）可代码论证。

**Spec:** `docs/superpowers/specs/2026-10-23-sve-sme-all-vl2-design.md` §9/§12（SKIP 语义与验收）；递延清单出处 = P5+P6 终审报告 + P6 批 B 审计。

## Global Constraints

- 修复模式统一：**缓冲上限 16 lanes（1024-bit 门内闭包）**，谓词钳制保留原语宽度（movmskpspd）；注释引用门控不变式。
- 每缺陷一 commit（一单元一 patch）；构建零告警 + `-e <test>` skip + zstd19 回归。
- 分支：`feat/allvl2-hardening` 从 `feat/p6-allvl`（`7e464454`）切出。
- gem5 边界：本组修复全部是**缓冲尺寸/谓词边界**（不是新几何）——既有 kernel 家族（chain/gather/stencil）四档实证覆盖受影响模式；movmskpspd 修复使行为**更窄**（只读 FPS_LANES=4/FPD_LANES=2，与 x86 原版语义逐位一致）。诚实边界记录于 commit 正文。

## 任务

### Task 1: vsum[8]→[16] 家族（11 文件，一 commit）

adcx_sve/adox_sve/adcx_arm_sve/adox_arm_sve/adcxlong_sve/adcxlong_arm_sve/adcx_adox_interleaved_sve/adcx_adox_interleaved_arm_sve（各 :76-77 附近）+ gmp_bigadd_sve + operand_space_sve：`uint64_t vsum[8];` → `uint64_t vsum[16];   /* 1024-bit gate: svcntd() <= 16 */`（gmp_bigadd/operand_space 的同型变量名按实际）。

### Task 2: ai[8]/lo_vec[8]→[16]（2 文件，一 commit）

bigint_mulx_sve.cpp:52/55 + gmp_bignum_sve.cpp:52/55：`ai[8]`/`lo_vec[8]` → `[16]` + 门控注释（for k<8 填充循环改为 k<16 或保持 8 填充+其余零初始化——按实际 svld1 谓词形态定：若 ptrue 则填 16）。

### Task 3: phase[8]→[16]（agu_stress_2src，一 commit）

:123 `phase[8]` → `[16]` + 注释（填充循环 k<n 已动态，n≤lanes≤16）。

### Task 4: chunk[64]→[128]（arm64_sdc_sve，一 commit）

:85 `chunk[64]` → `chunk[128];  /* 1024-bit gate: svcntb() <= 128 */`。

### Task 5: fsu_byteexact 跨行写扩容（一 commit）

:150 `vbuf = alloc(CACHE_LINE, CACHE_LINE * 2)` → `CACHE_LINE * 2 + 128`（`cross = vbuf + CACHE_LINE - 8` 起 svptrue 写 svcntb() 字节，1024-bit=128B，56+128=184 > 128 → 需 64+128+16 余量；读回同范围，`mask != svcntw()` 全宽比较保持）。

### Task 6: movmskpspd 谓词钳制（一 commit）

:123 `svwhilelt_b32(0, lanes < 32 ? lanes : 32)` → `svwhilelt_b32(0, 4)`（FPS_LANES=4——x86 MOVMSKPS 原语义 4-lane，out[32] 的 32 上限是为宽向量写的但源行只有 4 元素）；:141 同理 → `svwhilelt_b64(0, 2)`（FPD_LANES=2）。`vals[32]`/`out[32]`/`out[16]` 保留（防御）。掩码打包循环 `i<4`/`i<2` 不变——语义与原版逐位一致，且消除末行 OOB 读（VL>128 即触发的那个）。

### Task 7: neon_rot 3 文件补 run 漂移守卫（一 commit）

neon_rot_2src_sve / neon_rot_store_only_sve / neon_rot_u8x16_sve：run 开头补 `svcntb() != data->vl_bytes → log_error + report_fail_msg`（ldr_at_top 家族同款；u8x16 按 lane 计数几何选对应 svcnt 调用）。

### Task 8: fmopa_stress assert → release 生效检查（一 commit）

:243 `assert(n >= 4 && n <= kTileNMax);` → 显式 `if (!(n >= 4 && n <= kTileNMax)) { log_error(...); report_fail_msg(...); }`（注释更新：NDEBUG 下 assert 蒸发——P2 已实证该类；release 生效检查替代）。

### Task 9: x8/x4 命名 → xN（机械改名，一 commit）

sve_sha_kernels.h 3 函数（sha256_compress_x8→sha256_compress_xn、sha512_compress_x4→sha512_compress_xn、sha1_compress_x8→sha1_compress_xn）+ sve_hmac.h 5 函数（hmac_sha256_x8→hmac_sha256_xn 等）+ 18 个调用文件 sed；注释中 "8 条/4 条" 措辞同步 N 化。构建+17 测试 skip+zstd19。

### Task 10: sve512_fma_arm 接线（一 commit + gem5 家族证据引用）

meson `arm64_sve_lib` sources 加 `'sve512_fma_arm.cpp'`（P6 批 C 已迁 require 门控，独立编译探针零告警）；`--list-tests` 出现新 ID；`-e sve512_fma_arm` skip 干净；docblock/meson 注释更新（孤儿注释转为已接线）。**gem5 证据**：该测试 4 工作负载为 f64/f32 FMLA 链 + 特殊值形态——sve512_chain_dual kernel 的 chain_f64/chain_f32/special_f64 模式四档 48+16 run 已实证同族模式（家族级 scoping 先例）；诚实边界记录。

### Task 11: 收尾

progress-log 硬化单元条目（逐缺陷修复实录 + 谓词钳制语义论证）；README/meson 若有相关表述同步；push + PR（标题含"终审递延清零"）。

## Self-Review

- 覆盖：终审递延清单全部 5 项（7 类越界=Task 1-6；3 守卫=Task 7；assert=Task 8；命名=Task 9；孤儿=Task 10）。
- 无占位符；每处修复给具体行号与目标形态。
- Review Focus：①缓冲上限与门控闭包（16 lanes=1024-bit）②谓词钳制不改变前 4/2 lane 语义（movmskpspd 与 x86 原版逐位一致）③扩容后跨界点不变（fsu cross 仍在 line0-8）④改名 18 文件调用点零遗漏（grep 复核）⑤接线后孤儿注释与 meson 一致。
