#include "multimapAtom.h"
#include "atomPersist.h"
#include "cleaner.h"
#include "dkTierLimit.h"
#include "idRows.h"
#include "multidict.h"
#include "rapidhash.h"

defcleaner(multimapAtom *, multimapAtomFree);
defcleanerName(void *, zfree, Void);

/* The forward rows are {value, count} addressed by id: the count is the
 * row's second field */
static constexpr uint32_t ATOM_ROW_COUNT_ = 1;

struct multimapAtom {
    /* {value, count} by id: a read by id is positional (idRows.h) */
    idRows *forward;
    /* The reverse index (multimapAtomIndex): a multimap for ORDERED and
     * HASHED, a multidict for DICT */
    union {
        multimap *mapAtomReverse;
        multidict *dictAtomReverse;
    };
    /* The next generated id, with the table's id behaviour (bit 63 set:
     * permanent) and reverse index kind (bits 61-62) above it, so neither
     * costs a byte.  Ids stay below MULTIMAP_ATOM_ID_LIMIT. */
    uint64_t highest;
};

static constexpr uint64_t ATOM_PERMANENT_BIT_ = (uint64_t)1 << 63;
static constexpr unsigned ATOM_INDEX_SHIFT_ = 61;
static constexpr uint64_t ATOM_INDEX_BITS_ = (uint64_t)3 << ATOM_INDEX_SHIFT_;
static constexpr uint64_t ATOM_SETTING_BITS_ =
    ATOM_PERMANENT_BIT_ | ATOM_INDEX_BITS_;
static_assert(((uint64_t)1 << ATOM_INDEX_SHIFT_) == MULTIMAP_ATOM_ID_LIMIT,
              "the settings sit just above every id");

static inline uint64_t atomNextId_(const multimapAtom *ma) {
    return ma->highest & ~ATOM_SETTING_BITS_;
}

static inline void atomSetNextId_(multimapAtom *ma, const uint64_t id) {
    assert(id < MULTIMAP_ATOM_ID_LIMIT);
    ma->highest = (ma->highest & ATOM_SETTING_BITS_) | id;
}

bool multimapAtomIsPermanent(const multimapAtom *ma) {
    return (ma->highest & ATOM_PERMANENT_BIT_) != 0;
}

multimapAtomIndex multimapAtomGetIndex(const multimapAtom *ma) {
    return (multimapAtomIndex)((ma->highest & ATOM_INDEX_BITS_) >>
                               ATOM_INDEX_SHIFT_);
}

#ifdef DATAKIT_TEST
/* The index multimapAtomNew and NewPermanent make, and the mask a hash
 * passes through: the suite runs once per index, and once with 2-bit
 * hashes so most values collide */
static multimapAtomIndex atomTestDefaultIndex_ = MULTIMAP_ATOM_INDEX_HASHED;
static uint32_t atomTestHashMask_ = UINT32_MAX;
#endif

/* ====================================================================
 * Create
 * ==================================================================== */
/* A DICT index: a multidict with a class of its own (reached again through
 * multidictGetClass), keys matched as multidict matches them */
static multidict *atomDictNew_(void) {
    /* @own: container; free=atomDictFree_ */
    multidictClass *qdc = multidictDefaultClassNew();
    /* @own: container; free=atomDictFree_ */
    return multidictNew(&multidictTypeExactKey, qdc, multidictRandomSeed());
}

static void atomDictFree_(multidict *d) {
    if (d) {
        multidictClass *qdc = multidictGetClass(d);
        multidictFree(d);
        multidictDefaultClassFree(qdc);
    }
}

static void atomInitIndexed_(multimapAtom *ma, const multimapAtomIndex index,
                             const bool permanent) {
    /* Every multimap of the table takes the atom family's node limit */
    const flexCapSizeLimit limit = dkTierDefaultLimit(DK_TIER_MULTIMAP_ATOM);

    /* {value, count} by id */
    /* @own: container; free=multimapAtomFree */
    ma->forward = idRowsNew(2);

    /* ORDERED: {id} by value (resolved through the forward rows); HASHED:
     * the set of {hash, id}; DICT: value -> id */
    switch (index) {
    case MULTIMAP_ATOM_INDEX_ORDERED:
        /* @own: container; free=multimapAtomFree */
        ma->mapAtomReverse = multimapNewLimit(1, limit);
        break;
    case MULTIMAP_ATOM_INDEX_HASHED:
        /* @own: container; free=multimapAtomFree */
        ma->mapAtomReverse = multimapNewConfigure(2, true, false, limit);
        break;
    case MULTIMAP_ATOM_INDEX_DICT:
        /* @own: container; free=multimapAtomFree */
        ma->dictAtomReverse = atomDictNew_();
        break;
    }

    ma->highest = (permanent ? ATOM_PERMANENT_BIT_ : 0) |
                  ((uint64_t)index << ATOM_INDEX_SHIFT_);
}

void multimapAtomInit(multimapAtom *ma) {
    atomInitIndexed_(ma, MULTIMAP_ATOM_INDEX_HASHED, false);
}

multimapAtom *multimapAtomNewIndexed(const multimapAtomIndex index,
                                     const bool permanent) {
    /* @own: caller; free=multimapAtomFree */
    multimapAtom *ma = zcalloc(1, sizeof(*ma));
    atomInitIndexed_(ma, index, permanent);
    return ma;
}

static multimapAtomIndex atomDefaultIndex_(void) {
#ifdef DATAKIT_TEST
    return atomTestDefaultIndex_;
#else
    return MULTIMAP_ATOM_INDEX_HASHED;
#endif
}

multimapAtom *multimapAtomNew(void) {
    return multimapAtomNewIndexed(atomDefaultIndex_(), false);
}

multimapAtom *multimapAtomNewPermanent(void) {
    return multimapAtomNewIndexed(atomDefaultIndex_(), true);
}

/* ====================================================================
 * Free
 * ==================================================================== */
void multimapAtomFree(multimapAtom *ma) {
    if (ma) {
        idRowsFree(ma->forward);
        if (multimapAtomGetIndex(ma) == MULTIMAP_ATOM_INDEX_DICT) {
            atomDictFree_(ma->dictAtomReverse);
        } else {
            multimapFree(ma->mapAtomReverse);
        }

        zfree(ma);
    }
}

/* ====================================================================
 * Reverse index (value -> id)
 * ====================================================================
 * Every use of mapAtomReverse goes through the atomIndex*_ helpers,
 * which serve both kinds (multimapAtomIndex). */

static databox atomRefBox_(const uint64_t id) {
    return (databox){.data.u = id,
                     .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
}

/* A HASHED row's hash of 'value': databoxHashBytes, so values
 * databoxCompare calls equal share it */
static databox atomHashBox_(const databox *value) {
    uint64_t scratch;
    const uint8_t *bytes;
    size_t len;
    databoxHashBytes(value, &scratch, &bytes, &len);
    uint32_t h = (uint32_t)rapidhash(bytes, len);
#ifdef DATAKIT_TEST
    h &= atomTestHashMask_;
#endif
    return DATABOX_UNSIGNED(h);
}

/* The forward row of 'ref' into 'elements' (value, count) */
static bool atomForwardLookup_(const multimapAtom *ma, const databox *ref,
                               databox *elements[2]) {
    return idRowsGet(ma->forward, ref->data.u, elements);
}

/* Whether the atom 'ref' holds 'value' (databoxCompare equality, wide
 * values whole): a HASHED candidate's check */
static bool atomHolds_(const multimapAtom *ma, const databox *ref,
                       const databox *value) {
    databoxBig held;
    databox *const out = (databox *)&held;
    *out = (databox){};
    DATABOX_BIG_INIT(&held);
    databox count = {};
    databox *elements[2] = {out, &count};
    if (!atomForwardLookup_(ma, ref, elements)) {
        return false;
    }

    held.big =
        held.type == DATABOX_SIGNED_128 || held.type == DATABOX_UNSIGNED_128;
    return databoxCompare(out, value) == 0;
}

/* HASHED: the atom holding 'value' among the rows with 'hash', walked by
 * an iterator that crosses maps.  The walk starts at or before the first
 * of them: a positioned search lands on
 * any row with an equal key, so it is aimed at hash - 1, whose rows all
 * come first (and when there are none it lands on the first row at or
 * past it); hash 0 starts at the beginning.  Rows below 'hash' are
 * skipped. */
static bool atomHashedWalk_(const multimapAtom *ma, const databox *value,
                            const databox *hash, databox *ref) {
    multimapIterator iter;
    if (hash->data.u64 == 0) {
        multimapIteratorInit(ma->mapAtomReverse, &iter, true);
    } else {
        const databox before = DATABOX_UNSIGNED(hash->data.u64 - 1);
        if (!multimapIteratorInitAt(ma->mapAtomReverse, &iter, true, &before)) {
            return false;
        }
    }

    databox row[2];
    while (multimapIteratorNextInto(&iter, row, 2)) {
        const int order = databoxCompare(&row[0], hash);
        if (order > 0) {
            return false; /* past every row with the hash */
        }

        if (order < 0) {
            continue;
        }

        const databox candidate = atomRefBox_(row[1].data.u);
        if (atomHolds_(ma, &candidate, value)) {
            *ref = candidate;
            return true;
        }
    }

    return false;
}

/* HASHED: the atom holding 'value', its rows found by one keyed search
 * (a set's first row with the key 'hash') and read in place along that
 * map.  Rows with the hash that run on past the map's end go to
 * atomHashedWalk_, which crosses maps. */
/* The atom holding 'value' among the index rows of 'hash', the first of
 * which is 'found' */
static bool atomHashedFindFrom_(const multimapAtom *ma, const databox *value,
                                const databox *hash, const multimapEntry *found,
                                databox *ref);

static bool atomHashedFind_(const multimapAtom *ma, const databox *value,
                            const databox *hash, databox *ref) {
    multimapEntry row;
    if (!multimapGetUnderlyingEntry(ma->mapAtomReverse, hash, &row)) {
        return false;
    }

    return atomHashedFindFrom_(ma, value, hash, &row, ref);
}

static bool atomHashedFindFrom_(const multimapAtom *ma, const databox *value,
                                const databox *hash, const multimapEntry *found,
                                databox *ref) {
    const multimapEntry row = *found;
    flexEntry *fe = row.fe;
    while (true) {
        flexEntry *idEntry = flexNext(*row.map, fe);
        databox id = {};
        flexGetByType(idEntry, &id);
        const databox candidate = atomRefBox_(id.data.u);
        if (atomHolds_(ma, &candidate, value)) {
            *ref = candidate;
            return true;
        }

        fe = flexNext(*row.map, idEntry);
        if (!fe) {
            return atomHashedWalk_(ma, value, hash, ref);
        }

        databox next = {};
        flexGetByType(fe, &next);
        if (next.data.u != hash->data.u) {
            return false;
        }
    }
}

/* DICT: the id of 'value'.  No forward lookup: the dict holds the value
 * itself. */
static bool atomDictFind_(const multimapAtom *ma, const databox *value,
                          databox *ref) {
    databox id = {};
    if (!multidictFind(ma->dictAtomReverse, value, &id)) {
        return false;
    }

    *ref = atomRefBox_(id.data.u);
    return true;
}

/* DICT: finish any rehash a change started, so the dict's lookups (which
 * step an unfinished rehash) never change it: a table's lookups stay
 * reads, as under the other kinds */
static void atomDictSettle_(multimapAtom *ma) {
    while (multidictRehash(ma->dictAtomReverse, 1024)) {
    }
}

/* The index's rows (one per atom) */
static size_t atomIndexCount_(const multimapAtom *ma) {
    if (multimapAtomGetIndex(ma) == MULTIMAP_ATOM_INDEX_DICT) {
        return (size_t)multidictCount(ma->dictAtomReverse);
    }

    const multimap *index = ma->mapAtomReverse;
    return multimapCount(index);
}

/* The index's bytes */
static size_t atomIndexBytes_(const multimapAtom *ma) {
    return multimapAtomGetIndex(ma) == MULTIMAP_ATOM_INDEX_DICT
               ? (size_t)multidictBytes(ma->dictAtomReverse)
               : multimapBytes(ma->mapAtomReverse);
}

/* The id of 'value', into 'ref' */
static bool atomIndexFind_(const multimapAtom *ma, const databox *value,
                           databox *ref) {
    switch (multimapAtomGetIndex(ma)) {
    case MULTIMAP_ATOM_INDEX_HASHED: {
        const databox hash = atomHashBox_(value);
        return atomHashedFind_(ma, value, &hash, ref);
    }
    case MULTIMAP_ATOM_INDEX_DICT:
        return atomDictFind_(ma, value, ref);
    case MULTIMAP_ATOM_INDEX_ORDERED:
        break;
    }

    return multimapExistsWithReference(ma->mapAtomReverse, value, ref, ma);
}

/* Add the index row binding 'value' to 'ref' (the forward row is in) */
static void atomIndexInsert_(multimapAtom *ma, const databox *value,
                             const databox *ref) {
    switch (multimapAtomGetIndex(ma)) {
    case MULTIMAP_ATOM_INDEX_HASHED: {
        const databox hash = atomHashBox_(value);
        const databox id = DATABOX_UNSIGNED(ref->data.u);
        const databox *row[2] = {&hash, &id};
        multimapInsertFullWidth(&ma->mapAtomReverse, row);
        return;
    }
    case MULTIMAP_ATOM_INDEX_DICT: {
        const databox id = DATABOX_UNSIGNED(ref->data.u);
        [[maybe_unused]] const multidictResult added =
            multidictAdd(ma->dictAtomReverse, value, &id);
        assert(added == MULTIDICT_OK_INSERTED);
        atomDictSettle_(ma);
        return;
    }
    case MULTIMAP_ATOM_INDEX_ORDERED:
        break;
    }

    const databox *row[1] = {value};
    multimapInsertWithSurrogateKey(&ma->mapAtomReverse, row, ref, ma);
}

/* Remove the HASHED row {hash of 'value', id of 'ref'} */
static bool atomHashedDeleteRow_(multimapAtom *ma, const databox *value,
                                 const databox *ref) {
    const databox hash = atomHashBox_(value);
    const databox id = DATABOX_UNSIGNED(ref->data.u);
    const databox *row[2] = {&hash, &id};
    return multimapDeleteFullWidth(&ma->mapAtomReverse, row);
}

/* Remove the index row of 'value', reporting its id in 'foundRef' */
static bool atomIndexDeleteValue_(multimapAtom *ma, const databox *value,
                                  databox *foundRef) {
    switch (multimapAtomGetIndex(ma)) {
    case MULTIMAP_ATOM_INDEX_HASHED: {
        if (!atomIndexFind_(ma, value, foundRef)) {
            return false;
        }

        [[maybe_unused]] const bool deleted =
            atomHashedDeleteRow_(ma, value, foundRef);
        assert(deleted);
        return true;
    }
    case MULTIMAP_ATOM_INDEX_DICT: {
        /* The removed id is a number: its box holds it whole */
        databox id = {};
        if (!multidictGetAndDelete(ma->dictAtomReverse, value, &id)) {
            return false;
        }

        atomDictSettle_(ma);
        *foundRef = atomRefBox_(id.data.u);
        return true;
    }
    case MULTIMAP_ATOM_INDEX_ORDERED:
        break;
    }

    return multimapDeleteWithReference(&ma->mapAtomReverse, value, ma,
                                       foundRef);
}

/* Remove the index row of the atom 'ref', which holds 'value' */
static void atomIndexDeleteHeld_(multimapAtom *ma, const databox *value,
                                 const databox *ref) {
    switch (multimapAtomGetIndex(ma)) {
    case MULTIMAP_ATOM_INDEX_HASHED: {
        [[maybe_unused]] const bool deleted =
            atomHashedDeleteRow_(ma, value, ref);
        assert(deleted);
        return;
    }
    case MULTIMAP_ATOM_INDEX_DICT: {
        databox id = {};
        [[maybe_unused]] const bool deleted =
            multidictGetAndDelete(ma->dictAtomReverse, value, &id);
        assert(deleted && id.data.u == ref->data.u);
        atomDictSettle_(ma);
        return;
    }
    case MULTIMAP_ATOM_INDEX_ORDERED:
        break;
    }

    databox found = {};
    [[maybe_unused]] const bool deleted =
        multimapDeleteWithReference(&ma->mapAtomReverse, value, ma, &found);
    assert(deleted && found.data.u == ref->data.u);
}

/* ====================================================================
 * Insert (doesn't check for existing entry)
 * ==================================================================== */
/* The forward row of a new atom 'id': 'value' with one reference.  The
 * count is stored 0-based as 'false' while it is one reference (one byte
 * where the number 1 takes three); flexIncrbySigned counts on booleans,
 * so retains and releases work on it as on any integer.  Internal 0 = 1
 * reference, 1 = 2, and so on; the API layer adds 1 for display. */
static void atomForwardAdd_(multimapAtom *ma, const uint64_t id,
                            const databox *value) {
    const databox refcount = {.type = DATABOX_FALSE};
    const databox *row[2] = {value, &refcount};
    [[maybe_unused]] const bool added = idRowsSet(ma->forward, id, row);
    assert(added);
}

static void abstractInsert(multimapAtom *ma, const databox *reverseKey,
                           const uint64_t id) {
    const databox atomId = atomRefBox_(id);
    atomForwardAdd_(ma, id, reverseKey);
    atomIndexInsert_(ma, reverseKey, &atomId);
    assert(idRowsCount(ma->forward) == atomIndexCount_(ma));

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
    atomForwardAdd_(c->ma, c->id, c->key);

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
    switch (multimapAtomGetIndex(ma)) {
    case MULTIMAP_ATOM_INDEX_HASHED: {
        /* One search: a hash no atom has gets its index row where it was
         * looked for; a present hash's rows are checked for the key, and
         * a key that only shares its hash adds a row of its own */
        const databox hash = atomHashBox_(key);
        const databox idBox = DATABOX_UNSIGNED(id);
        const databox *row[2] = {&hash, &idBox};
        multimapEntry first;
        if (!multimapInsertIfAbsentEntry(&ma->mapAtomReverse, row, &first)) {
            if (atomHashedFindFrom_(ma, key, &hash, &first, ref)) {
                return true;
            }
            multimapInsertFullWidth(&ma->mapAtomReverse, row);
        }

        atomCreateForward_(&create);
        *ref = atomId;
        return false;
    }
    case MULTIMAP_ATOM_INDEX_DICT:
        /* A find (the common case: present), then a new atom's add */
        if (atomDictFind_(ma, key, ref)) {
            return true;
        }

        atomCreateForward_(&create);
        atomIndexInsert_(ma, key, &atomId);
        *ref = atomId;
        return false;
    case MULTIMAP_ATOM_INDEX_ORDERED:
        break;
    }

    const databox *reverseRow[1] = {key};
    if (!multimapInsertWithSurrogateKeyIfAbsent(&ma->mapAtomReverse, reverseRow,
                                                &atomId, ma, ref,
                                                atomCreateForward_, &create)) {
        return true;
    }

    assert(idRowsCount(ma->forward) == atomIndexCount_(ma));
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

/* Add 'delta' to the stored count of the atom 'ref' and return the result:
 * the new count, or on a refusal the current one (0 for an absent atom).
 * A reusing table only: a permanent one keeps no counts.  The row is
 * reached by id, with no search. */
static int64_t atomCountAdd_(multimapAtom *ma, const databox *ref,
                             const int64_t delta) {
    assert(!multimapAtomIsPermanent(ma));
    int64_t next = 0;
    idRowsFieldIncr(ma->forward, ref->data.u, ATOM_ROW_COUNT_, delta, &next);
    return next;
}

/* The atom of 'key' with one more reference: an existing atom's count goes
 * up; a new one is created with its one reference.  True when it existed;
 * 'ref' its reference. */
static bool atomRetainKey_(multimapAtom *ma, const databox *key, databox *ref) {
    const bool existed = atomFindOrCreate_(ma, key, ref);
    if (existed && !multimapAtomIsPermanent(ma)) {
        atomCountAdd_(ma, ref, 1);
    }

    return existed;
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
    const databox atomId = atomRefBox_(atomRef);

    /* If exists, delete so we can have a clean insert.
     * Instead of lookup->result->delete, we just attempt
     * a direct delete. If the first delete works, we know we can
     * delete from the other map too. */
    databox foundReference = {};
    if (atomIndexDeleteValue_(ma, reverseKey, &foundReference)) {
        [[maybe_unused]] const bool deleted =
            idRowsDelete(ma->forward, foundReference.data.u);
        assert(deleted);
    }

    /* Also clear any prior binding of this exact ID to a DIFFERENT key,
     * whose index row would otherwise outlive it */
    multimapAtomDeleteByRef(ma, &atomId);

    atomForwardAdd_(ma, atomRef, reverseKey);
    atomIndexInsert_(ma, reverseKey, &atomId);

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
    const bool existed = atomRetainKey_(ma, key, &ref); /* a checkout */

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

    return atomForwardLookup_(ma, ref, elements);
}

void multimapAtomLookupResult(const multimapAtom *ma, const databox *key,
                              multimapAtomResult *result) {
    databox count = {};
    databox *elements[2] = {&result->val, &count};

    /* 'val' is a plain databox with no wide storage, so a 96/128-bit
     * value is reported type-only instead of written through whatever
     * the caller's box held. */
    result->val.big = false;
    atomForwardLookup_(ma, key, elements);
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
    if (!atomForwardLookup_(ma, &ref, elements)) {
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
    if (atomForwardLookup_(ma, &ref, elements)) {
        box->big = box->type == DATABOX_SIGNED_128 ||
                   box->type == DATABOX_UNSIGNED_128;
    } else {
        *out = ref;
    }
}

/* Lookup USER DATA VALUE to ATOM REFERENCE */
bool multimapAtomLookupReference(const multimapAtom *ma, const databox *key,
                                 databox *atomRef) {
    return atomIndexFind_(ma, key, atomRef);
}

bool multimapAtomLookupRefcount(const multimapAtom *ma, const databox *key,
                                databox *count) {
    /* Zeroed: the value is discarded, so a 96/128-bit one decodes
     * type-only instead of through a stale pointer. */
    databox val = {};
    databox *elements[2] = {&val, count};

    return atomForwardLookup_(ma, key, elements);
}

/* ====================================================================
 * Use
 * ==================================================================== */
void multimapAtomRetainByRef(multimapAtom *ma, const databox *foundRef) {
    assert(foundRef->type == DATABOX_CONTAINER_REFERENCE_EXTERNAL);
    if (multimapAtomIsPermanent(ma)) {
        return; /* no count is kept: the atom lives until Delete or Free */
    }

    atomCountAdd_(ma, foundRef, 1);
}

void multimapAtomRetainById(multimapAtom *ma, const uint64_t id) {
    databox ref = {.data.u = id, .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
    multimapAtomRetainByRef(ma, &ref);
}

void multimapAtomRetain(multimapAtom *ma, const databox *key) {
    databox ref = {};
    atomRetainKey_(ma, key, &ref);
}

void multimapAtomRetainConvert(multimapAtom *ma, databox *key) {
    /* Zeroed: a reference id decodes narrow and leaves the flag bits as
     * they were, and this box is handed back to the caller whole. */
    databox ref = {};
    atomRetainKey_(ma, key, &ref);
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
 * live one (the forward rows' last id), so
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

    if (idRowsCount(ma->forward) == 0) {
        atomSetNextId_(ma, 0);
        return;
    }

    if (deletedId + 1 == atomNextId_(ma)) {
        uint64_t last;
        if (idRowsLast(ma->forward, &last) &&
            last + 1 < MULTIMAP_ATOM_ID_LIMIT) {
            atomSetNextId_(ma, last + 1);
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
        idRowsDelete(ma->forward, ref->data.u);
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

    const int64_t checkedOut = atomCountAdd_(ma, foundRef, -1);

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
    return conformRefcountDecr(ma, foundRef, nullptr);
}

bool multimapAtomRelease(multimapAtom *ma, const databox *key) {
    /* The count is updated by id.  ORDERED: the reverse row found here is
     * the one a last release deletes where it is.  HASHED and DICT: a last
     * release removes the index row by its value. */
    if (multimapAtomGetIndex(ma) != MULTIMAP_ATOM_INDEX_ORDERED) {
        databox foundRef = {};
        if (atomIndexFind_(ma, key, &foundRef)) {
            return conformRefcountDecr(ma, &foundRef, nullptr);
        }
    } else {
        multimapEntry me = {};
        if (multimapGetUnderlyingEntryWithReference(ma->mapAtomReverse, key,
                                                    &me, ma)) {
            databox foundRef = {};
            flexGetByType(me.fe, &foundRef);
            return conformRefcountDecr(ma, &foundRef, &me);
        }
    }

    /* else, releasing something we don't have is an error */
    assert(false && "Releasing something not in the atom table?");
    return false;
}

bool multimapAtomDelete(multimapAtom *ma, const databox *key) {
    /* One index search: the delete itself reports the atom's id */
    databox foundReference = {};
    if (!atomIndexDeleteValue_(ma, key, &foundReference)) {
        return false;
    }

    [[maybe_unused]] const bool deleteForward =
        idRowsDelete(ma->forward, foundReference.data.u);
    assert(deleteForward);

    atomAfterDelete_(ma, foundReference.data.u);
    return true;
}

bool multimapAtomDeleteByRef(multimapAtom *ma, const databox *ref) {
    assert(ref->type == DATABOX_CONTAINER_REFERENCE_EXTERNAL);

    /* The forward row gives the value the index is searched by.  It goes
     * once the index row is gone: the ORDERED index resolves ids through
     * the forward rows, and removing its row leaves them as they were. */
    databox value = {};
    databox count = {};
    databox *row[2] = {&value, &count};
    databoxWide wide[2];
    if (!idRowsGetWithSlots(ma->forward, ref->data.u, row, wide)) {
        return false;
    }

    assert(value.type != DATABOX_CONTAINER_REFERENCE_EXTERNAL);
    atomIndexDeleteHeld_(ma, &value, ref);
    idRowsDelete(ma->forward, ref->data.u);
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
    assert(idRowsCount(ma->forward) == atomIndexCount_(ma));

    return idRowsCount(ma->forward);
}

size_t multimapAtomBytes(const multimapAtom *ma) {
    assert(idRowsCount(ma->forward) == atomIndexCount_(ma));

    return idRowsBytes(ma->forward) + atomIndexBytes_(ma);
}

/* ====================================================================
 * Persistence (atomPersist.h)
 * ==================================================================== */
static constexpr unsigned ATOM_PERSIST_FLAG_PERMANENT_ = 1u;
/* The reverse index kind (multimapAtomIndex) in flag bits 1-2; the index
 * itself is rebuilt from the rows, never stored */
static constexpr unsigned ATOM_PERSIST_INDEX_SHIFT_ = 1;
static constexpr unsigned ATOM_PERSIST_FLAG_INDEX_ =
    3u << ATOM_PERSIST_INDEX_SHIFT_;

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

/* The bytes 'box' adds to a flex as one exact entry */
static uint64_t atomEntryBytes_(const databox *box) {
    const databox *one[1] = {box};
    return flexEncodeDataboxPtrsExact(nullptr, 0, one, 1) - FLEX_EMPTY_SIZE;
}

void multimapAtomGetStats(const multimapAtom *ma, multimapAtomStats *stats) {
    uint32_t unitLimit = 0;
    if (multimapAtomGetIndex(ma) != MULTIMAP_ATOM_INDEX_DICT) {
        dkTierStats index;
        multimapStats(ma->mapAtomReverse, &index);
        unitLimit = index.unitLimit;
    }

    *stats = (multimapAtomStats){.tableBytes = multimapAtomBytes(ma),
                                 .unitLimit = unitLimit};
    const bool permanent = multimapAtomIsPermanent(ma);

    idRowsIterator iter;
    idRowsIteratorInit(ma->forward, &iter);
    while (true) {
        /* Zeroed: a narrow decode leaves the flag bits as they were */
        uint64_t id = 0;
        databox value = {};
        databox count = {};
        databox *row[2] = {&value, &count};
        databoxWide wide[2];
        if (!idRowsIteratorNext(&iter, &id, row, wide)) {
            break;
        }

        uint64_t stored = 0;
        atomPersistCountOf_(&count, &stored);
        const uint64_t refs = permanent ? 1 : stored + 1;
        const databox ref = atomRefBox_(id);
        const uint64_t valueEntry = atomEntryBytes_(&value);

        stats->atoms++;
        stats->references += refs;
        stats->valueBytes += valueEntry;
        stats->inlineBytes += refs * valueEntry;
        stats->referenceBytes += refs * atomEntryBytes_(&ref);
    }

    stats->savedBytes = (int64_t)stats->inlineBytes -
                        (int64_t)(stats->tableBytes + stats->referenceBytes);
}

static uint8_t *multimapAtomPersistSnapshot_(const void *structure,
                                             size_t *len) {
    const multimapAtom *ma = structure;
    /* @own: caller; free=zfree */
    flex *f = flexNew();
    const databox flags = DATABOX_UNSIGNED(
        (multimapAtomIsPermanent(ma) ? ATOM_PERSIST_FLAG_PERMANENT_ : 0) |
        ((unsigned)multimapAtomGetIndex(ma) << ATOM_PERSIST_INDEX_SHIFT_));
    const databox next = DATABOX_UNSIGNED(atomNextId_(ma));
    const databox *header[2] = {&flags, &next};
    flexAppendMultipleExact(&f, 2, header);

    /* Rows in id order (the forward rows' order), 21 at a time */
    constexpr int BATCH = 21;
    databox boxes[BATCH * 3];
    databoxWide wide[BATCH * 3];
    const databox *ptrs[BATCH * 3];
    size_t used = 0;
    idRowsIterator iter;
    idRowsIteratorInit(ma->forward, &iter);
    while (true) {
        /* A narrow decode leaves the flag bits as they were: zero them */
        databox *row[2] = {&boxes[used * 3 + 1], &boxes[used * 3 + 2]};
        for (size_t i = 0; i < 3; i++) {
            boxes[used * 3 + i] = (databox){};
        }

        uint64_t id = 0;
        const bool more =
            idRowsIteratorNext(&iter, &id, row, &wide[used * 3 + 1]);
        if (more) {
            uint64_t count = 0;
            atomPersistCountOf_(row[1], &count);
            boxes[used * 3] = DATABOX_UNSIGNED(id);
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

/* ORDERED: the ids in value order as the reverse map; NULL when a value is
 * bound twice */
static multimap *atomPersistOrderedIndex_(const multimapAtom *ma,
                                          const databoxBig *values,
                                          databoxBig **byValue,
                                          const uint64_t *ids, size_t atoms) {
    qsort(byValue, atoms, sizeof(*byValue), atomPersistCompareValues_);
    /* The reverse map's builder takes the rows */
    /* @own: container; free=multimapFree */
    flex *rows = flexNew();
    for (size_t r = 0; r < atoms; r++) {
        const databox ref = atomRefBox_(ids[byValue[r] - values]);
        flexPushByType(&rows, &ref, FLEX_ENDPOINT_TAIL);
    }

    /* @own: caller; free=multimapFree */
    return multimapNewFromDumpWithReference(
        1, dkTierDefaultLimit(DK_TIER_MULTIMAP_ATOM), rows, ma);
}

/* One HASHED row before it is placed: a value's hash, its id, and which
 * value it is (for the duplicate check) */
typedef struct atomHashedRow_ {
    uint64_t id;
    uint32_t hash;
    uint32_t value;
} atomHashedRow_;

static int atomHashedRowCompare_(const void *a, const void *b) {
    const atomHashedRow_ *x = a;
    const atomHashedRow_ *y = b;
    if (x->hash != y->hash) {
        return x->hash < y->hash ? -1 : 1;
    }

    return (x->id > y->id) - (x->id < y->id);
}

/* HASHED: the {hash, id} set; NULL when a value is bound twice (equal
 * values share a hash, so only rows within one hash are compared) */
static multimap *atomPersistHashedIndex_(const databoxBig *values,
                                         const uint64_t *ids, size_t atoms) {
    cleanerName(zfree, Void) void *storage =
        zcalloc(atoms ? atoms : 1, sizeof(atomHashedRow_));
    atomHashedRow_ *rows = storage;
    for (size_t r = 0; r < atoms; r++) {
        rows[r] = (atomHashedRow_){
            .id = ids[r],
            .hash = (uint32_t)atomHashBox_((const databox *)&values[r]).data.u,
            .value = (uint32_t)r};
    }

    qsort(rows, atoms, sizeof(*rows), atomHashedRowCompare_);
    for (size_t r = 1; r < atoms; r++) {
        for (size_t k = r; k > 0 && rows[k - 1].hash == rows[r].hash; k--) {
            if (databoxCompare((const databox *)&values[rows[k - 1].value],
                               (const databox *)&values[rows[r].value]) == 0) {
                return nullptr;
            }
        }
    }

    /* @own: container; free=multimapFree */
    flex *f = flexNew();
    for (size_t r = 0; r < atoms; r++) {
        const databox hash = DATABOX_UNSIGNED(rows[r].hash);
        const databox id = DATABOX_UNSIGNED(rows[r].id);
        const databox *row[2] = {&hash, &id};
        flexAppendMultipleExact(&f, 2, row);
    }

    /* @own: caller; free=multimapFree */
    return multimapNewFromDump(2, true, false,
                               dkTierDefaultLimit(DK_TIER_MULTIMAP_ATOM), f);
}

/* DICT: each value -> its id, added to the empty dict 'd' sized for them
 * up front; false when a value is bound twice */
static bool atomPersistDictIndex_(multidict *d, const databoxBig *values,
                                  const uint64_t *ids, size_t atoms) {
    multidictExpand(d, atoms);
    for (size_t r = 0; r < atoms; r++) {
        const databox id = DATABOX_UNSIGNED(ids[r]);
        if (multidictAddNX(d, (const databox *)&values[r], &id) !=
            MULTIDICT_OK_INSERTED) {
            return false;
        }
    }

    return true;
}

static void *multimapAtomPersistRestore_(const uint8_t *data, size_t len) {
    if (!flexValidateBufferDeep(data, len)) {
        return nullptr;
    }

    const flex *f = (const flex *)data;
    const size_t n = flexCount(f);
    flexEntry *fe = flexHead(f);
    uint64_t flags;
    uint64_t next;
    if (n < 2 || (n - 2) % 3 || !flexReadUnsigned(f, &fe, &flags) ||
        !flexReadUnsigned(f, &fe, &next) ||
        (flags & ~(uint64_t)(ATOM_PERSIST_FLAG_PERMANENT_ |
                             ATOM_PERSIST_FLAG_INDEX_)) ||
        next >= MULTIMAP_ATOM_ID_LIMIT) {
        return nullptr;
    }

    const uint64_t indexField =
        (flags & ATOM_PERSIST_FLAG_INDEX_) >> ATOM_PERSIST_INDEX_SHIFT_;
    if (indexField > MULTIMAP_ATOM_INDEX_DICT) {
        return nullptr;
    }

    cleaner(multimapAtomFree) multimapAtom *ma =
        multimapAtomNewIndexed((multimapAtomIndex)indexField,
                               (flags & ATOM_PERSIST_FLAG_PERMANENT_) != 0);
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
        if (!flexReadUnsigned(f, &fe, &ids[r])) {
            return nullptr;
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
            return nullptr;
        }

        const databox countStored =
            count ? DATABOX_UNSIGNED(count) : (databox){.type = DATABOX_FALSE};
        const databox *forwardRow[2] = {(const databox *)&values[r],
                                        &countStored};
        idRowsSet(ma->forward, ids[r], forwardRow);
        byValue[r] = &values[r];
    }

    /* The index built straight from the rows (every builder refuses a
     * value bound to two ids) */
    const multimapAtomIndex index = multimapAtomGetIndex(ma);
    if (index == MULTIMAP_ATOM_INDEX_DICT) {
        if (!atomPersistDictIndex_(ma->dictAtomReverse, values, ids, atoms)) {
            return nullptr;
        }

        atomDictSettle_(ma);
    } else {
        /* @own: container; free=multimapFree */
        multimap *reverse =
            index == MULTIMAP_ATOM_INDEX_HASHED
                ? atomPersistHashedIndex_(values, ids, atoms)
                : atomPersistOrderedIndex_(ma, values, byValue, ids, atoms);
        if (!reverse) {
            return nullptr;
        }

        multimapFree(ma->mapAtomReverse);
        ma->mapAtomReverse = reverse;
    }

    atomSetNextId_(ma, next);
    multimapAtom *out = ma;
    ma = nullptr;
    return out;
}

static uint8_t *multimapAtomPersistEncodeOp_(persistOp op, const void *args,
                                             size_t argc, size_t *len) {
    *len = 0;
    const bool shaped = (op == PERSIST_OP_INSERT && argc == 2) ||
                        (op == PERSIST_OP_NUM_INCR && argc == 2) ||
                        (op == PERSIST_OP_DELETE && argc == 1);
    if (!shaped) {
        return nullptr;
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

    *ref = atomRefBox_(id->data.u64);
    return idRowsExists(ma->forward, id->data.u64);
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
            conformRefcountDecr(ma, &ref, nullptr);
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
    return idRowsCount(ma->forward) == atomIndexCount_(ma);
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
    printf("Forward rows (%zu, %zu bytes):\n", idRowsCount(ma->forward),
           idRowsBytes(ma->forward));
    idRowsIterator iter;
    idRowsIteratorInit(ma->forward, &iter);
    uint64_t id;
    databox value = {};
    databox count = {};
    databox *row[2] = {&value, &count};
    databoxWide wide[2];
    while (idRowsIteratorNext(&iter, &id, row, wide)) {
        printf("  %" PRIu64 ": ", id);
        databoxReprSay("", &value);
    }

    printf("Reverse Maps:\n");
    if (multimapAtomGetIndex(ma) == MULTIMAP_ATOM_INDEX_DICT) {
        multidictPrintStats(ma->dictAtomReverse);
    } else {
        multimapRepr(ma->mapAtomReverse);
    }
}

/* Callback for iterator */
static bool populateSet(void *userData, const databox *elements[]) {
    multimap **setMap = userData;

    multimapInsert(setMap, elements);

    return true;
}

/* populateSet for a HASHED index's {hash, id} rows: the id */
static bool populateSetHashedIds(void *userData, const databox *elements[]) {
    return populateSet(userData, &elements[1]);
}

/* Every id the reverse index holds, inserted into the set 'ids' (a
 * duplicate id shows as fewer set members than index rows) */
static void atomTestIndexIds_(const multimapAtom *ma, multimap **ids) {
    if (multimapAtomGetIndex(ma) == MULTIMAP_ATOM_INDEX_DICT) {
        multidictIterator iter;
        if (!multidictIteratorInit(ma->dictAtomReverse, &iter)) {
            return;
        }

        multidictEntry e;
        while (multidictIteratorNext(&iter, &e)) {
            const databox *id[1] = {&e.val};
            multimapInsert(ids, id);
        }

        multidictIteratorRelease(&iter);
        return;
    }

    multimapPredicate p = {.condition = MULTIMAP_CONDITION_ALL};
    multimapProcessUntil(ma->mapAtomReverse, &p, true,
                         multimapAtomGetIndex(ma) == MULTIMAP_ATOM_INDEX_HASHED
                             ? populateSetHashedIds
                             : populateSet,
                         ids);
}

#include "ctest.h"
#include "multimapFull.h"
#include "timeUtil.h"
#include <inttypes.h>

defcleanerName(uint64_t *, zfree, U64);
defcleaner(multimap *, multimapFree);

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
    ctestPoisonStack();
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

defcleaner(flex *, flexFree);

/* The bytes 'box' adds to a flex, measured by pushing it onto one */
static uint64_t atomTestPushedBytes_(const databox *box) {
    cleaner(flexFree) flex *f = flexNew();
    flexPushByTypeExact(&f, box, FLEX_ENDPOINT_TAIL);
    return flexBytes(f) - FLEX_EMPTY_SIZE;
}

/* The key of test atom 'i', written into 'buf' */
static databox atomTestKey_(char *buf, size_t cap, int32_t i) {
    snprintf(buf, cap, "atom-value-%06" PRId32, i);
    return databoxNewBytesString(buf);
}

/* What a test atom's stored count reads as */
typedef enum atomTestRefs {
    ATOM_TEST_REFS_READ = 0,      /* '*refs' holds its references (0: none) */
    ATOM_TEST_REFS_NOT_COUNT = 1, /* the count field is not an integer */
} atomTestRefs;

/* The references test atom 'i' holds, into '*refs': 0 when there is no
 * such atom, else the stored 0-based count plus one */
static atomTestRefs atomTestStored_(const multimapAtom *ma, char *buf,
                                    size_t cap, int32_t i, int64_t *refs) {
    const databox key = atomTestKey_(buf, cap, i);
    databox ref = {};
    *refs = 0;
    if (!multimapAtomLookupReference(ma, &key, &ref)) {
        return ATOM_TEST_REFS_READ;
    }

    databox count = {};
    multimapAtomLookupRefcount(ma, &ref, &count);
    switch (count.type) {
    case DATABOX_FALSE:
        *refs = 1;
        return ATOM_TEST_REFS_READ;
    case DATABOX_TRUE:
        *refs = 2;
        return ATOM_TEST_REFS_READ;
    case DATABOX_SIGNED_64:
        *refs = count.data.i64 + 1;
        return ATOM_TEST_REFS_READ;
    case DATABOX_UNSIGNED_64:
        *refs = (int64_t)count.data.u64 + 1;
        return ATOM_TEST_REFS_READ;
    default:
        return ATOM_TEST_REFS_NOT_COUNT;
    }
}

/* The suite, run once per index configuration by multimapAtomTest */
static int atomTestSuite_(void) {
    int err = 0;

    uint32_t seed = time(nullptr);
    srandom(seed);
    printf("Random seed: %" PRIu32 "\n", seed);

    TEST("create / free") {
        multimapAtom *ma = multimapAtomNew();
        if (multimapAtomCount(ma) != 0) {
            ERR("a new atom map holds %zu atoms", multimapAtomCount(ma));
        }

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

        const size_t forwardLen = idRowsCount(ma->forward);
        const size_t reverseLen = atomIndexCount_(ma);

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

        for (size_t i = 0; i < countof(vals); i++) {
            const int16_t *use = vals[i];
            if (i >= backupDeleteBy) {
                /* Remove i - 5 */
                printf("Removing %zu\n", i - backupDeleteBy);
                const int16_t *remove = vals[i - backupDeleteBy];

                for (size_t j = 0; j < loopExtent; j++) {
                    databox box = databoxNewSigned(remove[j]);
                    const databox key = box;

                    databox count;
                    assert(multimapAtomLookupReference(ma, &key, &count));

                    /* Quick sanity check to make sure it does exist and isn't
                     * created if we do a re-insert of known-existing value. */
                    multimapAtomInsertIfNewConvert(ma, &box);
                    assert(box.created == false);

                    assert(multimapAtomLookupReference(ma, &key, &count));

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
                assert(multimapAtomLookupReference(ma, &key, &count));

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
        /* 1..doUntil-1: every value 'i % mod' divides by is nonzero */
        const size_t mod = 1 + (size_t)random() % (doUntil - 1);
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

            ssize_t forwardLen = idRowsCount(ma->forward);
            ssize_t reverseLen = atomIndexCount_(ma);

            if (forwardLen != doUntil) {
                ERR("Expected forward length to be %zd but it was %zd!\n",
                    doUntil, forwardLen);
                multimapAtomRepr(ma);
                assert(false);
            }

            if (reverseLen != doUntil) {
                ERR("Expected reverse length to be %zd but it was %zd!\n",
                    doUntil, reverseLen);
                multimapAtomRepr(ma);
                assert(false);
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
        for (size_t j = 0; j < countof(sizes); j++) {
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

            ssize_t forwardLen = idRowsCount(ma->forward);
            ssize_t reverseLen = atomIndexCount_(ma);

            if (forwardLen != doUntil) {
                ERR("Expected forward length to be %zd but it was %zd!\n",
                    doUntil, forwardLen);
                multimapAtomRepr(ma);
                assert(false);
            }

            if (reverseLen != doUntil) {
                ERR("Expected reverse length to be %zd but it was %zd!\n",
                    doUntil, reverseLen);
                multimapAtomRepr(ma);
                assert(false);
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
            forwardLen = idRowsCount(ma->forward);
            reverseLen = atomIndexCount_(ma);

            if (forwardLen != 0) {
                ERR("Expected forward length to be 0 but it was %zd!\n",
                    forwardLen);
                multimapAtomRepr(ma);
                assert(false);
            }

            if (reverseLen != 0) {
                ERR("Expected reverse length to be 0 but it was %zd!\n",
                    reverseLen);
                multimapAtomRepr(ma);
                assert(false);
            }
        }

        printf("\n");
        multimapAtomFree(ma);
    }

    TEST("populate and delete and populate (providing atom id)") {
        multimapAtom *ma = multimapAtomNew();
        static const ssize_t sizes[] = {0, 1, 30, 90, 449, 450, 451, 900, 1800};
        for (size_t j = 0; j < countof(sizes); j++) {
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

            ssize_t forwardLen = idRowsCount(ma->forward);
            ssize_t reverseLen = atomIndexCount_(ma);

            assert(forwardLen == doUntil);
            assert(reverseLen == doUntil);

            /* GO AWAY */
            for (ssize_t i = 0; i < doUntil; i++) {
                multimapAtomDeleteById(ma, i);
            }

            forwardLen = idRowsCount(ma->forward);
            reverseLen = atomIndexCount_(ma);

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

        const ssize_t forwardLen = idRowsCount(ma->forward);
        const ssize_t reverseLen = atomIndexCount_(ma);

        assert(forwardLen == doUntil);
        assert(reverseLen == doUntil);

        /* test for uniqueness in reverse list */
        cleaner(multimapFree) multimap *duplicateHolder = multimapSetNew(1);

        atomTestIndexIds_(ma, &duplicateHolder);

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
        if (idRowsCount(ma->forward) != 1) {
            ERR("forward has %zu rows for one id, expected 1",
                idRowsCount(ma->forward));
        }
        if (atomIndexCount_(ma) != 1) {
            ERR("reverse has %zu rows, expected 1", atomIndexCount_(ma));
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
        /* Rows go straight into the forward rows ({value, refcount} by
         * id): every lookup under test reads only those rows.  The values are
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
            const databox *row[2] = {&vals[i], &DATABOX_BOX_FALSE};
            idRowsSet(ma->forward, idBase + i, row);
        }

        /* Pass 0 reads one page of forward rows; pass 1 adds filler rows
         * on pages of their own after it. */
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 1) {
                for (uint64_t f = 0; f < 400; f++) {
                    const databox fill = databoxNewUnsigned(f);
                    const databox *row[2] = {&fill, &DATABOX_BOX_FALSE};
                    idRowsSet(ma->forward, 1000 + f, row);
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
                ctestPoisonStack();
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

            if (pass == 1 && idRowsCount(ma->forward) < 400) {
                ERR("filler rows missing from the forward rows");
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
                ctestPoisonStack();
                multimapAtomInsert(ma, &vals[i]);
            }

            if (pass == 1) {
                /* An iterator records the tier of the map it walks (a
                 * DICT index is no multimap); the forward rows span
                 * several pages */
                bool reverseFull = true;
                if (multimapAtomGetIndex(ma) != MULTIMAP_ATOM_INDEX_DICT) {
                    multimapIterator reverseIter;
                    multimapIteratorInit(ma->mapAtomReverse, &reverseIter,
                                         true);
                    reverseFull = reverseIter.type == MULTIMAP_TYPE_FULL;
                }

                if (idRowsCount(ma->forward) < 2 * IDROWS_PAGE ||
                    !reverseFull) {
                    ERR("filler atoms did not make both maps Full");
                }
            }

            uint64_t ids[pathValueCount] = {};
            for (size_t i = 0; i < pathValueCount; i++) {
                databox ref = {};
                ctestPoisonStack();
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
                ctestPoisonStack();
                multimapAtomRetain(ma, &vals[i]);

                databox viaRetain = vals[i];
                ctestPoisonStack();
                multimapAtomRetainConvert(ma, &viaRetain);
                if (!atomTestIsCleanRef(&viaRetain, ids[i])) {
                    ERR("pass %d value %zu: RetainConvert box wrong", pass, i);
                }

                databox viaInsertRetain = vals[i];
                ctestPoisonStack();
                if (!multimapAtomInsertIfNewConvertAndRetain(
                        ma, &viaInsertRetain) ||
                    !atomTestIsCleanRef(&viaInsertRetain, ids[i])) {
                    ERR("pass %d value %zu: InsertIfNewConvertAndRetain box "
                        "wrong",
                        pass, i);
                }

                databox viaInsert = vals[i];
                ctestPoisonStack();
                if (!multimapAtomInsertIfNewConvert(ma, &viaInsert) ||
                    !atomTestIsCleanRef(&viaInsert, ids[i])) {
                    ERR("pass %d value %zu: InsertIfNewConvert box wrong", pass,
                        i);
                }

                ctestPoisonStack();
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
                        ctestPoisonStack();
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
                    ctestPoisonStack();
                    removed = multimapAtomDelete(ma, &vals[i]);
                    break;
                default:
                    ctestPoisonStack();
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

                ctestPoisonStack();
                if (multimapAtomDeleteByRef(ma, &ref)) {
                    ERR("pass %d value %zu: deleted twice", pass, i);
                }

                for (size_t j = 0; j < pathValueCount; j++) {
                    const databox refJ = {
                        .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                        .data.u = ids[j]};
                    databox found = {};
                    ctestPoisonStack();
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
                ctestPoisonStack();
                multimapAtomInsertWithExactAtomID(ma, exact, &vals[i]);
                ctestPoisonStack();
                multimapAtomInsertWithExactAtomID(ma, exact, &rebound);

                databox found = {};
                ctestPoisonStack();
                if (multimapAtomLookupReference(ma, &vals[i], &found)) {
                    ERR("pass %d value %zu: still bound after rebind", pass, i);
                }

                if (!atomTestResolvesTo(ma, &ref, &rebound) ||
                    multimapAtomCount(ma) != fill + 1) {
                    ERR("pass %d value %zu: rebind left the table wrong", pass,
                        i);
                }

                ctestPoisonStack();
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
                ERR("atom %d got id %" PRIuMAX, i, ids[i].data.u);
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
        const databox absent = {.data.u = 3,
                                .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
        databox none = {};
        if (!multimapAtomLookupRefcount(ma, &ids[1], &count) ||
            multimapAtomLookupRefcount(ma, &absent, &none) ||
            none.type != DATABOX_VOID || multimapAtomCount(ma) != 3 ||
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
            ERR("permanent ids repeated: d %" PRIuMAX ", e %" PRIuMAX, d.data.u,
                e.data.u);
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
            ERR("after the top release the next id is %" PRIuMAX ", want 4",
                f.data.u);
        }

        /* An interior hole stays unused */
        multimapAtomReleaseById(ma, &v[2]);
        databox g = databoxNewSigned(301);
        multimapAtomInsertConvert(ma, &g);
        if (g.data.u != 5) {
            ERR("an interior release moved the next id to %" PRIuMAX ", want 5",
                g.data.u);
        }

        /* Releasing down from the top skips back over the hole only to
         * the largest live id */
        multimapAtomReleaseById(ma, &g); /* live 0 1 3 4: next 5 */
        multimapAtomReleaseById(ma, &f); /* live 0 1 3: next 4 */
        databox h = databoxNewSigned(302);
        multimapAtomInsertConvert(ma, &h);
        if (h.data.u != 4) {
            ERR("the next id after releasing 5 and 4 is %" PRIuMAX ", want 4",
                h.data.u);
        }

        /* Last-in-first-out churn reuses one id */
        for (int i = 0; i < 10000; i++) {
            databox x = databoxNewSigned(1'000'000 + i);
            multimapAtomInsertConvert(ma, &x);
            if (x.data.u != 5) {
                ERR("churn %d got id %" PRIuMAX ", want 5", i, x.data.u);
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
            ERR("generated id %" PRIuMAX " reuses the exact-id range",
                gen.data.u);
        }

        const databox exactRef = {.type = DATABOX_CONTAINER_REFERENCE_EXTERNAL,
                                  .data.u = 5};
        databox found = {};
        if (idRowsCount(ma->forward) != 2 ||
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
            if (!ok || multimapAtomCount(ma) != 0 || atomIndexCount_(ma) != 0 ||
                multimapAtomLookupReference(ma, &aKey, &ref) ||
                multimapAtomLookupReference(ma, &bKey, &ref)) {
                ERR("way %d: the table did not empty on both sides", how);
            }

            databox c = databoxNewSigned(3);
            multimapAtomInsertConvert(ma, &c);
            if (c.data.u != 0) {
                ERR("way %d: first id after emptying is %" PRIuMAX ", want 0",
                    how, c.data.u);
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
            atomIndexCount_(ma) != 1 ||
            multimapAtomLookupReference(ma, &goneKey, &ref)) {
            ERR("the last release did not remove both rows");
        }

        databox found = {};
        if (!multimapAtomLookup(ma, &keep, &found) || found.data.i != 10 ||
            !multimapAtomDelete(ma, &keepKey) || multimapAtomCount(ma) != 0 ||
            atomIndexCount_(ma) != 0) {
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
        constexpr int atoms = 3000;
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
                        ERR("order %d: second offer of %s gave ref %" PRIuMAX
                            " existed %d, first gave %" PRIu64,
                            order, name, k.data.u, existed, refs[i]);
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
        for (size_t s = 0; s < countof(sizes); s++) {
            const int32_t n = sizes[s];
            cleaner(multimapAtomFree) multimapAtom *ma = multimapAtomNew();
            for (int32_t i = 0; i < n; i++) {
                snprintf(name, sizeof(name), "tiered-%06" PRId32, i);
                databox k = databoxNewBytesString(name);
                multimapAtomInsertIfNewConvert(ma, &k);
            }

            if (multimapAtomGetIndex(ma) == MULTIMAP_ATOM_INDEX_DICT) {
                /* No multimap tiers: every check below still runs */
                sawTier[MULTIMAP_TYPE_SMALL] = sawTier[MULTIMAP_TYPE_MEDIUM] =
                    sawTier[MULTIMAP_TYPE_FULL] = true;
            } else {
                sawTier[multimapGetType(ma->mapAtomReverse)] = true;
            }

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
                atomIndexCount_(ma) != multimapAtomCount(ma)) {
                ERR("%d atoms: %zu left (%zu reverse rows)", n,
                    multimapAtomCount(ma), atomIndexCount_(ma));
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
        constexpr int atoms = 3000;
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
            atomIndexCount_(ma) != atoms / 4) {
            ERR("after the releases: %zu atoms, %zu reverse rows, expected "
                "%d",
                multimapAtomCount(ma), atomIndexCount_(ma), atoms / 4);
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

    TEST("GetStats: references, entry bytes and the net saving") {
        /* A 48-byte value held 5 times and a 1-byte value held once.  The
         * oracle sizes each entry by pushing it onto a real flex, not
         * through the encoder the report uses */
        multimapAtom *ma = multimapAtomNew();
        const char *longValue =
            "a value long enough that sharing it pays off!!!!";
        databox big = databoxNewBytesString(longValue);
        databox tiny = databoxNewBytesString("x");
        databox bigRef = big;
        databox tinyRef = tiny;
        multimapAtomInsertIfNewConvert(ma, &bigRef);
        multimapAtomInsertIfNewConvert(ma, &tinyRef);
        for (int r = 0; r < 4; r++) {
            multimapAtomRetainById(ma, bigRef.data.u);
        }

        const uint64_t bigEntry = atomTestPushedBytes_(&big);
        const uint64_t tinyEntry = atomTestPushedBytes_(&tiny);
        const uint64_t bigRefEntry = atomTestPushedBytes_(&bigRef);
        const uint64_t tinyRefEntry = atomTestPushedBytes_(&tinyRef);

        multimapAtomStats s;
        multimapAtomGetStats(ma, &s);
        const uint64_t inlineWant = 5 * bigEntry + tinyEntry;
        const uint64_t refWant = 5 * bigRefEntry + tinyRefEntry;
        if (s.atoms != 2 || s.references != 6 ||
            s.valueBytes != bigEntry + tinyEntry ||
            s.tableBytes != multimapAtomBytes(ma) ||
            s.inlineBytes != inlineWant || s.referenceBytes != refWant ||
            s.savedBytes !=
                (int64_t)inlineWant - (int64_t)(s.tableBytes + refWant)) {
            ERR("stats: %" PRIu64 " atoms, %" PRIu64 " refs, values %" PRIu64
                ", inline %" PRIu64 " (want %" PRIu64 "), refs %" PRIu64
                " (want %" PRIu64 "), saved %" PRId64,
                s.atoms, s.references, s.valueBytes, s.inlineBytes, inlineWant,
                s.referenceBytes, refWant, s.savedBytes);
        }

        /* A lone 1-byte value costs more shared than inline */
        multimapAtom *lone = multimapAtomNew();
        databox x = databoxNewBytesString("x");
        multimapAtomInsertIfNewConvert(lone, &x);
        multimapAtomStats ls;
        multimapAtomGetStats(lone, &ls);
        if (ls.atoms != 1 || ls.references != 1 || ls.savedBytes >= 0) {
            ERR("a lone tiny value reported a saving of %" PRId64,
                ls.savedBytes);
        }

        /* A permanent table counts one reference per atom */
        multimapAtom *perm = multimapAtomNewPermanent();
        databox p = databoxNewBytesString(longValue);
        multimapAtomInsertIfNewConvert(perm, &p);
        multimapAtomRetainById(perm, p.data.u);
        multimapAtomStats ps;
        multimapAtomGetStats(perm, &ps);
        if (ps.atoms != 1 || ps.references != 1) {
            ERR("a permanent table counted %" PRIu64 " references",
                ps.references);
        }

        multimapAtomFree(perm);
        multimapAtomFree(lone);
        multimapAtomFree(ma);
    }

    TEST("retains and releases by key count at the row the search found") {
        /* Retain and release by key update the count at the forward row
         * their reverse search resolved; by id they search.  A replayable
         * mix of all six over enough atoms for both maps to reach the Full
         * tier, against an oracle of each atom's references (0: no atom),
         * checked for the touched atom after every operation and for all
         * of them every 1000 */
        typedef enum atomTestOp {
            ATOM_TEST_OP_INTERN_RETAIN = 0,
            ATOM_TEST_OP_RETAIN,
            ATOM_TEST_OP_RETAIN_CONVERT,
            ATOM_TEST_OP_RELEASE,
            ATOM_TEST_OP_RELEASE_BY_ID,
            ATOM_TEST_OP_RETAIN_BY_ID,
            ATOM_TEST_OP_COUNT
        } atomTestOp;

        constexpr int32_t atoms = 2000;
        constexpr int32_t ops = 20000;
        int64_t oracle[atoms] = {};

        multimapAtom *ma = multimapAtomNew();
        uint64_t rng = 0xA70C0;
        char name[32];
        for (int32_t op = 0; op < ops; op++) {
            const int32_t i = (int32_t)dkSplitmix64Bounded(&rng, atoms);
            const atomTestOp kind =
                (atomTestOp)dkSplitmix64Bounded(&rng, ATOM_TEST_OP_COUNT);
            databox key = atomTestKey_(name, sizeof(name), i);
            databox ref = {};
            const bool present = oracle[i] > 0;
            if (present) {
                multimapAtomLookupReference(ma, &key, &ref);
            }

            bool existed = present;
            switch (kind) {
            case ATOM_TEST_OP_INTERN_RETAIN:
                existed = multimapAtomInsertIfNewConvertAndRetain(ma, &key);
                oracle[i]++;
                break;
            case ATOM_TEST_OP_RETAIN:
                multimapAtomRetain(ma, &key);
                oracle[i]++;
                break;
            case ATOM_TEST_OP_RETAIN_CONVERT:
                multimapAtomRetainConvert(ma, &key);
                oracle[i]++;
                break;
            case ATOM_TEST_OP_RELEASE:
                if (present) {
                    const bool deleted = multimapAtomRelease(ma, &key);
                    oracle[i]--;
                    if (deleted != (oracle[i] == 0)) {
                        ERR("op %" PRId32 ": release of %s deleted %d", op,
                            name, deleted);
                    }
                }
                break;
            case ATOM_TEST_OP_RELEASE_BY_ID:
                if (present) {
                    multimapAtomReleaseById(ma, &ref);
                    oracle[i]--;
                }
                break;
            case ATOM_TEST_OP_RETAIN_BY_ID:
                if (present) {
                    multimapAtomRetainById(ma, ref.data.u);
                    oracle[i]++;
                }
                break;
            case ATOM_TEST_OP_COUNT:
                break;
            }

            if (existed != present) {
                ERR("op %" PRId32 " (kind %d): existed %d for an atom %s", op,
                    (int)kind, existed, present ? "present" : "absent");
                break;
            }

            int64_t refs = 0;
            if (atomTestStored_(ma, name, sizeof(name), i, &refs) !=
                    ATOM_TEST_REFS_READ ||
                refs != oracle[i]) {
                ERR("op %" PRId32 " (kind %d): %s holds %" PRId64
                    " references, want %" PRId64,
                    op, (int)kind, name, refs, oracle[i]);
                break;
            }

            if (op % 1000 == 999) {
                for (int32_t a = 0; a < atoms; a++) {
                    if (atomTestStored_(ma, name, sizeof(name), a, &refs) !=
                            ATOM_TEST_REFS_READ ||
                        refs != oracle[a]) {
                        ERR("after op %" PRId32 ": %s holds %" PRId64
                            " references, want %" PRId64,
                            op, name, refs, oracle[a]);
                        break;
                    }
                }
            }
        }

        multimapAtomFree(ma);
    }

    return err;
}

/* Test value 'i' of a mix where cross-type equality matters: bytes, a
 * signed integer, the same integer as a double (equal to the integer
 * form of i - 1 when i is odd), and a 128-bit value */
static databox atomTestMixedValue_(char *buf, size_t cap, int32_t i,
                                   databoxBig *wide) {
    switch (i % 4) {
    case 0:
        return atomTestKey_(buf, cap, i);
    case 1:
        return databoxNewSigned(i / 2);
    case 2:
        return DATABOX_DOUBLE((double)(i / 2));
    default:
        DATABOX_BIG_UNSIGNED_128(wide, ((__uint128_t)1 << 100) + (uint64_t)i);
        return *(const databox *)wide;
    }
}

/* Every index kind, with its name, in the order reports list them */
typedef struct atomTestKind_ {
    multimapAtomIndex index;
    const char *name;
} atomTestKind_;

static const atomTestKind_ atomTestKinds_[] = {
    {MULTIMAP_ATOM_INDEX_ORDERED, "ordered"},
    {MULTIMAP_ATOM_INDEX_HASHED, "hashed"},
    {MULTIMAP_ATOM_INDEX_DICT, "dict"},
};

#define ATOM_TEST_KINDS countof(atomTestKinds_)

/* One table per index kind fed one op stream: interning (new and
 * present), retains and releases by key, deletes and exact-id rebinding
 * over values of four types.  Every result, id and count must match the
 * ordered table's, before and after a snapshot round trip. */
static int atomTestDifferential_(void) {
    int err = 0;
    typedef enum atomTestDiffOp {
        ATOM_TEST_DIFF_INTERN = 0,
        ATOM_TEST_DIFF_RETAIN,
        ATOM_TEST_DIFF_RELEASE,
        ATOM_TEST_DIFF_DELETE,
        ATOM_TEST_DIFF_EXACT_ID,
        ATOM_TEST_DIFF_COUNT
    } atomTestDiffOp;

    constexpr int32_t values = 600;
    multimapAtom *t[ATOM_TEST_KINDS];
    for (size_t k = 0; k < ATOM_TEST_KINDS; k++) {
        t[k] = multimapAtomNewIndexed(atomTestKinds_[k].index, false);
    }

    uint64_t rng = 0xD1FF;
    char name[32];
    for (int32_t op = 0; op < 8000 && !err; op++) {
        const int32_t i = (int32_t)dkSplitmix64Bounded(&rng, values);
        const atomTestDiffOp kind =
            (atomTestDiffOp)dkSplitmix64Bounded(&rng, ATOM_TEST_DIFF_COUNT);
        databoxBig wide;
        const databox value = atomTestMixedValue_(name, sizeof(name), i, &wide);
        databox ref[ATOM_TEST_KINDS] = {};
        bool present[ATOM_TEST_KINDS];
        for (size_t k = 0; k < ATOM_TEST_KINDS; k++) {
            present[k] = multimapAtomLookupReference(t[k], &value, &ref[k]);
        }

        bool result[ATOM_TEST_KINDS] = {};
        uint64_t interned[ATOM_TEST_KINDS] = {};
        for (size_t k = 0; k < ATOM_TEST_KINDS; k++) {
            switch (kind) {
            case ATOM_TEST_DIFF_INTERN: {
                databox v = value;
                result[k] = multimapAtomInsertIfNewConvert(t[k], &v);
                interned[k] = v.data.u;
                break;
            }
            case ATOM_TEST_DIFF_RETAIN:
                multimapAtomRetain(t[k], &value);
                break;
            case ATOM_TEST_DIFF_RELEASE:
                if (present[0]) {
                    result[k] = multimapAtomRelease(t[k], &value);
                }
                break;
            case ATOM_TEST_DIFF_DELETE:
                result[k] = multimapAtomDelete(t[k], &value);
                break;
            case ATOM_TEST_DIFF_EXACT_ID:
                /* Rebinding: replaces any earlier binding of the id or of
                 * the value */
                multimapAtomInsertWithExactAtomID(
                    t[k], 2000 + (uint64_t)(i % 40), &value);
                break;
            case ATOM_TEST_DIFF_COUNT:
                break;
            }
        }

        for (size_t k = 1; k < ATOM_TEST_KINDS; k++) {
            if (present[k] != present[0] ||
                (present[0] && ref[k].data.u != ref[0].data.u) ||
                result[k] != result[0] || interned[k] != interned[0] ||
                multimapAtomCount(t[k]) != multimapAtomCount(t[0])) {
                ERR("op %" PRId32 " (kind %d) value %" PRId32
                    ": %s present %d id %" PRIu64 " result %d count %zu, "
                    "ordered present %d id %" PRIu64 " result %d count %zu",
                    op, (int)kind, i, atomTestKinds_[k].name, present[k],
                    ref[k].data.u64, result[k], multimapAtomCount(t[k]),
                    present[0], ref[0].data.u64, result[0],
                    multimapAtomCount(t[0]));
                break;
            }
        }
    }

    /* Every value resolves alike, before and after a snapshot */
    for (int round = 0; round < 2 && !err; round++) {
        for (int32_t i = 0; i < values; i++) {
            databoxBig wide;
            const databox value =
                atomTestMixedValue_(name, sizeof(name), i, &wide);
            databox ref[ATOM_TEST_KINDS] = {};
            bool present[ATOM_TEST_KINDS];
            for (size_t k = 0; k < ATOM_TEST_KINDS; k++) {
                present[k] = multimapAtomLookupReference(t[k], &value, &ref[k]);
                if (present[k] != present[0] ||
                    (present[0] && ref[k].data.u != ref[0].data.u)) {
                    ERR("round %d: value %" PRId32 " resolves apart under %s",
                        round, i, atomTestKinds_[k].name);
                }
            }
        }

        if (round == 0) {
            for (size_t k = 0; k < ATOM_TEST_KINDS; k++) {
                multimapAtomStats s;
                multimapAtomGetStats(t[k], &s);
                if (s.atoms) {
                    printf("    %" PRIu64 " atoms, %s: %.1f table bytes per "
                           "atom\n",
                           s.atoms, atomTestKinds_[k].name,
                           (double)s.tableBytes / (double)s.atoms);
                }

                size_t len = 0;
                cleanerName(zfree, Void) void *snap =
                    persistOpsMultimapAtom.snapshot(t[k], &len);
                multimapAtomFree(t[k]);
                t[k] = persistOpsMultimapAtom.restore(snap, len);
                if (!t[k] ||
                    multimapAtomGetIndex(t[k]) != atomTestKinds_[k].index) {
                    ERR("a %s snapshot did not restore with its index",
                        atomTestKinds_[k].name);
                    if (!t[k]) {
                        t[k] = multimapAtomNewIndexed(atomTestKinds_[k].index,
                                                      false);
                    }
                }
            }
        }
    }

    for (size_t k = 0; k < ATOM_TEST_KINDS; k++) {
        multimapAtomFree(t[k]);
    }

    return err;
}

int multimapAtomTest(int, char *[]) {
    int err = 0;

    /* The whole suite once per reverse index; the colliding-hash walk is
     * the differential stream's to drive (with 2-bit hashes the suite's
     * large tables would walk thousands of rows per find) */
    for (size_t k = 0; k < ATOM_TEST_KINDS; k++) {
        printf("\n=== multimapAtom suite: %s index ===\n\n",
               atomTestKinds_[k].name);
        atomTestDefaultIndex_ = atomTestKinds_[k].index;
        err += atomTestSuite_();
    }

    atomTestDefaultIndex_ = MULTIMAP_ATOM_INDEX_HASHED;

    for (size_t p = 0; p < 2; p++) {
        atomTestHashMask_ = p ? 0x3 : UINT32_MAX;
        TEST_DESC("every index kind agrees on one op stream (%s)",
                  p ? "2-bit hashes" : "full hashes") {
            err += atomTestDifferential_();
        }
    }

    atomTestHashMask_ = UINT32_MAX;

    TEST("index kinds: heap bytes per atom, measured at the allocator") {
        /* The same atoms under each kind, the whole table (structs,
         * pointer arrays, slots, allocator slack) metered: a kind's
         * memory cost as the process pays it, beside the bytes
         * multimapAtomBytes reports (map contents only).  ORDERED is the
         * smallest index and DICT the largest, for every value shape. */
        typedef enum atomTestShape {
            ATOM_TEST_SHAPE_SHORT = 0, /* 11-byte strings */
            ATOM_TEST_SHAPE_LONG,      /* 48-byte strings */
            ATOM_TEST_SHAPE_INTEGER,   /* integers past one byte */
            ATOM_TEST_SHAPE_COUNT
        } atomTestShape;
        static const char *const shapeNames[ATOM_TEST_SHAPE_COUNT] = {
            "11-byte strings", "48-byte strings", "integers"};

        constexpr int32_t atoms = 20000;
        char buf[64];
        for (int shape = 0; shape < ATOM_TEST_SHAPE_COUNT; shape++) {
            double perAtom[ATOM_TEST_KINDS] = {};
            for (size_t k = 0; k < ATOM_TEST_KINDS && !err; k++) {
                if (!ctestHeapMeterStart()) {
                    printf("(skipped: the allocator reports no usable "
                           "sizes)\n");
                    break;
                }

                multimapAtom *ma =
                    multimapAtomNewIndexed(atomTestKinds_[k].index, false);
                for (int32_t i = 0; i < atoms; i++) {
                    databox value;
                    switch ((atomTestShape)shape) {
                    case ATOM_TEST_SHAPE_SHORT:
                        snprintf(buf, sizeof(buf), "atom-%06" PRId32, i);
                        value = databoxNewBytesString(buf);
                        break;
                    case ATOM_TEST_SHAPE_LONG:
                        snprintf(buf, sizeof(buf),
                                 "a long shared value, number %026" PRId32, i);
                        value = databoxNewBytesString(buf);
                        break;
                    default:
                        value = databoxNewSigned(1'000'000 + (int64_t)i * 7);
                        break;
                    }

                    multimapAtomInsertIfNewConvert(ma, &value);
                }

                const int64_t heap = ctestHeapMeterStop();
                const size_t reported = multimapAtomBytes(ma);
                const size_t count = multimapAtomCount(ma);
                multimapAtomFree(ma);
                perAtom[k] = (double)heap / atoms;
                printf("    %s, %s: %.1f heap bytes per atom (%.1f "
                       "reported)\n",
                       shapeNames[shape], atomTestKinds_[k].name, perAtom[k],
                       (double)reported / atoms);
                if (count != (size_t)atoms || heap <= 0 ||
                    reported > (size_t)heap) {
                    ERR("%s, %s: %zu atoms, %" PRId64 " heap bytes, %zu "
                        "reported",
                        shapeNames[shape], atomTestKinds_[k].name, count, heap,
                        reported);
                }
            }

            if (perAtom[MULTIMAP_ATOM_INDEX_ORDERED] > 0 &&
                (perAtom[MULTIMAP_ATOM_INDEX_ORDERED] >=
                     perAtom[MULTIMAP_ATOM_INDEX_HASHED] ||
                 perAtom[MULTIMAP_ATOM_INDEX_HASHED] >=
                     perAtom[MULTIMAP_ATOM_INDEX_DICT])) {
                ERR("%s: heap per atom ordered %.1f, hashed %.1f, dict %.1f "
                    "(want rising)",
                    shapeNames[shape], perAtom[MULTIMAP_ATOM_INDEX_ORDERED],
                    perAtom[MULTIMAP_ATOM_INDEX_HASHED],
                    perAtom[MULTIMAP_ATOM_INDEX_DICT]);
            }
        }
    }

    TEST("a WAL op the table refuses changes nothing: wrong shapes, dead and "
         "out-of-range ids, a bound id, every truncation") {
        const persistOps *ops = &persistOpsMultimapAtom;
        for (size_t k = 0; k < countof(atomTestKinds_); k++) {
            multimapAtom *ma =
                multimapAtomNewIndexed(atomTestKinds_[k].index, false);
            for (int i = 0; i < 3; i++) {
                databox v = databoxNewSigned(1000 + i);
                multimapAtomInsertConvert(ma, &v); /* ids 0, 1, 2 */
            }

            size_t beforeLen = 0;
            cleanerName(zfree, Void) void *before =
                ops->snapshot(ma, &beforeLen);
            if (ops->estimateSize(ma) != multimapAtomBytes(ma)) {
                ERR("%s: estimateSize differs from Bytes",
                    atomTestKinds_[k].name);
            }

            const databox live = databoxNewUnsigned(1);
            const databox dead = databoxNewUnsigned(7);
            const databox past = databoxNewUnsigned(MULTIMAP_ATOM_ID_LIMIT);
            const databox negative = databoxNewSigned(-1);
            const databox value = databoxNewBytesString("new");
            const databox two = databoxNewSigned(2);
            /* A reference to another container is no value an atom holds
             * (a pointer would be: it is stored as an integer) */
            const databox ptr = {.data.u = 1,
                                 .type = DATABOX_CONTAINER_REFERENCE_EXTERNAL};
            const struct {
                persistOp op;
                const databox *args[2];
                size_t argc;
            } refused[] = {
                {PERSIST_OP_INSERT, {&live, &value}, 2},     /* id bound */
                {PERSIST_OP_INSERT, {&past, &value}, 2},     /* past the ids */
                {PERSIST_OP_INSERT, {&negative, &value}, 2}, /* not an id */
                {PERSIST_OP_INSERT, {&dead, &ptr}, 2},       /* not a value */
                {PERSIST_OP_NUM_INCR, {&dead, &two}, 2},     /* dead id */
                {PERSIST_OP_NUM_INCR, {&live, &two}, 2},     /* not +1 or -1 */
                {PERSIST_OP_NUM_INCR, {&live, &value}, 2},   /* not a number */
                {PERSIST_OP_DELETE, {&dead}, 1},             /* dead id */
                {PERSIST_OP_DELETE, {&past}, 1},             /* past the ids */
            };
            for (size_t r = 0; r < countof(refused); r++) {
                size_t len = 0;
                cleanerName(zfree, Void) void *body = persistOpEncodeBoxes(
                    refused[r].args, refused[r].argc, &len);
                if (ops->applyOp(&ma, refused[r].op, body, len)) {
                    ERR("%s: refused op %zu was applied",
                        atomTestKinds_[k].name, r);
                }
            }

            /* A valid insert's body cut short, and an op the table has
             * no meaning for */
            const databox *insertArgs[2] = {&dead, &value};
            size_t len = 0;
            cleanerName(zfree, Void) void *insert =
                ops->encodeOp(PERSIST_OP_INSERT, insertArgs, 2, &len);
            for (size_t cut = 0; cut < len; cut++) {
                if (ops->applyOp(&ma, PERSIST_OP_INSERT, insert, cut)) {
                    ERR("%s: an insert cut to %zu bytes was applied",
                        atomTestKinds_[k].name, cut);
                }
            }
            if (ops->applyOp(&ma, PERSIST_OP_CLEAR, insert, len) ||
                ops->encodeOp(PERSIST_OP_INSERT, insertArgs, 1, &len)) {
                ERR("%s: an op without its shape was taken",
                    atomTestKinds_[k].name);
            }

            size_t afterLen = 0;
            cleanerName(zfree, Void) void *after = ops->snapshot(ma, &afterLen);
            if (afterLen != beforeLen || memcmp(before, after, afterLen) != 0) {
                ERR("%s: a refused op changed the table",
                    atomTestKinds_[k].name);
            }
            multimapAtomFree(ma);
        }
    }

    TEST_FINAL_RESULT;
}
#endif
