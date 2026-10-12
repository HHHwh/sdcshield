#!/usr/bin/perl
# Consolidated quantitative evidence for the CORE145 LSU-hypothesis section.
# All numbers here are computed from /tmp/hpc_log.txt by the scripts listed.
print <<"EOT";
[1] FAIL9 U col234 (LCG:300065116, S4 10:49:35Z):
    - 600/600 halves: actual = golden x 2^-15 EXACTLY (f8divisor2.pl)
    - exponent fields: actual exp == golden exp - 15 for ALL 600 (ok=600 bad=0)
    - golden exp&0x11 classes: 0x10:307, 0x11:292, 0x01:1 (f9expdiv2.pl)
      -> a FIXED per-element xor of bits 52/56 would give +17 (not -15) for
         the 292 elements in class 0x11; observed is uniform -15 for both
         classes => the corruption is an ARITHMETIC division by 2^15 applied
         to the whole column (the normalize() divisor sqrt(z), single double),
         not per-element bit flips at the store/load boundary.
    - golden col234 norm structure: z=0.226529..., max |x_i|^2 share = 7.76%
      (f9norm.pl) => corrupting ONE element (any single element, by any
      mechanism incl. LSU load forwarding) changes z by <= 7.8%, i.e.
      |k-1| <= 0.078. Observed k = 2^-15 = 3.05e-5. => single-element
      corruption CANNOT produce FAIL9. Only a whole-column scalar can.
    - z exponent = 0x3fc = 01111111100b; exp-30 = 0x3de = 01111011110b;
      XOR = 0x022 (2 bits). A 2-bit flip in z's exponent field (the
      squaredNorm accumulator, a single double held in a register across
      the 600-element reduction) deflates z by exactly 2^30 => k = 2^-15
      with mantissa preservation. This is a REGISTER-storage bit flip.
[2] FAIL8 (same seed LCG:300065116, 10:49:25Z, 10 s before FAIL9):
    - col38 k = 24145.4913385 const to 2.67e-10 over 598 halves;
      col39 k = 0.248376594746 const to 2.13e-10; col11 k = 5997.175
      (lsu_divisor3.pl). k38*k39/k11 = 0.99999996.
    - None of the k's is a power of two (log2 frac 0.559/0.991/0.550)
      => mantissa-corrupting event; ratios constant across 598 elements
      computed in 598 separate loop iterations => single upstream scalar
      (same normalize-divisor position), NOT 598 independent load
      corruptions.
[3] Run-side vs golden-side (fork model, -Y -F):
    - All 12 failures attributed to the GOLDEN side (test_init period;
      loop-count = 0 for all 12; crash also loop-count 0, ttf 1000.572 ms).
    - P(12/12 on one side | unbiased persistent LSU datapath fault) =
      2^-12 = 1/4096 = 0.024% (lsu_sides.pl).
    - The run side executes the IDENTICAL instruction sequence and memory
      traffic (calculate_once on the same orig_matrix) => a persistent
      load/store datapath fault has no mechanism to prefer the init pass.
      A PRF/storage-cell fault CAN prefer init: init runs immediately
      after fork+first-touch (cold caches, different DVFS/thermal state).
[4] Test-coverage on the SAME core 145 in the same window (10:41-10:51):
    eigen_gemm 7 variants: 299 pass / 0 fail (memory-streaming, the most
    load/store-intensive workload class in the suite);
    eigen_sparse: 9 pass / 0 fail (indirect-addressed scatter/gather,
    the heaviest LSU-pressure pattern in the suite);
    eigen_svd family (NEON/noavx512/real): 167 pass / 0 fail;
    eigen_svd_cdouble_sve: 41 pass / 12 fail / 1 crash.
    (lsu_gemmcount.pl)
EOT
