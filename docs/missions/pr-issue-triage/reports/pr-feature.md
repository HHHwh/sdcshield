# pr-feature 角色报告 —— 3 个大特性 PR triage 与修复（2026-11-18）

**结论先行**：PR 518 / 814 / 695 三者全部从 **CONFLICTING 修复至 MERGEABLE**
（GitHub 实时确认），两轮/三轮 bot findings 全数处置（含同型未点名残留的
模式族收口），rule 2 自证完整（构建 + 受影响行为逐件实测 + 回归），全部
push 回各自 head 分支。未 merge、未 resolve thread、未改 `WIP:` 标题
（角色边界）。

验证环境：本机 Kunpeng-920（HiSilicon，无 SVE/SVE2/SME，96 逻辑核，gcc 10.3.1），
vendored 5 库 install/ 全在位。三 PR 的受影响行为在本机
全部可运行（518 是构建系统面、814 是 NEON/L3 面、695 是 Python 面），
无 gem5-fi 边界需求（详见各节）。
（身份勘误 2026-11-19：初版误写「firestorm（Apple M1 系）」——那是
`--dump-cpu-info` 按 part 0xd01 经 `simd-arm.conf:141` 的近似别名串
（`arch=firestorm → cortex_a76 # Apple M1 Firestorm-equivalent (approx)`），
非 MIDR 身份；本机真实身份为 Kunpeng-920，implementer 0x48，hostname
cathelper-k8s-worker2。验证结论不受影响。）

---

## PR 518 — feat/arm64-march-tiers-unify（march 档位统一）

**处置 commits**（push 至 `feat/arm64-march-tiers-unify`，ad22f79c..8367b293）：
1. `merge: main —— 第七档 march_arm64_sve2_compat 统一 eigen companion 字面量`

**Triage 发现**：分支上的 9 条 findings 已由此前 pr-loop 的 8 个 fix commits
处置完毕；真正阻塞是 **CONFLICTING**——main 两处演化撕裂本 PR：
- `docs/research/adding-tests.md`：main 31f6a99e（2026-11-17）删除了整个
  docs/research/ → 接受删除（该文件的修正随文件消亡，档位表唯一存活
  副本在 README）
- `tests/cpu/meson.build`：main 的 eigen `_sve2` companion（11-13）把
  本 PR 已变量化的区域重构成双目标循环（armv8.2-a+sve / armv8.2-a+sve2
  字面量）

**核心裁决——第七档 `march_arm64_sve2_compat`**：eigen companion 的 sve2 腿
不能复用 `march_arm64_sve2`（armv9-a+sve2+bf16+f32mm+f64mm）——armv9-a
基名需 GCC 11+，会重演 #661 的 20.03/22.03 gcc-10 镜像 Build 全红
（run 44011713221），且改变 40 库 2026-11-17 已 A/B 验证的编译产物。
新增根变量精确等于 `armv8.2-a+sve2`，值与 main 字面量逐字节相同。

**补齐的 findings 残留**：`sve512_fmmla_arm.cpp` 头注 doc 区三处 SVE2 残留
（ad22f79c 只修了 init 内注释）：L8「SVE2 FMMLA」→FMMLA（F32MM/F64MM 与
SVE2 正交）、L49 复合门控去 SVE2、L54-55 库名/march 误写更正
（tests_arm64_sve2→tests_arm64_sve_mm）。README/CLAUDE.md 档位表六档→
七档 + Eigen 特例段补双目标口径；plan 文档补 merge 后勘误（防按模板
重执行写回已删文件引用）。

**验证（rule 2，全部实测）**：
- 构建 954/954 干净，唯一警告 `stacksize.cpp _XOPEN_SOURCE redefined` 与
  main 基线构建**逐字节相同**（预存，非新增）
- **march 多重集 vs main 基线（同机同 flags 全量构建）逐字节 IDENTICAL**：
  `armv8.1-a+crc×3 / +crc+crypto×145 / armv8.2-a+sve×242 / +f32mm+f64mm×1 /
  armv8.2-a+sve2×40`——变量化零传参变化，等价性由多重集严格蕴含
  （同 flags→同二进制），故无运行时行为变化、无 gem5-fi 边界
- `--list-tests` 457 行与 main 基线 IDENTICAL；`-e zstd19 -t 2000 -n 1`
  → `exit: pass`；x86-64 检视（根定义惰性字符串 + 引用全在 aarch64 守卫内）
- 本机 gcc 10.3 不认 armv9-a 基名：march_arm64_sve2/sme 档按设计探测剔除
  （message 提示不阻断）——与 #661 Multi-OS Verify gcc-10 行为一致

---

## PR 814 — feat/migrate-opendcdiag-tests（opendcdiag 迁移，276 文件 +28k）

**处置 commits**（push 至 `feat/migrate-opendcdiag-tests`，a75b2855..cc82e1b5）：
1. `merge: main —— 目录统一裁决 + c51a739b knob 修复移植`
2. `fix(L3): 校验点全面收口 element-exact + 4 件 FP64 复制体退役 + T10/头注/原子化`
3. `docs(L3): 计数链同步实测 + 收录档案如实化`
4. `merge: 分支侧 docs 增量（adaptive 标定档案）融合`

**Triage 发现——三方演化冲突**：main 已两次吸收本 PR 早期快照（#472
squash 11-03 + 451c6272/418e4e09 neon 变体直合），双方对同一批文件演化出
两套目录组织（main: neon_memcpy_cache/neon_lock/cache vs PR: L3_cache_920
/arm_add/lock_arm），merge 后 **51 组同名测试双注册**。裁决采 PR 的最新
设计（L3_cache_920 统一目录是 11-18 user directive）：删除 main 侧重复件
（neon_memcpy_cache/ + neon_lock/ 整目录 + cache/ 4 标量）；arm-0102 基础
10 项取 main 退册；**移植 main c51a739b 的 knob init-only 修复**到
arm_add/low_load_cache_pressure{,_neon}（PR 版分叉早于该修复，run 内查询
会被框架 fail-loud abort）。

**Findings 处置（三轮 49 条 + 同型残留）**：第一轮 9 条（INJECT 残留等）
分支已修；二/三轮核心 = L3 家族 sum/k-space 聚合校验违反「逐元素精确
比对」铁律。按**模式族一次收敛**（三轮 review 就是同型缺陷逐轮点名新
复制体的循环，部分修不收敛）：
- **element-exact 收口 24 文件**：read _int 6 + read neon_fp64 保留 2 +
  write int 3 + write neon_fp64 3 + llc_{randomized,uniform}×3 数据通路 6 +
  adaptive_l3_sdc 四引擎×3 通路——golden 副本（写点双写/生成时拷贝）+
  逐 lane 位比对；global_sum/global_ksum 计数器随协议退役
- **退役 4 件 FP64 逐字节复制体**（diff 实证仅测试名行不同，零边际检出
  能力）：avx2_read_only / sse_read_L3 / sse_read_only /
  avx512_asymm_read_only 的 neon_fp64 版；read 家族保留每对代表件
- **T10 死代码 25 处**（throwing new 后 `if(!p)` 永假分支）→ bad_alloc
  干净 SKIP（对齐 gemm_cdouble_dynamic_square 先例）
- 头注修正 7 处（l3_execute_neon{,_fp64} 机制表述更新为 dc-civac 收口后
  语义 + 路径勘误；llcstress 自相矛盾句；llcstress_neon STRIDE_V=512 槽
  互斥前提；llc_uniform_load_neon 1/32→1/8 四倍错误；NUM_BLOCKS
  =256→262144 千倍错误）
- locv_b_pair_remote_store 的 ready 跨线程 flag volatile→atomic
  （release/acquire——两轮点名的「另一半」）
- fail 路径冗余 fprintf + ANSI 色码清除

**验证（rule 2，全部实测）**：
- 构建 1018 目标全过（中途三处脚本改造引入的编译错——NEON 无
  vmaxvq_u64、变量作用域、id 引用——当即修复重建），0 新警告
- **受影响测试逐件实测 37 件全 pass**：标量 L3 12 + 保留 FP64 10 +
  NEON 孪生 5 + locv_b 三态 `-n 4`（原子化后多线程正常）+ knob 移植 2 +
  adaptive_l3_sdc + 多线程样本（sse/asymm_write `-n 4`）+ 回归 zstd19
- 本机为 NEON/L3 可运行主机——全部受影响行为原生验证，无 gem5-fi 边界
- `--list-tests` **519 PROD / 628 q0 / 632 q-1** 实测落盘（README/CLAUDE.md
  计数链同步）；工作期间分支侧新推 2 个 docs commits（adaptive 21 臂
  标定档案）已 merge 融合（计数随退役同步）

**PR 814 open items（移交）**：
- `_int` read 家族中 avx2_read_only_int vs avx2_read_L3_int 为注释级复制
  （宣称侧重不同、实现相同）——review 未点名、系 633 原生迁移家族，
  未退役，留观察
- 本轮 push 后 pr-loop 将触发新一轮检视——adaptive_l3_sdc（11-18 最新
  大文件）尚未被任何一轮 review 覆盖，预期有新 findings

---

## PR 695 — feat/nodepool-offload（跨机卸载，11 文件 +4150）

**处置 commit**（push 至 `feat/nodepool-offload`，6035cf6c..087d8973）：
1. `merge: main —— _worktree_cleanup 取 11-17 检视收口版 + 测试锚同步 3× 预算`

**Triage 发现**：两轮 24 条 findings 已全数由分支 11-17 的 13 个 fix
commits 处置（逐条核对：密码 redact 20dc05b2 + docs/cases 历史遗留随
main 31f6a99e 目录删除自动消失、远程探活替代本机 os.kill、token 通道
主机密钥 b8d4cab2、PID 回显 1ca4aad9、_parse_probe 归一 3d3fd549、
unreachable 有界告警 6035cf6c、板上 worktree 清理 78659bcf、恒真断言
a6dc9ece 等）。真正阻塞是 CONFLICTING——`scripts/task_loop.py` 的
`_worktree_cleanup` 两分支并行修复同一生产缺陷（#811 僵尸 worktree）。

**核心裁决**：取 main 的 11-17 检视收口版（`_git_quiet` 全容错「清理绝不
外抛」+ remove 3× 预算 + prune 移 finally——main 版注明「nodepool 分支
同修经真实 git 验证」，且 PR 的 11-16 版 45s 短超时假设在生产体量不
成立）；PR 侧测试 `test_worktree_cleanup_unlocks_before_remove` 的 remove
预算断言随实现同步 45s→135s（锚定的是被取代的旧实现值）。

**验证（rule 2）**：
- py_compile 三模块通过；bash -n nodepool_setup.sh 通过
- pytest 全套 **239 passed**；唯一失败 `test_audit_non_dry_ensures_labels`
  为 **main 基线同款环境性失败**（依赖 `/home/sdc/wangxu/operator/
  sdcshield-autoci` 克隆路径，本机不存在；main 上裸跑同样失败，非本 PR
  引入）
- 纯 Python/SSH 工程，无 gem5-fi 面

**PR 695 open items（移交）**：上述环境性测试失败在能访问该克隆的主机
上复核；`test_audit_*` 系列对特定绝对路径的依赖本身值得在本机无关化。

---

## 汇总

| PR | 处置前 | 处置后 | push | 验证 |
|---|---|---|---|---|
| 518 march tiers | CONFLICTING, findings 已修 | **MERGEABLE** | ad22f79c..8367b293（1 commit） | 多重集/list-tests 双 IDENTICAL + 回归 pass |
| 814 opendcdiag | CONFLICTING, 49 findings | **MERGEABLE** | a75b2855..cc82e1b5（4 commits） | 构建 0 新警告 + 37 件实测 pass + 519 计数落盘 |
| 695 nodepool | CONFLICTING, 24 findings 已修 | **MERGEABLE** | 6035cf6c..087d8973（1 commit） | 239 passed（1 环境性预存失败同 main） |

**共同 open items（移交 orchestrator）**：
1. 三 PR 的 reviewDecision 仍为空——需人类 approving review（角色边界：
   不 merge、不 resolve thread、不改 WIP: 标题）
2. main 的 CLAUDE.md 存在 6 处 docs/research/ 悬空引用（31f6a99e 大清理
   遗留，main 自身问题，非本任务三 PR 范围）——建议单开 docs PR 修
3. 本机（Kunpeng-920 no-SVE 变体）与 CLAUDE.md 口径主机（SVE1-256 鲲鹏 920）
   是同族不同主机；三 PR 的 SVE 面验证依据 = march 多重集逐字节相等（518）/
   ARM64_REQUIRES_SVE 门控净变化 0（814 的 SVE 系测试）/无 SVE 面（695），
   如需 SVE 运行时行为佐证可在 SVE1-256 主机补跑（本机无 SVE）
