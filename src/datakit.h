#pragma once

#ifndef __STDC_WANT_LIB_EXT1__
#define __STDC_WANT_LIB_EXT1__ 1
#endif

#if defined(__linux__)
#include <features.h>
#endif

/* establish some global preconditions */
#include "dkPlatform.h"

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
#include <stdckdint.h> /* ckd_add, ckd_sub, ckd_mul: checked arithmetic */
#include <stddef.h>    /* ptrdiff_t, size_t */
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
/* __has_feature tests clang's sanitizer and language features; gcc
 * provides it from gcc 14 (older gcc: every feature reads absent) */
#ifndef __has_feature
#define __has_feature(x) 0
#endif

/* datakit is C11 (gnu11): _Static_assert is always available */
#define DK_C11 1
#define DK_SIZECHECK(thing, size)                                              \
    _Static_assert(sizeof(thing) == (size), #thing " changed from " #size "?")

#define DK_DIV_CEIL(a, b) (((a) + (b) - 1) / (b))
#define DK_BYTESUSED(value)                                                    \
    (value ? DK_DIV_CEIL(((sizeof(value) * 8) - __builtin_clzll(value)), 8) : 0)

/* ====================================================================
 * Feature exposers
 * ==================================================================== */
/* clang and gcc both provide __builtin_ctz / __builtin_clz */
#define DK_HAS_CTZ 1
#define DK_HAS_CLZ 1

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
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* ====================================================================
 * Function attributes
 * ==================================================================== */
#define DK_INLINE_ALWAYS static inline __attribute__((always_inline))

/* 'pure' means a function doesn't modify any memory and only interacts
 * with the world through its return value. */
#define DK_FN_PURE __attribute__((pure))

/* 'const' means a function accepts scalar arguments and only interacts
 * with the world through its return value based on its scalar arguments.
 * No memory accesses are allowed (also means no pointer parameters). */
#define DK_FN_CONST __attribute__((const))

#define DK_FN_NONNULL __attribute__((returns_nonnull))
#define DK_FN_UNUSED __attribute__((unused))

/* Keep a function unoptimized: timing loops the optimizer would fold away,
 * or stores it could drop.  clang spells this optnone; GCC has no optnone
 * and takes optimize("O0"). */
#if defined(__clang__)
#define DK_FN_NO_OPTIMIZE __attribute__((optnone))
#else
#define DK_FN_NO_OPTIMIZE __attribute__((optimize("O0")))
#endif

/* Exempt intended unsigned wraparound (and, in the second form, intended
 * narrowing conversions) from clang's integer sanitizers.  Those checks
 * exist only in clang; GCC's sanitizers have nothing to exempt. */
#if defined(__clang__)
#define DK_FN_NO_SANITIZE_UNSIGNED_WRAP                                        \
    __attribute__((no_sanitize("unsigned-integer-overflow")))
#define DK_FN_NO_SANITIZE_UNSIGNED_WRAP_CONVERSION                             \
    __attribute__((                                                            \
        no_sanitize("unsigned-integer-overflow", "implicit-conversion")))
#else
#define DK_FN_NO_SANITIZE_UNSIGNED_WRAP
#define DK_FN_NO_SANITIZE_UNSIGNED_WRAP_CONVERSION
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

    /* Optional: the bytes usable at a live allocation (at least the size
     * requested), so secure wipes also clear the allocator's slack.  NULL
     * when the allocator cannot report it; wipes then cover only the
     * bytes their owner tracks. */
    size_t (*localUsableSize)(const void *ptr);
} datakitConfig;

extern datakitConfig datakitConfigMemory__;
bool datakitConfigSet(datakitConfig *conf);

/* Bytes usable at the live allocation 'ptr', or 0 when the allocator
 * cannot report it (see datakitConfig.localUsableSize) */
#define zusablesize(ptr)                                                       \
    (datakitConfigMemory__.localUsableSize                                     \
         ? datakitConfigMemory__.localUsableSize(ptr)                          \
         : (size_t)0)

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
        /* @own: caller; free=zfree */                                         \
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

/* A zmalloc'd copy of the 'bytes' bytes at 'from' (released with zfree);
 * NULL for a NULL 'from'.  A zero-length copy is still an allocation. */
static inline void *zmemdup(const void *from, size_t bytes) {
    if (!from) {
        return NULL;
    }

    /* @own: caller; free=zfree */
    void *to = zmalloc(bytes ? bytes : 1);
    memcpy(to, from, bytes);
    return to;
}

#ifndef COUNT_ARRAY
#define COUNT_ARRAY(x) (sizeof(x) / sizeof(*(x)))
#endif
