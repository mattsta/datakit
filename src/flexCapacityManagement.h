#pragma once

#include "../deps/varint/src/varint.h" /* provide varint limits */
#include "flex.h"

/* Optimization levels for size-based filling */
static const uint32_t flexOptimizationSizeLimit[] = {
    0, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
static const size_t flexOptimizationSizeLimits =
    sizeof(flexOptimizationSizeLimit) / sizeof(*flexOptimizationSizeLimit);

typedef enum flexCapSizeLimit {
    FLEX_CAP_LEVEL_0 = 0,
    FLEX_CAP_LEVEL_64 = 1,
    FLEX_CAP_LEVEL_128 = 2,
    FLEX_CAP_LEVEL_256 = 3,
    FLEX_CAP_LEVEL_512 = 4,
    FLEX_CAP_LEVEL_1024 = 5,
    FLEX_CAP_LEVEL_2048 = 6,
    FLEX_CAP_LEVEL_4096 = 7,
    FLEX_CAP_LEVEL_8192 = 8,
    FLEX_CAP_LEVEL_16384 = 9,
    FLEX_CAP_LEVEL_32768 = 10,
    FLEX_CAP_LEVEL_65536 = 11
} flexCapSizeLimit;

DK_STATIC inline bool
flexCapSizeMeetsOptimizationRequirement(const size_t bytes, int fill) {
    return bytes <= flexOptimizationSizeLimit[fill];
}

DK_STATIC inline bool flexCapAllowInsert(const size_t bytes, const int fill,
                                         const size_t requestingBytes) {
    /* always allow insert if empty */
    if (bytes == FLEX_EMPTY_SIZE) {
        return true;
    }

    /* else, check if new bytes + encoding for new bytes is under size limit. */
    int flexEncodingOverhead;
    if (requestingBytes <= VARINT_SPLIT_FULL_NO_ZERO_STORAGE_1) {
        flexEncodingOverhead = 1;
    } else {
        /* else, estimate usable sizes having two bytes of overhead.
         * it's okay if this is inaccurate for lager 'requestingBytes'
         * values because those would spill into a new node anyway. */
        flexEncodingOverhead = 2;
    }

    /* flex encodings are the same forwards and backwards */
    flexEncodingOverhead *= 2;

    /* newBytes overestimates if 'bytes' encodes to an integer type
     * because 'bytes' could be reduced from a (up to 32 byte string)
     * down to a 1 to 8 byte integer. */
    const size_t newBytes = bytes + requestingBytes + flexEncodingOverhead;
    return flexCapSizeMeetsOptimizationRequirement(newBytes, fill);
}

DK_STATIC inline bool flexCapIsMergeable(const uint32_t aBytes,
                                         const uint32_t bBytes,
                                         const int fill) {
    /* approximate merged flex size (-3 to remove one estimated flex
     * header */
    const size_t mergeBytes = aBytes + bBytes - 3;
    return flexCapSizeMeetsOptimizationRequirement(mergeBytes, fill);
}
