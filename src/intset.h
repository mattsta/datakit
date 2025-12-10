#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Tiered intset implementation using pointer tagging
 * - Small tier: int16_t values only (single contiguous array)
 * - Medium tier: int16_t and int32_t values (separate sorted arrays)
 * - Full tier: int16_t, int32_t, and int64_t values (three sorted arrays)
 *
 * This avoids the old problem where adding one large value would
 * upgrade the entire array from int16 to int64 encoding.
 */

/* Opaque pointer type - actual tier determined by pointer tagging */
typedef struct intset intset;

/* For backward compatibility */
typedef enum intsetEnc {
    INTSET_ENC_INT16 = sizeof(int16_t),
    INTSET_ENC_INT32 = sizeof(int32_t),
    INTSET_ENC_INT64 = sizeof(int64_t)
} intsetEnc;

/* Create new intset */
intset *intsetNew(void);

/* Free intset */
void intsetFree(intset *is);

/* Add value to intset
 * is: pointer to intset pointer (may be modified due to reallocation/upgrade)
 * value: value to add
 * success: set to true if added, false if already exists */
void intsetAdd(intset **is, int64_t value, bool *success);

/* Remove value from intset
 * is: pointer to intset pointer (may be modified due to reallocation)
 * value: value to remove
 * success: set to true if removed, false if not found */
void intsetRemove(intset **is, int64_t value, bool *success);

/* Find if value exists in intset */
bool intsetFind(intset *is, int64_t value);

/* Get value at position (0-indexed)
 * Note: In tiered implementation, this performs virtual merge of arrays.
 * For Medium/Full tiers, this is O(n) worst case. Use iterator for traversal.
 */
bool intsetGet(intset *is, uint32_t pos, int64_t *value);

/* Get random value from intset */
int64_t intsetRandom(intset *is);

/* Get total count of elements */
size_t intsetCount(const intset *is);

/* Get total bytes used */
size_t intsetBytes(intset *is);

#ifdef DATAKIT_TEST
/* Print representation of intset */
void intsetRepr(const intset *is);

/* Run intset tests */
int intsetTest(int argc, char *argv[]);
#endif

/*
 * Original intset implementation:
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 *
 * Tiered implementation enhancements:
 * Copyright (c) 2024, Matt Stancliff
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
