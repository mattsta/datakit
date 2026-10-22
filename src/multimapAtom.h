#pragma once

/* multimapAtom: a reference-counted atom table (interning).  Each distinct
 * value is stored once and named by a small integer id, handed out as a
 * DATABOX_CONTAINER_REFERENCE_EXTERNAL box (a "ref").  Two multimaps hold
 * it: forward {id -> value, refcount} sorted by id, and reverse {id}
 * sorted by value, so value -> id and id -> value are both O(log n).
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
 *   that value.
 * - An id is valid while its atom exists.  Once the atom is deleted (by
 *   the last Release or by a Delete) its id is not reused while the table
 *   holds anything, but when the table empties, by any call, ids restart
 *   at 0 to stay small.  So a caller must not keep a bare id past the
 *   point where its own references are released, or across a point where
 *   the table can empty and refill: it may then name a later, unrelated
 *   value.  Hold a reference (Retain) for as long as the id is kept. */

#include "mflex.h"
#include "multimap.h"

typedef struct multimapAtom multimapAtom;

typedef struct multimapAtomResult {
    databox val;
    uint64_t refcount;
} multimapAtomResult;

void multimapAtomInit(multimapAtom *ma);
multimapAtom *multimapAtomNew(void);
multimapAtom *multimapAtomNewCompress(mflexState *state);
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
void multimapAtomLookupRefcount(const multimapAtom *ma, const databox *key,
                                databox *count);
bool multimapAtomLookupReference(const multimapAtom *ma, const databox *key,
                                 databox *atomRef);

bool multimapAtomLookupMin(const multimapAtom *ma, databox *minRef);

void multimapAtomRetain(multimapAtom *ma, const databox *key);
void multimapAtomRetainByRef(multimapAtom *ma, const databox *foundRef);
void multimapAtomRetainById(multimapAtom *ma, uint64_t id);

bool multimapAtomRelease(multimapAtom *ma, const databox *key);
bool multimapAtomReleaseById(multimapAtom *ma, const databox *foundRef);
bool multimapAtomDelete(multimapAtom *ma, const databox *key);

bool multimapAtomDeleteByRef(multimapAtom *ma, const databox *ref);
bool multimapAtomDeleteById(multimapAtom *ma, const uint64_t id);

size_t multimapAtomCount(const multimapAtom *ma);
size_t multimapAtomBytes(const multimapAtom *ma);

#ifdef DATAKIT_TEST
void multimapAtomRepr(const multimapAtom *ma);
int multimapAtomTest(int argc, char *argv[]);
#endif
