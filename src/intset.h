#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Note that these encodings are ordered, so:
 * INTSET_ENC_INT16 < INTSET_ENC_INT32 < INTSET_ENC_INT64. */
typedef enum intsetEnc {
    INTSET_ENC_INT16 = sizeof(int16_t),
    INTSET_ENC_INT32 = sizeof(int32_t),
    INTSET_ENC_INT64 = sizeof(int64_t)
} intsetEnc;

typedef struct intset {
    intsetEnc encoding;

    /* technically, 'length' should be a size_t, but in reality if a user has
     * more than 2^32 elements, their set will be over 35 GB rendering most
     * usage of it really bad performance-wise since 'contents' is a linearly
     * realloc'd array for additions/removals. */
    uint32_t length;

    /* variable elements fit the smallest storage type possible for all elements
     * during insert, but does not shrink on removal.
     * (e.g. if you add 5, 10, 20, it'll use int16_t storage.
     *       if you add 5, 10, 20, 200, it'll use int32_t storage.
     *       if you add 5, 10, INT_MAX + 1, it'll use int64_t storage.)
     *       NOTE: storage does NOT shrink. If you get upgraded to 8 byte
     *             storage then remove all 8 byte values, it remains 8 byte
     *             storage forever.
     *       Also, you're out of luck if you want to store larger than INT64_MAX
     */
    int8_t contents[];
} intset;

intset *intsetNew(void);
void intsetFree(intset *is);
void intsetAdd(intset **is, int64_t value, bool *success);
void intsetRemove(intset **is, int64_t value, bool *success);
bool intsetFind(intset *is, int64_t value);
bool intsetGet(intset *is, uint32_t pos, int64_t *value);
int64_t intsetRandom(intset *is);
size_t intsetCount(const intset *is);
size_t intsetBytes(intset *is);

#ifdef DATAKIT_TEST
void intsetRepr(const intset *is);
int intsetTest(int argc, char *argv[]);
#endif

/*
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
