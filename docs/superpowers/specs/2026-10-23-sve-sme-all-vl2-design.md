# SVE/SME 全向量长度动态适配（all-VL Phase 2）— 设计文档

日期：2026-10-23
状态：待用户批准
作者：会话产物（brainstorming），「用户已确认」= 用户拍板项；「controller 默认」= 作者代定，批准时请一并审。
前置：PR #242 = all-VL Phase 1（计划 `docs/superpowers/plans/2026-10-13-sve-sme-all-vl.md`，merge `2b016d9c`，覆盖 `sve512_stencil_axis_arm`、`sme_fmopa_stress_arm`、`sme_za_tileview_arm` 三个测试）。
涉及仓库：本仓 + gem5-fi（`/home/sdc/wangxu/code-review/gem5-fi`）。

## 1. 意图（why）

Phase 1 确立了「运行时读 VL/SVL + 范围检查 + 几何寄存器化 + gem5 四档实证」的模板，但只改造了
3 个测试。本设计是 Phase 2：**全部 SVE/SME 用例**消除单值/硬编码向量长度假设，动态支持
128/256/512/1024 四档；运行时检测 CPU 支持情况、用例内动态适配；手写汇编提前适配完毕。
同时（用户选定方案 B）把散落在几乎所有 SVE/SME 测试文件里的探测样板**上提为框架层 API**，成为后续所有
SVE/SME 测试的统一入口。

## 2. 目标 / 非目标 / 反目标

**目标**
1. 框架层新增 `framework/device/cpu/arm64/arm64_vl.h`：能力探测（诚实运行期，不受
   `device_has_feature` 编译期恒真陷阱影响）+ 当前线程 VL/SVL 查询 + 自 pin API。
2. 全部 SVE/SME 用例消除单值门控与硬编码 VL 假设（范围检查 + 几何自适应）。
3. 三组「按设计例外」全部纳入改造：sve512 家族 ×10、ipsec HMAC ×17、sleef/eigen ×8
   （后者 spike 先行，据证据定深度）。
4. 手写汇编按 Phase 1 不变式适配：**几何寄存器化、元素大小立即数保留**（唯一遗留
   `sme_fmopa_za_arm.cpp` 的 `add %[b], #16` 步距本次修复）。
5. 每个受影响几何模式配 gem5-fi 同源 dual kernel，四档 × {fixed, random×3} 全绿 + 回归锚
   逐位一致。
6. 手写探测样板清零（统一到框架 API）；六处文档同步；x86-64 侧零改动。

**非目标**
- 不做运行时逐档扫描（单次运行内切档轮跑）——用户已否（D1）。
- 不引入 SME2 用例（全仓零使用，维持现状）。
- 不改 mesh/A4 家族的固定小 lane 语义（忠实移植设计，任意 VL 下安全运行）。
- 不把 VL 塞进 `device_features` 位图（见 §5 硬边界）。

**反目标（做了就算失败）**
- 单值门控残留（`svcntb() != 64 → SKIP` 类「单值匹配」反模式）。
- golden 比对退化为跨 lane 聚合（sum/xor/reduce）——必须逐 lane/元素字节级 memcmp。
- 把「SKIP 理由不变」单独充当功能性修改的验证结论（AGENTS.md:26-32 规则）。
- 超范围 VL 进入 run（无上限缓冲保护）。
- x86-64 侧任何文件被改动。

## 3. 约束与事实（2026-10-22/23 探索实测）

### 3.1 用例清点（SVE 180 + SME 3 = 183 个 .cpp + 2 头；另框架探针 1；SME2 零使用）

| VL 处理类别 | 数量 | 代表 | 现状结论 |
|---|---|---|---|
| A 真 VL 无关（svcnt/svwhilelt 动态分批） | ~120 | fma/、arithmetic/、mem/、case/ 大部、ipsec EVP 7 | 已达标（适配当前档） |
| A4 固定小 lane 谓词 | ~28 | mesh/ 18、arm0102 kreg 5 | VL 安全、负载锁 4-lane（按设计） |
| B 编译期固定 128（`-msve-vector-bits=128`） | 8 | sleef_sve、eigen gemm/svd、顶层 sve_rot×5 | 不 pin 有堆损坏前科（`gemm_double_max_arm.cpp:40-47`） |
| C 512-only 单值门控 | 10 | sve512 家族（`svcntb()!=64→SKIP`） | 与四档目标正面冲突 |
| C VL≥256 门控 | 17 | ipsec HMAC（SHA 8-lane 内核约束） | VL=128 跳过 |
| SME 已动态 | 2 | za_tileview、fmopa_stress（Phase 1 模板） | 已达标 |
| SME 半动态遗留 | 1 | `sme_fmopa_za_arm.cpp:95-96` `add %[b], #16` | 本次修复 |

**手写汇编审计**：~40 文件含 asm，绝大多数是 `"=w"` 全寄存器 ldr/str（单指令、天然 VL 无关）
+ init `svcnt` 定尺寸 + run 期 fail-closed 守卫；SME naked kernel 已参数化。唯一硬编码 VL
假设即上表 `#16`。

**P0 级附带发现（bug/矛盾）**：
1. `tests/cpu/sve/sve_rot_ldr_at_top*.cpp` ×5 注释声称 "true VLA"，实际编入带
   `-msve-vector-bits=128` 的 tests_sve 库（`tests/cpu/meson.build:990` vs :447 注释矛盾）。
2. `neon_rot_ldr_at_top_mla_sve.cpp:117/163` 裸 `svuint32_t res;` 未初始化（c8ac06fe 修复
   名单漏掉此文件，同类 release 构建隐患）。
3. `tests/cpu/arm64/sve512_fma_arm.cpp` 孤儿未入任何构建目标。
4. CLAUDE.md 的 gem5 路径 `/home/sdc/wangxu/gem5-fi-fuzz` 本机不存在（实际
   `/home/sdc/wangxu/code-review/gem5-fi`）；「host is VL=256」是另一台机器（cortex x3b，
   `docs/architecture.md:130`）的过时记录。
5. progress-log 缺 PR #242 条目；`sme_za_tileview_arm` 注释声称的五相位 dual kernel 与四档
   checksum 不在本机任何 gem5-fi checkout（历史验证仓已消失）。

### 3.2 框架与构建事实

- 能力检测：`simd-arm.conf` → `armsimd_generate.pl` → `cpu_features.h` 生成管线；
  `detect_cpu()` 读 `getauxval(AT_HWCAP/HWCAP2)`（`framework/device/cpu/cpuid_internal.h:262`）。
- 陷阱：`device_has_feature(f) = (device_compiler_features & f) || (device_features & f)`
  （`framework/device/cpu/cpu_device.h:123`）——+sve 编译的 TU 恒真，故现行纪律是测试 init
  手写 `getauxval` 复核（全部 SVE/SME 测试如此）。
- **框架零 VL/SVL 概念**：无 rdvl/rdsvl、无 `prctl(PR_SVE_*/PR_SME_*)`、无 svcnt；
  `framework/device/cpu/arm64/arm64_cpuid.h` 是无人使用的自包含备用层。
- 构建矩阵：`tests_set_sve`（`-march=armv8.2-a+sve -msve-vector-bits=128` + Eigen 宏，
  `tests/cpu/meson.build:988-991`）；`tests_arm64_sve`（真 VLA，`arm64/meson.build:274`）；
  `tests_arm64_sve2`（`armv9-a+sve2+bf16+f32mm+f64mm`，:290）；`tests_arm64_sme`
  （`armv9-a+sve+sme+sme-f64f64` + has_argument 守卫，:341）。
- VL pinning 先例：外部工具 `/tmp/vlrun`（prctl 后 exec）**已消失**；eigen SVE 必须 pin。

### 3.3 环境事实

- **本机 CPU part 0xd01（Kunpeng 920，96 核）无 SVE/SME**（/proc/cpuinfo Features 与
  /proc/sys/abi 双确认，2026-10-23）→ 本战役所有功能验证只能走 gem5-fi 四档；本机只验证
  构建零警告 + SKIP 路径 + selftest。
- `gem5.opt`（1.1G，2026-10-15 构建）就绪；档位参数 `configs/se/arm_chaos.py
  --sve_vl/--sme_vl {1,2,4,8}` = 128/256/512/1024-bit。
- dual kernel 目录 `workloads/sdcshield/` 现有 4 个 `*_dual.c` + 语义探针 + PR #244 验证件。

## 4. 已锁定决策（用户拍板）

- **D1 语义 = 适配当前档**：推广 Phase 1 模板；init 探测能力 + 当前 VL/SVL，范围检查 +
  几何自适应；四档正确性由 gem5 逐档验证保证。不做运行时逐档扫描。
- **D2 范围 = 三组例外全部纳入**：sve512×10 重构、ipsec×17 参数化、sleef/eigen×8 真 VLA
  （spike 先行）。
- **D3 架构 = 方案 B 框架优先**：先建框架层 VL/SVL API，全量统一后再逐族重构。
- controller 默认：API 放独立新头 `arm64_vl.h`（`arm64_cpuid.h` 保持 feature-bit 定位）。

## 5. 框架层 API 设计（P1 交付物）

位置：`framework/device/cpu/arm64/arm64_vl.h`（arm64/ 目录仅 aarch64 构建，天然满足
「x86 untouched / port is additive」）。

**低层纯查询**（任何 aarch64 TU 可用；无 +sve march 依赖——rdvl/rdsvl 用 `.inst` 定数编码，
即 `framework/selftest.cpp:911-914` smstart 同款手法；getauxval/prctl 编译器无关）：

```c
bool arm64_cpu_has_sve(void);      /* getauxval(AT_HWCAP) & HWCAP_SVE，不受编译期宏陷阱影响 */
bool arm64_cpu_has_sve2(void);
bool arm64_cpu_has_sme(void);      /* getauxval(AT_HWCAP2) & HWCAP2_SME */
long arm64_sve_vl_bytes(void);     /* rdvl #1；无 SVE 返回 0 */
long arm64_sme_svl_bytes(void);    /* rdsvl #1；无 SME 返回 0 */
long arm64_sve_pin_vl(long bytes); /* prctl(PR_SVE_SET_VL)；返回实际生效值，失败 -errno */
long arm64_sme_pin_svl(long bytes);
```

**测试流助手**（收敛 183 处「手探 getauxval + svcnt 读取 + 范围检查 + log_skip」四段样板）：

```c
/* test_init 内一行；返回 >0 = 当前 VL 字节数（HWCAP 已确认且 min≤vl≤max）
   返回 -1 = 已 log_skip（缺能力或超范围，文案含实测值），调用方直接 return EXIT_SKIP */
long arm64_require_sve_vl(struct test *t, long min_bits, long max_bits);
long arm64_require_sme_svl(struct test *t, long min_bits, long max_bits);
```

**三条硬边界**：
1. VL 不进 `device_features` 位图——VL 是 per-thread 运行量，feature 是进程级位。
2. 自 pin 仅在 test_init 调用（worker 线程随后创建并继承 VL；实现时以 selftest 实证
   「clone 继承创建线程 VL」假设）。
3. 配套交付：`--dump-cpu-info` 增加 SVE VL / SME SVL 输出；selftest 新增 VL 探针自检
   （仿 `framework/selftest.cpp:2786-2850` feature-gate selftest 先例）。

## 6. 阶段划分（每阶段独立分支 → PR → gem5 四档验证）

| 阶段 | 内容 | 交付物 |
|---|---|---|
| P0 快修 | mla svundef、sve_rot×5 构建/文档矛盾、孤儿 sve512_fma_arm 处置（查 git 历史定夺：有意排除→注释说明；遗漏→接入+gem5 验证）、CLAUDE.md gem5 路径、progress-log 补 PR #242 条目 | ~5 个独立小 PR，不阻塞后续 |
| P1 框架层 | `arm64_vl.h` + dump-cpu-info + selftest + 文档（architecture.md / adding-tests.md） | 新 API + 自检 |
| P2 SME 收尾 | `sme_fmopa_za_arm` 全 VL 化（新 API + `#16` 步距寄存器化 + 范围检查）+ dual kernel | SME 侧全部达标 |
| P3 sve512×10 | 逐孪生对（_arm/_svd）重构：512 单值门控 → 范围检查、几何 svcnt 化 | ≈5-6 个 dual kernel |
| P4 ipsec×17 | SHA 流数参数化（VL=128 可跑），基于现成 `tests/cpu/sve/ipsec/sve_sha_kernels.h` | ≈3 个 dual kernel 模式 |
| P5 sleef/eigen×8 | **spike 先行**：Eigen SVE packet 后端 VLA 能力调查 + sleef VL=256 编译回归根因（progress-log:223 open item）；据证据定深度 | spike 报告（spec 附录）→ 重构或如实上报 |
| P6 存量迁移 | A 类已达标测试分批机械迁移到新 API（按目录分批，每批构建 + `-e <test> -t 2000 -n 1` 抽查） | 样板清零 |

阶段内 work item 串行（验证 → commit → push，one-patch-per-unit）；跨仓流水沿用 PR #242
模式：gem5-fi 分支先行（dual kernel 参数化 + 四档全绿 + findings.md 记录）→ sdcshield
同源落地，两仓 PR 描述互相引用。

## 7. gem5 验证协议（Phase 1 规程固化）

- **dual kernel 结构**：每几何模式一个，放 gem5-fi `workloads/sdcshield/`。同源双通道：
  通道 A 镜像测试校验语义（golden 比对 → `SDCSHIELD_FAILS=<n>`）；通道 B 独立 FNV-1a
  16-hex checksum（Escape/Masked 判别）。argv `[fixed|random] [seed]`；Makefile 沿用
  clang `-static -O2 -march=armv9-a+sve+sme`（FMOPA 类强制 `-ffp-contract=off` 对齐
  gem5 fplib 双舍入）。
- **运行矩阵**：受影响档位 128/256/512/1024 × {fixed, random×3} = 每 kernel 16 run；
  全绿 = 全部 `SDCSHIELD_FAILS=0` 且 random 各 seed checksum 互异。
- **回归锚**：重构前在原设计档跑旧 kernel 记录 checksum；重构后同档逐位一致
  （先例：`e9e2be2a` @ svl=4）。
- **记录落盘三处**：测试源码 docblock 简表（命令形态 + 档位 + checksum）+ gem5-fi
  `findings.md` 详细表 + sdcshield `progress-log.md` 条目。
- **历史边界**：本战役所有 dual kernel 落在本机 checkout 可复现；旧 za_tileview 五相位
  kernel 缺口不修不赖账（§3.1 发现 5），记为已知边界。

## 8. golden 策略与比对不变式

| 族 | golden 来源 | VL 适配 |
|---|---|---|
| sve512×10 | init 期标量同序重算（现行模式） | 几何 svcnt 化后自动适配 |
| ipsec×17 | 同一 SVE 内核 init 期预计算（现行设计，OpenSSL 交叉验证由 `scripts/sve-sha-verify` 兜底） | 流数参数化后同内核重算保持自一致 |
| SME fmopa_za | 精确整数操作数标量累加（现行） | 列数按实测 SVL 重算 |
| sleef/eigen×8 | spike 后定，结论写入本 spec 附录 | 不预设 |

**军规不变式**（PR #242 review 产出）：逐 lane/元素**字节级 memcmp/memcmp_or_fail**，
禁止 sum/xor/reduce 跨 lane 聚合。

## 9. SKIP 语义规范（新 API 内置）

1. **范围检查替代单值匹配**：`min ≤ 实测 VL ≤ max`，超范围 → skip，文案必须含实测值。
2. 两类文案分开：缺 HWCAP 位 = 能力 SKIP；能力在但档位超范围 = 范围 SKIP。
3. run 侧 **fail-closed 守卫保留**（防 init/run 间 VL 漂移：实测 ≠ init 记录值 → fail
   不 skip）。
4. 未实现功能继续遵守 placeholder honesty（EXIT_SKIP + "to be implemented" 文案）。

## 10. 文档同步（六处）

| 文档 | 改动 |
|---|---|
| `docs/architecture.md` | feature detection 章节后新增 VL/SVL 层小节（arm64_vl.h 定位、与 device_features 边界） |
| `docs/research/adding-tests.md` | 政策改写：SVE/SME 测试 init 从手写 getauxval → `arm64_require_*` 一行 |
| `docs/coding/writing_tests.md` | 补 VL 政策节：范围检查、fail-closed 守卫、字节级 memcmp 不变式（**P2+ 交付**——memcmp 不变式随 P2 首个改造用例落地时一并写入；P1 只交付框架 API 与 adding-tests 政策。对抗性评审 #6 修正：原表未排期形成无主条目） |
| `README.md` | SVE/SME 用例四档适配说明 + `--dump-cpu-info` 新输出 |
| `CLAUDE.md` | gem5 路径修正；过时「host is VL=256」陈述修正 |
| `docs/research/progress-log.md` | 补 PR #242 条目 + 本战役各阶段条目 |

## 11. 风险与边界

1. **Eigen 第三方墙**（P5 最大不确定）：vendored Eigen 5.0 SVE packet 后端可能本质固定
   宽度。spike 查明后三选一：(a) patch vendored Eigen（同步
   `docs/coding/OPEN_SOURCE_PROVENANCE.md`）；(b) 测试改 VLA intrinsics 直算、Eigen 仅作
   参考（改变测试性质，需用户决策）；(c) 带证据上报，回退自 pin + 诚实 SKIP。
2. **sleef VL=256 编译回归根因未知**：spike 走 systematic-debugging 流程，不盲改。
3. **prctl VL 传播**：设计假设 clone 继承创建线程 VL；P1 以 selftest 实证。
4. **迁移回归风险**（P6）：按目录小批量 + 每批构建 + 回归抽查。
5. **ipsec 压力语义变化**：参数化后 VL=128 跑 4-lane 与 8-lane 压力特性不同——四档验证
   + 文档明示。
6. **测试 ID 不改名**：sve512 家族保留 ID（运维脚本按前缀过滤，如
   `scripts/run/sdc_detect.sh` 剔除 `sme*`）。
7. **本机验证边界**（诚实声明）：本机无 SVE/SME，只能验证构建零警告 + SKIP 路径 +
   selftest；所有功能结论以 gem5 四档真实 run 输出为准（AGENTS.md:26-32）。

## 12. 验收标准

1. 受影响的 SVE/SME 用例在 gem5 四档 × 16 run 全绿（`SDCSHIELD_FAILS=0`，random checksum
   互异）。
2. 回归锚全部逐位一致（几何重构零语义漂移）。
3. 本机（无 SVE）：构建零警告 + 全部 SVE/SME 用例干净 SKIP + selftest VL 探针通过。
4. 手写探测样板清零：`tests/` 下不再有 SVE/SME 的 getauxval 手探 init 样板（探测统一走
   `arm64_vl.h`；验收以 grep 审计为准，框架自身实现除外）。
5. 六处文档同步完成。
6. x86-64 侧 diff 零改动（审计确认）。

## 附录 A：探索方法与口径

- 三路并行探索（SVE 用例清点 / SME 用例清点 / 框架·构建·gem5 环境）+ clangd LSP 符号
  导航（单文件级可用；跨文件索引因 builddir 缺失降级，以 grep 兜底）+ 本机 /proc 实测。
- 清点口径：以 `-msve-vector-bits` 编译归属、`svcnt*/svwhilelt/rdvl/rdsvl` 使用形态、
  `"=w"` 约束 asm、HWCAP 门控文案为准逐文件核验；分类定义见 §3.1 表。
