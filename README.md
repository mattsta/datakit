# datakit: the most efficient data structures possible

Also see:

- https://matt.sh/datakit
- https://matt.sh/best-database-ever
- https://matt.sh/trillion-dollar-data-structure

datakit is a C23 library of memory-dense data structures and the
machinery to run them in production: durability, integrity, replication
and concurrency. Everything is built on one value type and one encoded
array, so a key, a row or a whole structure can be stored, compared,
snapshotted, signed and shipped the same way.

## What's in datakit

| Family                           | Modules                                                                                                                                                                                                                                 | Start with                                                                                                           |
| -------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- |
| Values and encoding              | `databox` (one 16-byte box for any value), `flex` (an array of variable-width entries in one allocation), `mflex` (flex that compresses itself), varint, `dod` / `xof` / `float16` codecs                                               | [databox](docs/modules/core/DATABOX.md), [flex](docs/modules/flex/FLEX.md)                                           |
| Tiered containers                | `multimap`, `multilist`, `multiarray`, `multiOrderedSet`, `intset`: each keeps its data in the smallest form that fits and moves between tiers as it grows and shrinks                                                                  | [Choosing a Container](docs/CHOOSING_A_CONTAINER.md), [Tiered containers](docs/modules/core/TIERS.md)                |
| Hashing, caching, cold data      | `multidict` (hash table with incremental rehash and eviction), `multilru` (multi-level LRU), `multimapAtom` (interning), `coldmap` (compressed sorted blocks), `multitier` (hot, cold and sink levels), `idRows`                        | [multidict](docs/modules/multi/MULTIDICT.md), [multitier](docs/modules/multi/MULTITIER.md)                           |
| Sets, counting, indexes          | `intsetBig`, `multiroar` (roaring bitmaps), `hyperloglog`, `linearBloom` / `linearBloomCount`, Fenwick and segment trees                                                                                                                | [multiroar](docs/modules/multi/MULTIROAR.md), [Fenwick](docs/modules/fenwick/FENWICK.md)                             |
| Durability                       | `persist` (snapshots, write-ahead log, compaction, recovery), the `*P` wrappers that log every change, and the `datakit` operator tool                                                                                                  | [persist](docs/modules/persist/PERSIST.md)                                                                           |
| Integrity, security, replication | rapidhash, xxHash, BLAKE3, SHA-256, HMAC and the key ring, ChaCha20-Poly1305 and AES-GCM with a nonce-reuse log, the flex envelope, `clusterRing` (placement), `clusterMailbox` and `clusterReplicator` (verified frames between nodes) | [Hashes, HMAC, and the flex envelope](docs/HASH_AND_ENVELOPE.md), [clusterRing](docs/modules/system/CLUSTER_RING.md) |
| Concurrency, time, system        | `ownedState` (lock-free publication), `fastmutex`, `timerWheel`, `multiTimer`, `timeUtil`, ULIDs, per-thread random numbers, diagnostics (`dkDiag`), `membound` / `jebuf` allocation, `OSRegulate`                                      | [Thread Safety](docs/advanced/THREAD_SAFETY.md), [Architecture](docs/ARCHITECTURE.md)                                |

How they fit: a value is a `databox`; containers store their rows in
`flex` arrays; the containers persist through `persistOps` descriptors
(a snapshot format plus the operations a log replays);
the envelope signs or encrypts those bytes; and the cluster pieces move
them between nodes. [Architecture](docs/ARCHITECTURE.md) walks the
layers.

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

`tsan`, `fuzz` and `coverage` complete the set; see
[Build presets](docs/BUILD_PRESETS.md). datakit needs a C23 compiler
(it is built and tested with clang 22 and gcc 15).

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

Each module's test is named after it (`flex`, `multimap`, `persist`,
`hmacKeyring`, ...). Run `datakit-test list` for the full set — the registry in
`src/datakit-test.c` is the single source of truth, and CMake
auto-discovers CTest targets from it.

## Documentation

- **[Getting Started](docs/GETTING_STARTED.md)** - build, link, first programs
- **[Architecture](docs/ARCHITECTURE.md)** - the layers, from values to replication, and the patterns every module shares
- **[Choosing a Container](docs/CHOOSING_A_CONTAINER.md)** - which container for which job, measured
- **[API Quick Reference](docs/API_QUICK_REFERENCE.md)** - common operations at a glance
- **[Documentation Index](docs/INDEX.md)** - every module guide, the advanced topics, examples and development guides
- **[Thread Safety](docs/advanced/THREAD_SAFETY.md)** - what may run concurrently, and how
- **[Change Runbook](docs/development/RUNBOOK.md)** - how every change is tested, measured and committed

Every C example in the docs is compiled against the headers by the
`docs` gate (`dktools docexamples`), so the examples match the code.

## License

The Sovereign Individual's Covenant License, v1.0: free for individuals' own projects; no rights for corporate or other commercial use without a separate written license. See [LICENSE](LICENSE).
