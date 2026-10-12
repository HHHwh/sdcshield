# 2026-10-10 — SVE 轻测试 fracture 空转(hollow)修复:满载压测

## 1. 问题(战役级发现)

r21/r22 波次中 13 个轻量 SVE/SME 测试在 608 核节点上的真实计算压力仅为意图值的 ~0.5%:

- 实测(cn23423, fma_sve, -t 900000):925s 内 19,156 次 fracture 迭代,中位迭代 36.9ms;
  strace 取证:每迭代 fork 16 子进程(Heuristic 切片,每 module 一片)×38 线程,
  子进程 utime=0(<0.26ms/线程用户态计算)、stime=30-120ms(clone/exit churn)
- 节点侧:CPU ~97.5% 空闲,load ~95(线程生灭虚胀,非计算)
- 后果:这些测试的 "pass" 不构成 SDC 压测证据 —— 有效激励 ≈0.5%,SDC 几乎无被激发机会

## 2. 根因(代码实证, framework/sandstone_run.cpp)

- `test_time_condition()`(:572)每迭代调用 `max_loop_count_exceeded()`(:554):
  `unsigned(inner_loop_count) >= unsigned(current_max_loop_count)`
- 轻测试默认 `fracture_loop_count == 0` → 自动 fracture,`current_max_loop_count=40`(:1721)
- 轻测试体(μs 级)40 次迭代即满足计数条件 → test_run 返回 → 38 线程退出 →
  16 子进程退出 → 父进程 refork(`run_one_test_children` 每 `run_one_test_once` 全量 fork)
- 父循环(:1737-1766)直到 `runtime >= current_test_duration` 才停 → 85+/s 的
  fork/spawn churn 主导全部墙钟
- 自动加倍(:1758)仅在迭代 <10ms 时触发;实测迭代 37-63ms,永不触发

## 3. 修复

对 13 个测试的 `DECLARE_TEST` 增加 `.fracture_loop_count = -1,`
(字段语义见 framework/sandstone.h:405-408:`<0` = never fracture):

| 文件 | 归档 |
|---|---|
| tests/cpu/sve/fma2/fma_sve.cpp | libtests_sve.a |
| tests/cpu/sve/fma2/fpu_special_values_sve.cpp | libtests_sve.a |
| tests/cpu/arm64/sve512_f32_chain_arm.cpp | libtests_arm64_sve.a |
| tests/cpu/arm64/sve512_f64_chain_arm.cpp | libtests_arm64_sve.a |
| tests/cpu/arm64/sve512_f64_special_arm.cpp | libtests_arm64_sve.a |
| tests/cpu/arm64/sve512_gather_scatter_arm.cpp | libtests_arm64_sve.a |
| tests/cpu/arm64/sve512_stencil_axis_arm.cpp | libtests_arm64_sve.a |
| tests/cpu/arm64/sve512_xlswave_ctx_arm.cpp | libtests_arm64_sve.a |
| tests/cpu/arm64/sve512_f64_chain_svd.cpp | libtests_arm64_sve.a |
| tests/cpu/arm64/sve512_f32_chain_svd.cpp | libtests_arm64_sve.a |
| tests/cpu/arm64/sve512_gather_scatter_svd.cpp | libtests_arm64_sve.a |
| tests/cpu/arm64/sme_za_tileview_arm.cpp | libtests_arm64_sme.a |
| tests/cpu/arm64/sme_fmopa_stress_arm.cpp | libtests_arm64_sme.a |

机制:`-1` 经 unsigned 比较恒 false → `test_time_condition()` 仅受墙钟 deadline 约束 →
单次迭代内 16 子×38 线程持续计算满 `-t` 时长;父循环在首迭代后
`current_max_loop_count <= 0` → `goto out`,零 refork。

不修的测试(实测或论证非 hollow):
- acl_gemm_sve:1256ms/块,重负载,auto-fracture 开销可忽略
- eigen_svd_double / eigen_svd_fvectors / eigen_sparse:x86-64 共享(rule 5)且 per-pass 重
- zstd19 / mce_check:实测重负载 / EDAC 型
- eigen_gemm_*_dynamic_square:Eigen GEMM per-pass 重(与 max 修复同源),auto-fracture 无害

## 4. x86-64 非回归(rule 5)

- 11 个 tests/cpu/arm64/ 文件:x86 不编译(meson per-arch 守卫)
- fma_sve / fpu_special_values_sve(x86 编译):test_run 首调即 `log_skip`+`EXIT_SKIP`
  (placeholder),线程即刻退出,循环计数字段完全不参与执行路径 → 行为逐字节不变
- 框架零改动

## 5. 验证(rule 2)

1. 编译:build_066.sh(compile_commands.json 驱动 13 文件增量编译 →
   ar 重建 libtests_sve.a / libtests_arm64_sve.a / libtests_arm64_sme.a →
   原 clang++ 命令重链),零新警告
2. 计算节点实测(单节点验证作业):
   - fma_sve -t 90 修复版:进程数=16 子×38 线程持续存活、CPU idle <5%、
     yaml 单块 test-runtime ≈ -t(对比修复前 19,156 块)
   - 13 测试各 -t 20 短跑:全部 exit: pass(金标比对不受 fracture 影响)
   - zstd19 -t 60 回归:exit: pass
3. 回归基线:zstd19 通过即链路完好(框架未动)

## 6. 提交与重投

- 本地 feature 分支 fix/acl-gemm-sve-rounding:先 docs(本计划)后 code(13 文件单逻辑单元),
  `git commit -s`(Signed-off-by: wangxu <wangxumarshall@qq.com> 末行),push
- r23:stress_resume23.sh 用修复二进制,测试集 = r21 集合(13 轻测试修复版 +
  重测试)+ zstd19 + mce_check,每用例 -t ≥900s,10 节点水位;
  cn23294 补跑 gemm/svd/zstd19
- 诚实记账:verified.txt 对旧轻测试 pass 行加 hollow 注记(有效覆盖以 r23 重跑为准)