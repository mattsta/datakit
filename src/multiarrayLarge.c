#include "multiarrayLarge.h"
#include "multiarrayLargeInternal.h"

#include "multiarrayMediumInternal.h"
#include "multiarrayMediumLarge.h"

#include "datakit.h"

typedef struct multiarrayLargeResult {
    multiarrayLargeNode *prev;
    multiarrayLargeNode *current;
    multiarrayLargeNode *next;
    void *entry;
    int32_t offset;
} multiarrayLargeResult;

typedef uintptr_t multiarrayLargeptrXorPtr;

multiarrayLarge *multiarrayLargeNew(uint16_t len, uint16_t rowMax) {
    multiarrayLarge *e = zcalloc(1, sizeof(*e));
    e->len = len;
    e->rowMax = rowMax;

    e->head = zcalloc(1, sizeof(*e->head));
    e->tail = e->head;
    return e;
}

DK_STATIC void
multiarrayLargeNodeInsertAfter(multiarrayLarge *mar,
                               const multiarrayLargeResult worker,
                               multiarrayLargeNode *newNode);
multiarrayLarge *multiarrayLargeFromMedium(multiarrayMedium *medium) {
    const uint16_t len = medium->len;
    const uint16_t rowMax = medium->rowMax;

    multiarrayLarge tmp = {0};
    multiarrayLargeNode *head = NULL;
    multiarrayLargeResult worker = {0};
    for (uint16_t i = 0; i < medium->count; i++) {
        multiarrayLargeNode *node = zcalloc(1, sizeof(*node));
        node->data = medium->node[i].data;
        node->count = medium->node[i].count;
        if (i == 0) {
            /* With one node, head == tail */
            head = node;
            worker.prev = head;
        } else {
            worker.current = worker.prev;
            multiarrayLargeNodeInsertAfter(&tmp, worker, node);
            worker.prev = node;
        }
    }

    /* All done with the medium inner node container */
    zfree(medium->node);

    /* Now turn the medium container into a large container */
    multiarrayLarge *e = zrealloc(medium, sizeof(*e));
    e->head = head;
    e->tail = tmp.tail;
    e->len = len;
    e->rowMax = rowMax;

    return e;
}

/* routine shorthands */
#define _ptrXor(a, b) ((multiarrayLargeNode *)((uintptr_t)(a) ^ (uintptr_t)(b)))
#define getNext(prev, current) _ptrXor(prev, (current)->prevNext)

/* unused, prevent compiler warning */
#if 0
#define getPrev(current, next) _ptrXor(current, (next)->prevNext)
#endif

void multiarrayLargeFreeInside(multiarrayLarge *mar) {
    if (mar) {
        multiarrayLargeNode *prev = NULL;
        multiarrayLargeNode *e = mar->head;
        while (e) {
            /* Goodbye data array */
            zfree(e->data);

            /* Get next while retaining 'e' for free. */
            multiarrayLargeNode *loopCurrentE = e;
            e = getNext(prev, loopCurrentE);
            prev = loopCurrentE;

            /* Now free 'e' since we don't need its ptrXor value anymore. */
            zfree(loopCurrentE);
        }
    }
}

void multiarrayLargeFree(multiarrayLarge *mar) {
    multiarrayLargeFreeInside(mar);
    zfree(mar);
}

DK_STATIC inline multiarrayLargeResult
multiarrayLargeGetForwardWorker(multiarrayLargeNode *e, int32_t idx,
                                uint32_t len) {
    multiarrayLargeNode *prev = NULL;
    multiarrayLargeNode *current = e;
    multiarrayLargeNode *next = NULL;

    int32_t accum = 0;
    while ((next = getNext(prev, current)) && (accum + current->count) <= idx) {
        accum += current->count;
        prev = current;
        current = next;
    }

    idx = idx - accum;
    multiarrayLargeResult worker = {.prev = prev,
                                    .current = current,
                                    .next = next,
                                    .entry = current->data + (len * idx),
                                    .offset = idx};

    return worker;
}

DK_STATIC void multiarrayLargeNodeDelete(multiarrayLarge *mar,
                                         const multiarrayLargeResult worker) {
    multiarrayLargeNode *prev = worker.prev;
    multiarrayLargeNode *current = worker.current;
    multiarrayLargeNode *next = worker.next;

    if (!next && !prev) {
        /* We always leave one node available, so just zero out the contents. */
        memset(current->data, 0, mar->len * current->count);
        current->count = 0;
        return;
    }

    /* Enter: prev -> current -> next
     * Exit: prev -> next */

    /* reverse: prev
     * [delete current]
     * forward: next->next */
    if (prev) {
        /* Set prev->next = current->next FORWARD
         * Set prev->prev = current->prev REVERSE */
        /* Delete 'current' and add 'next' */
        prev->prevNext = prev->prevNext ^ (uintptr_t)current ^ (uintptr_t)next;
    }

    if (next) {
        /* Delete 'current' and add 'prev' */
        next->prevNext = next->prevNext ^ (uintptr_t)current ^ (uintptr_t)prev;
    }

    if (!prev) {
        mar->head = next;
    }

    if (!next) {
        mar->tail = prev;
    }

    assert(mar->head && mar->tail);

    /* Goodbye node */
    zfree(worker.current->data);
    zfree(worker.current);
}

DK_STATIC void
multiarrayLargeNodeInsertAfter(multiarrayLarge *mar,
                               const multiarrayLargeResult worker,
                               multiarrayLargeNode *newNode) {
    // multiarrayLargeNode *prev = worker.prev;
    multiarrayLargeNode *current = worker.current;
    multiarrayLargeNode *next = worker.next;

    /* Enter: prev -> current -> next
     * Exit: prev -> current -> newNode -> next */

    /* reverse: newNode
     * [delete current]
     * forward: next->next */
    if (next) {
        next->prevNext =
            next->prevNext ^ (uintptr_t)current ^ (uintptr_t)newNode;
    }

    /* reverse: newNode
     * [delete next]
     * forward: current->next */
    current->prevNext =
        (uintptr_t)newNode ^ (uintptr_t)next ^ current->prevNext;

    /* reverse: current
     * forward: next */
    newNode->prevNext = (uintptr_t)current ^ (uintptr_t)next;
    if (!next) {
        mar->tail = newNode;
    }
}

DK_STATIC void multiarrayLargeNodeInsert(multiarrayLarge *mar,
                                         const multiarrayLargeResult worker,
                                         multiarrayLargeNode *newNode) {
    multiarrayLargeNode *prev = worker.prev;
    // multiarrayLargeNode *next = worker.next;
    multiarrayLargeNode *current = worker.current;

    /* Enter: prev -> current -> next
     * Exit: prev -> newNode -> current -> next */

    /* reverse: prev->prev
     * [delete current]
     * forward: newNode */
    if (prev) {
        prev->prevNext =
            prev->prevNext ^ (uintptr_t)current ^ (uintptr_t)newNode;
    }

    /* reverse: current->newNode
     * [delete prev]
     * forward: current->next */
    current->prevNext =
        (uintptr_t)newNode ^ (uintptr_t)prev ^ current->prevNext;

    /* reverse: prev
     * forward: current */
    newNode->prevNext = (uintptr_t)prev ^ (uintptr_t)current;
    if (!prev) {
        mar->head = newNode;
    }
}

void multiarrayLargeInsert(multiarrayLarge *mar, const int32_t idx,
                           const void *s) {
    const uint16_t len = mar->len;
    const uint16_t rowMax = mar->rowMax;
    multiarrayLargeResult worker =
        multiarrayLargeGetForwardWorker(mar->head, idx, len);
    multiarrayLargeNode *found = worker.current;

    const int32_t offset = worker.offset;
    const size_t offsetLen = offset * len;

    const int32_t remaining = found->count - offset;
    const size_t remainingLen = remaining * len;

#if 0
    printf("Worker Returned: [%p, %p (%d), %p], %d, %d\n", worker.prev, worker.current, found->count, worker.next, offset, remaining);
#endif

    assert(remaining >= 0 && "You tried to insert beyond your entires.");

    /* If there's room in the found node for more entries, add directly .*/
    if (found->count < rowMax) {
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
        multiarrayLargeNode *split = zcalloc(1, sizeof(*split));
        if (offset == 0 || offset == (int32_t)rowMax) {
            multiarrayMediumLargeSplitNew(split, s, len);
            /* Just place one entry in new node.  No moving necessary. */

            if (offset == 0) { /* found->count < rowMax; inserting at HEAD */
                multiarrayLargeNodeInsert(mar, worker, split);
            } else { /* inserting at TAIL */
                multiarrayLargeNodeInsertAfter(mar, worker, split);
            }
        } else if (remaining < offset) {
            /* Inserting AFTER current */
            multiarrayLargeNodeInsertAfter(mar, worker, split);
            multiarrayMediumLargeNodeNewAfter(split, found, remaining,
                                              remainingLen, offsetLen, s, len);
        } else {
            /* Update pointers in prev, current, next. */
            /* Inserting BEFORE current */
            multiarrayLargeNodeInsert(mar, worker, split);
            multiarrayMediumLargeNodeNew(split, found, offset, remainingLen,
                                         offsetLen, s, len);
        }
    }
}

void *multiarrayLargeGet(const multiarrayLarge *mar, const int32_t idx) {
    int32_t index = idx;
    bool reverse = idx < 0 ? true : false; /* negative index == reverse */
    multiarrayLargeNode *startNode = mar->head;
    if (reverse) {
        /* Convert reverse index into forward index */
        index = (-idx) - 1;
        startNode = mar->tail;
    }

    /* If we only have one node, no lookup, just
     * return the index directly. */
    const multiarrayLarge *prev = NULL;
    if (getNext(prev, startNode) == NULL) {
        return startNode->data + (mar->len * index);
    }

    /* TODO: verify index is proper for reverse after this return */
    multiarrayLargeResult worker =
        multiarrayLargeGetForwardWorker(startNode, index, mar->len);
    return worker.entry;
}

void *multiarrayLargeGetForward(const multiarrayLarge *mar,
                                const uint32_t index) {
    multiarrayLargeNode *startNode = mar->head;

    /* If we only have one node, no lookup, just
     * return the index directly. */
    const multiarrayLarge *prev = NULL;
    if (getNext(prev, startNode) == NULL) {
        return startNode->data + (mar->len * index);
    }

    multiarrayLargeResult worker =
        multiarrayLargeGetForwardWorker(startNode, index, mar->len);
    return worker.entry;
}

void *multiarrayLargeGetHead(const multiarrayLarge *mar) {
    return mar->head->data;
}

void *multiarrayLargeGetTail(const multiarrayLarge *mar) {
    if (mar->tail->count > 0) {
        return mar->tail->data + (mar->len * (mar->tail->count - 1));
    } else {
        return multiarrayLargeGet(mar, -1);
    }
}

void multiarrayLargeDelete(multiarrayLarge *mar, const int32_t idx) {
    /* Delete:
     *   1. Find entry index in node
     *   2. Remove entry from its node.
     *   3. Shrink count of elements in node by one. */

    const uint16_t len = mar->len;
    multiarrayLargeResult worker =
        multiarrayLargeGetForwardWorker(mar->head, idx, len);

    multiarrayLargeNode *found = worker.current;

    const int32_t offset = worker.offset;
    const size_t offsetLen = offset * len;

    const int32_t remaining = found->count - offset - 1;
    const size_t remainingLen = remaining * len;

    assert(remaining >= 0 && "You tried to delete beyond your entires.");

#if 0
    printf("Worker Returned: [%p, %p (%d), %p], %d, %d\n", worker.prev, worker.current, found->count, worker.next, offset, remaining);
#endif

    if (found->count == 1) {
        /* Delete entire node */
        multiarrayLargeNodeDelete(mar, worker);
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

int multiarrayLargeTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int err = 0;
    static const int rowMax = 512;
    static const int globalMax =
        7280; /* max 512 16-byte entries per container */

    TEST("create") {
        multiarrayLarge *mar = multiarrayLargeNew(sizeof(s16), rowMax);
        assert(mar->head);
        assert(mar->head == mar->tail);

        int count = 0;
        s16 s = {0};
        multiarrayLargeNode *preHead = mar->head;
        multiarrayLargeNode *preTail = mar->tail;
        multiarrayLargeInsert(mar, count, &s);
        assert(preHead == mar->head); /* we don't create a new head here */
        assert(preTail == mar->tail); /* we don't create a new tail either */
        assert(mar->head->data);

        multiarrayLargeFree(mar);
    }

    TEST("insert before") {
        multiarrayLarge *mar = multiarrayLargeNew(sizeof(s16), rowMax);
        assert(mar->head);
        assert(mar->head == mar->tail);

        // int count = 0;
        for (int idx = 0; idx < globalMax; idx++) {
            s16 s = {.a = idx, .b = idx};
            multiarrayLargeNode *preHead = mar->head;
            multiarrayLargeNode *preTail = mar->tail;
            multiarrayLargeInsert(mar, idx, &s);
            assert(preHead ==
                   mar->head); /* we are growing by TAILS here, not by HEADS */

            /* Assert we added a new tail when reaching rowMax */
            if (idx > 0 && idx % rowMax == 0) {
                assert(preTail != mar->tail);
            } else {
                assert(preTail == mar->tail);
            }

            multiarrayLargeResult got =
                multiarrayLargeGetForwardWorker(mar->head, idx, sizeof(s16));
            assert(got.current->count <= rowMax);
            assert(((s16 *)(got.entry))->a == idx);
            assert(((s16 *)(got.entry))->b == idx);
            assert(got.entry == multiarrayLargeGet(mar, idx));
            // count++;
        }

        for (int idx = 0; idx < globalMax; idx++) {
            multiarrayLargeResult got =
                multiarrayLargeGetForwardWorker(mar->head, idx, sizeof(s16));
            assert(((s16 *)(got.entry))->a == idx);
            assert(((s16 *)(got.entry))->b == idx);
            assert(got.entry == multiarrayLargeGet(mar, idx));
        }

        for (int idx = 0; idx < globalMax; idx++) {
            multiarrayLargeDelete(mar, 0);
        }

        assert(mar->head == mar->tail);
        assert((mar->head->prevNext ^ mar->tail->prevNext) == 0);

        multiarrayLargeFree(mar);
    }

    TEST("insert before constant zero") {
        multiarrayLarge *mar = multiarrayLargeNew(sizeof(s16), rowMax);
        assert(mar->head);
        assert(mar->head == mar->tail);

        // int count = 0;
        for (int idx = 0; idx < globalMax; idx++) {
            s16 s = {.a = idx, .b = idx};
            multiarrayLargeNode *preHead = mar->head;
            multiarrayLargeInsert(mar, 0, &s);
            /* Assert we only add a new head when reaching rowMax */
            if (idx > 0 && idx % rowMax == 0) {
                assert(preHead != mar->head);
            } else {
                assert(preHead == mar->head);
            }

            multiarrayLargeResult got =
                multiarrayLargeGetForwardWorker(mar->head, 0, sizeof(s16));
            assert(((s16 *)(got.entry))->a == idx);
            assert(((s16 *)(got.entry))->b == idx);
            assert(got.entry == multiarrayLargeGet(mar, 0));
            // count++;
        }

        /* Because we inserted at the head each time,
         * our list values are backwards (1023 -> 0) */
        for (int idx = 0; idx < globalMax; idx++) {
            multiarrayLargeResult got =
                multiarrayLargeGetForwardWorker(mar->head, idx, sizeof(s16));
            assert(((s16 *)(got.entry))->a == globalMax - 1 - idx);
            assert(((s16 *)(got.entry))->b == globalMax - 1 - idx);
            assert(got.entry == multiarrayLargeGet(mar, idx));
        }

        for (int idx = 0; idx < globalMax; idx++) {
            multiarrayLargeDelete(mar, 0);
        }

        assert(mar->head == mar->tail);
        assert((mar->head->prevNext ^ mar->tail->prevNext) == 0);

        multiarrayLargeFree(mar);
    }

    /* need to test:
     *   - speeds at different maxRow
     *   - insert after
     *   - insert into middle positions of rows (before/after)
     *   - delete from rows (0, max, middles) */
    TEST("insert after") {
        multiarrayLarge *mar = multiarrayLargeNew(sizeof(s16), rowMax);
        assert(mar->head);
        assert(mar->head == mar->tail);

        /* If we're inserting after 0, we need a 0 to insert after. */
        s16 _s = {.a = 0, .b = 0};
        multiarrayLargeInsert(mar, 0, &_s);

        // int count = 0;
        for (int idx = 0; idx < globalMax; idx++) {
            s16 s = {.a = idx, .b = idx};
            multiarrayLargeNode *preHead = mar->head;
            multiarrayLargeNode *preTail = mar->tail;
            multiarrayLargeInsert(mar, idx + 1, &s);
            assert(preHead ==
                   mar->head); /* we are growing by TAILS here, not by HEADS */

            /* Assert we added a new tail when reaching rowMax */
            if (idx > 0 && (idx + 1) % rowMax == 0) {
                assert(preTail != mar->tail);
            } else {
                assert(preTail == mar->tail);
            }

            multiarrayLargeResult got = multiarrayLargeGetForwardWorker(
                mar->head, idx + 1, sizeof(s16));
            assert(got.current->count <= rowMax);
            assert(((s16 *)(got.entry))->a == idx);
            assert(((s16 *)(got.entry))->b == idx);
            assert(got.entry == multiarrayLargeGet(mar, idx + 1));
            // count++;
        }

        for (int idx = 0; idx < globalMax; idx++) {
            multiarrayLargeResult got = multiarrayLargeGetForwardWorker(
                mar->head, idx + 1, sizeof(s16));
            assert(((s16 *)(got.entry))->a == idx);
            assert(((s16 *)(got.entry))->b == idx);
            assert(got.entry == multiarrayLargeGet(mar, idx + 1));
        }

        for (int idx = 0; idx < globalMax; idx++) {
            multiarrayLargeDelete(mar, 0);
        }

        assert(mar->head == mar->tail);
        assert((mar->head->prevNext ^ mar->tail->prevNext) == 0);

        multiarrayLargeFree(mar);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
