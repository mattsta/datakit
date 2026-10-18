#include "varintDDStream.h"
#include "varintTagged.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define DD_STREAM_LITTLE_ENDIAN 1
#else
#define DD_STREAM_LITTLE_ENDIAN 0
#endif

#if defined(VARINT_DD_FORCE_SCALAR)
#define DD_STREAM_SIMD 0
#elif DD_STREAM_LITTLE_ENDIAN && defined(__AVX2__)
#define DD_STREAM_SIMD 1
#include <immintrin.h>
#elif DD_STREAM_LITTLE_ENDIAN && defined(__ARM_NEON) && defined(__aarch64__)
#define DD_STREAM_SIMD 2
#include <arm_neon.h>
#else
#define DD_STREAM_SIMD 0
#endif

/* ====================================================================
 * Wire format
 * ====================================================================
 *   [flags:1][count:tagged][hi stream][lo bitmap][lo bitstream]
 *
 * flags   bits 0-1  leading-limb mode (RAW or XOR)
 *         bits 2-7  trailing mantissa bits retained, 0 to 52
 *
 * No length fields appear anywhere. Each section is self-delimiting
 * given the count: the verbatim leading-limb stream is exactly eight
 * bytes per value, the XOR stream ends when the last value is decoded,
 * the bitmap is one bit per value, and each bitstream entry carries its
 * own width. That removes ~18 bytes of framing and, more importantly,
 * removes the chance of a length field disagreeing with its payload.
 *
 * Leading limbs come FIRST because reconstructing a trailing limb needs
 * its partner's exponent - the whole point of gap coding is that the
 * exponent is not stored. Decoding therefore has to see hi before lo. */

#define DD_STREAM_HI_MODE_MASK 0x03u
#define DD_STREAM_LO_BITS_SHIFT 2

/* A normalized trailing limb sits at least 53 exponents below its
 * partner. Gaps of 53 through 115 code inline as 0 through 62; the last
 * code is an escape that stores the limb verbatim, which covers
 * denormals, non-finite limbs, and hand-assembled pairs that never went
 * through varintDDNormalize. */
#define DD_STREAM_GAP_BASE 53
#define DD_STREAM_GAP_MAX 62
#define DD_STREAM_GAP_ESCAPE 63

/* ====================================================================
 * Scalar helpers
 * ==================================================================== */

static inline uint64_t ddBitsOfDouble(double value) {
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static inline double ddDoubleOfBits(uint64_t bits) {
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

/* varintTaggedGet takes its bound as int32_t. A buffer larger than
 * INT32_MAX would narrow to something negative or misleadingly small,
 * so clamp instead: a tagged varint is at most 9 bytes, and any limit
 * past that is equivalent to "plenty". */
static inline int32_t ddTaggedLimit(size_t available) {
    return available > (size_t)INT32_MAX ? INT32_MAX : (int32_t)available;
}

static inline void ddStore64LE(uint8_t *dst, uint64_t value) {
    for (uint32_t i = 0; i < 8; i++) {
        dst[i] = (uint8_t)(value >> (i * 8));
    }
}

static inline uint64_t ddLoad64LE(const uint8_t *src) {
    uint64_t value = 0;

    for (uint32_t i = 0; i < 8; i++) {
        value |= (uint64_t)src[i] << (i * 8);
    }

    return value;
}

/* ====================================================================
 * Bit I/O
 * ====================================================================
 * This module carries its own bit reader rather than using the one in
 * varintElias.h, for one reason that matters: varintBitReaderRead only
 * ASSERTS that it stays in bounds, so in a release build it will read
 * past the end of a truncated buffer. A codec whose decode contract
 * promises safety on untrusted input cannot be built on that.
 *
 * Every read here is checked and latches an overrun flag, so a
 * malformed stream degrades to "returns 0" instead of reading memory
 * it does not own.
 *
 * The writer doubles as a measuring tape: give it a NULL buffer and it
 * counts bits without storing any. varintDDStreamAnalyze reports exact
 * sizes that way, running the identical code the encoder runs rather
 * than a parallel estimate that could drift out of agreement. */

/* Bits accumulate MSB-first in a 64-bit register and leave in whole
 * eight-byte groups. That ordering is not a coincidence: packing bits
 * MSB-first IS big-endian, so a big-endian store of the accumulator
 * produces byte for byte what a bit-at-a-time loop would have, while
 * touching memory once per 64 bits instead of once per 8.
 *
 * Draining only ever writes whole, fully-determined words, and the
 * final partial word is written with its unused low bits zero. Padding
 * therefore cannot pick up whatever the caller's buffer already held,
 * which would make the encoder's output depend on uninitialized memory
 * and differ between runs on identical input. */
typedef struct ddBitWriter {
    uint8_t *buffer;  /* NULL measures without storing */
    size_t capacity;  /* bytes available at buffer */
    size_t bitPos;    /* total bits presented, drained or not */
    size_t byteCount; /* bytes actually drained to buffer */
    uint64_t acc;     /* pending bits, MSB-aligned */
    uint32_t accBits; /* how many of acc are live, 0..63 after a drain */
    bool overflow;
} ddBitWriter;

static void ddBitWriterInit(ddBitWriter *w, uint8_t *buffer, size_t capacity) {
    w->buffer = buffer;
    w->capacity = capacity;
    w->bitPos = 0;
    w->byteCount = 0;
    w->acc = 0;
    w->accBits = 0;
    w->overflow = false;
}

static void ddBitWriterDrain(ddBitWriter *w) {
    for (uint32_t i = 0; i < 8; i++) {
        w->buffer[w->byteCount + i] = (uint8_t)(w->acc >> (56 - i * 8));
    }

    w->byteCount += 8;
    w->acc = 0;
    w->accBits = 0;
}

static void ddBitWrite(ddBitWriter *w, uint64_t value, uint32_t bits) {
    if (bits == 0 || w->overflow) {
        return;
    }

    w->bitPos += bits;

    if (w->buffer == NULL) {
        return; /* measuring: the bit count is the whole answer */
    }

    if ((w->bitPos + 7) / 8 > w->capacity) {
        w->overflow = true;
        return;
    }

    /* Callers pass values already in range, but masking keeps a stray
     * high bit from corrupting bits that belong to another field. */
    const uint64_t masked =
        bits >= 64 ? value : (value & ((UINT64_C(1) << bits) - 1));

    if (w->accBits + bits < 64) {
        w->acc |= masked << (64 - w->accBits - bits);
        w->accBits += bits;
        return;
    }

    /* The write spans the end of the accumulator: take the bits that
     * fit, drain a full word, then seed the accumulator with the rest. */
    const uint32_t fits = 64 - w->accBits;
    const uint32_t rest = bits - fits;

    w->acc |= masked >> rest;
    ddBitWriterDrain(w);

    if (rest > 0) {
        w->acc = masked << (64 - rest);
        w->accBits = rest;
    }
}

/* Flush the partial word. Must be called before the buffer is read;
 * bits still in the accumulator have not reached memory. */
static size_t ddBitWriterFinish(ddBitWriter *w) {
    if (w->buffer != NULL && !w->overflow && w->accBits > 0) {
        const uint32_t tailBytes = (w->accBits + 7) / 8;

        for (uint32_t i = 0; i < tailBytes; i++) {
            w->buffer[w->byteCount + i] = (uint8_t)(w->acc >> (56 - i * 8));
        }

        w->byteCount += tailBytes;
        w->acc = 0;
        w->accBits = 0;
    }

    return (w->bitPos + 7) / 8;
}

static size_t ddBitWriterBytes(const ddBitWriter *w) {
    return (w->bitPos + 7) / 8;
}

typedef struct ddBitReader {
    const uint8_t *buffer;
    size_t bufferBytes; /* what may actually be touched */
    size_t totalBits;
    size_t bitPos;
    bool overrun;
} ddBitReader;

static void ddBitReaderInit(ddBitReader *r, const uint8_t *buffer,
                            size_t availableBytes) {
    r->buffer = buffer;
    r->bufferBytes = availableBytes;
    /* Clamp rather than multiply blindly; a bogus length must not wrap
     * the bit count around to something small and permissive. */
    r->totalBits =
        availableBytes > (SIZE_MAX / 8) ? SIZE_MAX : availableBytes * 8;
    r->bitPos = 0;
    r->overrun = false;
}

/* Reads up to 64 bits, MSB-first, matching the writer's ordering.
 *
 * The fast path is a single big-endian eight-byte load: because a field
 * starts at most 7 bits into a byte, one 64-bit word covers any field
 * of 57 bits or fewer whatever its alignment, and every field this
 * codec reads except the 64-bit verbatim escape is smaller than that.
 * It is guarded on the BUFFER length, not the bit length, since the
 * whole point of this reader is that it never touches a byte the caller
 * did not hand it. */
static uint64_t ddBitRead(ddBitReader *r, uint32_t bits) {
    uint64_t out = 0;

    if (bits == 0 || r->overrun) {
        return 0;
    }

    if (r->bitPos + bits > r->totalBits) {
        r->overrun = true;
        return 0;
    }

    const size_t startByte = r->bitPos >> 3;

    if (bits <= 57 && startByte + 8 <= r->bufferBytes) {
        const uint32_t bitOffset = (uint32_t)(r->bitPos & 7);
        uint64_t word = 0;

        for (uint32_t i = 0; i < 8; i++) {
            word = (word << 8) | r->buffer[startByte + i];
        }

        r->bitPos += bits;
        return (word << bitOffset) >> (64 - bits);
    }

    /* Near the end of the buffer, or a full-width field: walk bytes. */
    while (bits > 0) {
        const size_t byteIndex = r->bitPos >> 3;
        const uint32_t used = (uint32_t)(r->bitPos & 7);
        const uint32_t room = 8 - used;
        const uint32_t take = bits < room ? bits : room;
        const uint32_t mask = (UINT32_C(1) << take) - UINT32_C(1);
        const uint32_t chunk =
            (uint32_t)((r->buffer[byteIndex] >> (room - take)) & mask);

        out = (out << take) | chunk;
        r->bitPos += take;
        bits -= take;
    }

    return out;
}

static size_t ddBitReaderBytes(const ddBitReader *r) {
    return (r->bitPos + 7) / 8;
}

/* Elias gamma, matching varintElias.h's code but running over the
 * bounds-checked reader above. Values here are always 1 to 64, so a
 * code is at most 13 bits. */

static void ddGammaWrite(ddBitWriter *w, uint64_t value) {
    const uint32_t n = UINT32_C(63) - (uint32_t)__builtin_clzll(value);

    ddBitWrite(w, 0, n);         /* n leading zeros */
    ddBitWrite(w, value, n + 1); /* the value, leading 1 included */
}

static uint64_t ddGammaRead(ddBitReader *r) {
    uint32_t n = 0;

    while (ddBitRead(r, 1) == 0) {
        if (r->overrun) {
            return 0;
        }

        if (++n > 63) {
            r->overrun = true; /* no valid code is this long */
            return 0;
        }
    }

    if (r->overrun) {
        return 0;
    }

    const uint64_t rest = ddBitRead(r, n);

    if (r->overrun) {
        return 0;
    }

    return (1ULL << n) | rest;
}

/* ====================================================================
 * Trailing limbs
 * ==================================================================== */

static void ddWriteLoLimb(ddBitWriter *w, uint64_t hiBits, uint64_t loBits,
                          uint32_t loMantissaBits, size_t *escaped) {
    const uint32_t hiExp = (uint32_t)((hiBits >> 52) & 0x7FF);
    const uint32_t loExp = (uint32_t)((loBits >> 52) & 0x7FF);
    const int gap = (int)hiExp - (int)loExp - DD_STREAM_GAP_BASE;

    /* A denormal or non-finite trailing limb has no usable gap, and an
     * unnormalized pair may have no gap at all. Both take the escape. */
    if (loExp == 0 || loExp == 0x7FF || gap < 0 || gap > DD_STREAM_GAP_MAX) {
        ddGammaWrite(w, DD_STREAM_GAP_ESCAPE + 1);
        ddBitWrite(w, loBits, 64);
        (*escaped)++;
        return;
    }

    ddGammaWrite(w, (uint64_t)gap + 1);
    ddBitWrite(w, loBits >> 63, 1);

    /* Truncate toward zero. Rounding could push |lo| past ulp(hi)/2 and
     * hand back a pair that is no longer a normalized double-double. */
    ddBitWrite(w, (loBits & 0xFFFFFFFFFFFFFULL) >> (52 - loMantissaBits),
               loMantissaBits);
}

static uint64_t ddReadLoLimb(ddBitReader *r, uint64_t hiBits,
                             uint32_t loMantissaBits, bool *valid) {
    const uint64_t code = ddGammaRead(r);

    if (r->overrun || code == 0) {
        *valid = false;
        return 0;
    }

    const uint64_t gap = code - 1;

    if (gap == DD_STREAM_GAP_ESCAPE) {
        const uint64_t raw = ddBitRead(r, 64);

        if (r->overrun) {
            *valid = false;
            return 0;
        }

        return raw;
    }

    if (gap > DD_STREAM_GAP_MAX) {
        *valid = false;
        return 0;
    }

    const uint64_t sign = ddBitRead(r, 1);
    const uint64_t mantissa = ddBitRead(r, loMantissaBits)
                              << (52 - loMantissaBits);

    if (r->overrun) {
        *valid = false;
        return 0;
    }

    const uint32_t hiExp = (uint32_t)((hiBits >> 52) & 0x7FF);
    const int loExp = (int)hiExp - DD_STREAM_GAP_BASE - (int)gap;

    /* The encoder only ever emits gaps that land on a normal exponent,
     * so anything else means the stream was corrupted. */
    if (loExp < 1 || loExp > 2046) {
        *valid = false;
        return 0;
    }

    return (sign << 63) | ((uint64_t)loExp << 52) | mantissa;
}

/* ====================================================================
 * Leading limbs, verbatim
 * ====================================================================
 * Gathering one field out of an array of structs is a strided access,
 * which scalar code does at one value per iteration. Both vector
 * backends deinterleave in a single instruction pair, so this runs at
 * memory speed instead. */

static void ddPackHiRaw(uint8_t *dst, const varintDD *values, size_t count) {
    size_t i = 0;

#if DD_STREAM_SIMD == 2
    for (; i + 2 <= count; i += 2) {
        const float64x2x2_t v =
            vld2q_f64((const double *)(const void *)(values + i));
        vst1q_f64((double *)(void *)(dst + i * 8), v.val[0]);
    }
#elif DD_STREAM_SIMD == 1
    for (; i + 4 <= count; i += 4) {
        const __m256d a =
            _mm256_loadu_pd((const double *)(const void *)(values + i));
        const __m256d b =
            _mm256_loadu_pd((const double *)(const void *)(values + i + 2));
        const __m256d hs = _mm256_unpacklo_pd(a, b);

        _mm256_storeu_pd((double *)(void *)(dst + i * 8),
                         _mm256_permute4x64_pd(hs, _MM_SHUFFLE(3, 1, 2, 0)));
    }
#endif

    for (; i < count; i++) {
        ddStore64LE(dst + i * 8, ddBitsOfDouble(values[i].hi));
    }
}

/* Scatters leading limbs back into the strided layout. The trailing
 * limb of every element is assigned by the caller afterwards, so the
 * zeros written here are placeholders, not results. */
static void ddUnpackHiRaw(varintDD *values, const uint8_t *src, size_t count) {
    size_t i = 0;

#if DD_STREAM_SIMD == 2
    for (; i + 2 <= count; i += 2) {
        float64x2x2_t v;

        v.val[0] = vld1q_f64((const double *)(const void *)(src + i * 8));
        v.val[1] = vdupq_n_f64(0.0);
        vst2q_f64((double *)(void *)(values + i), v);
    }
#elif DD_STREAM_SIMD == 1
    for (; i + 4 <= count; i += 4) {
        const __m256d h =
            _mm256_loadu_pd((const double *)(const void *)(src + i * 8));
        const __m256d p = _mm256_permute4x64_pd(h, _MM_SHUFFLE(3, 1, 2, 0));
        const __m256d z = _mm256_setzero_pd();

        _mm256_storeu_pd((double *)(void *)(values + i),
                         _mm256_unpacklo_pd(p, z));
        _mm256_storeu_pd((double *)(void *)(values + i + 2),
                         _mm256_unpackhi_pd(p, z));
    }
#endif

    for (; i < count; i++) {
        values[i].hi = ddDoubleOfBits(ddLoad64LE(src + i * 8));
        values[i].lo = 0.0;
    }
}

/* ====================================================================
 * Leading limbs, XOR-chained
 * ====================================================================
 * The Gorilla encoding, the same shape varintDeltaDelta applies to
 * integers: XOR against the previous value, then send only the window
 * of bits that actually changed. Consecutive samples of a smooth
 * quantity share their sign, exponent, and most of their mantissa, so
 * the window is usually a handful of bits. Unrelated values share
 * nothing and the window is the whole word, which is why AUTO exists. */

static void ddWriteHiXor(ddBitWriter *w, const varintDD *values, size_t count) {
    uint64_t previous = 0;
    uint32_t windowLead = 0;
    uint32_t windowTrail = 0;
    bool haveWindow = false;

    for (size_t i = 0; i < count; i++) {
        const uint64_t bits = ddBitsOfDouble(values[i].hi);

        if (i == 0) {
            ddBitWrite(w, bits, 64);
            previous = bits;
            continue;
        }

        const uint64_t delta = bits ^ previous;
        previous = bits;

        if (delta == 0) {
            ddBitWrite(w, 0, 1); /* identical to the previous value */
            continue;
        }

        ddBitWrite(w, 1, 1);

        const uint32_t lead = (uint32_t)__builtin_clzll(delta);
        const uint32_t trail = (uint32_t)__builtin_ctzll(delta);

        if (haveWindow && lead >= windowLead && trail >= windowTrail) {
            /* fits the window already in effect: no descriptor needed */
            ddBitWrite(w, 0, 1);
            ddBitWrite(w, delta >> windowTrail, 64 - windowLead - windowTrail);
        } else {
            ddBitWrite(w, 1, 1);
            ddBitWrite(w, lead, 6);
            ddBitWrite(w, (uint64_t)(64 - lead - trail - 1), 6);
            ddBitWrite(w, delta >> trail, 64 - lead - trail);

            windowLead = lead;
            windowTrail = trail;
            haveWindow = true;
        }
    }
}

static bool ddReadHiXor(ddBitReader *r, varintDD *values, size_t count) {
    uint64_t previous = 0;
    uint32_t windowLead = 0;
    uint32_t windowTrail = 0;
    bool haveWindow = false;

    for (size_t i = 0; i < count; i++) {
        if (i == 0) {
            previous = ddBitRead(r, 64);

            if (r->overrun) {
                return false;
            }

            values[0].hi = ddDoubleOfBits(previous);
            continue;
        }

        const uint64_t changed = ddBitRead(r, 1);

        if (r->overrun) {
            return false;
        }

        if (changed == 0) {
            values[i].hi = ddDoubleOfBits(previous);
            continue;
        }

        const uint64_t freshWindow = ddBitRead(r, 1);

        if (r->overrun) {
            return false;
        }

        uint32_t meaningful;

        if (freshWindow) {
            windowLead = (uint32_t)ddBitRead(r, 6);
            meaningful = (uint32_t)ddBitRead(r, 6) + 1;

            if (r->overrun) {
                return false;
            }

            if (windowLead + meaningful > 64) {
                return false; /* window runs off the end of the word */
            }

            windowTrail = 64 - windowLead - meaningful;
            haveWindow = true;
        } else {
            if (!haveWindow) {
                return false; /* window reused before one was declared */
            }

            meaningful = 64 - windowLead - windowTrail;
        }

        const uint64_t payload = ddBitRead(r, meaningful);

        if (r->overrun) {
            return false;
        }

        previous ^= payload << windowTrail;
        values[i].hi = ddDoubleOfBits(previous);
    }

    return true;
}

/* ====================================================================
 * Measurement
 * ==================================================================== */

/* Bits the XOR stream would occupy, obtained by running the real
 * encoder against a writer with no buffer. */
static size_t ddMeasureHiXorBytes(const varintDD *values, size_t count) {
    ddBitWriter probe;

    ddBitWriterInit(&probe, NULL, 0);
    ddWriteHiXor(&probe, values, count);
    return ddBitWriterBytes(&probe);
}

static size_t ddMeasureLoBytes(const varintDD *values, size_t count,
                               uint32_t loMantissaBits, size_t *exactValues,
                               size_t *escapedLimbs) {
    ddBitWriter probe;
    size_t exact = 0;
    size_t escaped = 0;

    ddBitWriterInit(&probe, NULL, 0);

    for (size_t i = 0; i < count; i++) {
        const uint64_t loBits = ddBitsOfDouble(values[i].lo);

        /* The test is on the bit pattern, so a negative zero counts as
         * present and keeps its sign through the round trip. */
        if (loBits == 0) {
            exact++;
            continue;
        }

        if (loMantissaBits > 0) {
            ddWriteLoLimb(&probe, ddBitsOfDouble(values[i].hi), loBits,
                          loMantissaBits, &escaped);
        }
    }

    *exactValues = exact;
    *escapedLimbs = loMantissaBits > 0 ? escaped : 0;

    if (loMantissaBits == 0) {
        return 0; /* no bitmap and no bitstream are written at all */
    }

    return (count + 7) / 8 + ddBitWriterBytes(&probe);
}

bool varintDDStreamAnalyze(const varintDD *values, size_t count,
                           varintDDStreamHiMode hiMode, uint8_t loMantissaBits,
                           varintDDStreamMeta *meta) {
    if (meta == NULL || values == NULL) {
        return false;
    }

    if (loMantissaBits > VARINT_DD_STREAM_LOSSLESS ||
        hiMode > VARINT_DD_STREAM_HI_AUTO) {
        return false;
    }

    memset(meta, 0, sizeof(*meta));
    meta->count = count;
    meta->loMantissaBits = loMantissaBits;
    meta->maxRelativeError = varintDDStreamMaxRelativeError(loMantissaBits);
    meta->hiMode = VARINT_DD_STREAM_HI_RAW;

    const size_t header = 1 + (size_t)varintTaggedLen(count);

    if (count == 0) {
        meta->encodedSize = header;
        return false;
    }

    size_t hiBytes = count * 8;

    if (hiMode != VARINT_DD_STREAM_HI_RAW) {
        const size_t xorBytes = ddMeasureHiXorBytes(values, count);

        if (hiMode == VARINT_DD_STREAM_HI_XOR || xorBytes < hiBytes) {
            hiBytes = xorBytes;
            meta->hiMode = VARINT_DD_STREAM_HI_XOR;
        }
    }

    meta->hiBytes = hiBytes;
    meta->loBytes = ddMeasureLoBytes(values, count, loMantissaBits,
                                     &meta->exactValues, &meta->escapedLimbs);
    meta->encodedSize = header + meta->hiBytes + meta->loBytes;

    return meta->encodedSize < count * sizeof(varintDD);
}

/* ====================================================================
 * Encode
 * ==================================================================== */

size_t varintDDStreamEncode(uint8_t *dst, const varintDD *values, size_t count,
                            varintDDStreamHiMode hiMode, uint8_t loMantissaBits,
                            varintDDStreamMeta *meta) {
    if (dst == NULL) {
        return 0;
    }

    if (loMantissaBits > VARINT_DD_STREAM_LOSSLESS ||
        hiMode > VARINT_DD_STREAM_HI_AUTO) {
        return 0;
    }

    if (count > 0 && values == NULL) {
        return 0;
    }

    const size_t capacity = varintDDStreamMaxSize(count);

    /* A count so large that its own size bound wraps cannot be encoded
     * safely, because no caller could have allocated for it. */
    if (capacity == 0) {
        return 0;
    }

    size_t offset = 1; /* flags are written last, once AUTO has resolved */

    offset += (size_t)varintTaggedPut64(dst + offset, count);

    uint8_t resolvedHiMode = VARINT_DD_STREAM_HI_RAW;
    size_t hiBytes = 0;
    size_t loBytes = 0;
    size_t exactValues = 0;
    size_t escapedLimbs = 0;

    if (count > 0) {
        /* --- leading limbs --- */
        if (hiMode == VARINT_DD_STREAM_HI_RAW) {
            ddPackHiRaw(dst + offset, values, count);
            hiBytes = count * 8;
        } else {
            ddBitWriter w;

            ddBitWriterInit(&w, dst + offset, capacity - offset);
            ddWriteHiXor(&w, values, count);

            if (w.overflow) {
                return 0;
            }

            hiBytes = ddBitWriterFinish(&w);

            /* AUTO decides on the measured result, not an estimate.
             * Nothing has been written past this section yet, so losing
             * the bet costs one overwrite and no data movement. */
            if (hiMode == VARINT_DD_STREAM_HI_AUTO && hiBytes >= count * 8) {
                ddPackHiRaw(dst + offset, values, count);
                hiBytes = count * 8;
            } else {
                resolvedHiMode = VARINT_DD_STREAM_HI_XOR;
            }
        }

        offset += hiBytes;

        /* --- trailing limbs --- */
        if (loMantissaBits > 0) {
            const size_t bitmapBytes = (count + 7) / 8;

            if (capacity - offset < bitmapBytes) {
                return 0;
            }

            uint8_t *bitmap = dst + offset;
            ddBitWriter w;

            memset(bitmap, 0, bitmapBytes);
            ddBitWriterInit(&w, bitmap + bitmapBytes,
                            capacity - offset - bitmapBytes);

            /* One pass produces the bitmap and the bitstream together;
             * the bitmap is a byproduct of deciding what to emit. */
            for (size_t i = 0; i < count; i++) {
                const uint64_t loBits = ddBitsOfDouble(values[i].lo);

                if (loBits == 0) {
                    exactValues++;
                    continue;
                }

                bitmap[i / 8] |= (uint8_t)(UINT32_C(1) << (i % 8));
                ddWriteLoLimb(&w, ddBitsOfDouble(values[i].hi), loBits,
                              loMantissaBits, &escapedLimbs);
            }

            if (w.overflow) {
                return 0;
            }

            loBytes = bitmapBytes + ddBitWriterFinish(&w);
            offset += loBytes;
        } else {
            for (size_t i = 0; i < count; i++) {
                if (ddBitsOfDouble(values[i].lo) == 0) {
                    exactValues++;
                }
            }
        }
    }

    dst[0] = (uint8_t)(resolvedHiMode |
                       ((uint32_t)loMantissaBits << DD_STREAM_LO_BITS_SHIFT));

    if (meta != NULL) {
        memset(meta, 0, sizeof(*meta));
        meta->count = count;
        meta->encodedSize = offset;
        meta->hiBytes = hiBytes;
        meta->loBytes = loBytes;
        meta->exactValues = exactValues;
        meta->escapedLimbs = escapedLimbs;
        meta->maxRelativeError = varintDDStreamMaxRelativeError(loMantissaBits);
        meta->hiMode = resolvedHiMode;
        meta->loMantissaBits = loMantissaBits;
    }

    return offset;
}

/* ====================================================================
 * Decode
 * ==================================================================== */

size_t varintDDStreamGetCount(const uint8_t *src, size_t srcBytes) {
    if (src == NULL || srcBytes < 2) {
        return 0;
    }

    const uint8_t flags = src[0];

    if ((flags & DD_STREAM_HI_MODE_MASK) > VARINT_DD_STREAM_HI_XOR) {
        return 0;
    }

    if ((flags >> DD_STREAM_LO_BITS_SHIFT) > VARINT_DD_STREAM_LOSSLESS) {
        return 0;
    }

    uint64_t count = 0;

    if (varintTaggedGet(src + 1, ddTaggedLimit(srcBytes - 1), &count) ==
        VARINT_WIDTH_INVALID) {
        return 0;
    }

    return (size_t)count;
}

size_t varintDDStreamDecode(const uint8_t *src, size_t srcBytes,
                            varintDD *values, size_t maxCount) {
    if (src == NULL || values == NULL || srcBytes < 2) {
        return 0;
    }

    const uint8_t flags = src[0];
    const uint32_t hiMode = flags & DD_STREAM_HI_MODE_MASK;
    const uint32_t loMantissaBits = flags >> DD_STREAM_LO_BITS_SHIFT;

    if (hiMode > VARINT_DD_STREAM_HI_XOR ||
        loMantissaBits > VARINT_DD_STREAM_LOSSLESS) {
        return 0;
    }

    size_t offset = 1;
    uint64_t declaredCount = 0;
    const varintWidth countWidth = varintTaggedGet(
        src + offset, ddTaggedLimit(srcBytes - offset), &declaredCount);

    if (countWidth == VARINT_WIDTH_INVALID) {
        return 0;
    }

    offset += (size_t)countWidth;

    if (declaredCount == 0 || declaredCount > maxCount) {
        return 0;
    }

    const size_t count = (size_t)declaredCount;

    /* count came off the wire, so bound it before it multiplies. */
    if (count > (SIZE_MAX - offset) / 8) {
        return 0;
    }

    /* --- leading limbs, first, because the trailing ones need their
     *     exponents to reconstruct at all --- */
    if (hiMode == VARINT_DD_STREAM_HI_RAW) {
        if (srcBytes - offset < count * 8) {
            return 0;
        }

        ddUnpackHiRaw(values, src + offset, count);
        offset += count * 8;
    } else {
        ddBitReader r;

        ddBitReaderInit(&r, src + offset, srcBytes - offset);

        if (!ddReadHiXor(&r, values, count)) {
            return 0;
        }

        offset += ddBitReaderBytes(&r);
    }

    /* --- trailing limbs --- */
    if (loMantissaBits == 0) {
        for (size_t i = 0; i < count; i++) {
            values[i].lo = 0.0;
        }

        return count;
    }

    const size_t bitmapBytes = (count + 7) / 8;

    if (srcBytes - offset < bitmapBytes) {
        return 0;
    }

    const uint8_t *bitmap = src + offset;
    offset += bitmapBytes;

    /* Padding bits past the last value must be clear. Cheap to check,
     * and it catches a truncated or spliced stream early. */
    const uint32_t tailBits = (uint32_t)(count % 8);

    if (tailBits != 0 && (bitmap[bitmapBytes - 1] >> tailBits) != 0) {
        return 0;
    }

    ddBitReader r;
    ddBitReaderInit(&r, src + offset, srcBytes - offset);

    for (size_t i = 0; i < count; i++) {
        if ((bitmap[i / 8] & (uint8_t)(UINT32_C(1) << (i % 8))) == 0) {
            values[i].lo = 0.0;
            continue;
        }

        bool valid = true;
        const uint64_t loBits = ddReadLoLimb(&r, ddBitsOfDouble(values[i].hi),
                                             loMantissaBits, &valid);

        if (!valid) {
            return 0;
        }

        values[i].lo = ddDoubleOfBits(loBits);
    }

    return count;
}

/* ====================================================================
 * TESTS
 * ==================================================================== */
#if defined(VARINT_DD_STREAM_TEST) || defined(VARINT_DD_STREAM_FUZZ)

#include "ctest.h"
#include <stdlib.h>

static uint64_t ddsRandState;

static uint64_t ddsRand64(void) {
    /* splitmix64 */
    uint64_t z = (ddsRandState += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static double ddsRandDouble(int minExp, int maxExp) {
    const uint64_t mantissa = ddsRand64() & 0xFFFFFFFFFFFFFULL;
    const int span = maxExp - minExp + 1;
    const int exponent = minExp + (int)(ddsRand64() % (uint64_t)span);

    double v = ldexp(1.0 + (double)mantissa / 4503599627370496.0, exponent);

    if (ddsRand64() & 1) {
        v = -v;
    }

    return v;
}

/* A genuine normalized double-double, produced the way real ones are. */
static varintDD ddsRandDD(int minExp, int maxExp) {
    const varintDD a = varintDDFromDouble(ddsRandDouble(minExp, maxExp));
    const varintDD b = varintDDFromDouble(ddsRandDouble(minExp, maxExp));
    const varintDD c = varintDDFromDouble(ddsRandDouble(minExp, maxExp));
    return varintDDAdd(varintDDMul(a, b), c);
}

/* --------------------------------------------------------------------
 * Input corpus
 * --------------------------------------------------------------------
 * Round-trip testing on well-behaved data proves only that the happy
 * path works. Every pattern below targets something specific the wire
 * format has to survive: the escape hatch, the bitmap's sign
 * sensitivity, the XOR window, and the decoder's exponent validation. */

typedef enum ddsPattern {
    DDS_PATTERN_EXACT,         /* every trailing limb is +0.0 */
    DDS_PATTERN_GENERIC,       /* ordinary arithmetic results */
    DDS_PATTERN_MIXED,         /* some exact, some not */
    DDS_PATTERN_NEGATIVE_ZERO, /* -0.0 limbs: a bit pattern, not a value */
    DDS_PATTERN_DENORMAL_LO,   /* no usable exponent gap; must escape */
    DDS_PATTERN_NONFINITE_HI,  /* NaN and infinity in the leading limb */
    DDS_PATTERN_CONSTANT,      /* XOR should collapse this to nearly nothing */
    DDS_PATTERN_SMOOTH,        /* slowly varying: XOR's best case */
    DDS_PATTERN_UNNORMALIZED,  /* pairs that never went through normalize */
    DDS_PATTERN_RANDOM_BITS,   /* arbitrary bit patterns in both limbs */
    DDS_PATTERN_HUGE_GAP,      /* trailing limb far below the escape range */
    DDS_PATTERN_COUNT
} ddsPattern;

static const char *ddsPatternName(ddsPattern pattern) {
    static const char *const names[DDS_PATTERN_COUNT] = {
        "exact",        "generic",      "mixed",    "negative-zero",
        "denormal-lo",  "nonfinite-hi", "constant", "smooth",
        "unnormalized", "random-bits",  "huge-gap"};

    return names[pattern];
}

static void ddsFill(varintDD *values, size_t count, ddsPattern pattern) {
    double walk = 1.0;
    const varintDD constant = ddsRandDD(-10, 10);

    for (size_t i = 0; i < count; i++) {
        switch (pattern) {
        case DDS_PATTERN_EXACT:
            values[i] = varintDDFromDouble(ddsRandDouble(-40, 40));
            break;

        case DDS_PATTERN_GENERIC:
            values[i] = ddsRandDD(-40, 40);
            break;

        case DDS_PATTERN_MIXED:
            values[i] = (ddsRand64() & 1)
                            ? varintDDFromDouble(ddsRandDouble(-40, 40))
                            : ddsRandDD(-40, 40);
            break;

        case DDS_PATTERN_NEGATIVE_ZERO:
            values[i] = (varintDD){ddsRandDouble(-40, 40), -0.0};
            break;

        case DDS_PATTERN_DENORMAL_LO:
            values[i] =
                (varintDD){ddsRandDouble(-40, 40),
                           ldexp((double)(ddsRand64() & 0xFFFFF), -1074)};
            break;

        case DDS_PATTERN_NONFINITE_HI: {
            static const double specials[] = {(double)NAN, (double)INFINITY,
                                              -(double)INFINITY, 0.0, -0.0};
            values[i] =
                (varintDD){specials[ddsRand64() % 5],
                           (ddsRand64() & 1) ? 0.0 : ddsRandDouble(-40, 40)};
            break;
        }

        case DDS_PATTERN_CONSTANT:
            values[i] = constant;
            break;

        case DDS_PATTERN_SMOOTH:
            walk += ldexp((double)(int64_t)(ddsRand64() % 2001) - 1000.0, -20);
            values[i] = varintDDMul(varintDDFromDouble(walk),
                                    varintDDFromDouble(1.0000001));
            break;

        case DDS_PATTERN_UNNORMALIZED:
            /* deliberately violates |lo| <= ulp(hi)/2 */
            values[i] =
                (varintDD){ddsRandDouble(-10, 10), ddsRandDouble(-10, 10)};
            break;

        case DDS_PATTERN_RANDOM_BITS: {
            uint64_t hiBits = ddsRand64();
            uint64_t loBits = ddsRand64();
            values[i].hi = ddDoubleOfBits(hiBits);
            values[i].lo = ddDoubleOfBits(loBits);
            break;
        }

        case DDS_PATTERN_HUGE_GAP: {
            const double hi = ddsRandDouble(0, 40);
            values[i] = (varintDD){hi, ldexp(hi, -200)};
            break;
        }

        case DDS_PATTERN_COUNT:
        default:
            values[i] = varintDDZero();
            break;
        }
    }
}

/* Compare the raw bytes: a lossless round trip has to preserve NaN
 * payloads and the sign of a zero, neither of which == would notice. */
static bool ddsBitwiseEqual(const varintDD *a, const varintDD *b,
                            size_t count) {
    return count == 0 || memcmp(a, b, count * sizeof(varintDD)) == 0;
}

#ifdef VARINT_DD_STREAM_TEST

/* --------------------------------------------------------------------
 * Test: lossless round trip, plus the buffer contract
 * -------------------------------------------------------------------- */

#define DDS_GUARD_BYTES 32
#define DDS_GUARD_FILL 0xA7

static int ddsTestRoundTrip(void) {
    int err = 0;

    enum { MAX_COUNT = 300 };
    static varintDD source[MAX_COUNT];
    static varintDD decoded[MAX_COUNT];

    TEST("lossless round trip over the full input corpus");

    for (uint32_t pattern = 0; pattern < DDS_PATTERN_COUNT; pattern++) {
        for (size_t count = 0; count <= MAX_COUNT; count++) {
            /* keep the sweep dense at the small sizes where the bitmap
             * tail and the SIMD remainder live, then thin it out */
            if (count > 70 && count % 37 != 0) {
                continue;
            }

            static const varintDDStreamHiMode modes[] = {
                VARINT_DD_STREAM_HI_RAW, VARINT_DD_STREAM_HI_XOR,
                VARINT_DD_STREAM_HI_AUTO};

            for (size_t m = 0; m < 3; m++) {
                ddsFill(source, count, (ddsPattern)pattern);

                const size_t capacity = varintDDStreamMaxSize(count);
                uint8_t *buffer = malloc(capacity + DDS_GUARD_BYTES);

                if (buffer == NULL) {
                    ERRR("out of memory");
                    return err;
                }

                memset(buffer + capacity, DDS_GUARD_FILL, DDS_GUARD_BYTES);

                varintDDStreamMeta encodeMeta;
                const size_t written = varintDDStreamEncode(
                    buffer, source, count, modes[m], VARINT_DD_STREAM_LOSSLESS,
                    &encodeMeta);

                if (written == 0 && count > 0) {
                    ERR("encode failed for %s at count %zu mode %zu",
                        ddsPatternName((ddsPattern)pattern), count, m);
                    free(buffer);
                    return err;
                }

                if (written > capacity) {
                    ERR("encode wrote %zu bytes, past the %zu byte bound",
                        written, capacity);
                    free(buffer);
                    return err;
                }

                for (size_t g = 0; g < DDS_GUARD_BYTES; g++) {
                    if (buffer[capacity + g] != DDS_GUARD_FILL) {
                        ERR("encode ran past the end of its buffer for %s at "
                            "count %zu",
                            ddsPatternName((ddsPattern)pattern), count);
                        free(buffer);
                        return err;
                    }
                }

                /* Analyze must predict the encoder exactly, since both
                 * run the same measurement code. */
                varintDDStreamMeta analyzeMeta;
                varintDDStreamAnalyze(source, count, modes[m],
                                      VARINT_DD_STREAM_LOSSLESS, &analyzeMeta);

                if (count > 0 &&
                    (analyzeMeta.encodedSize != written ||
                     analyzeMeta.hiMode != encodeMeta.hiMode ||
                     analyzeMeta.hiBytes != encodeMeta.hiBytes ||
                     analyzeMeta.loBytes != encodeMeta.loBytes ||
                     analyzeMeta.exactValues != encodeMeta.exactValues ||
                     analyzeMeta.escapedLimbs != encodeMeta.escapedLimbs)) {
                    ERR("analyze disagreed with encode for %s at count %zu "
                        "(%zu vs %zu bytes)",
                        ddsPatternName((ddsPattern)pattern), count,
                        analyzeMeta.encodedSize, written);
                    free(buffer);
                    return err;
                }

                if (count == 0) {
                    free(buffer);
                    continue;
                }

                if (varintDDStreamGetCount(buffer, written) != count) {
                    ERR("GetCount misread the header for %s at count %zu",
                        ddsPatternName((ddsPattern)pattern), count);
                    free(buffer);
                    return err;
                }

                memset(decoded, 0, sizeof(decoded));

                const size_t got =
                    varintDDStreamDecode(buffer, written, decoded, MAX_COUNT);

                if (got != count) {
                    ERR("decode returned %zu, expected %zu, for %s", got, count,
                        ddsPatternName((ddsPattern)pattern));
                    free(buffer);
                    return err;
                }

                if (!ddsBitwiseEqual(source, decoded, count)) {
                    ERR("round trip was not bit exact for %s at count %zu "
                        "mode %zu",
                        ddsPatternName((ddsPattern)pattern), count, m);
                    free(buffer);
                    return err;
                }

                /* An output buffer one element short must be refused
                 * outright, not filled partially. */
                if (varintDDStreamDecode(buffer, written, decoded, count - 1) !=
                    0) {
                    ERR("decode accepted an undersized output buffer for %s",
                        ddsPatternName((ddsPattern)pattern));
                    free(buffer);
                    return err;
                }

                free(buffer);
            }
        }
    }

    return err;
}

/* --------------------------------------------------------------------
 * Test: the precision ladder
 * -------------------------------------------------------------------- */

static int ddsTestPrecisionLadder(void) {
    int err = 0;

    enum { COUNT = 2000 };
    static varintDD source[COUNT];
    static varintDD decoded[COUNT];

    TEST("every trailing-limb width honours its error bound");

    ddsFill(source, COUNT, DDS_PATTERN_GENERIC);

    const size_t capacity = varintDDStreamMaxSize(COUNT);
    uint8_t *buffer = malloc(capacity);

    if (buffer == NULL) {
        ERRR("out of memory");
        return err;
    }

    size_t previousSize = 0;

    for (uint32_t bits = 0; bits <= VARINT_DD_STREAM_LOSSLESS; bits++) {
        varintDDStreamMeta meta;
        const size_t written = varintDDStreamEncode(buffer, source, COUNT,
                                                    VARINT_DD_STREAM_HI_AUTO,
                                                    (uint8_t)bits, &meta);

        if (written == 0) {
            ERR("encode failed at %" PRIu32 " trailing bits", bits);
            free(buffer);
            return err;
        }

        if (varintDDStreamDecode(buffer, written, decoded, COUNT) != COUNT) {
            ERR("decode failed at %" PRIu32 " trailing bits", bits);
            free(buffer);
            return err;
        }

        /* More retained bits must never produce a smaller stream. */
        if (written < previousSize) {
            ERR("stream shrank from %zu to %zu when going to %" PRIu32 " bits",
                previousSize, written, bits);
            free(buffer);
            return err;
        }

        previousSize = written;

        const double bound = varintDDStreamMaxRelativeError((uint8_t)bits);

        for (size_t i = 0; i < COUNT; i++) {
            if (bits == VARINT_DD_STREAM_LOSSLESS) {
                if (memcmp(&source[i], &decoded[i], sizeof(varintDD)) != 0) {
                    ERR("lossless mode altered value %zu", i);
                    free(buffer);
                    return err;
                }

                continue;
            }

            /* error is entirely in the trailing limb */
            const double error = fabs(source[i].lo - decoded[i].lo);
            const double allowed = fabs(source[i].hi) * bound;

            if (error > allowed) {
                ERR("value %zu lost %.3e at %" PRIu32 " bits, bound was %.3e",
                    i, error, bits, allowed);
                free(buffer);
                return err;
            }

            /* Truncation is toward zero precisely so the result stays a
             * valid normalized double-double. */
            if (decoded[i].lo != 0.0 &&
                fabs(decoded[i].lo) > ldexp(fabs(decoded[i].hi), -52)) {
                ERR("value %zu decoded to an unnormalized pair at %" PRIu32
                    " bits",
                    i, bits);
                free(buffer);
                return err;
            }
        }
    }

    free(buffer);
    return err;
}

/* --------------------------------------------------------------------
 * Test: malformed input is refused, never followed
 * --------------------------------------------------------------------
 * Each candidate is copied into an exactly-sized heap block, so a read
 * even one byte past the declared length is a genuine heap overflow
 * that a sanitizer build will catch rather than silently tolerate. */

static int ddsTestMalformedInput(void) {
    int err = 0;

    enum { COUNT = 64, TRIALS = 400 };
    static varintDD source[COUNT];
    static varintDD decoded[COUNT * 4];

    TEST("truncated and corrupted streams are rejected safely");

    for (uint32_t pattern = 0; pattern < DDS_PATTERN_COUNT; pattern++) {
        ddsFill(source, COUNT, (ddsPattern)pattern);

        const size_t capacity = varintDDStreamMaxSize(COUNT);
        uint8_t *buffer = malloc(capacity);

        if (buffer == NULL) {
            ERRR("out of memory");
            return err;
        }

        const size_t written = varintDDStreamEncode(
            buffer, source, COUNT, VARINT_DD_STREAM_HI_AUTO,
            VARINT_DD_STREAM_LOSSLESS, NULL);

        /* --- every truncation --- */
        for (size_t prefix = 0; prefix <= written; prefix++) {
            uint8_t *exact = malloc(prefix > 0 ? prefix : 1);

            if (exact == NULL) {
                ERRR("out of memory");
                free(buffer);
                return err;
            }

            memcpy(exact, buffer, prefix);

            const size_t got =
                varintDDStreamDecode(exact, prefix, decoded, COUNT * 4);

            if (prefix < written && got == COUNT) {
                /* Succeeding on a short buffer would mean it read data
                 * that was not there. */
                ERR("decode succeeded on a %zu byte prefix of a %zu byte "
                    "stream (%s)",
                    prefix, written, ddsPatternName((ddsPattern)pattern));
                free(exact);
                free(buffer);
                return err;
            }

            (void)varintDDStreamGetCount(exact, prefix);
            free(exact);
        }

        /* --- random corruption --- */
        for (size_t trial = 0; trial < TRIALS; trial++) {
            uint8_t *corrupt = malloc(written);

            if (corrupt == NULL) {
                ERRR("out of memory");
                free(buffer);
                return err;
            }

            memcpy(corrupt, buffer, written);

            const size_t flips = 1 + (ddsRand64() % 8);

            for (size_t f = 0; f < flips; f++) {
                corrupt[ddsRand64() % written] ^= (uint8_t)(ddsRand64() & 0xFF);
            }

            /* The only requirement is that it does not read out of
             * bounds or report more values than it was given room for;
             * a corrupted stream may legitimately decode to garbage. */
            const size_t got =
                varintDDStreamDecode(corrupt, written, decoded, COUNT * 4);

            if (got > COUNT * 4) {
                ERR("decode reported %zu values with room for only %d", got,
                    COUNT * 4);
                free(corrupt);
                free(buffer);
                return err;
            }

            free(corrupt);
        }

        free(buffer);
    }

    /* --- degenerate headers --- */
    {
        static const uint8_t empty[1] = {0};

        if (varintDDStreamDecode(empty, 0, decoded, COUNT) != 0 ||
            varintDDStreamDecode(empty, 1, decoded, COUNT) != 0 ||
            varintDDStreamGetCount(empty, 0) != 0) {
            ERRR("an empty stream was not rejected");
            return err;
        }

        /* trailing-limb width past the legal maximum */
        const uint8_t badWidth[3] = {(uint8_t)(0 | (53u << 2)), 1, 0};

        if (varintDDStreamDecode(badWidth, sizeof(badWidth), decoded, COUNT) !=
                0 ||
            varintDDStreamGetCount(badWidth, sizeof(badWidth)) != 0) {
            ERRR("an illegal trailing-limb width was accepted");
            return err;
        }

        /* leading-limb mode 2 and 3 never appear on the wire */
        const uint8_t badMode[3] = {2, 1, 0};

        if (varintDDStreamDecode(badMode, sizeof(badMode), decoded, COUNT) !=
            0) {
            ERRR("an illegal leading-limb mode was accepted");
            return err;
        }
    }

    return err;
}

/* --------------------------------------------------------------------
 * Test: the modes actually earn their keep
 * -------------------------------------------------------------------- */

static int ddsTestCompressionBehaviour(void) {
    int err = 0;

    enum { COUNT = 4000 };
    static varintDD source[COUNT];

    TEST("each mode compresses the data it was designed for");

    const size_t capacity = varintDDStreamMaxSize(COUNT);
    uint8_t *buffer = malloc(capacity);

    if (buffer == NULL) {
        ERRR("out of memory");
        return err;
    }

    const size_t raw = COUNT * sizeof(varintDD);

    /* Values promoted from doubles carry no trailing limb at all, so
     * the bitmap is the entire cost of representing them. */
    {
        ddsFill(source, COUNT, DDS_PATTERN_EXACT);

        varintDDStreamMeta meta;
        const size_t written = varintDDStreamEncode(
            buffer, source, COUNT, VARINT_DD_STREAM_HI_AUTO,
            VARINT_DD_STREAM_LOSSLESS, &meta);

        if (meta.exactValues != COUNT) {
            ERR("expected %d exact values, counted %zu", COUNT,
                meta.exactValues);
            free(buffer);
            return err;
        }

        const double perValue = (double)written / COUNT;

        if (perValue > 8.2) {
            ERR("exact values cost %.2f bytes each, expected about 8.13",
                perValue);
            free(buffer);
            return err;
        }

        printf("\texact        %6.2f bytes/value (%.2fx)\n", perValue,
               (double)raw / (double)written);
    }

    /* A constant array is where the XOR chain should shine. */
    {
        ddsFill(source, COUNT, DDS_PATTERN_CONSTANT);

        const size_t written = varintDDStreamEncode(
            buffer, source, COUNT, VARINT_DD_STREAM_HI_AUTO,
            VARINT_DD_STREAM_LOSSLESS, NULL);
        const double perValue = (double)written / COUNT;

        if (perValue > 8.0) {
            ERR("constant data cost %.2f bytes/value, XOR should have "
                "collapsed the leading limbs",
                perValue);
            free(buffer);
            return err;
        }

        printf("\tconstant     %6.2f bytes/value (%.2fx)\n", perValue,
               (double)raw / (double)written);
    }

    /* Generic data is the honest case: the trailing mantissa is
     * incompressible, so only the exponent field and framing go away. */
    {
        ddsFill(source, COUNT, DDS_PATTERN_GENERIC);

        varintDDStreamMeta meta;
        const size_t written = varintDDStreamEncode(
            buffer, source, COUNT, VARINT_DD_STREAM_HI_AUTO,
            VARINT_DD_STREAM_LOSSLESS, &meta);
        const double perValue = (double)written / COUNT;

        if (perValue > 16.0) {
            ERR("generic data cost %.2f bytes/value, worse than storing it "
                "flat",
                perValue);
            free(buffer);
            return err;
        }

        printf("\tgeneric      %6.2f bytes/value (%.2fx), %zu escapes\n",
               perValue, (double)raw / (double)written, meta.escapedLimbs);
    }

    /* Smooth data exercises both halves at once. */
    {
        ddsFill(source, COUNT, DDS_PATTERN_SMOOTH);

        const size_t written = varintDDStreamEncode(
            buffer, source, COUNT, VARINT_DD_STREAM_HI_AUTO,
            VARINT_DD_STREAM_LOSSLESS, NULL);

        printf("\tsmooth       %6.2f bytes/value (%.2fx)\n",
               (double)written / COUNT, (double)raw / (double)written);
    }

    /* AUTO must never lose to the mode it is choosing between. */
    for (uint32_t pattern = 0; pattern < DDS_PATTERN_COUNT; pattern++) {
        ddsFill(source, COUNT, (ddsPattern)pattern);

        const size_t autoSize = varintDDStreamEncode(
            buffer, source, COUNT, VARINT_DD_STREAM_HI_AUTO,
            VARINT_DD_STREAM_LOSSLESS, NULL);
        const size_t rawSize =
            varintDDStreamEncode(buffer, source, COUNT, VARINT_DD_STREAM_HI_RAW,
                                 VARINT_DD_STREAM_LOSSLESS, NULL);
        const size_t xorSize =
            varintDDStreamEncode(buffer, source, COUNT, VARINT_DD_STREAM_HI_XOR,
                                 VARINT_DD_STREAM_LOSSLESS, NULL);

        const size_t best = rawSize < xorSize ? rawSize : xorSize;

        if (autoSize > best) {
            ERR("AUTO chose %zu bytes for %s when %zu was available", autoSize,
                ddsPatternName((ddsPattern)pattern), best);
            free(buffer);
            return err;
        }
    }

    free(buffer);
    return err;
}

#endif /* VARINT_DD_STREAM_TEST */

/* --------------------------------------------------------------------
 * Fuzzer
 * --------------------------------------------------------------------
 * Self-contained and deterministic, in the same shape as
 * varintPaletteFuzz: random shapes and widths through encode, decode,
 * and re-encode, asserting the invariants that must hold for any
 * input whatsoever. */

static int varintDDStreamFuzzRun(size_t iterations, uint64_t seed) {
    int err = 0;

    enum { MAX_COUNT = 512 };
    static varintDD source[MAX_COUNT];
    static varintDD decoded[MAX_COUNT];

    ddsRandState = seed;

    printf("varintDDStream fuzz: %zu iterations, seed %" PRIu64 "\n",
           iterations, (uint64_t)seed);

    for (size_t iteration = 0; iteration < iterations; iteration++) {
        const size_t count = (size_t)(ddsRand64() % MAX_COUNT) + 1;
        const ddsPattern pattern =
            (ddsPattern)(ddsRand64() % DDS_PATTERN_COUNT);
        const varintDDStreamHiMode mode =
            (varintDDStreamHiMode)(ddsRand64() % 3);
        const uint8_t bits = (uint8_t)(ddsRand64() % 53);

        ddsFill(source, count, pattern);

        const size_t capacity = varintDDStreamMaxSize(count);
        uint8_t *buffer = malloc(capacity);

        if (buffer == NULL) {
            ERRR("out of memory");
            return err;
        }

        varintDDStreamMeta meta;
        const size_t written =
            varintDDStreamEncode(buffer, source, count, mode, bits, &meta);

        if (written == 0 || written > capacity) {
            ERR("iteration %zu: encode produced %zu bytes (cap %zu)", iteration,
                written, capacity);
            free(buffer);
            return err;
        }

        const size_t got =
            varintDDStreamDecode(buffer, written, decoded, MAX_COUNT);

        if (got != count) {
            ERR("iteration %zu: decode returned %zu, expected %zu", iteration,
                got, count);
            free(buffer);
            return err;
        }

        if (bits == VARINT_DD_STREAM_LOSSLESS &&
            !ddsBitwiseEqual(source, decoded, count)) {
            ERR("iteration %zu: lossless round trip differed (%s, count %zu)",
                iteration, ddsPatternName(pattern), count);
            free(buffer);
            return err;
        }

        /* Re-encoding a decoded stream must reproduce it byte for byte,
         * which is a much stronger statement than the values merely
         * matching: it says the encoder is deterministic and that
         * decode recovered every bit the format carries. */
        {
            uint8_t *again = malloc(capacity);

            if (again == NULL) {
                ERRR("out of memory");
                free(buffer);
                return err;
            }

            const size_t rewritten = varintDDStreamEncode(
                again, decoded, count, (varintDDStreamHiMode)meta.hiMode, bits,
                NULL);

            if (rewritten != written || memcmp(again, buffer, written) != 0) {
                size_t at = 0;

                while (at < written && at < rewritten &&
                       again[at] == buffer[at]) {
                    at++;
                }

                ERR("iteration %zu: re-encoding was not idempotent (%s, "
                    "count %zu, mode %" PRIu32 ", %" PRIu32
                    " trailing bits): %zu bytes became "
                    "%zu, first difference at byte %zu",
                    iteration, ddsPatternName(pattern), count,
                    (uint32_t)meta.hiMode, (uint32_t)bits, written, rewritten,
                    at);
                free(again);
                free(buffer);
                return err;
            }

            free(again);
        }

        free(buffer);
    }

    return err;
}

#endif /* VARINT_DD_STREAM_TEST || VARINT_DD_STREAM_FUZZ */

#ifdef VARINT_DD_STREAM_TEST

int varintDDStreamTest(int argc, char *argv[]) {
    static int (*const suites[])(void) = {
        ddsTestRoundTrip,
        ddsTestPrecisionLadder,
        ddsTestMalformedInput,
        ddsTestCompressionBehaviour,
    };

    int err = 0;

    ddsRandState =
        argc > 1 ? strtoull(argv[1], NULL, 10) : 0x243F6A8885A308D3ULL;

    printf("varintDDStream: seed %" PRIu64 "\n", (uint64_t)ddsRandState);

    for (size_t i = 0; i < sizeof(suites) / sizeof(suites[0]); i++) {
        err += suites[i]();

        if (err) {
            break;
        }
    }

    if (!err) {
        err += varintDDStreamFuzzRun(2000, ddsRandState ^ 0x9E3779B9ULL);
    }

    TEST_FINAL_RESULT;
}

#endif /* VARINT_DD_STREAM_TEST */

#ifdef VARINT_DD_STREAM_FUZZ

int main(int argc, char *argv[]) {
    const size_t iterations =
        argc > 1 ? (size_t)strtoull(argv[1], NULL, 10) : 20000;
    const uint64_t seed =
        argc > 2 ? strtoull(argv[2], NULL, 10) : 0x243F6A8885A308D3ULL;

    const int err = varintDDStreamFuzzRun(iterations, seed);

    if (err) {
        printf("FUZZ FAILED with %d errors\n", err);
        return err;
    }

    printf("FUZZ PASSED\n");
    return 0;
}

#endif /* VARINT_DD_STREAM_FUZZ */
