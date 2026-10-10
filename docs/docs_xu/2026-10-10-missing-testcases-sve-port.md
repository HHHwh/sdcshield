# 179 机转移用例的 SVE 移植（missing_testcases_20261009，2026-10-10/11）

> 任务：把 `missing_testcases_20261009/` 转移包中的测试用例全部移植为 SVE 版本放入 `tests/cpu/sve/`，完成编译、可靠性测试、有效性冒烟测试。
> 实施计划：`docs/superpowers/plans/2026-10-10-missing-testcases-sve-port.md`；批次提交 24b250d7 →（8 个实现 commit）。

## 一、移植范围与裁决

转移包 = 179 机有效 SDC 用例登记册中本机所缺部分：**34 个源文件 / 45 个 DECLARE_TEST**（MANIFEST 记 38 有效 + 8 对照 = 46，其中 `mrn_rmw_diff_nop64` 实际不存在于源文件——仅被 nop64 的描述引用，实测 45）。

**移植 44 个（33 个新 .cpp 文件），eigen 1 个不移植**：

- **eigen_svd_cdouble_noavx512 不移植的证据**：(a) 其 NEON 原版 `tests/cpu/eigen_svd/svd_cdouble_noavx512.cpp` 本仓库已在场且与转移包**逐字节一致**（diff rc=0）；(b) 本仓库 `eigen_svd_cdouble_sve` 已是其 SVE 口——同 BDCSVD/同 `EigenSVDTest` golden 机制（init 算一次、run 重算 memcmp）/同 do-while 迭代结构/同 `group_math` + `fracture_loop_count=5`，且为严格超集（运行时维度 ≥300，`-O eigen_svd_cdouble_sve.mdim=300` 精确复现 noavx512 的 300×300 工况）。

44 个含 6 个同文件带出的 0-fail 对照（movbe_l2、mrn_rmw_l1d、mrn_rmw_diff_1m、mrn_rmw_nop8/16/64、k7p_zero）——它们比对真实数据、有完整检出路径，只是 179 机历史上零触发，照原样移植。

## 二、44 测试清单（批次 → 文件 → 探针维度）

| 批 | 原文件（misc/ 除注明外） | 测试（均 PROD 级） | 探针维度 |
|---|---|---|---|
| T1 | movbe_l1d_vs_l2.cpp | movbe_l1d_sve, movbe_l2_sve | 重载路径定位：input 堆 reload 命中 L1D vs dc civac 逐行冲刷后 L2 供给 |
| T1 | movbe_dump_golden.cpp | movbe_dump_golden_sve | init 线程落盘独立 golden（/var/tmp+pid 后缀，偏差见下） |
| T2 | mrn_rmw_fwd_vs_l1d.cpp | mrn_rmw_fwd_sve, mrn_rmw_l1d_sve | 存缓冲转发 vs L1D 阵列读（str→ldr 同址，evict 隔离） |
| T2 | mrn_rmw_fwd_vs_diff.cpp | mrn_rmw_diff_sve | 异址隔离：str tempA → ldr tempB（时间窗不变、仅换加载地址） |
| T2 | mrn_rmw_diff_footprint.cpp | mrn_rmw_diff_64k_sve, mrn_rmw_diff_1m_sve | tempB 足迹 64KB=L1D 踩边 vs 1MB=16×L1D |
| T2 | mrn_rmw_timing_probe.cpp | mrn_rmw_nop{,1,4,8,16,64}_sve | str→ldr 时间窗 6 点梯度（.rept mov x9,x9 与 ldr 融合单 asm 块） |
| T3 | mrn_nuke_{2,3,4}src_alu.cpp | mrn_nuke_{2,3,4}src_alu_sve | 依赖链深度 1/2/3（ALU 链式旋转） |
| T3 | mrn_nuke_dense.cpp | mrn_nuke_dense_sve | 载荷密度 16×（16384 元素，golden=src） |
| T3 | mrn_pairs_2src_alu.cpp | mrn_pairs_2src_alu_sve | mrn_pairs 8+8 展开塌缩为背靠背（与 nuke_2src 代码等同，谱系保留） |
| T3 | mrn_reloaded_2src_alu.cpp | mrn_reloaded_2src_alu_sve | 179 机全史最高率工况：无 dst、TEST_LOOP 1<<8、立即比对 |
| T4 | agu_stress_store_only.cpp | agu_stress_store_only_sve (mem/) | reload 必要性：双存无读（旋转 {+,^,&,\|}，OR 非 SUB） |
| T4 | neon_rot_store_only.cpp | neon_rot_store_only_sve | 向量通路 reload 必要性（NEON 基线 ~33 fail/30min） |
| T4 | neon_rot_u8x16.cpp | neon_rot_u8x16_sve | 宽度 vs 通道：满 VL 字节通道（VL=256 时 32 通道 vs 原 16） |
| T4 | memory/mem_disambig_alu.cpp | mem_disambig_alu_sve (mem/) | 消歧预测器：数据依赖随机地址上的 svst1→ldr z 转发（旋转 {^,+,&,−}） |
| T5 | neon_rot_ldr_at_top_sub.cpp | …_sub_sve | ALU 维度：sub/eor/bic/orn（进位 vs 组合逻辑签名） |
| T5 | neon_rot_ldr_at_top_mla.cpp | …_mla_sve | 代数维度：u32 MAC 通道 mul/mla式/mls式/eor（收窄持久化） |
| T5 | neon_rot_ldr_at_top_offdiag.cpp | …_offdiag_sve | 地址布局：单交错缓冲 [3i+1]/[3i+2]（源对相邻） |
| T5 | neon_rot_ldr_at_top_str3.cpp | …_str3_sve | 存侧计数梯度：3 str（temp/junk/dst，仅查 dst——原版盲区保留） |
| T5 | neon_rot_ldr_at_top_ldr4.cpp | …_ldr4_sve | 载侧计数梯度：4 ldr（顶部双死载；rowmajor 扫描） |
| T6 | …rowmajor_dump.cpp | …rowmajor_dump_sve | 仪表化：逐元素日志 + 元素粒度二进制记录（/var/tmp） |
| T6 | …rowmajor_gps.cpp | …rowmajor_gps_sve | GPS 自描述标签（magic\|byte_off\|~byte_off\|gen）+ 五缓冲转储 |
| T6 | …rowmajor_dualcmp.cpp | …rowmajor_dualcmp_sve | 双 store 腿对照 + a_only/b_only/ab_div/ab_same 分类 |
| T6 | …rowmajor_str3_checkall.cpp | …str3_checkall_sve | 三槽全查 slot_mask + 瞬态标记（index=0xFFFFFFFF） |
| T7 | …rowmajor_k5far.cpp | …k5far_sve | 交错双源：地址邻居恒为另一族（GPS 奇偶魔数） |
| T7 | …rowmajor_k5inter_rand.cpp | …k5inter_rand_sve | 布局对照：同布局自然随机（隔离布局效应 vs 标签效应） |
| T7 | …rowmajor_k3res.cpp | …k3res_sve | 驻留：4096 槽 4× 足迹（byte_off 16 位回绕） |
| T7 | …rowmajor_k7pattern.cpp | …k7p_{zero,one,rand,hiham}_sve | 模式激活：全 0/全 1/GPS 标签（确定性）/0x55·0xAA 高海明 |
| T8 | arm-0102/arm0102_kreg_select.cpp | arm0102_kreg_select_sve (kreg/) | 旋转 BSL 四式（组合式，无 svbsl） |
| T8 | arm-0102/arm0102_kreg_not.cpp | arm0102_kreg_not_sve | nand/nor/xnor/nandnot（svnot 覆盖） |
| T8 | arm-0102/arm0102_kreg_logic.cpp | arm0102_kreg_logic_sve | and/or/xor/andnot（1:1 映射） |
| T8 | arm-0102/arm0102_fma_f32.cpp | arm0102_fma_f32_sve | 三源 f32 FMA 旋转（neg 嵌套 fnma/fnms 逐字保留） |

## 三、三个移植范式（沿用既有套件，按家族对应）

1. **批处理范式**（mrn/movbe/mem 族，模板 mrn_rmw_sve）：固定元素数缓冲（全部 2 的幂，任意 2 幂 lanes 整除无尾越界）+ svwhilelt 谓词批 + **相位谓词旋转 ALU**（先算 4 个操作结果，逐 lane 相位 (base+k)%4 svsel 分派——修复批次 5 的 base%4 恒 0 bug 之教训）。
2. **槽范式**（ldr_at_top 全族，模板 neon_rot_ldr_at_top_sve/rowmajor_sve）：SLOTS×lanes 定容、init 捕获 lanes + run 开头 **VL 失配 fail-closed**、asm `ldr %0,[%1]`（`"=w"` z 寄存器）满 VL 源载/死载、逐槽 switch 单操作整向量（golden 与 run 同 SVE 单元）。
3. **pg4 子块范式**（arm0102 族，模板 arm0102_kreg_mask_sve）：4×32 位子块谓词保 NEON 4-lane 语义（svptrue 在 VL=256 是 8 lane 会越界砸堆——批次 1 实测 bug）+ golden 镜像同子块结构同相位 + **(COUNT+8) 元素尾部松弛**（新偏差，见下）。

**配对加载铁律**：所有"reload 刚存储地址"的加载（mrn 族 temp 回读、movbe_l1d 的 input 重载、diff 的 tempB、reloaded/u8x16/arm0102 放大器、mem_disambig 的随机地址回读、nop 系延迟加载）一律 **asm ldr z + "memory" clobber**——防 CSE（movbe_l1d 的 in1/in2 同地址双载）、防编译期 store→load 前递（u8x16 原版实证 NEON vld1q 被消除）、防跨 asm 重排。非配对的普通源载用 intrinsic svld1（既有 mrn_rmw_sve 实证 st1d→ld1d 三连在场，objdump 6 ld1d/3 st1d）。

## 四、与原版的已记录偏差（全部在源文件头注释中标注）

1. **evict 逐行化**：原版逐 4B/8B 元素 dc civac（同行重复冲刷），SVE 版对批覆盖的每条 64B 行各冲刷一次——终态等价（reload 前全部行已出 L1D）。
2. **ORN/BSL 组合式**：GCC 12 SVE1 ACLE 无 `svorn_*`/`svbsl_*`（编译实测确认）。ORN = `svorr(a, svnot(b))`；BSL(a,b,c) = `svorr(svand(a,b), svand(svnot(a),c))`。BIC/svnot 等其余 1:1。
3. **k3res byte_off 回绕**：VL=256 足迹 128KB 超 16 位标签域 → `byte_off & 0xFFFF`（补码取回绕值，完整性检查自洽；magic 不受影响，确定性可离线解码）。
4. **pg4 尾部松弛**：arm0102_kreg_mask_sve 模板最后子块的 asm 全宽 ldr z 过读 16B（越界 UB）→ 新端口缓冲 +8 元素（32B）。
5. **记录格式元素粒度**：rowmajor_dump 的二进制记录从原版 16B 向量 lo/hi 对改为逐 u64 元素单值五元组（srcA/srcB/exp/act/xor）——分辨率更细、信息集相同、VL 无关。
6. **golden 文件路径**：movbe_dump_golden 从固定 `/tmp/...`（tmpfs 崩机即失 + 并发互踩）改 `/var/tmp/movbe_input_golden_sve_<pid>.bin`（家族 dump 惯例 + pid 防并发）。
7. **movbe_l1d 堆直读**：movbe_sve（既有）先 memcpy 到栈再加载；l1d 端口被测路径就是 input 的堆 reload，必须堆直读（svtbl 构造照抄）。这是设计差异非缺陷修正。
8. **mem_disambig 满宽随机地址**：偏移钳制从 SIZE-16 收缩到 SIZE-lanes×8（满 VL 读写不出 4096B 缓冲）；全 lane 同值承载原标量语义（被测路径是随机地址 st1d→ld1d 转发）。
9. **nop 时间窗标定**：`.rept` 间隔在标量管线的周期标定不承诺在向量管线等价（设计=转发路径上的单调间隙梯度，保留）。
10. **k7p_rand 名字**：叫 rand 但用确定性 GPS 标签（原版如此，头注释照实说明）。

## 五、验证记录

### 第一遍（每批 60s × `--cpuset='!122'`，两阶段协议阶段 1——历史执行方式，见下注）

> **⚠️ 2026-10-11 用户裁定：`--cpuset='!122'` 跑法废止**——127 核满载会把后台守护进程挤到唯一空闲的 122 号故障核上集中负载，有崩溃风险。**可靠性测试的正确形态 = `--cpuset=0-63` 半核**（物理不含 122、其余核心空闲供守护进程分散）。下表为历史执行记录（幸运未出事），第二遍（T10）起按 0-63 半核执行。

构建：`meson setup --reconfigure && ninja` 每批零新增 error/warning（既有 mesh/jit 文件警告白名单外无；clangd 的 `xTEST_ID_*`/`pragma-pack` 诊断确认为过期 compile_commands 幽灵，GCC 实编干净）。

| 批 | 测试数 | 结果 |
|---|---|---|
| T1 movbe 三连 | 3 | 3× `exit: pass`（golden 文件 65536B 落盘；objdump 抽查 movbe_l1d：`ldr z`×2 + `tbl`×2 + `st1b` + `cmpne` 全在场） |
| T2 mrn_rmw | 11 | 11× `exit: pass` |
| T3 mrn 构造族 | 6 | 6× `exit: pass` |
| T4 store-only/字节/随机地址 | 4 | 4× `exit: pass` |
| T5 ldr_at_top 基础 | 5 | 5× `exit: pass` |
| T6 rowmajor 仪表化 | 4 | 4× `exit: pass`；零 fail 副作用（/var/tmp 记录文件全 0 字节 = init 创建；movbe_log/gps_rowmajor 无 fail 转储） |
| T7 K-cells | 7 | 7× `exit: pass` |
| T8 arm0102 | 4 | 4× `exit: pass` |

每批 zstd19 回归 `exit: pass`。计数：`--list-tests` 406 → **450**（+44）；`_sve` 名 119 → **163**；`tests/cpu/sve/` 120 → **153 文件**（+33，44 测试分布其中：k7pattern 4 测试 1 文件、timing_probe 6 测试 1 文件、l1d_vs_l2 与 footprint 各 2 测试 1 文件）。

### 可靠性测试（T10，0-63 半核）与有效性冒烟（T11，全核含 122）

**语义（2026-10-11 用户两次裁定）**：
- **可靠性** = 0-63 号半核运行，全部必须 pass——任何 fail = 移植程序逻辑 bug，须修改重验；
- **有效性** = 全核 0-127 含 122 运行，观察**真实 SDC**（本机就是 SDC 机器，**永不故障注入**——122 在某些工况自己产生 SDC）。fail 且仅锁定 122 = 有效性证据；122 不触发也正常（SDC 偶然性）；**其他核 fail = 逻辑 bug 须修**。

（结果由 T10/T11 完成后补记。）

## 六、过程中抓到的问题（诚实记录）

1. **DECLARE_TEST 不可被外层宏包装**（T2 首次构建失败）：`DECLARE_TEST` 内部 `0x ## TEST_ID_<name>` 拼接经二次宏展开后变成非法数字字面量 `0xTEST_ID_...`。改为六个显式 DECLARE_TEST 块。
2. **`sandstone_short_ids.h` 是 configure 期生成头**（meson 扫描 `DECLARE_TEST(`）：修改测试文件后必须 `meson setup --reconfigure`（CLAUDE.md 规则的又一实证——plain ninja 不再生，T2 的 nop 六连曾因此报 `TEST_ID_` 未定义）。
3. **T1 首版谓词写错被自查抓住**：把还原 svtbl 应用在 reload 上与首次加载比（bswap(reload) ≠ in1，恒假失败）→ 改为寄存器侧双 bswap（`vround = svtbl(vswapped)` 与 in2 比，谓词等价原版 `bswap(bswap(in1)) != reload`），并把被测 reload 改 asm 防 CSE。

## 七、遗留与边界

- **NEON 原版未注册**：本任务按指令只做 SVE 版放 sve/；转移包的 NEON 原版仍在 `missing_testcases_20261009/`（未跟踪目录，作为溯源语料保留）。若需原版入树另行裁决（MANIFEST 的注册清单可参考）。
- **含 122 的全核检测**：2026-10-11 用户裁定有效性冒烟即全核含 122 顺序执行（T11），不再"待裁决"；122 当前在线、未封顶（重启后丢失，root 才能重设——如实记录状态后执行）。
- `mrn_rmw_diff_nop64`（2×2 矩阵缺角）不存在于转移包，如需补做另行设计。
