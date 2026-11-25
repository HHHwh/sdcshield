# Copyright 2025 Intel Corporation.
# SPDX-License-Identifier: Apache-2.0
#
# fmov/: LDP/STP 访存循环拷贝探针族 + adaptive_ldp_stp 自适应引擎
# （346 套件 SIMDFPLoadStoreMemLoopCpy 谱系，2026-11-24 用户指令建此目录）。
# 自门控模式抄 tests/cpu/arm64/meson.build：整个 subdir 只在 aarch64 守卫内
# 进入，x86-64 零改动。纯 NEON 基线（armv8.1-a），无 +crypto/+sve 需求。

if host_machine.cpu_family() == 'aarch64'
    fmov_tests = files(
        'ldp_stp_cpy_p08.cpp',
        'ldp_stp_cpy_p13.cpp',
        'ldp_stp_cpy_p05.cpp',
        'ldp_stp_cpy_p06.cpp',
        'adaptive_ldp_stp.cpp',
    )

    fmov_tests_lib = static_library(
        'tests_fmov',
        fmov_tests,
        build_by_default: false,
        include_directories: [
            framework_incdir,
        ],
        dependencies: [
            boost_dep,
        ],
        cpp_args: [
            debug_c_flags,
            default_cpp_warn,
            default_cpp_flags,
            march_generic_flags,
            '-march=armv8.1-a',
        ],
    )

    # Intrinsics-built twins (_intr, 2026-11-25): compiled with
    # -fno-tree-loop-distribute-patterns — the intr arm's premise is a
    # compiler-built ELEMENT TRAIN, and GCC's loop-distribution pass
    # idiom-recognizes monotone (and even alternating-direction) copy
    # loops into libc memcpy() calls (observed 2026-11-25: `bl memcpy`),
    # which would duplicate the repo's existing 28-test memcpy fleet.
    # The asm arm above keeps its original flags untouched.
    fmov_intr_tests = files(
        'ldp_stp_cpy_p08_intr.cpp',
        'ldp_stp_cpy_p13_intr.cpp',
        'ldp_stp_cpy_p05_intr.cpp',
        'ldp_stp_cpy_p06_intr.cpp',
        'adaptive_ldp_stp_intr.cpp',
    )

    fmov_intr_tests_lib = static_library(
        'tests_fmov_intr',
        fmov_intr_tests,
        build_by_default: false,
        include_directories: [
            framework_incdir,
        ],
        dependencies: [
            boost_dep,
        ],
        cpp_args: [
            debug_c_flags,
            default_cpp_warn,
            default_cpp_flags,
            march_generic_flags,
            '-march=armv8.1-a',
            '-fno-tree-loop-distribute-patterns',
        ],
    )

    sandstone_tests += [
        fmov_tests_lib,
        fmov_intr_tests_lib,
    ]
endif
