# SDC 测试随机化加固方案（操作数数值 × 地址空间变异）—— 2026-09-20 核验定稿版

- **日期**：2026-09-19 初稿 / **2026-09-20 全量核验定稿**
- **状态**：核验完成、待用户批准（方案，未实施）
- **核验方法**：2026-09-20 对 09-19 草案的全部 19 项关键断言做源码级核验 + 本机实测（底稿 `.planning/2026-09-20-sdc-randomization-hardening/findings.md`，逐条 file:line 证据）。**17 项证实 / 2 项修正**。
- **背景**：122 号核心 SDC 狩猎任务——122 的故障数据通路未知（INT/FP/SIMD/内存），操作数 + 地址空间的双维随机化正是"映射故障面"的工具。

## 0. 相对 09-19 草案的修正（核验结论）

| 项 | 草案说法 | 核验结果 |
|---|---|---|
| jit/zero_control_vec_* | ARM64 假 pass | **修正**：init 已诚实 `EXIT_SKIP`（实测 `-v` 显示 `result: skip`）；run 内 `#if !x86 → EXIT_SUCCESS` 是不可达死代码。从第一梯队移除，并入 P3 顺手清理 |
| openssl_sha 竞技场 | "1.2KB 竞技场" | **修正加重**：`arena_size` 字段是 **uint8_t 类型**（openssl_sha.cpp:41-42，≤255B 截断 bug）+ golden 池 init 一次冻结 |
| cache_stress_aggressor | fail 不 report | **证实且加重**：victim 线程算出的 pass/fail **完全被丢弃**（run 只 join 后无条件 EXIT_SUCCESS，:92-105），错误仅写 stderr 且每迭代刷屏 |
| 本机核数 | 128 核 | 127 CPU 在线（nproc=127）；122 当前 **online** |
| memcpy_l3 尺寸 | sysconf 回退 8MB | 证实（sysconf 实测返回 0；真实 L3=56MB 由框架 `device_info[].cache[]` 正确持有，zstd19 -v 实证）——修法可行性成立 |

## 1. 问题定位（核验后的最终结论）

三个全局性缺口：

1. **无"每迭代重掷"**：随机操作数在 `test_init` 生成一次后冻结整场。一批测试连"每次运行"的随机都没有：
   - `arithmetic/adcx.cpp:128` 固定种子 `mt19937_64(12345)`；`arithmetic_arm/` 的 fisttp_arm/bigint_mulx_arm 固定种子 `0x123456789ABCDEF0`、operand_space_arm 混合模式表
   - `ipsec_*`（46 个测试）单个明文+密钥+golden 冻结整场（ipsec 模板 :76/:95-98 只在 init randomize）
   - `memcpy0` 值空间=1（src 一次 `memset(0x00)` 后整场复制同一份全 0）
   - `sve512_*_chain/special` 16/8 元素固定表确定性选取（:171/:180）；`atomic_simd_*` 只测 all-0/all-1（:51）
   - arm64 家族 17 文件零框架 RNG（其中 lsu/l2c/mmu_split/ooo/iex 的 6-8 元素高汉明表是**文档化故意设计**，缺陷仅为值空间小，非无校验）
2. **无"大 malloc + 随机偏移 + 尺寸变化"组合**：地址覆盖两极分化——大而固定（memcpy_l3 因 sysconf=0 永远 8MB 回退，真实 L3=56MB；mmu_stress_arm 16MB）或小而随机（mfence 32KB 随机索引）。唯一大区随机访问者是 lockgen（8MB 随机索引但仅聚合校验）。**无一测试做"数百 MB mmap + 随机偏移 + 随机块长"**。
3. **校验漏洞放大前两个问题**：
   - `cache_stress_aggressor`：victim 检出错误被静默吞掉（无条件 EXIT_SUCCESS）
   - `fma.cpp`/`fma_patterns_*`：容差比较吞 1-ULP 翻转（fmatail_*/fsu_byteexact 兄弟已是字节精确，标准不一致）
   - `vmx_*` 9 个 ARM64 假 pass（实测 exit: pass）——违反仓库自己的 placeholder-honesty 规则
   - `lock/spinlock/mesh`（~45 测试）聚合 Σ 校验，中途读坏但终和不变 → 漏检
   - `crc` 家族自比较（同函数跑两遍），一致错的 CRC 单元永远通过
   - `openssl_sha` arena_size **uint8_t 截断 bug**

**兼容性关键结论（核验证实）**：所有 golden 均为运行时计算（无源码硬编码 golden）→ 输入随机化与现有校验 100% 兼容。框架 RNG（random32/64/128、memset_random、frandom*）按线程独立流、`-s` 全局种子可复现、AES 引擎本机可用；**zstd/zlib/igzip 已每循环 memset_random —— 框架模式在本仓库有成功先例**，加固就是把这套模式推广到未用它的家族。框架 `device_info[].cache[]` 已正确填充 L1/L2/L3（零测试使用）——memcpy_l* 修尺寸探测的现成数据源。

## 2. 加固点总表（H1'-H18'，按性价比排序，证据见底稿 §2-§4）

| # | 加固点 | 类型 | 对象 | 风险 |
|---|---|---|---|---|
| H1' | `adcx` 固定种子 → 框架 `random64()` | 值 | 1 | 零 |
| H2' | `cache_stress_aggressor` fail 必须上报（victim→atomic bool→run report_fail）+ 去 stderr 刷屏 | 校验 | 1 | 零 |
| H3' | `vmx_*` ARM64 假 pass → `EXIT_SKIP`（9 个）；jit run 死代码顺手清理 | 诚实性 | 9 | 零 |
| H4' | `fma`/`fma_patterns_*` 容差 → 字节精确 memcmp | 校验 | 3 | 低 |
| H5' | **新测试 `random_access_sweep`**：大 mmap + 随机偏移块读写 + 尺寸变化 | 值+地址 | 新 | 中 |
| H6' | `ipsec` 模板：每 K 迭代重掷明文+密钥+重算 golden | 值+地址 | 46（一模板） | 低 |
| H7' | `openssl_sha/sha3/sm3sm4`：**修 uint8_t 截断 bug** + golden 池周期重填 + 竞技场扩 MB 级随机偏移 | 值+地址+bug | 3 | 低 |
| H8' | `memcpy_l{1d,2,3}` 尺寸探测改框架 cache 数据（56MB 生效）+ 每迭代随机 src/dst 偏移 | 地址 | 3 | 低 |
| H9' | `fma/vector/gather` 家族迁框架 RNG + 滑动窗口随机偏移；修 stderr 刷屏 | 值+地址 | ~33 | 中 |
| H10' | `atomic_simd_*` all-0/all-1 → `memset_random` | 值 | 3-5 | 零 |
| H11' | `eigen_*` 每 K 迭代重掷矩阵+重算 golden（每线程独立） | 值 | 16 | 中 |
| H12' | `sve512_*_chain/special` 固定表 → 框架随机（保留 specials 采样） | 值 | ~8 | 低 |
| H13' | GMP 系列 + arithmetic_arm 固定种子批：每迭代新操作数 | 值 | 4-6 | 低 |
| H14' | `spinlock/lock/mesh` 聚合 Σ → 每-op 读回校验（或随机间隔抽查） | 校验 | ~45 | 高（放最后） |
| H15' | `crc` 家族独立软件参考（抄 `isal_crc32_gzip` 模式） | 校验 | ~12 | 中 |
| H16' | `memcpy0` 全 0 → 每迭代 memset_random | 值 | 1 | 零 |
| H17' | `memcpy_sem/variations_dyn/shuffle` mt19937(random_device) → 框架 RNG（-s 复现） | 值 | 3 | 低 |
| H18' | arm64 零 RNG 批（lsu/l2c/mmu_split/ooo/iex 高汉明表 → 表+随机混合） | 值 | ~6 | 低（放后） |

**明确不做**：power_virus_dit / fpu_special_values / sve512_f64_special / fsu_byteexact 的特殊值表（文档化故意设计：高汉明切换、IEEE 边界覆盖，保持原样）；mmu_stress_arm 值面（地址面已 STRONG，不干扰 122 归因基线）；框架级新 API；zpclmul（独立工作量）。

## 3. 实施计划（one-patch-per-unit，P1-P17）

每 patch 独立：`ninja -C builddir` 零新告警 → 真实运行验证（引用实际输出）→ `zstd19` 回归 → x86-64 不触碰检查 → commit → push feature 分支。值加固类加 122 两阶段。

### 第一梯队：bug 级修复（零/低风险，先行）

- **P1（=H1'+H13' 部分）** `adcx.cpp` + `arithmetic_arm/{fisttp_arm,bigint_mulx_arm,operand_space_arm}` 固定种子 → 框架 RNG。验证：`-e adcx -t 5000 -n 1` pass ×2；`-e fisttp_arm -t 5000 -n 1` pass（若 ACL 未链则 skip 语义不变）。
- **P2（=H2'）** `cache_stress_aggressor.cpp`：victim fail 传回 + report_fail；stderr 改仅失败输出。验证：`-e cache_stress_aggressor -t 5000 -n 1` pass；fail-closed 自检（人为翻转一位确认能 fail）。
- **P3（=H3'）** `vmx_*` 9 个 ARM64 → log_skip + `EXIT_SKIP`（"to be implemented (placeholder)"）；jit 3 个 run 死代码清理。验证：`-e vmx_io_exit` 显示 skip reason；`--list-tests` 计数 291→~282（README 同步）；x86 语义不变。
- **P4（=H4'）** `fma.cpp`、`fma_patterns_avx512_{pd,ps}` 容差 → memcmp 字节精确。验证：`-e fma -t 5000 -n 1` pass；若实测暴露 NEON vs libm 真差异 → 回退保留容差+文档（不静默）。
- **P4b（=H16'）** `memcpy0` 每迭代 memset_random。验证：`-e memcpy0 -t 5000 -n 1` pass。

### 第二梯队：新测试 + 地址面（直接服务 122 狩猎）

- **P5（=H5'）** **新测试 `random_access_sweep`**（设计全文见底稿 §5）：每线程 mmap `S=min(512MB, MemAvail×0.6/threads)` + MADV_HUGEPAGE；每迭代随机偏移+随机块长（64B..2MB 对数分布）+ memset_random + NEON 读改写 + 同迭代标量 golden memcmp；knob：`mode={seq,rand,gather}` × `valmode={random,pattern,specials}` × `size_mb`。验证：build 零告警 → 单线程 pass → **122 两阶段** → zstd19 回归 → README/docs 更新。
- **P6（=H8'）** `memcpy_l{1d,2,3}` 尺寸探测改 `device_info[cpu].cache[level].cache_data`（sysfs 填充、本机实测 L3=58720256）+ 每迭代随机 src/dst 偏移。验证：`-e memcpy_l3_cache_size -t 5000 -n 1 -v` 确认 56MB（非 8MB）；60s 全核 pass。

### 第三梯队：家族级值加固

- **P7（=H6'）** `ipsec` 模板：每 K 迭代重掷（默认 K=1，`-O ipsec_<t>.reroll_every=N`）。验证：抽 3 代表 `-t 5000 -n 1` pass；全 46 个 `-t 2000` pass。
- **P8（=H7'）** `openssl_sha/sha3/sm3sm4`：修 uint8_t + golden 池周期重填 + MB 级随机偏移。验证：三测试 `-t 5000` pass。
- **P9（=H12'）** `sve512_*_chain/special` → 框架随机（保留 specials 比例）。验证：prctl launcher 下 `-t 2000 -n 1` pass；122 两阶段。
- **P10（=H10'）** `atomic_simd_128/256/512` → memset_random。验证：三测试 pass。
- **P11（=H13' 剩余）** `gmp_bignum/bigadd` 每迭代新对+golden 重算。验证：`-t 5000 -n 1` pass。
- **P12（=H9' 前半）** `fma/` 12 个：mt19937→框架 RNG、栈缓冲→≥256KB 滑动窗随机偏移、stderr 仅失败输出。验证：抽 3 个 pass + 迭代速率对比。
- **P13（=H9' 后半+H17'）** `vector/` 11 + `gather/` 10 + `memcpy_{sem,variations_dyn,shuffle}`：同 P12 模式；gather ref 改独立软件实现。验证：抽 4 代表 pass。
- **P14（=H11'）** `eigen_*` 16 个：每 K 迭代重掷+golden 重算，每线程独立矩阵，K 自适应。验证：`-e eigen_svd_double -t 10000 -n 1` pass ×2；全核 60s。

### 第四梯队：校验强度升级（改动面大，最后做）

- **P15（=H15'）** `crc` 家族独立软件参考。验证：抽 3 pass + crc32/crc32_fixed 去重。
- **P16（=H14' 前半）** `spinlock/lock` ~24 个：聚合→随机间隔抽查。验证：抽 3 代表 pass。
- **P17（=H14' 后半+H18'）** `mesh/` 27 个 + arm64 零 RNG 批：每块独立 golden memcmp。验证：抽 3 代表 pass。

## 4. 验证纪律（每 patch 强制）

1. `ninja -C builddir` 零新告警/错误
2. 真实运行 + 引用实际输出（result: pass、尺寸值、迭代数）
3. `zstd19` 回归 pass
4. x86-64 不触碰检查（diff 审查：`#elif __aarch64__` / 共享加宽 / x86 语义不变）
5. **值加固类（P5/P9/P12-P14）加做 122 两阶段**：`echo 0 > cpu122/online` → 全核 60s 零 fail 基线 → `echo 1 > cpu122/online` → 全核跑 fail 必须锁定 122。**offline/online 前征得用户同意**（122 当前 online，是狩猎任务观测对象）
6. 大颗粒度修改同步 README/docs（P3 测试计数、P5 新测试、P6 尺寸探测）

## 5. 风险与对策

| 风险 | 对策 |
|---|---|
| 随机化在健康核上偶发 false fail | golden 同迭代同线程流重算（复现铁律）；122 两阶段 60s 全核基线是防 false-fail 的门 |
| golden 重算算力 ×2 | 本就是双算压力测试；ipsec 用 K 自适应；eigen 大矩阵 K 按 desired_duration |
| Eigen 多线程 ULP 已知问题 | 每线程独立矩阵；svd_cdouble/sparse 维持 -n 1 语义 |
| 大 mmap OOM（43.6GB/127 线程） | S=min(512MB, MemAvail×0.6/threads)（RSS×12 余量法，抄 svd_cdouble_sve 实证公式）；knob 可调 |
| vmx→SKIP 使默认通过数下降（291→~282） | 诚实性修复的正确结果；README 计数同步更新 |
| pthread_cancel 后大缓冲泄漏 | test_cleanup/munmap 释放（cancel 语义 09-18 审计已确认） |
| 122 offline/online 影响狩猎任务 | 每次两阶段前征得用户同意；跑完立即恢复 online |
| memcpy_l3 尺寸 56MB 后时长超预算 | 尺寸变更同时调 desired_duration（抄 svd_cdouble_sve preinit 模式） |

## 6. 与 122 狩猎任务的衔接

- P5 的 knob 矩阵（mode×valmode×size）= 122 触发特征实验的直接载体：online 122 后扫 knob 找最快触发组合 → 回答"什么负载最快触发"
- P9/P12 值随机化覆盖 FP/SIMD 通路；P5/P6/P8 覆盖内存/缓存通路；P2/P4/P15/P16 消除"触发了但校验吞掉"的盲区
- 每个加固 patch 的 122 两阶段结果记入 `docs/docs_xu/` 进度日志，累积负载特征分析原始数据
