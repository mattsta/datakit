#include "dj.h"
/* ====================================================================
 * datakit JSON
 * (SIMD escape scanning adapted from rapidjson MIT:
 *  https://github.com/Tencent/rapidjson/blob/master/include/rapidjson/writer.h)
 * ==================================================================== */
#if __SSE4_2__
#include <nmmintrin.h>
#endif

#if __SSE2__
#include <emmintrin.h>
#endif

/* ARM NEON detection - check all common macros */
#if defined(__aarch64__) || defined(__ARM_NEON) || defined(__ARM_NEON__)
#define DJ_USE_NEON 1
#include <arm_neon.h>
#endif

/* womp womp */
#ifdef _MSC_VER
#include <intrin.h>
#endif

/* default buffer length when allocated */
/* Note: NOT 512 because leave some room for mds overhead in allocation block */
#define DEFAULT_LEN 500

/* ====================================================================
 * Bitmap utilities
 * ==================================================================== */
/* We use a bitmap to track if we are generating key-value pairs or just
 * regular lists of things. (bits set to 1 mean the current level is a
 * map and requires ':' appended after every other element).
 *
 * We use this approach because if we have nested elements of nested elements
 * of nested elements, etc, then each time we enter a new deeper level, we need
 * to remember the type of of the previous container so we can properly close
 * the [] or {} as necessary.
 *
 * Using a bitmap for this approach (instead of alternative approaches like
 * having a full stack of enum ints) is the most memory efficient way to achieve
 * the hierarchical tracking behavior we need to successfully implement a
 * build-as-you-append JSON generator.
 */
#define BOOL_IDX_BITS_PER_SLOT (sizeof(DJ_BITMAP) * 8)
static void boolIdxSet(djState *state, size_t offset, bool value) {
    static const size_t bytesPerEntry = sizeof(*state->boolIdx.v);
    const size_t byte = offset / 8;
    const size_t vOffset = byte / bytesPerEntry;
    const DJ_BITMAP mask = 1ULL << (offset % BOOL_IDX_BITS_PER_SLOT);

    DJ_BITMAP *useBitmap =
        state->boolIdx.vBigExtent ? state->boolIdx.vBig : state->boolIdx.v;

    /* If offset is larger than our static array, check the allocated array. */
    static const size_t totalBytesInDefaultV =
        bytesPerEntry * COUNT_ARRAY(state->boolIdx.v);

    if (byte >= totalBytesInDefaultV) {
        /* If offset is larger than our extended array, we either need to
         * allocate the extended array or grow the extended array. */
        if (byte >= state->boolIdx.vBigExtent) {
            /* new entry is beyond current vBig extent, so EITHER:
             *   - first grow: copy v[2] to new vBig and extend
             *   - OR -
             *   - extend already-created vBig */
            if (!state->boolIdx.vBigExtent) {
                state->boolIdx.vBigExtent = vOffset * 2;

                /* Allocate the new vBig externally because it is in union with
                 * 'v', so we can't assign to .vBig AND copy .v into .vBig at
                 * the same time. */
                DJ_BITMAP *vBig =
                    zcalloc(state->boolIdx.vBigExtent, bytesPerEntry);
                memcpy(vBig, state->boolIdx.v, totalBytesInDefaultV);

                /* attach vBig where it belongs since v is copied into it now */
                state->boolIdx.vBig = vBig;
            } else {
                /* else, we already have vBig and need to grow it */
                state->boolIdx.vBigExtent = vOffset * 2;
                state->boolIdx.vBig =
                    zrealloc(state->boolIdx.vBig,
                             state->boolIdx.vBigExtent * bytesPerEntry);

                /* We don't need to zero out the new memory because of how we
                 * zero out each new value before setting it. */
            }

            useBitmap = state->boolIdx.vBig;
        }
    }

    if (value) {
        /* Set bit! (true value is enabled bit) */
        useBitmap[vOffset] |= mask;
    } else {
        /* Clear bit! (false value is zero bit) */
        useBitmap[vOffset] &= ~mask;
    }
}

DK_INLINE_ALWAYS bool boolIdxRead(djState *state, const size_t offset) {
    static const size_t bytesPerEntry = sizeof(*state->boolIdx.v);
    const size_t byte = offset / 8;
    const size_t vOffset = byte / bytesPerEntry;
    const DJ_BITMAP mask = 1ULL << (offset % BOOL_IDX_BITS_PER_SLOT);

    const DJ_BITMAP *useBitmap =
        state->boolIdx.vBigExtent ? state->boolIdx.vBig : state->boolIdx.v;

    /* kinda (really) bad if our offset is womped into a goober */
    assert(vOffset <
           (state->boolIdx.vBigExtent ?: COUNT_ARRAY(state->boolIdx.v)));

    /* If bitmap position is 0, this is an ARRAY currently.
     * If bitmap position is 1, this is a MAP / OBJECT currently. */
    return useBitmap[vOffset] & mask;
}

/* ====================================================================
 * Type of container we are inside
 * ==================================================================== */
typedef enum djType { DJ_ARRAY, DJ_MAP } djType;
_Static_assert(DJ_ARRAY == 0, "ARRAY must be 0!");
_Static_assert(DJ_MAP == 1, "MAP must be 1!");

/* ====================================================================
 * Helper macros
 * ==================================================================== */
/* Map element increment counting */
#define logElement()                                                           \
    do {                                                                       \
        state->count++;                                                        \
    } while (0)

/* Basic string writing */
#define cl(a, b)                                                               \
    do {                                                                       \
        state->s = mdscatlen(state->s, a, b);                                  \
    } while (0)
#define cstr(a)                                                                \
    do {                                                                       \
        state->s = mdscatlen(state->s, a, strlen(a));                          \
    } while (0)
#define clcheck(a, b)                                                          \
    do {                                                                       \
        state->s = mdscatlencheckcomma(state->s, a, b);                        \
    } while (0)

/* Type setting / getting */
#define typeAdd(type)                                                          \
    do {                                                                       \
        boolIdxSet(state, ++state->depth, type);                               \
    } while (0)
#define typeRemove()                                                           \
    do {                                                                       \
        assert(state->depth != 0);                                             \
        state->depth--;                                                        \
                                                                               \
        /* If new depth is a MAP again, we need to restart 'is key' counter */ \
        if (typeCurrent() == DJ_MAP) {                                         \
            state->count = 0;                                                  \
        }                                                                      \
    } while (0)
#if 0
#define typeCurrent()                                                          \
    ({                                                                         \
        const bool found = boolIdxRead(state, state->depth);                   \
        printf("[%zu] Current type is: %s\n", state->depth,                    \
               found ? "MAP" : "ARRAY");                                       \
        found;                                                                 \
    })
#else
#define typeCurrent() boolIdxRead(state, state->depth)
#endif

/* Open a MAP or ARRAY */
#define typeOpen(what, sigil)                                                  \
    do {                                                                       \
        cl(sigil, strlen(sigil));                                              \
        typeAdd(what);                                                         \
    } while (0)

/* Close a MAP or ARRAY */
#define typeClose(what, sigil)                                                 \
    do {                                                                       \
        clcheck(sigil, strlen(sigil));                                         \
        typeRemove();                                                          \
    } while (0)

/* Indicate whether value is key, and keys are only if MAP, else comma. */
#define IS_KEY (typeCurrent() == DJ_MAP ? (state->count % 2 == 0) : false)

/* ====================================================================
 * DJ Implementation
 * ==================================================================== */
void djInit(djState *state) {
    djInitWithBuffer(state, mdsemptylen(DEFAULT_LEN));

    /* Mark "preallocated length" as all free space. */
    mdsclear(state->s);
}

void djInitWithBuffer(djState *state, mds *buf) {
    assert(!state->s);
    assert(buf);
    state->s = buf;

    /* Default: no extended array and no extended array count */
    state->ss = NULL;
    state->ssCount = 0;
}

/* This is a CONSUME operation on 'src' and renders 'src' unusable after
 * all contents of 'src' are put into 'dst'. */
void djAppend(djState *dst, djState *src) {
    assert(dst);
    assert(src);
    assert(dst->s);

    /* If no source, don't append! */
    if (!src->s || mdslen(src->s) == 0) {
        djFree(src);
        return;
    }

    /* Gather all elements from 'src' */
    size_t srcStrCount = 0;
    mds **srcStr = djGetMulti(src, &srcStrCount);

    /* Count new total */
    const size_t newTotalCount = (dst->ssCount ?: 1) + srcStrCount;

    /* If current capacity isn't big enough, grow it. */
    if (!dst->ss) {
        dst->ssCapacity = newTotalCount * 4;
        dst->ss = zcalloc(dst->ssCapacity, sizeof(*dst->ss));
        dst->ss[0] = dst->s;
        dst->ssCount = 1;
    } else {
        if (dst->ssCount + srcStrCount > dst->ssCapacity) {
            dst->ssCapacity = newTotalCount * 2;
            dst->ss = zrealloc(dst->ss, sizeof(*dst->ss) * dst->ssCapacity);
        }
    }

    /* Append returned 'src' strings to our 'dst'-ness */
    for (size_t i = 0; i < srcStrCount; i++) {
        dst->ss[dst->ssCount + i] = srcStr[i];
    }

    /* 's' is always the tail write buffer */
    dst->s = dst->ss[newTotalCount - 1];
    dst->ssCount = newTotalCount;

    /* If count == 1, then there IS NO ARRAY we just received
     * a pointer to the 's' inside 'src'. */
    /* If count > 1, then 'srcStr' is an allocated array we need
     * to vacate if we appended to a 'dst'-owned array. */
    if (srcStrCount > 1) {
        zfree(srcStr);
    }
}

DK_INLINE_ALWAYS void djTrailingCommaCleanup(djState *state) {
    const ssize_t last = mdslen(state->s) - 1;
    if (last > 0 && state->s[last] == ',') {
        mdsIncrLen(state->s, -1);
        state->s[last] = '\0';
    }
}

/* During generation, you could consume the buffer to receive
 * the current output and return it to the user incrementally
 * until you reach a point to use djFinalize() */
/* Note: this will need a little more work before it works in
 *       the middle of live generations, because the auto-trailing-comma
 *       removal will do the wrong thing if we are mid-element and you
 *       get the output (which will have an IMPORTANT comma probably
 *       removed).
 *       Fix by doing depth checks maybe? */
mds *djConsumeBuffer(djState *state, const bool reset) {
    /* We maybe left a trailing comma,
     * so when stopping JSON writing, remove
     * the trailing comma! */

    djTrailingCommaCleanup(state);

    mds *hollaback = state->s;
    state->s = reset ? mdsemptylen(DEFAULT_LEN) : NULL;
    return hollaback;
}

mds *djFinalize(djState *state) {
    assert(state->ssCount == 0 &&
           "Can't regular-finalize with multiple buffers!");

    /* If we allocated a type-by-depth extent array, free it */
    if (state->boolIdx.vBigExtent) {
        zfree(state->boolIdx.vBig);
    }

    /* 'false' because we don't want another mds after finalize */
    /* It's the caller's responsibility to free the resulting mds */
    return djConsumeBuffer(state, false);
}

void djFree(djState *state) {
    if (state->boolIdx.vBigExtent) {
        zfree(state->boolIdx.vBig);
    }

    mdsfree(state->s);
}

mds **djGetMulti(djState *state, size_t *arrayCount) {
    *arrayCount = state->ssCount ?: 1;

    /* If we updated 'state->s' but didn't update tail previous, put it
     * back in the array (the existing tail entry in the array would
     * no longer be valid anyway). */
    if (state->ss && (state->ss[state->ssCount - 1] != state->s)) {
        state->ss[state->ssCount - 1] = state->s;
    }

    /* No other adjustments. Caller will manage the final djFinalize() call
     * with tail comma removal when necessary. */
    return state->ss ?: &state->s;
}

mds **djFinalizeMulti(djState *state, size_t *arrayCount) {
    /* If we allocated a type-by-depth extent array, free it */
    if (state->boolIdx.vBigExtent) {
        zfree(state->boolIdx.vBig);
    }

    djTrailingCommaCleanup(state);
    return djGetMulti(state, arrayCount);
}

/* ====================================================================
 * Data Operations
 * ==================================================================== */
void djMapOpen(djState *state) {
    typeOpen(DJ_MAP, "{");

    /* Clear any previous count.
     * If we are opening a nested map, we know the previous map
     * was closed in a clean state and can resume again from 0
     * (where 0 (and any even count) means WRITING KEY) */
    state->count = 0;
}

/* Auto-detect whether closing list or map and do the right thing */
void djCloseElement(djState *state) {
    if (typeCurrent() == DJ_MAP) {
        djMapCloseElement(state);
    } else {
        djArrayCloseElement(state);
    }
}

void djMapCloseElement(djState *state) {
    typeClose(DJ_MAP, "},");
}

void djMapCloseFinal(djState *state) {
    typeClose(DJ_MAP, "}");
}

void djSetOpen(djState *state) {
    typeOpen(DJ_ARRAY, "{");
    logElement();
}

void djSetCloseElement(djState *state) {
    typeClose(DJ_ARRAY, "},");
}

void djSetCloseFinal(djState *state) {
    typeClose(DJ_ARRAY, "}");
}

void djArrayOpen(djState *state) {
    typeOpen(DJ_ARRAY, "[");
    logElement();
}

void djArrayCloseElement(djState *state) {
    typeClose(DJ_ARRAY, "],");
}

void djArrayCloseFinal(djState *state) {
    typeClose(DJ_ARRAY, "]");
}

void djTrue(djState *state) {
#ifndef NDEBUG
    assert(!IS_KEY);
#endif

    /* JSON spec says true,false,null can't be keys */
    /* Only strings can be keys in JSON objects even
     * though JavaScript allows keys to be T/F/N/numeric/etc */
    cstr("true,");
    logElement();
}

void djFalse(djState *state) {
#ifndef NDEBUG
    assert(!IS_KEY);
#endif

    cstr("false,");
    logElement();
}

void djNULL(djState *state) {
#ifndef NDEBUG
    assert(!IS_KEY);
#endif

    cstr("null,");
    logElement();
}

/* ====================================================================
 * Write properly escaped strings
 * ==================================================================== */
/* TODO: we could potentially move SPLAT into djState so ALL writer functions
 *       can take advantage of the pre-mds buffer space.
 *       But we need to be careful of an edge case where we need to remove a
 *       trailing comma from the buffer, but the buffer was flushed to mds,
 *       so we can't see the comma in the buffer—it's over in the tail of the
 *       mds—so we'd have to check two places? (if splatWritten == 0, check tail
 *       of mds?) */
#define SPLATLEN 128
_Static_assert(
    SPLATLEN >= 16,
    "We need to reserve a minimum of 16 bytes at a time for SIMD operating!");

#define FLUSH_SPLAT(len)                                                       \
    do {                                                                       \
        cl(maxSplatWrite, len);                                                \
                                                                               \
        /* reset for more! */                                                  \
        *splatWritten = 0;                                                     \
    } while (0)

#define SPLAT_ENSURE(len)                                                      \
    do {                                                                       \
        if (*splatWritten + (len) > SPLATLEN) {                                \
            FLUSH_SPLAT(*splatWritten);                                        \
        }                                                                      \
    } while (0)

#define SPLAT(n)                                                               \
    do {                                                                       \
        assert(*splatWritten < SPLATLEN);                                      \
        maxSplatWrite[(*splatWritten)++] = (n);                                \
    } while (0)

#define SPLAT_OFFSET (maxSplatWrite + *splatWritten)
#define SPLAT_LEN(buf, len)                                                    \
    do {                                                                       \
        memcpy(SPLAT_OFFSET, buf, len);                                        \
        *splatWritten += len;                                                  \
    } while (0)

#if __SSE2__ || __SSE4_2__
static bool findNextEscapeByteSIMD(djState *state, const void *data_,
                                   size_t len, size_t *const processedLen,
                                   uint8_t maxSplatWrite[SPLATLEN],
                                   size_t *const splatWritten) {
    if (len < 16) {
        *processedLen = 0;
        return len != 0;
    }

    const uint8_t *data = data_;
    const void *end = data + len;
    const void *nextAligned =
        (void *)(((uintptr_t)data + 15) & (uintptr_t)(~15));
    const void *endAligned = (void *)(((uintptr_t)end) & (uintptr_t)(~15));

    if (nextAligned > end) {
        *processedLen = 0;
        return true;
    }

    /* zero out to allow incrementing */
    *processedLen = 0;

    SPLAT_ENSURE(16);
    while (data != nextAligned) {
        const uint8_t p = *data++;
        /* If weird character, return parsed length so we format externally. */
        if (p < 0x20 || p == '\"' || p == '\\') {
            return true;
        }

        /* else, byte is okay, so plop it directly. */
        SPLAT(p);
        (*processedLen)++;
    }

    /* Verify we are 16-byte aligned... */
    assert(((uintptr_t)data & 15) == 0);

    /* Now we are aligned and can do big things... */
    const __m128i dq = _mm_set1_epi8('\"');
    const __m128i bs = _mm_set1_epi8('\\');
    const __m128i sp = _mm_set1_epi8(0x1F);

    for (; data != endAligned; data += 16) {
        const __m128i s = _mm_load_si128((__m128i *)data);
        const __m128i t1 = _mm_cmpeq_epi8(s, dq);
        const __m128i t2 = _mm_cmpeq_epi8(s, bs);

        /* s < 0x20 <=> max(s, 0x1F) == 0x1F */
        const __m128i t3 = _mm_cmpeq_epi8(_mm_max_epu8(s, sp), sp);
        const __m128i x = _mm_or_si128(_mm_or_si128(t1, t2), t3);
        const uint16_t r = _mm_movemask_epi8(x);

        /* If mask populated, some elements require escaping... */
        if (r) {
#ifdef _MSC_VER // Find the index of first escaped
            unsigned long offset;
            _BitScanForward(&offset, r);
            size_t okayLen = offset;
#else
            size_t okayLen = __builtin_ffs(r) - 1;
#endif

            SPLAT_ENSURE(okayLen);
            SPLAT_LEN(data, okayLen);

            *processedLen += okayLen;
            break;
        }

        /* If we get here, none of the 16 bytes tested have forbidden
         * characters, so we can write them directly to the buffer. */

        /* Manually dump the 16 byte vector into splat then update splat length
         * ourselves since this isn't a common enough operation to warrant its
         * own macro. */
        SPLAT_ENSURE(16);
        _mm_storeu_si128((__m128i *)SPLAT_OFFSET, s);
        *splatWritten += 16;

        *processedLen += 16;
    }

    assert(*processedLen <= len);
    return *processedLen != len;
}
#elif DJ_USE_NEON
static bool findNextEscapeByteSIMD(djState *state, const void *data_,
                                   size_t len, size_t *const processedLen,
                                   uint8_t maxSplatWrite[SPLATLEN],
                                   size_t *const splatWritten) {
    if (len < 16) {
        *processedLen = 0;
        return len != 0;
    }

    const uint8_t *data = data_;
    const void *end = data + len;
    const void *nextAligned =
        (void *)(((uintptr_t)data + 15) & (uintptr_t)(~15));
    const void *endAligned = (void *)(((uintptr_t)end) & (uintptr_t)(~15));

    if (nextAligned > end) {
        *processedLen = 0;
        return true;
    }

    /* zero out to allow incrementing */
    *processedLen = 0;

    SPLAT_ENSURE(16);
    while (data != nextAligned) {
        const uint8_t p = *data++;
        /* If weird character, return parsed length so we format externally. */
        if (p < 0x20 || p == '\"' || p == '\\') {
            return true;
        }

        /* else, byte is okay, so plop it directly. */
        SPLAT(p);
        (*processedLen)++;
    }

    /* Verify we are 16-byte aligned... */
    assert(((uintptr_t)data & 15) == 0);

    /* Now we are aligned and can do big things... */
    /* Characters that need escaping: '"', '\\', and control chars < 0x20 */
    const uint8x16_t dq = vdupq_n_u8('"');
    const uint8x16_t bs = vdupq_n_u8('\\');
    const uint8x16_t sp = vdupq_n_u8(0x20); /* space = 32 */

    for (; data != endAligned; data += 16) {
        const uint8x16_t s = vld1q_u8(data);

        /* Check for '"' or '\\' */
        uint8x16_t x = vceqq_u8(s, dq);
        x = vorrq_u8(x, vceqq_u8(s, bs));

        /* Check for control characters: s < 0x20 */
        x = vorrq_u8(x, vcltq_u8(s, sp));

        /* Quick check: if no escape chars in this block, continue */
        if (vmaxvq_u8(x) == 0) {
            /* No escaping needed - copy all 16 bytes directly */
            SPLAT_ENSURE(16);
            vst1q_u8(SPLAT_OFFSET, s);
            *splatWritten += 16;
            *processedLen += 16;
            continue;
        }

        /* Found escape char - need to find exact position.
         * Reverse bytes within each 64-bit lane so we can use CLZ
         * to find the first set byte from the beginning. */
        x = vrev64q_u8(x);
        uint64_t low = vgetq_lane_u64(vreinterpretq_u64_u8(x), 0);
        uint64_t high = vgetq_lane_u64(vreinterpretq_u64_u8(x), 1);

        size_t okayLen;
        if (low != 0) {
            /* Escape char in first 8 bytes */
            uint32_t lz = __builtin_clzll(low);
            okayLen = lz >> 3; /* Convert bit position to byte position */
        } else {
            /* Escape char in second 8 bytes */
            uint32_t lz = __builtin_clzll(high);
            okayLen = 8 + (lz >> 3);
        }

        SPLAT_ENSURE(okayLen);
        SPLAT_LEN(data, okayLen);
        *processedLen += okayLen;
        break;
    }

    assert(*processedLen <= len);
    return *processedLen != len;
}
#else
/* else, no SIMD available, so use scalar loop to check bytes. */
static bool findNextEscapeByteSIMD(djState *state, const void *data_,
                                   size_t len, size_t *const processedLen,
                                   uint8_t maxSplatWrite[SPLATLEN],
                                   size_t *const splatWritten) {
    const uint8_t *data = data_;
    *processedLen = 0;

    while (*processedLen < len) {
        const uint8_t p = data[*processedLen];

        /* If character needs escaping, return so caller handles it. */
        if (p < 0x20 || p == '\"' || p == '\\') {
            return true;
        }

        /* Byte is okay, copy to splat buffer if space available. */
        SPLAT_ENSURE(1);
        SPLAT(p);
        (*processedLen)++;
    }

    /* Processed all bytes without finding escape character. */
    return false;
}
#endif

#define DJ_16_Z 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
void djString(djState *state, const void *data_, size_t len,
              const bool supportUTF8) {
    static const uint8_t hexDigits[16] = {'0', '1', '2', '3', '4', '5',
                                          '6', '7', '8', '9', 'A', 'B',
                                          'C', 'D', 'E', 'F'};
    /* Table of characters to use for escaping a single byte sequence.
     * If value is 'u', then value is written as hex digits formatted \u00XX */
    /* We shorten the table to save some cache pollution potentially at the cost
     * of a branch at every useage of escape_ */
    static const uint8_t escape_[] = {
        'u',     'u', 'u', 'u', 'u', 'u', 'u', 'u', 'b', 't', 'n', 'u', 'f',
        'r',     'u',
        'u', // 00
        'u',     'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u', 'u',
        'u',     'u',
        'u', // 10
        0,       0,   '"', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
        0,       0,
        0, // 20
        DJ_16_Z,
        DJ_16_Z, // 30~4F
        0,       0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   '\\',
    };
#if 0
  0,       0,       0, // 50
        DJ_16_Z, DJ_16_Z, DJ_16_Z, DJ_16_Z, DJ_16_Z, DJ_16_Z,
        DJ_16_Z, DJ_16_Z, DJ_16_Z, DJ_16_Z // 60~FF
#endif

    /* Access 'escape_' with a full byte without overflowing */
#define esc(x) ((x < sizeof(escape_)) ? escape_[x] : 0)

    /* We don't need this optimization at the moment. If profiling shows
     * mdscatlen is too slow, then re-visit allocating everything up front then
     * using just memory writes in-place. */
#if 0
    /* TODO: this up-front gross growing allocation means
     *       we can use "dirty writes" to the mds buffer, but we
     *       need to track the mds offset and mds total length
     *       for final update at the end.
     *
     *       Much better than calling "mdscatlen" for EACH byte
     *       write though. */
    if (supportUTF8) {
        state->s = mdsexpandby(state->s, 2 + len * 6); /* "\uxxxx..." */
    } else {
        state->s = mdsexpandby(state->s, 2 + len * 12); /* "\uxxxx\uyyyy..." */
    }
#endif

    const uint8_t *data = data_;
    size_t processedLen = 0;

    /* queue up to SPLATLEN bytes to write all at once instead of appending
     * byte-by-byte to the resulting output. */
    uint8_t maxSplatWrite[SPLATLEN];
    size_t splatWritten_ = 0;
    size_t *const splatWritten = &splatWritten_;

    /* Open quote for string */
    SPLAT('"');

    while (findNextEscapeByteSIMD(state, data, len, &processedLen,
                                  maxSplatWrite, splatWritten)) {
#if 0
        printf("Returned processedLen: %zu (len: %zu)\n", processedLen, len);
#endif
        assert(processedLen <= len);
        data += processedLen;
        len -= processedLen;

        const uint8_t next = data[0];
        const uint8_t escapeCharacter = esc(next);
        if (!supportUTF8 && next >= 0x80) {
            /* Skipping UTF8 validation here. Assuming sequence is just okay. */
            cstr("\\u");
            assert(NULL && "Unicode expansion validation not supported yet!");
        } else if (escapeCharacter) {
            /* Verify we can write up to 6 bytes without overflowing splat */
            SPLAT_ENSURE(6);

            SPLAT('\\');
            SPLAT(escapeCharacter);
            if (escapeCharacter == 'u') {
                SPLAT_LEN("00", 2);
                SPLAT(hexDigits[next >> 4]);
                SPLAT(hexDigits[next & 0x0F]);
            }
        } else {
            /* okay... nothing to escape found... */
            SPLAT_ENSURE(1);
            SPLAT(data[0]);
        }

        /* Next! */
        data++;
        len--;
    }

    /* Open space for our final three splat characters if necessary... */
    SPLAT_ENSURE(3);

    /* Close quote for string (and always end with a comma) */
    SPLAT('"');

    /* Separator appropriate for where we are in the JSON... */
    if (IS_KEY) {
        SPLAT(':');
    } else {
        SPLAT(',');
    }

    FLUSH_SPLAT(*splatWritten);

    logElement();
}

void djStringDirect(djState *state, const void *data, size_t len) {
    /* Place 'data' in quotes with a trailing indicator... */
    state->s = mdscatlenquoteraw(state->s, data, len, IS_KEY ? ':' : ',');
    logElement();
}

void djNumericDirect(djState *state, const void *data, size_t len) {
    if (IS_KEY) {
        /* JSON requires keys to be quoted strings, so add annoying
         * quotes to the number here. */
        /* TODO: parameterize something to still output unquoted
         * numbers for other json-adjacent formats allowing integer
         * keys like python format output, etc */
        state->s = mdscatlenquoteraw(state->s, data, len, ':');
    } else {
        /* Place 'data' without quotes but with a trailing comma... */
        state->s = mdscatlennoquoteraw(state->s, data, len, ',');
    }

    logElement();
}

void djBox(djState *state, const databox *const box) {
    uint8_t buf[40];
    size_t len = 0;
    switch (box->type) {
    case DATABOX_SIGNED_64:
        len = StrInt64ToBuf(&buf, sizeof(buf), box->data.i);
        goto writeNumberBytesDirectly;
    case DATABOX_PTR:
        /* fallthrough */
    case DATABOX_UNSIGNED_64:
        len = StrUInt64ToBuf(&buf, sizeof(buf), box->data.u);
        goto writeNumberBytesDirectly;
    case DATABOX_SIGNED_128:
        assert(box->big);
        len = StrInt128ToBuf(&buf, sizeof(buf), *box->data.i128);
        goto writeNumberBytesDirectly;
    case DATABOX_UNSIGNED_128:
        assert(box->big);
        len = StrUInt128ToBuf(&buf, sizeof(buf), *box->data.u128);
        goto writeNumberBytesDirectly;
    case DATABOX_FLOAT_32:
        len = StrDoubleFormatToBufNice(&buf, sizeof(buf), box->data.f32);
        goto writeNumberBytesDirectly;
    case DATABOX_DOUBLE_64:
        len = StrDoubleFormatToBufNice(&buf, sizeof(buf), box->data.d64);
        goto writeNumberBytesDirectly;
    case DATABOX_BYTES:
    case DATABOX_BYTES_EMBED:
    case DATABOX_PTR_MDSC:
        djString(state, databoxBytes(box), databoxLen(box), true);
        break;
    case DATABOX_TRUE:
        djTrue(state);
        break;
    case DATABOX_FALSE:
        djFalse(state);
        break;
    case DATABOX_NULL:
        djNULL(state);
        break;
    default:
        assert(NULL && "bad type for json implant?");
        __builtin_unreachable();
    }

    /* Don't fall through to writeNumber */
    return;

writeNumberBytesDirectly:
    djNumericDirect(state, buf, len);
    return;
}

#ifdef DATAKIT_TEST
#include "ctest.h"

/* Helper to run djString and get output for testing */
static mds *djTestEscapeString(const void *data, size_t len) {
    djState testState_ = {{0}};
    djState *testState = &testState_;
    djInit(testState);
    djString(testState, data, len, false);
    return djFinalize(testState);
}

/* Helper to build expected escape output using reference scalar implementation
 */
static mds *djBuildExpectedEscape(const uint8_t *data, size_t len) {
    mds *expected = mdsnew("\""); /* Open quote */
    for (size_t i = 0; i < len; i++) {
        const uint8_t c = data[i];
        if (c == '"') {
            expected = mdscatlen(expected, "\\\"", 2);
        } else if (c == '\\') {
            expected = mdscatlen(expected, "\\\\", 2);
        } else if (c == '\n') {
            expected = mdscatlen(expected, "\\n", 2);
        } else if (c == '\r') {
            expected = mdscatlen(expected, "\\r", 2);
        } else if (c == '\t') {
            expected = mdscatlen(expected, "\\t", 2);
        } else if (c == '\b') {
            expected = mdscatlen(expected, "\\b", 2);
        } else if (c == '\f') {
            expected = mdscatlen(expected, "\\f", 2);
        } else if (c < 0x20) {
            /* Control char: \u00XX */
            char hex[7];
            snprintf(hex, sizeof(hex), "\\u00%02X", c);
            expected = mdscatlen(expected, hex, 6);
        } else {
            expected = mdscatlen(expected, &c, 1);
        }
    }
    expected = mdscatlen(expected, "\"", 1); /* Close quote */
    return expected;
}

int djTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    TEST("true") {
        djState testA_ = {{0}};
        djState *testA = &testA_;
        djInit(testA);

        djTrue(testA);
        mds *out = djFinalize(testA);
        printf("%s\n", out);
        mdsfree(out);
    }

    TEST("false") {
        djState testA_ = {{0}};
        djState *testA = &testA_;
        djInit(testA);

        djFalse(testA);
        mds *out = djFinalize(testA);
        printf("%s\n", out);
        mdsfree(out);
    }

    TEST("null") {
        djState testA_ = {{0}};
        djState *testA = &testA_;
        djInit(testA);

        djNULL(testA);
        mds *out = djFinalize(testA);
        printf("%s\n", out);
        mdsfree(out);
    }

    TEST("hello - direct") {
        djState testA_ = {{0}};
        djState *testA = &testA_;
        djInit(testA);

        djStringDirect(testA, "hello", 5);
        mds *out = djFinalize(testA);
        printf("%s\n", out);
        mdsfree(out);
    }

    TEST("hello - escaped") {
        djState testA_ = {{0}};
        djState *testA = &testA_;
        djInit(testA);

        djString(testA, "hello", 5, false);
        mds *out = djFinalize(testA);
        printf("%s\n", out);
        mdsfree(out);
    }

    TEST("hello plus garbage - escaped") {
        djState testA_ = {{0}};
        djState *testA = &testA_;
        djInit(testA);

        djString(testA, "hello\n\n\t\t", 9, false);
        mds *out = djFinalize(testA);
        printf("%s\n", out);
        mdsfree(out);
    }

    TEST("hello plus garbage SIMD - escaped") {
        djState testA_ = {{0}};
        djState *testA = &testA_;
        djInit(testA);

        djArrayOpen(testA);
        mds *str = mdsnew("hello");
        for (size_t i = 0; i < 32; i++) {
            char *what = "\n";
            if (i % 2 == 0) {
                what = "\t";
            } else if (i % 3 == 0) {
                /* provide a long (>= 16 okay characters) string to trigger SIMD
                 */
                what = "hello there how are you today is this long enough";
            }

            str = mdscatlen(str, what, strlen(what));

            djString(testA, str, mdslen(str), false);
        }

        mdsfree(str);

        djArrayCloseFinal(testA);
        mds *out = djFinalize(testA);
        printf("%s\n", out);
        mdsfree(out);
    }

    TEST("simple map") {
        djState testA_ = {{0}};
        djState *testA = &testA_;
        djInit(testA);

        djMapOpen(testA);
        djString(testA, "hello", 5, false);
        djStringDirect(testA, "pickles", 6);
        djMapCloseFinal(testA);

        mds *out = djFinalize(testA);
        printf("%s\n", out);
        mdsfree(out);
    }

    TEST("simple two map") {
        djState testA_ = {{0}};
        djState *testA = &testA_;
        djInit(testA);

        djMapOpen(testA);
        djString(testA, "hello", 5, false);
        djStringDirect(testA, "pickles", 6);
        djString(testA, "two", 3, false);
        djStringDirect(testA, "map", 3);
        djMapCloseFinal(testA);

        mds *out = djFinalize(testA);
        printf("%s\n", out);
        mdsfree(out);
    }

    TEST("various map") {
        djState testA_ = {{0}};
        djState *testA = &testA_;
        djInit(testA);

        djMapOpen(testA);
        djString(testA, "hello", 5, false);
        djStringDirect(testA, "pickles", 6);
        djString(testA, "two", 3, false);
        djStringDirect(testA, "map", 3);
        djString(testA, "true", 4, false);
        djTrue(testA);
        djString(testA, "false", 5, false);
        djFalse(testA);
        djString(testA, "null", 4, false);
        djNULL(testA);
        djMapCloseFinal(testA);

        mds *out = djFinalize(testA);
        printf("%s\n", out);
        mdsfree(out);
    }

    TEST("various map nested") {
        djState testA_ = {{0}};
        djState *testA = &testA_;
        djInit(testA);

        djMapOpen(testA);
        djString(testA, "hello", 5, false);
        djStringDirect(testA, "pickles", 6);

        djStringDirect(testA, "DAMAP", 5);

        /* Stress bitmap by growing beyond its static allocation */
        /* (default is 2 static, so add 3 more to grow beyond) */
        static const size_t maxNesting = sizeof(DJ_BITMAP) * 8 * (2 + 3);

        for (size_t i = 0; i < maxNesting; i++) {
            if (random() % 2 == 0) {
                djMapOpen(testA);
            } else {
                djArrayOpen(testA);
            }

            djString(testA, "here's a key ya coward", 22, false);
            const uint32_t picker = random();
            for (size_t j = 0; j < 2; j++) {
                /* If the LAST LAST element, don't place anything because
                 * the j==1 is a key for the next map, but we won't
                 * have a next map. */
                if (j && i == maxNesting - 1) {
                    break;
                }

                if (!j && picker % 2 == 0) {
                    djTrue(testA);
                } else if (!j && picker % 3 == 0) {
                    djFalse(testA);
                } else if (!j && picker % 5 == 0) {
                    djNULL(testA);
                } else if (picker % 7 == 0) {
                    djString(testA, "wallby", 5, false);
                } else {
                    djStringDirect(testA, "pickledNonense", 14);
                }
            }
        }

        for (size_t i = 0; i < maxNesting; i++) {
            djString(testA, "finalizerA", 10, false);
            djStringDirect(testA, "there it is", 11);
            djCloseElement(testA);
        }

        djString(testA, "two", 3, false);
        djStringDirect(testA, "map", 3);
        djString(testA, "true", 4, false);
        djTrue(testA);
        djString(testA, "false", 5, false);
        djFalse(testA);
        djString(testA, "null", 4, false);
        djNULL(testA);
        djMapCloseFinal(testA);

        mds *out = djFinalize(testA);
        printf("%s\n", out);
        mdsfree(out);
    }

    TEST("simple array") {
        djState testA_ = {{0}};
        djState *testA = &testA_;
        djInit(testA);

        djArrayOpen(testA);
        djString(testA, "hello", 5, false);
        djStringDirect(testA, "pickles", 6);
        djArrayCloseFinal(testA);

        mds *out = djFinalize(testA);
        printf("%s\n", out);
        mdsfree(out);
    }

    TEST("simple two array") {
        djState testA_ = {{0}};
        djState *testA = &testA_;
        djInit(testA);

        djArrayOpen(testA);
        djString(testA, "hello", 5, false);
        djStringDirect(testA, "pickles", 6);
        djString(testA, "two", 3, false);
        djStringDirect(testA, "map", 3);
        djArrayCloseFinal(testA);

        mds *out = djFinalize(testA);
        printf("%s\n", out);
        mdsfree(out);
    }

    TEST("various array") {
        djState testA_ = {{0}};
        djState *testA = &testA_;
        djInit(testA);

        djArrayOpen(testA);
        djString(testA, "hello", 5, false);
        djStringDirect(testA, "pickles", 6);
        djString(testA, "two", 3, false);
        djStringDirect(testA, "map", 3);
        djString(testA, "true", 4, false);
        djTrue(testA);
        djString(testA, "false", 5, false);
        djFalse(testA);
        djString(testA, "null", 4, false);
        djNULL(testA);
        djArrayCloseFinal(testA);

        mds *out = djFinalize(testA);
        printf("%s\n", out);
        mdsfree(out);
    }

    TEST("various nesting mechanics") {
        djState testA_ = {{0}};
        djState *testA = &testA_;
        djInit(testA);

        {
            djMapOpen(testA);
            djStringDirect(testA, "b", 1);
            {
                djMapOpen(testA);
                djStringDirect(testA, "c", 1);
                {
                    djMapOpen(testA);
                    djStringDirect(testA, "d", 1);
                    {
                        djMapOpen(testA);
                        djStringDirect(testA, "e", 1);
                        {
                            djMapOpen(testA);
                            djStringDirect(testA, "f", 1);
                            {
                                djMapOpen(testA);
                                djStringDirect(testA, "g", 1);
                                {
                                    djMapOpen(testA);
                                    djStringDirect(testA, "a", 1);
                                    djStringDirect(testA, "b", 1);
                                    djMapCloseElement(testA);
                                }
                                djMapCloseElement(testA);
                            }
                        }
                        djMapCloseElement(testA);

                        djStringDirect(testA, "mine", 4);
                        djArrayOpen(testA);
                        djStringDirect(testA, "e", 1);
                        djStringDirect(testA, "f", 1);
                        djStringDirect(testA, "g", 1);
                        djArrayCloseElement(testA);

                        djStringDirect(testA, "q", 1);
                        djSetOpen(testA);
                        djStringDirect(testA, "e", 1);
                        djStringDirect(testA, "f", 1);
                        djStringDirect(testA, "g", 1);
                        djSetCloseElement(testA);
                    }
                    djMapCloseElement(testA);
                }
                djMapCloseElement(testA);
            }
            djMapCloseElement(testA);
        }
        djMapCloseFinal(testA);

        mds *out = djFinalize(testA);
        printf("%s\n", out);
        mdsfree(out);
    }

    TEST("SIMD escape detection - comprehensive stress test") {
        printf("Testing escape detection with various string lengths...\n");
        uint32_t testCount = 0;
        uint32_t passCount = 0;

        /* Test 1: Strings with NO escape characters at various lengths */
        for (size_t len = 1; len <= 100; len++) {
            uint8_t *data = zmalloc(len);
            for (size_t i = 0; i < len; i++) {
                /* Use printable ASCII chars that don't need escaping */
                data[i] = 'A' + (i % 26);
            }

            mds *result = djTestEscapeString(data, len);
            mds *expected = djBuildExpectedEscape(data, len);

            testCount++;
            if (mdslen(result) == mdslen(expected) &&
                memcmp(result, expected, mdslen(result)) == 0) {
                passCount++;
            } else {
                printf("FAIL len=%zu no-escape: got [%s] expected [%s]\n", len,
                       result, expected);
            }

            mdsfree(result);
            mdsfree(expected);
            zfree(data);
        }

        /* Test 2: Single escape char at each position for various lengths */
        const uint8_t escapeChars[] = {'"', '\\', '\n', '\t', '\r', 0, 1, 0x1F};
        for (size_t ec = 0; ec < sizeof(escapeChars); ec++) {
            for (size_t len = 1; len <= 64; len++) {
                for (size_t pos = 0; pos < len; pos++) {
                    uint8_t *data = zmalloc(len);
                    for (size_t i = 0; i < len; i++) {
                        data[i] = 'A' + (i % 26);
                    }
                    data[pos] = escapeChars[ec];

                    mds *result = djTestEscapeString(data, len);
                    mds *expected = djBuildExpectedEscape(data, len);

                    testCount++;
                    if (mdslen(result) == mdslen(expected) &&
                        memcmp(result, expected, mdslen(result)) == 0) {
                        passCount++;
                    } else {
                        printf("FAIL esc=0x%02X len=%zu pos=%zu: "
                               "got [%s] expected [%s]\n",
                               escapeChars[ec], len, pos, result, expected);
                    }

                    mdsfree(result);
                    mdsfree(expected);
                    zfree(data);
                }
            }
        }

        /* Test 3: Multiple escape chars in same string */
        for (size_t len = 16; len <= 64; len += 8) {
            uint8_t *data = zmalloc(len);
            for (size_t i = 0; i < len; i++) {
                data[i] = 'X';
            }
            /* Put escapes at SIMD boundaries */
            if (len > 0) {
                data[0] = '"';
            }
            if (len > 15) {
                data[15] = '\\';
            }
            if (len > 16) {
                data[16] = '\n';
            }
            if (len > 31) {
                data[31] = '\t';
            }
            if (len > 32) {
                data[32] = 0x01;
            }

            mds *result = djTestEscapeString(data, len);
            mds *expected = djBuildExpectedEscape(data, len);

            testCount++;
            if (mdslen(result) == mdslen(expected) &&
                memcmp(result, expected, mdslen(result)) == 0) {
                passCount++;
            } else {
                printf("FAIL multi-escape len=%zu: got [%s] expected [%s]\n",
                       len, result, expected);
            }

            mdsfree(result);
            mdsfree(expected);
            zfree(data);
        }

        /* Test 4: Long strings with escape at specific SIMD boundary positions
         */
        const size_t boundaryPos[] = {0,  1,  15, 16, 17, 31,
                                      32, 33, 47, 48, 63, 64};
        for (size_t bp = 0; bp < sizeof(boundaryPos) / sizeof(boundaryPos[0]);
             bp++) {
            const size_t pos = boundaryPos[bp];
            const size_t len = 128;

            uint8_t *data = zmalloc(len);
            for (size_t i = 0; i < len; i++) {
                data[i] = 'Y';
            }
            data[pos] = '\\';

            mds *result = djTestEscapeString(data, len);
            mds *expected = djBuildExpectedEscape(data, len);

            testCount++;
            if (mdslen(result) == mdslen(expected) &&
                memcmp(result, expected, mdslen(result)) == 0) {
                passCount++;
            } else {
                printf("FAIL boundary pos=%zu: got [%s] expected [%s]\n", pos,
                       result, expected);
            }

            mdsfree(result);
            mdsfree(expected);
            zfree(data);
        }

        /* Test 5: All control characters (0x00-0x1F) */
        for (uint8_t ctrl = 0; ctrl < 0x20; ctrl++) {
            const size_t len = 32;
            uint8_t *data = zmalloc(len);
            for (size_t i = 0; i < len; i++) {
                data[i] = 'Z';
            }
            data[17] = ctrl; /* Position past first SIMD block */

            mds *result = djTestEscapeString(data, len);
            mds *expected = djBuildExpectedEscape(data, len);

            testCount++;
            if (mdslen(result) == mdslen(expected) &&
                memcmp(result, expected, mdslen(result)) == 0) {
                passCount++;
            } else {
                printf("FAIL ctrl=0x%02X: got [%s] expected [%s]\n", ctrl,
                       result, expected);
            }

            mdsfree(result);
            mdsfree(expected);
            zfree(data);
        }

        /* Test 6: Random stress test */
        srandom(12345); /* Fixed seed for reproducibility */
        for (uint32_t trial = 0; trial < 1000; trial++) {
            const size_t len = 1 + (random() % 200);
            uint8_t *data = zmalloc(len);

            for (size_t i = 0; i < len; i++) {
                /* Mix of printable and escape chars */
                const uint32_t r = random() % 100;
                if (r < 85) {
                    data[i] = 'A' + (random() % 26);
                } else if (r < 90) {
                    data[i] = '"';
                } else if (r < 95) {
                    data[i] = '\\';
                } else {
                    data[i] = random() % 0x20; /* Control char */
                }
            }

            mds *result = djTestEscapeString(data, len);
            mds *expected = djBuildExpectedEscape(data, len);

            testCount++;
            if (mdslen(result) == mdslen(expected) &&
                memcmp(result, expected, mdslen(result)) == 0) {
                passCount++;
            } else {
                printf("FAIL random trial=%u len=%zu\n", trial, len);
                /* Don't print full strings - could be huge */
            }

            mdsfree(result);
            mdsfree(expected);
            zfree(data);
        }

        printf("SIMD escape detection: %u/%u tests passed\n", passCount,
               testCount);
        assert(passCount == testCount);
    }

    return 0;
}

#endif
