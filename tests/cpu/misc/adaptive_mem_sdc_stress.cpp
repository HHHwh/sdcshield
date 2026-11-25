/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sandstone.h"
#include "adaptive_mem_sdc.h"
#include "adaptive_mem_sdc_detect.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__aarch64__)

#include <arm_neon.h>

namespace {

constexpr size_t kCanaryBytes = 64;
constexpr size_t kTailBytes = 64;
constexpr size_t kMaxDataOffset = 63;
constexpr size_t kVectorBytes = 16;
constexpr size_t kOffsets[] = { 0, 1, 7, 15, 56, 63 };
constexpr uint8_t kZeroPadding[kTailBytes + kMaxDataOffset] = {};

enum BufferId {
    SRC_A,
    SRC_B,
    SRC_C,
    SRC_A_SHADOW,
    SRC_B_SHADOW,
    SRC_C_SHADOW,
    GOLDEN,
    GOLDEN_SHADOW,
    PRIMARY,
    ALTERNATE,
    VECTOR,
    PLAN,
    PLAN_SHADOW,
    BUFFER_COUNT
};

const char *const kBufferNames[BUFFER_COUNT] = {
    "srcA", "srcB", "srcC",
    "srcA_shadow", "srcB_shadow", "srcC_shadow",
    "golden", "golden_shadow",
    "primary", "alternate", "vector", "plan", "plan_shadow"
};

enum class Access {
    Aligned,
    Unaligned,
    Cacheline,
    Random,
    Mixed
};

enum class InjectTarget {
    None,
    Source,
    Golden,
    Output,
    Replica,
    Canary,
    Plan
};

struct Config {
    unsigned block_kb;
    size_t block_bytes;
    unsigned rounds;
    Access access;
    unsigned grain;                 /* zero means mixed */
    bool soak;
};

struct Recipe {
    unsigned slot_rotation;
    bool use_c;
    unsigned integer_repeats;
    unsigned fp_repeats;
    unsigned random_repeats;
};

struct Injection {
    InjectTarget target = InjectTarget::None;
    size_t byte = 0;
    unsigned bit = 0;
    uint64_t sweep = 0;
    bool done = false;
};

class GuardedBuffer {
public:
    GuardedBuffer() = default;
    GuardedBuffer(const GuardedBuffer &) = delete;
    GuardedBuffer &operator=(const GuardedBuffer &) = delete;

    ~GuardedBuffer()
    {
        reset();
    }

    bool allocate(size_t bytes)
    {
        reset();
        logical_bytes_ = bytes;
        allocation_bytes_ = kCanaryBytes + kMaxDataOffset + bytes
                          + kTailBytes + kCanaryBytes;
        allocation_ = static_cast<uint8_t *>(malloc(allocation_bytes_));
        if (allocation_ == nullptr) {
            allocation_bytes_ = 0;
            logical_bytes_ = 0;
            return false;
        }
        data_ = allocation_ + kCanaryBytes;
        memset(allocation_, 0, allocation_bytes_);
        memset_random(front_expected_, sizeof(front_expected_));
        memset_random(back_expected_, sizeof(back_expected_));
        return true;
    }

    void set_offset(size_t offset)
    {
        data_offset_ = offset;
        /* The back padding includes the mandatory 64-byte tail and the
         * inactive offset slack.  Clearing both padding regions on every
         * layout change also removes bytes belonging to the previous layout. */
        memset(front_padding(), 0, front_padding_size());
        memset(back_padding(), 0, back_padding_size());
        memcpy(front(), front_expected_, kCanaryBytes);
        memcpy(back(), back_expected_, kCanaryBytes);
    }

    void reset()
    {
        free(allocation_);
        allocation_ = nullptr;
        data_ = nullptr;
        allocation_bytes_ = 0;
        logical_bytes_ = 0;
        data_offset_ = 0;
    }

    uint8_t *data() { return data_ + data_offset_; }
    const uint8_t *data() const { return data_ + data_offset_; }
    size_t size() const { return logical_bytes_; }

    uint8_t *front() { return allocation_; }
    const uint8_t *front() const { return allocation_; }
    uint8_t *front_padding() { return data_; }
    const uint8_t *front_padding() const { return data_; }
    size_t front_padding_size() const { return data_offset_; }
    uint8_t *back_padding() { return data() + logical_bytes_; }
    const uint8_t *back_padding() const { return data() + logical_bytes_; }
    size_t back_padding_size() const
    {
        return static_cast<size_t>(back() - back_padding());
    }
    uint8_t *back() { return allocation_ + allocation_bytes_ - kCanaryBytes; }
    const uint8_t *back() const { return allocation_ + allocation_bytes_ - kCanaryBytes; }
    const uint8_t *front_expected() const { return front_expected_; }
    const uint8_t *back_expected() const { return back_expected_; }

private:
    uint8_t *allocation_ = nullptr;
    uint8_t *data_ = nullptr;
    size_t allocation_bytes_ = 0;
    size_t logical_bytes_ = 0;
    size_t data_offset_ = 0;
    uint8_t front_expected_[kCanaryBytes] = {};
    uint8_t back_expected_[kCanaryBytes] = {};
};

/* set_offset is kept separate from allocation so mixed mode can rotate the
 * logical pointer without reallocating.  The data base is fixed here. */
inline uint64_t load_u64(const void *base, size_t offset)
{
    uint64_t value;
    memcpy(&value, static_cast<const uint8_t *>(base) + offset, sizeof(value));
    return value;
}

inline uint32_t load_u32(const void *base, size_t offset)
{
    uint32_t value;
    memcpy(&value, static_cast<const uint8_t *>(base) + offset, sizeof(value));
    return value;
}

inline void store_u64(void *base, size_t offset, uint64_t value)
{
    memcpy(static_cast<uint8_t *>(base) + offset, &value, sizeof(value));
}

inline void store_u32(void *base, size_t offset, uint32_t value)
{
    memcpy(static_cast<uint8_t *>(base) + offset, &value, sizeof(value));
}

inline int64_t bits_to_i64(uint64_t bits)
{
    int64_t value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

inline void store_double(void *base, size_t offset, double value)
{
    memcpy(static_cast<uint8_t *>(base) + offset, &value, sizeof(value));
}

inline uint64x2_t neon_load_u64(const void *address)
{
    uint64x2_t value;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(value) : "r"(address) : "memory");
    return value;
}

inline uint32x4_t neon_load_u32(const void *address)
{
    uint32x4_t value;
    __asm__ volatile("ldr %q0, [%1]" : "=w"(value) : "r"(address) : "memory");
    return value;
}

inline void neon_store_u64(void *address, uint64x2_t value)
{
    __asm__ volatile("str %q0, [%1]" : : "w"(value), "r"(address) : "memory");
}

inline void neon_store_u32(void *address, uint32x4_t value)
{
    __asm__ volatile("str %q0, [%1]" : : "w"(value), "r"(address) : "memory");
}

AdaptiveSdcLocation make_location(const char *site, int combo, int cpu,
                                  uint64_t sweep, size_t lane_bytes = 8)
{
    return { "STRESS-NEON", site, combo, cpu, sweep, kVectorBytes,
             lane_bytes, ADAPTIVE_SDC_CHECK_DIGESTS };
}

unsigned long long parse_bounded_env(const char *name, unsigned long long fallback,
                                     unsigned long long minimum,
                                     unsigned long long maximum,
                                     bool *valid_explicit = nullptr)
{
    if (valid_explicit != nullptr)
        *valid_explicit = false;
    const char *text = getenv(name);
    if (text == nullptr)
        return fallback;

    errno = 0;
    char *end = nullptr;
    const unsigned long long value = strtoull(text, &end, 10);
    if (text[0] < '0' || text[0] > '9' || end == text || *end != '\0' ||
        errno == ERANGE || value < minimum || value > maximum) {
        fprintf(stderr,
                "adaptive_mem_sdc: %s='%s' is invalid (expected %llu..%llu), using %llu\n",
                name, text, minimum, maximum, fallback);
        return fallback;
    }
    if (valid_explicit != nullptr)
        *valid_explicit = true;
    return value;
}

Access parse_access()
{
    const char *text = getenv("ADAPTIVE_MEM_SDC_ACCESS");
    if (text == nullptr || strcmp(text, "mixed") == 0)
        return Access::Mixed;
    if (strcmp(text, "aligned") == 0)
        return Access::Aligned;
    if (strcmp(text, "unaligned") == 0)
        return Access::Unaligned;
    if (strcmp(text, "cacheline") == 0)
        return Access::Cacheline;
    if (strcmp(text, "random") == 0)
        return Access::Random;
    fprintf(stderr,
            "adaptive_mem_sdc: ADAPTIVE_MEM_SDC_ACCESS='%s' is invalid, using mixed\n",
            text);
    return Access::Mixed;
}

unsigned parse_grain()
{
    const char *text = getenv("ADAPTIVE_MEM_SDC_GRAIN");
    if (text == nullptr || strcmp(text, "mixed") == 0)
        return 0;
    if (strcmp(text, "1") == 0)
        return 1;
    if (strcmp(text, "2") == 0)
        return 2;
    if (strcmp(text, "4") == 0)
        return 4;
    if (strcmp(text, "8") == 0)
        return 8;
    if (strcmp(text, "16") == 0)
        return 16;
    if (strcmp(text, "32") == 0)
        return 32;
    if (strcmp(text, "64") == 0)
        return 64;
    fprintf(stderr,
            "adaptive_mem_sdc: ADAPTIVE_MEM_SDC_GRAIN='%s' is invalid, using mixed\n",
            text);
    return 0;
}

const char *access_name(Access access)
{
    switch (access) {
    case Access::Aligned: return "aligned";
    case Access::Unaligned: return "unaligned";
    case Access::Cacheline: return "cacheline";
    case Access::Random: return "random";
    case Access::Mixed: return "mixed";
    }
    return "mixed";
}

const char *grain_name(unsigned grain)
{
    switch (grain) {
    case 1: return "1";
    case 2: return "2";
    case 4: return "4";
    case 8: return "8";
    case 16: return "16";
    case 32: return "32";
    case 64: return "64";
    default: return "mixed";
    }
}

const char *inject_target_name(InjectTarget target)
{
    switch (target) {
    case InjectTarget::Source: return "source";
    case InjectTarget::Golden: return "golden";
    case InjectTarget::Output: return "output";
    case InjectTarget::Replica: return "replica";
    case InjectTarget::Canary: return "canary";
    case InjectTarget::Plan: return "plan";
    case InjectTarget::None: return "none";
    }
    return "none";
}

unsigned long long parse_injection_number(const char *name,
                                          unsigned long long maximum,
                                          bool &valid)
{
    const char *text = getenv(name);
    if (text == nullptr)
        return 0;

    errno = 0;
    char *end = nullptr;
    const unsigned long long value = strtoull(text, &end, 10);
    if (text[0] < '0' || text[0] > '9' || end == text || *end != '\0' ||
        errno == ERANGE || value > maximum) {
        fprintf(stderr,
                "adaptive_mem_sdc: %s='%s' is invalid (expected 0..%llu), disabling injection\n",
                name, text, maximum);
        valid = false;
        return 0;
    }
    return value;
}

Injection parse_injection()
{
    Injection injection;
    const char *target = getenv("ADAPTIVE_MEM_SDC_INJECT");
    if (target == nullptr)
        return injection;

    if (strcmp(target, "source") == 0)
        injection.target = InjectTarget::Source;
    else if (strcmp(target, "golden") == 0)
        injection.target = InjectTarget::Golden;
    else if (strcmp(target, "output") == 0)
        injection.target = InjectTarget::Output;
    else if (strcmp(target, "replica") == 0)
        injection.target = InjectTarget::Replica;
    else if (strcmp(target, "canary") == 0)
        injection.target = InjectTarget::Canary;
    else if (strcmp(target, "plan") == 0)
        injection.target = InjectTarget::Plan;
    else {
        fprintf(stderr,
                "adaptive_mem_sdc: ADAPTIVE_MEM_SDC_INJECT='%s' is invalid, disabling injection\n",
                target);
        return injection;
    }

    bool valid = true;
    injection.byte = static_cast<size_t>(parse_injection_number(
            "ADAPTIVE_MEM_SDC_INJECT_BYTE", static_cast<unsigned long long>(SIZE_MAX), valid));
    injection.bit = static_cast<unsigned>(parse_injection_number(
            "ADAPTIVE_MEM_SDC_INJECT_BIT", 7, valid));
    injection.sweep = static_cast<uint64_t>(parse_injection_number(
            "ADAPTIVE_MEM_SDC_INJECT_SWEEP", UINT64_MAX, valid));
    if (!valid)
        injection.target = InjectTarget::None;
    return injection;
}

Config parse_config(int guarded_count)
{
    Config config;
    /* init already guarded this footprint: count × 16B equals block bytes. */
    config.block_kb = static_cast<unsigned>(guarded_count / 64);
    config.block_bytes = static_cast<size_t>(config.block_kb) * 1024;
    config.soak = false;
    const char *profile = getenv("ADAPTIVE_MEM_SDC_PROFILE");
    if (profile != nullptr && strcmp(profile, "soak") == 0)
        config.soak = true;
    config.rounds = static_cast<unsigned>(parse_bounded_env(
            "ADAPTIVE_MEM_SDC_ROUNDS", config.soak ? 8 : 3, 1, 16));
    config.access = parse_access();
    config.grain = parse_grain();
    return config;
}

Recipe make_recipe(int combo)
{
    const ComboDef &combo_def = kComboTable[combo];
    Recipe recipe;
    recipe.slot_rotation = (static_cast<unsigned>(combo) +
            static_cast<unsigned>(combo_def.load)) % 4;
    recipe.use_c = combo_def.ldr == 3;
    recipe.integer_repeats = combo_def.load == LOAD_MLA ? 2 : 1;
    recipe.fp_repeats = combo_def.load == LOAD_FP64ROT ? 2 : 1;
    recipe.random_repeats = static_cast<unsigned>(combo_def.str - 1);
    /* kComboTable is an enhanced recipe modifier here: it rotates operation
     * slots, selects source arity, and scales compute/copy intensity.  This
     * engine does not claim instruction equivalence with standard recipes. */
    return recipe;
}

size_t recipe_slot(const Recipe &recipe, size_t vector_index)
{
    return (vector_index + recipe.slot_rotation) % 4;
}

long long stress_memory_bytes()
{
    long long mem_bytes = -1;
    const char *override_text = getenv("ADAPTIVE_MEM_SDC_MEM_BUDGET_MB");
    if (override_text != nullptr) {
        errno = 0;
        char *end = nullptr;
        const long long mb = strtoll(override_text, &end, 10);
        if (override_text[0] != '\0' && end != override_text && *end == '\0' &&
            errno != ERANGE && mb > 0 && mb <= (1LL << 32)) {
            mem_bytes = mb * 1024LL * 1024LL;
        } else {
            fprintf(stderr,
                    "adaptive_mem_sdc: ADAPTIVE_MEM_SDC_MEM_BUDGET_MB='%s' is invalid, using MemAvailable\n",
                    override_text);
        }
    }

    if (mem_bytes < 0) {
        FILE *file = fopen("/proc/meminfo", "r");
        if (file != nullptr) {
            char line[128];
            long long available_kb = 0;
            while (fgets(line, sizeof(line), file) != nullptr) {
                if (sscanf(line, "MemAvailable: %lld", &available_kb) == 1) {
                    mem_bytes = available_kb * 1024LL;
                    break;
                }
            }
            fclose(file);
        }
    }
    if (mem_bytes < 0)
        mem_bytes = 1LL << 40;             /* same guard-off fallback as standard */
    return mem_bytes;
}

int stress_probe_count()
{
    bool explicit_block = false;
    const unsigned block_kb = static_cast<unsigned>(parse_bounded_env(
            "ADAPTIVE_MEM_SDC_BLOCK_KB", 64, 4, 8192, &explicit_block));

    int workers = total_thread_count();
    if (workers <= 0)
        workers = thread_count();
    if (workers <= 0)
        workers = 1;

    const long long mem_bytes = stress_memory_bytes();
    const unsigned long long budget = static_cast<unsigned long long>(
            mem_bytes - mem_bytes / 5);
    const unsigned long long per_worker_kb = 12ULL * 1024ULL;
    const unsigned long long per_worker_overhead =
            BUFFER_COUNT * (2ULL * kCanaryBytes + kMaxDataOffset + kTailBytes);
    auto need_bytes = [&](unsigned kb) {
        return static_cast<unsigned long long>(workers) *
               (per_worker_kb * kb + per_worker_overhead);
    };

    if (need_bytes(block_kb) <= budget)
        return static_cast<int>(block_kb * 64);

    if (explicit_block) {
        fprintf(stderr,
                "adaptive_mem_sdc: explicit stress BLOCK_KB=%u needs %llu bytes for %d workers, budget %llu; retaining operator choice\n",
                block_kb, need_bytes(block_kb), workers, budget);
        return static_cast<int>(block_kb * 64);
    }

    const unsigned long long total_overhead =
            static_cast<unsigned long long>(workers) * per_worker_overhead;
    unsigned long long fitting_kb = budget > total_overhead
            ? (budget - total_overhead) /
              (static_cast<unsigned long long>(workers) * per_worker_kb)
            : 0;
    fitting_kb &= ~3ULL;                 /* deterministic 4KB granularity */
    if (fitting_kb < 4) {
        fprintf(stderr,
                "adaptive_mem_sdc: stress 4KB needs %llu bytes for %d workers, budget %llu; no footprint fits\n",
                need_bytes(4), workers, budget);
        return -1;
    }
    if (fitting_kb > block_kb)
        fitting_kb = block_kb;
    const unsigned effective_kb = static_cast<unsigned>(fitting_kb);
    fprintf(stderr,
            "adaptive_mem_sdc: stress BLOCK_KB=%u exceeds budget %llu bytes for %d workers; downgraded to %uKB\n",
            block_kb, budget, workers, effective_kb);
    return static_cast<int>(effective_kb * 64);
}

Access effective_access(const Config &config, uint64_t sweep)
{
    if (config.access != Access::Mixed)
        return config.access;
    static const Access rotation[] = {
        Access::Aligned, Access::Unaligned, Access::Cacheline, Access::Random
    };
    return rotation[sweep % (sizeof(rotation) / sizeof(rotation[0]))];
}

size_t effective_grain(const Config &config, uint64_t sweep)
{
    if (config.grain != 0)
        return config.grain;
    static const size_t rotation[] = { 1, 2, 4, 8, 16, 32, 64 };
    return rotation[sweep % (sizeof(rotation) / sizeof(rotation[0]))];
}

size_t buffer_offset(const Config &config, size_t buffer, uint64_t sweep)
{
    if (config.access == Access::Mixed) {
        const size_t count = sizeof(kOffsets) / sizeof(kOffsets[0]);
        return kOffsets[(static_cast<size_t>(sweep) + buffer) % count];
    }

    switch (config.access) {
    case Access::Aligned:
        return 0;
    case Access::Unaligned:
        return kOffsets[1 + buffer % ((sizeof(kOffsets) / sizeof(kOffsets[0])) - 1)];
    case Access::Cacheline:
        return (buffer & 1) != 0 ? 56 : 63;
    case Access::Random:
        return kOffsets[buffer % (sizeof(kOffsets) / sizeof(kOffsets[0]))];
    case Access::Mixed:
        break;
    }
    return 0;
}

void check_canaries(const AdaptiveSdcLocation &location,
                    GuardedBuffer (&buffers)[BUFFER_COUNT], int &ok)
{
    for (size_t i = 0; i < BUFFER_COUNT; ++i) {
        char name[96];
        snprintf(name, sizeof(name), "%s.front_canary", kBufferNames[i]);
        ok &= adaptive_sdc_check_bytes(&location, name, buffers[i].front(),
                                       buffers[i].front_expected(), kCanaryBytes);
        snprintf(name, sizeof(name), "%s.front_padding", kBufferNames[i]);
        ok &= adaptive_sdc_check_bytes(&location, name, buffers[i].front_padding(),
                                       kZeroPadding, buffers[i].front_padding_size());
        snprintf(name, sizeof(name), "%s.back_padding", kBufferNames[i]);
        ok &= adaptive_sdc_check_bytes(&location, name, buffers[i].back_padding(),
                                       kZeroPadding, buffers[i].back_padding_size());
        snprintf(name, sizeof(name), "%s.back_canary", kBufferNames[i]);
        ok &= adaptive_sdc_check_bytes(&location, name, buffers[i].back(),
                                       buffers[i].back_expected(), kCanaryBytes);
    }
}

void check_sources(const AdaptiveSdcLocation &location,
                   GuardedBuffer (&buffers)[BUFFER_COUNT], int &ok)
{
    for (size_t i = 0; i < 3; ++i) {
        ok &= adaptive_sdc_check_bytes(&location, kBufferNames[SRC_A + i],
                                       buffers[SRC_A + i].data(),
                                       buffers[SRC_A_SHADOW + i].data(),
                                       buffers[SRC_A + i].size());
    }
}

void check_golden(const AdaptiveSdcLocation &location,
                  GuardedBuffer (&buffers)[BUFFER_COUNT], int &ok)
{
    ok &= adaptive_sdc_check_bytes(&location, "golden", buffers[GOLDEN].data(),
                                   buffers[GOLDEN_SHADOW].data(), buffers[GOLDEN].size());
}

void check_plan(const AdaptiveSdcLocation &location,
                GuardedBuffer (&buffers)[BUFFER_COUNT], int &ok)
{
    ok &= adaptive_sdc_check_bytes(&location, "plan", buffers[PLAN].data(),
                                   buffers[PLAN_SHADOW].data(), buffers[PLAN].size());
}

void check_integer_output(const AdaptiveSdcLocation &location, const char *name,
                          const GuardedBuffer &actual, const GuardedBuffer &golden,
                          const Recipe &recipe, int &ok)
{
    const int match = adaptive_sdc_check_bytes(&location, name, actual.data(),
                                               golden.data(), actual.size());
    ok &= match;
    if (match != 0)
        return;

    /* The full-block location uses 64-bit lanes.  Refine any byte mismatch
     * with the actual operation geometry, including the four-lane u32 slot. */
    for (size_t i = 0; i < actual.size() / kVectorBytes; ++i) {
        const size_t offset = i * kVectorBytes;
        if (memcmp(actual.data() + offset, golden.data() + offset, kVectorBytes) == 0)
            continue;
        AdaptiveSdcLocation lane_location = location;
        lane_location.site = "integer-output-vector";
        lane_location.lane_bytes = recipe_slot(recipe, i) == 3 ? 4 : 8;
        ok &= adaptive_sdc_check_bytes(&lane_location, name, actual.data() + offset,
                                       golden.data() + offset, kVectorBytes);
    }
}

void initialize_sources(GuardedBuffer (&buffers)[BUFFER_COUNT])
{
    const size_t bytes = buffers[SRC_A].size();
    for (size_t offset = 0; offset < bytes; offset += sizeof(uint64_t)) {
        const int64_t a = static_cast<int64_t>(random64() % 128) - 64;
        const int64_t b = static_cast<int64_t>(random64() % 128) - 64;
        const int64_t c = static_cast<int64_t>(random64() % 128) - 64;
        store_u64(buffers[SRC_A].data(), offset, static_cast<uint64_t>(a));
        store_u64(buffers[SRC_B].data(), offset, static_cast<uint64_t>(b));
        store_u64(buffers[SRC_C].data(), offset, static_cast<uint64_t>(c));
    }
    for (size_t i = 0; i < 3; ++i) {
        memcpy(buffers[SRC_A_SHADOW + i].data(), buffers[SRC_A + i].data(), bytes);
    }
}

void initialize_plan(GuardedBuffer (&buffers)[BUFFER_COUNT])
{
    const size_t entries = buffers[PLAN].size() / sizeof(uint64_t);
    for (size_t i = 0; i < entries; ++i)
        store_u64(buffers[PLAN].data(), i * sizeof(uint64_t), random64());
    memcpy(buffers[PLAN_SHADOW].data(), buffers[PLAN].data(), buffers[PLAN].size());
}

void __attribute__((noinline)) compute_integer_direct(
        GuardedBuffer (&buffers)[BUFFER_COUNT], GuardedBuffer &output,
        const Recipe &recipe)
{
    const size_t vectors = output.size() / kVectorBytes;
    for (size_t i = 0; i < vectors; ++i) {
        const size_t offset = i * kVectorBytes;
        const size_t slot = recipe_slot(recipe, i);
        if (slot == 3) {
            for (size_t lane = 0; lane < 4; ++lane) {
                const size_t lane_offset = offset + lane * sizeof(uint32_t);
                const uint32_t a = load_u32(buffers[SRC_A].data(), lane_offset);
                const uint32_t b = load_u32(buffers[SRC_B].data(), lane_offset);
                const uint32_t c = recipe.use_c
                        ? load_u32(buffers[SRC_C].data(), lane_offset) : b;
                store_u32(output.data(), lane_offset, a * b + c);
            }
            continue;
        }

        for (size_t lane = 0; lane < 2; ++lane) {
            const size_t lane_offset = offset + lane * sizeof(uint64_t);
            const uint64_t a = load_u64(buffers[SRC_A].data(), lane_offset);
            const uint64_t b = load_u64(buffers[SRC_B].data(), lane_offset);
            uint64_t result;
            if (slot == 0)
                result = a + b;
            else if (slot == 1)
                result = a ^ b;
            else
                result = a - b;
            store_u64(output.data(), lane_offset, result);
        }
    }
}

void __attribute__((noinline)) compute_integer_equivalent(
        GuardedBuffer (&buffers)[BUFFER_COUNT], GuardedBuffer &output,
        const Recipe &recipe)
{
    const size_t vectors = output.size() / kVectorBytes;
    for (size_t i = 0; i < vectors; ++i) {
        const size_t offset = i * kVectorBytes;
        const size_t slot = recipe_slot(recipe, i);
        if (slot == 3) {
            for (size_t lane = 0; lane < 4; ++lane) {
                const size_t lane_offset = offset + lane * sizeof(uint32_t);
                const uint32_t a = load_u32(buffers[SRC_A].data(), lane_offset);
                const uint32_t b = load_u32(buffers[SRC_B].data(), lane_offset);
                const uint32_t c = recipe.use_c
                        ? load_u32(buffers[SRC_C].data(), lane_offset) : b;
                uint32_t product = 0;
                for (unsigned shift = 0; shift < 32; ++shift) {
                    if (((b >> shift) & 1u) != 0)
                        product += a << shift;
                }
                store_u32(output.data(), lane_offset, product + c);
            }
            continue;
        }

        for (size_t lane = 0; lane < 2; ++lane) {
            const size_t lane_offset = offset + lane * sizeof(uint64_t);
            const uint64_t a = load_u64(buffers[SRC_A].data(), lane_offset);
            const uint64_t b = load_u64(buffers[SRC_B].data(), lane_offset);
            uint64_t result;
            if (slot == 0)
                result = a - (UINT64_C(0) - b);
            else if (slot == 1)
                result = (a | b) & ~(a & b);
            else
                result = a + ~b + UINT64_C(1);
            store_u64(output.data(), lane_offset, result);
        }
    }
}

void __attribute__((noinline)) compute_integer_neon(
        GuardedBuffer (&buffers)[BUFFER_COUNT], GuardedBuffer &output,
        const Recipe &recipe)
{
    const size_t vectors = output.size() / kVectorBytes;
    for (size_t i = 0; i < vectors; ++i) {
        const size_t offset = i * kVectorBytes;
        const size_t slot = recipe_slot(recipe, i);
        if (slot == 3) {
            const uint32x4_t a = neon_load_u32(buffers[SRC_A].data() + offset);
            const uint32x4_t b = neon_load_u32(buffers[SRC_B].data() + offset);
            const uint32x4_t c = recipe.use_c
                    ? neon_load_u32(buffers[SRC_C].data() + offset) : b;
            const uint32x4_t product = vmulq_u32(a, b);
            neon_store_u32(output.data() + offset, vaddq_u32(product, c));
            continue;
        }

        const uint64x2_t a = neon_load_u64(buffers[SRC_A].data() + offset);
        const uint64x2_t b = neon_load_u64(buffers[SRC_B].data() + offset);
        uint64x2_t result;
        if (slot == 0)
            result = vaddq_u64(a, b);
        else if (slot == 1)
            result = veorq_u64(a, b);
        else
            result = vsubq_u64(a, b);
        neon_store_u64(output.data() + offset, result);
    }
}

void __attribute__((noinline)) compute_fp_golden(
        GuardedBuffer (&buffers)[BUFFER_COUNT], const Recipe &recipe)
{
    const size_t vectors = buffers[GOLDEN].size() / kVectorBytes;
    for (size_t i = 0; i < vectors; ++i) {
        const size_t offset = i * kVectorBytes;
        const size_t slot = recipe_slot(recipe, i);
        for (size_t lane = 0; lane < 2; ++lane) {
            const size_t lane_offset = offset + lane * sizeof(uint64_t);
            const int64_t a = bits_to_i64(load_u64(buffers[SRC_A].data(), lane_offset));
            const int64_t b = bits_to_i64(load_u64(buffers[SRC_B].data(), lane_offset));
            const int64_t c = recipe.use_c && slot == 3
                    ? bits_to_i64(load_u64(buffers[SRC_C].data(), lane_offset)) : 0;
            int64_t fixed;
            if (slot == 0)
                fixed = a + b;
            else if (slot == 1)
                fixed = a - b;
            else if (slot == 2)
                fixed = a * b;
            else
                fixed = a + b + (recipe.use_c ? c : b);
            const double expected = static_cast<double>(fixed);
            store_double(buffers[GOLDEN].data(), lane_offset, expected);
        }
    }
}

void __attribute__((noinline, optimize("no-tree-vectorize"))) compute_fp_scalar(
        GuardedBuffer (&buffers)[BUFFER_COUNT], GuardedBuffer &output,
        const Recipe &recipe)
{
    const size_t vectors = output.size() / kVectorBytes;
    for (size_t i = 0; i < vectors; ++i) {
        const size_t offset = i * kVectorBytes;
        const size_t slot = recipe_slot(recipe, i);
        for (size_t lane = 0; lane < 2; ++lane) {
            const size_t lane_offset = offset + lane * sizeof(uint64_t);
            const double a = static_cast<double>(bits_to_i64(
                    load_u64(buffers[SRC_A].data(), lane_offset)));
            const double b = static_cast<double>(bits_to_i64(
                    load_u64(buffers[SRC_B].data(), lane_offset)));
            const double c = recipe.use_c && slot == 3
                    ? static_cast<double>(bits_to_i64(load_u64(
                            buffers[SRC_C].data(), lane_offset))) : 0.0;
            double result;
            if (slot == 0)
                result = a + b;
            else if (slot == 1)
                result = a - b;
            else if (slot == 2)
                result = a * b;
            else {
                const double sum = a + b;
                result = sum + (recipe.use_c ? c : b);
            }
            /* Fixed-point zero converts to +0.0; FP multiplication can
             * instead produce -0.0 for 0 × negative. */
            if (result == 0.0)
                result = 0.0;
            store_double(output.data(), lane_offset, result);
        }
    }
}

void __attribute__((noinline)) compute_fp_neon(
        GuardedBuffer (&buffers)[BUFFER_COUNT], GuardedBuffer &output,
        const Recipe &recipe)
{
    const size_t vectors = output.size() / kVectorBytes;
    for (size_t i = 0; i < vectors; ++i) {
        const size_t offset = i * kVectorBytes;
        const size_t slot = recipe_slot(recipe, i);
        const int64x2_t ai = vreinterpretq_s64_u64(
                neon_load_u64(buffers[SRC_A].data() + offset));
        const int64x2_t bi = vreinterpretq_s64_u64(
                neon_load_u64(buffers[SRC_B].data() + offset));
        const float64x2_t a = vcvtq_f64_s64(ai);
        const float64x2_t b = vcvtq_f64_s64(bi);
        const float64x2_t c = recipe.use_c && slot == 3
                ? vcvtq_f64_s64(vreinterpretq_s64_u64(
                        neon_load_u64(buffers[SRC_C].data() + offset))) : b;
        float64x2_t result;
        if (slot == 0)
            result = vaddq_f64(a, b);
        else if (slot == 1)
            result = vsubq_f64(a, b);
        else if (slot == 2)
            result = vmulq_f64(a, b);
        else
            result = vaddq_f64(vaddq_f64(a, b), recipe.use_c ? c : b);
        const float64x2_t zero = vdupq_n_f64(0.0);
        result = vbslq_f64(vceqq_f64(result, zero), zero, result);
        neon_store_u64(output.data() + offset, vreinterpretq_u64_f64(result));
    }
}

size_t chunk_offset(Access access, size_t grain, uint64_t random_index,
                    size_t block_bytes, uintptr_t primary_address)
{
    const size_t index = static_cast<size_t>(random_index) % block_bytes;
    if (access == Access::Aligned)
        return index & ~(grain - 1);

    if (access == Access::Cacheline && grain > 1) {
        /* Choose the actual virtual-address phase, not merely a block-relative
         * offset.  Every result is congruent to a crossing phase modulo 64 and
         * the line selection is explicitly capped at block_bytes-grain. */
        const size_t within = 64 - grain + 1 + (index / grain) % (grain - 1);
        const size_t base_phase = static_cast<size_t>(primary_address & 63);
        const size_t congruence = (within + 64 - base_phase) & 63;
        const size_t limit = block_bytes - grain;
        const size_t line_count = (limit - congruence) / 64 + 1;
        const size_t line = (index / 64) % line_count;
        return congruence + line * 64;
    }
    /* cacheline/grain=1 is intentionally just an ordinary random byte access. */
    return index;
}

void execute_random_access(const Config &config, const Recipe &recipe,
                           Access access, size_t grain,
                           GuardedBuffer (&buffers)[BUFFER_COUNT], int combo,
                           int cpu, uint64_t sweep, int &ok)
{
    const size_t block_bytes = config.block_bytes;
    const size_t entries = buffers[PLAN].size() / sizeof(uint64_t);
    memcpy(buffers[PRIMARY].data(), buffers[SRC_A].data(), block_bytes);
    memcpy(buffers[ALTERNATE].data(), buffers[SRC_A].data(), block_bytes);

    AdaptiveSdcLocation location = make_location("random-plan-pre", combo, cpu, sweep);
    check_plan(location, buffers, ok);

    for (unsigned round = 0; round < config.rounds; ++round) {
        location.site = "random-canary-pre";
        check_canaries(location, buffers, ok);

        /* str=2/3/4 maps to one/two/three copy-and-immediate-check passes. */
        for (unsigned repeat = 0; repeat < recipe.random_repeats; ++repeat) {
            for (size_t i = 0; i < entries; ++i) {
                const uint64_t raw = load_u64(buffers[PLAN].data(),
                                              i * sizeof(uint64_t));
                const size_t offset = chunk_offset(access, grain, raw, block_bytes,
                        reinterpret_cast<uintptr_t>(buffers[PRIMARY].data()));
                const size_t remaining = block_bytes - offset;
                const size_t bytes = grain < remaining ? grain : remaining;

                memcpy(buffers[PRIMARY].data() + offset,
                       buffers[SRC_A].data() + offset, bytes);
                const int primary_match = memcmp(buffers[PRIMARY].data() + offset,
                        buffers[SRC_A].data() + offset, bytes) == 0;
                ok &= primary_match;
                if (primary_match == 0) {
                    location.site = "random-primary-chunk";
                    ok &= adaptive_sdc_check_bytes(&location, "primary.chunk",
                            buffers[PRIMARY].data() + offset,
                            buffers[SRC_A].data() + offset, bytes);
                }

                memcpy(buffers[ALTERNATE].data() + offset,
                       buffers[SRC_A].data() + offset, bytes);
                const int alternate_match = memcmp(buffers[ALTERNATE].data() + offset,
                        buffers[SRC_A].data() + offset, bytes) == 0;
                ok &= alternate_match;
                if (alternate_match == 0) {
                    location.site = "random-alternate-chunk";
                    ok &= adaptive_sdc_check_bytes(&location, "alternate.chunk",
                            buffers[ALTERNATE].data() + offset,
                            buffers[SRC_A].data() + offset, bytes);
                }
            }
        }

        location.site = "random-canary-post";
        check_canaries(location, buffers, ok);
    }
}

uint8_t *injection_address(Injection &injection,
                           GuardedBuffer (&buffers)[BUFFER_COUNT], size_t &bytes)
{
    switch (injection.target) {
    case InjectTarget::Source:
        bytes = buffers[SRC_A].size();
        return buffers[SRC_A].data();
    case InjectTarget::Golden:
        bytes = buffers[GOLDEN].size();
        return buffers[GOLDEN].data();
    case InjectTarget::Output:
        bytes = buffers[PRIMARY].size();
        return buffers[PRIMARY].data();
    case InjectTarget::Replica:
        bytes = buffers[GOLDEN_SHADOW].size();
        return buffers[GOLDEN_SHADOW].data();
    case InjectTarget::Canary:
        bytes = kCanaryBytes;
        return buffers[SRC_A].front();
    case InjectTarget::Plan:
        bytes = buffers[PLAN].size();
        return buffers[PLAN].data();
    case InjectTarget::None:
        break;
    }
    bytes = 0;
    return nullptr;
}

void validate_injection(Injection &injection, GuardedBuffer (&buffers)[BUFFER_COUNT])
{
    if (injection.target == InjectTarget::None)
        return;
    size_t bytes = 0;
    uint8_t *address = injection_address(injection, buffers, bytes);
    if (address == nullptr || injection.byte >= bytes) {
        fprintf(stderr,
                "adaptive_mem_sdc: injection byte %zu is outside target %s (%zu bytes), disabling injection\n",
                injection.byte, inject_target_name(injection.target), bytes);
        injection.target = InjectTarget::None;
    }
}

void maybe_inject(Injection &injection, GuardedBuffer (&buffers)[BUFFER_COUNT],
                  int combo, int cpu, uint64_t sweep)
{
    if (injection.target == InjectTarget::None || injection.done ||
        injection.sweep != sweep)
        return;

    size_t bytes = 0;
    uint8_t *address = injection_address(injection, buffers, bytes);
    if (address == nullptr || injection.byte >= bytes)
        return;                         /* validated before entering TEST_LOOP */

    uint8_t *byte = address + injection.byte;
    const uint8_t before = *byte;
    const uint8_t after = static_cast<uint8_t>(before ^ (1u << injection.bit));
    *byte = after;
    injection.done = true;

    AdaptiveSdcLocation location = make_location("deterministic-injection",
                                                 combo, cpu, sweep);
    adaptive_sdc_record_injection(&location, inject_target_name(injection.target),
                                  byte, injection.byte, injection.bit, before, after);
}

} // namespace

extern "C" int adaptive_stress_probe_count(void)
{
    return stress_probe_count();
}

extern "C" int adaptive_stress_run_combo(struct test *test, int cpu, int combo)
{
    if (combo < 0 || combo >= ADAPT_COMBO_COUNT)
        return EXIT_SUCCESS;

    auto *data = static_cast<AdaptiveMemSdcData *>(test->data);
    const Config config = parse_config(data->count);
    const Recipe recipe = make_recipe(combo);
    Injection injection = parse_injection();
    const size_t plan_bytes = config.block_bytes / 2;
    GuardedBuffer buffers[BUFFER_COUNT];

    for (size_t i = 0; i < BUFFER_COUNT; ++i) {
        const size_t bytes = i == PLAN || i == PLAN_SHADOW ? plan_bytes
                                                           : config.block_bytes;
        if (!buffers[i].allocate(bytes)) {
            for (size_t j = 0; j < BUFFER_COUNT; ++j)
                buffers[j].reset();
            report_fail_msg(
                    "adaptive_mem_sdc profile=%s combo#%d access=%s grain=%s block=%uKB allocation failed for %s (%zu bytes)",
                    config.soak ? "soak" : "stress", combo,
                    access_name(config.access), grain_name(config.grain),
                    config.block_kb, kBufferNames[i], bytes);
        }
    }

    for (size_t i = 0; i < BUFFER_COUNT; ++i)
        buffers[i].set_offset(buffer_offset(config, i, 0));
    validate_injection(injection, buffers);

    uint64_t sweep = 0;
    TEST_LOOP(test, 1) {
        int ok = 1;
        for (size_t i = 0; i < BUFFER_COUNT; ++i)
            buffers[i].set_offset(buffer_offset(config, i, sweep));

        initialize_sources(buffers);
        initialize_plan(buffers);

        /* Freeze one independent golden snapshot per sweep.  Recomputing it
         * inside every redundant round would let the same-path fault refresh
         * both the expected value and its replica. */
        compute_integer_direct(buffers, buffers[GOLDEN], recipe);
        memcpy(buffers[GOLDEN_SHADOW].data(), buffers[GOLDEN].data(),
               config.block_bytes);

        for (unsigned round = 0; round < config.rounds; ++round) {
            AdaptiveSdcLocation location = make_location("integer-canary-pre",
                                                         combo, cpu, sweep);
            check_canaries(location, buffers, ok);
            location.site = "integer-input-pre";
            check_sources(location, buffers, ok);

            for (unsigned repeat = 0; repeat < recipe.integer_repeats; ++repeat) {
                compute_integer_direct(buffers, buffers[PRIMARY], recipe);
                compute_integer_equivalent(buffers, buffers[ALTERNATE], recipe);
                compute_integer_neon(buffers, buffers[VECTOR], recipe);
            }

            location.site = "integer-canary-post";
            check_canaries(location, buffers, ok);
            location.site = "integer-input-post";
            check_sources(location, buffers, ok);
            location.site = "integer-golden";
            check_golden(location, buffers, ok);
            location.site = "integer-primary";
            check_integer_output(location, "primary", buffers[PRIMARY],
                                 buffers[GOLDEN], recipe, ok);
            location.site = "integer-alternate";
            check_integer_output(location, "alternate", buffers[ALTERNATE],
                                 buffers[GOLDEN], recipe, ok);
            location.site = "integer-neon";
            check_integer_output(location, "vector", buffers[VECTOR],
                                 buffers[GOLDEN], recipe, ok);
        }

        /* Integer outputs are fully checked before their buffers are reused. */
        AdaptiveSdcLocation location = make_location("fp-canary-pre", combo, cpu, sweep);
        check_canaries(location, buffers, ok);
        location.site = "fp-input";
        check_sources(location, buffers, ok);
        compute_fp_golden(buffers, recipe);
        memcpy(buffers[GOLDEN_SHADOW].data(), buffers[GOLDEN].data(), config.block_bytes);
        for (unsigned repeat = 0; repeat < recipe.fp_repeats; ++repeat) {
            compute_fp_scalar(buffers, buffers[PRIMARY], recipe);
            compute_fp_neon(buffers, buffers[VECTOR], recipe);
        }
        location.site = "fp-canary-post";
        check_canaries(location, buffers, ok);
        location.site = "fp-golden";
        check_golden(location, buffers, ok);
        location.site = "fp-scalar";
        ok &= adaptive_sdc_check_bytes(&location, "primary", buffers[PRIMARY].data(),
                                       buffers[GOLDEN].data(), config.block_bytes);
        location.site = "fp-neon";
        ok &= adaptive_sdc_check_bytes(&location, "vector", buffers[VECTOR].data(),
                                       buffers[GOLDEN].data(), config.block_bytes);

        const Access access = effective_access(config, sweep);
        const size_t grain = effective_grain(config, sweep);
        execute_random_access(config, recipe, access, grain, buffers,
                              combo, cpu, sweep, ok);

        /* All computation is complete.  The selected sweep gets exactly one
         * bit flip before the final source/replica/output verification. */
        maybe_inject(injection, buffers, combo, cpu, sweep);

        location = make_location("final-canary", combo, cpu, sweep);
        check_canaries(location, buffers, ok);
        location.site = "final-source";
        check_sources(location, buffers, ok);
        location.site = "final-golden";
        check_golden(location, buffers, ok);
        location.site = "final-plan";
        check_plan(location, buffers, ok);
        location.site = "final-random-primary";
        ok &= adaptive_sdc_check_bytes(&location, "primary", buffers[PRIMARY].data(),
                                       buffers[SRC_A_SHADOW].data(), config.block_bytes);
        location.site = "final-random-alternate";
        ok &= adaptive_sdc_check_bytes(&location, "alternate", buffers[ALTERNATE].data(),
                                       buffers[SRC_A_SHADOW].data(), config.block_bytes);
        location.site = "final-fp-neon";
        ok &= adaptive_sdc_check_bytes(&location, "vector", buffers[VECTOR].data(),
                                       buffers[GOLDEN].data(), config.block_bytes);

        if (ok == 0) {
            if (data != nullptr && data->shm != nullptr)
                __atomic_store_n(&data->shm->sdc_flag, 1u, __ATOMIC_RELEASE);
            report_fail_msg(
                    "adaptive_mem_sdc profile=%s combo#%d access=%s grain=%s block=%uKB data miscompare",
                    config.soak ? "soak" : "stress", combo,
                    access_name(config.access), grain_name(config.grain), config.block_kb);
        }
        ++sweep;
    }

    return EXIT_SUCCESS;
}

#else  /* !__aarch64__ */

extern "C" int adaptive_stress_probe_count(void)
{
    return 64 * 64;
}

extern "C" int adaptive_stress_run_combo(struct test *test, int cpu, int combo)
{
    (void)test;
    (void)cpu;
    (void)combo;
    return EXIT_SUCCESS;
}

#endif
