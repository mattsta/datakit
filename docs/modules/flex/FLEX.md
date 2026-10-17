# flex - Compact Variable-Length Encoded Array

## Overview

`flex` is a **memory-efficient, pointer-free, variable-length encoded array** that serves as a universal sequential container for storing mixed-type data with minimal overhead. It automatically compresses data using optimal encodings and supports bidirectional traversal, sorted operations, and in-place modifications.

**Key Features:**

- **Pointer-free design**: Single contiguous memory allocation with no internal pointers
- **Variable-length encoding**: Automatic selection of minimal storage size for each element
- **Multi-type storage**: Seamlessly stores integers, floats, strings, booleans, and nested containers
- **Compact representation**: 2-byte overhead for empty flex, minimal per-entry overhead
- **Bidirectional traversal**: Efficient forward and backward iteration
- **Sorted operations**: Binary search with optional middle-entry hint for O(log n) performance
- **Zero-copy when possible**: Direct data access without intermediate allocations
- **Compression support**: Optional compression (cflex) for large flexes

**Header**: `flex.h`

**Source**: `flex.c`

## Data Structure

### Physical Layout

A flex consists of a header followed by zero or more entries:

```
┌──────────┬──────────┬─────────┬─────────┬─────────┐
│  BYTES   │  COUNT   │ ENTRY 1 │ ENTRY 2 │   ...   │
├──────────┼──────────┼─────────┼─────────┼─────────┤
│ 1-9 bytes│ 1-9 bytes│Variable │Variable │Variable │
│ (varint) │ (varint) │ length  │ length  │ length  │
└──────────┴──────────┴─────────┴─────────┴─────────┘
```

- **BYTES**: Split varint encoding of total flex size (1-9 bytes)
- **COUNT**: Tagged varint encoding of element count (1-9 bytes)
- **ENTRY**: Variable-length encoded elements

**The header width is not fixed.** Both fields are varints, so the header
grows and shrinks as the flex crosses a varint width boundary — and because
the BYTES field counts _itself_, widening it can push the total across the
next boundary, so the width is resolved by iterating until it stops
changing. Two consequences:

1. `flexBytes(f)` is the only source of truth for the total size; never
   assume a constant header offset.
2. Every insert and delete goes through the header-resize path, and that
   path reallocates to the **exact** new size. flex has no capacity
   concept — see [Bulk Operations](#bulk-operations) before appending in a
   loop.

### Entry Layout

Each entry has a symmetric forward/reverse encoding structure for bidirectional traversal:

```
Regular Entry:
┌──────────┬──────────┬──────────┐
│ ENCODING │   DATA   │ ENCODING │
│ (forward)│          │ (reverse)│
└──────────┴──────────┴──────────┘

Immediate Value Entry (true/false/null):
┌──────────┐
│ ENCODING │
│ (1 byte) │
└──────────┘
```

- **Forward encoding**: Type and length information (1-9 bytes)
- **Data**: The actual element data (0-N bytes)
- **Reverse encoding**: Same as forward, but reversed for backward traversal

### Type System

`flexType` is private to `flex.c` — it describes the physical in-memory and
on-disk format, so any addition, removal, or reordering is a format change.
The single encoding byte carries the type; string lengths ride in the same
byte space as a split varint, which is why the non-string types start where
that varint's tag space runs out.

```c
typedef enum flexType {
    /* Strings occupy 0-200: the encoding byte IS the leading byte of a
     * split varint holding the byte length.  FLEX_VARINT_SPLIT_MAX (200)
     * is the last such value, so FLEX_IS_STR(enc) is (enc <= 200). */
    FLEX_VARINT_SPLIT_MAX = 200,

    /* 20 integer types (201-220): NEG/UINT pairs, two apart */
    FLEX_NEG_8B = 201,    /* Negative, 1 byte of magnitude */
    FLEX_UINT_8B,         /* 202 - Unsigned, 1 byte */
    FLEX_NEG_16B,         /* 203 */
    FLEX_UINT_16B,        /* 204 */
    FLEX_NEG_24B,         /* 205 - 24-bit saves space over 32-bit */
    FLEX_UINT_24B,        /* 206 */
    FLEX_NEG_32B,         /* 207 */
    FLEX_UINT_32B,        /* 208 */
    FLEX_NEG_40B,         /* 209 */
    FLEX_UINT_40B,        /* 210 */
    FLEX_NEG_48B,         /* 211 */
    FLEX_UINT_48B,        /* 212 */
    FLEX_NEG_56B,         /* 213 */
    FLEX_UINT_56B,        /* 214 */
    FLEX_NEG_64B,         /* 215 */
    FLEX_UINT_64B,        /* 216 - end of FLEX_IS_INTEGER */
    /* no 72/80/88-bit encodings */
    FLEX_NEG_96B,         /* 217 - start of FLEX_IS_INTEGER_BIG */
    FLEX_UINT_96B,        /* 218 */
    /* no 104/112/120-bit encodings */
    FLEX_NEG_128B,        /* 219 */
    FLEX_UINT_128B,       /* 220 */

    /* 6 float types (221-226) */
    FLEX_REAL_B16B,       /* 221 - bfloat16 (truncated float32) */
    FLEX_REAL_16B,        /* 222 - IEEE 754-2008 binary16 */
    FLEX_REAL_32B,        /* 223 - IEEE 754-2008 binary32 */
    FLEX_REAL_64B,        /* 224 - IEEE 754-2008 binary64 */
    FLEX_REAL_32D,        /* 225 - decimal32 (reserved, unimplemented) */
    FLEX_REAL_64D,        /* 226 - decimal64 (reserved, unimplemented) */

    /* 2 mdsc pointer types (227-228) */
    FLEX_EXTERNAL_MDSC_48B,  /* 227 - pointer with top 16 bits dropped */
    FLEX_EXTERNAL_MDSC_64B,  /* 228 - full 64-bit pointer */

    /* 8 logical reference types (229-236), 8..64-bit reference IDs */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_8,   /* 229 */
    /* ... _16, _24, _32, _40, _48, _56 ... */
    FLEX_CONTAINER_REFERENCE_EXTERNAL_64,  /* 236 */

    /* 8 container types (237-244) — order must match databox's
     * DATABOX_CONTAINER_FLEX_* / _CFLEX_* order */
    FLEX_CONTAINER_MAP,    /* 237 */
    FLEX_CONTAINER_LIST,   /* 238 */
    FLEX_CONTAINER_SET,    /* 239 */
    FLEX_CONTAINER_TUPLE,  /* 240 */
    FLEX_CONTAINER_CMAP,   /* 241 - compressed variants */
    FLEX_CONTAINER_CLIST,  /* 242 */
    FLEX_CONTAINER_CSET,   /* 243 */
    FLEX_CONTAINER_CTUPLE, /* 244 */

    /* 245-250 unassigned */

    /* Immediate (single-byte) types: FLEX_IS_IMMEDIATE(enc) is enc >= 251 */
    FLEX_SAME = 251,         /* "same as previous entry" marker; UNUSED */
    FLEX_BYTES_EMPTY = 252,  /* zero-length byte array */
    FLEX_TRUE = 253,         /* Boolean true */
    FLEX_FALSE = 254,        /* Boolean false */
    FLEX_NULL = 255          /* NULL value */
} flexType;
```

Notes that matter when reading flex bytes by hand:

- A **non-negative** value always gets a `FLEX_UINT_*` encoding, even when
  pushed through `flexPushSigned`. The `FLEX_NEG_*` encodings are used only
  for values below zero, and they store the magnitude with a `-1` bias
  (there is no signed zero to encode).
- Integer encodings are chosen by _bytes actually used_, so `42` lands in
  `FLEX_UINT_8B` and `-42` in `FLEX_NEG_8B`.
- `FLEX_IS_INTEGER` covers 201-216 only; the 96/128-bit encodings are a
  separate `FLEX_IS_INTEGER_BIG` class and require a `databoxBig`.
- A `FLEX_NEG_*` 64-bit-or-narrower encoding decodes into an `int64_t`, so
  it holds values down to `INT64_MIN` only. A value below `INT64_MIN` is
  stored as `FLEX_NEG_96B` or wider, even when its biased magnitude would
  fit 64 unsigned bits, and reads back as `DATABOX_SIGNED_128`.
- Immediate encodings occupy **one** byte total: there is no trailing
  reverse encoding to skip, because there is no payload between them.

### Encoding Optimization

Flex automatically selects the most compact encoding:

| Data Type                      | Storage              | Example                 |
| ------------------------------ | -------------------- | ----------------------- |
| Strings, length 1..64          | 1 + N + 1 bytes      | "hello" → 7 bytes total |
| Strings, length 65..16447      | 2 + N + 2 bytes      |                         |
| Strings, length 16448..4210750 | 3 + N + 3 bytes      |                         |
| Strings, longer                | 5-9 + N + 5-9 bytes  |                         |
| Integers -256 to 255           | 1 + 1 + 1 = 3 bytes  | 42 → 3 bytes            |
| Integers -65536 to 65535       | 1 + 2 + 1 = 4 bytes  |                         |
| Integers, wider                | 1 + 3..8 + 1 bytes   | 24/40/48/56-bit widths  |
| Integers, 96- or 128-bit       | 1 + 12 or 16 + 1     | needs `databoxBig`      |
| float16 / bfloat16             | 1 + 2 + 1 = 4 bytes  | 0.578125 → 4 bytes      |
| float32                        | 1 + 4 + 1 = 6 bytes  |                         |
| float64                        | 1 + 8 + 1 = 10 bytes |                         |
| empty string / true/false/null | 1 byte               |                         |

Length prefixes above 4210750 bytes jump from 3 to 5 bytes: the split varint
has no 4-byte external form, so the next width up is `1 + 4`.

### Automatic Value Canonicalisation

`flexPushBytes` and `flexInsertBytes` run the byte string through
`StrScanScanReliable` first. A string that parses cleanly as a number is
stored in its **numeric** encoding, not as bytes — so pushing `"42"` and
reading it back yields `DATABOX_UNSIGNED_64`, not `DATABOX_BYTES`:

```c
flexPushBytes(&f, "42", 2, FLEX_ENDPOINT_TAIL);

databox box = {{0}};
flexGetByType(flexHead(f), &box);
assert(DATABOX_IS_INTEGER(&box));   /* NOT DATABOX_IS_BYTES */
```

This is a storage win for numeral-heavy data, and it is why round-tripping a
flex is value-preserving rather than byte-preserving.

`flexPushByType` routes `DATABOX_BYTES` / `DATABOX_BYTES_EMBED` through
`flexInsertBytes`, so **endpoint pushes canonicalise too**. The positional
`flexInsertByType` does not: it stores byte boxes verbatim. If you need
digit strings preserved as bytes, insert at a position rather than pushing,
or prefix/wrap the value so it no longer parses as a number.

`flexPushByType` also stores `DATABOX_PTR` as an unsigned integer, which is
why a pointer read back out of a flex arrives as `DATABOX_UNSIGNED_64` — see
`databoxNewPtrAsUnsigned` in [databox](../core/DATABOX.md).

Floats narrow the same way: `flexPushDouble` stores `FLEX_REAL_64B` only
when the value does not round-trip exactly through `float`, and then narrows
further to `FLEX_REAL_16B` or `FLEX_REAL_B16B` when those are exact. Use
`flexPushFloat16` / `flexPushFloatB16` to force a 16-bit encoding regardless
of truncation.

## Basic Types

```c
typedef uint8_t flex;         /* Flex itself (pointer to first byte) */
typedef uint8_t flexEntry;    /* Entry pointer (within flex) */
typedef uint8_t cflex;        /* Compressed flex */

/* Endpoints for push operations */
typedef enum flexEndpoint {
    FLEX_ENDPOINT_TAIL = -1,  /* Insert at end */
    FLEX_ENDPOINT_HEAD = 0    /* Insert at beginning */
} flexEndpoint;

/* Empty flex is just 2 bytes! */
#define FLEX_EMPTY_SIZE 2
```

## Creation and Destruction

### Basic Lifecycle

```c
/* Create a new empty flex */
flex *flexNew(void);

/* Duplicate an existing flex */
flex *flexDuplicate(const flex *f);

/* Reset flex to empty state (preserves allocation) */
void flexReset(flex **ff);

/* Free a flex */
void flexFree(flex *f);

/* Example usage */
flex *f = flexNew();
/* ... use flex ... */
flexFree(f);
```

`flexReset` zeroes the buffer and rewrites a 2-byte header, but does not
shrink the allocation — pair it with the `*Drain` deletes.

### Validating Untrusted Bytes

Iteration (`flexCount` / `flexHead` / `flexNext` / `flexGetByType`) trusts
the header's self-described size and will walk past the end if that header
lies. Gate any buffer that came from outside this process — replication, an
on-disk snapshot, IPC, a plugin blob, a fuzzer:

```c
/* O(1): validates the outer envelope only.  True iff 'buf' parses as a
 * well-formed header whose self-described total-bytes == buflen.
 * Reads at most 5 bytes.  Sufficient when the bytes are MAC-gated. */
bool flexValidateBuffer(const uint8_t *buf, size_t buflen);

/* O(N): walks every entry's length encoding, confirms each entry fits,
 * confirms the header's count matches the entries actually present, and
 * confirms the walk lands exactly on the claimed end (no trailing garbage,
 * no truncation).  Use when the bytes are NOT MAC-gated.  Does not decode
 * payloads. */
bool flexValidateBufferDeep(const uint8_t *buf, size_t buflen);

/* O(1): read the leading total-bytes claim WITHOUT requiring
 * buflen == claim — for a flex that is a strict PREFIX of a larger buffer
 * (e.g. a flex-encoded frame header followed by opaque payload bytes).
 * On success *out_total is the prefix length the flex claims; feed that to
 * flexValidateBuffer and treat buf + *out_total as the post-flex region. */
bool flexPeekTotalBytes(const uint8_t *buf, size_t buflen, size_t *out_total);
```

`flexPeekTotalBytes` returns false on NULL inputs, a `buflen` below the
minimum envelope, a leading-varint width exceeding `buflen`, or a claim
larger than `buflen`.

### flex <-> databox Factory

A flex and a databox carry the same type universe, so a one-element flex is
interchangeable with a single databox. These are the validated
wrapper/unwrapper for that case — use them instead of hand-rolling
`flexNew()` + `flexPushByType()` / `flexHead()` + `flexGetByType()`, and
instead of any ad-hoc `[type][len][bytes]` framing, whenever a value has to
travel as a self-describing byte buffer:

```c
/* Build */
flex *flexNewFromDatabox(const databox *box);
flex *flexNewFromDataboxes(const databox *boxes, size_t n);      /* n == 0 ok */
flex *flexNewFromDataboxPtrs(const databox *const *boxes, size_t n);
flex *flexNewFromDataboxBig(const databoxBig *b);

/* Defensive unwrap — validates the envelope AND the element count */
bool flexGetSingleDatabox(const uint8_t *buf, size_t buflen, databox *out);
bool flexGetTwoDataboxes(const uint8_t *buf, size_t buflen,
                         databox *a, databox *b);
bool flexGetSingleDataboxBig(const uint8_t *buf, size_t buflen,
                             databoxBig *out);

/* 64- or 128-bit numeric append; 128-bit values are normalised through
 * the DATABOX_BIG_*_128 macros so the push never depends on the caller
 * having set ->big */
void flexPushDataboxBig(flex **ff, const databoxBig *b, flexEndpoint where);

/* Round-trip + defensive-unwrap self test; returns the failure count */
int flexFactorySelfTest(void);
```

The returned flex **is** the byte buffer (length via `flexBytes`); free it
with `flexFree`. The unwrap calls return false and leave their outputs
untouched on any malformation. For `DATABOX_BYTES` the extracted box points
into `buf`, so it is valid only while `buf` lives.

`flexNewFromDataboxPtrs` takes the `const databox **` convention that
multimap and the persist ops use, avoiding a copy into a contiguous array
just to build the flex.

### Bulk Operations

```c
typedef struct flexBytesMember {
    const void *data;
    size_t len;
} flexBytesMember;

void flexBulkPushBytesMembers(flex **ff, const flexBytesMember *members,
                              size_t count);
```

See [Allocation Behavior](#allocation-behavior) for why this exists.

### Working Example

```c
/* Create and populate a flex with mixed types */
flex *f = flexNew();

/* Push various types */
flexPushSigned(&f, -42, FLEX_ENDPOINT_TAIL);
flexPushBytes(&f, "hello", 5, FLEX_ENDPOINT_TAIL);
flexPushDouble(&f, 3.14159, FLEX_ENDPOINT_TAIL);

const databox t = databoxBool(true);
flexPushByType(&f, &t, FLEX_ENDPOINT_TAIL);

/* Flex now contains: [-42, "hello", 3.14159, true] */

flexFree(f);
```

## Push Operations (Insert at Endpoints)

### Type-Specific Push

```c
/* Push bytes (string or binary data) */
void flexPushBytes(flex **ff, const void *s, size_t len, flexEndpoint where);

/* Push signed integer */
void flexPushSigned(flex **ff, int64_t i, flexEndpoint where);

/* Push unsigned integer */
void flexPushUnsigned(flex **ff, uint64_t u, flexEndpoint where);

/* Push float (auto-selects float16, bfloat16, or float32) */
void flexPushFloat(flex **ff, float f, flexEndpoint where);

/* Push float specifically as IEEE float16 */
void flexPushFloat16(flex **ff, float f, flexEndpoint where);

/* Push float specifically as bfloat16 */
void flexPushFloatB16(flex **ff, float f, flexEndpoint where);

/* Push double */
void flexPushDouble(flex **ff, double d, flexEndpoint where);

/* Push any databox type (polymorphic) */
void flexPushByType(flex **ff, const databox *box, flexEndpoint where);
```

### Push Examples

```c
flex *f = flexNew();

/* Push to tail (append) */
flexPushBytes(&f, "apple", 5, FLEX_ENDPOINT_TAIL);
flexPushSigned(&f, 100, FLEX_ENDPOINT_TAIL);
/* f: ["apple", 100] */

/* Push to head (prepend) */
flexPushBytes(&f, "first", 5, FLEX_ENDPOINT_HEAD);
/* f: ["first", "apple", 100] */

/* Push using databox */
databox box = databoxNewReal(2.718);
flexPushByType(&f, &box, FLEX_ENDPOINT_TAIL);
/* f: ["first", "apple", 100, 2.718] */

flexFree(f);
```

### Bulk Append

```c
/* Append multiple databox elements at once */
void flexAppendMultiple(flex **ff, const uint_fast32_t elementsPerEntry,
                        const databox **const box);

/* Example: Create a record with multiple fields */
flex *f = flexNew();

databox id = databoxNewUnsigned(12345);
databox name = databoxNewBytesString("Alice");
databox score = databoxNewReal(98.5);
databox active = databoxBool(true);

const databox *fields[] = {&id, &name, &score, &active};
flexAppendMultiple(&f, 4, fields);

/* f now contains all 4 fields in order */
flexFree(f);
```

## Insert Operations (Insert at Position)

### Insert at Specific Entry

```c
/* Insert bytes at position fe */
void flexInsertBytes(flex **ff, flexEntry *fe, const void *s, const size_t len);

/* Insert signed integer */
void flexInsertSigned(flex **ff, flexEntry *fe, int64_t i);

/* Insert unsigned integer */
void flexInsertUnsigned(flex **ff, flexEntry *fe, uint64_t u);

/* Insert float (auto-encoded: float16 -> bfloat16 -> float32) */
void flexInsertFloat(flex **ff, flexEntry *fe, float f);

/* Insert double (narrows to a float encoding when exact) */
void flexInsertDouble(flex **ff, flexEntry *fe, double d);

/* Insert any databox type */
void flexInsertByType(flex **ff, flexEntry *fe, const databox *box);
```

To force a 16-bit float encoding at a position, insert through a databox or
use the endpoint pushes `flexPushFloat16` / `flexPushFloatB16`.

`flexInsertBytes` runs the same numeral canonicalisation as `flexPushBytes`
(see [Automatic Value Canonicalisation](#automatic-value-canonicalisation)).

### Insert Examples

```c
flex *f = flexNew();
flexPushBytes(&f, "first", 5, FLEX_ENDPOINT_TAIL);
flexPushBytes(&f, "third", 5, FLEX_ENDPOINT_TAIL);
/* f: ["first", "third"] */

/* Insert in the middle */
flexEntry *fe = flexIndex(f, 1);  /* Get position of "third" */
flexInsertBytes(&f, fe, "second", 6);
/* f: ["first", "second", "third"] */

flexFree(f);
```

### Sorted Insert (Binary Search)

```c
/* Insert one element, keeping the flex sorted.
 * 'middleEntry' is an in/out hint the caller owns and reuses. */
bool flexInsertByTypeSortedWithMiddle(flex **ff, const databox *box,
                                      flexEntry **middleEntry);

/* Insert a multi-element entry (e.g. a key-value pair): duplicates ALLOWED */
bool flexInsertByTypeSortedWithMiddleMultiDirect(
    flex **ff, uint_fast32_t elementsPerEntry, const databox *box[],
    flexEntry **middleEntry);

/* Insert, or replace the existing entry.
 * compareUsingKeyElementOnly = true  -> match on element 0 only (map)
 * compareUsingKeyElementOnly = false -> match on the full entry width
 *                                       (a sorted BAG: duplicates allowed) */
bool flexInsertReplaceByTypeSortedWithMiddleMultiDirect(
    flex **ff, uint_fast32_t elementsPerEntry, const databox *box[],
    flexEntry **middleEntry, bool compareUsingKeyElementOnly);

/* TRUE SET insert: full-width compare AND dedupe.  A full-width-equal
 * entry is a no-op returning true.  Set-semantics containers must use
 * these, NOT the Direct call above with compareUsingKeyElementOnly=false. */
bool flexInsertReplaceByTypeSortedWithMiddleMultiDirectSet(
    flex **ff, uint_fast32_t elementsPerEntry, const databox *box[],
    flexEntry **middleEntry);
bool flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceSet(
    flex **ff, uint_fast32_t elementsPerEntry, const databox **box,
    flexEntry **middleEntry, const struct multimapAtom *referenceContainer);

/* Reference-container arms (see "With multimapAtom" below) */
bool flexInsertReplaceByTypeSortedWithMiddleMultiWithReference(
    flex **ff, uint_fast32_t elementsPerEntry, const databox **box,
    flexEntry **const middleEntry, bool compareUsingKeyElementOnly,
    const struct multimapAtom *referenceContainer);
bool flexInsertReplaceByTypeSortedWithMiddleMultiWithReferenceWithSurrogateKey(
    flex **ff, uint_fast32_t elementsPerEntry, const databox **box,
    const databox *boxInsertKey, flexEntry **middleEntry,
    bool compareUsingKeyElementOnly,
    const struct multimapAtom *referenceContainer);

/* Long keys are promoted to pointers; the recovered pointer is returned */
bool flexInsertReplaceByTypeSortedWithMiddleMultiDirectLongKeysBecomePointers(
    flex **ff, uint_fast32_t elementsPerEntry, const databox **box,
    flexEntry **middleEntry, bool compareUsingKeyElementOnly,
    void **recoveredPointer);
```

There is no `flexInsertByTypeSorted()` without a middle hint — the hint
parameter is part of every sorted-insert entry point. Pass a `flexEntry *`
initialized to `NULL` and let the calls maintain it.

### Sorted Insert Examples

```c
/* Example 1: Sorted list of integers */
flex *f = flexNew();
flexEntry *middle = NULL;

for (int i = 0; i < 100; i++) {
    int value = rand() % 1000;
    databox box = databoxNewSigned(value);
    flexInsertByTypeSortedWithMiddle(&f, &box, &middle);
}
/* f is now sorted, and 'middle' tracks the middle element */

/* Example 2: Sorted key-value map */
flex *fmap = flexNew();
flexEntry *mid = NULL;

const databox key1 = databoxNewBytesString("apple");
const databox val1 = databoxNewSigned(100);
const databox *pair1[2] = {&key1, &val1};
flexInsertByTypeSortedWithMiddleMultiDirect(&fmap, 2, pair1, &mid);

const databox key2 = databoxNewBytesString("banana");
const databox val2 = databoxNewSigned(200);
const databox *pair2[2] = {&key2, &val2};
flexInsertByTypeSortedWithMiddleMultiDirect(&fmap, 2, pair2, &mid);

/* fmap: [("apple", 100), ("banana", 200)] - sorted by key */

flexFree(fmap);
flexFree(f);
```

## Accessing Elements

### Indexing

```c
/* Get entry at index (supports negative indexing) */
flexEntry *flexIndex(const flex *f, int32_t index);

/* Get entry at index (direct, faster) */
flexEntry *flexIndexDirect(const flex *f, int32_t index);

/* Get head (first) entry — an INSERTION POSITION, never NULL */
flexEntry *flexHead(const flex *f);

/* Get head for ITERATION — NULL when the flex holds no entries */
flexEntry *flexHeadIfAny(const flex *f);

/* Get tail (last) entry */
flexEntry *flexTail(const flex *f);

/* Get tail of multi-element entries (e.g., last key-value pair) */
flexEntry *flexTailWithElements(const flex *const f,
                                uint_fast32_t elementsPerEntry);

/* Check if entry pointer is valid */
bool flexEntryIsValid(const flex *f, flexEntry *fe);
```

### Indexing Examples

```c
flex *f = flexNew();
for (int i = 0; i < 10; i++) {
    flexPushSigned(&f, i, FLEX_ENDPOINT_TAIL);
}
/* f: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9] */

/* Positive indexing */
flexEntry *fe0 = flexIndex(f, 0);   /* Points to 0 */
flexEntry *fe3 = flexIndex(f, 3);   /* Points to 3 */

/* Negative indexing (from end) */
flexEntry *feLast = flexIndex(f, -1);   /* Points to 9 */
flexEntry *fe2nd = flexIndex(f, -2);    /* Points to 8 */

/* Out of range returns NULL */
flexEntry *feInvalid = flexIndex(f, 100);  /* NULL */

/* Quick endpoint access */
flexEntry *head = flexHead(f);  /* Same as flexIndex(f, 0) */
flexEntry *tail = flexTail(f);  /* Same as flexIndex(f, -1) */

flexFree(f);
```

**`flexHead` vs `flexHeadIfAny`.** `flexHead` returns an insertion position
and is deliberately non-NULL even for an empty flex, where it equals the end
pointer. A loop written as

```c
fe = flexHead(f);
while (fe) { read(fe); fe = flexNext(f, fe); }   /* WRONG */
```

therefore reads one entry past the buffer on an empty flex, before
`flexNext` ever gets to apply its end check. Use `flexHeadIfAny` as the loop
initializer — it applies the same end check, so the loop is correct at both
ends and empty is simply zero iterations.

`flexEntryIsValid(f, fe)` is a bounds test (`f <= fe < f + flexBytes(f)`),
which is why `while (flexEntryIsValid(f, fe))` also terminates correctly:
the end pointer fails it. It does **not** verify that `fe` lands on an entry
boundary.

### Navigation

```c
/* Get next entry */
flexEntry *flexNext(const flex *f, flexEntry *fe);

/* Get previous entry */
flexEntry *flexPrev(const flex *f, flexEntry *fe);
```

### Navigation Examples

```c
flex *f = createList();  /* ["hello", "foo", "quux", 1024] */

/* Forward iteration */
flexEntry *fe = flexHead(f);
while (fe && flexEntryIsValid(f, fe)) {
    databox box = {{0}};
    flexGetByType(fe, &box);
    databoxRepr(&box);
    fe = flexNext(f, fe);
}

/* Backward iteration */
fe = flexTail(f);
while (fe && flexEntryIsValid(f, fe)) {
    databox box = {{0}};
    flexGetByType(fe, &box);
    databoxRepr(&box);
    fe = flexPrev(f, fe);
}

flexFree(f);
```

## Retrieving Data

### Get Functions

```c
/* Get data from entry into databox */
void flexGetByType(const flexEntry *fe, databox *outbox);

/* Get data with external reference resolution */
void flexGetByTypeWithReference(const flexEntry *const fe, databox *box,
                                const struct multimapAtom *referenceContainer);

/* Get data as a copy (allocated) */
void flexGetByTypeCopy(const flexEntry *const fe, databox *box);

/* Get and advance to next entry */
bool flexGetNextByType(flex *f, flexEntry **fe, databox *box);

/* Get signed integer value */
bool flexGetSigned(flexEntry *fe, int64_t *value);

/* Get unsigned integer value */
bool flexGetUnsigned(flexEntry *fe, uint64_t *value);
```

### Retrieval Examples

```c
flex *f = flexNew();
flexPushSigned(&f, 42, FLEX_ENDPOINT_TAIL);
flexPushBytes(&f, "world", 5, FLEX_ENDPOINT_TAIL);
flexPushDouble(&f, 3.14, FLEX_ENDPOINT_TAIL);

/* Get first element */
flexEntry *fe = flexHead(f);
databox box = {{0}};
flexGetByType(fe, &box);
assert(DATABOX_IS_SIGNED_INTEGER(&box));
assert(box.data.i64 == 42);

/* Iterate and get all elements */
fe = flexHeadIfAny(f);
while (fe && flexGetNextByType(f, &fe, &box)) {
    if (DATABOX_IS_INTEGER(&box)) {
        printf("Integer: %" PRId64 "\n", box.data.i64);
    } else if (DATABOX_IS_BYTES(&box)) {
        printf("String: %.*s\n", (int)box.len,
               databoxBytes(&box));
    } else if (DATABOX_IS_FLOAT(&box)) {
        printf("Float: %f\n", (box.type == DATABOX_FLOAT_32)
                                  ? (double)box.data.f32
                                  : box.data.d64);
    }
}

flexFree(f);
```

`flexGetByType` fills `type`, `data`, and `len` but leaves the databox flag
bits alone — declare the box as `databox box = {{0}};` so `allocated` is
false. The bytes it hands back point **into the flex buffer**, so they are
valid only until the next flex mutation. Use `flexGetByTypeCopy` when the
value must outlive the flex, and release it with `databoxFreeData`.

A 128-bit element cannot be decoded into a plain `databox` (its `data.i128`
is a pointer with nothing behind it) and will assert. Use
`flexGetSingleDataboxBig` or decode into a `DATABOX_BIG_INIT`'d
`databoxBig`.

## Finding Elements

### Linear Search

```c
/* Find bytes in flex (from head) */
flexEntry *flexFind(flex *f, const void *vstr, uint32_t vlen, uint32_t skip);

/* Find signed integer */
flexEntry *flexFindSigned(const flex *f, flexEntry *fe, int64_t sval,
                          uint32_t skip);

/* Find unsigned integer */
flexEntry *flexFindUnsigned(const flex *f, flexEntry *fe, uint64_t sval,
                            uint32_t skip);

/* Find string */
flexEntry *flexFindString(const flex *f, flexEntry *fe, const void *sval,
                          const size_t slen, uint32_t skip);

/* Find by databox type */
flexEntry *flexFindByType(flex *f, flexEntry *fe, const databox *box,
                          uint32_t skip);

/* Find from head specifically */
flexEntry *flexFindByTypeHead(flex *f, const databox *box, uint32_t skip);
```

### Reverse Search

```c
/* Find signed integer (tail to head) */
flexEntry *flexFindSignedReverse(const flex *f, flexEntry *fe, int64_t sval,
                                 uint32_t skip);

/* Find unsigned integer (tail to head) */
flexEntry *flexFindUnsignedReverse(const flex *f, flexEntry *fe, uint64_t sval,
                                   uint32_t skip);

/* Find string (tail to head) */
flexEntry *flexFindStringReverse(const flex *f, flexEntry *fe, const void *sval,
                                 const size_t slen, uint32_t skip);

/* Find by type (tail to head) */
flexEntry *flexFindByTypeReverse(flex *f, flexEntry *fe, const databox *box,
                                 uint32_t skip);
```

### Binary Search (Sorted Flexes)

```c
/* Binary search in sorted flex, comparing the KEY element of each entry */
flexEntry *flexFindByTypeSorted(const flex *f, uint_fast32_t elementsPerEntry,
                                const databox *compareAgainst);

/* Binary search comparing the FULL entry width */
flexEntry *flexFindByTypeSortedFullWidth(const flex *f,
                                         uint_fast32_t elementsPerEntry,
                                         const databox **compareAgainst);

/* Same, with a middle hint */
flexEntry *flexFindByTypeSortedWithMiddle(const flex *f,
                                          uint_fast32_t elementsPerEntry,
                                          const databox *compareAgainst,
                                          const flexEntry *middleFE);
flexEntry *flexFindByTypeSortedWithMiddleFullWidth(
    const flex *f, uint_fast32_t elementsPerEntry,
    const databox **compareAgainst, const flexEntry *middleFE);

/* Returns the ENTRY START even when the match is on a later element */
flexEntry *flexFindByTypeSortedWithMiddleGetEntry(
    const flex *f, uint_fast32_t elementsPerEntry,
    const databox *compareAgainst, const flexEntry *middleFE);

/* Locate the insertion POSITION for a value (not just an exact match) */
flexEntry *flexGetByTypeSortedWithMiddle(const flex *f,
                                         uint_fast32_t elementsPerEntry,
                                         const databox *compareAgainst,
                                         const flexEntry *middleFE);

/* Binary search resolving entries stored as multimapAtom references */
flexEntry *flexFindByTypeSortedWithMiddleWithReference(
    const flex *f, uint_fast32_t elementsPerEntry,
    const databox *compareAgainst, const flexEntry *middleFE,
    const struct multimapAtom *referenceContainer);
flexEntry *flexFindByTypeSortedWithMiddleFullWidthWithReference(
    const flex *f, uint_fast32_t elementsPerEntry,
    const databox **compareAgainst, const flexEntry *middleFE,
    const struct multimapAtom *referenceContainer);
```

`elementsPerEntry` is how many flex elements make up one logical entry: `1`
for a plain sorted list, `2` for key-value pairs, `N` for wider records. It
is also the stride the binary search uses, so passing the wrong value
searches the wrong positions rather than merely returning "not found".

### Search Examples

```c
/* Linear search */
flex *f = flexNew();
flexPushBytes(&f, "apple", 5, FLEX_ENDPOINT_TAIL);
flexPushBytes(&f, "banana", 6, FLEX_ENDPOINT_TAIL);
flexPushBytes(&f, "cherry", 6, FLEX_ENDPOINT_TAIL);

databox search = databoxNewBytesString("banana");
flexEntry *found = flexFindByType(f, flexHead(f), &search, 0);
if (found) {
    printf("Found banana!\n");
}

/* Binary search in sorted flex */
flex *sorted = flexNew();
flexEntry *middle = NULL;

for (int i = 0; i < 100; i++) {
    databox box = databoxNewSigned(i * 10);
    flexInsertByTypeSortedWithMiddle(&sorted, &box, &middle);
}

/* Now search for a value */
databox searchFor = databoxNewSigned(420);
flexEntry *result = flexFindByTypeSortedWithMiddle(sorted, 1, &searchFor, middle);
if (result) {
    printf("Found 420 in sorted flex!\n");
}

flexFree(sorted);
flexFree(f);
```

## Comparing Elements

```c
/* Compare entry with bytes */
bool flexCompareBytes(flex *fe, const void *s, size_t slen);

/* Compare entry with string */
bool flexCompareString(flexEntry *fe, const void *sstr, size_t slen);

/* Compare entry with unsigned integer */
bool flexCompareUnsigned(flexEntry *fe, uint64_t sval);

/* Compare entry with signed integer */
bool flexCompareSigned(flexEntry *fe, int64_t sval);

/* Compare entire multi-element entries */
int flexCompareEntries(const flex *f, const databox *const *elements,
                       uint_fast32_t elementsPerEntry, int_fast32_t offset);

/* Compare two flexes for equality */
bool flexEqual(const flex *a, const flex *b);
```

### Comparison Examples

```c
flex *f = flexNew();
flexPushBytes(&f, "hello", 5, FLEX_ENDPOINT_TAIL);
flexPushSigned(&f, 1024, FLEX_ENDPOINT_TAIL);

flexEntry *fe = flexHead(f);
if (flexCompareBytes(fe, "hello", 5)) {
    printf("First element is 'hello'\n");
}

fe = flexNext(f, fe);
if (flexCompareSigned(fe, 1024)) {
    printf("Second element is 1024\n");
}

flexFree(f);
```

## Replacing Elements

```c
/* Replace entry with databox */
void flexReplaceByType(flex **ff, flexEntry *fe, const databox *box);

/* Replace entry with bytes */
void flexReplaceBytes(flex **ff, flexEntry *fe, const void *s,
                      const size_t slen);

/* Replace with signed integer */
bool flexReplaceSigned(flex **ff, flexEntry *fe, int64_t value);

/* Replace with unsigned integer */
bool flexReplaceUnsigned(flex **ff, flexEntry *fe, uint64_t value);

/* Increment signed integer in place */
bool flexIncrbySigned(flex **ff, flexEntry *fe, int64_t incrby,
                      int64_t *newval);

/* Increment unsigned integer in place */
bool flexIncrbyUnsigned(flex **ff, flexEntry *fe, int64_t incrby,
                        uint64_t *newval);

/* Resize entry to new length (advanced) */
void flexResizeEntry(flex **ff, flexEntry *fe, size_t newLenForEntry);
```

### Replace Examples

```c
flex *f = flexNew();
flexPushBytes(&f, "old", 3, FLEX_ENDPOINT_TAIL);
flexPushSigned(&f, 10, FLEX_ENDPOINT_TAIL);

/* Replace string */
flexEntry *fe = flexHead(f);
flexReplaceBytes(&f, fe, "new", 3);
/* f: ["new", 10] */

/* Replace and increment integer */
fe = flexTail(f);
int64_t newValue;
flexIncrbySigned(&f, fe, 5, &newValue);
assert(newValue == 15);
/* f: ["new", 15] */

flexFree(f);
```

## Deleting Elements

### Single Delete

```c
/* Delete entry (updates fe to next valid entry) */
void flexDelete(flex **ff, flexEntry **fe);

/* Delete entry without updating pointer */
void flexDeleteNoUpdateEntry(flex **ff, flexEntry *fe);

/* Delete entry for draining (no realloc) */
void flexDeleteDrain(flex **ff, flexEntry **fe);

/* Quick macros */
#define flexDeleteHead(ff) flexDeleteNoUpdateEntry(ff, flexHead(*(ff)))
#define flexDeleteTail(ff) flexDeleteNoUpdateEntry(ff, flexTail(*(ff)))
```

### Range Delete

```c
/* Delete count entries starting at fe */
void flexDeleteCount(flex **ff, flexEntry **fe, uint32_t count);

/* Delete count entries at offset */
void flexDeleteOffsetCount(flex **ff, int32_t offset, uint32_t count);

/* Delete num entries starting at index */
void flexDeleteRange(flex **ff, int32_t index, uint32_t num);

/* Delete from start up to and including fe */
void flexDeleteUpToInclusive(flex **ff, flexEntry *fe);

/* Delete up to fe plus N more entries */
void flexDeleteUpToInclusivePlusN(flex **ff, flexEntry *fe,
                                  const int32_t nMore);

/* Delete from sorted flex with middle tracking */
void flexDeleteSortedValueWithMiddle(flex **ff, uint_fast32_t elementsPerEntry,
                                     flexEntry *fe, flexEntry **middleEntry);
```

### Drain Variants (No Realloc)

For bulk deletions, use drain variants to avoid reallocating on each delete:

```c
void flexDeleteCountDrain(flex **ff, flexEntry **fe, uint32_t count);
void flexDeleteOffsetCountDrain(flex **ff, int32_t offset, uint32_t count);
void flexDeleteRangeDrain(flex **ff, int32_t index, uint32_t num);
```

### Delete Examples

```c
/* Example 1: Delete while iterating */
flex *f = flexNew();
flexPushBytes(&f, "hello", 5, FLEX_ENDPOINT_TAIL);
flexPushBytes(&f, "foo", 3, FLEX_ENDPOINT_TAIL);
flexPushBytes(&f, "bar", 3, FLEX_ENDPOINT_TAIL);

flexEntry *fe = flexHead(f);
while (flexEntryIsValid(f, fe)) {
    databox box = {{0}};
    flexGetByType(fe, &box);

    if (DATABOX_IS_BYTES(&box) &&
        memcmp(databoxBytes(&box), "foo", 3) == 0) {
        flexDelete(&f, &fe);  /* Deletes and advances fe */
    } else {
        fe = flexNext(f, fe);
    }
}
/* f: ["hello", "bar"] */

/* Example 2: Delete range */
flexDeleteRange(&f, 0, 2);  /* Delete first 2 elements */
/* f: [] (empty) */

flexFree(f);

/* Example 3: Efficient bulk delete with drain */
f = flexNew();
for (int i = 0; i < 1000; i++) {
    flexPushSigned(&f, i, FLEX_ENDPOINT_TAIL);
}

/* Delete all from tail without reallocating each time */
while (flexCount(f) > 0) {
    flexDeleteOffsetCountDrain(&f, -1, 1);
}
/* Now shrink to final size */
flexReset(&f);

flexFree(f);
```

## Merging and Splitting

### Merging

```c
/* Merge two flexes in place.  Returns the merged flex, or NULL if either
 * argument is NULL or both point at the SAME flex. */
flex *flexMerge(flex **first, flex **second);

/* Append one flex's entries to another */
void flexBulkAppendFlex(flex **ff, const flex *zzb);

/* Fold an array of flexes into one new flex */
flex *flexBulkMergeFlex(const flex *const *const fs, const size_t count);
```

`flexMerge` reuses the **larger** of the two buffers rather than allocating
a third. On return exactly one of `*first` / `*second` holds the merged flex
(the same pointer the function returns) and the other has been freed and set
to `NULL`. Which one survives depends on the input sizes, so always free
through the returned pointer and never assume `*first` was the survivor.

### Splitting

```c
/* Split flex at index, return second half */
flex *flexSplitRange(flex **ff, int32_t index, uint32_t num);

/* Split sorted flex at middle */
flex *flexSplitMiddle(flex **ff, uint_fast32_t elementsPerEntry,
                      const flexEntry *middleEntry);

/* Split sorted flex in half */
flex *flexSplit(flex **ff, uint_fast32_t elementsPerEntry);
```

### Merge/Split Examples

```c
/* Merging */
flex *f1 = flexNew();
flexPushBytes(&f1, "hello", 5, FLEX_ENDPOINT_TAIL);
flexPushSigned(&f1, 100, FLEX_ENDPOINT_TAIL);

flex *f2 = flexNew();
flexPushBytes(&f2, "world", 5, FLEX_ENDPOINT_TAIL);
flexPushSigned(&f2, 200, FLEX_ENDPOINT_TAIL);

/* Merge f1 and f2: one buffer is reused, the other is freed and NULLed */
flex *merged = flexMerge(&f1, &f2);
/* merged: ["hello", 100, "world", 200] */
/* Exactly one of f1 / f2 now equals 'merged'; the other is NULL.
 * Do not free f1 or f2 — free 'merged'. */

/* Splitting */
flex *secondHalf = flexSplitRange(&merged, 2, 2);
/* merged: ["hello", 100] */
/* secondHalf: ["world", 200] */

flexFree(merged);
flexFree(secondHalf);
```

## Metadata and Introspection

```c
/* Check if flex is empty */
bool flexIsEmpty(const flex *f);

/* Get count of elements */
size_t flexCount(const flex *f);

/* Get total bytes used by flex (header + all entries) */
size_t flexBytes(const flex *f);

/* Get the WIDTH of the total-bytes header varint (1-9) — NOT an alias
 * for flexBytes.  Use this only when parsing the header by hand. */
size_t flexBytesLength(const flex *f);

/* Get middle entry (for sorted operations) */
flexEntry *flexMiddle(const flex *f, uint_fast32_t elementsPerEntry);
```

### Metadata Examples

```c
flex *f = flexNew();

assert(flexIsEmpty(f));
assert(flexCount(f) == 0);
assert(flexBytes(f) == FLEX_EMPTY_SIZE);  /* 2 bytes */

flexPushSigned(&f, 42, FLEX_ENDPOINT_TAIL);
flexPushBytes(&f, "hello", 5, FLEX_ENDPOINT_TAIL);

assert(!flexIsEmpty(f));
assert(flexCount(f) == 2);
printf("Flex uses %zu bytes\n", flexBytes(f));

flexFree(f);
```

## Arithmetic Operations

For flexes containing all integers or all floats, perform aggregate operations:

```c
/* Signed integer operations */
int64_t flexAddSigned(const flex *f);
int64_t flexSubtractSigned(const flex *f);
int64_t flexMultiplySigned(const flex *f);

/* Unsigned integer operations */
uint64_t flexAddUnsigned(const flex *f);
uint64_t flexSubtractUnsigned(const flex *f);
uint64_t flexMultiplyUnsigned(const flex *f);

/* Float operations */
double flexAddFloat(const flex *f);
double flexSubtractFloat(const flex *f);
double flexMultiplyFloat(const flex *f);

/* Double operations */
double flexAddDouble(const flex *f);
double flexSubtractDouble(const flex *f);
double flexMultiplyDouble(const flex *f);
```

### Arithmetic Examples

```c
flex *numbers = flexNew();
for (int i = 1; i <= 10; i++) {
    flexPushSigned(&numbers, i, FLEX_ENDPOINT_TAIL);
}

/* Sum: 1 + 2 + 3 + ... + 10 = 55 */
int64_t sum = flexAddSigned(numbers);
assert(sum == 55);

/* Product: 1 * 2 * 3 * ... * 10 = 3628800 */
int64_t product = flexMultiplySigned(numbers);
assert(product == 3628800);

flexFree(numbers);
```

## Compressed Flex (CFlex)

For large flexes, compress to save memory:

A cflex is **not** an opaque blob: it begins with a verbatim copy of the
source flex's header (the total-bytes and count varints), then a third
varint holding the compressed payload length, then the LZ4 data. Because the
header survives, `flexCount()` and `flexBytes()` read a cflex directly and
report the _uncompressed_ count and size without decompressing.

```c
/* Length of the LZ4 payload alone */
size_t cflexBytesCompressed(const cflex *c);

/* TOTAL physical size of the cflex:
 * header + payload-length varint + payload */
size_t cflexBytes(const cflex *c);

/* Duplicate compressed flex */
cflex *cflexDuplicate(const cflex *c);

/* Convert flex to compressed form */
bool flexConvertToCFlex(const flex *f, cflex *cBuffer, size_t cBufferLen);

/* Convert compressed flex back to regular flex */
bool cflexConvertToFlex(const cflex *c, flex **fBuffer, size_t *fBufferLen);
```

`flexConvertToCFlex` returns false — and leaves `cBuffer` untouched — when
the flex is smaller than an internal 64-byte floor, when `cBufferLen` is
below that same floor, when the flex exceeds LZ4's single-shot maximum input
size, or when LZ4 itself cannot fit the result in the buffer provided. That
floor is private to `flex.c` and is not exported as a public constant; just
check the return value. The function never grows `cBuffer` for you.

For a managed, self-sizing wrapper around this, use
[mflex](MFLEX.md) rather than driving cflex buffers by hand.

### Compression Examples

```c
/* Create large flex */
flex *f = flexNew();
for (int i = 0; i < 100000; i++) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "entry-%d", i);
    flexPushBytes(&f, buf, len, FLEX_ENDPOINT_TAIL);
}

size_t originalSize = flexBytes(f);
printf("Original size: %zu bytes\n", originalSize);

void *compressBuffer = zmalloc(originalSize * 2);
cflex *c = (cflex *)compressBuffer;

/* Returns false when the input is too small or would not shrink */
if (flexConvertToCFlex(f, c, originalSize * 2)) {
    size_t compressedSize = cflexBytesCompressed(c);
    printf("Compressed to: %zu bytes (%.1f%% reduction)\n",
           compressedSize,
           100.0 * (1.0 - (double)compressedSize / originalSize));

    /* Decompress when needed */
    flex *restored = NULL;
    size_t restoredSize = originalSize;
    if (cflexConvertToFlex(c, &restored, &restoredSize)) {
        assert(flexEqual(f, restored));
        flexFree(restored);
    }
}

zfree(compressBuffer);
flexFree(f);
```

## Debugging and Visualization

```c
#ifdef DATAKIT_TEST
/* Print human-readable representation of flex */
void flexRepr(const flex *f);

/* Run flex test suite */
int32_t flexTest(int32_t argc, char *argv[]);
#endif
```

### Debug Output Example

```c
flex *f = flexNew();
flexPushBytes(&f, "hello", 5, FLEX_ENDPOINT_TAIL);
flexPushSigned(&f, 42, FLEX_ENDPOINT_TAIL);
flexPushDouble(&f, 3.14, FLEX_ENDPOINT_TAIL);

#ifdef DATAKIT_TEST
flexRepr(f);
#endif

flexFree(f);
```

`flexRepr` first prints the header — total bytes, element count, the width
of each header varint, the resulting header size, and the tail offset — then
one brace-delimited record per entry carrying its address, forward and
reverse index, byte offset within the flex, total entry size, metadata size
(the two encoding copies), and payload length, followed by the decoded
value. Immediate types render as `{{TRUE}}`, `{{FALSE}}`, `{{NULL}}`, or
`{{EMPTY}}`.

It is a debug dump, not a stable format: do not parse it.

## Performance Characteristics

| Operation                 | Complexity | Notes                                          |
| ------------------------- | ---------- | ---------------------------------------------- |
| **Creation**              | O(1)       | `zcalloc` of 2 bytes                           |
| **Push to tail**          | O(n)       | exact-size realloc; may copy the buffer        |
| **Push to head**          | O(n)       | realloc plus shifting all existing data        |
| **Bulk push (members)**   | O(n)       | chunked build + one exact-sized merge          |
| **Index access**          | O(n)       | sequential scan (from whichever end is nearer) |
| **Head/tail access**      | O(1)       | direct calculation from the header             |
| **Next/prev**             | O(1)       | length encoded on both sides of the entry      |
| **Insert at position**    | O(n)       | must shift data after the insert point         |
| **Delete at position**    | O(n)       | must shift data after the delete point         |
| **Find (linear)**         | O(n)       | full scan                                      |
| **Find (sorted)**         | O(log n)   | binary search                                  |
| **Find with middle hint** | O(log n)   | same order, roughly one level saved            |
| **Merge**                 | O(n + m)   | concatenation into one new allocation          |
| **Split**                 | O(n)       | copies the extracted range into a new flex     |

Note `flexIndex` halves the scan by converting a forward index past the
midpoint into a reverse traversal (and vice versa) — still O(n), but with a
better constant than a naive walk. `flexIndexDirect` skips that conversion.

### Performance Tips

1. **Carry middle hints**: for sorted flexes, keep one `flexEntry *` alive
   across operations rather than re-deriving it
2. **Batch deletes**: use the `*Drain` variants, then `flexReset` once
3. **Batch appends**: `flexBulkPushBytesMembers` / `flexBulkMergeFlex` — a
   manual `zrealloc` cannot pre-size a flex
4. **Prefer tail operations**: head pushes pay for shifting the whole buffer
   on top of the realloc
5. **Keep sorted**: binary search beats linear scan by orders of magnitude
   at container-sized element counts

## Memory Management

### Memory Layout

```
Empty flex (2 bytes):
[0x02][0x00]
  ^     ^
  |     └─ count = 0 (1 byte tagged varint)
  └─────── total bytes = 2 (1 byte split varint)

Small flex with one integer (5 bytes):
[0x05][0x01][0xCA][0x2A][0xCA]
  ^     ^     ^     ^     ^
  |     |     |     |     └─ reverse encoding (same byte, for prev walks)
  |     |     |     └─────── data (42)
  |     |     └───────────── forward encoding: 0xCA = 202 = FLEX_UINT_8B
  |     └─────────────────── count = 1
  └───────────────────────── total bytes = 5
```

42 is non-negative, so it takes the **UINT** encoding even though it was
pushed with `flexPushSigned`. `-42` would use `FLEX_NEG_8B` (0xC9 = 201) and
store the biased magnitude.

### Memory Efficiency

Flex is optimized for minimal memory usage:

- **No pointers**: Entire structure is relocatable
- **Variable encoding**: Only stores necessary bytes for each value
- **Shared metadata**: Header overhead amortized across elements
- **No padding**: Byte-aligned, no alignment waste

### Memory Examples

```c
/* Compare memory usage of different structures */

/* Array of pointers (traditional) */
void *ptrs[100];
size_t ptrArraySize = sizeof(ptrs);  /* 800 bytes (64-bit) */

/* Flex with same 100 integers */
flex *f = flexNew();
for (int i = 0; i < 100; i++) {
    flexPushSigned(&f, i, FLEX_ENDPOINT_TAIL);
}
size_t flexSize = flexBytes(f);
printf("Pointer array: %zu bytes\n", ptrArraySize);
printf("Flex: %zu bytes\n", flexSize);
/* Flex typically uses 300-400 bytes for 100 small integers */

flexFree(f);
```

### Allocation Behavior

flex allocates **exactly** what it needs and nothing more. `flexNew()`
`zcalloc`s two bytes; every subsequent insert or delete runs through the
header-resize path, which `zrealloc`s to the exact new total size.

There is no reserve, no slack, and no way to pre-size a flex: a manual
`zrealloc` to a larger buffer is discarded by the next mutation, which
reallocs back down to the exact size. Appending N members one at a time
therefore moves O(total bytes squared).

The two supported ways to build a large flex cheaply:

```c
/* Many byte members at the tail */
void flexBulkPushBytesMembers(flex **ff, const flexBytesMember *members,
                              size_t count);

/* Fold an array of already-built flexes into one */
flex *flexBulkMergeFlex(const flex *const *fs, size_t count);
```

`flexBulkPushBytesMembers` builds bounded chunk flexes with the same
`flexPushBytes` call (numeral canonicalisation included) and folds them with
one exact-sized merge. The resulting bytes are identical to the direct loop:
entry encodings carry their own forward and backward length prefixes and
hold no cross-entry back-references, so no entry's encoding depends on its
position — only the header varints are recomputed. `*ff` is replaced by the
grown flex and the previous one is freed; content it already held keeps its
position ahead of the new members.

The one place deferred reallocation exists is deletion: the `*Drain`
variants skip the shrink `zrealloc` so emptying a flex is not N reallocs.

## Real-World Examples

### Example 1: Simple List

```c
/* Create a shopping list */
flex *shoppingList = flexNew();

flexPushBytes(&shoppingList, "milk", 4, FLEX_ENDPOINT_TAIL);
flexPushBytes(&shoppingList, "eggs", 4, FLEX_ENDPOINT_TAIL);
flexPushBytes(&shoppingList, "bread", 5, FLEX_ENDPOINT_TAIL);

/* Print list */
flexEntry *fe = flexHead(shoppingList);
int index = 1;
while (flexEntryIsValid(shoppingList, fe)) {
    databox box = {{0}};
    flexGetByType(fe, &box);
    printf("%d. %.*s\n", index++, (int)box.len, databoxBytes(&box));
    fe = flexNext(shoppingList, fe);
}

flexFree(shoppingList);
```

### Example 2: Key-Value Map (Sorted)

```c
/* Create a sorted map of user scores */
flex *scores = flexNew();
flexEntry *middle = NULL;

/* Insert key-value pairs */
const char *users[] = {"alice", "bob", "charlie", "diana"};
int userScores[] = {95, 87, 92, 98};

for (int i = 0; i < 4; i++) {
    databox key = databoxNewBytesString(users[i]);
    databox val = databoxNewSigned(userScores[i]);
    const databox *pair[2] = {&key, &val};

    flexInsertByTypeSortedWithMiddleMultiDirect(&scores, 2, pair, &middle);
}

/* Look up score for "charlie" */
databox searchKey = databoxNewBytesString("charlie");
flexEntry *found = flexFindByTypeSortedWithMiddle(scores, 2, &searchKey, middle);

if (found) {
    databox keyBox = {{0}};
    databox valBox = {{0}};

    flexGetByType(found, &keyBox);
    flexEntry *valueEntry = flexNext(scores, found);
    flexGetByType(valueEntry, &valBox);

    printf("Score for charlie: %" PRId64 "\n", valBox.data.i64);  /* 92 */
}

flexFree(scores);
```

### Example 3: Mixed-Type Record

```c
/* Create a user record with mixed types */
typedef struct {
    uint64_t id;
    const char *name;
    double balance;
    bool active;
} User;

User user = {
    .id = 12345,
    .name = "Alice Smith",
    .balance = 1234.56,
    .active = true
};

/* Encode to flex */
flex *record = flexNew();

databox fields[4];
fields[0] = databoxNewUnsigned(user.id);
fields[1] = databoxNewBytesString(user.name);
fields[2] = databoxNewReal(user.balance);
fields[3] = databoxBool(user.active);

const databox *fieldPtrs[] = {&fields[0], &fields[1], &fields[2], &fields[3]};
flexAppendMultiple(&record, 4, fieldPtrs);

/* Decode from flex */
flexEntry *fe = flexHeadIfAny(record);
databox decoded[4] = {{{0}}};

for (int i = 0; i < 4 && fe; i++) {
    flexGetByType(fe, &decoded[i]);
    fe = flexNext(record, fe);
}

/* Verify.  Note: flex bytes are NOT NUL-terminated — compare with the
 * box's length, never with strcmp(). */
assert(decoded[0].data.u64 == user.id);
assert(decoded[1].len == strlen(user.name));
assert(memcmp(databoxBytes(&decoded[1]), user.name, decoded[1].len) == 0);
assert(decoded[2].data.d64 == user.balance);  /* 1234.56 stays FLEX_REAL_64B */
assert(DATABOX_IS_TRUE(&decoded[3]));

flexFree(record);
```

### Example 4: Time Series Data

```c
/* Store time series of temperature readings */
flex *timeseries = flexNew();

/* Record format: [timestamp, temperature] */
time_t now = time(NULL);

for (int i = 0; i < 100; i++) {
    databox timestamp = databoxNewUnsigned(now + i * 60);  /* Every minute */
    databox temp = databoxNewReal(20.0 + (rand() % 100) / 10.0);  /* 20-30°C */

    const databox *reading[2] = {&timestamp, &temp};
    flexAppendMultiple(&timeseries, 2, reading);
}

/* Calculate average temperature */
double sum = 0.0;
int count = 0;
flexEntry *fe = flexHead(timeseries);

while (flexEntryIsValid(timeseries, fe)) {
    /* Skip timestamp, read temperature */
    fe = flexNext(timeseries, fe);
    if (!fe) break;

    databox tempBox = {{0}};
    flexGetByType(fe, &tempBox);
    /* Values that round-trip exactly through float come back FLOAT_32 */
    sum += (tempBox.type == DATABOX_FLOAT_32) ? (double)tempBox.data.f32
                                              : tempBox.data.d64;
    count++;

    fe = flexNext(timeseries, fe);
}

printf("Average temperature: %.2f°C\n", sum / count);

flexFree(timeseries);
```

### Example 5: Sorted Insert Performance Test

From the actual test suite:

```c
/* Test sorted insert and find with middle hint optimization */
flex *f = flexNew();
flexEntry *mid = NULL;

/* Insert 100 random integers in sorted order */
printf("Inserting 100 random values in sorted order...\n");
for (int i = 0; i < 100; i++) {
    databox valbox = databoxNewSigned(rand());
    flexInsertByTypeSortedWithMiddle(&f, &valbox, &mid);
    assert(mid == flexMiddle(f, 1));
}

/* Verify all can be found */
printf("Verifying all values can be found...\n");
flexEntry *fe = flexHead(f);
while (flexEntryIsValid(f, fe)) {
    databox stored = {{0}};
    flexGetByType(fe, &stored);

    /* Should find itself */
    flexEntry *found = flexFindByTypeSortedWithMiddle(f, 1, &stored, mid);
    assert(found == fe);

    fe = flexNext(f, fe);
}

printf("SUCCESS: All %zu values found correctly\n", flexCount(f));
flexFree(f);
```

### Example 6: Iterating and Deleting

From the test suite:

```c
/* Iterate from back to front, deleting all items */
flex *f = createList();  /* ["hello", "foo", "quux", 1024] */

flexEntry *fe = flexIndex(f, -1);
assert(fe == flexTail(f));

while (flexEntryIsValid(f, fe)) {
    databox box = {{0}};
    flexGetByType(fe, &box);

    printf("Entry: ");
    if (DATABOX_IS_BYTES(&box)) {
        printf("%.*s", (int)box.len, databoxBytes(&box));
    } else if (DATABOX_IS_INTEGER(&box)) {
        printf("%" PRId64, box.data.i64);
    }
    printf("\n");

    flexDelete(&f, &fe);
    fe = flexPrev(f, fe);
}

assert(flexIsEmpty(f));
flexFree(f);
```

## Common Use Cases

### 1. Configuration Storage

Flex is ideal for storing configuration with mixed types:

```c
flex *config = flexNew();

/* Store config as key-value pairs */
typedef struct {
    const char *key;
    databox value;
} ConfigEntry;

ConfigEntry settings[] = {
    {"port", databoxNewUnsigned(8080)},
    {"host", databoxNewBytesString("localhost")},
    {"timeout", databoxNewReal(30.5)},
    {"debug", databoxBool(true)},
};

for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
    databox keyBox = databoxNewBytesString(settings[i].key);
    const databox *pair[2] = {&keyBox, &settings[i].value};
    flexAppendMultiple(&config, 2, pair);
}
```

### 2. Message Queues

Flex's efficient tail operations make it perfect for queues:

```c
flex *queue = flexNew();

/* Enqueue */
void enqueue(flex **q, const databox *msg) {
    flexPushByType(q, msg, FLEX_ENDPOINT_TAIL);
}

/* Dequeue.  Copy the value out: the delete reallocs the buffer the
 * decoded bytes point into.  Caller releases with databoxFreeData(). */
bool dequeue(flex **q, databox *msg) {
    if (flexIsEmpty(*q)) return false;

    *msg = (databox){{0}};
    flexGetByTypeCopy(flexHead(*q), msg);
    flexDeleteHead(q);
    return true;
}

/* Usage */
databox msg = databoxNewBytesString("Hello, World!");
enqueue(&queue, &msg);

databox received = {{0}};
if (dequeue(&queue, &received)) {
    printf("Received: %.*s\n", (int)received.len, databoxBytes(&received));
    databoxFreeData(&received);
}

flexFree(queue);
```

### 3. Sparse Indices

Flex's compact representation is ideal for storing sparse index data:

```c
/* Store (position, value) pairs for sparse array */
flex *sparseIndex = flexNew();
flexEntry *middle = NULL;

/* Only store non-zero values */
for (int i = 0; i < 10000; i++) {
    if (rand() % 100 == 0) {  /* 1% density */
        databox pos = databoxNewUnsigned(i);
        databox val = databoxNewSigned(rand());
        const databox *entry[2] = {&pos, &val};

        flexInsertByTypeSortedWithMiddleMultiDirect(
            &sparseIndex, 2, entry, &middle);
    }
}

printf("Sparse array: 10000 positions, %zu stored (%.1f%% density)\n",
       flexCount(sparseIndex) / 2,
       100.0 * flexCount(sparseIndex) / 2 / 10000);

flexFree(sparseIndex);
```

### 4. Log Aggregation

Flex can efficiently store structured log entries:

```c
flex *logs = flexNew();

typedef struct {
    time_t timestamp;
    const char *level;
    const char *message;
} LogEntry;

void addLog(flex **logs, LogEntry *entry) {
    databox fields[3] = {
        databoxNewUnsigned(entry->timestamp),
        databoxNewBytesString(entry->level),
        databoxNewBytesString(entry->message)
    };
    const databox *fieldPtrs[3] = {&fields[0], &fields[1], &fields[2]};
    flexAppendMultiple(logs, 3, fieldPtrs);
}

/* Add some logs */
addLog(&logs, &(LogEntry){time(NULL), "INFO", "Server started"});
addLog(&logs, &(LogEntry){time(NULL), "WARN", "High memory usage"});
addLog(&logs, &(LogEntry){time(NULL), "ERROR", "Connection failed"});

flexFree(logs);
```

## Integration with Other Modules

### With databox

Flex and databox are designed to work seamlessly together:

```c
/* databox provides the type system, flex provides the storage */
databox box = databoxNewSigned(42);
flex *f = flexNew();
flexPushByType(&f, &box, FLEX_ENDPOINT_TAIL);

flexEntry *fe = flexHead(f);
databox retrieved = {{0}};
flexGetByType(fe, &retrieved);
assert(databoxEqual(&box, &retrieved));

flexFree(f);
```

### With multimapAtom (Reference Deduplication)

Flex supports storing references instead of duplicating large keys:

```c
#include "multimapAtom.h"

multimapAtom *atom = multimapAtomNew();
flex *f = flexNew();
flexEntry *middle = NULL;

/* Instead of storing duplicate keys, store references */
for (int i = 0; i < 1000; i++) {
    databox key = databoxNewBytesString("common-key");
    databox value = databoxNewSigned(i);

    /* Convert key to atom reference (deduplicated) */
    multimapAtomInsertIfNewConvert(atom, &key);

    const databox *pair[2] = {&key, &value};
    flexInsertReplaceByTypeSortedWithMiddleMultiWithReference(
        &f, 2, pair, &middle, true, atom);
}

/* All 1000 entries share the same key reference, not 1000 copies! */

multimapAtomFree(atom);
flexFree(f);
```

### With multimap/multilist

Flex serves as the underlying storage for these higher-level structures:

```c
/* multimap internally uses flex for storing key-value pairs */
/* multilist uses flex for list operations */
/* These modules provide higher-level APIs on top of flex */
```

### As Nested Container

The encoding reserves eight container types (`FLEX_CONTAINER_MAP` through
`FLEX_CONTAINER_CTUPLE`) whose payload is a nested flex or cflex, and the
readers understand them: `flexGetByType` decodes such an entry into the
matching `DATABOX_CONTAINER_FLEX_*` / `_CFLEX_*` box, and
`flexGetByTypeCopy` copies the nested buffer out.

The **writers do not accept them**: neither `flexPushByType` nor
`flexInsertByType` has a case for a `DATABOX_CONTAINER_*` box, so passing
one is undefined behavior, not an error return. Nested containers are
produced by the higher-level containers that own the encoding, not by
pushing a hand-built container databox.

To carry one flex inside another today, store it as bytes and re-wrap on
read:

```c
flex *inner = flexNew();
flexPushSigned(&inner, 1, FLEX_ENDPOINT_TAIL);
flexPushSigned(&inner, 2, FLEX_ENDPOINT_TAIL);
flexPushSigned(&inner, 3, FLEX_ENDPOINT_TAIL);

flex *outer = flexNew();
flexPushBytes(&outer, inner, flexBytes(inner), FLEX_ENDPOINT_TAIL);
flexFree(inner);   /* the bytes were copied into 'outer' */

/* Read back, validating before iterating the nested bytes */
databox box = {{0}};
flexGetByType(flexHead(outer), &box);
if (flexValidateBuffer(databoxBytes(&box), box.len)) {
    const flex *nested = (const flex *)databoxBytes(&box);
    assert(flexCount(nested) == 3);
}

flexFree(outer);
```

## Best Practices

### 1. Always Check Return Values

```c
/* WRONG - assumes success */
flexEntry *fe = flexIndex(f, 100);
databox box = {{0}};
flexGetByType(fe, &box);  /* Crash if fe is NULL! */

/* RIGHT - check for validity */
flexEntry *fe = flexIndex(f, 100);
if (fe && flexEntryIsValid(f, fe)) {
    databox box = {{0}};
    flexGetByType(fe, &box);
}
```

### 2. Carry the Middle Hint Across Sorted Operations

```c
/* SLOW - a fresh hint every iteration is no hint at all */
for (int i = 0; i < 1000; i++) {
    databox box = databoxNewSigned(i);
    flexEntry *middle = NULL;
    flexInsertByTypeSortedWithMiddle(&f, &box, &middle);
}

/* FAST - one hint, maintained by the calls themselves */
flexEntry *middle = NULL;
for (int i = 0; i < 1000; i++) {
    databox box = databoxNewSigned(i);
    flexInsertByTypeSortedWithMiddle(&f, &box, &middle);
}
```

The hint is a raw pointer into the flex buffer, so it is only valid for the
flex the calls have been maintaining it against — reset it to `NULL` after
any operation that did not update it.

### 3. Never "Preallocate" — Use the Bulk Calls

flex has **no capacity concept**. Every insert and delete reallocs to the
exact new total size, so an oversized `zrealloc` is undone by the very next
push. Batch instead:

```c
/* SLOW - O(total bytes^2): each push reallocs to the exact new size */
flex *f = flexNew();
for (size_t i = 0; i < 10000; i++) {
    flexPushBytes(&f, values[i], lengths[i], FLEX_ENDPOINT_TAIL);
}

/* FAST - chunked build folded with one exact-sized merge */
flexBytesMember members[10000];
for (size_t i = 0; i < 10000; i++) {
    members[i] = (flexBytesMember){.data = values[i], .len = lengths[i]};
}

flex *f = flexNew();
flexBulkPushBytesMembers(&f, members, COUNT_ARRAY(members));
```

### 4. Use Drain for Bulk Deletes

```c
/* SLOW - realloc on each delete */
while (flexCount(f) > 0) {
    flexDeleteOffsetCount(&f, 0, 1);
}

/* FAST - no realloc until done */
while (flexCount(f) > 0) {
    flexDeleteOffsetCountDrain(&f, 0, 1);
}
flexReset(&f);  /* Final cleanup */
```

### 5. Prefer Tail Operations

```c
/* SLOW - O(n) due to shifting all data */
flexPushBytes(&f, "new", 3, FLEX_ENDPOINT_HEAD);

/* FAST - O(1) amortized */
flexPushBytes(&f, "new", 3, FLEX_ENDPOINT_TAIL);
```

## Common Pitfalls

### 1. Stale Entry Pointers

```c
/* WRONG - entry pointer invalidated after modification */
flexEntry *fe = flexHead(f);
flexPushBytes(&f, "new", 3, FLEX_ENDPOINT_TAIL);
flexGetByType(fe, &box);  /* fe may be invalid if realloc happened! */

/* RIGHT - get fresh pointer after modification */
flexPushBytes(&f, "new", 3, FLEX_ENDPOINT_TAIL);
flexEntry *fe = flexHead(f);
flexGetByType(fe, &box);
```

### 2. Forgetting Element Count in Multi-Element Operations

```c
/* WRONG - searching for key-value pair as single element */
flexEntry *found = flexFindByTypeSortedWithMiddle(f, 1, &key, mid);

/* RIGHT - specify 2 elements per entry for key-value pairs */
flexEntry *found = flexFindByTypeSortedWithMiddle(f, 2, &key, mid);
```

### 3. Not Handling Empty Flex

```c
/* WRONG - crash if flex is empty */
flexEntry *fe = flexTail(f);
flexGetByType(fe, &box);

/* RIGHT - check first */
if (!flexIsEmpty(f)) {
    flexEntry *fe = flexTail(f);
    flexGetByType(fe, &box);
}
```

### 4. Mixing Sorted and Unsorted Operations

```c
/* WRONG - breaks sorted order */
flex *f = flexNew();
flexEntry *mid = NULL;
flexInsertByTypeSortedWithMiddle(&f, &box1, &mid);
flexPushByType(&f, &box2, FLEX_ENDPOINT_TAIL);  /* Breaks order! */
flexFindByTypeSortedWithMiddle(f, 1, &box3, mid);  /* May not find! */

/* RIGHT - stick to one mode or rebuild */
/* Either all sorted operations, or all unsorted operations */
```

### 5. Expecting Pushed Bytes to Come Back as Bytes

```c
/* WRONG - "42" is canonicalised to an integer on the way in */
flexPushBytes(&f, "42", 2, FLEX_ENDPOINT_TAIL);
databox box = {{0}};
flexGetByType(flexHead(f), &box);
assert(DATABOX_IS_BYTES(&box));   /* fails: it is an integer */
```

Any push of a numeral-looking string lands in a numeric encoding. Test with
`DATABOX_IS_INTEGER` / `DATABOX_IS_FLOAT` as well, or keep the value out of
numeral form.

### 6. Holding Decoded Bytes Across a Mutation

```c
/* WRONG - box.data points INTO the flex buffer */
databox box = {{0}};
flexGetByType(flexHead(f), &box);
flexPushSigned(&f, 1, FLEX_ENDPOINT_TAIL);   /* reallocs */
use(databoxBytes(&box));                      /* dangling */

/* RIGHT - copy out first */
databox box = {{0}};
flexGetByTypeCopy(flexHead(f), &box);
flexPushSigned(&f, 1, FLEX_ENDPOINT_TAIL);
use(databoxBytes(&box));
databoxFreeData(&box);
```

### 7. Iterating Bytes From an Untrusted Source

```c
/* WRONG - the header's claimed size is taken on faith */
size_t n = flexCount((const flex *)buf);

/* RIGHT - validate the envelope (or the whole structure) first */
if (flexValidateBufferDeep(buf, buflen)) {
    size_t n = flexCount((const flex *)buf);
}
```

## Thread Safety

Flex is **not thread-safe**. Concurrent access requires external synchronization:

```c
#include <pthread.h>

pthread_mutex_t flex_lock = PTHREAD_MUTEX_INITIALIZER;
flex *shared_flex = flexNew();

/* Thread-safe operations */
void thread_safe_push(databox *box) {
    pthread_mutex_lock(&flex_lock);
    flexPushByType(&shared_flex, box, FLEX_ENDPOINT_TAIL);
    pthread_mutex_unlock(&flex_lock);
}
```

## See Also

- [databox](../core/DATABOX.md) - Universal 16-byte container used by flex
- [mflex](MFLEX.md) - Transparently compressed flex wrapper
- [multimap](../multimap/MULTIMAP.md) - Sorted map built on flex
- [multimapAtom](../multimap/VARIANTS.md#multimapatom) - Reference deduplication for flex
- [multilist](../multilist/MULTILIST.md) - List abstraction using flex
- [Architecture Overview](../../ARCHITECTURE.md) - Design philosophy

## Testing

Run the flex test suite:

```bash
./src/datakit-test test flex
```

The test suite includes:

- Basic push/pop operations
- Index access (positive and negative)
- Forward and backward iteration
- Sorted insert and search
- Binary search with middle hints
- Merge and split operations
- Delete operations (single and range)
- Large data handling (>255 bytes)
- Compression/decompression
- Stress tests with random data
- Performance benchmarks

## References

The flex encoding is inspired by:

- **ziplist** (compact list encoding)
- **listpack** (improved ziplist successor)
- **Variable-length encoding** from Protocol Buffers
- **Split varints** for efficient byte count storage

Key improvements over predecessors:

- Symmetric forward/backward encoding for O(1) reverse traversal
- Wider type support (128-bit ints, float16, nested containers)
- Better sorted operation support with middle hints
- Reference types for deduplication
- Compression support

## License

Copyright 2016-2020 Matt Stancliff <matt@genges.com>

Licensed under the Apache License, Version 2.0
