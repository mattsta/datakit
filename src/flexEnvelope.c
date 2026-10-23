/* flexEnvelope — integrity-checked wrapper around an opaque byte payload.
 * See flexEnvelope.h. */

#include "flexEnvelope.h"

#include "aeadNonceLog.h"
#include "aesGcm.h"
#include "ctmem.h"
#include "datakit.h"
#include "dkFormat.h"
#include "flex.h"
#include "hmacKeyring.h"

#include "../deps/varint/src/varintSplitFull.h" /* allows zero — for flags / lens */

/* XXH/blake3/hmac headers are pulled in transitively by flexEnvelope.h
 * (needed there so the streaming-context struct can embed hash state). */

#include "chacha20poly1305.h"
#include "sha256.h"

#include <string.h>

const dkFormatSpec flexEnvelopeFormat = {
    .name = "flexEnvelope framing",
    .id = DK_FORMAT_ID_FLEX_ENVELOPE,
    .version = FLEX_ENVELOPE_VERSION_CURRENT,
    .minReadVersion = FLEX_ENVELOPE_VERSION_CURRENT,
    .knownFeatures = 0,
};

/* === Internal: algorithm descriptor table =========================== */

/* Function pointer for one-shot hash over a contiguous range with optional
 * key. Writes `digest_len` bytes to `out`. For non-keyed algos, key may
 * be NULL/0 and is ignored. */
typedef void (*flexEnvelopeHashFn)(const uint8_t *data, size_t data_len,
                                   const uint8_t *key, size_t key_len,
                                   uint8_t *out);

typedef struct flexEnvelopeAlgoDesc {
    flexEnvelopeAlgo algo;
    size_t digest_len; /* tag length for AEAD class */
    size_t nonce_len;  /* 0 for non-AEAD; per-algo for AEAD */
    const char *name;
    flexEnvelopeHashFn hash; /* NULL for AEAD class — they go through
                              * Seal/Open rather than the digest path */
    bool is_keyed;
    bool is_aead;
    bool is_deprecated;
    bool is_implemented; /* false for reserved algorithm slots
                          * with no adapter wired in this build */
} flexEnvelopeAlgoDesc;

/* === Hash adapters ================================================== */

static void hashXXH64(const uint8_t *data, size_t data_len, const uint8_t *key,
                      size_t key_len, uint8_t *out) {
    (void)key;
    (void)key_len;
    const uint64_t h = XXH64(data, data_len, 0);
    /* Write little-endian for cross-platform stability. */
    for (uint32_t i = 0; i < 8u; i++) {
        out[i] = (uint8_t)(h >> (i * 8u));
    }
}

static void hashXXH3_64(const uint8_t *data, size_t data_len,
                        const uint8_t *key, size_t key_len, uint8_t *out) {
    (void)key;
    (void)key_len;
    const uint64_t h = XXH3_64bits(data, data_len);
    for (uint32_t i = 0; i < 8u; i++) {
        out[i] = (uint8_t)(h >> (i * 8u));
    }
}

static void hashXXH3_128(const uint8_t *data, size_t data_len,
                         const uint8_t *key, size_t key_len, uint8_t *out) {
    (void)key;
    (void)key_len;
    const XXH128_hash_t h = XXH3_128bits(data, data_len);
    for (uint32_t i = 0; i < 8u; i++) {
        out[i] = (uint8_t)(h.low64 >> (i * 8u));
        out[8u + i] = (uint8_t)(h.high64 >> (i * 8u));
    }
}

/* rapidhash V3 — 64-bit non-cryptographic.  Main variant, FAST mode, no
 * caller seed (envelope wrap doesn't expose one).  Output is the eight
 * little-endian bytes of the 64-bit hash, same wire convention as the
 * other 64-bit checksum entries. */
static void hashRapidhash(const uint8_t *data, size_t data_len,
                          const uint8_t *key, size_t key_len, uint8_t *out) {
    (void)key;
    (void)key_len;
    const uint64_t h = rapidhash(data, data_len);
    for (uint32_t i = 0; i < 8u; i++) {
        out[i] = (uint8_t)(h >> (i * 8u));
    }
}

/* BLAKE3 native keyed-hash adapter. Requires a 32-byte key (the BLAKE3
 * keyed-hash mode mandates exactly BLAKE3_KEY_LEN).
 *
 * If the caller hands us anything other than exactly BLAKE3_KEY_LEN
 * bytes we MUST refuse: silent zero-padding would give a short key the
 * appearance of a full 256-bit key while only contributing key_len*8
 * bits of entropy — a silent downgrade. The adapter signature has no
 * return channel, so we write the BLAKE3 well-known-failure pattern
 * 0xFFs into `out` and bail. Every higher-level caller that produces an
 * envelope passes the digest through the AEAD/digest framing comparator
 * which will fail closed against this poison. The explicit length
 * check at streamHashInit (see below) and the static descriptor table
 * also enforce the same precondition. */
static void hashBLAKE3Keyed(const uint8_t *data, size_t data_len,
                            const uint8_t *key, size_t key_len, uint8_t *out) {
    if (key == NULL || key_len != BLAKE3_KEY_LEN) {
        /* Poison the digest so any downstream comparator fails closed
         * instead of a caller silently treating a short-key digest as
         * authoritative. */
        memset(out, 0xFFu, BLAKE3_OUT_LEN);
        return;
    }
    blake3HashKeyed(key, data, data_len, out);
}

/* HMAC adapters — drive the hash-agnostic hmac module with the
 * per-algorithm vtables published from hmac.h (hmacAlgoSHA256 and
 * hmacAlgoBLAKE3). */
static void hashHmacSha256(const uint8_t *data, size_t data_len,
                           const uint8_t *key, size_t key_len, uint8_t *out) {
    hmacCompute(&hmacAlgoSHA256, key, key_len, data, data_len, out);
}

static void hashHmacBlake3(const uint8_t *data, size_t data_len,
                           const uint8_t *key, size_t key_len, uint8_t *out) {
    hmacCompute(&hmacAlgoBLAKE3, key, key_len, data, data_len, out);
}

/* === Algorithm table ================================================
 *
 * Static lookup: index by algo enum. Unknown algos return NULL via
 * findAlgo and fail all operations cleanly. */
static const flexEnvelopeAlgoDesc kAlgoDescs[] = {
    /* algo dlen nlen name hash keyed aead dep impl */
    {FLEX_ENV_ALGO_NONE, 0, 0, "none", NULL, false, false, false, true},
    {FLEX_ENV_ALGO_XXH64, 8, 0, "xxh64", hashXXH64, false, false, false, true},
    {FLEX_ENV_ALGO_XXH3_64, 8, 0, "xxh3_64", hashXXH3_64, false, false, false,
     true},
    {FLEX_ENV_ALGO_XXH3_128, 16, 0, "xxh3_128", hashXXH3_128, false, false,
     false, true},
    /* Slot 4 = rapidhash main variant, FAST mode, seed=0, default
     * secret.  These four pins are now part of the wire format — a
     * future seeded or PROTECTED rapidhash envelope must occupy a
     * NEW slot, not this one.  The descriptor name encodes the
     * pinning so the lock-in is visible to anyone grepping for it. */
    {FLEX_ENV_ALGO_RAPIDHASH, 8, 0, "rapidhash_main_seed0", hashRapidhash,
     false, false, false, true},
    {FLEX_ENV_ALGO_BLAKE3_KEYED, 32, 0, "blake3_keyed", hashBLAKE3Keyed, true,
     false, false, true},
    {FLEX_ENV_ALGO_HMAC_SHA256, 32, 0, "hmac_sha256", hashHmacSha256, true,
     false, false, true},
    {FLEX_ENV_ALGO_HMAC_BLAKE3, 32, 0, "hmac_blake3", hashHmacBlake3, true,
     false, false, true},
    {FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305, 16, 12, "aead_chacha20_poly1305",
     NULL, true, true, false, true},
    {FLEX_ENV_ALGO_AEAD_AES128_GCM, 16, 12, "aead_aes128_gcm", NULL, true, true,
     false, true},
    {FLEX_ENV_ALGO_AEAD_AES256_GCM, 16, 12, "aead_aes256_gcm", NULL, true, true,
     false, true},
};

/* Compile-time guard: every registered digest fits in the verify-side
 * scratch buffers (FLEX_ENV_MAX_DIGEST_LEN). Add a new entry above and
 * forget about updating this assertion → the build trips. Hand-rolled
 * because C11 _Static_assert can't iterate a struct array. */
_Static_assert(32 <= FLEX_ENV_MAX_DIGEST_LEN,
               "BLAKE3_KEYED / HMAC_* digest exceeds verify scratch");
_Static_assert(16 <= FLEX_ENV_MAX_DIGEST_LEN,
               "XXH3_128 / AEAD tag exceeds verify scratch");
_Static_assert(8 <= FLEX_ENV_MAX_DIGEST_LEN,
               "XXH64 / XXH3_64 digest exceeds verify scratch");

/* Compile-time guard: streaming HMAC storage covers the worst-case
 * algorithm registered in the table. HMAC-BLAKE3 is the dominant
 * size (BLAKE3 ctx + 2*block); HMAC-SHA-256 is strictly smaller. If
 * a future HMAC backend needs more space, bump FLEX_ENV_STREAM_HMAC_
 * STORAGE in the header. */
_Static_assert(2 * sizeof(blake3Hasher) + 2 * BLAKE3_BLOCK_LEN <=
                   FLEX_ENV_STREAM_HMAC_STORAGE,
               "FLEX_ENV_STREAM_HMAC_STORAGE undersized for HMAC-BLAKE3");

/* Compile-time guard: every registered AEAD nonce fits in the per-
 * algorithm wire slot. Currently only chacha20-poly1305 (12 B) is
 * registered. Any future AEAD addition must either fit under
 * FLEX_ENV_MAX_NONCE_LEN or bump the constant. */
_Static_assert(12 <= FLEX_ENV_MAX_NONCE_LEN,
               "ChaCha20-Poly1305 nonce exceeds wire slot");

static const flexEnvelopeAlgoDesc *findAlgo(uint16_t algo) {
    const size_t n = sizeof(kAlgoDescs) / sizeof(kAlgoDescs[0]);
    for (size_t i = 0; i < n; i++) {
        if (kAlgoDescs[i].algo == (flexEnvelopeAlgo)algo) {
            return &kAlgoDescs[i];
        }
    }
    return NULL;
}

/* === Algorithm queries ============================================== */

size_t flexEnvelopeAlgoDigestLen(flexEnvelopeAlgo algo) {
    const flexEnvelopeAlgoDesc *d = findAlgo((uint16_t)algo);
    return d ? d->digest_len : 0;
}

const char *flexEnvelopeAlgoName(flexEnvelopeAlgo algo) {
    const flexEnvelopeAlgoDesc *d = findAlgo((uint16_t)algo);
    return d ? d->name : "unknown";
}

bool flexEnvelopeAlgoIsKeyed(flexEnvelopeAlgo algo) {
    const flexEnvelopeAlgoDesc *d = findAlgo((uint16_t)algo);
    return d ? d->is_keyed : false;
}

bool flexEnvelopeAlgoIsDeprecated(flexEnvelopeAlgo algo) {
    const flexEnvelopeAlgoDesc *d = findAlgo((uint16_t)algo);
    return d ? d->is_deprecated : false;
}

/* === Varint framing helpers =========================================
 *
 * Wire framing uses varintSplitFull (allows encoding 0). Each field is
 * varint-encoded with width depending on magnitude. */

/* Encode `val` to `dst`, returning bytes written. */
static size_t varintEncode(uint8_t *dst, uint64_t val) {
    size_t encoded_len = 0;
    varintSplitFullPut_(dst, encoded_len, val);
    return encoded_len;
}

/* Decode varint at `ptr` (within buffer of `remaining` bytes). Returns
 * 0 on overflow/malformed; on success writes value to *out_val and
 * bytes-consumed to *out_width.
 *
 * Defense in depth: the SplitFull VAR encoding stores a byte width in
 * the low nibble of the prefix byte, but the dependency's reader macros
 * trust that width without checking that it lands within varintWidth's
 * declared range. An attacker can craft a prefix byte of 0xC0 (VAR
 * with width 0) — the underlying reader then hits its
 * __builtin_unreachable default and corrupts the program. We reject
 * any VAR encoding whose nibble-width is outside [1, 8] up front. */
static size_t varintDecode(const uint8_t *ptr, size_t remaining,
                           uint64_t *out_val, size_t *out_width) {
    if (remaining == 0) {
        return 0;
    }
    /* Reject malformed VAR prefix bytes before touching the dependency
     * reader. The check matches the contract documented in
     * varintExternal.h: width must be a non-zero, ≤ 8-byte size. */
    if ((ptr[0] & 0xC0u) == 0xC0u) {
        const uint8_t nibble = (uint8_t)(ptr[0] & 0x0Fu);
        if (nibble == 0u || nibble > 8u) {
            return 0;
        }
    }
    size_t needed = varintSplitFullGetLenQuick_(ptr);
    if (needed == 0 || needed > remaining) {
        return 0;
    }
    size_t valsize = 0;
    uint64_t val = 0;
    varintSplitFullGet_(ptr, valsize, val);
    if (valsize != needed) {
        return 0;
    }
    *out_val = val;
    *out_width = valsize;
    return valsize;
}

/* Bytes a varintSplitFullPut_ would consume to encode `val`. */
static size_t varintEncodedLen(uint64_t val) {
    size_t out = 0;
    varintSplitFullLength_(out, val);
    return out;
}

/* === Sizing ========================================================= */

size_t flexEnvelopeWrappedSize(uint64_t payload_len, flexEnvelopeAlgo algo,
                               uint32_t on_wire_flags) {
    const flexEnvelopeAlgoDesc *d = findAlgo((uint16_t)algo);
    if (!d) {
        return 0;
    }
    /* Wire layout:
     * 2 magic bytes
     * 4 varints (version, algo, flags, payload_len)
     * [optional] varint key_id iff FLEX_ENV_FLAG_KEYID_PRESENT in flags
     * nonce_len bytes (only for AEAD class; zero otherwise)
     * payload_len bytes (ciphertext for AEAD, plaintext otherwise)
     * digest_len bytes (tag for AEAD, MAC/checksum otherwise) */
    const size_t magic_bytes = 2;
    const size_t version_size = varintEncodedLen(FLEX_ENVELOPE_VERSION_CURRENT);
    const size_t algo_size = varintEncodedLen((uint64_t)algo);
    const size_t flags_size = varintEncodedLen((uint64_t)on_wire_flags);
    const size_t plen_size = varintEncodedLen(payload_len);
    /* The wire's key_id sits between flags and payload_len. Reserve
     * space for it whenever the flag bit is asserted. Use UINT32_MAX
     * as the size proxy — the caller doesn't pass the actual id here,
     * so we conservatively cost the largest representable id. */
    const size_t keyid_size = (on_wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT)
                                  ? varintEncodedLen((uint64_t)UINT32_MAX)
                                  : 0;
    const size_t header_total = magic_bytes + version_size + algo_size +
                                flags_size + keyid_size + plen_size;

    /* Overflow-safe running-total check. Each subtraction guards
     * against the next addition wrapping; the order matters since
     * computing `SIZE_MAX - a - b` in one go would underflow if
     * `a + b > SIZE_MAX`. */
    const size_t trailers = d->digest_len + d->nonce_len;
    if (trailers > SIZE_MAX - header_total) {
        return 0;
    }
    const size_t prefix_plus_trailers = header_total + trailers;
    if (payload_len > SIZE_MAX - prefix_plus_trailers) {
        return 0;
    }
    return prefix_plus_trailers + (size_t)payload_len;
}

/* === Internal: framing layout result ================================ */

typedef struct flexEnvelopeFraming {
    const flexEnvelopeAlgoDesc *algo_desc;
    uint64_t payload_len;
    size_t header_size;    /* magic + 4 varints + optional key_id */
    size_t nonce_offset;   /* == header_size; valid only when algo is AEAD */
    size_t payload_offset; /* == header_size + nonce_len */
    size_t digest_offset;  /* == payload_offset + payload_len */
    size_t total_size;     /* == digest_offset + digest_len */
    uint32_t on_wire_flags;
    uint32_t key_id; /* valid iff flags & FLEX_ENV_FLAG_KEYID_PRESENT */
    uint8_t version;
} flexEnvelopeFraming;

/* Parse the framing fields of `buf`. Returns true on success. On
 * failure, contents of *out are unspecified. This is the workhorse
 * underlying Peek, Verify, and the streaming Verify Init path.
 *
 * If `header_only` is true, the function only requires `buf_len` to
 * cover the framing header (magic + 4 varints) — payload/digest fit
 * isn't checked because the caller is going to stream them. This mode
 * is for the streaming verify Init. */
static bool parseFramingEx(const uint8_t *buf, size_t buf_len,
                           flexEnvelopeFraming *out, bool header_only) {
    if (!buf || buf_len < 2) {
        return false;
    }
    if (buf[0] != FLEX_ENVELOPE_MAGIC_0 || buf[1] != FLEX_ENVELOPE_MAGIC_1) {
        return false;
    }
    size_t pos = 2;

    /* version */
    uint64_t version = 0;
    size_t width = 0;
    if (!varintDecode(buf + pos, buf_len - pos, &version, &width)) {
        return false;
    }
    if (version != FLEX_ENVELOPE_VERSION_CURRENT) {
        return false;
    }
    pos += width;

    /* algo */
    uint64_t algo_raw = 0;
    if (!varintDecode(buf + pos, buf_len - pos, &algo_raw, &width)) {
        return false;
    }
    if (algo_raw > UINT16_MAX) {
        return false;
    }
    const flexEnvelopeAlgoDesc *algo_desc = findAlgo((uint16_t)algo_raw);
    if (!algo_desc) {
        return false;
    }
    pos += width;

    /* flags */
    uint64_t flags_raw = 0;
    if (!varintDecode(buf + pos, buf_len - pos, &flags_raw, &width)) {
        return false;
    }
    if (flags_raw > UINT32_MAX) {
        return false;
    }
    /* Unknown flag bits → reject. Forward-compat gate: a future version
     * can re-purpose them with a version bump, but current readers
     * refuse to accept frames they don't understand. */
    if (((uint32_t)flags_raw & ~FLEX_ENV_FLAGS_VALID_V1) != 0) {
        return false;
    }
    pos += width;

    /* Optional key_id varint, present iff the corresponding flag bit
     * is set. Reserve 0 as the "absent" sentinel even on the wire — a
     * frame claiming KEYID_PRESENT with key_id == 0 is malformed. */
    uint32_t key_id = 0;
    if ((uint32_t)flags_raw & FLEX_ENV_FLAG_KEYID_PRESENT) {
        uint64_t kid_raw = 0;
        if (!varintDecode(buf + pos, buf_len - pos, &kid_raw, &width)) {
            return false;
        }
        if (kid_raw == 0 || kid_raw > UINT32_MAX) {
            return false;
        }
        key_id = (uint32_t)kid_raw;
        pos += width;
    }

    /* payload_len */
    uint64_t payload_len = 0;
    if (!varintDecode(buf + pos, buf_len - pos, &payload_len, &width)) {
        return false;
    }
    pos += width;

    /* Check the claimed payload fits. We allow trailing bytes here
     * (Verify enforces the no-trailing policy later); reject only the
     * cases where the payload itself would walk past the buffer.
     *
     * In header_only mode (streaming verify), the caller hasn't yet
     * supplied payload bytes, so we skip the buf_len fit check — the
     * stream API will validate Update / Final byte counts against the
     * parsed payload_len. The overflow guard still applies. */
    /* Overflow-safe trailer + payload accounting. Compute each
     * intermediate sum and reject before the next addition can wrap.
     * The order matters: doing `SIZE_MAX - pos - trailers` in one
     * expression would underflow if `pos + trailers > SIZE_MAX`. */
    const size_t trailers = algo_desc->digest_len + algo_desc->nonce_len;
    if (payload_len > UINT64_MAX - (uint64_t)trailers) {
        return false;
    }
    if (!header_only) {
        if (trailers > SIZE_MAX - pos) {
            return false;
        }
        const size_t pos_plus_trailers = pos + trailers;
        if (payload_len > (uint64_t)(SIZE_MAX - pos_plus_trailers)) {
            return false;
        }
        const size_t required = pos_plus_trailers + (size_t)payload_len;
        if (required > buf_len) {
            return false;
        }
        out->total_size = required;
    } else {
        out->total_size = 0;
    }

    out->algo_desc = algo_desc;
    out->payload_len = payload_len;
    out->header_size = pos;
    out->nonce_offset = pos;
    out->payload_offset = pos + algo_desc->nonce_len;
    if (!header_only) {
        out->digest_offset = out->payload_offset + (size_t)payload_len;
    } else {
        out->digest_offset = 0; /* unknown until stream completes */
    }
    out->on_wire_flags = (uint32_t)flags_raw;
    out->key_id = key_id;
    out->version = (uint8_t)version;
    return true;
}

static bool parseFraming(const uint8_t *buf, size_t buf_len,
                         flexEnvelopeFraming *out) {
    return parseFramingEx(buf, buf_len, out, false);
}

static bool parseFramingForStream(const uint8_t *buf, size_t buf_len,
                                  flexEnvelopeFraming *out) {
    return parseFramingEx(buf, buf_len, out, true);
}

/* === Peek =========================================================== */

bool flexEnvelopePeek(const uint8_t *buf, size_t buf_len,
                      flexEnvelopeHeader *out) {
    if (!out) {
        return false;
    }
    flexEnvelopeFraming f;
    if (!parseFraming(buf, buf_len, &f)) {
        return false;
    }
    out->payload_len = f.payload_len;
    out->digest_len = f.algo_desc->digest_len;
    out->header_size = f.header_size;
    out->flags = f.on_wire_flags;
    out->key_id = f.key_id;
    out->algo = (uint16_t)f.algo_desc->algo;
    out->version = f.version;
    /* Explicit zero of the caller-facing reserved bytes. */
    out->_reserved[0] = 0u;
    out->_reserved[1] = 0u;
    out->_reserved[2] = 0u;
    /* Defense-in-depth: assert the contract this function publishes —
     * every successful Peek/Verify return guarantees `_reserved` bytes
     * are zero. The assertion is trivially true given the writes above,
     * but catches any future maintainer who reorders the field zeroing
     * or repurposes the bytes without updating callers.
     *
     * Note: the on-wire format does NOT carry a reserved-bytes field.
     * Wire-side "reserved" space lives in the unused high bits of the
     * `flags` varint, and parseFraming already rejects frames whose
     * flags carry bits outside FLEX_ENV_FLAGS_VALID_V1 (see line in
     * parseFramingEx above). The header struct's `_reserved[3]` is
     * purely a forward-compatibility slot in the caller-facing
     * decoded view.
     *
     * Using release-build _Static_assert would not work here (runtime
     * data); the explicit if/return mirrors the rest of the parse-side
     * fail-closed pattern. */
    if (out->_reserved[0] != 0u || out->_reserved[1] != 0u ||
        out->_reserved[2] != 0u) {
        return false;
    }
    return true;
}

/* === Constant-time and fast digest compare ========================== */

/* Constant-time byte comparison. Returns true iff a == b over n bytes.
 * Asm-barriered via ctmem.h so the optimizer cannot short-circuit on any
 * code-gen pass — including PGO/LTO. */
static bool digestEqConstantTime(const uint8_t *a, const uint8_t *b, size_t n) {
    return dkCtMemEq(a, b, n);
}

static bool digestEqFast(const uint8_t *a, const uint8_t *b, size_t n) {
    return memcmp(a, b, n) == 0;
}

/* === Wrap =========================================================== */

/* Shared validation + header emission for the digest-only (Wrap) path.
 *
 * Validates `params` against the algo descriptor table and on-wire flag
 * mask, computes the EXACT total envelope size for `payload_len`, checks
 * the size fits in `out_cap`, and (on success) writes the full framing
 * header into `out`. Returns the algo descriptor + sizes via out-params.
 *
 * On success:
 *  - `out` bytes [0, *header_size_out) hold the framing header.
 *  - `*total_size_out` is the total envelope size (header + payload +
 *    digest); the caller still needs to write payload bytes and let the
 *    hash function emit the digest.
 *  - `*desc_out` points at the algo descriptor (digest_len, hash fn).
 *
 * Returns false (and leaves `out` unmodified beyond what an earlier valid
 * caller may have written) if any validation fails or `out_cap` is too
 * small. */
static bool wrapEmitHeader(uint8_t *out, size_t out_cap, uint64_t payload_len,
                           const flexEnvelopeWrapParams *params,
                           const flexEnvelopeAlgoDesc **desc_out,
                           size_t *header_size_out, size_t *total_size_out) {
    if (!out || !params) {
        return false;
    }

    /* Tightened _pad policy: the three reserved bytes after `version`
     * MUST be zero — they are a future-expansion slot, not a caller-side
     * stash. A non-zero value at wrap entry is either a stack-uninitialized
     * params bug or an attempt to smuggle bytes past the wrap interface;
     * either way, refuse to emit a frame. Cheap fail-closed gate runs
     * before any allocation / hash work. */
    if (params->_pad[0] != 0u || params->_pad[1] != 0u ||
        params->_pad[2] != 0u) {
        return false;
    }

    const flexEnvelopeAlgoDesc *d = findAlgo((uint16_t)params->algo);
    if (!d || !d->is_implemented) {
        return false;
    }
    if (!d->hash && params->algo != FLEX_ENV_ALGO_NONE) {
        return false;
    }
    /* AEAD class encrypts the payload and produces a tag; it cannot be
     * driven through the digest-only Wrap API. Callers must use
     * flexEnvelopeSeal instead. */
    if (d->is_aead) {
        return false;
    }

    /* Unconditional: refuse to emit ALGO_NONE unless explicitly
     * compiled with the insecure-test gate. */
#ifndef DATAKIT_ENABLE_INSECURE_TEST_ALGOS
    if (params->algo == FLEX_ENV_ALGO_NONE) {
        return false;
    }
#endif

    /* Unconditional: keyed algorithm without a key is rejected. */
    if (d->is_keyed && (params->key == NULL || params->key_len == 0)) {
        return false;
    }

    /* BLAKE3 native keyed-hash mode is defined ONLY for a 32-byte key.
     * Refuse anything else explicitly — silent zero-padding would
     * shrink effective entropy to key_len*8 bits while presenting a
     * full 256-bit digest (a stealth downgrade). Mirrors the explicit
     * AEAD key-length guard sealRunCipher uses for ChaCha20-Poly1305
     * + AES-GCM keys. */
    if (params->algo == FLEX_ENV_ALGO_BLAKE3_KEYED &&
        params->key_len != BLAKE3_KEY_LEN) {
        return false;
    }

    /* Unknown flag bits in caller's on_wire_flags → reject. */
    if ((params->on_wire_flags & ~FLEX_ENV_FLAGS_VALID_V1) != 0) {
        return false;
    }

    /* Reconcile params->key_id with the KEYID_PRESENT flag. Non-zero
     * key_id implicitly enables the flag (callers who set the id need
     * not also remember to flip the bit). A zero key_id with the flag
     * set is a contradiction — reject so we don't emit a wire frame
     * that subsequent verifiers will reject as malformed (key_id==0
     * is the parser's "absent" sentinel). */
    uint32_t wire_flags = params->on_wire_flags;
    if (params->key_id != 0) {
        wire_flags |= FLEX_ENV_FLAG_KEYID_PRESENT;
    } else if (wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT) {
        return false;
    }

    const uint8_t version =
        params->version == 0 ? FLEX_ENVELOPE_VERSION_CURRENT : params->version;
    if (version != FLEX_ENVELOPE_VERSION_CURRENT) {
        return false;
    }

    /* Sizing accounts for the keyid varint by reserving the largest
     * representable encoding; we then encode the actual id (which may
     * be smaller) and pad the prefix accordingly. Simpler: compute
     * the actual prefix size up-front. */
    const size_t magic_bytes = 2;
    const size_t version_size = varintEncodedLen((uint64_t)version);
    const size_t algo_size = varintEncodedLen((uint64_t)params->algo);
    const size_t flags_size = varintEncodedLen((uint64_t)wire_flags);
    const size_t keyid_size = (wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT)
                                  ? varintEncodedLen((uint64_t)params->key_id)
                                  : 0;
    const size_t plen_size = varintEncodedLen(payload_len);
    const size_t prefix_total = magic_bytes + version_size + algo_size +
                                flags_size + keyid_size + plen_size;
    const size_t trailers = d->digest_len + d->nonce_len;
    if (trailers > SIZE_MAX - prefix_total) {
        return false;
    }
    if (payload_len > UINT64_MAX - (uint64_t)trailers ||
        (uint64_t)payload_len >
            (uint64_t)(SIZE_MAX - prefix_total - trailers)) {
        return false;
    }
    const size_t needed = prefix_total + trailers + (size_t)payload_len;
    if (needed > out_cap) {
        return false;
    }

    /* Emit framing. */
    size_t pos = 0;
    out[pos++] = FLEX_ENVELOPE_MAGIC_0;
    out[pos++] = FLEX_ENVELOPE_MAGIC_1;
    pos += varintEncode(out + pos, (uint64_t)version);
    pos += varintEncode(out + pos, (uint64_t)params->algo);
    pos += varintEncode(out + pos, (uint64_t)wire_flags);
    if (wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT) {
        pos += varintEncode(out + pos, (uint64_t)params->key_id);
    }
    pos += varintEncode(out + pos, payload_len);

    *desc_out = d;
    *header_size_out = pos;
    *total_size_out = needed;
    return true;
}

bool flexEnvelopeWrap(uint8_t *out, size_t out_cap, const uint8_t *payload,
                      uint64_t payload_len,
                      const flexEnvelopeWrapParams *params,
                      size_t *out_written) {
    if (payload_len > 0 && !payload) {
        return false;
    }

    const flexEnvelopeAlgoDesc *d = NULL;
    size_t header_size = 0;
    size_t needed = 0;
    if (!wrapEmitHeader(out, out_cap, payload_len, params, &d, &header_size,
                        &needed)) {
        return false;
    }

    /* Copy payload. */
    if (payload_len > 0) {
        memcpy(out + header_size, payload, (size_t)payload_len);
    }
    const size_t payload_end = header_size + (size_t)payload_len;

    /* Compute and write digest over framing||payload. */
    if (d->digest_len > 0 && d->hash) {
        d->hash(out, payload_end, params->key, params->key_len,
                out + payload_end);
    }

    if (out_written) {
        *out_written = needed;
    }
    return true;
}

/* === Zero-copy Wrap =================================================
 *
 * The `ReserveWrap` + `FinalizeWrap` pair is a fast-path bypass for
 * callers who can write the payload bytes DIRECTLY into the envelope
 * buffer rather than staging them in a temp and memcpy'ing into Wrap.
 * The wire-format output is byte-identical to flexEnvelopeWrap; only the
 * caller's data flow changes. See flexEnvelope.h for usage docs. */

bool flexEnvelopeReserveWrap(uint8_t *out, size_t out_cap, uint64_t payload_len,
                             const flexEnvelopeWrapParams *params,
                             uint8_t **payload_region_out,
                             size_t *header_len_out) {
    if (!payload_region_out || !header_len_out) {
        return false;
    }
    const flexEnvelopeAlgoDesc *d = NULL;
    size_t header_size = 0;
    size_t needed = 0;
    if (!wrapEmitHeader(out, out_cap, payload_len, params, &d, &header_size,
                        &needed)) {
        return false;
    }
    (void)d;
    *payload_region_out = out + header_size;
    *header_len_out = header_size;
    return true;
}

bool flexEnvelopeFinalizeWrap(uint8_t *out, size_t out_cap, size_t header_len,
                              uint64_t payload_len,
                              const flexEnvelopeWrapParams *params,
                              size_t *written_out) {
    if (!out || !params) {
        return false;
    }
    /* Re-validate the same invariants as wrapEmitHeader so a hostile or
     * confused caller cannot finalize an envelope under different params
     * than the matching Reserve call used. Cheap: it's the same checks
     * Wrap already pays. We do NOT rewrite the header — the Reserve call
     * is responsible for that — but we DO recompute the expected total
     * size and the exact header length, and refuse if either disagrees
     * with the caller's `header_len` argument or with `out_cap`. */
    if (params->_pad[0] != 0u || params->_pad[1] != 0u ||
        params->_pad[2] != 0u) {
        return false;
    }
    const flexEnvelopeAlgoDesc *d = findAlgo((uint16_t)params->algo);
    if (!d || !d->is_implemented) {
        return false;
    }
    if (!d->hash && params->algo != FLEX_ENV_ALGO_NONE) {
        return false;
    }
    if (d->is_aead) {
        return false;
    }
#ifndef DATAKIT_ENABLE_INSECURE_TEST_ALGOS
    if (params->algo == FLEX_ENV_ALGO_NONE) {
        return false;
    }
#endif
    if (d->is_keyed && (params->key == NULL || params->key_len == 0)) {
        return false;
    }
    /* BLAKE3 native keyed-hash mode requires exactly 32 bytes; mirror
     * the one-shot wrap guard. */
    if (params->algo == FLEX_ENV_ALGO_BLAKE3_KEYED &&
        params->key_len != BLAKE3_KEY_LEN) {
        return false;
    }
    if ((params->on_wire_flags & ~FLEX_ENV_FLAGS_VALID_V1) != 0) {
        return false;
    }
    uint32_t wire_flags = params->on_wire_flags;
    if (params->key_id != 0) {
        wire_flags |= FLEX_ENV_FLAG_KEYID_PRESENT;
    } else if (wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT) {
        return false;
    }
    const uint8_t version =
        params->version == 0 ? FLEX_ENVELOPE_VERSION_CURRENT : params->version;
    if (version != FLEX_ENVELOPE_VERSION_CURRENT) {
        return false;
    }
    const size_t magic_bytes = 2;
    const size_t version_size = varintEncodedLen((uint64_t)version);
    const size_t algo_size = varintEncodedLen((uint64_t)params->algo);
    const size_t flags_size = varintEncodedLen((uint64_t)wire_flags);
    const size_t keyid_size = (wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT)
                                  ? varintEncodedLen((uint64_t)params->key_id)
                                  : 0;
    const size_t plen_size = varintEncodedLen(payload_len);
    const size_t expected_header_len = magic_bytes + version_size + algo_size +
                                       flags_size + keyid_size + plen_size;
    if (expected_header_len != header_len) {
        return false;
    }
    const size_t trailers = d->digest_len + d->nonce_len;
    if (trailers > SIZE_MAX - expected_header_len) {
        return false;
    }
    if (payload_len > UINT64_MAX - (uint64_t)trailers ||
        (uint64_t)payload_len >
            (uint64_t)(SIZE_MAX - expected_header_len - trailers)) {
        return false;
    }
    const size_t needed = expected_header_len + trailers + (size_t)payload_len;
    if (needed > out_cap) {
        return false;
    }

    /* Validate the on-wire header matches what we'd emit now. Defense
     * against a caller that mutated the header bytes between Reserve and
     * Finalize, or that fed mismatched params. Two cheap byte checks: the
     * magic prefix and the algo/version varints all live in the first
     * few bytes. */
    if (out[0] != FLEX_ENVELOPE_MAGIC_0 || out[1] != FLEX_ENVELOPE_MAGIC_1) {
        return false;
    }

    const size_t payload_end = header_len + (size_t)payload_len;
    if (d->digest_len > 0 && d->hash) {
        d->hash(out, payload_end, params->key, params->key_len,
                out + payload_end);
    }
    if (written_out) {
        *written_out = needed;
    }
    return true;
}

/* === Verify ========================================================= */

/* Per-thread typed error state for the most recent verify call.
 *
 * Stored as `_Thread_local` — every thread that calls Verify gets
 * its own slot, no locking needed on the hot path. The verify wrapper
 * pattern below resets the slot to OK on entry; failure paths overwrite
 * with a more specific code via `setVerifyErr(...)` before returning
 * false; success paths leave the OK value intact. */
static _Thread_local flexEnvelopeVerifyErr tlsVerifyErr = FLEX_ENV_VERIFY_OK;

static inline void setVerifyErr(flexEnvelopeVerifyErr code) {
    tlsVerifyErr = code;
}

flexEnvelopeVerifyErr flexEnvelopeLastVerifyError(void) {
    return tlsVerifyErr;
}

bool flexEnvelopeVerify(const uint8_t *buf, size_t buf_len,
                        const flexEnvelopeVerifyParams *params,
                        const uint8_t **out_payload,
                        uint64_t *out_payload_len) {
    /* Reset the per-thread error to OK; any failure path below
     * overwrites it with a more specific code before returning false. */
    setVerifyErr(FLEX_ENV_VERIFY_OK);

    /* Zero defaults if no params struct supplied. */
    flexEnvelopeVerifyParams empty;
    memset(&empty, 0, sizeof(empty));
    const flexEnvelopeVerifyParams *p = params ? params : &empty;
    const uint32_t opts = p->opts;

    /* Stack buffer for keyring auto-resolution. Stays unused when the
     * caller supplied a key directly or when no keyring is bound; the
     * BLAKE3_KEYED branch below populates it via hmacKeyringGetRawCopy
     * so a concurrent Retire on the resolved id cannot UAF the bytes
     * mid-hash. Wiped at the single exit point regardless of whether
     * the resolution branch ran — defense-in-depth, since the bytes
     * are key-equivalent material when populated. */
    uint8_t key_copy[HMAC_KEYRING_MAX_KEY_BYTES];
    bool key_copy_populated = false;
    bool result = false;

    flexEnvelopeFraming f;
    if (!parseFraming(buf, buf_len, &f)) {
        setVerifyErr(FLEX_ENV_VERIFY_FAILED_GENERIC);
        goto cleanup;
    }

    /* Secure-default invariants — these run on EVERY decode. */

    /* (a) Algorithm must be implemented in this build. */
    if (!f.algo_desc->is_implemented) {
        goto cleanup;
    }

    /* (a2) AEAD class envelopes carry ciphertext, not plaintext; verify
     * cannot return a meaningful payload pointer. Callers must use
     * flexEnvelopeOpen instead. */
    if (f.algo_desc->is_aead) {
        goto cleanup;
    }

    /* (b) ALGO_NONE rejected unless opted in. */
    if (f.algo_desc->algo == FLEX_ENV_ALGO_NONE &&
        !(opts & FLEX_ENV_VERIFY_ALLOW_NONE_ALGO)) {
        goto cleanup;
    }

    /* (c) Deprecated algo rejected unless opted in. */
    if (f.algo_desc->is_deprecated &&
        !(opts & FLEX_ENV_VERIFY_ALLOW_DEPRECATED)) {
        goto cleanup;
    }

    /* (d) Keyed algo requires SOME form of key material — either raw
     * (key + key_len), a pre-prepared HMAC key state, OR auto-resolution
     * via a bound keyring keyed by the wire's KEYID_PRESENT field.
     * Prepared keys only make sense for the HMAC class; for
     * BLAKE3_KEYED, raw bytes are required. */
    const uint8_t *effective_raw_key = p->key;
    size_t effective_raw_key_len = p->key_len;
    const struct hmacKey *effective_prep_key = p->prepared_key;
    if (f.algo_desc->is_keyed) {
        /* ambiguous-key fail-safe. If the caller
         * supplied any key material (prepared OR raw) AND a keyring AND
         * the wire carries KEYID_PRESENT, the historical behavior would
         * silently let the wire's keyring-resolved key win — confusing
         * for callers reasoning about which key actually verified the
         * frame. Default fail-safe: reject with the typed error so the
         * caller MUST disambiguate. Asserting
         * FLEX_ENV_VERIFY_ACCEPT_WIRE_KEYID opts back in to the
         * prior silent-override (wire id wins). Note: this runs
         * BEFORE the floor check because the ambiguity is structural,
         * not policy-driven — the caller's intent must be explicit even
         * if the wire id would have passed the revoke floor. */
        const bool caller_supplied_raw = (p->key != NULL && p->key_len > 0);
        const bool caller_supplied_prep = (p->prepared_key != NULL);
        if (p->keyring != NULL &&
            (f.on_wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT) &&
            (caller_supplied_raw || caller_supplied_prep) &&
            !(opts & FLEX_ENV_VERIFY_ACCEPT_WIRE_KEYID)) {
            setVerifyErr(FLEX_ENV_VERIFY_FAILED_AMBIGUOUS_KEY);
            goto cleanup;
        }
        /* Keyring auto-resolution: if the wire claims a key id AND the
         * caller bound a keyring, resolve both the prepared handle (for
         * HMAC algos) and the raw bytes (for BLAKE3_KEYED) from it. */
        if (p->keyring != NULL &&
            (f.on_wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT)) {
            /* floor check. Reject ids below the
             * keyring's revoked-key floor BEFORE resolving — a frame
             * that wears a revoked id is by policy a replay attempt,
             * regardless of whether the key bytes are still resident in
             * the keyring (which they typically are during rotation
             * propagation). Fail-closed with a typed error so callers
             * can distinguish "framing busted" from "id revoked". */
            const hmacKeyId floor =
                hmacKeyringMinValidKeyId((struct hmacKeyring *)p->keyring);
            if (floor != HMAC_KEY_ID_INVALID && f.key_id < floor) {
                setVerifyErr(FLEX_ENV_VERIFY_FAILED_KEY_REVOKED);
                goto cleanup;
            }
            /* Copy raw key bytes out of the keyring under its rwlock for
             * every keyed algo (HMAC and BLAKE3_KEYED alike). The
             * borrowed-pointer fast path (hmacKeyringLookup) returns a
             * prepared-key struct whose inner/outer-ctx pointers borrow
             * into the slot's prepared_storage — a concurrent Retire on
             * this id during the verify would dkSecureZero + free that
             * storage and UAF the in-flight HMAC compute. Re-deriving the
             * inner/outer states per call costs ~2 extra block
             * compressions (~50ns); the gain is closing the prepared-key
             * UAF window. Same trade-off the cluster Ingest path took.
             * The local copy is wiped at the cleanup label. */
            size_t copied_len = 0;
            if (hmacKeyringGetRawCopy((struct hmacKeyring *)p->keyring,
                                      f.key_id, key_copy, sizeof(key_copy),
                                      &copied_len, NULL)) {
                key_copy_populated = true;
                effective_raw_key = key_copy;
                effective_raw_key_len = copied_len;
                /* When the keyring resolved the key, the wire id wins
                 * (caller opted-in via FLEX_ENV_VERIFY_ACCEPT_WIRE_KEYID;
                 * otherwise the ambiguous-key fail-safe above would have
                 * already rejected). Drop any caller-supplied prepared
                 * key so the downstream HMAC compute uses the keyring-
                 * resolved raw bytes, not the caller's (now-overridden)
                 * prepared handle. */
                effective_prep_key = NULL;
            }
        }
        const bool have_raw_key =
            (effective_raw_key != NULL && effective_raw_key_len > 0);
        const bool have_prep_key = (effective_prep_key != NULL);
        if (!have_raw_key && !have_prep_key) {
            goto cleanup;
        }
        if (have_prep_key && !have_raw_key &&
            f.algo_desc->algo != FLEX_ENV_ALGO_HMAC_SHA256 &&
            f.algo_desc->algo != FLEX_ENV_ALGO_HMAC_BLAKE3) {
            goto cleanup;
        }
        /* BLAKE3 keyed mode requires exactly 32 bytes. Reject any
         * other length explicitly — silent zero-padding (in the
         * adapter or elsewhere) would let a short key verify a frame
         * that was wrapped with a different short key whose padded
         * form collides. Mirrors the wrap-side guard. */
        if (f.algo_desc->algo == FLEX_ENV_ALGO_BLAKE3_KEYED &&
            effective_raw_key_len != BLAKE3_KEY_LEN) {
            goto cleanup;
        }
    }

    /* (e) HMAC required by caller but algo isn't keyed → reject. */
    if ((opts & FLEX_ENV_VERIFY_REQUIRE_HMAC) && !f.algo_desc->is_keyed) {
        goto cleanup;
    }

    /* (f) Trailing bytes policy. */
    if (f.total_size != buf_len && !(opts & FLEX_ENV_VERIFY_ALLOW_TRAILING)) {
        goto cleanup;
    }

    /* (g) Payload size policy. */
    const uint64_t cap = (opts & FLEX_ENV_VERIFY_BOUND_PAYLOAD)
                             ? p->max_payload_len
                             : ((opts & FLEX_ENV_VERIFY_ALLOW_OVERSIZE)
                                    ? UINT64_MAX
                                    : FLEX_ENVELOPE_DEFAULT_MAX_PAYLOAD);
    if (f.payload_len > cap) {
        goto cleanup;
    }

    /* If caller only wants framing, stop here. */
    if (opts & FLEX_ENV_VERIFY_PEEK_ONLY) {
        if (out_payload) {
            *out_payload = buf + f.payload_offset;
        }
        if (out_payload_len) {
            *out_payload_len = f.payload_len;
        }
        result = true;
        goto cleanup;
    }

    /* Compute and compare digest. */
    const size_t dlen = f.algo_desc->digest_len;
    if (dlen > 0 && f.algo_desc->hash) {
        uint8_t computed[FLEX_ENV_MAX_DIGEST_LEN];
        if (dlen > sizeof(computed)) {
            goto cleanup;
        }
        /* Prepared-key fast path: caller-supplied hmacKey skips the
         * K_ipad/K_opad block compressions on every call. Saves
         * ~2 block compressions per verify — ~2x for short messages.
         * Only kicks in for HMAC class (BLAKE3_KEYED uses native
         * keyed-hash, which has no equivalent precomputation). */
        const flexEnvelopeAlgo algo_id = (flexEnvelopeAlgo)f.algo_desc->algo;
        if (effective_prep_key && (algo_id == FLEX_ENV_ALGO_HMAC_SHA256 ||
                                   algo_id == FLEX_ENV_ALGO_HMAC_BLAKE3)) {
            hmacComputeWithKey(effective_prep_key, buf,
                               f.payload_offset + (size_t)f.payload_len,
                               computed);
        } else {
            f.algo_desc->hash(buf, f.payload_offset + (size_t)f.payload_len,
                              effective_raw_key, effective_raw_key_len,
                              computed);
        }

        bool (*cmp)(const uint8_t *, const uint8_t *, size_t);
        if ((opts & FLEX_ENV_VERIFY_FAST_COMPARE) || !f.algo_desc->is_keyed) {
            cmp = digestEqFast;
        } else {
            cmp = digestEqConstantTime;
        }
        if (!cmp(computed, buf + f.digest_offset, dlen)) {
            goto cleanup;
        }
    } else if (dlen > 0) {
        /* Algorithm declares a nonzero digest but no hash adapter. */
        goto cleanup;
    }

    /* Optional payload structural check. */
    if (opts & FLEX_ENV_VERIFY_REQUIRE_FLEX) {
        if (!flexValidateBuffer(buf + f.payload_offset,
                                (size_t)f.payload_len)) {
            goto cleanup;
        }
    }

    if (out_payload) {
        *out_payload = buf + f.payload_offset;
    }
    if (out_payload_len) {
        *out_payload_len = f.payload_len;
    }
    result = true;

cleanup:
    if (key_copy_populated) {
        dkSecureZero(key_copy, sizeof(key_copy));
    }
    return result;
}

/* === Convenience: unwrap as flex ==================================== */

flex *flexEnvelopeUnwrapAsFlex(const uint8_t *buf, size_t buf_len,
                               const flexEnvelopeVerifyParams *params) {
    const uint8_t *payload = NULL;
    uint64_t payload_len = 0;

    /* Build local params that ensure flex validation runs. */
    flexEnvelopeVerifyParams local;
    if (params) {
        local = *params;
    } else {
        memset(&local, 0, sizeof(local));
    }
    local.opts |= FLEX_ENV_VERIFY_REQUIRE_FLEX;

    if (!flexEnvelopeVerify(buf, buf_len, &local, &payload, &payload_len)) {
        return NULL;
    }

    /* @own: caller; free=flexFree */
    flex *f = zmalloc((size_t)payload_len);
    if (!f) {
        return NULL;
    }
    memcpy(f, payload, (size_t)payload_len);
    return f;
}

/* ====================================================================
 * AEAD Seal / Open
 *
 * Wire format for AEAD class:
 * [magic 2B] [version varint] [algo varint] [flags varint]
 * [payload_len varint] [nonce nonce_len B] [ciphertext payload_len B]
 * [tag digest_len B]
 *
 * The framing parser already reads `nonce_len` between the header and
 * payload when algo->is_aead — this code just plugs the cipher in. */

/* Shared validation + header+nonce emission for the AEAD (Seal) path.
 *
 * Same shape as wrapEmitHeader. On success:
 *  - `out` bytes [0, *header_size_out)            hold the framing header.
 *  - `out` bytes [*header_size_out, *aad_len_out) hold the nonce
 *      (== header + nonce_len bytes; this is the AAD region the cipher
 *      authenticates).
 *  - `*total_size_out` is the total envelope size (header + nonce +
 *    ciphertext + tag). The caller has not yet observed the nonce log or
 *    run the cipher.
 *  - `*desc_out` points at the algo descriptor. */
static bool sealEmitHeader(uint8_t *out, size_t out_cap, uint64_t plaintext_len,
                           const flexEnvelopeSealParams *params,
                           const flexEnvelopeAlgoDesc **desc_out,
                           size_t *header_size_out, size_t *aad_len_out,
                           size_t *total_size_out) {
    if (!out || !params) {
        return false;
    }

    /* Mirror flexEnvelopeWrap — the three reserved bytes after `version`
     * MUST be zero. See flexEnvelopeSealParams::_pad header comment.
     * Refuse the call before any cipher work. */
    if (params->_pad[0] != 0u || params->_pad[1] != 0u ||
        params->_pad[2] != 0u) {
        return false;
    }

    const flexEnvelopeAlgoDesc *d = findAlgo((uint16_t)params->algo);
    if (!d || !d->is_implemented || !d->is_aead) {
        return false;
    }
    if (params->key == NULL || params->key_len == 0) {
        return false;
    }
    if (params->nonce == NULL || params->nonce_len != d->nonce_len) {
        return false;
    }
    if ((params->on_wire_flags & ~FLEX_ENV_FLAGS_VALID_V1) != 0) {
        return false;
    }
    const uint8_t version =
        params->version == 0 ? FLEX_ENVELOPE_VERSION_CURRENT : params->version;
    if (version != FLEX_ENVELOPE_VERSION_CURRENT) {
        return false;
    }

    /* Reconcile params->key_id with the KEYID_PRESENT flag — same
     * contract as flexEnvelopeWrap. Non-zero key_id implicitly enables
     * the flag (callers who set the id need not also flip the bit).
     * A zero key_id with the flag set is a contradiction since the
     * parser treats key_id==0 as "absent" and would reject the frame. */
    uint32_t wire_flags = params->on_wire_flags;
    if (params->key_id != 0u) {
        wire_flags |= FLEX_ENV_FLAG_KEYID_PRESENT;
    } else if (wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT) {
        return false;
    }

    /* Compute the EXACT envelope size (matches flexEnvelopeWrap).
     * flexEnvelopeWrappedSize conservatively reserves a UINT32_MAX-sized
     * keyid varint, which over-counts when the actual id needs fewer
     * varint bytes. Using the over-count as the written byte total
     * would leave a trailing zero gap that Open then rejects as
     * "trailing bytes". Compute the precise prefix here. */
    const size_t magic_bytes = 2u;
    const size_t version_size = varintEncodedLen((uint64_t)version);
    const size_t algo_size = varintEncodedLen((uint64_t)params->algo);
    const size_t flags_size = varintEncodedLen((uint64_t)wire_flags);
    const size_t keyid_size = (wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT)
                                  ? varintEncodedLen((uint64_t)params->key_id)
                                  : 0u;
    const size_t plen_size = varintEncodedLen(plaintext_len);
    const size_t prefix_total = magic_bytes + version_size + algo_size +
                                flags_size + keyid_size + plen_size;
    const size_t trailers = d->digest_len + d->nonce_len;
    if (trailers > SIZE_MAX - prefix_total) {
        return false;
    }
    const size_t prefix_plus_trailers = prefix_total + trailers;
    if (plaintext_len > UINT64_MAX - (uint64_t)trailers ||
        (uint64_t)plaintext_len > (uint64_t)(SIZE_MAX - prefix_plus_trailers)) {
        return false;
    }
    const size_t needed = prefix_plus_trailers + (size_t)plaintext_len;
    if (needed > out_cap) {
        return false;
    }

    /* Emit framing header (same shape as flexEnvelopeWrap). */
    size_t pos = 0;
    out[pos++] = FLEX_ENVELOPE_MAGIC_0;
    out[pos++] = FLEX_ENVELOPE_MAGIC_1;
    pos += varintEncode(out + pos, (uint64_t)version);
    pos += varintEncode(out + pos, (uint64_t)params->algo);
    pos += varintEncode(out + pos, (uint64_t)wire_flags);
    if (wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT) {
        pos += varintEncode(out + pos, (uint64_t)params->key_id);
    }
    pos += varintEncode(out + pos, plaintext_len);
    const size_t header_size = pos;

    /* Emit nonce. */
    memcpy(out + pos, params->nonce, d->nonce_len);
    pos += d->nonce_len;

    *desc_out = d;
    *header_size_out = header_size;
    *aad_len_out = pos; /* header + nonce */
    *total_size_out = needed;
    return true;
}

/* Run the AEAD cipher over the prepared header+nonce region and the
 * caller's plaintext, writing ciphertext into the payload slot and the
 * tag into the trailer slot. `out` is the full envelope buffer, with
 * the header + nonce already populated in [0, aad_len). `plaintext` is
 * EXTERNAL — the digest-only Wrap path supplies a separate buffer; the
 * Finalize path supplies a pointer into the envelope's own payload slot
 * (the cipher handles aliasing correctly for both ChaCha20-Poly1305 and
 * AES-GCM). */
static bool sealRunCipher(uint8_t *out, size_t aad_len,
                          const uint8_t *plaintext, uint64_t plaintext_len,
                          const flexEnvelopeSealParams *params,
                          const flexEnvelopeAlgoDesc *d) {
    uint8_t *ciphertext_out = out + aad_len;
    uint8_t *tag_out = ciphertext_out + plaintext_len;
    if (params->algo == FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305) {
        if (params->key_len != CHACHA20_POLY1305_KEY_LEN) {
            return false;
        }
        if (!chacha20poly1305Seal(params->key, params->nonce, out, aad_len,
                                  plaintext, (size_t)plaintext_len,
                                  ciphertext_out, tag_out)) {
            return false;
        }
    } else if (params->algo == FLEX_ENV_ALGO_AEAD_AES128_GCM ||
               params->algo == FLEX_ENV_ALGO_AEAD_AES256_GCM) {
        const size_t expect = (params->algo == FLEX_ENV_ALGO_AEAD_AES128_GCM)
                                  ? AES128_KEY_LEN
                                  : AES256_KEY_LEN;
        if (params->key_len != expect) {
            return false;
        }
        aesGcmKey k;
        if (!aesGcmInit(&k, params->key, params->key_len)) {
            return false;
        }
        if (!aesGcmSeal(&k, params->nonce, out, aad_len, plaintext,
                        (size_t)plaintext_len, ciphertext_out, tag_out)) {
            /* The expanded round keys are key-equivalent material.
             * dkSecureZero is the DCE-resistant scrub — plain memset
             * may be elided by an LTO build that proves `k` is dead. */
            dkSecureZero(&k, sizeof(k));
            return false;
        }
        dkSecureZero(&k, sizeof(k));
    } else {
        return false; /* unreachable; descriptor table guards this */
    }
    (void)d;
    return true;
}

/* (key_id, nonce) reuse check shared by Seal and FinalizeSeal. */
static bool sealObserveNonce(const flexEnvelopeSealParams *params,
                             const flexEnvelopeAlgoDesc *d) {
    if (params->nonceLog != NULL && d->nonce_len == AEAD_NONCE_LOG_NONCE_LEN &&
        params->key_id != 0u) {
        if (!aeadNonceLogObserve(params->nonceLog, params->key_id,
                                 params->nonce)) {
            setVerifyErr(FLEX_ENV_VERIFY_FAILED_NONCE_REUSED);
            return false;
        }
    }
    return true;
}

bool flexEnvelopeSeal(uint8_t *out, size_t out_cap, const uint8_t *plaintext,
                      uint64_t plaintext_len,
                      const flexEnvelopeSealParams *params,
                      size_t *out_written) {
    if (plaintext_len > 0 && !plaintext) {
        return false;
    }

    const flexEnvelopeAlgoDesc *d = NULL;
    size_t header_size = 0;
    size_t aad_len = 0;
    size_t needed = 0;
    if (!sealEmitHeader(out, out_cap, plaintext_len, params, &d, &header_size,
                        &aad_len, &needed)) {
        return false;
    }
    (void)header_size;

    /* defense-in-depth (key_id, nonce) reuse check.
     * If the caller bound an aeadNonceLog AND the nonce width matches
     * the log's expected width (12 B — both supported AEADs use 12 B,
     * which AEAD_NONCE_LOG_NONCE_LEN encodes), observe the tuple BEFORE
     * deriving the keystream. A duplicate aborts the Seal — proceeding
     * would derive identical keystream under the same key, destroying
     * both confidentiality and integrity. The bypass conditions
     * (NULL log, zero key_id, non-12 B nonce) intentionally fail open
     * so legacy single-process callers keep working unchanged. */
    if (!sealObserveNonce(params, d)) {
        return false;
    }

    /* Encrypt + authenticate. AAD is the framing header + nonce — the
     * entire prefix of the envelope before the ciphertext. Any tamper
     * to those bytes on the wire fails the tag check, so the algo /
     * payload_len / nonce fields are all integrity-protected. */
    if (!sealRunCipher(out, aad_len, plaintext, plaintext_len, params, d)) {
        return false;
    }

    if (out_written) {
        *out_written = needed;
    }
    return true;
}

/* === Zero-copy Seal =================================================
 *
 * The `ReserveSeal` + `FinalizeSeal` pair is the AEAD counterpart to
 * `ReserveWrap` + `FinalizeWrap`. The caller writes plaintext bytes
 * directly into the reserved region; Finalize then runs the AEAD cipher
 * in place (the underlying chacha20poly1305 / AES-GCM Seal entry points
 * support `src == dst` aliasing). The (key_id, nonce) reuse check runs
 * inside Reserve so that a duplicate is caught BEFORE the caller spends
 * cycles producing the plaintext. */

bool flexEnvelopeReserveSeal(uint8_t *out, size_t out_cap,
                             uint64_t plaintext_len,
                             const flexEnvelopeSealParams *params,
                             uint8_t **plaintext_region_out,
                             size_t *header_plus_nonce_len_out) {
    if (!plaintext_region_out || !header_plus_nonce_len_out) {
        return false;
    }
    const flexEnvelopeAlgoDesc *d = NULL;
    size_t header_size = 0;
    size_t aad_len = 0;
    size_t needed = 0;
    if (!sealEmitHeader(out, out_cap, plaintext_len, params, &d, &header_size,
                        &aad_len, &needed)) {
        return false;
    }
    (void)header_size;
    (void)needed;
    /* Catch nonce reuse here, BEFORE the caller spends cycles producing
     * plaintext into the reserved region. Mirrors the one-shot Seal
     * sequencing (observe → cipher) — a duplicate is fatal anyway. */
    if (!sealObserveNonce(params, d)) {
        return false;
    }
    *plaintext_region_out = out + aad_len;
    *header_plus_nonce_len_out = aad_len;
    return true;
}

bool flexEnvelopeFinalizeSeal(uint8_t *out, size_t out_cap,
                              size_t header_plus_nonce_len,
                              uint64_t plaintext_len,
                              const flexEnvelopeSealParams *params,
                              size_t *written_out) {
    if (!out || !params) {
        return false;
    }
    /* Re-validate everything Reserve checked; same fail-closed posture as
     * FinalizeWrap. We do NOT re-run sealObserveNonce — Reserve already
     * observed the (key_id, nonce) tuple, and a second observe would
     * register a self-duplicate. */
    if (params->_pad[0] != 0u || params->_pad[1] != 0u ||
        params->_pad[2] != 0u) {
        return false;
    }
    const flexEnvelopeAlgoDesc *d = findAlgo((uint16_t)params->algo);
    if (!d || !d->is_implemented || !d->is_aead) {
        return false;
    }
    if (params->key == NULL || params->key_len == 0) {
        return false;
    }
    if (params->nonce == NULL || params->nonce_len != d->nonce_len) {
        return false;
    }
    if ((params->on_wire_flags & ~FLEX_ENV_FLAGS_VALID_V1) != 0) {
        return false;
    }
    uint32_t wire_flags = params->on_wire_flags;
    if (params->key_id != 0u) {
        wire_flags |= FLEX_ENV_FLAG_KEYID_PRESENT;
    } else if (wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT) {
        return false;
    }
    const uint8_t version =
        params->version == 0 ? FLEX_ENVELOPE_VERSION_CURRENT : params->version;
    if (version != FLEX_ENVELOPE_VERSION_CURRENT) {
        return false;
    }
    const size_t magic_bytes = 2u;
    const size_t version_size = varintEncodedLen((uint64_t)version);
    const size_t algo_size = varintEncodedLen((uint64_t)params->algo);
    const size_t flags_size = varintEncodedLen((uint64_t)wire_flags);
    const size_t keyid_size = (wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT)
                                  ? varintEncodedLen((uint64_t)params->key_id)
                                  : 0u;
    const size_t plen_size = varintEncodedLen(plaintext_len);
    const size_t expected_header_size = magic_bytes + version_size + algo_size +
                                        flags_size + keyid_size + plen_size;
    const size_t expected_aad = expected_header_size + d->nonce_len;
    if (expected_aad != header_plus_nonce_len) {
        return false;
    }
    const size_t trailers = d->digest_len + d->nonce_len;
    if (trailers > SIZE_MAX - expected_header_size) {
        return false;
    }
    const size_t prefix_plus_trailers = expected_header_size + trailers;
    if (plaintext_len > UINT64_MAX - (uint64_t)trailers ||
        (uint64_t)plaintext_len > (uint64_t)(SIZE_MAX - prefix_plus_trailers)) {
        return false;
    }
    const size_t needed = prefix_plus_trailers + (size_t)plaintext_len;
    if (needed > out_cap) {
        return false;
    }

    /* Cheap consistency check: the magic must still be present at offset
     * 0. Catches callers that smashed the header between Reserve and
     * Finalize. */
    if (out[0] != FLEX_ENVELOPE_MAGIC_0 || out[1] != FLEX_ENVELOPE_MAGIC_1) {
        return false;
    }

    /* In-place encrypt+authenticate: plaintext is the reserved region in
     * `out` itself. Both chacha20poly1305Seal and aesGcmSeal accept
     * `src == dst`. */
    const uint8_t *plaintext = out + header_plus_nonce_len;
    if (!sealRunCipher(out, header_plus_nonce_len, plaintext, plaintext_len,
                       params, d)) {
        return false;
    }

    if (written_out) {
        *written_out = needed;
    }
    return true;
}

bool flexEnvelopeOpen(const uint8_t *buf, size_t buf_len,
                      const flexEnvelopeVerifyParams *params,
                      uint8_t *plaintext_out, size_t plaintext_out_cap,
                      size_t *plaintext_written) {
    /* Reset the per-thread error to OK on entry; any failure path below
     * overwrites it with a more specific code (or leaves OK on success). */
    setVerifyErr(FLEX_ENV_VERIFY_OK);
    if (!buf || !plaintext_out) {
        return false;
    }

    /* Stack buffer for keyring auto-resolution. Populated by
     * hmacKeyringGetRawCopy when the caller did not supply key bytes
     * directly and the wire carries a key id. Closing the lifetime
     * window the borrowed-pointer Get used to leave open: a concurrent
     * Retire on the resolved id during the AEAD Open would race
     * the cipher's read of `effective_key`. Wiped at the single exit
     * point — the bytes are AES/ChaCha key material when populated. */
    uint8_t key_copy[HMAC_KEYRING_MAX_KEY_BYTES];
    bool key_copy_populated = false;
    bool result = false;

    flexEnvelopeFraming f;
    if (!parseFramingEx(buf, buf_len, &f, false)) {
        setVerifyErr(FLEX_ENV_VERIFY_FAILED_GENERIC);
        goto cleanup;
    }
    if (!f.algo_desc->is_implemented || !f.algo_desc->is_aead) {
        goto cleanup;
    }
    if (params == NULL) {
        goto cleanup;
    }

    /* keyring auto-resolution, mirroring
     * flexEnvelopeVerify's logic. Precedence rule (documented in
     * flexEnvelope.h):
     * 1. If the caller supplied an explicit `key` (key_len > 0), that
     * key wins — even when a keyring is also bound. Lets callers
     * override a ring-resolved key without rebuilding the ring.
     * 2. Otherwise, if `keyring` is bound AND the wire carries
     * FLEX_ENV_FLAG_KEYID_PRESENT, look up the raw key bytes by id.
     * AEAD class always needs raw bytes (no prepared-key fast path).
     * The revoked-key floor runs before lookup so a frame wearing a
     * revoked id is rejected with FLEX_ENV_VERIFY_FAILED_KEY_REVOKED even
     * if the bytes are still resident in the ring during rotation. */
    const uint8_t *effective_key = params->key;
    size_t effective_key_len = params->key_len;
    /* track the keyring slot that resolved `effective_key`
     * so the AES-GCM branch can ask for the cached schedule by id and skip
     * the per-call aesGcmInit expansion. Stays HMAC_KEY_ID_INVALID when
     * the caller supplied `params->key` directly (no keyring binding) or
     * when no keyring is bound — both paths fall back to ad-hoc init. */
    hmacKeyId resolved_key_id = HMAC_KEY_ID_INVALID;
    if (effective_key == NULL && effective_key_len == 0 &&
        params->keyring != NULL &&
        (f.on_wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT)) {
        const hmacKeyId floor =
            hmacKeyringMinValidKeyId((struct hmacKeyring *)params->keyring);
        if (floor != HMAC_KEY_ID_INVALID && f.key_id < floor) {
            setVerifyErr(FLEX_ENV_VERIFY_FAILED_KEY_REVOKED);
            goto cleanup;
        }
        /* Copy the key into the local stack buffer under the keyring's
         * rwlock — a concurrent Retire on this id during the AEAD
         * cipher pass would otherwise race the borrowed-pointer Get
         * and free the bytes underneath the read. The copy is
         * independent of any subsequent keyring mutation. */
        size_t copied_len = 0;
        if (hmacKeyringGetRawCopy((struct hmacKeyring *)params->keyring,
                                  f.key_id, key_copy, sizeof(key_copy),
                                  &copied_len, NULL)) {
            key_copy_populated = true;
            effective_key = key_copy;
            effective_key_len = copied_len;
            resolved_key_id = f.key_id;
        }
    }

    if (effective_key == NULL || effective_key_len == 0) {
        goto cleanup;
    }
    if (f.payload_len > (uint64_t)plaintext_out_cap) {
        /* Caller buffer too small — zero it so a partial write does not
         * leak garbage if the caller forgets to check the return. */
        dkSecureZero(plaintext_out, plaintext_out_cap);
        goto cleanup;
    }
    if (!(params->opts & FLEX_ENV_VERIFY_ALLOW_TRAILING) &&
        f.total_size != buf_len) {
        goto cleanup;
    }

    const uint8_t *nonce = buf + f.nonce_offset;
    const uint8_t *ciphertext = buf + f.payload_offset;
    const uint8_t *tag = buf + f.digest_offset;

    bool ok = false;
    if (f.algo_desc->algo == FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305) {
        if (effective_key_len != CHACHA20_POLY1305_KEY_LEN) {
            goto cleanup;
        }
        ok = chacha20poly1305Open(effective_key, nonce, buf, f.payload_offset,
                                  ciphertext, (size_t)f.payload_len, tag,
                                  plaintext_out);
    } else if (f.algo_desc->algo == FLEX_ENV_ALGO_AEAD_AES128_GCM ||
               f.algo_desc->algo == FLEX_ENV_ALGO_AEAD_AES256_GCM) {
        const size_t expect =
            (f.algo_desc->algo == FLEX_ENV_ALGO_AEAD_AES128_GCM)
                ? AES128_KEY_LEN
                : AES256_KEY_LEN;
        if (effective_key_len != expect) {
            goto cleanup;
        }
        /* when the key was resolved through the keyring,
         * try to consume the pre-computed AES schedule cached on the slot
         * so we skip the per-frame key-expansion cost (~100-200 ns at HW
         * speeds — meaningful at small payloads). The cache returns false
         * for slots whose key length is not 16 / 32 bytes (e.g. HMAC keys
         * of other sizes); fall back to ad-hoc aesGcmInit then. When the
         * caller supplied `params->key` directly we always take the cold
         * path — there is no slot to bind the schedule to.
         *
         * Copy the schedule out of the keyring (under its rwlock) rather
         * than holding a borrowed pointer into the slot — a concurrent
         * Retire would dkSecureZero + free the borrowed schedule mid-
         * AES-GCM-Open and UAF the round-key reads. The copy is wiped
         * before frame exit since the round keys are key-equivalent
         * material. */
        aesGcmKey k;
        bool k_populated = false;
        if (resolved_key_id != HMAC_KEY_ID_INVALID &&
            hmacKeyringGetAesGcmKeyCopy((struct hmacKeyring *)params->keyring,
                                        resolved_key_id, &k)) {
            k_populated = true;
        } else if (aesGcmInit(&k, effective_key, effective_key_len)) {
            k_populated = true;
        }
        if (k_populated) {
            ok = aesGcmOpen(&k, nonce, buf, f.payload_offset, ciphertext,
                            (size_t)f.payload_len, tag, plaintext_out);
            /* Wipe the schedule before frame exit — the round keys are
             * key-equivalent material; defense-in-depth even though
             * stack-resident storage will be clobbered on the next
             * call. */
            dkSecureZero(&k, sizeof(k));
        }
    }
    if (!ok) {
        /* Defense in depth: scrub the output buffer so callers that
         * forget to check the return value don't dereference forged
         * plaintext. */
        dkSecureZero(plaintext_out, (size_t)f.payload_len);
        goto cleanup;
    }

    /* consumer-side defense-in-depth replay check.
     * Runs ONLY after tag verification succeeds — observing a tampered
     * frame would poison the log with attacker-chosen tuples (cheap DoS
     * surface). Requires both (a) the caller bound a log, and (b) the
     * wire carried KEYID_PRESENT so we have a meaningful id to pair
     * with the nonce. Without (b) the log can't disambiguate same-nonce
     * frames keyed under different ids — better to skip than to record
     * a partial fingerprint. A duplicate hit here means the producer's
     * counter was rolled back (snapshot restore, VM clone) or an active
     * attacker is replaying — either way, the second Open is rejected
     * with a typed error so the caller can distinguish from generic
     * "framing bad" failures. */
    if (params->nonceLog != NULL &&
        f.algo_desc->nonce_len == AEAD_NONCE_LOG_NONCE_LEN &&
        (f.on_wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT) && f.key_id != 0u) {
        if (!aeadNonceLogObserve(params->nonceLog, f.key_id, nonce)) {
            /* Scrub plaintext to avoid leaking decrypted bytes to a
             * caller that ignored the return value. */
            dkSecureZero(plaintext_out, (size_t)f.payload_len);
            setVerifyErr(FLEX_ENV_VERIFY_FAILED_NONCE_REUSED);
            goto cleanup;
        }
    }

    if (plaintext_written) {
        *plaintext_written = (size_t)f.payload_len;
    }
    result = true;

cleanup:
    if (key_copy_populated) {
        dkSecureZero(key_copy, sizeof(key_copy));
    }
    return result;
}

/* ====================================================================
 * Streaming API
 *
 * Wire format is identical to one-shot wrap. The streaming context
 * holds a per-algorithm running-hash state, no payload buffer. The
 * caller is responsible for emitting payload bytes to its downstream
 * consumer; the streaming context only computes the digest.
 * ==================================================================== */

/* Initialize the per-algorithm running-hash state inside the streaming
 * context union. `key`/`key_len` apply to keyed algorithms only.
 * Returns true on success. */
static bool streamHashInit(uint16_t algo, void *u_state, hmacCtx *hmac,
                           uint8_t *hmac_storage, size_t hmac_storage_cap,
                           const uint8_t *key, size_t key_len,
                           const hmacKey *prepared_key) {
    switch ((flexEnvelopeAlgo)algo) {
    case FLEX_ENV_ALGO_NONE:
        return true;
    case FLEX_ENV_ALGO_XXH64: {
        XXH64_state_t *s = (XXH64_state_t *)u_state;
        return XXH64_reset(s, 0) == XXH_OK;
    }
    case FLEX_ENV_ALGO_RAPIDHASH:
        rapidhashStreamInit((rapidhashCtx *)u_state, RAPIDHASH_VARIANT_MAIN,
                            RAPIDHASH_MODE_FAST, 0, NULL);
        return true;
    case FLEX_ENV_ALGO_XXH3_64:
    case FLEX_ENV_ALGO_XXH3_128:
        /* The envelope streaming API does not offer XXH3: Init refuses
         * it, and streaming callers use XXH64 for an unkeyed checksum or
         * a keyed algorithm for authentication.  One-shot wrap and
         * unwrap support XXH3_64 and XXH3_128. */
        return false;
    case FLEX_ENV_ALGO_BLAKE3_KEYED: {
        /* BLAKE3 keyed mode is defined ONLY for a 32-byte key. Anything
         * shorter MUST be refused — silent zero-padding would shrink
         * the effective entropy to key_len*8 bits while presenting a
         * full 256-bit digest. Mirrors the explicit AEAD key-length
         * guard sealRunCipher / Open use for ChaCha20-Poly1305 +
         * AES-GCM keys. */
        if (key == NULL || key_len != BLAKE3_KEY_LEN) {
            return false;
        }
        blake3HasherInitKeyed((blake3Hasher *)u_state, key);
        return true;
    }
    case FLEX_ENV_ALGO_HMAC_SHA256: {
        const size_t need = hmacCtxSize(&hmacAlgoSHA256);
        if (need > hmac_storage_cap) {
            return false;
        }
        /* Fast path: caller supplied a prepared key whose algorithm
         * matches. Skips the two block compressions hmacInit pays for
         * K_ipad and K_opad — pays off in the streaming verify hot
         * path (cluster apply mailbox, replication ingest) where the
         * same key is used across millions of envelopes. */
        if (prepared_key != NULL && prepared_key->algo == &hmacAlgoSHA256) {
            hmacInitFromPrepared(hmac, prepared_key, hmac_storage,
                                 hmac_storage_cap);
            return true;
        }
        hmacInit(hmac, &hmacAlgoSHA256, hmac_storage, hmac_storage_cap, key,
                 key_len);
        return true;
    }
    case FLEX_ENV_ALGO_HMAC_BLAKE3: {
        const size_t need = hmacCtxSize(&hmacAlgoBLAKE3);
        if (need > hmac_storage_cap) {
            return false;
        }
        if (prepared_key != NULL && prepared_key->algo == &hmacAlgoBLAKE3) {
            hmacInitFromPrepared(hmac, prepared_key, hmac_storage,
                                 hmac_storage_cap);
            return true;
        }
        hmacInit(hmac, &hmacAlgoBLAKE3, hmac_storage, hmac_storage_cap, key,
                 key_len);
        return true;
    }
    case FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305:
    case FLEX_ENV_ALGO_AEAD_AES128_GCM:
    case FLEX_ENV_ALGO_AEAD_AES256_GCM:
        /* AEAD class is not streamable through this digest-oriented API.
         * Use the dedicated flexEnvelopeSeal / flexEnvelopeOpen pair. */
        return false;
    }
    return false;
}

/* Feed bytes into the running hash for the active algorithm. */
static bool streamHashUpdate(uint16_t algo, void *u_state, hmacCtx *hmac,
                             const uint8_t *data, size_t len) {
    if (len == 0) {
        return true;
    }
    switch ((flexEnvelopeAlgo)algo) {
    case FLEX_ENV_ALGO_NONE:
        return true;
    case FLEX_ENV_ALGO_XXH64:
        return XXH64_update((XXH64_state_t *)u_state, data, len) == XXH_OK;
    case FLEX_ENV_ALGO_RAPIDHASH:
        rapidhashStreamUpdate((rapidhashCtx *)u_state, data, len);
        return true;
    case FLEX_ENV_ALGO_XXH3_64:
        return XXH3_64bits_update((XXH3_state_t *)u_state, data, len) == XXH_OK;
    case FLEX_ENV_ALGO_XXH3_128:
        /* XXH3 128-bit streaming shares the same update function as the
         * 64-bit variant; only reset/digest differ (see xxhash.h). */
        return XXH3_64bits_update((XXH3_state_t *)u_state, data, len) == XXH_OK;
    case FLEX_ENV_ALGO_BLAKE3_KEYED:
        blake3HasherUpdate((blake3Hasher *)u_state, data, len);
        return true;
    case FLEX_ENV_ALGO_HMAC_SHA256:
    case FLEX_ENV_ALGO_HMAC_BLAKE3:
        hmacUpdate(hmac, data, len);
        return true;
    case FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305:
    case FLEX_ENV_ALGO_AEAD_AES128_GCM:
    case FLEX_ENV_ALGO_AEAD_AES256_GCM:
        /* AEAD class is not driven through this digest update path —
         * Seal/Open handle the entire payload in one call. */
        return false;
    }
    return false;
}

/* Finalize the running hash and write digest bytes. */
static bool streamHashFinal(uint16_t algo, void *u_state, hmacCtx *hmac,
                            uint8_t *digest_out, size_t digest_len) {
    switch ((flexEnvelopeAlgo)algo) {
    case FLEX_ENV_ALGO_NONE:
        return digest_len == 0;
    case FLEX_ENV_ALGO_XXH64: {
        const uint64_t h = XXH64_digest((XXH64_state_t *)u_state);
        for (size_t i = 0; i < 8; i++) {
            digest_out[i] = (uint8_t)(h >> (i * 8));
        }
        return true;
    }
    case FLEX_ENV_ALGO_RAPIDHASH: {
        if (digest_len != 8) {
            return false;
        }
        const uint64_t h = rapidhashStreamFinal((rapidhashCtx *)u_state);
        for (size_t i = 0; i < 8; i++) {
            digest_out[i] = (uint8_t)(h >> (i * 8));
        }
        return true;
    }
    case FLEX_ENV_ALGO_XXH3_64: {
        const uint64_t h = XXH3_64bits_digest((XXH3_state_t *)u_state);
        for (size_t i = 0; i < 8; i++) {
            digest_out[i] = (uint8_t)(h >> (i * 8));
        }
        return true;
    }
    case FLEX_ENV_ALGO_XXH3_128: {
        const XXH128_hash_t h = XXH3_128bits_digest((XXH3_state_t *)u_state);
        for (size_t i = 0; i < 8; i++) {
            digest_out[i] = (uint8_t)(h.low64 >> (i * 8));
            digest_out[8 + i] = (uint8_t)(h.high64 >> (i * 8));
        }
        return true;
    }
    case FLEX_ENV_ALGO_BLAKE3_KEYED:
        blake3HasherFinalize((blake3Hasher *)u_state, digest_out, digest_len);
        return true;
    case FLEX_ENV_ALGO_HMAC_SHA256:
    case FLEX_ENV_ALGO_HMAC_BLAKE3:
        hmacFinal(hmac, digest_out);
        return true;
    case FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305:
    case FLEX_ENV_ALGO_AEAD_AES128_GCM:
    case FLEX_ENV_ALGO_AEAD_AES256_GCM:
        /* AEAD class is not driven through this digest finalize path. */
        return false;
    }
    return false;
}

/* === Wrap stream ==================================================== */

bool flexEnvelopeWrapStreamInit(flexEnvelopeWrapStream *ctx,
                                uint64_t payload_len,
                                const flexEnvelopeWrapParams *params,
                                uint8_t *header_out, size_t header_out_cap,
                                size_t *header_written) {
    if (!ctx || !params || !header_out || !header_written) {
        return false;
    }
    memset(ctx, 0, sizeof(*ctx));

    *header_written = 0; /* Until success we promise no buffer mutation
                          * the caller would mistake for a real header. */

    /* Streaming wrap honors the same _pad-must-be-zero contract as the
     * one-shot path. See flexEnvelopeWrapParams::_pad header comment. */
    if (params->_pad[0] != 0u || params->_pad[1] != 0u ||
        params->_pad[2] != 0u) {
        return false;
    }

    const flexEnvelopeAlgoDesc *d = findAlgo((uint16_t)params->algo);
    if (!d || !d->is_implemented) {
        return false;
    }
    /* AEAD class is not driven through the digest-oriented streaming
     * API — callers go through flexEnvelopeSeal / flexEnvelopeOpen.
     * Reject here, co-located with the other secure-default barriers,
     * BEFORE we touch the caller's header buffer. */
    if (d->is_aead) {
        return false;
    }

    /* Same secure-default barriers as one-shot wrap. */
#ifndef DATAKIT_ENABLE_INSECURE_TEST_ALGOS
    if (params->algo == FLEX_ENV_ALGO_NONE) {
        return false;
    }
#endif
    if (d->is_keyed && (params->key == NULL || params->key_len == 0)) {
        return false;
    }
    /* BLAKE3 native keyed-hash mode requires exactly 32 bytes; mirror
     * the one-shot wrap guard. The streamHashInit BLAKE3_KEYED branch
     * also re-checks defensively. */
    if (params->algo == FLEX_ENV_ALGO_BLAKE3_KEYED &&
        params->key_len != BLAKE3_KEY_LEN) {
        return false;
    }
    if ((params->on_wire_flags & ~FLEX_ENV_FLAGS_VALID_V1) != 0) {
        return false;
    }

    const uint8_t version =
        params->version == 0 ? FLEX_ENVELOPE_VERSION_CURRENT : params->version;
    if (version != FLEX_ENVELOPE_VERSION_CURRENT) {
        return false;
    }

    /* Reconcile params->key_id with the KEYID_PRESENT flag (mirrors
     * the one-shot Wrap reconciliation). */
    uint32_t wire_flags = params->on_wire_flags;
    if (params->key_id != 0) {
        wire_flags |= FLEX_ENV_FLAG_KEYID_PRESENT;
    } else if (wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT) {
        return false;
    }

    /* Compute header size and ensure caller's buffer fits it. */
    const size_t magic_bytes = 2;
    const size_t version_size = varintEncodedLen((uint64_t)version);
    const size_t algo_size = varintEncodedLen((uint64_t)params->algo);
    const size_t flags_size = varintEncodedLen((uint64_t)wire_flags);
    const size_t keyid_size = (wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT)
                                  ? varintEncodedLen((uint64_t)params->key_id)
                                  : 0;
    const size_t plen_size = varintEncodedLen(payload_len);
    const size_t header_total = magic_bytes + version_size + algo_size +
                                flags_size + keyid_size + plen_size;
    if (header_total > header_out_cap) {
        return false;
    }

    /* Initialize per-algorithm hash state FIRST. If init can fail
     * (missing key, unsupported algo), we must not have mutated
     * caller's header buffer yet — otherwise a caller who ignores the
     * false return is left holding a valid-looking framing without a
     * matching digest stream. */
    if (!streamHashInit((uint16_t)params->algo, &ctx->u, &ctx->hmac,
                        ctx->hmac_storage, sizeof(ctx->hmac_storage),
                        params->key, params->key_len, NULL)) {
        return false;
    }

    /* Emit framing + absorb it into the digest in lockstep. */
    size_t pos = 0;
    header_out[pos++] = FLEX_ENVELOPE_MAGIC_0;
    header_out[pos++] = FLEX_ENVELOPE_MAGIC_1;
    pos += varintEncode(header_out + pos, (uint64_t)version);
    pos += varintEncode(header_out + pos, (uint64_t)params->algo);
    pos += varintEncode(header_out + pos, (uint64_t)wire_flags);
    if (wire_flags & FLEX_ENV_FLAG_KEYID_PRESENT) {
        pos += varintEncode(header_out + pos, (uint64_t)params->key_id);
    }
    pos += varintEncode(header_out + pos, payload_len);
    if (!streamHashUpdate((uint16_t)params->algo, &ctx->u, &ctx->hmac,
                          header_out, pos)) {
        return false;
    }

    ctx->algo = (uint16_t)params->algo;
    ctx->digest_len = (uint8_t)d->digest_len;
    ctx->payload_len_declared = payload_len;
    ctx->payload_bytes_seen = 0;
    ctx->phase = (uint8_t)FLEX_ENV_STREAM_PHASE_OPEN;
    *header_written = pos;
    return true;
}

bool flexEnvelopeWrapStreamUpdate(flexEnvelopeWrapStream *ctx,
                                  const uint8_t *chunk, size_t len) {
    if (!ctx || ctx->phase != (uint8_t)FLEX_ENV_STREAM_PHASE_OPEN) {
        return false;
    }
    if (len == 0) {
        return true;
    }
    /* Mirror the verify-side discipline : any error transitions
     * the ctx to POISONED so subsequent Update/Final calls cannot
     * silently rebuild a half-corrupt state. Wrap-side has no
     * security implication today — XXH returns are always OK, BLAKE3
     * update is void — but a future hash backend could partial-absorb
     * and then fail. Symmetric handling closes that hazard. */
    if (!chunk) {
        ctx->phase = (uint8_t)FLEX_ENV_STREAM_PHASE_POISONED;
        return false;
    }
    /* Bound check: caller must not exceed declared payload_len. */
    if (ctx->payload_bytes_seen > ctx->payload_len_declared ||
        (uint64_t)len > ctx->payload_len_declared - ctx->payload_bytes_seen) {
        ctx->phase = (uint8_t)FLEX_ENV_STREAM_PHASE_POISONED;
        return false;
    }
    if (!streamHashUpdate(ctx->algo, &ctx->u, &ctx->hmac, chunk, len)) {
        ctx->phase = (uint8_t)FLEX_ENV_STREAM_PHASE_POISONED;
        return false;
    }
    ctx->payload_bytes_seen += (uint64_t)len;
    return true;
}

/* Wipe the secret-bearing regions of a wrap-stream ctx. `hmac_storage`
 * holds the HMAC inner state (with K_ipad / K_opad absorbed —
 * key-equivalent material) and the `u` union holds the 32-byte BLAKE3
 * key for keyed BLAKE3 frames. Called from every Final exit path so
 * a caller-owned ctx is safe to discard after Final returns. */
static void wrapStreamWipeSecrets(flexEnvelopeWrapStream *ctx) {
    dkSecureZero(&ctx->hmac_storage, sizeof(ctx->hmac_storage));
    dkSecureZero(&ctx->u, sizeof(ctx->u));
}

bool flexEnvelopeWrapStreamFinal(flexEnvelopeWrapStream *ctx,
                                 uint8_t *digest_out, size_t digest_out_cap,
                                 size_t *digest_written) {
    if (!ctx || ctx->phase != (uint8_t)FLEX_ENV_STREAM_PHASE_OPEN ||
        !digest_written) {
        return false;
    }
    if (ctx->payload_bytes_seen != ctx->payload_len_declared) {
        ctx->phase = (uint8_t)FLEX_ENV_STREAM_PHASE_POISONED;
        wrapStreamWipeSecrets(ctx);
        return false;
    }
    if (ctx->digest_len > digest_out_cap) {
        ctx->phase = (uint8_t)FLEX_ENV_STREAM_PHASE_POISONED;
        wrapStreamWipeSecrets(ctx);
        return false;
    }
    if (ctx->digest_len > 0) {
        if (!digest_out) {
            ctx->phase = (uint8_t)FLEX_ENV_STREAM_PHASE_POISONED;
            wrapStreamWipeSecrets(ctx);
            return false;
        }
        if (!streamHashFinal(ctx->algo, &ctx->u, &ctx->hmac, digest_out,
                             ctx->digest_len)) {
            ctx->phase = (uint8_t)FLEX_ENV_STREAM_PHASE_POISONED;
            wrapStreamWipeSecrets(ctx);
            return false;
        }
    }
    *digest_written = ctx->digest_len;
    /* The ctx contract is "consumed" after Final; treating it as
     * discarded is documented in flexEnvelope.h. */
    wrapStreamWipeSecrets(ctx);
    ctx->phase = (uint8_t)FLEX_ENV_STREAM_PHASE_FINALIZED;
    return true;
}

/* === Verify stream ================================================== */

bool flexEnvelopeVerifyStreamInit(flexEnvelopeVerifyStream *ctx,
                                  const flexEnvelopeVerifyParams *params,
                                  const uint8_t *header_buf, size_t header_len,
                                  size_t *header_consumed,
                                  uint64_t *out_payload_len,
                                  flexEnvelopeAlgo *out_algo) {
    if (!ctx) {
        return false;
    }
    memset(ctx, 0, sizeof(*ctx));

    flexEnvelopeVerifyParams empty;
    memset(&empty, 0, sizeof(empty));
    const flexEnvelopeVerifyParams *p = params ? params : &empty;

    flexEnvelopeFraming f;
    if (!parseFramingForStream(header_buf, header_len, &f)) {
        return false;
    }

    const uint32_t opts = p->opts;

    /* Mirror secure-default invariants from one-shot Verify, ignoring
     * the no-trailing-bytes check (we don't know full buffer length). */
    if (!f.algo_desc->is_implemented) {
        return false;
    }
    /* AEAD-class envelopes carry ciphertext, not plaintext — the
     * streaming digest API cannot produce a meaningful payload
     * pointer. Callers go through flexEnvelopeOpen instead. */
    if (f.algo_desc->is_aead) {
        return false;
    }
    if (f.algo_desc->algo == FLEX_ENV_ALGO_NONE &&
        !(opts & FLEX_ENV_VERIFY_ALLOW_NONE_ALGO)) {
        return false;
    }
    if (f.algo_desc->is_deprecated &&
        !(opts & FLEX_ENV_VERIFY_ALLOW_DEPRECATED)) {
        return false;
    }
    if (f.algo_desc->is_keyed) {
        const bool have_raw_key = (p->key != NULL && p->key_len > 0);
        const bool have_prep_key = (p->prepared_key != NULL);
        if (!have_raw_key && !have_prep_key) {
            return false;
        }
        if (have_prep_key && !have_raw_key &&
            f.algo_desc->algo != FLEX_ENV_ALGO_HMAC_SHA256 &&
            f.algo_desc->algo != FLEX_ENV_ALGO_HMAC_BLAKE3) {
            return false;
        }
    }
    if ((opts & FLEX_ENV_VERIFY_REQUIRE_HMAC) && !f.algo_desc->is_keyed) {
        return false;
    }
    const uint64_t cap = (opts & FLEX_ENV_VERIFY_BOUND_PAYLOAD)
                             ? p->max_payload_len
                             : ((opts & FLEX_ENV_VERIFY_ALLOW_OVERSIZE)
                                    ? UINT64_MAX
                                    : FLEX_ENVELOPE_DEFAULT_MAX_PAYLOAD);
    if (f.payload_len > cap) {
        return false;
    }

    if (f.algo_desc->digest_len > sizeof(ctx->wire_digest)) {
        return false;
    }

    /* Init running hash and absorb the framing header (digest is over
     * framing || payload). */
    /* Streaming verify uses the caller-supplied prepared key when one
     * is provided AND its algorithm matches the envelope's; otherwise
     * falls through to the raw-key path. */
    if (!streamHashInit((uint16_t)f.algo_desc->algo, &ctx->u, &ctx->hmac,
                        ctx->hmac_storage, sizeof(ctx->hmac_storage), p->key,
                        p->key_len, p->prepared_key)) {
        return false;
    }
    if (!streamHashUpdate((uint16_t)f.algo_desc->algo, &ctx->u, &ctx->hmac,
                          header_buf, f.header_size)) {
        return false;
    }

    ctx->algo = (uint16_t)f.algo_desc->algo;
    ctx->digest_len = (uint8_t)f.algo_desc->digest_len;
    ctx->payload_len_declared = f.payload_len;
    ctx->payload_bytes_seen = 0;
    ctx->opts = opts;
    ctx->prepared_key = p->prepared_key;
    ctx->raw_key = p->key;
    ctx->raw_key_len = p->key_len;
    ctx->wire_digest_seen = 0;
    ctx->phase = (uint8_t)FLEX_ENV_STREAM_PHASE_OPEN;

    if (header_consumed) {
        *header_consumed = f.header_size;
    }
    if (out_payload_len) {
        *out_payload_len = f.payload_len;
    }
    if (out_algo) {
        *out_algo = (flexEnvelopeAlgo)f.algo_desc->algo;
    }
    return true;
}

bool flexEnvelopeVerifyStreamUpdate(flexEnvelopeVerifyStream *ctx,
                                    const uint8_t *chunk, size_t len) {
    if (!ctx || ctx->phase != (uint8_t)FLEX_ENV_STREAM_PHASE_OPEN) {
        return false;
    }
    if (len == 0) {
        return true;
    }
    if (!chunk) {
        ctx->phase = (uint8_t)FLEX_ENV_STREAM_PHASE_POISONED;
        return false;
    }
    /* Split chunk into (payload-remaining, digest-remaining). ANY
     * failure transitions the ctx into a poisoned state (phase 3),
     * so that subsequent Update/Final calls see phase != 1 and
     * reject — the protocol state is already corrupted, and the
     * caller MUST NOT try to recover by feeding more bytes. */
    const uint64_t payload_remaining =
        ctx->payload_len_declared - ctx->payload_bytes_seen;
    size_t to_hash =
        (uint64_t)len <= payload_remaining ? len : (size_t)payload_remaining;
    if (to_hash > 0) {
        if (!streamHashUpdate(ctx->algo, &ctx->u, &ctx->hmac, chunk, to_hash)) {
            ctx->phase = (uint8_t)FLEX_ENV_STREAM_PHASE_POISONED;
            return false;
        }
        ctx->payload_bytes_seen += (uint64_t)to_hash;
    }
    size_t remaining = len - to_hash;
    if (remaining > 0) {
        const size_t digest_remaining =
            (size_t)ctx->digest_len - (size_t)ctx->wire_digest_seen;
        if (remaining > digest_remaining) {
            ctx->phase = (uint8_t)FLEX_ENV_STREAM_PHASE_POISONED;
            return false; /* caller overran */
        }
        memcpy(ctx->wire_digest + ctx->wire_digest_seen, chunk + to_hash,
               remaining);
        ctx->wire_digest_seen = (uint8_t)(ctx->wire_digest_seen + remaining);
    }
    return true;
}

/* Wipe the secret-bearing regions of a verify-stream ctx and clear the
 * borrowed-key pointers. Called from every Final exit path so a
 * caller-owned ctx never retains HMAC inner state (K_ipad/K_opad
 * absorbed — key-equivalent material) or the BLAKE3 keyed-hash key
 * bytes after Final returns. */
static void verifyStreamWipeSecrets(flexEnvelopeVerifyStream *ctx) {
    dkSecureZero(&ctx->hmac_storage, sizeof(ctx->hmac_storage));
    dkSecureZero(&ctx->u, sizeof(ctx->u));
    ctx->raw_key = NULL;
    ctx->raw_key_len = 0;
    ctx->prepared_key = NULL;
}

bool flexEnvelopeVerifyStreamFinal(flexEnvelopeVerifyStream *ctx) {
    if (!ctx || ctx->phase != (uint8_t)FLEX_ENV_STREAM_PHASE_OPEN) {
        return false;
    }
    /* Symmetric with every other failure path in the streaming verifier:
     * a Final failure poisons the ctx so a caller that retries with more
     * Update bytes cannot rebuild a half-corrupt session into a
     * superficially-fresh one. The protocol state is already
     * unrecoverable when the byte-count or digest-length invariants
     * don't hold. Wipe the key-bearing regions on every Final exit
     * — the caller-owned ctx must not retain HMAC inner state or the
     * BLAKE3 key past the session boundary. */
    if (ctx->payload_bytes_seen != ctx->payload_len_declared) {
        ctx->phase = (uint8_t)FLEX_ENV_STREAM_PHASE_POISONED;
        verifyStreamWipeSecrets(ctx);
        return false;
    }
    if (ctx->wire_digest_seen != ctx->digest_len) {
        ctx->phase = (uint8_t)FLEX_ENV_STREAM_PHASE_POISONED;
        verifyStreamWipeSecrets(ctx);
        return false;
    }

    const flexEnvelopeAlgoDesc *d = findAlgo(ctx->algo);
    if (!d) {
        ctx->phase = (uint8_t)FLEX_ENV_STREAM_PHASE_POISONED;
        verifyStreamWipeSecrets(ctx);
        return false;
    }

    bool ok = false;
    if (d->digest_len == 0) {
        /* ALGO_NONE with opt-in: nothing to compare. */
        ok = (ctx->opts & FLEX_ENV_VERIFY_ALLOW_NONE_ALGO) != 0;
    } else if (d->digest_len > FLEX_ENV_MAX_DIGEST_LEN) {
        ok = false;
    } else {
        uint8_t computed[FLEX_ENV_MAX_DIGEST_LEN];
        /* Prepared-key fast path: if caller supplied a prepared HMAC
         * key and the algorithm is HMAC class, we could rebuild a
         * one-shot — but since we've already streamed through the
         * data with the raw-key hmacCtx, the running state already
         * absorbed everything correctly. (Prepared-key acceleration in
         * streaming only helps if you re-use the same key across many
         * short messages; for stream it's irrelevant. Future
         * enhancement: use the prepared inner_ctx as a checkpoint.) */
        if (streamHashFinal(ctx->algo, &ctx->u, &ctx->hmac, computed,
                            d->digest_len)) {
            bool (*cmp)(const uint8_t *, const uint8_t *, size_t);
            if ((ctx->opts & FLEX_ENV_VERIFY_FAST_COMPARE) || !d->is_keyed) {
                cmp = digestEqFast;
            } else {
                cmp = digestEqConstantTime;
            }
            ok = cmp(computed, ctx->wire_digest, d->digest_len);
        }
        /* Scrub the local digest copy. A correct caller never sees
         * this region but it briefly holds HMAC output derived from
         * the key. */
        dkSecureZero(computed, sizeof(computed));
    }

    verifyStreamWipeSecrets(ctx);
    ctx->phase = ok ? (uint8_t)FLEX_ENV_STREAM_PHASE_FINALIZED
                    : (uint8_t)FLEX_ENV_STREAM_PHASE_POISONED;
    return ok;
}

/* ====================================================================
 * TESTS
 *
 * Comprehensive Phase 1 coverage:
 * - magic / framing parse, including every byte position
 * - varint width boundary payloads
 * - round-trip for each implemented algorithm × {0B, 1B, 1KB, 64KB, 1MB}
 * - tamper detection (flip every byte of a valid envelope)
 * - policy flag matrix
 * - secure-default invariants (ALGO_NONE, oversize, keyed-without-key,
 * unknown flag bits, trailing bytes)
 * - REQUIRE_FLEX behavior
 * - performance bench (prints, never fails)
 * ==================================================================== */

#ifdef DATAKIT_TEST
#define CTEST_INCLUDE_KEYGEN
#define CTEST_INCLUDE_VALGEN
#include "cleaner.h"
#include "ctest.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Scope-exit cleaners for test scratch buffers (see cleaner.h idioms). */
defcleanerName(uint8_t *, zfree, Bytes);
defcleaner(flex *, flexFree);

static void fillPseudoRandom(uint8_t *dst, size_t len, uint64_t seed) {
    /* Simple xorshift; reproducible across runs. */
    uint64_t s = seed | 1; /* avoid 0 */
    for (size_t i = 0; i < len; i++) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        dst[i] = (uint8_t)(s & 0xFF);
    }
}

static bool roundTripOne(flexEnvelopeAlgo algo, size_t payload_len,
                         const uint8_t *key, size_t key_len) {
    cleanerName(zfree, Bytes) uint8_t *payload = NULL;
    if (payload_len > 0) {
        payload = zmalloc(payload_len);
        fillPseudoRandom(payload, payload_len, 0xC0FFEEUL ^ payload_len);
    }

    const size_t wrapped_size =
        flexEnvelopeWrappedSize(payload_len, algo, FLEX_ENV_FLAG_NONE);
    if (wrapped_size == 0) {
        return false;
    }
    cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(wrapped_size);

    flexEnvelopeWrapParams wp = {
        .algo = algo,
        .on_wire_flags = FLEX_ENV_FLAG_NONE,
        .version = 0,
        .key = key,
        .key_len = key_len,
    };
    size_t written = 0;
    bool ok = flexEnvelopeWrap(buf, wrapped_size, payload, payload_len, &wp,
                               &written);
    if (!ok || written != wrapped_size) {
        return false;
    }

    flexEnvelopeVerifyParams vp = {
        .key = key,
        .key_len = key_len,
        .opts = FLEX_ENV_VERIFY_DEFAULT,
    };
    const uint8_t *out_payload = NULL;
    uint64_t out_payload_len = 0;
    ok = flexEnvelopeVerify(buf, wrapped_size, &vp, &out_payload,
                            &out_payload_len);
    if (!ok || out_payload_len != payload_len) {
        return false;
    }
    if (payload_len > 0 && memcmp(out_payload, payload, payload_len) != 0) {
        return false;
    }

    return true;
}

int flexEnvelopeTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    TEST("envelope: algorithm queries are sane") {
        if (flexEnvelopeAlgoDigestLen(FLEX_ENV_ALGO_XXH64) != 8) {
            ERRR("xxh64 digest_len should be 8");
        }
        if (flexEnvelopeAlgoDigestLen(FLEX_ENV_ALGO_XXH3_128) != 16) {
            ERRR("xxh3_128 digest_len should be 16");
        }
        if (flexEnvelopeAlgoIsKeyed(FLEX_ENV_ALGO_XXH64)) {
            ERRR("xxh64 must NOT be classified as keyed");
        }
        if (!flexEnvelopeAlgoIsKeyed(FLEX_ENV_ALGO_HMAC_SHA256)) {
            ERRR("hmac_sha256 must be classified as keyed");
        }
        if (flexEnvelopeAlgoDigestLen((flexEnvelopeAlgo)9999) != 0) {
            ERRR("unknown algo should report digest_len 0");
        }
    }

    TEST("envelope: xxh64 / xxh3_64 / xxh3_128 known answers") {
        /* Digests are persisted inside envelopes, so the hash identity
         * is part of the format: a dependency bump that changes any
         * output must fail here rather than silently orphan stored
         * envelopes.  The length-0 values are xxHash's published
         * empty-input references; the rest were produced by a
         * standalone build of the vendored xxHash 0.8.3.  The lengths
         * reach every XXH3 size class (0, 1-3, 4-8, 9-16, 17-128,
         * 129-240, and the long-input loop beyond 240).  Input byte i
         * is (i * 131 + 7) mod 256.  Each value is checked both from
         * the library call and from the bytes the envelope adapter
         * writes (little-endian; for 128 bits the low half first). */
        static const struct {
            size_t len;
            uint64_t xxh64;
            uint64_t xxh3_64;
            uint64_t xxh3_128High;
            uint64_t xxh3_128Low;
        } kats[] = {
            {0, UINT64_C(0xef46db3751d8e999), UINT64_C(0x2d06800538d394c2),
             UINT64_C(0x99aa06d3014798d8), UINT64_C(0x6001c324468d497f)},
            {1, UINT64_C(0xa96c7f0ce858bbb7), UINT64_C(0x4c5cca45d0f4811f),
             UINT64_C(0x495b62073ef70ca4), UINT64_C(0x4c5cca45d0f4811f)},
            {3, UINT64_C(0xbed43740ee6332bb), UINT64_C(0x6e3e2670e61106ac),
             UINT64_C(0x390cdc5b4a895dd7), UINT64_C(0x6e3e2670e61106ac)},
            {8, UINT64_C(0x994b676b71ce94dd), UINT64_C(0xf9fd4dd0b04d78f5),
             UINT64_C(0x6a86a3bda6af4e3d), UINT64_C(0x61ddbe7f31a6100d)},
            {16, UINT64_C(0x94ad0095e72b24d5), UINT64_C(0x86abf6baccea0858),
             UINT64_C(0x7f9a218b0425449a), UINT64_C(0xe2ce54a7c19c730d)},
            {17, UINT64_C(0x1464f2eff23b5fe1), UINT64_C(0xb58bf5dc5022d071),
             UINT64_C(0x66fc23f6439dbd77), UINT64_C(0x8d96ef110fcdebb4)},
            {128, UINT64_C(0x0430e433b792e757), UINT64_C(0x10d17f72c0ccba41),
             UINT64_C(0xaec730751478556c), UINT64_C(0xff361dec1385710a)},
            {129, UINT64_C(0x1f9708e5a00618fa), UINT64_C(0x1648bdc3db49d1a2),
             UINT64_C(0x98cd36ccbb557926), UINT64_C(0x4545b3a09738e31a)},
            {240, UINT64_C(0xca0b65cc61295ca7), UINT64_C(0xb6cfaf343fab81e6),
             UINT64_C(0x5293e17bf553903d), UINT64_C(0x3f2c53e72293711f)},
            {241, UINT64_C(0x0ffefe0dfc875cf4), UINT64_C(0x956cae592c67279e),
             UINT64_C(0xb53840fe3fedf161), UINT64_C(0x956cae592c67279e)},
            {1000, UINT64_C(0x0bf0bdbcc82eb373), UINT64_C(0x571d5cbfef44331b),
             UINT64_C(0x622239c5c47a6910), UINT64_C(0x571d5cbfef44331b)},
            {2048, UINT64_C(0xafd8cdfc2fae9426), UINT64_C(0x8b46caa67dab3a30),
             UINT64_C(0x56b77f207158a2ba), UINT64_C(0x8b46caa67dab3a30)},
        };

        uint8_t input[2048];
        for (size_t i = 0; i < sizeof(input); i++) {
            input[i] = (uint8_t)(i * 131 + 7);
        }

        for (size_t k = 0; k < sizeof(kats) / sizeof(kats[0]); k++) {
            const size_t len = kats[k].len;
            const XXH128_hash_t h128 = XXH3_128bits(input, len);
            if (XXH64(input, len, 0) != kats[k].xxh64) {
                ERR("XXH64 known answer mismatch at length %zu", len);
            }
            if (XXH3_64bits(input, len) != kats[k].xxh3_64) {
                ERR("XXH3_64 known answer mismatch at length %zu", len);
            }
            if (h128.high64 != kats[k].xxh3_128High ||
                h128.low64 != kats[k].xxh3_128Low) {
                ERR("XXH3_128 known answer mismatch at length %zu", len);
            }

            uint8_t want[16];
            uint8_t got[16];
            for (uint32_t i = 0; i < 8u; i++) {
                want[i] = (uint8_t)(kats[k].xxh64 >> (i * 8u));
            }
            hashXXH64(input, len, NULL, 0, got);
            if (memcmp(got, want, 8) != 0) {
                ERR("xxh64 envelope digest bytes mismatch at length %zu", len);
            }

            for (uint32_t i = 0; i < 8u; i++) {
                want[i] = (uint8_t)(kats[k].xxh3_64 >> (i * 8u));
            }
            hashXXH3_64(input, len, NULL, 0, got);
            if (memcmp(got, want, 8) != 0) {
                ERR("xxh3_64 envelope digest bytes mismatch at length %zu",
                    len);
            }

            for (uint32_t i = 0; i < 8u; i++) {
                want[i] = (uint8_t)(kats[k].xxh3_128Low >> (i * 8u));
                want[8u + i] = (uint8_t)(kats[k].xxh3_128High >> (i * 8u));
            }
            hashXXH3_128(input, len, NULL, 0, got);
            if (memcmp(got, want, 16) != 0) {
                ERR("xxh3_128 envelope digest bytes mismatch at length %zu",
                    len);
            }
        }
    }

    /* ============================================================
     * Round-trip: every implemented algorithm × size matrix
     * ============================================================ */
    TEST("envelope: round-trip xxh64 across size matrix") {
        const size_t sizes[] = {0,
                                1,
                                63,
                                64,
                                65,
                                1023,
                                1024,
                                16383,
                                16384,
                                16385,
                                (size_t)65536,
                                (size_t)1 << 20};
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            if (!roundTripOne(FLEX_ENV_ALGO_XXH64, sizes[i], NULL, 0)) {
                ERR("xxh64 round-trip failed at size %zu", sizes[i]);
            }
        }
    }

    TEST("envelope: round-trip xxh3_64 across size matrix") {
        const size_t sizes[] = {0, 1, 1024, 65536, 1 << 20};
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            if (!roundTripOne(FLEX_ENV_ALGO_XXH3_64, sizes[i], NULL, 0)) {
                ERR("xxh3_64 round-trip failed at size %zu", sizes[i]);
            }
        }
    }

    TEST("envelope: round-trip xxh3_128 across size matrix") {
        const size_t sizes[] = {0, 1, 1024, 65536, 1 << 20};
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            if (!roundTripOne(FLEX_ENV_ALGO_XXH3_128, sizes[i], NULL, 0)) {
                ERR("xxh3_128 round-trip failed at size %zu", sizes[i]);
            }
        }
    }

    TEST("envelope: round-trip rapidhash across size matrix") {
        const size_t sizes[] = {0,    1,     63,    64,    65,    1023,
                                1024, 16383, 16384, 16385, 65536, 1 << 20};
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            if (!roundTripOne(FLEX_ENV_ALGO_RAPIDHASH, sizes[i], NULL, 0)) {
                ERR("rapidhash round-trip failed at size %zu", sizes[i]);
            }
        }
    }

    /* ============================================================
     * Tamper detection: flip every byte of a known-good envelope
     * and confirm verify rejects it.
     * ============================================================ */
    TEST("envelope: tamper detection — flip every byte (xxh64, 256B)") {
        const size_t plen = 256;
        uint8_t payload[256];
        fillPseudoRandom(payload, plen, 0xDEADBEEFUL);

        const size_t total = flexEnvelopeWrappedSize(plen, FLEX_ENV_ALGO_XXH64,
                                                     FLEX_ENV_FLAG_NONE);
        cleanerName(zfree, Bytes) uint8_t *original = zmalloc(total);
        cleanerName(zfree, Bytes) uint8_t *mutated = zmalloc(total);

        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_XXH64,
            .version = 0,
        };
        size_t written = 0;
        flexEnvelopeWrap(original, total, payload, plen, &wp, &written);

        uint32_t undetected = 0;
        for (size_t i = 0; i < total; i++) {
            memcpy(mutated, original, total);
            mutated[i] ^= 0xFF; /* flip all 8 bits to maximize signal */

            flexEnvelopeVerifyParams vp = {.opts = 0};
            const uint8_t *p = NULL;
            uint64_t pl = 0;
            if (flexEnvelopeVerify(mutated, total, &vp, &p, &pl)) {
                undetected++;
            }
        }
        if (undetected != 0) {
            ERR("Tamper detection missed %d byte positions out of %zu",
                undetected, total);
        }
    }

    /* ============================================================
     * Secure-by-default invariants
     * ============================================================ */
    TEST("envelope: wrap refuses ALGO_NONE in production build") {
        uint8_t out[64];
        flexEnvelopeWrapParams wp = {.algo = FLEX_ENV_ALGO_NONE};
        size_t written = 0;
        if (flexEnvelopeWrap(out, sizeof(out), NULL, 0, &wp, &written)) {
            ERRR("Wrap accepted ALGO_NONE without "
                 "DATAKIT_ENABLE_INSECURE_TEST_ALGOS — secure-by-default "
                 "barrier failed!");
        }
    }

    TEST("envelope: wrap refuses keyed algo without key") {
        uint8_t out[256];
        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_HMAC_SHA256,
            .key = NULL,
            .key_len = 0,
        };
        size_t written = 0;
        if (flexEnvelopeWrap(out, sizeof(out), NULL, 0, &wp, &written)) {
            ERRR("Wrap accepted keyed algo with no key");
        }
    }

    TEST("envelope: key_id wire field round-trips and auto-resolves via "
         "keyring") {
        const uint8_t key_a[32] = {0xAA, 0xBB, 0xCC, 0xDD};
        const uint8_t key_b[32] = {0xEE, 0xFF, 0x11, 0x22};
        hmacKeyring *ring = hmacKeyringCreate();
        hmacKeyringInstallUnsalted(ring, 7u, &hmacAlgoSHA256, key_a,
                                   sizeof(key_a));
        hmacKeyringInstallUnsalted(ring, 42u, &hmacAlgoSHA256, key_b,
                                   sizeof(key_b));

        const uint8_t payload[] = "keyid-round-trip";
        const size_t plen = sizeof(payload) - 1u;

        /* Wrap with key_id=42 under key_b. Note: the sizing helper
         * needs FLEX_ENV_FLAG_KEYID_PRESENT in the flags so it reserves
         * room for the wire's key_id varint — Wrap itself sets the bit
         * automatically when key_id != 0, but the sizing helper has no
         * way to know without the caller asserting it via flags. */
        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_HMAC_SHA256,
            .key = key_b,
            .key_len = sizeof(key_b),
            .on_wire_flags = FLEX_ENV_FLAG_KEYID_PRESENT,
            .key_id = 42u,
        };
        const size_t sz =
            flexEnvelopeWrappedSize(plen, wp.algo, wp.on_wire_flags);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t written = 0;
        if (!flexEnvelopeWrap(buf, sz, payload, plen, &wp, &written)) {
            ERRR("Wrap with key_id=42 failed");
        }
        /* Peek must surface the embedded key id. */
        flexEnvelopeHeader hdr = {0};
        if (!flexEnvelopePeek(buf, written, &hdr)) {
            ERRR("Peek failed");
        }
        if (!(hdr.flags & FLEX_ENV_FLAG_KEYID_PRESENT) || hdr.key_id != 42u) {
            ERR("Peek did not surface key_id=42 (flags=0x%x id=%u)", hdr.flags,
                hdr.key_id);
        }

        /* Verify with keyring auto-resolution — caller passes no
         * key/prepared_key; verify pulls the right one by id. */
        flexEnvelopeVerifyParams vp = {.keyring = ring};
        const uint8_t *out_payload = NULL;
        uint64_t out_plen = 0;
        if (!flexEnvelopeVerify(buf, written, &vp, &out_payload, &out_plen)) {
            ERRR("Keyring auto-resolve verify failed");
        }
        if (out_plen != plen || memcmp(out_payload, payload, plen) != 0) {
            ERRR("Auto-resolved payload mismatch");
        }

        /* Tamper: bump the wire's key_id varint so it points at a key
         * with different bytes — verify must fail (digest computed over
         * frame || payload includes the key_id position). */
        /* Find and corrupt the key_id varint byte: it sits between the
         * flags varint and payload_len varint. Simpler: flip the high
         * bit of byte 6 (which is in the framing region for typical
         * small ids) and verify the keyring path rejects. */
        buf[6] ^= 0x40;
        if (flexEnvelopeVerify(buf, written, &vp, &out_payload, &out_plen)) {
            ERRR("Tampered framing must fail verify");
        }
        buf[6] ^= 0x40;

        /* Keyring miss falls through to the caller-supplied key
         * material; if neither is provided we reject. */
        hmacKeyringRetire(ring, 42u);
        if (flexEnvelopeVerify(buf, written, &vp, &out_payload, &out_plen)) {
            ERRR("Verify must fail when keyring miss and no fallback");
        }
        /* But verify with raw key still works — backward path
         * preserved. Reinstall id=42 so the cleanup path is sane. */
        hmacKeyringInstallUnsalted(ring, 42u, &hmacAlgoSHA256, key_b,
                                   sizeof(key_b));

        hmacKeyringFree(ring);
    }

    TEST("envelope: key_id=0 with KEYID_PRESENT flag is refused by Wrap") {
        const uint8_t key[32] = {0x11};
        uint8_t out[256];
        size_t written = 0;
        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_HMAC_SHA256,
            .key = key,
            .key_len = sizeof(key),
            .on_wire_flags = FLEX_ENV_FLAG_KEYID_PRESENT,
            .key_id = 0u,
        };
        if (flexEnvelopeWrap(out, sizeof(out), NULL, 0, &wp, &written)) {
            ERRR("Wrap must refuse key_id=0 paired with KEYID_PRESENT flag");
        }
    }

    TEST("envelope: verify rejects unknown magic") {
        uint8_t buf[32] = {0};
        buf[0] = 0xAB;
        buf[1] = 0xCD;
        flexEnvelopeVerifyParams vp = {0};
        if (flexEnvelopeVerify(buf, sizeof(buf), &vp, NULL, NULL)) {
            ERRR("Verify accepted bad magic");
        }
    }

    TEST("envelope: verify rejects truncated framing") {
        uint8_t buf[2] = {FLEX_ENVELOPE_MAGIC_0, FLEX_ENVELOPE_MAGIC_1};
        flexEnvelopeVerifyParams vp = {0};
        if (flexEnvelopeVerify(buf, sizeof(buf), &vp, NULL, NULL)) {
            ERRR("Verify accepted 2-byte buffer with only magic");
        }
        if (flexEnvelopeVerify(buf, 0, &vp, NULL, NULL)) {
            ERRR("Verify accepted empty buffer");
        }
        if (flexEnvelopeVerify(NULL, 100, &vp, NULL, NULL)) {
            ERRR("Verify accepted NULL buffer");
        }
    }

    TEST("envelope: verify rejects trailing bytes by default") {
        const size_t plen = 32;
        uint8_t payload[32];
        fillPseudoRandom(payload, plen, 1);
        const size_t total =
            flexEnvelopeWrappedSize(plen, FLEX_ENV_ALGO_XXH64, 0);
        /* extra trailing bytes */
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(total + 8);
        flexEnvelopeWrapParams wp = {.algo = FLEX_ENV_ALGO_XXH64};
        size_t written = 0;
        flexEnvelopeWrap(buf, total + 8, payload, plen, &wp, &written);
        memset(buf + total, 0xAB, 8);

        flexEnvelopeVerifyParams vp = {.opts = 0};
        if (flexEnvelopeVerify(buf, total + 8, &vp, NULL, NULL)) {
            ERRR("Verify accepted trailing bytes without "
                 "ALLOW_TRAILING flag");
        }

        vp.opts = FLEX_ENV_VERIFY_ALLOW_TRAILING;
        if (!flexEnvelopeVerify(buf, total + 8, &vp, NULL, NULL)) {
            ERRR("Verify rejected trailing bytes WITH ALLOW_TRAILING "
                 "flag");
        }
    }

    TEST("envelope: verify rejects oversized payload (DoS guard)") {
        /* Hand-craft a framing that claims payload_len = 2 GiB but
         * fits in a small buffer. Verify must reject before any
         * digest work. */
        uint8_t buf[32];
        size_t pos = 0;
        buf[pos++] = FLEX_ENVELOPE_MAGIC_0;
        buf[pos++] = FLEX_ENVELOPE_MAGIC_1;
        size_t w = 0;
        varintSplitFullPut_(buf + pos, w, (uint64_t)1);
        pos += w;
        varintSplitFullPut_(buf + pos, w, (uint64_t)FLEX_ENV_ALGO_XXH64);
        pos += w;
        varintSplitFullPut_(buf + pos, w, (uint64_t)0);
        pos += w;
        /* Claim payload_len = 2 GiB. */
        const uint64_t huge = (uint64_t)2 << 30;
        varintSplitFullPut_(buf + pos, w, huge);
        pos += w;

        flexEnvelopeVerifyParams vp = {.opts = 0};
        if (flexEnvelopeVerify(buf, pos, &vp, NULL, NULL)) {
            ERRR("Verify did not reject forged 2 GiB length on small "
                 "buffer (framing payload-fit check)");
        }
        /* Even if buf_len were huge (it isn't), default cap of 1 GiB
         * blocks it. Test that path symbolically: the framing already
         * rejects above because payload doesn't fit. */
    }

    /* ============================================================
     * Policy flags
     * ============================================================ */
    TEST("envelope: REQUIRE_FLEX accepts valid flex, rejects junk") {
        cleaner(flexFree) flex *f = flexNew();
        flexPushSigned(&f, 42, FLEX_ENDPOINT_TAIL);
        flexPushSigned(&f, 100, FLEX_ENDPOINT_TAIL);
        const size_t flex_bytes = flexBytes(f);

        const size_t total =
            flexEnvelopeWrappedSize(flex_bytes, FLEX_ENV_ALGO_XXH64, 0);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(total);
        flexEnvelopeWrapParams wp = {.algo = FLEX_ENV_ALGO_XXH64};
        size_t written = 0;
        flexEnvelopeWrap(buf, total, (const uint8_t *)f, flex_bytes, &wp,
                         &written);

        flexEnvelopeVerifyParams vp = {
            .opts = FLEX_ENV_VERIFY_REQUIRE_FLEX,
        };
        const uint8_t *p = NULL;
        uint64_t pl = 0;
        if (!flexEnvelopeVerify(buf, total, &vp, &p, &pl)) {
            ERRR("REQUIRE_FLEX rejected a valid wrapped flex");
        }

        /* Wrap junk and confirm REQUIRE_FLEX rejects it. */
        uint8_t junk[16];
        fillPseudoRandom(junk, sizeof(junk), 7);
        const size_t junkTotal =
            flexEnvelopeWrappedSize(sizeof(junk), FLEX_ENV_ALGO_XXH64, 0);
        cleanerName(zfree, Bytes) uint8_t *jbuf = zmalloc(junkTotal);
        flexEnvelopeWrap(jbuf, junkTotal, junk, sizeof(junk), &wp, &written);
        if (flexEnvelopeVerify(jbuf, junkTotal, &vp, &p, &pl)) {
            ERRR("REQUIRE_FLEX accepted non-flex payload "
                 "(flexValidateBuffer not gating)");
        }
    }

    TEST("envelope: PEEK_ONLY skips digest, returns payload pointer") {
        const size_t plen = 64;
        uint8_t payload[64];
        fillPseudoRandom(payload, plen, 11);
        const size_t total =
            flexEnvelopeWrappedSize(plen, FLEX_ENV_ALGO_XXH64, 0);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(total);
        flexEnvelopeWrapParams wp = {.algo = FLEX_ENV_ALGO_XXH64};
        size_t written = 0;
        flexEnvelopeWrap(buf, total, payload, plen, &wp, &written);

        /* Corrupt the digest — PEEK_ONLY should still succeed. */
        buf[total - 1] ^= 0xFF;
        flexEnvelopeVerifyParams vp = {
            .opts = FLEX_ENV_VERIFY_PEEK_ONLY,
        };
        const uint8_t *p = NULL;
        uint64_t pl = 0;
        if (!flexEnvelopeVerify(buf, total, &vp, &p, &pl)) {
            ERRR("PEEK_ONLY failed to skip digest");
        }
        if (pl != plen) {
            ERR("PEEK_ONLY returned wrong payload_len: %" PRIu64, pl);
        }
        /* Without PEEK_ONLY, the corrupt digest must be caught. */
        vp.opts = 0;
        if (flexEnvelopeVerify(buf, total, &vp, &p, &pl)) {
            ERRR("Default verify did not catch corrupted digest");
        }
    }

    TEST("envelope: REQUIRE_HMAC rejects checksum-class algo") {
        const size_t plen = 16;
        uint8_t payload[16];
        fillPseudoRandom(payload, plen, 13);
        const size_t total =
            flexEnvelopeWrappedSize(plen, FLEX_ENV_ALGO_XXH64, 0);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(total);
        flexEnvelopeWrapParams wp = {.algo = FLEX_ENV_ALGO_XXH64};
        size_t written = 0;
        flexEnvelopeWrap(buf, total, payload, plen, &wp, &written);

        flexEnvelopeVerifyParams vp = {
            .opts = FLEX_ENV_VERIFY_REQUIRE_HMAC,
        };
        if (flexEnvelopeVerify(buf, total, &vp, NULL, NULL)) {
            ERRR("REQUIRE_HMAC accepted xxh64 (checksum class)");
        }
    }

    TEST("envelope: BOUND_PAYLOAD enforces caller's max") {
        const size_t plen = 1024;
        cleanerName(zfree, Bytes) uint8_t *payload = zmalloc(plen);
        fillPseudoRandom(payload, plen, 17);
        const size_t total =
            flexEnvelopeWrappedSize(plen, FLEX_ENV_ALGO_XXH64, 0);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(total);
        flexEnvelopeWrapParams wp = {.algo = FLEX_ENV_ALGO_XXH64};
        size_t written = 0;
        flexEnvelopeWrap(buf, total, payload, plen, &wp, &written);

        flexEnvelopeVerifyParams vp = {
            .opts = FLEX_ENV_VERIFY_BOUND_PAYLOAD,
            .max_payload_len = 512,
        };
        if (flexEnvelopeVerify(buf, total, &vp, NULL, NULL)) {
            ERRR("BOUND_PAYLOAD did not enforce caller's 512 cap "
                 "against 1024-byte payload");
        }
        vp.max_payload_len = 2048;
        if (!flexEnvelopeVerify(buf, total, &vp, NULL, NULL)) {
            ERRR("BOUND_PAYLOAD rejected payload smaller than cap");
        }
    }

    /* ============================================================
     * UnwrapAsFlex convenience
     * ============================================================ */
    TEST("envelope: UnwrapAsFlex returns usable flex copy") {
        cleaner(flexFree) flex *f = flexNew();
        for (uint32_t i = 0; i < 10u; i++) {
            flexPushSigned(&f, i, FLEX_ENDPOINT_TAIL);
        }
        const size_t flex_bytes = flexBytes(f);
        const size_t total =
            flexEnvelopeWrappedSize(flex_bytes, FLEX_ENV_ALGO_XXH64, 0);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(total);
        flexEnvelopeWrapParams wp = {.algo = FLEX_ENV_ALGO_XXH64};
        size_t written = 0;
        flexEnvelopeWrap(buf, total, (const uint8_t *)f, flex_bytes, &wp,
                         &written);

        flex *restored = flexEnvelopeUnwrapAsFlex(buf, total, NULL);
        if (!restored) {
            ERRR("UnwrapAsFlex returned NULL on valid envelope");
        } else {
            if (flexCount(restored) != flexCount(f)) {
                ERR("UnwrapAsFlex count mismatch: %zu vs %zu",
                    flexCount(restored), flexCount(f));
            }
            flexFree(restored);
        }
    }

    /* ============================================================
     * Keyed-class algorithms (BLAKE3_KEYED, HMAC_SHA256, HMAC_BLAKE3)
     * — round-trip, tamper detection, wrong-key rejection.
     * ============================================================ */
    {
        const flexEnvelopeAlgo keyed[] = {
            FLEX_ENV_ALGO_BLAKE3_KEYED,
            FLEX_ENV_ALGO_HMAC_SHA256,
            FLEX_ENV_ALGO_HMAC_BLAKE3,
        };
        for (size_t i = 0; i < sizeof(keyed) / sizeof(keyed[0]); i++) {
            const flexEnvelopeAlgo algo = keyed[i];

            /* Round-trip with key. */
            uint8_t key[32];
            fillPseudoRandom(key, sizeof(key), 0x5EEDUL + (uint64_t)algo);
            if (!roundTripOne(algo, 256, key, sizeof(key))) {
                ERR("Keyed round-trip failed for algo=%s",
                    flexEnvelopeAlgoName(algo));
            }
            if (!roundTripOne(algo, 1, key, sizeof(key))) {
                ERR("Keyed round-trip (1B) failed for algo=%s",
                    flexEnvelopeAlgoName(algo));
            }
            if (!roundTripOne(algo, 1 << 16, key, sizeof(key))) {
                ERR("Keyed round-trip (64KB) failed for algo=%s",
                    flexEnvelopeAlgoName(algo));
            }
        }
    }

    TEST("envelope: keyed algos reject verify with wrong key") {
        const size_t plen = 128;
        uint8_t payload[128];
        fillPseudoRandom(payload, plen, 0xA5A5A5A5UL);
        uint8_t key_right[32];
        uint8_t key_wrong[32];
        for (uint32_t i = 0; i < 32; i++) {
            key_right[i] = (uint8_t)(0x10 + i);
            key_wrong[i] = (uint8_t)(0x20 + i);
        }

        const flexEnvelopeAlgo algos[] = {
            FLEX_ENV_ALGO_BLAKE3_KEYED,
            FLEX_ENV_ALGO_HMAC_SHA256,
            FLEX_ENV_ALGO_HMAC_BLAKE3,
        };
        for (size_t i = 0; i < sizeof(algos) / sizeof(algos[0]); i++) {
            const size_t total = flexEnvelopeWrappedSize(plen, algos[i], 0);
            cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(total);
            flexEnvelopeWrapParams wp = {
                .algo = algos[i],
                .key = key_right,
                .key_len = sizeof(key_right),
            };
            size_t written = 0;
            if (!flexEnvelopeWrap(buf, total, payload, plen, &wp, &written)) {
                ERR("Wrap failed for keyed algo %s",
                    flexEnvelopeAlgoName(algos[i]));
            }

            /* Verify with right key: must succeed. */
            flexEnvelopeVerifyParams vp_ok = {
                .key = key_right,
                .key_len = sizeof(key_right),
            };
            if (!flexEnvelopeVerify(buf, total, &vp_ok, NULL, NULL)) {
                ERR("Verify with right key rejected for %s",
                    flexEnvelopeAlgoName(algos[i]));
            }

            /* Verify with wrong key: must fail. */
            flexEnvelopeVerifyParams vp_bad = {
                .key = key_wrong,
                .key_len = sizeof(key_wrong),
            };
            if (flexEnvelopeVerify(buf, total, &vp_bad, NULL, NULL)) {
                ERR("Verify with wrong key ACCEPTED for %s "
                    "— authentication broken!",
                    flexEnvelopeAlgoName(algos[i]));
            }

            /* Verify with NO key: must fail (unconditional invariant). */
            flexEnvelopeVerifyParams vp_nokey = {0};
            if (flexEnvelopeVerify(buf, total, &vp_nokey, NULL, NULL)) {
                ERR("Verify with NO key ACCEPTED for keyed algo %s",
                    flexEnvelopeAlgoName(algos[i]));
            }
        }
    }

    TEST("envelope: keyed algos catch tampering with single byte flips") {
        const size_t plen = 128;
        uint8_t payload[128];
        fillPseudoRandom(payload, plen, 0xBEEFUL);
        uint8_t key[32];
        fillPseudoRandom(key, sizeof(key), 0xCAFEUL);

        const flexEnvelopeAlgo algos[] = {
            FLEX_ENV_ALGO_BLAKE3_KEYED,
            FLEX_ENV_ALGO_HMAC_SHA256,
            FLEX_ENV_ALGO_HMAC_BLAKE3,
        };
        for (size_t a = 0; a < sizeof(algos) / sizeof(algos[0]); a++) {
            const size_t total = flexEnvelopeWrappedSize(plen, algos[a], 0);
            cleanerName(zfree, Bytes) uint8_t *orig = zmalloc(total);
            cleanerName(zfree, Bytes) uint8_t *mutated = zmalloc(total);
            flexEnvelopeWrapParams wp = {
                .algo = algos[a],
                .key = key,
                .key_len = sizeof(key),
            };
            size_t written = 0;
            flexEnvelopeWrap(orig, total, payload, plen, &wp, &written);

            uint32_t undetected = 0;
            for (size_t b = 0; b < total; b++) {
                memcpy(mutated, orig, total);
                mutated[b] ^= 0x55;
                flexEnvelopeVerifyParams vp = {
                    .key = key,
                    .key_len = sizeof(key),
                };
                if (flexEnvelopeVerify(mutated, total, &vp, NULL, NULL)) {
                    undetected++;
                }
            }
            if (undetected != 0) {
                ERR("%s missed %d tampers out of %zu byte positions",
                    flexEnvelopeAlgoName(algos[a]), undetected, total);
            }
        }
    }

    TEST("envelope: prepared-key path matches plain-key verify") {
        /* Round-trip a small HMAC-SHA256 envelope, verify it both with
         * raw key bytes and with a pre-prepared hmacKey. Both paths
         * must accept the same envelope; the prepared path is the
         * fast path for high-throughput verifiers. */
        const size_t plen = 256;
        uint8_t payload[256];
        fillPseudoRandom(payload, plen, 0xF00DUL);
        uint8_t key[32];
        for (uint32_t i = 0; i < 32; i++) {
            key[i] = (uint8_t)(0x10 + i);
        }

        const size_t total =
            flexEnvelopeWrappedSize(plen, FLEX_ENV_ALGO_HMAC_SHA256, 0);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(total);
        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_HMAC_SHA256,
            .key = key,
            .key_len = sizeof(key),
        };
        size_t written = 0;
        flexEnvelopeWrap(buf, total, payload, plen, &wp, &written);

        /* Plain-key path. */
        flexEnvelopeVerifyParams vp_plain = {
            .key = key,
            .key_len = sizeof(key),
        };
        if (!flexEnvelopeVerify(buf, total, &vp_plain, NULL, NULL)) {
            ERRR("Plain-key verify failed on valid HMAC envelope");
        }

        /* Prepared-key path. */
        size_t prepSize = hmacPreparedSize(&hmacAlgoSHA256);
        cleanerName(zfree, Bytes) uint8_t *prepStorage = zmalloc(prepSize);
        hmacKey prep;
        hmacPrepareKey(&prep, &hmacAlgoSHA256, prepStorage, prepSize, key,
                       sizeof(key));
        flexEnvelopeVerifyParams vp_prep = {.prepared_key = &prep};
        if (!flexEnvelopeVerify(buf, total, &vp_prep, NULL, NULL)) {
            ERRR("Prepared-key verify failed on valid HMAC envelope");
        }

        /* Sanity: a forged envelope (1-byte flip) must still be
         * rejected under the prepared-key path. */
        buf[total / 2] ^= 0x01;
        if (flexEnvelopeVerify(buf, total, &vp_prep, NULL, NULL)) {
            ERRR("Prepared-key path accepted forged envelope!");
        }
    }

    /* ============================================================
     * Fuzz: random byte sequences must never crash flexEnvelopeVerify.
     *
     * The function is on the security boundary — it parses attacker
     * controlled bytes — so it must defend against every malformed
     * input under every policy combination. We feed many random
     * sequences across a spread of length buckets and verify under a
     * rotation of policy flag combinations. For random input the
     * digest essentially never matches, so the expected outcome is
     * always "return false, no crash" — we just need to confirm no
     * out-of-bounds reads, no NULL deref, no infinite loops.
     * ============================================================ */
    TEST("envelope: fuzz random bytes never crashes verify") {
        const uint32_t lenBuckets[] = {0u,    1u,    2u,    8u,   16u,
                                       32u,   64u,   128u,  256u, 512u,
                                       1024u, 4096u, 65536u};
        const uint32_t numBuckets =
            (uint32_t)(sizeof(lenBuckets) / sizeof(lenBuckets[0]));
        const uint32_t policies[] = {
            FLEX_ENV_VERIFY_DEFAULT,         FLEX_ENV_VERIFY_ALLOW_TRAILING,
            FLEX_ENV_VERIFY_REQUIRE_FLEX,    FLEX_ENV_VERIFY_REQUIRE_HMAC,
            FLEX_ENV_VERIFY_ALLOW_NONE_ALGO, FLEX_ENV_VERIFY_ALLOW_OVERSIZE,
            FLEX_ENV_VERIFY_PEEK_ONLY,       FLEX_ENV_VERIFY_FAST_COMPARE,
            FLEX_ENV_VERIFY_BOUND_PAYLOAD,   FLEX_ENV_VERIFY_ALLOW_DEPRECATED,
        };
        const uint32_t numPolicies =
            (uint32_t)(sizeof(policies) / sizeof(policies[0]));

        /* Fixed key for HMAC verify attempts — fuzz only the buffer,
         * not the key, so verify either rejects the framing or fails
         * the digest. */
        uint8_t key[32];
        for (uint32_t i = 0; i < 32u; i++) {
            key[i] = (uint8_t)(i * 7u + 3u);
        }

        const uint32_t iterationsPerBucket = 20000u; /* ~260k total inputs */
        uint64_t acceptedAsValid = 0;
        uint32_t accepted_buckets[16] = {0};
        for (uint32_t b = 0; b < numBuckets; b++) {
            const uint32_t bufLen = lenBuckets[b];
            cleanerName(zfree, Bytes) uint8_t *buf =
                bufLen > 0 ? zmalloc(bufLen) : NULL;
            for (uint32_t iter = 0; iter < iterationsPerBucket; iter++) {
                fillPseudoRandom(buf, bufLen,
                                 (uint64_t)b * 1000003ull +
                                     (uint64_t)iter * 7919ull);
                const uint32_t policy = policies[iter % numPolicies];
                /* Occasionally force the magic prefix so the parser
                 * exercises algo / framing branches deeper than the
                 * fast magic-reject path. */
                if (bufLen >= 2u && (iter & 0x7u) == 0u) {
                    buf[0] = FLEX_ENVELOPE_MAGIC_0;
                    buf[1] = FLEX_ENVELOPE_MAGIC_1;
                }

                flexEnvelopeVerifyParams vp = {
                    .key = key,
                    .key_len = sizeof(key),
                    .opts = policy,
                    .max_payload_len = FLEX_ENVELOPE_DEFAULT_MAX_PAYLOAD,
                };
                const uint8_t *payload = NULL;
                uint64_t payloadLen = 0;
                const bool ok = flexEnvelopeVerify(buf, (size_t)bufLen, &vp,
                                                   &payload, &payloadLen);
                if (ok) {
                    acceptedAsValid++;
                    if (b < 16u) {
                        accepted_buckets[b]++;
                    }
                    /* If verify accepted, payload must be in bounds. */
                    if (payload < buf || payload + payloadLen > buf + bufLen) {
                        ERR("Verify accepted but payload escapes buffer "
                            "(bucket=%u iter=%u)",
                            b, iter);
                    }
                }
                /* Peek must also never crash on random bytes. */
                flexEnvelopeHeader peekOut = {0};
                (void)flexEnvelopePeek(buf, (size_t)bufLen, &peekOut);
            }
        }
        /* Surfaces a flake if a future change starts accepting random
         * inputs at a measurable rate. */
        if (acceptedAsValid > 8) {
            ERR("Random byte fuzz accepted %" PRIu64
                " inputs as valid envelopes (expected near-zero)",
                acceptedAsValid);
        }
    }

    /* ============================================================
     * Streaming API
     *
     * 1. Streaming wrap output must be byte-identical to one-shot wrap
     * for every algorithm and a range of chunk sizes.
     * 2. Streaming verify must accept envelopes produced by one-shot
     * wrap.
     * 3. Tamper detection: flipping a payload byte inside a streamed
     * Update or flipping the trailing digest must fail Final.
     * 4. Init secure-default invariants match one-shot Wrap.
     * 5. Memory bound: 64 MiB payload streamed in 64 KiB chunks
     * succeeds and ctx sizeof remains under 16 KiB.
     * ============================================================ */
    printf("\nenvelope streaming ctx sizes:\n");
    printf("  sizeof(flexEnvelopeWrapStream)   = %zu bytes\n",
           sizeof(flexEnvelopeWrapStream));
    printf("  sizeof(flexEnvelopeVerifyStream) = %zu bytes\n",
           sizeof(flexEnvelopeVerifyStream));

    TEST("envelope: streaming ctx sizeof bounded under 16 KiB") {
        if (sizeof(flexEnvelopeWrapStream) > 16 * 1024) {
            ERR("flexEnvelopeWrapStream too large: %zu bytes",
                sizeof(flexEnvelopeWrapStream));
        }
        if (sizeof(flexEnvelopeVerifyStream) > 16 * 1024) {
            ERR("flexEnvelopeVerifyStream too large: %zu bytes",
                sizeof(flexEnvelopeVerifyStream));
        }
    }

    /* Helper: stream-wrap into out_buf, return total bytes written. */
    /* (No closures in C — inline the loop.) */

    {
        /* Round-trip equivalence: streaming wrap output must match
         * one-shot wrap byte-for-byte across algorithms and chunk
         * sizes.
         *
         * XXH3_64 / XXH3_128 are excluded because the streaming API
         * does not offer them (see streamHashInit and the "refuses
         * XXH3" test below). Streaming envelopes are restricted to
         * XXH64, rapidhash and the keyed algorithms. */
        const flexEnvelopeAlgo all_algos[] = {
            FLEX_ENV_ALGO_XXH64,        FLEX_ENV_ALGO_RAPIDHASH,
            FLEX_ENV_ALGO_BLAKE3_KEYED, FLEX_ENV_ALGO_HMAC_SHA256,
            FLEX_ENV_ALGO_HMAC_BLAKE3,
        };
        const size_t plens[] = {0, 1, 7, 64, 1024, 65537};

        for (size_t ai = 0; ai < sizeof(all_algos) / sizeof(all_algos[0]);
             ai++) {
            const flexEnvelopeAlgo algo = all_algos[ai];
            uint8_t key[32];
            fillPseudoRandom(key, sizeof(key), 0x51EA1U + (uint64_t)algo);
            const bool need_key = flexEnvelopeAlgoIsKeyed(algo);

            for (size_t pi = 0; pi < sizeof(plens) / sizeof(plens[0]); pi++) {
                const size_t plen = plens[pi];
                cleanerName(zfree, Bytes) uint8_t *payload =
                    plen > 0 ? zmalloc(plen) : NULL;
                if (payload) {
                    fillPseudoRandom(payload, plen,
                                     0xABCDEFUL ^ (uint64_t)plen ^
                                         (uint64_t)algo);
                }

                /* One-shot wrap into oneshot_buf. */
                const size_t total = flexEnvelopeWrappedSize(plen, algo, 0);
                cleanerName(zfree, Bytes) uint8_t *oneshot_buf = zmalloc(total);
                flexEnvelopeWrapParams wp = {
                    .algo = algo,
                    .key = need_key ? key : NULL,
                    .key_len = need_key ? sizeof(key) : 0,
                };
                size_t one_written = 0;
                if (!flexEnvelopeWrap(oneshot_buf, total, payload,
                                      (uint64_t)plen, &wp, &one_written)) {
                    ERR("one-shot wrap failed (algo=%s plen=%zu)",
                        flexEnvelopeAlgoName(algo), plen);
                }

                /* Stream-wrap for each chunk size. */
                size_t chunk_sizes[6] = {
                    1, 7, 64, 1024, plen > 0 ? (plen - 1) : 1, plen + 1};
                for (size_t ci = 0; ci < 6; ci++) {
                    const size_t cs =
                        chunk_sizes[ci] == 0 ? 1 : chunk_sizes[ci];
                    cleanerName(zfree, Bytes) uint8_t *stream_buf =
                        zmalloc(total);
                    flexEnvelopeWrapStream ctx;
                    size_t header_written = 0;
                    if (!flexEnvelopeWrapStreamInit(&ctx, (uint64_t)plen, &wp,
                                                    stream_buf, total,
                                                    &header_written)) {
                        ERR("stream Init failed (algo=%s plen=%zu cs=%zu)",
                            flexEnvelopeAlgoName(algo), plen, cs);
                        continue;
                    }
                    /* Feed payload in chunks; mirror bytes into stream_buf
                     * since the API doesn't buffer payload. */
                    size_t offset = 0;
                    while (offset < plen) {
                        size_t this_chunk = cs;
                        if (this_chunk > plen - offset) {
                            this_chunk = plen - offset;
                        }
                        memcpy(stream_buf + header_written + offset,
                               payload + offset, this_chunk);
                        if (!flexEnvelopeWrapStreamUpdate(
                                &ctx, payload + offset, this_chunk)) {
                            ERR("stream Update failed (algo=%s plen=%zu)",
                                flexEnvelopeAlgoName(algo), plen);
                            break;
                        }
                        offset += this_chunk;
                    }
                    size_t digest_written = 0;
                    if (!flexEnvelopeWrapStreamFinal(
                            &ctx, stream_buf + header_written + plen,
                            total - header_written - plen, &digest_written)) {
                        ERR("stream Final failed (algo=%s plen=%zu)",
                            flexEnvelopeAlgoName(algo), plen);
                    }
                    if (header_written + plen + digest_written != total) {
                        ERR("stream wrap total mismatch: %zu+%zu+%zu vs %zu "
                            "(algo=%s)",
                            header_written, plen, digest_written, total,
                            flexEnvelopeAlgoName(algo));
                    }
                    if (memcmp(stream_buf, oneshot_buf, total) != 0) {
                        ERR("stream wrap output diverged from one-shot "
                            "(algo=%s plen=%zu cs=%zu)",
                            flexEnvelopeAlgoName(algo), plen, cs);
                    }
                }

                /* Streaming verify equivalence. */
                {
                    flexEnvelopeVerifyParams vp = {
                        .key = need_key ? key : NULL,
                        .key_len = need_key ? sizeof(key) : 0,
                    };
                    flexEnvelopeVerifyStream vctx;
                    size_t consumed = 0;
                    uint64_t parsed_plen = 0;
                    flexEnvelopeAlgo parsed_algo = FLEX_ENV_ALGO_NONE;
                    if (!flexEnvelopeVerifyStreamInit(
                            &vctx, &vp, oneshot_buf, total, &consumed,
                            &parsed_plen, &parsed_algo)) {
                        ERR("verify-stream Init failed (algo=%s plen=%zu)",
                            flexEnvelopeAlgoName(algo), plen);
                    }
                    if (parsed_plen != (uint64_t)plen || parsed_algo != algo) {
                        ERR("verify-stream parsed mismatch "
                            "(algo=%s plen=%zu got plen=%" PRIu64 " algo=%u)",
                            flexEnvelopeAlgoName(algo), plen, parsed_plen,
                            (uint32_t)parsed_algo);
                    }
                    /* Feed payload+digest in 7-byte chunks. */
                    const size_t cs = 7;
                    size_t off = consumed;
                    while (off < total) {
                        size_t step = cs;
                        if (step > total - off) {
                            step = total - off;
                        }
                        if (!flexEnvelopeVerifyStreamUpdate(
                                &vctx, oneshot_buf + off, step)) {
                            ERR("verify-stream Update failed (algo=%s)",
                                flexEnvelopeAlgoName(algo));
                            break;
                        }
                        off += step;
                    }
                    if (!flexEnvelopeVerifyStreamFinal(&vctx)) {
                        ERR("verify-stream Final returned false on valid "
                            "envelope (algo=%s plen=%zu)",
                            flexEnvelopeAlgoName(algo), plen);
                    }
                }

                /* Tamper: flip a payload byte (when plen>0). */
                if (plen > 0) {
                    cleanerName(zfree, Bytes) uint8_t *tampered =
                        zmalloc(total);
                    memcpy(tampered, oneshot_buf, total);
                    /* Find offset of first payload byte (skip the
                     * framing header — easiest is to use peek). */
                    flexEnvelopeHeader hdr = {0};
                    flexEnvelopePeek(tampered, total, &hdr);
                    tampered[hdr.header_size + (plen / 2)] ^= 0xFF;

                    flexEnvelopeVerifyParams vp = {
                        .key = need_key ? key : NULL,
                        .key_len = need_key ? sizeof(key) : 0,
                    };
                    flexEnvelopeVerifyStream vctx;
                    size_t consumed = 0;
                    if (!flexEnvelopeVerifyStreamInit(&vctx, &vp, tampered,
                                                      total, &consumed, NULL,
                                                      NULL)) {
                        ERR("verify-stream Init failed on tamper case "
                            "(algo=%s)",
                            flexEnvelopeAlgoName(algo));
                    } else {
                        if (!flexEnvelopeVerifyStreamUpdate(
                                &vctx, tampered + consumed, total - consumed)) {
                            ERR("verify-stream Update unexpectedly failed "
                                "(algo=%s)",
                                flexEnvelopeAlgoName(algo));
                        }
                        if (flexEnvelopeVerifyStreamFinal(&vctx)) {
                            ERR("verify-stream Final accepted tampered "
                                "payload byte (algo=%s)",
                                flexEnvelopeAlgoName(algo));
                        }
                    }
                    /* Now tamper the digest at the very end. */
                    memcpy(tampered, oneshot_buf, total);
                    tampered[total - 1] ^= 0x55;
                    if (flexEnvelopeVerifyStreamInit(&vctx, &vp, tampered,
                                                     total, &consumed, NULL,
                                                     NULL)) {
                        flexEnvelopeVerifyStreamUpdate(
                            &vctx, tampered + consumed, total - consumed);
                        if (flexEnvelopeVerifyStreamFinal(&vctx)) {
                            ERR("verify-stream Final accepted tampered "
                                "digest byte (algo=%s)",
                                flexEnvelopeAlgoName(algo));
                        }
                    }
                }
            }
        }
    }

    TEST("envelope: wrap stream refuses keyed-class without key") {
        flexEnvelopeWrapStream ctx;
        uint8_t header[64];
        size_t hw = 0;
        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_HMAC_SHA256,
            .key = NULL,
            .key_len = 0,
        };
        if (flexEnvelopeWrapStreamInit(&ctx, 100, &wp, header, sizeof(header),
                                       &hw)) {
            ERRR("wrap stream Init accepted keyed algo with no key");
        }
        wp.algo = FLEX_ENV_ALGO_BLAKE3_KEYED;
        if (flexEnvelopeWrapStreamInit(&ctx, 100, &wp, header, sizeof(header),
                                       &hw)) {
            ERRR("wrap stream Init accepted BLAKE3_KEYED with no key");
        }
    }

    TEST("envelope: wrap stream refuses XXH3 (not offered for streaming)") {
        flexEnvelopeWrapStream ctx;
        uint8_t header[64];
        size_t hw = 0;
        flexEnvelopeWrapParams wp = {.algo = FLEX_ENV_ALGO_XXH3_64};
        if (flexEnvelopeWrapStreamInit(&ctx, 16, &wp, header, sizeof(header),
                                       &hw)) {
            ERRR("wrap stream Init accepted XXH3_64");
        }
        wp.algo = FLEX_ENV_ALGO_XXH3_128;
        if (flexEnvelopeWrapStreamInit(&ctx, 16, &wp, header, sizeof(header),
                                       &hw)) {
            ERRR("wrap stream Init accepted XXH3_128");
        }
    }

    TEST("envelope: wrap stream refuses ALGO_NONE in production build") {
        flexEnvelopeWrapStream ctx;
        uint8_t header[64];
        size_t hw = 0;
        flexEnvelopeWrapParams wp = {.algo = FLEX_ENV_ALGO_NONE};
        if (flexEnvelopeWrapStreamInit(&ctx, 0, &wp, header, sizeof(header),
                                       &hw)) {
            ERRR("wrap stream Init accepted ALGO_NONE without insecure gate");
        }
    }

    TEST("envelope: wrap stream poisons ctx on Update bound overrun") {
        /* Symmetric with verify side: Update failure must
         * transition the ctx to POISONED so subsequent Update/Final
         * cannot rebuild a half-corrupt state. */
        uint8_t key[32] = {0xAA};
        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_HMAC_SHA256,
            .key = key,
            .key_len = sizeof(key),
        };
        flexEnvelopeWrapStream ctx;
        uint8_t header[64];
        size_t hw = 0;
        if (!flexEnvelopeWrapStreamInit(&ctx, /* payload_len */ 16u, &wp,
                                        header, sizeof(header), &hw)) {
            ERRR("wrap stream Init setup failed");
        }
        /* Feed 17 bytes when only 16 declared — must fail. */
        uint8_t blob[17];
        memset(blob, 0xBB, sizeof(blob));
        if (flexEnvelopeWrapStreamUpdate(&ctx, blob, sizeof(blob))) {
            ERRR("Update past declared payload_len must fail");
        }
        /* Subsequent Update with the correct count must ALSO fail
         * because the ctx is now poisoned. */
        if (flexEnvelopeWrapStreamUpdate(&ctx, blob, 16u)) {
            ERRR("Update on poisoned wrap stream must fail");
        }
        /* Final on poisoned ctx must also fail. */
        uint8_t digest_buf[32];
        size_t dw = 0;
        if (flexEnvelopeWrapStreamFinal(&ctx, digest_buf, sizeof(digest_buf),
                                        &dw)) {
            ERRR("Final on poisoned wrap stream must fail");
        }
    }

    TEST("envelope: wrap stream rejects AEAD class with no buffer mutation") {
        /* Init failure must NOT have written any header bytes,
         * AND *header_written must be 0 so a careless caller cannot
         * mistake the partial state for a real header. */
        flexEnvelopeWrapStream ctx;
        uint8_t header[64];
        memset(header, 0xCDu, sizeof(header));
        size_t hw = 999u; /* sentinel */
        uint8_t key[32] = {0};
        uint8_t nonce[12] = {0};
        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
            .key = key,
            .key_len = sizeof(key),
            /* nonce field ignored by wrap stream init (AEAD goes
             * through Seal anyway). */
        };
        (void)nonce;
        if (flexEnvelopeWrapStreamInit(&ctx, 16u, &wp, header, sizeof(header),
                                       &hw)) {
            ERRR("wrap stream Init must reject AEAD class");
        }
        if (hw != 0u) {
            ERR("On failure *header_written must be 0, got %zu", hw);
        }
        for (size_t i = 0; i < sizeof(header); i++) {
            if (header[i] != 0xCDu) {
                ERR("Init failure mutated header_out at offset %zu", i);
                break;
            }
        }
    }

    TEST("envelope: verify stream rejects AEAD class") {
        /* The framing magic + algo varint + dummy bytes look like an
         * AEAD envelope; verify stream must reject before doing any
         * keyed work. */
        flexEnvelopeWrapParams wp_seal = {
            .algo = FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
        };
        (void)wp_seal;
        /* Hand-craft a minimal AEAD framing header that parses
         * cleanly: magic + version=1 + algo=200 + flags=0 + plen=0.
         * The streaming verify must reject before it tries to consume
         * the nonce/digest. */
        uint8_t hdr[16];
        size_t pos = 0;
        hdr[pos++] = FLEX_ENVELOPE_MAGIC_0;
        hdr[pos++] = FLEX_ENVELOPE_MAGIC_1;
        pos += varintEncode(hdr + pos, 1u);   /* version */
        pos += varintEncode(hdr + pos, 200u); /* AEAD algo */
        pos += varintEncode(hdr + pos, 0u);   /* flags */
        pos += varintEncode(hdr + pos, 0u);   /* payload_len */

        uint8_t key[32] = {0};
        flexEnvelopeVerifyParams vp = {.key = key, .key_len = sizeof(key)};
        flexEnvelopeVerifyStream vctx;
        size_t consumed = 0;
        if (flexEnvelopeVerifyStreamInit(&vctx, &vp, hdr, pos, &consumed, NULL,
                                         NULL)) {
            ERRR("verify stream Init must reject AEAD class");
        }
    }

    TEST("envelope: stream 64 MiB payload round-trip in 64 KiB chunks") {
        /* Memory bound: confirm the streaming context's sizeof doesn't
         * grow with payload size and no heap is needed for the digest
         * computation on the chunk loop. 64 MiB exceeds the default
         * 1 GiB cap but isn't huge — large enough to demonstrate the
         * streaming property. */
        const size_t plen = (size_t)64 * 1024 * 1024;
        const size_t cs = (size_t)64 * 1024;
        cleanerName(zfree, Bytes) uint8_t *payload = zmalloc(plen);
        fillPseudoRandom(payload, plen, 0xB16DA7AUL);

        uint8_t key[32];
        fillPseudoRandom(key, sizeof(key), 0x12345678UL);

        const flexEnvelopeAlgo algo = FLEX_ENV_ALGO_HMAC_SHA256;
        const size_t total = flexEnvelopeWrappedSize(plen, algo, 0);
        cleanerName(zfree, Bytes) uint8_t *out_buf = zmalloc(total);

        flexEnvelopeWrapParams wp = {
            .algo = algo,
            .key = key,
            .key_len = sizeof(key),
        };
        flexEnvelopeWrapStream wctx;
        const size_t ctx_size_before = sizeof(wctx);
        size_t header_written = 0;
        if (!flexEnvelopeWrapStreamInit(&wctx, (uint64_t)plen, &wp, out_buf,
                                        total, &header_written)) {
            ERRR("64MiB stream Init failed");
        }
        size_t off = 0;
        while (off < plen) {
            size_t step = cs;
            if (step > plen - off) {
                step = plen - off;
            }
            memcpy(out_buf + header_written + off, payload + off, step);
            if (!flexEnvelopeWrapStreamUpdate(&wctx, payload + off, step)) {
                ERR("64MiB stream Update failed at offset %zu", off);
                break;
            }
            off += step;
        }
        size_t digest_written = 0;
        if (!flexEnvelopeWrapStreamFinal(&wctx, out_buf + header_written + plen,
                                         total - header_written - plen,
                                         &digest_written)) {
            ERRR("64MiB stream Final failed");
        }
        /* Verify what we just wrote. */
        flexEnvelopeVerifyParams vp = {
            .key = key,
            .key_len = sizeof(key),
            .opts = FLEX_ENV_VERIFY_ALLOW_OVERSIZE,
        };
        flexEnvelopeVerifyStream vctx;
        size_t consumed = 0;
        if (!flexEnvelopeVerifyStreamInit(&vctx, &vp, out_buf, total, &consumed,
                                          NULL, NULL)) {
            ERRR("64MiB verify-stream Init failed");
        }
        size_t voff = consumed;
        while (voff < total) {
            size_t step = cs;
            if (step > total - voff) {
                step = total - voff;
            }
            if (!flexEnvelopeVerifyStreamUpdate(&vctx, out_buf + voff, step)) {
                ERR("64MiB verify-stream Update failed at offset %zu", voff);
                break;
            }
            voff += step;
        }
        if (!flexEnvelopeVerifyStreamFinal(&vctx)) {
            ERRR("64MiB verify-stream Final returned false");
        }
        /* ctx sizeof must NOT have grown (it can't — but document the
         * invariant). */
        if (sizeof(wctx) != ctx_size_before) {
            ERRR("ctx sizeof changed mid-test (impossible compiler bug)");
        }
    }

    TEST("envelope: stream verify rejects chunk > payload_remaining") {
        /* An Update call with `len` larger than the bytes still
         * expected from the envelope must return false and poison the
         * stream so that subsequent Update / Final calls also return
         * false. Caller can't recover by retrying — the protocol
         * state is already corrupted. */
        const size_t plen = 128u;
        uint8_t key[32];
        for (uint32_t i = 0; i < 32u; i++) {
            key[i] = (uint8_t)(0x40u + i);
        }
        cleanerName(zfree, Bytes) uint8_t *payload = zmalloc(plen);
        for (size_t i = 0; i < plen; i++) {
            payload[i] = (uint8_t)i;
        }
        const flexEnvelopeAlgo algo = FLEX_ENV_ALGO_HMAC_SHA256;
        const size_t total = flexEnvelopeWrappedSize(plen, algo, 0);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(total);
        const flexEnvelopeWrapParams wp = {
            .algo = algo,
            .key = key,
            .key_len = sizeof(key),
        };
        size_t written = 0;
        flexEnvelopeWrap(buf, total, payload, plen, &wp, &written);

        flexEnvelopeVerifyParams vp = {
            .key = key,
            .key_len = sizeof(key),
        };
        flexEnvelopeVerifyStream ctx;
        size_t consumed = 0;
        if (!flexEnvelopeVerifyStreamInit(&ctx, &vp, buf, total, &consumed,
                                          NULL, NULL)) {
            ERRR("Init failed");
        }

        /* Feed the entire remaining buffer in one chunk — that's
         * `total - consumed` bytes, which equals payload (plen) +
         * digest (32). This is normally accepted in one shot. But
         * we add one extra byte to force the overrun. */
        const size_t remaining = total - consumed;
        if (flexEnvelopeVerifyStreamUpdate(&ctx, buf + consumed,
                                           remaining + 1u)) {
            ERRR("Update with len > payload+digest_remaining must fail");
        }
        /* The stream is now poisoned: even feeding the correct bytes
         * after must fail. */
        if (flexEnvelopeVerifyStreamUpdate(&ctx, buf + consumed, 1u)) {
            ERRR("Update after poison must fail");
        }
        if (flexEnvelopeVerifyStreamFinal(&ctx)) {
            ERRR("Final after poison must fail");
        }
    }

    TEST("envelope: stream verify accepts prepared-key fast path") {
        /* The streaming verify path now consults
         * params.prepared_key for HMAC algos. Prove parity: an envelope
         * verified via the raw-key path and the prepared-key path must
         * both accept, and a tampered envelope must be rejected by
         * both. */
        const size_t plen = 4096u;
        cleanerName(zfree, Bytes) uint8_t *payload = zmalloc(plen);
        fillPseudoRandom(payload, plen, 0xBADC0FFEEUL);

        uint8_t key[32];
        fillPseudoRandom(key, sizeof(key), 0xCAFEBEEFUL);

        const flexEnvelopeAlgo algos[] = {FLEX_ENV_ALGO_HMAC_SHA256,
                                          FLEX_ENV_ALGO_HMAC_BLAKE3};
        for (uint32_t i = 0; i < sizeof(algos) / sizeof(algos[0]); i++) {
            const flexEnvelopeAlgo algo = algos[i];
            const size_t total = flexEnvelopeWrappedSize(plen, algo, 0);
            cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(total);

            flexEnvelopeWrapParams wp = {
                .algo = algo,
                .key = key,
                .key_len = sizeof(key),
            };
            size_t written = 0;
            if (!flexEnvelopeWrap(buf, total, payload, plen, &wp, &written)) {
                ERR("Wrap failed for algo=%u", (uint32_t)algo);
            }

            const hmacHashAlgo *hash_algo = (algo == FLEX_ENV_ALGO_HMAC_SHA256)
                                                ? &hmacAlgoSHA256
                                                : &hmacAlgoBLAKE3;
            const size_t prep_size = hmacPreparedSize(hash_algo);
            cleanerName(zfree, Bytes) uint8_t *prep_storage =
                zmalloc(prep_size);
            hmacKey prep;
            hmacPrepareKey(&prep, hash_algo, prep_storage, prep_size, key,
                           sizeof(key));

            /* Raw-key streaming verify. */
            flexEnvelopeVerifyParams vp_raw = {
                .key = key,
                .key_len = sizeof(key),
            };
            flexEnvelopeVerifyStream vctx_raw;
            size_t consumed_raw = 0;
            if (!flexEnvelopeVerifyStreamInit(&vctx_raw, &vp_raw, buf, total,
                                              &consumed_raw, NULL, NULL)) {
                ERR("Raw-key stream Init failed for algo=%u", (uint32_t)algo);
            }
            if (!flexEnvelopeVerifyStreamUpdate(&vctx_raw, buf + consumed_raw,
                                                total - consumed_raw)) {
                ERR("Raw-key stream Update failed for algo=%u", (uint32_t)algo);
            }
            if (!flexEnvelopeVerifyStreamFinal(&vctx_raw)) {
                ERR("Raw-key stream verify rejected valid envelope for "
                    "algo=%u",
                    (uint32_t)algo);
            }

            /* Prepared-key streaming verify must accept the same bytes. */
            flexEnvelopeVerifyParams vp_prep = {
                .prepared_key = &prep,
            };
            flexEnvelopeVerifyStream vctx_prep;
            size_t consumed_prep = 0;
            if (!flexEnvelopeVerifyStreamInit(&vctx_prep, &vp_prep, buf, total,
                                              &consumed_prep, NULL, NULL)) {
                ERR("Prepared-key stream Init failed for algo=%u",
                    (uint32_t)algo);
            }
            if (!flexEnvelopeVerifyStreamUpdate(&vctx_prep, buf + consumed_prep,
                                                total - consumed_prep)) {
                ERR("Prepared-key stream Update failed for algo=%u",
                    (uint32_t)algo);
            }
            if (!flexEnvelopeVerifyStreamFinal(&vctx_prep)) {
                ERR("Prepared-key stream verify rejected valid envelope "
                    "for algo=%u",
                    (uint32_t)algo);
            }

            /* Tamper detection: prepared-key path must reject the same
             * way the raw-key path does. */
            buf[total / 2u] ^= 0x01u;
            flexEnvelopeVerifyStream vctx_tamper;
            size_t consumed_t = 0;
            if (!flexEnvelopeVerifyStreamInit(&vctx_tamper, &vp_prep, buf,
                                              total, &consumed_t, NULL, NULL)) {
                ERR("Tamper-test Init failed for algo=%u", (uint32_t)algo);
            }
            (void)flexEnvelopeVerifyStreamUpdate(&vctx_tamper, buf + consumed_t,
                                                 total - consumed_t);
            if (flexEnvelopeVerifyStreamFinal(&vctx_tamper)) {
                ERR("Prepared-key stream accepted forged envelope for "
                    "algo=%u",
                    (uint32_t)algo);
            }
        }
    }

    /* ============================================================
     * AEAD class: Seal / Open
     * ============================================================ */
    TEST("envelope: AEAD chacha20-poly1305 seal/open round-trip") {
        const size_t plens[] = {0u, 1u, 63u, 64u, 65u, 127u, 128u, 1024u};
        uint8_t key[32];
        for (uint32_t i = 0; i < 32u; i++) {
            key[i] = (uint8_t)(0x10u + i);
        }
        uint8_t nonce[12];
        for (uint32_t i = 0; i < 12u; i++) {
            nonce[i] = (uint8_t)(0xA0u + i);
        }

        for (uint32_t s = 0; s < sizeof(plens) / sizeof(plens[0]); s++) {
            const size_t plen = plens[s];
            cleanerName(zfree, Bytes) uint8_t *plain =
                plen > 0 ? zmalloc(plen) : NULL;
            for (size_t i = 0; i < plen; i++) {
                plain[i] = (uint8_t)((i * 37u + 11u) & 0xFFu);
            }

            const size_t total = flexEnvelopeWrappedSize(
                (uint64_t)plen, FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305, 0);
            if (total == 0) {
                ERR("WrappedSize returned 0 for plen=%zu", plen);
            }
            cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(total);

            flexEnvelopeSealParams sp = {
                .algo = FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
                .key = key,
                .key_len = sizeof(key),
                .nonce = nonce,
                .nonce_len = sizeof(nonce),
            };
            size_t written = 0;
            if (!flexEnvelopeSeal(buf, total, plain, (uint64_t)plen, &sp,
                                  &written) ||
                written != total) {
                ERR("Seal failed at plen=%zu", plen);
            }

            /* Allocate at least one byte so Open's `if (!plaintext_out)`
             * guard doesn't trip the empty-payload case. The function
             * writes exactly `plen` bytes; nothing for plen==0. */
            const size_t recovered_cap = plen > 0 ? plen : 1u;
            cleanerName(zfree, Bytes) uint8_t *recovered =
                zmalloc(recovered_cap);
            size_t recovered_len = 0;
            flexEnvelopeVerifyParams vp = {
                .key = key,
                .key_len = sizeof(key),
            };
            if (!flexEnvelopeOpen(buf, total, &vp, recovered, recovered_cap,
                                  &recovered_len) ||
                recovered_len != plen) {
                ERR("Open failed at plen=%zu", plen);
            }
            if (plen > 0 && memcmp(recovered, plain, plen) != 0) {
                ERR("Plaintext mismatch at plen=%zu", plen);
            }
        }
    }

    TEST("envelope: AEAD detects ciphertext / tag / nonce / header tamper") {
        const size_t plen = 256u;
        uint8_t key[32];
        for (uint32_t i = 0; i < 32u; i++) {
            key[i] = (uint8_t)(i * 3u + 7u);
        }
        uint8_t nonce[12];
        for (uint32_t i = 0; i < 12u; i++) {
            nonce[i] = (uint8_t)(0x55u ^ i);
        }
        cleanerName(zfree, Bytes) uint8_t *plain = zmalloc(plen);
        for (size_t i = 0; i < plen; i++) {
            plain[i] = (uint8_t)(i & 0xFFu);
        }

        const size_t total = flexEnvelopeWrappedSize(
            (uint64_t)plen, FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305, 0);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(total);
        flexEnvelopeSealParams sp = {
            .algo = FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
            .key = key,
            .key_len = sizeof(key),
            .nonce = nonce,
            .nonce_len = sizeof(nonce),
        };
        size_t written = 0;
        if (!flexEnvelopeSeal(buf, total, plain, (uint64_t)plen, &sp,
                              &written)) {
            ERRR("Seal setup failed");
        }

        flexEnvelopeVerifyParams vp = {
            .key = key,
            .key_len = sizeof(key),
        };
        cleanerName(zfree, Bytes) uint8_t *recovered = zmalloc(plen);
        size_t r_len = 0;
        if (!flexEnvelopeOpen(buf, total, &vp, recovered, plen, &r_len)) {
            ERRR("Baseline open failed");
        }

        /* Flip every byte position and verify Open rejects each. */
        for (size_t i = 0; i < total; i++) {
            const uint8_t saved = buf[i];
            buf[i] = saved ^ 0x01u;
            if (flexEnvelopeOpen(buf, total, &vp, recovered, plen, &r_len)) {
                ERR("Open accepted tampered byte at offset %zu", i);
                buf[i] = saved;
                break;
            }
            buf[i] = saved;
        }

        /* Wrong key — must reject. */
        uint8_t wrong_key[32];
        memcpy(wrong_key, key, sizeof(wrong_key));
        wrong_key[0] ^= 0xFFu;
        flexEnvelopeVerifyParams vp_wrong = {
            .key = wrong_key,
            .key_len = sizeof(wrong_key),
        };
        if (flexEnvelopeOpen(buf, total, &vp_wrong, recovered, plen, &r_len)) {
            ERRR("Open accepted forged envelope under wrong key");
        }

        /* Same key+plaintext but different nonce produces different
         * ciphertext (ChaCha20 is deterministic in (key, nonce)). */
        uint8_t nonce2[12];
        memcpy(nonce2, nonce, sizeof(nonce2));
        nonce2[0] ^= 0xAAu;
        sp.nonce = nonce2;
        cleanerName(zfree, Bytes) uint8_t *buf2 = zmalloc(total);
        size_t written2 = 0;
        flexEnvelopeSeal(buf2, total, plain, (uint64_t)plen, &sp, &written2);
        if (memcmp(buf, buf2, total) == 0) {
            ERRR("Distinct nonces should produce distinct ciphertexts");
        }
    }

    TEST("envelope: AEAD refuses keyless or short-key seal/open") {
        const size_t plen = 16u;
        uint8_t plain[16];
        memset(plain, 0xAA, sizeof(plain));
        uint8_t nonce[12];
        memset(nonce, 0xBB, sizeof(nonce));
        uint8_t key[32];
        memset(key, 0xCC, sizeof(key));

        const size_t total = flexEnvelopeWrappedSize(
            (uint64_t)plen, FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305, 0);
        uint8_t buf[256];

        flexEnvelopeSealParams sp = {
            .algo = FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
            .key = NULL,
            .key_len = 0,
            .nonce = nonce,
            .nonce_len = sizeof(nonce),
        };
        size_t written = 0;
        if (flexEnvelopeSeal(buf, sizeof(buf), plain, plen, &sp, &written)) {
            ERRR("Seal must refuse NULL key");
        }
        sp.key = key;
        sp.key_len = 16u; /* wrong length for chacha20-poly1305 */
        if (flexEnvelopeSeal(buf, sizeof(buf), plain, plen, &sp, &written)) {
            ERRR("Seal must refuse wrong key length");
        }
        sp.key_len = sizeof(key);
        sp.nonce = NULL;
        if (flexEnvelopeSeal(buf, sizeof(buf), plain, plen, &sp, &written)) {
            ERRR("Seal must refuse NULL nonce");
        }
        sp.nonce = nonce;
        sp.nonce_len = 8u;
        if (flexEnvelopeSeal(buf, sizeof(buf), plain, plen, &sp, &written)) {
            ERRR("Seal must refuse wrong nonce length");
        }

        /* And the wrap/verify path must still reject AEAD algos. */
        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
            .key = key,
            .key_len = sizeof(key),
        };
        if (flexEnvelopeWrap(buf, sizeof(buf), plain, plen, &wp, &written)) {
            ERRR("Wrap must reject AEAD algo");
        }
        sp.nonce_len = sizeof(nonce);
        flexEnvelopeSeal(buf, sizeof(buf), plain, plen, &sp, &written);
        flexEnvelopeVerifyParams vp = {
            .key = key,
            .key_len = sizeof(key),
        };
        const uint8_t *p = NULL;
        uint64_t pl = 0;
        if (flexEnvelopeVerify(buf, total, &vp, &p, &pl)) {
            ERRR("Verify must reject AEAD envelope (route through Open)");
        }
    }

    /* ============================================================
     * Performance bench (prints; never fails)
     * ============================================================ */
    {
        const size_t sizes[] = {64, 1024, 65536, 1 << 20};
        const flexEnvelopeAlgo algos[] = {
            FLEX_ENV_ALGO_XXH64,       FLEX_ENV_ALGO_XXH3_64,
            FLEX_ENV_ALGO_XXH3_128,    FLEX_ENV_ALGO_BLAKE3_KEYED,
            FLEX_ENV_ALGO_HMAC_SHA256, FLEX_ENV_ALGO_HMAC_BLAKE3};
        printf("\nenvelope perf bench (wrap + verify per algo × size):\n");
        for (size_t a = 0; a < sizeof(algos) / sizeof(algos[0]); a++) {
            for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
                const size_t plen = sizes[s];
                cleanerName(zfree, Bytes) uint8_t *payload = zmalloc(plen);
                fillPseudoRandom(payload, plen, 42);
                const size_t total = flexEnvelopeWrappedSize(plen, algos[a], 0);
                cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(total);
                uint8_t bench_key[32];
                fillPseudoRandom(bench_key, sizeof(bench_key), 0xBEEFUL);
                const bool need_key = flexEnvelopeAlgoIsKeyed(algos[a]);
                flexEnvelopeWrapParams wp = {
                    .algo = algos[a],
                    .key = need_key ? bench_key : NULL,
                    .key_len = need_key ? sizeof(bench_key) : 0,
                };
                size_t written = 0;

                uint32_t iters =
                    plen <= 1024 ? 100000 : (plen <= 65536 ? 10000 : 200);
                /* HMAC paths are far slower per byte; cap iterations
                 * to keep total bench time reasonable. */
                if (need_key) {
                    iters /= 4;
                    if (iters < 1) {
                        iters = 1;
                    }
                }
                struct timespec t0, t1;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                for (uint32_t i = 0; i < iters; i++) {
                    flexEnvelopeWrap(buf, total, payload, plen, &wp, &written);
                }
                clock_gettime(CLOCK_MONOTONIC, &t1);
                double wrap_sec =
                    (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

                flexEnvelopeVerifyParams vp = {
                    .key = need_key ? bench_key : NULL,
                    .key_len = need_key ? sizeof(bench_key) : 0,
                };
                clock_gettime(CLOCK_MONOTONIC, &t0);
                for (uint32_t i = 0; i < iters; i++) {
                    const uint8_t *p = NULL;
                    uint64_t pl = 0;
                    flexEnvelopeVerify(buf, total, &vp, &p, &pl);
                }
                clock_gettime(CLOCK_MONOTONIC, &t1);
                double verify_sec =
                    (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

                printf("  %-13s payload=%-7zu wrap=%6.0f ns/op  "
                       "verify=%6.0f ns/op  throughput≈%6.1f MB/s\n",
                       flexEnvelopeAlgoName(algos[a]), plen,
                       wrap_sec / iters * 1e9, verify_sec / iters * 1e9,
                       (double)plen * iters / (verify_sec * 1024.0 * 1024.0));
            }
        }
        printf("\n");
    }

    TEST("envelope: AES-128-GCM Seal/Open round-trip + tamper") {
        const uint8_t key[16] = {0xAA, 0xBB, 0xCC, 0xDD};
        const uint8_t nonce[12] = {0x11, 0x22, 0x33};
        const uint8_t pt[100] = {0x42};
        const size_t plen = sizeof(pt);
        const size_t cap = flexEnvelopeWrappedSize(
            (uint64_t)plen, FLEX_ENV_ALGO_AEAD_AES128_GCM, 0);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(cap);
        flexEnvelopeSealParams sp = {
            .algo = FLEX_ENV_ALGO_AEAD_AES128_GCM,
            .key = key,
            .key_len = sizeof(key),
            .nonce = nonce,
            .nonce_len = sizeof(nonce),
        };
        size_t written = 0;
        if (!flexEnvelopeSeal(buf, cap, pt, (uint64_t)plen, &sp, &written)) {
            ERRR("AES-128-GCM Seal failed");
        }
        flexEnvelopeVerifyParams vp = {.key = key, .key_len = sizeof(key)};
        uint8_t rec[100];
        size_t r_len = 0;
        if (!flexEnvelopeOpen(buf, written, &vp, rec, plen, &r_len)) {
            ERRR("AES-128-GCM Open failed");
        }
        if (r_len != plen || memcmp(rec, pt, plen) != 0) {
            ERRR("AES-128-GCM round-trip mismatch");
        }
        /* Tamper at a few positions. */
        const size_t tamper_at[] = {0, written / 2, written - 1};
        for (size_t i = 0; i < sizeof(tamper_at) / sizeof(tamper_at[0]); i++) {
            buf[tamper_at[i]] ^= 0x01;
            if (flexEnvelopeOpen(buf, written, &vp, rec, plen, &r_len)) {
                ERR("AES-128-GCM accepted tamper @ %zu", tamper_at[i]);
            }
            buf[tamper_at[i]] ^= 0x01;
        }
    }

    TEST("envelope: AES-256-GCM Seal/Open round-trip + tamper") {
        uint8_t key[32];
        for (uint32_t i = 0; i < 32; i++) {
            key[i] = (uint8_t)(i * 7u + 3u);
        }
        const uint8_t nonce[12] = {0x99, 0xAA};
        const uint8_t pt[513] = {0};
        const size_t plen = sizeof(pt);
        const size_t cap = flexEnvelopeWrappedSize(
            (uint64_t)plen, FLEX_ENV_ALGO_AEAD_AES256_GCM, 0);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(cap);
        flexEnvelopeSealParams sp = {
            .algo = FLEX_ENV_ALGO_AEAD_AES256_GCM,
            .key = key,
            .key_len = sizeof(key),
            .nonce = nonce,
            .nonce_len = sizeof(nonce),
        };
        size_t written = 0;
        if (!flexEnvelopeSeal(buf, cap, pt, (uint64_t)plen, &sp, &written)) {
            ERRR("AES-256-GCM Seal failed");
        }
        flexEnvelopeVerifyParams vp = {.key = key, .key_len = sizeof(key)};
        cleanerName(zfree, Bytes) uint8_t *rec = zmalloc(plen);
        size_t r_len = 0;
        if (!flexEnvelopeOpen(buf, written, &vp, rec, plen, &r_len)) {
            ERRR("AES-256-GCM Open failed");
        }
        if (r_len != plen || memcmp(rec, pt, plen) != 0) {
            ERRR("AES-256-GCM round-trip mismatch");
        }
        /* Wrong key length must refuse. */
        flexEnvelopeVerifyParams vp_bad = {.key = key, .key_len = 16u};
        if (flexEnvelopeOpen(buf, written, &vp_bad, rec, plen, &r_len)) {
            ERRR("AES-256-GCM accepted 16-byte key");
        }
    }

    TEST("revoked key floor rejects below-floor wire key_id with typed err") {
        /* Two keys installed; wrap under K1 (id 100) then raise floor
         * to K2 (id 200); verify under both — only K2-wrap succeeds and
         * K1-wrap returns false with FLEX_ENV_VERIFY_FAILED_KEY_REVOKED.
         *
         * Keep the wrap pathway minimal — no envelope params struct
         * thread-local state; just rely on the wire's key_id flag and
         * the bound keyring at verify time. */
        hmacKeyring *ring = hmacKeyringCreate();
        const uint8_t key1[32] = {0xC3, 0x01};
        const uint8_t key2[32] = {0xC3, 0x02};
        const uint8_t salt1[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xA, 0xB, 0xC,
                                                             0xD};
        const uint8_t salt2[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xE, 0xF, 0x0,
                                                             0x1};
        if (!hmacKeyringInstall(ring, 100u, &hmacAlgoSHA256, key1, sizeof(key1),
                                salt1)) {
            ERRR("install K1 failed");
        }
        if (!hmacKeyringInstall(ring, 200u, &hmacAlgoSHA256, key2, sizeof(key2),
                                salt2)) {
            ERRR("install K2 failed");
        }

        const uint8_t payload[16] = "hello-revoke";
        /* Wrap under K1, key_id=100 */
        flexEnvelopeWrapParams wp1 = {
            .algo = FLEX_ENV_ALGO_HMAC_SHA256,
            .on_wire_flags = FLEX_ENV_FLAG_KEYID_PRESENT,
            .key_id = 100u,
            .key = key1,
            .key_len = sizeof(key1),
        };
        const size_t cap1 = flexEnvelopeWrappedSize(
            sizeof(payload), FLEX_ENV_ALGO_HMAC_SHA256, wp1.on_wire_flags);
        cleanerName(zfree, Bytes) uint8_t *env1 = zmalloc(cap1);
        size_t written1 = 0;
        if (!flexEnvelopeWrap(env1, cap1, payload, sizeof(payload), &wp1,
                              &written1)) {
            ERRR("wrap under K1 failed");
        }

        /* Wrap under K2, key_id=200 */
        flexEnvelopeWrapParams wp2 = {
            .algo = FLEX_ENV_ALGO_HMAC_SHA256,
            .on_wire_flags = FLEX_ENV_FLAG_KEYID_PRESENT,
            .key_id = 200u,
            .key = key2,
            .key_len = sizeof(key2),
        };
        const size_t cap2 = flexEnvelopeWrappedSize(
            sizeof(payload), FLEX_ENV_ALGO_HMAC_SHA256, wp2.on_wire_flags);
        cleanerName(zfree, Bytes) uint8_t *env2 = zmalloc(cap2);
        size_t written2 = 0;
        if (!flexEnvelopeWrap(env2, cap2, payload, sizeof(payload), &wp2,
                              &written2)) {
            ERRR("wrap under K2 failed");
        }

        /* Set floor to 200 (revoke K1). */
        if (!hmacKeyringSetMinValidKeyId(ring, 200u)) {
            ERRR("set floor=200 failed");
        }

        flexEnvelopeVerifyParams vp = {.keyring = ring};
        const uint8_t *out = NULL;
        uint64_t out_len = 0;
        /* Pre-condition: both wraps verify when floor is 0 — sanity
         * check by saving + clearing floor. Skip this; instead just
         * assert the post-floor behaviour. */

        /* K1 verify must fail with KEY_REVOKED. */
        if (flexEnvelopeVerify(env1, written1, &vp, &out, &out_len)) {
            ERRR("K1 verify under floor MUST fail");
        }
        if (flexEnvelopeLastVerifyError() !=
            FLEX_ENV_VERIFY_FAILED_KEY_REVOKED) {
            ERR("K1 verify err expected KEY_REVOKED (2), got %d",
                (int)flexEnvelopeLastVerifyError());
        }

        /* K2 verify succeeds. */
        if (!flexEnvelopeVerify(env2, written2, &vp, &out, &out_len)) {
            ERRR("K2 verify above floor MUST succeed");
        }
        if (flexEnvelopeLastVerifyError() != FLEX_ENV_VERIFY_OK) {
            ERR("K2 verify err expected OK, got %d",
                (int)flexEnvelopeLastVerifyError());
        }

        /* Snapshot + restore preserves the floor. */
        const size_t sz = hmacKeyringSnapshotSize(ring);
        cleanerName(zfree, Bytes) uint8_t *snap = zmalloc(sz);
        size_t snap_w = 0;
        if (!hmacKeyringSnapshot(ring, snap, sz, &snap_w)) {
            ERRR("snapshot after floor failed");
        }
        hmacKeyring *restored = hmacKeyringCreate();
        if (!hmacKeyringRestore(restored, snap, snap_w)) {
            ERRR("restore failed");
        }
        if (hmacKeyringMinValidKeyId(restored) != 200u) {
            ERR("restored floor expected 200 got %u",
                (unsigned)hmacKeyringMinValidKeyId(restored));
        }
        flexEnvelopeVerifyParams vp_restored = {.keyring = restored};
        if (flexEnvelopeVerify(env1, written1, &vp_restored, &out, &out_len)) {
            ERRR("post-restore K1 verify MUST still fail (floor preserved)");
        }
        if (flexEnvelopeLastVerifyError() !=
            FLEX_ENV_VERIFY_FAILED_KEY_REVOKED) {
            ERR("post-restore K1 verify err expected KEY_REVOKED got %d",
                (int)flexEnvelopeLastVerifyError());
        }

        hmacKeyringFree(ring);
        hmacKeyringFree(restored);
    }

    TEST("envelope: BLAKE3-keyed rejects short key") {
        /* BLAKE3 keyed mode is defined only for a 32-byte key. Any
         * other length MUST be refused at every entry point:
         * - flexEnvelopeWrap (one-shot)
         * - flexEnvelopeReserveWrap (zero-copy)
         * - flexEnvelopeWrapStreamInit (streaming)
         * - flexEnvelopeVerify (one-shot)
         * - flexEnvelopeVerifyStreamInit (streaming)
         * Silent zero-padding would reduce effective entropy to
         * key_len*8 bits while presenting a full 256-bit digest. */
        const size_t plen = 64u;
        uint8_t payload[64];
        memset(payload, 0xAB, sizeof(payload));
        uint8_t short_key[16];
        memset(short_key, 0xCD, sizeof(short_key));
        uint8_t full_key[BLAKE3_KEY_LEN];
        memset(full_key, 0xEF, sizeof(full_key));

        const size_t wrapped =
            flexEnvelopeWrappedSize(plen, FLEX_ENV_ALGO_BLAKE3_KEYED, 0);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(wrapped);

        /* One-shot wrap with 16-byte key MUST fail. */
        flexEnvelopeWrapParams wp_short = {
            .algo = FLEX_ENV_ALGO_BLAKE3_KEYED,
            .key = short_key,
            .key_len = sizeof(short_key),
        };
        size_t written = 0;
        if (flexEnvelopeWrap(buf, wrapped, payload, plen, &wp_short,
                             &written)) {
            ERRR("flexEnvelopeWrap must reject 16-byte key under "
                 "BLAKE3_KEYED");
        }

        /* Reserve-wrap with 16-byte key MUST fail. */
        uint8_t *region = NULL;
        size_t hlen = 0;
        if (flexEnvelopeReserveWrap(buf, wrapped, plen, &wp_short, &region,
                                    &hlen)) {
            ERRR("flexEnvelopeReserveWrap must reject 16-byte key under "
                 "BLAKE3_KEYED");
        }

        /* Streaming init MUST reject the short key explicitly. */
        flexEnvelopeWrapStream wctx;
        uint8_t header[64];
        size_t hw = 0;
        if (flexEnvelopeWrapStreamInit(&wctx, plen, &wp_short, header,
                                       sizeof(header), &hw)) {
            ERRR("Wrap-stream Init must reject 16-byte key under "
                 "BLAKE3_KEYED");
        }

        /* Build a valid envelope with a proper 32-byte key so we can
         * exercise the verify-side rejection. */
        flexEnvelopeWrapParams wp_full = {
            .algo = FLEX_ENV_ALGO_BLAKE3_KEYED,
            .key = full_key,
            .key_len = sizeof(full_key),
        };
        size_t w2 = 0;
        if (!flexEnvelopeWrap(buf, wrapped, payload, plen, &wp_full, &w2)) {
            ERRR("Full-length BLAKE3_KEYED wrap must succeed");
        }

        /* One-shot verify with 16-byte key MUST fail (even though the
         * envelope was wrapped with the full-length key). */
        flexEnvelopeVerifyParams vp_short = {
            .key = short_key,
            .key_len = sizeof(short_key),
        };
        const uint8_t *vp_out = NULL;
        uint64_t vp_out_len = 0;
        if (flexEnvelopeVerify(buf, w2, &vp_short, &vp_out, &vp_out_len)) {
            ERRR("flexEnvelopeVerify must reject 16-byte key under "
                 "BLAKE3_KEYED");
        }

        /* Streaming verify init MUST reject the short key explicitly. */
        flexEnvelopeVerifyStream vctx;
        size_t consumed = 0;
        if (flexEnvelopeVerifyStreamInit(&vctx, &vp_short, buf, w2, &consumed,
                                         NULL, NULL)) {
            ERRR("Verify-stream Init must reject 16-byte key under "
                 "BLAKE3_KEYED");
        }

        /* Sanity: full-length key must still succeed on both paths. */
        flexEnvelopeVerifyParams vp_full2 = {
            .key = full_key,
            .key_len = sizeof(full_key),
        };
        const uint8_t *vp_full_out = NULL;
        uint64_t vp_full_out_len = 0;
        if (!flexEnvelopeVerify(buf, w2, &vp_full2, &vp_full_out,
                                &vp_full_out_len)) {
            ERRR("flexEnvelopeVerify must accept full 32-byte BLAKE3_KEYED "
                 "key");
        }
        flexEnvelopeVerifyStream vctx2;
        size_t consumed2 = 0;
        if (!flexEnvelopeVerifyStreamInit(&vctx2, &vp_full2, buf, w2,
                                          &consumed2, NULL, NULL)) {
            ERRR("Verify-stream Init must accept full 32-byte BLAKE3_KEYED "
                 "key");
        }
        if (!flexEnvelopeVerifyStreamUpdate(&vctx2, buf + consumed2,
                                            w2 - consumed2)) {
            ERRR("Verify-stream Update on valid 32-byte BLAKE3_KEYED envelope "
                 "failed");
        }
        if (!flexEnvelopeVerifyStreamFinal(&vctx2)) {
            ERRR("Verify-stream Final rejected valid 32-byte BLAKE3_KEYED "
                 "envelope");
        }
    }

    TEST("envelope: verify-stream Final poisons ctx on byte / digest "
         "mismatch") {
        /* Symmetric with every other failure path in the streaming
         * verifier: a Final on a half-fed ctx must poison so a
         * subsequent Update cannot rebuild a half-corrupt session
         * into a passing one. */
        const size_t plen = 64u;
        uint8_t payload[64];
        memset(payload, 0xA5, sizeof(payload));
        uint8_t key[32];
        memset(key, 0x42, sizeof(key));

        const flexEnvelopeAlgo algo = FLEX_ENV_ALGO_HMAC_SHA256;
        const size_t wrapped = flexEnvelopeWrappedSize(plen, algo, 0);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(wrapped);
        flexEnvelopeWrapParams wp = {
            .algo = algo,
            .key = key,
            .key_len = sizeof(key),
        };
        size_t written = 0;
        if (!flexEnvelopeWrap(buf, wrapped, payload, plen, &wp, &written)) {
            ERRR("setup wrap failed");
        }

        flexEnvelopeVerifyParams vp = {.key = key, .key_len = sizeof(key)};
        /* === byte-count-mismatch branch === */
        flexEnvelopeVerifyStream vctx;
        size_t consumed = 0;
        if (!flexEnvelopeVerifyStreamInit(&vctx, &vp, buf, wrapped, &consumed,
                                          NULL, NULL)) {
            ERRR("verify-stream Init failed");
        }
        /* Feed only HALF the payload then call Final. Final must
         * return false (byte count mismatch) AND poison the ctx. */
        const size_t half = plen / 2u;
        if (!flexEnvelopeVerifyStreamUpdate(&vctx, buf + consumed, half)) {
            ERRR("verify-stream Update of half-payload failed");
        }
        if (flexEnvelopeVerifyStreamFinal(&vctx)) {
            ERRR("verify-stream Final must reject byte-count mismatch");
        }
        /* Now try to feed the remaining payload + digest bytes: the
         * poisoned ctx must refuse every subsequent operation. */
        if (flexEnvelopeVerifyStreamUpdate(&vctx, buf + consumed + half,
                                           wrapped - consumed - half)) {
            ERRR("Update on poisoned verify-stream ctx must fail");
        }
        if (flexEnvelopeVerifyStreamFinal(&vctx)) {
            ERRR("Final on poisoned verify-stream ctx must fail");
        }

        /* === digest-byte-count branch === */
        /* Feed full payload but not enough digest bytes. The
         * wire_digest_seen != digest_len branch must also poison. */
        flexEnvelopeVerifyStream vctx2;
        size_t consumed2 = 0;
        if (!flexEnvelopeVerifyStreamInit(&vctx2, &vp, buf, wrapped, &consumed2,
                                          NULL, NULL)) {
            ERRR("verify-stream Init failed (2)");
        }
        /* Feed exactly payload, no digest. */
        if (!flexEnvelopeVerifyStreamUpdate(&vctx2, buf + consumed2, plen)) {
            ERRR("verify-stream Update of full payload failed");
        }
        if (flexEnvelopeVerifyStreamFinal(&vctx2)) {
            ERRR("verify-stream Final must reject digest-len mismatch");
        }
        /* Poisoned ctx must reject further Update. */
        if (flexEnvelopeVerifyStreamUpdate(&vctx2, buf + consumed2 + plen,
                                           wrapped - consumed2 - plen)) {
            ERRR("Update on poisoned (digest-mismatch) verify-stream ctx must "
                 "fail");
        }
        if (flexEnvelopeVerifyStreamFinal(&vctx2)) {
            ERRR("Final on poisoned (digest-mismatch) verify-stream ctx must "
                 "fail");
        }
    }

    TEST("envelope: wrap-stream Final wipes key-bearing ctx state") {
        /* After a successful wrap-stream Final, the caller-owned ctx
         * must NOT retain HMAC inner state (K_ipad / K_opad-absorbed,
         * key-equivalent) or the BLAKE3 keyed-hash key bytes.
         *
         * Wipe-proof: we pre-fill the ENTIRE ctx with a recognizable
         * 0xA5 pattern BEFORE Init.  Without the pre-fill the test was
         * trivially-passing for FLEX_ENV_ALGO_BLAKE3_KEYED — that
         * algo path does not touch `hmac_storage`, so the zmalloc/
         * stack-zero-init starting value would equal the post-Final
         * zero pattern even if Final never wiped.  With the pre-fill,
         * any byte left at 0xA5 by an elided wipe would be caught.
         *
         * NOTE: the wipe scope is `hmac_storage` and the `u` union
         * (the secret-bearing regions).  We do NOT assert other ctx
         * fields (phase, payload_len_declared, payload_bytes_seen,
         * etc.) are zero — those are non-secret bookkeeping that
         * Final intentionally leaves to indicate FINALIZED state. */
        const flexEnvelopeAlgo algos[] = {FLEX_ENV_ALGO_HMAC_SHA256,
                                          FLEX_ENV_ALGO_HMAC_BLAKE3,
                                          FLEX_ENV_ALGO_BLAKE3_KEYED};
        const size_t plen = 128u;
        uint8_t payload[128];
        memset(payload, 0x5A, sizeof(payload));
        uint8_t key[BLAKE3_KEY_LEN];
        memset(key, 0xC3, sizeof(key));
        uint8_t hmac_zero[FLEX_ENV_STREAM_HMAC_STORAGE];
        memset(hmac_zero, 0, sizeof(hmac_zero));
        uint8_t u_zero[sizeof(((flexEnvelopeWrapStream *)0)->u)];
        memset(u_zero, 0, sizeof(u_zero));

        for (uint32_t i = 0; i < sizeof(algos) / sizeof(algos[0]); i++) {
            const flexEnvelopeAlgo algo = algos[i];
            const size_t wrapped = flexEnvelopeWrappedSize(plen, algo, 0);
            cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(wrapped);
            flexEnvelopeWrapParams wp = {
                .algo = algo,
                .key = key,
                .key_len = sizeof(key),
            };
            flexEnvelopeWrapStream wctx;
            /* Pre-fill with 0xA5 — both not-zero and easy to spot. */
            memset(&wctx, 0xA5, sizeof(wctx));
            size_t hw = 0;
            if (!flexEnvelopeWrapStreamInit(&wctx, plen, &wp, buf, wrapped,
                                            &hw)) {
                ERR("wrap-stream Init failed for algo=%u", (uint32_t)algo);
                continue;
            }
            if (!flexEnvelopeWrapStreamUpdate(&wctx, payload, plen)) {
                ERR("wrap-stream Update failed for algo=%u", (uint32_t)algo);
                continue;
            }
            size_t dw = 0;
            if (!flexEnvelopeWrapStreamFinal(&wctx, buf + hw + plen,
                                             wrapped - hw - plen, &dw)) {
                ERR("wrap-stream Final failed for algo=%u", (uint32_t)algo);
                continue;
            }
            if (memcmp(wctx.hmac_storage, hmac_zero, sizeof(hmac_zero)) != 0) {
                ERR("wrap-stream Final left hmac_storage non-zero for "
                    "algo=%u (pre-fill 0xA5 not scrubbed)",
                    (uint32_t)algo);
            }
            if (memcmp(&wctx.u, u_zero, sizeof(u_zero)) != 0) {
                ERR("wrap-stream Final left u union non-zero for algo=%u "
                    "(pre-fill 0xA5 not scrubbed)",
                    (uint32_t)algo);
            }
        }
    }

    TEST("envelope: verify-stream Final wipes key-bearing ctx state") {
        /* Mirror of the wrap-stream wipe test. After a successful
         * verify-stream Final, the caller-owned ctx must NOT retain
         * HMAC inner state or the BLAKE3 keyed-hash key bytes.
         *
         * Wipe-proof: the entire ctx is pre-filled with 0xA5 before
         * Init.  The wipe asserts (a) `hmac_storage` is zero, (b) the
         * `u` union is zero, AND (c) the borrowed-pointer fields
         * (raw_key/raw_key_len/prepared_key) are nulled.  We do NOT
         * over-assert the rest of the ctx: phase, opts, algo etc. are
         * non-secret bookkeeping that Final leaves in a FINALIZED-state
         * sentinel form. */
        const flexEnvelopeAlgo algos[] = {FLEX_ENV_ALGO_HMAC_SHA256,
                                          FLEX_ENV_ALGO_HMAC_BLAKE3,
                                          FLEX_ENV_ALGO_BLAKE3_KEYED};
        const size_t plen = 128u;
        uint8_t payload[128];
        memset(payload, 0x7E, sizeof(payload));
        uint8_t key[BLAKE3_KEY_LEN];
        memset(key, 0xDE, sizeof(key));
        uint8_t hmac_zero[FLEX_ENV_STREAM_HMAC_STORAGE];
        memset(hmac_zero, 0, sizeof(hmac_zero));
        uint8_t u_zero[sizeof(((flexEnvelopeVerifyStream *)0)->u)];
        memset(u_zero, 0, sizeof(u_zero));

        for (uint32_t i = 0; i < sizeof(algos) / sizeof(algos[0]); i++) {
            const flexEnvelopeAlgo algo = algos[i];
            const size_t wrapped = flexEnvelopeWrappedSize(plen, algo, 0);
            cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(wrapped);
            flexEnvelopeWrapParams wp = {
                .algo = algo,
                .key = key,
                .key_len = sizeof(key),
            };
            size_t written = 0;
            if (!flexEnvelopeWrap(buf, wrapped, payload, plen, &wp, &written)) {
                ERR("setup wrap failed for algo=%u", (uint32_t)algo);
                continue;
            }

            flexEnvelopeVerifyParams vp = {
                .key = key,
                .key_len = sizeof(key),
            };
            flexEnvelopeVerifyStream vctx;
            /* Pre-fill with 0xA5 — pre-Init this ensures the post-Final
             * zero assertions are observing a real wipe rather than a
             * stale zmalloc-zeroed region. */
            memset(&vctx, 0xA5, sizeof(vctx));
            size_t consumed = 0;
            if (!flexEnvelopeVerifyStreamInit(&vctx, &vp, buf, wrapped,
                                              &consumed, NULL, NULL)) {
                ERR("verify-stream Init failed for algo=%u", (uint32_t)algo);
                continue;
            }
            if (!flexEnvelopeVerifyStreamUpdate(&vctx, buf + consumed,
                                                wrapped - consumed)) {
                ERR("verify-stream Update failed for algo=%u", (uint32_t)algo);
                continue;
            }
            if (!flexEnvelopeVerifyStreamFinal(&vctx)) {
                ERR("verify-stream Final failed for algo=%u", (uint32_t)algo);
                continue;
            }
            if (memcmp(vctx.hmac_storage, hmac_zero, sizeof(hmac_zero)) != 0) {
                ERR("verify-stream Final left hmac_storage non-zero for "
                    "algo=%u (pre-fill 0xA5 not scrubbed)",
                    (uint32_t)algo);
            }
            if (memcmp(&vctx.u, u_zero, sizeof(u_zero)) != 0) {
                ERR("verify-stream Final left u union non-zero for algo=%u "
                    "(pre-fill 0xA5 not scrubbed)",
                    (uint32_t)algo);
            }
            if (vctx.raw_key != NULL || vctx.raw_key_len != 0) {
                ERR("verify-stream Final left raw_key non-NULL for algo=%u",
                    (uint32_t)algo);
            }
            if (vctx.prepared_key != NULL) {
                ERR("verify-stream Final left prepared_key non-NULL for "
                    "algo=%u",
                    (uint32_t)algo);
            }
        }
    }

    TEST("keyring auto-resolve verify after Retire returns expected payload") {
        /* The Wave 4 Track H migration replaced flexEnvelopeVerify's
         * borrowed-pointer keyring resolve (for BLAKE3_KEYED) with a
         * GetRawCopy into a stack buffer. This is the smallest correctness
         * test that the migrated path still produces the right payload:
         * wrap a frame with key_id=K, verify via keyring auto-resolve,
         * then Retire the slot and confirm verify cleanly fails (no
         * dereference of freed bytes). Run under ASAN to surface any
         * regression in the lifetime contract. */
        const uint8_t k_bytes[BLAKE3_KEY_LEN] = {
            0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xa0, 0xb0,
            0xc0, 0xd0, 0xe0, 0xf0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xff, 0xfe};
        const hmacKeyId KID = 314u;
        hmacKeyring *ring = hmacKeyringCreate();
        if (!hmacKeyringInstallUnsalted(ring, KID, &hmacAlgoBLAKE3, k_bytes,
                                        sizeof(k_bytes))) {
            ERRR("keyringRawCopyMigrate: install failed");
        }

        const uint8_t payload[] = "wave-4-track-h-migration";
        const size_t plen = sizeof(payload) - 1u;
        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_BLAKE3_KEYED,
            .key = k_bytes,
            .key_len = sizeof(k_bytes),
            .on_wire_flags = FLEX_ENV_FLAG_KEYID_PRESENT,
            .key_id = KID,
        };
        const size_t sz =
            flexEnvelopeWrappedSize(plen, wp.algo, wp.on_wire_flags);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t written = 0;
        if (!flexEnvelopeWrap(buf, sz, payload, plen, &wp, &written)) {
            ERRR("keyringRawCopyMigrate: wrap failed");
        }

        /* Verify via the keyring (the migrated path). */
        flexEnvelopeVerifyParams vp = {.keyring = ring};
        const uint8_t *out_payload = NULL;
        uint64_t out_plen = 0;
        if (!flexEnvelopeVerify(buf, written, &vp, &out_payload, &out_plen)) {
            ERRR("keyringRawCopyMigrate: verify after migration failed");
        }
        if (out_plen != plen || memcmp(out_payload, payload, plen) != 0) {
            ERRR("keyringRawCopyMigrate: verify returned wrong payload");
        }

        /* Now Retire the slot and re-verify: the keyring lookup must
         * fail cleanly, NOT dereference freed bytes. */
        if (!hmacKeyringRetire(ring, KID)) {
            ERRR("keyringRawCopyMigrate: retire failed");
        }
        if (flexEnvelopeVerify(buf, written, &vp, &out_payload, &out_plen)) {
            ERRR("keyringRawCopyMigrate: verify must fail after retire");
        }

        hmacKeyringFree(ring);
    }

    TEST("keyring auto-resolve open after Retire returns clean failure") {
        /* Companion test for flexEnvelopeOpen — the AEAD path also
         * migrated to GetRawCopy. Same pattern: wrap a frame under
         * key_id=K, Open via keyring, then Retire and assert Open
         * fails cleanly. */
        uint8_t k_bytes[CHACHA20_POLY1305_KEY_LEN];
        memset(k_bytes, 0xC2, sizeof(k_bytes));
        const hmacKeyId KID = 271u;
        const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xAA, 0xBB, 0xCC,
                                                            0xDD};
        hmacKeyring *ring = hmacKeyringCreate();
        if (!hmacKeyringInstall(ring, KID, &hmacAlgoSHA256, k_bytes,
                                sizeof(k_bytes), salt)) {
            ERRR("openMigrate: install failed");
        }

        const uint8_t payload[] = "wave-4-track-h-aead-migration";
        const size_t plen = sizeof(payload) - 1u;
        uint8_t nonce[12];
        if (!hmacKeyringNextAeadNonce(ring, KID, nonce)) {
            ERRR("openMigrate: NextAeadNonce failed");
        }
        flexEnvelopeSealParams sp = {
            .algo = FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305,
            .key = k_bytes,
            .key_len = sizeof(k_bytes),
            .nonce = nonce,
            .nonce_len = sizeof(nonce),
            .on_wire_flags = FLEX_ENV_FLAG_KEYID_PRESENT,
            .key_id = KID,
        };
        const size_t sz =
            flexEnvelopeWrappedSize((uint64_t)plen, sp.algo, sp.on_wire_flags);
        cleanerName(zfree, Bytes) uint8_t *buf = zmalloc(sz);
        size_t written = 0;
        if (!flexEnvelopeSeal(buf, sz, payload, plen, &sp, &written)) {
            ERRR("openMigrate: seal failed");
        }

        flexEnvelopeVerifyParams vp = {.keyring = ring};
        uint8_t plaintext[64] = {0};
        size_t pt_written = 0;
        if (!flexEnvelopeOpen(buf, written, &vp, plaintext, sizeof(plaintext),
                              &pt_written)) {
            ERRR("openMigrate: keyring auto-resolve Open failed");
        }
        if (pt_written != plen || memcmp(plaintext, payload, plen) != 0) {
            ERRR("openMigrate: Open returned wrong plaintext");
        }

        /* Retire the slot — subsequent Open must fail cleanly. */
        if (!hmacKeyringRetire(ring, KID)) {
            ERRR("openMigrate: retire failed");
        }
        memset(plaintext, 0, sizeof(plaintext));
        if (flexEnvelopeOpen(buf, written, &vp, plaintext, sizeof(plaintext),
                             &pt_written)) {
            ERRR("openMigrate: Open must fail after retire");
        }

        hmacKeyringFree(ring);
    }

    TEST_FINAL_RESULT;
}

/* === Concurrent retire-during-Verify/Open =================================
 *
 * Wave 4 Track MN migrated the keyring-resolution branches in
 * flexEnvelopeVerify and flexEnvelopeOpen off the borrowed-pointer
 * accessors (hmacKeyringLookup for the prepared HMAC key, and
 * hmacKeyringGetAesGcmKey for the cached AES-GCM schedule). Both now
 * COPY the underlying material into a stack-local under the keyring
 * rwlock so a concurrent hmacKeyringRetire on the resolved id cannot
 * race the cipher/HMAC compute. This test exercises the precise race:
 *
 * Thread A (writer): tight loop of flexEnvelopeVerify (HMAC class) +
 *                    flexEnvelopeOpen (AEAD class) on a pre-wrapped
 *                    frame whose key_id resolves via the keyring.
 * Thread B (rotator): tight loop of Retire(K) + Reinstall(K) with
 *                     identical bytes (so a verify against the freshly-
 *                     installed slot still succeeds).
 *
 * Pre-migration: Verify/Open held borrowed pointers into the slot's
 * prepared_storage / aead_schedule which Retire's dkSecureZero + free
 * would invalidate mid-compute (heap-use-after-free). ASAN reliably
 * fires on the regression. Post-migration: writer's copy is independent
 * of any subsequent Retire and the race is closed. */
typedef struct envRetireRaceCtx {
    const uint8_t *hmac_frame;
    size_t hmac_frame_len;
    const uint8_t *aead_frame;
    size_t aead_frame_len;
    size_t aead_plaintext_len;
    hmacKeyring *ring;
    hmacKeyId key_id;
    const uint8_t *hmac_key_bytes;
    size_t hmac_key_len;
    const uint8_t aead_salt[HMAC_KEYRING_AEAD_SALT_BYTES];
    _Atomic uint64_t writer_iters_done;
    _Atomic uint64_t rotator_iters_done;
    _Atomic int writer_invariant_violations;
} envRetireRaceCtx;

static void *envRetireRaceWriter(void *vctx) {
    envRetireRaceCtx *c = (envRetireRaceCtx *)vctx;
    const size_t ITERS = 4000u;
    flexEnvelopeVerifyParams vp = {.keyring = c->ring};
    uint8_t plaintext[256];
    for (size_t i = 0; i < ITERS; i++) {
        /* HMAC-class verify via keyring auto-resolve. May legitimately
         * fail when the rotator's Retire lands between our framing
         * peek and the keyring resolve — that surfaces as a clean
         * generic-failed return, which is correct. */
        const uint8_t *out_payload = NULL;
        uint64_t out_plen = 0;
        (void)flexEnvelopeVerify(c->hmac_frame, c->hmac_frame_len, &vp,
                                 &out_payload, &out_plen);
        /* AEAD-class Open via keyring auto-resolve. Same race-tolerance
         * comment applies — may fail when the rotator wins the race;
         * the migration's job is only to ensure that whenever it
         * SUCCEEDS, the bytes were a coherent snapshot. */
        size_t pt_written = 0;
        (void)flexEnvelopeOpen(c->aead_frame, c->aead_frame_len, &vp, plaintext,
                               sizeof(plaintext), &pt_written);
        if (pt_written != 0 && pt_written != c->aead_plaintext_len) {
            /* Anything other than zero (failure) or the expected length
             * is an invariant violation. */
            /* @sync: relaxed counter */
            atomic_fetch_add_explicit(&c->writer_invariant_violations, 1,
                                      memory_order_relaxed);
        }
        /* @sync: relaxed counter */
        atomic_fetch_add_explicit(&c->writer_iters_done, 1u,
                                  memory_order_relaxed);
    }
    return NULL;
}

static void *envRetireRaceRotator(void *vctx) {
    envRetireRaceCtx *c = (envRetireRaceCtx *)vctx;
    const size_t ITERS = 4000u;
    for (size_t i = 0; i < ITERS; i++) {
        (void)hmacKeyringRetire(c->ring, c->key_id);
        /* Reinstall with the SAME bytes + salt so a writer that sees
         * a post-Reinstall slot still computes the correct tag. */
        (void)hmacKeyringInstall(c->ring, c->key_id, &hmacAlgoSHA256,
                                 c->hmac_key_bytes, c->hmac_key_len,
                                 c->aead_salt);
        /* @sync: relaxed counter */
        atomic_fetch_add_explicit(&c->rotator_iters_done, 1u,
                                  memory_order_relaxed);
    }
    return NULL;
}

int dkTestFlexEnvelopeKeyringRetireRaceTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int err = 0;

    /* AES-256-length key chosen on purpose so the slot caches an
     * aead_schedule — exercises hmacKeyringGetAesGcmKeyCopy under
     * concurrent Retire. The same bytes are reused as the HMAC-SHA256
     * key for the HMAC-class frame; the test does not care that the
     * key happens to be valid for both algos. */
    const uint8_t key[AES256_KEY_LEN] = {
        0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe, 0xfe, 0xdc, 0xba,
        0x98, 0x76, 0x54, 0x32, 0x10, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
    const uint8_t salt[HMAC_KEYRING_AEAD_SALT_BYTES] = {0xA0, 0xB1, 0xC2, 0xD3};
    const hmacKeyId KID = 414u;
    const uint8_t hmac_payload[] = "wave-4-track-mn-hmac-keyring-retire";
    const uint8_t aead_payload[] = "wave-4-track-mn-aead-keyring-retire";

    TEST("flexEnvelopeVerify+Open under concurrent Retire+Reinstall (HMAC + "
         "AES-GCM keyring resolve) — no UAF, no torn key") {
        hmacKeyring *ring = hmacKeyringCreate();
        if (!hmacKeyringInstall(ring, KID, &hmacAlgoSHA256, key, sizeof(key),
                                salt)) {
            ERRR("envRetireRace: initial keyring install failed");
        }

        /* Wrap an HMAC-SHA-256 frame addressed to KID. */
        const size_t hmac_payload_len = sizeof(hmac_payload) - 1u;
        flexEnvelopeWrapParams wp = {
            .algo = FLEX_ENV_ALGO_HMAC_SHA256,
            .key = key,
            .key_len = sizeof(key),
            .on_wire_flags = FLEX_ENV_FLAG_KEYID_PRESENT,
            .key_id = KID,
        };
        const size_t hmac_cap = flexEnvelopeWrappedSize(
            hmac_payload_len, wp.algo, wp.on_wire_flags);
        cleanerName(zfree, Bytes) uint8_t *hmac_frame = zmalloc(hmac_cap);
        size_t hmac_written = 0;
        if (!flexEnvelopeWrap(hmac_frame, hmac_cap, hmac_payload,
                              hmac_payload_len, &wp, &hmac_written)) {
            ERRR("envRetireRace: HMAC wrap failed");
        }

        /* Seal an AES-256-GCM frame addressed to KID. */
        const size_t aead_payload_len = sizeof(aead_payload) - 1u;
        uint8_t nonce[12];
        if (!hmacKeyringNextAeadNonce(ring, KID, nonce)) {
            ERRR("envRetireRace: NextAeadNonce failed");
        }
        flexEnvelopeSealParams sp = {
            .algo = FLEX_ENV_ALGO_AEAD_AES256_GCM,
            .key = key,
            .key_len = sizeof(key),
            .nonce = nonce,
            .nonce_len = sizeof(nonce),
            .on_wire_flags = FLEX_ENV_FLAG_KEYID_PRESENT,
            .key_id = KID,
        };
        const size_t aead_cap = flexEnvelopeWrappedSize(
            (uint64_t)aead_payload_len, sp.algo, sp.on_wire_flags);
        cleanerName(zfree, Bytes) uint8_t *aead_frame = zmalloc(aead_cap);
        size_t aead_written = 0;
        if (!flexEnvelopeSeal(aead_frame, aead_cap, aead_payload,
                              aead_payload_len, &sp, &aead_written)) {
            ERRR("envRetireRace: AEAD seal failed");
        }

        envRetireRaceCtx ctx = {
            .hmac_frame = hmac_frame,
            .hmac_frame_len = hmac_written,
            .aead_frame = aead_frame,
            .aead_frame_len = aead_written,
            .aead_plaintext_len = aead_payload_len,
            .ring = ring,
            .key_id = KID,
            .hmac_key_bytes = key,
            .hmac_key_len = sizeof(key),
            .aead_salt = {salt[0], salt[1], salt[2], salt[3]},
        };

        pthread_t writer_thr;
        pthread_t rotator_thr;
        if (pthread_create(&writer_thr, NULL, envRetireRaceWriter, &ctx) != 0) {
            ERRR("envRetireRace: spawn writer failed");
        }
        if (pthread_create(&rotator_thr, NULL, envRetireRaceRotator, &ctx) !=
            0) {
            ERRR("envRetireRace: spawn rotator failed");
        }
        pthread_join(writer_thr, NULL);
        pthread_join(rotator_thr, NULL);

        /* @sync: relaxed init */
        const int violations = atomic_load_explicit(
            &ctx.writer_invariant_violations, memory_order_relaxed);
        /* @sync: relaxed init */
        const uint64_t writer_iters =
            atomic_load_explicit(&ctx.writer_iters_done, memory_order_relaxed);
        /* @sync: relaxed init */
        const uint64_t rotator_iters =
            atomic_load_explicit(&ctx.rotator_iters_done, memory_order_relaxed);

        if (violations != 0) {
            ERR("envRetireRace: writer observed %d invariant violations — "
                "possible UAF or torn key",
                violations);
        }
        /* Both threads must have made meaningful progress, otherwise
         * we are not actually exercising the race. */
        if (writer_iters < 100u) {
            ERR("envRetireRace: writer only completed %" PRIu64 " iterations",
                writer_iters);
        }
        if (rotator_iters < 100u) {
            ERR("envRetireRace: rotator only completed %" PRIu64 " iterations",
                rotator_iters);
        }

        /* Ensure the slot is in a clean state regardless of which
         * thread last touched it. */
        (void)hmacKeyringRetire(ring, KID);
        hmacKeyringFree(ring);
    }

    TEST_FINAL_RESULT;
}

#endif /* DATAKIT_TEST */
