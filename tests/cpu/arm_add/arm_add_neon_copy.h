/*  arm_add_neon_copy.h — explicit NEON copy loop for the _neon variants.
 *
 *  The _neon variants of the cache/memory tests replace their libc
 *  `memcpy()` load points with this helper: a direct vld1q_u8/vst1q_u8
 *  stream (NEON load/store take unaligned addresses, so any offset /
 *  random-length copy keeps working) plus a scalar tail for n % 16.
 *  Rationale: bypass the libc dispatcher and emit a dense, predictable
 *  NEON instruction stream — the switching-power band the excitation
 *  work targets (libc memcpy may also pick SVE or LDP/STP paths
 *  depending on the runtime IFUNC resolution, which would blur the
 *  "NEON datapath" attribution of these variants).
 */
#pragma once

#include <arm_neon.h>
#include <cstddef>
#include <cstdint>

namespace arm_add_neon {

static inline void neon_copy(void *dst, const void *src, size_t n) {
    uint8_t *d = static_cast<uint8_t *>(dst);
    const uint8_t *s = static_cast<const uint8_t *>(src);
    const size_t v = n / 16;
    for (size_t i = 0; i < v; ++i)
        vst1q_u8(d + i * 16, vld1q_u8(s + i * 16));
    for (size_t i = v * 16; i < n; ++i)
        d[i] = s[i];
}

} /* namespace arm_add_neon */
