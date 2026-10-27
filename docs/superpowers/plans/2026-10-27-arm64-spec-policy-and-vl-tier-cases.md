# ARM64 规格策略与 VL 分档用例 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 sdcshield 在运行时探测芯片的 ARM64 向量规格（SVE / SVE2 / SME / SME2 + 最高档），默认只跑该芯片支持的最高规格用例，并在 worker 线程显式按最高档执行；同时把唯一两个 tier-locked 的 Eigen 用例改成四档变体 + 函数指针分派。

**Architecture:** 三层。①**探测层** `arm64_spec`（新增于 `arm64_vl.h`，一次性探测并缓存；最大值用临时线程请求架构上限、由内核向下取整授予）。②**策略层**：`struct test` 增 `arm64_requires`/`arm64_min_vl_bits`；`SandstoneTestSet` 构造期按 spec 过滤（`-e`/`--test-list-file` 旁路）；worker 线程在 `test_run` 前按最高档拉起并读回校验。③**用例层**：192 个向量用例中仅 3 个需要改代码 —— xlswave 几何寄存器化（T2）、Eigen 两例四档变体 + 分派（T3）；其余 189 个已真 VLA，只补标签与证据。

**Tech Stack:** C++20 / C17、GCC-12.3.1（本机）与 GCC-16.2.0（CI）、meson+ninja、`arm64_vl.h`（`getauxval` + `.inst` 编码 `rdvl`/`rdsvl` + `prctl`）、gem5-fi CHAOS/SE（`--sve_vl/--sme_vl {1,2,4,8}`）。

**Spec:** `docs/superpowers/specs/2026-10-27-arm64-spec-policy-and-vl-tier-cases-design.md`

## Global Constraints

- **DCO**：`git commit -s`，末行 `Signed-off-by: wangxu <wangxumarshall@qq.com>`，其后无内容，无 `Co-Authored-By`。
- **分支**：提交前 `git branch --show-current`；**永不推 main**；一补丁一单元。
- **验证（规则 2）**：`./scripts/build.sh` 零新警告；运行受影响行为并引用真实输出；回归 `./builddir/sdcshield -e zstd19 -t 2000 -n 1` → `exit: pass`。
- **x86-64 零改动（规则 5）**：新字段在非 aarch64 恒 0；新逻辑全部在 `#if defined(__aarch64__)` 内。
- **档位常量**：128/256/512/1024 四档；**2048 只预留**（`ARM64_SPEC_MAX_TIER_BITS = 2048` 作为探测请求上限，不编译变体、不验证）。
- **本机能力**（2026-10-27 实测）：SVE 有、最高 256；SVE2 / SME / SME2 全无。
- **诚实边界**：gem5 验证物是同源内核/变体，不是框架二进制（二进制在 gem5 SE 受 pre-main page fault / prctl 致命 / 信号忽略三道墙）。
- **构建环境**（本机）：`PKG_CONFIG_PATH=./third-party/eigen5:/home/sdc/boost-local/usr/lib64/pkgconfig`，并需
  `-Dc_args/-Dcpp_args/-Dc_link_args/-Dcpp_link_args=-I/home/sdc/boost-local/usr/include`；
  vendored `install/` 需存在（可从既有树软链）。

---

### Task 1: `arm64_spec` 探测层（U1）

**Files:**
- Modify: `framework/device/cpu/arm64/arm64_vl.h`（追加 spec 结构与探测函数）
- Modify: `framework/device/cpu/cpu_device.cpp:69-80`（dump 增一行）
- Modify: `framework/selftest.cpp`（新增 `selftest_arm64_spec`）

**Interfaces:**
- Produces: `struct arm64_spec`、`const struct arm64_spec *arm64_spec_get(void)`、
  `const char *arm64_spec_describe(char *buf, size_t len)`、常量
  `ARM64_SPEC_MAX_TIER_BITS`(2048)、`ARM64_REQUIRES_SVE/SVE2/SME/SME2`(1/2/4/8)。

- [ ] **Step 1: 写失败的自检** — 在 `framework/selftest.cpp` 末尾新增：

```cpp
static int selftest_arm64_spec_run(struct test *test, int cpu)
{
    (void)cpu;
#if defined(__aarch64__) && defined(__linux__)
    const struct arm64_spec *s = arm64_spec_get();
    if (!s || s->max_sve_vl_bits < 0 || s->max_sve_vl_bits > 2048) {
        log_error("%s: arm64_spec_get() returned an impossible spec", test->id);
        return EXIT_FAILURE;
    }
    if (s->has_sve != arm64_cpu_has_sve() || s->has_sve2 != arm64_cpu_has_sve2()
            || s->has_sme != arm64_cpu_has_sme()) {
        log_error("%s: spec cache disagrees with the raw probes", test->id);
        return EXIT_FAILURE;
    }
    if (s->has_sve && (s->max_sve_vl_bits < 128 || s->max_sve_vl_bits > 2048)) {
        log_error("%s: max_sve_vl_bits %ld outside [128,2048]",
                  test->id, s->max_sve_vl_bits);
        return EXIT_FAILURE;
    }
#else
    (void)test;
#endif
    return EXIT_SUCCESS;
}

DECLARE_TEST(selftest_arm64_spec, "ARM64 spec probe self-test")
    .test_run = selftest_arm64_spec_run,
    .quality_level = TEST_QUALITY_SKIP,
END_DECLARE_TEST
```

- [ ] **Step 2: 跑它，确认失败** — `./builddir/sdcshield -e selftest_arm64_spec --quality=-1 -n 1` → 期望编译失败（`arm64_spec_get` 未声明）。

- [ ] **Step 3: 实现** — 在 `arm64_vl.h` 的 `arm64_require_*` 之前追加：

```c
/* ---- chip spec: one-shot probe, cached (design §4.1) ---- */
#define ARM64_SPEC_MAX_TIER_BITS 2048   /* 2048 reserved: probe ceiling only */
#define ARM64_REQUIRES_SVE  (1u << 0)
#define ARM64_REQUIRES_SVE2 (1u << 1)
#define ARM64_REQUIRES_SME  (1u << 2)
#define ARM64_REQUIRES_SME2 (1u << 3)

struct arm64_spec {
    bool has_sve, has_sve2, has_sme, has_sme2;
    long max_sve_vl_bits;
    long max_sme_svl_bits;
    bool max_vl_known;
};

/* Probe the MAXIMUM supported VL: PR_SVE_SET_VL is per-thread, so do it on a
 * throwaway pthread, request the architectural ceiling (2048-bit), and read
 * back what the kernel granted (round-down semantics). The caller's thread is
 * untouched. If prctl is refused entirely (seccomp / emulator), fall back to
 * the current thread's VL and mark max_vl_known=false — never claim a maximum
 * we could not measure. */
static inline void *arm64_spec_probe_thread(void *arg)
{
    long *out = (long *)arg;
    long cur = arm64_sve_vl_bytes();
    if (cur <= 0) { *out = 0; return NULL; }
    int rc = prctl(PR_SVE_SET_VL, (unsigned long)(ARM64_SPEC_MAX_TIER_BITS / 8));
    long got = (rc > 0) ? arm64_sve_vl_bytes() : 0;
    *out = (got > 0) ? got : cur;
    return NULL;
}

static inline const struct arm64_spec *arm64_spec_get(void)
{
    static struct arm64_spec spec;
    static bool done;
    if (done) return &spec;

    spec.has_sve  = arm64_cpu_has_sve();
    spec.has_sve2 = arm64_cpu_has_sve2();
    spec.has_sme  = arm64_cpu_has_sme();
    spec.has_sme2 = (getauxval(AT_HWCAP2) & (1UL << 37)) != 0;  /* HWCAP2_SME2 */
    spec.max_vl_known = false;

    if (spec.has_sve) {
        long bytes = 0;
        pthread_t t;
        if (pthread_create(&t, NULL, arm64_spec_probe_thread, &bytes) == 0) {
            pthread_join(t, NULL);
        }
        if (bytes <= 0) bytes = arm64_sve_vl_bytes();
        spec.max_sve_vl_bits = bytes > 0 ? bytes * 8 : 0;
        spec.max_vl_known = (bytes > 0)
            && (bytes * 8 <= ARM64_SPEC_MAX_TIER_BITS);
    }
    if (spec.has_sme) {
        long cur = arm64_sme_svl_bytes();
        int rc = prctl(PR_SME_SET_VL, (unsigned long)(ARM64_SPEC_MAX_TIER_BITS / 8));
        long got = (rc > 0) ? arm64_sme_svl_bytes() : 0;
        spec.max_sme_svl_bits = (got > 0 ? got : cur) * 8;
    }
    done = true;
    return &spec;
}

static inline const char *arm64_spec_describe(char *buf, size_t len)
{
    const struct arm64_spec *s = arm64_spec_get();
    if (s->has_sve) {
        if (s->max_vl_known)
            snprintf(buf, len, "SVE yes (max VL %ld bits)", s->max_sve_vl_bits);
        else
            snprintf(buf, len, "SVE yes (max VL unknown, fell back to %ld bits)",
                     s->max_sve_vl_bits);
    } else {
        snprintf(buf, len, "SVE no");
    }
    snprintf(buf + strlen(buf), len - strlen(buf), " | SVE2 %s | SME %s | SME2 %s",
             s->has_sve2 ? "yes" : "no", s->has_sme ? "yes" : "no",
             s->has_sme2 ? "yes" : "no");
    return buf;
}
```

需在文件顶部 include `<pthread.h>`、`<string.h>`、`<stdio.h>`（`arm64_vl.h` 现有 include 见文件头）。

- [ ] **Step 4: dump 增一行** — `framework/device/cpu/cpu_device.cpp` 在两行 VL 输出之后：

```cpp
#if defined(__aarch64__) && defined(__linux__)
    {
        char specbuf[128];
        printf("ARM64 spec: %s\n", arm64_spec_describe(specbuf, sizeof(specbuf)));
    }
#endif
```

- [ ] **Step 5: 验证（真实输出）**

```bash
./scripts/build.sh
./builddir/sdcshield --dump-cpu-info | grep "ARM64 spec"
# 期望：ARM64 spec: SVE yes (max VL 256 bits) | SVE2 no | SME no | SME2 no
./builddir/sdcshield -e selftest_arm64_spec --quality=-1 -n 1 | tail -2
# 期望：exit: pass
./builddir/sdcshield -e zstd19 -t 2000 -n 1 | tail -1   # exit: pass
```

- [ ] **Step 6: 提交**

```bash
git branch --show-current   # 期望 feat/arm64-spec-policy
git add framework/device/cpu/arm64/arm64_vl.h framework/device/cpu/cpu_device.cpp framework/selftest.cpp
git commit -s -m "feat(arm64): 新增 arm64_spec 芯片规格探测层 + dump-cpu-info 输出 + selftest"
```

---

### Task 2: `struct test` 元数据（U2）

**Files:**
- Modify: `framework/sandstone.h:354-372`（`struct test` 增两字段）
- Modify: `framework/selftest.cpp`（自检覆盖新字段）

**Interfaces:**
- Consumes: `ARM64_REQUIRES_*`（Task 1）
- Produces: `test::arm64_requires`（`uint8_t`）、`test::arm64_min_vl_bits`（`long`）。默认 0。

- [ ] **Step 1:** 在 `struct test` 的 `minimum_cpu` 之后插入：

```c
    /// ARM64 spec requirements: bit0=SVE bit1=SVE2 bit2=SME bit3=SME2 (0 = none).
    /// Unlike minimum_cpu this is checked at SELECTION time (see
    /// SandstoneTestSet) and drives the per-worker VL pin; it never grows
    /// device_features (VL is a per-thread runtime quantity, not a feature bit).
    uint8_t arm64_requires;

    /// Minimum VL tier this test needs, in bits (0 = any tier).
    long arm64_min_vl_bits;
```

- [ ] **Step 2:** 自检：在 `selftest_arm64_spec_run` 中加一段——遍历 `test_source()`，断言
  `arm64_min_vl_bits` 只可能是 `0/128/256/512/1024`（2048 预留但本轮不应出现）。
  代码：

```cpp
    for (struct test &t : test_source()) {
        long v = t.arm64_min_vl_bits;
        if (v != 0 && v != 128 && v != 256 && v != 512 && v != 1024 && v != 2048) {
            log_error("%s: test %s declares a non-tier arm64_min_vl_bits=%ld",
                      test->id, t.id, v);
            return EXIT_FAILURE;
        }
    }
```

- [ ] **Step 3:** `./scripts/build.sh` 零告警；`-e selftest_arm64_spec --quality=-1 -n 1` pass；`-e zstd19` pass。

- [ ] **Step 4:** 提交 `feat(framework): struct test 增 arm64_requires/arm64_min_vl_bits 元数据`。

---

### Task 3: 选择期过滤（U3）

**Files:**
- Modify: `framework/sandstone_tests.cpp:83-99`（ctor 过滤）
- Modify: `framework/sandstone_tests.cpp`（`--list-tests` 走同一集合——已天然如此，因为 list 来自 test_set）

**Interfaces:**
- Consumes: `arm64_spec_get()`（T1）、`test::arm64_requires`（T2）
- Produces: 默认集合已剔除本机不支持的扩展/档位用例。

- [ ] **Step 1: 写失败的验证脚本** — 先确认现状（SVE2 用例目前在默认集合里）：

```bash
./builddir/sdcshield --list-tests | grep -c sve512_fmmla_arm   # 现状 1
```

- [ ] **Step 2: 实现** — ctor 的 `for (struct test &test : test_source())` 循环内、quality 判断之后加：

```cpp
#if defined(__aarch64__) && defined(__linux__)
        /* Design §4.3: default to what THIS chip can run. Explicit requests
         * (-e / --test-list-file) never reach this ctor branch
         * (flags & enable_all_tests is clear), so they stay untouched. */
        {
            const struct arm64_spec *s = arm64_spec_get();
            uint8_t need = test.arm64_requires;
            if ((need & ARM64_REQUIRES_SVE)  && !s->has_sve)  continue;
            if ((need & ARM64_REQUIRES_SVE2) && !s->has_sve2) continue;
            if ((need & ARM64_REQUIRES_SME)  && !s->has_sme)  continue;
            if ((need & ARM64_REQUIRES_SME2) && !s->has_sme2) continue;
            if (test.arm64_min_vl_bits && s->max_sve_vl_bits
                    && s->max_sve_vl_bits < test.arm64_min_vl_bits)
                continue;
        }
#endif
```

需 `#include <device/cpu/arm64/arm64_vl.h>`（在该文件的 aarch64 段内）。

- [ ] **Step 3: 验证**

```bash
./scripts/build.sh
./builddir/sdcshield --list-tests | grep -c sve512_fmmla_arm          # 期望 0（SVE2 被过滤）
./builddir/sdcshield -e sve512_fmmla_arm -t 500 -n 1 | tail -2        # 期望 exit: skip（显式点名旁路 + 运行时诚实 SKIP）
./builddir/sdcshield --list-tests | wc -l                             # 记录前后差值
./builddir/sdcshield -e zstd19 -t 2000 -n 1 | tail -1                 # exit: pass
```

- [ ] **Step 4:** 提交 `feat(framework): 选择期按芯片 ARM64 spec 过滤默认用例集（-e/list-file 旁路）`。

> **注意**：此刻还没有任何用例挂标签（Task 5 才挂），所以本步的 `sve512_fmmla_arm` 仍是 0 过滤——**在 Task 5 之前这个断言会失败**。执行顺序上把 Task 3 的 Step 3 标记推迟到 Task 5 之后复核；Task 3 自身的提交仍成立（机制就位）。

---

### Task 4: worker 线程按最高档拉起（U4）

**Files:**
- Create: `framework/device/cpu/arm64/arm64_spec_prepare.h`
- Modify: `framework/sandstone_run.cpp:780-825`（worker 内 `test_start()` 之后）

**Interfaces:**
- Consumes: `arm64_spec_get()`、`test::arm64_requires`、`arm64_sve_pin_vl()`、`arm64_sme_pin_vl()`、`arm64_sve_vl_bytes()`、`arm64_sme_svl_bytes()`
- Produces: `int arm64_spec_prepare_test(const struct test *test)` — 返回 `EXIT_SUCCESS` / `EXIT_SKIP` / `EXIT_FAILURE`。

- [ ] **Step 1:** 新建头文件：

```c
#ifndef ARM64_SPEC_PREPARE_H
#define ARM64_SPEC_PREPARE_H

#include <sandstone.h>

#if defined(__aarch64__) && defined(__linux__)
#include <device/cpu/arm64/arm64_vl.h>

/* Design §4.4: run on THIS worker thread, immediately before test_run.
 * The framework pins every SVE/SVE2 test to the host's highest supported VL
 * so "highest spec" is an explicit, verifiable fact rather than an assumption
 * about the kernel default. Environment refusal is a SKIP (not our defect);
 * a readback mismatch after a successful pin is a FAIL (invariant broken). */
static inline int arm64_spec_prepare_test(const struct test *test)
{
    const struct arm64_spec *s = arm64_spec_get();
    uint8_t need = test->arm64_requires;

    if (need & (ARM64_REQUIRES_SVE | ARM64_REQUIRES_SVE2)) {
        long want = s->max_sve_vl_bits;
        if (want <= 0)
            return EXIT_SUCCESS;              /* nothing to pin; test gates itself */
        long rc = arm64_sve_pin_vl(want / 8);
        if (rc < 0) {
            log_skip(CpuNotSupportedSkipCategory,
                     "%s: cannot pin this worker to the %ld-bit SVE tier "
                     "(prctl rc=%ld)", test->id, want, rc);
            return EXIT_SKIP;
        }
        long got = arm64_sve_vl_bytes() * 8;
        if (got != want) {
            log_error("%s: pinned %ld-bit but read back %ld-bit",
                      test->id, want, got);
            report_fail_msg("%s: SVE VL readback mismatch after pin", test->id);
            return EXIT_FAILURE;
        }
        (void)arm64_spec_describe;            /* keep the symbol referenced */
    }
    if (need & (ARM64_REQUIRES_SME | ARM64_REQUIRES_SME2)) {
        long want = s->max_sme_svl_bits;
        if (want <= 0)
            return EXIT_SUCCESS;
        long rc = arm64_sme_pin_vl(want / 8);
        if (rc < 0) {
            log_skip(CpuNotSupportedSkipCategory,
                     "%s: cannot pin this worker to the %ld-bit SME SVL "
                     "(prctl rc=%ld)", test->id, want, rc);
            return EXIT_SKIP;
        }
        long got = arm64_sme_svl_bytes() * 8;
        if (got != want) {
            log_error("%s: pinned %ld-bit SVL but read back %ld-bit",
                      test->id, want, got);
            report_fail_msg("%s: SME SVL readback mismatch after pin", test->id);
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}
#else
static inline int arm64_spec_prepare_test(const struct test *test)
{
    (void)test;
    return EXIT_SUCCESS;   /* x86-64 and other arches: no behaviour change */
}
#endif

#endif
```

- [ ] **Step 2:** `framework/sandstone_run.cpp` 在 worker 的

```cpp
        test_start();
        ret = test_run_wrapper_function(current_test(), thread_number);
```

改为

```cpp
        test_start();
        ret = arm64_spec_prepare_test(current_test());
        if (ret == EXIT_SUCCESS)
            ret = test_run_wrapper_function(current_test(), thread_number);
```

并在文件顶部 include `#include <device/cpu/arm64/arm64_spec_prepare.h>`（放在现有
`device/cpu/...` 包含区）。

- [ ] **Step 3: 验证** — 需要一个挂了 `arm64_requires=SVE` 的用例（Task 5 才挂），
  临时用 `-O` 不行；因此本步先用一个临时探针：在 `selftest.cpp` 里加
  `selftest_arm64_spec_worker_pin`，其 `.arm64_requires = ARM64_REQUIRES_SVE`，
  run 内断言 `arm64_sve_vl_bytes()*8 == arm64_spec_get()->max_sve_vl_bits`。
  跑 `-e selftest_arm64_spec_worker_pin --quality=-1 -n 1` → pass；
  再跑 `--dump-cpu-info` 与本机实测 256 对齐。

- [ ] **Step 4:** 提交 `feat(framework): worker 线程在 test_run 前按芯片最高档拉起并读回校验`。

---

### Task 5: 给向量用例挂标签（U5）

**Files:**
- Modify: 向量用例的 `DECLARE_TEST` 块（按家族，非逐文件手改）
- Modify: `scripts/gha/` 或新增 `scripts/check-arm64-requires.py`（一致性检查）

**Interfaces:**
- Consumes: `ARM64_REQUIRES_*`（T1）、字段（T2）

- [ ] **Step 1:** 用脚本批量注入（按家族正则），每个 `DECLARE_TEST(<name>, ...)` 块在其
  `.groups =` 行之前插入 `.arm64_requires = <BITS>,`：

| 家族（文件路径正则） | `arm64_requires` |
|---|---|
| `tests/cpu/sve/**`（除 `ipsec/ipsec_*gcm*/cmac/xcbc*` 这 7 个无 SVE 数据通路的） | `ARM64_REQUIRES_SVE` |
| `tests/cpu/sve/sve512_fmmla_arm.cpp`、`tests/cpu/arm64/sve512_fmmla_arm.cpp`、`sve2_cross_precision_arm.cpp` | `ARM64_REQUIRES_SVE2` |
| `tests/cpu/arm64/sme_*.cpp` | `ARM64_REQUIRES_SME` |
| `tests/cpu/eigen_gemm/gemm_double_max_arm.cpp`、`tests/cpu/eigen_svd/svd_cdouble_sve.cpp` | `ARM64_REQUIRES_SVE` |

注入脚本（一次性，运行后删除）：

```python
import re, pathlib
MARK = ".arm64_requires = "
def patch(p, bits):
    s = p.read_text(encoding='utf-8')
    out, changed = [], 0
    for line in s.splitlines(keepends=True):
        out.append(line)
        if line.lstrip().startswith('.groups =') and MARK not in s:
            pass
    # 更稳的做法：按 DECLARE_TEST 块处理（见 Step 2 的检查器）
```

> 实际执行以 Step 2 的检查器驱动：检查器列出"未挂标签的向量用例"，人/agent 逐个补。
> **不写盲目的正则批量替换**——`DECLARE_TEST` 块结构不统一，误注入会把非向量用例标错。

- [ ] **Step 2:** 新增检查器 `scripts/check-arm64-requires.py`：

```python
#!/usr/bin/env python3
"""Fail if a test whose source contains SVE/SME intrinsics lacks arm64_requires."""
import re, subprocess, sys, pathlib
SUPPORT = ('tests/cpu/sve/', 'tests/cpu/arm64/')
INTR = re.compile(r'\b(sv[a-z0-9_]+_|smstart|smstop|fmopa|rdsvl|svcnt[bdhw])')
bad = []
for p in pathlib.Path('tests').rglob('*.cpp'):
    s = str(p)
    if not s.startswith(SUPPORT):
        continue
    src = p.read_text(encoding='utf-8', errors='replace')
    if not INTR.search(src):
        continue
    if '.arm64_requires' not in src:
        bad.append(s)
print('\n'.join(bad))
sys.exit(1 if bad else 0)
```

- [ ] **Step 3:** `python3 scripts/check-arm64-requires.py` → 输出为空、rc=0。
- [ ] **Step 4:** 复核 Task 3 的断言：`./builddir/sdcshield --list-tests | grep -c sve512_fmmla_arm` → **0**；
  `-e sve512_fmmla_arm` → `exit: skip`。
- [ ] **Step 5:** 提交 `feat(tests): 向量用例挂 arm64_requires 标签 + 一致性检查器`。

---

### Task 6: T2 — xlswave 几何寄存器化

**Files:**
- Modify: `tests/cpu/arm64/sve512_xlswave_ctx_arm.cpp`（`B_TILE` 常量与 Phase A 偏移）

**Interfaces:**
- Consumes: 无（独立单元）
- Produces: 该用例在 128/256/512/1024 四档几何自洽。

- [ ] **Step 1: 先量出现状失败** — `./builddir/sdcshield -e sve512_xlswave_ctx_arm -t 1000 -n 1`
  （本机 256 档）记录结果。若 pass，则用 `/tmp/vlrun 16` 在 128 档再跑一次，记录。
  **必须先用真实输出说明"当前几何在哪一档不自洽"**，否则不动代码。
- [ ] **Step 2:** 照 `sve512_stencil_axis_arm.cpp` 的范式改：

```cpp
    const size_t bt = 2 * (size_t)svcntd();   /* tile rows: runtime VL */
    /* result[] sized [bt]；Phase A 的 res0/res1 偏移改为 bt/2 推导，
       不再出现 `result + 8` 这类 512-bit 专有常量 */
```

并把 `constexpr size_t B_TILE = 16;` 删除或改为运行期量；Phase A 的
`axis_generic_dual(..., bt, res0, res1)` 调用与冻结 golden 的推导同步使用 `bt`。

- [ ] **Step 3: 验证** — 128 与 256 两档 `-n 1` 均 `exit: pass`；
  另跑 `-n 8` 确认无并发踩踏；回归 `zstd19`。
- [ ] **Step 4:** 提交 `fix(sve512): xlswave Phase A 几何改运行期 bt——四档自洽`。

---

### Task 7: T3 — Eigen 四档变体 + 函数指针分派

**Files:**
- Create: `tests/cpu/eigen_gemm/gemm_double_max_arm_body.cpp`（从现有 `.cpp` 抽出 run 主体）
- Create: `tests/cpu/eigen_svd/svd_cdouble_sve_body.cpp`
- Modify: `tests/cpu/eigen_gemm/gemm_double_max_arm.cpp`（分派 + init/cleanup + DECLARE_TEST）
- Modify: `tests/cpu/eigen_svd/svd_cdouble_sve.cpp`
- Modify: `tests/cpu/meson.build:1026-1065`（四个变体库）

**Interfaces:**
- Consumes: `arm64_sve_vl_bytes()`、`arm64_sve_pin_vl()`
- Produces: `namespace GEMM_TEST_NAMESPACE { int run_body(struct test *, int); }`（四个变体）

- [ ] **Step 1:** 把现有 `eigen_gemm_double_max_arm_run` 的 **test_run 主体**（pin 之后的部分，
  从 `const int dim = g_dim;` 到 `return EXIT_SUCCESS;`）移入新文件
  `gemm_double_max_arm_body.cpp`，包进命名空间：

```cpp
// gemm_double_max_arm_body.cpp — compiled four times, once per VL tier.
#include <Eigen/Dense>          // namespace renamed by -DEigen=EigenSVE@N@
#include <sandstone.h>
#include <device/cpu/arm64/arm64_vl.h>

namespace GEMM_TEST_NAMESPACE {    // injected by -D; e.g. gemm_vl256
using Mat = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic>;

int run_body(struct test *test, int cpu)
{
    /* ... 原 test_run 主体（Eigen 对象全部是 run 内局部量，无需跨 TU 传递） ... */
}
}  // namespace GEMM_TEST_NAMESPACE
```

- [ ] **Step 2:** 分派文件 `gemm_double_max_arm.cpp` 保留 init/cleanup/DECLARE_TEST，新增：

```cpp
namespace gemm_vl128  { int run_body(struct test *, int); }
namespace gemm_vl256  { int run_body(struct test *, int); }
namespace gemm_vl512  { int run_body(struct test *, int); }
namespace gemm_vl1024 { int run_body(struct test *, int); }

static int eigen_gemm_double_max_arm_run(struct test *test, int cpu)
{
    const long want = arm64_sve_vl_bytes() * 8;   /* framework already pinned */
    switch (want) {
    case 128:  return gemm_vl128::run_body(test, cpu);
    case 256:  return gemm_vl256::run_body(test, cpu);
    case 512:  return gemm_vl512::run_body(test, cpu);
    case 1024: return gemm_vl1024::run_body(test, cpu);
    default:
        log_skip(CpuNotSupportedSkipCategory,
                 "%s: no Eigen SVE variant compiled for the %ld-bit tier",
                 test->id, want);
        return EXIT_SKIP;
    }
}
```

每个 `run_body` 内部**保留**原有的 `arm64_sve_pin_vl(N/8)` + `.inst` 读回校验（幂等二次确认）。

- [ ] **Step 3:** meson —— 把 `tests_set_sve` 的单库改为四库：

```meson
foreach n : [128, 256, 512, 1024]
    _args = [
        '-march=armv8.2-a+sve',
        '-msve-vector-bits=@0@'.format(n),
        '-DEIGEN_ARM64_SVE_VL=@0@'.format(n),
        '-DEigen=EigenSVE@0@'.format(n),
        '-DGEMM_TEST_NAMESPACE=gemm_vl@0@'.format(n),
        '-DSVD_TEST_NAMESPACE=svd_vl@0@'.format(n),
    ]
    tests_sve_a = static_library('tests_sve_vl@0@'.format(n),
        sources: [...body files...],
        cpp_args: [tests_common_cpp_args, _args, '-DEIGEN_ARM64_USE_SVE'],
        ...)
    sandstone_tests += [tests_sve_a]
endforeach
```

`gemm_double_max_arm.cpp` / `svd_cdouble_sve.cpp`（分派文件）编入**不含**
`-msve-vector-bits` 的普通目标（`tests_set_base` 或新增 `tests_set_sve_dispatch`），
否则分派 TU 的 `arm64_sve_vl_bytes()` 也会被常量折叠。

- [ ] **Step 4: 验证**

```bash
./scripts/build.sh
./builddir/sdcshield -e eigen_gemm_double_max_arm -t 2000 -n 1   # 本机 256 → 走 256 变体，expect pass
/tmp/vlrun 16 ./builddir/sdcshield -e eigen_gemm_double_max_arm -t 2000 -n 1  # 128 变体
# 日志需显示选中的变体（在 run_body 首行加一行 log_warning 或在 dump 中体现）
./builddir/sdcshield -e eigen_svd_cdouble_sve -t 2000 -n 1
./builddir/sdcshield -e zstd19 -t 2000 -n 1                      # exit: pass
```

- [ ] **Step 5:** 提交 `feat(eigen): 四档 SVE 变体 + 运行时函数指针分派（128/256/512/1024）`。

---

### Task 8: gem5 验证物与四档矩阵（U8）

**Files:**
- Create: `/home/sdc/wangxu/gem5-fi/workloads/sdcshield/eigen_gemm_vl{512,1024}_dual.cpp`
- Create: `/home/sdc/wangxu/gem5-fi/workloads/sdcshield/eigen_svd_vl{512,1024}_dual.cpp`
- Create: `/home/sdc/wangxu/gem5-fi/workloads/sdcshield/sve2_fmmla_dual.c`
- Modify: `/home/sdc/wangxu/gem5-fi/workloads/sdcshield/Makefile`
- Create: `scripts/run/sve-vl-matrix.sh`（本仓，驱动四档矩阵）

**Interfaces:**
- Consumes: vendored Eigen（本仓 `third-party/eigen5`）、gem5 `arm_chaos.py`
- Produces: 四档矩阵的真实输出（tier / checksum / `SDCSHIELD_FAILS=0`）

- [ ] **Step 1:** Eigen 逐档内核（模板）：

```cpp
// eigen_gemm_vl512_dual.cpp — built with -msve-vector-bits=512
#include <Eigen/Dense>
#include <cstdio>
int main() {
    constexpr int N = 128;
    Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic> A(N, N), B(N, N), C;
    /* 确定性填充（整数可精确表示）→ C = A*B → 与标量 golden 逐位比对 */
    /* 输出 SDCSHIELD_FAILS=<n> 与 16 位 FNV-1a checksum */
}
```

编译：`gcc -static -O2 -march=armv8.2-a+sve -msve-vector-bits=512 -DEIGEN_ARM64_SVE_VL=512 -DEigen=EigenSVE512 -I<vendored eigen5>`。

- [ ] **Step 2:** SVE2 fmmla 内核：用 `svmmla_f32`/`svmmla_f64` + 标量 golden，静态编译 `-march=armv9-a+sve2+f32mm+f64mm`。

- [ ] **Step 3:** 跑矩阵（真实输出必须引用）：

```bash
cd /home/sdc/wangxu/gem5-fi
for k in eigen_gemm_vl512_dual eigen_gemm_vl1024_dual eigen_svd_vl512_dual \
         eigen_svd_vl1024_dual sve2_fmmla_dual; do
  for vl in 4 8; do
    build/ARM/gem5.opt --quiet --outdir=/tmp/g5/$k-$vl configs/se/arm_chaos.py \
      --cpu Atomic --max_faults 0 --cmd workloads/sdcshield/$k --sve_vl $vl \
      | grep -E "SDCSHIELD_FAILS|^[0-9a-f]{16}$"
  done
done
```

- [ ] **Step 4:** 本机 128/256 两档 + gem5 512/1024 两档，产出四档矩阵表（含每档 pass/skip 与原因）。
- [ ] **Step 5:** 提交（gem5-fi 仓与本仓各自一 commit）。

---

### Task 9: 文档同步（U9）

**Files:**
- Modify: `README.md`（VL 政策段）
- Modify: `CLAUDE.md`（平台怪癖节：新增 spec 策略条目）
- Modify: `docs/coding/writing_tests.md`（如何声明 `arm64_requires`）
- Modify: `docs/architecture.md`（选择期过滤 + worker pin 的数据流）

- [ ] **Step 1:** 四处同步，确保与代码 100% 一致（规则 7）。
- [ ] **Step 2:** 把 spec §9 的诚实边界逐条落进 README 与 CLAUDE.md。
- [ ] **Step 3:** 提交 `docs: ARM64 spec 策略与 VL 分档用例 文档同步`。

---

## Self-Review

**Spec 覆盖**：§4.1→T1、§4.2→T2、§4.3→T3、§4.4→T4、§4.5→T5、§4.6→T6、§4.7→T7、§4.8→T8、§4.9→T1+T4 的日志、§5 失败模式→T4 的实现；§6 非目标在 Global Constraints 与 T8 中显式排除 2048/SME2/gem5 二进制。

**类型一致性**：`arm64_requires`（`uint8_t`）在 T2 定义、T3/T4/T5 使用；`arm64_min_vl_bits`（`long`）在 T2 定义、T3 使用；`ARM64_REQUIRES_*` 在 T1 定义、T5 使用；`arm64_spec_prepare_test` 在 T4 定义并使用。`GEMM_TEST_NAMESPACE`/`SVD_TEST_NAMESPACE` 在 T7 的 meson 与源码一致。

**已知顺序依赖**：Task 3 的 Step 3 断言依赖 Task 5 的标签；已在 Task 3 中显式标注，Task 5 Step 4 复核。

---

## 评审修复（2026-10-27，as-built）

首轮实现合入前的独立检视发现并修复以下缺陷（逐项独立 commit）：

| # | 缺陷 | 修复 | 证据/边界 |
|---|---|---|---|
| A | 探测在调用线程上 save/restore：`PR_SVE_SET_VL` 返回可能带 `PR_SVE_VL_INHERIT` 位，被当成长度 → 虚假 ~1M-bit 最大值；restore 掩掉 INHERIT/ONEXEC；override 接受硬件不实现的中间档（如 384）→ worker 读回不符而 FAIL；缓存 `bool done` 无同步 | 改**临时线程**探测（设计 §4.1 原意），只取 `PR_*_VL_LEN_MASK` 长度；override 重新探测并按内核实际授予档归一；`__atomic` 状态机发布 | 内核源码 4.19/v5.10/master `sve_set_current_vl` 成功后返回 `sve_prctl_status()`（VL+INHERIT 位），不是 0——确认既有 pin 语义正确；本机无 SVE，线程探测路径不可达，诚实声明未在本机执行 |
| B | 只 pin worker：内核默认 VL < 芯片最高档时（§4.1 的存在前提），init 按默认档冻结 golden/缓冲区、worker 按最高档运行 → 全部漂移守卫 FAIL，无守卫用例可能越界 | `arm64_spec_pin_control_thread()` 在 `test_init` 前 pin 控制线程（init aux 与 worker 都继承其 VL）；worker 仍 pin+读回作权威判定；新增 `selftest_arm64_spec_init_pin` 断言 init 与 worker 档位一致 | 本机无 SVE → SKIP；CI N2（VL=128，默认=最大）可跑通但**不能**覆盖 default<max 场景；该场景需真 SVE 主机降 sysctl 默认档验证，列为验证边界 |
| C | `sve512_fmmla_arm` 从未在硬件执行过，golden 存在三重缺陷（第二操作数转置被漏掉；整向量当 n×n 而非每 4-lane 一组 2×2；FMMLA 是融合 2-积、顺序 `std::fma` 链差 1 ULP），且门控错误要求 SVE2、缺 VL 下界（f64 在 VL=128 SIGILL） | 由 09a0efdb 以真机基向量探测修正：每 4-lane 组 `out[g+2r+c]=Σ_k Zn[g+2r+k]·Zm[g+2c+k]`；改用精确整数操作数使 golden 与舍入/顺序无关；拆出 `tests_arm64_sve_mm`（`+f32mm+f64mm`，不要求 SVE2）；门控 256..1024 + `arm64_min_vl_bits=256` | 真机 256 档 `result: pass`（该用例首次通过）、128 档干净 SKIP；gem5 `--sve_vl 4/8` `SDCSHIELD_FAILS=0`。注：本轮初版仅把 Zm 改为列主序（n×n 口径），复算虽匹配 PR 记录的 `[-1,-4,3,-6]`，但被 09a0efdb 证明仅在 lanes==4 巧合成立，已在该提交合入时丢弃。本机 GCC 10.3 不支持 `+f32mm+f64mm`，该 TU 不在本机构建 |
| D | `sleef_sve`（tests/cpu/sleef，真 VLA）漏标 `arm64_requires`；检查器目录表也不含该目录；脚本 `tuple[str,int]` 在 Python 3.8（openEuler 20.03）直接 TypeError | 补标签；`SCAN_DIRS` 增 `tests/cpu/sleef`；加 `from __future__ import annotations` | `python3 scripts/check-arm64-requires.py` → ok（3.8.18 实跑）；默认集不再含 `sleef_sve`，显式 `-e` 仍注册并诚实 SKIP |
| E | `writing_tests.md` / `progress-log.md` 在 main 上遗留已提交的合并冲突标记；PR 新章节又插进句子中间 | 移除冲突标记、修复章节顺序 | `grep` 计数 0；纯文档 |
| F | 选择期最小档过滤对 SME-only 用例误用 SVE 最大值 | 按扩展选择 SVE/SME 最大值 | 当前无用例设置 `arm64_min_vl_bits`，为字段语义正确性 |

**本机验证（0202 cathelper-k8s-worker2，Kunpeng-920，无 SVE/SVE2/SME）**：

```
./scripts/build.sh                                  → 0 error / 0 new warning
./builddir/sdcshield --dump-cpu-info                → ARM64 spec: SVE no | SVE2 no | SME no | SME2 no
./builddir/sdcshield --list-tests | wc -l            → 296（spec 过滤后；main 上 478 已随本策略变为 host-dependent）
-e sve512_f64_chain_svd / eigen_* / sleef_sve        → skip 'test compiled with sve'，exit: pass
--selftests selftest_arm64_spec                      → pass
selftest_arm64_spec_worker_pin / _init_pin           → skip（no SVE）
zstd19 -t 2000 -n 1                                  → exit: pass
python3 scripts/check-arm64-requires.py              → ok
```

**未闭环边界（如实）**：本机（0202）无任何向量扩展；gem5-fi SE 的 prctl 无 handler（框架二进制
不可运行），因此临时线程探测、控制线程继承、worker pin/读回、default<max 一致性、Eigen 512/1024
变体均未在本机/本机 gem5 执行。FMMLA 已由 09a0efdb 在真机（F32MM/F64MM，无 SVE2，256 档）与
gem5（vl4/vl8）验证，但其修正本身不在本机执行。CI 的 N2 runner（VL=128）能覆盖 pin 成功路径与
init/worker 一致性（默认=最大时），覆盖不到 default<max；需在可 pin 的真 SVE 主机上把
`/proc/sys/abi/sve_default_vector_length` 调低后复测。SVE2 `sve2_cross_precision_arm` 仍无执行证据。
