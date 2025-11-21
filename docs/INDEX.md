# datakit Documentation Index

**datakit** is a high-performance C library providing the most efficient data structures possible, with a focus on optimal memory usage, compression, and performance.

## Quick Links

- [Getting Started Guide](GETTING_STARTED.md) - Build instructions and first steps
- [Architecture Overview](ARCHITECTURE.md) - Design patterns and philosophy
- [API Quick Reference](API_QUICK_REFERENCE.md) - Common operations across modules

## Module Documentation

### Core Infrastructure

- [datakit Core](modules/core/DATAKIT.md) - Main configuration and platform abstraction
- [databox](modules/core/DATABOX.md) - Universal 16-byte container for any data type
- [Configuration System](modules/core/CONFIG.md) - Compile-time feature detection
- [Utilities](modules/core/UTIL.md) - General utility functions

### String and Buffer Management

- [str](modules/string/STR.md) - String utilities and character classification
- [dks String Buffers](modules/string/DKS.md) - Template-based string buffers (mds/mdsc)
- [strDoubleFormat](modules/string/STR_DOUBLE_FORMAT.md) - Fast double-to-string conversion
- [String Utilities](modules/string/STR_UTILS.md) - Extended string operations

### Flexible Arrays

- [flex](modules/flex/FLEX.md) - Core compressed byte array
- [mflex](modules/flex/MFLEX.md) - Compressed/uncompressed flex wrapper
- [Capacity Management](modules/flex/CAPACITY.md) - Allocation strategies

### Multi-Container Modules

These containers use **scale-aware** implementations with small/medium/large variants optimized for different sizes.

#### MultiMap - Key-Value Store with Multi-Element Values

- [MultiMap Overview](modules/multimap/MULTIMAP.md) - API and usage guide
- [Implementation Variants](modules/multimap/VARIANTS.md) - Small, Medium, Full, and Atom versions
- [MultiMap Examples](modules/multimap/EXAMPLES.md) - Real-world usage patterns

#### MultiList - Linked List with Multi-Element Nodes

- [MultiList Overview](modules/multilist/MULTILIST.md) - API and usage guide
- [Implementation Variants](modules/multilist/VARIANTS.md) - Small, Medium, and Full versions
- [MultiList Examples](modules/multilist/EXAMPLES.md) - Real-world usage patterns

#### MultiArray - Dynamic Arrays with Multi-Element Support

- [MultiArray Overview](modules/multiarray/MULTIARRAY.md) - API and usage guide
- [Implementation Variants](modules/multiarray/VARIANTS.md) - Small, Medium, and Large versions
- [MultiArray Examples](modules/multiarray/EXAMPLES.md) - Real-world usage patterns

#### Other Multi-Containers

- [multidict](modules/multi/MULTIDICT.md) - General-purpose dictionary
- [multilru](modules/multi/MULTILRU.md) - Least Recently Used cache
- [multiroar](modules/multi/MULTIROAR.md) - Roaring bitmap implementation

### Integer Sets and Cardinality

- [intset](modules/intset/INTSET.md) - Variable-width integer set (16/32/64-bit)
- [intsetU32](modules/intset/INTSET_U32.md) - Fixed 32-bit unsigned integer set
- [intsetBig](modules/intset/INTSET_BIG.md) - 64-bit integer sets (x86_64 only)
- [hyperloglog](modules/intset/HYPERLOGLOG.md) - Probabilistic cardinality estimation

### Compression and Encoding

- [xof](modules/compression/XOF.md) - XOR filter for double compression (delta-of-delta)
- [dod](modules/compression/DOD.md) - Delta-of-delta encoding for int64 values
- [float16](modules/compression/FLOAT16.md) - Half-precision floating point (F16C)

### Memory Management

- [membound](modules/memory/MEMBOUND.md) - Thread-safe memory allocation limits
- [fibbuf](modules/memory/FIBBUF.md) - Fibonacci buffer sizing
- [jebuf](modules/memory/JEBUF.md) - Jemalloc-aligned buffer management
- [ptrPrevNext](modules/memory/PTR_PREV_NEXT.md) - Efficient pointer linking
- [offsetArray](modules/memory/OFFSET_ARRAY.md) - Offset-based array addressing

### System and OS Integration

- [OSRegulate](modules/system/OS_REGULATE.md) - OS resource limiting
- [setproctitle](modules/system/SETPROCTITLE.md) - Process title manipulation
- [versionOSRuntime](modules/system/VERSION_OS_RUNTIME.md) - OS/runtime detection
- [fastmutex](modules/system/FASTMUTEX.md) - High-performance mutex
- [timeUtil](modules/system/TIME_UTIL.md) - Time utilities
- [multiTimer](modules/system/MULTI_TIMER.md) - Multi-timer management

### Data Structure Utilities

- [list](modules/utils/LIST.md) - Doubly-linked list
- [multiheap](modules/utils/MULTIHEAP.md) - Heap data structure
- [intersectInt](modules/utils/INTERSECT_INT.md) - Set intersection operations

### Dependencies

- [lz4](modules/deps/LZ4.md) - LZ4 compression
- [xxHash](modules/deps/XXHASH.md) - Fast hashing
- [sha1](modules/deps/SHA1.md) - SHA1 cryptographic hash
- [imath](modules/deps/IMATH.md) - Arbitrary precision integer math
- [varint](modules/deps/VARINT.md) - Variable-length integer encoding

## Advanced Topics

- [Performance Tuning](advanced/PERFORMANCE.md) - Optimization guidelines
- [Memory Efficiency](advanced/MEMORY.md) - Understanding memory patterns
- [Scale-Aware Collections](advanced/SCALE_AWARE.md) - Choosing the right size variant
- [Thread Safety](advanced/THREAD_SAFETY.md) - Concurrent usage patterns
- [Platform Support](advanced/PLATFORMS.md) - Supported platforms and features

## Testing and Development

- [Running Tests](development/TESTING.md) - Test suite guide
- [Building from Source](development/BUILDING.md) - Detailed build instructions
- [Contributing Guidelines](development/CONTRIBUTING.md) - How to contribute
- [Debugging Tips](development/DEBUGGING.md) - Common issues and solutions

## Examples and Recipes

- [Common Patterns](examples/PATTERNS.md) - Frequently used code patterns
- [Migration Guide](examples/MIGRATION.md) - Upgrading from other libraries
- [Use Cases](examples/USE_CASES.md) - Real-world application examples
- [Performance Comparisons](examples/BENCHMARKS.md) - Benchmark results

## About

- **Version**: 0.3.0
- **License**: Zero Usage Unless Compensated (2016-2024)
- **Author**: Matt Stancliff
- **Status**: Development snapshot - some components complete, others in refinement

## Quick Stats

- **86+ header files** with comprehensive APIs
- **30+ testable modules** with extensive test coverage
- **10+ module categories** covering all common data structure needs
- **Cross-platform** support for Linux, macOS, BSD, Solaris, AIX, Windows
- **Performance-focused** with aggressive optimizations and minimal overhead
