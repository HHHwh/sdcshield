/*  ma_tail_probe.h — unified tail memory-access-pair probe (memory-access/
 *  family, 2026-11-04).
 *
 *  The user's measured fault signature: AFTER a load burst finishes, a
 *  back-to-back pair of stores (str -> str), or ldr immediately followed
 *  by two stores (ldr -> str -> str), can mis-place data on the failing
 *  core under pressure.  The rotating-ALU load head is already covered by
 *  the neon_rot family; THIS family sweeps OTHER load environments.
 *
 *  Every ma_* test has the same structure:
 *
 *      [LOAD HEAD]   the variable being swept (pure ldr scan, pure str
 *                    stream, copy alternation, dc-civac maintenance,
 *                    barriers, exclusive pairs, NT stores, fixed ALU)
 *      [TAIL PROBE]  immediately after the load, per 16-word segment:
 *                       mode A (str^2):   str temp[i] -> str dst[i]
 *                                          -> ldr dst[i] -> compare
 *                       mode B (ldr+str^2): ldr src[i] -> str temp[i]
 *                                          -> str dst[i] -> ldr dst[i]
 *                                          -> compare
 *                    All four accesses are consecutive inline asm —
 *                    the <=2-cycle store->load forwarding window is
 *                    structurally guaranteed, no barrier in between.
 *      [GOLDEN]      init-time snapshot of the random src; the probe's
 *                    writes must reproduce the source bits exactly, so a
 *                    mis-placed write lands on a slot whose golden differs
 *                    and the compare fires.
 *      [ANTI-FOLD]   the 16-word probe segment rotates across the 16 KiB
 *                    window every round, so stale forwarded data from a
 *                    previous round cannot satisfy the compare.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <sandstone.h>

#define MA_WORDS (16 * 1024 / 4)   /* 16 KiB window in uint32 words */
#define MA_SEG   16                /* probe segment: 16 words */

namespace ma_probe {

struct MAData {
    uint32_t src[MA_WORDS];     /* random source (init once)      */
    uint32_t temp[MA_WORDS];    /* first write-back of the pair   */
    uint32_t dst[MA_WORDS];     /* second write-back (back2back)  */
    uint32_t golden[MA_WORDS];  /* init snapshot of src           */
};

static inline void ma_init(MAData *d) {
    memset_random(d->src, sizeof(d->src));
    memcpy(d->golden, d->src, sizeof(d->src));
    memset(d->temp, 0, sizeof(d->temp));
    memset(d->dst, 0, sizeof(d->dst));
}

/* the tail access pair, all asm — consecutive, unbarriered */
static inline uint32_t ma_ldr(const uint32_t *a) {
    uint32_t v;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(a) : "memory");
    return v;
}
static inline void ma_str(uint32_t *a, uint32_t v) {
    __asm__ volatile("str %w0, [%1]" :: "r"(v), "r"(a) : "memory");
}

/* Mode A: load head ended -> str temp[i] -> str dst[i] -> ldr dst[i] */
static inline bool probe_str_str(MAData *d, uint32_t base, int i) {
    const uint32_t v = d->src[base + i];           /* value source */
    ma_str(&d->temp[base + i], v);                 /* first write  */
    ma_str(&d->dst[base + i], v);                  /* second write */
    const uint32_t back = ma_ldr(&d->dst[base + i]);
    return back == d->golden[base + i]
        && d->temp[base + i] == d->golden[base + i];
}

/* Mode B: load head ended -> ldr src[i] -> str temp[i] -> str dst[i]
 * -> ldr dst[i] — the ldr is immediately adjacent to the two stores */
static inline bool probe_ldr_str_str(MAData *d, uint32_t base, int i) {
    const uint32_t v = ma_ldr(&d->src[base + i]);  /* adjacent load */
    ma_str(&d->temp[base + i], v);
    ma_str(&d->dst[base + i], v);
    const uint32_t back = ma_ldr(&d->dst[base + i]);
    return back == d->golden[base + i]
        && d->temp[base + i] == d->golden[base + i];
}

} /* namespace ma_probe */
