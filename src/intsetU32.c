#include "intsetU32.h"
#include "datakit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Return the value at pos, using the configured encoding. */
DK_INLINE_ALWAYS uint32_t intsetU32Get_(const intsetU32 *const is, size_t pos) {
    return is->contents[pos];
}

uint32_t *intsetU32Array(intsetU32 *is) {
    return is->contents;
}

/* Set the value at pos, using the configured encoding. */
DK_INLINE_ALWAYS void intsetU32Set_(intsetU32 *is, size_t pos,
                                    uint_fast32_t value) {
    is->contents[pos] = value;
}

/* Create an empty intsetU32. */
intsetU32 *intsetU32New(void) {
    intsetU32 *is = zcalloc(1, sizeof(*is));
    return is;
}

intsetU32 *intsetU32NewLen(const uint32_t len) {
    intsetU32 *is = zcalloc(1, sizeof(intsetU32) + (sizeof(uint32_t) * len));
    return is;
}

void intsetU32Free(intsetU32 *is) {
    if (is) {
        zfree(is);
    }
}

intsetU32 *intsetU32Copy(const intsetU32 *is) {
    const size_t totalSize = intsetU32Bytes(is);
    intsetU32 *result = zmalloc(totalSize);
    memcpy(result, is, totalSize);
    return result;
}

/* Resize the intsetU32 */
void intsetU32Resize(intsetU32 **is, const uint32_t len) {
    /* TODO: if we are adding many sequential positions, should we
     *       just grow the intsetU32 struct so it has a 'total length'
     *       field so we can perform bulk allocations and continue
     *       writing until count==totalLength, *then* realloc again? */
    *is = zrealloc(*is, sizeof(intsetU32) + (sizeof(uint32_t) * len));
}

void intsetU32ShrinkToSize(intsetU32 **is) {
    *is = zrealloc(*is, sizeof(intsetU32) + (sizeof(uint32_t) * (*is)->count));
}

void intsetU32UpdateCount(intsetU32 *is, const uint32_t len) {
    is->count = len;
}

/* Search for the position of "value".
 * Return true when the value was found.
 *   Sets "pos" to the position of the value within the intsetU32.
 * Return false when value is not present in the intsetU32
 *   Sets "pos" to the position where "value" can be inserted. */
static bool intsetU32Search(const intsetU32 *const is,
                            const uint_fast32_t value, uint32_t *pos) {
    /* The value can never be found when the set is empty */
    if (is->count == 0) {
        if (pos) {
            *pos = 0;
        }

        return false;
    }

    /* Check for the case where we know we cannot find the value,
     * but do know the insert position. */
    if (value > intsetU32Get_(is, is->count - 1)) {
        if (pos) {
            *pos = is->count;
        }

        return false;
    } else if (value < intsetU32Get_(is, 0)) {
        if (pos) {
            *pos = 0;
        }

        return false;
    }

    uint_fast32_t min = 0;
    uint_fast32_t max = is->count - 1;

    while (max >= min) {
        uint_fast32_t mid = (min + max) >> 1;
        uint_fast32_t cur = intsetU32Get_(is, mid);
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

static void intsetU32MoveTail(intsetU32 *is, uint32_t from, uint32_t to) {
    void *src, *dst;
    uint_fast32_t bytes = is->count - from;

    src = is->contents + from;
    dst = is->contents + to;
    bytes *= sizeof(uint32_t);

    memmove(dst, src, bytes);
}

/* Insert an integer in the intsetU32 */
/* Returns 'true' if element is added as new.
 * Returns 'false' if element already existed. */
bool intsetU32Add(intsetU32 **is, uint32_t value) {
    uint32_t pos;

    /* Abort if the value is already present in the set.
     * This call will populate "pos" with the right position to insert
     * the value when it cannot be found. */
    if (intsetU32Search(*is, value, &pos)) {
        return false;
    }

    intsetU32Resize(is, (*is)->count + 1);
    if (pos < (*is)->count) {
        intsetU32MoveTail(*is, pos, pos + 1);
    }

    intsetU32Set_(*is, pos, value);
    (*is)->count++;
    return true;
}

/* Delete integer from intsetU32 */
bool intsetU32Remove(intsetU32 **is, uint32_t value) {
    uint32_t pos;

    if (intsetU32Search(*is, value, &pos)) {
        uint32_t len = (*is)->count;

        /* Overwrite value with tail and update count */
        if (pos < (len - 1)) {
            intsetU32MoveTail(*is, pos + 1, pos);
        }

        intsetU32Resize(is, len - 1);
        (*is)->count = len - 1;

        return true;
    }

    return false;
}

/* Determine whether a value belongs to this set */
bool intsetU32Exists(const intsetU32 *const is, const uint32_t value) {
    return intsetU32Search(is, value, NULL);
}

bool intsetU32Equal(const intsetU32 *const a, const intsetU32 *const b) {
    if (a == b) {
        return true;
    }

    if (a->count == b->count) {
        return memcmp(a->contents, b->contents,
                      sizeof(*a->contents) * a->count) == 0;
    }

    return false;
}

/* Check if 'b' is a subset of 'a' */
/* Check if 'b' is contained within 'a' */
bool intsetU32Subset(const intsetU32 *const a, const intsetU32 *const b) {
    if (a == b) {
        return true;
    }

    for (size_t i = 0; i < a->count; i++) {
        if (!intsetU32Exists(b, a->contents[i])) {
            return false;
        }
    }

    return true;
}

uint32_t intsetU32Merge(intsetU32 **dst, const intsetU32 *b) {
    uint32_t merged = 0;
    for (size_t i = 0; i < b->count; i++) {
        merged += intsetU32Add(dst, b->contents[i]);
    }

    return merged;
}

/* Return random member */
uint32_t intsetU32Random(intsetU32 *is) {
    return intsetU32Get_(is, random() % is->count);
}

bool intsetU32RandomDelete(intsetU32 **is, uint32_t *deleted) {
    *deleted = intsetU32Random(*is);
    return intsetU32Remove(is, *deleted);
}

/* Get the value at the given position. When this position is
 * out of range the function returns false, when in range it returns true. */
bool intsetU32Get(const intsetU32 *is, uint32_t pos, uint32_t *value) {
    if (pos < is->count) {
        *value = intsetU32Get_(is, pos);
        return true;
    }

    return false;
}

/* Return intsetU32 count */
size_t intsetU32Count(const intsetU32 *is) {
    return is->count;
}

/* Return intsetU32 blob size in bytes. */
size_t intsetU32Bytes(const intsetU32 *is) {
    return sizeof(intsetU32) + (sizeof(uint32_t) * is->count);
}

void intsetU32Repr(const intsetU32 *is) {
    printf("[");
    for (uint32_t i = 0; i < is->count; i++) {
        printf("%u, ", intsetU32Get_(is, i));
    }

    printf("]\n");
}

#ifdef DATAKIT_TEST
#include <sys/time.h>
#include <time.h>
static void ok(void) {
    printf("OK\n");
}

static long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (((long long)tv.tv_sec) * 1000000) + tv.tv_usec;
}

#define intsetU32Assert(_e)                                                    \
    ((_e) ? (void)0 : (_intsetU32Assert(#_e, __FILE__, __LINE__), abort()))
static void _intsetU32Assert(char *estr, char *file, int line) {
    printf("\n\n=== intsetU32AssertION FAILED ===\n");
    printf("==> %s:%d '%s' is not true\n", file, line, estr);
}

static intsetU32 *createSet(int bits, int size) {
    uint64_t mask = (1 << bits) - 1;
    uint64_t value;
    intsetU32 *is = intsetU32New();

    for (int i = 0; i < size; i++) {
        if (bits > 32) {
            value = (random() * random()) & mask;
        } else {
            value = random() & mask;
        }

        intsetU32Add(&is, value);
    }

    return is;
}

int intsetU32Test(int argc, char **argv) {
    intsetU32 *is;
    srandom(time(NULL));

    (void)argc;
    (void)argv;

    printf("Basic adding: ");
    {
        is = intsetU32New();
        intsetU32Assert(is);
        const bool s1 = intsetU32Add(&is, 5);
        intsetU32Assert(is);
        intsetU32Assert(s1);
        const bool s2 = intsetU32Add(&is, 6);
        intsetU32Assert(is);
        intsetU32Assert(s2);
        const bool s3 = intsetU32Add(&is, 4);
        intsetU32Assert(is);
        intsetU32Assert(s3);
        const bool s4 = intsetU32Add(&is, 4);
        intsetU32Assert(is);
        intsetU32Assert(!s4);
        ok();
    }

    printf("Large number of random adds: ");
    {
        uint32_t inserts = 0;
        is = intsetU32New();
        for (int i = 0; i < 1024; i++) {
            if (intsetU32Add(&is, random() % 0x800)) {
                inserts++;
            }
        }

        intsetU32Assert(is->count == inserts);
        ok();
    }

    printf("Upgrade from int16 to int32: ");
    {
        is = intsetU32New();
        intsetU32Add(&is, 32);
        intsetU32Add(&is, 65535);
        intsetU32Assert(intsetU32Exists(is, 32));
        intsetU32Assert(intsetU32Exists(is, 65535));

        is = intsetU32New();
        intsetU32Add(&is, 32);
        intsetU32Add(&is, -65535);
        intsetU32Assert(intsetU32Exists(is, 32));
        intsetU32Assert(intsetU32Exists(is, -65535));
        ok();
    }

    printf("Stress lookups: ");
    {
        long num = 100000, size = 10000;
        int i, bits = 20;
        long long start;
        is = createSet(bits, size);

        start = usec();
        for (i = 0; i < num; i++) {
            intsetU32Search(is, random() % ((1 << bits) - 1), NULL);
        }
        printf("%ld lookups, %ld element set, %" PRId64 "usec\n", num, size,
               usec() - start);
    }

    printf("Stress add+delete: ");
    {
        int i, v1, v2;
        is = intsetU32New();
        for (i = 0; i < 0xffff; i++) {
            v1 = random() % 0xfff;
            intsetU32Add(&is, v1);
            intsetU32Assert(intsetU32Exists(is, v1));

            v2 = random() % 0xfff;
            intsetU32Remove(&is, v2);
            intsetU32Assert(!intsetU32Exists(is, v2));
        }

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
