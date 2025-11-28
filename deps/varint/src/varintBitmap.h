#pragma once

#include "varint.h"
#include "varintExternal.h"

__BEGIN_DECLS

/* ====================================================================
 * Bitmap varints - Hybrid Dense/Sparse Encoding (Roaring-style)
 * ==================================================================== */
/* varint model Bitmap Container:
 *   Type encoded inside: container type byte determines encoding
 *   Size: variable based on container type and cardinality
 *   Layout: [container_type][data]
 *   Container types:
 *     0 = ARRAY  (sparse, varint list)
 *     1 = BITMAP (dense, 8192 bytes for 65536 bits)
 *     2 = RUNS   (run-length encoded)
 *
 *   Thresholds:
 *     Array -> Bitmap: cardinality > 4096
 *     Bitmap -> Array: cardinality < 4096
 *     Runs: based on run count efficiency
 *
 *   Pro: Automatic density adaptation, efficient set operations
 *   Con: More complex than simple formats
 *   Use cases: Inverted indexes, sparse sets, boolean arrays */

/* Container types */
typedef enum varintBitmapContainerType {
    VARINT_BITMAP_ARRAY = 0,  /* Sparse array of values */
    VARINT_BITMAP_BITMAP = 1, /* Dense bitmap (8192 bytes) */
    VARINT_BITMAP_RUNS = 2    /* Run-length encoded */
} varintBitmapContainerType;

/* Container threshold constants */
#define VARINT_BITMAP_MAX_VALUE 65536
#define VARINT_BITMAP_ARRAY_MAX 4096
#define VARINT_BITMAP_BITMAP_SIZE 8192 /* 65536 bits / 8 */
#define VARINT_BITMAP_DEFAULT_ARRAY_CAPACITY 16

/* Bitmap container structure */
typedef struct varintBitmap {
    varintBitmapContainerType type;
    uint32_t cardinality; /* Number of set bits */

    union {
        /* ARRAY container: sorted list of values */
        struct {
            uint16_t *values;
            uint32_t capacity;
        } array;

        /* BITMAP container: 8192 bytes (65536 bits) */
        struct {
            uint8_t *bits;
        } bitmap;

        /* RUNS container: array of [start, length] pairs */
        struct {
            uint16_t *runs; /* Interleaved [start, length] pairs */
            uint32_t numRuns;
            uint32_t capacity;
        } runs;
    } container;
} varintBitmap;

/* Compile-time size guarantees to prevent regressions */
_Static_assert(sizeof(varintBitmap) == 24,
               "varintBitmap size changed! Expected 24 bytes (2×4-byte + "
               "16-byte union, ZERO padding). "
               "This struct achieved 100% efficiency - do not break it!");
_Static_assert(sizeof(varintBitmap) <= 64,
               "varintBitmap exceeds single cache line (64 bytes)! "
               "Keep bitmap container struct cache-friendly.");

/* ====================================================================
 * Core API
 * ==================================================================== */

/* Create and destroy */
varintBitmap *varintBitmapCreate(void);
void varintBitmapFree(varintBitmap *vb);
varintBitmap *varintBitmapClone(const varintBitmap *vb);

/* Add/remove/query */
bool varintBitmapAdd(varintBitmap *vb, uint16_t value);
bool varintBitmapRemove(varintBitmap *vb, uint16_t value);
bool varintBitmapContains(const varintBitmap *vb, uint16_t value);

/* Set operations */
varintBitmap *varintBitmapAnd(const varintBitmap *vb1, const varintBitmap *vb2);
varintBitmap *varintBitmapOr(const varintBitmap *vb1, const varintBitmap *vb2);
varintBitmap *varintBitmapXor(const varintBitmap *vb1, const varintBitmap *vb2);
varintBitmap *varintBitmapAndNot(const varintBitmap *vb1,
                                 const varintBitmap *vb2);

/* Cardinality and size */
uint32_t varintBitmapCardinality(const varintBitmap *vb);
size_t varintBitmapSizeBytes(const varintBitmap *vb);

/* Encoding/Decoding (Serialization) */
size_t varintBitmapEncode(const varintBitmap *vb, uint8_t *buffer);
varintBitmap *varintBitmapDecode(const uint8_t *buffer, size_t len);

/* Iteration */
typedef struct varintBitmapIterator {
    const varintBitmap *vb;
    uint32_t position;     /* Current position in container */
    uint16_t currentValue; /* Current value */
    bool hasValue;
} varintBitmapIterator;

/* Compile-time size guarantees to prevent regressions */
_Static_assert(sizeof(varintBitmapIterator) == 16,
               "varintBitmapIterator size changed! Expected 16 bytes (8-byte "
               "pointer + 4-byte + 2-byte + 1-byte + 1 padding). "
               "93.75% efficient - minimal padding required for alignment.");
_Static_assert(sizeof(varintBitmapIterator) <= 64,
               "varintBitmapIterator exceeds single cache line (64 bytes)! "
               "Keep iterator struct cache-friendly.");

varintBitmapIterator varintBitmapCreateIterator(const varintBitmap *vb);
bool varintBitmapIteratorNext(varintBitmapIterator *it);

/* Bulk operations */
void varintBitmapAddMany(varintBitmap *vb, const uint16_t *values,
                         uint32_t count);
uint32_t varintBitmapToArray(const varintBitmap *vb, uint16_t *output);

/* Statistics
 * Fields ordered by size (8-byte → 4-byte) to eliminate padding */
typedef struct varintBitmapStats {
    size_t sizeBytes;
    varintBitmapContainerType type;
    uint32_t cardinality;
    uint32_t containerCapacity; /* For array/runs */
} varintBitmapStats;

/* Compile-time size guarantees to prevent regressions */
_Static_assert(
    sizeof(varintBitmapStats) == 24,
    "varintBitmapStats size changed! Expected 24 bytes (1×8-byte + 3×4-byte + "
    "4 padding). "
    "83.3% efficient - padding after last 4-byte field is acceptable.");
_Static_assert(sizeof(varintBitmapStats) <= 64,
               "varintBitmapStats exceeds single cache line (64 bytes)! "
               "Keep bitmap statistics cache-friendly.");

void varintBitmapGetStats(const varintBitmap *vb, varintBitmapStats *stats);

/* Container optimization */
void varintBitmapOptimize(varintBitmap *vb);
bool varintBitmapIsEmpty(const varintBitmap *vb);
void varintBitmapClear(varintBitmap *vb);

/* Range operations */
void varintBitmapAddRange(varintBitmap *vb, uint16_t min, uint16_t max);
void varintBitmapRemoveRange(varintBitmap *vb, uint16_t min, uint16_t max);

__END_DECLS
