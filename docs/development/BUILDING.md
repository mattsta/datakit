# Building Guide

This guide covers building datakit from source, including dependencies, build options, and troubleshooting.

## Table of Contents

- [Quick Start](#quick-start)
- [Prerequisites](#prerequisites)
- [Build Process](#build-process)
- [Build Options](#build-options)
- [Build Artifacts](#build-artifacts)
- [Platform-Specific Instructions](#platform-specific-instructions)
- [Troubleshooting](#troubleshooting)

## Quick Start

For the impatient:

```bash
mkdir build
cd build
cmake ..
make -j8
```

The test binary will be at `./src/datakit-test` and libraries in `./src/`.

## Prerequisites

### Required Tools

- **CMake** 3.5 or later
- **C Compiler** with C99/C11 support:
  - GCC 4.8+
  - Clang 3.4+
  - Apple Clang (Xcode Command Line Tools)
- **Make** or other CMake-compatible build system

### System Libraries

#### Linux

```bash
# Debian/Ubuntu
sudo apt-get install build-essential cmake

# Fedora/RHEL/CentOS
sudo yum install gcc gcc-c++ make cmake

# Arch Linux
sudo pacman -S base-devel cmake
```

#### macOS

```bash
# Install Xcode Command Line Tools
xcode-select --install

# Install CMake via Homebrew (optional, if not using Xcode)
brew install cmake
```

### Optional Dependencies

All dependencies are included in the `deps/` directory:

- **lz4**: Compression library
- **xxHash**: Fast hashing library
- **sha1**: SHA-1 implementation
- **varint**: Variable-length integer encoding
- **imath**: Arbitrary precision integer math

No external packages need to be installed.

## Build Process

### Standard Build

```bash
# Create build directory
mkdir build
cd build

# Configure
cmake ..

# Build
make -j8    # Use 8 parallel jobs

# Optionally run tests
make check
```

### Debug Build

Debug builds include symbols and disable optimizations:

```bash
mkdir build-debug
cd build-debug
cmake -DCMAKE_BUILD_TYPE=Debug ..
make -j8
```

Debug build characteristics:
- Compiler flags: `-O0 -Wno-format`
- Full debugging symbols
- No inlining optimizations
- Better for GDB/LLDB debugging

### Release Build

Release builds enable optimizations:

```bash
mkdir build-release
cd build-release
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j8
```

Release build characteristics:
- Compiler flags: `-O3`
- Maximum optimization
- Smaller binary size
- Best performance

### Out-of-Source Builds

CMake enforces out-of-source builds. The following are **prevented**:

```bash
# This will fail - source changes disabled
cd datakit
cmake .
```

Always create a separate build directory:

```bash
mkdir build && cd build
cmake ..
```

## Build Options

### CMake Configuration Options

#### BuildTestBinary

Controls whether the test binary is built (default: ON):

```bash
# Skip building tests (smaller, faster builds)
cmake -DBuildTestBinary=OFF ..

# Explicitly enable tests (default)
cmake -DBuildTestBinary=ON ..
```

#### Build32Bit

Build 32-bit binaries on 64-bit systems:

```bash
cmake -DBuild32Bit=ON ..
make -j8
```

This adds `-m32` to compilation and linking flags.

#### CMAKE_BUILD_TYPE

Control optimization level:

```bash
# Debug build (no optimization, debug symbols)
cmake -DCMAKE_BUILD_TYPE=Debug ..

# Release build (full optimization)
cmake -DCMAKE_BUILD_TYPE=Release ..

# Release with debug info
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..

# Minimum size release
cmake -DCMAKE_BUILD_TYPE=MinSizeRel ..
```

### Advanced Compiler Options

#### Sanitizers

Various sanitizers are available but **commented out by default** in CMakeLists.txt. To enable, edit `/home/user/datakit/src/CMakeLists.txt`:

```cmake
# Address Sanitizer (memory errors)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")

# Undefined Behavior Sanitizer
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=undefined")

# Memory Sanitizer (Clang only)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=memory")
```

Then rebuild:

```bash
cd build
cmake ..
make clean
make -j8
```

#### Custom Compiler Flags

Add extra compiler flags:

```bash
cmake -DCMAKE_C_FLAGS="-march=native" ..
```

Or set environment variables:

```bash
export CFLAGS="-march=native -mtune=native"
cmake ..
```

## Build Artifacts

After building, you'll find these artifacts:

### Test Binary

```
build/src/datakit-test          # Executable test suite
```

On macOS with debug builds:
```
build/src/datakit-test.dSYM/    # Debug symbols
```

### Libraries

```
build/src/datakit.so            # Shared module (.dylib on macOS)
build/src/datakit               # Static library
build/src/libdatakit.so.0.3.0   # Versioned shared library
```

Library versioning:
- **VERSION**: 0.3.0 (full version)
- **SOVERSION**: 0.3.0 (ABI version)

### Dependencies

Internal dependencies built as object files:

```
build/deps/lz4/
build/deps/xxHash/
build/deps/sha1/
build/deps/varint/
build/deps/imath/
```

## Platform-Specific Instructions

### Linux

#### Standard Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

#### Install System-Wide (Optional)

```bash
sudo make install
sudo ldconfig
```

Libraries install to `/usr/local/lib` by default.

#### Thread Support

Linux builds automatically link against:
- `pthread` - POSIX threads
- `m` - Math library

#### Random Number Generation

Linux builds detect and use:
- `getrandom()` system call (Linux 3.17+)
- `SYS_getrandom` syscall (fallback)
- `getentropy()` (if available)

### macOS

#### Standard Build

```bash
mkdir build && cd build
cmake ..
make -j$(sysctl -n hw.ncpu)
```

#### Debug Symbols

On macOS, debug symbols are generated automatically:

```bash
# After building with Debug configuration
ls -la src/datakit-test.dSYM/
```

#### Architecture

macOS builds use:
- `-undefined dynamic_lookup` for shared modules
- No explicit pthread linking (built into libSystem)
- No explicit math library (built into libSystem)

#### Xcode Integration

Generate Xcode project:

```bash
mkdir build-xcode
cd build-xcode
cmake -G Xcode ..
open datakit.xcodeproj
```

### Cross-Compilation

For cross-compilation, specify toolchain file:

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake ..
```

## Feature Detection

CMake automatically detects platform capabilities:

### Math Functions

- `isnan()` - NaN detection → `DK_HAS_ISNAN`
- `isfinite()` - Finite number check → `DK_HAS_ISFINITE`
- `isinf()` - Infinity detection → `DK_HAS_ISINF`

### Hardware Features

- **F16C instruction set** (x86-64):
  - Tests for `_cvtss_sh` and `_cvtsh_ss` intrinsics
  - Defines `DK_HAS_FL16` if available
  - Adds `-mf16c` compiler flag

### Random Number Sources

Priority order (first available is used):

1. `getrandom()` from `<sys/random.h>` → `DK_HAVE_GETRANDOM`
2. `SYS_getrandom` syscall → `DK_HAVE_LINUX_SYS_GETRANDOM`
3. `getentropy()` → `DK_HAVE_GETENTROPY`
4. `getentropy()` from `<sys/random.h>` → `DK_HAVE_GETENTROPY_SYS_RANDOM`

## Compiler Warnings

### Enabled Warnings

Datakit builds with strict warnings:

```
-Wall                           # Enable most warnings
-Wextra                         # Extra warnings
-Winline                        # Warn about inline failures
-Wshadow                        # Variable shadowing
-Wformat                        # Format string checking
-Wignored-attributes            # Ignored attributes
-Wempty-init-stmt              # Empty initialization
-Wextra-semi-stmt              # Extra semicolons
-Wno-missing-field-initializers # Allow partial struct init
-Wunused-macros                # Unused macro definitions
-fstrict-aliasing              # Strict aliasing rules
```

### Clang-Specific

```
-Wno-gnu-label-as-value        # Allow computed gotos
```

### Disabled Sanitizers

```
-fno-sanitize=alignment        # Allow alignment violations
```

This is intentional - datakit optimizes memory layout and occasionally uses non-aligned access patterns.

## Troubleshooting

### CMake Configuration Fails

#### "CMake version too old"

```
CMake Error: CMake 3.5 or higher is required
```

**Solution**: Upgrade CMake:
```bash
# Download from cmake.org or use package manager
pip install --upgrade cmake
```

#### "Compiler not found"

```
CMake Error: CMAKE_C_COMPILER not found
```

**Solution**: Install a C compiler:
```bash
# Linux
sudo apt-get install build-essential

# macOS
xcode-select --install
```

### Build Errors

#### "undefined reference to pthread_create"

On some Linux systems, pthread linking might fail.

**Solution**: Explicitly link pthread:
```bash
cmake -DCMAKE_EXE_LINKER_FLAGS="-lpthread" ..
```

#### "undefined reference to sqrt"

Math library not linked.

**Solution**: Explicitly link math library:
```bash
cmake -DCMAKE_EXE_LINKER_FLAGS="-lm" ..
```

#### "error: unknown type name 'uint64_t'"

Missing stdint.h include or C99 mode not enabled.

**Solution**: Ensure compiler supports C99:
```bash
cmake -DCMAKE_C_STANDARD=99 ..
```

### Warning: "inline function not inlined"

This is informational and safe to ignore. It indicates the compiler chose not to inline a function marked `inline`.

To disable: Remove `-Winline` from CMakeLists.txt.

### Architecture Mismatch

#### "cannot find -lxxx for -m32"

32-bit libraries not installed.

**Solution**: Install 32-bit development libraries:
```bash
# Debian/Ubuntu
sudo apt-get install gcc-multilib g++-multilib

# Fedora
sudo yum install glibc-devel.i686
```

### macOS Specific

#### "no member named 'st_mtimespec' in 'struct stat'"

This is a known issue with some macOS SDK versions.

**Solution**: Update Xcode Command Line Tools:
```bash
sudo rm -rf /Library/Developer/CommandLineTools
xcode-select --install
```

### Performance Issues

#### Slow Compilation

Use parallel builds:
```bash
make -j$(nproc)  # Linux
make -j$(sysctl -n hw.ncpu)  # macOS
```

#### Large Binary Size

Use release build with size optimization:
```bash
cmake -DCMAKE_BUILD_TYPE=MinSizeRel ..
```

Or disable test binary:
```bash
cmake -DBuildTestBinary=OFF ..
```

### Clean Builds

If you encounter persistent issues, do a clean rebuild:

```bash
cd build
make clean
rm -rf *
cmake ..
make -j8
```

Or start fresh:

```bash
rm -rf build
mkdir build
cd build
cmake ..
make -j8
```

### Checking Build Configuration

See what CMake detected:

```bash
cd build
cmake -L ..
```

For detailed information:

```bash
cmake -LA ..  # Show all variables
cmake -LAH .. # Show with help text
```

## Build System Details

### Directory Structure

```
build/
├── CMakeCache.txt              # CMake configuration cache
├── CMakeFiles/                 # CMake internal files
├── compile_commands.json       # Compilation database
├── deps/                       # Built dependencies
│   ├── lz4/
│   ├── xxHash/
│   ├── sha1/
│   ├── varint/
│   └── imath/
└── src/                        # Main build output
    ├── CMakeFiles/
    ├── datakit-test           # Test executable
    ├── datakit.so             # Shared module
    ├── datakit                # Static library
    └── libdatakit.so.0.3.0    # Versioned shared library
```

### Position Independent Code

All code is built with `-fPIC` (Position Independent Code), allowing it to be used in shared libraries.

### Compilation Database

`compile_commands.json` is generated automatically, useful for:
- Clang-based tools (clang-tidy, clangd)
- IDEs and editors with C/C++ support
- Static analysis tools

Enable explicitly if needed:
```bash
cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ..
```

## Next Steps

After building:

- See [TESTING.md](TESTING.md) for running tests
- See [DEBUGGING.md](DEBUGGING.md) for debugging techniques
- See [CONTRIBUTING.md](CONTRIBUTING.md) for development workflow
