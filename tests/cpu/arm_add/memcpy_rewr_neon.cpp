/*  NEON variant of tests/cpu/memory/memcpy_rewr_neon.cpp — auto-derived by the 2026-11-02
 *  cache/memory NEON pass: load-point neon_copy() calls replaced with the explicit
 *  vld1q/vst1q stream in arm_add_neon_copy.h (bypasses the libc IFUNC dispatcher,
 *  keeps the NEON datapath attribution).  Original test untouched; identifiers
 *  carry the _neon suffix to avoid ODR clashes with the original TU.
 */
/*
 *  memcpy_rewr_neon.cpp - producer/consumer memcpy coherence stress test for
 *  SDCShield (ARM64).
 *
 *  Origin: a standalone GlusterFS-IOT-scheduler-derived MPSC stress tool
 *  (memcpy_rewr_neon.c) that spawns pinned producer/consumer pthreads, routes
 *  requests through a 4-priority linked-list queue, and - crucially -
 *  checks that memcpy of the per-thread mem_info_s buffers yields byte-
 *  identical copies across b1..b5. A mismatch that does *not* crash is
 *  exactly the silent-data-corruption (SDC) SDCShield exists to catch.
 *
 *  Port to SDCShield
 *  ------------------
 *  - The standalone main()+pthread_create is replaced by the framework's
 *    forked-child + per-CPU worker-thread model. Each test_run(test,cpu)
 *    thread IS one pinned worker (the framework already pins it to
 *    device_info[cpu].cpu_number, equivalent to the original
 *    pthread_setaffinity_np). Roles (producer vs consumer) are assigned
 *    from CPU topology via device_info[cpu].numa_id/die_id/core_id, per
 *    the selected strategy (see memcpy_rewr_strategies.conf).
 *  - The MPSC queue, the ARM64 arch-timer cycle source, the sync
 *    primitives (__yawn/__wake/__yield), the mem_info_s layout, and the
 *    b1..b5 memcpy+memcmp SDC check are preserved verbatim.
 *  - Strategy selection is handled by the reusable strategy_config
 *    framework (SANDSTONE_STRATEGY_INDEX cycles strategies round-robin
 *    across repeated invocations; SANDSTONE_STRATEGY_CONF overrides the
 *    config path). When no config file is present (default path missing
 *    or SANDSTONE_STRATEGY_CONF dangling), the test degrades to the
 *    original tool's default mode instead of skipping: first-N-producers
 *    roles (N = max(1, threads/12)) and a MemAvailable-adaptive block_size
 *    clamped to [64 KiB, c_size] — see default_block_size() in the
 *    implementation.
 *
 *  ARM64-only: the arch-timer inline asm (mrs cntvct_el0 / cntfrq_el0) has
 *  no x86 equivalent and the x86-64 paths must stay untouched, so this
 *  test is built and registered only on aarch64 (see meson.build guard).
 *
 *  SPDX-License-Identifier: Apache-2.0
 */
#include <sandstone.h>
#include "arm_add_neon_copy.h"
using arm_add_neon::neon_copy;

#ifdef __aarch64__

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <vector>

#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>   /* access() for conf-existence probe */
#include <cerrno>     /* ENOENT/ENOTDIR discrimination     */

#include "../memory/list.h"
#include "../memory/strategy_config.h"

/* ---- original per-transfer buffer geometry (unchanged) ----
 * c_size is 2 MiB; the per-thread mem_info_s packs five 2 MiB fields (b1..b5)
 * interleaved with small tag arrays and aligned to 128 bytes. The 65536-byte
 * block_size configured in the .conf only ever touches the front of each
 * field, but the full struct is allocated so the layout - and thus the cache
 * colouring / aliasing behaviour being stressed - is identical to upstream.
 */
#define c_size (1024 * 1024 * 2)

struct mem_info_s {
    unsigned char flag;
    unsigned char b1[c_size];   // 1
    unsigned char num[7];
    unsigned char b2[c_size];   // 8
    unsigned char boy[3];
    unsigned char b3[c_size];   // 3
    unsigned char girl[4];
    unsigned char b4[c_size];   // 8
    unsigned char uncle[5];
    unsigned char b5[c_size];   // 5
} __attribute__((__aligned__(128)));

struct iatt {
    char data[128];
};

struct syncargs {
    int op_ret;
    int op_errno;
    struct iatt iatt1;
    struct iatt iatt2;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int done;
};

typedef enum {
    IOT_PRI_HI = 0,      /* low latency */
    IOT_PRI_NORMAL,      /* normal */
    IOT_PRI_LO,          /* bulk */
    IOT_PRI_LEAST,       /* least */
    IOT_PRI_MAX,
} iot_pri_t;

typedef struct {
    struct list_head clients;
    struct list_head reqs;
} iot_client_ctx_t;

enum _gf_boolean { _gf_false = 0, _gf_true = 1 };
typedef enum _gf_boolean gf_boolean_t;

struct iot_conf {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int32_t max_count;     /* configured maximum */
    int32_t curr_count;    /* actual number of threads running */
    int32_t sleep_count;
    int32_t idle_time;     /* in seconds */
    struct list_head clients[IOT_PRI_MAX];
    iot_client_ctx_t no_client[IOT_PRI_MAX];
    int32_t ac_iot_limit[IOT_PRI_MAX];
    int32_t ac_iot_count[IOT_PRI_MAX];
    int queue_sizes[IOT_PRI_MAX];
    int queue_size;
    pthread_attr_t w_attr;
    gf_boolean_t least_priority;
    void *self;                 /* upstream field named 'this'; renamed for C++ */
    size_t stack_size;
    gf_boolean_t down;
    gf_boolean_t mutex_inited;
    gf_boolean_t cond_inited;
};
typedef struct iot_conf iot_conf_t;

#define __yawn(args)                            \
    do {                                        \
        pthread_mutex_init(&args->mutex, NULL); \
        pthread_cond_init(&args->cond, NULL);   \
        args->done = 0;                         \
    } while (0)
#define __wake(args)                          \
    do {                                      \
        pthread_mutex_lock(&args->mutex);     \
        {                                     \
            args->done = 1;                   \
            pthread_cond_signal(&args->cond); \
        }                                     \
        pthread_mutex_unlock(&args->mutex);   \
    } while (0)
#define __yield(args)                                         \
    do {                                                      \
        pthread_mutex_lock(&args->mutex);                     \
        {                                                     \
            while (!args->done)                               \
                pthread_cond_wait(&args->cond, &args->mutex); \
        }                                                     \
        pthread_mutex_unlock(&args->mutex);                   \
        pthread_mutex_destroy(&args->mutex);                  \
        pthread_cond_destroy(&args->cond);                    \
    } while (0)

typedef struct _call_stub {
    struct list_head list;
    char wind;
    struct syncargs args;
} call_stub_t;

/* ---- ARM64 arch-timer cycle source (the original inline asm, unchanged) ----
 * Reads CNTVCT_EL0 (virtual counter) as a cycle source mixed into the data
 * pattern, with an ordering fence. cntfrq_el0 is also exposed for frequency.
 * The eor/add/ldr-to-xzr sequence is the original ordering hack kept verbatim.
 */
#define arch_counter_enforce_ordering(val) \
    do {                                   \
        unsigned long tmp, _val = (val);   \
        asm volatile("    eor    %0, %1, %1\n"   \
                     "    add    %0, sp, %0\n"   \
                     "    ldr    xzr, [%0]"      \
                     : "=r"(tmp)           \
                     : "r"(_val));         \
    } while (0)

#define notrace __attribute__((__no_instrument_function__))

#define read_sysreg_cntvct_el0                            \
    ({                                                    \
        unsigned long __val;                              \
        asm volatile("mrs %0, cntvct_el0" : "=r"(__val)); \
        __val;                                            \
    })

#define read_sysreg_cntfrq_el0                            \
    ({                                                    \
        unsigned long __val;                              \
        asm volatile("mrs %0, cntfrq_el0" : "=r"(__val)); \
        __val;                                            \
    })

static unsigned long user_archtimer_get_cntfrq(void)
{
    return read_sysreg_cntfrq_el0;
}

static inline unsigned long __arch_counter_get_cntvct(void)
{
    unsigned long cnt;
    asm volatile("isb" : : : "memory");
    cnt = read_sysreg_cntvct_el0;
    arch_counter_enforce_ordering(cnt);
    return cnt;
}

notrace static unsigned long arch_counter_get_cntvct(void)
{
    return __arch_counter_get_cntvct();
}

static unsigned long (*arch_timer_read_counter)(void) = arch_counter_get_cntvct;

static unsigned long myget_cycles(void)
{
    return arch_timer_read_counter();
}

/* ---- per-test shared state ----
 * In the standalone tool these were globals; in SDCShield they live in a
 * struct hung off test->data so the (forked) child's test_cleanup can free
 * them and so multiple tests never collide.
 */
struct memcpy_rewr_state {
    iot_conf_t conf;
    struct mem_info_s gl_tester;          /* producer-shared source */
    std::atomic<unsigned char> mem_flag;  /* 1 once an SDC mismatch is seen */
    std::atomic<long long> product;      /* consumer iteration counter */
    /* issue #374: the block-constant timer value the current gl_tester.b1
     * content was seeded with (i ^ c). Written only inside the queue mutex
     * (together with func_data_make's b1 memcpy), snapshotted by consumers
     * inside the same mutex — see do_iot_schedule / iot_worker. */
    uint64_t golden_cycles;
    int g_size;                           /* per-transfer payload */
    int g_size_t;                         /* scratch-copy payload */
    unsigned char a1[c_size];
    unsigned char a2[c_size];
    unsigned char a3[c_size];
    unsigned char a4[c_size];
    /* strategy + topology helpers for role determination. The StrategySet
     * is owned here (NOT a dangling pointer into a stack-local) so the
     * const Strategy* below stays valid for the whole test lifetime,
     * across all worker threads. */
    StrategySet strategy_set;
    const Strategy *strategy;
    size_t strategy_index;
    int producer_count;                   /* for first_n_producers */
    std::vector<int> numa_ids_sorted;     /* sorted-unique NUMA ids */
};

/*
 * Per-thread buffers. The original tool used __thread globals for m_tester
 * (~10 MiB) and m_tmp (2 MiB). Under SDCShield that does not work: the
 * framework spawns up to 192 worker threads each with an 8 MiB stack, and
 * the ~12 MiB of __thread storage per thread overflows it (SIGSEGV on
 * first touch). We therefore keep them per-thread but heap-allocate them
 * lazily through a thread_local pointer, which preserves the original
 * "each worker has its own private m_tester/m_tmp" semantics without
 * touching the stack.
 */
static thread_local struct mem_info_s *m_tester_p = nullptr;
static thread_local unsigned char *m_tmp_p = nullptr;

/* lazily allocate this thread's buffers; idempotent. */
static bool ensure_thread_buffers(void)
{
    if (!m_tester_p) {
        m_tester_p = static_cast<mem_info_s *>(malloc(sizeof(mem_info_s)));
        if (!m_tester_p)
            return false;
    }
    if (!m_tmp_p) {
        m_tmp_p = static_cast<unsigned char *>(malloc(c_size));
        if (!m_tmp_p)
            return false;
    }
    return true;
}

/* ---- data-prep + SDC check (semantics preserved verbatim) ---- */

static void func_data_make(memcpy_rewr_state *st, int size)
{
    if (!m_tmp_p)
        return;
    neon_copy(st->gl_tester.b1, m_tmp_p, size);
}

static void func_memtest(memcpy_rewr_state *st, int size)
{
    for (int i = 0; i < size; i++) {
        st->a1[i] = i ^ myget_cycles();
    }
    neon_copy(st->a2, st->a1, size);
    neon_copy(st->a3, st->a2, size);
    neon_copy(st->a4, st->a2, size);
}

static void func_struct_test1(memcpy_rewr_state *st, int size)
{
    if (!m_tester_p)
        return;
    neon_copy(m_tester_p->b1, st->gl_tester.b1, size);
}

static void func_struct_test2(memcpy_rewr_state *st, int size,
                              uint64_t golden_c, bool copied)
{
    if (!m_tester_p)
        return;
    neon_copy(m_tester_p->b2, m_tester_p->b1, size);
    neon_copy(m_tester_p->b3, m_tester_p->b2, size);
    neon_copy(m_tester_p->b5, m_tester_p->b2, size);
    neon_copy(m_tester_p->b4, m_tester_p->b1, size);
    /* issue #374: independent arithmetic golden for the transport chain
     * m_tmp -> gl_tester.b1 -> b1. The b2..b5 cross-compare below only
     * proves the copies agree WITH b1 (copy-vs-copy); b1 itself was never
     * verified against what the producer put in. expected[i] = i ^ golden_c
     * is computed in registers (never a memcpy / shared-buffer read), so a
     * persistent memcpy/LSU fault anywhere in the chain surfaces here.
     * `copied` guards the never-received-a-block case (b1 would be
     * uninitialized malloc memory — the b2..b5 garbage-vs-garbage compare
     * tolerates it, an arithmetic golden must not). */
    if (copied) {
        for (int i = 0; i < size; i++) {
            unsigned char expected =
                static_cast<unsigned char>(static_cast<uint64_t>(i) ^ golden_c);
            if (m_tester_p->b1[i] != expected) {
                log_error("memcpy_rewr_neon: SDC on CPU %d: b1[%d]=0x%x expected=0x%x "
                          "(arithmetic golden i^C, C=0x%llx)",
                          device_info[thread_num].cpu_number, i,
                          m_tester_p->b1[i], expected,
                          (unsigned long long)golden_c);
                st->mem_flag.store(1, std::memory_order_release);
            }
        }
    }
    for (int i = 0; i < size; i++) {
        if (m_tester_p->b1[i] != m_tester_p->b2[i] || m_tester_p->b3[i] != m_tester_p->b1[i] ||
            m_tester_p->b4[i] != m_tester_p->b1[i] || m_tester_p->b5[i] != m_tester_p->b1[i]) {
            /* SDC detected: log with the framework, set the flag, keep the
             * run going (do NOT abort the thread - we want the full window
             * of corruption captured). test_cleanup turns the flag into a
             * real report_fail. thread_num is this worker's CPU index. */
            log_error("memcpy_rewr_neon: SDC on CPU %d: b1=0x%x b2=0x%x b3=0x%x "
                      "b4=0x%x b5=0x%x idx=%d",
                      device_info[thread_num].cpu_number, m_tester_p->b1[i], m_tester_p->b2[i],
                      m_tester_p->b3[i], m_tester_p->b4[i], m_tester_p->b5[i], i);
            st->mem_flag.store(1, std::memory_order_release);
        }
    }
}

/* ---- IOT scheduler (linked-list MPSC queue, unchanged algorithm) ---- */

static int init_iot(memcpy_rewr_state *st)
{
    iot_conf_t *conf = &st->conf;
    int ret = -1;
    if ((ret = pthread_cond_init(&conf->cond, NULL)) != 0)
        goto out;
    conf->cond_inited = _gf_true;
    if ((ret = pthread_mutex_init(&conf->mutex, NULL)) != 0)
        goto out;
    conf->mutex_inited = _gf_true;
    for (int i = 0; i < IOT_PRI_MAX; i++) {
        INIT_LIST_HEAD(&conf->clients[i]);
        INIT_LIST_HEAD(&conf->no_client[i].clients);
        INIT_LIST_HEAD(&conf->no_client[i].reqs);
    }
    conf->ac_iot_limit[IOT_PRI_HI] = 64;
    conf->ac_iot_limit[IOT_PRI_NORMAL] = 64;
    conf->ac_iot_limit[IOT_PRI_LO] = 64;
    conf->ac_iot_limit[IOT_PRI_LEAST] = 64;
    conf->idle_time = 64;
    conf->down = _gf_false;
    ret = 0;
out:
    return ret;
}

static call_stub_t *__iot_dequeue(iot_conf_t *conf, int *pri)
{
    call_stub_t *stub = NULL;
    int i = 0;
    iot_client_ctx_t *ctx;
    *pri = -1;
    for (i = 0; i < IOT_PRI_MAX; i++) {
        if (conf->ac_iot_count[i] >= conf->ac_iot_limit[i])
            continue;
        if (list_empty(&conf->clients[i]))
            continue;
        ctx = list_first_entry(&conf->clients[i], iot_client_ctx_t, clients);
        if (!ctx)
            continue;
        if (list_empty(&ctx->reqs))
            continue;
        stub = list_first_entry(&ctx->reqs, call_stub_t, list);
        list_del_init(&stub->list);
        if (list_empty(&ctx->reqs)) {
            list_del_init(&ctx->clients);
        } else {
            list_rotate_left(&conf->clients[i]);
        }
        prefetchw(&conf->ac_iot_count[i]);
        conf->ac_iot_count[i]++;
        *pri = i;
        break;
    }
    if (!stub)
        return NULL;
    conf->queue_size--;
    conf->queue_sizes[*pri]--;
    return stub;
}

static void __iot_enqueue(memcpy_rewr_state *st, iot_conf_t *conf,
                          call_stub_t *stub, int pri)
{
    iot_client_ctx_t *ctx = NULL;
    if (pri < 0 || pri >= IOT_PRI_MAX)
        pri = IOT_PRI_MAX - 1;
    if (!ctx)
        ctx = &conf->no_client[pri];
    if (list_empty(&ctx->reqs))
        list_add_tail(&ctx->clients, &conf->clients[pri]);
    list_add_tail(&stub->list, &ctx->reqs);
    conf->queue_size++;
    conf->queue_sizes[pri]++;
    func_data_make(st, st->g_size);
}

static void *iot_worker(memcpy_rewr_state *st, struct test *test)
{
    iot_conf_t *conf = &st->conf;
    call_stub_t *stub = NULL;
    struct timespec sleep_till = { 0, };
    int ret = 0;
    int pri = -1;
    /* Short wait timeout so a consumer parked on an empty queue still wakes
     * up frequently enough to honour the framework's time budget
     * (test_time_condition). The upstream idle_time (64s) would park a
     * worker well past the test slot and deadlock shutdown. */
    const time_t wait_secs = 1;
    /* issue #374: snapshot of the (b1, golden_cycles) pair taken inside the
     * same critical section as the b1 copy; `copied` guards the
     * never-received-a-block case (see func_struct_test2). */
    uint64_t c_snap = 0;
    bool copied = false;
    for (;;) {
        sleep_till.tv_sec = time(NULL) + wait_secs;
        sleep_till.tv_nsec = 0;
        bool stop = false;
        pthread_mutex_lock(&conf->mutex);
        {
            if (pri != -1) {
                conf->ac_iot_count[pri]--;
                pri = -1;
            }
            while (conf->queue_size == 0) {
                if (conf->down || !test_time_condition(test)) {
                    stop = true;
                    break;
                }
                conf->sleep_count++;
                ret = pthread_cond_timedwait(&conf->cond, &conf->mutex, &sleep_till);
                conf->sleep_count--;
                if (conf->down || !test_time_condition(test)) {
                    stop = true;
                    break;
                }
                if (ret != 0 && conf->queue_size == 0) {
                    /* timed out with nothing to do; loop back and re-check
                     * the time budget without holding the mutex. */
                    stop = false;
                    /* re-arm the timeout for the next iteration */
                    sleep_till.tv_sec = time(NULL) + wait_secs;
                    ret = 0;
                    continue;
                }
            }
            if (!stop) {
                st->product.fetch_add(1, std::memory_order_relaxed);
                func_struct_test1(st, st->g_size);
                c_snap = st->golden_cycles;   /* issue #374: same lock as b1 */
                copied = true;
                stub = __iot_dequeue(conf, &pri);
            }
        }
        pthread_mutex_unlock(&conf->mutex);
        func_struct_test2(st, st->g_size, c_snap, copied);
        /* The upstream tool did __wake(&stub->args) here to release a producer
         * blocked in SYNCOP's __yield. Since our producers no longer block
         * (fire-and-forget into the MPSC queue) the stub's ownership now
         * ends here: __iot_dequeue already list_del_init'd it, so the
         * consumer that dequeued it frees it. */
        if (stub) {
            free(stub);
            stub = NULL;
        }
        if (stop || !test_time_condition(test))
            break;
    }
    return NULL;
}

static int do_iot_schedule(memcpy_rewr_state *st, iot_conf_t *conf,
                           call_stub_t *stub, int pri)
{
    /* issue #374: block-constant fill — one timer read per block, recorded
     * in st->golden_cycles, so the consumer's expected bytes are
     * arithmetically recomputable (i ^ c). Same data class as before (time-
     * varying across blocks, position-dependent within a block). */
    const uint64_t c = myget_cycles();
    for (int i = 0; i < st->g_size; i++)
        m_tmp_p[i] = static_cast<unsigned char>(static_cast<uint64_t>(i) ^ c);
    pthread_mutex_lock(&conf->mutex);
    {
        /* The golden_cycles write and func_data_make's gl_tester.b1 memcpy
         * (inside __iot_enqueue) share this critical section, so a consumer
         * always snapshots a consistent (b1, golden_cycles) pair. */
        st->golden_cycles = c;
        __iot_enqueue(st, conf, stub, pri);
        pthread_cond_signal(&conf->cond);
    }
    pthread_mutex_unlock(&conf->mutex);
    func_memtest(st, st->g_size_t);
    return 0;
}

/*
 * In the upstream tool SYNCOP is a synchronous RPC: the producer enqueues a
 * request and blocks in __yield() until the consumer __wake()s it. That works
 * with one or a handful of producers, but under SDCShield's fullsystem mode
 * the framework owns the time budget: when the test slot elapses it asks
 * threads to stop via test_time_condition(), but a producer stuck in __yield's
 * unbounded pthread_cond_wait would never notice and deadlock. We keep the
 * enqueue + signal path (which drives the MPSC queue and thus the consumer's
 * memcpy SDC check) but drop the unbounded wait: the producer fires the
 * request into the queue and moves on. The consumer still runs func_struct_
 * test1/test2 on every dequeue, so the coherence/SDC stress is preserved.
 */
#define SYNCOP(st, conf, stb, pri)           \
    do {                                     \
        do_iot_schedule(st, conf, stb, pri); \
    } while (0)

static void *thread_producer(memcpy_rewr_state *st, struct test *test)
{
    iot_conf_t *conf = &st->conf;
    int i = 0;
    for (;;) {
        call_stub_t *stub = static_cast<call_stub_t *>(malloc(sizeof(call_stub_t)));
        INIT_LIST_HEAD(&stub->list);
        SYNCOP(st, conf, stub, (i++) % IOT_PRI_MAX);
        /* Ownership of stub transferred to the MPSC queue; the consumer that
         * dequeues it frees it. Do NOT free here (use-after-free). */
        if (!test_time_condition(test))
            break;
    }
    return NULL;
}

/* ---- role assignment from CPU topology (replaces the CLI args) ---- */

enum role { ROLE_PRODUCER, ROLE_CONSUMER };

static role role_for_cpu(memcpy_rewr_state *st, int cpu)
{
    const std::string *rr = st->strategy->get("role_rule");
    const std::string &r = rr ? *rr : std::string("");

    if (r == "numa_split") {
        /* Producer if this CPU's NUMA id is in the lower half of the sorted
         * set of visible NUMA ids; consumer otherwise. On a 2-node box this
         * is node0->producer, node1->consumer (cross-die barrage). */
        int nid = device_info[cpu].numa_id;
        size_t half = st->numa_ids_sorted.size() / 2;
        for (size_t k = 0; k < half; ++k)
            if (st->numa_ids_sorted[k] == nid)
                return ROLE_PRODUCER;
        return ROLE_CONSUMER;
    }
    if (r == "die_even_odd") {
        /* Strategy 2: intra-die L3 / ring-bus brawl. On this Kunpeng 920
         * board sysfs topology/die_id is unavailable (always -1, see the
         * platform quirks in CLAUDE.md); the real Die/L3 granularity is
         * module_id (a.k.a. Linux cluster_id, 4 cores per cluster). We
         * split each cluster into even/odd-core_id halves so producers
         * and consumers share the same L3 slice and ring bus, keeping the
         * coherence brawl inside one Die instead of crossing the fabric. */
        int cid = device_info[cpu].core_id;
        return (cid % 2 == 0) ? ROLE_PRODUCER : ROLE_CONSUMER;
    }
    if (r == "first_n_producers") {
        /* First producer_count CPUs (in device_info order) produce. */
        return (cpu < st->producer_count) ? ROLE_PRODUCER : ROLE_CONSUMER;
    }
    /* Unknown rule: fall back to even/odd thread index. */
    return (cpu % 2 == 0) ? ROLE_PRODUCER : ROLE_CONSUMER;
}

/* ---- default-mode parameters (no conf file: original-tool semantics) ----
 * Read MemAvailable from /proc/meminfo, in KiB; -1 if unreadable.
 * Each meminfo line is "Key:  value kB" — consume the whole line so the
 * trailing unit token cannot desynchronise the fscanf scan.
 */
static long read_mem_available_kib(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;
    long val = -1;
    char key[64];
    while (fscanf(f, "%63s %ld", key, &val) == 2) {
        if (strcmp(key, "MemAvailable:") == 0)
            break;
        val = -1;
        int c;
        while ((c = fgetc(f)) != EOF && c != '\n')
            ;
    }
    fclose(f);
    return val;
}

/*
 * Default-mode block_size: scale the per-transfer payload to the largest
 * value the machine safely affords. Budget = half of MemAvailable spread
 * over all worker threads, each touching six block_size fronts (consumer:
 * m_tester b1..b5; producer: m_tmp — 6 is a conservative upper bound since
 * a thread only ever touches its own role's buffers). Clamped to
 * [64 KiB (reference-table floor), c_size (struct field width — also the
 * original tool's g_size>c_size overflow boundary)]. Returns bytes.
 */
static int default_block_size(int threads)
{
    const long lo = 64L * 1024;
    const long hi = (long)c_size;          /* 2 MiB */
    long mem_kib = read_mem_available_kib();
    if (mem_kib <= 0 || threads < 1)
        return (int)lo;                    /* unreadable/minimal: floor */
    long bytes = mem_kib * 1024L;          /* available bytes   */
    bytes /= 2;                            /* budget: half      */
    bytes /= threads;                      /* per worker thread */
    bytes /= 6;                            /* touched fronts    */
    if (bytes < lo)
        return (int)lo;
    if (bytes > hi)
        return (int)hi;
    return (int)bytes;
}

/* ---- SDCShield test entry points ---- */

static int memcpy_rewr_init(struct test *test)
{
    if (thread_count() < 2) {
        log_skip(CpuTopologyIssueSkipCategory,
                 "memcpy_rewr_neon requires at least 2 threads (producer+consumer); "
                 "skipping on this thread count");
        return EXIT_SKIP;
    }

    auto *st = new (std::nothrow) memcpy_rewr_state;
    if (!st)
        return EXIT_FAILURE;
    /* The struct holds atomics/vectors/strings (non-trivial), so value-init
     * instead of memset. Fields with non-trivial default constructors (atomics
     * default to 0; vectors/strings default empty) come up correctly; the
     * plain-old-data buffers (a1..a4, gl_tester) need not be zeroed. */
    st->g_size = 0;
    st->g_size_t = 0;
    st->strategy = nullptr;
    st->strategy_index = 0;
    st->producer_count = 4;
    st->mem_flag.store(0, std::memory_order_relaxed);
    st->product.store(0, std::memory_order_relaxed);

    /* Locate the default config beside this source file. The meson build
     * injects -DMEMCPY_REWR_SRC_DIR=<absolute source dir of the memory/>
     * subdir so the path is valid regardless of the caller's CWD (the
     * framework may run from a build dir or a different working directory).
     * Fall back to deriving from __FILE__ if the macro wasn't supplied. */
    std::string default_conf;
#ifdef MEMCPY_REWR_SRC_DIR
    default_conf = std::string(MEMCPY_REWR_SRC_DIR) + "/memcpy_rewr_strategies.conf";
#else
    {
        std::string src_path = __FILE__;
        std::string::size_type slash = src_path.find_last_of('/');
        std::string dir = (slash == std::string::npos) ? "." : src_path.substr(0, slash);
        default_conf = dir + "/memcpy_rewr_strategies.conf";
    }
#endif

    /* Resolve the effective conf path exactly as strategy_config_load
     * would (env override wins, even if empty), so the existence probe
     * below cannot disagree with the loader. */
    std::string conf_path = default_conf;
    if (const char *ov = std::getenv("SANDSTONE_STRATEGY_CONF"))
        conf_path = ov;

    const Strategy *s = nullptr;
    size_t idx = 0;
    const int probe_errno = (access(conf_path.c_str(), R_OK) != 0) ? errno : 0;
    const bool conf_absent = (probe_errno == ENOENT || probe_errno == ENOTDIR);
    if (!conf_absent) {
        /* A config is present (or present-but-unreadable): load it; open
         * and parse failures still skip loudly, exactly as before. */
        if (!strategy_config_load(st->strategy_set, conf_path)) {
            log_skip(TestResourceIssueSkipCategory,
                     "memcpy_rewr_neon: strategy config error: %s",
                     st->strategy_set.error.c_str());
            delete st;
            return EXIT_SKIP;
        }
        s = strategy_config_pick(st->strategy_set, "SANDSTONE_STRATEGY_INDEX", &idx);
        if (!s) {
            log_skip(TestResourceIssueSkipCategory,
                     "memcpy_rewr_neon: no strategies available");
            delete st;
            return EXIT_SKIP;
        }
    } else {
        /* No config file anywhere: degrade to the original standalone
         * tool's semantics instead of skipping. Roles split
         * first-N-producers (N = max(1, threads/12) — a 48-core box gets
         * the reference 4:44 split) via the existing role_rule code path;
         * block_size scales with MemAvailable (see default_block_size).
         * SANDSTONE_STRATEGY_INDEX has nothing to index here and is
         * ignored. */
        st->strategy_set.strategies.push_back(Strategy{});
        Strategy &d = st->strategy_set.strategies.back();
        d.name = "default_original";
        d.params["role_rule"] = "first_n_producers";
        d.params["producer_count"] =
            std::to_string(std::max(1, thread_count() / 12));
        d.params["block_size"] =
            std::to_string(default_block_size(thread_count()));
        s = &d;
        idx = 0;
        log_info("memcpy_rewr_neon: no strategy config at %s (errno %d); "
                 "using original-tool default mode",
                 conf_path.c_str(), probe_errno);
    }

    st->strategy = s;
    st->strategy_index = idx;
    st->g_size = static_cast<int>(s->get_long("block_size", 65536));
    st->g_size_t = static_cast<int>(s->get_long("block_size", 65536));
    st->producer_count = static_cast<int>(s->get_long("producer_count", 4));

    /* Build the sorted-unique NUMA-id set across the visible CPUs. */
    {
        std::vector<int> ids;
        int n = device_count();
        ids.reserve(n);
        for (int i = 0; i < n; ++i)
            ids.push_back(device_info[i].numa_id);
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        st->numa_ids_sorted = std::move(ids);
    }

    if (st->g_size <= 0 || st->g_size > c_size) {
        log_skip(TestResourceIssueSkipCategory,
                 "memcpy_rewr_neon: block_size %d out of range (1..%d)", st->g_size, (int)c_size);
        delete st;
        return EXIT_SKIP;
    }

    if (init_iot(st) != 0) {
        delete st;
        return EXIT_FAILURE;
    }

    /* Seed the shared producer source with a recognisable pattern once.
     * issue #374: block-constant c0 (one timer read, recorded in
     * st->golden_cycles) so the consumer can recompute the expected bytes
     * arithmetically (i ^ c0) — a register-ALU golden independent of every
     * memcpy/LSU transport in the chain under test. Before any producer
     * runs, consumers copying this seed verify against c0 consistently. */
    const uint64_t c0 = myget_cycles();
    for (int i = 0; i < st->g_size; i++)
        st->gl_tester.b1[i] = static_cast<unsigned char>(static_cast<uint64_t>(i) ^ c0);
    st->golden_cycles = c0;

    test->data = st;

    log_info("memcpy_rewr_neon: strategy[%zu]=%s block_size=%d threads=%d "
             "producer_count=%d numa_nodes=%zu arch_timer_freq=%lu",
             idx, s->name.c_str(), st->g_size, thread_count(),
             st->producer_count, st->numa_ids_sorted.size(),
             user_archtimer_get_cntfrq());
    return EXIT_SUCCESS;
}

static int memcpy_rewr_run(struct test *test, int cpu)
{
    auto *st = static_cast<memcpy_rewr_state *>(test->data);
    /* Heap-allocate this thread's ~12 MiB of working buffers (see the
     * thread_local note above); the stack is far too small for them. */
    if (!ensure_thread_buffers()) {
        log_error("memcpy_rewr_neon: out of memory for per-thread buffers on cpu %d",
                  device_info[cpu].cpu_number);
        return EXIT_FAILURE;
    }
    role r = role_for_cpu(st, cpu);
    if (r == ROLE_PRODUCER)
        thread_producer(st, test);
    else
        iot_worker(st, test);
    return EXIT_SUCCESS;
}

static int memcpy_rewr_cleanup(struct test *test)
{
    auto *st = static_cast<memcpy_rewr_state *>(test->data);
    if (!st)
        return EXIT_SUCCESS;

    /* Tear down the scheduler: wake any sleeping consumers so they exit. */
    iot_conf_t *conf = &st->conf;
    if (conf->mutex_inited) {
        pthread_mutex_lock(&conf->mutex);
        conf->down = _gf_true;
        pthread_cond_broadcast(&conf->cond);
        /* Drain any stubs still queued (producers fire-and-forget into the
         * MPSC queue, so the queue may be non-empty at shutdown). */
        for (int p = 0; p < IOT_PRI_MAX; ++p) {
            while (!list_empty(&conf->clients[p])) {
                iot_client_ctx_t *ctx = list_first_entry(&conf->clients[p],
                                                         iot_client_ctx_t, clients);
                while (!list_empty(&ctx->reqs)) {
                    call_stub_t *stub = list_first_entry(&ctx->reqs,
                                                         call_stub_t, list);
                    list_del_init(&stub->list);
                    free(stub);
                }
                list_del_init(&ctx->clients);
            }
            /* also drain the no_client per-priority fallback queue */
            while (!list_empty(&conf->no_client[p].reqs)) {
                call_stub_t *stub = list_first_entry(&conf->no_client[p].reqs,
                                                     call_stub_t, list);
                list_del_init(&stub->list);
                free(stub);
            }
        }
        pthread_mutex_unlock(&conf->mutex);
    }

    bool failed = (st->mem_flag.load(std::memory_order_acquire) != 0);

    if (conf->cond_inited)
        pthread_cond_destroy(&conf->cond);
    if (conf->mutex_inited)
        pthread_mutex_destroy(&conf->mutex);

    delete st;
    test->data = nullptr;

    if (failed) {
        report_fail_msg("memcpy_rewr_neon: silent data corruption detected in "
                         "mem_info_s memcpy (b1..b5 mismatch)");
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(memcpy_rewr_neon,
             "MPSC memcpy coherence stress (GlusterFS-IOT scheduler, ARM64 arch-timer); "
             "producer/consumer roles by NUMA/die strategy, detects SDC in b1..b5 copies")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = memcpy_rewr_init,
    .test_run = memcpy_rewr_run,
    .test_cleanup = memcpy_rewr_cleanup,
    .fracture_loop_count = -1,   /* never fracture (arm_add convention) */
    .quality_level = TEST_QUALITY_PROD,
    .flags = static_cast<test_flags>(test_schedule_fullsystem),
END_DECLARE_TEST

#else  /* !__aarch64__ */
/*
 * ARM64-only: the arch-timer inline asm (cntvct_el0/cntfrq_el0) has no x86
 * counterpart and the x86-64 paths must stay untouched. The test is built
 * and registered only on aarch64 via the meson guard, so no placeholder
 * skip is needed here.
 */
#endif /* __aarch64__ */
