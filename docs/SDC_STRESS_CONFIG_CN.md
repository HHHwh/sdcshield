# SDC 压力统一配置系统使用说明

> 位置：`framework/sdc_stress_config.h`（接口）+ `framework/sandstone_opts.cpp`（配置文件加载入口）
> 适用：sdcshield 全部已完成可配置化改造的 FP/向量/矩阵/加密业务用例

## 1. 设计目标

把 eigen / gather / fisttp / SVE FP / mesh / openssl / ipsec / openblas /
pocketfft / sleef 等目录用例的操作数尺寸从编译期硬编码改为运行时可配置，
使得**同一二进制**可以在不同内存/CPU 规格的机器上自动施加匹配环境的
测试压力，且无需重新编译即可调整。

## 2. 配置优先级链（高 → 低）

| 优先级 | 通道 | 示例 |
|---|---|---|
| 1 | 命令行 `-O` | `-O eigen_gemm_double_dynamic_square.mdim=1024` |
| 2 | 配置文件 | `$SDC_STRESS_CONFIG` 指定路径；缺省 `./sdc_stress.conf` |
| 3 | 环境变量 | `SDC_EIGEN_GEMM_DOUBLE_DYNAMIC_SQUARE_MDIM=1024` |
| 4 | 内置默认 | 多数尺寸类参数为 **0 = 环境自适应**（按物理内存/CPU 自动计算） |

- 命令行 `-O` 在配置文件之后解析，天然覆盖同名条目。
- 环境变量命名规则：`SDC_<TESTID>_<KNOB>`，全部大写，testid 中的 `.`
  与 `-` 均转为 `_`。
- 个别旧脚本专用变量（如 `SDC_MESH_BUFFER_BYTES`、`GEMM_K_DIM`）保留兼容。

## 3. 配置文件格式

`KEY=VALUE` 每行一条，名字空间与命令行 `-O` 完全一致（即
`testid.knob=value`）；`#` 之后为注释；空行忽略。格式错误在启动阶段以
`EX_USAGE` 退出，不会带病运行。`=` 两侧允许空格（`KEY = VALUE` 与
`KEY=VALUE` 等价）。

```ini
# sdc_stress.conf 样例
fisttp_arm.num_elements=1048576
openssl_sha.arena_bytes=1073741824
openssl_sm3sm4.data_size=65536
mesh_upi_sve_sym.buffer_bytes=8589934592
```

指定路径：`SDC_STRESS_CONFIG=/path/to/sdc_stress.conf sdcshield ...`

**分环境推荐配置（直接可用）**：`docs/configs/` 下提供三类典型规格的
预计算推荐值文件（值由本文 §5 模型对 8GB/8C、32GB/32C、128GB/64C
直接计算）：

- `sdc_stress_8GB_8C.conf.example`
- `sdc_stress_32GB_32C.conf.example`
- `sdc_stress_128GB_64C.conf.example`

用法：`SDC_STRESS_CONFIG=<文件> ./sdcshield ... -v`。注意：**未列出的
用例留空即走内存自适应**——推荐文件只是显式基线；核数与文件假设不同时，
count/nelems/arena 类按 1/线程数、mdim 类按 1/√线程数 换算，或删除条目
交回自适应。

## 4. 参数校验

所有尺寸类旋钮经 `sdc_knob_int_clamped()` 读取；环境变量通道经
`sdc_env_knob_int_clamped()` 读取（与 `-O`/配置文件完全一致的
clamp+告警语义）：

- **越界**（任一通道）：报警告并 **clamp 到安全边界**（不中断战役），例：
  `fisttp_arm.num_elements=1` → 警告 + 实际取 1024；
  `SDC_SLEEF_NEON_NELEMS=99999999999` → 警告 + 实际取 67108864。
- **格式错误**（配置文件行非法）：启动即退出（`EX_USAGE`）。
- **分组密码对齐**：SM4/AES-CBC 等无填充场景的 data_size 自动向下
  对齐到 16 字节，对齐后小于下限则取下限。
- **倍数约束**：SVE 向量访问类缓冲（sleef 的 4 倍数、mrn 族 8 倍数、
  movbe 族 16 倍数、neon_rot 槽位族的 svcntd()/VL 倍数）在显式配置
  非整倍值时告警并向下取整，保持对齐语义。

### 4.1 运行时压力映射日志（实际跑时确认打满/越界）

运行加 `-v`，每个用例 init 阶段输出压力映射日志（全部用例统一走
`sdc_stress_config.h` 咽喉点，无需逐用例打开）：

- `[压力映射] <testid>.<knob> = <值> (来源: ...)` —— 每个尺寸旋钮的
  **最终生效值与来源**（命令行/配置文件、环境变量、内存自适应、工作集
  自适应四选一）；自适应来源的行同时给出 **预期合计 ~X GiB = 物理内存
  的 Y%**，把该用例全部线程的预期总内存直接算给你看。
- `[欠压力]` —— 自适应计算结果触到安全上限（hi），实际内存只能达到
  预算的百分之几十；大内存+多线程机器上 fisttp/gather 等上限较低的
  用例会出现，属已知安全收口。
- `[内存越界]` —— 安全下限（lo）使预期合计超出预算，即"实际压力会
  超过设定的内存比例"——线程过多或内存过小时出现，是映射阶段的越界
  预警（区别于越界**配置**被 clamp 的「越界」告警）。

确认方法：`-v` 运行后 `grep '压力映射'` 拿到各用例预期合计，与
`top` 的 RES（或 `/proc/<pid>/status` VmHWM）对比，一致即按预期打满
内存；CPU 打满由 `-n` 线程数决定（日志中的「N 线程」即预期占核数），
用 `top` 的 `%Cpu(s)` 确认。core179 诊断探针未配置时按设计几何运行
（不追求打满），属预期行为。

## 5. 压力映射模型（参数 → 实际内存负载）

### 5.1 方阵类（eigen/SVD/GEMM 等）：`sdc_auto_square_dim`

```
N = sqrt( 物理内存 × mem_fraction
          / (bytes_per_elem × matrices_per_thread × rss_ratio × nthreads) )
```

### 5.1b 线性缓冲类（gather/fisttp/ipsec/sm3sm4）：`sdc_auto_linear_count`

```
n = 物理内存 × mem_fraction / (bytes_per_elem × arrays_per_thread × rss_ratio × nthreads)
```

> 注意：线性缓冲**必须**用本函数；误用 5.1 的 sqrt 模型会把压力欠缩放 √n 倍
> （128 线程下 fisttp 仅 ~187KB/线程而非预算的 60% 内存）。

- `bytes_per_elem`：double=8、float=4、complex<double>=16 …
- `matrices_per_thread`：每线程同时驻留的矩阵/缓冲数（含 golden/scratch）
- `rss_ratio`：算法峰值 RSS 相对矩阵体积的倍数（GEMM≈6，SVD≈12）
- `mem_fraction`：本用例允许占用的物理内存比例（多数 60%~90%）
- 结果 clamp 到各用例标定的 `[lo, hi]` 安全区间

验证数据（fisttp_arm，4 线程，40 循环）：1024→60ms，65536→93ms，
524288→477ms —— 压力随配置单调放大；自适应模式下 1 线程的自动尺寸
约为 4 线程的 2 倍（n ∝ 1/√threads）。

### 5.2 工作集字节类（mesh 家族）：`sdc_target_workset_bytes`

```
bytes = 物理内存 × mem_fraction / consumers
```

`consumers` 是同时运行的大内存用例数（如 2 路 mesh 战役），用于均摊。

### 5.3 arena 类（openssl sha/sha3）：`arena_bytes`

每线程 mmap 工作区 = 物理内存 × 60% / 线程数，clamp [1MiB, 4GiB]。
arena 越大，golden/被测元素的随机落点跨度越大（覆盖更多 cache set /
页 / THP 偏移）。

## 6. 各用例参数速查

### eigen 家族（eigen_gemm / eigen_svd / eigen_svd_jacobi / eigen_sparse）
| knob | 含义 | 范围 | 缺省 |
|---|---|---|---|
| `mdim` / `n` | 方阵维度 | 16~8192（按元素宽度不同） | 自适应 85% 内存 |
| （dump 变体）`mdim` | 同上，另兼容 `GEMM_K_DIM` 环境变量 | 16~4096 | 自适应 |

### gather 家族（10 个文件）
| knob | 含义 | 范围 | 缺省 |
|---|---|---|---|
| `data_size` | 每线程源/目标数组元素数 | 1024~16M | 自适应 60% 内存 |

### fisttp（fisttp_arm / fisttp_sve）
| knob | 含义 | 范围 | 缺省 |
|---|---|---|---|
| `num_elements` | 每线程转换表元素数 | 1024~16M | 自适应 60% 内存 |

### SVE mesh（tests/cpu/sve/mesh 18 个文件）
| knob | 含义 | 范围 | 缺省 |
|---|---|---|---|
| `buffer_bytes` | 共享缓冲目标字节 | 1MiB~4TiB | 物理内存 90% ÷ 2 |

兼容旧变量 `SDC_MESH_BUFFER_BYTES`（直接指定字节）。

### SVE eigen / acl_gemm
| knob | 含义 | 范围 | 缺省 |
|---|---|---|---|
| `eigen_svd_bidiag_sve.mdim` | 双对角化方阵维度 | 64~2048 | 自适应 65% |
| `acl_gemm_sve.dim` | F32 GEMM 方阵维度 | 64~4096 | 自适应 60% |

### SVE ipsec（24 个文件，统一）
| knob | 含义 | 范围 | 缺省 |
|---|---|---|---|
| `data_size` | 明文/密文字节（16B 对齐） | 1024~16M | 自适应 60% 内存 |

### openssl
| knob | 含义 | 范围 | 缺省 |
|---|---|---|---|
| `openssl_sha.arena_bytes` | 每线程工作 arena 字节 | 1MiB~4GiB | 物理内存 60%÷线程 |
| `openssl_sha3.arena_bytes` | 同上 | 1MiB~4GiB | 同上 |
| `openssl_sm3sm4.data_size` | SM4/SM3 数据字节（16B 对齐） | 1024~16M | 自适应 60% |

### openblas_gemm（5 个文件，已迁移统一链）
| knob | 含义 | 范围 | 缺省 |
|---|---|---|---|
| `mdim`（dgemm/sgemm/cgemm/zgemm） | 方阵维度 | 16~16384 | 自适应 85% 内存（rss 6.0，封顶 8192） |
| `transab` | 0=NN 1=NT 2=TN 3=TT | 0~3 | 0 |
| `beta_permille` | β 千分比 | 0~1000000 | 0 |
| `n`（lu） | 分解维度 | 16~16384 | 自适应（封顶 8192） |

> 旧版越界致命失败已改为告警+clamp；旧 knob 名不变（脚本兼容）。
> 自适应默认按 eigen GEMM 同款 rss 6.0 模型，避免 init golden 生成
> （O(n³)）在大内存机器上耗时过长。

### sleef（sleef_neon / sleef_sve，已迁移统一链）
| knob | 含义 | 范围 | 缺省 |
|---|---|---|---|
| `nelems` | 每 family 元素数（4 的倍数） | 128~64M | 自适应 60% 内存 |

### SVE mem（6 个文件）
| knob | 含义 | 范围 | 缺省 |
|---|---|---|---|
| `agu_stress_2src_sve.count` / `agu_stress_store_only_sve.count` | 每缓冲 u64 元素数 | 1024~32M | 自适应 60% |
| `arm64_sdc_sve.buffer_bytes` | 扫描缓冲字节 | 64KiB~1TiB | 物理内存 60%÷2 |
| `mem_disambig_alu_sve.buffer_bytes` | 消歧地址跨度字节 | 4096~1TiB | 物理内存 60%÷2 |
| `mmu_split_tlb_sve.pressure_pages` | 每线程 mmap 压力页数 | 8192~1M | 自适应按页折算 |
| `ooo_dep_chain_sve.chain_len` / `.rob_depth` | 依赖链节点数 / ROB 槽数 | 4096~16M / 2048~4M | 自适应 60% |

### SVE 顶层 sve_rot（5 个文件）
| knob | 含义 | 范围 | 缺省 |
|---|---|---|---|
| `slots`（5 个变体各按 testid） | 行缓冲槽位（槽距/模式不动） | 1024~1M（k3res 4096~4M） | 自适应 60% |

### core179 诊断探针（mrn 族 16 + movbe 3 + mite + neon_rot 族 18）

**语义与业务用例不同**：这些是 SDC 激发/时序诊断探针，缓冲几何本身
是激发参数。**未配置时默认 = 原设计值（不自适应放大）**；仅显式
配置（`-O` / 配置文件 / `SDC_<TESTID>_<KNOB>`）时 clamp 生效。

| knob | 含义 | 范围 | 缺省 |
|---|---|---|---|
| `count`（mrn 族 / movbe_sve / movbe_l1d_vs_l2 / mite / neon_rot_2src / store_only） | u64 元素数 | 原值~1<<22 | 原设计值 |
| `buffer_bytes`（movbe_slf_sweep / neon_rot_u8x16） | 缓冲字节 | 64KiB~256MiB | 原设计值 |
| `slots`（neon_rot_ldr_at_top 族 16 文件） | 槽位数 | 1024~1M（k3res 4096~4M） | 原设计值 |
| `base_count`（mrn_rmw_diff_footprint） | 基准实例元素数（第二实例恒为 ×16） | 8192~1<<22 | 8192 |

### fma2 / kreg(arm0102) / arithmetic 族（22 个文件）
| knob | 含义 | 范围 | 缺省 |
|---|---|---|---|
| `count`（movdq2q/movmskpspd/movq2dq/neon_add/adcx 族/adox 族/adcxlong/arm0102 族/crt_builtins） | 元素数 | 1024(或原值)~64M | 自适应 60% |
| `limbs`（gmp_bigadd_sve） | 2048-bit 大数 limb 数 | 256~1M | 自适应 60% |
| `steps`（iex_operand_combo_sve） | 串行 ALU 链步数 | 1024~1M | 自适应 60% |
| `num_pairs`（operand_space_sve） | 操作数对数 | 80~1M | 自适应 60% |
| `scale`（power_virus_dit_sve，1:1 成对缩放保占空比） | BURST/STALL 内层倍率 | 1~256 | 1 |

### pocketfft（保持原状，见第 9 节 B 类说明）
| knob | 含义 | 范围 | 缺省 |
|---|---|---|---|
| `n` | FFT 长度（白名单：512..16384 pow2、4099/8191 Bluestein、6144/10000 混合基数） | 白名单 | 4096 |

### 原生支持（无需改造，已有 runtime 旋钮）
- `mesh`（tests/cpu/mesh NEON 家族）：`SDC_MESH_BUFFER_BYTES`

## 7. 调整建议

| 场景 | 建议 |
|---|---|
| 常规回归 | 全部走缺省自适应（不传任何参数），各用例按内存比例自动分摊 |
| 内存通路重点激发 | mesh `buffer_bytes` 提到物理内存 90%；eigen `mdim` 显式给上限区间值 |
| 快速功能验证 | `fisttp_arm.num_elements=1024`、`*.data_size=1024`、`*.mdim=64` |
| 多用例同跑 | 各用例 mem_fraction 之和 ≤ 900‰（如 3 个用例各给 300） |
| 小内存机器 | 自适应模型自动缩小尺寸；显式配置时注意 clamp 上限即安全上限 |
| 复现历史行为 | `*.data_size=1024`、`mdim=256/128/221`、`arena_bytes=4194304` 等历史硬编码值 |

## 8. 用例侧接入新测试的方法

```cpp
#include "sdc_stress_config.h"

static int mytest_dim(struct test *test)
{
    int64_t v = sdc_knob_int_clamped(test, "mdim", 0, 16, 8192);
    if (v == 0)                                         /* 未配置 → 环境变量兜底 */
        sdc_env_knob_int_clamped("mytest", "mdim", &v, 16, 8192);
    if (v > 0)
        return (int)v;
    /* bytes_per_elem=8, matrices_per_thread=4, rss_ratio=6, 85% 内存 */
    return sdc_auto_square_dim(8, 4, 6.0, 850, thread_count(), 16, 8192);
}
```

在 `init()` 中调用一次并缓存（存入 `test->data`），`run()` 从缓存取，
避免循环内重复解析。

## 9. 保持固定的用例（B 类清单，逐文件审计结论）

以下用例的尺寸常量**有意保持固定**，不纳入可配置化（硬编码即正确性
语义，强行旋钮化会破坏测试本体）：

| 类别 | 文件 | 固定理由 |
|---|---|---|
| 掩码/宽度语义 | sve/kreg 的 kreg2/3/5/6/8/9、sve/vector2 全部 5 个 | KSHIFTL/KORTEST/VPMOVM2B 等 AVX-512 移植的**位宽语义**，宽度与掩码类型耦合 |
| FP 特殊值 golden | fma2/fpu_special_values_sve、fsu_byteexact_sve | IEEE-754 特殊值笛卡尔积 golden 表 + 字节精确链，表内容即测试本体 |
| 指令位宽移植语义 | sve/fma 族 10 个（VECTOR_SIZE=4/8/16） | VECTOR_SIZE 与 AVX2/AVX-512 移植目标的 lane 数绑定，改值即换指令语义 |
| dump 探针契约 | core179 的 movbe_dump_* 与 movbe_dump_probe_* 12 个 | dump 窗口/槽距（如 g1 的 256KB LLC set 攻击几何）是离线分析契约 |
| 缓存几何探针 | mem/l2c_cross_cache_line_sve、lsu_store_forward_sve、partial_store_forwarding_sve | 探测窗口 = 2×L2/128B 转发窗等缓存几何，运行时自适应或按设计固定 |
| 算法白名单 | pocketfft 的 n | FFT 长度白名单是算法谱系（pow2/Bluestein/混合基数），不适用区间 clamp 模型 |
| 操作数位宽 | arithmetic/gmp_bignum_sve、bigint_mulx_sve | 512-bit 乘法器语义位宽即测试名（memory 极小无压力意义） |
| x86 参考架构 | tests/cpu/fma、vector、mesh（AVX512/AVX2/SSE） | 仓库规则：x86-64 参考架构不动 |

## 10. 多环境验证记录（2026-10-15，Kunpeng 920 / 64C / 376GB）

- **构建**：`ninja -C builddir` 全量 163/163 目标零告警零错误。
- **回归**：`zstd19 -t 2000 -n 1` → pass；sleef_neon/openblas_dgemm
  显式 knob 与自适应默认均 pass。
- **三通道**：`-O`、配置文件（`SDC_STRESS_CONFIG`）、环境变量
  （`SDC_<TESTID>_<KNOB>`）逐通道验证生效且优先级正确（-O 覆盖配置文件）。
- **校验**：越界值（nelems=99999999999）在 -O 与环境变量两通道均
  告警+clamp；坏配置文件（无 '=' 行）以 `EX_USAGE`（64）退出。
- **压力映射**：sleef_neon nelems 1024→4194304（4096×）实测
  CPU 时间 0.87s→18.1s（≈20.9×），单调放大成立；fisttp_arm 既有
  数据见 5.1b。
- **自适应**：openblas_dgemm 未配置时自适应 mdim（85% 内存 rss6.0
  模型，本机封顶 8192）通过；sleef_neon 自适应（60%÷线程）通过。
- **SVE 用例**：本机是否有 SVE **须现场探测**（CLAUDE.md 第 2 条）。曾记录的那台
  无 SVE 开发机上，全部 SVE 用例运行时 log_skip 优雅跳过（编译全通过）；在
  **有** SVE 的板子上这些用例会真跑，压测数据必须取真实 run 而非 skip。
  SVE 硅片上的自适应行为由模型单元公式保证（sysconf 物理内存/在线 CPU 与
  arch 无关）。
- **模型级多规格验证**（直接调用 sdc_stress_config.h 自适应函数，
  1~192 线程扫描，376GB 主机）：
  - 线性类（sleef 语义，600‰）：总预算恒定 225.8GiB（=物理内存
    60%），16~192 线程均摊后逐线程 nelems 精确 ∝ 1/threads
    （未触顶区 4 线程/1 线程实测比 0.250）；小线程数触 64M hi
    clamp 正确收口。
  - 方阵类（eigen GEMM 语义，850‰）：总预算恒定 319.8GiB（=85%），
    未触顶区间 mdim 每倍线程 ×0.707（=1/√2 精确）；1~16 线程触
    8192 hi clamp。
  - 工作集类（mesh 语义，900‰÷2）：169.3GiB 精确 = 90%÷2。
  - 结论：任何内存/线程规格组合下，三类模型均把实际内存压力
    精确锚定到配置的物理内存比例；内存规格维度由公式
    （线性 n ∝ mem、方阵 N ∝ √mem）与上述恒定预算共同覆盖，
    CPU 规格维度由 1/threads、1/√threads 缩放律实测覆盖。
