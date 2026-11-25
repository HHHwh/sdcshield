/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b adaptive_ldp_stp
 * @parblock
 * Adaptive LDP/STP-vs-LDR/STR copy-engine SDC test (2026-11-24).
 * Chassis: tests/cpu/misc/adaptive_mem_sdc.cpp (shm wall-clock anchor,
 * fork_each_test per-child combo pick, verify round-robin, covfile,
 * combat/test modes); combo space swapped for the fmov copy family:
 *
 *   axis 1  instruction kind : ldp/stp PAIRS first, then ldr/str
 *            singles ("widest SIMD is most SDC-prone" axiom — the
 *            post-indexed pair form moves 32B/instr vs 16B)
 *   axis 2  load count       : 2 > 3 > 4   (2-source is the proven
 *            179 recipe; chained sources R = C(C(a,b),c)...)
 *   axis 3  store count      : 2 > 3 > 4   (temp -> junk* -> dst, the
 *            compared dst is ALWAYS the last store target)
 *   axis 4  consumer (middle load, 2026-11-24 user directive: only
 *            this batch's test cases): ALU rotation (p05 chassis
 *            add/eor/and/orr, j%4 slot) > EXT #8 cross-lane (p13) >
 *            pure copy (p08; extra sources stay as asm-volatile dead
 *            loads — port pressure, the ldr_at_top lineage semantics)
 *
 * Combo id arithmetic (priority order == id order, 54 combos):
 *   id = ((kind*3 + (loads-2))*3 + (stores-2))*3 + consumer
 *
 * Every combo body is a compile-time instantiation (C++ template over
 * <Kind, L, S, C>; the load/store asm blocks are per-arity literal
 * helpers, so each combo's back-to-back LDP/STP/LDR/STR train is
 * unrolled and post-index-pinned in the binary).  Buffers are
 * per-child (fork_each_test re-inits each child, auto seed): L 16KB
 * random sources + S 16KB targets + expected; the golden is a PURE
 * SCALAR word-level replica (issue #373 paradigm,
 * scripts/check-scalar-golden.sh gated on ald_scalar_golden).
 *
 * Env contract (campaign script, mirrors adaptive_mem_sdc):
 *   ADAPTIVE_LDP_STP_KEY        shm name suffix (fresh key = fresh
 *                               anchor; KEY'd objects are the campaign
 *                               script's to clean; the default
 *                               ppid-derived objects are reaped by a
 *                               later run's children when their parent
 *                               process is dead)
 *   ADAPTIVE_LDP_STP_VERIFY=1   verify mode: round-robin counter,
 *                               each child takes the next combo (a
 *                               short -T run covers all 54 goldens)
 *   ADAPTIVE_LDP_STP_COVFILE    per-child trace, one line per run:
 *                               "<iso> <pid> <mode> <combo>"
 *   ADAPTIVE_LDP_STP_COMBO      lock every child to this combo id
 *   ADAPTIVE_LDP_STP_MODE       combat (default: 30min wall-clock
 *                               slice per combo in priority order,
 *                               first SDC freezes later children to
 *                               idle) | test (survey: every combo one
 *                               TEST_SLICE window, detections reported
 *                               but NOT frozen, idle after the last
 *                               slot)
 *   ADAPTIVE_LDP_STP_TEST_SLICE  test-mode slice seconds (default 600)
 *
 * On SDC the failing child tags report_fail_msg with the combo
 * identity and sets the shm flag; the campaign script SIGKILLs the
 * main process on the first fail block (the framework itself never
 * exits on fail).
 * @endparblock
 */

#include "sandstone.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>

#if defined(__aarch64__)
#include <arm_neon.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/mman.h>
#endif

#define ALD_COMBO_COUNT 54
#define ALD_SLICE_SECONDS 1800            /* 30min combat slice */
#define ALD_COUNT 1024                    /* elements per buffer, family convention */
#define ALD_BYTES (ALD_COUNT * 16)        /* 16KB per buffer */

/* kind / consumer enums; combo id arithmetic (priority == id order) */
enum { ALD_K_PAIR = 0, ALD_K_SINGLE = 1 };
enum { ALD_C_ALUROT = 0, ALD_C_EXT = 1, ALD_C_COPY = 2 };

static inline void ald_combo_unpack(int id, int *kind, int *loads, int *stores, int *consumer)
{
    *consumer = id % 3; id /= 3;
    *stores = id % 3 + 2; id /= 3;
    *loads = id % 3 + 2; id /= 3;
    *kind = id;
}

#if defined(__aarch64__)

/* ------------------------------------------------------------------ */
/* shm machinery (mirrors adaptive_mem_sdc: O_EXCL creator publishes   */
/* the CLOCK_MONOTONIC anchor, latecomers spin on ready)               */
/* ------------------------------------------------------------------ */

struct AdaptiveLdpStpShm {
    struct timespec anchor;               /* wall-clock anchor */
    uint32_t ready;                       /* creator publish flag */
    uint32_t verify_ctr;                  /* verify-mode round-robin */
    uint32_t sdc_flag;                    /* set by any failing child */
};

/* Stale-object reaper (2026-11-24 review fix I-1; Linux-only cold path).
 * The default ppid-derived name deliberately outlives every child — the
 * cross-child anchor IS the sharing mechanism — so nothing in-process
 * can unlink it while the campaign lives, and the framework never runs
 * test code in the main process.  Instead each child reaps objects whose
 * parent process is demonstrably dead: a live campaign's object has a
 * live main pid (kill(pid,0)==0) and is skipped; a dead campaign's
 * object is unlinked here.  KEY'd objects are exempt — they follow the
 * campaign script's fresh-key discipline and are that script's to
 * clean.  pid-reuse fails open (skip): worst case the object lingers
 * until reboot or a manual rm. */
static void ald_shm_reap_stale(void)
{
    if (getenv("ADAPTIVE_LDP_STP_KEY") != nullptr)
        return;
    DIR *dir = opendir("/dev/shm");
    if (dir == nullptr)
        return;
    static const char prefix[] = "adaptive_ldp_stp_ppid";
    const size_t plen = sizeof(prefix) - 1;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, prefix, plen) != 0)
            continue;
        char *end = nullptr;
        const long pid = strtol(ent->d_name + plen, &end, 10);
        if (end == ent->d_name + plen || *end != '\0' || pid <= 1)
            continue;
        if (kill((pid_t)pid, 0) == 0 || errno != ESRCH)
            continue;               /* alive, or not provably dead: skip */
        char path[9 + 256 + 1];       /* "/dev/shm/" + NAME_MAX + NUL */
        snprintf(path, sizeof(path), "/dev/shm/%s", ent->d_name);
        if (unlink(path) == 0)
            fprintf(stderr, "adaptive_ldp_stp: reaped stale shm object %s "
                    "(parent process %ld is gone)\n", ent->d_name, pid);
    }
    closedir(dir);
}

static struct AdaptiveLdpStpShm *ald_shm_setup(void)
{
    ald_shm_reap_stale();

    const char *key = getenv("ADAPTIVE_LDP_STP_KEY");
    char name[128];
    if (key) {
        snprintf(name, sizeof(name), "/adaptive_ldp_stp_%s", key);
    } else {
        /* no key: derive from the main process pid so every child of
         * one run shares the name; a later ad-hoc run gets a fresh
         * object instead of reusing a stale anchor/sdc_flag */
        snprintf(name, sizeof(name), "/adaptive_ldp_stp_ppid%ld", (long)getppid());
    }

    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    bool creator = (fd >= 0);
    if (!creator) {
        if (errno != EEXIST)
            return nullptr;
        fd = shm_open(name, O_RDWR, 0600);
        if (fd < 0)
            return nullptr;
    }

    if (creator && ftruncate(fd, sizeof(struct AdaptiveLdpStpShm)) != 0) {
        close(fd);
        return nullptr;
    }

    auto *shm = static_cast<struct AdaptiveLdpStpShm *>(
            mmap(nullptr, sizeof(struct AdaptiveLdpStpShm), PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd, 0));
    close(fd);
    if (shm == MAP_FAILED)
        return nullptr;

    if (creator) {
        memset(shm, 0, sizeof(*shm));
        clock_gettime(CLOCK_MONOTONIC, &shm->anchor);
        __atomic_store_n(&shm->ready, 1u, __ATOMIC_RELEASE);
    } else {
        /* bounded spin: creator publishes within microseconds of our EEXIST */
        for (int i = 0; i < 200000; i++) {
            if (__atomic_load_n(&shm->ready, __ATOMIC_ACQUIRE) == 1u)
                break;
            usleep(10);
        }
    }
    return shm;
}

static int64_t ald_seconds_since(const struct timespec *anchor, struct timespec *now)
{
    clock_gettime(CLOCK_MONOTONIC, now);
    int64_t s = (int64_t)(now->tv_sec - anchor->tv_sec);
    if (now->tv_nsec < anchor->tv_nsec)
        s -= 1;
    return s < 0 ? 0 : s;
}

/* Pick this child's combo; -1 = idle (post-SDC in combat mode, survey
 * complete in test mode, or shm unusable). */
static int ald_pick_combo(struct AdaptiveLdpStpShm *shm, int test_mode, int test_slice)
{
    if (!shm)
        return -1;
    if (!test_mode &&
        __atomic_load_n(&shm->sdc_flag, __ATOMIC_ACQUIRE) != 0)
        return -1;
    if (__atomic_load_n(&shm->ready, __ATOMIC_ACQUIRE) != 1u)
        return -1;

    /* confirmation/campaign hook: lock every child to one combo */
    const char *force = getenv("ADAPTIVE_LDP_STP_COMBO");
    if (force != nullptr) {
        int c = atoi(force);
        if (c >= 0 && c < ALD_COMBO_COUNT)
            return c;
    }

    if (getenv("ADAPTIVE_LDP_STP_VERIFY") != nullptr) {
        uint32_t prev = __atomic_fetch_add(&shm->verify_ctr, 1u, __ATOMIC_RELAXED);
        return (int)(prev % ALD_COMBO_COUNT);
    }

    struct timespec now;
    if (test_mode) {
        /* survey: every combo gets exactly one test_slice window, in
         * id (= priority) order; after the last slot children idle */
        int64_t elapsed = ald_seconds_since(&shm->anchor, &now);
        int64_t idx = elapsed / test_slice;
        if (idx >= ALD_COMBO_COUNT)
            return -1;
        return (int)idx;
    }

    int64_t elapsed = ald_seconds_since(&shm->anchor, &now);
    int64_t idx = elapsed / ALD_SLICE_SECONDS;
    if (idx > ALD_COMBO_COUNT - 1)
        idx = ALD_COMBO_COUNT - 1;
    return (int)idx;
}

/* one line per child run: "<iso> <pid> <mode> <combo>" */
static void ald_covfile_trace(int test_mode, int combo)
{
    const char *path = getenv("ADAPTIVE_LDP_STP_COVFILE");
    if (!path)
        return;
    char line[160];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    char iso[32];
    strftime(iso, sizeof(iso), "%FT%T", &tm);
    snprintf(line, sizeof(line), "%s %d %s %d\n", iso, (int)getpid(),
             getenv("ADAPTIVE_LDP_STP_VERIFY") ? "verify"
                 : test_mode ? "test" : "normal",
             combo);
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0)
        return;
    (void)!write(fd, line, strlen(line));
    close(fd);
}

/* combo tag for reports: "combo#<id> <kind> l<L> s<S> <consumer>" */
static void ald_combo_tag(int id, char *buf, size_t n)
{
    int kind, loads, stores, consumer;
    ald_combo_unpack(id, &kind, &loads, &stores, &consumer);
    snprintf(buf, n, "combo#%d %s l%d s%d %s", id,
             kind == ALD_K_PAIR ? "ldp/stp" : "ldr/str",
             loads, stores,
             consumer == ALD_C_ALUROT ? "alurot"
                 : consumer == ALD_C_EXT ? "ext" : "copy");
}

/* ------------------------------------------------------------------ */
/* asm-pinned load/store trains, per arity (post-indexed forms)       */
/* ------------------------------------------------------------------ */

/* pair loads: L ldp lines back-to-back, 2L vector outputs */
static inline void ald_pair_ld2(uint64x2_t &a0, uint64x2_t &a1,
                                uint64x2_t &b0, uint64x2_t &b1,
                                const uint64_t *&p0, const uint64_t *&p1)
{
    __asm__ volatile(
        "ldp  %q0, %q1, [%4], #32\n\t"
        "ldp  %q2, %q3, [%5], #32\n\t"
        : "=w"(a0), "=w"(a1), "=w"(b0), "=w"(b1), "+r"(p0), "+r"(p1)
        :
        : "memory");
}

static inline void ald_pair_ld3(uint64x2_t &a0, uint64x2_t &a1,
                                uint64x2_t &b0, uint64x2_t &b1,
                                uint64x2_t &c0, uint64x2_t &c1,
                                const uint64_t *&p0, const uint64_t *&p1,
                                const uint64_t *&p2)
{
    __asm__ volatile(
        "ldp  %q0, %q1, [%6], #32\n\t"
        "ldp  %q2, %q3, [%7], #32\n\t"
        "ldp  %q4, %q5, [%8], #32\n\t"
        : "=w"(a0), "=w"(a1), "=w"(b0), "=w"(b1), "=w"(c0), "=w"(c1),
          "+r"(p0), "+r"(p1), "+r"(p2)
        :
        : "memory");
}

static inline void ald_pair_ld4(uint64x2_t &a0, uint64x2_t &a1,
                                uint64x2_t &b0, uint64x2_t &b1,
                                uint64x2_t &c0, uint64x2_t &c1,
                                uint64x2_t &d0, uint64x2_t &d1,
                                const uint64_t *&p0, const uint64_t *&p1,
                                const uint64_t *&p2, const uint64_t *&p3)
{
    __asm__ volatile(
        "ldp  %q0, %q1, [%8], #32\n\t"
        "ldp  %q2, %q3, [%9], #32\n\t"
        "ldp  %q4, %q5, [%10], #32\n\t"
        "ldp  %q6, %q7, [%11], #32\n\t"
        : "=w"(a0), "=w"(a1), "=w"(b0), "=w"(b1), "=w"(c0), "=w"(c1),
          "=w"(d0), "=w"(d1),
          "+r"(p0), "+r"(p1), "+r"(p2), "+r"(p3)
        :
        : "memory");
}

/* pair stores: S stp lines back-to-back, same result pair to each
 * target; the compared dst is the LAST target (temp -> junk* -> dst) */
static inline void ald_pair_st2(const uint64x2_t &r0, const uint64x2_t &r1,
                                uint64_t *&t0, uint64_t *&t1)
{
    __asm__ volatile(
        "stp  %q2, %q3, [%0], #32\n\t"
        "stp  %q2, %q3, [%1], #32\n\t"
        : "+r"(t0), "+r"(t1)
        : "w"(r0), "w"(r1)
        : "memory");
}

static inline void ald_pair_st3(const uint64x2_t &r0, const uint64x2_t &r1,
                                uint64_t *&t0, uint64_t *&t1, uint64_t *&t2)
{
    __asm__ volatile(
        "stp  %q3, %q4, [%0], #32\n\t"
        "stp  %q3, %q4, [%1], #32\n\t"
        "stp  %q3, %q4, [%2], #32\n\t"
        : "+r"(t0), "+r"(t1), "+r"(t2)
        : "w"(r0), "w"(r1)
        : "memory");
}

static inline void ald_pair_st4(const uint64x2_t &r0, const uint64x2_t &r1,
                                uint64_t *&t0, uint64_t *&t1,
                                uint64_t *&t2, uint64_t *&t3)
{
    __asm__ volatile(
        "stp  %q4, %q5, [%0], #32\n\t"
        "stp  %q4, %q5, [%1], #32\n\t"
        "stp  %q4, %q5, [%2], #32\n\t"
        "stp  %q4, %q5, [%3], #32\n\t"
        : "+r"(t0), "+r"(t1), "+r"(t2), "+r"(t3)
        : "w"(r0), "w"(r1)
        : "memory");
}

/* single loads: L ldr lines back-to-back */
static inline void ald_ld2(uint64x2_t &a, uint64x2_t &b,
                           const uint64_t *&p0, const uint64_t *&p1)
{
    __asm__ volatile(
        "ldr  %q0, [%2], #16\n\t"
        "ldr  %q1, [%3], #16\n\t"
        : "=w"(a), "=w"(b), "+r"(p0), "+r"(p1)
        :
        : "memory");
}

static inline void ald_ld3(uint64x2_t &a, uint64x2_t &b, uint64x2_t &c,
                           const uint64_t *&p0, const uint64_t *&p1,
                           const uint64_t *&p2)
{
    __asm__ volatile(
        "ldr  %q0, [%3], #16\n\t"
        "ldr  %q1, [%4], #16\n\t"
        "ldr  %q2, [%5], #16\n\t"
        : "=w"(a), "=w"(b), "=w"(c), "+r"(p0), "+r"(p1), "+r"(p2)
        :
        : "memory");
}

static inline void ald_ld4(uint64x2_t &a, uint64x2_t &b, uint64x2_t &c,
                           uint64x2_t &d,
                           const uint64_t *&p0, const uint64_t *&p1,
                           const uint64_t *&p2, const uint64_t *&p3)
{
    __asm__ volatile(
        "ldr  %q0, [%4], #16\n\t"
        "ldr  %q1, [%5], #16\n\t"
        "ldr  %q2, [%6], #16\n\t"
        "ldr  %q3, [%7], #16\n\t"
        : "=w"(a), "=w"(b), "=w"(c), "=w"(d),
          "+r"(p0), "+r"(p1), "+r"(p2), "+r"(p3)
        :
        : "memory");
}

/* single stores: S str lines back-to-back, same result to each target */
static inline void ald_st2(const uint64x2_t &r, uint64_t *&t0, uint64_t *&t1)
{
    __asm__ volatile(
        "str  %q2, [%0], #16\n\t"
        "str  %q2, [%1], #16\n\t"
        : "+r"(t0), "+r"(t1)
        : "w"(r)
        : "memory");
}

static inline void ald_st3(const uint64x2_t &r, uint64_t *&t0, uint64_t *&t1,
                           uint64_t *&t2)
{
    __asm__ volatile(
        "str  %q3, [%0], #16\n\t"
        "str  %q3, [%1], #16\n\t"
        "str  %q3, [%2], #16\n\t"
        : "+r"(t0), "+r"(t1), "+r"(t2)
        : "w"(r)
        : "memory");
}

static inline void ald_st4(const uint64x2_t &r, uint64_t *&t0, uint64_t *&t1,
                           uint64_t *&t2, uint64_t *&t3)
{
    __asm__ volatile(
        "str  %q4, [%0], #16\n\t"
        "str  %q4, [%1], #16\n\t"
        "str  %q4, [%2], #16\n\t"
        "str  %q4, [%3], #16\n\t"
        : "+r"(t0), "+r"(t1), "+r"(t2), "+r"(t3)
        : "w"(r)
        : "memory");
}

/* ------------------------------------------------------------------ */
/* hot-path consumers (intrinsics; loads/stores stay asm-pinned)      */
/* ------------------------------------------------------------------ */

static inline uint64x2_t ald_rot4(int slot, uint64x2_t a, uint64x2_t b)
{
    switch (slot) {
        case 0:  return vaddq_u64(a, b);
        case 1:  return veorq_u64(a, b);
        case 2:  return vandq_u64(a, b);
        default: return vorrq_u64(a, b);
    }
}

/* EXT #8 on 16B vectors == lane-rotate-by-one u64 == vextq_u64(x, y, 1):
 * result = (x.hi, y.lo) */
static inline uint64x2_t ald_ext2(uint64x2_t x, uint64x2_t y)
{
    return vextq_u64(x, y, 1);
}

/* ------------------------------------------------------------------ */
/* scalar golden (issue #373 paradigm: NO vector instruction on this  */
/* path; check-scalar-golden.sh gate target).  Word-level replica of  */
/* the consumer chains — the ops are lane-wise u64 arithmetic or      */
/* pure word shuffles, so the replica is exact.                       */
/* ------------------------------------------------------------------ */

/* noinline: an out-of-line call in the golden j-loop is a hard
 * auto-vectorization barrier — constprop clones of ald_scalar_golden
 * with constant consumer fold the outer switches away and GCC then
 * vectorizes the ALUROT fold (orr/and/eor v.16b — caught by the gate
 * 2026-11-24); a call boundary cannot be vectorized across */
__attribute__((noinline)) static uint64_t ald_rot64(int slot, uint64_t a, uint64_t b)
{
    switch (slot) {
        case 0:  return a + b;
        case 1:  return a ^ b;
        case 2:  return a & b;
        default: return a | b;
    }
}

/* noinline: the check-scalar-golden.sh gate disassembles THIS symbol's
 * extent — inlining it into the 54 template instantiations would make
 * it unresolvable (gate = resolution failure, not a pass) */
__attribute__((noinline)) static void ald_scalar_golden(int kind, int L, int consumer,
                              uint64_t *const *src, uint64_t *expected)
{
    if (kind == ALD_K_PAIR) {
        /* pair form: pair-step j covers elements 2j/2j+1 (words
         * 4j..4j+3); even chain slots j%4, odd chain (j+2)%4 */
        for (size_t j = 0; j < ALD_COUNT / 2; ++j) {
            const size_t w = j * 4;
            uint64_t e0 = src[0][w + 0], e1 = src[0][w + 1];
            uint64_t o0 = src[0][w + 2], o1 = src[0][w + 3];
            if (consumer == ALD_C_ALUROT) {
                const int s1 = (int)(j % 4);
                const int s2 = (int)((j + 2) % 4);
                for (int k = 1; k < L; ++k) {
                    e0 = ald_rot64(s1, e0, src[k][w + 0]);
                    e1 = ald_rot64(s1, e1, src[k][w + 1]);
                    o0 = ald_rot64(s2, o0, src[k][w + 2]);
                    o1 = ald_rot64(s2, o1, src[k][w + 3]);
                }
            } else if (consumer == ALD_C_EXT) {
                /* fold (lo,hi) = (hi, next.lo) */
                for (int k = 1; k < L; ++k) {
                    const uint64_t ne0 = e1, no0 = o1;
                    e1 = src[k][w + 0]; o1 = src[k][w + 2];
                    e0 = ne0;           o0 = no0;
                }
            }
            /* ALD_C_COPY: source 0 passes through untouched */
            expected[w + 0] = e0;
            expected[w + 1] = e1;
            expected[w + 2] = o0;
            expected[w + 3] = o1;
        }
    } else {
        /* single form: element j = words 2j/2j+1, slot j%4 */
        for (size_t j = 0; j < ALD_COUNT; ++j) {
            const size_t w = j * 2;
            uint64_t r0 = src[0][w + 0], r1 = src[0][w + 1];
            if (consumer == ALD_C_ALUROT) {
                const int s = (int)(j % 4);
                for (int k = 1; k < L; ++k) {
                    r0 = ald_rot64(s, r0, src[k][w + 0]);
                    r1 = ald_rot64(s, r1, src[k][w + 1]);
                }
            } else if (consumer == ALD_C_EXT) {
                for (int k = 1; k < L; ++k) {
                    const uint64_t n0 = r1;
                    r1 = src[k][w + 0];
                    r0 = n0;
                }
            }
            expected[w + 0] = r0;
            expected[w + 1] = r1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* combo bodies: compile-time instantiation over <Kind, L, S, C>      */
/* ------------------------------------------------------------------ */

struct AldCtx {
    struct test *test;
    int cpu;
    struct AdaptiveLdpStpShm *shm;
    int combo;
};

template <int Kind, int L, int S, int C>
static int ald_combo_body(struct AldCtx *ctx)
{
    /* TEST_LOOP's first macro parameter is a positional placeholder the
     * expansion does not reference — keep the binding warning-free */
    [[maybe_unused]] auto *test = ctx->test;

    /* per-child buffers: L sources + S targets + expected */
    uint64_t *srcbuf[L];
    uint64_t *tgtbuf[S];
    for (int k = 0; k < L; ++k) {
        srcbuf[k] = static_cast<uint64_t *>(aligned_alloc(32, ALD_BYTES));
        if (!srcbuf[k]) {
            for (int m = 0; m < k; ++m) free(srcbuf[m]);
            report_fail_msg("adaptive_ldp_stp: source buffer allocation failed");
            return EXIT_FAILURE;
        }
        memset_random(srcbuf[k], ALD_BYTES);
    }
    for (int m = 0; m < S; ++m) {
        tgtbuf[m] = static_cast<uint64_t *>(aligned_alloc(32, ALD_BYTES));
        if (!tgtbuf[m]) {
            for (int q = 0; q < m; ++q) free(tgtbuf[q]);
            for (int k = 0; k < L; ++k) free(srcbuf[k]);
            report_fail_msg("adaptive_ldp_stp: target buffer allocation failed");
            return EXIT_FAILURE;
        }
    }
    auto *expected = static_cast<uint64_t *>(aligned_alloc(32, ALD_BYTES));
    if (!expected) {
        for (int k = 0; k < L; ++k) free(srcbuf[k]);
        for (int m = 0; m < S; ++m) free(tgtbuf[m]);
        report_fail_msg("adaptive_ldp_stp: expected buffer allocation failed");
        return EXIT_FAILURE;
    }

    ald_scalar_golden(Kind, L, C, srcbuf, expected);

    char tag[64];
    ald_combo_tag(ctx->combo, tag, sizeof(tag));

    TEST_LOOP(test, 1 << 12) {
        /* one sweep over all ALD_COUNT elements */
        if constexpr (Kind == ALD_K_PAIR) {
            const uint64_t *p[L];
            uint64_t *t[S];
            for (int k = 0; k < L; ++k) p[k] = srcbuf[k];
            for (int m = 0; m < S; ++m) t[m] = tgtbuf[m];

            for (size_t j = 0; j < ALD_COUNT / 2; ++j) {
                [[maybe_unused]] uint64x2_t v0h, v0l, v1h, v1l, v2h, v2l, v3h, v3l;
                if constexpr (L == 2) {
                    ald_pair_ld2(v0h, v0l, v1h, v1l, p[0], p[1]);
                } else if constexpr (L == 3) {
                    ald_pair_ld3(v0h, v0l, v1h, v1l, v2h, v2l, p[0], p[1], p[2]);
                } else {
                    ald_pair_ld4(v0h, v0l, v1h, v1l, v2h, v2l, v3h, v3l,
                                 p[0], p[1], p[2], p[3]);
                }

                uint64x2_t r0, r1;
                if constexpr (C == ALD_C_COPY) {
                    r0 = v0h; r1 = v0l;    /* first source passes through;
                                             * others stay dead (port pressure) */
                } else {
                    r0 = v0h; r1 = v0l;
                    const int s1 = (int)(j % 4);
                    const int s2 = (int)((j + 2) % 4);
                    if constexpr (C == ALD_C_ALUROT) {
                        if constexpr (L >= 2) { r0 = ald_rot4(s1, r0, v1h); r1 = ald_rot4(s2, r1, v1l); }
                        if constexpr (L >= 3) { r0 = ald_rot4(s1, r0, v2h); r1 = ald_rot4(s2, r1, v2l); }
                        if constexpr (L >= 4) { r0 = ald_rot4(s1, r0, v3h); r1 = ald_rot4(s2, r1, v3l); }
                    } else {
                        if constexpr (L >= 2) { r0 = ald_ext2(r0, v1h); r1 = ald_ext2(r1, v1l); }
                        if constexpr (L >= 3) { r0 = ald_ext2(r0, v2h); r1 = ald_ext2(r1, v2l); }
                        if constexpr (L >= 4) { r0 = ald_ext2(r0, v3h); r1 = ald_ext2(r1, v3l); }
                    }
                }

                if constexpr (S == 2) {
                    ald_pair_st2(r0, r1, t[0], t[1]);
                } else if constexpr (S == 3) {
                    ald_pair_st3(r0, r1, t[0], t[1], t[2]);
                } else {
                    ald_pair_st4(r0, r1, t[0], t[1], t[2], t[3]);
                }
            }
        } else {
            const uint64_t *p[L];
            uint64_t *t[S];
            for (int k = 0; k < L; ++k) p[k] = srcbuf[k];
            for (int m = 0; m < S; ++m) t[m] = tgtbuf[m];

            for (size_t j = 0; j < ALD_COUNT; ++j) {
                [[maybe_unused]] uint64x2_t v0, v1, v2, v3;
                if constexpr (L == 2) {
                    ald_ld2(v0, v1, p[0], p[1]);
                } else if constexpr (L == 3) {
                    ald_ld3(v0, v1, v2, p[0], p[1], p[2]);
                } else {
                    ald_ld4(v0, v1, v2, v3, p[0], p[1], p[2], p[3]);
                }

                uint64x2_t r;
                if constexpr (C == ALD_C_COPY) {
                    r = v0;
                } else {
                    r = v0;
                    const int s = (int)(j % 4);
                    if constexpr (C == ALD_C_ALUROT) {
                        if constexpr (L >= 2) { r = ald_rot4(s, r, v1); }
                        if constexpr (L >= 3) { r = ald_rot4(s, r, v2); }
                        if constexpr (L >= 4) { r = ald_rot4(s, r, v3); }
                    } else {
                        if constexpr (L >= 2) { r = ald_ext2(r, v1); }
                        if constexpr (L >= 3) { r = ald_ext2(r, v2); }
                        if constexpr (L >= 4) { r = ald_ext2(r, v3); }
                    }
                }

                if constexpr (S == 2) {
                    ald_st2(r, t[0], t[1]);
                } else if constexpr (S == 3) {
                    ald_st3(r, t[0], t[1], t[2]);
                } else {
                    ald_st4(r, t[0], t[1], t[2], t[3]);
                }
            }
        }

        /* cold path: the LAST target is the compared dst */
        if (memcmp(tgtbuf[S - 1], expected, ALD_BYTES) != 0) {
            for (size_t i = 0; i < ALD_COUNT * 2; ++i) {
                if (tgtbuf[S - 1][i] != expected[i]) {
                    report_fail_msg("adaptive_ldp_stp: %s data miscompare "
                                    "at u64 %zu: expected 0x%016lx "
                                    "got 0x%016lx",
                                    tag, i, (unsigned long)expected[i],
                                    (unsigned long)tgtbuf[S - 1][i]);
                    if (ctx->shm)
                        __atomic_store_n(&ctx->shm->sdc_flag, 1u, __ATOMIC_RELEASE);
                    for (int k = 0; k < L; ++k) free(srcbuf[k]);
                    for (int m = 0; m < S; ++m) free(tgtbuf[m]);
                    free(expected);
                    return EXIT_FAILURE;
                }
            }
        }
    }

    for (int k = 0; k < L; ++k) free(srcbuf[k]);
    for (int m = 0; m < S; ++m) free(tgtbuf[m]);
    free(expected);
    return EXIT_SUCCESS;
}

/* the 54-entry dispatch table: id order == lexicographic priority
 * (kind > loads > stores > consumer) */
#define ALD_BODY(K, L, S) \
    &ald_combo_body<K, L, S, ALD_C_ALUROT>, \
    &ald_combo_body<K, L, S, ALD_C_EXT>,    \
    &ald_combo_body<K, L, S, ALD_C_COPY>
#define ALD_ROW(K, L) ALD_BODY(K, L, 2), ALD_BODY(K, L, 3), ALD_BODY(K, L, 4)
#define ALD_KIND(K)   ALD_ROW(K, 2), ALD_ROW(K, 3), ALD_ROW(K, 4)

static int (*const kAldRun[ALD_COMBO_COUNT])(struct AldCtx *) = {
    ALD_KIND(ALD_K_PAIR),
    ALD_KIND(ALD_K_SINGLE),
};

/* ------------------------------------------------------------------ */
/* framework plumbing                                                  */
/* ------------------------------------------------------------------ */

struct AldData {
    struct AdaptiveLdpStpShm *shm;
    int test_mode;
    int test_slice;
};

static int adaptive_ldp_stp_init(struct test *test)
{
    auto *data = static_cast<struct AldData *>(malloc(sizeof(struct AldData)));
    data->shm = ald_shm_setup();
    if (data->shm == nullptr)
        fprintf(stderr, "adaptive_ldp_stp: shm setup failed — this child "
                "will idle (combo sweep degraded)\n");
    data->test_mode = 0;
    data->test_slice = 600;
    const char *mode = getenv("ADAPTIVE_LDP_STP_MODE");
    if (mode != nullptr) {
        if (strcmp(mode, "test") == 0)
            data->test_mode = 1;
        else if (strcmp(mode, "combat") != 0)
            fprintf(stderr, "adaptive_ldp_stp: ADAPTIVE_LDP_STP_MODE must be combat|test, ignoring '%s'\n", mode);
    }
    const char *ts = getenv("ADAPTIVE_LDP_STP_TEST_SLICE");
    if (ts != nullptr) {
        int v = atoi(ts);
        if (v >= 1)
            data->test_slice = v;
        else
            fprintf(stderr, "adaptive_ldp_stp: ADAPTIVE_LDP_STP_TEST_SLICE must be >=1 second, keeping default 600\n");
    }
    test->data = data;
    return EXIT_SUCCESS;
}

static int adaptive_ldp_stp_run(struct test *test, int cpu)
{
    auto *data = static_cast<struct AldData *>(test->data);
    int combo = ald_pick_combo(data->shm, data->test_mode, data->test_slice);
    ald_covfile_trace(data->test_mode, combo);

    if (combo < 0)
        return EXIT_SUCCESS;             /* post-SDC or survey done or shm unusable: idle */

    struct AldCtx ctx = { test, cpu, data->shm, combo };
    return kAldRun[combo](&ctx);
}

static int adaptive_ldp_stp_cleanup(struct test *test)
{
    auto *data = static_cast<struct AldData *>(test->data);
    /* the shm MAPPING dies with the child process; the named object
     * intentionally outlives children (cross-child anchor) — ppid-derived
     * objects are reaped by a later run's children (dead-pid scan in
     * ald_shm_reap_stale), KEY'd objects belong to the campaign script */
    free(data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

#else  /* !__aarch64__ */

static int adaptive_ldp_stp_init(struct test *test)  { (void)test; return EXIT_SUCCESS; }
static int adaptive_ldp_stp_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SUCCESS; }
static int adaptive_ldp_stp_cleanup(struct test *test) { (void)test; return EXIT_SUCCESS; }

#endif

DECLARE_TEST(adaptive_ldp_stp, "Adaptive LDP/STP-vs-LDR/STR copy-engine SDC test: 54-combo priority sweep (kind: ldp/stp pairs first, then ldr/str singles > loads 2-4 chained > stores 2-4 temp-to-dst-last > consumer: ALU rotation / EXT #8 / pure copy — this batch's middle loads only, 2026-11-24), 30min wall-clock slice per combo (shm anchor), verify-mode round-robin for golden smoke, covfile trace; MODE=combat (first SDC freezes later children) or test (survey, one TEST_SLICE window per combo); SDC report carries the combo tag")
    .test_init    = adaptive_ldp_stp_init,
    .test_run     = adaptive_ldp_stp_run,
    .test_cleanup = adaptive_ldp_stp_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
