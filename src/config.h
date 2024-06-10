#pragma once

/* GCC / GNUC version checker helpers */
#define _DK_MK_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))

#if __GNUC__
#define DK_GNUC_VERSION_CODE                                                   \
    _DK_MK_VERSION(__GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__)
#define DK_GNUC_GTE(a, b, c) (DK_GNUC_VERSION_CODE >= _DK_MK_VERSION(a, b, c))
#define DK_GNUC_LT(a, b, c) (DK_GNUC_VERSION_CODE < _DK_MK_VERSION(a, b, c))
#else
#define DK_GNUC_GTE(a, b, c) 0
#define DK_GNUC_LT(a, b, c) 0
#endif

#if __clang__
#define DK_CLANG_VERSION_CODE                                                  \
    _DK_MK_VERSION(__clang_major__, __clang_minor__, __clang_patchlevel__)
#endif

/* Are we on a 64 bit, 32 bit, or unknown-bits system? */
static const char DK_BITS = sizeof(void *) == 8   ? 64
                            : sizeof(void *) == 4 ? 32
                                                  : -1;

/* ====================================================================
 * Platform checkers originally from numpy dk_os.h
 * ==================================================================== */
#if defined(linux) || defined(__linux) || defined(__linux__)
#define DK_OS_LINUX 1
#include <linux/version.h>
#if defined(__ANDROID__)
#define DK_OS_ANDROID 1
#endif
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) ||     \
    defined(__OpenBSD__) || defined(__DragonFly__)
#define DK_OS_BSD 1
#if defined(__APPLE__)
#define DK_OS_APPLE 1
#include <TargetConditionals.h>
#if TARGET_IPHONE_SIMULATOR
#define DK_OS_APPLE_IPHONE_SIMULATOR 1
#elif TARGET_OS_IPHONE
#define DK_OS_APPLE_IPHONE 1
#elif TARGET_OS_MAC
#define DK_OS_APPLE_MAC 1
#else
#error "Unknown Apple platform"
#endif
#elif defined(__FreeBSD__)
#define DK_OS_FREEBSD 1
#elif defined(__NetBSD__)
#define DK_OS_NETBSD 1
#elif defined(__OpenBSD__)
#define DK_OS_OPENBSD 1
#elif defined(__DragonFly__)
#define DK_OS_DRAGONFLY 1
#endif
#elif defined(sun) || defined(__sun)
#define DK_OS_SOLARIS 1
#elif defined(__CYGWIN__)
#define DK_OS_CYGWIN 1
#elif defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#define DK_OS_WIN32 1
#elif defined(_AIX)
#define DK_OS_AIX 1
#else
#warning "Couldn't detect operating system during build."
#warning "Feature detection of OS APIs probably won't work at all."
#define DK_OS_UNKNOWN
#endif

/* Test for proc filesystem */
#ifdef DK_OS_LINUX
#define HAVE_PROC_STAT 1
#define HAVE_PROC_MAPS 1
#define HAVE_PROC_SMAPS 1
#define HAVE_PROC_SOMAXCONN 1
#endif

/* Test for task_info() */
#ifdef DK_OS_APPLE
#define HAVE_TASKINFO 1
#endif

/* Test for backtrace() */
#if DK_OS_APPLE || (DK_OS_LINUX && defined(__GLIBC__))
#define HAVE_BACKTRACE 1
#endif

/* These are obviously all *compile time* checks.
 * Use uname(3) for runtime version checks. */
#if DK_OS_LINUX
/* MSG_NOSIGNAL was introduced in 2.2 (1999-01-25) and standardized
 * in POSIX.1-2008, so we don't bother with a version check. */
#define HAVE_MSG_NOSIGNAL 1

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 0, 0)
#define HAVE_SENDMMSG 1
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0)
#define HAVE_REUSEPORT 1
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 13, 0)
/* Technically in 3.6 (client) and 3.7 (server), but it was only enabled
 * by default (for client mode) as late as 3.13 */
#define HAVE_TFO 1
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0)
/* Get your act together, Linux. You've been implementing
 * parts of TFO across four different release versions! */
#define HAVE_TFO_IPV6 1
#endif
#endif

#if DK_OS_OPENBSD
/* OpenBSD has has REUSEPORT since 2007. OG REUSEPORT. */
#define HAVE_REUSEPORT
#endif

#if DK_OS_LINUX
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 11) && __GLIBC_PREREQ(2, 6)
#define HAVE_SYNC_FILE_RANGE 1
#endif
#else
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 11)
#define HAVE_SYNC_FILE_RANGE 1
#endif
#endif
#endif

/* ====================================================================
 * Constants from numpy dk_math.h
 * ==================================================================== */
#define DK_E 2.718281828459045235360287471352662498       /* e */
#define DK_LOG2E 1.442695040888963407359924681001892137   /* log_2 e */
#define DK_LOG10E 0.434294481903251827651128918916605082  /* log_10 e */
#define DK_LOGE2 0.693147180559945309417232121458176568   /* log_e 2 */
#define DK_LOGE10 2.302585092994045684017991454684364208  /* log_e 10 */
#define DK_PI 3.141592653589793238462643383279502884      /* pi */
#define DK_PI_2 1.570796326794896619231321691639751442    /* pi/2 */
#define DK_PI_4 0.785398163397448309615660845819875721    /* pi/4 */
#define DK_1_PI 0.318309886183790671537767526745028724    /* 1/pi */
#define DK_2_PI 0.636619772367581343075535053490057448    /* 2/pi */
#define DK_EULER 0.577215664901532860606512090082402431   /* Euler constant */
#define DK_SQRT2 1.414213562373095048801688724209698079   /* sqrt(2) */
#define DK_SQRT1_2 0.707106781186547524400844362104849039 /* 1/sqrt(2) */

#define DK_Ef 2.718281828459045235360287471352662498F       /* e */
#define DK_LOG2Ef 1.442695040888963407359924681001892137F   /* log_2 e */
#define DK_LOG10Ef 0.434294481903251827651128918916605082F  /* log_10 e */
#define DK_LOGE2f 0.693147180559945309417232121458176568F   /* log_e 2 */
#define DK_LOGE10f 2.302585092994045684017991454684364208F  /* log_e 10 */
#define DK_PIf 3.141592653589793238462643383279502884F      /* pi */
#define DK_PI_2f 1.570796326794896619231321691639751442F    /* pi/2 */
#define DK_PI_4f 0.785398163397448309615660845819875721F    /* pi/4 */
#define DK_1_PIf 0.318309886183790671537767526745028724F    /* 1/pi */
#define DK_2_PIf 0.636619772367581343075535053490057448F    /* 2/pi */
#define DK_EULERf 0.577215664901532860606512090082402431F   /* Euler constan*/
#define DK_SQRT2f 1.414213562373095048801688724209698079F   /* sqrt(2) */
#define DK_SQRT1_2f 0.707106781186547524400844362104849039F /* 1/sqrt(2) */

#define DK_El 2.718281828459045235360287471352662498L       /* e */
#define DK_LOG2El 1.442695040888963407359924681001892137L   /* log_2 e */
#define DK_LOG10El 0.434294481903251827651128918916605082L  /* log_10 e */
#define DK_LOGE2l 0.693147180559945309417232121458176568L   /* log_e 2 */
#define DK_LOGE10l 2.302585092994045684017991454684364208L  /* log_e 10 */
#define DK_PIl 3.141592653589793238462643383279502884L      /* pi */
#define DK_PI_2l 1.570796326794896619231321691639751442L    /* pi/2 */
#define DK_PI_4l 0.785398163397448309615660845819875721L    /* pi/4 */
#define DK_1_PIl 0.318309886183790671537767526745028724L    /* 1/pi */
#define DK_2_PIl 0.636619772367581343075535053490057448L    /* 2/pi */
#define DK_EULERl 0.577215664901532860606512090082402431L   /* Euler constan*/
#define DK_SQRT2l 1.414213562373095048801688724209698079L   /* sqrt(2) */
#define DK_SQRT1_2l 0.707106781186547524400844362104849039L /* 1/sqrt(2) */

/* ====================================================================
 * Conversion helpers
 * ==================================================================== */
#define DK_RAD2DEG_CONST (180.0 / DK_PI)
#define DK_DEG2RAD_CONST (DK_PI / 180.0)

#define DK_RAD2DEG(x) ((x) * DK_RAD2DEG_CONST)
#define DK_DEG2RAD(x) ((x) * DK_DEG2RAD_CONST)

/* ====================================================================
 * Float helpers from numpy numpy_math.h
 * ==================================================================== */
#if 0
#define DK_EXP754 (((uint64_t)0x7ff) << 52)
#define DK_MAN754 ((((uint64_t)1) << 52) - 1)
#define DK_IsNaN(X) (((X) & EXP754) == EXP754 && ((X) & MAN754) != 0)
#endif

#ifndef DK_HAS_ISNAN
#define dk_isnan(x) ((x) != (x))
#else
#if defined(_MSC_VER) && (_MSC_VER < 1900)
#define dk_isnan(x) _isnan((x))
#else
#define dk_isnan(x) isnan(x)
#endif
#endif

#ifndef DK_HAS_ISFINITE
#ifdef _MSC_VER
#define dk_isfinite(x) _finite((x))
#else
#define dk_isfinite(x) !dk_isnan((x) + (-(x)))
#endif
#else
#define dk_isfinite(x) isfinite((x))
#endif

#ifndef DK_HAS_ISINF
#define dk_isinf(x) (!dk_isfinite(x) && !dk_isnan(x))
#else
#if defined(_MSC_VER) && (_MSC_VER < 1900)
#define dk_isinf(x) (!_finite((x)) && !_isnan((x)))
#else
#define dk_isinf(x) isinf((x))
#endif
#endif

/* ====================================================================
 * Old backwards and inept contributions
 * ==================================================================== */
/* Includes parts of "config.h" from:
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/* Includes parts of numpy includes:
 * Copyright (c) 2005-2016, NumPy Developers.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *
 *     * Neither the name of the NumPy Developers nor the names of any
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
