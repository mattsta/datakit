#include "multimapAtom.h"
#include "atomPersist.h"
#include "cleaner.h"

defcleaner(multimapAtom *, multimapAtomFree);
defcleanerName(void *, zfree, Void);

struct multimapAtom {
    multimap *mapAtomForward; /* ID -> String, sorted by ID */
    multimap *mapAtomReverse; /* IDs sorted by string value */
    /* The next generated id, with the table's id behaviour in the top bit
     * (set: permanent), so the behaviour costs no bytes.  Ids stay below
     * MULTIMAP_ATOM_ID_LIMIT. */
    uint64_t highest;
};

#define ATOM_PERMANENT_BIT_ MULTIMAP_ATOM_ID_LIMIT

static inline uint64_t atomNextId_(const multimapAtom *ma) {
    return ma->highest & ~ATOM_PERMANENT_BIT_;
}

static inline void atomSetNextId_(multimapAtom *ma, const uint64_t id) {
    assert(id < MULTIMAP_ATOM_ID_LIMIT);
    ma->highest = (ma->highest & ATOM_PERMANENT_BIT_) | id;
}

bool multimapAtomIsPermanent(const multimapAtom *ma) {
    return (ma->highest & ATOM_PERMANENT_BIT_) != 0;
}

/* ====================================================================
 * Create
 * ==================================================================== */
void multimapAtomInit(multimapAtom *ma) {
    /* 3-map of: {ID, Key, Refcount} */
    /* @own: container; free=multimapAtomFree */
    ma->mapAtomForward = multimapNewLimit(3, FLEX_CAP_LEVEL_512);

    /* 1-map of ID, but SORTED BY 'KEY'.  Basically, a self-sorting list. */
    /* @own: container; free=multimapAtomFree */
    ma->mapAtomReverse = multimapNewLimit(1, FLEX_CAP_LEVEL_512);

    ma->highest = 0;
}

multimapAtom *multimapAtomNew(void) {
    /* @own: caller; free=multimapAtomFree */
    multimapAtom *ma = zcalloc(1, sizeof(*ma));
    multimapAtomInit(ma);
    return ma;
}

multimapAtom *multimapAtomNewPermanent(void) {
    /* @own: caller; free=multimapAtomFree */
    multimapAtom *ma = multimapAtomNew();
    ma->highest |= ATOM_PERMANENT_BIT_;
    return ma;
}

/* ====================================================================
 * Free
 * ==================================================================== */
void multimapAtomFree(multimapAtom *ma) {
    if (ma) {
        multimapFree(ma->mapAtomForward);
        multimapFree(ma->mapAtomReverse);
        zfree(ma);
    }
}

/* ====================================================================
 * Insert (doesn't check for existing entry)
 * ==================================================================== */
static void abstractInsert(multimapAtom *ma, const databox *reverseKey,
                           const uint64_t id) {
    const databox atomId = {.data.u = id,
                            .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
    /* Use 'false' as "only one requested so far" so if we don't have any
     * more ref counts, it only takes one byte in the multimap (vs 3 bytes
     * for encoding the number 1). multimapIncr() knows how to do math on
     * bool types, so this works correctly with retain/release.
     * Internal: 0 = 1 ref, 1 = 2 refs, etc. API layer adds +1 for display. */
    const databox refcount = {.type = DATABOX_FALSE};
    const databox *elements[3] = {&atomId, reverseKey, &refcount};

    [[maybe_unused]] const bool replaced =
        multimapInsert(&ma->mapAtomForward, elements);
    assert(!replaced);

    multimapInsertWithSurrogateKey(&ma->mapAtomReverse, &reverseKey, &atomId,
                                   ma);

    assert(multimapCount(ma->mapAtomForward) ==
           multimapCount(ma->mapAtomReverse));

    if (id >= atomNextId_(ma)) {
        atomSetNextId_(ma, id + 1);
    }
}

/* A new atom's forward row, made by atomFindOrCreate_ once its reverse
 * search knows the value is absent (before the reverse row goes in, as
 * that insert may resolve the new id) */
typedef struct atomCreate_ {
    multimapAtom *ma;
    const databox *key;
    uint64_t id;
} atomCreate_;

static void atomCreateForward_(void *ctx) {
    const atomCreate_ *c = ctx;
    const databox atomId = {.data.u = c->id,
                            .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
    const databox refcount = {.type = DATABOX_FALSE}; /* one reference */
    const databox *elements[3] = {&atomId, c->key, &refcount};
    [[maybe_unused]] const bool replaced =
        multimapInsert(&c->ma->mapAtomForward, elements);
    assert(!replaced);

    /* Generated ids continue past every id in use */
    if (c->id >= atomNextId_(c->ma)) {
        atomSetNextId_(c->ma, c->id + 1);
    }
}

/* The atom of 'key' in one reverse search: true when it existed ('ref'
 * its reference), false when this call created it with one reference and
 * the id 'id' ('ref' the new reference) */
static bool atomFindOrCreateWithId_(multimapAtom *ma, const databox *key,
                                    const uint64_t id, databox *ref) {
    atomCreate_ create = {.ma = ma, .key = key, .id = id};
    const databox atomId = {.data.u = create.id,
                            .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
    const databox *reverseRow[1] = {key};
    if (!multimapInsertWithSurrogateKeyIfAbsent(&ma->mapAtomReverse, reverseRow,
                                                &atomId, ma, ref,
                                                atomCreateForward_, &create)) {
        return true;
    }

    assert(multimapCount(ma->mapAtomForward) ==
           multimapCount(ma->mapAtomReverse));
    *ref = atomId;
    return false;
}

bool multimapAtomFindOrCreateWithId(multimapAtom *ma, const databox *key,
                                    uint64_t id, databox *ref) {
    return atomFindOrCreateWithId_(ma, key, id, ref);
}

static bool atomFindOrCreate_(multimapAtom *ma, const databox *key,
                              databox *ref) {
    return atomFindOrCreateWithId_(ma, key, atomNextId_(ma), ref);
}

void multimapAtomInsert(multimapAtom *ma, const databox *box) {
    abstractInsert(ma, box, atomNextId_(ma));
}

void multimapAtomInsertConvert(multimapAtom *ma, databox *box) {
    const uint64_t id = atomNextId_(ma);
    const databox incr = {.data.u = id,
                          .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};

    abstractInsert(ma, box, id);
    *box = incr;
}

/* ====================================================================
 * Insert With Exact Key (don't create auto-incremented counter ID)
 * ==================================================================== */
/* Exact-key atom maps *don't* have refcounts. It's basically a fancy
 * inverted index. */
/* Generated ids continue past every exact id, so an InsertConvert after
 * an exact insert never hands out an id that is already bound. */
void multimapAtomInsertWithExactAtomID(multimapAtom *ma, const uint64_t atomRef,
                                       const databox *reverseKey) {
    const databox atomId = {.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                            .data.u = atomRef};
    const databox *elements[3] = {&atomId, reverseKey, &DATABOX_BOX_FALSE};

    /* If exists, delete so we can have a clean insert.
     * Instead of lookup->result->delete, we just attempt
     * a direct delete. If the first delete works, we know we can
     * delete from the other map too. */
    databox foundReference = {};
    if (multimapDeleteWithReference(&ma->mapAtomReverse, reverseKey, ma,
                                    &foundReference)) {
        [[maybe_unused]] const bool deleted =
            multimapDelete(&ma->mapAtomForward, &foundReference);
        assert(deleted);
    }

    /* Also clear any prior binding of this exact ID to a DIFFERENT key.
     * mapAtomForward is a non-set map, so without this a colliding ID
     * (e.g. restoring a table with previously-assigned ids) would keep
     * two rows for one id and orphan one forever. */
    multimapAtomDeleteByRef(ma, &atomId);

    multimapInsert(&ma->mapAtomForward, elements);
    multimapInsertWithSurrogateKey(&ma->mapAtomReverse, &reverseKey, &atomId,
                                   ma);

    /* After the deletes above, which restart ids when they empty the table */
    if (atomRef >= atomNextId_(ma) && atomRef + 1 < MULTIMAP_ATOM_ID_LIMIT) {
        atomSetNextId_(ma, atomRef + 1);
    }
}

/* ====================================================================
 * Insert (checks for existing entry first then returns it if found)
 * ==================================================================== */
/* Returns true when value already existed.
 * Returns false when value is created */
bool multimapAtomInsertIfNewConvert(multimapAtom *ma, databox *key) {
    /* The reference replaces the whole box, as in InsertConvert: a decode
     * straight into 'key' would keep the 'big' and 'allocated' bits of a
     * 96/128-bit or heap value it held. */
    databox ref = {};
    const bool existed = atomFindOrCreate_(ma, key, &ref);
    *key = ref;
    key->created = !existed;
    return existed;
}

bool multimapAtomInsertIfNewConvertAndRetain(multimapAtom *ma, databox *key) {
    databox ref = {};
    const bool existed = atomFindOrCreate_(ma, key, &ref);
    if (existed) {
        /* Increase reference count automatically since is also a checkout */
        multimapAtomRetainByRef(ma, &ref);
    }

    *key = ref;
    key->created = !existed;
    return existed;
}

/* ====================================================================
 * Lookup
 * ==================================================================== */
/* Lookup ATOM REFERENCE to USER DATA VALUE */
bool multimapAtomLookup(const multimapAtom *ma, const databox *ref,
                        databox *key) {
    databox count = {};
    databox *elements[2] = {key, &count};

    return multimapLookup(ma->mapAtomForward, ref, elements);
}

void multimapAtomLookupResult(const multimapAtom *ma, const databox *key,
                              multimapAtomResult *result) {
    databox count = {};
    databox *elements[2] = {&result->val, &count};

    /* 'val' is a plain databox with no wide storage, so a 96/128-bit
     * value is reported type-only instead of written through whatever
     * the caller's box held. */
    result->val.big = false;
    multimapLookup(ma->mapAtomForward, key, elements);
    result->refcount = count.data.u;
}

void multimapAtomLookupConvert(const multimapAtom *ma, databox *box) {
    /* The reference is copied out first so the lookup key is never the
     * box being decoded into. */
    const databox ref = *box;
    databox count = {};
    databox *elements[2] = {box, &count};

    /* A plain databox can't hold a 96/128-bit value: report it type-only
     * rather than write it through the reference id in the payload. */
    box->big = false;
    if (!multimapLookup(ma->mapAtomForward, &ref, elements)) {
        *box = ref;
    }
}

void multimapAtomLookupConvertBig(const multimapAtom *ma, databoxBig *box) {
    const databox ref = {.data = box->data, .type = box->type};
    databox *const out = (databox *)box;
    databox count = {};
    databox *elements[2] = {out, &count};

    /* Reset every header bit, then aim the payload pointer at 'extra' so
     * a wide value has somewhere to land. */
    *out = (databox){};
    DATABOX_BIG_INIT(box);
    if (multimapLookup(ma->mapAtomForward, &ref, elements)) {
        box->big = box->type == DATABOX_SIGNED_128 ||
                   box->type == DATABOX_UNSIGNED_128;
    } else {
        *out = ref;
    }
}

/* Lookup USER DATA VALUE to ATOM REFERENCE */
bool multimapAtomLookupReference(const multimapAtom *ma, const databox *key,
                                 databox *atomRef) {
    return multimapExistsWithReference(ma->mapAtomReverse, key, atomRef, ma);
}

void multimapAtomLookupRefcount(const multimapAtom *ma, const databox *key,
                                databox *count) {
    /* Zeroed: the value is discarded, so a 96/128-bit one decodes
     * type-only instead of through a stale pointer. */
    databox val = {};
    databox *elements[2] = {&val, count};

    multimapLookup(ma->mapAtomForward, key, elements);
}

/* ====================================================================
 * Further Query / Act
 * ==================================================================== */
bool multimapAtomLookupMin(const multimapAtom *ma, databox *minRef) {
    /* mapAtomReverse is a 1-map, so we only need 1 element */
    return multimapFirstInto(ma->mapAtomReverse, minRef, 1);
}

/* ====================================================================
 * Use
 * ==================================================================== */
void multimapAtomRetainByRef(multimapAtom *ma, const databox *foundRef) {
    assert(foundRef->type == DATABOX_CONTAINER_REFERENCE_EXTERNAL);
#if 0
    databox kkey;
    databox count;

    databox *gots[2] = {&kkey, &count};

    assert(multimapLookup(ma->mapAtomForward, foundRef, gots));
    assert(multimapExistsWithReference(ma->mapAtomReverse, &kkey, &count, ma));
#endif
    if (multimapAtomIsPermanent(ma)) {
        return; /* no count is kept: the atom lives until Delete or Free */
    }

    multimapFieldIncr(&ma->mapAtomForward, foundRef, 2, 1);
}

void multimapAtomRetainById(multimapAtom *ma, const uint64_t id) {
    databox ref = {.data.u = id, .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
    multimapAtomRetainByRef(ma, &ref);
}

void multimapAtomRetain(multimapAtom *ma, const databox *key) {
    /* A new atom is created with its one reference */
    databox ref = {};
    if (atomFindOrCreate_(ma, key, &ref)) {
        multimapAtomRetainByRef(ma, &ref);
    }
}

void multimapAtomRetainConvert(multimapAtom *ma, databox *key) {
    /* Zeroed: a reference id decodes narrow and leaves the flag bits as
     * they were, and this box is handed back to the caller whole. */
    databox ref = {};
    if (atomFindOrCreate_(ma, key, &ref)) {
        multimapAtomRetainByRef(ma, &ref);
    }

    *key = ref;
}

/* ====================================================================
 * Un-use
 * ==================================================================== */
/* After the atom 'deletedId' is deleted, whichever call deleted it.  A
 * permanent table never hands an id out again.  A reusing table keeps its
 * ids small (fewer external-reference storage bytes) where that costs
 * nothing to discover: an empty table restarts at 0, and deleting the
 * atom with the highest id lowers the next id to just past the largest
 * live one (the forward map's last row, as it is sorted by id), so
 * last-in-first-out churn reuses the same ids.  An interior hole stays
 * unused until the table empties; multimapAtomDense reuses every hole,
 * lowest first.
 *
 * CONTRACT (reusing tables): an id is valid while its atom exists.  A
 * caller MUST NOT keep a bare id/ref after releasing its references: it
 * may then resolve to an unrelated later entry. */
static void atomAfterDelete_(multimapAtom *ma, const uint64_t deletedId) {
    if (multimapAtomIsPermanent(ma)) {
        return;
    }

    if (multimapCount(ma->mapAtomForward) == 0) {
        atomSetNextId_(ma, 0);
        return;
    }

    if (deletedId + 1 == atomNextId_(ma)) {
        databox last[3] = {};
        if (multimapLastInto(ma->mapAtomForward, last, 3) &&
            last[0].data.u + 1 < MULTIMAP_ATOM_ID_LIMIT) {
            atomSetNextId_(ma, last[0].data.u + 1);
        }
    }
}

/* Remove the atom 'ref' whose reverse row the caller already found
 * ('reverseEntry'): that row is deleted where it is, without a second
 * reverse search.  The reverse map resolves its ids through the forward
 * map, so the forward row goes last. */
static void atomDeleteKnown(multimapAtom *ma, multimapEntry *reverseEntry,
                            const databox *ref) {
    multimapDeleteEntryWithReference(&ma->mapAtomReverse, reverseEntry, ma);

    [[maybe_unused]] const bool deleteForward =
        multimapDelete(&ma->mapAtomForward, ref);
    assert(deleteForward);

    atomAfterDelete_(ma, ref->data.u);
}

/* Drop one reference to 'foundRef'; delete the atom when it was the last.
 * 'reverseEntry', when non-NULL, is the atom's reverse row, already found
 * by the caller. */
DK_INLINE_ALWAYS bool conformRefcountDecr(multimapAtom *ma,
                                          const databox *foundRef,
                                          multimapEntry *reverseEntry) {
    /* Decrement refcount first, then delete if it goes negative.
     * We use 0-based internal refcounts for memory efficiency:
     * Internal 0 = 1 ref, 1 = 2 refs, etc. (DATABOX_FALSE = 1 byte vs 3 bytes)
     * When it goes negative, no more references exist. */
    if (multimapAtomIsPermanent(ma)) {
        return false; /* permanent atoms live until Delete or Free */
    }

    const int64_t checkedOut =
        multimapFieldIncr(&ma->mapAtomForward, foundRef, 2, -1);

    /* Internal 0-based: goes negative when last reference released */
    if (checkedOut < 0) {
        if (reverseEntry) {
            atomDeleteKnown(ma, reverseEntry, foundRef);
        } else {
            [[maybe_unused]] const bool deleted =
                multimapAtomDeleteByRef(ma, foundRef);
            assert(deleted);
        }

        return true;
    }

    return false;
}

bool multimapAtomReleaseById(multimapAtom *ma, const databox *foundRef) {
    return conformRefcountDecr(ma, foundRef, NULL);
}

bool multimapAtomRelease(multimapAtom *ma, const databox *key) {
    /* The reverse row found here is the one a last release deletes */
    multimapEntry me = {};
    if (multimapGetUnderlyingEntryWithReference(ma->mapAtomReverse, key, &me,
                                                ma)) {
        databox foundRef = {};
        flexGetByType(me.fe, &foundRef);
        return conformRefcountDecr(ma, &foundRef, &me);
    }

    /* else, releasing something we don't have is an error */
    assert(NULL && "Releasing something not in the atom table?");
    return false;
}

bool multimapAtomDelete(multimapAtom *ma, const databox *key) {
    /* One reverse search: the delete itself reports the atom's id */
    databox foundReference = {};
    if (!multimapDeleteWithReference(&ma->mapAtomReverse, key, ma,
                                     &foundReference)) {
        return false;
    }

    [[maybe_unused]] const bool deleteForward =
        multimapDelete(&ma->mapAtomForward, &foundReference);
    assert(deleteForward);

    atomAfterDelete_(ma, foundReference.data.u);
    return true;
}

bool multimapAtomDeleteByRef(multimapAtom *ma, const databox *ref) {
    assert(ref->type == DATABOX_CONTAINER_REFERENCE_EXTERNAL);

    /* The forward row found here gives the value the reverse map is
     * searched by, and is deleted where it is once the reverse row is
     * gone (the reverse search resolves ids through the forward map, and
     * deleting the reverse row leaves the forward map as it was) */
    multimapEntry me = {};
    if (!multimapGetUnderlyingEntry(ma->mapAtomForward, ref, &me)) {
        return false;
    }

    /* The whole value: a 96/128-bit one lands in 'valueWide' */
    databox value;
    databoxWide valueWide;
    flexGetByTypeInto(flexNext(*me.map, me.fe), &value, &valueWide);
    assert(value.type != DATABOX_CONTAINER_REFERENCE_EXTERNAL);

    databox found = {};
    [[maybe_unused]] const bool deleteReverse =
        multimapDeleteWithReference(&ma->mapAtomReverse, &value, ma, &found);
    assert(deleteReverse && found.data.u == ref->data.u);

    multimapDeleteEntry(&ma->mapAtomForward, &me);
    atomAfterDelete_(ma, ref->data.u);
    return true;
}

bool multimapAtomDeleteById(multimapAtom *ma, const uint64_t id) {
    databox ref = {.data.u = id, .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
    return multimapAtomDeleteByRef(ma, &ref);
}

/* ====================================================================
 * Reporting
 * ==================================================================== */
size_t multimapAtomCount(const multimapAtom *ma) {
    assert(multimapCount(ma->mapAtomForward) ==
           multimapCount(ma->mapAtomReverse));

    return multimapCount(ma->mapAtomForward);
}

size_t multimapAtomBytes(const multimapAtom *ma) {
    assert(multimapCount(ma->mapAtomForward) ==
           multimapCount(ma->mapAtomReverse));

    return multimapBytes(ma->mapAtomForward) +
           multimapBytes(ma->mapAtomReverse);
}

/* ====================================================================
 * Persistence (atomPersist.h)
 * ==================================================================== */
#define ATOM_PERSIST_FLAG_PERMANENT_ 1u

/* A stored count (0-based: 0 is one reference) as a number; FieldIncr
 * leaves it FALSE, TRUE or an integer */
static bool atomPersistCountOf_(const databox *box, uint64_t *count) {
    switch (box->type) {
    case DATABOX_FALSE:
        *count = 0;
        return true;
    case DATABOX_TRUE:
        *count = 1;
        return true;
    case DATABOX_UNSIGNED_64:
        *count = box->data.u64;
        return true;
    case DATABOX_SIGNED_64:
        *count = (uint64_t)box->data.i64;
        return box->data.i64 >= 0;
    default:
        return false;
    }
}

static uint8_t *multimapAtomPersistSnapshot_(const void *structure,
                                             size_t *len) {
    const multimapAtom *ma = structure;
    /* @own: caller; free=zfree */
    flex *f = flexNew();
    const databox flags = DATABOX_UNSIGNED(
        multimapAtomIsPermanent(ma) ? ATOM_PERSIST_FLAG_PERMANENT_ : 0);
    const databox next = DATABOX_UNSIGNED(atomNextId_(ma));
    const databox *header[2] = {&flags, &next};
    flexAppendMultipleExact(&f, 2, header);

    /* Rows in id order (the forward map's order), 21 at a time */
    enum { BATCH = 21 };
    databox boxes[BATCH * 3];
    databoxWide wide[BATCH * 3];
    const databox *ptrs[BATCH * 3];
    size_t used = 0;
    multimapIterator iter;
    multimapIteratorInit(ma->mapAtomForward, &iter, true);
    while (true) {
        /* A narrow decode leaves the flag bits as they were: zero them */
        databox *row[3] = {&boxes[used * 3], &boxes[used * 3 + 1],
                           &boxes[used * 3 + 2]};
        for (size_t i = 0; i < 3; i++) {
            *row[i] = (databox){};
        }

        const bool more =
            multimapIteratorNextWithSlots(&iter, row, &wide[used * 3]);
        if (more) {
            uint64_t count = 0;
            atomPersistCountOf_(row[2], &count);
            boxes[used * 3].type = DATABOX_UNSIGNED_64; /* the id */
            boxes[used * 3 + 2] = DATABOX_UNSIGNED(count);
            for (size_t i = 0; i < 3; i++) {
                ptrs[used * 3 + i] = &boxes[used * 3 + i];
            }
            used++;
        }

        if (used && (used == BATCH || !more)) {
            flexAppendMultipleExact(&f, (uint_fast32_t)(used * 3), ptrs);
            used = 0;
        }

        if (!more) {
            break;
        }
    }

    *len = flexBytes(f);
    return (uint8_t *)f;
}

/* The next entry as an unsigned 64-bit value */
static bool atomPersistUnsigned_(const flex *f, flexEntry **fe, uint64_t *out) {
    databoxBig box;
    flexGetByTypeBig(*fe, &box);
    *fe = flexNext(f, *fe);
    *out = box.data.u64;
    return box.type == DATABOX_UNSIGNED_64;
}

/* A value an atom may intern from persisted bytes: a number, true,
 * false, null or bytes */
bool atomPersistValueOk(const databox *value) {
    switch (value->type) {
    case DATABOX_SIGNED_64:
    case DATABOX_UNSIGNED_64:
    case DATABOX_SIGNED_128:
    case DATABOX_UNSIGNED_128:
    case DATABOX_FLOAT_32:
    case DATABOX_DOUBLE_64:
    case DATABOX_TRUE:
    case DATABOX_FALSE:
    case DATABOX_NULL:
    case DATABOX_BYTES:
    case DATABOX_BYTES_EMBED:
        return true;
    default:
        return false;
    }
}

static int atomPersistCompareValues_(const void *a, const void *b) {
    return databoxCompare(*(const databox *const *)a,
                          *(const databox *const *)b);
}

static void *multimapAtomPersistRestore_(const uint8_t *data, size_t len) {
    if (!flexValidateBufferDeep(data, len)) {
        return NULL;
    }

    const flex *f = (const flex *)data;
    const size_t n = flexCount(f);
    flexEntry *fe = flexHead(f);
    uint64_t flags;
    uint64_t next;
    if (n < 2 || (n - 2) % 3 || !atomPersistUnsigned_(f, &fe, &flags) ||
        !atomPersistUnsigned_(f, &fe, &next) ||
        (flags & ~(uint64_t)ATOM_PERSIST_FLAG_PERMANENT_) ||
        next >= MULTIMAP_ATOM_ID_LIMIT) {
        return NULL;
    }

    cleaner(multimapAtomFree) multimapAtom *ma =
        (flags & ATOM_PERSIST_FLAG_PERMANENT_) ? multimapAtomNewPermanent()
                                               : multimapAtomNew();
    const bool permanent = multimapAtomIsPermanent(ma);
    const size_t atoms = (n - 2) / 3;

    /* Each value, kept in place (a 96/128-bit box points into itself) and
     * sorted through pointers, so the reverse map is built in value order
     * instead of searched once per atom */
    cleanerName(zfree, Void) void *storage =
        zcalloc(atoms ? atoms : 1,
                sizeof(databoxBig) + sizeof(databoxBig *) + sizeof(uint64_t));
    databoxBig *values = storage;
    databoxBig **byValue = (databoxBig **)(values + atoms);
    uint64_t *ids = (uint64_t *)(byValue + atoms);
    for (size_t r = 0; r < atoms; r++) {
        uint64_t count;
        databoxBig countBox;
        if (!atomPersistUnsigned_(f, &fe, &ids[r])) {
            return NULL;
        }

        flexGetByTypeBig(fe, &values[r]);
        fe = flexNext(f, fe);
        flexGetByTypeBig(fe, &countBox);
        fe = flexNext(f, fe);

        /* Ids ascend below the next id; a permanent table keeps no counts */
        if ((r && ids[r] <= ids[r - 1]) || ids[r] >= next ||
            !atomPersistValueOk((const databox *)&values[r]) ||
            countBox.type != DATABOX_UNSIGNED_64 ||
            !atomPersistCountOf_((const databox *)&countBox, &count) ||
            count > INT64_MAX || (permanent && count)) {
            return NULL;
        }

        const databox ref = {.data.u = ids[r],
                             .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
        const databox countStored =
            count ? DATABOX_UNSIGNED(count) : (databox){.type = DATABOX_FALSE};
        const databox *forwardRow[3] = {&ref, (const databox *)&values[r],
                                        &countStored};
        multimapAppend(&ma->mapAtomForward, forwardRow);
        byValue[r] = &values[r];
    }

    /* The ids in value order; the reverse map's builder refuses a value
     * bound twice */
    qsort(byValue, atoms, sizeof(*byValue), atomPersistCompareValues_);
    /* The reverse map's builder takes the rows */
    /* @own: container; free=multimapFree */
    flex *rows = flexNew();
    for (size_t r = 0; r < atoms; r++) {
        const databox ref = {.data.u = ids[byValue[r] - values],
                             .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
        flexPushByType(&rows, &ref, FLEX_ENDPOINT_TAIL);
    }

    /* @own: container; free=multimapAtomFree */
    multimap *reverse =
        multimapNewFromDumpWithReference(1, FLEX_CAP_LEVEL_512, rows, ma);
    if (!reverse) {
        return NULL;
    }

    multimapFree(ma->mapAtomReverse);
    ma->mapAtomReverse = reverse;
    atomSetNextId_(ma, next);
    multimapAtom *out = ma;
    ma = NULL;
    return out;
}

static uint8_t *multimapAtomPersistEncodeOp_(persistOp op, const void *args,
                                             size_t argc, size_t *len) {
    *len = 0;
    const bool shaped = (op == PERSIST_OP_INSERT && argc == 2) ||
                        (op == PERSIST_OP_NUM_INCR && argc == 2) ||
                        (op == PERSIST_OP_DELETE && argc == 1);
    if (!shaped) {
        return NULL;
    }
    return persistOpEncodeBoxes(args, argc, len);
}

/* The id box of a WAL op as a reference to a live atom */
static bool atomPersistLiveRef_(const multimapAtom *ma, const databoxBig *id,
                                databox *ref) {
    if (id->type != DATABOX_UNSIGNED_64 ||
        id->data.u64 >= MULTIMAP_ATOM_ID_LIMIT) {
        return false;
    }

    *ref = (databox){.data.u = id->data.u64,
                     .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
    return multimapExists(ma->mapAtomForward, ref);
}

static bool multimapAtomPersistApplyOp_(void *structure, persistOp op,
                                        const uint8_t *data, size_t len) {
    multimapAtom *ma = *(multimapAtom **)structure;
    databoxBig a;
    databoxBig b;
    databox ref;
    switch (op) {
    case PERSIST_OP_INSERT: {
        /* A new atom, with one reference, under exactly the id the
         * writer's table gave it */
        if (!flexGetTwoDataboxesBig(data, len, &a, &b) ||
            a.type != DATABOX_UNSIGNED_64 ||
            a.data.u64 >= MULTIMAP_ATOM_ID_LIMIT - 1 ||
            !atomPersistValueOk((const databox *)&b) ||
            atomPersistLiveRef_(ma, &a, &ref)) {
            return false;
        }

        return !atomFindOrCreateWithId_(ma, (const databox *)&b, a.data.u64,
                                        &ref);
    }
    case PERSIST_OP_NUM_INCR:
        /* One reference more (+1) or fewer (-1; the last one deletes) */
        if (!flexGetTwoDataboxesBig(data, len, &a, &b) ||
            !atomPersistLiveRef_(ma, &a, &ref) ||
            (b.type != DATABOX_SIGNED_64 && b.type != DATABOX_UNSIGNED_64)) {
            return false;
        }

        if (b.data.i64 == 1) {
            multimapAtomRetainByRef(ma, &ref);
            return true;
        }

        if (b.type == DATABOX_SIGNED_64 && b.data.i64 == -1) {
            conformRefcountDecr(ma, &ref, NULL);
            return true;
        }

        return false;
    case PERSIST_OP_DELETE:
        return flexGetSingleDataboxBig(data, len, &a) &&
               atomPersistLiveRef_(ma, &a, &ref) &&
               multimapAtomDeleteByRef(ma, &ref);
    default:
        return false;
    }
}

static void *multimapAtomPersistNewEmpty_(void) {
    /* @own: caller; free=multimapAtomFree */
    return multimapAtomNew();
}

static size_t multimapAtomPersistCount_(const void *structure) {
    return multimapAtomCount(structure);
}

static size_t multimapAtomPersistBytes_(const void *structure) {
    return multimapAtomBytes(structure);
}

static bool multimapAtomPersistValidate_(const void *structure) {
    const multimapAtom *ma = structure;
    return multimapCount(ma->mapAtomForward) ==
           multimapCount(ma->mapAtomReverse);
}

static void multimapAtomPersistFree_(void *structure) {
    multimapAtomFree(structure);
}

const persistOps persistOpsMultimapAtom = {
    .type = PERSIST_TYPE_MULTIMAP_ATOM,
    .name = "multimapAtom",
    .formatVersion = 1,
    .minReadVersion = 1,
    .snapshot = multimapAtomPersistSnapshot_,
    .restore = multimapAtomPersistRestore_,
    .newEmpty = multimapAtomPersistNewEmpty_,
    .count = multimapAtomPersistCount_,
    .estimateSize = multimapAtomPersistBytes_,
    .encodeOp = multimapAtomPersistEncodeOp_,
    .applyOp = multimapAtomPersistApplyOp_,
    .validate = multimapAtomPersistValidate_,
    .free = multimapAtomPersistFree_,
};

#ifdef DATAKIT_TEST
void multimapAtomRepr(const multimapAtom *ma) {
    printf("Forward Maps:\n");
    multimapRepr(ma->mapAtomForward);

    printf("Reverse Maps:\n");
    multimapRepr(ma->mapAtomReverse);
}

/* Callback for iterator */
static bool populateSet(void *userData, const databox *elements[]) {
    multimap **setMap = userData;

    multimapInsert(setMap, elements);

    return true;
}

#include "ctest.h"
#include "multimapFull.h"
#include "timeUtil.h"
#include <inttypes.h>

defcleanerName(uint64_t *, zfree, U64);
defcleaner(multimap *, multimapFree);

/* Fill the stack below the caller with 0xFF so a temporary that is read
 * before it is set up sees big = 1 and an all-ones pointer.  ASan is off
 * here so the array is not framed by redzones. */
static __attribute__((noinline, no_sanitize("address"))) void
atomTestPoisonStack(void) {
    volatile uint8_t junk[8192];
    for (size_t i = 0; i < sizeof(junk); i++) {
        junk[i] = 0xFF;
    }
}

/* True when 'ref' resolves to a value equal to 'want', read through an
 * armed databoxBig so a 96/128-bit value comes back whole.  Equal by
 * value, not type: storage keeps the canonical form, so a non-negative
 * signed value reads back unsigned. */
static bool atomTestResolvesTo(const multimapAtom *ma, const databox *ref,
                               const databox *want) {
    databoxBig got;
    memset(&got, 0xFF, sizeof(got));
    *(databox *)&got = (databox){};
    DATABOX_BIG_INIT(&got);
    atomTestPoisonStack();
    if (!multimapAtomLookup(ma, ref, (databox *)&got)) {
        return false;
    }

    if ((got.type == DATABOX_SIGNED_128 || got.type == DATABOX_UNSIGNED_128) &&
        got.data.u128 != (__uint128_t *)got.extra) {
        return false;
    }

    return databoxCompare((const databox *)&got, want) == 0;
}

/* A reference id box as the atom table hands it out: every flag clear. */
static bool atomTestIsCleanRef(const databox *box, const uint64_t id) {
    return box->type == DATABOX_CONTAINER_REFERENCE_EXTERNAL &&
           box->data.u == id && !box->big && !box->allocated;
}
int multimapAtomTest(int, char *[]) {
    int err = 0;

    uint32_t seed = time(NULL);
    srandom(seed);
    printf("Random seed: %" PRIu32 "\n", seed);

    TEST("create / free") {
        multimapAtom *ma = multimapAtomNew();
        multimapAtomFree(ma);
    }

    TEST("populate duplicates") {
        multimapAtom *ma = multimapAtomNew();
        char *greeting = "HELLO THERE AND EVEN MORE OF a HELLO to YOU!";
        const databox box = {.type = DATABOX_BYTES,
                             .len = strlen(greeting),
                             .data.bytes.cstart = greeting};

        for (size_t i = 0; i < 88; i++) {
            multimapAtomInsertWithExactAtomID(ma, i, &box);
        }

        const size_t forwardLen = multimapCount(ma->mapAtomForward);
        const size_t reverseLen = multimapCount(ma->mapAtomForward);

        multimapAtomRepr(ma);

        assert(forwardLen == 1);
        assert(reverseLen == 1);

        multimapAtomFree(ma);
    }

    /* This was a known-failing multimapFull lookup condition because multimap
     * interface wasn't propagating referenceContainer to
     * multimapUpgradeIfNecessary(), but now it is, so this test can't fail
     * again. If it does fail again, we've seriously screwed up somewhere else.
     */
#if 1
    TEST("manifest failing case for searching") {
        const int16_t val0[] = {
            -111, -177, -151, 19,   -117, -76,  -147, -228, 311,  177,  183,
            -195, -265, -31,  -178, 248,  -303, -172, -153, -388, 46,   122,
            -23,  -281, 227,  346,  -30,  134,  258,  -369, -128, -295, 362,
            187,  207,  -325, 167,  -223, -125, -98,  -181, -379, -290, 283,
            -279, -375, -309, -204, -291, 48,   212,  -6,   -96,  208,  324,
            -399, -216, -55,  53,   235,  344,  399,  265,  72,   340,  -247,
            314,  -120, 329,  -208, -344, 77,   -273, -236, 35,   78,   -339,
            299,  360,  -132, -94,  -15,  306,  253,  -48,  -326, 193,  335,
            96,   155,  114,  178,  194,  -299, 385,  40,   372,  381,  286,
            -327, 188,  -212, 379,  69,   7,    24,   162,  -24,  192,  164,
            -340, -20,  -302, 382,  389,  -308, -209, -257, 10,   302,  -255,
            -183, 325,  -122, -342, 2,    170,  -103, -263, -239, -352, -298,
            -171, 173,  319,  79,   -322, -115, 88,   -346, 296,  330,  99,
            305,  -123, 189,  109,  190,  -267, -60,  9,    289,  168,  197,
            388,  83,   -227, -320, 63,   182,  -262, -127, -176, -38,  284,
            392,  300,  -348, -33,  -139, -72,  -65,  291,  112,  -58,  185,
            303,  345,  -356, 149,  -374, 84,   -143, -78,  336,  322,  -218,
            393,  176,  233,  361,  366,  -394, 364,  -215, 64,   -66,  -370,
            -293, 318};

        const int16_t val1[] = {
            221,  -92,  -177, 383,  275,  123,  -328, -228, -36,  339,  -31,
            248,  59,   49,   -172, -230, -14,  -354, -246, 363,  201,  -297,
            227,  346,  -71,  134,  263,  -185, -369, -30,  -295, 362,  -325,
            73,   20,   320,  -392, -288, -204, 359,  -307, 61,   -188, -312,
            44,   -187, -319, 76,   54,   -201, -399, -216, -101, -55,  132,
            279,  342,  16,   399,  -285, 97,   257,  340,  -12,  -34,  -120,
            -304, -86,  146,  -150, 128,  116,  -56,  -21,  360,  299,  333,
            -95,  135,  253,  -202, 96,   -162, 397,  -229, -138, -157, -299,
            385,  37,   243,  -269, -109, 160,  95,   199,  -93,  -200, -323,
            22,   -220, -212, 394,  -238, 298,  -199, -156, 175,  224,  147,
            -315, 94,   164,  351,  -142, -302, -316, 41,   119,  -268, 56,
            -237, -209, -160, -337, -46,  -134, -255, -183, 30,   -122, 133,
            2,    -252, -305, 173,  319,  102,  350,  -144, -40,  -322, -115,
            -173, -385, -357, -329, -249, 226,  189,  -197, -84,  190,  -41,
            -85,  143,  -393, 195,  -367, -214, 354,  169,  -320, 182,  -366,
            -335, -91,  -225, -176, 392,  308,  -259, 249,  300,  -7,   373,
            -33,  -13,  104,  -72,  68,   -242, 140,  338,  -58,  185,  288,
            -356, -338, -107, -11,  336,  -394, 105,  29,   -190, 25,   -26,
            -106, -53};

        const int16_t val2[] = {
            221,  70,   -177, 383,  -90,  -318, -350, 339,  -31,  384,  -264,
            -230, -14,  -297, -97,  134,  234,  204,  100,  362,  -128, 207,
            -325, 73,   -179, 223,  -309, -204, 359,  -307, 309,  -77,  -135,
            -353, 48,   44,   -312, -10,  76,   -193, 369,  -96,  208,  148,
            4,    -399, -216, -217, 74,   347,  399,  72,   257,  340,  -198,
            -247, -2,   329,  -232, 128,  158,  228,  -56,  -21,  150,  360,
            -95,  -15,  378,  -202, 202,  193,  -254, 397,  292,  155,  114,
            -25,  194,  241,  243,  -269, 290,  160,  -158, 199,  205,  28,
            -45,  381,  286,  -238, 298,  276,  246,  -199, 370,  224,  -336,
            87,   192,  -121, -142, 349,  390,  41,   382,  206,  56,   389,
            -160, 395,  10,   337,  -9,   -183, 145,  325,  -342, 65,   348,
            2,    170,  219,  391,  -194, 315,  108,  -298, -35,  -332, -196,
            -322, -115, 88,   296,  127,  -385, 80,   -334, -222, 305,  -170,
            -146, 353,  200,  124,  -60,  -251, 9,    168,  -393, -4,   -214,
            -114, 83,   107,  -286, -256, 38,   63,   261,  282,  57,   50,
            -380, 308,  -250, -259, 165,  -348, 17,   -33,  -13,  31,   317,
            68,   338,  -58,  259,  42,   149,  247,  -107, 62,   -11,  176,
            131,  141,  -363, 233,  366,  -276, -182, 364,  -215, -219, 25,
            334,  -101};

        const int16_t val3[] = {
            -341, 70,   -177, 383,  -22,  123,  113,  -117, -260, 93,   -350,
            -76,  -306, -345, -264, -388, 85,   -191, -148, -243, -231, 271,
            -30,  346,  -141, -128, 362,  187,  -205, -75,  -223, 174,  -203,
            376,  -288, -204, 301,  -9,   184,  -188, -353, -193, 208,  151,
            324,  -145, 4,    -105, 52,   132,  74,   344,  23,   -400, 340,
            -198, -28,  -34,  -273, 77,   5,    186,  -232, 128,  -54,  237,
            213,  239,  14,   66,   -339, -5,   306,  135,  253,  -48,  -110,
            335,  -100, 292,  194,  385,  295,  40,   -158, -47,  240,  -200,
            -154, -323, -220, 188,  -212, 394,  327,  379,  103,  115,  246,
            276,  -330, 162,  224,  -57,  -24,  41,   119,  -26,  -371, 389,
            56,   -1,   1,    395,  -46,  -61,  -134, -180, -255, -183, 145,
            -292, 325,  374,  -122, -64,  -108, 2,    -211, -79,  -252, -352,
            391,  315,  108,  -314, -298, 173,  -40,  -270, -332, 36,   -115,
            296,  80,   -386, 267,  285,  305,  -123, 189,  -197, 341,  124,
            120,  269,  -251, 9,    137,  -235, -393, 197,  388,  169,  -286,
            -256, -175, -366, -91,  -225, 50,   -380, -250, 249,  -168, -348,
            -67,  8,    -139, 140,  332,  185,  26,   303,  149,  -143, 322,
            -102, 180,  176,  -126, 29,   64,   -66,  -190, 222,  21,   -106,
            -53,  -3};

        const int16_t val4[] = {
            221,  -92,  -81,  -22,  106,  -151, 34,   -90,  -260, -70,  -234,
            -195, -361, -36,  154,  248,  49,   -230, -300, -148, -377, 136,
            -243, -324, -253, -231, 227,  100,  207,  167,  -223, -125, 223,
            376,  -204, 359,  309,  184,  61,   -188, -140, -135, -10,  -319,
            -161, -193, -96,  -389, -145, 54,   148,  294,  310,  -216, -217,
            279,  235,  74,   -224, 265,  399,  -317, -287, 340,  -198, -28,
            -2,   -381, -344, 5,    -86,  -150, -152, 35,   158,  228,  -56,
            -54,  -373, 239,  66,   -15,  -326, -133, -202, 202,  193,  386,
            -244, -138, 114,  -174, -157, 37,   243,  290,  40,   -109, -43,
            199,  -93,  240,  -323, 142,  365,  -238, 370,  224,  24,   111,
            162,  -57,  351,  349,  231,  214,  -390, -371, 382,  56,   389,
            -257, 1,    321,  -160, 316,  -61,  -134, 274,  11,   -292, 30,
            325,  133,  250,  -284, -103, -239, -252, -19,  -35,  398,  12,
            319,  79,   256,  -115, -313, 127,  -249, 396,  -17,  -170, 189,
            -68,  200,  -8,   -311, -60,  -206, -114, 121,  388,  -99,  -256,
            -227, 181,  179,  -335, -331, 308,  39,   -310, -250, -348, 317,
            68,   -164, -65,  334,  -242, 209,  -248, 26,   -131, -87,  -42,
            -80,  203,  393,  180,  131,  141,  -182, -215, -370, 25,   222,
            21,   -293};

        const int16_t val5[] = {
            221,  -111, -22,  -151, 255,  34,   -207, -147, -70,  183,  -306,
            339,  -192, 154,  -178, 384,  -264, 110,  -14,  -246, 380,  216,
            -253, 227,  346,  134,  -128, -130, -325, -52,  139,  220,  20,
            320,  -181, 283,  -375, 301,  184,  -140, -312, 48,   -294, -135,
            60,   -193, -389, 151,  -145, -105, -360, 279,  344,  -83,  -247,
            -2,   314,  -304, 186,  146,  -232, -152, 116,  -82,  -373, 213,
            150,  236,  -226, 66,   -95,  -132, -5,   306,  135,  -48,  -326,
            335,  397,  155,  385,  37,   160,  372,  -154, -323, 157,  -159,
            217,  298,  69,   246,  370,  7,    111,  -121, 87,   192,  164,
            156,  245,  -340, 231,  214,  41,   171,  -383, -1,   210,  -124,
            -160, 10,   -61,  145,  374,  -122, -382, 65,   -362, 71,   2,
            170,  -284, 219,  -194, -298, -171, -116, 398,  -35,  79,   36,
            270,  256,  88,   244,  296,  -329, 330,  -249, 125,  -17,  305,
            -170, -283, -197, -84,  -41,  -8,   -267, -311, 371,  -60,  -4,
            -274, -214, -49,  354,  -286, 218,  -175, -225, -277, 284,  308,
            -18,  165,  -7,   -398, -33,  8,    104,  -396, 281,  334,  140,
            112,  -384, -278, 358,  185,  -296, -58,  288,  84,   336,  129,
            322,  -42,  -218, -102, 141,  364,  -66,  25,   222,  21,   -293,
            -167, 318};

        const int16_t val0_1[] = {
            5,    102,  349,  -78,  -203, 117,  301,  0,    -226, 323,  -197,
            -358, 232,  53,   307,  388,  398,  -278, 23,   395,  -372, 204,
            178,  -351, -109, -320, -232, 267,  -253, 230,  -229, -82,  -327,
            -50,  3,    348,  -361, 282,  -316, 293,  123,  341,  -40,  -347,
            -303, 250,  -248, 180,  207,  -213, 138,  125,  -371, 248,  -144,
            -393, -370, -11,  68,   46,   137,  325,  98,   219,  370,  -159,
            -68,  226,  25,   168,  -230, -70,  169,  132,  -95,  90,   -28,
            -136, -328, -202, 333,  233,  103,  -170, -211, 315,  298,  50,
            12,   -390, -284, -130, -48,  -63,  120,  -385, -240, -162, -354,
            177,  146,  -310, -182, -104, 165,  217,  -167, 86,   118,  -339,
            316,  -209, 399,  -271, 394,  55,   -116, -91,  -77,  387,  -399,
            -13,  147,  206,  214,  181,  312,  150,  -330, -37,  -288, -107,
            -58,  70,   -398, -191, -308, -184, -103, -225, -272, -363, 216,
            182,  179,  -237, 212,  167,  10,   275,  -26,  61,   40,   372,
            -92,  -61,  -179, 133,  -206, -33,  -171, 344,  -277, 266,  375,
            -285, -35,  2,    -238, 256,  -59,  -384, -321, -297, 80,   209,
            -302, -123, -110, -54,  73,   -173, 83,   104,  -51,  -249, -387,
            -5,   -156, -140, 228,  -163, -349, 342,  101,  -122, -8,   -324,
            45,   205};

        const int16_t val1_1[] = {
            1,    -53,  349,  -383, -203, 87,   321,  371,  306,  -275, 339,
            41,   -89,  318,  99,   -346, -149, -155, -223, 111,  230,  277,
            355,  319,  -82,  -327, -145, 392,  280,  282,  57,   257,  -46,
            89,   -139, -32,  -43,  -380, -303, 250,  290,  126,  385,  -201,
            269,  32,   -254, -228, 121,  -192, 138,  234,  -247, -379, 68,
            105,  278,  18,   386,  -214, -381, 325,  98,   -12,  -1,   -350,
            -241, 351,  25,   -45,  -70,  169,  185,  -204, 365,  -28,  208,
            383,  -283, 143,  -102, 144,  -245, 148,  72,   210,  -388, -289,
            -280, -344, 31,   -216, 298,  -391, -322, -196, 391,  227,  368,
            374,  124,  95,   -284, 218,  -130, 56,   -174, 120,  -385, 254,
            -65,  -165, 295,  356,  -367, -354, -84,  -355, -182, 246,  -104,
            165,  -199, -125, -60,  47,   -300, 273,  189,  202,  387,  -399,
            287,  93,   214,  -332, 181,  -246, 335,  -15,  -36,  -133, -288,
            350,  -184, 43,   264,  -368, 309,  -103, -141, -363, 110,  -260,
            -374, -108, 10,   -83,  221,  173,  -298, 61,   -61,  -360, 21,
            -56,  186,  -307, -33,  190,  171,  344,  4,    -277, -87,  -273,
            256,  -121, 194,  382,  322,  113,  209,  78,   289,  283,  -325,
            -54,  73,   -375, -234, 83,   -265, 108,  -249, -140, 63,   -163,
            -349, 96,   -171};

        const int16_t val2_1[] = {
            15,   92,   140,  349,  265,  139,  0,    -112, -197, 88,   -142,
            306,  242,  215,  -168, 398,  -278, -132, -231, 285,  -372, 178,
            -124, 28,   107,  -215, 267,  230,  -205, 332,  352,  3,    348,
            175,  -220, -219, -62,  280,  136,  -311, 293,  397,  -318, -43,
            65,   -74,  -326, 203,  -380, 250,  259,  207,  -254, 286,  308,
            141,  234,  82,   -393, -370, -11,  46,   347,  -381, 62,   -159,
            59,   -129, 34,   168,  -160, 334,  152,  -244, -212, -70,  -158,
            129,  -283, 144,  -134, 272,  72,   157,  210,  -259, -170, -280,
            -57,  -25,  -98,  -301, 220,  -47,  -391, -75,  368,  374,  124,
            -337, -284, -90,  218,  -48,  -63,  120,  -385, -65,  292,  20,
            -135, -162, 295,  -336, -157, -290, -113, -198, -199, -209, 394,
            -60,  -67,  -269, 376,  -13,  -29,  320,  -143, 93,   -309, 206,
            -332, -246, 150,  335,  16,   184,  350,  70,   24,   -111, -128,
            -272, 216,  -237, 212,  -374, 10,   -96,  17,   -26,  -304, 224,
            40,   346,  -252, -268, -16,  133,  -33,  190,  -76,  -171, 4,
            -277, 151,  -378, -285, -217, 33,   330,  -321, 343,  382,  322,
            378,  255,  209,  78,   -302, -123, -187, -54,  163,  -375, 239,
            -234, -173, -265, -156, -140, -305, -276, 342,  -42,  -122, 106,
            -270, 205};

        const int16_t val3_1[] = {
            -261, 213,  -78,  117,  -80,  -226, 321,  26,   -197, -142, 100,
            -299, 242,  306,  -151, 215,  -168, -278, 390,  -101, 327,  172,
            -55,  166,  -365, 178,  267,  -253, 159,  277,  319,  -229, -376,
            332,  -50,  -145, -333, 299,  175,  348,  238,  -62,  -316, 328,
            14,   291,  -43,  -40,  -347, -74,  -326, -380, 32,   345,  121,
            -192, 308,  138,  125,  248,  -247, 82,   -23,  -348, -233, 46,
            276,  386,  -381, 134,  325,  -154, 59,   358,  158,  201,  168,
            -287, 75,   -362, -70,  -138, -158, 208,  -127, 35,   144,  272,
            -202, 103,  -388, 244,  -289, -170, -98,  220,  231,  -322, -75,
            302,  368,  -270, -175, -90,  -147, 120,  -385, -65,  -165, -317,
            -357, -27,  -367, -221, 118,  -339, 399,  -386, 222,  66,   -282,
            393,  -300, -116, -67,  354,  287,  147,  206,  -332, 150,  335,
            -37,  16,   112,  -288, 350,  -398, 24,   264,  -111, -103, -225,
            -94,  -177, -237, -264, -218, 115,  167,  -69,  17,   -319, -304,
            -298, -56,  -341, -268, -262, 133,  -206, -274, -171, 11,   -277,
            33,   229,  256,  71,   -207, 91,   378,  -251, 54,   -302, 247,
            353,  -325, 73,   163,  -234, 211,  -331, 83,   -222, -51,  -387,
            -5,   228,  -163, -34,  -7,   -276, -193, 342,  101,  -42,  45,
            310,  340};

        const int16_t val4_1[] = {
            92,   5,    119,  -200, 85,   102,  349,  336,  117,  -78,  321,
            26,   371,  160,  -358, -314, 36,   -275, -151, 41,   377,  -118,
            215,  97,   337,  -89,  -9,   -149, -55,  -351, -223, -124, -320,
            -306, -229, -82,  260,  299,  -97,  238,  392,  280,  57,   -316,
            -32,  -318, 65,   203,  250,  259,  -21,  180,  -201, 32,   -254,
            338,  286,  198,  -371, 248,  82,   -393, 137,  46,   276,  278,
            -224, -41,  347,  325,  98,   62,   59,   226,  -129, 358,  158,
            296,  -397, -212, -138, 90,   -281, 365,  -127, 208,  -28,  -17,
            -283, 144,  148,  -364, -57,  -289, 300,  -216, 183,  298,  -391,
            231,  -120, -322, 391,  227,  302,  -337, -284, 218,  -4,   -385,
            292,  20,   -22,  -317, -356, -354, -27,  -182, 86,   274,  118,
            -114, 399,  19,   380,  -338, -300, -188, 393,  189,  202,  -334,
            311,  354,  -143, -137, 147,  313,  369,  -15,  16,   112,  -288,
            -107, 70,   -398, -191, 155,  -184, 8,    -128, -293, 199,  -177,
            -264, -374, -96,  17,   224,  249,  346,  21,   -312, -307, -262,
            -16,  190,  -19,  266,  -378, -217, -238, 49,   71,   -121, -207,
            -384, -210, 322,  378,  113,  -297, 54,   -123, -117, -110, -325,
            -54,  73,   -6,   -185, -265, 366,  -156, -172, -42,  -122, -8,
            396,  -324};

        const int16_t val5_1[] = {
            15,   -200, 102,  140,  349,  -78,  301,  321,  -112, 53,   242,
            377,  388,  398,  -93,  -89,  390,  -149, 164,  52,   -340, -231,
            285,  327,  395,  -223, 28,   -109, -320, 188,  277,  -79,  -306,
            319,  -82,  326,  260,  352,  299,  175,  -62,  -311, 123,  397,
            281,  -318, 291,  -74,  -119, 290,  -21,  269,  -213, 345,  -371,
            141,  156,  234,  84,   -247, -370, 329,  -233, 68,   46,   105,
            276,  -224, 386,  -214, 176,  -381, 98,   62,   226,  -241, 358,
            201,  -160, 334,  -95,  90,   208,  -245, 333,  153,  210,  157,
            -170, 298,  -322, -150, 374,  -337, -48,  -147, -14,  -189, 356,
            -357, -356, -354, 7,    146,  -395, -310, -182, -221, -199, -209,
            -131, -282, 360,  39,   47,   -300, 145,  -148, -116, 202,  -137,
            387,  -105, -91,  320,  -10,  181,  -36,  310,  -58,  -194, 384,
            -103, -141, -225, -20,  -257, 197,  -260, 199,  -237, -264, 115,
            -69,  191,  -294, -304, -298, 224,  249,  21,   372,  186,  -92,
            -312, -56,  -307, -268, -262, 116,  -206, 4,    -378, 193,  -238,
            -3,   256,  -207, 194,  -255, -210, 253,  80,   42,   195,  -251,
            -161, 353,  -187, 283,  -325, -54,  -331, 44,   -243, -265, -235,
            -72,  -222, 237,  -163, -305, -172, -122, -324, 106,  -270, 205,
            340,  -366};

        const int16_t val0_2[] = {
            -261, -17,  285,  -250, -331, -242, 131,  102,  387,  312,  -189,
            -237, -5,   -305, 230,  -267, -14,  -42,  305,  -317, 375,  -332,
            240,  158,  -357, 380,  -72,  178,  280,  -283, -220, -111, -10,
            -95,  171,  53,   275,  -238, 144,  239,  395,  224,  -396, 183,
            -131, -56,  136,  397,  315,  -203, -27,  -171, -181, 189,  328,
            361,  354,  70,   -346, 21,   -83,  276,  338,  -128, -375, -77,
            -327, 123,  -244, 295,  -93,  -349, 176,  31,   357,  -159, -48,
            175,  -119, 369,  39,   -8,   60,   -183, 134,  58,   -362, 40,
            294,  321,  -150, 192,  244,  336,  324,  -114, -58,  231,  -12,
            -302, -234, -19,  -194, 143,  191,  -147, 284,  -290, -16,  33,
            -278, 277,  81,   -347, 310,  -149, -180, 347,  -284, -271, 168,
            199,  209,  59,   -325, 385,  25,   55,   30,   -360, 359,  -241,
            108,  251,  341,  248,  -23,  282,  221,  -371, 186,  12,   -265,
            253,  365,  -39,  71,   -196, 172,  -134, -97,  -240, 303,  184,
            309,  -268, 6,    -239, -125, -206, 0,    355,  -25,  51,   -175,
            197,  210,  236,  -345, -270, -263, 149,  47,   -348, 139,  155,
            -34,  130,  89,   -351, 306,  -178, 196,  -366, -389, 112,  -51,
            -372, 363,  -300, 212,  314,  254,  -118, 28,   262,  -173, 200,
            -50,  -221};

        const int16_t val1_2[] = {
            205,  -46,  -17,  -250, 298,  -308, -145, -60,  -256, -242, 312,
            -189, -237, -229, -209, 62,   230,  194,  -310, 222,  342,  -14,
            122,  -321, -280, -3,   -332, -174, -80,  -172, 364,  120,  219,
            -220, -165, -111, -55,  85,   -86,  275,  217,  235,  144,  239,
            383,  266,  -292, 291,  388,  220,  106,  -181, -115, 137,  372,
            367,  -62,  334,  -319, 167,  -102, -110, 21,   -383, 76,   276,
            -192, 225,  -252, 378,  84,   -79,  -106, -375, 34,   -142, -113,
            -337, 49,   174,  249,  -369, 91,   103,  -159, -254, 273,  -88,
            -53,  125,  -162, -158, 169,  -358, 320,  161,  246,  -183, 111,
            -384, 3,    -52,  -150, -287, -87,  324,  -399, -114, 231,  289,
            -334, 20,   38,   143,  -194, -385, 284,  -208, -218, -390, -217,
            -281, -120, 245,  -109, 344,  -141, 310,  297,  -315, 238,  -284,
            206,  199,  -304, 209,  -379, 10,   -13,  272,  -32,  341,  330,
            -23,  270,  282,  148,  83,   -371, 253,  -247, -200, -43,  -129,
            362,  241,  -69,  356,  -36,  -202, -206, 107,  -353, -22,  35,
            -262, -25,  -61,  236,  -373, -31,  -105, 47,   -348, 139,  155,
            -295, 89,   95,   -351, 306,  -178, 353,  -366, -37,  389,  -391,
            349,  112,  227,  -214, 371,  -392, 164,  128,  -124, -85,  -253,
            267,  -221};

        const int16_t val2_2[] = {
            258,  331,  -338, 73,   129,  -17,  -324, -359, -193, -91,  -82,
            -361, -370, -60,  131,  -5,   194,  -14,  237,  77,   -354, 283,
            -273, 305,  -332, 165,  132,  350,  -357, 9,    340,  380,  -166,
            280,  -10,  120,  219,  -95,  -123, -356, 217,  63,   -30,  300,
            -396, -297, 136,  397,  -303, 106,  -148, -260, 328,  361,  322,
            -83,  21,   -224, -151, 386,  225,  -252, 84,   -286, -375, -398,
            -77,  -163, 123,  34,   -244, 295,  249,  357,  66,   -48,  273,
            -119, 125,  -162, 369,  -211, -393, -358, 320,  2,    60,   246,
            68,   50,   3,    -362, 244,  126,  324,  -399, 352,  -114, -302,
            -234, 104,  -272, 287,  -198, -194, 143,  -147, -385, -219, -103,
            146,  54,   -154, -197, -281, 335,  -278, 245,  -225, -185, 358,
            24,   -120, 81,   -400, 297,  -149, -315, -368, 15,   -255, -301,
            199,  59,   -325, -379, 55,   318,  10,   -352, -73,  341,  248,
            -100, -336, -23,  -344, 282,  148,  119,  -39,  -367, 253,  -247,
            214,  29,   -196, -43,  172,  -129, -134, -89,  48,   -240, -307,
            6,    113,  -202, 271,  377,  153,  -22,  223,  263,  -146, 202,
            -28,  -25,  75,   236,  82,   -117, -339, -34,  -167, -233, -195,
            196,  389,  -330, 227,  -214, -51,  212,  -201, 302,  -274, -387,
            267,  -221};

        const int16_t val3_2[] = {
            -84,  98,   129,  8,    313,  -81,  -17,  250,  -235, -145, -6,
            319,  211,  261,  -176, 230,  -267, -310, 122,  77,   -280, -354,
            283,  -329, -332, 165,  -174, -357, -341, -210, 364,  178,  -33,
            219,  85,   171,  -326, -112, 53,   -68,  217,  235,  383,  395,
            300,  -56,  183,  -297, -59,  -35,  154,  -148, -260, 372,  334,
            64,   -319, 386,  276,  -192, 338,  163,  84,   -138, -398, 370,
            -244, 22,   -93,  -349, 174,  176,  -311, -152, -245, 390,  91,
            -184, -78,  175,  273,  -312, 125,  -158, 369,  -108, -98,  50,
            58,   185,  360,  121,  -150, 192,  -232, 231,  -374, -302, 26,
            -157, 143,  -147, -290, -217, 33,   343,  -120, 277,  -225, -279,
            -70,  358,  24,   394,  -74,  187,  195,  -347, 297,  -333, -315,
            347,  -284, -271, 373,  379,  -126, 199,  351,  59,   209,  -379,
            385,  25,   69,   97,   -49,  -241, -187, 299,  -73,  -144, 341,
            -100, -276, 270,  282,  -168, 186,  -39,  253,  214,  71,   27,
            309,  157,  -202, 43,   -125, -282, 0,    153,  162,  -262, -146,
            -25,  202,  -135, 75,   393,  149,  -64,  -31,  82,   -348, 130,
            95,   -167, -195, 196,  -366, 353,  -92,  112,  -188, 392,  363,
            -51,  164,  254,  -257, 74,   376,  -289, 399,  -253, 93,   384,
            -50,  292};

        const int16_t val4_2[] = {
            -121, -133, 313,  8,    250,  19,   -82,  -361, 57,   -370, 131,
            102,  312,  -189, -237, -229, -14,  339,  117,  -364, -309, -273,
            283,  240,  140,  165,  -132, -174, 346,  -18,  -357, -341, -199,
            -130, 380,  -210, 364,  -75,  179,  280,  -10,  -316, -111, -86,
            -182, -95,  -112, 201,  -56,  232,  397,  -59,  -303, -203, 154,
            -190, -148, 372,  286,  334,  167,  -298, -224, -383, -343, 141,
            -252, 338,  188,  -314, -230, 110,  123,  304,  34,   -137, -244,
            -349, 174,  -355, 357,  151,  90,   -299, 390,  -296, -184, -78,
            -107, 96,   125,  -162, 39,   161,  60,   111,  68,   -108, -98,
            50,   -362, 360,  126,  332,  324,  -87,  -399, -99,  -234, 381,
            287,  1,    26,   -334, -198, -19,  -397, 284,  -218, -155, -16,
            -281, -109, 245,  -225, -140, -335, 344,  46,   -400, 127,  -347,
            297,  379,  -394, -255, 69,   30,   -13,  97,   150,  108,  -352,
            -24,  272,  78,   -276, 330,  -231, -57,  -45,  -371, 186,  32,
            -265, -168, 365,  -350, -196, -66,  172,  -129, -43,  362,  317,
            -307, -69,  113,  -202, -353, 153,  35,   124,  -28,  202,  -175,
            109,  -64,  -388, -294, 155,  -167, -391, -330, 349,  -92,  128,
            159,  279,  -124, 28,   -257, 79,   -289, 44,   -274, -173, -44,
            296,  292};

        const int16_t val5_2[] = {
            -121, -84,  -38,  331,  129,  313,  -17,  -81,  -235, -94,  -91,
            -82,  -291, 182,  -186, -237, 230,  36,   -267, -50,  122,  145,
            -280, 242,  140,  -132, 132,  346,  -365, -357, -318, -199, -130,
            -75,  -220, -111, -86,  -182, 67,   275,  -320, 183,  220,  -21,
            207,  -203, -204, 311,  -35,  -190, 372,  374,  -62,  361,  322,
            -110, -395, 141,  338,  163,  -138, -163, 110,  123,  34,   -142,
            37,   -244, -137, -113, 398,  -355, -243, -299, 390,  -119, -107,
            268,  -53,  115,  -162, 169,  -393, -358, 193,  -8,   -183, -136,
            111,  -384, 50,   3,    185,  213,  88,   -150, 323,  192,  126,
            -87,  231,  -12,  -41,  265,  -334, -157, -19,  38,   -147, -208,
            -219, -154, -71,  -278, 245,  -279, -258, -225, 358,  -143, -122,
            344,  310,  -149, 238,  -271, 56,   14,   226,  347,  199,  100,
            55,   30,   359,  396,  251,  11,   -24,  248,  330,  270,  -57,
            41,   83,   -371, -45,  170,  29,   -350, -43,  172,  -134, 27,
            184,  241,  309,  362,  87,   -69,  -239, -202, 107,  -282, 35,
            -262, -28,  208,  -31,  -105, -1,   155,  130,  -295, -195, -366,
            326,  -391, -92,  227,  392,  -372, -139, -300, 212,  -392, 164,
            128,  159,  279,  -85,  -257, 376,  -289, -387, 93,   4,    296,
            366,  -221};

        const int16_t *vals[] = {val0,   val1,   val2,   val3,   val4,
                                 val5,   val0_1, val1_1, val2_1, val3_1,
                                 val4_1, val5_1, val0_2, val1_2, val2_2,
                                 val3_2, val4_2, val5_2};

        const size_t backupDeleteBy = 5;
        const size_t loopExtent = 200;
        multimapAtom *ma = multimapAtomNew();

        for (size_t i = 0; i < COUNT_ARRAY(vals); i++) {
            const int16_t *use = vals[i];
            if (i >= backupDeleteBy) {
                /* Remove i - 5 */
                printf("Removing %zu\n", i - backupDeleteBy);
                const int16_t *remove = vals[i - backupDeleteBy];

                for (size_t j = 0; j < loopExtent; j++) {
                    databox box = databoxNewSigned(remove[j]);
                    const databox key = box;

                    databox count;
                    assert(multimapExistsWithReference(ma->mapAtomReverse, &key,
                                                       &count, ma));

                    /* Quick sanity check to make sure it does exist and isn't
                     * created if we do a re-insert of known-existing value. */
                    multimapAtomInsertIfNewConvert(ma, &box);
                    assert(box.created == false);

                    assert(multimapExistsWithReference(ma->mapAtomReverse, &key,
                                                       &count, ma));

                    const bool found =
                        multimapAtomLookupReference(ma, &key, &box);
                    if (!found) {
                        multimapAtomRepr(ma);
                    }

                    assert(found);

#if 0
                        databoxReprSay("Deleting", &key);
                        databoxReprSay("\tWITH REFERENCE", &box);
#endif
                    [[maybe_unused]] const bool deleted =
                        multimapAtomReleaseById(ma, &box);
#if 0
                    if (key.data.i == 25) {
                        printf("%sDELETED!\n", deleted ? "" : "NOT ");
                        printf("===========================\n");
                    }
#endif
                }
            }

#if 0
            printf("Populating %zu\n", i);
#endif
            for (size_t j = 0; j < loopExtent; j++) {
                databox box = databoxNewSigned(use[j]);
                const databox key = box;
#if 0
                    databoxReprSay("Adding", &key);
#endif

                multimapAtomInsertIfNewConvert(ma, &box);
#if 0
                    databoxReprSay("\tWITH REFERENCE", &box);
#endif

                databox count;
                assert(multimapExistsWithReference(ma->mapAtomReverse, &key,
                                                   &count, ma));

                /* If box was NOT created, that means it ALREADY existed, so we
                 * need to REFCOUNT it again so it only gets deleted after ALL
                 * usages are deleted.
                 *
                 * Alternatively, there is also
                 * multimapAtomInsertIfNewConvertAndRetain() */
                if (!box.created) {
                    multimapAtomRetainByRef(ma, &box);
                }
#if 0
                printf("===========================\n");
#endif
            }
        }

        multimapAtomFree(ma);
    }
#endif

    TEST("populate and half delete and populate (generate atom id)") {
        const ssize_t doUntil = 200;
        const size_t mod = random() % doUntil;
        printf("mod %zu\n", mod);
        multimapAtom *ma = multimapAtomNew();
        for (ssize_t j = 1; j <= 100; j++) {
            printf(".");
            fflush(stdout);
            for (ssize_t i = 0; i < doUntil; i++) {
                const int64_t populateVal = i % mod == 0 ? -i : i;
                databox box = databoxNewSigned(populateVal);
                databox obox = box;
                multimapAtomInsertIfNewConvert(ma, &box);

                const bool found =
                    multimapAtomLookupReference(ma, &obox, &obox);
                if (!found) {
                    multimapAtomRepr(ma);
                }

                assert(found);
                multimapAtomRetainByRef(ma, &box);
            }

            ssize_t forwardLen = multimapCount(ma->mapAtomForward);
            ssize_t reverseLen = multimapCount(ma->mapAtomReverse);

            if (forwardLen != doUntil) {
                ERR("Expected forward length to be %zd but it was %zd!\n",
                    doUntil, forwardLen);
                multimapAtomRepr(ma);
                assert(NULL);
            }

            if (reverseLen != doUntil) {
                ERR("Expected reverse length to be %zd but it was %zd!\n",
                    doUntil, reverseLen);
                multimapAtomRepr(ma);
                assert(NULL);
            }

            /* GO AWAY */
            const ssize_t valAdjust = random() % multimapAtomCount(ma);
            for (ssize_t i = valAdjust; i < doUntil; i++) {
                const int64_t populateVal = i % mod == 0 ? -i : i;
                databox box = databoxNewSigned(populateVal);
                const bool found = multimapAtomLookupReference(ma, &box, &box);
                if (!found) {
                    multimapAtomRepr(ma);
                }

                assert(found);

                /* Double release because, for testing, we do an extra Retain
                 * after we Create in the previous for loop, so it has a
                 * refcount of 2 at this point. */
                multimapAtomReleaseById(ma, &box);
                multimapAtomReleaseById(ma, &box);
            }
        }

        printf("\n");
        multimapAtomFree(ma);
    }

    TEST("populate and delete and populate (generate atom id)") {
        multimapAtom *ma = multimapAtomNew();
        /* Sizes on both sides of each tier change and of 450 (a size that
         * once crashed), then the largest; each cycle empties the table */
        static const ssize_t sizes[] = {1, 30, 90, 449, 450, 451, 900, 1800};
        for (size_t j = 0; j < sizeof(sizes) / sizeof(*sizes); j++) {
            printf(".");
            fflush(stdout);
            const ssize_t doUntil = sizes[j];
            for (ssize_t i = 0; i < doUntil; i++) {
                const int64_t populateVal = i; // % 2 == 0 ? -i : i;
                databox box = databoxNewSigned(populateVal);
                databox obox = box;
                multimapAtomInsertIfNewConvert(ma, &box);

                const bool found =
                    multimapAtomLookupReference(ma, &obox, &obox);
                if (!found) {
                    multimapAtomRepr(ma);
                }
                assert(found);

                multimapAtomRetainByRef(ma, &box);
            }

            ssize_t forwardLen = multimapCount(ma->mapAtomForward);
            ssize_t reverseLen = multimapCount(ma->mapAtomReverse);

            if (forwardLen != doUntil) {
                ERR("Expected forward length to be %zd but it was %zd!\n",
                    doUntil, forwardLen);
                multimapAtomRepr(ma);
                assert(NULL);
            }

            if (reverseLen != doUntil) {
                ERR("Expected reverse length to be %zd but it was %zd!\n",
                    doUntil, reverseLen);
                multimapAtomRepr(ma);
                assert(NULL);
            }

            /* GO AWAY */
            for (ssize_t i = 0; i < doUntil; i++) {
                const int64_t populateVal = i; // % 2 == 0 ? -i : i;
                databox box = databoxNewSigned(populateVal);
                const bool found = multimapAtomLookupReference(ma, &box, &box);
                if (!found) {
                    multimapAtomRepr(ma);
                }
                assert(found);

                /* Double release because, for testing, we do an extra Retain
                 * after we Create in the previous for loop, so it has a
                 * refcount of 2 at this point. */
                multimapAtomReleaseById(ma, &box);
                multimapAtomReleaseById(ma, &box);
            }

            /* By here, everything should be deleted... */
            forwardLen = multimapCount(ma->mapAtomForward);
            reverseLen = multimapCount(ma->mapAtomReverse);

            if (forwardLen != 0) {
                ERR("Expected forward length to be 0 but it was %zd!\n",
                    forwardLen);
                multimapAtomRepr(ma);
                assert(NULL);
            }

            if (reverseLen != 0) {
                ERR("Expected reverse length to be 0 but it was %zd!\n",
                    reverseLen);
                multimapAtomRepr(ma);
                assert(NULL);
            }
        }

        printf("\n");
        multimapAtomFree(ma);
    }

    TEST("populate and delete and populate (providing atom id)") {
        multimapAtom *ma = multimapAtomNew();
        static const ssize_t sizes[] = {0, 1, 30, 90, 449, 450, 451, 900, 1800};
        for (size_t j = 0; j < sizeof(sizes) / sizeof(*sizes); j++) {
            const ssize_t doUntil = sizes[j];
            for (ssize_t i = 0; i < doUntil; i++) {
                const int64_t populateVal = i % 2 == 0 ? -i : i;
                const databox box = databoxNewSigned(populateVal);
                multimapAtomInsertWithExactAtomID(ma, i, &box);

                const databox ref = {
                    .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL, .data.u = i};
                databox foundKey;
                const bool found = multimapAtomLookup(ma, &ref, &foundKey);
                if (!found) {
                    multimapAtomRepr(ma);
                }
                assert(databoxCompare(&foundKey, &box) == 0);
                assert(found);
            }

            ssize_t forwardLen = multimapCount(ma->mapAtomForward);
            ssize_t reverseLen = multimapCount(ma->mapAtomReverse);

            assert(forwardLen == doUntil);
            assert(reverseLen == doUntil);

            /* GO AWAY */
            for (ssize_t i = 0; i < doUntil; i++) {
                multimapAtomDeleteById(ma, i);
            }

            forwardLen = multimapCount(ma->mapAtomForward);
            reverseLen = multimapCount(ma->mapAtomReverse);

            assert(forwardLen == 0);
            assert(reverseLen == 0);
        }

        multimapAtomFree(ma);
    }

    TEST("populate lots (providing atom id)") {
        multimapAtom *ma = multimapAtomNew();
        const ssize_t doUntil = 70000;
        for (ssize_t i = 0; i < doUntil; i++) {
            const int64_t populateVal = i % 2 == 0 ? -i : i;
            const databox box = databoxNewSigned(populateVal);
            multimapAtomInsertWithExactAtomID(ma, i, &box);
        }

        const ssize_t forwardLen = multimapCount(ma->mapAtomForward);
        const ssize_t reverseLen = multimapCount(ma->mapAtomReverse);

        assert(forwardLen == doUntil);
        assert(reverseLen == doUntil);

        /* test for uniqueness in reverse list */
        cleaner(multimapFree) multimap *duplicateHolder = multimapSetNew(1);

        multimapPredicate p = {.condition = MULTIMAP_CONDITION_ALL};

        multimapProcessUntil(ma->mapAtomReverse, &p, true, populateSet,
                             &duplicateHolder);

        const size_t setSize = multimapCount(duplicateHolder);
        if (setSize != doUntil) {
            ERR("Expected %zd but got %zu instead!", doUntil, setSize);
            multimapAtomRepr(ma);
        }

        multimapAtomFree(ma);
    }

    TEST("high-scale stress test (20k entries)") {
        multimapAtom *ma = multimapAtomNew();
        const size_t numEntries = 20000;

        /* Insert all entries */
        int64_t startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < numEntries; i++) {
            databox box = databoxNewSigned((int64_t)i);
            multimapAtomInsertIfNewConvert(ma, &box);
        }
        int64_t insertNs = timeUtilMonotonicNs() - startNs;

        assert(multimapAtomCount(ma) == numEntries);
        printf("Inserted %zu entries in %.3f ms (%.0f/sec)\n", numEntries,
               insertNs / 1e6, numEntries / (insertNs / 1e9));

        /* Lookup all entries by reference */
        startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < numEntries; i++) {
            databox key = databoxNewSigned((int64_t)i);
            databox ref;
            bool found = multimapAtomLookupReference(ma, &key, &ref);
            assert(found);
        }
        int64_t lookupRefNs = timeUtilMonotonicNs() - startNs;
        printf("Lookup by key (reference): %zu lookups in %.3f ms (%.0f/sec)\n",
               numEntries, lookupRefNs / 1e6, numEntries / (lookupRefNs / 1e9));

        /* Lookup all entries by atom ID */
        startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < numEntries; i++) {
            databox ref = {.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                           .data.u = i};
            databox key;
            bool found = multimapAtomLookup(ma, &ref, &key);
            assert(found);
        }
        int64_t lookupIdNs = timeUtilMonotonicNs() - startNs;
        printf("Lookup by ID (key): %zu lookups in %.3f ms (%.0f/sec)\n",
               numEntries, lookupIdNs / 1e6, numEntries / (lookupIdNs / 1e9));

        /* Retain and release */
        startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < numEntries; i++) {
            databox ref = {.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                           .data.u = i};
            multimapAtomRetainByRef(ma, &ref);
        }
        int64_t retainNs = timeUtilMonotonicNs() - startNs;
        printf("Retain: %zu ops in %.3f ms (%.0f/sec)\n", numEntries,
               retainNs / 1e6, numEntries / (retainNs / 1e9));

        /* Memory efficiency */
        size_t bytesUsed = multimapAtomBytes(ma);
        printf("Memory: %zu bytes for %zu entries (%.2f bytes/entry)\n",
               bytesUsed, multimapAtomCount(ma),
               (double)bytesUsed / multimapAtomCount(ma));

        /* Release all (should not delete due to +1 retain) */
        for (size_t i = 0; i < numEntries; i++) {
            databox ref = {.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                           .data.u = i};
            multimapAtomReleaseById(ma, &ref);
        }
        assert(multimapAtomCount(ma) == numEntries);

        /* Release again (should delete all) */
        startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < numEntries; i++) {
            databox ref = {.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                           .data.u = i};
            multimapAtomReleaseById(ma, &ref);
        }
        int64_t releaseNs = timeUtilMonotonicNs() - startNs;
        printf("Release (delete): %zu ops in %.3f ms (%.0f/sec)\n", numEntries,
               releaseNs / 1e6, numEntries / (releaseNs / 1e9));

        assert(multimapAtomCount(ma) == 0);
        multimapAtomFree(ma);
    }

    TEST("lookup min/max correctness") {
        multimapAtom *ma = multimapAtomNew();

        /* Insert keys in random order */
        int64_t keys[] = {500, 100, 900, 300, 700, 200, 800, 400, 600, 1000};
        for (size_t i = 0; i < 10; i++) {
            databox box = databoxNewSigned(keys[i]);
            multimapAtomInsert(ma, &box);
        }

        /* Lookup minimum - should be 100 */
        databox minRef;
        bool found = multimapAtomLookupMin(ma, &minRef);
        assert(found);
        printf("Min ref: %" PRIuMAX " (expected ref for key 100)\n",
               minRef.data.u);

        /* Verify the min ref maps to key 100 */
        databox minKey;
        found = multimapAtomLookup(ma, &minRef, &minKey);
        assert(found);
        printf("Min key: %" PRIdMAX " (expected 100)\n", minKey.data.i);
        if (minKey.data.i != 100) {
            ERR("Expected min key to be 100 but got %" PRIdMAX, minKey.data.i);
        }

        multimapAtomFree(ma);
    }

    TEST("string key stress test") {
        multimapAtom *ma = multimapAtomNew();

        /* Insert string keys */
        const size_t numStrings = 10000;
        char buf[64];
        int64_t startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < numStrings; i++) {
            snprintf(buf, sizeof(buf), "key_%08zu_test", i);
            databox box = {.type = DATABOX_BYTES,
                           .len = strlen(buf),
                           .data.bytes.cstart = buf};
            multimapAtomInsertIfNewConvert(ma, &box);
        }
        int64_t insertNs = timeUtilMonotonicNs() - startNs;
        printf("Inserted %zu string keys in %.3f ms (%.0f/sec)\n", numStrings,
               insertNs / 1e6, numStrings / (insertNs / 1e9));

        assert(multimapAtomCount(ma) == numStrings);

        /* Lookup all string keys */
        startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < numStrings; i++) {
            snprintf(buf, sizeof(buf), "key_%08zu_test", i);
            databox key = {.type = DATABOX_BYTES,
                           .len = strlen(buf),
                           .data.bytes.cstart = buf};
            databox ref;
            bool found = multimapAtomLookupReference(ma, &key, &ref);
            assert(found);
        }
        int64_t lookupNs = timeUtilMonotonicNs() - startNs;
        printf("String key lookups: %zu in %.3f ms (%.0f/sec)\n", numStrings,
               lookupNs / 1e6, numStrings / (lookupNs / 1e9));

        multimapAtomFree(ma);
    }

    TEST("performance benchmark summary") {
        printf("\n=== MULTIMAP ATOM PERFORMANCE SUMMARY ===\n");

        multimapAtom *ma = multimapAtomNew();
        const size_t benchCount = 50000;

        /* Benchmark insert */
        int64_t startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < benchCount; i++) {
            databox box = databoxNewSigned((int64_t)i);
            multimapAtomInsertIfNewConvert(ma, &box);
        }
        int64_t insertNs = timeUtilMonotonicNs() - startNs;

        /* Benchmark lookup by key */
        startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < benchCount; i++) {
            databox key = databoxNewSigned((int64_t)i);
            databox ref;
            multimapAtomLookupReference(ma, &key, &ref);
        }
        int64_t lookupKeyNs = timeUtilMonotonicNs() - startNs;

        /* Benchmark lookup by ID */
        startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < benchCount; i++) {
            databox ref = {.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                           .data.u = i};
            databox key;
            multimapAtomLookup(ma, &ref, &key);
        }
        int64_t lookupIdNs = timeUtilMonotonicNs() - startNs;

        /* Benchmark retain */
        startNs = timeUtilMonotonicNs();
        for (size_t i = 0; i < benchCount; i++) {
            databox ref = {.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                           .data.u = i};
            multimapAtomRetainByRef(ma, &ref);
        }
        int64_t retainNs = timeUtilMonotonicNs() - startNs;

        printf("Insert rate:     %.0f ops/sec (%.1f us/op)\n",
               benchCount / (insertNs / 1e9),
               insertNs / (double)benchCount / 1000);
        printf("Lookup (by key): %.0f ops/sec (%.1f us/op)\n",
               benchCount / (lookupKeyNs / 1e9),
               lookupKeyNs / (double)benchCount / 1000);
        printf("Lookup (by ID):  %.0f ops/sec (%.1f us/op)\n",
               benchCount / (lookupIdNs / 1e9),
               lookupIdNs / (double)benchCount / 1000);
        printf("Retain rate:     %.0f ops/sec (%.1f us/op)\n",
               benchCount / (retainNs / 1e9),
               retainNs / (double)benchCount / 1000);
        printf(
            "Memory used:     %zu bytes for %zu entries (%.2f bytes/entry)\n",
            multimapAtomBytes(ma), multimapAtomCount(ma),
            (double)multimapAtomBytes(ma) / multimapAtomCount(ma));
        printf("==========================================\n\n");

        multimapAtomFree(ma);
    }

    TEST("exact atom id rebinding to a different key leaves no orphan") {
        multimapAtom *ma = multimapAtomNew();

        const databox keyA = databoxNewSigned(1111);
        const databox keyB = databoxNewSigned(2222);

        /* Bind id 5 -> keyA, then rebind the SAME id to keyB. */
        multimapAtomInsertWithExactAtomID(ma, 5, &keyA);
        multimapAtomInsertWithExactAtomID(ma, 5, &keyB);

        /* No duplicate/orphan: exactly one row per side. */
        if (multimapCount(ma->mapAtomForward) != 1) {
            ERR("forward has %zu rows for one id, expected 1",
                multimapCount(ma->mapAtomForward));
        }
        if (multimapCount(ma->mapAtomReverse) != 1) {
            ERR("reverse has %zu rows, expected 1",
                multimapCount(ma->mapAtomReverse));
        }

        /* id 5 resolves to the new key; the old key is unbound. */
        const databox ref = {.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                             .data.u = 5};
        databox foundKey;
        if (!multimapAtomLookup(ma, &ref, &foundKey) ||
            databoxCompare(&foundKey, &keyB) != 0) {
            ERR("id 5 did not resolve to the rebound key");
        }

        databox refOut;
        if (multimapAtomLookupReference(ma, &keyA, &refOut)) {
            ERR("old key A still bound after id rebind");
        }
        if (!multimapAtomLookupReference(ma, &keyB, &refOut) ||
            refOut.data.u != 5) {
            ERR("new key B not bound to id 5");
        }

        multimapAtomFree(ma);
    }

    TEST("reference lookups resolve 96/128-bit atom values") {
        /* Rows go straight into the forward map ({ID, value, refcount}):
         * every lookup under test reads only that map.  The values are
         * the 96/128-bit boundary set plus narrow and BYTES values, and
         * every output box starts 0xFF-poisoned, so stale 'big' bits
         * and a garbage payload pointer are always present. */
        multimapAtom *ma = multimapAtomNew();
        const __uint128_t one = 1;
        __uint128_t store[9] = {
            one << 64,          (one << 95) - 1, one << 95,
            one << 127,         ~(__uint128_t)0, ~(__uint128_t)0 >> 1,
            -(one << 63) - one, -(one << 95),    one << 127};
        enum { atomWideCount = 9, atomValueCount = 12 };
        databox vals[atomValueCount];
        for (size_t i = 0; i < atomWideCount; i++) {
            /* The last three are negative: -2^63-1, -2^95, INT128_MIN. */
            const bool neg = i >= 6;
            vals[i] = (databox){.type = neg ? DATABOX_SIGNED_128
                                            : DATABOX_UNSIGNED_128,
                                .big = true,
                                .data.u128 = &store[i]};
        }
        vals[9] = databoxNewSigned(-5);
        vals[10] = databoxNewUnsigned(77);
        vals[11] = databoxNewBytesString("a value longer than eight bytes");

        const uint64_t idBase = 100;
        for (size_t i = 0; i < atomValueCount; i++) {
            const databox ref = {.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                                 .data.u = idBase + i};
            const databox *row[3] = {&ref, &vals[i], &DATABOX_BOX_FALSE};
            multimapInsert(&ma->mapAtomForward, row);
        }

        /* Pass 0 runs on a Small forward map; pass 1 adds filler rows
         * until it is a Full map with several sub-maps. */
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                for (uint64_t f = 0; f < 400; f++) {
                    const databox ref = {
                        .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                        .data.u = 1000 + f};
                    const databox fill = databoxNewUnsigned(f);
                    const databox *row[3] = {&ref, &fill, &DATABOX_BOX_FALSE};
                    multimapInsert(&ma->mapAtomForward, row);
                }
            }

            for (size_t i = 0; i < atomValueCount; i++) {
                const databox *want = &vals[i];
                const bool wide = i < atomWideCount;
                const databox ref = {.type =
                                         DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                                     .data.u = idBase + i};

                databoxBig big;
                memset(&big, 0xFF, sizeof(big));
                big.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL;
                big.data.u = idBase + i;
                multimapAtomLookupConvertBig(ma, &big);
                if (big.type != want->type || big.big != wide ||
                    big.allocated) {
                    ERR("pass %d value %zu: ConvertBig gave type %d big %d "
                        "allocated %d",
                        pass, i, big.type, big.big, big.allocated);
                } else if (wide) {
                    if (big.data.u128 != (__uint128_t *)big.extra ||
                        *big.data.u128 != store[i]) {
                        ERR("pass %d value %zu: ConvertBig wide value wrong",
                            pass, i);
                    }
                } else if (databoxCompare((databox *)&big, want) != 0) {
                    ERR("pass %d value %zu: ConvertBig narrow value wrong",
                        pass, i);
                }

                databox plain;
                memset(&plain, 0xFF, sizeof(plain));
                plain.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL;
                plain.data.u = idBase + i;
                multimapAtomLookupConvert(ma, &plain);
                if (plain.type != want->type) {
                    ERR("pass %d value %zu: Convert gave type %d", pass, i,
                        plain.type);
                } else if (wide) {
                    if (plain.big || plain.data.ptr) {
                        ERR("pass %d value %zu: Convert of a wide value is "
                            "not type-only",
                            pass, i);
                    }
                } else if (databoxCompare(&plain, want) != 0) {
                    ERR("pass %d value %zu: Convert narrow value wrong", pass,
                        i);
                }

                multimapAtomResult result;
                memset(&result, 0xFF, sizeof(result));
                multimapAtomLookupResult(ma, &ref, &result);
                if (result.val.type != want->type) {
                    ERR("pass %d value %zu: Result gave type %d", pass, i,
                        result.val.type);
                } else if (wide && (result.val.big || result.val.data.ptr)) {
                    ERR("pass %d value %zu: Result of a wide value is not "
                        "type-only",
                        pass, i);
                }

                databox count = {};
                atomTestPoisonStack();
                multimapAtomLookupRefcount(ma, &ref, &count);
                if (count.type != DATABOX_FALSE) {
                    ERR("pass %d value %zu: refcount type %d", pass, i,
                        count.type);
                }
            }

            /* A reference that is not in the table stays a reference. */
            databoxBig miss;
            memset(&miss, 0xFF, sizeof(miss));
            miss.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL;
            miss.data.u = 999;
            multimapAtomLookupConvertBig(ma, &miss);
            if (miss.type != DATABOX_CONTAINER_REFERENCE_EXTERNAL ||
                miss.data.u != 999 || miss.big || miss.allocated) {
                ERR("pass %d: ConvertBig changed a missing reference", pass);
            }

            if (pass == 1 && multimapCount(ma->mapAtomForward) < 400) {
                ERR("filler rows missing from the forward map");
            }
        }

        multimapAtomFree(ma);
    }

    TEST("96/128-bit atom values through every insert, retain, release and "
         "delete path") {
        /* Each value is interned through the public API, then resolved,
         * retained, released and deleted through every entry point.  The
         * stack is 0xFF-poisoned before each call, so an internal box read
         * before it is set up has big = 1 and an all-ones pointer.  Pass 1
         * first interns filler atoms so both maps are Full. */
        const __uint128_t one = 1;
        __uint128_t store[9] = {
            one << 64,          (one << 95) - 1, one << 95,
            one << 127,         ~(__uint128_t)0, ~(__uint128_t)0 >> 1,
            -(one << 63) - one, -(one << 95),    one << 127};
        enum { pathWideCount = 9, pathValueCount = 13, pathFillCount = 900 };
        databox vals[pathValueCount];
        for (size_t i = 0; i < pathWideCount; i++) {
            /* The last three are negative: -2^63-1, -2^95, INT128_MIN. */
            const bool neg = i >= 6;
            vals[i] = (databox){.type = neg ? DATABOX_SIGNED_128
                                            : DATABOX_UNSIGNED_128,
                                .big = true,
                                .data.u128 = &store[i]};
        }
        vals[9] = databoxNewSigned(-5);
        vals[10] = databoxNewUnsigned(77);
        vals[11] = databoxNewBytesString("a value longer than eight bytes");
        vals[12] = databoxNewBytesString("abc");

        for (int pass = 0; pass < 2; pass++) {
            multimapAtom *ma = multimapAtomNew();
            const size_t fill = pass ? pathFillCount : 0;
            for (size_t f = 0; f < fill; f++) {
                const databox filler = databoxNewSigned(1'000'000 + (int64_t)f);
                multimapAtomInsert(ma, &filler);
            }

            for (size_t i = 0; i < pathValueCount; i++) {
                atomTestPoisonStack();
                multimapAtomInsert(ma, &vals[i]);
            }

            if (pass == 1) {
                /* An iterator records the tier of the map it walks. */
                multimapIterator forwardIter;
                multimapIterator reverseIter;
                multimapIteratorInit(ma->mapAtomForward, &forwardIter, true);
                multimapIteratorInit(ma->mapAtomReverse, &reverseIter, true);
                if (forwardIter.type != MULTIMAP_TYPE_FULL ||
                    reverseIter.type != MULTIMAP_TYPE_FULL) {
                    ERR("filler atoms did not make both maps Full");
                }
            }

            uint64_t ids[pathValueCount] = {};
            for (size_t i = 0; i < pathValueCount; i++) {
                databox ref = {};
                atomTestPoisonStack();
                if (!multimapAtomLookupReference(ma, &vals[i], &ref) ||
                    ref.type != DATABOX_CONTAINER_REFERENCE_EXTERNAL) {
                    ERR("pass %d value %zu: not interned", pass, i);
                    continue;
                }

                ids[i] = ref.data.u;
                if (!atomTestResolvesTo(ma, &ref, &vals[i])) {
                    ERR("pass %d value %zu: reference resolves wrong", pass, i);
                }
            }

            /* Four more references each, one per retain entry point, plus
             * a lookup convert that takes none.  Every converted box comes
             * back as a clean reference. */
            for (size_t i = 0; i < pathValueCount; i++) {
                atomTestPoisonStack();
                multimapAtomRetain(ma, &vals[i]);

                databox viaRetain = vals[i];
                atomTestPoisonStack();
                multimapAtomRetainConvert(ma, &viaRetain);
                if (!atomTestIsCleanRef(&viaRetain, ids[i])) {
                    ERR("pass %d value %zu: RetainConvert box wrong", pass, i);
                }

                databox viaInsertRetain = vals[i];
                atomTestPoisonStack();
                if (!multimapAtomInsertIfNewConvertAndRetain(
                        ma, &viaInsertRetain) ||
                    !atomTestIsCleanRef(&viaInsertRetain, ids[i])) {
                    ERR("pass %d value %zu: InsertIfNewConvertAndRetain box "
                        "wrong",
                        pass, i);
                }

                databox viaInsert = vals[i];
                atomTestPoisonStack();
                if (!multimapAtomInsertIfNewConvert(ma, &viaInsert) ||
                    !atomTestIsCleanRef(&viaInsert, ids[i])) {
                    ERR("pass %d value %zu: InsertIfNewConvert box wrong", pass,
                        i);
                }

                atomTestPoisonStack();
                multimapAtomRetainById(ma, ids[i]);
            }

            if (multimapAtomCount(ma) != fill + pathValueCount) {
                ERR("pass %d: retains changed the atom count", pass);
            }

            /* Five references each.  Remove every value through one of four
             * paths, then check that it is gone and that every other value
             * still resolves both ways. */
            bool live[pathValueCount];
            for (size_t i = 0; i < pathValueCount; i++) {
                live[i] = true;
            }

            size_t liveCount = fill + pathValueCount;
            for (size_t i = 0; i < pathValueCount; i++) {
                const databox ref = {.type =
                                         DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                                     .data.u = ids[i]};
                bool removed = false;
                switch (i % 4) {
                case 0:
                case 1:
                    for (int r = 0; r < 5; r++) {
                        atomTestPoisonStack();
                        removed = i % 4 == 0
                                      ? multimapAtomRelease(ma, &vals[i])
                                      : multimapAtomReleaseById(ma, &ref);
                        if (removed != (r == 4)) {
                            ERR("pass %d value %zu: release %d reported %d",
                                pass, i, r, removed);
                        }
                    }
                    break;
                case 2:
                    atomTestPoisonStack();
                    removed = multimapAtomDelete(ma, &vals[i]);
                    break;
                default:
                    atomTestPoisonStack();
                    removed = multimapAtomDeleteById(ma, ids[i]);
                    break;
                }

                if (!removed) {
                    ERR("pass %d value %zu: path %zu did not remove it", pass,
                        i, i % 4);
                }

                live[i] = false;
                liveCount--;
                if (multimapAtomCount(ma) != liveCount) {
                    ERR("pass %d value %zu: count %zu, expected %zu", pass, i,
                        multimapAtomCount(ma), liveCount);
                }

                atomTestPoisonStack();
                if (multimapAtomDeleteByRef(ma, &ref)) {
                    ERR("pass %d value %zu: deleted twice", pass, i);
                }

                const databox *wantMin = NULL;
                const databox fillMin = databoxNewSigned(1'000'000);
                if (fill) {
                    wantMin = &fillMin;
                }

                for (size_t j = 0; j < pathValueCount; j++) {
                    const databox refJ = {
                        .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                        .data.u = ids[j]};
                    databox found = {};
                    atomTestPoisonStack();
                    const bool bound =
                        multimapAtomLookupReference(ma, &vals[j], &found);
                    if (bound != live[j]) {
                        ERR("pass %d after %zu: value %zu bound %d, expected "
                            "%d",
                            pass, i, j, bound, live[j]);
                    } else if (live[j] &&
                               (!atomTestIsCleanRef(&found, ids[j]) ||
                                !atomTestResolvesTo(ma, &refJ, &vals[j]))) {
                        ERR("pass %d after %zu: value %zu resolves wrong", pass,
                            i, j);
                    }

                    if (live[j] &&
                        (!wantMin || databoxCompare(&vals[j], wantMin) < 0)) {
                        wantMin = &vals[j];
                    }
                }

                if (wantMin) {
                    databox minRef = {};
                    atomTestPoisonStack();
                    if (!multimapAtomLookupMin(ma, &minRef) ||
                        !atomTestResolvesTo(ma, &minRef, wantMin)) {
                        ERR("pass %d after %zu: minimum resolves wrong", pass,
                            i);
                    }
                }
            }

            /* Rebinding an exact id drops its old value through
             * DeleteByRef. */
            for (size_t i = 0; i < pathValueCount; i++) {
                const uint64_t exact = 5000000 + i;
                const databox ref = {.type =
                                         DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                                     .data.u = exact};
                const databox rebound = databoxNewSigned(-777 - (int64_t)i);
                atomTestPoisonStack();
                multimapAtomInsertWithExactAtomID(ma, exact, &vals[i]);
                atomTestPoisonStack();
                multimapAtomInsertWithExactAtomID(ma, exact, &rebound);

                databox found = {};
                atomTestPoisonStack();
                if (multimapAtomLookupReference(ma, &vals[i], &found)) {
                    ERR("pass %d value %zu: still bound after rebind", pass, i);
                }

                if (!atomTestResolvesTo(ma, &ref, &rebound) ||
                    multimapAtomCount(ma) != fill + 1) {
                    ERR("pass %d value %zu: rebind left the table wrong", pass,
                        i);
                }

                atomTestPoisonStack();
                if (!multimapAtomDeleteById(ma, exact) ||
                    multimapAtomCount(ma) != fill) {
                    ERR("pass %d value %zu: rebound id not deleted", pass, i);
                }
            }

            multimapAtomFree(ma);
        }
    }

    TEST("permanent: atoms outlive their references; ids never repeat") {
        multimapAtom *ma = multimapAtomNewPermanent();
        multimapAtom *reusing = multimapAtomNew();
        if (!multimapAtomIsPermanent(ma) || multimapAtomIsPermanent(reusing)) {
            ERR("the id behaviour is not what the constructor chose");
        }

        multimapAtomFree(reusing);

        databox ids[3];
        for (int i = 0; i < 3; i++) {
            ids[i] = databoxNewSigned(100 + i);
            multimapAtomInsertConvert(ma, &ids[i]);
            if (ids[i].data.u != (uint64_t)i) {
                ERR("atom %d got id %" PRIu64, i, (uint64_t)ids[i].data.u);
            }
        }

        const databox a = databoxNewSigned(100);
        for (int i = 0; i < 3; i++) {
            if (multimapAtomRelease(ma, &a) ||
                multimapAtomReleaseById(ma, &ids[0])) {
                ERR("a permanent atom reported being freed");
            }
        }

        multimapAtomRetainById(ma, ids[1].data.u);
        databox count = {};
        databox found = {};
        multimapAtomLookupRefcount(ma, &ids[1], &count);
        if (multimapAtomCount(ma) != 3 ||
            !multimapAtomLookup(ma, &ids[0], &found) || found.data.i != 100 ||
            count.data.u != 0) {
            ERR("Release or Retain changed a permanent table");
        }

        /* Delete the top atom, then everything: no id comes back */
        const databox c = databoxNewSigned(102);
        databox d = databoxNewSigned(103);
        if (!multimapAtomDelete(ma, &c)) {
            ERR("Delete refused a permanent atom");
        }

        multimapAtomInsertConvert(ma, &d);
        const databox b = databoxNewSigned(101);
        const databox dKey = databoxNewSigned(103);
        multimapAtomDelete(ma, &a);
        multimapAtomDelete(ma, &b);
        multimapAtomDelete(ma, &dKey);
        databox e = databoxNewSigned(104);
        multimapAtomInsertConvert(ma, &e);
        if (d.data.u != 3 || e.data.u != 4 || multimapAtomCount(ma) != 1) {
            ERR("permanent ids repeated: d %" PRIu64 ", e %" PRIu64,
                (uint64_t)d.data.u, (uint64_t)e.data.u);
        }

        multimapAtomFree(ma);
    }

    TEST("reusing: the top id comes back; interior holes wait") {
        multimapAtom *ma = multimapAtomNew();
        databox v[5];
        for (int i = 0; i < 5; i++) {
            v[i] = databoxNewSigned(200 + i);
            multimapAtomInsertConvert(ma, &v[i]); /* ids 0..4 */
        }

        /* Releasing the top atom frees its id for the next atom */
        if (!multimapAtomReleaseById(ma, &v[4])) {
            ERR("the last release did not delete the atom");
        }

        databox f = databoxNewSigned(300);
        multimapAtomInsertConvert(ma, &f);
        if (f.data.u != 4) {
            ERR("after the top release the next id is %" PRIu64 ", want 4",
                (uint64_t)f.data.u);
        }

        /* An interior hole stays unused */
        multimapAtomReleaseById(ma, &v[2]);
        databox g = databoxNewSigned(301);
        multimapAtomInsertConvert(ma, &g);
        if (g.data.u != 5) {
            ERR("an interior release moved the next id to %" PRIu64 ", want 5",
                (uint64_t)g.data.u);
        }

        /* Releasing down from the top skips back over the hole only to
         * the largest live id */
        multimapAtomReleaseById(ma, &g); /* live 0 1 3 4: next 5 */
        multimapAtomReleaseById(ma, &f); /* live 0 1 3: next 4 */
        databox h = databoxNewSigned(302);
        multimapAtomInsertConvert(ma, &h);
        if (h.data.u != 4) {
            ERR("the next id after releasing 5 and 4 is %" PRIu64 ", want 4",
                (uint64_t)h.data.u);
        }

        /* Last-in-first-out churn reuses one id */
        for (int i = 0; i < 10000; i++) {
            databox x = databoxNewSigned(1'000'000 + i);
            multimapAtomInsertConvert(ma, &x);
            if (x.data.u != 5) {
                ERR("churn %d got id %" PRIu64 ", want 5", i,
                    (uint64_t)x.data.u);
                break;
            }

            multimapAtomReleaseById(ma, &x);
        }

        multimapAtomFree(ma);
    }

    TEST("generated ids never collide with exact ids") {
        multimapAtom *ma = multimapAtomNew();
        const databox exactKey = databoxNewSigned(500);
        multimapAtomInsertWithExactAtomID(ma, 5, &exactKey);

        databox gen = databoxNewSigned(600);
        multimapAtomInsertConvert(ma, &gen);
        if (gen.data.u <= 5) {
            ERR("generated id %" PRIu64 " reuses the exact-id range",
                (uint64_t)gen.data.u);
        }

        const databox exactRef = {.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                                  .data.u = 5};
        databox found = {};
        if (multimapCount(ma->mapAtomForward) != 2 ||
            !multimapAtomLookup(ma, &exactRef, &found) ||
            databoxCompare(&found, &exactKey) != 0 ||
            !multimapAtomLookup(ma, &gen, &found) || found.data.i != 600) {
            ERR("an exact id and a generated id do not resolve separately");
        }

        multimapAtomFree(ma);
    }

    TEST("every way of emptying the table restarts ids at 0") {
        /* Release, Delete, DeleteByRef and DeleteById each empty it */
        for (int how = 0; how < 4; how++) {
            multimapAtom *ma = multimapAtomNew();
            databox a = databoxNewSigned(1);
            databox b = databoxNewSigned(2);
            const databox aKey = a;
            const databox bKey = b;
            multimapAtomInsertConvert(ma, &a);
            multimapAtomInsertConvert(ma, &b);

            bool ok = true;
            switch (how) {
            case 0:
                ok = multimapAtomRelease(ma, &aKey) &&
                     multimapAtomRelease(ma, &bKey);
                break;
            case 1:
                ok = multimapAtomDelete(ma, &aKey) &&
                     multimapAtomDelete(ma, &bKey);
                break;
            case 2:
                ok = multimapAtomDeleteByRef(ma, &a) &&
                     multimapAtomDeleteByRef(ma, &b);
                break;
            default:
                ok = multimapAtomDeleteById(ma, a.data.u) &&
                     multimapAtomDeleteById(ma, b.data.u);
                break;
            }

            databox ref = {};
            if (!ok || multimapAtomCount(ma) != 0 ||
                multimapCount(ma->mapAtomReverse) != 0 ||
                multimapAtomLookupReference(ma, &aKey, &ref) ||
                multimapAtomLookupReference(ma, &bKey, &ref)) {
                ERR("way %d: the table did not empty on both sides", how);
            }

            databox c = databoxNewSigned(3);
            multimapAtomInsertConvert(ma, &c);
            if (c.data.u != 0) {
                ERR("way %d: first id after emptying is %" PRIu64 ", want 0",
                    how, (uint64_t)c.data.u);
            }

            multimapAtomFree(ma);
        }
    }

    TEST("Delete and Release by key remove both rows; misses change nothing") {
        multimapAtom *ma = multimapAtomNew();
        databox keep = databoxNewSigned(10);
        databox gone = databoxNewSigned(20);
        const databox keepKey = keep;
        const databox goneKey = gone;
        const databox missing = databoxNewSigned(30);
        multimapAtomInsertConvert(ma, &keep);
        multimapAtomInsertConvert(ma, &gone);
        multimapAtomRetain(ma, &goneKey); /* two references */

        if (multimapAtomDelete(ma, &missing) || multimapAtomCount(ma) != 2) {
            ERR("deleting a missing key changed the table");
        }

        if (multimapAtomRelease(ma, &goneKey) || multimapAtomCount(ma) != 2) {
            ERR("releasing one of two references removed the atom");
        }

        databox ref = {};
        if (!multimapAtomRelease(ma, &goneKey) || multimapAtomCount(ma) != 1 ||
            multimapCount(ma->mapAtomReverse) != 1 ||
            multimapAtomLookupReference(ma, &goneKey, &ref)) {
            ERR("the last release did not remove both rows");
        }

        databox found = {};
        if (!multimapAtomLookup(ma, &keep, &found) || found.data.i != 10 ||
            !multimapAtomDelete(ma, &keepKey) || multimapAtomCount(ma) != 0 ||
            multimapCount(ma->mapAtomReverse) != 0) {
            ERR("Delete by key did not remove both rows of a live atom");
        }

        multimapAtomFree(ma);
    }

    TEST("interning in shuffled and descending order resolves every atom") {
        /* New values land anywhere in the reverse map, including at the
         * head of a sub-map and across tier upgrades, where the insert
         * itself resolves the new id: its forward row must already exist.
         * Each value is offered twice; the second offer finds the first
         * one's reference. */
        enum { atoms = 3000 };
        for (int32_t order = 0; order < 2; order++) {
            cleaner(multimapAtomFree) multimapAtom *ma = multimapAtomNew();
            cleanerName(zfree, U64) uint64_t *refs =
                zcalloc(atoms, sizeof(*refs));
            char name[32];
            for (int32_t pass = 0; pass < 2; pass++) {
                for (int32_t step = 0; step < atoms; step++) {
                    const int32_t i =
                        order ? atoms - 1 - step
                              : (int32_t)(((int64_t)step * 1201) % atoms);
                    snprintf(name, sizeof(name), "interned-%06" PRId32, i);
                    databox k = databoxNewBytesString(name);
                    const bool existed =
                        (step & 1)
                            ? multimapAtomInsertIfNewConvert(ma, &k)
                            : (multimapAtomRetainConvert(ma, &k), pass == 1);
                    if (pass == 0) {
                        refs[i] = k.data.u;
                    } else if (!existed || k.data.u != refs[i]) {
                        ERR("order %d: second offer of %s gave ref %" PRIu64
                            " existed %d, first gave %" PRIu64,
                            order, name, (uint64_t)k.data.u, existed, refs[i]);
                    }
                }
            }

            if (multimapAtomCount(ma) != atoms) {
                ERR("order %d: %zu atoms, expected %d", order,
                    multimapAtomCount(ma), atoms);
            }

            for (int32_t i = 0; i < atoms; i++) {
                snprintf(name, sizeof(name), "interned-%06" PRId32, i);
                const databox k = databoxNewBytesString(name);
                const databox ref = {.data.u = refs[i],
                                     .type =
                                         DATABOX_CONTAINER_REFERENCE_EXTERNAL};
                databox found = {};
                databox back = {};
                if (!multimapAtomLookupReference(ma, &k, &found) ||
                    found.data.u != refs[i] ||
                    !multimapAtomLookup(ma, &ref, &back) ||
                    databoxLen(&back) != strlen(name) ||
                    memcmp(databoxCBytes(&back), name, strlen(name)) != 0) {
                    ERR("order %d: %s does not resolve both ways", order, name);
                }
            }
        }
    }

    TEST("release by key and delete by id with the reverse map in every "
         "tier") {
        /* Release and DeleteByRef reuse the entry their first search found;
         * table sizes from a few atoms to thousands put the reverse map in
         * each tier while that happens */
        const int32_t sizes[] = {6, 60, 200, 400, 900, 2500};
        bool sawTier[MULTIMAP_TYPE_FULL + 1] = {false};
        char name[32];
        for (size_t s = 0; s < COUNT_ARRAY(sizes); s++) {
            const int32_t n = sizes[s];
            cleaner(multimapAtomFree) multimapAtom *ma = multimapAtomNew();
            for (int32_t i = 0; i < n; i++) {
                snprintf(name, sizeof(name), "tiered-%06" PRId32, i);
                databox k = databoxNewBytesString(name);
                multimapAtomInsertIfNewConvert(ma, &k);
            }

            sawTier[multimapGetType(ma->mapAtomReverse)] = true;

            /* Odd atoms leave by their value, every fourth by its id */
            for (int32_t i = 0; i < n; i++) {
                snprintf(name, sizeof(name), "tiered-%06" PRId32, i);
                const databox k = databoxNewBytesString(name);
                if (i % 2) {
                    if (!multimapAtomRelease(ma, &k)) {
                        ERR("%d atoms: release of %s kept it", n, name);
                    }
                } else if (i % 4 == 0) {
                    databox ref = {};
                    if (!multimapAtomLookupReference(ma, &k, &ref) ||
                        !multimapAtomDeleteById(ma, ref.data.u)) {
                        ERR("%d atoms: delete by id of %s failed", n, name);
                    }
                }
            }

            for (int32_t i = 0; i < n; i++) {
                snprintf(name, sizeof(name), "tiered-%06" PRId32, i);
                const databox k = databoxNewBytesString(name);
                databox ref = {};
                const bool want = i % 4 == 2;
                const bool present = multimapAtomLookupReference(ma, &k, &ref);
                databox back = {};
                if (present != want ||
                    (present &&
                     (!multimapAtomLookup(ma, &ref, &back) ||
                      databoxLen(&back) != strlen(name) ||
                      memcmp(databoxCBytes(&back), name, strlen(name)) != 0))) {
                    ERR("%d atoms: %s present %d (want %d) or unresolvable", n,
                        name, present, want);
                }
            }

            if (multimapAtomCount(ma) != (size_t)(n / 4 + (n % 4 > 2)) ||
                multimapCount(ma->mapAtomReverse) != multimapAtomCount(ma)) {
                ERR("%d atoms: %zu left (%zu reverse rows)", n,
                    multimapAtomCount(ma), multimapCount(ma->mapAtomReverse));
            }
        }

        if (!sawTier[MULTIMAP_TYPE_SMALL] || !sawTier[MULTIMAP_TYPE_MEDIUM] ||
            !sawTier[MULTIMAP_TYPE_FULL]) {
            ERR("reverse tiers seen: small %d medium %d full %d",
                sawTier[MULTIMAP_TYPE_SMALL], sawTier[MULTIMAP_TYPE_MEDIUM],
                sawTier[MULTIMAP_TYPE_FULL]);
        }
    }

    TEST("last releases by key across sub-maps keep every survivor "
         "resolvable") {
        /* Enough string atoms for both maps to split into sub-maps; the
         * last releases empty whole runs of rows, so sub-maps empty and
         * merge while every other atom must still resolve both ways */
        enum { atoms = 3000 };
        multimapAtom *ma = multimapAtomNew();
        char name[32];
        for (int32_t i = 0; i < atoms; i++) {
            snprintf(name, sizeof(name), "atom-value-%06" PRId32, i);
            databox k = databoxNewBytesString(name);
            multimapAtomInsertIfNewConvert(ma, &k);
            if (i % 4 == 0) {
                databox again = databoxNewBytesString(name);
                multimapAtomRetain(ma, &again); /* survives one release */
            }
        }

        /* Release runs of 50 in a scattered order of runs */
        for (int32_t run = 0; run < atoms / 50; run++) {
            const int32_t first = ((run * 37) % (atoms / 50)) * 50;
            for (int32_t i = first; i < first + 50; i++) {
                snprintf(name, sizeof(name), "atom-value-%06" PRId32, i);
                const databox k = databoxNewBytesString(name);
                const bool deleted = multimapAtomRelease(ma, &k);
                if (deleted != (i % 4 != 0)) {
                    ERR("release of %s reported deleted %d", name, deleted);
                }
            }
        }

        if (multimapAtomCount(ma) != atoms / 4 ||
            multimapCount(ma->mapAtomReverse) != atoms / 4) {
            ERR("after the releases: %zu atoms, %zu reverse rows, expected "
                "%d",
                multimapAtomCount(ma), multimapCount(ma->mapAtomReverse),
                atoms / 4);
        }

        for (int32_t i = 0; i < atoms; i++) {
            snprintf(name, sizeof(name), "atom-value-%06" PRId32, i);
            const databox k = databoxNewBytesString(name);
            databox ref = {};
            const bool present = multimapAtomLookupReference(ma, &k, &ref);
            if (present != (i % 4 == 0)) {
                ERR("%s present %d after the releases", name, present);
                continue;
            }

            databox back = {};
            if (present &&
                (!multimapAtomLookup(ma, &ref, &back) ||
                 databoxLen(&back) != strlen(name) ||
                 memcmp(databoxCBytes(&back), name, strlen(name)) != 0)) {
                ERR("%s's reference no longer resolves to it", name);
            }
        }

        multimapAtomFree(ma);
    }

    TEST_FINAL_RESULT;
}
#endif
