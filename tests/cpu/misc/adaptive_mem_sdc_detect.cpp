/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "adaptive_mem_sdc_detect.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace {

constexpr size_t kMaxHistogramLanes = 32;
constexpr size_t kHexWindowBytes = 16;

void append_format(std::string &out, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    va_list measure;
    va_copy(measure, args);
    const int length = vsnprintf(nullptr, 0, format, measure);
    va_end(measure);
    if (length > 0) {
        const size_t offset = out.size();
        const size_t count = static_cast<size_t>(length);
        out.resize(offset + count + 1);
        vsnprintf(&out[offset], count + 1, format, args);
        out.resize(offset + count);
    }
    va_end(args);
}

/* Keep arbitrary caller tags from turning one event into multiple lines. */
void append_quoted(std::string &out, const char *value)
{
    if (value == nullptr) {
        out += "null";
        return;
    }

    out.push_back('"');
    for (const char *p = value; *p != '\0'; ++p) {
        const unsigned char ch = static_cast<unsigned char>(*p);
        switch (ch) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20 || ch == 0x7f)
                append_format(out, "\\x%02x", static_cast<unsigned>(ch));
            else
                out.push_back(static_cast<char>(ch));
            break;
        }
    }
    out.push_back('"');
}

void write_stderr(const std::string &line)
{
    const ssize_t written = write(STDERR_FILENO, line.data(), line.size());
    (void)written;
}

void report_event_log_error(const char *operation, const char *path, int error,
                            ssize_t written, size_t expected)
{
    std::string line = "adaptive_sdc event=event_log_error operation=";
    line += operation;
    line += " path=";
    append_quoted(line, path);
    append_format(line, " written=%zd expected=%zu error=", written, expected);
    append_quoted(line, error != 0 ? strerror(error) : "short_write");
    line.push_back('\n');
    write_stderr(line);
}

void emit_line(std::string &line)
{
    line.push_back('\n');
    write_stderr(line);

    const char *path = getenv("ADAPTIVE_MEM_SDC_EVENT_LOG");
    if (path == nullptr || *path == '\0')
        return;

    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        report_event_log_error("open", path, errno, -1, line.size());
        return;
    }

    /* One write is intentional: concurrent workers append whole event lines. */
    const ssize_t written = write(fd, line.data(), line.size());
    const int error = written < 0 ? errno : 0;
    const int closed = close(fd);
    (void)closed;
    if (written < 0 || static_cast<size_t>(written) != line.size())
        report_event_log_error("write", path, error, written, line.size());
}

void append_prefix(std::string &line, const char *event, const AdaptiveSdcLocation *loc)
{
    struct timespec realtime = {};
    struct timespec monotonic = {};
    const bool have_realtime = clock_gettime(CLOCK_REALTIME, &realtime) == 0;
    const bool have_monotonic = clock_gettime(CLOCK_MONOTONIC, &monotonic) == 0;

    line = "adaptive_sdc event=";
    line += event;

    bool formatted = false;
    if (have_realtime) {
        const time_t seconds = realtime.tv_sec;
        struct tm utc = {};
        char iso[32];
        if (gmtime_r(&seconds, &utc) != nullptr &&
            strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &utc) != 0) {
            append_format(line, " realtime=%s.%09ldZ", iso, realtime.tv_nsec);
            formatted = true;
        }
    }
    if (!formatted)
        line += " realtime=unavailable";

    if (have_monotonic) {
        const int64_t monotonic_ns = static_cast<int64_t>(monotonic.tv_sec) * INT64_C(1000000000)
                                   + static_cast<int64_t>(monotonic.tv_nsec);
        append_format(line, " monotonic_ns=%" PRId64, monotonic_ns);
    } else {
        line += " monotonic_ns=unavailable";
    }

    append_format(line, " pid=%ld combo=%d cpu=%d", static_cast<long>(getpid()),
                  loc != nullptr ? loc->combo : -1, loc != nullptr ? loc->cpu : -1);
    if (loc != nullptr)
        append_format(line, " sweep=%" PRIu64, loc->sweep);
    else
        line += " sweep=0";

    line += " engine=";
    append_quoted(line, loc != nullptr ? loc->engine : nullptr);
    line += " site=";
    append_quoted(line, loc != nullptr ? loc->site : nullptr);
    append_format(line, " vector_bytes=%zu lane_bytes=%zu flags=0x%x",
                  loc != nullptr ? loc->vector_bytes : 0,
                  loc != nullptr ? loc->lane_bytes : 0,
                  loc != nullptr ? loc->flags : 0u);
}

void append_pointer(std::string &line, const void *address)
{
    append_format(line, "0x%" PRIxPTR, reinterpret_cast<uintptr_t>(address));
}

void append_digests(std::string &line, uint32_t actual_crc, uint32_t expected_crc,
                    uint64_t actual_fnv, uint64_t expected_fnv)
{
    append_format(line,
                  " actual_crc32=%08" PRIx32 " expected_crc32=%08" PRIx32
                  " actual_fnv1a64=%016" PRIx64 " expected_fnv1a64=%016" PRIx64,
                  actual_crc, expected_crc, actual_fnv, expected_fnv);
}

size_t lane_count(const AdaptiveSdcLocation *loc)
{
    if (loc == nullptr || loc->vector_bytes == 0 || loc->lane_bytes == 0 ||
        loc->vector_bytes % loc->lane_bytes != 0)
        return 0;
    return loc->vector_bytes / loc->lane_bytes;
}

void append_hex_window(std::string &line, const uint8_t *bytes, size_t size)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
        line.push_back(hex[bytes[i] >> 4]);
        line.push_back(hex[bytes[i] & 0x0f]);
    }
}

} // namespace

extern "C" uint32_t adaptive_sdc_crc32(const void *data, size_t size)
{
    if (data == nullptr && size != 0)
        return 0;

    const auto *bytes = static_cast<const uint8_t *>(data);
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
    return crc ^ UINT32_C(0xffffffff);
}

extern "C" uint64_t adaptive_sdc_fnv1a64(const void *data, size_t size)
{
    if (data == nullptr && size != 0)
        return 0;

    const auto *bytes = static_cast<const uint8_t *>(data);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

extern "C" int adaptive_sdc_check_bytes(const AdaptiveSdcLocation *loc, const char *buffer_name,
                                        const void *actual, const void *expected, size_t bytes)
{
    const bool enhanced = loc != nullptr && (loc->flags & ADAPTIVE_SDC_CHECK_DIGESTS) != 0;

    if (bytes != 0 && (actual == nullptr || expected == nullptr)) {
        std::string line;
        append_prefix(line, "check_error", loc);
        line += " reason=invalid_input buffer=";
        append_quoted(line, buffer_name);
        append_format(line, " bytes=%zu actual_null=%s expected_null=%s", bytes,
                      actual == nullptr ? "true" : "false",
                      expected == nullptr ? "true" : "false");
        emit_line(line);
        return 0;
    }

    const bool bytes_equal = bytes == 0 || memcmp(actual, expected, bytes) == 0;
    if (bytes_equal && !enhanced)
        return 1;

    const uint32_t actual_crc = adaptive_sdc_crc32(actual, bytes);
    const uint32_t expected_crc = adaptive_sdc_crc32(expected, bytes);
    const uint64_t actual_fnv = adaptive_sdc_fnv1a64(actual, bytes);
    const uint64_t expected_fnv = adaptive_sdc_fnv1a64(expected, bytes);
    const bool digest_mismatch = actual_crc != expected_crc || actual_fnv != expected_fnv;

    if (bytes_equal) {
        if (!digest_mismatch)
            return 1;

        std::string line;
        append_prefix(line, "sdc", loc);
        line += " reason=digest_mismatch digest_mismatch=true buffer=";
        append_quoted(line, buffer_name);
        append_format(line, " bytes=%zu", bytes);
        append_digests(line, actual_crc, expected_crc, actual_fnv, expected_fnv);
        emit_line(line);
        return 0;
    }

    const auto *actual_bytes = static_cast<const uint8_t *>(actual);
    const auto *expected_bytes = static_cast<const uint8_t *>(expected);
    const size_t lanes = lane_count(loc);
    size_t histogram[kMaxHistogramLanes] = {};
    size_t histogram_overflow = 0;
    size_t histogram_unmapped = 0;
    size_t mismatch_count = 0;
    size_t first = 0;

    /* Do not stop at the first mismatch: lane evidence needs the whole buffer. */
    for (size_t i = 0; i < bytes; ++i) {
        if (actual_bytes[i] == expected_bytes[i])
            continue;
        if (mismatch_count == 0)
            first = i;
        ++mismatch_count;
        if (lanes == 0) {
            ++histogram_unmapped;
            continue;
        }
        const size_t lane = (i % loc->vector_bytes) / loc->lane_bytes;
        if (lane < kMaxHistogramLanes)
            ++histogram[lane];
        else
            ++histogram_overflow;
    }

    std::string line;
    append_prefix(line, "sdc", loc);
    line += " reason=byte_mismatch buffer=";
    append_quoted(line, buffer_name);
    append_format(line, " bytes=%zu mismatch_count=%zu first_address=", bytes, mismatch_count);
    append_pointer(line, actual_bytes + first);
    append_format(line, " first_offset=%zu", first);
    if (loc != nullptr && loc->vector_bytes != 0)
        append_format(line, " vector_index=%zu", first / loc->vector_bytes);
    else
        line += " vector_index=unavailable";
    if (lanes != 0)
        append_format(line, " lane_index=%zu", (first % loc->vector_bytes) / loc->lane_bytes);
    else
        line += " lane_index=unavailable";

    append_format(line, " expected_byte=0x%02x actual_byte=0x%02x xor_byte=0x%02x",
                  static_cast<unsigned>(expected_bytes[first]),
                  static_cast<unsigned>(actual_bytes[first]),
                  static_cast<unsigned>(expected_bytes[first] ^ actual_bytes[first]));
    append_digests(line, actual_crc, expected_crc, actual_fnv, expected_fnv);
    line += digest_mismatch ? " digest_mismatch=true" : " digest_mismatch=false";

    line += " lane_histogram=";
    if (lanes == 0) {
        line += "unavailable";
    } else {
        const size_t logged_lanes = std::min(lanes, kMaxHistogramLanes);
        line.push_back('[');
        for (size_t lane = 0; lane < logged_lanes; ++lane)
            append_format(line, "%s%zu:%zu", lane != 0 ? "," : "", lane, histogram[lane]);
        line.push_back(']');
    }
    append_format(line, " lane_histogram_overflow=%zu lane_histogram_unmapped=%zu",
                  histogram_overflow, histogram_unmapped);

    const size_t window_begin = first > 8 ? first - 8 : 0;
    const size_t window_size = std::min(kHexWindowBytes, bytes - window_begin);
    append_format(line, " window_offset=%zu window_bytes=%zu actual_window=",
                  window_begin, window_size);
    append_hex_window(line, actual_bytes + window_begin, window_size);
    line += " expected_window=";
    append_hex_window(line, expected_bytes + window_begin, window_size);

    emit_line(line);
    return 0;
}

extern "C" void adaptive_sdc_record_injection(const AdaptiveSdcLocation *loc, const char *target,
                                               const void *address, size_t byte_offset, unsigned bit,
                                               uint8_t before, uint8_t after)
{
    std::string line;
    append_prefix(line, "injection", loc);
    line += " classification=intentional_injection real_sdc=false target=";
    append_quoted(line, target);
    line += " address=";
    append_pointer(line, address);
    append_format(line, " byte_offset=%zu bit=%u before=0x%02x after=0x%02x",
                  byte_offset, bit, static_cast<unsigned>(before), static_cast<unsigned>(after));
    emit_line(line);
}
