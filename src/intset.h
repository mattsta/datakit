#pragma once

#include "dkTier.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Tiered intset implementation using pointer tagging
 * - Small tier: int16_t values only (single contiguous array)
 * - Medium tier: int16_t and int32_t values (separate sorted arrays)
 * - Full tier: int16_t, int32_t, and int64_t values (three sorted arrays)
 *
 * Each value is stored at the narrowest width that holds it, so one large
 * value costs its own 8 bytes, not a wider encoding for every value.
 */

/* Opaque pointer type - actual tier determined by pointer tagging */
typedef struct intset intset;

/* The byte width a value needs (intsetCommon.h picks it per value) */
typedef enum intsetEnc {
    INTSET_ENC_INT16 = sizeof(int16_t),
    INTSET_ENC_INT32 = sizeof(int32_t),
    INTSET_ENC_INT64 = sizeof(int64_t)
} intsetEnc;

/* Create new intset */
[[nodiscard]] intset *intsetNew(void);

/* Free intset */
void intsetFree(intset *is);
/* Empty the set: a new empty Small, freeing every tier's arrays.
 * Rewrites '*is'. */
void intsetReset(intset **is);
/* A deep copy in the same tier */
[[nodiscard]] intset *intsetCopy(const intset *is);
/* Move to the narrowest tier the values allow: a Full holding no 64-bit
 * values becomes a Medium (its arrays move, nothing is copied), and a
 * Medium holding no 32-bit values becomes a Small, within the counts and
 * bytes those tiers hold.  Removes never move a set down (one add of a
 * wide value would move it straight back up), so this is the call for a
 * set whose wide values are gone for good.  Rewrites '*is'. */
void intsetCompact(intset **is);

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
bool intsetFind(const intset *is, int64_t value);

/* Get value at position (0-indexed), in O(log n).  To visit every value,
 * use the iterator below (O(1) per value). */
bool intsetGet(const intset *is, uint32_t pos, int64_t *value);

/* Walks an intset's values in ascending order, O(1) per value.  The
 * intset must not change during the walk.  Values are runs of the tier's
 * arrays (see intset.c); the fields are private. */
typedef struct intsetIterator {
    const void *run[5];
    uint64_t left[5];
    uint8_t width[5];
    uint8_t runs;
    uint8_t current;
} intsetIterator;

void intsetIteratorInit(intsetIterator *it, const intset *is);
bool intsetIteratorNext(intsetIterator *it, int64_t *value);

/* Get random value from intset */
int64_t intsetRandom(const intset *is);

/* Get total count of elements */
size_t intsetCount(const intset *is);

/* O(1) smallest/largest member; false when empty */
bool intsetMin(const intset *is, int64_t *value);
bool intsetMax(const intset *is, int64_t *value);

/* Get total bytes used */
size_t intsetBytes(const intset *is);

/* The tier, value-width arrays (1 Small, 2 Medium, 3 Full), members and
 * bytes: the members' bytes at their widths, and intsetBytes (with the
 * tier's struct).  'unitLimit' is the byte size where Small (64 KB) and
 * Medium (8 MB) grow regardless of widths; 0 for Full.  NULL reports all
 * zeros. */
void intsetStats(const intset *is, dkTierStats *out);

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
