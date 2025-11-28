# Contributing to Varint Library

Thank you for your interest in contributing! This document provides guidelines for contributing to the varint library.

## Development Workflow

### 1. Fork and Clone
```bash
git clone https://github.com/mattsta/varint.git
cd varint
```

### 2. Create a Feature Branch
```bash
git checkout -b feature/your-feature-name
```

### 3. Make Changes
- Follow existing code style
- Add tests for new features
- Update documentation as needed

### 4. Run Local Tests
```bash
# Compiler warning checks (GCC + Clang)
./scripts/build/run_all_compilers.sh

# Unit tests with sanitizers
./scripts/test/run_unit_tests.sh

# Comprehensive example testing
./scripts/test/test_all_comprehensive.sh

# All tests with specific sanitizer
./scripts/test/run_all_tests.sh asan    # or ubsan, both, none
```

### 5. Ensure Quality Gates Pass

**Required for PR Approval**:
- ✅ GCC: 25/25 files clean (no warnings with `-Wall -Wextra -Wpedantic -Wsign-conversion -Wconversion -Werror`)
- ✅ Clang: 25/25 files clean (same strict flags)
- ✅ Unit tests: 8/8 pass with ASan+UBSan
- ✅ Examples: 41/41 pass with ASan+UBSan
- ✅ No sanitizer violations (address, undefined behavior, memory leaks)

### 6. Commit Guidelines

Follow conventional commits format:
```
<type>(<scope>): <subject>

<body>

<footer>
```

**Types**:
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation changes
- `perf`: Performance improvements
- `refactor`: Code refactoring
- `test`: Test additions/changes
- `chore`: Build/tooling changes

**Example**:
```
feat(varintTagged): Add batch encoding API

Implements varintTaggedPutBatch() for encoding multiple values
with SIMD optimizations using AVX2 instructions.

Benchmark shows 3.2x speedup for arrays of 1000+ integers.

Closes #123
```

### 7. Submit Pull Request

**PR Title Format**: `[Type] Brief description`

**PR Description Must Include**:
1. **What**: Summary of changes
2. **Why**: Problem being solved or feature rationale
3. **How**: Implementation approach
4. **Testing**: Evidence of testing (paste test output)
5. **Performance**: Impact on performance (if applicable)
6. **Breaking Changes**: Any API changes (if applicable)

**Example PR Description**:
```markdown
## What
Adds SIMD-accelerated batch encoding for varintTagged

## Why
Encoding large arrays one value at a time leaves performance on the table.
Users with bulk encoding needs (e.g., database column compression) would benefit
from vectorized operations.

## How
- Implemented varintTaggedPutBatch() using AVX2 intrinsics
- Falls back to scalar loop when AVX2 unavailable
- Processes 8 values at a time using 256-bit vectors

## Testing
```
All tests pass:
✅ GCC: 25/25 clean
✅ Clang: 25/25 clean
✅ Unit tests: 8/8 pass
✅ Examples: 41/41 pass
✅ Sanitizers: 0 violations

New benchmark:
- Scalar: 100M ops/sec
- SIMD:   320M ops/sec (3.2x speedup)
```

## Performance
3.2x faster for batch encoding (arrays of 100+ values)
No regression for single-value encoding

## Breaking Changes
None - new API, existing APIs unchanged
```

## Code Style

### C Code Style
```c
// Function naming: module prefix + camelCase
varintWidth varintTaggedPut64(uint8_t *dst, uint64_t value);

// Variables: camelCase
uint64_t encodedValue;
const uint8_t *srcBuffer;

// Constants: UPPER_SNAKE_CASE
#define VARINT_MAX_WIDTH 9

// Macros: UPPER_SNAKE_CASE or prefix_mixedCase_
#define VARINT_ADD_OR_ABORT_OVERFLOW_(val, add, result)

// Indentation: 4 spaces (not tabs)
if (condition) {
    doSomething();
}

// Braces: K&R style (opening brace on same line)
void function(void) {
    // ...
}

// Line length: prefer < 80 chars, max 100 chars
```

### Documentation
```c
/**
 * Encodes a 64-bit integer using tagged varint format.
 *
 * @param dst Destination buffer (must be at least 9 bytes)
 * @param value Integer value to encode (0 to UINT64_MAX)
 * @return Number of bytes written (1-9), or 0 on error
 *
 * The tagged format uses the first byte to indicate the total
 * width, allowing direct length determination without decoding.
 * Encoded values maintain sortability in big-endian byte order.
 *
 * Example:
 *   uint8_t buf[9];
 *   varintWidth len = varintTaggedPut64(buf, 12345);
 *   assert(len == 3);  // 12345 requires 3 bytes
 */
varintWidth varintTaggedPut64(uint8_t *dst, uint64_t value);
```

## Adding New Features

### 1. New Encoding Type
If adding a new varint encoding:
- Add header: `src/varint<Name>.h`
- Add implementation: `src/varint<Name>.c` (if needed)
- Add tests: `src/varint<Name>Test.c`
- Add examples: `examples/standalone/example_<name>.c`
- Add documentation: `docs/modules/varint<Name>.md`
- Update `docs/CHOOSING_VARINTS.md` with comparison

### 2. New Feature to Existing Module
- Add functions with existing naming conventions
- Add tests to existing test file
- Add examples demonstrating usage
- Update module documentation

### 3. Performance Optimization
- Add benchmark in `src/varintCompare.c`
- Include before/after performance data
- Ensure no regression in other operations
- Consider fallback for unsupported platforms

## Testing Requirements

### Unit Tests
Every new function must have:
- ✅ Boundary value tests (min, max, 0)
- ✅ Round-trip encode/decode verification
- ✅ Error condition handling
- ✅ Buffer overflow protection

### Example Code
- ✅ Standalone example demonstrating feature
- ✅ Integration example showing real-world usage
- ✅ Example must compile and run cleanly
- ✅ Example must pass with ASan+UBSan

### Documentation
- ✅ API documentation in header
- ✅ Module guide in `docs/modules/`
- ✅ Usage examples in docs
- ✅ Performance characteristics documented

## CI/CD Pipeline

All PRs trigger automated testing:

### Continuous Integration (ci.yml)
Runs on every push/PR:
- ✅ Compiler warnings (GCC + Clang on Ubuntu + macOS)
- ✅ Unit tests with sanitizers (ASan, UBSan, both)
- ✅ 41 comprehensive examples with ASan+UBSan
- ✅ CMake build verification
- ✅ Cross-platform testing (Linux, macOS)

### Release Verification (release.yml)
Runs on version tags:
- ✅ All compiler checks must pass
- ✅ All tests must pass
- ✅ Creates release archive
- ✅ Quality gate validation

### Nightly Testing (nightly.yml)
Runs daily at 2 AM UTC:
- ✅ Extended sanitizer testing (ASan, UBSan, TSan)
- ✅ Compiler matrix (GCC 10-13, Clang 11-15)
- ✅ Performance benchmarks
- ✅ Valgrind memory analysis

**View CI Status**: Check the Actions tab on GitHub

## Performance Guidelines

### Benchmarking
```bash
# Build optimized binary
gcc -O3 -march=native -I. -o varintCompare \
    src/varintCompare.c src/*.c -lm

# Run benchmark
./varintCompare 1000
```

### Performance Expectations
- Encode: 200-500M ops/sec (typical x86_64 @ 3GHz)
- Decode: 300-600M ops/sec
- Packed operations: 500M-1B ops/sec

### Optimization Checklist
- [ ] Minimize branches in hot paths
- [ ] Use `__builtin_expect()` for unlikely branches
- [ ] Consider SIMD for batch operations
- [ ] Profile before optimizing (`perf record/report`)
- [ ] Verify no regression with `varintCompare`

## Memory Safety

### Required Practices
- ✅ All buffer accesses must be bounds-checked
- ✅ Use explicit type casts for integer conversions
- ✅ Check for integer overflow with `__builtin_*_overflow()`
- ✅ Validate all function inputs
- ✅ Zero-initialize local variables
- ✅ Use `const` for read-only pointers

### Sanitizer Testing
```bash
# Address sanitizer (buffer overflows, use-after-free)
./run_all_tests.sh asan

# Undefined behavior sanitizer (integer overflow, null deref)
./run_all_tests.sh ubsan

# Both sanitizers
./run_all_tests.sh both

# Thread sanitizer (data races)
./run_all_tests.sh tsan
```

## Platform Support

### Primary Platforms
- ✅ Linux (Ubuntu 20.04+, tested in CI)
- ✅ macOS (12+, tested in CI)

### Compiler Support
- ✅ GCC 10+ (tested in nightly CI)
- ✅ Clang 11+ (tested in nightly CI)

### Architecture Support
- ✅ x86_64 (primary target)
- 🔄 ARM64 (should work, needs CI)
- 🔄 ARMv7 (should work, needs testing)

## Getting Help

- **Questions**: Open a GitHub issue with `[Question]` prefix
- **Bugs**: Open a GitHub issue with full reproduction steps
- **Features**: Open a GitHub issue with use case description
- **Security**: Email maintainer directly (see SECURITY.md)

## Code Review Process

PRs are reviewed for:
1. **Correctness**: Does it work as intended?
2. **Testing**: Adequate test coverage?
3. **Performance**: No regressions?
4. **Style**: Follows code style?
5. **Documentation**: Properly documented?
6. **Safety**: Memory safe, no UB?

**Typical Review Timeline**: 1-3 days for initial feedback

## License

By contributing, you agree that your contributions will be licensed under the same license as the project (MIT/Public Domain dual-license).

---

**Thank you for contributing to the varint library!**
