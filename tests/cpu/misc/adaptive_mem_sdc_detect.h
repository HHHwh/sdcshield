/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ADAPTIVE_MEM_SDC_DETECT_H
#define ADAPTIVE_MEM_SDC_DETECT_H

#include <stddef.h>
#include <stdint.h>

#define ADAPTIVE_SDC_CHECK_DIGESTS 1u

/* vector_bytes is the total bytes per vector and lane_bytes the bytes per
 * lane.  Zero disables the corresponding vector/lane annotation.  Callers
 * normally describe at most 32 lanes; larger or invalid layouts cannot index
 * the fixed histogram and are accounted separately as overflow/unmapped. */
struct AdaptiveSdcLocation {
    const char *engine;
    const char *site;
    int combo;
    int cpu;
    uint64_t sweep;
    size_t vector_bytes;
    size_t lane_bytes;
    unsigned flags;
};

/* A null pointer with nonzero size has no digest and returns zero.  Null is
 * valid for an empty buffer, producing the standard empty-input digest. */
extern "C" uint32_t adaptive_sdc_crc32(const void *data,size_t size);
extern "C" uint64_t adaptive_sdc_fnv1a64(const void *data,size_t size);

/* Return 1 for a byte-exact match (and matching digests when requested), 0
 * for a mismatch or invalid nonempty input.  The default successful path is
 * only memcmp plus the flag test; diagnostics are entirely cold-path. */
extern "C" int adaptive_sdc_check_bytes(const AdaptiveSdcLocation *loc,const char *buffer_name,const void *actual,const void *expected,size_t bytes);

/* Record an intentional injection, explicitly distinct from an observed SDC. */
extern "C" void adaptive_sdc_record_injection(const AdaptiveSdcLocation *loc,const char *target,const void *address,size_t byte_offset,unsigned bit,uint8_t before,uint8_t after);

#endif /* ADAPTIVE_MEM_SDC_DETECT_H */
