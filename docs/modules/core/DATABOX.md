# databox - Universal 16-Byte Container

## Overview

`databox` is a **universal 16-byte fixed-size container** that can hold any data type through a tagged union. It provides type-safe polymorphic storage without heap allocation for most common types.

**Key Features:**

- Fixed 16-byte size for predictable memory layout
- Type tagging for runtime type safety
- Inline storage for small values (≤ 8 bytes)
- Automatic embedding optimization for small byte arrays
- Support for integers, floats, pointers, strings, and containers
- Optional 32-byte `databoxBig` variant for 128-bit integers

**Header**: `databox.h`

**Source**: `databox.c`

## Data Structure

```c
/* 16-byte databox structure */
typedef struct databox {
    databoxUnion data;  /* 8 bytes - the actual value */
    uint64_t type : 8;      /* Type tag (databoxType enum) */
    uint64_t allocated : 1;  /* True if data.bytes.start needs freeing */
    uint64_t created : 1;    /* True if retrieved and created */
    uint64_t big : 1;        /* True if actually a databoxBig */
    uint64_t unused : 5;     /* Reserved for future use */
    uint64_t len : 48;       /* Length of data (up to 281 TB!) */
} databox;

/* The 8-byte data union */
typedef union databoxUnion {
    int8_t i8;
    uint8_t u8;
    int16_t i16;
    uint16_t u16;
    int32_t i32;
    uint32_t u32;
    int64_t i64;
    uint64_t u64;
    intmax_t i;
    uintmax_t u;
    float f32;
    double d64;

    /* For 128-bit integers (in databoxBig) */
    __int128_t *i128;
    __uint128_t *u128;

    /* For bytes/strings */
    union {
        const char *ccstart;
        char *cstart;
        char cembed[8];
        const uint8_t *custart;
        uint8_t *start;
        uint8_t embed[8];     /* Inline storage! */
        uint8_t embed4[4];
        uint8_t embed8[8];
        size_t offset;
    } bytes;

    /* For pointers */
    void *ptr;
    uintptr_t uptr;
} databoxUnion;
```

### Metadata Flags

The four metadata bits live in the same 8 bytes as `type` and `len`:

| Flag        | Meaning                                                                                                          |
| ----------- | ---------------------------------------------------------------------------------------------------------------- |
| `allocated` | the box owns heap memory behind `data` (BYTES, a 96/128-bit payload, MDSC); `databoxFreeData` will free it       |
| `created`   | the box was created by a retrieve-and-create path                                                                |
| `big`       | `data.i128`/`u128` is a live pointer to 16 bytes of storage (a `databoxBig`'s `extra`, or an owned heap payload) |
| `unused`    | 5 reserved bits                                                                                                  |

**Zero-initialize boxes you hand to a fill-in API.** Container getters
(`flexGetByType`, and the multimap getters layered on it) write `type`,
`data`, and `len` but deliberately do **not** clear the flag bits — clearing
them on every element read would be pure cost on the hot path. A box declared
without an initializer therefore carries a garbage `allocated` bit into
`databoxFreeData`. Always start from a zeroed box:

```c
databox box = {{0}};          /* zeroes data, type, flags, and len */
flexGetByType(fe, &box);
```

The same applies to boxes filled by the `databoxNew*` constructors before
they are inspected for ownership: `databoxNewBytes`, `databoxNewBytesAllowEmbed`,
`databoxNewOffsetAllowEmbed`, and the embed arm of
`databoxNewBytesAllocateOrEmbed` set only `type`/`data`/`len`, so testing
`.allocated` on their result is not meaningful. Track ownership from the
constructor you called, or zero the box and use the update macros.

## Type System

### Supported Types

```c
typedef enum databoxType {
    /* Special values */
    DATABOX_VOID = 0,         /* Undefined/empty */
    DATABOX_ERROR = 1,        /* User-defined error */
    DATABOX_TRUE = 13,        /* Boolean true */
    DATABOX_FALSE = 14,       /* Boolean false */
    DATABOX_NULL = 15,        /* NULL value */

    /* Numeric types - fixed 8 bytes */
    DATABOX_SIGNED_64 = 2,    /* int64_t */
    DATABOX_UNSIGNED_64 = 3,  /* uint64_t */
    DATABOX_FLOAT_32 = 6,     /* float */
    DATABOX_DOUBLE_64 = 7,    /* double */

    /* 128-bit types (databoxBig only) */
    DATABOX_SIGNED_128 = 4,   /* __int128_t */
    DATABOX_UNSIGNED_128 = 5, /* __uint128_t */

    /* Linear data structure markers */
    DATABOX_ARRAY_START = 8,
    DATABOX_ARRAY_END = 9,
    DATABOX_MAP_START = 10,   /* implicit end because Key-Value */
    DATABOX_LIST_START = 11,
    DATABOX_LIST_END = 12,

    /* Pointer type */
    DATABOX_PTR = 16,         /* void * */

    /* Byte/string types */
    DATABOX_BYTES = 18,            /* Pointer to bytes (not owned) */
    DATABOX_BYTES_EMBED = 19,      /* ≤8 bytes stored inline */
    DATABOX_BYTES_VOID = 20,       /* Pre-allocated space */
    DATABOX_BYTES_NEVER_FREE = 21, /* Bytes we don't own */
    DATABOX_BYTES_OFFSET = 22,     /* Offset into external buffer */

    /* Container types */
    DATABOX_CONTAINER_REFERENCE_EXTERNAL = 17,
    DATABOX_CONTAINER_FLEX_MAP = 23,
    DATABOX_CONTAINER_FLEX_LIST = 24,
    DATABOX_CONTAINER_FLEX_SET = 25,
    DATABOX_CONTAINER_FLEX_TUPLE = 26,
    DATABOX_CONTAINER_CFLEX_MAP = 27,
    DATABOX_CONTAINER_CFLEX_LIST = 28,
    DATABOX_CONTAINER_CFLEX_SET = 29,
    DATABOX_CONTAINER_CFLEX_TUPLE = 30,

    DATABOX_PTR_MDSC = 31,    /* only when DATABOX_ENABLE_PTR_MDSC */

    DATABOX_MAX = 255         /* the type field is ONE byte */
} databoxType;
```

Ordering is load-bearing in three places, so do not renumber:

- `SIGNED_64` / `UNSIGNED_64` / `SIGNED_128` / `UNSIGNED_128` / `FLOAT_32` /
  `DOUBLE_64` must stay adjacent at 2..7 (range checks and numeric-class
  compares depend on it).
- `type >= DATABOX_BYTES` is the "has a `len`" test, so every
  variable-length and container type sits at or above 18.
- `DATABOX_CONTAINER_OFFSET(encoding)` is
  `encoding - DATABOX_CONTAINER_FLEX_MAP`, so the four FLEX container types
  stay in the order of flex's `FLEX_CONTAINER_MAP`..`_TUPLE`.  (The four
  CFLEX types have no flex encoding yet; flex refuses them.)

## Creation Functions

### Numeric Types

```c
/* Create from integer */
databox databoxNewSigned(int64_t val);
databox databoxNewUnsigned(uint64_t val);

/* Create from floating point */
databox databoxNewReal(double value);

/* Example usage */
databox num = databoxNewSigned(-42);
databox unum = databoxNewUnsigned(UINT64_C(12345));
databox pi = databoxNewReal(3.14159);
```

`databoxNewSigned`, `databoxNewUnsigned`, and `databoxNewPtr` are `static
inline` in the header: they appear on hot boxing paths (flex/multimap
inserts) where a call per element would dominate the work.

`databoxNewReal` narrows: a `double` that round-trips exactly through
`float` is stored as `DATABOX_FLOAT_32`, otherwise as `DATABOX_DOUBLE_64`.
Test with `DATABOX_IS_FLOAT()` (which covers both) rather than assuming
`DATABOX_DOUBLE_64`.

### Boolean and NULL

```c
/* Create boolean values */
databox databoxBool(bool which);
databox databoxNull(void);
databox databoxVoid(void);

/* Predefined constants */
extern const databox DATABOX_BOX_TRUE;
extern const databox DATABOX_BOX_FALSE;
extern const databox DATABOX_BOX_NULL;
extern const databox DATABOX_BOX_VOID;

/* Example usage */
databox t = databoxBool(true);
databox f = DATABOX_BOX_FALSE;
databox n = databoxNull();
```

### Byte Arrays and Strings

```c
/* Create from bytes (reference only - no copy) */
databox databoxNewBytes(const void *ptr, size_t len);

/* Create from C string */
databox databoxNewBytesString(const char *str);

/* Create with automatic embedding for small data */
databox databoxNewBytesAllowEmbed(const void *ptr, size_t len);

/* Create with allocation or embedding */
databox databoxNewBytesAllocateOrEmbed(const void *ptr, size_t len);

/* Create offset-based reference */
databox databoxNewOffsetAllowEmbed(const void *ptr, size_t offset, size_t len);

/* Copy 'len' bytes into freshly allocated storage owned by 'box' */
void databoxCopyBytes(databox *box, const void *ptr, size_t len);

/* Example usage */
const char *msg = "Hello, World!";
databox str1 = databoxNewBytesString(msg);  /* 13 bytes - not embedded */

const char *tiny = "Hi";
databox str2 = databoxNewBytesAllowEmbed(tiny, 2);  /* 2 bytes - EMBEDDED! */

/* Embedded storage means no heap allocation */
assert(str2.type == DATABOX_BYTES_EMBED);
```

Which constructor copies:

| Constructor                      | Copies?                             | `allocated`   |
| -------------------------------- | ----------------------------------- | ------------- |
| `databoxNewBytes`                | no, points at caller memory         | untouched     |
| `databoxNewBytesString`          | no (`databoxNewBytes` + `strlen`)   | untouched     |
| `databoxNewBytesAllowEmbed`      | only when `len <= 8` (into `embed`) | untouched     |
| `databoxNewBytesAllocateOrEmbed` | always (embed when `len <= 8`)      | set when heap |
| `databoxCopyBytes`               | always (`zmalloc`)                  | set           |

### Bytes update macros

```c
/* Embed when <= 8 bytes, otherwise reference the caller's pointer */
databoxUpdateBytesAllowEmbed(box, ptr, len);

/* Same, but store an OFFSET into a base buffer for the non-embed case */
databoxUpdateOffsetAllowEmbed(box, ptr, offset, len);

/* Resolve an offset box against its base buffer */
databoxOffsetBoxToRealBox(box, base);

/* Hand ownership of an existing heap buffer to the box: sets
 * type/data/len/allocated in one step so databoxFreeData() releases it. */
databoxUpdateBytesOwned(box, ptr, len);

/* Fill an iovec from a box (base + len) */
databoxToIovec(box, iov);
```

Use `databoxUpdateBytesOwned` instead of hand-writing the four-field
assignment whenever you already hold a heap pointer (an `snprintf` result,
parser output, and similar).

### Pointers

```c
/* Create from pointer */
databox databoxNewPtr(void *ptr);          /* type = DATABOX_PTR */

/* Box a pointer as DATABOX_UNSIGNED_64 instead of DATABOX_PTR */
databox databoxNewPtrAsUnsigned(const void *ptr);
void *databoxRetrievePtrUnsigned(const databox *box);

/* Example usage */
int *mydata = zmalloc(sizeof(int) * 100);
databox ptrbox = databoxNewPtr(mydata);

/* Later retrieve it */
void *retrieved = ptrbox.data.ptr;
```

**Sorted containers want `databoxNewPtrAsUnsigned`.** flex storage coerces
`DATABOX_PTR` to unsigned, so a pointer stored in a sorted container comes
back out of `flexGetByType` as `DATABOX_UNSIGNED_64`. Boxing it as unsigned
up front keeps every element of an entry in one compare class and keeps the
live box identical to the decoded one. Retrieve with
`databoxRetrievePtrUnsigned()`, which is correct for both. (`databoxCompare`
itself also coerces a `DATABOX_PTR` operand into the unsigned integer order
so mixed comparisons stay transitive — but the type tag still differs, so
prefer boxing as unsigned.) In-memory use only: a stored pointer is
meaningless once persisted.

## Macros for Direct Creation

```c
/* Compound literal macros for quick initialization */
#define DATABOX_SIGNED(d)    (databox){ .data.i64 = (d), .type = DATABOX_SIGNED_64 }
#define DATABOX_UNSIGNED(d)  (databox){ .data.u64 = (d), .type = DATABOX_UNSIGNED_64 }
#define DATABOX_DOUBLE(d)    (databox){ .data.d64 = (d), .type = DATABOX_DOUBLE_64 }
#define DATABOX_WITH_BYTES(b, l) (databox){ .data.bytes.start = (b), .type = DATABOX_BYTES, .len = (l) }

/* Special float values */
#define DATABOX_NAN              /* NaN double */
#define DATABOX_INFINITY_POSITIVE /* +∞ */
#define DATABOX_INFINITY_NEGATIVE /* -∞ */

/* Example usage */
databox nums[] = {
    DATABOX_SIGNED(-100),
    DATABOX_UNSIGNED(200),
    DATABOX_DOUBLE(2.718281828),
    DATABOX_NAN
};
```

## Macros for In-Place Modification

```c
/* Set values directly in existing databox */
#define DATABOX_SET_SIGNED(box, d)    /* Set as signed integer */
#define DATABOX_SET_UNSIGNED(box, d)  /* Set as unsigned integer */
#define DATABOX_SET_DOUBLE(box, d)    /* Set as double */
#define DATABOX_SET_FLOAT(box, d)     /* Set as float */

/* Example usage */
databox box;
DATABOX_SET_SIGNED(&box, -42);
assert(box.type == DATABOX_SIGNED_64);
assert(box.data.i64 == -42);

DATABOX_SET_DOUBLE(&box, 3.14);
assert(box.type == DATABOX_DOUBLE_64);
assert(box.data.d64 == 3.14);
```

## Type Checking Macros

```c
/* Check specific types */
#define DATABOX_IS_BYTES_EMBED(box)      /* True if embedded bytes */
#define DATABOX_IS_BYTES(box)            /* True if bytes (embed or ptr) */
#define DATABOX_IS_INTEGER(box)          /* True if signed or unsigned */
#define DATABOX_IS_SIGNED_INTEGER(box)   /* True if signed */
#define DATABOX_IS_UNSIGNED_INTEGER(box) /* True if unsigned */
#define DATABOX_IS_FLOAT(box)            /* True if float or double */
#define DATABOX_IS_NUMERIC(box)          /* True if integer or float */
#define DATABOX_IS_PTR(box)              /* True if DATABOX_PTR */
#define DATABOX_IS_REF(box)              /* True if external container reference */
#define DATABOX_IS_TRUE(box)             /* True if DATABOX_TRUE */
#define DATABOX_IS_FALSE(box)            /* True if DATABOX_FALSE */
#define DATABOX_IS_NULL(box)             /* True if DATABOX_NULL */
#define DATABOX_IS_VOID(box)             /* True if DATABOX_VOID */
#define DATABOX_IS_FIXED(box)            /* True if fixed-size type */

/* Boolean evaluation (always exact inverses of each other) */
#define DATABOX_IS_TRUEISH(box)   /* True if not false/null/0 */
#define DATABOX_IS_FALSEISH(box)  /* True if false/null/0 */

/* Size of the value: 'len' for BYTES and containers, else 8 */
#define DATABOX_SIZE(box)

/* Index of a container encoding within its family (FLEX_MAP == 0) */
#define DATABOX_CONTAINER_OFFSET(encoding)

/* Example usage */
databox num = databoxNewSigned(42);
if (DATABOX_IS_INTEGER(&num)) {
    printf("It's an integer!\n");
}
if (DATABOX_IS_NUMERIC(&num)) {
    printf("It's a number!\n");
}

databox str = databoxNewBytesAllowEmbed("hi", 2);
if (DATABOX_IS_BYTES_EMBED(&str)) {
    printf("Embedded! No heap allocation\n");
}
```

## Accessing Data

### Getting Bytes

```c
/* Get bytes from databox */
bool databoxGetBytes(databox *box, uint8_t **buf, size_t *len);

/* Get size only */
bool databoxGetSize(const databox *box, size_t *len);
size_t databoxGetSizeMinimum(const databox *box);

/* Macros for direct access */
#define databoxLen(box)        /* Get length */
#define databoxBytes(box)      /* Get byte pointer (embed or ptr) */
#define databoxCBytes(box)     /* Get const byte pointer */
#define databoxBytesEmbed(box) /* Get embedded bytes directly */

/* Example usage */
databox str = databoxNewBytesString("Hello");

uint8_t *bytes;
size_t len;
if (databoxGetBytes(&str, &bytes, &len)) {
    printf("String: %.*s (length: %zu)\n", (int)len, bytes, len);
}

/* Direct macro access */
printf("Length: %zu\n", databoxLen(&str));
const uint8_t *data = databoxCBytes(&str);
```

`databoxGetBytes` / `databoxGetSize` serve every box that carries bytes
(`BYTES`, embedded, `BYTES_NEVER_FREE`, `PTR_MDSC`, flex aggregates) and the
value bytes of 64-bit integers and reals; they return `false` for types with
no byte form (booleans, null, 96/128-bit, void, offset).

### Accessing Numeric Values

```c
/* Access values directly through union */
databox num = databoxNewSigned(-42);
int64_t val = num.data.i64;
printf("Value: %" PRId64 "\n", val);

databox pi = databoxNewReal(3.14159);
/* databoxNewReal narrows to FLOAT_32 when the value round-trips exactly */
double d = (pi.type == DATABOX_FLOAT_32) ? pi.data.f32 : pi.data.d64;
printf("Pi: %.5f\n", d);

/* Type-safe access with checking */
databox unknown = getSomeBox();
if (DATABOX_IS_SIGNED_INTEGER(&unknown)) {
    printf("Signed: %" PRId64 "\n", unknown.data.i64);
} else if (DATABOX_IS_UNSIGNED_INTEGER(&unknown)) {
    printf("Unsigned: %" PRIu64 "\n", unknown.data.u64);
} else if (DATABOX_IS_FLOAT(&unknown)) {
    /* DATABOX_IS_FLOAT covers both FLOAT_32 and DOUBLE_64 */
    const double value = (unknown.type == DATABOX_FLOAT_32) ? unknown.data.f32
                                                            : unknown.data.d64;
    printf("Real: %f\n", value);
}
```

There is no `DATABOX_IS_DOUBLE` macro: use `DATABOX_IS_FLOAT` (float **or**
double) and branch on `type == DATABOX_FLOAT_32` when the distinction
matters.

## Memory Management

### Copying databoxes

```c
/* Deep copy a databox */
databox databoxCopy(const databox *src);

/* Copy bytes from one databox to another */
void databoxCopyBytesFromBox(databox *dst, const databox *src);

/* Allocate memory for bytes if needed */
bool databoxAllocateIfNeeded(databox *box);

/* Example usage */
databox original = databoxNewBytesString("Original");
databox copy = databoxCopy(&original);

/* copy now has its own memory */
databoxFreeData(&copy);
/* original is still valid; it never owned the bytes */
```

All three give a copy independent of the source's storage, by type:

| Source                                  | Copy                                                    |
| --------------------------------------- | ------------------------------------------------------- |
| `BYTES`, `BYTES_NEVER_FREE`             | owned `BYTES` on the heap, or `BYTES_EMBED` if ≤ 8 bytes |
| `BYTES_EMBED`                           | itself (the bytes ride inside the box)                  |
| flex aggregate (`CONTAINER_FLEX_*`/`CFLEX_*`) | a heap copy under the same type                   |
| `PTR_MDSC`                              | a new mdsc string                                       |
| `BYTES_VOID`, `BYTES_OFFSET`            | not copied (no bytes of their own)                      |

Release any copy with `databoxFreeData`. A copy can come out cheaper than
its source (a short string embeds). `databoxAllocateIfNeeded` returns
`true` only when the box now owns a new copy, and leaves a box that already
owns its bytes alone. Resolve an offset box with
`databoxOffsetBoxToRealBox()` before copying it.

A 96/128-bit value is owned the same way as BYTES. All three copy its
16-byte payload to a heap block and set `allocated = 1` and `big = 1`, so
the copy survives the source (a `databoxBig`, a multilist entry, a flex)
going away; release it with `databoxFreeData`. Every other copy result has
`big = 0`. A type-only wide box (`data.ptr == NULL`, as a decode into a
plain box reports it) stays type-only, and `databoxAllocateIfNeeded` leaves
an already-owned wide payload alone.

```c
/* 'borrowed' points into storage that is about to change or go away */
databox kept = databoxCopy(borrowed);  /* heap-owned if 96/128-bit */
/* ... */
databoxFreeData(&kept);

/* Or keep it without allocating, in a databoxBig you own */
databoxBig keptBig;
databoxBigCopyFrom(&keptBig, borrowed);
```

`databoxBigCopyFrom(dst, src)` copies `src` into a `databoxBig` without
allocating: a wide payload is copied into `dst->extra` and the pointer is
aimed there (`big = 1`); any other value gets `big = 0`. `dst` never owns
anything (`allocated = 0`): BYTES keep borrowing from wherever `src` points,
and `src` keeps whatever it owned. `src` may alias `dst`.

### Freeing databoxes

```c
/* Release the data the box owns; the box struct itself survives */
void databoxFreeData(databox *box);

/* databoxFreeData() AND zfree(box) — for a HEAP-ALLOCATED databox */
void databoxFree(databox *box);
```

**These two are not interchangeable.** `databoxFree()` calls
`databoxFreeData()` and then `zfree(box)`, so it must only be handed a
pointer to a `databox` that was itself heap-allocated. Passing it the
address of a stack or embedded `databox` frees a non-heap pointer.

```c
/* Stack box: release owned data only */
databox str = {{0}};
databoxCopyBytes(&str, "a longer string than eight bytes", 32);
databoxFreeData(&str);        /* frees the bytes, str becomes VOID */

/* Heap box: release data AND the box */
databox *heapBox = zcalloc(1, sizeof(*heapBox));
databoxCopyBytes(heapBox, payload, payloadLen);
databoxFree(heapBox);         /* frees bytes, then frees heapBox */
```

`databoxFreeData` only releases when `allocated` is set **and** the type is
`DATABOX_BYTES`, `DATABOX_SIGNED_128` / `DATABOX_UNSIGNED_128` (a heap
payload from one of the copy helpers or `flexGetByTypeCopy`), or
`DATABOX_PTR_MDSC` (released with `mdscfree`). A `databoxBig` whose payload
pointer aims at its own `extra` holds the value inline and is left alone,
even if a stale `allocated` bit is set. It is a no-op for every other type,
so calling it on an embedded or reference box is always safe:

```c
/* Embedded databoxes hold no heap memory */
databox tiny = databoxNewBytesAllowEmbed("Hi", 2);
assert(tiny.type == DATABOX_BYTES_EMBED);
databoxFreeData(&tiny);       /* safe no-op */
```

### Retain Cache (Advanced)

```c
/* Cache for efficient memory reuse */
typedef struct databoxRetainCache {
    uint8_t *bytes[16];  /* Sizes: 128, 256, 512, ..., 4MB */
} databoxRetainCache;

/* Retain bytes using cache */
ssize_t databoxRetainBytesSelf(databox *dst, databoxRetainCache *cache);

/* Pre-sized variant: caller already sized and supplied the destination */
void databoxRetainBytesSelfExact(databox *dst, void *bytes);

/* Example usage */
databoxRetainCache cache = {0};

/* Pre-allocate cache slots (slot i holds 128 << i bytes) */
for (size_t i = 0; i < COUNT_ARRAY(cache.bytes); i++) {
    cache.bytes[i] = zmalloc((size_t)128 << i);
}

/* Use cache for efficient copying */
databox box = {{0}};
box = databoxNewBytes(data, 1000);
const ssize_t slot = databoxRetainBytesSelf(&box, &cache);

/* Clean up cache */
for (size_t i = 0; i < COUNT_ARRAY(cache.bytes); i++) {
    zfree(cache.bytes[i]);
}
```

Return value of `databoxRetainBytesSelf`:

| Result | Meaning                                                                                  |
| ------ | ---------------------------------------------------------------------------------------- |
| `>= 0` | copied into `cache.bytes[slot]`; the box now points at **cache** memory                  |
| `-1`   | retained privately: re-embedded (`len <= 8`), or `zmalloc`'d (no populated slot fits, larger than the largest slot included); an mdsc string or flex aggregate copied under its own type |
| `-2`   | nothing done: the box references no external bytes (non-byte types, embedded bytes, VOID, OFFSET) or already owns its mdsc string / aggregate |

Plain bytes (`BYTES`, `BYTES_NEVER_FREE`) come back as `BYTES`.

A non-negative result means the box borrows the cache buffer: `allocated`
stays false, so `databoxFreeData()` on it is a no-op and the cache keeps
ownership. The box's bytes change when that slot is reused, so copy or
reset the box before then.

`databoxRetainBytesSelfExact` presupposes `DATABOX_BYTES` and that the
caller has already sized `bytes` to `box->len`; it still prefers embedding
when `len <= 8`, in which case `bytes` goes unused.

Neither function handles `DATABOX_BYTES_OFFSET` boxes.

## Comparison and Equality

```c
/* Compare two databoxes */
int databoxCompare(const databox *a, const databox *b);

/* Check equality */
bool databoxEqual(const databox *a, const databox *b);

/* Example usage */
databox a = databoxNewBytesString("apple");
databox b = databoxNewBytesString("banana");
databox c = databoxNewBytesString("apple");

int cmp = databoxCompare(&a, &b);
if (cmp < 0) {
    printf("a < b\n");  /* "apple" < "banana" */
}

if (databoxEqual(&a, &c)) {
    printf("a equals c\n");  /* Both "apple" */
}

/* Numeric comparison */
databox n1 = databoxNewSigned(100);
databox n2 = databoxNewSigned(200);
assert(databoxCompare(&n1, &n2) < 0);
```

`databoxEqual` is exactly `databoxCompare(a, b) == 0`.

### Hashing numbers by value

```c
/* true, with a 64-bit key in *key, for a numeric box; false otherwise */
bool databoxNumericHashKey(const databox *box, uint64_t *key);
```

A hash table that uses `databoxCompare` for equality must hash numbers so
that equal values hash alike, whatever their boxes: `22`, `22.0f`, `22.0`
and a 128-bit 22 are one key. `databoxNumericHashKey` gives that key for
`SIGNED_64`, `UNSIGNED_64`, 96/128-bit boxes (with a payload; a deferred box
aborts), `FLOAT_32`, `DOUBLE_64`, `DATABOX_PTR` and
`DATABOX_CONTAINER_REFERENCE_EXTERNAL`; it returns false for every other
type, which the caller hashes by bytes.

- An integer in the int64 range keys as its int64 bits, in any box,
  integral reals included.
- Other integers, up to the 128-bit range, key as a mix of their sign and
  magnitude, in any box.
- `-0.0` keys as 0; every NaN shares one key.
- Any other real keys as a mix of its double bits.

Unequal values may share a key: it is a hash input, not an identity.

### Compare classes

- **Bytes vs bytes** — natural sort, not plain `memcmp`. A run of ASCII
  digits compares as a number: the longer run is the larger value, and
  equal-length runs fall back to `memcmp` over the digit bytes. Everything
  else compares byte-by-byte. So `"file2.txt" < "file10.txt"`, and
  `"120abc" < "120zzz"`. The ordering is a strict total order (run-length
  first is transitive), which is what makes it safe as the binary-search
  comparator inside flex and multimap.
- **Numeric vs numeric** — signed, unsigned, 96/128-bit, float, and double
  all cross-compare by exact mathematical value, in either direction, on every
  platform: an integer above 2^53 against a double never goes through a
  rounding conversion. The order is a strict weak order:
  - `-0.0` equals `+0.0` (and the integer 0);
  - `-inf` and `+inf` sit below and above every finite value;
  - NaN sorts above `+inf`. Every NaN, whatever its sign or payload, float
    or double, is the same position in this order, so containers, which find
    keys with `databoxCompare(...) == 0`, can find and delete a NaN key.

`databoxCompare` is the identity order. For value equality use
`databoxEqual`, which follows IEEE: NaN equals nothing, itself included, and
`-0.0` equals `+0.0`. This is the same split as Java's `Double.equals` (keys)
versus `==` (values).

- **`DATABOX_PTR`** and **`DATABOX_CONTAINER_REFERENCE_EXTERNAL`** — join
  the numeric class as unsigned integers (raw pointer bits, or the
  reference id), against every numeric type. flex stores a pointer as an
  unsigned integer and decodes it back as `DATABOX_UNSIGNED_64`, and a
  container must not disagree with itself; joining the whole numeric class,
  reals included, keeps the order transitive.
- **Everything else** (`TRUE`, `FALSE`, `NULL`, bytes-vs-number, …) — equal
  only to its own type; mismatched types sort by the global type order.

Comparing a `DATABOX_VOID` box asserts in debug builds.

## Canonical Numerals and Minimal Storage

These functions give a value its minimal-storage form: a byte string that
spells a number becomes that number, and a real holding a whole number
becomes that integer. They are an explicit, opt-in conversion: flex's
auto-convert push path (`flexPushBytes`, `flexPushByType`, ...) uses them,
while flex's exact path, every datakit container (multilist, multimap,
multidict, multiOrderedSet, multilru, ...) and persistence never do.
Containers store exactly the box they are given.  **For minimal storage in a
container, pass each box through `databoxCanonical` before writing it:**

```c
databoxBig scratch;
const databox *stored = databoxCanonical(&value, &scratch); /* "42" -> 42,
                                                             5.0 -> 5 */
multimapInsert(&m, (const databox *[]){&key, stored});
```

Either way, numbers are one value to every comparison and key match: 5 and
5.0 are the same multimap or multidict key whichever form was stored.

```c
/* true, with *out holding the canonical number, when 'in' is a BYTES or
 * BYTES_EMBED numeral; false, with *out untouched, otherwise. */
bool databoxCanonicalNumeral(const databox *in, databoxBig *out);

/* true, with *out the SIGNED_64 / UNSIGNED_64 it equals, for a FLOAT_32 or
 * DOUBLE_64 holding a whole number in [-2^63, 2^64); -0.0, fractions,
 * infinities and NaN stay reals */
bool databoxIntegralRealAsInteger(const databox *in, databox *out);

/* 'in' itself, or 'scratch' holding the minimal form of 'in': a numeral
 * string's number, or a whole real's integer */
static inline const databox *databoxCanonical(const databox *in,
                                              databoxBig *scratch);
```

| Bytes                                                                                                                                                                         | Canonical form                                                                  |
| ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------- |
| `"22"`, `"0"`, `"-5"`, up to `"9223372036854775807"`, down to `"-9223372036854775808"`                                                                                        | `DATABOX_SIGNED_64`                                                             |
| `"9223372036854775808"` to `"18446744073709551615"`                                                                                                                           | `DATABOX_UNSIGNED_64`                                                           |
| `"18446744073709551616"` to `"340282366920938463463374607431768211455"`                                                                                                       | `DATABOX_UNSIGNED_128`                                                          |
| `"-9223372036854775809"` to `"-170141183460469231731687303715884105728"`                                                                                                      | `DATABOX_SIGNED_128`                                                            |
| `"22.0"`, `"-0.5"`, `"0.4"`: reals whose re-print gives exactly the input bytes                                                                                               | `DATABOX_FLOAT_32` when the value is exact as a float, else `DATABOX_DOUBLE_64` |
| `"007"`, `"01"`, `"-0"`, `"-01"`, `"+5"`, `" 22"`, `"22 "`, `"1e3"`, `".5"`, `"5."`, `"-0.0"`, `"299.5000"`, `"123.10"`, `"nan"`, `"inf"`, numerals outside the 128-bit range | not a numeral: the caller keeps the bytes                                       |

The 64-bit forms are exactly what `StrScanScanReliable` accepts; the 128-bit
forms are the integers beyond 64 bits that `StrScanScanReliableConvert128`
accepts. `databoxCanonicalNumeral` reads only `DATABOX_BYTES` and
`DATABOX_BYTES_EMBED`; `databoxCanonical` also turns a whole real box into
its integer, and passes every other type through.  A numeral keeps its
spelling: `"22.0"` is the real 22.0 (it prints back as `"22.0"`), not 22.

Ownership and lifetime:

- Neither function allocates. The result never borrows from the input bytes.
- A narrow result has `big = 0`. A 128-bit result keeps its payload in the
  `databoxBig`'s own `extra` (`big = 1`, `allocated = 0`), so it is valid for
  as long as the scratch is and must not be copied by value; move it with
  `databoxBigCopyFrom`.
- On a rejection, `*out` (or `scratch`) is not written.

`databoxCanonical` is the form to use when you opt in. Its inline fast reject
returns `in` without a call for non-bytes boxes, empty strings, and strings
whose first byte is neither `-` nor a digit, so ordinary string keys pay one
or two branches and only numeral-looking strings reach the scanner:

```c
databoxBig scratch;
const databox *key = databoxCanonical(callerKey, &scratch);
/* store or compare 'key' */
```

## Numeric Updates

Every operation that adds to a stored number follows one policy, chosen by
what the number is.

**Stored values** (container values, scores and counters: `flexIncrby*`,
`multidictIncrBy` / `IncrByFloat`, `multimapFieldIncrBy`,
`multiOrderedSetIncrBy`, `multiFenwick`, `multiFenwickFlat`,
`multiFenwick2d`, and the elements of the typed `fenwick<T>` /
`fenwick2d<T>` trees):

- Integers add **exactly**. A result outside the value's range (int64, or
  [INT64_MIN, UINT64_MAX] where the API can return either kind) is
  **refused**: the call reports failure and the stored value is unchanged.
  Nothing wraps and nothing saturates.
- A float or double operand makes the sum a double. An `inf` or NaN result is
  refused the same way.
- Adding zero succeeds and changes nothing.
- An integer result keeps an integer type; it may come back in its minimal
  same-type form (a non-negative SIGNED reads as UNSIGNED).

**Fixed-width bit fields** (`varintPacked` `SetIncr`): the value
**saturates** at 0 and `2^bits - 1`, so an overflow never spills into the
neighbouring slot. A saturated value is an ordinary number: it can be
decremented again.

**Estimator counters** (the counting Bloom filter in `linearBloomCount.h`):
the counter **saturates and sticks**. A saturated
counter's true count is unknown, so insertion never lowers it. Only the
structure's own aging (Bloom decay) reduces counters, and never below 0.

**Wrapping arithmetic** (modulo 2^64) appears only inside codecs and hashes
(dod deltas, xof XORs, hash mixing), and inside the typed Fenwick and
segment trees' internal cells. Their stored elements never wrap (see
above); a derived _sum_ that does not fit the tree's C type comes back
modulo 2^bits, since those APIs return a bare C value and have no way to
refuse. Sums that fit are always exact. The databox trees return exact
sums or refuse.

## Debugging and Display

```c
/* Print databox representation to stdout */
void databoxRepr(const databox *box);

/* Get string representation */
const char *databoxReprStr(const databox *const box);

/* Print with message */
void databoxReprSay(const char *msg, const databox *box);

/* Example usage */
databox num = databoxNewSigned(42);
databoxRepr(&num);  /* Prints: "{SIGNED: 42}" — no trailing newline */

databox str = databoxNewBytesString("Hello");
databoxReprSay("My string:", &str);  /* Prints: "My string: {BYTES: Hello}\n" */

const char *repr = databoxReprStr(&str);
printf("Repr: %s\n", repr);
```

`databoxReprStr` formats into a `_Thread_local static` buffer, so the
returned pointer is valid only until the next `databoxReprStr` call **on the
same thread**. Copy it if you need to keep it. `databoxRepr` prints without
a newline; `databoxReprSay` prints `msg`, a space, the repr, and a newline.

## Real-World Examples

### Example 1: Polymorphic Array

```c
/* Store different types in a single array */
databox values[100];
int count = 0;

/* Add various types */
values[count++] = databoxNewSigned(42);
values[count++] = databoxNewReal(3.14);
values[count++] = databoxNewBytesString("hello");
values[count++] = databoxBool(true);
values[count++] = databoxNull();

/* Process them */
for (int i = 0; i < count; i++) {
    if (DATABOX_IS_INTEGER(&values[i])) {
        printf("Integer: %" PRId64 "\n", values[i].data.i64);
    } else if (DATABOX_IS_FLOAT(&values[i])) {
        printf("Float: %f\n", (values[i].type == DATABOX_FLOAT_32)
                                  ? (double)values[i].data.f32
                                  : values[i].data.d64);
    } else if (DATABOX_IS_BYTES(&values[i])) {
        const uint8_t *str = databoxCBytes(&values[i]);
        printf("String: %.*s\n", (int)databoxLen(&values[i]), str);
    } else if (DATABOX_IS_TRUE(&values[i])) {
        printf("Boolean: true\n");
    } else if (DATABOX_IS_NULL(&values[i])) {
        printf("NULL value\n");
    }
}
```

### Example 2: Efficient Small String Storage

```c
/* Store many small strings efficiently */
databox strings[1000];

const char *inputs[] = {
    "a", "bc", "def", "ghij", "klmno", "pqrstu", "vwxyz12"
};

for (int i = 0; i < COUNT_ARRAY(inputs); i++) {
    /* Automatically embeds strings ≤ 8 bytes */
    strings[i] = databoxNewBytesAllowEmbed(inputs[i], strlen(inputs[i]));

    if (DATABOX_IS_BYTES_EMBED(&strings[i])) {
        printf("%s - EMBEDDED (no heap allocation!)\n", inputs[i]);
    } else {
        printf("%s - uses pointer\n", inputs[i]);
    }
}

/* Output:
 * a - EMBEDDED (no heap allocation!)
 * bc - EMBEDDED (no heap allocation!)
 * def - EMBEDDED (no heap allocation!)
 * ghij - EMBEDDED (no heap allocation!)
 * klmno - EMBEDDED (no heap allocation!)
 * pqrstu - EMBEDDED (no heap allocation!)
 * vwxyz12 - EMBEDDED (no heap allocation!)
 */

/* No cleanup needed for embedded values! */
```

### Example 3: Key-Value Store with Mixed Types

```c
typedef struct {
    databox key;
    databox value;
} kvpair;

kvpair store[100];
int count = 0;

/* Insert various key-value pairs */
void insert(const char *key, databox value) {
    store[count].key = databoxNewBytesString(key);
    store[count].value = value;
    count++;
}

/* Usage */
insert("count", databoxNewSigned(42));
insert("pi", databoxNewReal(3.14159));
insert("name", databoxNewBytesString("Alice"));
insert("active", databoxBool(true));

/* Lookup */
databox *find(const char *key) {
    databox searchKey = databoxNewBytesString(key);
    for (int i = 0; i < count; i++) {
        if (databoxEqual(&store[i].key, &searchKey)) {
            return &store[i].value;
        }
    }
    return NULL;
}

/* Retrieve and use */
databox *result = find("count");
if (result && DATABOX_IS_INTEGER(result)) {
    printf("count = %" PRId64 "\n", result->data.i64);
}

result = find("name");
if (result && DATABOX_IS_BYTES(result)) {
    printf("name = %.*s\n",
           (int)databoxLen(result),
           databoxCBytes(result));
}
```

### Example 4: Offset-Based String References

```c
/* Large buffer with many substrings */
const char *bigbuffer = "apple|banana|cherry|date|elderberry";
size_t buflen = strlen(bigbuffer);

databox fruits[5];
size_t offsets[] = {0, 6, 13, 20, 25};
size_t lengths[] = {5, 6, 6, 4, 10};

/* Create offset references (no copying!) */
for (int i = 0; i < 5; i++) {
    fruits[i] = databoxNewOffsetAllowEmbed(
        bigbuffer, offsets[i], lengths[i]
    );
}

/* Later, resolve offsets to actual bytes */
for (int i = 0; i < 5; i++) {
    if (fruits[i].type == DATABOX_BYTES_OFFSET) {
        /* Convert offset to real pointer */
        databoxOffsetBoxToRealBox(&fruits[i], bigbuffer);
    }

    const uint8_t *str = databoxCBytes(&fruits[i]);
    printf("Fruit: %.*s\n", (int)databoxLen(&fruits[i]), str);
}
```

## 128-Bit Integer Support (databoxBig)

```c
/* 32-byte, 16-byte-aligned databoxBig for 128-bit integers */
typedef struct databoxBig {
    databoxUnion data;                /* 8 bytes */
    databoxInternals_;                /* 8 bytes: same type/flags/len as databox */
    _Alignas(16) uint8_t extra[16];   /* Backing store for the 128-bit value */
} databoxBig;

/* Sets big = true and points data.u128 at the box's own 'extra';
 * leaves every other field as it was */
#define DATABOX_BIG_INIT(dbig)

/* Set 128-bit values (each does DATABOX_BIG_INIT first) */
#define DATABOX_BIG_SIGNED_128(box, d)    /* Set as __int128_t */
#define DATABOX_BIG_UNSIGNED_128(box, d)  /* Set as __uint128_t */

/* Compound literals for 64-bit values in a databoxBig */
#define DATABOX_BIG_SIGNED(d)
#define DATABOX_BIG_UNSIGNED(d)

/* Example usage */
databoxBig bigbox = {{0}};
DATABOX_BIG_INIT(&bigbox);

__int128_t huge = (__int128_t)1 << 100;
DATABOX_BIG_SIGNED_128(&bigbox, huge);

/* Access */
__int128_t value = *bigbox.data.i128;
```

`databox` and `databoxBig` share an identical prefix (`data` plus the
metadata word), which is what lets one be passed where the other is
expected. The difference is storage: `data.i128`/`data.u128` are
**pointers**, so a plain 16-byte `databox` has nowhere to put a 128-bit
value — it can only borrow one. `databoxBig` supplies that storage in
`extra`, and `DATABOX_BIG_INIT` is what aims the pointer at it. Anything
that may hold more than 64 bits (numeric quantum sets, `intsetBig`,
`flexPushDataboxBig`, `flexGetSingleDataboxBig`) must therefore use a
`databoxBig`. Decoding a 128-bit element into a plain `databox` with
`big = 0` reports only the type (`data.ptr == NULL`), and pushing such a
type-only box asserts.

`extra` is 16-byte aligned (`_Alignof(databoxBig) == 16`, static-asserted)
because decoders store the value through `data.u128` as a full
`__uint128_t`. The size stays 32 bytes; structs that embed a `databoxBig`
inherit the 16-byte alignment.

### Who sets up the wide capability

Decoders read `big` on the destination: with `big = 1` they write the value
through the pointer, with `big = 0` they report the type only. So **whoever
owns the destination storage sets it up before decoding**:

- A caller passing its own box to a fill-in API uses `DATABOX_BIG_INIT` on a
  `databoxBig` to receive wide values, or zeroes the box to receive the type
  only. An uninitialized box is a caller bug: stale `big` bits turn into a
  write through a stale pointer.
- When the destination is yours anyway, prefer the capability-owning
  decoders in flex (`flexGetByTypeBig`, `flexGetNextByTypeBig`,
  `flexGetTwoDataboxesBig`): they ignore the box's prior state and return
  `big = 1` exactly when the value is wide.
- Library-filled results and library temporaries always carry their own
  wide storage.
- A narrow decode overwrites the pointer, so re-run `DATABOX_BIG_INIT`
  before each decode into the same box.

### Wide slots and deferred boxes

```c
/* 16 bytes, 16-byte aligned: storage for one 96/128-bit payload */
typedef struct databoxWide {
    _Alignas(16) uint8_t b[16];
} databoxWide;

/* True for a 96/128-bit type with data.ptr == NULL */
static inline bool databoxIsWideDeferred(const databox *box);
```

A `databoxWide` is a slot, not a box. A decoder that is handed a slot aims
a plain `databox`'s `data.i128`/`data.u128` at it, with `big = 1` and
`allocated = 0`, so the box borrows the slot and the value is valid for as
long as the slot is. Narrow values never touch the slot, so one on the stack
or at the tail of an iterator costs nothing until a wide value arrives. Use
a `databoxBig` when you want the box and its storage in one object, and a
`databoxWide` when the box is a plain `databox` (an entry, an array element,
a temporary) and the storage sits elsewhere.

A **deferred** box has a wide type and no payload (`data.ptr == NULL`). A
decode reports a wide value this way when its destination has no wide
storage, so the caller still learns the type and can read the value again
into storage it owns. No flag bit marks the state; `databoxIsWideDeferred`
tests the type and the pointer.

A deferred box is not a value. Comparing, inserting or encoding one is a
caller bug, and those paths fail fast: their 96/128-bit arms call
`databoxWidePayloadRequire(box)`, which reports a FATAL
`DK_DIAG_DATABOX_WIDE_DEFERRED` diagnostic naming the caller and aborts
through `databoxWidePayloadMissing` instead of dereferencing NULL (see
[DIAGNOSTICS.md](DIAGNOSTICS.md): your diagnostic hook runs before the
abort). Narrow values never reach the check.

## Best Practices

### 1. Always Check Types

```c
/* WRONG - no type checking */
databox box = getSomeBox();
int64_t val = box.data.i64;  /* What if it's not an integer? */

/* RIGHT - check type first */
databox box = getSomeBox();
if (DATABOX_IS_SIGNED_INTEGER(&box)) {
    int64_t val = box.data.i64;
    /* Safe to use */
} else {
    /* Handle wrong type */
}
```

### 2. Use Embedding for Small Data

```c
/* Borrows the caller's memory: nothing is copied, and the box dies
 * with whatever 'small_data' points at. */
databox str = databoxNewBytes(small_data, 3);

/* Copies the 3 bytes into the box itself: no allocation, and the box
 * outlives 'small_data'. */
databox str = databoxNewBytesAllowEmbed(small_data, 3);

/* Copies unconditionally: embeds at <= 8 bytes, otherwise zmalloc. */
databox str = databoxNewBytesAllocateOrEmbed(small_data, 3);
```

Prefer the `AllowEmbed` forms whenever the box may outlive its source: the
copy is free for 8 bytes or fewer, and the box stays self-contained.

### 3. Clean Up Allocated Boxes

```c
/* A copying constructor may have allocated; releasing is unconditional
 * because databoxFreeData() checks the 'allocated' flag itself. */
databox str = databoxNewBytesAllocateOrEmbed(large_data, 1000);
/* ... use it ... */
databoxFreeData(&str);   /* stack box: data only, never the box */
```

Use `databoxFree()` **only** on a `databox` that was itself heap-allocated —
it ends with `zfree(box)`.

### 4. Use Macros for Performance

```c
/* Function call (a static inline, so usually free after inlining) */
databox num = databoxNewSigned(42);

/* Compound literal: builds a temp struct then copies it */
databox num = DATABOX_SIGNED(42);

/* In-place: writes the two fields directly, no temp struct */
databox num = {{0}};
DATABOX_SET_SIGNED(&num, 42);
```

The compound-literal macros read nicely, but compilers implement them by
materializing a temporary struct and copying it back — the in-place
`DATABOX_SET_*` macros avoid that. Note that neither form clears the flag
bits, so declare with `= {{0}}` whenever the box's ownership flags will be
read later.

## Performance Characteristics

| Operation  | Complexity | Notes                           |
| ---------- | ---------- | ------------------------------- |
| Creation   | O(1)       | Constant time, may embed        |
| Copy       | O(n)       | n = byte length if allocated    |
| Compare    | O(n)       | n = min(len_a, len_b) for bytes |
| Type Check | O(1)       | Macro expansion, no cost        |
| Access     | O(1)       | Direct union access             |
| Free       | O(1)       | Single free call if needed      |

## Memory Usage

- **Fixed overhead**: 16 bytes per databox (regardless of contents)
- **Embedded strings**: No extra memory for ≤ 8 bytes
- **Large data**: 16 bytes + pointer to external data
- **128-bit integers**: 32 bytes (databoxBig)

## Thread Safety

databox itself is **not thread-safe**. Concurrent access requires external synchronization:

```c
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
databox shared_box = {{0}};

/* Thread-safe modification */
pthread_mutex_lock(&lock);
DATABOX_SET_SIGNED(&shared_box, new_value);
pthread_mutex_unlock(&lock);
```

## Common Pitfalls

### 1. Calling databoxFree on a Stack Box

```c
/* WRONG - databoxFree() ends with zfree(box) */
databox box = databoxNewBytesAllocateOrEmbed(data, len);
databoxFree(&box);          /* frees a stack address! */

/* RIGHT - release the owned data, leave the struct alone */
databox box = databoxNewBytesAllocateOrEmbed(data, len);
/* ... use it ... */
databoxFreeData(&box);

/* RIGHT - databoxFree() is for heap-allocated databoxes */
databox *box = zcalloc(1, sizeof(*box));
databoxCopyBytes(box, data, len);
databoxFree(box);
```

### 2. Using Offset Boxes Without Resolution

```c
/* WRONG */
databox offset = databoxNewOffsetAllowEmbed(base, offset, len);
const uint8_t *ptr = databoxBytes(&offset);  /* Garbage! offset not resolved */

/* RIGHT */
databox offset = databoxNewOffsetAllowEmbed(base, offset, len);
databoxOffsetBoxToRealBox(&offset, base);  /* Resolve first */
const uint8_t *ptr = databoxBytes(&offset);  /* Now valid */
```

### 3. Modifying Referenced Data

```c
char buffer[100] = "Hello";
databox ref = databoxNewBytes(buffer, 5);  /* Reference, not copy */

strcpy(buffer, "World");  /* Changes referenced data! */
const char *str = databoxCBytes(&ref);
/* str now points to "World", not "Hello" */

/* If you need isolation, allocate a copy */
databox copy = databoxNewBytesAllocateOrEmbed(buffer, 5);
strcpy(buffer, "World");  /* copy still has "Hello" */
```

### 4. Reading Flags From an Uninitialized Box

```c
/* WRONG - flexGetByType writes type/data/len but not the flag bits */
databox box;
flexGetByType(fe, &box);
if (box.allocated) { /* reads uninitialized memory */ }

/* RIGHT */
databox box = {{0}};
flexGetByType(fe, &box);
```

## See Also

- [flex](../flex/FLEX.md) - Flexible arrays often used with databox
- [multimap](../multimap/MULTIMAP.md) - Uses databox for polymorphic storage
- [Architecture Overview](../../ARCHITECTURE.md#3-universal-container---databox) - Design philosophy

## Testing

Run the databox test suite:

```bash
./src/datakit-test test databox
```

The test suite includes:

- Type checking and conversion
- Embedding optimization
- Comparison and equality
- Memory management
- Copy operations
- Edge cases and boundary conditions
