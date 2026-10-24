# Getting Started with datakit

This guide will help you build and start using the datakit library.

## Building datakit

### Prerequisites

- CMake 3.10 or higher (3.21+ to use the build presets)
- clang or gcc with C11 support
- Make (the presets use Unix Makefiles)
- Linux, macOS, or a BSD. Windows/MSVC is not supported.

### Build Steps

The presets in `CMakePresets.json` are the supported entry points:

```sh
cd datakit

# Default development configuration: Release-O3 + ASAN/UBSAN
cmake --preset dev
cmake --build --preset dev
```

That produces `build-dev/`. The other presets are `debug` (-O0 +
symbols + sanitizers), `perf` (native + LTO, no sanitizers — use this
for every benchmark), and `safe` (sanitizers, for CI).

A hand-configured out-of-source build also works:

```sh
mkdir build && cd build
cmake ..
make -j8
```

### Build Options

```sh
# Choose an optimization level in a hand-configured build
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake -DCMAKE_BUILD_TYPE=Release ..

# Sanitizers (note the exact option spelling; all default to OFF)
cmake -DEnableASAN=ON -DEnableUBSAN=ON ..

# Skip the test binary
cmake -DBuildTestBinary=OFF ..
```

### Build Outputs

After building, `build-dev/src/` (or `build/src/`) holds:

1. **Static library**: `datakit.a` - Link statically into your application
2. **Shared library**: `libdatakit.so.0.3.0` (`.dylib` on macOS) - Dynamic linking
3. **Loadable module**: `datakit.so` - For `dlopen`-style consumers
4. **Test executable**: `datakit-test` - Run tests and examples

Note the missing `lib` prefix on the static library and the module:
the build clears `CMAKE_STATIC_LIBRARY_PREFIX` and
`CMAKE_SHARED_MODULE_PREFIX`.

## Running Tests

The test suite provides both validation and usage examples:

```sh
# Discover what exists — the binary is the source of truth
./build-dev/src/datakit-test list

# Run one module's tests
./build-dev/src/datakit-test test multimap
./build-dev/src/datakit-test test multilist
./build-dev/src/datakit-test test flex
./build-dev/src/datakit-test test intset
./build-dev/src/datakit-test test hyperloglog

# Size-specific variants
./build-dev/src/datakit-test test multimapFull
./build-dev/src/datakit-test test multilistFull
./build-dev/src/datakit-test test multiarrayLarge

# Everything (long)
./build-dev/src/datakit-test test ALL
```

Every registered test is also a CTest target, which is the faster way
to run several:

```sh
ctest --test-dir build-dev -j8 -R '^(flex|mflex|multimap)$'
```

### Available Tests

A representative slice; run `datakit-test list` for the complete set
with aliases.

**Core Data Structures:**

- `flex` - Flexible compressed array
- `mflex` - Compressed flex wrapper
- `multimap`, `multimapMedium`, `multimapFull`, `multimapAtom` (`atom`)
- `multilist` (`ml`), `multilistFull`
- `multiarray`, `multiarraySmall`, `multiarrayMedium`, `multiarrayLarge`
- `multidict` - Hash table
- `list`, `ptrPrevNext` (`ppn`)

**Integer Sets and Trees:**

- `intset` - Tiered variable-width integer set
- `intsetU32` - 32-bit unsigned integer set
- `intsetBig` - 128-bit-backed sets; registered only where the
  toolchain provides `__int128`
- `hyperloglog` (`hll`) - Cardinality estimation
- `linearBloom` (`bloom`) - Bloom filter
- `fenwickI64` (`fw`), `segmentI64` (`seg`), `multiFenwick` (`mfw`),
  `multiFenwickFlat` (`mfwf`), `fenwick2d` (`fw2d`), `multiFenwick2d`
  (`mfw2d`); `fenwickTypes` and `segmentTypes` test every typed tree;
  `fenwickBench` (`fwbench`) prints their speed and size matrix

**Specialized Containers:**

- `multiroar` - Roaring bitmap
- `multilru` (`lru`) - LRU cache
- `multiOrderedSet` (`mos`) - Ordered set

**Compression/Encoding:**

- `xof` - XOR filter float compression
- `dod` - Delta-of-delta integer encoding
- `float16` (`f16`) - Half-precision floats
- `compressionBench` (`compress`)

**String/Buffer:**

- `str` - String utilities
- `mds` - Full string buffer (benchmark: `bench mdsbench`)
- `mdsc` - Compact string buffer (benchmark: `bench mdscbench`)
- `strDoubleFormat` - Double-to-string formatting

**Memory/System:**

- `membound` - Bounded arena allocator
- `memtest` - Memory subsystem checks
- `fibbuf` - Fibonacci buffer sizing
- `jebuf` - Jemalloc size classes
- `ptrlib`, `bigmath`, `bbits`, `ulid`

**Timers:**

- `timerWheel` (`tw`), `multiTimer` (`timer`), `timeUtil` (`time`)

**Persistence:**

- `persist`, `persistCtx` (`pctx`), plus per-type codec tests
  (`multimapP`, `multilistP`, `multidictP`, `flexP`, `intsetP`,
  `multiOrderedSetP`, `multiroarP`, `multilruP`)

**Hashing / Integrity / Cluster:**

- `blake3` (`b3`), `sha256`, `sha1`, `hmac`, `hmacKeyring` (`keyring`)
- `chacha20poly1305` (`c20`), `aesGcm` (`aes`), `x25519` (`ecdh`),
  `rapidhash` (`rh`)
- `flexEnvelope` (`fenv`), `aeadNonceLog` (`noncelog`)
- `clusterRing` (`ring`), `clusterMailbox` (`mb`),
  `clusterReplicator` (`rep`)

**Utilities:**

- `databox`, `databoxLinear` - Universal container
- `offsetArray` - Offset-based arrays
- `util` - General utilities
- `intersectInt` - Integer set intersection
- `perfRegression` (`perf-baseline`) - Regression gate

## Your First datakit Program

### Example 1: Using flex (Flexible Array)

```c
#include "databox.h"
#include "flex.h"
#include <stdio.h>

int main(void) {
    flex *f = flexNew();

    /* Append elements. The trailing argument picks the endpoint. */
    flexPushBytes(&f, "Hello", 5, FLEX_ENDPOINT_TAIL);
    flexPushBytes(&f, "World", 5, FLEX_ENDPOINT_TAIL);
    flexPushBytes(&f, "!", 1, FLEX_ENDPOINT_TAIL);

    /* Iterate. Use flexHeadIfAny, not flexHead: flexHead returns an
     * insertion POSITION and is non-NULL even for an empty flex, so a
     * flexHead-seeded loop reads one entry past the buffer when empty. */
    for (flexEntry *fe = flexHeadIfAny(f); fe; fe = flexNext(f, fe)) {
        databox box;
        flexGetByType(fe, &box);
        const size_t len = databoxLen(&box);
        printf("Element: %.*s (len=%zu)\n", (int)len,
               (const char *)databoxCBytes(&box), len);
    }

    /* Cleanup */
    flexFree(f);
    return 0;
}
```

### Example 2: Using multimap (Key-Value Store)

Every multimap read and write goes through `databox`, and the entry
width is fixed when the map is created.

```c
#include "databox.h"
#include "multimap.h"
#include <stdio.h>

static void put(multimap **m, const char *k, const char *v) {
    databox key = databoxNewBytesString(k);
    databox val = databoxNewBytesString(v);
    const databox *entry[2] = {&key, &val};
    multimapInsert(m, entry); /* replaces the entry when the key exists */
}

int main(void) {
    /* 2 elements per entry == key + value */
    multimap *m = multimapNew(2);

    put(&m, "user:1", "Alice");
    put(&m, "user:2", "Bob");
    put(&m, "user:1", "Alice Smith"); /* Update */

    /* Lookup fills elementsPerEntry - 1 boxes: the values only. */
    databox key = databoxNewBytesString("user:1");
    databox value;
    if (multimapLookupInto(m, &key, &value, 1)) {
        printf("Found: %.*s\n", (int)databoxLen(&value),
               (const char *)databoxCBytes(&value));
    }

    /* Count entries */
    printf("Total entries: %zu\n", multimapCount(m));

    /* Cleanup */
    multimapFree(m);
    return 0;
}
```

### Example 3: Using intset (Integer Set)

`intset` takes `intset **` because adding a value can retier and
reallocate the set in place.

```c
#include "intset.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

int main(void) {
    intset *is = intsetNew();

    /* Add integers */
    bool success;
    intsetAdd(&is, 42, &success);
    intsetAdd(&is, 100, &success);
    intsetAdd(&is, -50, &success);

    /* Check membership */
    if (intsetFind(is, 42)) {
        printf("42 is in the set\n");
    }

    /* Get size */
    printf("Set size: %zu\n", intsetCount(is));

    /* Smallest and largest are O(1) */
    int64_t lo;
    int64_t hi;
    if (intsetMin(is, &lo) && intsetMax(is, &hi)) {
        printf("Range: %" PRId64 " .. %" PRId64 "\n", lo, hi);
    }

    /* Remove element */
    intsetRemove(&is, 42, &success);

    /* Cleanup */
    intsetFree(is);
    return 0;
}
```

### Example 4: Using hyperloglog (Cardinality Estimation)

`hyperloglogAdd` takes `hyperloglog **` because the sparse
representation can grow and get promoted to dense as cardinality
rises.

```c
#include "hyperloglog.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

int main(void) {
    hyperloglog *hll = hyperloglogNew();

    /* Add elements */
    for (int i = 0; i < 100000; i++) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "element:%d", i);
        hyperloglogAdd(&hll, buf, (size_t)len);
    }

    /* Estimate cardinality. 'invalid' reports a corrupt encoding. */
    bool invalid = false;
    uint64_t estimate = hyperloglogCount(hll, &invalid);
    printf("Estimated unique elements: %" PRIu64 " (actual: 100000)\n",
           estimate);
    printf("Error rate: %.2f%%\n",
           ((double)estimate - 100000.0) / 1000.0);

    /* Cleanup */
    hyperloglogFree(hll);
    return 0;
}
```

## Compilation

### Linking Against datakit

datakit installs nothing and ships no pkg-config file, so point the
compiler at the source and build trees directly. The static library is
`datakit.a` — no `lib` prefix — so link it by path rather than with
`-ldatakit`.

```sh
DK=../datakit    # wherever you checked datakit out

# Static
cc -o myapp myapp.c -I"$DK/src" "$DK/build-dev/src/datakit.a" -lm -lpthread

# Versioned shared library
cc -o myapp myapp.c -I"$DK/src" -L"$DK/build-dev/src" -ldatakit \
   -Wl,-rpath,"$DK/build-dev/src"
```

On macOS, drop `-lm -lpthread`: both live in libSystem.

### CMake Integration

`add_subdirectory` is the supported path, since there is no install
tree or config package to find:

```cmake
cmake_minimum_required(VERSION 3.10)
project(MyApp C)

add_subdirectory(deps/datakit)

add_executable(myapp myapp.c)
target_include_directories(myapp PRIVATE deps/datakit/src)
target_link_libraries(myapp datakit-static)   # or datakit-library
```

If you only want the library, set `BuildTestBinary=OFF` before the
`add_subdirectory` call so the test runner is skipped:

```cmake
set(BuildTestBinary OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/datakit)
```

## Next Steps

Now that you have datakit built and running:

1. **Explore the Modules**: Read the [Architecture Overview](ARCHITECTURE.md) to understand design patterns
2. **Choose Your Containers**: Learn about [Scale-Aware Collections](advanced/SCALE_AWARE.md)
3. **Study Examples**: Check out [Common Patterns](examples/PATTERNS.md) for real-world usage
4. **Performance Tuning**: Read [Performance Guide](advanced/PERFORMANCE.md) for optimization tips
5. **Deep Dive**: Explore individual module documentation in the [Index](INDEX.md)

## Common Issues

### Build Failures

**Issue**: `cmake: command not found`

```bash
# Install CMake
sudo apt-get install cmake  # Debian/Ubuntu
brew install cmake          # macOS
```

**Issue**: Missing dependencies

```bash
# The dependencies are included in deps/
# Make sure you have the full source tree
```

**Issue**: Platform-specific features not available

```bash
# Some features require specific platform support
# src/dkPlatform.h sets the DK_OS_* flag for the target; OS-specific code
# is guarded by those flags (e.g. /proc readers by DK_OS_LINUX)
```

### Runtime Issues

**Issue**: Segmentation fault

- Check whether the API takes `container *` or `container **`. Anything
  that can reallocate (`flexPush*`, `intsetAdd`, `multimapInsert`,
  `hyperloglogAdd`) takes the double pointer, and passing a stale
  single pointer afterwards reads freed memory.
- Build under the `dev` or `debug` preset; ASAN names the offending
  access directly.

**Issue**: Memory leaks

- Always call the corresponding free function for each container
- ASAN's leak checker is on in the `dev`, `debug`, and `safe` presets
- Or use valgrind: `valgrind --leak-check=full ./myapp`

**Issue**: Assertion failures

- Debug builds include assertions for safety
- Read the assertion message for hints
- Common: accessing NULL pointers, invalid indices

## Getting Help

- **Documentation**: Check the [Index](INDEX.md) for module-specific guides
- **Examples**: Read the `*Test()` functions at the end of each
  implementation file (guarded by `#ifdef DATAKIT_TEST`) — they are the
  most complete, always-compiled usage examples in the tree
- **Source Code**: Headers in `src/*.h` have detailed API documentation
- **Website**: Visit https://matt.sh/datakit for additional resources

## Performance Tips

1. **Choose the Right Variant**: Use small/medium/large variants based on expected data size
2. **Pre-allocate**: Some containers support capacity hints to avoid reallocation
3. **Batch Operations**: Group insertions together when possible
4. **Profile First**: Use the built-in test benchmarks as reference
5. **Read the Source**: The implementation often contains performance notes
