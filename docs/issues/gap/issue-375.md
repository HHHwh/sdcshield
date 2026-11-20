# Issue #375 解决方案文档：聚合/容差校验族 KAT 锚缺失与检出盲区

- Issue：[#375](https://github.com/wangxumarshall/sdcshield/issues/375)（2026-11-17 立案）
- 本文核查日期：2026-11-18（核查基线 = main `727cc8d3`；行号为当日实查）

## 一、结论先行

三缺口现状分裂：**缺口 A（isal_crc 族 10 文件无 KAT 锚）原样未动，是本 issue 最大剩余面**；
缺口 B 六行中 power_virus_dit 一行已修（随 #376 分段检查点落地）、其余五行仍在但均属
「声明检出界」级别；缺口 C 四行中两行已修（acl_gemm_sve 转 byte-exact、stencil_axis 分段
+ 容差收紧 1e-4→1e-11）、一行报废（eigen_svd_bidiag_sve 已退役、文件不存在）、一行在
x86 参考架构不动域（acl_gemm.cpp）。

## 二、现状核查（逐项实测）

### 缺口 A：isal_crc 族（10 文件）——未动，原样

`tests/cpu/crc/isal_crc64_ecma182_refl.cpp:31-47` 当前形态（10 个 `isal_*.cpp` 同构，
实测 `grep -l` 全数命中）：

```c
uint64_t crc1 = crc64_ecma_refl(0, local_data.data(), BLOCK_SIZE);      // :31 加速核
uint64_t crc2 = crc64_ecma_refl_base(0, local_data.data(), BLOCK_SIZE); // :36 ISA-L 可移植 C
bool data_ok = (crc1 == crc2);                                          // :39 双实现交叉
uint64_t store_buf = crc1; /* memcpy reload */ bool consistent = ...;   // :41-44 自拷贝
bool passed = data_ok && consistent;                                    // :47
```

三个缺口全部仍在：(1) 加速核与 `_base` 同出 vendored ISA-L（`third-party/isa-l/`，ARM64
侧为预构建 `crc64_ecma_refl_pmull.o`）——多项式表共模，表错则双实现同错、比对通过；
(2) 无 KAT（`grep -ln "123456789\|kat" tests/cpu/crc/isal_*.cpp` 零命中）；
(3) `consistent` 是栈上 8 字节自拷贝，无检出意义（L1 填充）。

树内先例（已核实存在、可直接抄）：

- `tests/cpu/crc/zpclmul.cpp:55-72`：`kat_selfcheck()`——标准向量 `"123456789"` →
  `0xCBF43926`（CRC-32/ISO-HDLC）+ 135 字节尾折路径向量，run 循环前 fail-closed 门禁
  （`:86-88`）；`zpclmul_rep.cpp:62` 同款。
- `tests/cpu/arm64/crypto_test.cpp:152-178`：FIPS-197 NIST 向量锚定 init golden，不符即
  `return EXIT_FAILURE`（"refusing to run a broken SDC detector"）。

### 缺口 B：聚合族六行——一行已修、五行待声明

| 测试 | 当前状态 | 位置（当前行号） |
|---|---|---|
| ooo_dep_chain_arm（+_sve） | 未动。4 条整数链各自 `acc != golden_*` 精确比对（逐链独立、整数无舍入），补偿盲区不存在于链内；盲区仅在「链终点才比」——中途翻转若被后续整数运算湮灭（如乘 0/溢出回绕）不可检 | `tests/cpu/arm64/ooo_dep_chain_arm.cpp:224-249`、`tests/cpu/sve/mem/ooo_dep_chain_sve.cpp:268-293` |
| mite | 未动。整数组 sum 聚合，单 bit 可检、补偿性多 bit 不可检 | `tests/cpu/misc/mite.cpp:136-143`（`checksum += val` → `!= golden_checksum`） |
| neon_rot_..._k2pureload | 部分演进：golden 已是标量独立 fold（#373 修复，XOR 序无关），但仍是 4 个聚合 XOR 终值——补偿性双 lane 错误精确抵消不可检 | `tests/cpu/misc/neon_rot_ldr_at_top_rowmajor_k2pureload.cpp:126-146` |
| power_virus_dit（+_sve 两版） | **已修**（随 #376）：2048 步 burst 改 32 个 SEG_LEN=64 边界分段字节比对，中途翻转在仍 supra-ulp 时被捕获 | `tests/cpu/arm64/power_virus_dit.cpp:115-196`；`power_virus_dit_sve_arm.cpp:129-184`（fma2 目录的 `power_virus_dit_sve` 仍终态-only，归 #376 跟踪，本卡不重复） |
| arm64_sdc（+_sve） | 未动。CRC32(`__crc32cb`)/CRC64/加法 checksum 三聚合 golden 各自 `memcmp_or_fail`；已有 1-bit 注入自检，加法成员有补偿盲区（CRC 两成员可交叉覆盖多数补偿态） | `tests/cpu/arm64/sdc_test.cpp:172-194` |

### 缺口 C：容差吞单 bit——两修一废一不动

| 测试 | 当前状态 | 位置 |
|---|---|---|
| fma_sve | **未动，仍是最刺眼一行**：`approx_equal` 1e-6 相对 + 1e-7 绝对下限（`:21-29`），run 期 `data_ok = approx_equal(result, ref)`（`:84`）；另带同款自拷贝 `consistent`（`:88-92`）。同目录 fma2 其余（neon_add_sve、fsu_byteexact_sve 等）均 byte-exact | `tests/cpu/sve/fma2/fma_sve.cpp:21-29,84` |
| eigen_svd_bidiag_sve | **已退役**：文件不存在（`find tests -name "eigen_svd_bidiag*"` 零命中，2026-11-02 退役，CLAUDE.md 有档）。该行报废 | —— |
| acl_gemm_sve | **已修**：1e-4 容差已除，改为逐 16 行块 `memcmp` 字节精确比对 naive_gemm golden（`a[i]` 有限值清洗后 golden 确定性成立），失配走逐元素位级证据 cold path | `tests/cpu/sve/arithmetic/acl_gemm_sve.cpp:98,162` |
| sve512_stencil_axis_arm 形态 B | **已修**（随 #376）：容差收紧 `kTolRel = 1e-11`（`:132`）+ 128 个 kRandSeg=64 边界检查点，边界容差按迭代数比例化（早边界更紧），末边界复现旧终态口径 | `tests/cpu/arm64/sve512_stencil_axis_arm.cpp:127-165,462-496` |
| （参照行）acl_gemm.cpp | x86-64 参考架构，1e-4f 绝对容差有完整头注论证（long-double golden 求和序差 → 绝对界 64×1.2e-7≈7.7e-6，取 10× 余量）。**rule 5 不动域**，仅作对照 | `tests/cpu/arithmetic_arm/acl_gemm.cpp:140-152` |

## 三、根因

1. **缺口 A**：`tests/cpu/crc/isal_*.cpp`（10 文件，run 期交叉比对在各自 `:31-47` 区段）
   ——校验语义只有「同源双实现交叉」，缺独立于 ISA-L 的外部标准答案锚；共模表错与
   自拷贝检查均无检出力。
2. **缺口 B**：上表五行——聚合校验（sum/XOR/终点值）对补偿性多 bit 错误数学上不可检，
   属校验语义的固有边界而非实现 bug；power_virus 行已消。
3. **缺口 C**：`tests/cpu/sve/fma2/fma_sve.cpp:21-29` ——相对容差下界远大于单 bit 翻转
   引起的相对误差（f32 尾数 23 bit，末位翻转 ≈2⁻²³ 相对量级 << 1e-6），单 bit SDC
   必然通过。

## 四、建议修复路径

### 缺口 A（优先做，收益/成本比最高）

对 10 个 `isal_*.cpp` 统一加 KAT 门禁（抄 `zpclmul.cpp:55-72` 形态，init 或 run 首轮
fail-closed）：

1. 每文件内嵌 1–2 条标准检查串的规范 CRC 值（逐字节十六进制常量，**不经过 ISA-L 计算**）：
   - CRC-64/ECMA-182（refl/norm）、CRC-64/ISO、CRC-64/XZ、CRC-64/Jones 对 `"123456789"`
     的 check 值（取自 CRC Catalogue `reveng` 权威表，如 ECMA-182 refl = `0x755309...`，
     落地时以目录值为准并注明出处）；iscsi/t10dif/gzip(CRC-32) 同理。
   - 追加一条 >BLOCK_SIZE 长度向量覆盖 `_base` 的分块路径（zpclmul 的 135 字节先例）。
2. 门禁不符 → `report_fail_msg` 硬 FAIL（"golden/KAT mismatch — detector itself is
   broken"，crypto_test 先例），不是 SKIP。
3. 顺手项：`consistent` 自拷贝检查改名或删除（sse_sym #324 先例：恒真检查改名去误导），
   至少在注释声明其 L1 填充定位、非数据校验。
4. 单文件一个 commit（rule 1），10 个文件顺序做。

### 缺口 B（声明为主）

1. 上表未动五行在各自校验处注释显式声明检出界：**「单 bit / 单 lane 错误可检；
   补偿性多元素错误（和保持/XOR 保持）不可检」**（issue 验收方向原话，机械可查）。
2. arm64_sdc 的三探测器互补性写进注释（CRC32 Castagnoli + CRC64 多项式 + 加法checksum
   三域正交，补偿态需同时骗过三者）。
3. 不建议为这五行重构 per-element golden：整数链/聚合是其压力形态的一部分（ooo_dep 的
   长依赖链本身就是被测对象），逐块化会改压力形态——与 issue「不能逐块的显式声明」
   的裁量一致。

### 缺口 C（fma_sve 单点）

1. **整数操作数化**（CLAUDE.md FMMLA 先例）：a/b/c 取整数域 f32（|x|≤2²⁴ 内整数），
   `svmla` 与 `fmaf` 同为单次舍入且操作数精确可表示 → 结果 bit 确定性，`approx_equal`
   换 `memcmp`（同目录 fsu_byteexact_sve/neon_add_sve 先例）。操作数从框架 RNG 取整
   数域，保持 -s 可复现。
2. 若保持随机浮点操作数（压力形态考量），退而求其次：容差收不下单 bit（1e-6 → 任何
   值都 ≥ 数个 ulp），必须走整数化，无第三路——如实写明。
3. `consistent` 自拷贝检查同缺口 A 第 3 条处置。

## 五、验证边界

1. **KAT 注入自检（issue 验收方向 1）**：对任一 isal 文件人为单字节损坏多项式表常量
   （或 monkeypatch KAT 期望值），`-e isal_crc64_ecma182_refl -t 2000 -n 1` 必须 FAIL
   ——证明 KAT 锚有 teeth；恢复后同命令 `exit: pass`。
2. **fma_sve**：整数化后 `./builddir/sdcshield -e fma_sve -t 2000 -n 1` → `exit: pass`
   （issue 验收方向 2），且断言无容差路径残留（`grep approx_equal` 零命中）。
3. **检出界声明**：`grep -rn "补偿性" tests/...` 机械可查（issue 验收方向 3）。
4. **回归红线**：每文件改动后 `-e zstd19 -t 2000 -n 1` → pass；`./scripts/build.sh`
   零新告警。
5. **x86 非回归**：isal/fma_sve 均在 ARM64 路径（`#ifdef __aarch64__` 或 ARM64 专属目录），
   x86-64 侧靠检视；acl_gemm.cpp（x86）不动。
6. 本机边界：以上全部可在本机（SVE-256）真实执行，无 gem5 依赖（无 VL 档位诉求）；
   CRC check 值取自外部权威目录（reveng CRC Catalogue），落地时逐值核对出处并写进
   注释——这是「独立于 ISA-L 的锚」的成立前提。
