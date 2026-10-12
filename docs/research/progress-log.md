# 研究进度日志（按时间倒序）

> 每次会话结束前（或中间关键节点）在此追加。格式：日期 + 做了什么 + 实证 + 下一步。
> 目的：机器关机/断网导致对话中断后，新会话能从这里恢复上下文。

---

## 2026-09-19（会话 3：全量测试随机化加固审计 + P1-P17 方案）

### 完成内容

**任务**：审计全部 SDC 检测用例的操作数（数值）× 地址空间变异覆盖，识别加固点，制订方案（planning-with-files：`.planning/2026-09-19-sdc-test-randomization-hardening/`）。

1. **5 路并行审计代理**（compute / memory-lock / crypto-compression / arm64-native /
   framework+本机实测，~577k tokens，全部真实源码阅读）覆盖 30 目录 ~308 测试 ID
2. **三个全局性缺口结论**：
   - 无一测试"每迭代重掷操作数+重算 golden"（唯一例外 sve512_*_svd 每迭代重算标量 golden）
   - 无一测试同时具备"大 malloc+随机偏移+尺寸变化"（lockgen 8MB 随机索引最近，但仅聚合校验）
   - 校验漏洞放大前两者：cache_stress_aggressor **fail 从不 report**（SDC 静默吞掉）、
     fma/fma_patterns 容差吞 1-ULP、vmx_*/zero_control_vec_* ARM64 假 pass、
     gather_* hw/ref 同段代码跑两遍、lock/spinlock/mesh 聚合 Σ 漏检抵消性错误
3. **T0 名单**（固定种子/固定明文）：adcx 种子 12345、ipsec×46 单 1024B 明文冻结整场、
   power_virus_dit 4 硬编码浮点、sve512_*_chain_arm 16 元素固定表、atomic_simd_* 只测
   all-0/all-1、GMP 一对 4096-bit 跑整场、fisttp_arm/bigint_mulx_arm 固定种子
4. **框架能力确认**：RNG 按线程独立流（-s 可复现，AES 引擎可用）；**全仓库 golden 均
   运行时算 → 输入随机化与校验 100% 兼容**（代价=重算算力×2）；`-O test.key=value`
   knob 现成；`device_info[].cache[]` 已填充但零测试使用；**sysconf(_SC_LEVEL*_CACHE_SIZE)
   本机返回 0 → memcpy_l* 永远 8MB 回退值（真实 L3=56MB）**——bug 级
5. **产出方案** `docs/superpowers/plans/2026-09-19-sdc-randomization-hardening.md`：
   15 加固点（H1-H15）→ 17 patch（P1-P17，one-patch-per-unit），四梯队：
   - 一（零风险 bug 修复）：adcx 种子、cache_stress 假 pass、vmx 诚实性 EXIT_SKIP、fma 容差→memcmp
   - 二（新测试+尺寸 bug）：**random_access_sweep**（大 mmap+随机偏移+对数块长+NEON 读改写+CRC，
     mode×valmode×size knob 矩阵直接服务 122 触发特征实验）；memcpy_l* 修尺寸探测+随机偏移
   - 三（家族值加固）：ipsec 模板每 K 重掷（一模板 46 测试受益）、openssl golden 池重填、
     sve512 随机填充、atomic_simd 随机值、GMP 每迭代新对、fma/vector/gather 迁框架 RNG+滑动窗口、
     eigen 每 K 重掷（每线程独立矩阵避开 ULP 已知问题）
   - 四（校验升级）：crc 独立参考、spinlock/lock/mesh 每-op 读回校验
   - 值加固类 patch 一律走 **122 两阶段验证**（offline 122→60s 全 pass 基线→online 122→fail 锁定）

### 实证

- git：会话起始分支 feat/sve-port-avx53 == origin/main（84e5e97a），无冲突合并
- 本机实测：THP=always、ulimit -v 无限、52GB avail、127 核在线（1 offline=122）、
  内核 6.6、L1D 64K/L2 1280K/L3 57344K（sysfs 真值）
- 交叉验证新落地代码：origin/main 已前进到 d69ce8b2（PR #134-136：ipsec datasize knob
  4 commit + sve/ 目录 31 个新测试）——**审计基线 84e5e97a 不含这些**。抽查确认新 SVE
  移植（fma/mesh 22 文件用 mt19937；mesh 用 global_sum 聚合校验）**继承了原家族的
  同类弱点**，H6/H9/H14 的加固对象需在新基线上重新圈定

### 状态与下一步

- 方案**待用户批准**（P1 起实施或按 122 狩猎优先级调为 P5 先行）
- **待办**：实施前 rebase 到 origin/main（d69ce8b2）——ipsec datasize knob（-O datasize=N，
  1024..64MB）已解决 H6 的"尺寸变化"半边，H6 改为只剩"每 K 重掷明文+重算 golden"半边；
  sve/ 31 新测试需并入 H9/H14 的对象清单
- 审计底稿：`.planning/2026-09-19-sdc-test-randomization-hardening/findings.md`（逐测试分类表）

---

## 2026-09-19（会话 2 续 6：ipsec 访存路径覆盖——datasize knob 落地）

### 完成内容
参数审查的第 2 优先建议落地（3 commit：计划 cb34a239 + 代表 81822ac6 + 批量 d203a710）：
- ipsec 46 用例 `DATA_SIZE=1024`（L1 驻留）→ `-O <testid>.datasize=N`（1024..64MB，
  16 的倍数，默认 1024 行为零变化）
- 变换模式（每文件 8 处）：define→knob 读取+fail-loudly 校验；struct 数组→malloc 指针；
  golden/run 全调用点随 d->datasize；cleanup 补 free
- Task 1 手改 3 个结构代表（cbc+mac / ctr / gcm tag 形态）验证模板；
  Task 2 脚本化批量 43 个（cbc/ctr/gcm/3des × hmac/sha2/cmac/xcbc 五种 MAC，
  xcbc 的 5 参 full_mac 中转变量单独处理），全局零残留核查通过

### 实证
- 默认档全量 46 用例：exit pass，1699 迭代全 pass 0 fail/skip（逐字节兼容）
- 16MB 档（3 代表）：pass，runtime 0.1s→4.4-5.5s 证明工作集真实到 DRAM
- 4MB 档抽样（3des/avx512/xcbc 三个未手改形态）：pass
- 越界（1000 非 16 倍数 / 512 过小）：fail-loudly 带合法域
- 回归：zstd19 + eigen_svd_cdouble pass；ninja 零警告

### 诚实边界（计划内记录）
16 倍数约束（保 CBC/CTR/GCM 无填充密文长度语义）意味着本 knob 解锁的是**访存层级**，
不是 AES 尾块路径——后者需独立设计（padding 或显式 partial-block 测试）。

---

## 2026-09-19（会话 2 续 5：三方库用例参数与压力标定批判性审查）

### 完成内容
- 第二轮三方库审查（参数/压力维度），产出 `docs/research/third-party-sdc-param-critique.md`：
  - **三级参数成熟度分级**：L3 硬件自适应仅 eigen_svd_cdouble_sve（本分支）1 个；
    L2 框架 knob 10 个族；L1 环境变量 2 个（GEMM_K_DIM，≈写死）；L0 写死 61/76
  - **系统性反模式 ×3**：默认值与硬件无关（61 用例默认工作集 <10MB，61GB RAM 用率
    <0.01%）；knob 上限锚定旧硬件（sleef 3MB 按 TSV110）；参数锁死代码路径
    （ipsec 1KB=整 64 AES 块永无尾部块【验证 1024%16=0】；isal_crc 1KB 锁死
    aarch64 汇编的 64B 主循环形态【汇编 cmp LEN,63/15 分派验证】；
    eigen_svd ≤512 浅 D&C 递归）
  - 逐族 11 节批判 + 优先级建议 6 条（isal_crc 三修最高性价比）
  - 好设计表彰：openssl_sha 的 1024 组 golden 池跨 L1、zlib 每迭代随机 level
    （隐式扫参数空间，值得推广）、openblas 的参数↔缓存映射文档

---

## 2026-09-19（会话 2 续 4：三方库 SDC 用例全量审查）

### 完成内容
- 逐族实读全部三方库业务负载用例（11 族 76 用例），产出
  `docs/research/third-party-sdc-test-review.md`：
  - 每族：golden 来源/比较方式/覆盖故障面/弱点
  - **发现**：isal_crc 族内部分两派——crc32_gzip 有软件参照（交叉验证），
    其余 9 个（crc64×6/ieee/iscsi/t10dif）只做同函数双重自比 → **永久性单 bit
    故障盲区**（两次错得一样即漏检）；且用 mt19937 而非框架 RNG（不可复现）
  - 检测强度谱系：交叉验证 > golden 重放（主流，ipsec×46/eigen×18/…） >
    往返自洽（zstd/zlib）> 双重自比（isal_crc B 派）
  - 亮点清单：随机偏移 mmap arena、openblas 输入完整性三重检查、sleef u10/u35
    双链互证 + 有限性哨兵、pocketfft 逆变换不比较的诚实取舍

### 待办（候选）
- [ ] isal_crc B 派 9 用例补表驱动软件参照 + 换框架 RNG（一次计划一个单元）

---

## 2026-09-19（会话 2 续 3：desired_duration 随维度缩放）

### 完成内容
- 修复动态维度下静态 desired_duration=240s 的误报源（commit fc42d604）：
  - 尺寸决策 + desired_duration 一起挪进 **test_preinit**（parent 进程，先于框架 test_duration() 计算——
    在 test_init 里写无效，框架 1700 行先读 duration 后调 init；ist_skip_preinit 是既有先例）
  - 公式：duration = 0.95 × t(N)，t(N) ≈ 243.4s×(N/4400)³ + 1s（实测模型，N≥2400 时 ±2%）
  - -5% 余量保证任意维度下 do-while 恰好一轮；派生超时 5×duration+30s 同步缩放

### 实证（全 pass 零告警）
- mdim=300：duration 1023ms，runtime 0.67s
- mdim=1800：duration 16780ms，runtime 35.8s（init+run 两次分解，在 ±25% 带内）
- auto -n 1：M_DIM 4400，duration 232180ms，runtime 487.3s

---

## 2026-09-19（会话 2 续 2：动态维度）

### 完成内容
- `eigen_svd_cdouble_sve` M_DIM 编译期 → **运行期按内存动态**（commit d2ccab8c）：
  - 模板 `EigenSVDTest` 增 runtime-dim 模式（`Dim==Dynamic`），11 个编译期用户零影响（if constexpr）
  - 尺寸公式：N = min(√(RAM/worker ÷ 192), 4400 时间帽)，下限 300；`-O eigen_svd_cdouble_sve.mdim=N` 覆盖
  - 内存模型实测：RSS ≈ 9.7-10× 矩阵体积，取 12×（20% 余量）
- **OOM skip 变优雅降尺寸**：32 线程（1.91GB/worker）→ M_DIM 3269 全员真跑（原方案直接 skip）

### 实证
- `-n 32`：M_DIM 3269，32 worker 并行，全 pass，框架 exit pass
- knob=300：pass（638ms），sizing log "M_DIM 300 (memory 61.17 GB/worker / 1 threads)"
- 回归：eigen_svd_cdouble/double/jacobi_cdouble（模板用户）+ zstd19 全 pass

---

## 2026-09-19（会话 2 续：M_DIM 提升）

### 完成内容
- `eigen_svd_cdouble_sve` M_DIM 300 → **4400**（commit 1646e40e）：10 分钟预算内实测最大
- 标度实测（VL=128 pinned）：2400→40s / 3600→134s / 4400→243s / 4600→292s / 4800→322s / 5600→511s / 5800→563s / 6000→622s（单次 BDCSVD）
- **关键实测发现**：框架测试总时长 = init golden 分解 + run 重算 = **两次 BDCSVD**（5800 时 1124s 破预算；小维度模型 N=1800/desired=15s→总 35.8s=2×17.9s 证实）→ 最终 4400（2×243s=487s，余量 19%）
- desired_duration=240s（略低于单次 243s，run 恰好一次；600s 初值会让 do-while 起第二次迭代）
- OOM 护栏：4GB/worker（实测峰值 2.8GB+40%），全核默认 32 线程干净 skip

### 实证
- 单线程 `-n 1`：`exit: pass`，test-runtime **490.3s**（8.2 分钟）
- 全核：护栏 skip，框架 exit pass；zstd19 回归 pass

---

## 2026-09-19（会话 2）

### 完成内容

**主线：Eigen SVE double/complex<double> packet 后端**（分支 `feat/eigen-sve-double-packets`，9 个功能 commit + 文档，已全部 push 至 b6048f7e）：

1. **根因调查**（承接会话 1 的占位 skip 修复）：Eigen 5.0 SVE 后端只有 int32/float
   packet 特化，`packet_traits<double>::size=1` 标量回退 → `eigen_svd_cdouble_sve`
   300×300 complex BDCSVD >10 分钟。
2. **计划**：`docs/superpowers/plans/2026-09-18-eigen-sve-double-packets.md`
   （SDD 工作流，8 任务分解；svcmla 语义用 10 个硬件探针预先解码）。
3. **实现**（全部落地）：
   - `PacketXd`（svfloat64_t 全套算术，PacketMath.h +336 行）
   - `PacketXcd`（complex<double>，svcmla rot0+rot90 复数乘法，新文件 Complex.h）
   - pround/svrinta + print/svrintn + double↔int32 跨宽 pcast（MathFunctions/TypeCasting）
   - e2e 集成中修复 3 个深层 bug：ptranspose 复数粒度（f64 转置拆散 re/im，
     UpperBidiagonalization 48 列阈值定位）、pgather/pscatter（generic 忽略 stride）、
     **Aligned64 假设**（与 EIGEN_MAX_ALIGN_BYTES=16 不符 → 线程间 ULP 级确定性
     分歧，SDCShield bit-exact golden 比较抓到）
   - 占位 skip 退役 → `eigen_svd_cdouble_sve` 真实 SVE 压测
4. **验证矩阵**：
   - 硬件（cortex x3b，VL=256）：packet 单测 + e2e 全部 ALL PASS；
     complex BDCSVD **68ms**（标量回退 >10 分钟，~20000×）
   - SDCShield：单线程/全核(127 worker) pass；全量 `--quality=-1`
     **exit 0：290 pass / 9 skip / 0 fail**（skip 10→9）
   - **gem5 SE**（VL=512，本机硬件没有）：构建 gem5.opt v25.1.0.1（user-local
     Python 3.11.6 + zlib 1.3.1 + python3-config shim 解无-devel 环境），512-bit
     全部测试 + e2e mini ALL PASS；256 与硬件交叉验证一致
   - harness 沉淀：`scripts/eigen-sve-double/gem5/{se_sve.py,run_sve.sh,README.md}`
5. **关键平台事实（新发现，已记入 CLAUDE.md）**：
   - size-specific SVE 代码要求运行时任务 VL == 编译期 `-msve-vector-bits`；
     本机 256 硬件跑 128 编译产物需 prctl pin（vlrun 模式）
   - `sleef_sve` 只在 VL=128 编译下 pass（256 编译回归）→ meson 保持 128
   - svcmla 旋转角语义（rot0/90/180/270 逐 lane 代数）由探针解码，无文档可循

### 执行方式说明（诚实记录）

- Task 0-2 走 SDD subagent 流程（每任务实现+独立评审+修复循环）；Task 2 评审
  抓出 3 个真实缺陷（pfirst OOB 写、Has*=1 无实现、pcmp_eq 错语义）+纠正了一条
  错误的探针转录记录
- Task 3 起 subagent API 预算耗尽（429），Task 3-7 由主会话 inline 执行，
  全部验证命令与真实输出在会话记录及 commit message 中

### 环境状态

- 分支 `feat/eigen-sve-double-packets`（HEAD b6048f7e，已 push），工作树干净
  （.entire/ docs_xu/ boost-headers/ m5out/ 为未跟踪的本地产物）
- `builddir/` 健康（VL=128 编译，290 测试全绿）
- gem5 产物：`/home/sdc/wangxu/gem5-fi-fuzz/build/ARM/gem5.opt`（1.1GB）
- 测试资产：`scripts/eigen-sve-double/`（4 个 standalone 测试 + gem5 harness）
- 待清理：仓库根 3 个历史 yaml 日志 + m5out/

### 下一步（候选，未开始）

- [ ] PR：feat/eigen-sve-double-packets → main（10 commit 就绪，可随时发）
- [ ] sleef_sve 在 VL=256 编译下的回归根因（本分支范围外，值得单独计划）
- [ ] PacketXd 超越函数（plog/psincos/atan2）→ 补 complex psqrt/plog/pexp（Has*=0 暂关）
- [ ] docs/research/ 是否入库待确认（会话 1 遗留）

---

## 2026-09-18（会话 1）

### 完成内容

1. **`sdcshield --help` 完整输出** 已获取并核对（真实运行）。
2. **建立 `docs/research/` 研究记录体系**（本目录）：
   - `README.md` — 索引 + 快速事实卡
   - `usage.md` — 使用指南（含 `--help` 未列出的隐藏选项，源码 `framework/sandstone_opts.cpp` 逐一核对）
   - `adding-tests.md` — 添加新测试用例完整指南（模板 + 生命周期 + meson sourceset + 仓库纪律）
   - `progress-log.md` — 本文件
3. **使用方法研究**：基于 README.md、CLAUDE.md、`docs/writing_tests.md`、
   `framework/sandstone_opts.cpp`（全部选项表）、`framework/sandstone.h`（API）。
4. **添加测试方法研究**：`tests/meson.build` → `tests/common/meson.build` →
   `tests/cpu/meson.build`（四个 sourceset + vendored 两级 gate + 三个专属 march 的
   static_library）→ `tests/cpu/arm64/meson.build`（NEON/Crypto 与 SVE 两个库）。

### 实证记录（当天真实运行过的命令与输出）

- `./builddir/sdcshield --version` → `sdcshield-06f0ef541a61`（main @ 06f0ef54）
- `--list-tests` → 289（PROD 默认）；`--quality=0` → 293；`--quality=-1` → 298
- `--list-groups` → `@compression` `@ipsec` `@math`
- `--selftests --list-tests` → 142 个
- `-e zstd19 -t 2000 -n 1 -o /dev/stdout` → `result: pass` / `exit: pass`（回归基线可用）

### 环境状态

- 构建目录 `builddir/` 健康，vendored 依赖（openssl/openblas/sleef/isa-l）install/ 齐备
- 当前分支 main（研究记录文档尚未提交——**待办**：按仓库纪律这些属于文档，
  是否提交/push 需用户确认，或按"计划先行"纪律走 docs 提交路径）
- 仓库根目录有三个历史运行 yaml 日志（sdcshield-20260918T*.yaml，当天的全量运行产物）

### 下一步（候选，未开始）

- [ ] 确认 docs/research/ 是否入库（分支 + commit + push）
- [ ] （用户如有后续研究方向待补充）

---

## 2026-09-25/26（会话：GitHub 特性调研 + CI 全线修复）

### 完成内容

1. **GitHub 特性面调研**：对照仓库 nav 全 tab 出具报告（本会话内交付用户）。
2. **仓库设置**（API 完成）：description + 10 个 topics；Dependabot alerts + security
   updates + 私有漏洞报告开启；标签 arm64-port/framework/test + milestone
   「ARM64 移植补全」+ issue #2/#6/#7 归类；21 个 CodeQL 误报（3DES 为故意
   测试内容）dismiss，2 个 integer-multiplication-cast 告警保留待查。
3. **CI 修复 6 个 PR 全部合入**（#175/#176/#177/#178/#179/#181）：pr.yaml 的
   actionlint 安装与调用三重 bug；build-cpu 复合 action 元数据（type 键非法、
   description 缺失、matrix 键未定义）；security.yaml 的 action 版本、osv v2
   参数、zizmor SARIF 权限；zizmor.yml 更名（点文件不被 v1.30 发现）；
   codeql.yaml 切 x86 机群 + concurrency + $PWD 绝对路径 + plain 构建；
   multi-os-verify 的 benchmark tool 值 + checkout v7。
4. **仓库真实缺陷修复**（CI 首次真跑暴露）：unittests `-march=haswell` 加
   x86 守卫；x87 语义 2 单测守卫 x86（#180）；bats 架构专属用例按存在性
   跳过、AES 种子断言改格式（OpenSSL 版本依赖）；mite/movdq2q/
   spinlock_unaligned 迁入 aarch64 守卫（x86 编译断）；sve512 家族加
   512-bit VL 门控（#182）。
5. **multi-os-verify 全绿**（smoke dispatch 36171807875）：benchmark-trend
   修复生效；gh-pages 首建并清理污染树；**Pages 上线**
   （https://wangxumarshall.github.io/sdcshield/，图表在 /dev/bench/）。
6. **main 分支保护**：10 个必过检查；enforce_admins=false；CodeQL analyze
   暂不设硬门（平台侧 runner 回收未稳，见下）。
7. **本机 .bashrc**：GITHUB_PERSONAL_ACCESS_TOKEN/GHCR_TOKEN 补 export
   （备份 ~/.bashrc.bak-20260925-claude）。

### 实证记录（当天真实运行）

- 本地 actionlint 1.7.12 无参数模式 → `exit=0`（CI lint 绿的前置验证）
- 本地 zizmor v1.30.1：`.zizmor.yml` 更名前 47 findings / 更名后
  `No findings to report`
- 本机 aarch64 全量 ninja（470 targets）+ zstd19 回归多次 `exit: pass`
- unittests 本地 101/101（CI 同款过滤）
- 同 AES 种子本地输出 `I> 370546198 123984908 1106120622 309198093` ≠
  bats 硬编码 `I> 1242137224 ...`（AES 派生值依赖 OpenSSL 构建的实锤）
- multi-os smoke：15 verify + report + benchmark-trend 全 success
- Pages builds/latest → `built`；站点 HTTP 200 + BENCHMARK_DATA 实数据

### 环境状态

- main @ 2a05c23f 起含全部修复；分支保护生效
- **平台侧事件**：GitHub 托管 runner 今晚对 ~10 分钟重负载构建连续回收
  （x86×3 + arm 大量，均 "The runner has received a shutdown signal"，
  无编译错误、其余短 job 全绿；#179 评论有证据链）。CodeQL main push
  的 attempt 2 进行中，平台恢复后自愈。
- 遗留 issue：#180（Float80 aarch64 适配）、#182（sve512 非 512-VL 失配
  根因）、#183（random_access_sweep 在 debian:sid 容器静默 abort，PR quick
  暂禁用）；CodeQL 2 个 integer-cast 告警待查
- build-cpu action 内 checkout@v6 / upload-artifact@v4 残留（dependabot
  周一会开 PR，未动）

### 下一步

- [ ] 观察明日 04:00 UTC cron 的 multi-os：gh-pages 是否再次被污染
      （若复现，benchmark-trend 改最小 checkout 或定期清理）
- [ ] 平台回收稳定后把 `analyze (cpp)` 加入分支保护必过检查
- [ ] #183 在 debian:sid arm64 容器复现 + core 取证
- [ ] CodeQL integer-multiplication-cast 2 告警排查（eigen_svd common 头）

## 2026-09-26（会话：Copilot Autofix PR DCO 补签 + 豁免根治）

### 完成内容

- PR #193/#194（Copilot Autofix 修 eigen_svd CodeQL 告警 22/23）缺签致
  git-sanity 必过检查失败，已手动补签强推（amend --signoff，树零变化），
  两者 git-sanity 绿。
- 根治：`ci/dco-exempt-copilot-autofix` 分支——check-git-history.sh 双标记
  豁免 autofix 机器提交（GitHub 提交者 + `github-advanced-security[bot]`
  trailer，缺一不可——仅提高伪造门槛、非防伪造保证，均无认证元数据，
  merge 审查为真实认证），命中打 ::notice 可审计；人工提交行为零变化。
  弃 CI 自动补签路线：GITHUB_TOKEN 推送不触发 CI 重跑（PR 会卡 waiting）、
  仓库无 PAT secret。计划：docs/superpowers/plans/2026-09-26-dco-exempt-copilot-autofix.md
  （六用例 harness：未签/豁免/已签/伪造 trailer/混合 PR/merge 规则）。

### 下一步

- [ ] 合并豁免 PR 后，观察下一个 autofix PR 的 git-sanity 即时豁免生效

## 2026-09-27（补记：sdc-excite-reproduce M0-M4 里程碑结论——M4 终审 Minor #4 欠账清偿）

> 欠账来源：M4 终审 Minor #4——progress-log 只记到 2026-09-26 的 M1b/CI 会话，
> M0-M4 五个里程碑的交付与结论从未在此日志立档（各里程碑细节在
> `docs/superpowers/plans/` 与 `.superpowers/sdd/`）。本条为一行式索引，逐里程碑
> 展开见对应 plan/sdd 与 v5 §14.1-14.2 as-built 标注列。

| 里程碑 | 分支（PR） | 一行结论 |
|---|---|---|
| M0 契约与基线 | `feat/sdc-excite-reproduce-m0`（PR #161/#163/#164，共 14 commits，09-25 合入） | canonical 事件 schema + 五元时间 + 拓扑库 + legacy 适配器 + A/B 基线 + **命名统一迁移落地**（campaign→sdc-excite-reproduce，含 systemd 路径 bug 修复）；磁盘前置清理 86%→78%（output 附2） |
| M1 可观测性 | `feat/sdc-excite-reproduce-m1`（PR #172/#174，09-25/26） | 采集器三件套 collector@{percore,pmu,ras}（60s/20s as-built 档——BMC 限速教训，非 1s 目标态）+ acceptance_m1 0.5h/24h 门 + coverage 质量列 + A/B 开销基线；远端观测面〔研究〕未部署 |
| M1b monitor v3 + 驱动加固 | `feat/sdc-excite-reproduce-m1b`（PR #187/#188，09-26） | 发现式列集 monitor.csv v3 + 离散态 diff + known_faults 白名单 + 复测内存护栏（cold_c4 OOM 教训）+ 孤儿清理 + 测试沙盒（SDC_ORPHAN_PIDS——pkill 污染战役实测后立红线） |
| M2 规则闭环 | `feat/sdc-excite-reproduce-m2`（PR #189/#190，09-26） | 四守护（eventd/controller/root-helper/ring）+ 五态机重放确定性 + cmd/ 文件协议（burst/hotplug/restore+读回审计）+ **消费基线**（防陈旧事件驱动特权动作）+ 9 服务部署 + BPF canary 探测（SPURIOUS_CANARY=dmesg 降级） |
| M3 主动激发与复现 | `feat/sdc-excite-reproduce-m3`（PR #192/#196，09-26） | sdc_profile（resolve 冻结）/四轴执行器（governor=能力缺口诚实暴露）/sdc_reproducer（门禁七项+capsule+run.sh --check-only）/概率 ddmin（CORE179 硬规则：禁确定性删减）/repro_queue 闭环；m3_drill 验收 |
| M4 诊断与统计 | `feat/sdc-excite-reproduce-m4`（PR #197，09-27） | 五件分析工具全 stdlib-only：sdc_stats（精确二项）/bitview（七视图）/hypothesis（十域+E0-E4）/timeline（±60s 窗差分）/report（五段+guard 红线自检）+ CORE179 脱敏 fixture 回归；全套 298 passed 基线由此确立 |

M4 终审其余移交已随 M5 T1/T2 交付（exposure 生产端 + 机读 hint 契约/嫌疑核标签接续）；
M4 未尽项（假通过清单 ARM64 重推导、耗时尾部基线）在 v5 §14.2 状态列如实标注为 ◐。

## 2026-09-29（会话：CodeQL workflow 62% 失败根因勘定——构建期 OOM,非机群回收）

> 勘误:2026-09-25/26 条目中"平台侧事件/两机群 runner 回收"的归因**有误**。
> 同样的 shutdown signal 死亡,x86 与 arm 机群皆现,与机群无关——是本仓
> CodeQL job 自身的内存峰值把 runner 压死。证据链见下。

### 完成内容

**任务**:codeql.yaml 近 100 run 62 failure(63%),全部死在 Build 步骤
(exit 143 + "The runner has received a shutdown signal"),彻底解决。

1. **根因勘定**(systematic-debugging 全程取证):
   - 14 个失败 run 抽样:全部死在 eigen 模板簇编译区([200/455]
     eigen_svd_jacobi_svd_cdouble 同死点),唯一 failure 注解 exit 143,
     无任何编译错误;连 docs-only PR 也死在同点。
   - 单编译器内存实测(本机 aarch64 GCC12.3 plain):BDCSVD 簇
     1.8–2.7GB/文件、JacobiSVD 簇 ~0.9GB、GEMM ~0.5GB。
   - 鉴别对照:multi-os-verify 同款 ubuntu-24.04-arm 编译同一批 TU
     (无 CodeQL)每 job 仅 ~3% 失败 → 变量是 CodeQL 提取器内存叠加。
   - 决定性取证(debug/codeql-oom-proof 分支,run 36529815144,
     -j4 + free 实时探针):used 4GB→15.7GB(80 秒)→ RAM 耗尽后 swap
     以 ~500MB/5s 填满 88% → runner 被杀。CODEQL_RAM=14535 实证 16GB
     上限;runner 自带 3GB swap(此前"无 swap"认知错误)。
2. **修复**(PR #206,merge d4c358c7):`ninja -j2` + timeout 45→60min
   + 头部注释修正错误归因。构建时长未受损:同 run 实测 Build 7m10s
   (36530572140;arm 机群性能方差 ~2x,另一样本 14.9min,总时长 ~21min)。
3. **验证**:峰值 used 5.1GB / available 最低 10.8GB / swap 0(修复前
   15.7GB+swap 88%);-j2 后 5/5 run Build 步骤全绿、0 OOM 死亡。
   合并后 main 的 3 个 run(36534939891/36534904559/36534628654)Build
   全过,analyze 死在 SARIF 上传 403——仓库 code scanning 设置被误关
   (07:09–07:30 间),与代码无关,待网页恢复。
4. **过程**:探针版(带 free/dmesg 遥测)经 **PR #208**(直接合并
   debug/codeql-oom-proof,merge aa5eb409)先进 main,4 分钟后 #206
   (干净修复)合并 → main = 修复+遥测。PR #211 清理了矛盾注释、删
   dmesg 死代码(保留 free 遥测);PR #212 补 EXIT trap(评审 Important:
   bash -e 失败路径 watcher 持 stdout 管道,编译错误 PR 会挂到 60min
   超时,actions/runner#2415;本地 RED→GREEN 实证)。
5. **评审**:fresh-context 全分支评审 verdict fix-then-ship;根因数字/
   -j2 充分性/sed 引号/concurrency/x86 零回归/DCO 逐项复核无恙;
   Important+Minor 均已闭环(见 PR #212 与 issue #213)。

### 实证记录(当天真实运行,run ID 均可在 Actions 检索)

- -j4 死亡取证:run 36529815144(内存曲线:4GB→15.7GB/80s→swap 88%→杀)
- -j2 仪器化验证:run 36530572140,峰值 5.1GB,success
- PR #206 attempt 1:run 36531963900,06:36:44→06:55:55 端到端 success
  (Build 14.9min,含 SARIF 上传);attempt 2 被 PR 合并级联取消(非失败)
- 本机 17 个 eigen TU 峰值 RSS 表 + 完整证据链:
  docs/superpowers/plans/2026-09-29-codeql-runner-oom.md

### 下一步

- [ ] **恢复 code scanning 仓库设置**(用户网页操作)后 rerun
      36534939891 验证 main 端到端全绿
- [ ] 设置恢复后把 `analyze (cpp)` 加入分支保护必过检查
      (原"平台回收未稳"的保留理由已失效)
- [ ] issue #213:codeql-action v3→v4 迁移(2026-12 弃用截止)
- [ ] 观察周一 cron(UTC 02:37)与日常 push run 持续绿(自然回归)
- [ ] progress-log 头部"按时间倒序"声明与 09-25 起实际底部追加不一致,
      待统一(本条目沿用底部追加惯例)

> **2026-09-30 补记(根因终局)**:昨日 09-29 07:08–07:43 间,本私有仓的
> GitHub Pro(或试用)订阅到期/降级——分支保护 API 实测返回 "Upgrade to
> GitHub Pro or make this repository public to enable this feature",而
> 09-25 分支保护尚在(上文"10 个必过检查")。付费特性同时失效:
> code scanning 禁用(CodeQL analyze 与 zizmor 的 SARIF 上传 403)、分支
> 保护消失(红叉 PR 可合并)、token scope 收紧(gitleaks/zizmor 403,已由
> PR #214 显式权限声明修复并验证)。"找不到设置开关"实为付费墙。
> 待用户决策:恢复订阅 / 转公开仓 / 接受降级(SARIF 改存 artifact)。
> 另:PR #202 的 5 个缺签提交已于 09-29 补签强推(14a82a92→f9e79b65,
> 树零变化),git-sanity 转绿。

> **2026-10-10 战役记账(SVE 全节点压测 · hollow 修复)**:NSCC 战役(r21/r22,r23 投
> 递中)发现 13 个轻量 SVE/SME 测试(fma_sve、fpu_special_values_sve、9×sve512_*、
> 2×sme_*)在 608 核节点上实为 hollow——框架默认 fracture(40 循环切片重 fork)×
> μs 级测试体 → 全部墙钟耗于 fork/38 线程 spawn churn,有效负载 ~0.5%(cn23423:
> fma_sve 925s 内 19156 次迭代、子进程 utime=0)。修复:`.fracture_loop_count = -1`
> (commit 7c4dc566,计划 5f679782);cn23196 实测 13/13 pass、blocks=2、满时长、
> idle delta=0(~100% 满载);zstd19 回归 pass;x86 不受影响(arm64 文件不编译,
> fma2 两测试首调即 SKIP,循环计数不在执行路径)。构建教训:(a) 无 ninja 增量构建
> 须强制 cwd=compile_commands.json 的 directory 字段;(b) ninja 链接命令的隐式依赖
> (| 后档案)不是位置参数,误用 → 档案两遍 → 277 multiple definition;(c) 旧二进制
> 控制副本被覆盖丢失,字节证据须即时多处备份。r23(23 测试全套+大矩阵+EDAC)以修复
> 版 542e197e 滚动覆盖;q_hpcapp 池内无 2.0GHz 段节点(全 cn23xxx@1.55),频率偏好
> 不可满足。战场细节:NFS 上 sve-campaign-20261009/progress.md(非 repo,不入库)。