# Scripts Directory

This directory contains all automation scripts for building, testing, and validating the varint library.

## Directory Structure

```
scripts/
├── build/          # Build and compiler checking scripts
│   ├── check_warnings.sh       # Single compiler warning check
│   └── run_all_compilers.sh    # Multi-compiler verification (GCC + Clang)
└── test/           # Test execution scripts
    ├── run_all_tests.sh                # Unit tests with optional sanitizers
    ├── run_unit_tests.sh               # Unit tests with ASan+UBSan
    ├── test_all_comprehensive.sh       # All examples with sanitizers
    ├── test_all_examples_sanitizers.sh # All examples comprehensive
    ├── test_trie_comprehensive.sh      # Trie system full test suite
    ├── test_trie_fast.sh               # Trie fast tests (no valgrind)
    ├── test_trie_memory_safety.sh      # Trie with valgrind
    ├── test_trie_sanitizers.sh         # Trie with ASan+UBSan
    └── test_trie_server.sh             # Trie server basic tests
```

## Build Scripts

### `scripts/build/run_all_compilers.sh`

Comprehensive compiler warning verification with both GCC and Clang.

**Usage:**

```bash
./scripts/build/run_all_compilers.sh
```

**What it does:**

- Runs strict warning checks on all source files
- Tests with both GCC and Clang compilers
- Saves results to `compiler_check_results/`
- Reports summary of files passing/failing

**Exit codes:**

- `0` - All files pass on both compilers
- Non-zero - One or more files have warnings

### `scripts/build/check_warnings.sh`

Single compiler warning checker (used by `run_all_compilers.sh`).

**Usage:**

```bash
./scripts/build/check_warnings.sh [gcc|clang]
```

**Flags used:**

- **Clang:** `-O2 -Wall -Wextra -Wpedantic -Wsign-conversion -Wconversion -Werror`
- **GCC:** `-O2 -Wall -Wextra -Werror`

## Test Scripts

### Unit Tests

#### `scripts/test/run_all_tests.sh`

Runs all 8 unit tests with configurable sanitizers.

**Usage:**

```bash
./scripts/test/run_all_tests.sh [none|asan|ubsan|both]
```

**Examples:**

```bash
./scripts/test/run_all_tests.sh            # No sanitizers (fast)
./scripts/test/run_all_tests.sh asan       # AddressSanitizer only
./scripts/test/run_all_tests.sh ubsan      # UndefinedBehaviorSanitizer only
./scripts/test/run_all_tests.sh both       # Both sanitizers (recommended)
```

**Tests:**

- varintDeltaTest
- varintFORTest
- varintPFORTest
- varintFloatTest
- varintBitmapTest
- varintDictTest
- varintAdaptiveTest
- varintGroupTest

#### `scripts/test/run_unit_tests.sh`

Quick unit test runner with ASan+UBSan enabled by default.

**Usage:**

```bash
./scripts/test/run_unit_tests.sh
```

### Example Tests

#### `scripts/test/test_all_comprehensive.sh`

Comprehensive sanitizer testing for all 43 examples.

**Usage:**

```bash
./scripts/test/test_all_comprehensive.sh
```

**What it tests:**

- 16 standalone examples
- 9 integration examples
- 15 advanced examples
- 3 reference examples
- All with AddressSanitizer + UndefinedBehaviorSanitizer

**Duration:** 2-5 minutes depending on system

#### `scripts/test/test_all_examples_sanitizers.sh`

Alternative comprehensive example testing using CMake build system.

**Usage:**

```bash
./scripts/test/test_all_examples_sanitizers.sh
```

### Trie Server/Client Tests

These scripts test the advanced trie server/client example system.

#### `scripts/test/test_trie_comprehensive.sh`

Full trie system test suite with persistence and valgrind.

**Usage:**

```bash
./scripts/test/test_trie_comprehensive.sh
```

**What it tests:**

- All 10 trie commands (PING, INSERT, SEARCH, etc.)
- Persistence (save/load)
- Memory safety with valgrind
- Repeated automation tests

**Duration:** 1-3 minutes

#### `scripts/test/test_trie_fast.sh`

Fast trie tests without valgrind (for quick validation).

**Usage:**

```bash
./scripts/test/test_trie_fast.sh
```

**Duration:** 10-30 seconds

#### `scripts/test/test_trie_memory_safety.sh`

Focused memory safety testing with valgrind.

**Usage:**

```bash
./scripts/test/test_trie_memory_safety.sh
```

**Valgrind options:**

- Full leak checking
- Track origins
- Show all leak kinds
- Error exit code on failure

#### `scripts/test/test_trie_sanitizers.sh`

Modern sanitizer testing (ASan + UBSan) for trie system.

**Usage:**

```bash
./scripts/test/test_trie_sanitizers.sh
```

**Faster than valgrind, catches:**

- Buffer overflows
- Use-after-free
- Memory leaks
- Undefined behavior
- Integer overflows

#### `scripts/test/test_trie_server.sh`

Basic trie server functionality tests.

**Usage:**

```bash
./scripts/test/test_trie_server.sh
```

## Quick Reference

**Pre-commit checks:**

```bash
./scripts/build/run_all_compilers.sh    # Compiler warnings
./scripts/test/run_all_tests.sh both    # Unit tests
./scripts/test/test_all_comprehensive.sh # All examples
```

**Fast validation:**

```bash
make test                                # CMake unit tests
./scripts/test/run_unit_tests.sh        # Quick unit tests
./scripts/test/test_trie_fast.sh        # Quick trie tests
```

**Memory safety deep dive:**

```bash
./scripts/test/test_trie_memory_safety.sh    # Valgrind
./scripts/test/test_trie_sanitizers.sh       # ASan+UBSan
./scripts/test/test_all_comprehensive.sh     # All examples
```

## CMake Integration

These scripts are also integrated into the CMake build system:

```bash
make test              # Runs all unit tests via CTest
make test-comprehensive # Runs test_all_comprehensive.sh
make check-warnings    # Runs run_all_compilers.sh
```

## CI/CD Integration

All scripts are used in GitHub Actions workflows:

- **ci.yml** - Runs on every push/PR
  - Compiler warnings (both GCC and Clang)
  - Unit tests with sanitizers
  - All examples with sanitizers

- **release.yml** - Runs on version tags
  - Full quality gate validation
  - All tests must pass

- **nightly.yml** - Runs daily at 2 AM UTC
  - Extended sanitizer testing
  - Compiler matrix testing
  - Performance benchmarks

## Output Artifacts

Scripts create the following artifacts (all gitignored):

- `build/` - CMake build artifacts
- `build_tests/` - Unit test build artifacts
- `build_examples_sanitizers/` - Example builds with sanitizers
- `build_sanitizers/` - Trie server sanitizer builds
- `compiler_check_results/` - Compiler check logs
  - `gcc_results.txt`
  - `clang_results.txt`

## Exit Codes

All scripts follow standard exit code conventions:

- `0` - Success, all tests/checks passed
- `1` - Failure, one or more tests/checks failed
- `>1` - Error in script execution

## Notes

- All scripts automatically navigate to repository root
- Scripts can be run from any directory
- Build artifacts are created in repository root
- All scripts support running from scripts/ subdirectories
