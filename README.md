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

./src/datakit-test test multimap
./src/datakit-test test multimapFull
./src/datakit-test test multilist
./src/datakit-test test multilistFull
./src/datakit-test test multiarray
./src/datakit-test test multiarraySmall
./src/datakit-test test multiarrayMedium
./src/datakit-test test multiarrayLarge
./src/datakit-test test xof
./src/datakit-test test dod
./src/datakit-test test intset
./src/datakit-test test intsetU32
./src/datakit-test test hll
./src/datakit-test test strDoubleFormat
./src/datakit-test test multiroar
./src/datakit-test test fibbuf
./src/datakit-test test jebuf
./src/datakit-test test mflex
./src/datakit-test test flex
./src/datakit-test test membound
./src/datakit-test test multimapatom
./src/datakit-test test multilru
./src/datakit-test test ptrprevnext
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
- [String utilities](docs/modules/string/) - str, dks (mds/mdsc), strDoubleFormat

**Scale-Aware Containers:**
- [multimap](docs/modules/multimap/) - Key-value store (small/medium/full/atom variants)
- [multilist](docs/modules/multilist/) - Linked lists with multi-element nodes
- [multiarray](docs/modules/multiarray/) - Dynamic arrays (small/medium/large variants)
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
- [System modules](docs/modules/system/) - OS integration, timers, mutexes

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

Some components may be abandoned experiments and I just forgot to remove them from this snapshot.
