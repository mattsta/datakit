# Interning: the atom table

Interning stores each distinct value once and hands out a small integer id for it. Structures that would
each hold a copy of the same string hold the id instead (1-9 bytes as a varint), and comparing two
interned values is comparing two ids.

datakit has one interning type, `multimapAtom` (`multimapAtom.h`). A value is interned once, every holder
takes a reference, and the value is freed when the last reference goes (unless the table is permanent).
Any value a databox holds can be an atom: strings, integers of any width, floats, booleans. Equality is
`databoxCompare()`'s, so 5, 5.0 and a 128-bit 5 are one atom.

Three choices, made at creation in a `multimapAtomConfig` (a zeroed config is the default), shape a table:

| Choice                 | Options                                  | Decides                                        |
| ---------------------- | ---------------------------------------- | ---------------------------------------------- |
| `.index` (value -> id) | `HASHED` (default), `ORDERED`, `SLOTS`   | how a value's id is found, and its bytes       |
| `.store` (id -> value) | `PACKED` (default), `DIRECT`             | how an id's value is read, and its bytes       |
| `.ids`                 | `REUSE` (default), `PERMANENT`, `LOWEST` | which id a new atom gets, and whether it lasts |

## Choosing

- **The default (`multimapAtomNew()`): HASHED + PACKED.** Values packed as flex entries and a hashed index
  of a few bytes per atom: close to the values' own bytes.
- **Speed first: SLOTS + DIRECT.** Every operation in constant time: a lookup by id is an array read and a
  count change is one add, a find by value one hash and a probe. About 15-25 bytes more per atom than the
  default.
- **The fewest bytes: ORDERED + PACKED.** Ids sorted by their values: searches cost about log n
  comparisons, each read from the packed rows.
- **Ids index something sized by their range** (an array, a bitmap, a compact encoding) **and atoms come
  and go in any order: LOWEST ids.** A new atom always gets the lowest id not in use, so ids stay as dense
  as the live count allows.
- **A symbol table that only grows: PERMANENT ids.** An atom lives until Delete, Reset or Free; retain and
  release do nothing; an id is never reused, so a bare id stays valid for the table's life.

The index kinds and the stores are compared with measurements in
[multimap VARIANTS](../multimap/VARIANTS.md#multimapatom).

## Ids and references

| Behaviour   | Reuse                                                                  | Bytes added per atom            |
| ----------- | ---------------------------------------------------------------------- | ------------------------------- |
| `REUSE`     | the last release frees an atom; freeing the highest id lowers the next | none                            |
| `PERMANENT` | never: an atom lives until Delete, Reset or Free                       | none                            |
| `LOWEST`    | the last release frees an atom; new atoms take the lowest free id      | about 2 bytes per isolated hole |

- **Ids count from 0.** `multimapAtomNextId` names the id the next new atom gets.
- **Intern takes a reference.** `multimapAtomIntern` returns the existing id with its count raised by one,
  or a new atom's id with a count of one.
- **Each holder owns one reference.** Store the id in a second place: `multimapAtomRetainById`. Drop it:
  `multimapAtomReleaseById` (true when that release freed the atom).
- **Look without taking a reference:** `multimapAtomGetId` (false when absent).
- **Never keep a bare id past your last reference** in a REUSE or LOWEST table: the next new atom may get
  it.

## Example: interning tags held by many records

```c
#include "multimapAtom.h"

multimapAtom *tags = multimapAtomNew(); /* HASHED, PACKED, ids reused */

/* Each record stores the tag's id (one reference per record) */
const databox red = databoxNewBytesString("red");
const uint64_t redId = multimapAtomIntern(tags, &red); /* count 1 */
const uint64_t again = multimapAtomIntern(tags, &red); /* same id, count 2 */

/* Reading a record back */
databox name = {};
if (multimapAtomLookupById(tags, redId, &name)) {
    /* 'name' points into the table: read it, do not keep or free it */
}

/* Deleting the two records */
multimapAtomReleaseById(tags, again);       /* count 1 */
if (multimapAtomReleaseById(tags, redId)) { /* count 0: "red" is freed */
    /* redId may now be handed to the next new tag */
}

multimapAtomFree(tags);
```

## Example: dense ids for a bitmap

```c
#include "multimapAtom.h"
#include "multiroar.h"

multimapAtom *users = multimapAtomNewConfig(&(multimapAtomConfig){
    .ids = MULTIMAP_ATOM_IDS_LOWEST,
    .index = MULTIMAP_ATOM_INDEX_SLOTS,
    .store = MULTIMAP_ATOM_STORE_DIRECT});
multiroar *online = multiroarBitNew();

const databox who = databoxNewBytesString("alice");
const uint64_t id = multimapAtomIntern(users, &who);
multiroarBitSet(online, id); /* ids stay small: a dense bitmap */

/* The last release frees the id; the next new user gets the lowest free one */
multimapAtomReleaseById(users, id);

multiroarFree(online);
multimapAtomFree(users);
```

## Plain-id calls

| Call                                                                            | Does                                                                         |
| ------------------------------------------------------------------------------- | ---------------------------------------------------------------------------- |
| `multimapAtomNew()` / `multimapAtomNewConfig(&config)`                          | a default table / one with `config`'s settings (NULL for an unknown setting) |
| `multimapAtomGetIndex` / `multimapAtomGetStore` / `multimapAtomGetIds`          | its settings                                                                 |
| `multimapAtomIntern(ma, &value)`                                                | the id, taking a reference                                                   |
| `multimapAtomGetId(ma, &value, &id)`                                            | the id without a reference; false when absent                                |
| `multimapAtomLookupById(ma, id, &out)`                                          | the value (BYTES borrow from the table); false when absent                   |
| `multimapAtomRetainById(ma, id)` / `multimapAtomReleaseById(ma, id)`            | one more / one fewer reference (Release true when it freed the atom)         |
| `multimapAtomRefcountById(ma, id)`                                              | the references held (0: no such id)                                          |
| `multimapAtomDeleteById(ma, id)`                                                | delete the atom whatever its count                                           |
| `multimapAtomNextId(ma)`                                                        | the id the next new atom gets                                                |
| `multimapAtomCount(ma)` / `multimapAtomBytes(ma)` / `multimapAtomGetStats(...)` | atoms held / memory / what sharing values through the table saves            |
| `multimapAtomReset(ma)` / `multimapAtomFree(ma)`                                | every atom gone, the settings kept / free the table                          |

The reference calls (`multimapAtomInsertConvert`, `multimapAtomLookup`, ... with
`DATABOX_CONTAINER_REFERENCE_EXTERNAL` boxes) are the ones multimaps use to keep references in place of
values; they are listed in [multimap VARIANTS](../multimap/VARIANTS.md#api).

## Best practices

- Intern at the boundary (when a value enters the system), and pass ids inside it.
- Pick permanent ids for vocabularies that never shrink (field names, enum-like tags): no counting cost,
  and an id is safe to cache anywhere.
- Retain and release in the same layer that stores the id, so ownership is one rule: one stored copy, one
  reference.
- `multimapAtomBytes` reports the table's memory: measure with your values before choosing settings.
- Not thread-safe: guard a shared table with one lock (or give each thread its own).

## Persistence

`multimapAtom` is a persisted type (`persistOpsMultimapAtom`, `atomPersist.h`): a table snapshots and replays
a WAL like any container, with every id kept exactly and its settings recorded. See
[PERSIST.md](../persist/PERSIST.md).

## Performance

100k distinct 16-byte strings (perf preset, clang 22, Apple M1 Max; `perfRegression` modules
`multimapAtom` and `multimapAtomSlotsDirect`; ns per operation, medians of 5 runs; heap bytes metered at the
allocator over 20k 11-byte strings):

|                             | default (HASHED + PACKED) | fast (SLOTS + DIRECT) |
| --------------------------- | ------------------------: | --------------------: |
| heap per atom               |                  32 bytes |              52 bytes |
| intern a new value          |                       599 |                    96 |
| intern a present value      |                       305 |                    27 |
| id -> value (scattered ids) |                        30 |                   2.7 |
| value -> id (scattered)     |                       415 |                    54 |
| retain by id                |                        26 |                   1.3 |
| release by value            |                       340 |                    26 |
