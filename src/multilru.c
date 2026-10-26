#include "datakit.h"

#include "cleaner.h"
#include "fibbuf.h"
#include "jebuf.h"
#include "multilru.h"
#include "str.h"
#include "timeUtil.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#ifdef DATAKIT_TEST
defcleanerName(uint8_t *, zfree, Bytes);
defcleanerName(multilruPtr *, zfree, Ptrs);
defcleanerName(uint64_t *, zfree, U64s);
defcleaner(multilru *, multilruFree);
#endif

/* ====================================================================
 * Finer-Grained Adaptive Entry Structures (5-16 bytes)
 * ====================================================================
 *
 * Entry width is selected based on capacity, with gradual progression
 * to minimize migration costs at tier boundaries:
 *
 *   Width  Address Bits  Max Entries   Migration Cost
 *   -----  ------------  -----------   --------------
 *   5      16 bits       64K           -
 *   6      20 bits       1M            64K × 1B = 64KB
 *   7      24 bits       16M           1M × 1B = 1MB
 *   8      28 bits       256M          16M × 1B = 16MB
 *   9      32 bits       4B            256M × 1B = 256MB
 *   10     36 bits       64B           4B × 1B = 4GB
 *   11     40 bits       1T            64B × 1B = 64GB
 *   12     44 bits       16T           1T × 1B = 1TB
 *   16     60 bits       1E            16T × 4B = 64TB
 *
 * Entry bit layout (all widths):
 *   Bits 0 to 2N-1:     prev (N bits), next (N bits)
 *   Bits 2N to 2N+5:    level (6 bits)
 *   Bit 2N+6:           isPopulated (1 bit)
 *   Bit 2N+7:           isHead (1 bit)
 *
 * Where N = address bits for the width tier.
 */

/* ====================================================================
 * Bit Packing Primitives
 * ====================================================================
 * These functions read/write arbitrary bit fields from byte arrays.
 * Used for widths that don't align to native integer boundaries.
 */

/* Read up to 64 bits from a byte array starting at bitOffset */
DK_INLINE_ALWAYS uint64_t readBitsLE(const uint8_t *data, uint32_t bitOffset,
                                     uint8_t numBits) {
    uint64_t result = 0;
    uint32_t byteOffset = bitOffset / 8;
    uint32_t bitShift = bitOffset % 8;

    /* Read enough bytes to cover the bit range */
    uint32_t bitsNeeded = bitShift + numBits;
    uint32_t bytesNeeded = (bitsNeeded + 7) / 8;

    /* Assemble bytes into result (little-endian) */
    uint64_t raw = 0;
    for (uint32_t i = 0; i < bytesNeeded && i < 8; i++) {
        raw |= (uint64_t)data[byteOffset + i] << (i * 8);
    }

    /* Extract the bit field */
    result = (raw >> bitShift) & ((1ULL << numBits) - 1);
    return result;
}

/* Write up to 64 bits to a byte array starting at bitOffset */
DK_INLINE_ALWAYS void writeBitsLE(uint8_t *data, uint32_t bitOffset,
                                  uint8_t numBits, uint64_t value) {
    uint32_t byteOffset = bitOffset / 8;
    uint32_t bitShift = bitOffset % 8;

    /* Mask the value to the correct number of bits */
    uint64_t mask = (numBits == 64) ? ~0ULL : ((1ULL << numBits) - 1);
    value &= mask;

    /* Read-modify-write the affected bytes */
    uint32_t bitsNeeded = bitShift + numBits;
    uint32_t bytesNeeded = (bitsNeeded + 7) / 8;

    /* Read current bytes */
    uint64_t raw = 0;
    for (uint32_t i = 0; i < bytesNeeded && i < 8; i++) {
        raw |= (uint64_t)data[byteOffset + i] << (i * 8);
    }

    /* Clear the target bits and set new value */
    uint64_t clearMask = mask << bitShift;
    raw = (raw & ~clearMask) | (value << bitShift);

    /* Write back the bytes */
    for (uint32_t i = 0; i < bytesNeeded && i < 8; i++) {
        data[byteOffset + i] = (raw >> (i * 8)) & 0xFF;
    }
}

/* ====================================================================
 * Width Tier Definitions
 * ====================================================================
 * Each tier specifies:
 *   - Entry width in bytes
 *   - Address bits for prev/next pointers
 *   - Maximum addressable entries
 */

typedef struct {
    uint8_t width;       /* Entry size in bytes */
    uint8_t addressBits; /* Bits for prev/next indices */
    uint64_t maxEntries; /* Maximum entries at this width */
} lruWidthTier;

/* Width tiers in ascending order */
static const lruWidthTier WIDTH_TIERS[] = {
    {5, 16, (1ULL << 16) - 1},  /* 64K */
    {6, 20, (1ULL << 20) - 1},  /* 1M */
    {7, 24, (1ULL << 24) - 1},  /* 16M */
    {8, 28, (1ULL << 28) - 1},  /* 256M */
    {9, 32, (1ULL << 32) - 1},  /* 4B */
    {10, 36, (1ULL << 36) - 1}, /* 64B */
    {11, 40, (1ULL << 40) - 1}, /* 1T */
    {12, 44, (1ULL << 44) - 1}, /* 16T */
    {16, 60, (1ULL << 60) - 1}, /* 1E */
};
#define NUM_WIDTH_TIERS (countof(WIDTH_TIERS))

/* Constants for each width tier (use WIDTH_TIERS array when possible) */
#define MAX_ENTRIES_W5 ((1ULL << 16) - 1)
#define MAX_ENTRIES_W6 ((1ULL << 20) - 1)
#define MAX_ENTRIES_W7 ((1ULL << 24) - 1)
#define MAX_ENTRIES_W8 ((1ULL << 28) - 1)
#define MAX_ENTRIES_W9 ((1ULL << 32) - 1)
#define MAX_ENTRIES_W10 ((1ULL << 36) - 1)
#define MAX_ENTRIES_W11 ((1ULL << 40) - 1)
#define MAX_ENTRIES_W12 ((1ULL << 44) - 1)
#define MAX_ENTRIES_W16 ((1ULL << 60) - 1)

/* ====================================================================
 * Per-Level Metadata
 * ==================================================================== */
typedef struct multilruLevel {
    uint64_t head;   /* Index of level head marker (sentinel) */
    uint64_t tail;   /* Index of tail entry (LRU at this level) */
    uint64_t count;  /* Number of entries at this level */
    uint64_t weight; /* Total weight of entries at this level */
} multilruLevel;

/* ====================================================================
 * Main Structure
 * ==================================================================== */
struct multilru {
    /* Entry storage - type depends on entryWidth */
    void *entries;
    uint8_t entryWidth; /* 5, 6, 7, 8, 9, 10, 11, 12, or 16 bytes */

    /* Optional per-entry weights for size-aware eviction */
    uint64_t *weights;    /* NULL if weight tracking disabled */
    uint64_t totalWeight; /* Sum of all entry weights */

    /* Per-level tracking with tail pointers for S4LRU demotion */
    multilruLevel *levels;
    size_t maxLevels;

    /* Level occupancy mask for fast updateLowest() */
    uint64_t levelMask; /* Bit i set = level i has entries */

    /* Slot allocation: high water mark + intrusive free list for holes */
    uint64_t nextFresh; /* Next never-used index (sequential allocation) */
    uint64_t freeHead;  /* Head of recycled slots chain (0 = empty) */
    uint64_t freeCount; /* Number of recycled slots in free list */

    /* Policy configuration */
    multilruPolicy policy;
    multilruEvictStrategy evictStrategy;
    uint64_t maxCount;  /* 0 = unlimited */
    uint64_t maxWeight; /* 0 = unlimited */

    /* Eviction control */
    bool autoEvict; /* Auto-evict on insert when over limits (default: true) */
    void (*evictCallback)(size_t evictedPtr,
                          void *userData); /* Called on true eviction */
    void *evictCallbackData;               /* User data passed to callback */

    /* State */
    uint64_t capacity; /* Allocated entry slots */
    uint64_t count;    /* Active entries */
    uint64_t lowest;   /* Current LRU entry index */
    uint64_t
        targetCapacity; /* Initial capacity target (cleared after first grow) */

    /* Operational statistics (lifetime counters) */
    uint64_t statInserts;    /* Total insert operations */
    uint64_t statEvictions;  /* True evictions from level 0 */
    uint64_t statDemotions;  /* Demotions (level N -> N-1) */
    uint64_t statPromotions; /* Promotions (level N -> N+1) */
    uint64_t statDeletes;    /* Direct delete operations */
};

/* ====================================================================
 * Entry Accessor Functions
 * ====================================================================
 * Unified interface for reading/writing entry fields across all widths.
 * Each width has optimized accessors based on its bit layout.
 */

/* Get entry pointer for global index */
DK_STATIC inline void *entryPtr(const multilru *mlru, uint64_t idx) {
    return (uint8_t *)mlru->entries + idx * mlru->entryWidth;
}

/* ====================================================================
 * Width-Specific Entry Accessors
 * ====================================================================
 * Optimized accessors for each supported width.
 */

/* --- 5-byte entries (16-bit addresses) --- */
DK_STATIC inline uint64_t entry5GetPrev(const void *e) {
    const uint8_t *p = e;
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8);
}
DK_STATIC inline uint64_t entry5GetNext(const void *e) {
    const uint8_t *p = e;
    return (uint64_t)p[2] | ((uint64_t)p[3] << 8);
}
DK_STATIC inline uint8_t entry5GetLevel(const void *e) {
    return ((const uint8_t *)e)[4] & 0x3F;
}
DK_STATIC inline bool entry5GetPopulated(const void *e) {
    return (((const uint8_t *)e)[4] >> 6) & 1;
}
DK_STATIC inline bool entry5GetHead(const void *e) {
    return (((const uint8_t *)e)[4] >> 7) & 1;
}
DK_STATIC inline void entry5SetPrev(void *e, uint64_t v) {
    uint8_t *p = e;
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}
DK_STATIC inline void entry5SetNext(void *e, uint64_t v) {
    uint8_t *p = e;
    p[2] = v & 0xFF;
    p[3] = (v >> 8) & 0xFF;
}
DK_STATIC inline void entry5SetLevel(void *e, uint8_t v) {
    uint8_t *p = e;
    p[4] = (p[4] & 0xC0) | (v & 0x3F);
}
DK_STATIC inline void entry5SetPopulated(void *e, bool v) {
    uint8_t *p = e;
    p[4] = (p[4] & 0xBF) | ((v ? 1 : 0) << 6);
}
DK_STATIC inline void entry5SetHead(void *e, bool v) {
    uint8_t *p = e;
    p[4] = (p[4] & 0x7F) | ((v ? 1 : 0) << 7);
}

/* --- 6-byte entries (20-bit addresses) --- */
/* Layout: prev[0:19], next[20:39], level[40:45], pop[46], head[47] */
DK_STATIC inline uint64_t entry6GetPrev(const void *e) {
    return readBitsLE(e, 0, 20);
}
DK_STATIC inline uint64_t entry6GetNext(const void *e) {
    return readBitsLE(e, 20, 20);
}
DK_STATIC inline uint8_t entry6GetLevel(const void *e) {
    return readBitsLE(e, 40, 6);
}
DK_STATIC inline bool entry6GetPopulated(const void *e) {
    return readBitsLE(e, 46, 1);
}
DK_STATIC inline bool entry6GetHead(const void *e) {
    return readBitsLE(e, 47, 1);
}
DK_STATIC inline void entry6SetPrev(void *e, uint64_t v) {
    writeBitsLE(e, 0, 20, v);
}
DK_STATIC inline void entry6SetNext(void *e, uint64_t v) {
    writeBitsLE(e, 20, 20, v);
}
DK_STATIC inline void entry6SetLevel(void *e, uint8_t v) {
    writeBitsLE(e, 40, 6, v);
}
DK_STATIC inline void entry6SetPopulated(void *e, bool v) {
    writeBitsLE(e, 46, 1, v);
}
DK_STATIC inline void entry6SetHead(void *e, bool v) {
    writeBitsLE(e, 47, 1, v);
}

/* --- 7-byte entries (24-bit addresses) --- */
/* Layout: prev[0:23], next[24:47], level[48:53], pop[54], head[55] */
DK_STATIC inline uint64_t entry7GetPrev(const void *e) {
    const uint8_t *p = e;
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16);
}
DK_STATIC inline uint64_t entry7GetNext(const void *e) {
    const uint8_t *p = e;
    return (uint64_t)p[3] | ((uint64_t)p[4] << 8) | ((uint64_t)p[5] << 16);
}
DK_STATIC inline uint8_t entry7GetLevel(const void *e) {
    return ((const uint8_t *)e)[6] & 0x3F;
}
DK_STATIC inline bool entry7GetPopulated(const void *e) {
    return (((const uint8_t *)e)[6] >> 6) & 1;
}
DK_STATIC inline bool entry7GetHead(const void *e) {
    return (((const uint8_t *)e)[6] >> 7) & 1;
}
DK_STATIC inline void entry7SetPrev(void *e, uint64_t v) {
    uint8_t *p = e;
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
}
DK_STATIC inline void entry7SetNext(void *e, uint64_t v) {
    uint8_t *p = e;
    p[3] = v & 0xFF;
    p[4] = (v >> 8) & 0xFF;
    p[5] = (v >> 16) & 0xFF;
}
DK_STATIC inline void entry7SetLevel(void *e, uint8_t v) {
    uint8_t *p = e;
    p[6] = (p[6] & 0xC0) | (v & 0x3F);
}
DK_STATIC inline void entry7SetPopulated(void *e, bool v) {
    uint8_t *p = e;
    p[6] = (p[6] & 0xBF) | ((v ? 1 : 0) << 6);
}
DK_STATIC inline void entry7SetHead(void *e, bool v) {
    uint8_t *p = e;
    p[6] = (p[6] & 0x7F) | ((v ? 1 : 0) << 7);
}

/* Native-width loads and stores at any alignment.  Entries sit at
 * idx * entryWidth, so a 9-byte entry's 32-bit fields (and any field of
 * a width that does not divide the allocation's alignment) are
 * misaligned; memcpy of a constant size compiles to one load or store
 * where the target allows unaligned access, and is defined everywhere. */
DK_INLINE_ALWAYS uint32_t entryLoad32(const void *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
DK_INLINE_ALWAYS void entryStore32(void *p, uint32_t v) {
    memcpy(p, &v, sizeof(v));
}
DK_INLINE_ALWAYS uint64_t entryLoad64(const void *p) {
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
DK_INLINE_ALWAYS void entryStore64(void *p, uint64_t v) {
    memcpy(p, &v, sizeof(v));
}
DK_INLINE_ALWAYS __uint128_t entryLoad128(const void *p) {
    __uint128_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
DK_INLINE_ALWAYS void entryStore128(void *p, __uint128_t v) {
    memcpy(p, &v, sizeof(v));
}

/* --- 8-byte entries (28-bit addresses) --- */
/* Layout: prev[0:27], next[28:55], level[56:61], pop[62], head[63] */
DK_STATIC inline uint64_t entry8GetPrev(const void *e) {
    return entryLoad64(e) & 0x0FFFFFFF;
}
DK_STATIC inline uint64_t entry8GetNext(const void *e) {
    return (entryLoad64(e) >> 28) & 0x0FFFFFFF;
}
DK_STATIC inline uint8_t entry8GetLevel(const void *e) {
    return (entryLoad64(e) >> 56) & 0x3F;
}
DK_STATIC inline bool entry8GetPopulated(const void *e) {
    return (entryLoad64(e) >> 62) & 1;
}
DK_STATIC inline bool entry8GetHead(const void *e) {
    return (entryLoad64(e) >> 63) & 1;
}
DK_STATIC inline void entry8SetPrev(void *e, uint64_t v) {
    entryStore64(e, (entryLoad64(e) & ~0x0FFFFFFFULL) | (v & 0x0FFFFFFF));
}
DK_STATIC inline void entry8SetNext(void *e, uint64_t v) {
    entryStore64(e, (entryLoad64(e) & ~(0x0FFFFFFFULL << 28)) |
                        ((v & 0x0FFFFFFF) << 28));
}
DK_STATIC inline void entry8SetLevel(void *e, uint8_t v) {
    entryStore64(e, (entryLoad64(e) & ~(0x3FULL << 56)) |
                        ((uint64_t)(v & 0x3F) << 56));
}
DK_STATIC inline void entry8SetPopulated(void *e, bool v) {
    entryStore64(e, (entryLoad64(e) & ~(1ULL << 62)) |
                        ((uint64_t)(v ? 1 : 0) << 62));
}
DK_STATIC inline void entry8SetHead(void *e, bool v) {
    entryStore64(e, (entryLoad64(e) & ~(1ULL << 63)) |
                        ((uint64_t)(v ? 1 : 0) << 63));
}

/* --- 9-byte entries (32-bit addresses) --- */
/* Layout: prev[0:31], next[32:63], level[64:69], pop[70], head[71] */
DK_STATIC inline uint64_t entry9GetPrev(const void *e) {
    return entryLoad32(e);
}
DK_STATIC inline uint64_t entry9GetNext(const void *e) {
    return entryLoad32((const uint8_t *)e + 4);
}
DK_STATIC inline uint8_t entry9GetLevel(const void *e) {
    return ((const uint8_t *)e)[8] & 0x3F;
}
DK_STATIC inline bool entry9GetPopulated(const void *e) {
    return (((const uint8_t *)e)[8] >> 6) & 1;
}
DK_STATIC inline bool entry9GetHead(const void *e) {
    return (((const uint8_t *)e)[8] >> 7) & 1;
}
DK_STATIC inline void entry9SetPrev(void *e, uint64_t v) {
    entryStore32(e, (uint32_t)v);
}
DK_STATIC inline void entry9SetNext(void *e, uint64_t v) {
    entryStore32((uint8_t *)e + 4, (uint32_t)v);
}
DK_STATIC inline void entry9SetLevel(void *e, uint8_t v) {
    uint8_t *p = e;
    p[8] = (p[8] & 0xC0) | (v & 0x3F);
}
DK_STATIC inline void entry9SetPopulated(void *e, bool v) {
    uint8_t *p = e;
    p[8] = (p[8] & 0xBF) | ((v ? 1 : 0) << 6);
}
DK_STATIC inline void entry9SetHead(void *e, bool v) {
    uint8_t *p = e;
    p[8] = (p[8] & 0x7F) | ((v ? 1 : 0) << 7);
}

/* --- 10-byte entries (36-bit addresses) --- */
/* Layout: prev[0:35], next[36:71], level[72:77], pop[78], head[79] */
DK_STATIC inline uint64_t entry10GetPrev(const void *e) {
    return readBitsLE(e, 0, 36);
}
DK_STATIC inline uint64_t entry10GetNext(const void *e) {
    return readBitsLE(e, 36, 36);
}
DK_STATIC inline uint8_t entry10GetLevel(const void *e) {
    return readBitsLE(e, 72, 6);
}
DK_STATIC inline bool entry10GetPopulated(const void *e) {
    return readBitsLE(e, 78, 1);
}
DK_STATIC inline bool entry10GetHead(const void *e) {
    return readBitsLE(e, 79, 1);
}
DK_STATIC inline void entry10SetPrev(void *e, uint64_t v) {
    writeBitsLE(e, 0, 36, v);
}
DK_STATIC inline void entry10SetNext(void *e, uint64_t v) {
    writeBitsLE(e, 36, 36, v);
}
DK_STATIC inline void entry10SetLevel(void *e, uint8_t v) {
    writeBitsLE(e, 72, 6, v);
}
DK_STATIC inline void entry10SetPopulated(void *e, bool v) {
    writeBitsLE(e, 78, 1, v);
}
DK_STATIC inline void entry10SetHead(void *e, bool v) {
    writeBitsLE(e, 79, 1, v);
}

/* --- 11-byte entries (40-bit addresses) --- */
/* Layout: prev[0:39], next[40:79], level[80:85], pop[86], head[87] */
DK_STATIC inline uint64_t entry11GetPrev(const void *e) {
    return readBitsLE(e, 0, 40);
}
DK_STATIC inline uint64_t entry11GetNext(const void *e) {
    return readBitsLE(e, 40, 40);
}
DK_STATIC inline uint8_t entry11GetLevel(const void *e) {
    return readBitsLE(e, 80, 6);
}
DK_STATIC inline bool entry11GetPopulated(const void *e) {
    return readBitsLE(e, 86, 1);
}
DK_STATIC inline bool entry11GetHead(const void *e) {
    return readBitsLE(e, 87, 1);
}
DK_STATIC inline void entry11SetPrev(void *e, uint64_t v) {
    writeBitsLE(e, 0, 40, v);
}
DK_STATIC inline void entry11SetNext(void *e, uint64_t v) {
    writeBitsLE(e, 40, 40, v);
}
DK_STATIC inline void entry11SetLevel(void *e, uint8_t v) {
    writeBitsLE(e, 80, 6, v);
}
DK_STATIC inline void entry11SetPopulated(void *e, bool v) {
    writeBitsLE(e, 86, 1, v);
}
DK_STATIC inline void entry11SetHead(void *e, bool v) {
    writeBitsLE(e, 87, 1, v);
}

/* --- 12-byte entries (44-bit addresses) --- */
/* Layout: prev[0:43], next[44:87], level[88:93], pop[94], head[95] */
DK_STATIC inline uint64_t entry12GetPrev(const void *e) {
    return readBitsLE(e, 0, 44);
}
DK_STATIC inline uint64_t entry12GetNext(const void *e) {
    return readBitsLE(e, 44, 44);
}
DK_STATIC inline uint8_t entry12GetLevel(const void *e) {
    return readBitsLE(e, 88, 6);
}
DK_STATIC inline bool entry12GetPopulated(const void *e) {
    return readBitsLE(e, 94, 1);
}
DK_STATIC inline bool entry12GetHead(const void *e) {
    return readBitsLE(e, 95, 1);
}
DK_STATIC inline void entry12SetPrev(void *e, uint64_t v) {
    writeBitsLE(e, 0, 44, v);
}
DK_STATIC inline void entry12SetNext(void *e, uint64_t v) {
    writeBitsLE(e, 44, 44, v);
}
DK_STATIC inline void entry12SetLevel(void *e, uint8_t v) {
    writeBitsLE(e, 88, 6, v);
}
DK_STATIC inline void entry12SetPopulated(void *e, bool v) {
    writeBitsLE(e, 94, 1, v);
}
DK_STATIC inline void entry12SetHead(void *e, bool v) {
    writeBitsLE(e, 95, 1, v);
}

/* --- 16-byte entries (60-bit addresses) --- */
/* Layout: prev[0:59], next[60:119], level[120:125], pop[126], head[127] */
DK_STATIC inline uint64_t entry16GetPrev(const void *e) {
    return (uint64_t)(entryLoad128(e) & (((__uint128_t)1 << 60) - 1));
}
DK_STATIC inline uint64_t entry16GetNext(const void *e) {
    return (uint64_t)((entryLoad128(e) >> 60) & (((__uint128_t)1 << 60) - 1));
}
DK_STATIC inline uint8_t entry16GetLevel(const void *e) {
    return (uint8_t)((entryLoad128(e) >> 120) & 0x3F);
}
DK_STATIC inline bool entry16GetPopulated(const void *e) {
    return (entryLoad128(e) >> 126) & 1;
}
DK_STATIC inline bool entry16GetHead(const void *e) {
    return (entryLoad128(e) >> 127) & 1;
}
DK_STATIC inline void entry16SetPrev(void *e, uint64_t v) {
    const __uint128_t mask = ((__uint128_t)1 << 60) - 1;
    entryStore128(e, (entryLoad128(e) & ~mask) | (v & mask));
}
DK_STATIC inline void entry16SetNext(void *e, uint64_t v) {
    const __uint128_t mask = (((__uint128_t)1 << 60) - 1) << 60;
    entryStore128(e,
                  (entryLoad128(e) & ~mask) |
                      (((__uint128_t)v & (((__uint128_t)1 << 60) - 1)) << 60));
}
DK_STATIC inline void entry16SetLevel(void *e, uint8_t v) {
    const __uint128_t mask = (__uint128_t)0x3F << 120;
    entryStore128(e, (entryLoad128(e) & ~mask) |
                         (((__uint128_t)(v & 0x3F)) << 120));
}
DK_STATIC inline void entry16SetPopulated(void *e, bool v) {
    const __uint128_t mask = (__uint128_t)1 << 126;
    entryStore128(e, (entryLoad128(e) & ~mask) |
                         (((__uint128_t)(v ? 1 : 0)) << 126));
}
DK_STATIC inline void entry16SetHead(void *e, bool v) {
    const __uint128_t mask = (__uint128_t)1 << 127;
    entryStore128(e, (entryLoad128(e) & ~mask) |
                         (((__uint128_t)(v ? 1 : 0)) << 127));
}

/* ====================================================================
 * Unified Entry Accessors (dispatch by width)
 * ====================================================================
 * These functions dispatch to the appropriate width-specific accessor.
 * Using a switch on entryWidth allows the compiler to optimize.
 *
 * Every list operation runs through these dispatchers and the bit
 * primitives under them, so they are always inlined.  A plain inline hint
 * is not enough: this file is large enough that GCC's unit-growth budget
 * runs out first and leaves the hottest accessors as out-of-line calls.
 */

DK_INLINE_ALWAYS uint64_t entryGetPrev(const multilru *mlru, uint64_t idx) {
    const void *e = entryPtr(mlru, idx);
    switch (mlru->entryWidth) {
    case 5:
        return entry5GetPrev(e);
    case 6:
        return entry6GetPrev(e);
    case 7:
        return entry7GetPrev(e);
    case 8:
        return entry8GetPrev(e);
    case 9:
        return entry9GetPrev(e);
    case 10:
        return entry10GetPrev(e);
    case 11:
        return entry11GetPrev(e);
    case 12:
        return entry12GetPrev(e);
    case 16:
        return entry16GetPrev(e);
    default:
        return 0;
    }
}

DK_INLINE_ALWAYS uint64_t entryGetNext(const multilru *mlru, uint64_t idx) {
    const void *e = entryPtr(mlru, idx);
    switch (mlru->entryWidth) {
    case 5:
        return entry5GetNext(e);
    case 6:
        return entry6GetNext(e);
    case 7:
        return entry7GetNext(e);
    case 8:
        return entry8GetNext(e);
    case 9:
        return entry9GetNext(e);
    case 10:
        return entry10GetNext(e);
    case 11:
        return entry11GetNext(e);
    case 12:
        return entry12GetNext(e);
    case 16:
        return entry16GetNext(e);
    default:
        return 0;
    }
}

DK_INLINE_ALWAYS uint8_t entryGetLevel(const multilru *mlru, uint64_t idx) {
    const void *e = entryPtr(mlru, idx);
    switch (mlru->entryWidth) {
    case 5:
        return entry5GetLevel(e);
    case 6:
        return entry6GetLevel(e);
    case 7:
        return entry7GetLevel(e);
    case 8:
        return entry8GetLevel(e);
    case 9:
        return entry9GetLevel(e);
    case 10:
        return entry10GetLevel(e);
    case 11:
        return entry11GetLevel(e);
    case 12:
        return entry12GetLevel(e);
    case 16:
        return entry16GetLevel(e);
    default:
        return 0;
    }
}

DK_INLINE_ALWAYS bool entryGetPopulated(const multilru *mlru, uint64_t idx) {
    const void *e = entryPtr(mlru, idx);
    switch (mlru->entryWidth) {
    case 5:
        return entry5GetPopulated(e);
    case 6:
        return entry6GetPopulated(e);
    case 7:
        return entry7GetPopulated(e);
    case 8:
        return entry8GetPopulated(e);
    case 9:
        return entry9GetPopulated(e);
    case 10:
        return entry10GetPopulated(e);
    case 11:
        return entry11GetPopulated(e);
    case 12:
        return entry12GetPopulated(e);
    case 16:
        return entry16GetPopulated(e);
    default:
        return false;
    }
}

DK_INLINE_ALWAYS bool entryGetHead(const multilru *mlru, uint64_t idx) {
    const void *e = entryPtr(mlru, idx);
    switch (mlru->entryWidth) {
    case 5:
        return entry5GetHead(e);
    case 6:
        return entry6GetHead(e);
    case 7:
        return entry7GetHead(e);
    case 8:
        return entry8GetHead(e);
    case 9:
        return entry9GetHead(e);
    case 10:
        return entry10GetHead(e);
    case 11:
        return entry11GetHead(e);
    case 12:
        return entry12GetHead(e);
    case 16:
        return entry16GetHead(e);
    default:
        return false;
    }
}

DK_INLINE_ALWAYS void entrySetPrev(multilru *mlru, uint64_t idx, uint64_t v) {
    void *e = entryPtr(mlru, idx);
    switch (mlru->entryWidth) {
    case 5:
        entry5SetPrev(e, v);
        break;
    case 6:
        entry6SetPrev(e, v);
        break;
    case 7:
        entry7SetPrev(e, v);
        break;
    case 8:
        entry8SetPrev(e, v);
        break;
    case 9:
        entry9SetPrev(e, v);
        break;
    case 10:
        entry10SetPrev(e, v);
        break;
    case 11:
        entry11SetPrev(e, v);
        break;
    case 12:
        entry12SetPrev(e, v);
        break;
    case 16:
        entry16SetPrev(e, v);
        break;
    }
}

DK_INLINE_ALWAYS void entrySetNext(multilru *mlru, uint64_t idx, uint64_t v) {
    void *e = entryPtr(mlru, idx);
    switch (mlru->entryWidth) {
    case 5:
        entry5SetNext(e, v);
        break;
    case 6:
        entry6SetNext(e, v);
        break;
    case 7:
        entry7SetNext(e, v);
        break;
    case 8:
        entry8SetNext(e, v);
        break;
    case 9:
        entry9SetNext(e, v);
        break;
    case 10:
        entry10SetNext(e, v);
        break;
    case 11:
        entry11SetNext(e, v);
        break;
    case 12:
        entry12SetNext(e, v);
        break;
    case 16:
        entry16SetNext(e, v);
        break;
    }
}

DK_INLINE_ALWAYS void entrySetLevel(multilru *mlru, uint64_t idx, uint8_t v) {
    void *e = entryPtr(mlru, idx);
    switch (mlru->entryWidth) {
    case 5:
        entry5SetLevel(e, v);
        break;
    case 6:
        entry6SetLevel(e, v);
        break;
    case 7:
        entry7SetLevel(e, v);
        break;
    case 8:
        entry8SetLevel(e, v);
        break;
    case 9:
        entry9SetLevel(e, v);
        break;
    case 10:
        entry10SetLevel(e, v);
        break;
    case 11:
        entry11SetLevel(e, v);
        break;
    case 12:
        entry12SetLevel(e, v);
        break;
    case 16:
        entry16SetLevel(e, v);
        break;
    }
}

DK_INLINE_ALWAYS void entrySetPopulated(multilru *mlru, uint64_t idx, bool v) {
    void *e = entryPtr(mlru, idx);
    switch (mlru->entryWidth) {
    case 5:
        entry5SetPopulated(e, v);
        break;
    case 6:
        entry6SetPopulated(e, v);
        break;
    case 7:
        entry7SetPopulated(e, v);
        break;
    case 8:
        entry8SetPopulated(e, v);
        break;
    case 9:
        entry9SetPopulated(e, v);
        break;
    case 10:
        entry10SetPopulated(e, v);
        break;
    case 11:
        entry11SetPopulated(e, v);
        break;
    case 12:
        entry12SetPopulated(e, v);
        break;
    case 16:
        entry16SetPopulated(e, v);
        break;
    }
}

DK_INLINE_ALWAYS void entrySetHead(multilru *mlru, uint64_t idx, bool v) {
    void *e = entryPtr(mlru, idx);
    switch (mlru->entryWidth) {
    case 5:
        entry5SetHead(e, v);
        break;
    case 6:
        entry6SetHead(e, v);
        break;
    case 7:
        entry7SetHead(e, v);
        break;
    case 8:
        entry8SetHead(e, v);
        break;
    case 9:
        entry9SetHead(e, v);
        break;
    case 10:
        entry10SetHead(e, v);
        break;
    case 11:
        entry11SetHead(e, v);
        break;
    case 12:
        entry12SetHead(e, v);
        break;
    case 16:
        entry16SetHead(e, v);
        break;
    }
}

/* Convenience macros that forward to the new accessor functions */
#define E_PREV(mlru, idx) entryGetPrev(mlru, idx)
#define E_NEXT(mlru, idx) entryGetNext(mlru, idx)
#define E_LEVEL(mlru, idx) entryGetLevel(mlru, idx)
#define E_POPULATED(mlru, idx) entryGetPopulated(mlru, idx)
#define E_HEAD(mlru, idx) entryGetHead(mlru, idx)

#define E_SET_PREV(mlru, idx, v) entrySetPrev(mlru, idx, v)
#define E_SET_NEXT(mlru, idx, v) entrySetNext(mlru, idx, v)
#define E_SET_LEVEL(mlru, idx, v) entrySetLevel(mlru, idx, v)
#define E_SET_POPULATED(mlru, idx, v) entrySetPopulated(mlru, idx, v)
#define E_SET_HEAD(mlru, idx, v) entrySetHead(mlru, idx, v)

/* Clear all entry fields to zero */
DK_STATIC void entryClear(multilru *mlru, uint64_t idx) {
    memset(entryPtr(mlru, idx), 0, mlru->entryWidth);
}

/* ====================================================================
 * Width Selection
 * ==================================================================== */

/* Select the smallest width that can hold the given capacity */
DK_STATIC uint8_t selectWidth(uint64_t capacity) {
    for (size_t i = 0; i < NUM_WIDTH_TIERS; i++) {
        if (capacity <= WIDTH_TIERS[i].maxEntries) {
            return WIDTH_TIERS[i].width;
        }
    }
    /* Return largest width as fallback */
    return WIDTH_TIERS[NUM_WIDTH_TIERS - 1].width;
}

/* Get the maximum entries supported by a given width */
DK_STATIC uint64_t maxEntriesForWidth(uint8_t width) {
    for (size_t i = 0; i < NUM_WIDTH_TIERS; i++) {
        if (WIDTH_TIERS[i].width == width) {
            return WIDTH_TIERS[i].maxEntries;
        }
    }
    /* Return largest capacity as fallback */
    return WIDTH_TIERS[NUM_WIDTH_TIERS - 1].maxEntries;
}

/* Get the next larger width tier (returns same width if already at max) */
DK_STATIC uint8_t nextWidth(uint8_t currentWidth) {
    for (size_t i = 0; i < NUM_WIDTH_TIERS - 1; i++) {
        if (WIDTH_TIERS[i].width == currentWidth) {
            return WIDTH_TIERS[i + 1].width;
        }
    }
    /* Already at largest or unknown width */
    return currentWidth;
}

/* ====================================================================
 * Intrusive Free List Operations
 * ====================================================================
 * Free entries are chained using their prev/next fields.
 * This provides O(1) allocation without external tracking arrays.
 */

/* Push entry onto free list */
DK_STATIC void freeListPush(multilru *mlru, uint64_t idx) {
    entryClear(mlru, idx);
    E_SET_NEXT(mlru, idx, mlru->freeHead);
    mlru->freeHead = idx;
    mlru->freeCount++;
}

/* Pop entry from free list - O(1) */
DK_STATIC uint64_t freeListPop(multilru *mlru) {
    if (mlru->freeHead == 0) {
        return 0;
    }

    uint64_t idx = mlru->freeHead;
    mlru->freeHead = E_NEXT(mlru, idx);
    mlru->freeCount--;

    entryClear(mlru, idx);
    return idx;
}

/* ====================================================================
 * Width Upgrade
 * ==================================================================== */

/* Read entry fields directly from a buffer with given width (for migration) */
DK_STATIC void entryReadByWidth(const void *entries, uint8_t width,
                                uint64_t idx, uint64_t *prev, uint64_t *next,
                                uint8_t *level, bool *populated, bool *isHead) {
    const void *e = (const uint8_t *)entries + idx * width;
    switch (width) {
    case 5:
        *prev = entry5GetPrev(e);
        *next = entry5GetNext(e);
        *level = entry5GetLevel(e);
        *populated = entry5GetPopulated(e);
        *isHead = entry5GetHead(e);
        break;
    case 6:
        *prev = entry6GetPrev(e);
        *next = entry6GetNext(e);
        *level = entry6GetLevel(e);
        *populated = entry6GetPopulated(e);
        *isHead = entry6GetHead(e);
        break;
    case 7:
        *prev = entry7GetPrev(e);
        *next = entry7GetNext(e);
        *level = entry7GetLevel(e);
        *populated = entry7GetPopulated(e);
        *isHead = entry7GetHead(e);
        break;
    case 8:
        *prev = entry8GetPrev(e);
        *next = entry8GetNext(e);
        *level = entry8GetLevel(e);
        *populated = entry8GetPopulated(e);
        *isHead = entry8GetHead(e);
        break;
    case 9:
        *prev = entry9GetPrev(e);
        *next = entry9GetNext(e);
        *level = entry9GetLevel(e);
        *populated = entry9GetPopulated(e);
        *isHead = entry9GetHead(e);
        break;
    case 10:
        *prev = entry10GetPrev(e);
        *next = entry10GetNext(e);
        *level = entry10GetLevel(e);
        *populated = entry10GetPopulated(e);
        *isHead = entry10GetHead(e);
        break;
    case 11:
        *prev = entry11GetPrev(e);
        *next = entry11GetNext(e);
        *level = entry11GetLevel(e);
        *populated = entry11GetPopulated(e);
        *isHead = entry11GetHead(e);
        break;
    case 12:
        *prev = entry12GetPrev(e);
        *next = entry12GetNext(e);
        *level = entry12GetLevel(e);
        *populated = entry12GetPopulated(e);
        *isHead = entry12GetHead(e);
        break;
    case 16:
        *prev = entry16GetPrev(e);
        *next = entry16GetNext(e);
        *level = entry16GetLevel(e);
        *populated = entry16GetPopulated(e);
        *isHead = entry16GetHead(e);
        break;
    default:
        *prev = *next = 0;
        *level = 0;
        *populated = *isHead = false;
    }
}

/* Upgrade entry width to support more entries */
DK_STATIC bool upgradeWidth(multilru *mlru, uint8_t newWidth) {
    if (newWidth <= mlru->entryWidth) {
        return true; /* Already at this width or larger */
    }

    void *oldEntries = mlru->entries;
    uint8_t oldWidth = mlru->entryWidth;
    uint64_t oldCapacity = mlru->capacity;

    /* Allocate new entries array with new width */
    /* @own: container; free=multilruFree */
    void *newEntries = zcalloc(oldCapacity, newWidth);
    if (!newEntries) {
        return false;
    }

    /* Switch to new width so E_SET macros work correctly */
    mlru->entries = newEntries;
    mlru->entryWidth = newWidth;

    /* Migrate all entries */
    for (uint64_t i = 0; i < oldCapacity; i++) {
        uint64_t prev, next;
        uint8_t level;
        bool populated, isHead;

        /* Read from old array using old width */
        entryReadByWidth(oldEntries, oldWidth, i, &prev, &next, &level,
                         &populated, &isHead);

        /* Write to new array using new width (via mlru which now has new width)
         */
        E_SET_PREV(mlru, i, prev);
        E_SET_NEXT(mlru, i, next);
        E_SET_LEVEL(mlru, i, level);
        E_SET_POPULATED(mlru, i, populated);
        E_SET_HEAD(mlru, i, isHead);
    }

    zfree(oldEntries);
    return true;
}

/* ====================================================================
 * Growth
 * ==================================================================== */
DK_STATIC bool grow(multilru *mlru) {
    const uint64_t oldCapacity = mlru->capacity;
    const size_t oldBytes = (size_t)mlru->entryWidth * oldCapacity;
    size_t targetBytes;
    if (oldBytes == 0 && mlru->targetCapacity > 0) {
        /* Use requested initial capacity */
        targetBytes = (size_t)mlru->targetCapacity * mlru->entryWidth;
        mlru->targetCapacity = 0; /* Clear after use */
    } else {
        targetBytes = oldBytes == 0 ? 4096 : fibbufNextSizeBuffer(oldBytes);
    }
    size_t newBytes = jebufSizeAllocation(targetBytes);
    uint64_t newCapacity = newBytes / mlru->entryWidth;

    /* Check if we need to upgrade width */
    uint64_t maxForWidth = maxEntriesForWidth(mlru->entryWidth);
    if (newCapacity > maxForWidth) {
        /* Try to upgrade to next width */
        uint8_t newWidth = nextWidth(mlru->entryWidth);
        if (newWidth == mlru->entryWidth) {
            /* Already at largest width, cap capacity */
            newCapacity = maxForWidth;
            if (oldCapacity >= maxForWidth) {
                return false; /* Can't grow further */
            }
            goto do_grow;
        }

        /* Perform width upgrade */
        if (!upgradeWidth(mlru, newWidth)) {
            return false;
        }

        /* Recalculate capacity for new width */
        newBytes = jebufSizeAllocation(targetBytes);
        newCapacity = newBytes / mlru->entryWidth;
        maxForWidth = maxEntriesForWidth(mlru->entryWidth);
        if (newCapacity > maxForWidth) {
            newCapacity = maxForWidth;
        }
    }

do_grow:
    const uint64_t actualNewCapacity = newCapacity;
    const size_t currentBytes = (size_t)mlru->entryWidth * oldCapacity;

    /* Use oldCapacity == 0 to determine if this is initial allocation,
     * not !mlru->entries, because upgradeWidth may have set entries to
     * a non-NULL pointer even with zero capacity. */
    if (oldCapacity == 0) {
        if (mlru->entries) {
            /* upgradeWidth allocated a zero-capacity entries array */
            zfree(mlru->entries);
        }
        /* @own: container; free=multilruFree */
        mlru->entries = zcalloc(actualNewCapacity, mlru->entryWidth);
        if (mlru->weights) {
            /* weights might be the marker value (1) - allocate fresh */
            // cppcheck-suppress intToPointerCast - checking for disabled
            // sentinel value
            if (mlru->weights == (uint64_t *)1) {
                mlru->weights = NULL;
            }
            zfree(mlru->weights);
            /* @own: container; free=multilruFree */
            mlru->weights = zcalloc(actualNewCapacity, sizeof(uint64_t));
        }
    } else {
        /* @own: container; free=multilruFree */
        mlru->entries = zrealloc(mlru->entries,
                                 (size_t)actualNewCapacity * mlru->entryWidth);
        /* Zero new entries */
        memset((uint8_t *)mlru->entries + currentBytes, 0,
               (size_t)(actualNewCapacity - oldCapacity) * mlru->entryWidth);

        if (mlru->weights) {
            /* @own: container; free=multilruFree */
            mlru->weights =
                zrealloc(mlru->weights, actualNewCapacity * sizeof(uint64_t));
            memset(mlru->weights + oldCapacity, 0,
                   (actualNewCapacity - oldCapacity) * sizeof(uint64_t));
        }
    }

    /* New slots are available via nextFresh - no need to push to free list.
     * This makes grow() O(1) instead of O(newCapacity - oldCapacity). */
    mlru->capacity = actualNewCapacity;
    return true;
}

/* ====================================================================
 * Level Operations
 * ==================================================================== */

/* Insert entry at head of level (most recently used position) */
DK_STATIC void insertAtLevelHead(multilru *mlru, uint64_t idx, size_t level) {
    uint64_t headIdx = mlru->levels[level].head;

    /* Get current first entry in level (entry before head marker) */
    uint64_t oldFirst = E_PREV(mlru, headIdx);

    /* Set up new entry */
    E_SET_LEVEL(mlru, idx, level);
    E_SET_POPULATED(mlru, idx, 1);
    E_SET_HEAD(mlru, idx, 0);
    E_SET_PREV(mlru, idx, oldFirst);
    E_SET_NEXT(mlru, idx, headIdx);

    /* Link old first's next to new entry */
    if (oldFirst) {
        E_SET_NEXT(mlru, oldFirst, idx);
    }

    /* Link head marker's prev to new entry */
    E_SET_PREV(mlru, headIdx, idx);

    /* Update level count and weight */
    mlru->levels[level].count++;
    if (mlru->weights) {
        mlru->levels[level].weight += mlru->weights[idx];
    }

    /* Update levelMask - this level now has entries */
    mlru->levelMask |= (1ULL << level);

    /* Update level tail if this is first entry in level */
    if (mlru->levels[level].tail == 0) {
        mlru->levels[level].tail = idx;
    }

    /* Update global lowest if necessary */
    if (mlru->lowest == 0) {
        mlru->lowest = idx;
    } else {
        /* New entry might be new lowest if at level 0 */
        if (level == 0) {
            /* Check if we should update lowest */
            size_t lowestLevel = E_LEVEL(mlru, mlru->lowest);
            if (lowestLevel > 0) {
                mlru->lowest = idx;
            }
        }
    }
}

/* Remove entry from its current position in the list */
DK_STATIC void removeFromList(multilru *mlru, uint64_t idx) {
    uint64_t prevIdx = E_PREV(mlru, idx);
    uint64_t nextIdx = E_NEXT(mlru, idx);
    size_t level = E_LEVEL(mlru, idx);

    /* Update prev's next pointer */
    if (prevIdx) {
        E_SET_NEXT(mlru, prevIdx, nextIdx);
    }

    /* Update next's prev pointer */
    if (nextIdx) {
        E_SET_PREV(mlru, nextIdx, prevIdx);
    }

    /* Update level tail if we removed it */
    if (mlru->levels[level].tail == idx) {
        /* New tail is the next entry toward head (since we insert at head,
         * next points toward newer entries, and tail is oldest).
         * When tail is removed, the second-oldest becomes new tail. */
        if (nextIdx && !E_HEAD(mlru, nextIdx)) {
            mlru->levels[level].tail = nextIdx;
        } else {
            mlru->levels[level].tail = 0;
        }
    }

    /* Update level count and weight */
    mlru->levels[level].count--;
    if (mlru->weights) {
        mlru->levels[level].weight -= mlru->weights[idx];
    }

    /* Update levelMask if this level is now empty */
    if (mlru->levels[level].count == 0) {
        mlru->levelMask &= ~(1ULL << level);
    }
}

/* Find and update the lowest entry pointer */
DK_STATIC void updateLowest(multilru *mlru) {
    mlru->lowest = 0;

    /* Use levelMask to find first populated level in O(1) */
    if (mlru->levelMask == 0) {
        return; /* No entries in any level */
    }

    /* Find lowest set bit = lowest populated level */
    size_t level = __builtin_ctzll(mlru->levelMask);
    if (level < mlru->maxLevels) {
        mlru->lowest = mlru->levels[level].tail;
    }
}

/* ====================================================================
 * Initialization
 * ==================================================================== */

DK_STATIC void initLevels(multilru *mlru) {
    /* Allocate level metadata */
    /* @own: container; free=multilruFree */
    mlru->levels = zcalloc(mlru->maxLevels, sizeof(multilruLevel));

    /* Reserve slots 1 through maxLevels for level head markers */
    for (size_t i = 0; i < mlru->maxLevels; i++) {
        uint64_t headIdx = i + 1;

        /* Remove from free list if present */
        /* (This is a simplified approach - head markers are set during grow) */

        /* Initialize head marker */
        E_SET_POPULATED(mlru, headIdx, 1);
        E_SET_HEAD(mlru, headIdx, 1);
        E_SET_LEVEL(mlru, headIdx, i);

        /* Chain head markers together */
        if (i > 0) {
            E_SET_PREV(mlru, headIdx, i);
            E_SET_NEXT(mlru, i, headIdx);
        }

        mlru->levels[i].head = headIdx;
        mlru->levels[i].tail = 0;
        mlru->levels[i].count = 0;
        mlru->levels[i].weight = 0;
    }
}

/* ====================================================================
 * Creation
 * ==================================================================== */

multilru *multilruNew(void) {
    /* @own: caller; free=multilruFree */
    return multilruNewWithLevelsCapacity(7, 0);
}

multilru *multilruNewWithLevels(size_t maxLevels) {
    /* @own: caller; free=multilruFree */
    return multilruNewWithLevelsCapacity(maxLevels, 0);
}

multilru *multilruNewWithLevelsCapacity(size_t maxLevels,
                                        size_t startCapacity) {
    multilruConfig config = {
        .maxLevels = maxLevels,
        .startCapacity = startCapacity,
        .policy = MLRU_POLICY_COUNT,
        .evictStrategy = MLRU_EVICT_LRU,
        .maxCount = 0,
        .maxWeight = 0,
        .enableWeights = false,
    };
    /* @own: caller; free=multilruFree */
    return multilruNewWithConfig(&config);
}

multilru *multilruNewWithConfig(const multilruConfig *config) {
    /* @own: caller; free=multilruFree */
    multilru *mlru = zcalloc(1, sizeof(*mlru));

    mlru->maxLevels = config->maxLevels > 0 ? config->maxLevels : 7;
    if (mlru->maxLevels > 64) {
        mlru->maxLevels = 64;
    }

    mlru->policy = config->policy;
    mlru->evictStrategy = config->evictStrategy;
    mlru->maxCount = config->maxCount;
    mlru->maxWeight = config->maxWeight;
    mlru->autoEvict = true; /* Enable auto-eviction by default */

    /* Determine initial capacity and entry width */
    size_t initialCapacity = config->startCapacity > 0
                                 ? config->startCapacity + mlru->maxLevels + 1
                                 : 256;

    mlru->entryWidth = selectWidth(initialCapacity);
    mlru->targetCapacity = initialCapacity;

    /* Enable weight tracking if requested */
    if (config->enableWeights || config->policy == MLRU_POLICY_SIZE ||
        config->policy == MLRU_POLICY_HYBRID ||
        config->evictStrategy == MLRU_EVICT_SIZE_WEIGHTED ||
        config->evictStrategy == MLRU_EVICT_SIZE_LRU) {
        /* Will be allocated during grow() */
        // cppcheck-suppress intToPointerCast - sentinel value marks weights for
        // deferred allocation
        mlru->weights = (uint64_t *)1; /* Non-NULL marker */
    }

    /* Initial allocation */
    grow(mlru);

    /* Fix weights pointer after grow */
    // cppcheck-suppress intToPointerCast - checking for deferred allocation
    // sentinel
    if (mlru->weights == (uint64_t *)1) {
        /* @own: container; free=multilruFree */
        mlru->weights = zcalloc(mlru->capacity, sizeof(uint64_t));
    }

    /* Initialize level head markers */
    initLevels(mlru);

    /* Set nextFresh to first slot after level head markers.
     * Indices 0 is reserved (invalid), 1..maxLevels are head markers. */
    mlru->nextFresh = mlru->maxLevels + 1;

    return mlru;
}

void multilruFree(multilru *mlru) {
    if (!mlru) {
        return;
    }

    zfree(mlru->entries);
    zfree(mlru->weights);
    zfree(mlru->levels);
    zfree(mlru);
}

/* ====================================================================
 * Core Operations
 * ==================================================================== */

/* Check if we need to evict based on current policy */
DK_STATIC bool needsEviction(const multilru *mlru) {
    switch (mlru->policy) {
    case MLRU_POLICY_COUNT:
        return mlru->maxCount > 0 && mlru->count > mlru->maxCount;
    case MLRU_POLICY_SIZE:
        return mlru->maxWeight > 0 && mlru->totalWeight > mlru->maxWeight;
    case MLRU_POLICY_HYBRID:
        return (mlru->maxCount > 0 && mlru->count > mlru->maxCount) ||
               (mlru->maxWeight > 0 && mlru->totalWeight > mlru->maxWeight);
    default:
        return false;
    }
}

/* Auto-evict entries until within policy limits (if autoEvict enabled) */
DK_STATIC void enforcePolicy(multilru *mlru) {
    if (!mlru->autoEvict) {
        return; /* Manual eviction mode - caller handles eviction */
    }

    while (needsEviction(mlru) && mlru->count > 0) {
        multilruPtr ptr;
        if (!multilruRemoveMinimum(mlru, &ptr)) {
            break;
        }
        /* Continue until a true eviction or limits satisfied */
    }
}

multilruPtr multilruInsert(multilru *mlru) {
    return multilruInsertWeighted(mlru, 0);
}

multilruPtr multilruInsertWeighted(multilru *mlru, uint64_t weight) {
    /* Get a slot: prefer recycled holes, then fresh sequential allocation */
    uint64_t idx = freeListPop(mlru);
    if (idx == 0) {
        /* No recycled slots - use next fresh index */
        if (mlru->nextFresh >= mlru->capacity) {
            /* Need more capacity */
            if (!grow(mlru)) {
                return 0; /* Can't allocate */
            }
        }
        idx = mlru->nextFresh++;
    }

    /* Set weight if tracking enabled */
    if (mlru->weights) {
        mlru->weights[idx] = weight;
        mlru->totalWeight += weight;
    }

    /* Insert at level 0 head */
    insertAtLevelHead(mlru, idx, 0);
    mlru->count++;
    mlru->statInserts++;

    /* Auto-evict if over policy limits */
    enforcePolicy(mlru);

    /* The policy pass holds no pin on the fresh entry: an insert
     * heavier than the remaining budget can cascade demotions until
     * the fresh entry itself is the eviction victim. Returning its idx
     * then would hand the caller a freed handle (and the next insert
     * could recycle it to a second owner). Report "didn't fit". */
    if (!E_POPULATED(mlru, idx)) {
        return 0;
    }

    return idx;
}

void multilruIncrease(multilru *mlru, multilruPtr ptr) {
    uint64_t idx = ptr;

    /* Bounds check */
    if (idx == 0 || idx >= mlru->capacity) {
        return;
    }

    if (!E_POPULATED(mlru, idx) || E_HEAD(mlru, idx)) {
        return; /* Invalid entry */
    }

    size_t currentLevel = E_LEVEL(mlru, idx);
    size_t targetLevel = currentLevel + 1;
    if (targetLevel >= mlru->maxLevels) {
        targetLevel = mlru->maxLevels - 1;
    }

    /* Count promotion if actually moving up */
    if (targetLevel > currentLevel) {
        mlru->statPromotions++;
    }

    /* Update lowest if we're moving it */
    bool wasLowest = (idx == mlru->lowest);

    /* Save weight before removal */
    uint64_t entryWeight = mlru->weights ? mlru->weights[idx] : 0;

    /* Remove from current position */
    removeFromList(mlru, idx);

    /* If we were lowest and there are other entries, find new lowest */
    if (wasLowest && mlru->count > 1) {
        updateLowest(mlru);
    }

    /* Restore weight (removeFromList decremented it) */
    if (mlru->weights) {
        mlru->weights[idx] = entryWeight;
    }

    /* Insert at head of target level */
    insertAtLevelHead(mlru, idx, targetLevel);

    /* Fix lowest if we're still the only entry or now at lower level */
    if (mlru->count == 1 || (wasLowest && mlru->lowest == 0)) {
        mlru->lowest = idx;
    }
}

void multilruUpdateWeight(multilru *mlru, multilruPtr ptr, uint64_t newWeight) {
    if (!mlru->weights) {
        return;
    }

    uint64_t idx = ptr;

    /* Bounds check */
    if (idx == 0 || idx >= mlru->capacity) {
        return;
    }

    if (!E_POPULATED(mlru, idx) || E_HEAD(mlru, idx)) {
        return;
    }

    uint64_t oldWeight = mlru->weights[idx];
    size_t level = E_LEVEL(mlru, idx);

    mlru->weights[idx] = newWeight;
    mlru->totalWeight = mlru->totalWeight - oldWeight + newWeight;
    mlru->levels[level].weight =
        mlru->levels[level].weight - oldWeight + newWeight;
}

void multilruDecrease(multilru *mlru, multilruPtr ptr) {
    const uint64_t idx = ptr;
    if (idx == 0 || idx >= mlru->capacity || !E_POPULATED(mlru, idx) ||
        E_HEAD(mlru, idx)) {
        return; /* Invalid entry */
    }

    const size_t level = E_LEVEL(mlru, idx);
    if (level == 0) {
        return;
    }

    /* Remove from the current level (which subtracts the weight), then
     * restore the weight for the re-insertion at the head of level - 1
     * (second chance) */
    const uint64_t entryWeight = mlru->weights ? mlru->weights[idx] : 0;
    removeFromList(mlru, idx);
    if (mlru->weights) {
        mlru->weights[idx] = entryWeight;
    }
    insertAtLevelHead(mlru, idx, level - 1);
    updateLowest(mlru);
    mlru->statDemotions++;
}

multilruPtr multilruPeekMinimum(const multilru *mlru) {
    return mlru->count ? mlru->lowest : 0;
}

multilruPtr multilruPeekNextHandle(const multilru *mlru) {
    return mlru->freeHead ? mlru->freeHead : mlru->nextFresh;
}

size_t multilruFreeHandles(const multilru *mlru, multilruPtr out[], size_t n) {
    size_t got = 0;
    for (uint64_t idx = mlru->freeHead; idx && got < n;
         idx = E_NEXT(mlru, idx)) {
        out[got++] = idx;
    }
    return got;
}

bool multilruRemoveMinimum(multilru *mlru, multilruPtr *out) {
    if (mlru->count == 0 || mlru->lowest == 0) {
        return false;
    }

    uint64_t idx = mlru->lowest;
    size_t level = E_LEVEL(mlru, idx);

    if (out) {
        *out = idx;
    }

    /* S4LRU Demotion: if level > 0, demote instead of evict.  The demoted
     * entry is returned; the caller decides whether to continue. */
    if (level > 0) {
        multilruDecrease(mlru, idx);
        return true;
    }

    /* True eviction from level 0 */
    removeFromList(mlru, idx);

    /* Update weight tracking */
    if (mlru->weights) {
        mlru->totalWeight -= mlru->weights[idx];
        mlru->weights[idx] = 0;
    }

    /* Update lowest */
    if (mlru->count > 1) {
        updateLowest(mlru);
    } else {
        mlru->lowest = 0;
    }

    /* Notify callback BEFORE freeing (so caller can clean up external data) */
    if (mlru->evictCallback) {
        mlru->evictCallback(idx, mlru->evictCallbackData);
    }

    mlru->statEvictions++;

    /* Add to free list */
    freeListPush(mlru, idx);
    mlru->count--;

    return true;
}

void multilruDelete(multilru *mlru, multilruPtr ptr) {
    uint64_t idx = ptr;

    /* Bounds check - ignore invalid indices */
    if (idx == 0 || idx >= mlru->capacity) {
        return;
    }

    if (!E_POPULATED(mlru, idx) || E_HEAD(mlru, idx)) {
        return;
    }

    bool wasLowest = (idx == mlru->lowest);

    /* Remove from list */
    removeFromList(mlru, idx);

    /* Update weight tracking */
    if (mlru->weights) {
        mlru->totalWeight -= mlru->weights[idx];
        mlru->weights[idx] = 0;
    }

    /* Update lowest if needed */
    if (wasLowest) {
        if (mlru->count > 1) {
            updateLowest(mlru);
        } else {
            mlru->lowest = 0;
        }
    }

    /* Add to free list */
    freeListPush(mlru, idx);
    mlru->count--;
    mlru->statDeletes++;
}

/* ====================================================================
 * Eviction Operations
 * ==================================================================== */

size_t multilruEvictN(multilru *mlru, multilruPtr out[], size_t n) {
    size_t evicted = 0;

    while (evicted < n && mlru->count > 0) {
        multilruPtr ptr;
        if (!multilruRemoveMinimum(mlru, &ptr)) {
            break;
        }

        /* Check if it was a true eviction (from level 0) or demotion */
        if (!E_POPULATED(mlru, ptr)) {
            /* Entry was freed - true eviction */
            if (out) {
                out[evicted] = ptr;
            }
            evicted++;
        }
        /* If still populated, it was demoted - continue until true eviction */
    }

    return evicted;
}

size_t multilruEvictToSize(multilru *mlru, uint64_t targetWeight,
                           multilruPtr out[], size_t maxN) {
    if (!mlru->weights) {
        return 0;
    }

    size_t evicted = 0;

    while (evicted < maxN && mlru->totalWeight > targetWeight &&
           mlru->count > 0) {
        multilruPtr ptr;
        if (!multilruRemoveMinimum(mlru, &ptr)) {
            break;
        }

        if (!E_POPULATED(mlru, ptr)) {
            if (out) {
                out[evicted] = ptr;
            }
            evicted++;
        }
    }

    return evicted;
}

/* ====================================================================
 * Queries
 * ==================================================================== */

size_t multilruCount(const multilru *mlru) {
    return mlru->count;
}

size_t multilruBytes(const multilru *mlru) {
    size_t bytes = sizeof(*mlru);
    bytes += (size_t)mlru->capacity * mlru->entryWidth;
    bytes += mlru->maxLevels * sizeof(multilruLevel);
    if (mlru->weights) {
        bytes += (size_t)mlru->capacity * sizeof(uint64_t);
    }
    return bytes;
}

uint64_t multilruTotalWeight(const multilru *mlru) {
    return mlru->totalWeight;
}

size_t multilruLevelCount(const multilru *mlru, size_t level) {
    if (level >= mlru->maxLevels) {
        return 0;
    }
    return mlru->levels[level].count;
}

uint64_t multilruLevelWeight(const multilru *mlru, size_t level) {
    if (level >= mlru->maxLevels) {
        return 0;
    }
    return mlru->levels[level].weight;
}

uint64_t multilruGetWeight(const multilru *mlru, multilruPtr ptr) {
    if (!mlru->weights || ptr == 0 || ptr >= mlru->capacity) {
        return 0;
    }
    return mlru->weights[ptr];
}

size_t multilruGetLevel(const multilru *mlru, multilruPtr ptr) {
    if (ptr == 0 || ptr >= mlru->capacity) {
        return 0;
    }
    return E_LEVEL(mlru, ptr);
}

bool multilruIsPopulated(const multilru *mlru, multilruPtr ptr) {
    if (ptr == 0 || ptr >= mlru->capacity) {
        return false;
    }
    return E_POPULATED(mlru, ptr) && !E_HEAD(mlru, ptr);
}

void multilruGetStats(const multilru *mlru, multilruStats *stats) {
    /* Current state */
    stats->count = mlru->count;
    stats->capacity = mlru->capacity;
    stats->totalWeight = mlru->totalWeight;
    stats->bytesUsed = multilruBytes(mlru);

    /* Slot allocation state */
    stats->nextFresh = mlru->nextFresh;
    stats->freeCount = mlru->freeCount;

    /* Lifetime counters */
    stats->inserts = mlru->statInserts;
    stats->evictions = mlru->statEvictions;
    stats->demotions = mlru->statDemotions;
    stats->promotions = mlru->statPromotions;
    stats->deletes = mlru->statDeletes;

    /* Configuration */
    stats->maxCount = mlru->maxCount;
    stats->maxWeight = mlru->maxWeight;
    stats->maxLevels = mlru->maxLevels;
    stats->entryWidth = mlru->entryWidth;
    stats->autoEvict = mlru->autoEvict;
}

/* ====================================================================
 * Bulk Queries
 * ==================================================================== */

size_t multilruCollectLowest(const multilru *mlru, multilruPtr out[],
                             size_t n) {
    size_t found = 0;

    /* Scan levels from 0 upward */
    for (size_t level = 0; level < mlru->maxLevels && found < n; level++) {
        uint64_t tail = mlru->levels[level].tail;
        if (tail == 0) {
            continue;
        }

        /* Walk forward from tail (oldest) toward head (newest) at this level */
        uint64_t current = tail;
        while (current && found < n) {
            if (E_POPULATED(mlru, current) && !E_HEAD(mlru, current)) {
                out[found++] = current;
            }

            uint64_t next = E_NEXT(mlru, current);
            if (next == 0 || E_HEAD(mlru, next)) {
                break;
            }
            current = next;
        }
    }

    return found;
}

size_t multilruCollectHighest(const multilru *mlru, multilruPtr out[],
                              size_t n) {
    size_t found = 0;

    /* Scan levels from highest downward */
    for (ssize_t level = mlru->maxLevels - 1; level >= 0 && found < n;
         level--) {
        uint64_t headIdx = mlru->levels[level].head;
        uint64_t first = E_PREV(mlru, headIdx);

        if (first == 0 || E_HEAD(mlru, first)) {
            continue;
        }

        /* Walk backward from head (newest to oldest at this level) */
        uint64_t current = first;
        while (current && found < n) {
            if (E_POPULATED(mlru, current) && !E_HEAD(mlru, current)) {
                out[found++] = current;
            }

            uint64_t prev = E_PREV(mlru, current);
            if (prev == 0 || E_HEAD(mlru, prev)) {
                break;
            }
            current = prev;
        }
    }

    return found;
}

/* Slots past the last entry found are set to 0, which no live entry uses */
void multilruGetNLowest(const multilru *mlru, multilruPtr out[], size_t n) {
    const size_t found = multilruCollectLowest(mlru, out, n);
    for (size_t i = found; i < n; i++) {
        out[i] = 0;
    }
}

void multilruGetNHighest(const multilru *mlru, multilruPtr out[], size_t n) {
    const size_t found = multilruCollectHighest(mlru, out, n);
    for (size_t i = found; i < n; i++) {
        out[i] = 0;
    }
}

/* ====================================================================
 * Runtime Configuration
 * ==================================================================== */

void multilruSetPolicy(multilru *mlru, multilruPolicy policy) {
    mlru->policy = policy;
}

void multilruSetEvictStrategy(multilru *mlru, multilruEvictStrategy strategy) {
    mlru->evictStrategy = strategy;
}

multilruPolicy multilruGetPolicy(const multilru *mlru) {
    return mlru->policy;
}

multilruEvictStrategy multilruGetEvictStrategy(const multilru *mlru) {
    return mlru->evictStrategy;
}

void multilruSetMaxCount(multilru *mlru, uint64_t maxCount) {
    mlru->maxCount = maxCount;
}

uint64_t multilruGetMaxCount(const multilru *mlru) {
    return mlru->maxCount;
}

void multilruSetMaxWeight(multilru *mlru, uint64_t maxWeight) {
    mlru->maxWeight = maxWeight;
}

uint64_t multilruGetMaxWeight(const multilru *mlru) {
    return mlru->maxWeight;
}

/* ====================================================================
 * Eviction Control
 * ==================================================================== */

void multilruSetAutoEvict(multilru *mlru, bool autoEvict) {
    mlru->autoEvict = autoEvict;
}

bool multilruGetAutoEvict(const multilru *mlru) {
    return mlru->autoEvict;
}

void multilruSetEvictCallback(multilru *mlru,
                              void (*callback)(size_t evictedPtr,
                                               void *userData),
                              void *userData) {
    mlru->evictCallback = callback;
    mlru->evictCallbackData = userData;
}

bool multilruNeedsEviction(const multilru *mlru) {
    switch (mlru->policy) {
    case MLRU_POLICY_COUNT:
        return mlru->maxCount > 0 && mlru->count > mlru->maxCount;
    case MLRU_POLICY_SIZE:
        return mlru->maxWeight > 0 && mlru->totalWeight > mlru->maxWeight;
    case MLRU_POLICY_HYBRID:
        return (mlru->maxCount > 0 && mlru->count > mlru->maxCount) ||
               (mlru->maxWeight > 0 && mlru->totalWeight > mlru->maxWeight);
    default:
        return false;
    }
}

/* ====================================================================
 * Introspection
 * ==================================================================== */

size_t multilruMaxLevels(const multilru *mlru) {
    return mlru->maxLevels;
}

multilruEntryWidth multilruGetEntryWidth(const multilru *mlru) {
    return mlru->entryWidth;
}

size_t multilruCapacity(const multilru *mlru) {
    return mlru->capacity;
}

bool multilruHasWeights(const multilru *mlru) {
    return mlru && mlru->weights != NULL;
}

/* ====================================================================
 * Testing
 * ==================================================================== */
ssize_t multilruTraverseSize(const multilru *mlru) {
    size_t count = 0;

    for (size_t level = 0; level < mlru->maxLevels; level++) {
        count += mlru->levels[level].count;
    }

    return count;
}

#ifdef DATAKIT_TEST
#include "ctest.h"
#include <stdio.h>
void multilruRepr(const multilru *mlru) {
    printf("{count {used %" PRIu64 "} {capacity %" PRIu64 "}} {lowest %" PRIu64
           "} {bytes {allocated %zu}}"
           " {width %u}\n",
           mlru->count, mlru->capacity, mlru->lowest, multilruBytes(mlru),
           mlru->entryWidth);

    /* Print level pointers */
    printf("{%" PRIu64 "} ", mlru->lowest);
    for (size_t i = 0; i < mlru->maxLevels; i++) {
        printf("[%zu] -> h%" PRIu64 " t%" PRIu64 " c%" PRIu64 "; ", i,
               mlru->levels[i].head, mlru->levels[i].tail,
               mlru->levels[i].count);
    }
    printf("\n");

    /* Print linked list */
    printf("(");
    uint64_t current = mlru->lowest;
    size_t safety = 0;
    while (current && safety++ < mlru->capacity + mlru->maxLevels + 10) {
        if (E_HEAD(mlru, current)) {
            printf("[H%zu] -> ", (size_t)E_LEVEL(mlru, current));
        } else {
            printf("(%" PRIu64 ") -> ", current);
        }
        current = E_NEXT(mlru, current);
    }
    printf("{count %zu}\n\n", (size_t)multilruTraverseSize(mlru));
}

/* ====================================================================
 * TEST HELPER FUNCTIONS (defined outside test function for C compliance)
 * ==================================================================== */

/* Fuzz operation types */
typedef enum {
    FUZZ_INSERT,
    FUZZ_INSERT_WEIGHTED,
    FUZZ_INCREASE,
    FUZZ_REMOVE_MIN,
    FUZZ_DELETE,
    FUZZ_UPDATE_WEIGHT,
    FUZZ_EVICT_N,
    FUZZ_EVICT_TO_SIZE,
    FUZZ_SET_POLICY,
    FUZZ_SET_MAX_COUNT,
    FUZZ_SET_MAX_WEIGHT,
    FUZZ_OP_COUNT
} FuzzOp;

/* Helper: Verify all invariants hold for the multilru */
static bool verifyInvariants(multilru *mlru, const char *context) {
    bool ok = true;

    /* Invariant 1: Level counts sum to total count */
    size_t levelSum = 0;
    for (size_t i = 0; i < mlru->maxLevels; i++) {
        levelSum += multilruLevelCount(mlru, i);
    }
    if (levelSum != multilruCount(mlru)) {
        printf("INVARIANT FAILED [%s]: level sum %zu != count %zu\n", context,
               levelSum, multilruCount(mlru));
        ok = false;
    }

    /* Invariant 2: Level weights sum to total weight (if weights enabled) */
    if (mlru->weights) {
        uint64_t weightSum = 0;
        for (size_t i = 0; i < mlru->maxLevels; i++) {
            weightSum += multilruLevelWeight(mlru, i);
        }
        if (weightSum != multilruTotalWeight(mlru)) {
            printf("INVARIANT FAILED [%s]: weight sum %" PRIu64
                   " != total %" PRIu64 "\n",
                   context, weightSum, multilruTotalWeight(mlru));
            ok = false;
        }
    }

    /* Invariant 3: If count > 0, lowest must be valid */
    if (mlru->count > 0 && mlru->lowest == 0) {
        printf("INVARIANT FAILED [%s]: count %" PRIu64 " but lowest is 0\n",
               context, mlru->count);
        ok = false;
    }

    /* Invariant 4: nextFresh must be within capacity */
    if (mlru->nextFresh > mlru->capacity) {
        printf("INVARIANT FAILED [%s]: nextFresh %" PRIu64
               " > capacity %" PRIu64 "\n",
               context, mlru->nextFresh, mlru->capacity);
        ok = false;
    }

    /* Invariant 5: count + freeCount = nextFresh - (maxLevels + 1)
     * All slots from maxLevels+1 to nextFresh-1 are either active or recycled
     */
    uint64_t usedSlots = mlru->nextFresh - mlru->maxLevels - 1;
    if (mlru->count + mlru->freeCount != usedSlots) {
        printf("INVARIANT FAILED [%s]: count %" PRIu64 " + free %" PRIu64
               " != used slots "
               "%" PRIu64 "\n",
               context, mlru->count, mlru->freeCount, usedSlots);
        ok = false;
    }

    return ok;
}

/* Zipfian distribution generator for access pattern testing */
static size_t zipfian(uint64_t *seed, size_t n, double skew) {
    double sum = 0;
    for (size_t i = 1; i <= n; i++) {
        sum += 1.0 / pow((double)i, skew);
    }

    double rnd = (double)(xoroshiro128plus(seed) % 1'000'000) / 1'000'000.0;
    double cumulative = 0;

    for (size_t i = 1; i <= n; i++) {
        cumulative += (1.0 / pow((double)i, skew)) / sum;
        if (rnd <= cumulative) {
            return i - 1;
        }
    }
    return n - 1;
}

/* Static callback for eviction workflow tests */
static size_t testEvictedPtrs[100];
static size_t testEvictCount;

static void testEvictionCallback(size_t ptr, void *) {
    if (testEvictCount < 100) {
        testEvictedPtrs[testEvictCount++] = ptr;
    }
}

/* A fixed mix of inserts, promotions, deletes and evictions; writes the
 * handle of every RemoveMinimum and then every surviving entry's level to
 * trace[], returning how many values it wrote.  Identical traces from two
 * caches mean the entry encoding round-trips at both widths. */
static constexpr int MLRU_WIDTH_TRACE_MAX = 8192;
static size_t mlruWidthWorkload_(multilru *mlru, uint64_t *trace) {
    constexpr int inserts = 1500;
    multilruPtr ptrs[inserts];
    size_t n = 0;
    uint64_t seed[2] = {0x9E3779B97F4A7C15ULL, 0xD1B54A32D192ED03ULL};

    for (size_t i = 0; i < inserts; i++) {
        ptrs[i] = multilruInsert(mlru);
    }

    for (size_t i = 0; i < 4 * inserts; i++) {
        multilruIncrease(mlru, ptrs[xoroshiro128plus(seed) % inserts]);
    }

    for (size_t i = 0; i < inserts; i += 7) {
        multilruDelete(mlru, ptrs[i]);
        ptrs[i] = 0;
    }

    for (size_t i = 0; i < 400 && n < MLRU_WIDTH_TRACE_MAX; i++) {
        multilruPtr out = 0;
        if (!multilruRemoveMinimum(mlru, &out)) {
            break;
        }
        trace[n++] = out;
    }

    for (size_t i = 0; i < inserts && n < MLRU_WIDTH_TRACE_MAX; i++) {
        if (ptrs[i] && multilruIsPopulated(mlru, ptrs[i])) {
            trace[n++] =
                ((uint64_t)ptrs[i] << 8) | multilruGetLevel(mlru, ptrs[i]);
        }
    }

    return n;
}

/* Each width's setters write exactly their entry, and the migration reader
 * gets back every field at its extremes; returns the mismatches */
#define MLRU_LAYOUT_CHECK(W, BITS)                                             \
    do {                                                                       \
        const uint64_t top = (UINT64_C(1) << (BITS)) - 1;                      \
        const uint64_t idxs[] = {0, 1, top / 3, top};                          \
        for (size_t pi = 0; pi < 4; pi++) {                                    \
            for (size_t ni = 0; ni < 4; ni++) {                                \
                for (uint8_t lv = 0; lv < 64; lv += 21) {                      \
                    for (int fl = 0; fl < 4; fl++) {                           \
                        uint8_t buf[3 * (W)];                                  \
                        memset(buf, 0xa5, sizeof(buf));                        \
                        void *e = buf + (W);                                   \
                        entry##W##SetPrev(e, idxs[pi]);                        \
                        entry##W##SetNext(e, idxs[ni]);                        \
                        entry##W##SetLevel(e, lv);                             \
                        entry##W##SetPopulated(e, fl & 1);                     \
                        entry##W##SetHead(e, fl & 2);                          \
                        uint64_t p = 0, nx = 0;                                \
                        uint8_t l = 0;                                         \
                        bool pop = false, head = false;                        \
                        entryReadByWidth(buf, (W), 1, &p, &nx, &l, &pop,       \
                                         &head);                               \
                        bool outside = false;                                  \
                        for (size_t k = 0; k < (W); k++) {                     \
                            outside |=                                         \
                                buf[k] != 0xa5 || buf[2 * (W) + k] != 0xa5;    \
                        }                                                      \
                        if (p != idxs[pi] || nx != idxs[ni] || l != lv ||      \
                            pop != (bool)(fl & 1) || head != (bool)(fl & 2) || \
                            outside) {                                         \
                            bad++;                                             \
                        }                                                      \
                    }                                                          \
                }                                                              \
            }                                                                  \
        }                                                                      \
    } while (0)

static int multilruTestEntryLayouts(void) {
    int bad = 0;
    MLRU_LAYOUT_CHECK(5, 16);
    MLRU_LAYOUT_CHECK(6, 20);
    MLRU_LAYOUT_CHECK(7, 24);
    MLRU_LAYOUT_CHECK(8, 28);
    MLRU_LAYOUT_CHECK(9, 32);
    MLRU_LAYOUT_CHECK(10, 36);
    MLRU_LAYOUT_CHECK(11, 40);
    MLRU_LAYOUT_CHECK(12, 44);
    MLRU_LAYOUT_CHECK(16, 60);
    return bad;
}

int multilruTest(int, char *[]) {
    int err = 0;

    TEST("every entry width round-trips its fields through migration") {
        const int bad = multilruTestEntryLayouts();
        if (bad) {
            ERR("%d entry layout mismatches", bad);
        }
    }

    TEST("growing past 64K entries widens entries and keeps levels") {
        /* 70000 entries cross the 5-byte limit; levels set before the
         * upgrade survive it, and removal by minimum drains every entry
         * once in non-decreasing level order */
        multilru *mlru = multilruNewWithLevels(7);
        constexpr int N = 70000;
        cleanerName(zfree, Ptrs) multilruPtr *ptrs =
            zcalloc(N, sizeof(multilruPtr));
        cleanerName(zfree, Bytes) uint8_t *want = zcalloc(N, 1);
        for (size_t i = 0; i < N; i++) {
            ptrs[i] = multilruInsert(mlru);
            if (i < 60000 && i % 7 == 0) {
                const uint8_t ups = (uint8_t)(i % 9);
                for (uint8_t u = 0; u < ups; u++) {
                    multilruIncrease(mlru, ptrs[i]);
                }
                want[i] = ups > 6 ? 6 : ups;
            }
        }
        size_t wrong = 0;
        for (size_t i = 0; i < N; i++) {
            wrong += multilruGetLevel(mlru, ptrs[i]) != want[i];
        }
        if (mlru->entryWidth != 6 || multilruCount(mlru) != N || wrong) {
            ERR("upgrade: width %u, count %zu, %zu levels wrong",
                mlru->entryWidth, multilruCount(mlru), wrong);
        }
        /* Each handle's insert slot, to look up the level it should have */
        multilruPtr maxHandle = 0;
        for (size_t i = 0; i < N; i++) {
            maxHandle = ptrs[i] > maxHandle ? ptrs[i] : maxHandle;
        }
        cleanerName(zfree, U64s) uint64_t *slotOf =
            zcalloc(maxHandle + 1, sizeof(uint64_t));
        for (size_t i = 0; i < N; i++) {
            slotOf[ptrs[i]] = i + 1;
        }
        /* RemoveMinimum demotes an entry above level 0 (S4LRU) and evicts
         * one at level 0, so each handle comes back its level + 1 times,
         * the last time unpopulated */
        cleanerName(zfree, Bytes) uint8_t *returns = zcalloc(N, 1);
        size_t evicted = 0;
        size_t strays = 0;
        multilruPtr out;
        while (multilruRemoveMinimum(mlru, &out)) {
            const uint64_t slot = out <= maxHandle ? slotOf[out] : 0;
            if (!slot) {
                strays++;
                continue;
            }
            returns[slot - 1]++;
            if (!multilruIsPopulated(mlru, out)) {
                evicted++;
                slotOf[out] = 0; /* evicted: must not come back */
            }
        }
        size_t miscounted = 0;
        for (size_t i = 0; i < N; i++) {
            miscounted += returns[i] != want[i] + 1;
        }
        if (evicted != N || strays || miscounted) {
            ERR("upgrade: %zu evicted of %d, %zu strays, %zu entries returned "
                "the wrong number of times",
                evicted, (int)N, strays, miscounted);
        }
        multilruFree(mlru);
    }

    /* ================================================================
     * BASIC TESTS
     * ================================================================ */

    TEST("width tier definitions") {
        /* Verify WIDTH_TIERS array is correctly defined */
        assert(NUM_WIDTH_TIERS == 9);
        assert(WIDTH_TIERS[0].width == 5);
        assert(WIDTH_TIERS[0].addressBits == 16);
        assert(WIDTH_TIERS[0].maxEntries == MAX_ENTRIES_W5);
        assert(WIDTH_TIERS[8].width == 16);
        assert(WIDTH_TIERS[8].addressBits == 60);
        assert(WIDTH_TIERS[8].maxEntries == MAX_ENTRIES_W16);
        printf("Width tiers: %zu tiers from %u to %u bytes\n", NUM_WIDTH_TIERS,
               WIDTH_TIERS[0].width, WIDTH_TIERS[NUM_WIDTH_TIERS - 1].width);
    }

    TEST("width selection") {
        /* Test each tier boundary */
        assert(selectWidth(100) == MLRU_WIDTH_5); /* Well within W5 */
        assert(selectWidth(MAX_ENTRIES_W5) == MLRU_WIDTH_5);
        assert(selectWidth(MAX_ENTRIES_W5 + 1) == MLRU_WIDTH_6);
        assert(selectWidth(MAX_ENTRIES_W6) == MLRU_WIDTH_6);
        assert(selectWidth(MAX_ENTRIES_W6 + 1) == MLRU_WIDTH_7);
        assert(selectWidth(MAX_ENTRIES_W7) == MLRU_WIDTH_7);
        assert(selectWidth(MAX_ENTRIES_W7 + 1) == MLRU_WIDTH_8);
        assert(selectWidth(MAX_ENTRIES_W8) == MLRU_WIDTH_8);
        assert(selectWidth(MAX_ENTRIES_W8 + 1) == MLRU_WIDTH_9);
        assert(selectWidth(MAX_ENTRIES_W9) == MLRU_WIDTH_9);
        assert(selectWidth(MAX_ENTRIES_W9 + 1) == MLRU_WIDTH_10);
        assert(selectWidth(MAX_ENTRIES_W10) == MLRU_WIDTH_10);
        assert(selectWidth(MAX_ENTRIES_W10 + 1) == MLRU_WIDTH_11);
        assert(selectWidth(MAX_ENTRIES_W11) == MLRU_WIDTH_11);
        assert(selectWidth(MAX_ENTRIES_W11 + 1) == MLRU_WIDTH_12);
        assert(selectWidth(MAX_ENTRIES_W12) == MLRU_WIDTH_12);
        assert(selectWidth(MAX_ENTRIES_W12 + 1) == MLRU_WIDTH_16);
        assert(selectWidth(MAX_ENTRIES_W16) == MLRU_WIDTH_16);
        printf("Width selection (all 9 tiers): PASSED\n");
    }

    TEST("create empty") {
        cleaner(multilruFree) multilru *mlru = multilruNew();
        assert(mlru != NULL);
        assert(multilruCount(mlru) == 0);
        assert(mlru->entryWidth == MLRU_WIDTH_5);
        multilruRepr(mlru);
    }

    TEST("basic insert and remove") {
        cleaner(multilruFree) multilru *mlru = multilruNew();

        multilruPtr p1 = multilruInsert(mlru);
        assert(p1 != 0);
        assert(multilruCount(mlru) == 1);

        multilruPtr p2 = multilruInsert(mlru);
        assert(p2 != 0);
        assert(multilruCount(mlru) == 2);

        multilruPtr removed;
        bool ok = multilruRemoveMinimum(mlru, &removed);
        assert(ok);
        assert(multilruCount(mlru) == 1);

        printf("basic insert and remove: PASSED\n");
    }

    TEST("level promotion") {
        cleaner(multilruFree) multilru *mlru = multilruNew();

        multilruPtr p = multilruInsert(mlru);
        assert(multilruGetLevel(mlru, p) == 0);

        multilruIncrease(mlru, p);
        assert(multilruGetLevel(mlru, p) == 1);

        multilruIncrease(mlru, p);
        assert(multilruGetLevel(mlru, p) == 2);

        /* Promote to max level */
        for (int i = 0; i < 10; i++) {
            multilruIncrease(mlru, p);
        }
        assert(multilruGetLevel(mlru, p) == mlru->maxLevels - 1);

        printf("level promotion: PASSED\n");
    }

    TEST("S4LRU demotion") {
        cleaner(multilruFree) multilru *mlru = multilruNew();

        /* Insert and promote entry to level 3 */
        multilruPtr p = multilruInsert(mlru);
        multilruIncrease(mlru, p); /* L1 */
        multilruIncrease(mlru, p); /* L2 */
        multilruIncrease(mlru, p); /* L3 */
        assert(multilruGetLevel(mlru, p) == 3);

        /* RemoveMinimum should demote, not evict */
        multilruPtr removed;
        bool ok = multilruRemoveMinimum(mlru, &removed);
        assert(ok);
        assert(removed == p);
        assert(multilruIsPopulated(mlru, p));   /* Still populated! */
        assert(multilruGetLevel(mlru, p) == 2); /* Demoted to L2 */
        assert(multilruCount(mlru) == 1);       /* Count unchanged */

        /* Demote again */
        ok = multilruRemoveMinimum(mlru, &removed);
        assert(ok);
        assert(multilruGetLevel(mlru, p) == 1); /* L1 */

        /* Demote to L0 */
        ok = multilruRemoveMinimum(mlru, &removed);
        assert(ok);
        assert(multilruGetLevel(mlru, p) == 0); /* L0 */

        /* Now it should truly evict */
        ok = multilruRemoveMinimum(mlru, &removed);
        assert(ok);
        assert(!multilruIsPopulated(mlru, p)); /* Now freed */
        assert(multilruCount(mlru) == 0);

        printf("S4LRU demotion: PASSED\n");
    }

    TEST("per-level counts") {
        cleaner(multilruFree) multilru *mlru = multilruNew();

        /* Insert 10 entries */
        multilruPtr ptrs[10];
        for (int i = 0; i < 10; i++) {
            ptrs[i] = multilruInsert(mlru);
        }
        assert(multilruLevelCount(mlru, 0) == 10);

        /* Promote some to different levels */
        multilruIncrease(mlru, ptrs[0]); /* L1 */
        multilruIncrease(mlru, ptrs[1]); /* L1 */
        multilruIncrease(mlru, ptrs[1]); /* L2 */
        multilruIncrease(mlru, ptrs[2]); /* L1 */
        multilruIncrease(mlru, ptrs[2]); /* L2 */
        multilruIncrease(mlru, ptrs[2]); /* L3 */

        assert(multilruLevelCount(mlru, 0) == 7);
        assert(multilruLevelCount(mlru, 1) == 1);
        assert(multilruLevelCount(mlru, 2) == 1);
        assert(multilruLevelCount(mlru, 3) == 1);

        /* Verify total */
        size_t total = 0;
        for (size_t i = 0; i < mlru->maxLevels; i++) {
            total += multilruLevelCount(mlru, i);
        }
        assert(total == 10);

        printf("per-level counts: PASSED\n");
    }

    TEST("weight tracking") {
        multilruConfig config = {
            .maxLevels = 7,
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        multilruPtr p1 = multilruInsertWeighted(mlru, 1000);
        multilruPtr p2 = multilruInsertWeighted(mlru, 2000);
        multilruPtr p3 = multilruInsertWeighted(mlru, 500);

        assert(multilruTotalWeight(mlru) == 3500);
        assert(multilruGetWeight(mlru, p1) == 1000);
        assert(multilruGetWeight(mlru, p2) == 2000);
        assert(multilruGetWeight(mlru, p3) == 500);

        /* Update weight */
        multilruUpdateWeight(mlru, p2, 3000);
        assert(multilruTotalWeight(mlru) == 4500);
        assert(multilruGetWeight(mlru, p2) == 3000);

        /* Delete should update weight */
        multilruDelete(mlru, p1);
        assert(multilruTotalWeight(mlru) == 3500);

        printf("weight tracking: PASSED\n");
    }

    TEST("free list reuse") {
        cleaner(multilruFree) multilru *mlru = multilruNew();

        /* Insert many entries */
        multilruPtr ptrs[100];
        for (int i = 0; i < 100; i++) {
            ptrs[i] = multilruInsert(mlru);
        }

        /* Delete all */
        for (int i = 0; i < 100; i++) {
            multilruDelete(mlru, ptrs[i]);
        }
        assert(multilruCount(mlru) == 0);

        /* Reinsert - should reuse freed slots */
        for (int i = 0; i < 100; i++) {
            ptrs[i] = multilruInsert(mlru);
            assert(ptrs[i] != 0);
        }
        assert(multilruCount(mlru) == 100);

        printf("free list reuse: PASSED\n");
    }

    TEST("GetNLowest and GetNHighest") {
        cleaner(multilruFree) multilru *mlru = multilruNew();

        multilruPtr ptrs[20];
        for (int i = 0; i < 20; i++) {
            ptrs[i] = multilruInsert(mlru);
        }

        /* Promote some entries */
        for (int i = 10; i < 20; i++) {
            for (int j = 0; j < (i - 9); j++) {
                multilruIncrease(mlru, ptrs[i]);
            }
        }

        /* Get lowest 5 */
        multilruPtr lowest[5] = {};
        multilruGetNLowest(mlru, lowest, 5);
        for (int i = 0; i < 5; i++) {
            assert(lowest[i] != 0);
            assert(multilruGetLevel(mlru, lowest[i]) == 0);
        }

        /* Get highest 5 */
        multilruPtr highest[5] = {};
        multilruGetNHighest(mlru, highest, 5);
        for (int i = 0; i < 5; i++) {
            assert(highest[i] != 0);
            /* Should be from higher levels */
        }

        printf("GetNLowest and GetNHighest: PASSED\n");
    }

    TEST("stress test") {
        cleaner(multilruFree) multilru *mlru =
            multilruNewWithLevelsCapacity(7, 10000);

        /* Insert 10000 entries */
        for (int i = 0; i < 10000; i++) {
            multilruPtr p = multilruInsert(mlru);
            assert(p != 0);

            /* Randomly promote */
            int promotes = i % 7;
            for (int j = 0; j < promotes; j++) {
                multilruIncrease(mlru, p);
            }
        }
        assert(multilruCount(mlru) == 10000);

        /* Verify level count sum */
        size_t total = 0;
        for (size_t i = 0; i < mlru->maxLevels; i++) {
            total += multilruLevelCount(mlru, i);
        }
        assert(total == 10000);

        /* Remove 5000 */
        for (int i = 0; i < 5000; i++) {
            multilruPtr removed;
            bool ok = multilruRemoveMinimum(mlru, &removed);
            assert(ok);
        }

        /* Verify remaining */
        total = 0;
        for (size_t i = 0; i < mlru->maxLevels; i++) {
            total += multilruLevelCount(mlru, i);
        }
        assert(total == multilruCount(mlru));

        printf("stress test: PASSED\n");
    }

    TEST("policy enforcement - count") {
        multilruConfig config = {
            .maxLevels = 4,
            .startCapacity = 100,
            .policy = MLRU_POLICY_COUNT,
            .maxCount = 50,
            .enableWeights = false,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Insert 100 entries - should auto-evict to stay at 50 */
        for (int i = 0; i < 100; i++) {
            multilruPtr p = multilruInsert(mlru);
            assert(p != 0);
        }
        assert(multilruCount(mlru) <= 50);

        printf("policy enforcement - count: PASSED\n");
    }

    TEST("policy enforcement - weight") {
        multilruConfig config = {
            .maxLevels = 4,
            .startCapacity = 100,
            .policy = MLRU_POLICY_SIZE,
            .maxWeight = 500,
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Insert entries with weight 10 each - should evict to stay under 500
         */
        for (int i = 0; i < 100; i++) {
            multilruPtr p = multilruInsertWeighted(mlru, 10);
            assert(p != 0);
        }
        assert(multilruTotalWeight(mlru) <= 500);
        assert(multilruCount(mlru) <= 50);

        printf("policy enforcement - weight: PASSED\n");
    }

    TEST("video cache scenario") {
        /* Simulate cache with 12GB video and 100x 100MB videos = 22GB total
         * Cache limit: 15GB
         * Verify size-aware eviction */
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 200,
            .policy = MLRU_POLICY_SIZE,
            .maxWeight = 15ULL * 1024 * 1024 * 1024, /* 15GB */
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Add one 12GB video */
        multilruPtr bigVideo =
            multilruInsertWeighted(mlru, 12ULL * 1024 * 1024 * 1024);
        assert(bigVideo != 0);

        /* Add 100x 100MB videos */
        for (int i = 0; i < 100; i++) {
            multilruPtr p = multilruInsertWeighted(mlru, 100ULL * 1024 * 1024);
            assert(p != 0);
        }

        /* Verify total weight is under limit */
        assert(multilruTotalWeight(mlru) <= 15ULL * 1024 * 1024 * 1024);

        printf("video cache scenario: PASSED\n");
    }

    TEST("width upgrade 5 to 6") {
        /* Start with small capacity (5-byte width, max 64K entries) */
        cleaner(multilruFree) multilru *mlru =
            multilruNewWithLevelsCapacity(4, 100);
        assert(multilruGetEntryWidth(mlru) == MLRU_WIDTH_5);

        /* Insert 70000 entries (past 64K threshold) */
        const size_t targetCount = 70000;
        for (size_t i = 0; i < targetCount; i++) {
            multilruPtr p = multilruInsert(mlru);
            assert(p != 0);

            /* Promote some entries to test data integrity after upgrade */
            if (i % 7 == 0) {
                multilruIncrease(mlru, p);
            }
        }

        /* Verify width upgraded to 6 (20-bit addresses, up to 1M entries) */
        assert(multilruGetEntryWidth(mlru) == MLRU_WIDTH_6);
        assert(multilruCount(mlru) == targetCount);

        /* Verify level counts are consistent */
        size_t total = 0;
        for (size_t i = 0; i < mlru->maxLevels; i++) {
            total += multilruLevelCount(mlru, i);
        }
        assert(total == targetCount);

        /* Verify removals still work */
        for (size_t i = 0; i < 1000; i++) {
            multilruPtr removed;
            bool ok = multilruRemoveMinimum(mlru, &removed);
            assert(ok);
        }

        printf("width upgrade 5 to 6: PASSED\n");
    }

    TEST("maxEntriesForWidth") {
        /* Verify maxEntriesForWidth returns correct limits for all widths */
        assert(maxEntriesForWidth(MLRU_WIDTH_5) == MAX_ENTRIES_W5);
        assert(maxEntriesForWidth(MLRU_WIDTH_6) == MAX_ENTRIES_W6);
        assert(maxEntriesForWidth(MLRU_WIDTH_7) == MAX_ENTRIES_W7);
        assert(maxEntriesForWidth(MLRU_WIDTH_8) == MAX_ENTRIES_W8);
        assert(maxEntriesForWidth(MLRU_WIDTH_9) == MAX_ENTRIES_W9);
        assert(maxEntriesForWidth(MLRU_WIDTH_10) == MAX_ENTRIES_W10);
        assert(maxEntriesForWidth(MLRU_WIDTH_11) == MAX_ENTRIES_W11);
        assert(maxEntriesForWidth(MLRU_WIDTH_12) == MAX_ENTRIES_W12);
        assert(maxEntriesForWidth(MLRU_WIDTH_16) == MAX_ENTRIES_W16);
        printf("maxEntriesForWidth (all widths): PASSED\n");
    }

    TEST("bit packing round-trip for all widths") {
        /* Test that entry accessors correctly read/write at max values */
        uint8_t buf[16] = {};

        /* Test 16-byte entries (60-bit addresses) */
        uint64_t maxIdx60 = (1ULL << 60) - 1;
        entry16SetPrev(buf, maxIdx60);
        entry16SetNext(buf, maxIdx60);
        entry16SetLevel(buf, 63);
        entry16SetPopulated(buf, true);
        entry16SetHead(buf, true);
        assert(entry16GetPrev(buf) == maxIdx60);
        assert(entry16GetNext(buf) == maxIdx60);
        assert(entry16GetLevel(buf) == 63);
        assert(entry16GetPopulated(buf) == true);
        assert(entry16GetHead(buf) == true);

        /* Test 5-byte entries (16-bit addresses) */
        memset(buf, 0, sizeof(buf));
        uint64_t maxIdx16 = (1ULL << 16) - 1;
        entry5SetPrev(buf, maxIdx16);
        entry5SetNext(buf, maxIdx16);
        entry5SetLevel(buf, 63);
        entry5SetPopulated(buf, true);
        entry5SetHead(buf, true);
        assert(entry5GetPrev(buf) == maxIdx16);
        assert(entry5GetNext(buf) == maxIdx16);
        assert(entry5GetLevel(buf) == 63);
        assert(entry5GetPopulated(buf) == true);
        assert(entry5GetHead(buf) == true);

        /* Test 6-byte entries (20-bit addresses) with bit packing */
        memset(buf, 0, sizeof(buf));
        uint64_t maxIdx20 = (1ULL << 20) - 1;
        entry6SetPrev(buf, maxIdx20);
        entry6SetNext(buf, maxIdx20);
        entry6SetLevel(buf, 63);
        entry6SetPopulated(buf, true);
        entry6SetHead(buf, true);
        assert(entry6GetPrev(buf) == maxIdx20);
        assert(entry6GetNext(buf) == maxIdx20);
        assert(entry6GetLevel(buf) == 63);
        assert(entry6GetPopulated(buf) == true);
        assert(entry6GetHead(buf) == true);

        /* Test 10-byte entries (36-bit addresses) with bit packing */
        memset(buf, 0, sizeof(buf));
        uint64_t maxIdx36 = (1ULL << 36) - 1;
        entry10SetPrev(buf, maxIdx36);
        entry10SetNext(buf, maxIdx36);
        entry10SetLevel(buf, 63);
        entry10SetPopulated(buf, true);
        entry10SetHead(buf, true);
        assert(entry10GetPrev(buf) == maxIdx36);
        assert(entry10GetNext(buf) == maxIdx36);
        assert(entry10GetLevel(buf) == 63);
        assert(entry10GetPopulated(buf) == true);
        assert(entry10GetHead(buf) == true);

        printf("bit packing round-trip (all widths): PASSED\n");
    }

    TEST("runtime configuration") {
        multilruConfig config = {
            .maxLevels = 4,
            .startCapacity = 100,
            .policy = MLRU_POLICY_COUNT,
            .maxCount = 0,
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Insert some entries */
        for (int i = 0; i < 100; i++) {
            multilruInsertWeighted(mlru, 10);
        }
        assert(multilruCount(mlru) == 100);

        /* Set max count and policy - should not auto-evict existing entries */
        multilruSetMaxCount(mlru, 50);

        /* Insert one more - should trigger eviction down to maxCount */
        multilruInsertWeighted(mlru, 10);
        assert(multilruCount(mlru) <= 50);

        /* Change to weight policy */
        multilruSetPolicy(mlru, MLRU_POLICY_SIZE);
        multilruSetMaxWeight(mlru, 200);

        /* Insert more - should evict based on weight */
        for (int i = 0; i < 50; i++) {
            multilruInsertWeighted(mlru, 10);
        }
        assert(multilruTotalWeight(mlru) <= 200);

        printf("runtime configuration: PASSED\n");
    }

    TEST("weight update") {
        multilruConfig config = {
            .maxLevels = 4,
            .startCapacity = 100,
            .policy = MLRU_POLICY_SIZE,
            .maxWeight = 1000,
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Insert entry with weight 100 */
        multilruPtr p = multilruInsertWeighted(mlru, 100);
        assert(multilruGetWeight(mlru, p) == 100);
        assert(multilruTotalWeight(mlru) == 100);

        /* Update weight */
        multilruUpdateWeight(mlru, p, 500);
        assert(multilruGetWeight(mlru, p) == 500);
        assert(multilruTotalWeight(mlru) == 500);

        /* Insert more and verify total */
        multilruInsertWeighted(mlru, 200);
        assert(multilruTotalWeight(mlru) == 700);

        printf("weight update: PASSED\n");
    }

    TEST("evictN and evictToSize") {
        multilruConfig config = {
            .maxLevels = 4,
            .startCapacity = 100,
            .policy = MLRU_POLICY_COUNT,
            .maxCount = 0, /* No auto-eviction */
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Insert 50 entries with weight 10 each */
        for (int i = 0; i < 50; i++) {
            multilruInsertWeighted(mlru, 10);
        }
        assert(multilruCount(mlru) == 50);
        assert(multilruTotalWeight(mlru) == 500);

        /* Evict 10 entries */
        multilruPtr evicted[20];
        size_t n = multilruEvictN(mlru, evicted, 10);
        assert(n == 10);
        assert(multilruCount(mlru) == 40);

        /* Evict to target weight of 200 (need to evict ~20 more) */
        n = multilruEvictToSize(mlru, 200, evicted, 20);
        assert(multilruTotalWeight(mlru) <= 200);

        printf("evictN and evictToSize: PASSED\n");
    }

    TEST("collect and getN with n above the population") {
        cleaner(multilruFree) multilru *mlru = multilruNew();
        const multilruPtr sentinel = (multilruPtr)0xDEADBEEF;

        /* Empty cache: nothing written, GetN zero-fills */
        multilruPtr out[10];
        for (size_t i = 0; i < 10; i++) {
            out[i] = sentinel;
        }
        assert(multilruCollectLowest(mlru, out, 10) == 0);
        assert(multilruCollectHighest(mlru, out, 10) == 0);
        for (size_t i = 0; i < 10; i++) {
            assert(out[i] == sentinel);
        }

        /* Three entries, the last one promoted */
        multilruPtr e[3];
        for (size_t i = 0; i < 3; i++) {
            e[i] = multilruInsert(mlru);
        }
        multilruIncrease(mlru, e[2]);

        for (int highest = 0; highest < 2; highest++) {
            multilruPtr got[10];
            for (size_t i = 0; i < 10; i++) {
                got[i] = sentinel;
            }

            const size_t n = highest ? multilruCollectHighest(mlru, got, 10)
                                     : multilruCollectLowest(mlru, got, 10);
            assert(n == 3);
            for (size_t i = n; i < 10; i++) {
                assert(got[i] == sentinel); /* untouched */
            }

            /* Every entry appears exactly once */
            for (size_t i = 0; i < 3; i++) {
                size_t seen = 0;
                for (size_t j = 0; j < n; j++) {
                    seen += got[j] == e[i];
                }
                assert(seen == 1);
            }

            /* Coldest first vs hottest first */
            assert(got[0] == (highest ? e[2] : e[0]));

            multilruPtr filled[10];
            for (size_t i = 0; i < 10; i++) {
                filled[i] = sentinel;
            }
            if (highest) {
                multilruGetNHighest(mlru, filled, 10);
            } else {
                multilruGetNLowest(mlru, filled, 10);
            }

            for (size_t i = 0; i < 10; i++) {
                assert(filled[i] == (i < n ? got[i] : 0));
            }
        }

        /* n smaller than the population stops at n */
        multilruPtr two[2];
        assert(multilruCollectLowest(mlru, two, 2) == 2);
        assert(two[0] == e[0] && two[1] == e[1]);

        printf("collect and getN with n above the population: PASSED\n");
    }

    TEST("edge cases") {
        cleaner(multilruFree) multilru *mlru = multilruNew();

        /* Remove from empty */
        multilruPtr ptr;
        assert(!multilruRemoveMinimum(mlru, &ptr));

        /* GetNLowest from empty */
        multilruPtr lowest[5] = {};
        multilruGetNLowest(mlru, lowest, 5);
        for (int i = 0; i < 5; i++) {
            assert(lowest[i] == 0);
        }

        /* Single entry operations */
        multilruPtr single = multilruInsert(mlru);
        assert(single != 0);
        assert(multilruCount(mlru) == 1);

        /* Promote single entry multiple times */
        for (int i = 0; i < 10; i++) {
            multilruIncrease(mlru, single);
        }
        assert(multilruCount(mlru) == 1);
        assert(multilruGetLevel(mlru, single) == 6); /* Max level is 6 (0-6) */

        /* Delete specific entry */
        multilruDelete(mlru, single);
        assert(multilruCount(mlru) == 0);

        /* Delete invalid entry (should be no-op) */
        multilruDelete(mlru, 999);
        assert(multilruCount(mlru) == 0);

        printf("edge cases: PASSED\n");
    }

    TEST("fuzz test - random operations") {
        cleaner(multilruFree) multilru *mlru =
            multilruNewWithLevelsCapacity(7, 1000);

        /* Track inserted entries for validation */
        multilruPtr entries[1000] = {};
        size_t entryCount = 0;

        uint64_t seed[2] = {12345, 67890};

        for (int iter = 0; iter < 10000; iter++) {
            uint64_t rnd = xoroshiro128plus(seed);
            int op = rnd % 4;

            switch (op) {
            case 0: /* Insert */
                if (entryCount < 1000) {
                    multilruPtr p = multilruInsert(mlru);
                    if (p != 0) {
                        entries[entryCount++] = p;
                    }
                }
                break;

            case 1: /* Remove */
                if (entryCount > 0) {
                    multilruPtr removed;
                    if (multilruRemoveMinimum(mlru, &removed)) {
                        /* Remove from tracking (swap with last) */
                        for (size_t i = 0; i < entryCount; i++) {
                            if (entries[i] == removed) {
                                entries[i] = entries[--entryCount];
                                break;
                            }
                        }
                    }
                }
                break;

            case 2: /* Promote */
                if (entryCount > 0) {
                    size_t idx = rnd % entryCount;
                    multilruIncrease(mlru, entries[idx]);
                }
                break;

            case 3: /* Delete specific */
                if (entryCount > 0) {
                    size_t idx = rnd % entryCount;
                    multilruDelete(mlru, entries[idx]);
                    entries[idx] = entries[--entryCount];
                }
                break;
            }

            /* Verify count invariant */
            size_t levelTotal = 0;
            for (size_t i = 0; i < 7; i++) {
                levelTotal += multilruLevelCount(mlru, i);
            }
            assert(levelTotal == multilruCount(mlru));
        }

        printf("fuzz test - random operations: PASSED\n");
    }

    TEST("performance benchmark") {
        printf("\n=== MULTILRU PERFORMANCE SUMMARY ===\n");

        cleaner(multilruFree) multilru *mlru =
            multilruNewWithLevelsCapacity(7, 131072);
        const size_t benchCount = 500000;

        /* Benchmark insert */
        int64_t startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < benchCount; i++) {
            multilruInsert(mlru);
        }
        int64_t insertNs = timeUtilMonotonicNs() - startNs;

        /* Benchmark increase */
        multilruPtr testPtr = mlru->lowest;
        startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < 100000 && testPtr; i++) {
            multilruIncrease(mlru, testPtr);
        }
        int64_t increaseNs = timeUtilMonotonicNs() - startNs;

        /* Benchmark remove */
        startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < 100000; i++) {
            multilruPtr removed;
            multilruRemoveMinimum(mlru, &removed);
        }
        int64_t removeNs = timeUtilMonotonicNs() - startNs;

        printf("Insert rate:   %.0f ops/sec (%.1f ns/op)\n",
               benchCount / (insertNs / 1e9), (double)insertNs / benchCount);
        printf("Increase rate: %.0f ops/sec (%.1f ns/op)\n",
               100000.0 / (increaseNs / 1e9), (double)increaseNs / 100000);
        printf("Remove rate:   %.0f ops/sec (%.1f ns/op)\n",
               100000.0 / (removeNs / 1e9), (double)removeNs / 100000);
        printf("Memory used:   %zu bytes for %zu entries (%.2f bytes/entry)\n",
               multilruBytes(mlru), multilruCount(mlru),
               multilruCount(mlru) > 0
                   ? (double)multilruBytes(mlru) / multilruCount(mlru)
                   : 0);
        printf("Entry width:   %u bytes\n", mlru->entryWidth);
        printf("=====================================\n\n");
    }

    /* ================================================================
     * COMPREHENSIVE TEST SUITE - PART 2
     * ================================================================ */

    TEST("API completeness - all creation functions") {
        /* Test multilruNew */
        {
            cleaner(multilruFree) multilru *m1 = multilruNew();
            assert(m1 != NULL);
            assert(multilruMaxLevels(m1) == 7);
            assert(multilruCount(m1) == 0);
        }

        /* Test multilruNewWithLevels */
        {
            cleaner(multilruFree) multilru *m2 = multilruNewWithLevels(4);
            assert(m2 != NULL);
            assert(multilruMaxLevels(m2) == 4);
        }

        /* Test multilruNewWithLevelsCapacity */
        {
            cleaner(multilruFree) multilru *m3 =
                multilruNewWithLevelsCapacity(10, 5000);
            assert(m3 != NULL);
            assert(multilruMaxLevels(m3) == 10);
            assert(multilruCapacity(m3) >= 5000);
        }

        /* Test multilruNewWithConfig - all options */
        {
            multilruConfig config = {
                .maxLevels = 8,
                .startCapacity = 1000,
                .policy = MLRU_POLICY_HYBRID,
                .evictStrategy = MLRU_EVICT_SIZE_WEIGHTED,
                .maxCount = 500,
                .maxWeight = 10000,
                .enableWeights = true,
            };
            cleaner(multilruFree) multilru *m4 = multilruNewWithConfig(&config);
            assert(m4 != NULL);
            assert(multilruMaxLevels(m4) == 8);
        }

        printf("API completeness - all creation functions: PASSED\n");
    }

    TEST("API completeness - all query functions") {
        multilruConfig config = {
            .maxLevels = 5,
            .startCapacity = 100,
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Insert entries at various levels with weights */
        multilruPtr ptrs[50];
        for (int i = 0; i < 50; i++) {
            ptrs[i] = multilruInsertWeighted(mlru, (i + 1) * 10);
            for (int j = 0; j < (i % 5); j++) {
                multilruIncrease(mlru, ptrs[i]);
            }
        }

        /* Test all query functions */
        assert(multilruCount(mlru) == 50);
        assert(multilruBytes(mlru) > 0);
        assert(multilruTotalWeight(mlru) ==
               50 * 51 / 2 * 10); /* Sum 10+20+...+500 */
        assert(multilruMaxLevels(mlru) == 5);
        assert(multilruGetEntryWidth(mlru) == MLRU_WIDTH_5);
        assert(multilruCapacity(mlru) >= 50);

        /* Test per-level queries */
        size_t totalFromLevels = 0;
        uint64_t totalWeightFromLevels = 0;
        for (size_t i = 0; i < 5; i++) {
            totalFromLevels += multilruLevelCount(mlru, i);
            totalWeightFromLevels += multilruLevelWeight(mlru, i);
        }
        assert(totalFromLevels == 50);
        assert(totalWeightFromLevels == multilruTotalWeight(mlru));

        /* Test per-entry queries */
        for (int i = 0; i < 50; i++) {
            assert(multilruIsPopulated(mlru, ptrs[i]));
            assert(multilruGetWeight(mlru, ptrs[i]) == (uint64_t)(i + 1) * 10);
            assert(multilruGetLevel(mlru, ptrs[i]) == (size_t)(i % 5));
        }

        printf("API completeness - all query functions: PASSED\n");
    }

    TEST("API completeness - all modification functions") {
        multilruConfig config = {
            .maxLevels = 4,
            .startCapacity = 100,
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Test Insert */
        multilruPtr p1 = multilruInsert(mlru);
        assert(p1 != 0);
        assert(multilruGetWeight(mlru, p1) == 0);

        /* Test InsertWeighted */
        multilruPtr p2 = multilruInsertWeighted(mlru, 100);
        assert(p2 != 0);
        assert(multilruGetWeight(mlru, p2) == 100);

        /* Test Increase */
        assert(multilruGetLevel(mlru, p1) == 0);
        multilruIncrease(mlru, p1);
        assert(multilruGetLevel(mlru, p1) == 1);
        multilruIncrease(mlru, p1);
        assert(multilruGetLevel(mlru, p1) == 2);

        /* Test UpdateWeight */
        multilruUpdateWeight(mlru, p2, 200);
        assert(multilruGetWeight(mlru, p2) == 200);
        assert(multilruTotalWeight(mlru) == 200);

        /* Test Delete */
        multilruDelete(mlru, p1);
        assert(multilruCount(mlru) == 1);
        assert(!multilruIsPopulated(mlru, p1));

        /* Test RemoveMinimum */
        multilruPtr removed;
        bool ok = multilruRemoveMinimum(mlru, &removed);
        assert(ok);
        assert(removed == p2);
        assert(multilruCount(mlru) == 0);

        printf("API completeness - all modification functions: PASSED\n");
    }

    TEST("API completeness - all eviction functions") {
        multilruConfig config = {
            .maxLevels = 4,
            .startCapacity = 200,
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Insert 100 entries with weight 10 each */
        for (int i = 0; i < 100; i++) {
            multilruInsertWeighted(mlru, 10);
        }
        assert(multilruCount(mlru) == 100);
        assert(multilruTotalWeight(mlru) == 1000);

        /* Test EvictN */
        multilruPtr evicted[50];
        size_t n = multilruEvictN(mlru, evicted, 20);
        assert(n == 20);
        assert(multilruCount(mlru) == 80);
        assert(multilruTotalWeight(mlru) == 800);

        /* Test EvictToSize */
        n = multilruEvictToSize(mlru, 500, evicted, 50);
        assert(multilruTotalWeight(mlru) <= 500);

        /* Test GetNLowest */
        multilruPtr lowest[10] = {};
        multilruGetNLowest(mlru, lowest, 10);
        for (int i = 0; i < 10 && lowest[i] != 0; i++) {
            assert(multilruGetLevel(mlru, lowest[i]) == 0);
        }

        /* Test GetNHighest - promote some entries first */
        for (size_t i = 0; i < multilruCount(mlru) && i < 10; i++) {
            if (lowest[i] != 0) {
                for (int j = 0; j < 3; j++) {
                    multilruIncrease(mlru, lowest[i]);
                }
            }
        }
        multilruPtr highest[10];
        multilruGetNHighest(mlru, highest, 10);
        /* Highest should be at level 3 */
        for (int i = 0; i < 10 && highest[i] != 0; i++) {
            assert(multilruGetLevel(mlru, highest[i]) >= 1);
        }

        printf("API completeness - all eviction functions: PASSED\n");
    }

    TEST("API completeness - all configuration functions") {
        cleaner(multilruFree) multilru *mlru = multilruNew();

        /* Test SetPolicy */
        multilruSetPolicy(mlru, MLRU_POLICY_COUNT);
        multilruSetPolicy(mlru, MLRU_POLICY_SIZE);
        multilruSetPolicy(mlru, MLRU_POLICY_HYBRID);

        /* Test SetEvictStrategy */
        multilruSetEvictStrategy(mlru, MLRU_EVICT_LRU);
        multilruSetEvictStrategy(mlru, MLRU_EVICT_SIZE_WEIGHTED);
        multilruSetEvictStrategy(mlru, MLRU_EVICT_SIZE_LRU);

        /* Test SetMaxCount */
        multilruSetMaxCount(mlru, 100);
        multilruSetMaxCount(mlru, 0);

        /* Test SetMaxWeight */
        multilruSetMaxWeight(mlru, 10000);
        multilruSetMaxWeight(mlru, 0);

        printf("API completeness - all configuration functions: PASSED\n");
    }

    TEST("boundary conditions - empty cache") {
        cleaner(multilruFree) multilru *mlru = multilruNew();

        /* Operations on empty cache */
        assert(multilruCount(mlru) == 0);
        assert(multilruTotalWeight(mlru) == 0);

        multilruPtr ptr;
        assert(!multilruRemoveMinimum(mlru, &ptr));

        multilruPtr lowest[5] = {};
        multilruGetNLowest(mlru, lowest, 5);
        for (int i = 0; i < 5; i++) {
            assert(lowest[i] == 0);
        }

        multilruPtr highest[5] = {};
        multilruGetNHighest(mlru, highest, 5);
        for (int i = 0; i < 5; i++) {
            assert(highest[i] == 0);
        }

        multilruPtr evicted[5];
        assert(multilruEvictN(mlru, evicted, 5) == 0);

        /* Delete non-existent entry (should be no-op) */
        multilruDelete(mlru, 999);
        assert(multilruCount(mlru) == 0);

        printf("boundary conditions - empty cache: PASSED\n");
    }

    TEST("boundary conditions - single entry") {
        cleaner(multilruFree) multilru *mlru = multilruNew();

        multilruPtr p = multilruInsert(mlru);
        assert(p != 0);
        assert(multilruCount(mlru) == 1);

        /* Promote to max level */
        for (int i = 0; i < 100; i++) {
            multilruIncrease(mlru, p);
        }
        assert(multilruGetLevel(mlru, p) == 6); /* Max is 6 for 7 levels */

        /* GetNLowest/Highest should return the single entry */
        multilruPtr lowest[5] = {};
        multilruGetNLowest(mlru, lowest, 5);
        assert(lowest[0] == p);

        multilruPtr highest[5] = {};
        multilruGetNHighest(mlru, highest, 5);
        assert(highest[0] == p);

        /* Remove the single entry - needs multiple calls due to S4LRU demotion
         * Entry at level 6 gets demoted: 6→5→4→3→2→1→0→evict (7 calls) */
        multilruPtr removed;
        for (int i = 0; i < 7; i++) {
            bool ok = multilruRemoveMinimum(mlru, &removed);
            assert(ok);
            assert(removed == p);
        }
        assert(multilruCount(mlru) == 0);

        printf("boundary conditions - single entry: PASSED\n");
    }

    TEST("boundary conditions - max levels") {
        /* Test with maximum supported levels (64) */
        cleaner(multilruFree) multilru *mlru = multilruNewWithLevels(64);
        assert(multilruMaxLevels(mlru) == 64);

        multilruPtr p = multilruInsert(mlru);

        /* Promote through all 64 levels */
        for (int i = 0; i < 100; i++) {
            multilruIncrease(mlru, p);
        }
        assert(multilruGetLevel(mlru, p) == 63);

        printf("boundary conditions - max levels: PASSED\n");
    }

    TEST("boundary conditions - capacity limits") {
        /* Start with minimal capacity */
        cleaner(multilruFree) multilru *mlru =
            multilruNewWithLevelsCapacity(4, 10);

        /* Insert more than initial capacity to trigger growth */
        for (int i = 0; i < 1000; i++) {
            multilruPtr p = multilruInsert(mlru);
            assert(p != 0);
        }
        assert(multilruCount(mlru) == 1000);
        assert(multilruCapacity(mlru) >= 1000);

        printf("boundary conditions - capacity limits: PASSED\n");
    }

    /* ================================================================
     * INVARIANT VERIFICATION TESTS
     * ================================================================ */

    TEST("invariant verification - basic operations") {
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 100,
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        assert(verifyInvariants(mlru, "after creation"));

        /* Insert entries */
        for (int i = 0; i < 50; i++) {
            multilruInsertWeighted(mlru, 10);
            assert(verifyInvariants(mlru, "after insert"));
        }

        /* Promote entries */
        multilruPtr lowest[10] = {};
        multilruGetNLowest(mlru, lowest, 10);
        for (int i = 0; i < 10; i++) {
            if (lowest[i] != 0) {
                multilruIncrease(mlru, lowest[i]);
                assert(verifyInvariants(mlru, "after promote"));
            }
        }

        /* Remove entries */
        for (int i = 0; i < 25; i++) {
            multilruPtr removed;
            multilruRemoveMinimum(mlru, &removed);
            assert(verifyInvariants(mlru, "after remove"));
        }

        printf("invariant verification - basic operations: PASSED\n");
    }

    /* ================================================================
     * COMPREHENSIVE FUZZING HARNESS
     * ================================================================ */

    TEST("comprehensive fuzz - all operations") {
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 1000,
            .policy = MLRU_POLICY_HYBRID,
            .maxCount = 500,
            .maxWeight = 50000,
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Track live entries */
        multilruPtr entries[2000] = {};
        size_t entryCount = 0;
        const size_t maxEntries = 2000;

        uint64_t seed[2] = {0xDEADBEEF, 0xCAFEBABE};
        const int iterations = 50000;

        for (int iter = 0; iter < iterations; iter++) {
            uint64_t rnd = xoroshiro128plus(seed);
            FuzzOp op = rnd % FUZZ_OP_COUNT;

            switch (op) {
            case FUZZ_INSERT:
                if (entryCount < maxEntries) {
                    multilruPtr p = multilruInsert(mlru);
                    if (p != 0) {
                        entries[entryCount++] = p;
                    }
                }
                break;

            case FUZZ_INSERT_WEIGHTED:
                if (entryCount < maxEntries) {
                    uint64_t weight = (rnd >> 16) % 1000 + 1;
                    multilruPtr p = multilruInsertWeighted(mlru, weight);
                    if (p != 0) {
                        entries[entryCount++] = p;
                    }
                }
                break;

            case FUZZ_INCREASE:
                if (entryCount > 0) {
                    size_t idx = (rnd >> 8) % entryCount;
                    multilruIncrease(mlru, entries[idx]);
                }
                break;

            case FUZZ_REMOVE_MIN:
                if (entryCount > 0) {
                    multilruPtr removed;
                    if (multilruRemoveMinimum(mlru, &removed)) {
                        /* Find and remove from tracking */
                        for (size_t i = 0; i < entryCount; i++) {
                            if (entries[i] == removed) {
                                entries[i] = entries[--entryCount];
                                break;
                            }
                        }
                    }
                }
                break;

            case FUZZ_DELETE:
                if (entryCount > 0) {
                    size_t idx = (rnd >> 8) % entryCount;
                    multilruDelete(mlru, entries[idx]);
                    entries[idx] = entries[--entryCount];
                }
                break;

            case FUZZ_UPDATE_WEIGHT:
                if (entryCount > 0) {
                    size_t idx = (rnd >> 8) % entryCount;
                    uint64_t newWeight = (rnd >> 16) % 1000 + 1;
                    multilruUpdateWeight(mlru, entries[idx], newWeight);
                }
                break;

            case FUZZ_EVICT_N:
                if (entryCount > 0) {
                    multilruPtr evicted[10];
                    size_t n =
                        multilruEvictN(mlru, evicted, (rnd >> 8) % 10 + 1);
                    for (size_t i = 0; i < n; i++) {
                        for (size_t j = 0; j < entryCount; j++) {
                            if (entries[j] == evicted[i]) {
                                entries[j] = entries[--entryCount];
                                break;
                            }
                        }
                    }
                }
                break;

            case FUZZ_EVICT_TO_SIZE:
                if (entryCount > 0 && mlru->weights) {
                    uint64_t target = multilruTotalWeight(mlru) / 2;
                    multilruPtr evicted[50];
                    size_t n = multilruEvictToSize(mlru, target, evicted, 50);
                    for (size_t i = 0; i < n; i++) {
                        for (size_t j = 0; j < entryCount; j++) {
                            if (entries[j] == evicted[i]) {
                                entries[j] = entries[--entryCount];
                                break;
                            }
                        }
                    }
                }
                break;

            case FUZZ_SET_POLICY:
                multilruSetPolicy(mlru, rnd % 3);
                break;

            case FUZZ_SET_MAX_COUNT:
                multilruSetMaxCount(mlru, (rnd >> 8) % 1000);
                break;

            case FUZZ_SET_MAX_WEIGHT:
                multilruSetMaxWeight(mlru, (rnd >> 8) % 100000);
                break;

            default:
                break;
            }

            /* Verify invariants periodically */
            if (iter % 1000 == 0) {
                char ctx[64];
                snprintf(ctx, sizeof(ctx), "fuzz iter %d", iter);
                assert(verifyInvariants(mlru, ctx));
            }
        }

        assert(verifyInvariants(mlru, "fuzz final"));
        printf("comprehensive fuzz - all operations: PASSED\n");
    }

    TEST("fuzz - policy combinations") {
        /* Test all policy and strategy combinations */
        multilruPolicy policies[] = {MLRU_POLICY_COUNT, MLRU_POLICY_SIZE,
                                     MLRU_POLICY_HYBRID};
        multilruEvictStrategy strategies[] = {
            MLRU_EVICT_LRU, MLRU_EVICT_SIZE_WEIGHTED, MLRU_EVICT_SIZE_LRU};

        for (int p = 0; p < 3; p++) {
            for (int s = 0; s < 3; s++) {
                multilruConfig config = {
                    .maxLevels = 4,
                    .startCapacity = 100,
                    .policy = policies[p],
                    .evictStrategy = strategies[s],
                    .maxCount = 50,
                    .maxWeight = 5000,
                    .enableWeights = true,
                };
                cleaner(multilruFree) multilru *mlru =
                    multilruNewWithConfig(&config);

                uint64_t seed[2] = {p * 1000 + s, 12345};

                /* Random operations */
                for (int i = 0; i < 500; i++) {
                    uint64_t rnd = xoroshiro128plus(seed);
                    if (rnd % 3 == 0) {
                        multilruInsertWeighted(mlru, rnd % 200 + 1);
                    } else if (rnd % 3 == 1 && multilruCount(mlru) > 0) {
                        multilruPtr removed;
                        multilruRemoveMinimum(mlru, &removed);
                    } else {
                        multilruPtr lowest[1];
                        if (multilruCollectLowest(mlru, lowest, 1) == 1) {
                            multilruIncrease(mlru, lowest[0]);
                        }
                    }
                }

                assert(verifyInvariants(mlru, "policy combo"));
            }
        }
        printf("fuzz - policy combinations: PASSED\n");
    }

    TEST("fuzz - width transitions") {
        /* Test that invariants hold across width upgrades (5->6 at 64K
         * boundary) */
        cleaner(multilruFree) multilru *mlru =
            multilruNewWithLevelsCapacity(4, 100);
        assert(multilruGetEntryWidth(mlru) == MLRU_WIDTH_5);

        multilruPtr entries[80000];
        size_t count = 0;

        /* Insert past 64K threshold */
        for (size_t i = 0; i < 80000; i++) {
            entries[count] = multilruInsert(mlru);
            if (entries[count] != 0) {
                /* Randomly promote */
                if (i % 7 == 0) {
                    multilruIncrease(mlru, entries[count]);
                }
                count++;
            }

            /* Verify invariants at key points */
            if (i == 50000 || i == 65000 || i == 79999) {
                assert(verifyInvariants(mlru, "width transition"));
            }
        }

        /* 80K entries should be using width 6 (supports up to 1M) */
        assert(multilruGetEntryWidth(mlru) == MLRU_WIDTH_6);
        assert(verifyInvariants(mlru, "after width upgrade"));

        /* Remove half and verify */
        for (size_t i = 0; i < count / 2; i++) {
            multilruPtr removed;
            multilruRemoveMinimum(mlru, &removed);
        }
        assert(verifyInvariants(mlru, "after mass removal"));

        printf("fuzz - width transitions: PASSED\n");
    }

    TEST("fuzz - ID allocation correctness") {
        /* Comprehensive test for ID allocation, recycling, and reuse.
         * Verifies:
         * - No duplicate IDs are ever returned
         * - Recycled IDs were previously freed
         * - Fresh IDs are actually unused
         * - nextFresh and freeCount invariants hold */

        const size_t MAX_ENTRIES = 10000;
        const int NUM_OPS = 100000;

        cleaner(multilruFree) multilru *mlru =
            multilruNewWithLevelsCapacity(4, 256);

        /* Bitmap to track which IDs are currently live (in the cache).
         * Size dynamically based on IDs we see (IDs can exceed MAX_ENTRIES). */
        size_t bitmapCapacity = MAX_ENTRIES * 2; /* Start with 2x headroom */
        cleanerName(zfree, Bytes) uint8_t *liveIds =
            zcalloc(1, (bitmapCapacity + 7) / 8);

        /* Track all IDs ever seen for duplicate detection */
        cleanerName(zfree, Ptrs) multilruPtr *liveList =
            zcalloc(MAX_ENTRIES, sizeof(multilruPtr));
        size_t liveCount = 0;

        /* Track the highest ID we've ever seen (should match nextFresh - 1) */
        uint64_t highestIdSeen = 0;

        uint64_t seed[2] = {0xDEADBEEF, 0xCAFEBABE};

        /* Helper macro to ensure bitmap can hold given ID */
#define ENSURE_BITMAP_CAPACITY(id)                                             \
    do {                                                                       \
        if ((id) >= bitmapCapacity) {                                          \
            size_t newCap = fibbufNextSizeBuffer((id) + 1);                    \
            liveIds = zrealloc(liveIds, (newCap + 7) / 8);                     \
            memset(liveIds + (bitmapCapacity + 7) / 8, 0,                      \
                   (newCap + 7) / 8 - (bitmapCapacity + 7) / 8);               \
            bitmapCapacity = newCap;                                           \
        }                                                                      \
    } while (0)

        /* Helper macros for bitmap */
#define ID_IS_LIVE(id) ((liveIds[(id) / 8] >> ((id) % 8)) & 1)
#define ID_SET_LIVE(id) (liveIds[(id) / 8] |= (1 << ((id) % 8)))
#define ID_CLEAR_LIVE(id) (liveIds[(id) / 8] &= ~(1 << ((id) % 8)))

        /* Test 1: Rapid insert/delete cycles */
        for (int op = 0; op < NUM_OPS; op++) {
            uint64_t rnd = xoroshiro128plus(seed);

            if (rnd % 3 != 0 && liveCount < MAX_ENTRIES - 1) {
                /* Insert */
                multilruPtr id = multilruInsert(mlru);
                assert(id != 0 && "Insert should succeed");

                /* Ensure bitmap can hold this ID */
                ENSURE_BITMAP_CAPACITY(id);

                /* Verify not already live (no duplicates) */
                assert(!ID_IS_LIVE(id) &&
                       "New ID must not already be in cache");

                /* Mark as live */
                ID_SET_LIVE(id);
                liveList[liveCount++] = id;

                /* Track highest */
                if (id > highestIdSeen) {
                    highestIdSeen = id;
                }
            } else if (liveCount > 0) {
                /* Delete a random live entry */
                size_t delIdx = xoroshiro128plus(seed) % liveCount;
                multilruPtr id = liveList[delIdx];

                /* Verify it's actually live */
                assert(ID_IS_LIVE(id) && "Deleting entry must be live");

                multilruDelete(mlru, id);
                ID_CLEAR_LIVE(id);

                /* Remove from live list (swap with last) */
                liveList[delIdx] = liveList[--liveCount];
            }

            /* Periodically verify invariants */
            if (op % 5000 == 0) {
                assert(verifyInvariants(mlru, "id alloc fuzz"));
                assert(multilruCount(mlru) == liveCount);
            }
        }

        assert(verifyInvariants(mlru, "after test 1"));
        printf("  ID alloc test 1 (insert/delete cycles): OK (%zu entries)\n",
               liveCount);

        /* Test 2: Delete all, then refill - should reuse recycled IDs */
        size_t oldLiveCount = liveCount;

        /* Delete all entries */
        while (liveCount > 0) {
            multilruPtr id = liveList[--liveCount];
            multilruDelete(mlru, id);
            ID_CLEAR_LIVE(id);
        }

        assert(multilruCount(mlru) == 0);
        assert(verifyInvariants(mlru, "after delete all"));

        /* Record nextFresh before reinserting */
        uint64_t nextFreshBefore = mlru->nextFresh;

        /* Refill - these should all be recycled IDs (nextFresh shouldn't
         * change) */
        for (size_t i = 0; i < oldLiveCount && i < 1000; i++) {
            multilruPtr id = multilruInsert(mlru);
            assert(id != 0);
            ENSURE_BITMAP_CAPACITY(id);
            assert(!ID_IS_LIVE(id) && "Recycled ID must not be live");
            assert(id <= highestIdSeen && "Should reuse existing IDs");

            ID_SET_LIVE(id);
            liveList[liveCount++] = id;
        }

        /* nextFresh should not have changed since we recycled */
        assert(mlru->nextFresh == nextFreshBefore &&
               "Should reuse recycled IDs, not allocate fresh");
        assert(verifyInvariants(mlru, "after refill with recycled"));
        printf("  ID alloc test 2 (recycle after delete-all): OK\n");

        /* Test 3: Exhaust recycled, then fresh allocation resumes */
        /* Delete all again */
        while (liveCount > 0) {
            multilruPtr id = liveList[--liveCount];
            multilruDelete(mlru, id);
            ID_CLEAR_LIVE(id);
        }

        uint64_t freeCountBefore = mlru->freeCount;
        nextFreshBefore = mlru->nextFresh;

        /* Insert more than we have in free list.
         * We want to exhaust the free list and then allocate 100 fresh IDs.
         * Limit to MAX_ENTRIES to avoid overflowing liveList. */
        size_t wantedFresh = 100;
        size_t insertCount = (freeCountBefore + wantedFresh < MAX_ENTRIES)
                                 ? freeCountBefore + wantedFresh
                                 : MAX_ENTRIES;
        size_t actualFresh =
            (insertCount > freeCountBefore) ? insertCount - freeCountBefore : 0;

        for (size_t i = 0; i < insertCount; i++) {
            multilruPtr id = multilruInsert(mlru);
            assert(id != 0);
            ENSURE_BITMAP_CAPACITY(id);
            assert(!ID_IS_LIVE(id));

            ID_SET_LIVE(id);
            liveList[liveCount++] = id;

            if (id > highestIdSeen) {
                highestIdSeen = id;
            }
        }

        /* nextFresh should have advanced by the number of fresh allocations */
        assert(mlru->nextFresh == nextFreshBefore + actualFresh);
        assert(verifyInvariants(mlru, "after exhaust and fresh"));
        printf("  ID alloc test 3 (exhaust recycled + fresh): OK\n");

        /* Test 4: Interleaved pattern - insert 3, delete 1, repeat */
        for (int round = 0; round < 500; round++) {
            /* Insert 3 */
            for (int i = 0; i < 3 && liveCount < MAX_ENTRIES - 1; i++) {
                multilruPtr id = multilruInsert(mlru);
                if (id == 0) {
                    break; /* Capacity reached */
                }
                ENSURE_BITMAP_CAPACITY(id);
                assert(!ID_IS_LIVE(id));
                ID_SET_LIVE(id);
                liveList[liveCount++] = id;
                if (id > highestIdSeen) {
                    highestIdSeen = id;
                }
            }

            /* Delete 1 */
            if (liveCount > 0) {
                size_t delIdx = xoroshiro128plus(seed) % liveCount;
                multilruPtr id = liveList[delIdx];
                assert(ID_IS_LIVE(id));
                multilruDelete(mlru, id);
                ID_CLEAR_LIVE(id);
                liveList[delIdx] = liveList[--liveCount];
            }
        }

        assert(verifyInvariants(mlru, "after interleaved"));
        printf("  ID alloc test 4 (interleaved pattern): OK (%zu entries)\n",
               liveCount);

        /* Test 5: LIFO vs FIFO recycling - delete in order, reinsert */
        /* Clear all */
        while (liveCount > 0) {
            multilruPtr id = liveList[--liveCount];
            multilruDelete(mlru, id);
            ID_CLEAR_LIVE(id);
        }

        /* Insert 100 entries and record IDs */
        multilruPtr orderedIds[100];
        for (int i = 0; i < 100; i++) {
            orderedIds[i] = multilruInsert(mlru);
            assert(orderedIds[i] != 0);
            ENSURE_BITMAP_CAPACITY(orderedIds[i]);
            assert(!ID_IS_LIVE(orderedIds[i]));
            ID_SET_LIVE(orderedIds[i]);
            liveList[liveCount++] = orderedIds[i];
        }

        /* Delete in forward order */
        for (int i = 0; i < 100; i++) {
            assert(ID_IS_LIVE(orderedIds[i]));
            multilruDelete(mlru, orderedIds[i]);
            ID_CLEAR_LIVE(orderedIds[i]);
            liveCount--;
        }

        /* Reinsert - free list is LIFO so we expect reverse order */
        multilruPtr reinsertedIds[100];
        for (int i = 0; i < 100; i++) {
            reinsertedIds[i] = multilruInsert(mlru);
            assert(reinsertedIds[i] != 0);
            ENSURE_BITMAP_CAPACITY(reinsertedIds[i]);
            assert(!ID_IS_LIVE(reinsertedIds[i]));
            ID_SET_LIVE(reinsertedIds[i]);
            liveList[liveCount++] = reinsertedIds[i];
        }

        /* Verify LIFO order: last deleted = first reinserted */
        for (int i = 0; i < 100; i++) {
            assert(reinsertedIds[i] == orderedIds[99 - i] &&
                   "Free list should be LIFO");
        }

        assert(verifyInvariants(mlru, "after LIFO test"));
        printf("  ID alloc test 5 (LIFO recycling order): OK\n");

        /* Test 6: Stress test - many rapid cycles */
        for (int cycle = 0; cycle < 10; cycle++) {
            /* Clear all */
            while (liveCount > 0) {
                multilruPtr id = liveList[--liveCount];
                multilruDelete(mlru, id);
                ID_CLEAR_LIVE(id);
            }

            /* Insert 1000 */
            for (int i = 0; i < 1000; i++) {
                multilruPtr id = multilruInsert(mlru);
                assert(id != 0);
                ENSURE_BITMAP_CAPACITY(id);
                assert(!ID_IS_LIVE(id));
                ID_SET_LIVE(id);
                liveList[liveCount++] = id;
            }

            assert(verifyInvariants(mlru, "stress cycle"));
        }
        printf("  ID alloc test 6 (stress cycles): OK\n");

#undef ENSURE_BITMAP_CAPACITY
#undef ID_IS_LIVE
#undef ID_SET_LIVE
#undef ID_CLEAR_LIVE

        printf("fuzz - ID allocation correctness: PASSED\n");
    }

    TEST("fuzz - ID allocation edge cases") {
        /* Test edge cases in ID allocation */

        /* Test 1: Single entry insert/delete cycles */
        {
            cleaner(multilruFree) multilru *mlru =
                multilruNewWithLevelsCapacity(4, 16);
            multilruPtr lastId = 0;

            for (int i = 0; i < 1000; i++) {
                multilruPtr id = multilruInsert(mlru);
                assert(id != 0);

                /* After first cycle, should always get same recycled ID */
                if (i > 0) {
                    assert(id == lastId &&
                           "Single entry should recycle same ID");
                }

                multilruDelete(mlru, id);
                lastId = id;
            }

            assert(multilruCount(mlru) == 0);
            assert(mlru->freeCount == 1); /* One recycled slot */
            printf("  Edge case 1 (single entry cycles): OK\n");
        }

        /* Test 2: Fill to capacity, delete all, refill */
        {
            cleaner(multilruFree) multilru *mlru =
                multilruNewWithLevelsCapacity(4, 100);
            multilruPtr ids[95]; /* Leave room for 5 level heads */

            /* Fill completely (100 capacity - 5 level heads = 95 entries) */
            for (int i = 0; i < 95; i++) {
                ids[i] = multilruInsert(mlru);
                assert(ids[i] != 0);
            }

            /* Delete all */
            for (int i = 0; i < 95; i++) {
                multilruDelete(mlru, ids[i]);
            }

            assert(mlru->freeCount == 95);
            uint64_t nextFreshBefore = mlru->nextFresh;

            /* Refill - all should be recycled */
            for (int i = 0; i < 95; i++) {
                ids[i] = multilruInsert(mlru);
                assert(ids[i] != 0);
            }

            assert(mlru->nextFresh == nextFreshBefore);
            assert(mlru->freeCount == 0);
            printf("  Edge case 2 (fill/empty/refill): OK\n");
        }

        /* Test 3: Alternating insert/delete never exceeds initial capacity */
        {
            cleaner(multilruFree) multilru *mlru =
                multilruNewWithLevelsCapacity(4, 100);
            uint64_t initialCapacity = mlru->capacity;

            for (int i = 0; i < 10000; i++) {
                multilruPtr id = multilruInsert(mlru);
                assert(id != 0);
                multilruDelete(mlru, id);
            }

            /* Should never have grown - always recycling same slot */
            assert(mlru->capacity == initialCapacity);
            assert(mlru->nextFresh ==
                   mlru->maxLevels + 2); /* Only used 1 slot */
            printf("  Edge case 3 (alternating never grows): OK\n");
        }

        /* Test 4: Delete in middle creates holes, properly recycled */
        {
            cleaner(multilruFree) multilru *mlru =
                multilruNewWithLevelsCapacity(4, 100);
            multilruPtr ids[50];

            /* Insert 50 */
            for (int i = 0; i < 50; i++) {
                ids[i] = multilruInsert(mlru);
            }

            /* Delete every other one (creates holes) */
            for (int i = 0; i < 50; i += 2) {
                multilruDelete(mlru, ids[i]);
            }

            assert(mlru->freeCount == 25);

            /* Reinsert 25 - should fill holes */
            uint64_t nextFreshBefore = mlru->nextFresh;
            for (int i = 0; i < 25; i++) {
                multilruPtr id = multilruInsert(mlru);
                assert(id != 0);
            }

            assert(mlru->nextFresh == nextFreshBefore); /* No fresh allocs */
            assert(mlru->freeCount == 0);
            printf("  Edge case 4 (holes properly recycled): OK\n");
        }

        /* Test 5: Growth doesn't push to free list */
        {
            cleaner(multilruFree) multilru *mlru =
                multilruNewWithLevelsCapacity(4, 16);

            /* Fill initial capacity */
            for (int i = 0; i < 11; i++) { /* 16 - 5 heads = 11 */
                multilruPtr id = multilruInsert(mlru);
                assert(id != 0);
            }

            uint64_t freeCountBefore = mlru->freeCount;
            assert(freeCountBefore == 0);

            /* Next insert triggers growth */
            multilruPtr id = multilruInsert(mlru);
            assert(id != 0);

            /* Free count should still be 0 - growth doesn't push slots */
            assert(mlru->freeCount == 0);
            assert(mlru->capacity > 16);

            /* nextFresh should be exactly at next unused slot */
            assert(mlru->nextFresh == (uint64_t)(mlru->maxLevels + 1 + 12));
            printf("  Edge case 5 (growth O(1) no free list push): OK\n");
        }

        printf("fuzz - ID allocation edge cases: PASSED\n");
    }

    /* ================================================================
     * ACCESS PATTERN BENCHMARKS
     * ================================================================ */

    TEST("access patterns - sequential") {
        printf("\n--- Sequential Access Pattern ---\n");
        cleaner(multilruFree) multilru *mlru =
            multilruNewWithLevelsCapacity(7, 10000);

        /* Insert 10000 entries */
        multilruPtr ptrs[10000];
        for (int i = 0; i < 10000; i++) {
            ptrs[i] = multilruInsert(mlru);
        }

        /* Sequential access: access each entry once in order */
        int64_t startNs = timeUtilMonotonicNs();
        for (int round = 0; round < 10; round++) {
            for (int i = 0; i < 10000; i++) {
                multilruIncrease(mlru, ptrs[i]);
            }
        }
        int64_t elapsed = timeUtilMonotonicNs() - startNs;

        printf("Sequential: %.2f M ops/sec\n",
               (100000.0 / (elapsed / 1e9)) / 1e6);

        /* Check level distribution - should be fairly uniform */
        printf("Level distribution: ");
        for (size_t i = 0; i < 7; i++) {
            printf("L%zu=%zu ", i, multilruLevelCount(mlru, i));
        }
        printf("\n");

        printf("access patterns - sequential: PASSED\n");
    }

    TEST("access patterns - random uniform") {
        printf("\n--- Random Uniform Access Pattern ---\n");
        cleaner(multilruFree) multilru *mlru =
            multilruNewWithLevelsCapacity(7, 10000);

        multilruPtr ptrs[10000];
        for (int i = 0; i < 10000; i++) {
            ptrs[i] = multilruInsert(mlru);
        }

        uint64_t seed[2] = {42, 123};

        /* Random uniform access */
        int64_t startNs = timeUtilMonotonicNs();
        for (int i = 0; i < 100000; i++) {
            size_t idx = xoroshiro128plus(seed) % 10000;
            multilruIncrease(mlru, ptrs[idx]);
        }
        int64_t elapsed = timeUtilMonotonicNs() - startNs;

        printf("Random uniform: %.2f M ops/sec\n",
               (100000.0 / (elapsed / 1e9)) / 1e6);

        printf("Level distribution: ");
        for (size_t i = 0; i < 7; i++) {
            printf("L%zu=%zu ", i, multilruLevelCount(mlru, i));
        }
        printf("\n");

        printf("access patterns - random uniform: PASSED\n");
    }

    TEST("access patterns - zipfian (hot/cold)") {
        printf("\n--- Zipfian (Hot/Cold) Access Pattern ---\n");
        cleaner(multilruFree) multilru *mlru =
            multilruNewWithLevelsCapacity(7, 10000);

        multilruPtr ptrs[10000];
        for (int i = 0; i < 10000; i++) {
            ptrs[i] = multilruInsert(mlru);
        }

        uint64_t seed[2] = {42, 123};

        /* Zipfian access - 80% of accesses go to 20% of entries */
        int64_t startNs = timeUtilMonotonicNs();
        for (int i = 0; i < 100000; i++) {
            size_t idx = zipfian(seed, 10000, 1.0);
            multilruIncrease(mlru, ptrs[idx]);
        }
        int64_t elapsed = timeUtilMonotonicNs() - startNs;

        printf("Zipfian (skew=1.0): %.2f M ops/sec\n",
               (100000.0 / (elapsed / 1e9)) / 1e6);

        printf("Level distribution: ");
        for (size_t i = 0; i < 7; i++) {
            printf("L%zu=%zu ", i, multilruLevelCount(mlru, i));
        }
        printf("\n");

        /* Hot entries should be at higher levels */
        assert(multilruGetLevel(mlru, ptrs[0]) >= 4); /* Most accessed */

        printf("access patterns - zipfian (hot/cold): PASSED\n");
    }

    TEST("access patterns - working set") {
        printf("\n--- Working Set Access Pattern ---\n");
        cleaner(multilruFree) multilru *mlru =
            multilruNewWithLevelsCapacity(7, 10000);

        multilruPtr ptrs[10000];
        for (int i = 0; i < 10000; i++) {
            ptrs[i] = multilruInsert(mlru);
        }

        uint64_t seed[2] = {42, 123};

        /* Simulate working set: access a subset heavily, then shift */
        int64_t startNs = timeUtilMonotonicNs();
        for (int phase = 0; phase < 10; phase++) {
            size_t wsStart = (phase * 1000) % 10000;
            size_t wsSize = 1000;

            for (int i = 0; i < 10000; i++) {
                size_t idx = wsStart + (xoroshiro128plus(seed) % wsSize);
                if (idx >= 10000) {
                    idx -= 10000;
                }
                multilruIncrease(mlru, ptrs[idx]);
            }
        }
        int64_t elapsed = timeUtilMonotonicNs() - startNs;

        printf("Working set: %.2f M ops/sec\n",
               (100000.0 / (elapsed / 1e9)) / 1e6);

        printf("Level distribution: ");
        for (size_t i = 0; i < 7; i++) {
            printf("L%zu=%zu ", i, multilruLevelCount(mlru, i));
        }
        printf("\n");

        printf("access patterns - working set: PASSED\n");
    }

    TEST("access patterns - scan resistance") {
        printf("\n--- Scan Resistance Test ---\n");
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 1000,
            .policy = MLRU_POLICY_COUNT,
            .maxCount = 500,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Insert initial working set and promote heavily */
        multilruPtr hotSet[100];
        for (int i = 0; i < 100; i++) {
            hotSet[i] = multilruInsert(mlru);
            for (int j = 0; j < 5; j++) {
                multilruIncrease(mlru, hotSet[i]);
            }
        }

        /* Every handle issued after the hot set, so a hot slot that was
         * evicted and handed to a new entry is not mistaken for a survivor */
        multilruPtr issued[400 + 1000];
        size_t issuedCount = 0;

        /* Fill rest of cache */
        for (int i = 0; i < 400; i++) {
            issued[issuedCount++] = multilruInsert(mlru);
        }

        /* Record hot set levels before scan */
        size_t hotLevelsBefore[100];
        for (int i = 0; i < 100; i++) {
            hotLevelsBefore[i] = multilruGetLevel(mlru, hotSet[i]);
        }

        /* Simulate scan: insert 1000 entries (2x cache size) */
        for (int i = 0; i < 1000; i++) {
            issued[issuedCount++] = multilruInsert(mlru);
        }

        /* Check how many hot entries survived: still populated, and their
         * slot never reissued. Inserts never promote an existing entry, so
         * the scan can only have demoted a survivor. */
        int survived = 0;
        for (int i = 0; i < 100; i++) {
            bool reissued = false;
            for (size_t k = 0; k < issuedCount && !reissued; k++) {
                reissued = issued[k] == hotSet[i];
            }

            if (!reissued && multilruIsPopulated(mlru, hotSet[i])) {
                survived++;
                assert(multilruGetLevel(mlru, hotSet[i]) <= hotLevelsBefore[i]);
            }
        }

        printf("Hot entries survived scan: %d/100 (%.0f%%)\n", survived,
               survived * 100.0 / 100);

        /* S4LRU should protect hot entries - expect most to survive */
        assert(survived >= 50); /* At least 50% should survive */

        printf("access patterns - scan resistance: PASSED\n");
    }

    /* ================================================================
     * SCALE AND MEMORY BENCHMARKS
     * ================================================================ */

    TEST("scale benchmark - various sizes") {
        printf("\n=== SCALE BENCHMARK ===\n");
        printf("%-12s %-12s %-12s %-12s %-12s %-12s\n", "Entries", "Width",
               "Bytes/Entry", "Insert/sec", "Promote/sec", "Remove/sec");
        printf("---------------------------------------------------------------"
               "-----"
               "----\n");

        size_t scales[] = {100, 1000, 10000, 100000, 500000};
        for (int s = 0; s < 5; s++) {
            size_t scale = scales[s];
            cleaner(multilruFree) multilru *mlru =
                multilruNewWithLevelsCapacity(7, scale);

            /* Benchmark insert */
            int64_t startNs = timeUtilMonotonicNs();
            for (size_t i = 0; i < scale; i++) {
                multilruInsert(mlru);
            }
            int64_t insertNs = timeUtilMonotonicNs() - startNs;

            /* Benchmark promote */
            multilruPtr testPtr = mlru->lowest;
            startNs = timeUtilMonotonicNs();
            size_t promoteCount = scale < 100000 ? scale : 100000;
            for (size_t i = 0; i < promoteCount; i++) {
                multilruIncrease(mlru, testPtr);
            }
            int64_t promoteNs = timeUtilMonotonicNs() - startNs;

            /* Benchmark remove */
            startNs = timeUtilMonotonicNs();
            size_t removeCount = scale < 100000 ? scale / 2 : 50000;
            for (size_t i = 0; i < removeCount; i++) {
                multilruPtr removed;
                multilruRemoveMinimum(mlru, &removed);
            }
            int64_t removeNs = timeUtilMonotonicNs() - startNs;

            double bytesPerEntry =
                (double)multilruBytes(mlru) / multilruCount(mlru);

            printf("%-12zu %-12u %-12.2f %-12.0f %-12.0f %-12.0f\n", scale,
                   mlru->entryWidth, bytesPerEntry, scale / (insertNs / 1e9),
                   promoteCount / (promoteNs / 1e9),
                   removeCount / (removeNs / 1e9));
        }
        printf("\n");
    }

    TEST("memory efficiency analysis") {
        printf("\n=== MEMORY EFFICIENCY ANALYSIS ===\n");

        /* Test memory usage with and without weights */
        printf("\n--- Without Weights ---\n");
        printf("%-12s %-12s %-12s %-12s\n", "Entries", "Width", "Total Bytes",
               "Bytes/Entry");

        size_t testSizes[] = {100, 1000, 10000, 65000, 70000, 100000};
        for (int i = 0; i < 6; i++) {
            cleaner(multilruFree) multilru *mlru =
                multilruNewWithLevelsCapacity(7, testSizes[i]);
            for (size_t j = 0; j < testSizes[i]; j++) {
                multilruInsert(mlru);
            }
            printf("%-12zu %-12u %-12zu %-12.2f\n", testSizes[i],
                   mlru->entryWidth, multilruBytes(mlru),
                   (double)multilruBytes(mlru) / testSizes[i]);
        }

        printf("\n--- With Weights ---\n");
        printf("%-12s %-12s %-12s %-12s\n", "Entries", "Width", "Total Bytes",
               "Bytes/Entry");

        for (int i = 0; i < 6; i++) {
            multilruConfig config = {
                .maxLevels = 7,
                .startCapacity = testSizes[i],
                .enableWeights = true,
            };
            cleaner(multilruFree) multilru *mlru =
                multilruNewWithConfig(&config);
            for (size_t j = 0; j < testSizes[i]; j++) {
                multilruInsertWeighted(mlru, j * 10);
            }
            printf("%-12zu %-12u %-12zu %-12.2f\n", testSizes[i],
                   mlru->entryWidth, multilruBytes(mlru),
                   (double)multilruBytes(mlru) / testSizes[i]);
            fflush(stdout);
        }
        printf("memory efficiency analysis: PASSED\n");
    }

    TEST("S4LRU effectiveness analysis") {
        printf("\n=== S4LRU EFFECTIVENESS ANALYSIS ===\n");

        /* Compare hit rates under different access patterns */
        printf("Testing hit rate with cache size 1000, accessing 5000 unique "
               "items\n\n");

        /* Prepare access sequence with zipfian distribution */
        uint64_t seed[2] = {42, 123};
        size_t accessSeq[50000];
        for (int i = 0; i < 50000; i++) {
            accessSeq[i] = zipfian(seed, 5000, 1.0);
        }

        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 1000,
            .policy = MLRU_POLICY_COUNT,
            .maxCount = 1000,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Simulate cache accesses */
        size_t hits = 0;
        size_t misses = 0;
        multilruPtr items[5000] = {}; /* 0 means not in cache */

        for (int i = 0; i < 50000; i++) {
            size_t item = accessSeq[i];

            if (items[item] != 0 && multilruIsPopulated(mlru, items[item])) {
                /* Hit - promote */
                multilruIncrease(mlru, items[item]);
                hits++;
            } else {
                /* Miss - insert */
                items[item] = multilruInsert(mlru);
                misses++;

                /* Check if old mapping is still valid */
                for (size_t j = 0; j < 5000; j++) {
                    if (j != item && items[j] != 0 &&
                        !multilruIsPopulated(mlru, items[j])) {
                        items[j] = 0; /* Evicted */
                    }
                }
            }
        }

        double hitRate = (double)hits / (hits + misses) * 100;
        printf("Hit rate: %.2f%% (%zu hits, %zu misses)\n", hitRate, hits,
               misses);

        printf("Final level distribution:\n");
        for (size_t i = 0; i < 7; i++) {
            printf("  Level %zu: %zu entries\n", i,
                   multilruLevelCount(mlru, i));
        }

        printf("\nS4LRU effectiveness analysis: PASSED\n");
    }

    TEST("weighted eviction effectiveness") {
        printf("\n=== WEIGHTED EVICTION EFFECTIVENESS ===\n");

        /* Simulate video cache scenario */
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 1000,
            .policy = MLRU_POLICY_SIZE,
            .maxWeight = 10000, /* 10GB cache */
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Insert mix of large and small items */
        multilruPtr largeItems[20];
        multilruPtr smallItems[200];
        int largeCount = 0, smallCount = 0;

        for (int i = 0; i < 220; i++) {
            if (i % 11 == 0 && largeCount < 20) {
                /* Large item (1GB) */
                largeItems[largeCount++] = multilruInsertWeighted(mlru, 1000);
            } else if (smallCount < 200) {
                /* Small item (10MB) */
                smallItems[smallCount++] = multilruInsertWeighted(mlru, 10);
            }
        }

        printf("After initial insert:\n");
        printf("  Total weight: %" PRIu64 "\n", multilruTotalWeight(mlru));
        printf("  Entry count: %zu\n", multilruCount(mlru));

        /* Access small items frequently */
        for (int round = 0; round < 10; round++) {
            for (int i = 0; i < smallCount; i++) {
                if (multilruIsPopulated(mlru, smallItems[i])) {
                    multilruIncrease(mlru, smallItems[i]);
                }
            }
        }

        /* Count survivors */
        int largeSurvived = 0, smallSurvived = 0;
        for (int i = 0; i < largeCount; i++) {
            if (multilruIsPopulated(mlru, largeItems[i])) {
                largeSurvived++;
            }
        }
        for (int i = 0; i < smallCount; i++) {
            if (multilruIsPopulated(mlru, smallItems[i])) {
                smallSurvived++;
            }
        }

        printf("\nAfter frequent access to small items:\n");
        printf("  Large items survived: %d/%d\n", largeSurvived, largeCount);
        printf("  Small items survived: %d/%d\n", smallSurvived, smallCount);

        /* With weight-based eviction and S4LRU, frequently accessed small
         * items should survive while cold large items get evicted */
        assert(smallSurvived > largeSurvived);

        printf("\nweighted eviction effectiveness: PASSED\n");
    }

    /* ================================================================
     * STRESS TESTS
     * ================================================================ */

    TEST("stress - rapid insert/delete") {
        cleaner(multilruFree) multilru *mlru =
            multilruNewWithLevelsCapacity(7, 1000);

        /* Rapid alternating insert/delete */
        int64_t startNs = timeUtilMonotonicNs();
        for (int i = 0; i < 100000; i++) {
            multilruPtr p = multilruInsert(mlru);
            multilruDelete(mlru, p);
        }
        int64_t elapsed = timeUtilMonotonicNs() - startNs;

        printf("Rapid insert/delete: %.2f M ops/sec\n",
               (200000.0 / (elapsed / 1e9)) / 1e6);

        assert(multilruCount(mlru) == 0);
        assert(verifyInvariants(mlru, "rapid insert/delete"));

        printf("stress - rapid insert/delete: PASSED\n");
    }

    TEST("stress - promote storm") {
        cleaner(multilruFree) multilru *mlru =
            multilruNewWithLevelsCapacity(7, 1000);

        /* Insert entries */
        multilruPtr ptrs[1000];
        for (int i = 0; i < 1000; i++) {
            ptrs[i] = multilruInsert(mlru);
        }

        /* Promote all entries rapidly */
        int64_t startNs = timeUtilMonotonicNs();
        for (int round = 0; round < 100; round++) {
            for (int i = 0; i < 1000; i++) {
                multilruIncrease(mlru, ptrs[i]);
            }
        }
        int64_t elapsed = timeUtilMonotonicNs() - startNs;

        printf("Promote storm: %.2f M ops/sec\n",
               (100000.0 / (elapsed / 1e9)) / 1e6);

        assert(verifyInvariants(mlru, "promote storm"));

        printf("stress - promote storm: PASSED\n");
    }

    TEST("stress - eviction pressure") {
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 1000,
            .policy = MLRU_POLICY_COUNT,
            .maxCount = 100, /* Tiny cache */
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Insert way more than capacity */
        int64_t startNs = timeUtilMonotonicNs();
        for (int i = 0; i < 10000; i++) {
            multilruInsertWeighted(mlru, i % 100 + 1);
        }
        int64_t elapsed = timeUtilMonotonicNs() - startNs;

        printf("Eviction pressure: %.2f M ops/sec (with auto-evict)\n",
               (10000.0 / (elapsed / 1e9)) / 1e6);

        assert(multilruCount(mlru) <= 100);
        assert(verifyInvariants(mlru, "eviction pressure"));

        printf("stress - eviction pressure: PASSED\n");
    }

    /* ----------------------------------------------------------------
     * Production Eviction Workflow Tests
     * ---------------------------------------------------------------- */

    TEST("eviction workflow - callback notification") {
        /* Test callback-based eviction where external data is cleaned up
         * when entries are evicted */
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 100,
            .policy = MLRU_POLICY_COUNT,
            .maxCount = 10, /* Small cache to force eviction */
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Reset static eviction tracker */
        testEvictCount = 0;
        memset(testEvictedPtrs, 0, sizeof(testEvictedPtrs));

        multilruSetEvictCallback(mlru, testEvictionCallback, NULL);

        /* Verify auto-evict is enabled by default */
        assert(multilruGetAutoEvict(mlru) == true);

        /* Insert 20 entries - should trigger evictions after entry 10 */
        multilruPtr ptrs[20];
        for (int i = 0; i < 20; i++) {
            ptrs[i] = multilruInsert(mlru);
            assert(ptrs[i] != 0);
        }

        /* Should have evicted 10 entries (20 inserted, max 10 kept) */
        assert(multilruCount(mlru) == 10);
        /* Callback should have been called for evictions */
        assert(testEvictCount > 0);

        /* All evicted pointers should be valid (non-zero) */
        for (size_t i = 0; i < testEvictCount; i++) {
            assert(testEvictedPtrs[i] != 0);
        }

        /* Disable callback */
        multilruSetEvictCallback(mlru, NULL, NULL);

        /* More insertions should not call callback */
        size_t prevCount = testEvictCount;
        for (int i = 0; i < 10; i++) {
            multilruInsert(mlru);
        }
        assert(testEvictCount == prevCount); /* No new callbacks */

        printf("eviction workflow - callback notification: PASSED\n");
    }

    TEST("eviction workflow - manual polling") {
        /* Test manual eviction workflow where caller disables auto-evict
         * and polls/evicts manually */
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 100,
            .policy = MLRU_POLICY_COUNT,
            .maxCount = 10,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Disable auto-eviction */
        multilruSetAutoEvict(mlru, false);
        assert(multilruGetAutoEvict(mlru) == false);

        /* Insert 20 entries - cache should grow past limit */
        multilruPtr ptrs[20];
        for (int i = 0; i < 20; i++) {
            ptrs[i] = multilruInsert(mlru);
            assert(ptrs[i] != 0);
        }

        /* Cache should exceed limit (no auto-eviction) */
        assert(multilruCount(mlru) == 20);
        assert(multilruNeedsEviction(mlru) == true);

        /* Manual eviction loop - like a production cache would do */
        size_t evictedCount = 0;
        multilruPtr evictedPtrs[20];
        while (multilruNeedsEviction(mlru)) {
            multilruPtr evicted;
            bool ok = multilruRemoveMinimum(mlru, &evicted);
            assert(ok);
            if (evictedCount < 20) {
                evictedPtrs[evictedCount++] = evicted;
            }
            /* In production: cleanup external data for 'evicted' here */
        }

        /* Should now be within limits */
        assert(multilruCount(mlru) <= 10);
        assert(multilruNeedsEviction(mlru) == false);

        /* Every entry sat at level 0, so each call was a true eviction of
         * the oldest entry: the 10 handed back are the first 10 inserted,
         * in order, and none is still populated. */
        assert(evictedCount == 10);
        for (size_t i = 0; i < evictedCount; i++) {
            assert(evictedPtrs[i] == ptrs[i]);
            assert(!multilruIsPopulated(mlru, evictedPtrs[i]));
        }

        /* Re-enable auto-evict for further insertions */
        multilruSetAutoEvict(mlru, true);

        /* Now insertions should auto-evict */
        for (int i = 0; i < 10; i++) {
            multilruInsert(mlru);
        }
        assert(multilruCount(mlru) <= 10);

        printf("eviction workflow - manual polling: PASSED\n");
    }

    TEST("eviction workflow - weight-based manual") {
        /* Test manual eviction with weight/size-based policy */
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 100,
            .policy = MLRU_POLICY_SIZE,
            .maxWeight = 1000, /* Max 1000 bytes */
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Disable auto-eviction */
        multilruSetAutoEvict(mlru, false);

        /* Insert entries totaling 2000 bytes */
        for (int i = 0; i < 20; i++) {
            multilruInsertWeighted(mlru, 100); /* 100 bytes each */
        }

        /* Total weight should be 2000, over limit */
        assert(multilruTotalWeight(mlru) == 2000);
        assert(multilruNeedsEviction(mlru) == true);

        /* Evict until under weight limit */
        while (multilruNeedsEviction(mlru)) {
            multilruPtr evicted;
            multilruRemoveMinimum(mlru, &evicted);
            /* In production: free external data sized by original weight */
        }

        /* Should be at or under 1000 bytes */
        assert(multilruTotalWeight(mlru) <= 1000);
        assert(multilruNeedsEviction(mlru) == false);

        printf("eviction workflow - weight-based manual: PASSED\n");
    }

    TEST("eviction workflow - hybrid policy") {
        /* Test hybrid policy (count AND weight limits) */
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 100,
            .policy = MLRU_POLICY_HYBRID,
            .maxCount = 20,
            .maxWeight = 500,
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);
        multilruSetAutoEvict(mlru, false);

        /* Insert 15 entries at 50 bytes each = 750 bytes total
         * Under count limit (20) but over weight limit (500) */
        for (int i = 0; i < 15; i++) {
            multilruInsertWeighted(mlru, 50);
        }

        assert(multilruCount(mlru) == 15);
        assert(multilruTotalWeight(mlru) == 750);
        /* Should need eviction due to weight, not count */
        assert(multilruNeedsEviction(mlru) == true);

        /* Evict until under both limits */
        while (multilruNeedsEviction(mlru)) {
            multilruPtr evicted;
            multilruRemoveMinimum(mlru, &evicted);
        }

        assert(multilruTotalWeight(mlru) <= 500);
        assert(multilruCount(mlru) <= 20);

        printf("eviction workflow - hybrid policy: PASSED\n");
    }

    /* ----------------------------------------------------------------
     * Dynamic Cache Resizing Tests
     * ---------------------------------------------------------------- */

    TEST("resize - expand count limit") {
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 100,
            .policy = MLRU_POLICY_COUNT,
            .maxCount = 10,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Fill to capacity */
        for (int i = 0; i < 10; i++) {
            multilruInsert(mlru);
        }
        assert(multilruCount(mlru) == 10);
        assert(multilruNeedsEviction(mlru) == false);

        /* Expand limit */
        multilruSetMaxCount(mlru, 20);
        assert(multilruGetMaxCount(mlru) == 20);
        assert(multilruNeedsEviction(mlru) == false);

        /* Can now insert more without eviction */
        for (int i = 0; i < 10; i++) {
            multilruInsert(mlru);
        }
        assert(multilruCount(mlru) == 20);
        assert(multilruNeedsEviction(mlru) == false);

        printf("resize - expand count limit: PASSED\n");
    }

    TEST("resize - shrink count limit (gradual eviction)") {
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 200,
            .policy = MLRU_POLICY_COUNT,
            .maxCount = 100,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);
        multilruSetAutoEvict(mlru, false); /* Manual mode for precise control */

        /* Fill to capacity */
        for (int i = 0; i < 100; i++) {
            multilruInsert(mlru);
        }
        assert(multilruCount(mlru) == 100);

        /* Shrink limit - should NOT immediately evict */
        multilruSetMaxCount(mlru, 50);
        assert(multilruGetMaxCount(mlru) == 50);
        assert(multilruCount(mlru) == 100); /* Still 100 entries */
        assert(multilruNeedsEviction(mlru) == true);

        /* Gradual eviction in batches */
        int batches = 0;
        while (multilruNeedsEviction(mlru)) {
            multilruPtr evicted[10];
            multilruEvictN(mlru, evicted, 10);
            batches++;
            /* In production: yield to event loop here */
        }

        assert(multilruCount(mlru) <= 50);
        assert(multilruNeedsEviction(mlru) == false);
        assert(batches > 1); /* Verified batched eviction */

        printf("resize - shrink count limit (gradual eviction): PASSED\n");
    }

    TEST("resize - expand weight limit") {
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 100,
            .policy = MLRU_POLICY_SIZE,
            .maxWeight = 1000,
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        /* Fill to weight capacity */
        for (int i = 0; i < 10; i++) {
            multilruInsertWeighted(mlru, 100);
        }
        assert(multilruTotalWeight(mlru) == 1000);
        assert(multilruNeedsEviction(mlru) == false);

        /* Expand limit */
        multilruSetMaxWeight(mlru, 2000);
        assert(multilruGetMaxWeight(mlru) == 2000);

        /* Can now insert more without eviction */
        for (int i = 0; i < 10; i++) {
            multilruInsertWeighted(mlru, 100);
        }
        assert(multilruTotalWeight(mlru) == 2000);
        assert(multilruNeedsEviction(mlru) == false);

        printf("resize - expand weight limit: PASSED\n");
    }

    TEST("resize - shrink weight limit (gradual eviction)") {
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 200,
            .policy = MLRU_POLICY_SIZE,
            .maxWeight = 10000,
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);
        multilruSetAutoEvict(mlru, false);

        /* Fill to weight capacity */
        for (int i = 0; i < 100; i++) {
            multilruInsertWeighted(mlru, 100);
        }
        assert(multilruTotalWeight(mlru) == 10000);

        /* Shrink limit - should NOT immediately evict */
        multilruSetMaxWeight(mlru, 5000);
        assert(multilruGetMaxWeight(mlru) == 5000);
        assert(multilruTotalWeight(mlru) == 10000); /* Still full */
        assert(multilruNeedsEviction(mlru) == true);

        /* Gradual eviction using evictToSize */
        while (multilruNeedsEviction(mlru)) {
            multilruPtr evicted[10];
            size_t n = multilruEvictToSize(mlru, 5000, evicted, 10);
            if (n == 0) {
                break; /* Safety */
            }
        }

        assert(multilruTotalWeight(mlru) <= 5000);
        assert(multilruNeedsEviction(mlru) == false);

        printf("resize - shrink weight limit (gradual eviction): PASSED\n");
    }

    TEST("resize - multiple resize operations") {
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 200,
            .policy = MLRU_POLICY_COUNT,
            .maxCount = 50,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);
        multilruSetAutoEvict(mlru, false);

        /* Initial fill */
        for (int i = 0; i < 50; i++) {
            multilruInsert(mlru);
        }
        assert(multilruCount(mlru) == 50);

        /* Expand */
        multilruSetMaxCount(mlru, 100);
        for (int i = 0; i < 50; i++) {
            multilruInsert(mlru);
        }
        assert(multilruCount(mlru) == 100);

        /* Shrink below current */
        multilruSetMaxCount(mlru, 30);
        assert(multilruNeedsEviction(mlru) == true);

        /* Partial eviction */
        multilruPtr evicted[20];
        multilruEvictN(mlru, evicted, 20);

        /* Expand again before fully evicted */
        multilruSetMaxCount(mlru, 90);
        assert(multilruNeedsEviction(mlru) == false); /* Now under new limit */

        /* Final shrink */
        multilruSetMaxCount(mlru, 40);
        while (multilruNeedsEviction(mlru)) {
            multilruEvictN(mlru, evicted, 10);
        }
        assert(multilruCount(mlru) <= 40);

        printf("resize - multiple resize operations: PASSED\n");
    }

    TEST("resize - progress tracking") {
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 200,
            .policy = MLRU_POLICY_COUNT,
            .maxCount = 100,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);
        multilruSetAutoEvict(mlru, false);

        /* Fill */
        for (int i = 0; i < 100; i++) {
            multilruInsert(mlru);
        }

        /* Shrink */
        multilruSetMaxCount(mlru, 50);

        /* Track progress */
        size_t initialOver = multilruCount(mlru) - multilruGetMaxCount(mlru);
        assert(initialOver == 50);

        /* Evict in batches, tracking progress */
        while (multilruNeedsEviction(mlru)) {
            size_t current = multilruCount(mlru);
            uint32_t limit = multilruGetMaxCount(mlru);
            size_t remaining = current > limit ? current - limit : 0;

            multilruPtr evicted[10];
            (void)multilruEvictN(mlru, evicted, 10);

            /* Progress should decrease */
            size_t newRemaining =
                multilruCount(mlru) > limit ? multilruCount(mlru) - limit : 0;
            assert(newRemaining <= remaining);
        }

        assert(multilruCount(mlru) <= 50);

        printf("resize - progress tracking: PASSED\n");
    }

    /* ----------------------------------------------------------------
     * Statistics API Tests
     * ---------------------------------------------------------------- */

    TEST("stats - basic counters") {
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 100,
            .policy = MLRU_POLICY_COUNT,
            .maxCount = 50,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);
        multilruSetAutoEvict(mlru, false);

        multilruStats stats;
        multilruGetStats(mlru, &stats);

        /* Initial state */
        assert(stats.count == 0);
        assert(stats.inserts == 0);
        assert(stats.evictions == 0);
        assert(stats.demotions == 0);
        assert(stats.promotions == 0);
        assert(stats.deletes == 0);
        assert(stats.maxCount == 50);
        assert(stats.maxLevels == 7);
        assert(stats.autoEvict == false);
        /* Slot allocation - initially at first usable slot */
        assert(stats.nextFresh == 8); /* maxLevels(7) + 1 */
        assert(stats.freeCount == 0);

        /* Insert some entries */
        multilruPtr ptrs[20];
        for (int i = 0; i < 20; i++) {
            ptrs[i] = multilruInsert(mlru);
        }

        multilruGetStats(mlru, &stats);
        assert(stats.count == 20);
        assert(stats.inserts == 20);
        assert(stats.evictions == 0);
        /* After 20 inserts: nextFresh advanced by 20, no recycled slots */
        assert(stats.nextFresh == 8 + 20);
        assert(stats.freeCount == 0);

        /* Promote entries */
        for (int i = 0; i < 10; i++) {
            multilruIncrease(mlru, ptrs[i]);
        }

        multilruGetStats(mlru, &stats);
        assert(stats.promotions == 10);

        /* Delete some entries */
        for (int i = 0; i < 5; i++) {
            multilruDelete(mlru, ptrs[i]);
        }

        multilruGetStats(mlru, &stats);
        assert(stats.count == 15);
        assert(stats.deletes == 5);
        /* After 5 deletes: nextFresh unchanged, 5 recycled slots */
        assert(stats.nextFresh == 8 + 20);
        assert(stats.freeCount == 5);

        /* Insert 3 more - should recycle from free list */
        for (int i = 0; i < 3; i++) {
            multilruInsert(mlru);
        }

        multilruGetStats(mlru, &stats);
        assert(stats.count == 18);
        /* nextFresh still unchanged (used recycled slots) */
        assert(stats.nextFresh == 8 + 20);
        assert(stats.freeCount == 2); /* 5 - 3 = 2 left */

        printf("stats - basic counters: PASSED\n");
    }

    TEST("stats - eviction and demotion tracking") {
        multilruConfig config = {
            .maxLevels = 4, /* Fewer levels to test demotion chain */
            .startCapacity = 100,
            .policy = MLRU_POLICY_COUNT,
            .maxCount = 10,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);
        multilruSetAutoEvict(mlru, false);

        /* Insert 10 entries */
        multilruPtr ptrs[10];
        for (int i = 0; i < 10; i++) {
            ptrs[i] = multilruInsert(mlru);
        }

        /* Promote all to highest level */
        for (int round = 0; round < 3; round++) {
            for (int i = 0; i < 10; i++) {
                multilruIncrease(mlru, ptrs[i]);
            }
        }

        multilruStats stats;
        multilruGetStats(mlru, &stats);
        assert(stats.promotions == 30);

        /* Evict - should demote first, then evict from level 0 */
        size_t demotionsBefore = stats.demotions;
        size_t evictionsBefore = stats.evictions;

        /* Force eviction until we get a true eviction */
        multilruPtr removed;
        int ops = 0;
        while (multilruRemoveMinimum(mlru, &removed) && ops < 50) {
            ops++;
            if (multilruCount(mlru) < 10) {
                break;
            }
        }

        multilruGetStats(mlru, &stats);
        /* Should have some demotions (entries falling through levels) */
        assert(stats.demotions > demotionsBefore);
        /* Should have at least one eviction */
        assert(stats.evictions > evictionsBefore);

        printf("stats - eviction and demotion tracking: PASSED\n");
    }

    TEST("stats - configuration snapshot") {
        multilruConfig config = {
            .maxLevels = 12,
            .startCapacity = 1000,
            .policy = MLRU_POLICY_SIZE,
            .maxWeight = 50000,
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        multilruStats stats;
        multilruGetStats(mlru, &stats);

        assert(stats.maxLevels == 12);
        assert(stats.maxWeight == 50000);
        assert(stats.autoEvict == true);
        assert(stats.bytesUsed > 0);
        assert(stats.capacity > 0);

        /* Change config and verify stats reflect it */
        multilruSetMaxWeight(mlru, 100000);
        multilruSetAutoEvict(mlru, false);

        multilruGetStats(mlru, &stats);
        assert(stats.maxWeight == 100000);
        assert(stats.autoEvict == false);

        printf("stats - configuration snapshot: PASSED\n");
    }

    TEST("stats - weighted operations") {
        multilruConfig config = {
            .maxLevels = 7,
            .startCapacity = 100,
            .policy = MLRU_POLICY_SIZE,
            .maxWeight = 1000,
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);
        multilruSetAutoEvict(mlru, false);

        /* Insert weighted entries */
        for (int i = 0; i < 10; i++) {
            multilruInsertWeighted(mlru, 100);
        }

        multilruStats stats;
        multilruGetStats(mlru, &stats);
        assert(stats.count == 10);
        assert(stats.totalWeight == 1000);
        assert(stats.inserts == 10);

        printf("stats - weighted operations: PASSED\n");
    }

    TEST("insert heavier than budget never returns a freed handle") {
        /* An insert that exceeds the remaining weight budget can make
         * the fresh entry itself the eviction victim during the
         * post-insert policy pass; Insert must then report 0, not the
         * already-recycled handle */
        multilruConfig config = {
            .maxLevels = 4,
            .maxWeight = 10,
            .policy = MLRU_POLICY_SIZE,
            .enableWeights = true,
        };
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);

        multilruPtr held[2];
        held[0] = multilruInsertWeighted(mlru, 5);
        held[1] = multilruInsertWeighted(mlru, 5);
        /* Promote existing entries off level 0 so the fresh insert is
         * the first true-eviction candidate */
        multilruIncrease(mlru, held[0]);
        multilruIncrease(mlru, held[1]);

        const multilruPtr fresh = multilruInsertWeighted(mlru, 100);
        if (fresh != 0 && !multilruIsPopulated(mlru, fresh)) {
            ERR("Insert returned freed handle %" PRIu64, (uint64_t)fresh);
        }
    }

    TEST("out-of-range handles and levels are refused without effect") {
        /* Unweighted: the weight calls are inert */
        cleaner(multilruFree) multilru *plain = multilruNew();
        const multilruPtr p = multilruInsert(plain);
        multilruUpdateWeight(plain, p, 99);
        if (multilruEvictToSize(plain, 0, NULL, 4) != 0 ||
            multilruGetWeight(plain, p) != 0 ||
            multilruTotalWeight(plain) != 0) {
            ERR("an unweighted LRU took a weight");
        }

        /* Weighted, levels asked past 64 clamp to 64 */
        multilruConfig config = {.maxLevels = 200, .enableWeights = true};
        cleaner(multilruFree) multilru *mlru = multilruNewWithConfig(&config);
        const multilruPtr a = multilruInsertWeighted(mlru, 7);
        const multilruPtr b = multilruInsertWeighted(mlru, 11);
        const uint64_t total = multilruTotalWeight(mlru);
        const multilruPtr bogus[] = {0, (multilruPtr)1 << 40, a + b + 1000};
        for (size_t i = 0; i < countof(bogus); i++) {
            multilruUpdateWeight(mlru, bogus[i], 1000);
            multilruIncrease(mlru, bogus[i]);
            multilruDecrease(mlru, bogus[i]);
            if (multilruGetWeight(mlru, bogus[i]) != 0 ||
                multilruGetLevel(mlru, bogus[i]) != 0) {
                ERR("bogus handle %zu reads a weight or level", i);
            }
        }
        /* A level head is not an entry either: handle 1 is a head */
        multilruUpdateWeight(mlru, 1, 1000);
        if (multilruTotalWeight(mlru) != total || multilruCount(mlru) != 2 ||
            multilruGetWeight(mlru, a) != 7 ||
            multilruGetWeight(mlru, b) != 11) {
            ERR("a refused update changed the weights");
        }
        if (multilruLevelCount(mlru, 63) != 0 ||
            multilruLevelCount(mlru, 64) != 0 ||
            multilruLevelWeight(mlru, 64) != 0 ||
            multilruLevelCount(mlru, 0) != 2 ||
            multilruLevelWeight(mlru, 0) != 18) {
            ERR("level queries past the 64 levels answered");
        }
        /* Valid update moves the totals */
        multilruUpdateWeight(mlru, a, 70);
        if (multilruTotalWeight(mlru) != 81 ||
            multilruLevelWeight(mlru, 0) != 81) {
            ERR("update to 70: total %" PRIu64, multilruTotalWeight(mlru));
        }
    }

    TEST("every entry width runs the same workload identically") {
        /* Widths above 5 normally need 64K to 16T entries.  Upgrading a
         * small cache in place (as growth does) puts every width,
         * including the 9-byte one whose fields are misaligned, through
         * the full workload; the width-5 run is the reference trace. */
        static constexpr uint8_t widths[] = {5, 6, 7, 8, 9, 10, 11, 12, 16};
        cleanerName(zfree, U64s) uint64_t *reference =
            zcalloc(MLRU_WIDTH_TRACE_MAX, sizeof(uint64_t));
        cleanerName(zfree, U64s) uint64_t *trace =
            zcalloc(MLRU_WIDTH_TRACE_MAX, sizeof(uint64_t));
        size_t referenceLen = 0;

        for (size_t w = 0; w < sizeof(widths); w++) {
            cleaner(multilruFree) multilru *mlru =
                multilruNewWithLevelsCapacity(4, 64);
            if (!upgradeWidth(mlru, widths[w])) {
                ERR("upgradeWidth to %u failed", widths[w]);
                continue;
            }

            const size_t len = mlruWidthWorkload_(mlru, trace);
            if (mlru->entryWidth != widths[w]) {
                ERR("width %u became %u during the workload", widths[w],
                    mlru->entryWidth);
            }

            if (!verifyInvariants(mlru, "width workload")) {
                ERR("invariants failed at width %u", widths[w]);
            }

            if (w == 0) {
                memcpy(reference, trace, len * sizeof(*trace));
                referenceLen = len;
            } else if (len != referenceLen ||
                       memcmp(reference, trace, len * sizeof(*trace)) != 0) {
                ERR("width %u trace differs from width 5", widths[w]);
            }
        }

        if (referenceLen < 1000) {
            ERR("workload trace too short (%zu)", referenceLen);
        }
    }

    TEST_FINAL_RESULT;
}
#endif
