# multimap - Scale-Aware Sorted Key-Value Store

## Overview

`multimap` is a **self-optimizing sorted key-value container** that automatically
transitions between three internal representations as data grows. Rows stay
sorted at all times, so lookups are binary searches and iteration is ordered.

**Key Features:**

- Automatic scaling: 16-byte Small -> 28-byte Medium -> 48-byte Full (plus
  per-submap metadata)
- Multi-column rows: N elements per row (up to 65,535 for Small/Medium)
- Sorted storage with binary search inside every submap
- Map mode (unique keys, values replaced) or Set mode (unique whole rows)
- Reference containers (`multimapAtom`) for string interning
- Forward/reverse iteration with an arbitrary starting key
- Predicate-driven walk (`multimapProcessUntil`) and bulk trim
  (`multimapDeleteByPredicate`)

**Headers**: `multimap.h`, `multimapCommon.h`

**Source**: `multimap.c`, `multimapSmall.c`, `multimapMedium.c`,
`multimapFull.c`

Examples below use `PRId64` / `PRIu64` from `<inttypes.h>`. 64-bit values are
always `int64_t` / `uint64_t` and are always printed through those macros.

> **Compression note**: `multimapNewCompress()` and the `compress` argument of
> `multimapNewConfigure()` record a compression flag in the tagged pointer and
> in each variant's struct, but no variant currently reads it. Today those
> constructors behave exactly like `multimapNewLimit()`. Do not size capacity
> planning around compression.

## Architecture

The multimap family uses **pointer tagging** to manage three underlying
implementations behind one opaque `multimap *`:

```
┌─────────────┐
│  multimap*  │  low 2 bits: variant tag; high 16 bits: 2 x 8-bit metadata
└──────┬──────┘  (compress flag, flexCapSizeLimit level)
       │
       ├──[tag=1]──> multimapSmall  (16 bytes, 1 flex)
       ├──[tag=2]──> multimapMedium (28 bytes, 2 flex arrays)
       └──[tag=3]──> multimapFull   (48 bytes + N submaps)
```

`multimap.c` sets `PTRLIB_BITS_LOW 2` and `PTRLIB_BITS_HIGH 16`, so the
size-limit level and compression flag ride in the unused high bits of the
pointer instead of costing struct space in every variant.

When you call `multimapInsert()`, the implementation:

1. Reads the variant from the low tag bits
2. Routes to the Small/Medium/Full function via a computed goto
3. Checks the upgrade thresholds and, if exceeded, `zrealloc()`s the container
   struct into the next larger variant
4. Re-tags and writes the new pointer back through your `multimap **`

**You never need to know which variant you are using** - the API is identical.
Because the upgrade reallocates the container struct in place, every mutating
entry point takes `multimap **`.

There is no automatic downgrade: a map that reached Full stays Full even after
deletions (Full does reclaim emptied submaps internally).

## Data Structures

### Common Types

From `multimapCommon.h`:

```c
/* Element ("column") count type. Small and Medium store this in a 16-bit
 * bitfield, so those two tiers cap at 65,535 elements per row. */
typedef uint32_t multimapElements;

/* Storage class for the number of submaps in a multimapFull */
typedef uint32_t multimapFullIdx;

/* Cached byte offset of the middle row of a submap (binary-search seed) */
typedef uint32_t multimapFullMiddle;

/* Total rows across every submap of a multimapFull */
typedef uint64_t multimapFullValues;

typedef struct multimapIterator {
    void *mm;         /* untagged pointer back to the multimap */
    flexEntry *entry; /* current position */
    flex *map;        /* submap currently being iterated */
    uint32_t mapIndex;              /* Medium/Full: which submap index */
    uint32_t elementsPerEntry : 16; /* cached mm->elementsPerEntry */
    uint32_t type : 2;              /* 1=Small, 2=Medium, 3=Full */
    uint32_t forward : 1;           /* iterating forward? */
    uint32_t unused : 13;
} multimapIterator; /* statically asserted to be exactly 32 bytes */

typedef struct multimapEntry {
    flex **map;              /* pointer to the owning flex slot */
    flexEntry *fe;           /* entry within that flex */
    multimapFullIdx mapIdx;  /* Full: which submap holds the entry */
} multimapEntry;

typedef enum multimapCondition {
    MULTIMAP_CONDITION_NONE = 0,
    MULTIMAP_CONDITION_ALL, /* foreach */
    MULTIMAP_CONDITION_LESS_THAN,
    MULTIMAP_CONDITION_LESS_THAN_EQUAL,
    MULTIMAP_CONDITION_EQUAL,
    MULTIMAP_CONDITION_GREATER_THAN,
    MULTIMAP_CONDITION_GREATER_THAN_EQUAL
} multimapCondition;

/* Reads as: [MAP CONTENTS] [CONDITION] [COMPARE AGAINST] */
typedef struct multimapPredicate {
    multimapCondition condition;
    databox compareAgainst;
} multimapPredicate;

typedef enum multimapType {
    MULTIMAP_TYPE_SMALL = 1,
    MULTIMAP_TYPE_MEDIUM = 2,
    MULTIMAP_TYPE_FULL = 3
} multimapType;
```

### Borrowed Values

Every read API (`multimapLookup`, `multimapFirst`, `multimapLast`,
`multimapIteratorNext`, `multimapRandomValue`) fills `databox` values that
**point into the underlying flex storage** for byte-typed elements. Numeric,
boolean and reference elements are immediate and always safe to keep.

Byte pointers are valid only until the next mutation of that multimap. Copy
them (`strndup`, `memcpy`, `databoxCopyBytesFromBox`) before inserting,
deleting, replacing, or incrementing anything.

### 96/128-bit Values

Keys and values may be 96/128-bit integers on every tier. A wide value does
not fit a plain `databox`, so the read APIs above follow the `databox.h`
rule: **the owner of the output box sets up its wide capability**.

- To receive a wide value, pass a `databoxBig` armed with
  `DATABOX_BIG_INIT`. Re-arm it before each read: a narrow value overwrites
  the payload pointer.
- A zeroed box receives only the type (`SIGNED_128` / `UNSIGNED_128`, with
  `data.ptr == NULL`). An uninitialized box is a caller bug.
- `multimapDeleteRandomValue()` is a removing read, like `multilistPop()`: a
  wide value is written in band into an armed box, and otherwise comes back
  in a 16-byte heap payload the caller owns (`big = 1`, `allocated = 1`).
  BYTES of 8 bytes or less come back embedded (`DATABOX_BYTES_EMBED`, nothing
  owned) and longer strings as heap copies. Release each box with
  `databoxFreeData()`; its `big` flag must be initialized too.
- Two removing paths report a wide value type-only (`data.ptr == NULL`,
  `big = 0`) whatever the box holds, as documented exceptions:
  `multimapInsertReplacing()`'s `replacedOut` and the `foundReference` of
  `multimapDeleteWithFound()` / `multimapDeleteWithReference()`.

```c
databoxBig val;
DATABOX_BIG_INIT(&val);
databox *out[1] = {(databox *)&val};
if (multimapLookup(m, &key, out) && val.type == DATABOX_UNSIGNED_128) {
    __uint128_t v = *val.data.u128; /* lives in val.extra */
}
```

Rows the library decodes for itself always carry wide storage: the
`multimapProcessUntil()` walker and the key set operations see a box per
column with `big` set exactly on wide values, and internal searches (Medium
routing, Full range boxes) compare wide keys by value.

## Creation Functions

### Basic Creation

Signatures exactly as declared in `multimap.h`:

```c
/* Default size-limit level is FLEX_CAP_LEVEL_2048 */
multimap *multimapNew(multimapElements elementsPerEntry);

/* 'limit' is a flexCapSizeLimit level; the header spells it uint32_t */
multimap *multimapNewLimit(multimapElements elementsPerEntry,
                           const uint32_t limit);

/* Records a compression flag that no variant currently consumes */
multimap *multimapNewCompress(multimapElements elementsPerEntry,
                              const uint32_t limit);

/* Set mode: entire rows are unique (see "Insertion Semantics") */
multimap *multimapSetNew(multimapElements elementsPerEntry);

multimap *multimapNewConfigure(multimapElements elementsPerEntry, bool isSet,
                               bool compress, flexCapSizeLimit sizeLimit);
```

Usage:

```c
multimap *m = multimapNew(2);       /* key + value */
multimap *users = multimapNew(4);   /* id, name, age, score */
multimap *uniqueRows = multimapSetNew(1);
multimap *bulk = multimapNewLimit(2, FLEX_CAP_LEVEL_4096);
```

### Size Limits

`flexCapSizeLimit` (from `flexCapacityManagement.h`) is an **index** into
`flexOptimizationSizeLimit[]`, not a byte count:

```c
FLEX_CAP_LEVEL_0     = 0   /*     0 bytes (degenerate) */
FLEX_CAP_LEVEL_64    = 1   /*    64 bytes */
FLEX_CAP_LEVEL_128   = 2   /*   128 bytes */
FLEX_CAP_LEVEL_256   = 3   /*   256 bytes */
FLEX_CAP_LEVEL_512   = 4   /*   512 bytes */
FLEX_CAP_LEVEL_1024  = 5   /*  1024 bytes */
FLEX_CAP_LEVEL_2048  = 6   /*  2048 bytes (multimap default) */
FLEX_CAP_LEVEL_4096  = 7   /*  4096 bytes */
FLEX_CAP_LEVEL_8192  = 8   /*  8192 bytes */
FLEX_CAP_LEVEL_16384 = 9   /* 16384 bytes */
FLEX_CAP_LEVEL_32768 = 10  /* 32768 bytes */
FLEX_CAP_LEVEL_65536 = 11  /* 65536 bytes */
```

The resolved byte value is used three different ways:

- **Small**: upgrade to Medium once total bytes exceed the limit
- **Medium**: upgrade to Full once total bytes exceed `limit * 3`
- **Full**: becomes `maxSize`, the per-submap split cutoff

### Copying and Metadata

```c
multimap *multimapCopy(const multimap *m);
size_t multimapCount(const multimap *m);   /* number of ROWS, not elements */
size_t multimapBytes(const multimap *m);   /* flex data bytes only */
flex *multimapDump(const multimap *m);     /* caller owns; flexFree() it */
```

`multimapCount()` divides the raw element count by `elementsPerEntry`, so it is
the row count. `multimapBytes()` sums only the flex payloads; the container
struct, the submap pointer array, the middle-offset array and the rangeBox
array are **not** included.

```c
multimap *original = multimapNew(2);
/* ... populate ... */
multimap *backup = multimapCopy(original);

printf("Contains %zu rows\n", multimapCount(original));
printf("Flex payload: %zu bytes\n", multimapBytes(original));
```

## Insertion Semantics

There are three distinct insert behaviors. Choosing the wrong one is the most
common multimap mistake.

| Call                        | Compares     | Duplicate rows | Existing match      |
| --------------------------- | ------------ | -------------- | ------------------- |
| `multimapInsert` (map mode) | element 0    | no             | replaces values     |
| `multimapInsert` (set mode) | all elements | no             | no-op               |
| `multimapInsertFullWidth`   | all elements | **yes**        | inserts another row |

```c
/* Insert or replace by key; returns true when an existing row matched */
bool multimapInsert(multimap **m, const databox *elements[]);

/* Insert only when no row has the key (in a set, any row with the key),
 * in one search; true when inserted */
bool multimapInsertIfAbsent(multimap **m, const databox *elements[]);

/* The same, filling 'me' with the present row when the key was there (a
 * set's first row with the key), to change it in place: find-or-insert
 * in one search */
bool multimapInsertIfAbsentEntry(multimap **m, const databox *elements[],
                                 multimapEntry *me);

/* Sorted bag: all elements form the key, duplicates are allowed */
void multimapInsertFullWidth(multimap **m, const databox *elements[]);

/* Insert into the highest submap directly (see below) */
void multimapAppend(multimap **m, const databox *elements[]);
```

### Map Mode (default)

`multimapNew()` produces a map: element 0 is the key, and only element 0 is
compared. Re-inserting an existing key overwrites elements 1..N-1 and returns
`true`.

```c
multimap *m = multimapNew(2);

databox key = databoxNewBytesString("username");
databox val = databoxNewBytesString("alice");
const databox *elements[2] = {&key, &val};

bool replaced = multimapInsert(&m, elements);  /* false: new key */

databox val2 = databoxNewBytesString("bob");
const databox *update[2] = {&key, &val2};
replaced = multimapInsert(&m, update);         /* true: value replaced */
assert(multimapCount(m) == 1);
```

### Set Mode

`multimapSetNew()` / `multimapNewConfigure(..., isSet=true, ...)` produce a
**set of whole rows**. Insert compares _all_ elements. A row that already
exists verbatim is reported as found and the flex is left untouched; a row that
differs in any element is a new, separate row.

```c
multimap *s = multimapSetNew(2);

databox key = databoxNewBytesString("alice");
databox v1 = databoxNewSigned(1);
const databox *e1[2] = {&key, &v1};
multimapInsert(&s, e1);                 /* false: inserted */

bool dup = multimapInsert(&s, e1);      /* true: exact row already present */
assert(dup && multimapCount(s) == 1);   /* deduped, not appended */

databox v2 = databoxNewSigned(2);
const databox *e2[2] = {&key, &v2};
multimapInsert(&s, e2);                 /* false: DIFFERENT row, inserted */
assert(multimapCount(s) == 2);          /* set mode does NOT replace by key */
```

If you want "one row per key, newest value wins", use map mode. Set mode only
guarantees that no two rows are identical across all elements.

### Full-Width Insert

`multimapInsertFullWidth()` is the duplicates-allowed sorted bag: all elements
participate in the sort order and identical rows may repeat.

```c
multimap *timeseries = multimapNew(2);

databox ts1 = databoxNewSigned(1000);
databox temp1 = databoxNewReal(20.5);
const databox *row1[2] = {&ts1, &temp1};
multimapInsertFullWidth(&timeseries, row1);

databox ts2 = databoxNewSigned(1000);   /* same timestamp */
databox temp2 = databoxNewReal(21.0);   /* different reading */
const databox *row2[2] = {&ts2, &temp2};
multimapInsertFullWidth(&timeseries, row2);

assert(multimapCount(timeseries) == 2); /* both stored */
```

Do not mix `multimapInsertFullWidth()` and `multimapInsert()` on the same
container: the first allows duplicate keys, the second silently replaces one of
them.

### Append

`multimapAppend()` skips the submap-selection step and inserts straight into the
highest submap. It is **not** an O(1) push: the insert inside that submap is
still a sorted insert (and, in map mode, still replaces an existing key).

Its saving is the map-level search, which matters only for Medium and Full;
Small routes `Append` directly to `Insert`.

```c
/* Correct use: keys are non-decreasing */
for (int64_t i = 0; i < 10000; i++) {
    databox k = databoxNewSigned(i);
    databox v = databoxNewSigned(i * 2);
    const databox *e[2] = {&k, &v};
    multimapAppend(&m, e);
}
```

Appending a key that sorts below an earlier submap places it in the wrong
submap and corrupts range routing. Use `multimapInsert()` whenever key order is
not guaranteed.

### Surrogate Key Insertion (Advanced)

For use with `multimapAtom` reference containers:

```c
void multimapInsertWithSurrogateKey(
    multimap **m, const databox *elements[], const databox *insertKey,
    const struct multimapAtom *referenceContainer);
```

`insertKey` is used for ordering and storage in place of `elements[0]`, while
comparisons resolve references through `referenceContainer`. See
[VARIANTS.md](VARIANTS.md) for the atom API.

## Lookup Operations

### Basic Lookup

```c
bool multimapExists(const multimap *m, const databox *key);
bool multimapExistsFullWidth(const multimap *m, const databox *elements[]);
bool multimapExistsWithReference(const multimap *m, const databox *key,
                                 databox *foundRef,
                                 const struct multimapAtom *referenceContainer);

bool multimapLookup(const multimap *m, const databox *key,
                    databox *elements[]);
```

**`multimapLookup()` fills `elementsPerEntry - 1` boxes**: it skips the key you
searched with, so `elements[0]` receives the row's _second_ element.

```c
databox searchKey = databoxNewBytesString("username");

if (multimapExists(m, &searchKey)) {
    databox value;
    databox *results[1] = {&value};   /* 2 columns - 1 = 1 box */

    if (multimapLookup(m, &searchKey, results)) {
        databoxRepr(&value);
    }
}

/* 4-column map: uid | name | age | score */
databox uid = databoxNewSigned(1001);
databox name, age, score;
databox *results[3] = {&name, &age, &score};   /* 4 - 1 = 3 boxes */

if (multimapLookup(users, &uid, results)) {
    databoxRepr(&name);
    databoxRepr(&age);
    databoxRepr(&score);
}
```

### Contiguous-Array Readers

`multimap.h` ships inline helpers that build the pointer array for you, capped
at `MULTIMAP_INTO_MAX` (16) boxes. They are zero-cost with a literal `n`.

```c
enum { MULTIMAP_INTO_MAX = 16 };

/* n == elementsPerEntry - 1 (values only) */
bool multimapLookupInto(const multimap *m, const databox *key,
                        databox out[], uint32_t n);

/* n == elementsPerEntry (full width) */
bool multimapFirstInto(multimap *m, databox out[], uint32_t n);
bool multimapLastInto(multimap *m, databox out[], uint32_t n);
bool multimapIteratorNextInto(multimapIterator *iter, databox out[],
                              uint32_t n);
```

`n` is a contract, not a request: these are never partial reads, so an
undersized `out[]` is written past its end.

```c
databox row[4];
if (multimapFirstInto(users, row, 4)) {
    databoxRepr(&row[0]);   /* key */
}

databox values[3];
if (multimapLookupInto(users, &uid, values, 3)) {
    databoxRepr(&values[0]);  /* name */
}
```

### First and Last

```c
bool multimapFirst(multimap *m, databox *elements[]);  /* lowest key */
bool multimapLast(multimap *m, databox *elements[]);   /* highest key */
```

Both fill the **full** row (`elementsPerEntry` boxes) and return `false` on an
empty container.

```c
databox firstKey, firstName;
databox *first[2] = {&firstKey, &firstName};

if (multimapFirst(m, first)) {
    databoxRepr(&firstKey);
}
```

### Random Access

```c
bool multimapRandomValue(multimap *m, bool fromTail, databox **found,
                         multimapEntry *me);
```

`fromTail == true` selects the **last** row deterministically;
`fromTail == false` selects a uniformly random row.

Despite the `databox **` spelling, `found` is a **full-width pointer array**:
pass `NULL`, or an array of `elementsPerEntry` pointers that each address a real
`databox`. Passing the address of a single box overruns the caller's stack.

```c
databox boxes[2];
databox *found[2] = {&boxes[0], &boxes[1]};
multimapEntry me;

if (multimapRandomValue(m, false, found, &me)) {
    databoxRepr(found[0]);
}

/* Only the position is wanted: skip the fill entirely */
if (multimapRandomValue(m, true, NULL, &me)) {
    /* me now points at the tail row */
}
```

### Underlying Entry Access (Advanced)

```c
bool multimapGetUnderlyingEntry(multimap *m, const databox *key,
                                multimapEntry *me);
bool multimapGetUnderlyingEntryWithReference(
    multimap *m, const databox *key, multimapEntry *me,
    const struct multimapAtom *referenceContainer);
/* The row equal to every element (as multimapExistsFullWidth finds it) */
bool multimapGetUnderlyingEntryFullWidth(multimap *m, const databox *elements[],
                                         multimapEntry *me);

bool multimapResizeEntry(multimap **m, multimapEntry *me, size_t newLen);
bool multimapReplaceEntry(multimap **m, multimapEntry *me, const databox *box);
void multimapDeleteEntry(multimap **m, multimapEntry *me);
```

A located row can be deleted with `multimapDeleteEntry()` without a second
search, as long as the map has not changed since. The persisted wrappers
(`multimapDeleteP`, `multimapDeleteFullWidthP`) locate, log, then delete
this way.

`me->fe` lands on the row's **key**. Walk it forward with `flexNext()` to reach
a later column, then mutate through `multimapReplaceEntry()` /
`multimapResizeEntry()`.

The columns that order rows are never rewritten in place. In a map that is
the key (element 0): keep the entry off it. In a set it is every element, so
on a set both calls (and every field increment) refuse, return false with the
row unchanged, and report `DK_DIAG_MULTIMAP_SET_REWRITE`. Change such a row by
deleting it and inserting the new one.

Always mutate through the multimap wrappers, never with a raw
`flexReplaceByType()` on `*me.map`. The wrappers rescan the cached middle offset
(and, on Full, refresh the submap's rangeBox) after the write. A raw flex write
leaves those caches pointing into stale bytes, and every later sorted search
trusts them blindly.

```c
multimapEntry me;
databox searchKey = databoxNewBytesString("counter");

if (multimapGetUnderlyingEntry(m, &searchKey, &me)) {
    me.fe = flexNext(*me.map, me.fe);        /* move to column 1 */
    databox newValue = databoxNewSigned(999);
    multimapReplaceEntry(&m, &me, &newValue);
}
```

## Deletion Operations

### Basic Deletion

```c
bool multimapDelete(multimap **m, const databox *key);
bool multimapDeleteFullWidth(multimap **m, const databox *elements[]);
bool multimapDeleteWithFound(multimap **m, const databox *key,
                             databox *foundReference);
bool multimapDeleteWithReference(multimap **m, const databox *key,
                                 const struct multimapAtom *referenceContainer,
                                 databox *foundReference);
bool multimapDeleteRandomValue(multimap **m, bool fromTail,
                               databox **deleted);
```

`multimapDeleteRandomValue()` takes the same full-width `deleted` array contract
as `multimapRandomValue()`, but the values it hands back are **copies**, so they
stay valid after the row is gone (short strings embedded, longer strings and
unarmed wide values on the heap; see "96/128-bit Values").

```c
databox key = databoxNewBytesString("username");
if (multimapDelete(&m, &key)) {
    printf("Entry deleted\n");
}

databox deletedRef;
if (multimapDeleteWithFound(&m, &key, &deletedRef)) {
    databoxRepr(&deletedRef);
}
```

Deletion never shrinks a container back to a smaller variant. Full does free
submaps that become empty and collapses to a single empty submap on
`multimapReset()`.

### Predicate-Based Deletion

```c
bool multimapDeleteByPredicate(multimap **m, const multimapPredicate *p);
```

**Only `MULTIMAP_CONDITION_LESS_THAN_EQUAL` is implemented.** All three variants
assert on any other condition. The call is a sorted-prefix trim: every row whose
key is `<= compareAgainst` is removed in one shot, without a per-row walk.

```c
databox limit = databoxNewSigned(100);
multimapPredicate pred = {.condition = MULTIMAP_CONDITION_LESS_THAN_EQUAL,
                          .compareAgainst = limit};

if (multimapDeleteByPredicate(&m, &pred)) {
    printf("Trimmed all rows with key <= 100\n");
}
```

For any other condition, iterate and delete explicitly, collecting keys first
(copying byte keys) because deletion invalidates iterator position.

## Iteration

### Basic Iteration

```c
void multimapIteratorInit(const multimap *m, multimapIterator *iter,
                          bool forward);
bool multimapIteratorInitAt(const multimap *m, multimapIterator *iter,
                            bool forward, const databox *box);
bool multimapIteratorNext(multimapIterator *iter, databox **elements);
```

`multimapIteratorNext()` fills the **full** row. In reverse mode it walks rows
from highest to lowest while still filling `elements[0..N-1]` in column order,
so the same array works in both directions.

`multimapIteratorInitAt()` seeds at the first row whose key is `>= box` and
returns `false` when that position is past the end (the iterator is still
initialized and simply yields nothing).

```c
multimapIterator iter;
databox key, value;
databox *elements[2] = {&key, &value};

/* Forward */
multimapIteratorInit(m, &iter, true);
while (multimapIteratorNext(&iter, elements)) {
    databoxRepr(&key);
    databoxRepr(&value);
}

/* Reverse */
multimapIteratorInit(m, &iter, false);
while (multimapIteratorNext(&iter, elements)) {
    /* highest key first */
}

/* Range scan from a key */
databox startKey = databoxNewSigned(1000);
if (multimapIteratorInitAt(m, &iter, true, &startKey)) {
    while (multimapIteratorNext(&iter, elements)) {
        /* rows with key >= 1000 */
    }
}
```

Do not mutate a multimap while an iterator over it is live; insert and delete
both move flex bytes.

### Iterating Into Your Own Wide Slots

```c
bool multimapIteratorNextWithSlots(multimapIterator *iter, databox **elements,
                                   databoxWide *slots);
```

Library code that iterates a multimap into entries it owns (the multidict slot
class, for example) passes `slots`, an array of `elementsPerEntry`
`databoxWide` slots, one per column. Each element is decoded with
`flexGetByTypeInto()` into `slots[i]`: a 96/128-bit element comes back with
`big = 1`, `allocated = 0` and its payload in `slots[i]`, valid until that slot
is reused. A narrow element writes only type, data and len, leaves the box's
flag bits as they were, and never touches its slot. With `slots == NULL` it is
`multimapIteratorNext()`, with the in-band caller-box contract.

### Walking With a Predicate

```c
typedef bool(multimapElementWalker)(void *userData, const databox *elements[]);

size_t multimapProcessUntil(multimap *m, const multimapPredicate *p,
                            bool forward, multimapElementWalker *walker,
                            void *userData);

bool multimapProcessPredicate(const multimapPredicate *p, const databox *value);
```

`multimapProcessUntil()` walks rows and tests the predicate against
`elements[0]` (the key). It **stops at the first row that fails** - it is an
"until", not a filter - and returns how many rows were passed to `walker`.
Returning `false` from the walker also stops the walk.

Unlike `multimapDeleteByPredicate()`, this path supports every
`multimapCondition`. The walk decodes each row into its own boxes plus one
`databoxWide` slot per column (16 columns on the stack, wider rows on the
heap), so the walker receives 96/128-bit values with live payloads and `big`
set exactly on them. The boxes and slots are reused for the next row: copy
anything the walker keeps.

```c
static bool printElement(void *userData, const databox *elements[]) {
    uint64_t *count = userData;
    printf("Entry %" PRIu64 ": ", (*count)++);
    databoxRepr(elements[0]);
    return true; /* continue */
}

uint64_t count = 0;
multimapPredicate pred = {.condition = MULTIMAP_CONDITION_ALL};
multimapProcessUntil(m, &pred, true, printElement, &count);
```

## Field Operations

### Field Increment

```c
bool multimapFieldIncrBy(multimap **m, const databox *key, uint32_t fieldOffset,
                         int64_t incrBy, int64_t *newValue);
typedef bool(flexProceed)(void *ctx); /* flex.h */
bool multimapFieldIncrByIf(multimap **m, const databox *key,
                           uint32_t fieldOffset, int64_t incrBy,
                           int64_t *newValue, flexProceed *proceed, void *ctx);
int64_t multimapFieldIncr(multimap **m, const databox *key,
                          uint32_t fieldOffset, int64_t incrBy);
```

Behavior, exactly as implemented:

- One search: the key is looked up, then `fieldOffset` columns are walked
  forward from it. `fieldOffset` must be `>= 1`; offset 0 is the key itself.
- The field must hold an integer in `int64_t` range or a boolean. A missing
  key, any other field (bytes, a `DATABOX_UNSIGNED_64` above `INT64_MAX`), or a
  sum outside `int64_t` is **refused**: `multimapFieldIncrBy` returns false and
  the map is unchanged. The sum is exact.
- A set refuses every increment: its values are part of each row's sort order,
  so a value changed in place would leave the row out of order for full-width
  searches. Delete the row and insert the new one instead.
- `incrBy == 0` returns the current value and writes nothing.
- Booleans participate: `FALSE` acts as 0 and `TRUE` as 1. `FALSE + 1` becomes
  `TRUE`, `TRUE - 1` becomes `FALSE`, and any other result makes the field an
  integer. `multimapAtom` relies on this to keep a one-reference refcount down
  to a single byte.
- `multimapFieldIncrByIf` asks `proceed(ctx)` once the increment is known to
  apply and change the field (never for `incrBy == 0` or a refusal). A false
  answer leaves the map unchanged. On any false return `*newValue` is the
  field's current value (0 for a missing key or a non-counter field). A
  persisted map logs the increment from `proceed`, so a log that is refused
  declines it and nothing has to be undone.
- `multimapFieldIncr` returns the sum, or on a refusal the current value (0 for
  a missing key or a non-counter field), so it cannot report a refusal.
- The write goes through `multimapReplaceEntry()`, so it maintains the cached
  middle but does not re-split an oversized submap.

```c
multimap *counters = multimapNew(2);

databox key = databoxNewBytesString("pageviews");
databox count = databoxNewSigned(0);
const databox *init[2] = {&key, &count};
multimapInsert(&counters, init);

int64_t newCount = multimapFieldIncr(&counters, &key, 1, 1);
printf("New count: %" PRId64 "\n", newCount);

newCount = multimapFieldIncr(&counters, &key, 1, 10);
printf("New count: %" PRId64 "\n", newCount);

/* Row: [UserID, LoginCount, PointsEarned] */
multimap *stats = multimapNew(3);

databox uid = databoxNewSigned(1001);
databox logins = databoxNewSigned(0);
databox points = databoxNewSigned(0);
const databox *row[3] = {&uid, &logins, &points};
multimapInsert(&stats, row);

multimapFieldIncr(&stats, &uid, 1, 1);     /* LoginCount */
multimapFieldIncr(&stats, &uid, 2, 100);   /* PointsEarned */

/* Missing key: no crash, returns 0, inserts nothing */
databox absent = databoxNewSigned(999999);
assert(multimapFieldIncr(&stats, &absent, 1, 1) == 0);
```

## Set Operations

All three helpers use element 0 of each row as the key and insert into `dst`
with `multimapInsert()`, so `dst` should normally be a 1-element multimap.

### Intersection

```c
void multimapIntersectKeys(multimap **restrict dst,
                           multimapIterator *restrict const a,
                           multimapIterator *restrict const b);
```

A sorted zipper walk over two already-initialized iterators. The two sources may
have different `elementsPerEntry`.

```c
multimap *result = multimapNew(1);

multimapIterator iter1, iter2;
multimapIteratorInit(map1, &iter1, true);
multimapIteratorInit(map2, &iter2, true);

multimapIntersectKeys(&result, &iter1, &iter2);
```

### Difference

```c
void multimapDifferenceKeys(multimap **restrict dst,
                            multimapIterator *restrict const a,
                            multimapIterator *restrict const b,
                            const bool symmetricDifference);
```

**Both iterators must come from 1-element multimaps**; the implementation
asserts `a->elementsPerEntry == b->elementsPerEntry == 1`.

```c
multimap *difference = multimapNew(1);
multimapIteratorInit(keysA, &iter1, true);
multimapIteratorInit(keysB, &iter2, true);

multimapDifferenceKeys(&difference, &iter1, &iter2, false); /* A - B */
```

### Copy Keys

```c
void multimapCopyKeys(multimap **restrict dst, const multimap *restrict src);
```

Effectively a union: every key of `src` is inserted into `dst`, so repeated
calls accumulate.

```c
multimap *keysOnly = multimapNew(1);
multimapCopyKeys(&keysOnly, sourceMap);
```

## Memory Management

```c
void multimapReset(multimap *m);  /* drop all rows, keep the container */
void multimapFree(multimap *m);   /* destroy; NULL-safe */
```

`multimapReset()` does not change the variant, but Full collapses back to a
single empty submap so emptied submaps cannot keep stale metadata alive.
`multimapFree(NULL)` is a no-op.

```c
multimap *m = multimapNew(2);
/* ... use it ... */
multimapReset(m);   /* reuse the container */
multimapFree(m);    /* destroy */
```

`multimapDump()` returns a freshly allocated `flex` containing every row
concatenated; free it with `flexFree()`.

### Reference Containers (Advanced)

`multimapAtom` interns byte strings into small integer references with
reference counting. The `*WithReference` and `*WithSurrogateKey` entry points
resolve those references during comparison so a multimap keyed by references
still sorts by the underlying strings. See
[VARIANTS.md](VARIANTS.md#multimapatom) for the full atom API.

## Performance Characteristics

`n` = rows in the searched submap, `M` = number of submaps in a Full.

| Operation      | Small                       | Medium              | Full                           |
| -------------- | --------------------------- | ------------------- | ------------------------------ |
| Lookup/Exists  | O(log n)                    | O(1) route + log n  | O(log M) route + O(log n)      |
| Insert/Replace | O(log n) search + O(n) move | same, half the data | same, per-submap only          |
| Delete         | O(log n) search + O(n) move | same                | same, per-submap only          |
| Append         | = Insert                    | skips route         | skips route                    |
| Iteration      | O(n)                        | O(n)                | O(n + M)                       |
| First/Last     | O(1)                        | O(1)                | O(1)                           |
| Trim (`<=`)    | one bulk delete             | one bulk delete     | submap drops + one bulk delete |

Binary search inside a submap is a real binary search: flex maintains a backward
offset index, and each multimap tier caches the byte offset of the middle row as
the search seed.

### Space

| Variant | Container | Per-submap metadata                                         | Growth trigger     |
| ------- | --------- | ----------------------------------------------------------- | ------------------ |
| Small   | 16 bytes  | none (1 flex)                                               | bytes > limit      |
| Medium  | 28 bytes  | none (2 flex)                                               | bytes > limit \* 3 |
| Full    | 48 bytes  | 8 (flex\*) + 4 (middle) + 32 (rangeBox, for submaps 1..N-1) | never              |

Full's metadata lives in three `multiarray` containers, which add their own
allocation overhead on top of the per-submap figures.

### Automatic Upgrade Thresholds

As implemented in `multimapUpgradeIfNecessary_()`:

```c
/* Small -> Medium */
multimapSmallBytes(small) > flexOptimizationSizeLimit[limit] &&
    multimapSmallCount(small) > (small->elementsPerEntry * 2)

/* Medium -> Full */
multimapMediumBytes(medium) > (flexOptimizationSizeLimit[limit] * 3) &&
    flexCount(medium->map[0]) && flexCount(medium->map[1])
```

The Small guard needs at least two complete rows so the split produces two
non-empty halves. The Medium guard requires both submaps to be non-empty for the
same reason. Thresholds are checked after every `Insert`, `InsertFullWidth`,
`Append` and `InsertWithSurrogateKey`.

## Best Practices

### 1. Choose the size limit for your access pattern

A lookup inside a submap is a binary search over a flex, whose entries are
variable-length, so each probe walks entry by entry: the walk grows with the
submap, while the routing across submaps (one compare per halving) barely
does. Measured at 100,000 rows of a 16-byte key and an integer
(`dktools perfcompare NAME --base HEAD --modules multimapLimits`, Apple M1 Max,
clang 22 perf, medians of 15+15 runs, ns per call):

| limit | lookup | insert | bytes |
| ---: | ---: | ---: | ---: |
| 256 | 527 | 681 | 2,265,663 |
| 512 | 513 | 687 | 2,252,934 |
| 1024 | 510 | 707 | 2,244,129 |
| 2048 (default) | 606 | 738 | 2,239,704 |
| 4096 | 753 | 977 | 2,237,182 |
| 8192 | 1022 | 1464 | 2,236,256 |

For small rows at scale, 512 or 1024 is ~16% faster to look up than the
default for 0.2-0.6% more memory; larger limits are slower for reads and
writes alike. Rows near the limit in size (a multiroar chunk is up to a 1 KB
bitmap) need a limit well above one row, or every submap holds one row.

```c
/* Many small rows, random access: shorter walks inside each submap */
multimap *index = multimapNewLimit(2, FLEX_CAP_LEVEL_1024);
```

### 2. Keep rows narrow

```c
/* Good: natural column grouping */
multimap *users = multimapNew(4);  /* id, name, age, email */
```

Every read API fills `elementsPerEntry` boxes, the `*Into` helpers stop at 16
columns, and `multimapProcessUntil()` allocates its row on the heap above 16.
Split wide rows across multiple multimaps rather than growing one.

### 3. Pick map mode or set mode deliberately

```c
multimap *byKey = multimapNew(2);      /* newest value per key wins */
multimap *uniqueRows = multimapSetNew(2); /* no duplicate (key,value) pairs */
```

### 4. Use Append only for non-decreasing keys

```c
for (int64_t i = 0; i < 10000; i++) {
    databox k = databoxNewSigned(i);
    databox v = databoxNewSigned(i * 2);
    const databox *e[2] = {&k, &v};
    multimapAppend(&m, e);  /* skips the submap search */
}
```

### 5. Copy byte values before mutating

```c
databox value;
databox *results[1] = {&value};
if (multimapLookup(m, &key, results)) {
    char *owned = strndup((const char *)databoxCBytes(&value),
                          databoxLen(&value));
    multimapDelete(&m, &key);   /* 'value' is now dangling; 'owned' is not */
    free(owned);
}
```

## Common Patterns

### Pattern 1: Dictionary

```c
multimap *dict = multimapNew(2);

databox key = databoxNewBytesString("color");
databox val = databoxNewBytesString("blue");
const databox *kv[2] = {&key, &val};
multimapInsert(&dict, kv);

databox result;
if (multimapLookupInto(dict, &key, &result, 1)) {
    databoxRepr(&result);
}

multimapDelete(&dict, &key);
```

### Pattern 2: Time Series

```c
multimap *timeseries = multimapNew(2);  /* timestamp + value */

for (int64_t i = 0; i < 1000; i++) {
    databox ts = databoxNewSigned(baseTimestamp + i);
    databox temp = databoxNewReal(20.0 + (i % 10));
    const databox *reading[2] = {&ts, &temp};
    multimapAppend(&timeseries, reading);   /* timestamps only increase */
}

databox startTime = databoxNewSigned(targetTimestamp);
multimapIterator iter;

if (multimapIteratorInitAt(timeseries, &iter, true, &startTime)) {
    databox row[2];
    while (multimapIteratorNextInto(&iter, row, 2)) {
        printf("%" PRId64 ": %.2f\n", row[0].data.i64, row[1].data.d64);
    }
}
```

### Pattern 3: Leaderboard

Scores repeat, so the store must allow duplicate keys: use full-width inserts
and read the tail for the top entries.

```c
/* Row: score | playerName */
multimap *leaderboard = multimapNew(2);

databox score1 = databoxNewSigned(9500);
databox player1 = databoxNewBytesString("Alice");
const databox *entry1[2] = {&score1, &player1};
multimapInsertFullWidth(&leaderboard, entry1);

databox score2 = databoxNewSigned(8200);
databox player2 = databoxNewBytesString("Bob");
const databox *entry2[2] = {&score2, &player2};
multimapInsertFullWidth(&leaderboard, entry2);

databox top[2];
if (multimapLastInto(leaderboard, top, 2)) {
    printf("Top player: %.*s with %" PRId64 " points\n",
           (int)databoxLen(&top[1]), (const char *)databoxCBytes(&top[1]),
           top[0].data.i64);
}

multimapIterator iter;
multimapIteratorInit(leaderboard, &iter, false);  /* highest first */

databox row[2];
while (multimapIteratorNextInto(&iter, row, 2)) {
    printf("%" PRId64 ": %.*s\n", row[0].data.i64,
           (int)databoxLen(&row[1]), (const char *)databoxCBytes(&row[1]));
}
```

### Pattern 4: Multi-Column Table

```c
/* Table: UserID | Name | Age | Active */
multimap *users = multimapNew(4);

databox uid = databoxNewSigned(1001);
databox name = databoxNewBytesString("Charlie");
databox age = databoxNewSigned(30);
databox status = databoxBool(true);
const databox *row[4] = {&uid, &name, &age, &status};
multimapInsert(&users, row);

databox cols[3];   /* 4 columns - 1 key */
if (multimapLookupInto(users, &uid, cols, 3)) {
    printf("User %" PRId64 ": %.*s, age %" PRId64 ", active: %s\n",
           uid.data.i64,
           (int)databoxLen(&cols[0]), (const char *)databoxCBytes(&cols[0]),
           cols[1].data.i64,
           DATABOX_IS_TRUE(&cols[2]) ? "yes" : "no");
}
```

## Thread Safety

multimap is **not thread-safe**. Every mutating call may reallocate both the
flex payload and the container struct, so readers must be excluded during
writes:

```c
pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;
multimap *shared = multimapNew(2);

/* Reader */
pthread_rwlock_rdlock(&lock);
bool exists = multimapExists(shared, &key);
pthread_rwlock_unlock(&lock);

/* Writer */
pthread_rwlock_wrlock(&lock);
multimapInsert(&shared, elements);
pthread_rwlock_unlock(&lock);
```

Borrowed byte values read under the read lock must be copied before the lock is
released.

## See Also

- [VARIANTS.md](VARIANTS.md) - Small/Medium/Full internals and the atom API
- [EXAMPLES.md](EXAMPLES.md) - Worked multimap programs
- [databox](../core/DATABOX.md) - Universal value container
- [flex](../flex/FLEX.md) - Underlying sorted byte-packed storage
- [multiarray](../multiarray/MULTIARRAY.md) - Dynamic arrays behind Full

## Testing

```bash
./src/datakit-test test multimap
./src/datakit-test test multimapMedium
./src/datakit-test test multimapFull
./src/datakit-test test multimapAtom   # alias: atom
```

The multimap suite covers:

- Small -> Medium -> Full automatic transitions
- Insert, lookup and delete across all three variants
- Map/set/full-width insert semantics
- Forward and reverse iteration
- Field increment, including the missing-key and boolean cases
- Sorted-prefix trim via `multimapDeleteByPredicate()`
- `multimapVerify()` invariant checks (cached middles and Full rangeBoxes)
  after mutations
