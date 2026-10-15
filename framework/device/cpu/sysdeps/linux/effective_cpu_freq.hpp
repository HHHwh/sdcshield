/*
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LINUX_EFFECTIVE_FREQ_HPP
#define LINUX_EFFECTIVE_FREQ_HPP

#if !defined(__x86_64__) && !defined(__aarch64__)
#  include "../generic/effective_cpu_freq.hpp"
#elif defined(__x86_64__)
#  include "sandstone_p.h"
#  include <limits>
#  include <x86intrin.h>

class CPUTimeFreqStamp
{
public:
    static constexpr uint32_t APERF_MSR{ 0xe8 };
    static constexpr uint32_t MPERF_MSR{ 0xe7 };

    void Snapshot(const int thread_num)
    {
        cpu_number = device_info[thread_num].cpu_number;
        ns = MonotonicTimePoint::clock::now();
        tsc = __rdtscp(&tsc_aux);

        if (!read_msr(cpu_number, APERF_MSR, &aperf))
            aperf = 0;

        if (!read_msr(cpu_number, MPERF_MSR, &mperf))
            mperf = 0;
    }

    static double EffectiveFrequencyMHz(const CPUTimeFreqStamp& before, const CPUTimeFreqStamp& after)
    {
        assert(after.cpu_number == before.cpu_number);

        // Case of bogus data when, e.g., SDCShield is run unprivileged
        if (before.mperf >= after.mperf || before.aperf >= after.aperf || before.tsc >= after.tsc
                || before.ns >= after.ns)
            return std::numeric_limits<double>::quiet_NaN();

        const auto nsecs = after.ns - before.ns;
        const double secs = std::chrono::duration_cast<std::chrono::duration<double>>(nsecs).count();
        const double tsc_freq = (after.tsc - before.tsc) / secs;
        const double perf_ratio = 1.0 * (after.aperf - before.aperf) / (after.mperf - before.mperf);

        return tsc_freq * perf_ratio / 1000000.0;
    }

private:
    int cpu_number;
    uint32_t tsc_aux;
    MonotonicTimePoint ns;
    uint64_t tsc;
    uint64_t aperf, mperf;
};

#else // __aarch64__
/*
 * On AArch64 there is no TSC and no APERF/MPERF MSR to derive an
 * "effective vs nominal" frequency ratio. The closest portable
 * equivalent is the ARM PMU cpu_cycles counter exposed through
 * perf_event_open(): counting actual core cycles over a wall-clock
 * interval gives the real (effective) core frequency directly:
 *
 *   freq = delta_cycles / delta_seconds
 *
 * A per-thread perf file descriptor (kept alive across the two
 * Snapshot() calls) accumulates cycles; PERF_FORMAT_TOTAL_TIME_ENABLED
 * gives the time the counter was active, which we cross-check against
 * the monotonic clock.  If perf_event_open() is unavailable (e.g. the
 * perf_paranoid sysctl or an old kernel), we fall back to NaN like the
 * generic stub.
 */
#  include "sandstone_p.h"
#  include <fcntl.h>
#  include <limits>
#  include <string.h>
#  include <sys/ioctl.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#  include <linux/perf_event.h>

class CPUTimeFreqStamp
{
public:
    void Snapshot(const int thread_num)
    {
        cpu_number = device_info[thread_num].cpu_number;
        ns = MonotonicTimePoint::clock::now();
        cycles = read_perf_cycles(cpu_number);
    }

    static double EffectiveFrequencyMHz(const CPUTimeFreqStamp& before, const CPUTimeFreqStamp& after)
    {
        if (before.cycles == UINT64_MAX || after.cycles == UINT64_MAX)
            return std::numeric_limits<double>::quiet_NaN();   // perf unavailable

        if (before.ns >= after.ns || after.cycles < before.cycles)
            return std::numeric_limits<double>::quiet_NaN();

        const auto nsecs = after.ns - before.ns;
        const double secs = std::chrono::duration_cast<std::chrono::duration<double>>(nsecs).count();
        if (secs <= 0.0)
            return std::numeric_limits<double>::quiet_NaN();

        return (after.cycles - before.cycles) / secs / 1000000.0;
    }

private:
    int cpu_number = -1;
    MonotonicTimePoint ns;
    uint64_t cycles = 0;

    /* A persistent per-thread perf fd accumulates the calling thread's
     * cpu_cycles across the two Snapshot() calls. The fd is cached
     * thread-locally keyed by the observed cpu_number; the same fd is
     * read at both snapshots so the delta is meaningful. */
    struct PerfFd {
        int cpu_number = -1;
        int fd = -1;
        ~PerfFd() { if (fd >= 0) close(fd); }
    };
    static thread_local PerfFd cached_fd;

    static uint64_t read_perf_cycles(int cpu_number)
    {
        // (Re)open the counter if this snapshot is for a different CPU than
        // the cached fd (e.g. after rescheduling) or if it was never opened.
        if (cached_fd.cpu_number != cpu_number) {
            if (cached_fd.fd >= 0)
                close(cached_fd.fd);
            cached_fd.fd = -1;
            cached_fd.cpu_number = cpu_number;

            struct perf_event_attr pe;
            memset(&pe, 0, sizeof(pe));
            pe.type = PERF_TYPE_HARDWARE;
            pe.size = sizeof(pe);
            pe.config = PERF_COUNT_HW_CPU_CYCLES;
            pe.disabled = 0;          // start counting immediately
            pe.exclude_kernel = 1;
            pe.exclude_hv = 1;
            pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED;

            /* pid=0 counts the *calling* thread (A-001): the previous
             * argument order opened pid=cpu_number / cpu=-1, attaching
             * to whichever unrelated task happened to have
             * TID == cpu_number. cpu=-1 keeps the counter following
             * the thread. */
            cached_fd.fd = syscall(__NR_perf_event_open, &pe, /*pid=*/0, /*cpu=*/-1, -1, 0);
            if (cached_fd.fd < 0) {
                cached_fd.fd = -1;
                return UINT64_MAX;   // signal "unavailable"
            }
        }

        if (cached_fd.fd < 0)
            return UINT64_MAX;

        struct {
            uint64_t value;          // cpu_cycles count
            uint64_t time_enabled;   // ns the counter was active
        } read_data;
        ssize_t n = read(cached_fd.fd, &read_data, sizeof(read_data));
        if (n != sizeof(read_data))
            return UINT64_MAX;
        return read_data.value;
    }
};

thread_local CPUTimeFreqStamp::PerfFd CPUTimeFreqStamp::cached_fd{};

#endif // __x86_64__ / __aarch64__

#endif // LINUX_EFFECTIVE_FREQ_HPP
