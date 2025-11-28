# Varint Library Quick Reference Card

## 1-Minute Quickstart

```c
#include "varintTagged.h"

uint8_t buf[9];
uint64_t original = 12345;

// Encode
varintWidth len = varintTaggedPut64(buf, original);

// Decode
uint64_t decoded;
varintTaggedGet64(buf, &decoded);
// decoded == 12345
```

---

## Choosing the Right Encoding (Decision Tree)

```
Need sortable keys? ──────────> varintTagged (big-endian, sortable)
        │ No
        v
Have external width? ─────────> varintExternal (zero overhead, fastest)
        │ No
        v
Known bit boundaries? ────────> varintSplit (3-level hybrid)
        │ No
        v
Legacy compatibility? ────────> varintChained (continuation bits)
        │ No
        v
Fixed bit-width arrays? ──────> varintPacked (any bit width 1-64)
```

---

## Core Encodings Comparison

| Type         | Best For           | 1-Byte Max | Overhead | Speed   | Sortable |
| ------------ | ------------------ | ---------- | -------- | ------- | -------- |
| **Tagged**   | DB keys, indexes   | 240        | Low      | Fast    | ✓        |
| **External** | Compact storage    | 255        | None     | Fastest | ✗        |
| **Split**    | Known boundaries   | 63         | Low      | Fast    | ✗        |
| **Chained**  | Legacy protocols   | 127        | High     | Slow    | ✗        |
| **Packed**   | Fixed-width arrays | Custom     | None     | Fast    | ✓        |

---

## Common Patterns

### Pattern 1: Tagged (Sortable Keys)

```c
// Database primary keys, sorted indexes
uint8_t key[9];
varintWidth keyLen = varintTaggedPut64(key, userID);
// Keys sort lexicographically by value
```

### Pattern 2: External (Maximum Compression)

```c
// When you store width separately (e.g., array metadata)
varintWidth width = varintExternalPut(buf, value);
// Later: uint64_t v = varintExternalGet(buf, width);
```

### Pattern 3: Packed Arrays (Fixed-Width)

```c
// 1000 integers, 12 bits each
#define PACK_STORAGE_BITS 12
#include "varintPacked.h"

uint8_t array[1500];  // (1000 * 12) / 8 = 1500 bytes
varintPacked12Set(array, index, value);
uint16_t v = varintPacked12Get(array, index);
```

### Pattern 4: Adaptive (Auto-Selection)

```c
// Let the library choose best encoding
#include "varintAdaptive.h"

varintAdaptive adaptive = {0};
varintAdaptiveChooseBest(&adaptive, data, count);
// Analyzes distribution and picks optimal encoding
```

---

## API Patterns

### Encoding Pattern

```c
varintWidth varint<Type>Put(uint8_t *dst, uint64_t value);
// Returns: bytes written (1-9), or 0 on error
```

### Decoding Pattern

```c
varintWidth varint<Type>Get(const uint8_t *src, uint64_t *result);
// Returns: bytes read (1-9), or 0 on error
```

### Length Calculation

```c
varintWidth varint<Type>Length(uint64_t value);
// Returns: bytes needed to encode value (1-9)
```

---

## Space Usage Examples

| Value Range | Tagged  | External | Split     | Native uint64_t |
| ----------- | ------- | -------- | --------- | --------------- |
| 0-240       | 1 byte  | 1 byte   | 1 byte    | 8 bytes         |
| 1,000       | 2 bytes | 2 bytes  | 2 bytes   | 8 bytes         |
| 65,535      | 3 bytes | 2 bytes  | 2 bytes   | 8 bytes         |
| 1,000,000   | 4 bytes | 3 bytes  | 3-4 bytes | 8 bytes         |
| 2^32-1      | 5 bytes | 4 bytes  | 5 bytes   | 8 bytes         |
| 2^48-1      | 7 bytes | 6 bytes  | 7 bytes   | 8 bytes         |
| 2^63-1      | 9 bytes | 8 bytes  | 9 bytes   | 8 bytes         |

**Savings**: Typically 50-75% for datasets with small-to-medium values

---

## Common Use Cases

### Time Series Database

```c
// Timestamps typically delta-encoded
#include "varintDelta.h"

uint64_t timestamps[] = {1700000000, 1700000015, 1700000030, ...};
uint8_t compressed[...];

varintDeltaEncode(compressed, timestamps, count);
// Deltas: 0, 15, 15, ... (1-2 bytes each instead of 8)
```

### Network Protocol

```c
// Message length + data
uint8_t buf[MAX_SIZE];
varintWidth lenBytes = varintExternalPut(buf, dataLength);
memcpy(buf + lenBytes, data, dataLength);
// Saves bytes on typical small messages
```

### Sparse Matrix (CSR Format)

```c
// Column indices (typically < 10,000)
#include "varintPacked.h"

#define PACK_STORAGE_BITS 14  // 0-16,383 range
#include "varintPacked.h"

varintPacked14Set(colIndices, i, colIdx);
```

### Key-Value Store

```c
// Sortable keys for B-tree
varintWidth keyLen = varintTaggedPut64(key, userId);
// Keys sort in value order automatically
```

---

## Performance Tips

### ✅ DO

- Use **External** for maximum compression (when width is stored elsewhere)
- Use **Tagged** for database keys (sortable)
- Use **Packed** for large arrays of similar-sized integers
- Batch encode/decode when possible
- Use stack buffers (varint max is 9 bytes)

### ❌ DON'T

- Don't use varints for random-access hot paths (unless packed)
- Don't use Chained (slowest, legacy only)
- Don't allocate dynamically (zero-copy design)
- Don't ignore return values (0 = error)

---

## Error Handling

```c
// Always check return value
varintWidth len = varintTaggedPut64(buf, value);
if (len == 0) {
    // Error: value too large or buffer issue
    return ERROR;
}

// Bounds checking (when available)
varintWidth len = varintTaggedGet(buf, bufLen, &value);
if (len == 0) {
    // Error: insufficient buffer or invalid encoding
    return ERROR;
}
```

---

## Thread Safety

**Thread-Safe** (pure functions, no state):

- ✅ varintTagged, varintExternal, varintSplit, varintChained
- ✅ varintPacked (if buffers not shared)

**Not Thread-Safe** (mutable state):

- ⚠️ varintDict, varintBitmap, varintAdaptive
- Requires external synchronization (mutex/RWlock)

---

## Build Integration

### CMake

```cmake
add_subdirectory(path/to/varint)
target_link_libraries(myapp varint)
```

### Header-Only (Packed Arrays)

```c
#define PACK_STORAGE_BITS 12
#include "varintPacked.h"
// No linking required, macros generate functions
```

### Manual Compilation

```bash
gcc -I./varint/src myapp.c varint/src/varintTagged.c -o myapp
```

---

## Module Map

```
src/
├── varintTagged.{c,h}      # Sortable, big-endian (1-9 bytes)
├── varintExternal.{c,h}    # Zero overhead, fastest (1-8 bytes)
├── varintSplit.h           # 3-level hybrid (1-9 bytes)
├── varintChained.{c,h}     # Legacy, continuation bits (1-10 bytes)
├── varintPacked.h          # Fixed-width arrays (any bits 1-64)
├── varintDelta.{c,h}       # Delta encoding for sequences
├── varintFOR.{c,h}         # Frame of Reference encoding
├── varintPFOR.{c,h}        # Patched Frame of Reference
├── varintDict.{c,h}        # Dictionary encoding
├── varintBitmap.{c,h}      # Roaring bitmap implementation
├── varintAdaptive.{c,h}    # Auto-select best encoding
├── varintFloat.{c,h}       # Float compression
└── varintDimension.{c,h}   # Matrix/tensor packing
```

---

## Debugging Checklist

```c
// Enable bounds checking
#define VARINT_DEBUG 1

// Check encoding validity
assert(varint<Type>Put(buf, value) > 0);

// Verify round-trip
uint64_t original = 12345, decoded;
varintWidth len = varint<Type>Put(buf, original);
varint<Type>Get(buf, &decoded);
assert(original == decoded);

// Test with sanitizers
gcc -fsanitize=address,undefined myapp.c ...
```

---

## Documentation Links

- **Full API**: See `docs/modules/varint*.md`
- **Architecture**: `docs/ARCHITECTURE.md`
- **Choosing Guide**: `docs/CHOOSING_VARINTS.md`
- **Correctness**: `docs/CORRECTNESS_AUDIT.md`
- **System Audit**: `docs/COMPREHENSIVE_SYSTEM_AUDIT.md`

---

## Quick Benchmark (Intel x86_64)

| Operation       | Cycles | ns @ 3GHz | Throughput |
| --------------- | ------ | --------- | ---------- |
| Tagged Encode   | ~10    | ~3.3      | 300 M/s    |
| Tagged Decode   | ~8     | ~2.7      | 370 M/s    |
| External Encode | ~8     | ~2.7      | 370 M/s    |
| External Decode | ~6     | ~2.0      | 500 M/s    |
| Packed Get      | ~3     | ~1.0      | 1 B/s      |
| Packed Set      | ~4     | ~1.3      | 750 M/s    |

**Note**: Actual performance depends on value distribution and cache effects.

---

## Common Pitfalls

### ❌ Pitfall 1: Not checking buffer size

```c
uint8_t buf[2];  // Too small!
varintTaggedPut64(buf, 1000000);  // Buffer overflow!
```

✅ **Fix**: Always allocate at least 9 bytes for varints

### ❌ Pitfall 2: Mixing encoding types

```c
varintTaggedPut64(buf, value);
varintExternalGet(buf, width);  // Wrong decoder!
```

✅ **Fix**: Match encoder/decoder types

### ❌ Pitfall 3: Assuming width

```c
memcpy(dst, src, 8);  // Assumes 8-byte varint
```

✅ **Fix**: Use returned width or varint<Type>Length()

---

**Last Updated**: 2025-11-19
**Version**: 1.0.0
**License**: Public Domain / MIT
