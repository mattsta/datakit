# datakit: the most efficient data structures possible

Also see:

- https://matt.sh/datakit
- https://matt.sh/best-database-ever
- https://matt.sh/trillion-dollar-data-structure

## Usage

```haskell
mkdir build
cd build
cmake ..
make -j8

ctest -j8 --output-on-failure --stop-on-failure
```

## Documentation

**Comprehensive documentation is now available in the `docs/` directory!**

### Quick Start

- **[Getting Started Guide](docs/GETTING_STARTED.md)** - Build instructions and first examples
- **[Architecture Overview](docs/ARCHITECTURE.md)** - Design patterns and philosophy
- **[API Quick Reference](docs/API_QUICK_REFERENCE.md)** - Common operations at a glance
- **[Documentation Index](docs/INDEX.md)** - Complete module listing

### Module Documentation

**Core & Utilities:**

- [databox](docs/modules/core/DATABOX.md) - Universal 16-byte container
- [flex](docs/modules/flex/FLEX.md) - Compressed variable-length arrays
- [mflex](docs/modules/flex/MFLEX.md) - Transparent LZ4 compression wrapper
- **String utilities:**
  - [str](docs/modules/string/STR.md) - Core string operations
  - [dks](docs/modules/string/DKS.md) - Dynamic key strings (mds/mdsc)
  - [strDoubleFormat](docs/modules/string/STR_DOUBLE_FORMAT.md) - Efficient double formatting
  - [String utils](docs/modules/string/STR_UTILS.md) - Additional utilities

**Scale-Aware Containers:**

- **multimap** - Key-value store (small/medium/full/atom variants)
  - [Overview](docs/modules/multimap/MULTIMAP.md) - Core functionality
  - [Variants](docs/modules/multimap/VARIANTS.md) - Choosing the right size
  - [Examples](docs/modules/multimap/EXAMPLES.md) - Usage patterns
- **multilist** - Linked lists with multi-element nodes
  - [Overview](docs/modules/multilist/MULTILIST.md) - Core functionality
  - [Variants](docs/modules/multilist/VARIANTS.md) - Choosing the right size
  - [Examples](docs/modules/multilist/EXAMPLES.md) - Usage patterns
- **multiarray** - Dynamic arrays (small/medium/large variants)
  - [Overview](docs/modules/multiarray/MULTIARRAY.md) - Core functionality
  - [Variants](docs/modules/multiarray/VARIANTS.md) - Choosing the right size
  - [Examples](docs/modules/multiarray/EXAMPLES.md) - Usage patterns
- [multidict](docs/modules/multi/MULTIDICT.md) - Generic hash table
- [multilru](docs/modules/multi/MULTILRU.md) - Multi-level LRU cache
- [multiroar](docs/modules/multi/MULTIROAR.md) - Roaring bitmaps

**Integer Sets & Cardinality:**

- [intset](docs/modules/intset/INTSET.md) - Variable-width integer sets (16/32/64-bit)
- [intsetU32](docs/modules/intset/INTSET_U32.md) - Fixed 32-bit integer sets
- [hyperloglog](docs/modules/intset/HYPERLOGLOG.md) - Probabilistic cardinality estimation

**Compression & Encoding:**

- [xof](docs/modules/compression/XOF.md) - XOR filter for double compression
- [dod](docs/modules/compression/DOD.md) - Delta-of-delta integer encoding
- [float16](docs/modules/compression/FLOAT16.md) - Half-precision floats

**Memory & System:**

- [membound](docs/modules/memory/MEMBOUND.md) - Memory allocation limits
- [fibbuf](docs/modules/memory/FIBBUF.md) - Fibonacci buffer sizing
- [jebuf](docs/modules/memory/JEBUF.md) - Jemalloc-aligned allocation
- **System modules:**
  - [fastmutex](docs/modules/system/FASTMUTEX.md) - Fast mutex primitives
  - [multiTimer](docs/modules/system/MULTI_TIMER.md) - High-performance timers
  - [osRegulate](docs/modules/system/OS_REGULATE.md) - Resource regulation
  - [setproctitle](docs/modules/system/SETPROCTITLE.md) - Process title setting
  - [timeUtil](docs/modules/system/TIME_UTIL.md) - Time utilities
  - [versionOSRuntime](docs/modules/system/VERSION_OS_RUNTIME.md) - OS version detection

### Advanced Topics

- [Scale-Aware Design](docs/advanced/SCALE_AWARE.md) - Choosing the right size variant
- [Performance Guide](docs/advanced/PERFORMANCE.md) - Optimization techniques
- [Memory Patterns](docs/advanced/MEMORY.md) - Understanding memory efficiency
- [Thread Safety](docs/advanced/THREAD_SAFETY.md) - Concurrent usage patterns
- [Platform Support](docs/advanced/PLATFORMS.md) - Cross-platform considerations

### Examples & Development

- [Common Patterns](docs/examples/PATTERNS.md) - Frequently used code patterns
- [Use Cases](docs/examples/USE_CASES.md) - Real-world applications
- [Benchmarks](docs/examples/BENCHMARKS.md) - Performance comparisons
- [Migration Guide](docs/examples/MIGRATION.md) - From Redis, STL, glib, etc.
- [Testing Guide](docs/development/TESTING.md) - Running and writing tests
- [Building Guide](docs/development/BUILDING.md) - Detailed build instructions
- [Debugging Guide](docs/development/DEBUGGING.md) - Troubleshooting techniques

## Status

This is a recently updated development snapshot.

Many components are complete and now fully documented.

Some components are a first or second draft and need another feature refactoring cycle or two.
