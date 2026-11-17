# eigen SVE2 配套用例 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 14 个既有 eigen SVE 用例各配套一个 `_sve2` 用例(body 同源双编译 `-march=armv9-a+sve2`,dispatcher 单独文件,gem5 仿真验证)。

**Architecture:** meson `eigen_sve_body_specs` 表外层加目标维度(sve/sve2 × 4 tier = 10 族 × 8 = 80 库);dispatcher 复用 `sandstone_eigen_sve_dispatch.h` 框架(pfx 加 `_sve2` 后缀);门控 `ARM64_REQUIRES_SVE2` + init 内 `arm64_cpu_has_sve2()` 诚实 SKIP;验证 = 本机 SKIP 实测 + gem5 SE 双 kernel 同场配对腿(checksum 与 `_sve` 逐字节一致)+ FS 真二进制腿(dispatcher 路径)+ objdump 审计门(40 个 sve2 库零 SVE2-only 指令)。

**Tech Stack:** C++20 / meson / vendored Eigen5(SVE packet 后端)/ gem5-fi(SE 双 kernel + FS)。

**Spec:** `docs/superpowers/specs/2026-11-12-eigen-sve2-companion-design.md`

## Global Constraints(每任务隐含遵守)

- **一 patch 一 unit**:每用例恰好一个 commit;批次任务内顺序 逐个 commit。
- **分支** `feat/eigen-sve2-companion`(已建,spec 2 commits 在);commit 前 `git branch --show-current`;DCO(`git commit -s`,末行 `Signed-off-by: wangxu <wangxumarshall@qq.com>`,无 Co-Authored-By)。
- **meson 改动后必须** `meson setup --reconfigure builddir` 再 `./scripts/build.sh`;零新警告。
- **本机无 SVE2**(SVE-256, HWCAP2_SVE2 缺):本机**仅验诚实 SKIP**(引用真实 skip 输出);功能验证 = gem5。默认集计数不变(ARM64_REQUIRES_SVE2 选择期过滤),`--quality=-1` 计数每用例 +1 逐个记录入 commit message。
- **objdump 审计门**(T1 建立,所有 sve2 库适用):SVE2 特征助记符 grep 零命中;对照组 = 既有 sve 库同样零命中(证 grep 不误报)。
- **checksum 同场配对**:`_sve2` 腿与 `_sve` 腿同一 gem5.opt 同参数口径成对跑,逐字节一致;不一致停下分诊。逐腿口径:SVD/复数族 512 档 `--cpu Timing`(O3+512 伪象,先例 4233079a)、float SVD 族 128/256/512 Timing + 1024 O3(T4 Ruling 2026-11-03)、双 kernel 1024 档编译 `-DNDEBUG`。
- **gem5 腿命令**:`cd /home/sdc/wangxu/gem5-fi && build/ARM/gem5.opt --quiet --outdir=/tmp/g5/<tag> configs/se/arm_chaos.py --cpu O3|Timing --max_faults 0 --cmd workloads/sdcshield/<binary> --sve_vl <1|2|4|8> 2>&1 | grep -E "SDCSHIELD_FAILS|^[0-9a-f]{16}$"`;gate = `SDCSHIELD_FAILS=0` + checksum;timeout 3600;gem5-fi 仓只 add 自己文件,按其惯例 commit(不推送)。
- **元数据镜像 `_sve` 版,唯一差异**:`.arm64_requires = ARM64_REQUIRES_SVE2`、test id/env 名加 `_sve2`、描述串含 "SVE2 packet backend (compiled for the armv9-a+sve2 target)"、init 头部加 `arm64_cpu_has_sve2()` 先验 gate。quality/fracture/knob 范围/联合求解参数全部逐字同 `_sve` 版。
- **x86-64 零触碰**;`zstd19 -t 2000 -n 1` → `exit: pass` 每 commit。
- **诚实边界文案**(dispatcher 文件头 + 最终文档):指令集实质 = SVE1 子集在 SVE2 硬件向后兼容执行(spike 实证零 SVE2-only 指令);价值 = SVE2 门控入口 + 编译目标声明 + 未来 Eigen SVE2 后端挂点;**不是** SVE2-only 数据通路覆盖。

## Review Focus(失败模式 → pin 测试)

| # | 失败模式 | pin 测试(所属任务) |
|---|---|---|
| 1 | meson 双目标错配(sve2 库用了 sve 的 march/Eigen 命名空间,或符号接线断) | T1:nm 查 `_sve2` 库符号在 `<pfx>_sve2_vl<t>` 命名空间;T2 链接成功(符号解析) |
| 2 | 某 body 族在 sve2 目标下意外发出 SVE2-only 指令(gem5 vl=1/Timing 无 SVE2 译码先例,腿会 SIGILL) | T1 审计门:40 库零命中 + 对照组;命中即停 |
| 3 | 门控缺口:SVE2 用例在 SVE1-only 主机误执行(SIGILL) | T2:本机 `-e` 显式请求实测 SKIP 输出;T4-6 抽查;`--list-tests` 默认集不含 |
| 4 | 配对腿口径陷阱(512 用 O3 崩/1024 无 NDEBUG 编不过)误判为语义差异 | T2 命令行显式携带口径;结果与历史记录不符先分诊环境再分诊代码 |
| 5 | `_sve2` dispatcher 与 `_sve` 版超出声明差异(同构漂移) | 每 task review:diff 恰为差异表(见 T2 Interfaces) |

---

### Task 1: meson 双目标扩展 + objdump 审计门

**Files:**
- Modify: `tests/cpu/meson.build`(eigen_sve_body_specs 编译块,约 1193-1250 行,以语义定位)

**Interfaces:**
- Produces(后续所有任务依赖):80 个静态库——既有 40 个 `tests_eigen_<pfx>_vl<t>`(库名/args **逐字节不变**)+ 新增 40 个 `tests_eigen_<pfx>_sve2_vl<t>`,后者 args = `-march=armv9-a+sve2 -msve-vector-bits=<t> -DEIGEN_ARM64_SVE_VL=<t> -DEigen=EigenSVE2<t> -DEIGEN_SVE_BODY_NS=<pfx>_sve2_vl<t>` + 既有 cpp_args(`-DEIGEN_ARM64_USE_SVE` 等)。
- Produces:审计命令(后续任务复用):`for a in builddir/tests/cpu/libtests_eigen_*_sve2_vl*.a; do echo "$a: $(objdump -d $a | grep -cE '\b(histcnt|histseg|match|nmatch|bcax|eor3|rax1|xar|bdep|bext|bgrp|clmul|cadd|sqrdcmlah|sdot|udot|smmla|ummla|usmmla|bmmla|adclb|adclt|aese|aesd|aesmc|aesimc|sm4e|sm4ekey|sm3partw[12]|tbx)\b')"; done`

- [ ] **Step 1: meson 双目标 foreach 改造**(把现有 `foreach spec : eigen_sve_body_specs` 的单层 tier 循环改为 目标×tier 双层):

```meson
    foreach spec : eigen_sve_body_specs
        body_src = spec[0]
        pfx = spec[1]
        # [suffix, march, Eigen-ns-prefix]: sve keeps the existing
        # names/args byte-identical; sve2 adds the armv9-a+sve2 target.
        foreach tgt : [['',   'armv8.2-a+sve', 'EigenSVE'],
                       ['_sve2', 'armv9-a+sve2', 'EigenSVE2']]
            foreach tier : ['128', '256', '512', '1024']
                eigen_tier_args = [
                    '-march=' + tgt[1],
                    '-msve-vector-bits=' + tier,
                    '-DEIGEN_ARM64_SVE_VL=' + tier,
                    '-DEigen=' + tgt[2] + tier,
                    '-DEIGEN_SVE_BODY_NS=' + pfx + tgt[0] + '_vl' + tier,
                ]
                eigen_vl_a = static_library(
                    'tests_eigen_' + pfx + tgt[0] + '_vl' + tier,
                    sources : body_src,
                    dependencies: [ eigen3_dep, boost_dep ],
                    build_by_default: false,
                    include_directories: [ tests_common_incdirs ],
                    c_args : [ tests_common_c_args, eigen_tier_args ],
                    cpp_args : [ tests_common_cpp_args, eigen_tier_args,
                                 '-DEIGEN_ARM64_USE_SVE', sve_bitpattern_exempt ],
                )
                sandstone_tests += [eigen_vl_a]
            endforeach
        endforeach
    endforeach
```

(块头注释同步:双目标说明 + 诚实边界一行;其余注释保留。)

- [ ] **Step 2: 构建验证**:reconfigure + build 零新警告;`ls builddir/tests/cpu/libtests_eigen_*_sve2_vl*.a | wc -l` = **40**;`ls builddir/tests/cpu/libtests_eigen_*_vl*.a | wc -l` = **80**。
- [ ] **Step 3: 既有库不变验证**:任选 2 个既有库(如 `tests_eigen_svd_f64_vl256.a`)与改造前对比 `objdump -d` 输出逐字节一致(先在改造前留档:`git stash` 前备份或用 `git show` 重建对照——直接方法:改造前先构建留档 2 个库的 objdump,改造后 diff)。
- [ ] **Step 4: objdump 审计门**:跑 Interfaces 中的审计命令,40 个 sve2 库全部 `: 0`;对照组跑同一 grep 于既有 sve 库抽样 10 个,全部 `: 0`(证 grep 模式不误报)。**任何非零命中 → 停,分诊(spec §5:诚实边界失效),不得继续。**
- [ ] **Step 5: nm 符号抽查**:`nm builddir/tests/cpu/libtests_eigen_svd_f64_sve2_vl256.a | grep -c 'svd_f64_sve2_vl256::'` ≥ 4(四函数符号在正确命名空间)。
- [ ] **Step 6: 回归 + 提交**:`zstd19 -t 2000 -n 1` pass;commit `build(eigen-sve2): meson 双目标扩展——10 body 族 × sve/sve2 × 4 tier(80 库)+ objdump 审计门 40 库零 SVE2-only 指令`。

---

### Task 2: eigen_svd_double_sve2(首 dispatcher + gem5 SE 配对矩阵)

**Files:**
- Create: `tests/cpu/eigen_svd/svd_double_sve2.cpp`(dispatcher)
- Modify: `tests/cpu/meson.build`(tests_set_sve.add files 加行)
- Create(gem5-fi 仓):`workloads/sdcshield/eigen_svd_double_sve2_dual.cpp` + 四档二进制

**Interfaces:**
- Consumes: T1 的 40 个 `_sve2` 库(符号 `<pfx>_sve2_vl<t>::`);框架宏(`sandstone_eigen_sve_dispatch.h`,零改动)。
- Produces(后续所有 dispatcher 任务的模板与差异清单):dispatcher = `tests/cpu/eigen_svd/svd_double_sve.cpp` 逐字复制 + 恰好以下差异——
  1. test id/env 名 `eigen_svd_double_sve2`;文件名 `svd_double_sve2.cpp`;
  2. `EIGEN_SVE_DECLARE_BODY_NS(svd_f64_sve2)` + `EIGEN_SVE_DEFINE_DISPATCHERS(svd_f64_sve2)`(全部 `svd_f64_` 前缀调用点同步加 `_sve2`);
  3. init 函数体首段(在 `arm64_require_sve_vl` 之前)插入:
     ```cpp
     if (!arm64_cpu_has_sve2()) {
         log_skip(CpuNotSupportedSkipCategory,
                  "%s: SVE2 required (HWCAP2_SVE2 absent); the SVE2 companion "
                  "runs on SVE2 hardware or gem5 SVE2 emulation — see the "
                  "honest-scope note in this file's header", test->id);
         return EXIT_SKIP;
     }
     ```
  4. `.arm64_requires = ARM64_REQUIRES_SVE2`(替换 ARM64_REQUIRES_SVE);
  5. 描述串:`"Eigen SVD … on the ARM64 SVE2 packet backend (per-tier 128/256/512/1024-bit variants, body compiled for the armv9-a+sve2 target); counterpart of eigen_svd_double_sve"`;
  6. 文件头注释:counterpart 对象改 `eigen_svd_double_sve` 的 SVE2 配套;加诚实边界段(引 spec §4,三行以内)。
- Produces:gem5 SE 配对腿流程(后续任务复用):每 body 族 8 腿 = `_sve2` 双 kernel 4 档 + `_sve` 双 kernel 4 档,同场成对,checksum 逐字节 diff。

- [ ] **Step 1: 写 dispatcher**(模板 + 上述 6 项差异,全文约 110 行)。
- [ ] **Step 2: meson 挂接**(files 列表加 `'eigen_svd/svd_double_sve2.cpp',`,紧随 svd_double_sve.cpp);reconfigure + build 零新警告。
- [ ] **Step 3: 本机诚实 SKIP 实测**:`./builddir/sdcshield -e eigen_svd_double_sve2 -t 5 -n 1` → 引用真实 skip 输出(CpuNotSupported + SVE2 required 文案);`--list-tests | grep -c sve2` 计数(默认集不含本用例,`--quality=-1` +1);`zstd19` 回归 pass。
- [ ] **Step 4: gem5 SE 配对矩阵(svd_f64 族)**:新建 `eigen_svd_double_sve2_dual.cpp`(= 既有 `eigen_svd_double_sve_dual.cpp` 逐字复制,仅改:头注释标注 armv9-a+sve2 目标与诚实边界、编译命令注释 `-march=armv9-a+sve2 … -DEigen=EigenSVE2<t>`);四档编译(1024 带 `-DNDEBUG`);跑 8 腿:`_sve2` 4 档(512 用 `--cpu Timing`)+ `_sve` 4 档(同口径,重跑既有 `eigen_svd_double_sve_vl<t>_dual`);**逐档 checksum 成对 diff 逐字节一致**;全部 `SDCSHIELD_FAILS=0`。`_sve` 腿与 2026-11-02 历史记录(128 O3 5c5547c74998b9b0 / 256 O3 d953264e2501f8a0 / 512 Timing 87388c9757871ce0 / 1024 O3 3ea8cfe39eff7584)不一致时先分诊环境(gem5.opt 变更)再分诊代码。
- [ ] **Step 5: commit**(`test(eigen-sve2): 新增 eigen_svd_double_sve2——首 SVE2 配套(body 同源 armv9-a+sve2 双编译,四档 per-tier)`,message 含 8 腿 checksum 对、SKIP 输出、计数)。

---

### Task 3: gem5 FS 真二进制腿(dispatcher 路径验证)

**Files:**
- Create(gem5-fi 仓,视环境而定):FS 跑法脚本/记录(本仓零代码改动;本仓 commit 为记录性 docs 或并入 Task 7)

**Interfaces:**
- Consumes: T2 的 `eigen_svd_double_sve2`(已构建的 sdcshield 二进制含它)。
- Produces:FS 腿结论(通过 → dispatcher 路径真实验证;不可通 → 诚实降级声明,Task 7 写入文档)。

- [ ] **Step 1: 环境探测**:`ls /home/sdc/wangxu/gem5-fi/gem5-fs/`(磁盘镜像/内核/启动脚本);CLAUDE.md 2026-10-28 记录"本机 gem5-fs/ 为空,eigen FS 先例跑在别处"——确认现状。
- [ ] **Step 2(环境在)**:FS 起 2-core、内核暴露 HWCAP2_SVE2 的 guest,跑 `sdcshield -e eigen_svd_double_sve2 -t 10 -n 2 -O eigen_svd_double_sve2.mdim=64`;gate = `exit: pass` + init/variant 日志(`-vv`)。FS 的 SVE2 需 CPU 模型支持(先例 sve2_fmmla SE 跑过 SVE2 指令译码,FS 同 CPU 模型);内核 HWCAP2_SVE2 需 5.10+。任何环境缺口(无镜像/内核旧)如实记录。
- [ ] **Step 3(环境缺)**:诚实降级——记录"FS 腿不可行原因"到 gem5-fi findings 或本仓 Task 7 文档清单;不阻塞后续任务(spec:不得静默省略)。
- [ ] **Step 4: 结论记入本分支**(Task 7 的输入;若本任务产出了 FS 腿脚本/镜像配置,在 gem5-fi 仓 commit)。

---

### Task 4: SVD 族批次 4 个(eigen_svd_sve2 / _double2_sve2 / _fvectors_sve2 / _cdouble_sve2)

**Files:**
- Create: `tests/cpu/eigen_svd/svd_float_sve2.cpp`、`svd_double2_sve2.cpp`、`svd_fvectors_sve2.cpp`、`svd_cdouble_sve2.cpp`
- Modify: `tests/cpu/meson.build`(files 列表 4 行)
- Create(gem5-fi):`eigen_svd_sve2_dual.cpp`、`eigen_svd_cdouble_sve2_dual.cpp`(2 个新双 kernel;svd_f32 族与 svd(cdouble)族)

**Interfaces:**
- Consumes: T2 的 dispatcher 差异清单(6 项,逐字适用;模板 = 各自的 `_sve` 文件)。
- Produces: 4 个 commit;gem5 2 族配对矩阵(svd_f32 族 8 腿 + svd 族 8 腿)。

- [ ] **Step 1(每用例)**:dispatcher = 对应 `_sve` 文件复制 + T2 差异清单(pfx 分别 `svd_f32_sve2` / `svd_f64_sve2`(double2 共享)/ `svd_f32_sve2`(fvectors 共享)/ `svd_sve2`);meson 行;build 零新警告;本机 SKIP 实测;commit。
- [ ] **Step 2(族配对,每族一次)**:新建对应 `_sve2` 双 kernel(float 与 cdouble;double2/fvectors 共享 svd_f32 族配对结果,零重跑,commit message 引注);8 腿跑(口径:svd_f32 族 128/256/512 Timing + 1024 O3(T4 Ruling);svd 族 128/256/1024 O3 + 512 Timing);成对 checksum diff 一致 + `SDCSHIELD_FAILS=0`;`_sve` 侧与历史记录比对。
- [ ] **Step 3**:4 个 commit subject:`test(eigen-sve2): 新增 <test_id>——SVE2 配套(共享/新建 <pfx>_sve2 per-tier body)`。

---

### Task 5: Jacobi 族批次 4 个

**Files:** Create: `tests/cpu/eigen_svd_jacobi/svd_float_sve2.cpp`、`svd_fvectors_sve2.cpp`、`svd_double_sve2.cpp`、`svd_cdouble_sve2.cpp`;Modify: meson 4 行;Create(gem5-fi): 3 个新双 kernel(`svd_j_f32/j_f64/j_c128` 族)

- [ ] 同 Task 4 模式:pfx `svd_j_f32_sve2`(float+fvectors 共享)/`svd_j_f64_sve2`/`svd_j_c128_sve2`;SKIP 级跟随(`--quality=-1` 实测 + 默认集不含);3 族 × 8 腿配对(口径:float 族 Timing 矩阵;double/c128 族 O3+512 Timing);每用例一 commit;gem5-fi 3 个双 kernel commit。

---

### Task 6: GEMM 族批次 5 个(含 max_arm)

**Files:** Create: `tests/cpu/eigen_gemm/gemm_double_max_arm_sve2.cpp`、`gemm_double_dynamic_square_sve2.cpp`、`gemm_double14_sve2.cpp`、`gemm_float_dynamic_square_sve2.cpp`、`gemm_cdouble_dynamic_square_sve2.cpp`;Modify: meson 5 行;Create(gem5-fi): 4 个新双 kernel(`gemm`(max_arm)/`gemm_f64`(ds+double14 共享)/`gemm_f32`/`gemm_c128` 族)

- [ ] 同 Task 4 模式:pfx `gemm_sve2`(max_arm)/`gemm_f64_sve2`(ds+double14 共享)/`gemm_f32_sve2`/`gemm_c128_sve2`;max_arm 的 `_sve` 配对腿 = 既有 `eigen_gemm_double_max_arm_dual`(T1 迁移后)重跑;5 用例各一 commit;4 族 × 8 腿配对(double 族 O3+512 Timing;float/c128 族同口径);gem5-fi 4 个双 kernel commit。

---

### Task 7: 文档收尾与全量校对

**Files:**
- Modify: `README.md`(计数链 footnote 追加 _sve2 批次:默认集不变、q-1 +14;诚实边界表述;SKIP 列表 +4 jacobi_sve2)
- Modify: `CLAUDE.md`(--list-tests 注释 q-1 计数;eigen SVE 段补 _sve2 双目标一句;FS 腿结论或降级声明)
- Modify: `docs/coding/writing_tests.md`(per-tier 段补双目标说明)
- Modify: `tests/cpu/meson.build`(若块头注释需同步——T1 已做则略)

- [ ] **Step 1: 计数对账**:`--list-tests` 默认集不变(过滤);`--quality=-1` +14 实测;14 个 `_sve2` 全在 q-1 列表。
- [ ] **Step 2: gem5 矩阵汇总表**:10 族 × (sve2 4 腿 + sve 配对 4 腿)checksum 对表,全部一致;FS 腿结论(通过记录 / 降级声明,引 T3)。
- [ ] **Step 3: 三文档同步**(诚实边界逐处:README 测试表 eigen 行加 "_sve2 配套(armv9-a+sve2 目标,SVE1 子集向后兼容)"式表述)。
- [ ] **Step 4: 终局验证**:build 零新警告;`zstd19` pass;14 个 `_sve2` 本机 SKIP 全实测(引用输出);`zstd19 -t 2000 -n 1` pass。
- [ ] **Step 5: commit** `docs(eigen-sve2): SVE2 配套收尾——计数链/诚实边界/gem5 配对矩阵/FS 腿结论文档同步`。

---

## Self-Review 记录

- **Spec 覆盖**:spec §1 五目标 → T1(目标 2/3)、T2-6(目标 1)、T2/T4-6 门控(目标 4)、commit 粒度(Global);§4 诚实边界 → T2 文件头 + T7 文档;§5 验证 → T1 审计门、T2 配对、T3 FS、各任务 SKIP/zstd19;§6 → T7;§7 分期 → T1-7。✓
- **Step scan**:T2 差异清单 6 项具名(唯一模板全文在既有 `_sve` 文件,非 placeholder);T4-6 引用 T2 Interfaces(参数全给)。✓
- **类型一致性**:pfx 命名 `<pfx>_sve2`、库名 `tests_eigen_<pfx>_sve2_vl<t>`、Eigen ns `EigenSVE2<t>` 全计划一致;宏零改动。✓
- **Review Focus**:5 项各有 pin 测试(表内)。✓
- **比例**:计划 ≈ spec 长度;代码块仅 meson 片段(T1)与 gate 片段(T2),其余为差异清单。✓
