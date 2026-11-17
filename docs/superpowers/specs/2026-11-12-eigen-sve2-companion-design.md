# eigen 系列 SVE2 配套用例 — 设计文档

日期:2026-11-12。前置:feat/eigen-sve-unification(2026-11-03 merge `b12d9ada`)
——12 个 SVE 用例 + `sandstone_eigen_sve_dispatch.h` per-tier dispatcher 框架
+ meson `eigen_sve_body_specs` 表驱动编译块,全部先例与裁决直接复用。
用户决策:方案 A(body 同源双编译,dispatcher 单独文件,编译选项配套
`+sve2`);范围 = 每个 eigen SVE 用例(14 个)配套一个 `_sve2` 变体。

## 1. 目标与非目标

**目标**:
1. 14 个既有 eigen SVE 用例各配套一个 `_sve2` 用例(单独 dispatcher
   文件 `<test_id>_sve2.cpp`,test id `<test_id>_sve2`);
2. 编译选项配套 SVE2:per-tier body 以 `-march=armv9-a+sve2` 目标再编
   译 4 档(128/256/512/1024),Eigen 命名空间 `EigenSVE2<t>`,符号
   命名空间 `<pfx>_sve2_vl<t>`;
3. body 源文件**零新增**(方案 A:同一份 body 源,meson 双目标编译)
   ——"尽可能不要单独实现"的字面最优;
4. 门控 `ARM64_REQUIRES_SVE2`(选择期 spec 过滤)+ init 内
   `arm64_cpu_has_sve2()` 诚实 SKIP;
5. 每用例一个 commit(规则 1)。

**非目标(如实声明)**:
- SVE2-only 数据通路覆盖——由 `sve2_cross_precision_arm` 等 SVE2 内核
  用例承担,本设计不涉及(见 §4 诚实边界);
- 修改 vendored Eigen(其 SVE 后端无 SVE2 路径,零触碰);
- sparse/dump/noavx512(SVE 轮已实证排除,理由不变);
- SME/SME2。

## 2. 事实基线(2026-11-12 本会话 spike 实证)

| 事实 | 证据 |
|---|---|
| Eigen SVE 后端纯 SVE1 intrinsics,零 SVE2 引用 | grep third-party/eigen5/.../arch/SVE/ 零命中 |
| `+sve2` 目标编译 Eigen FP 负载与 `+sve` 反汇编完全一致 | 同一 GEMM 负载三目标(armv8.2-a+sve / armv8.4-a+sve2 / armv9-a+sve2,-msve-vector-bits=256):唯一助记符集合相同、总指令数相同(4268=4268)、SVE2 特征指令(histcnt/match/bcax/eor3/sdot/smmla/cadd/clmul 等)0 条 |
| `-msve-vector-bits` 定宽机制在 +sve2 下正常 | 同上 spike 编译通过 |
| `ARM64_REQUIRES_SVE2`(1u<<1)+ spec 过滤 + VL pin 链已存在 | framework/sandstone.h:358;arm64_spec_prepare.h:54-67(SVE2 与 SVE 同走 VL pin);`arm64_cpu_has_sve2()`(arm64_vl.h:111) |
| 既有 SVE2 用例门控先例 | tests/cpu/arm64/sve2_cross_precision_arm.cpp:HWCAP2_SVE2 检查 + ARM64_REQUIRES_SVE2 |
| gem5 SVE2 仿真先例 | gem5-fi workloads/sdcshield/sve2_fmmla_dual.c:`-march=armv9-a+sve2+...` 编译,`--sve_vl {1,2,4,8}` 跑 |
| 本机无 SVE2(HWCAP2_SVE2 缺,SVE-256) | 历次实测(CLAUDE.md 本机记录) |

## 3. 设计(方案 A:body 同源双编译)

### 3.1 meson 双目标扩展

现有 `eigen_sve_body_specs` 表(body 源 × pfx)外层加目标维度:

```meson
foreach spec : eigen_sve_body_specs          # [body_src, pfx] 不变
    foreach tgt : [['sve', 'armv8.2-a+sve', ''],           # 既有,行为零变化
                   ['sve2', 'armv9-a+sve2', '_sve2']]      # 新增
        # march = tgt[1];Eigen 命名空间 EigenSVE<t> 或 EigenSVE2<t>;
        # 符号命名空间 <pfx>[_sve2]_vl<t>;库名 tests_eigen_<pfx>[_sve2]_vl<t>
```

(既有库名/命名空间**逐字节不变**——sve 目标的 args 与现状相同;
sve2 目标仅多 march 差异与 `_sve2` 后缀。)

### 3.2 dispatcher(14 个新文件,单独文件)

每用例一个 `<原名>_sve2.cpp`,结构 = 对应 `_sve` dispatcher 逐字复制 +
以下差异(全量参数化,模板 = 既有 14 个 `_sve` 文件):
- test id / env 名:`<test_id>_sve2`;
- `EIGEN_SVE_DECLARE_BODY_NS(<pfx>_sve2)` + `EIGEN_SVE_DEFINE_DISPATCHERS(<pfx>_sve2)`;
- init 门控:`arm64_cpu_has_sve2()` 先验(false → `log_skip`
  CpuNotSupportedSkipCategory,理由 "SVE2 required …")+ 既有
  `arm64_require_sve_vl(test, 128, 1024)` 与 `tier_select` 链不变
  (SVE2 是 SVE 超集,VL 语义相同);
- `.arm64_requires = ARM64_REQUIRES_SVE2`(唯一元数据差异,其余
  quality/fracture/knob 范围/联合求解参数全部镜像 `_sve` 版);
- 描述串 `…SVE2 packet backend (compiled for the armv9-a+sve2 target)…`;
- 文件头注释含诚实边界摘要(引 spec §4)。

框架头 `sandstone_eigen_sve_dispatch.h` **零改动**(pfx 参数化天然支持)。

### 3.3 清单(14 dispatcher,每件一 commit)

`eigen_svd_cdouble_sve2`、`eigen_gemm_double_max_arm` 的 `_sve2` 版
(既有 2 个 SVE 用例的配套)+ 上轮 12 个 `_sve` 用例的配套:
`eigen_svd{,_double,_double2,_fvectors}_sve2`、
`eigen_svd_jacobi{,_double,_fvectors,_cdouble}_sve2`(SKIP 级跟随)、
`eigen_gemm_{float,cdouble,double}_dynamic_square_sve2`、
`eigen_gemm_double14_sve2`。

## 4. 诚实边界(placeholder honesty 同款待遇)

SVE2 变体的指令集实质 = **SVE1 子集在 SVE2 硬件上向后兼容执行**
(spike 实证:`+sve2` 目标下 Eigen FP 负载零 SVE2-only 指令)。其价值:
① SVE2 硬件上的门控入口(默认集按 `has_sve2` 过滤后仍完整);
② 编译目标声明(未来 Eigen 若加 SVE2 后端路径,重建即得);
③ 独立注册文件可单独观察。dispatcher 文件头注释明示此边界;
README/CLAUDE.md 文档同步时如实表述,不得写成"SVE2 数据通路覆盖"。

## 5. 验证策略(规则 2;本机无 SVE2 = gem5-fi 条款最严格场景)

每 commit:
- `meson setup --reconfigure builddir && ./scripts/build.sh` 零新警告;
- 本机:**诚实 SKIP 路径实测**(`-e <test_id>_sve2` 引用真实 skip
  输出;ARM64_REQUIRES_SVE2 选择期过滤使默认集不含它,`--list-tests`
  计数不变、`--quality=-1` 计数 +14 逐个累加记录);
- **gem5-fi 四档**(功能验证主体):双 kernel 源按
  `-march=armv9-a+sve2 -msve-vector-bits=<t> -DEIGEN_ARM64_SVE_VL=<t>
  -DEigen=EigenSVE2<t>` 编译,`arm_chaos.py --sve_vl {1,2,4,8}` 四腿,
  gate = `SDCSHIELD_FAILS=0` + checksum;
- **checksum 互证(同场配对,my call)**:同源不同目标 → gem5 上
  `_sve2` 腿 checksum 与 `_sve` 腿**同场配对重跑**逐字节一致(不依赖
  2026-11-02/03 的历史腿记录——其记录散落、且跨会话 gem5.opt 二进制
  可能漂移;每用例的配对腿在同一 gem5.opt、同一参数口径下成对跑,
  `_sve` 腿重跑结果若与历史记录不一致须先分诊)。逐腿口径陷阱显式
  载入:SVD/复数族 512 档 `--cpu Timing`(O3+512 伪象,先例
  4233079a)、float SVD 族 O3 伪象矩阵用 Timing 腿矩阵(T4 Ruling)、
  1024 档编译 `-DNDEBUG`。不一致即编译目标引入语义差异,停下分诊;
- **objdump 审计门(my call,§2 spike 推广为全量验收)**:P1 的 meson
  双目标编译后,对全部 **40 个新静态库**(10 body 族 × 4 tier ×
  sve2 目标)做 SVE2 特征助记符 grep 审计(histcnt/match/nmatch/bcax/
  eor3/rax1/xar/bdep/bext/bgrp/clmul/cadd/sqrdcmlah/sdot/udot/smmla/
  ummla/usmmla/bmmla/adclb/adclt/aes*/sm4*/sm3*/tbx 等),零命中才
  通过;若有命中,§4 诚实边界表述失效、gem5 SVE2 译码无先例(vl=1 与
  Timing CPU 均未跑过 SVE2 指令),必须停下分诊;
- **gem5 FS 真二进制腿(用户决策:无真机,仿真验证;2026-11-12)**:
  dispatcher 自身的执行路径(init 门控链 / tier_select / 命名空间接线
  / run 分派)在 SE 双 kernel 与本机 SKIP 之外**不被执行**——P1 增加
  gem5 FS 全系统腿(先例:FS 2-core `eigen_svd_double -n 2` pass,
  CLAUDE.md 2026-10-24 记载):至少一条 `_sve2` 真测试(首选
  `eigen_svd_double_sve2`,小 mdim、`-n 2`)在 FS 下全链跑通,内核
  HWCAP2_SVE2 暴露、init→tier_select→worker pin→run 分派真实执行,
  gate = `exit: pass`。FS 腿是 dispatcher 路径的唯一真实验证手段;
  若 FS 环境(内核 HWCAP2_SVE2 / SVE2 CPU 模型)不可通,诚实降级为
  边界声明(§4 记录"dispatcher 路径未经真实验证,正确性由与 _sve 版
  逐字同构 + 链接期符号解析论证"),不得静默省略;
- `zstd19 -t 2000 -n 1` 回归;x86-64 零触碰目检(全 aarch64 guard);
- **P1 meson 验收口径(勘误)**:全部 40 个 `_sve2` 库编译成功零新
  警告 + 上述 objdump 审计全量 40 库零命中 + 首个用例族的 4+4 库
  (sve/sve2 × 4 tier)符号接线经链接验证;既有 sve 目标 40 库的
  库名/命名空间/args 逐字节不变。

## 6. 文档同步(规则 7)

README(计数链 footnote 追加 _sve2 批次 + 诚实边界表述)、CLAUDE.md
(--list-tests 注释、eigen SVE 段落补 _sve2 表述)、
docs/coding/writing_tests.md(per-tier 段补双目标说明)。

## 7. 分期(每任务一 commit)

P1:meson 双目标扩展 + 首个 dispatcher(`eigen_svd_double_sve2`,
连带 gem5 双 kernel 首建与四档矩阵跑通)+ 文档骨架;
P2-13:其余 13 个 dispatcher(gemm 族与 jacobi 族可按 SVE 轮的批次
模式合并派发,commit 仍每用例一个);
P14:文档收尾与全量校对(计数对账、SKIP 全清单、gem5 矩阵汇总表)。
