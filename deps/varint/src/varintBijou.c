/*
 * varintBijou — Bijective Offset varint (bijou64)
 *
 * A faithful C port of the bijou64 encoding by Brooklyn Zelenka
 * (Ink & Switch). The reference implementation and specification live in
 * bijou/bijou64/ (Rust) and bijou/bijou64/SPEC.md. The encoding is
 * adapted from VARU64 (Aljoscha Meyer) with per-tier offsets inspired by
 * Git's pack offset encoding and SQLite4's varint.
 *
 * The wire format and offset table are reproduced under CC BY-SA 4.0; see
 * bijou/bijou64/SPEC.md for the original specification text.
 */

#include "varintBijou.h"

/* OFFSET[tier]: the first value representable only at tier `tier`.
 * Index 0 is unused. Matches the recurrence in bijou64 SPEC.md:
 *   OFFSET[1] = 248, OFFSET[n] = OFFSET[n-1] + 256^(n-1). */
static const uint64_t varintBijouOffset[9] = {
    0,
    VARINT_BIJOU_OFFSET_1,
    VARINT_BIJOU_OFFSET_2,
    VARINT_BIJOU_OFFSET_3,
    VARINT_BIJOU_OFFSET_4,
    VARINT_BIJOU_OFFSET_5,
    VARINT_BIJOU_OFFSET_6,
    VARINT_BIJOU_OFFSET_7,
    VARINT_BIJOU_OFFSET_8,
};

varintWidth varintBijouLen(uint64_t x) {
    if (x <= VARINT_BIJOU_MAX_1) {
        return 1;
    }

    /* Walk tiers until x fits below the next tier's offset. tier maxes at
     * 8 (we never index varintBijouOffset[9]). */
    varintWidth tier = 1;
    while (tier < 8 && x >= varintBijouOffset[tier + 1]) {
        tier++;
    }

    return (varintWidth)(tier + 1);
}

varintWidth varintBijouGetLen(const uint8_t *z) {
    if (z[0] < VARINT_BIJOU_TAG_THRESHOLD) {
        return 1;
    }

    /* tag 0xF8 -> tier 1 -> 2 bytes ... tag 0xFF -> tier 8 -> 9 bytes.
     * length = (tag - 247) + 1 = tag - 246. */
    return (varintWidth)(z[0] - 246);
}

varintWidth varintBijouPut64(uint8_t *z, uint64_t x) {
    if (x <= VARINT_BIJOU_MAX_1) {
        z[0] = (uint8_t)x;
        return 1;
    }

    varintWidth tier = 1;
    while (tier < 8 && x >= varintBijouOffset[tier + 1]) {
        tier++;
    }

    z[0] = (uint8_t)(247 + tier);

    /* Big-endian payload of (x - OFFSET[tier]) in `tier` bytes. */
    uint64_t payload = x - varintBijouOffset[tier];
    for (varintWidth i = 0; i < tier; i++) {
        z[1 + i] = (uint8_t)(payload >> (8 * (tier - 1 - i)));
    }

    return (varintWidth)(tier + 1);
}

varintWidth varintBijouGet64(const uint8_t *z, int32_t n, uint64_t *pResult) {
    if (n < 1) {
        return 0;
    }

    uint8_t tag = z[0];
    if (tag < VARINT_BIJOU_TAG_THRESHOLD) {
        *pResult = tag;
        return 1;
    }

    varintWidth tier = (varintWidth)(tag - 247); /* 1..8 */
    if (n < (int32_t)tier + 1) {
        return 0;
    }

    uint64_t payload = 0;
    for (varintWidth i = 0; i < tier; i++) {
        payload = (payload << 8) | (uint64_t)z[1 + i];
    }

    /* Only tier 8 can overflow (OFFSET[8] + payload > UINT64_MAX). */
    uint64_t offset = varintBijouOffset[tier];
    if (payload > UINT64_MAX - offset) {
        return 0;
    }

    *pResult = offset + payload;
    return (varintWidth)(tier + 1);
}

#ifdef VARINT_BIJOU_TEST
#include "ctest.h"
#include <string.h>

/* Test vectors transcribed verbatim from bijou/bijou64/SPEC.md and the
 * reference Rust test suite (bijou/bijou64/src/lib.rs). Byte-exact match
 * here proves wire compatibility with the upstream implementation. */
typedef struct bijouVector {
    uint64_t value;
    uint8_t bytes[9];
    varintWidth len;
} bijouVector;

static const bijouVector vectors[] = {
    /* Tier 0: single byte */
    {0, {0x00}, 1},
    {1, {0x01}, 1},
    {42, {0x2A}, 1},
    {97, {0x61}, 1},
    {127, {0x7F}, 1},
    {128, {0x80}, 1},
    {150, {0x96}, 1},
    {247, {0xF7}, 1},
    /* Tier 1: tag 0xF8 + 1 byte */
    {248, {0xF8, 0x00}, 2},
    {249, {0xF8, 0x01}, 2},
    {300, {0xF8, 0x34}, 2},
    {503, {0xF8, 0xFF}, 2},
    /* Tier 2: tag 0xF9 + 2 bytes */
    {504, {0xF9, 0x00, 0x00}, 3},
    {1000, {0xF9, 0x01, 0xF0}, 3},
    {65535, {0xF9, 0xFE, 0x07}, 3},
    {66039, {0xF9, 0xFF, 0xFF}, 3},
    /* Tier 3: tag 0xFA + 3 bytes */
    {66040, {0xFA, 0x00, 0x00, 0x00}, 4},
    {67000, {0xFA, 0x00, 0x03, 0xC0}, 4},
    {16843255, {0xFA, 0xFF, 0xFF, 0xFF}, 4},
    /* Tier 4: tag 0xFB + 4 bytes */
    {16843256, {0xFB, 0x00, 0x00, 0x00, 0x00}, 5},
    {4311810551ULL, {0xFB, 0xFF, 0xFF, 0xFF, 0xFF}, 5},
    /* Tier 5: tag 0xFC + 5 bytes */
    {4311810552ULL, {0xFC, 0x00, 0x00, 0x00, 0x00, 0x00}, 6},
    /* Tier 8: tag 0xFF + 8 bytes */
    {72340172838076920ULL,
     {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
     9},
    {UINT64_MAX, {0xFF, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0x07}, 9},
};

int varintBijouTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    TEST("bijou64 spec test vectors: encode matches expected bytes");
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        const bijouVector *v = &vectors[i];
        uint8_t buf[9] = {0};
        varintWidth w = varintBijouPut64(buf, v->value);
        if (w != v->len) {
            ERR("value %" PRIu64 ": encoded length %d, expected %d", v->value,
                (int)w, v->len);
        } else if (memcmp(buf, v->bytes, (size_t)v->len) != 0) {
            ERR("value %" PRIu64 ": encoded bytes mismatch", v->value);
        }
    }

    TEST("bijou64 spec test vectors: decode matches expected value");
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        const bijouVector *v = &vectors[i];
        uint64_t out = 0;
        varintWidth w = varintBijouGet64(v->bytes, v->len, &out);
        if (w != v->len) {
            ERR("value %" PRIu64 ": decoded length %d, expected %d", v->value,
                (int)w, v->len);
        } else if (out != v->value) {
            ERR("bytes for %" PRIu64 ": decoded %" PRIu64, v->value, out);
        }
    }

    TEST("bijou64 GetLen agrees with Put64 width");
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        const bijouVector *v = &vectors[i];
        if (varintBijouGetLen(v->bytes) != v->len) {
            ERR("value %" PRIu64 ": GetLen %d != %d", v->value,
                (int)varintBijouGetLen(v->bytes), v->len);
        }
        if (varintBijouLen(v->value) != v->len) {
            ERR("value %" PRIu64 ": Len %d != %d", v->value,
                (int)varintBijouLen(v->value), v->len);
        }
    }

    TEST("bijou64 exhaustive round-trip across tier boundaries");
    {
        /* All tier-0/1/2 values plus a sweep through the tier boundaries. */
        const uint64_t boundaries[] = {
            VARINT_BIJOU_OFFSET_1, VARINT_BIJOU_OFFSET_2, VARINT_BIJOU_OFFSET_3,
            VARINT_BIJOU_OFFSET_4, VARINT_BIJOU_OFFSET_5, VARINT_BIJOU_OFFSET_6,
            VARINT_BIJOU_OFFSET_7, VARINT_BIJOU_OFFSET_8};
        for (size_t i = 0; i < 70000; i++) {
            uint8_t enc[9];
            uint64_t out = 0;
            varintWidth w = varintBijouPut64(enc, i);
            varintWidth r = varintBijouGet64(enc, 9, &out);
            if (r != w || out != i) {
                ERR("round-trip failed for %zu (w=%d r=%d out=%" PRIu64 ")", i,
                    (int)w, (int)r, out);
                break;
            }
        }
        for (size_t b = 0; b < sizeof(boundaries) / sizeof(boundaries[0]);
             b++) {
            for (int64_t d = -2; d <= 2; d++) {
                uint64_t x = boundaries[b] + (uint64_t)d;
                uint8_t enc[9];
                uint64_t out = 0;
                varintWidth w = varintBijouPut64(enc, x);
                varintWidth r = varintBijouGet64(enc, 9, &out);
                if (r != w || out != x) {
                    ERR("boundary round-trip failed for %" PRIu64, x);
                }
            }
        }
    }

    TEST("bijou64 canonicality: no overlong encoding decodes to a shorter "
         "value");
    {
        /* A forged wider-tier encoding of a tier-1 value must NOT decode
         * back to that value (this is the property SQLite4's varint lacks
         * on tiers 3+). [0xF9,0x00,0x00] decodes to OFFSET[2]=504, never 0. */
        uint8_t forged[] = {0xF9, 0x00, 0x00};
        uint64_t out = 0;
        varintBijouGet64(forged, 3, &out);
        if (out != VARINT_BIJOU_OFFSET_2) {
            ERR("forged [F9 00 00] decoded to %" PRIu64 ", expected %llu", out,
                (unsigned long long)VARINT_BIJOU_OFFSET_2);
        }
    }

    TEST("bijou64 error handling: short buffers and tier-8 overflow");
    {
        uint64_t out = 0;
        if (varintBijouGet64((const uint8_t *)"", 0, &out) != 0) {
            ERRR("empty buffer should return 0");
        }
        uint8_t shortBuf[] = {0xF9, 0x00}; /* needs 3 bytes, only 2 */
        if (varintBijouGet64(shortBuf, 2, &out) != 0) {
            ERRR("truncated tier-2 should return 0");
        }
        uint8_t overflow[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                              0xFF, 0xFF, 0xFF, 0xFF};
        if (varintBijouGet64(overflow, 9, &out) != 0) {
            ERRR("tier-8 overflow should return 0");
        }
    }

    TEST("bijou64 memcmp order equals numeric order");
    {
        uint8_t a[9] = {0}, b[9] = {0};
        const uint64_t probes[] = {0,        1,          247,           248,
                                   503,      504,        65535,         66040,
                                   1u << 20, 1ull << 40, UINT64_MAX - 1};
        size_t np = sizeof(probes) / sizeof(probes[0]);
        for (size_t i = 0; i < np; i++) {
            for (size_t j = 0; j < np; j++) {
                memset(a, 0, sizeof(a));
                memset(b, 0, sizeof(b));
                varintBijouPut64(a, probes[i]);
                varintBijouPut64(b, probes[j]);
                int cmp = memcmp(a, b, sizeof(a));
                int want = (probes[i] < probes[j])   ? -1
                           : (probes[i] > probes[j]) ? 1
                                                     : 0;
                int got = (cmp < 0) ? -1 : (cmp > 0) ? 1 : 0;
                if (got != want) {
                    ERR("memcmp order wrong for %" PRIu64 " vs %" PRIu64,
                        probes[i], probes[j]);
                }
            }
        }
    }

    TEST_FINAL_RESULT;
}
#endif
