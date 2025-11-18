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
        size_t offset;
    } bytes;

    /* For pointers */
    void *ptr;
    uintptr_t uptr;
} databoxUnion;
```

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

    /* ... and more container variants */
} databoxType;
```

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
databox unum = databoxNewUnsigned(12345ULL);
databox pi = databoxNewReal(3.14159);
```

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

/* Example usage */
const char *msg = "Hello, World!";
databox str1 = databoxNewBytesString(msg);  /* 13 bytes - not embedded */

const char *tiny = "Hi";
databox str2 = databoxNewBytesAllowEmbed(tiny, 2);  /* 2 bytes - EMBEDDED! */

/* Embedded storage means no heap allocation */
assert(str2.type == DATABOX_BYTES_EMBED);
```

### Pointers

```c
/* Create from pointer */
databox databoxNewPtr(void *ptr);

/* Example usage */
int *mydata = malloc(sizeof(int) * 100);
databox ptrbox = databoxNewPtr(mydata);

/* Later retrieve it */
void *retrieved = ptrbox.data.ptr;
```

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
#define DATABOX_IS_PTR(box)              /* True if pointer */
#define DATABOX_IS_TRUE(box)             /* True if DATABOX_TRUE */
#define DATABOX_IS_FALSE(box)            /* True if DATABOX_FALSE */
#define DATABOX_IS_NULL(box)             /* True if DATABOX_NULL */
#define DATABOX_IS_VOID(box)             /* True if DATABOX_VOID */
#define DATABOX_IS_FIXED(box)            /* True if fixed-size type */

/* Boolean evaluation */
#define DATABOX_IS_TRUEISH(box)   /* True if not false/null/0 */
#define DATABOX_IS_FALSEISH(box)  /* True if false/null/0 */

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

### Accessing Numeric Values

```c
/* Access values directly through union */
databox num = databoxNewSigned(-42);
int64_t val = num.data.i64;
printf("Value: %ld\n", val);

databox pi = databoxNewReal(3.14159);
double d = pi.data.d64;
printf("Pi: %.5f\n", d);

/* Type-safe access with checking */
databox unknown = /* ... from somewhere ... */;
if (DATABOX_IS_SIGNED_INTEGER(&unknown)) {
    int64_t value = unknown.data.i64;
    printf("Signed: %ld\n", value);
} else if (DATABOX_IS_DOUBLE(&unknown)) {
    double value = unknown.data.d64;
    printf("Double: %f\n", value);
}
```

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
databoxFree(&copy);
/* original is still valid */
```

### Freeing databoxes

```c
/* Free allocated data but keep databox structure */
void databoxFreeData(databox *box);

/* Free data and reset to VOID */
void databoxFree(databox *box);

/* Example usage */
databox str = databoxNewBytesAllocateOrEmbed("Hello", 5);

/* If this allocated memory: */
if (str.allocated) {
    databoxFree(&str);  /* Free memory */
    assert(str.type == DATABOX_VOID);
}

/* Embedded databoxes don't need freeing */
databox tiny = databoxNewBytesAllowEmbed("Hi", 2);
if (!tiny.allocated) {
    /* No heap allocation, nothing to free! */
}
```

### Retain Cache (Advanced)

```c
/* Cache for efficient memory reuse */
typedef struct databoxRetainCache {
    uint8_t *bytes[16];  /* Sizes: 128, 256, 512, ..., 4MB */
} databoxRetainCache;

/* Retain bytes using cache */
ssize_t databoxRetainBytesSelf(databox *dst, databoxRetainCache *cache);
void databoxRetainBytesSelfExact(databox *dst, void *bytes);

/* Example usage */
databoxRetainCache cache = {0};

/* Pre-allocate cache slots */
for (int i = 0; i < 16; i++) {
    size_t size = 128 << i;  /* 128, 256, 512, ... */
    cache.bytes[i] = malloc(size);
}

/* Use cache for efficient copying */
databox box = databoxNewBytes(data, 1000);
ssize_t slot = databoxRetainBytesSelf(&box, &cache);
if (slot >= 0) {
    printf("Used cache slot %zd\n", slot);
}

/* Clean up cache */
for (int i = 0; i < 16; i++) {
    free(cache.bytes[i]);
}
```

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
databoxRepr(&num);  /* Prints: "[SIGNED_64: 42]" or similar */

databox str = databoxNewBytesString("Hello");
databoxReprSay("My string:", &str);  /* Prints: "My string: [BYTES: ...]" */

const char *repr = databoxReprStr(&str);
printf("Repr: %s\n", repr);
```

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
        printf("Integer: %ld\n", values[i].data.i64);
    } else if (DATABOX_IS_FLOAT(&values[i])) {
        printf("Float: %f\n", values[i].data.d64);
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
    printf("count = %ld\n", result->data.i64);
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
/* 32-byte databoxBig for 128-bit integers */
typedef struct databoxBig {
    databoxUnion data;   /* 8 bytes */
    /* Same metadata as databox */
    uint8_t extra[16];   /* Extra storage for 128-bit values */
} databoxBig;

/* Initialize a databoxBig */
#define DATABOX_BIG_INIT(dbig) /* Initialize big variant */

/* Set 128-bit values */
#define DATABOX_BIG_SIGNED_128(box, d)    /* Set as __int128_t */
#define DATABOX_BIG_UNSIGNED_128(box, d)  /* Set as __uint128_t */

/* Example usage */
databoxBig bigbox;
DATABOX_BIG_INIT(&bigbox);

__int128_t huge = (__int128_t)1 << 100;
DATABOX_BIG_SIGNED_128(&bigbox, huge);

/* Access */
__int128_t value = *bigbox.data.i128;
```

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
/* WRONG - always allocates */
databox str = databoxNewBytes(small_data, 3);
/* May allocate heap memory for 3 bytes! */

/* RIGHT - embed when possible */
databox str = databoxNewBytesAllowEmbed(small_data, 3);
/* Stores inline, no allocation! */
```

### 3. Clean Up Allocated Boxes

```c
/* Remember to free allocated databoxes */
databox str = databoxNewBytesAllocateOrEmbed(large_data, 1000);
/* ... use it ... */
if (str.allocated) {
    databoxFree(&str);
}

/* Or always free (safe for non-allocated too) */
databoxFree(&str);  /* Sets to VOID, handles allocated flag */
```

### 4. Use Macros for Performance

```c
/* SLOWER - function call */
databox num = databoxNewSigned(42);

/* FASTER - compile-time initialization */
databox num = DATABOX_SIGNED(42);

/* FASTEST - in-place modification */
databox num;
DATABOX_SET_SIGNED(&num, 42);
```

## Performance Characteristics

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| Creation | O(1) | Constant time, may embed |
| Copy | O(n) | n = byte length if allocated |
| Compare | O(n) | n = min(len_a, len_b) for bytes |
| Type Check | O(1) | Macro expansion, no cost |
| Access | O(1) | Direct union access |
| Free | O(1) | Single free call if needed |

## Memory Usage

- **Fixed overhead**: 16 bytes per databox (regardless of contents)
- **Embedded strings**: No extra memory for ≤ 8 bytes
- **Large data**: 16 bytes + pointer to external data
- **128-bit integers**: 32 bytes (databoxBig)

## Thread Safety

databox itself is **not thread-safe**. Concurrent access requires external synchronization:

```c
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
databox shared_box;

/* Thread-safe modification */
pthread_mutex_lock(&lock);
DATABOX_SET_SIGNED(&shared_box, new_value);
pthread_mutex_unlock(&lock);
```

## Common Pitfalls

### 1. Forgetting to Check Allocated Flag

```c
/* WRONG */
databox box = databoxNewBytesAllocateOrEmbed(data, len);
/* ... use it ...*/
/* Never freed if allocated! Memory leak! */

/* RIGHT */
databox box = databoxNewBytesAllocateOrEmbed(data, len);
/* ... use it ... */
databoxFree(&box);  /* Always safe to call */
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

## See Also

- [flex](../flex/FLEX.md) - Flexible arrays often used with databox
- [multimap](../multimap/MULTIMAP.md) - Uses databox for polymorphic storage
- [Architecture Overview](../../ARCHITECTURE.md#universal-container---databox) - Design philosophy

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
