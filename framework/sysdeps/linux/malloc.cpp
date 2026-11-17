/*
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sandstone_p.h>

#include <errno.h>
#include <malloc.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

// Sanitizer builds must skip this override entirely (mirrors the existing
// ASan guard): the sanitizer runtime calls malloc through dlsym during its
// own initialization, which would land in this instrumented override and
// re-enter the still-uninitialized sanitizer runtime (SIGSEGV in
// __tsan_func_entry before main, observed 2026-10-31 on a TSan build).
// GCC-only macro, same as the ASan guard below it; a clang TSan build would
// need __has_feature(thread_sanitizer) instead (this repo builds with GCC).
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)

// For Sandstone, we'd like to always return zeroed memory to our tests, to
// ensure they are deerministic regardless of refactorings that may have added
// or removed memory allocations or deallocations. We could that in two ways:
// - by setting mallopt(M_PERTURB) to value 0xff. See mallopt(3) for more
//   information
// - by memset'ting the block after allocation
//
// However, glibc has bugs and doesn't apply the M_PERTURB setting correctly
// (see https://sourceware.org/bugzilla/show_bug.cgi?id=26731 and I think
// realloc() has problems too). Therefore, we always memset.

#  ifndef __GLIBC__
#    error "Whoa, this code only works with glibc!"
#  endif

extern "C" {
#define DECLARE_OVERRIDE(FUNC)  extern decltype(FUNC) __libc_ ## FUNC

// The following lines are parsed by the buildsystem, so be careful when
// modifying them. See extract-malloc.sh.
DECLARE_OVERRIDE(calloc);
DECLARE_OVERRIDE(malloc);
DECLARE_OVERRIDE(memalign);
DECLARE_OVERRIDE(posix_memalign);
DECLARE_OVERRIDE(pvalloc);
DECLARE_OVERRIDE(realloc);
DECLARE_OVERRIDE(valloc);

#undef DECLARE_OVERRIDE
}

static void *bzero_block(void *ptr, size_t len)
{
    return memset(ptr, 0, len);
}

// Reachability contract with test code (issue #640): this link-level
// override never returns NULL to callers — a failed allocation terminates
// the process right here (raise(SIGKILL) below 256 MiB, abort() above).
// operator new therefore never observes a NULL return and std::bad_alloc
// is structurally unreachable in production builds: every
// catch (const std::bad_alloc &) in the tests (e.g. the T10 eigen
// init down-grade / worker thread-level SKIP paths) fires only in
// sanitizer builds, where this whole override is compiled out by the
// __SANITIZE_* guard above. Production OOM always manifests as this
// crash (framework-reported OutOfMemory), never as a resource SKIP.
static __attribute__((noinline, noreturn)) void null_pointer_consumption(void *ptr, size_t size)
{
    static const char msg[] = "Out of memory condition\n";
    (void) ptr;
    IGNORE_RETVAL(write(STDERR_FILENO, msg, sizeof(msg) - 1));

    // if the size is non-silly, simulate the Linux OOM killer
    if (size < 256 * 1024 * 1024)
        raise(SIGKILL);
    abort();
}

static inline void *check_null_pointer(void *ptr, size_t size, size_t count = 1)
{
    if (__builtin_expect(!ptr, 0))
        null_pointer_consumption(ptr, size * count);
    return ptr;
}

static inline void *checked_allocation(size_t size, void *block)
{
    check_null_pointer(block, size);
    size = malloc_usable_size(block);
    return bzero_block(block, size);
}

// different from all the rest
int posix_memalign(void **newptr, size_t alignment, size_t size)
{
    *newptr = aligned_alloc(alignment, size);
    return *newptr ? 0 : errno;
}

// pvalloc is valloc rounding up to the actual page size
void *pvalloc(size_t bytes)
{
    bytes = ROUND_UP_TO_PAGE(bytes);
    return valloc(bytes);
}

void *aligned_alloc(size_t alignment, size_t size)
{
    return checked_allocation(size, __libc_memalign(alignment, size));
}

void *memalign(size_t alignment, size_t size)
{
    return checked_allocation(size, __libc_memalign(alignment, size));
}

void *valloc(size_t size)
{
    return checked_allocation(size, __libc_valloc(size));
}

void *malloc(size_t size)
{
    return checked_allocation(size, __libc_malloc(size));
}

void *calloc(size_t n, size_t size)
{
    // calloc always zeroes memory, so we don't need to do it ourselves
    return check_null_pointer(__libc_calloc(n, size), n, size);
}

void *realloc(void *ptr, size_t size)
{
    size_t oldsize = malloc_usable_size(ptr);
    void *newptr = check_null_pointer(__libc_realloc(ptr, size), size);
    size = malloc_usable_size(newptr);

    ptrdiff_t bytestoclear = size - oldsize;
    if (bytestoclear > 0)
        bzero_block(reinterpret_cast<uint8_t *>(newptr) + oldsize, bytestoclear);
    return newptr;
}
#endif // !__SANITIZE_ADDRESS__ && !__SANITIZE_THREAD__
