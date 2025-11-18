# Debugging Guide

This guide covers debugging techniques, common issues, and tools for developing and troubleshooting datakit.

## Table of Contents

- [Debug Builds](#debug-builds)
- [Debugging Tools](#debugging-tools)
- [Common Issues](#common-issues)
- [Memory Debugging](#memory-debugging)
- [Performance Profiling](#performance-profiling)
- [Static Analysis](#static-analysis)

## Debug Builds

### Creating a Debug Build

Always use a debug build when debugging:

```bash
mkdir build-debug
cd build-debug
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j8
```

Debug build characteristics:
- **No optimization** (`-O0`) - easier to step through
- **Debug symbols** - variable names and line numbers
- **No inlining** - easier to trace function calls
- **Relaxed format warnings** - `-Wno-format`

### Verify Debug Symbols

Check if symbols are present:

```bash
# Linux
nm ./src/datakit-test | grep flexNew
readelf -w ./src/datakit-test | head -50

# macOS
nm ./src/datakit-test | grep flexNew
dsymutil --verify ./src/datakit-test
```

## Debugging Tools

### GDB (GNU Debugger)

#### Basic Usage

```bash
# Start debugger
gdb ./src/datakit-test

# Common commands
(gdb) run test flex              # Run with arguments
(gdb) break flexNew              # Set breakpoint on function
(gdb) break flex.c:1234          # Set breakpoint at line
(gdb) break flex.c:1234 if i==5  # Conditional breakpoint
(gdb) continue                   # Continue execution
(gdb) next                       # Step over
(gdb) step                       # Step into
(gdb) finish                     # Step out
(gdb) print variable             # Print variable
(gdb) backtrace                  # Show call stack
(gdb) frame 3                    # Switch to frame 3
(gdb) info locals                # Show local variables
(gdb) quit                       # Exit debugger
```

#### Debugging Crashes

When a test crashes:

```bash
gdb ./src/datakit-test
(gdb) run test multimap
# ... program crashes ...
(gdb) backtrace          # See where it crashed
(gdb) frame 0            # Look at crash location
(gdb) list               # Show source code
(gdb) print *ptr         # Examine variables
```

#### Running Specific Tests

```bash
gdb --args ./src/datakit-test test flex
(gdb) break flexTest
(gdb) run
```

#### Pretty Printing

For better structure visualization:

```bash
(gdb) set print pretty on
(gdb) set print array on
(gdb) set print object on
```

#### Save Breakpoints

```bash
# Save session
(gdb) save breakpoints my-breakpoints.txt

# Restore later
(gdb) source my-breakpoints.txt
```

### LLDB (Debugger for Clang/macOS)

LLDB has similar but slightly different syntax:

```bash
# Start debugger
lldb ./src/datakit-test

# Common commands
(lldb) run test flex              # Run with arguments
(lldb) breakpoint set -n flexNew  # Set breakpoint
(lldb) breakpoint set -f flex.c -l 1234
(lldb) continue                   # Continue
(lldb) next                       # Step over
(lldb) step                       # Step into
(lldb) finish                     # Step out
(lldb) print variable             # Print variable
(lldb) bt                         # Backtrace
(lldb) frame select 3             # Switch frame
(lldb) frame variable             # Show local variables
(lldb) quit                       # Exit
```

### Core Dumps

#### Enable Core Dumps

```bash
# Check current limit
ulimit -c

# Enable unlimited core dumps
ulimit -c unlimited

# Run program (core dump will be generated on crash)
./src/datakit-test test multimap
```

#### Analyze Core Dump

```bash
# Linux
gdb ./src/datakit-test core

# macOS (core dumps in /cores/)
lldb ./src/datakit-test -c /cores/core.12345
```

#### Make Core Dumps Permanent

Add to `~/.bashrc` or `~/.zshrc`:

```bash
ulimit -c unlimited
```

## Common Issues

### Segmentation Faults

#### Symptom
```
Segmentation fault (core dumped)
```

#### Debugging Steps

1. **Run under debugger:**
```bash
gdb ./src/datakit-test
(gdb) run test flex
(gdb) backtrace
```

2. **Check for NULL pointers:**
```c
if (!ptr) {
    printf("Pointer is NULL!\n");
    return NULL;
}
```

3. **Use valgrind:**
```bash
valgrind --leak-check=full ./src/datakit-test test flex
```

#### Common Causes

- Dereferencing NULL pointers
- Buffer overflows
- Use-after-free
- Stack overflow (deep recursion)
- Uninitialized pointers

### Assertion Failures

#### Symptom
```
datakit-test: flex.c:1234: flexNew: Assertion `size > 0' failed.
Aborted (core dumped)
```

#### Debugging Steps

1. **Read the assertion:**
   - File: `flex.c`
   - Line: `1234`
   - Function: `flexNew`
   - Condition: `size > 0`

2. **Check the failing condition:**
```bash
gdb ./src/datakit-test
(gdb) break flex.c:1234
(gdb) run test flex
(gdb) print size
```

3. **Trace back to see why condition failed:**
```bash
(gdb) backtrace
(gdb) up
(gdb) print size
```

#### Common Causes

- Invalid input parameters
- Incorrect state assumptions
- Logic errors
- Race conditions (multi-threaded code)

### Memory Corruption

#### Symptoms

- Random crashes
- Values changing unexpectedly
- Crashes in allocator (malloc/free)
- Different behavior on different runs

#### Debugging Techniques

1. **Electric Fence:**
```bash
# Install electric fence
sudo apt-get install electric-fence  # Linux

# Link against electric fence
export LD_PRELOAD=/usr/lib/libefence.so.0.0
./src/datakit-test test flex
```

2. **Address Sanitizer:**

Edit `/home/user/datakit/src/CMakeLists.txt`:
```cmake
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
```

Rebuild and run:
```bash
cd build-debug
cmake ..
make clean
make -j8
./src/datakit-test test flex
```

3. **Valgrind (detailed in next section)**

### Test Failures

#### Symptom
```
test — Basic operations
flex.c:flexTest:1234    ERROR! Expected 5 but got 3
Sorry, not all tests passed!  In fact, 1 tests failed.
```

#### Debugging Steps

1. **Identify the test:**
   - File: `flex.c`
   - Function: `flexTest`
   - Line: `1234`

2. **Run under debugger:**
```bash
gdb ./src/datakit-test
(gdb) break flex.c:1234
(gdb) run test flex
```

3. **Add debug output:**
```c
printf("DEBUG: Before operation, count=%d\n", count);
result = flexDoSomething(f);
printf("DEBUG: After operation, count=%d, result=%d\n", count, result);
```

4. **Simplify the test:**
   - Comment out parts of the test
   - Reduce iteration counts
   - Test with minimal data

## Memory Debugging

### Valgrind

#### Basic Memory Check

```bash
valgrind --leak-check=full ./src/datakit-test test flex
```

#### Detailed Memory Check

```bash
valgrind \
    --leak-check=full \
    --show-leak-kinds=all \
    --track-origins=yes \
    --verbose \
    --log-file=valgrind-output.txt \
    ./src/datakit-test test flex

# Review output
less valgrind-output.txt
```

#### Common Valgrind Output

**Memory Leak:**
```
==12345== 48 bytes in 1 blocks are definitely lost
==12345==    at 0x4C2FB0F: malloc (in /usr/lib/valgrind/...)
==12345==    by 0x10A123: flexNew (flex.c:234)
==12345==    by 0x10A456: flexTest (flex.c:6789)
```

**Solution:** Add `flexFree(f)` before returning.

**Invalid Read:**
```
==12345== Invalid read of size 8
==12345==    at 0x10A789: flexNext (flex.c:456)
==12345==    by 0x10AABC: flexTest (flex.c:7890)
==12345==  Address 0x52345678 is 0 bytes after a block of size 48 alloc'd
```

**Solution:** Buffer overflow - check array bounds.

**Uninitialized Value:**
```
==12345== Conditional jump or move depends on uninitialised value(s)
==12345==    at 0x10ADEF: flexCompare (flex.c:1234)
```

**Solution:** Initialize variables before use.

#### Valgrind Options Reference

```bash
--leak-check=full              # Detailed leak detection
--show-leak-kinds=all          # Show all leak types
--track-origins=yes            # Track origin of uninitialized values
--leak-check-heuristics=all    # More thorough leak detection
--num-callers=30               # Longer stack traces
--malloc-fill=0xAB             # Fill malloc'd memory
--free-fill=0xCD               # Fill freed memory
```

### Memory Sanitizer (Clang)

Detect uninitialized memory reads:

```bash
# Edit CMakeLists.txt to enable
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=memory")

# Rebuild
cmake .. && make clean && make -j8

# Run
./src/datakit-test test flex
```

### Undefined Behavior Sanitizer

Detect undefined behavior:

```bash
# Edit CMakeLists.txt
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=undefined")

# Rebuild and run
cmake .. && make clean && make -j8
./src/datakit-test test flex
```

Catches:
- Integer overflow
- Division by zero
- Null pointer dereference
- Misaligned access
- Type punning violations

## Performance Profiling

### gprof

1. **Build with profiling:**
```bash
cmake -DCMAKE_C_FLAGS="-pg" ..
make clean
make -j8
```

2. **Run program:**
```bash
./src/datakit-test test flex
```

3. **Generate profile:**
```bash
gprof ./src/datakit-test gmon.out > profile.txt
less profile.txt
```

### perf (Linux)

```bash
# Record profile
perf record -g ./src/datakit-test test flex

# View report
perf report

# Annotate source
perf annotate
```

### Instruments (macOS)

```bash
# Profile with Instruments
instruments -t "Time Profiler" ./src/datakit-test test flex

# Or open Instruments GUI
open -a Instruments
```

### Cachegrind (Valgrind)

Cache and branch prediction profiling:

```bash
valgrind --tool=cachegrind ./src/datakit-test test flex

# View results
cg_annotate cachegrind.out.<pid>
```

### Callgrind (Valgrind)

Call graph profiling:

```bash
valgrind --tool=callgrind ./src/datakit-test test flex

# Visualize with kcachegrind
kcachegrind callgrind.out.<pid>
```

## Static Analysis

### Compiler Warnings

Datakit enables extensive warnings by default. To see all warnings:

```bash
cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j8 2>&1 | tee build-warnings.txt
```

Review warnings in `build-warnings.txt`.

### Clang Static Analyzer

```bash
# Use scan-build
mkdir build-analyze
cd build-analyze
scan-build cmake ..
scan-build make -j8

# View results
scan-view /tmp/scan-build-<timestamp>
```

### Clang-Tidy

```bash
# Generate compile_commands.json
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ..

# Run clang-tidy on a file
clang-tidy src/flex.c

# Run on all files
find src -name '*.c' | xargs clang-tidy
```

Common checks:
```bash
clang-tidy -checks='*' src/flex.c
clang-tidy -checks='modernize-*,readability-*' src/flex.c
```

### Cppcheck

```bash
# Install cppcheck
sudo apt-get install cppcheck

# Run on entire codebase
cppcheck --enable=all --inconclusive --std=c99 src/
```

### Sparse

Linux kernel's semantic checker:

```bash
# Install sparse
sudo apt-get install sparse

# Run via CGO wrapper
CC=cgcc make clean
CC=cgcc make -j8
```

## Advanced Debugging Techniques

### Logging and Tracing

Add debug logging:

```c
#ifdef DEBUG_TRACE
#define TRACE(fmt, ...) \
    fprintf(stderr, "[%s:%d] " fmt "\n", __func__, __LINE__, ##__VA_ARGS__)
#else
#define TRACE(fmt, ...) do {} while(0)
#endif
```

Build with tracing:
```bash
cmake -DCMAKE_C_FLAGS="-DDEBUG_TRACE" ..
```

### Watch Points

Set hardware watchpoints in GDB:

```bash
(gdb) watch variable
(gdb) watch *0x12345678  # Watch memory address
(gdb) continue           # Break when value changes
```

### Reverse Debugging (GDB)

Record and replay:

```bash
(gdb) record
(gdb) continue
# ... program crashes ...
(gdb) reverse-step      # Step backward
(gdb) reverse-continue  # Continue backward
```

### Printf Debugging

Sometimes the simplest approach:

```c
printf("DEBUG %s:%d: value=%d, ptr=%p\n", __FILE__, __LINE__, value, ptr);
fflush(stdout);  // Ensure output before crash
```

### Conditional Compilation

Use debug-only code:

```c
#ifdef DATAKIT_TEST
    // Validation code
    assert(validate_structure(f));
#endif
```

### Rubber Duck Debugging

When stuck:
1. Explain the problem to someone (or a rubber duck)
2. Walk through code line by line
3. Often you'll find the issue while explaining

## Debugging Specific Issues

### Data Structure Corruption

1. **Add validation functions:**
```c
bool flexValidate(flex *f) {
    // Check structure invariants
    if (flexCount(f) < 0) return false;
    if (flexBytes(f) < FLEX_EMPTY_SIZE) return false;
    // ... more checks ...
    return true;
}
```

2. **Call after each operation:**
```c
flexPush(&f, value);
assert(flexValidate(f));
```

### Race Conditions

For multi-threaded code:

```bash
# Thread sanitizer
cmake -DCMAKE_C_FLAGS="-fsanitize=thread" ..
make clean && make -j8

# Helgrind (Valgrind)
valgrind --tool=helgrind ./src/datakit-test test multimap

# DRD (Valgrind)
valgrind --tool=drd ./src/datakit-test test multimap
```

### Deadlocks

1. Enable lock debugging
2. Check lock ordering
3. Use timeout locks
4. Review with helgrind/DRD

## Tips and Best Practices

1. **Always use debug builds when debugging**
2. **Enable core dumps** - invaluable for post-mortem analysis
3. **Run valgrind regularly** - catch issues early
4. **Use sanitizers during development** - they catch bugs at runtime
5. **Check warnings** - they often indicate real problems
6. **Write asserts liberally** - catch invalid states early
7. **Test incrementally** - easier to isolate issues
8. **Keep debug symbols** - even in release builds (RelWithDebInfo)
9. **Use version control** - bisect to find when bugs were introduced
10. **Document workarounds** - explain non-obvious code

## Getting Help

If you're stuck:

1. **Review error messages carefully** - they usually point to the issue
2. **Check git history** - see if recent changes introduced the bug
3. **Simplify the problem** - create minimal reproduction case
4. **Read the code** - understand what it's supposed to do
5. **Take a break** - fresh eyes often spot issues immediately

## Reference Links

- [GDB Documentation](https://sourceware.org/gdb/documentation/)
- [LLDB Tutorial](https://lldb.llvm.org/use/tutorial.html)
- [Valgrind Manual](https://valgrind.org/docs/manual/manual.html)
- [AddressSanitizer](https://github.com/google/sanitizers/wiki/AddressSanitizer)
- [Clang Static Analyzer](https://clang-analyzer.llvm.org/)
