/* multilistContractTest.c - persistOpsMultilist in the persistence
 * contract */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../cleaner.h"
#include "../../multilist.h"
#include "../../str.h"

defcleaner(mflexState *, mflexStateFree);

static void pcMultilistPush(multilist **ml, mflexState *state,
                            const persistContractBuild *b, const databox *v,
                            bool tail) {
    if (tail) {
        multilistPushByTypeTail(ml, state, v);
    } else {
        multilistPushByTypeHead(ml, state, v);
    }
    persistContractRecord(
        b->log, tail ? PERSIST_OP_PUSH_TAIL : PERSIST_OP_PUSH_HEAD, v, 1);
}

static void *pcMultilistBuild(const persistContractBuild *b) {
    cleaner(mflexStateFree) mflexState *state = mflexStateCreate();
    cleaner(mflexStateFree) mflexState *second = mflexStateCreate();
    /* A small node size, so builds reach the Medium and Full tiers */
    /* @own: caller; free=multilistFree */
    multilist *ml = multilistNew(FLEX_CAP_LEVEL_256, 0);
    persistContractStart(b->log, ml);

    uint64_t rng = b->seed;
    uint8_t buf[64];
    for (size_t i = 0; i < b->size; i++) {
        const int64_t count = (int64_t)multilistCount(ml);
        /* Mostly pushes, so large builds grow through every tier */
        const uint64_t roll = splitmix64(&rng) % 32;
        if (roll < 22 || count == 0) {
            databoxBig v;
            persistContractValue(&rng, &v, buf, sizeof(buf));
            pcMultilistPush(&ml, state, b, (const databox *)&v, roll & 1);
        } else if (roll < 24) {
            const bool tail = roll == 23;
            databox got = {{0}};
            multilistPop(&ml, state, &got, tail);
            databoxFreeData(&got);
            persistContractRecord(
                b->log, tail ? PERSIST_OP_POP_TAIL : PERSIST_OP_POP_HEAD, NULL,
                0);
        } else if (roll == 24) {
            /* A short range from either end (negative starts count from
             * the tail) */
            const int64_t start =
                (int64_t)(splitmix64(&rng) % (uint64_t)(2 * count)) - count;
            const int64_t n = 1 + (int64_t)(splitmix64(&rng) % 4);
            const databox s = DATABOX_SIGNED(start);
            const databox c = DATABOX_SIGNED(n);
            const databox *args[] = {&s, &c};
            multilistDelRange(&ml, state, start, n);
            persistContractRecord(b->log, PERSIST_OP_DELETE_AT, args, 2);
        } else if (roll == 25) {
            /* Trim a few from each end */
            const int64_t head = (int64_t)(splitmix64(&rng) % 3);
            const int64_t tail = (int64_t)(splitmix64(&rng) % 3);
            const databox h = DATABOX_UNSIGNED((uint64_t)head);
            const databox t = DATABOX_UNSIGNED((uint64_t)tail);
            const databox *args[] = {&h, &t};
            if (head > 0) {
                multilistDelRange(&ml, state, 0, head);
            }
            if (tail > 0) {
                multilistDelRange(&ml, state, -tail, tail);
            }
            persistContractRecord(b->log, PERSIST_OP_TRIM, args, 2);
        } else if (roll == 26) {
            if (count > 1) {
                mflexState *pair[2] = {state, second};
                multilistRotate(ml, pair);
            }
            persistContractRecord(b->log, PERSIST_OP_ROTATE, NULL, 0);
        } else {
            databoxBig v;
            persistContractValue(&rng, &v, buf, sizeof(buf));
            const int64_t at = (int64_t)(splitmix64(&rng) % (uint64_t)count);
            const databox idx = DATABOX_SIGNED(at);
            const databox *args[] = {&idx, (const databox *)&v};
            multilistReplaceByTypeAtIndex(&ml, state, at, (const databox *)&v);
            persistContractRecord(b->log, PERSIST_OP_REPLACE, args, 2);
        }

        if (b->size >= 100 && i == b->size / 3) {
            multilistFree(ml);
            /* @own: caller; free=multilistFree */
            ml = multilistNew(FLEX_CAP_LEVEL_2048, 0);
            persistContractRecord(b->log, PERSIST_OP_CLEAR, NULL, 0);
        }
    }

    for (size_t i = 0; i < b->stringCount; i++) {
        pcMultilistPush(&ml, state, b, &b->strings[i], i & 1);
    }
    return ml;
}

static bool pcMultilistHoldsBytes(const void *structure, const databox *bytes) {
    const multilist *ml = structure;
    cleaner(mflexStateFree) mflexState *a = mflexStateCreate();
    cleaner(mflexStateFree) mflexState *c = mflexStateCreate();
    mflexState *pair[2] = {a, c};
    multilistIterator iter = {0};
    multilistIteratorInitForwardReadOnly((multilist *)ml, pair, &iter);
    multilistEntry entry = {0};
    bool found = false;
    while (!found && multilistNext(&iter, &entry)) {
        found = persistContractSameBytes(&entry.box, bytes);
    }
    multilistIteratorRelease(&iter);
    return found;
}

const persistContractType persistContractMultilist = {
    .ops = &persistOpsMultilist,
    .build = pcMultilistBuild,
    .holdsBytes = pcMultilistHoldsBytes,
};

#endif /* DATAKIT_TEST */
