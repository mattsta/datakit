# Using varint as a CMake Dependency

This guide covers how to include the varint library in other CMake projects.

## Table of Contents

- [Quick Start](#quick-start)
- [Available Targets](#available-targets)
- [Developer Workflow](#developer-workflow)
- [Using the DepsManager System](#using-the-depsmanager-system)
- [Offline / Air-Gapped Deployments](#offline--air-gapped-deployments)
- [Version Pinning](#version-pinning)
- [CMake Options](#cmake-options)
- [Alternative Methods](#alternative-methods)
- [Troubleshooting](#troubleshooting)

---

## Quick Start

Add this to your project's `CMakeLists.txt`:

```cmake
include(FetchContent)

FetchContent_Declare(
    varint
    GIT_REPOSITORY https://github.com/mattsta/varint.git
    GIT_TAG        v1.0.0
)
FetchContent_MakeAvailable(varint)

target_link_libraries(myapp PRIVATE varint-static)
```

Headers are automatically available via the target's include directories.

---

## Available Targets

| Target                    | Description                                   |
| ------------------------- | --------------------------------------------- |
| `varint-static`           | Static library with core varint encodings     |
| `varint-library`          | Shared library with core varint encodings     |
| `varintDimension-static`  | Static library with dimension-aware encodings |
| `varintDimension-library` | Shared library with dimension-aware encodings |

---

## Developer Workflow

When developing both varint and projects that use it, you need changes in
varint to immediately reflect in dependent projects without committing/pushing.

### CMake's Built-in Override (Simplest)

CMake's FetchContent supports `FETCHCONTENT_SOURCE_DIR_<NAME>` to override
any dependency with a local path:

```bash
# Your project uses FetchContent normally in CMakeLists.txt

# Override at configure time to use local varint:
cmake -B build -DFETCHCONTENT_SOURCE_DIR_VARINT=~/repos/varint

# For release builds, omit the override:
cmake -B build-release
```

This requires **no changes** to your CMakeLists.txt.

### Explicit Mode Switching

For projects that frequently switch modes:

```cmake
# CMakeLists.txt
include(FetchContent)

set(VARINT_LOCAL_PATH "" CACHE PATH "Path to local varint for development")

if(VARINT_LOCAL_PATH AND EXISTS "${VARINT_LOCAL_PATH}/CMakeLists.txt")
    message(STATUS "EDIT MODE: Using local varint from ${VARINT_LOCAL_PATH}")
    FetchContent_Declare(varint SOURCE_DIR "${VARINT_LOCAL_PATH}")
else()
    message(STATUS "RELEASE MODE: Fetching varint v1.0.0")
    FetchContent_Declare(
        varint
        GIT_REPOSITORY https://github.com/mattsta/varint.git
        GIT_TAG        v1.0.0
    )
endif()

FetchContent_MakeAvailable(varint)
```

Usage:

```bash
# Edit Mode - test local changes
cmake -B build -DVARINT_LOCAL_PATH=~/repos/varint

# Release Mode - use published version
cmake -B build-release
```

---

## Using the DepsManager System

For projects with multiple dependencies, we provide a **generalized dependency
management system** in `cmake/deps-manager/`. This handles edit mode, remote
fetching, vendored sources, and offline bundles for any number of dependencies.

### Setup

```bash
# Copy to your project
cp -r cmake/deps-manager your-project/cmake/

# Initialize
cd your-project
./cmake/deps-manager/deps-manage.sh init
```

### Declare Dependencies

Edit `deps/deps.cmake`:

```cmake
deps_add(varint
    GIT https://github.com/mattsta/varint.git
    TAG v1.0.0
    TARGETS varint-static
)

deps_add(json
    GIT https://github.com/nlohmann/json.git
    TAG v3.11.3
    TARGETS nlohmann_json
    OPTIONS JSON_BuildTests=OFF
)
```

### Use in CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
project(myproject)

include(cmake/deps-manager/DepsManager.cmake)
deps_init()
include(deps/deps.cmake)
deps_resolve()

add_executable(myapp main.c)
target_link_libraries(myapp PRIVATE varint-static nlohmann_json)
```

### Switch Between Edit and Remote Modes

```bash
# Set up local development for varint
./cmake/deps-manager/deps-manage.sh local varint ~/repos/varint

# Check status of all dependencies
./cmake/deps-manager/deps-manage.sh status

# Remove local override (switch back to remote)
./cmake/deps-manager/deps-manage.sh unlocal varint
```

See `cmake/deps-manager/README.md` for complete documentation.

---

## Offline / Air-Gapped Deployments

### Option 1: Use DepsManager Bundle

```bash
# On a machine with network access
./cmake/deps-manager/deps-manage.sh bundle -o releases/ -v 1.0.0

# Transfer to offline machine
tar -xzf deps-bundle-1.0.0.tar.gz
mv deps-bundle-1.0.0/* your-project/vendor/

# Build normally - auto-detects vendored deps
cmake -B build
```

### Option 2: Vendor into Your Repository

```bash
git clone https://github.com/mattsta/varint.git
cp -r varint your-project/vendor/varint
rm -rf your-project/vendor/varint/.git
```

In your CMakeLists.txt:

```cmake
add_subdirectory(vendor/varint)
target_link_libraries(myapp PRIVATE varint-static)
```

### Option 3: URL Instead of Git

```cmake
FetchContent_Declare(
    varint
    URL https://github.com/mattsta/varint/archive/refs/tags/v1.0.0.tar.gz
    URL_HASH SHA256=<hash>
)
```

For truly offline builds, use a local file:

```cmake
FetchContent_Declare(
    varint
    URL file:///path/to/varint-1.0.0.tar.gz
)
```

---

## Version Pinning

### Release Tag (Recommended)

```cmake
FetchContent_Declare(
    varint
    GIT_REPOSITORY https://github.com/mattsta/varint.git
    GIT_TAG        v1.0.0
)
```

### Specific Commit (Most Reproducible)

```cmake
FetchContent_Declare(
    varint
    GIT_REPOSITORY https://github.com/mattsta/varint.git
    GIT_TAG        abc123def456789...  # Full 40-char SHA
)
```

### Branch (Development Only)

```cmake
FetchContent_Declare(
    varint
    GIT_REPOSITORY https://github.com/mattsta/varint.git
    GIT_TAG        main
)
```

**Warning**: Branch tracking makes builds non-reproducible.

---

## CMake Options

| Option                  | Default               | Description            |
| ----------------------- | --------------------- | ---------------------- |
| `VARINT_BUILD_TESTS`    | `OFF` (as dependency) | Build test executables |
| `VARINT_BUILD_EXAMPLES` | `OFF` (as dependency) | Build example programs |

Override before `FetchContent_MakeAvailable`:

```cmake
set(VARINT_BUILD_TESTS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(varint)
```

---

## Alternative Methods

### System Installation with find_package

```bash
cd varint
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
sudo cmake --install build
```

Use in your project:

```cmake
find_package(varint REQUIRED)
target_link_libraries(myapp PRIVATE varint::varint-static)
```

### Git Submodule

```bash
git submodule add https://github.com/mattsta/varint.git external/varint
```

```cmake
add_subdirectory(external/varint)
target_link_libraries(myapp PRIVATE varint-static)
```

---

## Troubleshooting

### "Target not found" errors

Ensure `FetchContent_MakeAvailable(varint)` is called before `target_link_libraries`.

### Header not found

Include directories are set automatically via `target_link_libraries`. Verify
you're linking against a varint target.

### Edit mode changes not reflecting

1. Ensure path points to correct directory with `CMakeLists.txt`
2. Re-run cmake configuration (changes require reconfigure, not just rebuild)

### Offline build fails

Use vendored mode or DepsManager bundle:

```bash
./cmake/deps-manager/deps-manage.sh bundle -v 1.0.0
# Transfer and extract on offline machine
```

---

## CMake Version Requirements

- **Minimum**: CMake 3.16
- **Recommended**: CMake 4.0+
