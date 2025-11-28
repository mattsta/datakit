# Varint Library Development Tools

This directory contains tools for analyzing and optimizing the varint library's memory layout and performance characteristics.

## Struct Memory Optimization Tools

### `struct_pahole_analyzer.sh` - Primary Analysis Tool ⭐

**Purpose:** Comprehensive struct layout analysis using DWARF debug information

**Requirements:**

```bash
sudo apt-get install dwarves  # Provides pahole
```

**Usage:**

```bash
./struct_pahole_analyzer.sh
```

**Output:**

- Detailed struct layouts with byte offsets
- Padding analysis
- Cache line boundary detection
- Efficiency percentages
- Actionable optimization recommendations

**Example Output:**

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

  Cache Line Analysis (64-byte cache lines):
    ✓ Fits entirely in 1 cache line (40 bytes)
      → Excellent locality of reference

  Padding Analysis:
    ✓ No internal padding (100% efficient)
```

---

### `struct_audit.c` - Manual Field-by-Field Analysis

**Purpose:** Detailed field analysis using C compiler introspection

**Compile:**

```bash
gcc -g -I../src -o struct_audit struct_audit.c -lm
```

**Run:**

```bash
./struct_audit
```

**Features:**

- Uses `offsetof()` and `sizeof()` for exact field positions
- Visual table layout of all fields
- Padding calculation between fields
- Optimization suggestions based on field sizes

**When to use:**

- When `pahole` is not available
- For portable analysis (works on any platform)
- To understand compiler behavior

---

### `struct_size_check.c` - Regression Testing

**Purpose:** Quick verification that struct sizes haven't regressed

**Compile:**

```bash
gcc -I../src -o struct_size_check struct_size_check.c
```

**Run:**

```bash
./struct_size_check
```

**Output:**

```
varintFORMeta                 :  48 bytes (expected <=  48) ✓
varintPFORMeta                :  40 bytes (expected <=  48) ✓
varintFloatMeta               :  48 bytes (expected <=  48) ✓
...
Total bytes before:  376
Total bytes after:   368
Bytes saved:         8 (2.1% reduction)
```

**Use in CI/CD:**

```bash
# Add to your test suite
./struct_size_check
if [ $? -ne 0 ]; then
    echo "Struct size regression detected!"
    exit 1
fi
```

---

### `struct_analyzer.c` - Legacy Manual Analyzer

**Purpose:** Early version of manual field analysis

**Status:** Superseded by `struct_pahole_analyzer.sh` + `pahole`

**Note:** Kept for reference. Use `struct_pahole_analyzer.sh` for new analysis.

---

## Workflow

### Adding a New Struct

1. **Define struct** in appropriate header file following conventions:

   ```c
   /* Brief description
    * Fields ordered by size (8-byte → 4-byte → 1-byte) to eliminate padding */
   typedef struct MyNewStruct {
       /* 8-byte fields */
       uint64_t field1;
       size_t field2;

       /* 4-byte fields */
       uint32_t field3;

       /* 1-byte fields */
       bool flag1;
   } MyNewStruct;
   ```

2. **Add to analyzer script**:

   ```bash
   vim struct_pahole_analyzer.sh
   # Add "MyNewStruct" to STRUCTS array
   ```

3. **Run analysis**:

   ```bash
   ./struct_pahole_analyzer.sh | grep -A30 "MyNewStruct"
   ```

4. **Add size check**:

   ```bash
   vim struct_size_check.c
   # Add: CHECK_SIZE(MyNewStruct, expected_size, "MyNewStruct");
   ```

5. **Verify**:
   ```bash
   gcc -I../src -o struct_size_check struct_size_check.c
   ./struct_size_check
   ```

### Optimizing an Existing Struct

1. **Analyze current layout**:

   ```bash
   pahole -C StructName struct_audit_dwarf
   ```

2. **Identify issues**:
   - Look for `padding:` lines (wasted bytes)
   - Look for `cacheline...boundary` markers (cache line splits)

3. **Reorder fields**:
   - Group 8-byte fields first
   - Then 4-byte fields
   - Then 2-byte fields
   - Then 1-byte fields

4. **Document the change**:

   ```c
   /* Fields ordered by size (8-byte → 4-byte → 1-byte) to eliminate padding */
   ```

5. **Verify improvement**:

   ```bash
   ./struct_pahole_analyzer.sh | grep -A30 "StructName"
   ```

6. **Update size check** with new expected size

7. **Run tests**:
   ```bash
   cd ..
   ./build/test_relevant_module
   ```

---

## pahole Quick Reference

### Basic Commands

```bash
# Analyze specific struct
pahole -C StructName binary

# Show all structs
pahole binary

# List struct sizes
pahole --sizes binary | grep varint

# Show reorganization suggestions
pahole --reorganize binary

# Show reorganization steps
pahole --show_reorg_steps binary

# Expand nested structs
pahole -E -C StructName binary

# Show only structs with padding
pahole --show_padded binary
```

### Interpreting Output

```
struct Example {
	uint64_t  field1;  /*     0     8 */  ← offset 0, size 8 bytes
	uint32_t  field2;  /*     8     4 */  ← offset 8, size 4 bytes
	/* padding: 4 */    /* 12-15: WASTED */
	uint64_t  field3;  /*    16     8 */

	/* --- cacheline 1 boundary (64 bytes) --- */  ← Crosses cache line!
	uint8_t   field4;  /*    64     1 */

	/* size: 72, cachelines: 2, members: 4 */
	/* padding: 4 */
	/* last cacheline: 8 bytes */
};
```

**Key indicators:**

- `padding: N` → N bytes wasted
- `cacheline X boundary` → Struct crosses cache line
- `size: X` → Total struct size
- `cachelines: N` → Number of cache lines spanned

---

## Best Practices

### Field Ordering

1. **Size-based ordering** (primary rule):
   - 8-byte → 4-byte → 2-byte → 1-byte

2. **Hot/cold separation** (secondary rule):
   - Frequently accessed fields first
   - Rarely accessed fields last
   - Keeps hot fields in same cache line

3. **Logical grouping** (tertiary rule):
   - Group related fields together
   - Maintain readability

### Documentation

Always add comments explaining the ordering:

```c
/* Metadata structure for XYZ encoding
 *
 * Memory layout:
 *   Fields ordered by size (8-byte → 4-byte → 1-byte) for zero padding
 *   Hot fields (used during encoding) in first 32 bytes for cache locality
 *   Total size: 40 bytes (fits in 1 cache line)
 */
typedef struct StructName {
    // ...
} StructName;
```

### Static Assertions

Add size assertions to prevent regressions:

```c
/* Ensure struct size hasn't regressed */
_Static_assert(sizeof(StructName) <= 48,
               "StructName exceeded 48 bytes - check field ordering");
```

---

## Troubleshooting

### pahole shows "NOT FOUND"

**Cause:** Struct not compiled with debug symbols, or optimized out

**Solution:**

```bash
gcc -g3 -O0 -I../src -o struct_audit_dwarf struct_audit.c -lm
```

### pahole command not found

**Solution:**

```bash
sudo apt-get install dwarves
```

### Struct size different from expected

**Cause:** Compiler padding rules, alignment requirements

**Debug:**

```bash
pahole -C StructName binary
# Look for padding and alignment
```

---

## See Also

- [Struct Optimization Guide](../docs/struct-optimization-guide.md) - Comprehensive documentation
- [pahole man page](https://www.mankier.com/1/pahole) - Full pahole documentation
- [Data Structure Alignment](https://en.wikipedia.org/wiki/Data_structure_alignment) - Wikipedia

---

## Changelog

### 2024-11-19

- Added `struct_pahole_analyzer.sh` - Primary analysis tool using DWARF
- Added `struct_size_check.c` - Regression testing tool
- Optimized 6 metadata structs (8 bytes saved)
- Created comprehensive documentation
