# multiarray - Scale-Aware Dynamic Array

## Overview

`multiarray` is a **self-optimizing dynamic array** that automatically transitions between three internal representations based on data size. It provides O(1) indexed access with minimal memory overhead by using the most efficient storage format for your current dataset size.

**Key Features:**

- Automatic scaling from 8 bytes (native) → 24 bytes (small) → variable size (medium) → linked list (large)
- Random access by index with O(1) performance
- Pointer-stable variants for fixed-size elements
- Transparent upgrades between variants as data grows
- Support for arbitrary element sizes
- Optional native integration for inline arrays

**Headers**: `multiarray.h`, `multiarraySmall.h`, `multiarrayMedium.h`, `multiarrayLarge.h`

**Source**: `multiarray.c`, `multiarraySmall.c`, `multiarrayMedium.c`, `multiarrayLarge.c`

## Architecture

The multiarray family uses **pointer tagging** to transparently manage four underlying implementations:

```
┌─────────────┐
│  multiarray*│  Opaque pointer with 2-bit type tag
└──────┬──────┘
       │
       ├──[tag=0]──> Native      (direct uint8_t*, managed by user)
       ├──[tag=1]──> Small       (24 bytes, single array)
       ├──[tag=2]──> Medium      (16 + N*16 bytes, pointer array)
       └──[tag=3]──> Large       (24 + N*16 bytes, XOR linked list)
```

When you call operations on multiarray:

1. The implementation checks the current variant via pointer tag
2. Routes to the appropriate function (Native/Small/Medium/Large)
3. Automatically upgrades to the next variant if size threshold exceeded
4. Transparently updates your pointer to the new variant

**You never need to know which variant you're using** - the API is identical.

## Data Structures

### multiarray Type

```c
/* Opaque multiarray type */
typedef struct multiarray multiarray;
typedef uint32_t multiarrayIdx;

/* Internal variant types */
typedef enum multiarrayType {
    MULTIARRAY_TYPE_NATIVE = 0,  /* 8 bytes, fixed */
    MULTIARRAY_TYPE_SMALL = 1,   /* 16 + 8 bytes, fixed */
    MULTIARRAY_TYPE_MEDIUM = 2,  /* 16 + (16 * N) bytes pointer array */
    MULTIARRAY_TYPE_LARGE = 3    /* 24 + (16 * N) bytes XOR linked list */
} multiarrayType;
```

### multiarraySmall

```c
struct multiarraySmall {
    uint8_t *data;      /* Single contiguous array */
    uint16_t count;     /* Current number of elements */
    uint16_t len;       /* Size of each element in bytes */
    uint16_t rowMax;    /* Maximum elements before upgrade */
};

/* Total: 16 bytes fixed overhead + data array */
```

### multiarrayMedium

```c
struct multiarrayMedium {
    multiarrayMediumNode *node;  /* Array of data pointers */
    uint32_t count;              /* Number of nodes */
    uint16_t len;                /* Size of each element */
    uint16_t rowMax;             /* Max elements per node */
};

typedef struct multiarrayMediumNode {
    uint8_t *data;      /* Pointer to data chunk */
    uint16_t count;     /* Elements in this node */
} multiarrayMediumNode;

/* Total: 16 bytes + (count * sizeof(node)) overhead */
```

### multiarrayLarge

```c
struct multiarrayLarge {
    multiarrayLargeNode *head;   /* First node */
    multiarrayLargeNode *tail;   /* Last node */
    uint16_t len;                /* Size of each element */
    uint16_t rowMax;             /* Max elements per node */
};

typedef struct multiarrayLargeNode {
    uint8_t *data;          /* Pointer to data chunk */
    uint16_t count;         /* Elements in this node */
    uintptr_t prevNext;     /* XOR of prev and next pointers */
} multiarrayLargeNode;

/* Total: 24 bytes + (nodes * 24 bytes) overhead */
```

## Creation Functions

### Container API

```c
/* Create new multiarray with element size and max elements */
multiarray *multiarrayNew(uint16_t len, uint16_t rowMax);

/* Get number of elements */
size_t multiarrayCount(const multiarray *m);

/* Get total bytes used */
size_t multiarrayBytes(const multiarray *m);

/* Example usage */
multiarray *arr = multiarrayNew(sizeof(int64_t), 1024);
```

### Native API (Inline Arrays)

```c
/* Create native array (direct allocation) */
#define multiarrayNativeNew(holder) (multiarraySmallNativeNew(holder))

/* Free native array */
#define multiarrayNativeFree(mar) /* ... */

/* Example: inline array of structs */
typedef struct MyData {
    int64_t id;
    double value;
} MyData;

MyData *arr = multiarrayNativeNew(arr[1024]);
/* arr is now allocated for 1024 MyData elements */
```

### Small Direct API

```c
/* Direct API for typed arrays */
#define multiarraySmallDirectNew(e)  (zcalloc(1, sizeof(*(e))))
#define multiarraySmallDirectFree(e) (zfree(e))

/* Example */
typedef struct Point {
    double x, y, z;
} Point;

Point *points = multiarraySmallDirectNew(points);
/* points is allocated and ready to use */
```

## Access Operations

### Get by Index

```c
/* Container API */
void *multiarrayGet(multiarray *m, multiarrayIdx idx);
void *multiarrayGetHead(multiarray *m);
void *multiarrayGetTail(multiarray *m);

/* Example */
multiarray *arr = multiarrayNew(sizeof(int64_t), 1024);
/* ... insert data ... */

int64_t *value = (int64_t *)multiarrayGet(arr, 42);
printf("Element 42: %ld\n", *value);

int64_t *first = (int64_t *)multiarrayGetHead(arr);
int64_t *last = (int64_t *)multiarrayGetTail(arr);
```

### Native API Access

```c
/* Get element from native array */
#define multiarrayNativeGet(mar, holder, result, idx) /* ... */
#define multiarrayNativeGetHead(mar, holder, result) /* ... */
#define multiarrayNativeGetTail(mar, holder, result, count) /* ... */

/* Forward iteration optimized */
#define multiarrayNativeGetForward(mar, holder, result, idx) /* ... */

/* Example */
typedef struct Data {
    uint32_t id;
    uint32_t value;
} Data;

Data *native = multiarrayNativeNew(native[100]);
int count = 0;

/* ... populate ... */

Data *item;
multiarrayNativeGet(native, Data, item, 5);
printf("Item 5: id=%u, value=%u\n", item->id, item->value);
```

### Small API Access

```c
/* Get from multiarraySmall */
#define multiarraySmallGet(mar, idx) /* ... */
#define multiarraySmallGetHead(mar) /* ... */
#define multiarraySmallGetTail(mar) /* ... */

/* Direct array access */
#define multiarraySmallDirectGet(e, idx) (&(e)[idx])
#define multiarraySmallDirectGetHead(e) multiarraySmallDirectGet(e, 0)
#define multiarraySmallDirectGetTail(e, count) /* ... */

/* Example */
multiarraySmall *small = multiarraySmallNew(sizeof(int64_t), 512);
/* ... populate ... */

int64_t *val = (int64_t *)multiarraySmallGet(small, 10);
printf("Element 10: %ld\n", *val);
```

## Insertion Operations

### Container API

```c
/* Insert after index */
void multiarrayInsertAfter(multiarray **m, multiarrayIdx idx, void *what);

/* Insert before index */
void multiarrayInsertBefore(multiarray **m, multiarrayIdx idx, void *what);

/* Example */
multiarray *arr = multiarrayNew(sizeof(double), 1024);

double val1 = 3.14;
multiarrayInsertAfter(&arr, 0, &val1);  /* Insert at beginning */

double val2 = 2.71;
multiarrayInsertAfter(&arr, 0, &val2);  /* Insert after first */

/* Array now contains: [3.14, 2.71] */
```

### Native API

```c
/* Insert into native array with automatic upgrade */
#define multiarrayNativeInsert(mar, holder, rowMax, localCount, idx, s) /* ... */

/* Example: automatic upgrading */
typedef struct Entry {
    uint64_t key;
    uint64_t value;
} Entry;

Entry *arr = multiarrayNativeNew(arr[1024]);
int count = 0;
const int rowMax = 1024;

Entry e1 = {.key = 1, .value = 100};
multiarrayNativeInsert(arr, Entry, rowMax, count, 0, &e1);
/* count is now 1 */

/* After rowMax inserts, arr automatically upgrades to Medium variant */
for (int i = 0; i < 2000; i++) {
    Entry e = {.key = i, .value = i * 10};
    multiarrayNativeInsert(arr, Entry, rowMax, count, count, &e);
    /* arr may now be Medium or Large! */
}
```

### Small API

```c
/* Insert into multiarraySmall */
void multiarraySmallInsert(multiarraySmall *mar, uint16_t idx, void *s);

/* Direct API insert (opens slot) */
#define multiarraySmallDirectInsert(e, count, idx) /* ... */

/* Native insert (opens slot) */
#define multiarraySmallNativeInsert(e, holder, count, idx) /* ... */

/* Example: Small API */
multiarraySmall *small = multiarraySmallNew(sizeof(int32_t), 512);

int32_t value = 42;
multiarraySmallInsert(small, 0, &value);  /* Insert at head */

/* Example: Direct API */
typedef struct Point {
    float x, y;
} Point;

Point *points = multiarraySmallDirectNew(points);
int count = 0;

multiarraySmallDirectInsert(points, count, 0);
points[0].x = 1.0f;
points[0].y = 2.0f;
count++;
```

## Deletion Operations

### Container API

```c
/* Delete element at index */
void multiarrayDelete(multiarray **m, const uint32_t index);

/* Example */
multiarray *arr = multiarrayNew(sizeof(int64_t), 1024);
/* ... populate ... */

multiarrayDelete(&arr, 5);  /* Remove element at index 5 */
```

### Native API

```c
/* Delete from native array */
#define multiarrayNativeDelete(mar, holder, count, idx) /* ... */

/* Example */
typedef struct Item {
    uint32_t id;
} Item;

Item *items = multiarrayNativeNew(items[100]);
int count = 10;  /* assume populated */

multiarrayNativeDelete(items, Item, count, 3);
/* count is now 9, element at index 3 removed */
```

### Small API

```c
/* Delete from multiarraySmall */
void multiarraySmallDelete(multiarraySmall *mar, uint16_t idx);

/* Direct API delete */
#define multiarraySmallDirectDelete(e, count, idx) /* ... */

/* Native delete */
#define multiarraySmallNativeDelete(e, holder, count, idx) /* ... */

/* Example */
multiarraySmall *small = multiarraySmallNew(sizeof(double), 512);
/* ... populate ... */

multiarraySmallDelete(small, 0);  /* Remove first element */
```

## Medium and Large APIs

### Medium Operations

```c
/* Create Medium variant */
multiarrayMedium *multiarrayMediumNew(int16_t len, int16_t rowMax);

/* Upgrade from Small to Medium */
multiarrayMedium *multiarrayMediumFromSmall(multiarraySmall *small);

/* Create with initial data */
multiarrayMedium *multiarrayMediumNewWithData(int16_t len, int16_t rowMax,
                                              uint16_t count, void *data);

/* Access operations */
void *multiarrayMediumGet(const multiarrayMedium *mar, const int32_t idx);
void *multiarrayMediumGetForward(const multiarrayMedium *mar, const uint32_t idx);
void *multiarrayMediumGetHead(const multiarrayMedium *mar);
void *multiarrayMediumGetTail(const multiarrayMedium *mar);

/* Modification operations */
void multiarrayMediumInsert(multiarrayMedium *mar, const int32_t idx, const void *s);
void multiarrayMediumDelete(multiarrayMedium *mar, const int32_t idx);

/* Memory management */
void multiarrayMediumFreeInside(multiarrayMedium *e);
void multiarrayMediumFree(multiarrayMedium *e);
```

### Large Operations

```c
/* Create Large variant */
multiarrayLarge *multiarrayLargeNew(uint16_t len, uint16_t rowMax);

/* Upgrade from Medium to Large */
multiarrayLarge *multiarrayLargeFromMedium(multiarrayMedium *medium);

/* Access operations */
void *multiarrayLargeGet(const multiarrayLarge *mar, const int32_t idx);
void *multiarrayLargeGetForward(const multiarrayLarge *mar, const uint32_t idx);
void *multiarrayLargeGetHead(const multiarrayLarge *mar);
void *multiarrayLargeGetTail(const multiarrayLarge *mar);

/* Modification operations */
void multiarrayLargeInsert(multiarrayLarge *mar, const int32_t idx, const void *s);
void multiarrayLargeDelete(multiarrayLarge *mar, const int32_t idx);

/* Memory management */
void multiarrayLargeFreeInside(multiarrayLarge *e);
void multiarrayLargeFree(multiarrayLarge *e);
```

## Real-World Examples

### Example 1: Dynamic Integer Array

```c
#include "multiarray.h"

/* Simple growing integer array */
multiarray *arr = multiarrayNew(sizeof(int64_t), 1024);

/* Insert values */
for (int i = 0; i < 10000; i++) {
    int64_t val = i * i;
    multiarrayInsertAfter(&arr, i > 0 ? i - 1 : 0, &val);
}

/* Access values */
int64_t *val = (int64_t *)multiarrayGet(arr, 5000);
printf("Element 5000: %ld\n", *val);

/* Count and size */
printf("Elements: %zu\n", multiarrayCount(arr));
printf("Bytes: %zu\n", multiarrayBytes(arr));
```

### Example 2: Native Array with Auto-Upgrade

```c
typedef struct Record {
    uint64_t timestamp;
    double value;
} Record;

/* Start with native array */
Record *records = multiarrayNativeNew(records[1024]);
int count = 0;
const int rowMax = 1024;

/* Insert records - automatically upgrades when full */
for (int i = 0; i < 10000; i++) {
    Record r = {
        .timestamp = time(NULL) + i,
        .value = rand() / (double)RAND_MAX
    };

    multiarrayNativeInsert(records, Record, rowMax, count, count, &r);
    /* After 1024 inserts, this becomes Medium */
    /* After more inserts, may become Large */
}

/* Access works the same regardless of variant */
Record *r;
multiarrayNativeGet(records, Record, r, 5000);
printf("Record 5000: ts=%lu, val=%.3f\n", r->timestamp, r->value);

/* Clean up (handles all variants) */
multiarrayNativeFree(records);
```

### Example 3: Small Direct Array

```c
typedef struct Point3D {
    float x, y, z;
} Point3D;

/* Direct allocation */
Point3D *points = multiarraySmallDirectNew(points);
int count = 0;

/* Insert points */
for (int i = 0; i < 100; i++) {
    multiarraySmallDirectInsert(points, count, count);
    points[count].x = i * 1.0f;
    points[count].y = i * 2.0f;
    points[count].z = i * 3.0f;
    count++;
}

/* Access by index */
printf("Point 50: (%.1f, %.1f, %.1f)\n",
       points[50].x, points[50].y, points[50].z);

/* Delete element */
multiarraySmallDirectDelete(points, count, 25);
count--;

multiarraySmallDirectFree(points);
```

## Performance Characteristics

### Time Complexity

| Operation | Small        | Medium                              | Large                  |
| --------- | ------------ | ----------------------------------- | ---------------------- |
| Get       | O(1)         | O(1) per node + linear search nodes | O(n) traverse list     |
| Insert    | O(n) memmove | O(n) in node                        | O(n) traverse + insert |
| Delete    | O(n) memmove | O(n) in node                        | O(n) traverse + delete |
| Head/Tail | O(1)         | O(1)                                | O(1)                   |

### Space Complexity

| Variant | Fixed Overhead | Per Element  | Best For                        |
| ------- | -------------- | ------------ | ------------------------------- |
| Native  | 0 bytes        | element size | < 1024 elements, stack-friendly |
| Small   | 16 bytes       | element size | < 2048 elements                 |
| Medium  | 16 bytes + 16N | element size | 2K - 100K elements              |
| Large   | 24 bytes + 24N | element size | 100K+ elements                  |

### Automatic Upgrade Thresholds

```c
/* Native → Medium: when count reaches rowMax */
if (localCount == rowMax) {
    upgrade_to_medium();
}

/* Medium → Large: when pointer array > data array size */
if ((sizeof(void *) * medium->count) > (sizeof(element) * rowMax)) {
    upgrade_to_large();
}
```

## Best Practices

### 1. Choose Appropriate rowMax

```c
/* Small, cache-friendly arrays */
multiarray *small = multiarrayNew(sizeof(int32_t), 128);

/* Large arrays that will grow */
multiarray *large = multiarrayNew(sizeof(int64_t), 4096);
```

### 2. Use Native for Known Sizes

```c
/* If you know the exact size, use native */
MyStruct *arr = multiarrayNativeNew(arr[1000]);
/* Direct access, no overhead */
```

### 3. Prefer Forward Access

```c
/* When iterating, use GetForward for better cache locality */
for (int i = 0; i < count; i++) {
    void *elem = multiarrayMediumGetForward(mar, i);
    /* Process element */
}
```

### 4. Pass by Double-Pointer for Mutations

```c
/* WRONG - pointer won't upgrade */
void addItem(multiarray *m, void *item) {
    multiarrayInsertAfter(&m, 0, item);  /* m is local copy! */
}

/* RIGHT - allows upgrade */
void addItem(multiarray **m, void *item) {
    multiarrayInsertAfter(m, 0, item);  /* *m can be updated */
}
```

## Common Patterns

### Pattern 1: Growing Array

```c
multiarray *arr = multiarrayNew(sizeof(double), 1024);

for (int i = 0; i < 100000; i++) {
    double val = i * 3.14;
    multiarrayInsertAfter(&arr, i > 0 ? i - 1 : 0, &val);
}
/* Automatically upgraded to optimal variant */
```

### Pattern 2: Stack-Based Small Array

```c
typedef struct Entry {
    uint32_t key;
    uint32_t value;
} Entry;

Entry *local = multiarrayNativeNew(local[100]);
/* Use for temporary operations */
multiarrayNativeFree(local);
```

### Pattern 3: Chunked Data Processing

```c
/* Medium variant naturally chunks data */
multiarrayMedium *mar = multiarrayMediumNew(sizeof(DataPoint), 512);

/* Process by chunks for cache efficiency */
for (int nodeIdx = 0; nodeIdx < mar->count; nodeIdx++) {
    multiarrayMediumNode *node = &mar->node[nodeIdx];
    /* Process entire node at once */
    for (int i = 0; i < node->count; i++) {
        DataPoint *dp = (DataPoint *)(node->data + i * mar->len);
        /* Process data point */
    }
}
```

## Thread Safety

multiarray is **not thread-safe** by default. For concurrent access:

```c
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
multiarray *shared = multiarrayNew(sizeof(int64_t), 1024);

/* Reader */
pthread_mutex_lock(&lock);
void *val = multiarrayGet(shared, 42);
pthread_mutex_unlock(&lock);

/* Writer */
pthread_mutex_lock(&lock);
int64_t data = 999;
multiarrayInsertAfter(&shared, 0, &data);
pthread_mutex_unlock(&lock);
```

## Memory Management

### Freeing Resources

```c
/* Container API - frees everything */
multiarray *arr = multiarrayNew(sizeof(int), 1024);
/* Note: multiarrayFree not shown in headers, variants must be freed directly */

/* Small API */
multiarraySmall *small = multiarraySmallNew(sizeof(int), 512);
multiarraySmallFree(small);

/* Medium API */
multiarrayMedium *medium = multiarrayMediumNew(sizeof(int), 512);
multiarrayMediumFree(medium);

/* Large API */
multiarrayLarge *large = multiarrayLargeNew(sizeof(int), 512);
multiarrayLargeFree(large);
```

## See Also

- [VARIANTS.md](VARIANTS.md) - Detailed comparison of Small/Medium/Large variants
- [EXAMPLES.md](EXAMPLES.md) - Real-world usage examples
- [databox](../core/DATABOX.md) - Universal container often used with multiarray

## Testing

Run the multiarray test suite:

```bash
./src/datakit-test test multiarray
```

The test suite includes:

- Native → Small → Medium → Large automatic transitions
- Insertion and deletion across all variants
- Random access and sequential access
- Memory leak detection
- Performance benchmarks
