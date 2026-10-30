# Common Coding Patterns

Short, copy-paste-ready idioms that recur across datakit modules. Every
snippet here is checked against the current headers in `src/`.

Scope: this file holds **small idioms**. Complete applications live in
[USE_CASES.md](USE_CASES.md), measurement recipes live in
[BENCHMARKS.md](BENCHMARKS.md), and deep per-module walkthroughs live in
`docs/modules/<module>/EXAMPLES.md`.

## Table of Contents

1. [Container Creation](#container-creation)
2. [Iteration](#iteration)
3. [Reading Entries Into Arrays](#reading-entries-into-arrays)
4. [Memory Management](#memory-management)
5. [Error Handling](#error-handling)
6. [Data Transformation](#data-transformation)
7. [Set and Bitmap Operations](#set-and-bitmap-operations)
8. [String Building](#string-building)
9. [Compression](#compression)
10. [Timing and Randomness](#timing-and-randomness)

---

## Container Creation

### Pattern: Constructors Return a Pointer, Mutators Take Its Address

Every datakit container is created by a constructor that **returns** the
container. Mutating calls then take the _address_ of your pointer,
because an insert can reallocate the container or promote it to a larger
internal variant:

```c
#include "multimap.h"

/* Create: the constructor returns the container. */
multimap *m = multimapNew(2);  /* 2 elements per entry: key + value */

/* Mutate: pass &m, because insert may reallocate or promote m. */
databox key = databoxNewBytesString("username");
databox value = databoxNewBytesString("alice");
const databox *entry[2] = {&key, &value};
multimapInsert(&m, entry);

/* Read: read-only calls take the plain pointer. */
printf("entries: %zu\n", multimapCount(m));

multimapFree(m);
m = NULL;  /* multimapFree does NOT clear your pointer; do it yourself. */
```

The same split applies to `flex` (`flexNew()` then `flexPushByType(&f,
...)`), `multilist` (`multilistNewLimit()` then `multilistPushByTypeTail(&ml,
...)`), and `intset` (`intsetNew()` then `intsetAdd(&is, ...)`). Never
pass a pointer you have not initialized to a mutator.

### Pattern: Let multimap Pick Its Own Variant

`multimap` is a tagged handle that starts as `multimapSmall` and promotes
itself to `multimapMedium` and then `multimapFull` as it grows. Use the
generic API unless you have measured a reason not to:

```c
/* Preferred: one API, automatic promotion at every scale. */
multimap *m = multimapNew(2);

/* Direct variants exist when you want to pin the representation
 * (no promotion, no tag dispatch). Signatures differ from the
 * generic constructor: */
multimapSmall *sm = multimapSmallNew(2, false);  /* (elements, isSet) */
multimapMedium *mm = multimapMediumNew(2);       /* (elements)        */
```

See `docs/modules/multimap/VARIANTS.md` for when pinning a variant pays.

### Pattern: Size the Node Budget Up Front

Containers built on `flex` nodes take a `flexCapSizeLimit` describing how
large one node grows before splitting. Larger nodes mean fewer splits and
better locality; smaller nodes mean cheaper individual updates:

```c
#include "flexCapacityManagement.h"

/* multimap with 4 KB nodes instead of the 2 KB default. */
multimap *m = multimapNewLimit(2, FLEX_CAP_LEVEL_4096);

/* multilist: (node size limit, uncompressed-nodes-at-each-end depth);
 * a nonzero depth LZ4-compresses the nodes between the ends. */
multilist *ml = multilistNewLimit(FLEX_CAP_LEVEL_2048, 0);

/* multiarray: (element size, elements per row). */
typedef struct Record { uint64_t id; double value; } Record;
multiarray *arr = multiarrayNew(sizeof(Record), 2048);
```

`FLEX_CAP_LEVEL_*` names run from `FLEX_CAP_LEVEL_64` to
`FLEX_CAP_LEVEL_65536`.

---

## Iteration

### Pattern: Forward Iteration

```c
/* multimap: iterator fills one pointer per element in the entry. */
multimapIterator iter;
multimapIteratorInit(m, &iter, true);  /* true = forward */

databox entry[2];
while (multimapIteratorNextInto(&iter, entry, 2)) {
    printf("%.*s = %.*s\n",
           (int)databoxLen(&entry[0]), (const char *)databoxCBytes(&entry[0]),
           (int)databoxLen(&entry[1]), (const char *)databoxCBytes(&entry[1]));
}
```

```c
/* multilist: needs a TWO-slot mflexState array and its own
 * iterator type (multilistIterator, not multimapIterator). */
mflexState *s0 = mflexStateCreate();
mflexState *s1 = mflexStateCreate();
mflexState *s[2] = {s0, s1};

multilistIterator lit;
multilistIteratorInitForwardReadOnly(ml, s, &lit);

multilistEntry le;
while (multilistNext(&lit, &le)) {
    /* le.box references data inside the list node. */
}

multilistIteratorRelease(&lit);
mflexStateFree(s0);
mflexStateFree(s1);
```

```c
/* flex: start from flexHeadIfAny, NOT flexHead.
 * flexHead returns an insertion POSITION and is non-NULL even when the
 * flex is empty; flexHeadIfAny applies the same end check flexNext does. */
for (flexEntry *fe = flexHeadIfAny(f); fe; fe = flexNext(f, fe)) {
    databox box;
    flexGetByType(fe, &box);
    /* box REFERENCES bytes inside f: valid only until f is mutated. */
}
```

### Pattern: Reverse Iteration

```c
/* multimap */
multimapIteratorInit(m, &iter, false);  /* false = reverse */

/* multilist */
multilistIteratorInitReverseReadOnly(ml, s, &lit);

/* flex */
for (flexEntry *fe = flexTail(f); fe && flexEntryIsValid(f, fe);
     fe = flexPrev(f, fe)) {
    databox box;
    flexGetByType(fe, &box);
}
```

### Pattern: Start Iteration at a Key

`multimapIteratorInitAt` positions at (or after) a key and returns false
when the map holds nothing at or past it. This is the range-query idiom
for any sorted multimap:

```c
databox startKey = databoxNewSigned(1700000000);
multimapIterator iter;
if (multimapIteratorInitAt(m, &iter, true, &startKey)) {
    databox entry[2];
    while (multimapIteratorNextInto(&iter, entry, 2)) {
        if (entry[0].data.i64 > endTimestamp) {
            break;
        }
        /* entry is inside [start, end] */
    }
}
```

### Pattern: Deleting During Iteration

`multidict` has a safe iterator that tolerates deletes:

```c
multidictIterator iter;
multidictIteratorGetSafe(d, &iter);

multidictEntry e;
while (multidictIteratorNext(&iter, &e)) {
    if (shouldDelete(&e.key)) {
        multidictDelete(d, &e.key);
    }
}
multidictIteratorRelease(&iter);
```

Sorted containers have no safe iterator: collect keys first, delete
after. Bound the collection so one pass cannot grow without limit:

```c
enum { DELETE_BATCH = 1024 };
databox toDelete[DELETE_BATCH];
size_t deleteCount = 0;

multimapIterator iter;
multimapIteratorInit(m, &iter, true);

databox entry[2];
while (deleteCount < DELETE_BATCH &&
       multimapIteratorNextInto(&iter, entry, 2)) {
    if (shouldDelete(&entry[1])) {
        toDelete[deleteCount++] = databoxCopy(&entry[0]);
    }
}

for (size_t i = 0; i < deleteCount; i++) {
    multimapDelete(&m, &toDelete[i]);
    databoxFreeData(&toDelete[i]);
}
```

`databoxCopy` matters here: an iterator's boxes reference bytes inside
the container, and the first delete invalidates them.

`multimapDeleteByPredicate` does the same job in one call when your
condition fits a `multimapPredicate`.

### Pattern: Indexed Access

```c
/* intset is sorted; positions are stable between mutations. */
for (size_t i = 0; i < intsetCount(is); i++) {
    int64_t value;
    if (intsetGet(is, (uint32_t)i, &value)) {
        printf("%" PRId64 " ", value);
    }
}

/* flex by index (O(n) walk — prefer the iterator for full scans). */
flexEntry *fe = flexIndex(f, 42);
if (fe) {
    databox box;
    flexGetByType(fe, &box);
}

/* multiarray returns a pointer to the stored element itself. */
Record *r = (Record *)multiarrayGet(arr, 42);

/* multilist by index (single mflexState, not the two-slot array). */
mflexState *state = mflexStateCreate();
multilistEntry le;
if (multilistIndexGet(ml, state, 42, &le)) {
    /* le.box */
}
mflexStateFree(state);
```

---

## Reading Entries Into Arrays

The raw multimap read API takes an array of `databox *`. The `*Into`
helpers build that wiring for you, so call sites stop hand-rolling
`databox *ptrs[N] = {&a, &b, ...}`:

```c
/* Lookup fills elementsPerEntry - 1 boxes (values only; you have the key). */
multimap *m = multimapNew(3);  /* key + value + timestamp */
databox values[2];
if (multimapLookupInto(m, &key, values, 2)) {
    /* values[0] = value, values[1] = timestamp */
}

/* First/Last/IteratorNext fill elementsPerEntry boxes (full width). */
databox entry[3];
if (multimapFirstInto(m, entry, 3)) {
    /* entry[0] = key, entry[1] = value, entry[2] = timestamp */
}
```

`n` is a contract, not a request: these are not partial reads, so an
undersized array is written past its end. The helpers are capped at
`MULTIMAP_INTO_MAX` (16); wider maps use the raw pointer-array API.

---

## Memory Management

### Pattern: Free in Reverse Order of Creation

```c
multimap *m = multimapNew(2);
mflexState *state = mflexStateCreate();

/* ... use ... */

mflexStateFree(state);
multimapFree(m);
m = NULL;
```

No datakit `*Free` clears the caller's pointer. Null it yourself if the
variable outlives the free.

### Pattern: Know Which Reads Own Their Bytes

```c
/* REFERENCE — points into the container. No free; invalid after mutation. */
databox ref;
flexGetByType(fe, &ref);

/* OWNED — allocates. Release with databoxFreeData. */
databox owned;
flexGetByTypeCopy(fe, &owned);
databoxFreeData(&owned);

/* Pops always hand you an owned copy (the source entry is gone). */
databox popped;
if (multilistPopHead(&ml, state, &popped)) {
    printf("%.*s\n", (int)databoxLen(&popped),
           (const char *)databoxCBytes(&popped));
    databoxFreeData(&popped);
}
```

`databoxFreeData` is a no-op on a box that does not own its buffer, so it
is always safe to call — but reaching for it does not make a reference
into a copy. Use `databoxCopy` when you need a box to outlive its
container.

Never print a box with `%s`: box bytes are not guaranteed to be
NUL-terminated. Use `%.*s` with `databoxLen` / `databoxCBytes`.

### Pattern: Grow a Buffer with fibbuf + jebuf

Fibonacci growth keeps reallocation amortized; `jebuf` snaps the request
to an allocator size class and tells you whether the new class is
actually different from the one you already occupy:

```c
#include "datakit.h" /* zmalloc / zrealloc */
#include "fibbuf.h"
#include "jebuf.h"

size_t currentSize = 1024;
uint8_t *buf = zmalloc(currentSize);

/* Need more room... */
const size_t wantSize = fibbufNextSizeBuffer(currentSize);
const size_t allocSize = jebufSizeAllocation(wantSize);

if (jebufUseNewAllocation(currentSize, allocSize)) {
    buf = zrealloc(buf, allocSize);
    currentSize = allocSize;
}
```

When the size class is unchanged, `jebufUseNewAllocation` returns false
and you skip the realloc entirely.

### Pattern: Allocate From a Bounded Arena

`membound` is an arena you create and pass explicitly — it is not a
global allocator switch, and it is never embedded inside a datakit
container:

```c
#include "membound.h"

membound *arena = memboundCreate(100 * 1024 * 1024);  /* 100 MB */

void *p = memboundAlloc(arena, 1024 * 1024);
if (!p) {
    fprintf(stderr, "arena exhausted (%zu / %zu bytes used)\n",
            memboundBytesUsed(arena), memboundCapacity(arena));
    return false;
}

memboundFree(arena, p);
memboundShutdown(arena);
```

See `docs/modules/memory/MEMBOUND.md` for the fixed / dynamic / preset
constructors and the eviction-pressure callbacks.

---

## Error Handling

### Pattern: Check Return Values

```c
/* Boolean "was it found" results. */
databox key = databoxNewBytesString("username");
databox value;
if (multimapLookupInto(m, &key, &value, 1)) {
    printf("found: %.*s\n", (int)databoxLen(&value),
           (const char *)databoxCBytes(&value));
} else {
    fprintf(stderr, "key not found\n");
}

/* Out-parameter results: intsetAdd reports whether the value was new. */
bool added;
intsetAdd(&is, 42, &added);
if (!added) {
    printf("42 was already present\n");
}

/* Constructors return NULL only on allocation failure. */
flex *f = flexNew();
if (!f) {
    return false;
}
```

### Pattern: Guard Empty and Out-of-Range

```c
if (multimapCount(m) == 0) {
    return;
}

if (flexIsEmpty(f)) {
    return;
}

int64_t value;
if (!intsetGet(is, index, &value)) {
    fprintf(stderr, "index %" PRIu32 " out of bounds (count %zu)\n",
            index, intsetCount(is));
    return;
}
```

---

## Data Transformation

### Pattern: Parse a String Into Whatever It Actually Is

`StrScanScanReliable` detects the narrowest exact type and boxes it:

```c
#include "str.h"
#include <inttypes.h>

const char *input = "42.5";
databox result;

if (StrScanScanReliable(input, strlen(input), &result)) {
    switch (result.type) {
    case DATABOX_SIGNED_64:
        printf("integer: %" PRId64 "\n", result.data.i64);
        break;
    case DATABOX_UNSIGNED_64:
        printf("unsigned: %" PRIu64 "\n", result.data.u64);
        break;
    case DATABOX_DOUBLE_64:
        printf("double: %f\n", result.data.d64);
        break;
    default:
        break;
    }
}

/* When you already know it is an unsigned integer: */
uint64_t parsed;
if (StrBufToUInt64("12345", 5, &parsed)) {
    printf("value: %" PRIu64 "\n", parsed);
}
```

### Pattern: Format Numbers Without printf

```c
#include "str.h"
#include "strDoubleFormat.h"

char buf[32];
size_t len = StrInt64ToBuf(buf, sizeof(buf), -12345);
printf("%.*s\n", (int)len, buf);            /* -12345 */

len = StrUInt64ToBuf(buf, sizeof(buf), UINT64_C(18446744073709551615));
printf("%.*s\n", (int)len, buf);

/* Shortest round-trippable rendering of a double. */
char dbuf[64];
len = StrDoubleFormatToBufNice(dbuf, sizeof(dbuf), 3.14159);
printf("%.*s\n", (int)len, dbuf);           /* 3.14159 */

/* StrDoubleToBuf returns a signed length; negative means "did not fit". */
int32_t written = StrDoubleToBuf(dbuf, sizeof(dbuf), 3.14159265359);
if (written > 0) {
    printf("%.*s\n", written, dbuf);
}
```

### Pattern: Inspect a databox

```c
if (DATABOX_IS_SIGNED_INTEGER(&box)) {
    printf("signed: %" PRId64 "\n", box.data.i64);
} else if (DATABOX_IS_UNSIGNED_INTEGER(&box)) {
    printf("unsigned: %" PRIu64 "\n", box.data.u64);
} else if (DATABOX_IS_FLOAT(&box)) {
    printf("float: %f\n", box.data.d64);
} else if (DATABOX_IS_BYTES(&box)) {
    printf("bytes: %.*s\n", (int)databoxLen(&box),
           (const char *)databoxCBytes(&box));
} else if (DATABOX_IS_NULL(&box)) {
    printf("null\n");
}
```

`DATABOX_IS_BYTES` covers both the pointer and the embedded (≤ 8 byte)
representations, and `databoxCBytes` picks the right one — never read
`data.bytes.cstart` directly.

---

## Set and Bitmap Operations

### Pattern: intset for Sparse Signed Integers

`intset` is a sorted, width-adaptive array (2 / 4 / 8 bytes per element,
upgraded automatically). It has no built-in set algebra, so combine by
walking:

```c
static intset *setUnion(intset *a, intset *b) {
    intset *result = intsetNew();

    for (size_t i = 0; i < intsetCount(a); i++) {
        int64_t v;
        intsetGet(a, (uint32_t)i, &v);
        intsetAdd(&result, v, NULL);
    }

    for (size_t i = 0; i < intsetCount(b); i++) {
        int64_t v;
        intsetGet(b, (uint32_t)i, &v);
        intsetAdd(&result, v, NULL);  /* duplicates are ignored */
    }

    return result;
}

static intset *setIntersect(intset *a, intset *b) {
    intset *result = intsetNew();

    for (size_t i = 0; i < intsetCount(a); i++) {
        int64_t v;
        intsetGet(a, (uint32_t)i, &v);
        if (intsetFind(b, v)) {
            intsetAdd(&result, v, NULL);
        }
    }

    return result;
}
```

Walk the _smaller_ set and probe the larger one — `intsetFind` is a
branchless binary search, so the probe side should be the long one.

### Pattern: multiroar for Dense Unsigned ID Sets

When your members are dense unsigned IDs, `multiroar` does the set
algebra for you and stores it compressed:

```c
#include "multiroar.h"

multiroar *active = multiroarBitNew();
multiroar *premium = multiroarBitNew();

multiroarBitSet(active, 1001);
multiroarBitSet(active, 1002);
multiroarBitSet(active, 1003);

multiroarBitSet(premium, 1002);
multiroarBitSet(premium, 1003);
multiroarBitSet(premium, 1004);

multiroar *all = multiroarNewOr(active, premium);
multiroar *activePremium = multiroarNewAnd(active, premium);
multiroar *activeFree = multiroarNewAndNot(active, premium);
multiroar *exclusive = multiroarNewXor(active, premium);

printf("active+premium: %" PRIu64 "\n", multiroarBitCount(activePremium));
printf("bitmap bytes:   %" PRIu64 "\n", multiroarMemoryUsage(all));

multiroarFree(active);
multiroarFree(premium);
multiroarFree(all);
multiroarFree(activePremium);
multiroarFree(activeFree);
multiroarFree(exclusive);
```

Use `multiroarNewAndNot(a, b)` for set difference rather than composing
`multiroarNewNot` with `multiroarNewAnd`: `multiroarNot` only flips
chunks the bitmap has already materialized, so the two-step version is
not a true set difference — and the complement of a sparse bitmap is
dense, so it would also materialize an enormous intermediate.

Bulk loading has its own entry points — `multiroarBitSetMany`,
`multiroarFromArray`, `multiroarBitSetRange` — which batch far better
than a `multiroarBitSet` loop.

To enumerate members:

```c
multiroarIterator it;
multiroarIteratorInit(all, &it);

uint64_t position;
while (multiroarIteratorNext(&it, &position)) {
    printf("%" PRIu64 " ", position);
}
```

---

## String Building

`mds` is datakit's growable string. It carries its own length, so it
handles embedded NUL bytes, and every mutator returns the (possibly
moved) buffer — always reassign:

```c
#include "mds.h"

mds *s = mdsempty();

s = mdscat(s, "Hello");
s = mdscat(s, " ");
s = mdscat(s, "World");
s = mdscatprintf(s, " %" PRId64, INT64_C(42));

printf("%.*s\n", (int)mdslen(s), s);  /* Hello World 42 */

mdsfree(s);
```

`mdsc` (via `mdsc.h`) is the same API with a compact header — cheaper per
string, so prefer it when you hold millions of short strings. Pre-sizing
with `mdsemptylen(n)` avoids the growth steps entirely when you know the
final length.

---

## Compression

### Pattern: Transparent Node Compression with mflex

```c
#include "mflex.h"

mflexState *state = mflexStateCreate();
mflex *m = mflexNew();

for (uint64_t i = 0; i < 100000; i++) {
    mflexPushDouble(&m, state, (double)i, FLEX_ENDPOINT_TAIL);
}

if (mflexIsCompressed(m)) {
    const double ratio =
        (double)mflexBytesUncompressed(m) / mflexBytesActual(m);
    printf("compressed %.2fx\n", ratio);
}

/* Open decompresses into state's scratch buffer; Close puts it back. */
flex *f = mflexOpen(m, state);
/* ... work with f as an ordinary flex ... */
mflexCloseGrow(&m, state, f);

mflexFree(m);
mflexStateFree(state);
```

Push operations take `mflex **` because a push can replace the handle
with a compressed representation.

### Pattern: Delta-of-Delta for Monotonic Integers

`dod` is a bit-packed stream, not a container: you own the backing
buffer, encode with `dodAppend` after seeding two values, and decode
sequentially with a `dodReader`.

```c
#include "dod.h"

enum { COUNT = 10000 };

/* Two dod words per value is always enough headroom. */
dod *bits = zcalloc(COUNT * 2, sizeof(*bits));

int64_t base = 1700000000;
dodVal t0 = base;
dodVal t1 = base + 60;

size_t usedBits = 0;
for (size_t i = 2; i < COUNT; i++) {
    const dodVal next = base + (int64_t)i * 60;  /* one sample per minute */
    dodAppend(bits, t0, t1, next, &usedBits);
    t0 = t1;
    t1 = next;
}

printf("encoded %d timestamps in %zu bytes (raw would be %zu)\n",
       COUNT, (usedBits + 7) / 8, COUNT * sizeof(int64_t));

/* Decode: the reader is seeded with the same first two values. */
dodReader r;
dodReaderInit(&r, base, base + 60);
for (size_t i = 2; i < COUNT; i++) {
    const dodVal v = dodReaderNext(&r, bits);
    (void)v;
}

zfree(bits);
```

`dodReaderNextN` decodes a batch into an array, and `dodReaderRemaining`
reports how many values are left given a known total.

### Pattern: XOR Compression for Float Series

`xof` follows the same shape for doubles: seed with `xofInit`, extend
with `xofAppend` carrying the previous value and zero-run state, decode
with an `xofReader`.

```c
#include "xof.h"

enum { COUNT = 10000 };
xof *bits = zcalloc(COUNT * 2, sizeof(*bits));
double *values = zcalloc(COUNT, sizeof(*values));

values[0] = 20.0;
for (size_t i = 1; i < COUNT; i++) {
    values[i] = values[i - 1] + 0.1;
}

size_t usedBits = 0;
xofInit(bits, &usedBits, values[0]);

int_fast32_t prevLeadingZeroes = -1;
int_fast32_t prevTrailingZeroes = -1;
for (size_t i = 1; i < COUNT; i++) {
    xofAppend(bits, &usedBits, &prevLeadingZeroes, &prevTrailingZeroes,
              values[i - 1], values[i]);
}

xofReader r;
xofReaderInit(&r, bits);
double first = xofReaderCurrent(&r);
for (size_t i = 1; i < COUNT; i++) {
    double v = xofReaderNext(&r, bits);
    (void)v;
}
(void)first;

zfree(bits);
zfree(values);
```

Slowly-varying series compress best: consecutive values share leading
mantissa bits, so each XOR result is mostly zeros.

Details for both codecs: `docs/modules/compression/DOD.md` and
`docs/modules/compression/XOF.md`.

---

## Timing and Randomness

### Pattern: Use timeUtil, Not gettimeofday

`timeUtil` gives whole-number wall and monotonic clocks. Use the
**monotonic** clocks for durations — wall clocks step when the system
clock is adjusted:

```c
#include "timeUtil.h"
#include <inttypes.h>

const uint64_t startNs = timeUtilMonotonicNs();
doWork();
const uint64_t elapsedNs = timeUtilMonotonicNs() - startNs;

printf("%" PRIu64 " ns (%.3f ms)\n", elapsedNs, elapsedNs / 1e6);
```

`timeUtilNs` / `Us` / `Ms` / `S` are the wall-clock family — use those
for timestamps you store, and `timeUtilMonotonicNs` / `Us` / `Ms` for
anything you subtract.

### Pattern: Use the str.h Generators for Non-Cryptographic Randomness

```c
#include "str.h"

/* Seed once from any nonzero source; splitmix64 spreads a weak seed. */
uint64_t seedState = timeUtilNs();
uint64_t s[2] = {splitmix64(&seedState), splitmix64(&seedState)};

for (size_t i = 0; i < 1000; i++) {
    const uint64_t r = xoroshiro128plus(s);
    const uint64_t id = r % 5000;
    /* ... */
}
```

These are fast and reproducible from a seed, which is exactly what test
data generation and benchmarks want. They are **not** cryptographic — key
material comes from the crypto module, never from here.

---

## Summary

1. Constructors return the container; mutators take its address.
2. `*Free` never clears your pointer.
3. Let `multimap` and `multilist` pick their own internal variant.
4. Iterate with iterators; use `flexHeadIfAny` as a loop initializer.
5. Use the `*Into` readers instead of hand-wiring `databox *` arrays.
6. Know whether a read gave you a reference or an owned copy.
7. `multilist` iterators need a two-slot `mflexState` array.
8. Print boxes with `%.*s`, never `%s`.
9. Reach for `timeUtil`, `mds`, `fibbuf`/`jebuf`, and `str.h` generators
   before hand-rolling clocks, buffers, growth, or RNGs.

Next:

- [USE_CASES.md](USE_CASES.md) — complete applications
- [BENCHMARKS.md](BENCHMARKS.md) — how to measure
- `docs/modules/` — per-module reference and deep examples
