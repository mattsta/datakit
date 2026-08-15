#include "varintPalette.h"
#include "endianIsLittle.h"
#include <stdlib.h>
#include <string.h>

/* Big-endian 32-bit load/store over unaligned bytes: the wire format is
 * defined as an MSB-first byte sequence, so on little-endian hosts a
 * bswap turns one 4-byte access into the whole window operation. */
static inline uint32_t paletteLoad32BE_(const uint8_t *p) {
    uint32_t w;
    memcpy(&w, p, sizeof(w));
    return endianIsLittle() ? __builtin_bswap32(w) : w;
}

static inline void paletteStore32BE_(uint8_t *p, uint32_t w) {
    if (endianIsLittle()) {
        w = __builtin_bswap32(w);
    }
    memcpy(p, &w, sizeof(w));
}

/* ====================================================================
 * Internal encoding plan
 * ====================================================================
 * Everything both Analyze and Encode need: the palette, canonical code
 * assignment, per-block coded/verbatim classification, and exact section
 * sizes. Encode writes from the plan; Analyze only reports its totals. */

typedef struct palettePlan {
    uint64_t palVal[VARINT_PALETTE_MAX_SYMBOLS];
    uint64_t palFreq[VARINT_PALETTE_MAX_SYMBOLS];
    uint16_t palCode[VARINT_PALETTE_MAX_SYMBOLS];
    uint8_t palLen[VARINT_PALETTE_MAX_SYMBOLS];
    uint8_t m;
    uint8_t maxBits;
    size_t numBlocks;
    size_t verbatimBlocks;
    size_t codedBits;
    size_t verbatimBytes;
    uint8_t *blockMask;      /* bit set = verbatim block; caller frees */
    uint8_t *symIdx;         /* per-value palette index, filled ONLY for coded
                              * blocks (verbatim blocks abandon the scan early
                              * and leave their entries untouched); caller
                              * frees */
    uint32_t palCoveragePct; /* % of values that are palette members */
    uint16_t *blockBits;     /* per-block coded bit length (coded blocks
                              * only; verbatim entries unused) */
    uint8_t *blockWidth;     /* per-block verbatim byte width 1..8
                              * (verbatim blocks only) */
} palettePlan;

static void palettePlanFree_(palettePlan *plan) {
    free(plan->blockMask);
    free(plan->symIdx);
    free(plan->blockBits);
    free(plan->blockWidth);
}

/* Byte width (1..8) of a value's little-endian fixed representation. */
static inline uint8_t paletteByteWidth_(uint64_t v) {
    return (uint8_t)((64 - __builtin_clzll(v | 1) + 7) / 8);
}

/* Width-parameterized little-endian value I/O. On little-endian hosts
 * these compile to a masked memcpy; the byte loop is the BE fallback. */
static inline uint64_t paletteLoadLEWidth_(const uint8_t *p, uint8_t width) {
    if (endianIsLittle()) {
        uint64_t v = 0;
        memcpy(&v, p, width);
        return v;
    }
    uint64_t v = 0;
    for (uint8_t i = 0; i < width; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

static inline void paletteStoreLEWidth_(uint8_t *p, uint64_t v, uint8_t width) {
    if (endianIsLittle()) {
        memcpy(p, &v, width);
        return;
    }
    for (uint8_t i = 0; i < width; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

/* LSD radix sort for u64: O(n) with byte-wide passes, replacing qsort's
 * O(n log n) comparator overhead in the frequency scan. Passes whose
 * byte is constant across the array are skipped (very common: small
 * alphabets touch only a few low bytes). Result always lands back in
 * `a`; `tmp` is caller-provided ping-pong space of the same size. */
static void paletteRadixSortU64_(uint64_t *a, uint64_t *tmp, size_t n) {
    /* All eight histograms in ONE pass over the data, so trivial byte
     * columns (constant across the array — the common case for small
     * alphabets) cost nothing beyond this single read. */
    size_t hist[8][256];
    memset(hist, 0, sizeof(hist));
    for (size_t i = 0; i < n; i++) {
        const uint64_t v = a[i];
        hist[0][v & 0xFF]++;
        hist[1][(v >> 8) & 0xFF]++;
        hist[2][(v >> 16) & 0xFF]++;
        hist[3][(v >> 24) & 0xFF]++;
        hist[4][(v >> 32) & 0xFF]++;
        hist[5][(v >> 40) & 0xFF]++;
        hist[6][(v >> 48) & 0xFF]++;
        hist[7][(v >> 56) & 0xFF]++;
    }

    uint64_t *src = a;
    uint64_t *dst = tmp;
    for (int pass = 0; pass < 8; pass++) {
        const int shift = pass * 8;
        /* Column constant? Then this pass is the identity — skip. */
        if (hist[pass][(src[0] >> shift) & 0xFF] == n) {
            continue;
        }
        size_t start[256];
        size_t pos = 0;
        for (int b = 0; b < 256; b++) {
            start[b] = pos;
            pos += hist[pass][b];
        }
        for (size_t i = 0; i < n; i++) {
            dst[start[(src[i] >> shift) & 0xFF]++] = src[i];
        }
        uint64_t *swap = src;
        src = dst;
        dst = swap;
    }
    if (src != a) {
        memcpy(a, src, n * sizeof(*a));
    }
}

/* Linear probe of the palette; m <= 16 so this stays in registers, and
 * the palette is frequency-sorted so skewed data exits after ~1-2
 * probes. */
static inline int palFind_(const palettePlan *plan, uint64_t v) {
    for (int i = 0; i < plan->m; i++) {
        if (plan->palVal[i] == v) {
            return i;
        }
    }
    return -1;
}

#if defined(__ARM_NEON) && defined(__aarch64__)
#define VARINT_PALETTE_NEON 1
#include <arm_neon.h>

/* NEON classification of one FULL 64-value block: resolve every value
 * to its palette index (0xFF = not in palette) by sweeping broadcast
 * compares, then table-lookup the code lengths and detect outliers with
 * horizontal reductions. Unlike the scalar probe this cannot early-exit,
 * so it only pays off when average scalar probe depth is high (near-
 * uniform palettes); the benchmark gates whether it is used at all. */
static bool paletteClassifyBlockNEON_(const palettePlan *plan,
                                      const uint64_t *v, uint8_t *symIdx,
                                      size_t *bitsOut) {
    uint8_t idx8[VARINT_PALETTE_BLOCK_VALUES];

    for (size_t i = 0; i < VARINT_PALETTE_BLOCK_VALUES; i += 2) {
        const uint64x2_t vv = vld1q_u64(&v[i]);
        uint64x2_t acc = vdupq_n_u64(0xFF);
        for (int k = 0; k < plan->m; k++) {
            const uint64x2_t eq = vceqq_u64(vv, vdupq_n_u64(plan->palVal[k]));
            acc = vbslq_u64(eq, vdupq_n_u64((uint64_t)k), acc);
        }
        idx8[i] = (uint8_t)vgetq_lane_u64(acc, 0);
        idx8[i + 1] = (uint8_t)vgetq_lane_u64(acc, 1);
    }

    /* Outlier detection + code-length summation, 16 lanes at a time.
     * vqtbl1q returns 0 for out-of-range indices, so 0xFF lanes add
     * nothing to the bit count and are caught by the max reduction. */
    uint8_t lenTab[16] = {0};
    memcpy(lenTab, plan->palLen, plan->m);
    const uint8x16_t lens = vld1q_u8(lenTab);

    size_t bits = 0;
    bool verbatim = false;
    for (size_t c = 0; c < VARINT_PALETTE_BLOCK_VALUES; c += 16) {
        const uint8x16_t chunk = vld1q_u8(&idx8[c]);
        if (vmaxvq_u8(chunk) == 0xFF) {
            verbatim = true;
            break;
        }
        bits += vaddlvq_u8(vqtbl1q_u8(lens, chunk));
    }
    if (verbatim) {
        return false;
    }

    memcpy(symIdx, idx8, VARINT_PALETTE_BLOCK_VALUES);
    *bitsOut = bits;
    return true;
}
#endif /* __ARM_NEON && __aarch64__ */

/* Compute Huffman code lengths for m <= 16 weighted symbols.
 * Classic two-smallest merge over a flat node array (<= 31 nodes);
 * a 16-leaf tree cannot exceed depth 15, so no length limiting needed. */
static void paletteHuffLengths_(const uint64_t *freq, uint8_t m, uint8_t *len) {
    if (m == 1) {
        /* A single symbol still needs one bit so the coded stream has a
         * defined length per value. */
        len[0] = 1;
        return;
    }

    uint64_t f[2 * VARINT_PALETTE_MAX_SYMBOLS - 1];
    int16_t parent[2 * VARINT_PALETTE_MAX_SYMBOLS - 1];
    bool isRoot[2 * VARINT_PALETTE_MAX_SYMBOLS - 1];

    int n = m;
    for (int i = 0; i < m; i++) {
        f[i] = freq[i];
        parent[i] = -1;
        isRoot[i] = true;
    }

    for (int roots = m; roots > 1; roots--) {
        int a = -1;
        int b = -1;
        for (int i = 0; i < n; i++) {
            if (!isRoot[i]) {
                continue;
            }
            if (a < 0 || f[i] < f[a]) {
                b = a;
                a = i;
            } else if (b < 0 || f[i] < f[b]) {
                b = i;
            }
        }
        f[n] = f[a] + f[b];
        parent[n] = -1;
        isRoot[n] = true;
        parent[a] = (int16_t)n;
        parent[b] = (int16_t)n;
        isRoot[a] = false;
        isRoot[b] = false;
        n++;
    }

    for (int i = 0; i < m; i++) {
        uint8_t depth = 0;
        for (int16_t p = parent[i]; p >= 0; p = parent[p]) {
            depth++;
        }
        len[i] = depth;
    }
}

/* Assign canonical codes from lengths: codes increase with (length,
 * palette index), so encoder and decoder derive identical codes from the
 * lengths array alone. */
static void paletteCanonicalCodes_(const uint8_t *len, uint8_t m,
                                   uint8_t maxBits, uint16_t *code) {
    uint16_t blCount[VARINT_PALETTE_MAX_CODE_BITS + 1] = {0};
    for (int i = 0; i < m; i++) {
        blCount[len[i]]++;
    }

    uint16_t next[VARINT_PALETTE_MAX_CODE_BITS + 1];
    uint16_t c = 0;
    blCount[0] = 0;
    for (int b = 1; b <= maxBits; b++) {
        c = (uint16_t)((c + blCount[b - 1]) << 1);
        next[b] = c;
    }

    for (int b = 1; b <= maxBits; b++) {
        for (int i = 0; i < m; i++) {
            if (len[i] == b) {
                code[i] = next[b]++;
            }
        }
    }
}

/* Offer one (value, frequency) pair to the palette-in-progress. Callers
 * MUST offer pairs in ascending value order so tie-breaking (strictly
 * greater frequency evicts; ties keep the earlier = smaller value) is
 * identical across the hash and radix counting paths. */
static inline void paletteOffer_(palettePlan *plan, uint64_t v, uint64_t freq) {
    if (plan->m < VARINT_PALETTE_MAX_SYMBOLS) {
        plan->palVal[plan->m] = v;
        plan->palFreq[plan->m] = freq;
        plan->m++;
        return;
    }
    int min = 0;
    for (int k = 1; k < VARINT_PALETTE_MAX_SYMBOLS; k++) {
        if (plan->palFreq[k] < plan->palFreq[min]) {
            min = k;
        }
    }
    if (freq > plan->palFreq[min]) {
        plan->palVal[min] = v;
        plan->palFreq[min] = freq;
    }
}

/* Exact frequency counting via open addressing, for streams whose
 * distinct-value count fits the table. Returns the number of distinct
 * values on success, or 0 when the table overflows (caller falls back
 * to the radix-sort path). counts[slot] == 0 marks an empty slot (an
 * occupied slot's count is always >= 1). */
#define PALETTE_HASH_SLOTS 4096
#define PALETTE_HASH_MAX_DISTINCT 3072

static size_t paletteHashCount_(const uint64_t *values, size_t count,
                                uint64_t *keys, uint64_t *counts) {
    memset(counts, 0, PALETTE_HASH_SLOTS * sizeof(*counts));
    size_t distinct = 0;
    for (size_t i = 0; i < count; i++) {
        const uint64_t v = values[i];
        size_t h = (size_t)((v * 0x9E3779B97F4A7C15ULL) >> 52);
        while (counts[h] != 0 && keys[h] != v) {
            h = (h + 1) & (PALETTE_HASH_SLOTS - 1);
        }
        if (counts[h] == 0) {
            if (distinct == PALETTE_HASH_MAX_DISTINCT) {
                return 0;
            }
            keys[h] = v;
            distinct++;
        }
        counts[h]++;
    }
    return distinct;
}

static int cmpU64_(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Classify one block as verbatim and account its storage: fixed width
 * at the block max, or tagged varints, whichever is smaller. Shared by
 * normal classification and the high-cardinality fast path. */
static void paletteVerbatimPlanBlock_(palettePlan *plan, const uint64_t *values,
                                      size_t b, size_t start, size_t end) {
    plan->blockMask[b >> 3] |= (uint8_t)(1u << (b & 7));
    plan->verbatimBlocks++;
    uint8_t width = 1;
    size_t taggedBytes = 0;
    for (size_t v = start; v < end; v++) {
        const uint8_t bw = paletteByteWidth_(values[v]);
        if (bw > width) {
            width = bw;
        }
        taggedBytes += varintTaggedLenQuick(values[v]);
    }
    const size_t fixedBytes = (end - start) * (size_t)width;
    if (fixedBytes <= taggedBytes) {
        plan->blockWidth[b] = width;
        plan->verbatimBytes += 1 + fixedBytes;
    } else {
        plan->blockWidth[b] = 0; /* tagged mode */
        plan->verbatimBytes += 1 + taggedBytes;
    }
}

/* High-cardinality sampling pre-pass. Probes a fixed-size deterministic
 * sample (constant cost — ~0.1% of a 1M-value encode, and skipped
 * entirely below the count threshold, so "normal" encodes are
 * unaffected). Returns true when the stream is provably hopeless for
 * palette coding: if >= 7/8 of sampled values are distinct, the best
 * possible 16-entry palette covers ~<=15% of the stream, making the
 * probability of any fully-covered 64-value block ~0.15^64 — every
 * block is verbatim no matter which palette is chosen, so the full
 * frequency count and per-value classification are pure waste. */
#define PALETTE_SAMPLE_MIN_COUNT 4096
#define PALETTE_SAMPLE_SIZE 1024

static bool paletteSampleLooksUnique_(const uint64_t *values, size_t count,
                                      uint64_t *keys, uint64_t *counts) {
    memset(counts, 0, PALETTE_HASH_SLOTS * sizeof(*counts));
    const size_t stride = count / PALETTE_SAMPLE_SIZE;
    size_t distinct = 0;
    for (size_t s = 0; s < PALETTE_SAMPLE_SIZE; s++) {
        const uint64_t v = values[s * stride];
        size_t h = (size_t)((v * 0x9E3779B97F4A7C15ULL) >> 52);
        while (counts[h] != 0 && keys[h] != v) {
            h = (h + 1) & (PALETTE_HASH_SLOTS - 1);
        }
        if (counts[h] == 0) {
            keys[h] = v;
            distinct++;
        }
        counts[h]++;
    }
    return distinct >= PALETTE_SAMPLE_SIZE * 7 / 8;
}

/* Build the full plan: palette selection, code assignment, block
 * classification, section sizes. Returns false on allocation failure. */
static bool paletteBuildPlan_(const uint64_t *values, size_t count,
                              palettePlan *plan) {
    memset(plan, 0, sizeof(*plan));
    plan->numBlocks =
        (count + VARINT_PALETTE_BLOCK_VALUES - 1) / VARINT_PALETTE_BLOCK_VALUES;

    plan->blockMask = calloc((plan->numBlocks + 7) / 8, 1);
    plan->symIdx = malloc(count);
    plan->blockBits = malloc(plan->numBlocks * sizeof(*plan->blockBits));
    plan->blockWidth = malloc(plan->numBlocks);
    if (!plan->blockMask || !plan->symIdx || !plan->blockBits ||
        !plan->blockWidth) {
        return false;
    }

    /* Frequency scan, cheapest exact method first: hash counting for
     * small alphabets (one ~O(1)-probe pass), radix sort + run scan
     * when the distinct count overflows the hash table. */
    uint64_t *hash = malloc(2 * PALETTE_HASH_SLOTS * sizeof(*hash));
    if (!hash) {
        return false;
    }
    uint64_t *keys = hash;
    uint64_t *counts = hash + PALETTE_HASH_SLOTS;

    /* High-cardinality fast path: when a constant-cost sample proves no
     * 16-entry palette could cover any 64-value block, skip frequency
     * counting AND per-value classification entirely — declare every
     * block verbatim (always a valid lossless plan) under a degenerate
     * 1-symbol palette header. */
    if (count >= PALETTE_SAMPLE_MIN_COUNT &&
        paletteSampleLooksUnique_(values, count, keys, counts)) {
        free(hash);
        plan->m = 1;
        plan->palVal[0] = values[0];
        plan->palFreq[0] = 1;
        paletteHuffLengths_(plan->palFreq, 1, plan->palLen);
        plan->maxBits = 1;
        paletteCanonicalCodes_(plan->palLen, 1, 1, plan->palCode);
        for (size_t b = 0; b < plan->numBlocks; b++) {
            const size_t start = b * VARINT_PALETTE_BLOCK_VALUES;
            const size_t end = start + VARINT_PALETTE_BLOCK_VALUES < count
                                   ? start + VARINT_PALETTE_BLOCK_VALUES
                                   : count;
            paletteVerbatimPlanBlock_(plan, values, b, start, end);
        }
        return true;
    }

    const size_t distinct = paletteHashCount_(values, count, keys, counts);
    if (distinct > 0) {
        /* Compact occupied slots to sorted (value asc) pairs so offers
         * arrive in the same order the radix path would produce. */
        uint64_t *pairs = malloc(2 * distinct * sizeof(*pairs));
        if (!pairs) {
            free(hash);
            return false;
        }
        size_t np = 0;
        for (size_t s = 0; s < PALETTE_HASH_SLOTS; s++) {
            if (counts[s] != 0) {
                pairs[np++] = keys[s];
            }
        }
        qsort(pairs, np, sizeof(*pairs), cmpU64_);
        for (size_t k = 0; k < np; k++) {
            const uint64_t v = pairs[k];
            size_t h = (size_t)((v * 0x9E3779B97F4A7C15ULL) >> 52);
            while (keys[h] != v || counts[h] == 0) {
                h = (h + 1) & (PALETTE_HASH_SLOTS - 1);
            }
            paletteOffer_(plan, v, counts[h]);
        }
        free(pairs);
        free(hash);
    } else {
        free(hash);
        uint64_t *sorted = malloc(2 * count * sizeof(*sorted));
        if (!sorted) {
            return false;
        }
        memcpy(sorted, values, count * sizeof(*sorted));
        paletteRadixSortU64_(sorted, sorted + count, count);

        size_t i = 0;
        while (i < count) {
            const uint64_t v = sorted[i];
            size_t j = i;
            while (j < count && sorted[j] == v) {
                j++;
            }
            paletteOffer_(plan, v, j - i);
            i = j;
        }
        free(sorted);
    }

    /* Order palette by frequency descending (value ascending on ties) so
     * the layout is deterministic for identical inputs. */
    for (int a = 1; a < plan->m; a++) {
        const uint64_t fv = plan->palFreq[a];
        const uint64_t vv = plan->palVal[a];
        int b = a - 1;
        while (b >= 0 && (plan->palFreq[b] < fv ||
                          (plan->palFreq[b] == fv && plan->palVal[b] > vv))) {
            plan->palFreq[b + 1] = plan->palFreq[b];
            plan->palVal[b + 1] = plan->palVal[b];
            b--;
        }
        plan->palFreq[b + 1] = fv;
        plan->palVal[b + 1] = vv;
    }

    paletteHuffLengths_(plan->palFreq, plan->m, plan->palLen);
    for (int k = 0; k < plan->m; k++) {
        if (plan->palLen[k] > plan->maxBits) {
            plan->maxBits = plan->palLen[k];
        }
    }
    paletteCanonicalCodes_(plan->palLen, plan->m, plan->maxBits, plan->palCode);

#ifdef VARINT_PALETTE_NEON
    /* Palette coverage (% of values that are palette members) gates the
     * NEON classifier. Measured: branch-free NEON sweeps beat the scalar
     * early-exit probe at every probe depth (2.1x on skewed data), but
     * on low-coverage data most blocks are verbatim and scalar bails on
     * the first outlier while NEON always pays for the full block. */
    {
        uint64_t covered = 0;
        for (int k = 0; k < plan->m; k++) {
            covered += plan->palFreq[k];
        }
        plan->palCoveragePct = (uint32_t)(covered * 100 / count);
    }
#endif

    /* Classify blocks: a block is verbatim if any value falls outside
     * the palette; otherwise its exact coded bit cost is accumulated and
     * each value's palette index is cached so the bitstream writer never
     * repeats the palette probe. */
#ifdef VARINT_PALETTE_NEON
    bool useNeonClassify = plan->palCoveragePct >= 50;
#ifdef VARINT_PALETTE_FORCE_NEON_CLASSIFY
    useNeonClassify = true;
#endif
#ifdef VARINT_PALETTE_FORCE_SCALAR_CLASSIFY
    useNeonClassify = false;
#endif
#endif

    for (size_t b = 0; b < plan->numBlocks; b++) {
        const size_t start = b * VARINT_PALETTE_BLOCK_VALUES;
        const size_t end = start + VARINT_PALETTE_BLOCK_VALUES < count
                               ? start + VARINT_PALETTE_BLOCK_VALUES
                               : count;
        size_t bits = 0;
        bool verbatim = false;
#ifdef VARINT_PALETTE_NEON
        if (useNeonClassify && end - start == VARINT_PALETTE_BLOCK_VALUES) {
            verbatim = !paletteClassifyBlockNEON_(plan, &values[start],
                                                  &plan->symIdx[start], &bits);
        } else
#endif
        {
            for (size_t v = start; v < end; v++) {
                const int idx = palFind_(plan, values[v]);
                if (idx < 0) {
                    verbatim = true;
                    break;
                }
                plan->symIdx[v] = (uint8_t)idx;
                bits += plan->palLen[idx];
            }
        }
        if (verbatim) {
            paletteVerbatimPlanBlock_(plan, values, b, start, end);
        } else {
            plan->blockBits[b] = (uint16_t)bits;
            plan->codedBits += bits;
        }
    }

    return true;
}

/* ====================================================================
 * Public API
 * ==================================================================== */

bool varintPaletteAnalyze(const uint64_t *values, size_t count,
                          varintPaletteMeta *meta) {
    if (meta) {
        memset(meta, 0, sizeof(*meta));
        meta->count = count;
    }
    if (count == 0) {
        return false;
    }

    palettePlan plan;
    if (!paletteBuildPlan_(values, count, &plan)) {
        palettePlanFree_(&plan);
        return false;
    }

    size_t palBytes = 0;
    for (int i = 0; i < plan.m; i++) {
        palBytes += varintTaggedLenQuick(plan.palVal[i]);
    }
    const size_t maskBytes = (plan.numBlocks + 7) / 8;
    const size_t codedBytes = (plan.codedBits + 7) / 8;
    const size_t codedBlocks = plan.numBlocks - plan.verbatimBlocks;
    const size_t total =
        varintTaggedLenQuick((uint64_t)count) + 1 + palBytes +
        ((size_t)plan.m + 1) / 2 + maskBytes + 2 * codedBlocks +
        varintTaggedLenQuick((uint64_t)plan.verbatimBytes) +
        plan.verbatimBytes + varintTaggedLenQuick((uint64_t)codedBytes) +
        codedBytes;

    if (meta) {
        meta->encodedSize = total;
        meta->codedBlocks = plan.numBlocks - plan.verbatimBlocks;
        meta->verbatimBlocks = plan.verbatimBlocks;
        meta->paletteSize = plan.m;
        meta->maxCodeBits = plan.maxBits;
    }

    palettePlanFree_(&plan);
    return total < count * sizeof(uint64_t);
}

/* Streaming MSB-first bit writer. Codes are <= 15 bits and the
 * accumulator drains below 8 bits after each push, so 64 bits of state
 * never overflow. */
typedef struct paletteBitWriter {
    uint8_t *out;
    uint64_t acc;
    uint32_t nbits;
} paletteBitWriter;

static inline void paletteBwPush_(paletteBitWriter *w, uint32_t code,
                                  uint32_t len) {
    w->acc = (w->acc << len) | code;
    w->nbits += len;
    /* Drain four bytes at once; codes are <= 15 bits so the accumulator
     * never holds more than 46 bits before a drain. */
    if (w->nbits >= 32) {
        w->nbits -= 32;
        paletteStore32BE_(w->out, (uint32_t)(w->acc >> w->nbits));
        w->out += 4;
    }
}

static inline void paletteBwFlush_(paletteBitWriter *w) {
    while (w->nbits >= 8) {
        w->nbits -= 8;
        *w->out++ = (uint8_t)(w->acc >> w->nbits);
    }
    if (w->nbits) {
        *w->out++ = (uint8_t)(w->acc << (8 - w->nbits));
        w->nbits = 0;
    }
}

size_t varintPaletteEncode(uint8_t *dst, const uint64_t *values, size_t count,
                           varintPaletteMeta *meta) {
    if (meta) {
        memset(meta, 0, sizeof(*meta));
        meta->count = count;
    }

    uint8_t *p = dst;
    p += varintTaggedPut64(p, count);
    if (count == 0) {
        if (meta) {
            meta->encodedSize = (size_t)(p - dst);
        }
        return (size_t)(p - dst);
    }

    palettePlan plan;
    if (!paletteBuildPlan_(values, count, &plan)) {
        palettePlanFree_(&plan);
        return 0;
    }

    /* Palette section: size byte, values, nibble-packed code lengths. */
    *p++ = plan.m;
    for (int i = 0; i < plan.m; i++) {
        p += varintTaggedPut64(p, plan.palVal[i]);
    }
    for (int i = 0; i < plan.m; i += 2) {
        const uint8_t lo = plan.palLen[i];
        const uint8_t hi = (i + 1 < plan.m) ? plan.palLen[i + 1] : 0;
        *p++ = (uint8_t)(lo | (hi << 4));
    }

    /* Block routing bitmask. */
    const size_t maskBytes = (plan.numBlocks + 7) / 8;
    memcpy(p, plan.blockMask, maskBytes);
    p += maskBytes;

    /* Per-coded-block bit lengths (u16 LE, block order): lets the
     * decoder start every coded block independently — interleaved or
     * parallel decode instead of one serial bit cursor. */
    for (size_t b = 0; b < plan.numBlocks; b++) {
        if (plan.blockMask[b >> 3] & (1u << (b & 7))) {
            continue;
        }
        p[0] = (uint8_t)(plan.blockBits[b] & 0xFF);
        p[1] = (uint8_t)(plan.blockBits[b] >> 8);
        p += 2;
    }

    /* Verbatim section: length prefix, then per verbatim block a width
     * byte and its values at that fixed little-endian width. */
    p += varintTaggedPut64(p, plan.verbatimBytes);
    for (size_t b = 0; b < plan.numBlocks; b++) {
        if (!(plan.blockMask[b >> 3] & (1u << (b & 7)))) {
            continue;
        }
        const size_t start = b * VARINT_PALETTE_BLOCK_VALUES;
        const size_t end = start + VARINT_PALETTE_BLOCK_VALUES < count
                               ? start + VARINT_PALETTE_BLOCK_VALUES
                               : count;
        const uint8_t width = plan.blockWidth[b];
        *p++ = width;
        if (width == 0) {
            for (size_t v = start; v < end; v++) {
                p += varintTaggedPut64(p, values[v]);
            }
        } else {
            for (size_t v = start; v < end; v++) {
                paletteStoreLEWidth_(p, values[v], width);
                p += width;
            }
        }
    }

    /* Coded section: length prefix, then the Huffman bitstream over
     * coded blocks only. */
    const size_t codedBytes = (plan.codedBits + 7) / 8;
    p += varintTaggedPut64(p, codedBytes);

    paletteBitWriter w = {p, 0, 0};
    for (size_t b = 0; b < plan.numBlocks; b++) {
        if (plan.blockMask[b >> 3] & (1u << (b & 7))) {
            continue;
        }
        const size_t start = b * VARINT_PALETTE_BLOCK_VALUES;
        const size_t end = start + VARINT_PALETTE_BLOCK_VALUES < count
                               ? start + VARINT_PALETTE_BLOCK_VALUES
                               : count;
        for (size_t v = start; v < end; v++) {
            const uint8_t idx = plan.symIdx[v];
            paletteBwPush_(&w, plan.palCode[idx], plan.palLen[idx]);
        }
    }
    paletteBwFlush_(&w);
    p = w.out;

    if (meta) {
        meta->encodedSize = (size_t)(p - dst);
        meta->codedBlocks = plan.numBlocks - plan.verbatimBlocks;
        meta->verbatimBlocks = plan.verbatimBlocks;
        meta->paletteSize = plan.m;
        meta->maxCodeBits = plan.maxBits;
    }

    palettePlanFree_(&plan);
    return (size_t)(p - dst);
}

/* Bounded tagged-varint read: never touches bytes at or past `end`.
 * Returns bytes consumed, or 0 on truncation. */
static inline size_t paletteBoundedTagged_(const uint8_t *p, const uint8_t *end,
                                           uint64_t *val) {
    const ptrdiff_t rem = end - p;
    if (rem < 1) {
        return 0;
    }
    return varintTaggedGet(p, rem > 9 ? 9 : (int32_t)rem, val);
}

size_t varintPaletteGetCount(const uint8_t *src, size_t srcBytes) {
    uint64_t count;
    if (!src || paletteBoundedTagged_(src, src + srcBytes, &count) == 0) {
        return 0;
    }
    if (count > (uint64_t)SIZE_MAX) {
        return 0;
    }
    return (size_t)count;
}

/* Decode one coded block from its own bit offset. Used for the tail of
 * interleaved groups and for streams too small for the pair table. */
static void paletteDecodeCodedBlock_(const uint8_t *coded, const uint8_t *table,
                                     const uint16_t *pair, const uint64_t *pal,
                                     uint32_t maxBits, uint32_t pairBits,
                                     size_t bitPos, uint64_t *values,
                                     size_t out, size_t n) {
    size_t i = 0;
    if (pair) {
        while (i + 1 < n) {
            const uint32_t window = paletteLoad32BE_(coded + (bitPos >> 3));
            const uint32_t w2 = (window << (bitPos & 7)) >> (32 - pairBits);
            const uint16_t ent = pair[w2];
            const size_t two = (ent >> 14) & 1;
            values[out] = pal[ent & 0x0F];
            if (two) {
                values[out + 1] = pal[(ent >> 4) & 0x0F];
            }
            out += 1 + two;
            i += 1 + two;
            bitPos += (ent >> 8) & 0x3F;
        }
    }
    for (; i < n; i++) {
        const uint32_t window = paletteLoad32BE_(coded + (bitPos >> 3));
        const uint32_t prefix = (window << (bitPos & 7)) >> (32 - maxBits);
        const uint8_t entry = table[prefix];
        values[out++] = pal[entry >> 4];
        bitPos += entry & 0x0F;
    }
}

size_t varintPaletteDecode(const uint8_t *src, size_t srcBytes,
                           uint64_t *values, size_t maxCount) {
    if (!src) {
        return 0;
    }
    const uint8_t *p = src;
    const uint8_t *const end = src + srcBytes;

    uint64_t count64;
    size_t w = paletteBoundedTagged_(p, end, &count64);
    if (w == 0) {
        return 0;
    }
    p += w;
    if (count64 == 0) {
        return 0;
    }
    /* Compare in the u64 domain BEFORE any size_t cast so a lying count
     * can't truncate on 32-bit targets. */
    if (count64 > (uint64_t)maxCount) {
        return 0;
    }
    const size_t count = (size_t)count64;

    if (end - p < 1) {
        return 0;
    }
    const uint8_t m = *p++;
    if (m == 0 || m > VARINT_PALETTE_MAX_SYMBOLS) {
        return 0;
    }

    uint64_t pal[VARINT_PALETTE_MAX_SYMBOLS];
    for (int i = 0; i < m; i++) {
        w = paletteBoundedTagged_(p, end, &pal[i]);
        if (w == 0) {
            return 0;
        }
        p += w;
    }

    const size_t lenBytes = ((size_t)m + 1) / 2;
    if ((size_t)(end - p) < lenBytes) {
        return 0;
    }
    uint8_t len[VARINT_PALETTE_MAX_SYMBOLS];
    for (int i = 0; i < m; i += 2) {
        const uint8_t packed = *p++;
        len[i] = packed & 0x0F;
        if (i + 1 < m) {
            len[i + 1] = packed >> 4;
        }
    }

    uint8_t maxBits = 0;
    for (int i = 0; i < m; i++) {
        if (len[i] == 0 || len[i] > VARINT_PALETTE_MAX_CODE_BITS) {
            return 0;
        }
        if (len[i] > maxBits) {
            maxBits = len[i];
        }
    }

    /* Kraft inequality: the claimed lengths must form a prefix code that
     * fits the flat table, or the fill below would write out of bounds.
     * '>' rather than '!=' because a 1-symbol palette legitimately
     * half-fills its table (forced length 1). */
    uint32_t kraft = 0;
    for (int i = 0; i < m; i++) {
        kraft += 1u << (maxBits - len[i]);
    }
    if (kraft > (1u << maxBits)) {
        return 0;
    }

    uint16_t code[VARINT_PALETTE_MAX_SYMBOLS];
    paletteCanonicalCodes_(len, m, maxBits, code);

    /* Pair table: when two max-length codes fit a 14-bit window and the
     * stream is long enough to amortize the build, index by 2*maxBits
     * bits and decode two symbols per lookup, halving the serial
     * bit-cursor dependency chain. Entry layout:
     *   bits 0-3   first palette index
     *   bits 4-7   second palette index (pair entries only)
     *   bits 8-13  total bits consumed
     *   bit  14    1 = two symbols, 0 = one symbol
     * In a Kraft-complete table every window yields two symbols; the
     * single-symbol form exists for the half-filled m==1 table and for
     * corrupt streams (where it stays harmlessly in range). */
    const uint32_t pairBits = 2u * maxBits;
    const bool usePairs = pairBits <= 14 && count >= 4096;

    const size_t numBlocks =
        (count + VARINT_PALETTE_BLOCK_VALUES - 1) / VARINT_PALETTE_BLOCK_VALUES;
    const size_t maskBytes = (numBlocks + 7) / 8;
    if ((size_t)(end - p) < maskBytes) {
        return 0;
    }
    const uint8_t *mask = p;
    p += maskBytes;

    size_t verbBlocks = 0;
    for (size_t b = 0; b < numBlocks; b++) {
        verbBlocks += (mask[b >> 3] >> (b & 7)) & 1;
    }
    const size_t codedCount = numBlocks - verbBlocks;

    /* Per-coded-block bit lengths (u16 LE, block order). Every length
     * and their total are validated against the coded section BEFORE
     * any offset is trusted, so pass 2's cursors are in bounds by
     * construction. */
    if ((size_t)(end - p) < 2 * codedCount) {
        return 0;
    }
    const uint8_t *blockBitsRaw = p;
    p += 2 * codedCount;
    uint64_t totalBits = 0;
    for (size_t c = 0; c < codedCount; c++) {
        const uint16_t bb =
            (uint16_t)(blockBitsRaw[2 * c] |
                       ((uint16_t)blockBitsRaw[2 * c + 1] << 8));
        if (bb > VARINT_PALETTE_BLOCK_VALUES * VARINT_PALETTE_MAX_CODE_BITS) {
            return 0;
        }
        totalBits += bb;
    }

    uint64_t verbatimBytes;
    w = paletteBoundedTagged_(p, end, &verbatimBytes);
    if (w == 0) {
        return 0;
    }
    p += w;
    if (verbatimBytes > (uint64_t)(end - p)) {
        return 0;
    }
    const uint8_t *verb = p;
    const uint8_t *verbEnd = p + verbatimBytes;
    p += verbatimBytes;

    uint64_t codedBytes;
    w = paletteBoundedTagged_(p, end, &codedBytes);
    if (w == 0) {
        return 0;
    }
    p += w;
    /* Must hold before the allocation below: caps both the alloc size
     * and the memcpy source range. */
    if (codedBytes > (uint64_t)(end - p)) {
        return 0;
    }
    if ((totalBits + 7) / 8 > codedBytes) {
        return 0;
    }

    /* Pass 1: verbatim blocks. Pure fixed-width unpack, needs no tables
     * or allocations, and runs before any so failures are plain
     * returns. */
    for (size_t b = 0; b < numBlocks; b++) {
        if (!(mask[b >> 3] & (1u << (b & 7)))) {
            continue;
        }
        const size_t start = b * VARINT_PALETTE_BLOCK_VALUES;
        const size_t n = (count - start) < VARINT_PALETTE_BLOCK_VALUES
                             ? (count - start)
                             : VARINT_PALETTE_BLOCK_VALUES;
        if (verb >= verbEnd) {
            return 0;
        }
        const uint8_t width = *verb++;
        if (width > 8) {
            return 0;
        }
        if (width == 0) {
            /* Tagged mode: mixed-magnitude block. */
            for (size_t i = 0; i < n; i++) {
                w = paletteBoundedTagged_(verb, verbEnd, &values[start + i]);
                if (w == 0) {
                    return 0;
                }
                verb += w;
            }
        } else {
            if ((size_t)(verbEnd - verb) < n * width) {
                return 0;
            }
            if (endianIsLittle() && width == 8) {
                /* Whole block is the output representation already. */
                memcpy(&values[start], verb, n * 8);
            } else if (endianIsLittle()) {
                /* Masked 8-byte loads (one load per value instead of a
                 * variable-length memcpy call). A load at value i spans
                 * bytes [i*width, i*width+8), so it stays inside the
                 * section only while i*width + 8 <= n*width; the rest
                 * take the exact-width path. */
                const uint64_t vmask = (1ULL << (8 * width)) - 1;
                const size_t nFast =
                    (n * width >= 8) ? (n * width - 8) / width + 1 : 0;
                for (size_t i = 0; i < nFast; i++) {
                    uint64_t v;
                    memcpy(&v, verb + i * width, 8);
                    values[start + i] = v & vmask;
                }
                for (size_t i = nFast; i < n; i++) {
                    values[start + i] =
                        paletteLoadLEWidth_(verb + i * width, width);
                }
            } else {
                for (size_t i = 0; i < n; i++) {
                    values[start + i] =
                        paletteLoadLEWidth_(verb + i * width, width);
                }
            }
            verb += n * width;
        }
    }

    if (codedCount == 0) {
        return count;
    }

    /* All header validation passed — only now allocate. */

    /* Flat terminal decode table: every maxBits-wide prefix resolves to
     * (palette index << 4 | code length) in one lookup. Zero-filled so a
     * corrupt bitstream resolves to a harmless in-range entry. */
    uint8_t *table = calloc((size_t)1 << maxBits, 1);
    if (!table) {
        return 0;
    }
    for (int i = 0; i < m; i++) {
        const size_t fill = (size_t)1 << (maxBits - len[i]);
        const size_t base = (size_t)code[i] << (maxBits - len[i]);
        memset(table + base, (i << 4) | len[i], fill);
    }

    uint16_t *pair = NULL;
    if (usePairs) {
        const uint32_t n2 = 1u << pairBits;
        pair = malloc((size_t)n2 * sizeof(*pair));
        if (pair) {
            for (uint32_t w2 = 0; w2 < n2; w2++) {
                const uint8_t e1 = table[w2 >> maxBits];
                const uint32_t l1 = e1 & 0x0F;
                uint16_t ent = (uint16_t)(e1 >> 4); /* single, 0 bits */
                if (l1 != 0) {
                    const uint32_t rest = (w2 << l1) & (n2 - 1);
                    const uint8_t e2 = table[rest >> maxBits];
                    const uint32_t l2 = e2 & 0x0F;
                    if (l2 != 0) {
                        ent =
                            (uint16_t)((e1 >> 4) | ((uint32_t)(e2 >> 4) << 4) |
                                       ((l1 + l2) << 8) | (1u << 14));
                    } else {
                        ent = (uint16_t)((e1 >> 4) | (l1 << 8));
                    }
                }
                pair[w2] = ent;
            }
        }
    }

    /* Padded copy of the coded stream so the 32-bit peek window (and a
     * whole block of worst-case reads past a corrupt length) never
     * touches memory outside the buffer. */
    const size_t pad =
        VARINT_PALETTE_BLOCK_VALUES * VARINT_PALETTE_MAX_CODE_BITS / 8 + 8;
    uint8_t *coded = calloc((size_t)codedBytes + pad, 1);
    size_t *cb = malloc(codedCount * 2 * sizeof(*cb));
    if (!coded || !cb) {
        free(pair);
        free(table);
        free(coded);
        free(cb);
        return 0;
    }
    memcpy(coded, p, codedBytes);

    /* Coded-block cursor setup: bit offsets by prefix sum of the
     * validated per-block lengths, output offsets from block index. */
    size_t *cbBit = cb;
    size_t *cbOut = cb + codedCount;
    {
        size_t bit = 0;
        size_t c = 0;
        for (size_t b = 0; b < numBlocks; b++) {
            if (mask[b >> 3] & (1u << (b & 7))) {
                continue;
            }
            cbBit[c] = bit;
            cbOut[c] = b * VARINT_PALETTE_BLOCK_VALUES;
            bit += (size_t)(blockBitsRaw[2 * c] |
                            ((uint16_t)blockBitsRaw[2 * c + 1] << 8));
            c++;
        }
    }

    /* Pass 2: coded blocks. Independent bit offsets let four cursors
     * run in lockstep — four loads/lookups in flight per iteration
     * instead of one serial dependency chain (Huff0-style ILP). */
    size_t g = 0;
    if (pair) {
        for (; g + 4 <= codedCount; g += 4) {
            size_t bp[4];
            size_t op[4];
            size_t rem[4];
            for (int j = 0; j < 4; j++) {
                bp[j] = cbBit[g + j];
                op[j] = cbOut[g + j];
                rem[j] = (count - op[j]) < VARINT_PALETTE_BLOCK_VALUES
                             ? (count - op[j])
                             : VARINT_PALETTE_BLOCK_VALUES;
            }
            while (rem[0] >= 2 && rem[1] >= 2 && rem[2] >= 2 && rem[3] >= 2) {
                for (int j = 0; j < 4; j++) {
                    const uint32_t window =
                        paletteLoad32BE_(coded + (bp[j] >> 3));
                    const uint32_t w2 =
                        (window << (bp[j] & 7)) >> (32 - pairBits);
                    const uint16_t ent = pair[w2];
                    const size_t two = (ent >> 14) & 1;
                    values[op[j]] = pal[ent & 0x0F];
                    if (two) {
                        values[op[j] + 1] = pal[(ent >> 4) & 0x0F];
                    }
                    op[j] += 1 + two;
                    rem[j] -= 1 + two;
                    bp[j] += (ent >> 8) & 0x3F;
                }
            }
            for (int j = 0; j < 4; j++) {
                paletteDecodeCodedBlock_(coded, table, pair, pal, maxBits,
                                         pairBits, bp[j], values, op[j],
                                         rem[j]);
            }
        }
    }
    for (; g < codedCount; g++) {
        const size_t out0 = cbOut[g];
        const size_t n = (count - out0) < VARINT_PALETTE_BLOCK_VALUES
                             ? (count - out0)
                             : VARINT_PALETTE_BLOCK_VALUES;
        paletteDecodeCodedBlock_(coded, table, pair, pal, maxBits, pairBits,
                                 cbBit[g], values, out0, n);
    }

    free(cb);
    free(pair);
    free(table);
    free(coded);
    return count;
}

/* ====================================================================
 * Palette-of-Deltas Variant
 * ==================================================================== */

size_t varintPaletteDeltaEncode(uint8_t *dst, const uint64_t *values,
                                size_t count, varintPaletteMeta *meta) {
    if (meta) {
        memset(meta, 0, sizeof(*meta));
        meta->count = count;
    }

    uint8_t *p = dst;
    p += varintTaggedPut64(p, count);
    if (count == 0) {
        if (meta) {
            meta->encodedSize = (size_t)(p - dst);
        }
        return (size_t)(p - dst);
    }

    p += varintTaggedPut64(p, values[0]);
    if (count == 1) {
        if (meta) {
            meta->encodedSize = (size_t)(p - dst);
        }
        return (size_t)(p - dst);
    }

    /* Wrapped first differences: unsigned subtraction is mod-2^64, and
     * the decoder's prefix sum wraps identically, so ANY sequence
     * round-trips — no monotonicity requirement. */
    uint64_t *deltas = malloc((count - 1) * sizeof(*deltas));
    if (!deltas) {
        return 0;
    }
    for (size_t i = 1; i < count; i++) {
        deltas[i - 1] = values[i] - values[i - 1];
    }

    const size_t inner = varintPaletteEncode(p, deltas, count - 1, meta);
    free(deltas);
    if (inner == 0) {
        return 0;
    }
    if (meta) {
        /* Inner meta describes the delta-domain stream; restore the
         * caller-visible frame totals. */
        meta->count = count;
        meta->encodedSize = (size_t)(p - dst) + inner;
    }
    return (size_t)(p - dst) + inner;
}

size_t varintPaletteDeltaDecode(const uint8_t *src, size_t srcBytes,
                                uint64_t *values, size_t maxCount) {
    if (!src) {
        return 0;
    }
    const uint8_t *p = src;
    const uint8_t *const end = src + srcBytes;

    uint64_t count64;
    size_t w = paletteBoundedTagged_(p, end, &count64);
    if (w == 0) {
        return 0;
    }
    p += w;
    if (count64 == 0) {
        return 0;
    }
    if (count64 > (uint64_t)maxCount) {
        return 0;
    }
    const size_t count = (size_t)count64;

    uint64_t first;
    w = paletteBoundedTagged_(p, end, &first);
    if (w == 0) {
        return 0;
    }
    p += w;
    values[0] = first;
    if (count == 1) {
        return 1;
    }

    /* Decode deltas into values[1..] then prefix-sum in place. */
    const size_t got =
        varintPaletteDecode(p, (size_t)(end - p), values + 1, count - 1);
    if (got != count - 1) {
        return 0;
    }
    for (size_t i = 1; i < count; i++) {
        values[i] += values[i - 1];
    }
    return count;
}

/* ====================================================================
 * Unit Tests
 * ==================================================================== */
#ifdef VARINT_PALETTE_TEST
#include "ctest.h"
#include <stdio.h>

/* Deterministic LCG so tests never depend on platform rand(). */
static uint64_t testRngState_ = 0x2545F4914F6CDD1DULL;
static uint64_t testRng_(void) {
    testRngState_ =
        testRngState_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return testRngState_ >> 16;
}

static bool roundTrip_(const uint64_t *values, size_t count,
                       varintPaletteMeta *meta, size_t *encodedOut) {
    uint8_t *buf = malloc(varintPaletteMaxSize(count));
    uint64_t *dec = malloc((count ? count : 1) * sizeof(*dec));
    size_t written = varintPaletteEncode(buf, values, count, meta);
    bool ok = written > 0 || count == 0;
    if (encodedOut) {
        *encodedOut = written;
    }
    if (ok && count > 0) {
        size_t got = varintPaletteDecode(buf, written, dec, count);
        ok = (got == count) && memcmp(dec, values, count * sizeof(*dec)) == 0;
    }
    free(buf);
    free(dec);
    return ok;
}

/* --------------------------------------------------------------------
 * Test helpers: value generators, raw stream builder, header parser.
 * Crafted-stream tests go through these helpers only, so a future wire
 * format change touches the helpers, not every test.
 * ------------------------------------------------------------------ */

typedef enum testDist_ {
    DIST_CONSTANT,
    DIST_UNIFORM_CARD,
    DIST_ZIPF,
    DIST_UNIQUE,
    DIST_HOT16_OUTLIERS,
} testDist_;

static void genValues_(uint64_t seed, uint64_t *values, size_t count,
                       uint32_t cardinality, testDist_ dist) {
    testRngState_ = seed | 1;
    if (cardinality == 0) {
        cardinality = 1;
    }

    /* Base alphabet; slot 0/1 sometimes get the extremes. */
    uint64_t alpha[64];
    const uint64_t base = testRng_();
    for (uint32_t k = 0; k < cardinality && k < 64; k++) {
        alpha[k] = base + (uint64_t)k * 977;
    }
    if ((seed & 1) && cardinality >= 1) {
        alpha[0] = 0;
    }
    if ((seed & 2) && cardinality >= 2) {
        alpha[1] = UINT64_MAX;
    }

    for (size_t i = 0; i < count; i++) {
        switch (dist) {
        case DIST_CONSTANT:
            values[i] = alpha[0];
            break;
        case DIST_UNIFORM_CARD:
            values[i] = alpha[testRng_() % cardinality];
            break;
        case DIST_ZIPF: {
            uint64_t r = testRng_();
            uint32_t sym = 0;
            while (sym + 1 < cardinality && (r & 1)) {
                sym++;
                r >>= 1;
            }
            values[i] = alpha[sym];
            break;
        }
        case DIST_UNIQUE:
            values[i] = (testRng_() << 20) ^ i;
            break;
        case DIST_HOT16_OUTLIERS:
            values[i] = (testRng_() % 64 == 0) ? (testRng_() | (1ULL << 60))
                                               : alpha[i % 16];
            break;
        }
    }
}

/* Raw stream builder for crafting (possibly invalid) encodings. */
typedef struct streamBuilder_ {
    uint8_t buf[1024];
    size_t len;
} streamBuilder_;

static void sbTagged_(streamBuilder_ *sb, uint64_t v) {
    sb->len += varintTaggedPut64(sb->buf + sb->len, v);
}

static void sbByte_(streamBuilder_ *sb, uint8_t b) {
    sb->buf[sb->len++] = b;
}

static void sbFill_(streamBuilder_ *sb, uint8_t b, size_t n) {
    memset(sb->buf + sb->len, b, n);
    sb->len += n;
}

static void sbU16_(streamBuilder_ *sb, uint16_t v) {
    sbByte_(sb, (uint8_t)(v & 0xFF));
    sbByte_(sb, (uint8_t)(v >> 8));
}

/* Assemble a header: count, m, palette 0..m-1 (1-byte tagged each),
 * nibble-packed lens, one mask byte, then verbatim/coded frames. */
static void sbHeader_(streamBuilder_ *sb, uint64_t count, uint8_t m,
                      const uint8_t *lens, uint8_t maskByte) {
    sb->len = 0;
    sbTagged_(sb, count);
    sbByte_(sb, m);
    for (uint8_t i = 0; i < m; i++) {
        sbTagged_(sb, i);
    }
    for (uint8_t i = 0; i < m; i += 2) {
        const uint8_t hi = (uint8_t)((i + 1 < m) ? lens[i + 1] : 0);
        sbByte_(sb, (uint8_t)(lens[i] | (hi << 4)));
    }
    sbByte_(sb, maskByte);
}

/* Parse palette size + code lengths back out of a valid encoding. */
static bool parseLens_(const uint8_t *src, size_t srcBytes, uint8_t *mOut,
                       uint8_t *lens) {
    const uint8_t *p = src;
    const uint8_t *end = src + srcBytes;
    uint64_t count;
    size_t w = paletteBoundedTagged_(p, end, &count);
    if (w == 0) {
        return false;
    }
    p += w;
    if (p >= end) {
        return false;
    }
    const uint8_t m = *p++;
    if (m == 0 || m > VARINT_PALETTE_MAX_SYMBOLS) {
        return false;
    }
    for (uint8_t i = 0; i < m; i++) {
        uint64_t v;
        w = paletteBoundedTagged_(p, end, &v);
        if (w == 0) {
            return false;
        }
        p += w;
    }
    if ((size_t)(end - p) < ((size_t)m + 1) / 2) {
        return false;
    }
    for (uint8_t i = 0; i < m; i += 2) {
        const uint8_t packed = *p++;
        lens[i] = packed & 0x0F;
        if (i + 1 < m) {
            lens[i + 1] = packed >> 4;
        }
    }
    *mOut = m;
    return true;
}

int varintPaletteTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("Skewed distribution compresses toward entropy") {
        enum { N = 8192 };
        uint64_t *values = malloc(N * sizeof(*values));
        /* Geometric-ish skew over 8 symbols: symbol k with prob ~2^-k */
        for (size_t i = 0; i < N; i++) {
            uint64_t r = testRng_() & 0xFF;
            uint64_t sym = 0;
            for (uint64_t bit = 128; bit > 1 && (r & bit); bit >>= 1) {
                sym++;
            }
            values[i] = 1000 + sym;
        }

        varintPaletteMeta meta;
        size_t encoded;
        if (!roundTrip_(values, N, &meta, &encoded)) {
            ERRR("Round-trip failed for skewed distribution");
        }
        /* ~2 bits/value expected; require better than 1 byte/value. */
        if (encoded > N) {
            ERR("Skewed data compressed poorly: %zu bytes for %d values",
                encoded, N);
        }
        if (meta.verbatimBlocks != 0) {
            ERR("No outliers present, but %zu verbatim blocks",
                meta.verbatimBlocks);
        }
        free(values);
    }

    TEST("Outlier blocks routed verbatim, rest stays coded") {
        enum { N = 4096 };
        uint64_t *values = malloc(N * sizeof(*values));
        /* 16 hot values fill the palette completely... */
        for (size_t i = 0; i < N; i++) {
            values[i] = i % 16;
        }
        /* ...so these once-seen outliers cannot join it and must route
         * their blocks to verbatim storage. */
        values[100] = 0xDEADBEEFCAFEULL;
        values[1000] = 0xFEEDFACE12345ULL;
        values[4000] = 0xABCDEF987654ULL;

        varintPaletteMeta meta;
        size_t encoded;
        if (!roundTrip_(values, N, &meta, &encoded)) {
            ERRR("Round-trip failed with outlier blocks");
        }
        if (meta.verbatimBlocks != 3) {
            ERR("Expected exactly 3 verbatim blocks, got %zu",
                meta.verbatimBlocks);
        }
        if (meta.codedBlocks != N / VARINT_PALETTE_BLOCK_VALUES - 3) {
            ERR("Coded block count wrong: %zu", meta.codedBlocks);
        }
        free(values);
    }

    TEST("All-unique data survives as fully verbatim") {
        enum { N = 500 };
        uint64_t values[N];
        for (size_t i = 0; i < N; i++) {
            values[i] = testRng_();
        }
        varintPaletteMeta meta;
        if (!roundTrip_(values, N, &meta, NULL)) {
            ERRR("Round-trip failed for all-unique data");
        }
        if (meta.codedBlocks != 0) {
            ERR("Unique data should be fully verbatim, got %zu coded",
                meta.codedBlocks);
        }
    }

    TEST("Single distinct value approaches 1 bit per value") {
        enum { N = 8000 };
        uint64_t *values = malloc(N * sizeof(*values));
        for (size_t i = 0; i < N; i++) {
            values[i] = 42;
        }
        varintPaletteMeta meta;
        size_t encoded;
        if (!roundTrip_(values, N, &meta, &encoded)) {
            ERRR("Round-trip failed for constant data");
        }
        if (meta.paletteSize != 1 || meta.maxCodeBits != 1) {
            ERR("Constant data: paletteSize=%u maxCodeBits=%u",
                meta.paletteSize, meta.maxCodeBits);
        }
        /* 8000 values * 1 bit = 1000 bytes, + 2 bytes per block for the
         * independent decode offsets (125 blocks), + small header. */
        if (encoded > 1400) {
            ERR("Constant data too large: %zu bytes", encoded);
        }
        free(values);
    }

    TEST("Tail block shorter than 64 values round-trips") {
        for (size_t count = 1; count <= 200; count += 13) {
            uint64_t values[200];
            for (size_t i = 0; i < count; i++) {
                values[i] = (i % 5) + ((i == count - 1) ? 1000000 : 0);
            }
            if (!roundTrip_(values, count, NULL, NULL)) {
                ERR("Round-trip failed at count=%zu", count);
                break;
            }
        }
    }

    TEST("17 distinct values: 16 coded, rarest forces verbatim") {
        enum { N = 1700 };
        uint64_t *values = malloc(N * sizeof(*values));
        for (size_t i = 0; i < N; i++) {
            values[i] = i % 16; /* 16 evenly hot values */
        }
        values[N - 1] = 999999; /* 17th value, appears once */

        varintPaletteMeta meta;
        if (!roundTrip_(values, N, &meta, NULL)) {
            ERRR("Round-trip failed for 17-value stream");
        }
        if (meta.paletteSize != 16) {
            ERR("Expected full 16-entry palette, got %u", meta.paletteSize);
        }
        if (meta.verbatimBlocks != 1) {
            ERR("Expected 1 verbatim block for lone outlier, got %zu",
                meta.verbatimBlocks);
        }
        free(values);
    }

    TEST("Analyze matches Encode exactly") {
        enum { N = 3000 };
        uint64_t *values = malloc(N * sizeof(*values));
        for (size_t i = 0; i < N; i++) {
            values[i] = testRng_() % 10;
        }
        values[512] = 0x123456789ULL;

        varintPaletteMeta analyzed;
        varintPaletteAnalyze(values, N, &analyzed);

        uint8_t *buf = malloc(varintPaletteMaxSize(N));
        varintPaletteMeta encoded;
        size_t written = varintPaletteEncode(buf, values, N, &encoded);

        if (analyzed.encodedSize != written) {
            ERR("Analyze predicted %zu bytes, encode wrote %zu",
                analyzed.encodedSize, written);
        }
        if (analyzed.verbatimBlocks != encoded.verbatimBlocks) {
            ERR("Analyze/encode verbatim mismatch: %zu vs %zu",
                analyzed.verbatimBlocks, encoded.verbatimBlocks);
        }
        free(buf);
        free(values);
    }

    TEST("Empty and header queries") {
        uint8_t buf[64];
        size_t written = varintPaletteEncode(buf, NULL, 0, NULL);
        if (written == 0) {
            ERRR("Empty encode should still emit a count header");
        }
        if (varintPaletteGetCount(buf, written) != 0) {
            ERRR("Empty stream count should be 0");
        }

        uint64_t one = 77;
        written = varintPaletteEncode(buf, &one, 1, NULL);
        if (varintPaletteGetCount(buf, written) != 1) {
            ERRR("Count header mismatch for single value");
        }
        uint64_t dec;
        if (varintPaletteDecode(buf, written, &dec, 1) != 1 || dec != 77) {
            ERRR("Single value round-trip failed");
        }
        if (varintPaletteGetCount(buf, 0) != 0) {
            ERRR("GetCount must reject an empty source window");
        }
    }

    TEST("Decode rejects undersized output buffer") {
        uint64_t values[100];
        for (size_t i = 0; i < 100; i++) {
            values[i] = i % 3;
        }
        uint8_t *buf = malloc(varintPaletteMaxSize(100));
        size_t written = varintPaletteEncode(buf, values, 100, NULL);

        uint64_t small[10];
        if (varintPaletteDecode(buf, written, small, 10) != 0) {
            ERRR("Decode should decline when maxCount < count");
        }
        free(buf);
    }

    TEST("Palette beats fixed-width dict sizing on skewed data") {
        /* 90% one value, 10% spread across 15 others: entropy ~0.8 bits,
         * while any fixed-width index needs 4 bits. */
        enum { N = 6400 };
        uint64_t *values = malloc(N * sizeof(*values));
        for (size_t i = 0; i < N; i++) {
            values[i] = (testRng_() % 10 == 0) ? (testRng_() % 15) + 1 : 0;
        }
        varintPaletteMeta meta;
        size_t encoded;
        if (!roundTrip_(values, N, &meta, &encoded)) {
            ERRR("Round-trip failed on 90/10 skew");
        }
        /* 4-bit fixed indices would be N/2 = 3200 bytes; entropy coding
         * should land well under that. */
        if (encoded >= N / 2) {
            ERR("Skewed stream not beating 4-bit fixed width: %zu bytes",
                encoded);
        }
        free(values);
    }

    TEST("Property sweep: 200 random configs round-trip with invariants") {
        static const size_t counts[] = {1,   2,   63,  64,   65,  127,
                                        128, 129, 500, 1000, 4096};
        static const uint32_t cards[] = {1, 2, 3, 15, 16, 17, 40};
        size_t failures = 0;

        for (uint64_t cfg = 0; cfg < 200 && failures == 0; cfg++) {
            testRngState_ = cfg * 0x9E3779B97F4A7C15ULL + 1;
            const size_t count =
                (cfg % 3 == 0)
                    ? (testRng_() % 5000) + 1
                    : counts[testRng_() % (sizeof(counts) / sizeof(counts[0]))];
            const uint32_t card =
                cards[testRng_() % (sizeof(cards) / sizeof(cards[0]))];
            const testDist_ dist = (testDist_)(testRng_() % 5);

            uint64_t *values = malloc(count * sizeof(*values));
            genValues_(cfg * 7919 + 13, values, count, card, dist);

            varintPaletteMeta analyzed;
            varintPaletteAnalyze(values, count, &analyzed);

            uint8_t *buf = malloc(varintPaletteMaxSize(count));
            varintPaletteMeta meta;
            const size_t written =
                varintPaletteEncode(buf, values, count, &meta);

            const size_t numBlocks = (count + VARINT_PALETTE_BLOCK_VALUES - 1) /
                                     VARINT_PALETTE_BLOCK_VALUES;
            if (written == 0 || written > varintPaletteMaxSize(count)) {
                ERR("cfg %llu: written %zu vs max %zu", (unsigned long long)cfg,
                    written, varintPaletteMaxSize(count));
                failures++;
            }
            if (analyzed.encodedSize != written) {
                ERR("cfg %llu: analyze %zu != encode %zu",
                    (unsigned long long)cfg, analyzed.encodedSize, written);
                failures++;
            }
            if (meta.codedBlocks + meta.verbatimBlocks != numBlocks) {
                ERR("cfg %llu: block accounting %zu+%zu != %zu",
                    (unsigned long long)cfg, meta.codedBlocks,
                    meta.verbatimBlocks, numBlocks);
                failures++;
            }

            uint64_t *dec = malloc(count * sizeof(*dec));
            const size_t got = varintPaletteDecode(buf, written, dec, count);
            if (got != count ||
                memcmp(dec, values, count * sizeof(*dec)) != 0) {
                ERR("cfg %llu: round-trip failed (count=%zu card=%u dist=%d)",
                    (unsigned long long)cfg, count, card, (int)dist);
                failures++;
            }

            free(dec);
            free(buf);
            free(values);
        }
    }

    TEST("Exhaustive truncation: every strict prefix is rejected") {
        /* Mixed stream: coded + verbatim blocks + short tail. */
        enum { N = 300 };
        uint64_t values[N];
        genValues_(42, values, N, 17, DIST_HOT16_OUTLIERS);

        uint8_t *buf = malloc(varintPaletteMaxSize(N));
        const size_t written = varintPaletteEncode(buf, values, N, NULL);
        uint64_t dec[N];

        for (size_t cut = 0; cut < written; cut++) {
            if (varintPaletteDecode(buf, cut, dec, N) != 0) {
                ERR("Truncated prefix of %zu/%zu bytes accepted", cut, written);
                break;
            }
        }
        /* And the untruncated stream still decodes. */
        if (varintPaletteDecode(buf, written, dec, N) != N) {
            ERRR("Full stream no longer decodes after truncation sweep");
        }
        free(buf);
    }

    TEST("Mutation mini-fuzz: 5000 corrupted streams never misbehave") {
        enum { N = 300 };
        uint64_t values[N];
        genValues_(1234, values, N, 17, DIST_HOT16_OUTLIERS);

        uint8_t *pristine = malloc(varintPaletteMaxSize(N));
        const size_t written = varintPaletteEncode(pristine, values, N, NULL);

        uint8_t *mutated = malloc(written);
        uint64_t dec[N];
        testRngState_ = 0xFEEDBEEF;

        for (int iter = 0; iter < 5000; iter++) {
            memcpy(mutated, pristine, written);
            const int flips = 1 + (int)(testRng_() % 4);
            for (int f = 0; f < flips; f++) {
                /* Bias half the flips into the header-heavy prefix. */
                const size_t pos =
                    (testRng_() & 1)
                        ? testRng_() % (written < 48 ? written : 48)
                        : testRng_() % written;
                mutated[pos] ^= (uint8_t)(testRng_() | 1);
            }
            const size_t got = varintPaletteDecode(mutated, written, dec, N);
            if (got != 0) {
                /* A successful decode must honor the (possibly mutated)
                 * header's own count claim. */
                const size_t claimed = varintPaletteGetCount(mutated, written);
                if (got != claimed) {
                    ERR("iter %d: decode returned %zu but header claims %zu",
                        iter, got, claimed);
                    break;
                }
            }
        }
        free(mutated);
        free(pristine);
    }

    TEST("Canonical Huffman invariants from encoded headers") {
        uint8_t m;
        uint8_t lens[VARINT_PALETTE_MAX_SYMBOLS];
        uint8_t *buf = malloc(varintPaletteMaxSize(4096));
        uint64_t *values = malloc(4096 * sizeof(*values));

        /* Two equal-frequency symbols → both get 1-bit codes. */
        for (size_t i = 0; i < 4096; i++) {
            values[i] = i & 1;
        }
        size_t written = varintPaletteEncode(buf, values, 4096, NULL);
        if (!parseLens_(buf, written, &m, lens) || m != 2 || lens[0] != 1 ||
            lens[1] != 1) {
            ERRR("Equal pair should code 1+1 bits");
        }

        /* Strong skew → hot symbol gets the 1-bit code. */
        for (size_t i = 0; i < 4096; i++) {
            values[i] = (i % 16 == 0) ? (i / 16) % 4 + 1 : 0;
        }
        written = varintPaletteEncode(buf, values, 4096, NULL);
        if (!parseLens_(buf, written, &m, lens) || lens[0] != 1) {
            ERRR("Hot symbol of skewed stream should get a 1-bit code");
        }
        /* Kraft completeness for m >= 2: sum of 2^-len == 1. */
        if (m >= 2) {
            uint32_t maxBits = 0;
            for (uint8_t i = 0; i < m; i++) {
                if (lens[i] > maxBits) {
                    maxBits = lens[i];
                }
            }
            uint32_t kraft = 0;
            for (uint8_t i = 0; i < m; i++) {
                kraft += 1u << (maxBits - lens[i]);
            }
            if (kraft != (1u << maxBits)) {
                ERR("Huffman lengths not Kraft-complete: %u/%u", kraft,
                    1u << maxBits);
            }
        }

        /* m == 1: forced single 1-bit code (Kraft sum legitimately ½). */
        for (size_t i = 0; i < 100; i++) {
            values[i] = 5;
        }
        written = varintPaletteEncode(buf, values, 100, NULL);
        if (!parseLens_(buf, written, &m, lens) || m != 1 || lens[0] != 1) {
            ERRR("Single-symbol palette should carry one forced 1-bit code");
        }
        free(values);
        free(buf);
    }

    TEST("Crafted adversarial streams are all rejected") {
        streamBuilder_ sb;
        uint64_t dec[64];
        const uint8_t oneLen[16] = {1, 1, 1, 1, 1, 1, 1, 1,
                                    1, 1, 1, 1, 1, 1, 1, 1};

        /* Non-Kraft lengths: 16 symbols all claiming 1 bit. Regression
         * test for the flat-table OOB write. Rejected at the Kraft
         * check, before any section parsing. */
        sbHeader_(&sb, 64, 16, oneLen, 0x00);
        sbU16_(&sb, 64);   /* coded block bits */
        sbTagged_(&sb, 0); /* verbatimBytes */
        sbTagged_(&sb, 8); /* codedBytes */
        sbFill_(&sb, 0, 8);
        if (varintPaletteDecode(sb.buf, sb.len, dec, 64) != 0) {
            ERRR("Non-Kraft lengths accepted (OOB-write regression)");
        }

        /* Lying verbatimBytes: claims more than the buffer holds. */
        const uint8_t l1[1] = {1};
        sbHeader_(&sb, 64, 1, l1, 0x01);
        sbTagged_(&sb, 100000);
        if (varintPaletteDecode(sb.buf, sb.len, dec, 64) != 0) {
            ERRR("Oversized verbatimBytes accepted");
        }

        /* Lying codedBytes. */
        sbHeader_(&sb, 64, 1, l1, 0x00);
        sbU16_(&sb, 64);   /* coded block bits: 64 x 1-bit codes */
        sbTagged_(&sb, 0); /* verbatimBytes */
        sbTagged_(&sb, 100000);
        if (varintPaletteDecode(sb.buf, sb.len, dec, 64) != 0) {
            ERRR("Oversized codedBytes accepted");
        }

        /* Coded block claiming more bits than 64 max-length codes. */
        sbHeader_(&sb, 64, 1, l1, 0x00);
        sbU16_(&sb, 2000); /* > 64*15 = 960 */
        sbTagged_(&sb, 0);
        sbTagged_(&sb, 250);
        sbFill_(&sb, 0, 250);
        if (varintPaletteDecode(sb.buf, sb.len, dec, 64) != 0) {
            ERRR("Oversized per-block bit length accepted");
        }

        /* Block-bit total exceeding the coded section. */
        sbHeader_(&sb, 64, 1, l1, 0x00);
        sbU16_(&sb, 900); /* needs 113 bytes... */
        sbTagged_(&sb, 0);
        sbTagged_(&sb, 8); /* ...but only 8 present */
        sbFill_(&sb, 0, 8);
        if (varintPaletteDecode(sb.buf, sb.len, dec, 64) != 0) {
            ERRR("Block bits exceeding coded section accepted");
        }

        /* Mask claims a verbatim block but the section is empty. */
        sbHeader_(&sb, 64, 1, l1, 0x01);
        sbTagged_(&sb, 0); /* verbatimBytes = 0 */
        sbTagged_(&sb, 0); /* codedBytes = 0 */
        if (varintPaletteDecode(sb.buf, sb.len, dec, 64) != 0) {
            ERRR("Verbatim block with empty section accepted");
        }

        /* Verbatim width byte above 8 (0 is valid: tagged mode). */
        sbHeader_(&sb, 64, 1, l1, 0x01);
        sbTagged_(&sb, 1 + 64); /* width byte + 64 one-byte values */
        sbByte_(&sb, 9);
        sbFill_(&sb, 7, 64);
        sbTagged_(&sb, 0); /* codedBytes */
        if (varintPaletteDecode(sb.buf, sb.len, dec, 64) != 0) {
            ERRR("Verbatim width 9 accepted");
        }

        /* Palette size 0 and 17. */
        sb.len = 0;
        sbTagged_(&sb, 64);
        sbByte_(&sb, 0);
        if (varintPaletteDecode(sb.buf, sb.len, dec, 64) != 0) {
            ERRR("m=0 accepted");
        }
        sb.buf[sb.len - 1] = 17;
        if (varintPaletteDecode(sb.buf, sb.len, dec, 64) != 0) {
            ERRR("m=17 accepted");
        }

        /* Zero code length among m=2. */
        const uint8_t lz[2] = {1, 0};
        sbHeader_(&sb, 64, 2, lz, 0x00);
        sbTagged_(&sb, 0);
        sbTagged_(&sb, 8);
        sbFill_(&sb, 0, 8);
        if (varintPaletteDecode(sb.buf, sb.len, dec, 64) != 0) {
            ERRR("Zero code length accepted");
        }

        /* NULL source. */
        if (varintPaletteDecode(NULL, 100, dec, 64) != 0) {
            ERRR("NULL source accepted");
        }
    }

    TEST("Outlier placement at block boundaries") {
        enum { N = 256 };
        uint64_t values[N];
        const size_t spots[] = {0, 63, 64, 65, 127, 128, N - 1};

        for (size_t s = 0; s < sizeof(spots) / sizeof(spots[0]); s++) {
            for (size_t i = 0; i < N; i++) {
                values[i] = i % 16;
            }
            values[spots[s]] = 0xF00DF00DF00DULL;

            varintPaletteMeta meta;
            if (!roundTrip_(values, N, &meta, NULL)) {
                ERR("Round-trip failed with outlier at %zu", spots[s]);
                break;
            }
            if (meta.verbatimBlocks != 1) {
                ERR("Outlier at %zu: expected 1 verbatim block, got %zu",
                    spots[s], meta.verbatimBlocks);
                break;
            }
        }

        /* Alternating coded/verbatim blocks. */
        for (size_t i = 0; i < N; i++) {
            values[i] = i % 16;
            if ((i / VARINT_PALETTE_BLOCK_VALUES) % 2 == 0 &&
                i % VARINT_PALETTE_BLOCK_VALUES == 7) {
                values[i] = 0x8000000000000000ULL + i;
            }
        }
        varintPaletteMeta meta;
        if (!roundTrip_(values, N, &meta, NULL)) {
            ERRR("Alternating coded/verbatim round-trip failed");
        }
        if (meta.verbatimBlocks != 2 || meta.codedBlocks != 2) {
            ERR("Alternation: verbatim=%zu coded=%zu (want 2/2)",
                meta.verbatimBlocks, meta.codedBlocks);
        }
    }

    TEST("High-cardinality fast path: all-verbatim degenerate plan") {
        enum { N = 8192 }; /* >= PALETTE_SAMPLE_MIN_COUNT */
        uint64_t *values = malloc(N * sizeof(*values));
        testRngState_ = 0xABCDEF;
        for (size_t i = 0; i < N; i++) {
            values[i] = (testRng_() << 20) ^ i; /* effectively all unique */
        }

        varintPaletteMeta meta;
        if (!roundTrip_(values, N, &meta, NULL)) {
            ERRR("Fast-path round-trip failed");
        }
        if (meta.paletteSize != 1 || meta.codedBlocks != 0) {
            ERR("Fast path not taken: paletteSize=%u codedBlocks=%zu",
                meta.paletteSize, meta.codedBlocks);
        }

        /* Guard: high-coverage data at the same count must NOT trigger
         * the fast path — hot repeats dominate any sample. */
        for (size_t i = 0; i < N; i++) {
            values[i] = (testRng_() % 64 == 0) ? testRng_() : i % 16;
        }
        if (!roundTrip_(values, N, &meta, NULL)) {
            ERRR("Guard round-trip failed");
        }
        if (meta.codedBlocks == 0) {
            ERRR("Fast path wrongly triggered on high-coverage data");
        }
        free(values);
    }

    TEST("Palette-of-deltas: skewed gaps compress, all edges round-trip") {
        enum { N = 4000 };
        uint64_t *values = malloc(N * sizeof(*values));
        uint64_t *dec = malloc(N * sizeof(*dec));
        uint8_t *buf = malloc(varintPaletteDeltaMaxSize(N));

        /* Monotonic with a skewed 4-gap alphabet. */
        static const uint64_t gaps[] = {1, 1, 1, 1, 1, 1, 2, 2, 5, 10};
        uint64_t v = 1000000;
        testRngState_ = 0x600D;
        for (size_t i = 0; i < N; i++) {
            v += gaps[testRng_() % 10];
            values[i] = v;
        }
        varintPaletteMeta meta;
        size_t written = varintPaletteDeltaEncode(buf, values, N, &meta);
        if (written == 0 || written > varintPaletteDeltaMaxSize(N)) {
            ERR("Delta encode size out of range: %zu", written);
        }
        /* ~1.6 bits/gap expected; plain delta varints would be N bytes. */
        if (written > N / 2) {
            ERR("Skewed gaps compressed poorly: %zu bytes for %d values",
                written, N);
        }
        if (varintPaletteDeltaDecode(buf, written, dec, N) != N ||
            memcmp(dec, values, N * sizeof(*dec)) != 0) {
            ERRR("Palette-of-deltas round-trip failed");
        }

        /* Wrap-around: sequence crossing 2^64 must round-trip. */
        uint64_t wrapv[130];
        wrapv[0] = UINT64_MAX - 40;
        for (size_t i = 1; i < 130; i++) {
            wrapv[i] = wrapv[i - 1] + ((i % 4) + 1); /* wraps past 2^64 */
        }
        written = varintPaletteDeltaEncode(buf, wrapv, 130, NULL);
        if (varintPaletteDeltaDecode(buf, written, dec, 130) != 130 ||
            memcmp(dec, wrapv, 130 * sizeof(*dec)) != 0) {
            ERRR("Wrap-around sequence round-trip failed");
        }

        /* Non-monotonic input is legal (wrapped deltas). */
        uint64_t zig[200];
        for (size_t i = 0; i < 200; i++) {
            zig[i] = (i & 1) ? 100 : 200;
        }
        written = varintPaletteDeltaEncode(buf, zig, 200, NULL);
        if (varintPaletteDeltaDecode(buf, written, dec, 200) != 200 ||
            memcmp(dec, zig, 200 * sizeof(*dec)) != 0) {
            ERRR("Non-monotonic round-trip failed");
        }

        /* count 0/1/2 edges + undersized output + truncation. */
        written = varintPaletteDeltaEncode(buf, values, 0, NULL);
        if (written == 0) {
            ERRR("Empty delta encode should emit a header");
        }
        written = varintPaletteDeltaEncode(buf, values, 1, NULL);
        if (varintPaletteDeltaDecode(buf, written, dec, 1) != 1 ||
            dec[0] != values[0]) {
            ERRR("Single-value delta round-trip failed");
        }
        written = varintPaletteDeltaEncode(buf, values, 2, NULL);
        if (varintPaletteDeltaDecode(buf, written, dec, 2) != 2) {
            ERRR("Two-value delta round-trip failed");
        }
        if (varintPaletteDeltaDecode(buf, written, dec, 1) != 0) {
            ERRR("Undersized maxCount accepted");
        }
        for (size_t cut = 0; cut < written; cut++) {
            if (varintPaletteDeltaDecode(buf, cut, dec, 2) != 0) {
                ERR("Truncated delta prefix %zu accepted", cut);
                break;
            }
        }
        free(values);
        free(dec);
        free(buf);
    }

    TEST("maxCount edges around the true count") {
        enum { N = 130 };
        uint64_t values[N];
        genValues_(77, values, N, 5, DIST_ZIPF);

        uint8_t *buf = malloc(varintPaletteMaxSize(N));
        const size_t written = varintPaletteEncode(buf, values, N, NULL);

        uint64_t dec[N + 10];
        if (varintPaletteDecode(buf, written, dec, N - 1) != 0) {
            ERRR("maxCount == count-1 must be rejected");
        }
        if (varintPaletteDecode(buf, written, dec, N) != N) {
            ERRR("maxCount == count must succeed");
        }
        uint64_t dec2[N + 10];
        if (varintPaletteDecode(buf, written, dec2, N + 10) != N ||
            memcmp(dec, dec2, N * sizeof(*dec)) != 0) {
            ERRR("maxCount == count+10 must decode identically");
        }
        free(buf);
    }

    TEST_FINAL_RESULT;
    return 0;
}

#endif /* VARINT_PALETTE_TEST */
