# Building Guide

This guide covers building datakit from source, including dependencies, build options, and troubleshooting.

## Table of Contents

- [Quick Start](#quick-start)
- [Prerequisites](#prerequisites)
- [Build Presets](#build-presets)
- [Build Process](#build-process)
- [Build Options](#build-options)
- [Build Artifacts](#build-artifacts)
- [Platform-Specific Instructions](#platform-specific-instructions)
- [Troubleshooting](#troubleshooting)

## Quick Start

For the impatient:

```sh
cmake --preset dev && cmake --build --preset dev
```

The test binary will be at `build-dev/src/datakit-test` and the
libraries alongside it in `build-dev/src/`.

## Prerequisites

### Required Tools

- **CMake** 3.10 or later for a plain build; **3.21 or later** to use
  the presets in `CMakePresets.json`
- **C Compiler**: clang or gcc, with C11 support. Apple Clang from the
  Xcode Command Line Tools works. MSVC is not supported and datakit
  contains no MSVC-specific code paths.
- **Make** or another CMake-compatible generator (the presets use Unix
  Makefiles)

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

## Build Presets

`CMakePresets.json` defines the four supported configurations. Each
build preset builds the same four targets — `datakit-test`,
`datakit-static`, `datakit-shared`, and `datakit-library` — into
`build-<preset>/`.

| Preset  | Build type | Sanitizers  | Extra flags               | Use for                           |
| ------- | ---------- | ----------- | ------------------------- | --------------------------------- |
| `dev`   | Release    | ASAN, UBSAN | —                         | Default day-to-day development    |
| `debug` | Debug      | ASAN, UBSAN | —                         | Stepping in a debugger            |
| `perf`  | Release    | none        | `-O3 -mcpu=native -flto`  | Every benchmark and timing number |
| `safe`  | Release    | ASAN, UBSAN | —                         | Strict CI correctness runs        |

```sh
cmake --preset dev    && cmake --build --preset dev
cmake --preset debug  && cmake --build --preset debug
cmake --preset perf   && cmake --build --preset perf
cmake --preset safe   && cmake --build --preset safe
```

Never benchmark with a sanitizer preset — ASAN/UBSAN add 3-10x
overhead and invalidate the measurement.

All presets set `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, so
`build-<preset>/compile_commands.json` is available for clangd and
clang-tidy.

## Build Process

### Standard Build

If you cannot use presets (CMake older than 3.21), configure by hand:

```sh
# Create build directory
mkdir build
cd build

# Configure
cmake ..

# Build
make -j8    # Use 8 parallel jobs

# Optionally run tests
cmake --build . --target datakit-quickcheck
```

### Debug Build

Debug builds include symbols and disable optimizations:

```sh
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

```sh
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

#### DatakitTestMode

Enables `DATAKIT_TEST` mode (default: ON). It is independent of
`BuildTestBinary` so consuming projects can keep the in-source test
symbols without also building the runner.

#### Language and float128 options

- `UseC23` (default OFF) - compile as C23, which enables `_Float128`
  where the toolchain provides it
- `UseFloat128` (default OFF) - force-enable `__float128`; may require
  libquadmath
- `ForceNoFloat128` (default OFF) - force-disable every float128 path,
  useful for exercising the fallback code

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

Sanitizers are real CMake options, not source edits. The `dev`,
`debug`, and `safe` presets already turn ASAN and UBSAN on; to control
them in a hand-configured build:

```sh
cmake -DEnableASAN=ON ..   # AddressSanitizer (memory errors)
cmake -DEnableUBSAN=ON ..  # UndefinedBehaviorSanitizer
cmake -DEnableMSAN=ON ..   # MemorySanitizer (Clang only)
cmake -DEnableTSAN=ON ..   # ThreadSanitizer
```

All four default to OFF. ASAN and TSAN are mutually exclusive — enable
TSAN on its own. MSAN requires Clang; with gcc the option is ignored
with a warning.

UBSAN is enabled as `-fsanitize=undefined -fno-sanitize=function`. The
`function` check is dropped because datakit's hot dispatch loops
(`dod.c`, `multilist.c`, `multimap.c`, `xof.c`, `multiOrderedSet.c`)
use computed gotos, which that check misreads as indirect calls and
which produce relocations the macOS linker rejects. Every other UBSAN
check stays active — including the alignment check.

`DATAKIT_UBSAN_ALLOW_UNALIGNED` (default OFF) restores the legacy
`-fno-sanitize=alignment` behavior. It masks real alignment bugs; turn
it on only if a specific consumer needs the old behavior.

Then rebuild:

```sh
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

Three library targets are built from one shared set of object files.
The static and module prefixes are cleared, so the names have no `lib`:

```
build/src/datakit.a             # datakit-static  (STATIC)
build/src/datakit.so            # datakit-shared  (MODULE; .so on Linux, .dylib on macOS)
build/src/libdatakit.so.0.3.0   # datakit-library (versioned SHARED)
```

Library versioning:
- **VERSION**: 0.3.0 (full version)
- **SOVERSION**: 0.3.0 (ABI version)

`SOVERSION` only moves on a breaking ABI change; `VERSION` moves with
ordinary feature and maintenance releases.

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

#### Installing

The build declares no `install()` rules and ships no pkg-config file,
so `make install` does nothing useful. Consume datakit either by
copying the built libraries and the `src/*.h` headers where you want
them, or by adding the repository to your own build with
`add_subdirectory()` and linking the `datakit-static` or
`datakit-library` target directly.

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

### BSD (FreeBSD, OpenBSD, NetBSD, DragonFly)

```sh
mkdir build && cd build
cmake ..
gmake -j$(sysctl -n hw.ncpu)
```

Use `gmake` (GNU make) where the base system `make` is BSD make. BSD
builds link `pthread` and `m` the same way Linux does, and pick up
`getentropy()` for random-number generation.

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
-Wempty-init-stmt              # Empty initialization
-Wextra-semi-stmt              # Extra semicolons
-Wno-gnu-label-as-value        # Allow computed gotos
```

### Sanitizer check exclusions

Only one UBSAN check is dropped, and only when UBSAN is on:

```
-fno-sanitize=function         # computed-goto dispatch loops (see Sanitizers above)
```

The alignment check used to be suppressed unconditionally; it is now
active by default, and re-suppressing it requires opting in to
`DATAKIT_UBSAN_ALLOW_UNALIGNED`.

## Troubleshooting

### CMake Configuration Fails

#### "CMake version too old"

```
CMake Error: CMake 3.10 or higher is required
```

A plain build needs 3.10; `--preset` needs 3.21.

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
    ├── datakit.a              # Static library
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
