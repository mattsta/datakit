# idRows: rows addressed by id

**Header**: `idRows.h` **Source**: `idRows.c` **Persistence**: `persistOpsIdRows`

idRows holds rows of `elementsPerRow` databoxes, each row named by a `uint64_t`
id. Reading a row by its id involves no comparison of any value: the id alone
locates the row. It is the forward store of every atom table
(`{value, refcount}` by atom id), and fits anywhere ids are handed out by a
counter or a free list.

## API

| Call                                                                      | Does                                                       |
| ------------------------------------------------------------------------- | ---------------------------------------------------------- |
| `idRowsNew(elementsPerRow)` / `idRowsFree(r)`                             | make and free; every row has `elementsPerRow` elements     |
| `idRowsSet(r, id, row)`                                                   | store the row of `id`, replacing any it had (true: new id) |
| `idRowsGet(r, id, row)`                                                   | read the row of `id` into `row[]` boxes (false: absent)    |
| `idRowsGetWithSlots(r, id, row, slots)`                                   | as Get, decoding 96/128-bit elements into `slots[i]`       |
| `idRowsReplaceField(r, id, field, box)`                                   | replace one element in place (false: absent id)            |
| `idRowsFieldIncr(r, id, field, by, &sum)`                                 | add to an integer element in place, refusing overflow      |
| `idRowsDelete(r, id)` / `idRowsClear(r)`                                  | remove one row / every row                                 |
| `idRowsExists(r, id)`                                                     | whether `id` has a row                                     |
| `idRowsFirst(r, &id)` / `idRowsLast(r, &id)`                              | the lowest / highest id present (false: empty)             |
| `idRowsIteratorInit(r, &it)` / `idRowsIteratorNext(&it, &id, row, slots)` | every row in id order                                      |
| `idRowsCount(r)` / `idRowsBytes(r)` / `idRowsElementsPerRow(r)`           | rows held, bytes held, row width                           |

```c
#include "idRows.h"

/* Sessions by id: {user name, request count} */
idRows *sessions = idRowsNew(2);

const databox user = databoxNewBytesString("alice");
const databox zero = databoxNewSigned(0);
const databox *row[2] = {&user, &zero};
idRowsSet(sessions, 1001, row);

int64_t requests = 0;
idRowsFieldIncr(sessions, 1001, 1, 1, &requests); /* in place: requests == 1 */

databox name, count;
databox *out[2] = {&name, &count};
if (idRowsGet(sessions, 1001, out)) {
    /* 'name' points into the page: valid until the next change to the rows */
}

idRowsIterator it;
idRowsIteratorInit(sessions, &it);
uint64_t id;
while (idRowsIteratorNext(&it, &id, out, nullptr)) {
    /* rows in id order; changing the rows ends the iteration */
}

idRowsFree(sessions);
```

Rules worth knowing:

- **Byte strings read out point into the rows** and stay valid only until the next change (Set,
  ReplaceField, FieldIncr, Delete, Clear). Copy what you keep.
- **Wide integers** (96/128-bit) need slots: `idRowsGetWithSlots`, or the iterator's `slots`
  argument. Without them the element reports its type only.
- **`idRowsFieldIncr` refuses rather than wraps**: an element that is not a 64-bit integer, or a sum
  past `int64_t`, leaves the row unchanged and returns false (the current value in `*newValue`).
- **Any `uint64_t` id is valid**, dense or not; dense ids read fastest (see Costs).

## Layout

```
idRows
 ├── directory: finds a page by its page number (id / 64)
 │     dense:  page[] indexed by (pageNumber - base)    while a quarter or
 │             [base, base + span), NULL for empty pages  more of span is used
 │     sparse: pageNo[] sorted, page[] parallel        otherwise
 │             (binary search)
 │
 └── page (one flex per 64 consecutive ids that hold at least one row)

     ┌──────────────── entry 0: the page index (bytes) ───────────────┐
     │ present: uint64 LE   bit s set = id (64p + s) has a row         │
     │ offset[0..capacity): uint32 LE, the first `rows` in use:        │
     │   offset[k] = bytes from the index's first byte to the row of   │
     │   the k-th present id (k = popcount(present below its bit))     │
     └─────────────────────────────────────────────────────────────────┘
     entry 1..: the rows, in id order, each `elementsPerRow` flex entries
```

A read: directory → page → test the id's bit → popcount the bits below it →
that offset → decode the row's entries. Rows are stored exactly as given
(`"22"` stays bytes; 96/128-bit values keep their width).

## Changing rows

- **Set (new id)**: the row's entries are inserted where the next present
  row starts (or at the end), the later offsets move up one place and by
  the row's bytes, and the id's bit is set. A page whose offsets are full
  doubles its capacity first (one rewrite of the index entry; every
  offset then moves by the bytes the entry grew).
- **Set (present id)**, **replace a field**, **increment a field**: the row
  changes in place; the later offsets move by the bytes it gained or lost.
- **Delete**: the row's entries are removed, the later offsets move down
  one place, and the bit clears. No sentinel is left: the count, the
  offsets and the bitmap all shrink. The index halves its capacity when a
  quarter is in use, and a page left with no row is freed and leaves the
  directory.

Nothing else moves: a flex header that grows shifts the index and the rows
together, so offsets (measured from the index) stay valid.

## Costs

|                     | bytes                                                       |
| ------------------- | ----------------------------------------------------------- |
| a row               | its entries + 4 (offset) to 8 (offset plus capacity room)   |
| a page holding rows | flex header (2-4) + index entry header (2-4) + 8 (presence) |
| directory, dense    | 8 per page it spans (at most 4 x pages in use + 64)         |
| directory, sparse   | 16 per page in use                                          |

The directory never costs memory for ids that were never used or were
used and deleted: a counter that has run to 10^9 with a few rows left
costs a few pages, and a window of live ids sliding up the number line
keeps a dense directory over the window alone. Any `uint64_t` id is valid.

Measured (clang 22, Apple M-series, 100k rows of `{string, small int}`,
`perfRegression` module `idRows`): get ~16 ns in order, ~25 ns scattered;
set ~180 ns; delete ~100 ns; 2.4 MB of heap, against 2.66 MB and 120 ns gets
for the same rows in a multimap keyed by id. One row in every 8th page
(sparse directory): ~7 MB for 100k rows, gets ~80-95 ns (a binary search
over the pages).

## Persistence

`persistOpsIdRows` (format 1) snapshots `[elementsPerRow, rows, then per row
in id order: id, elements]` and replays `INSERT [id, elements...]`,
`UPDATE [id, field, value]`, `NUM_INCR [id, field, by]`, `DELETE [id]` and
`CLEAR`. Restore refuses a width of 0, a row count the entries do not match
and ids out of order, and allocates O(rows) whatever the ids, so untrusted
bytes cannot make it reserve memory for an id range. It is in the
persistence contract (round trip, WAL replay, damage, numeral-looking
strings). The atom tables keep their own snapshot format (their rows are
rebuilt into idRows on restore).

## When to use it

- Ids from a counter or a free list, read and updated by id far more than
  searched by value: use idRows.
- Keys with an order of their own (strings, scores): a multimap or a
  sorted set; an idRows read is fastest when the ids are dense, and still
  O(log pages) when they are not.
