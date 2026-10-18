/* SHA-256 compression — ARMv8 Crypto Extensions (FEAT_SHA256).
 *
 * Implements the same H0..H7 chaining state as the portable sha256.c
 * but performs each 64-byte block via the dedicated NEON crypto
 * intrinsics (sha256h / sha256h2 / sha256su0 / sha256su1).  Expected
 * throughput on Apple Silicon is several GB/s, ~20-50x the scalar
 * loop.  Falls back to a stub on non-aarch64-crypto builds so the
 * file always compiles. */

#include "sha256ArmCrypto.h"

#include <string.h>

/* ------------------------------------------------------------------------ */
/* Runtime capability                                                       */
/* ------------------------------------------------------------------------ */

#if defined(__ARM_FEATURE_CRYPTO) && defined(__ARM_NEON) && defined(__aarch64__)
#define SHA256_ARM_CRYPTO_BUILD 1
#else
#define SHA256_ARM_CRYPTO_BUILD 0
#endif

#if SHA256_ARM_CRYPTO_BUILD

#include <arm_neon.h>

#if defined(__APPLE__)
/* On Apple Silicon the SHA-256 crypto extension is mandatory on every
 * shipping core, so we can shortcut runtime probing.  This avoids
 * dragging in sysctl just to discover a static answer. */
#define SHA256_ARM_CRYPTO_RUNTIME_OK() (true)
#elif defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_SHA2
#define HWCAP_SHA2 (1UL << 6)
#endif
#define SHA256_ARM_CRYPTO_RUNTIME_OK() ((getauxval(AT_HWCAP) & HWCAP_SHA2) != 0)
#else
/* Conservative: assume the compiler enabled crypto deliberately. */
#define SHA256_ARM_CRYPTO_RUNTIME_OK() (true)
#endif

bool sha256ArmCryptoAvailable(void) {
    return SHA256_ARM_CRYPTO_RUNTIME_OK();
}

/* ------------------------------------------------------------------------ */
/* Round constants — laid out as sixteen uint32x4_t groups of four.         */
/* ------------------------------------------------------------------------ */

static const uint32_t K_table[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

/* ------------------------------------------------------------------------ */
/* Compression                                                              */
/* ------------------------------------------------------------------------ */

/* The target("crypto") attribute lets this function emit sha256h*
 * regardless of whether the global -march= enables it. */
__attribute__((target("crypto"))) static void
compressBlocks(uint32_t state[8], const uint8_t *blocks, size_t num_blocks) {
    /* Load chaining state once and keep across blocks. */
    uint32x4_t abcd = vld1q_u32(state);
    uint32x4_t efgh = vld1q_u32(state + 4);

    for (size_t b = 0; b < num_blocks; b++) {
        const uint8_t *block = blocks + b * 64;

        /* Snapshot of incoming state for the Merkle-Damgård feed-forward. */
        uint32x4_t abcd_save = abcd;
        uint32x4_t efgh_save = efgh;

        /* Load 4×4 message words and byte-swap each lane: SHA-256
         * specifies big-endian message words but NEON loads in host
         * order (little-endian on aarch64). */
        uint32x4_t msg0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 0)));
        uint32x4_t msg1 =
            vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 16)));
        uint32x4_t msg2 =
            vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 32)));
        uint32x4_t msg3 =
            vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(block + 48)));

        uint32x4_t tmp;
        uint32x4_t prev_abcd;

        /* Rounds 0..3 */
        tmp = vaddq_u32(msg0, vld1q_u32(&K_table[0]));
        prev_abcd = abcd;
        abcd = vsha256hq_u32(abcd, efgh, tmp);
        efgh = vsha256h2q_u32(efgh, prev_abcd, tmp);

        /* Rounds 4..7 */
        tmp = vaddq_u32(msg1, vld1q_u32(&K_table[4]));
        prev_abcd = abcd;
        abcd = vsha256hq_u32(abcd, efgh, tmp);
        efgh = vsha256h2q_u32(efgh, prev_abcd, tmp);

        /* Rounds 8..11 */
        tmp = vaddq_u32(msg2, vld1q_u32(&K_table[8]));
        prev_abcd = abcd;
        abcd = vsha256hq_u32(abcd, efgh, tmp);
        efgh = vsha256h2q_u32(efgh, prev_abcd, tmp);

        /* Rounds 12..15 */
        tmp = vaddq_u32(msg3, vld1q_u32(&K_table[12]));
        prev_abcd = abcd;
        abcd = vsha256hq_u32(abcd, efgh, tmp);
        efgh = vsha256h2q_u32(efgh, prev_abcd, tmp);

        /* Rounds 16..19: schedule produces new msg0; consume in this round. */
        msg0 = vsha256su0q_u32(msg0, msg1);
        msg0 = vsha256su1q_u32(msg0, msg2, msg3);
        tmp = vaddq_u32(msg0, vld1q_u32(&K_table[16]));
        prev_abcd = abcd;
        abcd = vsha256hq_u32(abcd, efgh, tmp);
        efgh = vsha256h2q_u32(efgh, prev_abcd, tmp);

        /* Rounds 20..23 */
        msg1 = vsha256su0q_u32(msg1, msg2);
        msg1 = vsha256su1q_u32(msg1, msg3, msg0);
        tmp = vaddq_u32(msg1, vld1q_u32(&K_table[20]));
        prev_abcd = abcd;
        abcd = vsha256hq_u32(abcd, efgh, tmp);
        efgh = vsha256h2q_u32(efgh, prev_abcd, tmp);

        /* Rounds 24..27 */
        msg2 = vsha256su0q_u32(msg2, msg3);
        msg2 = vsha256su1q_u32(msg2, msg0, msg1);
        tmp = vaddq_u32(msg2, vld1q_u32(&K_table[24]));
        prev_abcd = abcd;
        abcd = vsha256hq_u32(abcd, efgh, tmp);
        efgh = vsha256h2q_u32(efgh, prev_abcd, tmp);

        /* Rounds 28..31 */
        msg3 = vsha256su0q_u32(msg3, msg0);
        msg3 = vsha256su1q_u32(msg3, msg1, msg2);
        tmp = vaddq_u32(msg3, vld1q_u32(&K_table[28]));
        prev_abcd = abcd;
        abcd = vsha256hq_u32(abcd, efgh, tmp);
        efgh = vsha256h2q_u32(efgh, prev_abcd, tmp);

        /* Rounds 32..35 */
        msg0 = vsha256su0q_u32(msg0, msg1);
        msg0 = vsha256su1q_u32(msg0, msg2, msg3);
        tmp = vaddq_u32(msg0, vld1q_u32(&K_table[32]));
        prev_abcd = abcd;
        abcd = vsha256hq_u32(abcd, efgh, tmp);
        efgh = vsha256h2q_u32(efgh, prev_abcd, tmp);

        /* Rounds 36..39 */
        msg1 = vsha256su0q_u32(msg1, msg2);
        msg1 = vsha256su1q_u32(msg1, msg3, msg0);
        tmp = vaddq_u32(msg1, vld1q_u32(&K_table[36]));
        prev_abcd = abcd;
        abcd = vsha256hq_u32(abcd, efgh, tmp);
        efgh = vsha256h2q_u32(efgh, prev_abcd, tmp);

        /* Rounds 40..43 */
        msg2 = vsha256su0q_u32(msg2, msg3);
        msg2 = vsha256su1q_u32(msg2, msg0, msg1);
        tmp = vaddq_u32(msg2, vld1q_u32(&K_table[40]));
        prev_abcd = abcd;
        abcd = vsha256hq_u32(abcd, efgh, tmp);
        efgh = vsha256h2q_u32(efgh, prev_abcd, tmp);

        /* Rounds 44..47 */
        msg3 = vsha256su0q_u32(msg3, msg0);
        msg3 = vsha256su1q_u32(msg3, msg1, msg2);
        tmp = vaddq_u32(msg3, vld1q_u32(&K_table[44]));
        prev_abcd = abcd;
        abcd = vsha256hq_u32(abcd, efgh, tmp);
        efgh = vsha256h2q_u32(efgh, prev_abcd, tmp);

        /* Rounds 48..51 */
        msg0 = vsha256su0q_u32(msg0, msg1);
        msg0 = vsha256su1q_u32(msg0, msg2, msg3);
        tmp = vaddq_u32(msg0, vld1q_u32(&K_table[48]));
        prev_abcd = abcd;
        abcd = vsha256hq_u32(abcd, efgh, tmp);
        efgh = vsha256h2q_u32(efgh, prev_abcd, tmp);

        /* Rounds 52..55 */
        msg1 = vsha256su0q_u32(msg1, msg2);
        msg1 = vsha256su1q_u32(msg1, msg3, msg0);
        tmp = vaddq_u32(msg1, vld1q_u32(&K_table[52]));
        prev_abcd = abcd;
        abcd = vsha256hq_u32(abcd, efgh, tmp);
        efgh = vsha256h2q_u32(efgh, prev_abcd, tmp);

        /* Rounds 56..59 — final two iterations don't need a new su0/su1
         * pair because we won't reference msg2/msg3 again. */
        msg2 = vsha256su0q_u32(msg2, msg3);
        msg2 = vsha256su1q_u32(msg2, msg0, msg1);
        tmp = vaddq_u32(msg2, vld1q_u32(&K_table[56]));
        prev_abcd = abcd;
        abcd = vsha256hq_u32(abcd, efgh, tmp);
        efgh = vsha256h2q_u32(efgh, prev_abcd, tmp);

        /* Rounds 60..63 */
        msg3 = vsha256su0q_u32(msg3, msg0);
        msg3 = vsha256su1q_u32(msg3, msg1, msg2);
        tmp = vaddq_u32(msg3, vld1q_u32(&K_table[60]));
        prev_abcd = abcd;
        abcd = vsha256hq_u32(abcd, efgh, tmp);
        efgh = vsha256h2q_u32(efgh, prev_abcd, tmp);

        /* Feed-forward (the Merkle-Damgård chaining step). */
        abcd = vaddq_u32(abcd, abcd_save);
        efgh = vaddq_u32(efgh, efgh_save);
    }

    vst1q_u32(state, abcd);
    vst1q_u32(state + 4, efgh);
}

void sha256ArmCryptoCompressBlocks(uint32_t state[8], const uint8_t *blocks,
                                   size_t num_blocks) {
    if (num_blocks == 0) {
        return;
    }
    compressBlocks(state, blocks, num_blocks);
}

/* Four-lane multi-buffer compressor.  Each lane keeps its own abcd/efgh
 * register pair and its own four message-schedule registers, so the
 * working set is 4 × 6 = 24 NEON Q-registers — fits aarch64's 32-reg
 * file with room to spare for tmp/prev_abcd scratch.  Interleaving the
 * four streams inside every round group lets the OoO core overlap the
 * ~4-cycle vsha256hq latency across lanes; throughput is bounded by
 * issue port (not latency) once all four lanes are in flight. */
__attribute__((target("crypto"))) static void
compressBlocksMulti4(uint32_t states[4][8], const uint8_t *const blocks[4],
                     size_t num_blocks) {
    uint32x4_t abcd0 = vld1q_u32(states[0]);
    uint32x4_t efgh0 = vld1q_u32(states[0] + 4);
    uint32x4_t abcd1 = vld1q_u32(states[1]);
    uint32x4_t efgh1 = vld1q_u32(states[1] + 4);
    uint32x4_t abcd2 = vld1q_u32(states[2]);
    uint32x4_t efgh2 = vld1q_u32(states[2] + 4);
    uint32x4_t abcd3 = vld1q_u32(states[3]);
    uint32x4_t efgh3 = vld1q_u32(states[3] + 4);

    const uint8_t *p0 = blocks[0];
    const uint8_t *p1 = blocks[1];
    const uint8_t *p2 = blocks[2];
    const uint8_t *p3 = blocks[3];

    for (size_t b = 0; b < num_blocks; b++) {
        uint32x4_t abcd0_save = abcd0;
        uint32x4_t efgh0_save = efgh0;
        uint32x4_t abcd1_save = abcd1;
        uint32x4_t efgh1_save = efgh1;
        uint32x4_t abcd2_save = abcd2;
        uint32x4_t efgh2_save = efgh2;
        uint32x4_t abcd3_save = abcd3;
        uint32x4_t efgh3_save = efgh3;

        /* Per-lane message-schedule registers (msgA..msgD per lane).
         * Loaded big-endian to match SHA-256's word order. */
        uint32x4_t m0a = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p0 + 0)));
        uint32x4_t m0b = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p0 + 16)));
        uint32x4_t m0c = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p0 + 32)));
        uint32x4_t m0d = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p0 + 48)));
        uint32x4_t m1a = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p1 + 0)));
        uint32x4_t m1b = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p1 + 16)));
        uint32x4_t m1c = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p1 + 32)));
        uint32x4_t m1d = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p1 + 48)));
        uint32x4_t m2a = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p2 + 0)));
        uint32x4_t m2b = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p2 + 16)));
        uint32x4_t m2c = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p2 + 32)));
        uint32x4_t m2d = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p2 + 48)));
        uint32x4_t m3a = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p3 + 0)));
        uint32x4_t m3b = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p3 + 16)));
        uint32x4_t m3c = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p3 + 32)));
        uint32x4_t m3d = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p3 + 48)));

        uint32x4_t k;
        uint32x4_t t0, t1, t2, t3;
        uint32x4_t pa0, pa1, pa2, pa3;

/* Round group: four SHA-256 rounds across all four lanes, interleaved
 * so each lane's sha256h/sha256h2 pair can be issued before the prior
 * lane's result is consumed. */
#define ROUND_GROUP_INIT(KIDX, MA, MB, MC, MD)                                 \
    do {                                                                       \
        k = vld1q_u32(&K_table[(KIDX)]);                                       \
        t0 = vaddq_u32(MA, k);                                                 \
        t1 = vaddq_u32(MB, k);                                                 \
        t2 = vaddq_u32(MC, k);                                                 \
        t3 = vaddq_u32(MD, k);                                                 \
        pa0 = abcd0;                                                           \
        pa1 = abcd1;                                                           \
        pa2 = abcd2;                                                           \
        pa3 = abcd3;                                                           \
        abcd0 = vsha256hq_u32(abcd0, efgh0, t0);                               \
        abcd1 = vsha256hq_u32(abcd1, efgh1, t1);                               \
        abcd2 = vsha256hq_u32(abcd2, efgh2, t2);                               \
        abcd3 = vsha256hq_u32(abcd3, efgh3, t3);                               \
        efgh0 = vsha256h2q_u32(efgh0, pa0, t0);                                \
        efgh1 = vsha256h2q_u32(efgh1, pa1, t1);                                \
        efgh2 = vsha256h2q_u32(efgh2, pa2, t2);                                \
        efgh3 = vsha256h2q_u32(efgh3, pa3, t3);                                \
    } while (0)

#define ROUND_GROUP_SCHED(KIDX, M0A, M0B, M0C, M0D, M1A, M1B, M1C, M1D, M2A,   \
                          M2B, M2C, M2D, M3A, M3B, M3C, M3D)                   \
    do {                                                                       \
        M0A = vsha256su0q_u32(M0A, M0B);                                       \
        M1A = vsha256su0q_u32(M1A, M1B);                                       \
        M2A = vsha256su0q_u32(M2A, M2B);                                       \
        M3A = vsha256su0q_u32(M3A, M3B);                                       \
        M0A = vsha256su1q_u32(M0A, M0C, M0D);                                  \
        M1A = vsha256su1q_u32(M1A, M1C, M1D);                                  \
        M2A = vsha256su1q_u32(M2A, M2C, M2D);                                  \
        M3A = vsha256su1q_u32(M3A, M3C, M3D);                                  \
        ROUND_GROUP_INIT(KIDX, M0A, M1A, M2A, M3A);                            \
    } while (0)

        /* Rounds 0..3, 4..7, 8..11, 12..15 — message words come straight
         * from the loaded block, no schedule expansion yet. */
        ROUND_GROUP_INIT(0, m0a, m1a, m2a, m3a);
        /* The macro fuses the four per-lane sha256h*/
        k = vld1q_u32(&K_table[4]);
        t0 = vaddq_u32(m0b, k);
        t1 = vaddq_u32(m1b, k);
        t2 = vaddq_u32(m2b, k);
        t3 = vaddq_u32(m3b, k);
        pa0 = abcd0;
        pa1 = abcd1;
        pa2 = abcd2;
        pa3 = abcd3;
        abcd0 = vsha256hq_u32(abcd0, efgh0, t0);
        abcd1 = vsha256hq_u32(abcd1, efgh1, t1);
        abcd2 = vsha256hq_u32(abcd2, efgh2, t2);
        abcd3 = vsha256hq_u32(abcd3, efgh3, t3);
        efgh0 = vsha256h2q_u32(efgh0, pa0, t0);
        efgh1 = vsha256h2q_u32(efgh1, pa1, t1);
        efgh2 = vsha256h2q_u32(efgh2, pa2, t2);
        efgh3 = vsha256h2q_u32(efgh3, pa3, t3);

        k = vld1q_u32(&K_table[8]);
        t0 = vaddq_u32(m0c, k);
        t1 = vaddq_u32(m1c, k);
        t2 = vaddq_u32(m2c, k);
        t3 = vaddq_u32(m3c, k);
        pa0 = abcd0;
        pa1 = abcd1;
        pa2 = abcd2;
        pa3 = abcd3;
        abcd0 = vsha256hq_u32(abcd0, efgh0, t0);
        abcd1 = vsha256hq_u32(abcd1, efgh1, t1);
        abcd2 = vsha256hq_u32(abcd2, efgh2, t2);
        abcd3 = vsha256hq_u32(abcd3, efgh3, t3);
        efgh0 = vsha256h2q_u32(efgh0, pa0, t0);
        efgh1 = vsha256h2q_u32(efgh1, pa1, t1);
        efgh2 = vsha256h2q_u32(efgh2, pa2, t2);
        efgh3 = vsha256h2q_u32(efgh3, pa3, t3);

        k = vld1q_u32(&K_table[12]);
        t0 = vaddq_u32(m0d, k);
        t1 = vaddq_u32(m1d, k);
        t2 = vaddq_u32(m2d, k);
        t3 = vaddq_u32(m3d, k);
        pa0 = abcd0;
        pa1 = abcd1;
        pa2 = abcd2;
        pa3 = abcd3;
        abcd0 = vsha256hq_u32(abcd0, efgh0, t0);
        abcd1 = vsha256hq_u32(abcd1, efgh1, t1);
        abcd2 = vsha256hq_u32(abcd2, efgh2, t2);
        abcd3 = vsha256hq_u32(abcd3, efgh3, t3);
        efgh0 = vsha256h2q_u32(efgh0, pa0, t0);
        efgh1 = vsha256h2q_u32(efgh1, pa1, t1);
        efgh2 = vsha256h2q_u32(efgh2, pa2, t2);
        efgh3 = vsha256h2q_u32(efgh3, pa3, t3);

        /* Rounds 16..19 — first schedule expansion. */
        ROUND_GROUP_SCHED(16, m0a, m0b, m0c, m0d, m1a, m1b, m1c, m1d, m2a, m2b,
                          m2c, m2d, m3a, m3b, m3c, m3d);

        /* Rounds 20..23 */
        ROUND_GROUP_SCHED(20, m0b, m0c, m0d, m0a, m1b, m1c, m1d, m1a, m2b, m2c,
                          m2d, m2a, m3b, m3c, m3d, m3a);

        /* Rounds 24..27 */
        ROUND_GROUP_SCHED(24, m0c, m0d, m0a, m0b, m1c, m1d, m1a, m1b, m2c, m2d,
                          m2a, m2b, m3c, m3d, m3a, m3b);

        /* Rounds 28..31 */
        ROUND_GROUP_SCHED(28, m0d, m0a, m0b, m0c, m1d, m1a, m1b, m1c, m2d, m2a,
                          m2b, m2c, m3d, m3a, m3b, m3c);

        /* Rounds 32..35 */
        ROUND_GROUP_SCHED(32, m0a, m0b, m0c, m0d, m1a, m1b, m1c, m1d, m2a, m2b,
                          m2c, m2d, m3a, m3b, m3c, m3d);

        /* Rounds 36..39 */
        ROUND_GROUP_SCHED(36, m0b, m0c, m0d, m0a, m1b, m1c, m1d, m1a, m2b, m2c,
                          m2d, m2a, m3b, m3c, m3d, m3a);

        /* Rounds 40..43 */
        ROUND_GROUP_SCHED(40, m0c, m0d, m0a, m0b, m1c, m1d, m1a, m1b, m2c, m2d,
                          m2a, m2b, m3c, m3d, m3a, m3b);

        /* Rounds 44..47 */
        ROUND_GROUP_SCHED(44, m0d, m0a, m0b, m0c, m1d, m1a, m1b, m1c, m2d, m2a,
                          m2b, m2c, m3d, m3a, m3b, m3c);

        /* Rounds 48..51 */
        ROUND_GROUP_SCHED(48, m0a, m0b, m0c, m0d, m1a, m1b, m1c, m1d, m2a, m2b,
                          m2c, m2d, m3a, m3b, m3c, m3d);

        /* Rounds 52..55 */
        ROUND_GROUP_SCHED(52, m0b, m0c, m0d, m0a, m1b, m1c, m1d, m1a, m2b, m2c,
                          m2d, m2a, m3b, m3c, m3d, m3a);

        /* Rounds 56..59 */
        ROUND_GROUP_SCHED(56, m0c, m0d, m0a, m0b, m1c, m1d, m1a, m1b, m2c, m2d,
                          m2a, m2b, m3c, m3d, m3a, m3b);

        /* Rounds 60..63 */
        ROUND_GROUP_SCHED(60, m0d, m0a, m0b, m0c, m1d, m1a, m1b, m1c, m2d, m2a,
                          m2b, m2c, m3d, m3a, m3b, m3c);

#undef ROUND_GROUP_INIT
#undef ROUND_GROUP_SCHED

        /* Feed-forward per lane. */
        abcd0 = vaddq_u32(abcd0, abcd0_save);
        efgh0 = vaddq_u32(efgh0, efgh0_save);
        abcd1 = vaddq_u32(abcd1, abcd1_save);
        efgh1 = vaddq_u32(efgh1, efgh1_save);
        abcd2 = vaddq_u32(abcd2, abcd2_save);
        efgh2 = vaddq_u32(efgh2, efgh2_save);
        abcd3 = vaddq_u32(abcd3, abcd3_save);
        efgh3 = vaddq_u32(efgh3, efgh3_save);

        p0 += 64;
        p1 += 64;
        p2 += 64;
        p3 += 64;
    }

    vst1q_u32(states[0], abcd0);
    vst1q_u32(states[0] + 4, efgh0);
    vst1q_u32(states[1], abcd1);
    vst1q_u32(states[1] + 4, efgh1);
    vst1q_u32(states[2], abcd2);
    vst1q_u32(states[2] + 4, efgh2);
    vst1q_u32(states[3], abcd3);
    vst1q_u32(states[3] + 4, efgh3);
}

/* v2 attempt — message schedule shifted four round groups earlier so
 * the OoO core has more headroom to hide vsha256su0/su1 latency behind
 * the prior groups' vsha256hq/h2q ops.
 *
 * Measured on Apple Silicon: 1.43x vs 1.46x for v1.  The OoO core
 * apparently already hides the latency at v1's structure, and the
 * extended live range across more groups costs slightly more in
 * register pressure / scheduling.  Kept here as documentation of the
 * attempt; v1 remains the wired path until a measured win lands.
 *
 * To compare experimentally, change sha256ArmCryptoCompressBlocksMulti4
 * below to call compressBlocksMulti4V2 instead of compressBlocksMulti4. */
__attribute__((target("crypto"))) __attribute__((unused)) static void
compressBlocksMulti4V2(uint32_t states[4][8], const uint8_t *const blocks[4],
                       size_t num_blocks) {
    uint32x4_t abcd0 = vld1q_u32(states[0]);
    uint32x4_t efgh0 = vld1q_u32(states[0] + 4);
    uint32x4_t abcd1 = vld1q_u32(states[1]);
    uint32x4_t efgh1 = vld1q_u32(states[1] + 4);
    uint32x4_t abcd2 = vld1q_u32(states[2]);
    uint32x4_t efgh2 = vld1q_u32(states[2] + 4);
    uint32x4_t abcd3 = vld1q_u32(states[3]);
    uint32x4_t efgh3 = vld1q_u32(states[3] + 4);

    const uint8_t *p0 = blocks[0];
    const uint8_t *p1 = blocks[1];
    const uint8_t *p2 = blocks[2];
    const uint8_t *p3 = blocks[3];

    for (size_t b = 0; b < num_blocks; b++) {
        uint32x4_t abcd0_save = abcd0;
        uint32x4_t efgh0_save = efgh0;
        uint32x4_t abcd1_save = abcd1;
        uint32x4_t efgh1_save = efgh1;
        uint32x4_t abcd2_save = abcd2;
        uint32x4_t efgh2_save = efgh2;
        uint32x4_t abcd3_save = abcd3;
        uint32x4_t efgh3_save = efgh3;

        uint32x4_t m0a = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p0 + 0)));
        uint32x4_t m0b = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p0 + 16)));
        uint32x4_t m0c = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p0 + 32)));
        uint32x4_t m0d = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p0 + 48)));
        uint32x4_t m1a = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p1 + 0)));
        uint32x4_t m1b = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p1 + 16)));
        uint32x4_t m1c = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p1 + 32)));
        uint32x4_t m1d = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p1 + 48)));
        uint32x4_t m2a = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p2 + 0)));
        uint32x4_t m2b = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p2 + 16)));
        uint32x4_t m2c = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p2 + 32)));
        uint32x4_t m2d = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p2 + 48)));
        uint32x4_t m3a = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p3 + 0)));
        uint32x4_t m3b = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p3 + 16)));
        uint32x4_t m3c = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p3 + 32)));
        uint32x4_t m3d = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p3 + 48)));

        uint32x4_t k;
        uint32x4_t t0, t1, t2, t3;
        uint32x4_t pa0, pa1, pa2, pa3;

/* h/h2 round group, no schedule. */
#define ROUND_H_ONLY(KIDX, M0, M1, M2, M3)                                     \
    do {                                                                       \
        k = vld1q_u32(&K_table[(KIDX)]);                                       \
        t0 = vaddq_u32(M0, k);                                                 \
        t1 = vaddq_u32(M1, k);                                                 \
        t2 = vaddq_u32(M2, k);                                                 \
        t3 = vaddq_u32(M3, k);                                                 \
        pa0 = abcd0;                                                           \
        pa1 = abcd1;                                                           \
        pa2 = abcd2;                                                           \
        pa3 = abcd3;                                                           \
        abcd0 = vsha256hq_u32(abcd0, efgh0, t0);                               \
        abcd1 = vsha256hq_u32(abcd1, efgh1, t1);                               \
        abcd2 = vsha256hq_u32(abcd2, efgh2, t2);                               \
        abcd3 = vsha256hq_u32(abcd3, efgh3, t3);                               \
        efgh0 = vsha256h2q_u32(efgh0, pa0, t0);                                \
        efgh1 = vsha256h2q_u32(efgh1, pa1, t1);                                \
        efgh2 = vsha256h2q_u32(efgh2, pa2, t2);                                \
        efgh3 = vsha256h2q_u32(efgh3, pa3, t3);                                \
    } while (0)

/* h/h2 round group AND simultaneously schedule the message that will
 * be consumed four round groups later.  Issuing the su0/su1 chain in
 * parallel with this group's h/h2 lets the OoO core overlap their
 * crypto-port issue.  Schedule of M0X = su1(su0(M0X, M0Y), M0Z, M0W)
 * writes back into M0X for reuse without an extra register. */
#define ROUND_H_AND_SCHED(KIDX, USE_A, USE_B, USE_C, USE_D, S0X, S0Y, S0Z,     \
                          S0W, S1X, S1Y, S1Z, S1W, S2X, S2Y, S2Z, S2W, S3X,    \
                          S3Y, S3Z, S3W)                                       \
    do {                                                                       \
        k = vld1q_u32(&K_table[(KIDX)]);                                       \
        t0 = vaddq_u32(USE_A, k);                                              \
        t1 = vaddq_u32(USE_B, k);                                              \
        t2 = vaddq_u32(USE_C, k);                                              \
        t3 = vaddq_u32(USE_D, k);                                              \
        pa0 = abcd0;                                                           \
        pa1 = abcd1;                                                           \
        pa2 = abcd2;                                                           \
        pa3 = abcd3;                                                           \
        abcd0 = vsha256hq_u32(abcd0, efgh0, t0);                               \
        abcd1 = vsha256hq_u32(abcd1, efgh1, t1);                               \
        abcd2 = vsha256hq_u32(abcd2, efgh2, t2);                               \
        abcd3 = vsha256hq_u32(abcd3, efgh3, t3);                               \
        S0X = vsha256su0q_u32(S0X, S0Y);                                       \
        S1X = vsha256su0q_u32(S1X, S1Y);                                       \
        S2X = vsha256su0q_u32(S2X, S2Y);                                       \
        S3X = vsha256su0q_u32(S3X, S3Y);                                       \
        efgh0 = vsha256h2q_u32(efgh0, pa0, t0);                                \
        efgh1 = vsha256h2q_u32(efgh1, pa1, t1);                                \
        efgh2 = vsha256h2q_u32(efgh2, pa2, t2);                                \
        efgh3 = vsha256h2q_u32(efgh3, pa3, t3);                                \
        S0X = vsha256su1q_u32(S0X, S0Z, S0W);                                  \
        S1X = vsha256su1q_u32(S1X, S1Z, S1W);                                  \
        S2X = vsha256su1q_u32(S2X, S2Z, S2W);                                  \
        S3X = vsha256su1q_u32(S3X, S3Z, S3W);                                  \
    } while (0)

        /* Phase 1 (rounds 0..15): use raw block words; schedule the
         * four messages that will be consumed at rounds 16..31. */
        ROUND_H_AND_SCHED(0, m0a, m1a, m2a, m3a, m0a, m0b, m0c, m0d, m1a, m1b,
                          m1c, m1d, m2a, m2b, m2c, m2d, m3a, m3b, m3c, m3d);
        ROUND_H_AND_SCHED(4, m0b, m1b, m2b, m3b, m0b, m0c, m0d, m0a, m1b, m1c,
                          m1d, m1a, m2b, m2c, m2d, m2a, m3b, m3c, m3d, m3a);
        ROUND_H_AND_SCHED(8, m0c, m1c, m2c, m3c, m0c, m0d, m0a, m0b, m1c, m1d,
                          m1a, m1b, m2c, m2d, m2a, m2b, m3c, m3d, m3a, m3b);
        ROUND_H_AND_SCHED(12, m0d, m1d, m2d, m3d, m0d, m0a, m0b, m0c, m1d, m1a,
                          m1b, m1c, m2d, m2a, m2b, m2c, m3d, m3a, m3b, m3c);

        /* Phase 2 (rounds 16..47): use scheduled messages; schedule the
         * next set in parallel. */
        ROUND_H_AND_SCHED(16, m0a, m1a, m2a, m3a, m0a, m0b, m0c, m0d, m1a, m1b,
                          m1c, m1d, m2a, m2b, m2c, m2d, m3a, m3b, m3c, m3d);
        ROUND_H_AND_SCHED(20, m0b, m1b, m2b, m3b, m0b, m0c, m0d, m0a, m1b, m1c,
                          m1d, m1a, m2b, m2c, m2d, m2a, m3b, m3c, m3d, m3a);
        ROUND_H_AND_SCHED(24, m0c, m1c, m2c, m3c, m0c, m0d, m0a, m0b, m1c, m1d,
                          m1a, m1b, m2c, m2d, m2a, m2b, m3c, m3d, m3a, m3b);
        ROUND_H_AND_SCHED(28, m0d, m1d, m2d, m3d, m0d, m0a, m0b, m0c, m1d, m1a,
                          m1b, m1c, m2d, m2a, m2b, m2c, m3d, m3a, m3b, m3c);
        ROUND_H_AND_SCHED(32, m0a, m1a, m2a, m3a, m0a, m0b, m0c, m0d, m1a, m1b,
                          m1c, m1d, m2a, m2b, m2c, m2d, m3a, m3b, m3c, m3d);
        ROUND_H_AND_SCHED(36, m0b, m1b, m2b, m3b, m0b, m0c, m0d, m0a, m1b, m1c,
                          m1d, m1a, m2b, m2c, m2d, m2a, m3b, m3c, m3d, m3a);
        ROUND_H_AND_SCHED(40, m0c, m1c, m2c, m3c, m0c, m0d, m0a, m0b, m1c, m1d,
                          m1a, m1b, m2c, m2d, m2a, m2b, m3c, m3d, m3a, m3b);
        ROUND_H_AND_SCHED(44, m0d, m1d, m2d, m3d, m0d, m0a, m0b, m0c, m1d, m1a,
                          m1b, m1c, m2d, m2a, m2b, m2c, m3d, m3a, m3b, m3c);

        /* Phase 3 (rounds 48..63): no further schedule work — last four
         * groups consume the already-scheduled messages. */
        ROUND_H_ONLY(48, m0a, m1a, m2a, m3a);
        ROUND_H_ONLY(52, m0b, m1b, m2b, m3b);
        ROUND_H_ONLY(56, m0c, m1c, m2c, m3c);
        ROUND_H_ONLY(60, m0d, m1d, m2d, m3d);

#undef ROUND_H_ONLY
#undef ROUND_H_AND_SCHED

        abcd0 = vaddq_u32(abcd0, abcd0_save);
        efgh0 = vaddq_u32(efgh0, efgh0_save);
        abcd1 = vaddq_u32(abcd1, abcd1_save);
        efgh1 = vaddq_u32(efgh1, efgh1_save);
        abcd2 = vaddq_u32(abcd2, abcd2_save);
        efgh2 = vaddq_u32(efgh2, efgh2_save);
        abcd3 = vaddq_u32(abcd3, abcd3_save);
        efgh3 = vaddq_u32(efgh3, efgh3_save);

        p0 += 64;
        p1 += 64;
        p2 += 64;
        p3 += 64;
    }

    vst1q_u32(states[0], abcd0);
    vst1q_u32(states[0] + 4, efgh0);
    vst1q_u32(states[1], abcd1);
    vst1q_u32(states[1] + 4, efgh1);
    vst1q_u32(states[2], abcd2);
    vst1q_u32(states[2] + 4, efgh2);
    vst1q_u32(states[3], abcd3);
    vst1q_u32(states[3] + 4, efgh3);
}

void sha256ArmCryptoCompressBlocksMulti4(uint32_t states[4][8],
                                         const uint8_t *const blocks[4],
                                         size_t num_blocks) {
    if (num_blocks == 0) {
        return;
    }
    compressBlocksMulti4(states, blocks, num_blocks);
}

#else /* !SHA256_ARM_CRYPTO_BUILD — delegate to portable so direct           \
       *                              callers get correct (just slower)        \
       *                              output instead of silent no-op. */

bool sha256ArmCryptoAvailable(void) {
    return false;
}

void sha256ArmCryptoCompressBlocks(uint32_t state[8], const uint8_t *blocks,
                                   size_t num_blocks) {
    sha256PortableCompressBlocks(state, blocks, num_blocks);
}

void sha256ArmCryptoCompressBlocksMulti4(uint32_t states[4][8],
                                         const uint8_t *const blocks[4],
                                         size_t num_blocks) {
    for (uint32_t lane = 0; lane < 4; lane++) {
        sha256PortableCompressBlocks(states[lane], blocks[lane], num_blocks);
    }
}

#endif /* SHA256_ARM_CRYPTO_BUILD */

/* ------------------------------------------------------------------------ */
/* Test driver                                                              */
/* ------------------------------------------------------------------------ */

#ifdef DATAKIT_TEST

#include "cleaner.h"
#include "ctest.h"
#include "datakit.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Scope-exit cleaners (see cleaner.h idioms) */
defcleanerName(uint8_t *, zfree, U8);

/* Wrapper that mirrors sha256Hash but routes the bulk-compress through
 * our accelerated path.  Padding logic is inlined directly from
 * FIPS 180-4 §5.1.1 so we don't need to touch sha256.c. */
static void sha256ArmCryptoHash(const void *data, size_t len,
                                uint8_t out[SHA256_DIGEST_LEN]) {
    sha256Ctx ctx;
    sha256Init(&ctx);

    const uint8_t *p = (const uint8_t *)data;
    ctx.bit_count = (uint64_t)len * 8;

    /* Bulk-compress all complete 64-byte blocks via the crypto path. */
    size_t full = len / SHA256_BLOCK_LEN;
    if (full > 0) {
        sha256ArmCryptoCompressBlocks(ctx.state, p, full);
        p += full * SHA256_BLOCK_LEN;
        len -= full * SHA256_BLOCK_LEN;
    }

    /* Stage the tail and append the standard padding.  We route the
     * final one or two blocks back through the crypto path too so the
     * test exercises it on the boundary case. */
    uint8_t tail[2 * SHA256_BLOCK_LEN];
    memcpy(tail, p, len);
    tail[len] = 0x80;
    size_t pad_to = (len < 56) ? SHA256_BLOCK_LEN : 2 * SHA256_BLOCK_LEN;
    memset(tail + len + 1, 0, pad_to - len - 1 - 8);

    uint64_t bits = ctx.bit_count;
    uint8_t *lenp = tail + pad_to - 8;
    lenp[0] = (uint8_t)(bits >> 56);
    lenp[1] = (uint8_t)(bits >> 48);
    lenp[2] = (uint8_t)(bits >> 40);
    lenp[3] = (uint8_t)(bits >> 32);
    lenp[4] = (uint8_t)(bits >> 24);
    lenp[5] = (uint8_t)(bits >> 16);
    lenp[6] = (uint8_t)(bits >> 8);
    lenp[7] = (uint8_t)bits;

    sha256ArmCryptoCompressBlocks(ctx.state, tail, pad_to / SHA256_BLOCK_LEN);

    for (uint32_t i = 0; i < 8; i++) {
        uint32_t v = ctx.state[i];
        out[i * 4 + 0] = (uint8_t)(v >> 24);
        out[i * 4 + 1] = (uint8_t)(v >> 16);
        out[i * 4 + 2] = (uint8_t)(v >> 8);
        out[i * 4 + 3] = (uint8_t)v;
    }
}

static int hashEqual(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, SHA256_DIGEST_LEN) == 0;
}

static void hashToHex(const uint8_t *h, char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < SHA256_DIGEST_LEN; i++) {
        out[i * 2] = hex[h[i] >> 4];
        out[i * 2 + 1] = hex[h[i] & 0xf];
    }
    out[64] = 0;
}

int sha256ArmCryptoTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    /* Stub-delegation guard.  On non-ARM-crypto builds
     * sha256ArmCryptoCompressBlocks delegates to the portable path —
     * but ONLY if the stub is wired correctly.  Catch the silent-no-op
     * regression class by running a small known-input differential
     * against the portable digest on every platform. */
    TEST("stub delegation: arm-crypto-compress matches portable on direct "
         "call") {
        static const char *vectors[] = {
            "", "abc", "The quick brown fox jumps over the lazy dog"};
        for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
            const char *s = vectors[i];
            const size_t n = strlen(s);
            uint8_t ref[SHA256_DIGEST_LEN], got[SHA256_DIGEST_LEN];
            sha256Hash(s, n, ref);
            sha256ArmCryptoHash((const uint8_t *)s, n, got);
            if (!hashEqual(ref, got)) {
                char rh[65], gh[65];
                hashToHex(ref, rh);
                hashToHex(got, gh);
                ERR("stub vector[%zu] '%s': got %s, ref %s", i, s, gh, rh);
            }
        }
    }

    if (!sha256ArmCryptoAvailable()) {
        printf("    (ARM crypto unavailable; remaining suite skipped)\n");
        TEST_FINAL_RESULT;
    }

    TEST("sha256 arm crypto matches portable on known vectors") {
        struct {
            const char *input;
            size_t len;
        } cases[] = {
            {"", 0},
            {"abc", 3},
            {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56},
            {"The quick brown fox jumps over the lazy dog", 43},
            {"1234567890123456789012345678901234567890123456789012345", 55},
            {"12345678901234567890123456789012345678901234567890123456", 56},
            {"123456789012345678901234567890123456789012345678901234567", 57},
            {"1234567890123456789012345678901234567890123456789012345678901234"
             "5",
             65},
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            uint8_t expected[SHA256_DIGEST_LEN];
            uint8_t got[SHA256_DIGEST_LEN];
            sha256Hash(cases[i].input, cases[i].len, expected);
            sha256ArmCryptoHash(cases[i].input, cases[i].len, got);
            if (!hashEqual(got, expected)) {
                char gh[65], eh[65];
                hashToHex(got, gh);
                hashToHex(expected, eh);
                ERR("vector len=%zu: got %s, expected %s", cases[i].len, gh,
                    eh);
            }
        }
    }

    TEST("sha256 arm crypto differential vs portable on 1M random inputs") {
        const uint32_t iters = 1000000;
        /* Linear-congruential PRNG so the suite is deterministic and
         * independent of rand() implementation churn. */
        uint64_t rng = 0xDEADBEEFCAFEBABEULL;
        uint32_t mismatches = 0;
        cleanerName(zfree, U8) uint8_t *buf = zmalloc(4096);

        for (uint32_t t = 0; t < iters; t++) {
            rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
            size_t n = (size_t)((rng >> 33) % 4097); /* 0..4096 */
            for (size_t i = 0; i < n; i++) {
                rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
                buf[i] = (uint8_t)(rng >> 56);
            }

            uint8_t portable[SHA256_DIGEST_LEN];
            uint8_t crypto[SHA256_DIGEST_LEN];
            sha256Hash(buf, n, portable);
            sha256ArmCryptoHash(buf, n, crypto);
            if (!hashEqual(portable, crypto)) {
                mismatches++;
                if (mismatches <= 3) {
                    char gh[65], eh[65];
                    hashToHex(crypto, gh);
                    hashToHex(portable, eh);
                    ERR("mismatch at iter %u (n=%zu): got %s, expected %s", t,
                        n, gh, eh);
                }
            }
        }

        if (mismatches == 0) {
            printf("    %u random inputs: all match\n", iters);
        } else {
            ERR("%u total mismatches over %u iterations", mismatches, iters);
        }
    }

    TEST("sha256 arm crypto performance bench") {
        const size_t bufSize = 1 << 20; /* 1 MiB */
        const uint32_t iters = 64;      /* total 64 MiB */
        cleanerName(zfree, U8) uint8_t *buf = zmalloc(bufSize);
        memset(buf, 0xa5, bufSize);

        /* Portable timing: the scalar compress function itself, fed the
         * same bulk buffer as the crypto timing below. (sha256Update()
         * would dispatch to the crypto path on this hardware and time
         * crypto against crypto.) */
        struct timespec t0, t1;
        uint32_t portableState[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372,
                                     0xa54ff53a, 0x510e527f, 0x9b05688c,
                                     0x1f83d9ab, 0x5be0cd19};
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (uint32_t i = 0; i < iters; i++) {
            sha256PortableCompressBlocks(portableState, buf,
                                         bufSize / SHA256_BLOCK_LEN);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double secs_portable = (double)(t1.tv_sec - t0.tv_sec) +
                               (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

        /* Crypto-accelerated timing: feed the full buffer per iteration
         * as a single bulk call. */
        uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (uint32_t i = 0; i < iters; i++) {
            sha256ArmCryptoCompressBlocks(state, buf,
                                          bufSize / SHA256_BLOCK_LEN);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double secs_crypto = (double)(t1.tv_sec - t0.tv_sec) +
                             (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

        double mib = (double)((uint64_t)bufSize * iters) / (1024.0 * 1024.0);
        double portable_rate = secs_portable > 0 ? mib / secs_portable : 0.0;
        double crypto_rate = secs_crypto > 0 ? mib / secs_crypto : 0.0;

        printf("    portable: %.2f MiB in %.3fs = %.2f MiB/s\n", mib,
               secs_portable, portable_rate);
        printf("    crypto:   %.2f MiB in %.3fs = %.2f MiB/s\n", mib,
               secs_crypto, crypto_rate);
        if (portable_rate > 0 && crypto_rate > 0) {
            printf("    speedup:  %.2fx\n", crypto_rate / portable_rate);
        }

        /* Same input, same block count: both paths must agree, and using
         * both states keeps the optimizer from eliding either loop. */
        if (memcmp(state, portableState, sizeof(state)) != 0) {
            ERRR("portable and crypto compress disagree on the bench input");
        }
    }

    TEST("multi-buffer 4-lane matches portable on random inputs") {
        const uint32_t iters = 5000;
        uint64_t rng = 0xA5A5C0FFEE12345ULL;
        uint32_t mismatches = 0;

        const size_t max_blocks = 16;
        const size_t max_bytes = max_blocks * SHA256_BLOCK_LEN;
        uint8_t *bufs[4];
        for (uint32_t lane = 0; lane < 4; lane++) {
            /* @own: container; free=zfree */
            bufs[lane] = zmalloc(max_bytes);
        }

        for (uint32_t t = 0; t < iters; t++) {
            rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
            size_t nb = 1 + (size_t)((rng >> 33) % max_blocks);
            size_t nbytes = nb * SHA256_BLOCK_LEN;

            for (uint32_t lane = 0; lane < 4; lane++) {
                for (size_t i = 0; i < nbytes; i++) {
                    rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
                    bufs[lane][i] = (uint8_t)(rng >> 56);
                }
            }

            /* Reference: full hash per lane via the single-buffer path. */
            uint8_t ref[4][SHA256_DIGEST_LEN];
            for (uint32_t lane = 0; lane < 4; lane++) {
                sha256ArmCryptoHash(bufs[lane], nbytes, ref[lane]);
            }

            /* Multi-buffer: compress through the 4-lane path, then run
             * the standard padding tail through the single-buffer path
             * to produce the final digest.  Because the input is an
             * exact multiple of SHA256_BLOCK_LEN, the tail is purely the
             * 0x80 + zero-padding + length block. */
            uint32_t st[4][8];
            for (uint32_t lane = 0; lane < 4; lane++) {
                st[lane][0] = 0x6a09e667;
                st[lane][1] = 0xbb67ae85;
                st[lane][2] = 0x3c6ef372;
                st[lane][3] = 0xa54ff53a;
                st[lane][4] = 0x510e527f;
                st[lane][5] = 0x9b05688c;
                st[lane][6] = 0x1f83d9ab;
                st[lane][7] = 0x5be0cd19;
            }
            const uint8_t *bptrs[4] = {bufs[0], bufs[1], bufs[2], bufs[3]};
            sha256ArmCryptoCompressBlocksMulti4(st, bptrs, nb);

            for (uint32_t lane = 0; lane < 4; lane++) {
                uint8_t tail[SHA256_BLOCK_LEN] = {0};
                tail[0] = 0x80;
                uint64_t bits = (uint64_t)nbytes * 8;
                tail[SHA256_BLOCK_LEN - 8] = (uint8_t)(bits >> 56);
                tail[SHA256_BLOCK_LEN - 7] = (uint8_t)(bits >> 48);
                tail[SHA256_BLOCK_LEN - 6] = (uint8_t)(bits >> 40);
                tail[SHA256_BLOCK_LEN - 5] = (uint8_t)(bits >> 32);
                tail[SHA256_BLOCK_LEN - 4] = (uint8_t)(bits >> 24);
                tail[SHA256_BLOCK_LEN - 3] = (uint8_t)(bits >> 16);
                tail[SHA256_BLOCK_LEN - 2] = (uint8_t)(bits >> 8);
                tail[SHA256_BLOCK_LEN - 1] = (uint8_t)bits;
                sha256ArmCryptoCompressBlocks(st[lane], tail, 1);

                uint8_t got[SHA256_DIGEST_LEN];
                for (uint32_t i = 0; i < 8; i++) {
                    uint32_t v = st[lane][i];
                    got[i * 4 + 0] = (uint8_t)(v >> 24);
                    got[i * 4 + 1] = (uint8_t)(v >> 16);
                    got[i * 4 + 2] = (uint8_t)(v >> 8);
                    got[i * 4 + 3] = (uint8_t)v;
                }

                if (!hashEqual(got, ref[lane])) {
                    mismatches++;
                    if (mismatches <= 3) {
                        char gh[65], eh[65];
                        hashToHex(got, gh);
                        hashToHex(ref[lane], eh);
                        ERR("multi4 mismatch iter=%u lane=%u nb=%zu: "
                            "got %s, ref %s",
                            t, lane, nb, gh, eh);
                    }
                }
            }
        }

        for (uint32_t lane = 0; lane < 4; lane++) {
            zfree(bufs[lane]);
        }

        if (mismatches == 0) {
            printf("    %u iterations, 4 lanes each: all match\n", iters);
        } else {
            ERR("%u total mismatches over %u iterations", mismatches, iters);
        }
    }

    TEST("sha256 arm crypto multi-buffer performance bench") {
        const size_t bufSize = 64u * 1024u; /* 64 KiB per lane */
        const uint32_t iters = 4096;
        const size_t nb = bufSize / SHA256_BLOCK_LEN;

        uint8_t *bufs[4];
        for (uint32_t lane = 0; lane < 4; lane++) {
            /* @own: container; free=zfree */
            bufs[lane] = zmalloc(bufSize);
            memset(bufs[lane], (int)(0x10u + lane), bufSize);
        }

        uint32_t st1[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        uint32_t st4[4][8];
        for (uint32_t lane = 0; lane < 4; lane++) {
            memcpy(st4[lane], st1, sizeof(st1));
        }

        struct timespec t0, t1;

        /* Single-buffer: four serial calls per iteration. */
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (uint32_t i = 0; i < iters; i++) {
            for (uint32_t lane = 0; lane < 4; lane++) {
                sha256ArmCryptoCompressBlocks(st4[lane], bufs[lane], nb);
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double secs_single = (double)(t1.tv_sec - t0.tv_sec) +
                             (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

        /* Reset state. */
        for (uint32_t lane = 0; lane < 4; lane++) {
            memcpy(st4[lane], st1, sizeof(st1));
        }

        /* Multi-buffer 4-lane. */
        const uint8_t *bptrs[4] = {bufs[0], bufs[1], bufs[2], bufs[3]};
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (uint32_t i = 0; i < iters; i++) {
            sha256ArmCryptoCompressBlocksMulti4(st4, bptrs, nb);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double secs_multi = (double)(t1.tv_sec - t0.tv_sec) +
                            (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

        const double total_mib =
            (double)((uint64_t)bufSize * iters * 4u) / (1024.0 * 1024.0);
        const double rate_single =
            secs_single > 0 ? total_mib / secs_single : 0.0;
        const double rate_multi = secs_multi > 0 ? total_mib / secs_multi : 0.0;

        printf("    single x4: %.2f MiB in %.3fs = %.2f MiB/s\n", total_mib,
               secs_single, rate_single);
        printf("    multi4:    %.2f MiB in %.3fs = %.2f MiB/s\n", total_mib,
               secs_multi, rate_multi);
        if (rate_single > 0 && rate_multi > 0) {
            printf("    speedup:   %.2fx\n", rate_multi / rate_single);
        }

        /* Anti-DCE. */
        if ((st4[0][0] | st4[3][7]) == 0xFFFFFFFF) {
            printf("    (anti-DCE: %08x)\n", st4[0][0]);
        }

        for (uint32_t lane = 0; lane < 4; lane++) {
            zfree(bufs[lane]);
        }
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
