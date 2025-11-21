# dks - Dynamic String Template System

## Overview

`dks` is a **template-based dynamic string buffer system** that generates two implementations from a single codebase: `mds` (full) and `mdsc` (compact). It provides efficient string manipulation with automatic memory management and space-optimized headers.

**Key Features:**

- Template system generates `mds` (full) and `mdsc` (compact) from single source
- Automatic header size optimization (1 to 6 bytes overhead)
- Binary-safe string operations
- Printf-style formatting
- String splitting and joining
- UTF-8 aware operations
- Built-in hashing (XXH64, XXH3)

**Template Header**: `dks.h`

**Generated Types**:

- `mds` - Full variant with rich metadata
- `mdsc` - Compact variant optimized for space

**Implementation**: `mds.c` and `mdsc.c` (both include `dks.c` with different defines)

## Template System Explained

The DKS system uses **C preprocessor templates** to generate two distinct string implementations from a single source file (`dks.c`):

### How It Works

```c
/* dks.h - Template header that defines function names */
#if defined(DATAKIT_DKS_FULL)
    typedef char mds;
    #define DKS_TYPE mds
#elif defined(DATAKIT_DKS_COMPACT)
    typedef char mdsc;
    #define DKS_TYPE mdsc
#endif

/* Creates function names like mdsnew, mdscnew, etc. */
#define DKS_NEW DK_NAME(DKS_TYPE, new)
```

### Generating mds (Full)

```c
/* mds.c */
#define DATAKIT_DKS_FULL
#include "dks.c"
```

This generates all functions prefixed with `mds`:

- `mdsnewlen()`, `mdscat()`, `mdsfree()`, etc.

### Generating mdsc (Compact)

```c
/* mdsc.c */
#define DATAKIT_DKS_COMPACT
#include "dks.c"
```

This generates all functions prefixed with `mdsc`:

- `mdscnewlen()`, `mdsccat()`, `mdscfree()`, etc.

## Difference Between mds and mdsc

The key difference is in **header storage strategy**:

### mds (Full) - Rich Metadata

```c
/* Memory layout: [LEN][FREE_TYPE][BYTES...] */
typedef char mds;

/* Header sizes grow dynamically based on string size:
   - Can track more metadata
   - Slightly larger overhead
   - Better for frequently resized strings */
```

### mdsc (Compact) - Space Optimized

```c
/* Memory layout: [LEN][FREE_TYPE][BYTES...] (more aggressive packing) */
typedef char mdsc;

/* Header sizes optimized for minimal overhead:
   - Tighter packing
   - Less metadata space
   - Better for many small strings */
```

### Header Size Progression

Both variants use the same type progression, but `mdsc` packs more aggressively:

| Type   | Header Size | Max String Length | Max Free Space        |
| ------ | ----------- | ----------------- | --------------------- |
| DKS_8  | 2 bytes     | 255 bytes         | 63 bytes (2-bit type) |
| DKS_16 | 4 bytes     | 64 KB             | 16 KB (2-bit type)    |
| DKS_24 | 6 bytes     | 16 MB             | 2 MB (3-bit type)     |
| DKS_32 | 8 bytes     | 4 GB              | 536 MB (3-bit type)   |
| DKS_40 | 10 bytes    | 1 TB              | 137 GB (3-bit type)   |
| DKS_48 | 12 bytes    | 281 TB            | 35 TB (3-bit type)    |

Headers automatically upgrade when string grows beyond current type's capacity.

## Data Structure

```c
/* Both mds and mdsc use same internal structure */
typedef struct dksInfo {
    uint8_t *start;   /* Start of allocation */
    char *buf;        /* Start of string data */
    size_t len;       /* Current string length */
    size_t free;      /* Available space */
    dksType type;     /* Header type (DKS_8, DKS_16, etc.) */
} dksInfo;

/* Type uses 2 or 3 bits stored in the FREE_TYPE field */
typedef enum dksType {
    DKS_8 = 0x00,   /* 2-bit type */
    DKS_16 = 0x02,  /* 2-bit type */
    DKS_24 = 0x01,  /* 3-bit type */
    DKS_32 = 0x03,  /* 3-bit type */
    DKS_40 = 0x05,  /* 3-bit type */
    DKS_48 = 0x07,  /* 3-bit type */
} dksType;
```

## Creation Functions

### Create New String

```c
/* Create from buffer and length */
mds *mdsnewlen(const void *init, size_t initlen);
mdsc *mdscnewlen(const void *init, size_t initlen);

/* Create from null-terminated C string */
mds *mdsnew(const char *init);
mdsc *mdscnew(const char *init);

/* Create empty string */
mds *mdsempty(void);
mdsc *mdscempty(void);

/* Create empty with pre-allocated space */
mds *mdsemptylen(size_t len);
mdsc *mdscemptylen(size_t len);

/* Create from file */
mds *mdsNewFromFile(const char *filename);
mdsc *mdscNewFromFile(const char *filename);

/* Example usage */
mds *s1 = mdsnew("Hello");
mds *s2 = mdsnewlen("World", 5);
mds *s3 = mdsempty();
mds *s4 = mdsemptylen(1024);  /* Pre-allocate 1KB */

mdsc *compact = mdscnew("Compact string");
```

### Duplicate String

```c
/* Create copy of string */
mds *mdsdup(const mds *s);
mdsc *mdscdup(const mdsc *s);

/* Example usage */
mds *original = mdsnew("Original");
mds *copy = mdsdup(original);
/* Both strings are independent */
```

## Basic Operations

### String Length and Capacity

```c
/* Get current length */
size_t mdslen(const mds *s);
size_t mdsclen(const mdsc *s);

/* Get available free space */
size_t mdsavail(const mds *s);
size_t mdscavail(const mdsc *s);

/* Get total buffer allocation size */
size_t mdsbufallocsize(const mds *s);
size_t mdscbufallocsize(const mdsc *s);

/* Example usage */
mds *s = mdsnew("Hello");
printf("Length: %zu\n", mdslen(s));      /* 5 */
printf("Avail: %zu\n", mdsavail(s));     /* Varies */
printf("Total: %zu\n", mdsbufallocsize(s));  /* len + avail */
```

### Append Operations

```c
/* Append buffer */
mds *mdscatlen(mds *s, const void *t, size_t len);
mdsc *mdsccatlen(mdsc *s, const void *t, size_t len);

/* Append C string */
mds *mdscat(mds *s, const char *t);
mdsc *mdsccat(mdsc *s, const char *t);

/* Append another dks string */
mds *mdscatmds(mds *s, const mds *t);
mdsc *mdsccatmdsc(mdsc *s, const mdsc *t);

/* Append with comma checking */
mds *mdscatlencheckcomma(mds *s, const void *t, size_t len);
mdsc *mdsccatlencheckcomma(mdsc *s, const void *t, size_t len);

/* Example usage */
mds *s = mdsnew("Hello");
s = mdscat(s, " ");
s = mdscat(s, "World");
printf("%s\n", s);  /* "Hello World" */

mds *s2 = mdsnew("!");
s = mdscatmds(s, s2);
printf("%s\n", s);  /* "Hello World!" */
```

### Prepend Operations

```c
/* Prepend buffer to string */
mds *mdsprependlen(mds *s, const void *t, size_t len);
mdsc *mdscprependlen(mdsc *s, const void *t, size_t len);

/* Example usage */
mds *s = mdsnew("World");
s = mdsprependlen(s, "Hello ", 6);
printf("%s\n", s);  /* "Hello World" */
```

### Copy Operations

```c
/* Replace string content */
mds *mdscopylen(mds *s, const char *t, size_t len);
mdsc *mdsccopylen(mdsc *s, const char *t, size_t len);

mds *mdscopy(mds *s, const char *t);
mdsc *mdsccopy(mdsc *s, const char *t);

/* Example usage */
mds *s = mdsnew("Hello");
s = mdscopy(s, "Goodbye");
printf("%s\n", s);  /* "Goodbye" */
```

## Formatted Output

### Printf-style Formatting

```c
/* Append formatted string */
mds *mdscatprintf(mds *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
mdsc *mdsccatprintf(mdsc *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Append with va_list */
mds *mdscatvprintf(mds *s, const char *fmt, va_list ap);
mdsc *mdsccatvprintf(mdsc *s, const char *fmt, va_list ap);

/* Example usage */
mds *s = mdsnew("Count: ");
s = mdscatprintf(s, "%d", 42);
s = mdscatprintf(s, ", Value: %.2f", 3.14);
printf("%s\n", s);  /* "Count: 42, Value: 3.14" */
```

### Fast Custom Formatting

```c
/* Custom fast format specifiers:
   %s - C string
   %S - dks string
   %i - signed int
   %I - int64_t
   %u - unsigned int
   %U - uint64_t
   %% - literal %
*/
mds *mdscatfmt(mds *s, const char *fmt, ...);
mdsc *mdsccatfmt(mdsc *s, const char *fmt, ...);

/* Example usage */
mds *s = mdsempty();
s = mdscatfmt(s, "Number: %i, Large: %I", 42, 9223372036854775807LL);
printf("%s\n", s);
```

## String Manipulation

### Clear and Update

```c
/* Clear string (keep allocation) */
void mdsclear(mds *s);
void mdscclear(mdsc *s);

/* Update length after manual modification */
void mdsupdatelen(mds *s);
void mdscupdatelen(mdsc *s);

/* Force specific length */
void mdsupdatelenforce(mds *s, size_t newlen);
void mdscupdatelenforce(mdsc *s, size_t newlen);

/* Example usage */
mds *s = mdsnew("Hello");
mdsclear(s);
printf("Length: %zu\n", mdslen(s));  /* 0 */

/* Manual modification */
strcpy(s, "Modified");
mdsupdatelen(s);  /* Recalculate length */
```

### Trim and Range

```c
/* Trim characters from both ends */
mds *mdstrim(mds *s, const char *cset);
mdsc *mdsctrim(mdsc *s, const char *cset);

/* Extract range (modifies in place) */
void mdsrange(mds *s, ssize_t start, ssize_t end);
void mdscrange(mdsc *s, ssize_t start, ssize_t end);

/* Extract substring (modifies in place) */
void mdssubstr(mds *s, size_t start, size_t length);
void mdscsubstr(mdsc *s, size_t start, size_t length);

/* UTF-8 aware substring */
void mdssubstrutf8(mds *s, size_t start, size_t length);
void mdscsubstrutf8(mdsc *s, size_t start, size_t length);

/* Example usage */
mds *s = mdsnew("  Hello World  ");
s = mdstrim(s, " ");
printf("'%s'\n", s);  /* 'Hello World' */

mdsrange(s, 0, 4);
printf("'%s'\n", s);  /* 'Hello' */
```

### Case Conversion

```c
/* Convert to lowercase */
void mdstolower(mds *s);
void mdsctolower(mdsc *s);

/* Convert to uppercase */
void mdstoupper(mds *s);
void mdsctoupper(mdsc *s);

/* Example usage */
mds *s = mdsnew("Hello World");
mdstolower(s);
printf("%s\n", s);  /* "hello world" */

mdstoupper(s);
printf("%s\n", s);  /* "HELLO WORLD" */
```

### Character Mapping

```c
/* Replace characters */
void mdsmapchars(mds *s, const char *from, const char *to, size_t setlen);
void mdscmapchars(mdsc *s, const char *from, const char *to, size_t setlen);

/* Example usage */
mds *s = mdsnew("Hello World");
mdsmapchars(s, "lo", "LL", 2);
printf("%s\n", s);  /* "HeLLL WLrLd" */
```

## Splitting and Joining

### Split String

```c
/* Split by separator */
mds **mdssplitlen(const void *s, size_t len, const void *sep,
                 size_t seplen, size_t *count);
mdsc **mdscsplitlen(const void *s, size_t len, const void *sep,
                   size_t seplen, size_t *count);

/* Split with maximum count */
mds **mdssplitlenMax(const void *s, size_t len, const void *sep,
                    size_t seplen, size_t *count, size_t maxCount);
mdsc **mdscsplitlenMax(const void *s, size_t len, const void *sep,
                      size_t seplen, size_t *count, size_t maxCount);

/* Free split result */
void mdssplitlenfree(mds **tokens, size_t count);
void mdscsplitlenfree(mdsc **tokens, size_t count);

/* Example usage */
mds *s = mdsnew("apple,banana,cherry");
size_t count;
mds **tokens = mdssplitlen(s, mdslen(s), ",", 1, &count);

for (size_t i = 0; i < count; i++) {
    printf("Token %zu: %s\n", i, tokens[i]);
}
/* Output:
   Token 0: apple
   Token 1: banana
   Token 2: cherry */

mdssplitlenfree(tokens, count);
```

### Split Arguments (Shell-style)

```c
/* Split command-line style arguments */
mds **mdssplitargs(const char *line, int *argc);
mdsc **mdscsplitargs(const char *line, int *argc);

/* Example usage */
int argc;
mds **argv = mdssplitargs("command -f 'file name.txt' arg", &argc);

for (int i = 0; i < argc; i++) {
    printf("Arg %d: '%s'\n", i, argv[i]);
}
/* Output:
   Arg 0: 'command'
   Arg 1: '-f'
   Arg 2: 'file name.txt'
   Arg 3: 'arg' */

mdssplitlenfree(argv, argc);
```

### Join Strings

```c
/* Join array of strings */
mds *mdsjoin(char **argv, int argc, char *sep);
mdsc *mdscjoin(char **argv, int argc, char *sep);

/* Example usage */
char *words[] = {"apple", "banana", "cherry"};
mds *joined = mdsjoin(words, 3, ", ");
printf("%s\n", joined);  /* "apple, banana, cherry" */
```

## Comparison

```c
/* Compare two strings */
int mdscmp(const mds *s1, const mds *s2);
int mdsccmp(const mdsc *s1, const mdsc *s2);

/* Example usage */
mds *s1 = mdsnew("apple");
mds *s2 = mdsnew("banana");
if (mdscmp(s1, s2) < 0) {
    printf("s1 comes before s2\n");
}
```

## Memory Management

### Expand and Shrink

```c
/* Pre-allocate space (with automatic size calculation) */
mds *mdsexpandby(mds *s, size_t addlen);
mdsc *mdscexpandby(mdsc *s, size_t addlen);

/* Pre-allocate exact amount */
mds *mdsexpandbyexact(mds *s, size_t addlen);
mdsc *mdscexpandbyexact(mdsc *s, size_t addlen);

/* Grow with zeros */
mds *mdsgrowzero(mds *s, size_t len);
mdsc *mdscgrowzero(mdsc *s, size_t len);

/* Remove free space */
mds *mdsremovefreespace(mds *s);
mdsc *mdscremovefreespace(mdsc *s);

/* Example usage */
mds *s = mdsempty();
s = mdsexpandby(s, 1024);  /* Reserve 1KB */
printf("Available: %zu\n", mdsavail(s));  /* ~1024 */

/* After many appends... */
s = mdsremovefreespace(s);  /* Shrink to exact size */
```

### Manually Adjust Length

```c
/* Increment/decrement length */
void mdsIncrLen(mds *s, ssize_t incr);
void mdscIncrLen(mdsc *s, ssize_t incr);

/* Example usage */
mds *s = mdsemptylen(10);
memcpy(s, "Hello", 5);
mdsIncrLen(s, 5);  /* Notify we wrote 5 bytes */
printf("%s\n", s);  /* "Hello" */
```

### Free Memory

```c
/* Free string */
void mdsfree(mds *s);
void mdscfree(mdsc *s);

/* Free and zero memory */
void mdsfreezero(mds *s);
void mdscfreezero(mdsc *s);

/* Convert to native malloc'd buffer */
uint8_t *mdsnative(mds *s, size_t *len);
uint8_t *mdscnative(mdsc *s, size_t *len);

/* Example usage */
mds *s = mdsnew("Hello");
mdsfree(s);

/* Secure erase */
mds *secret = mdsnew("password123");
mdsfreezero(secret);  /* Zeros memory before freeing */

/* Convert to raw buffer */
mds *s2 = mdsnew("Data");
size_t len;
uint8_t *buf = mdsnative(s2, &len);
/* buf is now a regular malloc'd buffer */
/* s2 is no longer valid */
free(buf);
```

## Hashing

```c
/* XXH64 hash */
uint64_t mdsXXH64(const mds *s, uint64_t seed);
uint64_t mdscXXH64(const mdsc *s, uint64_t seed);

/* XXH3 64-bit hash */
uint64_t mdsXXH3_64(const mds *s, uint64_t seed);
uint64_t mdscXXH3_64(const mdsc *s, uint64_t seed);

/* XXH3 128-bit hash */
__uint128_t mdsXXH3_128(const mds *s, uint64_t seed);
__uint128_t mdscXXH3_128(const mdsc *s, uint64_t seed);

/* Example usage */
mds *s = mdsnew("Hello World");
uint64_t hash = mdsXXH3_64(s, 0);
printf("Hash: 0x%016lx\n", hash);
```

## UTF-8 Support

```c
/* Get UTF-8 character count */
size_t mdslenutf8(mds *s);
size_t mdsclenutf8(mdsc *s);

/* Example usage */
mds *s = mdsnew("Hello 💛");
printf("Bytes: %zu\n", mdslen(s));        /* 10 */
printf("Characters: %zu\n", mdslenutf8(s));  /* 7 */
```

## Utility Functions

### From Integer

```c
/* Create string from integer */
mds *mdsfromint64(int64_t value);
mdsc *mdscfromint64(int64_t value);

/* Example usage */
mds *s = mdsfromint64(12345);
printf("%s\n", s);  /* "12345" */
```

### Get Absolute Path

```c
/* Convert filename to absolute path */
mds *mdsgetabsolutepath(char *filename);
mdsc *mdscgetabsolutepath(char *filename);

/* Example usage */
mds *path = mdsgetabsolutepath("./file.txt");
printf("Absolute: %s\n", path);
```

### Format IP Address

```c
/* Format IP and port */
mds *mdsformatip(char *ip, int port);
mdsc *mdscformatip(char *ip, int port);

/* Example usage */
mds *addr = mdsformatip("192.168.1.1", 8080);
printf("Address: %s\n", addr);  /* "192.168.1.1:8080" */
```

## Real-World Examples

### Example 1: Build HTTP Request

```c
mds *buildHttpRequest(const char *method, const char *path,
                      const char *host, const char *body) {
    mds *req = mdsempty();

    /* Request line */
    req = mdscatfmt(req, "%s %s HTTP/1.1\r\n", method, path);

    /* Headers */
    req = mdscatfmt(req, "Host: %s\r\n", host);
    req = mdscatfmt(req, "Content-Length: %U\r\n",
                    (uint64_t)(body ? strlen(body) : 0));
    req = mdscat(req, "\r\n");

    /* Body */
    if (body) {
        req = mdscat(req, body);
    }

    return req;
}

/* Usage */
mds *request = buildHttpRequest("GET", "/api/users",
                                "example.com", NULL);
printf("%s", request);
mdsfree(request);
```

### Example 2: Parse CSV Line

```c
void parseCSV(const char *line) {
    size_t count;
    mds **fields = mdssplitlen(line, strlen(line), ",", 1, &count);

    for (size_t i = 0; i < count; i++) {
        /* Trim whitespace */
        fields[i] = mdstrim(fields[i], " \t");
        printf("Field %zu: '%s'\n", i, fields[i]);
    }

    mdssplitlenfree(fields, count);
}
```

### Example 3: Build JSON Safely

```c
mds *buildJSON(const char *name, int age) {
    mds *json = mdsempty();

    json = mdscat(json, "{");

    /* Add name field */
    json = mdscat(json, "\"name\": \"");
    json = mdscatrepr(json, name, strlen(name));  /* Escape special chars */
    json = mdscat(json, "\", ");

    /* Add age field */
    json = mdscatfmt(json, "\"age\": %i", age);

    json = mdscat(json, "}");

    return json;
}
```

### Example 4: When to Use mds vs mdsc

```c
/* Use mds for strings that grow/shrink frequently */
mds *log = mdsempty();
for (int i = 0; i < 1000; i++) {
    log = mdscatprintf(log, "Entry %d\n", i);
    /* mds handles frequent growth efficiently */
}

/* Use mdsc for many small, static strings */
mdsc *tags[10000];
for (int i = 0; i < 10000; i++) {
    tags[i] = mdscnew("tag");
    /* mdsc uses less memory per string */
}
```

## Performance Characteristics

| Operation      | Complexity | Notes                              |
| -------------- | ---------- | ---------------------------------- |
| mdsnew/mdscnew | O(n)       | n = initial length                 |
| mdscat         | O(n)       | Amortized O(1) with pre-allocation |
| mdscatprintf   | O(n)       | n = formatted output length        |
| mdssplitlen    | O(n)       | n = string length                  |
| mdscmp         | O(n)       | n = min(len1, len2)                |
| Header upgrade | O(n)       | Only when crossing type boundary   |

## Memory Overhead

| String Size | DKS_8         | DKS_16         | DKS_24            | DKS_32 |
| ----------- | ------------- | -------------- | ----------------- | ------ |
| 10 bytes    | 2 bytes (20%) | -              | -                 | -      |
| 100 bytes   | -             | 4 bytes (4%)   | -                 | -      |
| 1 KB        | -             | 4 bytes (0.4%) | -                 | -      |
| 1 MB        | -             | -              | 6 bytes (0.0006%) | -      |

## Thread Safety

DKS strings are **not thread-safe**. Concurrent access requires external synchronization:

```c
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
mds *shared;

pthread_mutex_lock(&lock);
shared = mdscat(shared, "data");
pthread_mutex_unlock(&lock);
```

## See Also

- [STR](STR.md) - Core string utilities
- [STR_DOUBLE_FORMAT](STR_DOUBLE_FORMAT.md) - Fast double formatting
- Original inspiration: [sds (Simple Dynamic Strings)](https://github.com/antirez/sds)

## Testing

Run the dks test suite:

```bash
./src/datakit-test test mds
./src/datakit-test test mdsc
```

The test suite validates:

- Header type progression
- Memory management
- String operations
- Edge cases
- Performance benchmarks
