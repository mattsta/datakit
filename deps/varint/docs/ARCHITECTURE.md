# Varint Library Architecture

## Overview

The varint library provides a comprehensive suite of variable-length integer encoding systems optimized for different use cases. Instead of being limited to fixed-width integers (8, 16, 32, 64 bits), varints allow storing integers using any byte width (8, 16, 24, 32, 40, 48, 56, 64 bits), significantly reducing memory usage when most values are small but occasional large values are needed.

## Core Design Philosophy

**Problem**: Fixed-width integers waste space. If you need to store millions of small numbers (typically under 1000) but occasionally need values up to 2^64, using 8 bytes for every number wastes 7 bytes per entry for the common case.

**Solution**: Variable-length encoding where each integer uses only the bytes needed to represent its value, with metadata indicating the width.

**Key Trade-off**: Storage metadata overhead vs. space savings. Different varint types handle this trade-off differently.

## Architecture Layers

The library is organized into three architectural layers:

### Layer 1: Core Varint Encodings (4 Types)

The foundation layer provides four primary varint encoding strategies, each with different metadata storage approaches:

1. **[varintTagged](modules/varintTagged.md)** - Metadata in first byte, sortable, big-endian
2. **[varintExternal](modules/varintExternal.md)** - Metadata external to encoding, zero overhead
3. **[varintSplit](modules/varintSplit.md)** - Three-level encoding with known bit boundaries
4. **[varintChained](modules/varintChained.md)** - Continuation bits in each byte (legacy)

### Layer 2: Variant Encodings (5 Specialized Types)

Enhanced versions of core encodings optimized for specific scenarios:

1. **varintSplitFull** - Extended split using all first-byte values
2. **varintSplitFullNoZero** - Split variant without zero optimization
3. **varintSplitFull16** - Split optimized for 16-bit minimum values
4. **varintSplitReversed** - Reverse-traversable split varints
5. **varintExternalBigEndian** - Big-endian external encoding

### Layer 3: Advanced Features (6 Systems)

High-level abstractions built on varint primitives:

1. **[varintPacked](modules/varintPacked.md)** - Fixed-width bit-packed arrays with arbitrary bit widths
2. **[varintDimension](modules/varintDimension.md)** - Bit-packed matrix storage with variable dimensions
3. **[varintBitstream](modules/varintBitstream.md)** - Bit-level read/write operations
4. **[varintRLE](modules/varintRLE.md)** - Run-length encoding for consecutive repeated values
5. **[varintElias](modules/varintElias.md)** - Elias Gamma/Delta universal codes for small integers
6. **[varintBP128](modules/varintBP128.md)** - SIMD-accelerated block-packed encoding for large arrays

## Quick Comparison Matrix

| Type         | Metadata Location | Encoding      | Max Bytes | 1-Byte Max   | Sortable | Speed   | Best For                            |
| ------------ | ----------------- | ------------- | --------- | ------------ | -------- | ------- | ----------------------------------- |
| **Tagged**   | First byte        | Big-endian    | 9         | 240          | Yes      | Fast    | Database keys, sorted data          |
| **External** | External          | Little-endian | 8         | 255          | No       | Fastest | Compact storage, metadata elsewhere |
| **Split**    | First byte        | Hybrid        | 9         | 63           | No       | Fast    | Known bit boundaries, packing       |
| **Chained**  | Continuation bits | Variable      | 9         | 127          | No       | Slowest | Legacy compatibility                |
| **Packed**   | N/A               | Bit-level     | N/A       | Configurable | Yes      | Fast    | Fixed-width integer arrays          |

## Performance Characteristics

### Space Efficiency

For storing 1 million integers with typical distributions:

| Value Range   | Tagged  | External | Split  | Chained |
| ------------- | ------- | -------- | ------ | ------- |
| 0-240         | 1 MB    | 1 MB     | 1 MB   | 1 MB    |
| 0-65535       | ~2-3 MB | 2 MB     | 2-3 MB | 2-3 MB  |
| 0-16777215    | ~3-4 MB | 3 MB     | 3-4 MB | 3-4 MB  |
| Random 64-bit | ~9 MB   | 8 MB     | 9 MB   | 9 MB    |

External is always most space-efficient (no metadata overhead), but requires external width tracking.

### Time Complexity

| Operation  | Tagged | External | Split | Chained |
| ---------- | ------ | -------- | ----- | ------- |
| Encode     | O(1)   | O(1)     | O(1)  | O(w)    |
| Decode     | O(1)   | O(1)     | O(1)  | O(w)    |
| Get Length | O(1)   | External | O(1)  | O(w)    |

_w = width in bytes (typically 1-9)_

Chained requires byte-by-byte traversal until finding the continuation bit, making it O(w) instead of O(1).

## Code Organization

### Header-Only vs. Compiled

**Header-Only Templates** (Macro-based):

- `varintPacked.h` - Define `PACK_STORAGE_BITS` to generate custom bit-width functions
- `varintSplit.h` - Macro-based split encoding
- `varintSplitFull*.h` - Split variant implementations
- `varintBitstream.h` - Bit-level operations

**Compiled Modules** (.c + .h):

- `varintTagged.c/.h` - Performance-critical tagged encoding
- `varintExternal.c/.h` - External encoding with endian variants
- `varintChained.c/.h` - Legacy chained encoding
- `varintDimension.c/.h` - Matrix packing operations

### Optimization Strategies

1. **Fast Path Macros**: Quick inline operations for common small values (1-3 bytes)
2. **Function Fallbacks**: Full implementations for larger values
3. **Architecture-Specific**: `-march=native` compilation for CPU optimizations
4. **Unrolled Loops**: Manual loop unrolling in critical paths
5. **Compile-Time Specialization**: Template-like macro generation for specific bit widths

## Build System

Built with CMake, producing:

```
build/src/
├── libvarint.so          # Shared library
├── varint.a              # Static library
├── varintCompare         # Performance benchmark (20+ encoding tests)
├── varintDimensionTest   # Matrix operations test
└── varintPackedTest      # Bit-packing test
```

**Compilation Flags**: `-Wall -Wextra -pedantic -std=c99 -march=native -O3`

## Real-World Use Cases

### Database Storage

Use **varintTagged** for sortable keys that need to support both small and large IDs:

- User IDs (typically small, but can grow)
- Timestamps (growing monotonically)
- Auto-increment keys

### Network Protocols

Use **varintChained** for legacy compatibility with:

- Protocol Buffers
- SQLite3 database format
- LevelDB key-value store

### In-Memory Compression

Use **varintExternal** when you maintain metadata separately:

- Column stores with type metadata
- Arrays with known element widths
- Cache structures with external size tracking

### Bit-Packed Arrays

Use **varintPacked** for massive arrays of bounded integers:

- 8000 integers at 14 bits each (saves 25% over 16-bit storage)
- IP address components (0-255 needs 8 bits, not 16 or 32)
- Game world coordinates with known bounds

### Sparse Matrices

Use **varintDimension** for:

- Machine learning feature matrices
- Graph adjacency matrices with small node IDs
- Scientific computing with sparse data

### Pattern Matching & Routing

Use **varintBitstream** + **varintExternal** for:

- AMQP-style message broker routing with wildcard patterns
- Trie data structures with compact node encoding
- API gateway path matching with hierarchical patterns
- Event routing systems with prefix sharing

**Example**: AMQP-style pattern matching trie (see `examples/advanced/trie_pattern_matcher.c`)

- varintBitstream: 3-bit node flags (terminal, wildcard type)
- varintExternal: Variable-width subscriber IDs and segment lengths
- Achieves 0.7 bytes/pattern at 1M scale through prefix sharing
- 2391x faster than naive linear matching at 100K patterns
- O(m) constant-time matching regardless of pattern count

### Run-Length Compression

Use **varintRLE** for:

- Audio/video data with silence regions (8-100x compression)
- Sparse data with repeated zero values
- Game state with many identical tiles/cells
- Log aggregation with repeated status codes

### Succinct Data Structures

Use **varintElias** for:

- Huffman codebook storage (code lengths 1-15)
- Tree depth encoding (B-trees, tries)
- Small gap encoding in sorted lists
- Rank/select data structure auxiliary arrays

### Search Engine Indexes

Use **varintBP128** for:

- Posting lists with sorted document IDs
- Time series with monotonic timestamps
- Graph adjacency lists (sorted neighbors)
- Column store databases with clustered values

**Example**: 1M document IDs with delta BP128 achieves 8x compression with 1.2+ GB/s decode throughput.

## Module Dependencies

```
varint.h (common types)
    ├── endianIsLittle.h (endian detection)
    │
    ├── varintTagged.c/.h
    ├── varintExternal.c/.h
    │   └── varintExternalBigEndian.c/.h
    ├── varintChained.c/.h
    │   └── varintChainedSimple.c/.h
    ├── varintSplit.h
    │   ├── varintSplitFull.h
    │   ├── varintSplitFullNoZero.h
    │   └── varintSplitFull16.h
    │
    ├── varintBitstream.h
    ├── varintPacked.h (template)
    ├── varintDimension.h/.c
    │   └── varintPacked.h (uses internally)
    │
    ├── varintRLE.c/.h
    │   └── varintTagged.h (uses for length/value encoding)
    ├── varintElias.c/.h (bit-level Gamma/Delta codes)
    └── varintBP128.c/.h
        └── varintTagged.h (uses for delta first value)
```

## API Patterns

### Consistent Naming

All modules follow consistent naming:

- `varint{Type}Put*` - Encode value
- `varint{Type}Get*` - Decode value
- `varint{Type}Len*` - Get/calculate length
- `varint{Type}*Quick*` - Fast path macros for small values

### Type Safety

- `varintWidth` enum for width metadata (1-16 bytes)
- `uint64_t` for value parameters
- `uint8_t*` for byte array buffers
- Return values indicate encoding width

### Example Workflow

```c
#include "varintTagged.h"

// Encoding
uint64_t value = 1234567;
uint8_t buffer[9];  // Max 9 bytes for tagged
varintWidth width = varintTaggedPut64(buffer, value);
// width = 4 (value needs 4 bytes)

// Decoding
uint64_t decoded;
varintTaggedGet64(buffer, &decoded);
assert(decoded == value);

// Get length without decoding
varintWidth len = varintTaggedGetLen(buffer);
assert(len == width);
```

## Testing & Benchmarking

### varintCompare.c

Comprehensive performance benchmark testing 20+ varint implementations with 134 million random numbers:

- Baseline overhead measurement
- Cycles per operation
- Operations per second
- Small number optimizations
- Reversed encoding tests

### varintPackedTest.c

Tests bit-packed arrays at multiple widths:

- 12-bit, 13-bit, 14-bit packing
- Sorted insertion
- Binary search membership
- Deletion operations

### varintDimensionTest.c

Matrix dimension storage and retrieval validation.

## Error Handling

The library uses assertions and compile-time checks:

- `assert()` for value range validation
- Compile-time `#error` for invalid configurations
- No runtime exceptions (C99 compliance)

## Thread Safety

All functions are thread-safe for read-only operations. Concurrent writes to the same buffer require external synchronization.

## Next Steps

- [Choosing the Right Varint Type](../CHOOSING_VARINTS.md)
- Module-specific guides:
  - [varintTagged Guide](modules/varintTagged.md)
  - [varintExternal Guide](modules/varintExternal.md)
  - [varintSplit Guide](modules/varintSplit.md)
  - [varintChained Guide](modules/varintChained.md)
  - [varintPacked Guide](modules/varintPacked.md)
  - [varintDimension Guide](modules/varintDimension.md)
  - [varintBitstream Guide](modules/varintBitstream.md)

## Contributing

When adding new varint types:

1. Follow existing naming conventions
2. Provide both macro and function implementations where appropriate
3. Add comprehensive tests to `varintCompare.c`
4. Document encoding format with bit diagrams
5. Update this architecture guide

## References

- SQLite4 (source of varintTagged): https://sqlite.org/src4/doc/trunk/www/varint.wiki
- SQLite3 (source of varintChained): https://sqlite.org/src/file/src/util.c
- LevelDB (source of varintChainedSimple): https://github.com/google/leveldb
