#pragma once

#include "databox.h"

/* Forward-declare multidict because
 * multidict depends on multidictType and multidictType depends on multidict */
typedef struct multidict multidict;

/* ====================================================================
 * Result Codes
 * ==================================================================== */
typedef enum {
    MULTIDICT_ERR = -1,        /* Operation failed */
    MULTIDICT_OK_REPLACED = 0, /* Key existed, value was replaced */
    MULTIDICT_OK_INSERTED = 1  /* New key was inserted */
} multidictResult;

/* ====================================================================
 * Statistics Structure
 * ==================================================================== */
/* Basic stats - lightweight, always available */
typedef struct multidictStats {
    uint64_t count;      /* Total number of key-value pairs */
    uint64_t slots;      /* Total number of hash slots */
    uint32_t loadFactor; /* Current load factor (count * 100 / slots) */
    uint64_t usedBytes;  /* Bytes used by slot structures */
    uint64_t keyBytes;   /* Bytes used by keys */
    uint64_t valBytes;   /* Bytes used by values */
    uint64_t totalBytes; /* Total memory usage */
    bool isRehashing;    /* Whether rehashing is in progress */
} multidictStats;

/* Detailed stats - includes distribution data for deep analysis */
#define MULTIDICT_STATS_VECTLEN 50
typedef struct multidictDetailedStats {
    /* Basic stats */
    multidictStats basic;

    /* Distribution analysis */
    uint64_t usedSlots;   /* Number of non-empty slots */
    uint64_t maxChainLen; /* Longest chain in hash table */
    float avgChainLen;    /* Average chain length (used slots only) */
    uint64_t
        chainDistribution[MULTIDICT_STATS_VECTLEN]; /* Chain length histogram */

    /* Rehashing table stats (if applicable) */
    bool hasRehashTable;
    multidictStats rehashTable; /* Stats for ht[1] during rehashing */
} multidictDetailedStats;

/* Load factor metrics - includes both count-based and byte-based metrics */
typedef struct multidictLoadMetrics {
    /* Legacy count-based metric */
    uint32_t countLoadFactor; /* (count * 100) / slots */

    /* Byte-based metrics */
    uint64_t avgSlotBytes;   /* usedBytes / usedSlots (0 if no used slots) */
    uint32_t byteLoadFactor; /* (avgSlotBytes * 100) / targetSlotBytes */

    /* Statistics */
    uint64_t usedSlots;       /* Number of non-empty slots */
    uint64_t totalUsedBytes;  /* Sum of all slot sizes */
    uint64_t maxSlotBytes;    /* Largest slot size */
    uint64_t targetSlotBytes; /* Target avg slot size for expansion */
} multidictLoadMetrics;

/* How a dict hashes and matches its keys.
 *
 * hashFunction hashes the bytes of a key: a byte string's own bytes, and
 * for any other key the bytes of its value.
 *
 * keyCompare decides when two byte-string keys (DATABOX_BYTES and
 * DATABOX_BYTES_EMBED) are the same key; it returns nonzero when they
 * are.  It is resolved once, when the dict is created:
 *   - NULL, or the compare of multidictTypeExactKey: keys match by their
 *     exact bytes, straight through the slot layer.  This is the fast path
 *     and is never slowed by the other kinds.
 *   - Any other compare: every operation that matches a key (add, find,
 *     delete, exists, the conditional adds, increments, touch and LRU
 *     level, eviction, copy and merge) treats byte-string keys it reports
 *     equal as one key.  Keys it reports equal must hash equal.  The
 *     compare receives the class's privdata.
 * Integer, float and every other non-byte-string key always match by
 * value, exactly, whatever the compare is.
 *
 * Spelling: a dict holds one entry per key, under the spelling that first
 * inserted it.  Adding or replacing under another spelling the compare
 * reports equal updates the value and keeps the stored spelling, and
 * iteration, scans, random keys and eviction callbacks report the stored
 * spelling.  A lookup that does not match a key's exact bytes walks the
 * key's slot comparing each byte-string key there, so its cost grows
 * with slot size (two entries a slot on average at the default load
 * factor). */
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
    multidictSlot **slot; /* the table cell the operation used */
} multidictOp;

typedef void(multidictIterProcess)(void *privdata, const databox *key,
                                   const databox *val);

/* Eviction callback - called when an entry is about to be evicted due to
 * memory pressure. Return true to allow eviction, false to prevent it.
 * A vetoed entry stays in the dict and, under LRU eviction, keeps its LRU
 * level and rank unchanged (reading it for the callback is not an access).
 * The same multidictEvictToLimit call does not offer a vetoed LRU victim
 * again and moves on to the next-coldest entry; once 64 LRU entries have
 * been passed over, the rest of that call picks random victims. */
typedef bool(multidictEvictionCallback)(void *privdata, const databox *key,
                                        const databox *val);

/* multidictClass is a (17 * 8) + 1, padded = 144 byte struct: the
 * per-operation hooks fill the first cache line. */
typedef struct multidictClass {
    /* Private data this class needs (atom dict, key dict, etc) */
    void *privdata;

    /*** INTERFACE ***/
    /* Group by operation type per-cache line.  A hook that may reallocate
     * a slot takes its table cell (multidictSlot **) and updates it.
     * insertByType returns 1 for a new key, 0 for a replaced one (filling
     * 'replaced', when not NULL, with the value it overwrote, as
     * multimapInsertReplacing reports it) and -1 for no slot. */
    int64_t (*insertByType)(struct multidictClass *qdc, multidictSlot **slot,
                            const databox *keybox, const databox *valbox,
                            databox *replaced);
    /* this is operation too: */
    void *(*operationSlotGetOrCreate)(struct multidictClass *qdc,
                                      multidictSlot **slot,
                                      const databox *keybox);
    /* Remove 'keybox' from the slot in one search; false when it is not
     * there.  'removedVal', when not NULL, receives the removed value:
     * without 'take' under multimapDeleteReporting's rules (type and
     * length valid, payload not to be read), with 'take' as an owned copy
     * (multimapDeleteTaking). */
    bool (*operationRemove)(struct multidictClass *qdc, multidictSlot **slot,
                            const databox *keybox, databox *removedVal,
                            bool take);
    bool (*findValueByKey)(struct multidictClass *qdc, multidictSlot *slot,
                           const databox *keybox, databox *valbox);
    void *(*createSlot)(void);
    uint32_t (*freeSlot)(struct multidictClass *qdc, multidictSlot *slot);
    size_t (*sizeBytes)(multidictSlot *slot);
    /* END OF CACHE LINE 1 (8 pointers * 8 bytes = 64 bytes) */

    /* Iteration.  iterNext fills the entry the way multidictEntry
     * documents: a 96/128-bit field is decoded into iter->wide[0..2]
     * (key, val, extra), and the key and value flags are set up so the
     * entry needs no initialization. */
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

    /* Rehash uses getIter/iterNext, insertByType and freeSlot: every row
     * of an old slot is read in place and inserted into its new slot, then
     * the old slot is freed whole, so insertByType must copy what a row
     * borrows and freeSlot must not release what the copies hold. */
    void (*free)(struct multidictClass *qdc);
    /*** END OF INTERFACE ***/

    uint32_t (*freeClass)(struct multidictClass *qdc);
    /* Shared flags for entire class */
    uint8_t disableResize; /* one resize switch across all shared dicts */
} multidictClass;

/* A slot class's iteration state.  'wide' is the wide storage for the
 * entry the class fills: a 96/128-bit key, value or extra field is
 * decoded into wide[0], wide[1] or wide[2] (flexGetByTypeInto with that
 * slot), so the entry field points into the iterator and stays valid
 * until the next iterNext.  Narrow values never touch it.
 * 144 bytes, 16-byte aligned. */
typedef struct multidictSlotIterator {
    uint8_t iterspace[64]; /* 64 bytes the iterator can use as it needs.
                              Mainly used for mocking a stack-allocated struct
                              so we don't have to malloc/free custom iters. */
    multidictSlot *slot;
    void *entry;
    uint32_t index;
    databoxWide wide[3]; /* key, val, extra */
} multidictSlotIterator;
#if DK_C11
_Static_assert(sizeof(multidictSlotIterator) == 144,
               "multidictSlotIterator carries three wide slots at its tail");
#endif

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

/* An entry the library fills (multidictIteratorNext, slot iterators).
 * A 96/128-bit key or value has big = 1 and data.i128 / data.u128
 * pointing at the slot iterator's 'wide' storage; any other value has
 * big = 0 and allocated = 0.  The library sets the key and value up
 * before each fill, so the entry needs no initialization.  'extra' is
 * written only by slot classes that use it.
 *
 * Lifetime: the fields borrow.  BYTES point into the dict until it
 * changes; a 96/128-bit value points into the iterator and is valid
 * until the next multidictIteratorNext on it.  A struct copy of the
 * iterator has its own storage, and a field read from the original still
 * points into the original.  Keep a value longer with databoxCopy
 * (owned; release with databoxFreeData) or databoxBigCopyFrom (re-homed
 * into a databoxBig).
 *
 * 3 * databox = 48 bytes. */
typedef struct multidictEntry {
    databox key;
    databox val;
    databox extra; /* sometimes we need a third field */
} multidictEntry;
#if DK_C11
_Static_assert(sizeof(multidictEntry) == 48,
               "multidictEntry is three databox fields");
#endif

typedef multidictIterProcess multidictScanFunction;

/* This is the initial size of every hash table */
#define MULTIDICT_HT_INITIAL_SIZE 1

/* ------------------------------- Accessors ---------------------------------*/
/* These are proper functions for encapsulation - implementation details are
 * hidden */
uint64_t multidictCount(const multidict *d);     /* Total key-value pairs */
uint64_t multidictSlotCount(const multidict *d); /* Total hash slots */
bool multidictIsRehashing_(const multidict *d);  /* Is rehash in progress? */

/* Backwards compatibility macros - prefer the function versions above */
#define multidictSize(d) multidictCount(d)
#define multidictSlots(d) multidictSlotCount(d)
#define multidictIsRehashing(d) multidictIsRehashing_(d)

/* 96/128-bit keys and values.
 *
 * Keys and values may be 96/128-bit integers; a key hashes by value, so a
 * 128-bit box holding a value that fits 64 bits finds the same entry as
 * the 64-bit box.  Such a value does not fit a databox, so where it lands
 * depends on who owns the destination (see databox.h, "the destination
 * owner sets the wide capability"):
 *
 *   - Caller boxes (multidictFind's valbox, multidictGetRandomKey's
 *     keybox) are filled in place, as flexGetByType fills them: pass a
 *     DATABOX_BIG_INIT'd databoxBig, re-initialized before each call, to
 *     receive a wide value; a zeroed box receives the type only.  BYTES
 *     results borrow from the dict until its next call: even a lookup can
 *     run a rehash step that moves the entry and frees the bytes, so pass
 *     a databoxCopy of the key when finding or deleting it.
 *   - multidictGetSomeKeys ignores the prior contents of 'keys': each
 *     slot is cleared before it is filled, so a 96/128-bit key comes back
 *     deferred (type only, data.ptr == NULL; databoxIsWideDeferred).
 *     Read it again with multidictGetRandomKey or iteration if needed.
 *   - Removing reads (multidictGetAndDelete, multidictPopRandom) never
 *     read their destination boxes and return self-contained values:
 *     BYTES and 96/128-bit values are heap copies.  Release each one with
 *     databoxFreeData.
 *   - multidictEntry fields carry their wide values in the iterator (see
 *     multidictEntry).  The boxes passed to scan callbacks and eviction
 *     callbacks carry their own wide storage and are valid for the
 *     duration of the call; in both, big = 1 exactly for a wide value.
 *   - multidictIncrBy and multidictIncrByFloat treat a 96/128-bit value as
 *     non-numeric and fail with MULTIDICT_ERR. */

/* API */
multidict *multidictNew(multidictType *type, multidictClass *qdc, int32_t seed);
multidictClass *multidictGetClass(multidict *d);
/* The type 'd' was created with (how its keys hash and match) */
multidictType *multidictGetType(const multidict *d);
multidictClass *multidictDefaultClassNew(void);
void multidictDefaultClassFree(multidictClass *qdc);
bool multidictExpand(multidict *d, uint64_t size);
multidictResult multidictAdd(multidict *d, const databox *keybox,
                             const databox *valbox);
bool multidictDelete(multidict *ht, const databox *keybox);
void multidictFree(multidict *d);
bool multidictFind(multidict *d, const databox *keybox, databox *valbox);
/* Find the value of the C-string key 'key'.  True only when the key is
 * present and its value is a byte string: '*val' then points at the
 * value's bytes, borrowed from the dict until it changes.  The length is
 * not reported and the bytes are not NUL-terminated; use multidictFind
 * for anything else. */
bool multidictFindByString(multidict *d, const char *key, uint8_t **val);
bool multidictResize(multidict *d);

/* Conditional Insert Operations */
multidictResult multidictAddNX(multidict *d, const databox *keybox,
                               const databox *valbox);
multidictResult multidictAddXX(multidict *d, const databox *keybox,
                               const databox *valbox);
multidictResult multidictReplace(multidict *d, const databox *keybox,
                                 const databox *valbox);

/* Atomic Get-and-Delete Operations */
bool multidictGetAndDelete(multidict *d, const databox *keybox,
                           databox *valbox);
bool multidictPopRandom(multidict *d, databox *keybox, databox *valbox);

/* Numeric Operations (the platform increment policy, DATABOX.md "Numeric
 * updates").  A missing key is created holding 'increment'.
 *
 * multidictIncrBy: the stored value must be an integer that fits int64; the
 * sum is exact, and a result outside int64 is refused.  A float, a 96/128-bit
 * value or a non-number returns MULTIDICT_ERR (use IncrByFloat for floats).
 *
 * multidictIncrByFloat: the stored value must be a float, double or 64-bit
 * integer; the sum is a double, and an inf or NaN result is refused.
 *
 * On MULTIDICT_ERR the stored value and '*result' are unchanged. */
multidictResult multidictIncrBy(multidict *d, const databox *keybox,
                                int64_t increment, int64_t *result);
multidictResult multidictIncrByFloat(multidict *d, const databox *keybox,
                                     double increment, double *result);

/* Dict Operations */
multidict *multidictCopy(const multidict *src);
typedef enum {
    MULTIDICT_MERGE_REPLACE, /* Replace dst value with src on collision */
    MULTIDICT_MERGE_KEEP     /* Keep dst value on collision */
} multidictMergeMode;
uint64_t multidictMerge(multidict *dst, const multidict *src,
                        multidictMergeMode mode);

/* Convenience functions */
bool multidictExists(multidict *d, const databox *keybox);
bool multidictExistsByString(multidict *d, const char *key);

/* Statistics */
void multidictGetStats(multidict *d, multidictStats *stats);
void multidictGetDetailedStats(multidict *d, multidictDetailedStats *stats);
void multidictGetLoadMetrics(multidict *d, multidictLoadMetrics *metrics);
uint32_t multidictLoadFactor(multidict *d);
uint64_t multidictBytes(multidict *d);
uint64_t multidictKeyBytes(multidict *d);
uint64_t multidictValBytes(multidict *d);

/* Bulk operations */
uint32_t multidictAddMultiple(multidict *d, const databox *keys,
                              const databox *vals, uint32_t count);
uint32_t multidictDeleteMultiple(multidict *d, const databox *keys,
                                 uint32_t count);

/* Iterators */
bool multidictIteratorInit(multidict *d, multidictIterator *iter);
bool multidictIteratorGetSafe(multidict *d, multidictIterator *iter);
bool multidictIteratorNext(multidictIterator *iter, multidictEntry *e);
void multidictIteratorRelease(multidictIterator *iter);

/* Fills 'keybox' with a random key; returns false only when the dict is
 * empty, including while a rehash is in flight. */
bool multidictGetRandomKey(multidict *d, databox *keybox);
uint32_t multidictGetSomeKeys(multidict *d, databox *keys, uint32_t count);
void multidictEmpty(multidict *d);
void multidictPrintStats(multidict *d);
void multidictResizeEnable(multidict *d);
void multidictResizeDisable(multidict *d);
bool multidictRehash(multidict *d, int32_t n);
int64_t multidictRehashMilliseconds(multidict *d, int64_t ms);
bool multidictSetHashFunctionSeed(multidict *d, uint32_t seed);
uint32_t multidictGetHashFunctionSeed(multidict *d);
uint64_t multidictScan(multidict *d, uint64_t v, multidictScanFunction *fn,
                       void *privdata);

/* Memory Management / Self-Management */
void multidictSetMaxMemory(multidict *d, uint64_t maxBytes);
uint64_t multidictGetMaxMemory(multidict *d);
void multidictSetEvictionCallback(multidict *d, multidictEvictionCallback *cb,
                                  void *privdata);
uint32_t
multidictEvictToLimit(multidict *d);     /* Evict entries to fit in limit */
bool multidictIsOverLimit(multidict *d); /* Check if over memory limit */

/* Byte-Based Expansion Configuration */
void multidictEnableByteBasedExpansion(multidict *d, uint64_t targetSlotBytes,
                                       uint64_t maxSlotBytes);
void multidictDisableByteBasedExpansion(multidict *d);
bool multidictIsByteBasedExpansion(multidict *d);

/* LRU Policy Configuration */
typedef enum multidictEvictPolicy {
    MULTIDICT_EVICT_NONE = 0, /* No eviction (default) */
    MULTIDICT_EVICT_RANDOM,   /* Random eviction (existing behavior) */
    MULTIDICT_EVICT_LRU,      /* Least Recently Used */
    MULTIDICT_EVICT_LFU,      /* Least Frequently Used (approx via levels) */
    MULTIDICT_EVICT_SIZE_LRU, /* LRU weighted by entry size */
} multidictEvictPolicy;

/* LRU Tracking (optional, zero overhead when disabled)
 *
 * LRU use, the named-key rule: every call that names an existing key is one
 * use of it and promotes it exactly one LRU level, whatever the outcome:
 *   - multidictFind, multidictFindByString, multidictTouch;
 *   - multidictExists, multidictExistsByString;
 *   - multidictAdd of a present key, multidictAddNX (even when it refuses
 *     because the key is present), multidictAddXX, multidictReplace;
 *   - multidictIncrBy / multidictIncrByFloat of a present key, including
 *     one the increment policy refuses.
 * An insert tracks a new key at the coldest level, and a delete stops
 * tracking it.  Calls that name no existing key never promote: a miss,
 * iteration, scans, random keys, persistence, rehashing, the value read
 * for an eviction callback, and multidictGetLRULevel (an inspector). */
bool multidictEnableLRU(multidict *d, size_t levels);
void multidictDisableLRU(multidict *d);
bool multidictHasLRU(multidict *d);
void multidictSetEvictPolicy(multidict *d, multidictEvictPolicy policy);
multidictEvictPolicy multidictGetEvictPolicy(multidict *d);
void multidictTouch(multidict *d, const databox *keybox);
int multidictGetLRULevel(multidict *d, const databox *keybox);

/* Whether 'keybox' is present, as an inspector: no LRU use (for a caller
 * that names the key in the operation it decides on) */
bool multidictExistsNoTouch(multidict *d, const databox *keybox);

/* Hashes */
uint32_t multidictIntHashFunction(uint32_t key);
uint32_t multidictLongLongHashFunction(uint64_t key);
/* The seeded hash of exact keys: XXH64 (XXH32 on 32-bit builds) with the
 * dict's seed */
uint32_t multidictGenHashFunction(multidict *d, const void *key, int32_t len);
/* The seeded hash of case keys: multidictGenHashFunction of the key with
 * 'A'-'Z' folded to 'a'-'z', computed without allocating */
uint32_t multidictGenCaseHashFunction(multidict *d, const uint8_t *buf,
                                      int32_t len);

/* Hash table types.
 *
 * multidictTypeExactKey: byte-string keys match by their exact bytes;
 * keys are binary, so they may hold NUL bytes.
 *
 * multidictTypeCaseKey: byte-string keys match ASCII case-insensitively:
 * 'A'-'Z' and 'a'-'z' fold together, and every other byte (NUL, bytes
 * above 0x7F) must match exactly, independent of the C locale.  "Key",
 * "KEY" and "key" are one key, stored under the spelling first added (see
 * multidictType).  Integer and other non-byte-string keys match exactly.
 * Persist snapshots record which of the two a dict uses, so it restores
 * with the same key matching (multidictNewPWithType, multidictOpenP); a
 * dict with any other type persists only through a caller-supplied
 * restore (see persistOpsMultidict in persist.h). */
extern multidictType multidictTypeExactKey; /* exact keys */
extern multidictType multidictTypeCaseKey;  /* ASCII case-insensitive keys */

/* Debugging */
void multidictRepr(const multidict *d);
void multidictOpRepr(const multidictOp *op);

#ifdef DATAKIT_TEST
int multidictTest(int argc, char *argv[]);
#endif

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
