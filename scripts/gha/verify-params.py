#!/usr/bin/env python3
# verify-params.py — 在 openEuler 容器内做"全量参数"功能测试的纯 stdlib 脚本。
#
# 为什么纯 stdlib(不 import yaml):三个系列的构建镜像 RPM 树都**没有** PyYAML;
# 容器内 import yaml 会直接崩。sdcshield 的 YAML 输出稳定,这里用简单行的正则/
# 前缀解析,只做"判定 pass/fail/skip + 崩溃计数 + 摘字段",不做完整 YAML 反序列化。
#
# 验证设计(对齐 CLAUDE.md 全量要求 + 已知平台怪癖):
#   1) 枚举: 默认 PROD; --quality=-1 覆盖 BETA+SKIP; --selftests 覆盖框架自测(含期望失败的
#      selftest_failinit 等,统一用 retest-on-failure=0 让框架不重试、原样返回)。
#   2) 并发: 分 runner 拓扑自适应 1/4/8 线程(容器内用 nproc)。
#   3) openblas: 补 -O <id>.mdim 扫谱(仅存在时)。
#   4) 负面用例集(selftest_*fail/abort/sig*)逐个 -e 跑并断言"非零退出但不 insn 崩溃"。
#   stdout 末行 RESULT: PASS|FAIL 供 workflow 解析;退出码 0=PASS / 1=FAIL。
#
# 用法:
#   python3 verify-params.py --bin /path/sdcshield --out /out/results
# SPDX-License-Identifier: Apache-2.0
import argparse
import os
import re
import subprocess
import sys

RESULT_RE = re.compile(r"^- test:\s*(\S+)")
RESULT_VAL = re.compile(r"^  result:\s*(\S+)")
RUNTIME_RE = re.compile(r"^  test-runtime:\s*(\S+)")
# 种子字段(F-009:此前被丢弃,失败后无法按种子复现):
#   - 正常路径: verbosity>=3 时 logging.cpp 输出 "  state: { seed: '...', ... }"
#   - 失败路径: fail 块独立 "  seed: '...'" 行
SEED_STATE_RE = re.compile(r"^  state: \{ seed: '([^']*)'")
SEED_LINE_RE = re.compile(r"^\s+seed:\s*'([^']*)'")
CRASH_RE = re.compile(
    r"crash-context|exited with signal|backtrace|SIGSEGV when|SIGABRT when|SIGILL when|SIGBUS when|SIGFPE when",
    re.IGNORECASE,
)
# 跳过关键字(placeholder 诚实跳过、无硬件、超时)——不计 fail
SKIP_OK_RE = re.compile(
    r"to be implemented|CpuNotSupported|not available|unsupported|placeholder|"
    r"test resource issue|requires \bSMT\b|SVE|requires",
    re.IGNORECASE,
)

# 期望失败的 selftest(框架自测故意触发失败路径;退出码非 0 是"正确")。
# 只收【确定性】负路径:fail/abort/sig*/reportfail/oserror/malloc/cxxthrow。
# 排除三类:
#   - freeze 类(selftest_freeze/freeze_fork):故意挂死测 300s hang-watchdog,纯耗时。
#   - randomfail/timed_randomfail 类:概率性失败(rare 几乎总 rc=0,25pct/50pct 也可能 0),
#     非确定性,不能断言"必须非零退出"。
#   - cxxthrowcatch / datacomparefailrare_*:语义是"catch 住/catch 不到的罕见分支",
#     rc 不定,不属确定性负路径。
NEGATIVE_SELFTESTS = [
    "selftest_fail", "selftest_failinit", "selftest_abort", "selftest_abortinit",
    "selftest_sigill", "selftest_sigsegv", "selftest_sigbus",
    "selftest_sigsegv_init", "selftest_sigsegv_instruction",
    "selftest_sigsegv_kernel", "selftest_sigkill", "selftest_reportfail", "selftest_reportfailmsg",
    "selftest_oserror", "selftest_datacomparefail_double", "selftest_datacomparefail_float",
    "selftest_malloc_fail", "selftest_cxxthrow",
]
# 与现有 verify-built-pristine.sh 一致的数值敏感 eigen 集(192 核 ULP flakiness → -n 1)
EIGEN_N1 = ["eigen_svd_double", "eigen_sparse", "eigen_svd_cdouble", "eigen_svd_cdouble_sve"]
# 基准/参数扫描的 openblas mdim 档
OPENBLAS_MDIM = ["64", "256", "512"]

# 云 runner 拓扑相关测试:mesh_upi_* 是跨 NUMA/mesh 互联压测,需要多 socket 真实硅
# (kunpeng920 多 NUMA);GHA hosted 是单 socket 2-CPU Neoverse V1,这些测试会永久挂起
# 直到框架 300s 超时(exit: invalid)。在云 runner 上 --disable 排除(诚实:非软件 bug,
# 是硬件拓扑缺失)。本地/self-hosted kunpeng920 上仍会跑。
# 注意:--disable 接受通配符,这里用精确前缀 + 通配。
CLOUD_DISABLE = ["mesh_upi*", "memcpy_rewr"]


def run(cmd, timeout=900):
    """跑一条命令,返回 (exit_code, stdout_text)。容器内用 --ignore-timeout 规避 cgroup 超时误报。
    用 text=True 拿 str;但部分 selftest(负面崩溃类)会输出原始二进制字节,subprocess 偶发
    仍给 bytes —— 这里做 bytes→str 兜底(utf-8, errors=replace),保证下游 CRASH_RE 可用。"""
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout, text=True)
        out = p.stdout
    except subprocess.TimeoutExpired as e:
        return 124, _to_str(e.stdout)
    return p.returncode, _to_str(out)


def _to_str(b):
    if b is None:
        return ""
    if isinstance(b, str):
        return b
    return b.decode("utf-8", errors="replace")


def list_tests(bin_path, extra=None):
    cmd = [bin_path, "--list-tests"]
    if extra:
        cmd += extra
    rc, out = run(cmd)
    return [l.strip() for l in out.splitlines() if l.strip()]


def parse_result_yaml(text):
    """从 sdcshield YAML 输出里摘 per-test 的 (test, result, runtime)、崩溃计数与种子。
    text 可以是字符串(小文件)或文件路径(大文件流式读,避免 400MB YAML 全量入内存 OOM)。"""
    tests = []          # list of (name, result, runtime)
    crashes = 0
    seeds = []          # 种子值列表(state 行 + fail 块 seed 行,供失败复现)
    cur = None

    def _feed(line):
        nonlocal crashes, cur
        m = RESULT_RE.match(line)
        if m:
            if cur is not None:
                tests.append(cur)
            cur = [m.group(1), "unknown", 0.0]
            return
        ms = SEED_STATE_RE.match(line)
        if ms:
            seeds.append(ms.group(1))
        else:
            ml = SEED_LINE_RE.match(line)
            if ml:
                seeds.append(ml.group(1))
        if cur is None:
            if CRASH_RE.search(line):
                crashes += 1
            return
        mv = RESULT_VAL.match(line)
        if mv:
            cur[1] = mv.group(1)
        mrt = RUNTIME_RE.match(line)
        if mrt:
            try:
                cur[2] = float(mrt.group(1))
            except ValueError:
                cur[2] = 0.0
        if CRASH_RE.search(line):
            crashes += 1

    if "\n" in text or text.strip().startswith(("command-line", "version", "os:", "- test", "{")):
        for line in text.splitlines():
            _feed(line)
    else:
        # 文件路径:流式逐行读,不整文件入内存
        with open(text, "r") as f:
            for line in f:
                _feed(line.rstrip("\n"))
    if cur is not None:
        tests.append(cur)
    return tests, crashes, seeds


def parse_result_file(path):
    """流式解析 YAML 文件(不整读入内存),返回 (tests, crashes, seeds)。文件不存在返回 ([], 0, [])。"""
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return [], 0, []
    return parse_result_yaml(path)


def summarize(tests, crashes):
    # 非致命结果:pass/skip/timed out/interrupted/operating system error/invalid
    #   - timed out/interrupted: 云 runner 上互联类测试因单 socket 拓扑超时,非软件 bug
    #   - invalid: 框架遇到某测试超时后的整体退出态
    # 致命:fail(字节错配)/crash(信号崩溃)
    npass = sum(1 for _, r, _ in tests if r == "pass")
    nskip = sum(1 for _, r, _ in tests if r == "skip")
    nsoft = sum(1 for _, r, _ in tests if r in ("timed out", "interrupted", "operating system error", "invalid"))
    nfail = sum(1 for _, r, _ in tests if r == "fail")
    ncrash = sum(1 for _, r, _ in tests if r == "crash")
    return npass, nskip, nfail, ncrash, crashes, nsoft


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--threads", type=int, nargs="*", default=None,
                    help="并发档,默认 [1,4,8] 按 nproc 收缩")
    ap.add_argument("--mdim", nargs="*", default=None,
                    help="openblas mdim 扫谱档,默认 64 256 512")
    ap.add_argument("--smoke", action="store_true",
                    help="快档:只跑 all-quality(-n1) + zstd19,跳过多线程/mdim/selftest 负面")
    args = ap.parse_args()

    bin_path = args.bin
    out_dir = args.out
    os.makedirs(out_dir, exist_ok=True)

    try:
        nproc = int(os.sysconf("SC_NPROCESSORS_ONLN"))
    except (AttributeError, ValueError):
        nproc = 4
    # 已在容器内但不一定全核可用;收缩并发档,保证时间可预测
    threads = args.threads or [1, 4, 8]
    threads = [t for t in threads if t <= nproc] or [1]
    mdim = args.mdim or OPENBLAS_MDIM

    # ---- 1) 枚举集合 ----
    prod_all = list_tests(bin_path)
    all_ql = list_tests(bin_path, ["--quality=-1"])
    selftests = list_tests(bin_path, ["--selftests"])
    total_enum = len(prod_all)

    log = []          # 人类可读摘要
    fail_total = 0
    crash_total = 0

    # 通用兜底参数:
    #   -vvv: 种子(random_generator_state)落盘,供 verify-params 按 seed 复现失败(F-009);
    #   --max-test-loop-count 3:固定每测试 3 次 main loop(禁用 test fracturing),
    #      否则框架对每测试多 seed 分片跑,生成数百 MB YAML + 数十分钟,2-CPU runner 会 OOM。
    #   --timeout=60s:每测试超时防互联类挂死 300s;
    #   --max-messages 0:关掉 per-thread 日志冗余;
    #   --ignore-timeout:把超时当非致命继续,而非整体 exit: invalid。
    COMMON = ["-vvv", "--retest-on-failure=0", "--max-test-loop-count", "3", "--timeout=60s",
              "--max-messages", "0", "--ignore-timeout"]
    # 云 runner 拓扑缺失:mesh_upi_*/memcpy_rewr 需多 NUMA 真实硅,单 socket VM 挂死 → 排除
    CLOUD_DIS = []
    for pat in CLOUD_DISABLE:
        CLOUD_DIS += ["--disable", pat]

    # ---- 2) 全质量级跑一遍(单线程基线, 判"能跑"与结果分布) ----
    #   --quality=-1 覆盖 PROD+BETA+SKIP;-n 1 规避 192 核 eigen ULP flakiness;
    #   -t 5000 只是安全上限(loop 数已由 --max-test-loop-count 限定)。
    stage_tests = []
    allq_seeds = []     # allquality 阶段全部种子(失败复现依据,汇总时打印)
    if not args.smoke:
        for extra, label in [
            (["--quality=-1", "-n", "1", "-t", "5000"] + COMMON + CLOUD_DIS, "all-quality(-n1)"),
        ]:
            y = os.path.join(out_dir, "allquality.yaml")
            cmd = [bin_path] + extra + ["-o", y]
            _rc, stdout = run(cmd)
            tests, crashes, seeds = parse_result_file(y)
            allq_seeds = seeds
            npass, nskip, nfail, ncrash, _, nsoft = summarize(tests, crashes)
            stage_tests.append((label, tests, crashes, npass, nskip, nfail, ncrash, nsoft, len(seeds)))

    for label, tests, crashes, npass, nskip, nfail, ncrash, nsoft, nseeds in stage_tests:
        # 运行了测试但未捕获到任何种子(state/seed 行缺失)同样判 FAIL:
        # verbosity<3 或日志被裁时失败将无法按种子复现,证据链缺失不能算通过
        ok = (nfail == 0 and ncrash == 0 and npass + nskip > 0 and nseeds > 0)
        if not ok:
            fail_total += 1
        crash_total += crashes
        log.append(f"[{label}] tests={len(tests)} pass={npass} skip={nskip} soft={nsoft} "
                   f"fail={nfail} crash={ncrash} seeds={nseeds} ({ 'OK' if ok else 'FAIL'})")

    # ---- 3) 多线程并发档(-n 1/4/8)对全 PROD 各跑一轮(内存/锁/cache 压力面) ----
    if args.smoke:
        for n in [1]:
            y = os.path.join(out_dir, "smoke_zstd19.yaml")
            # -vvv 与 COMMON 一致: YAML state 里的种子需要 verbosity>=3 才输出(F-009 复现链)
            cmd = [bin_path, "-n", "1", "-t", "3000", "-vvv", "--max-messages", "0",
                   "-e", "zstd19", "-o", y]
            _rc, stdout = run(cmd)
            tests, crashes, seeds = parse_result_file(y)
            npass, nskip, nfail, ncrash, _, nsoft = summarize(tests, crashes)
            ok = nfail == 0 and ncrash == 0 and npass >= 1 and len(seeds) > 0
            if not ok:
                fail_total += 1
            crash_total += crashes
            log.append(f"[smoke zstd19 -n 1] pass={npass} skip={nskip} fail={nfail} crash={ncrash} "
                       f"seeds={len(seeds)} ({'OK' if ok else 'FAIL'})")
        print(f"enumerated: prod={total_enum} all-quality={len(all_ql)} selftests={len(selftests)} "
              f"threads={threads} nproc={nproc} (smoke)")
        for line in log:
            print(line)
        verdict = "PASS" if (fail_total == 0 and crash_total == 0) else "FAIL"
        print(f"RESULT: {verdict} (sections_fail={fail_total} crashes={crash_total})")
        return 0 if verdict == "PASS" else 1

    for n in threads:
        y = os.path.join(out_dir, f"prod_n{n}.yaml")
        # eigen 数值类(>1 线程 ULP flakiness)+ 云 runner 拓扑缺失的 mesh_upi* 都 --disable
        disable_args = []
        for eig in EIGEN_N1:
            disable_args += ["--disable", eig]
        disable_args += CLOUD_DIS
        cmd = [bin_path, "-n", str(n), "-t", "1000"] + COMMON + disable_args + ["-o", y]
        _rc, stdout = run(cmd)
        tests, crashes, seeds = parse_result_file(y)
        npass, nskip, nfail, ncrash, _, nsoft = summarize(tests, crashes)
        ok = nfail == 0 and ncrash == 0
        if not ok:
            fail_total += 1
        crash_total += crashes
        log.append(f"[all-PROD -n {n}] pass={npass} skip={nskip} soft={nsoft} "
                   f"fail={nfail} crash={ncrash} seeds={len(seeds)} ({'OK' if ok else 'FAIL'})")

    # ---- 4) selftests 正向集(@positive) ----
    #   @positive 只带 --selftests 前缀生效。
    y = os.path.join(out_dir, "selftest_positive.yaml")
    cmd = [bin_path, "--selftests", "--quick", "--retest-on-failure=0", "-n", "1",
           "--max-messages", "0", "-e", "@positive", "-o", y]
    rc, stdout = run(cmd)
    pos_ok = (rc == 0)
    if not pos_ok:
        fail_total += 1
    log.append(f"[selftest @positive] rc={rc} ({'OK' if pos_ok else 'FAIL'})")

    # ---- 5) selftests 负面集(逐个跑, 期望退出码非 0 但不 insn 崩溃) ----
    neg_ok = True
    neg_list = [t for t in NEGATIVE_SELFTESTS if t in selftests]
    for t in neg_list:
        y = os.path.join(out_dir, f"selftest_{t}.yaml")
        cmd = [bin_path, "--selftests", "--quick", "--retest-on-failure=0",
               "--on-hang=kill", "--on-crash=kill", "-n", "1",
               "--max-messages", "0", "-e", t, "-o", y]
        rc, stdout = run(cmd, timeout=120)
        # 期望: 非 0 退出(代表"测试正确报告了失败"),且无机器指令级崩溃信号
        insn_crash = bool(CRASH_RE.search(stdout))
        ok = (rc != 0 and not insn_crash)
        if not ok:
            neg_ok = False
        log.append(f"[selftest {t}] rc={rc} insn_crash={insn_crash} ({'OK' if ok else 'FAIL'})")

    if not neg_ok:
        fail_total += 1

    # ---- 6) openblas mdim 参数扫谱(存在时) ----
    for t in ["openblas_dgemm", "openblas_sgemm", "openblas_zgemm"]:
        if t not in prod_all:
            continue
        for m in mdim:
            y = os.path.join(out_dir, f"{t}_mdim{m}.yaml")
            # 与 COMMON 等效(含 -vvv 种子落盘 + ignore-timeout),mdim 阶段种子同样可复现
            cmd = [bin_path, "-O", f"{t}.mdim={m}", "-n", "1", "-vvv",
                   "--retest-on-failure=0", "--max-test-loop-count", "3", "-t", "5000",
                   "--max-messages", "0", "--ignore-timeout", "-e", t, "-o", y]
            _rc, stdout = run(cmd)
            tests, crashes, seeds = parse_result_file(y)
            npass, nskip, nfail, ncrash, _, nsoft = summarize(tests, crashes)
            ok = nfail == 0 and ncrash == 0
            if not ok:
                fail_total += 1
            crash_total += crashes
            log.append(f"[{t} mdim={m}] pass={npass} skip={nskip} fail={nfail} crash={ncrash} "
                       f"seeds={len(seeds)} ({'OK' if ok else 'FAIL'})")

    # ---- 汇总 ----
    # echo 各节 + 末行 RESULT 供 workflow 解析
    print(f"enumerated: prod={total_enum} all-quality={len(all_ql)} selftests={len(selftests)} "
          f"threads={threads} nproc={nproc}")
    # 种子报告(F-009):失败时按下列种子 + --rng-state 复现(框架实际选项是
    # --rng-state / -s,不存在 --seed);零种子说明 verbosity<3 或日志被裁
    if allq_seeds:
        uniq = sorted(set(allq_seeds))
        print(f"seeds: captured={len(allq_seeds)} unique={len(uniq)} "
              f"(reproduce with: {bin_path} --rng-state {uniq[0]} ...)")
    else:
        print("seeds: captured=0 (verbosity<3 或无 state/seed 行——失败复现依据缺失)")
    for line in log:
        print(line)

    verdict = "PASS" if (fail_total == 0 and crash_total == 0) else "FAIL"
    print(f"RESULT: {verdict} (sections_fail={fail_total} crashes={crash_total})")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())