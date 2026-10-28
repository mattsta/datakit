# ULID: time-ordered identifiers

**Header**: `ulid.h` **Source**: `ulid.c`

A ULID is an identifier that sorts by the time it was made. datakit's is 128 bits (a 64-bit nanosecond
timestamp above 64 random bits), written as 25 Base36 characters (`0-9A-Z`, decoded case-insensitively).
Compact variants pack a coarser timestamp and a smaller low field into 64 or 32 bits.

## Generators

All state lives in a caller-owned `ulidGen`: its random generator, its clock, and what it emitted last.
Independent generators are independent; one generator is single-threaded.

```c
#include "ulid.h"

ulidGen gen;
ulidGenInit(&gen, ULID_CLOCK_REALTIME); /* or zero-init: self-seeds, REALTIME */

ulid_t id;
ulidGenerate(&gen, &id);

char text[ULID_ENCODED_LENGTH + 1];
ulidEncode(&id, text, sizeof(text));    /* 25 chars + NUL */

ulid_t back;
if (ulidDecode(text, &back)) {          /* false: not 25 Base36 chars, or past 2^128 - 1 */
    const uint64_t ns = ulidGetTimestampNs(&back); /* Unix ns */
}
```

- **Ordering**: ids from one generator strictly increase, also when the clock repeats an instant or steps
  backwards (a watermark keeps emission ahead of every earlier id). `ulidCompare`, `memcmp` of the bytes,
  and `strcmp` of the Base36 text all agree.
- **Clock**: `ULID_CLOCK_REALTIME` reads the wall clock for each id; `ULID_CLOCK_MONOTONIC_OFFSET` reads the
  monotonic clock plus an offset taken at init (immune to wall-clock steps, drifts slowly over long
  uptimes).
- Utilities: `ulidIsZero`, `ulidClear`, `ulidCopy`, `ulidGetRandom`, `ulidGenerateAndEncode`, and
  `ulidGenNew` / `ulidGenFree` for a heap generator.

## Compact variants

Every 64-bit variant is a time field above a low field. The time counts units from an epoch (1970 unless
named); the range is how long that field lasts from its epoch.

| Variant       | Layout                                             | Precision | Lasts               | Low field         |
| ------------- | -------------------------------------------------- | --------- | ------------------- | ----------------- |
| `EPOCH2020`   | 48-bit ms since 2020 + 16-bit random               | ms        | ~8,900 years        | random            |
| `EPOCH2024`   | 48-bit ms since 2024 + 16-bit random               | ms        | ~8,900 years        | random            |
| `EPOCHCUSTOM` | 48-bit ms since `config.customEpochNs` + 16 random | ms        | ~8,900 years        | random            |
| `NS`          | 62-bit ns + 2-bit random                           | ns        | to 2116             | random (2 bits)   |
| `US`          | 52-bit us + 12-bit random                          | us        | to 2112             | random            |
| `MS`          | 42-bit ms + 22-bit random                          | ms        | to 2109             | random            |
| `MSCOUNT`     | 42-bit ms + 22-bit counter from 0 each ms          | ms        | to 2109             | counter           |
| `HYBRID`      | 44-bit ms + 20-bit ns within the ms                | ns        | to 2527             | exact sub-ms ns   |
| `SNOWFLAKE`   | 41-bit ms since 2020 + 10-bit machine + 13-bit seq | ms        | to 2089             | machine, sequence |
| `32MS`        | 26-bit ms + 6-bit random (32-bit)                  | ms        | an 18.6-hour window | random            |
| `32S`         | 22-bit s + 10-bit random (32-bit)                  | s         | a 48.5-day window   | random            |

`ulid64GetVariantName`, `ulid64GetVariantDescription`, `ulid64GetVariantRangeYears`,
`ulid64GetVariantRandomBits`, `ulid64GetVariantPrecision` and `ulid64GetMaxTimestamp` report each layout;
`ulid64ValidateTimestamp(ns, type)` says whether a variant holds a time.

```c
ulid64_t id;
if (!ulid64Generate(&gen, &id, ULID_VARIANT_MS)) {
    /* not a 64-bit variant, or the clock is outside its range */
}
const uint64_t ns = ulid64GetTimestampNs(&id, ULID_VARIANT_MS);   /* to the ms */

ulidVariantConfig cfg;
ulidVariantConfigInit(&cfg, ULID_VARIANT_SNOWFLAKE);
cfg.machineId = 17;                                    /* 0..1023, distinct per generator */
if (ulidVariantConfigValidate(&cfg)) {
    ulid64GenerateWithConfig(&gen, &id, &cfg);
}
```

Rules every 64-bit variant follows:

- **Ids of one variant from one generator strictly increase.** Within one unit of the time field, a random
  or counted id is the previous id plus one (ULID's monotonic rule). When the low field is spent, the id
  borrows the next unit: `MS` takes at most 2^22 ids a millisecond from one generator, minus the random
  start.
- **A clock outside the range makes no id**: `ulid64Generate` returns false and leaves the id zero (before
  the variant's epoch, or past what its time field holds). Ids never wrap.
- **The low field is read with `ulid64GetRandom`**; `ulid64GetCounter` (`MSCOUNT`), `ulid64GetNsDelta`
  (`HYBRID`), `ulid64GetSnowflakeMachineId` and `ulid64GetSnowflakeSequence` name theirs.
- **`EPOCHCUSTOM` reads back as an offset**: `ulid64GetTimestampNs` gives the time since the configured
  epoch; add the epoch.

The 32-bit variants keep only a window of time: their time field is the Unix time modulo the window, so ids
sort only within one window (`ulid32GetTimestampNs` is the time within it). Within one unit an id follows
the previous one while its low field has room.

The dual-timestamp variants (`DUALNS`, `DUALNS_INTERLEAVED`) are 128-bit: two nanosecond timestamps,
ordered by the first then the second, or in Z-order (bit-interleaved), through `ulidGenerateDualNs*`,
`ulidFromDualNs*` and `ulidGet{First,Second}TimestampNs*`.

## Choosing

- **Unique across machines, sortable**: the 128-bit ULID (64 random bits).
- **64 bits, sortable for a century, ms is enough**: `MS` (22 random bits) or `MSCOUNT` (no randomness,
  dense within a ms; unique only per generator).
- **Fleet ids without coordination beyond a machine number**: `SNOWFLAKE` (unique when every generator has
  its own `machineId`).
- **Exact ns ordering from one generator**: `HYBRID` (ns precision, no random bits) or `NS`.
- **Smallest ids for short-lived things** (sessions, cache entries): the 32-bit variants, accepting their
  window.

## Encoding

`ulidEncode` / `ulidDecode` pick the fastest implementation available (scalar, SWAR, SSE2, AVX2, NEON);
`ulidSetEncodeImpl` forces one and `ulidIsImplAvailable` / `ulidGetImplName` report them. All give the
same bytes. `ulid64Encode` / `ulid64Decode` use 13 characters, `ulid32Encode` / `ulid32Decode` 7; a decode
refuses a wrong length, a non-Base36 character, or a value past the type.
