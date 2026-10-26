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

#include "cleaner.h"
#include "flexCapacityManagement.h"
#include "mflex.h"
#include "multilistFull.h"

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleaner(mflexState *, mflexStateFree);
defcleaner(flex *, flexFree);

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
    /* @own: caller; free=multilistFullFree */
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

size_t multilistFullBytesActual(const multilistFull *ml) {
    size_t totalBytes = 0;
    for (mlNodeId i = 0; i < ml->count; i++) {
        totalBytes += mflexBytesActual(getNode(i));
    }

    return totalBytes;
}

mlNodeId multilistFullNodeCount(const multilistFull *ml) {
    return ml->count;
}

void multilistFullStats(const multilistFull *ml, dkTierStats *out) {
    /* Nodes as stored (compressed ones at their compressed size), plus the
     * struct and the node array's pointers */
    const uint64_t data = multilistFullBytesActual(ml);
    *out = (dkTierStats){.count = ml->values,
                         .dataBytes = data,
                         .totalBytes = data + sizeof(*ml) +
                                       (uint64_t)ml->count * sizeof(mflex *),
                         .units = ml->count,
                         .unitLimit = ml->fill < flexOptimizationSizeLimits
                                          ? flexOptimizationSizeLimit[ml->fill]
                                          : 0,
                         .tier = 3};
}

#define COMPRESS_MAX ((1 << 15) - 1)
static uint16_t multilistFullClampDepth_(uint32_t compress) {
    if (compress > COMPRESS_MAX) {
        assert(NULL);
        compress = COMPRESS_MAX;
    }

    return (uint16_t)compress;
}

DK_STATIC void multilistFullCompressRange__(const multilistFull *ml,
                                            mflexState *state, mlNodeId lo,
                                            mlNodeId hi);

/* Whether node 'at' is one of the end nodes the depth rule keeps
 * uncompressed.  With compression off every node is an end node. */
static bool multilistFullIsEndNode_(const multilistFull *ml, mlNodeId at) {
    const mlNodeId depth = (mlNodeId)ml->compress;
    return depth == 0 || at < depth || at > (ml->count - 1) - depth;
}

/* Whether putting every node on the depth rule has to compress or
 * decompress a node, which needs an mflexState: a compressed end node,
 * or an uncompressed interior node.  Every other change is a retag. */
static bool multilistFullDepthRuleNeedsState_(const multilistFull *ml) {
    for (mlNodeId at = 0; at < ml->count; at++) {
        if (multilistFullIsEndNode_(ml, at) == mflexIsCompressed(getNode(at))) {
            return true;
        }
    }

    return false;
}

/* Put every node on the depth rule for the current depth: end nodes are
 * never compressed and interior nodes are compressed.  'state' may be
 * NULL only when multilistFullDepthRuleNeedsState_() is false: then every
 * end node is already uncompressed (mflexSetCompressNever only retags
 * it) and every interior node is already compressed (left alone). */
static void multilistFullApplyDepthRule_(const multilistFull *ml,
                                         mflexState *state) {
    if (ml->compress == 0) {
        for (mlNodeId at = 0; at < ml->count; at++) {
            mflexSetCompressNever(getNodePtr(at), state);
        }

        return;
    }

    multilistFullCompressRange__(ml, state, 0, ml->count - 1);
}

/* Change the compress depth of a list that may hold nodes, using
 * 'state' to compress or decompress the nodes the new depth moves
 * across the end/interior boundary. */
void multilistFullSetCompressDepthWithState(multilistFull *ml,
                                            mflexState *state,
                                            uint32_t compress) {
    assert(state);
    const uint16_t depth = multilistFullClampDepth_(compress);
    if (depth == ml->compress) {
        return;
    }

    ml->compress = depth;
    multilistFullApplyDepthRule_(ml, state);
}

/* Change the compress depth.  The list holds no mflexState, so when a
 * node has to be compressed or decompressed for the new depth this
 * borrows a temporary one for the duration of the call; a list whose
 * nodes only need retagging (an empty list, a single node, or a change
 * that moves no node across a boundary) allocates nothing. */
void multilistFullSetCompressDepth(multilistFull *ml, uint32_t compress) {
    const uint16_t depth = multilistFullClampDepth_(compress);
    if (depth == ml->compress) {
        return;
    }

    ml->compress = depth;
    if (multilistFullDepthRuleNeedsState_(ml)) {
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
        multilistFullApplyDepthRule_(ml, state);
    } else {
        multilistFullApplyDepthRule_(ml, NULL);
    }
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

void multilistFullSetOptions(multilistFull *ml, uint32_t fill, uint32_t depth) {
    multilistFullSetDepth(ml, fill, depth);
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

/* Put nodes [lo, hi] (clamped to the list) on the compress depth rule:
 * the 'compress' nodes at each end are never compressed, and every node
 * between them is compressed.  An interior node that is already
 * compressed is left alone. */
DK_STATIC void multilistFullCompressRange__(const multilistFull *ml,
                                            mflexState *state, mlNodeId lo,
                                            mlNodeId hi) {
    const mlNodeId tail = mlTailIdx(ml);
    const mlNodeId depth = (mlNodeId)ml->compress;
    if (lo < 0) {
        lo = 0;
    }

    if (hi > tail) {
        hi = tail;
    }

    for (mlNodeId at = lo; at <= hi; at++) {
        mflex **node = getNodePtr(at);
        if (at < depth || at > tail - depth) {
            mflexSetCompressNever(node, state);
        } else if (!mflexIsCompressed(*node)) {
            mflexSetCompressAuto(node, state);
        }
    }
}

/* Restore the compress depth rule after a split insert at 'nodeIdx' and
 * the merges around it changed the node count from 'countBefore'.
 *
 * The split and the merges only touch nodes in [nodeIdx - 2, nodeIdx + 2]
 * (merges only move indexes down, and the lowest merge partner stays
 * put), so that window is re-checked as a whole.  Every other node is
 * unchanged.  A node before the window keeps its index, so only its
 * distance from the tail changed.  A node after the window moved by the
 * net count change, so only its distance from the head changed.  Those
 * are re-checked only where that change carried them across a depth
 * boundary. */
DK_STATIC void multilistFullCompressAfterSplit__(const multilistFull *ml,
                                                 mflexState *state,
                                                 mlNodeId nodeIdx,
                                                 mlNodeId countBefore) {
    if (!multilistFullAllowsCompression(ml)) {
        return;
    }

    const mlNodeId depth = (mlNodeId)ml->compress;
    const mlNodeId net = ml->count - countBefore;
    const mlNodeId lo = nodeIdx - 2;
    const mlNodeId hi = nodeIdx + 2;
    multilistFullCompressRange__(ml, state, lo, hi);

    /* Before the window: the last interior index moved from
     * (countBefore - 1 - depth) to (count - 1 - depth). */
    const mlNodeId edgeBefore = countBefore - 1 - depth;
    const mlNodeId edgeAfter = ml->count - 1 - depth;
    const mlNodeId tailLo = (edgeBefore < edgeAfter ? edgeBefore : edgeAfter);
    const mlNodeId tailHi = (edgeBefore < edgeAfter ? edgeAfter : edgeBefore);
    multilistFullCompressRange__(ml, state, tailLo + 1,
                                 tailHi < lo - 1 ? tailHi : lo - 1);

    /* After the window: nodes moved by 'net' across the first interior
     * index 'depth'. */
    if (net != 0) {
        const mlNodeId headLo = net > 0 ? depth : depth + net;
        const mlNodeId headHi = net > 0 ? depth + net - 1 : depth - 1;
        multilistFullCompressRange__(ml, state,
                                     headLo > hi + 1 ? headLo : hi + 1, headHi);
    }
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

    mflexPushByTypeExact(mlHeadPtr(ml), state, box, FLEX_ENDPOINT_HEAD);

    ml->values++;
}

/* Add new entry to tail node of multilistFull. */
void multilistFullPushByTypeTail(multilistFull *ml, mflexState *state,
                                 const databox *box) {
    (void)_multilistFullMflexForTail(ml, state, DATABOX_SIZE(box));

    mflexPushByTypeExact(mlTailPtr(ml), state, box, FLEX_ENDPOINT_TAIL);

    ml->values++;
}

/* Append 'fl' as a new never-compress tail node, keeping 'fl' itself
 * as the node's memory.  The depth rule is not re-applied: the caller
 * does that.  An empty 'fl' is freed and adds no node, and on an empty
 * list 'fl' replaces the one empty node instead of following it, so the
 * list never holds an empty node beside others. */
DK_STATIC void multilistFullAppendFlexNode__(multilistFull *ml, flex *fl) {
    if (flexCount(fl) == 0) {
        flexFree(fl);
        return;
    }

    if (ml->count == 1 && ml->values == 0) {
        mflex **only = getNodePtr(0);
        mflexFree(*only);
        ml->values = flexCount(fl);
        *only = mflexConvertFromFlexNoCompress(fl);
        return;
    }

    multilistFullInsertNodeAfter__Empty(ml, mlTailIdx(ml));

    ml->values += flexCount(fl);

    mflex **node = mlTailPtr(ml);
    *node = mflexConvertFromFlexNoCompress(fl);
}

/* The node an append moved out of the tail's end zone into the
 * interior, or -1 if none did (compression off, or too few nodes for an
 * interior). */
static mlNodeId multilistFullAppendMovedNode_(const multilistFull *ml) {
    if (!multilistFullAllowsCompression(ml)) {
        return -1;
    }

    const mlNodeId moved = ml->count - 1 - (mlNodeId)ml->compress;
    if (moved < 0 || multilistFullIsEndNode_(ml, moved)) {
        return -1;
    }

    return moved;
}

/* Note: *keeps* 'fl' inside the new tail node.  The new tail pushes the
 * node 'compress' places before it into the interior, where it is
 * compressed through 'state'. */
void multilistFullAppendFlexWithState(multilistFull *ml, mflexState *state,
                                      flex *fl) {
    multilistFullAppendFlexNode__(ml, fl);
    const mlNodeId moved = multilistFullAppendMovedNode_(ml);
    if (moved >= 0) {
        mflexSetCompressAuto(getNodePtr(moved), state);
    }
}

/* As multilistFullAppendFlexWithState, borrowing a temporary mflexState
 * only when a node has to be compressed. */
void multilistFullAppendFlex(multilistFull *ml, flex *fl) {
    multilistFullAppendFlexNode__(ml, fl);
    const mlNodeId moved = multilistFullAppendMovedNode_(ml);
    if (moved >= 0 && !mflexIsCompressed(getNode(moved))) {
        cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
        mflexSetCompressAuto(getNodePtr(moved), state);
    }
}

/* Build a list from the 'flCount' flexes in 'fl', in order, consuming
 * them.  '_ml' is NULL or an allocation to reuse (the Medium tier being
 * upgraded), and is reallocated into the list.  A flex of two or more
 * elements is split at its middle into two nodes, a one-element flex
 * becomes one node, and an empty flex is freed and adds nothing; with no
 * elements at all the list keeps the one empty node every list has.
 * Every node starts never-compress, and the depth rule is applied once,
 * through 'state', after all of them are in place, so any count of
 * inputs gives compressed interior nodes and uncompressed end nodes. */
multilistFull *multilistFullNewFromFlexConsumeGrow(void *_ml, mflexState *state,
                                                   flex *fl[], size_t flCount,
                                                   const uint32_t depth,
                                                   const uint32_t fillLimit) {
    /* @own: caller; free=multilistFullFree */
    multilistFull *ml = zrealloc(_ml, sizeof(*ml));

    /* Clean our fields... */
    memset(ml, 0, sizeof(*ml));
    multilistFullSetFill(ml, fillLimit);
    ml->compress = multilistFullClampDepth_(depth);

    for (size_t i = 0; i < flCount; i++) {
        flex *firstHalf = fl[i];
        const size_t iCount = flexCount(firstHalf);

        if (iCount == 0) {
            /* Consumed, but nothing to keep. */
            flexFree(firstHalf);
            continue;
        }

        if (iCount == 1) {
            /* inserting flex with one element, so don't split into two nodes */
            multilistFullAppendFlexNode__(ml, firstHalf);
        } else {
            /* inserting a flex with more than one element, so split! */
            flex *secondHalf = flexSplit(&firstHalf, 1);

            multilistFullAppendFlexNode__(ml, firstHalf);
            multilistFullAppendFlexNode__(ml, secondHalf);
        }
    }

    if (ml->count == 0) {
        mflex *m = mflexNewNoCompress();
        reallocIncrCountBefore(ml, 0, m);
    }

    multilistFullApplyDepthRule_(ml, state);
    return ml;
}

/* Append all values of flex 'fl' individually into 'multilistFull'.
 *
 * Returns 'multilistFull' argument. */
multilistFull *multilistFullAppendValuesFromFlex(multilistFull *ml,
                                                 mflexState *state,
                                                 const flex *fl) {
    /* A 96/128-bit value lands in 'wide', reused for each element */
    databox holder = {};
    databoxWide wide;
    flexEntry *fe = flexHeadIfAny(fl);

    while (fe) {
        flexGetByTypeInto(fe, &holder, &wide);
        multilistFullPushByTypeTail(ml, state, &holder);
        fe = flexNext(fl, fe);
    }

    return ml;
}

/* Concatenate every node, in order, into one new flex.  A compressed node
 * is opened into 'state' and copied out before the next node reuses it.
 *
 * Returns a new flex; release with flexFree(). */
flex *multilistFullDump(const multilistFull *ml, mflexState *state) {
    if (ml->count == 1) {
        /* @own: caller; free=flexFree */
        return flexDuplicate(mflexOpenReadOnly(getNode(0), state));
    }

    /* @own: caller; free=flexFree */
    flex *all = flexNew();
    for (mlNodeId i = 0; i < ml->count; i++) {
        flexBulkAppendFlex(&all, mflexOpenReadOnly(getNode(i), state));
    }

    return all;
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

multilistFull *multilistFullAppendValuesFlex(multilistFull *ml,
                                             mflexState *state,
                                             const flex *fl) {
    return multilistFullAppendValuesFromFlex(ml, state, fl);
}

multilistFull *multilistFulllCreateFlex(int fill, int compress,
                                        mflexState *state, const flex *fl) {
    return multilistFullNewFromFlex(fill, compress, state, fl);
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
 * '*ff' is the node opened through 'state'.  For a compressed node it is
 * the state's scratch buffer, not the node's own memory, so an emptied
 * node is closed back through 'state' before it is freed.  Freeing '*ff'
 * directly would free the scratch buffer out from under 'state' and leak
 * the node.  Removing a node shifts its neighbours across the compress
 * depth boundaries, so the depth rule is re-applied.
 *
 * Returns true if the entire node was deleted, false if node still exists.
 * Also updates in/out param 'fe' with the next offset in the flex. */
DK_STATIC bool multilistFullDelIndex(multilistFull *ml, mflexState *state,
                                     const mlNodeId nodeIdx, flex **ff,
                                     flexEntry **fe) {
    flexDelete(ff, fe);
    ml->values--;

    if (flexIsEmpty(*ff) && ml->count > 1) {
        /* Only delete node if it's not the only node. */
        mflex **node = getNodePtr(nodeIdx);
        mflexCloseNoCompress(node, state, *ff);
        reallocDecrCount(ml, nodeIdx);
        multilistFullCompressRenew__(ml, state);

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

    const bool nodeDeleted = multilistFullDelIndex(
        iter->ml, iter->state[0], iter->nodeIdx, &entry->f, &entry->fe);

    if (nodeDeleted && iter->forward) {
        /* The following nodes moved down one index, so the next node is
         * now at this node's index: step back so Next's advance lands on
         * it instead of skipping it.  (In reverse the previous node keeps
         * its index.) */
        iter->nodeIdx--;
    }

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

    /* (After a node removal 'fe' is NULL and Next resets the offset.) */
    multilistIterOffsetAfterDelete_(iter, entry->offset);
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

/* Whether a delete may merge nodes 'a' and 'b' (a before b): both fit in
 * half the node size (the headroom keeps an insert from splitting what a
 * delete just merged), and both are uncompressed.  A compressed node
 * opens into the decompression buffer, which appending would reallocate
 * behind the buffer's owner, and the merged node can become an end node
 * (which must never stay compressed).  A list with no node size (fill
 * outside the size table) never merges here. */
DK_STATIC bool multilistFullDeleteMergeFits_(const multilistFull *ml,
                                             mlNodeId a, mlNodeId b) {
    if (ml->fill >= flexOptimizationSizeLimits ||
        mflexIsCompressed(getNode(a)) || mflexIsCompressed(getNode(b))) {
        return false;
    }
    return mflexBytesUncompressed(getNode(a)) +
               mflexBytesUncompressed(getNode(b)) <=
           flexOptimizationSizeLimit[ml->fill] / 2;
}

/* After a range delete left node 'nodeIdx' (closed) holding values:
 * merge it with its next neighbour, or else its previous one, when the
 * pair fits, so scattered deletes don't leave many nearly empty nodes */
DK_STATIC void multilistFullCoalesce_(multilistFull *ml, mflexState *state,
                                      mlNodeId nodeIdx) {
    mflexState *pair[2] = {state, state};
    bool merged = false;
    if (nodeIdx + 1 < ml->count &&
        multilistFullDeleteMergeFits_(ml, nodeIdx, nodeIdx + 1)) {
        merged = multilistFullMflexMerge_(ml, pair, nodeIdx, nodeIdx + 1);
    } else if (nodeIdx > 0 && nodeIdx < ml->count &&
               multilistFullDeleteMergeFits_(ml, nodeIdx - 1, nodeIdx)) {
        merged = multilistFullMflexMerge_(ml, pair, nodeIdx - 1, nodeIdx);
    }
    if (merged) {
        multilistFullCompressRenew__(ml, state); /* node positions moved */
    }
}

/* Bytes a repacked node leaves unused below the node size, for the flex
 * header to grow as its byte count does */
#define MULTILIST_FULL_COMPACT_SLACK 8

/* The most leading elements of 'fl' that fit in 'room' bytes; at least
 * one */
static uint32_t multilistFullLeadingFit_(const flex *fl, size_t room) {
    const uint8_t *head = (const uint8_t *)flexHead(fl);
    flexEntry *fe = flexHead(fl);
    uint32_t n = 0;
    const size_t count = flexCount(fl);
    while (n + 1 < count) {
        flexEntry *after = flexNext(fl, fe);
        if ((size_t)((const uint8_t *)after - head) > room) {
            break;
        }
        fe = after;
        n++;
    }
    return n ? n : 1;
}

/* Repack every value into full nodes, in order: each node's values are
 * copied (decompressed when needed) onto a carry flex, full nodes are cut
 * off its front and appended to a new list, which applies the compression
 * depth as it goes.  Memory beyond the two lists is about one node.  A
 * list with no node size (fill outside the size table) is left as is. */
void multilistFullCompact(multilistFull *ml, mflexState *state) {
    if (ml->fill >= flexOptimizationSizeLimits || ml->count < 2) {
        return;
    }

    const size_t nodeBytes = flexOptimizationSizeLimit[ml->fill];
    const size_t room = nodeBytes > MULTILIST_FULL_COMPACT_SLACK
                            ? nodeBytes - MULTILIST_FULL_COMPACT_SLACK
                            : nodeBytes;
    /* @own: caller; free=multilistFullFree */
    multilistFull *out = multilistFullNew(ml->fill, ml->compress);
    /* @own: caller; free=flexFree */
    flex *carry = flexNew();
    for (mlNodeId i = 0; i < ml->count; i++) {
        /* Copied out before any append, which may use 'state' to compress */
        flexBulkAppendFlex(&carry, mflexOpenReadOnly(getNode(i), state));
        while (flexBytes(carry) > nodeBytes) {
            flex *node = flexSplitRange(&carry, 0,
                                        multilistFullLeadingFit_(carry, room));
            multilistFullAppendFlexWithState(out, state, node);
        }
    }
    multilistFullAppendFlexWithState(out, state, carry);

    /* Uncompressed, the packed layout is never larger (fewer nodes and flex
     * headers).  Compressed, a node can compress a little worse at its new
     * boundaries than it did at its old ones (measured: 3,000 repetitive
     * strings, 178 nodes to 177, compressed bytes up 2.9%), so the smaller
     * layout is kept */
    if (multilistFullBytesActual(out) + (size_t)out->count * sizeof(mflex *) >=
        multilistFullBytesActual(ml) + (size_t)ml->count * sizeof(mflex *)) {
        multilistFullFree(out);
        return;
    }

    /* 'ml' takes the packed nodes; 'out' frees the old ones */
    const multilistFull packed = *out;
    *out = *ml;
    *ml = packed;
    multilistFullFree(out);
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

/* Split 'node' into two parts at the element at 'offset', so a value can
 * be pushed onto the end of the returned node that touches that element.
 *
 * 'offset' is an entry offset: from the node head when zero or positive,
 * from the node tail when negative (a reverse walk gives -1 for the last
 * element).  It is resolved to a head offset OFFSET before splitting.
 *
 * If 'after', the returned node is placed after the input node.
 *             The returned node has elements [OFFSET+1, END].
 *             The input node keeps elements [0, OFFSET].
 *             Pushing at the returned node's head lands right after OFFSET.
 *
 * If !'after', the returned node is placed before the input node.
 *              The returned node has elements [0, OFFSET-1].
 *              The input node keeps elements [OFFSET, END].
 *              Pushing at the returned node's tail lands right before OFFSET.
 *
 * Either side may be empty (OFFSET at the node's head or tail).
 *
 * Returns index of newly allocated nodeId */
/* Note: accepts *open* node/flex as input, then *closes* before returning. */
DK_STATIC mlNodeId multilistFullSplitNodeFromOpen_(
    multilistFull *ml, mflexState *state, mflex **node, flex **ff,
    const mlNodeId nodeIdx, const int offset, const bool after) {
    /* A tail-relative offset must be made head-relative: the split range
     * below is a head-relative start plus a count. */
    const int at = offset < 0 ? (int)flexCount(*ff) + offset : offset;
    assert(at >= 0 && (size_t)at < flexCount(*ff));

    /* An extent of -1 means "continue deleting until the list ends" */
    const int origStart = after ? at + 1 : 0;
    const int origExtent = after ? -1 : at;

    D("After %d (%d -> %d); ranges: [%d, %d]", after, offset, at, origStart,
      origExtent);

    flex *deletedContents = flexSplitRange(ff, origStart, origExtent);
    mflexCloseShrink(node, state, *ff);

    const mlNodeId newIdx = multilistFullInsertNodeEmpty__(ml, nodeIdx, after);
    mflex **newNode = getNodePtr(newIdx);
    *newNode = mflexConvertFromFlexNoCompress(deletedContents);

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
        const mlNodeId countBefore = ml->count;
        mlNodeId newIdx = multilistFullSplitNodeFromOpen_(
            ml, state[0], node, &f, entry->nodeIdx, entry->offset, after);

        /* Insert into new split node */
        mflex **newNode = getNodePtr(newIdx);
        flex *newF = mflexOpen(*newNode, state[0]);
        flexPushByTypeExact(&newF, box,
                            after ? FLEX_ENDPOINT_HEAD : FLEX_ENDPOINT_TAIL);
        mflexCloseGrow(newNode, state[0], newF);

        ml->values++;

        /* The merges can remove nodes on either side of the split, so
         * 'newIdx' may no longer name the new node: re-apply the depth
         * rule over everything the split and merges could have moved. */
        multilistFullMergeNodes_(ml, state, entry->nodeIdx);
        multilistFullCompressAfterSplit__(ml, state[0], entry->nodeIdx,
                                          countBefore);
    } else {
        if (after) {
            D("Not full, inserting after current position.");
            flexEntry *next = flexNext(f, entry->fe);
            if (next == NULL) {
                flexPushByTypeExact(&f, box, FLEX_ENDPOINT_TAIL);
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
    if (values <= 0 || start >= ml->values || start < -ml->values) {
        /* (an empty list has no valid start) */
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

    /* Merge around the cut once: the node the range started in, or the
     * last node when the range ran to the end */
    multilistFullCoalesce_(
        ml, state, entry.nodeIdx < ml->count ? entry.nodeIdx : ml->count - 1);
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
    multilistEntry entry = {};

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

/* Close the node a writable iterator holds open.  The iterator forgets
 * the closed node (as Next does when it leaves a node), so releasing
 * twice, or rewinding after a release, never closes it a second time.
 * A read-only iterator holds nothing to close: releasing it is a no-op. */
void multilistFullIteratorRelease(multilistIterator *iter) {
    if (!iter->readOnly && iter->f) {
        mflexCloseGrow(getNodeMlPtr(iter->ml, iter->nodeIdx), iter->state[0],
                       iter->f);
        iter->f = NULL;
        iter->fe = NULL;
    }
}

/* Restart an initialized iterator at the head (forward) or the tail
 * (reverse), keeping its state pair and readOnly mode.  The open node is
 * released first, so this is safe mid-iteration. */
void multilistFullRewind(multilistFull *ml, multilistIterator *iter) {
    multilistFullIteratorRelease(iter);
    multilistFullIteratorInit(ml, iter->state, iter, true, iter->readOnly);
}

void multilistFullRewindTail(multilistFull *ml, multilistIterator *iter) {
    multilistFullIteratorRelease(iter);
    multilistFullIteratorInit(ml, iter->state, iter, false, iter->readOnly);
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
        /* Populate value from existing flex position; a wide value
         * lands in the iterator's slot */
        flexGetByTypeInto(iter->fe, &entry->box, &iter->wide);

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

        /* @own: container; free=mflexFree */
        *copyNode = mflexDuplicate(origNode);
        copy->values += mflexCount(*copyNode);

        if (nodeIdx + 1 < orig->count) {
            /* Create next node (if the next iteration isn't beyond nodes) */
            multilistFullInsertNodeAfter__Empty(copy, nodeIdx);
        }
    }

    return copy;
}

/* The one positional-lookup engine: locates the element at 'index'
 * (0-based forward; negative counts from the tail) by walking node
 * counts from the cheapest available starting anchor.
 *
 * Without a caller anchor the anchors are head and tail (the endpoint
 * walk below).  With a caller anchor — a previously located entry plus
 * its known global index — the anchor's node is used as a third
 * starting point whenever it is strictly closer than both endpoints,
 * and when the target lands in the anchor's own (open) node the final
 * flex walk starts from the anchor entry itself via
 * flexIndexFromAnchor.
 *
 * Returns true if element found ('entry' populated).
 * Returns false if element out of range ('entry' not usable). */
DK_STATIC bool multilistFullIndexEngine_(multilistFull *ml, mflexState *state,
                                         mlOffsetId index,
                                         const multilistEntry *anchor,
                                         const mlOffsetId anchorIdx,
                                         multilistEntry *entry,
                                         const bool openNode) {
    if (anchor && anchor->nodeIdx >= 0 && anchor->nodeIdx < ml->count) {
        const mlOffsetId total = ml->values;

        /* Normalize target and anchor to forward element ranks */
        const mlOffsetId t = index < 0 ? total + index : index;
        if (t < 0 || t >= total) {
            return false;
        }

        const mlOffsetId a = anchorIdx < 0 ? total + anchorIdx : anchorIdx;

        const mlOffsetId dHead = t;
        const mlOffsetId dTail = (total - 1) - t;
        const mlOffsetId dAnchor = t > a ? t - a : a - t;

        if (a >= 0 && a < total && dAnchor < dHead && dAnchor < dTail) {
            /* Anchor is strictly nearest: recover the rank of the
             * anchor node's first element, then walk node counts
             * toward the target. */
            mlNodeId nearNodeIdx = anchor->nodeIdx;
            mflex *node = getNode(nearNodeIdx);
            mlOffsetId nodeCount = mflexCount(node);

            /* anchor->offset may be tail-relative (negative) */
            const mlOffsetId anchorOff = anchor->offset < 0
                                             ? nodeCount + anchor->offset
                                             : anchor->offset;
            mlOffsetId nodeFirst = a - anchorOff;

            while (t < nodeFirst) {
                nearNodeIdx--;
                assert(nearNodeIdx >= 0);
                node = getNode(nearNodeIdx);
                nodeCount = mflexCount(node);
                nodeFirst -= nodeCount;
            }

            while (t >= nodeFirst + nodeCount) {
                nodeFirst += nodeCount;
                nearNodeIdx++;
                assert(nearNodeIdx < ml->count);
                node = getNode(nearNodeIdx);
                nodeCount = mflexCount(node);
            }

            entry->ml = ml;
            entry->nodeIdx = nearNodeIdx;
            entry->offset = (int32_t)(t - nodeFirst);

            if (openNode) {
                if (nearNodeIdx == anchor->nodeIdx && anchor->f && anchor->fe) {
                    /* Target shares the anchor's node and the anchor
                     * was opened: reuse its open view and walk from
                     * the anchor entry itself. */
                    entry->f = anchor->f;
                    entry->fe =
                        flexIndexFromAnchor(entry->f, entry->offset, anchor->fe,
                                            (int32_t)anchorOff);
                } else {
                    entry->f = mflexOpen(node, state);
                    entry->fe = flexIndex(entry->f, entry->offset);
                }

                if (!entry->fe) {
                    entry->fe = flexHead(entry->f);
                    return false;
                }

                /* No wide storage here: a wide value is deferred */
                flexGetByTypeInto(entry->fe, &entry->box, NULL);
            }

            return true;
        }
        /* fallthrough: an endpoint is at least as close */
    }

    mlNodeId nodeIdx = 0;
    const mlOffsetId originalIndex = index;
    const mlOffsetId count = ml->values;

    /* Range check first, so negating 'index' below cannot overflow.
     * Every index of an empty list is out of range, but an opened entry
     * is still aimed at the list's one (empty) node, so
     * InsertByType{Before,After} can add a first element through it. */
    if (index >= count || index < -count) {
        if (count == 0 && openNode) {
            entry->ml = ml;
            entry->nodeIdx = 0;
            entry->offset = 0;
            entry->f = mflexOpen(getNode(0), state);
            entry->fe = flexHead(entry->f);
        }

        return false;
    }

#if 1
    /* Pre-process 'index' to determine if it would be faster to Iterate
     * from the other side of the list as was requested. */
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

        /* A single read has no wide storage: a wide value is deferred */
        flexGetByTypeInto(entry->fe, &entry->box, NULL);
        return true;
    }

    /* else, return true because we know the bounds exist,
     * but we didn't get them. */
    return true;
}

/* Populate 'entry' with the element at the specified zero-based index.
 * Negative integers count from the tail (-1 = tail, etc).
 *
 * Returns true if element found ('entry' populated).
 * Returns false if element out of range ('entry' not usable). */
bool multilistFullIndex(multilistFull *ml, mflexState *state, mlOffsetId index,
                        multilistEntry *entry, const bool openNode) {
    return multilistFullIndexEngine_(ml, state, index, NULL, 0, entry,
                                     openNode);
}

/* Explicit-anchor entry point into the shared positional-lookup
 * engine: the anchor is a previously located entry (obtained after the
 * most recent mutation) plus its known global index.  Never slower
 * than multilistFullIndex — the engine only uses the anchor when it is
 * strictly closer than both endpoints. */
bool multilistFullIndexNear(multilistFull *ml, mflexState *state,
                            const mlOffsetId index,
                            const multilistEntry *anchor,
                            const mlOffsetId anchorIdx, multilistEntry *entry,
                            const bool openNode) {
    return multilistFullIndexEngine_(ml, state, index, anchor, anchorIdx, entry,
                                     openNode);
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

    /* Get tail entry (a 96/128-bit value lands in 'wide') */
    databox box = {};
    databoxWide wide;
    flexGetByTypeInto(fe, &box, &wide);

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
            multilistFullDelIndex(ml, state[0], mlTailIdx(ml), &fTail, &fe);

            /* re-store 'fTail' in 'tail' */
            mflexCloseNoCompress(tail, state[0], fTail);
        }

        multilistFullCompressRenew__(ml, state[0]);
    } else {
        /* else, multilistFull only has one node, the head flex is also the
         * tail flex, and we already opened it, so just use it directly. */
        flexPushByTypeExact(&fTail, &box, FLEX_ENDPOINT_HEAD);

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
multilistFullPopResult multilistFullPop(multilistFull *ml, mflexState *state,
                                        databox *box, const bool fromTail) {
    if (ml->values == 0) {
        return MULTILIST_FULL_POP_EMPTY;
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
        /* The element is about to be deleted, so hand out an owned copy. */
        multilistPopCopy_(fe, box);

        /* No merge check: a pop only shrinks an end node until it empties
         * and goes, leaving at most one sparse node at each end */
        if (!multilistFullDelIndex(ml, state, nodeIdx, &f, &fe)) {
            mflexCloseShrink(node, state, f);
        }

        return ml->count <= 3 ? MULTILIST_FULL_POP_POPPED_FEW_NODES
                              : MULTILIST_FULL_POP_POPPED;
    }

    return MULTILIST_FULL_POP_EMPTY;
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
    cleaner(mflexStateFree) mflexState *state = mflexStateCreate();

    for (mlNodeId nodeIdx = 0; nodeIdx < ml->count; nodeIdx++) {
        const mflex *node = getNodeMl(ml, nodeIdx);
        flex *f = mflexOpen(node, state);
        flexRepr(f);
    }
}

#define multilistFullPush_(WHAT, a, b, c, d)                                   \
    do {                                                                       \
        /* by its given length: 'c' need not be NUL-terminated */              \
        databox pushBox_ = databoxNewBytes(c, d);                              \
        multilistFullPushByType##WHAT(a, b, &pushBox_);                        \
    } while (0)

#define multilistFullPushHead(a, b, c, d) multilistFullPush_(Head, a, b, c, d)
#define multilistFullPushTail(a, b, c, d) multilistFullPush_(Tail, a, b, c, d)

static bool multilistFullReplaceAtIndex(multilistFull *ml, mflexState *state,
                                        mlOffsetId index, void *data,
                                        size_t len) {
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

#define OK printf("\tOK\n")

/* Build with -DML_TEST_VERBOSE=1 to print list shapes as the tests run */
#ifndef ML_TEST_VERBOSE
#define ML_TEST_VERBOSE 0
#endif

[[maybe_unused]] static void compressedRepr(multilistFull *ml) {
    printf("[");
    for (mlNodeId depth = 0; depth < ml->count; depth++) {
        printf("%s, ", mflexIsCompressed(getNode(depth)) ? "C" : "U");
    }
    printf("\b\b]\n");
}

static void mlInfo([[maybe_unused]] multilistFull *ml) {
#if ML_TEST_VERBOSE
    printf("Container length (nodes): %" PRId32 "\n", ml->count);
    printf("Container values (elements): %" PRId64 "\n", ml->values);
    if (ml->count > 0) {
        printf("\t(elements in head: %zu)\n", mflexCount(mlHead(ml)));
        printf("\t(elements in tail: %zu)\n", mflexCount(mlTail(ml)));
        if (mlHead(ml) == mlTail(ml)) {
            printf("\tHEAD==TAIL\n");
        }
    }

    printf("\n");
#endif
}

/* Iterate over an entire multilistFull.
 *
 * Returns physical count of elements found by iterating over the list. */
static int itrprintr_(multilistFull *ml, mflexState *state[2], bool print,
                      bool forward) {
    multilistIterator iter = {};

    multilistFullIteratorInitReadOnly(ml, state, &iter, forward);
    multilistEntry entry = {};
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
static int multilistFullCompare(flexEntry *p1, const void *val, int vcount) {
    return flexCompareBytes(p1, val, vcount);
}

#define mlVerify(a, b, c, d, e)                                                \
    do {                                                                       \
        err += mlVerify_(a, s, b, c, d, e);                                    \
    } while (0)

/* Verify list metadata matches physical list contents. */
static int mlVerify_(multilistFull *ml, mflexState *state[2],
                     [[maybe_unused]] mlNodeId count, uint32_t values,
                     [[maybe_unused]] uint32_t headValues,
                     [[maybe_unused]] uint32_t tailValues) {
    int err = 0;

#if 0
    printf("[");
    for (mlNodeId depth = 0; depth < ml->count; depth++) {
        printf("%s, ", mflexIsCompressed(getNode(depth)) ? "C" : "U");
    }
    printf("\b\b]\n");
#endif

    mlInfo(ml);
    if (values != ml->values) {
        ERR("multilistFull values wrong: expected %d, got %" PRIu64, values,
            ml->values);
    }

    /* Node and per-end counts are not checked: accounting is by size, so
     * the node count and the head and tail entry counts follow the bytes */

    int loopr = itrprintr(ml, state, false);
    if (loopr != (int)ml->values) {
        ERR("multilistFull cached values do not match the actual values: "
            "expected %" PRIu64 ", got %d",
            ml->values, loopr);
    }

    int rloopr = itrprintrRev(ml, state, false);
    if (loopr != rloopr) {
        ERR("multilistFull forward values %d differ from reverse values %d",
            loopr, rloopr);
    }

    if (ml->count == 0 && !err) {
        OK;
        return err;
    }

    if (multilistFullAllowsCompression(ml)) {
        mlNodeId lowRaw = ml->compress;
        mlNodeId highRaw = ml->count - ml->compress;

        for (mlNodeId at = 0; at < ml->count; at++) {
            mflex *node = getNode(at);
            if (node && (at < lowRaw || at >= highRaw)) {
                if (mflexIsCompressed(node)) {
                    ERR("node %d is compressed at depth %d ((%u, %u); total "
                        "nodes: %u; size (uncomp): %zu; size (comp): %zu)",
                        at, ml->compress, lowRaw, highRaw, ml->count,
                        mflexBytesUncompressed(node),
                        mflexBytesCompressed(node));
                }
            } else {
                if (!mflexIsCompressed(node) &&
                    mflexBytesUncompressed(node) > 64) {
                    ERR("node %d is NOT compressed at depth %d ((%u, %u); "
                        "total nodes: %u; size (uncomp): %zu; size (comp): "
                        "%zu)",
                        at, ml->compress, lowRaw, highRaw, ml->count,
                        mflexBytesUncompressed(node),
                        mflexBytesCompressed(node));
                }
            }
        }
    }

    if (!err) {
        OK;
    }

    return err;
}

/* main test, but callable from other files */
/* Exact match of a decoded box against 'want'; a 96/128-bit value must
 * carry a live payload, in 'inlineAt' when that is set. */
static bool mlfWideSame(const databox *box, const databoxBig *want,
                        const void *inlineAt) {
    if (box->type != want->type) {
        return false;
    }

    if (box->type == DATABOX_SIGNED_128 || box->type == DATABOX_UNSIGNED_128) {
        return box->big && box->data.ptr &&
               (!inlineAt || (const void *)box->data.ptr == inlineAt) &&
               memcmp(box->data.ptr, want->extra, sizeof(want->extra)) == 0;
    }

    if (box->type == DATABOX_BYTES) {
        return box->len == want->len &&
               memcmp(box->data.bytes.start, want->data.bytes.start,
                      box->len) == 0;
    }

    return box->data.i == want->data.i;
}

/* multilistFullIndex into a poisoned entry matches 'want': a 96/128-bit
 * element must come back deferred, and decoding the entry's position
 * again into a slot must give the value. */
static bool mlfIndexSame(multilistFull *ml, mflexState *state, mlOffsetId i,
                         const databoxBig *want) {
    multilistEntry entry;
    memset(&entry, 0xFF, sizeof(entry));
    if (!multilistFullIndex(ml, state, i, &entry, true)) {
        return false;
    }

    if (want->type != DATABOX_SIGNED_128 &&
        want->type != DATABOX_UNSIGNED_128) {
        return mlfWideSame(&entry.box, want, NULL);
    }

    if (entry.box.type != want->type || !databoxIsWideDeferred(&entry.box)) {
        return false;
    }

    databoxWide slot;
    flexGetByTypeInto(entry.fe, &entry.box, &slot);
    return mlfWideSame(&entry.box, want, &slot);
}

/* Split test elements: fixed-width strings, one per id.  The long run of
 * 'x' makes any node of two or more of them compress, so an interior
 * node that is not compressed is a real depth-rule failure. */
#define MLF_SPLIT_LEN 48
static databox mlfSplitBox(uint32_t id, char buf[MLF_SPLIT_LEN + 1]) {
    /* "split-" then the id as six digits, written into a fixed template
     * so the string is always exactly MLF_SPLIT_LEN bytes. */
    static constexpr char tmpl[] =
        "split-000000-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    static_assert(sizeof(tmpl) == MLF_SPLIT_LEN + 1, "template length");
    memcpy(buf, tmpl, sizeof(tmpl));
    for (size_t d = 11; d >= 6; d--) {
        buf[d] = (char)('0' + id % 10);
        id /= 10;
    }

    return databoxNewBytes(buf, MLF_SPLIT_LEN);
}

/* An uncompressed node is still on auto compression (as opposed to
 * marked never-compress) if closing a copy of it compresses the copy.
 * mflex may leave an auto node uncompressed after a shrink, when its
 * uncompressed form needs no larger allocation; the next grow compresses
 * it.  A never-compress node's copy stays uncompressed. */
defcleaner(mflex *, mflexFree);
static bool mlfNodeIsAutoCompress(const mflex *node) {
    cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
    cleaner(mflexFree) mflex *copy = mflexDuplicate(node);
    flex *f = mflexOpen(copy, state);
    mflexCloseGrow(&copy, state, f);
    return mflexIsCompressed(copy);
}

/* Check every node of 'ml' against the compress depth rule: the
 * 'compress' nodes at each end are not compressed and are marked
 * never-compress (when big enough to tell), and every node between
 * them is compressed, or is on auto compression and left uncompressed by
 * mflex's shrink policy (see mlfNodeIsAutoCompress), or is too small to
 * be worth it (as mlVerify_ allows).
 * Returns NULL, or what failed with its node in 'badNode'. */
static const char *mlfDepthRuleCheck(const multilistFull *ml,
                                     mlNodeId *badNode) {
    if (!multilistFullAllowsCompression(ml)) {
        return NULL;
    }

    const mlNodeId tail = mlTailIdx(ml);
    const mlNodeId depth = (mlNodeId)ml->compress;
    for (mlNodeId at = 0; at <= tail; at++) {
        const mflex *node = getNode(at);
        const bool edge = at < depth || at > tail - depth;
        *badNode = at;
        if (edge && mflexIsCompressed(node)) {
            return "an end node is compressed";
        }

        if (edge && mflexBytesUncompressed(node) > 64 &&
            mlfNodeIsAutoCompress(node)) {
            return "an end node is on auto compression (the next grow "
                   "would compress it)";
        }

        if (!edge && !mflexIsCompressed(node) &&
            mflexBytesUncompressed(node) > 64 && !mlfNodeIsAutoCompress(node)) {
            return "an interior node is marked never-compress";
        }
    }

    return NULL;
}

/* The strict form of the depth rule, for a list whose nodes were all
 * just put on it (SetCompressDepth, AppendFlex): end nodes are
 * uncompressed and never-compress, and interior nodes over 64 bytes are
 * compressed, not merely on auto compression.  Depth 0 makes every node
 * an end node.  Returns NULL, or what failed with its node in 'badNode'. */
static const char *mlfDepthRuleExact(const multilistFull *ml,
                                     mlNodeId *badNode) {
    for (mlNodeId at = 0; at < ml->count; at++) {
        const mflex *node = getNode(at);
        const bool big = mflexBytesUncompressed(node) > 64;
        *badNode = at;
        if (multilistFullIsEndNode_(ml, at)) {
            if (mflexIsCompressed(node)) {
                return "an end node is compressed";
            }

            if (big && mlfNodeIsAutoCompress(node)) {
                return "an end node is on auto compression";
            }
        } else if (big && !mflexIsCompressed(node)) {
            return "an interior node is not compressed";
        }
    }

    return NULL;
}

static bool mlfSplitIs(const databox *box, uint32_t id) {
    char buf[MLF_SPLIT_LEN + 1];
    const databox want = mlfSplitBox(id, buf);
    return box->type == DATABOX_BYTES && box->len == MLF_SPLIT_LEN &&
           memcmp(box->data.bytes.start, want.data.bytes.start,
                  MLF_SPLIT_LEN) == 0;
}

/* Compare 'ml' with the oracle 'want' (element ids, head to tail) by
 * walking it forward and in reverse, and check its node accounting.
 * Returns NULL on a match, else what differed. */
static const char *mlfSplitCheck(multilistFull *ml, mflexState *state[2],
                                 const uint32_t *want, uint32_t n) {
    if (multilistFullCount(ml) != n) {
        return "wrong value count";
    }

    size_t sum = 0;
    for (mlNodeId at = 0; at < ml->count; at++) {
        const size_t c = mflexCount(getNode(at));
        if (c == 0 && !(n == 0 && ml->count == 1)) {
            /* (an empty list keeps its one node) */
            return "empty node";
        }

        sum += c;
    }

    if (sum != n) {
        return "node counts do not add up to the value count";
    }

    for (int dir = 0; dir < 2; dir++) {
        const bool forward = dir == 0;
        multilistIterator iter;
        multilistEntry entry;
        multilistFullIteratorInitReadOnly(ml, state, &iter, forward);
        uint32_t seen = 0;
        while (multilistFullNext(&iter, &entry)) {
            if (seen >= n) {
                return "walked past the end";
            }

            const uint32_t idx = forward ? seen : n - 1 - seen;
            if (!mlfSplitIs(&entry.box, want[idx])) {
                return forward ? "forward walk differs"
                               : "reverse walk differs";
            }

            seen++;
        }

        if (seen != n) {
            return "walk ended early";
        }
    }

    return NULL;
}

int multilistFullTest(int, char *[]) {
    uint32_t err = 0;

    const int depth[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const size_t depthCount = countof(depth);
    int64_t runtime[depthCount];
    const int defaultCompressSizeLimit = 1;

    cleaner(mflexStateFree) mflexState *s0 = mflexStateCreate();
    cleaner(mflexStateFree) mflexState *s1 = mflexStateCreate();
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

        TEST("rotate two values in two nodes swaps them") {
            /* At fill 1 a value of 80 bytes fills a node: two values are
             * two nodes, which rotate by swapping; twice is the identity */
            char a[80];
            char b[80];
            memset(a, 'a', sizeof(a));
            memset(b, 'b', sizeof(b));
            multilistFull *ml = multilistFullNew(1, depth[_i]);
            multilistFullPushTail(ml, s0, a, sizeof(a));
            multilistFullPushTail(ml, s0, b, sizeof(b));
            static constexpr char orders[3][2] = {
                {'a', 'b'}, {'b', 'a'}, {'a', 'b'}};
            for (int r = 0; r < 3; r++) {
                if (r > 0) {
                    multilistFullRotate(ml, s);
                }
                for (int i = 0; i < 2; i++) {
                    multilistEntry entry;
                    const bool found =
                        multilistFullIndex(ml, s0, i, &entry, true);
                    if (ml->count != 2 || !found ||
                        databoxLen(&entry.box) != sizeof(a) ||
                        ((const char *)databoxBytes(&entry.box))[0] !=
                            orders[r][i]) {
                        ERR("(%zu nodes, found %d) after %d rotations, value "
                            "%d is not '%c'",
                            (size_t)ml->count, found, r, i, orders[r][i]);
                    }
                }
            }
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
            databox box = {};
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
            databox box = {};
            if (multilistFullPopHead(ml, s0, &box) ==
                MULTILIST_FULL_POP_EMPTY) {
                ERR("pop of a filled list found it empty");
            }
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
            /* An integer pops as that integer; a numeral string pops as the
             * same string (multilist stores exactly what it is given). */
            const databox num = databoxNewSigned(55513);
            multilistFullPushByTypeHead(ml, s0, &num);
            multilistFullPushHead(ml, s0, "55513", 5);
            mlInfo(ml);
            databox box = {};
            if (multilistFullPopHead(ml, s0, &box) ==
                    MULTILIST_FULL_POP_EMPTY ||
                !DATABOX_IS_BYTES(&box) || databoxLen(&box) != 5 ||
                memcmp(databoxBytes(&box), "55513", 5) != 0) {
                ERR("numeral string did not pop as the same string");
            }
            databoxFreeData(&box);
            box = (databox){};
            if (multilistFullPopHead(ml, s0, &box) ==
                    MULTILIST_FULL_POP_EMPTY ||
                !DATABOX_IS_INTEGER(&box) || box.data.i != 55513) {
                ERR("integer did not pop as 55513");
            }
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
                databox box = {};
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
                databox box = {};
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

            multilistIterator iter = {};
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

            multilistIterator iter = {};
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
                multilistIterator iter = {};
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
            for (int64_t v = 1111; v <= 4444; v += 1111) {
                const databox num = databoxNewSigned(v);
                multilistFullPushByTypeTail(ml, s0, &num);
            }
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
                const databox pushBox = databoxNewSigned(nums[i]);
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
                multilistIterator iter = {};
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
                size_t resB = countof(resultB);
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
                multilistIterator iter = {};
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
                    const databox pushBox = databoxNewSigned(nums[i]);
                    multilistFullPushByTypeTail(ml, s0, &pushBox);
                }

                multilistEntry entry;
                multilistIterator iter = {};
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
                int64_t nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = i;
                    const databox numBox = databoxNewSigned(nums[i]);
                    multilistFullPushByTypeTail(ml, s0, &numBox);
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
                int64_t nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    const databox numBox = databoxNewSigned(nums[i]);
                    multilistFullPushByTypeTail(ml, s0, &numBox);
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
                cleaner(flexFree) flex *fl = flexNew();
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

    /* Compression depth against list length, at every fill: lists from a
     * few nodes to thousands, depths from 1 to past the node count, and
     * every node's compression state checked against its position. */
    const int listSizes[] = {30, 251, 1000, 5000};
    const size_t depths[] = {1, 2, 3, 5, 8, 13, 21, 39};
    int64_t start = timeUtilMs();
    for (size_t list = 0; list < (countof(listSizes)); list++) {
        for (size_t f = 0; f < flexOptimizationSizeLimits; f++) {
            for (size_t d = 0; d < countof(depths); d++) {
                const size_t _depth = depths[d];
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

    TEST("insert into a full node before and after every position, from "
         "forward and reverse entries") {
        /* Every node is full, so every insert splits its node.  The entry
         * comes from a forward walk (head-relative offset) or a reverse
         * walk (tail-relative, negative offset).  After each insert the
         * list must match an array oracle.  Depth 1 compresses the
         * interior nodes. */
        enum { SPLIT_NODES = 4, SPLIT_MAX = 512, SPLIT_INS = 100000 };
        static uint32_t want[SPLIT_MAX + 1];
        bool failed = false;
        for (uint32_t d = 0; d < 2 && !failed; d++) {
            char buf[MLF_SPLIT_LEN + 1];

            /* Node capacity: the push that opens a second node is one
             * past it. */
            multilistFull *probe = multilistFullNew(FLEX_CAP_LEVEL_512, d);
            uint32_t perNode = 0;
            while (probe->count < 2) {
                const databox box = mlfSplitBox(perNode, buf);
                multilistFullPushByTypeTail(probe, s0, &box);
                perNode++;
            }
            multilistFullFree(probe);
            perNode--;

            const uint32_t n = perNode * SPLIT_NODES;
            if (perNode < 2 || n > SPLIT_MAX) {
                ERR("depth %u: node capacity %u is out of range", d, perNode);
                break;
            }

            for (uint32_t g = 0; g < n && !failed; g++) {
                for (uint32_t mode = 0; mode < 4 && !failed; mode++) {
                    const bool after = mode & 1;
                    const bool forward = !(mode & 2);
                    multilistFull *ml = multilistFullNew(FLEX_CAP_LEVEL_512, d);
                    for (uint32_t i = 0; i < n; i++) {
                        const databox box = mlfSplitBox(i, buf);
                        multilistFullPushByTypeTail(ml, s0, &box);
                        want[i] = i;
                    }

                    if (ml->count != SPLIT_NODES ||
                        (d && !mflexIsCompressed(getNode(1)))) {
                        ERR("depth %u: built %u nodes, interior %s", d,
                            (unsigned)ml->count,
                            mflexIsCompressed(getNode(1)) ? "compressed"
                                                          : "uncompressed");
                        failed = true;
                    }

                    multilistIterator iter;
                    multilistEntry entry;
                    multilistFullIteratorInitReadOnly(ml, s, &iter, forward);
                    const uint32_t steps = forward ? g + 1 : n - g;
                    bool found = true;
                    for (uint32_t k = 0; k < steps && found; k++) {
                        found = multilistFullNext(&iter, &entry);
                    }

                    const databox ins = mlfSplitBox(SPLIT_INS + g, buf);
                    if (!found || !mlfSplitIs(&entry.box, g) ||
                        (entry.offset < 0) == forward ||
                        mflexAllowInsert_(ml, entry.nodeIdx,
                                          DATABOX_SIZE(&ins))) {
                        ERR("depth %u: element %u: bad %s entry (offset %d)", d,
                            g, forward ? "forward" : "reverse",
                            found ? (int)entry.offset : 0);
                        failed = true;
                        multilistFullFree(ml);
                        break;
                    }

                    if (after) {
                        multilistFullInsertByTypeAfter(ml, s, &entry, &ins);
                    } else {
                        multilistFullInsertByTypeBefore(ml, s, &entry, &ins);
                    }

                    const uint32_t pos = after ? g + 1 : g;
                    memmove(&want[pos + 1], &want[pos],
                            (n - pos) * sizeof(*want));
                    want[pos] = SPLIT_INS + g;

                    const char *why = mlfSplitCheck(ml, s, want, n + 1);
                    if (why) {
                        ERR("depth %u: insert %s element %u from a %s "
                            "entry: %s",
                            d, after ? "after" : "before", g,
                            forward ? "forward" : "reverse", why);
                        failed = true;
                    }

                    multilistFullFree(ml);
                }
            }
        }
    }

    TEST("split and merge inserts keep every node's compression on the "
         "depth rule") {
        /* Lists of full nodes with one-element nodes between them (built
         * with fill 0 pushes), so an insert into a full node splits it
         * and a split half can merge with a small neighbour.  After every
         * random insert each node must match the depth rule and the list
         * must match an array oracle.  Both splits and merges must
         * occur. */
        enum {
            CR_GROUPS = 8,
            CR_PER_GROUP = 12,
            CR_INSERTS = 48,
            CR_ROUNDS = 6,
            CR_MAX = 256,
            CR_INS = 200000
        };
        static uint32_t want[CR_MAX];
        for (uint32_t d = 1; d <= 2; d++) {
            uint32_t splits = 0;
            uint32_t merges = 0;
            bool failed = false;
            uint64_t seed = 0x9E3779B97F4A7C15ULL * d;
            for (uint32_t round = 0; round < CR_ROUNDS && !failed; round++) {
                char buf[MLF_SPLIT_LEN + 1];
                multilistFull *ml = multilistFullNew(FLEX_CAP_LEVEL_512, d);
                uint32_t n = 0;
                for (uint32_t g = 0; g < CR_GROUPS; g++) {
                    multilistFullSetFill(ml, FLEX_CAP_LEVEL_512);
                    for (uint32_t k = 0; k < CR_PER_GROUP + 2; k++) {
                        if (k == CR_PER_GROUP) {
                            /* one-element node, then a fresh node */
                            multilistFullSetFill(ml, FLEX_CAP_LEVEL_0);
                        }

                        const databox box = mlfSplitBox(n, buf);
                        multilistFullPushByTypeTail(ml, s0, &box);
                        want[n] = n;
                        n++;
                    }
                }

                multilistFullSetFill(ml, FLEX_CAP_LEVEL_512);

                mlNodeId bad = -1;
                const char *why = mlfDepthRuleCheck(ml, &bad);
                if (why) {
                    ERR("depth %u round %u: after building, node %d: %s", d,
                        round, bad, why);
                    failed = true;
                }

                for (uint32_t i = 0; i < CR_INSERTS && !failed; i++) {
                    seed =
                        seed * 6364136223846793005ULL + 1442695040888963407ULL;
                    const uint32_t g = (uint32_t)((seed >> 33) % n);
                    const bool after = (seed >> 13) & 1;
                    const uint32_t id = CR_INS + round * CR_INSERTS + i;
                    const databox ins = mlfSplitBox(id, buf);
                    multilistEntry entry;
                    if (!multilistFullIndexGet(ml, s0, g, &entry)) {
                        ERR("depth %u round %u: index %u not found", d, round,
                            g);
                        failed = true;
                        break;
                    }

                    const bool split = !mflexAllowInsert_(ml, entry.nodeIdx,
                                                          DATABOX_SIZE(&ins));
                    const mlNodeId countBefore = ml->count;
                    if (after) {
                        multilistFullInsertByTypeAfter(ml, s, &entry, &ins);
                    } else {
                        multilistFullInsertByTypeBefore(ml, s, &entry, &ins);
                    }

                    if (split) {
                        splits++;
                        if (ml->count <= countBefore) {
                            merges++;
                        }
                    }

                    const uint32_t pos = after ? g + 1 : g;
                    memmove(&want[pos + 1], &want[pos],
                            (n - pos) * sizeof(*want));
                    want[pos] = id;
                    n++;

                    why = mlfSplitCheck(ml, s, want, n);
                    if (!why) {
                        why = mlfDepthRuleCheck(ml, &bad);
                    }

                    if (why) {
                        ERR("depth %u round %u insert %u (%s element %u, "
                            "%s): node %d: %s",
                            d, round, i, after ? "after" : "before", g,
                            split ? "split" : "no split", bad, why);
                        failed = true;
                    }
                }

                multilistFullFree(ml);
            }

            if (!failed && (splits == 0 || merges == 0)) {
                ERR("depth %u: %u split inserts, %u with merges: both must "
                    "happen",
                    d, splits, merges);
            }
        }
    }

    TEST("every node-moving operation keeps the depth rule and the contents") {
        /* Random mixes of split/merge inserts, DelRange (positive and
         * negative starts), DelEntry walks (forward and reverse, emptying
         * whole nodes, compressed ones included), Pop from either end,
         * Rotate, Replace and Push at either end, on lists of full nodes
         * with one-element nodes between them.  After every operation the
         * list must match an array oracle and every node must match the
         * depth rule.  Each kind of node removal must actually happen. */
        enum {
            MV_GROUPS = 8,
            MV_PER_GROUP = 12,
            MV_OPS = 120,
            MV_ROUNDS = 4,
            MV_MAX = 512,
            MV_NEW = 300000
        };
        static uint32_t want[MV_MAX];
        static const char *const opName[] = {"insert", "DelRange", "DelEntry",
                                             "Pop",    "Rotate",   "Replace",
                                             "Push"};
        for (uint32_t d = 1; d <= 2; d++) {
            uint32_t opsRun[7] = {};
            uint32_t entryNodeDrops = 0;
            uint32_t popNodeDrops = 0;
            uint32_t rangeNodeDrops = 0;
            uint32_t nextId = MV_NEW;
            bool failed = false;
            uint64_t seed = 0xD1B54A32D192ED03ULL * d;
#define MV_RAND()                                                              \
    ((seed = seed * 6364136223846793005ULL + 1442695040888963407ULL) >> 33)
            for (uint32_t round = 0; round < MV_ROUNDS && !failed; round++) {
                char buf[MLF_SPLIT_LEN + 1];
                multilistFull *ml = multilistFullNew(FLEX_CAP_LEVEL_512, d);
                uint32_t n = 0;
                for (uint32_t g = 0; g < MV_GROUPS; g++) {
                    multilistFullSetFill(ml, FLEX_CAP_LEVEL_512);
                    for (uint32_t k = 0; k < MV_PER_GROUP + 2; k++) {
                        if (k == MV_PER_GROUP) {
                            multilistFullSetFill(ml, FLEX_CAP_LEVEL_0);
                        }

                        const databox box = mlfSplitBox(n, buf);
                        multilistFullPushByTypeTail(ml, s0, &box);
                        want[n] = n;
                        n++;
                    }
                }

                multilistFullSetFill(ml, FLEX_CAP_LEVEL_512);

                for (uint32_t op = 0; op < MV_OPS && !failed; op++) {
                    uint32_t kind = (uint32_t)(MV_RAND() % 7);
                    if (n == 0 ||
                        (n < 24 && (kind == 1 || kind == 2 || kind == 3))) {
                        kind = 6; /* too small to shrink: grow instead */
                    } else if (n > MV_MAX - 40 && (kind == 0 || kind == 6)) {
                        kind = 1; /* too large to grow: shrink instead */
                    }

                    const mlNodeId countBefore = ml->count;
                    const char *why = NULL;
                    switch (kind) {
                    case 0: { /* insert before / after a random element */
                        const uint32_t g = (uint32_t)(MV_RAND() % n);
                        const bool after = MV_RAND() & 1;
                        const databox ins = mlfSplitBox(nextId, buf);
                        multilistEntry entry;
                        if (!multilistFullIndexGet(ml, s0, g, &entry)) {
                            why = "index not found";
                            break;
                        }

                        if (after) {
                            multilistFullInsertByTypeAfter(ml, s, &entry, &ins);
                        } else {
                            multilistFullInsertByTypeBefore(ml, s, &entry,
                                                            &ins);
                        }

                        const uint32_t pos = after ? g + 1 : g;
                        memmove(&want[pos + 1], &want[pos],
                                (n - pos) * sizeof(*want));
                        want[pos] = nextId++;
                        n++;
                        break;
                    }
                    case 1: { /* DelRange from a positive or negative start */
                        const int64_t count = 1 + (int64_t)(MV_RAND() % 24);
                        const bool fromTail = MV_RAND() & 1;
                        const uint32_t at = (uint32_t)(MV_RAND() % n);
                        const mlOffsetId from =
                            fromTail ? -(mlOffsetId)(n - at) : (mlOffsetId)at;
                        const uint32_t room = n - at;
                        const uint32_t del =
                            count < (int64_t)room ? (uint32_t)count : room;
                        if (!multilistFullDelRange(ml, s0, from, count)) {
                            why = "DelRange refused a valid range";
                            break;
                        }

                        memmove(&want[at], &want[at + del],
                                (n - at - del) * sizeof(*want));
                        n -= del;
                        if (ml->count < countBefore) {
                            rangeNodeDrops++;
                        }
                        break;
                    }
                    case 2: { /* DelEntry walk deleting ids == r mod 3 */
                        const bool forward = MV_RAND() & 1;
                        const uint32_t r = (uint32_t)(MV_RAND() % 3);
                        multilistIterator iter;
                        multilistEntry entry;
                        multilistFullIteratorInit(ml, s, &iter, forward, false);
                        uint32_t seen = 0;
                        uint32_t kept = 0;
                        static uint32_t keep[MV_MAX];
                        while (multilistFullNext(&iter, &entry)) {
                            const uint32_t idx = forward ? seen : n - 1 - seen;
                            if (seen >= n ||
                                !mlfSplitIs(&entry.box, want[idx])) {
                                why =
                                    "the DelEntry walk read the wrong element";
                                break;
                            }

                            seen++;
                            if (want[idx] % 3 == r) {
                                multilistFullDelEntry(&iter, &entry);
                            } else {
                                keep[kept++] = want[idx];
                            }
                        }

                        multilistFullIteratorRelease(&iter);
                        if (!why && seen != n) {
                            why = "the DelEntry walk skipped elements";
                        }

                        if (!why) {
                            /* 'keep' is in walk order */
                            for (uint32_t k = 0; k < kept; k++) {
                                want[k] =
                                    forward ? keep[k] : keep[kept - 1 - k];
                            }

                            n = kept;
                        }

                        if (ml->count < countBefore) {
                            entryNodeDrops++;
                        }
                        break;
                    }
                    case 3: { /* Pop from either end */
                        const bool fromTail = MV_RAND() & 1;
                        databox got = {};
                        if (!multilistFullPop(ml, s0, &got, fromTail) ||
                            !mlfSplitIs(&got, want[fromTail ? n - 1 : 0])) {
                            why = "Pop returned the wrong element";
                        } else if (fromTail) {
                            n--;
                        } else {
                            memmove(&want[0], &want[1],
                                    (n - 1) * sizeof(*want));
                            n--;
                        }

                        databoxFreeData(&got);
                        if (ml->count < countBefore) {
                            popNodeDrops++;
                        }
                        break;
                    }
                    case 4: { /* Rotate: the tail moves to the head */
                        multilistFullRotate(ml, s);
                        const uint32_t last = want[n - 1];
                        memmove(&want[1], &want[0], (n - 1) * sizeof(*want));
                        want[0] = last;
                        break;
                    }
                    case 5: { /* Replace a random element */
                        const uint32_t at = (uint32_t)(MV_RAND() % n);
                        const databox rep = mlfSplitBox(nextId, buf);
                        if (!multilistFullReplaceByTypeAtIndex(ml, s0, at,
                                                               &rep)) {
                            why = "Replace refused a valid index";
                            break;
                        }

                        want[at] = nextId++;
                        break;
                    }
                    default: { /* Push at either end */
                        const bool head = MV_RAND() & 1;
                        const databox box = mlfSplitBox(nextId, buf);
                        if (head) {
                            multilistFullPushByTypeHead(ml, s0, &box);
                            memmove(&want[1], &want[0], n * sizeof(*want));
                            want[0] = nextId++;
                        } else {
                            multilistFullPushByTypeTail(ml, s0, &box);
                            want[n] = nextId++;
                        }

                        n++;
                        break;
                    }
                    }

                    opsRun[kind]++;
                    mlNodeId bad = -1;
                    if (!why) {
                        why = mlfSplitCheck(ml, s, want, n);
                    }

                    if (!why) {
                        why = mlfDepthRuleCheck(ml, &bad);
                    }

                    if (why) {
                        ERR("depth %u round %u op %u (%s, %u elements): node "
                            "%d: %s",
                            d, round, op, opName[kind], n, bad, why);
                        failed = true;
                    }
                }

                multilistFullFree(ml);
            }
#undef MV_RAND

            for (uint32_t k = 0; !failed && k < 7; k++) {
                if (!opsRun[k]) {
                    ERR("depth %u: %s never ran", d, opName[k]);
                }
            }

            if (!failed &&
                (!entryNodeDrops || !popNodeDrops || !rangeNodeDrops)) {
                ERR("depth %u: node removals: DelEntry %u, Pop %u, DelRange "
                    "%u; each must happen",
                    d, entryNodeDrops, popNodeDrops, rangeNodeDrops);
            }
        }
    }

    TEST("SetCompressDepth on a populated list puts every node on the new "
         "depth rule") {
        /* Lists of 0 to 13 full nodes go through depth sequences that
         * raise, lower, repeat and turn off compression, alternating the
         * stateless call and the WithState call.  After every change
         * each node must match the rule exactly and the list must match
         * an array oracle; pushes at both ends must then keep both. */
        enum { SD_MAX = 1024, SD_NEW = 400000 };
        static uint32_t want[SD_MAX];
        static constexpr uint32_t seqs[][7] = {
            {0, 1, 2, 0, UINT32_MAX},       {0, 2, 1, 3, 0, UINT32_MAX},
            {1, 1, 4, 1, 0, UINT32_MAX},    {2, 0, 6, 0, UINT32_MAX},
            {3, 2, 1, 0, 2, 5, UINT32_MAX},
        };
        char buf[MLF_SPLIT_LEN + 1];
        multilistFull *probe = multilistFullNew(FLEX_CAP_LEVEL_512, 0);
        uint32_t perNode = 0;
        while (probe->count < 2) {
            const databox box = mlfSplitBox(perNode, buf);
            multilistFullPushByTypeTail(probe, s0, &box);
            perNode++;
        }
        multilistFullFree(probe);
        perNode--;

        const uint32_t sizes[] = {0,           1,           perNode,
                                  perNode * 2, perNode * 3, perNode * 5 + 1,
                                  perNode * 8, perNode * 13};
        bool failed = false;
        for (size_t z = 0; z < countof(sizes) && !failed; z++) {
            for (size_t q = 0; q < countof(seqs) && !failed; q++) {
                uint32_t n = sizes[z];
                uint32_t nextId = SD_NEW;
                if (n + 64 > SD_MAX) {
                    ERR("size %u is too large for the oracle", n);
                    failed = true;
                    break;
                }

                multilistFull *ml =
                    multilistFullNew(FLEX_CAP_LEVEL_512, seqs[q][0]);
                for (uint32_t i = 0; i < n; i++) {
                    const databox box = mlfSplitBox(i, buf);
                    multilistFullPushByTypeTail(ml, s0, &box);
                    want[i] = i;
                }

                for (uint32_t k = 1; seqs[q][k] != UINT32_MAX && !failed; k++) {
                    const uint32_t d = seqs[q][k];
                    if (k % 2) {
                        multilistFullSetCompressDepth(ml, d);
                    } else {
                        multilistFullSetCompressDepthWithState(ml, s0, d);
                    }

                    mlNodeId bad = -1;
                    const char *why = ml->compress != d
                                          ? "the depth was not recorded"
                                          : mlfDepthRuleExact(ml, &bad);
                    if (!why) {
                        why = mlfSplitCheck(ml, s, want, n);
                    }

                    /* The list keeps working on the new depth. */
                    for (uint32_t p = 0; p < 8 && !why; p++) {
                        const databox box = mlfSplitBox(nextId, buf);
                        if (p % 2) {
                            multilistFullPushByTypeHead(ml, s0, &box);
                            memmove(&want[1], &want[0], n * sizeof(*want));
                            want[0] = nextId++;
                        } else {
                            multilistFullPushByTypeTail(ml, s0, &box);
                            want[n] = nextId++;
                        }

                        n++;
                    }

                    if (!why) {
                        why = mlfSplitCheck(ml, s, want, n);
                    }

                    if (!why) {
                        why = mlfDepthRuleCheck(ml, &bad);
                    }

                    if (why) {
                        ERR("%u elements, sequence %zu, step %u (depth %u, "
                            "%u nodes): node %d: %s",
                            sizes[z], q, k, d, (unsigned)ml->count, bad, why);
                        failed = true;
                    }
                }

                multilistFullFree(ml);
            }
        }
    }

    TEST("AppendFlex keeps the depth rule as appended nodes move inward") {
        /* Each append pushes the node 'depth' places before the new tail
         * into the interior, which must then be compressed.  Appends of
         * one-element and multi-element flexes alternate between
         * AppendFlex and AppendFlexWithState; after each one every node
         * must match the rule exactly and the list must match an array
         * oracle. */
        enum { AF_MAX = 512, AF_APPENDS = 9 };
        static uint32_t want[AF_MAX];
        char buf[MLF_SPLIT_LEN + 1];
        for (uint32_t d = 0; d <= 2; d++) {
            multilistFull *ml = multilistFullNew(FLEX_CAP_LEVEL_512, d);
            uint32_t n = 0;
            for (; n < 6; n++) {
                const databox box = mlfSplitBox(n, buf);
                multilistFullPushByTypeTail(ml, s0, &box);
                want[n] = n;
            }

            for (uint32_t a = 0; a < AF_APPENDS; a++) {
                const uint32_t take = a % 3 == 0 ? 1 : 4 + a;
                flex *fl = flexNew();
                for (uint32_t k = 0; k < take; k++) {
                    const databox box = mlfSplitBox(n, buf);
                    flexPushByType(&fl, &box, FLEX_ENDPOINT_TAIL);
                    want[n] = n;
                    n++;
                }

                if (a % 2) {
                    multilistFullAppendFlexWithState(ml, s0, fl);
                } else {
                    multilistFullAppendFlex(ml, fl);
                }

                mlNodeId bad = -1;
                const char *why = mlfDepthRuleExact(ml, &bad);
                if (!why) {
                    why = mlfSplitCheck(ml, s, want, n);
                }

                if (why) {
                    ERR("depth %u append %u (%u nodes): node %d: %s", d, a,
                        (unsigned)ml->count, bad, why);
                    break;
                }
            }

            multilistFullFree(ml);
        }
    }

    TEST("AppendFlex on an empty list, and of an empty flex, leaves no empty "
         "node") {
        /* A new list and a list emptied by pops each hold one empty
         * node: the appended flex must replace it.  Appending an empty
         * flex must add nothing.  The list must then match an array
         * oracle with no empty node, follow the depth rule, and pop its
         * head correctly. */
        constexpr int AE_N = 20;
        static uint32_t want[AE_N + 8];
        char buf[MLF_SPLIT_LEN + 1];
        for (uint32_t d = 0; d <= 1; d++) {
            for (int emptied = 0; emptied < 2; emptied++) {
                for (int withState = 0; withState < 2; withState++) {
                    multilistFull *ml = multilistFullNew(FLEX_CAP_LEVEL_512, d);
                    if (emptied) {
                        for (uint32_t i = 0; i < AE_N; i++) {
                            const databox box = mlfSplitBox(i, buf);
                            multilistFullPushByTypeTail(ml, s0, &box);
                        }

                        databox got = {};
                        while (multilistFullPopHead(ml, s0, &got)) {
                            databoxFreeData(&got);
                            memset(&got, 0, sizeof(got));
                        }
                    }

                    uint32_t n = 0;
                    for (int round = 0; round < 3; round++) {
                        /* empty, then 7 elements, then empty again */
                        flex *fl = flexNew();
                        for (uint32_t k = 0; round == 1 && k < 7; k++) {
                            const databox box = mlfSplitBox(100 + n, buf);
                            flexPushByType(&fl, &box, FLEX_ENDPOINT_TAIL);
                            want[n] = 100 + n;
                            n++;
                        }

                        if (withState) {
                            multilistFullAppendFlexWithState(ml, s0, fl);
                        } else {
                            multilistFullAppendFlex(ml, fl);
                        }

                        mlNodeId bad = -1;
                        const char *why = ml->count != 1
                                              ? "an empty node was kept"
                                              : mlfSplitCheck(ml, s, want, n);
                        if (!why) {
                            why = mlfDepthRuleExact(ml, &bad);
                        }

                        if (why) {
                            ERR("depth %u, %s list, %s, round %d: %s", d,
                                emptied ? "emptied" : "new",
                                withState ? "WithState" : "stateless", round,
                                why);
                        }
                    }

                    databox head = {};
                    if (!multilistFullPopHead(ml, s0, &head) ||
                        !mlfSplitIs(&head, want[0])) {
                        ERR("depth %u: the head popped wrong", d);
                    }

                    databoxFreeData(&head);
                    multilistFullFree(ml);
                }
            }
        }
    }

    TEST("NewFromFlexConsumeGrow keeps the depth rule for any input count") {
        /* 0 to 6 input flexes, from a uniform and a mixed size pattern
         * (empty, one-element and multi-element flexes), at depths 0 to
         * 2.  The list must hold the inputs' elements in order, with two
         * nodes per multi-element input and one per one-element input,
         * and every node must match the depth rule exactly. */
        enum { CG_MAX_IN = 6, CG_MAX = 256 };
        static uint32_t want[CG_MAX];
        static constexpr uint32_t patterns[2][CG_MAX_IN] = {
            {8, 8, 8, 8, 8, 8},
            {9, 1, 0, 14, 2, 1},
        };
        char buf[MLF_SPLIT_LEN + 1];
        for (uint32_t d = 0; d <= 2; d++) {
            for (uint32_t p = 0; p < 2; p++) {
                for (uint32_t in = 0; in <= CG_MAX_IN; in++) {
                    flex *fl[CG_MAX_IN];
                    uint32_t n = 0;
                    mlNodeId wantNodes = 0;
                    for (uint32_t i = 0; i < in; i++) {
                        const uint32_t c = patterns[p][i];
                        fl[i] = flexNew();
                        for (uint32_t k = 0; k < c; k++) {
                            const databox box = mlfSplitBox(n, buf);
                            flexPushByType(&fl[i], &box, FLEX_ENDPOINT_TAIL);
                            want[n] = n;
                            n++;
                        }

                        wantNodes += c >= 2 ? 2 : (mlNodeId)c;
                    }

                    if (wantNodes == 0) {
                        wantNodes = 1;
                    }

                    multilistFull *ml = multilistFullNewFromFlexConsumeGrow(
                        NULL, s0, fl, in, d, FLEX_CAP_LEVEL_512);
                    mlNodeId bad = -1;
                    const char *why =
                        ml->count != wantNodes ? "wrong node count" : NULL;
                    if (!why && ml->compress != d) {
                        why = "the depth was not recorded";
                    }

                    if (!why) {
                        why = mlfDepthRuleExact(ml, &bad);
                    }

                    if (!why) {
                        why = mlfSplitCheck(ml, s, want, n);
                    }

                    if (why) {
                        ERR("depth %u, pattern %u, %u inputs (%u nodes): "
                            "node %d: %s",
                            d, p, in, (unsigned)ml->count, bad, why);
                    }

                    multilistFullFree(ml);
                }
            }
        }
    }

    TEST("an empty flex appends no values and builds an empty list") {
        cleaner(flexFree) flex *none = flexNew();
        multilistFull *ml = multilistFullNewFromFlex(2, 0, s0, none);
        const size_t built = multilistFullCount(ml);
        const databox one = databoxNewSigned(1);
        multilistFullPushByTypeTail(ml, s0, &one);
        multilistFullAppendValuesFromFlex(ml, s0, none);
        multilistFullAppendValuesFlex(ml, s0, none);
        databox got = {};
        if (built != 0 || multilistFullCount(ml) != 1 ||
            !multilistFullPop(ml, s0, &got, false) || got.data.i != 1) {
            ERR("built %zu values, then hold %zu, want 0 then 1", built,
                multilistFullCount(ml));
        }
        multilistFullFree(ml);
    }

    TEST("wide: NewFromFlex, AppendValuesFromFlex, Rotate and Pop keep "
         "96/128-bit values") {
        /* Element i: a positive or negative wide value unique to i, a
         * string, or a negative 64-bit value.  Entries are 0xFF-poisoned
         * before every read. */
        constexpr int WIDE_N = 240;
        databoxBig want[WIDE_N];
        char strs[WIDE_N][32];
        cleaner(flexFree) flex *src = flexNew();
        for (int i = 0; i < WIDE_N; i++) {
            memset(&want[i], 0, sizeof(want[i]));
            switch (i % 4) {
            case 0:
                DATABOX_BIG_UNSIGNED_128(&want[i], ((__uint128_t)1 << 64) +
                                                       (__uint128_t)i);
                break;
            case 1:
                DATABOX_BIG_SIGNED_128(&want[i], -((__int128_t)1 << 95) - i);
                break;
            case 2: {
                const int n = snprintf(strs[i], sizeof(strs[i]),
                                       "full-wide-%d-padding", i);
                *(databox *)&want[i] = databoxNewBytes(strs[i], (size_t)n);
                break;
            }
            default:
                *(databox *)&want[i] = databoxNewSigned(-(int64_t)i * 7919);
                break;
            }

            flexPushByType(&src, (const databox *)&want[i], FLEX_ENDPOINT_TAIL);
        }

        for (uint32_t d = 0; d < 2; d++) {
            multilistFull *ml = multilistFullNewFromFlex(2, (int)d, s0, src);
            if (ml->count < 2 || multilistFullCount(ml) != WIDE_N) {
                ERR("depth %u: NewFromFlex gave %u nodes, %zu values", d,
                    (unsigned)ml->count, multilistFullCount(ml));
            }

            multilistIterator iter;
            multilistEntry entry;
            multilistFullIteratorInitForwardReadOnly(ml, s, &iter);
            for (int i = 0; i < WIDE_N; i++) {
                memset(&entry, 0xFF, sizeof(entry));
                if (!multilistFullNext(&iter, &entry) ||
                    !mlfWideSame(&entry.box, &want[i], &iter.wide)) {
                    ERR("depth %u: NewFromFlex element %d differs", d, i);
                    break;
                }
            }
            multilistFullIteratorRelease(&iter);

            /* Append the same values again: 2 x WIDE_N in order. */
            multilistFullAppendValuesFromFlex(ml, s0, src);
            for (int i = 0; i < 2 * WIDE_N; i++) {
                if (!mlfIndexSame(ml, s0, i, &want[i % WIDE_N])) {
                    ERR("depth %u: appended element %d differs", d, i);
                    break;
                }
            }

            /* Rotate: the tail (WIDE_N - 1, a 64-bit value), then a string
             * and two wide values move to the head. */
            for (int r = 0; r < 4; r++) {
                multilistFullRotate(ml, s);
                const int at = WIDE_N - 1 - r;
                if (!mlfIndexSame(ml, s0, 0, &want[at])) {
                    ERR("depth %u: rotate %d moved element %d wrong", d, r, at);
                }
            }

            /* Pop the rotated head back off (into a plain box, then a
             * databoxBig) and the original head of the list after it. */
            for (int r = 0; r < 4; r++) {
                const int at = WIDE_N - 4 + r;
                databoxBig got;
                if (r % 2) {
                    memset(&got, 0xFF, sizeof(got));
                    DATABOX_BIG_INIT(&got);
                } else {
                    memset(&got, 0, sizeof(got));
                }

                if (!multilistFullPopHead(ml, s0, (databox *)&got) ||
                    !mlfWideSame((databox *)&got, &want[at],
                                 r % 2 ? got.extra : NULL)) {
                    ERR("depth %u: pop %d of element %d differs", d, r, at);
                }
                databoxFreeData((databox *)&got);
            }

            databox head = {};
            if (!multilistFullPopHead(ml, s0, &head) ||
                !mlfWideSame(&head, &want[0], NULL) || !head.allocated) {
                ERR("depth %u: pop of the original head differs", d);
            }
            databoxFreeData(&head);
            multilistFullFree(ml);
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

    return err;
}
#endif
