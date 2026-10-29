# Interning: atom tables and pools

Interning stores each distinct value once and hands out a small integer id for it. Structures that would
each hold a copy of the same string hold the id instead (1-9 bytes as a varint), and comparing two
interned values is comparing two ids.

datakit has three interning types. They share one model: a value is interned once, every holder takes a
reference, and the value is freed when the last reference goes (unless the table is permanent).

| Type           | Header           | What it is                                                                                             |
| -------------- | ---------------- | ------------------------------------------------------------------------------------------------------ |
| `stringPool`   | `stringPool.h`   | Hash table value -> id plus an array id -> value: fastest by id                                        |
| `multimapAtom` | `multimapAtom.h` | Compact table: an index (hashed, ordered or dict) plus id rows; reusing, permanent or lowest-first ids |
| `atomPool`     | `atomPool.h`     | One interface over `stringPool` (HASH) or `multimapAtom` (TREE)                                        |

## Choosing

- **Most code: `atomPool`.** One API whichever backend you pick, so the choice can change later without
  touching callers. `atomPoolNewDefault()` is HASH.
- **Lookups by id dominate, memory is plentiful: HASH** (`stringPool`). Id -> value is an array index.
- **Many atoms, memory matters: TREE** (`multimapAtom`). Several times fewer bytes per atom; interning and
  value lookups cost a search of its index (see the numbers below).
- **Ids index something sized by their range** (an array, a bitmap, a compact encoding) **and atoms come
  and go in any order: LOWEST ids** (`atomPoolNewWithIds(type, ATOM_POOL_IDS_LOWEST)`, `multimapAtomNewConfig` with `.ids = MULTIMAP_ATOM_IDS_LOWEST`). A new
  atom always gets the lowest id not in use, so ids stay as dense as the live count allows.
- **A symbol table that only grows: permanent ids** (`ATOM_POOL_IDS_PERMANENT`, `stringPoolNewWithIds(
STRING_POOL_IDS_PERMANENT)`, `multimapAtomNewConfig` with `.ids = MULTIMAP_ATOM_IDS_PERMANENT`). An atom lives until Reset or Free; retain
  and release do nothing; an id is never reused, so a bare id stays valid for the table's life.

`multimapAtom`'s own index kinds (HASHED, the default; ORDERED; DICT) are compared in
[multimap VARIANTS](../multimap/VARIANTS.md#multimapatom).

## Ids and references

| Behaviour   | Reuse                                                             | Bytes added per atom                                   |
| ----------- | ----------------------------------------------------------------- | ------------------------------------------------------ |
| `REUSE`     | the last release frees an atom; its id comes back later           | none (HASH: 8 bytes per freed id waiting to be reused) |
| `PERMANENT` | never: an atom lives until Reset or Free                          | none                                                   |
| `LOWEST`    | the last release frees an atom; new atoms take the lowest free id | about 2 bytes per isolated hole                        |

- **Ids are 1-based in `atomPool` and `stringPool`**: 0 means "not found" or "failed". A bare
  `multimapAtom` hands out ids from 0; `atomPool`'s TREE backend adds 1 at its
  boundary so callers see the same convention for both backends.
- **Intern takes a reference.** `atomPoolIntern` (and `stringPoolIntern`) returns the existing id with
  its count raised by one, or a new atom with a count of one.
- **Each holder owns one reference.** Store the id in a second place: `atomPoolRetain`. Drop it:
  `atomPoolRelease` (true when that release freed the atom).
- **Look without taking a reference:** `atomPoolGetId` (0 when absent) and `atomPoolExists`.
- **Never keep a bare id past your last reference** in a reusing table: the next new atom may get it.

## Example: interning tags held by many records

```c
#include "atomPool.h"

atomPool *tags = atomPoolNewDefault(); /* HASH, ids reused */

/* Each record stores the tag's id (one reference per record) */
const databox red = databoxNewBytesString("red");
const uint64_t redId = atomPoolIntern(tags, &red); /* count 1 */
const uint64_t again = atomPoolIntern(tags, &red); /* same id, count 2 */

/* Reading a record back */
databox name;
if (atomPoolLookup(tags, redId, &name)) {
    /* 'name' points into the pool: read it, do not keep or free it */
}

/* Deleting the two records */
atomPoolRelease(tags, again);           /* count 1 */
if (atomPoolRelease(tags, redId)) {     /* count 0: "red" is freed */
    /* redId may now be handed to the next new tag */
}

atomPoolFree(tags);
```

## Example: dense ids for a bitmap

```c
#include "multimapAtom.h"
#include "multiroar.h"

multimapAtom *users =
    multimapAtomNewConfig(&(multimapAtomConfig){.ids = MULTIMAP_ATOM_IDS_LOWEST});
multiroar *online = multiroarBitNew();

databox who = databoxNewBytesString("alice");
multimapAtomInsertIfNewConvert(users, &who); /* 'who' is now the reference */
multiroarBitSet(online, who.data.u);         /* ids stay small: dense bitmap */

databox back;
multimapAtomLookup(users, &who, &back);

/* The last release frees the id; the next new user gets the lowest free one */
multimapAtomReleaseById(users, &who);

multiroarFree(online);
multimapAtomFree(users);
```

## atomPool calls

| Call                                                                           | Does                                                                                        |
| ------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------- |
| `atomPoolNew(type)` / `atomPoolNewDefault()` / `atomPoolNewWithIds(type, ids)` | a pool on `ATOM_POOL_HASH` or `ATOM_POOL_TREE` (default HASH), reusing ids or as `ids` says |
| `atomPoolGetType(pool)` / `atomPoolTypeName(type)` / `atomPoolGetIds(pool)`    | its backend, a backend's name, its id behaviour                                             |
| `atomPoolIntern(pool, &str)`                                                   | the id, taking a reference (0 on failure)                                                   |
| `atomPoolGetId(pool, &str)` / `atomPoolExists(pool, &str)`                     | the id without a reference (0: absent) / whether present                                    |
| `atomPoolLookup(pool, id, &out)`                                               | the string                                                                                  |
| `atomPoolRetain(pool, id)` / `atomPoolRelease(pool, id)`                       | one more / one fewer reference (Release true when it freed the atom)                        |
| `atomPoolRefcount(pool, id)`                                                   | the references held (0: no such id)                                                         |
| `atomPoolCount(pool)` / `atomPoolBytes(pool)`                                  | atoms held / memory                                                                         |
| `atomPoolReset(pool)` / `atomPoolFree(pool)`                                   | every atom gone, ids start over / free the pool                                             |

## stringPool calls

`stringPool` is the HASH backend, usable on its own. Its calls match `atomPool`'s:

| Call                                                         | Does                                                                         |
| ------------------------------------------------------------ | ---------------------------------------------------------------------------- |
| `stringPoolNew()` / `stringPoolNewWithIds(ids)`              | an empty pool (reusing ids, or `STRING_POOL_IDS_PERMANENT` / `_LOWEST`)      |
| `stringPoolIdsOf(pool)`                                      | the id behaviour it was made with                                            |
| `stringPoolIntern(pool, &str)`                               | the string's id, taking a reference (new strings start at one); 0 on failure |
| `stringPoolGetId(pool, &str)` / `stringPoolExists`           | the id without taking a reference (0: absent) / whether it is present        |
| `stringPoolLookup(pool, id, &out)`                           | the string (it points into the pool: valid until that string is freed)       |
| `stringPoolRetain(pool, id)` / `stringPoolRelease(pool, id)` | one more / one fewer reference; Release is true when it freed the string     |
| `stringPoolRefcount(pool, id)`                               | the references held (0: no such id)                                          |
| `stringPoolCount(pool)` / `stringPoolBytes(pool)`            | strings held / memory                                                        |
| `stringPoolReset(pool)` / `stringPoolFree(pool)`             | drop every string (ids start over) / free the pool                           |

## Best practices

- Intern at the boundary (when a value enters the system), and pass ids inside it.
- Pick permanent ids for vocabularies that never shrink (field names, enum-like tags): no counting cost,
  and an id is safe to cache anywhere.
- Retain and release in the same layer that stores the id, so ownership is one rule: one stored copy, one
  reference.
- `atomPoolBytes` / `stringPoolBytes` / `multimapAtomBytes` report the table's memory: measure with
  your data before choosing a backend.
- Not thread-safe: guard a shared table with one lock (or give each thread its own).

## Persistence

`stringPool`, `atomPool` and the `multimapAtom` family are persisted types (the persist registry lists
them; `persistOpsForType` finds their descriptors), so a pool snapshots and replays a WAL like any
container. See [PERSIST.md](../persist/PERSIST.md).

## Performance

`atomPool` over each backend, 100k distinct 16-byte strings (perf preset, clang 22, Apple M-series;
`perfRegression` module `multimapAtom`, rows `atomPoolHash` / `atomPoolTree`; ns per operation, medians of
5 runs):

|                              | HASH (`stringPool`) | TREE (`multimapAtom`) |
| ---------------------------- | ------------------: | --------------------: |
| heap per atom (metered)      |            80 bytes |              45 bytes |
| intern a new string          |                 350 |                   600 |
| intern a present string      |                  87 |                   430 |
| id -> string (scattered ids) |                 3.8 |                    39 |
| string -> id (`GetId`)       |                 127 |                   389 |
| retain + release by id       |                   4 |                   121 |

HASH trades 1.8x the memory for every operation being several times faster; id -> string is an array
read. TREE's per-atom bytes are close to the string itself plus a few bytes of index. Measure with your own
values (`atomPoolBytes`) when memory decides it: longer strings shrink the relative difference.
