/*
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal page-size helpers -- the single canonical home of ROUND_UP_TO,
 * sandstone_page_size() and ROUND_UP_TO_PAGE.
 *
 * Included by both sandstone_p.h (framework proper) and
 * sandstone_test_utils.h (unit-tested constructs), so the implementation
 * the unit tests exercise is byte-for-byte the one the framework's
 * mmap/mprotect users run.  Keep this header usable from C and C++ and
 * free of any include other than <unistd.h>, <stdio.h> and <stdlib.h>
 * (the fatal helper reports straight to stderr -- logging may not be
 * initialised yet where the page size is first needed; on Windows
 * <unistd.h> resolves to the sysdeps/windows shim, whose getpagesize()
 * is backed by GetSystemInfo).
 */

#ifndef INC_SANDSTONE_PAGE_H
#define INC_SANDSTONE_PAGE_H

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

/* align to a power of 2 */
#define ROUND_UP_TO(value, n)       (((value) + (n) - 1) & (~((n) - 1)))

/*
 * Page size used for mmap/mprotect alignment.  MUST match the system's actual
 * page size, otherwise mprotect() will round to the kernel's page boundary and
 * may make adjacent data read-only (causing SIGSEGV on kernels with >4 KB
 * pages, e.g. ARM64 16 KB / 64 KB configurations).  Resolve once via
 * sysconf(_SC_PAGESIZE) (getpagesize() through the sysdeps/windows shim on
 * Windows) and cache it.  The cache is an atomic lazy-init: per-CPU worker
 * threads race on the first call (issues #446/#589), so loads/stores go
 * through __atomic_ builtins on BOTH branches and neither caches an
 * unvalidated value.
 */
static inline bool sandstone_is_valid_page_size(long page_size)
{
    return page_size > 0 && (page_size & (page_size - 1)) == 0;
}

/* There is no safe fallback page size (a hardcoded one breaks kernels with
 * bigger pages, see the comment above) and logging may not be initialized
 * yet, so write the diagnosis to stderr directly and abort — like
 * malloc.cpp's null_pointer_consumption().  Fail loudly, never cache garbage. */
static __attribute__((unused, noinline, noreturn)) void sandstone_page_size_fatal(long page_size, const char *source)
{
    fprintf(stderr, "internal error: unusable page size from %s: 0x%lx\n",
            source, (unsigned long)page_size);
    abort();
}

#ifdef __cplusplus
extern "C++"
{
#endif
static inline unsigned sandstone_page_size(void)
{
    static long cached = 0;
    long page_size = __atomic_load_n(&cached, __ATOMIC_ACQUIRE);
    if (__builtin_expect(!page_size, false)) {
        const char *source;
#ifdef _WIN32
        source = "getpagesize()";             /* sysdeps/windows/unistd.h shim */
        page_size = getpagesize();
#else
        source = "sysconf(_SC_PAGESIZE)";
        page_size = sysconf(_SC_PAGESIZE);
#endif
        if (__builtin_expect(!sandstone_is_valid_page_size(page_size), false))
            sandstone_page_size_fatal(page_size, source);
        __atomic_store_n(&cached, page_size, __ATOMIC_RELEASE);
    }
    return (unsigned)page_size;
}
#ifdef __cplusplus
}
#endif

#define ROUND_UP_TO_PAGE(value)     ROUND_UP_TO(value, sandstone_page_size())

#endif /* INC_SANDSTONE_PAGE_H */
