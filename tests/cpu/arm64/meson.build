# Copyright 2025 Intel Corporation.
# SPDX-License-Identifier: Apache-2.0
#
# ARM64-native NEON and crypto (AES) tests. These are self-contained
# aarch64-only tests using <arm_neon.h>; they are skipped (no-op) on x86-64
# because the entire subdir is entered only under the aarch64 guard below.
# (The x86-64 counterpart of crypto coverage is the OpenSSL SHA test under
# tests/cpu/openssl, gated separately by ssl_link_type.)

if host_machine.cpu_family() == 'aarch64'
    # crypto_test.cpp uses vaeseq_u8 / vaesmcq_u8, which require the +crypto
    # target; neon_test.cpp uses baseline armv8.1-A NEON. A single march covers
    # both files.
    arm64_cpp_flags = [
        '-march=armv8.1-a+crc+crypto',
    ]

    arm64_tests = files(
        'neon_test.cpp',
        'crypto_test.cpp',
        # di/dt voltage-transient power virus ("版图三 瞬态故障动态激发").
        # Alternates NEON full-load bursts with yield stalls to induce voltage
        # droop and excite transient SDC on the SIMD/FMA path — the only
        # burst/stall power-virus in the suite. Built only on aarch64.
        'power_virus_dit.cpp',
        'sdc_test.cpp',
        # Integer Execution Unit SDC stress — coverage-weak unit (IEX 70%).
        # Full IEX opcode combination (add/sub/and/or/xor/mul/shift/clz/rbit/
        # rev) over high-Hamming operands with serial dependency, plus
        # carry-flag dependent adcs/sbcs/csel chains run twice to detect
        # non-deterministic flag-rename SDC — the opcode-mix and flag-rename
        # surfaces operand_space_arm/adcx/mrn_ never cover. Built only on
        # aarch64.
        'iex_operand_combo_arm.cpp',
        # Instruction-Fetch Unit SDC stress — coverage-weak unit (IFU 66%).
        # JIT-generated data-dependent indirect-branch (br) dispatch table
        # with a high-Hamming target permutation and padded blocks, to
        # torture the branch predictor / BTB / instruction-cache path that no
        # existing test targets. Built only on aarch64.
        'ifu_branch_target_arm.cpp',
        # Out-of-order execution SDC stress — coverage-weak unit (OoO 56%).
        # Pointer-chasing dependency chain, carry-dependent mixed ALU+memory
        # chain, RW-hazard interleave, and long dependent ALU chains to stress
        # the scheduler/ROB/rename under serial dependencies — the replay/
        # rename surface no existing test targets. Built only on aarch64.
        'ooo_dep_chain_arm.cpp',
        # LSU store-buffer/load-forwarding SDC stress — coverage-weak unit
        # (LSU 54%). Same-size forward at widths 1/2/4/8/16B and offsets
        # including the cache-line boundary, partial-store forwarding/merging,
        # store-after-load hazard, 128B cross-line forward, and strided
        # same-line forward — the forwarding datapath no existing memory test
        # targets. Built only on aarch64.
        'lsu_store_forward_arm.cpp',
        # L2 cache coherency/split-access SDC stress — coverage-weak unit
        # (L2C 40%). Adds cross-64B-line exclusive ldxr/stxr RMW, store-to-load
        # forwarding across the line boundary, dc-civac writeback probe, and a
        # 2x-L2 thrash sweep — the coherency/snoop/split surfaces that
        # memcpy_l2_cache_size never touches. Built only on aarch64.
        'l2c_cross_cache_line_arm.cpp',
        # MMU/TLB/PTW SDC stress — the coverage-weakest unit (MMU 20%). Adds
        # page-color aliasing (one physical page, many VAs), dual-TLB-lookup
        # cross-page and cross-cache-line split accesses, and a large-ASID
        # pressure sweep, none of which mmu_stress_arm exercises. Built only
        # on aarch64.
        'mmu_split_tlb_arm.cpp',
        # Floating-point/SIMD Unit byte-exact SDC stress (FSU 80% — highest
        # weak unit, but with a real gap: fma.cpp compares with a 1e-6
        # tolerance, so a 1-bit FSU SDC passes. This test is byte-exact
        # throughout: FMA over the full IEEE-754 special-value table, cross-
        # cache-line 128B NEON data path, single<->double widen/narrow, and
        # a high-Hamming dependent FMA chain, all compared bit-exact against
        # libm. Built only on aarch64.
        'fsu_byteexact_arm.cpp',
        # AGU throughput stress: 2 source loads + data-ALU + store/reload/
        # store chain (3 ldr + 2 str per element), maximizing address-
        # generation pressure. Rotation uses {add,eor,and,orr} — a different
        # composition than mrn_nuke_2src_alu's {add,sub,eor,and} — to test
        # rotation-composition robustness of the core-179 trigger recipe.
        # Dual-arch inline asm (x86 movq / aarch64 str-ldr), placed in the
        # ARM64 suite per the arm64 classification. Built only on aarch64.
        'agu_stress_2src.cpp',
        # NEON/vector-datapath port of the core-179 trigger recipe: 2 source
        # vector loads + rotating vector ALU {add,eor,and,orr} (uint64x2) +
        # back-to-back vector store/reload/store. The asm-guaranteed str q ->
        # ldr q pair (the compiler eliminates the intrinsic reload) makes it
        # discriminate whether the trigger recipe is bound to the scalar
        # ALU/L1D path or also fires on the SIMD datapath. Uses uint64x2_t
        # unconditionally, so it can only be built on aarch64.
        'neon_rot_2src.cpp',
        # movbe byte-swap SDC series (moved from tests/cpu/misc): the core-179
        # research testbed of the byte-swap round-trip trigger. movbe is the
        # baseline (bswap + store + bswap-back + compare), movbe_dump adds the
        # full dump on failure, and the 11 movbe_dump_probe_* variants remove
        # or vary one ingredient at a time (no-store, reload position, ...)
        # to localize the trigger. All portable __builtin_bswap32 (no arch
        # guards), classified ARM64-only with the rest of the core-179 probes.
        'movbe.cpp',
        'movbe_dump.cpp',
        'movbe_dump_probe_a.cpp',
        'movbe_dump_probe_b.cpp',
        'movbe_dump_probe_c.cpp',
        'movbe_dump_probe_d.cpp',
        'movbe_dump_probe_e.cpp',
        'movbe_dump_probe_f.cpp',
        'movbe_dump_probe_g1.cpp',
        'movbe_dump_probe_g2.cpp',
        'movbe_dump_probe_h.cpp',
        'movbe_dump_probe_x.cpp',
        'movbe_dump_probe_xn.cpp',
        # NOTE: neon_rot_ldr_at_top_rowmajor.cpp and its k*/sve* variant
        # family live in tests/cpu/misc/ + tests/cpu/sve/ (registered in
        # tests/cpu/meson.build). This directory's copy was a byte-identical
        # duplicate of misc/neon_rot_ldr_at_top_rowmajor.cpp (commits 8e397ec0
        # and 3a4043f2 landed the same file twice) and its double
        # registration broke the final link with "multiple definition of
        # _test_neon_rot_ldr_at_top_rowmajor" — removed here.
    )

    arm64_tests_lib = static_library(
        'tests_arm64',
        arm64_tests,
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
            arm64_cpp_flags,
        ],
    )

    # SVE full-vector-length (sve512_fma family) SDC stress tests. Compiled
    # in their own static library because <arm_sve.h> intrinsics require the
    # +sve target, while the tests_arm64 baseline stays armv8.1-a+crc+crypto:
    # enabling +sve for the whole library could let GCC auto-vectorize the
    # NEON tests into SVE instructions, which would SIGILL on SVE-less CPUs
    # such as this Kunpeng 920. Every test probes HWCAP_SVE first thing in
    # test_init and returns a clean EXIT_SKIP on SVE-less CPUs, so their SVE
    # codegen only ever executes on SVE-capable hardware. Built only on
    # aarch64.
    arm64_sve_lib = static_library(
        'tests_arm64_sve',
        files(
            # workload 4: gather/FMA/scatter round-trip over a permuted
            # index table, byte-exact vs a scalar golden indexing through
            # the SAME indirection.
            'sve512_gather_scatter_arm.cpp',
            # workload 1: 512-step serial svmla_f64_x dependency chain over
            # high-Hamming finite operands (|x| <= 2, so the chain can never
            # overflow to Inf/NaN), byte-exact vs a scalar fma() golden
            # recomputed with the identical operation order.
            'sve512_f64_chain_arm.cpp',
            # workload 2: same serial FMLA chain over the IEEE-754 special
            # table (NaN/sNaN/Inf/+-0/+-1), compared by category per
            # fsu_byteexact_arm precedent — a category change is always a
            # failure; NaN payloads are not byte-compared because IEEE 754
            # does not require deterministic NaN payload propagation.
            'sve512_f64_special_arm.cpp',
            # workload 3: the same serial FMLA chain structure at f32 width
            # (svmla_f32_x), exercising the 16-lane f32 FMA/predicate
            # datapath at full 512-bit width, byte-exact vs a scalar
            # fmaf() golden recomputed with the identical operation order.
            'sve512_f32_chain_arm.cpp',
            # SVD-format variant of workload 1: same serial svmla_f64_x
            # chain, but the operand streams are walked through a 16x16
            # nested block traversal (BDCSVD-style block-of-matrix access)
            # over an SVD-scale allocation (300*300*512*VL elements per
            # stream, ~2.7 GiB each at a 512-bit VL). The per-iteration
            # touched set (~24 MB, 5.76 MB step stride) overflows L2 and
            # pressures L3/TLB — the first of eigen_svd_cdouble_sve's
            # SVD-only trigger paths (working set beyond L2).
            'sve512_f64_chain_svd.cpp',
            # SVD-format variant of workload 2: the same SVD-scale block
            # traversal over the IEEE-754 special-value table (64-step
            # chain), compared per lane by category (fsu_byteexact_arm
            # precedent). Allocation: 300*300*64*VL per stream (~360 MB
            # each at a 512-bit VL).
            'sve512_f64_special_svd.cpp',
            # SVD-format variant of workload 3: the same SVD-scale block
            # traversal at f32 width (svmla_f32_x, 16-lane f32 datapath),
            # byte-exact vs a scalar fmaf() golden. Allocation:
            # 300*300*512*VL f32 elements per stream (~2.7 GiB each at a
            # 512-bit VL).
            'sve512_f32_chain_svd.cpp',
            # SVD-format variant of workload 4: gather/FMA/scatter
            # round-trip over a 2-D 16x16 block-permuted index table
            # (90000 f64 elements, ~2.9 MB working set across src/dst),
            # byte-exact vs a scalar golden indexing through the SAME
            # indirection.
            'sve512_gather_scatter_svd.cpp',
            # SCF/stencil axis-kernel SDC trigger replicator — verbatim
            # port of the cn23154 NUMA3 field reproducer (example.cpp):
            # svdup_f64(coeff[r]) coefficient loads (ld1rd replicate-load
            # codegen under clang), dual-vector svmla_x chain, full unroll,
            # memory-resident loop counter, per-iteration VA[63:48] canary.
            # One deliberate semantic fix vs the standalone, per the field
            # diagnosis (5 bit-identical reruns): the expected value is
            # 1.5*iterations*(j+1) (RADIUS=6), the stale RADIUS=4 formula
            # iterations*(j+1) was a software false positive.
            'sve512_stencil_axis_arm.cpp',
            # XLSDFT composite-context SDC stress (escalation T1):
            # interleaves the stencil axis kernel (phase A), the SVD-format
            # gather/scatter over an L2-overflow working set (phase B) and
            # a workspace pointer-table canary (phase C) in one per-core
            # loop — the known fault-window ingredients without the MPI/
            # libomp scaffolding.
            'sve512_xlswave_ctx_arm.cpp',
            # libomp fork/join context emulation (escalation T2):
            # SVE FMLA bursts alternating with time-synchronized
            # collective park windows + function-pointer-table/counter
            # metadata canaries — the runtime-interaction ingredient of
            # the fault window, access-pattern only (layout honestly not
            # replicated). Deadlock-free by construction.
            'libomp_ctx_arm.cpp',
        ),
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
            '-march=armv8.2-a+sve',
            sve_sanitize_exempt,
        ],
    )

    # SME/ZA tile-view SDC stress (sme_za_tileview_arm). Own static_library
    # because the naked-asm ZA kernel needs armv9-a+sve+sme+sme-f64f64, which
    # neither the tests_arm64 baseline (armv8.1-a+crc+crypto) nor the SVE
    # library (armv8.2-a+sve) can host, and which GCC-12 cannot parse at all.
    # Compiler-support probed with the C++ compiler object so GCC builds
    # degrade gracefully (test simply absent, the gmp/acl/openblas pattern);
    # the test itself re-gates at runtime on HWCAP2_SME and the measured
    # 512-bit SVL geometry, so it only ever executes on characterized SME
    # hardware. Built only on aarch64.
    sme_march = '-march=armv9-a+sve+sme+sme-f64f64'
    sme_args = [sme_march]
    # Clang keeps __ARM_FEATURE_BTI defined for v8.5+/v9 marches regardless
    # of emission; the TU undefines the macro (see its header comment), and
    # this flag keeps that claim true by explicitly disabling BTI emission.
    if cpp.has_argument('-mbranch-protection=none')
        sme_args += ['-mbranch-protection=none']
    endif
    if cpp.has_argument(sme_march)
        arm64_sme_lib = static_library(
            'tests_arm64_sme',
            files(
                'sme_za_tileview_arm.cpp',
                'sme_fmopa_stress_arm.cpp',
            ),
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
                sme_args,
                sve_sanitize_exempt,
            ],
        )

        sandstone_tests += [
            arm64_tests_lib,
            arm64_sve_lib,
            arm64_sme_lib,
        ]
    else
        message('Compiler lacks SME support (' + sme_march + ') - '
                + 'sme_za_tileview_arm is not built')
        sandstone_tests += [
            arm64_tests_lib,
            arm64_sve_lib,
        ]
    endif
endif
