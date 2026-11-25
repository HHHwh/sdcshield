/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b adaptive_ldp_stp_intr
 * @parblock
 * Intrinsics-built twin of adaptive_ldp_stp (2026-11-25, user
 * directive: one C++-built extension per fmov case).
 *
 * Same engine chassis as the asm twin — shm wall-clock anchor
 * (fork_each_test per-child combo pick), verify round-robin, covfile
 * trace, combat/test modes, SDC-flag freeze, combo-tagged reports —
 * but the combo space and the construction differ:
 *
 *   - ZERO inline assembly: loads are vld1q_u64, stores vst1q_u64,
 *     consumers pure intrinsics; the compiler schedules and encodes
 *     freely (this TU compiles with -fno-tree-loop-distribute-patterns
 *     so no train can collapse into a libc call).
 *   - The KIND axis (ldp/stp pairs vs ldr/str singles) is DROPPED:
 *     intrinsics cannot select the instruction encoding, so the kind
 *     axis exists only in the asm arm.  27 combos remain:
 *         id = ((loads-2)*3 + (stores-2))*3 + consumer
 *     loads 2 > 3 > 4 (chained sources, pair-step layout: even/odd
 *     element chains, slots j%4 / (j+2)%4), stores 2 > 3 > 4
 *     (temp -> junk* -> dst, the compared dst ALWAYS last), consumer
 *     ALU rotation > EXT #8 (vextq_u64(x, y, 1)) > pure copy.
 *
 * COPY-consumer semantics difference vs the asm twin (deliberate,
 * recorded): the asm arm keeps its beyond-the-first source loads
 * alive with asm volatile (dead-load port pressure, the ldr_at_top
 * lineage).  Pure intrinsics whose results are unused are DCE'd by
 * GCC, so this arm makes the extra sources LIVE instead: they are
 * xor-accumulated into a per-sweep accumulator whose final value is
 * cold-checked against the scalar-predicted golden.  Same load ports,
 * one extra vector xor per extra source — the closest honest
 * translation of "extra load pressure without feeding the result".
 *
 * Golden: PURE SCALAR (issue #373 paradigm, check-scalar-golden.sh
 * gated on aldi_scalar_golden): the same word-level fold as the asm
 * twin's pair-form branch, plus (COPY only) the accumulator golden:
 * acc lane 0 = xor of every EXTRA source's even-index words, lane 1
 * = odd-index words (each extra word is xor'd exactly once per sweep).
 *
 * Env contract (mirrors the asm engine with the _INTR suffix):
 *   ADAPTIVE_LDP_STP_INTR_{KEY, VERIFY, COVFILE, COMBO, MODE,
 *   TEST_SLICE} — see the asm twin's header for the full semantics.
 * The shm name prefix ("adaptive_ldp_stp_intr_") and the stale-object
 * reaper's prefix scan are this engine's own; the two engines can run
 * in the same suite without colliding.
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

#define ALDI_COMBO_COUNT 27
#define ALDI_SLICE_SECONDS 1800            /* 30min combat slice */
#define ALDI_COUNT 1024                    /* elements per buffer, family convention */
#define ALDI_BYTES (ALDI_COUNT * 16)      /* 16KB per buffer */

enum { ALDI_C_ALUROT = 0, ALDI_C_EXT = 1, ALDI_C_COPY = 2 };

static inline void aldi_combo_unpack(int id, int *loads, int *stores, int *consumer)
{
    *consumer = id % 3; id /= 3;
    *stores = id % 3 + 2; id /= 3;
    *loads = id + 2;
}

#if defined(__aarch64__)

/* ------------------------------------------------------------------ */
/* shm machinery (mirrors the asm engine; own name prefix)              */
/* ------------------------------------------------------------------ */

struct AdaptiveLdpStpIntrShm {
    struct timespec anchor;               /* wall-clock anchor */
    uint32_t ready;                       /* creator publish flag */
    uint32_t verify_ctr;                  /* verify-mode round-robin */
    uint32_t sdc_flag;                    /* set by any failing child */
};

/* Stale-object reaper: the default ppid-derived name outlives every
 * child by design (cross-child anchor); each child reaps objects whose
 * parent process is demonstrably dead.  KEY'd objects belong to the
 * campaign script.  See the asm twin for the full rationale. */
static void aldi_shm_reap_stale(void)
{
    if (getenv("ADAPTIVE_LDP_STP_INTR_KEY") != nullptr)
        return;
    DIR *dir = opendir("/dev/shm");
    if (dir == nullptr)
        return;
    static const char prefix[] = "adaptive_ldp_stp_intr_ppid";
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
        char path[9 + 256 + 1];     /* "/dev/shm/" + NAME_MAX + NUL */
        snprintf(path, sizeof(path), "/dev/shm/%s", ent->d_name);
        if (unlink(path) == 0)
            fprintf(stderr, "adaptive_ldp_stp_intr: reaped stale shm object %s "
                    "(parent process %ld is gone)\n", ent->d_name, pid);
    }
    closedir(dir);
}

static struct AdaptiveLdpStpIntrShm *aldi_shm_setup(void)
{
    aldi_shm_reap_stale();

    const char *key = getenv("ADAPTIVE_LDP_STP_INTR_KEY");
    char name[144];
    if (key) {
        snprintf(name, sizeof(name), "/adaptive_ldp_stp_intr_%s", key);
    } else {
        snprintf(name, sizeof(name), "/adaptive_ldp_stp_intr_ppid%ld", (long)getppid());
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

    if (creator && ftruncate(fd, sizeof(struct AdaptiveLdpStpIntrShm)) != 0) {
        close(fd);
        return nullptr;
    }

    auto *shm = static_cast<struct AdaptiveLdpStpIntrShm *>(
            mmap(nullptr, sizeof(struct AdaptiveLdpStpIntrShm), PROT_READ | PROT_WRITE,
                 MAP_SHARED, fd, 0));
    close(fd);
    if (shm == MAP_FAILED)
        return nullptr;

    if (creator) {
        memset(shm, 0, sizeof(*shm));
        clock_gettime(CLOCK_MONOTONIC, &shm->anchor);
        __atomic_store_n(&shm->ready, 1u, __ATOMIC_RELEASE);
    } else {
        for (int i = 0; i < 200000; i++) {
            if (__atomic_load_n(&shm->ready, __ATOMIC_ACQUIRE) == 1u)
                break;
            usleep(10);
        }
    }
    return shm;
}

static int64_t aldi_seconds_since(const struct timespec *anchor, struct timespec *now)
{
    clock_gettime(CLOCK_MONOTONIC, now);
    int64_t s = (int64_t)(now->tv_sec - anchor->tv_sec);
    if (now->tv_nsec < anchor->tv_nsec)
        s -= 1;
    return s < 0 ? 0 : s;
}

static int aldi_pick_combo(struct AdaptiveLdpStpIntrShm *shm, int test_mode, int test_slice)
{
    if (!shm)
        return -1;
    if (!test_mode &&
        __atomic_load_n(&shm->sdc_flag, __ATOMIC_ACQUIRE) != 0)
        return -1;
    if (__atomic_load_n(&shm->ready, __ATOMIC_ACQUIRE) != 1u)
        return -1;

    const char *force = getenv("ADAPTIVE_LDP_STP_INTR_COMBO");
    if (force != nullptr) {
        int c = atoi(force);
        if (c >= 0 && c < ALDI_COMBO_COUNT)
            return c;
    }

    if (getenv("ADAPTIVE_LDP_STP_INTR_VERIFY") != nullptr) {
        uint32_t prev = __atomic_fetch_add(&shm->verify_ctr, 1u, __ATOMIC_RELAXED);
        return (int)(prev % ALDI_COMBO_COUNT);
    }

    struct timespec now;
    if (test_mode) {
        int64_t elapsed = aldi_seconds_since(&shm->anchor, &now);
        int64_t idx = elapsed / test_slice;
        if (idx >= ALDI_COMBO_COUNT)
            return -1;
        return (int)idx;
    }

    int64_t elapsed = aldi_seconds_since(&shm->anchor, &now);
    int64_t idx = elapsed / ALDI_SLICE_SECONDS;
    if (idx > ALDI_COMBO_COUNT - 1)
        idx = ALDI_COMBO_COUNT - 1;
    return (int)idx;
}

static void aldi_covfile_trace(int test_mode, int combo)
{
    const char *path = getenv("ADAPTIVE_LDP_STP_INTR_COVFILE");
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
             getenv("ADAPTIVE_LDP_STP_INTR_VERIFY") ? "verify"
                 : test_mode ? "test" : "normal",
             combo);
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0)
        return;
    (void)!write(fd, line, strlen(line));
    close(fd);
}

static void aldi_combo_tag(int id, char *buf, size_t n)
{
    int loads, stores, consumer;
    aldi_combo_unpack(id, &loads, &stores, &consumer);
    snprintf(buf, n, "combo#%d l%d s%d %s", id, loads, stores,
             consumer == ALDI_C_ALUROT ? "alurot"
                 : consumer == ALDI_C_EXT ? "ext" : "copy");
}

/* ------------------------------------------------------------------ */
/* scalar golden (issue #373 paradigm — no vector instruction here;    */
/* noinline: the gate disassembles THIS symbol; ald_rot64-style call   */
/* barriers against constprop-clone auto-vectorization, the asm arm  */
/* hit that on 2026-11-24)                                             */
/* ------------------------------------------------------------------ */

__attribute__((noinline)) static uint64_t aldi_rot64(int slot, uint64_t a, uint64_t b)
{
    switch (slot) {
        case 0:  return a + b;
        case 1:  return a ^ b;
        case 2:  return a & b;
        default: return a | b;
    }
}

/* expected: the pair-form fold (same derivation as the asm engine).
 * acc_golden (COPY only): lane 0 = xor of every EXTRA source's
 * even-index words, lane 1 = odd-index words; zeros otherwise. */
__attribute__((noinline)) static void aldi_scalar_golden(int L, int consumer,
                                                         uint64_t *const *src,
                                                         uint64_t *expected,
                                                         uint64_t *acc_golden)
{
    acc_golden[0] = 0;
    acc_golden[1] = 0;

    for (size_t j = 0; j < ALDI_COUNT / 2; ++j) {
        const size_t w = j * 4;
        uint64_t e0 = src[0][w + 0], e1 = src[0][w + 1];
        uint64_t o0 = src[0][w + 2], o1 = src[0][w + 3];
        if (consumer == ALDI_C_ALUROT) {
            const int s1 = (int)(j % 4);
            const int s2 = (int)((j + 2) % 4);
            for (int k = 1; k < L; ++k) {
                e0 = aldi_rot64(s1, e0, src[k][w + 0]);
                e1 = aldi_rot64(s1, e1, src[k][w + 1]);
                o0 = aldi_rot64(s2, o0, src[k][w + 2]);
                o1 = aldi_rot64(s2, o1, src[k][w + 3]);
            }
        } else if (consumer == ALDI_C_EXT) {
            for (int k = 1; k < L; ++k) {
                const uint64_t ne0 = e1, no0 = o1;
                e1 = src[k][w + 0]; o1 = src[k][w + 2];
                e0 = ne0;           o0 = no0;
            }
        } else {                        /* ALDI_C_COPY */
            for (int k = 1; k < L; ++k) {
                acc_golden[0] ^= src[k][w + 0] ^ src[k][w + 2];
                acc_golden[1] ^= src[k][w + 1] ^ src[k][w + 3];
            }
        }
        expected[w + 0] = e0;
        expected[w + 1] = e1;
        expected[w + 2] = o0;
        expected[w + 3] = o1;
    }
}

/* ------------------------------------------------------------------ */
/* combo bodies: compile-time instantiation over <L, S, C>              */
/* ------------------------------------------------------------------ */

/* hot-path consumer: the rotating integer ALU (family chassis) */
static inline uint64x2_t aldi_rot4(int slot, uint64x2_t a, uint64x2_t b)
{
    switch (slot) {
        case 0:  return vaddq_u64(a, b);
        case 1:  return veorq_u64(a, b);
        case 2:  return vandq_u64(a, b);
        default: return vorrq_u64(a, b);
    }
}

struct AldiCtx {
    struct test *test;
    int cpu;
    struct AdaptiveLdpStpIntrShm *shm;
    int combo;
};

template <int L, int S, int C>
static int aldi_combo_body(struct AldiCtx *ctx)
{
    /* TEST_LOOP's first macro parameter is a positional placeholder the
     * expansion does not reference — keep the binding warning-free */
    [[maybe_unused]] auto *test = ctx->test;

    uint64_t *srcbuf[L];
    uint64_t *tgtbuf[S];
    for (int k = 0; k < L; ++k) {
        srcbuf[k] = static_cast<uint64_t *>(aligned_alloc(32, ALDI_BYTES));
        if (!srcbuf[k]) {
            for (int m = 0; m < k; ++m) free(srcbuf[m]);
            report_fail_msg("adaptive_ldp_stp_intr: source buffer allocation failed");
            return EXIT_FAILURE;
        }
        memset_random(srcbuf[k], ALDI_BYTES);
    }
    for (int m = 0; m < S; ++m) {
        tgtbuf[m] = static_cast<uint64_t *>(aligned_alloc(32, ALDI_BYTES));
        if (!tgtbuf[m]) {
            for (int q = 0; q < m; ++q) free(tgtbuf[q]);
            for (int k = 0; k < L; ++k) free(srcbuf[k]);
            report_fail_msg("adaptive_ldp_stp_intr: target buffer allocation failed");
            return EXIT_FAILURE;
        }
    }
    auto *expected = static_cast<uint64_t *>(aligned_alloc(32, ALDI_BYTES));
    uint64_t acc_golden[2];
    if (!expected) {
        for (int k = 0; k < L; ++k) free(srcbuf[k]);
        for (int m = 0; m < S; ++m) free(tgtbuf[m]);
        report_fail_msg("adaptive_ldp_stp_intr: expected buffer allocation failed");
        return EXIT_FAILURE;
    }

    aldi_scalar_golden(L, C, srcbuf, expected, acc_golden);

    char tag[48];
    aldi_combo_tag(ctx->combo, tag, sizeof(tag));

    TEST_LOOP(test, 1 << 12) {
        const uint64_t *p[L];
        uint64_t *t[S];
        for (int k = 0; k < L; ++k) p[k] = srcbuf[k];
        for (int m = 0; m < S; ++m) t[m] = tgtbuf[m];
        uint64x2_t acc = vdupq_n_u64(0);

        for (size_t j = 0; j < ALDI_COUNT / 2; ++j) {
            const size_t w = j * 4;
            const uint64x2_t v0e = vld1q_u64(&p[0][w + 0]);
            const uint64x2_t v0o = vld1q_u64(&p[0][w + 2]);
            [[maybe_unused]] uint64x2_t v1e, v1o, v2e, v2o, v3e, v3o;
            if constexpr (L >= 2) {
                v1e = vld1q_u64(&p[1][w + 0]);
                v1o = vld1q_u64(&p[1][w + 2]);
            }
            if constexpr (L >= 3) {
                v2e = vld1q_u64(&p[2][w + 0]);
                v2o = vld1q_u64(&p[2][w + 2]);
            }
            if constexpr (L >= 4) {
                v3e = vld1q_u64(&p[3][w + 0]);
                v3o = vld1q_u64(&p[3][w + 2]);
            }

            uint64x2_t r0, r1;
            if constexpr (C == ALDI_C_COPY) {
                /* first source passes through; the extra sources stay
                 * live via the checked xor accumulator (DCE defense —
                 * see header) */
                r0 = v0e; r1 = v0o;
                if constexpr (L >= 2) { acc = veorq_u64(acc, v1e); acc = veorq_u64(acc, v1o); }
                if constexpr (L >= 3) { acc = veorq_u64(acc, v2e); acc = veorq_u64(acc, v2o); }
                if constexpr (L >= 4) { acc = veorq_u64(acc, v3e); acc = veorq_u64(acc, v3o); }
            } else if constexpr (C == ALDI_C_ALUROT) {
                r0 = v0e; r1 = v0o;
                const int s1 = (int)(j % 4);
                const int s2 = (int)((j + 2) % 4);
                if constexpr (L >= 2) { r0 = aldi_rot4(s1, r0, v1e); r1 = aldi_rot4(s2, r1, v1o); }
                if constexpr (L >= 3) { r0 = aldi_rot4(s1, r0, v2e); r1 = aldi_rot4(s2, r1, v2o); }
                if constexpr (L >= 4) { r0 = aldi_rot4(s1, r0, v3e); r1 = aldi_rot4(s2, r1, v3o); }
            } else {                        /* ALDI_C_EXT */
                r0 = v0e; r1 = v0o;
                if constexpr (L >= 2) { r0 = vextq_u64(r0, v1e, 1); r1 = vextq_u64(r1, v1o, 1); }
                if constexpr (L >= 3) { r0 = vextq_u64(r0, v2e, 1); r1 = vextq_u64(r1, v2o, 1); }
                if constexpr (L >= 4) { r0 = vextq_u64(r0, v3e, 1); r1 = vextq_u64(r1, v3o, 1); }
            }

            /* stores: every target gets the result pair, dst last */
            if constexpr (S >= 1) { vst1q_u64(&t[0][w + 0], r0); vst1q_u64(&t[0][w + 2], r1); }
            if constexpr (S >= 2) { vst1q_u64(&t[1][w + 0], r0); vst1q_u64(&t[1][w + 2], r1); }
            if constexpr (S >= 3) { vst1q_u64(&t[2][w + 0], r0); vst1q_u64(&t[2][w + 2], r1); }
            if constexpr (S >= 4) { vst1q_u64(&t[3][w + 0], r0); vst1q_u64(&t[3][w + 2], r1); }
        }

        /* cold path: the LAST target is the compared dst; COPY combos
         * additionally cold-check the accumulator against its golden */
        const uint64_t acc0 = vgetq_lane_u64(acc, 0);
        const uint64_t acc1 = vgetq_lane_u64(acc, 1);
        if (memcmp(tgtbuf[S - 1], expected, ALDI_BYTES) != 0 ||
            (C == ALDI_C_COPY && (acc0 != acc_golden[0] || acc1 != acc_golden[1]))) {
            if (C == ALDI_C_COPY && (acc0 != acc_golden[0] || acc1 != acc_golden[1])) {
                report_fail_msg("adaptive_ldp_stp_intr: %s accumulator mismatch: "
                                "expected {%016lx,%016lx} got {%016lx,%016lx} "
                                "(copy-consumer extra-source load train)",
                                tag, (unsigned long)acc_golden[0],
                                (unsigned long)acc_golden[1],
                                (unsigned long)acc0, (unsigned long)acc1);
                if (ctx->shm)
                    __atomic_store_n(&ctx->shm->sdc_flag, 1u, __ATOMIC_RELEASE);
            }
            for (size_t i = 0; i < ALDI_COUNT * 2; ++i) {
                if (tgtbuf[S - 1][i] != expected[i]) {
                    report_fail_msg("adaptive_ldp_stp_intr: %s data miscompare "
                                    "at u64 %zu: expected 0x%016lx "
                                    "got 0x%016lx (compiler-built, no asm)",
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

/* ------------------------------------------------------------------ */
/* framework plumbing                                                  */
/* ------------------------------------------------------------------ */

/* the 27-entry dispatch table: id order == lexicographic priority
 * (loads > stores > consumer); kind axis exists only in the asm arm */
#define ALDI_BODY(L, S) \
    &aldi_combo_body<L, S, ALDI_C_ALUROT>, \
    &aldi_combo_body<L, S, ALDI_C_EXT>,    \
    &aldi_combo_body<L, S, ALDI_C_COPY>
#define ALDI_ROW(L) ALDI_BODY(L, 2), ALDI_BODY(L, 3), ALDI_BODY(L, 4)

static int (*const kAldiRun[ALDI_COMBO_COUNT])(struct AldiCtx *) = {
    ALDI_ROW(2),
    ALDI_ROW(3),
    ALDI_ROW(4),
};

struct AldiData {
    struct AdaptiveLdpStpIntrShm *shm;
    int test_mode;
    int test_slice;
};

static int adaptive_ldp_stp_intr_init(struct test *test)
{
    auto *data = static_cast<struct AldiData *>(malloc(sizeof(struct AldiData)));
    data->shm = aldi_shm_setup();
    if (data->shm == nullptr)
        fprintf(stderr, "adaptive_ldp_stp_intr: shm setup failed — this child "
                "will idle (combo sweep degraded)\n");
    data->test_mode = 0;
    data->test_slice = 600;
    const char *mode = getenv("ADAPTIVE_LDP_STP_INTR_MODE");
    if (mode != nullptr) {
        if (strcmp(mode, "test") == 0)
            data->test_mode = 1;
        else if (strcmp(mode, "combat") != 0)
            fprintf(stderr, "adaptive_ldp_stp_intr: ADAPTIVE_LDP_STP_INTR_MODE must be combat|test, ignoring '%s'\n", mode);
    }
    const char *ts = getenv("ADAPTIVE_LDP_STP_INTR_TEST_SLICE");
    if (ts != nullptr) {
        int v = atoi(ts);
        if (v >= 1)
            data->test_slice = v;
        else
            fprintf(stderr, "adaptive_ldp_stp_intr: ADAPTIVE_LDP_STP_INTR_TEST_SLICE must be >=1 second, keeping default 600\n");
    }
    test->data = data;
    return EXIT_SUCCESS;
}

static int adaptive_ldp_stp_intr_run(struct test *test, int cpu)
{
    auto *data = static_cast<struct AldiData *>(test->data);
    int combo = aldi_pick_combo(data->shm, data->test_mode, data->test_slice);
    aldi_covfile_trace(data->test_mode, combo);

    if (combo < 0)
        return EXIT_SUCCESS;             /* post-SDC or survey done or shm unusable: idle */

    struct AldiCtx ctx = { test, cpu, data->shm, combo };
    return kAldiRun[combo](&ctx);
}

static int adaptive_ldp_stp_intr_cleanup(struct test *test)
{
    auto *data = static_cast<struct AldiData *>(test->data);
    /* the shm MAPPING dies with the child; the named object outlives
     * children by design — ppid-derived objects reaped by later runs,
     * KEY'd objects belong to the campaign script (see asm twin) */
    free(data);
    test->data = nullptr;
    return EXIT_SUCCESS;
}

#else  /* !__aarch64__ */

static int adaptive_ldp_stp_intr_init(struct test *test)  { (void)test; return EXIT_SUCCESS; }
static int adaptive_ldp_stp_intr_run(struct test *test, int cpu) { (void)test; (void)cpu; return EXIT_SUCCESS; }
static int adaptive_ldp_stp_intr_cleanup(struct test *test) { (void)test; return EXIT_SUCCESS; }

#endif

DECLARE_TEST(adaptive_ldp_stp_intr, "Intrinsics-built twin of adaptive_ldp_stp: 27-combo priority sweep (loads 2-4 chained > stores 2-4 temp-to-dst-last > consumer ALU rotation / EXT / pure copy), zero asm — vld1q/vst1q with compiler-chosen encodings (-fno-tree-loop-distribute-patterns), COPY combos keep extra sources live via a checked xor accumulator; shm wall-clock anchor (own prefix), verify round-robin, covfile, combat/test modes; SDC report carries the combo tag (ARM64)")
    .test_init    = adaptive_ldp_stp_intr_init,
    .test_run     = adaptive_ldp_stp_intr_run,
    .test_cleanup = adaptive_ldp_stp_intr_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
