# 审查 P0 整改方案（2026-10 review remediation）

> 分支：`fix/review-2026-10-p0-remediation`（自 main @ d6539e8e 切出；4ced912a..d6539e8e 新增仅 fracture_loop_count=0 显式化与 CI/文档，经核实未触及任何 P0 问题点，复核结论不变）
> 审查基线：`d2bde251`（review-report.md 全部发现基于此）；三态判定对照点：`4ced912a`（HEAD 复核时点）
> 本文是本分支全部修复工作的唯一权威方案；执行中发现与事实不符处，先修订本文再动代码。

## 0. 摘要与范围

2026-10 多代理对抗性审查产出 109 条净发现（Critical 11 / High 29 / Medium 32 / Low 24 / Info 13）。
对照 `d2bde251..HEAD`（103 提交）逐条复核：**P0 点名发现 41 条中 39 条完全仍存在、0 条已修复**；
唯一部分误报为 B1-001（7 文件中 2 个首轮校验实际可达）。假阴性链——
**冻结/固定输入 → no-op 或同源 golden 恒 pass → 脚本吞错 → 统计不分层 → "零 SDC 健康"**——每一环都原样在位。

本 PR 范围（P0 四批次 + 批次外补充）：

| 批次 | 主题 | 发现 | 文件数 |
|---|---|---|---|
| A | no-op / 恒 pass 逻辑缺陷 | B1-001*、B1-003、B1-004、B2-001、B3-005 | 25 |
| B | R2b 冻结输入 → 逐轮随机 | B2-003、B2-004~007、B1-015 | ≈39 |
| C | R1 自建 RNG → 收敛框架种子 | B1-002、B1-005、B1-006、B1-007、B1-010、B3-007、B3-008、B3-010、A-001 | ≈46 |
| D | 脚本吞错 + CI 随机性门禁 | D-003、D-006（含 O-5）、D-007、D-008、D-009、F-009、O-1 | ≈12 脚本 |

*B1-001 复核修正：仅 4 个 iter 屏障文件 + 1 个结构性互等文件（见 §2.2）。

不在本 PR 范围（后续批次，见 §3.5）：B1-008/009/011/012、B3-006、C 域部署安全项（C-001~003、D-001/002/004/005）、O-2/O-3/O-4。

## 1. 现状基线（Task 2 研究结论）

### 1.1 架构与生命周期
- sdcshield fork 自 Intel OpenDCDiag，移植 ARM64（Kunpeng 920）；x86-64 为不可改动的参考架构（CLAUDE.md 铁律 #5：只做加法式 `#elif __aarch64__`，绝不修改 x86 分支）。
- 框架主文件：`framework/sandstone.cpp` / `sandstone.h`（注意：不存在 framework.cpp）、`framework/sandstone_run.cpp`、`framework/random.cpp`。
- 测试发现机制：`DECLARE_TEST` 家族（sandstone.h L156-185）将测试描述符放入 `section("tests")`，链接器 `__start_tests`~`__stop_tests` 遍历注册。
- 执行模型：`main()`（sandstone.cpp）→ `random_init_global`（L1097 一次性）→ 每测试每线程 `random_init_thread`（sandstone_run.cpp L781）→ run() 内 `TEST_LOOP`；fracture 模式每轮 `random_advance_seed`（L1818/L1865）；exec 模式种子经 argv[3] 传递。

### 1.2 框架 RNG 机制（修复必须对齐的事实）
- 引擎：ARM64 为 AES 引擎（random.cpp L385-422，`#pragma GCC target("+crypto")`）或 LCG；引擎选择：`random_init_global`（L578-643）按参数（文件 / `Engine:state` 串 / 无参随机选）。
- 线程隔离：`rng_for_thread`（L118-126）——全局引擎索引 0，工作线程 1..N，各线程流独立。
- 框架 API：`random32()/random64()/frandom()/frandomf()/frandom_scale()/frandomf_scale()/memset_random()` 等；**`memset_random()` 才是框架缓冲填充 API**（不存在 random_buffer）。
- 回放：`random_format_seed()` 输出 `EngineName:hexstate`，经 `-s/--rng-state`（注意：不是 --seed）回传即可逐字节复现。
- 拦截：libc `srand()` 直接 abort；`std::random_device` 映射到框架引擎（mesh 移植曾利用此特性）。但测试自建 `std::mt19937` 状态机仍脱离框架每轮推进管理（种子只在构造时取一次，`random_advance_seed` 管不到它），失败日志也不具备框架 seed 语义——因此 R1 修复方向是**整体替换为框架 API**，而非仅换种子来源。
- R 分类：R0（框架 RNG，合格）/ R0i-R2b（init 一次冻结：High/Critical）/ R1（自建 mt19937(random_device)）/ R2a（全固定无随机）/ R3（固定但数据无关，可接受）。

### 1.3 测试模式
- `TEST_LOOP(test, N)`（sandstone.h L144-148）：双层 for——内层执行 N 次（N 为 2 的幂），外层 `test_loop_condition(N)` 判时间片退出。
- 数据模式：`init()`（每线程一次，生成数据）→ `run()`（TEST_LOOP 内反复执行 DUT+比对）。R2b 问题即 init 生成后 run 永久冻结复用。
- 正确范本：
  - **348ad1e0**（NEON ipsec 46 文件）：per-thread local struct + `TEST_LOOP` 内 `memset_random` 重掷 + 同轮 `compute_golden` 重算——批次 B 的对齐标杆。
  - **46141ed4**（eigen_gemm 非 dump 5 文件，基线前）：`kRerollEvery = 16` + `fill_rand()`（frandom_scale）每 16 轮重掷——大缓冲库用例的节流范本。
  - **2731971d**（NEON mesh）：sense-reversal barrier——B1-001 修复模板。
- 清理钩子名为 `test_cleanup`（非 test_teardown）；无 TEST_STRESS 宏。

### 1.4 双架构守卫惯例（两模式）
- 框架层：**加法式**——`#if defined(__x86_64__)` … `#elif defined(__aarch64__)` … `#else #error`（范本 random.cpp L44-50）；x86 分支一个字符都不动。
- 测试移植层：**占位式**——`#ifdef __aarch64__` 真实现 + `#else` 占位 stub；`DECLARE_TEST` 放守卫外全架构注册（范本 tests/cpu/sve/mesh/mesh_upi_sse_symm_int.cpp）。
- 诚实占位：EXIT_SKIP + reason 以 `"to be implemented (placeholder):"` 开头，且不得标 PROD。

### 1.5 构建体系
- meson；tests/cpu/arm64/meson.build 四库策略：`tests_arm64`（armv8.1-a+crc+crypto）、`tests_arm64_sve`（armv8.2-a+sve）、`tests_arm64_sve2`（armv9-a+sve2+…）、`tests_arm64_sme`；tests/cpu/sve/meson.build 承接 53 个 avx 命名测试的 SVE 移植。
- 修编译选项只动对应库的 meson.build；`ninja -C builddir` 必须零新告警。

## 2. 复核表（Task 4：d2bde251 → 4ced912a 三态判定）

### 2.1 判定汇总

| 域 | 发现 | 级别 | 判定 | 备注 |
|---|---|---|---|---|
| A | B1-001 SVE mesh 首轮屏障死锁 | Critical | **部分仍存在（5/7）+ 误报（2/7）** | 见 §2.2 |
| A | B1-003 gather/gatherscatter no-op | Critical | 仍存在（10/10） | DUT 与 golden 同一标量表达式；无任何向量指令 |
| A | B1-004 kreg 自比恒 pass | Critical | 仍存在（6/6） | `sw = hw` 直接赋值 / 同 C 表达式 |
| A | B2-001 zpclmul 假 CRC | Critical | 仍存在（2/2） | hw 路径 return zlib 结果；`vaddq_u8` 结果 (void) 丢弃 |
| A | B3-005 adox 同源 | Critical | 仍存在（2/2） | golden 与 DUT 逐行同构 `__int128` 循环 |
| B | B2-003 SVE ipsec 冻结 | Critical | 仍存在（24/24） | `memset_random` 仅在 init；TEST_LOOP 永久重放 |
| B | B2-004/005/006/007 库用例冻结 | High | 仍存在 | openblas/pocketfft/sleef(2)/eigen dump(3)；非 dump eigen 5 文件基线前已 R0 合格（46141ed4） |
| B | B1-015 stencil/bidiag 全固定 | High* | 仍存在（2/2） | *级别口径三处不一（findings Medium / 主报告 Critical），本方案按 High 处理 |
| C | B1-002 SVE mesh mt19937 | High | 仍存在（18/18） | 基线后 3 个 sum-bit 提交未触及 RNG |
| C | B1-005 SVE fma mt19937 | High | 仍存在（11/11） | 10 fma + 1 fma2；AVX 侧 31cbfc40 修复未及 SVE |
| C | B1-006 GMP randseed | High | 仍存在（4/4） | `gmp_randseed_ui(rng_state, std::random_device{}())` |
| C | B1-007 f32 指数常量 | High | 仍存在（2/2） | `0x3FEU` 应为 `0x7EU`；一行修复 |
| C | B1-010 insert_extract 竞态 | High | 仍存在（RNG 子项基线前已修 0445a218） | 共享 mem_buf 跨线程踩踏 |
| C | B3-007 memory 8 文件 mt19937 | High | 仍存在（8/8） | memcpy*/vmovnt*；另有 105 处 fprintf 残留（非本 PR） |
| C | B3-008 load_port mt19937 | High | 仍存在 | |
| C | B3-010 isal_crc32_gzip mt19937 | High | 仍存在 | |
| C | A-001 perf_event_open pid 误用 | High | 仍存在 | **位于双架构共享层** `framework/device/cpu/sysdeps/linux/effective_cpu_freq.hpp:147`，非 x86-only，可修 |
| D | D-003 spectrum 零检测 | Critical | 仍存在 | 硬编码 BIN 路径 / 无 set -e / 空 glob 零循环仍 exit 0 |
| D | D-006 大小写+删日志 | High | 仍存在 | `grep -q "result: FAIL"` 永不匹配小写 `result: fail`（logging.cpp L2171）；删通过日志家族实为 9 处（O-5） |
| D | D-007 sdc_detect 无 --fatal-skips | High | 仍存在 | 冒烟名单含 R2b/R1 用例 |
| D | D-008 生产无 -vvv | High | 仍存在 | 种子/loop-count 不落盘 |
| D | D-009 hunt 脚本注释与实现不符 | Medium→建议 High | 仍存在 | 注释称传 --fatal-skips 实未传；无 SDC 时删日志 |
| D | F-009 CI 随机性门禁缺失 | High | 仍存在 | PR 模板零 RNG 项；verify-params.py 丢弃种子字段 |
| D | **O-1 offline-build 零执行仍 PASS**（复核新发现） | 建议 High | 仍存在 | `verify-built-pristine.sh` L89-104 `\|\| true` 吞退出码：崩溃/零执行 → FAILS=0 → VERDICT=PASS；`run-full-tests.sh` L118-134 同款 |

基线后 103 提交中**零条 P0 发现被修复**；SVE mesh 域 3 个新提交（0d040a30/4b56f8cc/5e8e2d6d）仅添加失败冷路径诊断——且在 4 个死锁文件中这些诊断本身不可达（死代码）。

### 2.2 B1-001 修正（对报告的唯一实质性推翻）
- **误报 2 文件**：`mesh_upi_sve_asymm_write_int.cpp`、`mesh_upi_sve_distrib_write_int.cpp`——多写者 `fetch_add` 连续写满 128 块（带偏移），读者等 `allocated_blocks >= NUM_BLOCKS` 可满足，首轮校验可达且语义正确。作者在 556d54e3 commit message 中也仅自认 4 个文件。
- **仍存在 5 文件**：
  - 4 个 iter 屏障文件（symm_int / sym / wide_sym / asymm_int）：首轮 `current_iter=0` 使 `iter == current_iter` 恒真，全员自旋，校验不可达；
  - `asymm_distrib_int`：机制与报告描述不同——非轮次屏障，而是"单写者每写 1 块即回外层等重置 vs 读者等满 128 块"的结构性互等；结论（vacuous pass）不变。
- 修复影响面：B1-001 按 5 文件整改，非 7。

### 2.3 报告质量结论
- 交叉核对（C-001 证据、D-004 计数 5/7、D-007 14 槽结构）全部一致，**无失实引用**；
- 需修正：B1-001 清单 7→5、B1-009 对 ooo_dep_chain_sve.cpp 的机制描述（实为固定种子 LCG + VT 异或表）、B1-015 级别口径统一、A-001 补注双架构层；
- Grep 普查：tests/ 下 mt19937/gmp 构造用法 70 文件，与报告 R1 统计精确吻合，无文件级重大遗漏。

## 3. 修复清单（文件级）

### 3.1 批次 A——no-op / 恒 pass（Task 6）

| # | 发现 | 文件 | 修法 |
|---|---|---|---|
| A1 | B1-003 | tests/cpu/gather/ 全部 10 文件：gather_{i32,i64,f32,f64}.cpp、gatherscatter_{i32,i64,f32,f64}.cpp、gatherscatter{ps,pd}.cpp（执行期勘误：无独立 gatherscatter/ 目录，10 文件均在 gather/ 下；套系移植新增于 9e06e31e，输入 RNG 已于 0445a218 换为框架 API） | 执行期勘误：10 文件编入 tests_set_base 双架构共享库（无 SVE/NEON 专属 flags），非 SVE 库，`ld1gather` 不可用 → 取 NEON 路线（方案原选项 b）：aarch64 DUT 接 NEON 逐 lane gather/scatter（`vld1q_lane_*`→`vmulq_n_*`/`vaddq_*` 向量计算→`vst1q_lane_*`，同库 load_port.cpp 先例），golden 独立标量实现；非 aarch64 按 load_port 先例诚实 skip（CpuNotSupported + "to be implemented (placeholder):"，消除 x86 侧 vacuous pass，铁律 #6）；输入生成保持现状（已为框架 RNG 每轮重掷，R0 合格）；同步更新 tests/cpu/meson.build L617-621 过时注释（"std::mt19937 / no arm_neon.h" 与修复后事实不符） |
| A2 | B1-004 | tests/cpu/sve/kreg/kreg{2,3,5,6,8,9}.cpp（6） | golden 换独立计算路径（消除 `sw = hw` 与同表达式自比）；kreg2/9 的恒真断言改为真实交叉验证 |
| A3 | B2-001 | tests/cpu/sve/zpclmul/zpclmul.cpp、zpclmul_rep.cpp（2） | hw 路径用 PMULL/PMULL2 实现 CRC-32 真多项式折叠；golden 保留 zlib `crc32`；NEON 中间结果参与折叠而非 (void) 丢弃 |
| A4 | B3-005 | tests/cpu/arithmetic/adox.cpp、adcx_adox_interleaved.cpp（2） | golden 换独立实现（如分段累加/不同进位序），与 DUT `__int128` 链交叉验证；输入框架 RNG 重掷（与批次 C 联动，本提交内一并去 mt19937） |
| A5 | B1-001 | 4 个 iter 屏障文件：mesh_upi_sve_{symm_int,sym,wide_sym,asymm_int}.cpp；另 mesh_upi_sve_asymm_distrib_int.cpp（5） | 前 4 个移植 2731971d sense-reversal barrier；distrib_int 重构写者节流（写满 128 块后再等待重置），消除互等；顺带去 mt19937（B1-002 联动） |

### 3.2 批次 B——R2b 冻结 → 逐轮随机（Task 7）

| # | 发现 | 文件 | 修法 |
|---|---|---|---|
| B1 | B2-003 | tests/cpu/sve/ipsec/ipsec_*_sve.cpp（24） | 严格套用 348ad1e0 模板：per-thread local struct + TEST_LOOP 内 `memset_random` 重掷 key/iv/plaintext + 同轮 `compute_golden` 重算 |
| B2 | B2-004 | tests/cpu/openblas_gemm/dgemm.cpp | `kRerollEvery=16` 节流重掷 + 重算 golden（46141ed4 模式） |
| B3 | B2-005 | tests/cpu/pocketfft/fft.cpp | 同上 |
| B4 | B2-006 | tests/cpu/sleef/sleef_sve.cpp、sleef_neon.cpp | 同上 |
| B5 | B2-007 | tests/cpu/eigen_gemm/gemm_{double,float,cdouble}_dynamic_square_dump.cpp（3） | 对齐同目录非 dump 5 文件的 46141ed4 模式（kRerollEvery + fill_rand） |
| B6 | B1-015 | tests/cpu/arm64/sve512_stencil_axis_arm.cpp、tests/cpu/sve/eigen/eigen_svd_bidiag_sve.cpp | 输入/系数改框架 RNG 每轮重掷；bidiag golden 换独立实现（现与 DUT 同函数自比）；无法独立实现 golden 的显式降级（非 PROD + R3 论证） |

### 3.3 批次 C——R1 收敛框架 RNG + 批次外补充（Task 8）

| # | 发现 | 文件 | 修法 |
|---|---|---|---|
| C1 | B1-002 | tests/cpu/sve/mesh/ 18 文件（含 A5 的 5 个） | 删 `std::mt19937(std::random_device{}())`，改 `random32/random64/frandom*/memset_random`；与 A5 同批文件在 A5 提交内一并处理 |
| C2 | B1-005 | tests/cpu/sve/fma/ 10 文件 + fma2/fma_sve.cpp | 同上 |
| C3 | B1-006 | tests/cpu/arithmetic_arm/ 4 文件 | `gmp_randseed_ui` 种子改 `random64()` 派生 |
| C4 | B3-007 | tests/cpu/memory/ 8 文件（memcpy1、memcpy_shuffle_v2、vmovnt1{,_vex,2,2_vex,3,3_vex}） | mt19937 → 框架 API |
| C5 | B3-008 | tests/cpu/load_port.cpp | 同上 |
| C6 | B3-010 | tests/cpu/isal_crc32_gzip.cpp | 同上 |
| C7 | B1-007 | tests/cpu/arm64/sve512_f32_chain_arm.cpp:173、sve512_f32_chain_svd.cpp:150 | `0x3FEU → 0x7EU` 一行 + 更正注释（f32 偏置） |
| C8 | B1-010 | tests/cpu/vector/insert_extract.cpp | per-thread 数据副本（对齐 SVE 版栈缓冲做法），消除共享 mem_buf 竞态 |
| C9 | A-001 | framework/device/cpu/sysdeps/linux/effective_cpu_freq.hpp:147 | `perf_event_open(&pe, pid, cpu, ...)` 参数序修正：按 CPU 采集应传 `(-1, cpu_number)`；当前 cpu_number 占 pid 位导致 fd 恒失败。位于双架构共享层，不触碰 x86 专属逻辑 |

### 3.4 批次 D——脚本 fail-loud + CI 门禁（Task 9）

| # | 发现 | 文件 | 修法 |
|---|---|---|---|
| D1 | D-003 | scripts/run/run_sdc_spectrum.sh | `set -euo pipefail`；BIN 存在性检查；空 glob 防护（`shopt -s nullglob` + 计数为零 → 非零退出）；零检测记录时 VERDICT≠PASS |
| D2 | D-006+O-5 | run_all_cores_test.sh、run_groups_final.sh + 删日志家族（run_18_cycles.sh:48、run_core_rotation_stress.sh:52、run_adjacent_cores_stress.sh:65、run_all_cores_fixed_v2.sh:67、run_groups.sh:85） | 失败匹配改大小写不敏感 `grep -Eqi 'result: *fail'`；只删通过日志、失败/异常日志一律保留 |
| D3 | D-007 | scripts/run/sdc_detect.sh | exec 行追加 `--fatal-skips`（skip 即失败，冒烟名单含 R2b/R1 用例，修复后逐步移出） |
| D4 | D-008 | scripts/sdc-excite-reproduce/sdc-excite-reproduce.sh | 生产驱动 FLAGS 固定追加 `-vvv`（种子与 per-thread loop-count 落盘） |
| D5 | D-009 | scripts/run/run-sdc-hunt-30min.sh | 命令实际追加 `--fatal-skips`（兑现注释）；无 SDC 时保留日志不删 |
| D6 | F-009 | 新增 scripts/gha/check-randomization.py + .github/PULL_REQUEST_TEMPLATE.md + scripts/gha/verify-params.py | CI 随机性门禁：扫描默认 PROD 用例源码，必须含框架 RNG 调用或 `R3-justified:` 论证注释；PR 模板加自验项；verify-params.py 补提取种子字段 |
| D7 | O-1 | scripts/offline-build/verify-built-pristine.sh、run-full-tests.sh | 去 `\|\| true`；`exit: unknown`/零执行/崩溃均判 FAIL 非零退出 |

### 3.5 非本 PR 范围（后续批次，记录在案）
- B1-008（sve/arithmetic 5 固定种子）、B1-009（4 表轮转）、B1-011（7 文件 init 注入，含报告遗漏的 fsu_byteexact_sve.cpp）、B1-012（fpu special 2）、B3-006（movbe 2）、sve/arithmetic 6 个 random_device 变体与 sve/kreg+vector2 10 文件（R1 补录）——量大且模式与 C 批次相同，机械替换可另 PR 快速跟进；
- C 域部署安全（C-001/002/003、D-001/002/004/005）——涉及部署形态决策（systemd User=、目录权限模型），需与运维侧对齐后单独整改；
- O-2（benchmark 静默降级）、O-3（run_cluster_anchored.sh）、O-4（set -e 横向 17 文件）——O-3/O-4 随 D 批次模式顺手可带，但为控制本 PR 审查面，列后续；
- fprintf 调试残留（105 处/9 文件）——独立清理 PR。

## 4. 技术实现路径

1. **顺序**：A → B → C → D（先恢复"检测真实存在"的语义，再补随机性与门禁；C 依赖 A/B 中同文件的先导改动，如 mesh 先修屏障再换 RNG 则在同一提交内完成）。
2. **每提交一个修复单元**（CLAUDE.md 铁律 #1）：同文件多问题（如 mesh 屏障+mt19937）合并为一个单元提交，提交信息说明覆盖的发现编号。
3. **golden 独立性原则**：DUT 与 golden 必须走不同实现路径（指令集 vs 标量/库参考、不同算法序）；不确定时用已知答案测试（KAT）双锁。
4. **节流原则**：大缓冲（≥1MB）用例每轮全量重掷会显著降吞吐，统一 `kRerollEvery = 16` 模式（46141ed4 范本）；小缓冲每轮重掷。
5. **框架 API 优先序**：`memset_random`（缓冲）> `random64/random32`（标量）> `frandom*_scale`（浮点定标）。
6. **x86 保护**：所有 framework 改动 diff 必须目检确认 x86 分支零字符变化；测试文件确认 `#ifdef __aarch64__` 守卫内改动。
7. **降级诚实**：SVE/SVE2 特性在目标机不可用路径 → EXIT_SKIP + `"to be implemented (placeholder):"` 前缀 + 非 PROD。

## 5. 预期改进效果

- **批次 A**：25 个 PROD 用例从 vacuous pass（永不可能 fail）变为真实检测；SVE mesh 校验路径首次可达（连带激活 3 个新提交加的失败诊断）。
- **批次 B**：≈39 个用例每轮/每 16 轮换新输入，冻结盲区消除；失败场景可经 `-s` 精确重放。
- **批次 C**：≈46 个文件数据生成纳入框架种子管理（每线程流隔离、`-s` 回放、失败日志带 seed 语义）；B1-007 修正后 f32 指数域才真正覆盖规格范围；B1-010 消除跨线程数据竞态（假阳性噪声源）；A-001 恢复频率监控真实数据。
- **批次 D**：脚本病态场景（BIN 缺失/空清单/崩溃/零执行/小写 fail）全部非零退出且不打印 PASS；失败日志强制保留；生产日志可复现；CI 门禁使"无随机 PROD 用例"无法再合入。
- **净效果**：假阴性链四环中前三环（恒 pass、冻结输入、脚本吞错）在本 PR 内闭合；第四环（统计分层，C-003）留待后续——但 D 批次门禁已使其难以隐性累积。

## 6. 风险与缓解

| 风险 | 等级 | 缓解 |
|---|---|---|
| 独立 golden 本身有 bug → 修复后用例假阳性 fail | 高 | 每项提交必做 KAT 双锁 + sense-reversal 反向验证（篡改 DUT 必须 fail、不篡改必须稳定 pass 跑 ≥3 轮） |
| SVE ipsec 24 文件批量改动引入遗漏/错改 | 高 | 严格机械套用 348ad1e0 模板；每文件独立验证；`--list-tests` 计数对照 |
| mesh 屏障重构引入新死锁或数据竞争 | 高 | 移植 2731971d 已验证模式；首轮推进注入验证（人为设 2 轮短跑确认校验路径可达） |
| 每轮重掷拖慢吞吐（t 2000 回归超时） | 中 | kRerollEvery 节流；回归前后记录单测耗时对比，劣化 >20% 需回查 |
| zpclmul PMULL 重写算错多项式 | 中 | 与 zlib crc32 全量交叉 + 标准测试向量（"123456789" → 0xCBF43926）KAT |
| 脚本改动破坏现有 CI/现场流程 | 中 | 每脚本红队场景实测（空 glob/BIN 缺失/小写 fail 三构造样例）；正常路径冒烟不回归 |
| CI 随机性门禁误伤合法 R3 用例 | 中 | R3 论证注释白名单机制；门禁自身用构造违规样例验证（可拦截/可放行双向测试） |
| A-001 perf 修正改变监控行为 | 低 | 修正后 fd 成功路径才有输出，属恢复设计意图；验证读数非 UINT64_MAX |

## 7. 验证设计

每个提交单元（CLAUDE.md 铁律 #2 逐条落实）：
1. `ninja -C builddir` 零新告警；
2. 真跑受影响用例（引用真实输出行）；
3. **sense-reversal**：篡改 DUT 表达式/输入 → 必须 fail（修复前恒 pass 的用例这是决定性证明）；
4. 回归：`-e zstd19 -t 2000 -n 1` → `exit: pass`；
5. x86 目检：diff 无 x86 分支变化。

批次级：
- **B 批次重放验证**：抽取 ≥3 用例，记录失败日志 seed → `-s EngineName:hexstate` 重放 → 字节级复现；
- **D 批次红队**：构造 BIN 缺失 / 空 list / 空 glob / 零非 skip 用例 / 崩溃（SIGSEGV）/ 小写 fail 六类样例，逐一确认非零退出且不打印 PASS；门禁脚本用"无 RNG 无论证 PROD 样例"确认拦截、"含 RNG 样例"确认放行；
- **--list-tests 计数对照**：修复前后 PROD/SKIP 变化逐条有据（降级项列明理由）；
- **mesh 首轮可达性**：短跑（限 2 轮）确认 4+1 文件校验路径执行（`--list-tests`/verbose 日志证据）；
- **汇总**：`-e zstd19` 全量回归 + 抽验 R0 用例重放 + x86 非回归目检汇总表（进 PR 描述）。

## 8. 提交策略

- 分支：`fix/review-2026-10-p0-remediation`；本文档为分支首个 commit。
- 提交粒度：A1-A5 / B1-B6 / C1-C9 / D1-D7 各为一个或多个单一修复单元提交，提交信息含发现编号（如 `tests(gather): fix no-op DUT in 10 gather/gatherscatter tests (B1-003)`）。
- `git commit -s`（DCO，Signed-off-by: wangxu <wangxumarshall@qq.com>），无 Co-Authored-By。
- 全程不触碰用户未跟踪文件（docs/superpowers/reviews/、specs/、run_sdc_supplement.sh、sdc-detect-*.yaml/txt 等）。
- 终态：推送分支 → 创建 PR，描述含"审查发现 ↔ 修复"对照表与全部验证证据。
