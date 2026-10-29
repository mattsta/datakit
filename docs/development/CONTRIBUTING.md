# Contributing Guide

This guide covers the development workflow, coding standards, and best practices for contributing to datakit.

## Table of Contents

- [License Considerations](#license-considerations)
- [Development Workflow](#development-workflow)
- [Code Organization](#code-organization)
- [Coding Standards](#coding-standards)
- [Testing Requirements](#testing-requirements)
- [Documentation](#documentation)
- [Submitting Changes](#submitting-changes)

## License Considerations

**IMPORTANT**: Datakit is licensed under the Sovereign Individual's
Covenant License, v1.0 ([LICENSE](../../LICENSE)):

- Individuals may use, copy, modify and learn from it for their own
  individual projects, free of charge.
- No rights are granted to corporations or any other commercial entity,
  their employees acting for them, or AI training. Any such use needs a
  separate written commercial license from the author.

**Before contributing:**

- Understand that this is **not an open source project**
- Commercial use requires a separate license
- Contact the copyright holder before making contributions
- Contributions may require a contributor agreement
- Check with Matt Stancliff (matt@genges.com) for contribution terms

This guide assumes you have permission to work on the codebase.

## Development Workflow

### Setting Up Development Environment

1. **Clone the repository:**

```bash
git clone <repository-url>
cd datakit
```

2. **Create development build:**

```sh
cmake --preset dev && cmake --build --preset dev
```

Use `--preset debug` when you need `-O0` and full symbols, and
`--preset perf` for any timing measurement.

3. **Verify tests pass:**

```sh
ctest --test-dir build/dev -j8 --output-on-failure
```

### Branch Strategy

Use feature branches for development:

```bash
# Create feature branch
git checkout -b feature/my-new-feature

# Or for bug fixes
git checkout -b fix/issue-description
```

### Development Cycle

1. **Make changes** to source files
2. **Rebuild** incrementally:
   ```sh
   cmake --build --preset dev
   ```
3. **Run tests** for affected modules — by name, not the whole suite:
   ```sh
   ./build/dev/src/datakit-test test <module>
   ctest --test-dir build/dev -j8 -R '^(<module>|<other>)$'
   ```
4. **Check for warnings**:
   ```sh
   cmake --build --preset dev 2>&1 | grep -i warning
   ```
5. **Read the sanitizer output** — the `dev` preset runs ASAN and
   UBSAN, so leaks and UB surface in the test run itself. Reach for
   valgrind when you need its specific reports:
   ```sh
   valgrind --leak-check=full ./build/debug/src/datakit-test test <module>
   ```
6. **Commit changes** when tests pass

### Pre-commit Checklist

Before committing:

- [ ] Code compiles without warnings
- [ ] Tests for everything you changed pass, by name
- [ ] New tests added for new functionality
- [ ] `ctest -R allocOwnership` prints `100% clean pattern`
- [ ] Valgrind reports no memory leaks
- [ ] Code follows style guidelines
- [ ] Comments explain non-obvious logic
- [ ] Test code included in relevant files

## Code Organization

### Directory Structure

```
datakit/
├── src/                    # Source files
│   ├── *.c                # Implementation files
│   ├── *.h                # Header files
│   ├── datakit-test.c     # Test dispatcher
│   ├── ctest.h            # Test framework
│   └── CMakeLists.txt     # Build configuration
├── deps/                   # Dependencies
│   ├── lz4/
│   ├── xxHash/
│   ├── sha1/
│   ├── varint/
│   └── imath/
├── docs/                   # Documentation
│   └── development/       # Development guides
├── CMakeLists.txt         # Root build configuration
└── README.md              # Project overview
```

### File Organization

Each data structure typically has:

- **Header file** (`module.h`):
  - API declarations
  - Type definitions
  - Inline functions
  - Test function declarations (under `#ifdef DATAKIT_TEST`)

- **Implementation file** (`module.c`):
  - Function implementations
  - Internal helpers
  - Test code at the end (under `#ifdef DATAKIT_TEST`)

### Adding a New Data Structure

1. **Create header file** (`src/newmodule.h`):

```c
#pragma once

#include "datakit.h"
__BEGIN_DECLS

typedef struct newModule {
    // Structure definition
} newModule;

// API functions
newModule *newModuleNew(void);
void newModuleFree(newModule *nm);
// ... more functions ...

#ifdef DATAKIT_TEST
int newModuleTest(int argc, char *argv[]);
#endif

__END_DECLS
```

2. **Create implementation file** (`src/newmodule.c`):

```c
#include "newmodule.h"

newModule *newModuleNew(void) {
    /* @own: caller; free=newModuleFree */
    newModule *nm = zmalloc(sizeof(*nm));
    // Initialize
    return nm;
}

void newModuleFree(newModule *nm) {
    if (!nm) return;
    // Clean up
    zfree(nm);
}

// ... more implementations ...

#ifdef DATAKIT_TEST
#include "ctest.h"

int newModuleTest(int argc, char *argv[]) {
    int err = 0;

    TEST("Basic operations");
    {
        newModule *nm = newModuleNew();
        // Test code
        newModuleFree(nm);
    }

    TEST_FINAL_RESULT;
}
#endif
```

3. **Add the source to the build** (`src/CMakeLists.txt`) — only the
   `.c` file; the test list is generated, not hand-written:

```cmake
add_library(${PROJECT_NAME} OBJECT
    # ... existing files ...
    newmodule.c
)
```

4. **Register the test** (`src/datakit-test.c`) — one include plus one
   row in `testRegistry[]`. `test ALL` walks that table and
   `src/CMakeLists.txt` parses it to emit CTest targets, so there is
   nothing else to update:

```c
#include "newmodule.h"

/* In testRegistry[]. T() passes argc/argv straight through;
 * T_ADJ() passes argc - 2 / argv + 2; the _A variants add
 * comma-separated aliases. The macro derives the function name by
 * appending "Test", so T(newModule) calls newModuleTest(). */
T(newModule),
T_A(newModule, "nm,newmod"),
```

## Coding Standards

### C Language Standard

- **Standard**: C23 (`-std=gnu23`) for every datakit file; the vendored
  libraries in `deps/` keep their own standards
- **Some files**: pinned to strict `-std=c23` for consistent
  floating-point behavior (`str.c`, `bigmath-libbf.c`)
- **Extensions**: GNU extensions allowed (computed gotos, statement
  expressions, `__builtin_*`)
- **Types**: `<stdint.h>` fixed-width types only. No bare `long` or
  `long long`; print with `<inttypes.h>` macros (`PRIu64`, `PRIx64`),
  never `%llu` / `%llx`.

### Naming Conventions

**Types:**

```c
typedef struct flexEntry flexEntry;    // Lower camelCase
typedef enum flexEndpoint flexEndpoint;
```

**Functions:**

```c
flex *flexNew(void);                   // moduleName + Action
void flexPushBytes(flex **ff, ...);    // moduleName + Verb + Object
int64_t flexAddSigned(const flex *f);  // moduleName + Verb + Type
```

**Constants:**

```c
#define FLEX_ENDPOINT_TAIL -1          // SCREAMING_SNAKE_CASE
#define FLEX_EMPTY_SIZE 2
```

**Macros:**

```c
#define flexDeleteHead(ff) ...         // Function-like: camelCase
#define DK_FN_PURE ...                 // Attribute: DK_ prefix
```

**Variables:**

```c
flex *f;                               // Short, descriptive
int count;
size_t totalBytes;
```

### Code Style

**Indentation:**

- 4 spaces (no tabs except in Makefiles)
- Configure editor: `sw=4 ts=4 expandtab`

**Braces:**

```c
// Function braces on new line (K&R style for functions)
void functionName(void)
{
    // code
}

// Control flow braces on same line
if (condition) {
    // code
} else {
    // code
}

// Single-line allowed for simple statements
if (condition) return;
```

**Spacing:**

```c
// Space after keywords
if (condition)
while (condition)
for (i = 0; i < n; i++)

// No space for function calls
functionCall(arg1, arg2)

// Space around operators
a = b + c;
x = (y * 2) + 1;

// Pointer declarations
flex *f;           // Preferred: asterisk with type
flex* f;           // Also acceptable
flex * f;          // Avoid
```

**Line Length:**

- Prefer lines under 80 characters
- Maximum 100 characters for readability
- Break long lines at logical points

**Function Length:**

- Keep functions focused and manageable
- Split complex functions into helpers
- Aim for < 100 lines per function

### Memory Management

**Allocators.** Only the datakit allocators (`zmalloc`/`zcalloc`/
`zrealloc`/`zfree`, `zmemalign` for aligned storage). Never libc
`malloc`/`free` — they are deprecation-poisoned in `datakit.h`.

**Ownership discipline (mechanically enforced).** Every allocation
site AND every container-constructor call (`flexNew`, `multimapNew`,
…) must be exactly one of:

1. **SCOPED** — declared with the `cleaner.h` scope-exit attribute so
   it is freed on every exit path, including early error returns:

```c
#include "cleaner.h"
defcleaner(flex *, flexFree);            // once per file; the trailing
defcleanerName(uint8_t *, zfree, Bytes); // ';' is REQUIRED

cleaner(flexFree) flex *f = flexNew();
cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(n);
// no explicit frees — every return path releases both
```

Ownership handoff on a success path uses the disarm idiom (the
cleanup skips NULL):

```c
cleaner(multimapFree) multimap *m = multimapNew(2);
// ... construction steps that may bail with a bare `return NULL` ...
multimap *out = m;
m = NULL; // disarmed: caller owns it now
return out;
```

2. **ANNOTATED** — the allocation intentionally outlives the
   function, marked with a structured comment trailing on the
   allocation line or alone on the line directly above:

```c
/* @own: caller; free=multimapFree */
/* @own: container; free=flexFree */
/* @own: static */
```

Grammar: `@own: (caller|container|static|cache)(; free=IDENT)?`.
`caller` = returned or written through an out-param, the caller
releases it; `container` = adopted into a struct, that struct's
Free releases it; `static` = deliberate process-lifetime;
`cache` = owned by a pool that manages reuse and release.

**The gate.** `ctest -R allocOwnership` (or
`cd tools && uv run dktools allocaudit check --constructors`) must
print `100% clean pattern`; it runs automatically with plain `ctest`.
New containers are registered in the `CONSTRUCTORS` table in
`tools/src/dktools/allocaudit.py` — one line mapping the constructor
to its releasing function.

**Idioms for the hard cases:**

- A `goto` must never jump over a cleaner declaration (clang rejects
  the jump). Hoist the declaration NULL-initialized above the first
  `goto` and assign at the original site.
- Arrays of owned objects: elements cannot carry the attribute, so
  use a holder struct whose cleanup frees each element and then the
  array (see `roarArray` in `multiroar.c`).
- Secret-holding buffers that must be wiped before free: a holder
  carrying `{ptr, len}` whose cleanup runs the secure wipe then
  `zfree` (see `hkrSecretBuf` in `hmacKeyring.c`).
- Paired teardown with ordering (a multidict and its class): the
  class must outlive `multidictFree` because teardown runs class
  callbacks — `cls = multidictGetClass(d); multidictFree(d);
multidictDefaultClassFree(cls);`.
- Loop-body braces are the per-iteration scope; a cleaner declared
  inside the loop body fires every iteration.
- Never extend a large buffer's lifetime past its original free
  point — wrap it in a nested braced scope to keep the early
  release.

**Defensive freeing** stays the rule for every `XFree`:

```c
void moduleFree(module *m) {
    if (!m) return;  // Safe to call with NULL
    // free members, then the container
    zfree(m);
}
```

### Handles: Can a Call Move It?

A handle is a type callers hold only by pointer: an opaque struct
(`typedef struct multimap multimap;`) or a byte-buffer typedef
(`typedef uint8_t flex;`). Some handles live at an address that can
change during an ordinary call (the object grows, shrinks or changes
tier). Each handle's typedef says which kind it is, in a comment on its
line or directly above it:

```c
/* @handle: moves: a call that can move it takes its address ('**') */
typedef struct multimap multimap;

/* @handle: returns: a call that can move it returns the new address */
typedef char mds;

/* @handle: fixed: its address holds from creation to free */
typedef struct multilru multilru;
```

- `moves`: every call that can relocate it takes `T **` and writes the
  new address back (`multimapInsert(&m, ...)`); a `T *` call never moves
  it. Hold one owner variable and pass its address, and never keep a
  second copy of the pointer across a moving call. flex, mflex,
  multimap, multilist, multiOrderedSet, multiarray, intset, hyperloglog,
  the multiFenwick family and the typed 2D Fenwick trees are `moves`.
- `returns`: calls that can relocate it return the new address, and the
  caller stores it (`s = mdscatlen(s, ...)`). The `mds`/`mdsc` strings.
- `fixed`: the address holds from creation to free; no call takes
  `T **` except to hand out a new handle (a parameter named `out`,
  `*Out` or `*Ptr`). An array of handles that is only read is
  `T *const *`, not `T **`.

**The gate.** `dktools headers` (the `headers` line of `verify` and
`validate`) fails on an untagged handle in a public header and on a tag
its calls contradict: `moves` with no `T **` call, `returns` with no call
returning one, `fixed` with a `T **` parameter. `dktools handles --list`
prints every handle with its tag and its `T **` calls.

### Public Headers Show Data, Not Internals

A public header shows a struct's fields only when callers fill or read
them: configurations, results, statistics, iterators callers declare.
A handle's fields (anything a public call returns by pointer, such as
`multimapP *multimapNewP(...)`) and structs no public call names are the
module's own: they live in its `.c`, or in a `<module>Internal.h` its
family's files include when several need them (`multimapSmallInternal.h`,
`intsetFullInternal.h`), behind `typedef struct T T;` in the public
header. Test support goes in test-named headers (`dkTest*.h`,
`*TestVectors.h`, `*TestCommon.h`), which are not API.

**The gate.** `dktools headers` fails on a handle's or unnamed struct's
body in a public header unless `tools/structs-allow.txt` names it with
a reason (a call table callers implement, caller-allocated state such
as `ulidGen`). `dktools structs` lists them.

### Error Handling

**Return values:**

```c
// Use bool for success/failure
bool moduleInsert(module *m, key k, value v);

// Use NULL for allocation failures
module *moduleNew(void) {
    module *m = zmalloc(sizeof(*m));
    if (!m) return NULL;
    return m;
}

// Use negative for errors, >= 0 for success
int moduleOperation(module *m) {
    if (!m) return -1;
    // ... do work ...
    return 0;  // Success
}
```

**Assertions:**

```c
// Use asserts for invariants
assert(sizeof(databox) == 16);
assert(count >= 0);

// Use asserts liberally in test code
assert(flexCount(f) == expectedCount);
```

### Comments

**Header comments:**

```c
/* Brief description of what this function does.
 *
 * More detailed explanation if needed.
 * Explain parameters, return values, side effects.
 */
void complexFunction(void);
```

**Inline comments:**

```c
// Explain non-obvious logic
count++; // Include the terminating entry

/* Multi-line explanation
 * of complex algorithm or
 * important behavior */
```

**No pending-work markers:** comments state what the code is and why. Fix
a gap before committing, or keep it in your own notes; the `spell` gate
refuses `TODO`, `FIXME`, `XXX` and `HACK` markers in sources and docs.

```c
/* NOTE: This must be called before moduleInit() */
```

**Documentation:**

- Explain **why**, not what (code shows what)
- Document assumptions and invariants
- Explain algorithmic choices
- Note performance characteristics

### Platform Compatibility

**Endianness:**

```c
#include "conformLittleEndian.h"
// Use conversion macros when needed
```

**64-bit assumptions:**

```c
// Check for 64-bit where required
#if __x86_64__
// 64-bit specific code
#endif
```

**Platform-specific code:**

```c
#if defined(__APPLE__)
/* macOS-specific */
#elif defined(__linux__)
/* Linux-specific */
#elif defined(__FreeBSD__) || defined(__OpenBSD__) ||                          \
    defined(__NetBSD__) || defined(__DragonFly__)
/* BSD-specific */
#else
/* Fall back to plain POSIX rather than #error where you can */
#endif
```

datakit targets POSIX systems built with clang or gcc. Do not
introduce `_WIN32`, `_MSC_VER`, `_M_X64`, `<intrin.h>`,
`__forceinline`, or any other MSVC construct — Windows support was
removed deliberately, and vendored upstream code has its MSVC branches
stripped as part of integration.

## Testing Requirements

### Test Coverage

Every new feature must include:

1. **Basic functionality tests**
2. **Edge case tests** (empty, single element, maximum)
3. **Stress tests** (large scale, many operations)
4. **Error handling tests**

See [TESTING.md](TESTING.md) for details.

### Test Quality

Good tests:

- ✅ Are deterministic (same results every run)
- ✅ Are independent (don't depend on other tests)
- ✅ Clean up all resources
- ✅ Have clear failure messages
- ✅ Test one thing at a time
- ✅ Are fast (sub-second for most tests)

Bad tests:

- ❌ Use random input without seeds
- ❌ Leak memory
- ❌ Depend on execution order
- ❌ Have generic error messages
- ❌ Test multiple unrelated things
- ❌ Take too long to run

### Example Test Structure

```c
#ifdef DATAKIT_TEST
#include "ctest.h"

int myModuleTest(int argc, char *argv[]) {
    int err = 0;

    TEST("Basic CRUD operations");
    {
        myModule *m = myModuleNew();
        if (!m) {
            ERR("Failed to create module");
        }

        // Create
        bool created = myModuleInsert(m, "key", "value");
        if (!created) {
            ERR("Insert failed for key=%s", "key");
        }

        // Read
        const char *value = myModuleGet(m, "key");
        if (!value || strcmp(value, "value") != 0) {
            ERR("Expected 'value' but got '%s'", value);
        }

        // Update
        bool updated = myModuleUpdate(m, "key", "newvalue");
        if (!updated) {
            ERR("Update failed");
        }

        // Delete
        bool deleted = myModuleDelete(m, "key");
        if (!deleted) {
            ERR("Delete failed");
        }

        if (myModuleCount(m) != 0) {
            ERR("Expected 0 elements but got %zu", myModuleCount(m));
        }

        myModuleFree(m);
    }

    TEST("Edge cases");
    {
        myModule *m = myModuleNew();

        // Test with NULL
        bool result = myModuleInsert(m, NULL, "value");
        if (result) {
            ERR("Should reject NULL key");
        }

        // Test with empty string
        result = myModuleInsert(m, "", "value");
        // ... validate behavior ...

        myModuleFree(m);
    }

    TEST("Stress test");
    {
        myModule *m = myModuleNew();
        const int COUNT = 10000;

        // Insert many elements
        for (int i = 0; i < COUNT; i++) {
            char key[64];
            snprintf(key, sizeof(key), "key%d", i);
            if (!myModuleInsert(m, key, "value")) {
                ERR("Insert failed at iteration %d", i);
                break;
            }
        }

        // Verify count
        if (myModuleCount(m) != COUNT) {
            ERR("Expected %d elements but got %zu",
                COUNT, myModuleCount(m));
        }

        // Cleanup
        myModuleFree(m);
    }

    TEST_FINAL_RESULT;
}
#endif
```

## Documentation

### Code Documentation

- **Public APIs**: Document in headers
- **Complex algorithms**: Explain in implementation
- **Non-obvious behavior**: Add comments
- **Performance notes**: Document time/space complexity

### External Documentation

When adding major features, update:

- `README.md` - High-level usage examples
- `docs/` - Detailed documentation
- Module-specific docs in `docs/modules/`

### Commit Messages

Good commit messages:

```
Add red-black tree implementation

- Implements self-balancing binary search tree
- O(log n) insert, delete, and search
- Includes comprehensive test suite with 10k elements
- Memory tested with valgrind

Fixes #123
```

Bad commit messages:

```
fix bug
update code
changes
wip
```

Format:

```
<type>: <short summary>

<detailed description>

<footer>
```

Types: `feat`, `fix`, `docs`, `test`, `refactor`, `perf`, `style`

## Submitting Changes

### Before Submitting

1. **Run the full test suite:**

```sh
cmake --preset dev && cmake --build --preset dev
ctest --test-dir build/dev -j8 --output-on-failure
```

2. **Check for memory errors:**

The `dev` preset's ASAN/UBSAN already covers most of this. For a
sanitizer-free cross-check:

```sh
for test in flex multimap multilist; do
    valgrind --leak-check=full ./build/perf/src/datakit-test test $test
done
```

3. **Verify no warnings:**

```sh
cmake --build --preset dev --clean-first 2>&1 | grep -i warning
```

4. **Run static analysis:**

```sh
cmake --build build/dev --target cppcheck
```

5. **Test on multiple platforms** (if possible):
   - Linux
   - macOS

### Pull Request Process

**Note**: Given the proprietary nature of this project, coordinate with the maintainer before submitting changes.

1. **Create feature branch:**

```bash
git checkout -b feature/descriptive-name
```

2. **Make changes** following guidelines above

3. **Commit with clear messages:**

```bash
git add src/mymodule.c src/mymodule.h
git commit -m "feat: add new module with tests"
```

4. **Push to repository:**

```bash
git push origin feature/descriptive-name
```

5. **Submit pull request** with:
   - Clear description of changes
   - Test results
   - Performance impact (if any)
   - Breaking changes (if any)

### Review Process

Expect review feedback on:

- Code style and standards
- Test coverage
- Performance implications
- API design
- Documentation
- Memory management

Be prepared to:

- Address review comments
- Add more tests
- Refactor code
- Update documentation

## Best Practices

### Performance

- **Profile before optimizing**
- **Use appropriate data structures**
- **Minimize allocations in hot paths**
- **Consider cache locality**
- **Document performance characteristics**

### Maintainability

- **Keep functions small and focused**
- **Use descriptive names**
- **Comment non-obvious code**
- **Avoid premature optimization**
- **Refactor duplicated code**

### Safety

- **Check all allocations**
- **Validate inputs**
- **Free all allocated memory**
- **Use asserts for invariants**
- **Test edge cases**

### Testing

- **Test as you develop**
- **Run valgrind frequently**
- **Keep tests fast**
- **Make tests deterministic**
- **Clean up test resources**

## Getting Help

For questions or clarifications:

1. **Review existing code** for patterns
2. **Check documentation** in docs/
3. **Read similar modules** for examples
4. **Contact maintainer** if needed

## Resources

- [RUNBOOK.md](RUNBOOK.md) - The step-by-step checklist for every change
- [BUILDING.md](BUILDING.md) - Build instructions
- [TESTING.md](TESTING.md) - Testing guide
- [DEBUGGING.md](DEBUGGING.md) - Debugging techniques
- README.md - Project overview
- docs/ - Additional documentation
