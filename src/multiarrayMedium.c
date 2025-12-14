#include "multiarrayMedium.h"
#include "multiarrayMediumInternal.h"
#include "multiarraySmallInternal.h"

#include "multiarrayMediumLarge.h"

#include <assert.h>

/* We typedef'd multiarraySmallNode to multiarrayMediumNode because
 * all we need is 'data' and 'count' and small has them both already. */
typedef struct multiarrayMediumResult {
    multiarrayMediumNode *current;
    void *entry;
    int32_t offset;
    uint32_t nodeIdx;
} multiarrayMediumResult;

multiarrayMedium *multiarrayMediumNewWithData(int16_t len, int16_t rowMax,
                                              uint16_t count, void *data) {
    multiarrayMedium *mar = zcalloc(1, sizeof(*mar));
    mar->node = zcalloc(2, sizeof(*mar->node));
    mar->node[0].data = data;
    mar->node[0].count = count;

#if 0
    mar->node[1].data = zcalloc(1, len);
    mar->node[1].count = 0;
#endif

    mar->count = 2;

    mar->len = len;
    mar->rowMax = rowMax;

    return mar;
}

DK_STATIC multiarrayMedium *multiarrayMediumNew_(int16_t len, int16_t rowMax,
                                                 multiarrayMediumNode *node) {
    multiarrayMedium *mar;

    /* Allocate one unused node length in our node array
     * since the user will probably start using this
     * right away. */
    if (node) {
        /* Copy contents of our initializer node
         * to temp storage so we can realloc our temp
         * node into our new container.
         * Then, restore initializer node as our first
         * inner node whlie also allocating an extra
         * unused node. */
        multiarrayMediumNode tmp = *node;
        mar = zrealloc(node, sizeof(*mar));
        mar->node = zcalloc(2, sizeof(*mar->node));
        mar->node[0] = tmp;
        mar->count = 2;
    } else {
        mar = zcalloc(1, sizeof(*mar));
        mar->node = zcalloc(1, sizeof(*mar->node));
        mar->count = 1;
    }

    /* Accounting data.  Free to store since we need to pad out
     * our struct to an 8 byte boundary anyway. */
    mar->len = len;
    mar->rowMax = rowMax;

    return mar;
}

multiarrayMedium *multiarrayMediumNew(int16_t len, int16_t rowMax) {
    return multiarrayMediumNew_(len, rowMax, NULL);
}

/* Initialize a new Medium from Small */
multiarrayMedium *multiarrayMediumFromSmall(multiarraySmall *small) {
    return multiarrayMediumNew_(small->len, small->rowMax, small);
}

void multiarrayMediumFreeInside(multiarrayMedium *mar) {
    if (mar) {
        for (size_t nodeIdx = 0; nodeIdx < mar->count; nodeIdx++) {
            zfree(mar->node[nodeIdx].data);
        }

        zfree(mar->node);
    }
}

void multiarrayMediumFree(multiarrayMedium *mar) {
    multiarrayMediumFreeInside(mar);
    zfree(mar);
}

#define getNode(idx) (&mar->node[idx])
DK_STATIC multiarrayMediumResult
multiarrayMediumGetForwardWorker(multiarrayMedium *mar, int32_t idx) {
    int32_t accum = 0;
    uint32_t nodeIdx = 0;
    /* While there are more nodes... */
    while ((nodeIdx + 1) < mar->count &&
           ((accum + getNode(nodeIdx)->count) <= idx)) {
        accum += getNode(nodeIdx)->count;
        nodeIdx++;
    }

    idx = idx - accum;
    void *nodeData = getNode(nodeIdx)->data;
    multiarrayMediumResult worker = {
        .current = getNode(nodeIdx),
        .entry = nodeData ? (uint8_t *)nodeData + (mar->len * idx) : NULL,
        .offset = idx,
        .nodeIdx = nodeIdx};

    return worker;
}

DK_STATIC multiarrayMediumNode *
multiarrayMediumNodeInsert(multiarrayMedium *mar, uint32_t nodeIdx) {
    multiarraySmallDirectInsert(mar->node, mar->count, nodeIdx);
    mar->count++;
    return mar->node + nodeIdx;
}

DK_STATIC multiarrayMediumNode *
multiarrayMediumNodeInsertAfter(multiarrayMedium *mar, uint32_t nodeIdx) {
    return multiarrayMediumNodeInsert(mar, nodeIdx + 1);
}

DK_STATIC void multiarrayMediumNodeDelete(multiarrayMedium *mar,
                                          uint32_t nodeIdx) {
    multiarrayMediumNode *node = getNode(nodeIdx);
    if (mar->count == 1) {
        /* Always leave one node available.  Just zero out contents. */
        memset(node->data, 0, mar->len * node->count);
        node->count = 0;
    } else {
        if (node->count == 1) {
            /* We're about to delete the node, so free the node's data. */
            zfree(node->data);
        }

        multiarraySmallDirectDelete(mar->node, mar->count, nodeIdx);
        mar->count--;
    }
}

void multiarrayMediumInsert(multiarrayMedium *mar, const int32_t idx,
                            const void *s) {
    multiarrayMediumResult worker = multiarrayMediumGetForwardWorker(mar, idx);
    multiarrayMediumNode *found = worker.current;

    const uint16_t len = mar->len;
    const int32_t offset = worker.offset;
    const size_t offsetLen = offset * len;

    const int32_t remaining = found->count - offset;
    const size_t remainingLen = remaining * len;

#if 0
    printf("Worker Returned: [%p (%d)], %d, %d\n", worker.current, found->count,
           offset, remaining);
#endif

    assert(remaining >= 0 && "You tried to insert beyond your entires.");

    /* If there's room in the found node for more entries, add directly .*/
    if (found->count < mar->rowMax) {
        /* increase count, realloc, move entries up, write new entry. */
        multiarrayMediumLargeInsertAtIdx(found, remaining, remainingLen,
                                         offsetLen, found->count, s, len);
        found->count++;
    } else { /* else, we need to add a new node and insert it somewhere. */
             /* split entries at 'offset', write 'offset' to smallest half. */
             /* [CURRENT] -> [SPLIT][OLD CURRENT]
              *  - or -
              * [CURRENT] -> [OLD CURRENT][SPLIT]
              *  - or -
              * [CURRENT] -> [CURRENT][SPLIT] */
        multiarrayMediumNode *split = NULL;
        if (offset == 0) { /* found->count < rowMax; inserting at HEAD */
            split = multiarrayMediumNodeInsert(mar, worker.nodeIdx);
            multiarrayMediumLargeSplitNew(split, s, len);
        } else if (offset == mar->rowMax) { /* inserting at TAIL */
            split = multiarrayMediumNodeInsertAfter(mar, worker.nodeIdx);
            multiarrayMediumLargeSplitNew(split, s, len);
        } else if (remaining < offset) {
            /* Inserting AFTER current */
            split = multiarrayMediumNodeInsertAfter(mar, worker.nodeIdx);
            found = getNode(worker.nodeIdx);
            multiarrayMediumLargeNodeNewAfter(split, found, remaining,
                                              remainingLen, offsetLen, s, len);
        } else {
            /* Update pointers in prev, current, next. */
            /* Inserting BEFORE current */
            split = multiarrayMediumNodeInsert(mar, worker.nodeIdx);
            found = getNode(worker.nodeIdx + 1);
            multiarrayMediumLargeNodeNew(split, found, offset, remainingLen,
                                         offsetLen, s, len);
        }
    }
}

void *multiarrayMediumGet(const multiarrayMedium *mar, const int32_t idx) {
    int32_t index = idx;
    int32_t nodeIdx = 0;
    bool reverse = idx < 0 ? true : false; /* negative index == reverse */
    if (reverse) {
        /* Convert reverse index into zero-based forward conterpart. */
        index = (-idx) - 1;
        nodeIdx = mar->count - 1;
    }

    int32_t accum = 0;
    multiarrayMediumNode *node = getNode(nodeIdx);
    while ((accum + node->count) <= index) {
        accum += node->count;
        if (reverse) {
            node = getNode(--nodeIdx);
        } else {
            node = getNode(++nodeIdx);
        }
    }

    assert(node->count > 0);

    int32_t offset = index - accum;
    if (reverse) {
        /* Convert reverse index to forward index */
        offset = (node->count) + ((-index) - 1 + accum);
    }

    return node->data + (offset * mar->len);
}

void *multiarrayMediumGetForward(const multiarrayMedium *mar,
                                 const uint32_t index) {
    int32_t nodeIdx = 0;

    uint32_t accum = 0;
    multiarrayMediumNode *node = getNode(nodeIdx);
    while ((accum + node->count) <= index) {
        accum += node->count;
        node = getNode(++nodeIdx);
    }

    assert(node->count > 0);

    int32_t offset = index - accum;
    return node->data + (offset * mar->len);
}

void *multiarrayMediumGetHead(const multiarrayMedium *mar) {
    return getNode(0)->data;
}

void *multiarrayMediumGetTail(const multiarrayMedium *mar) {
    const multiarrayMediumNode *node = getNode(mar->count - 1);
    if (node->count > 0) {
        return node->data + (mar->len * (node->count - 1));
    }

    return multiarrayMediumGet(mar, -1);
}

void multiarrayMediumDelete(multiarrayMedium *mar, const int32_t idx) {
    /* Delete:
     *   1. Find entry index in node
     *   2. Remove entry from its node.
     *   3. Shrink count of elements in node by one. */

    multiarrayMediumResult worker = multiarrayMediumGetForwardWorker(mar, idx);

    multiarrayMediumNode *found = worker.current;

    const uint16_t len = mar->len;
    const int32_t offset = worker.offset;
    const size_t offsetLen = offset * len;

    const int32_t remaining = found->count - offset - 1;
    const size_t remainingLen = remaining * len;

    assert(remaining >= 0 && "You tried to delete beyond your entires.");

    if (found->count == 1) {
        /* Delete entire node */
        multiarrayMediumNodeDelete(mar, worker.nodeIdx);
    } else {
        /* Delete found offset in node. */
        multiarrayMediumLargeDeleteAtIdx(found, remaining, remainingLen,
                                         offsetLen, found->count, mar->len);
        found->count--;
    }
}

#ifdef DATAKIT_TEST
#include "ctest.h"

#include <assert.h>

typedef struct s16 {
    int64_t a;
    int64_t b;
} s16;

int multiarrayMediumTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    static const int rowMax = 512;
    static const int globalMax =
        7280; /* max 512 16-byte entries per container */

    TEST("create") {
        multiarrayMedium *mar = multiarrayMediumNew(sizeof(s16), rowMax);

        s16 s = {0};
        multiarrayMediumInsert(mar, 0, &s);
        assert(mar->node[0].data);

        multiarrayMediumFree(mar);
    }

    TEST("insert before") {
        multiarrayMedium *mar = multiarrayMediumNew(sizeof(s16), rowMax);

        // int count = 0;
        for (int idx = 0; idx < globalMax; idx++) {
            s16 s = {.a = idx, .b = idx};
            multiarrayMediumInsert(mar, idx, &s);

            multiarrayMediumResult got =
                multiarrayMediumGetForwardWorker(mar, idx);
            assert(got.current->count <= rowMax);
            assert(((s16 *)(got.entry))->a == idx);
            assert(((s16 *)(got.entry))->b == idx);
            assert(got.entry == multiarrayMediumGet(mar, idx));
            // count++;
        }

        for (int idx = 0; idx < globalMax; idx++) {
            multiarrayMediumResult got =
                multiarrayMediumGetForwardWorker(mar, idx);
            assert(((s16 *)(got.entry))->a == idx);
            assert(((s16 *)(got.entry))->b == idx);
            assert(got.entry == multiarrayMediumGet(mar, idx));
        }

        multiarrayMediumFree(mar);
    }

    TEST("insert before constant zero") {
        multiarrayMedium *mar = multiarrayMediumNew(sizeof(s16), rowMax);

        // int count = 0;
        for (int idx = 0; idx < globalMax; idx++) {
            s16 s = {.a = idx, .b = idx};

            uint32_t marPrevCount = mar->count;
            multiarrayMediumInsert(mar, 0, &s);
            if (idx && idx % rowMax == 0) {
                assert(mar->count != marPrevCount);
                assert(mar->node[1].count == rowMax);
            }

            multiarrayMediumResult got =
                multiarrayMediumGetForwardWorker(mar, 0);
            assert(((s16 *)(got.entry))->a == idx);
            assert(((s16 *)(got.entry))->b == idx);
            assert(got.entry == multiarrayMediumGet(mar, 0));
            // count++;
        }

        /* Because we inserted at the head each time,
         * our list values are backwards (1023 -> 0) */
        for (int idx = 0; idx < globalMax; idx++) {
            multiarrayMediumResult got =
                multiarrayMediumGetForwardWorker(mar, idx);
            assert(((s16 *)(got.entry))->a == globalMax - 1 - idx);
            assert(((s16 *)(got.entry))->b == globalMax - 1 - idx);
            assert(got.entry == multiarrayMediumGet(mar, idx));
        }

        for (int idx = 0; idx < globalMax; idx++) {
            multiarrayMediumDelete(mar, 0);
        }

        assert(mar->count == 1);

        multiarrayMediumFree(mar);
    }

    /* need to test:
     *   - speeds at different maxRow
     *   - insert after
     *   - insert into middle positions of rows (before/after)
     *   - delete from rows (0, max, middles) */
    TEST("insert after") {
        multiarrayMedium *mar = multiarrayMediumNew(sizeof(s16), rowMax);

        /* If we're inserting after 0, we need a 0 to insert after. */
        s16 _s = {.a = 0, .b = 0};
        multiarrayMediumInsert(mar, 0, &_s);

        // int count = 0;
        for (int idx = 0; idx < globalMax; idx++) {
            s16 s = {.a = idx, .b = idx};
            multiarrayMediumInsert(mar, idx + 1, &s);

            multiarrayMediumResult got =
                multiarrayMediumGetForwardWorker(mar, idx + 1);
            assert(got.current->count <= rowMax);
            assert(((s16 *)(got.entry))->a == idx);
            assert(((s16 *)(got.entry))->b == idx);
            assert(got.entry == multiarrayMediumGet(mar, idx + 1));
            // count++;
        }

        for (int idx = 0; idx < globalMax; idx++) {
            multiarrayMediumResult got =
                multiarrayMediumGetForwardWorker(mar, idx + 1);
            assert(((s16 *)(got.entry))->a == idx);
            assert(((s16 *)(got.entry))->b == idx);
            assert(got.entry == multiarrayMediumGet(mar, idx + 1));
        }

        for (int idx = 0; idx < globalMax; idx++) {
            multiarrayMediumDelete(mar, 0);
        }

        assert(mar->count == 1);

        multiarrayMediumFree(mar);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
