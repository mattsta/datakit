# Platform-Specific Features and Considerations

## Overview

datakit is designed to be **highly portable** across different operating systems and CPU architectures while leveraging platform-specific optimizations when available. This guide explains platform differences, feature detection, and optimization opportunities.

## Supported Platforms

### Operating Systems

| Platform | Status | Notes |
|----------|--------|-------|
| **Linux** | Primary | Fully supported, extensively tested |
| **macOS** | Supported | Darwin/BSD features |
| **FreeBSD** | Supported | BSD variants fully supported |
| **OpenBSD** | Supported | BSD variants fully supported |
| **NetBSD** | Supported | BSD variants fully supported |
| **Solaris** | Supported | Legacy Unix support |
| **AIX** | Supported | IBM Unix support |
| **Windows** | Partial | Cygwin, MinGW supported |

### CPU Architectures

| Architecture | Status | Special Features |
|-------------|--------|-----------------|
| **x86_64** | Primary | F16C, AVX, SSE support |
| **ARM64** | Supported | NEON available |
| **ARM32** | Supported | Limited SIMD |
| **RISC-V** | Experimental | Basic support |
| **PowerPC** | Supported | Big-endian testing |
| **MIPS** | Supported | Basic support |

## Feature Detection

### Compile-Time Detection

datakit uses `config.h` for compile-time feature detection:

```c
/* Random number generation */
#if defined(__linux__)
    #define HAVE_GETRANDOM 1
#elif defined(__APPLE__) || defined(__OpenBSD__)
    #define HAVE_GETENTROPY 1
#else
    #define HAVE_DEV_URANDOM 1
#endif

/* Memory advice */
#if defined(__linux__) || defined(__APPLE__)
    #define HAVE_MADVISE 1
#endif

/* CPU instructions */
#if defined(__F16C__)
    #define HAVE_F16C 1  /* float16 hardware support */
#endif

#if defined(__AVX__)
    #define HAVE_AVX 1   /* AVX vectorization */
#endif
```

### Runtime Detection

Some features are detected at runtime:

```c
/* CPU feature detection example */
#if defined(__x86_64__) || defined(_M_X64)
    bool has_f16c = __builtin_cpu_supports("f16c");
    bool has_avx = __builtin_cpu_supports("avx");
    bool has_avx2 = __builtin_cpu_supports("avx2");
#endif
```

## Platform-Specific Optimizations

### Float16 Hardware Acceleration (x86_64)

**F16C instruction set** (Intel Ivy Bridge+, AMD Jaguar+):

```c
#if __F16C__
/* Hardware conversion: ~0.5 ns per conversion */
#include <immintrin.h>

#define float16Encode(v) _cvtss_sh(v, 0)
#define float16Decode(v) _cvtsh_ss(v)

#else
/* Software fallback: ~15 ns per conversion */
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

**Performance impact:**
```
Hardware (F16C):  0.5 ns/conversion  (200M conversions/sec)
Software:        15.0 ns/conversion  (66M conversions/sec)
Speedup:         30x
```

### Random Number Generation

Different platforms provide different random sources:

```c
/* Linux 3.17+ - fastest, most secure */
#ifdef __linux__
    #include <sys/random.h>
    ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);
#endif

/* macOS, OpenBSD - secure, slower */
#if defined(__APPLE__) || defined(__OpenBSD__)
    #include <sys/random.h>
    int getentropy(void *buf, size_t buflen);
#endif

/* Fallback - portable but requires /dev/urandom */
#ifndef HAVE_GETRANDOM
    int fd = open("/dev/urandom", O_RDONLY);
    read(fd, buf, len);
    close(fd);
#endif
```

**Performance comparison:**
```
getrandom():      ~500 ns for 16 bytes
getentropy():     ~1200 ns for 16 bytes
/dev/urandom:     ~2000 ns for 16 bytes (open/read/close)
```

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

```c
/* BSD, Linux with libsetproctitle */
#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    #include <sys/types.h>
    #include <unistd.h>
    setproctitle("datakit-server: handling request");
#endif

/* Linux native (via /proc/self/cmdline) */
#ifdef __linux__
    void setProcessTitle(const char *title) {
        /* Custom implementation */
        prctl(PR_SET_NAME, title, 0, 0, 0);
    }
#endif
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

### GCC vs Clang vs MSVC

```c
/* Compiler detection */
#if defined(__GNUC__) && !defined(__clang__)
    #define COMPILER_GCC 1
#elif defined(__clang__)
    #define COMPILER_CLANG 1
#elif defined(_MSC_VER)
    #define COMPILER_MSVC 1
#endif

/* Inline hints */
#if defined(COMPILER_GCC) || defined(COMPILER_CLANG)
    #define ALWAYS_INLINE __attribute__((always_inline)) inline
    #define NEVER_INLINE __attribute__((noinline))
    #define LIKELY(x) __builtin_expect(!!(x), 1)
    #define UNLIKELY(x) __builtin_expect(!!(x), 0)
#elif defined(COMPILER_MSVC)
    #define ALWAYS_INLINE __forceinline
    #define NEVER_INLINE __declspec(noinline)
    #define LIKELY(x) (x)
    #define UNLIKELY(x) (x)
#endif

/* Builtin functions */
#if defined(COMPILER_GCC) || defined(COMPILER_CLANG)
    #define popcount(x) __builtin_popcountll(x)
    #define clz(x) __builtin_clzll(x)  /* Count leading zeros */
    #define ctz(x) __builtin_ctzll(x)  /* Count trailing zeros */
#elif defined(COMPILER_MSVC)
    #include <intrin.h>
    #define popcount(x) __popcnt64(x)
    #define clz(x) _lzcnt_u64(x)
    #define ctz(x) _tzcnt_u64(x)
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

# MSVC
/O2              # Maximum optimization
/GL              # Whole program optimization
/arch:AVX2       # Use AVX2 instructions
```

## Cross-Platform Build System

### CMake Configuration

```cmake
cmake_minimum_required(VERSION 3.10)
project(datakit C)

# Platform detection
if(UNIX AND NOT APPLE)
    set(LINUX TRUE)
endif()

# Compiler flags
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -O3)

    # Enable F16C on x86_64
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        add_compile_options(-mf16c)
    endif()

    # Enable NEON on ARM
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64")
        add_compile_options(-mfpu=neon)
    endif()
endif()

# Platform-specific sources
if(LINUX)
    list(APPEND SOURCES src/linux_specific.c)
elseif(APPLE)
    list(APPEND SOURCES src/macos_specific.c)
elseif(BSD)
    list(APPEND SOURCES src/bsd_specific.c)
endif()

# Feature detection
include(CheckIncludeFile)
include(CheckFunctionExists)

check_include_file(sys/random.h HAVE_SYS_RANDOM_H)
check_function_exists(getrandom HAVE_GETRANDOM)
check_function_exists(getentropy HAVE_GETENTROPY)
check_function_exists(madvise HAVE_MADVISE)

# Generate config.h
configure_file(config.h.in config.h)
```

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

# Test on CentOS
docker run -v $(pwd):/src centos:8 \
    bash -c "cd /src && make test"

# Test on FreeBSD
docker run -v $(pwd):/src freebsd:13 \
    bash -c "cd /src && gmake test"
```

### CI/CD Pipeline

```yaml
# GitHub Actions example
name: Cross-Platform Tests

on: [push, pull_request]

jobs:
  test:
    strategy:
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]
        compiler: [gcc, clang]
        exclude:
          - os: windows-latest
            compiler: gcc

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

/* MSVC intrinsics */
#elif defined(_MSC_VER)
    typedef volatile LONG64 atomic_counter;

    #define atomic_inc(ptr) InterlockedIncrement64(ptr)
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

#elif defined(_WIN32)
    #include <windows.h>
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    uint64_t ns = (counter.QuadPart * 1000000000ULL) / freq.QuadPart;
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
- Windows (MinGW, Cygwin)

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
