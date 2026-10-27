#pragma once

/* multimapAtom: a reference-counted atom table (interning).  Each distinct
 * value is stored once and named by a small integer id, handed out as a
 * DATABOX_CONTAINER_REFERENCE_EXTERNAL box (a "ref").  Two multimaps hold
 * it: forward {id -> value, refcount} sorted by id, and a reverse index
 * from value to id (multimapAtomIndex below: ids sorted by value, or
 * value hashes with ids).
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
 * ID BEHAVIOURS (chosen at creation; neither adds a byte per atom)
 * - Reusing (multimapAtomNew): the last Release deletes an atom, and ids
 *   are reused where that costs nothing to discover: deleting the atom
 *   with the highest id lowers the next id to just past the largest live
 *   one, so last-in-first-out churn reuses the same ids, and an emptied
 *   table restarts at 0.  An interior hole stays unused until the table
 *   empties.  An id is valid while its atom exists: a caller must not keep
 *   a bare id after releasing its references, as it may then name a later,
 *   unrelated value.  Hold a reference (Retain) for as long as the id is
 *   kept.
 * - Permanent (multimapAtomNewPermanent), like Erlang atoms: an atom lives
 *   until Delete or Free.  Retain and Release are accepted and change
 *   nothing (no count is kept), and no id is ever handed out twice, not
 *   even after a Delete, so an id names one value for the table's life.
 * - Lowest first: multimapAtomDense (multimapAtomDense.h) reuses every
 *   freed id, always handing out the lowest id not in use. */

#include "multimap.h"

typedef struct multimapAtom multimapAtom;

/* Ids are below this (the counter's top two bits record the id behaviour
 * and the reverse index) */
#define MULTIMAP_ATOM_ID_LIMIT ((uint64_t)1 << 62)

/* How the table finds a value's id (chosen at creation; neither adds a
 * pointer):
 * - ORDERED: the reverse rows are ids sorted by value.  A find compares
 *   about log n rows, each through a forward-map lookup of its id (about
 *   log^2 n).  The smaller index.
 * - HASHED: the reverse rows are {hash of the value, id}, sorted by hash.
 *   A find is an integer search for the hash, then one forward lookup per
 *   candidate (normally one): about log n.  About 5 more bytes per atom.
 * Equality is databoxCompare's in both (5, 5.0 and a 128-bit 5 are one
 * value), and the hash never reaches a snapshot: restore rebuilds it. */
typedef enum multimapAtomIndex {
    MULTIMAP_ATOM_INDEX_ORDERED = 0,
    MULTIMAP_ATOM_INDEX_HASHED = 1,
} multimapAtomIndex;

typedef struct multimapAtomResult {
    databox val;
    uint64_t refcount;
} multimapAtomResult;

void multimapAtomInit(multimapAtom *ma);
multimapAtom *multimapAtomNew(void);
/* A permanent table (see ID BEHAVIOURS above) */
multimapAtom *multimapAtomNewPermanent(void);
/* A table with the reverse index 'index', reusing or permanent ids.
 * multimapAtomNew and NewPermanent make HASHED tables. */
multimapAtom *multimapAtomNewIndexed(multimapAtomIndex index, bool permanent);
bool multimapAtomIsPermanent(const multimapAtom *ma);
multimapAtomIndex multimapAtomGetIndex(const multimapAtom *ma);
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
/* The atom of 'key' in one search: true when it existed ('ref' its
 * reference), false when this call created it with one reference under
 * 'id' ('ref' the new reference).  'id' must not be bound to another
 * value: a table that picks its own ids (multimapAtomDense) passes the
 * one it will take when the atom is new. */
bool multimapAtomFindOrCreateWithId(multimapAtom *ma, const databox *key,
                                    uint64_t id, databox *ref);

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
void multimapAtomLookupRefcount(const multimapAtom *ma, const databox *key,
                                databox *count);
bool multimapAtomLookupReference(const multimapAtom *ma, const databox *key,
                                 databox *atomRef);

void multimapAtomRetain(multimapAtom *ma, const databox *key);
/* Retain 'key' (inserting it when new) and replace it with its reference */
void multimapAtomRetainConvert(multimapAtom *ma, databox *key);
void multimapAtomRetainByRef(multimapAtom *ma, const databox *foundRef);
void multimapAtomRetainById(multimapAtom *ma, uint64_t id);

bool multimapAtomRelease(multimapAtom *ma, const databox *key);
bool multimapAtomReleaseById(multimapAtom *ma, const databox *foundRef);
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
} multimapAtomStats;

/* Fill 'stats' for 'ma': one pass over the atoms */
void multimapAtomGetStats(const multimapAtom *ma, multimapAtomStats *stats);

#ifdef DATAKIT_TEST
void multimapAtomRepr(const multimapAtom *ma);
int multimapAtomTest(int argc, char *argv[]);
#endif
