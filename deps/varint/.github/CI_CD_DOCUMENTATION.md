# CI/CD Documentation for Varint Library

## Overview

The varint library uses **GitHub Actions** for comprehensive automated testing, validation, and release management. The CI/CD system leverages all existing test automation infrastructure to ensure maximum code quality and production readiness.

---

## CI/CD Architecture

### Three-Tier Testing Strategy

```
┌─────────────────────────────────────────────────────┐
│  Tier 1: Continuous Integration (Every Push/PR)    │
│  - Compiler warnings (GCC + Clang)                  │
│  - Unit tests with sanitizers                       │
│  - 41 comprehensive examples                        │
│  - Multi-platform builds                            │
└─────────────────────────────────────────────────────┘
                         ↓
┌─────────────────────────────────────────────────────┐
│  Tier 2: Release Verification (On Tags)             │
│  - ALL quality gates must pass                      │
│  - Zero warnings on both compilers                  │
│  - Zero sanitizer violations                        │
│  - Creates release artifacts                        │
└─────────────────────────────────────────────────────┘
                         ↓
┌─────────────────────────────────────────────────────┐
│  Tier 3: Nightly Extended Testing (Daily @ 2 AM)    │
│  - Extended sanitizer matrix                        │
│  - Compiler matrix (GCC 10-13, Clang 11-15)         │
│  - Performance benchmarks                           │
│  - Valgrind memory analysis                         │
└─────────────────────────────────────────────────────┘
```

---

## Workflows

### 1. Continuous Integration (ci.yml)

**Trigger**: Every push, every pull request
**Duration**: ~10-15 minutes
**Platforms**: Ubuntu 22.04, Ubuntu 20.04, macOS 13, macOS 12

#### Jobs

##### Job 1: Compiler Warning Verification
**Purpose**: Ensure zero compiler warnings with strictest flags

**Strategy**:
```yaml
matrix:
  os: [ubuntu-22.04, ubuntu-20.04, macos-13, macos-12]
```

**Process**:
1. Install GCC + Clang
2. Run `./run_all_compilers.sh`
3. Verify GCC: 25/25 files PASS
4. Verify Clang: 25/25 files PASS
5. Upload results as artifacts

**Strict Flags Used**:
```
-Wall -Wextra -Wpedantic -Wsign-conversion -Wconversion -Werror
```

**Success Criteria**: Zero warnings, zero errors

---

##### Job 2: Unit Tests with Sanitizers
**Purpose**: Validate correctness with memory safety checks

**Strategy**:
```yaml
matrix:
  os: [ubuntu-22.04, macos-13]
  sanitizer: [none, asan, ubsan, both]
```

**Process**:
1. Run `./run_unit_tests.sh`
2. Execute 8 unit tests:
   - varintDelta, varintFOR, varintGroup, varintPFOR
   - varintDict, varintBitmap, varintAdaptive, varintFloat
3. Verify all tests pass

**Sanitizers**:
- `asan`: AddressSanitizer (buffer overflows, use-after-free)
- `ubsan`: UndefinedBehaviorSanitizer (integer overflow, null deref)
- `both`: Both sanitizers combined

**Success Criteria**: 8/8 tests PASS, 0 sanitizer violations

---

##### Job 3: Comprehensive Example Testing
**Purpose**: Validate all 41 examples with sanitizers

**Strategy**:
```yaml
matrix:
  os: [ubuntu-22.04, macos-13]
```

**Process**:
1. Run `./test_all_comprehensive.sh`
2. Execute all examples with ASan+UBSan:
   - 14 Standalone examples
   - 9 Integration examples
   - 14 Advanced examples (including trie server/client)
   - 3 Reference examples
   - 1 Interactive test
3. Verify all pass

**Timeout**: 15 minutes

**Success Criteria**: 41/41 examples PASS, 0 sanitizer violations

---

##### Job 4: Build System Verification
**Purpose**: Ensure CMake builds work on all platforms

**Strategy**:
```yaml
matrix:
  os: [ubuntu-22.04, macos-13]
  compiler: [gcc, clang]
```

**Process**:
1. CMake configure with Release build
2. CMake build with parallelization
3. Verify build artifacts created

**Success Criteria**: Clean build, no errors

---

##### Job 5: Sanitizer Matrix Testing
**Purpose**: Test with multiple sanitizer types

**Strategy**:
```yaml
matrix:
  sanitizer: [address, undefined, thread]
```

**Process**:
1. Run `./run_all_tests.sh <sanitizer>`
2. Check for violations
3. Upload results

**Note**: MemorySanitizer excluded (requires instrumented libraries)

---

##### Job 6: Cross-Architecture Testing
**Purpose**: Validate on different architectures

**Current**: x86_64 (amd64)
**Future**: ARM64, ARMv7 (when cross-compilation configured)

---

##### Job 7: Static Analysis
**Purpose**: Additional code quality checks

**Tools**: cppcheck (when available)

**Process**: Static analysis on all source files

---

##### Job 8: Summary Report
**Purpose**: Aggregate results and generate summary

**Process**:
1. Collect results from all jobs
2. Generate GitHub Step Summary
3. Mark workflow as pass/fail

**Display**: Visible in GitHub Actions UI and PR checks

---

### 2. Release Verification (release.yml)

**Trigger**: Git tags matching `v*` or release creation
**Duration**: ~20 minutes
**Purpose**: Final validation before release

#### Quality Gate

**ALL checks must pass for release approval**:

```
✅ GCC:   25/25 files clean (strict warnings)
✅ Clang: 25/25 files clean (strict warnings)
✅ Unit Tests: 8/8 passed with sanitizers
✅ Examples: 41/41 passed with ASan+UBSan
✅ Build: Clean on all platforms
```

#### Process

1. **Compiler Checks** (BLOCKING)
   - Run `./run_all_compilers.sh`
   - FAIL if any warnings found

2. **Unit Tests** (BLOCKING)
   - Run `./run_unit_tests.sh`
   - FAIL if any test fails

3. **Comprehensive Examples** (BLOCKING)
   - Run `./test_all_comprehensive.sh`
   - FAIL if any example fails
   - Timeout: 20 minutes

4. **Create Release Archive**
   - Package source files
   - Include docs, examples, build system
   - Create `varint-<version>.tar.gz`

5. **Quality Gate Summary**
   - Generate release report
   - Upload artifacts

**Success Criteria**: ALL gates pass, release ready

---

### 3. Nightly Extended Testing (nightly.yml)

**Trigger**:
- Scheduled: 2 AM UTC daily
- Manual: workflow_dispatch

**Duration**: ~1-2 hours
**Purpose**: Deep testing, performance tracking

#### Jobs

##### Extended Sanitizer Testing
**Matrix**: asan, ubsan, both, tsan

**Process**:
1. Run unit tests with sanitizer
2. Run comprehensive examples with sanitizer
3. Run all tests with sanitizer
4. Check for violations
5. Upload results (7-day retention)

##### Compiler Matrix Testing
**Matrix**: GCC 10-13, Clang 11-15

**Purpose**: Ensure compatibility with multiple compiler versions

**Process**:
1. Install specific compiler version
2. Run `./run_all_compilers.sh` with that compiler
3. Upload results per compiler

**Note**: Some combinations may not be available on all platforms

##### Performance Benchmarks

**Process**:
1. Build optimized binary (`-O3`)
2. Run `./varintCompare_perf 100`
3. Collect performance metrics
4. Upload results (30-day retention)

**Metrics Tracked**:
- Encode/decode cycles
- Throughput (ops/sec)
- Compression ratios

##### Valgrind Memory Analysis

**Process**:
1. Build without sanitizers
2. Run tests under Valgrind
3. Check for memory leaks
4. Upload analysis (7-day retention)

**Checks**:
- Memory leaks
- Invalid reads/writes
- Uninitialized memory usage

---

## Existing Test Infrastructure

All CI/CD workflows leverage existing automation:

### 1. run_all_compilers.sh
**Purpose**: GCC + Clang strict warning verification

**Process**:
```bash
./run_all_compilers.sh
```

**Output**:
- `compiler_check_results/gcc_results.txt`
- `compiler_check_results/clang_results.txt`
- `compiler_check_results/summary.txt`

**Success**: 25/25 files on both compilers

---

### 2. run_unit_tests.sh
**Purpose**: Execute 8 unit tests with sanitizers

**Process**:
```bash
./run_unit_tests.sh
```

**Tests**:
- varintDelta (delta encoding)
- varintFOR (Frame of Reference)
- varintGroup (grouped varints)
- varintPFOR (Patched FOR)
- varintDict (dictionary encoding)
- varintBitmap (Roaring bitmaps)
- varintAdaptive (auto-selection)
- varintFloat (float compression)

**Output**: Test summary with pass/fail status

---

### 3. test_all_comprehensive.sh
**Purpose**: Execute ALL 41 examples with ASan+UBSan

**Process**:
```bash
./test_all_comprehensive.sh
```

**Categories**:
1. **Standalone** (14 examples): Basic encoding demonstrations
2. **Integration** (9 examples): Real-world use cases
3. **Advanced** (14 examples): Complex systems (trie server, etc.)
4. **Reference** (3 examples): Reference implementations
5. **Interactive** (1 example): Interactive testing

**Sanitizers**: `-fsanitize=address,undefined`

**Output**: Detailed pass/fail for each example

---

### 4. run_all_tests.sh
**Purpose**: Run tests with specific sanitizer configuration

**Usage**:
```bash
./run_all_tests.sh none   # No sanitizers
./run_all_tests.sh asan   # AddressSanitizer
./run_all_tests.sh ubsan  # UndefinedBehaviorSanitizer
./run_all_tests.sh both   # Both sanitizers
```

**Process**:
- Builds all tests with specified sanitizer flags
- Executes test suite
- Reports results

---

## GitHub Actions Integration

### Viewing CI Status

**PR Checks**:
- All CI jobs appear as checks on PRs
- Must pass before merge (if branch protection enabled)
- Click "Details" to see full logs

**Actions Tab**:
- All workflow runs visible at `/actions`
- Filter by workflow, branch, status
- Download artifacts from successful runs

**Badges**:
README.md displays real-time status:
```markdown
![CI Status](https://github.com/mattsta/varint/actions/workflows/ci.yml/badge.svg)
```

---

### Artifacts

**Uploaded Artifacts** (available in Actions UI):

1. **Compiler Results** (30-day retention)
   - GCC results per platform
   - Clang results per platform
   - Summary reports

2. **Test Outputs** (30-day retention)
   - Comprehensive test logs
   - Sanitizer output
   - Performance benchmarks

3. **Release Archives** (90-day retention)
   - Source tarball
   - Documentation bundle

4. **Nightly Results** (7-day retention)
   - Extended test logs
   - Compiler matrix results
   - Valgrind analysis

---

## Quality Gates

### PR Merge Requirements

**Automated Checks** (enforced by CI):
```
✅ Compiler Warnings:  GCC + Clang must be 25/25 clean
✅ Unit Tests:         8/8 must pass
✅ Examples:           41/41 must pass
✅ Sanitizers:         0 violations allowed
✅ Build:              Must compile on all platforms
```

**Manual Review** (human reviewer):
- Code quality
- Documentation updates
- Test coverage
- Performance impact

---

### Release Quality Gates

**Blocking Criteria** (MUST pass):
```
1. ✅ All CI checks pass
2. ✅ All nightly tests pass (last run)
3. ✅ No open high-priority bugs
4. ✅ Documentation updated
5. ✅ CHANGELOG.md updated
6. ✅ Version number incremented
```

**Release Checklist**:
- [ ] All tests passing
- [ ] Performance benchmarks acceptable
- [ ] Documentation up to date
- [ ] Migration guide (if API changes)
- [ ] Git tag created (`v*.*.*`)
- [ ] Release notes written

---

## Troubleshooting CI Failures

### Compiler Warnings Failure

**Symptom**: "Compiler checks FAILED"

**Debug**:
1. Download `compiler-results-<os>` artifact
2. Check `gcc_results.txt` and `clang_results.txt`
3. Identify failing file and warning
4. Run locally: `./run_all_compilers.sh`
5. Fix warning with explicit casts or code change

**Common Fixes**:
- Add explicit type casts: `(uint8_t)`, `(uint64_t)`
- Fix sign conversions: `(int32_t)`, `(uint32_t)`
- Check macro expansions for type safety

---

### Unit Test Failure

**Symptom**: "Unit tests FAILED"

**Debug**:
1. Check which test failed in logs
2. Run locally: `./run_unit_tests.sh`
3. Run specific test with sanitizers:
   ```bash
   gcc -fsanitize=address,undefined -I. -o test \
       src/<TestName>.c src/*.c -lm
   ./test
   ```
4. Fix identified issue

---

### Example Test Failure

**Symptom**: "Examples FAILED"

**Debug**:
1. Download `comprehensive-test-output-<os>` artifact
2. Find failing example in log
3. Run locally: `./test_all_comprehensive.sh`
4. Run specific example with sanitizers
5. Check for:
   - Compilation errors
   - Runtime crashes
   - Sanitizer violations
   - Timeouts

---

### Sanitizer Violation

**Symptom**: "Sanitizer violations detected"

**Types**:

**AddressSanitizer**:
- Buffer overflows
- Use-after-free
- Stack/heap buffer overflow
- Global buffer overflow

**UndefinedBehaviorSanitizer**:
- Integer overflow
- Division by zero
- Null pointer dereference
- Invalid type casts

**ThreadSanitizer**:
- Data races
- Lock order violations

**Fix Process**:
1. Read sanitizer output carefully
2. Locate source file and line number
3. Understand root cause
4. Apply fix (bounds check, explicit cast, etc.)
5. Verify with local sanitizer run

---

## Performance Monitoring

### Benchmark Tracking

**Current**: Manual comparison of `varintCompare` output
**Future**: Automated performance regression detection

**Metrics Tracked**:
- Encode cycles/operation
- Decode cycles/operation
- Throughput (ops/second)
- Memory usage
- Cache efficiency

**Baseline Storage**: Nightly artifacts (30-day retention)

**Regression Detection**:
- Compare current run to baseline
- Alert if >5% performance degradation
- Generate performance trend reports

---

## Local Testing

### Before Pushing

**Quick Check** (~2 minutes):
```bash
./run_all_compilers.sh
```

**Comprehensive** (~5 minutes):
```bash
./run_all_compilers.sh
./run_unit_tests.sh
./test_all_comprehensive.sh
```

**Full CI Simulation** (~15 minutes):
```bash
# Run all combinations
for sanitizer in none asan ubsan both; do
  ./run_all_tests.sh $sanitizer
done

./test_all_comprehensive.sh
```

---

## CI/CD Maintenance

### Updating Workflows

**File Locations**:
- `.github/workflows/ci.yml` - Main CI
- `.github/workflows/release.yml` - Release validation
- `.github/workflows/nightly.yml` - Extended testing

**Testing Workflow Changes**:
1. Create test branch
2. Modify workflow
3. Push to trigger CI
4. Verify in Actions tab
5. Merge when working

### Adding New Tests

**Process**:
1. Add test script to root (e.g., `new_test.sh`)
2. Make executable: `chmod +x new_test.sh`
3. Add to appropriate workflow job
4. Test locally first
5. Commit and push

**Example**:
```yaml
- name: Run new test
  run: |
    chmod +x new_test.sh
    ./new_test.sh
```

---

## Future Enhancements

### Planned Improvements

1. **Coverage Tracking**
   - Integrate gcov/lcov
   - Generate coverage reports
   - Track coverage trends

2. **Fuzzing Integration**
   - Add OSS-Fuzz integration
   - Continuous fuzzing
   - Crash corpus management

3. **Performance Regression**
   - Automated baseline comparison
   - Performance trend graphs
   - Alert on regressions

4. **Cross-Platform Expansion**
   - Windows MSVC testing
   - ARM64 builds
   - Big-endian testing

5. **Docker Integration**
   - Containerized builds
   - Reproducible environments
   - Multi-arch builds

---

## Contact & Support

**CI/CD Issues**: Open GitHub issue with `[CI]` prefix
**Workflow Questions**: See `.github/CONTRIBUTING.md`
**Failures**: Check Actions logs first, then open issue

---

**Last Updated**: 2025-11-19
**CI/CD Version**: 1.0.0
