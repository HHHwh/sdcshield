/*  ma_tail_probe_fp64.h — unified tail memory-access-pair probe, FP64
 *  datapath variant (tests/cpu/fp64/, 2026-11-10).
 *
 *  FP64 variant of tests/cpu/memory-access/ma_tail_probe.h (parent
 *  untouched).  Same family structure — [LOAD HEAD] + [TAIL PROBE]
 *  (str->str / ldr->str->str back-to-back asm, <=2-cycle store->load
 *  forwarding window) + [GOLDEN] (init snapshot) + [ANTI-FOLD]
 *  (rotating 16-element segment) — with the payload re-typed to the
 *  FP64 datapath: the tail accesses are 64-bit FP-register asm
 *  (ldr/str on d registers) instead of 32-bit GPR (ldr/str %w),
 *  so the forwarding window under probe runs through the FP store
 *  port / FP register file.
 *
 *  Window keeps the parent's 16 KiB footprint (2048 f64 words);
 *  the segment keeps 16 ELEMENTS (its byte width naturally doubles
 *  from 64B to 128B).  Golden compare is BIT-EXACT (u64 lane compare
 *  via memcpy), NOT double ==: random bit patterns contain NaNs and
 *  NaN != NaN would false-fail a numerical compare — the parent's
 *  bit-precision discipline is preserved exactly.
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <sandstone.h>

#define MA_F64_WORDS (16 * 1024 / 8)   /* 16 KiB window in f64 words */
#define MA_F64_SEG   16                /* probe segment: 16 elements */

namespace ma_probe_f64 {

struct MAF64Data {
    double src[MA_F64_WORDS];     /* random source (init once)      */
    double temp[MA_F64_WORDS];    /* first write-back of the pair   */
    double dst[MA_F64_WORDS];     /* second write-back (back2back)  */
    double golden[MA_F64_WORDS];  /* init snapshot of src           */
};

static inline void ma_init_f64(MAF64Data *d) {
    memset_random(d->src, sizeof(d->src));
    memcpy(d->golden, d->src, sizeof(d->src));
    memset(d->temp, 0, sizeof(d->temp));
    memset(d->dst, 0, sizeof(d->dst));
}

/* bit-exact equality on f64 lanes (NaN-safe, parent's precision) */
static inline bool f64_bits_eq(double a, double b) {
    uint64_t ua, ub;
    memcpy(&ua, &a, sizeof(ua));
    memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
}

static inline uint64_t f64_bits(double d) {
    uint64_t u;
    memcpy(&u, &d, sizeof(u));
    return u;
}

/* the tail access pair, all asm — consecutive, unbarriered, d regs */
static inline double ma_ldr_f64(const double *a) {
    double v;
    __asm__ volatile("ldr %d0, [%1]" : "=w"(v) : "r"(a) : "memory");
    return v;
}
static inline void ma_str_f64(double *a, double v) {
    __asm__ volatile("str %d0, [%1]" :: "w"(v), "r"(a) : "memory");
}

/* Mode A: load head ended -> str temp[i] -> str dst[i] -> ldr dst[i] */
static inline bool probe_str_str_f64(MAF64Data *d, uint32_t base, int i) {
    const double v = d->src[base + i];           /* value source */
    ma_str_f64(&d->temp[base + i], v);           /* first write  */
    ma_str_f64(&d->dst[base + i], v);            /* second write */
    const double back = ma_ldr_f64(&d->dst[base + i]);
    return f64_bits_eq(back, d->golden[base + i])
        && f64_bits_eq(d->temp[base + i], d->golden[base + i]);
}

/* Mode B: load head ended -> ldr src[i] -> str temp[i] -> str dst[i]
 * -> ldr dst[i] — the ldr is immediately adjacent to the two stores */
static inline bool probe_ldr_str_str_f64(MAF64Data *d, uint32_t base, int i) {
    const double v = ma_ldr_f64(&d->src[base + i]);  /* adjacent load */
    ma_str_f64(&d->temp[base + i], v);
    ma_str_f64(&d->dst[base + i], v);
    const double back = ma_ldr_f64(&d->dst[base + i]);
    return f64_bits_eq(back, d->golden[base + i])
        && f64_bits_eq(d->temp[base + i], d->golden[base + i]);
}

} /* namespace ma_probe_f64 */
