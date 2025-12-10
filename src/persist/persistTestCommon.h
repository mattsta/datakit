/* persistTestCommon.h - Shared test infrastructure for persistent wrappers
 *
 * Provides comprehensive, well-encapsulated test utilities that ensure
 * thorough coverage across all persistent data structure wrappers.
 */

#pragma once

#ifdef DATAKIT_TEST

#include "../ctest.h"
#include "../databox.h"
#include <string.h>
#include <unistd.h>

/* ============================================================================
 * Test Data Generators
 * ============================================================================
 */

/* Generate varied test databoxes covering all common types */
typedef enum {
    PTEST_TYPE_SIGNED_SMALL, /* Small signed integers (-128 to 127) */
    PTEST_TYPE_SIGNED_LARGE, /* Large signed integers */
    PTEST_TYPE_UNSIGNED,     /* Unsigned integers */
    PTEST_TYPE_FLOAT,        /* Float values */
    PTEST_TYPE_DOUBLE,       /* Double values */
    PTEST_TYPE_BYTES_SHORT,  /* Short byte strings (1-8 bytes) */
    PTEST_TYPE_BYTES_MEDIUM, /* Medium byte strings (9-64 bytes) */
    PTEST_TYPE_TRUE,         /* Boolean true */
    PTEST_TYPE_FALSE,        /* Boolean false */
    PTEST_TYPE_NULL,         /* Null value */
    PTEST_TYPE_COUNT
} ptestDataType;

/* Generate a databox of specified type with given seed for variation */
static inline void ptestGenerateBox(databox *box, ptestDataType type,
                                    int seed) {
    switch (type) {
    case PTEST_TYPE_SIGNED_SMALL:
        *box = DATABOX_SIGNED((int8_t)(seed % 256 - 128));
        break;
    case PTEST_TYPE_SIGNED_LARGE:
        *box = DATABOX_SIGNED((int64_t)seed * 1000000LL + seed);
        break;
    case PTEST_TYPE_UNSIGNED:
        *box = DATABOX_UNSIGNED((uint64_t)seed * 12345ULL);
        break;
    case PTEST_TYPE_FLOAT:
        DATABOX_SET_FLOAT(box, (float)seed * 3.14159f);
        break;
    case PTEST_TYPE_DOUBLE:
        *box = DATABOX_DOUBLE((double)seed * 2.718281828);
        break;
    case PTEST_TYPE_BYTES_SHORT:
        /* Use stack buffer - caller must use immediately */
        box->type = DATABOX_BYTES;
        box->len = (seed % 8) + 1;
        box->data.bytes.start = (uint8_t *)"TESTDATA";
        break;
    case PTEST_TYPE_BYTES_MEDIUM:
        box->type = DATABOX_BYTES;
        box->len = (seed % 48) + 9; /* 9-56 bytes (string is 56 bytes long) */
        box->data.bytes.start =
            (uint8_t
                 *)"TESTDATA_MEDIUM_LENGTH_STRING_FOR_TESTING_PURPOSES_HERE";
        break;
    case PTEST_TYPE_TRUE:
        box->type = DATABOX_TRUE;
        break;
    case PTEST_TYPE_FALSE:
        box->type = DATABOX_FALSE;
        break;
    case PTEST_TYPE_NULL:
        box->type = DATABOX_NULL;
        break;
    default:
        *box = DATABOX_SIGNED(seed);
        break;
    }
}

/* Generate varied integer for intset testing */
static inline int64_t ptestGenerateInt(int seed, int variation) {
    switch (variation % 5) {
    case 0:
        return (int64_t)seed; /* Simple */
    case 1:
        return (int64_t)seed * -1; /* Negative */
    case 2:
        return (int64_t)seed * 1000000LL; /* Large */
    case 3:
        return INT64_MIN + seed; /* Near min */
    case 4:
        return INT64_MAX - seed; /* Near max */
    default:
        return seed;
    }
}

/* ============================================================================
 * Path Utilities
 * ============================================================================
 */

static inline void ptestCleanupFiles(const char *basePath) {
    char snapPath[512], walPath[512];
    snprintf(snapPath, sizeof(snapPath), "%s.snap", basePath);
    snprintf(walPath, sizeof(walPath), "%s.wal", basePath);
    unlink(snapPath);
    unlink(walPath);
}

/* ============================================================================
 * Verification Utilities
 * ============================================================================
 */

/* Compare two databoxes for equality - use datakit's native comparison */
static inline bool ptestBoxesEqual(const databox *a, const databox *b) {
    return databoxEqual(a, b);
}

/* ============================================================================
 * Test Sequence Generators
 * ============================================================================
 */

typedef enum {
    PTEST_SEQ_LINEAR,      /* 0, 1, 2, 3, ... */
    PTEST_SEQ_REVERSE,     /* n, n-1, n-2, ... */
    PTEST_SEQ_RANDOM_ISH,  /* Pseudo-random pattern */
    PTEST_SEQ_ALTERNATING, /* 0, n, 1, n-1, 2, n-2, ... */
    PTEST_SEQ_POWERS,      /* 1, 2, 4, 8, 16, ... */
} ptestSequence;

static inline int ptestGetSeqValue(ptestSequence seq, int index, int max) {
    switch (seq) {
    case PTEST_SEQ_LINEAR:
        return index;
    case PTEST_SEQ_REVERSE:
        return max - index - 1;
    case PTEST_SEQ_RANDOM_ISH:
        return (index * 7919 + 104729) %
               max; /* Large primes for pseudo-random */
    case PTEST_SEQ_ALTERNATING:
        return (index % 2 == 0) ? (index / 2) : (max - 1 - index / 2);
    case PTEST_SEQ_POWERS:
        return (1 << (index % 20)) % max;
    default:
        return index;
    }
}

/* ============================================================================
 * Recovery Test Pattern
 *
 * Standard pattern for testing persistence:
 * 1. Create structure
 * 2. Perform operations
 * 3. Close (sync to disk)
 * 4. Re-open (recover from disk)
 * 5. Verify all data
 * 6. Perform more operations
 * 7. Close again
 * 8. Re-open again
 * 9. Verify all data again
 * ============================================================================
 */

#define PTEST_RECOVERY_CYCLES 3 /* Number of close/reopen cycles to test */

/* ============================================================================
 * Integer Test Values for intsetP
 *
 * Comprehensive set of edge case integers for thorough testing
 * ============================================================================
 */

/* Edge case integer values for testing */
static const int64_t PTEST_INT_EDGE_CASES[] = {
    0, /* Zero */
    1,
    -1, /* Near zero */
    127,
    128,
    -128,
    -129, /* int8 boundaries */
    255,
    256, /* uint8 boundary */
    32767,
    32768,
    -32768,
    -32769, /* int16 boundaries */
    65535,
    65536, /* uint16 boundary */
    2147483647LL,
    2147483648LL,
    -2147483648LL,
    -2147483649LL, /* int32 boundaries */
    4294967295LL,
    4294967296LL, /* uint32 boundary */
    INT64_MAX,
    INT64_MIN, /* int64 boundaries */
    INT64_MAX - 1,
    INT64_MIN + 1, /* Near int64 boundaries */
};
#define PTEST_INT_EDGE_COUNT (sizeof(PTEST_INT_EDGE_CASES) / sizeof(int64_t))

/* Generate a comprehensive set of integers covering ranges */
static inline int64_t ptestIntByRange(int index) {
    /* Cycle through different ranges */
    int range = index % 8;
    int val = index / 8;

    switch (range) {
    case 0:
        return val; /* Small positive */
    case 1:
        return -val - 1; /* Small negative */
    case 2:
        return (int64_t)val * 100; /* Medium positive */
    case 3:
        return (int64_t)val * -100; /* Medium negative */
    case 4:
        return (int64_t)val * 100000; /* Large positive */
    case 5:
        return (int64_t)val * -100000; /* Large negative */
    case 6:
        return INT64_MAX - val; /* Near max */
    case 7:
        return INT64_MIN + val; /* Near min */
    default:
        return val;
    }
}

/* ============================================================================
 * Sorted Tracking for Verification
 *
 * For intsets which maintain sorted order, track expected values
 * ============================================================================
 */

#define PTEST_MAX_TRACKED 2048

typedef struct {
    int64_t values[PTEST_MAX_TRACKED];
    uint32_t count;
} ptestIntTracker;

static inline void ptestTrackerInit(ptestIntTracker *t) {
    t->count = 0;
}

/* Add value in sorted order (like intset does) */
static inline bool ptestTrackerAdd(ptestIntTracker *t, int64_t value) {
    if (t->count >= PTEST_MAX_TRACKED) {
        return false;
    }

    /* Find insertion point */
    uint32_t pos = 0;
    while (pos < t->count && t->values[pos] < value) {
        pos++;
    }

    /* Check for duplicate */
    if (pos < t->count && t->values[pos] == value) {
        return false; /* Already exists */
    }

    /* Shift to make room */
    memmove(&t->values[pos + 1], &t->values[pos],
            (t->count - pos) * sizeof(int64_t));

    t->values[pos] = value;
    t->count++;
    return true;
}

/* Remove value */
static inline bool ptestTrackerRemove(ptestIntTracker *t, int64_t value) {
    for (uint32_t i = 0; i < t->count; i++) {
        if (t->values[i] == value) {
            memmove(&t->values[i], &t->values[i + 1],
                    (t->count - i - 1) * sizeof(int64_t));
            t->count--;
            return true;
        }
        if (t->values[i] > value) {
            return false; /* Not found (sorted) */
        }
    }
    return false;
}

static inline bool ptestTrackerContains(const ptestIntTracker *t,
                                        int64_t value) {
    /* Binary search since sorted */
    int32_t lo = 0, hi = (int32_t)t->count - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        if (t->values[mid] == value) {
            return true;
        } else if (t->values[mid] < value) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return false;
}

/* ============================================================================
 * Databox List Tracking for Verification
 *
 * For flex/multilist which maintain ordered lists
 * ============================================================================
 */

typedef struct {
    databox boxes[PTEST_MAX_TRACKED];
    uint32_t count;
} ptestBoxTracker;

static inline void ptestBoxTrackerInit(ptestBoxTracker *t) {
    t->count = 0;
}

static inline bool ptestBoxTrackerPushHead(ptestBoxTracker *t,
                                           const databox *box) {
    if (t->count >= PTEST_MAX_TRACKED) {
        return false;
    }
    memmove(&t->boxes[1], &t->boxes[0], t->count * sizeof(databox));
    t->boxes[0] = *box;
    t->count++;
    return true;
}

static inline bool ptestBoxTrackerPushTail(ptestBoxTracker *t,
                                           const databox *box) {
    if (t->count >= PTEST_MAX_TRACKED) {
        return false;
    }
    t->boxes[t->count++] = *box;
    return true;
}

static inline bool ptestBoxTrackerPopHead(ptestBoxTracker *t, databox *box) {
    if (t->count == 0) {
        return false;
    }
    *box = t->boxes[0];
    memmove(&t->boxes[0], &t->boxes[1], (t->count - 1) * sizeof(databox));
    t->count--;
    return true;
}

static inline bool ptestBoxTrackerPopTail(ptestBoxTracker *t, databox *box) {
    if (t->count == 0) {
        return false;
    }
    t->count--;
    *box = t->boxes[t->count];
    return true;
}

static inline bool ptestBoxTrackerGet(const ptestBoxTracker *t, uint32_t index,
                                      databox *box) {
    if (index >= t->count) {
        return false;
    }
    *box = t->boxes[index];
    return true;
}

/* ============================================================================
 * Key-Value Tracking for Verification
 *
 * For multimap/multidict key-value structures
 * ============================================================================
 */

typedef struct {
    databox keys[PTEST_MAX_TRACKED];
    databox values[PTEST_MAX_TRACKED];
    uint32_t count;
} ptestKVTracker;

static inline void ptestKVTrackerInit(ptestKVTracker *t) {
    t->count = 0;
}

/* Find index of key, or -1 if not found */
static inline int32_t ptestKVTrackerFindKey(const ptestKVTracker *t,
                                            const databox *key) {
    for (uint32_t i = 0; i < t->count; i++) {
        if (ptestBoxesEqual(&t->keys[i], key)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static inline bool ptestKVTrackerInsert(ptestKVTracker *t, const databox *key,
                                        const databox *value) {
    /* Check for existing key */
    int32_t existing = ptestKVTrackerFindKey(t, key);
    if (existing >= 0) {
        /* Update existing */
        t->values[existing] = *value;
        return true;
    }

    if (t->count >= PTEST_MAX_TRACKED) {
        return false;
    }

    t->keys[t->count] = *key;
    t->values[t->count] = *value;
    t->count++;
    return true;
}

static inline bool ptestKVTrackerDelete(ptestKVTracker *t, const databox *key) {
    int32_t idx = ptestKVTrackerFindKey(t, key);
    if (idx < 0) {
        return false;
    }

    memmove(&t->keys[idx], &t->keys[idx + 1],
            (t->count - idx - 1) * sizeof(databox));
    memmove(&t->values[idx], &t->values[idx + 1],
            (t->count - idx - 1) * sizeof(databox));
    t->count--;
    return true;
}

static inline bool ptestKVTrackerLookup(const ptestKVTracker *t,
                                        const databox *key, databox *value) {
    int32_t idx = ptestKVTrackerFindKey(t, key);
    if (idx < 0) {
        return false;
    }
    *value = t->values[idx];
    return true;
}

/* ============================================================================
 * Static String Pool for Testing
 *
 * Pre-allocated strings for bytes testing without memory management
 * ============================================================================
 */

static const char *PTEST_STRINGS[] = {
    "a",
    "bb",
    "ccc",
    "dddd",
    "eeeee",
    "hello",
    "world",
    "testing",
    "persistence",
    "short",
    "medium_length_string_here",
    "this_is_a_longer_string_for_testing_byte_sequences",
    "UPPERCASE",
    "MixedCase",
    "with spaces here",
    "special!@#$%chars",
    "unicode\xc3\xa9test",
    "", /* empty string */
};
#define PTEST_STRING_COUNT (sizeof(PTEST_STRINGS) / sizeof(char *))

static inline void ptestGenerateStringBox(databox *box, int seed) {
    const char *str = PTEST_STRINGS[seed % PTEST_STRING_COUNT];
    box->type = DATABOX_BYTES;
    box->len = strlen(str);
    box->data.bytes.start = (uint8_t *)str;
}

/* ============================================================================
 * Verification Macros
 *
 * Macros for common verification patterns with detailed error messages
 * ============================================================================
 */

#define PTEST_VERIFY_COUNT(structure, expected, getter)                        \
    do {                                                                       \
        uint32_t actual = getter(structure);                                   \
        if (actual != (expected)) {                                            \
            ERR("Count mismatch: expected %u, got %u", (unsigned)(expected),   \
                (unsigned)actual);                                             \
        }                                                                      \
    } while (0)

#define PTEST_VERIFY_CONTAINS(structure, value, finder)                        \
    do {                                                                       \
        if (!finder(structure, value)) {                                       \
            ERR("Value %lld should exist but not found", (long long)(value));  \
        }                                                                      \
    } while (0)

#define PTEST_VERIFY_NOT_CONTAINS(structure, value, finder)                    \
    do {                                                                       \
        if (finder(structure, value)) {                                        \
            ERR("Value %lld should not exist but was found",                   \
                (long long)(value));                                           \
        }                                                                      \
    } while (0)

/* ============================================================================
 * Operation Mix Generators
 *
 * Generate realistic operation sequences for testing
 * ============================================================================
 */

typedef enum {
    PTEST_OP_INSERT,
    PTEST_OP_DELETE,
    PTEST_OP_LOOKUP,
    PTEST_OP_COUNT
} ptestOpType;

/* Generate operation type based on seed - weighted towards inserts */
static inline ptestOpType ptestGetOpType(int seed, int totalOps) {
    int threshold = seed % 100;

    /* First half of test: mostly inserts */
    if (seed < totalOps / 2) {
        if (threshold < 70) {
            return PTEST_OP_INSERT;
        }
        if (threshold < 90) {
            return PTEST_OP_LOOKUP;
        }
        return PTEST_OP_DELETE;
    }
    /* Second half: more balanced */
    if (threshold < 40) {
        return PTEST_OP_INSERT;
    }
    if (threshold < 70) {
        return PTEST_OP_LOOKUP;
    }
    return PTEST_OP_DELETE;
}

#endif /* DATAKIT_TEST */
