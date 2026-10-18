# Unified Build and Test System

This document describes the unified build and test system for the varint library.

## Quick Start

```bash
# Build everything
mkdir build && cd build
cmake ..
make -j4

# Run unit tests
make test

# Run comprehensive tests with sanitizers
make varint-test-comprehensive

# Check for compiler warnings
make varint-check-warnings
```

## Build System Overview

The varint project uses CMake as its primary build system with integrated CTest for unified testing.

### Build Targets

- **`make`** or **`make all`** - Builds all libraries, tests, and examples
- **`make test`** - Runs all unit tests via CTest
- **`make varint-test-comprehensive`** - Runs comprehensive test suite with AddressSanitizer and UndefinedBehaviorSanitizer
- **`make varint-check-warnings`** - Checks all source files for compiler warnings with GCC and Clang

### Project Structure

```
varint/
├── CMakeLists.txt              # Root build configuration with test integration
├── src/
│   ├── CMakeLists.txt          # Source library and unit test builds
│   ├── varint*.c               # Implementation files
│   ├── varint*.h               # Header files
│   └── varint*Test.c          # Unit test files
├── examples/
│   ├── CMakeLists.txt          # Example builds
│   ├── standalone/             # Standalone example programs
│   ├── integration/            # Integration examples
│   └── advanced/               # Advanced use cases
├── scripts/
│   ├── test/                   # Test execution scripts
│   │   ├── test_all_comprehensive.sh       # Comprehensive test runner
│   │   ├── run_all_tests.sh                # Unit tests with sanitizers
│   │   └── run_unit_tests.sh               # Quick unit test runner
│   └── build/                  # Build and compiler checking scripts
│       ├── run_all_compilers.sh            # Multi-compiler checker
│       └── check_warnings.sh               # Single-compiler checker
```

## Testing Levels

### 1. Unit Tests (CTest)

Fast, focused tests for individual components. Run with:

```bash
cd build
make test
# or
ctest
# or with verbose output
ctest --verbose
```

**Unit Tests Included:**

- `varint-packed` - Packed varint encoding
- `varint-dimension` - Dimensional varint encoding
- `varint-delta` - Delta encoding with ZigZag
- `varint-for` - Frame of Reference encoding
- `varint-pfor` - Patched Frame of Reference
- `varint-group` - Grouped varint encoding
- `varint-dict` - Dictionary encoding
- `varint-adaptive` - Adaptive encoding selection
- `varint-float` - Float compression
- `varint-bitmap` - Bitmap encoding

### 2. Comprehensive Tests (Sanitizers)

Thorough testing with memory safety checks. Run with:

```bash
cd build
make varint-test-comprehensive
```

This runs:

- All unit tests
- All standalone examples
- All integration examples
- All advanced examples
- All reference examples

With **AddressSanitizer** and **UndefinedBehaviorSanitizer** enabled to catch:

- Memory leaks
- Buffer overflows
- Use-after-free
- Undefined behavior
- Integer overflows

### 3. Compiler Warning Checks

Cross-compiler warning validation. Run with:

```bash
cd build
make varint-check-warnings
```

This checks all source files with:

- GCC with `-Wall -Wextra -Wpedantic -Wformat`
- Clang with `-Wall -Wextra -Wpedantic -Wformat`

Results are saved to `compiler_checks/` directory.

## Build Options

### Debug vs Release

```bash
# Debug build (no optimization, debug symbols)
cmake -DCMAKE_BUILD_TYPE=Debug ..
make

# Release build (O3 optimization)
cmake -DCMAKE_BUILD_TYPE=Release ..
make
```

### Disable Test Building

```bash
cmake -DVARINT_BUILD_TESTS=OFF ..
make
```

## Continuous Integration

The `.github/workflows/` directory contains GitHub Actions workflows that run:

1. **Build Matrix** - Builds on Ubuntu/macOS with GCC/Clang
2. **Unit Tests** - Runs `make test` on all platforms
3. **Comprehensive Tests** - Runs `make varint-test-comprehensive` with sanitizers
4. **Compiler Checks** - Runs `make varint-check-warnings` to ensure zero warnings
5. **cppcheck** - Static analysis for code quality

## Development Workflow

### Before Committing

**ALWAYS** run the complete test suite locally:

```bash
# 1. Build everything
mkdir -p build && cd build
cmake ..
make -j4

# 2. Run unit tests
make test

# 3. Run comprehensive tests
make varint-test-comprehensive

# 4. Check for warnings
make varint-check-warnings
```

If ANY step fails, **DO NOT COMMIT**. Fix the issues first.

### Adding New Tests

To add a new unit test:

1. Create `src/varintMyFeatureTest.c` with this structure:

   ```c
   #include "varintMyFeature.h"
   #include "ctest.h"
   #include <inttypes.h>

   int varintMyFeatureTest(int argc, char *argv[]) {
       (void)argc;
       (void)argv;
       int32_t err = 0;

       TEST("My test case") {
           // Test code here
           if (condition_fails) {
               ERRR("Test failed");
           }
       }

       TEST_FINAL_RESULT;
   }

   #ifdef VARINT_MY_FEATURE_TEST_STANDALONE
   int main(int argc, char *argv[]) {
       return varintMyFeatureTest(argc, argv);
   }
   #endif
   ```

2. Add to `src/CMakeLists.txt`:

   ```cmake
   add_executable(varintMyFeatureTest varintMyFeatureTest.c varintMyFeature.c varintExternal.c)
   target_compile_definitions(varintMyFeatureTest PRIVATE VARINT_MY_FEATURE_TEST_STANDALONE)
   add_test(NAME varint-myfeature COMMAND varintMyFeatureTest)
   ```

3. Add to root `CMakeLists.txt` dependencies:
   ```cmake
   add_custom_target(varint-test-comprehensive
       ...
       DEPENDS
           ... varintMyFeatureTest
   )
   ```

### Adding New Examples

To add a new example:

1. Create `examples/category/my_example.c`
2. Add to `examples/CMakeLists.txt`:

   ```cmake
   add_executable(my_example category/my_example.c ${VARINT_SOURCES})
   ```

3. The comprehensive test suite will automatically discover and test it

## Cross-Platform Compatibility

### Printf Format Specifiers

**ALWAYS** use `<inttypes.h>` macros for sized integer types:

```c
#include <inttypes.h>

uint64_t value = 12345;
printf("Value: %" PRIu64 "\n", value);  // ✓ CORRECT

// NEVER use:
printf("Value: %lu\n", value);   // ✗ WRONG - fails on macOS
printf("Value: %llu\n", value);  // ✗ WRONG - fails on Linux
```

**Format Macros:**

- `PRIu64` for `uint64_t`
- `PRId64` for `int64_t`
- `PRIu32` for `uint32_t`
- `PRId32` for `int32_t`
- `%zu` for `size_t` (already portable)

### Compiler Differences

The build system tests with both GCC and Clang because they have different warning sets:

- GCC catches certain portability issues
- Clang catches different undefined behavior
- **Both must pass with zero warnings**

## Troubleshooting

### "make: \*\*\* No targets specified and no makefile found"

You need to run `cmake` first:

```bash
mkdir build && cd build
cmake ..
```

### "Test failed: undefined reference to main"

The test file is missing its `#ifdef` wrapper. Check that it has:

```c
#ifdef VARINT_MY_TEST_STANDALONE
int main(int argc, char *argv[]) {
    return myTest(argc, argv);
}
#endif
```

And that CMakeLists.txt has:

```cmake
target_compile_definitions(myTest PRIVATE VARINT_MY_TEST_STANDALONE)
```

### "warning: format '%lu' expects argument of type 'unsigned long'"

Use `<inttypes.h>` macros instead:

```c
#include <inttypes.h>
printf("%" PRIu64, my_uint64_value);
```

### "AddressSanitizer: heap-buffer-overflow"

Run the specific test under gdb or valgrind:

```bash
cd build/src
gdb ./varintMyTest
run
bt
```

## Why This Unified System?

**Before:** Multiple disconnected test systems:

- Manual compilation of tests
- Separate shell scripts for comprehensive tests
- No unified "does everything work?" command
- Easy to forget to run all tests

**After:** Single unified system:

- `make test` - Fast unit tests
- `make varint-test-comprehensive` - Complete validation
- `make varint-check-warnings` - Zero-warning guarantee
- CI automatically runs everything
- **Impossible to forget** - it's just `make test`

## Summary Commands

```bash
# Development cycle
mkdir -p build && cd build
cmake ..
make -j4 && make test && make varint-test-comprehensive && make varint-check-warnings

# If all pass → commit and push
# If any fail → fix issues first
```

**Rule:** If `make test && make varint-test-comprehensive && make varint-check-warnings` all pass, the code is ready to commit.
