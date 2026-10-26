# C — SVE/SVE2/SME/SME2 ISA 能力、服务器硅片矩阵与仓库接入现状调研

> 调研日期 2026-09-16。为「最大化激发 ARM64 浮点向量单元 SDC」测试增强提供 ISA 侧依据。
> 标注约定:「实测」= 本机命令/源码直接验证;「联网」= Web 检索(来源附后);「知识」= 自身知识未查证。

---

## 1. 仓库接入现状

### 1.1 特性检测:比预期完整得多 — `cpu_feature_sme` 其实已存在

**任务背景中说「simd-arm.conf 含 sme 字样但无 cpu_feature_sme」——这是不准确的,实测结论相反:**

- `framework/device/cpu/simd-arm.conf` 已定义全部 SVE/SVE2/SME/SME2 特性位(实测全文读取):
  - HWCAP:`sve`(bit 22)、`asimdfhm`(23, NEON FP16-FML)、`fphp`(9)、`asimdhp`(10)、`fcma`(14)、`asimddp`(20)、`bf16`(HWCAP2 14)、`i8mm`(13)
  - HWCAP2 SVE 族:`sve2`(1)、`sveaes`、`svepmull`、`svebitperm`、`svesha3`、`svesm4`、`svei8mm`(9)、`svef32mm`(10)、`svef64mm`(11)、`svebf16`(12)、`sve_ebf16`(33)、`sve2p1`(36)
  - HWCAP2 SME 族(共 16 位):`sme`(23)、`sme_i16i64`(24)、`sme_f64f64`(25)、`sme_i8i32`(26)、`sme_f16f32`(27)、`sme_b16f32`(28)、`sme_f32f32`(29)、`sme_fa64`(30)、`sme2`(37)、`sme2p1`(38)、`sme_i16i32`(39)、`sme_bi32i32`(40)、`sme_b16b16`(41)、`sme_f16f16`(42)
  - 依赖链正确:`sve2` 依赖 `sve`,全部 `sme_*` 依赖 `sme`(required-feature 列)
- 生成的 `builddir/framework/device/cpu/cpu_features.h`(实测 grep):**`cpu_feature_sme = CPU_FEATURE_CONSTANT(55)` 存在**,连同全部 16 个 sme/sme2 子特性位;`cpu_feature_sve`(22)、`cpu_feature_sve2`(33)、`cpu_feature_svef32mm/f64mm/svebf16` 等全在,头文件共 263 处 `cpu_feature_` 定义/引用。
- `armsimd_generate.pl`(实测 168-180 行)compiler_macro 表映射 `sve→__ARM_FEATURE_SVE`、`sve2→__ARM_FEATURE_SVE2`、`sve2p1→__ARM_FEATURE_SVE2P1`、`sme→__ARM_FEATURE_SME`、`sme2→__ARM_FEATURE_SME2`,驱动 `device_compiler_features`(编译期特性集合)。
- `framework/selftest.cpp`(实测 2748-2770 行)已有 `.minimum_cpu = cpu_feature_sve` 和 `cpu_feature_sve2p1` 的自测测试 → **框架的 minimum_cpu 门控对 SME 同样可用,零改动**。
- 运行时填充:`cpuid_internal.h:278-279` `getauxval(AT_HWCAP/AT_HWCAP2)` → `device_features`;`arm64_cpuid.h:162-206` 已有 `arm64_has_sve()/arm64_has_sve2()` helper。
- **`sme` 字样的真相:不是占位符也不是遗漏,是完整的检测能力;缺的只是「用 SME 指令的测试」与「+sme 编译库」。**
- 发现一处 conf 疑似错误(实测 136 行):`arch=neoverse_v2 neoverse_n2 sve2p1,sme,sme2` —— 公开资料中 Neoverse V2(Armv9.1 级,Grace/Graviton4/Cobalt 均用)无 SME,SM​E 从 Neoverse V3(Armv9.2)开始(联网查证,见 §4)。此行会导致 dump-cpu-info 把 V2 识别成带 SME 的架构,建议后续修正。

### 1.2 SVE 测试库组织(两套 SVE 库,机制不同)

**库 A:`tests_arm64_sve`**(`tests/cpu/arm64/meson.build:152-227`,实测):
- 9 个 sve512 测试(arm 家族 4 + SVD 家族 4 + stencil_axis 1,#271-#280)
- 编译 flags:`-march=armv8.2-a+sve`,**无 `-msve-vector-bits`** → sizeless `svfloat64_t` 采用**运行时 VL**(0xd22 上=512-bit 全宽)
- 独立成库的原因(meson 注释原文):+sve 若进 `tests_arm64` 基线库,GCC 会把 NEON 测试自动向量化成 SVE 指令,在无 SVE 的 Kunpeng 920 上 SIGILL
- 目标机侧注意(memory 记录):cn23154 的树被手工改名 `tests_arm64_sve512`,且有一个仓库不存在的 `sme_fmopa_arm.cpp`(他人本地新增,未回流)

**库 B:`tests_sve`**(`tests/cpu/meson.build:888-930` + sourceset `tests_set_sve`,实测):
- 成员:`eigen_svd/svd_cdouble_sve.cpp` + `sleef/sleef_sve.cpp`
- 编译 flags:`-march=armv8.2-a+sve` **`-msve-vector-bits=128`**(固定 128-bit SVE,Eigen 的 SVE packet 后端需要定宽)+ `-DEIGEN_ARM64_USE_SVE` + **`-DEigen=EigenSVE`**(namespace 重命名,使 SVE 后端的 Eigen 与 NEON 基线 Eigen 符号共存——与 x86 侧 `EigenAVX2/EigenAVX512` 同机制)
- `-msve-vector-bits=128` 下 `svfloat64_t` 恰 2 lane,sleef_sve.cpp 注释明确按此设计

### 1.3 sve512 测试的代码模式(sve512_f64_chain_arm.cpp / gather_scatter / stencil_axis 精读)

1. **ACLE intrinsics(`<arm_sve.h>`)+ `#ifdef __aarch64__` 全包裹**,非 aarch64 走 placeholder skip
2. **运行时探测先于任何 SVE 指令**:`getauxval(AT_HWCAP) & HWCAP_SVE` 为 0 → `log_skip(CpuNotSupportedSkipCategory) + EXIT_SKIP`。sleef_sve.cpp:93-110 注释解释了为什么不用 `device_has_feature(cpu_feature_sve)`:它是 `(device_compiler_features & f) || (device_features & f)`,TU 以 +sve 编译时编译期一半恒真 → **+SVE 编译的 TU 内必须手写 HWCAP 探测**
3. **框架还有第二道自动门**:`DECLARE_TEST` 宏默认 `.compiler_minimum_device = device_compiler_features`(sandstone.h:163),框架对「编译期特性 ⊄ 运行时特性」的测试自动 skip("test compiled with sve"),sandstone.cpp:462 消费同一字段
4. **VL 探测**:`svcntd()` 在 init 里取每向量 f64 lane 数;stencil_axis 进一步要求 `svcntd()==8`(即 512-bit),否则 skip——按 VL 门控的先例
5. **golden 模式**:标量 C++ 以**相同运算顺序**重算(`std::fma`/`fmaf`),逐 lane **字节级 memcmp**;特殊值表按类别比较(fsu_byteexact_arm 先例,NaN payload 不比)
6. 使用的 intrinsic 全集(实测 grep 全部 sve512 测试 + sleef_sve):`svld1_f32/f64`、`svld1_u64`、`svld1_gather_u64index_f64`、`svst1_f32/f64`、`svst1_scatter_u64index_f64`、`svmla_f32_x/svmla_f64_x/svmla_x`、`svptrue_b64`、`svwhilelt`、`svcntd`、`svdup_f64`、`svfloat32/64_t/svuint64_t/svbool_t` —— **仅此而已,见 §2 覆盖差距**

---

## 2. SVE/SVE2 浮点指令族清单(按压力特征分组;✓=本仓已覆盖,✗=未覆盖)

以下指令分组基于 Arm ARM(SVE/SVE2 extensions)知识(标注「知识」处)与本仓源码 grep 实测对照。

### 2.1 常规 FMA 族 — 吞吐压力(FMA 管线满载)
| 指令族 | 语义 | 压力特征 | 覆盖 |
|---|---|---|---|
| `FMLA z,d,d,d` / `FMLS` | 逐 lane 融合乘加 | 每 cycle 2×VL 宽 FMA,主吞吐路径 | ✓ `svmla_f64_x/f32_x`(chain/special/stencil/SVD 8 测试) |
| `FMLA z,d,d,Zm[i]`(indexed) | 操作数取自 Zm 固定 lane | GEMM microkernel 形态;寄存器堆读端口不同(单 lane 广播) | ✗ |
| `FMAD`(multiply-add,标量对形式) | 两操作数标量 | 谓词全假时的退化路径 | ✗(影响小) |
| `FSCALE`(2^x 逐 lane 缩放) | 指数路径 | 指数加法器单独激励 | ✗ |
| `FRINTA/N/P/M/Z/X`(SVE1 即有,frint 特性位是 NEON 侧) | 取整 | 舍入模式多路径 | ✗ |
| `FRECPS/FRSQRTS` | 倒数/开方 Newton 步 | 除法/开方微码压力 | ✗ |

### 2.2 跨精度/低精度乘加族 — 独立数据通路(最高价值差距)
| 指令族 | 语义 | 依赖特性 | 压力特征 | 覆盖 |
|---|---|---|---|---|
| `FMLALB/T`(FP16→FP32 扩展精度乘加) | 半宽输入全宽累加 | SVE2 | 独立半精度管线+移位拼接;不同舍入行为 | ✗ |
| `FMLALLBB/TB/BT/TT`(FP8→FP16) | 1/4 宽输入 | SVE2(部分核无) | 最低精度通路 | ✗ |
| `FDOT`(FP8/FP16 点积累加) | 多组乘加水平折叠 | SVE2 | 点积竖直折叠单元 | ✗ |
| `BFDOT`(BF16×2→FP32) | BF16 点积 | `svebf16` | BF16 datapath(cn23154 ZFR0.BF16=1) | ✗ |
| `FMLALB/T .bf16` | BF16 扩展精度 | `svebf16` | 同上 | ✗ |
| `FDMLA`(FP32×FP32→FP64 累加,**仅 SVE2**) | 双 narrow 输入宽累加 | `sve2` | 跨宽度对齐/移位器+宽加法器 | ✗ |
| `FTSMUL`(两 lane 交叠乘,BF16 训练用) | 交错乘 | SVE2 | 奇偶 lane 交换网络 | ✗ |
| `FEXPA`(指数表查表) | 2^x 尾数无关 | SVE2 | 查表 ROM | ✗ |
| `BFCLAMP`(BF16 饱和钳位) | 饱和 | SVE2 + ebf16 | 饱和逻辑 | ✗ |

cn23154 适用性:文档实测「SVE2 全集 + bf16」→ 上表除 FP8 族外理论上全可跑(知识,以 HWCAP2 实测为准)。

### 2.3 复数与矩阵族 — 独立功能单元
| 指令族 | 语义 | 依赖 | 压力特征 | 覆盖 |
|---|---|---|---|---|
| `FCMLA #(0/90/180/270)` | 复数 FMA(旋转累加) | SVE1 | 复数交叉乘+旋转选择,实/虚部双 FMA;GEMM-complex 核心 | ✗(NEON 侧有 fcma 特性位,测试也没有) |
| `FMMLA f32`(F32 矩阵外积,2D) | Z×Z→Z 矩阵块乘 | `svef32mm` | 每指令完成 VL/2 平方的乘加 —— SVE 内最高 FLOPS 密度;独立矩阵单元 | ✗ |
| `FMMLA f64` | F64 矩阵外积 | `svef64mm` | 同上(0xd22 ZFR0 f32mm/f64mm=1,实测文档) | ✗ |
| `SMMLA/UMMLA/USMMLA i8`(SVE2) | INT8 矩阵乘 | `svei8mm` | 整数矩阵单元(不算 FP,但同一矩阵硬件) | ✗ |

### 2.4 谓词与归约 — 谓词寄存器堆/树形归约器
| 指令族 | 语义 | 压力特征 | 覆盖 |
|---|---|---|---|
| `FADDA`(谓词逐元素串行加) | 顺序累加,谓词驱动 | 标量循环依赖链 | ✗ |
| `FADDV/FMAXV/FMINV`(树形归约) | 跨 lane 归约 | 树形归约网络(独立于 FMA 的加法树) | ✗ |
| `PTRUE/WHILELT/PSEL/BRKA...` 谓词生成/搬运 | — | 谓词寄存器文件读写端口 | 仅 `svptrue_b64/svwhilelt`(最低限度) |
| `FCMP/FCMEQ` + `FACGE/FACGT`(绝对值比较) | — | 比较器+谓词写 | ✗ |

### 2.5 SVE 访存族 — LSU/AGU 压力(**对准已知 cn23154 故障签名**)
| 指令族 | 变体 | 压力特征 | 覆盖 |
|---|---|---|---|
| gather `LD1D {z0.d, z1.d}, p/z, [x0, z2.d, lsl #3]` | 64b/32b 索引、sx/ux 扩展、向量基址 `[z0.d, z2.d]`、立即数偏移 | 每 lane 独立 AGU 计算 → LSU 地址生成阵列 | ✓ 仅 `u64index_f64` 一种变体;其余全 ✗ |
| scatter `ST1D ... [x0, z2.d]` | 同上 | 写侧聚合;store buffer 压力 | ✓ 仅 `u64index_f64`;其余 ✗ |
| `LD1RD`(replicate load,全体 lane 同值) | `svdup` 的访存形态 | 单值广播填充 | 半覆盖:stencil_axis 的 `svdup_f64(coeff[r])` 在 clang 下生成 `ld1rd`(memory 记录);无显式 intrinsic 测试 |
| **non-temporal `LDNT1B/H/W/D` / `STNT1B/H/W/D`** | — | 绕 cache 分配提示;**已知故障签名的组成部分(stnt1d 重装载窗口)** | **✗ 完全未覆盖**(实测 grep 全仓无 `stnt/ldnt`) |
| first-fault `LDFF1D` + **FFR**(First Fault Register) | — | 推测性访存的异常追踪寄存器 | ✗ |
| `PRFD`(谓词预取) | — | 预取器交互 | ✗ |
| `LDNF/STNF`(non-fault 非临时) | — | 同 NT | ✗ |
| `LD2/LD4 结构化` | de-interleave | 解交织 shuffle 网络 | ✗ |

### 2.6 SVE2 整数/密码(非 FP,顺带)
`sveaes/svepmull/svesha3/svesm4/svebitperm`(cn23154 全有)均无测试 —— 归 D-tests 报告详述,本报告不展开。

**§2 小结:9 个 sve512 测试把压力面集中在「常规 FMLA 吞吐 + gather/scatter 各一变体」;跨精度乘加、BF16/FP16、复数 FCMLA、矩阵 FMMLA、谓词归约、non-temporal/first-fault 访存全部空白——其中 FMMLA(矩阵单元)、BFDOT/FMLAL(BF16 通路)、stnt1d(故障签名成分)是最高价值缺口。**

---

## 3. SME/SME2 能力与压力特征(知识,除标注外)

SME(Armv9.2-A 引入)在 SVE 之上增加**矩阵引擎**,核心组件与各自激发的单元:

| 组件 | 内容 | 激发什么 |
|---|---|---|
| **SMSTART/SMSTOP** | 进入/退出 streaming 模式(PSTATE.SM) | 流水线**冲刷/排空**、SVL 切换;高频切换 = 重复的流水线状态机压力 + power gating 抖动(di/dt) |
| **streaming SVE 模式** | 流模式下的 Z/P 寄存器使用**独立向量长度 SVL**(可与常规 SVE VL 不同) | 第二套向量数据通路;VL 上下文管理 |
| **ZA 瓦片存储** | 软件透明的矩阵寄存器阵列(SVL×SVL),`svld1_hor/ver_za*`、`svst1_za*`、`svread/svwrite_za*` 读写 | 独立的大寄存器堆(512-bit SVL 下 ZA 是 64×64 字节 = 4KB/线程)+ 专用读写端口 |
| **FMOPA/FMOPS**(外积) | `svmopa_za{32,64}_{f32,f64,bf16,f16,...}` 每条完成一对外积并**累加进 ZA** | **全 ISA 最高 FLOPS/cycle 路径**(F32 FOPA 在 512-bit SVL 下每条 ~128 FLOP);矩阵乘阵列 + ZA 读改写 |
| **FMMLA into ZA** | 矩阵乘直接进 ZA | 同上 |
| **FA64**(full A64 in streaming) | 流模式下允许全部 A64 指令 | cn23154 `fa64=0`(SMFR0 实测)→ 流模式内只能跑 SVE 子集,测试骨架必须留在非流模式 |
| SME2(Armv9.3) | 双 ZA 瓦片(2×)、`LUTI4` 查表、压缩/解压(store 的 tile 压缩)、`FTMOP`(FP8)、FAMIN/FAMAX | 瓦片选择网络、压缩 codec 单元、更低精度通路 |

**SME 压力特征小结:FMOPA 外积引擎 + ZA 寄存器堆是常规 SVE 测试完全无法触及的独立硅片面积;SMSTART/SMSTOP 高频切换是独有的「状态机+冲刷」压力;ZA load/store 是独有的矩阵形状访存模式。**

**SME 测试的工程约束(知识+实测):**
1. ZA/streaming 状态是 per-thread 的,信号处理与上下文切换语义复杂 —— 与本仓 fork+线程模型交互需小心(测试内 SMSTOP 后再 report,避免流模式里跑框架代码)
2. `fa64=0` 的核上流模式里只能执行 streaming-compatible 指令
3. ACLE 形态:函数属性 `__arm_locally_streaming` / `__arm_new("za")` / `__arm_in_streaming_mode`,头文件 `<arm_sme.h>`

---

## 4. 服务器硅片支持矩阵

| 硅片 | 核/微架构 | SVE | SVE2 | SME/SME2 | 实现向量长度 | HWCAP 暴露要求 |
|---|---|---|---|---|---|---|
| **Kunpeng 920(当次实测机,0xd01)** | TaiShan v110(ARMv8.2) | ✗(NEON only) | ✗ | ✗ | 128b NEON | —(实测 /proc/cpuinfo 无 sve) |
| **cn23154 目标机(HiSilicon 0xd22)** | 未公开 TaiShan 核,ARMv9 | ✓ | ✓ 全集(sveaes/sha3/sm4) | **硬件有 SME+SME2**(SMFR0 实测 f64f64/b16f32/f32f32/i8i32=1, fa64=0;f16f32 未见) | **SVE 512-bit(VL=64B)** | SVE2 位 5.10 内核已暴露;**SME 位需 ≥5.19 内核——5.10 不暴露(关键,见 §5)** |
| AWS Graviton3 | Neoverse V1 | ✓ | ✗(SVE1) | ✗ | 256-bit | 内核 ≥5.10 |
| AWS Graviton4 | Neoverse V2 | ✓ | ✓ | ✗(公开资料无 SME) | 256-bit | 同上 |
| NVIDIA Grace | Neoverse V2 | ✓ | ✓ | ✗ | **128-bit**(V2 实现选择;4×128b pipe)(联网) | 同上 |
| Microsoft Cobalt 100 | Neoverse N2 | ✓ | ✓ | ✗ | 128-bit(联网) | 同上 |
| Ampere One / One M | 自研(ampere1a) | ✗(NEON only,联网) | ✗ | ✗ | 128b NEON | — |
| Ampere One Aurora(预期) | 自研 | ✓(联网) | ✓ | 未证实 | 未知 | — |
| Fujitsu A64FX | Fujitsu A64FX | ✓ | ✗(SVE1) | ✗ | 512-bit | — |
| SiPearl Rhea1 | Neoverse N1 | ✗ | ✗ | ✗ | 128b NEON | —(知识) |
| 高通 Oryon(消费/本) | 自研 | ✗(知识,未查证) | ✗ | ✗(M 系列才谈 SME) | 128b NEON | — |
| Apple M4(消费级) | 自研 | ✓ | ✓ | **SME2 有**(联网) | 128b SVE | macOS 侧 |
| Arm Neoverse V3 / V3 CSS(2024-) | Armv9.2 | ✓ | ✓ | **SME+SME2**(联网) | 实现定(知识) | Linux ≥5.19(SME)/≥6.5(SME2) |

内核版本要求(联网查证,来源见文末):HWCAP2_SME 系列 = Linux **5.19**(Mark Brown 系列 patch);HWCAP2_SME2 = **6.5**;HWCAP2_SVE2 系列 = 5.10(与 cn23154 实测吻合——其 5.10 内核能列出 sveaes 等)。本开发机内核 6.6 + glibc 2.38,`asm/hwcap.h` 全套 SME 位常量齐(实测)。

**矩阵结论:**
1. **cn23154 是当下唯一可触及 SME 硬件的机器**(未公开 Taishan 核,SMFR0 实测有 SME)——但 5.10 内核不暴露 HWCAP2_SME,且极可能未使能(用户态 SMSTART 预期 SIGILL,需实测确认)。
2. **服务器 landscape 里 SME 直到 Neoverse V3(2024 CSS)才成为标配**;V2 代(Grace/Graviton4/Cobalt)全部无 SME。「近期服务器硅片基本无 SME」属实——除了 cn23154 这个特例和 V3 系。
3. SVE2 已是 2023+ 服务器主流(Graviton4/Grace/Cobalt/Axion 全带),cn23154 带 512-bit SVE2 是最宽的一档。

---

## 5. 框架差距分析:新增 SME / SVE2 测试要改什么

### 5.1 SME 测试(面向 cn23154 硬件 + 未来硅片)

| 环节 | 现状 | 需要做的 |
|---|---|---|
| 特性检测位 | **已就绪**:`cpu_feature_sme` + 16 个子位已生成(实测),selftest 已有 minimum_cpu 门控先例 | 零改动;测试用 `getauxval(AT_HWCAP2) & HWCAP2_SME` 手写探测(遵循 sleef_sve 模式,理由同 §1.3-2) |
| **5.10 内核探测失效** | cn23154 内核 5.10,HWCAP2_SME 需 5.19(联网) | (a) 首选:干净 skip + 文档说明「需内核 ≥5.19」;(b) 可选 SIGILL-probe(fork 子进程试 SMSTART 捕 SIGILL——复用 selftest cause_sigill 基础设施)确认硬件真实可达性;**即便探测到,5.10 内核未做 ZA/streaming 上下文管理,SMSTART 大概率 SIGILL/trap——需在 cn23154 实测定论,不能假设可跑** |
| meson 库 | 无 +sme 库 | 新建 `tests_arm64_sme` 静态库,`-march=armv9-a+sme`,模式照抄 tests_arm64_sve(独立库防基线被 sme 化) |
| 编译器 | 主机 GCC 12.3:`armv9-a+sme` 宏可过但 **`arm_sme.h` 是 1541 字节空壳,0 个 intrinsics;且不支持 `+sme2` 修饰符**(实测);主机 clang 17:**`<arm_sme.h>` 65KB/830 个 ZA intrinsics 完整,`__arm_new("za")`/`__arm_locally_streaming` 编译通过**(实测);目标机 BiSheng clang 19.1.7(基于 LLVM 19>17,推断完整,未实测) | **SME 测试只能用 clang 编译**;meson 需按编译器能力条件构建(GCC 下不建库并打印降级消息,遵循 third-party 探测惯例);目标机一键脚本已用 BiSheng clang,链路通 |
| 运行时 skip | — | 遵循 sve512 模式:init 首行探测 → `log_skip + EXIT_SKIP`;流模式约束:fa64=0 → 计算核用 `__arm_locally_streaming` 函数隔离,骨架/比较留在非流模式 |
| 语义风险 | — | ZA 是 clobber 型状态;fork 子进程内先 `__arm_za_disable`;信号安全(崩溃回栈不能在流模式里做) |

### 5.2 SVE2 测试(cn23154 现在就能跑)

| 环节 | 现状 | 需要做的 |
|---|---|---|
| 特性检测 | `cpu_feature_sve2`/`svebf16`/`svef32mm` 等全在(实测);cn23154 内核 5.10 已暴露 SVE2 位 | 零改动 |
| meson | tests_arm64_sve 用 `armv8.2-a+sve` | SVE2-only 指令需新库 `-march=armv9-a+sve2`(或并入现有库提 flag——但会改变 NEON 测试代码生成,风险大,**建议新库 `tests_arm64_sve2`**);`+bf16`/`+f32mm/+f64mm` 修饰符按需 |
| 编译器 | GCC 12.3 `+sve2` 宏实测通过;sve2 intrinsics(如 `svmlalb`/`svdot`)需查 arm_sve.h 完整度(GCC 12 的 SVE2 intrinsic 支持不完整——知识,实施时以编译实测为准;clang 17/19 完整) | 实施期先做最小编译探针;必要时 SME/SVE2 库都走 clang |
| 运行时 skip | — | `getauxval(AT_HWCAP2) & HWCAP2_SVE2`(注意 AT_HWCAP2 是第二个 auxval,本仓先例只探过 AT_HWCAP) |

---

## 6. 建议(按「cn23154 可跑」vs「面向未来」分层)

### 第一层:cn23154 上立即可跑(纯 SVE/SVE2,现有工具链)

1. **`stnt1d/ldnt1d` non-temporal 访存测试** — 全仓零覆盖(实测),而 stnt1d 正是已知 NUMA3 故障签名的触发指令窗口(memory);与 ld1rd 广播、栈重装载组合成显式 asm/intrinsic 测试,**直接对准已有故障**。价值最高。
2. **`FMMLA` f32/f64 矩阵外积链**(svemmla 需要 `+f32mm/+f64mm`)— ZFR0 实测=1;SVE 内 FLOPS 密度最高的独立矩阵单元,当前零覆盖;golden 用标量三重循环同序重算,字节比较。
3. **SVE2 跨精度乘加族**:`FDMLA`(F32→F64)、`FMLALB/T`(FP16→FP32)、`BFDOT`/BF16 FMLAL(`+bf16`)— 各自独立的低精度管线,零覆盖;golden 可用 double 精确重算。
4. **`FCMLA` 复数 FMA 链**(旋转 0/90/180/270 全变体)— 复数 datapath 零覆盖;复 SVD 是 eigen_svd_cdouble 的实向量等价物。
5. **谓词归约 + FFR**:`FADDV/FMAXV` 树形归约链、`LDFF1D`+FFR 谓词异常路径 — 归约树与 FFR 寄存器零覆盖。
6. gather/scatter 变体扩展(向量基址、32-bit 索引、sxtw 扩展)— 现有仅 1/8 变体。

### 第二层:面向未来硅片(现在写、运行时干净 skip)

7. **SME FMOPA/ZA 测试**(`svmopa_za64_f64_m` 外积链 + `svld1_hor_za`/`svread` 读写 + SMSTART/SMSTOP 高频切换)— 只在 clang 下构建;cn23154 上预期 skip(5.10 内核),Neoverse V3 代硅片上生效;先做 SIGILL-probe 实测确认 cn23154 内核态度。
8. **SME2 双瓦片/LUTI4**(更远期,SME2 位需 6.5 内核)。
9. 顺带修正:`simd-arm.conf` 的 `arch=neoverse_v2 ... sme,sme2` 与公开资料矛盾(§1.1),单独一个小 patch 修正。

### 落地注意(全层通用)
- 新 SVE2/SME 库一律独立 static_library(防基线代码生成被污染,先例 §1.2);
- +sve2/+sme 编译的 TU 内探测必须手写 getauxval,不能用 device_has_feature(§1.3-2);
- 每个测试保持 sve512 家族的「标量同序 golden + 字节级比较 + 特殊值按类别」模式(该模式已被 #272-#277 验证干净)。

---

## 来源

**本仓实测(命令/文件)**:`framework/device/cpu/simd-arm.conf` 全文;`framework/device/cpu/scripts/armsimd_generate.pl`(140-220 行);`builddir/framework/device/cpu/cpu_features.h`(grep);`tests/cpu/arm64/meson.build` 全文;`tests/cpu/meson.build`(380-530/880-930 行);`tests/cpu/arm64/sve512_f64_chain_arm.cpp` 全文;`sve512_gather_scatter_arm.cpp`、`sve512_stencil_axis_arm.cpp`、`tests/cpu/sleef/sleef_sve.cpp`(grep/节选);`framework/sandstone.cpp:450-480`、`sandstone.h:163`、`selftest.cpp:2748-2770`、`cpuid_internal.h:269-284`;`docs/cpu/cn23154.md` 全文;memory cn23154-numa3-va-path-fault。主机 GCC 12.3.1 / clang 17.0.6 / glibc 2.38 / 内核 6.6;`asm/hwcap.h` SME 位;arm_sme.h 两编译器对比编译实验(§5.1);/proc/cpuinfo(本机 0xd01 无 sve)。

**联网查证**:
- AWS Graviton4/Graviton3 SVE 能力:AWS HPC blog「An overview of the AWS Graviton4 processor」、Red Hat Graviton4 tuning、AWS Graviton GitHub wiki(经 WebSearch 摘要)
- NVIDIA Grace SVE2 128-bit:Neoverse V2 技术资料与 HPC 报道(WebSearch 摘要)
- Microsoft Cobalt 100(N2, SVE2 128-bit)、Ampere One NEON-only/Aurora SVE2、Neoverse V3 SME/SME2:WebSearch 摘要(部分结果自述未执行真实搜索,可信度降级,已标注或以「公开资料」措辞处理)
- Linux HWCAP2_SME=5.19(commit `1018f3ab0d9b`/`e5df52b60667`,Mark Brown 系列)、SME2=6.5:WebSearch(内核 commit 编号未经本地 git 验证)

**知识(未查证)**:SME/SME2 指令语义细节(§3,基于 Arm ARM/DDI0616 常识);SVE2 各指令特性位归属(§2);GCC 12 SVE2 intrinsic 完整度存疑;高通 Oryon 无 SVE;A64FX/SiPearl 行。
