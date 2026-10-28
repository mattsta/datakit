/* idRowsContractTest.c - persistOpsIdRows in the persistence contract */

#include "persistContract.h"

#ifdef DATAKIT_TEST

#include "../../idRows.h"
#include "../../str.h"

/* An id of op number 'i': mostly dense (so rows are replaced, changed and
 * deleted again), some far up the range (so the directory goes sparse) */
static uint64_t pcIdRowsId(uint64_t *rng, size_t size) {
    const uint64_t r = splitmix64(rng);
    if (r % 8 == 0) {
        return (UINT64_C(1) << 40) + splitmix64(rng) % 64 * 4096;
    }

    return splitmix64(rng) % (size + 8);
}

static void *pcIdRowsBuild(const persistContractBuild *b) {
    /* Rows of {any value, an integer count} */
    /* @own: caller; free=idRowsFree */
    idRows *r = idRowsNew(2);
    persistContractStart(b->log, r);

    uint64_t rng = b->seed;
    uint8_t buf[64];
    for (size_t i = 0; i < b->size; i++) {
        const databox id = DATABOX_UNSIGNED(pcIdRowsId(&rng, b->size));
        const uint64_t roll = splitmix64(&rng) % 10;
        databoxBig value;
        persistContractValue(&rng, &value, buf, sizeof(buf));
        if (roll == 0) {
            /* Only a change made is logged: replay meets the same rows */
            const databox *args[1] = {&id};
            if (idRowsDelete(r, id.data.u)) {
                persistContractRecord(b->log, PERSIST_OP_DELETE, args, 1);
            }
        } else if (roll == 1) {
            const databox field = DATABOX_UNSIGNED(0);
            const databox *args[3] = {&id, &field, (const databox *)&value};
            if (idRowsReplaceField(r, id.data.u, 0, (const databox *)&value)) {
                persistContractRecord(b->log, PERSIST_OP_UPDATE, args, 3);
            }
        } else if (roll == 2) {
            const databox field = DATABOX_UNSIGNED(1);
            const databox by =
                DATABOX_SIGNED((int64_t)(splitmix64(&rng) % 9) - 4);
            const databox *args[3] = {&id, &field, &by};
            if (idRowsFieldIncr(r, id.data.u, 1, by.data.i, nullptr)) {
                persistContractRecord(b->log, PERSIST_OP_NUM_INCR, args, 3);
            }
        } else {
            const databox count =
                DATABOX_SIGNED((int64_t)(splitmix64(&rng) % 1000) - 500);
            const databox *row[2] = {(const databox *)&value, &count};
            const databox *args[3] = {&id, row[0], row[1]};
            idRowsSet(r, id.data.u, row);
            persistContractRecord(b->log, PERSIST_OP_INSERT, args, 3);
        }

        if (b->size >= 100 && i == b->size / 3) {
            idRowsClear(r);
            persistContractRecord(b->log, PERSIST_OP_CLEAR, nullptr, 0);
        }
    }

    /* Each string as a row's value */
    for (size_t i = 0; i < b->stringCount; i++) {
        const databox id = DATABOX_UNSIGNED((UINT64_C(1) << 50) + i);
        const databox count = DATABOX_SIGNED(0);
        const databox *row[2] = {&b->strings[i], &count};
        const databox *args[3] = {&id, row[0], row[1]};
        idRowsSet(r, id.data.u, row);
        persistContractRecord(b->log, PERSIST_OP_INSERT, args, 3);
    }

    return r;
}

static bool pcIdRowsHoldsBytes(const void *structure, const databox *bytes) {
    idRowsIterator iter;
    idRowsIteratorInit(structure, &iter);
    uint64_t id;
    databox value = {};
    databox count = {};
    databox *row[2] = {&value, &count};
    databoxWide wide[2];
    while (idRowsIteratorNext(&iter, &id, row, wide)) {
        if (persistContractSameBytes(&value, bytes)) {
            return true;
        }

        value = (databox){};
    }

    return false;
}

const persistContractType persistContractIdRows = {
    .ops = &persistOpsIdRows,
    .build = pcIdRowsBuild,
    .holdsBytes = pcIdRowsHoldsBytes,
};

#endif /* DATAKIT_TEST */
