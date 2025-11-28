#include "varintFOR.h"
#include <assert.h>
#include <string.h>

/* ====================================================================
 * SIMD Platform Detection and Includes
 * ==================================================================== */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define VARINT_FOR_NEON 1
#include <arm_neon.h>
#endif

#if defined(__AVX2__)
#define VARINT_FOR_AVX2 1
#include <immintrin.h>
#elif defined(__SSE4_1__)
#define VARINT_FOR_SSE4 1
#include <smmintrin.h>
#elif defined(__SSE2__)
#define VARINT_FOR_SSE2 1
#include <emmintrin.h>
#endif

/* Compute optimal offset width for a given range */
varintWidth varintFORComputeWidth(const uint64_t range) {
    varintWidth width;
    varintExternalUnsignedEncoding(range, width);
    return width;
}

/* Analyze array to find min, max, range, and optimal width */
void varintFORAnalyze(const uint64_t *values, const size_t count,
                      varintFORMeta *meta) {
    assert(count > 0);
    assert(values != NULL);
    assert(meta != NULL);

    uint64_t minVal = values[0];
    uint64_t maxVal = values[0];

    /* Find min and max in one pass */
    for (size_t i = 1; i < count; i++) {
        if (values[i] < minVal) {
            minVal = values[i];
        }
        if (values[i] > maxVal) {
            maxVal = values[i];
        }
    }

    /* Compute range and optimal offset width */
    uint64_t range = maxVal - minVal;
    varintWidth offsetWidth = varintFORComputeWidth(range);

    /* Fill metadata */
    meta->minValue = minVal;
    meta->maxValue = maxVal;
    meta->range = range;
    meta->offsetWidth = offsetWidth;
    meta->count = count;
    meta->encodedSize = varintFORSize(meta);
}

/* Calculate encoded size: min_value + offset_width + count + (count *
 * offset_width) */
size_t varintFORSize(const varintFORMeta *meta) {
    /* Use tagged varints for self-describing header */
    varintWidth minWidth = varintTaggedLen(meta->minValue);
    varintWidth countWidth = varintTaggedLen(meta->count);

    /* Header: min_value + offset_width (1 byte) + count + offsets */
    return minWidth + 1 + countWidth + (meta->count * meta->offsetWidth);
}

/* Encode array using Frame-of-Reference */
size_t varintFOREncode(uint8_t *dst, const uint64_t *values, const size_t count,
                       varintFORMeta *meta) {
    assert(dst != NULL);
    assert(values != NULL);
    assert(count > 0);

    /* Local metadata storage - must be at function scope */
    varintFORMeta localMeta;

    /* Analyze if not already done */
    if (meta == NULL || meta->count != count) {
        varintFORAnalyze(values, count, &localMeta);
        if (meta != NULL) {
            *meta = localMeta;
        }
        meta = &localMeta;
    }

    uint8_t *ptr = dst;

    /* Encode min value using tagged varint (self-describing) */
    varintWidth minWidth = varintTaggedPut64(ptr, meta->minValue);
    ptr += minWidth;

    /* Encode offset width (1 byte) */
    *ptr = (uint8_t)meta->offsetWidth;
    ptr++;

    /* Encode count using tagged varint (self-describing) */
    varintWidth countWidth = varintTaggedPut64(ptr, meta->count);
    ptr += countWidth;

    /* Encode all offsets at fixed width */
    for (size_t i = 0; i < count; i++) {
        uint64_t offset = values[i] - meta->minValue;
        varintExternalPutFixedWidthQuick_(ptr, offset, meta->offsetWidth);
        ptr += meta->offsetWidth;
    }

    return (size_t)(ptr - dst);
}

/* Read metadata from encoded FOR data */
void varintFORReadMetadata(const uint8_t *src, varintFORMeta *meta) {
    assert(src != NULL);
    assert(meta != NULL);

    const uint8_t *ptr = src;

    /* Decode min value using tagged varint (self-describing) */
    uint64_t minValue;
    varintWidth minWidth = varintTaggedGet64(ptr, &minValue);
    ptr += minWidth;

    /* Decode offset width (1 byte) */
    varintWidth offsetWidth = (varintWidth)(*ptr);
    ptr++;

    /* Decode count using tagged varint (self-describing) */
    uint64_t count;
    varintWidth countWidth = varintTaggedGet64(ptr, &count);

    /* Fill metadata */
    meta->minValue = minValue;
    meta->count = (size_t)count;
    meta->offsetWidth = offsetWidth;

    /* Range and max will be computed if needed */
    meta->range = 0;
    meta->maxValue = minValue;
    meta->encodedSize =
        minWidth + 1 + countWidth + ((size_t)count * offsetWidth);
}

/* Decode entire FOR-encoded array */
size_t varintFORDecode(const uint8_t *src, uint64_t *values,
                       const size_t maxCount) {
    assert(src != NULL);
    assert(values != NULL);

    varintFORMeta meta;
    varintFORReadMetadata(src, &meta);

    if (meta.count > maxCount) {
        /* Not enough space in output buffer */
        return 0;
    }

    /* Calculate offset to data section using tagged varint lengths */
    varintWidth minWidth = varintTaggedLen(meta.minValue);
    varintWidth countWidth = varintTaggedLen(meta.count);

    const uint8_t *dataPtr = src + minWidth + 1 + countWidth;

    /* Decode all offsets and add back min value */
    for (size_t i = 0; i < meta.count; i++) {
        uint64_t offset;
        varintExternalGetQuick_(dataPtr, meta.offsetWidth, offset);
        values[i] = meta.minValue + offset;
        dataPtr += meta.offsetWidth;
    }

    return meta.count;
}

/* Random access to specific index */
uint64_t varintFORGetAt(const uint8_t *src, const size_t index) {
    assert(src != NULL);

    varintFORMeta meta;
    varintFORReadMetadata(src, &meta);

    assert(index < meta.count);

    /* Calculate offset to requested element using tagged varint lengths */
    varintWidth minWidth = varintTaggedLen(meta.minValue);
    varintWidth countWidth = varintTaggedLen(meta.count);

    const uint8_t *dataPtr =
        src + minWidth + 1 + countWidth + (index * meta.offsetWidth);

    /* Decode offset and add min value */
    uint64_t offset;
    varintExternalGetQuick_(dataPtr, meta.offsetWidth, offset);
    return meta.minValue + offset;
}

/* Get minimum value from encoded data */
uint64_t varintFORGetMinValue(const uint8_t *src) {
    assert(src != NULL);

    uint64_t minValue;
    varintTaggedGet64(src, &minValue);
    return minValue;
}

/* Get count from encoded data */
size_t varintFORGetCount(const uint8_t *src) {
    assert(src != NULL);

    /* Skip min value (tagged varint) */
    uint64_t minValue;
    varintWidth minWidth = varintTaggedGet64(src, &minValue);

    /* Skip offset width byte */
    const uint8_t *countPtr = src + minWidth + 1;

    /* Decode count (tagged varint) */
    uint64_t count;
    varintTaggedGet64(countPtr, &count);
    return (size_t)count;
}

/* Get offset width from encoded data */
varintWidth varintFORGetOffsetWidth(const uint8_t *src) {
    assert(src != NULL);

    /* Skip min value (tagged varint) to get to offset width byte */
    uint64_t minValue;
    varintWidth minWidth = varintTaggedGet64(src, &minValue);

    return (varintWidth)src[minWidth];
}

/* ====================================================================
 * SIMD-Accelerated Batch Operations
 * ==================================================================== */

/* Runtime SIMD availability check */
bool varintFORHasSIMD(void) {
#if defined(VARINT_FOR_NEON) || defined(VARINT_FOR_AVX2) ||                    \
    defined(VARINT_FOR_SSE4) || defined(VARINT_FOR_SSE2)
    return true;
#else
    return false;
#endif
}

/* Minimum count to benefit from SIMD (overhead vs scalar) */
#define VARINT_FOR_SIMD_MIN_COUNT 16

#ifdef VARINT_FOR_NEON
/* ====================================================================
 * ARM NEON SIMD Implementation
 * ==================================================================== */

/* NEON pairwise min/max for uint64x2 - manual implementation
 * since vminq_u64/vmaxq_u64 require ARMv8.1-A or later */
static inline uint64x2_t neon_min_u64(uint64x2_t a, uint64x2_t b) {
    /* vcgtq_u64 returns all 1s where a > b, all 0s otherwise */
    uint64x2_t mask = vcgtq_u64(a, b);
    /* Select b where a > b, otherwise select a */
    return vbslq_u64(mask, b, a);
}

static inline uint64x2_t neon_max_u64(uint64x2_t a, uint64x2_t b) {
    /* vcgtq_u64 returns all 1s where a > b, all 0s otherwise */
    uint64x2_t mask = vcgtq_u64(a, b);
    /* Select a where a > b, otherwise select b */
    return vbslq_u64(mask, a, b);
}

/* NEON min/max horizontal reduction for uint64x2 */
static inline uint64_t neon_hmin_u64(uint64x2_t v) {
    return vgetq_lane_u64(v, 0) < vgetq_lane_u64(v, 1) ? vgetq_lane_u64(v, 0)
                                                       : vgetq_lane_u64(v, 1);
}

static inline uint64_t neon_hmax_u64(uint64x2_t v) {
    return vgetq_lane_u64(v, 0) > vgetq_lane_u64(v, 1) ? vgetq_lane_u64(v, 0)
                                                       : vgetq_lane_u64(v, 1);
}

/* NEON-accelerated min/max analysis */
static void varintFORBatchAnalyzeNEON(const uint64_t *values,
                                      const size_t count, varintFORMeta *meta) {
    assert(count > 0);

    /* Process 2 uint64s at a time with NEON */
    size_t i = 0;
    uint64x2_t minVec = vdupq_n_u64(UINT64_MAX);
    uint64x2_t maxVec = vdupq_n_u64(0);

    /* Main SIMD loop - 2 elements per iteration */
    const size_t simdCount = (count / 2) * 2;
    for (; i < simdCount; i += 2) {
        uint64x2_t vals = vld1q_u64(&values[i]);
        minVec = neon_min_u64(minVec, vals);
        maxVec = neon_max_u64(maxVec, vals);
    }

    /* Horizontal reduction */
    uint64_t minVal = neon_hmin_u64(minVec);
    uint64_t maxVal = neon_hmax_u64(maxVec);

    /* Handle remaining elements */
    for (; i < count; i++) {
        if (values[i] < minVal) {
            minVal = values[i];
        }
        if (values[i] > maxVal) {
            maxVal = values[i];
        }
    }

    /* Fill metadata */
    uint64_t range = maxVal - minVal;
    meta->minValue = minVal;
    meta->maxValue = maxVal;
    meta->range = range;
    meta->offsetWidth = varintFORComputeWidth(range);
    meta->count = count;
    meta->encodedSize = varintFORSize(meta);
}

/* NEON-accelerated decode for 1-byte offsets */
static size_t varintFORBatchDecodeNEON1(const uint8_t *dataPtr,
                                        uint64_t *values, const size_t count,
                                        const uint64_t minValue) {
    size_t i = 0;
    uint64x2_t minVec = vdupq_n_u64(minValue);

    /* Process 8 bytes at a time, expanding to 64-bit */
    const size_t simdCount = (count / 8) * 8;
    for (; i < simdCount; i += 8) {
        /* Load 8 bytes */
        uint8x8_t bytes = vld1_u8(&dataPtr[i]);

        /* Expand to 16-bit, then 32-bit, then 64-bit */
        uint16x8_t u16 = vmovl_u8(bytes);
        uint32x4_t u32_lo = vmovl_u16(vget_low_u16(u16));
        uint32x4_t u32_hi = vmovl_u16(vget_high_u16(u16));

        /* Expand to 64-bit and add minValue */
        uint64x2_t u64_0 = vaddq_u64(vmovl_u32(vget_low_u32(u32_lo)), minVec);
        uint64x2_t u64_1 = vaddq_u64(vmovl_u32(vget_high_u32(u32_lo)), minVec);
        uint64x2_t u64_2 = vaddq_u64(vmovl_u32(vget_low_u32(u32_hi)), minVec);
        uint64x2_t u64_3 = vaddq_u64(vmovl_u32(vget_high_u32(u32_hi)), minVec);

        /* Store results */
        vst1q_u64(&values[i + 0], u64_0);
        vst1q_u64(&values[i + 2], u64_1);
        vst1q_u64(&values[i + 4], u64_2);
        vst1q_u64(&values[i + 6], u64_3);
    }

    /* Handle remaining elements */
    for (; i < count; i++) {
        values[i] = minValue + dataPtr[i];
    }

    return count;
}

/* NEON-accelerated decode for 2-byte offsets */
static size_t varintFORBatchDecodeNEON2(const uint8_t *dataPtr,
                                        uint64_t *values, const size_t count,
                                        const uint64_t minValue) {
    const uint16_t *data16 = (const uint16_t *)dataPtr;
    size_t i = 0;
    uint64x2_t minVec = vdupq_n_u64(minValue);

    /* Process 4 uint16s at a time */
    const size_t simdCount = (count / 4) * 4;
    for (; i < simdCount; i += 4) {
        uint16x4_t u16 = vld1_u16(&data16[i]);
        uint32x4_t u32 = vmovl_u16(u16);

        uint64x2_t u64_lo = vaddq_u64(vmovl_u32(vget_low_u32(u32)), minVec);
        uint64x2_t u64_hi = vaddq_u64(vmovl_u32(vget_high_u32(u32)), minVec);

        vst1q_u64(&values[i + 0], u64_lo);
        vst1q_u64(&values[i + 2], u64_hi);
    }

    /* Handle remaining elements */
    for (; i < count; i++) {
        values[i] = minValue + data16[i];
    }

    return count;
}

/* NEON-accelerated decode for 4-byte offsets */
static size_t varintFORBatchDecodeNEON4(const uint8_t *dataPtr,
                                        uint64_t *values, const size_t count,
                                        const uint64_t minValue) {
    const uint32_t *data32 = (const uint32_t *)dataPtr;
    size_t i = 0;
    uint64x2_t minVec = vdupq_n_u64(minValue);

    /* Process 2 uint32s at a time */
    const size_t simdCount = (count / 2) * 2;
    for (; i < simdCount; i += 2) {
        uint32x2_t u32 = vld1_u32(&data32[i]);
        uint64x2_t u64 = vaddq_u64(vmovl_u32(u32), minVec);
        vst1q_u64(&values[i], u64);
    }

    /* Handle remaining elements */
    for (; i < count; i++) {
        values[i] = minValue + data32[i];
    }

    return count;
}

#endif /* VARINT_FOR_NEON */

#ifdef VARINT_FOR_AVX2
/* ====================================================================
 * x86 AVX2 SIMD Implementation
 * ==================================================================== */

/* AVX2-accelerated min/max analysis */
static void varintFORBatchAnalyzeAVX2(const uint64_t *values,
                                      const size_t count, varintFORMeta *meta) {
    assert(count > 0);

    size_t i = 0;
    __m256i minVec = _mm256_set1_epi64x((int64_t)UINT64_MAX);
    __m256i maxVec = _mm256_setzero_si256();

    /* Main SIMD loop - 4 elements per iteration */
    const size_t simdCount = (count / 4) * 4;
    for (; i < simdCount; i += 4) {
        __m256i vals = _mm256_loadu_si256((const __m256i *)&values[i]);
        /* AVX2 doesn't have unsigned 64-bit min/max, use signed comparison */
        /* This works for values < 2^63 */
        minVec = _mm256_min_epu64(minVec, vals);
        maxVec = _mm256_max_epu64(maxVec, vals);
    }

    /* Horizontal reduction */
    uint64_t minArr[4], maxArr[4];
    _mm256_storeu_si256((__m256i *)minArr, minVec);
    _mm256_storeu_si256((__m256i *)maxArr, maxVec);

    uint64_t minVal = minArr[0];
    uint64_t maxVal = maxArr[0];
    for (int j = 1; j < 4; j++) {
        if (minArr[j] < minVal) {
            minVal = minArr[j];
        }
        if (maxArr[j] > maxVal) {
            maxVal = maxArr[j];
        }
    }

    /* Handle remaining elements */
    for (; i < count; i++) {
        if (values[i] < minVal) {
            minVal = values[i];
        }
        if (values[i] > maxVal) {
            maxVal = values[i];
        }
    }

    /* Fill metadata */
    uint64_t range = maxVal - minVal;
    meta->minValue = minVal;
    meta->maxValue = maxVal;
    meta->range = range;
    meta->offsetWidth = varintFORComputeWidth(range);
    meta->count = count;
    meta->encodedSize = varintFORSize(meta);
}

#endif /* VARINT_FOR_AVX2 */

/* ====================================================================
 * Public SIMD Batch API
 * ==================================================================== */

/* SIMD-accelerated batch analysis */
void varintFORBatchAnalyze(const uint64_t *values, const size_t count,
                           varintFORMeta *meta) {
    assert(values != NULL);
    assert(count > 0);
    assert(meta != NULL);

#ifdef VARINT_FOR_NEON
    if (count >= VARINT_FOR_SIMD_MIN_COUNT) {
        varintFORBatchAnalyzeNEON(values, count, meta);
        return;
    }
#endif

#ifdef VARINT_FOR_AVX2
    if (count >= VARINT_FOR_SIMD_MIN_COUNT) {
        varintFORBatchAnalyzeAVX2(values, count, meta);
        return;
    }
#endif

    /* Fallback to scalar */
    varintFORAnalyze(values, count, meta);
}

/* SIMD-accelerated batch decode */
size_t varintFORBatchDecode(const uint8_t *src, uint64_t *values,
                            const size_t maxCount) {
    assert(src != NULL);
    assert(values != NULL);

    varintFORMeta meta;
    varintFORReadMetadata(src, &meta);

    if (meta.count > maxCount) {
        return 0;
    }

#ifdef VARINT_FOR_NEON
    if (meta.count >= VARINT_FOR_SIMD_MIN_COUNT) {
        /* Calculate offset to data section for SIMD decode */
        varintWidth minWidth = varintTaggedLen(meta.minValue);
        varintWidth countWidth = varintTaggedLen(meta.count);
        const uint8_t *dataPtr = src + minWidth + 1 + countWidth;

        switch (meta.offsetWidth) {
        case VARINT_WIDTH_8B:
            return varintFORBatchDecodeNEON1(dataPtr, values, meta.count,
                                             meta.minValue);
        case VARINT_WIDTH_16B:
            return varintFORBatchDecodeNEON2(dataPtr, values, meta.count,
                                             meta.minValue);
        case VARINT_WIDTH_32B:
            return varintFORBatchDecodeNEON4(dataPtr, values, meta.count,
                                             meta.minValue);
        default:
            break; /* Fall through to scalar for larger widths */
        }
    }
#endif

    /* Fallback to scalar decode */
    return varintFORDecode(src, values, maxCount);
}

/* SIMD-accelerated batch encode */
size_t varintFORBatchEncode(uint8_t *dst, const uint64_t *values,
                            const size_t count, varintFORMeta *meta) {
    assert(dst != NULL);
    assert(values != NULL);
    assert(count > 0);

    /* Use SIMD analysis if beneficial */
    varintFORMeta localMeta;
    if (meta == NULL || meta->count != count) {
        varintFORBatchAnalyze(values, count, &localMeta);
        if (meta != NULL) {
            *meta = localMeta;
        }
        meta = &localMeta;
    }

    /* Header encoding is same as scalar */
    uint8_t *ptr = dst;

    varintWidth minWidth = varintTaggedPut64(ptr, meta->minValue);
    ptr += minWidth;

    *ptr = (uint8_t)meta->offsetWidth;
    ptr++;

    varintWidth countWidth = varintTaggedPut64(ptr, meta->count);
    ptr += countWidth;

    /* Encode offsets - SIMD could help here for fixed widths */
    /* For now, use optimized scalar loop */
    for (size_t i = 0; i < count; i++) {
        uint64_t offset = values[i] - meta->minValue;
        varintExternalPutFixedWidthQuick_(ptr, offset, meta->offsetWidth);
        ptr += meta->offsetWidth;
    }

    return (size_t)(ptr - dst);
}

/* Decode a block of values starting at given index */
size_t varintFORDecodeBlock(const uint8_t *src, uint64_t *values,
                            const size_t startIndex, const size_t blockSize) {
    assert(src != NULL);
    assert(values != NULL);

    varintFORMeta meta;
    varintFORReadMetadata(src, &meta);

    if (startIndex >= meta.count) {
        return 0;
    }

    const size_t actualBlockSize = (startIndex + blockSize > meta.count)
                                       ? (meta.count - startIndex)
                                       : blockSize;

    /* Calculate offset to start of block */
    varintWidth minWidth = varintTaggedLen(meta.minValue);
    varintWidth countWidth = varintTaggedLen(meta.count);
    const uint8_t *dataPtr =
        src + minWidth + 1 + countWidth + (startIndex * meta.offsetWidth);

#ifdef VARINT_FOR_NEON
    if (actualBlockSize >= VARINT_FOR_SIMD_MIN_COUNT) {
        switch (meta.offsetWidth) {
        case VARINT_WIDTH_8B:
            return varintFORBatchDecodeNEON1(dataPtr, values, actualBlockSize,
                                             meta.minValue);
        case VARINT_WIDTH_16B:
            return varintFORBatchDecodeNEON2(dataPtr, values, actualBlockSize,
                                             meta.minValue);
        case VARINT_WIDTH_32B:
            return varintFORBatchDecodeNEON4(dataPtr, values, actualBlockSize,
                                             meta.minValue);
        default:
            break;
        }
    }
#endif

    /* Scalar fallback */
    for (size_t i = 0; i < actualBlockSize; i++) {
        uint64_t offset;
        varintExternalGetQuick_(dataPtr, meta.offsetWidth, offset);
        values[i] = meta.minValue + offset;
        dataPtr += meta.offsetWidth;
    }

    return actualBlockSize;
}
