# datakit: the most efficient data structures possible

Also see:

- https://matt.sh/datakit
- https://matt.sh/best-database-ever
- https://matt.sh/trillion-dollar-data-structure

## Building

datakit is POSIX-only: Linux, macOS, and the BSD family, built with
clang or gcc. CMake 3.10+ is required for a plain build; 3.21+ for the
presets below.

The presets are the supported entry points. Each builds `datakit-test`
plus the static, shared, and module library targets:

```sh
cmake --preset dev    && cmake --build --preset dev     # Release-O3 + ASAN/UBSAN (default)
cmake --preset debug  && cmake --build --preset debug   # -O0 + symbols + ASAN/UBSAN
cmake --preset perf   && cmake --build --preset perf    # Release-O3 + native + LTO, no sanitizers
cmake --preset safe   && cmake --build --preset safe    # Release-O3 + sanitizers, for CI
```

Each preset builds into `build/<preset>/`. Use `perf` for any
benchmark number. Sanitizers slow code down unevenly, by 1.0-7.7x across
perfRegression's rows (`dev` vs `perf` on an Apple M1 Max, clang 22).
Paths bound by memory bandwidth or hardware instructions barely change;
scalar code doing many loads and stores per byte (BLAKE3, membound) slows
the most. That invalidates measurements and can reorder rankings. To
reproduce, run `datakit-test test perfRegression` from both builds and
compare the outputs with `dktools perfdiff` (see
[docs/development/TESTING.md](docs/development/TESTING.md)).

A plain out-of-source build also works:

```sh
mkdir build && cd build
cmake ..
make -j8
```

## Testing

Every registered test is also a CTest target, so both of these work:

```sh
ctest --test-dir build/dev -j8 --output-on-failure
./build/dev/src/datakit-test test flex
```

`datakit-test` uses a verb grammar: no arguments prints usage, `list`
enumerates every test with its aliases, `list --json` emits the same as
JSON, `test <name>` runs one test, `test ALL` runs the whole suite
(long — prefer running the tests for what you changed), and
`bench <name>` / `speed` run benchmarks.

Frequently used names (aliases in parentheses):

```sh
./build/dev/src/datakit-test test flex
./build/dev/src/datakit-test test mflex
./build/dev/src/datakit-test test multimap          # also multimapFull, multimapMedium, multimapAtom (atom)
./build/dev/src/datakit-test test multilist         # (ml), also multilistFull
./build/dev/src/datakit-test test multiarray        # also multiarraySmall/Medium/Large
./build/dev/src/datakit-test test multidict
./build/dev/src/datakit-test test multiroar         # roaring bitmaps
./build/dev/src/datakit-test test multilru          # (lru, mlru)
./build/dev/src/datakit-test test multiOrderedSet   # (mos)
./build/dev/src/datakit-test test intset            # also intsetU32, plus intsetBig where __int128 exists
./build/dev/src/datakit-test test hyperloglog       # (hll)
./build/dev/src/datakit-test test str               # also strDoubleFormat, mds, mdsc
./build/dev/src/datakit-test test databox           # also databoxLinear
./build/dev/src/datakit-test test persist           # also persistCtx (pctx) + the *P codec tests
./build/dev/src/datakit-test test ulid
./build/dev/src/datakit-test test bigmath
./build/dev/src/datakit-test test timerWheel        # (tw), also multiTimer (timer), timeUtil (time)
./build/dev/src/datakit-test test fenwickI64        # (fw, bit, fenwick), also segmentI64 (seg), multiFenwick (mfw)
./build/dev/src/datakit-test test blake3            # (b3), also sha256, hmac, aesGcm (aes)
./build/dev/src/datakit-test test flexEnvelope      # (fenv, env)
./build/dev/src/datakit-test test clusterRing       # (ring, cluster, chr), also clusterMailbox (mb)
./build/dev/src/datakit-test test perfRegression    # (perf-baseline) regression gate
```

Run `datakit-test list` for the full set — the registry in
`src/datakit-test.c` is the single source of truth, and CMake
auto-discovers CTest targets from it.

## Documentation

**Comprehensive documentation is now available in the `docs/` directory!**

### Quick Start

- **[Getting Started Guide](docs/GETTING_STARTED.md)** - Build instructions and first examples
- **[Architecture Overview](docs/ARCHITECTURE.md)** - Design patterns and philosophy
- **[API Quick Reference](docs/API_QUICK_REFERENCE.md)** - Common operations at a glance
- **[Choosing a Container](docs/CHOOSING_A_CONTAINER.md)** - Memory against compute: which container for which job, measured
- **[Documentation Index](docs/INDEX.md)** - Complete module listing
- **[Hashes, HMAC, and the flex envelope](docs/HASH_AND_ENVELOPE.md)** - Integrity, MAC, and AEAD guide

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
- [intsetBig](docs/modules/intset/INTSET_BIG.md) - Bucketed wide-range integer sets (128-bit inputs)
- [hyperloglog](docs/modules/intset/HYPERLOGLOG.md) - Probabilistic cardinality estimation

**Prefix Sums & Range Queries:**

- [Fenwick trees](docs/modules/fenwick/FENWICK.md) - Prefix sums, point updates and rank: typed, databox and 2D variants, with a choosing guide and measured performance
- [Segment trees](docs/modules/fenwick/SEGMENT.md) - Range SUM / MIN / MAX with point and range assignment

**Compression & Encoding:**

- [xof](docs/modules/compression/XOF.md) - XOR-encoded doubles
- [dod](docs/modules/compression/DOD.md) - Delta-of-delta integer encoding
- [float16](docs/modules/compression/FLOAT16.md) - Half-precision floats

**Memory & System:**

- [membound](docs/modules/memory/MEMBOUND.md) - Bounded arena allocator
- [fibbuf](docs/modules/memory/FIBBUF.md) - Fibonacci buffer sizing
- [jebuf](docs/modules/memory/JEBUF.md) - Jemalloc-aligned allocation
- [ptrPrevNext](docs/modules/memory/PTR_PREV_NEXT.md) - Varint-packed prev/next offset pairs
- [offsetArray](docs/modules/memory/OFFSET_ARRAY.md) - Offset-based array addressing
- **System modules:**
  - [fastmutex](docs/modules/system/FASTMUTEX.md) - Fast mutex primitives
  - [multiTimer](docs/modules/system/MULTI_TIMER.md) - High-performance timers
  - [OSRegulate](docs/modules/system/OS_REGULATE.md) - Resource regulation
  - [setproctitle](docs/modules/system/SETPROCTITLE.md) - Process title setting
  - [timerWheel](docs/modules/system/TIMER_WHEEL.md) - Hierarchical timing wheel
  - [timeUtil](docs/modules/system/TIME_UTIL.md) - Time utilities
  - [versionOSRuntime](docs/modules/system/VERSION_OS_RUNTIME.md) - OS version detection

**Data Structure Utilities:**

- [multiheap](docs/modules/utils/MULTIHEAP.md) - Keyed byte-region store (ref -> resizable bytes)
- [intersectInt](docs/modules/utils/INTERSECT_INT.md) - Integer set intersection
- [rapidhash](docs/modules/utils/RAPIDHASH.md) - Fast non-cryptographic 64-bit hashing

### Advanced Topics

- [Scale-Aware Design](docs/advanced/SCALE_AWARE.md) - Choosing the right size variant
- [Performance Guide](docs/advanced/PERFORMANCE.md) - Optimization techniques
- [Memory Patterns](docs/advanced/MEMORY.md) - Understanding memory efficiency
- [Thread Safety](docs/advanced/THREAD_SAFETY.md) - Concurrent usage patterns
- [Platform Support](docs/advanced/PLATFORMS.md) - Cross-platform considerations

### Examples & Development

- [Common Patterns](docs/examples/PATTERNS.md) - Frequently used code patterns
- [Use Cases](docs/examples/USE_CASES.md) - Real-world applications
- [Benchmarking Guide](docs/examples/BENCHMARKS.md) - How to measure datakit correctly
- [Testing Guide](docs/development/TESTING.md) - Running and writing tests
- [Building Guide](docs/development/BUILDING.md) - Detailed build instructions
- [Debugging Guide](docs/development/DEBUGGING.md) - Troubleshooting techniques
- [Contributing Guide](docs/development/CONTRIBUTING.md) - Style and workflow
- [Change Runbook](docs/development/RUNBOOK.md) - The checklist every change follows: test, format, verify, measure, commit

### Performance

- [Container Performance Matrix](docs/PERFORMANCE_MATRIX.md) - Operations per second and bytes per element for every container, element kind and size (`dktools matrix`)
- [Performance Baselines Ledger](docs/PERFORMANCE_BASELINES.md) - Dated benchmark captures
- [Hash / Envelope Performance](docs/HASH_PERFORMANCE.md) - Hash and envelope throughput reference

## Status

This is a recently updated development snapshot.

Many components are complete and now fully documented.

Some components are a first or second draft and need another feature refactoring cycle or two.

## License

The Sovereign Individual's Covenant License, v1.0: free for individuals' own projects; no rights for corporate or other commercial use without a separate written license. See [LICENSE](LICENSE).
