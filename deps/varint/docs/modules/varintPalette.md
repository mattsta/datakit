# varintPalette: Palette Huffman with Verbatim-Block Outlier Routing

## Overview

**varintPalette** entropy-codes streams dominated by a small set of frequent values. It selects the ≤16 most frequent values as a **palette**, builds a canonical Huffman code over only those palette symbols, and classifies each 64-value block as **coded** (every value is in the palette) or **verbatim** (at least one value is not). Verbatim blocks are stored outside the bitstream (fixed-width or tagged, whichever is smaller per block), so the decode hot path is completely branchless: one flat-table lookup per symbol (or per symbol _pair_), no escape codes, no second-stage lookups.

The design is adapted from Cloudflare's Unweight technical report (Cf-TR-2026.04, "Unweight: Lossless MLP Weight Compression for LLM Inference"), which applies the same three ideas to BF16 exponent bytes on GPUs:

1. **Palette-restricted alphabet** — coding at most 16 symbols bounds the maximum Huffman code length at 15 bits, so the flat decode table (≤32768 one-byte entries) makes every entry terminal.
2. **Row/block-level outlier routing** — outliers are handled by classifying whole blocks as verbatim via a bitmask, instead of per-symbol escape codes that put branches in the decode loop (contrast with varintPFOR's per-value exception patching).
3. **Branchless sliding-window bitreader** — the decoder peeks a 32-bit big-endian window over a padded buffer, so word-boundary crossings never branch (the CPU analog of the report's `__funnelshift_r` reader).

## Key Characteristics

| Property        | Value                                                                |
| --------------- | -------------------------------------------------------------------- |
| Implementation  | Header (.h) + Compiled (.c)                                          |
| Encoding Format | palette + code lengths + block bitmask + verbatim + bitstream        |
| Best For        | Frequency-skewed streams with ≤16 dominant values                    |
| Compression     | ~entropy bits/value for palette-dominated data (down to 1 bit/value) |
| Random Access   | None; sequential block decode                                        |
| Decode          | Branchless: single flat-table lookup per symbol                      |

## Encoding Format

```
[count:tagged]
[paletteSize:1]                          m ∈ 1..16
[palette values: tagged × m]             frequency-descending order
[code lengths: ceil(m/2) bytes]          nibble per symbol, canonical Huffman
[block bitmask: ceil(numBlocks/8)]       bit set = verbatim block
[coded block bits: u16 LE × codedBlocks] bit length of each coded block
[verbatimBytes:tagged][verbatim blocks]  per block: [width:1][values]
[codedBytes:tagged][Huffman bitstream]   coded blocks only, MSB-first
```

Blocks are 64 values; the final block may be shorter. Canonical codes are derived from the lengths array alone, so encoder and decoder always agree without transmitting the codes themselves.

The per-coded-block bit lengths (~0.25 bits/value overhead) give every coded block an independent decode start — the decoder runs four bit cursors in lockstep (Huff0-style instruction-level parallelism), and the layout is multithread-ready.

Each verbatim block picks the smaller of two storages: **fixed width** (`width` = the block's max byte width 1–8; branch-free unpack, and a width-8 block decodes as a single `memcpy`) or **tagged varints** (`width` = 0; wins when one large outlier would inflate every neighbor's width).

## Performance (Apple M-series, 1M values, median of 21)

| Distribution                    | Ratio  | Encode     | Decode      |
| ------------------------------- | ------ | ---------- | ----------- |
| skewed (8-symbol geometric)     | 3.5%   | 206 Mval/s | 1029 Mval/s |
| constant                        | 2.0%   | 297 Mval/s | 1059 Mval/s |
| mixed (16 hot + outlier blocks) | 7.6%   | 183 Mval/s | 824 Mval/s  |
| unique (fully verbatim)         | 100.2% | 190 Mval/s | 4680 Mval/s |

Reproduce with `varintPaletteBench [values] [repeats]`. Key mechanisms: hash-based frequency counting with a radix-sort fallback, NEON block classification gated by palette coverage, a double-symbol decode table (two symbols per lookup when `2×maxCodeBits ≤ 14`), the four-way interleaved block decode, and a **high-cardinality sampling fast path** — a constant-cost 1024-value sample that, when ≥7/8 distinct (proving no 16-entry palette could cover any block), skips frequency counting and classification entirely and emits the all-verbatim plan directly (unique-data encode: 21 → 190 Mval/s, with zero measurable cost on other distributions). x86 builds use the scalar classifier; all other optimizations are architecture-neutral.

## Palette-of-Deltas Variant (`varintPaletteDelta*`)

A thin transform wrapper: `[count:tagged][first:tagged][palette stream over the wrapped mod-2⁶⁴ first differences]`. Decode reverses with a wrapping prefix sum, so **any** uint64 sequence round-trips — monotonicity is not required. It captures streams whose _gaps_ form a skewed small alphabet (timestamps with a few interval classes, IDs with mixed strides): plain delta pays ≥1 byte per gap and BP128_DELTA pays the max gap's bit width, while palette-of-deltas pays the gap entropy (~1.6 bits for a 90/10-style gap mix). Registered as `VARINT_CODEC_PALETTE_DELTA` in compete's default mask; compete arbitrates against DELTA/DoD/BP128_DELTA per stream as usual. The bounded `varintPaletteDeltaDecode(src, srcBytes, …)` is safe on untrusted input like the base codec.

## When It Wins

- **Skewed small alphabets**: status codes, enum columns, quantized sensor readings, opcode streams. Where `varintDict` charges every value a fixed-width index, varintPalette charges each value its entropy — a 90/10 skew costs ~0.8 bits/value instead of 4 bits.
- **Mostly-clean data with rare wild outliers**: outliers cost their own block (64 values stored verbatim) but never slow down or expand the coded stream.
- **Constant or near-constant streams**: a single-value stream costs 1 bit/value (though `varintRLE` or `varintStride` will usually win those outright — let `varintCompete` decide).

## When It Loses

- High-cardinality data (>16 values covering most of the stream) — every block goes verbatim and you pay tagged-varint size plus header overhead.
- Runs of repeated values — `varintRLE` collapses runs to constant size; palette still pays per value.
- Sortable/arithmetic structure — delta-family codecs exploit ordering; palette ignores it.

varintPalette participates in `varintCompete`'s default mask (`VARINT_CODEC_PALETTE`), so evidence-based selection handles these tradeoffs automatically.

## API

```c
/* Analysis without encoding: returns true if palette beats raw u64 storage */
varintPaletteMeta meta;
bool useful = varintPaletteAnalyze(values, count, &meta);

/* Encode */
uint8_t *buf = malloc(varintPaletteMaxSize(count));
size_t written = varintPaletteEncode(buf, values, count, &meta);

/* Decode — srcBytes-bounded and fully validated, safe on untrusted input */
uint64_t *out = malloc(count * sizeof(uint64_t));
size_t got = varintPaletteDecode(buf, written, out, count); /* == count */

/* Peek count without decoding */
size_t n = varintPaletteGetCount(buf, written);
```

`varintPaletteMeta` reports `paletteSize`, `maxCodeBits`, `codedBlocks`, `verbatimBlocks`, and the exact `encodedSize` (Analyze and Encode always agree byte-for-byte).

## Implementation Notes

- **Palette selection**: sort-and-run-scan frequency counting, keeping the 16 highest-frequency values (ties broken by ascending value for determinism).
- **Huffman construction**: classic two-smallest merge over ≤31 nodes; a 16-leaf tree cannot exceed depth 15, so no length-limiting pass is needed.
- **Flat decode table**: `2^maxBits` one-byte entries of `(palette_index << 4) | code_length`, built by range-filling each symbol's prefix span. Typically ≤256 bytes for realistic skews.
- **Safety**: every decode read is bounded by `srcBytes`; the decoder validates palette size, code lengths (including the Kraft inequality, so a corrupt header cannot overflow the table fill), per-block bit lengths against the coded section, and verbatim widths; it zero-fills tables so corrupt bitstreams resolve to in-range entries and decodes bits from a padded private copy. Hardened by `varintPaletteFuzz`, a self-contained deterministic fuzzer (`scripts/test/run_palette_fuzz.sh` runs long ASan+UBSan sessions; failures reproduce exactly from the printed seed).
