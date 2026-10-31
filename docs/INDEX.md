# datakit Documentation Index

**datakit** is a high-performance C library providing the most efficient data structures possible, with a focus on optimal memory usage, compression, and performance.

## Quick Links

- [Getting Started Guide](GETTING_STARTED.md) - Build instructions and first steps
- [Architecture Overview](ARCHITECTURE.md) - Design patterns and philosophy
- [API Quick Reference](API_QUICK_REFERENCE.md) - Common operations across modules
- [Choosing a Container](CHOOSING_A_CONTAINER.md) - Which container for which job, by measured throughput and bytes per element ([matrix](PERFORMANCE_MATRIX.md))
- [Hashes, HMAC, and the flex envelope](HASH_AND_ENVELOPE.md) - Integrity and AEAD usage guide

## Module Documentation

### Core Infrastructure

- [databox](modules/core/DATABOX.md) - Universal 16-byte container for any data type
- [Diagnostics (dkDiag)](modules/core/DIAGNOSTICS.md) - How datakit reports misuse and conditions without printing
- [Tiered containers](modules/core/TIERS.md) - The lifecycle, stats, size-limit defaults, tier rules and Compact shared by multimap, multiOrderedSet, multilist, multiarray and intset
- [bigmath](modules/core/BIGMATH.md) - Arbitrary-precision signed integers in a caller-owned 64-byte struct

Core platform plumbing has no standalone guide; read the headers
directly: `src/datakit.h` (platform abstraction and shared macros),
`src/dkPlatform.h` (compiler check, `DK_OS_*` flags, `DK_BITS`), `src/util.h` (general
utility functions).

### String and Buffer Management

- [str](modules/string/STR.md) - String utilities and character classification
- [dks String Buffers](modules/string/DKS.md) - Template-based string buffers (mds/mdsc)
- [strDoubleFormat](modules/string/STR_DOUBLE_FORMAT.md) - Fast double-to-string conversion
- [String Utilities](modules/string/STR_UTILS.md) - Extended string operations
- [dj](modules/string/DJ.md) - Streaming JSON writer: separators tracked, chunked output, parts joined
  without copying

### Flexible Arrays

- [flex](modules/flex/FLEX.md) - Core compressed byte array
- [mflex](modules/flex/MFLEX.md) - Compressed/uncompressed flex wrapper

Flex growth/capacity policy (`flexCapSizeLimit`, `FLEX_CAP_LEVEL_*`)
lives in `src/flexCapacityManagement.h`.

### Multi-Container Modules

These containers use **scale-aware** implementations with small/medium/large variants optimized for different sizes.

#### MultiMap - Key-Value Store with Multi-Element Values

- [MultiMap Overview](modules/multimap/MULTIMAP.md) - API and usage guide
- [Implementation Variants](modules/multimap/VARIANTS.md) - Small, Medium, and Full size tiers, plus Atom (the reference-container variant, not a size tier)
- [MultiMap Examples](modules/multimap/EXAMPLES.md) - Real-world usage patterns

#### MultiList - Linked List with Multi-Element Nodes

- [MultiList Overview](modules/multilist/MULTILIST.md) - API and usage guide
- [Implementation Variants](modules/multilist/VARIANTS.md) - Small, Medium, and Full versions
- [MultiList Examples](modules/multilist/EXAMPLES.md) - Real-world usage patterns

#### MultiArray - Dynamic Arrays with Multi-Element Support

- [MultiArray Overview](modules/multiarray/MULTIARRAY.md) - API and usage guide
- [Implementation Variants](modules/multiarray/VARIANTS.md) - Small, Medium, and Large versions, plus the `multiarrayNative*` macros for caller-owned inline arrays
- [MultiArray Examples](modules/multiarray/EXAMPLES.md) - Real-world usage patterns

#### Other Multi-Containers

- [multidict](modules/multi/MULTIDICT.md) - General-purpose dictionary
- [multiOrderedSet](modules/multi/MULTIORDEREDSET.md) - Sorted set by score with member lookup and ranks
- [multilru](modules/multi/MULTILRU.md) - Least Recently Used cache
- [multiroar](modules/multi/MULTIROAR.md) - Roaring bitmap implementation
- [idRows](modules/multi/IDROWS.md) - Rows addressed by integer id, read and changed in place without a search
- [coldmap](modules/multi/COLDMAP.md) - A sorted map in compressed blocks, for rows read far more than written
- [multitier](modules/multi/MULTITIER.md) - A key-value map in hot (multidict), cold (coldmap) and sink levels
- [Interning](modules/multi/ATOMS.md) - the atom table (multimapAtom): index, store and id choices, ids and references, measured costs

#### Persistence

- [persist](modules/persist/PERSIST.md) - Snapshots, WAL, compaction and recovery; the P wrappers and their durability results
- [Operating on persisted files](modules/persist/OPERATORS.md) - Inspect, verify, dump and repair snapshots, WALs and quarantine files with the `datakit` command (Python package in `python/`)

### Integer Sets and Cardinality

- [intset](modules/intset/INTSET.md) - Variable-width integer set (16/32/64-bit)
- [intsetU32](modules/intset/INTSET_U32.md) - Fixed 32-bit unsigned integer set
- [intsetBig](modules/intset/INTSET_BIG.md) - Bucketed wide-range integer set (128-bit inputs)
- [hyperloglog](modules/intset/HYPERLOGLOG.md) - Probabilistic cardinality estimation
- [linearBloom and linearBloomCount](modules/intset/LINEAR_BLOOM.md) - Fixed-size Bloom filters: membership, and counts with decay

`src/intsetBig.h` adds 128-bit-backed integer sets; it is compiled only
where the toolchain provides `__int128`, and its `intsetBig` test is
dropped from the CTest list otherwise.

### Prefix-Sum and Range-Query Trees

- [Fenwick trees](modules/fenwick/FENWICK.md) - Prefix sums, point updates, rank (LowerBound): typed `fenwick<T>`, databox `multiFenwickFlat` / `multiFenwick`, 2D `fenwick2d<T>` / `multiFenwick2d`; architecture, contracts, choosing guide and measured performance
- [Segment trees](modules/fenwick/SEGMENT.md) - Range SUM / MIN / MAX with point and range assignment, every integer and float type

### Compression and Encoding

- [xof](modules/compression/XOF.md) - Lossless double compression: each value XORed with the one before (Gorilla)
- [dod](modules/compression/DOD.md) - Delta-of-delta encoding for int64 values
- [float16](modules/compression/FLOAT16.md) - Half-precision floating point (F16C)

### Memory Management

- [membound](modules/memory/MEMBOUND.md) - Bounded arena allocator (locking varies by tier)
- [memtest](modules/memory/MEMTEST.md) - RAM testing from inside a process: regions, fresh allocations, the process's own memory
- [fibbuf](modules/memory/FIBBUF.md) - Fibonacci buffer sizing
- [jebuf](modules/memory/JEBUF.md) - Jemalloc-aligned buffer management
- [ptrPrevNext](modules/memory/PTR_PREV_NEXT.md) - Efficient pointer linking
- [offsetArray](modules/memory/OFFSET_ARRAY.md) - Offset-based array addressing

### System and OS Integration

- [OSRegulate](modules/system/OS_REGULATE.md) - OS resource limiting
- [setproctitle](modules/system/SETPROCTITLE.md) - Process title manipulation
- [versionOSRuntime](modules/system/VERSION_OS_RUNTIME.md) - OS/runtime detection
- [dataspeed](modules/system/DATASPEED.md) - Machine measurement: cache and memory bandwidth, latency,
  detected cache sizes, CPU throughput
- [fastmutex](modules/system/FASTMUTEX.md) - High-performance mutex
- [ownedState](modules/system/OWNED_STATE.md) - Single-writer publication without reader locks, and deferred reclamation of what it replaces
- [clusterRing](modules/system/CLUSTER_RING.md) - Replica placement by consistent hashing: strategies, quorums, topology affinity, health and rebalance plans
- [timeUtil](modules/system/TIME_UTIL.md) - Time utilities
- [multiTimer](modules/system/MULTI_TIMER.md) - Multi-timer management
- [timerWheel](modules/system/TIMER_WHEEL.md) - Hierarchical timing wheel

### Data Structure Utilities

- [multiheap](modules/utils/MULTIHEAP.md) - Keyed byte-region store (ref -> resizable bytes)
- [intersectInt](modules/utils/INTERSECT_INT.md) - Set intersection operations
- [ULID](modules/utils/ULID.md) - Time-ordered identifiers: 128-bit ULIDs and 64/32-bit compact variants, their layouts and ranges
- [Random numbers](modules/utils/RANDOM.md) - Which source for which job: the OS CSPRNG, per-thread draws, replayable streams
- [rapidhash](modules/utils/RAPIDHASH.md) - Fast non-cryptographic 64-bit hashing: variants, seeds and secrets, streaming

### Bundled Dependencies

All dependencies are vendored under `deps/`; nothing needs to be
installed. They carry their own upstream documentation:

- [deps overview](../deps/README.md) - What is vendored and why
- [varint](../deps/varint/README.md) - Variable-length integer encoding
  (see also its [architecture](../deps/varint/docs/ARCHITECTURE.md) and
  [quick reference](../deps/varint/docs/QUICK_REFERENCE.md))
- [imath](../deps/imath/README.md) - Arbitrary precision integer math
- [xxHash](../deps/xxHash/README.md) - Fast non-cryptographic hashing
- lz4 - Block/frame compression; API in `deps/lz4/lz4.h` and
  `deps/lz4/lz4frame.h`
- sha1 - SHA-1 digest; API in `deps/sha1/sha1.h`

## Hashing, Integrity, Encryption and Replication

- [Hashes, HMAC, and the flex envelope](HASH_AND_ENVELOPE.md) - Usage guide for
  BLAKE3, SHA-256, HMAC, the keyring, AEAD, and the envelope wire format.
  Within it:
  - [hmacKeyring](HASH_AND_ENVELOPE.md#hmac-keyring--multiple-keys-rotation-friendly) - keys by id, rotation, per-key AEAD nonce counters
  - [aeadNonceLog](HASH_AND_ENVELOPE.md#aead-nonce-reuse-detector-defense-in-depth) - refuses a (key, nonce) seen before
  - [ChaCha20-Poly1305 and AES-GCM](HASH_AND_ENVELOPE.md#streaming-aead-state-machine) - one-shot and streaming AEAD
  - [flexEnvelope](HASH_AND_ENVELOPE.md#flex-envelope--integrity-checked-wrapper) - checksummed, MACed or encrypted frames around any bytes
  - [clusterMailbox](HASH_AND_ENVELOPE.md#cluster-mailbox--verified-frame-router) - verified frames from many origins with replay protection
  - [clusterReplicator](HASH_AND_ENVELOPE.md#clusterreplicatorh) - ship any persisted structure through a mailbox
- [Hash / envelope performance profile](HASH_PERFORMANCE.md) - Throughput and
  latency reference for those paths

## Advanced Topics

- [Performance Tuning](advanced/PERFORMANCE.md) - Optimization guidelines
- [Performance Baselines Ledger](PERFORMANCE_BASELINES.md) - Dated benchmark captures and the decisions they support
- [Container Performance Matrix](PERFORMANCE_MATRIX.md) - Create, read, update and delete per second and bytes per element, every container, kind and size (generated)
- [Memory Efficiency](advanced/MEMORY.md) - Understanding memory patterns
- [Scale-Aware Collections](advanced/SCALE_AWARE.md) - Choosing the right size variant
- [Thread Safety](advanced/THREAD_SAFETY.md) - Concurrent usage patterns
- [Platform Support](advanced/PLATFORMS.md) - Supported platforms and features

## Testing and Development

- [Running Tests](development/TESTING.md) - Test suite guide
- [Building from Source](development/BUILDING.md) - Detailed build instructions
- [Build presets](BUILD_PRESETS.md) - dev, debug, perf, safe, tsan, fuzz and coverage
- [Contributing Guidelines](development/CONTRIBUTING.md) - How to contribute
- [Change Runbook](development/RUNBOOK.md) - The checklist every change follows
- [Debugging Tips](development/DEBUGGING.md) - Common issues and solutions

## Examples and Recipes

- [Common Patterns](examples/PATTERNS.md) - Frequently used code patterns
- [Use Cases](examples/USE_CASES.md) - Real-world application examples
- [Benchmarking Guide](examples/BENCHMARKS.md) - How to measure datakit correctly (recorded numbers live in the [baselines ledger](PERFORMANCE_BASELINES.md))

## About

- **Version**: 0.3.0
- **License**: The Sovereign Individual's Covenant License, v1.0 ([LICENSE](../LICENSE))
- **Author**: Matt Stancliff

## Platforms

Linux, macOS and the BSD family, built with clang or gcc (C23); there is
no Windows/MSVC support. See [Platform Support](advanced/PLATFORMS.md).
