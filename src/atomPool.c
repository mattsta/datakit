#include "atomPool.h"
#include "atomPersist.h"
#include "cleaner.h"

defcleaner(atomPool *, atomPoolFree);
defcleaner(void *, zfree);

#include "datakit.h"
#include "multimapAtom.h"
#include "multimapAtomDense.h"
#include "stringPool.h"
#include "timeUtil.h"

#include <assert.h>

defcleanerName(databox *, zfree, Databoxes);
defcleanerName(char *, zfree, Chars);
defcleanerName(uint64_t *, zfree, U64);
defcleanerName(uint8_t *, zfree, U8);

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
    atomPoolIds ids;              /* Id behaviour */
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

static void treeFree(void *impl) {
    multimapAtomFree((multimapAtom *)impl);
}

/* .reset is NULL: multimapAtom has no in-place clear, so atomPoolReset
 * recreates the backend instead (a NULL reset means "recreate"). */
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
    .reset = NULL,
    .free = treeFree,
};

/* ====================================================================
 * multimapAtomDense Backend Wrappers (TREE, lowest-first ids)
 * ==================================================================== */

static uint64_t denseIntern(void *impl, const databox *str) {
    databox key = *str;
    multimapAtomDenseInsertIfNewConvertAndRetain((multimapAtomDense *)impl,
                                                 &key);
    return TREE_ID_TO_EXTERNAL(key.data.u);
}

static uint64_t denseGetId(const void *impl, const databox *str) {
    return treeGetId(multimapAtomDenseBase(impl), str);
}

static bool denseExists(const void *impl, const databox *str) {
    return treeExists(multimapAtomDenseBase(impl), str);
}

static bool denseLookup(const void *impl, uint64_t id, databox *str) {
    return treeLookup(multimapAtomDenseBase(impl), id, str);
}

static void denseRetain(void *impl, uint64_t id) {
    multimapAtomDenseRetainById((multimapAtomDense *)impl,
                                TREE_ID_FROM_EXTERNAL(id));
}

static bool denseRelease(void *impl, uint64_t id) {
    return multimapAtomDenseReleaseById((multimapAtomDense *)impl,
                                        TREE_ID_FROM_EXTERNAL(id));
}

static uint64_t denseRefcount(const void *impl, uint64_t id) {
    return treeRefcount(multimapAtomDenseBase(impl), id);
}

static size_t denseCount(const void *impl) {
    return multimapAtomDenseCount((const multimapAtomDense *)impl);
}

static size_t denseBytes(const void *impl) {
    return multimapAtomDenseBytes((const multimapAtomDense *)impl);
}

static void denseFree(void *impl) {
    multimapAtomDenseFree((multimapAtomDense *)impl);
}

static const atomPoolVTable denseVTable = {
    .intern = denseIntern,
    .getId = denseGetId,
    .exists = denseExists,
    .lookup = denseLookup,
    .retain = denseRetain,
    .release = denseRelease,
    .refcount = denseRefcount,
    .count = denseCount,
    .bytes = denseBytes,
    .reset = NULL,
    .free = denseFree,
};

/* ====================================================================
 * Public API Implementation
 * ==================================================================== */

static void *atomPoolImplNew(atomPoolType type, atomPoolIds ids) {
    switch (type) {
    case ATOM_POOL_HASH: {
        static const stringPoolIds hashIds[] = {
            [ATOM_POOL_IDS_REUSE] = STRING_POOL_IDS_REUSE,
            [ATOM_POOL_IDS_PERMANENT] = STRING_POOL_IDS_PERMANENT,
            [ATOM_POOL_IDS_LOWEST] = STRING_POOL_IDS_LOWEST,
        };
        /* @own: caller; free=stringPoolFree */
        return stringPoolNewWithIds(hashIds[ids]);
    }
    case ATOM_POOL_TREE:
        switch (ids) {
        case ATOM_POOL_IDS_PERMANENT:
            return multimapAtomNewPermanent();
        case ATOM_POOL_IDS_LOWEST:
            return multimapAtomDenseNew();
        default:
            return multimapAtomNew();
        }
    default:
        return NULL;
    }
}

atomPool *atomPoolNewWithIds(atomPoolType type, atomPoolIds ids) {
    if ((type != ATOM_POOL_HASH && type != ATOM_POOL_TREE) ||
        (ids != ATOM_POOL_IDS_REUSE && ids != ATOM_POOL_IDS_PERMANENT &&
         ids != ATOM_POOL_IDS_LOWEST)) {
        return NULL;
    }

    /* atomPoolFree is safe on a partially-constructed pool (it only
     * tears down impl when both impl and vtable are set). */
    cleaner(atomPoolFree) atomPool *pool = zcalloc(1, sizeof(*pool));
    if (!pool) {
        return NULL;
    }

    pool->type = type;
    pool->ids = ids;
    pool->impl = atomPoolImplNew(type, ids);
    if (type == ATOM_POOL_HASH) {
        pool->vtable = &hashVTable;
    } else {
        pool->vtable = ids == ATOM_POOL_IDS_LOWEST ? &denseVTable : &treeVTable;
    }

    if (!pool->impl) {
        return NULL;
    }

    atomPool *out = pool;
    pool = NULL;
    return out;
}

atomPool *atomPoolNew(atomPoolType type) {
    /* @own: caller; free=atomPoolFree */
    return atomPoolNewWithIds(type, ATOM_POOL_IDS_REUSE);
}

atomPoolIds atomPoolGetIds(const atomPool *pool) {
    return pool->ids;
}

atomPool *atomPoolNewDefault(void) {
    /* @own: caller; free=atomPoolFree */
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
        if (pool->vtable->reset) {
            pool->vtable->reset(pool->impl);
        } else {
            /* Backend has no in-place clear: recreate it empty (was a
             * silent no-op for TREE, leaving atoms interned forever) */
            pool->vtable->free(pool->impl);
            pool->impl = atomPoolImplNew(pool->type, pool->ids);
        }
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
 * Persistence (atomPersist.h)
 * ==================================================================== */
/* The descriptor of a pool's backend */
static const persistOps *atomPoolBackendOps_(atomPoolType type,
                                             atomPoolIds ids) {
    if (type == ATOM_POOL_HASH) {
        return &persistOpsStringPool;
    }

    return ids == ATOM_POOL_IDS_LOWEST ? &persistOpsMultimapAtomDense
                                       : &persistOpsMultimapAtom;
}

static uint8_t *atomPoolPersistSnapshot_(const void *structure, size_t *len) {
    const atomPool *pool = structure;
    size_t innerLen = 0;
    cleanerName(zfree, U8) uint8_t *inner =
        atomPoolBackendOps_(pool->type, pool->ids)
            ->snapshot(pool->impl, &innerLen);
    if (!inner) {
        return NULL;
    }

    const databox type = DATABOX_UNSIGNED(pool->type);
    const databox ids = DATABOX_UNSIGNED(pool->ids);
    const databox backend = databoxNewBytes(inner, innerLen);
    const databox *row[3] = {&type, &ids, &backend};
    /* @own: caller; free=zfree */
    flex *f = flexNewFromDataboxPtrsExact(row, 3);
    *len = flexBytes(f);
    return (uint8_t *)f;
}

static void *atomPoolPersistRestore_(const uint8_t *data, size_t len) {
    if (!flexValidateBufferDeep(data, len) ||
        flexCount((const flex *)data) != 3) {
        return NULL;
    }

    const flex *f = (const flex *)data;
    databox type;
    databox ids;
    databox backend;
    flexEntry *fe = flexHead(f);
    flexGetByType(fe, &type);
    fe = flexNext(f, fe);
    flexGetByType(fe, &ids);
    fe = flexNext(f, fe);
    flexGetByType(fe, &backend);
    if (type.type != DATABOX_UNSIGNED_64 || ids.type != DATABOX_UNSIGNED_64 ||
        type.data.u64 > ATOM_POOL_TREE || ids.data.u64 > ATOM_POOL_IDS_LOWEST ||
        !DATABOX_IS_BYTES(&backend)) {
        return NULL;
    }

    const atomPoolType t = (atomPoolType)type.data.u64;
    const atomPoolIds i = (atomPoolIds)ids.data.u64;
    const persistOps *ops = atomPoolBackendOps_(t, i);
    void *impl = ops->restore(databoxBytes(&backend), backend.len);
    if (!impl) {
        return NULL;
    }

    /* The backend's own id behaviour must be the one the pool names */
    static const stringPoolIds hashIds[] = {
        [ATOM_POOL_IDS_REUSE] = STRING_POOL_IDS_REUSE,
        [ATOM_POOL_IDS_PERMANENT] = STRING_POOL_IDS_PERMANENT,
        [ATOM_POOL_IDS_LOWEST] = STRING_POOL_IDS_LOWEST,
    };
    const bool fits =
        t == ATOM_POOL_HASH
            ? stringPoolIdsOf(impl) == hashIds[i]
            : (i == ATOM_POOL_IDS_LOWEST ||
               multimapAtomIsPermanent(impl) == (i == ATOM_POOL_IDS_PERMANENT));
    if (!fits) {
        ops->free(impl);
        return NULL;
    }

    /* @own: caller; free=atomPoolFree */
    atomPool *pool = zcalloc(1, sizeof(*pool));
    pool->type = t;
    pool->ids = i;
    pool->impl = impl;
    if (t == ATOM_POOL_HASH) {
        pool->vtable = &hashVTable;
    } else {
        pool->vtable = i == ATOM_POOL_IDS_LOWEST ? &denseVTable : &treeVTable;
    }

    return pool;
}

static uint8_t *atomPoolPersistEncodeOp_(persistOp op, const void *args,
                                         size_t argc, size_t *len) {
    *len = 0;
    if (op == PERSIST_OP_CLEAR) {
        return persistOpNoBody(len);
    }
    if ((op != PERSIST_OP_INSERT && op != PERSIST_OP_NUM_INCR) || argc != 2) {
        return NULL;
    }
    return persistOpEncodeBoxes(args, argc, len);
}

/* The pool's ops are its backend's with the pool's 1-based ids: a TREE
 * backend counts from 0 */
static bool atomPoolPersistApplyOp_(void *structure, persistOp op,
                                    const uint8_t *data, size_t len) {
    atomPool *pool = *(atomPool **)structure;
    if (op == PERSIST_OP_CLEAR) {
        atomPoolReset(pool);
        return true;
    }

    const persistOps *ops = atomPoolBackendOps_(pool->type, pool->ids);
    if (pool->type == ATOM_POOL_HASH) {
        return ops->applyOp(&pool->impl, op, data, len);
    }

    databoxBig id;
    databoxBig arg;
    if ((op != PERSIST_OP_INSERT && op != PERSIST_OP_NUM_INCR) ||
        !flexGetTwoDataboxesBig(data, len, &id, &arg) ||
        id.type != DATABOX_UNSIGNED_64 || id.data.u64 == 0) {
        return false;
    }

    const databox treeId = DATABOX_UNSIGNED(TREE_ID_FROM_EXTERNAL(id.data.u64));
    const databox *args[2] = {&treeId, (const databox *)&arg};
    size_t innerLen = 0;
    cleanerName(zfree, U8) uint8_t *inner =
        ops->encodeOp(op, args, 2, &innerLen);
    return inner && ops->applyOp(&pool->impl, op, inner, innerLen);
}

static void *atomPoolPersistNewEmpty_(void) {
    /* @own: caller; free=atomPoolFree */
    return atomPoolNewDefault();
}

static size_t atomPoolPersistCount_(const void *structure) {
    return atomPoolCount(structure);
}

static size_t atomPoolPersistBytes_(const void *structure) {
    return atomPoolBytes(structure);
}

static void atomPoolPersistFree_(void *structure) {
    atomPoolFree(structure);
}

const persistOps persistOpsAtomPool = {
    .type = PERSIST_TYPE_ATOM_POOL,
    .name = "atomPool",
    .formatVersion = 1,
    .minReadVersion = 1,
    .snapshot = atomPoolPersistSnapshot_,
    .restore = atomPoolPersistRestore_,
    .newEmpty = atomPoolPersistNewEmpty_,
    .count = atomPoolPersistCount_,
    .estimateSize = atomPoolPersistBytes_,
    .encodeOp = atomPoolPersistEncodeOp_,
    .applyOp = atomPoolPersistApplyOp_,
    .free = atomPoolPersistFree_,
};

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

    TEST("the default pool, backend types and their names") {
        cleaner(atomPoolFree) atomPool *def = atomPoolNewDefault();
        cleaner(atomPoolFree) atomPool *tree = atomPoolNew(ATOM_POOL_TREE);
        if (!def || atomPoolGetType(def) != ATOM_POOL_HASH ||
            atomPoolGetType(tree) != ATOM_POOL_TREE ||
            strcmp(atomPoolTypeName(ATOM_POOL_HASH),
                   atomPoolTypeName(ATOM_POOL_TREE)) == 0 ||
            strcmp(atomPoolTypeName((atomPoolType)99), "UNKNOWN") != 0) {
            ERRR("default pool, GetType or TypeName wrong");
        }
    }

    /* Test both backends */
    atomPoolType types[] = {ATOM_POOL_HASH, ATOM_POOL_TREE};
    const char *typeNames[] = {"HASH", "TREE"};

    for (int t = 0; t < 2; t++) {
        atomPoolType type = types[t];
        printf("\n=== Testing %s backend ===\n", typeNames[t]);

        TEST("atomPool basic operations") {
            cleaner(atomPoolFree) atomPool *pool = atomPoolNew(type);
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

        cleanup_basic:; /* pool frees itself at scope exit */
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
            cleaner(atomPoolFree) atomPool *pool = atomPoolNew(type);
            if (!pool) {
                ERRR("Failed to create atomPool");
            }

            const size_t N = 1000;
            cleaner(zfree) void *idsAlloc = zmalloc(N * sizeof(uint64_t));
            uint64_t *ids = idsAlloc;
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

        cleanup_many:; /* ids + pool free themselves at scope exit */
        }
    }

    TEST("atomPool reset empties both backends") {
        /* TREE reset used to be a silent no-op: atoms stayed interned
         * and counts went stale, silently diverging from HASH */
        const atomPoolType resetBackends[2] = {ATOM_POOL_HASH, ATOM_POOL_TREE};
        for (int t = 0; t < 2; t++) {
            atomPool *pool = atomPoolNew(resetBackends[t]);
            databox s1 = databoxNewBytesString("reset_a");
            databox s2 = databoxNewBytesString("reset_b");

            atomPoolIntern(pool, &s1);
            atomPoolIntern(pool, &s2);
            if (atomPoolCount(pool) != 2) {
                ERR("backend %d: expected 2 pre-reset, got %zu", t,
                    atomPoolCount(pool));
            }

            atomPoolReset(pool);

            if (atomPoolCount(pool) != 0) {
                ERR("backend %d: expected 0 after reset, got %zu", t,
                    atomPoolCount(pool));
            }
            if (atomPoolExists(pool, &s1)) {
                ERR("backend %d: atom survived reset", t);
            }

            /* Pool must be fully usable after reset */
            const uint64_t reinternedId = atomPoolIntern(pool, &s1);
            if (reinternedId == 0 || atomPoolCount(pool) != 1) {
                ERR("backend %d: re-intern after reset failed (count %zu)", t,
                    atomPoolCount(pool));
            }

            atomPoolFree(pool);
        }
    }

    /* Performance comparison */
    TEST("atomPool performance comparison") {
        printf("\n=== Performance Comparison: HASH vs TREE ===\n\n");

        const size_t N = 50000;

        /* Prepare test data */
        cleanerName(zfree, Databoxes) databox *keys =
            zmalloc(N * sizeof(databox));
        cleanerName(zfree, Chars) char *keyData = zmalloc(N * 20);
        for (size_t i = 0; i < N; i++) {
            int len = snprintf(keyData + i * 20, 20, "testkey%08zu", i);
            keys[i] = databoxNewBytesAllowEmbed(keyData + i * 20, len);
        }

        cleaner(atomPoolFree) atomPool *hashPool = atomPoolNew(ATOM_POOL_HASH);
        cleaner(atomPoolFree) atomPool *treePool = atomPoolNew(ATOM_POOL_TREE);

        cleanerName(zfree, U64) uint64_t *ids = zmalloc(N * sizeof(uint64_t));

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
    }

    TEST("id behaviours: an unknown one is refused; each reports itself") {
        if (atomPoolNewWithIds(ATOM_POOL_TREE, (atomPoolIds)7) ||
            atomPoolNewWithIds((atomPoolType)7, ATOM_POOL_IDS_REUSE)) {
            ERRR("an unknown type or id behaviour was accepted");
        }

        for (int t = 0; t < 2; t++) {
            for (int ids = 0; ids < 3; ids++) {
                atomPool *pool =
                    atomPoolNewWithIds((atomPoolType)t, (atomPoolIds)ids);
                if (!pool || atomPoolGetIds(pool) != (atomPoolIds)ids) {
                    ERR("type %d ids %d: not created as asked", t, ids);
                }

                atomPoolFree(pool);
            }
        }
    }

    TEST("permanent: atoms outlive Release in both backends") {
        for (int t = 0; t < 2; t++) {
            atomPool *pool =
                atomPoolNewWithIds((atomPoolType)t, ATOM_POOL_IDS_PERMANENT);
            const databox a = databoxNewBytesString("alpha");
            const databox b = databoxNewBytesString("beta");
            const uint64_t ida = atomPoolIntern(pool, &a);
            const uint64_t idb = atomPoolIntern(pool, &b);
            atomPoolRetain(pool, ida);
            if (atomPoolRelease(pool, ida) || atomPoolRelease(pool, ida) ||
                atomPoolCount(pool) != 2 || atomPoolRefcount(pool, ida) != 1 ||
                !atomPoolExists(pool, &a)) {
                ERR("%s: a released permanent atom changed",
                    atomPoolTypeName((atomPoolType)t));
            }

            atomPoolReset(pool);
            const databox c = databoxNewBytesString("gamma");
            if (atomPoolCount(pool) != 0 || atomPoolIntern(pool, &c) == 0 ||
                atomPoolGetIds(pool) != ATOM_POOL_IDS_PERMANENT) {
                ERR("%s: Reset did not keep the permanent behaviour",
                    atomPoolTypeName((atomPoolType)t));
            }

            (void)idb;
            atomPoolFree(pool);
        }
    }

    TEST("lowest: both backends hand out the oracle's lowest free id") {
        /* The same seeded interns and releases on both backends: each new
         * atom must get the lowest id not in use (ids are 1-based), so
         * the two id sequences are identical */
        enum { LOW_IDS_ = 512 };
        uint64_t sequence[2][2000] = {{0}};
        for (int t = 0; t < 2; t++) {
            atomPool *pool =
                atomPoolNewWithIds((atomPoolType)t, ATOM_POOL_IDS_LOWEST);
            bool live[LOW_IDS_ + 1] = {false};
            size_t liveCount = 0;
            uint64_t rng = 0x5EED;
            for (size_t op = 0; op < 2000; op++) {
                rng =
                    rng * UINT64_C(6364136223846793005) + 1442695040888963407u;
                const uint64_t r = rng >> 33;
                if (r % 3 != 0 || liveCount == 0) {
                    if (liveCount == LOW_IDS_) {
                        continue;
                    }

                    uint64_t want = 1;
                    while (live[want]) {
                        want++;
                    }

                    char buf[32];
                    snprintf(buf, sizeof(buf), "atom%zu", op);
                    const databox s = databoxNewBytesString(buf);
                    const uint64_t got = atomPoolIntern(pool, &s);
                    sequence[t][op] = got;
                    if (got != want) {
                        ERR("%s op %zu: intern got id %" PRIu64
                            ", want %" PRIu64,
                            atomPoolTypeName((atomPoolType)t), op, got, want);
                        break;
                    }

                    live[want] = true;
                    liveCount++;
                } else {
                    uint64_t id = 1 + (r % LOW_IDS_);
                    while (!live[id]) {
                        id = id % LOW_IDS_ + 1;
                    }

                    if (!atomPoolRelease(pool, id)) {
                        ERR("%s op %zu: release of id %" PRIu64
                            " kept the atom",
                            atomPoolTypeName((atomPoolType)t), op, id);
                        break;
                    }

                    live[id] = false;
                    liveCount--;
                }
            }

            atomPoolFree(pool);
        }

        if (memcmp(sequence[0], sequence[1], sizeof(sequence[0])) != 0) {
            ERRR("the two backends handed out different ids");
        }
    }

    TEST("Repr: both backends, empty and holding atoms") {
        for (int t = 0; t < 2; t++) {
            atomPool *pool = atomPoolNew((atomPoolType)t);
            atomPoolRepr(pool);
            const databox s = databoxNewBytesString("printed");
            atomPoolIntern(pool, &s);
            atomPoolRepr(pool);
            atomPoolFree(pool);
        }
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
