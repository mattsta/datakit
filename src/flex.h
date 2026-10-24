#pragma once

#include "databox.h"
__BEGIN_DECLS

/* Pre-declare multimapAtom existence.
 * We don't want to include multimapAtom.h here because of
 * recursive dependencies. */
struct multimapAtom;

typedef enum flexEndpoint {
    FLEX_ENDPOINT_TAIL = -1,
    FLEX_ENDPOINT_HEAD = 0
} flexEndpoint;

/* That's right, an empty 'flex' is just 2 bytes! */
#define FLEX_EMPTY_SIZE 2

typedef uint8_t flex;
typedef uint8_t flexEntry;

typedef uint8_t cflex;

flex *flexNew(void);
void flexReset(flex **ff);
flex *flexDuplicate(const flex *f);
void flexFree(flex *f);

/* Validators for untrusted input.
 *
 * flexValidateBuffer, flexValidateBufferDeep and flexPeekTotalBytes
 * accept any (buf, buflen) pair: each returns true or false without
 * asserting and without reading outside [buf, buf + buflen).
 *
 * The rest of flex assumes well-formed input and asserts on it.  Pass
 * untrusted bytes to flexHead / flexNext / flexGetByType / flexCount
 * only after flexValidateBufferDeep accepts them.  That check covers
 * forward iteration and decoding of this flex's own entries:
 *   - It rejects MDSC entries, whose payload is a pointer into the
 *     process that wrote them, so validated bytes never decode to a
 *     pointer flex would dereference.  Container references
 *     (DATABOX_CONTAINER_REFERENCE_EXTERNAL) are opaque integers that
 *     flex never dereferences; a caller that stores pointers in them
 *     must not accept them from untrusted bytes.
 *   - A 96/128-bit entry decodes fully only into wide-capable storage:
 *     flexGetByTypeBig, or a databoxBig set up with DATABOX_BIG_INIT;
 *     see flexGetByType.
 *   - It does not verify reverse-traversal trailers, so walk untrusted
 *     bytes forward only (flexHead / flexNext, not flexTail / flexPrev).
 *   - It does not verify the contents of nested containers.  A
 *     container box decoded from validated bytes points at nested flex
 *     bytes nobody has checked; validate them before reading them
 *     (flexPeekTotalBytes(start, len, &total), then
 *     flexValidateBufferDeep(start, total)).
 *
 * flexTest runs every single-byte mutation of several valid flexes
 * through these functions and decodes every buffer the deep check
 * accepts. */

/* flexValidateBuffer — defensive bounds-check on a putative flex buffer.
 *
 * Returns true iff `buf` of length `buflen` parses as a well-formed
 * flex header whose self-described total-bytes equals `buflen`.
 *
 * Designed for callers that consume flex bytes from untrusted sources
 * (cross-node replication, on-disk recovery, plugin-supplied byte
 * blobs, etc.) and must NOT crash on malformed input.  It checks only
 * the header; before iterating untrusted bytes with flexCount /
 * flexHead / flexNext / flexGetByType, use flexValidateBufferDeep,
 * which also bounds every entry.
 *
 * Conservative: does NOT walk every entry's bytes (that would
 * duplicate flex's internal logic); it only validates the outer
 * envelope.  This catches the common failure modes:
 *   - input shorter than the minimum header size
 *   - leading varint claims a width that doesn't fit in `buflen`
 *   - decoded total-bytes != actual `buflen`
 *
 * Cost: O(1).  Reads at most 5 bytes from the head of the buffer. */
bool flexValidateBuffer(const uint8_t *buf, size_t buflen);

/* flexValidateBufferDeep — strict O(N) validation of a putative flex
 * buffer.  Walks every entry's length-encoding bytes and confirms each
 * entry fits within the buffer; confirms the entry count claimed by the
 * header matches the number of entries actually present; confirms the
 * walk terminates exactly at the buffer's claimed end (no trailing
 * garbage, no truncation); confirms every negative 128-bit entry holds
 * a value no lower than INT128_MIN (its stored magnitude has bit 127
 * clear; every 96-bit negative magnitude is in range).
 *
 * Returns true iff the buffer is structurally valid for safe forward
 * iteration with flexHead / flexNext / flexGetByType / flexCount, holds
 * no MDSC (pointer) entry, and holds no integer outside the 128-bit
 * range; see the paragraph above for what it does not cover.
 *
 * Designed for callers that consume flex bytes from untrusted sources
 * WITHOUT a wrapping MAC (synthetic fuzzers, cross-process IPC, on-disk
 * recovery prior to MAC verification, plugin-supplied blobs).  The O(1)
 * flexValidateBuffer suffices when bytes are MAC-gated upstream; use
 * this variant when they are not.
 *
 * Cost: O(N) in entry count.  Reads every entry's encoding byte plus
 * its length-encoding bytes, and the top payload byte of a negative
 * 128-bit entry.  Does NOT otherwise decode entry payloads. */
bool flexValidateBufferDeep(const uint8_t *buf, size_t buflen);

/* flexPeekTotalBytes — safely read the leading total-bytes claim from a
 * putative flex buffer WITHOUT requiring `buflen == claim`.
 *
 * Use this when a flex sits as a STRICT PREFIX of a larger buffer (e.g.
 * the inner-frame header sealed inside a clusterMailbox envelope: the
 * flex-encoded header is followed by opaque payload bytes that the flex
 * itself does not cover).  Callers feed `buf` and the OUTER `buflen`;
 * on success `*out_total` holds the prefix length the flex claims,
 * which the caller can then pass to `flexValidateBuffer(buf, *out_total)`
 * for the full header validation and treat `buf + *out_total` as the
 * start of the post-flex region.
 *
 * Returns false on: NULL inputs; `buflen` smaller than the minimum flex
 * envelope; leading-varint width exceeds `buflen`; claim larger than
 * `buflen`.  Cost: O(1).  Reads at most 5 bytes from the head. */
bool flexPeekTotalBytes(const uint8_t *buf, size_t buflen, size_t *out_total);

flexEntry *flexMiddle(const flex *f, uint_fast32_t elementsPerEntry);

/* Merge */
flex *flexMerge(flex **first, flex **second);
void flexBulkAppendFlex(flex **ff, const flex *zzb);
flex *flexBulkMergeFlex(const flex *const *const fs, const size_t count);

/* BULK BYTE-MEMBER APPEND — the O(total bytes) form of "append these N
 * byte members at the tail".
 *
 * flex has NO capacity concept: every append reallocs to the EXACT new
 * size (flexResize_ -> zrealloc(newTotalBytes)), so appending N members
 * into one flex a member at a time moves O(total bytes^2).  Once a
 * batch reaches megabytes that is the difference between a millisecond
 * and tens of seconds, on whatever thread does it.
 *
 * This builds bounded chunk flexes with the SAME flexPushBytes call
 * (numeral canonicalisation included) and folds them with ONE
 * exact-sized flexBulkMergeFlex.  The bytes are IDENTICAL to the direct
 * loop: a flex payload is the concatenation of self-describing entry
 * encodings that carry their own forward and backward length prefixes
 * and hold no cross-entry back-references, so no entry's encoding
 * depends on its position; only the header's totalBytes/count varints
 * are recomputed, to exactly the values the direct loop would leave.
 *
 * `*ff` is replaced by the grown flex (the previous one is freed).  Any
 * content it already held keeps its position ahead of the new members. */
typedef struct flexBytesMember {
    const void *data;
    size_t len;
} flexBytesMember;

void flexBulkPushBytesMembers(flex **ff, const flexBytesMember *members,
                              size_t count);

/* Insert to head or tail of flex.
 *
 * Two paths, chosen by the caller:
 *
 *   Auto-convert (flexPushBytes, flexPushByType, flexNewFromDatabox*,
 *   flexPushDataboxBig): bytes content that spells a numeral is stored as that
 *   number (databoxCanonical in databox.h): "22" is stored and read back as the
 *   integer 22.
 *
 *   Exact (the *Exact functions): stores exactly the value given.  Only the
 *   same-type minimal width applies (integer width, lossless double-to-float);
 *   BYTES stay BYTES, so "22" reads back as the 2-byte string "22".  The sorted
 *   and positional inserts (flexInsertByType and friends) are exact too.
 *   Anything that must round-trip its input, such as persistence, uses this
 *   path. */
void flexPushBytes(flex **ff, const void *data, size_t len, flexEndpoint where);
void flexPushBytesExact(flex **ff, const void *data, size_t len,
                        flexEndpoint where);
void flexPushByTypeExact(flex **ff, const databox *box, flexEndpoint where);
void flexPushSigned(flex **ff, int64_t i, flexEndpoint where);
void flexPushUnsigned(flex **ff, uint64_t u, flexEndpoint where);
void flexPushFloat16(flex **ff, float value, flexEndpoint where);
void flexPushFloatB16(flex **ff, float f, flexEndpoint where);
void flexPushFloat(flex **ff, float value, flexEndpoint where);
void flexPushDouble(flex **ff, double value, flexEndpoint where);
void flexPushByType(flex **ff, const databox *box, flexEndpoint where);

/* flex <-> databox single-element isomorphism.
 *
 * A flex and a databox carry the same type universe, so a flex holding
 * exactly ONE element is interchangeable with a single databox.  These two
 * helpers are the clean, validated wrapper/unwrapper for that case — use
 * them instead of hand-rolling flexNew()+flexPushByType() /
 * flexHead()+flexGetByType() (and instead of any ad-hoc [type][len][bytes]
 * framing) whenever you need to carry one value as a self-describing byte
 * buffer (a per-op delta, a single-value envelope, etc.).
 *
 * flexNewFromDatabox: allocate a fresh flex containing just `box`.  The
 * returned flex IS the byte buffer (length via flexBytes); free with
 * flexFree.  Returns NULL only on allocation failure.
 *
 * flexGetSingleDatabox: the defensive inverse for bytes from an untrusted
 * source (replication / on-disk recovery).  Validates the buffer envelope
 * and that it holds exactly one element, then extracts it into `*out`.
 * Returns false (leaving *out untouched) on any malformation, on an MDSC
 * (pointer) entry, and on a 96/128-bit integer, which a plain databox
 * cannot hold (use flexGetSingleDataboxBig for those).  For BYTES the
 * extracted box points INTO `buf`, so it is valid only while `buf`
 * lives; copy via databox helpers if it must outlive the buffer. */
flex *flexNewFromDatabox(const databox *box);
bool flexGetSingleDatabox(const uint8_t *buf, size_t buflen, databox *out);

/* Defensive 2-element flex unwrap — the [index,value] / [start,count] /
 * [key,field] pair shape.  Validates the buffer + requires exactly two
 * elements, then extracts them into *a and *b.  Returns false, leaving
 * both boxes untouched, on any malformation, an MDSC (pointer) entry, or
 * a 96/128-bit integer (use flexGetTwoDataboxesBig for those).  (For
 * BYTES the boxes point into `buf`; copy if they must outlive it.) */
bool flexGetTwoDataboxes(const uint8_t *buf, size_t buflen, databox *a,
                         databox *b);

/* flexNewFromDataboxes: the multi-value member of the same factory family —
 * build a fresh flex containing `n` values (boxes[0..n), in order).  Use this
 * instead of open-coding flexNew()+a flexPushByType() loop everywhere a
 * container needs to externalise its members as a flex (snapshots, multi-
 * member deltas).  n==0 yields a valid empty flex.  Returns NULL only on
 * allocation failure; free with flexFree. */
flex *flexNewFromDataboxes(const databox *boxes, size_t n);

/* Same as flexNewFromDataboxes but over an array of databox POINTERS — the
 * convention multimap/persistOps use (const databox **).  Avoids copying the
 * boxes into a contiguous value array just to build the flex. */
flex *flexNewFromDataboxPtrs(const databox *const *boxes, size_t n);

/* The exact-path factories: the same as the three above, but each value is
 * stored exactly as given (flexPushByTypeExact). */
flex *flexNewFromDataboxExact(const databox *box);
flex *flexNewFromDataboxesExact(const databox *boxes, size_t n);
flex *flexNewFromDataboxPtrsExact(const databox *const *boxes, size_t n);

/* databoxBig arm of the factory — for values that may exceed 64 bits (numeric
 * quantum sets / intsetBig).  A plain databox cannot hold a 128-bit value
 * inline (its data.i128 is a pointer), and flexGetByType into a plain databox
 * reports only the type of a 128-bit element (see flexGetByType), so
 * 128-bit-capable encode/decode must go through a databoxBig (which carries
 * the 16-byte `extra` backing store).
 *
 * flexPushDataboxBig: append one numeric databoxBig (64- or 128-bit) to `*ff`.
 *   128-bit values are normalised through DATABOX_BIG_*_128 so the push never
 *   depends on the caller having set ->big.
 * flexNewFromDataboxBig: a fresh 1-element flex holding `b` (free with
 * flexFree). flexGetSingleDataboxBig: defensive inverse — validate + require
 * exactly one element + decode into `*out` with flexGetByTypeBig, so any
 * prior state of `*out` is ignored and every type, 96/128-bit included,
 * decodes fully.  Returns false, leaving `*out` untouched, on malformation
 * or an MDSC (pointer) entry.
 * flexGetTwoDataboxesBig: the same for the 2-element pair shape (see
 * flexGetTwoDataboxes), decoding both entries with flexGetByTypeBig.
 * Returns false, leaving both boxes untouched, on malformation or an MDSC
 * entry.  In both, a 96/128-bit value lives in the box's own `extra` and
 * BYTES point into `buf`. */
void flexPushDataboxBig(flex **ff, const databoxBig *b, flexEndpoint where);
flex *flexNewFromDataboxBig(const databoxBig *b);
void flexPushDataboxBigExact(flex **ff, const databoxBig *b,
                             flexEndpoint where);
flex *flexNewFromDataboxBigExact(const databoxBig *b);
bool flexGetSingleDataboxBig(const uint8_t *buf, size_t buflen,
                             databoxBig *out);
bool flexGetTwoDataboxesBig(const uint8_t *buf, size_t buflen, databoxBig *a,
                            databoxBig *b);

/* Self-test for the flex<->databox factory (round-trip + defensive unwrap).
 * Returns the number of failed checks (0 == all passed). */
int flexFactorySelfTest(void);

/* Insert at existing position 'fe'.  flexInsertBytes canonicalizes a numeral
 * exactly as flexPushBytes does; flexInsertByType stores BYTES verbatim. */
void flexInsertBytes(flex **ff, flexEntry *fe, const void *data,
                     const size_t len);
void flexInsertSigned(flex **ff, flexEntry *fe, int64_t i);
void flexInsertUnsigned(flex **ff, flexEntry *fe, uint64_t u);
void flexInsertFloat16(flex **ff, flexEntry *fe, float value);
void flexInsertFloatB16(flex **ff, flexEntry *fe, float value);
void flexInsertFloat(flex **ff, flexEntry *fe, float value);
void flexInsertDouble(flex **ff, flexEntry *fe, double dvalue);
void flexInsertByType(flex **ff, flexEntry *fe, const databox *box);
bool flexInsertByTypeSortedWithMiddle(flex **ff, const databox *box,
                                      flexEntry **middleEntry);
flexEntry *flexFindByTypeSortedWithMiddleGetEntry(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox *compareAgainst, const flexEntry *middleFE);
bool flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
    flex **ff, uint_fast32_t elementsPerEntry, const databox *box[],
    flexEntry **middleEntry, bool compareUsingKeyElementOnly);
/* Replacing variants of the two inserts named without the suffix: same
 * behavior, and a key-match replace also reports the values it overwrote
 * through 'replacedOut' ('elementsPerEntry - 1' slots in tuple order after
 * the key; every slot is VOID when nothing was replaced).  'replacedOut'
 * may alias the value boxes in 'box' (new values in, old values out).
 * Only inline payloads (integers up to 64 bits, reals, pointers,
 * references) stay valid.  A BYTES or container slot reports where the
 * old entry was, and the replace rewrites those bytes in place even when
 * the flex is not reallocated, so such a slot must not be read; a caller
 * that needs an old variable-length value reads it with flexGetByTypeCopy
 * before the insert.  A 96/128-bit integer does not fit a databox, so its
 * slot reports only the type (SIGNED_128 or UNSIGNED_128) with a NULL
 * payload pointer; read such a value before the insert if it is needed. */
bool flexInsertReplaceByTypeSortedWithMiddleMultiDirectReplacing(
    flex **ff, uint_fast32_t elementsPerEntry, const databox *box[],
    flexEntry **middleEntry, bool compareUsingKeyElementOnly,
    databox *replacedOut);
bool flexInsertReplaceByTypeSortedWithMiddleMultiDirectLongKeysBecomePointersReplacing(
    flex **ff, uint_fast32_t elementsPerEntry, const databox *box[],
    flexEntry **middleEntry, bool compareUsingKeyElementOnly,
    void **recoveredPointer, databox *replacedOut);
/* TRUE SET inserts: full-width compare AND dedupe — a found
 * full-width-equal entry is a no-op returning true.  The Direct call
 * above with compareUsingKeyElementOnly=false is the
 * duplicates-ALLOWED sorted bag; mapIsSet multimaps must use these. */
bool flexInsertReplaceByTypeSortedWithMiddleMultiDirectSet(
    flex **ff, uint_fast32_t elementsPerEntry, const databox *box[],
    flexEntry **middleEntry);
/* Insert-if-absent in one search: key-only compare, and a found key leaves
 * the flex (and '*middleEntry') unchanged.  Returns true when the key was
 * present, false when the row was inserted. */
bool flexInsertByTypeSortedWithMiddleMultiDirectIfAbsent(
    flex **ff, uint_fast32_t elementsPerEntry, const databox *box[],
    flexEntry **middleEntry);
bool flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceSet(
    flex **ff, uint_fast32_t elementsPerEntry, const databox **box,
    flexEntry **middleEntry, const struct multimapAtom *referenceContainer);
bool flexInsertReplaceByTypeSortedWithMiddleMultiDirectLongKeysBecomePointers(
    flex **ff, const uint_fast32_t elementsPerEntry, const databox **box,
    flexEntry **middleEntry, bool compareUsingKeyElementOnly,
    void **recoveredPointer);
bool flexInsertReplaceByTypeSortedWithMiddleMultiWithReference(
    flex **ff, uint_fast32_t elementsPerEntry, const databox **box,
    flexEntry **const middleEntry, bool compareUsingKeyElementOnly,
    const struct multimapAtom *referenceContainer);
bool flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKey(
    flex **ff, const uint_fast32_t elementsPerEntry, const databox **box,
    const databox *boxInsertKey, flexEntry **middleEntry,
    bool compareUsingKeyElementOnly,
    const struct multimapAtom *const referenceContainer);
/* Runs once an insert-if-absent knows its key is absent, before the new
 * entry goes in; it must not change the flex being inserted into. */
typedef void(flexWhenAbsent)(void *ctx);

/* Insert-if-absent for a map of atom references inserting under a
 * surrogate key, in one search: true when the key (searched by value
 * through 'referenceContainer') was present, with '*foundEntry' its entry
 * and the flex unchanged; false when 'whenAbsent' (may be NULL) ran and
 * 'boxInsertKey' was inserted. */
bool flexInsertByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKeyIfAbsent(
    flex **ff, uint_fast32_t elementsPerEntry, const databox **box,
    const databox *boxInsertKey, flexEntry **middleEntry,
    const struct multimapAtom *referenceContainer, flexEntry **foundEntry,
    flexWhenAbsent *whenAbsent, void *whenAbsentCtx);
bool flexInsertByTypeSortedWithMiddleMultiDirect(flex **ff,
                                                 uint_fast32_t elementsPerEntry,
                                                 const databox *box[],
                                                 flexEntry **middleEntry);
void flexResizeEntry(flex **ff, flexEntry *fe, size_t newLenForEntry);

/* Append the 'elementsPerEntry' values of 'box' to the tail in one
 * reallocation (the auto form: one per 32 values); a NULL value is stored
 * as null, as the pushes store it.  The two forms
 * name their conversion as the single-value pushes do:
 *   - flexAppendMultiple converts like flexPushByType: a string that
 *     spells a number is stored as that number (the smallest encoding,
 *     for a reader that converts back to its own schema);
 *   - flexAppendMultipleExact stores every value as given, like
 *     flexPushByTypeExact (persisted and wire formats, keys whose type
 *     must survive).
 * Each writes exactly the bytes its single-value push writes, value by
 * value, so a row may be built either way. */
void flexAppendMultiple(flex **ff, uint_fast32_t elementsPerEntry,
                        const databox *const *box);
void flexAppendMultipleExact(flex **ff, uint_fast32_t elementsPerEntry,
                             const databox *const *box);

/* A row of up to FLEX_ROW_MAX values gathered for one append, for code
 * that writes a record field by field (snapshots, wire headers, tuples):
 *
 *     flexRow row = FLEX_ROW_INIT;
 *     flexRowUnsigned(&row, id);
 *     flexRowBytes(&row, name, nameLen);
 *     flexRowAppendExact(&f, &row);   (or flexRowAppend: auto conversion)
 *
 * The append writes the row in one reallocation, with the conversion its
 * name gives (see flexAppendMultiple), and leaves the row empty for the
 * next record.  The row copies each box; a BYTES or 96/128-bit value
 * still points at the caller's storage, which must live until the
 * append. */
#define FLEX_ROW_MAX 32
typedef struct flexRow {
    databox box[FLEX_ROW_MAX];
    uint32_t n;
} flexRow;
#define FLEX_ROW_INIT ((flexRow){.n = 0})

static inline void flexRowAdd(flexRow *r, const databox *box) {
    assert(r->n < FLEX_ROW_MAX);
    r->box[r->n++] = *box;
}

static inline void flexRowUnsigned(flexRow *r, const uint64_t v) {
    const databox box = databoxNewUnsigned(v);
    flexRowAdd(r, &box);
}

static inline void flexRowSigned(flexRow *r, const int64_t v) {
    const databox box = databoxNewSigned(v);
    flexRowAdd(r, &box);
}

static inline void flexRowBytes(flexRow *r, const void *data, size_t len) {
    const databox box = databoxNewBytes(data, len);
    flexRowAdd(r, &box);
}

void flexRowAppend(flex **ff, flexRow *r);
void flexRowAppendExact(flex **ff, flexRow *r);

flexEntry *flexFindByTypeSortedWithMiddleFullWidthWithReference(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox **const compareAgainst, const flexEntry *middleFE,
    const struct multimapAtom *referenceContainer);

flexEntry *flexFindByTypeSortedWithMiddleWithReference(
    const flex *const f, const uint_fast32_t elementsPerEntry,
    const databox *compareAgainst, const flexEntry *middleFE,
    const struct multimapAtom *referenceContainer);

/* The position of 'compareAgainst' (a value, not a reference) in a sorted
 * flex whose rows may hold references into 'referenceContainer': its row
 * when present, else the row it would precede (the end past the last).
 * The Find form above returns NULL on a miss instead. */
flexEntry *flexGetByTypeSortedWithMiddleWithReference(
    const flex *f, uint_fast32_t elementsPerEntry,
    const databox *compareAgainst, const flexEntry *middleFE,
    const struct multimapAtom *referenceContainer);

/* Insert 'box' into a sorted set (one element per entry) whose entries may
 * be references into 'referenceContainer': a reference is placed by the
 * value it resolves to and stored as the reference; any other box is
 * placed and stored as itself. */
void flexInsertByTypeSortedWithReference(
    flex **ff, const databox *box,
    const struct multimapAtom *referenceContainer);

/* Compare entire map entries */
int flexCompareEntries(const flex *f, const databox *const *elements,
                       uint_fast32_t elementsPerEntry, int_fast32_t offset);

/* Get pointer to entry */
flexEntry *flexIndexDirect(const flex *f, int32_t index);
flexEntry *flexIndex(const flex *f, int32_t index);
/* Anchored index: walks from the nearest of head, a caller-held anchor
 * entry at 'anchorElemIdx' (either direction), or tail.  Same index
 * convention and NULL-out-of-range contract as flexIndex().
 * flexIndexWithMiddle specializes the anchor to a MAINTAINED sorted-map
 * middle (multimap tiers cache one). */
flexEntry *flexIndexFromAnchor(const flex *f, int32_t index,
                               const flexEntry *anchorFE,
                               int32_t anchorElemIdx);
flexEntry *flexIndexWithMiddle(const flex *f, uint_fast32_t elementsPerEntry,
                               int32_t index, const flexEntry *middle);
flexEntry *flexNext(const flex *f, flexEntry *fe);
flexEntry *flexPrev(const flex *f, flexEntry *fe);

bool flexEntryIsValid(const flex *f, flexEntry *fe);

/* Quick endpoint retrieval */
flexEntry DK_FN_PURE *flexHead(const flex *f);
/* flexHead for ITERATION: NULL when the flex holds no entries.
 *
 * flexHead returns an insertion POSITION and is deliberately non-NULL even
 * for an empty flex — where it equals the end pointer.  A
 * `fe = flexHead(f); while (fe) { read(fe); fe = flexNext(f, fe); }` loop
 * therefore reads one entry PAST the buffer on an empty flex before
 * flexNext ever gets to apply its end check.  Use this as the loop
 * initializer instead: it applies the same end check flexNext does, so the
 * loop is correct at both ends and empty is simply zero iterations. */
flexEntry DK_FN_PURE *flexHeadIfAny(const flex *f);
flexEntry DK_FN_PURE *flexTail(const flex *f);
flexEntry DK_FN_PURE *flexTailWithElements(const flex *const f,
                                           uint_fast32_t elementsPerEntry);
#define flexHeadOrTail(f, endpoint)                                            \
    ((endpoint) == FLEX_ENDPOINT_TAIL ? flexTail(f) : flexHead(f))
#define flexHeadOrIndex(f, endpoint)                                           \
    ((endpoint) == FLEX_ENDPOINT_HEAD ? flexHead(f) : flexIndex(f, endpoint))

/* Retrieve data
 *
 * flexGetByType decodes the entry at 'fe' into 'box' without copying:
 *   - BYTES and container boxes point into the flex, so they are valid
 *     only until the flex changes.
 *   - A nested container (DATABOX_CONTAINER_FLEX_*) reports 'len' as the
 *     nested flex's bytes plus the reverse-traversal trailer stored after
 *     it, so 'len' is larger than flexBytes(data.bytes.start).  Use
 *     flexBytes for the nested flex's size.  Inserting or pushing the box
 *     back sizes the entry from the nested flex and ignores 'len', so a
 *     box read here round-trips.
 *   - A 96/128-bit integer is written through data.i128 / data.u128 only
 *     when 'box' is a databoxBig set up with DATABOX_BIG_INIT.  Into a
 *     plain databox (big == 0) only the type is reported (SIGNED_128 or
 *     UNSIGNED_128) and data.ptr is set to NULL.  A box's 'big' flag must
 *     therefore be initialized: zero the box, or use DATABOX_BIG_INIT.
 *     The owner of the destination sets it up (see databox.h); when the
 *     destination is yours, prefer flexGetByTypeBig.
 *   - "96/128-bit integer" means the value, not only the encoding: a
 *     64-bit negative entry whose stored magnitude exceeds INT64_MAX
 *     holds a value below INT64_MIN and decodes as SIGNED_128 too.
 *   - Only type, data and (for variable-length types) len are written;
 *     the flag bits ('allocated', 'big') are left as they were.
 *   - An MDSC entry reports DATABOX_PTR_MDSC with the stored pointer.
 *
 * flexGetByTypeInto decodes like flexGetByType, except that a 96/128-bit
 * value goes to 'slot' rather than wherever 'box' points: data.i128 /
 * data.u128 is aimed at 'slot', with big = 1 and allocated = 0, so the box
 * borrows the slot and the value is valid for as long as the slot is.
 * With a NULL slot the value is reported deferred: type only,
 * data.ptr = NULL, big = 0, allocated = 0 (see databoxIsWideDeferred).
 * It never reads the box's prior state.  Every other value writes only
 * type, data and (for variable-length types) len, exactly as
 * flexGetByType, and the flag bits are left as they were, so don't hand
 * it a box whose consumer reads stale 'allocated' or 'big' bits.  This is
 * the decode for temporaries and iterator entries: a narrow value never
 * touches the slot, so a stack or struct-tail databoxWide costs nothing
 * until a wide value arrives.  flexGetByTypeIntoWithReference also
 * resolves a reference entry through 'referenceContainer', and a wide
 * resolved value lands in 'slot' the same way.
 *
 * flexGetByTypeBig decodes into a databoxBig and owns its wide capability:
 * it never reads the box's prior state, so uninitialized or poisoned
 * storage is fine.  On return allocated = 0, and big = 1 exactly when the
 * value is 96/128-bit, with data.i128 / data.u128 aimed at box->extra.
 * It is flexGetByTypeInto with 'extra' as the slot, after clearing the
 * header.
 * Any other value has big = 0, so its leading 16 bytes can be copied out
 * as a plain databox.  BYTES and containers borrow from the flex as with
 * flexGetByType, and a wide value borrows from 'box' itself: copy the box
 * with databoxCopy (owned) or databoxBigCopyFrom (re-homed) to keep it
 * past either.  flexGetNextByTypeBig is flexGetNextByType decoding with
 * flexGetByTypeBig.
 *
 * flexGetByTypeCopy produces a self-contained value that never borrows
 * from the flex: BYTES and containers are copied to the heap
 * (allocated = 1).  A 96/128-bit value is written into the storage 'box'
 * already points at when 'box->big' is set (a DATABOX_BIG_INIT'd
 * databoxBig, or a box that already owns a wide heap payload), leaving
 * 'allocated' as it was; otherwise it goes to a 16-byte heap block with
 * allocated = 1 and big = 1.  Every other value has allocated = 0 and
 * big = 0.  Release the result with databoxFreeData.  As with
 * flexGetByType, 'big' must be initialized. */
void flexGetByType(const flexEntry *fe, databox *box);
void flexGetByTypeWithReference(const flexEntry *const fe, databox *box,
                                const struct multimapAtom *referenceContainer);
void flexGetByTypeInto(const flexEntry *fe, databox *box, databoxWide *slot);
void flexGetByTypeIntoWithReference(
    const flexEntry *fe, databox *box, databoxWide *slot,
    const struct multimapAtom *referenceContainer);
void flexGetByTypeCopy(const flexEntry *const fe, databox *box);
bool flexGetNextByType(flex *f, flexEntry **fe, databox *box);
void flexGetByTypeBig(const flexEntry *fe, databoxBig *box);
bool flexGetNextByTypeBig(flex *f, flexEntry **fe, databoxBig *box);

/* Retrieve specific data.  Each returns false when the value does not fit
 * the result type, including any 96/128-bit value. */
bool flexGetSigned(flexEntry *fe, int64_t *value);
bool flexGetUnsigned(flexEntry *fe, uint64_t *value);

/* Replace.  flexReplaceSigned/Unsigned and flexIncrby* take 64-bit
 * integer entries (flexIncrbySigned also booleans) and return false for
 * any other entry, 96/128-bit values included.  A new value that fits
 * the entry's width is written in place, and the entry's sign follows
 * the new value. */
void flexReplaceByType(flex **ff, flexEntry *fe, const databox *box);
void flexReplaceBytes(flex **ff, flexEntry *fe, const void *s,
                      const size_t len);
bool flexReplaceSigned(flex **ff, flexEntry *fe, int64_t value);
bool flexReplaceUnsigned(flex **ff, flexEntry *fe, uint64_t value);
/* Add 'incrby' to the integer at 'fe' (the platform increment policy, see
 * DATABOX.md "Numeric updates"): the sum of the stored value (whichever sign
 * it was stored with) and 'incrby' is exact, and a result outside the call's
 * range (int64 for Signed, uint64 for Unsigned) is refused, returning false
 * with the value unchanged; there is no wrap and no saturation.  So
 * incrementing by zero returns true with the value unchanged when the value
 * is in that range.  On success '*newval' (if non-NULL) receives the new
 * value.  Also returns false for an entry that is not a 64-bit integer. */
bool flexIncrbySigned(flex **ff, flexEntry *fe, int64_t incrby,
                      int64_t *newval);
bool flexIncrbyUnsigned(flex **ff, flexEntry *fe, int64_t incrby,
                        uint64_t *newval);

/* Compares */
bool flexCompareBytes(flex *fe, const void *data, size_t len);
bool flexCompareString(flexEntry *fe, const void *str, size_t len);
bool flexCompareUnsigned(flexEntry *fe, uint64_t val);
bool flexCompareSigned(flexEntry *fe, int64_t val);

/* Finding (head to tail) */
/* The first entry from the head equal to the bytes 'vstr' of length 'vlen',
 * comparing one entry and then skipping 'skip' entries (skip 1 visits every
 * other entry, e.g. the keys of a key/value flex).  A string entry matches
 * exactly the same bytes, and a numeric entry (96/128-bit included) matches
 * when 'vstr' is a numeral whose canonical number (databoxCanonical) has the
 * same value, so "22" finds a stored 22 and 22.0, and a raw stored "22".
 * flexFind is the read-side partner of the auto-convert push path
 * (flexPushBytes stores "22" as 22).  Returns NULL when nothing matches. */
flexEntry *flexFind(flex *f, const void *vstr, uint32_t vlen, uint32_t skip);

/* Typed finds, starting at 'fe' and skipping 'skip' entries after each
 * comparison; NULL when nothing matches.  They match by kind and value and
 * never across kinds:
 *   flexFindSigned / flexFindUnsigned: a 64-bit integer entry of the same
 *     mathematical value (-1 never matches a stored UINT64_MAX).
 *   flexFindString: a string entry with exactly these bytes, the empty
 *     string included.
 *   flexFindByType: by the box's type: bytes match bytes as above;
 *     integers, reals and 96/128-bit integers match any numeric entry of
 *     equal value (databoxCompare, so a NaN finds a NaN); TRUE, FALSE and
 *     NULL match only themselves.  Other types (containers, pointers) match
 *     nothing. */
flexEntry *flexFindSigned(const flex *f, flexEntry *fe, int64_t val,
                          uint32_t skip);
flexEntry *flexFindUnsigned(const flex *f, flexEntry *fe, uint64_t val,
                            uint32_t skip);
flexEntry *flexFindString(const flex *f, flexEntry *fe, const void *val,
                          const size_t len, uint32_t skip);
flexEntry *flexFindByType(flex *f, flexEntry *fe, const databox *box,
                          uint32_t skip);
flexEntry *flexFindByTypeSorted(const flex *f, uint_fast32_t elementsPerEntry,
                                const databox *compareAgainst);
flexEntry *flexFindByTypeSortedFullWidth(const flex *f,
                                         uint_fast32_t elementsPerEntry,
                                         const databox **compareAgainst);
flexEntry *flexGetByTypeSortedWithMiddle(const flex *f,
                                         uint_fast32_t elementsPerEntry,
                                         const databox *compareAgainst,
                                         const flexEntry *middleFE);
flexEntry *flexFindByTypeSortedWithMiddle(const flex *f,
                                          uint_fast32_t elementsPerEntry,
                                          const databox *compareAgainst,
                                          const flexEntry *middleFE);
flexEntry *flexFindByTypeSortedWithMiddleFullWidth(
    const flex *f, uint_fast32_t elementsPerEntry,
    const databox **compareAgainst, const flexEntry *middleFE);
/* flexFindByTypeSortedWithMiddle, returning the FIRST row whose key
 * matches when several rows share it (a set's rows), so the row chosen
 * does not depend on where the search happened to land */
flexEntry *flexFindByTypeSortedWithMiddleLowest(const flex *f,
                                                uint_fast32_t elementsPerEntry,
                                                const databox *compareAgainst,
                                                const flexEntry *middleFE);
flexEntry *flexFindByTypeSortedWithMiddleLowestWithReference(
    const flex *f, uint_fast32_t elementsPerEntry,
    const databox *compareAgainst, const flexEntry *middleFE,
    const struct multimapAtom *referenceContainer);
flexEntry *flexFindByTypeHead(flex *f, const databox *box, uint32_t skip);

/* Finding (tail to head) */
flexEntry *flexFindSignedReverse(const flex *f, flexEntry *fe, int64_t val,
                                 uint32_t skip);
flexEntry *flexFindUnsignedReverse(const flex *f, flexEntry *fe, uint64_t val,
                                   uint32_t skip);
flexEntry *flexFindStringReverse(const flex *f, flexEntry *fe, const void *val,
                                 const size_t len, uint32_t skip);
flexEntry *flexFindByTypeReverse(flex *f, flexEntry *fe, const databox *box,
                                 uint32_t skip);

/* Metadata */
bool DK_FN_PURE flexIsEmpty(const flex *f);
size_t DK_FN_PURE flexCount(const flex *f);
size_t DK_FN_PURE flexBytes(const flex *f);
/* The width in bytes of the header's total-bytes field (it widens as the
 * flex grows) */
size_t DK_FN_PURE flexBytesLength(const flex *f);

/* Verify metadata and physical entries are equal in both flexes. */
bool flexEqual(const flex *a, const flex *b);

/* Deleting */
void flexDelete(flex **ff, flexEntry **fe);
void flexDeleteNoUpdateEntry(flex **ff, flexEntry *fe);
void flexDeleteDrain(flex **ff, flexEntry **fe);
void flexDeleteCount(flex **ff, flexEntry **fe, uint32_t count);
void flexDeleteOffsetCount(flex **ff, int32_t offset, uint32_t count);
void flexDeleteRange(flex **ff, int32_t index, uint32_t count);
void flexDeleteUpToInclusive(flex **ff, flexEntry *fe);
void flexDeleteUpToInclusivePlusN(flex **ff, flexEntry *fe,
                                  const int32_t nMore);
void flexDeleteSortedValueWithMiddle(flex **ff, uint_fast32_t elementsPerEntry,
                                     flexEntry *fe, flexEntry **middleEntry);
flex *flexSplitRange(flex **ff, int32_t index, uint32_t num);
flex *flexSplitMiddle(flex **ff, uint_fast32_t elementsPerEntry,
                      const flexEntry *middleEntry);
flex *flexSplit(flex **ff, uint_fast32_t elementsPerEntry);
#define flexDeleteHead(ff) flexDeleteNoUpdateEntry(ff, flexHead(*(ff)))
#define flexDeleteTail(ff) flexDeleteNoUpdateEntry(ff, flexTail(*(ff)))

/* Draining a flex (doesn't shrink allocation for each delete) */
void flexDeleteCountDrain(flex **ff, flexEntry **fe, uint32_t count);
void flexDeleteOffsetCountDrain(flex **ff, int32_t offset, uint32_t count);
void flexDeleteRangeDrain(flex **ff, int32_t index, uint32_t num);

/* Fold every entry of a flex: the sum, the first entry minus the rest, or
 * the product (an empty flex gives 0, 0 and 1).  Under the numeric policy
 * (DATABOX.md "Numeric Updates"): the Signed / Unsigned folds take integer
 * entries of any width whose values fit int64 / uint64 and compute
 * exactly; Float / Double take any numbers (integers converted) and
 * compute in double (Float narrows the result).  Each returns false,
 * leaving '*result' unset, for an entry of another kind, an integer
 * entry or partial result outside the range, or a result that is not
 * finite. */
bool flexAddSigned(const flex *f, int64_t *result);
bool flexAddUnsigned(const flex *f, uint64_t *result);
bool flexSubtractSigned(const flex *f, int64_t *result);
bool flexSubtractUnsigned(const flex *f, uint64_t *result);
bool flexMultiplySigned(const flex *f, int64_t *result);
bool flexMultiplyUnsigned(const flex *f, uint64_t *result);
bool flexAddFloat(const flex *f, float *result);
bool flexSubtractFloat(const flex *f, float *result);
bool flexMultiplyFloat(const flex *f, float *result);
bool flexAddDouble(const flex *f, double *result);
bool flexSubtractDouble(const flex *f, double *result);
bool flexMultiplyDouble(const flex *f, double *result);

/* CFlex */
size_t cflexBytesCompressed(const cflex *c);
size_t cflexBytes(const cflex *c);
cflex *cflexDuplicate(const cflex *c);
bool flexConvertToCFlex(const flex *f, cflex *cBuffer, size_t cBufferLen);
bool cflexConvertToFlex(const cflex *c, flex **fBuffer, size_t *fBufferLen);

#ifdef DATAKIT_TEST
void flexRepr(const flex *f);
int32_t flexTest(int32_t argc, char *argv[]);

/* Run every single-byte mutation of several valid flexes through the
 * validators.  Returns the number of failures.  flexTest runs it; other
 * suites may call it too. */
int32_t flexValidateTotalitySweep(void);
#endif

__END_DECLS
