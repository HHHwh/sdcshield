/*
 * Copyright 2026 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Output is in "Test Anything Protocol" format, as per http://testanything.org/tap-specification.html
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "sandstone.h"
#include "sandstone_p.h"
#include "sandstone_system.h"
#include "sandstone_thread.h"
#include "sandstone_tests.h"
#include "sandstone_utils.h"
#include "forkfd.h"
#include "interrupt_monitor.hpp"
#include "topology.h"
#if SANDSTONE_DEVICE_CPU
#   include "effective_cpu_freq.hpp"
#endif

#include <chrono>
#include <map>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#if __has_include(<malloc.h>)
#  include <malloc.h>
#endif
#include <stdio.h>
#include <string.h>
#ifdef __unix__
#  include <poll.h>
#endif
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#if __has_include(<sys/auxv.h>)         // FreeBSD and Linux
#  include <sys/auxv.h>
#endif
#ifdef __linux__
#  include <sys/eventfd.h>
#  include <sys/types.h>
#endif
#ifdef __unix__
#  include <sys/resource.h>
#endif
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <device/cpu/arm64/arm64_spec_prepare.h>

#if defined(__aarch64__) && defined(__linux__)
/* T7 VmHWM 反馈闭环(spec §4.4): sdc_joint_cache(联合求解口径登记) +
 * sdc_read_vm_hwm。仅 arm64 门控 —— x86 的 Main::mem_feedback 字段编译
 * 存在但无写入/读取者(rule 5, 见 test_data.h)。 */
# include "sdc_stress_config.h"
# include "sdc_numa.h"
# include <algorithm>
#endif

#ifdef _WIN32
#  include <ntstatus.h>
#  include <shlwapi.h>
#  include <windows.h>
#  include <psapi.h>
#endif

using namespace std::chrono;
using namespace std::chrono_literals;

device_features_t device_features;

static const struct test *s_current_test = nullptr;

#ifdef __llvm__
thread_local int thread_num __attribute__((tls_model("initial-exec")));
#else
thread_local int thread_num = 0;
#endif

const struct test *current_test() noexcept
{
    return s_current_test;
}

static const char *path_to_exe()
{
#if defined(__linux__)
    return "/proc/self/exe";
#elif defined(_WIN32)
    // see https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulefilenamea
    return _pgmptr;
#else
    return sApp->path_to_self.c_str();
#endif
}

#ifndef _WIN32
static ChildExitStatus test_result_from_exit_code(forkfd_info info)
{
    ChildExitStatus r = {};
    r.extra = info.status;
    if (info.code == CLD_EXITED) {
        // normal exit
        int8_t status = info.status;    // the cast to int8_t transforms 255 into -1
        switch (status) {
        case -1:
            r.result = TestResult::Skipped;
            r.extra = 0;
            break;
        case EXIT_SUCCESS:
            r.result = TestResult::Passed;
            r.extra = 0;
            break;
        case EXIT_FAILURE:
            r.result = TestResult::Failed;
            r.extra = 0;
            break;
        default:
            // a FW problem getting started
            r.result = TestResult::OperatingSystemError;
            break;
        }
    } else if (info.code == CLD_KILLED || info.code == CLD_DUMPED) {
        r.result = info.code == CLD_KILLED ? TestResult::Killed : TestResult::CoreDumped;
        if (info.status == SIGKILL)
            r.result = TestResult::OutOfMemory;
        else if (info.status == SIGQUIT)
            r.result = TestResult::TimedOut;
    } else {
        assert(false && "Impossible condition; did we get a CLD_STOPPED??");
        __builtin_unreachable();
    }
    return r;
}
#else
static constexpr DWORD EXIT_TIMEOUT = 2;
static ChildExitStatus test_result_from_exit_code(DWORD status)
{
    // Exit code mapping:
    // -1: EXIT_SKIP:       TestResult::Skipped
    // 0: EXIT_SUCCESS:     TestResult::Passed
    // 1: EXIT_FAILURE:     TestResult::Failed
    // 2:                   TestResult::TimedOut
    // 3:                   Special case for abort()
    // 4-255:               exit() code (from sysexits.h)
    // anything else:       an NTSTATUS
    //
    // https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/abort?view=msvc-160
    // says: "If the Windows error reporting handler is not invoked, then abort
    // calls _exit to terminate the process with exit code 3"

    switch (status) {
    case DWORD(-1):
        return { TestResult::Skipped };
    case EXIT_SUCCESS:
        return { TestResult::Passed };
    case EXIT_FAILURE:
        return { TestResult::Failed };
    case EXIT_TIMEOUT:
        return { TestResult::TimedOut };
    case 3:
        return { TestResult::Killed, unsigned(STATUS_FAIL_FAST_EXCEPTION) };
    }
    if (status > 255)
        return { TestResult::Killed, status };

    // must be a FW problem getting started
    return { TestResult::OperatingSystemError, status };
}
#endif // _WIN32

static void merge_test_result(ChildExitStatus &current, const ChildExitStatus& incoming)
{
    // don't decrease a previously-stored severity
    if (current.result < incoming.result)
        current = incoming;
    current.endtime = MonotonicTimePoint::clock::now();
}

static void __attribute__((noreturn)) report_fail_common()
{
    logging_mark_thread_failed(thread_num);
#ifdef _WIN32
    /* does not call the cleanup handlers */
    _endthread();
#else
    /* does call the cleanup handalers */
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, nullptr);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nullptr);
    pthread_cancel(pthread_self());
#endif
    __builtin_unreachable();
}

void _report_fail(const struct test *test, const char *file, int line)
{
    /* Keep this very early */
    if (sApp->shmem->cfg.ud_on_failure)
        ud2();

    if (!SandstoneConfig::NoLogging)
        log_error("Failed at %s:%d", file, line);
    report_fail_common();
}

void _report_fail_msg(const char *file, int line, const char *fmt, ...)
{
    /* Keep this very early */
    if (sApp->shmem->cfg.ud_on_failure)
        ud2();

    if (!SandstoneConfig::NoLogging)
        log_error("Failed at %s:%d: %s", file, line, va_start_and_stdprintf(fmt).c_str());
    report_fail_common();
}

/* Like memcmp(), but returns the offset of the byte that differed (negative if equal) */
static ptrdiff_t memcmp_offset(const uint8_t *d1, const uint8_t *d2, size_t size)
{
    ptrdiff_t i = 0;

    for (; i < (ptrdiff_t) size; ++i) {
        if (d1[i] != d2[i])
            return i;
    }
    return -1;
}

#ifndef NDEBUG
// we can only rely on the CI for this
bool _memcmp_or_fail_check_fmt(const char *fmt, ...)
{
    std::string msg = va_start_and_stdprintf(fmt);
    if (msg.contains('\n'))
        log_yaml(SANDSTONE_LOG_DEBUG, ("memcmp_or_fail: " + msg).c_str());
    return true;
}

bool SandstoneMemcmpOrFail::test_formatter(std::function<std::string ()> cb, size_t)
{
    log_yaml(SANDSTONE_LOG_DEBUG, ("memcmp_or_fail: " + cb()).c_str());
    return true;
}

bool SandstoneMemcmpOrFail::test_formatter(std::function<std::string (ptrdiff_t)> cb, size_t max)
{
    auto doit = [&](ptrdiff_t idx) {
        auto payload = cb(idx);
        if (!payload.empty()) {
            std::string yaml = stdprintf("memcmp_or_fail %td: ", idx) + payload;
            log_yaml(SANDSTONE_LOG_DEBUG, yaml.c_str());
        }
    };
    doit(-1);
    doit(max - 1);
    return true;
}

bool _memcmp_fail_check_cb(char *(*cb)(void *token, ptrdiff_t), void *token, size_t max)
{
    auto doit = [&](ptrdiff_t idx) {
        char *r = cb(token, idx);
        std::string yaml = stdprintf("memcmp_or_fail_cb %td: ", idx) + r;
        log_yaml(SANDSTONE_LOG_DEBUG, yaml.c_str());
        free(r);
    };
    doit(-1);
    doit(max - 1);
    return true;
}
#endif

void _memcmp_fail_report_cb(const void *actual, const void *expected, size_t size,
                            DataType type, char *(*cb)(void *token, ptrdiff_t), void *token)
{
    // Execute UD2 early if we've failed
    if (sApp->shmem->cfg.ud_on_failure)
        ud2();
    if (SandstoneConfig::NoLogging)
        report_fail_common();

    using Fn = decltype(cb);
    auto wrapper = [](const void *pcb, void *token, ptrdiff_t idx) -> std::string {
        Fn callback = reinterpret_cast<Fn>(const_cast<void *>(pcb));
        return callback(token, idx);
    };
    SandstoneMemcmpOrFail::report(actual, expected, size, type, wrapper,
                                  reinterpret_cast<const void *>(cb), token);
}

void _memcmp_fail_report(const void *_actual, const void *_expected, size_t size, DataType type, const char *fmt, ...)
{
    // Execute UD2 early if we've failed
    if (sApp->shmem->cfg.ud_on_failure)
        ud2();

    using namespace SandstoneMemcmpOrFail;
    if (SandstoneConfig::NoLogging)
        fmt = nullptr;
    if (!fmt)
        report(_actual, _expected, size, type, nullptr, nullptr);

    auto wrapper = [](const void *pfmt, void *pva, ptrdiff_t) {
        return vstdprintf(static_cast<const char *>(pfmt), *static_cast<va_list *>(pva));
    };
    va_list va;
    va_start(va, fmt);
    report(_actual, _expected, size, type, wrapper, fmt, &va);
    va_end(va);
}

void SandstoneMemcmpOrFail::report(const void *_actual, const void *_expected, size_t size,
                                   DataType type, FormatterCallback formatter, const void *t1, void *t2)
{
    // Execute UD2 early if we've failed
    if (sApp->shmem->cfg.ud_on_failure)
        ud2();

    if (!SandstoneConfig::NoLogging) {
        // mark time-to-fail before the calling the callback
        logging_mark_thread_failed(thread_num);

        auto actual = static_cast<const uint8_t *>(_actual);
        auto expected = static_cast<const uint8_t *>(_expected);
        ptrdiff_t offset = memcmp_offset(actual, expected, size);

        logging_report_mismatched_data(type, actual, expected, size, offset, formatter, t1, t2);

        // Lane-offset histogram of the mismatching bytes (SEVI ASPLOS'26
        // Obs.13: 98.5% of vector-SDC cases corrupt a single lane, and
        // multi-lane corruption is 96% adjacent-lane — a single-lane or
        // adjacent-lane pattern is the fingerprint of one bad physical
        // unit, while scattered offsets suggest a datapath/buffer fault).
        // Failure-branch only: the pass path never runs this loop.
        {
            // log_yaml(char, const char *) expects a pre-formatted YAML
            // message: first line = "text:" description, following lines =
            // indented details. The heading below is the description, the
            // histogram is the detail body; a single-line message would trip
            // the parser's second-line assert (debug builds only).
            std::string hist = "lane offsets\n";
            ptrdiff_t first_bad = -1, last_bad = -1;
            size_t count = 0;
            for (size_t i = 0; i < size; ++i) {
                if (actual[i] != expected[i]) {
                    if (first_bad < 0)
                        first_bad = ptrdiff_t(i);
                    last_bad = ptrdiff_t(i);
                    ++count;
                }
            }
            if (count > 0) {
                // per-8-byte lane summary (f64/f32 vector lanes both
                // align to 4/8-byte boundaries; report byte offsets)
                hist += stdprintf("%zu mismatching bytes in [%td..%td]",
                                  count, first_bad, last_bad);
                if (count == 1)
                    hist += " (single-lane pattern — one bad physical unit fingerprint)";
                else if (size_t(last_bad - first_bad + 1) <= count + 8)
                    hist += " (adjacent-lane pattern — one bad physical unit fingerprint)";
                else
                    hist += " (scattered offsets — datapath/buffer fault?)";
                log_yaml(SANDSTONE_LOG_WARNING, hist.c_str());
            }
        }
    }

    report_fail_common();
}

static bool shouldTestTheTest(const struct test *the_test)
{
    if (!sApp->test_tests_enabled())
        return false;
    if constexpr (InterruptMonitor::InterruptMonitorWorks)
        if (the_test == &_test_mce_check)
            return false;

    // check some flags in the_test
    return true;
}

inline void test_the_test_data<true>::prepare_test_tests(const struct test *the_test)
{
    if (!shouldTestTheTest(the_test))
        return;

    hwm_at_start = memfpt_current_high_water_mark();
    time_at_test_start = MonotonicTimePoint::clock::now();
    per_thread.resize(sApp->thread_count);
    std::fill_n(per_thread.begin(), sApp->thread_count, PerThread{});
}

inline void test_the_test_data<true>::at_run_threads_start(const struct test *the_test)
{
    if (!shouldTestTheTest(the_test))
        return;
    time_at_run_threads_start = MonotonicTimePoint::clock::now();
}

inline void test_the_test_data<true>::at_loop_start(const struct test *the_test)
{
    if (!shouldTestTheTest(the_test))
        return;
    per_thread[thread_num].time_at_loop_start = MonotonicTimePoint::clock::now();
}

inline void test_the_test_data<true>::test_tests_iteration(const struct test *the_test)
{
    if (!shouldTestTheTest(the_test))
        return;

    int n = sApp->test_thread_data(thread_num)->inner_loop_count;
    if (n >= DesiredIterations)
        return;

    auto &me = per_thread[thread_num];
    me.iteration_times[n] = std::chrono::steady_clock::now();
}

inline void test_the_test_data<true>::test_tests_finish(const struct test *the_test)
{
    // implemented as a macro because GCC can't inline variadic functions
#define maybe_log_error(flag, ...) ({                       \
        std::string msg = stdprintf(__VA_ARGS__);           \
        if (the_test->flags & flag)                         \
            log_message(-1, SANDSTONE_LOG_WARNING "%s", msg.c_str()); \
        else                                                \
            log_message(-1, SANDSTONE_LOG_ERROR "%s", msg.c_str());   \
    })

    using namespace std::chrono;
    if (!shouldTestTheTest(the_test))
        return;

    // check if the test has failed
    bool has_failed = false;
    auto failchecker = [&has_failed](PerThreadData::Common *data, int) {
        if (data->has_failed())
            has_failed = true;
    };
    for_each_main_thread(failchecker);
    if (!has_failed)
        for_each_test_thread(failchecker);

    MonotonicTimePoint now = std::chrono::steady_clock::now();

    size_t current_hwm = memfpt_current_high_water_mark();
    if (current_hwm < hwm_at_start) {
        // Also fires when current_hwm == 0 and hwm_at_start != 0 (tracking stopped).
        // hwm_at_start == 0 is a valid zero baseline (freshly forked child had no
        // private pages yet), so current_hwm >= 0 == hwm_at_start is not an error.
        log_warning("High water mark memory footprinting failed (%zu kB at start, %zu kB now)",
                    hwm_at_start, current_hwm);
    } else if (current_hwm) {
        size_t per_thread_avg = (current_hwm - hwm_at_start) / sApp->thread_count;
        if (per_thread_avg < MaxAcceptableMemoryUseKB)
            log_info("Test memory use: (%zu - %zu) / %d = %zu kB",
                     current_hwm, hwm_at_start, sApp->thread_count, per_thread_avg);
        else
            maybe_log_error(test_flag_ignore_memory_use,
                            "Test uses too much memory: (%zu - %zu) / %d = %zu kB",
                            current_hwm, hwm_at_start, sApp->thread_count, per_thread_avg);
    }

    if (has_failed)
        return;                     // no point in reporting timing of failed tests

    if (sApp->shmem->current_test_endtime == MonotonicTimePoint::max() || the_test->desired_duration < 0) {
        return;
    }

    // check the overall time
    Duration expected_runtime = sApp->shmem->current_test_endtime - time_at_test_start;
    Duration min_expected_runtime = expected_runtime - expected_runtime / 4;
    Duration max_expected_runtime = expected_runtime + expected_runtime / 4;
    Duration actual_runtime = now - time_at_test_start;
    Duration difference = abs(expected_runtime - actual_runtime);

    if ((actual_runtime > min_expected_runtime && actual_runtime < max_expected_runtime)
            || difference < OverallTestTimeIgnore) {
        // Acceptable timing
        log_info("Overall time: %s", format_duration(actual_runtime).c_str());
    } else if (actual_runtime > max_expected_runtime) {
        maybe_log_error(test_flag_ignore_test_overtime,
                        "Test ran for longer than expected: %s (desired %s, max %s)",
                        format_duration(actual_runtime).c_str(), format_duration(expected_runtime).c_str(),
                        format_duration(max_expected_runtime).c_str());
    } else  {
        maybe_log_error(test_flag_ignore_test_undertime,
                        "Test ran shorter than expected: %s (desired %s, min %s)",
                        format_duration(actual_runtime).c_str(), format_duration(expected_runtime).c_str(),
                        format_duration(min_expected_runtime).c_str());
    }

    // check the timings in each thread
    Duration average = {};
    int average_counts = 0;
    int while_loops = 0;
    log_info("Sampled init timing: %s", format_duration(time_at_run_threads_start - time_at_test_start).c_str());
    for (int t = 0; t < sApp->thread_count; ++t) {
        PerThread &thr = per_thread[t];
        if (thr.iteration_times[0].time_since_epoch() == 0s)
            continue;

        std::array<Duration, DesiredIterations> iteration_times = {};
        if (thr.time_at_loop_start.time_since_epoch() > 0s) {
            iteration_times[0] = thr.iteration_times[0] - thr.time_at_loop_start;
            log_message(t, SANDSTONE_LOG_DEBUG "Sampled full 1st iteration timing: %s",
                format_duration(thr.iteration_times[0] - time_at_run_threads_start).c_str());
        } else { // no TEST_LOOP in test - no timing of additional run() overhead
            iteration_times[0] = thr.iteration_times[0] - time_at_run_threads_start;
        }
        int n = 0;
        for (int i = 1; i < DesiredIterations; ++i) {
            if (thr.iteration_times[i].time_since_epoch() == 0s)
                break;
            ++n;
            iteration_times[n] = thr.iteration_times[i] - thr.iteration_times[i - 1];
        }

        if (n) {
            Duration this_average = (thr.iteration_times[n] - thr.iteration_times[0]) / n;
            average += this_average;
            if (iteration_times[0] < 1ms && iteration_times[0] * 4 < this_average)
                ++while_loops;
        } else {
            average += iteration_times[0];
        }
        ++average_counts;

        log_message(t, SANDSTONE_LOG_DEBUG "Sampled iteration timings: %s, %s, %s, %s",
                    format_duration(iteration_times[0]).c_str(),
                    format_duration(iteration_times[1]).c_str(),
                    format_duration(iteration_times[2]).c_str(),
                    format_duration(iteration_times[3]).c_str());
    }

    if (average_counts == 0) {
        log_error("run() function did not call test_time_condition() in any thread");
    } else {
        if (while_loops)
            maybe_log_error(test_flag_ignore_do_while,
                            "run() function appears to use while (test_time_condition()) instead of do {} while");

        // find the threads where test_time_condition() wasn't called
        for (int t = 0; average_counts != sApp->thread_count && t < sApp->thread_count; ++t) {
            PerThread &thr = per_thread[t];
            if (thr.iteration_times[0].time_since_epoch() == 0s)
                log_message(t, SANDSTONE_LOG_WARNING "run() function did not call test_time_condition() in this thread");
        }

        average /= average_counts;
        if (average < 1us) {
            // avoid division by zero
            maybe_log_error(test_flag_ignore_loop_timing,
                            "Inner loop is FAR too short, couldn't even get accurate timings");
        } else if (average < MinimumLoopDuration) {
            maybe_log_error(test_flag_ignore_loop_timing,
                            "Inner loop is too short (average %s) -- suggest making the test %" PRId64 "x longer",
                            format_duration(average).c_str(), TargetLoopDuration.count() / average.count());
        } else if (average > MaximumLoopDuration) {
            maybe_log_error(test_flag_ignore_loop_timing,
                            "Inner loop is too long (average %s) -- suggest making the test %" PRId64 "x shorter",
                            format_duration(average).c_str(), average.count() / TargetLoopDuration.count());
        } else {
            log_info("Inner loop average duration: %s", format_duration(average).c_str());
        }
    }

#undef maybe_log_error
}

static MonotonicTimePoint calculate_wallclock_deadline(Duration duration, MonotonicTimePoint *pnow = nullptr)
{
    MonotonicTimePoint now = MonotonicTimePoint::clock::now();
    if (pnow)
        *pnow = now;

    return now + duration;
}

bool wallclock_deadline_has_expired(MonotonicTimePoint deadline)
{
    MonotonicTimePoint now = MonotonicTimePoint::clock::now();

    if (now > deadline)
        return true;
    if (sApp->shmem->cfg.use_strict_runtime && now > sApp->endtime)
        return true;
    return false;
}

static bool max_loop_count_exceeded(const struct test *the_test)
{
    PerThreadData::Test *data = sApp->test_thread_data(thread_num);

    // unsigned comparisons so sApp->current_max_loop_count == -1 causes an always false
    if (unsigned(data->inner_loop_count) >= unsigned(sApp->shmem->current_max_loop_count))
        return true;

    /* Desired duration -1 means "runs once" */
    if (the_test->desired_duration == -1)
        return true;
    return false;
}

extern "C" void test_loop_iterate() noexcept;    // see below

/* returns 1 if the test should keep running, useful for a while () loop */
#undef test_time_condition
bool test_time_condition() noexcept
{
    test_loop_iterate();
    sApp->test_tests_iteration(current_test());
    sApp->test_thread_data(thread_num)->inner_loop_count++;

    if (max_loop_count_exceeded(current_test()))
        return 0;  // end the test if max loop count exceeded

    return !wallclock_deadline_has_expired(sApp->shmem->current_test_endtime);
}

bool test_loop_condition(int N) noexcept
{
    if (sApp->shmem->current_test_sleep_duration != 0us)
    {
        usleep(sApp->shmem->current_test_sleep_duration.count() / N);
    }
    return test_time_condition();
}

bool test_is_retry() noexcept
{
    // negative values indicate a retry
    return sApp->current_iteration_count < 0;
}

static void init_internal(const struct test *test)
{
    print_temperature_of_device();

    logging_init(test);
    ulog_update(test);
}

static void init_per_thread_data()
{
    auto initer = [](auto *data, int) { data->init(); };
    for_each_main_thread(initer);
    for_each_test_thread(initer);
}

static void cleanup_internal(const struct test *test)
{
    logging_finish();
}

int cleanup_global(int exit_code, PerThreadFailures per_thread_failures)
{
#if SANDSTONE_FREQUENCY_MANAGER
    if (sApp->vary_frequency_mode)
        sApp->frequency_manager->restore_core_frequency_initial_state();

    if (sApp->vary_uncore_frequency_mode)
        sApp->frequency_manager->restore_uncore_frequency_initial_state();
#endif

    exit_code = print_application_footer(exit_code, std::move(per_thread_failures));
    return logging_close_global(exit_code);
}

template <uint32_t X, uint32_t Y, typename P = int>
static void inline __attribute__((always_inline)) assembly_marker(P param = 0)
{
#ifdef __x86_64__
    // GCC doesn't provide __SSC_MARK in <x86gprintrin.h>
    asm ("movl %0, %%ebx\n\t"
         "fs addr32 nop"
        : : "i" (X ^ Y), "D" (param) : "ebx");
#else
    /* On non-x86 (e.g. AArch64) there is no Intel SDE to hook (it is an
     * x86 instruction-set emulator), so there is no marker instruction to
     * emit. The noinline test_start()/test_end() functions themselves still
     * serve as symbol anchors that Valgrind's --toggle-collect can latch
     * onto by name, which is the only external tool that runs on AArch64
     * here. (void) param; keeps the parameter used. */
    (void) param;
#endif
}

namespace AssemblyMarker {
static constexpr uint32_t Test = 0x00;
static constexpr uint32_t TestLoop = 0x100;
static constexpr uint32_t Start = 0x53;             // "S"
static constexpr uint32_t Iterate = 0x49;           // "I"
static constexpr uint32_t End = 0x45;               // "E";
}

extern "C" {
// The test_start() and test_stop() functions are no-op, but exist to
// facilitate catching test starts and stops from external tools like the Intel
// SDE (-start_address test_start -stop_address test_end) and Valgrind
// (--toggle-collect=thread_runner).

static void __attribute__((noinline, noclone)) test_start()
{
    using namespace AssemblyMarker;
    assembly_marker<Test, Start>();
}

#if defined(__x86_64__)
// sysv_abi 是 x86-64 调用约定属性,arm64(AAPCS64)上被 GCC 忽略并告警,故仅 x86_64 应用。
static void __attribute__((noinline, noclone, sysv_abi)) test_end(ThreadState state)
#else
static void __attribute__((noinline, noclone)) test_end(ThreadState state)
#endif
{
    using namespace AssemblyMarker;
    assembly_marker<Test, End>(state);
}

// This wrapper function is needed by emulation to be able to identify
// what the starting LIP to start execution on when they land on the emulator.
static __attribute__((noinline))
int test_run_wrapper_function(const struct test *test, int thread_number)
{
    return test->test_run(const_cast<struct test *>(test), thread_number);
}

#ifndef _WIN32
#  pragma GCC visibility push(hidden)
#endif

// Ditto for TEST_LOOP:
//     -start_address test_loop_start -stop_address test_loop_end
// OR: -start_address test_loop_start -stop_address test_loop_iterate
//
// Notes:
// - if the test fails, the functions following a report_fail() or
//   memcpy_or_fail() will not run
// - The test will run N loops of the content (see TEST_LOOP docs)
//   between calls of these functions

void test_loop_start() noexcept
{
    sApp->at_loop_start(current_test());
    using namespace AssemblyMarker;
    assembly_marker<TestLoop, Start>();
}

void test_loop_iterate() noexcept
{
    using namespace AssemblyMarker;
    assembly_marker<TestLoop, Iterate>();
}

void test_loop_end() noexcept
{
    using namespace AssemblyMarker;
    assembly_marker<TestLoop, End>(sApp->test_thread_data(thread_num)->inner_loop_count);
}

#ifndef _WIN32
#  pragma GCC visibility pop
#endif
} // extern "C"

DeviceScheduler *device_scheduler = nullptr;
void reschedule_internal(DeviceScheduler *scheduler)
{
    assert(thread_num >= 0 && "reschedule() can only be called from test threads!");
    sApp->test_thread_data(thread_num)->previous_cpu = LogicalProcessor(sched_getcpu());
    scheduler->reschedule_to_next_device();
}

static uintptr_t thread_runner(int thread_number)
{
    // convert from internal Sandstone numbering to the system one
    pin_to_logical_processor(LogicalProcessor(device_info[thread_number].cpu_number), current_test()->id);

    PerThreadData::Test *this_thread = sApp->test_thread_data(thread_number);
    random_init_thread(thread_number);
    int ret = EXIT_FAILURE;

    auto cleanup = [&] {
        // let SIGQUIT handler know we're done
        ThreadState new_state = thread_failed;
        if (!this_thread->has_failed()) {
            if (this_thread->has_skipped() || ret < EXIT_SUCCESS)
                new_state = thread_skipped;
            else if (ret == EXIT_SUCCESS)
                new_state = thread_succeeded;
        }
        this_thread->thread_state.store(new_state, std::memory_order_relaxed);

        if (new_state == thread_failed) {
            if (sApp->shmem->cfg.ud_on_failure)
                ud2();
            logging_mark_thread_failed(thread_number);
            logging_run_callback();
        } else if (new_state == thread_skipped) {
            logging_mark_thread_skipped(thread_number);
        }
        test_end(new_state);

        // If rescheduling, do cleanup
        if (device_scheduler)
            device_scheduler->finish_reschedule();
    };

#if SANDSTONE_DEVICE_CPU
    CPUTimeFreqStamp before;
    before.Snapshot(thread_number);
#endif

    {
        auto e = scopeExit(std::move(cleanup));
        // indicate to SIGQUIT handler that we're running
        this_thread->thread_state.store(thread_running, std::memory_order_relaxed);

        test_start();
        /* Design §4.4: pin THIS worker to the chip's highest supported tier
           before the test body runs — the only thread whose VL matters (see
           arm64_spec_prepare.h and arm64_vl.h's thread-model boundary).
           Returns EXIT_SUCCESS unconditionally on non-arm64, so x86-64 is
           untouched. */
        ret = arm64_spec_prepare_test(current_test());
        if (ret == EXIT_SUCCESS)
            ret = test_run_wrapper_function(current_test(), thread_number);

        // cleanup resets thread_state
    }

#if SANDSTONE_DEVICE_CPU
    CPUTimeFreqStamp after;
    after.Snapshot(thread_number);
    this_thread->effective_freq_mhz = CPUTimeFreqStamp::EffectiveFrequencyMHz(before, after);
#endif

    // our caller doesn't care what we return, but the returned value helps if
    // you're running strace
    return ret;
}

static void protect_shmem()
{
    size_t protected_len = sApp->shmem->thread_data_offset;
    assert(protected_len == ROUND_UP_TO_PAGE(protected_len) &&
            "SharedMemory::main_thread_data is not page-aligned");
    IGNORE_RETVAL(mprotect(sApp->shmem, protected_len, PROT_READ));
}

__attribute__((weak, noclone, noinline)) int print_application_footer(int exit_code, PerThreadFailures per_thread_failures)
{
    return exit_code;
}

static void run_threads_in_parallel(const struct test *test)
{
    SandstoneTestThread thr[sApp->thread_count];    // NOLINT: -Wvla
    int i;

    for (i = 0; i < sApp->thread_count; i++) {
        thr[i].start(thread_runner, i);
    }
    /* wait for threads to end */
    for (i = 0; i < sApp->thread_count; i++) {
        thr[i].join();
    }
}

static void run_threads_sequentially(const struct test *test)
{
    // we still start one thread, in case the test uses report_fail_msg()
    // (which uses pthread_cancel())
    SandstoneTestThread thread;
    thread.start([](int t) {
        for ( ; t != sApp->thread_count; thread_num = ++t)
            thread_runner(t);
        return uintptr_t(t);
    }, 0);
    thread.join();
}

static void run_threads(const struct test *test)
{
    switch (test->flags & test_schedule_mask) {
    default:
        run_threads_in_parallel(test);
        break;

    case test_schedule_sequential:
        run_threads_sequentially(test);
        break;
    }
}

namespace {
struct StartedChild
{
    intptr_t pid;
    int fd;
};

struct ChildrenList
{
    ChildrenList() = default;
    ChildrenList(const ChildrenList &) = delete;
    ChildrenList &operator=(const ChildrenList &) = delete;
#ifdef _WIN32
    ~ChildrenList()
    {
        for (auto &p : handles) {
            HANDLE h = reinterpret_cast<HANDLE>(p);
            if (h != INVALID_HANDLE_VALUE)
                CloseHandle(h);
        }
    }
#else
    ~ChildrenList()
    {
        for (auto &p : pollfds) {
            if (p.fd != -1)
                close(p.fd);
        }
    }

    std::vector<pollfd> pollfds;
#endif
    std::vector<pid_t> handles;
    std::vector<ChildExitStatus> results;

    void add(StartedChild child)
    {
        handles.push_back(child.pid);
#ifndef _WIN32
        pollfds.emplace_back(pollfd{ .fd = child.fd, .events = POLLIN });
#endif
    }
};
} // unnamed namespace

static void wait_for_children(ChildrenList &children, const struct test *test)
{
    Duration remaining = test_timeout(sApp->current_test_duration);
    int children_left = children.handles.size();
    children.results.resize(children_left);

#if !defined(_WIN32)
    // add even if -1
    children.pollfds.emplace_back(pollfd{ .fd = sApp->shmem->server_debug_socket, .events = POLLIN });
    auto remove_debug_socket = scopeExit([&] { children.pollfds.pop_back(); });

    static constexpr int TimeoutSignal = SIGQUIT;
    auto kill_children = [&](int sig = SIGKILL) {
        // Send the signal to the child's process group, so all its children
        // get the signal too.
        for (pid_t child : children.handles) {
            if (child)
                kill(-child, sig);
        }
    };
    auto single_wait = [&, caughtSignal = 0](milliseconds timeout) mutable {
        int ret = poll(children.pollfds.data(), children.pollfds.size(), timeout.count());
        if (ret == 0)
            return 0;           // timed out

        if (__builtin_expect(ret < 0 && errno != EINTR, false)) {
            perror("poll");
            exit(EX_OSERR);
        }
        if (ret < 0) {
            // we've received a signal, which one?
            auto [signal, count] = last_signal();
            if (signal != 0) {
                // forward the signal to all children
                kill_children(signal);
            }

            // if it was SIGINT, we print a message and wait for the test
            if (count == 1 && signal == SIGINT) {
                logging_printf(LOG_LEVEL_QUIET, "# Caught SIGINT, stopping current test "
                                                "(press Ctrl+C again to exit without waiting)\n");
                logging_print_log_file_name();
                enable_interrupt_catch();       // re-arm SIGINT handler
                caughtSignal = signal;
                return 0;
            } else {
                // for any other signal (e.g., SIGTERM), we don't
                return int(signal);
            }
        }

        if (pollfd &pfd = children.pollfds.back(); pfd.revents & POLLIN) {
            // one child (or more than one) is crashing
            debug_crashed_child(children.handles);
        }

        // check if any of the children have exited
        for (int i = 0; i < children.pollfds.size() - 1; ++i) {
            pollfd &pfd = children.pollfds[i];
            if (pfd.revents == 0)
                continue;

            // wait for this child
            struct forkfd_info info;
            struct rusage usage;
            EINTR_LOOP(ret, forkfd_wait4(pfd.fd, &info, WNOHANG, &usage));
            if (ret == -1) {
                if (errno == EAGAIN)
                    continue;           // shouldn't happen...
                perror("forkfd_wait");
                exit(EX_OSERR);
            }
            forkfd_close(pfd.fd);
            pfd.fd = -1;
            pfd.events = 0;
            children.handles[i] = 0;
            merge_test_result(children.results[i], test_result_from_exit_code(info));
            children.results[i].usage = usage;
            --children_left;
        }
        return caughtSignal;
    };
#elif defined(_WIN32)
    static constexpr DWORD TimeoutSignal = EXIT_TIMEOUT;
    auto kill_children = [&](DWORD exitCode = DWORD(-1)) {
        // Note: Windows code cannot kill grand-children processes!
        for (intptr_t child : children.handles) {
            HANDLE hnd = HANDLE(child);
            if (hnd == INVALID_HANDLE_VALUE)
                continue;
            TerminateProcess(hnd, exitCode);
        }
    };
    auto single_wait = [&](milliseconds timeout) {
        HANDLE handles[MAXIMUM_WAIT_OBJECTS];
        DWORD nCount = 0;
        if (sApp->shmem->debug_event)
            handles[nCount++] = HANDLE(sApp->shmem->debug_event);
        for (auto it = children.handles.begin(); it != children.handles.end() && nCount < MAXIMUM_WAIT_OBJECTS; ++it) {
            HANDLE hnd = HANDLE(*it);
            if (hnd == INVALID_HANDLE_VALUE)
                continue;
            handles[nCount] = hnd;
            ++nCount;
        }
        bool bWaitAll = false;
        DWORD result = WaitForMultipleObjects(nCount, handles, bWaitAll, timeout.count());
        if (result == WAIT_TIMEOUT)
            return 0;

        DWORD idx = result - WAIT_OBJECT_0;
        if (idx >= nCount) [[unlikely]] {
            DWORD err = GetLastError();
            fprintf(stderr, "%s: WaitForMultipleObjects() failed: %lx; children left = %d: %s\n",
                    program_invocation_name, result, children_left, win32_strerror(err).c_str());
            abort();
        }
        if (idx == 0 && sApp->shmem->debug_event) {
            // one child (or more than one) is crashing
            debug_crashed_child(children.handles);
            return 0;
        }

        HANDLE hExited = handles[idx];
        if (GetExitCodeProcess(hExited, &result) == 0) {
            fprintf(stderr, "%s: GetExitCodeProcess(child = %p) failed: %lx\n",
                    program_invocation_name, handles[idx], GetLastError());
            abort();
        }

        auto childResult = test_result_from_exit_code(result);
        if (FILETIME dummy, stime, utime; GetProcessTimes(hExited, &dummy, &dummy, &stime, &utime)) {
            auto cvt = [](FILETIME f) {
                // FILETIME stores tenths of microseconds (100 ns) granularity
                uint64_t time = f.dwHighDateTime;
                time = (time << 32) | f.dwLowDateTime;
                struct timeval tv;
                tv.tv_sec = time / 1000 / 1000 / 10;
                tv.tv_usec = time % (1000 * 1000) / 10;
                return tv;
            };
            childResult.usage.ru_stime = cvt(stime);
            childResult.usage.ru_utime = cvt(utime);
        }
        if (PROCESS_MEMORY_COUNTERS mi; GetProcessMemoryInfo(hExited, &mi, sizeof(mi))) {
            childResult.usage.ru_maxrss = mi.PeakWorkingSetSize;
            childResult.usage.ru_majflt = mi.PageFaultCount;
        }

        // close the handle and store result
        for (idx = 0; idx < int(children.handles.size()); ++idx) {
            if (hExited == HANDLE(children.handles[idx])) {
                CloseHandle(hExited);
                children.handles[idx] = intptr_t(INVALID_HANDLE_VALUE);
                merge_test_result(children.results[idx], childResult);
                --children_left;
                return 0;
            }
        }

        fprintf(stderr, "%s: INTERNAL ERROR: somehow got unknown handle 0x%p\n",
                program_invocation_name, hExited);
        abort();
    };
#else
#  error "What platform is this?"
#endif
    auto terminate_children = [&] {
        for (size_t i = 0; i < children.handles.size(); ++i) {
            auto child = children.handles[i];
            if (children.results[i].endtime == MonotonicTimePoint{}) {
                debug_hung_child(child, children.handles);
#ifdef _WIN32
                log_message(-int(i) - 1, SANDSTONE_LOG_ERROR "Child %ld did not exit, using TerminateProcess()",
                            GetProcessId(HANDLE(child)));
#else
                log_message(-int(i) - 1, SANDSTONE_LOG_ERROR "Child %d did not exit, sending signal SIGQUIT", child);
#endif
                children.results[i].result = TestResult::TimedOut;
            }
        }
        kill_children(TimeoutSignal);
    };
    auto wait_for_all_children = [&](Duration remaining) {
        MonotonicTimePoint deadline = steady_clock::now() + remaining;
        for ( ; children_left && remaining > 0s; remaining = deadline - steady_clock::now()) {
            int ret = single_wait(ceil<milliseconds>(remaining));
            if (ret == 0)
                continue;

            MonotonicTimePoint now = MonotonicTimePoint::clock::now();
            for (ChildExitStatus &result : children.results) {
                result.result = TestResult::Interrupted;
                if (result.endtime == MonotonicTimePoint())
                    result.endtime = now;
            }

            // Problem waiting: we must have caught a signal
            // (child has likely not been able to write results)
            int exit_code = 128 | ret;

            logging_print_results(children.results, test);
            exit_code = cleanup_global(exit_code, {});

            // now exit with the same signal
            disable_interrupt_catch();
            raise(exit_code & 0x7f);
            _exit(exit_code);           // just in case
        }
    };

    /* first wait set : normal exit */
    wait_for_all_children(remaining);
    if (children_left == 0)
        return;

    /* at least one child timed out; force them to exit */
    terminate_children();

    /* wait for the termination to take effect */
    wait_for_all_children(sApp->timeout_to_kill);
    if (children_left) {
        /* timed out again, take drastic measures */
        kill_children();
        wait_for_all_children(20s);
    }
    if (children_left) {
        for (size_t i = 0; i < children.handles.size(); ++i) {
            if (children.handles[i] == 0)
                continue;
            log_platform_message(SANDSTONE_LOG_ERROR "# Child %td is hung and won't exit",
                                 intptr_t(children.handles[i]));
            children.results[i] = { TestResult::TimedOut };
        }
    }
}

extern bool logging_in_test;

static TestResult prepare_test_common(struct test *test)
{
    logging_in_test = true;
    return prepare_test_for_device(test);
}

static void finish_test_common(struct test *test)
{
    finish_test_for_device(test);
    logging_in_test = false;
}

#if defined(__aarch64__) && defined(__linux__)
/* ---- VmHWM 反馈闭环 (T7, spec §4.4) ---------------------------------- */

/* test->id 的 FNV-1a 64 位哈希。写入点与读侧聚合共用同一实现; 0 保留为
 * "空槽"哨兵(fresh memfd 零页)。 */
static uint64_t sdc_test_id_hash(const char *s)
{
    if (!s)
        return 0;
    uint64_t h = 1469598103934665603ull;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 1099511628211ull;
    }
    return h;
}

/* 读侧聚合(sdc_stress_config.h 声明, 仅 arm64 联合求解分支调用): 遍历
 * 全部 slice 的 Main::mem_feedback 槽(MAP_SHARED shmem; 子进程经
 * raw_main_thread_data 从 shmem 头重算数组基址读兄弟槽), 同 test 哈希的
 * 槽取 eff_ratio 的 median —— q4: max 会取到最噪样本, median 防抖。
 * 返回 <0 = 无有效反馈(首 fracture / no_fork / 未迁移用例)。 */
double sdc_feedback_eff_median(const char *test_id)
{
    if (!test_id || !sApp->shmem || sApp->shmem->main_thread_count <= 0)
        return -1.0;
    uint64_t hash = sdc_test_id_hash(test_id);
    if (hash == 0)
        return -1.0;
    std::vector<double> vals;
    for (int i = 0; i < sApp->shmem->main_thread_count; ++i) {
        const PerThreadData::MemFeedback *fb =
                &sApp->raw_main_thread_data(i)->mem_feedback;
        /* seqlock 读侧(PR#302 检视 #13/#26): 前后 acquire 各读一次 seq,
         * 序号一致且为偶才采纳快照; 奇数(兄弟 child 正在写)/序号变动
         * (读取期间发生过写)的槽丢弃——共享字段的重排混合态不再可能
         * 进入 median。空槽 seq=0 恒为偶, 走既有 samples==0 无效判据。 */
        uint32_t s1 = __atomic_load_n(&fb->seq, __ATOMIC_ACQUIRE);
        if (s1 & 1)
            continue;
        uint64_t h = fb->test_id_hash;
        uint32_t samples = fb->samples;
        double eff = fb->eff_ratio;
        uint32_t s2 = __atomic_load_n(&fb->seq, __ATOMIC_ACQUIRE);
        if (s1 != s2)
            continue;
        if (h == hash && samples > 0 && eff > 0.0)
            vals.push_back(eff);
    }
    if (vals.empty())
        return -1.0;
    std::sort(vals.begin(), vals.end());
    size_t m = vals.size();
    return (m & 1) ? vals[m / 2] : 0.5 * (vals[m / 2 - 1] + vals[m / 2]);
}

/* 写入点(run_one_test_inner 尾部调用): eff_ratio = VmHWM/(n²·elem·本
 * slice线程), EMA α=0.5 写进本 slice 的 Main 槽。n/elem/threads 取自本
 * child test_init 里联合求解登记的 sdc_joint_cache —— 显式 knob/未迁移
 * 用例无口径则不写。写侧始终记录(±15% 死区与 0.7×静态锚下限都在读侧
 * 判断, 见 sdc_joint_dim_slice)。基线污染口径: fork child 的 VmHWM 起点
 * = 父进程当时 RSS(实测 ~8.3MB, eigen 家族 <1%); 联合求解探针的高水位
 * 同样计入(探针远小于正式工作集)—— 记录为已知口径。 */
static void sdc_feedback_write(const struct test *test)
{
    if (!test || !test->id)
        return;
    auto it = sdc_joint_cache().find(test->id);
    if (it == sdc_joint_cache().end())
        return;
    const struct sdc_joint_ctx &ctx = it->second;
    if (ctx.n <= 0 || ctx.threads <= 0 || ctx.elem_bytes <= 0)
        return;
    uint64_t hwm = sdc_read_vm_hwm();
    if (hwm == 0)
        return;
    double denom = (double)ctx.n * (double)ctx.n
                 * (double)ctx.elem_bytes * (double)ctx.threads;
    if (!(denom > 0.0))
        return;
    double eff = (double)hwm / denom;

    PerThreadData::MemFeedback *fb = &sApp->main_thread_data()->mem_feedback;
    uint64_t hash = sdc_test_id_hash(test->id);
    /* seqlock 发布(PR#302 检视 #13/#26, 收口 T9 注释自认的已知边界):
     * 本槽是 MAP_SHARED 上的单写者(本 slice 的当前 child)/多读者(兄弟
     * slice child 的 test_init 读侧)共享字段, 普通非原子 store/RMW 的
     * 编译器/架构重排可使读者见到混合态(新 hash 配旧 eff_ratio——异常
     * 偏大的旧 eff 经读侧无上钳的向上修正直接放大 n_mem)。写序:
     *   [seq→奇(relaxed)] → 复位+EMA 字段更新 → [seq+1→偶(release)]
     * release 保证字段 store 不下沉到偶数发布之后; 首次置奇用 relaxed
     * 即可——字段 store 被提前到置奇之前时, 读者见到的仍是完整
     * (新或旧)快照, 语义无损。T9 的『先清 samples 再发布 hash』源序
     * 保留(双保险: 复位期间读者见奇数丢弃或见稳定偶数且 samples==0
     * 判无效)。
     * 被杀写者恢复(置奇用 s1 = seq0|1 而非 seq0+1 的原因): 上代 child
     * 可在两次 seq store 之间被杀(超时 SIGQUIT/OOM), seq 停在奇数。
     * 若用 seq0+1: 奇数+1=偶——半写字段被当"稳定态"发布一次, 且
     * seq0+2 又落回奇数, 此后每次写入的 锁定=偶/发布=奇 相位永久
     * 倒置, 保护彻底失效。用 |1: 奇数保持奇(无虚假稳定窗口),
     * 完整重写字段后发布 s1+1=偶, 不变式单次写入即恢复; 正常路径
     * (seq0 偶)与 +1 完全同值。序号单调不减, 2^32 回绕需 40 亿次
     * 写入(每 test 每 fracture 一次)——不可达。
     * 上一测试的 eff_ratio 被采进本测试 median 的窗口由 seq 一致性
     * 判据关闭: 复位期间读者要么见奇数丢弃, 要么见稳定偶数且
     * samples==0 判无效。 */
    uint32_t seq0 = __atomic_load_n(&fb->seq, __ATOMIC_RELAXED);
    uint32_t s1 = seq0 | 1;                 /* 奇数=写入中(被杀写者恢复) */
    __atomic_store_n(&fb->seq, s1, __ATOMIC_RELAXED);
    if (fb->test_id_hash != hash) {         /* 槽被上一测试占用 → 重置 */
        fb->samples = 0;
        fb->test_id_hash = hash;
        fb->eff_ratio = 0.0;
    }
    if (fb->samples == 0)
        fb->eff_ratio = eff;                /* 首样本直写 */
    else
        fb->eff_ratio = 0.5 * fb->eff_ratio + 0.5 * eff;   /* EMA α=0.5 */
    ++fb->samples;
    __atomic_store_n(&fb->seq, s1 + 1, __ATOMIC_RELEASE);   /* 偶数=稳定 */
    log_warning("[反馈] %s: VmHWM=%.1f MiB / (n=%d²×%dB×%d线程) → "
                "eff_ratio=%.3f (EMA α=0.5, 样本=%u)",
                test->id, (double)hwm / 1048576.0, ctx.n, ctx.elem_bytes,
                ctx.threads, fb->eff_ratio, (unsigned)fb->samples);
}
#endif /* __aarch64__ && __linux__ */

static TestResult run_one_test_inner(struct test *test, bool init_in_aux_thread = false)
{
    s_current_test = test;
    auto resetCurrentTest = scopeExit([&] { s_current_test = nullptr; });

    TestResult state = [&] {
        int ret = 0;
        test->per_thread = sApp->user_thread_data.data();
        std::fill_n(test->per_thread, sApp->thread_count, test_data_per_thread{});
        init_per_thread_data();

        TestResult state = prepare_test_common(test);
        auto finish_test = scopeExit([&] { finish_test_common(test); }); // we can return early
        if (state != TestResult::Passed) {
            return state;
        }
        sApp->prepare_test_tests(test);

        /* ARM64 spec policy (design §4.4): raise the CONTROL thread to the
           chip's highest tier before test_init's aux thread and the per-CPU
           workers are created — both inherit this thread's VL, so init's
           goldens/buffers and the run-side lanes agree even when the kernel
           default VL is below the silicon maximum. Workers re-verify and
           carry the SKIP/FAIL verdict (arm64_spec_prepare.h). */
        arm64_spec_pin_control_thread(test);

        if (test->test_init) {
            if (init_in_aux_thread) {
                // ensure the init function is run pinned to a specific logical
                // processor but its pinning doesn't affect this control thread
                auto init_thread_runner = [](void *testptr) {
                    auto test = static_cast</*nonconst*/ struct test *>(testptr);
                    pin_to_logical_processor(LogicalProcessor(device_info[0].cpu_number));
                    thread_num = -1;
                    intptr_t ret = test->test_init(test);
                    return reinterpret_cast<void *>(ret);
                };
                pthread_t init_thread;
                void *retptr;
                pthread_create(&init_thread, nullptr, init_thread_runner, test);
                pthread_join(init_thread, &retptr);
                ret = intptr_t(retptr);
            } else {
                ret = test->test_init(test);
            }
            if (ret > 0) {
                state = TestResult::Failed;
            } else if (ret < 0) {
                state = TestResult::Skipped;
            } else {
                // check if the thread has failed with log_error() or skipped
                // with log_skip()
                PerThreadData::Main *thr = sApp->main_thread_data();
                if (thr->has_skipped())
                    state = TestResult::Skipped;
                else if (thr->has_failed())
                    state = TestResult::Failed;
            }
        }

        if (state == TestResult::Failed) {
            logging_mark_thread_failed(-1);
            if (ret > 0)
                log_error("Init function failed with code %i", ret);
            return TestResult::Failed;
        } else if (state == TestResult::Skipped) {
            if (ret != 0 && ret != EXIT_SKIP)
                log_skip(RuntimeSkipCategory, "Unexpected OS error: %s", strerror(-ret));
            return TestResult::Skipped;
        }

        sApp->at_run_threads_start(test);
        run_threads(test);

        if (test->test_cleanup) {
            ret = test->test_cleanup(test);
            PerThreadData::Main *thr = sApp->main_thread_data();
            if (ret > EXIT_SUCCESS || thr->has_failed()) {
                state = TestResult::Failed;
            } else if (ret == EXIT_SKIP || thr->has_skipped()) {
                log_skip(RuntimeSkipCategory, "SKIP requested in cleanup");
                state = TestResult::Skipped;
            } else if (ret < EXIT_SUCCESS) {
                log_skip(RuntimeSkipCategory, "Unexpected OS error in cleanup: %s", strerror(-ret));
                state = TestResult::Skipped;
            }
        }

        sApp->test_tests_finish(test);
#if defined(__aarch64__) && defined(__linux__)
        /* T7 (spec §4.4): VmHWM 反馈写入 —— test_cleanup 之后、返回之前。
         * 门控(M6): no_fork 下测试在主进程跑, VmHWM 跨测试累计只会虚高,
         * 不写; test_in_parent(父进程内跑)由 is_main_process() 同理排除。
         * 选择偏差(m1, 如实记录): 超时/被杀 child 到不了这里 —— 反馈只
         * 收成功样本, c 偏乐观方向, 读侧 15% 死区 + 0.7×静态锚下限兜底。 */
        if (sApp->current_fork_mode() != SandstoneApplication::ForkMode::no_fork
                && !sApp->is_main_process())
            sdc_feedback_write(test);
#endif
        return state;
    }();

    logging_cleanup_failure_callback();
    return state;
}


TestResult child_run(/*nonconst*/ struct test *test, int child_number)
{
    if (sApp->current_fork_mode() != SandstoneApplication::ForkMode::no_fork) {
        protect_shmem();
        sApp->select_main_thread(child_number);
        pin_to_logical_processors(sApp->main_thread_data()->device_range, "control");
        restrict_topology(sApp->main_thread_data()->device_range);
        sApp->thread_count = sApp->main_thread_data()->thread_range.thread_count;
        signals_init_child();
        debug_init_child();
    }

    return run_one_test_inner(test, true);
}

static StartedChild call_forkfd()
{
    pid_t pid;

#ifdef __linux__
    /*
     * glibc up until release 2.25 used to cache the process's PID in each thread's
     * specific data and that's what was returned from getpid(). The fork()
     * function did overwrite it so that the child side would get the correct PID,
     * but forkfd() has code to use clone() directly if the kernel is recent
     * enough (CLONE_PIDFD support). When that happens, the getpid() function calls
     * in the child side will fail. See:
     *
     * https://sourceware.org/bugzilla/show_bug.cgi?id=15368
     * https://yarchive.net/comp/linux/getpid_caching.html
     */
    static constexpr eventfd_t PidProperlyUpdated = 0x646950646f6f47,
        PidIncorrectlyCached = 0x646950646142;

    /*
     * Additionally, Red Hat botched the backport of the pidfd feature from 5.4
     * to their 4.18 tree for Red Hat Enterprise Linux 8.5/8.6. Kernel releases
     * 358 to 391 support pidfd, but poll(2) fails to wait on it and always
     * returns immediately
     *
     * https://bugreports.qt.io/browse/QTBUG-100174
     * https://bugzilla.redhat.com/show_bug.cgi?id=2107643
     * https://access.redhat.com/errata/RHSA-2022:6460
     */
    static constexpr eventfd_t PollWorks = 0x6c6c6f50646f6f47,
            PollFails = 0x6c6c6f50646142;
    static int ffd_extra_flags = -1;
    if (__builtin_expect(ffd_extra_flags < 0, false)) {
        // determine if we need to pass FFD_USE_FORK
        pid_t parentpid = getpid();
        int efdpid = eventfd(0, EFD_CLOEXEC);
        int efdpoll = eventfd(0, EFD_CLOEXEC);
        assert(efdpid != -1);

        int ffd = forkfd(FFD_CLOEXEC, &pid);

        if (ffd == -1) {
            // forkfd failed (probably CLONE_PIDFD rejected)
            ffd_extra_flags = FFD_USE_FORK;
        } else if (ffd == FFD_CHILD_PROCESS) {
            // child side - wait for parent to try to poll() us
            eventfd_t pollstatus;
            eventfd_read(efdpoll, &pollstatus);
            close(efdpoll);
            if (pollstatus == PollFails)
                _exit(127);

            // confirm that the PID updated
            bool pid_updated = (getpid() != parentpid);
            eventfd_write(efdpid, pid_updated ? PidProperlyUpdated : PidIncorrectlyCached);
            close(efdpid);
            if (!pid_updated)
                _exit(127);

            // no problems seen, return to caller as child process
            return { .pid = pid, .fd = FFD_CHILD_PROCESS };
        } else {
            // parent side
            int ret;
            eventfd_t result;

            // check if poll() works
            struct pollfd pfd = { .fd = ffd, .events = POLLIN };
            EINTR_LOOP(ret, poll(&pfd, 1, 1));  // 1ms is enough
            if (ret == 1) {
                eventfd_write(efdpoll, PollFails);
                close(efdpoll);
            } else {
                // it does, proceed with the getpid() check
                eventfd_write(efdpoll, PollWorks);
                close(efdpoll);

                EINTR_LOOP(ret, eventfd_read(efdpid, &result));
                close(efdpid);

                if (result == PidProperlyUpdated) {
                    // no problems seen, return to caller as parent process
                    ffd_extra_flags = 0;
                    return { .pid = pid, .fd = ffd };
                }
            }

            // found a problem, wait for the child and try again
            EINTR_LOOP(ret, forkfd_wait(ffd, nullptr, nullptr));
            forkfd_close(ffd);
            ffd_extra_flags = FFD_USE_FORK;
        }
    }
#else
    static constexpr int ffd_extra_flags = 0;
#endif

    int ffd = forkfd(FFD_CLOEXEC | ffd_extra_flags, &pid);
    if (ffd == -1) {
        perror("fork");
        exit(EX_OSERR);
    }
    return { .pid = pid, .fd = ffd };
}

static StartedChild spawn_child(int child_number, const std::vector<const char *> &common_args)
{
    assert(sApp->shmemfd != -1);
    std::string childnumstr = stdprintf("%d", child_number);

    StartedChild ret = {};

    // argument order must match exec_mode_run()
    std::vector argv = {
        static_cast<const char *>(program_invocation_name), "-x", childnumstr.c_str(),
    };
    argv.insert(argv.end(), common_args.begin(), common_args.end());
    assert(argv.back() == nullptr);

    if (sApp->gdb_server_comm.size()) {
#ifdef _WIN32
        // we need the actual Windows handle, because the file
        // descriptors don't inherit properly via gdbserver
       static std::string shmemfdstr = stdprintf("h%tx", _get_osfhandle(sApp->shmemfd));
       argv[3] = shmemfdstr.c_str();
#endif
       argv.insert(argv.begin(), {
            "gdbserver", "--no-startup-with-shell", sApp->gdb_server_comm.c_str(),
        });
    }

#ifdef _WIN32
    // save stderr
    static int saved_stderr = _dup(STDERR_FILENO);
    logging_init_child_preexec();

    if (sApp->gdb_server_comm.size()) {
        ret.pid = _spawnvp(_P_NOWAIT, argv[0], const_cast<char **>(argv.data()));
    } else {
        ret.pid = _spawnv(_P_NOWAIT, path_to_exe(), const_cast<char **>(argv.data()));
    }

    int saved_errno = errno;

    // restore stderr
    _dup2(saved_stderr, STDERR_FILENO);

    if (ret.pid == -1) {
        errno = saved_errno;
        perror("_spawnv");
        exit(EX_OSERR);
    }
#else
    ret = call_forkfd();
    if (ret.fd == FFD_CHILD_PROCESS) {
        logging_init_child_preexec();

        if (sApp->gdb_server_comm.size()) {
            execvp(argv[0], const_cast<char **>(argv.data()));
        }

        execv(path_to_exe(), const_cast<char **>(argv.data()));
        /* does not return */
        perror("execv");
        _exit(EX_OSERR);
    }
#endif /* __linux__ */

    return ret;
}

static int slices_for_test(const struct test *test)
{
    SlicePlans::Type type = [=]() {
        switch (test->flags & test_schedule_mask) {
        case test_schedule_sequential:  // sequential tests see the full system
        case test_schedule_fullsystem:
            return SlicePlans::FullSystem;

        case test_schedule_isolate_socket:
            return SlicePlans::IsolateSockets;

#ifdef SANDSTONE_DEVICE_CPU
        case test_schedule_isolate_coregroup:
            return SlicePlans::IsolateCoreGroup;
#elif defined(SANDSTONE_DEVICE_GPU)
        case test_schedule_isolate_numa_domain:
            return SlicePlans::IsolateNuma;
#endif

        case test_schedule_default:
            break;
        }
        return SlicePlans::Heuristic;
    }();
    if (type == SlicePlans::FullSystem) {
        sApp->main_thread_data()->device_range = { 0, sApp->device_count };
        sApp->main_thread_data()->thread_range = { 0, sApp->thread_count };
        return 1;
    }

    const SlicePlans::Slices &plan = sApp->slice_plans.plans[type];
    for (size_t i = 0; i < plan.size(); ++i) {
        sApp->main_thread_data(i)->device_range = plan[i].device_range;
        sApp->main_thread_data(i)->thread_range = plan[i].thread_range;
    }

    return plan.size();
}

static void run_one_test_children(ChildrenList &children, const struct test *test)
{
    int child_count = slices_for_test(test);
    if (sApp->current_fork_mode() != SandstoneApplication::ForkMode::exec_each_test) {
        assert(sApp->current_fork_mode() != SandstoneApplication::ForkMode::child_exec_each_test
                && "child_exec_each_test mode can only happen in the child side!");
        assert((sApp->current_fork_mode() != SandstoneApplication::ForkMode::no_fork || child_count == 1)
               && "-fno-fork can only start 1 child!");

        for (int i = 0; i < child_count; ++i) {
            StartedChild ret = { .fd = FFD_CHILD_PROCESS };
            if (sApp->current_fork_mode() == SandstoneApplication::ForkMode::fork_each_test)
                ret = call_forkfd();
            if (ret.fd == FFD_CHILD_PROCESS) {
                /* child - run test's code */
                logging_init_child_preexec();
                TestResult result = child_run(const_cast<struct test *>(test), i);
                if (sApp->current_fork_mode() == SandstoneApplication::ForkMode::fork_each_test)
                    _exit(test_result_to_exit_code(result));

                children.results.emplace_back(ChildExitStatus{ result });
                return;
            } else {
                children.add(ret);
            }
        }
    } else {
        std::string shmemfdstr = stdprintf("%d", sApp->shmemfd);
        std::string random_seed = random_format_seed();
        std::vector<const char *> common_args = { shmemfdstr.c_str(), test->id, random_seed.c_str() };
        save_test_knob_args(common_args, test->id);
        common_args.push_back(nullptr);
        for (int i = 0; i < child_count; ++i)
            children.add(spawn_child(i, common_args));
    }

    /* wait for the children */
    wait_for_children(children, test);
}

static void run_one_test_init_in_parent(ChildrenList &children, const struct test *test)
{
    // Set up device/thread ranges so logging can iterate over them
    sApp->main_thread_data()->device_range = { 0, sApp->device_count };
    sApp->main_thread_data()->thread_range = { 0, sApp->thread_count };

    s_current_test = test;
    auto resetCurrentTest = scopeExit([&] { s_current_test = nullptr; });

    TestResult res;
    init_per_thread_data();
    int ret = test->test_init(const_cast<struct test *>(test));

    PerThreadData::Main *main = sApp->main_thread_data();
    if (ret < 0) [[likely]] {
        assert(main->has_skipped() &&
               "Internal error: init-in-parent returned a skip but did not call log_skip()");
        res = TestResult::Skipped;
    } else if (ret == EXIT_SUCCESS) [[likely]] {
        if (main->has_skipped()) {
            res = TestResult::Skipped;
        } else if (main->has_failed()) {
            res = TestResult::Failed;
        } else {
            assert(!"Internal error: init-in-parent succeeded");
            __builtin_unreachable();
        }
    } else {
        assert(main->has_failed() &&
               "Internal error: init-in-parent returned an error but did not call log_error()");
        res = TestResult::Failed;
    }

    children.results.emplace_back(ChildExitStatus{ res });
}

static void run_one_test_in_parent(ChildrenList &children, const struct test *test)
{
    // Set up device/thread ranges so logging can iterate over them
    sApp->main_thread_data()->device_range = { 0, sApp->device_count };
    sApp->main_thread_data()->thread_range = { 0, sApp->thread_count };
    auto state = run_one_test_inner(const_cast<struct test*>(test));
    children.results.emplace_back(ChildExitStatus{ state });
}

static TestResult run_one_test_once(const struct test *test)
{
    ChildrenList children;

    sApp->current_test_count++;
    if (device_features_t missing = (test->compiler_minimum_device | test->minimum_cpu) & ~device_features) {
        init_per_thread_data();

        // be as brief as possible: if the feature missing is required by the
        // test, then report only those. if not, report the features the test
        // was compiled with, but not those already required by the framework.
        device_features_t missing_to_report = missing & ~test->compiler_minimum_device;
        if (missing_to_report) {
            log_skip(CpuNotSupportedSkipCategory, "test requires %s\n", device_features_to_string(missing_to_report).c_str());
        } else {
            missing_to_report = missing & ~device_compiler_features;
            log_skip(CpuNotSupportedSkipCategory, "test compiled with %s\n", device_features_to_string(missing_to_report).c_str());
        }

        children.results.emplace_back(ChildExitStatus{ TestResult::Skipped });
    } else if (test->quality_level == TEST_QUALITY_BETA && test->quality_level < sApp->requested_quality) {
        init_per_thread_data();
        log_skip(TestResourceIssueSkipCategory, "Test %s is in BETA quality, try again using --beta option", test->id);
        children.results.emplace_back(ChildExitStatus{ TestResult::Skipped });
    } else if (test->flags & test_in_parent) {
        run_one_test_in_parent(children, test);
    } else if (test->flags & test_init_in_parent) {
        run_one_test_init_in_parent(children, test);
    } else {
        run_one_test_children(children, test);
    }

    // print results and find out if the test failed
    MonotonicTimePoint now = MonotonicTimePoint::clock::now();
    for (ChildExitStatus &result : children.results) {
        if (result.endtime == MonotonicTimePoint())
            result.endtime = now;
    }
    TestResult testResult = logging_print_results(children.results, test);
    switch (testResult) {
    case TestResult::Passed:
    case TestResult::Skipped:
    case TestResult::Failed:
    case TestResult::CoreDumped:
    case TestResult::Killed:
        break;          // continue running tests

    case TestResult::OperatingSystemError:
    case TestResult::TimedOut:
    case TestResult::OutOfMemory:
        if (!sApp->ignore_os_errors) {
            _exit(cleanup_global(EXIT_INVALID, {}));
        } else {
            // not a pass either, but won't affect the result
            testResult = TestResult::Skipped;
        }
        break;

    case TestResult::Interrupted:
        assert(false && "internal error: shouldn't have got here!");
        __builtin_unreachable();
        break;
    }

    return testResult;
}

static void analyze_test_failures(const struct test *test, int fail_count, int attempt_count,
                                  const PerThreadFailures &per_thread_failures)
{
    logging_printf(LOG_LEVEL_VERBOSE(1), "# Test failed %d out of %d times"
                                         " (%.1f%%)\n", fail_count, attempt_count,
                   fail_count * 100.0 / attempt_count);
    logging_restricted(LOG_LEVEL_QUIET, "Test failed (#%s%x).", test->id, fail_count - 1);

    // First, determine if all threads failed the exact same way
    bool all_threads_failed_equally = true;
    PerThreadFailures::value_type fail_pattern = 0;
    int nfailures = 0;
    for (size_t i = 0; i < sApp->thread_count && all_threads_failed_equally; ++i) {
        if (per_thread_failures[i]) {
            if (++nfailures == 1)
                fail_pattern = per_thread_failures[i];
            else if (per_thread_failures[i] != fail_pattern)
                all_threads_failed_equally = false;
        }
    }
    if (all_threads_failed_equally && nfailures == sApp->thread_count) {
        logging_printf(LOG_LEVEL_VERBOSE(1), "# All threads failed equally. This is highly unlikely (SW bug?)\n");
        return;
    }

    analyze_test_failures_for_topology(test, per_thread_failures);
}

static ShortDuration test_duration(const test_cfg_info &test_cfg)
{
    const struct test *test = test_cfg.test;

    /* Start with the test preferred default time */
    ShortDuration target_duration(test->desired_duration);
    ShortDuration min_duration(test->minimum_duration);
    ShortDuration max_duration(test->maximum_duration);

    /* apply the global (-t) override */
    if (sApp->test_time.count())
        target_duration = sApp->test_time;

    /* apply the per-test time from the test list (overrides global -t) */
    if (test_cfg.duration.count())
        target_duration = test_cfg.duration;

    /* fallback to the default if test preference is zero */
    if (target_duration <= 0s)
        target_duration = SandstoneApplication::DefaultTestDuration;

    /* if --force-test-time specified, ignore the test-specified time limits */
    if (sApp->force_test_time)
        return target_duration;

    /* clip to the maximum duration */
    if (max_duration != 0s && target_duration > max_duration)
        target_duration = max_duration;
    /* and clip to the minimum duration */
    if (target_duration < min_duration)
        target_duration = min_duration;

    return target_duration;
}

TestResult run_one_test(const test_cfg_info &test_cfg, PerThreadFailures &per_thread_failures)
{
    const struct test *test = test_cfg.test;
    TestResult state = TestResult::Skipped;
    int fail_count = 0;
    std::unique_ptr<char[]> random_allocation;
    MonotonicTimePoint first_iteration_target;
    bool auto_fracture = false;
    Duration runtime = 0ms;

    // resize and zero the storage
    if (per_thread_failures.size() == sApp->thread_count) {
        std::fill_n(per_thread_failures.begin(), sApp->thread_count, 0);
    } else {
        per_thread_failures.clear();
        per_thread_failures.resize(sApp->thread_count, 0);
    }
    auto mark_up_per_thread_fail = [&per_thread_failures, &fail_count](int i) {
        ++fail_count;
        if (i >= SandstoneApplication::MaxRetestCount)
            return;
        for_each_test_thread([&](PerThreadData::Test *data, int i) {
            using U = PerThreadFailures::value_type;
            if (data->has_failed())
                per_thread_failures[i] |= U(1) << i;
        });
    };

    sApp->current_test_duration = test_duration(test_cfg);
    first_iteration_target = MonotonicTimePoint::clock::now() + 10ms;

    if (sApp->inject_idle > 0) {
        float idle_rate = sApp->inject_idle / 100.0;
        sApp->shmem->current_test_sleep_duration = duration_cast<microseconds>(
            sApp->current_test_duration * idle_rate
        );
        // When injecting idle time, reduce the test duration accordingly
        sApp->current_test_duration = duration_cast<ShortDuration>(
            sApp->current_test_duration * (1 - idle_rate)
        );
    }

    sApp->shmem->cfg.quick_run = sApp->max_test_loop_count == 1;
    if (sApp->max_test_loop_count) {
        sApp->shmem->current_max_loop_count = sApp->max_test_loop_count;
    } else if (test->desired_duration == -1) {
        sApp->shmem->current_max_loop_count = -1;
    } else if (sApp->test_tests_enabled()) {
        // don't fracture in the test-the-test mode
        sApp->shmem->current_max_loop_count = -1;
    } else if (test->fracture_loop_count == 0) {
        /* for automatic fracture mode, do a 40 loop count */
        sApp->shmem->current_max_loop_count = 40;
        auto_fracture = true;
    } else {
        sApp->shmem->current_max_loop_count = test->fracture_loop_count;
    }

    assert(sApp->retest_count >= 0);

    /* First we go to do our -- possibly fractured -- normal run, we'll do retries after */
    for (sApp->current_iteration_count = 0;; ++sApp->current_iteration_count) {

#if SANDSTONE_FREQUENCY_MANAGER
        //change frequency per fracture
        if (sApp->vary_frequency_mode == true)
            sApp->frequency_manager->change_core_frequency();

        //change uncore frequency per fracture
        if (sApp->vary_uncore_frequency_mode == true)
            sApp->frequency_manager->change_uncore_frequency();
#endif

        init_internal(test);

        // calculate starttime->endtime, reduce the overhead to have better test runtime calculations
        sApp->shmem->current_test_endtime =
                calculate_wallclock_deadline(sApp->current_test_duration - runtime,
                                             &sApp->current_test_starttime);
        state = run_one_test_once(test);
        runtime += MonotonicTimePoint::clock::now() - sApp->current_test_starttime;

        cleanup_internal(test);

        if ((sApp->shmem->current_max_loop_count > 0
             && MonotonicTimePoint::clock::now() < first_iteration_target && auto_fracture))
            sApp->shmem->current_max_loop_count *= 2;

        /* don't repeat skipped tests */
        if (state == TestResult::Skipped)
            goto out;

        if (state != TestResult::Passed) {
            // this counts as the first failure regardless of how many fractures we've run
            mark_up_per_thread_fail(0);
            break;
        }

        // do we fracture?
        if (sApp->shmem->current_max_loop_count <= 0 || sApp->max_test_loop_count
                || (runtime >= sApp->current_test_duration))
            goto out;

        // Advance the random seed.
        random_advance_seed();
    }

    /* now we process retries */
    if (fail_count > 0) {
        // disable fracture
        if (sApp->shmem->current_max_loop_count > 0 &&
                sApp->shmem->current_max_loop_count != sApp->max_test_loop_count)
            sApp->shmem->current_max_loop_count = -1;

        int iterations;
        auto should_retry_test = [&]() {
            // allow testing double the regular count if we've only ever
            // failed once (the original run)
            if (sApp->total_retest_count == 0)
                return false;
            if (fail_count == 1)
                return iterations <= 2 * sApp->retest_count;
            return iterations <= sApp->retest_count;
        };
        for (iterations = 1; should_retry_test(); ++iterations) {
            // reset for the next retry iteration
            if (sApp->total_retest_count > 0)
                --sApp->total_retest_count;
            sApp->current_iteration_count = -iterations;
            init_internal(test);
            sApp->shmem->current_test_endtime =
                    calculate_wallclock_deadline(sApp->current_test_duration,
                                                 &sApp->current_test_starttime);
            state = run_one_test_once(test);
            cleanup_internal(test);

            if (state > TestResult::Passed)
                mark_up_per_thread_fail(iterations);
        }

        analyze_test_failures(test, fail_count, iterations, per_thread_failures);
        state = TestResult::Failed;
    }

out:
#if SANDSTONE_FREQUENCY_MANAGER
    //reset frequency level idx for the next test
    if (sApp->vary_frequency_mode || sApp->vary_uncore_frequency_mode)
        sApp->frequency_manager->reset_frequency_level_idx();
#endif

    random_advance_seed();      // advance seed for the next test
    logging_flush();
    return state;
}
