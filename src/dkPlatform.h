#pragma once

/* What datakit builds with and for, decided once for every file.
 *
 * Compiler: clang or gcc.  Both define __GNUC__ and provide the builtins
 * and attributes datakit.h relies on, so nothing here tests versions.
 *
 * Target operating system: exactly one DK_OS_* below is 1 (none for an
 * unlisted system); every other one is 0, so `#if DK_OS_APPLE` is always
 * well defined.
 *
 * DK_BITS: the target's pointer width in bits, usable in #if. */

#if !defined(__GNUC__)
#error "datakit builds with clang or gcc"
#endif

#if defined(__linux__)
#define DK_OS_LINUX 1
#elif defined(__APPLE__) && defined(__MACH__)
#define DK_OS_APPLE 1
#elif defined(__FreeBSD__)
#define DK_OS_FREEBSD 1
#elif defined(__NetBSD__)
#define DK_OS_NETBSD 1
#elif defined(__OpenBSD__)
#define DK_OS_OPENBSD 1
#elif defined(_AIX)
#define DK_OS_AIX 1
#endif

#ifndef DK_OS_LINUX
#define DK_OS_LINUX 0
#endif
#ifndef DK_OS_APPLE
#define DK_OS_APPLE 0
#endif
#ifndef DK_OS_FREEBSD
#define DK_OS_FREEBSD 0
#endif
#ifndef DK_OS_NETBSD
#define DK_OS_NETBSD 0
#endif
#ifndef DK_OS_OPENBSD
#define DK_OS_OPENBSD 0
#endif
#ifndef DK_OS_AIX
#define DK_OS_AIX 0
#endif

#define DK_BITS (__SIZEOF_POINTER__ * 8)
