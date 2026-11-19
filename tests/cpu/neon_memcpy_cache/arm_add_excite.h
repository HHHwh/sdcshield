/*  arm_add_excite.h — shared NEON/memory excitation filler for arm_add.
 *
 *  Why: the seven arm_add probes verify memory-ordering semantics with
 *  wait-heavy instruction mixes (barrier stalls, exclusive-pair spins,
 *  cross-core handshakes) — measured socket-4 power was ~0 W above idle
 *  while neon_rot_ldr_at_top_rowmajor pulls +54 W / +16 C.  Excitation
 *  on core 179 needs switching power, whose dominant term is cross-level
 *  MEMORY TRAFFIC: neon_rot walks a 96 KB working set (6 x 16 KB arrays,
 *  bigger than the 64 KB L1D) with a streamed 3-load/2-store loop and
 *  independent iterations (maximum memory-level parallelism).  Two
 *  drafts that failed first, kept here as calibration history:
 *    v1: register dependency chain + one 64 B line — ~0 W (stalls do
 *        not burn power);
 *    v2: streamed 8 KB stack window — +6 W max (the whole window sits
 *        inside L1D, nothing crosses levels).
 *  This v3 copies the neon_rot SHAPE:
 *
 *    window : 4 arrays x 32 KB (srcA/srcB/scratch/dst) = 128 KB total
 *             footprint, lazily heap-allocated per worker thread
 *             (thread_local in an inline function — one instance per
 *             worker across all TUs including this header).
 *    pass 1 : streamed init of srcA/srcB from an evolved pattern
 *             (32 KB + 32 KB stores crossing L1).
 *    pass 2 : the neon_rot loop shape — load srcA[i] + srcB[i] +
 *             scratch[i] (the scratch load is deliberately unused, it
 *             exists purely as a third stream, exactly like
 *             neon_rot's unused_x), dual-source compute (ADD/XOR/MUL
 *             — the MUL link is for multiplier-array toggle), store
 *             dst[i] + scratch[i]; a register-resident XOR checksum
 *             accumulates in flight.
 *    pass 3 : streamed reload of dst accumulating a second checksum;
 *             mismatch vs pass 2 = a store/load-path SDC and the
 *             caller MUST report it (probe, not just a heater).
 *
 *    evolve : pass-1 pattern derives from (cpu, round), so a stale line
 *             cannot hide behind a golden fold (issue #374 lineage).
 *    scope  : the window is thread_local filler-private — it never
 *             touches the calling test's shared data, safe under any
 *             concurrency protocol.
 *
 *  Traffic per call: 64 KB init stores + 96 KB loads + 64 KB stores +
 *  32 KB verify loads = ~256 KB across a 128 KB footprint.
 *  Calibrated 2026-11-02 against neon_rot on socket 4
 *  (ipmitool sdr get Power CPU4_TEMP).
 */
#pragma once

#include <arm_neon.h>
#include <cstdint>
#include <cstdlib>

namespace arm_add_excite {

#ifndef ARM_ADD_EXCITE_VECS
#define ARM_ADD_EXCITE_VECS 2048   /* per array: 2048 x 16B = 32 KB */
#endif

/* One excitation round.  Returns true when the pass-3 reload checksum
 * matches the pass-2 in-flight checksum; false = SDC, caller reports. */
static inline bool fill(unsigned cpu, uint64_t round) {
    constexpr int N = ARM_ADD_EXCITE_VECS;

    /* 128 KB filler-private window, one per worker thread */
    static thread_local uint32_t *srcA = nullptr;
    static thread_local uint32_t *srcB = nullptr;
    static thread_local uint32_t *scratch = nullptr;
    static thread_local uint32_t *dst = nullptr;
    if (srcA == nullptr) {
        srcA    = static_cast<uint32_t *>(aligned_alloc(64, N * 16));
        srcB    = static_cast<uint32_t *>(aligned_alloc(64, N * 16));
        scratch = static_cast<uint32_t *>(aligned_alloc(64, N * 16));
        dst     = static_cast<uint32_t *>(aligned_alloc(64, N * 16));
        for (int i = 0; i < N; ++i) {          /* one-time scratch init */
            vst1q_u32(&scratch[i * 4],
                      vdupq_n_u32((uint32_t)(i * 0x85EBu + 7u)));
        }
    }

    const uint32x4_t k0 = {0x9E3779B9u, 0x85EBCA6Bu, 0xC2B2AE35u, 0x27D4EB2Fu};
    const uint32x4_t k1 = vdupq_n_u32(0x2545F491u);

    /* pass 1: streamed init of the two sources (evolved per round) */
    for (int i = 0; i < N; ++i) {
        vst1q_u32(&srcA[i * 4],
                  veorq_u32(k0, vdupq_n_u32((uint32_t)(i * 0x9E37u
                                                    + (unsigned)round))));
        vst1q_u32(&srcB[i * 4],
                  vdupq_n_u32((uint32_t)((round << 13) ^ (uint64_t)cpu
                                         ^ (uint64_t)i * 0x2545F491u)));
    }

    /* pass 2: the neon_rot loop shape — 3 streams in, 2 out, MUL link */
    uint32x4_t csum = vdupq_n_u32((uint32_t)round ^ (uint64_t)cpu);
    for (int i = 0; i < N; ++i) {
        uint32x4_t unused_x = vld1q_u32(&scratch[i * 4]);
        (void)unused_x;                       /* third stream, neon_rot's unused_x */
        uint32x4_t a = vld1q_u32(&srcA[i * 4]);
        uint32x4_t b = vld1q_u32(&srcB[i * 4]);
        uint32x4_t r = veorq_u32(vaddq_u32(a, b), vmulq_u32(b, k1));
        vst1q_u32(&dst[i * 4], r);
        vst1q_u32(&scratch[i * 4], r);        /* second store stream */
        csum = veorq_u32(csum, r);
    }

    /* pass 3: streamed reload of dst, second checksum, full compare */
    uint32x4_t csum2 = vdupq_n_u32((uint32_t)round ^ (uint64_t)cpu);
    for (int i = 0; i < N; ++i) {
        uint32x4_t v = vld1q_u32(&dst[i * 4]);
        csum2 = veorq_u32(csum2, v);
    }

    return vmaxvq_u32(veorq_u32(csum, csum2)) == 0;
}

} /* namespace arm_add_excite */
