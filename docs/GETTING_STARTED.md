# Getting Started with datakit

This guide will help you build and start using the datakit library.

## Building datakit

### Prerequisites

- CMake 3.5 or higher
- C99-compatible compiler (GCC, Clang, etc.)
- Make or Ninja build system
- 64-bit or 32-bit architecture supported

### Build Steps

```bash
# Clone the repository (if not already done)
git clone <repository-url> datakit
cd datakit

# Create build directory
mkdir build
cd build

# Configure with CMake
cmake ..

# Build with parallel jobs
make -j8

# Optional: Install to system
sudo make install
```

### Build Options

```bash
# Debug build with symbols
cmake -DCMAKE_BUILD_TYPE=Debug ..

# Release build with optimizations
cmake -DCMAKE_BUILD_TYPE=Release ..

# Enable sanitizers (Address Sanitizer and Undefined Behavior Sanitizer)
cmake -DENABLE_ASAN=ON -DENABLE_UBSAN=ON ..
```

### Build Outputs

After building, you'll have:

1. **Static library**: `libdatakit.a` - Link statically into your application
2. **Shared library**: `libdatakit.so` (`.dylib` on macOS) - Dynamic linking
3. **Test executable**: `datakit-test` - Run tests and examples

## Running Tests

The test suite provides both validation and usage examples:

```bash
# From the build directory
cd build

# Run all tests
./src/datakit-test test ALL

# Run specific module tests
./src/datakit-test test multimap
./src/datakit-test test multilist
./src/datakit-test test flex
./src/datakit-test test intset
./src/datakit-test test hyperloglog

# Run size-specific variants
./src/datakit-test test multimapFull
./src/datakit-test test multilistSmall
./src/datakit-test test multiarrayLarge
```

### Available Tests

Here are all the available test modules:

**Core Data Structures:**
- `flex` - Flexible compressed array
- `mflex` - Compressed flex wrapper
- `multimap` - Key-value map with multi-element values
- `multilist` - Linked list with multi-element nodes
- `multiarray` - Dynamic array with multi-element support

**Size Variants:**
- `multimapFull`, `multimap` (small/medium implied)
- `multilistFull`, `multilistSmall`, `multilist`
- `multiarrayLarge`, `multiarrayMedium`, `multiarraySmall`

**Integer Sets:**
- `intset` - Variable-width integer set
- `intsetU32` - 32-bit unsigned integer set
- `intsetBig` - 64-bit large integer set (x86_64 only)
- `hyperloglog` or `hll` - Cardinality estimation

**Specialized Containers:**
- `multiroar` - Roaring bitmap
- `multilru` - LRU cache
- `multimapatom` or `atom` - Atom-based map

**Compression/Encoding:**
- `xof` - XOR filter float compression
- `dod` - Delta-of-delta integer encoding
- `float16` or `f16` - Half-precision floats

**String/Buffer:**
- `str` - String utilities
- `mds` / `mdsbench` - Full string buffer
- `mdsc` / `mdscbench` - Compact string buffer
- `strDoubleFormat` - Double-to-string formatting

**Memory/System:**
- `membound` - Memory allocation limits
- `fibbuf` - Fibonacci buffer sizing
- `jebuf` - Jemalloc buffer sizing
- `ptrprevnext` or `ppn` - Pointer linking

**Utilities:**
- `databox` - Universal container
- `offsetArray` - Offset-based arrays
- `util` - General utilities
- `sha1` - SHA1 hashing

## Your First datakit Program

### Example 1: Using flex (Flexible Array)

```c
#include "flex.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    flex *f = NULL;

    /* Create new flex and add elements */
    flexPushBytes(&f, "Hello", 5);
    flexPushBytes(&f, "World", 5);
    flexPushBytes(&f, "!", 1);

    /* Iterate through elements */
    flexEntry entry;
    flexIter *iter = flexIterator(f, FLEX_START_HEAD);
    while (flexNext(iter, &entry)) {
        printf("Element: %.*s (len=%zu)\n",
               (int)entry.len, (char *)entry.data, entry.len);
    }
    flexIteratorFree(iter);

    /* Cleanup */
    flexFree(f);
    return 0;
}
```

### Example 2: Using multimap (Key-Value Store)

```c
#include "multimap.h"
#include <stdio.h>

int main(void) {
    multimap *m = NULL;

    /* Create new multimap */
    multimapNew(&m);

    /* Insert key-value pairs */
    multimapInsertWithSurrogateKey(&m, "user:1", 6, "Alice", 5);
    multimapInsertWithSurrogateKey(&m, "user:2", 6, "Bob", 3);
    multimapInsertWithSurrogateKey(&m, "user:1", 6, "Alice Smith", 11); /* Update */

    /* Lookup value by key */
    void *value;
    size_t vlen;
    if (multimapLookup(m, "user:1", 6, &value, &vlen)) {
        printf("Found: %.*s\n", (int)vlen, (char *)value);
    }

    /* Count elements */
    printf("Total elements: %zu\n", multimapCount(m));

    /* Cleanup */
    multimapFree(&m);
    return 0;
}
```

### Example 3: Using intset (Integer Set)

```c
#include "intset.h"
#include <stdio.h>

int main(void) {
    intset *is = intsetNew();

    /* Add integers */
    uint8_t success;
    is = intsetAdd(is, 42, &success);
    is = intsetAdd(is, 100, &success);
    is = intsetAdd(is, -50, &success);

    /* Check membership */
    if (intsetFind(is, 42)) {
        printf("42 is in the set\n");
    }

    /* Get size */
    printf("Set size: %u\n", intsetLen(is));

    /* Remove element */
    is = intsetRemove(is, 42, &success);

    /* Cleanup */
    intsetFree(is);
    return 0;
}
```

### Example 4: Using hyperloglog (Cardinality Estimation)

```c
#include "hyperloglog.h"
#include <stdio.h>

int main(void) {
    hyperloglog *hll = hyperloglogCreate();

    /* Add elements */
    for (int i = 0; i < 100000; i++) {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "element:%d", i);
        hyperloglogAdd(hll, (uint8_t *)buf, len);
    }

    /* Estimate cardinality */
    uint64_t estimate = hyperloglogCount(hll);
    printf("Estimated unique elements: %lu (actual: 100000)\n", estimate);
    printf("Error rate: %.2f%%\n",
           ((double)estimate - 100000.0) / 1000.0);

    /* Cleanup */
    hyperloglogFree(hll);
    return 0;
}
```

## Compilation

### Linking Against datakit

```bash
# Using static library
gcc -o myapp myapp.c -I/path/to/datakit/src -L/path/to/datakit/build/src -ldatakit -lm -lpthread

# Using shared library
gcc -o myapp myapp.c -I/path/to/datakit/src -L/path/to/datakit/build/src -ldatakit -Wl,-rpath=/path/to/datakit/build/src

# Using pkg-config (if installed)
gcc -o myapp myapp.c $(pkg-config --cflags --libs datakit)
```

### CMake Integration

If you're using CMake for your project:

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.5)
project(MyApp)

# Find datakit
find_library(DATAKIT_LIB datakit PATHS /path/to/datakit/build/src)
include_directories(/path/to/datakit/src)

# Add your executable
add_executable(myapp myapp.c)
target_link_libraries(myapp ${DATAKIT_LIB} m pthread)
```

Or using add_subdirectory:

```cmake
# Add datakit as subdirectory
add_subdirectory(deps/datakit)

# Link against it
add_executable(myapp myapp.c)
target_link_libraries(myapp datakit)
```

## Next Steps

Now that you have datakit built and running:

1. **Explore the Modules**: Read the [Architecture Overview](ARCHITECTURE.md) to understand design patterns
2. **Choose Your Containers**: Learn about [Scale-Aware Collections](../advanced/SCALE_AWARE.md)
3. **Study Examples**: Check out [Common Patterns](../examples/PATTERNS.md) for real-world usage
4. **Performance Tuning**: Read [Performance Guide](../advanced/PERFORMANCE.md) for optimization tips
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
# Check src/config.h for detected features
```

### Runtime Issues

**Issue**: Segmentation fault
- Make sure to initialize pointers to NULL before creating containers
- Example: `multimap *m = NULL; multimapNew(&m);`

**Issue**: Memory leaks
- Always call the corresponding free function for each container
- Use valgrind to detect leaks: `valgrind --leak-check=full ./myapp`

**Issue**: Assertion failures
- Debug builds include assertions for safety
- Read the assertion message for hints
- Common: accessing NULL pointers, invalid indices

## Getting Help

- **Documentation**: Check the [Index](INDEX.md) for module-specific guides
- **Examples**: Look at test code in `src/*-test.c` files or test functions at the end of implementation files
- **Source Code**: Headers in `src/*.h` have detailed API documentation
- **Website**: Visit https://matt.sh/datakit for additional resources

## Performance Tips

1. **Choose the Right Variant**: Use small/medium/large variants based on expected data size
2. **Pre-allocate**: Some containers support capacity hints to avoid reallocation
3. **Batch Operations**: Group insertions together when possible
4. **Profile First**: Use the built-in test benchmarks as reference
5. **Read the Source**: The implementation often contains performance notes
