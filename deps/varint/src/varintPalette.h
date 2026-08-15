#pragma once

#include "varint.h"
#include "varintTagged.h"

__BEGIN_DECLS

/* ====================================================================
 * Palette Huffman Encoding with Verbatim-Block Outlier Routing
 * ==================================================================== */
/* varint model Palette Huffman:
 *   Type encoded by: canonical Huffman over the <=16 most frequent values
 *   Size: ~(entropy bits per value) for palette-dominated data
 *   Layout: [count][paletteSize][palette...][codeLens][blockMask]
 *           [codedBlockBits: u16 LE per coded block]
 *           [verbatimLen][per verbatim block: width byte + fixed-width LE]
 *           [codedLen][huffman bitstream]
 *   The per-block bit lengths give every coded block an independent
 *   decode start (interleaved/parallel decode); verbatim blocks store
 *   values at their block's max byte width for branch-free unpacking
 *   Meaning: Frequency-skewed streams compress to their entropy; rare
 *            outliers are routed at block granularity, not per symbol
 *   Pros: Entropy coding for skewed distributions (beats DICT's
 *           fixed-width indices whenever frequencies are non-uniform)
 *         Branchless decode: flat terminal table, one lookup per symbol
 *         Outliers never touch the hot path (no escape codes)
 *   Cons: Whole blocks fall back to verbatim if any value is rare
 *         No random access; decode is sequential per block
 *
 * Design adapted from Cloudflare's Unweight report (Cf-TR-2026.04):
 *   - restrict the coded alphabet to a 16-value palette so every flat
 *     decode-table entry is terminal (single lookup, no second stage)
 *   - lift outlier handling from per-symbol escapes to block-level
 *     verbatim routing tracked in a bitmask
 *   - decode with a sliding bit window over a padded buffer so word
 *     boundary crossings never branch */

/* Values per block for coded/verbatim classification. */
#define VARINT_PALETTE_BLOCK_VALUES 64

/* Maximum palette entries (coded alphabet size). */
#define VARINT_PALETTE_MAX_SYMBOLS 16

/* Maximum canonical Huffman code length; inherent bound for a 16-leaf
 * tree, so the flat decode table is at most 2^15 single-byte entries. */
#define VARINT_PALETTE_MAX_CODE_BITS 15

/* Palette encoding metadata structure */
typedef struct varintPaletteMeta {
    size_t count;          /* Number of values in original data */
    size_t encodedSize;    /* Total encoded size in bytes */
    size_t codedBlocks;    /* Blocks fully expressible in the palette */
    size_t verbatimBlocks; /* Blocks routed to verbatim storage */
    uint8_t paletteSize;   /* Palette entries used (1-16) */
    uint8_t maxCodeBits;   /* Longest Huffman code length (1-15) */
} varintPaletteMeta;

/* Compile-time size guarantees to prevent regressions */
_Static_assert(sizeof(varintPaletteMeta) == 40,
               "varintPaletteMeta size changed! Expected 40 bytes "
               "(4×8-byte + 2×1-byte + 6 padding).");
_Static_assert(sizeof(varintPaletteMeta) <= 64,
               "varintPaletteMeta exceeds single cache line (64 bytes)! "
               "Keep palette metadata cache-friendly.");

/* Maximum possible encoded size (worst case: every block verbatim) */
static inline size_t varintPaletteMaxSize(size_t count) {
    /* count(<=9) + paletteSize(1) + palette(<=16*9) + lens(<=8)
     * + blockMask(ceil(blocks/8)) + codedBlockBits(2/block <= count/32)
     * + verbatim frame(<=9 + (1 + 64*8) per block ~ 8.02*count)
     * + coded frame(<=9 + ~1.9*count) — a value is verbatim OR coded,
     * so 9*count + count/16 + 256 dominates every mix. */
    return count * 9 + count / 16 + 256;
}

/* Analyze array and fill metadata structure without encoding.
 * Returns true if palette encoding would beat raw 8-byte storage */
bool varintPaletteAnalyze(const uint64_t *values, size_t count,
                          varintPaletteMeta *meta);

/* Encode array using palette Huffman with verbatim-block routing
 * dst: output buffer (must be at least varintPaletteMaxSize bytes)
 * values: input array of values
 * count: number of values
 * meta: optional metadata output (can be NULL)
 * Returns: number of bytes written to dst, or 0 on allocation failure */
size_t varintPaletteEncode(uint8_t *dst, const uint64_t *values, size_t count,
                           varintPaletteMeta *meta);

/* Decode palette-encoded array
 * src: input buffer containing palette-encoded data
 * srcBytes: total bytes available at src; decode never reads past this,
 *           so untrusted input is safe to pass directly
 * values: output buffer for decoded values
 * maxCount: maximum values that fit in output buffer
 * Returns: number of values decoded, or 0 on malformed/truncated input
 *          or if the output buffer is too small */
size_t varintPaletteDecode(const uint8_t *src, size_t srcBytes,
                           uint64_t *values, size_t maxCount);

/* Get count of original values from encoded data header.
 * Returns 0 for an empty stream OR a malformed/truncated header. */
size_t varintPaletteGetCount(const uint8_t *src, size_t srcBytes);

/* ====================================================================
 * Palette-of-Deltas Variant
 * ====================================================================
 * Palette coding over the wrapped (mod-2^64) first-difference
 * transform: [count:tagged][first:tagged][palette stream of deltas].
 * Captures streams whose GAPS form a skewed small alphabet — regular
 * timestamps with a few interval classes, paginated IDs with mixed
 * strides — where plain delta pays >=1 byte per gap and plain palette
 * sees high cardinality. Decode reverses with a wrapping prefix sum,
 * so any uint64 sequence round-trips exactly. Meta (when provided)
 * describes the inner delta-domain palette stream. */

static inline size_t varintPaletteDeltaMaxSize(size_t count) {
    /* count(<=9) + first(<=9) + inner stream over count-1 deltas */
    return 18 + varintPaletteMaxSize(count);
}

size_t varintPaletteDeltaEncode(uint8_t *dst, const uint64_t *values,
                                size_t count, varintPaletteMeta *meta);

/* Bounded like varintPaletteDecode; safe on untrusted input. */
size_t varintPaletteDeltaDecode(const uint8_t *src, size_t srcBytes,
                                uint64_t *values, size_t maxCount);

#ifdef VARINT_PALETTE_TEST
int varintPaletteTest(int argc, char *argv[]);
#endif

__END_DECLS
