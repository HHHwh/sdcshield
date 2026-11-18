/*
 * Copyright 2023 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sandstone_thread.h"
#include "sandstone_p.h"

#include "gettid.h"

#include <sys/mman.h>

#if defined(__linux__) && !defined(MADV_GUARD_INSTALL)
#  define MADV_GUARD_INSTALL    102     /* since 6.13 */
#endif

namespace {
struct SandstoneTestThreadAttributes
{
    /*
     * Guard region between per-thread stacks.  MUST be at least as large as
     * the kernel page size and, like THREAD_STACK_SIZE, be a multiple of it:
     * every mprotect()/madvise() address below is mmap_base + N * (guard +
     * THREAD_STACK_SIZE) [+ guard], which stays page-aligned only while that
     * holds.  Linux REJECTS a non-page-aligned address with EINVAL -- it
     * does NOT round it down -- so a geometry mismatch leaves the stacks
     * stuck PROT_NONE (mprotect path) or without guards between them
     * (madvise path), crashing the next pthread_create() inside its
     * explicit_bzero.  A swallowed EINVAL used to hide this until issue
     * #396; the setup calls below now exit loudly on failure.
     */
    static size_t GuardSize()
    {
        size_t pg = sandstone_page_size();
        return (8192 < pg) ? pg : 8192;
    }
    pthread_attr_t thread_attr;
    unsigned char *stacks_block;
    SandstoneTestThreadAttributes()
        : stacks_block(allocate_stack_block())
    {
        pthread_attr_init(&thread_attr);
        pthread_attr_setstacksize(&thread_attr, THREAD_STACK_SIZE);

    }
    ~SandstoneTestThreadAttributes()
    {
        pthread_attr_destroy(&thread_attr);
    }
    SandstoneTestThreadAttributes(const SandstoneTestThreadAttributes &) = delete;
    SandstoneTestThreadAttributes &operator=(const SandstoneTestThreadAttributes &) = delete;

    static unsigned char *allocate_stack_block();
    void update_with_stack_for(int thread);
};

#ifndef _WIN32
#include <sysexits.h>
/*
 * Thread-stack guard setup must not fail silently: Linux rejects a
 * non-page-aligned mprotect()/madvise() address with EINVAL (it does not
 * round it down), so a swallowed failure leaves the stacks PROT_NONE or
 * guard-less and the next pthread_create() crashes with no prior message
 * (issue #396).  Report and exit instead.
 *
 * _exit(EX_SOFTWARE), not abort(): this runs inside the forked test child
 * (the static thread_attributes below first constructs there), and the
 * parent's test_result_from_exit_code() classifies a SIGABRT as "crash"
 * (Killed/CoreDumped) -- indistinguishable from a genuine test segfault in
 * the machine-readable result -- while a plain exit status other than
 * -1/0/1 becomes OperatingSystemError, i.e. a framework problem, which is
 * what this is.  And _exit rather than exit, because the child is midway
 * through a static constructor and must not run destructors of globals.
 */
[[noreturn]] static void thread_stack_setup_failed(const char *what, const void *addr)
{
    const char *err = strerror_for_mmap();     // capture errno before fprintf
    fprintf(stderr, "%s: %s(%p) failed: %s (page size %u, guard %zu)\n",
            program_invocation_name, what, addr, err,
            sandstone_page_size(), SandstoneTestThreadAttributes::GuardSize());
    _exit(EX_SOFTWARE);
}
#endif
} // unnamed namespace

#ifndef _WIN32
unsigned char *SandstoneTestThreadAttributes::allocate_stack_block()
{
    size_t guard = GuardSize();
    size_t size = thread_count() * (THREAD_STACK_SIZE + guard) + guard;
    void *map = mmap(nullptr, size, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (map == MAP_FAILED) {
        /*
         * Safe fallback (the default stacks each carry a one-page guard), but
         * not a silent one: the enhanced guard geometry would be lost with no
         * trace, the same blind spot issue #396 condemns.  One diagnostic
         * line per forked test child: thread_attributes is a function-local
         * static first constructed in each child, so a persistently failing
         * machine prints one line per test, not one line for the whole run.
         */
        const char *err = strerror_for_mmap();  // capture errno before fprintf
        fprintf(stderr, "%s: mmap(%zu) for per-thread stacks failed: %s -- "
                        "falling back to the default thread stacks (no guard region between them)\n",
                program_invocation_name, size, err);
        return nullptr;
    }

    auto ptr = static_cast<unsigned char *>(map);

#ifdef MADV_GUARD_INSTALL
    // check if MADV_GUARD_INSTALL works
    if (madvise(map, guard, MADV_GUARD_INSTALL) == 0) {
        // yes, let's use it
        // we mark the entire region we've allocated as read-writable, then
        // mark specific regions as guards
        if (mprotect(map, size, PROT_READ | PROT_WRITE) != 0)
            thread_stack_setup_failed("mprotect", map);

        for (int i = 0; i < thread_count(); ++i) {
            ptr += guard + THREAD_STACK_SIZE;
            // and mark the above this thread's stack (below the next's) as a guard
            if (madvise(ptr, guard, MADV_GUARD_INSTALL) != 0)
                thread_stack_setup_failed("madvise(MADV_GUARD_INSTALL)", ptr);
        }

        return ptr;
    }
#endif

    for (int i = 0; i < thread_count(); ++i) {
        ptr += guard;
        if (mprotect(ptr, THREAD_STACK_SIZE, PROT_READ | PROT_WRITE) != 0)
            thread_stack_setup_failed("mprotect", ptr);
        ptr += THREAD_STACK_SIZE;
    }
    return ptr;
}

void SandstoneTestThreadAttributes::update_with_stack_for(int thread)
{
    if (!stacks_block)
        return;
    unsigned char *stacktop = stacks_block;
    stacktop -= (THREAD_STACK_SIZE + GuardSize()) * unsigned(thread);
    pthread_attr_setstack(&thread_attr, stacktop - THREAD_STACK_SIZE, THREAD_STACK_SIZE);
}
#else
unsigned char *SandstoneTestThreadAttributes::allocate_stack_block()
{
    return nullptr;
}
void SandstoneTestThreadAttributes::update_with_stack_for(int)
{
}
#endif


[[maybe_unused]] static bool check_run_from_correct_thread()
{
#ifdef __linux__
    // only tested for Linux
    return getpid() == gettid();
#else
    return true;
#endif
}

void SandstoneTestThread::start(RunnerFunction *f, int t)
{
    assert(check_run_from_correct_thread());

    static SandstoneTestThreadAttributes thread_attributes;
    auto runner = +[](void *ptr) {
        auto self = static_cast<SandstoneTestThread *>(ptr);
        ::thread_num = self->thread_num;
        sApp->test_thread_data(self->thread_num)->tid.store(gettid());
        auto clear_tid = scopeExit([self] { sApp->test_thread_data(self->thread_num)->tid.store(0, std::memory_order_relaxed); });
        return reinterpret_cast<void *>(self->target(self->thread_num));
    };

    thread_num = t;
    target = f;
    thread_attributes.update_with_stack_for(t);
    pthread_create(&thread, &thread_attributes.thread_attr, runner, this);
}

uintptr_t SandstoneTestThread::join()
{
    void *result;
    pthread_join(thread, &result);
    return uintptr_t(result);
}
