#pragma once

#include "databox.h"

/* Forward-declare multidict because
 * multidict depends on multidictType and multidictType depends on multidict */
typedef struct multidict multidict;

typedef struct multidictType {
    uint32_t (*hashFunction)(struct multidict *d, const void *key,
                             uint32_t len);
    int32_t (*keyCompare)(void *privdata, const void *key1,
                          const uint32_t key1_sz, const void *key2,
                          const uint32_t key2_sz);
} multidictType;

typedef void multidictSlot;
struct multidictEntry;
struct multidictSlotIterator;

typedef struct multidictOp {
    struct multidict *d;
    struct multidictHT *ht;
    void *result;
} multidictOp;

typedef void(multidictIterProcess)(void *privdata, const databox *key,
                                   const databox *val);

/* multidictClass is a (15 * 8) + 4 = 124 byte struct. */
/* RE-ORGANIZE BASED ON COMMON USAGE PER CACHE LINE. */
typedef struct multidictClass {
    /* Private data this class needs (atom dict, key dict, etc) */
    void *privdata;

    /*** INTERFACE ***/
    /* Group by operation type per-cache line */
    int64_t (*insertByType)(struct multidictClass *qdc, multidictSlot *slot,
                            const databox *keybox, const databox *valbox);
    /* this is operation too: */
    void *(*operationSlotGetOrCreate)(struct multidictClass *qdc,
                                      multidictSlot **slot,
                                      const databox *keybox);
    void *(*operationRemove)(struct multidictClass *qdc, multidictSlot **slot,
                             const databox *keybox);
    bool (*findValueByKey)(struct multidictClass *qdc, multidictSlot *slot,
                           const databox *keybox, databox *valbox);
    void *(*createSlot)(void);
    uint32_t (*freeSlot)(struct multidictClass *qdc, multidictSlot *slot);
    uint32_t (*sizeBytes)(multidictSlot *slot);
    /* END OF CACHE LINE 1 (8 pointers * 8 bytes = 64 bytes) */

    /* Iteration */
    bool (*getIter)(struct multidictSlotIterator *iter, multidictSlot *slot);
    bool (*iterNext)(struct multidictSlotIterator *iter,
                     struct multidictEntry *entry);

    /* For random key extraction (findKey(random() % count) */
    uint32_t (*countSlot)(multidictSlot *slot);
    bool (*findKeyByPosition)(struct multidictClass *qdc, multidictSlot *slot,
                              uint32_t pos, databox *keybox);

    /* For SCAN */
    void (*iterateAll)(struct multidictClass *qdc, multidictSlot *slot,
                       multidictIterProcess process, void *privdata);

    /* For rehash (lookup last key, hash, move last key to new slot) */
    bool (*lastKey)(multidictSlot *slot, databox *keybox);
    bool (*migrateLast)(void *dst, void *src);
    void (*free)(struct multidictClass *qdc);
    /*** END OF INTERFACE ***/

    uint32_t (*freeClass)(struct multidictClass *qdc);
    /* Shared flags for entire class (could make this a 64 bit bitfield) */
    uint8_t disableResize; /* one resize switch across all shared dicts */

    /* RE-DO MATH */
    /* END OF CACHE LINE 2 (Currently 4 remaining bytes to fill up line) */
} multidictClass;

typedef struct multidictSlotIterator {
    uint8_t iterspace[64]; /* 64 bytes the iterator can use as it needs.
                              Mainly used for mocking a stack-allocated struct
                              so we don't have to malloc/free custom iters. */
    multidictSlot *slot;
    void *entry;
    uint32_t index;
} multidictSlotIterator;

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * multidictAdd, multidictFind, and other functions against the multidictionary
 * even while iterating.
 * Otherwise it is a non safe iterator, and only multidictNext()
 * should be called while iterating. */
typedef struct multidictIterator {
    multidict *d;
    multidictSlotIterator iter;
    multidictSlot *current;
    uint64_t fingerprint; /* detect changes to HT during iteration */
    int64_t index;
    int32_t table;
    bool safe;
} multidictIterator;

/* databox = 20 bytes; 3 * databox = 60 byte total struct */
typedef struct multidictEntry {
    /* used to have 'iterator' here, but... it wasn't used. */
    databox key;
    databox val;
    databox extra; /* sometimes we need a third field */
    /* used to have 'offset' here too, but it was unused. */
} multidictEntry;

typedef struct multidictKeyInfo {
    uint8_t *key;
    uint32_t klen;
    int64_t klong;
} multidictKeyInfo;

typedef multidictIterProcess multidictScanFunction;

/* This is the initial size of every hash table */
#define MULTIDICT_HT_INITIAL_SIZE 1

/* ------------------------------- Macros ------------------------------------*/
#define multidictCompareKeys(d, key1, k1len, key2, k2len)                      \
    (((d)->type->keyCompare)                                                   \
         ? (d)->type->keyCompare((d)->privdata, key1, k1len, key2, k2len)      \
         : (key1) == (key2))

#define multidictHashKey(d, key, len) (d)->type->hashFunction(d, key, len)
#define multidictSlots(d) ((d)->ht[0].size + (d)->ht[1].size)
#define multidictSize(d) ((d)->ht[0].count + (d)->ht[1].count)
#define multidictIsRehashing(d) ((d)->rehashing)

/* API */
multidict *multidictNew(multidictType *type, multidictClass *qdc, int32_t seed);
multidictClass *multidictGetClass(multidict *d);
bool multidictExpand(multidict *d, uint64_t size);
int64_t multidictAdd(multidict *d, const databox *keybox,
                     const databox *valbox);
bool multidictDelete(multidict *ht, const databox *keybox);
void multidictFree(multidict *d);
bool multidictFind(multidict *d, const databox *keybox, databox *valbox);
bool multidictFindByString(multidict *d, char *key, uint8_t **val);
void *multidictFetchValue(multidict *d, const void *key);
bool multidictResize(multidict *d);

/* Iterators */
bool multidictIteratorInit(multidict *d, multidictIterator *iter);
bool multidictIteratorGetSafe(multidict *d, multidictIterator *iter);
bool multidictIteratorNext(multidictIterator *iter, multidictEntry *e);
void multidictIteratorRelease(multidictIterator *iter);

bool multidictGetRandomKey(multidict *d, databox *keybox);
uint32_t multidictGetSomeKeys(multidict *d, multidictKeyInfo *des[],
                              uint32_t count);
void multidictPrintStats(multidict *d);
void multidictResizeEnable(multidict *d);
void multidictResizeDisable(multidict *d);
bool multidictRehash(multidict *d, int32_t n);
int64_t multidictRehashMilliseconds(multidict *d, int64_t ms);
bool multidictSetHashFunctionSeed(multidict *d, uint32_t seed);
uint32_t multidictGetHashFunctionSeed(multidict *d);
uint64_t multidictScan(multidict *d, uint64_t v, multidictScanFunction *fn,
                       void *privdata);

/* Hashes */
uint32_t multidictIntHashFunction(uint32_t key);
uint32_t multidictLongLongHashFunction(uint64_t key);
uint32_t multidictGenHashFunction(multidict *d, const void *key, int32_t len);
uint32_t multidictGenCaseHashFunction(multidict *d, const uint8_t *buf,
                                      int32_t len);

/* Hash table types */
extern multidictType multidictTypeExactKey; /* exact keys */
extern multidictType multidictTypeCaseKey;  /* case insensitive keys */

/* Debugging */
void multidictRepr(const multidict *d);
void multidictOpRepr(const multidictOp *op);

/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2016, Matt Stancliff <matt@genges.com>
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
