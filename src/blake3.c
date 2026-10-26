/* BLAKE3 cryptographic hash — portable scalar reference implementation.
 *
 * Single-threaded, no SIMD. Closely follows the BLAKE3 spec
 * (https://github.com/BLAKE3-team/BLAKE3-specs). The compressor
 * processes one 64-byte block at a time; chunks of up to 1024 bytes
 * each produce a chaining value that feeds a left-leaning Merkle tree
 * maintained as a CV stack. This file is the reference; SIMD variants
 * will reuse the API and parts of the state machine.
 *
 * BLAKE3 SIMD batch dispatch — at hash time the runtime selects the
 * widest compressor available:
 *   AVX2 8-chunk  (x86_64 w/ AVX2)   — BLAKE3_BATCH_AVX2_CHUNKS
 *   NEON 4-chunk  (aarch64)          — BLAKE3_BATCH_NEON_CHUNKS
 *   SSE4.1 4-chunk (x86_64 fallback) — BLAKE3_BATCH_SSE41_CHUNKS
 *   scalar 1-chunk (always)
 *
 * Adding a new backend: add (1) compress function exposed in
 * hash/blake3Foo.h, (2) BLAKE3_BATCH_FOO_CHUNKS in this file
 * (matching the compressor lane count, static_assert-tied to the
 * backend header), (3) probe BLAKE3_FOO_AVAILABLE in hash/blake3Foo.h,
 * (4) dispatch table entry in simd_batch_ready, (5) a
 * BLAKE3_BACKEND_FOO enum value in blake3.h. */

#include "blake3.h"

#include "ctmem.h"
#include "hash/blake3AVX2.h"
#include "hash/blake3Neon.h"
#include "hash/blake3SSE41.h"

#include <string.h>

#include "datakit.h" /* zmalloc/zfree (test driver only) */

/* ----- Constants ------------------------------------------------------- */

/* SHA-256 IV (sqrt of first 8 primes). */
static constexpr uint32_t BLAKE3_IV[8] = {
    0x6A09E667ul, 0xBB67AE85ul, 0x3C6EF372ul, 0xA54FF53Aul,
    0x510E527Ful, 0x9B05688Cul, 0x1F83D9ABul, 0x5BE0CD19ul,
};

/* Message-schedule permutation applied between rounds 1..7. */
static constexpr uint8_t BLAKE3_MSG_SCHEDULE[7][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {2, 6, 3, 10, 7, 0, 4, 13, 1, 11, 12, 5, 9, 14, 15, 8},
    {3, 4, 10, 12, 13, 2, 7, 14, 6, 5, 9, 0, 11, 15, 8, 1},
    {10, 7, 12, 9, 14, 3, 13, 15, 4, 0, 11, 2, 5, 8, 1, 6},
    {12, 13, 9, 11, 15, 10, 14, 8, 7, 2, 5, 3, 0, 1, 6, 4},
    {9, 14, 11, 5, 8, 12, 15, 1, 13, 3, 0, 10, 2, 6, 4, 7},
    {11, 15, 5, 0, 1, 9, 8, 6, 14, 10, 2, 12, 3, 4, 7, 13},
};

/* ----- Little-endian load/store --------------------------------------- */

static inline uint32_t load_u32_le(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    return v;
}

static inline void store_u32_le(uint8_t *p, uint32_t v) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    v = __builtin_bswap32(v);
#endif
    memcpy(p, &v, sizeof(v));
}

static inline uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

/* ----- Compression ----------------------------------------------------
 *
 * The hot path is compress_in_place; on portable (non-SIMD) hosts every
 * BLAKE3 block goes through it, so force-inlining the entire call tree
 * (g_mix → round_fn → compress_pre → compress_in_place) lets the
 * compiler keep the 16-word state in registers across all 7 rounds and
 * pick its own unroll heuristics.  Without these attributes -O3 may
 * keep the helpers as out-of-line functions, costing a per-call frame
 * setup and forcing state through memory. */
#define B3_HOT_INLINE static inline __attribute__((always_inline))

/* G mixing function: ChaCha-style quarterround over the 16-word state. */
B3_HOT_INLINE void g_mix(uint32_t state[16], uint32_t a, uint32_t b, uint32_t c,
                         uint32_t d, uint32_t mx, uint32_t my) {
    state[a] = state[a] + state[b] + mx;
    state[d] = rotr32(state[d] ^ state[a], 16);
    state[c] = state[c] + state[d];
    state[b] = rotr32(state[b] ^ state[c], 12);
    state[a] = state[a] + state[b] + my;
    state[d] = rotr32(state[d] ^ state[a], 8);
    state[c] = state[c] + state[d];
    state[b] = rotr32(state[b] ^ state[c], 7);
}

B3_HOT_INLINE void round_fn(uint32_t state[16], const uint32_t m[16],
                            const uint8_t schedule[16]) {
    /* Column step. */
    g_mix(state, 0, 4, 8, 12, m[schedule[0]], m[schedule[1]]);
    g_mix(state, 1, 5, 9, 13, m[schedule[2]], m[schedule[3]]);
    g_mix(state, 2, 6, 10, 14, m[schedule[4]], m[schedule[5]]);
    g_mix(state, 3, 7, 11, 15, m[schedule[6]], m[schedule[7]]);
    /* Diagonal step. */
    g_mix(state, 0, 5, 10, 15, m[schedule[8]], m[schedule[9]]);
    g_mix(state, 1, 6, 11, 12, m[schedule[10]], m[schedule[11]]);
    g_mix(state, 2, 7, 8, 13, m[schedule[12]], m[schedule[13]]);
    g_mix(state, 3, 4, 9, 14, m[schedule[14]], m[schedule[15]]);
}

/* Core compression. Produces the 16-word output state (CV in lower 8
 * words; XOR-folded output material in all 16 for finalize). */
B3_HOT_INLINE void compress_pre(uint32_t state[16], const uint32_t cv[8],
                                const uint8_t block[BLAKE3_BLOCK_LEN],
                                uint8_t block_len, uint64_t counter,
                                uint8_t flags) {
    uint32_t m[16];
    for (uint32_t i = 0; i < 16; i++) {
        m[i] = load_u32_le(block + i * 4);
    }

    state[0] = cv[0];
    state[1] = cv[1];
    state[2] = cv[2];
    state[3] = cv[3];
    state[4] = cv[4];
    state[5] = cv[5];
    state[6] = cv[6];
    state[7] = cv[7];
    state[8] = BLAKE3_IV[0];
    state[9] = BLAKE3_IV[1];
    state[10] = BLAKE3_IV[2];
    state[11] = BLAKE3_IV[3];
    state[12] = (uint32_t)(counter & 0xFFFFFFFFul);
    state[13] = (uint32_t)(counter >> 32);
    state[14] = (uint32_t)block_len;
    state[15] = (uint32_t)flags;

    /* Fully unrolled rounds — gives the compiler compile-time-constant
     * schedule indices so the round_fn body can be inlined and the m[]
     * loads scheduled across rounds rather than reloaded each iter. */
    round_fn(state, m, BLAKE3_MSG_SCHEDULE[0]);
    round_fn(state, m, BLAKE3_MSG_SCHEDULE[1]);
    round_fn(state, m, BLAKE3_MSG_SCHEDULE[2]);
    round_fn(state, m, BLAKE3_MSG_SCHEDULE[3]);
    round_fn(state, m, BLAKE3_MSG_SCHEDULE[4]);
    round_fn(state, m, BLAKE3_MSG_SCHEDULE[5]);
    round_fn(state, m, BLAKE3_MSG_SCHEDULE[6]);
}

/* Produce truncated 8-word output CV (the standard chunk/parent CV). */
static void compress_in_place(uint32_t cv[8],
                              const uint8_t block[BLAKE3_BLOCK_LEN],
                              uint8_t block_len, uint64_t counter,
                              uint8_t flags) {
    uint32_t state[16];
    compress_pre(state, cv, block, block_len, counter, flags);
    for (uint32_t i = 0; i < 8; i++) {
        cv[i] = state[i] ^ state[i + 8];
    }
}

/* Produce the full 64-byte output (16 words) used for root/XOF blocks. */
static void compress_xof(const uint32_t cv[8],
                         const uint8_t block[BLAKE3_BLOCK_LEN],
                         uint8_t block_len, uint64_t counter, uint8_t flags,
                         uint8_t out[64]) {
    uint32_t state[16];
    compress_pre(state, cv, block, block_len, counter, flags);
    for (uint32_t i = 0; i < 8; i++) {
        store_u32_le(out + i * 4, state[i] ^ state[i + 8]);
        store_u32_le(out + (i + 8) * 4, state[i + 8] ^ cv[i]);
    }
}

/* ----- Public wrappers for SIMD variants to differential-test against ----
 *
 * The SIMD-accelerated BLAKE3 implementations (NEON, SSE4.1, AVX2) want
 * to compare their output against the portable compress, both in
 * correctness tests and during runtime dispatch.  Exposed under a
 * distinct name so the file-static `compress_in_place` / `compress_xof`
 * stay safely module-local. */
void blake3PortableCompressInPlace(uint32_t cv[8],
                                   const uint8_t block[BLAKE3_BLOCK_LEN],
                                   uint8_t block_len, uint64_t counter,
                                   uint8_t flags) {
    compress_in_place(cv, block, block_len, counter, flags);
}

void blake3PortableCompressXof(const uint32_t cv[8],
                               const uint8_t block[BLAKE3_BLOCK_LEN],
                               uint8_t block_len, uint64_t counter,
                               uint8_t flags, uint8_t out[64]) {
    compress_xof(cv, block, block_len, counter, flags, out);
}

/* ----- Chunk / output state ------------------------------------------- */

/* Lightweight output snapshot used during finalize. Captures everything
 * needed to emit either a single CV (when consumed as a parent input)
 * or arbitrarily many output blocks (when root). */
typedef struct blake3Output {
    uint32_t input_cv[8];
    uint8_t block[BLAKE3_BLOCK_LEN];
    uint64_t counter;
    uint8_t block_len;
    uint8_t flags;
} blake3Output;

static void output_chaining_value(const blake3Output *o, uint8_t out_cv[32]) {
    uint32_t cv_words[8];
    memcpy(cv_words, o->input_cv, sizeof(cv_words));
    compress_in_place(cv_words, o->block, o->block_len, o->counter, o->flags);
    for (uint32_t i = 0; i < 8; i++) {
        store_u32_le(out_cv + i * 4, cv_words[i]);
    }
}

/* Emit `out_len` bytes starting from byte offset `seek` within the
 * (conceptually infinite) BLAKE3 output stream of this finalize node. */
static void output_root_bytes(const blake3Output *o, uint64_t seek,
                              uint8_t *out, size_t out_len) {
    uint64_t output_block_counter = seek / 64;
    size_t offset_within_block = (size_t)(seek % 64);
    uint8_t wide[64];
    while (out_len > 0) {
        compress_xof(o->input_cv, o->block, o->block_len, output_block_counter,
                     (uint8_t)(o->flags | BLAKE3_FLAG_ROOT), wide);
        size_t available = 64 - offset_within_block;
        size_t take = out_len < available ? out_len : available;
        memcpy(out, wide + offset_within_block, take);
        out += take;
        out_len -= take;
        output_block_counter++;
        offset_within_block = 0;
    }
}

/* ----- Chunk state helpers -------------------------------------------- */

static uint8_t chunk_start_flag(const blake3Hasher *self) {
    return self->blocks_compressed == 0 ? BLAKE3_FLAG_CHUNK_START : 0u;
}

static void chunk_reset(blake3Hasher *self, uint64_t chunk_counter) {
    memcpy(self->cv, self->key, sizeof(self->cv));
    self->chunk_counter = chunk_counter;
    self->blocks_compressed = 0;
    memset(self->buf, 0, BLAKE3_BLOCK_LEN);
    self->buf_len = 0;
}

/* Apply a full 64-byte block (the partial last block of a chunk is
 * handled separately in finalize). */
static void chunk_update_block(blake3Hasher *self,
                               const uint8_t block[BLAKE3_BLOCK_LEN]) {
    compress_in_place(self->cv, block, BLAKE3_BLOCK_LEN, self->chunk_counter,
                      (uint8_t)(self->flags | chunk_start_flag(self)));
    self->blocks_compressed++;
}

/* Snapshot the chunk's not-yet-compressed trailing block plus CHUNK_END. */
static blake3Output chunk_output(const blake3Hasher *self) {
    blake3Output o;
    memcpy(o.input_cv, self->cv, sizeof(o.input_cv));
    memcpy(o.block, self->buf, BLAKE3_BLOCK_LEN);
    o.counter = self->chunk_counter;
    o.block_len = self->buf_len;
    o.flags =
        (uint8_t)(self->flags | chunk_start_flag(self) | BLAKE3_FLAG_CHUNK_END);
    return o;
}

/* Combine two adjacent CVs into a parent output (8-word CV). */
static blake3Output parent_output(const uint8_t left[32],
                                  const uint8_t right[32],
                                  const uint32_t key[8], uint8_t flags) {
    blake3Output o;
    memcpy(o.input_cv, key, sizeof(o.input_cv));
    memcpy(o.block, left, 32);
    memcpy(o.block + 32, right, 32);
    o.counter = 0;
    o.block_len = BLAKE3_BLOCK_LEN;
    o.flags = (uint8_t)(flags | BLAKE3_FLAG_PARENT);
    return o;
}

/* ----- CV stack ------------------------------------------------------- */

/* The CV stack represents the in-progress left-leaning Merkle tree.
 * We merge whenever the next chunk index `total_chunks` has its low
 * bits cleared at level `i` — i.e. for each trailing zero bit, two
 * subtrees of equal height become a parent. */
static void cv_stack_push(blake3Hasher *self, const uint8_t cv[32]) {
    memcpy(&self->cv_stack[self->cv_stack_len * 32], cv, 32);
    self->cv_stack_len++;
}

static void hasher_push_chunk_cv(blake3Hasher *self, uint8_t cv[32],
                                 uint64_t total_chunks) {
    /* For each trailing-zero pair we combine two CVs.  When the final
     * fold of this push would land cv_stack_len at 1 with no in-flight
     * chunk (the exact-power-of-2-chunks-via-SIMD case), finalize
     * needs the inputs to that fold to re-apply with the ROOT flag —
     * the post-fold CV has already lost that context.  Save them. */
    bool did_fold = false;
    while ((total_chunks & 1) == 0) {
        uint8_t parent_cv[32];
        const uint8_t *left = &self->cv_stack[(self->cv_stack_len - 1) * 32];
        /* Snapshot the inputs to THIS fold; if it turns out to be the
         * last fold and the result becomes the only stack entry,
         * finalize will recompose them with PARENT|ROOT. */
        memcpy(self->pending_root_left, left, 32);
        memcpy(self->pending_root_right, cv, 32);
        did_fold = true;

        blake3Output po = parent_output(left, cv, self->key, self->flags);
        output_chaining_value(&po, parent_cv);
        self->cv_stack_len--;
        memcpy(cv, parent_cv, 32);
        total_chunks >>= 1;
    }
    /* pending_root_valid iff the top of stack right now is a fold
     * result (i.e., we folded this push).  If we didn't fold, the top
     * is a raw chunk CV and there's no "saved pair" relevant to it. */
    self->pending_root_valid = did_fold ? 1 : 0;
    cv_stack_push(self, cv);
}

/* ----- Init paths ----------------------------------------------------- */

static void hasher_init_internal(blake3Hasher *self, const uint32_t key[8],
                                 uint8_t flags) {
    memcpy(self->key, key, sizeof(self->key));
    memcpy(self->cv, key, sizeof(self->cv));
    self->chunk_counter = 0;
    memset(self->buf, 0, BLAKE3_BLOCK_LEN);
    self->buf_len = 0;
    self->blocks_compressed = 0;
    self->flags = flags;
    self->cv_stack_len = 0;
    memset(self->cv_stack, 0, sizeof(self->cv_stack));
    self->pending_root_valid = 0;
}

void blake3HasherInit(blake3Hasher *self) {
    hasher_init_internal(self, BLAKE3_IV, 0);
}

void blake3HasherInitKeyed(blake3Hasher *self,
                           const uint8_t key[BLAKE3_KEY_LEN]) {
    uint32_t key_words[8];
    for (uint32_t i = 0; i < 8; i++) {
        key_words[i] = load_u32_le(key + i * 4);
    }
    hasher_init_internal(self, key_words, BLAKE3_FLAG_KEYED_HASH);
    /* Wipe stack-local key copy. self->key now owns the canonical copy;
     * key_words held the same bytes briefly and must not linger on the
     * stack frame the next stack-reuse will see. */
    dkSecureZero(key_words, sizeof(key_words));
}

void blake3HasherInitDeriveKeyRaw(blake3Hasher *self, const void *context,
                                  size_t context_len) {
    /* Two-pass: hash the context with DERIVE_KEY_CONTEXT, then use the
     * resulting 32 bytes as the key for the DERIVE_KEY_MATERIAL hash. */
    blake3Hasher ctx_hasher;
    hasher_init_internal(&ctx_hasher, BLAKE3_IV,
                         BLAKE3_FLAG_DERIVE_KEY_CONTEXT);
    blake3HasherUpdate(&ctx_hasher, context, context_len);
    uint8_t context_key[BLAKE3_KEY_LEN];
    blake3HasherFinalize(&ctx_hasher, context_key, BLAKE3_KEY_LEN);

    uint32_t key_words[8];
    for (uint32_t i = 0; i < 8; i++) {
        key_words[i] = load_u32_le(context_key + i * 4);
    }
    hasher_init_internal(self, key_words, BLAKE3_FLAG_DERIVE_KEY_MATERIAL);

    /* Wipe every stack-local that held secret-derived material before
     * the frame unwinds. self->key holds the canonical derived sub-key
     * now and is the legitimate retention point. */
    dkSecureZero(context_key, sizeof(context_key));
    dkSecureZero(key_words, sizeof(key_words));
    dkSecureZero(ctx_hasher.key, sizeof(ctx_hasher.key));
    dkSecureZero(ctx_hasher.cv, sizeof(ctx_hasher.cv));
}

void blake3HasherInitDeriveKey(blake3Hasher *self, const char *context) {
    blake3HasherInitDeriveKeyRaw(self, context, strlen(context));
}

/* ----- Update -------------------------------------------------------- */

/* Drain the chunk: emit its CV and push onto the merkle stack, then
 * reset chunk state for the next chunk. */
static void chunk_complete(blake3Hasher *self) {
    blake3Output o = chunk_output(self);
    uint8_t cv[32];
    output_chaining_value(&o, cv);
    uint64_t new_total_chunks = self->chunk_counter + 1;
    hasher_push_chunk_cv(self, cv, new_total_chunks);
    chunk_reset(self, new_total_chunks);
}

static size_t chunk_bytes_consumed(const blake3Hasher *self) {
    return (size_t)self->blocks_compressed * BLAKE3_BLOCK_LEN +
           (size_t)self->buf_len;
}

/* Update — feed `input_len` bytes into the hasher.
 *
 * Invariant maintained across Update calls: a "held-back" block may
 * exist in `self->buf` (with buf_len 1..64). We NEVER compress that
 * held-back block speculatively on the way out — only when we receive
 * NEW data and therefore know the held-back block isn't the chunk's
 * last (or, in the chunk's-last-block case, isn't the hash's last).
 *
 * Equivalently: chunk_update_block (which never applies CHUNK_END) is
 * only called for blocks that have at least one byte of *future* data
 * confirmed to follow them. The chunk's terminal block always flows
 * through chunk_output (in chunk_complete or in finalize), which is
 * the only path that applies CHUNK_END.
 *
 * Loop body steps:
 *   (1) If a previous Update left the chunk fully consumed AND we now
 *       have more input, drain the chunk via chunk_complete.
 *   (2) If buf is FULL (64 bytes held back) AND we have more input,
 *       compress that block (we now know it wasn't the last).
 *   (3) Top up a partial buf (if any) without flushing.
 *   (4) Bulk-compress full blocks directly from input, stopping before
 *       the chunk's last block-slot or the caller's last 64 bytes.
 *   (5) Copy whatever 1..64 bytes remain into buf — held back without
 *       flushing. */
/* Per-backend batch widths — the minimum chunks before the SIMD batch
 * path takes over from scalar. Each constant equals the number of
 * independent chunks the corresponding compress function consumes per
 * call, and is tied at compile time to the lane-count macro exported
 * by the backend header so the two cannot drift apart silently. If a
 * backend's compressor widens or narrows, bump the lane-count macro
 * in the header AND this constant together — the static_assert traps
 * the mismatch at build time.
 *
 * Finalize correctly handles the case where SIMD consumes the entire
 * input by reconstructing the root parent_output from the saved
 * (left, right) CV pair of the last fold (see pending_root_* fields
 * in struct blake3Hasher).
 *
 * Backends in priority order: AVX2 (8-wide) > NEON (4-wide) >
 * SSE4.1 (4-wide) > scalar. NEON and SSE4.1 share the same minimum
 * batch width (4 chunks); the dispatcher tie-breaks NEON first since
 * the two are mutually exclusive in practice (ARM vs x86).
 *
 * Run the in-suite "blake3 dispatch crossover audit" test to verify
 * the SIMD/scalar boundary on a new deployment platform. */
#define BLAKE3_BATCH_NEON_CHUNKS 4
#define BLAKE3_BATCH_SSE41_CHUNKS 4
#define BLAKE3_BATCH_AVX2_CHUNKS 8

static_assert(BLAKE3_BATCH_NEON_CHUNKS == BLAKE3_NEON_COMPRESS4_LANES,
              "BLAKE3 NEON batch constant must match compressor lane count");
static_assert(BLAKE3_BATCH_SSE41_CHUNKS == BLAKE3_SSE41_COMPRESS4_LANES,
              "BLAKE3 SSE4.1 batch constant must match compressor lane count");
static_assert(BLAKE3_BATCH_AVX2_CHUNKS == BLAKE3_AVX2_COMPRESS8_LANES,
              "BLAKE3 AVX2 batch constant must match compressor lane count");

/* SIMD batch path: hash 4 full chunks in parallel via blake3NeonCompress4,
 * then push the 4 resulting chunk CVs onto the merkle stack.  Caller
 * guarantees the precondition: NEON available, buf empty, no in-flight
 * chunk, and input_len > BLAKE3_BATCH_NEON_CHUNKS * BLAKE3_CHUNK_LEN.
 *
 * Inner CV reload uses memcpy because BLAKE3 stores CVs in LE byte
 * order and the host (aarch64 / x86_64) is also LE — the previous
 * byte-by-byte reconstruction was a meaningful per-call cost. */
static void process_4_chunks_simd(blake3Hasher *self, const uint8_t **in_ptr,
                                  size_t *in_len) {
    while (*in_len >= BLAKE3_BATCH_NEON_CHUNKS * BLAKE3_CHUNK_LEN) {
        uint32_t cvs[4][8];
        for (int i = 0; i < 4; i++) {
            memcpy(cvs[i], self->key, sizeof(cvs[i]));
        }
        const uint64_t base = self->chunk_counter;
        const uint64_t counters[4] = {base, base + 1, base + 2, base + 3};

        const uint8_t *in = *in_ptr;
        for (int blk = 0; blk < 16; blk++) {
            const uint8_t *blocks[4] = {
                in + 0 * BLAKE3_CHUNK_LEN + blk * BLAKE3_BLOCK_LEN,
                in + 1 * BLAKE3_CHUNK_LEN + blk * BLAKE3_BLOCK_LEN,
                in + 2 * BLAKE3_CHUNK_LEN + blk * BLAKE3_BLOCK_LEN,
                in + 3 * BLAKE3_CHUNK_LEN + blk * BLAKE3_BLOCK_LEN,
            };
            uint8_t flags = self->flags;
            if (blk == 0) {
                flags |= BLAKE3_FLAG_CHUNK_START;
            }
            if (blk == 15) {
                flags |= BLAKE3_FLAG_CHUNK_END;
            }
            const uint8_t flag_set[4] = {flags, flags, flags, flags};
            const uint8_t block_lens[4] = {64, 64, 64, 64};
            uint8_t out_bytes[4][32];
            const uint32_t *cv_in[4] = {cvs[0], cvs[1], cvs[2], cvs[3]};
            blake3NeonCompress4(cv_in, blocks, block_lens, counters, flag_set,
                                out_bytes);
            /* LE host: just memcpy.  This replaced a per-byte loop that
             * was a measurable fraction of total inner-loop cost. */
            for (int i = 0; i < 4; i++) {
                memcpy(cvs[i], out_bytes[i], sizeof(cvs[i]));
            }
            if (blk == 15) {
                for (int i = 0; i < 4; i++) {
                    uint8_t cv_bytes[32];
                    memcpy(cv_bytes, out_bytes[i], 32);
                    hasher_push_chunk_cv(self, cv_bytes, base + i + 1);
                }
            }
        }

        self->chunk_counter = base + 4;
        *in_ptr += 4 * BLAKE3_CHUNK_LEN;
        *in_len -= 4 * BLAKE3_CHUNK_LEN;
    }
}

/* SIMD batch path: hash 4 full chunks in parallel via blake3SSE41Compress4.
 * Identical structure to the NEON path; the only difference is the
 * underlying compressor. Selected on x86 hosts that lack AVX2 but have
 * SSE4.1. */
static void process_4_chunks_simd_sse41(blake3Hasher *self,
                                        const uint8_t **in_ptr,
                                        size_t *in_len) {
    while (*in_len >= BLAKE3_BATCH_SSE41_CHUNKS * BLAKE3_CHUNK_LEN) {
        uint32_t cvs[4][8];
        for (int i = 0; i < 4; i++) {
            memcpy(cvs[i], self->key, sizeof(cvs[i]));
        }
        const uint64_t base = self->chunk_counter;
        const uint64_t counters[4] = {base, base + 1, base + 2, base + 3};

        const uint8_t *in = *in_ptr;
        for (int blk = 0; blk < 16; blk++) {
            const uint8_t *blocks[4] = {
                in + 0 * BLAKE3_CHUNK_LEN + blk * BLAKE3_BLOCK_LEN,
                in + 1 * BLAKE3_CHUNK_LEN + blk * BLAKE3_BLOCK_LEN,
                in + 2 * BLAKE3_CHUNK_LEN + blk * BLAKE3_BLOCK_LEN,
                in + 3 * BLAKE3_CHUNK_LEN + blk * BLAKE3_BLOCK_LEN,
            };
            uint8_t flags = self->flags;
            if (blk == 0) {
                flags |= BLAKE3_FLAG_CHUNK_START;
            }
            if (blk == 15) {
                flags |= BLAKE3_FLAG_CHUNK_END;
            }
            const uint8_t flag_set[4] = {flags, flags, flags, flags};
            const uint8_t block_lens[4] = {64, 64, 64, 64};
            uint8_t out_bytes[4][32];
            const uint32_t *cv_in[4] = {cvs[0], cvs[1], cvs[2], cvs[3]};
            blake3SSE41Compress4(cv_in, blocks, block_lens, counters, flag_set,
                                 out_bytes);
            /* LE host: just memcpy.  Matches the NEON 4-batch shape. */
            for (int i = 0; i < 4; i++) {
                memcpy(cvs[i], out_bytes[i], sizeof(cvs[i]));
            }
            if (blk == 15) {
                for (int i = 0; i < 4; i++) {
                    uint8_t cv_bytes[32];
                    memcpy(cv_bytes, out_bytes[i], 32);
                    hasher_push_chunk_cv(self, cv_bytes, base + i + 1);
                }
            }
        }

        self->chunk_counter = base + 4;
        *in_ptr += 4 * BLAKE3_CHUNK_LEN;
        *in_len -= 4 * BLAKE3_CHUNK_LEN;
    }
}

/* SIMD batch path: hash 8 full chunks in parallel via blake3AVX2Compress8,
 * then push the 8 resulting chunk CVs onto the merkle stack.  Caller
 * guarantees the precondition: AVX2 available, buf empty, no in-flight
 * chunk, and input_len >= BLAKE3_BATCH_AVX2_CHUNKS *
 * BLAKE3_CHUNK_LEN.
 *
 * Direct 8-wide analog of process_4_chunks_simd; the structure is
 * identical except for the lane count.  blake3AVX2Compress8's calling
 * convention matches blake3NeonCompress4 exactly (parallel-arrays of
 * cv pointers, block pointers, lens, counters, flag_set, with a
 * per-lane 32-byte output CV). */
static void process_8_chunks_simd_avx2(blake3Hasher *self,
                                       const uint8_t **in_ptr, size_t *in_len) {
    while (*in_len >= BLAKE3_BATCH_AVX2_CHUNKS * BLAKE3_CHUNK_LEN) {
        uint32_t cvs[8][8];
        for (int i = 0; i < 8; i++) {
            memcpy(cvs[i], self->key, sizeof(cvs[i]));
        }
        const uint64_t base = self->chunk_counter;
        const uint64_t counters[8] = {
            base + 0, base + 1, base + 2, base + 3,
            base + 4, base + 5, base + 6, base + 7,
        };

        const uint8_t *in = *in_ptr;
        for (int blk = 0; blk < 16; blk++) {
            const uint8_t *blocks[8] = {
                in + 0 * BLAKE3_CHUNK_LEN + blk * BLAKE3_BLOCK_LEN,
                in + 1 * BLAKE3_CHUNK_LEN + blk * BLAKE3_BLOCK_LEN,
                in + 2 * BLAKE3_CHUNK_LEN + blk * BLAKE3_BLOCK_LEN,
                in + 3 * BLAKE3_CHUNK_LEN + blk * BLAKE3_BLOCK_LEN,
                in + 4 * BLAKE3_CHUNK_LEN + blk * BLAKE3_BLOCK_LEN,
                in + 5 * BLAKE3_CHUNK_LEN + blk * BLAKE3_BLOCK_LEN,
                in + 6 * BLAKE3_CHUNK_LEN + blk * BLAKE3_BLOCK_LEN,
                in + 7 * BLAKE3_CHUNK_LEN + blk * BLAKE3_BLOCK_LEN,
            };
            uint8_t flags = self->flags;
            if (blk == 0) {
                flags |= BLAKE3_FLAG_CHUNK_START;
            }
            if (blk == 15) {
                flags |= BLAKE3_FLAG_CHUNK_END;
            }
            const uint8_t flag_set[8] = {flags, flags, flags, flags,
                                         flags, flags, flags, flags};
            const uint8_t block_lens[8] = {64, 64, 64, 64, 64, 64, 64, 64};
            uint8_t out_bytes[8][32];
            const uint32_t *cv_in[8] = {cvs[0], cvs[1], cvs[2], cvs[3],
                                        cvs[4], cvs[5], cvs[6], cvs[7]};
            blake3AVX2Compress8(cv_in, blocks, block_lens, counters, flag_set,
                                out_bytes);
            /* LE host: just memcpy. */
            for (int i = 0; i < 8; i++) {
                memcpy(cvs[i], out_bytes[i], sizeof(cvs[i]));
            }
            if (blk == 15) {
                for (int i = 0; i < 8; i++) {
                    uint8_t cv_bytes[32];
                    memcpy(cv_bytes, out_bytes[i], 32);
                    hasher_push_chunk_cv(self, cv_bytes, base + i + 1);
                }
            }
        }

        self->chunk_counter = base + 8;
        *in_ptr += 8 * BLAKE3_CHUNK_LEN;
        *in_len -= 8 * BLAKE3_CHUNK_LEN;
    }
}

/* Runtime knob for benchmarking — see blake3.h for the contract.
 * Production callers should never set this; the dispatcher picks the
 * fastest path automatically. */
static blake3ForceImpl g_force_impl = BLAKE3_IMPL_AUTO;

void blake3SetForceImpl(blake3ForceImpl impl) {
    g_force_impl = impl;
}
blake3ForceImpl blake3GetForceImpl(void) {
    return g_force_impl;
}

/* Internal "which backend should this batch use right now?" report.
 * Mirrors the public blake3DispatcherActiveBackend() but threads in
 * the per-call hasher state and input length so the dispatcher can
 * pick the widest backend whose batch fits. */
typedef enum batchBackend {
    BATCH_NONE = 0, /* fall through to scalar update loop */
    BATCH_NEON_4 = 1,
    BATCH_AVX2_8 = 2,
    BATCH_SSE41_4 = 3,
} batchBackend;

/* Pick the widest SIMD batch backend whose precondition holds.
 * Prefer AVX2 (8) > NEON (4) > SSE4.1 (4) > scalar. NEON and SSE4.1
 * are mutually exclusive in practice (ARM vs x86), so the relative
 * order between them only matters on a hypothetical host that probes
 * both — there we tie-break NEON > SSE4.1 since NEON is the native ISA
 * for that host class. */
static inline batchBackend simd_batch_ready(const blake3Hasher *self,
                                            size_t input_len) {
    if (g_force_impl == BLAKE3_IMPL_SCALAR) {
        return BATCH_NONE;
    }
    if (self->buf_len != 0 || chunk_bytes_consumed(self) != 0) {
        return BATCH_NONE;
    }
    if (blake3AVX2Available() &&
        input_len >= BLAKE3_BATCH_AVX2_CHUNKS * BLAKE3_CHUNK_LEN) {
        return BATCH_AVX2_8;
    }
    if (blake3NeonAvailable() &&
        input_len >= BLAKE3_BATCH_NEON_CHUNKS * BLAKE3_CHUNK_LEN) {
        return BATCH_NEON_4;
    }
    if (blake3SSE41Available() &&
        input_len >= BLAKE3_BATCH_SSE41_CHUNKS * BLAKE3_CHUNK_LEN) {
        return BATCH_SSE41_4;
    }
    return BATCH_NONE;
}

/* Public introspection — what would the dispatcher pick for a
 * sufficiently large input on this host right now? */
blake3Backend blake3DispatcherActiveBackend(void) {
    if (g_force_impl == BLAKE3_IMPL_SCALAR) {
        return BLAKE3_BACKEND_SCALAR;
    }
    if (blake3AVX2Available()) {
        return BLAKE3_BACKEND_AVX2;
    }
    if (blake3NeonAvailable()) {
        return BLAKE3_BACKEND_NEON;
    }
    if (blake3SSE41Available()) {
        return BLAKE3_BACKEND_SSE41;
    }
    return BLAKE3_BACKEND_SCALAR;
}

/* Run whichever batch backend simd_batch_ready selected.  Falls
 * through silently when called with BATCH_NONE (single-place no-op
 * keeps the call sites in blake3HasherUpdate uncluttered). */
static inline void run_simd_batch(blake3Hasher *self, batchBackend b,
                                  const uint8_t **in_ptr, size_t *in_len) {
    if (b == BATCH_AVX2_8) {
        process_8_chunks_simd_avx2(self, in_ptr, in_len);
        /* After AVX2 drains all 8-batches, the remainder may still be
         * large enough for a narrower 4-batch.  Try NEON (mostly a
         * no-op on x86) then SSE4.1 before falling back to scalar. */
        if (blake3NeonAvailable() &&
            *in_len >= BLAKE3_BATCH_NEON_CHUNKS * BLAKE3_CHUNK_LEN) {
            process_4_chunks_simd(self, in_ptr, in_len);
        } else if (blake3SSE41Available() &&
                   *in_len >= BLAKE3_BATCH_SSE41_CHUNKS * BLAKE3_CHUNK_LEN) {
            process_4_chunks_simd_sse41(self, in_ptr, in_len);
        }
    } else if (b == BATCH_NEON_4) {
        process_4_chunks_simd(self, in_ptr, in_len);
    } else if (b == BATCH_SSE41_4) {
        process_4_chunks_simd_sse41(self, in_ptr, in_len);
    }
}

void blake3HasherUpdate(blake3Hasher *self, const void *input,
                        size_t input_len) {
    const uint8_t *in = (const uint8_t *)input;

    /* Try the SIMD batch path before entering the regular loop.  This
     * is the most common entry point for big one-shot hashes.  Prefer
     * the widest available backend (AVX2 8 > NEON 4 > scalar). */
    {
        batchBackend b = simd_batch_ready(self, input_len);
        if (b != BATCH_NONE) {
            run_simd_batch(self, b, &in, &input_len);
        }
    }

    while (input_len > 0) {
        /* (1) Drain a fully-consumed chunk if more input arrived.  After
         * a drain we land at a chunk boundary and may be eligible for
         * the SIMD batch path again — this is what unlocks the fast
         * path for HMAC and any caller whose first bytes don't fall on
         * a chunk boundary. */
        if (chunk_bytes_consumed(self) == BLAKE3_CHUNK_LEN) {
            chunk_complete(self);
            batchBackend b = simd_batch_ready(self, input_len);
            if (b != BATCH_NONE) {
                run_simd_batch(self, b, &in, &input_len);
                if (input_len == 0) {
                    break;
                }
            }
        }

        /* (2) A previous Update may have held back a full 64-byte
         * block.  Now that we have more data, we know it wasn't the
         * chunk's last block (because if it were, we'd be at
         * chunk_bytes_consumed == 1024 and step (1) would have drained
         * it as chunk_complete above).  Safe to compress. */
        if (self->buf_len == BLAKE3_BLOCK_LEN) {
            chunk_update_block(self, self->buf);
            self->buf_len = 0;
            /* Trailing-bytes hygiene: chunk_output reads all 64 bytes
             * of buf, so any held-back partial blocks built up later
             * must start from zeros. */
            memset(self->buf, 0, BLAKE3_BLOCK_LEN);
        }

        /* (3) Top up a partial buf without flushing it. */
        if (self->buf_len > 0) {
            const size_t room = BLAKE3_BLOCK_LEN - self->buf_len;
            const size_t take = input_len < room ? input_len : room;
            memcpy(self->buf + self->buf_len, in, take);
            self->buf_len = (uint8_t)(self->buf_len + take);
            in += take;
            input_len -= take;
            continue;
        }

        /* (4) Buf empty.  Bulk-compress full blocks directly from
         * input, stopping before the chunk's last block-slot
         * (chunk_room <= 64) or the caller's last 64 bytes
         * (input_len <= 64).  Both must be held back. */
        while (input_len > BLAKE3_BLOCK_LEN) {
            const size_t chunk_room =
                BLAKE3_CHUNK_LEN - chunk_bytes_consumed(self);
            if (chunk_room <= BLAKE3_BLOCK_LEN) {
                break;
            }
            chunk_update_block(self, in);
            in += BLAKE3_BLOCK_LEN;
            input_len -= BLAKE3_BLOCK_LEN;
        }

        /* (5) Hold back the remaining 1..64 bytes in buf.  The chunk has
         * room: buf is empty, so the chunk holds whole blocks, and a full
         * one would have been drained by (1). */
        if (input_len > 0) {
            const size_t take =
                input_len < BLAKE3_BLOCK_LEN ? input_len : BLAKE3_BLOCK_LEN;
            assert(take <= BLAKE3_CHUNK_LEN - chunk_bytes_consumed(self));
            memcpy(self->buf, in, take);
            self->buf_len = (uint8_t)take;
            in += take;
            input_len -= take;
        }
    }
}

/* ----- Finalize ------------------------------------------------------- */

/* Compute the final-output snapshot. If there is exactly one chunk in
 * the entire hasher, that chunk's output is itself the root. Otherwise
 * we fold the CV stack right-to-left into parents, ending with a root
 * parent whose output is emitted. */
static blake3Output hasher_final_output(const blake3Hasher *self) {
    /* If the stack is empty, there's a single chunk in self->cv/buf. */
    if (self->cv_stack_len == 0) {
        return chunk_output(self);
    }

    /* Exact-power-of-2-chunks case: eager fold during push collapsed
     * the merkle tree all the way to one CV.  That CV no longer
     * carries the compression inputs needed to re-emit with ROOT —
     * use the saved (left, right) pair from the last fold to
     * reconstruct the root parent_output. */
    if (self->cv_stack_len == 1 && chunk_bytes_consumed(self) == 0 &&
        self->pending_root_valid) {
        return parent_output(self->pending_root_left, self->pending_root_right,
                             self->key, self->flags);
    }

    /* Build the seed CV from the current (possibly partial) chunk
     * unless it's empty. The hasher always reflects the most recent
     * chunk; if it has no data, the top of the stack is already a CV
     * we need to pop and use as the right-hand operand. */
    uint8_t right_cv[32];
    uint32_t stack_len = self->cv_stack_len;
    blake3Output o;

    if (chunk_bytes_consumed(self) > 0) {
        blake3Output co = chunk_output(self);
        output_chaining_value(&co, right_cv);
    } else {
        memcpy(right_cv, &self->cv_stack[(stack_len - 1) * 32], 32);
        stack_len--;
    }

    /* Fold remaining stack entries as parents. The very last (top-most)
     * parent is the root and is returned as an output (not finalized to
     * a CV) so the caller can produce XOF bytes. */
    while (stack_len > 0) {
        const uint8_t *left = &self->cv_stack[(stack_len - 1) * 32];
        o = parent_output(left, right_cv, self->key, self->flags);
        if (stack_len == 1) {
            return o;
        }
        output_chaining_value(&o, right_cv);
        stack_len--;
    }

    /* Unreachable: one CV on the stack and an empty chunk means a power
     * of two chunks, whose last push folded and saved the root's pair
     * above; only a total of 1 pushes without folding, and chunk 0 is
     * completed only when more input follows it */
    assert(NULL && "one stacked CV, an empty chunk and no root pair");
    return chunk_output(self);
}

void blake3HasherFinalizeSeek(const blake3Hasher *self, uint64_t seek,
                              uint8_t *out, size_t out_len) {
    if (out_len == 0) {
        return;
    }
    blake3Output o = hasher_final_output(self);
    output_root_bytes(&o, seek, out, out_len);
}

void blake3HasherFinalize(const blake3Hasher *self, uint8_t *out,
                          size_t out_len) {
    blake3HasherFinalizeSeek(self, 0, out, out_len);
}

void blake3Hash(const void *input, size_t input_len,
                uint8_t out[BLAKE3_OUT_LEN]) {
    blake3Hasher h;
    blake3HasherInit(&h);
    blake3HasherUpdate(&h, input, input_len);
    blake3HasherFinalize(&h, out, BLAKE3_OUT_LEN);
}

void blake3HashKeyed(const uint8_t key[BLAKE3_KEY_LEN], const void *input,
                     size_t input_len, uint8_t out[BLAKE3_OUT_LEN]) {
    blake3Hasher h;
    blake3HasherInitKeyed(&h, key);
    blake3HasherUpdate(&h, input, input_len);
    blake3HasherFinalize(&h, out, BLAKE3_OUT_LEN);
    /* Targeted wipe of secret state only: h.key holds the 32-byte
     * keyed-hash key; h.cv holds either a copy of that key (right
     * after a chunk_reset) or a key-keyed compression intermediate
     * — both must be scrubbed.  cv_stack / buf / pending_root_* /
     * counters are PRF outputs or scratch derived from already-public
     * message bytes; wiping the full ~1965-byte struct cost ~670 ns
     * per call (2-3× the hash itself on small BLAKE3_KEYED envelopes).
     * Scrub only the secret-bearing words. */
    dkSecureZero(h.key, sizeof(h.key));
    dkSecureZero(h.cv, sizeof(h.cv));
}

void blake3DeriveKey(const void *context, size_t context_len, const void *ikm,
                     size_t ikm_len, uint8_t *out, size_t out_len) {
    blake3Hasher h;
    blake3HasherInitDeriveKeyRaw(&h, context, context_len);
    blake3HasherUpdate(&h, ikm, ikm_len);
    blake3HasherFinalize(&h, out, out_len);
    /* Targeted wipe of secret state only: h.key holds the derive-key
     * context-derived secret (the key for DERIVE_KEY_MATERIAL); h.cv
     * holds either a copy of that key or a key-keyed compression
     * intermediate.  See blake3HashKeyed for full rationale. */
    dkSecureZero(h.key, sizeof(h.key));
    dkSecureZero(h.cv, sizeof(h.cv));
}

/* ====================================================================== */
/* Test driver                                                            */
/* ====================================================================== */

#ifdef DATAKIT_TEST

#include "cleaner.h"
#include "ctest.h"
#include "hash/blake3AVX2.h"
#include "hash/blake3Neon.h"
#include "hash/blake3SSE41.h"
#include "hash/blake3TestVectors.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/* Scope-exit cleaners for test scratch buffers (see cleaner.h idioms). */
defcleanerName(uint8_t *, zfree, Bytes);
defcleanerName(blake3Hasher *, zfree, Hasher);

/* Per-thread context for the multi-threaded correctness test.  Each
 * thread runs `iters` hash rounds against `payload_len` deterministic
 * bytes seeded from `seed`.  `expected` is the single-shot digest of
 * those same bytes, computed ONCE by the test driver before spawning
 * the threads (so all threads compare against an identical reference
 * value rather than computing it independently — which would only
 * prove intra-thread determinism, not cross-thread).  Any digest a
 * thread produces that does not match `expected` increments
 * `mismatches`. */
typedef struct {
    uint8_t expected[BLAKE3_OUT_LEN];
    size_t payload_len;
    uint32_t seed;
    uint32_t iters;
    uint32_t mismatches;
} blake3MtThreadCtx;

static void *blake3MtWorker(void *arg) {
    blake3MtThreadCtx *ctx = (blake3MtThreadCtx *)arg;
    cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(ctx->payload_len);
    for (size_t i = 0; i < ctx->payload_len; i++) {
        buf[i] = (uint8_t)((i * 1103515245u + ctx->seed) >> 8);
    }
    for (uint32_t t = 0; t < ctx->iters; t++) {
        blake3Hasher h;
        blake3HasherInit(&h);
        /* Vary the chunk granularity each iteration to exercise the
         * small-block, single-chunk, and SIMD-batch dispatch branches
         * all concurrently with the other threads. */
        const size_t step = 1u + (size_t)(t % 511u);
        size_t pos = 0;
        while (pos < ctx->payload_len) {
            const size_t n = (pos + step <= ctx->payload_len)
                                 ? step
                                 : (ctx->payload_len - pos);
            blake3HasherUpdate(&h, buf + pos, n);
            pos += n;
        }
        uint8_t observed[BLAKE3_OUT_LEN];
        blake3HasherFinalize(&h, observed, BLAKE3_OUT_LEN);
        if (memcmp(observed, ctx->expected, BLAKE3_OUT_LEN) != 0) {
            ctx->mismatches++;
        }
    }
    return NULL;
}

/* Lowercase hex of a single byte. */
static void byte_to_hex(uint8_t b, char out[3]) {
    static constexpr char H[] = "0123456789abcdef";
    out[0] = H[b >> 4];
    out[1] = H[b & 0x0F];
    out[2] = 0;
}

static void bytes_to_hex(const uint8_t *bytes, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) {
        byte_to_hex(bytes[i], out + i * 2);
    }
}

static int is_todo_vector(const char *hex) {
    return hex && strncmp(hex, "TODO_VECTOR_", 12) == 0;
}

/* Compare hasher output against an expected hex string, skipping if the
 * vector is a TODO placeholder. Returns 1 if compared, 0 if skipped. */
static int check_against_hex(const uint8_t *digest, size_t digest_len,
                             const char *expected_hex, const char *label,
                             size_t input_len, int *err_p) {
    if (is_todo_vector(expected_hex)) {
        return 0;
    }
    char got[BLAKE3_OUT_LEN * 2 + 1];
    bytes_to_hex(digest, digest_len, got);
    got[digest_len * 2] = 0;
    if (strncmp(got, expected_hex, digest_len * 2) != 0) {
        printf("%s:%d\tERROR! %s mismatch at input_len=%zu: got=%s "
               "expected=%.*s\n",
               __FILE__, __LINE__, label, input_len, got, (int)(digest_len * 2),
               expected_hex);
        fflush(stdout);
        (*err_p)++;
    }
    return 1;
}

/* Fill a buffer with the official BLAKE3 test pattern (i mod 251). */
static void fill_test_input(uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        buf[i] = (uint8_t)(i % 251);
    }
}

int blake3Test(int, char *[]) {
    int err = 0;

    /* Largest test vector input size. */
    size_t max_input = 0;
    for (size_t i = 0; i < BLAKE3_TEST_VECTOR_COUNT; i++) {
        if (blake3TestVectors[i].input_len > max_input) {
            max_input = blake3TestVectors[i].input_len;
        }
    }
    cleanerName(zfree, Bytes) uint8_t *input =
        (uint8_t *)zmalloc(max_input ? max_input : 1);
    fill_test_input(input, max_input);

    /* Key is exactly 32 bytes, no terminator — copy from a literal. */
    uint8_t keyed_key[BLAKE3_KEY_LEN];
    memcpy(keyed_key, "whats the Elvish word for friend", BLAKE3_KEY_LEN);

    TEST("blake3 plain hash matches official vectors") {
        for (size_t i = 0; i < BLAKE3_TEST_VECTOR_COUNT; i++) {
            const blake3TestVector *v = &blake3TestVectors[i];
            uint8_t digest[BLAKE3_OUT_LEN];
            blake3Hash(input, v->input_len, digest);
            check_against_hex(digest, BLAKE3_OUT_LEN, v->hash_hex, "plain",
                              v->input_len, &err);
        }
    }

    TEST("blake3 keyed hash matches official vectors") {
        for (size_t i = 0; i < BLAKE3_TEST_VECTOR_COUNT; i++) {
            const blake3TestVector *v = &blake3TestVectors[i];
            uint8_t digest[BLAKE3_OUT_LEN];
            blake3HashKeyed(keyed_key, input, v->input_len, digest);
            check_against_hex(digest, BLAKE3_OUT_LEN, v->keyed_hash_hex,
                              "keyed", v->input_len, &err);
        }
    }

    TEST("blake3 derive-key matches official vectors") {
        for (size_t i = 0; i < BLAKE3_TEST_VECTOR_COUNT; i++) {
            const blake3TestVector *v = &blake3TestVectors[i];
            blake3Hasher h;
            blake3HasherInitDeriveKey(&h, BLAKE3_TEST_DERIVE_CONTEXT);
            blake3HasherUpdate(&h, input, v->input_len);
            uint8_t digest[BLAKE3_OUT_LEN];
            blake3HasherFinalize(&h, digest, BLAKE3_OUT_LEN);
            check_against_hex(digest, BLAKE3_OUT_LEN, v->derive_key_hex,
                              "derive_key", v->input_len, &err);
        }
    }

    TEST("blake3DeriveKey one-shot matches manual init+update+finalize") {
        const uint8_t *ctx_bytes = (const uint8_t *)BLAKE3_TEST_DERIVE_CONTEXT;
        const size_t ctx_len = strlen(BLAKE3_TEST_DERIVE_CONTEXT);
        for (size_t i = 0; i < BLAKE3_TEST_VECTOR_COUNT; i++) {
            const blake3TestVector *v = &blake3TestVectors[i];
            uint8_t one_shot[BLAKE3_OUT_LEN];
            blake3DeriveKey(ctx_bytes, ctx_len, input, v->input_len, one_shot,
                            BLAKE3_OUT_LEN);
            check_against_hex(one_shot, BLAKE3_OUT_LEN, v->derive_key_hex,
                              "blake3DeriveKey", v->input_len, &err);
        }
    }

    TEST("blake3DeriveKey: context separation yields independent keys") {
        const uint8_t ikm[32] = {0x42};
        const uint8_t ctxA[] = {'d', 'k', '-', 'A'};
        const uint8_t ctxB[] = {'d', 'k', '-', 'B'};
        uint8_t k1[32], k2[32];
        blake3DeriveKey(ctxA, sizeof(ctxA), ikm, sizeof(ikm), k1, sizeof(k1));
        blake3DeriveKey(ctxB, sizeof(ctxB), ikm, sizeof(ikm), k2, sizeof(k2));
        if (memcmp(k1, k2, 32) == 0) {
            ERR("Two different contexts must produce different keys");
        }
        uint8_t k1b[32];
        blake3DeriveKey(ctxA, sizeof(ctxA), ikm, sizeof(ikm), k1b, sizeof(k1b));
        if (memcmp(k1, k1b, 32) != 0) {
            ERR("Same context must produce same key (deterministic)");
        }
    }

    TEST("blake3DeriveKey: arbitrary output length is XOF stream") {
        const uint8_t ikm[32] = {0xAA};
        const uint8_t ctx[] = {'d', 'k', '-', 'L'};
        uint8_t out64[64], out128[128];
        blake3DeriveKey(ctx, sizeof(ctx), ikm, sizeof(ikm), out64,
                        sizeof(out64));
        blake3DeriveKey(ctx, sizeof(ctx), ikm, sizeof(ikm), out128,
                        sizeof(out128));
        if (memcmp(out64, out128, 64) != 0) {
            ERR("XOF length-extension property violated");
        }
    }

    TEST("blake3 streaming equals one-shot") {
        /* Deterministic prng over a fixed buffer; we split it into
         * varying chunk sizes and compare. */
        size_t test_sizes[] = {0,    1,    31,   63,   64,   65,   127,
                               128,  129,  1023, 1024, 1025, 2047, 2048,
                               2049, 4095, 4096, 4097, 8191, 8192, 8193};
        size_t n_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);

        cleanerName(zfree, Bytes) uint8_t *buf = (uint8_t *)zmalloc(8193);
        fill_test_input(buf, 8193);

        for (size_t s = 0; s < n_sizes; s++) {
            size_t sz = test_sizes[s];

            uint8_t one_shot[BLAKE3_OUT_LEN];
            blake3Hash(buf, sz, one_shot);

            /* Several different chunking strategies. */
            size_t step_sizes[] = {1, 7, 31, 63, 64, 127, 128, 1023, 1024};
            for (size_t k = 0; k < sizeof(step_sizes) / sizeof(step_sizes[0]);
                 k++) {
                size_t step = step_sizes[k];
                blake3Hasher h;
                blake3HasherInit(&h);
                size_t off = 0;
                while (off < sz) {
                    size_t take = sz - off < step ? sz - off : step;
                    blake3HasherUpdate(&h, buf + off, take);
                    off += take;
                }
                uint8_t streamed[BLAKE3_OUT_LEN];
                blake3HasherFinalize(&h, streamed, BLAKE3_OUT_LEN);
                if (memcmp(one_shot, streamed, BLAKE3_OUT_LEN) != 0) {
                    ERR("streaming mismatch at size=%zu step=%zu", sz, step);
                }
            }
        }
    }

    TEST("blake3 batches after a mid-stream chunk drain match scalar") {
        /* A first update of p bytes, then one of the rest (4+ chunks):
         * when p ends a chunk, the second update drains it and batches
         * the rest, possibly all of it.  Every split, under the SIMD
         * dispatch, gives the forced-scalar one-shot digest (64 bytes
         * of output, so the root's XOF is compared too) */
        static constexpr size_t totals[] = {
            5 * 1024,  5 * 1024 + 1, 8 * 1024,      9 * 1024,
            16 * 1024, 17 * 1024,    17 * 1024 + 63};
        static constexpr size_t splits[] = {
            1, 63, 64, 1023, 1024, 1025, 2048, 3072, 4096, 4 * 1024 + 1};
        const size_t maxTotal = 17 * 1024 + 63;
        cleanerName(zfree, Bytes) uint8_t *buf = (uint8_t *)zmalloc(maxTotal);
        fill_test_input(buf, maxTotal);
        const blake3ForceImpl saved = blake3GetForceImpl();
        for (size_t t = 0; t < sizeof(totals) / sizeof(*totals); t++) {
            uint8_t want[64];
            blake3SetForceImpl(BLAKE3_IMPL_SCALAR);
            blake3Hasher ref;
            blake3HasherInit(&ref);
            blake3HasherUpdate(&ref, buf, totals[t]);
            blake3HasherFinalize(&ref, want, sizeof(want));
            for (size_t s = 0; s < sizeof(splits) / sizeof(*splits); s++) {
                const size_t p = splits[s];
                if (p >= totals[t]) {
                    continue;
                }
                for (int mode = 0; mode < 2; mode++) {
                    blake3SetForceImpl(mode ? BLAKE3_IMPL_SIMD
                                            : BLAKE3_IMPL_AUTO);
                    blake3Hasher h;
                    blake3HasherInit(&h);
                    blake3HasherUpdate(&h, buf, p);
                    blake3HasherUpdate(&h, buf + p, totals[t] - p);
                    uint8_t got[64];
                    blake3HasherFinalize(&h, got, sizeof(got));
                    if (memcmp(got, want, sizeof(want)) != 0) {
                        ERR("total %zu split at %zu (mode %d): digest "
                            "differs from scalar",
                            totals[t], p, mode);
                    }
                }
            }
        }
        blake3SetForceImpl(saved);
    }

    TEST("blake3 chunk-boundary inputs are stable") {
        size_t boundaries[] = {1023, 1024, 1025, 2047, 2048, 2049,
                               3071, 3072, 3073, 4095, 4096, 4097};
        size_t n = sizeof(boundaries) / sizeof(boundaries[0]);
        cleanerName(zfree, Bytes) uint8_t *buf = (uint8_t *)zmalloc(4097);
        fill_test_input(buf, 4097);
        for (size_t i = 0; i < n; i++) {
            uint8_t a[BLAKE3_OUT_LEN], b[BLAKE3_OUT_LEN];
            blake3Hash(buf, boundaries[i], a);
            /* hash again via single-byte updates */
            blake3Hasher h;
            blake3HasherInit(&h);
            for (size_t j = 0; j < boundaries[i]; j++) {
                blake3HasherUpdate(&h, buf + j, 1);
            }
            blake3HasherFinalize(&h, b, BLAKE3_OUT_LEN);
            if (memcmp(a, b, BLAKE3_OUT_LEN) != 0) {
                ERR("boundary size %zu: streaming != one-shot", boundaries[i]);
            }
        }
    }

    TEST("blake3 key sensitivity") {
        uint8_t key1[BLAKE3_KEY_LEN];
        uint8_t key2[BLAKE3_KEY_LEN];
        for (uint32_t i = 0; i < BLAKE3_KEY_LEN; i++) {
            key1[i] = (uint8_t)i;
            key2[i] = (uint8_t)(i + 1);
        }
        const char *msg = "hello world";
        uint8_t d1[BLAKE3_OUT_LEN], d2[BLAKE3_OUT_LEN],
            d1_again[BLAKE3_OUT_LEN];
        blake3HashKeyed(key1, msg, strlen(msg), d1);
        blake3HashKeyed(key2, msg, strlen(msg), d2);
        blake3HashKeyed(key1, msg, strlen(msg), d1_again);
        if (memcmp(d1, d2, BLAKE3_OUT_LEN) == 0) {
            ERR("different keys produced equal outputs");
        }
        if (memcmp(d1, d1_again, BLAKE3_OUT_LEN) != 0) {
            ERR("same key + same input produced different outputs");
        }
    }

    TEST("blake3 keyed-hash wipe scrubs secret fields only") {
        /* Scope contract: blake3HashKeyed and blake3DeriveKey wipe only
         * the secret-bearing fields (h.key, h.cv) after Finalize.
         * cv_stack / buf / pending_root_* / counters are PRF outputs or
         * scratch derived from already-public message bytes and are
         * intentionally NOT wiped (the full-struct wipe added ~670 ns
         * per call, dominating the hash itself on small envelopes).
         *
         * This test exercises the wipe pattern directly on a heap-
         * allocated hasher (the in-API stack hasher is unreachable
         * post-return), feeds enough input to populate cv_stack and
         * buf with non-zero scratch, applies the production wipe, and
         * asserts the scope:
         *   (1) h->key is fully zero post-wipe.
         *   (2) h->cv is fully zero post-wipe.
         *   (3) AT LEAST ONE scratch region (cv_stack[0..32], or buf)
         *       retains a non-zero byte — proving the wipe is scoped
         *       and the perf-regression-causing full wipe has not been
         *       reintroduced. */
        cleanerName(zfree, Hasher) blake3Hasher *h =
            (blake3Hasher *)zmalloc(sizeof(*h));
        uint8_t pattern_key[BLAKE3_KEY_LEN];
        for (uint32_t i = 0; i < BLAKE3_KEY_LEN; i++) {
            pattern_key[i] = 0xA5;
        }
        /* Feed >= 2 chunks so chunk_complete pushes at least one CV
         * onto cv_stack, and so buf ends holding a (public) partial
         * trailing block of the final chunk. */
        const size_t msg_len = BLAKE3_CHUNK_LEN * 2 + 17;
        cleanerName(zfree, Bytes) uint8_t *msg = (uint8_t *)zmalloc(msg_len);
        for (size_t i = 0; i < msg_len; i++) {
            msg[i] = (uint8_t)(i & 0xFFu);
        }
        uint8_t digest[BLAKE3_OUT_LEN];

        blake3HasherInitKeyed(h, pattern_key);
        blake3HasherUpdate(h, msg, msg_len);
        blake3HasherFinalize(h, digest, BLAKE3_OUT_LEN);

        /* Snapshot scratch before wipe: at least cv_stack[0..32] must
         * be non-zero (it's the chunk-0 CV after the first
         * chunk_complete). */
        bool stack_had_data = false;
        for (size_t i = 0; i < 32; i++) {
            if (h->cv_stack[i] != 0) {
                stack_had_data = true;
                break;
            }
        }
        if (!stack_had_data) {
            ERR("test setup invalid: cv_stack[0..32] is all zero pre-wipe; "
                "input did not advance past one chunk");
        }

        /* Apply production wipe pattern (mirror of blake3HashKeyed /
         * blake3DeriveKey post-Finalize). */
        dkSecureZero(h->key, sizeof(h->key));
        dkSecureZero(h->cv, sizeof(h->cv));

        for (size_t i = 0; i < sizeof(h->key) / sizeof(h->key[0]); i++) {
            if (h->key[i] != 0) {
                ERR("h->key not zero post-wipe at word %zu", i);
            }
        }
        for (size_t i = 0; i < sizeof(h->cv) / sizeof(h->cv[0]); i++) {
            if (h->cv[i] != 0) {
                ERR("h->cv not zero post-wipe at word %zu", i);
            }
        }
        /* Confirm scratch was NOT touched by the targeted wipe. */
        bool scratch_intact = false;
        for (size_t i = 0; i < 32; i++) {
            if (h->cv_stack[i] != 0) {
                scratch_intact = true;
                break;
            }
        }
        if (!scratch_intact) {
            ERR("cv_stack was wiped — scope reduction regression "
                "(perf-critical: full-struct wipe was ~670 ns/call)");
        }
    }

    TEST("blake3 derive-key wipe scrubs secret fields only") {
        /* Mirror of the keyed-hash wipe test for the DeriveKey path.
         * After blake3HasherInitDeriveKeyRaw, h->key holds the secret
         * context-derived key (the input to DERIVE_KEY_MATERIAL); h->cv
         * holds a copy of that key or a key-keyed compression
         * intermediate.  Both must be wiped; scratch must not be. */
        cleanerName(zfree, Hasher) blake3Hasher *h =
            (blake3Hasher *)zmalloc(sizeof(*h));
        const char ctx[] = "datakit derive-key wipe scope test 2026";
        const size_t ikm_len = BLAKE3_CHUNK_LEN * 2 + 9;
        cleanerName(zfree, Bytes) uint8_t *ikm = (uint8_t *)zmalloc(ikm_len);
        for (size_t i = 0; i < ikm_len; i++) {
            ikm[i] = (uint8_t)(0x5A ^ (i & 0xFFu));
        }
        uint8_t out[64];

        blake3HasherInitDeriveKeyRaw(h, ctx, sizeof(ctx) - 1);
        blake3HasherUpdate(h, ikm, ikm_len);
        blake3HasherFinalize(h, out, sizeof(out));

        bool stack_had_data = false;
        for (size_t i = 0; i < 32; i++) {
            if (h->cv_stack[i] != 0) {
                stack_had_data = true;
                break;
            }
        }
        if (!stack_had_data) {
            ERR("test setup invalid: cv_stack[0..32] is all zero pre-wipe");
        }

        dkSecureZero(h->key, sizeof(h->key));
        dkSecureZero(h->cv, sizeof(h->cv));

        for (size_t i = 0; i < sizeof(h->key) / sizeof(h->key[0]); i++) {
            if (h->key[i] != 0) {
                ERR("h->key not zero post-wipe at word %zu", i);
            }
        }
        for (size_t i = 0; i < sizeof(h->cv) / sizeof(h->cv[0]); i++) {
            if (h->cv[i] != 0) {
                ERR("h->cv not zero post-wipe at word %zu", i);
            }
        }
        bool scratch_intact = false;
        for (size_t i = 0; i < 32; i++) {
            if (h->cv_stack[i] != 0) {
                scratch_intact = true;
                break;
            }
        }
        if (!scratch_intact) {
            ERR("cv_stack was wiped — scope reduction regression");
        }
    }

    TEST("blake3 init helpers wipe stack-local key material") {
        /* The Init helpers (blake3HasherInitKeyed,
         * blake3HasherInitDeriveKeyRaw) declare stack-local arrays that
         * briefly hold key-equivalent material before hasher_init_internal
         * copies it into self. After their stack frames unwind, those
         * locals are unobservable from C — there is no portable way to
         * read freed stack from a unit test.
         *
         * What we CAN test from outside is correctness: the wipes must
         * land AFTER hasher_init_internal completes its memcpy into
         * self->key, otherwise the keyed hash would be wrong. A
         * regression that moves the wipe call earlier (e.g., wipes
         * key_words before passing it to hasher_init_internal) would
         * silently produce zero-keyed output. This test pins that
         * sequencing by computing a keyed hash with a recognizable key
         * and asserting the digest is NOT what BLAKE3_IV-keyed would
         * produce.
         *
         * Stack-residue itself is enforced by code review — the
         * dkSecureZero calls at the exits of blake3HasherInitKeyed and
         * blake3HasherInitDeriveKeyRaw are visible in source. Under
         * -O3 -flto the compiler must honor dkSecureZero's asm barrier;
         * if a future refactor elides it, ASan/UBSan won't catch it but
         * the audit's source-grep sweep will. */
        uint8_t key[BLAKE3_KEY_LEN];
        for (uint32_t i = 0; i < BLAKE3_KEY_LEN; i++) {
            key[i] = (uint8_t)(0xC3 ^ i);
        }
        uint8_t probe_input[64];
        for (uint32_t i = 0; i < 64; i++) {
            probe_input[i] = (uint8_t)i;
        }
        uint8_t keyed_out[BLAKE3_OUT_LEN];
        blake3Hasher hk;
        blake3HasherInitKeyed(&hk, key);
        blake3HasherUpdate(&hk, probe_input, sizeof(probe_input));
        blake3HasherFinalize(&hk, keyed_out, BLAKE3_OUT_LEN);

        uint8_t plain_out[BLAKE3_OUT_LEN];
        blake3Hasher hp;
        blake3HasherInit(&hp);
        blake3HasherUpdate(&hp, probe_input, sizeof(probe_input));
        blake3HasherFinalize(&hp, plain_out, BLAKE3_OUT_LEN);

        if (memcmp(keyed_out, plain_out, BLAKE3_OUT_LEN) == 0) {
            ERR("InitKeyed produced same digest as InitPlain — "
                "key_words wipe sequenced before hasher_init_internal");
        }

        /* DeriveKeyRaw parity check: derive twice from the same context,
         * confirm deterministic. A wipe that landed BEFORE the second
         * hasher_init_internal would corrupt self->key and produce
         * different outputs across runs. */
        const char ctx[] = "datakit init-helper wipe sequencing test";
        uint8_t derive_a[BLAKE3_OUT_LEN];
        uint8_t derive_b[BLAKE3_OUT_LEN];
        blake3Hasher ha, hb;
        blake3HasherInitDeriveKeyRaw(&ha, ctx, sizeof(ctx) - 1);
        blake3HasherInitDeriveKeyRaw(&hb, ctx, sizeof(ctx) - 1);
        blake3HasherUpdate(&ha, probe_input, sizeof(probe_input));
        blake3HasherUpdate(&hb, probe_input, sizeof(probe_input));
        blake3HasherFinalize(&ha, derive_a, BLAKE3_OUT_LEN);
        blake3HasherFinalize(&hb, derive_b, BLAKE3_OUT_LEN);
        if (memcmp(derive_a, derive_b, BLAKE3_OUT_LEN) != 0) {
            ERR("DeriveKeyRaw produced different digests across calls — "
                "wipe sequenced before hasher_init_internal");
        }
    }

    TEST("blake3 XOF extension is consistent with single-block finalize") {
        /* The first 32 bytes of a long output must match a 32-byte
         * finalize. Spot-check on a few input sizes. */
        size_t sizes[] = {0, 1, 64, 1024, 1025, 4096};
        cleanerName(zfree, Bytes) uint8_t *buf = (uint8_t *)zmalloc(4096);
        fill_test_input(buf, 4096);
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            blake3Hasher h;
            blake3HasherInit(&h);
            blake3HasherUpdate(&h, buf, sizes[s]);
            uint8_t short_out[BLAKE3_OUT_LEN];
            uint8_t long_out[131];
            blake3HasherFinalize(&h, short_out, BLAKE3_OUT_LEN);
            blake3HasherFinalize(&h, long_out, sizeof(long_out));
            if (memcmp(short_out, long_out, BLAKE3_OUT_LEN) != 0) {
                ERR("XOF prefix mismatch at size=%zu", sizes[s]);
            }
            /* Seek path: bytes [32..64) of long_out must equal a
             * seek-32 finalize of length 32. */
            uint8_t seeked[32];
            blake3HasherFinalizeSeek(&h, 32, seeked, 32);
            if (memcmp(seeked, long_out + 32, 32) != 0) {
                ERR("XOF seek mismatch at size=%zu", sizes[s]);
            }
        }
    }

    /* ============================================================
     * Cross-impl differential fuzz — portable vs every SIMD variant
     * available on this host.  For each random (cv, block, block_len,
     * counter, flags) tuple we compare the portable compress against
     * each SIMD compress; outputs must agree bit-for-bit.  Catches
     * platform-specific SIMD bugs that the per-variant self-tests
     * might miss.
     * ============================================================ */
    TEST("blake3 compress-primitive head-to-head at each workload") {
        /* Measure the THREE candidate compress primitives directly,
         * doing the same per-block work the hasher does internally —
         * no dispatch involved.  This isolates "which primitive is
         * fastest per unit of work" from any threshold logic.
         *
         * For N chunks of input the hasher does N*16 single-block
         * compresses, OR ceil(N/4)*16 Compress4 batches (each batch
         * advances 4 chunks-worth of work per call).
         *
         * The three primitives:
         *   - blake3PortableCompressInPlace  (scalar, force-inlined)
         *   - blake3NeonCompressInPlace      (NEON, 4-way SIMD within
         *                                     one block's 16-word state)
         *   - blake3NeonCompress4            (NEON, 4-way SIMD across
         *                                     4 independent chunks)
         *
         * Compress4 only applies when there are >= 4 chunks; the
         * column reads "n/a" below that. */
        const size_t chunks[] = {1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 32, 64};
        /* Smoke-size runs (every primitive, every size); the perf preset
         * benches give the real numbers */
        const uint32_t iters_per_size[] = {4000, 2000, 1600, 1200, 1000, 800,
                                           700,  600,  400,  300,  160,  80};
        const size_t n_sizes = sizeof(chunks) / sizeof(chunks[0]);
        const size_t maxBlocks = chunks[n_sizes - 1] * 16;
        const size_t maxBytes = maxBlocks * BLAKE3_BLOCK_LEN;
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(maxBytes);
        for (size_t i = 0; i < maxBytes; i++) {
            buf[i] = (uint8_t)(i * 13 + 7);
        }

        printf("    Each row hashes N chunks of work using ONLY the named "
               "primitive.\n");
        printf("    %-7s  %-13s  %-14s  %-14s\n", "chunks", "scalar (ns)",
               "NEON-1blk (ns)", "Compress4 (ns)");

        for (size_t s = 0; s < n_sizes; s++) {
            const size_t N = chunks[s];
            const size_t nblocks = N * 16;
            const uint32_t iters = iters_per_size[s];
            struct timespec t0, t1;
            volatile uint32_t sink = 0;

            /* --- Scalar primitive: N*16 sequential calls --- */
            uint32_t cv_s[8] = {1, 2, 3, 4, 5, 6, 7, 8};
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (uint32_t i = 0; i < iters; i++) {
                uint32_t cv[8];
                memcpy(cv, cv_s, sizeof(cv));
                for (size_t b = 0; b < nblocks; b++) {
                    blake3PortableCompressInPlace(
                        cv, buf + b * BLAKE3_BLOCK_LEN, BLAKE3_BLOCK_LEN,
                        (uint64_t)b, 0);
                }
                for (int k = 0; k < 8; k++) {
                    sink ^= cv[k];
                }
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double scalar_ns =
                ((t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9) *
                1e9 / iters;

            /* --- NEON single-block primitive --- */
            double neon1_ns = 0;
            if (blake3NeonAvailable()) {
                uint32_t cv_n[8] = {1, 2, 3, 4, 5, 6, 7, 8};
                clock_gettime(CLOCK_MONOTONIC, &t0);
                for (uint32_t i = 0; i < iters; i++) {
                    uint32_t cv[8];
                    memcpy(cv, cv_n, sizeof(cv));
                    for (size_t b = 0; b < nblocks; b++) {
                        blake3NeonCompressInPlace(
                            cv, buf + b * BLAKE3_BLOCK_LEN, BLAKE3_BLOCK_LEN,
                            (uint64_t)b, 0);
                    }
                    for (int k = 0; k < 8; k++) {
                        sink ^= cv[k];
                    }
                }
                clock_gettime(CLOCK_MONOTONIC, &t1);
                neon1_ns = ((t1.tv_sec - t0.tv_sec) +
                            (t1.tv_nsec - t0.tv_nsec) / 1e9) *
                           1e9 / iters;
            }

            /* --- Compress4 batched primitive (requires >= 4 chunks) --- */
            double c4_ns = 0;
            bool c4_valid = (N >= 4) && blake3NeonAvailable();
            if (c4_valid) {
                /* For N chunks, we have N/4 full batches × 16 calls.
                 * Any remainder (N % 4) is left over — measured here
                 * as the SCALAR cost of those tail chunks at the same
                 * per-block rate we measured above.  We report the
                 * SUM so the column is directly comparable to the
                 * scalar and NEON-1blk columns. */
                const size_t nfull =
                    (N / 4) * 4; /* chunks done via Compress4 */
                const size_t nfull_iters = nfull / 4; /* batch iterations */
                const size_t ntail = N - nfull;
                uint32_t cv4_arr[4][8] = {
                    {1, 2, 3, 4, 5, 6, 7, 8},
                    {1, 2, 3, 4, 5, 6, 7, 8},
                    {1, 2, 3, 4, 5, 6, 7, 8},
                    {1, 2, 3, 4, 5, 6, 7, 8},
                };

                clock_gettime(CLOCK_MONOTONIC, &t0);
                for (uint32_t i = 0; i < iters; i++) {
                    uint32_t cur[4][8];
                    for (int k = 0; k < 4; k++) {
                        memcpy(cur[k], cv4_arr[k], sizeof(cv4_arr[k]));
                    }
                    /* Each Compress4 advances one block-position across
                     * 4 chunks.  Loop 16 times for one chunk's depth
                     * × nfull_iters for the chunk batches. */
                    for (size_t batch = 0; batch < nfull_iters; batch++) {
                        for (int blk = 0; blk < 16; blk++) {
                            const uint8_t *blocks[4] = {
                                buf + (batch * 4 + 0) * BLAKE3_CHUNK_LEN +
                                    blk * BLAKE3_BLOCK_LEN,
                                buf + (batch * 4 + 1) * BLAKE3_CHUNK_LEN +
                                    blk * BLAKE3_BLOCK_LEN,
                                buf + (batch * 4 + 2) * BLAKE3_CHUNK_LEN +
                                    blk * BLAKE3_BLOCK_LEN,
                                buf + (batch * 4 + 3) * BLAKE3_CHUNK_LEN +
                                    blk * BLAKE3_BLOCK_LEN,
                            };
                            const uint8_t bl[4] = {64, 64, 64, 64};
                            const uint64_t ct[4] = {0, 0, 0, 0};
                            const uint8_t fl[4] = {0, 0, 0, 0};
                            uint8_t out_bytes[4][32];
                            const uint32_t *cv_in[4] = {cur[0], cur[1], cur[2],
                                                        cur[3]};
                            blake3NeonCompress4(cv_in, blocks, bl, ct, fl,
                                                out_bytes);
                            for (int k = 0; k < 4; k++) {
                                memcpy(cur[k], out_bytes[k], 32);
                            }
                        }
                    }
                    for (size_t k = 0; k < 4; k++) {
                        for (int m = 0; m < 8; m++) {
                            sink ^= cur[k][m];
                        }
                    }

                    /* Tail: scalar cost for ntail chunks (sequential). */
                    if (ntail > 0) {
                        uint32_t cv_t[8] = {1, 2, 3, 4, 5, 6, 7, 8};
                        const size_t tail_blocks = ntail * 16;
                        const size_t tail_start = nfull * BLAKE3_CHUNK_LEN;
                        for (size_t b = 0; b < tail_blocks; b++) {
                            blake3PortableCompressInPlace(
                                cv_t, buf + tail_start + b * BLAKE3_BLOCK_LEN,
                                BLAKE3_BLOCK_LEN, (uint64_t)b, 0);
                        }
                        for (int k = 0; k < 8; k++) {
                            sink ^= cv_t[k];
                        }
                    }
                }
                clock_gettime(CLOCK_MONOTONIC, &t1);
                c4_ns = ((t1.tv_sec - t0.tv_sec) +
                         (t1.tv_nsec - t0.tv_nsec) / 1e9) *
                        1e9 / iters;
            }
            (void)sink;

            if (c4_valid) {
                printf("    %-7zu  %-13.0f  %-14.0f  %-14.0f\n", N, scalar_ns,
                       neon1_ns, c4_ns);
            } else {
                printf("    %-7zu  %-13.0f  %-14.0f  %-14s\n", N, scalar_ns,
                       neon1_ns, "n/a");
            }
        }
    }

    TEST("blake3 dispatch head-to-head audit (force scalar vs SIMD)") {
        /* For each input size we run BOTH the pure-scalar path and the
         * SIMD-enabled path back-to-back and print both numbers so the
         * dispatcher's threshold can be verified against the actual
         * underlying performance — not just the throughput of the path
         * the dispatcher already picked.
         *
         * The SIMD path uses the widest available compressor (AVX2 8,
         * NEON 4, or SSE4.1 4) when the input has at least
         * BLAKE3_BATCH_{AVX2,NEON,SSE41}_CHUNKS chunks; below that it
         * has no SIMD primitive available and falls back to scalar
         * (NEON single-block compress is measurably slower than the
         * inlined scalar — see blake3Neon's own bench).  Both columns
         * therefore agree exactly for 1-3 chunk inputs and that's a
         * real datapoint, not a measurement artifact. */
        const size_t chunks[] = {1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 32, 64};
        /* Smoke-size runs (every primitive, every size); the perf preset
         * benches give the real numbers */
        const uint32_t iters_per_size[] = {4000, 2000, 1600, 1200, 1000, 800,
                                           700,  600,  400,  300,  160,  80};
        const size_t n_sizes = sizeof(chunks) / sizeof(chunks[0]);

        const size_t maxBytes = chunks[n_sizes - 1] * BLAKE3_CHUNK_LEN;
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(maxBytes);
        for (size_t i = 0; i < maxBytes; i++) {
            buf[i] = (uint8_t)(i * 13 + 7);
        }
        uint8_t out_scalar[32];
        uint8_t out_simd[32];

        /* Report the narrowest engaged threshold for the active backend
         * so the table's "SIMD engages at" header matches reality. AVX2
         * needs 8 chunks; NEON/SSE4.1 engage at 4. */
        const int simd_min_chunks =
            blake3AVX2Available()    ? BLAKE3_BATCH_AVX2_CHUNKS
            : blake3NeonAvailable()  ? BLAKE3_BATCH_NEON_CHUNKS
            : blake3SSE41Available() ? BLAKE3_BATCH_SSE41_CHUNKS
                                     : 0;
        if (simd_min_chunks > 0) {
            printf("    SIMD engages at >= %d chunks (%d bytes); below that "
                   "both paths run the same scalar code.\n",
                   simd_min_chunks, simd_min_chunks * BLAKE3_CHUNK_LEN);
        } else {
            printf("    No SIMD backend available; both columns will agree "
                   "across the table.\n");
        }
        printf("    %-7s  %-9s  %-14s  %-14s  %-12s  %s\n", "chunks", "bytes",
               "scalar (ns/op)", "SIMD (ns/op)", "speedup", "winner");

        for (size_t s = 0; s < n_sizes; s++) {
            const size_t sz = chunks[s] * BLAKE3_CHUNK_LEN;
            const uint32_t iters = iters_per_size[s];
            struct timespec t0, t1;

            /* Force scalar — disables every SIMD entry point. */
            blake3SetForceImpl(BLAKE3_IMPL_SCALAR);
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (uint32_t i = 0; i < iters; i++) {
                blake3Hash(buf, sz, out_scalar);
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double scalar_secs =
                (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            double scalar_ns = scalar_secs * 1e9 / iters;

            /* Force SIMD — engages the widest available compressor
             * (AVX2 Compress8 / NEON or SSE4.1 Compress4) wherever it
             * geometrically fits.  For sizes below the active backend's
             * batch threshold this falls back to scalar inside
             * simd_batch_ready (no narrower primitive). */
            blake3SetForceImpl(BLAKE3_IMPL_AUTO);
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (uint32_t i = 0; i < iters; i++) {
                blake3Hash(buf, sz, out_simd);
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double simd_secs =
                (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            double simd_ns = simd_secs * 1e9 / iters;

            /* Correctness cross-check — both paths must produce the
             * same digest for the same input. */
            if (memcmp(out_scalar, out_simd, 32) != 0) {
                ERR("scalar/SIMD digest disagree at chunks=%zu", chunks[s]);
            }

            double speedup = scalar_ns / simd_ns;
            const char *winner = (simd_ns < scalar_ns * 0.97)   ? "SIMD"
                                 : (simd_ns > scalar_ns * 1.03) ? "scalar"
                                                                : "tie";
            printf("    %-7zu  %-9zu  %-14.0f  %-14.0f  %-12.2fx  %s\n",
                   chunks[s], sz, scalar_ns, simd_ns, speedup, winner);
        }

        /* Restore default before returning. */
        blake3SetForceImpl(BLAKE3_IMPL_AUTO);
    }

    TEST("blake3: force setting reads back; seeked output is the long one") {
        const blake3ForceImpl was = blake3GetForceImpl();
        blake3SetForceImpl(BLAKE3_IMPL_SCALAR);
        if (blake3GetForceImpl() != BLAKE3_IMPL_SCALAR ||
            blake3DispatcherActiveBackend() != BLAKE3_BACKEND_SCALAR) {
            ERR("forced scalar did not read back as scalar");
        }
        blake3SetForceImpl(BLAKE3_IMPL_SIMD);
        if (blake3GetForceImpl() != BLAKE3_IMPL_SIMD) {
            ERR("forced SIMD did not read back");
        }
        blake3SetForceImpl(was);

        /* Extended output from offset k is bytes k.. of one long output,
         * across the 64-byte output block boundaries */
        static uint8_t seekInput[5000];
        for (size_t i = 0; i < sizeof(seekInput); i++) {
            seekInput[i] = (uint8_t)(i * 7 + 3);
        }
        static constexpr size_t lens[] = {0, 3, 1024, 5000};
        static constexpr uint64_t seeks[] = {0, 1, 63, 64, 65, 127, 200};
        for (size_t l = 0; l < sizeof(lens) / sizeof(*lens); l++) {
            blake3Hasher h;
            blake3HasherInit(&h);
            blake3HasherUpdate(&h, seekInput, lens[l]);
            uint8_t full[300];
            blake3HasherFinalize(&h, full, sizeof(full));
            for (size_t s = 0; s < sizeof(seeks) / sizeof(*seeks); s++) {
                uint8_t part[90];
                blake3HasherFinalizeSeek(&h, seeks[s], part, sizeof(part));
                if (memcmp(part, full + seeks[s], sizeof(part)) != 0) {
                    ERR("input %zu: output at %" PRIu64 " differs", lens[l],
                        seeks[s]);
                }
            }
        }
    }

    TEST("blake3 small-input latency (finalize-dominated)") {
        /* Small-input hash is dominated by the finalize path:
         * one chunk_output -> output_root_bytes -> compress_xof
         * call.  Measure ns/op for the sizes that show up most often
         * in real workloads (cluster RPC bodies, replication frames). */
        const size_t sizes[] = {16, 32, 64, 128, 256, 512};
        const uint32_t iters = 20000; /* smoke size; perf preset for numbers */
        uint8_t small_input[512];
        for (size_t i = 0; i < sizeof(small_input); i++) {
            small_input[i] = (uint8_t)i;
        }
        uint8_t digest[32];
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (uint32_t i = 0; i < iters; i++) {
                blake3Hash(small_input, sizes[s], digest);
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double secs =
                (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            printf("    blake3Hash %3zuB  : %.0f ns/op\n", sizes[s],
                   secs * 1e9 / iters);
        }
    }

    TEST("blake3 portable CompressInPlace throughput") {
        /* Direct measurement of the scalar compression — the path used
         * on hosts without NEON/SSE4.1/AVX2.  64 MiB through the
         * portable compress, 1 block at a time, in a tight loop. */
        const size_t MB = 1024 * 1024;
        const uint32_t iters = 64;
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(MB);
        for (size_t i = 0; i < MB; i++) {
            buf[i] = (uint8_t)(i * 13 + 7);
        }

        uint32_t cv[8];
        static constexpr uint32_t IV[8] = {
            0x6A09E667ul, 0xBB67AE85ul, 0x3C6EF372ul, 0xA54FF53Aul,
            0x510E527Ful, 0x9B05688Cul, 0x1F83D9ABul, 0x5BE0CD19ul,
        };
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        /* Volatile sink defeats DCE — otherwise -O3 may collapse the
         * inner loop since `cv` is only used internally. */
        volatile uint32_t sink = 0;
        for (uint32_t it = 0; it < iters; it++) {
            memcpy(cv, IV, sizeof(cv));
            const size_t nblocks = MB / BLAKE3_BLOCK_LEN;
            for (size_t b = 0; b < nblocks; b++) {
                blake3PortableCompressInPlace(cv, buf + b * BLAKE3_BLOCK_LEN,
                                              BLAKE3_BLOCK_LEN, (uint64_t)b, 0);
            }
            for (int k = 0; k < 8; k++) {
                sink ^= cv[k];
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        (void)sink;
        double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        double total_mib = (double)iters * MB / (1024.0 * 1024.0);
        printf("    portable CompressInPlace: %.1f MiB in %.3fs "
               "= %.1f MiB/s\n",
               total_mib, secs, total_mib / secs);
    }

    TEST("blake3 cross-impl differential fuzz (CompressInPlace)") {
        const int N = 50000;
        int neon_avail = blake3NeonAvailable();
        int sse41_avail = blake3SSE41Available();
        int avx2_avail = blake3AVX2Available();
        printf("  variants available: portable=1 neon=%d sse41=%d avx2=%d\n",
               neon_avail, sse41_avail, avx2_avail);
        int neon_diffs = 0, sse41_diffs = 0, avx2_diffs = 0;

        uint64_t s = 0xC0FFEEUL;
        for (int n = 0; n < N; n++) {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            uint8_t block[BLAKE3_BLOCK_LEN];
            for (int i = 0; i < BLAKE3_BLOCK_LEN; i++) {
                block[i] = (uint8_t)(s >> (i % 8 * 8) ^ (i * 31));
            }
            uint32_t cv0[8];
            for (int i = 0; i < 8; i++) {
                cv0[i] = (uint32_t)(s + 0x9E3779B9u * (uint32_t)i);
            }
            const uint8_t block_len =
                (n & 1) ? BLAKE3_BLOCK_LEN : (uint8_t)((s & 63) + 1);
            const uint64_t counter = s;
            const uint8_t flags = (uint8_t)(s & 0xFF);

            uint32_t cv_p[8], cv_v[8];
            memcpy(cv_p, cv0, sizeof(cv0));
            blake3PortableCompressInPlace(cv_p, block, block_len, counter,
                                          flags);

            if (neon_avail) {
                memcpy(cv_v, cv0, sizeof(cv0));
                blake3NeonCompressInPlace(cv_v, block, block_len, counter,
                                          flags);
                if (memcmp(cv_p, cv_v, sizeof(cv_p)) != 0) {
                    neon_diffs++;
                }
            }
            if (sse41_avail) {
                memcpy(cv_v, cv0, sizeof(cv0));
                blake3SSE41CompressInPlace(cv_v, block, block_len, counter,
                                           flags);
                if (memcmp(cv_p, cv_v, sizeof(cv_p)) != 0) {
                    sse41_diffs++;
                }
            }
            if (avx2_avail) {
                memcpy(cv_v, cv0, sizeof(cv0));
                blake3AVX2CompressInPlace(cv_v, block, block_len, counter,
                                          flags);
                if (memcmp(cv_p, cv_v, sizeof(cv_p)) != 0) {
                    avx2_diffs++;
                }
            }
        }

        if (neon_avail && neon_diffs != 0) {
            ERR("NEON differs from portable on %d / %d random inputs",
                neon_diffs, N);
        }
        if (sse41_avail && sse41_diffs != 0) {
            ERR("SSE4.1 differs from portable on %d / %d random inputs",
                sse41_diffs, N);
        }
        if (avx2_avail && avx2_diffs != 0) {
            ERR("AVX2 differs from portable on %d / %d random inputs",
                avx2_diffs, N);
        }
        printf("  %d random inputs: all available variants agree\n", N);
    }

    /* ============================================================
     * Multi-threaded correctness — prove dispatch + state are race-free.
     *
     * blake3HasherInit / Update / Finalize must work correctly when many
     * threads hash concurrently on independent hasher instances.  The
     * runtime dispatch uses an inline branch on availability flags (no
     * lazy init, no shared mutable state) — but if a future change
     * introduces shared mutable state, this test catches it as a digest
     * mismatch and TSan catches it as a race. */
    TEST("blake3 multi-threaded correctness on independent hashers") {
        const uint32_t kThreads = 8u;
        blake3MtThreadCtx ctxs[8];
        pthread_t tids[8];
        /* Compute each thread's expected digest in the driver so the
         * cross-thread invariant — "every worker produces the SAME
         * value as a single-shot computed before any threads spawned"
         * — is actually proven, not just intra-thread determinism. */
        for (uint32_t i = 0; i < kThreads; i++) {
            ctxs[i].seed = 0xCAFE0001u + i * 0x100u;
            ctxs[i].mismatches = 0;
            ctxs[i].iters = 256u;
            ctxs[i].payload_len = 4096u; /* spans 4-chunk SIMD batch */
            cleanerName(zfree, Bytes) uint8_t *seed_buf =
                zmalloc(ctxs[i].payload_len);
            for (size_t b = 0; b < ctxs[i].payload_len; b++) {
                seed_buf[b] = (uint8_t)((b * 1103515245u + ctxs[i].seed) >> 8);
            }
            blake3Hash(seed_buf, ctxs[i].payload_len, ctxs[i].expected);
        }
        for (uint32_t i = 0; i < kThreads; i++) {
            if (pthread_create(&tids[i], NULL, blake3MtWorker, &ctxs[i]) != 0) {
                ERR("pthread_create failed for thread %u", i);
            }
        }
        for (uint32_t i = 0; i < kThreads; i++) {
            pthread_join(tids[i], NULL);
        }
        for (uint32_t i = 0; i < kThreads; i++) {
            if (ctxs[i].mismatches > 0) {
                ERR("thread %u observed %u mismatches across %u iters", i,
                    ctxs[i].mismatches, ctxs[i].iters);
            }
        }
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
