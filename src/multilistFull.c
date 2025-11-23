/* multilistFull.c - A chunked collection of in-order flex nodes
 *
 * Copyright 2014-2016 Matt Stancliff <matt@genges.com>
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

#if 0
#define DATAKIT_TEST_VERBOSE
#endif

#include "datakit.h"

#include "flexCapacityManagement.h"
#include "mflex.h"
#include "multilistFull.h"

/* ====================================================================
 * Management Defines, Types, and Macros
 * ==================================================================== */
/* multilistFull is a 24 byte struct.
 * 'node' is a multiarray of 'mflex *'
 * 'values' is the number of all elements across all mflexes
 * 'count' is the number of nodes
 * 'fill' is either the size-based limit to reach before making a new node
 * 'compress' is 0 if compression is disabled - or - it is the number of
 *            nodes to leave uncompressed from both sides of the list.
 * Note: head/tail nodes are *never* compressed. */
struct multilistFull {
    multiarray *node;  /* array nodes holding flexes */
    mlOffsetId values; /* total number of all values in all flexes */
    mlNodeId count;    /* number of mflexes */
    uint16_t fill;     /* fill factor for individual nodes */
    uint16_t compress; /* depth of end nodes not to compress;0=off */
};

/* STORAGE_MAX is the maximum number of entires in our
 * mflex array before it grows to a new multiarray
 * storage type.  The larger the number, the more reallocation
 * overhead with every insert.  The smaller the number, the sooner
 * it'll upgrade to a larger (and slightly less efficient, but
 * more scalable) container.
 * Max  64 =   512 byte array (8 bytes * 64 nodes)
 *     128 =  1024 byte array (8 * 128 nodes)
 *     256 =  2048 byte array (8 * 256 nodes)
 *     512 =  4096 byte array (8 * 512 nodes)
 *    1024 =  8192 byte array (8 * 1024 nodes)
 *    2048 = 16384 byte array (8 * 2048 nodes) */
#define STORAGE_MAX 2048

/* Note: We use 'uintptr_t' as the underlying type because
 *       the 'mflex' type is not complete and can't be dereferenced. */
#define MAR_MFLEX_STORAGE uintptr_t *

#define _reallocGrowNode(_m, idx, _data)                                       \
    do {                                                                       \
        multiarrayNativeInsert(_m->node, MAR_MFLEX_STORAGE, STORAGE_MAX,       \
                               _m->count, idx, &_data);                        \
    } while (0)

#define reallocIncrCountBefore(_m, idx, node) _reallocGrowNode(_m, idx, node)

#define reallocIncrCountAfter(_m, idx, node) _reallocGrowNode(_m, idx + 1, node)

#define reallocDecrCount(_m, idx)                                              \
    do {                                                                       \
        /* Free existing data then shrink allocation.                          \
         * This kills the 'idx' */                                             \
        mflexFree(getNodeMl(_m, idx));                                         \
        multiarrayNativeDelete((_m)->node, MAR_MFLEX_STORAGE, (_m)->count,     \
                               idx);                                           \
    } while (0)

#define getNode(idx) getNodeMl(ml, idx)
#define getNodePtr(idx) getNodeMlPtr(ml, idx)
#define getNode_(ml, idx, dst)                                                 \
    multiarrayNativeGetForward(ml->node, MAR_MFLEX_STORAGE, dst, idx)
#define mlIdxExists(ml, idx) ((idx) < (ml)->count && (idx) >= 0)

/* We're doing more exact math now, so we don't need these ± 1 guards. */
#if 0
#define mlNextExists(ml, idx) ((idx) + 1 < (ml)->count)
#define mlPrevExists(ml, idx) ((int32_t)(idx) - 1 > 0)
#endif

static inline mflex *getNodeMl(const multilistFull *ml, const mlOffsetId idx) {
    uintptr_t **m;
    getNode_(ml, idx, m);
    return (mflex *)*m;
}

static inline mflex **getNodeMlPtr(const multilistFull *ml,
                                   const mlOffsetId idx) {
    uintptr_t **m;
    getNode_(ml, idx, m);
    return (mflex **)m;
}

/* Create a new multilistFull.
 * Free with multilistFullFree(). */
multilistFull *multilistFullCreate(void) {
    multilistFull *ml = zcalloc(1, sizeof(*ml));
    ml->fill = -2;

    /* Create our first mflex */
    mflex *m = mflexNewNoCompress();
    reallocIncrCountBefore(ml, 0, m);

    return ml;
}

size_t multilistFullCount(const multilistFull *ml) {
    return ml->values;
}

size_t multilistFullBytes(const multilistFull *ml) {
    size_t totalBytes = 0;
    for (mlNodeId i = 0; i < ml->count; i++) {
        totalBytes += mflexBytesUncompressed(getNode(i));
    }

    return totalBytes;
}

size_t multilistBytesActual(const multilistFull *ml) {
    size_t totalBytes = 0;
    for (mlNodeId i = 0; i < ml->count; i++) {
        totalBytes += mflexBytesActual(getNode(i));
    }

    return totalBytes;
}

#define COMPRESS_MAX ((1 << 15) - 1)
void multilistFullSetCompressDepth(multilistFull *ml, uint32_t compress) {
    if (compress > COMPRESS_MAX) {
        assert(NULL);
        compress = COMPRESS_MAX;
    }

    ml->compress = compress;
}

#define FILL_MAX (flexOptimizationSizeLimits - 1)
void multilistFullSetFill(multilistFull *ml, uint32_t fill) {
    if (fill > FILL_MAX) {
        fill = FILL_MAX;
    }

    ml->fill = fill;
}

void multilistFullSetDepth(multilistFull *ml, uint32_t fill, uint32_t depth) {
    multilistFullSetFill(ml, fill);
    multilistFullSetCompressDepth(ml, depth);
}

/* Create a new multilistFull with some default parameters. */
multilistFull *multilistFullNew(uint32_t fill, uint32_t compress) {
    multilistFull *ml = multilistFullCreate();
    multilistFullSetDepth(ml, fill, compress);
    return ml;
}

/* Free entire multilistFull. */
void multilistFullFree(multilistFull *ml) {
    if (ml) {
        for (mlNodeId i = 0; i < ml->count; i++) {
            mflex *node = getNode(i);
            ml->values -= mflexCount(node);
            mflexFree(node);
        }

        assert(ml->values == 0);
        multiarrayNativeFree(ml->node);
        zfree(ml);
    }
}

#define multilistFullAllowsCompression(_ml) ((_ml)->compress > 0)

#define mlHeadIdx(ml) (0)
#define mlTailIdx(ml) ((ml)->count - 1)

static inline mflex *mlHead(const multilistFull *ml) {
    uintptr_t **m;
    multiarrayNativeGetHead(ml->node, MAR_MFLEX_STORAGE, m);
    return (mflex *)*m;
}

static inline mflex *mlTail(const multilistFull *ml) {
    uintptr_t **m;
    multiarrayNativeGetTail(ml->node, MAR_MFLEX_STORAGE, m, ml->count);
    return (mflex *)*m;
}

static inline mflex **mlHeadPtr(const multilistFull *ml) {
    uintptr_t **m;
    multiarrayNativeGetHead(ml->node, MAR_MFLEX_STORAGE, m);
    return (mflex **)m;
}

static inline mflex **mlTailPtr(const multilistFull *ml) {
    uintptr_t **m;
    multiarrayNativeGetTail(ml->node, MAR_MFLEX_STORAGE, m, ml->count);
    return (mflex **)m;
}

#define multilistFullCompressRenew__(ml, state)                                \
    multilistFullCompress__(ml, state, 0, false)

#define multilistFullCompressMiddle__(ml, state, idx)                          \
    multilistFullCompress__(ml, state, idx, true)

/* Force 'multilistFull' to meet compression guidelines set by compress depth.
 * The only way to guarantee interior nodes get compressed is to iterate
 * to our "interior" compress depth then compress the next node we find.
 * If compress depth is larger than the entire list, we return immediately. */
DK_STATIC void multilistFullCompress__(const multilistFull *ml,
                                       mflexState *state,
                                       mlNodeId requestedCompressNodeIdx,
                                       const bool middleOnly) {
    if (!multilistFullAllowsCompression(ml)) {
        return;
    }

    if (!middleOnly) {
        /* Iterate until we reach compress depth for both sides of the list. */
        for (mlNodeId depth = 0;
             depth < ml->compress && (depth * 2) < ml->count; depth++) {
            mflexSetCompressNever(getNodePtr(mlHeadIdx(ml) + depth), state);
            mflexSetCompressNever(getNodePtr(mlTailIdx(ml) - depth), state);
        }
    }

    /* If length is less than our compress depth (from both sides),
     * we can't compress anything. */
    if (ml->count <= (ml->compress * 2)) {
        /* Not enough nodes to compress */
        return;
    }

    if (requestedCompressNodeIdx >= (mlHeadIdx(ml) + ml->compress) &&
        requestedCompressNodeIdx <= (int)(mlTailIdx(ml) - ml->compress)) {
        mflexSetCompressAuto(getNodePtr(requestedCompressNodeIdx), state);
    }

    /* Now compress interior nodes one level beyond our compress depth. */
    D("Compressing interior ± 1: %d, %d\n", mlHeadIdx(ml) + ml->compress,
      mlTailIdx(ml) - ml->compress);

    mflexSetCompressAuto(getNodePtr(mlHeadIdx(ml) + ml->compress), state);
    mflexSetCompressAuto(getNodePtr(mlTailIdx(ml) - ml->compress), state);

#if 0
    printf("[");
    for (mlNodeId depth = 0; depth < ml->count; depth++) {
        printf("%s, ", mflexIsCompressed(getNode(depth)) ? "C" : "U");
    }
    printf("\b\b]\n");
#endif
}

DK_STATIC mlNodeId multilistFullInsertNodeAfter__(multilistFull *ml,
                                                  mflexState *state,
                                                  mlNodeId nodeIdx) {
    mflex *m = mflexNewNoCompress();
    reallocIncrCountAfter(ml, nodeIdx, m);

    multilistFullCompressRenew__(ml, state);

    return nodeIdx + 1;
}

DK_STATIC mlNodeId multilistFullInsertNodeBefore__(multilistFull *ml,
                                                   mflexState *state,
                                                   mlNodeId nodeIdx) {
    mflex *m = mflexNewNoCompress();
    reallocIncrCountBefore(ml, nodeIdx, m);

    multilistFullCompressRenew__(ml, state);

    return nodeIdx;
}

DK_STATIC mlNodeId multilistFullInsertNodeAfter__Empty(multilistFull *ml,
                                                       mlNodeId nodeIdx) {
    mflex *m = NULL;
    reallocIncrCountAfter(ml, nodeIdx, m);
    /* we don't recompress here because the new node is empty
     * and the data field will get replaced after we return */
    return nodeIdx + 1;
}

DK_STATIC mlNodeId multilistFullInsertNodeBefore__Empty(multilistFull *ml,
                                                        mlNodeId nodeIdx) {
    mflex *m = NULL;
    reallocIncrCountBefore(ml, nodeIdx, m);
    /* we don't recompress here because the new node is empty
     * and the data field will get replaced after we return */
    return nodeIdx;
}

DK_STATIC mlNodeId multilistFullInsertNodeEmpty__(multilistFull *ml,
                                                  mlNodeId nodeIdx,
                                                  bool after) {
    if (after) {
        return multilistFullInsertNodeAfter__Empty(ml, nodeIdx);
    }

    return multilistFullInsertNodeBefore__Empty(ml, nodeIdx);
}

DK_STATIC bool mflexAllowInsert_(const multilistFull *ml, mlNodeId nodeIdx,
                                 size_t bytes) {
    if (!mlIdxExists(ml, nodeIdx)) {
        return false;
    }

    const mflex *node = getNode(nodeIdx);
    return flexCapAllowInsert(mflexBytesUncompressed(node), ml->fill, bytes);
}

/* Return flex to insert new element of 'bytes' at head, which may also
 * involve creating a new head element if adding 'bytes' to head would make
 * head grow too big. */
DK_STATIC inline mflex *
_multilistFullMflexForHead(multilistFull *ml, mflexState *state, size_t bytes) {
    if (!mflexAllowInsert_(ml, mlHeadIdx(ml), bytes)) {
        multilistFullInsertNodeBefore__(ml, state, mlHeadIdx(ml));
    }

    return mlHead(ml);
}

DK_STATIC inline mflex *
_multilistFullMflexForTail(multilistFull *ml, mflexState *state, size_t bytes) {
    if ((!mflexAllowInsert_(ml, mlTailIdx(ml), bytes))) {
        multilistFullInsertNodeAfter__(ml, state, mlTailIdx(ml));
    }

    return mlTail(ml);
}

/* Add new entry to head node of multilistFull. */
void multilistFullPushByTypeHead(multilistFull *ml, mflexState *state,
                                 const databox *box) {
    (void)_multilistFullMflexForHead(ml, state, DATABOX_SIZE(box));

    mflexPushByType(mlHeadPtr(ml), state, box, FLEX_ENDPOINT_HEAD);

    ml->values++;
}

/* Add new entry to tail node of multilistFull. */
void multilistFullPushByTypeTail(multilistFull *ml, mflexState *state,
                                 const databox *box) {
    (void)_multilistFullMflexForTail(ml, state, DATABOX_SIZE(box));

    mflexPushByType(mlTailPtr(ml), state, box, FLEX_ENDPOINT_TAIL);

    ml->values++;
}

/* Note: *keeps* 'fl' inside new node. */
void multilistFullAppendFlex(multilistFull *ml, flex *fl) {
    multilistFullInsertNodeAfter__Empty(ml, mlTailIdx(ml));

    ml->values += flexCount(fl);

    mflex **node = mlTailPtr(ml);
    *node = mflexConvertFromFlexNoCompress(fl);
}

multilistFull *multilistFullNewFromFlexConsumeGrow(void *_ml, mflexState *state,
                                                   flex *fl[], size_t flCount,
                                                   const uint32_t depth,
                                                   const uint32_t fillLimit) {
    multilistFull *ml = zrealloc(_ml, sizeof(*ml));

    /* Clean our fields... */
    memset(ml, 0, sizeof(*ml));

    /* create initial mulitlistFull structure... */
    const size_t f0Count = flexCount(fl[0]);
    assert(f0Count > 0);

    if (f0Count == 1) {
        mflex *m = mflexConvertFromFlexNoCompress(fl[0]);
        reallocIncrCountBefore(ml, 0, m);
        ml->values += flexCount(fl[0]);
    } else {
        flex *f0 = fl[0];
        flex *f1 = flexSplit(&f0, 1);
        mflex *m0 = mflexConvertFromFlexNoCompress(f0);
        reallocIncrCountBefore(ml, 0, m0);

        ml->values += flexCount(f0);
        multilistFullAppendFlex(ml, f1);
    }

    multilistFullSetDepth(ml, fillLimit, depth);

    for (size_t i = 1; i < flCount; i++) {
        flex *firstHalf = fl[i];
        const size_t iCount = flexCount(firstHalf);

        if (iCount == 0) {
            /* No entries? no actions! */
            continue;
        }

        if (iCount == 1) {
            /* inserting flex with one element, so don't split into two nodes */
            multilistFullAppendFlex(ml, firstHalf);
        } else {
            /* inserting a flex with more than one element, so split! */
            flex *secondHalf = flexSplit(&firstHalf, 1);

            multilistFullAppendFlex(ml, firstHalf);
            multilistFullAppendFlex(ml, secondHalf);
        }

        multilistFullCompressRenew__(ml, state);
    }

    return ml;
}

/* Append all values of flex 'fl' individually into 'multilistFull'.
 *
 * Returns 'multilistFull' argument. */
multilistFull *multilistFullAppendValuesFromFlex(multilistFull *ml,
                                                 mflexState *state,
                                                 const flex *fl) {
    databox holder = {{0}};
    flexEntry *fe = flexHead(fl);

    while (fe) {
        flexGetByType(fe, &holder);
        multilistFullPushByTypeTail(ml, state, &holder);
        fe = flexNext(fl, fe);
    }

    return ml;
}

/* Create new (potentially multi-node) multilistFull from
 * a single existing flex.
 *
 * Returns new multilistFull. */
multilistFull *multilistFullNewFromFlex(int fill, int compress,
                                        mflexState *state, const flex *fl) {
    return multilistFullAppendValuesFromFlex(multilistFullNew(fill, compress),
                                             state, fl);
}

DK_STATIC void multilistFullDelNode__(multilistFull *ml, mflexState *state,
                                      mlNodeId nodeIdx) {
    mflex **node = getNodePtr(nodeIdx);

    ml->values -= mflexCount(*node);

    if (ml->count == 1) {
        /* We always leave one node available, so only delete contents. */
        mflexReset(node);
    } else {
        reallocDecrCount(ml, nodeIdx);

        /* If we deleted a node within our compress depth, we
         * now have compressed nodes needing to be decompressed. */
        multilistFullCompressRenew__(ml, state);
    }
}

/* Delete one entry from list given the node for the entry and a pointer
 * to the entry in the node.
 *
 * Note: multilistFullDelIndex() *requires* uncompressed nodes because you
 *       already had to get *fe from an uncompressed node somewhere.
 *
 * Returns true if the entire node was deleted, false if node still exists.
 * Also updates in/out param 'fe' with the next offset in the flex. */
DK_STATIC bool multilistFullDelIndex(multilistFull *ml, const mlNodeId nodeIdx,
                                     flex **ff, flexEntry **fe) {
    flexDelete(ff, fe);
    ml->values--;

    if (flexIsEmpty(*ff) && ml->count > 1) {
        /* Only delete node if it's not the only node. */
        flexFree(*ff);
        *getNodePtr(nodeIdx) = NULL;
        reallocDecrCount(ml, nodeIdx);

        *ff = NULL;
        *fe = NULL;

        /* return 'true' to signal the entire node is now deleted */
        return true;
    }

    /* return 'false' to signal node still exists */
    return false;
}

/* Delete one element represented by 'entry'
 *
 * 'entry' stores enough metadata to delete the proper position in
 * the correct flex in the correct multilistFull node. */
void multilistFullDelEntry(multilistIterator *iter, multilistEntry *entry) {
    assert(!iter->readOnly);

    multilistFullDelIndex(iter->ml, iter->nodeIdx, &entry->f, &entry->fe);

    iter->f = entry->f;

    /* Only update iter->fe *if* it currently exists.
     * Otherwise, the next entry is the end and we don't care about replacing
     * it. */
    if (iter->fe) {
        if (!iter->forward && entry->fe) {
            /* update for reverse direction */
            iter->fe = flexPrev(iter->f, entry->fe);
        } else {
            /* update for forward direction */
            iter->fe = entry->fe;
        }
    }
}

/* Replace multilistFull entry at offset 'index' by 'data' with length 'bytes'.
 *
 * Returns true if replace happened.
 * Returns false if replace failed and no changes happened. */
bool multilistFullReplaceByTypeAtIndex(multilistFull *ml, mflexState *state,
                                       const mlOffsetId index,
                                       const databox *box) {
    multilistEntry entry;
    /* FullIndex opens the mflex, providing flex as entry.f */
    if (multilistFullIndexGet(ml, state, index, &entry)) {
        /* multilistFullIndex provides an uncompressed node */

        mflex **node = getNodePtr(entry.nodeIdx);
        flexReplaceByType(&entry.f, entry.fe, box);
        mflexCloseGrow(node, state, entry.f);
        return true;
    }

    return false;
}

/* Given two nodes, merge their inner lists.
 *
 * Note: 'a' must be to the LEFT of 'b'.
 * (e.g. [A, B, C, D] merge(A, B) creates: [AB, C, D])
 *
 * Returns true if merge happened. */
DK_STATIC bool multilistFullMflexMerge_(multilistFull *ml, mflexState *state[2],
                                        mlNodeId nodeIdxA, mlNodeId nodeIdxB) {
    mflex **aa = getNodePtr(nodeIdxA);
    mflex **bb = getNodePtr(nodeIdxB);

    flex *a = mflexOpen(*aa, state[0]);
    flex *b = mflexOpen(*bb, state[1]);

    D("Requested merge (a,b) (%zu, %zu)", flexCount(a), flexCount(b));
#if 0
    if (flexMerge(&a, &b)) {
        /* We merged flexes! Now remove the unused mflex. */
        mlNodeId deletedIdx;
        mlNodeId keptIdx;
        flex *keep;
        mflexState *keepState;

        if (a) {
            keptIdx = nodeIdxA;
            deletedIdx = nodeIdxB;
            keep = a;
            keepState = state[0];
        } else {
            keptIdx = nodeIdxB;
            deletedIdx = nodeIdxA;
            keep = b;
            keepState = state[1];
        }

        /* If layout is [Keep, Delete], keep doesn't change index.
         * If layout is [Delete, Keep], keep moves back one index. */
        reallocDecrCount(ml, deletedIdx);
        if (deletedIdx < keptIdx) {
            keptIdx--;
        }

        mflexCloseGrow(getNodePtr(keptIdx), keepState, keep);
        return true;
    }

    return false;
#else
    flexBulkAppendFlex(&a, b);
    mflexCloseGrow(aa, state[0], a);
    reallocDecrCount(ml, nodeIdxB);
    return true;
#endif
}

DK_STATIC inline bool multilistFullIsMergeable_(const multilistFull *ml,
                                                mlNodeId nodeIdxA,
                                                mlNodeId nodeIdxB) {
    const mflex *a = getNode(nodeIdxA);
    const mflex *b = getNode(nodeIdxB);
#if 1
    return flexCapIsMergeable(mflexBytesUncompressed(a),
                              mflexBytesUncompressed(b), ml->fill);
#else
    return (mflexBytesUncompressed(a) + mflexBytesUncompressed(b)) <= 4096;
#endif
}

/* Attempt to merge flexes within two nodes on either side of 'center'.
 *
 * We attempt to merge:
 *   - (center->prev->prev, center->prev)
 *   - (center->next, center->next->next)
 *   - (center->prev, center)
 *   - (center, center->next)
 */
DK_STATIC void multilistFullMergeNodes_(multilistFull *ml, mflexState *state[2],
                                        const mlNodeId nodeIdxCenter) {
    const mlNodeId tail = mlTailIdx(ml);

    mlNodeId center = nodeIdxCenter;
    mlNodeId prev = center - 1;
    mlNodeId next = center + 1;
    mlNodeId prevPrev = prev - 1;
    mlNodeId nextNext = next + 1;

    /* addition and subtraction bounds restrictions */
    if (prev < 0) {
        prev = prevPrev = 0;
    } else if (prevPrev < 0) {
        prevPrev = 0;
    }

    if (next > tail) {
        next = nextNext = tail;
    } else if (nextNext > tail) {
        nextNext = tail;
    }

    /* Try to merge prevPrev and prev */
    if (prevPrev != prev && multilistFullIsMergeable_(ml, prevPrev, prev)) {
        if (multilistFullMflexMerge_(ml, state, prevPrev, prev)) {
            next--;
            nextNext--;
            center--;
        }
    }

    /* Try to merge next and nextNext */
    if (next != nextNext && multilistFullIsMergeable_(ml, next, nextNext)) {
        if (multilistFullMflexMerge_(ml, state, next, nextNext)) {
            /* We don't care because this is 'after' our center. */
        }
    }

    /* Try to merge center node and previous node */
    if (prev != center && multilistFullIsMergeable_(ml, prev, center)) {
        if (multilistFullMflexMerge_(ml, state, prev, center)) {
            next--;
            center--;
        }
    }

    /* Use result of center merge (or original) to merge with next node. */
    if (center != next && multilistFullIsMergeable_(ml, center, next)) {
        multilistFullMflexMerge_(ml, state, center, next);
    }
}

/* Split 'node' into two parts, parameterized by 'offset' and 'after'.
 *
 * The 'after' argument controls which mflex gets returned.
 * If 'after', returned node has elements after 'offset'.
 *             input node keeps elements up to 'offset', including 'offset'.
 * If !'after', returned node has elements up to 'offset', including 'offset'.
 *              input node keeps elements after 'offset'.
 *
 * If 'after', returned node will have elements _after_ 'offset'.
 *             The returned node will have elements [OFFSET+1, END].
 *             The input node keeps elements [0, OFFSET].
 *
 * If !'after', returned node will keep elements up to and including 'offset'.
 *             The returned node will have elements [0, OFFSET].
 *             The input node keeps elements [OFFSET+1, END].
 *
 * The input node keeps all elements not taken by the returned node.
 *
 * Returns index of newly allocated nodeId */
/* Worker function for _multilistFullSplitNode().
 * Note: accepts *open* node/flex as input, then *closes* before returning. */
DK_STATIC mlNodeId multilistFullSplitNodeFromOpen_(
    multilistFull *ml, mflexState *state, mflex **node, flex **ff,
    const mlNodeId nodeIdx, const int offset, const bool after) {
    /* -1 here means "continue deleting until the list ends" */
    const int origStart = after ? offset + 1 : 0;
    const int origExtent = after ? -1 : offset;

    D("After %d (%d); ranges: [%d, %d]", after, offset, origStart, origExtent);

    flex *deletedContents = flexSplitRange(ff, origStart, origExtent);
    mflexCloseShrink(node, state, *ff);

    const mlNodeId newIdx = multilistFullInsertNodeEmpty__(ml, nodeIdx, after);
    mflex **newNode = getNodePtr(newIdx);
    *newNode = mflexConvertFromFlexNoCompress(deletedContents);

    return newIdx;
}

__attribute__((unused)) DK_STATIC mlNodeId _multilistFullSplitNode(
    multilistFull *ml, mflexState *state, const mlNodeId nodeIdx,
    const int offset, const bool after) {
    mflex **node = getNodePtr(nodeIdx);
    flex *f = mflexOpen(*node, state);

#if VERIFY_SPLIT_RESULT
    const int prevalues = mflexCount(*node);
#endif

    const mlNodeId newIdx = multilistFullSplitNodeFromOpen_(
        ml, state, node, &f, nodeIdx, offset, after);

#if VERIFY_SPLIT_RESULT
    assert(prevalues ==
           getNode(after ? nodeIdx : nodeIdx - 1)->values + newNode->values);
#endif

    D("After split lengths: orig (%zu), new (%zu)",
      mflexCount(getNode(after ? nodeIdx : nodeIdx - 1)),
      mflexCount(getNode(newIdx)));

    return newIdx;
}

/* Insert a new entry before or after existing entry 'entry'.
 *
 * If 'after', new value is inserted after 'entry',
 * If !'after', new value is inserted before 'entry'. */
DK_STATIC void multilistFullInsert_(multilistFull *ml, mflexState *state[2],
                                    const multilistEntry *entry,
                                    const databox *box, const bool after) {
    mflex **node = getNodePtr(entry->nodeIdx);
    flex *f = entry->f;

    bool full = false;
    /* Populate accounting flag for easier boolean checks later */
    if (!mflexAllowInsert_(ml, entry->nodeIdx, DATABOX_SIZE(box))) {
        D("Current node is full with values %zu and requested fill %" PRIu16,
          mflexCount(*node), ml->fill);
        full = true;
    }

    /* Note: this 'Insert' is placed at the result of a
     *       multilistFullIndexGet() call (described by 'entry')
     *       and multilistFullIndexGet() already decompressed the
     *       target node for us, so we don't need to deal with
     *       any decompress/recompress here. */
    if (full) {
        /* node is full we need to split it. */
        /* covers both after and !after cases */
        D("\tsplitting node [%d], %d...", entry->nodeIdx, entry->offset);
        mlNodeId newIdx = multilistFullSplitNodeFromOpen_(
            ml, state[0], node, &f, entry->nodeIdx, entry->offset, after);

        /* Insert into new split node */
        mflex **newNode = getNodePtr(newIdx);
        flex *newF = mflexOpen(*newNode, state[0]);
        flexPushByType(&newF, box,
                       after ? FLEX_ENDPOINT_HEAD : FLEX_ENDPOINT_TAIL);
        mflexCloseGrow(newNode, state[0], newF);

        ml->values++;

        multilistFullMergeNodes_(ml, state, entry->nodeIdx);
        multilistFullCompressMiddle__(ml, state[0], newIdx);
    } else {
        if (after) {
            D("Not full, inserting after current position.");
            flexEntry *next = flexNext(f, entry->fe);
            if (next == NULL) {
                flexPushByType(&f, box, FLEX_ENDPOINT_TAIL);
            } else {
                flexInsertByType(&f, next, box);
            }
        } else {
            D("Not full, inserting before current position.");
            flexInsertByType(&f, entry->fe, box);
        }

        mflexCloseGrow(node, state[0], f);
        ml->values++;
    }
}

void multilistFullInsertByTypeBefore(multilistFull *ml, mflexState *state[2],
                                     const multilistEntry *entry,
                                     const databox *box) {
    multilistFullInsert_(ml, state, entry, box, false);
}

void multilistFullInsertByTypeAfter(multilistFull *ml, mflexState *state[2],
                                    const multilistEntry *entry,
                                    const databox *box) {
    multilistFullInsert_(ml, state, entry, box, true);
}

/* Delete a range of elements from the multilistFull.
 *
 * Returns true if one or more entries were deleted.
 * Returns false if no entires were deleted. */
bool multilistFullDelRange(multilistFull *ml, mflexState *state,
                           const mlOffsetId start, const int64_t values) {
    if (values <= 0 || ml->values == 0) {
        return false;
    }

    mlOffsetId extent = values; /* range is inclusive of start position */

    if (start >= 0 && extent > (ml->values - start)) {
        /* if requesting delete more elements than exist, limit to list size. */
        extent = ml->values - start;
    } else if (start < 0 && extent > (-start)) {
        /* else, if at negative offset, limit max size to rest of list. */
        extent = -start;
    }

    multilistEntry entry;
    if (!multilistFullIndexCheck(ml, state, start, &entry)) {
        return false;
    }

    D("multilistFull delete request for start %" PRId64 ", values %" PRId64
      ", extent: %" PRId64,
      start, values, extent);

    /* iterate over next nodes until 'extent' elements are deleted. */
    for (mlNodeId nodeIdx = entry.nodeIdx; extent; nodeIdx++) {
        mflex **node = getNodePtr(nodeIdx);
        const mlOffsetId nodeCount = mflexCount(*node);

        uint32_t del;
        if (entry.offset == 0 && extent >= nodeCount) {
            /* If we are deleting more than the values of this node, we
             * can just delete the entire node without flex math. */
            del = nodeCount;
        } else if (entry.offset >= 0 && (entry.offset + extent) >= nodeCount) {
            /* If deleting more nodes after this one, calculate delete based
             * on size of current node. */
            del = nodeCount - entry.offset;
        } else if (entry.offset < 0) {
            /* If offset is negative, we are in the first run of this loop
             * and we are deleting the entire range from this start offset
             * to end of list.  Since the Negative offset is the number
             * of elements until the tail of the list, just use it directly
             * as the deletion values. */
            del = -entry.offset;

            /* If the positive offset is greater than the remaining extent,
             * we only delete the remaining extent, not the entire offset. */
            if (del > extent) {
                del = extent;
            }
        } else {
            /* else, we are deleting less than the extent of this node, so
             * use extent directly. */
            del = extent;
        }

        D("[%" PRId64
          "]: asking to del: %d because offset: %d; (ENTIRE NODE: %d), "
          "node values: %zu",
          extent, del, entry.offset, del == mflexCount(*node),
          mflexCount(*node));

        if (del == nodeCount) {
            /* If delete request is every value in this node,
             * delete the entire node. */
            multilistFullDelNode__(ml, state, nodeIdx);

            /* We just deleted a node, so nodeIdx needs to drop by one. */
            nodeIdx--;
        } else {
            mflexDeleteOffsetCount(node, state, entry.offset, del);
            ml->values -= del;
        }

        extent -= del;
        entry.offset = 0;
    }

    return true;
}

/* Populates a multilistFull iterator.
 *
 * Every call to multilistFullNext() will return the next element of the
 * multilistFull based on this iterator. */
void multilistFullIteratorInit(multilistFull *ml, mflexState *state[2],
                               multilistIterator *iter, const bool forward,
                               const bool readOnly) {
    if (forward) {
        iter->nodeIdx = 0;
        iter->offset = 0;
    } else {
        iter->nodeIdx = mlTailIdx(ml);
        iter->offset = -1;
    }

    iter->state[0] = state[0];
    iter->state[1] = state[1];
    iter->forward = forward;
    iter->ml = ml;

    iter->f = mflexOpen(getNode(iter->nodeIdx), state[0]);
    iter->fe = flexIndexDirect(iter->f, iter->offset);

    iter->readOnly = readOnly;
}

/* Initialize an iterator at a specific offset 'idx'.
 *
 * multilistFullNext() elements will be returned in the order
 * requested by 'forward' */
bool multilistFullIteratorInitAtIdx(multilistFull *ml, mflexState *state[2],
                                    multilistIterator *iter,
                                    const mlOffsetId idx, const bool forward,
                                    const bool readOnly) {
    multilistEntry entry = {0};

    if (multilistFullIndexGet(ml, state[0], idx, &entry)) {
        iter->nodeIdx = entry.nodeIdx;
        iter->offset = entry.offset;

        iter->state[0] = state[0];
        iter->state[1] = state[1];
        iter->forward = forward;
        iter->ml = ml;

        iter->f = entry.f;
        iter->fe = entry.fe;

        iter->readOnly = readOnly;

        return true;
    }

    return false;
}

void multilistFullIteratorRelease(multilistIterator *iter) {
    if (!iter->readOnly && iter->f) {
        mflexCloseGrow(getNodeMlPtr(iter->ml, iter->nodeIdx), iter->state[0],
                       iter->f);
    }
}

/* Get next element in iterator.
 *
 * Note: You must NOT insert into the list while iterating over it.
 * You *may* delete from the list while iterating using the
 * multilistFullDelEntry() function.
 * If you insert into the multilistFull while iterating, you should
 * re-create the iterator after your addition.
 *
 * multilistFullIteratorInitForward(ml, &iter);
 * multilistEntry entry;
 * while (multilistFullNext(&iter, &entry)) {
 * }
 *
 * Populates 'entry' with value for this iteration.
 * Returns false when iteration is complete or when iteration fails. */
bool multilistFullNext(multilistIterator *iter, multilistEntry *entry) {
    mlNodeId nodeIdx = iter->nodeIdx;
    const multilistFull *ml = iter->ml;

    if (iter->fe) {
        /* Populate value from existing flex position */
        flexGetByType(iter->fe, &entry->box);

        entry->ml = iter->ml;
        entry->nodeIdx = iter->nodeIdx;

        entry->f = iter->f;
        entry->fe = iter->fe;
        entry->offset = iter->offset;

        /* use existing iterator offset and get prev/next as necessary. */
        if (iter->forward) {
            iter->fe = flexNext(iter->f, iter->fe);
            iter->offset += 1;
        } else {
            iter->fe = flexPrev(iter->f, iter->fe);
            iter->offset -= 1;
        }

        return true;
    }

    /* If no current entry (meaning reached beyond head or tail),
     * but current flex, then save flex back to mflex, get new offset
     * then get next entry again .*/
    if (!iter->readOnly && iter->f) {
        mflexCloseShrink(getNodePtr(nodeIdx), iter->state[0], iter->f);
        iter->f = NULL;
    }

    if (iter->forward) {
        /* Forward traversal */
        D("Jumping to start of next node");
        iter->nodeIdx++;
        iter->offset = 0;
    } else {
        /* Reverse traversal */
        D("Jumping to end of previous node");
        iter->nodeIdx--;
        iter->offset = -1;
    }

    nodeIdx = iter->nodeIdx;

    if (!mlIdxExists(ml, nodeIdx)) {
        D("Returning because current node is out of bounds");
        return false;
    }

    iter->f = mflexOpen(getNode(nodeIdx), iter->state[0]);
    iter->fe = flexIndexDirect(iter->f, iter->offset);

    /* If we didn't get a next entry, no more entries exist anywhere. */
    if (!iter->fe) {
        return false;
    }

    return multilistFullNext(iter, entry);
}

/* Duplicate the multilistFull.
 * On success a copy of the original multilistFull is returned.
 *
 * The original multilistFull is not modified.
 *
 * Returns newly allocated multilistFull. */
multilistFull *multilistFullDuplicate(multilistFull *orig) {
    multilistFull *copy = multilistFullNew(orig->fill, orig->compress);

    /* Our multilistFull comes with an initial empty flex, but we
     * replace it below, so go ahead and get it out of the way
     * up front before we overwrite it and it leaks. */
    mflexFree(getNodeMl(copy, 0));

    for (mlNodeId nodeIdx = 0; nodeIdx < orig->count; nodeIdx++) {
        const mflex *origNode = getNodeMl(orig, nodeIdx);
        mflex **copyNode = getNodeMlPtr(copy, nodeIdx);

        *copyNode = mflexDuplicate(origNode);
        copy->values += mflexCount(*copyNode);

        if (nodeIdx + 1 < orig->count) {
            /* Create next node (if the next iteration isn't beyond nodes) */
            multilistFullInsertNodeAfter__Empty(copy, nodeIdx);
        }
    }

    return copy;
}

/* Populate 'entry' with the element at the specified zero-based index.
 * Negative integers count from the tail (-1 = tail, etc).
 *
 * Returns true if element found ('entry' populated).
 * Returns false if element out of range ('entry' not usable). */
bool multilistFullIndex(multilistFull *ml, mflexState *state, mlOffsetId index,
                        multilistEntry *entry, const bool openNode) {
    mlNodeId nodeIdx = 0;
    const mlOffsetId originalIndex = index;

#if 1
    /* Pre-process 'index' to determine if it would be faster to Iterate
     * from the other side of the list as was requested. */
    const mlOffsetId count = ml->values;
    const mlOffsetId halfCount = count / 2;
    if (index >= 0) {
        if (index < count && index > halfCount) {
            /* If we're using a forward index for an element more than half
             * way through the list, convert to a reverse traversal. */
            index = -(count - index);
        }
    } else {
        if ((-index) <= count && (-index) > halfCount) {
            /* If we're using a reverse index for an element more than half
             * way through the list, convert to a forward traversal. */
            index += count;
        }
    }
#endif

    const bool reverse = index < 0 ? true : false; /* negative == reverse */
    if (reverse) {
        index = (-index) - 1;
        nodeIdx = mlTailIdx(ml);
    }

    if (ml->values > 0 && index >= ml->values) {
        return false;
    }

    mflex *currentNode = getNode(nodeIdx);
    mlOffsetId currentCount = mflexCount(currentNode);
    if (ml->count == 1) {
        /* nodeIdx is 0 by default */
        entry->offset = originalIndex;
    } else {
        mlOffsetId accum = 0;
        while ((accum + currentCount) <= index) {
            D("Skipping over (%p) %zu at accum %" PRId64 "",
              (void *)currentNode, mflexCount(currentNode), accum);
            accum += currentCount;
            if (reverse) {
                nodeIdx--;
            } else {
                nodeIdx++;
            }
            currentNode = getNode(nodeIdx);
            currentCount = mflexCount(currentNode);
        }

        D("Found node: %p at accum %" PRId64 ", idx %" PRId64 ", sub+ %" PRId64
          ", sub- %" PRId64,
          (void *)currentNode, accum, index, index - accum,
          (-index) - 1 + accum);

        if (reverse) {
            /* reverse = need negative offset for tail-to-head, so undo
             * the result of the original if (index < 0) above. */
            entry->offset = (-index) - 1 + accum;
        } else {
            /* forward = normal head-to-tail offset. */
            entry->offset = index - accum;
        }
    }

    entry->ml = ml;
    entry->nodeIdx = nodeIdx;

    if (openNode) {
        /* The caller will use our result, so we don't re-compress here.
         * The caller can recompress or delete the node as needed. */
        entry->f = mflexOpen(currentNode, state);
        entry->fe = flexIndex(entry->f, entry->offset);

        if (!entry->fe) {
            entry->fe = flexHead(entry->f);
            return false;
        }

        flexGetByType(entry->fe, &entry->box);
        return true;
    }

    /* else, return true because we know the bounds exist,
     * but we didn't get them. */
    return true;
}

/* Rotate multilistFull by moving the tail element to the head. */
void multilistFullRotate(multilistFull *ml, mflexState *state[2]) {
    if (ml->values <= 1) {
        return;
    }

    assert(!mflexIsCompressed(mlTail(ml)));

    if (ml->values == 2 && ml->count == 2) {
        /* If two values and two nodes, just swap node positions. */
        mflex *tmp;
        mflex **head = mlHeadPtr(ml);
        mflex **tail = mlTailPtr(ml);

        tmp = *tail;
        *tail = *head;
        *head = tmp;

        return;
    }

    /* Get tail entry position */
    mflex **tail = mlTailPtr(ml);
    flex *fTail = mflexOpen(*tail, state[0]);
    flexEntry *fe = flexTail(fTail);

    /* Get tail entry */
    databox box = {{0}};
    flexGetByType(fe, &box);

    /* Copy tail entry to head (must happen before tail is deleted) */
    if (ml->count > 1) {
        /* If more than one node, then head != tail and we can add
         * to the head mflex. */
        multilistFullPushByTypeHead(ml, state[1], &box);

        /* a new head could have been created, causing the tail to
         * move its origin pointer position. */
        tail = mlTailPtr(ml);

        if (mflexCount(*tail) == 1) {
            /* tail has one element, but we just copied it to the head,
             * so just delete entire tail. */
            reallocDecrCount(ml, mlTailIdx(ml));

            /* uncompress new tail */
            mflexSetCompressNever(mlTailPtr(ml), state[0]);

            /* mark value as removed it got added when inserted
             * to head of the list above. */
            ml->values--;
        } else {
            /* Remove tail entry. */
            multilistFullDelIndex(ml, mlTailIdx(ml), &fTail, &fe);

            /* re-store 'fTail' in 'tail' */
            mflexCloseNoCompress(tail, state[0], fTail);
        }

        multilistFullCompressRenew__(ml, state[0]);
    } else {
        /* else, multilistFull only has one node, the head flex is also the
         * tail flex, and we already opened it, so just use it directly. */
        flexPushByType(&fTail, &box, FLEX_ENDPOINT_HEAD);

        /* now get new tail entry again since it just moved */
        fe = flexTail(fTail);

        /* delete tail */
        flexDeleteOffsetCount(&fTail, -1, 1);

        /* put modified 'fTail' back into 'tail' */
        mflexCloseNoCompress(tail, state[0], fTail);
    }
}

/* pop from multilistFull head or tail.
 *
 * Return value of false means no elements available. */
bool multilistFullPop(multilistFull *ml, mflexState *state, databox *box,
                      const bool fromTail) {
    if (ml->values == 0) {
        return false;
    }

    mflex **node;
    flex *f;
    mlNodeId nodeIdx;
    flexEndpoint flIdx;

    if (fromTail) {
        /* tail */
        nodeIdx = mlTailIdx(ml);
        node = mlTailPtr(ml);
        flIdx = FLEX_ENDPOINT_TAIL;
    } else {
        /* else, head */
        nodeIdx = mlHeadIdx(ml);
        node = mlHeadPtr(ml);
        flIdx = FLEX_ENDPOINT_HEAD;
    }

    f = mflexOpen(*node, state);
    flexEntry *fe = flexHeadOrTail(f, flIdx);

    if (fe) {
        flexGetByTypeCopy(fe, box);

        if (!multilistFullDelIndex(ml, nodeIdx, &f, &fe)) {
            mflexCloseShrink(node, state, f);
        }

        return true;
    }

    return false;
}

/* ====================================================================
 * Testing
 * ==================================================================== */
#if 0
#ifndef DATAKIT_TEST
#define DATAKIT_TEST
#endif
#endif

#ifdef DATAKIT_TEST
/* ====================================================================
 * Debuggles
 * ==================================================================== */
void multilistFullRepr(multilistFull *ml) {
    mflexState *state = mflexStateCreate();

    for (mlNodeId nodeIdx = 0; nodeIdx < ml->count; nodeIdx++) {
        const mflex *node = getNodeMl(ml, nodeIdx);
        flex *f = mflexOpen(node, state);
        flexRepr(f);
    }

    mflexStateFree(state);
}

#define multilistFullPush_(WHAT, a, b, c, d)                                   \
    do {                                                                       \
        databox pushBox_ = databoxNewBytesString(c);                           \
        pushBox_.len = d;                                                      \
        multilistFullPushByType##WHAT(a, b, &pushBox_);                        \
    } while (0)

#define multilistFullPushHead(a, b, c, d) multilistFullPush_(Head, a, b, c, d)
#define multilistFullPushTail(a, b, c, d) multilistFullPush_(Tail, a, b, c, d)

bool multilistFullReplaceAtIndex(multilistFull *ml, mflexState *state,
                                 mlOffsetId index, void *data, size_t len) {
    return multilistFullReplaceByTypeAtIndex(ml, state, index,
                                             &DATABOX_WITH_BYTES(data, len));
}

static void multilistFullInsertBefore(multilistFull *ml, mflexState *state[2],
                                      multilistEntry *entry, void *data,
                                      size_t len) {
    multilistFullInsertByTypeBefore(ml, state, entry,
                                    &DATABOX_WITH_BYTES(data, len));
}

static void multilistFullInsertAfter(multilistFull *ml, mflexState *state[2],
                                     multilistEntry *entry, void *data,
                                     size_t len) {
    multilistFullInsertByTypeAfter(ml, state, entry,
                                   &DATABOX_WITH_BYTES(data, len));
}

#include "ctest.h"
#include "str.h" /* for StrInt64ToBuf */
#include "timeUtil.h"

CTEST_INCLUDE_GEN(str)

#define yell(str, ...)                                                         \
    do {                                                                       \
        printf("ERROR! " str "\n\n", __VA_ARGS__);                             \
        assert(NULL);                                                          \
    } while (0)

#define OK printf("\tOK\n")

#define ML_TEST_VERBOSE 0

__attribute__((unused)) static void compressedRepr(multilistFull *ml) {
    printf("[");
    for (mlNodeId depth = 0; depth < ml->count; depth++) {
        printf("%s, ", mflexIsCompressed(getNode(depth)) ? "C" : "U");
    }
    printf("\b\b]\n");
}

static void mlInfo(multilistFull *ml) {
#if ML_TEST_VERBOSE
    printf("Container length (nodes): %lu\n", ml->count);
    printf("Container values (elements): %lu\n", ml->values);
    if (mlHead(ml)) {
        printf("\t(elements in head: %zu)\n", flexCount(mlHead(ml)->data.fl));
    }

    if (mlTail(ml)) {
        printf("\t(elements in tail: %zu)\n", flexCount(mlTail(ml)->data.fl));
    }

    if (mlHead(ml) == mlTail(ml)) {
        printf("\tHEAD==TAIL\n");
    }

    printf("\n");
#else
    (void)ml;
#endif
}

/* Iterate over an entire multilistFull.
 *
 * Returns physical count of elements found by iterating over the list. */
static int itrprintr_(multilistFull *ml, mflexState *state[2], bool print,
                      bool forward) {
    multilistIterator iter = {0};

    multilistFullIteratorInitReadOnly(ml, state, &iter, forward);
    multilistEntry entry = {0};
    uint32_t i = 0;
    while (multilistFullNext(&iter, &entry)) {
        if (i > ml->values) {
            assert(NULL);
        }

        if (print) {
            printf("[%3d (%2d)]: [%.*s] (%" PRIi64 ")\n", i, entry.nodeIdx,
                   (int32_t)entry.box.len, entry.box.data.bytes.cstart,
                   entry.box.data.i64);
        }

        i++;
    }

    return i;
}

static int itrprintr(multilistFull *ml, mflexState *state[2], bool print) {
    return itrprintr_(ml, state, print, true);
}

static int itrprintrRev(multilistFull *ml, mflexState *state[2], bool print) {
    return itrprintr_(ml, state, print, false);
}

/* Passthrough to flexCompare(); for testing only */
int multilistFullCompare(flexEntry *p1, const void *val, int vcount) {
    return flexCompareBytes(p1, val, vcount);
}

#define mlVerify(a, b, c, d, e)                                                \
    do {                                                                       \
        err += mlVerify_(a, s, b, c, d, e);                                    \
    } while (0)

/* Verify list metadata matches physical list contents. */
static int mlVerify_(multilistFull *ml, mflexState *state[2], mlNodeId count,
                     uint32_t values, uint32_t headValues,
                     uint32_t tailValues) {
    int errors = 0;

#if 0
    printf("[");
    for (mlNodeId depth = 0; depth < ml->count; depth++) {
        printf("%s, ", mflexIsCompressed(getNode(depth)) ? "C" : "U");
    }
    printf("\b\b]\n");
#endif

    mlInfo(ml);
    if (values != ml->values) {
        yell("multilistFull values wrong: expected %d, got %" PRIu64 "", values,
             ml->values);
        errors++;
    }

#if 0
    if (count != ml->count) {
        yell("mflex count wrong: expected %d, got %u", count, ml->count);
        errors++;
    }

    if (headValues != mflexCount(mlHead(ml))) {
        yell("multilistFull head values wrong: expected %d, got %zu",
             headValues, mflexCount(mlHead(ml)));
        errors++;
    }

    if (tailValues != mflexCount(mlTail(ml))) {
        yell("multilistFull tail values wrong: expected %d, got %zu",
             tailValues, mflexCount(mlTail(ml)));
        errors++;
    }
#else
    /* We moved from optional entry-level accounting to
     * mandatory size-based accounting, so these checks
     * are no longer valid. */
    (void)count;
    (void)headValues;
    (void)tailValues;
#endif

    int loopr = itrprintr(ml, state, false);
    if (loopr != (int)ml->values) {
        yell("multilistFull cached values not match actual values: expected "
             "%" PRIu64 ", got actual "
             "%d",
             ml->values, loopr);
        errors++;
    }

    int rloopr = itrprintrRev(ml, state, false);
    if (loopr != rloopr) {
        yell("multilistFull has different forward values than reverse values!  "
             "Forward values is %d, reverse values is %d.",
             loopr, rloopr);
        errors++;
    }

    if (ml->count == 0 && !errors) {
        OK;
        return errors;
    }

    if (multilistFullAllowsCompression(ml)) {
        mlNodeId lowRaw = ml->compress;
        mlNodeId highRaw = ml->count - ml->compress;

        for (mlNodeId at = 0; at < ml->count; at++) {
            mflex *node = getNode(at);
            if (node && (at < lowRaw || at >= highRaw)) {
                if (mflexIsCompressed(node)) {
                    yell("Node %d is "
                         "compressed at depth %d ((%u, %u); total "
                         "nodes: %u; size (uncomp): %zu; size (comp): %zu)",
                         at, ml->compress, lowRaw, highRaw, ml->count,
                         mflexBytesUncompressed(node),
                         mflexBytesCompressed(node));
                    errors++;
                }
            } else {
                if (!mflexIsCompressed(node) &&
                    mflexBytesUncompressed(node) > 64) {
                    yell("Node %d is NOT "
                         "compressed at depth %d ((%u, %u); total "
                         "nodes: %u; size (uncomp): %zu; size (comp): %zu)",
                         at, ml->compress, lowRaw, highRaw, ml->count,
                         mflexBytesUncompressed(node),
                         mflexBytesCompressed(node));
                    errors++;
                }
            }
        }
    }

    if (!errors) {
        OK;
    }

    return errors;
}

/* main test, but callable from other files */
int multilistFullTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    uint32_t err = 0;

    const int depth[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const size_t depthCount = sizeof(depth) / sizeof(*depth);
    int64_t runtime[depthCount];
    const int defaultCompressSizeLimit = 1;

    mflexState *s0 = mflexStateCreate();
    mflexState *s1 = mflexStateCreate();
    mflexState *s[2] = {s0, s1};

    for (size_t _i = 0; _i < depthCount; _i++) {
        printf("Testing Option %d\n", depth[_i]);
        int64_t start = timeUtilMs();

        TEST("create list") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            mlVerify(ml, 1, 0, 0, 0);
            multilistFullFree(ml);
        }

        TEST("add to tail of empty list") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            const databox pushBox = databoxNewBytesString("hello");
            multilistFullPushByTypeTail(ml, s0, &pushBox);
            /* 1 for head and 1 for tail beacuse 1 node = head = tail */
            mlVerify(ml, 1, 1, 1, 1);
            multilistFullFree(ml);
        }

        TEST("add to head of empty list") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            const databox pushBox = databoxNewBytesString("hello");
            multilistFullPushByTypeHead(ml, s0, &pushBox);
            /* 1 for head and 1 for tail beacuse 1 node = head = tail */
            mlVerify(ml, 1, 1, 1, 1);
            multilistFullFree(ml);
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("add to tail 5x at fill %zu at compress %d", f,
                      depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                for (int i = 0; i < 5; i++) {
                    databox pushBox = databoxNewBytesString(genstr("hello", i));
                    pushBox.len = 32;
                    multilistFullPushByTypeTail(ml, s0, &pushBox);
                }

                if (ml->values != 5) {
                    ERROR;
                }

                if (f == 32) {
                    mlVerify(ml, 1, 5, 5, 5);
                }

                multilistFullFree(ml);
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("add to head 5x at fill %zu at compress %d", f,
                      depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                for (int i = 0; i < 5; i++) {
                    databox pushBox = databoxNewBytesString(genstr("hello", i));
                    pushBox.len = 32;
                    multilistFullPushByTypeHead(ml, s0, &pushBox);
                }

                if (ml->values != 5) {
                    ERROR;
                }

                if (f == 32) {
                    mlVerify(ml, 1, 5, 5, 5);
                }

                multilistFullFree(ml);
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("add to tail 500x at fill %zu at compress %d", f,
                      depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    databox pushBox = databoxNewBytesString(genstr("hello", i));
                    pushBox.len = 64;
                    multilistFullPushByTypeTail(ml, s0, &pushBox);
                }

                if (ml->values != 500) {
                    ERROR;
                }

                if (f == 32) {
                    mlVerify(ml, 16, 500, 32, 20);
                }

                multilistFullFree(ml);
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("add to head 500x at fill %zu at compress %d", f,
                      depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistFullPushHead(ml, s0, genstr("hello", i), 32);
                }

                if (ml->values != 500) {
                    ERROR;
                }

                if (f == 32) {
                    mlVerify(ml, 16, 500, 20, 32);
                }

                multilistFullFree(ml);
            }
        }

        TEST("rotate empty") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            multilistFullRotate(ml, s);
            mlVerify(ml, 1, 0, 0, 0);
            multilistFullFree(ml);
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("rotate one val once at fill %zu", f) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                multilistFullPushHead(ml, s0, "hello", 6);

                multilistFullRotate(ml, s);

                /* Ignore compression verify because flex is
                 * too small to compress. */
                mlVerify(ml, 1, 1, 1, 1);
                multilistFullFree(ml);
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("rotate 504 val 5000 times at fill %zu at compress %d", f,
                      depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                multilistFullPushHead(ml, s0, "900", 3);
                multilistFullPushHead(ml, s0, "7000", 4);
                multilistFullPushHead(ml, s0, "-1200", 5);
                multilistFullPushHead(ml, s0, "42", 2);
                for (int i = 0; i < 500; i++) {
                    multilistFullPushHead(ml, s0, genstr("hello", i), 64);
                }

                assert(ml->values == 504);

                mlInfo(ml);
                for (int i = 0; i < 5000; i++) {
                    mlInfo(ml);
                    assert(ml->values == 504);
                    multilistFullRotate(ml, s);
                    assert(ml->values == 504);
                }

                if (f == 1) {
                    mlVerify(ml, 504, 504, 1, 1);
                } else if (f == 2) {
                    mlVerify(ml, 252, 504, 2, 2);
                } else if (f == 32) {
                    mlVerify(ml, 16, 504, 32, 24);
                }

                multilistFullFree(ml);
            }
        }

        TEST("pop empty") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            databox box = {{0}};
            bool found = multilistFullPopHead(ml, s0, &box);
            assert(!found);
            mlVerify(ml, 1, 0, 0, 0);
            multilistFullFree(ml);
        }

        TEST("pop 1 string from 1") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            char *populate = genstr("hello", 331);
            multilistFullPushHead(ml, s0, populate, 32);
            uint8_t *data;
            uint32_t bytes;
            mlInfo(ml);
            databox box = {{0}};
            multilistFullPopHead(ml, s0, &box);
            bytes = box.len;
            data = box.data.bytes.start;
            assert(data != NULL);
            assert(bytes == 32);
            if (strcmp(populate, (char *)data)) {
                ERR("Pop'd value (%.*s) didn't equal original value (%s)",
                    bytes, data, populate);
            }

            mlVerify(ml, 1, 0, 0, 0);
            databoxFreeData(&box);
            multilistFullFree(ml);
        }

        TEST("pop head 1 number from 1") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            multilistFullPushHead(ml, s0, "55513", 5);
            int64_t lv;
            mlInfo(ml);
            databox box = {{0}};
            multilistFullPopHead(ml, s0, &box);
            lv = box.data.i;
            assert(lv == 55513);
            mlVerify(ml, 1, 0, 0, 0);
            databoxFreeData(&box);
            multilistFullFree(ml);
        }

        TEST("pop head 500 from 500") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistFullPushHead(ml, s0, genstr("hello", i), 32);
            }

            mlInfo(ml);
            for (int i = 0; i < 500; i++) {
                uint8_t *data;
                uint32_t bytes;
                databox box = {{0}};
                bool found = multilistFullPopHead(ml, s0, &box);
                data = box.data.bytes.start;
                bytes = box.len;
                assert(found);
                assert(data != NULL);
                assert(bytes == 32);
                if (strcmp(genstr("hello", 499 - i), (char *)data)) {
                    ERR("Pop'd value (%.*s) didn't equal original value (%s)",
                        bytes, data, genstr("hello", 499 - i));
                }
                databoxFreeData(&box);
            }

            mlVerify(ml, 1, 0, 0, 0);
            multilistFullFree(ml);
        }

        TEST("pop head 5000 from 500") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistFullPushHead(ml, s0, genstr("hello", i), 32);
            }

            for (int i = 0; i < 5000; i++) {
                uint8_t *data;
                uint32_t bytes;
                databox box = {{0}};
                bool found = multilistFullPopHead(ml, s0, &box);
                data = box.data.bytes.start;
                bytes = box.len;
                if (i < 500) {
                    assert(found);
                    assert(data);
                    assert(bytes == 32);
                    if (strcmp(genstr("hello", 499 - i), (char *)data)) {
                        ERR("Pop'd value (%.*s) didn't equal original value "
                            "(%s)",
                            bytes, data, genstr("hello", 499 - i));
                        assert(NULL);
                    }
                } else {
                    assert(!found);
                }
                databoxFreeData(&box);
            }
            mlVerify(ml, 1, 0, 0, 0);
            multilistFullFree(ml);
        }

        TEST("iterate forward over 500 list") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistFullPushHead(ml, s0, genstr("hello", i), 32);
            }

            multilistIterator iter = {0};
            multilistFullIteratorInitForwardReadOnly(ml, s, &iter);
            multilistEntry entry;
            int i = 499, values = 0;
            while (multilistFullNext(&iter, &entry)) {
                char *h = genstr("hello", i);
                if (strcmp(entry.box.data.bytes.cstart, h)) {
                    ERR("value [%s] didn't match [%s] at position %d",
                        entry.box.data.bytes.start, h, i);
                }

                i--;
                values++;
            }

            if (values != 500) {
                ERR("Didn't iterate over exactly 500 elements (%d)", i);
            }

            mlVerify(ml, 16, 500, 20, 32);
            multilistFullFree(ml);
        }

        TEST("iterate reverse over 500 list") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistFullPushHead(ml, s0, genstr("hello", i), 32);
            }

            multilistIterator iter = {0};
            multilistFullIteratorInitReverseReadOnly(ml, s, &iter);
            multilistEntry entry;
            int i = 0;
            while (multilistFullNext(&iter, &entry)) {
                char *h = genstr("hello", i);
                if (strcmp(entry.box.data.bytes.cstart, h)) {
                    ERR("value [%s] didn't match [%s] at position %d",
                        entry.box.data.bytes.start, h, i);
                }

                i++;
            }

            if (i != 500) {
                ERR("Didn't iterate over exactly 500 elements (%d)", i);
            }

            mlVerify(ml, 16, 500, 20, 32);
            multilistFullFree(ml);
        }

        TEST("insert before with 0 elements") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            multilistEntry entry;
            multilistFullIndexGet(ml, s0, 0, &entry);
            multilistFullInsertBefore(ml, s, &entry, "abc", 4);
            mlVerify(ml, 1, 1, 1, 1);
            multilistFullFree(ml);
        }

        TEST("insert after with 0 elements") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            multilistEntry entry;
            multilistFullIndexGet(ml, s0, 0, &entry);
            multilistFullInsertAfter(ml, s, &entry, "abc", 4);
            mlVerify(ml, 1, 1, 1, 1);
            multilistFullFree(ml);
        }

        TEST("insert after 1 element") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            multilistFullPushHead(ml, s0, "hello", 6);
            multilistEntry entry;
            multilistFullIndexGet(ml, s0, 0, &entry);
            multilistFullInsertAfter(ml, s, &entry, "abc", 4);
            mlVerify(ml, 1, 2, 2, 2);
            multilistFullFree(ml);
        }

        TEST("insert before 1 element") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            multilistFullPushHead(ml, s0, "hello", 6);
            multilistEntry entry;
            multilistFullIndexGet(ml, s0, 0, &entry);
            multilistFullInsertAfter(ml, s, &entry, "abc", 4);
            mlVerify(ml, 1, 2, 2, 2);
            multilistFullFree(ml);
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("insert once in elements while iterating at fill %zu at "
                      "compress %d\n",
                      f, depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                multilistFullPushTail(ml, s0, "abc", 3);
                multilistFullSetFill(ml, 0);
                multilistFullPushTail(ml, s0, "def", 3); /* unique node */
                multilistFullSetFill(ml, f);
                multilistFullPushTail(ml, s0, "bob", 3); /* reset for +3 */
                multilistFullPushTail(ml, s0, "foo", 3);
                multilistFullPushTail(ml, s0, "zoo", 3);

                itrprintr(ml, s, false);
                /* insert "bar" before "bob" while iterating over list. */
                multilistIterator iter = {0};
                multilistFullIteratorInitForwardReadOnly(ml, s, &iter);
                multilistEntry entry;
                while (multilistFullNext(&iter, &entry)) {
                    if (!strncmp(entry.box.data.bytes.cstart, "bob", 3)) {
                        /* Insert as fill = 1 so it spills into new node. */
                        multilistFullInsertBefore(ml, s, &entry, "bar", 3);
                        /* note: we DO NOT support insert while iterating,
                         * meaning if you insert during an iteration, you
                         * must immediately exit the iteration.
                         *
                         * if you need more generic insert-while-iterating
                         * behavior, create a series of
                         * IteratorInsert{Before,After}Entry, etc. */
                        break;
                    }
                }

                /* verify results */
                multilistFullIndexGet(ml, s0, 0, &entry);
                if (strncmp(entry.box.data.bytes.cstart, "abc", 3)) {
                    ERR("Value 0 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }

                multilistFullIndexGet(ml, s0, 1, &entry);
                if (strncmp(entry.box.data.bytes.cstart, "def", 3)) {
                    ERR("Value 1 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                    assert(NULL);
                }

                multilistFullIndexGet(ml, s0, 2, &entry);
                if (strncmp(entry.box.data.bytes.cstart, "bar", 3)) {
                    ERR("Value 2 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }

                multilistFullIndexGet(ml, s0, 3, &entry);
                if (strncmp(entry.box.data.bytes.cstart, "bob", 3)) {
                    ERR("Value 3 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }

                multilistFullIndexGet(ml, s0, 4, &entry);
                if (strncmp(entry.box.data.bytes.cstart, "foo", 3)) {
                    ERR("Value 4 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }

                multilistFullIndexGet(ml, s0, 5, &entry);
                if (strncmp(entry.box.data.bytes.cstart, "zoo", 3)) {
                    ERR("Value 5 didn't match, instead got: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }

                multilistFullFree(ml);
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC(
                "insert [before] 250 new in middle of 500 elements at fill"
                " %zu at compress %d",
                f, depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistFullPushTail(ml, s0, genstr("hello", i), 32);
                }

                for (int i = 0; i < 250; i++) {
                    multilistEntry entry;
                    multilistFullIndexGet(ml, s0, 250, &entry);
                    multilistFullInsertBefore(ml, s, &entry, genstr("abc", i),
                                              32);
                }

                if (f == 32) {
                    mlVerify(ml, 25, 750, 32, 20);
                }

                multilistFullFree(ml);
            }
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("insert [after] 250 new in middle of 500 elements at "
                      "fill %zu at compress %d",
                      f, depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistFullPushHead(ml, s0, genstr("hello", i), 32);
                }

                for (int i = 0; i < 250; i++) {
                    multilistEntry entry;
                    multilistFullIndexGet(ml, s0, 250, &entry);
                    multilistFullInsertAfter(ml, s, &entry, genstr("abc", i),
                                             32);
                }

                if (ml->values != 750) {
                    ERR("List size not 750, but rather %" PRIu64 "",
                        ml->values);
                }

                if (f == 32) {
                    mlVerify(ml, 26, 750, 20, 32);
                }

                multilistFullFree(ml);
            }
        }

        TEST("duplicate empty list") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            mlVerify(ml, 1, 0, 0, 0);
            multilistFull *copy = multilistFullDuplicate(ml);
            mlVerify(copy, 1, 0, 0, 0);
            multilistFullFree(ml);
            multilistFullFree(copy);
        }

        TEST("duplicate list of 1 element") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            multilistFullPushHead(ml, s0, genstr("hello", 3), 32);
            mlVerify(ml, 1, 1, 1, 1);
            multilistFull *copy = multilistFullDuplicate(ml);
            mlVerify(copy, 1, 1, 1, 1);
            multilistFullFree(ml);
            multilistFullFree(copy);
        }

        TEST("duplicate list of 500") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistFullPushHead(ml, s0, genstr("hello", i), 32);
            }

            mlVerify(ml, 16, 500, 20, 32);

            multilistFull *copy = multilistFullDuplicate(ml);
            mlVerify(copy, 16, 500, 20, 32);
            multilistFullFree(ml);
            multilistFullFree(copy);
        }

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("index 1,200 from 500 list at fill %zu at compress %d", f,
                      depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistFullPushTail(ml, s0, genstr("hello", i + 1), 32);
                }

                multilistEntry entry;
                multilistFullIndexGet(ml, s0, 1, &entry);
                if (!strcmp(entry.box.data.bytes.cstart, "hello2")) {
                    OK;
                } else {
                    ERR("Value: %s", entry.box.data.bytes.start);
                }
                multilistFullIndexGet(ml, s0, 200, &entry);
                if (!strcmp(entry.box.data.bytes.cstart, "hello201")) {
                    OK;
                } else {
                    ERR("Value: %s", entry.box.data.bytes.start);
                }

                multilistFullFree(ml);
            }

            TEST_DESC("index -1,-2 from 500 list at fill %zu at compress %d", f,
                      depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistFullPushTail(ml, s0, genstr("hello", i + 1), 32);
                }

                multilistEntry entry;
                multilistFullIndexGet(ml, s0, -1, &entry);
                if (!strcmp(entry.box.data.bytes.cstart, "hello500")) {
                    OK;
                } else {
                    ERR("Value: %s", entry.box.data.bytes.start);
                }

                multilistFullIndexGet(ml, s0, -2, &entry);
                if (!strcmp(entry.box.data.bytes.cstart, "hello499")) {
                    OK;
                } else {
                    ERR("Value: %s", entry.box.data.bytes.start);
                }

                multilistFullFree(ml);
            }

            TEST_DESC("index -100 from 500 list at fill %zu at compress %d", f,
                      depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                for (int i = 0; i < 500; i++) {
                    multilistFullPushTail(ml, s0, genstr("hello", i + 1), 32);
                }

                multilistEntry entry;
                multilistFullIndexGet(ml, s0, -100, &entry);
                if (!strcmp(entry.box.data.bytes.cstart, "hello401")) {
                    OK;
                } else {
                    ERR("Value: %s", entry.box.data.bytes.start);
                }

                multilistFullFree(ml);
            }

            TEST_DESC(
                "index too big +1 from 50 list at fill %zu at compress %d", f,
                depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                for (int i = 0; i < 50; i++) {
                    multilistFullPushTail(ml, s0, genstr("hello", i + 1), 32);
                }

                multilistEntry entry;
                if (multilistFullIndexCheck(ml, s0, 50, &entry)) {
                    ERR("Index found at 50 with 50 list: %.*s",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                } else {
                    OK;
                }

                multilistFullFree(ml);
            }
        }

        TEST("delete range empty list") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            multilistFullDelRange(ml, s0, 5, 20);
            mlVerify(ml, 1, 0, 0, 0);
            multilistFullFree(ml);
        }

        TEST("delete range of entire node in list of one node") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 32; i++) {
                multilistFullPushHead(ml, s0, genstr("hello", i), 32);
            }

            mlVerify(ml, 1, 32, 32, 32);
            multilistFullDelRange(ml, s0, 0, 32);
            mlVerify(ml, 1, 0, 0, 0);
            multilistFullFree(ml);
        }

        TEST("delete range of entire node with overflow valuess") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 32; i++) {
                multilistFullPushHead(ml, s0, genstr("hello", i), 32);
            }

            mlVerify(ml, 1, 32, 32, 32);
            multilistFullDelRange(ml, s0, 0, 128);
            mlVerify(ml, 1, 0, 0, 0);
            multilistFullFree(ml);
        }

        TEST("delete middle 100 of 500 list") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistFullPushTail(ml, s0, genstr("hello", i + 1), 32);
            }

            mlVerify(ml, 16, 500, 32, 20);
            multilistFullDelRange(ml, s0, 200, 100);
            mlVerify(ml, 14, 400, 32, 20);
            multilistFullFree(ml);
        }

        TEST("delete negative 1 from 500 list") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistFullPushTail(ml, s0, genstr("hello", i + 1), 32);
            }

            mlVerify(ml, 16, 500, 32, 20);
            multilistFullDelRange(ml, s0, -1, 1);
            mlVerify(ml, 16, 499, 32, 19);
            multilistFullFree(ml);
        }

        TEST("delete negative 1 from 500 list with overflow valuess") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistFullPushTail(ml, s0, genstr("hello", i + 1), 32);
            }

            mlVerify(ml, 16, 500, 32, 20);
            multilistFullDelRange(ml, s0, -1, 128);
            mlVerify(ml, 16, 499, 32, 19);
            multilistFullFree(ml);
        }

        TEST("delete negative 100 from 500 list") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 500; i++) {
                multilistFullPushTail(ml, s0, genstr("hello", i + 1), 32);
            }

            multilistFullDelRange(ml, s0, -100, 100);
            mlVerify(ml, 13, 400, 32, 16);
            multilistFullFree(ml);
        }

        TEST("delete -10 values 5 from 50 list") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            for (int i = 0; i < 50; i++) {
                multilistFullPushTail(ml, s0, genstr("hello", i + 1), 32);
            }

            mlVerify(ml, 2, 50, 32, 18);
            multilistFullDelRange(ml, s0, -10, 5);
            mlVerify(ml, 2, 45, 32, 13);
            multilistFullFree(ml);
        }

        TEST("numbers only list read") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            multilistFullPushTail(ml, s0, "1111", 4);
            multilistFullPushTail(ml, s0, "2222", 4);
            multilistFullPushTail(ml, s0, "3333", 4);
            multilistFullPushTail(ml, s0, "4444", 4);
            mlVerify(ml, 1, 4, 4, 4);
            multilistEntry entry;
            multilistFullIndexGet(ml, s0, 0, &entry);
            if (entry.box.data.i64 != 1111) {
                ERR("Not 1111, %" PRIi64 "", entry.box.data.i64);
            }

            multilistFullIndexGet(ml, s0, 1, &entry);
            if (entry.box.data.i64 != 2222) {
                ERR("Not 2222, %" PRIi64 "", entry.box.data.i64);
            }

            multilistFullIndexGet(ml, s0, 2, &entry);
            if (entry.box.data.i64 != 3333) {
                ERR("Not 3333, %" PRIi64 "", entry.box.data.i64);
            }

            multilistFullIndexGet(ml, s0, 3, &entry);
            if (entry.box.data.i64 != 4444) {
                ERR("Not 4444, %" PRIi64 "", entry.box.data.i64);
            }

            if (multilistFullIndexGet(ml, s0, 4, &entry)) {
                ERR("Index past elements: %" PRIi64 "", entry.box.data.i64);
            }

            multilistFullIndexGet(ml, s0, -1, &entry);
            if (entry.box.data.i64 != 4444) {
                ERR("Not 4444 (reverse), %" PRIi64 "", entry.box.data.i64);
            }

            multilistFullIndexGet(ml, s0, -2, &entry);
            if (entry.box.data.i64 != 3333) {
                ERR("Not 3333 (reverse), %" PRIi64 "", entry.box.data.i64);
            }

            multilistFullIndexGet(ml, s0, -3, &entry);
            if (entry.box.data.i64 != 2222) {
                ERR("Not 2222 (reverse), %" PRIi64 "", entry.box.data.i64);
            }

            multilistFullIndexGet(ml, s0, -4, &entry);
            if (entry.box.data.i64 != 1111) {
                ERR("Not 1111 (reverse), %" PRIi64 "", entry.box.data.i64);
            }

            if (multilistFullIndexGet(ml, s0, -5, &entry)) {
                ERR("Index past elements (reverse), %" PRIi64 "",
                    entry.box.data.i64);
            }

            multilistFullFree(ml);
        }

        TEST("numbers larger list read") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            int64_t nums[5000];
            for (int i = 0; i < 5000; i++) {
                nums[i] = -5157318210846258176 + i;
                const databox pushBox = {.data.i64 = nums[i],
                                         .type = DATABOX_SIGNED_64};
                multilistFullPushByTypeTail(ml, s0, &pushBox);
            }

            multilistFullPushTail(ml, s0, "xxxxxxxxxxxxxxxxxxxx", 20);
            multilistEntry entry;
            for (int i = 0; i < 5000; i++) {
                multilistFullIndexGet(ml, s0, i, &entry);
                if (entry.box.data.i64 != nums[i]) {
                    ERR("[%d] Not longval %" PRIi64 " but rather %" PRIi64 "",
                        i, nums[i], entry.box.data.i64);
                }

                entry.box.data.i64 = 0xdeadbeef;
            }

            multilistFullIndexGet(ml, s0, 5000, &entry);
            if (strncmp(entry.box.data.bytes.cstart, "xxxxxxxxxxxxxxxxxxxx",
                        20)) {
                ERR("String val not match: %s", entry.box.data.bytes.start);
            }

            mlVerify(ml, 157, 5001, 32, 9);
            multilistFullFree(ml);
        }

        TEST("numbers larger list read B") {
            multilistFull *ml =
                multilistFullNew(defaultCompressSizeLimit, depth[_i]);
            multilistFullPushTail(ml, s0, "99", 2);
            multilistFullPushTail(ml, s0, "98", 2);
            multilistFullPushTail(ml, s0, "xxxxxxxxxxxxxxxxxxxx", 20);
            multilistFullPushTail(ml, s0, "96", 2);
            multilistFullPushTail(ml, s0, "95", 2);
            multilistFullReplaceAtIndex(ml, s0, 1, "foo", 3);
            multilistFullReplaceAtIndex(ml, s0, -1, "bar", 3);
            multilistFullFree(ml);
            OK;
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("lrem test at fill %zu at compress %d", f, depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                char *words[] = {"abc", "foo", "bar",  "foobar", "foobared",
                                 "zap", "bar", "test", "foo"};
                char *result[] = {"abc", "foo",  "foobar", "foobared",
                                  "zap", "test", "foo"};
                char *resultB[] = {"abc",      "foo", "foobar",
                                   "foobared", "zap", "test"};
                for (int i = 0; i < 9; i++) {
                    multilistFullPushTail(ml, s0, words[i], strlen(words[i]));
                }

                /* lrem 0 bar */
                multilistIterator iter = {0};
                multilistFullIteratorInitForward(ml, s, &iter);
                multilistEntry entry;
                int i = 0;
                while (multilistFullNext(&iter, &entry)) {
                    if (multilistFullCompare(entry.fe, "bar", 3)) {
                        multilistFullDelEntry(&iter, &entry);
                    }

                    i++;
                }

                /* check result of lrem 0 bar */
                multilistFullIteratorInitForwardReadOnly(ml, s, &iter);
                i = 0;
                int ok = 1;
                while (multilistFullNext(&iter, &entry)) {
                    /* Result must be: abc, foo, foobar, foobared, zap, test,
                     * foo */
                    if (strncmp(entry.box.data.bytes.cstart, result[i],
                                (int32_t)entry.box.len)) {
                        ERR("No match at position %d, got %.*s instead of %s",
                            i, (int32_t)entry.box.len,
                            entry.box.data.bytes.start, result[i]);
                        ok = 0;
                    }

                    i++;
                }

                multilistFullPushTail(ml, s0, "foo", 3);

                /* lrem -2 foo */
                multilistFullIteratorInitReverse(ml, s, &iter);
                i = 0;
                int del = 2;
                while (multilistFullNext(&iter, &entry)) {
                    if (multilistFullCompare(entry.fe, "foo", 3)) {
                        multilistFullDelEntry(&iter, &entry);
                        del--;
                    }

                    if (!del) {
                        break;
                    }

                    i++;
                }

                multilistFullIteratorRelease(&iter);

                /* check result of lrem -2 foo */
                /* (we're ignoring the '2' part and still deleting all foo
                 * because we only have two foo) */
                multilistFullIteratorInitReverseReadOnly(ml, s, &iter);
                i = 0;
                size_t resB = sizeof(resultB) / sizeof(*resultB);
                while (multilistFullNext(&iter, &entry)) {
                    /* Result must be: abc, foo, foobar, foobared, zap, test,
                     * foo */
                    if (strncmp(entry.box.data.bytes.cstart,
                                resultB[resB - 1 - i],
                                (int32_t)entry.box.len)) {
                        ERR("No match at position %d, got %.*s instead of %s",
                            i, (int32_t)entry.box.len,
                            entry.box.data.bytes.start, resultB[resB - 1 - i]);
                        ok = 0;
                    }

                    i++;
                }

                /* final result of all tests */
                if (ok) {
                    OK;
                }

                multilistFullFree(ml);
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("iterate reverse + delete at fill %zu at compress %d", f,
                      depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                multilistFullPushTail(ml, s0, "abc", 3);
                multilistFullPushTail(ml, s0, "def", 3);
                multilistFullPushTail(ml, s0, "hij", 3);
                multilistFullPushTail(ml, s0, "jkl", 3);
                multilistFullPushTail(ml, s0, "oop", 3);

                multilistEntry entry;
                multilistIterator iter = {0};
                multilistFullIteratorInitReverse(ml, s, &iter);
                int i = 0;
                while (multilistFullNext(&iter, &entry)) {
                    if (multilistFullCompare(entry.fe, "hij", 3)) {
                        multilistFullDelEntry(&iter, &entry);
                    }

                    i++;
                }

                if (i != 5) {
                    ERR("Didn't iterate 5 times, iterated %d times.", i);
                    multilistFullRepr(ml);
                }

                /* Check results after deletion of "hij" */
                multilistFullIteratorInitForward(ml, s, &iter);
                i = 0;
                char *vals[] = {"abc", "def", "jkl", "oop"};
                while (multilistFullNext(&iter, &entry)) {
                    if (!multilistFullCompare(entry.fe, vals[i], 3)) {
                        ERR("Value at %d didn't match %s\n", i, vals[i]);
                    }

                    i++;
                }

                multilistFullFree(ml);
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("iterator at index test at fill %zu at compress %d", f,
                      depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                int64_t nums[5000];
                for (int i = 0; i < 760; i++) {
                    nums[i] = -5157318210846258176 + i;
                    const databox pushBox = {.data.i64 = nums[i],
                                             .type = DATABOX_SIGNED_64};
                    multilistFullPushByTypeTail(ml, s0, &pushBox);
                }

                multilistEntry entry;
                multilistIterator iter = {0};
                multilistFullIteratorInitAtIdxForwardReadOnly(ml, s, &iter,
                                                              437);
                int i = 437;
                while (multilistFullNext(&iter, &entry)) {
                    if (entry.box.data.i64 != nums[i]) {
                        ERR("Expected %" PRIi64 ", but got %" PRIi64 "",
                            entry.box.data.i64, nums[i]);
                    }

                    i++;
                }

                multilistFullFree(ml);
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("ltrim test A at fill %zu at compress %d", f, depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                int64_t nums[5000];
                for (int i = 0; i < 32; i++) {
                    nums[i] = -5157318210846258176 + i;
                    const databox pushBox = DATABOX_SIGNED(nums[i]);
                    multilistFullPushByTypeTail(ml, s0, &pushBox);
                }

                if (f == 32) {
                    mlVerify(ml, 1, 32, 32, 32);
                }

                /* ltrim 25 53 (keep [25,32] inclusive = 7 remaining) */
                multilistFullDelRange(ml, s0, 0, 25);
                multilistFullDelRange(ml, s0, 0, 0);
                multilistEntry entry;
                for (int i = 0; i < 7; i++) {
                    multilistFullIndexGet(ml, s0, i, &entry);
                    if (entry.box.data.i64 != nums[25 + i]) {
                        ERR("Deleted invalid range!  Expected %" PRIi64
                            " but got "
                            "%" PRIi64 "",
                            entry.box.data.i64, nums[25 + i]);
                    }
                }
                if (f == 32) {
                    mlVerify(ml, 1, 7, 7, 7);
                }

                multilistFullFree(ml);
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("ltrim test B at fill %zu at compress %d", f, depth[_i]) {
                /* Force-disable compression because our 33 sequential
                 * integers don't compress and the check always fails. */
                multilistFull *ml = multilistFullNew(f, 0);
                char num[32];
                int64_t nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = i;
                    int bytes = StrInt64ToBuf(num, sizeof(num), nums[i]);
                    multilistFullPushTail(ml, s0, num, bytes);
                }

                if (f == 32) {
                    mlVerify(ml, 2, 33, 32, 1);
                }

                /* ltrim 5 16 (keep [5,16] inclusive = 12 remaining) */
                multilistFullDelRange(ml, s0, 0, 5);
                multilistFullDelRange(ml, s0, -16, 16);
                if (f == 32) {
                    mlVerify(ml, 1, 12, 12, 12);
                }

                multilistEntry entry;
                multilistFullIndexGet(ml, s0, 0, &entry);
                if (entry.box.data.i64 != 5) {
                    ERR("A: longval not 5, but %" PRIi64 "",
                        entry.box.data.i64);
                } else {
                    OK;
                }
                multilistFullIndexGet(ml, s0, -1, &entry);
                if (entry.box.data.i64 != 16) {
                    ERR("B! got instead: %" PRIi64 "", entry.box.data.i64);
                } else {
                    OK;
                }
                multilistFullPushTail(ml, s0, "bobobob", 7);
                multilistFullIndexGet(ml, s0, -1, &entry);
                if (strncmp(entry.box.data.bytes.cstart, "bobobob", 7)) {
                    ERR("Tail doesn't match bobobob, it's %.*s instead",
                        (int32_t)entry.box.len, entry.box.data.bytes.start);
                }

                for (int i = 0; i < 12; i++) {
                    multilistFullIndexGet(ml, s0, i, &entry);
                    if (entry.box.data.i64 != nums[5 + i]) {
                        ERR("Deleted invalid range!  Expected %" PRIi64
                            " but got "
                            "%" PRIi64 "",
                            entry.box.data.i64, nums[5 + i]);
                    }
                }
                multilistFullFree(ml);
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("ltrim test C at fill %zu at compress %d", f, depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                int64_t nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    const databox pushBox = DATABOX_SIGNED(nums[i]);
                    multilistFullPushByTypeTail(ml, s0, &pushBox);
                }

                if (f == 32) {
                    mlVerify(ml, 2, 33, 32, 1);
                }

                /* ltrim 3 3 (keep [3,3] inclusive = 1 remaining) */
                multilistFullDelRange(ml, s0, 0, 3);
                multilistFullDelRange(ml, s0, -29,
                                      4000); /* make sure not loop forever */
                if (f == 32) {
                    mlVerify(ml, 1, 1, 1, 1);
                }

                multilistEntry entry;
                multilistFullIndexGet(ml, s0, 0, &entry);
                if (entry.box.data.i64 != -5157318210846258173) {
                    ERROR;
                } else {
                    OK;
                }

                multilistFullFree(ml);
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC("ltrim test D at fill %zu at compress %d", f, depth[_i]) {
                multilistFull *ml = multilistFullNew(f, depth[_i]);
                char num[32];
                int64_t nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int bytes = StrInt64ToBuf(num, sizeof(num), nums[i]);
                    multilistFullPushTail(ml, s0, num, bytes);
                }

                if (f == 32) {
                    mlVerify(ml, 2, 33, 32, 1);
                }

                multilistFullDelRange(ml, s0, -12, 3);
                if (ml->values != 30) {
                    ERR("Didn't delete exactly three elements!  values is: "
                        "%" PRId64 "",
                        ml->values);
                }

                multilistFullFree(ml);
            }
        }

        mflexStateReset(s0);
        mflexStateReset(s1);

        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            TEST_DESC(
                "create multilistFull from flex at fill %zu at compress %d", f,
                depth[_i]) {
                flex *fl = flexNew();
                int64_t nums[64];
                char num[64];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int bytes = StrInt64ToBuf(num, sizeof(num), nums[i]);
                    flexPushBytes(&fl, num, bytes, FLEX_ENDPOINT_TAIL);
                }

                for (int i = 0; i < 33; i++) {
                    flexPushBytes(&fl, genstr("hello", i), 32,
                                  FLEX_ENDPOINT_TAIL);
                }

                multilistFull *ml =
                    multilistFullNewFromFlex(f, depth[_i], s0, fl);
                flexFree(fl);

                if (f == 1) {
                    mlVerify(ml, 66, 66, 1, 1);
                } else if (f == 32) {
                    mlVerify(ml, 3, 66, 32, 2);
                } else if (f == 66) {
                    mlVerify(ml, 1, 66, 66, 66);
                }

                multilistFullFree(ml);
            }
        }

        int64_t stop = timeUtilMs();
        runtime[_i] = stop - start;
    }

    mflexStateReset(s0);
    mflexStateReset(s1);

    /* Run a longer test of compression depth outside of primary test loop. */
    int listSizes[] = {30, 40, 50, 100, 250, 251, 500, 999, 1000, 5000, 10000};
    int64_t start = timeUtilMs();
    for (size_t list = 0; list < (sizeof(listSizes) / sizeof(*listSizes));
         list++) {
        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            for (size_t _depth = 1; _depth < 40; _depth++) {
                /* skip over many redundant test cases */
                TEST_DESC("verify specific compression of interior nodes with "
                          "%d list "
                          "at fill %zu at compress depth %zu",
                          listSizes[list], f, _depth) {
                    multilistFull *ml = multilistFullNew(f, _depth);
                    assert(ml->compress);
                    for (int i = 0; i < listSizes[list]; i++) {
                        multilistFullPushTail(ml, s0,
                                              genstr("hello TAIL", i + 1), 64);
                        multilistFullPushHead(ml, s0,
                                              genstr("hello HEAD", i + 1), 64);
                    }

                    assert(ml->compress);

                    mlNodeId lowRaw = ml->compress;
                    mlNodeId highRaw = ml->count - ml->compress;

                    for (mlNodeId at = 0; at < ml->count; at++) {
                        const mflex *node = getNode(at);

                        if (mflexBytesActual(node) == FLEX_EMPTY_SIZE) {
                            ERR("Node %d is empty.  Why?", at);
                        }

                        if (at < lowRaw || at >= highRaw) {
                            if (mflexIsCompressed(node)) {
                                ERR("Node %d is "
                                    "compressed at depth %zu ((%u, %u); total "
                                    "nodes: %u; size: %zu)",
                                    at, _depth, lowRaw, highRaw, ml->count,
                                    mflexBytesActual(node));
                                assert(NULL);
                            }
                        } else {
                            if (!mflexIsCompressed(node)) {
                                ERR("Node %d is NOT "
                                    "compressed at depth %zu ((%u, %u); total "
                                    "nodes: %u; size: %zu)",
                                    at, _depth, lowRaw, highRaw, ml->count,
                                    mflexBytesActual(node));
                                assert(NULL);
                            }
                        }
                    }

                    multilistFullFree(ml);
                }
            }
        }
    }
    int64_t stop = timeUtilMs();

    printf("\n");
    for (size_t i = 0; i < depthCount; i++) {
        fprintf(stderr, "Compress Depth %02d: %0.3f seconds.\n", depth[i],
                (float)runtime[i] / 1000);
    }

    fprintf(stderr, "Final Stress Loop: %0.2f seconds.\n",
            (float)(stop - start) / 1000);
    printf("\n");

    if (!err) {
        printf("ALL TESTS PASSED!\n");
    } else {
        ERR("Sorry, not all tests passed!  In fact, %d tests failed.", err);
    }
    fflush(stdout);
    fflush(stderr);

    mflexStateFree(s0);
    mflexStateFree(s1);

    return err;
}
#endif
