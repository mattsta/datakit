#pragma once

/* multimapAtom: a reference-counted atom table (interning).  Each distinct
 * value is stored once and named by a small integer id, handed out as a
 * DATABOX_CONTAINER_REFERENCE_EXTERNAL box (a "ref").  Two maps hold it:
 * forward {id -> value, refcount} rows addressed by id (multimapAtomStore:
 * a read by id is positional, with no search), and a reverse index from
 * value to id (multimapAtomIndex: ids sorted by value, value hashes with
 * ids, or a table of id slots probed by hash).
 *
 * REFERENCES
 * - An Insert creates the atom with one reference.  Retain adds one;
 *   Release drops one and deletes the atom when the last one goes (and
 *   returns true then).  Delete removes an atom whatever its count.
 * - The count is stored 0-based (0 = one reference), so it costs one byte
 *   while it is a single reference.  LookupRefcount and LookupResult
 *   report that stored 0-based count; add 1 for the number of references.
 *
 * IDS
 * - Generated ids (Insert*Convert, Retain of a new value) count up from 0
 *   and always continue past every id InsertWithExactAtomID has bound, so
 *   a generated id is never one already in use.  InsertWithExactAtomID
 *   binds the caller's id, replacing any earlier binding of that id or of
 *   that value.  Ids are below MULTIMAP_ATOM_ID_LIMIT.
 *
 * ID BEHAVIOURS (multimapAtomIds, chosen at creation; none adds a byte
 * per atom)
 * - REUSE (multimapAtomNew): the last Release deletes an atom, and ids
 *   are reused where that costs nothing to discover: deleting the atom
 *   with the highest id lowers the next id to just past the largest live
 *   one, so last-in-first-out churn reuses the same ids, and an emptied
 *   table restarts at 0.  An interior hole stays unused until the table
 *   empties.
 * - PERMANENT, like Erlang atoms: an atom lives until Delete or Free.
 *   Retain and Release are accepted and change nothing (no count is
 *   kept), and no id is ever handed out twice, not even after a Delete,
 *   so an id names one value for the table's life.
 * - LOWEST: every freed id is reused, and a new atom always gets the
 *   lowest id not in use, so the ids in use never exceed the high-water
 *   mark of the live count.  For ids that index something sized by their
 *   range (arrays, bitmaps, compact reference encodings) while atoms come
 *   and go in any order.  Freed ids below the highest live one are kept
 *   in a multiroar, made on the first such hole and freed when the last
 *   one fills: about 2 bytes per isolated hole, none while there are
 *   none.
 * Under REUSE and LOWEST an id is valid while its atom exists: a caller
 * must not keep a bare id after releasing its references, as it may then
 * name a later, unrelated value.  Hold a reference (Retain) for as long
 * as the id is kept. */

#include "multimap.h"

/* @handle: fixed: its address holds from creation to free */
typedef struct multimapAtom multimapAtom;

/* Ids are below this (the counter's top five bits record the table's
 * settings) */
#define MULTIMAP_ATOM_ID_LIMIT ((uint64_t)1 << 59)

typedef enum multimapAtomIds {
    MULTIMAP_ATOM_IDS_REUSE = 0,
    MULTIMAP_ATOM_IDS_PERMANENT = 1,
    MULTIMAP_ATOM_IDS_LOWEST = 2,
} multimapAtomIds;

/* How the table finds a value's id (chosen at creation; the kind costs no
 * byte):
 * - ORDERED: the reverse rows are ids sorted by value.  A find compares
 *   about log n rows, each read positionally from the forward rows by its
 *   id.  The smallest index.
 * - HASHED: the reverse rows are {hash of the value, id}, sorted by hash.
 *   A find is an integer search for the hash, then one forward lookup per
 *   candidate (normally one): about log n.  About 5 more bytes per atom.
 * - SLOTS: an open-addressed table of ids, 9 bytes a slot (the id and a
 *   control byte holding 7 bits of the value's hash), kept between 3/8
 *   and 3/4 full.  A find is one hash and a probe from its slot; a slot
 *   whose hash bits match resolves its id in the forward rows (normally
 *   one).  Constant time, the fastest finds, and about 12-24 bytes per
 *   atom whatever the value's length: values are held once, by the
 *   forward rows.
 * Equality is databoxCompare's in all three (5, 5.0 and a 128-bit 5 are
 * one value).  HASHED and SLOTS hash with a per-process random seed, so
 * colliding values cannot be computed offline.  No index reaches a
 * snapshot: restore rebuilds it. */
typedef enum multimapAtomIndex {
    MULTIMAP_ATOM_INDEX_HASHED = 0,
    MULTIMAP_ATOM_INDEX_ORDERED = 1,
    MULTIMAP_ATOM_INDEX_SLOTS = 2,
} multimapAtomIndex;

typedef struct multimapAtomResult {
    databox val;
    uint64_t refcount;
} multimapAtomResult;

/* Where the table holds each atom's value and count (chosen at creation;
 * a lookup by id reads it directly under both):
 * - PACKED: idRows pages, the values encoded as flex entries (a small
 *   integer in a byte or two, a string with a byte or two of length).
 *   The compact store.
 * - DIRECT: one 24-byte row per id (the value's box and its count),
 *   strings over 8 bytes in their own allocation.  A read by id is an
 *   array index and a count change is one add: the fast store, for
 *   tables read and retained by id far more than they are searched. */
typedef enum multimapAtomStore {
    MULTIMAP_ATOM_STORE_PACKED = 0,
    MULTIMAP_ATOM_STORE_DIRECT = 1,
} multimapAtomStore;

/* A table's settings: a zeroed config is the default (HASHED, REUSE,
 * PACKED) */
typedef struct multimapAtomConfig {
    multimapAtomIndex index; /* HASHED (the zero value), ORDERED, SLOTS */
    multimapAtomIds ids;     /* REUSE (the zero value), PERMANENT, LOWEST */
    multimapAtomStore store; /* PACKED (the zero value), DIRECT */
} multimapAtomConfig;

/* A default table (HASHED index, REUSE ids, PACKED store) */
multimapAtom *multimapAtomNew(void);
/* A table with 'config's settings; NULL for an unknown index, ids or store */
multimapAtom *multimapAtomNewConfig(const multimapAtomConfig *config);
/* A copy of 'ma': its settings, every atom at the same id with the same
 * count, the same next id (so references into 'ma' read the same in it) */
multimapAtom *multimapAtomCopy(const multimapAtom *ma);
multimapAtomIds multimapAtomGetIds(const multimapAtom *ma);
multimapAtomIndex multimapAtomGetIndex(const multimapAtom *ma);
multimapAtomStore multimapAtomGetStore(const multimapAtom *ma);
/* The id the next new atom gets (the lowest unused one under LOWEST) */
uint64_t multimapAtomNextId(const multimapAtom *ma);
void multimapAtomFree(multimapAtom *ma);

/* The *Convert calls (InsertConvert, InsertIfNewConvert*, RetainConvert)
 * replace the caller's value box with the atom's reference as a whole
 * box: the 'big' and 'allocated' bits of the value it held, such as a
 * 96/128-bit value's, are cleared, and nothing it owned is freed. */
void multimapAtomInsert(multimapAtom *ma, const databox *box);
void multimapAtomInsertConvert(multimapAtom *ma, databox *box);
bool multimapAtomInsertIfNewConvert(multimapAtom *ma, databox *key);
bool multimapAtomInsertIfNewConvertAndRetain(multimapAtom *ma, databox *key);

void multimapAtomInsertWithExactAtomID(multimapAtom *ma, const uint64_t atomRef,
                                       const databox *key);
/* Resolve the reference 'ref' to its value in 'key'.  'key' is a
 * caller-provided output box: pass a DATABOX_BIG_INIT'd databoxBig to
 * receive a 96/128-bit value, or a zeroed box to receive only its type
 * (see databox.h). */
bool multimapAtomLookup(const multimapAtom *ma, const databox *ref,
                        databox *key);

/* Replace the reference in 'box' with the value it names.  The reference
 * is copied out before the decode, so the box is never both the lookup
 * key and the output.  A plain databox has no room for a 96/128-bit
 * value: such a value is reported type-only (data.ptr NULL, big = 0).
 * A reference that is not in the table leaves 'box' unchanged. */
void multimapAtomLookupConvert(const multimapAtom *ma, databox *box);

/* As multimapAtomLookupConvert, into storage that holds any value.  Only
 * the reference (type and payload) is read from 'box', so stale header
 * bits are harmless.  On return allocated = 0, and big = 1 exactly for a
 * 96/128-bit value, which lives in box->extra with data.i128 / data.u128
 * aimed at it.  A BYTES value borrows from the atom table.  A reference
 * that is not in the table is left in 'box' with big = 0. */
void multimapAtomLookupConvertBig(const multimapAtom *ma, databoxBig *box);

/* 'result->val' is a plain databox, so a 96/128-bit value is reported
 * type-only (data.ptr NULL, big = 0). */
void multimapAtomLookupResult(const multimapAtom *ma, const databox *key,
                              multimapAtomResult *result);
/* The stored 0-based count of the atom 'key' (a reference); false, with
 * 'count' untouched, when no atom has that id */
bool multimapAtomLookupRefcount(const multimapAtom *ma, const databox *key,
                                databox *count);
bool multimapAtomLookupReference(const multimapAtom *ma, const databox *key,
                                 databox *atomRef);

void multimapAtomRetain(multimapAtom *ma, const databox *key);
/* Retain 'key' (inserting it when new) and replace it with its reference */
void multimapAtomRetainConvert(multimapAtom *ma, databox *key);
void multimapAtomRetainByRef(multimapAtom *ma, const databox *foundRef);
void multimapAtomRetainById(multimapAtom *ma, uint64_t id);

/* One reference fewer to the value 'key', the reference 'ref' or the id
 * 'id'; the last one deletes the atom.  True when the atom was deleted
 * (false too for a value, reference or id not in the table). */
bool multimapAtomRelease(multimapAtom *ma, const databox *key);
bool multimapAtomReleaseByRef(multimapAtom *ma, const databox *ref);
bool multimapAtomReleaseById(multimapAtom *ma, uint64_t id);

/* Interning by plain ids (an atom's id as a uint64_t, from 0):
 * - Intern: the id of 'value' with one reference more, the atom created
 *   with its one reference when new.
 * - GetId: the id of 'value' without a reference; false when absent.
 * - LookupById: the value of 'id' into 'value' (as multimapAtomLookup:
 *   BYTES borrow from the table; a 96/128-bit value needs a
 *   DATABOX_BIG_INIT'd databoxBig); false when no atom has the id.
 * - RefcountById: the references held (a PERMANENT table's atoms hold
 *   one), 0 when no atom has the id.
 * Retain and release by id with RetainById / ReleaseById. */
uint64_t multimapAtomIntern(multimapAtom *ma, const databox *value);
bool multimapAtomGetId(const multimapAtom *ma, const databox *value,
                       uint64_t *id);
bool multimapAtomLookupById(const multimapAtom *ma, uint64_t id,
                            databox *value);
uint64_t multimapAtomRefcountById(const multimapAtom *ma, uint64_t id);

/* Every atom gone, the settings kept: ids start over */
void multimapAtomReset(multimapAtom *ma);
bool multimapAtomDelete(multimapAtom *ma, const databox *key);

bool multimapAtomDeleteByRef(multimapAtom *ma, const databox *ref);
bool multimapAtomDeleteById(multimapAtom *ma, const uint64_t id);

size_t multimapAtomCount(const multimapAtom *ma);
size_t multimapAtomBytes(const multimapAtom *ma);

/* What sharing values through the table costs and saves.  Sizes are bytes
 * as flex entries store them: a value's or an id's entry is what it adds
 * to a flex.  'savedBytes' is inlineBytes - (tableBytes + referenceBytes):
 * positive when the table holds its values for less than the references
 * would take holding each value themselves, negative when it costs more
 * (few references per value, or values about as small as an id). */
typedef struct multimapAtomStats {
    uint64_t atoms;          /* values in the table */
    uint64_t references;     /* references held; one per atom when permanent */
    uint64_t valueBytes;     /* each value's entry, once */
    uint64_t tableBytes;     /* the table's maps (multimapAtomBytes) */
    uint64_t inlineBytes;    /* each reference holding its value's entry */
    uint64_t referenceBytes; /* each reference holding its id's entry */
    int64_t savedBytes;      /* inlineBytes - (tableBytes + referenceBytes) */
    uint32_t unitLimit;      /* bytes per node of the table's multimap index
                                (the atom family's limit when made:
                                dkTierLimit.h); 0 for a SLOTS index */
} multimapAtomStats;

/* Fill 'stats' for 'ma': one pass over the atoms */
void multimapAtomGetStats(const multimapAtom *ma, multimapAtomStats *stats);

#ifdef DATAKIT_TEST
void multimapAtomRepr(const multimapAtom *ma);
int multimapAtomTest(int argc, char *argv[]);
#endif
