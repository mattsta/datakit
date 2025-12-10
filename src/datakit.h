#pragma once

#ifndef __STDC_WANT_LIB_EXT1__
#define __STDC_WANT_LIB_EXT1__ 1
#endif

#if defined(linux) || defined(__linux) || defined(__linux__)
#include <features.h>
#endif

/* establish some global preconditions */
#include "config.h"

/* ====================================================================
 * Macros to expose features
 * ==================================================================== */
#ifndef _BSD_SOURCE
#define _BSD_SOURCE
#endif

#if DK_OS_AIX
#define _ALL_SOURCE
#endif

#if DK_OS_LINUX || DK_OS_OPENBSD
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#elif !defined(__NetBSD__)
#define _XOPEN_SOURCE
#endif

#if DK_OS_SOLARIS
#define _POSIX_C_SOURCE 199506L
#endif

#if DK_OS_APPLE
#define _DARWIN_USE_64_BIT_INODE
#endif

#ifndef _LARGEFILE_SOURCE
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#define _FILE_OFFSET_BITS 64

/* ====================================================================
 * Headers
 * ==================================================================== */
#if DK_OS_LINUX
#ifndef __USE_GNU
#define __USE_GNU 1
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#endif

#if DK_OS_APPLE
#include <AvailabilityMacros.h>
#endif

#include <errno.h>

#if DK_OS_LINUX
#ifndef __USE_XOPEN2K8
#define __USE_XOPEN2K8 /* ugh, stupid centos */
#endif
#endif
#include <assert.h>
#include <inttypes.h> /* PRId64, PRIu64, etc */
#include <limits.h>   /* UINT_MAX, etc */
#include <stdbool.h>
#include <stddef.h> /* ptrdiff_t, size_t */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>   /* memcpy, memmem, etc */
#include <sys/time.h> /* gettimeofday */
#include <time.h>     /* linux / BSD time interfaces */
#include <unistd.h>   /* getpid / load available feature flags */

#if DK_OS_APPLE
#include <mach/clock.h>     /* OS X calendar clock */
#include <mach/mach.h>      /* OS X mach kernel */
#include <mach/mach_time.h> /* OS X time interfaces */
#endif

#include "jebuf.h"

#if defined(DATAKIT_TEST) || defined(DATAKIT_TEST_VERBOSE)
#include <stdio.h> /* for printf (debug printing), snprintf (genstr) */
#endif

/* ====================================================================
 * Platform switchers
 * ==================================================================== */
#if _POSIX_SYNCHRONIZED_IO > 0
#define dk_fsync fdatasync
#else
#define dk_fsync fsync
#endif

/* ====================================================================
 * Compiler helpers
 * ==================================================================== */
/* only clang gives us __has_builtin */
#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

/* only clang gives us __has_feature */
#ifndef __has_feature
#define __has_feature(x) 0
#endif

/* GCC added __builtin_unreachable in 4.5, clang has feature detection. */
#if DK_GNUC_LT(4, 5, 0) && !__has_builtin(__builtin_unreachable)
#define __builtin_unreachable() (void)0
#endif

#if __STDC_VERSION__ >= 201112L
#define DK_C11 1
#endif

#if DK_C11
#define DK_SIZECHECK(thing, size)                                              \
    _Static_assert(sizeof(thing) == (size), #thing " changed from " #size "?")
#else
#define DK_SIZECHECK(thing, size) (void)0
#endif

#ifndef __clang__
#define dk_fallthrough __attribute__((fallthrough))
#else
#define dk_fallthrough (void)0
#endif

#define DK_DIV_CEIL(a, b) (((a) + (b) - 1) / (b))
#define DK_BYTESUSED(value)                                                    \
    (value ? DK_DIV_CEIL(((sizeof(value) * 8) - __builtin_clzll(value)), 8) : 0)

/* ====================================================================
 * Feature exposers
 * ==================================================================== */
#if DK_GNUC_GTE(3, 0, 0) || __has_builtin(__builtin_ctz)
#define DK_HAS_CTZ 1
#endif

#if DK_GNUC_GTE(3, 0, 0) || __has_builtin(__builtin_clz)
#define DK_HAS_CLZ 1
#endif

#if (__i386 || __amd64 || __powerpc__ || __arm64__) && DK_GNUC_VERSION_CODE
#if DK_CLANG_VERSION_CODE
#define DK_HAS_ATOMICS
#endif
#elif defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if DK_GNUC_GTE(4, 1, 0) && __GLIBC_PREREQ(2, 6)
#define DK_HAS_ATOMICS
#endif
#else
#warning "No atomic intrinsics detected."
#endif

/* ====================================================================
 * Testing helpers
 * ==================================================================== */
/* If not verbose testing, remove all debug printing. */
#if DK_TEST_VERBOSE
#define D(...)                                                                 \
    do {                                                                       \
        char *filenameOnly = strrchr(__FILE__, '/');                           \
        char *filePos = filenameOnly ? ++filenameOnly : __FILE__;              \
        printf("%s:%s:%d:\t", filePos, __func__, __LINE__);                    \
        printf(__VA_ARGS__);                                                   \
        printf("\n");                                                          \
    } while (0)
#else
#define D(...)
#endif

/* Unused arguments generate annoying warnings... */
#define DK_NOTUSED(x) ((void)(x))

#ifndef DK_STATIC
#define DK_STATIC static
#endif

/* ====================================================================
 * Macro helpers
 * ==================================================================== */
/* If c is upper case, make it lowercase. We don't care about locales. */
#define DK_LOWER(c) (((c) >= 'A' && (c) <= 'Z') ? ((c) - 'A' + 'a') : (c))

#ifndef DK_CAT
#define DK_CAT(A, B) A##B
#define DK_NAME(A, B) DK_CAT(A, B)
#endif

/* ====================================================================
 * Float helpers
 * ==================================================================== */
/* https://en.wikipedia.org/wiki/Double-precision_floating-point_format#Exponent_encoding
 * https://en.wikipedia.org/wiki/Double-precision_floating-point_format#Double-precision_examples
 */
#define DK_NAN_64 0xfff8000000000000ULL /* quiet, non-signaling */
#define DK_INFINITY_POSITIVE_64 0x7ff0000000000000ULL
#define DK_INFINITY_NEGATIVE_64 0xfff0000000000000ULL

/* https://en.wikipedia.org/wiki/Single-precision_floating-point_format#Exponent_encoding
 * https://en.wikipedia.org/wiki/Single-precision_floating-point_format#Single-precision_examples
 */
#define DK_NAN_32 0xff000000U /* quiet, non-signaling */
#define DK_INFINITY_POSITIVE_32 0x7f800000U
#define DK_INFINITY_NEGATIVE_32 0xff800000U

/* ====================================================================
 * Known behavior helpers (expect)
 * ==================================================================== */
#if DK_GNUC_GTE(3, 0, 2) || DK_CLANG_VERSION_CODE || __INTEL_COMPILER >= 800
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif

/* ====================================================================
 * Known behavior helpers (always inline from lz4)
 * ==================================================================== */
#if defined(_MSC_VER) /* Visual Studio */
#define DK_INLINE_ALWAYS static __forceinline
#elif DK_GNUC_VERSION_CODE || DK_CLANG_VERSION_CODE /* normal c99+ code */
#define DK_INLINE_ALWAYS static inline __attribute__((always_inline))
#else /* fallback */
#define DK_INLINE_ALWAYS static inline
#endif

/* 'pure' means a function doesn't modify any memory and only interacts
 * with the world through its return value. */
#if DK_GNUC_GTE(2, 96, 0) || DK_CLANG_VERSION_CODE
#define DK_FN_PURE __attribute__((pure))
#else
#define DK_FN_PURE
#endif

/* 'const' means a function accepts scalar arguments and only interacts
 * with the world through its return value based on its scalar arguments.
 * No memory accesses are allowed (also means no pointer parameters). */
#if DK_GNUC_GTE(2, 5, 0) || DK_CLANG_VERSION_CODE
#define DK_FN_CONST __attribute__((const))
#else
#define DK_FN_CONST
#endif

#if DK_GNUC_GTE(4, 8, 2) || DK_CLANG_VERSION_CODE
#define DK_FN_NONNULL __attribute__((returns_nonnull))
#else
#define DK_FN_NONNULL
#endif

#if DK_GNUC_GTE(3, 2, 0) || DK_CLANG_VERSION_CODE
#define DK_FN_UNUSED __attribute__((unused))
#else
#define DK_FN_UNUSED
#endif

/* ====================================================================
 * Integer Helpers
 * ==================================================================== */
#define DK_INT128_TO_UINT128(i)                                                \
    ({                                                                         \
        assert((i) < 0);                                                       \
        (__uint128_t)(~(i)) + 1;                                               \
    })
#if 1
/* Manually undo two's compliment negative number */
#define DK_INT64_TO_UINT64(i)                                                  \
    ({                                                                         \
        assert((i) < 0);                                                       \
        (uint64_t)(~(i)) + 1;                                                  \
    })
#else
#define DK_INT64_TO_UINT64(i)                                                  \
    ((i) == INT64_MIN ? (uint64_t)INT64_MAX + 1 : (uint64_t)(-(i)))
#endif

/* ====================================================================
 * Alignment helpers
 * ==================================================================== */
/* On 64 bits, this is (ptr & 7); on 32 bits this is (ptr & 3) */
#define DK_STEP_UNALIGNMENT(stepBytes, ptr)                                    \
    ((uintptr_t)(ptr) & ((stepBytes) - 1))
#define DK_WORD_UNALIGNMENT(ptr) DK_STEP_UNALIGNMENT(sizeof(void *), ptr)

#define DK_IS_STEP_ALIGNED(stepBytes, ptr)                                     \
    (!DK_STEP_UNALIGNMENT(stepBytes, ptr))
#define DK_IS_WORD_ALIGNED(ptr) DK_IS_STEP_ALIGNED(sizeof(void *), ptr)

/* ====================================================================
 * Global memory config
 * ==================================================================== */
typedef struct datakitConfig {
    /* Primary Interface */
    void *(*localCalloc)(size_t count, size_t sz);
    void *(*localRealloc)(void *ptr, size_t sz);
    void *(*localReallocSlate)(void *ptr, size_t sz);
    void (*localFree)(void *ptr);

    /* Secondary Interface */
    void *(*localMalloc)(size_t sz);
    int (*localMemalign)(void **memptr, size_t alignment, size_t size);
} datakitConfig;

extern datakitConfig datakitConfigMemory__;
bool datakitConfigSet(datakitConfig *conf);

#if !defined(IKNOWWHATIMDOING) && defined(__GNUC__)
void *calloc(size_t count, size_t size) __attribute__((deprecated));
int posix_memalign(void **memptr, size_t alignment, size_t size)
    __attribute__((deprecated));
void free(void *ptr) __attribute__((deprecated));
void *malloc(size_t size) __attribute__((deprecated));
void *realloc(void *ptr, size_t size) __attribute__((deprecated));
#ifndef strdup
/* on some versions of linux, strdup is a macro and this breaks things
 * without checking for the 'strdup' macro first.. */
char *strdup(const char *s) __attribute__((deprecated));
#endif
#endif

#define zmemalign(ptr, align, size)                                            \
    (datakitConfigMemory__.localMemalign(ptr, align, size))
#define zcalloc(count, sz) (datakitConfigMemory__.localCalloc(count, sz))
#define zfree(ptr) (datakitConfigMemory__.localFree(ptr))
#define zrealloc(ptr, sz) (datakitConfigMemory__.localRealloc(ptr, sz))
#define zreallocAdjusted(ptr, sz)                                              \
    (datakitConfigMemory__.localRealloc(ptr, jebufSizeAllocation(sz)))
#define zreallocSlate(ptr, sz)                                                 \
    (datakitConfigMemory__.localReallocSlate(ptr, sz))
#define zreallocSlateAdjusted(ptr, sz)                                         \
    (datakitConfigMemory__.localReallocSlate(ptr, jebufSizeAllocation(sz)))

#define zmemalign_ (datakitConfigMemory__.localMemalign)
#define zcalloc_ (datakitConfigMemory__.localCalloc)
#define zmalloc_ (datakitConfigMemory__.localMalloc)
#define zfree_ (datakitConfigMemory__.localFree)

/*  We expect reallocation failure to segfault and dump, so it's always safe
 *  to realloc back to your own pointer without checking for NULL. */
#define zreallocSelf(ptr, sz)                                                  \
    do {                                                                       \
        (ptr) = zrealloc(ptr, sz);                                             \
    } while (0)
#define zreallocSelfAdjusted(ptr, sz)                                          \
    do {                                                                       \
        (ptr) = zreallocAdjusted(ptr, sz);                                     \
    } while (0)
#define zreallocSlateSelf(ptr, sz)                                             \
    do {                                                                       \
        (ptr) = zreallocSlate(ptr, sz);                                        \
    } while (0)
#define zrealloaSlatecSelfAdjusted(ptr, sz)                                    \
    do {                                                                       \
        (ptr) = zreallocSlateAdjusted(ptr, sz);                                \
    } while (0)

#ifdef DK_MALLOC_REAL
#define zmalloc(sz) (datakitConfigMemory__.localMalloc(sz))
#else
/* Free zeros for everyone! */
#define zmalloc(sz) zcalloc(1, sz)
#endif

#ifndef COUNT_ARRAY
#define COUNT_ARRAY(x) (sizeof(x) / sizeof(*(x)))
#endif
