/*
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sandstone_p.h"
#include "cpu_device.h"
#include <device/cpu/arm64/arm64_vl.h>

#include <cinttypes>

extern constexpr const device_features_t minimum_cpu_features = device_compiler_features;

// The feature-detection tables are generated per-arch with arch-native
// symbol names (arm64_locators / arm64_architectures on AArch64,
// x86_locators / x86_architectures on x86-64), so the two contracts are
// fully decoupled: an x86-side rename can neither drop ARM64 features nor
// collide with them.  Select the current arch's contract here; the table
// types are layout-compatible (both expose array length; both architecture
// entries expose {features, name}).
#if defined(__aarch64__)
#  define CPU_LOCATORS       arm64_locators
#  define CPU_ARCHITECTURES  arm64_architectures
#else
#  define CPU_LOCATORS       x86_locators
#  define CPU_ARCHITECTURES  x86_architectures
#endif

std::string device_features_to_string(device_features_t f)
{
    std::string result;
    const char *comma = "";
    for (size_t i = 0; i < std::size(CPU_LOCATORS); ++i) {
        if (f & CPU_FEATURE_CONSTANT(i)) {
            result += comma;
            result += features_string + features_indices[i] + 1;
            comma = ",";
        }
    }
    return result;
}

void dump_device_info()
{
    int i;

    // find the best matching CPU
    const char *detected = "<unknown>";
    for (const auto &arch : CPU_ARCHITECTURES) {
        if ((arch.features & device_features) == arch.features) {
            detected = arch.name;
            break;
        }
        if (sApp->shmem->cfg.verbosity > 1)
            printf("CPU is not %s: missing %s\n", arch.name,
                   device_features_to_string(arch.features & ~device_features).c_str());
    }
    printf("Detected CPU: %s; family-model-stepping (hex): %02x-%02x-%02x; CPU features: %s\n",
           detected, sApp->hwinfo.family, sApp->hwinfo.model, sApp->hwinfo.stepping,
           device_features_to_string(device_features).c_str());
#if defined(__aarch64__) && defined(__linux__)
    /* Per-thread vector lengths, distinct from the process-wide feature
       bits above; see docs/architecture.md "SVE/SME vector-length layer".
       Linux-only: on Windows ARM64 the arm64_vl.h stubs always report
       absence, and printing "SVE not present" there would assert a hardware
       fact this OS cannot probe — print nothing rather than a falsehood.
       Query once and reuse: VL is a per-thread runtime quantity, so calling
       the probe twice per line could even self-contradict if it changed. */
    if (arm64_cpu_has_sve()) {
        long vl = arm64_sve_vl_bytes();
        printf("SVE vector length: %ld bits (%ld bytes)\n", vl * 8, vl);
    } else {
        printf("SVE vector length: n/a (SVE not present)\n");
    }
    if (arm64_cpu_has_sme()) {
        long svl = arm64_sme_svl_bytes();
        printf("SME streaming vector length: %ld bits (%ld bytes)\n", svl * 8, svl);
    } else {
        printf("SME streaming vector length: n/a (SME not present)\n");
    }
#endif
    printf("# CPU\tPkgID\tCoreID\tThrdID\tModId\tDieId\tNUMAId\tApicId\tMicrocode\tPPIN\n");
    for (i = 0; i < device_count(); ++i) {
        printf("%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t0x%" PRIx64, device_info[i].cpu_number,
               device_info[i].package_id, device_info[i].core_id, device_info[i].thread_id,
               device_info[i].module_id, device_info[i].die_id, device_info[i].numa_id, device_info[i].hwid,
               device_info[i].microcode);
        const HardwareInfo::PackageInfo *pkg = sApp->hwinfo.find_package_id(device_info[i].package_id);
        if (pkg && pkg->ppin)
            printf("\t%016" PRIx64, pkg->ppin);
        puts("");
    }
}

TestResult prepare_test_for_device(struct test *test)
{
    auto has_smt = []() -> bool {
        for(int idx = 0; idx < device_count() - 1; idx++) {
            if (device_info[idx].package_id == device_info[idx + 1].package_id &&
                device_info[idx].core_id == device_info[idx + 1].core_id)
                return true;
        }
        return false;
    };

    if (test->flags & test_requires_smt) {
        if (!Topology::topology().isValid()) {
            log_skip(CpuTopologyIssueSkipCategory, "Test requires topology information");
            return TestResult::Skipped;
        }
        if (!has_smt()) {
            log_skip(CpuTopologyIssueSkipCategory, "Test requires SMT (hyperthreading)");
            return TestResult::Skipped;
        }
    }
    return TestResult::Passed;
}

void finish_test_for_device(struct test *test)
{}

// Currently empty
std::vector<struct test*> special_tests_for_device()
{
    return {};
}
