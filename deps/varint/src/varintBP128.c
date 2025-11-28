#include "varintBP128.h"
#include "varintTagged.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* SIMD detection */
#if defined(__AVX2__)
#define VARINT_BP128_AVX2 1
#include <immintrin.h>
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#define VARINT_BP128_NEON 1
#include <arm_neon.h>
#endif

/* ====================================================================
 * Bit-width Calculation
 * ==================================================================== */

uint8_t varintBP128MaxBitWidth32(const uint32_t *values, size_t count) {
    if (count == 0) {
        return 0;
    }

    uint32_t maxVal = 0;

#if defined(VARINT_BP128_NEON)
    /* NEON vectorized max finding */
    if (count >= 4) {
        uint32x4_t vmax = vdupq_n_u32(0);
        size_t i = 0;
        for (; i + 4 <= count; i += 4) {
            uint32x4_t v = vld1q_u32(&values[i]);
            vmax = vmaxq_u32(vmax, v);
        }
        /* Horizontal max */
        uint32x2_t vmax2 = vpmax_u32(vget_low_u32(vmax), vget_high_u32(vmax));
        vmax2 = vpmax_u32(vmax2, vmax2);
        maxVal = vget_lane_u32(vmax2, 0);
        /* Handle remainder */
        for (; i < count; i++) {
            if (values[i] > maxVal) {
                maxVal = values[i];
            }
        }
    } else
#elif defined(VARINT_BP128_AVX2)
    /* AVX2 vectorized max finding */
    if (count >= 8) {
        __m256i vmax = _mm256_setzero_si256();
        size_t i = 0;
        for (; i + 8 <= count; i += 8) {
            __m256i v = _mm256_loadu_si256((const __m256i *)&values[i]);
            vmax = _mm256_max_epu32(vmax, v);
        }
        /* Horizontal max - extract to 128-bit, then scalar */
        __m128i hi = _mm256_extracti128_si256(vmax, 1);
        __m128i lo = _mm256_castsi256_si128(vmax);
        __m128i max128 = _mm_max_epu32(hi, lo);
        max128 = _mm_max_epu32(max128, _mm_shuffle_epi32(max128, 0x4E));
        max128 = _mm_max_epu32(max128, _mm_shuffle_epi32(max128, 0xB1));
        maxVal = _mm_cvtsi128_si32(max128);
        /* Handle remainder */
        for (; i < count; i++) {
            if (values[i] > maxVal) {
                maxVal = values[i];
            }
        }
    } else
#endif
    {
        /* Scalar fallback */
        for (size_t i = 0; i < count; i++) {
            if (values[i] > maxVal) {
                maxVal = values[i];
            }
        }
    }

    return varintBP128BitsNeeded32(maxVal);
}

uint8_t varintBP128MaxBitWidth64(const uint64_t *values, size_t count) {
    if (count == 0) {
        return 0;
    }

    uint64_t maxVal = 0;
    for (size_t i = 0; i < count; i++) {
        if (values[i] > maxVal) {
            maxVal = values[i];
        }
    }

    return varintBP128BitsNeeded64(maxVal);
}

/* ====================================================================
 * Block Encoding/Decoding (32-bit)
 * ==================================================================== */

size_t varintBP128EncodeBlock32(uint8_t *dst, const uint32_t *values) {
    uint8_t bitWidth =
        varintBP128MaxBitWidth32(values, VARINT_BP128_BLOCK_SIZE);

    /* Write header */
    *dst++ = bitWidth;

    if (bitWidth == 0) {
        /* All zeros - just header */
        return 1;
    }

    /* Pack values bit by bit */
    size_t bitPos = 0;
    uint8_t *out = dst;
    memset(out, 0, (VARINT_BP128_BLOCK_SIZE * bitWidth + 7) / 8);

    for (size_t i = 0; i < VARINT_BP128_BLOCK_SIZE; i++) {
        uint32_t val = values[i];
        for (uint8_t b = 0; b < bitWidth; b++) {
            if ((val >> b) & 1) {
                size_t byteIdx = bitPos / 8;
                size_t bitIdx = bitPos % 8;
                out[byteIdx] |= (1 << bitIdx);
            }
            bitPos++;
        }
    }

    return 1 + (VARINT_BP128_BLOCK_SIZE * bitWidth + 7) / 8;
}

size_t varintBP128DecodeBlock32(const uint8_t *src, uint32_t *values) {
    uint8_t bitWidth = *src++;

    if (bitWidth == 0) {
        /* All zeros */
        memset(values, 0, VARINT_BP128_BLOCK_SIZE * sizeof(uint32_t));
        return 1;
    }

    /* Unpack values */
    size_t bitPos = 0;
    const uint8_t *in = src;

    for (size_t i = 0; i < VARINT_BP128_BLOCK_SIZE; i++) {
        uint32_t val = 0;
        for (uint8_t b = 0; b < bitWidth; b++) {
            size_t byteIdx = bitPos / 8;
            size_t bitIdx = bitPos % 8;
            if ((in[byteIdx] >> bitIdx) & 1) {
                val |= (1U << b);
            }
            bitPos++;
        }
        values[i] = val;
    }

    return 1 + (VARINT_BP128_BLOCK_SIZE * bitWidth + 7) / 8;
}

/* ====================================================================
 * Delta Block Encoding/Decoding (32-bit)
 * ==================================================================== */

size_t varintBP128DeltaEncodeBlock32(uint8_t *dst, const uint32_t *values,
                                     uint32_t prevValue) {
    /* Compute deltas */
    uint32_t deltas[VARINT_BP128_BLOCK_SIZE];
    uint32_t prev = prevValue;

    for (size_t i = 0; i < VARINT_BP128_BLOCK_SIZE; i++) {
        deltas[i] = values[i] - prev;
        prev = values[i];
    }

    /* Encode deltas as a block */
    return varintBP128EncodeBlock32(dst, deltas);
}

size_t varintBP128DeltaDecodeBlock32(const uint8_t *src, uint32_t *values,
                                     uint32_t prevValue) {
    /* Decode deltas */
    uint32_t deltas[VARINT_BP128_BLOCK_SIZE];
    size_t consumed = varintBP128DecodeBlock32(src, deltas);

    /* Prefix sum to recover values */
    uint32_t prev = prevValue;

#if defined(VARINT_BP128_NEON)
    /* NEON vectorized prefix sum */
    uint32x4_t vprev = vdupq_n_u32(prev);
    for (size_t i = 0; i < VARINT_BP128_BLOCK_SIZE; i += 4) {
        uint32x4_t d = vld1q_u32(&deltas[i]);
        /* Partial sums within vector */
        d = vaddq_u32(d, vextq_u32(vdupq_n_u32(0), d, 3));
        d = vaddq_u32(d, vextq_u32(vdupq_n_u32(0), d, 2));
        /* Add previous */
        d = vaddq_u32(d, vprev);
        vst1q_u32(&values[i], d);
        /* Update prev for next iteration */
        vprev = vdupq_n_u32(vgetq_lane_u32(d, 3));
    }
#else
    /* Scalar prefix sum */
    for (size_t i = 0; i < VARINT_BP128_BLOCK_SIZE; i++) {
        prev += deltas[i];
        values[i] = prev;
    }
#endif

    return consumed;
}

/* ====================================================================
 * Array Encoding (32-bit)
 * ==================================================================== */

size_t varintBP128Encode32(uint8_t *dst, const uint32_t *values, size_t count,
                           varintBP128Meta *meta) {
    if (count == 0) {
        if (meta) {
            memset(meta, 0, sizeof(*meta));
        }
        return 0;
    }

    uint8_t *ptr = dst;
    size_t fullBlocks = count / VARINT_BP128_BLOCK_SIZE;
    size_t remainder = count % VARINT_BP128_BLOCK_SIZE;
    uint8_t maxBitWidth = 0;

    /* Encode full blocks */
    for (size_t b = 0; b < fullBlocks; b++) {
        size_t blockBytes =
            varintBP128EncodeBlock32(ptr, &values[b * VARINT_BP128_BLOCK_SIZE]);
        if (*ptr > maxBitWidth) {
            maxBitWidth = *ptr;
        }
        ptr += blockBytes;
    }

    /* Encode partial block if any */
    if (remainder > 0) {
        /* Pad with zeros to make a full block */
        uint32_t padded[VARINT_BP128_BLOCK_SIZE] = {0};
        memcpy(padded, &values[fullBlocks * VARINT_BP128_BLOCK_SIZE],
               remainder * sizeof(uint32_t));

        /* Write marker for partial block: bit 7 set + remainder count */
        uint8_t bitWidth = varintBP128MaxBitWidth32(padded, remainder);
        *ptr++ = 0x80 | bitWidth; /* Mark as partial */
        *ptr++ = (uint8_t)remainder;

        if (bitWidth > 0) {
            /* Pack only the values we have */
            size_t bitPos = 0;
            memset(ptr, 0, (remainder * bitWidth + 7) / 8);
            for (size_t i = 0; i < remainder; i++) {
                uint32_t val = padded[i];
                for (uint8_t b = 0; b < bitWidth; b++) {
                    if ((val >> b) & 1) {
                        size_t byteIdx = bitPos / 8;
                        size_t bitIdx = bitPos % 8;
                        ptr[byteIdx] |= (1 << bitIdx);
                    }
                    bitPos++;
                }
            }
            ptr += (remainder * bitWidth + 7) / 8;
        }

        if ((bitWidth & 0x7F) > maxBitWidth) {
            maxBitWidth = bitWidth & 0x7F;
        }
    }

    if (meta) {
        meta->count = count;
        meta->blockCount = fullBlocks + (remainder > 0 ? 1 : 0);
        meta->encodedBytes = (size_t)(ptr - dst);
        meta->lastBlockSize =
            remainder > 0 ? remainder : VARINT_BP128_BLOCK_SIZE;
        meta->maxBitWidth = maxBitWidth;
    }

    return (size_t)(ptr - dst);
}

size_t varintBP128Decode32(const uint8_t *src, uint32_t *values,
                           size_t maxCount) {
    const uint8_t *ptr = src;
    size_t decoded = 0;

    while (decoded < maxCount) {
        uint8_t header = *ptr;

        if (header & 0x80) {
            /* Partial block */
            uint8_t bitWidth = header & 0x7F;
            ptr++;
            uint8_t blockCount = *ptr++;

            if (decoded + blockCount > maxCount) {
                blockCount = (uint8_t)(maxCount - decoded);
            }

            if (bitWidth == 0) {
                memset(&values[decoded], 0, blockCount * sizeof(uint32_t));
            } else {
                size_t bitPos = 0;
                for (size_t i = 0; i < blockCount; i++) {
                    uint32_t val = 0;
                    for (uint8_t b = 0; b < bitWidth; b++) {
                        size_t byteIdx = bitPos / 8;
                        size_t bitIdx = bitPos % 8;
                        if ((ptr[byteIdx] >> bitIdx) & 1) {
                            val |= (1U << b);
                        }
                        bitPos++;
                    }
                    values[decoded + i] = val;
                }
                /* ptr advances but we break after; suppress unused warning */
            }
            decoded += blockCount;
            break; /* Partial block is always last */
        } else {
            /* Full block */
            if (decoded + VARINT_BP128_BLOCK_SIZE > maxCount) {
                break; /* Not enough room */
            }
            size_t consumed = varintBP128DecodeBlock32(ptr, &values[decoded]);
            ptr += consumed;
            decoded += VARINT_BP128_BLOCK_SIZE;
        }
    }

    return decoded;
}

/* ====================================================================
 * Delta Array Encoding (32-bit)
 * ==================================================================== */

size_t varintBP128DeltaEncode32(uint8_t *dst, const uint32_t *values,
                                size_t count, varintBP128Meta *meta) {
    if (count == 0) {
        if (meta) {
            memset(meta, 0, sizeof(*meta));
        }
        return 0;
    }

    uint8_t *ptr = dst;

    /* Write first value using varint */
    ptr += varintTaggedPut64(ptr, values[0]);

    /* Write remaining values as delta-encoded blocks */
    size_t remaining = count - 1;
    const uint32_t *dataPtr = &values[1];
    uint32_t prevValue = values[0];
    uint8_t maxBitWidth = 0;
    size_t blockCount = 0;

    while (remaining >= VARINT_BP128_BLOCK_SIZE) {
        size_t blockBytes =
            varintBP128DeltaEncodeBlock32(ptr, dataPtr, prevValue);
        if ((*ptr & 0x7F) > maxBitWidth) {
            maxBitWidth = *ptr & 0x7F;
        }
        ptr += blockBytes;
        prevValue = dataPtr[VARINT_BP128_BLOCK_SIZE - 1];
        dataPtr += VARINT_BP128_BLOCK_SIZE;
        remaining -= VARINT_BP128_BLOCK_SIZE;
        blockCount++;
    }

    /* Handle remainder */
    if (remaining > 0) {
        uint32_t deltas[VARINT_BP128_BLOCK_SIZE] = {0};
        uint32_t prev = prevValue;
        for (size_t i = 0; i < remaining; i++) {
            deltas[i] = dataPtr[i] - prev;
            prev = dataPtr[i];
        }

        uint8_t bitWidth = varintBP128MaxBitWidth32(deltas, remaining);
        *ptr++ = 0x80 | bitWidth;
        *ptr++ = (uint8_t)remaining;

        if (bitWidth > 0) {
            size_t bitPos = 0;
            memset(ptr, 0, (remaining * bitWidth + 7) / 8);
            for (size_t i = 0; i < remaining; i++) {
                uint32_t val = deltas[i];
                for (uint8_t b = 0; b < bitWidth; b++) {
                    if ((val >> b) & 1) {
                        size_t byteIdx = bitPos / 8;
                        size_t bitIdx = bitPos % 8;
                        ptr[byteIdx] |= (1 << bitIdx);
                    }
                    bitPos++;
                }
            }
            ptr += (remaining * bitWidth + 7) / 8;
        }

        if (bitWidth > maxBitWidth) {
            maxBitWidth = bitWidth;
        }
        blockCount++;
    }

    if (meta) {
        meta->count = count;
        meta->blockCount = blockCount;
        meta->encodedBytes = (size_t)(ptr - dst);
        meta->lastBlockSize =
            remaining > 0 ? remaining : VARINT_BP128_BLOCK_SIZE;
        meta->maxBitWidth = maxBitWidth;
    }

    return (size_t)(ptr - dst);
}

size_t varintBP128DeltaDecode32(const uint8_t *src, uint32_t *values,
                                size_t maxCount) {
    if (maxCount == 0) {
        return 0;
    }

    const uint8_t *ptr = src;

    /* Read first value */
    uint64_t first64;
    ptr += varintTaggedGet64(ptr, &first64);
    values[0] = (uint32_t)first64;

    size_t decoded = 1;
    uint32_t prevValue = values[0];

    while (decoded < maxCount) {
        uint8_t header = *ptr;

        if (header & 0x80) {
            /* Partial block */
            uint8_t bitWidth = header & 0x7F;
            ptr++;
            uint8_t blockCount = *ptr++;

            if (decoded + blockCount > maxCount) {
                blockCount = (uint8_t)(maxCount - decoded);
            }

            /* Decode deltas and accumulate */
            size_t bitPos = 0;
            for (size_t i = 0; i < blockCount; i++) {
                uint32_t delta = 0;
                if (bitWidth > 0) {
                    for (uint8_t b = 0; b < bitWidth; b++) {
                        size_t byteIdx = bitPos / 8;
                        size_t bitIdx = bitPos % 8;
                        if ((ptr[byteIdx] >> bitIdx) & 1) {
                            delta |= (1U << b);
                        }
                        bitPos++;
                    }
                }
                prevValue += delta;
                values[decoded + i] = prevValue;
            }
            /* ptr would advance here but we break after; suppress warning */
            decoded += blockCount;
            break;
        } else {
            /* Full block */
            if (decoded + VARINT_BP128_BLOCK_SIZE > maxCount) {
                break;
            }
            size_t consumed =
                varintBP128DeltaDecodeBlock32(ptr, &values[decoded], prevValue);
            ptr += consumed;
            prevValue = values[decoded + VARINT_BP128_BLOCK_SIZE - 1];
            decoded += VARINT_BP128_BLOCK_SIZE;
        }
    }

    return decoded;
}

/* ====================================================================
 * 64-bit Variants
 * ==================================================================== */

size_t varintBP128Encode64(uint8_t *dst, const uint64_t *values, size_t count,
                           varintBP128Meta *meta) {
    /* Convert to 32-bit if possible, otherwise use 64-bit encoding */
    /* For simplicity, we'll use a straightforward 64-bit implementation */
    if (count == 0) {
        if (meta) {
            memset(meta, 0, sizeof(*meta));
        }
        return 0;
    }

    uint8_t *ptr = dst;
    uint8_t maxBitWidth = 0;

    /* Write count */
    ptr += varintTaggedPut64(ptr, count);

    /* Encode each value with its required bits */
    for (size_t i = 0; i < count; i += VARINT_BP128_BLOCK_SIZE) {
        size_t blockSize = (count - i < VARINT_BP128_BLOCK_SIZE)
                               ? count - i
                               : VARINT_BP128_BLOCK_SIZE;

        uint8_t bitWidth = varintBP128MaxBitWidth64(&values[i], blockSize);
        if (bitWidth > maxBitWidth) {
            maxBitWidth = bitWidth;
        }

        /* Write header */
        if (blockSize < VARINT_BP128_BLOCK_SIZE) {
            *ptr++ = 0x80 | bitWidth;
            *ptr++ = (uint8_t)blockSize;
        } else {
            *ptr++ = bitWidth;
        }

        if (bitWidth > 0) {
            size_t bitPos = 0;
            size_t packedBytes = (blockSize * bitWidth + 7) / 8;
            memset(ptr, 0, packedBytes);

            for (size_t j = 0; j < blockSize; j++) {
                uint64_t val = values[i + j];
                for (uint8_t b = 0; b < bitWidth; b++) {
                    if ((val >> b) & 1) {
                        size_t byteIdx = bitPos / 8;
                        size_t bitIdx = bitPos % 8;
                        ptr[byteIdx] |= (1 << bitIdx);
                    }
                    bitPos++;
                }
            }
            ptr += packedBytes;
        }
    }

    if (meta) {
        meta->count = count;
        meta->blockCount =
            (count + VARINT_BP128_BLOCK_SIZE - 1) / VARINT_BP128_BLOCK_SIZE;
        meta->encodedBytes = (size_t)(ptr - dst);
        meta->lastBlockSize = count % VARINT_BP128_BLOCK_SIZE;
        if (meta->lastBlockSize == 0) {
            meta->lastBlockSize = VARINT_BP128_BLOCK_SIZE;
        }
        meta->maxBitWidth = maxBitWidth;
    }

    return (size_t)(ptr - dst);
}

size_t varintBP128Decode64(const uint8_t *src, uint64_t *values,
                           size_t maxCount) {
    const uint8_t *ptr = src;

    /* Read count */
    uint64_t count;
    ptr += varintTaggedGet64(ptr, &count);

    if (count > maxCount) {
        count = maxCount;
    }

    size_t decoded = 0;
    while (decoded < count) {
        uint8_t header = *ptr++;
        size_t blockSize = VARINT_BP128_BLOCK_SIZE;
        uint8_t bitWidth = header;

        if (header & 0x80) {
            bitWidth = header & 0x7F;
            blockSize = *ptr++;
        }

        if (decoded + blockSize > count) {
            blockSize = count - decoded;
        }

        if (bitWidth == 0) {
            memset(&values[decoded], 0, blockSize * sizeof(uint64_t));
        } else {
            size_t bitPos = 0;
            for (size_t j = 0; j < blockSize; j++) {
                uint64_t val = 0;
                for (uint8_t b = 0; b < bitWidth; b++) {
                    size_t byteIdx = bitPos / 8;
                    size_t bitIdx = bitPos % 8;
                    if ((ptr[byteIdx] >> bitIdx) & 1) {
                        val |= (1ULL << b);
                    }
                    bitPos++;
                }
                values[decoded + j] = val;
            }
            ptr += (blockSize * bitWidth + 7) / 8;
        }
        decoded += blockSize;
    }

    return decoded;
}

size_t varintBP128DeltaEncode64(uint8_t *dst, const uint64_t *values,
                                size_t count, varintBP128Meta *meta) {
    if (count == 0) {
        if (meta) {
            memset(meta, 0, sizeof(*meta));
        }
        return 0;
    }

    uint8_t *ptr = dst;

    /* Write first value */
    ptr += varintTaggedPut64(ptr, values[0]);

    /* Compute deltas and encode */
    uint64_t *deltas = (uint64_t *)ptr; /* Temporary - we'll overwrite */
    (void)deltas;                       /* We'll encode inline */

    uint64_t prev = values[0];
    uint8_t maxBitWidth = 0;

    for (size_t i = 1; i < count; i += VARINT_BP128_BLOCK_SIZE) {
        size_t blockSize = (count - i < VARINT_BP128_BLOCK_SIZE)
                               ? count - i
                               : VARINT_BP128_BLOCK_SIZE;

        /* Compute deltas and find max */
        uint64_t blockDeltas[VARINT_BP128_BLOCK_SIZE];
        uint64_t blockMax = 0;
        for (size_t j = 0; j < blockSize; j++) {
            uint64_t delta = values[i + j] - prev;
            blockDeltas[j] = delta;
            if (delta > blockMax) {
                blockMax = delta;
            }
            prev = values[i + j];
        }

        uint8_t bitWidth = varintBP128BitsNeeded64(blockMax);
        if (bitWidth > maxBitWidth) {
            maxBitWidth = bitWidth;
        }

        /* Write header */
        if (blockSize < VARINT_BP128_BLOCK_SIZE) {
            *ptr++ = 0x80 | bitWidth;
            *ptr++ = (uint8_t)blockSize;
        } else {
            *ptr++ = bitWidth;
        }

        if (bitWidth > 0) {
            size_t packedBytes = (blockSize * bitWidth + 7) / 8;
            memset(ptr, 0, packedBytes);
            size_t bitPos = 0;

            for (size_t j = 0; j < blockSize; j++) {
                uint64_t val = blockDeltas[j];
                for (uint8_t b = 0; b < bitWidth; b++) {
                    if ((val >> b) & 1) {
                        size_t byteIdx = bitPos / 8;
                        size_t bitIdx = bitPos % 8;
                        ptr[byteIdx] |= (1 << bitIdx);
                    }
                    bitPos++;
                }
            }
            ptr += packedBytes;
        }
    }

    if (meta) {
        meta->count = count;
        meta->blockCount =
            (count + VARINT_BP128_BLOCK_SIZE - 2) / VARINT_BP128_BLOCK_SIZE;
        meta->encodedBytes = (size_t)(ptr - dst);
        meta->maxBitWidth = maxBitWidth;
    }

    return (size_t)(ptr - dst);
}

size_t varintBP128DeltaDecode64(const uint8_t *src, uint64_t *values,
                                size_t maxCount) {
    if (maxCount == 0) {
        return 0;
    }

    const uint8_t *ptr = src;

    /* Read first value */
    ptr += varintTaggedGet64(ptr, &values[0]);

    size_t decoded = 1;
    uint64_t prev = values[0];

    while (decoded < maxCount) {
        uint8_t header = *ptr++;
        size_t blockSize = VARINT_BP128_BLOCK_SIZE;
        uint8_t bitWidth = header;

        if (header & 0x80) {
            bitWidth = header & 0x7F;
            blockSize = *ptr++;
        }

        if (decoded + blockSize > maxCount) {
            blockSize = maxCount - decoded;
        }

        /* Decode deltas and accumulate */
        size_t bitPos = 0;
        for (size_t j = 0; j < blockSize; j++) {
            uint64_t delta = 0;
            if (bitWidth > 0) {
                for (uint8_t b = 0; b < bitWidth; b++) {
                    size_t byteIdx = bitPos / 8;
                    size_t bitIdx = bitPos % 8;
                    if ((ptr[byteIdx] >> bitIdx) & 1) {
                        delta |= (1ULL << b);
                    }
                    bitPos++;
                }
            }
            prev += delta;
            values[decoded + j] = prev;
        }
        if (bitWidth > 0) {
            ptr += (blockSize * bitWidth + 7) / 8;
        }
        decoded += blockSize;

        if (header & 0x80) {
            break; /* Partial block is always last */
        }
    }

    return decoded;
}

/* ====================================================================
 * Utility Functions
 * ==================================================================== */

bool varintBP128IsBeneficial32(const uint32_t *values, size_t count) {
    if (count == 0) {
        return false;
    }

    size_t fullBlocks = count / VARINT_BP128_BLOCK_SIZE;
    size_t remainder = count % VARINT_BP128_BLOCK_SIZE;
    size_t estimatedSize = 0;

    for (size_t b = 0; b < fullBlocks; b++) {
        uint8_t bw = varintBP128MaxBitWidth32(
            &values[b * VARINT_BP128_BLOCK_SIZE], VARINT_BP128_BLOCK_SIZE);
        estimatedSize += 1 + (VARINT_BP128_BLOCK_SIZE * bw + 7) / 8;
    }

    if (remainder > 0) {
        uint8_t bw = varintBP128MaxBitWidth32(
            &values[fullBlocks * VARINT_BP128_BLOCK_SIZE], remainder);
        estimatedSize += 2 + (remainder * bw + 7) / 8;
    }

    return estimatedSize < count * sizeof(uint32_t);
}

bool varintBP128IsBeneficial64(const uint64_t *values, size_t count) {
    if (count == 0) {
        return false;
    }

    size_t estimatedSize = 10; /* Rough count overhead */
    for (size_t i = 0; i < count; i += VARINT_BP128_BLOCK_SIZE) {
        size_t blockSize = (count - i < VARINT_BP128_BLOCK_SIZE)
                               ? count - i
                               : VARINT_BP128_BLOCK_SIZE;
        uint8_t bw = varintBP128MaxBitWidth64(&values[i], blockSize);
        estimatedSize += (blockSize < VARINT_BP128_BLOCK_SIZE ? 2 : 1) +
                         (blockSize * bw + 7) / 8;
    }

    return estimatedSize < count * sizeof(uint64_t);
}

bool varintBP128IsSorted32(const uint32_t *values, size_t count) {
    for (size_t i = 1; i < count; i++) {
        if (values[i] < values[i - 1]) {
            return false;
        }
    }
    return true;
}

bool varintBP128IsSorted64(const uint64_t *values, size_t count) {
    for (size_t i = 1; i < count; i++) {
        if (values[i] < values[i - 1]) {
            return false;
        }
    }
    return true;
}

size_t varintBP128GetCount(const uint8_t *src, size_t srcBytes) {
    (void)srcBytes;
    uint64_t count;
    varintTaggedGet64(src, &count);
    return (size_t)count;
}

/* ====================================================================
 * Unit Tests
 * ==================================================================== */
#ifdef VARINT_BP128_TEST
#include "ctest.h"

int varintBP128Test(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("BP128 block encode/decode 32-bit") {
        uint32_t values[VARINT_BP128_BLOCK_SIZE];
        for (size_t i = 0; i < VARINT_BP128_BLOCK_SIZE; i++) {
            values[i] = (uint32_t)(i * 7);
        }

        uint8_t encoded[VARINT_BP128_MAX_BLOCK_BYTES];
        size_t encodedBytes = varintBP128EncodeBlock32(encoded, values);

        uint32_t decoded[VARINT_BP128_BLOCK_SIZE];
        size_t consumedBytes = varintBP128DecodeBlock32(encoded, decoded);

        if (encodedBytes != consumedBytes) {
            ERR("Bytes mismatch: encoded %zu, consumed %zu", encodedBytes,
                consumedBytes);
        }

        for (size_t i = 0; i < VARINT_BP128_BLOCK_SIZE; i++) {
            if (decoded[i] != values[i]) {
                ERR("Value mismatch at %zu: expected %u, got %u", i, values[i],
                    decoded[i]);
            }
        }
    }

    TEST("BP128 array encode/decode 32-bit") {
        uint32_t values[300];
        for (size_t i = 0; i < 300; i++) {
            values[i] = (uint32_t)(i * 3);
        }

        uint8_t encoded[4096];
        varintBP128Meta meta;
        size_t encodedBytes = varintBP128Encode32(encoded, values, 300, &meta);

        if (meta.count != 300) {
            ERR("Meta count wrong: expected 300, got %zu", meta.count);
        }

        uint32_t decoded[300];
        size_t decodedCount = varintBP128Decode32(encoded, decoded, 300);

        if (decodedCount != 300) {
            ERR("Decoded count: expected 300, got %zu", decodedCount);
        }

        for (size_t i = 0; i < 300; i++) {
            if (decoded[i] != values[i]) {
                ERR("Value mismatch at %zu: expected %u, got %u", i, values[i],
                    decoded[i]);
            }
        }

        (void)encodedBytes;
    }

    TEST("BP128 delta encode/decode 32-bit") {
        /* Sorted sequence */
        uint32_t values[200];
        for (size_t i = 0; i < 200; i++) {
            values[i] = (uint32_t)(1000 + i * 5);
        }

        uint8_t encoded[2048];
        varintBP128Meta meta;
        size_t encodedBytes =
            varintBP128DeltaEncode32(encoded, values, 200, &meta);

        uint32_t decoded[200];
        size_t decodedCount = varintBP128DeltaDecode32(encoded, decoded, 200);

        if (decodedCount != 200) {
            ERR("Delta decoded count: expected 200, got %zu", decodedCount);
        }

        for (size_t i = 0; i < 200; i++) {
            if (decoded[i] != values[i]) {
                ERR("Delta value mismatch at %zu: expected %u, got %u", i,
                    values[i], decoded[i]);
            }
        }

        /* Delta encoding should compress sorted data well */
        if (encodedBytes >= 200 * sizeof(uint32_t)) {
            ERRR("Delta encoding didn't compress sorted data");
        }
    }

    TEST("BP128 64-bit encode/decode") {
        uint64_t values[150];
        for (size_t i = 0; i < 150; i++) {
            values[i] = (uint64_t)(i * 100);
        }

        uint8_t encoded[4096];
        varintBP128Meta meta;
        size_t encodedBytes = varintBP128Encode64(encoded, values, 150, &meta);

        uint64_t decoded[150];
        size_t decodedCount = varintBP128Decode64(encoded, decoded, 150);

        if (decodedCount != 150) {
            ERR("64-bit decoded count: expected 150, got %zu", decodedCount);
        }

        for (size_t i = 0; i < 150; i++) {
            if (decoded[i] != values[i]) {
                ERR("64-bit value mismatch at %zu", i);
            }
        }

        (void)encodedBytes;
    }

    TEST("BP128 delta 64-bit") {
        uint64_t values[100];
        for (size_t i = 0; i < 100; i++) {
            values[i] = (uint64_t)(10000 + i * 10);
        }

        uint8_t encoded[2048];
        varintBP128Meta meta;
        size_t encodedBytes =
            varintBP128DeltaEncode64(encoded, values, 100, &meta);

        uint64_t decoded[100];
        size_t decodedCount = varintBP128DeltaDecode64(encoded, decoded, 100);

        if (decodedCount != 100) {
            ERR("Delta 64-bit decoded count: expected 100, got %zu",
                decodedCount);
        }

        for (size_t i = 0; i < 100; i++) {
            if (decoded[i] != values[i]) {
                ERR("Delta 64-bit mismatch at %zu: expected %llu, got %llu", i,
                    (unsigned long long)values[i],
                    (unsigned long long)decoded[i]);
            }
        }

        (void)encodedBytes;
    }

    TEST("BP128 compression benefit analysis") {
        /* Small values should compress well */
        uint32_t small[100];
        for (size_t i = 0; i < 100; i++) {
            small[i] = (uint32_t)(i % 16);
        }
        if (!varintBP128IsBeneficial32(small, 100)) {
            ERRR("BP128 should be beneficial for small values");
        }

        /* Sorted check */
        const uint32_t sorted[] = {1, 2, 3, 5, 8, 13, 21};
        if (!varintBP128IsSorted32(sorted, 7)) {
            ERRR("Should detect sorted array");
        }

        const uint32_t unsorted[] = {1, 5, 3, 7};
        if (varintBP128IsSorted32(unsorted, 4)) {
            ERRR("Should detect unsorted array");
        }
    }

    TEST("BP128 zero values") {
        const uint32_t zeros[VARINT_BP128_BLOCK_SIZE] = {0};

        uint8_t encoded[VARINT_BP128_MAX_BLOCK_BYTES];
        size_t encodedBytes = varintBP128EncodeBlock32(encoded, zeros);

        /* All zeros should just be 1 byte header with bitWidth=0 */
        if (encodedBytes != 1) {
            ERR("Zero block should be 1 byte, got %zu", encodedBytes);
        }

        uint32_t decoded[VARINT_BP128_BLOCK_SIZE];
        varintBP128DecodeBlock32(encoded, decoded);

        for (size_t i = 0; i < VARINT_BP128_BLOCK_SIZE; i++) {
            if (decoded[i] != 0) {
                ERR("Zero decode failed at %zu: got %u", i, decoded[i]);
            }
        }
    }

    TEST("BP128 max values") {
        uint32_t maxVals[VARINT_BP128_BLOCK_SIZE];
        for (size_t i = 0; i < VARINT_BP128_BLOCK_SIZE; i++) {
            maxVals[i] = UINT32_MAX;
        }

        uint8_t encoded[VARINT_BP128_MAX_BLOCK_BYTES];
        size_t encodedBytes = varintBP128EncodeBlock32(encoded, maxVals);

        uint32_t decoded[VARINT_BP128_BLOCK_SIZE];
        varintBP128DecodeBlock32(encoded, decoded);

        for (size_t i = 0; i < VARINT_BP128_BLOCK_SIZE; i++) {
            if (decoded[i] != UINT32_MAX) {
                ERR("Max value decode failed at %zu", i);
            }
        }

        (void)encodedBytes;
    }

    TEST("BP128 boundary sizes") {
        /* Test array sizes around 128 boundary */
        size_t testSizes[] = {1, 2, 127, 128, 129, 255, 256, 257, 500, 1000};

        for (size_t t = 0; t < sizeof(testSizes) / sizeof(testSizes[0]); t++) {
            size_t count = testSizes[t];
            uint32_t *values = malloc(count * sizeof(uint32_t));
            if (!values) {
                continue;
            }

            for (size_t i = 0; i < count; i++) {
                values[i] = (uint32_t)(i * 7);
            }

            uint8_t *encoded = malloc(varintBP128MaxBytes(count));
            if (!encoded) {
                free(values);
                continue;
            }
            varintBP128Meta meta;
            varintBP128Encode32(encoded, values, count, &meta);

            if (meta.count != count) {
                ERR("Boundary size %zu: meta.count=%zu", count, meta.count);
            }

            uint32_t *decoded = malloc(count * sizeof(uint32_t));
            if (!decoded) {
                free(values);
                free(encoded);
                continue;
            }
            size_t decodedCount = varintBP128Decode32(encoded, decoded, count);

            if (decodedCount != count) {
                ERR("Boundary size %zu: decoded %zu values", count,
                    decodedCount);
            }

            bool match = true;
            for (size_t i = 0; i < count && match; i++) {
                if (decoded[i] != values[i]) {
                    ERR("Boundary size %zu: mismatch at %zu", count, i);
                    match = false;
                }
            }

            free(values);
            free(encoded);
            free(decoded);
        }
    }

    TEST("BP128 single value") {
        const uint32_t values[] = {42};

        uint8_t encoded[100];
        varintBP128Meta meta;
        varintBP128Encode32(encoded, values, 1, &meta);

        if (meta.count != 1) {
            ERR("Single value: meta.count=%zu", meta.count);
        }

        uint32_t decoded[1];
        size_t decodedCount = varintBP128Decode32(encoded, decoded, 1);

        if (decodedCount != 1) {
            ERR("Single value: decoded %zu", decodedCount);
        }
        if (decoded[0] != 42) {
            ERR("Single value: expected 42, got %u", decoded[0]);
        }
    }

    TEST("BP128 empty array") {
        uint8_t encoded[100];
        varintBP128Meta meta;
        size_t encodedBytes = varintBP128Encode32(encoded, NULL, 0, &meta);

        if (encodedBytes != 0) {
            ERR("Empty array: encodedBytes=%zu", encodedBytes);
        }
        if (meta.count != 0) {
            ERR("Empty array: meta.count=%zu", meta.count);
        }
    }

    TEST("BP128 powers of 2") {
        uint32_t values[32];
        for (int i = 0; i < 32; i++) {
            values[i] = 1U << i;
        }

        uint8_t encoded[1024];
        varintBP128Meta meta;
        varintBP128Encode32(encoded, values, 32, &meta);

        uint32_t decoded[32];
        size_t decodedCount = varintBP128Decode32(encoded, decoded, 32);

        if (decodedCount != 32) {
            ERR("Powers of 2: decoded %zu values", decodedCount);
        }

        for (int i = 0; i < 32; i++) {
            if (decoded[i] != values[i]) {
                ERR("Powers of 2: mismatch at %d", i);
            }
        }
    }

    TEST("BP128 various bit widths") {
        /* Test with different required bit widths */
        uint8_t testWidths[] = {1, 2, 4, 8, 10, 16, 24, 32};

        for (size_t w = 0; w < sizeof(testWidths); w++) {
            uint8_t width = testWidths[w];
            uint32_t maxVal = (width >= 32) ? UINT32_MAX : (1U << width) - 1;

            uint32_t values[VARINT_BP128_BLOCK_SIZE];
            for (size_t i = 0; i < VARINT_BP128_BLOCK_SIZE; i++) {
                values[i] = maxVal;
            }

            uint8_t encoded[VARINT_BP128_MAX_BLOCK_BYTES];
            size_t encodedBytes = varintBP128EncodeBlock32(encoded, values);

            /* Header byte should contain bitWidth */
            if (encoded[0] != width) {
                ERR("Bit width %u: header shows %u", width, encoded[0]);
            }

            uint32_t decoded[VARINT_BP128_BLOCK_SIZE];
            varintBP128DecodeBlock32(encoded, decoded);

            for (size_t i = 0; i < VARINT_BP128_BLOCK_SIZE; i++) {
                if (decoded[i] != maxVal) {
                    ERR("Bit width %u: mismatch at %zu", width, i);
                    break;
                }
            }

            (void)encodedBytes;
        }
    }

    TEST("BP128 delta constant gaps") {
        /* Sorted sequence with constant gap of 10 */
        uint32_t values[200];
        for (size_t i = 0; i < 200; i++) {
            values[i] = (uint32_t)(100 + i * 10);
        }

        uint8_t encoded[2048];
        varintBP128Meta meta;
        size_t encodedBytes =
            varintBP128DeltaEncode32(encoded, values, 200, &meta);

        /* Constant gap of 10 = 4 bits max */
        if (meta.maxBitWidth > 4) {
            ERR("Constant gap 10: maxBitWidth=%u, expected <=4",
                meta.maxBitWidth);
        }

        uint32_t decoded[200];
        size_t decodedCount = varintBP128DeltaDecode32(encoded, decoded, 200);

        if (decodedCount != 200) {
            ERR("Constant gap: decoded %zu values", decodedCount);
        }

        for (size_t i = 0; i < 200; i++) {
            if (decoded[i] != values[i]) {
                ERR("Constant gap: mismatch at %zu: expected %u, got %u", i,
                    values[i], decoded[i]);
            }
        }

        (void)encodedBytes;
    }

    TEST("BP128 delta increasing gaps") {
        /* Sorted sequence with increasing gaps */
        uint32_t values[100];
        values[0] = 0;
        for (size_t i = 1; i < 100; i++) {
            values[i] = values[i - 1] + (uint32_t)i;
        }

        uint8_t encoded[2048];
        varintBP128Meta meta;
        varintBP128DeltaEncode32(encoded, values, 100, &meta);

        uint32_t decoded[100];
        size_t decodedCount = varintBP128DeltaDecode32(encoded, decoded, 100);

        if (decodedCount != 100) {
            ERR("Increasing gaps: decoded %zu values", decodedCount);
        }

        for (size_t i = 0; i < 100; i++) {
            if (decoded[i] != values[i]) {
                ERR("Increasing gaps: mismatch at %zu", i);
            }
        }
    }

    TEST("BP128 64-bit large values") {
        uint64_t values[50];
        for (size_t i = 0; i < 50; i++) {
            values[i] = (1ULL << 40) + i * 1000;
        }

        uint8_t encoded[4096];
        varintBP128Meta meta;
        varintBP128Encode64(encoded, values, 50, &meta);

        uint64_t decoded[50];
        size_t decodedCount = varintBP128Decode64(encoded, decoded, 50);

        if (decodedCount != 50) {
            ERR("64-bit large: decoded %zu values", decodedCount);
        }

        for (size_t i = 0; i < 50; i++) {
            if (decoded[i] != values[i]) {
                ERR("64-bit large: mismatch at %zu", i);
            }
        }
    }

    TEST("BP128 delta 64-bit sorted") {
        uint64_t values[100];
        for (size_t i = 0; i < 100; i++) {
            values[i] = (uint64_t)(1000000 + i * 100);
        }

        uint8_t encoded[2048];
        varintBP128Meta meta;
        size_t encodedBytes =
            varintBP128DeltaEncode64(encoded, values, 100, &meta);

        uint64_t decoded[100];
        size_t decodedCount = varintBP128DeltaDecode64(encoded, decoded, 100);

        if (decodedCount != 100) {
            ERR("Delta 64-bit sorted: decoded %zu values", decodedCount);
        }

        for (size_t i = 0; i < 100; i++) {
            if (decoded[i] != values[i]) {
                ERR("Delta 64-bit sorted: mismatch at %zu", i);
            }
        }

        /* Should compress well */
        if (encodedBytes >= 100 * sizeof(uint64_t)) {
            ERRR("Delta 64-bit should compress");
        }
    }

    TEST("BP128 meta structure") {
        uint32_t values[300];
        for (size_t i = 0; i < 300; i++) {
            values[i] = (uint32_t)(i * 5);
        }

        uint8_t encoded[4096];
        varintBP128Meta meta;
        size_t encodedBytes = varintBP128Encode32(encoded, values, 300, &meta);

        if (meta.count != 300) {
            ERR("Meta count: expected 300, got %zu", meta.count);
        }

        /* 300 values = 2 full blocks + 1 partial of 44 */
        size_t expectedBlocks =
            (300 + VARINT_BP128_BLOCK_SIZE - 1) / VARINT_BP128_BLOCK_SIZE;
        if (meta.blockCount != expectedBlocks) {
            ERR("Meta blockCount: expected %zu, got %zu", expectedBlocks,
                meta.blockCount);
        }

        if (meta.encodedBytes != encodedBytes) {
            ERR("Meta encodedBytes: expected %zu, got %zu", encodedBytes,
                meta.encodedBytes);
        }

        /* Last block should be 300 % 128 = 44 */
        if (meta.lastBlockSize != 44) {
            ERR("Meta lastBlockSize: expected 44, got %zu", meta.lastBlockSize);
        }
    }

    TEST("BP128 stress test large array") {
        size_t count = 10000;
        uint32_t *values = malloc(count * sizeof(uint32_t));
        if (!values) {
            ERRR("malloc failed");
        }

        for (size_t i = 0; i < count; i++) {
            values[i] = (uint32_t)(i * 17 % 10000);
        }

        uint8_t *encoded = malloc(varintBP128MaxBytes(count));
        if (!encoded) {
            free(values);
            ERRR("malloc failed");
        }
        varintBP128Meta meta;
        varintBP128Encode32(encoded, values, count, &meta);

        if (meta.count != count) {
            ERR("Stress: meta.count=%zu", meta.count);
        }

        uint32_t *decoded = malloc(count * sizeof(uint32_t));
        if (!decoded) {
            free(values);
            free(encoded);
            ERRR("malloc failed");
        }
        size_t decodedCount = varintBP128Decode32(encoded, decoded, count);

        if (decodedCount != count) {
            ERR("Stress: decoded %zu values", decodedCount);
        }

        bool match = true;
        for (size_t i = 0; i < count && match; i++) {
            if (decoded[i] != values[i]) {
                ERR("Stress: mismatch at %zu", i);
                match = false;
            }
        }

        free(values);
        free(encoded);
        free(decoded);
    }

    TEST("BP128 delta stress test") {
        size_t count = 10000;
        uint32_t *values = malloc(count * sizeof(uint32_t));
        if (!values) {
            ERRR("malloc failed");
        }

        /* Sorted sequence with variable gaps */
        values[0] = 0;
        for (size_t i = 1; i < count; i++) {
            values[i] = values[i - 1] + (uint32_t)((i % 100) + 1);
        }

        uint8_t *encoded = malloc(varintBP128MaxBytes(count) + 1024);
        if (!encoded) {
            free(values);
            ERRR("malloc failed");
        }
        varintBP128Meta meta;
        size_t encodedBytes =
            varintBP128DeltaEncode32(encoded, values, count, &meta);

        uint32_t *decoded = malloc(count * sizeof(uint32_t));
        if (!decoded) {
            free(values);
            free(encoded);
            ERRR("malloc failed");
        }
        size_t decodedCount = varintBP128DeltaDecode32(encoded, decoded, count);

        if (decodedCount != count) {
            ERR("Delta stress: decoded %zu values", decodedCount);
        }

        bool match = true;
        for (size_t i = 0; i < count && match; i++) {
            if (decoded[i] != values[i]) {
                ERR("Delta stress: mismatch at %zu: expected %u, got %u", i,
                    values[i], decoded[i]);
                match = false;
            }
        }

        /* Should compress well due to small deltas */
        double ratio = (double)encodedBytes / (count * sizeof(uint32_t));
        if (ratio > 0.5) {
            ERR("Delta stress: poor compression ratio %.2f%%", ratio * 100);
        }

        free(values);
        free(encoded);
        free(decoded);
    }

    TEST("BP128 getCount utility") {
        /* varintBP128GetCount reads the count header from 64-bit encoding */
        uint64_t values[100];
        for (size_t i = 0; i < 100; i++) {
            values[i] = (uint64_t)i;
        }

        uint8_t encoded[2048];
        varintBP128Encode64(encoded, values, 100, NULL);

        size_t count = varintBP128GetCount(encoded, 2048);
        if (count != 100) {
            ERR("GetCount: expected 100, got %zu", count);
        }
    }

    TEST("BP128 64-bit boundary sizes") {
        size_t testSizes[] = {1, 64, 127, 128, 129, 256};

        for (size_t t = 0; t < sizeof(testSizes) / sizeof(testSizes[0]); t++) {
            size_t count = testSizes[t];
            uint64_t *values = malloc(count * sizeof(uint64_t));
            if (!values) {
                continue;
            }

            for (size_t i = 0; i < count; i++) {
                values[i] = (uint64_t)(i * 1000);
            }

            uint8_t *encoded = malloc(varintBP128MaxBytes(count) * 2);
            if (!encoded) {
                free(values);
                continue;
            }
            varintBP128Meta meta;
            varintBP128Encode64(encoded, values, count, &meta);

            uint64_t *decoded = malloc(count * sizeof(uint64_t));
            if (!decoded) {
                free(values);
                free(encoded);
                continue;
            }
            size_t decodedCount = varintBP128Decode64(encoded, decoded, count);

            if (decodedCount != count) {
                ERR("64-bit boundary size %zu: decoded %zu values", count,
                    decodedCount);
            }

            bool match = true;
            for (size_t i = 0; i < count && match; i++) {
                if (decoded[i] != values[i]) {
                    ERR("64-bit boundary size %zu: mismatch at %zu", count, i);
                    match = false;
                }
            }

            free(values);
            free(encoded);
            free(decoded);
        }
    }

    TEST("BP128 IsSorted edge cases") {
        /* Empty array */
        if (!varintBP128IsSorted32(NULL, 0)) {
            ERRR("Empty should be sorted");
        }

        /* Single element */
        const uint32_t single[] = {42};
        if (!varintBP128IsSorted32(single, 1)) {
            ERRR("Single should be sorted");
        }

        /* Equal elements */
        const uint32_t equal[] = {5, 5, 5, 5, 5};
        if (!varintBP128IsSorted32(equal, 5)) {
            ERRR("Equal elements should be sorted");
        }

        /* Strictly increasing */
        const uint32_t increasing[] = {1, 2, 3, 4, 5};
        if (!varintBP128IsSorted32(increasing, 5)) {
            ERRR("Increasing should be sorted");
        }

        /* Non-monotonic */
        const uint32_t nonMono[] = {1, 3, 2, 4, 5};
        if (varintBP128IsSorted32(nonMono, 5)) {
            ERRR("Non-monotonic should not be sorted");
        }

        /* 64-bit versions */
        const uint64_t sorted64[] = {1, 2, 3, 4, 5};
        if (!varintBP128IsSorted64(sorted64, 5)) {
            ERRR("64-bit sorted should be detected");
        }
    }

    TEST("BP128 mixed small and large values") {
        uint32_t values[256];
        for (size_t i = 0; i < 256; i++) {
            if (i < 128) {
                values[i] = (uint32_t)(i % 16); /* Small values, 4 bits */
            } else {
                values[i] = (uint32_t)(i * 1000); /* Larger values */
            }
        }

        uint8_t encoded[4096];
        varintBP128Meta meta;
        varintBP128Encode32(encoded, values, 256, &meta);

        uint32_t decoded[256];
        size_t decodedCount = varintBP128Decode32(encoded, decoded, 256);

        if (decodedCount != 256) {
            ERR("Mixed values: decoded %zu", decodedCount);
        }

        for (size_t i = 0; i < 256; i++) {
            if (decoded[i] != values[i]) {
                ERR("Mixed values: mismatch at %zu", i);
                break;
            }
        }
    }

    TEST_FINAL_RESULT;
    return 0;
}

#endif /* VARINT_BP128_TEST */
