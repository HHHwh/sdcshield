# pr-docs triage 报告：PR 629 / PR 628（2026-11-18）

角色：pr-issue-triage / pr-docs。范围：2 个 docs-only PR（无 `WIP:` 前缀、属快道），
triage 既有 bot findings、修真实问题 push 回 head 分支、rule 2 自证。merge 决定权
在 orchestrator（gate 批准后 `gh pr merge --squash`），本报告只给结论与证据。

## 结论速览

| PR | head（triage 后） | findings | 冲突 | 验证 | 快道可合并 |
|---|---|---|---|---|---|
| 629 readme-deep-verify | `df073e17`（未 push，无需） | 10 条全部已修，独立复核通过 | 无（CLEAN） | 见 §2 | **可** |
| 628 agent-full-automation-cn-render-fix | `5f833089` → **`ec3e7bab`（本次 push）** | 2 条全部已修，复核通过 | CONFLICTING → **已解，MERGEABLE** | 见 §3 | **可**（待新 head 的 bot 静默窗 + CI 走完） |

---

## 1. PR 629（docs/readme-deep-verify）

### 1.1 findings triage（10 条，3 轮 review，全部 by 本地 claude bot）

三轮对应关系：第 1 轮（11-10 13:51，4 条 Important/Minor）→ 11-17 06:05–06:09 五个
修复 commit；第 2 轮（11-17 09:48，3 条）→ 10:37/10:38 两个修复 commit
（dc1371cc、df073e17）；第 3 轮（11-17 10:02，2 条）与第 2 轮同题，是对修复前
状态的复审，其诉求同样由 dc1371cc/df073e17 覆盖。逐条：

| # | 位置 | 级别 | 问题 | 修复 commit | 复核结果 |
|---|---|---|---|---|---|
| 4525656394 | README L363 | Important | 锁族分解式 47=31+14+2 两子项证伪（_neon 实 12；31 混入 aarch64-only unaligned） | a4235baf | **已修且独立证实**（见 1.2） |
| 4525656407 | README L126 | Important | 499→569 后保留失效归因「少的是 vendored 组」 | a0487b28 + dc1371cc | 已修：归因改双边（vendored 组 + 冻结后增量探针） |
| 4525656417 | plan :28 | Minor | 分级表行 ✓ 靠算术达成、表头/脚注口径互斥 | 935978c0 | 已修：表头改「源码全量口径：tests section 机械计数」 |
| 4525656429 | plan :56 | Minor | 「非推算」边界声明与自身矩阵矛盾 | 76876fe0 | 已修：明示 569/687 为机判口径+算术、非本机枚举 |
| 4525656438 | plan :16 | Minor | 主机名「cortex x3b」失实 | 6bc02381 | 已修：Kunpeng 0xd02（HiSilicon 0x48） |
| 4597842160 | README L126 | Important | 「非 vendored」桶装 vendored 的 eigen-SVE 8，括注自相矛盾 | dc1371cc | 已修：标签改中性「增量探针」，点名 vendored 11=eigen-SVE 8+aor/cbw 3 |
| 4597842169 | plan :50 | Minor | 修复节滞留旧分解式 47=31+14+2 | df073e17 | 已修：同步为 30 双架构+17 aarch64-only |
| 4597842177 | README L126 | Minor | `_neon` 14 vs 12 指代不一致 | dc1371cc | 已修：L126 改称「NEON 锁变体 14」，与 L363「12 `_neon`」不再冲突 |
| 4597991587 | plan :50 | Minor | 同 4597842169（复审时 fix 尚未落） | df073e17 | 同上，已修 |
| 4597991604 | README L126 | Minor | 同 4597842160 + 「58 是 merge 增量非冻结后全量」 | dc1371cc | 已修：限定「2026-11-03 merge 增量 58 PROD」 |

### 1.2 独立复核（不轻信修复文本，全部对源码/二进制重证）

- **锁/原子族 47 = 30 双架构 + 17 aarch64-only（12 `_neon` + 2 stale_read + 2
  even_n/odd_n + 1 unaligned）——逐名实证吻合**：
  - 五目录 DECLARE_TEST 枚举 lock 9 + atomic 6 + spinlock 19 + spinlock_add 3 +
    neon_lock 13 = 50；其中 `atomic_seq_cst` 不匹配族四前缀（`lock*`/
    `lockless_cmpxchg*`/`atomic_simd_*`/`spinlock_*`）→ 族内 49；减
    `tests/cpu/meson.build:258/260` 注释掉的 `atomic_simd_128/256_unaligned` 2 个
    → **47** ✓
  - 分解：`_neon` 后缀 12（neon_lock 目录 11 + `spinlock_add_neon` 1，
    `even_odd_neon.cpp:203/225` 的 `spinlock_stress_cmpxchg_even_n/_odd_n` 确为
    `_n` 后缀）+ `spinlock_stale_read{,_xchg}` 2 + `spinlock_unaligned` 1（meson
    `aarch64` 门在 881 行起，USCAT 理由注释在案）+ 双架构基础 30（lock 9 +
    atomic_simd 3 + spinlock 主套 18）= 47 ✓
  - 运行时：本机（HiSilicon Kunpeng-920，implementer 0x48 / part 0xd01，无 SVE）
    q-1 全量名单 grep 族四前缀 = **47** ✓（锁族不依赖 vendored 库，缺依赖构建下
    结论有效。主机身份更正见 §3 开头）
- **L126 算术自洽**：58 = 21+7+14+9+8−1 ✓；vendored 至多 11 = 8+3 ✓（−1 退役的
  bidiag 亦 vendored，故实为 10，「至多 11」上界仍真）；余 ≥47 ✓。所引为
  2026-11-03 merge 增量账目（时名 `arm_add/`，2026-11-03 同日更名
  `neon_memcpy_cache/`，meson.build:924 在案），作为日期账目成立。
- **569/687**：plan :56 已如实披露为 11-03 机判口径 + 算术（687=683+4、
  569=565+4，+4 为 SME/SVE2-only PROD，四名 `sme_fmopa_stress_arm`/
  `sme_fmopa_za_arm`/`sme_za_tileview_arm`/`sve2_cross_precision_arm` 于 L394
  脚注点名）。本机为 **no-SVE 主机**（非 plan 的 0xd02 SVE1 机），无法复测 SVE1
  三档 565/675/683——该项验证边界与 plan 披露一致，不视为缺陷。
  （旁证：本机缺 vendored 库构建实测 382 PROD/490 q0/494 q-1、`_test_` 符号
  687 个、tests section 686 实例×176B=120,736B 整除；对照 CLAUDE.md no-SVE
  全依赖口径 458/566/570，差额 76 ≈ ipsec 70 + acl 2 + sleef 1 等缺位组，自洽。）
- **行号引用**：plan 文档 14 处 README/源码行号引用在 **PR 自身树**上 14/14 准确
  （dgemm.cpp:26、sdc_detect.sh:43/:72、excite.sh:87、arm64_vl.h:289/:391、
  sandstone.h:433/:437/:507/:747 及 README 各行）。
- **构建**：见 §3（构建在 628 树上跑的 main+628 与 629 同源码；629 为 docs-only，
  改动 2 文件均非构建输入）。

### 1.3 遗留观察（不阻塞快道，如实披露）

- 实测 merge main 进 629 树（干净自动合并后即 abort，未 push）：合并后 plan 的
  14 处行号引用 13 处仍准，唯「Eigen SVD」段 L540→L541 漂 1 行——**由 main 自身
  增长（bfb4e964/a2b6971e 等 seed 披露插入）所致，非 plan 写错**。未追改：行号
  随 main 演进必然再漂，且 push 任何 cosmetic 修复都会重启 5-bot review 周期 +
  静默窗，延迟当夜快道合并；plan 行自身已内容锚定（「Eigen 版本」），读者可
  grep 定位。若 orchestrator 判断需要，可另起 docs PR 收口。
- 分级表 8/110/569/687 为 11-03 时点快照，main 侧 L394 脚注已如实披露
  「687→701」（11-13 +14 `_sve2` 配套）——表内数字与脚注披露并存、口径自洽，
  非 629 引入的问题，不动。

### 1.4 快道判定

CLEAN/MERGEABLE、docs-only、无 `WIP:`、findings 全处置 → **可快道合并**。
无需任何新 push。

---

## 2. PR 628（docs/agent-full-automation-cn-render-fix）

### 2.1 findings triage（2 条）

| # | 位置 | 级别 | 问题 | 修复 commit | 复核结果 |
|---|---|---|---|---|---|
| 4525273533 | CN :9 译注 | Minor | 英文版同病只挂译注无跟踪 | cb92d3e2 | **已修且实证**：EN 状态机围栏 L45 顶层、内容行无 `> ` 前缀，译注同步为「中英文版均已迁至顶层」 |
| JSON 挂 body | EN :134 | Minor | 甘特闭合围栏缩进 4 空格非法闭合、吞 Citations 节 | 5f833089 | **已修且实证**：闭合围栏列 0（`cat -A`），Citations 节存续 |

### 2.2 冲突处置（本次 push 的实质工作）

PR 与 main **CONFLICTING**：main `65cf83b4`（2026-11-13 红线反转 7 文档）与本支
在状态机图区双向改动。解法（`ec3e7bab`，merge commit）：**结构取本支**（mermaid
围栏顶层——本 PR 的存在目的）**+ 内容取 main**（「等待合并关口/Await Merge Gate」
label、merge 门正文表述、译注④红线反转）。译注区两支各有一个「④」：main 的 ④
（合并政策）保留原号，本支的渲染修复注顺延为 **⑤**，并如实注明图中「等待合并
关口」一行系随 ④ 政策反转同步、非迁移所致。

净差异复核（`git diff origin/main`）：仅 EN/CN 状态机围栏去块引用、EN 甘特闭合
围栏去缩进、CN 译注⑤，43+/40−，全部在 PR 意图内；main 的政策内容全量保留。

### 2.3 rule 2 自证（docs 口径：构建 + 渲染 + 引用）

- **构建**：`PKG_CONFIG_PATH=third-party/eigen5 meson setup`（cpp_std=gnu++20 +
  cpp23 polyfill，本机 GCC 10.3 同主树口径）→ `./scripts/build.sh` 858/858
  targets、exit 0、`sdcshield` 链接成功、零新警告（改动仅 2 个 .md，非构建输入）。
- **渲染**：mermaid 11.17.2 无头 parse（jsdom shim）**4/4 PASS**——EN/CN 状态机
  （含 main 新 label）+ EN/CN 甘特图；GitHub `POST /markdown`（gfm）渲染两文件
  各 7×h2、2×table、**2×`highlight-source-mermaid`** div（GitHub 识别为可渲染
  mermaid）、Citations/引用节存续（无围栏吞节）、CN 仅剩译注 1 个 blockquote、
  「Await Merge Gate」label 在渲染 HTML 中逐 token 在位。
- **lint**：tabs 0、codespell 0 findings。
- **push 后状态**：`5f833089..ec3e7bab` → PR **MERGEABLE**（UNSTABLE = 新 head
  CI/bot 轮在跑，auto-pr/pr-agent 已 pass）；files 仍恰为 2 个目标文档。

### 2.4 快道判定

docs-only、无 `WIP:`、findings 全处置、冲突已解、MERGEABLE → **可快道合并**
（待新 head ec3e7bab 的 bot 完成信号 + 300s 静默窗 + lint/git-sanity 由夜间
pipeline 门判，按 spec 2026-11-12-full-auto-pipeline 走）。

---

## 3. 验证边界（诚实披露）

- **主机身份更正（review fix 轮，2026-11-19）**：初版报告写「no-SVE 鲲鹏
  （`firestorm` 0xd01）」，把 `--dump-cpu-info` 的 `Detected CPU: firestorm` 当
  主机标签——该串是 `framework/device/cpu/simd-arm.conf:141` 按 part 0xd01 的
  **特征位匹配别名**（该行注释自述 "Apple M1 Firestorm-equivalent (approx)"），
  非 MIDR 身份，与本 PR 629 修正过的「cortex x3b」（finding 4525656438）同类。
  本机实测：`lscpu` Model name = **Kunpeng-920**（HiSilicon，implementer 0x48 /
  part 0xd01），hostname cathelper-k8s-worker2，SVE/SVE2/SME 全无。验证行为发生
  在鲲鹏目标平台上，结论不受影响。
- 565/675/683（SVE1 三档）无法在本机复测：本机为无 SVE 的 HiSilicon
  Kunpeng-920（part 0xd01），非 plan 记录的 0xd02 SVE1 机；该项以 plan :56 的
  机判口径披露为准，未发现矛盾。
- 本机构建缺 vendored 5 库 install/（meson log 实证 ipsec/ssl/sleef/isal/acl/
  openblas 组构建期缺席），故 382/490/494 与 CLAUDE.md no-SVE 全依赖口径
  458/566/570 的差 76 即缺位组数，自洽但不构成对全依赖口径的直接复测。
- PR 629 未做任何 push（无必要）；PR 628 仅 push 合 main 的 merge commit
  `ec3e7bab` 一个。
