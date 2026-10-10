# missing_testcases_20261009 → SVE 移植实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `missing_testcases_20261009/` 转移包中的 44 个测试（38 有效 + 6 在文件内的 0-fail 对照；eigen 1 个已有 SVE 口不移植）全部移植为 SVE 版本放入 `tests/cpu/sve/`，完成编译、可靠性测试（!122 全核两遍 60s 零 fail）与有效性冒烟测试（objdump SVE 指令证据 + 故障注入检出证明）。

**Architecture:** 三个既有移植范式按家族对应复用：批处理范式（svwhilelt 批 + 相位谓词旋转 ALU，模板 `mrn_rmw_sve.cpp`/`mrn_nuke_sve.cpp`/`movbe_sve.cpp`）、槽范式（SLOTS×lanes 定容 + asm `ldr %0,[%1]` z 寄存器满 VL 加载 + init/run VL 失配 fail-closed，模板 `neon_rot_ldr_at_top_rowmajor_sve.cpp`/`neon_rot_2src_sve.cpp`）、pg4 子块范式（4×32 位子块谓词保 NEON 语义，模板 `arm0102_kreg_mask_sve.cpp`）。每批一个 commit（与 09-21 batch 1-7 先例一致）。

**Tech Stack:** C++ + arm_sve.h（SVE1, `-march=armv8.2-a+sve`，运行时 VL 无关）、sandstone 框架（TEST_LOOP/memset_random/random32/random64/report_fail_msg/log_error）、meson。

**Spec:** `missing_testcases_20261009/MANIFEST.md`（转移包登记册）+ 本目录 34 个源文件；既有 120 个 SVE 测试为语义/结构参照。

## Global Constraints（所有任务隐含遵守）

1. **x86-64 不触碰**：只新增 `tests/cpu/sve/**` 文件 + 修改 `tests/cpu/sve/meson.build`（aarch64 guard 内）。不改任何 x86 路径。
2. **命名**：原名 + `_sve` 后缀；已核对与既有 144 个 SVE 测试名零冲突（406 基线 `--list-tests` 复核）。
3. **每文件头注释**：`@test @b <name>` + parblock 描述（中文/英文与家族先例一致），标注 "port of <原测试名>（missing_testcases_20261009 transfer, tests/cpu/<原路径>）" 溯源。
4. **SVE 探测**：每个 test_init 首先查 `getauxval(AT_HWCAP) & HWCAP_SVE`，无 SVE → `log_skip(CpuNotSupportedSkipCategory, "to be implemented (placeholder): ARM SVE required for <name>")` + `return EXIT_SKIP`。非 aarch64 提供 `#else` stub（同上 skip）。完全照抄模板文件的写法。
5. **指令事实（已实测 GCC 12.3）**：`svorn_*` 和 `svbsl_*` **不存在**于 GCC 12 SVE1 头。ORN 用 `svorr_x(pg, a, svnot_x(pg, b))`；BSL 用 `svorr_x(pg, svand_x(pg,a,b), svand_x(pg, svnot_x(pg,a), c))`。其余（svnot/svmla_f32/svmsb_f32/svneg_f32/svbic/svindex/svmul_s32/svsub/svsel/svcmpne_n/svptest_any…）均已编译验证存在。
6. **VL 安全两条铁律**：
   - 槽范式/pg4 范式：init 捕获 `lanes=svcntd()/svcntw()` 存入 data，run 开头 `if ((int)svcntd() != data->lanes) { log_error(...); report_fail_msg(...); }`（fail-closed，照抄 rowmajor_sve:120-124）。
   - pg4 范式尾部 asm 全宽读越界：缓冲分配 `(COUNT + 8) * sizeof(uint32)`（+8 元素 = VL=256 时 32B 松弛），注释说明（arm0102_kreg_mask_sve 原版最后子块 asm ldr z 会过读 16B，本批补松弛——偏差记录在案）。
   - 批处理范式幂次安全性：所有 COUNT 取 2 的幂（1024/8192/16384/131072 均可被任意 2 幂 lanes 整除），无尾部越界。
7. **竞态纪律**（insert_extract_sve 1023c841 教训）：共享 `test->data` 缓冲只读或同值写（原文件 2/3 的 tempA 同值写、movbe 的 swapped 同值写——保留原设计并注释安全性论证）；temp/dst/junk 等可变缓冲一律 run 内 per-thread 分配。
8. **RNG**：`memset_random`/`random32`/`random64` init 期一次性填充，不固定种子；GPS-tag/pattern 类（gps/k5far/k3res/k7p 全系）为确定性填充、**不用 RNG**（照抄原版）。
9. **热路径保真**：asm volatile + "memory" clobber 防消除/防重排；"reload 刚存储地址"一律 asm `ldr %0,[%1]`（`=w` 约束）或已被先例验证的 svld1（objdump 冒烟兜底核查）；N-nop 延迟必须与 ldr 融合在同一 asm 块（`.rept N mov x9,x9 .endr ldr`）。
10. **验证协议（每任务每测试）**：
    - `ninja -C builddir` 零新增 error/warning（既有良性警告白名单见 CLAUDE.md）；
    - `./builddir/sdcshield -e <test> -t 60000 --cpuset='!122' 2>&1 | tail -2` → `exit: pass`（P0 先验证 cpuset 排除语法；122 现已重新在线，两阶段协议阶段 1 = 排除 122）；
    - 回归：`./builddir/sdcshield -e zstd19 -t 3000 -n 1` → `exit: pass`；
    - commit + push（分支 feat/sve-port-avx53，非 main）。
11. **禁止**：不主动跑含 122 的全核压测（10-09 panic #3 先例，机器现 122 在线、重启后未封顶）——如需阶段 2 观察须用户裁决。**不 kill/重启任何进程**。
12. meson 改动后必须 `PKG_CONFIG_PATH=./third-party/eigen5 meson setup --reconfigure builddir && ninja -C builddir`（CLAUDE.md 规则）。
13. 诚实纪律：所有验证结论引用真实命令输出；未跑过的不写"通过"。

## 源→目标映射总表（44 测试）

| 任务 | 原文件（missing_testcases_20261009/tests/cpu/…） | 新文件（tests/cpu/sve/…） | 测试名 |
|---|---|---|---|
| T1 | misc/movbe_l1d_vs_l2.cpp | core179/movbe_l1d_vs_l2_sve.cpp | movbe_l1d_sve, movbe_l2_sve |
| T1 | misc/movbe_dump_golden.cpp | core179/movbe_dump_golden_sve.cpp | movbe_dump_golden_sve |
| T2 | misc/mrn_rmw_fwd_vs_l1d.cpp | core179/mrn_rmw_fwd_vs_l1d_sve.cpp | mrn_rmw_fwd_sve, mrn_rmw_l1d_sve |
| T2 | misc/mrn_rmw_fwd_vs_diff.cpp | core179/mrn_rmw_fwd_vs_diff_sve.cpp | mrn_rmw_diff_sve |
| T2 | misc/mrn_rmw_diff_footprint.cpp | core179/mrn_rmw_diff_footprint_sve.cpp | mrn_rmw_diff_64k_sve, mrn_rmw_diff_1m_sve |
| T2 | misc/mrn_rmw_timing_probe.cpp | core179/mrn_rmw_timing_probe_sve.cpp | mrn_rmw_nop_sve, mrn_rmw_nop1/4/8/16/64_sve |
| T3 | misc/mrn_nuke_2src_alu.cpp | core179/mrn_nuke_2src_alu_sve.cpp | mrn_nuke_2src_alu_sve |
| T3 | misc/mrn_nuke_3src_alu.cpp | core179/mrn_nuke_3src_alu_sve.cpp | mrn_nuke_3src_alu_sve |
| T3 | misc/mrn_nuke_4src_alu.cpp | core179/mrn_nuke_4src_alu_sve.cpp | mrn_nuke_4src_alu_sve |
| T3 | misc/mrn_nuke_dense.cpp | core179/mrn_nuke_dense_sve.cpp | mrn_nuke_dense_sve |
| T3 | misc/mrn_pairs_2src_alu.cpp | core179/mrn_pairs_2src_alu_sve.cpp | mrn_pairs_2src_alu_sve |
| T3 | misc/mrn_reloaded_2src_alu.cpp | core179/mrn_reloaded_2src_alu_sve.cpp | mrn_reloaded_2src_alu_sve |
| T4 | misc/agu_stress_store_only.cpp | mem/agu_stress_store_only_sve.cpp | agu_stress_store_only_sve |
| T4 | misc/neon_rot_store_only.cpp | core179/neon_rot_store_only_sve.cpp | neon_rot_store_only_sve |
| T4 | misc/neon_rot_u8x16.cpp | core179/neon_rot_u8x16_sve.cpp | neon_rot_u8x16_sve |
| T4 | memory/mem_disambig_alu.cpp | mem/mem_disambig_alu_sve.cpp | mem_disambig_alu_sve |
| T5 | misc/neon_rot_ldr_at_top_sub.cpp | core179/neon_rot_ldr_at_top_sub_sve.cpp | neon_rot_ldr_at_top_sub_sve |
| T5 | misc/neon_rot_ldr_at_top_mla.cpp | core179/neon_rot_ldr_at_top_mla_sve.cpp | neon_rot_ldr_at_top_mla_sve |
| T5 | misc/neon_rot_ldr_at_top_offdiag.cpp | core179/neon_rot_ldr_at_top_offdiag_sve.cpp | neon_rot_ldr_at_top_offdiag_sve |
| T5 | misc/neon_rot_ldr_at_top_str3.cpp | core179/neon_rot_ldr_at_top_str3_sve.cpp | neon_rot_ldr_at_top_str3_sve |
| T5 | misc/neon_rot_ldr_at_top_ldr4.cpp | core179/neon_rot_ldr_at_top_ldr4_sve.cpp | neon_rot_ldr_at_top_ldr4_sve |
| T6 | misc/neon_rot_ldr_at_top_rowmajor_dump.cpp | core179/neon_rot_ldr_at_top_rowmajor_dump_sve.cpp | neon_rot_ldr_at_top_rowmajor_dump_sve |
| T6 | misc/neon_rot_ldr_at_top_rowmajor_gps.cpp | core179/neon_rot_ldr_at_top_rowmajor_gps_sve.cpp | neon_rot_ldr_at_top_rowmajor_gps_sve |
| T6 | misc/neon_rot_ldr_at_top_rowmajor_dualcmp.cpp | core179/neon_rot_ldr_at_top_rowmajor_dualcmp_sve.cpp | neon_rot_ldr_at_top_rowmajor_dualcmp_sve |
| T6 | misc/neon_rot_ldr_at_top_rowmajor_str3_checkall.cpp | core179/neon_rot_ldr_at_top_rowmajor_str3_checkall_sve.cpp | neon_rot_ldr_at_top_str3_checkall_sve（名字无 rowmajor，与原版一致） |
| T7 | misc/neon_rot_ldr_at_top_rowmajor_k5far.cpp | core179/neon_rot_ldr_at_top_rowmajor_k5far_sve.cpp | neon_rot_ldr_at_top_rowmajor_k5far_sve |
| T7 | misc/neon_rot_ldr_at_top_rowmajor_k5inter_rand.cpp | core179/neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve.cpp | neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve |
| T7 | misc/neon_rot_ldr_at_top_rowmajor_k3res.cpp | core179/neon_rot_ldr_at_top_rowmajor_k3res_sve.cpp | neon_rot_ldr_at_top_rowmajor_k3res_sve |
| T7 | misc/neon_rot_ldr_at_top_rowmajor_k7pattern.cpp | core179/neon_rot_ldr_at_top_rowmajor_k7pattern_sve.cpp | …k7p_zero/one/rand/hiham_sve（4 测试） |
| T8 | arm-0102/arm0102_kreg_select.cpp | kreg/arm0102_kreg_select_sve.cpp | arm0102_kreg_select_sve |
| T8 | arm-0102/arm0102_kreg_not.cpp | kreg/arm0102_kreg_not_sve.cpp | arm0102_kreg_not_sve |
| T8 | arm-0102/arm0102_kreg_logic.cpp | kreg/arm0102_kreg_logic_sve.cpp | arm0102_kreg_logic_sve |
| T8 | arm-0102/arm0102_fma_f32.cpp | kreg/arm0102_fma_f32_sve.cpp | arm0102_fma_f32_sve |
| — | eigen_svd/svd_cdouble_noavx512.cpp | **不移植**（eigen_svd_cdouble_sve 已是其 SVE 口：同 BDCSVD/golden 机制/迭代结构，`-O eigen_svd_cdouble_sve.mdim=300` 精确复现 300×300 工况；NEON 原版本仓库 `tests/cpu/eigen_svd/svd_cdouble_noavx512.cpp` 已在场且与转移包逐字节一致）——T9 文档记录此裁决 | — |

注：MANIFEST 提到的 `mrn_rmw_diff_nop64` 在转移包源文件中**不存在**（仅 nop64 描述里引用），实际 45 个 DECLARE_TEST 减 eigen 1 个 = 44。

## 公共代码片段（各任务引用，勿重复发明）

**A. SVE 探测 + skip（init 开头，照抄模板）**
```cpp
#if defined(__aarch64__)
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "to be implemented (placeholder): ARM SVE required for <name>");
        return EXIT_SKIP;
    }
#endif
```

**B. asm 保证加载/存储（槽范式 & pg4 范式）**
```cpp
static inline svuint64_t load_vec_sve(const void *addr) {
    svuint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}
static inline void store_vec_sve(void *addr, svuint64_t val) {
    svst1_u64(svptrue_b64(), static_cast<uint64_t *>(addr), val);
}
```

**C. N-nop 延迟加载（T2 timing probe 专用；.rept 必须与 ldr 同一块）**
```cpp
static inline svuint64_t load_vec_delayed_sve(const void *addr) {   /* N=2 */
    svuint64_t res;
    __asm__ volatile(".rept 2\n\tmov x9, x9\n\t.endr\n\tldr %0, [%1]"
                     : "=w"(res) : "r"(addr) : "memory");
    return res;
}
/* 共 6 个变体函数: delayed(2)/delayed1(1)/delayed4(4)/delayed8(8)/delayed16(16)/delayed64(64) */
```

**D. L1D 逐行驱逐（T1 movbe_l2 / T2 mrn_rmw_l1d）**
```cpp
static inline void evict_l1d_line(const void *addr) {
    __asm__ volatile("dc civac, %0\n\tdsb sy\n\t" :: "r"(addr) : "memory");
}
/* 向量批用法: 对覆盖 [buf+base*es, buf+(base+n)*es) 的每条 64B 行各调一次 evict,
 * 一条 dsb 已含于 helper; 原版逐元素驱逐(同行重复), 移植逐行一次 — 语义等价(全部行已冲刷), 注释记录 */
static inline void evict_range(const void *p, size_t bytes) {
    const char *c = (const char *)p;
    uintptr_t start = (uintptr_t)c & ~(uintptr_t)63;
    for (uintptr_t a = start; a < (uintptr_t)c + bytes; a += 64)
        __asm__ volatile("dc civac, %0\n\tdsb sy\n\t" :: "r"(a) : "memory");
}
```

**E. 相位谓词旋转 ALU（批处理范式核心，照抄 mrn_rmw_sve:82-92）**
```cpp
svuint64_t r_add = svadd_u64_x(pg, va, vb), r_sub = svsub_u64_x(pg, va, vb),
           r_eor = sveor_u64_x(pg, va, vb), r_and = svand_u64_x(pg, va, vb);
uint64_t phase[8];
for (int k = 0; k < n; ++k) phase[k] = (base + k) % 4;
svuint64_t vphase = svld1_u64(pgn, phase);
svuint64_t res = svsel_u64(svcmpeq_n_u64(pgn, vphase, 0), r_add,
              svsel_u64(svcmpeq_n_u64(pgn, vphase, 1), r_sub,
              svsel_u64(svcmpeq_n_u64(pgn, vphase, 2), r_eor, r_and)));
```

**F. GPS 标签（T6 gps / T7 k5far/k3res/k7p_rand；确定性，无 RNG）**
```cpp
#define GPS_MAGIC_A 0xA5A5u
#define GPS_MAGIC_B 0x5A5Au
#define GPS_MAGIC_S 0x0F0Fu
static inline uint64_t gps_tag(uint16_t magic, uint16_t byte_off, uint16_t gen) {
    return ((uint64_t)magic << 48) | ((uint64_t)byte_off << 32)
         | ((uint64_t)(uint16_t)~byte_off << 16) | gen;
}
/* 每 u64 字: byte_off = 该字在自身缓冲内的字节偏移; k3res 缓冲 >64KB 时 byte_off &= 0xFFFF
 * (16 位字段回绕, 确定性可离线解码, 注释记录) */
```

**G. 故障记录结构（T6 dump/str3_checkall；元素粒度替代原版 16B 向量对，注释记录格式差异）**
```cpp
#pragma pack(1)
struct fi_record {
    uint64_t ts_us;      /* clock_gettime(CLOCK_MONOTONIC) */
    int32_t  cpu;        /* device_info[cpu].cpu_number */
    uint32_t iter;       /* 0-based sweep */
    uint8_t  sweep_dir;  /* 0=asc 1=desc (rowmajor 系) */
    uint8_t  op;         /* slot%4 */
    uint16_t _pad;
    uint32_t index;      /* u64 元素下标 */
    uint64_t srcA, srcB, exp, act, xor;
};
#pragma pack()
/* 追加写 /var/tmp/<testname>_<pid>.bin; 每次 write() 一条 */
```

---

### Task 0: 构建环境恢复 + 基线固定（无 commit）

**背景**：/tmp/{gmp,zlib,zstd}-install 被重启清除（已知易失依赖）；`ninja -C builddir` 报 `'/tmp/gmp-install/lib/libgmp.a', needed by 'sdcshield', missing`。网络可用（repo.openeuler.org 通，dnf download 已验证 rc=0）。基线：`--list-tests` 406 项、`_sve` 119 项、zstd19 `exit: pass`。

- [x] **Step 0.1: 下载 devel RPM**（实测: gmp-devel-6.3.0-5 / zlib-devel-1.2.13-5 / zstd-devel-1.5.5-4, oe2403sp4）
- [x] **Step 0.2: 解包到 build.ninja 期望的精确路径**（三个 .a + 全部头文件就位）
- [x] **Step 0.3: 基线构建 + 回归**（ninja 16/16 全绿; zstd19 `exit: pass`; `--list-tests` = **406** 基线）
- [x] **Step 0.4: 验证 cpuset 排除语法**（`--cpuset='!122'` → cpu-info 第 4 段 `{starting_cpu: 96, count: 31}` + fullsocket `64→63 threads`，122 确被排除；语法可用）
```bash
mkdir -p /tmp/rpm-dl && cd /tmp/rpm-dl && dnf download gmp-devel zlib-devel libzstd-devel
# 预期: gmp-devel-6.3.0、zlib-devel-1.2.13、libzstd-devel-1.5.5（openEuler 24.03 版本）
```
- [ ] **Step 0.2: 解包到 build.ninja 期望的精确路径（不装系统）**
```bash
for r in /tmp/rpm-dl/*.rpm; do rpm2cpio "$r" | cpio -idmD /tmp/rpm-extract; done
mkdir -p /tmp/gmp-install/lib /tmp/gmp-install/include /tmp/zlib-install/lib /tmp/zlib-install/include /tmp/zstd-install/lib /tmp/zstd-install/include
cp -P /tmp/rpm-extract/usr/lib64/libgmp.a /tmp/gmp-install/lib/
cp -P /tmp/rpm-extract/usr/include/gmp.h /tmp/gmp-install/include/
cp -P /tmp/rpm-extract/usr/lib64/libz.a /tmp/zlib-install/lib/
cp -P /tmp/rpm-extract/usr/include/zlib.h /tmp/rpm-extract/usr/include/zconf.h /tmp/zlib-install/include/
cp -P /tmp/rpm-extract/usr/lib64/libzstd.a /tmp/zstd-install/lib/
cp -P /tmp/rpm-extract/usr/include/zstd.h /tmp/rpm-extract/usr/include/zstd_errors.h /tmp/zstd-install/include/
ls -la /tmp/gmp-install/lib/libgmp.a /tmp/zlib-install/lib/libz.a /tmp/zstd-install/lib/libzstd.a
# 预期: 三个 .a 存在（若头文件名不同, ls /tmp/rpm-extract/usr/include/ 实查后对应拷贝）
```
- [ ] **Step 0.3: 基线构建 + 回归**
```bash
cd /home/sdc/root-xupeng/sdcshield && ninja -C builddir 2>&1 | tail -3
# 预期: no work to do 或全绿（零 error）
./builddir/sdcshield -e zstd19 -t 3000 -n 1 2>&1 | tail -2   # 预期 exit: pass
./builddir/sdcshield --list-tests | wc -l                     # 预期 406（记录为基线）
```
- [ ] **Step 0.4: 验证 cpuset 排除语法（两阶段协议阶段 1 的载体）**
```bash
./builddir/sdcshield -e zstd19 -t 3000 --cpuset='!122' -v 2>&1 | grep -iE 'cpu|thread' | head -3
# 预期: 显示 127 线程/CPU 集（若 '!' 语法不支持, 改用 --cpuset=0-121,123-127 并更新本计划所有命令）
```

---

### Task 1: movbe 三连（3 测试 / 2 文件）

**Files:**
- Create: `tests/cpu/sve/core179/movbe_l1d_vs_l2_sve.cpp`（2 测试）
- Create: `tests/cpu/sve/core179/movbe_dump_golden_sve.cpp`
- Modify: `tests/cpu/sve/meson.build`（sve_avx53_sources 追加，批注释 `# missing_testcases_20261009 ports (batch 1/8): movbe reload-path probes`）

**模板**：`tests/cpu/sve/core179/movbe_sve.cpp`（svtbl [3,2,1,0] bswap 构造、vidx 表、svwhilelt_b8 谓词、堆直存直读）。

**原文件语义**（必读：`missing_testcases_20261009/tests/cpu/misc/movbe_l1d_vs_l2.cpp` + `movbe_dump_golden.cpp`）：

**Interfaces:**
- Produces: 测试 `movbe_l1d_sve`/`movbe_l2_sve`/`movbe_dump_golden_sve`；`movbe_sve_data` 型共享 struct {input[16384 u32], swapped[16384 u32]}（aligned_alloc_safe 64, 64KB each, random32 填充——两文件各自定义，不跨文件共享符号）。

- [ ] **Step 1.1: 写 movbe_l1d_vs_l2_sve.cpp**

结构（两测试共用 init/cleanup/log helper，各配 run）：
- init（共用 `movbe_probe_sve_init`）：SVE 探测 → malloc struct → `aligned_alloc_safe(64, 1<<14 * 4)` ×2 → `input[i]=random32()`。
- `movbe_l1d_sve_run`：批处理范式，`TEST_LOOP(test, 1<<13)`；每批 `lanes=svcntw()` 个 u32：vidx 表构造照抄 movbe_sve:67-75；`svuint8_t in = svld1_u8(pg, (const uint8_t*)(data->input + base))` **直接从堆加载**（这是与 movbe_sve 的差异：movbe_sve 先 memcpy 到栈；本测试被测路径就是 input 的堆 reload，必须堆直读）；`vswapped = svtbl_u8(in, vidx)`；`svst1_u8(pg, (uint8_t*)(data->swapped+base), vswapped)`（堆 store）；`vrestored = svtbl_u8(svld1_u8(pg, (const uint8_t*)(data->input+base)), vidx)`（**堆 reload input = 被测 L1D-hit 路径**）；svst1 到栈 out_bytes；逐元素比较 `restored[i] != input[base+i]` → `log_error("movbe_l1d_sve: round-trip failed at index %u: input 0x%08x restored 0x%08x xor 0x%08x", ...)` + `report_fail_msg(...)`。
- `movbe_l2_sve_run`：同上，但 reload 前对 input 批覆盖的行 `evict_range(data->input + base, n*4)`（片段 D）；log tag 换 movbe_l2_sve。头注释记录：原版逐 4B 元素 dc civac（同行重复驱逐），SVE 版逐行一次——语义等价。
- DECLARE_TEST ×2，desc 分别注明 L1D-hit / L2-reload 探针（对照原版 desc 翻译），`.quality_level = TEST_QUALITY_PROD`。

- [ ] **Step 1.2: 写 movbe_dump_golden_sve.cpp**

- init：同上 + input 填充后把整缓冲写 `O_WRONLY|O_CREAT|O_TRUNC, 0644` 到 **`/var/tmp/movbe_input_golden_sve_<pid>.bin`**（getpid() 后缀；头注释记录两处偏差：原版固定路径 `/tmp/movbe_input_golden.bin`（tmpfs 崩机即失 + 并发互踩），SVE 版按家族 dump 惯例（rowmajor_dump 用 /var/tmp）改 /var/tmp + pid 后缀）。open/write 失败 → `log_error` 后继续（照抄原版容错）。
- run：与 movbe_l1d_sve_run 相同热路径（原版 run 与 movbe_dump 逐字节一致 = 无 evict 的 reload 探针），tag `movbe_dump_golden_sve`。
- cleanup：free + 关 fd（若 init 开成功）。

- [ ] **Step 1.3: 注册 + 构建**

```bash
# meson.build sve_avx53_sources 列表末尾（'arithmetic/acl_gemm_sve.cpp' 之后）追加:
#   'core179/movbe_l1d_vs_l2_sve.cpp',
#   'core179/movbe_dump_golden_sve.cpp',
PKG_CONFIG_PATH=./third-party/eigen5 meson setup --reconfigure builddir 2>&1 | tail -3
ninja -C builddir 2>&1 | grep -E 'error|warning' | grep -vE 'sysv_abi|assume|Wrestrict' ; echo "build rc=$?"
# 预期: 无新增 error/warning 输出, rc=0
```

- [ ] **Step 1.4: 逐测试验证（60s × !122）**
```bash
for t in movbe_l1d_sve movbe_l2_sve movbe_dump_golden_sve; do
  ./builddir/sdcshield -e $t -t 60000 --cpuset='!122' 2>&1 | tail -1
done
# 预期: 三行 exit: pass
./builddir/sdcshield -e zstd19 -t 3000 -n 1 2>&1 | tail -1   # 预期 exit: pass
ls -la /var/tmp/movbe_input_golden_sve_*.bin 2>/dev/null | head -1   # dump_golden 的落盘证据
```

- [ ] **Step 1.5: Commit + push**
```bash
git add tests/cpu/sve/core179/movbe_l1d_vs_l2_sve.cpp tests/cpu/sve/core179/movbe_dump_golden_sve.cpp tests/cpu/sve/meson.build
git commit -m "tests(sve): port movbe reload-path trio from missing_testcases_20261009 — L1D-hit vs L2-reload probe pair + init-golden-dump (3 tests)"
git push origin feat/sve-port-avx53
```

---

### Task 2: mrn_rmw 家族（11 测试 / 4 文件）

**Files:**
- Create: `tests/cpu/sve/core179/mrn_rmw_fwd_vs_l1d_sve.cpp`（fwd + l1d）
- Create: `tests/cpu/sve/core179/mrn_rmw_fwd_vs_diff_sve.cpp`（diff）
- Create: `tests/cpu/sve/core179/mrn_rmw_diff_footprint_sve.cpp`（diff_64k + diff_1m）
- Create: `tests/cpu/sve/core179/mrn_rmw_timing_probe_sve.cpp`（nop ×6）
- Modify: `tests/cpu/sve/meson.build`

**模板**：`tests/cpu/sve/core179/mrn_rmw_sve.cpp`（批处理范式 + 片段 E 相位谓词）。**必读原文件** 4 个（`missing_testcases_20261009/tests/cpu/misc/mrn_rmw_*.cpp`）。

**Interfaces:**
- 旋转 `{+,-,^,&}`（i%4）跨 init golden 与 run 一致（相位 (base+k)%4）。
- 共享只读 srcA/srcB/expected；per-thread temp/dst（fwd/nop 系）或共享同值写 tempA + 只读 tempB（diff 系）。

- [ ] **Step 2.1: mrn_rmw_fwd_vs_l1d_sve.cpp**
  - 共用 init（3×8KB：srcA/srcB/expected，memset_random，golden 标量 i%4）+ 共用 cleanup。
  - `mrn_rmw_fwd_sve_run`：**逐字复用 mrn_rmw_sve_run 主体**（片段 E + `svst1_u64(pg, temp+base, res); svst1_u64(pg, dst+base, svld1_u64(pg, temp+base));`）+ 每 sweep `memcmp(dst, expected, 8192)`，失配时冷路径标量扫描首个失配 i → `log_error("mrn_rmw_fwd_sve: miscompare at index %zu: golden 0x%016llx actual 0x%016llx xor 0x%016llx", ...)` → `report_fail_msg`。
  - `mrn_rmw_l1d_sve_run`：同上，但 `svst1_u64(temp)` 与 `svld1_u64(temp)` 之间插入 `evict_range(temp + base, n * 8)`（片段 D）。头注释记录逐行 vs 原版逐元素的等价性论证。
- [ ] **Step 2.2: mrn_rmw_fwd_vs_diff_sve.cpp**（`mrn_rmw_diff_sve`）
  - 5 缓冲共享（srcA/srcB/expected/tempA/tempB 各 8KB；tempB 预填 expected，tempA 清零）。头注释记录 tempA 共享同值写安全性论证（所有线程写同一确定值、无人读 tempA、64B 对齐单拷贝原子；verdict: 无假阳性机制，保留原设计保真）。
  - run：批处理；res（片段 E）；`svst1_u64(pg, tempA+base, res)` → `svuint64_t got = svld1_u64(pg, tempB+base)`（**不同地址**——隔离转发路径）→ **批内立即**逐元素比较 `got[k] != expected[base+k]`（svst1 到栈再标量比较，或 svcmpne_n + svptest_any 后标量定位）→ 失配 `log_error` + `report_fail_msg`。
- [ ] **Step 2.3: mrn_rmw_diff_footprint_sve.cpp**（`mrn_rmw_diff_64k_sve` COUNT=8192 + `mrn_rmw_diff_1m_sve` COUNT=131072）
  - 参数化 init_common(count) + 共用 run（`const char *tag = test->id; int count = data->count;`——照抄原版 file-3 结构）；其余同 Step 2.2。
- [ ] **Step 2.4: mrn_rmw_timing_probe_sve.cpp**（6 测试：`mrn_rmw_nop_sve`(2 nop)、`mrn_rmw_nop1_sve`、`mrn_rmw_nop4_sve`、`mrn_rmw_nop8_sve`、`mrn_rmw_nop16_sve`、`mrn_rmw_nop64_sve`）
  - 共用 init/cleanup（同 fwd 的 3 缓冲）。
  - **6 个独立 run 函数**（.rept 需编译期 N，不能参数化共用）：每个 = mrn_rmw_fwd_sve_run，但 `svld1_u64(pg, temp+base)` 换成 `load_vec_delayedN_sve(temp + base)`（片段 C，N∈{2,1,4,8,16,64}；6 个 inline helper）。头注释记录：nop 间隔按原版周期标定移植到向量管线，间隔语义（str→ldr 时间窗）保留，绝对周期数不承诺等价。
- [ ] **Step 2.5: 注册 4 文件 + 构建**（同 Step 1.3 模式，meson 注释 `batch 2/8: mrn_rmw load-path family`）
- [ ] **Step 2.6: 逐测试验证**
```bash
for t in mrn_rmw_fwd_sve mrn_rmw_l1d_sve mrn_rmw_diff_sve mrn_rmw_diff_64k_sve mrn_rmw_diff_1m_sve \
         mrn_rmw_nop_sve mrn_rmw_nop1_sve mrn_rmw_nop4_sve mrn_rmw_nop8_sve mrn_rmw_nop16_sve mrn_rmw_nop64_sve; do
  ./builddir/sdcshield -e $t -t 60000 --cpuset='!122' 2>&1 | tail -1
done
# 预期: 11 行 exit: pass（nop 系首跑允许较慢启动; 任何 fail/崩溃 → 停下按 systematic-debugging 排查）
./builddir/sdcshield -e zstd19 -t 3000 -n 1 2>&1 | tail -1
```
- [ ] **Step 2.7: Commit + push**：`tests(sve): port mrn_rmw load-path family from missing_testcases_20261009 — fwd/l1d/diff/64k/1m/nop×6 (11 tests)`

---

### Task 3: mrn 构造族（6 测试 / 6 文件）

**Files:** `core179/mrn_nuke_2src_alu_sve.cpp`、`mrn_nuke_3src_alu_sve.cpp`、`mrn_nuke_4src_alu_sve.cpp`、`mrn_nuke_dense_sve.cpp`、`mrn_pairs_2src_alu_sve.cpp`、`mrn_reloaded_2src_alu_sve.cpp` + meson。

**模板**：`mrn_rmw_sve.cpp`（2/3/4src、pairs）+ `mrn_nuke_sve.cpp`（dense）+ `neon_rot_2src_sve.cpp` 的 `load_vec_sve`（reloaded 的 asm reload）。**必读原文件** 6 个。

- [ ] **Step 3.1: mrn_nuke_2src_alu_sve.cpp** — 主体 = mrn_rmw_sve_run 换名（同旋转 {+,-,^,&}、同 str→ldr→str、同 memcmp）；fail msg `mrn_nuke_2src_alu_sve data miscompare`。
- [ ] **Step 3.2: mrn_nuke_3src_alu_sve.cpp** — 3 源（srcA/B/C + expected）；旋转链：`{a^b^c, (a+b)^c, a^(b+c), (a^b)+c}`（片段 E 扩展：先算 4 个链结果 r0..r3 = svxor(svxor(a,b),c) / svxor(svadd(a,b),c) / svxor(a,svadd(b,c)) / svadd(svxor(a,b),c)，再 4 路 svsel 相位选择）；golden init 用标量同式；其余同 2src。
- [ ] **Step 3.3: mrn_nuke_4src_alu_sve.cpp** — 4 源；旋转链：`{a^b^c^d, ((a+b)^c)+d, (a^b)+(c^d), a^(b+c)^d}`；同上模式。
- [ ] **Step 3.4: mrn_nuke_dense_sve.cpp** — **逐字复用 mrn_nuke_sve.cpp 主体**，仅 COUNT 1024→16384（128KB/缓冲，无 ALU，golden=src）。
- [ ] **Step 3.5: mrn_pairs_2src_alu_sve.cpp** — 代码与 3.1 相同（头注释记录：原版与 nuke_2src 代码等同、仅谱系不同——mrn_pairs 的 8+8 展开塌缩 + 2src ALU，照实保留）。
- [ ] **Step 3.6: mrn_reloaded_2src_alu_sve.cpp** — 无 dst；`TEST_LOOP(test, 1<<8)`；每批：res（片段 E）→ `svst1_u64(pg, temp+base, res)` → `svuint64_t got = load_vec_sve(temp + base)`（**asm ldr z**，原版用 asm load_qword；且 reload 刚存地址有被编译器前递消除风险，asm 兜底）→ 批内立即逐元素比较 → `log_error("mrn_reloaded_2src_alu_sve: qword miscompare at %zu: expected 0x%016llx got 0x%016llx", ...)` + `report_fail_msg`。
- [ ] **Step 3.7: 注册 + 构建**（meson 注释 `batch 3/8: mrn constructed family`）
- [ ] **Step 3.8: 验证 6 测试**（同前模式，全部 `exit: pass`）+ zstd19 回归
- [ ] **Step 3.9: Commit + push**：`tests(sve): port mrn constructed family from missing_testcases_20261009 — nuke 2/3/4src + dense + pairs + reloaded (6 tests)`

---

### Task 4: store-only / 字节通道 / 随机地址（4 测试 / 4 文件）

**Files:** `mem/agu_stress_store_only_sve.cpp`、`core179/neon_rot_store_only_sve.cpp`、`core179/neon_rot_u8x16_sve.cpp`、`mem/mem_disambig_alu_sve.cpp` + meson。**必读原文件** 4 个。

- [ ] **Step 4.1: mem/agu_stress_store_only_sve.cpp** — mrn_rmw_sve 骨架，旋转 **`{+,^,&,|}`**（注意是 OR 不是 SUB，原版 agu 族的旋转）→ r_add/r_eor/r_and/r_orr 四结果相位选择；然后 `svst1_u64(pg, temp+base, res); svst1_u64(pg, dst+base, res)`（**两存无读**）；memcmp(dst, expected)。temp 只写不读（注释：纯 store 端口压力，reload 必要性对照）。
- [ ] **Step 4.2: core179/neon_rot_store_only_sve.cpp** — neon_rot_2src_sve 模板去掉 reload：asm ldr z 载 a/b → 相位选择 {add,eor,and,orr} → `store_vec_sve(temp+base, res); store_vec_sve(dst+base, res)`；memcmp。头注释记录 NEON 基线 ~33 fail/30min 的对照语义。
- [ ] **Step 4.3: core179/neon_rot_u8x16_sve.cpp** — 字节通道：缓冲 `uint8_t[16384]` ×3（srcA/srcB/expected，memset_random）；golden init 标量逐字节 `j%4 → {+,^,&,|}`；run 批 `svcntb()` 字节：`load_vec_sve_u8`（asm ldr z，返回 svuint8_t）载 a/b → 4 结果 svadd_u8_x/sveor_u8_x/svand_u8_x/svorr_u8_x 相位选择（phase 按**字节** (base+k)%4）→ `svst1_u8` 存 temp → `svst1_u8(dst, load_vec_sve_u8(temp))`（**asm reload**，原版如此）→ memcmp。头注释：16 字节通道 → 满 VL 字节通道（VL=256 时 32 通道），宽度-通道假设鉴别语义保留。
- [ ] **Step 4.4: mem/mem_disambig_alu_sve.cpp** — 随机地址转发：
  - init：`malloc(512*8)` ×3（offsets/values/expected——保持原版 malloc 不对齐）；`lanes=svcntd()` 捕获；`off = (random64() % (4096 - lanes*8)) & ~7ULL`；`values[i]=random64()`；expected 按旋转 `value {^,+, &,-} off`（i%4，**有减法**——原版序）。
  - run：per-thread `buffer = aligned_alloc(64, 4096)`；VL fail-closed 检查；`TEST_LOOP(test, 1<<13)`；每 pattern：`res = value op off`（标量，i%4）→ `svuint64_t v = svdup_u64(res)` → `svst1_u64(svptrue_b64(), (uint64_t*)buffer + off/8, v)` → `svuint64_t got = load_vec_sve((uint64_t*)buffer + off/8)`（asm reload 刚存地址）→ `svbool_t bad = svcmpne_n_u64(svptrue_b64(), got, res); if (svptest_any(bad)) report_fail_msg("mem_disambig_alu_sve: store-to-load forward (ALU) mismatch at pattern %d (offset %llu): expected 0x%llx", i, (unsigned long long)off, ...)`。
  - 头注释记录两处设计说明：全 lane 同值（原版标量语义的向量承载，被测路径是随机地址 st1d→ld1d 转发）；offset 上界按 lanes 收缩保证满 VL 读写不出 4096B 缓冲。
- [ ] **Step 4.5: 注册 + 构建 + 验证 4 测试 + zstd19 回归**（模式同前；meson 注释 `batch 4/8: store-only + byte-lane + random-address probes`）
- [ ] **Step 4.6: Commit + push**：`tests(sve): port store-only/byte-lane/random-address probes from missing_testcases_20261009 (4 tests)`

---

### Task 5: ldr_at_top 基础变体（5 测试 / 5 文件）

**Files:** `core179/neon_rot_ldr_at_top_sub_sve.cpp`、`_mla_sve.cpp`、`_offdiag_sve.cpp`、`_str3_sve.cpp`、`_ldr4_sve.cpp` + meson。

**模板**：`tests/cpu/sve/core179/neon_rot_ldr_at_top_sve.cpp`（原系父本移植，fwd-only 扫描）+ `neon_rot_ldr_at_top_rowmajor_sve.cpp`（rowmajor 交替扫描 + VL fail-closed + 槽范式缓冲）。**必读原文件** 5 个 + 两个模板。

**Interfaces（槽范式）:** data struct 含 `int lanes` + 各 `uint64_t*/uint32_t*` 缓冲（SLOTS×lanes 元素，init 时 `aligned_alloc(64, elems*es)`）；run 开头 VL fail-closed 检查；golden init 逐槽 `switch(slot%4)` 单操作整向量（与 run 同 SVE 单元）；asm ldr z 载源、`store_vec_sve` 存。

- [ ] **Step 5.1: sub** — 槽范式 u64，SLOTS=1024，**fwd-only 扫描**（照抄 neon_rot_ldr_at_top_sve 的顺序循环，非 rowmajor）；旋转 `{sub, eor, bic, orn}`：`svsub_u64_x / sveor_u64_x / svbic_u64_x / svorr_u64_x(pg, a, svnot_u64_x(pg, b))`（ORN 组合，头注释记录 svorn 不存在于 GCC 12 SVE1）；顶部死载 scratch（asm ldr z，`(void)` 丢弃）；背靠背双存 temp/dst；每 sweep memcmp(dst)。头注释：进位 vs 组合逻辑签名鉴别语义。
- [ ] **Step 5.2: mla** — 槽范式 **u32**（`svuint32_t`，lanes=svcntw()，SLOTS=1024 向量 = 1024×lanes u32）；init 填充后**收窄持久化** `srcA[i] = srcA[i] & 0x0000FFFF`（标量逐 u32，照抄原版 narrow_for_mul）→ golden 逐槽 `{vmul, a+a*b(mla式), b-a*b(mls式), eor}` → SVE：`svmul_s32_x / svadd_s32_x(pg, svmul_s32_x(pg,sa,sb), sa) / svsub_s32_x(pg, svmul_s32_x(pg,sa,sb), sb) / sveor_u32_x`（svreinterpret_s32_u32 包转）；fwd-only；双存；memcmp。
- [ ] **Step 5.3: offdiag** — 单 interleaved 缓冲 `3×SLOTS` 向量（u64 槽范式）+ scratch + expected；死载 scratch 顶部；`a = load_vec_sve(interleaved + (3*i+1)*lanes)`、`b = load_vec_sve(interleaved + (3*i+2)*lanes)`（**相邻 32B**——地址间隔必要性鉴别）；旋转父系 `{add,eor,and,orr}`；fwd-only；双存；memcmp。头注释记录总加载足迹与父系相同（3×16KB→3×SLOTS×lanes×8B）。
- [ ] **Step 5.4: str3** — **rowmajor 交替扫描**（照抄 rowmajor_sve 的 sweep 结构）+ 顶部死载 scratch + 2 源载 + 旋转 {add,eor,and,orr} + **三存** `temp[i]/junk[i]/dst[i]` 同一 res（junk 只写不读）；memcmp 仅 dst（原版盲区保留——str3_checkall 才补全）。per-thread temp/dst/junk 三缓冲。
- [ ] **Step 5.5: ldr4** — rowmajor；共享 5 缓冲（srcA/srcB/scratch1/scratch2/expected）；**顶部两个死载**（scratch1、scratch2 连续 asm ldr z）→ 2 源载 → 旋转 → 双存；memcmp。
- [ ] **Step 5.6: 注册 + 构建 + 验证 5 测试 + zstd19 回归**（meson 注释 `batch 5/8: ldr_at_top base variants`）
- [ ] **Step 5.7: Commit + push**：`tests(sve): port neon_rot_ldr_at_top base variants from missing_testcases_20261009 — sub/mla/offdiag/str3/ldr4 (5 tests)`

---

### Task 6: rowmajor 仪表化变体（4 测试 / 4 文件）

**Files:** `core179/neon_rot_ldr_at_top_rowmajor_dump_sve.cpp`、`_gps_sve.cpp`、`_dualcmp_sve.cpp`、`core179/neon_rot_ldr_at_top_rowmajor_str3_checkall_sve.cpp` + meson。

**模板**：`neon_rot_ldr_at_top_rowmajor_sve.cpp`（热路径逐字）+ 原文件冷路径（标量化移植——冷路径性能无关紧要，全部用标量 u64 循环做分类/记录，规避 NEON 向量对格式）。**必读原文件** 4 个。

- [ ] **Step 6.1: dump** — 热路径 = rowmajor_sve 逐字；init 追加 `open("/var/tmp/neon_rot_ldr_at_top_rowmajor_dump_sve_<pid>.bin", O_WRONLY|O_CREAT|O_APPEND, 0644)`（失败 log_error 继续）；冷路径：memcmp 失配 → **noinline** 标量扫描全元素，逐失配 i：`log_error`(index/op/sweep_dir/srcA/srcB/exp/act/xor 十六进制) + `write(fd, &rec, sizeof rec)`（片段 G 记录结构）；cleanup 关 fd。头注释记录记录格式与原版差异（原版 16B 向量对 lo/hi → SVE 版元素粒度单 u64 五元组）。
- [ ] **Step 6.2: gps** — init `mkdir("movbe_log/gps_rowmajor", 0755)`（**保留相对路径**，头注释记录 CWD 依赖与原版一致）；GPS 标签填充（片段 F：srcA=0xA5A5/srcB=0x5A5A/scratch=0x0F0F，byte_off=字内偏移，gen=0，**无 RNG**）；golden 从标签数据按槽旋转 {add,eor,and,orr}；热路径 rowmajor 逐字；冷路径：五缓冲整转储（dst/expected/srcA/srcB/scratch 顺序 write 到 `movbe_log/gps_rowmajor/neon_rot_ldr_at_top_rowmajor_gps_sve_fail_cpu<cpu>_sweep<sweep>.bin`）+ 尾行 `cpu=%d sweep=%lu\n` + report_fail_msg。
- [ ] **Step 6.3: dualcmp** — init 同 mkdir；热路径 rowmajor 双存逐字；**每 sweep 两个无条件 memcmp**（dst vs golden、temp vs golden）；任一失配 → noinline 标量分类循环统计 `{a_only(dst错/temp对), b_only, ab_div(两者互异), ab_same(同错≠golden), first_bad}` → 五缓冲转储 + stats 行 → `report_fail_msg` 带四计数摘要。头注释记录读出语义（哪条 store 腿 / 分叉上游）。
- [ ] **Step 6.4: str3_checkall**（测试名**无 rowmajor**）— str3 热路径（三存 temp/junk/dst）+ 每 sweep **三个无条件 memcmp** → slot_mask(bit0=temp,bit1=junk,bit2=dst)；失配 → noinline 冷扫描：逐槽三路比较得 index_slot_mask + srcA/srcB 冷重读（asm ldr z，load-错误 vs ALU-错误分类）+ 逐失配 log + 记录写 + **瞬态处理**（重读干净 → 写 sweep 级记录 index=0xFFFFFFFF）；`/var/tmp/neon_rot_ldr_at_top_str3_checkall_sve_<pid>.bin` 追加。
- [ ] **Step 6.5: 注册 + 构建 + 验证 4 测试**（60s × !122）+ zstd19 回归（meson 注释 `batch 6/8: rowmajor instrumented variants`）。验证时同时确认不产生误转储：`ls movbe_log/gps_rowmajor/ /var/tmp/*_sve_*.bin 2>/dev/null` 应只有 dump_golden 的 golden 文件、无 fail 转储。
- [ ] **Step 6.6: Commit + push**：`tests(sve): port rowmajor instrumented variants from missing_testcases_20261009 — dump/gps/dualcmp/str3_checkall (4 tests)`

---

### Task 7: K-cells（7 测试 / 4 文件）

**Files:** `core179/neon_rot_ldr_at_top_rowmajor_k5far_sve.cpp`、`_k5inter_rand_sve.cpp`、`_k3res_sve.cpp`、`_k7pattern_sve.cpp`（4 测试）+ meson。

**模板**：rowmajor_sve（骨架+转储纪律）+ gps_sve（T6 刚建的标签/转储模式，可复制其冷路径函数）。**必读原文件** 4 个。

- [ ] **Step 7.1: k5far** — 单 interleaved src 缓冲 `2×SLOTS` 向量（u64）；死载 scratch 顶部；`a = load_vec_sve(src + 2*i*lanes)`、`b = load_vec_sve(src + (2*i+1)*lanes)`；GPS 标签**按向量槽奇偶选族**（槽 e：magic = e&1 ? 0x5A5A : 0xA5A5；字内 byte_off = 该字在 interleaved 全缓冲内偏移）；旋转 {add,eor,and,orr}；双存；memcmp；冷路径物化 viewA/viewB（aligned_alloc 反交错拷贝 + free，冷路径）→ 五缓冲转储（dst/expected/viewA/viewB/scratch）+ `k5far sweep=%lu` 行。
- [ ] **Step 7.2: k5inter_rand** — 同 k5far 布局但 `memset_random` 填充（自然随机对照，隔离布局效应 vs 标签效应）；转储 `k5ir_*`。
- [ ] **Step 7.3: k3res** — **SLOTS=4096**（4× 足迹；VL=256 时每缓冲 128KB）；GPS 标签**平行双缓冲布局**（非交错）；byte_off 超过 16 位字段时 `& 0xFFFF` 回绕（头注释记录）；旋转父系；双存；memcmp；五缓冲转储 `k3res_*`（约 640KB/文件，照常）。
- [ ] **Step 7.4: k7pattern**（`k7p_zero/one/rand/hiham_sve` 4 测试共用实现）— `k7p_init_common(test, pattern)` / `k7p_run_common(test, cpu, name)` / `k7p_cleanup_common`；pattern 枚举：`zero`（全 0x0000000000000000）/ `one`（全 0xFFFFFFFFFFFFFFFF）/ `rand`（GPS 标签，确定性——**注意名字叫 rand 但不用 RNG**，头注释照原版说明）/ `hiham`（逐字节 `(byte_off + b) & 1 ? 0xAA : 0x55`）；`pattern_word(pattern, which, byte_off)` 辅助函数；四 DECLARE_TEST 仅差 pattern 枚举与 name（转储文件名/失败消息用 name）；热路径 rowmajor 骨架逐字；memcmp + 五缓冲转储 `<name>_fail_cpu<cpu>_sweep<sweep>.bin`。
- [ ] **Step 7.5: 注册 + 构建 + 验证 7 测试 + zstd19 回归**（meson 注释 `batch 7/8: K-cells`）
- [ ] **Step 7.6: Commit + push**：`tests(sve): port K-cells from missing_testcases_20261009 — k5far/k5inter_rand/k3res/k7p×4 (7 tests)`

---

### Task 8: arm-0102 家族（4 测试 / 4 文件）

**Files:** `kreg/arm0102_kreg_select_sve.cpp`、`arm0102_kreg_not_sve.cpp`、`arm0102_kreg_logic_sve.cpp`、`arm0102_fma_f32_sve.cpp` + meson。

**模板**：`tests/cpu/sve/kreg/arm0102_kreg_mask_sve.cpp`（pg4 子块范式完整模板：`#define KREG_SVE_SUBLANES 4`、pg4 截断的 store_vec_sve、asm ldr z 全宽载 + pg4 截断、golden 循环镜像子块结构/相位 `(base+sub)%4`）。**必读原文件** 4 个 + 模板。**缓冲按 Global Constraint 6 分配 `(1024 + 8) * sizeof(uint32)`（+8 元素松弛防尾部 asm 全宽读过界——头注释记录与原版模板的差异）**。

**Interfaces:**
- 载荷函数签名统一 `static inline svuint32_t payload_rotating(svbool_t pg4, svuint32_t a, svuint32_t b, int phase)`（fma 版多一参 `svfloat32_t c` 返回 svfloat32_t）。
- 放大器统一：`store_vec_sve(temp + base + sub, res); store_vec_sve(dst + base + sub, load_vec_sve(temp + base + sub));`（str z → ldr z → str z）。

- [ ] **Step 8.1: kreg_select** — 载荷（phase i%4，**BSL 组合式**，Global Constraint 5）：
```cpp
case 0: return svorr_u32_x(pg, svand_u32_x(pg, a, b),      /* BSL(a,b,b^a) */
                           svand_u32_x(pg, svnot_u32_x(pg, a), sveor_u32_x(pg, b, a)));
case 1: return svorr_u32_x(pg, svand_u32_x(pg, a, a),      /* BSL(a,a,b) */
                           svand_u32_x(pg, svnot_u32_x(pg, a), b));
case 2: return svorr_u32_x(pg, svand_u32_x(pg, a, b),      /* BSL(a,b,a) */
                           svand_u32_x(pg, svnot_u32_x(pg, a), a));
default: return svorr_u32_x(pg, svand_u32_x(pg, a, svand_u32_x(pg, a, b)),  /* BSL(a, a&b, a^b) */
                           svand_u32_x(pg, svnot_u32_x(pg, a), sveor_u32_x(pg, a, b)));
```
  （以原文件 `arm0102_kreg_select.cpp` 的 vbslq 表达式为准逐一对应；上表按 agent 摘要 case0=`vbslq(a,b,b^a)` 等写出，**实现时对照原文件核对每个 case 的三操作数**。）
- [ ] **Step 8.2: kreg_not** — 载荷 `{nand, nor, xnor, nandnot}`：`svnot_u32_x(pg, svand/svorr/sveor/svbic_u32_x(pg, a, b))`。
- [ ] **Step 8.3: kreg_logic** — 载荷 `{and, or, xor, andnot}`：`svand/svorr/sveor/svbic_u32_x`（kreg1_sve 先例）。
- [ ] **Step 8.4: fma_f32** — **f32 通道 pg4**：3 源（a/b/c 均 asm ldr z，float 缓冲 `(1024+8)` f32）；init 通道 `frandomf_scale(100.0f)` 逐元素填（照抄原版——raw random 会出 NaN/Inf）；载荷按原文件 vfmaq/vfmsq/vnegq 表达式逐一映射：`vfmaq_f32(d,a,b) → svmla_f32_x(pg4,d,a,b)`、`vfmsq_f32(d,a,b) → svmsb_f32_x(pg4,d,a,b)`、`vnegq_f32(v) → svneg_f32_x(pg4,v)`（嵌套组合保持原式结构）；golden 用相同映射逐子块预计算；放大器 str/ldr/str；memcmp(dst, expected, 1024*4)。
- [ ] **Step 8.5: 注册 + 构建 + 验证 4 测试 + zstd19 回归**（meson 注释 `batch 8/8: arm-0102 trigger-recipe variants`）
- [ ] **Step 8.6: Commit + push**：`tests(sve): port arm-0102 kreg/fma variants from missing_testcases_20261009 — select/not/logic/fma_f32 (4 tests)`

---

### Task 9: 文档同步（README 计数 + docs_xu 批次文档）

**Files:**
- Modify: `README.md`（测试计数脚注：`--list-tests` 实测 406 → 450，及 SVE 套件 120 → 164 文件描述行——以实测为准）
- Create: `docs/docs_xu/2026-10-10-missing-testcases-sve-port.md`（批次文档）

- [ ] **Step 9.1: 实测计数**
```bash
./builddir/sdcshield --list-tests | wc -l          # 预期 450（406 + 44）
./builddir/sdcshield --list-tests | grep -c '_sve' # 预期 163（119 + 44）
find tests/cpu/sve -name '*.cpp' | wc -l           # 预期 164（120 + 44... 核对: 新文件数 = 2+4+6+4+5+4+4+4 = 33 个文件含 44 测试）
```
（注意：44 测试分布在 33 个新文件中——k7pattern 4 测试 1 文件、timing_probe 6 测试 1 文件、l1d_vs_l2 与 footprint 各 2 测试 1 文件。）
- [ ] **Step 9.2: README 计数与 SVE 套件行更新**（只改计数/清单行，不动其他）。
- [ ] **Step 9.3: docs_xu 批次文档** — 内容必须包含：44 测试全表（名/文件/家族/探针维度）、eigen 不移植裁决及证据（eigen_svd_cdouble_sve 同构 + NEON 原版在场逐字节一致）、`mrn_rmw_diff_nop64` 不存在的事实记录、三个范式说明、已知偏差清单（evict 逐行化 / ORN·BSL 组合式 / k3res byte_off 回绕 / pg4 松弛分配 / 记录格式元素粒度 / golden 文件 /var/tmp+pid / movbe_sve 栈中转 vs l1d 堆直读——后者是设计差异非偏差）、验证结果汇总（引用真实输出）。
- [ ] **Step 9.4: Commit + push**：`docs: sync README counts + docs_xu batch record for missing_testcases SVE ports (44 tests)`

---

### Task 10: 可靠性测试（全量第二遍）

**目的**：证明 44 个新测试在健康核上零假阳性（两遍独立采样，RNG 引擎每运行随机选——跨引擎零误报证据）。

- [ ] **Step 10.1: 全量第二遍 60s × !122**
```bash
mkdir -p sdc_hunt_logs/2026-10-10-missing-sve-reliability
TESTS="<44 个测试名，逗号分隔或 -e 正则>"
# sdcshield -e 支持逗号分隔多测试; 逐个跑并记录:
for t in <44 names>; do
  ./builddir/sdcshield -e $t -t 60000 --cpuset='!122' \
    > sdc_hunt_logs/2026-10-10-missing-sve-reliability/$t.log 2>&1
  echo "$t rc=$? $(tail -1 sdc_hunt_logs/2026-10-10-missing-sve-reliability/$t.log)"
done | tee sdc_hunt_logs/2026-10-10-missing-sve-reliability/summary.txt
# 预期: 44 行全 rc=0 + exit: pass（第一遍 = 各任务 Step 验证时已跑）
```
- [ ] **Step 10.2: 汇总核验** — `grep -c 'exit: pass' summary.txt` = 44；任何非 pass → 停下排查（systematic-debugging），不得带病收尾。
- [ ] **Step 10.3: 可靠性结论写入 docs_xu 文档**（T9 的文档补上第二遍数据；或作为文档的验证节）。

---

### Task 11: 有效性冒烟测试（SVE 指令证据 + 故障注入检出）

- [ ] **Step 11.1: objdump SVE 指令证据（44 测试全覆盖）**
```bash
nm builddir/sdcshield | grep -E ' [tT] ' | grep -E '(movbe_l1d|movbe_l2|movbe_dump_golden|mrn_rmw_|mrn_nuke_|mrn_pairs_2src|mrn_reloaded_2src|agu_stress_store_only|neon_rot_store_only|neon_rot_u8x16|ldr_at_top_.*_sve|arm0102_(kreg_select|kreg_not|kreg_logic|fma_f32))_sve?_?run' > /tmp/symbols.txt
# 对每个 run 符号:
#   objdump -d --disassemble=<sym> builddir/sdcshield | grep -cE '\b(ldr[[:space:]]+z|ld1d|st1d|ld1b|st1b|whilelt|ptrue)\b'
# 预期: 每个测试 ≥ 1（asm ldr z 或谓词 ld1d/st1d 必须在场——证明热路径真 SVE 且 reload 未被消除）
# 写成 for 循环脚本, 输出 <test>: <count> 清表存入 docs_xu 文档附录
```
- [ ] **Step 11.2: 故障注入检出证明（每批 1 代表 × 8）**

代表（每任务一个）：`movbe_l1d_sve`(T1)、`mrn_rmw_fwd_sve`(T2)、`mrn_nuke_2src_alu_sve`(T3)、`mem_disambig_alu_sve`(T4)、`neon_rot_ldr_at_top_sub_sve`(T5)、`neon_rot_ldr_at_top_rowmajor_dualcmp_sve`(T6)、`neon_rot_ldr_at_top_rowmajor_k7p_rand_sve`(T7)、`arm0102_kreg_select_sve`(T8)。

方法（git worktree 隔离，不动主树）：
```bash
git worktree add /tmp/fi-check-wt HEAD
# boost 头未跟踪, 需带过去:
mkdir -p /tmp/fi-check-wt/third-party && cp -r third-party/boost-headers /tmp/fi-check-wt/third-party/
# 8 个注入点（sed 精确改动，每处一行）:
#  - expected[] 类(6 个): init 的 golden 循环之后插 data->expected[0] ^= 1;
#    sed -i 's|test->data = data;|data->expected[0] ^= 1;\n    test->data = data;|' <文件>
#  - movbe_l1d_sve: idx4 表 {3, 2, 1, 0} → {3, 2, 1, 1}（破坏 bswap 可逆性）
#  - mem_disambig_alu_sve: expected 计算后 data->expected[0] ^= 1;
cd /tmp/fi-check-wt && PKG_CONFIG_PATH=./third-party/eigen5 meson setup builddir-fi --buildtype=release 2>&1 | tail -2
ninja -C builddir-fi sdcshield 2>&1 | tail -2        # 预期: 构建成功（第三方缺失项优雅降级）
for t in <8 代表名>; do
  ./builddir-fi/sdcshield -e $t -t 10000 -n 4 2>&1 | tail -1
done
# 预期: 8 行全为 fail/非 pass（report_fail 触发 = 检出路径有效）
```
- [ ] **Step 11.3: 清理**
```bash
git worktree remove --force /tmp/fi-check-wt && rm -rf /tmp/rpm-dl /tmp/rpm-extract /tmp/sve_probe*.c
# 主树 git status 应干净（除既有未跟踪研究文件）
```
- [ ] **Step 11.4: 有效性结论 + 全部验证证据写入 docs_xu 文档**（objdump 清表 + 8 代表 fail 输出 + 可靠性两遍汇总）→ 补 commit：`docs(docs_xu): effectiveness evidence for missing_testcases SVE ports` + push。
- [ ] **Step 11.5: 收尾报告** — 向用户汇报：44 测试清单、编译/可靠性/有效性三项证据摘要、已知偏差清单、eigen 裁决、含 122 的阶段 2 观察待裁决（不自行启动）。

## Self-Review 记录

- 覆盖核对：MANIFEST 38 有效 + 8 对照（其中 diff_nop64 不存在→实际 45）− eigen 1（已有 SVE 口）= 44 ✓ 全部落入 T1-T8 映射表。
- 占位符扫描：无 TBD/TODO；svorn/BSL 等"不可用"项均给出已编译验证的组合式代码。
- 类型/命名一致性：44 个测试名在映射表与各任务步骤一致；`_sve` 后缀统一；`neon_rot_ldr_at_top_str3_checkall_sve` 名字无 rowmajor 与原版对齐。
- 已知风险：k7pattern 的 k7p_rand 用 GPS 标签（非 RNG）易误读——已在 Step 7.4 显式标注；timing probe 的 .rept 与 ldr 必须同 asm 块——已在片段 C 定死；批处理范式尾部整除性已论证（Global Constraint 6）。
