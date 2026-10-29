#include "dj.h"

#include <math.h> /* isfinite */
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
#ifdef __ARM_NEON
#define DJ_USE_NEON 1
#include <arm_neon.h>
#endif

/* default buffer length when allocated */
/* Note: NOT 512 because leave some room for mds overhead in allocation block */
static constexpr int DEFAULT_LEN = 500;

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
    static constexpr size_t bytesPerEntry = sizeof(*state->boolIdx.v);
    const size_t vOffset = offset / BOOL_IDX_BITS_PER_SLOT;
    const DJ_BITMAP mask = 1ULL << (offset % BOOL_IDX_BITS_PER_SLOT);

    DJ_BITMAP *useBitmap =
        state->boolIdx.vBigExtent ? state->boolIdx.vBig : state->boolIdx.v;

    /* Extents count entries: the inline array, or vBig once allocated. */
    static constexpr size_t totalBytesInDefaultV =
        bytesPerEntry * countof(state->boolIdx.v);
    const size_t extent = state->boolIdx.vBigExtent ? state->boolIdx.vBigExtent
                                                    : countof(state->boolIdx.v);

    if (vOffset >= extent) {
        /* The entry is past the current extent, so EITHER:
         *   - first grow: copy v[2] to a new vBig and extend
         *   - OR -
         *   - extend the existing vBig */
        if (!state->boolIdx.vBigExtent) {
            state->boolIdx.vBigExtent = vOffset * 2;

            /* Allocate the new vBig externally because it is in union with
             * 'v', so we can't assign to .vBig AND copy .v into .vBig at
             * the same time. */
            /* @own: container; free=zfree */
            DJ_BITMAP *vBig = zcalloc(state->boolIdx.vBigExtent, bytesPerEntry);
            memcpy(vBig, state->boolIdx.v, totalBytesInDefaultV);

            /* attach vBig where it belongs since v is copied into it now */
            state->boolIdx.vBig = vBig;
        } else {
            state->boolIdx.vBigExtent = vOffset * 2;
            /* @own: container; free=zfree */
            state->boolIdx.vBig = zrealloc(
                state->boolIdx.vBig, state->boolIdx.vBigExtent * bytesPerEntry);

            /* The new entries need no zeroing: every depth's bit is set
             * when that depth is entered, before it is read. */
        }

        useBitmap = state->boolIdx.vBig;
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
    const size_t vOffset = offset / BOOL_IDX_BITS_PER_SLOT;
    const DJ_BITMAP mask = 1ULL << (offset % BOOL_IDX_BITS_PER_SLOT);

    const DJ_BITMAP *useBitmap =
        state->boolIdx.vBigExtent ? state->boolIdx.vBig : state->boolIdx.v;

    /* kinda (really) bad if our offset is womped into a goober */
    assert(vOffset < (state->boolIdx.vBigExtent ?: countof(state->boolIdx.v)));

    /* If bitmap position is 0, this is an ARRAY currently.
     * If bitmap position is 1, this is a MAP / OBJECT currently. */
    return useBitmap[vOffset] & mask;
}

/* ====================================================================
 * Type of container we are inside
 * ==================================================================== */
typedef enum djType { DJ_ARRAY, DJ_MAP } djType;
static_assert(DJ_ARRAY == 0, "ARRAY must be 0!");
static_assert(DJ_MAP == 1, "MAP must be 1!");

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
/* An empty buffer with DEFAULT_LEN bytes of room */
static mds *djFreshBuffer_(void) {
    /* @own: caller; free=mdsfree */
    mds *s = mdsemptylen(DEFAULT_LEN);
    mdsclear(s); /* the length is room, not content */
    return s;
}

void djInit(djState *state) {
    /* @own: container; free=mdsfree */
    djInitWithBuffer(state, djFreshBuffer_());
}

void djInitWithBuffer(djState *state, mds *buf) {
    assert(!state->s);
    assert(buf);
    state->s = buf;

    /* Default: no extended array and no extended array count */
    state->ss = nullptr;
    state->ssCount = 0;
}

/* Free the depth bitmap if it outgrew the inline array. */
static void djReleaseDepthBitmap(djState *state) {
    if (state->boolIdx.vBigExtent) {
        zfree(state->boolIdx.vBig);
    }

    memset(&state->boolIdx, 0, sizeof(state->boolIdx));
}

void djAppend(djState *dst, djState *src) {
    assert(dst);
    assert(src);
    assert(dst->s);

    /* If no source, don't append! */
    if (!src->s || mdslen(src->s) == 0) {
        djFree(src);
        return;
    }

    /* Gather all elements from 'src'; its depth state is no longer needed */
    size_t srcStrCount = 0;
    mds **srcStr = djGetMulti(src, &srcStrCount);
    djReleaseDepthBitmap(src);

    if (!dst->ss) {
        dst->ssCapacity = (1 + srcStrCount) * 4;
        /* @own: container; free=zfree */
        dst->ss = zcalloc(dst->ssCapacity, sizeof(*dst->ss));
        dst->ss[0] = dst->s;
        dst->ssCount = 1;
    } else {
        /* Writes since the last append may have moved the tail buffer. */
        dst->ss[dst->ssCount - 1] = dst->s;
        if (dst->ssCount + srcStrCount > dst->ssCapacity) {
            dst->ssCapacity = (dst->ssCount + srcStrCount) * 2;
            /* @own: container; free=zfree */
            dst->ss = zrealloc(dst->ss, sizeof(*dst->ss) * dst->ssCapacity);
        }
    }

    for (size_t i = 0; i < srcStrCount; i++) {
        dst->ss[dst->ssCount + i] = srcStr[i];
    }

    /* 's' is always the tail write buffer */
    dst->ssCount += srcStrCount;
    dst->s = dst->ss[dst->ssCount - 1];

    /* The buffers now belong to 'dst'; an array 'src' allocated is
     * vacated. */
    if (src->ss) {
        zfree(src->ss);
    }

    memset(src, 0, sizeof(*src));
}

/* Remove the separator every element leaves after itself; true when there
 * was one */
DK_INLINE_ALWAYS bool djTrailingCommaCleanup(djState *state) {
    const ssize_t last = mdslen(state->s) - 1;
    if (last >= 0 && state->s[last] == ',') {
        mdsIncrLen(state->s, -1);
        state->s[last] = '\0';
        return true;
    }

    return false;
}

/* The output so far, without its trailing separator.  With 'reset' the
 * state writes on into a fresh buffer that starts with the separator held
 * back, so the next element follows it and a close replaces it, exactly
 * as in one buffer: the chunks joined are the document. */
mds *djConsumeBuffer(djState *state, const bool reset) {
    assert(state->ssCount == 0 && "Can't consume one of multiple buffers!");
    const bool separator = djTrailingCommaCleanup(state);

    mds *out = state->s;
    state->s = nullptr;
    if (reset) {
        /* @own: container; free=mdsfree */
        state->s = djFreshBuffer_();
        if (separator) {
            state->s = mdscatlen(state->s, ",", 1);
        }
    }

    return out;
}

mds *djFinalize(djState *state) {
    assert(state->ssCount == 0 &&
           "Can't regular-finalize with multiple buffers!");

    djReleaseDepthBitmap(state);

    /* 'false' because we don't want another mds after finalize */
    /* It's the caller's responsibility to free the resulting mds */
    mds *out = djConsumeBuffer(state, false);
    memset(state, 0, sizeof(*state));
    return out;
}

void djFree(djState *state) {
    djReleaseDepthBitmap(state);

    /* The array's tail entry can lag 's', so 's' is freed in its place. */
    if (state->ss) {
        for (size_t i = 0; i + 1 < state->ssCount; i++) {
            mdsfree(state->ss[i]);
        }

        zfree(state->ss);
    }

    mdsfree(state->s);
    memset(state, 0, sizeof(*state));
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
    djReleaseDepthBitmap(state);
    djTrailingCommaCleanup(state);

    mds **out = state->ss;
    if (out) {
        out[state->ssCount - 1] = state->s;
        *arrayCount = state->ssCount;
    } else {
        /* @own: caller; free=zfree */
        out = zmalloc(sizeof(*out));
        out[0] = state->s;
        *arrayCount = 1;
    }

    memset(state, 0, sizeof(*state));
    return out;
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
static constexpr int SPLATLEN = 128;
static_assert(
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

/* Whether the scan stops at byte 'p': '"', '\\' and control bytes always;
 * bytes from 0x80 up (decoded as UTF-8 by the caller) when 'high' is
 * 0x80, never when it is 0 (text already known to be valid UTF-8, copied
 * as it is) */
DK_INLINE_ALWAYS bool djScanStops_(const uint8_t p, const uint8_t high) {
    return p < 0x20 || p == '\"' || p == '\\' || (p & high);
}

/* The stops among the 16 bytes at 'p' as a mask (0: none), and the index
 * of the first stop in a nonzero mask.  The scan tests the mask before it
 * uses the index, so each block's load waits only on a predicted branch,
 * never on the previous block's mask. */
#if __SSE2__ || __SSE4_2__
#define DJ_SCAN_BLOCK 16
DK_INLINE_ALWAYS uint64_t djBlockStops_(const uint8_t *p, const uint8_t high) {
    const __m128i s = _mm_loadu_si128((const __m128i *)p);
    /* s < 0x20 <=> max(s, 0x1F) == 0x1F; a byte's own top bit, kept by
     * 'high', marks 0x80 and up for the movemask */
    const __m128i sp = _mm_set1_epi8(0x1F);
    const __m128i x =
        _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(s, _mm_set1_epi8('\"')),
                                  _mm_cmpeq_epi8(s, _mm_set1_epi8('\\'))),
                     _mm_or_si128(_mm_cmpeq_epi8(_mm_max_epu8(s, sp), sp),
                                  _mm_and_si128(s, _mm_set1_epi8((char)high))));
    return (uint32_t)_mm_movemask_epi8(x);
}

DK_INLINE_ALWAYS uint32_t djFirstStop_(const uint64_t stops) {
    return (uint32_t)stdc_trailing_zeros(stops);
}
#elif DJ_USE_NEON
#define DJ_SCAN_BLOCK 16
DK_INLINE_ALWAYS uint64_t djBlockStops_(const uint8_t *p, const uint8_t high) {
    const uint8x16_t s = vld1q_u8(p);
    uint8x16_t x = vceqq_u8(s, vdupq_n_u8('"'));
    x = vorrq_u8(x, vceqq_u8(s, vdupq_n_u8('\\')));
    x = vorrq_u8(x, vcltq_u8(s, vdupq_n_u8(0x20)));
    x = vorrq_u8(x, vtstq_u8(s, vdupq_n_u8(high)));
    /* Four bits per byte, in byte order */
    return vget_lane_u64(
        vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(x), 4)), 0);
}

DK_INLINE_ALWAYS uint32_t djFirstStop_(const uint64_t stops) {
    return (uint32_t)stdc_trailing_zeros(stops) / 4;
}
#else
#define DJ_SCAN_BLOCK 0
#endif

/* Copy the bytes at 'data_' that need no escaping into the splat, up to
 * the first that does or the end: '*processedLen' bytes; true when it
 * stopped at one */
DK_INLINE_ALWAYS bool findNextEscapeByteSIMD(djState *state, const void *data_,
                                             size_t len,
                                             size_t *const processedLen,
                                             uint8_t maxSplatWrite[SPLATLEN],
                                             size_t *const splatWritten,
                                             const uint8_t high) {
    const uint8_t *data = data_;
    size_t done = 0;
#if DJ_SCAN_BLOCK
    while (len - done >= DJ_SCAN_BLOCK) {
        const uint64_t stops = djBlockStops_(data + done, high);
        /* The whole block is copied (one fixed-size store); only its
         * clean bytes count */
        SPLAT_ENSURE(DJ_SCAN_BLOCK);
        memcpy(SPLAT_OFFSET, data + done, DJ_SCAN_BLOCK);
        if (stops) {
            const uint32_t clean = djFirstStop_(stops);
            *splatWritten += clean;
            *processedLen = done + clean;
            return true;
        }

        *splatWritten += DJ_SCAN_BLOCK;
        done += DJ_SCAN_BLOCK;
    }
#endif

    while (done < len) {
        if (djScanStops_(data[done], high)) {
            *processedLen = done;
            return true;
        }

        SPLAT_ENSURE(1);
        SPLAT(data[done]);
        done++;
    }

    *processedLen = done;
    return false;
}

#define DJ_16_Z 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
void djString(djState *state, const void *data_, size_t len,
              const bool supportUTF8) {
    static constexpr uint8_t hexDigits[16] = {'0', '1', '2', '3', '4', '5',
                                              '6', '7', '8', '9', 'A', 'B',
                                              'C', 'D', 'E', 'F'};
    /* Table of characters to use for escaping a single byte sequence.
     * If value is 'u', then value is written as hex digits formatted \u00XX */
    /* We shorten the table to save some cache pollution potentially at the cost
     * of a branch at every usage of escape_ */
    static constexpr uint8_t escape_[] = {
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

    /* \uXXXX for the UTF-16 code unit 'unit' */
#define DJ_SPLAT_U16_(unit)                                                    \
    do {                                                                       \
        SPLAT('\\');                                                           \
        SPLAT('u');                                                            \
        SPLAT(hexDigits[((unit) >> 12) & 0x0F]);                               \
        SPLAT(hexDigits[((unit) >> 8) & 0x0F]);                                \
        SPLAT(hexDigits[((unit) >> 4) & 0x0F]);                                \
        SPLAT(hexDigits[(unit) & 0x0F]);                                       \
    } while (0)

    /* Bytes from 0x80 up stop the scan.  Kept as UTF-8, the first one
     * validates the rest of the text in one vector pass, and valid text is
     * then copied without stopping; otherwise each stops for the decode
     * below.  ASCII text never pays the validation. */
    uint8_t high = 0x80;
    bool validated = !supportUTF8;

    /* Open quote for string */
    SPLAT('"');

    while (findNextEscapeByteSIMD(state, data, len, &processedLen,
                                  maxSplatWrite, splatWritten, high)) {
        assert(processedLen <= len);
        data += processedLen;
        len -= processedLen;

        const uint8_t next = data[0];
        const uint8_t escapeCharacter = esc(next);
        size_t consumed = 1;
        if (next >= 0x80 && !validated) {
            validated = true;
            if (StrUtf8Valid(data, len)) {
                high = 0;
                continue; /* scan on from this byte, copying */
            }
        }

        if (next >= 0x80) {
            /* The decode is the validation: a whole valid sequence is one
             * character, copied or escaped; any other byte is one U+FFFD.
             * A run of characters from 0x80 up is decoded here. */
            consumed = 0;
            do {
                const uint8_t *at = data + consumed;
                uint32_t cp = 0;
                const size_t seq = StrUtf8DecodeOne(at, len - consumed, &cp);
                if (!seq) {
                    cp = 0xFFFD;
                }

                SPLAT_ENSURE(12);
                if (supportUTF8) {
                    if (seq && len - consumed >= 4) {
                        /* One fixed-size copy; the sequence's bytes count */
                        memcpy(SPLAT_OFFSET, at, 4);
                        *splatWritten += seq;
                    } else if (seq) {
                        SPLAT_LEN(at, seq);
                    } else {
                        SPLAT_LEN("\xef\xbf\xbd", 3);
                    }
                } else if (cp < 0x10000) {
                    DJ_SPLAT_U16_(cp);
                } else {
                    /* Above the BMP: a surrogate pair */
                    const uint32_t v = cp - 0x10000;
                    DJ_SPLAT_U16_(0xD800 + (v >> 10));
                    DJ_SPLAT_U16_(0xDC00 + (v & 0x3FF));
                }

                consumed += seq ? seq : 1;
            } while (consumed < len && data[consumed] >= 0x80);
        } else {
            /* The scan stops below 0x80 only at bytes with an escape */
            assert(escapeCharacter);
            SPLAT_ENSURE(6);
            if (escapeCharacter == 'u') {
                DJ_SPLAT_U16_(next);
            } else {
                SPLAT('\\');
                SPLAT(escapeCharacter);
            }
        }

        /* Next! */
        data += consumed;
        len -= consumed;
    }
#undef DJ_SPLAT_U16_

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

bool djBox(djState *state, const databox *const box) {
    uint8_t buf[48]; /* StrInt128ToBuf needs 41 */
    size_t len = 0;
    const bool isKey = IS_KEY;
    switch (box->type) {
    case DATABOX_SIGNED_64:
        len = StrInt64ToBuf(&buf, sizeof(buf), box->data.i);
        goto writeNumberBytesDirectly;
    case DATABOX_PTR:
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
    case DATABOX_DOUBLE_64: {
        const double d = box->type == DATABOX_FLOAT_32 ? (double)box->data.f32
                                                       : box->data.d64;
        /* A key is quoted, so "nan" or "inf" is still a valid key; a
         * value has no JSON form. */
        if (!isfinite(d) && !isKey) {
            return false;
        }

        len = StrDoubleFormatToBufNice(&buf, sizeof(buf), d);
        goto writeNumberBytesDirectly;
    }
    case DATABOX_BYTES:
    case DATABOX_BYTES_EMBED:
    case DATABOX_BYTES_NEVER_FREE:
    case DATABOX_PTR_MDSC:
        djString(state, databoxBytes(box), databoxLen(box), true);
        return true;
    case DATABOX_TRUE:
    case DATABOX_FALSE:
    case DATABOX_NULL: {
        static const char *const words[] = {"true", "false", "null"};
        const char *word = words[box->type - DATABOX_TRUE];
        if (isKey) {
            /* Keys are strings, as numeric keys are quoted */
            djStringDirect(state, word, strlen(word));
        } else if (box->type == DATABOX_TRUE) {
            djTrue(state);
        } else if (box->type == DATABOX_FALSE) {
            djFalse(state);
        } else {
            djNULL(state);
        }

        return true;
    }
    default:
        return false;
    }

writeNumberBytesDirectly:
    djNumericDirect(state, buf, len);
    return true;
}

#ifdef DATAKIT_TEST
#include "cleaner.h"
#include "ctest.h"
#include "dkRandom.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(uint8_t *, zfree, U8);
defcleaner(mds *, mdsfree);

/* Concatenate output buffers in order */
static mds *djTestJoin(mds *const *parts, size_t count) {
    /* @own: caller; free=mdsfree */
    mds *joined = mdsempty();
    for (size_t i = 0; i < count; i++) {
        joined = mdscatlen(joined, parts[i], mdslen(parts[i]));
    }

    return joined;
}

static bool djTestIs(const mds *out, const char *want) {
    return mdslen(out) == strlen(want) && memcmp(out, want, mdslen(out)) == 0;
}

/* The four hex digits at 'p' */
static uint32_t djTestHex4_(const char *p) {
    uint32_t u = 0;
    for (size_t h = 0; h < 4; h++) {
        const char c = p[h];
        u = u * 16 + (uint32_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }

    return u;
}

/* The bytes the JSON string literal 'lit' (quotes included) stands for:
 * every escape read back, surrogate pairs joined, as UTF-8; NULL for a
 * malformed literal */
static mds *djTestUnescape_(const mds *lit) {
    const size_t n = mdslen(lit);
    if (n < 2 || lit[0] != '"' || lit[n - 1] != '"') {
        return nullptr;
    }

    /* @own: caller; free=mdsfree */
    mds *out = mdsempty();
    for (size_t i = 1; i + 1 < n; i++) {
        if (lit[i] != '\\') {
            out = mdscatlen(out, &lit[i], 1);
            continue;
        }

        if (i + 2 >= n) {
            mdsfree(out);
            return nullptr;
        }

        const char e = lit[++i];
        static const char simple[] = "\"\\/bfnrt";
        static const char meant[] = "\"\\/\b\f\n\r\t";
        const char *at = strchr(simple, e);
        if (at && e) {
            out = mdscatlen(out, &meant[at - simple], 1);
            continue;
        }

        if (e != 'u' || i + 4 >= n) {
            mdsfree(out);
            return nullptr;
        }

        uint32_t cp = djTestHex4_(&lit[i + 1]);
        i += 4;
        /* A high surrogate and the low one after it are one codepoint */
        if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < n && lit[i + 1] == '\\' &&
            lit[i + 2] == 'u') {
            const uint32_t low = djTestHex4_(&lit[i + 3]);
            if (low >= 0xDC00 && low < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 6;
            }
        }

        uint8_t enc[4];
        out = mdscatlen(out, enc, StrUtf8Encode(enc, cp));
    }

    return out;
}

/* Step 'step' of writing [1,2,{"k":"v"},[],3] */
static constexpr size_t djTestStreamSteps = 11;
static void djTestStreamStep_(djState *st, const size_t step) {
    switch (step) {
    case 0:
        djArrayOpen(st);
        break;
    case 1:
        djNumericDirect(st, "1", 1);
        break;
    case 2:
        djNumericDirect(st, "2", 1);
        break;
    case 3:
        djMapOpen(st);
        break;
    case 4:
        djStringDirect(st, "k", 1);
        break;
    case 5:
        djStringDirect(st, "v", 1);
        break;
    case 6:
        djMapCloseElement(st);
        break;
    case 7:
        djArrayOpen(st);
        break;
    case 8:
        djArrayCloseElement(st);
        break;
    case 9:
        djNumericDirect(st, "3", 1);
        break;
    default:
        djArrayCloseFinal(st);
        break;
    }
}

/* Write the integers [from, to) as values */
static void djTestNumbers(djState *state, int from, int to) {
    for (int i = from; i < to; i++) {
        char buf[16];
        const int n = snprintf(buf, sizeof(buf), "%d", i);
        djNumericDirect(state, buf, (size_t)n);
    }
}

/* Helper to run djString and get output for testing */
static mds *djTestEscapeString(const void *data, size_t len) {
    djState testState_ = {};
    djState *testState = &testState_;
    djInit(testState);
    djString(testState, data, len, false);
    return djFinalize(testState);
}

/* Helper to build expected escape output using reference scalar implementation
 */
static mds *djBuildExpectedEscape(const uint8_t *data, size_t len) {
    /* @own: caller; free=mdsfree */
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

int djTest(int, char *[]) {
    int err = 0;

    TEST("true") {
        djState testA_ = {};
        djState *testA = &testA_;
        djInit(testA);

        djTrue(testA);
        cleaner(mdsfree) mds *out = djFinalize(testA);
        if (!djTestIs(out, "true")) {
            ERR("true wrote %s", out);
        }
    }

    TEST("false") {
        djState testA_ = {};
        djState *testA = &testA_;
        djInit(testA);

        djFalse(testA);
        cleaner(mdsfree) mds *out = djFinalize(testA);
        if (!djTestIs(out, "false")) {
            ERR("false wrote %s", out);
        }
    }

    TEST("null") {
        djState testA_ = {};
        djState *testA = &testA_;
        djInit(testA);

        djNULL(testA);
        cleaner(mdsfree) mds *out = djFinalize(testA);
        if (!djTestIs(out, "null")) {
            ERR("null wrote %s", out);
        }
    }

    TEST("hello - direct") {
        djState testA_ = {};
        djState *testA = &testA_;
        djInit(testA);

        djStringDirect(testA, "hello", 5);
        cleaner(mdsfree) mds *out = djFinalize(testA);
        if (!djTestIs(out, "\"hello\"")) {
            ERR("direct string wrote %s", out);
        }
    }

    TEST("hello - escaped") {
        djState testA_ = {};
        djState *testA = &testA_;
        djInit(testA);

        djString(testA, "hello", 5, false);
        cleaner(mdsfree) mds *out = djFinalize(testA);
        if (!djTestIs(out, "\"hello\"")) {
            ERR("escaped string wrote %s", out);
        }
    }

    TEST("escaped strings: UTF-8 copied or escaped to ASCII; bad bytes "
         "become U+FFFD") {
        static const struct {
            const char *in;
            const char *utf8;
            const char *ascii;
        } cases[] = {
            /* U+00E9, U+2603, U+1F600 */
            {"h\xc3\xa9llo \xe2\x98\x83 \xf0\x9f\x98\x80",
             "\"h\xc3\xa9llo \xe2\x98\x83 \xf0\x9f\x98\x80\"",
             "\"h\\u00E9llo \\u2603 \\uD83D\\uDE00\""},
            {"tab\there \"q\" \\ \x01", "\"tab\\there \\\"q\\\" \\\\ \\u0001\"",
             "\"tab\\there \\\"q\\\" \\\\ \\u0001\""},
            /* a stray 0xFF, an encoded surrogate, an overlong '/', and a
             * sequence cut short at the end: one U+FFFD per byte */
            {"a\xff"
             "b\xed\xa0\x80"
             "c\xc0\xaf"
             "d\xe2\x98",
             "\"a\xef\xbf\xbd"
             "b\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd"
             "c\xef\xbf\xbd\xef\xbf\xbd"
             "d\xef\xbf\xbd\xef\xbf\xbd\"",
             "\"a\\uFFFDb\\uFFFD\\uFFFD\\uFFFDc\\uFFFD\\uFFFDd\\uFFFD\\uFFFD"
             "\""},
            /* Invalid text with valid characters in it: U+00E9 with four
             * or more bytes after it, U+20AC ending the input */
            {"\xff\xc3\xa9xyz\xe2\x82\xac",
             "\"\xef\xbf\xbd\xc3\xa9xyz\xe2\x82\xac\"",
             "\"\\uFFFD\\u00E9xyz\\u20AC\""},
            /* U+10FFFF, the highest codepoint */
            {"\xf4\x8f\xbf\xbf", "\"\xf4\x8f\xbf\xbf\"", "\"\\uDBFF\\uDFFF\""},
        };
        for (size_t c = 0; c < countof(cases); c++) {
            for (int utf8 = 0; utf8 < 2; utf8++) {
                djState st = {};
                djInit(&st);
                djString(&st, cases[c].in, strlen(cases[c].in), utf8);
                cleaner(mdsfree) mds *out = djFinalize(&st);
                if (!djTestIs(out, utf8 ? cases[c].utf8 : cases[c].ascii)) {
                    ERR("case %zu flag %d: wrote %s", c, utf8, out);
                }
            }
        }
    }

    TEST("escaped strings of random codepoints read back exactly, in both "
         "forms") {
        uint64_t rng = 0x5eedd17a11ULL;
        for (int round = 0; round < 400; round++) {
            /* Lengths across the 16-byte blocks and the 128-byte splat */
            const size_t cps = dkSplitmix64Bounded(&rng, 120);
            cleaner(mdsfree) mds *in = mdsempty();
            for (size_t i = 0; i < cps; i++) {
                const uint64_t pick = dkSplitmix64Bounded(&rng, 8);
                uint32_t cp;
                if (pick < 3) {
                    cp = (uint32_t)dkSplitmix64Bounded(&rng, 0x80);
                } else if (pick < 5) {
                    cp = 0x80 + (uint32_t)dkSplitmix64Bounded(&rng, 0x780);
                } else if (pick < 7) {
                    cp = 0x800 + (uint32_t)dkSplitmix64Bounded(&rng, 0xF800);
                    if (cp >= 0xD800 && cp < 0xE000) {
                        cp -= 0x800; /* surrogates are not characters */
                    }
                } else {
                    cp =
                        0x10000 + (uint32_t)dkSplitmix64Bounded(&rng, 0x100000);
                }

                uint8_t enc[4];
                const size_t encLen = StrUtf8Encode(enc, cp);
                in = mdscatlen(in, enc, encLen);
            }

            for (int utf8 = 0; utf8 < 2; utf8++) {
                djState st = {};
                djInit(&st);
                djString(&st, in, mdslen(in), utf8);
                cleaner(mdsfree) mds *out = djFinalize(&st);
                bool ascii = true;
                for (size_t i = 0; i < mdslen(out); i++) {
                    ascii &= (uint8_t)out[i] < 0x80;
                }

                cleaner(mdsfree) mds *back = djTestUnescape_(out);
                if (!back || mdslen(back) != mdslen(in) ||
                    memcmp(back, in, mdslen(in)) != 0 || (!utf8 && !ascii)) {
                    ERR("round %d flag %d: %zu codepoints did not read back "
                        "(ascii %d)",
                        round, utf8, cps, ascii);
                    break;
                }
            }
        }
    }

    TEST("hello plus garbage - escaped") {
        djState testA_ = {};
        djState *testA = &testA_;
        djInit(testA);

        djString(testA, "hello\n\n\t\t", 9, false);
        cleaner(mdsfree) mds *out = djFinalize(testA);
        if (!djTestIs(out, "\"hello\\n\\n\\t\\t\"")) {
            ERR("escaped controls wrote %s", out);
        }
    }

    TEST("hello plus garbage SIMD - escaped") {
        djState testA_ = {};
        djState *testA = &testA_;
        djInit(testA);

        /* Each string escaped by the scalar reference, joined as an array */
        cleaner(mdsfree) mds *expected = mdsnew("[");
        djArrayOpen(testA);
        cleaner(mdsfree) mds *str = mdsnew("hello");
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
            cleaner(mdsfree) mds *escaped =
                djBuildExpectedEscape((const uint8_t *)str, mdslen(str));
            expected = mdscatlen(expected, i ? "," : "", i ? 1 : 0);
            expected = mdscatlen(expected, escaped, mdslen(escaped));
        }

        djArrayCloseFinal(testA);
        expected = mdscatlen(expected, "]", 1);
        cleaner(mdsfree) mds *out = djFinalize(testA);
        if (!djTestIs(out, expected)) {
            ERR("escaped array wrote %s, expected %s", out, expected);
        }
    }

    /* "pickles" written with length 6 is "pickle" */
    TEST("simple map") {
        djState testA_ = {};
        djState *testA = &testA_;
        djInit(testA);

        djMapOpen(testA);
        djString(testA, "hello", 5, false);
        djStringDirect(testA, "pickles", 6);
        djMapCloseFinal(testA);

        cleaner(mdsfree) mds *out = djFinalize(testA);
        if (!djTestIs(out, "{\"hello\":\"pickle\"}")) {
            ERR("simple map wrote %s", out);
        }
    }

    TEST("simple two map") {
        djState testA_ = {};
        djState *testA = &testA_;
        djInit(testA);

        djMapOpen(testA);
        djString(testA, "hello", 5, false);
        djStringDirect(testA, "pickles", 6);
        djString(testA, "two", 3, false);
        djStringDirect(testA, "map", 3);
        djMapCloseFinal(testA);

        cleaner(mdsfree) mds *out = djFinalize(testA);
        if (!djTestIs(out, "{\"hello\":\"pickle\",\"two\":\"map\"}")) {
            ERR("two map wrote %s", out);
        }
    }

    TEST("various map") {
        djState testA_ = {};
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

        cleaner(mdsfree) mds *out = djFinalize(testA);
        if (!djTestIs(out,
                      "{\"hello\":\"pickle\",\"two\":\"map\",\"true\":true,"
                      "\"false\":false,\"null\":null}")) {
            ERR("various map wrote %s", out);
        }
    }

    TEST("various map nested") {
        djState testA_ = {};
        djState *testA = &testA_;
        djInit(testA);

        /* The expected text, built alongside by the JSON separator rules
         * alone: in a map, element 2k+1 follows ':' and element 2k+2
         * follows ','; in an array every element after the first follows
         * ',' */
        cleaner(mdsfree) mds *expected = mdsempty();
        constexpr int maxDepth = sizeof(DJ_BITMAP) * 8 * (2 + 3) + 2;
        bool isMap[maxDepth];
        uint32_t elements[maxDepth];
        int32_t depth = -1;
#define DJ_EXPECT_ELEMENT_(text)                                               \
    do {                                                                       \
        if (depth >= 0 && elements[depth]) {                                   \
            expected = mdscat(                                                 \
                expected, (isMap[depth] && elements[depth] % 2) ? ":" : ",");  \
        }                                                                      \
        if (depth >= 0) {                                                      \
            elements[depth]++;                                                 \
        }                                                                      \
        expected = mdscat(expected, text);                                     \
    } while (0)
#define DJ_EXPECT_OPEN_(map)                                                   \
    do {                                                                       \
        DJ_EXPECT_ELEMENT_((map) ? "{" : "[");                                 \
        depth++;                                                               \
        isMap[depth] = (map);                                                  \
        elements[depth] = 0;                                                   \
    } while (0)
#define DJ_EXPECT_CLOSE_()                                                     \
    do {                                                                       \
        expected = mdscat(expected, isMap[depth] ? "}" : "]");                 \
        depth--;                                                               \
    } while (0)

        djMapOpen(testA);
        DJ_EXPECT_OPEN_(true);
        djString(testA, "hello", 5, false);
        DJ_EXPECT_ELEMENT_("\"hello\"");
        djStringDirect(testA, "pickles", 6);
        DJ_EXPECT_ELEMENT_("\"pickle\"");

        djStringDirect(testA, "DAMAP", 5);
        DJ_EXPECT_ELEMENT_("\"DAMAP\"");

        /* Stress bitmap by growing beyond its static allocation */
        /* (default is 2 static, so add 3 more to grow beyond) */
        static constexpr size_t maxNesting = sizeof(DJ_BITMAP) * 8 * (2 + 3);

        uint64_t rng = 0x2545F4914F6CDD1DULL;
        for (size_t i = 0; i < maxNesting; i++) {
            dkLcg64Next(&rng);
            const bool map = (rng >> 40) % 2 == 0;
            if (map) {
                djMapOpen(testA);
            } else {
                djArrayOpen(testA);
            }
            DJ_EXPECT_OPEN_(map);

            djString(testA, "here's a key ya coward", 22, false);
            DJ_EXPECT_ELEMENT_("\"here's a key ya coward\"");
            const uint32_t picker = (uint32_t)(rng >> 33);
            for (size_t j = 0; j < 2; j++) {
                /* If the LAST LAST element, don't place anything because
                 * the j==1 is a key for the next map, but we won't
                 * have a next map. */
                if (j && i == maxNesting - 1) {
                    break;
                }

                if (!j && picker % 2 == 0) {
                    djTrue(testA);
                    DJ_EXPECT_ELEMENT_("true");
                } else if (!j && picker % 3 == 0) {
                    djFalse(testA);
                    DJ_EXPECT_ELEMENT_("false");
                } else if (!j && picker % 5 == 0) {
                    djNULL(testA);
                    DJ_EXPECT_ELEMENT_("null");
                } else if (picker % 7 == 0) {
                    djString(testA, "wallby", 5, false);
                    DJ_EXPECT_ELEMENT_("\"wallb\"");
                } else {
                    djStringDirect(testA, "pickledNonense", 14);
                    DJ_EXPECT_ELEMENT_("\"pickledNonense\"");
                }
            }
        }

        for (size_t i = 0; i < maxNesting; i++) {
            djString(testA, "finalizerA", 10, false);
            DJ_EXPECT_ELEMENT_("\"finalizerA\"");
            djStringDirect(testA, "there it is", 11);
            DJ_EXPECT_ELEMENT_("\"there it is\"");
            djCloseElement(testA);
            DJ_EXPECT_CLOSE_();
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
        DJ_EXPECT_ELEMENT_("\"two\"");
        DJ_EXPECT_ELEMENT_("\"map\"");
        DJ_EXPECT_ELEMENT_("\"true\"");
        DJ_EXPECT_ELEMENT_("true");
        DJ_EXPECT_ELEMENT_("\"false\"");
        DJ_EXPECT_ELEMENT_("false");
        DJ_EXPECT_ELEMENT_("\"null\"");
        DJ_EXPECT_ELEMENT_("null");
        DJ_EXPECT_CLOSE_();
#undef DJ_EXPECT_ELEMENT_
#undef DJ_EXPECT_OPEN_
#undef DJ_EXPECT_CLOSE_

        cleaner(mdsfree) mds *out = djFinalize(testA);
        if (depth != -1 || !djTestIs(out, expected)) {
            ERR("nested output differs from the expected text (depth %d)",
                depth);
        }
    }

    TEST("simple array") {
        djState testA_ = {};
        djState *testA = &testA_;
        djInit(testA);

        djArrayOpen(testA);
        djString(testA, "hello", 5, false);
        djStringDirect(testA, "pickles", 6);
        djArrayCloseFinal(testA);

        cleaner(mdsfree) mds *out = djFinalize(testA);
        if (!djTestIs(out, "[\"hello\",\"pickle\"]")) {
            ERR("simple array wrote %s", out);
        }
    }

    TEST("simple two array") {
        djState testA_ = {};
        djState *testA = &testA_;
        djInit(testA);

        djArrayOpen(testA);
        djString(testA, "hello", 5, false);
        djStringDirect(testA, "pickles", 6);
        djString(testA, "two", 3, false);
        djStringDirect(testA, "map", 3);
        djArrayCloseFinal(testA);

        cleaner(mdsfree) mds *out = djFinalize(testA);
        if (!djTestIs(out, "[\"hello\",\"pickle\",\"two\",\"map\"]")) {
            ERR("two array wrote %s", out);
        }
    }

    TEST("various array") {
        djState testA_ = {};
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

        cleaner(mdsfree) mds *out = djFinalize(testA);
        if (!djTestIs(out,
                      "[\"hello\",\"pickle\",\"two\",\"map\",\"true\",true,"
                      "\"false\",false,\"null\",null]")) {
            ERR("various array wrote %s", out);
        }
    }

    TEST("various nesting mechanics") {
        djState testA_ = {};
        djState *testA = &testA_;
        djInit(testA);

        /* Each block opens one container and closes it */
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
                            djMapCloseElement(testA);
                        }

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
                        djMapCloseElement(testA);
                    }
                    djMapCloseElement(testA);
                }
                djMapCloseElement(testA);
            }
            djMapCloseFinal(testA);
        }

        cleaner(mdsfree) mds *out = djFinalize(testA);
        if (!djTestIs(out, "{\"b\":{\"c\":{\"d\":{\"e\":{\"f\":{\"g\":{\"a\":"
                           "\"b\"}}},\"mine\":[\"e\",\"f\",\"g\"],\"q\":{\"e\","
                           "\"f\",\"g\"}}}}}")) {
            ERR("nesting mechanics wrote %s", out);
        }
    }

    TEST("SIMD escape detection - comprehensive stress test") {
        printf("Testing escape detection with various string lengths...\n");
        uint32_t testCount = 0;
        uint32_t passCount = 0;

        /* Test 1: Strings with NO escape characters at various lengths */
        for (size_t len = 1; len <= 100; len++) {
            cleanerName(zfree, U8) uint8_t *data = zmalloc(len);
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
        }

        /* Test 2: Single escape char at each position for various lengths */
        const uint8_t escapeChars[] = {'"', '\\', '\n', '\t', '\r', 0, 1, 0x1F};
        for (size_t ec = 0; ec < sizeof(escapeChars); ec++) {
            for (size_t len = 1; len <= 64; len++) {
                for (size_t pos = 0; pos < len; pos++) {
                    cleanerName(zfree, U8) uint8_t *data = zmalloc(len);
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
                }
            }
        }

        /* Test 3: Multiple escape chars in same string */
        for (size_t len = 16; len <= 64; len += 8) {
            cleanerName(zfree, U8) uint8_t *data = zmalloc(len);
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
        }

        /* Test 4: Long strings with escape at specific SIMD boundary positions
         */
        const size_t boundaryPos[] = {0,  1,  15, 16, 17, 31,
                                      32, 33, 47, 48, 63, 64};
        for (size_t bp = 0; bp < countof(boundaryPos); bp++) {
            const size_t pos = boundaryPos[bp];
            const size_t len = 128;

            cleanerName(zfree, U8) uint8_t *data = zmalloc(len);
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
        }

        /* Test 5: All control characters (0x00-0x1F) */
        for (uint8_t ctrl = 0; ctrl < 0x20; ctrl++) {
            const size_t len = 32;
            cleanerName(zfree, U8) uint8_t *data = zmalloc(len);
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
        }

        /* Test 6: Random stress test */
        srandom(12345); /* Fixed seed for reproducibility */
        for (uint32_t trial = 0; trial < 1000; trial++) {
            const size_t len = 1 + (random() % 200);
            cleanerName(zfree, U8) uint8_t *data = zmalloc(len);

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
        }

        printf("SIMD escape detection: %u/%u tests passed\n", passCount,
               testCount);
        assert(passCount == testCount);
    }

    TEST("djBox: every scalar as a value; no JSON form writes nothing") {
        djState st = {};
        djInit(&st);
        djArrayOpen(&st);

        __int128_t wide = -((__int128_t)1 << 100);
        const databox wideBox = {
            .type = DATABOX_SIGNED_128, .big = true, .data.i128 = &wide};
        const databox written[] = {
            databoxNewSigned(-5),
            databoxNewUnsigned(7),
            wideBox,
            DATABOX_DOUBLE(1.5),
            {.type = DATABOX_FLOAT_32, .data.f32 = 0.5f},
            databoxNewBytesString("a\"b"),
            databoxNewBytesAllowEmbed("hi", 2),
            {.type = DATABOX_BYTES_NEVER_FREE,
             .data.bytes.ccstart = "nf",
             .len = 2},
            DATABOX_BOX_TRUE,
            DATABOX_BOX_FALSE,
            DATABOX_BOX_NULL,
        };
        const databox refused[] = {
            DATABOX_NAN,
            DATABOX_INFINITY_POSITIVE,
            {.type = DATABOX_FLOAT_32, .data.f32 = -INFINITY},
            DATABOX_BOX_VOID,
            {.type = DATABOX_ERROR},
            {.type = DATABOX_BYTES_VOID, .len = 4},
            {.type = DATABOX_BYTES_OFFSET, .data.bytes.offset = 3, .len = 2},
            {.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL, .data.u = 9},
            {.type = DATABOX_ARRAY_START},
        };
        for (size_t i = 0; i < countof(written); i++) {
            if (!djBox(&st, &written[i])) {
                ERR("scalar %zu was refused", i);
            }

            /* A refusal between writes leaves no trace in the output */
            const databox *r = &refused[i % countof(refused)];
            if (djBox(&st, r)) {
                ERR("type %d was written", r->type);
            }
        }

        djArrayCloseFinal(&st);
        cleaner(mdsfree) mds *out = djFinalize(&st);
        if (!djTestIs(out, "[-5,7,-1267650600228229401496703205376,1.5,0.5,"
                           "\"a\\\"b\",\"hi\",\"nf\",true,false,null]")) {
            ERR("scalars wrote %s", out);
        }
    }

    TEST("djBox: keys are strings; a refused value keeps the pairing") {
        djState st = {};
        djInit(&st);
        djMapOpen(&st);
        const databox x = databoxNewBytesString("x");
        const databox steps[] = {
            databoxNewSigned(3),
            DATABOX_BOX_TRUE,
            DATABOX_BOX_TRUE,
            DATABOX_BOX_NULL,
            DATABOX_NAN,
            databoxNewUnsigned(1),
            DATABOX_BOX_FALSE,
            DATABOX_NAN, /* refused value */
            x,
        };
        for (size_t i = 0; i < countof(steps); i++) {
            const bool wrote = djBox(&st, &steps[i]);
            if (wrote != (i != 7)) {
                ERR("step %zu wrote %d", i, wrote);
            }
        }

        djMapCloseFinal(&st);
        cleaner(mdsfree) mds *out = djFinalize(&st);
        if (!djTestIs(out, "{\"3\":true,\"true\":null,\"nan\":1,\"false\":"
                           "\"x\"}")) {
            ERR("keyed boxes wrote %s", out);
        }
    }

    TEST("djNumericDirect and sets: bare values, quoted keys") {
        djState st = {};
        djInit(&st);
        djArrayOpen(&st);
        djSetOpen(&st);
        djNumericDirect(&st, "1", 1);
        djNumericDirect(&st, "2", 1);
        djSetCloseElement(&st);
        djMapOpen(&st);
        djNumericDirect(&st, "5", 1);
        djNumericDirect(&st, "6", 1);
        djMapCloseElement(&st);
        djSetOpen(&st);
        djSetCloseFinal(&st);
        djArrayCloseFinal(&st);
        cleaner(mdsfree) mds *out = djFinalize(&st);
        if (!djTestIs(out, "[{1,2},{\"5\":6},{}]")) {
            ERR("numeric and set output %s", out);
        }

        /* A lone separator is still a trailing comma */
        djState lone = {};
        djInit(&lone);
        djNumericDirect(&lone, "", 0);
        cleaner(mdsfree) mds *empty = djFinalize(&lone);
        if (!djTestIs(empty, "")) {
            ERR("a lone separator finalized to '%s'", empty);
        }
    }

    TEST("djAppend: buffers move in order; matches one buffer written "
         "straight through") {
        djState oracle = {};
        djInit(&oracle);
        djTestNumbers(&oracle, 0, 740);

        djState dst = {};
        djInit(&dst);
        djTestNumbers(&dst, 0, 10);

        djState src1 = {};
        djInit(&src1);
        djTestNumbers(&src1, 10, 20);
        djAppend(&dst, &src1);
        if (src1.s || src1.ss) {
            ERR("the appended source was not left zeroed");
        }

        /* Enough to move the tail buffer before the next append */
        djTestNumbers(&dst, 20, 700);

        /* A source that itself spans two buffers */
        djState src2 = {};
        djInit(&src2);
        djTestNumbers(&src2, 700, 710);
        djState src3 = {};
        djInit(&src3);
        djTestNumbers(&src3, 710, 720);
        djAppend(&src2, &src3);
        djTestNumbers(&src2, 720, 730);
        djAppend(&dst, &src2);

        /* An empty source adds nothing */
        djState src4 = {};
        djInit(&src4);
        djAppend(&dst, &src4);

        djTestNumbers(&dst, 730, 740);

        size_t viewCount = 0;
        mds **view = djGetMulti(&dst, &viewCount);
        size_t oracleCount = 0;
        mds **oracleView = djGetMulti(&oracle, &oracleCount);
        cleaner(mdsfree) mds *joinedView = djTestJoin(view, viewCount);
        cleaner(mdsfree) mds *oracleRaw = djTestJoin(oracleView, oracleCount);
        if (viewCount != 4 || oracleCount != 1 ||
            !djTestIs(joinedView, oracleRaw)) {
            ERR("view has %zu buffers (want 4) and %s the oracle", viewCount,
                djTestIs(joinedView, oracleRaw) ? "matches" : "differs from");
        }

        size_t count = 0;
        mds **parts = djFinalizeMulti(&dst, &count);
        cleaner(mdsfree) mds *joined = djTestJoin(parts, count);
        cleaner(mdsfree) mds *want = djFinalize(&oracle);
        if (count != 4 || !djTestIs(joined, want) || dst.s || dst.ss) {
            ERR("finalized %zu buffers; output %s", count,
                djTestIs(joined, want) ? "matches" : "differs");
        }

        for (size_t i = 0; i < count; i++) {
            mdsfree(parts[i]);
        }

        zfree(parts);
        djFree(&dst); /* zeroed by the finalize: a no-op */

        /* One buffer still comes back as an owned array */
        djState one = {};
        djInit(&one);
        djTestNumbers(&one, 4, 5);
        mds **single = djFinalizeMulti(&one, &count);
        if (count != 1 || !djTestIs(single[0], "4")) {
            ERR("single-buffer finalize gave %zu buffers", count);
        }

        mdsfree(single[0]);
        zfree(single);
    }

    TEST("djConsumeBuffer mid-document: the chunks join to the document") {
        static const char *const want = "[1,2,{\"k\":\"v\"},[],3]";
        /* cut < steps: consume after that step only; cut == steps: after
         * every step */
        for (size_t cut = 0; cut <= djTestStreamSteps; cut++) {
            djState st = {};
            djInit(&st);
            cleaner(mdsfree) mds *joined = mdsempty();
            for (size_t step = 0; step < djTestStreamSteps; step++) {
                djTestStreamStep_(&st, step);
                if (cut == step || cut == djTestStreamSteps) {
                    mds *chunk = djConsumeBuffer(&st, true);
                    if (mdslen(chunk) != strlen(chunk)) {
                        ERR("cut %zu step %zu: a chunk holds a NUL byte", cut,
                            step);
                    }
                    joined = mdscatlen(joined, chunk, mdslen(chunk));
                    mdsfree(chunk);
                }
            }

            mds *rest = djFinalize(&st);
            joined = mdscatlen(joined, rest, mdslen(rest));
            mdsfree(rest);
            if (!djTestIs(joined, want)) {
                ERR("cut %zu: chunks joined to %s", cut, joined);
            }
        }
    }

    TEST("djFree: frees every appended buffer and leaves a zeroed state") {
        djState dst = {};
        djInit(&dst);
        djTestNumbers(&dst, 0, 5);
        for (int i = 0; i < 6; i++) {
            djState src = {};
            djInit(&src);
            djTestNumbers(&src, i * 100, i * 100 + 50);
            djAppend(&dst, &src);
            djTestNumbers(&dst, 0, 200);
        }

        djFree(&dst);
        djFree(&dst);
        if (dst.s || dst.ss || dst.ssCount) {
            ERR("djFree did not leave the state zeroed");
        }
    }

    TEST("deep nesting: types tracked past the inline bitmap") {
        /* Alternate map and array levels with one entry each, close every
         * inner level through the type-detecting close, and compare with
         * the text the levels imply. */
        const size_t depth = 300; /* even: the innermost level is an array */
        djState st = {};
        djInit(&st);
        cleaner(mdsfree) mds *want = mdsempty();
        for (size_t i = 0; i < depth; i++) {
            if (i % 2 == 0) {
                djMapOpen(&st);
                djStringDirect(&st, "k", 1);
                want = mdscatlen(want, "{\"k\":", 5);
            } else {
                djArrayOpen(&st);
                want = mdscatlen(want, "[", 1);
            }
        }

        djNumericDirect(&st, "1", 1);
        want = mdscatlen(want, "1", 1);
        for (size_t i = depth; i-- > 1;) {
            djCloseElement(&st);
            want = mdscatlen(want, i % 2 == 0 ? "}" : "]", 1);
        }

        djMapCloseFinal(&st);
        want = mdscatlen(want, "}", 1);
        cleaner(mdsfree) mds *out = djFinalize(&st);
        if (!djTestIs(out, want)) {
            ERR("deep nesting closed the wrong container types");
        }

        djFree(&st); /* the finalize released the bitmap: a no-op */

        /* An unfinished deep state frees its bitmap too */
        djState open = {};
        djInit(&open);
        for (size_t i = 0; i < depth; i++) {
            djArrayOpen(&open);
        }

        djFree(&open);
    }

    TEST_FINAL_RESULT;
}

#endif
