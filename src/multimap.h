#pragma once

#include "dkTier.h"
#include "dkTierLimit.h"
#include "flexCapacityManagement.h"
#include "multimapCommon.h"

#include <assert.h>

/* opaque multimap type; there's no user accessible data here. */
typedef struct multimap multimap;

multimap *multimapNew(multimapElements elementsPerEntry);
multimap *multimapNewLimit(multimapElements elementsPerEntry,
                           const flexCapSizeLimit limit);
multimap *multimapNewCompress(multimapElements elementsPerEntry,
                              const flexCapSizeLimit limit);
multimap *multimapNewConfigure(multimapElements elementsPerEntry, bool isSet,
                               bool compress, flexCapSizeLimit sizeLimit);
multimap *multimapSetNew(multimapElements elementsPerEntry);
multimap *multimapCopy(const multimap *m);
size_t multimapCount(const multimap *m);
size_t multimapBytes(const multimap *m);
flex *multimapDump(const multimap *m);
/* True for a multimap made by multimapSetNew (or multimapNewConfigure
 * with isSet): rows are deduplicated whole, so one key may hold several
 * rows.  False for a map, where inserting a key replaces its row. */
bool multimapIsSet(const multimap *m);
/* multimapNewFromDump for a map of atom references: 'rows' (taken) hold
 * reference keys whose values, resolved through 'referenceContainer',
 * strictly ascend.  Built straight into the tier and inner maps inserts
 * would reach under the size limit 'limit', in O(rows) beyond the order
 * check.  NULL, with 'rows' freed, when the rows are not whole, out of
 * order or repeat a value. */
multimap *
multimapNewFromDumpWithReference(multimapElements elementsPerEntry,
                                 flexCapSizeLimit limit, flex *rows,
                                 const struct multimapAtom *referenceContainer);
/* The tier currently holding the rows (Small, Medium or Full) */
multimapType multimapGetType(const multimap *m);
/* The tier, inner maps (1, 2, or Full's count), rows, and bytes: the rows'
 * flex bytes, and those plus each tier's struct and Full's per-map arrays.
 * 'unitLimit' is the size limit's byte cutoff, where Small and Full's inner
 * maps split. */
void multimapStats(const multimap *m, dkTierStats *out);
/* A multimap holding `rows` (full-width rows, as multimapDump writes them),
 * configured as multimapNewConfigure configures one (`isSet`, `compress`,
 * `limit`): a dump larger than the Medium tier holds under `limit` is cut
 * straight into Full's inner maps of about `limit` bytes in O(rows), with
 * no per-row insert, and later inserts split at `limit` too.  Takes `rows`.
 * NULL, with `rows` freed, when the rows are not whole or not in the
 * multimap's row order (ascending by whole row; a set's rows strictly
 * ascending). */
multimap *multimapNewFromDump(multimapElements elementsPerEntry, bool isSet,
                              bool compress, flexCapSizeLimit limit,
                              flex *rows);

/* The configuration a multimap was made with, in every tier: its size
 * limit (when a tier grows, and the size of Full's inner maps) and whether
 * it compresses */
flexCapSizeLimit multimapLimit(const multimap *m);
bool multimapCompresses(const multimap *m);

bool multimapInsert(multimap **m, const databox *elements[]);
/* multimapInsert that also reports the value entries a replace overwrote
 * through 'replacedOut' ('elementsPerEntry - 1' slots in tuple order after
 * the key, every one VOID when nothing was replaced).  Integers up to 64
 * bits, reals and pointers are reported by value and stay valid.  A BYTES
 * or container slot points into the rewritten map and must not be read.  A
 * 96/128-bit integer does not fit a databox, so its slot reports only the
 * type (SIGNED_128 or UNSIGNED_128) with a NULL payload pointer (data.i128
 * or data.u128); read such a value before the insert if it is needed. */
bool multimapInsertReplacing(multimap **m, const databox *elements[],
                             databox *replacedOut);
/* Inserts the row only when no row has its key, in one search: true when
 * inserted, false when the key was present (the map unchanged).  In a set
 * any row with the key counts as present. */
bool multimapInsertIfAbsent(multimap **m, const databox *elements[]);
/* multimapInsertIfAbsent that, when the key is present, fills 'me' with
 * its row (as multimapGetUnderlyingEntry finds it: a set's first row with
 * the key) for a change in place: find-or-insert in one search. */
bool multimapInsertIfAbsentEntry(multimap **m, const databox *elements[],
                                 multimapEntry *me);
void multimapInsertFullWidth(multimap **m, const databox *elements[]);

/* multimapInsert for rows arriving in ascending key order: a row at or past
 * the last key goes straight into the highest map; any other row is
 * inserted where it belongs, so the map stays ordered either way */
void multimapAppend(multimap **m, const databox *elements[]);

void multimapInsertWithSurrogateKey(
    multimap **m, const databox *elements[], const databox *insertKey,
    const struct multimapAtom *referenceContainer);
/* For a map of atom references: insert the row under 'insertKey' only
 * when no row holds the key elements[0] (compared by value through
 * 'referenceContainer'), in one search.  True when inserted; false when
 * present, with 'foundKey' the stored reference and the map unchanged.
 * 'whenAbsent' (may be NULL) runs once the key is known absent, before the
 * row goes in: an atom table adds the row the new reference resolves
 * through, which the insert may already need. */
bool multimapInsertWithSurrogateKeyIfAbsent(
    multimap **m, const databox *elements[], const databox *insertKey,
    const struct multimapAtom *referenceContainer, databox *foundKey,
    flexWhenAbsent *whenAbsent, void *whenAbsentCtx);

bool multimapGetUnderlyingEntry(multimap *m, const databox *key,
                                multimapEntry *me);
bool multimapGetUnderlyingEntryWithReference(
    multimap *m, const databox *key, multimapEntry *me,
    const struct multimapAtom *referenceContainer);
/* The row equal to all of 'elements' (key and every value), as
 * multimapExistsFullWidth finds it: true with 'me' naming that row */
bool multimapGetUnderlyingEntryFullWidth(multimap *m, const databox *elements[],
                                         multimapEntry *me);
/* Delete the row 'me' names (filled by GetUnderlyingEntry*, with no change
 * to the map since), without searching for it again.  A map of atom
 * references passes its referenceContainer. */
void multimapDeleteEntry(multimap **m, multimapEntry *me);
void multimapDeleteEntryWithReference(
    multimap **m, multimapEntry *me,
    const struct multimapAtom *referenceContainer);
/* Rewrite in place the entry 'me' names (a value column reached from the
 * row's key with flexNext).  The columns that order rows are never
 * rewritten in place: in a map that is the key (element 0, the caller's
 * to keep off), in a set every element, so on a set these refuse (false,
 * the row unchanged) and report DK_DIAG_MULTIMAP_SET_REWRITE; change such
 * a row by deleting it and inserting the new one.  True when written. */
bool multimapResizeEntry(multimap **m, multimapEntry *me, size_t newLen);
bool multimapReplaceEntry(multimap **m, multimapEntry *me, const databox *box);
/* multimapReplaceEntry for a map whose keys are references into
 * 'referenceContainer': the replace may then re-balance the Full tier's
 * sub-maps (resolving keys through the references), which the plain
 * form must not do without the container */
bool multimapReplaceEntryWithReference(
    multimap **m, multimapEntry *me, const databox *box,
    const struct multimapAtom *referenceContainer);

bool multimapExists(const multimap *m, const databox *key);
bool multimapExistsFullWidth(const multimap *m, const databox *elements[]);
bool multimapExistsWithReference(const multimap *m, const databox *key,
                                 databox *foundRef,
                                 const struct multimapAtom *referenceContainer);

/* Output boxes.  Every read that fills caller boxes (Lookup, First, Last,
 * LookupByPosition, RandomValue, IteratorNext and the *Into helpers)
 * decodes in place under the databox.h rule: the caller owns the boxes
 * and sets up their wide capability.  To receive a 96/128-bit value, pass
 * a databoxBig armed with DATABOX_BIG_INIT, and re-arm it before each
 * read, since a narrow value overwrites the payload pointer.  A zeroed box
 * receives only the type (data.ptr NULL).  An uninitialized box is a
 * caller bug.  Rows the library decodes for itself (ProcessUntil walkers,
 * the key set operations) always carry wide storage, with 'big' set
 * exactly on 96/128-bit values.
 *
 * Removing reads.  DeleteRandomValue hands back values that outlive the
 * row, the way multilistPop does: a 96/128-bit value is written in band
 * when the box has 'big' set (an armed databoxBig), and otherwise comes
 * back in a 16-byte heap payload the caller owns (big = 1,
 * allocated = 1).  BYTES of 8 bytes or less come back embedded
 * (DATABOX_BYTES_EMBED, nothing owned) and longer ones as heap copies.
 * Release each box with databoxFreeData, and initialize 'big' as for any
 * read.  Three removing
 * paths are documented exceptions that report a 96/128-bit value
 * type-only (data.ptr NULL, big = 0) whatever the box holds:
 * InsertReplacing's replacedOut, DeleteReporting's removedOut and
 * DeleteWithFound / DeleteWithReference's foundReference. */
bool multimapLookup(const multimap *m, const databox *key, databox *elements[]);
/* multimapLookup for a map whose keys are references into
 * 'referenceContainer' (written by the WithReference inserts): 'key' is the
 * value, found by what the stored references resolve to */
bool multimapLookupWithReference(const multimap *m, const databox *key,
                                 databox *elements[],
                                 const struct multimapAtom *referenceContainer);

/* NOTE: despite the scalar-looking `databox **found`, this fills a
 * FULL-WIDTH pointer array (elementsPerEntry boxes) when non-NULL -
 * pass NULL or an array sized to the map's width, never a single
 * box's address. */
bool multimapRandomValue(multimap *m, bool fromTail, databox **found,
                         multimapEntry *me);

/* In a set, where one key can hold several rows, a key addresses its FIRST
 * row (the lowest in row order): Lookup returns it, Delete removes it,
 * FieldIncr and GetUnderlyingEntry reach it.  The choice is fixed by the
 * rows alone, never by how the map is laid out, so replaying the same
 * operations always touches the same rows. */
bool multimapDelete(multimap **m, const databox *key);
/* multimapDelete that also reports the deleted row's value entries through
 * 'removedOut' (NULL, or 'elementsPerEntry - 1' slots in tuple order after
 * the key) under InsertReplacing's replacedOut rules: numbers by value, a
 * BYTES or container slot with its type and length but pointing into the
 * rewritten map (not to be read), a 96/128-bit value type-only.  One
 * search, where Lookup then Delete is two. */
bool multimapDeleteReporting(multimap **m, const databox *key,
                             databox *removedOut);
/* multimapDelete that hands the deleted row's value entries back through
 * 'takenOut' (NULL, or 'elementsPerEntry - 1' slots) as DeleteRandomValue
 * does (see "Removing reads"): self-contained and owned by the caller,
 * each released with databoxFreeData.  One search, where Lookup, copy,
 * then Delete is two. */
bool multimapDeleteTaking(multimap **m, const databox *key, databox *takenOut);
bool multimapDeleteFullWidth(multimap **m, const databox *elements[]);
/* foundReference, when non-NULL, receives the deleted row's key as stored
 * (the reference id in a surrogate-keyed map).  A BYTES key points into
 * the rewritten map and must not be read, and a 96/128-bit key is
 * reported type-only (see "Removing reads" above). */
bool multimapDeleteWithReference(multimap **m, const databox *key,
                                 const struct multimapAtom *referenceContainer,
                                 databox *foundReference);
bool multimapDeleteWithFound(multimap **m, const databox *key,
                             databox *foundReference);
bool multimapDeleteRandomValue(multimap **m, bool fromTail, databox **deleted);

/* Add 'incrBy' to the integer field at 'fieldOffset' (1 = first value
 * after the key) of 'key''s row, following the platform increment policy
 * (DATABOX.md "Numeric updates").  The field must be a SIGNED_64, an
 * UNSIGNED_64 up to INT64_MAX, or TRUE/FALSE (counted as 1/0; a boolean
 * stays boolean while the result is 0 or 1).  The sum is exact.
 *
 * multimapFieldIncrBy returns true with the new value in '*newValue' (if
 * non-NULL); it returns false, changing nothing, for a missing key, a field
 * that is not such a counter, a result outside int64, or a set (a set's
 * values order its rows, so changing one in place would misplace the row;
 * delete the row and insert the new one instead).
 *
 * multimapFieldIncr returns the new value, or on any refusal the current
 * value (0 for a missing key or a non-counter field); it cannot report a
 * refusal, so prefer multimapFieldIncrBy. */
bool multimapFieldIncrBy(multimap **m, const databox *key, uint32_t fieldOffset,
                         int64_t incrBy, int64_t *newValue);
/* multimapFieldIncrBy in the same one search that first asks 'proceed'
 * (may be NULL: always proceed) once the increment is known to apply and
 * change the field: a false answer leaves the map unchanged.  True when
 * the field holds the sum (an increment by 0 asks nothing); '*newValue'
 * (may be NULL) is the sum, or else the field's current value (0 for a
 * missing key or a field that is not a counter).  flexProceed: flex.h. */
bool multimapFieldIncrByIf(multimap **m, const databox *key,
                           uint32_t fieldOffset, int64_t incrBy,
                           int64_t *newValue, flexProceed *proceed, void *ctx);
int64_t multimapFieldIncr(multimap **m, const databox *key,
                          uint32_t fieldOffset, int64_t incrBy);

/* Empty the map: an empty Small with the same row width, set mode, limit
 * and compression, freeing a larger tier's structures (a Small keeps its
 * allocation).  Rewrites '*m'. */
void multimapReset(multimap **m);
void multimapFree(multimap *m);

void multimapEntryResize(multimap **m, const databox *key, size_t newSize);
void multimapEntryReplace(multimap **m, const databox *key,
                          const databox *elements[]);

bool multimapFirst(multimap *m, databox *elements[]);
bool multimapLast(multimap *m, databox *elements[]);

/* ====================================================================
 * Reads into a contiguous out-array
 *
 * These build the pointer array the raw API needs so call sites don't
 * hand-roll `databox *ptrs[N] = {&a, &b, ...}` wiring. `n` is the
 * number of boxes THE READ FILLS and each API fills a different count:
 *
 *   - First/Last/IteratorNext fill elementsPerEntry boxes (full width)
 *   - Lookup fills elementsPerEntry - 1 boxes (values only; the key is
 *     yours already)
 *
 * An undersized array gets written past its end (these are not partial
 * reads), so `n` is a contract, not a request. Bounded at
 * MULTIMAP_INTO_MAX; wider maps use the raw API. Zero-cost: with a
 * literal `n` the wiring loop unrolls to the same address-of stores
 * the hand-rolled initializer produced.
 * ==================================================================== */
enum { MULTIMAP_INTO_MAX = 16 };

/* n == elementsPerEntry - 1 (value elements only) */
static inline bool multimapLookupInto(const multimap *m, const databox *key,
                                      databox out[], uint32_t n) {
    assert(n <= MULTIMAP_INTO_MAX);
    databox *ptrs[MULTIMAP_INTO_MAX];
    for (uint32_t i = 0; i < n; i++) {
        ptrs[i] = &out[i];
    }
    return multimapLookup(m, key, ptrs);
}

/* n == elementsPerEntry (full width) */
static inline bool multimapFirstInto(multimap *m, databox out[], uint32_t n) {
    assert(n <= MULTIMAP_INTO_MAX);
    databox *ptrs[MULTIMAP_INTO_MAX];
    for (uint32_t i = 0; i < n; i++) {
        ptrs[i] = &out[i];
    }
    return multimapFirst(m, ptrs);
}

/* n == elementsPerEntry (full width) */
static inline bool multimapLastInto(multimap *m, databox out[], uint32_t n) {
    assert(n <= MULTIMAP_INTO_MAX);
    databox *ptrs[MULTIMAP_INTO_MAX];
    for (uint32_t i = 0; i < n; i++) {
        ptrs[i] = &out[i];
    }
    return multimapLast(m, ptrs);
}

/* Positional row access: keyIndex is 0-based from the head, negative
 * from the tail (-1 = last row).  Rows are located through maintained
 * anchors (sub-map row counts, then the nearest of each flex's head /
 * cached middle / tail), so the walk is over at most a quarter of the
 * owning flex.  Lookup fills FULL-width rows (key included); Delete
 * removes the EXACT addressed row. */
bool multimapDeleteByPosition(multimap **m, int64_t keyIndex);
bool multimapLookupByPosition(multimap *m, int64_t keyIndex,
                              databox *elements[]);

bool multimapDeleteByPredicate(multimap **m, const multimapPredicate *p);
bool multimapProcessPredicate(const multimapPredicate *p, const databox *value);
typedef bool(multimapElementWalker)(void *userData, const databox *elements[]);
size_t multimapProcessUntil(multimap *m, const multimapPredicate *p,
                            bool forward, multimapElementWalker *walker,
                            void *userData);

bool multimapIteratorInitAt(const multimap *m, multimapIterator *iter,
                            bool forward, const databox *box);
void multimapIteratorInit(const multimap *m, multimapIterator *iter,
                          bool forward);
bool multimapIteratorNext(multimapIterator *iter, databox **elements);

/* multimapIteratorNext that decodes 96/128-bit elements into storage the
 * iterating code owns rather than through the caller's boxes.  'slots'
 * holds elementsPerEntry databoxWide slots, one per tuple position:
 * elements[i] is decoded with flexGetByTypeInto into slots[i], so a wide
 * element comes back with big = 1, allocated = 0 and its payload in
 * slots[i], valid until the slot is reused.  A narrow element writes only
 * type, data and len (the boxes' flag bits are left as they were) and
 * never touches its slot.  With NULL 'slots' this is multimapIteratorNext
 * (the in-band caller-box contract above).  This is the entry point for
 * library code that iterates a multimap into its own entries, such as the
 * multidict slot class. */
bool multimapIteratorNextWithSlots(multimapIterator *iter, databox **elements,
                                   databoxWide *slots);

/* Iterator companion to the *Into readers above: same contract, same
 * width rule (`n` == elementsPerEntry). */
static inline bool multimapIteratorNextInto(multimapIterator *iter,
                                            databox out[], uint32_t n) {
    assert(n <= MULTIMAP_INTO_MAX);
    databox *ptrs[MULTIMAP_INTO_MAX];
    for (uint32_t i = 0; i < n; i++) {
        ptrs[i] = &out[i];
    }
    return multimapIteratorNext(iter, ptrs);
}

void multimapIntersectKeys(multimap **restrict dst,
                           multimapIterator *restrict const a,
                           multimapIterator *restrict const b);

void multimapDifferenceKeys(multimap **restrict dst,
                            multimapIterator *restrict const a,
                            multimapIterator *restrict const b,
                            const bool symmetricDifference);

void multimapCopyKeys(multimap **restrict dst, const multimap *restrict src);

#ifdef DATAKIT_TEST
void multimapRepr(const multimap *m);
void multimapVerify(const multimap *m);
int multimapTest(int argc, char *argv[]);
/* Tests the ...InsertReplacing entry points of every layer (flex, Small,
 * Medium, Full, the generic multimap and its tier upgrades): a replace
 * reports the tuple it overwrote, a fresh insert reports VOID.  Returns the
 * number of failures.  Run by multimapTest and multimapFullTest. */
int32_t multimapInsertReplacingSweep(void);
/* Drives 96/128-bit keys and values through Small, Medium and Full maps
 * (insert, lookup, exists, iterate, range, positional reads, ProcessUntil,
 * the key set operations, FieldIncr, delete, trim by predicate, refill,
 * copy, and full-width rows) against an independent 128-bit oracle, with
 * 0xFF-poisoned output boxes.  Returns the number of failures.  Run by
 * multimapTest and multimapFullTest. */
int32_t multimapWideSweep(void);
/* The normal / extended / mixed matrix (64-bit only, 96/128-bit only, and
 * interleaved across the 2^63 / 2^64 / 2^96 boundaries) through Small,
 * Medium and Full maps.  Each map is built, read through every path,
 * trimmed, refilled, copied, read through the removing paths
 * (InsertReplacing, DeleteWithFound, a DeleteRandomValue drain) and freed
 * inside an allocation-counting window that must end where it began.
 * Returns the number of failures. */
int32_t multimapWideMatrixSweep(void);
/* The same three data modes through multimapP: snapshot, WAL and
 * recovery, checked against the oracle before and after reopening.
 * Returns the number of failures. */
int32_t multimapWidePersistSweep(void);
/* Count live blocks from the datakit allocator between Begin and End (not
 * nestable, single-threaded).  End restores the allocator and returns the
 * blocks allocated and not freed inside the window. */
void multimapTestAllocCountBegin(void);
int64_t multimapTestAllocCountEnd(void);
#endif
