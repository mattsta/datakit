#pragma once

#include "varint.h"
__BEGIN_DECLS

/* ====================================================================
 * varintBijou — Bijective Offset varint (bijou64)
 * ====================================================================
 * A faithful C port of bijou64 (BIJective Offset U64) by Brooklyn
 * Zelenka / Ink & Switch. See bijou/bijou64/SPEC.md for the canonical
 * specification and test vectors.
 *
 * varint model Bijective Offset Container:
 *   Type encoded inside: first byte (tag) of the varint
 *   Size: 1 byte to 9 bytes
 *   Layout: big endian (sort-compatible with memcmp())
 *   Meaning: full width known from first byte; payload is the value
 *            minus a per-tier offset.
 *
 * Encoding:
 *   - Values 0..247 are stored as a single byte equal to the value.
 *   - Larger values use tag byte 247+tier (0xF8..0xFF) followed by
 *     `tier` big-endian payload bytes holding (value - OFFSET[tier]).
 *
 * Relationship to varintTagged (SQLite4 varint): identical tag-byte
 * framing family, but bijou64 applies an offset on *every* tier, which
 * makes it canonical by construction (no overlong encodings possible).
 * varintTagged only offsets tiers 1-2 but reuses spare tag values
 * (241-248) for extra 2-byte capacity, so it is more compact for small
 * values at the cost of canonicality on tiers 3+.
 *
 * Pro: canonical-by-construction, memcmp-sortable, 1 byte for 0..247.
 * Con: not as compact as varintTagged in the 248..2287 range. */

/* First value that requires each tier (1-indexed). OFFSET[0] is unused
 * (tier 0 values are stored as the tag byte itself).
 *   OFFSET[1] = 248
 *   OFFSET[n] = OFFSET[n-1] + 256^(n-1)   for n >= 2
 * Matches bijou64 SPEC.md exactly. */
#define VARINT_BIJOU_OFFSET_1 248ULL
#define VARINT_BIJOU_OFFSET_2 504ULL
#define VARINT_BIJOU_OFFSET_3 66040ULL
#define VARINT_BIJOU_OFFSET_4 16843256ULL
#define VARINT_BIJOU_OFFSET_5 4311810552ULL
#define VARINT_BIJOU_OFFSET_6 1103823438328ULL
#define VARINT_BIJOU_OFFSET_7 282578800148984ULL
#define VARINT_BIJOU_OFFSET_8 72340172838076920ULL

/* Tag byte threshold: values below this encode as a single byte. */
#define VARINT_BIJOU_TAG_THRESHOLD 248

/* These are the maximum values for each bijou64 byte width (inclusive). */
#define VARINT_BIJOU_MAX_1 247ULL
#define VARINT_BIJOU_MAX_2 503ULL
#define VARINT_BIJOU_MAX_3 66039ULL
#define VARINT_BIJOU_MAX_4 16843255ULL
#define VARINT_BIJOU_MAX_5 4311810551ULL
#define VARINT_BIJOU_MAX_6 1103823438327ULL
#define VARINT_BIJOU_MAX_7 282578800148983ULL
#define VARINT_BIJOU_MAX_8 72340172838076919ULL
#define VARINT_BIJOU_MAX_9 UINT64_MAX

/* Encode x into z[] (must hold up to 9 bytes). Returns bytes written. */
varintWidth varintBijouPut64(uint8_t *z, uint64_t x);

/* Decode the bijou64 in the first n bytes of z[]. Writes the value into
 * *pResult and returns the number of bytes consumed. Returns 0 if there
 * are too few bytes, or if a tier-8 payload would overflow uint64_t. */
varintWidth varintBijouGet64(const uint8_t *z, int32_t n, uint64_t *pResult);

/* Number of bytes x needs as a bijou64 (1..9). */
varintWidth varintBijouLen(uint64_t x);

/* Number of bytes the bijou64 at z occupies, read from the first byte only. */
varintWidth varintBijouGetLen(const uint8_t *z);

#ifdef VARINT_BIJOU_TEST
int varintBijouTest(int argc, char *argv[]);
#endif

__END_DECLS
