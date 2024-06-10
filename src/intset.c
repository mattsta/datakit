#include "intset.h"
#include "datakit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Return the required encoding for the provided value. */
static uint8_t intsetValueEncoding_(int64_t v) {
    if (v < INT32_MIN || v > INT32_MAX) {
        return INTSET_ENC_INT64;
    }

    if (v < INT16_MIN || v > INT16_MAX) {
        return INTSET_ENC_INT32;
    }

    return INTSET_ENC_INT16;
}

/* Return the value at pos, given an encoding. */
static int64_t intsetGetEncoded_(const intset *is, const size_t pos,
                                 const intsetEnc enc) {
    if (enc == INTSET_ENC_INT64) {
        return ((int64_t *)(is->contents))[pos];
    }

    if (enc == INTSET_ENC_INT32) {
        return ((int32_t *)(is->contents))[pos];
    }

    return ((int16_t *)(is->contents))[pos];
}

/* Return the value at pos, using the configured encoding. */
static int64_t intsetGet_(const intset *is, const size_t pos) {
    return intsetGetEncoded_(is, pos, is->encoding);
}

/* Set the value at pos, using the configured encoding. */
static void intsetSet_(intset *is, const size_t pos, const int64_t value) {
    const intsetEnc encoding = is->encoding;

    if (encoding == INTSET_ENC_INT64) {
        ((int64_t *)is->contents)[pos] = value;
    } else if (encoding == INTSET_ENC_INT32) {
        ((int32_t *)is->contents)[pos] = value;
    } else {
        ((int16_t *)is->contents)[pos] = value;
    }
}

/* Create an empty intset. */
intset *intsetNew(void) {
    intset *is = zcalloc(1, sizeof(*is));
    is->encoding = INTSET_ENC_INT16;
    return is;
}

void intsetFree(intset *is) {
    if (is) {
        zfree(is);
    }
}

/* Resize the intset */
static void intsetResize(intset **is, uint32_t len) {
    const size_t size = len * (*is)->encoding;
    *is = zrealloc(*is, sizeof(intset) + size);
}

/* Search for the position of "value". Return 1 when the value was found and
 * sets "pos" to the position of the value within the intset. Return 0 when
 * the value is not present in the intset and sets "pos" to the position
 * where "value" can be inserted. */
static bool intsetSearch(intset *is, int64_t value, uint32_t *pos) {
    int min = 0;
    int max = is->length - 1;
    int mid = -1;
    int64_t cur = -1;

    /* The value can never be found when the set is empty */
    if (is->length == 0) {
        if (pos) {
            *pos = 0;
        }

        return false;
    }

    /* Check for the case where we know we cannot find the value,
     * but do know the insert position. */
    if (value > intsetGet_(is, is->length - 1)) {
        if (pos) {
            *pos = is->length;
        }

        return false;
    } else if (value < intsetGet_(is, 0)) {
        if (pos) {
            *pos = 0;
        }

        return false;
    }

    while (max >= min) {
        mid = ((unsigned int)min + (unsigned int)max) >> 1;
        cur = intsetGet_(is, mid);
        if (value > cur) {
            min = mid + 1;
        } else if (value < cur) {
            max = mid - 1;
        } else {
            if (pos) {
                *pos = mid;
            }

            return true;
        }
    }

    if (pos) {
        *pos = min;
    }

    return false;
}

/* Upgrades the intset to a larger encoding and inserts the given integer. */
static void intsetUpgradeAndAdd(intset **is, int64_t value) {
    uint8_t curenc = (*is)->encoding;
    uint8_t newenc = intsetValueEncoding_(value);
    int length = (*is)->length;
    int prepend = value < 0 ? 1 : 0;

    /* First set new encoding and resize */
    (*is)->encoding = newenc;
    intsetResize(is, length + 1);

    /* Upgrade back-to-front so we don't overwrite values.
     * Note that the "prepend" variable is used to make sure we have an empty
     * space at either the beginning or the end of the intset. */
    while (length--) {
        intsetSet_(*is, length + prepend,
                   intsetGetEncoded_(*is, length, curenc));
    }

    /* Set the value at the beginning or the end. */
    if (prepend) {
        intsetSet_(*is, 0, value);
    } else {
        intsetSet_(*is, (*is)->length, value);
    }

    (*is)->length++;
}

static void intsetMoveTail(intset *is, uint32_t from, uint32_t to) {
    void *src, *dst;
    uint32_t bytes = is->length - from;
    uint32_t encoding = is->encoding;

    if (encoding == INTSET_ENC_INT64) {
        src = (int64_t *)is->contents + from;
        dst = (int64_t *)is->contents + to;
        bytes *= sizeof(int64_t);
    } else if (encoding == INTSET_ENC_INT32) {
        src = (int32_t *)is->contents + from;
        dst = (int32_t *)is->contents + to;
        bytes *= sizeof(int32_t);
    } else {
        src = (int16_t *)is->contents + from;
        dst = (int16_t *)is->contents + to;
        bytes *= sizeof(int16_t);
    }

    memmove(dst, src, bytes);
}

/* Insert an integer in the intset */
void intsetAdd(intset **is, int64_t value, bool *success) {
    const uint_fast8_t valenc = intsetValueEncoding_(value);
    uint32_t pos;
    if (success) {
        *success = true;
    }

    /* Upgrade encoding if necessary. If we need to upgrade, we know that
     * this value should be either appended (if > 0) or prepended (if < 0),
     * because it lies outside the range of existing values. */
    if (valenc > (*is)->encoding) {
        /* This always succeeds, so we don't need to curry *success. */
        return intsetUpgradeAndAdd(is, value);
    }

    /* Abort if the value is already present in the set.
     * This call will populate "pos" with the right position to insert
     * the value when it cannot be found. */
    if (intsetSearch(*is, value, &pos)) {
        if (success) {
            *success = false;
        }

        return;
    }

    intsetResize(is, (*is)->length + 1);
    if (pos < (*is)->length) {
        intsetMoveTail(*is, pos, pos + 1);
    }

    intsetSet_(*is, pos, value);
    (*is)->length++;
}

/* Delete integer from intset */
void intsetRemove(intset **is, int64_t value, bool *success) {
    uint8_t valenc = intsetValueEncoding_(value);
    uint32_t pos;
    if (success) {
        *success = false;
    }

    if (valenc <= (*is)->encoding && intsetSearch(*is, value, &pos)) {
        uint32_t len = (*is)->length;

        /* We know we can delete */
        if (success) {
            *success = true;
        }

        /* Overwrite value with tail and update length */
        if (pos < (len - 1)) {
            intsetMoveTail(*is, pos + 1, pos);
        }

        intsetResize(is, len - 1);
        (*is)->length = len - 1;
    }
}

/* Determine whether a value belongs to this set */
bool intsetFind(intset *is, int64_t value) {
    uint8_t valenc = intsetValueEncoding_(value);
    return valenc <= is->encoding && intsetSearch(is, value, NULL);
}

/* Return random member */
int64_t intsetRandom(intset *is) {
    return intsetGet_(is, random() % is->length);
}

/* Get the value at the given position. When this position is
 * out of range the function returns 0, when in range it returns 1. */
bool intsetGet(intset *is, uint32_t pos, int64_t *value) {
    if (pos < is->length) {
        *value = intsetGet_(is, pos);
        return true;
    }

    return false;
}

/* Return intset length */
size_t intsetCount(const intset *is) {
    return is->length;
}

/* Return intset blob size in bytes. */
size_t intsetBytes(intset *is) {
    return sizeof(intset) + is->length * is->encoding;
}

#ifdef DATAKIT_TEST
#include <sys/time.h>
#include <time.h>

void intsetRepr(const intset *is) {
    printf("[");
    for (uint32_t i = 0; i < is->length; i++) {
        printf("%" PRId64 ", ", intsetGet_(is, i));
    }

    printf("]\n");
}

#if 0
static void error(char *err) {
    printf("%s\n", err);
    exit(1);
}

#endif

static void ok(void) {
    printf("OK\n");
}

static long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (((long long)tv.tv_sec) * 1000000) + tv.tv_usec;
}

#define intsetAssert(_e)                                                       \
    ((_e) ? (void)0 : (_intsetAssert(#_e, __FILE__, __LINE__), abort()))
static void _intsetAssert(char *estr, char *file, int line) {
    printf("\n\n=== intsetAssertION FAILED ===\n");
    printf("==> %s:%d '%s' is not true\n", file, line, estr);
}

static intset *createSet(int bits, int size) {
    uint64_t mask = (1 << bits) - 1;
    uint64_t value;
    intset *is = intsetNew();

    for (int i = 0; i < size; i++) {
        if (bits > 32) {
            value = (random() * random()) & mask;
        } else {
            value = random() & mask;
        }

        intsetAdd(&is, value, NULL);
    }

    return is;
}

static void checkConsistency(intset *is) {
    for (uint32_t i = 0; i < is->length - 1; i++) {
        uint32_t encoding = is->encoding;

        if (encoding == INTSET_ENC_INT16) {
            int16_t *i16 = (int16_t *)is->contents;
            intsetAssert(i16[i] < i16[i + 1]);
        } else if (encoding == INTSET_ENC_INT32) {
            int32_t *i32 = (int32_t *)is->contents;
            intsetAssert(i32[i] < i32[i + 1]);
        } else {
            int64_t *i64 = (int64_t *)is->contents;
            intsetAssert(i64[i] < i64[i + 1]);
        }
    }
}

#define UNUSED(x) (void)(x)
int intsetTest(int argc, char **argv) {
    bool success;
    intset *is;
    srandom(time(NULL));

    UNUSED(argc);
    UNUSED(argv);

    printf("Value encodings: ");
    {
        intsetAssert(intsetValueEncoding_(-32768) == INTSET_ENC_INT16);
        intsetAssert(intsetValueEncoding_(+32767) == INTSET_ENC_INT16);
        intsetAssert(intsetValueEncoding_(-32769) == INTSET_ENC_INT32);
        intsetAssert(intsetValueEncoding_(+32768) == INTSET_ENC_INT32);
        intsetAssert(intsetValueEncoding_(-2147483648) == INTSET_ENC_INT32);
        intsetAssert(intsetValueEncoding_(+2147483647) == INTSET_ENC_INT32);
        intsetAssert(intsetValueEncoding_(-2147483649) == INTSET_ENC_INT64);
        intsetAssert(intsetValueEncoding_(+2147483648) == INTSET_ENC_INT64);
        intsetAssert(intsetValueEncoding_(-9223372036854775808ull) ==
                     INTSET_ENC_INT64);
        intsetAssert(intsetValueEncoding_(+9223372036854775807ull) ==
                     INTSET_ENC_INT64);
        ok();
    }

    printf("Basic adding: ");
    {
        is = intsetNew();
        intsetAssert(is);
        intsetAdd(&is, 5, &success);
        intsetAssert(is);
        intsetAssert(success);
        intsetAdd(&is, 6, &success);
        intsetAssert(is);
        intsetAssert(success);
        intsetAdd(&is, 4, &success);
        intsetAssert(is);
        intsetAssert(success);
        intsetAdd(&is, 4, &success);
        intsetAssert(is);
        intsetAssert(!success);
        ok();
    }

    printf("Large number of random adds: ");
    {
        uint32_t inserts = 0;
        is = intsetNew();
        for (int i = 0; i < 1024; i++) {
            intsetAdd(&is, random() % 0x800, &success);
            if (success) {
                inserts++;
            }
        }

        intsetAssert(is->length == inserts);
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int16 to int32: ");
    {
        is = intsetNew();
        intsetAdd(&is, 32, NULL);
        intsetAssert(is->encoding == INTSET_ENC_INT16);
        intsetAdd(&is, 65535, NULL);
        intsetAssert(is->encoding == INTSET_ENC_INT32);
        intsetAssert(intsetFind(is, 32));
        intsetAssert(intsetFind(is, 65535));
        checkConsistency(is);

        is = intsetNew();
        intsetAdd(&is, 32, NULL);
        intsetAssert(is->encoding == INTSET_ENC_INT16);
        intsetAdd(&is, -65535, NULL);
        intsetAssert(is->encoding == INTSET_ENC_INT32);
        intsetAssert(intsetFind(is, 32));
        intsetAssert(intsetFind(is, -65535));
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int16 to int64: ");
    {
        is = intsetNew();
        intsetAdd(&is, 32, NULL);
        intsetAssert(is->encoding == INTSET_ENC_INT16);
        intsetAdd(&is, 4294967295, NULL);
        intsetAssert(is->encoding == INTSET_ENC_INT64);
        intsetAssert(intsetFind(is, 32));
        intsetAssert(intsetFind(is, 4294967295));
        checkConsistency(is);

        is = intsetNew();
        intsetAdd(&is, 32, NULL);
        intsetAssert(is->encoding == INTSET_ENC_INT16);
        intsetAdd(&is, -4294967295, NULL);
        intsetAssert(is->encoding == INTSET_ENC_INT64);
        intsetAssert(intsetFind(is, 32));
        intsetAssert(intsetFind(is, -4294967295));
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int32 to int64: ");
    {
        is = intsetNew();
        intsetAdd(&is, 65535, NULL);
        intsetAssert(is->encoding == INTSET_ENC_INT32);
        intsetAdd(&is, 4294967295, NULL);
        intsetAssert(is->encoding == INTSET_ENC_INT64);
        intsetAssert(intsetFind(is, 65535));
        intsetAssert(intsetFind(is, 4294967295));
        checkConsistency(is);

        is = intsetNew();
        intsetAdd(&is, 65535, NULL);
        intsetAssert(is->encoding == INTSET_ENC_INT32);
        intsetAdd(&is, -4294967295, NULL);
        intsetAssert(is->encoding == INTSET_ENC_INT64);
        intsetAssert(intsetFind(is, 65535));
        intsetAssert(intsetFind(is, -4294967295));
        checkConsistency(is);
        ok();
    }

    printf("Stress lookups: ");
    {
        long num = 100000, size = 10000;
        int i, bits = 20;
        long long start;
        is = createSet(bits, size);
        checkConsistency(is);

        start = usec();
        for (i = 0; i < num; i++) {
            intsetSearch(is, random() % ((1 << bits) - 1), NULL);
        }
        printf("%ld lookups, %ld element set, %lldusec\n", num, size,
               usec() - start);
    }

    printf("Stress add+delete: ");
    {
        int i, v1, v2;
        is = intsetNew();
        for (i = 0; i < 0xffff; i++) {
            v1 = random() % 0xfff;
            intsetAdd(&is, v1, NULL);
            intsetAssert(intsetFind(is, v1));

            v2 = random() % 0xfff;
            intsetRemove(&is, v2, NULL);
            intsetAssert(!intsetFind(is, v2));
        }

        checkConsistency(is);
        ok();
    }

    return 0;
}

#endif

/* This is modified and improved version of original malfunctions introduced by:
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
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
