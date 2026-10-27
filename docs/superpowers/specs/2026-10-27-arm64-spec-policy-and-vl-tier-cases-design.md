# ARM64 规格策略与 VL 分档用例 设计

**日期**: 2026-10-27
**状态**: 待评审
**依赖**: all-VL Phase 2（已并入 main b0c01188）

---

## 1. 背景与目标

### 1.1 用户诉求（原话要点）

1. 重构 SVE / SVE2 / SME / SME2 的检测用例，使用例**支持所有规格**；
2. 128 / 256 规格的用例**在本机验证**；512 / 1024 / 2048 的用例**经 `../gem5-fi` 验证**；
3. sdcshield 测试时**探测芯片规格**，默认使用「当前 CPU 支持的最高规格」的 SVE / SVE2 / SME / SME2 用例。

### 1.2 已确认的范围裁剪（用户决定）

- **2048-bit：只预留，不实现。**
- **SME2：只预留（探测位 + 标签），不新增用例。**
- 验证规则改为：**本机能跑的在本机跑，跑不了的全部走 gem5。**

### 1.3 本设计要解决的问题

all-VL Phase 2 已把绝大多数 SVE 用例改成真 VLA（编译期不再锁 VL）。因此本设计**不是重写用例**，而是补三块真实缺口：

- **缺口 A**：框架没有"探测芯片规格并按规格选用例"的机制（`device_features` 只有 sve/sve2 位，**没有 VL 档位概念**）。
- **缺口 B**：用例没有"我要求哪个 ISA 扩展 / 哪个最低档"的元数据，无法在选择期按规格过滤。
- **缺口 C**：512 / 1024 档缺可复现的运行证据；Eigen 两例仍是 128 档独占。

---

## 2. 术语

| 术语 | 含义 |
|---|---|
| **spec** | 本机 ARM64 向量规格：{has_sve, has_sve2, has_sme, has_sme2, max_sve_vl_bits, max_sme_svl_bits} |
| **档 / tier** | 向量长度档位。128 / 256 / 512 / 1024-bit（2048 预留） |
| **最高档** | spec.max_sve_vl_bits（SVE 侧）/ spec.max_sme_svl_bits（SME 侧） |
| **T1–T6** | 用例分型，见 §4.5 |

沿用既有行话：文档层已定型 "VL/SVL tier（档）"、"supported / absent tier"、"highest supported tier"（见 CLAUDE.md 规则 2 与 `docs/superpowers/plans/2026-10-26-host-vector-capability-probe-rule.md`）。**"all-VL" 是 Phase 代号，不作为本设计的属性名。**

---

## 3. 现状（2026-10-27 实测基线）

### 3.1 本机（当次开发机）

探测命令与结果：

```
MIDR                    = 0x00000000480fd020        (HiSilicon part 0xd02)
HWCAP_SVE               = 1
HWCAP2_SVE2             = 0
HWCAP2_SME / SME2       = 0 / 0
/proc/cpuinfo Features  = sve, svebf16, svef32mm, svef64mm, svei8mm（无 sve2 / sme）
PR_SVE_GET_VL 默认      = 32 B = 256 bit（= 本机最大）
PR_SVE_SET_VL 请求 128/256/512/1024/2048/4096 → 实际 128 / 256 / 256 / 256 / 256 / 256
```

**结论**：本机只有 SVE1，最高档 256-bit；SVE2 / SME / SME2 全部不可用。

### 3.2 用例盘点（main b0c01188）

```
--list-tests | wc -l                                  → 483 个注册用例
--list-tests | grep -icE 'sve|sme'                    → 192 个名字含 sve/sme
git grep -l DECLARE_TEST -- tests/cpu/sve tests/cpu/arm64 | wc -l → 206 个源文件
```

### 3.3 编译期 VL 绑定现状

```
git show main:tests/cpu/meson.build | grep -n 'msve-vector-bits'
  → 1031: '-msve-vector-bits=128'     ← 全仓唯一一处
```

- `march_sve`（`:1029-1031`）只喂给 **`tests_set_sve`**；其成员**仅 Eigen 两例**。
- `tests_set_sve_vla`、`tests_sve_avx53`、`tests_sve_avx53_ipsec`、`tests_sve_crt` 一律只有 `-march=armv8.2-a+sve`（**无** vector-bits）。
- `tests/cpu/arm64/` 的全部 SVE 用例亦无 vector-bits。

**即：除 Eigen 两例外，全部向量用例已是真 VLA。**

### 3.4 gem5-fi 现状

```
configs/se/arm_chaos.py:50  --sve_vl  choices=[1,2,4,8]   (128/256/512/1024-bit)
configs/se/arm_chaos.py:53  --sme_vl  choices=[1,2,4,8]
```

**sdcshield 框架二进制在 gem5-fi SE 下不可运行**，三道独立阻断（`gem5-fi/findings.md:131-137`，本设计复核）：

1. pre-main page fault（三种入参同 tick 同地址，未达 `main()`）；
2. `prctl` 在 ARM64 SE 表中无 handler（`CHAOS/gem5/src/arch/arm/linux/se_workload.cc:679`）→ `fatal`；而框架每个 worker 线程都调 `prctl(PR_SET_NAME)`（`framework/sysdeps/linux/cpu_affinity.cpp:20`）；
3. 信号 syscall 被忽略 → 崩溃通道无保真。

**项目既有的 gem5 验证物是"从用例提取的同源内核"**（`workloads/sdcshield/*_dual.c`，静态链接，`arm_chaos.py --cmd <kernel> --sve_vl N`）。

---

## 4. 设计

### 4.1 探测层：`struct arm64_spec`

新增于 `framework/device/cpu/arm64/arm64_vl.h`（与既有 VL API 同族，复用其 getauxval/`.inst` 基础设施）：

```c
struct arm64_spec {
    bool has_sve, has_sve2, has_sme, has_sme2;
    long max_sve_vl_bits;    /* 本机 SVE 支持的最大 VL（bit）；无 SVE 为 0 */
    long max_sme_svl_bits;   /* 本机 SME 最大 SVL（bit）；无 SME 为 0 */
    bool max_vl_known;       /* false = prctl 全被拒，max_* 已回落当前 VL */
};

const struct arm64_spec *arm64_spec_get(void);   /* 进程内探测一次并缓存 */
```

**「最大支持档」的求法**（当前档 ≠ 最大档：当前档可能被
`/proc/sys/abi/sve_default_vector_length` 调低）：

1. 在**一次性临时线程**里执行探测（`PR_SVE_SET_VL` 是逐线程语义，临时线程退出即无副作用）；
2. 请求架构上限 2048-bit；内核向下取整授予本机真实最大值；
3. 用 `.inst` 编码的 `arm64_sve_vl_bytes()` 读回（不用 `svcntb()`——在 `-msve-vector-bits` 编译单元里会被常量折叠，Eigen 两例已踩过此坑）；
4. SME 侧同理，用 `PR_SME_SET_VL` + `rdsvl`；无 SME 则 `max_sme_svl_bits = 0`，不做请求。

**回落语义**：若第 2 步的前置 `PR_SVE_GET_VL` 或第 3 步全被拒（容器 seccomp、gem5 SE），则
`max_sve_vl_bits = 当前线程 VL`、`max_vl_known = false`，并在 dump/日志中**显式标注"最大值不可知，已回落"**，绝不谎报。

### 4.2 用例元数据

`struct test`（`framework/sandstone.h`）在 `compiler_minimum_device` 旁新增：

```c
uint8_t arm64_requires;     /* bit0=SVE bit1=SVE2 bit2=SME bit3=SME2；0 = 无向量要求 */
long    arm64_min_vl_bits;  /* 用例要求的最低档（bit）；0 = 任意档 */
```

`DECLARE_TEST` 增加两个链式设置项，风格与既有 `.minimum_cpu` 一致：

```c
DECLARE_TEST(sve512_f64_chain_arm, "...")
    .arm64_requires = ARM64_REQUIRES_SVE,
    .arm64_min_vl_bits = 0,
```

非 aarch64 构建中这两个字段恒为 0（由宏在 `#if defined(__aarch64__)` 外给默认值），**x86-64 行为零改动**（规则 5）。

### 4.3 选择期过滤

**落点**：`SandstoneTestSet::SandstoneTestSet` 的 `enable_all_tests` 分支
（`framework/sandstone_tests.cpp:89-98`，与 quality / optional 并列）。

```c
/* 伪代码 */
const struct arm64_spec *s = arm64_spec_get();
if (test.arm64_requires && !spec_satisfies(s, &test))   /* 扩展缺失 */
    continue;
if (test.arm64_min_vl_bits && s->max_sve_vl_bits < test.arm64_min_vl_bits)
    continue;
```

**显式优先**：`-e <name>` 与 `--test-list-file` 命中的用例**不参与过滤** ——
它们照常注册，运行时如实 SKIP。理由：显式点名是用户的明确意图，隐藏它会让排障变难。

**`--list-tests` 反映过滤后的集合**（默认即"本机可跑的用例"）。

### 4.4 worker 线程按最高档拉起

**位置（as-built，评审修正）**：必须在 **控制线程**（`test_init` 的 aux 线程与
per-CPU worker 的创建者）调用 `test_init` **之前** pin 一次，再在每个 worker 的
`test_run` 之前 pin 并读回校验。只 pin worker 会在"内核默认 VL < 芯片最高档"
（§4.1 的存在前提）时让 init 按默认档冻结 golden/缓冲区、worker 按最高档运行，
触发全部 init/run 漂移守卫（无守卫的用例则可能越界）。控制线程的 pin 是
best-effort（静默），权威判定仍在 worker：

```
# 控制线程，test_init 之前（子线程继承此 VL）
arm64_spec_pin_control_thread(test)      # 静默；失败留给 worker 判定

# 每个 worker，test_run 之前
if (test->arm64_requires & (SVE|SVE2)):
    target = spec->max_sve_vl_bits
    rc = arm64_sve_pin_vl(target/8)
    if rc < 0:                        → SKIP (CpuNotSupported, "无法 pin 到 N 档")
    actual = arm64_sve_vl_bytes()*8
    if actual != target:              → FAIL (不变式破坏)
同理 SME/SME2 用 max_sme_svl_bits + PR_SME_SET_VL + rdsvl
```

**Eigen 两例的覆盖**：它们要求 SVE，控制线程先 pin 到最高档（init 按同一档
分配变体）；随后 Eigen 的分派 TU 在 worker 上再 pin/读回并选择对应变体（§4.7），
幂等确认，语义不变。

### 4.5 用例分型

| 型 | 家族 | 现状机制 | 本设计要求 |
|---|---|---|---|
| **T1 真 VLA·零代码改动** | `tests/cpu/sve/*` 绝大多数（arithmetic 16、case 51、fma 10、fma2 8、kreg 11、mem 9、mesh 18、vector2 5、ipsec HMAC 17、sleef、sve_rot×5）+ `tests/cpu/arm64/` 的 sve512 多数 + stencil / fcmla / nt_reload / pred_ops / fpcr_cartesian / power_virus_dit_sve | 无 `-msve-vector-bits`；几何由 `svcnt*` / `svwhilelt` / `rdvl` 运行期取得 | 只挂 `arm64_requires = SVE` + 补 512/1024 证据 |
| **T2 几何写死** | **仅** `tests/cpu/arm64/sve512_xlswave_ctx_arm.cpp`（`B_TILE=16` 常量 + `n+8` 硬偏移） | VLA，但几何按 512 写死 | 几何寄存器化（照 `sve512_stencil_axis_arm` 的 `bt = 2*svcntd()` 范式） |
| **T3 tier-locked·多档分派** | **仅** `tests/cpu/eigen_gemm/gemm_double_max_arm.cpp`、`tests/cpu/eigen_svd/svd_cdouble_sve.cpp` | `-msve-vector-bits=128` + `test_run` 自 pin 128 | 同源编 4 档变体 + 运行时函数指针分派（§4.7） |
| **T4 扩展专属** | SVE2：`sve512_fmmla_arm`、`sve2_cross_precision_arm`；SME：`sme_za_tileview_arm`、`sme_fmopa_stress_arm`、`sme_fmopa_za_arm` | VLA / 手写 asm | 挂 `arm64_requires = SVE2 / SME`；本机缺能力→选择期过滤；证据走 gem5 |
| **T5 预留** | SME2（0 用例）、2048-bit（无档位） | — | 只保留探测位 / 标签 / 文档，不实现 |
| **T6 非向量** | 其余约 291（= 483 − 192；LSU / OoO / NEON / 标量 / 加密…） | 标量或 NEON | `arm64_requires = 0`，不受过滤影响 |

**判断依据**：§3.2 实测的 **192 个**向量用例（注册名含 `sve`/`sme`）中，真正需要改代码的
只有 **3 个**（T2 一个 + T3 两个）；其余 **189 个**"支持所有规格"已是既成事实，
缺的是标签与证据。

### 4.6 T2：xlswave 几何寄存器化

`tests/cpu/arm64/sve512_xlswave_ctx_arm.cpp` 现状把 Phase A 的 tile 宽度写成常量
（`B_TILE = 16`）并按 512-bit 的 8-lane 偏移取第二次 `svld1`。改法照
`sve512_stencil_axis_arm.cpp` 已验证的范式：

```
const size_t bt = 2 * svcntd();          /* 运行期 tile 行宽 */
按 bt 重新推导 n / result / result+lanes 的偏移与 Phase A 标量 golden
```

Phase A 的 golden 必须保持 **init 期冻结**语义（PR #283 评审 finding A）：
冻结时按"init 期算出的 bt"计算，run 期只读；并在 run 侧做 bt 漂移 fail-closed
（与 `ARM64_SVE_VL_DRIFT_CHECK_F64` 同款）。

### 4.7 T3：Eigen 四档变体 + 函数指针分派

**为什么不能 VLA**（已两次 spike 确证）：Eigen 的 SVE packet 用
`arm_sve_vector_bits(EIGEN_ARM64_SVE_VL)` 定宽，且该定宽向量是 `PacketXcd` 的
**struct 成员**，`NumTraits` 又要 `sizeof` —— sizeless 类型两者都做不到。
（`third-party/eigen5/Eigen/src/Core/arch/SVE/PacketMath.h:673`、`.../SVE/Complex.h:30`）

**做法**：同源编 4 份，互不冲突，运行时按实际档位取函数指针。

```
meson:
  for N in [128, 256, 512, 1024]:
      static_library('eigengemm_vl@N@', <同一份 .cpp>,
          cpp_args: march + ['-msve-vector-bits=@N@',
                             '-DEIGEN_ARM64_SVE_VL=@N@',
                             '-DEigen=EigenSVE@N@',
                             '-DGEMM_TEST_NAMESPACE=gemm_vl@N@'])
```

源码结构（两个文件同构）：

```cpp
namespace GEMM_TEST_NAMESPACE {          /* 由 -D 注入，四份互不冲突 */
  int run_body(struct test *, int cpu);  /* 原来的 test_run 主体 */
}
/* 分派 TU（无 -msve-vector-bits） */
static int gemm_dispatch(struct test *t, int cpu) {
    long want = arm64_sve_vl_bytes() * 8;      /* 框架已 pin 到最高档 */
    switch (want) {
      case 128:  return gemm_vl128::run_body(t, cpu);
      case 256:  return gemm_vl256::run_body(t, cpu);
      case 512:  return gemm_vl512::run_body(t, cpu);
      case 1024: return gemm_vl1024::run_body(t, cpu);
      default:   log_skip(CpuNotSupported, "未编译 %ld 档变体", want);
                 return EXIT_SKIP;
    }
}
```

**每个变体仍需自 pin 校验**：分派前在 `run_body` 内调用
`arm64_sve_pin_vl(N/8)` 并读回校验，保证"选的变体档位 == 实际执行档位"。
框架已 pin 到最高档，此处是幂等的二次确认（防止最高档 pin 被环境降级）。

### 4.8 验证矩阵

| 档位 | 谁验证 | 具体物 |
|---|---|---|
| **128-bit** | 本机原生 | 真实二进制跑 T1/T2（`vlrun 16`）+ T3 的 128 变体 |
| **256-bit** | 本机原生 | 真实二进制按"本机最高档 = 256"跑 T1/T2 + T3 的 256 变体（多档分派首次生效） |
| **512-bit** | gem5-fi | T1/T2 → 既有 dual kernel；T3 → **新增逐档 Eigen 独立内核**（`-msve-vector-bits=512`，`--sve_vl 4`） |
| **1024-bit** | gem5-fi | 同上（`-msve-vector-bits=1024`，`--sve_vl 8`） |
| **2048-bit** | — | 预留不实现 |
| **SVE2** | gem5-fi | **新增 fmmla/bf16 内核**（`--sve_vl` 四档中本机缺的档位） |
| **SME** | gem5-fi | 既有 `sme_fmopa_dual` / `sme_za_tileview_dual`（`--sme_vl 4/8`） |
| **SME2** | — | 预留不实现 |

gem5 内核清单（复用既有，位于 gem5-fi `workloads/sdcshield/` 及各 feature 分支）：
`sve512_chain_dual`、`sve512_gather_dual`、`sve512_fma_dual`、`sve_arith_bounds_dual`、
`sleef_vla_dual`、`sve_rot_dual`(`sve_rot_vla_dual`)、`stencil_axis_dual`、`agu_2src_dual`、
`mrn_reload_rmw_dual`、`sha`/`hmac` 系列、`sme_fmopa_dual`、`sme_za_tileview_dual`。

**新增**：`eigen_gemm_vl{512,1024}_dual`、`eigen_svd_vl{512,1024}_dual`、`sve2_fmmla_dual`。

### 4.9 可观测

- `--dump-cpu-info` 增打印（`framework/device/cpu/cpu_device.cpp`）：

```
ARM64 spec: SVE yes (max VL 256 bits) | SVE2 no | SME no | SME2 no
```
  若 `max_vl_known == false`，该行改为 `max VL unknown (fell back to 256 bits)`。

- 运行日志在每个 SVE/SVE2/SME 用例开始时记录一行：
```
[spec] <test-id>: 本机最高档 256 → 实际 pin 256 (SVE)
```

---

## 5. 失败模式与语义

| 情形 | 语义 |
|---|---|
| pin 被环境拒绝（容器 seccomp / gem5 SE 的 prctl） | 该用例 **SKIP**（`CpuNotSupported`），原因写"无法 pin 到 N 档" |
| pin 返回成功但**读回值 ≠ 请求值** | **FAIL**（`EXIT_FAILURE`）——不变式破坏，不放行 |
| 探测拿不到最大档（prctl 全被拒） | 回落当前线程 VL，`max_vl_known=false`，dump/日志显式标注 |
| T3 没有当前档的变体 | **SKIP**，原因写"未编译 N 档变体" |
| `-e` / `--test-list-file` 显式点名本机不支持的用例 | **不过滤**，照常注册并运行时如实 SKIP |
| 非 aarch64 / 非 Linux | spec 全 false，过滤不发生，行为与今天一致 |

---

## 6. 非目标

1. **2048-bit**：只预留（常量、探测上限、文档），不编译、不验证。
2. **SME2 用例**：只预留探测位与标签，不新增用例。
3. **让 sdcshield 二进制在 gem5 SE 中运行**：三道墙（pre-main page fault / prctl / 信号）
   属独立的 gem5 SE 开发，不在本设计范围。
4. **逐文件 gem5 内核**：T1 家族只覆盖"有 dual 内核的代表性家族"，不做逐文件。

---

## 7. 实施拆解（一补丁一单元）

| # | 单元 | 动执行路径? | 独立可回退 |
|---|---|---|---|
| U1 | `arm64_spec` 探测层 + `--dump-cpu-info` + `selftest_arm64_spec` | 否 | 是 |
| U2 | `struct test` 两个新字段 + `DECLARE_TEST` 支持 + 自检 | 否 | 是 |
| U3 | 选择期过滤（`SandstoneTestSet` ctor）+ `-e`/list-file 旁路 + `--list-tests` 反映 | 是 | 是 |
| U4 | worker 线程按最高档拉起 + 读回校验 | 是 | 是 |
| U5 | 204 个向量用例挂 `arm64_requires` 标签（按家族） | 否 | 是 |
| U6 | T2：xlswave 几何寄存器化 | 是（单文件） | 是 |
| U7 | T3：Eigen 四档变体 + 函数指针分派 | 是（2 文件 + meson） | 是 |
| U8 | gem5 验证物（Eigen 逐档内核 + SVE2 fmmla 内核）+ 四档矩阵 | 否 | 是 |
| U9 | 文档同步（README / CLAUDE.md / docs + 本 spec 的诚实边界） | 否 | 是 |

顺序保证：U1–U5 任一步中断，仓库仍可用（U3/U4 之前行为与今天一致）。

---

## 8. 验收标准

1. **探测正确**：本机 `--dump-cpu-info` 报 `SVE yes (max VL 256 bits) | SVE2 no | SME no | SME2 no`，与 §3.1 实测一致。
2. **过滤正确**：默认集合中不含 SVE2/SME 用例；`-e sve512_fmmla_arm` 仍注册且 SKIP。
3. **最高档生效**：SVE 用例日志显示"本机最高档 256 → 实际 pin 256"；构造的 selftest 断言 worker 内读回 == spec.max。
4. **T2**：xlswave 在 128/256 本机 pass，gem5 512/1024 `SDCSHIELD_FAILS=0`。
5. **T3**：本机 128/256 两变体各自 pass 且分派选中正确变体；gem5 512/1024 逐档内核 `FAILS=0`。
6. **回归**：每单元 `./scripts/build.sh` 零新警告；`-e zstd19 -t 2000 -n 1` → `exit: pass`。
7. **x86-64**：新字段/宏在非 aarch64 恒 0，代码零改动。

---

## 9. 诚实边界

1. gem5 侧验证的是**同源内核 / 同源 Eigen 变体**，不是框架二进制本体。
2. T1 的 204 个用例里，gem5 只覆盖**有 dual 内核的代表性家族**，不是逐文件。
3. 本机只能验证 **SVE 的 128/256**；SVE2/SME 的全部档位、以及 T3 的 512/1024 变体，
   以 gem5 为**唯一**证据。
4. `max_vl_known=false` 的环境里，"最高档"是回落值，日志会如实标注。
5. 2048-bit 与 SME2 无任何执行证据（按用户决定预留）。
