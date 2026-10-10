# Copyright 2025 Intel Corporation.
# SPDX-License-Identifier: Apache-2.0
#
# SVE ports of the 53 avx-named tests (tests/cpu/fma, mesh, eigen_svd, ipsec
# families). The originals are NEON 128-bit implementations written on SVE-less
# silicon (see docs/docs_xu/2026-09-19-avx-named-tests-audit.md); these ports
# keep the workload environments byte-for-byte (vector width, random ranges,
# special-value injection, block/stride structure, atomic synchronization,
# golden patterns) and only swap the SIMD engine to SVE1 (armv8.2-a+sve).
#
# Naming rule (user decision 2026-09-19): _avx/_avx2 -> _sve, _avx512 ->
# _sve_wide. The _wide suffix reflects the wider-vector variant of the pair
# (original avx2=256-bit vs avx512=512-bit), NOT the SVE2 instruction set —
# no SVE2 instructions are used (target silicon has SVE1 only, VL=256).
#
# Build discipline mirrors tests/cpu/arm64/meson.build: the whole subdir is
# entered only under the aarch64 guard (x86-64 untouched), and the SVE object
# code lives in its own static library because <arm_sve.h> intrinsics require
# the +sve target — enabling +sve for tests_base could let GCC auto-vectorize
# NEON tests into SVE instructions, which would SIGILL on SVE-less CPUs.
# Every test probes HWCAP_SVE first thing in test_init and returns a clean
# EXIT_SKIP on SVE-less CPUs (sleef_sve.cpp precedent).

if host_machine.cpu_family() == 'aarch64'
    sve_avx53_sources = files(
        # FMA domain (10 ports of tests/cpu/fma/*avx*): same workload
        # environments (VECTOR_SIZE, random ranges, special-value injection
        # patterns, INNER_ITERATIONS, byte-exact memcmp + store/reload
        # consistency) with the SIMD engine swapped to SVE1. VL-agnostic:
        # loads/stores go through svwhilelt predicates in svcntd()/svcntw()
        # batches, so the tests run on any SVE vector length.
        'fma/fma_tail_sve.cpp',
        'fma/fma_tail_sve_wide.cpp',
        'fma/fmatail_sve.cpp',
        'fma/fmatail_sve_wide.cpp',
        'fma/fmatail_nested_sve.cpp',
        'fma/fmatail_nested_sve_wide.cpp',
        'fma/fmatail_double_nested_sve.cpp',
        'fma/fmatail_double_nested_sve_wide.cpp',
        'fma/fma_patterns_sve_wide_ps.cpp',
        'fma/fma_patterns_sve_wide_pd.cpp',
        # Mesh domain framework A (12 ports of tests/cpu/mesh/*avx*): the
        # multi-core cooperative write/barrier/read-verify/round-flip
        # protocols and block structures (8/16-element blocks, atomic
        # synchronization, sum + store/reload consistency checks) are
        # preserved from the NEON originals; only the block data movement
        # goes through SVE predicated ld/st. Note: the asymm variants fix a
        # stack-overread UB in the originals (reader loop ran to
        # TOTAL_ELEMENTS over an 8-element stack buffer); the SVE ports sum
        # exactly the BLOCK_ELEMENTS that are written, which matches the
        # described intent.
        'mesh/mesh_upi_sve_sym.cpp',
        'mesh/mesh_upi_sve_symm_int.cpp',
        'mesh/mesh_upi_sve_asymm_int.cpp',
        'mesh/mesh_upi_sve_asymm_write_int.cpp',
        'mesh/mesh_upi_sve_asymm_distrib_int.cpp',
        'mesh/mesh_upi_sve_asymm_distrib_write_int.cpp',
        'mesh/mesh_upi_sve_wide_sym.cpp',
        'mesh/mesh_upi_sve_wide_symm_int.cpp',
        'mesh/mesh_upi_sve_wide_asymm_int.cpp',
        'mesh/mesh_upi_sve_wide_asymm_write_int.cpp',
        'mesh/mesh_upi_sve_wide_asymm_distrib_int.cpp',
        'mesh/mesh_upi_sve_wide_asymm_distrib_write_int.cpp',
        # Mesh domain framework B (6 ports): avx2 trio = per-thread 4MB
        # sweep (read_only/read_L3 golden-sum scan, write_only
        # write-and-readback); avx512 trio = 16-element-block variants
        # (read_only/read_L3 init-golden scans, write_only cooperative
        # writer/reader protocol matching its original).
        'mesh/mesh_upi_sve_read_only_int.cpp',
        'mesh/mesh_upi_sve_read_L3_int.cpp',
        'mesh/mesh_upi_sve_write_only_int.cpp',
        'mesh/mesh_upi_sve_wide_asymm_read_only_int.cpp',
        'mesh/mesh_upi_sve_wide_asymm_read_L3_int.cpp',
        'mesh/mesh_upi_sve_wide_asymm_write_only_int.cpp',
        # Eigen domain (1): SVD-family workload without the Eigen library.
        # The SVE-backend sibling eigen_svd_cdouble_sve currently crashes
        # (vendored Eigen5 SVE double packet SIGSEGV on the BDCSVD
        # workload), so this port is a self-written deterministic
        # Householder bidiagonalization — the same FMA-dense SVD first
        # phase — with SVE-vectorized inner loops and the EigenSVDTest
        # golden pattern (init computes once, run recomputes and
        # byte-compares). n=128 calibrated at ~0.92ms/iter.
        'eigen/eigen_svd_bidiag_sve.cpp',
        # kreg batch (7 ports of the 64-batch plan): software simulations of
        # x86 AVX-512 k-register ops upgraded to REAL SVE predicate
        # instructions — the only true k-register equivalent on ARM.
        # kreg2: KSHIFTL/R -> svlsl/svlsr; kreg3: KORTEST/KTEST ->
        # svptest_any; kreg5: KUNPCK/KNOT -> svzip1/sveor; kreg6/kreg8:
        # mask expansion -> svsel; kreg9: KMOVB/W/D/Q -> vector/predicate
        # lanes + svcntp; arm0102: trigger recipe on ldr z/str z with
        # predicate compare expansion. Software references are now
        # independent scalar goldens (the originals compared an
        # expression with itself).
        'kreg/kreg2_sve.cpp',
        'kreg/kreg3_sve.cpp',
        'kreg/kreg5_sve.cpp',
        'kreg/kreg6_sve.cpp',
        'kreg/kreg8_sve.cpp',
        'kreg/kreg9_sve.cpp',
        'kreg/arm0102_kreg_mask_sve.cpp',
        # Batch 2 (12 NEON-direct replacements of the 64-test plan):
        # vector/FPU/misc tests whose NEON intrinsics map 1:1 onto SVE
        # instructions. Workload parameters preserved per original.
        'fma2/neon_add_sve.cpp',
        'fma2/fma_sve.cpp',
        'fma2/fpu_special_values_sve.cpp',
        'fma2/power_virus_dit_sve.cpp',
        'fma2/movdq2q_sve.cpp',
        'fma2/movq2dq_sve.cpp',
        'fma2/movmskpspd_sve.cpp',
        'fma2/fsu_byteexact_sve.cpp',
        'vector2/kreg1_sve.cpp',
        'vector2/kreg4_sve.cpp',
        'vector2/kreg7_sve.cpp',
        'vector2/swizzle_sve.cpp',
        'vector2/insert_extract_sve.cpp',
        # Batch 3 (2 conversions): fisttp on real svcvt lanes
        # (round-toward-zero semantics verified); IEX opcode-combo with
        # svclz (real instruction) + svtbl byte-reverse for rbit/rev
        # (bit-exact vs software reference, 20k random vectors).
        'arithmetic/fisttp_sve.cpp',
        'arithmetic/iex_operand_combo_sve.cpp',
        # Batch 4 (10): carry chains on svadd lanes with the serial carry
        # fold (equivalence vs __int128 verified on 20k random chains);
        # operand-space adds/muls (svmul low half; svmulh is SVE2 so the
        # high half folds __int128 on the same operand stream); 512-bit
        # bigint multiply on svmul partial products with an init-time
        # cross-check against the schoolbook golden.
        'arithmetic/adcx_sve.cpp',
        'arithmetic/adox_sve.cpp',
        'arithmetic/adcxlong_sve.cpp',
        'arithmetic/adcx_arm_sve.cpp',
        'arithmetic/adox_arm_sve.cpp',
        'arithmetic/adcxlong_arm_sve.cpp',
        'arithmetic/adcx_adox_interleaved_sve.cpp',
        'arithmetic/adcx_adox_interleaved_arm_sve.cpp',
        'arithmetic/operand_space_sve.cpp',
        'arithmetic/bigint_mulx_sve.cpp',
        # Batch 5 (21): core-179 dataflow family on SVE lanes — movbe byte-swap
        # round-trips (svtbl [3,2,1,0], verified vs __builtin_bswap32 on 50k
        # vectors), 11 probe variants (no-store/nops/SLF/global-store/NUMA/
        # line-window/set-attack/line-sweep/no-op-ALU/param-nops), the mrn
        # memory-renaming family, neon_rot_2src on z registers (asm ldr z/
        # str z), and mite's constant chain with predicate branches.
'core179/movbe_sve.cpp',
'core179/movbe_dump_sve.cpp',
'core179/movbe_dump_probe_a_sve.cpp',
'core179/movbe_dump_probe_b_sve.cpp',
'core179/movbe_dump_probe_c_sve.cpp',
'core179/movbe_dump_probe_d_sve.cpp',
'core179/movbe_dump_probe_e_sve.cpp',
'core179/movbe_dump_probe_f_sve.cpp',
'core179/movbe_dump_probe_g1_sve.cpp',
'core179/movbe_dump_probe_g2_sve.cpp',
'core179/movbe_dump_probe_h_sve.cpp',
'core179/movbe_dump_probe_x_sve.cpp',
'core179/movbe_dump_probe_xn_sve.cpp',
'core179/mrn_nuke_sve.cpp',
'core179/mrn_pairs_sve.cpp',
'core179/mrn_flags_sve.cpp',
'core179/mrn_reloaded_sve.cpp',
'core179/mrn_rmw_sve.cpp',
'core179/mrn_rmw_dump_sve.cpp',
'core179/neon_rot_2src_sve.cpp',
'core179/mite_sve.cpp',
'core179/movbe_slf_sweep_sve.cpp',
'core179/neon_rot_ldr_at_top_sve.cpp',
'core179/neon_rot_ldr_at_top_rowmajor_sve.cpp',
        # Batch 6 (first 2 of 7): partial-width predicated stores (the
        # native SVE form of partial stores) with store-buffer partial-hit
        # loads; AGU 2-source vector-load throughput with the rotating
        # composition and the str z/ldr z/str z chain.
        'mem/partial_store_forwarding_sve.cpp',
        'mem/agu_stress_2src_sve.cpp',
        # Batch 6 remainder (5): LSU store-forward with scalar asm micro-ops
        # preserved (they ARE the tested forwarding window) and the 128-bit
        # cross-line phase on svst1/svld1; L2C cross-line with ldxr/stxr/
        # dc-civac preserved and the 16B forward vectorized; MMU alias/split
        # with mmap/memfd logic intact and 128-bit stores vectorized; OoO
        # pointer-chase asm ldr preserved (the dependency chain IS the
        # test); arm64_sdc CRC/checksum detectors on svld1 scans with the
        # fail-closed self-check kept verbatim.
        'mem/lsu_store_forward_sve.cpp',
        'mem/l2c_cross_cache_line_sve.cpp',
        'mem/mmu_split_tlb_sve.cpp',
        'mem/ooo_dep_chain_sve.cpp',
        'mem/arm64_sdc_sve.cpp',
        # missing_testcases_20261009 ports (batch 1/8): movbe reload-path
        # probes — L1D-hit vs L2-reload differential pair + init-golden-dump
        # cross-validation (asm-guaranteed input reload, svtbl bswap on
        # lanes, per-line dc civac eviction in the l2 variant).
        'core179/movbe_l1d_vs_l2_sve.cpp',
        'core179/movbe_dump_golden_sve.cpp',
        # missing_testcases_20261009 ports (batch 2/8): mrn_rmw load-path
        # family — forwarding vs L1D-array (evict) pair, different-address
        # isolation probe + 64KB/1MB footprint scan, and the 6-point
        # nop-delay timing series (.rept mov x9,x9 fused with the ldr z).
        'core179/mrn_rmw_fwd_vs_l1d_sve.cpp',
        'core179/mrn_rmw_fwd_vs_diff_sve.cpp',
        'core179/mrn_rmw_diff_footprint_sve.cpp',
        'core179/mrn_rmw_timing_probe_sve.cpp',
        # missing_testcases_20261009 ports (batch 3/8): mrn constructed
        # family — nuke dependency-chain depth axis (2/3/4-source ALU
        # chains), 16x load-density cell, the pairs back-to-back collapse,
        # and the reloaded qword-segment immediate-verify variant.
        'core179/mrn_nuke_2src_alu_sve.cpp',
        'core179/mrn_nuke_3src_alu_sve.cpp',
        'core179/mrn_nuke_4src_alu_sve.cpp',
        'core179/mrn_nuke_dense_sve.cpp',
        'core179/mrn_pairs_2src_alu_sve.cpp',
        'core179/mrn_reloaded_2src_alu_sve.cpp',
        # missing_testcases_20261009 ports (batch 4/8): reload-necessity
        # and lane/axis probes — agu store-only (2 stores no load, OR
        # rotation), neon_rot store-only on z registers, the full-VL
        # byte-lane variant (width-vs-lane), and the random-address
        # disambiguation probe (svdup pattern at data-dependent offsets).
        'mem/agu_stress_store_only_sve.cpp',
        'core179/neon_rot_store_only_sve.cpp',
        'core179/neon_rot_u8x16_sve.cpp',
        'mem/mem_disambig_alu_sve.cpp',
        # missing_testcases_20261009 ports (batch 5/8): ldr_at_top base
        # variants — ALU-dimension (sub/eor/bic/orn, ORN composed), the
        # 32-bit MAC-lane variant (narrowed persisted sources), the
        # interleaved-layout address-separation cell, the 3-store and
        # 4-load count-gradient cells.
        'core179/neon_rot_ldr_at_top_sub_sve.cpp',
        'core179/neon_rot_ldr_at_top_mla_sve.cpp',
        'core179/neon_rot_ldr_at_top_offdiag_sve.cpp',
        'core179/neon_rot_ldr_at_top_str3_sve.cpp',
        'core179/neon_rot_ldr_at_top_ldr4_sve.cpp',
        # missing_testcases_20261009 ports (batch 6/8): rowmajor
        # instrumented variants — per-element binary records (dump), the
        # GPS self-describing-tag chassis with five-buffer dumps, the
        # dual-store-leg comparison, and the str3 3-way slot-mask
        # checkall with transient markers.
        'core179/neon_rot_ldr_at_top_rowmajor_dump_sve.cpp',
        'core179/neon_rot_ldr_at_top_rowmajor_gps_sve.cpp',
        'core179/neon_rot_ldr_at_top_rowmajor_dualcmp_sve.cpp',
        'core179/neon_rot_ldr_at_top_rowmajor_str3_checkall_sve.cpp',
        # missing_testcases_20261009 ports (batch 7/8): K-cells — the
        # interleaved dual-source layout cell (GPS-tagged + its natural-
        # random control), the 4x-footprint residency cell (byte_off tag
        # wrap documented), and the 4-cell pattern-activation battery
        # (zero/one/GPS-rand/hiham).
        'core179/neon_rot_ldr_at_top_rowmajor_k5far_sve.cpp',
        'core179/neon_rot_ldr_at_top_rowmajor_k5inter_rand_sve.cpp',
        'core179/neon_rot_ldr_at_top_rowmajor_k3res_sve.cpp',
        'core179/neon_rot_ldr_at_top_rowmajor_k7pattern_sve.cpp',
        # missing_testcases_20261009 ports (batch 8/8): arm-0102
        # trigger-recipe variants on pg4 sub-blocks — BSL select
        # (composed, no svbsl in GCC 12), NOT-logic, mask-logic, and the
        # 3-source f32 FMA rotation (neg-nested fnma/fnms kept verbatim);
        # +8-element buffer slack covers the asm full-VL ldr z tail.
        'kreg/arm0102_kreg_select_sve.cpp',
        'kreg/arm0102_kreg_not_sve.cpp',
        'kreg/arm0102_kreg_logic_sve.cpp',
        'kreg/arm0102_fma_f32_sve.cpp',
        # Batch 7 (4): library-backed tests re-implemented on SVE —
        # GMP big-add on the carry chain, GMP multiply on svmul partial
        # products (bigint_mulx skeleton), compiler-rt soft-float kept as
        # the tested integer path with an SVE probe, ACL GEMM on svmla
        # micro-kernels vs the naive golden.
        'arithmetic/gmp_bigadd_sve.cpp',
        'arithmetic/gmp_bignum_sve.cpp',
        'arithmetic/acl_gemm_sve.cpp',
    )

    # compiler-rt soft-float test: needs libclang_rt.builtins (mirror of the
    # arithmetic_arm crt block — same probe path, SVE probe added in init).
    crt_builtins_path_sve = '/usr/lib/clang/17/lib/aarch64-openEuler-linux-gnu/libclang_rt.builtins.a'
    if run_command('test', '-f', crt_builtins_path_sve, check: false).returncode() == 0
        crt_builtins_lib_sve = declare_dependency(
            link_args : ['-l:libclang_rt.builtins.a',
                         '-L' + '/usr/lib/clang/17/lib/aarch64-openEuler-linux-gnu'],
        )
        tests_sve_crt = static_library(
            'tests_sve_crt',
            sources: files('arithmetic/crt_builtins_sve.cpp'),
            build_by_default: false,
            include_directories: [framework_incdir],
            dependencies: [boost_dep, crt_builtins_lib_sve],
            cpp_args: [
                debug_c_flags,
                default_cpp_warn,
                default_cpp_flags,
                march_generic_flags,
                '-march=armv8.2-a+sve',
                sve_sanitize_exempt,
            ],
        )
        sandstone_tests += [tests_sve_crt]
    else
        message('libclang_rt.builtins not found — crt_builtins_sve not built (same gate as the arithmetic_arm original)')
    endif

    # IPSec domain (24 ports). Two tiers:
    # - HMAC/SHA family (17): encryption stays on OpenSSL EVP, the MAC runs
    #   on the multi-stream SVE HMAC layer (tests/cpu/sve/ipsec/sve_hmac.h
    #   over sve_sha_kernels.h — both verified bit-for-bit against OpenSSL,
    #   5200+1360 vectors, see scripts/sve-sha-verify). NSTREAM lanes: lane 0
    #   carries the original ciphertext message (MAC semantics identical to
    #   the avx-named original), lanes 1..N-1 carry deterministic key-stream
    #   variant messages that fill the remaining SVE lanes; goldens for all
    #   lanes precomputed in init.
    # - EVP-only family (7: GCM x2, CMAC/XCBC x5): the workload has no SHA
    #   component to move to SVE (CMAC/XCBC are AES-CBC-MAC chains inside
    #   OpenSSL; upstream has no SVE AES path), so these are faithful
    #   compute-preserving ports with honest descriptions.
    if framework_config.get('SANDSTONE_SSL_BUILD') == 1
        tests_sve_avx53_ipsec = static_library(
            'tests_sve_avx53_ipsec',
            sources: files(
                'ipsec/ipsec_3des_docsis_hmac_sha1_sve_wide.cpp',
                'ipsec/ipsec_3des_docsis_hmac_sha224_sve_wide.cpp',
                'ipsec/ipsec_3des_docsis_hmac_sha256_sve_wide.cpp',
                'ipsec/ipsec_3des_docsis_hmac_sha384_sve_wide.cpp',
                'ipsec/ipsec_3des_docsis_hmac_sha512_sve_wide.cpp',
                'ipsec/ipsec_aes128_cbc_hmac_sha1_sve.cpp',
                'ipsec/ipsec_aes192_ctr_hmac_sha1_sve.cpp',
                'ipsec/ipsec_aes192_ctr_hmac_sha224_sve.cpp',
                'ipsec/ipsec_aes192_ctr_hmac_sha256_sve.cpp',
                'ipsec/ipsec_aes192_ctr_hmac_sha384_sve.cpp',
                'ipsec/ipsec_aes192_ctr_hmac_sha512_sve.cpp',
                'ipsec/ipsec_aes256_cbc_hmac_sha1_sve.cpp',
                'ipsec/ipsec_aes128_cbc_sha2_224_sve.cpp',
                'ipsec/ipsec_aes192_cbc_sha2_384_sve.cpp',
                'ipsec/ipsec_aes192_cbc_sha2_512_sve.cpp',
                'ipsec/ipsec_aes256_cbc_sha2_224_sve.cpp',
                'ipsec/ipsec_aes256_cbc_sha2_256_sve.cpp',
                'ipsec/ipsec_aes192_gcm_sve.cpp',
                'ipsec/ipsec_aes192_gcm_sve_wide.cpp',
                'ipsec/ipsec_3des_docsis_aes_cmac_sve_wide.cpp',
                'ipsec/ipsec_3des_docsis_aes_xcbc_sve_wide.cpp',
                'ipsec/ipsec_aes128_cbc_aes_cmac_sve.cpp',
                'ipsec/ipsec_aes192_cbc_xcbc_96_sve.cpp',
                'ipsec/ipsec_aes192_ctr_xcbc_sve.cpp',
            ),
            build_by_default: false,
            include_directories: [
                framework_incdir,
            ],
            dependencies: [
                boost_dep,
                crypto_dep,
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
        sandstone_tests += [tests_sve_avx53_ipsec]
    endif

    tests_sve_avx53_lib = static_library(
        'tests_sve_avx53',
        sources: sve_avx53_sources,
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

    sandstone_tests += [
        tests_sve_avx53_lib,
    ]
endif
