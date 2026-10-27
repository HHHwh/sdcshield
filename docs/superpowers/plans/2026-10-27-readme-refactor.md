# README.md 重构(基于代码核对,删除过时内容)实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 以代码/脚本实测核对为依据,修正 README.md 中约 20 项过时或错误声明,补齐 2026-10-27 ARM64 spec policy 等缺失内容,删除已失效的表述。

**Architecture:** 文档重构——不改任何代码。三个探索 agent + LSP 已完成全量核对(证据见下);按"过时项清单"逐节修改 README,数字口径统一为 host-dependent(spec policy 后默认集随芯片能力过滤);完成后用本机构建实测 `--list-tests` 锚定数字,DCO 单 commit 提交。

**Tech Stack:** Markdown 文档;验证手段 = grep 源码证据 + `./builddir/sdcshield --list-tests` 实测。

**Spec:** 本计划自身即 spec(核对结果即需求);背景见 CLAUDE.md「ARM64 spec policy (2026-10-27)」条目。

## Global Constraints

- **One patch per unit**:README 重构 = 单个 commit,`git commit -s`,末行 `Signed-off-by: wangxu <wangxumarshall@qq.com>`,无 Co-Authored-By。
- 提交前 `git branch --show-current` 确认在特性分支(非 main),push 到该分支。
- **诚实口径**:所有用例数字必须标注"host-dependent"与实测日期/机型;无本机实测的数字引用 CLAUDE.md 2026-10-27 记录(SVE1 主机 ~478 / 无 SVE 鲲鹏 920 297)。
- 不虚构任何未核对过的事实;证据不足的表述宁可删除。
- x86 非回归:不涉及(纯文档)。

## 核对结论摘要(修改依据)

### A. 重大机制缺失/口径错误(P0)

| # | README 现状 | 实际(证据) | 处置 |
|---|---|---|---|
| A1 | 用例数固定"478 PROD / 484 q=0 / 489 q=-1"(实测 2026-10-26) | 2026-10-27 spec policy 合入后,选择期按 `arm64_requires` 过滤,默认集 **host-dependent**(CLAUDE.md:SVE1 ~478 / 无 SVE 297);`framework/sandstone_tests.cpp:99-125` | 全部数字改 host-dependent 口径 |
| A2 | 无 ARM64 spec 策略描述 | `struct test::arm64_requires`/`arm64_min_vl_bits`(`framework/sandstone.h:354-437`)、`arm64_spec_get()`(`framework/device/cpu/arm64/arm64_vl.h:245-250`)、`SDCSHIELD_ARM64_SPEC_MAX_VL`、控制线程 pin | 新增小节 |
| A3 | "四个代码检视机器人" | 五 bot:四外部 SaaS + 本地无头 claude(`scripts/pr_local_review.py`,`scripts/pr_loop.py` 头注、`docs/build-deploy-pr-ci/pr-loop.md`) | 改五 bot |
| A4 | "`-F` 首个 FAIL 即停——SKIP 不算失败,占位用例不会误停" | `sdc_detect.sh:132-143` 传 `--fatal-skips`,SKIP 按失败处理 | 修正 |
| A5 | knob 表多行过时:openblas mdim "16..4096 默认 256"、lu.n "16..2048"、sleef_neon "128..262144 默认 1024"、ipsec SVE "无此 knob"、"eigen NEON 家族、openssl×3 无 knob" | mdim/lu.n 实际 **16..16384、内存自适应默认、越界 clamp+warning**(`tests/cpu/openblas_gemm/dgemm.cpp:124-145`、`lu.cpp:119-126`);sleef **128..64M、自适应**(`sleef_neon.cpp:85-157`,sleef_sve 同构);ipsec SVE 有 `.data_size`;eigen_gemm_*.mdim / eigen_sparse.n / eigen_svd_bidiag_sve.mdim / openssl_sha{,3}.arena_bytes / openssl_sm3sm4.data_size / gather*.data_size / zlib* level / sve512 bounded/distance 均为真实 knob | 重写 knob 表 |
| A6 | CI 节缺 ci_triage/sanitizer/auto-pr;"docs-only PR 自动跳过" | `scripts/ci_triage.py` + `docs/build-deploy-pr-ci/ci-triage.md` 存在;`sanitizer.yaml`(每日 ASan+UBSan)、`auto-pr.yml` 存在;pr.yaml **无** paths 过滤,docs-only 不跳过 | 补齐+删错误声明 |
| A7 | `gh run download --name built-24.03-SP3` | 实际 artifact 名 `built-24.03-LTS-SP3`(sp 含 LTS- 前缀,`multi-os-verify.yml:320`) | 修示例 |

### B. 数字/事实小修(P1)

| # | README 现状 | 实际 | 处置 |
|---|---|---|---|
| B1 | mesh_upi_* 45(含 18 SVE) | **46**(新增 `mesh_upi_avx512_symm_f64`,README 他处已列) | 45→46 |
| B2 | 优先序 P1/P2 描述:arm64_sdc 属 P2 | `sdc_detect.sh:72` 正则首匹配,arm64_sdc 实归 **P1** | 修表 |
| B3 | "`container-build.sh` 注入 `-Denable_acl=disabled`" | 该 option 由 CI workflow 传(`multi-os-verify.yml:139`);container-build.sh 走 ACL 自动检测(`container-build.sh:290-329`) | 归属修正 |
| B4 | package-release "~6MB tarball" | 脚本头注释与设计文档写 **~20MB**(`package-release.sh:7`) | 6MB→20MB |
| B5 | campaign 冒烟 "~15min" | 脚本头注释 **~10min**(`run_sdc_campaign.sh:22`) | 修正 |
| B6 | vendored Eigen "5.0.0+" | 实际 **5.0.1**(`third-party/eigen5/Eigen/Version`) | 更新版本号 |
| B7 | "两个 Eigen SVE 用例逐档分派" | 实为 `eigen_gemm_double_max_arm`(4 档)+ `eigen_svd_cdouble_sve`(3 档);`eigen_svd_bidiag_sve` 是自写 Householder 真 VLA、非 Eigen 库(`tests/cpu/meson.build:1029-1084`) | 精确化表述 |
| B8 | "SVE 版 avx53 套件(tests/cpu/sve/,53 个)" | 53 为子集口径;tests/cpu/sve/ 目录现共 172 个用例 | 括注改"其中 53 个 avx 命名移植" |
| B9 | benchmark "nightly" | 每日 UTC 04:00 cron 驱动(同一 multi-os-verify.yml) | 措辞修正 |
| B10 | pocketfft knob 表缺 8191(同页正文有,自相矛盾) | 白名单含 8191(`tests/cpu/pocketfft/fft.cpp:165-183`) | 表补 8191 |
| B11 | zstd knob 分组:zstd1/zstd19 列有 level | `zstd` 有 level+maxbuffersize;`zstd1`/`zstd19` **仅 maxbuffersize**(level 固定);`zfuzz` 无 knob;`zstd_aaa`、`zlib*` 有 knob 未列(`tests/cpu/zstd/test.c:28-203`) | 重写该行 |
| B12 | 检视 bot 段/延伸节仅四 bot;CLAUDE.md ci-triage 手册无入口 | 五 bot + ci_triage | 补 |

### C. 保留不动(核对为一致)

- sdc_detect.sh 的 SDC_BIN 发现顺序/产物文件名/透传/`-h`、detect 子命令打包(2026-10-09 后)、CI tarball 未含;
- excite.sh 全部旗标/幂等/SMOKE_TIMEOUT_S=2400/M2 人工步骤;
- run_sdc_campaign.sh 阶段结构/.done/KNOWN_FLAKY/逐核二分(分类补列 `transient`);
- sdc-run.sh scan、sdc_machine_scan.sh、run_sdc_spectrum.sh、offline-build 各脚本其余声明;
- multi-os-verify.yml 主体、pr.yaml lint/DCO/build-cpu/multi-version 三角落、security/codeql、check-randomization R1/R2/R3、lts-stability 29 条目、全部文档链接;
- vendored 库版本(OpenSSL 3.5.0/OpenBLAS 0.3.29/SLEEF 3.9.0/isa-l 2.32.1/ACL v23.02);
- 旧二进制(2026-09-12)判读警示块——非过时(仍在流通),保留;
- excite 冒烟"实测约 23 分钟"——实测记录,保留。

---

### Task 1: 修正「一行命令」与优先序节(A4/B2)

**Files:**
- Modify: `README.md:39-68`

- [ ] **Step 1: 修正 `-F` 行**——改为同时说明 `--fatal-skips`(SKIP 也按失败停),删除"SKIP 不算失败,占位用例不会误停"表述。依据 `sdc_detect.sh:132-143`。
- [ ] **Step 2: 修正 P1/P2 桶描述**——P1 加 `arm64_sdc`/`acl_gemm`/`neon_add`(正则首匹配归 P1,`sdc_detect.sh:72-80`);P2 表述改为"sve512/power_virus_dit/ooo_dep_chain/lsu_store_forward/l2c_cross_cache_line 等"。
- [ ] **Step 3: 验证**——`grep -n "fatal-skips" scripts/run/sdc_detect.sh` 输出存在;README 无"占位用例不会误停"残留。

### Task 2: 用例数量口径 host-dependent 化(A1)+ 新增 spec policy 小节(A2)

**Files:**
- Modify: `README.md:166, 367, 393-402`(数字口径);`README.md:365` 前后(新小节)

- [ ] **Step 1: 新增「ARM64 spec 策略与默认用例集」小节**(置于「测试用例与检测能力」节首):
  - 一次探测(`arm64_spec_get()`:扩展位 + 架构最大档,读回内核实际授予);
  - 选择期按 `struct test::arm64_requires`/`arm64_min_vl_bits` 过滤 → 默认集 host-dependent(无 SVE 芯片如鲲鹏 920 远小于 SVE1 主机);
  - 控制线程在 `test_init` 前 pin 到芯片最大档,worker 继承并重 pin 验证读回;
  - `-e`/`--test-list-file` 旁路过滤(显式请求仍注册、诚实 SKIP);
  - `SDCSHIELD_ARM64_SPEC_MAX_VL=<bits>` 只降不升、重探测归一;
  - 引用 `scripts/check-arm64-requires.py` 检查器。
- [ ] **Step 2: 全部固定数字改口径**——"478"→"host-dependent:SVE1 主机 ~478 / 无 SVE 鲲鹏 920 297(2026-10-27 实测口径)";484/489 同口径说明;质量分级表 PROD 行与合计行改为"随 host 能力过滤,上界 489(源码全量)";`--list-tests` 构建注释行同步。
- [ ] **Step 3: 验证**——本机构建完成后 `./builddir/sdcshield --list-tests | wc -l` 与 `--quality=0`/`--quality=-1` 实测,把本机数字写进 README(机型+日期口径)。

### Task 3: 重写 knob 表(A5/B10/B11)+ 能力表小修(B1/B7/B8)

**Files:**
- Modify: `README.md:280-282, 378-382, 490-506`

- [ ] **Step 1: 重写「当前支持 knob 的测试全集」表**——按实测域修正:
  - `openblas_{d,s,z,c}gemm .mdim`:16..16384,**默认按内存自适应**([128,8192] 夹取,越界 clamp+warning);
  - `openblas_lu .n`:16..16384,自适应;
  - `sleef_neon/.sleef_sve .nelems`:128..67108864(4 倍数),自适应默认;
  - `pocketfft_fft .n`:512/1024/2048/4096/8192/16384/4099/**8191**/6144/10000;
  - zstd 行拆分:`zstd`(level+maxbuffersize)、`zstd1`/`zstd19`(仅 maxbuffersize,level 固定 1/19)、`zstd_aaa`、`zlib*`(level/maxbuffersize)、`zfuzz`(无 knob);
  - `ipsec_*` 行:SVE 24 变体有 `.data_size`(snake_case,上限按 HMAC 缓冲,默认自适应);
  - 补行:`eigen_gemm_*`(.mdim 16..4096/8192)、`eigen_gemm_double_max_arm.mdim`(1024..4096)、`eigen_svd_bidiag_sve.mdim`(64..2048)、`eigen_sparse.n`(64..8192)、`openssl_sha/.sha3 .arena_bytes`(1MiB..4GiB)、`openssl_sm3sm4.data_size`、`gather*/gatherscatter* .data_size`、`sve512_* .bounded`、`sve512_nt_reload_arm.distance`;
  - "仍无 knob"句改写:仅 `isal_crc*` 等仍为编译期常量。
- [ ] **Step 2: 能力表小修**——mesh 45→46;avx53 括注改"其中 53 个 avx 命名移植(目录共 172 个 SVE 用例)";Eigen 段"两个"表述精确化(gemm 4 档/svd 3 档,bidiag_sve 非 Eigen)。
- [ ] **Step 3: 正文中同步旧数字**——`README.md:280`(GEMM 尺寸节)、`README.md:381/498`(sleef)等正文的"默认 256/1024"与"越界 fail-loudly"同步为自适应+clamp 口径。
- [ ] **Step 4: 验证**——`grep -n "16..4096\|262144\|默认 256" README.md` 无残留旧口径(工作集表格里的 mdim 行仍有效,保留)。

### Task 4: CI/机器人节修正(A3/A6/A7/B9/B12)

**Files:**
- Modify: `README.md:219-234, 534-535`

- [ ] **Step 1: 检视机器人节改五 bot**——四外部 SaaS(codex/CodeRabbit/PR-Agent/Greptile)+ 第 5 个本地无头 claude 对抗检视(`scripts/pr_local_review.py`);`bot-coverage.yml` 仍轮询四外部 bot;链接 pr-loop.md。
- [ ] **Step 2: PR CI 节**——删除"docs-only PR 自动跳过"(pr.yaml 无 paths 过滤);补 `sanitizer.yaml`(每日 ASan+UBSan 夜巡)与 `auto-pr.yml`(特性分支自动开 PR)一句。
- [ ] **Step 3: 新增 CI 无人值守分诊**——`scripts/ci_triage.py` + `docs/build-deploy-pr-ci/ci-triage.md`:main CI 红 → issue → headless agent 修复 → fix/ci-* PR → pr-loop 闭环 → 人工 merge。
- [ ] **Step 4: 小修**——artifact 示例名 `built-24.03-LTS-SP3`;benchmark "nightly"→"每日 cron";campaign 分类补 `transient`。
- [ ] **Step 5: 验证**——`grep -n "四个代码检视\|docs-only\|built-24.03-SP3" README.md` 无残留。

### Task 5: 离线构建节小修(B3/B4/B5/B6)

**Files:**
- Modify: `README.md:186-217, 258`

- [ ] **Step 1: 修正**——`-Denable_acl=disabled` 归属 CI workflow(22.03 容器构建对 ACL 走自动检测);package-release ~6MB→~20MB;campaign 冒烟 ~15min→~10min;Eigen 5.0.0+→5.0.1(vendored 口径)。
- [ ] **Step 2: 验证**——`grep -n "6MB\|~15min\|enable_acl" README.md` 核对修正结果。

### Task 6: 整体校验 + 构建实测锚定 + 提交

- [ ] **Step 1: 全文一致性自查**——数字口径统一;无遗留矛盾(如同页两处 pocketfft 白名单);链接全部有效(agent 已核对存在的清单)。
- [ ] **Step 2: 构建与实测**——后台构建完成;`./builddir/sdcshield --list-tests | wc -l`、`--quality=0`、`--quality=-1` 三口径实测写入 README(标注机型 0xd02 SVE-256/openEuler/日期 2026-10-27);`-e zstd19 -t 2000 -n 1` 回归 `exit: pass`(规则 2c)。
- [ ] **Step 3: 提交推送**——`git checkout -b docs/readme-refactor-2026-10-27`(或沿用当前特性分支);`git add README.md && git commit -s`;push 该分支,不推 main。

## Self-Review

- 覆盖:核对清单 A/B 全部条目均已映射到 Task;C 类(一致项)明确不动。
- 占位符:无——每步给出具体改动与验证命令。
- 一致性:数字口径全文统一为 host-dependent;297(no-SVE)与 ~478(SVE1)来自 CLAUDE.md 2026-10-27 记录,本机实测数字以构建产物为准。
