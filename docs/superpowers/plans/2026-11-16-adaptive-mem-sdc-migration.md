# adaptive_mem_sdc 迁移 SDCShield 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (Native 执行, 用户已裁定 2026-11-16). Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把研究仓 `arm64-sdc-fuzzing/opendcdiag` 的 `adaptive_mem_sdc` 自适应组合轮转 SDC 测试用例（三文件 + SVE 引擎 TU）逐字节迁移进 sdcshield，在 aarch64 注册，本机验证 NEON 路径，SVE 路径按 sve_rot 先例过编译+objdump 门并诚实声明运行期边界。

**Architecture:** 三源文件（`adaptive_mem_sdc.h` / `adaptive_mem_sdc.cpp` NEON 引擎+DECLARE_TEST / `adaptive_mem_sdc_sve.cpp` SVE 引擎 TU）原样落位 `tests/cpu/misc/`；NEON TU 进 `tests/cpu/meson.build` 的 aarch64 门控源列表；SVE TU 进**新建独立 static_library**（+sve2 march 探针 + sanitizer 豁免，库名避撞既有 `tests_sve`）。框架 API 两侧兼容（DECLARE_TEST 签名同、fork_each_test 模型同、VERIFY/COVFILE 全 env 驱动自包含）。

**Tech Stack:** Meson + Ninja（`./scripts/build.sh` 规范入口）、GCC 12.3.1 aarch64、arm_sve.h intrinsics、沙岩框架。

**Spec:** 研究仓 `docs/ADAPTIVE_MEM_SDC_DESIGN_20261113.md`（v2, approved）——行为规格（30 组合表、kPriorityOrder、MODE=combat|test、ISA 阶梯、env 旋钮、验证协议）；数据源 `docs/20261114/adaptive_combo_sweep/RESULTS.md`（TTF 实测序）。本计划是**迁移**而非重新实现：spec 定义的行为已全部实现于迁移源，逐字节保真 + 门控注册即继承。

## Global Constraints

- **迁移源 = 研究仓工作树现状**（含 11-16 未提交改动：MODE=test、kPriorityOrder、ISA 阶梯、COMBO 锁定钩子）——不是研究仓 HEAD。三个源文件以 md5 全等为准。
- **一次一个单元一提交**（仓库铁律 1/3）：feat 提交（代码+meson）与 docs 计数同步提交分开；`git commit -s` DCO（`Signed-off-by: wangxu <wangxumarshall@qq.com>`，末行，无 Co-Authored-By）；只推 `feat/migrate-opendcdiag-tests`，提交前 `git branch --show-current` 核对。
- **工作树卫生**：sdcshield 工作树已有在飞改动（`tests/cpu/arm_add/memcpy*` ×9、`docs/research/progress-log.md`、third-party rpm 子模块指针）——**不属于本任务，一律不 add、不 stash、不 checkout**；所有 `git add` 逐文件点名。
- **x86-64 untouched**（铁律 5）：一切注册在 `if host_machine.cpu_family() == 'aarch64'` 门内；x86 非回归以检视断言（无任何 x86 路径触碰）。
- **5h 扫描互斥**（研究仓 exclusive 铁律）：研究仓 adaptive_mem_sdc 5h 扫描占用掩码 96-189 至 ~22:05——**期间**一切 sdcshield 构建/反汇编钉 `taskset -c 8-95` 且并行度 ≤12；**一切运行期验证（含轻载 verify 冒烟）与全核 combat 推迟到扫描收官后**。杀框架进程必须 SIGKILL + pgrep 验证（铁律）。
- **零新警告**（铁律 2a）：构建输出与迁移前基线 diff，无新增警告行。
- **SVE 验证档位（用户裁定 A）**：编译门 + objdump 编码门 + progress-log 诚实边界声明 + 后续战役挂账（对齐 sve_rot 家族 2026-10-24 先例与母仓 spec §8"运行期验证是后续战役"）。不做 gem5-fi 提核验证。

## Review Focus

1. **x86 注册泄漏**：若 NEON TU 误入 base 列表，x86 构建会注册一个恒 pass 的桩测试（铁律 6"no-op 报 pass 是 bug"）。→ Task 1 的 meson diff 检视步骤断言门控；commit message 载明 x86 untouched。
2. **库名撞车**：研究仓 SVE 库名 `tests_sve` 在 sdcshield 已被占用（dispatch TU 库）。→ Task 1 命名 `tests_sve_adaptive`，构建门直接暴露 meson 重复目标错。
3. **sanitizer ICE**：GCC 12.3 对 SVE intrinsics + 插桩有 codegen ICE（仓库文档：14 TU 全崩实证）。新 SVE 库不带 `sve_sanitize_exempt` 会炸 CI 夜巡。→ Task 1 库定义带豁免（对齐既有全部 SVE 库纪律）。
4. **sNaN 双副本复发**：研究仓 e91fbd55 修过 verify 期 sNaN 双副本假失配（3.01e8 轮零失配实证）——迁移源含此修复，保真即继承。→ Task 1 md5 门 + Task 2 verify 冒烟 30/30 组合零失配。
5. **测量污染**：扫描期任何 sdcshield 负载都会污染 5h 扫描的检出率口径（与 10min 臂对拍的整个目的）。→ Global Constraint 的钉核+推迟条款；Task 2 开头有扫描收官前置检查。

---

### Task 1: 三文件落位 + meson 注册 + 编译门

**Files:**
- Create: `tests/cpu/misc/adaptive_mem_sdc.h`（逐字节自研究仓 `tests/cpu/misc/adaptive_mem_sdc.h`）
- Create: `tests/cpu/misc/adaptive_mem_sdc.cpp`（同上）
- Create: `tests/cpu/misc/adaptive_mem_sdc_sve.cpp`（同上）
- Modify: `tests/cpu/meson.build`（两处：aarch64 门控列表加 NEON TU；SVE 库区新建 static_library 块）

**Interfaces:**
- Consumes: 框架 `DECLARE_TEST(name, "desc")` / `.quality_level = TEST_QUALITY_PROD` / `END_DECLARE_TEST`（sdcshield 兼容已证：8dda7f3a 同式迁移）；`struct test` 的 `.data` 字段；fork_each_test 循环模型（sdcshield `framework/sandstone_run.cpp:1681` 在）。
- Produces: 注册名 `adaptive_mem_sdc`（Task 2/4 引用）；库名 `tests_sve_adaptive`（Task 3 objdump 目标路径依据）；符号 `adaptive_sve_run_combo`（NEON TU 在 aarch64 无条件引用，链接完整性由构建门证明）。

- [ ] **Step 1: 复制三文件并 md5 门**

```bash
S=/home/sdc/root/arm64-sdc-fuzzing/opendcdiag/tests/cpu/misc
D=/home/sdc/wangxu/sdcshield/tests/cpu/misc
for f in adaptive_mem_sdc.h adaptive_mem_sdc.cpp adaptive_mem_sdc_sve.cpp; do
  cp "$S/$f" "$D/$f"
done
for f in adaptive_mem_sdc.h adaptive_mem_sdc.cpp adaptive_mem_sdc_sve.cpp; do
  md5sum "$S/$f" "$D/$f"
done
```
Expected: 三对 md5 逐一相等（迁移源=研究仓工作树现状）。

- [ ] **Step 2: meson 注册——NEON TU 入 aarch64 门控列表**

在 `tests/cpu/meson.build` 的 `if host_machine.cpu_family() == 'aarch64'` 门内、misc/ 研究迁移测试集群中（`'misc/movbe_dump_golden.cpp'` 所在列表），按该列表排序惯例插入 `'misc/adaptive_mem_sdc.cpp',`。该 TU 带 `#else /* !__aarch64__ */` 桩但**不入 base 列表**（x86 不注册，见 Review Focus 1）。

- [ ] **Step 3: meson 注册——SVE 引擎独立库块**

在 SVE 库区（`tests_sve_vla` 块之后）新增，镜像研究仓 `tests/cpu/meson.build:676-692` 的块并做两处 sdcshield 适配（库名 + sanitizer 豁免）：

```meson
# adaptive_mem_sdc SVE/SVE2 引擎 TU: 独立翻译单元用 +sve2 march 编译
# (研究仓逐字节迁移; 编译期无需 SVE 硬件——本机鲲鹏 920 编过、
#  运行期 HWCAP 永不选中; SVE 机上由 adaptive_mem_sdc.cpp 的
#  getauxval 探测分发)。库名避撞既有 tests_sve (dispatch TU 库);
#  sve_sanitize_exempt 对齐全部 SVE 库纪律 (GCC 12.3 SVE+插桩 ICE)。
if host_machine.cpu_family() == 'aarch64'
    sve2_march = cpp.compiles(
        '#include <arm_sve.h>\nint f(void) { return 0; }',
        args: ['-march=armv9-a+sve2'],
        name: 'armv9-a+sve2 compile probe',
    ) ? ['-march=armv9-a+sve2'] : ['-march=armv8.2-a+sve2']

    tests_sve_adaptive_a = static_library(
        'tests_sve_adaptive',
        sources: ['misc/adaptive_mem_sdc_sve.cpp'],
        build_by_default: false,
        include_directories: [tests_common_incdirs],
        cpp_args: [tests_common_cpp_args, sve2_march, sve_sanitize_exempt],
    )
    sandstone_tests += [tests_sve_adaptive_a]
endif
```

- [ ] **Step 4: 构建门（钉核，零新警告）**

```bash
cd /home/sdc/wangxu/sdcshield
taskset -c 8-95 ./scripts/build.sh -j 12 2>&1 | tee /tmp/sdcshield_build_adaptive.log
```
Expected: 构建成功；`grep -i "warning" /tmp/sdcshield_build_adaptive.log` 与迁移前基线对比**零新增**（新 TU 自身零警告）；meson 无重复目标错（Review Focus 2）；链接成（`adaptive_sve_run_combo` 符号解析，Review Focus 3 的编译面）。产出二进制 `builddir/sdcshield`。

- [ ] **Step 5: meson diff 检视（x86 untouched 断言）**

```bash
git diff tests/cpu/meson.build
```
Expected: 两处改动均在 `host_machine.cpu_family() == 'aarch64'` 门内；无任何 base/x86 列表触碰。

### Task 2: NEON 路径本机运行验证（5h 扫描收官后执行）

**Files:**
- Test: `builddir/sdcshield`（运行期验证，无新文件）

**Interfaces:**
- Consumes: Task 1 的注册名 `adaptive_mem_sdc` 与二进制；env 旋钮 `ADAPTIVE_MEM_SDC_VERIFY` / `ADAPTIVE_MEM_SDC_COVFILE`（研究仓 spec §6 语义）。
- Produces: 验证实录（progress-log 条目与 commit message 的证据引用）。

- [ ] **Step 1: 扫描收官前置检查**

```bash
pgrep -x opendcdiag || echo "scan done, clear to run"
```
Expected: 主进程已退（5h 扫描 ~22:05 收官，末槽 idle）。**仍存活则等待，不并行**（Review Focus 5）。

- [ ] **Step 2: 用例在场检查**

```bash
./builddir/sdcshield -l | grep adaptive_mem_sdc
```
Expected: 一行 `adaptive_mem_sdc`（PROD 级，默认集可见）。

- [ ] **Step 3: verify 模式黄金冒烟——30 组合全覆盖零失配**

```bash
ADAPTIVE_MEM_SDC_VERIFY=1 ADAPTIVE_MEM_SDC_COVFILE=/tmp/adaptive_cov.txt \
  taskset -c 8-95 ./builddir/sdcshield -e adaptive_mem_sdc -n 4 -t 420s \
  --ignore-os-error --ignore-timeout -o /tmp/adaptive_verify.log -q; echo "exit=$?"
sort -k3 -n /tmp/adaptive_cov.txt | awk '{print $3}' | sort -u | wc -l
grep -c "result: fail" /tmp/adaptive_verify.log || true
```
Expected: `exit=0`；cov 覆盖数 = **30**（0..29 全轮转——时长是旋钮不是断言：覆盖不足 30 则倍增 `-t` 重跑直至覆盖；研究仓 gv 门先例 318 pass）；fail 块数 = **0！！！**（Review Focus 4：sNaN 修复保真证明）。

- [ ] **Step 4: combat 模式真检出——179 上真 SDC + 组合标签**

```bash
taskset -c 96-189 ./builddir/sdcshield -e adaptive_mem_sdc -n 94 -t 180s \
  --ignore-os-error --ignore-timeout -o /tmp/adaptive_combat.log -q; echo "exit=$?"
grep -A8 "result: fail" /tmp/adaptive_combat.log | grep "adaptive_mem_sdc combo#"
grep -A8 "result: fail" /tmp/adaptive_combat.log | grep "id: {" | sort | uniq -c
```
Expected: exit≠0（fail 路径通）；≥1 条 `adaptive_mem_sdc combo#N [...] data miscompare`（标签格式含 dtype/ldr/str/载入族）；fail 块 id 行 `logical: 179` 三字段互证（本机 179 缺陷核，TTF 6-45s/组合量级，spec §8 预期）。这是"affected behavior 真实输出"（铁律 2b）的最强形态。

- [ ] **Step 5: 仓库回归门**

```bash
./builddir/sdcshield -e zstd19 -t 2000 -n 1 -o /tmp/zstd19_reg.log -q; echo "exit=$?"; grep "result:" /tmp/zstd19_reg.log | tail -1
```
Expected: `exit=0`，`result: pass`（铁律 2c）。

### Task 3: SVE 引擎编译+objdump 编码门（可在扫描期执行，钉核）

**Files:**
- Test: `builddir/` 下 `tests_sve_adaptive` 库对象（构建产物，无新文件）

**Interfaces:**
- Consumes: Task 1 的 `tests_sve_adaptive` 库与 Step 4 产物。
- Produces: SVE 编码门证据（progress-log 边界声明的支撑材料）。

- [ ] **Step 1: 定位引擎对象文件**

```bash
find builddir -path "*tests_sve_adaptive*" -name "*.o" -o -path "*tests_sve_adaptive*" -name "*.a" | head
```
Expected: `libtests_sve_adaptive.a.p/adaptive_mem_sdc_sve.cpp.o` 类路径存在。

- [ ] **Step 2: SVE 指令在场 + 动态 VL 几何门**

```bash
OBJ=<Step1 路径>
objdump -d "$OBJ" | grep -cE "\b(st1d|st1w|ld1d|ld1w|add\tz|eor\tz|and\tz|mul\tz)\b"   # SVE 数据通路指令 >0
objdump -d "$OBJ" | grep -cE "\b(cntd|cntw|cntb|rdvl|whilelt)\b"                       # VL 动态几何 >0
objdump -d "$OBJ" | grep -cE "\bsqdmulh\b|svptrue_pat 固宽模式"                        # SVE2-only 槽 = 0（头注释声明 future extension）
```
Expected: 前两个计数 >0（引擎真有 SVE 编码且 VL 运行期自适应）；第三个 = 0（无 SVE2-only 指令——与"armv8.2-a+sve 可编译"的设计声明一致）。

- [ ] **Step 3: 与研究仓对象编码对拍**

```bash
RSRC=$(find /home/sdc/root/arm64-sdc-fuzzing/opendcdiag/builddir -name "adaptive_mem_sdc_sve.cpp.o" | head -1)
diff <(objdump -d "$OBJ" | awk '{$1=""; print}' | grep -E "^\s") \
     <(objdump -d "$RSRC" | awk '{$1=""; print}' | grep -E "^\s") | head -20
```
Expected: 指令序列一致或差异可由两侧 `tests_common_cpp_args` 旗标差完全解释（同一 GCC 12.3.1 + 同一 +sve2 march；差异行须逐条归因后入 progress-log）。研究仓对象不存在时改为只做 Step 2 三断言并在 progress-log 注明对拍缺位原因。

### Task 4: docs 同步 + 提交 + 推送

**Files:**
- Modify: `README.md`（用例计数行 ~339）
- Modify: `CLAUDE.md`（计数相关行，对照 3cadb99a 先例的 4 行）
- Modify: `docs/research/progress-log.md`（**追加**条目；该文件已有他人在飞未提交内容——只追加不改动既有行，提交策略见 Step 4）

**Interfaces:**
- Consumes: Task 1-3 全部门通过的证据；Task 2 Step 2 实测计数。
- Produces: 两个提交（feat + docs）推送至 `feat/migrate-opendcdiag-tests`。

- [ ] **Step 1: 实测计数三口径**

```bash
./builddir/sdcshield -l | wc -l; ./builddir/sdcshield --quality=0 -l | wc -l; ./builddir/sdcshield --quality=-1 -l | wc -l
```
Expected: 相对迁移前各 **+1**（README 现值 519/628/632 → 520/629/633；以实测为准）。

- [ ] **Step 2: README.md + CLAUDE.md 计数同步**

按 3cadb99a 先例格式更新三口径数字，措辞含 `+1 PROD, 2026-11-16 measured`。

- [ ] **Step 3: progress-log 追加条目**

内容四要素：① 溯源（逐字节自研究仓工作树 2026-11-16 现状，含 MODE=test/kPriorityOrder/ISA 阶梯；spec=ADAPTIVE_MEM_SDC_DESIGN_20261113.md v2）；② NEON 验证实录（verify 30/30 零失配！！！、combat 179 真检出+组合标签、zstd19 回归）；③ SVE 边界声明（编译+objdump 门通过；运行期验证未做，对齐 sve_rot 2026-10-24 先例与母仓 spec §8，后续战役挂账——SVE 机或 gem5-fi 提核）；④ TTF 优先序数据源（研究仓 20261114 全扫 RESULTS.md）。

- [ ] **Step 4: 提交与推送（工作树卫生）**

```bash
git branch --show-current    # 必须 feat/migrate-opendcdiag-tests
git add tests/cpu/misc/adaptive_mem_sdc.h tests/cpu/misc/adaptive_mem_sdc.cpp \
        tests/cpu/misc/adaptive_mem_sdc_sve.cpp tests/cpu/meson.build \
        docs/superpowers/plans/2026-11-16-adaptive-mem-sdc-migration.md
git commit -s -m "feat(arm64): migrate adaptive_mem_sdc adaptive combo-rotation SDC test from research repo"
git add README.md CLAUDE.md
git commit -s -m "docs: sync counts after adaptive_mem_sdc migration (+1 PROD, 2026-11-16 measured)"
git push origin feat/migrate-opendcdiag-tests
```
Expected: 两提交干净（`git show --stat` 只含点名文件，**绝无** arm_add/memcpy*、rpm 子模块混入）。progress-log 若仍含他人在飞内容则**该文件不入提交**，条目留工作树并在汇报中言明（提交与否由用户裁决——不代他人提交未收口的战役台账）。
