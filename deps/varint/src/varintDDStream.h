#pragma once

#include "varint.h"
#include "varintDD.h"

__BEGIN_DECLS

/* ====================================================================
 * Double-Double Stream Compression
 * ==================================================================== */
/* varint model DD Stream:
 *   Type encoded by: leading limbs verbatim or XOR-chained, trailing
 *                    limbs by presence bitmap + exponent gap + mantissa
 *   Size: 8.1 to 16 bytes per value, tunable down to 8.1 by precision
 *   Layout: [flags:1][count:tagged][loBitmap][loBitstream][hiStream]
 *   Meaning: lossless (or precision-laddered) storage of 106-bit values
 *   Pros: The trailing limb of a normalized double-double is nearly
 *           free to store: its exponent is determined by the leading
 *           limb to within a few bits, and it is exactly zero for any
 *           value promoted from a plain double
 *         One knob spans the whole precision ladder, from full 106-bit
 *           lossless down to plain 53-bit doubles, in ~1-bit steps
 *         Decode is bounds-checked against a byte count, so untrusted
 *           input is safe to pass directly
 *   Cons: No random access; the trailing-limb bitstream is sequential
 *         Generic double-double data compresses only ~6% losslessly,
 *           because a full trailing mantissa is incompressible entropy
 *         Leading limbs only shrink when consecutive values are similar
 *
 * WHY THE TRAILING LIMB IS ALMOST FREE
 *
 * Normalization guarantees |lo| <= ulp(hi)/2, so if E is the raw IEEE
 * biased exponent field, E(lo) <= E(hi) - 53 always. Storing E(lo)
 * directly wastes 11 bits on information the leading limb already
 * carries. This codec stores the GAP instead:
 *
 *     g = E(hi) - E(lo) - 53      (>= 0 for any normalized value)
 *
 * and g is not merely small, it is geometrically distributed. For
 * values produced by arithmetic, lo/ulp(hi) is roughly uniform over
 * [0, 1/2], which puts g at 0 about half the time, 1 a quarter of the
 * time, and so on. Elias-gamma coding a geometric variable costs about
 * 2.3 bits on average rather than 11 - and gamma is already in this
 * library, in varintElias.h.
 *
 * The other half of the win needs no coding at all. A double-double
 * promoted from a double has lo == +0.0 exactly, as does any value
 * whose arithmetic happened to come out exact. Those cost one bitmap
 * bit each and nothing else, which is what takes an all-exact array
 * from 16 bytes per value to 8.13.
 *
 * WHAT THIS DOES NOT DO
 *
 * For fully generic double-double data the lossless win is small, and
 * saying so plainly is more useful than a flattering benchmark: a
 * trailing mantissa is 52 bits of near-uniform entropy and nothing
 * compresses it. The gap coding removes the exponent field and the
 * bitmap removes the exact values; what remains is the mantissa. The
 * real lever there is loMantissaBits, which trades precision for space
 * along a continuous ladder rather than in one all-or-nothing step. */

/* Leading-limb strategy. */
typedef enum varintDDStreamHiMode {
    /* Verbatim little-endian doubles: 8 bytes each, no assumptions.
     * The right answer whenever consecutive values are unrelated. */
    VARINT_DD_STREAM_HI_RAW = 0,

    /* XOR against the previous value, then strip the leading and
     * trailing zero bytes of the difference (the Gorilla encoding,
     * as used by varintDeltaDelta for integers). Slowly varying
     * sequences - trajectories, integrations, sampled signals - keep
     * their high bits identical and collapse to a few bits each. */
    VARINT_DD_STREAM_HI_XOR = 1,

    /* Encode with XOR, keep it only if it actually came out smaller
     * than verbatim. The comparison is exact, not estimated, because
     * the leading-limb stream is written last and can simply be
     * rewritten in place. */
    VARINT_DD_STREAM_HI_AUTO = 2,
} varintDDStreamHiMode;

/* Trailing-limb mantissa bits retained. This is the precision ladder:
 * every bit dropped costs one bit of storage per non-exact value and
 * doubles the reconstruction error.
 *
 *   52  lossless: bit-exact round trip of every input, including the
 *       sign of a negative zero trailing limb
 *   k   ~2^-(53+k) relative error
 *   0   trailing limbs are dropped entirely, along with the bitmap and
 *       the bitstream; the result is a plain double stream at 2^-53 */
#define VARINT_DD_STREAM_LOSSLESS 52
#define VARINT_DD_STREAM_DROP_LO 0

/* Encoding statistics. */
typedef struct varintDDStreamMeta {
    size_t count;            /* values encoded */
    size_t encodedSize;      /* total bytes */
    size_t hiBytes;          /* bytes spent on leading limbs */
    size_t loBytes;          /* bytes spent on the bitmap and bitstream */
    size_t exactValues;      /* values whose trailing limb was exactly +0.0 */
    size_t escapedLimbs;     /* trailing limbs that needed the raw fallback */
    double maxRelativeError; /* 0.0 when lossless */
    uint8_t hiMode;          /* varintDDStreamHiMode actually used */
    uint8_t loMantissaBits;  /* trailing mantissa bits retained */
} varintDDStreamMeta;

/* Compile-time size guarantees to prevent regressions */
_Static_assert(sizeof(varintDDStreamMeta) == 64,
               "varintDDStreamMeta size changed! Expected 64 bytes "
               "(7x8-byte + 2x1-byte + 6 padding).");
_Static_assert(sizeof(varintDDStreamMeta) <= 64,
               "varintDDStreamMeta exceeds a single cache line (64 bytes)! "
               "Keep stream metadata cache-friendly.");

/* Maximum possible encoded size, for pre-allocating an output buffer.
 *
 * Worst case per value is a trailing limb that escapes to raw storage
 * (13 bits of gamma plus 64 raw) alongside a leading limb whose XOR
 * form does not compress (2 control bits, 12 of window, 64 of payload).
 * Both round up to 10 bytes. */
static inline size_t varintDDStreamMaxSize(size_t count) {
    if (count == 0) {
        return 2; /* flags byte plus a single-byte zero count */
    }

    /* Returns 0 when the bound itself would wrap. Silently wrapping
     * would be worse than useless: a caller doing exactly what this
     * function is for - sizing its output buffer - would allocate a
     * small block and hand it to an encoder expecting a large one.
     * varintDDStreamEncode rejects such counts for the same reason. */
    if (count > (SIZE_MAX - 32) / 22) {
        return 0;
    }

    /* flags(1) + count(<=9) + bitmap(count/8) + lo(10/value)
     * + alignment(1) + hi(10/value) */
    return 21 * count + count / 8 + 32;
}

/* Reconstruction error ceiling for a given trailing-limb width.
 * Truncation is toward zero, which keeps |lo| <= ulp(hi)/2 intact, so
 * a decoded value is always still a valid normalized double-double. */
static inline double varintDDStreamMaxRelativeError(uint8_t loMantissaBits) {
    if (loMantissaBits >= VARINT_DD_STREAM_LOSSLESS) {
        return 0.0;
    }

    /* |lo| <= 2^-53 |hi|, and truncating its mantissa to k bits keeps
     * all but a 2^-k fraction of that. */
    return ldexp(1.0, -(53 + (int)loMantissaBits));
}

/* Measure what an encode would produce, without producing it.
 * Fills meta (hiMode is resolved, so AUTO reports what it would pick).
 * Returns true when encoding would beat a flat 16 bytes per value. */
bool varintDDStreamAnalyze(const varintDD *values, size_t count,
                           varintDDStreamHiMode hiMode, uint8_t loMantissaBits,
                           varintDDStreamMeta *meta);

/* Encode an array of double-double values.
 * dst: output buffer, at least varintDDStreamMaxSize(count) bytes
 * loMantissaBits: 0 to 52, see the precision ladder above
 * meta: optional statistics output, may be NULL
 * Returns bytes written, or 0 if loMantissaBits is out of range. */
size_t varintDDStreamEncode(uint8_t *dst, const varintDD *values, size_t count,
                            varintDDStreamHiMode hiMode, uint8_t loMantissaBits,
                            varintDDStreamMeta *meta);

/* Decode a stream.
 * srcBytes: bytes available at src. Decoding never reads beyond this,
 *           so untrusted input is safe to pass directly.
 * maxCount: capacity of values, in elements
 * Returns values decoded, or 0 on malformed, truncated, or too-large
 * input. A partial result is never reported as a success. */
size_t varintDDStreamDecode(const uint8_t *src, size_t srcBytes,
                            varintDD *values, size_t maxCount);

/* Value count from the header alone, without decoding the body.
 * Returns 0 for an empty stream OR a malformed one. */
size_t varintDDStreamGetCount(const uint8_t *src, size_t srcBytes);

#ifdef VARINT_DD_STREAM_TEST
int varintDDStreamTest(int argc, char *argv[]);
#endif

__END_DECLS
