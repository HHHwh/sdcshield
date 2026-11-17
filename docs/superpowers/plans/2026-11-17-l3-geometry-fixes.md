# L3 几何修正系列实施计划（2026-11-17）

> 背景：L3 审计（本会话）判定 14 个 L3/LLC 用例中若干个在鲲鹏 920
> 分布式 L3（24MB/24 核簇，sysfs+框架双源实测）上参数/描述失真。
> 用户裁定：立即修改并推送 GitHub 通报其他机器。

**Goal**: 5 个修正单元，逐一验证提交推送至 `feat/migrate-opendcdiag-tests`。

## 单元清单

1. **fix(arm64) llc_randomized_load + llc_uniform_load**：32MiB/线程
   硬编码窗 → `device_info` 探测的簇 L3 尺寸（照 memcpy_l3_cache_size
   的 get_l3_size 三步探测模式，init 期探测经 test->data 传递）。
   容量边界工作集语义入注释。真代码改动。
2. **fix(arm64) memcpy_l3_cache_size**：注释"本机实测 56MB" → 24MB/簇
   （异板陈旧记录，本机框架 cache-info 实证 24MB）。
3. **fix(arm64) l3_execute**：描述与头注释诚实化——ic ivau 重取指由
   L2（PoU）供数，非 L3；保留测试名（注册表稳定），只修描述。
4. **docs(arm64) mesh write_only ×3**：4MB/线程私有窗的 L3 驻留并发
   边界注记（簇内 ≤6 线程驻留；全簇 24×4MB=96MB 流式）。
5. **docs(arm64) llcstress**：检测面注记——64MB 窗压 L3 替换真实，
   但校验对走 store-buffer/L1 转发，L3 驻留数据完整性由 mesh read_L3
   族承担。

## 验证口径

- 单元 1（代码）：构建零新警告 + 两用例实跑 exit:pass + zstd19 回归。
- 单元 2-5（注释/描述）：构建零新警告 + `-l` 新描述在场。
- x86-64 untouched：全部文件在 aarch64 门控列表（memcpy 为注释改动）。

## 审计事实基线（改动依据）

- L1D 64KB/核，L2 512KB/核私有，L3 24MB/簇共享 24 核（8 簇分布式）
- 框架 cache-info：L3 unified 25165824B，breakdown {24MB, cpu0, ×24}
- mesh 族 4MB 共享数组 ∈ (L2, 簇 L3) → 几何正确不改
- 32MiB/线程 vs 每线程有效份额 1MB → 32× 过订阅 = DRAM 流式化

## 单元 6（2026-11-17 追加）：L3_cache_920 精选目录

用户指令：复审八件套代码逻辑，能真检测本机 L3 者收入新目录
`tests/cpu/L3_cache_920/`。终裁 **1/8 合格**（memcpy_l3_cache_size：
L3 尺度窗 + memcmp 全量重读校验），连带其 NEON 孪生
（memcpy_l3_cache_size_neon，同构合格，含 arm_add 相对包含修复与
56MB 勘误）。收录判据：工作集 L3 尺度 + 校验点经存储层级重读比对。
其余 7 件落选原因入目录 README（README 同时登记 mesh read 族六个
未随迁的合格者）。SVE 孪生不随迁（属 tests/cpu/sve 组织）。
