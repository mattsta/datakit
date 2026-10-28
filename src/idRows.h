#pragma once

#include "databox.h"
#include "flex.h"

/* idRows: rows of 'elementsPerRow' databoxes addressed by dense integer
 * ids, read and written in place without a search.
 *
 * Ids are grouped in pages of IDROWS_PAGE consecutive ids.  A page is one
 * flex: entry 0 is its index (a 64-bit presence mask, then one 32-bit
 * offset per present id), then the present rows in id order.  A read is
 * the page pointer, a bit test and a popcount into the index, and the
 * row: no comparison, whatever the row's values.  The index costs 8 bytes
 * a page and 4 a row, so a page holding a single row stays small: ids
 * may be sparse, and one page pointer per IDROWS_PAGE ids (up to the
 * highest page) is the only cost of the holes.
 *
 * Values are stored exactly (flex's exact write path).  A read decodes
 * into the caller's boxes: byte strings point into the page and stay
 * valid until the next change to the rows; a 96/128-bit value needs a
 * slot (the *WithSlots forms) or reports its type only. */

#define IDROWS_PAGE 64u

typedef struct idRows idRows;

[[nodiscard]] idRows *idRowsNew(uint32_t elementsPerRow);
void idRowsFree(idRows *r);

/* Rows held, and the bytes of the pages and the page pointers */
size_t idRowsCount(const idRows *r);
size_t idRowsBytes(const idRows *r);

bool idRowsExists(const idRows *r, uint64_t id);
/* The row of 'id' into 'row' (elementsPerRow boxes); false when absent.
 * WithSlots decodes a 96/128-bit element into slots[i]. */
bool idRowsGet(const idRows *r, uint64_t id, databox *row[]);
bool idRowsGetWithSlots(const idRows *r, uint64_t id, databox *row[],
                        databoxWide *slots);

/* Store 'row' as the row of 'id', replacing any row it had: true when
 * the id was new */
bool idRowsSet(idRows *r, uint64_t id, const databox *row[]);
/* Replace element 'field' of the row of 'id' with 'box'; false when the
 * id is absent */
bool idRowsReplaceField(idRows *r, uint64_t id, uint32_t field,
                        const databox *box);
/* Add 'incrBy' to integer element 'field' of the row of 'id' (booleans
 * count as 0 and 1, as flexIncrbySigned takes them).  True with the sum
 * in '*newValue' (may be NULL); false, the row unchanged, for an absent
 * id, an element that is not a 64-bit integer, or a sum past int64_t,
 * with '*newValue' the element's current value (0 when it has none). */
bool idRowsFieldIncr(idRows *r, uint64_t id, uint32_t field, int64_t incrBy,
                     int64_t *newValue);
/* Remove the row of 'id': true when it was present */
bool idRowsDelete(idRows *r, uint64_t id);

/* The lowest and the highest id present; false when empty */
bool idRowsFirst(const idRows *r, uint64_t *id);
bool idRowsLast(const idRows *r, uint64_t *id);

/* Every row in id order.  Changing the rows ends an iteration. */
typedef struct idRowsIterator {
    const idRows *r;
    uint64_t next; /* the next id to look at */
} idRowsIterator;

void idRowsIteratorInit(const idRows *r, idRowsIterator *iter);
/* The next present row: its id and elements (wide ones into 'slots',
 * which may be NULL); false at the end */
bool idRowsIteratorNext(idRowsIterator *iter, uint64_t *id, databox *row[],
                        databoxWide *slots);

#ifdef DATAKIT_TEST
int idRowsTest(int argc, char *argv[]);
#endif
