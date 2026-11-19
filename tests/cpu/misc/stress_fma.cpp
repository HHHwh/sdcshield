/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b stress_fma
 * @parblock
 * Single-instruction pressure test for core-179 SDC: hot loop saturates the
 * scalar double-precision FMA instruction (`fmadd`) only. Same store-reload
 * load-path layout as the verified-to-trigger mrn_rmw_dump
 * (str res->temp[i]; dst[i]=ldr temp[i]), but every element computes
 * a*b + c via inline-asm FMADD instead of an integer ALU op — so the only
 * arithmetic in the hot loop is FMA.
 *
 * Purpose: isolate whether the FMA instruction itself triggers the 179
 * defect. Prior evidence: eigen_gemm (which emits fmadd/fmla/fadd/fmul)
 * triggers 179 SDC with mantissa-concentrated bit flips; this test removes
 * Eigen's inlining/interleaving and the fmul/fadd variants, running pure
 * fmadd so any trigger is attributable to fmadd alone (vs eigen's mixed
 * stream). "Does not trigger" is also information — it would mean eigen's
 * particular inlined schedule, not fmadd per se, is what hits 179.
 *
 * asm constraint note (critical, verified empirically): FMADD requires
 * scalar FP registers (d0-d31). gcc inline-asm constraints:
 *   - `"=r"` → general-purpose x register → assembler error
 *     "expected a scalar SIMD or floating-point register"
 *   - `"=w"` → vector v register → same assembler error (fmadd needs d, not v)
 *   - CORRECT: `"=w"` constraint + `%d0` modifier forces the scalar d alias
 *     of the allocated v register. Verified: `fmadd %d0,%d1,%d2,%d3`
 *     with `"=&w"` compiles and chains correctly (5-iter chain = 30.0).
 *   - `&` early-clobber: the output register must not alias input registers
 *     because the micro-op reads all inputs before writing output.
 *
 * On miscompare, logs inputA/B/C/golden/actual + IEEE-754 bit xor and
 * per-byte breakdown (log-only; the former per-miscompare /tmp record was
 * removed — nothing consumed the file and the log carries the same fields).
 * All logging in the cold failure branch; the FMA hot path is stable.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

#define STRESS_COUNT 1024

struct StressFmaData {
    double *srcA;    // mul1
    double *srcB;    // mul2
    double *srcC;    // accumulator (res = c + a*b)
    double *expected;
};

#if defined(__aarch64__)
static inline void store_double(void *addr, double val) {
    __asm__ volatile ("str %d1, [%0]" : : "r"(addr), "w"(val) : "memory");
}
static inline double load_double(const void *addr) {
    double res;
    __asm__ volatile ("ldr %d0, [%1]" : "=w"(res) : "r"(addr) : "memory");
    return res;
}
// Pure scalar double FMA: res = c + a*b.  %d modifier forces scalar d reg.
static inline double op_fmadd(double a, double b, double c) {
    double res;
    __asm__ volatile ("fmadd %d0, %d1, %d2, %d3" : "=&w"(res) : "w"(a), "w"(b), "w"(c));
    return res;
}
#else
static inline void store_double(void *addr, double val) {
    *reinterpret_cast<double*>(addr) = val;
}
static inline double load_double(const void *addr) {
    return *reinterpret_cast<const double*>(addr);
}
static inline double op_fmadd(double a, double b, double c) { return a * b + c; }
#endif

static int stress_fma_init(struct test *test) {
    auto *data = static_cast<StressFmaData *>(malloc(sizeof(StressFmaData)));
    data->srcA = static_cast<double *>(aligned_alloc_safe(64, STRESS_COUNT * sizeof(double)));
    data->srcB = static_cast<double *>(aligned_alloc_safe(64, STRESS_COUNT * sizeof(double)));
    data->srcC = static_cast<double *>(aligned_alloc_safe(64, STRESS_COUNT * sizeof(double)));
    data->expected = static_cast<double *>(aligned_alloc_safe(64, STRESS_COUNT * sizeof(double)));

    // Fill with random byte patterns -> random IEEE-754 doubles (incl denormals/NaN-ish bits).
    // Avoid generating actual NaN/Inf as that would mask arithmetic errors deterministically:
    // zero the exponent-all-ones patterns by masking exponent to 11 bits max usable.
    memset_random(data->srcA, STRESS_COUNT * sizeof(double));
    memset_random(data->srcB, STRESS_COUNT * sizeof(double));
    memset_random(data->srcC, STRESS_COUNT * sizeof(double));
    // Sanitize: clear the exponent-all-ones (0x7FF..) bits so no NaN/Inf inputs
    // (a finite input whose FMA is also finite keeps the miscompare meaningful).
    for (int i = 0; i < STRESS_COUNT; i++) {
        uint64_t *pa = reinterpret_cast<uint64_t*>(&data->srcA[i]);
        uint64_t *pb = reinterpret_cast<uint64_t*>(&data->srcB[i]);
        uint64_t *pc = reinterpret_cast<uint64_t*>(&data->srcC[i]);
        *pa &= ~(1ULL << 62 | 1ULL << 61);  // cap exponent top 2 bits → never all-ones
        *pb &= ~(1ULL << 62 | 1ULL << 61);
        *pc &= ~(1ULL << 62 | 1ULL << 61);
    }

    // Compute golden values using the same op (software FMA via the inline asm path
    // would be circular, so use plain C a*b+c — the compiler emits fmadd anyway on
    // aarch64, which is fine: golden is computed in init on a cold path, not the hot loop).
    for (int i = 0; i < STRESS_COUNT; i++) {
        data->expected[i] = data->srcA[i] * data->srcB[i] + data->srcC[i];
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int stress_fma_run(struct test *test, int cpu) {
    auto *data = static_cast<StressFmaData *>(test->data);
    (void)cpu;
    double *temp = static_cast<double *>(aligned_alloc_safe(64, STRESS_COUNT * sizeof(double)));
    double *dst  = static_cast<double *>(aligned_alloc_safe(64, STRESS_COUNT * sizeof(double)));

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < STRESS_COUNT; i++) {
            double a = data->srcA[i];
            double b = data->srcB[i];
            double c = data->srcC[i];
            double res = op_fmadd(a, b, c);    // the ONLY arithmetic instruction (pure fmadd)
            store_double(&temp[i], res);
            store_double(&dst[i], load_double(&temp[i]));
        }

        // Byte-exact compare (no tolerance) — a single mantissa bit flip fails.
        if (memcmp(dst, data->expected, STRESS_COUNT * sizeof(double)) != 0) {
            for (int i = 0; i < STRESS_COUNT; i++) {
                if (dst[i] != data->expected[i]) {
                    uint64_t golden_bits, actual_bits, a_bits, b_bits, c_bits;
                    memcpy(&golden_bits, &data->expected[i], sizeof(uint64_t));
                    memcpy(&actual_bits, &dst[i], sizeof(uint64_t));
                    memcpy(&a_bits, &data->srcA[i], sizeof(uint64_t));
                    memcpy(&b_bits, &data->srcB[i], sizeof(uint64_t));
                    memcpy(&c_bits, &data->srcC[i], sizeof(uint64_t));
                    uint64_t xorv = golden_bits ^ actual_bits;
                    unsigned char *gb = (unsigned char *)&golden_bits;
                    unsigned char *ab = (unsigned char *)&actual_bits;
                    unsigned char *xb = (unsigned char *)&xorv;
                    log_error("stress_fma: miscompare at index %u: "
                              "inputA=0x%016lX inputB=0x%016lX inputC=0x%016lX "
                              "golden=0x%016lX actual=0x%016lX xor=0x%016lX (flipped bit(s)) | "
                              "bytes[b7..b0] golden=%02X%02X%02X%02X%02X%02X%02X%02X "
                              "actual=%02X%02X%02X%02X%02X%02X%02X%02X "
                              "xor=%02X%02X%02X%02X%02X%02X%02X%02X",
                              (unsigned)i,
                              (unsigned long)a_bits, (unsigned long)b_bits, (unsigned long)c_bits,
                              (unsigned long)golden_bits, (unsigned long)actual_bits, (unsigned long)xorv,
                              gb[7], gb[6], gb[5], gb[4], gb[3], gb[2], gb[1], gb[0],
                              ab[7], ab[6], ab[5], ab[4], ab[3], ab[2], ab[1], ab[0],
                              xb[7], xb[6], xb[5], xb[4], xb[3], xb[2], xb[1], xb[0]);

                    report_fail_msg("stress_fma: data miscompare at index %u", (unsigned)i);
                }
            }
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int stress_fma_cleanup(struct test *test) {
    auto *data = static_cast<StressFmaData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->srcC);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(stress_fma, "single-instruction pressure test: hot loop saturates scalar double-precision FMA (fmadd) only (mrn_rmw store-reload layout, pure fmadd) — isolates whether FMA triggers core-179 SDC")
    .test_init = stress_fma_init,
    .test_run = stress_fma_run,
    .test_cleanup = stress_fma_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
