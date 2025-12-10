# DepsManager - Universal CMake Dependency Manager

A generalized, portable system for managing CMake dependencies across projects with
support for local development (edit mode), remote fetching, and offline bundles.

## Features

- **Single configuration file** for all dependencies
- **Edit Mode** - Use local checkouts for active development
- **Remote Mode** - Fetch from git repositories automatically
- **Vendored Mode** - Use bundled sources for offline builds
- **Auto-detection** - Automatically finds local sources in standard locations
- **Per-dependency overrides** - Configure individual deps differently
- **Lock file generation** - Record exact versions for reproducibility
- **Bundle creation** - Create offline archives with all dependencies
- **CLI tool** - Manage dependencies from the command line

## Quick Start

### 1. Copy to Your Project

```bash
# Copy the deps-manager directory to your project
cp -r cmake/deps-manager your-project/cmake/
```

### 2. Initialize

```bash
cd your-project
./cmake/deps-manager/deps-manage.sh init
```

This creates:
- `deps/deps.cmake` - Dependency declarations
- `deps/.local.cmake` - Local overrides (gitignored)

### 3. Declare Dependencies

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

### 4. Use in CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
project(myproject)

# Include the dependency manager
include(cmake/deps-manager/DepsManager.cmake)

# Initialize
deps_init()

# Load dependency declarations
include(deps/deps.cmake)

# Resolve all dependencies (fetches/locates them)
deps_resolve()

# Now use the dependencies
add_executable(myapp main.c)
target_link_libraries(myapp PRIVATE varint-static nlohmann_json)
```

### 5. Build

```bash
cmake -B build
cmake --build build
```

## Configuration

### deps/deps.cmake - Main Configuration

Declare all your dependencies here. This file is committed to your repository.

```cmake
deps_add(<name>
    GIT <url>              # Git repository URL
    TAG <tag>              # Git tag, branch, or commit (default: main)
    TARGETS <t1> <t2>      # CMake targets this dependency provides
    OPTIONS <k=v> ...      # CMake options to set
    MODE <mode>            # Override mode: auto, local, remote
    LOCAL_PATH <path>      # Explicit local path
    SUBDIRECTORY <dir>     # Subdirectory containing CMakeLists.txt
    EXCLUDE_FROM_ALL       # Exclude from ALL target
)
```

### deps/.local.cmake - Local Overrides

Personal overrides for local development. This file is gitignored.

```cmake
# Point to your local checkouts
set(DEPS_VARINT_PATH "$ENV{HOME}/repos/varint")
set(DEPS_JSON_PATH "/path/to/my/json-fork")

# Or change the default mode
set(DEPS_DEFAULT_MODE "local")
```

### Environment Variables

| Variable | Description |
|----------|-------------|
| `DEPS_MODE` | Global mode: `auto`, `local`, `remote` |
| `DEPS_LOCAL_ROOT` | Root directory for local checkouts (default: `~/repos`) |
| `DEPS_<NAME>_PATH` | Per-dependency path override |

### CMake Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `DEPS_DEFAULT_MODE` | `auto` | Default mode for all dependencies |
| `DEPS_LOCAL_ROOT` | `~/repos` | Root for local checkouts |
| `DEPS_DIR` | `${CMAKE_SOURCE_DIR}/deps` | Dependencies directory |
| `DEPS_VENDOR_DIRS` | `vendor;external;third_party;deps` | Vendor directory search list |

## Mode Resolution

The dependency manager resolves each dependency using this priority:

1. **Explicit path** (`DEPS_<NAME>_PATH` variable or env)
2. **CMake override** (`FETCHCONTENT_SOURCE_DIR_<NAME>`)
3. **Local symlink/directory** (`deps/<name>/`)
4. **Default local root** (`~/repos/<name>/`)
5. **Vendor directories** (`vendor/<name>/`, `external/<name>/`, etc.)
6. **Git fetch** (using declared GIT url)

If mode is `local`, only steps 1-5 are tried.
If mode is `remote`, only step 6 is used.
If mode is `auto` (default), all steps are tried in order.

## CLI Tool

### deps-manage.sh Commands

```bash
# Initialize dependency structure
./cmake/deps-manager/deps-manage.sh init

# Show status of all dependencies
./cmake/deps-manager/deps-manage.sh status

# Set up local development for a dependency
./cmake/deps-manager/deps-manage.sh local varint ~/repos/varint

# Remove local override
./cmake/deps-manager/deps-manage.sh unlocal varint

# Clone all dependencies to local
./cmake/deps-manager/deps-manage.sh sync

# Create offline bundle
./cmake/deps-manager/deps-manage.sh bundle -o releases/ -v 1.0.0
```

### Example Workflow

```bash
# Developer A: Working on the main project
cd myproject
cmake -B build && cmake --build build  # Uses remote deps

# Developer B: Also working on varint
cd myproject
./cmake/deps-manager/deps-manage.sh local varint ~/repos/varint
cmake -B build  # Now uses local varint
# Make changes to ~/repos/varint, rebuild myproject to test

# Ready to commit - switch back to remote
./cmake/deps-manager/deps-manage.sh unlocal varint
cmake -B build  # Verify it works with remote version
```

## Directory Structure

```
myproject/
├── CMakeLists.txt
├── cmake/
│   └── deps-manager/          # Copy this entire directory
│       ├── DepsManager.cmake  # Core CMake module
│       ├── deps-manage.sh     # CLI tool
│       ├── example-deps.cmake # Example configuration
│       └── README.md          # This documentation
├── deps/
│   ├── deps.cmake             # Your dependency declarations
│   ├── .local.cmake           # Local overrides (gitignored)
│   ├── deps.lock              # Generated lock file
│   └── varint/                # Local checkout or symlink (optional)
└── src/
```

## Advanced Usage

### Lock Files

Generate a lock file with exact versions:

```cmake
# In CMakeLists.txt, after deps_resolve()
deps_lock()  # Creates deps/deps.lock
```

Or use the lock file:

```cmake
deps_init()
include(deps/deps.cmake)
if(EXISTS "${CMAKE_SOURCE_DIR}/deps/deps.lock")
    include(deps/deps.lock)  # Override with locked versions
endif()
deps_resolve()
```

### Custom Mode Logic

```cmake
# Use local if available, require remote for CI
if(DEFINED ENV{CI})
    set(DEPS_DEFAULT_MODE "remote")  # CI always uses remote
else()
    set(DEPS_DEFAULT_MODE "auto")    # Local dev: prefer local
endif()

deps_init()
```

### Inspecting Dependencies

```cmake
deps_resolve()

# Get mode for a specific dependency
deps_get_mode(varint mode)
message(STATUS "varint mode: ${mode}")

# Check if using local
deps_is_local(varint is_local)
if(is_local)
    message(STATUS "Using local varint - changes will reflect immediately")
endif()

# Get list of all dependencies
deps_list(all_deps)
foreach(dep IN LISTS all_deps)
    deps_get_source(${dep} src)
    message(STATUS "${dep} -> ${src}")
endforeach()
```

### Conditional Dependencies

```cmake
deps_add(optional_feature
    GIT https://github.com/example/feature.git
    TAG v1.0.0
    TARGETS feature_lib
)

# Only resolve if needed
if(ENABLE_OPTIONAL_FEATURE)
    deps_resolve()
else()
    # Skip resolution - deps_add just declares, doesn't fetch
endif()
```

### Multiple Projects Sharing Dependencies

Set a common local root:

```bash
# In ~/.bashrc or project-specific script
export DEPS_LOCAL_ROOT=~/workspace/shared-deps

# All projects will look for local deps in ~/workspace/shared-deps/
```

## Offline / Air-Gapped Builds

### Create Bundle

```bash
# On a machine with network access
./cmake/deps-manager/deps-manage.sh bundle -o bundle/ -v 1.0.0
# Creates: bundle/deps-bundle-1.0.0.tar.gz
```

### Use Bundle

```bash
# On offline machine
tar -xzf deps-bundle-1.0.0.tar.gz
mv deps-bundle-1.0.0/* myproject/vendor/

# Build normally - will auto-detect vendored deps
cmake -B build
```

## Troubleshooting

### Dependency not found

```
[DepsManager] Could not resolve dependency (mode=local)
```

- Check that the local path exists and contains CMakeLists.txt
- Verify `DEPS_<NAME>_PATH` is set correctly
- Try `deps-manage.sh status` to see current state

### Version mismatch

If you need a specific version but local has different:

```cmake
# Force remote even if local exists
deps_add(mylib
    GIT ...
    TAG v2.0.0
    MODE remote
)
```

### Target not found

Ensure `deps_resolve()` is called before using targets:

```cmake
deps_init()
include(deps/deps.cmake)
deps_resolve()  # Must be before target_link_libraries

add_executable(myapp main.c)
target_link_libraries(myapp PRIVATE mylib)  # Now mylib exists
```

### Changes not reflecting

For local dependencies, cmake doesn't automatically detect source changes.
Force a rebuild:

```bash
cmake --build build --target myapp --clean-first
```

Or for full reconfigure:

```bash
rm -rf build && cmake -B build
```

## License

This dependency manager is released as public domain / CC0.
Use it freely in any project.
