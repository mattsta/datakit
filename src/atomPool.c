#include "atomPool.h"
#include "datakit.h"
#include "multimapAtom.h"
#include "stringPool.h"
#include "timeUtil.h"

#include <assert.h>

/* ====================================================================
 * VTable - Function Pointer Interface for Backend Polymorphism
 * ==================================================================== */

typedef struct atomPoolVTable {
    uint64_t (*intern)(void *impl, const databox *str);
    uint64_t (*getId)(const void *impl, const databox *str);
    bool (*exists)(const void *impl, const databox *str);
    bool (*lookup)(const void *impl, uint64_t id, databox *str);
    void (*retain)(void *impl, uint64_t id);
    bool (*release)(void *impl, uint64_t id);
    uint64_t (*refcount)(const void *impl, uint64_t id);
    size_t (*count)(const void *impl);
    size_t (*bytes)(const void *impl);
    void (*reset)(void *impl);
    void (*free)(void *impl);
} atomPoolVTable;

struct atomPool {
    void *impl;                   /* Backend implementation */
    const atomPoolVTable *vtable; /* Function pointers */
    atomPoolType type;            /* Backend type */
};

/* ====================================================================
 * stringPool Backend Wrappers
 * ==================================================================== */

static uint64_t hashIntern(void *impl, const databox *str) {
    return stringPoolIntern((stringPool *)impl, str);
}

static uint64_t hashGetId(const void *impl, const databox *str) {
    return stringPoolGetId((const stringPool *)impl, str);
}

static bool hashExists(const void *impl, const databox *str) {
    return stringPoolExists((const stringPool *)impl, str);
}

static bool hashLookup(const void *impl, uint64_t id, databox *str) {
    return stringPoolLookup((const stringPool *)impl, id, str);
}

static void hashRetain(void *impl, uint64_t id) {
    stringPoolRetain((stringPool *)impl, id);
}

static bool hashRelease(void *impl, uint64_t id) {
    return stringPoolRelease((stringPool *)impl, id);
}

static uint64_t hashRefcount(const void *impl, uint64_t id) {
    return stringPoolRefcount((const stringPool *)impl, id);
}

static size_t hashCount(const void *impl) {
    return stringPoolCount((const stringPool *)impl);
}

static size_t hashBytes(const void *impl) {
    return stringPoolBytes((const stringPool *)impl);
}

static void hashReset(void *impl) {
    stringPoolReset((stringPool *)impl);
}

static void hashFree(void *impl) {
    stringPoolFree((stringPool *)impl);
}

static const atomPoolVTable hashVTable = {
    .intern = hashIntern,
    .getId = hashGetId,
    .exists = hashExists,
    .lookup = hashLookup,
    .retain = hashRetain,
    .release = hashRelease,
    .refcount = hashRefcount,
    .count = hashCount,
    .bytes = hashBytes,
    .reset = hashReset,
    .free = hashFree,
};

/* ====================================================================
 * multimapAtom Backend Wrappers
 * ==================================================================== */

/* Helper macro to create a proper atom reference databox.
 * multimapAtom uses DATABOX_CONTAINER_REFERENCE_EXTERNAL with data.u */
#define ATOM_REF(id)                                                           \
    (databox) {                                                                \
        .data.u = (id), .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL           \
    }

/* multimapAtom uses 0-based atom IDs, but atomPool uses 0 as "invalid/error".
 * We offset TREE backend IDs by +1 to make them 1-based externally. */
#define TREE_ID_TO_EXTERNAL(id) ((id) + 1)
#define TREE_ID_FROM_EXTERNAL(id) ((id) - 1)

static uint64_t treeIntern(void *impl, const databox *str) {
    multimapAtom *ma = (multimapAtom *)impl;
    databox key = *str;

    /* InsertIfNewConvertAndRetain: inserts if new, retains if exists.
     * After this call, key is converted to a
     * DATABOX_CONTAINER_REFERENCE_EXTERNAL with the atom ID in data.u */
    multimapAtomInsertIfNewConvertAndRetain(ma, &key);

    /* key is now converted to contain the atom ID in data.u.
     * Offset by +1 so 0 becomes 1 (since 0 means "error" in atomPool API) */
    return TREE_ID_TO_EXTERNAL(key.data.u);
}

static uint64_t treeGetId(const void *impl, const databox *str) {
    const multimapAtom *ma = (const multimapAtom *)impl;
    databox atomRef;

    if (multimapAtomLookupReference(ma, str, &atomRef)) {
        return TREE_ID_TO_EXTERNAL(atomRef.data.u);
    }
    return 0;
}

static bool treeExists(const void *impl, const databox *str) {
    const multimapAtom *ma = (const multimapAtom *)impl;
    databox atomRef;
    return multimapAtomLookupReference(ma, str, &atomRef);
}

static bool treeLookup(const void *impl, uint64_t id, databox *str) {
    const multimapAtom *ma = (const multimapAtom *)impl;
    databox ref = ATOM_REF(TREE_ID_FROM_EXTERNAL(id));
    return multimapAtomLookup(ma, &ref, str);
}

static void treeRetain(void *impl, uint64_t id) {
    multimapAtom *ma = (multimapAtom *)impl;
    multimapAtomRetainById(ma, TREE_ID_FROM_EXTERNAL(id));
}

static bool treeRelease(void *impl, uint64_t id) {
    multimapAtom *ma = (multimapAtom *)impl;
    databox ref = ATOM_REF(TREE_ID_FROM_EXTERNAL(id));
    return multimapAtomReleaseById(ma, &ref);
}

static uint64_t treeRefcount(const void *impl, uint64_t id) {
    const multimapAtom *ma = (const multimapAtom *)impl;
    databox ref = ATOM_REF(TREE_ID_FROM_EXTERNAL(id));
    databox key;

    if (multimapAtomLookup(ma, &ref, &key)) {
        /* multimapAtomLookupRefcount expects the atom reference (ID), not key.
         * mapAtomForward is indexed by atom ID: {ID, Key, Refcount} */
        databox count;
        multimapAtomLookupRefcount(ma, &ref, &count);
        /* multimapAtom uses 0-based internal refcounts for memory efficiency
         * (DATABOX_FALSE = 1 byte vs 3 bytes for number 1).
         * Translate to 1-based for consistent API: internal 0 = 1 ref. */
        return count.data.u + 1;
    }
    return 0;
}

static size_t treeCount(const void *impl) {
    return multimapAtomCount((const multimapAtom *)impl);
}

static size_t treeBytes(const void *impl) {
    return multimapAtomBytes((const multimapAtom *)impl);
}

static void treeReset(void *impl) {
    (void)impl;
    /* TREE backend reset is not implemented - multimapAtom doesn't support it.
     * Callers should free and create a new pool instead.
     * For now, this is a no-op that leaves the pool unchanged. */
}

static void treeFree(void *impl) {
    multimapAtomFree((multimapAtom *)impl);
}

static const atomPoolVTable treeVTable = {
    .intern = treeIntern,
    .getId = treeGetId,
    .exists = treeExists,
    .lookup = treeLookup,
    .retain = treeRetain,
    .release = treeRelease,
    .refcount = treeRefcount,
    .count = treeCount,
    .bytes = treeBytes,
    .reset = treeReset,
    .free = treeFree,
};

/* ====================================================================
 * Public API Implementation
 * ==================================================================== */

atomPool *atomPoolNew(atomPoolType type) {
    atomPool *pool = zcalloc(1, sizeof(*pool));
    if (!pool) {
        return NULL;
    }

    pool->type = type;

    switch (type) {
    case ATOM_POOL_HASH:
        pool->impl = stringPoolNew();
        pool->vtable = &hashVTable;
        break;
    case ATOM_POOL_TREE:
        pool->impl = multimapAtomNew();
        pool->vtable = &treeVTable;
        break;
    default:
        zfree(pool);
        return NULL;
    }

    if (!pool->impl) {
        zfree(pool);
        return NULL;
    }

    return pool;
}

atomPool *atomPoolNewDefault(void) {
    return atomPoolNew(ATOM_POOL_HASH);
}

void atomPoolFree(atomPool *pool) {
    if (!pool) {
        return;
    }

    if (pool->impl && pool->vtable) {
        pool->vtable->free(pool->impl);
    }

    zfree(pool);
}

void atomPoolReset(atomPool *pool) {
    if (pool && pool->impl && pool->vtable) {
        pool->vtable->reset(pool->impl);
    }
}

uint64_t atomPoolIntern(atomPool *pool, const databox *str) {
    if (!pool || !pool->impl || !str) {
        return 0;
    }
    return pool->vtable->intern(pool->impl, str);
}

uint64_t atomPoolGetId(const atomPool *pool, const databox *str) {
    if (!pool || !pool->impl || !str) {
        return 0;
    }
    return pool->vtable->getId(pool->impl, str);
}

bool atomPoolExists(const atomPool *pool, const databox *str) {
    if (!pool || !pool->impl || !str) {
        return false;
    }
    return pool->vtable->exists(pool->impl, str);
}

bool atomPoolLookup(const atomPool *pool, uint64_t id, databox *str) {
    if (!pool || !pool->impl || id == 0) {
        return false;
    }
    return pool->vtable->lookup(pool->impl, id, str);
}

void atomPoolRetain(atomPool *pool, uint64_t id) {
    if (pool && pool->impl && id != 0) {
        pool->vtable->retain(pool->impl, id);
    }
}

bool atomPoolRelease(atomPool *pool, uint64_t id) {
    if (!pool || !pool->impl || id == 0) {
        return false;
    }
    return pool->vtable->release(pool->impl, id);
}

uint64_t atomPoolRefcount(const atomPool *pool, uint64_t id) {
    if (!pool || !pool->impl || id == 0) {
        return 0;
    }
    return pool->vtable->refcount(pool->impl, id);
}

size_t atomPoolCount(const atomPool *pool) {
    if (!pool || !pool->impl) {
        return 0;
    }
    return pool->vtable->count(pool->impl);
}

size_t atomPoolBytes(const atomPool *pool) {
    if (!pool || !pool->impl) {
        return 0;
    }
    return pool->vtable->bytes(pool->impl);
}

atomPoolType atomPoolGetType(const atomPool *pool) {
    return pool ? pool->type : ATOM_POOL_HASH;
}

const char *atomPoolTypeName(atomPoolType type) {
    switch (type) {
    case ATOM_POOL_HASH:
        return "HASH (stringPool)";
    case ATOM_POOL_TREE:
        return "TREE (multimapAtom)";
    default:
        return "UNKNOWN";
    }
}

/* ====================================================================
 * Testing
 * ==================================================================== */

#ifdef DATAKIT_TEST

#include "ctest.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define DOUBLE_NEWLINE 0
#include "perf.h"

void atomPoolRepr(const atomPool *pool) {
    if (!pool) {
        printf("atomPool: (null)\n");
        return;
    }
    printf("atomPool: type=%s count=%zu bytes=%zu\n",
           atomPoolTypeName(pool->type), atomPoolCount(pool),
           atomPoolBytes(pool));
}

int atomPoolTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;

    /* Test both backends */
    atomPoolType types[] = {ATOM_POOL_HASH, ATOM_POOL_TREE};
    const char *typeNames[] = {"HASH", "TREE"};

    for (int t = 0; t < 2; t++) {
        atomPoolType type = types[t];
        printf("\n=== Testing %s backend ===\n", typeNames[t]);

        TEST("atomPool basic operations") {
            atomPool *pool = atomPoolNew(type);
            if (!pool) {
                ERRR("Failed to create atomPool");
            }

            /* Intern some strings */
            databox s1 = databoxNewBytesString("hello");
            databox s2 = databoxNewBytesString("world");
            databox s3 = databoxNewBytesString("hello"); /* duplicate */

            uint64_t id1 = atomPoolIntern(pool, &s1);
            uint64_t id2 = atomPoolIntern(pool, &s2);
            uint64_t id3 = atomPoolIntern(pool, &s3);

            if (id1 == 0 || id2 == 0 || id3 == 0) {
                ERRR("Intern returned 0");
            }

            if (id1 != id3) {
                ERR("Duplicate string got different ID: %" PRIu64
                    " vs %" PRIu64,
                    id1, id3);
            }

            if (id1 == id2) {
                ERRR("Different strings got same ID");
            }

            /* Check count */
            if (atomPoolCount(pool) != 2) {
                ERR("Count should be 2, got %zu", atomPoolCount(pool));
            }

            /* Lookup by ID */
            databox resolved = {0};
            if (!atomPoolLookup(pool, id1, &resolved)) {
                ERR("Lookup by ID failed for id %" PRIu64, id1);
                goto cleanup_basic;
            }
            if (resolved.len != s1.len ||
                memcmp(resolved.data.ptr, s1.data.ptr, s1.len) != 0) {
                ERRR("Lookup returned wrong string");
            }

            /* GetId without retain */
            uint64_t lookupId = atomPoolGetId(pool, &s1);
            if (lookupId != id1) {
                ERRR("GetId returned wrong ID");
            }

            /* Exists */
            if (!atomPoolExists(pool, &s1)) {
                ERRR("Exists returned false for interned string");
            }
            databox notExist = databoxNewBytesString("nothere");
            if (atomPoolExists(pool, &notExist)) {
                ERRR("Exists returned true for non-existent string");
            }

            /* Refcount: hello was interned twice */
            uint64_t rc = atomPoolRefcount(pool, id1);
            if (rc != 2) {
                ERR("Refcount should be 2, got %" PRIu64, rc);
            }

            /* Release once */
            atomPoolRelease(pool, id1);
            rc = atomPoolRefcount(pool, id1);
            if (rc != 1) {
                ERR("Refcount after release should be 1, got %" PRIu64, rc);
            }

            /* Retain */
            atomPoolRetain(pool, id1);
            rc = atomPoolRefcount(pool, id1);
            if (rc != 2) {
                ERR("Refcount after retain should be 2, got %" PRIu64, rc);
            }

        cleanup_basic:
            atomPoolFree(pool);
        }

        TEST("atomPool reset") {
            atomPool *pool = atomPoolNew(type);
            if (!pool) {
                ERRR("Failed to create atomPool");
            }

            databox s1 = databoxNewBytesString("test1");
            databox s2 = databoxNewBytesString("test2");
            atomPoolIntern(pool, &s1);
            atomPoolIntern(pool, &s2);

            if (atomPoolCount(pool) != 2) {
                ERRR("Count before reset should be 2");
            }

            atomPoolReset(pool);

            /* TREE backend doesn't support reset, so skip the count check */
            if (type == ATOM_POOL_HASH) {
                if (atomPoolCount(pool) != 0) {
                    ERR("Count after reset should be 0, got %zu",
                        atomPoolCount(pool));
                }
            } else {
                /* TREE reset is a no-op, count stays the same */
                printf(
                    "  (TREE backend reset not supported, skipping check)\n");
            }

            atomPoolFree(pool);
        }
        TEST("atomPool refcount lifecycle") {
            atomPool *pool = atomPoolNew(type);
            if (!pool) {
                ERRR("Failed to create atomPool");
            }

            databox s1 = databoxNewBytesString("refcount_test");

            /* Initial intern: refcount = 1 */
            uint64_t id1 = atomPoolIntern(pool, &s1);
            if (atomPoolRefcount(pool, id1) != 1) {
                ERR("Initial refcount should be 1, got %" PRIu64,
                    atomPoolRefcount(pool, id1));
            }

            /* Second intern of same string: refcount = 2 */
            uint64_t id2 = atomPoolIntern(pool, &s1);
            if (id1 != id2) {
                ERRR("Same string should get same ID");
            }
            if (atomPoolRefcount(pool, id1) != 2) {
                ERR("Refcount after second intern should be 2, got %" PRIu64,
                    atomPoolRefcount(pool, id1));
            }

            /* Multiple retains: refcount = 5 */
            atomPoolRetain(pool, id1);
            atomPoolRetain(pool, id1);
            atomPoolRetain(pool, id1);
            if (atomPoolRefcount(pool, id1) != 5) {
                ERR("Refcount after 3 retains should be 5, got %" PRIu64,
                    atomPoolRefcount(pool, id1));
            }

            /* Multiple releases: refcount = 2 */
            atomPoolRelease(pool, id1);
            atomPoolRelease(pool, id1);
            atomPoolRelease(pool, id1);
            if (atomPoolRefcount(pool, id1) != 2) {
                ERR("Refcount after 3 releases should be 2, got %" PRIu64,
                    atomPoolRefcount(pool, id1));
            }

            /* Verify entry still exists and is correct */
            databox resolved = {0};
            if (!atomPoolLookup(pool, id1, &resolved)) {
                ERRR("Lookup failed after releases");
            }
            if (resolved.len != s1.len ||
                memcmp(resolved.data.ptr, s1.data.ptr, s1.len) != 0) {
                ERRR("Lookup returned wrong string");
            }

            atomPoolFree(pool);
        }

        TEST("atomPool release to deletion") {
            atomPool *pool = atomPoolNew(type);
            if (!pool) {
                ERRR("Failed to create atomPool");
            }

            databox s1 = databoxNewBytesString("delete_me");

            /* Intern once */
            uint64_t id1 = atomPoolIntern(pool, &s1);
            if (atomPoolCount(pool) != 1) {
                ERR("Count should be 1, got %zu", atomPoolCount(pool));
            }

            /* Release should delete the entry */
            atomPoolRelease(pool, id1);

            /* Entry should no longer exist */
            if (atomPoolCount(pool) != 0) {
                ERR("Count should be 0 after release, got %zu",
                    atomPoolCount(pool));
            }

            /* Lookup should fail */
            databox resolved = {0};
            if (atomPoolLookup(pool, id1, &resolved)) {
                ERRR("Lookup should fail for deleted entry");
            }

            /* GetId should return 0 for deleted entry */
            if (atomPoolGetId(pool, &s1) != 0) {
                ERRR("GetId should return 0 for deleted entry");
            }

            /* Exists should return false */
            if (atomPoolExists(pool, &s1)) {
                ERRR("Exists should return false for deleted entry");
            }

            /* Re-interning should work and give a new (possibly same) ID */
            uint64_t id2 = atomPoolIntern(pool, &s1);
            if (id2 == 0) {
                ERRR("Re-intern returned 0");
            }
            if (atomPoolCount(pool) != 1) {
                ERR("Count should be 1 after re-intern, got %zu",
                    atomPoolCount(pool));
            }

            atomPoolFree(pool);
        }

        TEST("atomPool GetId vs Intern") {
            atomPool *pool = atomPoolNew(type);
            if (!pool) {
                ERRR("Failed to create atomPool");
            }

            databox s1 = databoxNewBytesString("getid_test");

            /* GetId on non-existent entry should return 0 */
            uint64_t id0 = atomPoolGetId(pool, &s1);
            if (id0 != 0) {
                ERR("GetId on non-existent should return 0, got %" PRIu64, id0);
            }

            /* GetId should not create entry */
            if (atomPoolCount(pool) != 0) {
                ERR("GetId should not create entry, count=%zu",
                    atomPoolCount(pool));
            }

            /* Now intern it */
            uint64_t id1 = atomPoolIntern(pool, &s1);
            if (atomPoolRefcount(pool, id1) != 1) {
                ERR("After intern, refcount should be 1, got %" PRIu64,
                    atomPoolRefcount(pool, id1));
            }

            /* GetId should return same ID without incrementing refcount */
            uint64_t id2 = atomPoolGetId(pool, &s1);
            if (id2 != id1) {
                ERR("GetId returned different ID: %" PRIu64 " vs %" PRIu64, id2,
                    id1);
            }
            if (atomPoolRefcount(pool, id1) != 1) {
                ERR("GetId should not change refcount, got %" PRIu64,
                    atomPoolRefcount(pool, id1));
            }

            /* Intern should increment refcount */
            uint64_t id3 = atomPoolIntern(pool, &s1);
            if (id3 != id1) {
                ERRR("Intern returned different ID");
            }
            if (atomPoolRefcount(pool, id1) != 2) {
                ERR("Intern should increment refcount to 2, got %" PRIu64,
                    atomPoolRefcount(pool, id1));
            }

            atomPoolFree(pool);
        }

        TEST("atomPool edge cases") {
            atomPool *pool = atomPoolNew(type);
            if (!pool) {
                ERRR("Failed to create atomPool");
            }

            /* Empty string */
            databox empty = databoxNewBytesString("");
            uint64_t emptyId = atomPoolIntern(pool, &empty);
            if (emptyId == 0) {
                ERRR("Intern of empty string returned 0");
            }

            databox emptyResolved = {0};
            if (!atomPoolLookup(pool, emptyId, &emptyResolved)) {
                ERRR("Lookup of empty string failed");
            }
            if (emptyResolved.len != 0) {
                ERR("Empty string should have len 0, got %" PRIu64,
                    (uint64_t)emptyResolved.len);
            }

            /* Long string */
            char longStr[512];
            memset(longStr, 'X', sizeof(longStr) - 1);
            longStr[sizeof(longStr) - 1] = '\0';
            databox longBox = databoxNewBytesString(longStr);
            uint64_t longId = atomPoolIntern(pool, &longBox);
            if (longId == 0) {
                ERRR("Intern of long string returned 0");
            }

            databox longResolved = {0};
            if (!atomPoolLookup(pool, longId, &longResolved)) {
                ERRR("Lookup of long string failed");
            }
            if (longResolved.len != sizeof(longStr) - 1) {
                ERR("Long string len mismatch: %" PRIu64 " vs %zu",
                    (uint64_t)longResolved.len, sizeof(longStr) - 1);
            }

            /* Invalid ID should not crash and return appropriate values */
            if (atomPoolRefcount(pool, 0) != 0) {
                ERRR("Refcount of ID 0 should be 0");
            }
            if (atomPoolRefcount(pool, 999999) != 0) {
                ERRR("Refcount of non-existent ID should be 0");
            }

            databox invalidResolved = {0};
            if (atomPoolLookup(pool, 0, &invalidResolved)) {
                ERRR("Lookup of ID 0 should fail");
            }
            if (atomPoolLookup(pool, 999999, &invalidResolved)) {
                ERRR("Lookup of non-existent ID should fail");
            }

            atomPoolFree(pool);
        }

        TEST("atomPool many entries with refcounts") {
            atomPool *pool = atomPoolNew(type);
            if (!pool) {
                ERRR("Failed to create atomPool");
            }

            const size_t N = 1000;
            uint64_t *ids = zmalloc(N * sizeof(uint64_t));
            char buf[32];

            /* Intern N unique strings */
            for (size_t i = 0; i < N; i++) {
                snprintf(buf, sizeof(buf), "entry_%04zu", i);
                databox box = databoxNewBytesString(buf);
                ids[i] = atomPoolIntern(pool, &box);
                if (ids[i] == 0) {
                    ERR("Intern failed for entry %zu", i);
                    goto cleanup_many;
                }
            }

            if (atomPoolCount(pool) != N) {
                ERR("Count should be %zu, got %zu", N, atomPoolCount(pool));
            }

            /* Verify all refcounts are 1 */
            for (size_t i = 0; i < N; i++) {
                uint64_t rc = atomPoolRefcount(pool, ids[i]);
                if (rc != 1) {
                    ERR("Entry %zu refcount should be 1, got %" PRIu64, i, rc);
                }
            }

            /* Retain every other entry */
            for (size_t i = 0; i < N; i += 2) {
                atomPoolRetain(pool, ids[i]);
            }

            /* Verify refcounts: even indices = 2, odd = 1 */
            for (size_t i = 0; i < N; i++) {
                uint64_t expected = (i % 2 == 0) ? 2 : 1;
                uint64_t rc = atomPoolRefcount(pool, ids[i]);
                if (rc != expected) {
                    ERR("Entry %zu refcount should be %" PRIu64
                        ", got %" PRIu64,
                        i, expected, rc);
                }
            }

            /* Release all once - odd entries should be deleted */
            for (size_t i = 0; i < N; i++) {
                atomPoolRelease(pool, ids[i]);
            }

            /* Even entries remain (refcount 1), odd deleted */
            if (atomPoolCount(pool) != N / 2) {
                ERR("After release, count should be %zu, got %zu", N / 2,
                    atomPoolCount(pool));
            }

            /* Verify even entries still exist with refcount 1 */
            for (size_t i = 0; i < N; i += 2) {
                uint64_t rc = atomPoolRefcount(pool, ids[i]);
                if (rc != 1) {
                    ERR("Even entry %zu refcount should be 1, got %" PRIu64, i,
                        rc);
                }
            }

            /* Verify odd entries are deleted */
            for (size_t i = 1; i < N; i += 2) {
                databox resolved = {0};
                if (atomPoolLookup(pool, ids[i], &resolved)) {
                    ERR("Odd entry %zu should be deleted", i);
                }
            }

        cleanup_many:
            zfree(ids);
            atomPoolFree(pool);
        }
    }

    /* Performance comparison */
    TEST("atomPool performance comparison") {
        printf("\n=== Performance Comparison: HASH vs TREE ===\n\n");

        const size_t N = 50000;

        /* Prepare test data */
        databox *keys = zmalloc(N * sizeof(databox));
        char *keyData = zmalloc(N * 20);
        for (size_t i = 0; i < N; i++) {
            int len = snprintf(keyData + i * 20, 20, "testkey%08zu", i);
            keys[i] = databoxNewBytesAllowEmbed(keyData + i * 20, len);
        }

        atomPool *hashPool = atomPoolNew(ATOM_POOL_HASH);
        atomPool *treePool = atomPoolNew(ATOM_POOL_TREE);

        uint64_t *ids = zmalloc(N * sizeof(uint64_t));

        /* Benchmark insert */
        uint64_t hashInsertStart = timeUtilUs();
        for (size_t i = 0; i < N; i++) {
            ids[i] = atomPoolIntern(hashPool, &keys[i]);
        }
        uint64_t hashInsertUs = timeUtilUs() - hashInsertStart;

        uint64_t treeInsertStart = timeUtilUs();
        for (size_t i = 0; i < N; i++) {
            atomPoolIntern(treePool, &keys[i]);
        }
        uint64_t treeInsertUs = timeUtilUs() - treeInsertStart;

        /* Benchmark lookup by ID */
        databox resolved;
        uint64_t hashLookupStart = timeUtilUs();
        for (size_t i = 0; i < N; i++) {
            atomPoolLookup(hashPool, ids[i], &resolved);
        }
        uint64_t hashLookupUs = timeUtilUs() - hashLookupStart;

        uint64_t treeLookupStart = timeUtilUs();
        for (size_t i = 0; i < N; i++) {
            atomPoolLookup(treePool, ids[i], &resolved);
        }
        uint64_t treeLookupUs = timeUtilUs() - treeLookupStart;

#define MOPS(n, us) ((double)(n) / (double)(us))

        printf("┌─────────────────┬─────────────────┬─────────────────┬────────"
               "─┐\n");
        printf("│ Operation       │ HASH (O(1))     │ TREE (O(log n)) │ Ratio  "
               " │\n");
        printf("├─────────────────┼─────────────────┼─────────────────┼────────"
               "─┤\n");
        printf("│ Intern          │ %8.2f M/s    │ %8.2f M/s    │ %5.1fx  │\n",
               MOPS(N, hashInsertUs), MOPS(N, treeInsertUs),
               (double)treeInsertUs / hashInsertUs);
        printf("│ Lookup (by ID)  │ %8.2f M/s    │ %8.2f M/s    │ %5.1fx  │\n",
               MOPS(N, hashLookupUs), MOPS(N, treeLookupUs),
               (double)treeLookupUs / hashLookupUs);
        printf("├─────────────────┼─────────────────┼─────────────────┼────────"
               "─┤\n");
        printf("│ Memory          │ %8.2f MB     │ %8.2f MB     │ %5.1fx  │\n",
               (double)atomPoolBytes(hashPool) / (1024 * 1024),
               (double)atomPoolBytes(treePool) / (1024 * 1024),
               (double)atomPoolBytes(hashPool) / atomPoolBytes(treePool));
        printf("└─────────────────┴─────────────────┴─────────────────┴────────"
               "─┘\n\n");

#undef MOPS

        printf("Recommendation:\n");
        printf("  - Use ATOM_POOL_HASH when lookup speed is critical\n");
        printf("  - Use ATOM_POOL_TREE when memory efficiency is critical\n");

        atomPoolFree(hashPool);
        atomPoolFree(treePool);
        zfree(ids);
        zfree(keys);
        zfree(keyData);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
