#include "stringPool.h"
#include "databox.h"
#include "datakit.h"
#include "fibbuf.h"
#include "multidict.h"

#include <assert.h>
#include <inttypes.h>
#include <string.h>

/* ====================================================================
 * Internal Structure
 * ====================================================================
 *
 * Two-way mapping:
 *   - strToId: multidict hash table (string → ID) for O(1) lookup
 *   - idToStr: dynamic array of entries for O(1) ID→string
 *
 * Entry storage:
 *   - Each entry stores: { string data, refcount }
 *   - Entries are allocated separately and referenced by pointer
 *   - IDs are 1-indexed (0 = invalid/not found)
 *
 * Free list:
 *   - Released IDs go to free list for reuse
 *   - Minimizes ID growth and keeps IDs compact
 */

typedef struct poolEntry {
    uint8_t *data;     /* String bytes (owned, allocated separately) */
    size_t len;        /* String length */
    uint64_t refcount; /* Reference count (0 = free slot) */
} poolEntry;

struct stringPool {
    multidict *strToId;      /* string → ID mapping (O(1) hash lookup) */
    multidictClass *mdClass; /* Class for the multidict (owned) */
    poolEntry *entries;      /* ID → entry array (O(1) index access) */
    size_t capacity;         /* Allocated slots in entries array */
    size_t count;            /* Active (non-free) entries */
    uint64_t nextId;         /* Next fresh ID to allocate */
    uint64_t *freeList;      /* Stack of recycled IDs */
    size_t freeCount;        /* Number of IDs in free list */
    size_t freeCapacity;     /* Allocated size of free list */
};

/* Initial capacities */
#define INITIAL_CAPACITY 16
#define INITIAL_FREE_CAPACITY 16

/* ====================================================================
 * Creation / Destruction
 * ==================================================================== */

stringPool *stringPoolNew(void) {
    stringPool *pool = zcalloc(1, sizeof(*pool));
    if (!pool) {
        return NULL;
    }

    /* Create hash table for string→ID lookup */
    pool->mdClass = multidictDefaultClassNew();
    if (!pool->mdClass) {
        zfree(pool);
        return NULL;
    }

    pool->strToId = multidictNew(&multidictTypeExactKey, pool->mdClass, 0);
    if (!pool->strToId) {
        multidictDefaultClassFree(pool->mdClass);
        zfree(pool);
        return NULL;
    }

    /* Allocate initial entry array */
    pool->entries = zcalloc(INITIAL_CAPACITY, sizeof(poolEntry));
    if (!pool->entries) {
        multidictFree(pool->strToId);
        zfree(pool);
        return NULL;
    }
    pool->capacity = INITIAL_CAPACITY;

    /* Allocate initial free list */
    pool->freeList = zmalloc(INITIAL_FREE_CAPACITY * sizeof(uint64_t));
    if (!pool->freeList) {
        zfree(pool->entries);
        multidictFree(pool->strToId);
        zfree(pool);
        return NULL;
    }
    pool->freeCapacity = INITIAL_FREE_CAPACITY;

    /* IDs start at 1 (0 = invalid) */
    pool->nextId = 1;
    pool->count = 0;
    pool->freeCount = 0;

    return pool;
}

void stringPoolFree(stringPool *pool) {
    if (!pool) {
        return;
    }

    /* Free all string data */
    for (size_t i = 0; i < pool->nextId; i++) {
        if (pool->entries[i].data) {
            zfree(pool->entries[i].data);
        }
    }

    zfree(pool->entries);
    zfree(pool->freeList);
    multidictFree(pool->strToId);
    multidictDefaultClassFree(pool->mdClass);
    zfree(pool);
}

void stringPoolReset(stringPool *pool) {
    if (!pool) {
        return;
    }

    /* Free all string data */
    for (size_t i = 0; i < pool->nextId; i++) {
        if (pool->entries[i].data) {
            zfree(pool->entries[i].data);
            pool->entries[i].data = NULL;
            pool->entries[i].len = 0;
            pool->entries[i].refcount = 0;
        }
    }

    /* Clear hash table */
    multidictEmpty(pool->strToId);

    /* Reset state */
    pool->nextId = 1;
    pool->count = 0;
    pool->freeCount = 0;
}

/* ====================================================================
 * Internal Helpers
 * ==================================================================== */

/* Ensure entry array has space for ID */
static bool ensureCapacity(stringPool *pool, uint64_t id) {
    if (id < pool->capacity) {
        return true;
    }

    size_t newCap = pool->capacity;
    while (newCap <= id) {
        newCap = fibbufNextSizeBuffer(newCap);
    }

    poolEntry *newEntries = zrealloc(pool->entries, newCap * sizeof(poolEntry));
    if (!newEntries) {
        return false;
    }

    /* Zero-init new slots */
    memset(&newEntries[pool->capacity], 0,
           (newCap - pool->capacity) * sizeof(poolEntry));

    pool->entries = newEntries;
    pool->capacity = newCap;
    return true;
}

/* Push ID to free list */
static bool pushFreeId(stringPool *pool, uint64_t id) {
    if (pool->freeCount >= pool->freeCapacity) {
        size_t newCap = fibbufNextSizeBuffer(pool->freeCapacity);
        uint64_t *newList = zrealloc(pool->freeList, newCap * sizeof(uint64_t));
        if (!newList) {
            return false;
        }
        pool->freeList = newList;
        pool->freeCapacity = newCap;
    }
    pool->freeList[pool->freeCount++] = id;
    return true;
}

/* Pop ID from free list (returns 0 if empty) */
static uint64_t popFreeId(stringPool *pool) {
    if (pool->freeCount == 0) {
        return 0;
    }
    return pool->freeList[--pool->freeCount];
}

/* Allocate next available ID */
static uint64_t allocateId(stringPool *pool) {
    /* Prefer recycled IDs */
    uint64_t id = popFreeId(pool);
    if (id != 0) {
        return id;
    }

    /* Allocate fresh ID */
    id = pool->nextId++;
    if (!ensureCapacity(pool, id)) {
        pool->nextId--;
        return 0;
    }
    return id;
}

/* ====================================================================
 * Interning Operations
 * ==================================================================== */

uint64_t stringPoolIntern(stringPool *pool, const databox *str) {
    if (!pool || !str) {
        return 0;
    }

    /* Check if already interned */
    databox existingId;
    if (multidictFind(pool->strToId, str, &existingId)) {
        /* Already exists - increment refcount and return */
        uint64_t id = existingId.data.u;
        pool->entries[id].refcount++;
        return id;
    }

    /* Allocate new ID */
    uint64_t id = allocateId(pool);
    if (id == 0) {
        return 0;
    }

    /* Copy string data */
    const uint8_t *srcBytes = databoxBytes(str);
    size_t len = str->len;

    uint8_t *data = zmalloc(len);
    if (!data) {
        pushFreeId(pool, id);
        return 0;
    }
    memcpy(data, srcBytes, len);

    /* Store entry */
    pool->entries[id].data = data;
    pool->entries[id].len = len;
    pool->entries[id].refcount = 1;

    /* Add to hash table: string → ID */
    databox keyBox = *str;
    /* Point to our owned copy */
    keyBox.data.bytes.start = data;

    databox valBox = {.type = DATABOX_UNSIGNED_64, .data.u = id};
    multidictAdd(pool->strToId, &keyBox, &valBox);

    pool->count++;
    return id;
}

uint64_t stringPoolGetId(const stringPool *pool, const databox *str) {
    if (!pool || !str) {
        return 0;
    }

    databox existingId;
    if (multidictFind(pool->strToId, str, &existingId)) {
        return existingId.data.u;
    }
    return 0;
}

bool stringPoolExists(const stringPool *pool, const databox *str) {
    return stringPoolGetId(pool, str) != 0;
}

/* ====================================================================
 * Lookup Operations
 * ==================================================================== */

bool stringPoolLookup(const stringPool *pool, uint64_t id, databox *str) {
    if (!pool || !str || id == 0 || id >= pool->nextId) {
        return false;
    }

    poolEntry *entry = &pool->entries[id];
    if (entry->refcount == 0 || entry->data == NULL) {
        return false;
    }

    str->type = DATABOX_BYTES;
    str->len = entry->len;
    str->data.bytes.start = entry->data;
    return true;
}

/* ====================================================================
 * Reference Counting
 * ==================================================================== */

void stringPoolRetain(stringPool *pool, uint64_t id) {
    if (!pool || id == 0 || id >= pool->nextId) {
        return;
    }

    poolEntry *entry = &pool->entries[id];
    if (entry->refcount > 0) {
        entry->refcount++;
    }
}

bool stringPoolRelease(stringPool *pool, uint64_t id) {
    if (!pool || id == 0 || id >= pool->nextId) {
        return false;
    }

    poolEntry *entry = &pool->entries[id];
    if (entry->refcount == 0) {
        return false;
    }

    entry->refcount--;
    if (entry->refcount == 0) {
        /* Remove from hash table */
        databox keyBox = {.type = DATABOX_BYTES,
                          .len = entry->len,
                          .data.bytes.start = entry->data};
        multidictDelete(pool->strToId, &keyBox);

        /* Free string data */
        zfree(entry->data);
        entry->data = NULL;
        entry->len = 0;

        /* Recycle ID */
        pushFreeId(pool, id);
        pool->count--;

        return true; /* Entry was freed */
    }

    return false; /* Entry still active */
}

uint64_t stringPoolRefcount(const stringPool *pool, uint64_t id) {
    if (!pool || id == 0 || id >= pool->nextId) {
        return 0;
    }
    return pool->entries[id].refcount;
}

/* ====================================================================
 * Statistics
 * ==================================================================== */

size_t stringPoolCount(const stringPool *pool) {
    return pool ? pool->count : 0;
}

size_t stringPoolBytes(const stringPool *pool) {
    if (!pool) {
        return 0;
    }

    size_t bytes = sizeof(stringPool);
    bytes += pool->capacity * sizeof(poolEntry);
    bytes += pool->freeCapacity * sizeof(uint64_t);
    bytes += multidictBytes(pool->strToId);

    /* Add string data bytes */
    for (size_t i = 1; i < pool->nextId; i++) {
        if (pool->entries[i].data) {
            bytes += pool->entries[i].len;
        }
    }

    return bytes;
}

/* ====================================================================
 * Testing
 * ==================================================================== */

#ifdef DATAKIT_TEST
#include "ctest.h"
#include "multimapAtom.h"
#include "timeUtil.h"
#include <stdio.h>

void stringPoolRepr(const stringPool *pool) {
    printf("stringPool: count=%zu capacity=%zu nextId=%" PRIu64
           " freeCount=%zu\n",
           pool->count, pool->capacity, pool->nextId, pool->freeCount);

    for (size_t i = 1; i < pool->nextId; i++) {
        poolEntry *e = &pool->entries[i];
        if (e->refcount > 0) {
            printf("  [%zu] refcount=%" PRIu64 " len=%zu data='%.*s'\n", i,
                   e->refcount, e->len, (int)e->len, e->data);
        }
    }
}

int stringPoolTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    TEST("create / free") {
        stringPool *pool = stringPoolNew();
        assert(pool != NULL);
        assert(stringPoolCount(pool) == 0);
        stringPoolFree(pool);
    }

    TEST("basic intern and lookup") {
        stringPool *pool = stringPoolNew();

        databox str1 = databoxNewBytesString("hello");
        databox str2 = databoxNewBytesString("world");

        uint64_t id1 = stringPoolIntern(pool, &str1);
        uint64_t id2 = stringPoolIntern(pool, &str2);

        assert(id1 != 0);
        assert(id2 != 0);
        assert(id1 != id2);
        assert(stringPoolCount(pool) == 2);

        /* Lookup by ID */
        databox result;
        bool found1 = stringPoolLookup(pool, id1, &result);
        assert(found1 == true);
        assert(result.len == 5);
        assert(memcmp(result.data.bytes.start, "hello", 5) == 0);

        bool found2 = stringPoolLookup(pool, id2, &result);
        assert(found2 == true);
        assert(result.len == 5);
        assert(memcmp(result.data.bytes.start, "world", 5) == 0);

        /* Lookup by string */
        assert(stringPoolGetId(pool, &str1) == id1);
        assert(stringPoolGetId(pool, &str2) == id2);

        stringPoolFree(pool);
    }

    TEST("duplicate intern returns same ID and increments refcount") {
        stringPool *pool = stringPoolNew();

        databox str = databoxNewBytesString("duplicate");

        uint64_t id1 = stringPoolIntern(pool, &str);
        assert(stringPoolRefcount(pool, id1) == 1);

        uint64_t id2 = stringPoolIntern(pool, &str);
        assert(id2 == id1);
        assert(stringPoolRefcount(pool, id1) == 2);

        uint64_t id3 = stringPoolIntern(pool, &str);
        assert(id3 == id1);
        assert(stringPoolRefcount(pool, id1) == 3);

        assert(stringPoolCount(pool) == 1);

        stringPoolFree(pool);
    }

    TEST("retain and release") {
        stringPool *pool = stringPoolNew();

        databox str = databoxNewBytesString("refcounted");
        uint64_t id = stringPoolIntern(pool, &str);
        assert(stringPoolRefcount(pool, id) == 1);

        stringPoolRetain(pool, id);
        assert(stringPoolRefcount(pool, id) == 2);

        stringPoolRetain(pool, id);
        assert(stringPoolRefcount(pool, id) == 3);

        /* Release doesn't free until refcount=0 */
        assert(stringPoolRelease(pool, id) == false);
        assert(stringPoolRefcount(pool, id) == 2);

        assert(stringPoolRelease(pool, id) == false);
        assert(stringPoolRefcount(pool, id) == 1);

        /* Final release frees the entry */
        assert(stringPoolRelease(pool, id) == true);
        assert(stringPoolRefcount(pool, id) == 0);
        assert(stringPoolCount(pool) == 0);

        /* Lookup should fail now */
        databox result;
        assert(stringPoolLookup(pool, id, &result) == false);
        assert(stringPoolExists(pool, &str) == false);

        stringPoolFree(pool);
    }

    TEST("ID recycling") {
        stringPool *pool = stringPoolNew();

        databox str1 = databoxNewBytesString("first");
        databox str2 = databoxNewBytesString("second");
        databox str3 = databoxNewBytesString("third");

        uint64_t id1 = stringPoolIntern(pool, &str1);
        uint64_t id2 = stringPoolIntern(pool, &str2);
        uint64_t id3 = stringPoolIntern(pool, &str3);

        /* Release middle one */
        stringPoolRelease(pool, id2);
        assert(stringPoolCount(pool) == 2);

        /* New intern should reuse id2 */
        databox str4 = databoxNewBytesString("fourth");
        uint64_t id4 = stringPoolIntern(pool, &str4);
        assert(id4 == id2); /* Recycled! */
        assert(stringPoolCount(pool) == 3);

        /* Verify all lookups still work */
        databox result;
        assert(stringPoolLookup(pool, id1, &result) == true);
        assert(stringPoolLookup(pool, id3, &result) == true);
        assert(stringPoolLookup(pool, id4, &result) == true);
        assert(memcmp(result.data.bytes.start, "fourth", 6) == 0);

        stringPoolFree(pool);
    }

    TEST("reset") {
        stringPool *pool = stringPoolNew();

        for (int i = 0; i < 100; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "string%d", i);
            databox str = databoxNewBytesString(buf);
            stringPoolIntern(pool, &str);
        }
        assert(stringPoolCount(pool) == 100);

        stringPoolReset(pool);
        assert(stringPoolCount(pool) == 0);

        /* Should be able to add new entries */
        databox str = databoxNewBytesString("after_reset");
        uint64_t id = stringPoolIntern(pool, &str);
        assert(id == 1); /* IDs reset too */
        assert(stringPoolCount(pool) == 1);

        stringPoolFree(pool);
    }

    TEST("stress test - 10K unique strings") {
        stringPool *pool = stringPoolNew();
        const size_t N = 10000;

        int64_t startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < N; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "key%08zu", i);
            databox str = databoxNewBytesString(buf);
            uint64_t id = stringPoolIntern(pool, &str);
            assert(id != 0);
        }
        int64_t insertNs = timeUtilMonotonicNs() - startNs;
        printf("Insert: %zu strings in %.3f ms (%.0f/sec)\n", N, insertNs / 1e6,
               N / (insertNs / 1e9));

        assert(stringPoolCount(pool) == N);

        /* Lookup by string */
        startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < N; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "key%08zu", i);
            databox str = databoxNewBytesString(buf);
            uint64_t id = stringPoolGetId(pool, &str);
            assert(id != 0);
        }
        int64_t lookupStrNs = timeUtilMonotonicNs() - startNs;
        printf("Lookup by string: %zu in %.3f ms (%.0f/sec)\n", N,
               lookupStrNs / 1e6, N / (lookupStrNs / 1e9));

        /* Lookup by ID */
        startNs = timeUtilMonotonicNs();
        for (size_t i = 1; i <= N; i++) {
            databox result;
            bool found = stringPoolLookup(pool, i, &result);
            assert(found);
        }
        int64_t lookupIdNs = timeUtilMonotonicNs() - startNs;
        printf("Lookup by ID: %zu in %.3f ms (%.0f/sec)\n", N, lookupIdNs / 1e6,
               N / (lookupIdNs / 1e9));

        printf("Memory: %zu bytes for %zu entries (%.2f bytes/entry)\n",
               stringPoolBytes(pool), stringPoolCount(pool),
               (double)stringPoolBytes(pool) / stringPoolCount(pool));

        stringPoolFree(pool);
    }

    TEST("stress test - duplicates with refcounting") {
        stringPool *pool = stringPoolNew();
        const size_t UNIQUE = 1000;
        const size_t REFS_PER = 10;

        /* Intern each string multiple times */
        for (size_t r = 0; r < REFS_PER; r++) {
            for (size_t i = 0; i < UNIQUE; i++) {
                char buf[32];
                snprintf(buf, sizeof(buf), "dup%08zu", i);
                databox str = databoxNewBytesString(buf);
                stringPoolIntern(pool, &str);
            }
        }

        assert(stringPoolCount(pool) == UNIQUE);

        /* Verify refcounts */
        for (size_t i = 1; i <= UNIQUE; i++) {
            assert(stringPoolRefcount(pool, i) == REFS_PER);
        }

        /* Release all refs except one */
        for (size_t r = 0; r < REFS_PER - 1; r++) {
            for (size_t i = 1; i <= UNIQUE; i++) {
                stringPoolRelease(pool, i);
            }
        }
        assert(stringPoolCount(pool) == UNIQUE);

        /* Final release should free all */
        for (size_t i = 1; i <= UNIQUE; i++) {
            bool freed = stringPoolRelease(pool, i);
            assert(freed == true);
        }
        assert(stringPoolCount(pool) == 0);

        stringPoolFree(pool);
    }

    TEST("performance benchmark summary") {
        printf("\n=== STRING POOL PERFORMANCE SUMMARY ===\n");

        stringPool *pool = stringPoolNew();
        const size_t N = 50000;

        /* Insert unique strings */
        int64_t startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < N; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "bench%08zu", i);
            databox str = databoxNewBytesString(buf);
            stringPoolIntern(pool, &str);
        }
        int64_t insertNs = timeUtilMonotonicNs() - startNs;

        /* Lookup by string */
        startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < N; i++) {
            char buf[32];
            snprintf(buf, sizeof(buf), "bench%08zu", i);
            databox str = databoxNewBytesString(buf);
            stringPoolGetId(pool, &str);
        }
        int64_t lookupStrNs = timeUtilMonotonicNs() - startNs;

        /* Lookup by ID */
        startNs = timeUtilMonotonicNs();
        for (size_t i = 1; i <= N; i++) {
            databox result;
            stringPoolLookup(pool, i, &result);
        }
        int64_t lookupIdNs = timeUtilMonotonicNs() - startNs;

        /* Retain */
        startNs = timeUtilMonotonicNs();
        for (size_t i = 1; i <= N; i++) {
            stringPoolRetain(pool, i);
        }
        int64_t retainNs = timeUtilMonotonicNs() - startNs;

        printf("Insert rate:       %.0f ops/sec (%.1f us/op)\n",
               N / (insertNs / 1e9), insertNs / (double)N / 1000);
        printf("Lookup (by str):   %.0f ops/sec (%.1f us/op)\n",
               N / (lookupStrNs / 1e9), lookupStrNs / (double)N / 1000);
        printf("Lookup (by ID):    %.0f ops/sec (%.1f us/op)\n",
               N / (lookupIdNs / 1e9), lookupIdNs / (double)N / 1000);
        printf("Retain rate:       %.0f ops/sec (%.1f us/op)\n",
               N / (retainNs / 1e9), retainNs / (double)N / 1000);
        printf("Memory used:       %zu bytes (%.2f bytes/entry)\n",
               stringPoolBytes(pool),
               (double)stringPoolBytes(pool) / stringPoolCount(pool));
        printf("=========================================\n\n");

        stringPoolFree(pool);
    }

    TEST("COMPARISON: stringPool vs multimapAtom") {
        printf("\n");
        printf("╔══════════════════════════════════════════════════════════════"
               "═╗\n");
        printf("║     STRING INTERNING COMPARISON: stringPool vs multimapAtom  "
               " ║\n");
        printf("╚══════════════════════════════════════════════════════════════"
               "═╝\n\n");

        const size_t N = 50000;
        char buf[32];

        /* ============ stringPool benchmarks ============ */
        stringPool *sp = stringPoolNew();

        /* Insert */
        int64_t spInsertStart = timeUtilMonotonicNs();
        for (size_t i = 0; i < N; i++) {
            snprintf(buf, sizeof(buf), "key%08zu", i);
            databox str = databoxNewBytesString(buf);
            stringPoolIntern(sp, &str);
        }
        int64_t spInsertNs = timeUtilMonotonicNs() - spInsertStart;

        /* Lookup by string */
        int64_t spLookupStrStart = timeUtilMonotonicNs();
        for (size_t i = 0; i < N; i++) {
            snprintf(buf, sizeof(buf), "key%08zu", i);
            databox str = databoxNewBytesString(buf);
            stringPoolGetId(sp, &str);
        }
        int64_t spLookupStrNs = timeUtilMonotonicNs() - spLookupStrStart;

        /* Lookup by ID */
        int64_t spLookupIdStart = timeUtilMonotonicNs();
        for (size_t i = 1; i <= N; i++) {
            databox result;
            stringPoolLookup(sp, i, &result);
        }
        int64_t spLookupIdNs = timeUtilMonotonicNs() - spLookupIdStart;

        /* Retain */
        int64_t spRetainStart = timeUtilMonotonicNs();
        for (size_t i = 1; i <= N; i++) {
            stringPoolRetain(sp, i);
        }
        int64_t spRetainNs = timeUtilMonotonicNs() - spRetainStart;

        /* Release */
        int64_t spReleaseStart = timeUtilMonotonicNs();
        for (size_t i = 1; i <= N; i++) {
            stringPoolRelease(sp, i);
        }
        int64_t spReleaseNs = timeUtilMonotonicNs() - spReleaseStart;

        size_t spBytes = stringPoolBytes(sp);
        size_t spCount = stringPoolCount(sp);

        stringPoolFree(sp);

        /* ============ multimapAtom benchmarks ============ */
        multimapAtom *ma = multimapAtomNew();

        /* Insert */
        int64_t maInsertStart = timeUtilMonotonicNs();
        for (size_t i = 0; i < N; i++) {
            snprintf(buf, sizeof(buf), "key%08zu", i);
            databox str = databoxNewBytesString(buf);
            multimapAtomInsert(ma, &str);
        }
        int64_t maInsertNs = timeUtilMonotonicNs() - maInsertStart;

        /* Lookup by string (get reference) */
        int64_t maLookupStrStart = timeUtilMonotonicNs();
        for (size_t i = 0; i < N; i++) {
            snprintf(buf, sizeof(buf), "key%08zu", i);
            databox str = databoxNewBytesString(buf);
            databox ref;
            multimapAtomLookupReference(ma, &str, &ref);
        }
        int64_t maLookupStrNs = timeUtilMonotonicNs() - maLookupStrStart;

        /* Lookup by ID - need to get actual refs first */
        int64_t maLookupIdStart = timeUtilMonotonicNs();
        for (size_t i = 0; i < N; i++) {
            snprintf(buf, sizeof(buf), "key%08zu", i);
            databox str = databoxNewBytesString(buf);
            databox ref;
            if (multimapAtomLookupReference(ma, &str, &ref)) {
                databox key;
                multimapAtomLookup(ma, &ref, &key);
            }
        }
        int64_t maLookupIdNs = timeUtilMonotonicNs() - maLookupIdStart;

        /* Retain - use key-based retain */
        int64_t maRetainStart = timeUtilMonotonicNs();
        for (size_t i = 0; i < N; i++) {
            snprintf(buf, sizeof(buf), "key%08zu", i);
            databox str = databoxNewBytesString(buf);
            multimapAtomRetain(ma, &str);
        }
        int64_t maRetainNs = timeUtilMonotonicNs() - maRetainStart;

        /* Release */
        int64_t maReleaseStart = timeUtilMonotonicNs();
        for (size_t i = 0; i < N; i++) {
            snprintf(buf, sizeof(buf), "key%08zu", i);
            databox str = databoxNewBytesString(buf);
            multimapAtomRelease(ma, &str);
        }
        int64_t maReleaseNs = timeUtilMonotonicNs() - maReleaseStart;

        size_t maBytes = multimapAtomBytes(ma);
        size_t maCount = multimapAtomCount(ma);

        multimapAtomFree(ma);

        /* ============ Print comparison ============ */
        printf("Workload: %zu unique strings, 14-byte keys\n\n", N);

        /* Helper macro for M ops/s formatting */
#define MOPS(rate) ((rate) / 1e6)

        printf("┌─────────────────┬─────────────────────┬─────────────────────┬"
               "─────────┐\n");
        printf("│ Operation       │ stringPool (O(1))   │ "
               "multimapAtom(O(lgN))│ Ratio   │\n");
        printf("├─────────────────┼─────────────────────┼─────────────────────┼"
               "─────────┤\n");

        double spInsertRate = N / (spInsertNs / 1e9);
        double maInsertRate = N / (maInsertNs / 1e9);
        printf("│ Insert          │ %8.2f M ops/s    │ %8.2f M ops/s    │ "
               "%5.1fx  │\n",
               MOPS(spInsertRate), MOPS(maInsertRate),
               spInsertRate / maInsertRate);

        double spLookupStrRate = N / (spLookupStrNs / 1e9);
        double maLookupStrRate = N / (maLookupStrNs / 1e9);
        printf("│ Lookup (string) │ %8.2f M ops/s    │ %8.2f M ops/s    │ "
               "%5.1fx  │\n",
               MOPS(spLookupStrRate), MOPS(maLookupStrRate),
               spLookupStrRate / maLookupStrRate);

        double spLookupIdRate = spLookupIdNs > 0 ? N / (spLookupIdNs / 1e9) : 0;
        double maLookupIdRate = maLookupIdNs > 0 ? N / (maLookupIdNs / 1e9) : 0;
        if (spLookupIdNs == 0 || spLookupIdRate > 1e9) {
            printf("│ Lookup (by ID)  │   >1000 M ops/s    │ %8.2f M ops/s    "
                   "│  >999x  │\n",
                   MOPS(maLookupIdRate));
        } else {
            printf("│ Lookup (by ID)  │ %8.2f M ops/s    │ %8.2f M ops/s    │ "
                   "%5.1fx  │\n",
                   MOPS(spLookupIdRate), MOPS(maLookupIdRate),
                   spLookupIdRate / maLookupIdRate);
        }

        double spRetainRate = spRetainNs > 0 ? N / (spRetainNs / 1e9) : 0;
        double maRetainRate = maRetainNs > 0 ? N / (maRetainNs / 1e9) : 0;
        if (spRetainNs == 0 || spRetainRate > 1e9) {
            printf("│ Retain          │   >1000 M ops/s    │ %8.2f M ops/s    "
                   "│  >999x  │\n",
                   MOPS(maRetainRate));
        } else {
            printf("│ Retain          │ %8.2f M ops/s    │ %8.2f M ops/s    │ "
                   "%5.1fx  │\n",
                   MOPS(spRetainRate), MOPS(maRetainRate),
                   spRetainRate / maRetainRate);
        }

        double spReleaseRate = spReleaseNs > 0 ? N / (spReleaseNs / 1e9) : 0;
        double maReleaseRate = maReleaseNs > 0 ? N / (maReleaseNs / 1e9) : 0;
        if (spReleaseNs == 0 || spReleaseRate > 1e9) {
            printf("│ Release         │   >1000 M ops/s    │ %8.2f M ops/s    "
                   "│  >999x  │\n",
                   MOPS(maReleaseRate));
        } else {
            printf("│ Release         │ %8.2f M ops/s    │ %8.2f M ops/s    │ "
                   "%5.1fx  │\n",
                   MOPS(spReleaseRate), MOPS(maReleaseRate),
                   spReleaseRate / maReleaseRate);
        }

        printf("├─────────────────┼─────────────────────┼─────────────────────┼"
               "─────────┤\n");

        double spBytesPerEntry = (double)spBytes / spCount;
        double maBytesPerEntry = (double)maBytes / maCount;
        printf(
            "│ Memory/entry    │ %11.1f bytes   │ %11.1f bytes   │ %5.1fx  │\n",
            spBytesPerEntry, maBytesPerEntry,
            spBytesPerEntry / maBytesPerEntry);

        printf("│ Total memory    │ %8.2f MB        │ %8.2f MB        │ %5.1fx "
               " │\n",
               (double)spBytes / (1024 * 1024), (double)maBytes / (1024 * 1024),
               (double)spBytes / maBytes);

        printf("└─────────────────┴─────────────────────┴─────────────────────┴"
               "─────────┘\n\n");

#undef MOPS

        printf("=== ARCHITECTURE & USAGE RECOMMENDATIONS ===\n\n");

        printf("1. COMBINING stringPool WITH multiOrderedSet:\n");
        printf("   "
               "┌─────────────────────────────────────────────────────────────┐"
               "\n");
        printf("   │  Client Code                                              "
               "  │\n");
        printf("   │     │                                                     "
               "  │\n");
        printf("   │     ▼                                                     "
               "  │\n");
        printf("   │  stringPool (shared)  ◄──────────────────────┐            "
               "  │\n");
        printf("   │     │ intern: string → ID (O(1))             │            "
               "  │\n");
        printf("   │     │ lookup: ID → string (O(1))             │            "
               "  │\n");
        printf("   │     ▼                                        │            "
               "  │\n");
        printf("   │  multiOrderedSet (pool mode)                 │            "
               "  │\n");
        printf("   │     └─ stores IDs instead of strings ────────┘            "
               "  │\n");
        printf("   │     └─ memberIndex: ID → score (O(1) hash)                "
               "  │\n");
        printf("   │     └─ scoreMap: score → ID (O(log n) tree)               "
               "  │\n");
        printf("   "
               "└─────────────────────────────────────────────────────────────┘"
               "\n\n");

        printf("2. OPTIMAL SETUP FOR FASTEST OPERATIONS:\n");
        printf("   // Create shared pool for multiple ordered sets\n");
        printf("   stringPool *pool = stringPoolNew();\n");
        printf("   \n");
        printf("   // Create ordered sets that share the pool\n");
        printf("   multiOrderedSetFull *set1 = "
               "multiOrderedSetFullNewWithPool(pool);\n");
        printf("   multiOrderedSetFull *set2 = "
               "multiOrderedSetFullNewWithPool(pool);\n");
        printf("   \n");
        printf("   // HOT PATH: Use IDs directly for internal operations\n");
        printf(
            "   uint64_t id = stringPoolIntern(pool, &memberStr);  // Once\n");
        printf("   // ... pass 'id' around, not the string ...\n");
        printf("   \n");
        printf("   // COLD PATH: Convert back to string only for "
               "display/output\n");
        printf("   stringPoolLookup(pool, id, &displayStr);\n\n");

        printf("3. PERFORMANCE CHARACTERISTICS:\n");
        printf(
            "   "
            "┌─────────────────────┬─────────────────────────────────────┐\n");
        printf("   │ Operation           │ Performance                         "
               "│\n");
        printf(
            "   "
            "├─────────────────────┼─────────────────────────────────────┤\n");
        printf("   │ Add member          │ O(1) intern + O(log n) insert       "
               "│\n");
        printf("   │ Check exists (str)  │ O(1) hash lookup                    "
               "│\n");
        printf("   │ Check exists (ID)   │ O(1) array lookup (200+ M ops/s)    "
               "│\n");
        printf("   │ Get score           │ O(1) hash lookup                    "
               "│\n");
        printf("   │ Get rank            │ O(n) - unavoidable for sorted sets  "
               "│\n");
        printf("   │ Union/Intersect     │ O(n) with ID dedup (fast)           "
               "│\n");
        printf("   "
               "└─────────────────────┴─────────────────────────────────────┘\n"
               "\n");

        printf("4. WHEN TO USE POOL MODE:\n");
        printf("   [YES] Long member strings (>20 bytes) - saves memory\n");
        printf("   [YES] Same members across multiple sets - shared storage\n");
        printf("   [YES] Frequent exists/score lookups - O(1) is crucial\n");
        printf("   [YES] Union/intersection operations - ID comparison fast\n");
        printf("   [NO]  Short strings (<10 bytes) - ID overhead dominates\n");
        printf("   [NO]  Single-use sets - no sharing benefit\n");
        printf(
            "   [NO]  Memory-constrained + small data - inline is smaller\n\n");

        printf("5. MEMORY TRADE-OFF GUIDANCE:\n");
        printf("   - Pool overhead per entry: ~24 bytes + ID storage\n");
        printf("   - Break-even string length: ~18-20 bytes\n");
        printf(
            "   - Sharing multiplier: N sets sharing = N-1 copies saved\n\n");
    }

    TEST_FINAL_RESULT;
}
#endif
