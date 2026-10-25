# Platform-Specific Features and Considerations

## Overview

datakit is **POSIX-only** and portable across the Unix-like operating
systems and CPU architectures below, leveraging platform-specific
optimizations when available. This guide explains platform differences,
feature detection, and optimization opportunities.

**There is no Windows support.** No `_WIN32`/`_MSC_VER` code paths, no
MSVC build, no MinGW target. The supported compilers are clang and gcc
only; upstream code carrying MSVC branches has those branches stripped
when it is vendored in.

## Supported Platforms

### Operating Systems

| Platform      | Status      | Notes                                            |
| ------------- | ----------- | ------------------------------------------------ |
| **Linux**     | Primary     | Fully supported, extensively tested              |
| **macOS**     | Primary     | Darwin; also the main Apple Silicon test host    |
| **FreeBSD**   | Supported   | `DK_OS_FREEBSD`; BSD random/proctitle paths      |
| **OpenBSD**   | Supported   | `DK_OS_OPENBSD`; `getentropy()` path             |
| **NetBSD**    | Supported   | `DK_OS_NETBSD`                                   |
| **DragonFly** | Best effort | no `DK_OS_*` flag; generic POSIX paths only      |
| **AIX**       | Best effort | `DK_OS_AIX` exists; not built or tested by CI    |
| **Solaris**   | Unsupported | Removed entirely; do not add `__sun` branches    |
| **Windows**   | Unsupported | Removed entirely; do not add MSVC/Win32 branches |

### CPU Architectures

| Architecture | Status       | Special Features       |
| ------------ | ------------ | ---------------------- |
| **x86_64**   | Primary      | F16C, AVX, SSE support |
| **ARM64**    | Supported    | NEON available         |
| **ARM32**    | Supported    | Limited SIMD           |
| **RISC-V**   | Experimental | Basic support          |
| **PowerPC**  | Supported    | Big-endian testing     |
| **MIPS**     | Supported    | Basic support          |

## Feature Detection

### Compile-Time Detection

Three mechanisms, each in one place:

- **Target operating system** — `src/dkPlatform.h` sets exactly one of
  `DK_OS_LINUX`, `DK_OS_APPLE`, `DK_OS_FREEBSD`, `DK_OS_NETBSD`,
  `DK_OS_OPENBSD`, `DK_OS_AIX` to 1 and the rest to 0, from the compiler's
  predefined macros (`__linux__`, `__APPLE__`, ...). OS-specific code
  tests them with `#if` (the `/proc` readers in `OSRegulate.c` with
  `DK_OS_LINUX`, `task_info()` with `DK_OS_APPLE`). It also refuses a
  compiler other than clang or gcc and defines `DK_BITS`, the pointer width.
- **System calls that vary by libc** — `src/CMakeLists.txt` compiles and
  runs small probes and passes the results as compile definitions:
  `DK_HAVE_GETRANDOM`, `DK_HAVE_LINUX_SYS_GETRANDOM`, `DK_HAVE_GETENTROPY`,
  `DK_HAVE_GETENTROPY_SYS_RANDOM` (see Random Number Generation below).
- **CPU instructions** — code tests the compiler's own target macros
  directly (`__F16C__`, `__AVX2__`, `__ARM_NEON`, `__ARM_FEATURE_CRYPTO`),
  so the instruction set chosen with `-m` flags decides the path.

### Runtime Detection

Some features are detected at runtime:

```c
/* CPU feature detection example */
#if defined(__x86_64__)
    bool has_f16c = __builtin_cpu_supports("f16c");
    bool has_avx = __builtin_cpu_supports("avx");
    bool has_avx2 = __builtin_cpu_supports("avx2");
#endif
```

Guard on the compiler-defined architecture macros only (`__x86_64__`,
`__aarch64__`, `__ARM_NEON`, …). MSVC's `_M_X64` / `_M_AMD64` spellings
have no meaning in a clang/gcc-only tree.

## Platform-Specific Optimizations

### Float16 Hardware Acceleration (x86_64)

**F16C instruction set** (Intel Ivy Bridge+, AMD Jaguar+):

```c
#if __F16C__
/* Hardware conversion */
#include <immintrin.h>

#define float16Encode(v) _cvtss_sh(v, 0)
#define float16Decode(v) _cvtsh_ss(v)

#else
/* Software fallback */
uint16_t float16Encode(float value) {
    /* Bit manipulation implementation */
}

float float16Decode(uint16_t value) {
    /* Bit manipulation implementation */
}
#endif
```

**Compile flags:**

```bash
# Enable F16C on modern x86_64
gcc -mf16c -O3 -o program program.c

# Check if CPU supports F16C
gcc -march=native -dM -E - < /dev/null | grep F16C
```

**Performance impact:** F16C has not been measured here, because there is no
x86-64 test host. The same comparison on AArch64 (NEON hardware conversion
vs the portable functions) is in
[PERFORMANCE.md](PERFORMANCE.md#float16-encoding). There the vectorized
portable loops come within 1.2x of hardware, so measure on your target with
`./build/perf/src/datakit-test test compressionBench` before relying on a
speed-up.

### Random Number Generation

`portableRandomDirect` (`src/portableRandom.c`) takes the first source the
build's probes found, in this order:

1. `getrandom()` (`DK_HAVE_GETRANDOM`, glibc 2.25+ and the BSDs that have it)
2. the raw `SYS_getrandom` syscall (`DK_HAVE_LINUX_SYS_GETRANDOM`, Linux 3.17+
   on an older glibc)
3. `getentropy()` (`DK_HAVE_GETENTROPY` / `DK_HAVE_GETENTROPY_SYS_RANDOM`,
   macOS and OpenBSD)
4. otherwise `/dev/urandom` — the probes run test programs, so a cross
   build detects none and reads the device

**Performance comparison:** there is no in-tree benchmark for these sources,
so no figures are given. `getrandom()` and `getentropy()` are single system
calls. The `/dev/urandom` fallback costs an `open`, a `read` and a `close`
per use unless the descriptor is kept open, so it is the slowest of the
three.

### Memory Management

#### madvise() - Memory Hints

```c
#if defined(__linux__) || defined(__APPLE__)
    #include <sys/mman.h>

    /* Hint: sequential access */
    madvise(ptr, len, MADV_SEQUENTIAL);

    /* Hint: random access */
    madvise(ptr, len, MADV_RANDOM);

    /* Hint: won't need soon */
    madvise(ptr, len, MADV_DONTNEED);

    /* Linux-specific: transparent huge pages */
    #ifdef __linux__
        madvise(ptr, len, MADV_HUGEPAGE);
    #endif
#endif
```

**Use cases:**

```c
/* Large sequential scan */
flex *large = /* ... allocate 100 MB ... */;
madvise(large, flexBytes(large), MADV_SEQUENTIAL);
// Kernel prefetches aggressively

/* Large random access structure */
multimap *random_access = /* ... */;
madvise(random_access, multimapBytes(random_access), MADV_RANDOM);
// Kernel reduces prefetching
```

#### mlock() - Prevent Swapping

```c
#if defined(__linux__) || defined(__APPLE__)
    #include <sys/mman.h>

    /* Lock sensitive data in RAM */
    void *crypto_keys = malloc(4096);
    mlock(crypto_keys, 4096);

    /* ... use keys ... */

    munlock(crypto_keys, 4096);
    free(crypto_keys);
#endif
```

**Warning**: Requires elevated privileges or increased `ulimit -l`.

### Process Management

#### setproctitle - Process Name

datakit's `setproctitle.h` selects one of three variants at compile
time: the BSDs alias the native `setproctitle(3)` behind a
**zero-argument** `setProctitleInit()` macro, Linux/macOS use a real
`setProctitleInit(char *argv[])` that relocates the argv/environ
region, and other platforms compile both away. Portable code must
guard the init call — `setProctitleInit(argv)` is a compile error on
the macro platforms (see
[SETPROCTITLE](../modules/system/SETPROCTITLE.md) for the full
variant table):

```c
#include "setproctitle.h"

int main(int argc, char *argv[]) {
#if DK_OS_LINUX || DK_OS_APPLE
    setProctitleInit(argv);
#else
    setProctitleInit();
#endif
    setproctitle("datakit-server: handling request");
    /* ... */
}
```

#### Process Limits

```c
#include <sys/resource.h>

/* Get/Set resource limits */
struct rlimit limit;

/* File descriptors */
getrlimit(RLIMIT_NOFILE, &limit);
limit.rlim_cur = 65536;
setrlimit(RLIMIT_NOFILE, &limit);

/* Memory size */
getrlimit(RLIMIT_AS, &limit);
limit.rlim_cur = 1024 * 1024 * 1024;  /* 1 GB */
setrlimit(RLIMIT_AS, &limit);

/* CPU time */
getrlimit(RLIMIT_CPU, &limit);
limit.rlim_cur = 3600;  /* 1 hour */
setrlimit(RLIMIT_CPU, &limit);
```

### Endianness

datakit handles both little-endian and big-endian systems:

```c
/* Detect endianness at compile time */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    #define DATAKIT_BIG_ENDIAN 1
#else
    #define DATAKIT_LITTLE_ENDIAN 1
#endif

/* Byte swapping when needed */
#ifdef DATAKIT_BIG_ENDIAN
    static inline uint32_t le32toh(uint32_t val) {
        return __builtin_bswap32(val);
    }
    static inline uint64_t le64toh(uint64_t val) {
        return __builtin_bswap64(val);
    }
#else
    #define le32toh(x) (x)
    #define le64toh(x) (x)
#endif
```

**Testing on big-endian:**

```bash
# QEMU can emulate big-endian systems
qemu-system-ppc64 -M pseries -cpu POWER9 ...
```

## Platform-Specific Performance

### Linux-Specific Optimizations

```c
/* Transparent Huge Pages (THP) */
#ifdef __linux__
    void *large_alloc = mmap(NULL, size, PROT_READ|PROT_WRITE,
                            MAP_PRIVATE|MAP_ANONYMOUS|MAP_HUGETLB, -1, 0);
    if (large_alloc != MAP_FAILED) {
        /* Using 2MB pages instead of 4KB */
        /* Reduces TLB pressure significantly */
    }
#endif

/* perf_event for profiling */
#ifdef __linux__
    #include <linux/perf_event.h>
    /* Hardware performance counters */
#endif

/* io_uring for async I/O (Linux 5.1+) */
#ifdef __linux__
    #include <liburing.h>
    /* Extremely fast I/O operations */
#endif
```

### macOS-Specific Optimizations

```c
/* Dispatch queues (GCD) */
#ifdef __APPLE__
    #include <dispatch/dispatch.h>

    dispatch_queue_t queue = dispatch_get_global_queue(
        DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);

    dispatch_async(queue, ^{
        /* Parallel work */
    });
#endif

/* Memory pressure notifications */
#ifdef __APPLE__
    #include <dispatch/dispatch.h>

    dispatch_source_t source = dispatch_source_create(
        DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
        DISPATCH_MEMORYPRESSURE_WARN, queue);

    dispatch_source_set_event_handler(source, ^{
        /* React to memory pressure */
        flush_caches();
    });
#endif
```

### FreeBSD-Specific Features

```c
/* jemalloc is native on FreeBSD */
#ifdef __FreeBSD__
    #include <malloc_np.h>

    /* Query jemalloc statistics */
    size_t allocated;
    size_t len = sizeof(allocated);
    mallctl("stats.allocated", &allocated, &len, NULL, 0);
#endif

/* Capsicum security framework */
#ifdef __FreeBSD__
    #include <sys/capsicum.h>

    /* Enter capability mode (sandbox) */
    cap_enter();
#endif
```

## Compiler Differences

### GCC vs Clang

```c
/* Compiler detection */
#if defined(__GNUC__) && !defined(__clang__)
    #define COMPILER_GCC 1
#elif defined(__clang__)
    #define COMPILER_CLANG 1
#endif

/* Inline hints */
#if defined(COMPILER_GCC) || defined(COMPILER_CLANG)
    #define ALWAYS_INLINE __attribute__((always_inline)) inline
    #define NEVER_INLINE __attribute__((noinline))
    #define LIKELY(x) __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

/* Builtin functions */
#if defined(COMPILER_GCC) || defined(COMPILER_CLANG)
    #define popcount(x) __builtin_popcountll(x)
    #define clz(x) __builtin_clzll(x)  /* Count leading zeros */
    #define ctz(x) __builtin_ctzll(x)  /* Count trailing zeros */
#endif
```

### Optimization Flags

```bash
# GCC/Clang
-O3              # Maximum optimization
-march=native    # Use all CPU features
-mtune=native    # Optimize for local CPU
-flto            # Link-time optimization
-ffast-math      # Aggressive float optimizations

# Profile-guided optimization
gcc -fprofile-generate -O3 program.c -o program
./program < test_data
gcc -fprofile-use -O3 program.c -o program
```

## Cross-Platform Build System

### CMake Configuration

`src/CMakeLists.txt` builds one source list for every platform: OS-specific
code lives inside the shared files behind `DK_OS_*` (from
`src/dkPlatform.h`), so no per-platform source files are added. The build:

- sets the warning flags (`-Wall -Wextra -Winline`, and
  `-Wmissing-prototypes` on datakit's own objects) and the C standard;
- adds instruction-set flags only where the target needs them (`-mssse3` on
  x86_64; aarch64 has NEON without flags);
- probes `getrandom`, `SYS_getrandom` and `getentropy` with
  `check_c_source_runs` and passes `DK_HAVE_*` compile definitions.

Presets (`CMakePresets.json`, see [Build Presets](../BUILD_PRESETS.md))
select the build type, sanitizers and coverage.

### Makefile Portability

```makefile
# Detect platform
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

# Platform-specific settings
ifeq ($(UNAME_S),Linux)
    CFLAGS += -D_GNU_SOURCE
    LDFLAGS += -pthread
endif

ifeq ($(UNAME_S),Darwin)
    CFLAGS += -D_DARWIN_C_SOURCE
endif

ifeq ($(findstring BSD,$(UNAME_S)),BSD)
    CFLAGS += -D_BSD_SOURCE
endif

# CPU-specific flags
ifeq ($(UNAME_M),x86_64)
    CFLAGS += -mf16c -mavx
endif

ifeq ($(UNAME_M),aarch64)
    CFLAGS += -march=armv8-a
endif
```

## Testing Across Platforms

### Docker-Based Testing

```bash
# Test on Ubuntu
docker run -v $(pwd):/src ubuntu:22.04 \
    bash -c "cd /src && make test"

# Test on Alpine (musl libc)
docker run -v $(pwd):/src alpine:latest \
    bash -c "cd /src && make test"

# Test on Fedora
docker run -v $(pwd):/src fedora:latest \
    bash -c "cd /src && make test"
```

FreeBSD and the other BSDs cannot be covered by Docker — the kernel
differs. Use a VM or a real host, and build with `gmake` there.

### CI/CD Pipeline

```yaml
# GitHub Actions example
name: Cross-Platform Tests

on: [push, pull_request]

jobs:
  test:
    strategy:
      matrix:
        os: [ubuntu-latest, macos-latest]
        compiler: [gcc, clang]

    runs-on: ${{ matrix.os }}

    steps:
      - uses: actions/checkout@v2

      - name: Build
        run: |
          mkdir build
          cd build
          cmake -DCMAKE_C_COMPILER=${{ matrix.compiler }} ..
          cmake --build .

      - name: Test
        run: |
          cd build
          ctest --output-on-failure
```

## Platform-Specific Issues

### Issue 1: Alignment Requirements

```c
/* Some architectures require strict alignment */
#if defined(__arm__) && !defined(__ARM_FEATURE_UNALIGNED)
    /* ARM32 without unaligned support */
    #define REQUIRE_ALIGNMENT 1
#endif

#ifdef REQUIRE_ALIGNMENT
    /* Use memcpy for unaligned access */
    uint32_t read_unaligned(const void *ptr) {
        uint32_t value;
        memcpy(&value, ptr, sizeof(value));
        return value;
    }
#else
    /* Direct access is safe */
    #define read_unaligned(ptr) (*(uint32_t*)(ptr))
#endif
```

### Issue 2: Atomic Operations

```c
/* C11 atomics preferred */
#if __STDC_VERSION__ >= 201112L && !defined(__STDC_NO_ATOMICS__)
    #include <stdatomic.h>
    typedef atomic_uint_fast64_t atomic_counter;

    #define atomic_inc(ptr) atomic_fetch_add(ptr, 1)
    #define atomic_load_relaxed(ptr) atomic_load_explicit(ptr, memory_order_relaxed)

/* GCC/Clang builtins */
#elif defined(__GNUC__)
    typedef uint64_t atomic_counter;

    #define atomic_inc(ptr) __sync_fetch_and_add(ptr, 1)
    #define atomic_load_relaxed(ptr) (*(ptr))
#endif
```

### Issue 3: Time Functions

```c
/* High-resolution time */
#if defined(__linux__)
    #include <time.h>
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = ts.tv_sec * 1000000000ULL + ts.tv_nsec;

#elif defined(__APPLE__)
    #include <mach/mach_time.h>
    uint64_t ns = mach_absolute_time();
#endif
```

## Best Practices

### 1. Use Feature Detection

```c
/* GOOD: Runtime feature detection */
#if defined(__x86_64__)
    if (__builtin_cpu_supports("f16c")) {
        use_hardware_float16();
    } else {
        use_software_float16();
    }
#endif

/* AVOID: Hardcoded assumptions */
#ifdef __x86_64__
    always_use_hardware_float16();  /* May crash on old CPUs */
#endif
```

### 2. Test on Multiple Platforms

```bash
# Test matrix
- Linux (glibc, musl)
- macOS (latest, previous)
- FreeBSD (latest)

# Test architectures
- x86_64
- ARM64
- Big-endian (PowerPC, SPARC)
```

### 3. Avoid Platform-Specific APIs

```c
/* GOOD: POSIX-compliant */
#include <pthread.h>
pthread_mutex_t lock;
pthread_mutex_lock(&lock);

/* AVOID: Platform-specific unless necessary */
#ifdef __linux__
    #include <linux/futex.h>
    syscall(SYS_futex, ...);  /* Linux-only */
#endif
```

### 4. Document Platform Requirements

```c
/*
 * Requirements:
 * - C11 or later
 * - POSIX threads (pthread)
 * - 64-bit architecture recommended
 *
 * Optional:
 * - F16C for hardware float16 (x86_64)
 * - AVX for SIMD operations
 * - getrandom() for secure RNG (Linux 3.17+)
 */
```

## See Also

- [PERFORMANCE.md](PERFORMANCE.md) - Platform-specific optimizations
- [THREAD_SAFETY.md](THREAD_SAFETY.md) - Platform threading models
- [Architecture Overview](../ARCHITECTURE.md) - Platform abstraction design
