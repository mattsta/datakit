#pragma once

/* aeadNonceLog — defense-in-depth detector for (key_id, nonce) reuse.
 *
 * Even with hmacKeyring's monotonic AEAD counter, real systems
 * can reuse a nonce by:
 * - restarting without restoring the counter from durable state,
 * - rolling back the counter from a stale checkpoint,
 * - mixing keyring-issued and externally-supplied nonces under the
 * same key,
 * - cloning a VM/snapshotting and resuming both sides.
 *
 * For ChaCha20-Poly1305 a single (key, nonce) repeat destroys BOTH
 * confidentiality and integrity. This module gives callers an opt-in
 * tracker that fails an attempted Seal BEFORE the keystream is derived
 * if the (key_id, nonce) tuple has already been observed in the recent
 * window.
 *
 * Trade-offs (intentional):
 * - Bounded memory. The window is a fixed-capacity open-addressing
 * hash set holding at most 7/8 of its capacity; past that the oldest
 * observation is evicted (FIFO). Calls outside the window are
 * forgotten — callers should rotate the key well before that.
 * - No false negatives within the window: a reuse IS detected as
 * long as both observations land in the live window (removing an
 * entry never hides another from lookup).
 *
 * Thread-safety: one mutex guards the whole structure. Contended
 * workloads can shard by creating N logs and partitioning by key_id.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "hmac.h" /* hmacKeyId */

/* @handle: fixed: its address holds from creation to free */
typedef struct aeadNonceLog aeadNonceLog;

/* AEAD nonce width covered by this module — sized for ChaCha20-Poly1305
 * and AES-GCM (both 12 bytes). */
#define AEAD_NONCE_LOG_NONCE_LEN ((size_t)12)

/* Largest capacity a tracker can have (2^24 slots, about 600 MB) */
#define AEAD_NONCE_LOG_MAX_CAP ((size_t)1 << 24u)

/* Construct a tracker with at least `min_capacity` slots. Internally
 * rounded up to the next power of two; minimum effective capacity is
 * 64. Returns NULL on allocation failure or when min_capacity exceeds
 * AEAD_NONCE_LOG_MAX_CAP. */
[[nodiscard]] aeadNonceLog *aeadNonceLogCreate(size_t min_capacity);

/* Tear down the tracker. Safe to call with NULL. Memory is wiped on
 * free so a leaked log does not leak observed nonces. */
void aeadNonceLogFree(aeadNonceLog *log);

/* Observe a (key_id, nonce) tuple.
 * - Returns true if the tuple was inserted (not seen in window).
 * - Returns false if the tuple is a duplicate of one already in the
 * window — caller MUST NOT proceed with Seal.
 *
 * `key_id` must not be HMAC_KEY_ID_INVALID (which returns false). */
bool aeadNonceLogObserve(aeadNonceLog *log, hmacKeyId key_id,
                         const uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN]);
/* aeadNonceLogObserve of the key `key_id` held at `generation` (the
 * keyring's hmacKeyringGetGeneration; aeadNonceLogObserve is generation
 * 0): the key a rotation installs under the same id is another key, and
 * its nonces are not reuses of the old key's */
bool aeadNonceLogObserveAt(aeadNonceLog *log, hmacKeyId key_id,
                           uint64_t generation,
                           const uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN]);

/* Remove every entry whose key_id matches, at every generation. Call on
 * key retire so the window doesn't keep memory of a retired key. */
void aeadNonceLogClearKey(aeadNonceLog *log, hmacKeyId key_id);

/* Current number of distinct entries tracked. */
size_t aeadNonceLogSize(const aeadNonceLog *log);

/* Capacity (post round-up). */
size_t aeadNonceLogCapacity(const aeadNonceLog *log);

/* ---- Persistence (flex-based) ------------------------------------------
 *
 * Wire format (a single flex):
 * Entry 0: capacity (unsigned, post-round-up cap from create)
 * Then one 3-entry record per tracked tuple, oldest first:
 * (a) nonce (12 bytes)
 * (b) key_id (unsigned, 1..UINT32_MAX)
 * (c) generation (unsigned)
 *
 * The flex entry count is 1 + 3N for N tuples. */

/* Serialize the log into `out`. If `out == NULL` returns the required
 * buffer size and does not modify anything. Otherwise writes at most
 * `out_cap` bytes and returns the number actually written, or 0 on
 * failure (NULL log, out too small, etc.). */
size_t aeadNonceLogToBytes(const aeadNonceLog *log, uint8_t *out,
                           size_t out_cap);

/* Parse a previously-serialized log. Returns a freshly-allocated
 * aeadNonceLog populated with the same capacity, tuples and eviction
 * order; caller frees via aeadNonceLogFree. Every entry of the buffer is
 * bounded with flexValidateBufferDeep before it is read. Returns NULL on
 * parse/validation failure: a malformed entry anywhere in the buffer, a
 * nonce that is not 12 bytes, a key_id or capacity that is not a 64-bit
 * unsigned integer in range (a 96/128-bit entry is refused), a capacity
 * that is not a power of two in [64, AEAD_NONCE_LOG_MAX_CAP], more
 * tuples than 7/8 of the capacity, or a repeated tuple. */
[[nodiscard]] aeadNonceLog *aeadNonceLogFromBytes(const uint8_t *buf,
                                                  size_t len);

/* persistOps descriptor for use by the persist framework
 * (PERSIST_TYPE_AEAD_NONCE_LOG = 93). */
struct persistOps;
extern const struct persistOps persistOpsAeadNonceLog;

#ifdef DATAKIT_TEST
int aeadNonceLogTest(int argc, char *argv[]);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
