# varintBijou: Bijective Offset varint (bijou64)

## Overview

**varintBijou** is a faithful C port of [bijou64](https://github.com/inkandswitch/subduction/blob/main/bijou64/SPEC.md) (**BIJ**ective **O**ffset **U64**) by Brooklyn Zelenka / Ink & Switch. It is a close cousin of [varintTagged](varintTagged.md): the same tag-byte framing, big-endian payloads, `memcmp`-sortable order, and length-from-first-byte property. The single distinguishing feature is **canonicality by construction** — bijou64 applies a per-tier offset on _every_ tier, so each value has exactly one valid encoding and no "overlong" byte sequence can decode to a value that also has a shorter form.

This matters for content-addressed and canonical protocols (hashing, dedup, Merkle structures) where two byte sequences decoding to the same number — or one value having several encodings — silently produces a different hash. Where VARU64 and SQLite4's varint rely on a deletable runtime check to reject overlong encodings, bijou64 makes the offset arithmetic load-bearing: encode a value in the wrong tier and the offsets produce a _different_ value, which fails any round-trip or hash comparison immediately.

**Key Features:** canonical-by-construction (no overlong encodings), `memcmp`-sortable, O(1) length from first byte, single byte for values 0–247, full `uint64_t` range in ≤ 9 bytes, byte-exact wire compatibility with the upstream Rust/TypeScript implementations.

## Key Characteristics

| Property        | Value                                               |
| --------------- | --------------------------------------------------- |
| Implementation  | Header (.h) + Compiled (.c)                         |
| Encoding Format | `[tag]` (0–247) or `[247+tier][big-endian payload]` |
| Layout          | Big-endian (sort-compatible with `memcmp()`)        |
| Best For        | Canonical sortable keys, content-addressed storage  |
| Canonical       | Yes — by construction, not by runtime check         |
| Random Access   | Single value; length known from first byte          |

## Encoding Format

- Values **0–247** are stored as a single byte equal to the value.
- Larger values use a tag byte `247 + tier` (`0xF8`–`0xFF`) followed by `tier`
  big-endian payload bytes holding `value − OFFSET[tier]`.

Length is `tag − 246` for `tag ≥ 248`, otherwise 1.

### Offset Table

`OFFSET[1] = 248`, `OFFSET[n] = OFFSET[n-1] + 256^(n-1)`:

| Tag       | Total bytes | Offset                 | Value range (inclusive)    |
| --------- | ----------- | ---------------------- | -------------------------- |
| 0x00–0xF7 | 1           | 0                      | 0 – 247                    |
| 0xF8      | 2           | 248                    | 248 – 503                  |
| 0xF9      | 3           | 504                    | 504 – 66,039               |
| 0xFA      | 4           | 66,040                 | 66,040 – 16,843,255        |
| 0xFB      | 5           | 16,843,256             | 16,843,256 – 4,311,810,551 |
| 0xFC      | 6           | 4,311,810,552          | … – 1,103,823,438,327      |
| 0xFD      | 7           | 1,103,823,438,328      | … – 282,578,800,148,983    |
| 0xFE      | 8           | 282,578,800,148,984    | … – 72,340,172,838,076,919 |
| 0xFF      | 9           | 72,340,172,838,076,920 | … – `UINT64_MAX`           |

## Relationship to varintTagged (SQLite4 varint)

Both are the same tag-byte family. The trade-off:

|                                 | varintTagged (SQLite4)             | varintBijou (bijou64) |
| ------------------------------- | ---------------------------------- | --------------------- |
| 1-byte range                    | 0–240                              | 0–247                 |
| 2-byte range                    | 241–**2,287**                      | 248–503               |
| Offsets applied                 | tiers 1–2 only                     | **all tiers**         |
| Canonical by construction       | No (tiers 3+ admit overlong forms) | **Yes**               |
| Spare tag bytes reused for data | Yes (241–248)                      | No                    |

`varintTagged` is more compact in the common 248–2,287 band because it reuses spare tag values for payload; `varintBijou` trades that compactness for full canonicality. Choose by whether canonicality or small-value size matters more for your workload.

## API

```c
varintWidth varintBijouPut64(uint8_t *z, uint64_t x);   /* returns 1..9 bytes written */
varintWidth varintBijouGet64(const uint8_t *z, int32_t n, uint64_t *pResult);
                                                        /* returns bytes consumed, 0 on
                                                           short buffer or tier-8 overflow */
varintWidth varintBijouLen(uint64_t x);                 /* bytes x needs (1..9) */
varintWidth varintBijouGetLen(const uint8_t *z);        /* length from first byte only */
```

A conforming decoder signals only two errors (both reported as a 0 return here):
buffer-too-short, and tier-8 arithmetic overflow (`OFFSET[8] + payload > UINT64_MAX`).
There is no "non-canonical encoding" error because non-canonical encodings are
structurally impossible.

## When to Use

| Use case                                          | Choice         |
| ------------------------------------------------- | -------------- |
| Canonical sortable keys (hashing, dedup, Merkle)  | **Bijou**      |
| Smallest bytes for small sortable values          | varintTagged   |
| Maximum space efficiency, width tracked elsewhere | varintExternal |
| Legacy LEB128 / continuation-bit interop          | varintChained  |

## Integration

- Built into the core `varint` object library alongside `varintTagged`.
- Test target: `varintBijouTest` — validates against the upstream SPEC test
  vectors (byte-exact), exhaustive round-trips across tier boundaries,
  canonicality, error handling, and `memcmp` order.

## See Also

- [varintTagged](varintTagged.md) — the SQLite4 varint; closest relative, more compact for small values
- Upstream [bijou64 SPEC](https://github.com/inkandswitch/subduction/blob/main/bijou64/SPEC.md) — format definition, rationale, and prior-art comparison
