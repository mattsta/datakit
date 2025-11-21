# mflex - Compressed Flex Wrapper

## Overview

`mflex` is a **transparent compression wrapper** for `flex` arrays that automatically compresses and decompresses data using LZ4. It provides the same API as flex but with automatic compression when beneficial, reducing memory usage while maintaining high performance.

**Key Features:**

- Transparent LZ4 compression/decompression
- Automatic compression decisions (compress only when beneficial)
- Pointer tagging to track compressed vs uncompressed state
- Reusable state buffers to minimize allocations
- Support for no-compress mode
- Same API patterns as flex
- Three internal states: FLEX, CFLEX (compressed), NO_COMPRESS

**Headers**: `mflex.h`, `mflexInternal.h`

**Source**: `mflex.c`

## Architecture

### Pointer Tagging

mflex uses **low-bit pointer tagging** to track compression state:

```
┌──────────────┐
│   mflex *    │  Tagged pointer (3 possible states)
└──────┬───────┘
       │
       ├──[tag=1]──> flex * (uncompressed)
       ├──[tag=2]──> cflex * (LZ4 compressed)
       └──[tag=3]──> flex * (never compress)
```

**Tag bits**:

- `MFLEX_TYPE_FLEX = 1`: Points to regular flex
- `MFLEX_TYPE_CFLEX = 2`: Points to compressed flex (cflex)
- `MFLEX_TYPE_NO_COMPRESS = 3`: Points to flex, compression disabled

### State Management

To avoid repeated allocations during open/close cycles, mflex uses **reusable state buffers**:

```c
typedef struct mflexState {
    struct {
        union {
            flex *f;      /* Uncompressed buffer */
            uint8_t *buf; /* Compressed buffer */
        } ptr;
        size_t len;       /* Buffer size */
        bool retained;    /* Is buffer currently in use? */
    } buf[2];  /* buf[0] = uncompressed, buf[1] = compressed */

    size_t lenPreferred;  /* Target buffer size */
} mflexState;
```

**Benefits**:

- Reuse buffers across many compress/decompress cycles
- Grow buffers to "preferred" size
- Avoid malloc/free churn in hot loops

## Data Structures

### Main Type

```c
typedef struct mflex mflex;
typedef struct mflexState mflexState;

/* mflex is actually a tagged pointer to either:
 *   - flex *
 *   - cflex *
 * The tag determines which. */
```

### Internal State

```c
struct mflexState {
    struct {
        union {
            flex *f;
            uint8_t *buf;
        } ptr;
        size_t len;
        bool retained;
    } buf[2];  /* UNCOMPRESSED=0, COMPRESSED=1 */

    size_t lenPreferred;
};
```

## State Management

### Creation and Destruction

```c
/* Create state with specific initial buffer size */
mflexState *mflexStateNew(size_t initialBufferSize);

/* Create state with default 64KB buffers */
mflexState *mflexStateCreate(void);

/* Update preferred buffer size */
void mflexStatePreferredLenUpdate(mflexState *state, size_t len);

/* Get current preferred size */
size_t mflexStatePreferredLen(mflexState *state);

/* Reset buffers to preferred size (shrink/grow if needed) */
void mflexStateReset(mflexState *state);

/* Free state */
void mflexStateFree(mflexState *state);

/* Example: Lifecycle */
mflexState *state = mflexStateCreate();

/* Use state for many operations... */
for (int i = 0; i < 1000; i++) {
    mflex *m = /* ... */;
    /* Perform operations */
    mflexStateReset(state);  /* Clean up after each batch */
}

mflexStateFree(state);
```

### State Buffer Management

The state contains **two buffers**:

1. **Uncompressed buffer** (`buf[0]`):
   - Used for decompressing cflexes
   - Used as temporary during operations
   - Reused across multiple operations

2. **Compressed buffer** (`buf[1]`):
   - Used for compressing flexes
   - Receives compressed output
   - Swapped with mflex when compression succeeds

**Retained flag**:

- When `retained = true`, the buffer is "owned" by user code
- When `retained = false`, the buffer is available for state operations
- Prevents double-free and use-after-free bugs

## Creation Functions

```c
/* Create new mflex (auto-compress) */
mflex *mflexNew(void);

/* Create new mflex (never compress) */
mflex *mflexNewNoCompress(void);

/* Duplicate existing mflex */
mflex *mflexDuplicate(const mflex *m);

/* Convert flex to mflex (auto-compress) */
mflex *mflexConvertFromFlex(flex *f, mflexState *state);

/* Convert flex to mflex (never compress) */
mflex *mflexConvertFromFlexNoCompress(flex *f);

/* Example: Create and use */
mflexState *state = mflexStateCreate();
mflex *m = mflexNew();

/* ... operations ... */

mflexFree(m);
mflexStateFree(state);
```

## Metadata

```c
/* Check if empty */
bool mflexIsEmpty(const mflex *m);

/* Get element count */
size_t mflexCount(const mflex *m);

/* Get uncompressed size */
size_t mflexBytesUncompressed(const mflex *m);

/* Get compressed size (0 if not compressed) */
size_t mflexBytesCompressed(const mflex *m);

/* Get actual bytes used (compressed or uncompressed) */
size_t mflexBytesActual(const mflex *m);

/* Check if currently compressed */
bool mflexIsCompressed(const mflex *m);

/* Example: Check compression */
printf("Uncompressed: %zu bytes\n", mflexBytesUncompressed(m));
printf("Compressed: %zu bytes\n", mflexBytesCompressed(m));
printf("Actual: %zu bytes\n", mflexBytesActual(m));
printf("Compression: %s\n", mflexIsCompressed(m) ? "YES" : "NO");

if (mflexIsCompressed(m)) {
    double ratio = (double)mflexBytesUncompressed(m) / mflexBytesActual(m);
    printf("Ratio: %.2fx\n", ratio);
}
```

## Push Operations

All push operations automatically handle compression:

```c
/* Push various types */
void mflexPushBytes(mflex **mm, mflexState *state, const void *s,
                    size_t len, flexEndpoint where);
void mflexPushSigned(mflex **mm, mflexState *state, const int64_t i,
                     flexEndpoint where);
void mflexPushUnsigned(mflex **mm, mflexState *state, const uint64_t u,
                       flexEndpoint where);
void mflexPushHalfFloat(mflex **mm, mflexState *state, const float fl,
                        flexEndpoint where);
void mflexPushFloat(mflex **mm, mflexState *state, const float fl,
                    flexEndpoint where);
void mflexPushDouble(mflex **mm, mflexState *state, const double d,
                     flexEndpoint where);
void mflexPushByType(mflex **mm, mflexState *state, const databox *box,
                     flexEndpoint where);

/* Example: Build compressed array */
mflexState *state = mflexStateCreate();
mflex *m = mflexNew();

for (int i = 0; i < 10000; i++) {
    mflexPushDouble(&m, state, (double)i * 3.14159, FLEX_ENDPOINT_TAIL);
}

printf("Count: %zu, Compressed: %zu bytes\n",
       mflexCount(m), mflexBytesActual(m));
```

**Important**: Note that push operations take `mflex **mm` (pointer-to-pointer). This allows the function to replace the mflex with a compressed version.

## Delete Operations

```c
/* Delete count elements starting at offset */
void mflexDeleteOffsetCount(mflex **mm, mflexState *state,
                            int32_t offset, uint32_t count);

/* Example: Delete last element */
mflexDeleteOffsetCount(&m, state, -1, 1);

/* Example: Delete first 10 elements */
mflexDeleteOffsetCount(&m, state, 0, 10);
```

## Memory Management

```c
/* Reset to empty (frees data, creates new empty mflex) */
void mflexReset(mflex **mm);

/* Free mflex */
void mflexFree(mflex *m);

/* Example */
mflexReset(&m);  /* m is now empty */
mflexFree(m);    /* m is freed */
```

## Open/Close Pattern

For advanced operations, use **Open/Close**:

```c
/* Open for reading/writing (may decompress) */
flex *mflexOpen(const mflex *m, mflexState *state);

/* Open for read-only access */
const flex *mflexOpenReadOnly(const mflex *m, mflexState *state);

/* Close after growing (insert/push operations) */
void mflexCloseGrow(mflex **mm, mflexState *state, flex *f);

/* Close after shrinking (delete operations) */
void mflexCloseShrink(mflex **mm, mflexState *state, flex *f);

/* Close without compression attempt */
void mflexCloseNoCompress(mflex **mm, mflexState *state, const flex *f);
```

### Example: Manual Operations

```c
mflexState *state = mflexStateCreate();
mflex *m = mflexNew();

/* Populate with initial data */
for (int i = 0; i < 1000; i++) {
    mflexPushSigned(&m, state, i, FLEX_ENDPOINT_TAIL);
}

/* Open for direct manipulation */
flex *f = mflexOpen(m, state);

/* Use regular flex operations */
flexPushSigned(&f, 9999, FLEX_ENDPOINT_HEAD);

flexEntry *fe = flexHead(f);
databox box;
flexGetByType(f, &fe, &box);
printf("First element: %ld\n", box.data.i64);

/* Close and recompress */
mflexCloseGrow(&m, state, f);

printf("After operations: %zu bytes\n", mflexBytesActual(m));

mflexFree(m);
mflexStateFree(state);
```

### Example: Read-Only Access

```c
/* If only reading, no need to close */
const flex *f = mflexOpenReadOnly(m, state);

flexEntry *fe = flexHead(f);
while (fe) {
    databox box;
    flexGetByType(f, &fe, &box);
    printf("Element: %ld\n", box.data.i64);
}

/* No close needed for read-only */
```

## Compression Control

```c
/* Disable compression (convert to uncompressed) */
void mflexSetCompressNever(mflex **mm, mflexState *state);

/* Enable compression (try to compress) */
void mflexSetCompressAuto(mflex **mm, mflexState *state);

/* Example: Disable compression for hot data */
mflex *hotData = mflexNew();

/* ... populate ... */

/* Frequently accessed, disable compression */
mflexSetCompressNever(&hotData, state);

/* Example: Re-enable compression for cold data */
mflex *coldData = mflexNew();

/* ... populate ... */
mflexSetCompressNever(&coldData, state);  /* Disable initially */

/* Later, when access frequency drops */
mflexSetCompressAuto(&coldData, state);   /* Try to compress */
```

## Compression Behavior

### When Compression Happens

mflex attempts compression after:

- Push operations (`mflexPush*`)
- Manual close with `mflexCloseGrow`
- Conversion from flex with `mflexConvertFromFlex`

mflex does **not** compress if:

- Type is `MFLEX_TYPE_NO_COMPRESS`
- Compressed size >= uncompressed size
- Compressed size not in smaller allocator size class

### Compression Decision Algorithm

```c
/* Pseudocode from mflexCloseGrow */
if (type != MFLEX_TYPE_NO_COMPRESS) {
    size_t uncompressed = flexBytes(f);

    if (flexConvertToCFlex(f, buffer, bufferSize)) {
        size_t compressed = cflexBytes(cflex);

        /* Only use compression if it reduces allocator class */
        if (jebufUseNewAllocation(uncompressed, compressed)) {
            /* Use compressed version */
            return CFLEX;
        }
    }
}

/* Use uncompressed version */
return FLEX;
```

**Key insight**: Compression must move to a **smaller allocator size class** to be worthwhile. Saving a few bytes within the same size class doesn't help.

### Size Class Example

```c
/* jemalloc size classes (example) */
512, 1024, 2048, 4096, 8192, 16384, ...

/* Example 1: Worthwhile compression */
Uncompressed: 2000 bytes -> size class 2048
Compressed:    900 bytes -> size class 1024
Result: USE COMPRESSED (saves 1024 bytes)

/* Example 2: Not worthwhile */
Uncompressed: 2000 bytes -> size class 2048
Compressed:   1800 bytes -> size class 2048
Result: USE UNCOMPRESSED (no real savings)
```

## Performance Characteristics

### Time Complexity

| Operation           | Compressed                         | Uncompressed | Notes                       |
| ------------------- | ---------------------------------- | ------------ | --------------------------- |
| Open                | O(n) decompress                    | O(1)         | Decompress only if needed   |
| Close               | O(n) compress                      | O(1)         | Compress only if beneficial |
| Push (compressed)   | O(n) open + O(1) push + O(n) close |              | Full cycle                  |
| Push (uncompressed) | O(1) push                          | O(1)         | Direct operation            |
| Get metadata        | O(1)                               | O(1)         | Always fast                 |
| IsCompressed        | O(1)                               | O(1)         | Check tag bits              |

**Notes**:

- n = size of flex data
- LZ4 compression is very fast (~500 MB/s)
- LZ4 decompression is very fast (~2000 MB/s)
- For small arrays (< 1KB), overhead dominates

### Space Complexity

**Per mflex**:

- If compressed: cflexBytes(cflex) + overhead
- If uncompressed: flexBytes(flex)
- No wrapper struct overhead (just pointer tagging)

**Per mflexState**:

- Two buffers: ~128 KB by default (64 KB each)
- Adjustable via `mflexStateNew(size)`
- Buffers shrink/grow to `lenPreferred` on reset

**Compression Ratio** (typical):

- Repeated integers: 10-20x
- Sequential integers: 5-10x
- Random integers: 1-2x
- Strings: 2-5x
- Mixed data: 2-3x

### Compression Overhead

**When to use mflex**:

- Large arrays (> 4KB)
- Compressible data (repeated values, patterns)
- Infrequent modifications
- Memory-constrained environments

**When NOT to use mflex**:

- Small arrays (< 1KB)
- Incompressible data (random, encrypted)
- Frequent modifications
- CPU-constrained environments

## Common Patterns

### Pattern 1: Compressed Log Buffer

```c
/* Store large amounts of log data */
typedef struct logBuffer {
    mflex *entries;
    mflexState *state;
} logBuffer;

logBuffer *logNew() {
    logBuffer *log = malloc(sizeof(*log));
    log->state = mflexStateCreate();
    log->entries = mflexNew();
    return log;
}

void logAppend(logBuffer *log, const char *message) {
    mflexPushBytes(&log->entries, log->state,
                   message, strlen(message),
                   FLEX_ENDPOINT_TAIL);
}

void logPrint(logBuffer *log) {
    const flex *f = mflexOpenReadOnly(log->entries, log->state);

    flexEntry *fe = flexHead(f);
    int i = 0;
    while (fe) {
        databox box;
        flexGetByType(f, &fe, &box);
        printf("[%d] %.*s\n", i++,
               (int)box.len, box.data.bytes.start);
    }
}
```

### Pattern 2: Adaptive Compression

```c
/* Compress when inactive, decompress when active */
typedef struct adaptiveData {
    mflex *data;
    mflexState *state;
    time_t lastAccess;
    int accessCount;
} adaptiveData;

void access(adaptiveData *ad) {
    ad->lastAccess = time(NULL);
    ad->accessCount++;

    /* If frequently accessed, disable compression */
    if (ad->accessCount > 100) {
        mflexSetCompressNever(&ad->data, ad->state);
    }
}

void periodicCheck(adaptiveData *ad) {
    time_t now = time(NULL);

    /* If idle for > 60 seconds, enable compression */
    if (now - ad->lastAccess > 60) {
        ad->accessCount = 0;
        mflexSetCompressAuto(&ad->data, ad->state);
    }
}
```

### Pattern 3: Batch Operations

```c
/* Batch operations for efficiency */
void batchAppend(mflex **m, mflexState *state,
                 const int64_t values[], size_t count) {
    /* Open once */
    flex *f = mflexOpen(*m, state);

    /* Many operations */
    for (size_t i = 0; i < count; i++) {
        flexPushSigned(&f, values[i], FLEX_ENDPOINT_TAIL);
    }

    /* Close once (compress once) */
    mflexCloseGrow(m, state, f);
}

/* vs. individual operations (slow!) */
void individualAppend(mflex **m, mflexState *state,
                      const int64_t values[], size_t count) {
    for (size_t i = 0; i < count; i++) {
        /* Each call: decompress, modify, compress */
        mflexPushSigned(m, state, values[i], FLEX_ENDPOINT_TAIL);
    }
}
```

## Best Practices

### 1. Reuse State Across Operations

```c
/* GOOD: One state for many operations */
mflexState *state = mflexStateCreate();

for (int i = 0; i < 1000; i++) {
    mflex *m = processData(i);
    mflexPushSigned(&m, state, i, FLEX_ENDPOINT_TAIL);
    /* ... */
}

mflexStateFree(state);

/* BAD: Creating state every time */
for (int i = 0; i < 1000; i++) {
    mflexState *state = mflexStateCreate();  /* DON'T DO THIS */
    /* ... */
    mflexStateFree(state);
}
```

### 2. Reset State Periodically

```c
mflexState *state = mflexStateCreate();

for (int i = 0; i < 1000000; i++) {
    /* ... operations ... */

    /* Reset every 1000 iterations to prevent buffer bloat */
    if (i % 1000 == 0) {
        mflexStateReset(state);
    }
}
```

### 3. Use Read-Only Opens When Possible

```c
/* GOOD: Read-only, no close needed */
const flex *f = mflexOpenReadOnly(m, state);
flexEntry *fe = flexHead(f);
/* ... iterate ... */
/* No close! */

/* BAD: Read-write when not needed */
flex *f = mflexOpen(m, state);
/* ... iterate ... */
mflexCloseGrow(&m, state, f);  /* Unnecessary recompression */
```

### 4. Batch Operations

```c
/* GOOD: Open once, many operations, close once */
flex *f = mflexOpen(m, state);
for (int i = 0; i < 1000; i++) {
    flexPushSigned(&f, i, FLEX_ENDPOINT_TAIL);
}
mflexCloseGrow(&m, state, f);

/* BAD: Open/close every operation */
for (int i = 0; i < 1000; i++) {
    mflexPushSigned(&m, state, i, FLEX_ENDPOINT_TAIL);
    /* Decompresses and recompresses every time! */
}
```

### 5. Disable Compression for Hot Data

```c
/* Frequently modified data */
mflex *hot = mflexNewNoCompress();

/* OR disable later */
mflex *m = mflexNew();
mflexSetCompressNever(&m, state);
```

## Testing

From the test suite in `mflex.c`:

```c
TEST("populate entries") {
    mflexState *state = mflexStateCreate();
    mflex *m = mflexNew();

    for (size_t i = 0; i < 8192; i++) {
        mflexPushDouble(&m, state, 999999999.9999999,
                        FLEX_ENDPOINT_TAIL);
    }

    assert(mflexCount(m) == 8192);
    assert(mflexBytesActual(m) != mflexBytesUncompressed(m));

    printf("Uncompressed: %zu, Compressed: %zu\n",
           mflexBytesUncompressed(m), mflexBytesActual(m));

    mflexFree(m);
    mflexStateFree(state);
}

TEST("remove entries") {
    mflexState *state = mflexStateCreate();
    mflex *m = mflexNew();

    /* Populate */
    for (size_t i = 0; i < 8192; i++) {
        mflexPushDouble(&m, state, 999999999.9999999,
                        FLEX_ENDPOINT_TAIL);
    }

    /* Delete all */
    while (mflexCount(m)) {
        mflexDeleteOffsetCount(&m, state, -1, 1);
    }

    assert(mflexCount(m) == 0);

    mflexFree(m);
    mflexStateFree(state);
}
```

## Debugging

```c
/* Check compression state */
if (mflexIsCompressed(m)) {
    printf("Compressed: %zu -> %zu bytes (%.2fx)\n",
           mflexBytesUncompressed(m),
           mflexBytesCompressed(m),
           (double)mflexBytesUncompressed(m) / mflexBytesCompressed(m));
} else {
    printf("Uncompressed: %zu bytes\n", mflexBytesActual(m));
}

/* Check if state buffers are retained */
#ifdef DATAKIT_TEST
extern bool STATE_UNCOMPRESSED_RETAINED;
assert(!STATE_UNCOMPRESSED_RETAINED);  /* Should be false after close */
#endif
```

## Thread Safety

mflex and mflexState are **not thread-safe**. For concurrent access:

```c
/* Option 1: One state per thread */
__thread mflexState *threadState = NULL;

void threadInit() {
    threadState = mflexStateCreate();
}

/* Option 2: Lock protected state */
pthread_mutex_t stateLock = PTHREAD_MUTEX_INITIALIZER;
mflexState *sharedState = NULL;

void operation(mflex **m) {
    pthread_mutex_lock(&stateLock);
    mflexPushSigned(m, sharedState, 42, FLEX_ENDPOINT_TAIL);
    pthread_mutex_unlock(&stateLock);
}
```

## Comparison with Other Approaches

| Approach                   | Pros            | Cons                        |
| -------------------------- | --------------- | --------------------------- |
| mflex (transparent)        | No API changes  | Overhead on every operation |
| Manual compress/decompress | Full control    | Error-prone, verbose        |
| Always compressed          | Maximum savings | Slow for frequent access    |
| Never compressed           | Fast access     | High memory usage           |

**When to use mflex**:

- Want compression without API changes
- Mixed access patterns (hot/cold data)
- Memory is more important than CPU

**When to use manual**:

- Need fine-grained control
- Know exact access patterns
- Want to optimize specific operations

## See Also

- [flex](FLEX.md) - Underlying packed array structure
- [cflex](CFLEX.md) - LZ4-compressed flex
- [databox](../core/DATABOX.md) - Universal value container
- [jebuf](../mem/JEBUF.md) - Allocator size class utilities

## Implementation Notes

**Design Philosophy**:

- Transparency: Same API as flex
- Lazy: Only compress when closing
- Smart: Only compress if beneficial
- Efficient: Reuse buffers via state

**Pointer Tagging**:
Using low 2 bits for tags (pointers are always 4-byte aligned):

```c
#define _PTRLIB_TYPE(ptr) ((uintptr_t)(ptr) & 0x03)
#define _PTRLIB_USE(ptr) ((void *)((uintptr_t)(ptr) & ~0x03))
#define _PTRLIB_TAG(ptr, type) ((void *)(((uintptr_t)(ptr) & ~0x03) | (type)))
```

**LZ4 Integration**:
Uses LZ4 from the flex/cflex layer:

- `flexConvertToCFlex()` - compress flex to cflex
- `cflexConvertToFlex()` - decompress cflex to flex

**Use Cases**:

- Compressed logs and metrics
- Cold data in caches
- Large datasets with low access frequency
- Memory-constrained embedded systems
