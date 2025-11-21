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

Tests are built automatically when you build the project:

```bash
mkdir build
cd build
cmake ..
make -j8
```

The test binary will be created at `./src/datakit-test`.

To skip building the test binary (for production builds):

```bash
cmake -DBuildTestBinary=OFF ..
make -j8
```

### Running Individual Tests

Run a specific test by name:

```bash
./src/datakit-test test <test_name>
```

Available tests include:

```bash
# Data structure tests
./src/datakit-test test flex
./src/datakit-test test mflex
./src/datakit-test test multimap
./src/datakit-test test multimapFull
./src/datakit-test test multilist
./src/datakit-test test multilistFull
./src/datakit-test test multiarray
./src/datakit-test test multiarraySmall
./src/datakit-test test multiarrayMedium
./src/datakit-test test multiarrayLarge
./src/datakit-test test multimapatom
./src/datakit-test test multilru
./src/datakit-test test multiroar

# Utility tests
./src/datakit-test test intset
./src/datakit-test test intsetU32
./src/datakit-test test hll              # HyperLogLog
./src/datakit-test test dod
./src/datakit-test test xof
./src/datakit-test test fibbuf
./src/datakit-test test jebuf
./src/datakit-test test membound
./src/datakit-test test ptrprevnext
./src/datakit-test test strDoubleFormat

# String tests
./src/datakit-test test str
./src/datakit-test test mds
./src/datakit-test test mdsc
./src/datakit-test test util

# Other
./src/datakit-test test databox
./src/datakit-test test offsetArray
```

### Running All Tests

Run all tests at once:

```bash
./src/datakit-test test ALL
```

Or use CMake's test runner:

```bash
make check
```

Or using ctest directly:

```bash
ctest -VV  # Very verbose output
ctest      # Regular output
```

### Test Exit Codes

- `0` - All tests passed
- Non-zero - Number of test failures
- `-3` - Test not found

## Test Framework

### Overview

Datakit uses a custom lightweight testing framework called `ctest`, defined in `/home/user/datakit/src/ctest.h`. Tests are embedded directly in the source files and compiled conditionally based on the `DATAKIT_TEST` preprocessor flag.

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

After writing your test, add it to `/home/user/datakit/src/datakit-test.c`:

1. Include your header:

```c
#include "mymodule.h"
```

2. Add test case in the `main()` function:

```c
} else if (!strcasecmp(argv[2], "mymodule")) {
    return myModuleTest(argc - 2, argv + 2);
```

3. Add to the ALL test suite:

```c
} else if (!strcasecmp(argv[2], "ALL")) {
    uint32_t result = 0;
    // ... existing tests ...
    result += myModuleTest(argc, argv);
    return result;
}
```

4. Add to CMake test list in `/home/user/datakit/src/CMakeLists.txt`:

```cmake
set(datakitTests
    # ... existing tests ...
    mymodule
    )
```

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
         ./src/datakit-test test flex
```

For all tests:

```bash
for test in flex multimap multilist; do
    echo "Testing $test..."
    valgrind --leak-check=full ./src/datakit-test test $test 2>&1 | \
        grep -E "definitely lost|indirectly lost|ERROR SUMMARY"
done
```

### Performance Testing

Some tests include timing information. Watch for output like:

```
SUCCESS (0.123456 sec total; 0.012345 sec deletion; 1,234,567 total bytes)
```

You can also use the dedicated speed test:

```bash
./src/datakit-test test speed <arg1> <arg2>
```

## Continuous Integration

### Pre-commit Testing

Before committing, run the full test suite:

```bash
cd build
make clean
make -j8
make check
```

Or test individual modules you modified:

```bash
./src/datakit-test test <module_name>
```

### Release Testing

For release builds, test with optimizations enabled:

```bash
mkdir build-release
cd build-release
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j8
./src/datakit-test test ALL
```

### Platform Testing

Test on both debug and release configurations:

```bash
# Debug build (default)
mkdir build-debug
cd build-debug
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j8
make check

# Release build
mkdir build-release
cd build-release
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j8
make check
```

## Benchmarking

Some modules include benchmark tests:

```bash
./src/datakit-test test mdsbench
./src/datakit-test test mdscbench
./src/datakit-test test allstr  # Runs both tests and benchmarks
```

## Test Organization

Tests are organized as follows:

- **Source Files**: Tests are at the end of implementation files (`.c`)
- **Test Declarations**: Declared in headers with `#ifdef DATAKIT_TEST`
- **Test Runner**: `/home/user/datakit/src/datakit-test.c` dispatches to individual tests
- **CMake Integration**: Tests are registered in `/home/user/datakit/src/CMakeLists.txt`

## Tips for Effective Testing

1. **Use descriptive TEST() names**: Help identify which category failed
2. **Clean up resources**: Always free allocated memory
3. **Check edge cases**: Empty, single element, maximum size
4. **Use meaningful error messages**: Include actual vs expected values
5. **Test both success and failure paths**: Ensure error handling works
6. **Avoid platform dependencies**: Tests should work on Linux and macOS
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

1. Run under GDB: `gdb --args ./src/datakit-test test <name>`
2. Run with valgrind: `valgrind ./src/datakit-test test <name>`
3. Check for uninitialized variables
4. Verify all pointers before dereferencing
5. Ensure proper cleanup of resources

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
