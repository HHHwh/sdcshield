# Issue #332 解决方案文档：mesh 族读阶段校验能力缺口（自拷贝零检出 + 求和聚合盲区）

- Issue：[#332](https://github.com/wangxumarshall/sdcshield/issues/332)（2026-11-17 立案，源自 PR #308 二轮检视意见 #9/#10，预先存在缺陷）
- 本文核查日期：2026-11-18（核查基线 = main `727cc8d3`；行号为当日实查）

## 一、结论先行

**缺口 A 的三孪生已修其二（avx_sym、sse_sym 均已落地「整数激励 + 双侧 sum」正解），
剩余 avx512_sym 一个孪生外加 sve_wide_sym 一个同类文件；缺口 B 的「低成本先行项」
（写侧立即读回硬 FAIL）已在 14 个文件落地，剩余 3 处丢弃点（含 1 处零检出的
write_only 用例）。** issue 提出的两条正解路线之一（整数化 + 双侧 sum）已被 avx_sym/
sse_sym/sve_sym 实证可行，剩余文件是同构移植，不是设计难题。

## 二、现状核查（逐文件实测）

### 缺口 A：f32 sym 族读阶段无 golden（自拷贝比较零检出）

issue 点名的三孪生 + SVE 侧同族，当前状态：

| 文件 | 读阶段校验 | dist() | 判定 |
|---|---|---|---|
| `tests/cpu/mesh/mesh_upi_avx_sym.cpp` | `read_sum == expected_sum`（`:342-351`）+ 写侧 CAS 累计 global_sum（`:303-304`） | 整数值 `(float)(int32_t)(...)`（`:234`） | **已修**（issue 正解路线 1） |
| `tests/cpu/mesh/mesh_upi_sse_sym.cpp` | 同上（`:241-273`，含 TOCTOU 快照修复 `:235-241`）；自拷贝检查改名 `store_ok` 并诚实声明（`:243-251`，#324） | 整数值（`:146`） | **已修** |
| `tests/cpu/sve/mesh/mesh_upi_sve_sym.cpp` | 同上（`:263-264`、CAS `:214`） | 整数值（`:172`） | **已修** |
| `tests/cpu/mesh/mesh_upi_avx512_sym.cpp` | **仅栈上自拷贝比较**（`:296-332`，`:303-334`，consistent 恒真）；`global_sum` 字段「未使用，但保留」（`:24`） | 非整数 `frandomf_scale(2000.0f) - 1000.0f`（`:214`） | **未修——issue 原样** |
| `tests/cpu/sve/mesh/mesh_upi_sve_wide_sym.cpp` | 同 avx512_sym：仅自拷贝（阶段 2，`:258` 起）；`(void)local_sum` 显式丢弃（`:234`），global_sum「未使用，但保留」（`:25`） | 非整数（`:156`） | **未修——issue 以为是已修**（issue 注「sve_sym/sve_wide_sym 有 global_sum 聚合校验」，实测 sve_wide_sym 没有：字段在、累计被 void） |

（int 求和族对照：sve_symm_int / avx512_symm_int / avx2_symm_int / sse_symm_int 四文件
均已有 `read_sum == expected_sum` 聚合校验——整数域无次序依赖，不属本缺口。）

issue 对 avx512_sym 读阶段的引用行号（立案时 `:160-190`、自注释 `:135-137`）已漂移至
`:296-332` 与 `:300-302`；文件自注释仍诚实承认「读阶段只做 store_buf 自一致性比较，
不对照写入值……校验能力缺口由 #332 立案重构」（`:300-302`）。

issue 论证的「为什么不能直接加 sum golden」仍成立且已被正解消除：非整数 float 的
double 求和不结合，写侧认领序 vs 读侧缓冲序的舍入差会引入次序依赖假 FAIL——已修
三文件的解法正是 issue 路线 1（整数值化后 |x|≤2×10³ 的整数在 double 求和下精确，
任意块序 `read_sum == expected_sum` 恒成立，`:232` 注释原话）。

### 缺口 B：写侧逐 lane 比对被丢弃 + 聚合校验补偿盲区

**先行项（写侧立即读回 → 硬 FAIL）落地面**：14 个文件已有
`immediate read-back mismatch → report_fail` 硬 FAIL 路径（avx512_sym `:247-275`、
sse_sym `:184-215`、sve_wide_sym `:218-232`、avx_sym、avx512_symm_f64、
avx512_asymm_write_int、avx512_asymm_write_only_int、sse_asymm_distrib_int、
avx512_asymm_distrib_write_int、sve_asymm_distrib_int、sve_wide_asymm_int、
sve_wide_asymm_write_int、sve_wide_asymm_distrib_int、sve_wide_asymm_distrib_write_int）。

**剩余丢弃点（3 处）**：

1. `tests/cpu/sve/mesh/mesh_upi_sve_asymm_int.cpp:196-201`——写侧 `svcmpeq` 逐 lane
   结果 `(void)cmp` 丢弃（注释「忽略，因为后续读者会校验」）；文件头 `:55` 有诚实
   声明。读侧唯一数据校验是全缓冲 `read_sum` 聚合（`:239`、`:255-256`）vs
   `expected_sum`——单 lane 单 bit 可检，补偿性双 lane 精确抵消不可检，失败后无逐
   lane 定位（cold path 只有 sum mismatch 的 golden/actual/xor 十六进制行 `:271-274`）。
2. `tests/cpu/mesh/mesh_upi_avx512_asymm_int.cpp:147-163`——写侧 `[[maybe_unused]]
   bool block_ok` 计算后丢弃（`:162` 注释「原代码发现不一致时仅记录，我们也同样处理」）。
3. `tests/cpu/sve/mesh/mesh_upi_sve_wide_asymm_write_only_int.cpp:174-186`——
   write_only 用例，block_ok 丢弃；文件头 `:41-42` 有 R6 勘误自认「结果被完全丢弃，
   无记录、无 FAIL 路径」。**该用例无读者，写侧回读是其唯一可能的内容校验**——丢弃
   即零内容校验（压力语义上 write_only 加逐块回读会引入读流量，见修复路径的权衡）。

## 三、根因

1. **缺口 A 剩余**：`mesh_upi_avx512_sym.cpp`（读阶段 `:296-332`）与
   `mesh_upi_sve_wide_sym.cpp`（阶段 2）——读阶段比较两臂消费同一份刚载入的数据
   （栈上 store_buf 自拷贝），对共享缓冲任何损坏零检出；且 `dist()` 非整数（`:214` /
   `:156`）阻断 sum golden 直加（次序依赖假 FAIL）。全局唯一逐字节校验只剩写线程
   自己的立即读回（同线程、紧随 own store、L1/L2 命中）——写后延迟出现或跨核路径
   损坏无字节级 golden 比对。
2. **缺口 B 剩余**：上述 3 处写侧比对结果丢弃；求和族聚合校验对补偿性多 lane 错误
   数学上不可检（固有边界，非实现 bug）。

## 四、建议修复路径

### 缺口 A（avx512_sym + sve_wide_sym，同构移植已验证解法）

1. 照抄 avx_sym/sse_sym/sve_sym 三文件已落地的四件套（一文件一 commit，rule 1）：
   - `dist()` 整数值化：`(float)(int32_t)(frandomf_scale(2000.0f) - 1000.0f)`
     （avx512_sym `:214` / sve_wide_sym `:156` 处改写）；
   - 写侧按块 `local_sum`（double）CAS 累计进 `global_sum`（字段已在，avx512_sym
     `:24` / sve_wide_sym `:25`，去掉「未使用」注释）；
   - 读侧全量重读累计 `read_sum`，**读循环前**快照 `expected_sum`（TOCTOU 先例
     sse_sym `:235-241`）；
   - `passed = (read_sum == expected_sum) && store_ok`，自拷贝检查改名 `store_ok`
     并保留诚实声明注释（#324 先例）。
2. 孪生同步注意：avx512_sym 与 avx_sym 头注互称孪生（`mesh_upi_avx512_sym.cpp:56-59`
   「孪生同步……改此函数必须双文件同步」）——本次改动恰好是把两孪生再次对齐，
   注释里的同步义务随修复解除。
3. **不选** per-block 可重算公式路线（issue 路线 2）：symm_f64 先例存在，但三文件
   已实证路线 1 更小且消除次序依赖，族内一致性优先。

### 缺口 B（3 处丢弃点 + 声明）

1. `sve_asymm_int` 与 `avx512_asymm_int` 的写侧比对升级硬 FAIL（14 文件同款
   `if (!block_ok) { fprintf ...; report_fail_msg(...); return EXIT_FAILURE; }`，
   avx512_sym `:264-275` 模板）。确定性论证同 issue：块为本线程独占写，写后立即
   回读失配 = 真实 store/load 损坏，零假阳性。
2. `sve_wide_asymm_write_only_int` 权衡后二选一，如实记录取舍：
   - 升级硬 FAIL：好路径零开销（比较结果本来就算了），坏路径多一次读——但
     write_only 的压力语义是纯写流，逐块回读已在写流中混入读（现状就有回读，只是
     丢弃结果——所以升级 FAIL **不改变**流量形态，纯增检出）；推荐此项；
   - 或删除回读比较与 block_ok（保持纯写流的字面语义），头注声明 write_only
     无内容校验、检出依赖同族 write_int/read_only 配对。现状「算了又丢」是最差的
     中间态（白付回读流量、零检出）。
3. 求和族聚合校验的检出界声明（issue 验收方向原话）：在 sve_asymm_int 读侧校验处
   （`:255-256` 附近）与同族文件注释「单 lane 错误可检、补偿性多 lane（和保持）
     不可检」——与 #375 缺口 B 的声明动作合并执行，勿重复立卡。

## 五、验证边界

1. **缺口 A 修复后本机可全验**（avx512_sym 是 NEON 实现，本机可跑；sve_wide_sym 需
   SVE——本机 no-SVE，按 gem5-fi 规则或 SVE 主机验证）：
   - `./builddir/sdcshield -e mesh_upi_avx512_sym -n 2 -t 5000` → `exit: pass`，
     yaml 日志含 `[压力映射]` sizing 行；整数值化后任意认领序 sum 恒等（无假 FAIL，
     `-s` 固定 seed 复跑 3 次结果一致）。
   - **检出能力证明（issue 验收方向）**：临时注入实验——init 后、读阶段前翻转缓冲
     一位（开发验证补丁，不进主干），读阶段 sum 失配必须 FAIL；撤销注入后 pass。
     注入补丁仅用于验证，不随修复 commit 进入主干。
2. **缺口 B 硬 FAIL 升级**：同上注入法对写侧验（翻转写后立即回读路径的一位）；
   回归 `-e zstd19 -t 2000 -n 1` → pass；`./scripts/build.sh` 零新告警。
3. **sve_wide_sym**：本机诚实 SKIP（`result: skip, 'test compiled with sve'`，本日
   实测同族输出）；按 CLAUDE.md rule 2 gem5-fi 规则在 `/home/sdc/wangxu/gem5-fi`
   验证（VL 档 128/256 至少两档，引真实 checksum 与 `SDCSHIELD_FAILS=0`），或在
   SVE-256 主机直跑。
4. **x86 非回归**：全部改动在 `#ifdef __aarch64__` 内（x86 占位 SKIP 路径不动，
   `mesh_upi_avx512_sym.cpp:415-424`），检视确认。
5. 诚实边界：本文未实现修复代码；「已修三文件」的判定基于源码精读（sum 校验 +
   整数 dist + 硬 FAIL 三要素齐备），其历史 PR 的 gem5 验收留档未复核。

## 关联

- 母卡 #334（已关）；#358（空过 PASS → 诚实 SKIP，已关）；#324（恒真检查改名，已关）；
  #325（sym 族 sizing/超时，已关）；#375 缺口 B（求和族检出界声明，并行卡——声明
  动作合并执行）；#357（等待耐心，同族不同机制）。
