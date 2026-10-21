# dks - Dynamic String Template System

## Overview

`dks` is a **template-based dynamic string buffer system** that generates two implementations from a single codebase: `mds` (full) and `mdsc` (compact). It provides efficient string manipulation with automatic memory management and space-optimized headers.

**Key Features:**

- Template system generates `mds` (full) and `mdsc` (compact) from single source
- Automatic header size selection: 1-6 bytes for `mdsc`, 2-12 bytes for `mds`
- Binary-safe string operations
- Printf-style formatting
- String splitting and joining
- Extensive UTF-8 support (codepoints, grapheme clusters, display width)
- Built-in hashing (XXH64, XXH3-64, XXH3-128)

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

The key difference is **how many header fields there are**, and therefore
whether free space is tracked at all.

### mds (Full) - Tracks Free Space

```c
/* Memory layout: [LENGTH][FREE + TYPE][BYTES...][NUL] */
typedef char mds;
```

Two equal-sized header elements, so the header is **twice** the element
size. The trailing element holds the free-space count with the type bits
packed into its low bits; `mdsnewlen` also rounds the allocation up through
`jebufSizeAllocation`, so an `mds` normally starts life with slack. That
slack is what makes repeated appends cheap.

### mdsc (Compact) - No Free Space At All

```c
/* Memory layout: [LENGTH + TYPE][BYTES...][NUL] */
typedef char mdsc;
```

**One** header element, so the header is exactly the element size, and the
type bits are packed into the length field itself. There is no free-space
field: `mdscavail()` always returns 0 and `mdscbufallocsize()` always equals
`mdsclen()`. Every mutation resizes to exactly what is needed.

That is the real trade: `mdsc` is smaller per string and cheaper to hold in
bulk; `mds` is cheaper to grow.

### Header Size Progression

```c
typedef enum dksType {
    /* 2-bit types (the byte ends in 0) */
    DKS_8  = 0x00,
    DKS_16 = 0x02,
    /* 3-bit types (the byte ends in 1) */
    DKS_24 = 0x01,
    DKS_32 = 0x03,
    DKS_40 = 0x05,
    DKS_48 = 0x07,
} dksType;
```

The element width per type is 1, 2, 3, 4, 5, and 6 bytes respectively. What
each variant can hold differs because the type bits are stolen from
different fields:

**mds (full)** — header is 2 elements; `length` gets the whole element,
`free` loses the type bits:

| Type   | Header   | Max length | Max free space |
| ------ | -------- | ---------- | -------------- |
| DKS_8  | 2 bytes  | 255        | 63             |
| DKS_16 | 4 bytes  | 65535      | 16383          |
| DKS_24 | 6 bytes  | 16 MiB     | 2 MiB          |
| DKS_32 | 8 bytes  | 4 GiB      | 512 MiB        |
| DKS_40 | 10 bytes | 1 TiB      | 128 GiB        |
| DKS_48 | 12 bytes | 256 TiB    | 32 TiB         |

**mdsc (compact)** — header is 1 element and `length` itself loses the type
bits, so the reachable length per type is the _shared_ maximum:

| Type   | Header  | Max length |
| ------ | ------- | ---------- |
| DKS_8  | 1 byte  | 63         |
| DKS_16 | 2 bytes | 16383      |
| DKS_24 | 3 bytes | 2 MiB      |
| DKS_32 | 4 bytes | 512 MiB    |
| DKS_40 | 5 bytes | 128 GiB    |
| DKS_48 | 6 bytes | 32 TiB     |

There are deliberately no DKS_56 or DKS_64 types: DKS_48 already addresses
256 TiB as a full length and 32 TiB as a shared one.

Headers upgrade automatically when a string grows past its current type's
capacity — the upgrade rewrites the header, so it is O(n).

## Data Structure

A `mds *` / `mdsc *` **is a `char *` pointing at the string bytes** — the
header sits immediately _before_ it. That is what makes a dks string
directly usable with `printf("%s")` and `memcmp`, and it is why every
mutating call returns a (possibly new) pointer you must reassign.

Internally both variants decode their header into the same scratch struct:

```c
typedef struct dksInfo {
    uint8_t *start;   /* Start of the allocation (before the header) */
    DKS_TYPE *buf;    /* Start of the string data (what callers hold) */
    size_t len;       /* Current string length */
    size_t free;      /* Available space — always 0 for mdsc */
    dksType type;     /* Header type (DKS_8, DKS_16, ...) */
} dksInfo;
```

The type bits live in the low bits of the header element that precedes the
string data: the free-space field for `mds`, the length field for `mdsc`.
Types whose value ends in `0` use 2 type bits, those ending in `1` use 3.

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
printf("Length: %zu\n", mdslen(s));          /* 5 */
printf("Avail: %zu\n", mdsavail(s));         /* varies with the allocator */
printf("Total: %zu\n", mdsbufallocsize(s));  /* len + avail */

/* On mdsc there is no free-space field at all */
mdsc *c = mdscnew("Hello");
assert(mdscavail(c) == 0);
assert(mdscbufallocsize(c) == mdsclen(c));
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
   %s - NUL-terminated C string
   %S - dks string (length taken from its header, so binary-safe)
   %i - int32_t
   %I - int64_t
   %u - uint32_t
   %U - uint64_t
   %b - databox, appended as raw bytes
   %B - databox, rendered by type (bytes, signed, unsigned, float, double)
   %% - literal %
   Any other %<char> emits that character literally.
*/
mds *mdscatfmt(mds *s, const char *fmt, ...);
mdsc *mdsccatfmt(mdsc *s, const char *fmt, ...);

/* Example usage */
mds *s = mdsempty();
s = mdscatfmt(s, "Number: %i, Large: %I", 42, INT64_MAX);
printf("%s\n", s);

/* %B renders a databox by its type, using the fast integer and
 * double formatters rather than snprintf */
const databox box = DATABOX_DOUBLE(3.5);
s = mdscatfmt(s, " value=%B", &box);
```

`mdscatfmt` is not `printf`: it has no width, precision, or padding, and it
does no type checking. That is the point — each specifier dispatches
straight to a fast writer. Use `mdscatprintf` when you need real `printf`
semantics (it carries the `format(printf, 2, 3)` attribute, so the compiler
checks those calls).

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

## Escaping and Quoting

```c
/* Append 'p' with non-printable bytes escaped, wrapped in quotes */
mds *mdscatrepr(mds *s, const char *p, size_t len);
mdsc *mdsccatrepr(mdsc *s, const char *p, size_t len);

/* Append raw, quoting only when needed; 'append' is a trailing
 * separator byte to emit (0 for none) */
mds *mdscatlenquoteraw(mds *s, const void *t, size_t len, uint8_t append);
mds *mdscatlennoquoteraw(mds *s, const void *t, size_t len, uint8_t append);

/* Total allocation size including the header */
size_t mdsallocsize(mds *s);
size_t mdscallocsize(mdsc *s);
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

/* Secure erase: wipes the header, contents, free space and terminator,
 * plus the allocator's slack past them when the allocator reports its
 * usable size (datakitConfig.localUsableSize: the libc default on macOS,
 * Linux and FreeBSD, and memshard), with dkSecureZero, then frees.
 * Without that report, bytes the string no longer tracks are not covered:
 * an mdsc shrunk after growing, or free space mdsclear() had to hide.
 * Memory a shrinking realloc already gave back is never covered, so wipe
 * a secret before shrinking its string. */
mds *secret = mdsnew("password123");
mdsfreezero(secret);

/* Convert to a plain buffer: the string bytes are moved to the front of
 * the existing allocation and the base pointer is returned. */
mds *s2 = mdsnew("Data");
size_t len = 0;
uint8_t *buf = mdsnative(s2, &len);
/* s2 is no longer valid.  'buf' is the ORIGINAL allocation, so it is
 * still zmalloc-owned and still sized for the old header — release it
 * with zfree(), and zrealloc() it down yourself if you want it tight. */
zfree(buf);
```

`mdsnative` deliberately does not shrink the allocation: the caller may not
want the `realloc`, and it returns `*len` precisely so the caller can decide.

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
printf("Hash: 0x%016" PRIx64 "\n", hash);
```

## UTF-8 Support

Every `Str*` UTF-8 helper documented in
[STR_UTILS](STR_UTILS.md) has a dks-flavored wrapper that takes the string's
length from its header instead of a separate `len` argument. Shown here with
the `mds` prefix; `mdsc` versions exist for all of them.

```c
/* Length and validation */
size_t mdslenutf8(mds *s);
bool mdsutf8valid(const mds *s);
size_t mdsutf8validcount(const mds *s, bool *valid);

/* Cursor movement */
const uint8_t *mdsutf8advance(const mds *s, size_t n);
const uint8_t *mdsutf8retreat(const mds *s, const uint8_t *pos, size_t n);
uint32_t mdsutf8peek(const mds *s, const uint8_t *pos);
size_t mdsutf8offsetat(const mds *s, size_t charIndex);
size_t mdsutf8indexat(const mds *s, size_t byteOffset);

/* Truncation and substrings */
size_t mdsutf8truncate(const mds *s, size_t maxChars);
size_t mdsutf8truncatebytes(const mds *s, size_t maxBytes);
void mdsutf8substring(const mds *s, size_t startChar, size_t endChar,
                      size_t *outOffset, size_t *outLen);
size_t mdsutf8substringcopy(const mds *s, size_t startChar, size_t endChar,
                            void *buf, size_t bufLen);
size_t mdsutf8split(const mds *s, size_t charIndex);

/* Comparison */
int mdsutf8compare(const mds *s1, const mds *s2);
int mdsutf8comparen(const mds *s1, const mds *s2, size_t n);
int mdsutf8comparecaseinsensitiveascii(const mds *s1, const mds *s2);
bool mdsutf8startswith(const mds *s, const void *prefix, size_t prefixLen);
bool mdsutf8endswith(const mds *s, const void *suffix, size_t suffixLen);
bool mdsutf8equal(const mds *s1, const mds *s2);
bool mdsutf8equalcaseinsensitiveascii(const mds *s1, const mds *s2);

/* Search (byte offset, or SIZE_MAX when not found) */
size_t mdsutf8find(const mds *s, const void *needle, size_t needleLen);
size_t mdsutf8findlast(const mds *s, const void *needle, size_t needleLen);
size_t mdsutf8findchar(const mds *s, uint32_t codepoint);
size_t mdsutf8findcharlast(const mds *s, uint32_t codepoint);
size_t mdsutf8findcharnth(const mds *s, uint32_t codepoint, size_t n);
bool mdsutf8contains(const mds *s, const void *needle, size_t needleLen);
size_t mdsutf8count(const mds *s, const void *needle, size_t needleLen);
size_t mdsutf8countchar(const mds *s, uint32_t codepoint);
size_t mdsutf8findanychar(const mds *s, const void *charSet, size_t charSetLen);
size_t mdsutf8findnotchar(const mds *s, const void *charSet, size_t charSetLen);
size_t mdsutf8spanchar(const mds *s, const void *charSet, size_t charSetLen);
size_t mdsutf8spannotchar(const mds *s, const void *charSet, size_t charSetLen);

/* Terminal display width (East Asian Width aware) */
size_t mdsutf8width(const mds *s);
size_t mdsutf8widthn(const mds *s, size_t n);
size_t mdsutf8truncatewidth(const mds *s, size_t maxWidth);
size_t mdsutf8indexatwidth(const mds *s, size_t targetWidth);
size_t mdsutf8padwidth(const mds *s, size_t targetWidth);
bool mdsutf8isnarrow(const mds *s);
bool mdsutf8haswide(const mds *s);

/* Grapheme clusters (what a user perceives as one character) */
size_t mdsutf8graphemenext(const mds *s);
size_t mdsutf8graphemecount(const mds *s);
size_t mdsutf8graphemeadvance(const mds *s, size_t n);
bool mdsutf8graphemeat(const mds *s, size_t n, size_t *startOut,
                       size_t *endOut);
size_t mdsutf8graphemewidth(const mds *s);
size_t mdsutf8graphemetruncate(const mds *s, size_t n);
```

```c
/* Example usage */
mds *s = mdsnew("Hello 💛");
printf("Bytes: %zu\n", mdslen(s));           /* 10 */
printf("Codepoints: %zu\n", mdslenutf8(s));  /* 7  */
printf("Cells: %zu\n", mdsutf8width(s));     /* 8 — the emoji is wide */
```

Three different "lengths" are in play and they are routinely different:
**bytes** (`mdslen`), **codepoints** (`mdslenutf8`), and **display cells**
(`mdsutf8width`). A grapheme cluster adds a fourth
(`mdsutf8graphemecount`) — a base character plus its combining marks counts
as one. Pick the one that matches what you are doing: buffer sizing wants
bytes, indexing wants codepoints, terminal layout wants cells, and
user-visible truncation wants graphemes.

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
/* mds: tracks free space, so repeated appends reuse slack instead of
 * reallocating on every call */
mds *log = mdsempty();
for (int i = 0; i < 1000; i++) {
    log = mdscatprintf(log, "Entry %d\n", i);
}

/* mdsc: one-element header and no free-space field, so each string
 * costs a few bytes less — but every append resizes exactly. */
mdsc *tags[10000];
for (int i = 0; i < 10000; i++) {
    tags[i] = mdscnew("tag");   /* 1-byte header for a 3-byte string */
}
```

Rule of thumb: **`mds` for strings you build, `mdsc` for strings you keep.**
Appending in a loop to an `mdsc` is the one pattern to avoid — with no free
space to consume, it reallocates every time.

## Performance Characteristics

| Operation             | Complexity | Notes                                     |
| --------------------- | ---------- | ----------------------------------------- |
| mdsnew/mdscnew        | O(n)       | n = initial length                        |
| mdslen/mdsclen        | O(1)       | decoded from the header, never a `strlen` |
| mdscat (mds)          | O(n)       | amortized O(1) while free space lasts     |
| mdsccat (mdsc)        | O(n)       | always reallocates: no free-space field   |
| mdscatprintf          | O(n)       | n = formatted output length               |
| mdssplitlen           | O(n)       | n = string length                         |
| mdscmp                | O(n)       | n = min(len1, len2)                       |
| Header upgrade        | O(n)       | only when crossing a type boundary        |
| UTF-8 width/graphemes | O(n)       | must decode; no length field shortcut     |

## Memory Overhead

Header cost for a string of a given length (the `mds` allocation is also
rounded up by `jebufSizeAllocation`, so its real footprint is larger):

| String length | mdsc header | mds header |
| ------------- | ----------- | ---------- |
| 10 bytes      | 1 byte      | 2 bytes    |
| 100 bytes     | 2 bytes     | 2 bytes    |
| 1 KiB         | 2 bytes     | 4 bytes    |
| 1 MiB         | 3 bytes     | 6 bytes    |
| 1 GiB         | 4 bytes     | 8 bytes    |

`mdsc` crosses to DKS_16 at 64 bytes (its 1-byte length field loses 2 bits
to the type), while `mds` keeps its 2-byte header all the way to 255.

## Thread Safety

DKS strings are **not thread-safe**. Concurrent access requires external synchronization:

```c
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
mds *shared = mdsempty();

pthread_mutex_lock(&lock);
shared = mdscat(shared, "data");   /* must reassign: the pointer may move */
pthread_mutex_unlock(&lock);
```

Note the reassignment. Every growing operation may reallocate, so the
returned pointer — not the one you passed in — is the valid string.

## See Also

- [STR](STR.md) - Scanning, conversion, and numeric formatting primitives
- [STR_UTILS](STR_UTILS.md) - The `Str*` UTF-8, popcount, and bitmap helpers
  that the dks UTF-8 wrappers call
- [STR_DOUBLE_FORMAT](STR_DOUBLE_FORMAT.md) - Fast double formatting

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
