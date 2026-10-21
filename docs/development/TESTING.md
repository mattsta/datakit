# Testing Guide

This guide covers how to run tests, write new tests, and work with the test framework in datakit.

## Table of Contents

- [Running Tests](#running-tests)
- [Test Framework](#test-framework)
- [Writing New Tests](#writing-new-tests)
- [Test Coverage](#test-coverage)
- [Continuous Integration](#continuous-integration)

## Running Tests

### Building the Test Binary

The test binary is one of the four targets every build preset builds:

```sh
cmake --preset dev && cmake --build --preset dev
```

That produces `build-dev/src/datakit-test`. Substitute `debug`, `perf`,
or `safe` for a different configuration (see
[BUILDING.md](BUILDING.md)). A plain `mkdir build && cd build && cmake
.. && make -j8` also works and puts the binary at `build/src/datakit-test`.

To skip building the test binary (for production builds):

```sh
cmake -DBuildTestBinary=OFF ..
```

### The datakit-test verb grammar

`datakit-test` takes a command as its first argument:

| Invocation       | Behavior                                               |
| ---------------- | ------------------------------------------------------ |
| _(no arguments)_ | Print usage and exit non-zero                          |
| `help`           | Print usage                                            |
| `list`           | List every registered test with its aliases            |
| `list --json`    | Same listing as JSON, plus a `count` field             |
| `test <name>`    | Run one test (name or alias, case-insensitive)         |
| `test ALL`       | Run every registered test                              |
| `bench <name>`   | Run a benchmark (`mdsbench`, `mdscbench`)              |
| `speed [opts]`   | Run the dataspeed system benchmark (`--json`, `--csv`) |

`test ALL` runs the whole suite and takes a long time. In day-to-day
work, run only the tests covering what you changed, and batch them with
ctest when you need several.

### Discovering test names

The registry in `src/datakit-test.c` is the single source of truth. Ask
the binary rather than trusting a hand-maintained list:

```sh
./build-dev/src/datakit-test list
./build-dev/src/datakit-test list --json
```

### Running Individual Tests

```sh
./build-dev/src/datakit-test test <name>
```

Frequently used names, with aliases in comments:

```sh
# Containers
./build-dev/src/datakit-test test flex
./build-dev/src/datakit-test test mflex
./build-dev/src/datakit-test test multimap        # multimapFull, multimapMedium, multimapAtom (atom)
./build-dev/src/datakit-test test multilist       # ml; also multilistFull
./build-dev/src/datakit-test test multiarray      # multiarraySmall / Medium / Large
./build-dev/src/datakit-test test multidict
./build-dev/src/datakit-test test multiroar
./build-dev/src/datakit-test test multilru        # lru, mlru
./build-dev/src/datakit-test test multiOrderedSet # mos
./build-dev/src/datakit-test test list
./build-dev/src/datakit-test test ptrPrevNext     # ppn

# Numeric / set types
./build-dev/src/datakit-test test intset
./build-dev/src/datakit-test test intsetU32
./build-dev/src/datakit-test test hyperloglog     # hll
./build-dev/src/datakit-test test linearBloom     # bloom
./build-dev/src/datakit-test test float16         # f16
./build-dev/src/datakit-test test fenwickI64      # fw, bit, fenwick
./build-dev/src/datakit-test test segmentI64      # seg, segtree, segment
./build-dev/src/datakit-test test multiFenwick    # mfw, mbit
./build-dev/src/datakit-test test multiFenwickFlat # mfwf
./build-dev/src/datakit-test test fenwick2d       # fw2d
./build-dev/src/datakit-test test multiFenwick2d  # mfw2d
./build-dev/src/datakit-test test fenwickTypes    # fwtypes: every typed fenwick
./build-dev/src/datakit-test test segmentTypes    # segtypes: every typed segment
./build-perf/src/datakit-test test fenwickBench   # fwbench: speed/size matrix

# Encoding / compression
./build-dev/src/datakit-test test dod
./build-dev/src/datakit-test test xof
./build-dev/src/datakit-test test compressionBench # compress, cbench

# Strings
./build-dev/src/datakit-test test str
./build-dev/src/datakit-test test strDoubleFormat
./build-dev/src/datakit-test test mds
./build-dev/src/datakit-test test mdsc

# Buffers, memory, utilities
./build-dev/src/datakit-test test fibbuf
./build-dev/src/datakit-test test jebuf
./build-dev/src/datakit-test test ptrlib
./build-dev/src/datakit-test test bigmath
./build-dev/src/datakit-test test membound
./build-dev/src/datakit-test test memtest
./build-dev/src/datakit-test test databox         # also databoxLinear
./build-dev/src/datakit-test test offsetArray
./build-dev/src/datakit-test test util
./build-dev/src/datakit-test test ulid
./build-dev/src/datakit-test test bbits

# Timers
./build-dev/src/datakit-test test timerWheel      # tw
./build-dev/src/datakit-test test multiTimer      # timer
./build-dev/src/datakit-test test timeUtil        # time

# Persistence
./build-dev/src/datakit-test test persist
./build-dev/src/datakit-test test persistCtx      # pctx
./build-dev/src/datakit-test test multimapP       # mmP; also multilistP, multidictP, flexP,
                                                  # intsetP, multiOrderedSetP, multiroarP, multilruP

# Hashing, MAC, AEAD, envelope
./build-dev/src/datakit-test test blake3          # b3; also blake3Neon, blake3SSE41, blake3AVX2
./build-dev/src/datakit-test test sha256          # also sha256ArmCrypto, sha256ShaNi
./build-dev/src/datakit-test test hmac
./build-dev/src/datakit-test test hmacKeyring     # keyring
./build-dev/src/datakit-test test chacha20poly1305 # c20, chacha, aead-prim
./build-dev/src/datakit-test test aesGcm          # aes, aesgcm
./build-dev/src/datakit-test test x25519          # ecdh
./build-dev/src/datakit-test test rapidhash       # rh, rapid
./build-dev/src/datakit-test test flexEnvelope    # fenv, env
./build-dev/src/datakit-test test aeadNonceLog    # noncelog, nonce-log

# Cluster
./build-dev/src/datakit-test test clusterRing     # ring, cluster, chr
./build-dev/src/datakit-test test clusterMailbox  # mailbox, mb
./build-dev/src/datakit-test test clusterReplicator # replicator, rep

# Regression gate
./build-dev/src/datakit-test test perfRegression  # perf-baseline, perfbase
```

Several dozen narrow, focused crypto and cluster checks are registered
alongside these (for example `envelopeZeroCopy`, `keyringPersistOps`,
`mailboxRestoreFuzz`). Use `list` to see them all.

### Running All Tests

```sh
./build-dev/src/datakit-test test ALL
```

CMake auto-discovers one CTest target per registered test by parsing
the registry out of `src/datakit-test.c`, so ctest is the better way to
run more than one:

```sh
# Everything, 8 at a time
ctest --test-dir build-dev -j8 --output-on-failure

# Just the tests you touched
ctest --test-dir build-dev -j8 -R '^(flex|mflex|multimap)$'
```

Two convenience targets wrap ctest:

```sh
cmake --build build-dev --target datakit-check       # ctest -VV
cmake --build build-dev --target datakit-quickcheck  # ctest --output-on-failure
```

### Test Exit Codes

- `0` - All tests passed
- Non-zero - Number of test failures
- `-3` - Test not found

## Test Framework

### Overview

datakit uses a custom lightweight testing framework defined in
`src/ctest.h` (unrelated to CMake's `ctest` runner). Tests are embedded
directly in the source files and compiled conditionally based on the
`DATAKIT_TEST` preprocessor flag.

### Key Macros

The ctest framework provides these macros:

```c
// Test identification
TEST(name)                    // Print test name
TEST_DESC(name, ...)          // Print formatted test description

// Error reporting
ERROR                         // Increment error counter
ERR(format, ...)             // Print formatted error with location
ERRR(message)                // Print simple error with location

// Test completion
TEST_FINAL_RESULT            // Print results and return error count

// Helper macros
currentFilename()            // Print file:function:line info
```

### String Generation Helpers

For tests that need generated key/value strings:

```c
#define CTEST_INCLUDE_KEYGEN    // Include genkey() function
#define CTEST_INCLUDE_VALGEN    // Include genval() function
#define CTEST_INCLUDE_KVGEN     // Include both genkey() and genval()
```

These generate strings in the format "prefix123" where 123 is an integer.

## Writing New Tests

### Basic Test Structure

Tests follow this pattern in source files:

```c
#ifdef DATAKIT_TEST
int myModuleTest(int argc, char *argv[]) {
    int err = 0;  // Error counter

    TEST("Description of test category");

    // Test code here
    if (condition_fails) {
        ERR("Expected %d but got %d", expected, actual);
    }

    if (another_failure) {
        ERRR("This specific operation failed");
    }

    TEST_FINAL_RESULT;  // Returns err count
}
#endif
```

### Complete Example

Here's a complete example based on the datakit patterns:

```c
// In mymodule.h
#ifdef DATAKIT_TEST
int myModuleTest(int argc, char *argv[]);
#endif

// In mymodule.c
#ifdef DATAKIT_TEST
#define CTEST_INCLUDE_KVGEN  // Include key/value generators
#include "ctest.h"

int myModuleTest(int argc, char *argv[]) {
    int err = 0;

    TEST("Basic operations");
    {
        myModule *m = myModuleNew();
        if (!m) {
            ERRR("Failed to create module");
        }

        // Test insertion
        bool result = myModuleInsert(m, "key1", "value1");
        if (!result) {
            ERR("Insert failed for key=%s", "key1");
        }

        myModuleFree(m);
    }

    TEST("Stress test with many elements");
    {
        myModule *m = myModuleNew();
        const int iterations = 10000;

        for (int i = 0; i < iterations; i++) {
            char *key = genkey("k", i);
            char *val = genval("v", i);

            if (!myModuleInsert(m, key, val)) {
                ERR("Failed at iteration %d", i);
                break;
            }
        }

        if (myModuleCount(m) != iterations) {
            ERR("Expected %d elements but got %zu",
                iterations, myModuleCount(m));
        }

        myModuleFree(m);
    }

    TEST_FINAL_RESULT;
}
#endif
```

### Adding Test to Test Runner

There is exactly one place to register a test: the `testRegistry[]`
table in `src/datakit-test.c`. `test ALL` walks that table, and
`src/CMakeLists.txt` parses it at configure time to generate the CTest
targets — so nothing else needs editing.

1. Include your header in `src/datakit-test.c`:

```c
#include "mymodule.h"
```

2. Add one entry to `testRegistry[]`, picking the macro that matches
   your test function:

```c
/* Plain: test name is the function prefix, args passed through */
T(myModule),

/* With aliases (comma-separated, matched case-insensitively) */
T_A(myModule, "mm,mymod"),

/* Adjusted args: the test sees argc - 2 / argv + 2, so argv[0] is
 * the first argument AFTER "test <name>" */
T_ADJ(myModule),

/* Aliases plus adjusted args */
T_A_ADJ(myModule, "mm,mymod"),
```

3. Rebuild. `datakit-test list` and `ctest -N` should both show the new
   name.

A test that only compiles under a feature guard belongs inside the same
`#if` in the registry, and needs a matching removal in the CMake
discovery block (see how `intsetBig` is handled for `__int128`).

## Test Coverage

### Manual Testing Patterns

Datakit tests follow comprehensive patterns:

1. **Basic Operations**: Create, insert, retrieve, delete
2. **Boundary Conditions**: Empty structures, single element, maximum size
3. **Stress Tests**: Large numbers of operations (typically 10,000-100,000)
4. **Random Operations**: Randomized insertions, deletions, lookups
5. **Memory Validation**: All allocations properly freed

### Memory Testing

Run tests under valgrind to detect memory leaks:

```bash
valgrind --leak-check=full --show-leak-kinds=all \
         ./build-debug/src/datakit-test test flex
```

For all tests:

```bash
for test in flex multimap multilist; do
    echo "Testing $test..."
    valgrind --leak-check=full ./build-debug/src/datakit-test test $test 2>&1 | \
        grep -E "definitely lost|indirectly lost|ERROR SUMMARY"
done
```

The `dev`, `debug`, and `safe` presets already build with ASAN and
UBSAN, which catch most of what valgrind would — reach for valgrind
when you need its specific reports or a sanitizer-free binary.

### Performance Testing

Some tests include timing information. Watch for output like:

```
SUCCESS (0.123456 sec total; 0.012345 sec deletion; 1,234,567 total bytes)
```

`speed` is its own top-level command, not a test name:

```sh
./build-perf/src/datakit-test speed            # defaults: 64 MB working set
./build-perf/src/datakit-test speed 64 10      # 64 MB, 10 legacy iterations
./build-perf/src/datakit-test speed --json     # machine-readable
```

Always measure with the `perf` preset. Sanitizer builds invalidate every
number, and unevenly. Measured across perfRegression's rows (Apple M1 Max,
clang 22), the `dev` preset (ASan+UBSan) ran 1.0-7.7x slower than `perf`.
Memory-bandwidth-bound and hardware-instruction paths (xxh3 on large
inputs, SHA-256 with ARMv8 Crypto) barely changed. Scalar code with many
instrumented loads and stores per byte (BLAKE3 at 16 KiB and up, membound)
slowed the most. Reproduce it with:

```sh
./build-dev/src/datakit-test test perfRegression > dev.tsv
./build-perf/src/datakit-test test perfRegression > perf.tsv
uv run --project tools dktools perfdiff perf.tsv dev.tsv --tolerance 0
```

## Continuous Integration

### Pre-commit Testing

Test what you changed, by name:

```sh
cmake --build --preset dev
./build-dev/src/datakit-test test <name>
ctest --test-dir build-dev -j8 -R '^(<name>|<other>)$'
```

The full suite is long; run it deliberately rather than as a reflex:

```sh
ctest --test-dir build-dev -j8 --output-on-failure
```

### Configuration Coverage

Before a release, run the suite under more than one preset — the
sanitizer builds and the optimized build find different bugs:

```sh
cmake --preset safe && cmake --build --preset safe
ctest --test-dir build-safe -j8 --output-on-failure

cmake --preset perf && cmake --build --preset perf
ctest --test-dir build-perf -j8 --output-on-failure
```

## Benchmarking

Benchmarks live in a separate registry and use the `bench` verb:

```sh
./build-perf/src/datakit-test bench mdsbench
./build-perf/src/datakit-test bench mdscbench
```

`allstr` is a pseudo-test that runs `mds`, `mdsbench`, `mdsc`, and
`mdscbench` together:

```sh
./build-perf/src/datakit-test test allstr
```

Some benchmarks are registered as ordinary tests instead, such as
`compressionBench` (aliases `compress`, `cbench`) and the
`perfRegression` gate.

## Test Organization

Tests are organized as follows:

- **Source Files**: Tests are at the end of implementation files (`.c`)
- **Test Declarations**: Declared in headers with `#ifdef DATAKIT_TEST`
- **Test Runner**: `src/datakit-test.c` holds `testRegistry[]` and dispatches to individual tests
- **CMake Integration**: `src/CMakeLists.txt` parses `testRegistry[]` and emits one CTest target per name

## Tips for Effective Testing

1. **Use descriptive TEST() names**: Help identify which category failed
2. **Clean up resources**: Always free allocated memory
3. **Check edge cases**: Empty, single element, maximum size
4. **Use meaningful error messages**: Include actual vs expected values
5. **Test both success and failure paths**: Ensure error handling works
6. **Avoid platform dependencies**: Tests should work on Linux, macOS, and BSD
7. **Keep tests fast**: Aim for sub-second execution for individual tests
8. **Test incrementally**: Add tests as you develop features

## Troubleshooting Test Failures

### Test Not Found

If you get "Test not found!" error:

- Check spelling of test name (case-insensitive)
- Ensure test is registered in datakit-test.c
- Verify test binary was rebuilt after adding test

### Segmentation Faults

If tests crash:

1. Run under a debugger: `lldb -- ./build-debug/src/datakit-test test <name>`
   (macOS) or `gdb --args ./build-debug/src/datakit-test test <name>` (Linux)
2. Rebuild under the `debug` or `safe` preset and read the sanitizer report
3. Run with valgrind: `valgrind ./build-debug/src/datakit-test test <name>`
4. Check for uninitialized variables
5. Verify all pointers before dereferencing
6. Ensure proper cleanup of resources

### Assertion Failures

If asserts fail:

- Check the assertion message and location
- Review recent changes to that code path
- Run in debug build for better error messages
- Add print statements to trace execution

### Intermittent Failures

If tests fail randomly:

- May indicate race conditions (check threading)
- May indicate uninitialized memory
- Run under valgrind's helgrind or DRD tools
- Check for dependencies on random number generation
