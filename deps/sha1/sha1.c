/*	$NetBSD: sha1.c,v 1.6 2009/11/06 20:31:18 joerg Exp $	*/
/*	$OpenBSD: sha1.c,v 1.9 1997/07/23 21:12:32 kstailey Exp $	*/

/*
 * SHA-1 in C
 * By Steve Reid <steve@edmweb.com>
 * 100% Public Domain
 *
 * Test Vectors (from FIPS PUB 180-1)
 * "abc"
 *   A9993E36 4706816A BA3E2571 7850C26C 9CD0D89D
 * "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
 *   84983E44 1C3BD26E BAAE4AA1 F95129E5 E54670F1
 * A million repetitions of "a"
 *   34AA973C D4C4DAA4 F61EEB2B DBAD2731 6534016F
 */

#define SHA1HANDSOFF /* Copies data before messing with it. */

#include "sha1.h"
#include <sys/cdefs.h>

#if defined(_KERNEL) || defined(_STANDALONE)
__KERNEL_RCSID(0, "$NetBSD: sha1.c,v 1.6 2009/11/06 20:31:18 joerg Exp $");

#include <lib/libkern/libkern.h>

#else

#if defined(LIBC_SCCS) && !defined(lint)
__RCSID("$NetBSD: sha1.c,v 1.6 2009/11/06 20:31:18 joerg Exp $");
#endif /* LIBC_SCCS and not lint */

// #include "namespace.h"
#include <assert.h>
#include <string.h>

#endif

#include <sys/types.h>
// #include <sys/sha1.h>

#define _DIAGASSERT(x) (void)0

#if HAVE_NBTOOL_CONFIG_H
#include "nbtool_config.h"
#endif

#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

/*
 * blk0() and blk() perform the initial expand.
 * I got the idea of expanding during the round function from SSLeay
 */
#if BYTE_ORDER == LITTLE_ENDIAN
#define blk0(i)                                                                \
    (block->l[i] = (rol(block->l[i], 24) & 0xFF00FF00) |                       \
                   (rol(block->l[i], 8) & 0x00FF00FF))
#else
#define blk0(i) block->l[i]
#endif
#define blk(i)                                                                 \
    (block->l[i & 15] = rol(block->l[(i + 13) & 15] ^ block->l[(i + 8) & 15] ^ \
                                block->l[(i + 2) & 15] ^ block->l[i & 15],     \
                            1))

/*
 * (R0+R1), R2, R3, R4 are the different operations (rounds) used in SHA1
 */
#define R0(v, w, x, y, z, i)                                                   \
    z += ((w & (x ^ y)) ^ y) + blk0(i) + 0x5A827999 + rol(v, 5);               \
    w = rol(w, 30);
#define R1(v, w, x, y, z, i)                                                   \
    z += ((w & (x ^ y)) ^ y) + blk(i) + 0x5A827999 + rol(v, 5);                \
    w = rol(w, 30);
#define R2(v, w, x, y, z, i)                                                   \
    z += (w ^ x ^ y) + blk(i) + 0x6ED9EBA1 + rol(v, 5);                        \
    w = rol(w, 30);
#define R3(v, w, x, y, z, i)                                                   \
    z += (((w | x) & y) | (w & x)) + blk(i) + 0x8F1BBCDC + rol(v, 5);          \
    w = rol(w, 30);
#define R4(v, w, x, y, z, i)                                                   \
    z += (w ^ x ^ y) + blk(i) + 0xCA62C1D6 + rol(v, 5);                        \
    w = rol(w, 30);

#if !defined(_KERNEL) && !defined(_STANDALONE)
#if defined(__weak_alias)
__weak_alias(SHA1Transform, _SHA1Transform) __weak_alias(SHA1Init, _SHA1Init)
    __weak_alias(SHA1Update, _SHA1Update) __weak_alias(SHA1Final, _SHA1Final)
#endif
#endif

        typedef union {
    uint8_t c[64];
    uint32_t l[16];
} CHAR64LONG16;

/* old sparc64 gcc could not compile this */
#undef SPARC64_GCC_WORKAROUND
#if defined(__sparc64__) && defined(__GNUC__) && __GNUC__ < 3
#define SPARC64_GCC_WORKAROUND
#endif

#ifdef SPARC64_GCC_WORKAROUND
void do_R01(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e,
            CHAR64LONG16 *);
void do_R2(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e,
           CHAR64LONG16 *);
void do_R3(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e,
           CHAR64LONG16 *);
void do_R4(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e,
           CHAR64LONG16 *);

#define nR0(v, w, x, y, z, i) R0(*v, *w, *x, *y, *z, i)
#define nR1(v, w, x, y, z, i) R1(*v, *w, *x, *y, *z, i)
#define nR2(v, w, x, y, z, i) R2(*v, *w, *x, *y, *z, i)
#define nR3(v, w, x, y, z, i) R3(*v, *w, *x, *y, *z, i)
#define nR4(v, w, x, y, z, i) R4(*v, *w, *x, *y, *z, i)

void do_R01(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e,
            CHAR64LONG16 *block) {
    nR0(a, b, c, d, e, 0);
    nR0(e, a, b, c, d, 1);
    nR0(d, e, a, b, c, 2);
    nR0(c, d, e, a, b, 3);
    nR0(b, c, d, e, a, 4);
    nR0(a, b, c, d, e, 5);
    nR0(e, a, b, c, d, 6);
    nR0(d, e, a, b, c, 7);
    nR0(c, d, e, a, b, 8);
    nR0(b, c, d, e, a, 9);
    nR0(a, b, c, d, e, 10);
    nR0(e, a, b, c, d, 11);
    nR0(d, e, a, b, c, 12);
    nR0(c, d, e, a, b, 13);
    nR0(b, c, d, e, a, 14);
    nR0(a, b, c, d, e, 15);
    nR1(e, a, b, c, d, 16);
    nR1(d, e, a, b, c, 17);
    nR1(c, d, e, a, b, 18);
    nR1(b, c, d, e, a, 19);
}

void do_R2(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e,
           CHAR64LONG16 *block) {
    nR2(a, b, c, d, e, 20);
    nR2(e, a, b, c, d, 21);
    nR2(d, e, a, b, c, 22);
    nR2(c, d, e, a, b, 23);
    nR2(b, c, d, e, a, 24);
    nR2(a, b, c, d, e, 25);
    nR2(e, a, b, c, d, 26);
    nR2(d, e, a, b, c, 27);
    nR2(c, d, e, a, b, 28);
    nR2(b, c, d, e, a, 29);
    nR2(a, b, c, d, e, 30);
    nR2(e, a, b, c, d, 31);
    nR2(d, e, a, b, c, 32);
    nR2(c, d, e, a, b, 33);
    nR2(b, c, d, e, a, 34);
    nR2(a, b, c, d, e, 35);
    nR2(e, a, b, c, d, 36);
    nR2(d, e, a, b, c, 37);
    nR2(c, d, e, a, b, 38);
    nR2(b, c, d, e, a, 39);
}

void do_R3(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e,
           CHAR64LONG16 *block) {
    nR3(a, b, c, d, e, 40);
    nR3(e, a, b, c, d, 41);
    nR3(d, e, a, b, c, 42);
    nR3(c, d, e, a, b, 43);
    nR3(b, c, d, e, a, 44);
    nR3(a, b, c, d, e, 45);
    nR3(e, a, b, c, d, 46);
    nR3(d, e, a, b, c, 47);
    nR3(c, d, e, a, b, 48);
    nR3(b, c, d, e, a, 49);
    nR3(a, b, c, d, e, 50);
    nR3(e, a, b, c, d, 51);
    nR3(d, e, a, b, c, 52);
    nR3(c, d, e, a, b, 53);
    nR3(b, c, d, e, a, 54);
    nR3(a, b, c, d, e, 55);
    nR3(e, a, b, c, d, 56);
    nR3(d, e, a, b, c, 57);
    nR3(c, d, e, a, b, 58);
    nR3(b, c, d, e, a, 59);
}

void do_R4(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e,
           CHAR64LONG16 *block) {
    nR4(a, b, c, d, e, 60);
    nR4(e, a, b, c, d, 61);
    nR4(d, e, a, b, c, 62);
    nR4(c, d, e, a, b, 63);
    nR4(b, c, d, e, a, 64);
    nR4(a, b, c, d, e, 65);
    nR4(e, a, b, c, d, 66);
    nR4(d, e, a, b, c, 67);
    nR4(c, d, e, a, b, 68);
    nR4(b, c, d, e, a, 69);
    nR4(a, b, c, d, e, 70);
    nR4(e, a, b, c, d, 71);
    nR4(d, e, a, b, c, 72);
    nR4(c, d, e, a, b, 73);
    nR4(b, c, d, e, a, 74);
    nR4(a, b, c, d, e, 75);
    nR4(e, a, b, c, d, 76);
    nR4(d, e, a, b, c, 77);
    nR4(c, d, e, a, b, 78);
    nR4(b, c, d, e, a, 79);
}
#endif

/*
 * Hash a single 512-bit block. This is the core of the algorithm.
 */
void SHA1Transform(uint32_t state[5], const uint8_t buffer[64]) {
    uint32_t a, b, c, d, e;
    CHAR64LONG16 *block;

#ifdef SHA1HANDSOFF
    CHAR64LONG16 workspace;
#endif

    _DIAGASSERT(buffer != 0);
    _DIAGASSERT(state != 0);

#ifdef SHA1HANDSOFF
    block = &workspace;
    (void)memcpy(block, buffer, 64);
#else
    block = (CHAR64LONG16 *)(void *)buffer;
#endif

    /* Copy context->state[] to working vars */
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

#ifdef SPARC64_GCC_WORKAROUND
    do_R01(&a, &b, &c, &d, &e, block);
    do_R2(&a, &b, &c, &d, &e, block);
    do_R3(&a, &b, &c, &d, &e, block);
    do_R4(&a, &b, &c, &d, &e, block);
#else
    /* 4 rounds of 20 operations each. Loop unrolled. */
    R0(a, b, c, d, e, 0);
    R0(e, a, b, c, d, 1);
    R0(d, e, a, b, c, 2);
    R0(c, d, e, a, b, 3);
    R0(b, c, d, e, a, 4);
    R0(a, b, c, d, e, 5);
    R0(e, a, b, c, d, 6);
    R0(d, e, a, b, c, 7);
    R0(c, d, e, a, b, 8);
    R0(b, c, d, e, a, 9);
    R0(a, b, c, d, e, 10);
    R0(e, a, b, c, d, 11);
    R0(d, e, a, b, c, 12);
    R0(c, d, e, a, b, 13);
    R0(b, c, d, e, a, 14);
    R0(a, b, c, d, e, 15);
    R1(e, a, b, c, d, 16);
    R1(d, e, a, b, c, 17);
    R1(c, d, e, a, b, 18);
    R1(b, c, d, e, a, 19);
    R2(a, b, c, d, e, 20);
    R2(e, a, b, c, d, 21);
    R2(d, e, a, b, c, 22);
    R2(c, d, e, a, b, 23);
    R2(b, c, d, e, a, 24);
    R2(a, b, c, d, e, 25);
    R2(e, a, b, c, d, 26);
    R2(d, e, a, b, c, 27);
    R2(c, d, e, a, b, 28);
    R2(b, c, d, e, a, 29);
    R2(a, b, c, d, e, 30);
    R2(e, a, b, c, d, 31);
    R2(d, e, a, b, c, 32);
    R2(c, d, e, a, b, 33);
    R2(b, c, d, e, a, 34);
    R2(a, b, c, d, e, 35);
    R2(e, a, b, c, d, 36);
    R2(d, e, a, b, c, 37);
    R2(c, d, e, a, b, 38);
    R2(b, c, d, e, a, 39);
    R3(a, b, c, d, e, 40);
    R3(e, a, b, c, d, 41);
    R3(d, e, a, b, c, 42);
    R3(c, d, e, a, b, 43);
    R3(b, c, d, e, a, 44);
    R3(a, b, c, d, e, 45);
    R3(e, a, b, c, d, 46);
    R3(d, e, a, b, c, 47);
    R3(c, d, e, a, b, 48);
    R3(b, c, d, e, a, 49);
    R3(a, b, c, d, e, 50);
    R3(e, a, b, c, d, 51);
    R3(d, e, a, b, c, 52);
    R3(c, d, e, a, b, 53);
    R3(b, c, d, e, a, 54);
    R3(a, b, c, d, e, 55);
    R3(e, a, b, c, d, 56);
    R3(d, e, a, b, c, 57);
    R3(c, d, e, a, b, 58);
    R3(b, c, d, e, a, 59);
    R4(a, b, c, d, e, 60);
    R4(e, a, b, c, d, 61);
    R4(d, e, a, b, c, 62);
    R4(c, d, e, a, b, 63);
    R4(b, c, d, e, a, 64);
    R4(a, b, c, d, e, 65);
    R4(e, a, b, c, d, 66);
    R4(d, e, a, b, c, 67);
    R4(c, d, e, a, b, 68);
    R4(b, c, d, e, a, 69);
    R4(a, b, c, d, e, 70);
    R4(e, a, b, c, d, 71);
    R4(d, e, a, b, c, 72);
    R4(c, d, e, a, b, 73);
    R4(b, c, d, e, a, 74);
    R4(a, b, c, d, e, 75);
    R4(e, a, b, c, d, 76);
    R4(d, e, a, b, c, 77);
    R4(c, d, e, a, b, 78);
    R4(b, c, d, e, a, 79);
#endif

    /* Add the working vars back into context.state[] */
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

/*
 * SHA1Init - Initialize new context
 */
void SHA1Init(SHA1_CTX *context) {
    _DIAGASSERT(context != 0);

    /* SHA1 initialization constants */
    context->state[0] = 0x67452301;
    context->state[1] = 0xEFCDAB89;
    context->state[2] = 0x98BADCFE;
    context->state[3] = 0x10325476;
    context->state[4] = 0xC3D2E1F0;
    context->count[0] = context->count[1] = 0;
}

/*
 * Run your data through this.
 */
void SHA1Update(SHA1_CTX *context, const uint8_t *data, uint32_t len) {
    uint32_t i, j;

    _DIAGASSERT(context != 0);
    _DIAGASSERT(data != 0);

    /* count is the 64-bit message length in bits: len << 3 adds the low
     * 29 bits of len (plus a carry), len >> 29 the high 3. */
    j = context->count[0];
    if ((context->count[0] += len << 3) < j) {
        context->count[1]++;
    }
    context->count[1] += len >> 29;

    /* j bytes are buffered (j <= 63); len > 63 - j means the input
     * completes a block, without the j + len overflow near UINT32_MAX. */
    j = (j >> 3) & 63;
    if (len > 63 - j) {
        (void)memcpy(&context->buffer[j], data, (i = 64 - j));
        SHA1Transform(context->state, context->buffer);
        for (; i + 63 < len; i += 64) {
            SHA1Transform(context->state, &data[i]);
        }
        j = 0;
    } else {
        i = 0;
    }
    (void)memcpy(&context->buffer[j], &data[i], len - i);
}

/*
 * Add padding and return the message digest.
 */
void SHA1Final(uint8_t digest[20], SHA1_CTX *context) {
    uint32_t i;
    uint8_t finalcount[8];
    uint32_t used;

    _DIAGASSERT(digest != 0);
    _DIAGASSERT(context != 0);

    for (i = 0; i < 8; i++) {
        finalcount[i] = (uint8_t)((context->count[(i >= 4 ? 0 : 1)] >>
                                   ((3 - (i & 3)) * 8)) &
                                  255); /* Endian independent */
    }

    /* Pad in place: one 0x80 byte, zeros up to byte 56 of a block, then
     * the 64-bit big-endian message length in bits. When fewer than 9
     * bytes of the current block remain, the padding spills into one
     * more block. Every SHA1Transform() here reads the full 64-byte
     * context->buffer. */
    used = (context->count[0] >> 3) & 63;
    context->buffer[used++] = 0x80;
    if (used > 56) {
        (void)memset(&context->buffer[used], 0, 64 - used);
        SHA1Transform(context->state, context->buffer);
        used = 0;
    }
    (void)memset(&context->buffer[used], 0, 56 - used);
    (void)memcpy(&context->buffer[56], finalcount, 8);
    SHA1Transform(context->state, context->buffer);

    if (digest) {
        for (i = 0; i < 20; i++) {
            digest[i] =
                (uint8_t)((context->state[i >> 2] >> ((3 - (i & 3)) * 8)) &
                          255);
        }
    }
}

#ifdef DATAKIT_TEST
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/* Known-answer vectors. The empty-input digest and the FIPS PUB 180-1
 * vectors are the published values. The prefix vectors were computed
 * with an independent SHA-1 (shasum) over prefixes of sha1TestPattern
 * and pin every padding boundary: 55 bytes is the longest message whose
 * padding fits in its own block, 56 and 57 spill into a second block,
 * 63/64/65 straddle one full block, 119/120 repeat the 55/56 boundary
 * one block later and 128 is exactly two blocks. */
static const char sha1TestPattern[] =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ+/"
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ+/";

typedef struct {
    const char *data; /* NULL means a prefix of sha1TestPattern */
    uint32_t len;
    const char *hex;
} sha1TestVector;

static const sha1TestVector sha1TestVectors[] = {
    {"", 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
    {"abc", 3, "a9993e364706816aba3e25717850c26c9cd0d89d"},
    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
     "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
    {NULL, 55, "9340477258b95b9c56bcd313f092ae5dbae8f29b"},
    {NULL, 56, "c36256841f28695b1a551b7f3d20cc45f1b613e4"},
    {NULL, 57, "e10336cca44542f78d6288799d96341f83a1d38f"},
    {NULL, 63, "4df67b96b98741c1a9ada3720ed78368f015b4a9"},
    {NULL, 64, "c780a9e38419c3104226137fe158098283246434"},
    {NULL, 65, "fd74a03da2a25f32cd6eb912fd3b6f6595d2ce34"},
    {NULL, 119, "ac5913a5a6b46284a037e15282b0bf153aafe7da"},
    {NULL, 120, "95bf6b00b64e02c13c42f471a979de4c05a4844b"},
    {NULL, 128, "c7f167dca466d8f2d098ce06a2b8942603b20071"},
};

/* Update chunk sizes. The digest must not depend on how the input is
 * split across SHA1Update() calls. */
static const uint32_t sha1TestChunks[] = {1, 3, 17, 55, 63, 64, 65, 1000};

static void sha1TestHex(const uint8_t digest[SHA1_DIGEST_LENGTH],
                        char hex[SHA1_DIGEST_STRING_LENGTH]) {
    static const char digits[] = "0123456789abcdef";
    for (uint32_t i = 0; i < SHA1_DIGEST_LENGTH; i++) {
        hex[i * 2] = digits[digest[i] >> 4];
        hex[i * 2 + 1] = digits[digest[i] & 15];
    }
    hex[SHA1_DIGEST_STRING_LENGTH - 1] = '\0';
}

/* Hash len bytes in chunk-sized updates. When period is non-zero the
 * input is data repeated (data holds at least period + chunk bytes of
 * the repeating sequence); otherwise data holds all len bytes. */
static int sha1TestCheck(const char *what, const uint8_t *data, uint32_t period,
                         uint32_t len, uint32_t chunk, const char *want) {
    SHA1_CTX ctx;
    uint8_t digest[SHA1_DIGEST_LENGTH];
    char got[SHA1_DIGEST_STRING_LENGTH];

    SHA1Init(&ctx);
    for (uint32_t off = 0; off < len; off += chunk) {
        const uint32_t n = len - off < chunk ? len - off : chunk;
        const uint32_t at = period ? off % period : off;
        SHA1Update(&ctx, data + at, n);
    }
    SHA1Final(digest, &ctx);
    sha1TestHex(digest, got);

    if (strcmp(got, want) != 0) {
        printf("sha1: %s len=%" PRIu32 " chunk=%" PRIu32 ": got %s, want %s\n",
               what, len, chunk, got, want);
        return 1;
    }
    return 0;
}

/* Run the known-answer vectors; returns the number of failures. */
int sha1Test(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int failures = 0;

    for (uint32_t v = 0;
         v < sizeof(sha1TestVectors) / sizeof(sha1TestVectors[0]); v++) {
        const sha1TestVector *tv = &sha1TestVectors[v];
        const char *data = tv->data ? tv->data : sha1TestPattern;
        for (uint32_t c = 0;
             c < sizeof(sha1TestChunks) / sizeof(sha1TestChunks[0]); c++) {
            failures += sha1TestCheck(tv->data ? "vector" : "pattern",
                                      (const uint8_t *)data, 0, tv->len,
                                      sha1TestChunks[c], tv->hex);
        }
    }

    /* FIPS PUB 180-1: one million repetitions of "a". */
    static uint8_t as[2048];
    memset(as, 'a', sizeof(as));
    for (uint32_t c = 0; c < sizeof(sha1TestChunks) / sizeof(sha1TestChunks[0]);
         c++) {
        failures +=
            sha1TestCheck("million-a", as, 1, 1000000, sha1TestChunks[c],
                          "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
    }

    /* One SHA1Update of 2^29 bytes: len << 3 wraps to 0 in 32 bits, so
     * the whole message length must arrive through len >> 29 into the
     * high count word. The digest of 2^29 zero bytes is from shasum. */
    const uint32_t bigLen = UINT32_C(1) << 29;
    uint8_t *zeros = calloc(1, bigLen);
    if (zeros == NULL) {
        printf("sha1: cannot allocate %" PRIu32 " bytes\n", bigLen);
        failures++;
    } else {
        failures += sha1TestCheck("zeros", zeros, 0, bigLen, bigLen,
                                  "5b088492c9f4778f409b7ae61477dec124c99033");
        free(zeros);
    }

    return failures;
}
#endif
