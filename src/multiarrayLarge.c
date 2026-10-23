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
    /* @own: caller; free=multiarrayLargeFree */
    multiarrayLarge *e = zcalloc(1, sizeof(*e));
    e->len = len;
    e->rowMax = rowMax;

    /* @own: container; free=multiarrayLargeFree */
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
        /* @own: container; free=multiarrayLargeFree */
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

    /* Now turn the medium container into a large container.
     * A single-node medium never ran the InsertAfter loop, so tmp.tail
     * stays NULL and the tail is the head itself. */
    /* @own: caller; free=multiarrayLargeFree */
    multiarrayLarge *e = zrealloc(medium, sizeof(*e));
    e->head = head;
    e->tail = tmp.tail ? tmp.tail : head;
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

size_t multiarrayLargeCount(const multiarrayLarge *mar) {
    /* Walk the xor-linked node list summing per-node entry counts. */
    size_t total = 0;
    const multiarrayLargeNode *prev = NULL;
    const multiarrayLargeNode *e = mar->head;
    while (e) {
        total += e->count;

        const multiarrayLargeNode *loopCurrentE = e;
        e = getNext(prev, loopCurrentE);
        prev = loopCurrentE;
    }

    return total;
}

size_t multiarrayLargeBytes(const multiarrayLarge *mar) {
    /* Bytes of element data storage (metadata overhead excluded) */
    return (size_t)mar->len * multiarrayLargeCount(mar);
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
        /* @own: container; free=multiarrayLargeFree */
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
    if (idx < 0) {
        /* Reverse: accumulate node counts from the tail, then convert
         * to a forward offset inside the landing node (mirror of
         * multiarrayMediumGet's reverse arithmetic) */
        const int32_t index = (-idx) - 1;
        multiarrayLargeNode *prev = NULL;
        multiarrayLargeNode *current = mar->tail;
        multiarrayLargeNode *next = NULL;
        int32_t accum = 0;

        while ((next = getNext(prev, current)) &&
               (accum + current->count) <= index) {
            accum += current->count;
            prev = current;
            current = next;
        }

        const int32_t offset = (int32_t)current->count + idx + accum;
        return current->data + (mar->len * offset);
    }

    multiarrayLargeResult worker =
        multiarrayLargeGetForwardWorker(mar->head, idx, mar->len);
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
    multiarrayLargeDeleteTaking(mar, idx, NULL);
}

void multiarrayLargeLocate(const multiarrayLarge *mar, const uint32_t idx,
                           multiarrayLargeNode **prev,
                           multiarrayLargeNode **node, uint32_t *offset) {
    const multiarrayLargeResult worker =
        multiarrayLargeGetForwardWorker(mar->head, (int32_t)idx, mar->len);
    *prev = worker.prev;
    *node = worker.current;
    *offset = (uint32_t)worker.offset;
}

/* Delete the 'count' entries from 'idx' (all within the array), located
 * once: whole nodes are unlinked, a partial node moves its tail once */
void multiarrayLargeDeleteRange(multiarrayLarge *mar, const uint32_t idx,
                                uint32_t count) {
    assert(idx + (size_t)count <= multiarrayLargeCount(mar));
    if (count == 0) {
        return;
    }

    const size_t len = mar->len;
    multiarrayLargeResult worker =
        multiarrayLargeGetForwardWorker(mar->head, (int32_t)idx, mar->len);
    uint32_t offset = (uint32_t)worker.offset;
    while (count) {
        multiarrayLargeNode *node = worker.current;
        const uint32_t inNode = node->count - offset;
        const uint32_t take = count < inNode ? count : inNode;
        if (take == node->count) {
            multiarrayLargeNode *prev = worker.prev;
            multiarrayLargeNode *next = worker.next;
            if (!prev && !next) {
                /* The last node stays, empty */
                zfree(node->data);
                node->data = NULL;
                node->count = 0;
                break;
            }

            /* Unlinked and freed; 'prev' now links to 'next' */
            multiarrayLargeNodeDelete(mar, worker);
            worker.current = next;
            worker.next = next ? getNext(prev, next) : NULL;
        } else {
            const uint32_t after = node->count - offset - take;
            memmove(node->data + len * offset,
                    node->data + len * (offset + take), len * after);
            node->count -= take;
            /* @own: container; free=multiarrayLargeFree */
            node->data = zrealloc(node->data, len * node->count);
            worker.prev = node;
            worker.current = worker.next;
            worker.next = worker.current ? getNext(node, worker.current) : NULL;
        }

        count -= take;
        offset = 0;
    }
}

/* Delete entry 'idx', first copying it to 'out' when 'out' is not NULL
 * (one locate for both) */
void multiarrayLargeDeleteTaking(multiarrayLarge *mar, const int32_t idx,
                                 void *out) {
    /* Delete:
     *   1. Find entry index in node
     *   2. Remove entry from its node.
     *   3. Shrink count of elements in node by one. */

    const uint16_t len = mar->len;
    multiarrayLargeResult worker =
        multiarrayLargeGetForwardWorker(mar->head, idx, len);

    multiarrayLargeNode *found = worker.current;
    if (out) {
        memcpy(out, worker.entry, len);
    }

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

    TEST("GetForward reaches what Get reaches, one node or many") {
        multiarrayLarge *mar = multiarrayLargeNew(sizeof(int64_t), 16);
        for (int64_t i = 0; i < 200; i++) {
            multiarrayLargeInsert(mar, (int32_t)i, &i);
            if (multiarrayLargeGetForward(mar, 0) !=
                multiarrayLargeGet(mar, 0)) {
                ERR("index 0 differs at %" PRId64 " entries", i + 1);
                break;
            }
        }
        for (int32_t i = 0; i < 200; i++) {
            const int64_t *got = multiarrayLargeGetForward(mar, (uint32_t)i);
            if (got != multiarrayLargeGet(mar, i) || *got != i) {
                ERR("GetForward(%" PRId32 ") differs from Get", i);
                break;
            }
        }
        multiarrayLargeFree(mar);
    }

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

    TEST("reverse (negative) indexing across multiple nodes") {
        /* Small rowMax forces multi-node lists; Get(-k) must return
         * the k-th element from the end, node-boundary independent */
        multiarrayLarge *mar = multiarrayLargeNew(sizeof(uint32_t), 4);

        for (int32_t i = 0; i < 21; i++) {
            uint32_t v = 1000 + (uint32_t)i;
            multiarrayLargeInsert(mar, i, &v);
        }

        int32_t bad = 0;
        for (int32_t k = 1; k <= 21; k++) {
            const uint32_t want = 1000 + (uint32_t)(21 - k);
            const uint32_t *got = multiarrayLargeGet(mar, -k);
            if (!got || *got != want) {
                bad++;
            }
        }
        if (bad > 0) {
            ERR("%d of 21 reverse lookups wrong", bad);
        }

        /* GetTail rides the same path */
        const uint32_t *tail = multiarrayLargeGetTail(mar);
        if (!tail || *tail != 1020) {
            ERRR("GetTail returned wrong element");
        }

        multiarrayLargeFree(mar);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
