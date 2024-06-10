/* dks — a modernized modern string buffer
 *
 * Copyright 2016-2019 Matt Stancliff <matt@genges.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "datakit.h"

#include <assert.h>
#include <limits.h> /* LLONG_MIN, etc */
#include <stdarg.h> /* va_start / va_copy / va_end */
#include <stdio.h>  /* snprintf */
__BEGIN_DECLS

#if defined(DATAKIT_DKS_FULL) || defined(DATAKIT_DKS_FULL_HEADER)
typedef char mds;
#define DKS_TYPE mds
#elif defined(DATAKIT_DKS_COMPACT) || defined(DATAKIT_DKS_COMPACT_HEADER)
typedef char mdsc;
#define DKS_TYPE mdsc
#else
#error "Must provide type when requesting dks."
#endif

/* Public function names */
#define DKS_NEWLEN DK_NAME(DKS_TYPE, newlen)
#define DKS_NEW DK_NAME(DKS_TYPE, new)
#define DKS_NEWFROMFILE DK_NAME(DKS_TYPE, NewFromFile)
#define DKS_XXH64 DK_NAME(DKS_TYPE, XXH64)
#define DKS_XXH3_64 DK_NAME(DKS_TYPE, XXH3_64)
#define DKS_XXH3_128 DK_NAME(DKS_TYPE, XXH3_128)
#define DKS_EMPTY DK_NAME(DKS_TYPE, empty)
#define DKS_EMPTYLEN DK_NAME(DKS_TYPE, emptylen)
#define DKS_DUP DK_NAME(DKS_TYPE, dup)
#define DKS_FREE DK_NAME(DKS_TYPE, free)
#define DKS_FREEZERO DK_NAME(DKS_TYPE, freezero)
#define DKS_NATIVE DK_NAME(DKS_TYPE, native)
#define DKS_GROWZERO DK_NAME(DKS_TYPE, growzero)
#define DKS_CATLEN DK_NAME(DKS_TYPE, catlen)
#define DKS_CATLEN_CHECK_COMMA DK_NAME(DKS_TYPE, catlencheckcomma)
#define DKS_CATLEN_QUOTE_RAW DK_NAME(DKS_TYPE, catlenquoteraw)
#define DKS_CATLEN_NOQUOTE_RAW DK_NAME(DKS_TYPE, catlennoquoteraw)
#define DKS_PREPENDLEN DK_NAME(DKS_TYPE, prependlen)
#define DKS_CAT DK_NAME(DKS_TYPE, cat)
#define DKS_CATANOTHER DK_NAME(DKS_TYPE, DK_NAME(cat, DKS_TYPE))
#define DKS_COPY DK_NAME(DKS_TYPE, copy)
#define DKS_COPYLEN DK_NAME(DKS_TYPE, copylen)
#define DKS_LEN DK_NAME(DKS_TYPE, len)
#define DKS_AVAIL DK_NAME(DKS_TYPE, avail)
#define DKS_BUFALLOCSIZE DK_NAME(DKS_TYPE, bufallocsize)
#define DKS_CATVPRINTF DK_NAME(DKS_TYPE, catvprintf)
#define DKS_CATPRINTF DK_NAME(DKS_TYPE, catprintf)
#define DKS_CATFMT DK_NAME(DKS_TYPE, catfmt)
#define DKS_TRIM DK_NAME(DKS_TYPE, trim)
#define DKS_RANGE DK_NAME(DKS_TYPE, range)
#define DKS_SUBSTR DK_NAME(DKS_TYPE, substr)
#define DKS_SUBSTR_UTF8 DK_NAME(DKS_TYPE, substrutf8)
#define DKS_UPDATELEN DK_NAME(DKS_TYPE, updatelen)
#define DKS_UPDATELENFORCE DK_NAME(DKS_TYPE, updatelenforce)
#define DKS_CLEAR DK_NAME(DKS_TYPE, clear)
#define DKS_CMP DK_NAME(DKS_TYPE, cmp)
#define DKS_SPLITLEN DK_NAME(DKS_TYPE, splitlen)
#define DKS_SPLITLEN_MAX DK_NAME(DKS_TYPE, splitlenMax)
#define DKS_SPLITLENFREE DK_NAME(DKS_TYPE, splitlenfree)
#define DKS_TOLOWER DK_NAME(DKS_TYPE, tolower)
#define DKS_TOUPPER DK_NAME(DKS_TYPE, toupper)
#define DKS_CATREPR DK_NAME(DKS_TYPE, catrepr)
#define DKS_SPLITARGS DK_NAME(DKS_TYPE, splitargs)
#define DKS_MAPCHARS DK_NAME(DKS_TYPE, mapchars)
#define DKS_JOIN DK_NAME(DKS_TYPE, join)
#define DKS_EXPANDBY DK_NAME(DKS_TYPE, expandby)
#define DKS_EXPANDBYEXACT DK_NAME(DKS_TYPE, expandbyexact)
#define DKS_INCRLEN DK_NAME(DKS_TYPE, IncrLen)
#define DKS_REMOVEFREESPACE DK_NAME(DKS_TYPE, removefreespace)
#define DKS_ALLOCSIZE DK_NAME(DKS_TYPE, allocsize)
#define DKS_GETABSOLUTEPATH DK_NAME(DKS_TYPE, getabsolutepath)
#define DKS_FORMATIP DK_NAME(DKS_TYPE, formatip)
#define DKS_LENUTF8 DK_NAME(DKS_TYPE, lenutf8)
#define DKS_FROMINT64 DK_NAME(DKS_TYPE, fromint64)
#define DKS_TEST DK_NAME(DKS_TYPE, Test)
#define DKS_BENCHMAIN DK_NAME(DKS_TYPE, BenchMain)

/* Static function names */
#define DKS_CONTAINERTYPE DK_NAME(DKS_TYPE, ContainerType)
#define DKS_BASE DK_NAME(DKS_TYPE, base)
#define DKS_SETPREVIOUSINTEGERANDTYPE                                          \
    DK_NAME(DKS_TYPE, SetPreviousIntegerAndType)
#define DKS_GETPREVIOUSINTEGERWITHTYPEREMOVED                                  \
    DK_NAME(DKS_TYPE, GetPreviousIntegerWithTypeRemoved)
#define DKS_MAXSHAREDFORTYPE DK_NAME(DKS_TYPE, MaxSharedForType)
#define DKS_PREVIOUSINTEGERWITHTYPE DK_NAME(DKS_TYPE, PreviousIntegerWithType)
#define DKS_INFOEXPANDBY DK_NAME(DKS_TYPE, InfoExpandBy)
#define DKS_INFOUPDATELENFREE DK_NAME(DKS_TYPE, InfoUpdateLenFree)
#define DKS__INFOUPDATELENFREE DK_NAME(DKS_TYPE, _InfoUpdateLenFree)
#define DKS_INFOUPDATELENFREENOTERMINATE                                       \
    DK_NAME(DKS_TYPE, InfoUpdateLenFreeNoTerminate)
#define DKS_INCREASELENGTHBY DK_NAME(DKS_TYPE, IncreaseLengthBy)
#define DKS_INCREASELENGTHBYNOTERMINATE                                        \
    DK_NAME(DKS_TYPE, IncreaseLengthByNoTerminate)
#define DKS_UPGRADEFREEALLOCATION DK_NAME(DKS_TYPE, UpgradeFreeAllocation)
#define DKS_U64TOSTR DK_NAME(DKS_TYPE, u64toStr)
#define DKS_ISHEXDIGIT DK_NAME(DKS_TYPE, IsHexDigit)       /* breakout */
#define DKS_HEXDIGITTOINT DK_NAME(DKS_TYPE, HexDigitToInt) /* breakout */
#define DKS_TYPENAME DK_NAME(DKS_TYPE, TypeName)
#define DKS_RANDBYTES DK_NAME(DKS_TYPE, randbytes)

/* Exported functions */
DKS_TYPE *DKS_NEWLEN(const void *init, size_t initlen);
DKS_TYPE *DKS_NEW(const char *init);
DKS_TYPE *DKS_NEWFROMFILE(const char *filename);
uint64_t DKS_XXH64(const DKS_TYPE *s, const uint64_t seed);
uint64_t DKS_XXH3_64(const DKS_TYPE *s, const uint64_t seed);
__uint128_t DKS_XXH3_128(const DKS_TYPE *s, const uint64_t seed);
DKS_TYPE *DKS_EMPTY(void);
DKS_TYPE *DKS_EMPTYLEN(size_t len);
DKS_TYPE *DKS_DUP(const DKS_TYPE *s);
void DKS_FREE(DKS_TYPE *s);
void DKS_FREEZERO(DKS_TYPE *s);
uint8_t *DKS_NATIVE(DKS_TYPE *s, size_t *len);
DKS_TYPE *DKS_GROWZERO(DKS_TYPE *s, size_t len);
DKS_TYPE *DKS_CATLEN(DKS_TYPE *s, const void *t, size_t len);
DKS_TYPE *DKS_CATLEN_CHECK_COMMA(DKS_TYPE *s, const void *t, size_t len);
DKS_TYPE *DKS_CATLEN_QUOTE_RAW(DKS_TYPE *s, const void *t, size_t len,
                               uint8_t append);
DKS_TYPE *DKS_CATLEN_NOQUOTE_RAW(DKS_TYPE *s, const void *t, size_t len,
                                 uint8_t append);
DKS_TYPE *DKS_PREPENDLEN(DKS_TYPE *s, const void *t, const size_t len);
DKS_TYPE *DKS_CAT(DKS_TYPE *s, const char *t);
DKS_TYPE *DKS_CATANOTHER(DKS_TYPE *s, const DKS_TYPE *t);
DKS_TYPE *DKS_COPYLEN(DKS_TYPE *s, const char *t, size_t len);
DKS_TYPE *DKS_COPY(DKS_TYPE *s, const char *t);
size_t DKS_LEN(const DKS_TYPE *s);
size_t DKS_AVAIL(const DKS_TYPE *s);
size_t DKS_BUFALLOCSIZE(const DKS_TYPE *s);

DKS_TYPE *DKS_CATVPRINTF(DKS_TYPE *s, const char *fmt, va_list ap);
#ifdef __GNUC__
DKS_TYPE *DKS_CATPRINTF(DKS_TYPE *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
DKS_TYPE *DKS_CATPRINTF(DKS_TYPE *s, const char *fmt, ...);
#endif

DKS_TYPE *DKS_CATFMT(DKS_TYPE *s, char const *fmt, ...);
DKS_TYPE *DKS_TRIM(DKS_TYPE *s, const char *cset);
void DKS_RANGE(DKS_TYPE *s, ssize_t start, ssize_t end);
void DKS_SUBSTR(DKS_TYPE *s, size_t start, size_t length);
void DKS_SUBSTR_UTF8(DKS_TYPE *s, size_t start, size_t length);
void DKS_UPDATELEN(DKS_TYPE *s);
void DKS_UPDATELENFORCE(DKS_TYPE *s, size_t newlen);
void DKS_CLEAR(DKS_TYPE *s);
int DKS_CMP(const DKS_TYPE *s1, const DKS_TYPE *s2);
DKS_TYPE **DKS_SPLITLEN(const void *s, size_t len, const void *sep,
                        size_t seplen, size_t *count);
DKS_TYPE **DKS_SPLITLEN_MAX(const void *s, size_t len, const void *sep,
                            size_t seplen, size_t *count, size_t maxCount);
void DKS_SPLITLENFREE(DKS_TYPE **tokens, size_t count);
void DKS_TOLOWER(DKS_TYPE *s);
void DKS_TOUPPER(DKS_TYPE *s);
DKS_TYPE *DKS_CATREPR(DKS_TYPE *s, const char *p, size_t len);
DKS_TYPE **DKS_SPLITARGS(const char *line, int *argc);
void DKS_MAPCHARS(DKS_TYPE *s, const char *from, const char *to, size_t setlen);
DKS_TYPE *DKS_JOIN(char **argv, int argc, char *sep);

/* Low level functions exposed to the user API */
DKS_TYPE *DKS_EXPANDBY(DKS_TYPE *s,
                       size_t addlen); /* automatically "guesses" better size */
DKS_TYPE *DKS_EXPANDBYEXACT(DKS_TYPE *s,
                            size_t addlen); /* only grows exactly 'addlen' */
void DKS_INCRLEN(DKS_TYPE *s, ssize_t incr);
DKS_TYPE *DKS_REMOVEFREESPACE(DKS_TYPE *s);
size_t DKS_ALLOCSIZE(DKS_TYPE *s);

/* Single purpose helpers */
DKS_TYPE *DKS_GETABSOLUTEPATH(char *filename);
DKS_TYPE *DKS_FORMATIP(char *ip, int port);
size_t DKS_LENUTF8(DKS_TYPE *s);
DKS_TYPE *DKS_FROMINT64(int64_t value);

#ifdef DATAKIT_TEST
int DKS_TEST(int argc, char *argv[]);
int DKS_BENCHMAIN(void);
#endif

#if defined(DATAKIT_DKS_FULL_HEADER) || defined(DATAKIT_DKS_COMPACT_HEADER)
#undef DKS_TYPE

#undef DATAKIT_DKS_FULL
#undef DATAKIT_DKS_FULL_HEADER
#undef DATAKIT_DKS_COMPACT
#undef DATAKIT_DKS_COMPACT_HEADER

#undef DKS_NEWLEN
#undef DKS_NEW
#undef DKS_NEWFROMFILE
#undef DKS_XXH64
#undef DKS_XXH3_64
#undef DKS_XXH3_128
#undef DKS_EMPTY
#undef DKS_EMPTYLEN
#undef DKS_DUP
#undef DKS_FREE
#undef DKS_FREEZERO
#undef DKS_NATIVE
#undef DKS_GROWZERO
#undef DKS_CATLEN
#undef DKS_CATLEN_CHECK_COMMA
#undef DKS_CATLEN_QUOTE_RAW
#undef DKS_CATLEN_NOQUOTE_RAW
#undef DKS_PREPENDLEN
#undef DKS_CAT
#undef DKS_CATANOTHER
#undef DKS_COPY
#undef DKS_COPYLEN
#undef DKS_LEN
#undef DKS_AVAIL
#undef DKS_BUFALLOCSIZE
#undef DKS_CATVPRINTF
#undef DKS_CATPRINTF
#undef DKS_CATFMT
#undef DKS_TRIM
#undef DKS_RANGE
#undef DKS_SUBSTR
#undef DKS_SUBSTR_UTF8
#undef DKS_UPDATELEN
#undef DKS_UPDATELENFORCE
#undef DKS_CLEAR
#undef DKS_CMP
#undef DKS_SPLITLEN
#undef DKS_SPLITLEN_MAX
#undef DKS_SPLITLENFREE
#undef DKS_TOLOWER
#undef DKS_TOUPPER
#undef DKS_CATREPR
#undef DKS_SPLITARGS
#undef DKS_MAPCHARS
#undef DKS_JOIN
#undef DKS_EXPANDBY
#undef DKS_EXPANDBYEXACT
#undef DKS_INCRLEN
#undef DKS_REMOVEFREESPACE
#undef DKS_ALLOCSIZE
#undef DKS_GETABSOLUTEPATH
#undef DKS_FORMATIP
#undef DKS_LENUTF8
#undef DKS_FROMINT64
#undef DKS_TEST
#undef DKS_BENCHMAIN

#undef DKS_CONTAINERTYPE
#undef DKS_BASE
#undef DKS_SETPREVIOUSINTEGERANDTYPE
#undef DKS_GETPREVIOUSINTEGERWITHTYPEREMOVED
#undef DKS_MAXSHAREDFORTYPE
#undef DKS_PREVIOUSINTEGERWITHTYPE
#undef DKS_INFOEXPANDBY
#undef DKS_INFOUPDATELENFREE
#undef DKS__INFOUPDATELENFREE
#undef DKS_INFOUPDATELENFREENOTERMINATE
#undef DKS_INCREASELENGTHBY
#undef DKS_INCREASELENGTHBYNOTERMINATE
#undef DKS_UPGRADEFREEALLOCATION
#undef DKS_U64TOSTR
#undef DKS_ISHEXDIGIT
#undef DKS_HEXDIGITTOINT
#undef DKS_TYPENAME
#undef DKS_RANDBYTES

#endif
__END_DECLS

/* original mundane sds implementation by:
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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
