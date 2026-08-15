/* ====================================================================
 * varintPaletteFuzz — self-contained deterministic fuzzer
 * ====================================================================
 * No external fuzzing tooling: a plain executable driving three rotated
 * strategies from one LCG, so every failure reproduces from its seed.
 *
 *   1. Structured round-trip generation (self-checking oracle):
 *      random count/cardinality/skew/outlier parameters chosen to hit
 *      every format section (tail blocks, all-verbatim, full palette,
 *      1-symbol palette, extreme values) → encode → decode → compare.
 *   2. Mutation of valid encodings: header-biased byte flips and
 *      truncations; decode must never crash, and a successful decode
 *      must return exactly the mutated header's own count claim.
 *   3. Pure-random decode: arbitrary bytes are rejected safely.
 *
 * The same value streams are also pushed through the varintCompete
 * frame (encode/decode round-trip) on a share of iterations.
 *
 * Usage: varintPaletteFuzz [iterations] [seed]
 *   defaults: 20000 iterations, seed 0x5EED (sized for ctest).
 * On failure prints the exact reproduce command and exits nonzero.
 * Run under ASan/UBSan via scripts/test/run_palette_fuzz.sh for the
 * long sessions. */

#include "varintAdaptive.h"
#include "varintBP128.h"
#include "varintBitmap.h"
#include "varintCompete.h"
#include "varintDict.h"
#include "varintElias.h"
#include "varintFOR.h"
#include "varintGroup.h"
#include "varintPFOR.h"
#include "varintPalette.h"
#include "varintRLE.h"
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Above the palette sampling-fast-path threshold (4096) so the
 * high-cardinality shortcut gets regular fuzz coverage. */
#define FUZZ_MAX_VALUES 8192

static uint64_t rngState_;
static uint64_t rng_(void) {
    rngState_ = rngState_ * 6364136223846793005ULL + 1442695040888963407ULL;
    return rngState_ >> 16;
}

static uint64_t gIterations;
static uint64_t gSeed;
static uint64_t gIter;
static size_t gCount;
static const char *gPhase = "";

/* Library-side assert() calls abort() and would otherwise die without
 * fuzz context; this handler makes ANY abort as reproducible as a
 * FUZZ_CHECK failure. (Triage lesson: a failure that appears only in
 * plain -O3 builds — passing at -O0 and under ASan/UBSan — usually
 * means an uninitialized read, which sanitizers here cannot see.) */
static void abortContext_(int sig) {
    (void)sig;
    fprintf(stderr,
            "FUZZ ABORT (library assert?)\n"
            "  iteration : %" PRIu64 " (strategy slot %" PRIu64 ", phase %s)\n"
            "  count     : %zu\n"
            "  reproduce : varintPaletteFuzz %" PRIu64 " %" PRIu64 "\n",
            gIter, gIter % 9, gPhase, gCount, gIterations, gSeed);
    signal(SIGABRT, SIG_DFL);
    /* fall through to the default abort */
}

static void fail_(const char *what, const char *detail) {
    fprintf(stderr,
            "FUZZ FAILURE: %s (%s)\n"
            "  iteration : %" PRIu64 "\n"
            "  reproduce : varintPaletteFuzz %" PRIu64 " %" PRIu64 "\n",
            what, detail, gIter, gIterations, gSeed);
    exit(1);
}

#define FUZZ_CHECK(cond, what, detail)                                         \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fail_((what), (detail));                                           \
        }                                                                      \
    } while (0)

/* ====================================================================
 * Structured generation
 * ==================================================================== */

static size_t genCount_(void) {
    /* Bias toward block boundaries and small counts; occasionally big. */
    static const size_t edges[] = {1, 2, 63, 64, 65, 127, 128, 129, 4096};
    const uint64_t pick = rng_() % 10;
    if (pick < 3) {
        return edges[rng_() % (sizeof(edges) / sizeof(edges[0]))];
    }
    if (pick < 9) {
        return 1 + (size_t)(rng_() % 512);
    }
    return 1 + (size_t)(rng_() % FUZZ_MAX_VALUES);
}

static size_t genValues_(uint64_t *values) {
    const size_t count = genCount_();
    const uint32_t style = (uint32_t)(rng_() % 6);
    uint32_t card = 1 + (uint32_t)(rng_() % 48);
    if (rng_() % 4 == 0) {
        /* Bias to the palette-capacity boundary. */
        card = 15 + (uint32_t)(rng_() % 3);
    }

    uint64_t alpha[48];
    const uint64_t base = rng_();
    for (uint32_t k = 0; k < card; k++) {
        alpha[k] = base + (uint64_t)k * 977;
    }
    if (rng_() & 1) {
        alpha[0] = 0;
    }
    if (card >= 2 && (rng_() & 1)) {
        alpha[1] = UINT64_MAX;
    }

    for (size_t i = 0; i < count; i++) {
        switch (style) {
        case 0: /* constant */
            values[i] = alpha[0];
            break;
        case 5: /* monotonic with alphabet gaps (palette-delta shape) */
            values[i] =
                (i == 0) ? base : values[i - 1] + (alpha[rng_() % card] & 0xFF);
            break;
        case 1: /* uniform over alphabet */
            values[i] = alpha[rng_() % card];
            break;
        case 2: { /* geometric skew */
            uint64_t r = rng_();
            uint32_t sym = 0;
            while (sym + 1 < card && (r & 1)) {
                sym++;
                r >>= 1;
            }
            values[i] = alpha[sym];
            break;
        }
        case 3: /* all-unique */
            values[i] = (rng_() << 20) ^ i;
            break;
        default: /* hot alphabet + rare unique outliers */
            values[i] =
                (rng_() % 64 == 0) ? (rng_() | (1ULL << 60)) : alpha[i % card];
            break;
        }
    }
    return count;
}

/* ====================================================================
 * Strategies
 * ==================================================================== */

static void strategyRoundTrip_(uint64_t *values, size_t count, uint8_t *enc,
                               uint64_t *dec) {
    varintPaletteMeta analyzed;
    varintPaletteAnalyze(values, count, &analyzed);

    const size_t written = varintPaletteEncode(enc, values, count, NULL);
    FUZZ_CHECK(written > 0, "encode returned 0", "round-trip");
    FUZZ_CHECK(written <= varintPaletteMaxSize(count),
               "encode exceeded MaxSize", "round-trip");
    FUZZ_CHECK(analyzed.encodedSize == written, "Analyze != Encode size",
               "round-trip");

    const size_t got = varintPaletteDecode(enc, written, dec, count);
    FUZZ_CHECK(got == count, "decode count mismatch", "round-trip");
    FUZZ_CHECK(memcmp(dec, values, count * sizeof(*dec)) == 0,
               "decode value mismatch", "round-trip");
}

static void strategyMutate_(const uint8_t *pristine, size_t written,
                            uint8_t *scratch, uint64_t *dec) {
    memcpy(scratch, pristine, written);

    /* Sometimes also truncate. */
    size_t len = written;
    if (rng_() % 4 == 0) {
        len = rng_() % written;
    }

    const int flips = 1 + (int)(rng_() % 8);
    if (len > 0) {
        for (int f = 0; f < flips; f++) {
            const size_t bound = (rng_() & 1) && len > 48 ? 48 : len;
            scratch[rng_() % bound] ^= (uint8_t)(rng_() | 1);
        }
    }

    const size_t got = varintPaletteDecode(scratch, len, dec, FUZZ_MAX_VALUES);
    if (got != 0) {
        const size_t claimed = varintPaletteGetCount(scratch, len);
        FUZZ_CHECK(got == claimed, "decode ignored mutated count claim",
                   "mutation");
    }
}

static void strategyRandom_(uint8_t *scratch, uint64_t *dec) {
    const size_t len = 1 + (size_t)(rng_() % 600);
    for (size_t i = 0; i < len; i++) {
        scratch[i] = (uint8_t)rng_();
    }
    (void)varintPaletteDecode(scratch, len, dec, FUZZ_MAX_VALUES);
}

static void strategyCompete_(uint64_t *values, size_t count, uint8_t *enc,
                             uint64_t *dec) {
    const size_t written = varintCompeteEncodeUnsigned(
        enc, values, count, VARINT_COMPETE_DEFAULT_MASK, NULL);
    FUZZ_CHECK(written > 0, "compete encode returned 0", "compete");

    const size_t read = varintCompeteDecodeUnsigned(enc, written, count, dec);

    varintCompeteHeader h;
    const char *winner = "?";
    if (varintCompeteReadHeader(enc, written, &h) != 0) {
        winner = varintCodecName(h.codecID);
    }
    FUZZ_CHECK(read == written, "compete decode byte mismatch", winner);
    if (memcmp(dec, values, count * sizeof(*dec)) != 0) {
        size_t at = 0;
        while (at < count && dec[at] == values[at]) {
            at++;
        }
        fprintf(stderr,
                "compete mismatch: winner=%s count=%zu first-diff@%zu "
                "want=%" PRIu64 " got=%" PRIu64 "\n",
                winner, count, at, values[at], dec[at]);
        fail_("compete decode value mismatch", winner);
    }
}

/* Signed compete round-trip. The signed codecs reinterpret and wrap in
 * the value domain, so extreme int64 magnitudes (the historical
 * saturating-arithmetic corruption case) must round-trip exactly. */
static void strategyCompeteSigned_(uint64_t *values, size_t count, uint8_t *enc,
                                   uint64_t *dec) {
    int64_t *sv = (int64_t *)values;
    /* Inject extremes at random positions. */
    static const int64_t extremes[] = {INT64_MIN, INT64_MAX, -1, 0,
                                       INT64_MIN + 1};
    const size_t injections = 1 + (size_t)(rng_() % 4);
    for (size_t k = 0; k < injections; k++) {
        sv[rng_() % count] = extremes[rng_() % 5];
    }

    const size_t written =
        varintCompeteEncode(enc, sv, count, VARINT_COMPETE_DEFAULT_MASK, NULL);
    FUZZ_CHECK(written > 0, "signed compete encode returned 0", "signed");

    const size_t read =
        varintCompeteDecode(enc, written, count, (int64_t *)dec);
    FUZZ_CHECK(read == written, "signed compete byte mismatch", "signed");
    if (memcmp(dec, sv, count * sizeof(*dec)) != 0) {
        varintCompeteHeader h;
        const char *winner = varintCompeteReadHeader(enc, written, &h) != 0
                                 ? varintCodecName(h.codecID)
                                 : "?";
        fail_("signed compete value mismatch", winner);
    }
}

/* Direct per-codec round-trips: compete only decodes the winning codec,
 * so a losing codec's decoder is never otherwise exercised. Each codec
 * that accepts arbitrary u64 input must round-trip it exactly whenever
 * its encoder emits anything. */
static void strategyDirectCodecs_(const uint64_t *values, size_t count,
                                  uint8_t *enc, uint64_t *dec) {
    size_t n;

    n = varintRLEEncodeWithHeader(enc, values, count, NULL);
    if (n > 0) {
        const size_t got = varintRLEDecodeWithHeader(enc, dec, count);
        FUZZ_CHECK(got == count &&
                       memcmp(dec, values, count * sizeof(*dec)) == 0,
                   "direct round-trip failed", "RLE");
    }

    n = varintFOREncode(enc, values, count, NULL);
    if (n > 0) {
        const size_t got = varintFORDecode(enc, dec, count);
        FUZZ_CHECK(got == count &&
                       memcmp(dec, values, count * sizeof(*dec)) == 0,
                   "direct round-trip failed", "FOR");
    }

    varintPFORMeta pm;
    n = varintPFOREncode(enc, values, (uint32_t)count, VARINT_PFOR_THRESHOLD_95,
                         &pm);
    if (n > 0) {
        varintPFORMeta rm;
        varintPFORReadMeta(enc, &rm);
        varintPFORDecode(enc, dec, &rm);
        FUZZ_CHECK(memcmp(dec, values, count * sizeof(*dec)) == 0,
                   "direct round-trip failed", "PFOR");
    }

    n = varintDictEncode(enc, values, count);
    if (n > 0) {
        const size_t got = varintDictDecodeInto(enc, n, dec, count);
        FUZZ_CHECK(got == count &&
                       memcmp(dec, values, count * sizeof(*dec)) == 0,
                   "direct round-trip failed", "DICT");
    }

    n = varintBP128Encode64(enc, values, count, NULL);
    if (n > 0) {
        const size_t got = varintBP128Decode64(enc, dec, count);
        FUZZ_CHECK(got == count &&
                       memcmp(dec, values, count * sizeof(*dec)) == 0,
                   "direct round-trip failed", "BP128");
    }
}

/* Palette-of-deltas: full oracle round-trip plus mutation and random
 * decode against its bounded decoder. */
static void strategyPaletteDelta_(const uint64_t *values, size_t count,
                                  uint8_t *enc, uint8_t *scratch,
                                  uint64_t *dec) {
    const size_t written = varintPaletteDeltaEncode(enc, values, count, NULL);
    FUZZ_CHECK(written > 0, "delta encode returned 0", "palette-delta");
    FUZZ_CHECK(written <= varintPaletteDeltaMaxSize(count),
               "delta encode exceeded MaxSize", "palette-delta");

    const size_t got =
        varintPaletteDeltaDecode(enc, written, dec, FUZZ_MAX_VALUES);
    FUZZ_CHECK(got == count && memcmp(dec, values, count * sizeof(*dec)) == 0,
               "delta round-trip failed", "palette-delta");

    /* Mutate. */
    memcpy(scratch, enc, written);
    size_t len = written;
    if (rng_() % 4 == 0) {
        len = rng_() % written;
    }
    if (len > 0) {
        const int flips = 1 + (int)(rng_() % 8);
        for (int f = 0; f < flips; f++) {
            const size_t bound = (rng_() & 1) && len > 48 ? 48 : len;
            scratch[rng_() % bound] ^= (uint8_t)(rng_() | 1);
        }
    }
    (void)varintPaletteDeltaDecode(scratch, len, dec, FUZZ_MAX_VALUES);

    /* Pure random. */
    const size_t rlen = 1 + (size_t)(rng_() % 600);
    for (size_t i = 0; i < rlen; i++) {
        scratch[i] = (uint8_t)rng_();
    }
    (void)varintPaletteDeltaDecode(scratch, rlen, dec, FUZZ_MAX_VALUES);
}

/* Round-trip oracles for the remaining codec family members that have
 * their own container formats: the adaptive selector, Elias gamma and
 * delta arrays (positive-integer domain), grouped varints (<=64 fields)
 * and the u16 bitmap set. `aux` holds domain-transformed copies. */
static void strategyMoreCodecs_(const uint64_t *values, size_t count,
                                uint8_t *enc, uint8_t *scratch, uint64_t *aux,
                                uint64_t *dec) {
    size_t n;

    /* Adaptive selector (heuristic cousin of compete). */
    gPhase = "adaptive";
    n = varintAdaptiveEncode(enc, values, count, NULL);
    if (n > 0) {
        const size_t got = varintAdaptiveDecode(enc, dec, count, NULL);
        FUZZ_CHECK(got == count &&
                       memcmp(dec, values, count * sizeof(*dec)) == 0,
                   "direct round-trip failed", "ADAPTIVE");
    }

    /* Elias gamma + delta over the >=1 domain. */
    gPhase = "elias";
    for (size_t i = 0; i < count; i++) {
        aux[i] = values[i] | 1;
    }
    n = varintEliasGammaEncodeArray(enc, aux, count, NULL);
    if (n > 0) {
        const size_t got = varintEliasGammaDecodeArray(enc, n * 8, dec, count);
        FUZZ_CHECK(got == count && memcmp(dec, aux, count * sizeof(*dec)) == 0,
                   "direct round-trip failed", "ELIAS_GAMMA");
    }
    n = varintEliasDeltaEncodeArray(enc, aux, count, NULL);
    if (n > 0) {
        const size_t got = varintEliasDeltaDecodeArray(enc, n * 8, dec, count);
        FUZZ_CHECK(got == count && memcmp(dec, aux, count * sizeof(*dec)) == 0,
                   "direct round-trip failed", "ELIAS_DELTA");
    }

    /* Grouped varints: first <=64 values as one group. */
    gPhase = "group";
    const uint8_t fields =
        (uint8_t)(count < VARINT_GROUP_MAX_FIELDS ? count
                                                  : VARINT_GROUP_MAX_FIELDS);
    n = varintGroupEncode(enc, values, fields);
    if (n > 0) {
        uint8_t gotFields = 0;
        const size_t used = varintGroupDecode(enc, dec, &gotFields, fields);
        FUZZ_CHECK(used > 0 && gotFields == fields &&
                       memcmp(dec, values, fields * sizeof(*dec)) == 0,
                   "direct round-trip failed", "GROUP");
    }

    /* Bitmap set over the u16 projection: decode then re-encode must be
     * byte-identical. */
    gPhase = "bitmap";
    varintBitmap *vb = varintBitmapCreate();
    if (vb) {
        const size_t nvals = count < 1024 ? count : 1024;
        for (size_t i = 0; i < nvals; i++) {
            varintBitmapAdd(vb, (uint16_t)values[i]);
        }
        const size_t bytes = varintBitmapEncode(vb, enc);
        if (bytes > 0) {
            varintBitmap *back = varintBitmapDecode(enc, bytes);
            FUZZ_CHECK(back != NULL, "bitmap decode failed", "BITMAP");
            const size_t bytes2 = varintBitmapEncode(back, scratch);
            FUZZ_CHECK(bytes2 == bytes && memcmp(enc, scratch, bytes) == 0,
                       "bitmap re-encode mismatch", "BITMAP");
            varintBitmapFree(back);
        }
        varintBitmapFree(vb);
    }
}

static int cmp64_(const void *a, const void *b) {
    const uint64_t x = *(const uint64_t *)a;
    const uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* BP128Delta accepts only ascending input — feed it a sorted copy. */
static void strategyBP128DeltaSorted_(uint64_t *values, size_t count,
                                      uint8_t *enc, uint64_t *dec) {
    qsort(values, count, sizeof(*values), cmp64_);
    const size_t n = varintBP128DeltaEncode64(enc, values, count, NULL);
    if (n > 0) {
        const size_t got = varintBP128DeltaDecode64(enc, dec, count);
        FUZZ_CHECK(got == count &&
                       memcmp(dec, values, count * sizeof(*dec)) == 0,
                   "direct round-trip failed", "BP128_DELTA");
    }
}

int main(int argc, char *argv[]) {
    gIterations = (argc > 1) ? strtoull(argv[1], NULL, 0) : 20000;
    gSeed = (argc > 2) ? strtoull(argv[2], NULL, 0) : 0x5EED;
    rngState_ = gSeed | 1;

    uint64_t *values = malloc(FUZZ_MAX_VALUES * sizeof(*values));
    uint64_t *dec = malloc(FUZZ_MAX_VALUES * sizeof(*dec));
    uint64_t *aux = malloc(FUZZ_MAX_VALUES * sizeof(*aux));
    uint8_t *enc = malloc(varintCompeteMaxEncodedSize(FUZZ_MAX_VALUES));
    uint8_t *scratch = malloc(varintCompeteMaxEncodedSize(FUZZ_MAX_VALUES));
    if (!values || !dec || !aux || !enc || !scratch) {
        fprintf(stderr, "fuzz: allocation failed\n");
        return 2;
    }

    signal(SIGABRT, abortContext_);

    for (gIter = 0; gIter < gIterations; gIter++) {
        const size_t count = genValues_(values);
        gCount = count;

        /* Every iteration exercises the oracle... */
        strategyRoundTrip_(values, count, enc, dec);
        const size_t written = varintPaletteEncode(enc, values, count, NULL);

        /* ...then rotates through the adversarial strategies. The last
         * two mutate `values`, so they run after everything that needs
         * the original stream. */
        switch (gIter % 9) {
        case 0:
        case 1:
            strategyMutate_(enc, written, scratch, dec);
            break;
        case 2:
            strategyRandom_(scratch, dec);
            break;
        case 3:
            strategyCompete_(values, count, scratch, dec);
            break;
        case 4:
            strategyDirectCodecs_(values, count, scratch, dec);
            break;
        case 5:
            strategyPaletteDelta_(values, count, enc, scratch, dec);
            break;
        case 6:
            strategyMoreCodecs_(values, count, enc, scratch, aux, dec);
            break;
        case 7:
            strategyCompeteSigned_(values, count, scratch, dec);
            break;
        default:
            strategyBP128DeltaSorted_(values, count, scratch, dec);
            break;
        }
    }

    printf("varintPaletteFuzz: %" PRIu64 " iterations clean (seed %" PRIu64
           ")\n",
           gIterations, gSeed);
    free(values);
    free(dec);
    free(aux);
    free(enc);
    free(scratch);
    return 0;
}
