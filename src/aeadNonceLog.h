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
 * hash set with FIFO eviction. Calls outside the window are
 * forgotten — callers should rotate the key well before that.
 * - No false negatives within the window: a reuse IS detected as
 * long as both observations land in the live window.
 * - Fail-safe on collision overflow: if the probe chain saturates we
 * return false (treat as reuse). That biases the caller toward key
 * rotation rather than silent acceptance.
 *
 * Thread-safety: one mutex guards the whole structure. Contended
 * workloads can shard by creating N logs and partitioning by key_id.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct aeadNonceLog aeadNonceLog;

/* AEAD nonce width covered by this module — sized for ChaCha20-Poly1305
 * and AES-GCM (both 12 bytes). */
#define AEAD_NONCE_LOG_NONCE_LEN ((size_t)12)

/* Construct a tracker with at least `min_capacity` slots. Internally
 * rounded up to the next power of two; minimum effective capacity is
 * 64. Returns NULL on allocation failure. */
aeadNonceLog *aeadNonceLogCreate(size_t min_capacity);

/* Tear down the tracker. Safe to call with NULL. Memory is wiped on
 * free so a leaked log does not leak observed nonces. */
void aeadNonceLogFree(aeadNonceLog *log);

/* Observe a (key_id, nonce) tuple.
 * - Returns true if the tuple was inserted (not seen in window).
 * - Returns false if the tuple is a duplicate of one already in the
 * window — caller MUST NOT proceed with Seal.
 * - Returns false if the probe chain saturates (treat as reuse).
 *
 * `key_id` must be non-zero; passing zero returns false (reserved). */
bool aeadNonceLogObserve(aeadNonceLog *log, uint32_t key_id,
                         const uint8_t nonce[AEAD_NONCE_LOG_NONCE_LEN]);

/* Remove every entry whose key_id matches. Call on key retire so the
 * window doesn't keep memory of a retired key. */
void aeadNonceLogClearKey(aeadNonceLog *log, uint32_t key_id);

/* Current number of distinct entries tracked. */
size_t aeadNonceLogSize(const aeadNonceLog *log);

/* Capacity (post round-up). */
size_t aeadNonceLogCapacity(const aeadNonceLog *log);

/* ---- Persistence (flex-based) ------------------------------------------
 *
 * Wire format (a single flex):
 * Entry 0: version (unsigned, currently 1)
 * Entry 1: capacity (unsigned, post-round-up cap from create)
 * Entry 2: record_count (unsigned, N — number of live (key_id, nonce, gen)
 * tuples following)
 * Entry 3..3+4N-1: per-record block of 4 entries:
 * (a) key_id (unsigned 32-bit)
 * (b) nonce_lo (unsigned 64-bit; first 8 bytes of nonce, little-endian)
 * (c) nonce_hi (unsigned 32-bit; last 4 bytes of nonce, little-endian)
 * (d) gen (unsigned 64-bit; monotonic insertion sequence)
 *
 * total flex entry count is always 3 + 4*record_count. */

#define AEAD_NONCE_LOG_PERSIST_VERSION ((uint64_t)1)

/* Serialize the log into `out`. If `out == NULL` returns the required
 * buffer size and does not modify anything. Otherwise writes at most
 * `out_cap` bytes and returns the number actually written, or 0 on
 * failure (NULL log, out too small, etc.). */
size_t aeadNonceLogToBytes(const aeadNonceLog *log, uint8_t *out,
                           size_t out_cap);

/* Parse a previously-serialized log. Returns a freshly-allocated
 * aeadNonceLog populated with the same capacity and record set; caller
 * frees via aeadNonceLogFree. Returns NULL on parse/validation failure,
 * including any field that is not a 64-bit unsigned integer (a
 * 96/128-bit entry is refused). */
aeadNonceLog *aeadNonceLogFromBytes(const uint8_t *buf, size_t len);

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
