# Struct Memory Optimization Guide

This guide documents the struct memory optimization process for the varint library, including tools, techniques, and best practices for maintaining optimal memory layout.

## Table of Contents

1. [Overview](#overview)
2. [Quick Start](#quick-start)
3. [Optimization Tools](#optimization-tools)
4. [Analysis Results](#analysis-results)
5. [Best Practices](#best-practices)
6. [Cache Line Considerations](#cache-line-considerations)
7. [Maintenance & Regression Prevention](#maintenance--regression-prevention)

---

## Overview

Struct memory layout optimization focuses on two key goals:

1. **Eliminate padding** - Minimize wasted space between struct fields
2. **Optimize cache locality** - Keep frequently-accessed fields within cache line boundaries (64 bytes on modern x86-64 CPUs)

### Why This Matters

- **Memory efficiency**: Padding can waste 10-30% of struct size
- **Performance**: Cache misses can be 100x slower than L1 cache hits
- **Binary size**: Smaller structs = smaller binaries
- **Bandwidth**: Less memory traffic improves bandwidth utilization

---

## Quick Start

### Run Complete Analysis

```bash
cd tools/
./struct_pahole_analyzer.sh
```

This will show:

- DWARF-based struct layout from `pahole`
- Padding waste analysis
- Cache line boundary crossings
- Optimization recommendations

### Verify No Regressions

```bash
cd tools/
gcc -I../src -o struct_size_check struct_size_check.c
./struct_size_check
```

Expected output:

```
varintFORMeta                 :  48 bytes (expected <=  48) ✓
varintPFORMeta                :  40 bytes (expected <=  48) ✓
...
Total bytes saved:         8 (2.1% reduction)
```

---

## Optimization Tools

### 1. `pahole` - DWARF Debug Info Analyzer

**Installation:**

```bash
# Debian/Ubuntu
sudo apt-get install dwarves

# Red Hat/CentOS/Fedora
sudo yum install dwarves
```

**Basic Usage:**

```bash
# Compile with debug symbols
gcc -g3 -O0 -I../src -o struct_audit_dwarf struct_audit.c -lm

# Analyze specific struct
pahole -C varintPFORMeta struct_audit_dwarf
```

**Output Example:**

```
struct varintPFORMeta {
	uint64_t                   min;                  /*     0     8 */
	uint64_t                   exceptionMarker;      /*     8     8 */
	uint64_t                   thresholdValue;       /*    16     8 */
	varintWidth                width;                /*    24     4 */
	uint32_t                   count;                /*    28     4 */
	uint32_t                   exceptionCount;       /*    32     4 */
	uint32_t                   threshold;            /*    36     4 */

	/* size: 40, cachelines: 1, members: 7 */
	/* last cacheline: 40 bytes */
};
```

**Key Features:**

- Shows exact byte offsets and sizes
- Identifies padding bytes
- Shows cache line boundaries (`/* --- cacheline 1 boundary (64 bytes) --- */`)
- Calculates total size and cacheline count

**Advanced Commands:**

```bash
# Show automatic reorganization suggestions
pahole --reorganize struct_audit_dwarf

# Show reorganization steps
pahole --show_reorg_steps struct_audit_dwarf

# Expand nested structs/unions
pahole -E -C varintAdaptiveMeta struct_audit_dwarf

# List all structs with sizes
pahole --sizes struct_audit_dwarf | grep varint
```

### 2. `struct_pahole_analyzer.sh` - Automated Analysis Script

**Location:** `tools/struct_pahole_analyzer.sh`

**Features:**

- Analyzes all varint structs automatically
- Color-coded output (green = good, yellow = warning, red = needs attention)
- Cache line analysis with boundary detection
- Padding efficiency calculations
- Actionable recommendations

**Usage:**

```bash
cd tools/
./struct_pahole_analyzer.sh
```

### 3. `struct_audit.c` - Manual Field Analysis

**Location:** `tools/struct_audit.c`

**Compile & Run:**

```bash
cd tools/
gcc -g -I../src -o struct_audit struct_audit.c -lm
./struct_audit
```

**Features:**

- Uses `offsetof()` and `sizeof()` for field-by-field analysis
- Visual field layout tables
- Optimization recommendations based on field sizes
- Before/after comparisons

### 4. `struct_size_check.c` - Regression Testing

**Location:** `tools/struct_size_check.c`

**Purpose:** Verify struct sizes haven't regressed after optimization

**Compile & Run:**

```bash
cd tools/
gcc -I../src -o struct_size_check struct_size_check.c
./struct_size_check
```

---

## Analysis Results

### Optimization Summary

| Struct                    | Before  | After   | Savings | Cache Lines | Efficiency |
| ------------------------- | ------- | ------- | ------- | ----------- | ---------- |
| `varintFORMeta`           | 48      | 48      | 0       | 1           | 91.7%      |
| `varintPFORMeta`          | 48      | 40      | **8**   | 1           | **100%** ✓ |
| `varintFloatMeta`         | 48      | 48      | 0       | 1           | 87.5%      |
| `varintAdaptiveDataStats` | 80      | 80      | 0       | 2           | 93.8%      |
| `varintAdaptiveMeta`      | 72      | 72      | 0       | 2           | 94.4%      |
| `varintDictStats`         | 56      | 56      | 0       | 1           | **100%** ✓ |
| `varintBitmapStats`       | 24      | 24      | 0       | 1           | 83.3%      |
| **TOTAL**                 | **376** | **368** | **8**   | -           | **97.9%**  |

### Key Achievements

1. **varintPFORMeta**: Perfect 100% efficiency (no padding)
2. **varintDictStats**: Perfect 100% efficiency (no padding)
3. **Most structs** fit in single cache line (excellent locality)
4. **8 bytes total savings** across all metadata structs

### Structs Spanning Multiple Cache Lines

Two structs span 2 cache lines due to their size:

#### `varintAdaptiveDataStats` (80 bytes)

```
Cacheline 1 (bytes 0-63):  HOT fields
  - count, minValue, maxValue, range, uniqueCount, avgDelta, maxDelta, outlierCount

Cacheline 2 (bytes 64-79): COLD fields
  - uniqueRatio, outlierRatio, isSorted, isReverseSorted, fitsInBitmapRange
```

**Rationale:** Hot fields (frequently accessed for encoding decisions) are in the first cache line. Cold fields (computed statistics for analysis) are in the second.

#### `varintAdaptiveMeta` (72 bytes)

```
Cacheline 1 (bytes 0-63):  Frequently accessed
  - originalCount, encodedSize, encodingMeta union (48 bytes)

Cacheline 2 (bytes 64-71): Read once at decode
  - encodingType
```

**Rationale:** The union containing FOR/PFOR metadata is the bulk of the struct and must be in the first cache line. `encodingType` is read once during decode header parsing.

---

## Best Practices

### Field Ordering Rules

1. **Order by size** (largest to smallest):
   - 8-byte: `uint64_t`, `size_t`, `double`, pointers (on 64-bit)
   - 4-byte: `uint32_t`, `int`, `float`, `varintWidth`
   - 2-byte: `uint16_t`, `short`
   - 1-byte: `uint8_t`, `char`, `bool`

2. **Group same-sized fields** together

3. **Place hot fields first** (frequently accessed together)

4. **Place cold fields last** (rarely accessed or independent)

### Example: Before vs After

**BEFORE** (inefficient):

```c
typedef struct {
    uint64_t minValue;       /*  0 -  7 */
    varintWidth offsetWidth; /*  8 - 11 */  ← 4-byte field
    /* padding: 4 bytes */    /* 12 - 15 */  ← WASTED!
    uint64_t maxValue;       /* 16 - 23 */
    size_t count;            /* 24 - 31 */
} BadStruct;  // 32 bytes (4 bytes padding)
```

**AFTER** (optimized):

```c
typedef struct {
    uint64_t minValue;       /*  0 -  7 */
    uint64_t maxValue;       /*  8 - 15 */
    size_t count;            /* 16 - 23 */
    varintWidth offsetWidth; /* 24 - 27 */  ← Moved to end
} GoodStruct;  // 28 bytes (no padding!)
```

### Adding Comments

Document the optimization reasoning:

```c
/* Metadata structure for XYZ encoding
 * Fields ordered by size (8-byte → 4-byte → 1-byte) to eliminate padding
 * Hot fields (frequently accessed during encode) placed first for cache locality */
typedef struct StructName {
    /* 8-byte fields: Hot path */
    uint64_t field1;
    size_t field2;

    /* 4-byte fields */
    uint32_t field3;
    varintWidth field4;

    /* 1-byte fields: Cold path */
    bool flag1;
    uint8_t field5;
} StructName;
```

---

## Cache Line Considerations

### Cache Line Basics

Modern x86-64 CPUs use **64-byte cache lines**:

- Data is loaded from RAM in 64-byte chunks
- Cache miss penalty: ~100 cycles (L1) to ~300 cycles (RAM)
- Cache hit: ~4 cycles

### Optimization Strategies

1. **Keep hot structs under 64 bytes**
   - Entire struct fits in one cache line
   - All fields accessed with zero cache misses

2. **Group hot fields at beginning**
   - Most frequently accessed fields in first 64 bytes
   - Rarely-used fields can cross cache line boundary

3. **Align critical structs to cache lines**

   ```c
   struct HotStruct {
       // ...
   } __attribute__((aligned(64)));  // Force 64-byte alignment
   ```

4. **Consider splitting large structs**
   - If different parts accessed separately
   - Example: Separate "hot" metadata from "cold" statistics

### Checking Cache Line Usage

```bash
pahole -C StructName binary | grep "cacheline.*boundary"
```

If you see:

```
/* --- cacheline 1 boundary (64 bytes) --- */
```

→ Struct spans multiple cache lines. Review field ordering!

---

## Maintenance & Regression Prevention

### 1. Add Static Assertions

**All varint metadata structs now have compile-time size guarantees!**

Prevent accidental size regressions with `_Static_assert`:

```c
/* Example from varintPFOR.h - 100% efficient struct */
_Static_assert(sizeof(varintPFORMeta) == 40,
    "varintPFORMeta size changed! Expected 40 bytes (3×8-byte + 4×4-byte, ZERO padding). "
    "This struct achieved 100% efficiency - do not break it!");
_Static_assert(sizeof(varintPFORMeta) <= 64,
    "varintPFORMeta exceeds single cache line (64 bytes)! "
    "Keep this struct cache-friendly for hot encoding paths.");
```

**Benefits:**

- **Compile-time verification** - Catches regressions immediately
- **Self-documenting** - Error messages explain the expected layout
- **Cache line awareness** - Second assertion enforces single-cacheline requirement

**Where to add:**
Place `_Static_assert` statements immediately after the struct definition, before any function declarations.

**All assertions are in place for:**

- ✅ varintFORMeta (48 bytes)
- ✅ varintPFORMeta (40 bytes) - 100% efficient
- ✅ varintFloatMeta (48 bytes)
- ✅ varintAdaptiveDataStats (80 bytes)
- ✅ varintAdaptiveMeta (72 bytes)
- ✅ varintDictStats (56 bytes) - 100% efficient
- ✅ varintBitmapStats (24 bytes)

**What happens if you break it:**

```c
// If you accidentally add a field incorrectly:
typedef struct varintPFORMeta {
    uint32_t count;         /* BAD: 4-byte field first causes padding */
    uint64_t min;
    // ...
} varintPFORMeta;

// Compiler error at build time:
// error: static assertion failed: "varintPFORMeta size changed!
//        Expected 40 bytes (3×8-byte + 4×4-byte, ZERO padding).
//        This struct achieved 100% efficiency - do not break it!"
```

### 2. Run Analysis on Every Struct Addition

When adding a new struct:

```bash
# 1. Add struct to tools/struct_pahole_analyzer.sh
vim tools/struct_pahole_analyzer.sh  # Add to STRUCTS array

# 2. Run analysis
./tools/struct_pahole_analyzer.sh

# 3. Add size check
vim tools/struct_size_check.c  # Add CHECK_SIZE() line

# 4. Verify
gcc -I../src -o struct_size_check struct_size_check.c
./struct_size_check
```

### 3. Include in CI/CD

Add to `.github/workflows` or CI config:

```yaml
- name: Check struct memory efficiency
  run: |
    cd tools
    gcc -I../src -o struct_size_check struct_size_check.c
    ./struct_size_check
    # Fail if structs grew
    test $(./struct_size_check | grep "Total bytes after" | awk '{print $4}') -le 368
```

### 4. Document Changes in Commit Messages

```
Optimize varintXYZMeta struct layout

- Reordered fields by size (8-byte → 4-byte → 1-byte)
- Moved offsetWidth to end to eliminate 4 bytes padding
- Reduced from 48 to 44 bytes (8.3% reduction)
- All fields still in single cache line (44 < 64 bytes)

Verified with:
  pahole -C varintXYZMeta struct_audit_dwarf

Before: 48 bytes (91.7% efficient)
After:  44 bytes (100% efficient)
```

---

## Advanced Topics

### Understanding Alignment

Compilers align fields to their natural boundary:

- `uint64_t` aligns to 8-byte boundary (offset must be multiple of 8)
- `uint32_t` aligns to 4-byte boundary (offset must be multiple of 4)
- `uint16_t` aligns to 2-byte boundary
- `uint8_t` has no alignment requirement

**Example:**

```c
struct Example {
    uint8_t a;      /* offset 0, size 1 */
    /* padding 3 */  /* offset 1-3 (align next field to 4) */
    uint32_t b;     /* offset 4, size 4 */
    uint8_t c;      /* offset 8, size 1 */
    /* padding 3 */  /* offset 9-11 (struct size must be multiple of largest alignment) */
};  /* total size 12 (aligned to 4) */
```

### Struct Size Rounding

Total struct size is rounded up to the largest field alignment:

- Struct with `uint64_t` → size rounded to multiple of 8
- Struct with only `uint32_t` → size rounded to multiple of 4

This ensures array elements are properly aligned.

### Unions and Padding

Unions take the size of their largest member, rounded to alignment:

```c
union Example {
    varintFORMeta forMeta;    /* 48 bytes */
    varintPFORMeta pforMeta;  /* 40 bytes */
};  /* size: 48 bytes (largest member) */
```

### Bit Fields (Use Sparingly)

Can save space but have performance costs:

```c
struct BitFieldExample {
    unsigned int flag1 : 1;   /* 1 bit */
    unsigned int flag2 : 1;   /* 1 bit */
    unsigned int value : 14;  /* 14 bits */
    /* total: 16 bits = 2 bytes */
};
```

**Pros:**

- Space-efficient for flags
- Good for hardware register mapping

**Cons:**

- Slower access (bit masking/shifting)
- Non-portable across compilers
- Can't take address of bit field

**Recommendation:** Use only when space is critical (e.g., network protocols, file formats).

---

## References

### Tools

- **pahole**: https://www.mankier.com/1/pahole
- **dwarves**: https://github.com/acmel/dwarves

### Further Reading

- [Data Alignment: Straighten Up and Fly Right](https://developer.ibm.com/articles/pa-dalign/)
- [Cache-Friendly Code](https://www.intel.com/content/www/us/en/developer/articles/technical/cache-blocking-techniques.html)
- [Struct Packing in C](https://www.catb.org/esr/structure-packing/)

---

## Changelog

### 2024-11-19: Initial Optimization

- Optimized 6 metadata structs
- Saved 8 bytes total (2.1% reduction)
- varintPFORMeta: 48 → 40 bytes (100% efficient)
- varintDictStats: Already perfect (100% efficient)
- Added pahole-based analysis tools
- Created comprehensive documentation
