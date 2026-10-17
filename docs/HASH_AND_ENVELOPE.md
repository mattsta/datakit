# Hashes, HMAC, and the flex envelope — usage guide

Day-to-day reference for callers of datakit's hashing, keyed-MAC, and
integrity-envelope APIs.

## What's in the box

| Module            | Public header        | Purpose                                                                                                                      |
| ----------------- | -------------------- | ---------------------------------------------------------------------------------------------------------------------------- |
| BLAKE3            | `blake3.h`           | Modern cryptographic hash; default for new code. Plain / keyed / derive-key modes.                                           |
| SHA-256           | `sha256.h`           | FIPS 180-4 SHA-256. Use when interop demands it.                                                                             |
| HMAC              | `hmac.h`             | RFC 2104 HMAC over any hash; ships with SHA-256 and BLAKE3 backends.                                                         |
| HMAC keyring      | `hmacKeyring.h`      | Caller-owned registry of prepared MAC keys, indexed by 32-bit id. Rotation-friendly.                                         |
| ChaCha20-Poly1305 | `chacha20poly1305.h` | Portable RFC 8439 AEAD (encrypt + authenticate in one pass).                                                                 |
| flex envelope     | `flexEnvelope.h`     | Integrity wrapper around any byte buffer. Supports checksum, MAC, and AEAD classes; one-shot, streaming, and seal/open APIs. |
| Cluster mailbox   | `clusterMailbox.h`   | End-to-end verified-frame router with per-origin replay protection and durable watermarks.                                   |

The hash modules each pick the fastest available implementation at
runtime — ARM crypto extensions, NEON, AVX2, etc. Callers don't need
to know about variants; they just call the public API. See
**[Backends and runtime SIMD dispatch](#backends-and-runtime-simd-dispatch)**
below for the dispatch table, introspection helpers, and force-override
hooks (test-only).

### Related headers

- `src/flexEnvelope.h` — wire format for the envelope itself,
  including the outer-header vs. inner-payload framing contract
  (documented at the top of the header).
- `src/persist.h` — how persist binds a keyring and the
  `key_id`-tagged snapshot/WAL header.
- `src/aeadNonceLog.h` — defense-in-depth nonce observer's
  architecture, persist format, and eviction policy.

## When to use which

| Need                                                                   | Use                                                                                                 |
| ---------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------- |
| Fast unkeyed digest, no adversary                                      | BLAKE3 (`blake3Hash`)                                                                               |
| Wider collision margin for unkeyed digest                              | BLAKE3 128-bit prefix (XOF) or xxh3_128 envelope                                                    |
| Keyed digest, modern primitive                                         | BLAKE3 keyed-hash (`blake3HashKeyed`)                                                               |
| Keyed digest, interop with HMAC-expecting tools                        | HMAC-SHA-256 (`hmacCompute(&hmacAlgoSHA256, ...)`)                                                  |
| Wrap a byte payload (flex, RPC body, replication frame) with integrity | flex envelope (`flexEnvelopeWrap` / `flexEnvelopeVerify`)                                           |
| Many small HMAC verifies with the same key                             | Prepared-key HMAC (`hmacPrepareKey` + `hmacComputeWithKey`)                                         |
| Several MAC keys with rotation, looked up by id                        | HMAC keyring (`hmacKeyringInstall` + `hmacKeyringLookup`)                                           |
| Encrypt AND authenticate a payload                                     | AEAD envelope (`flexEnvelopeSeal` / `flexEnvelopeOpen` with `FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305`) |
| Wrap > 1 GiB payload without buffering                                 | Streaming envelope (`flexEnvelopeWrapStream*` / `flexEnvelopeVerifyStream*`)                        |
| Receive verified frames from a peer with replay protection             | Cluster mailbox (`clusterMailboxIngest` + an apply callback)                                        |
| Survive restart without losing replay protection                       | Mailbox snapshot/restore (`clusterMailboxSnapshot` + `clusterMailboxRestore`)                       |

## Backends and runtime SIMD dispatch

Every hash and AEAD primitive in this module ships at least a portable
scalar reference and one or more vectorized backends; selection is
runtime, per-process, and based on CPUID / `getauxval(AT_HWCAP)` /
`sysctl hw.optional.*` probes done lazily on first use. Callers never
need to know which backend ran — but for tests, perf triage, and
deterministic fuzz runs the dispatcher exposes a uniform pair of
introspect + force-override hooks (see "test-only override" below).

| Algorithm                                 | Backends (preferred → fallback)                                 | Source files                                                               | Introspect                        | Force-override (test)       |
| ----------------------------------------- | --------------------------------------------------------------- | -------------------------------------------------------------------------- | --------------------------------- | --------------------------- |
| BLAKE3 (single-block + multi-chunk batch) | AVX2 (8-way) → SSE4.1 (4-way) → NEON (4-way) → scalar           | `hash/blake3AVX2.c`, `hash/blake3SSE41.c`, `hash/blake3Neon.c`, `blake3.c` | `blake3DispatcherActiveBackend()` | `blake3SetForceImpl()`      |
| SHA-256                                   | SHA-NI (x86) → ARMv8-Crypto → scalar                            | `hash/sha256ShaNi.c`, `hash/sha256ArmCrypto.c`, `sha256.c`                 | `sha256ActiveBackend()`           | `sha256SetForceBackend()`   |
| AES-GCM                                   | AES-NI + PCLMUL (x86-64) → ARMv8-AES + PMULL (AArch64) → scalar | `hash/aesGcmNi.c`, `hash/aesGcmArmCrypto.c`, `aesGcm.c`                    | `aesGcmActiveBackend()`           | `aesGcmSetForceBackend()`   |
| ChaCha20 (keystream)                      | AVX2 (8-block) → NEON (4-block) → scalar                        | `hash/chacha20AVX2.c`, `hash/chacha20Neon.c`, `chacha20poly1305.c`         | `chacha20ActiveBackend()`         | `chacha20SetForceBackend()` |
| Poly1305 (one-shot + streaming Update)    | AVX2 (4-block parallel r²/r³/r⁴) → NEON (2-block) → scalar      | `hash/poly1305AVX2.c`, `hash/poly1305Neon.c`, `chacha20poly1305.c`         | `poly1305ActiveBackend()`         | `poly1305SetForceBackend()` |

All backends for one primitive are bit-identical on every input — the
parity tests in `datakit-test` enumerate the table and assert
scalar-vs-SIMD agreement on RFC test vectors plus 100K random inputs
per pair. When you add a new backend, do the same: register it,
then write the parity test that runs against the dispatched and the
forced-scalar paths.

### BLAKE3 multi-chunk dispatch (B1/B2/B3)

BLAKE3's chunk-tree parallelism gets the largest SIMD win on
medium-and-up payloads. The dispatcher in `blake3.c` chooses
`process_8_chunks_simd_avx2` (8 KiB minimum input) on AVX2 hosts,
`process_4_chunks_simd` (4 KiB minimum) on NEON or SSE4.1 hosts, and
falls back to scalar single-chunk processing for smaller inputs. The
per-backend chunk batch widths are pinned at compile time via
`_Static_assert` (`BLAKE3_BATCH_AVX2_CHUNKS == 8`, `_NEON_CHUNKS ==
4`, `_SSE41_CHUNKS == 4`) so a backend that drifts out of sync with
its compressor's lane count fails the build, not at runtime.

### Test-only override

Each `*SetForceBackend` (or `blake3SetForceImpl`) accepts a sentinel
to revert to auto-detect. Use them only from tests; in production
threads the default auto-detect produces the fastest correct backend
and the override is process-global (it'd impact unrelated threads).
A typical parity test pattern:

```c
/* Pin scalar, hash, capture digest. */
chacha20SetForceBackend(CHACHA20_FORCE_SCALAR);
poly1305SetForceBackend(POLY1305_FORCE_SCALAR);
chacha20poly1305Seal(..., out_scalar, tag_scalar);

/* Pin SIMD, hash same input, compare. */
chacha20SetForceBackend(CHACHA20_FORCE_NEON);   /* or _AVX2 */
poly1305SetForceBackend(POLY1305_FORCE_NEON);
chacha20poly1305Seal(..., out_simd, tag_simd);

assert(memcmp(out_scalar, out_simd, len) == 0);
assert(memcmp(tag_scalar, tag_simd, 16) == 0);

/* Restore auto-detect for the rest of the process. */
chacha20SetForceBackend(CHACHA20_FORCE_AUTO);
poly1305SetForceBackend(POLY1305_FORCE_AUTO);
```

### Throughput reference numbers

Order-of-magnitude figures on Apple M-series (single-thread, hot
cache, 4 KiB payload, dev preset). Hardware-specific numbers will
vary; the perfRegression harness captures the canonical baselines.

| Primitive                | Backend                         | Throughput (approx.)     | Notes                                                      |
| ------------------------ | ------------------------------- | ------------------------ | ---------------------------------------------------------- |
| ChaCha20-Poly1305 (Seal) | scalar                          | ~150 MB/s                | portable-C baseline                                        |
| ChaCha20-Poly1305 (Seal) | NEON 4-block + Poly1305 2-block | ~202 MB/s                | M1/M2 with NEON dispatch active                            |
| AES-GCM (Seal/Open)      | scalar                          | ~80 MB/s                 | pure-C ref; cache-timing-vulnerable, avoid in prod         |
| AES-GCM (Seal/Open)      | ARMv8-AES + PMULL               | multi-GB/s               | M-series with hardware-AES dispatch; ≥ 30× scalar baseline |
| AES-GCM (Seal/Open)      | AES-NI + PCLMUL                 | multi-GB/s               | x86-64 with AES-NI dispatch; ≥ 30× scalar baseline         |
| BLAKE3 (≥ 8 KiB)         | AVX2 8-way                      | ≥ 2× scalar x86 baseline | dispatch active for inputs ≥ 8 chunks (8 KiB)              |
| SHA-256                  | SHA-NI or ARMv8-Crypto          | hardware-bounded         | preferred over BLAKE3 where hosts have the ext.            |

Use `./build-perf/src/datakit-test test perfRegression` +
`cd tools && uv run dktools perfdiff BASELINE.tsv CURRENT.tsv` (against a
locally kept, uncommitted baseline TSV) for the live numbers on your hardware
— see `docs/HASH_PERFORMANCE.md` for the per-algorithm baseline
spreadsheet and the regression bar (5 %).

## Secure-wipe rule — `dkSecureZero` is the only correct way to clear secrets

End-of-life clearing of key material, MAC keys, nonces still-in-flight,
AEAD `keystream_buf` remnants, and Poly1305 / SHA-256 intermediate
state **MUST** go through `dkSecureZero(ptr, n)` from `ctmem.h`.

```c
#include "ctmem.h"

uint8_t round_keys[176];
aesKeySchedule(round_keys, key);
/* ... use round_keys ... */
dkSecureZero(round_keys, sizeof(round_keys));   /* correct */
```

Plain `memset(round_keys, 0, sizeof(round_keys))` is **forbidden** on
end-of-life secret material in this codebase: under `-O3 -flto` the
compiler is allowed to prove that the buffer is dead after the
`memset` and eliminate the writes entirely (this is a documented
hazard, not a theoretical one; see CVE-2017-14159 for the canonical
case). `dkSecureZero` uses a volatile-pointer byte loop followed by
an inline-asm `"memory"` barrier so the writes survive every
optimization level we ship.

Plain `memset(..., 0, ...)` is still fine for **initialising** a
fresh struct before any secret has been written into it; the rule
applies only to end-of-life clearing.

Every crypto module ships compliant end-of-life wipes today:

- `hmacKeyring.c` — keyring slot retire / free (`hmacKeyringRetire`,
  `hmacKeyringFree`) clear the prepared inner+outer SHA states and
  the key bytes themselves.
- `chacha20poly1305.c` — `SealStreamFinal` / `OpenStreamFinal` wipe
  the entire `chacha20poly1305StreamCtx` (key, nonce, keystream
  remainder, Poly1305 state) once the tag has been emitted. See
  **[Streaming AEAD state machine](#streaming-aead-state-machine)** below.
- `hmac.c`, `sha256.c` — Final paths wipe intermediate state.
- `aesGcm.c`, `aesGcmNi.c`, `aesGcmArmCrypto.c` — round-key schedules
  and GHASH precomputes are wiped on context teardown.

If you grep new code and find `memset(..., 0, ...)` on a buffer that
just held a secret, replace it with `dkSecureZero` and add a test
that hex-dumps the buffer immediately after end-of-life and asserts
it is all-zero — the `dkTestChaChaStreamCtxWipe` test pattern.

## Streaming AEAD state machine

The streaming ChaCha20-Poly1305 API
(`chacha20poly1305SealStream{Init,Aad,Update,Final}` and the matching
`OpenStream*`) has three contracts callers MUST honor — they are
enforced fail-closed in the implementation, so violating them is a
runtime `false` return, not undefined behavior.

1. **Phase ordering.** `Init → (Aad)* → (Update)* → Final`. Calling
   `Aad` after the first `Update` returns `false`. Calling `Update`
   after `Final` returns `false` (the ctx has been wiped — see #3).
2. **Final is single-shot.** A second call to
   `SealStreamFinal` / `OpenStreamFinal` returns `false` and emits no
   output. This contract is exercised by
   `dkTestStreamFinalTwice`.
3. **Post-Final ctx is bytewise zero.** Once `Final` succeeds (and
   even when it fails because of a verify mismatch on Open), the
   entire `chacha20poly1305StreamCtx` is `dkSecureZero`-cleared
   before return. The key, the nonce, up to 64 bytes of unused
   keystream, and the Poly1305 accumulator are all gone from the
   stack frame. Pin this contract by
   `memcmp`-against-zero in `dkTestChaChaStreamCtxWipe`.
4. **Mid-stream tamper.** On Open, a single bit flipped anywhere in
   the ciphertext or AAD makes `Final` return `false`. Mid-stream
   tamper is **not** detected by `Update` (Poly1305 is an XOR-MAC
   accumulator with no incremental verifier) — only by `Final`.

The state-machine guarantees live in the `phase` field of the ctx
struct (`chacha20poly1305StreamPhase`); read the contracts at the
top of `chacha20poly1305.h` for the canonical statement.

## X25519 small-order pre-reject

The default `x25519(out_shared, priv, peer_public)` performs a
constant-time pre-check against the canonical 12-element small-order
table (RFC 7748 §6.1 + curve25519-dalek's documented set) BEFORE
running the Montgomery ladder. Any peer public key that matches a
small-order point — including all-zero, the curve identity, and the
ten non-trivial small-order generators — causes `x25519` to return
`false` and leave `out_shared` untouched.

The pre-reject is constant-time (`dkCtMemEq` over the table) so it
does not leak whether the peer was small-order vs. just a verify
failure. The all-zero output post-multiply check from earlier
revisions is retained as belt-and-braces: even if a new small-order
point were discovered, the post-multiply check still catches it.

Treat `x25519` returning `false` as authoritative: the peer is either
malicious (sending a small-order key to force the shared secret to a
known value), malformed, or generated under a defective RNG. Never
proceed with the ECDH derivation, even with a different KDF context.

## BLAKE3 — basic usage

### One-shot hash

```c
#include "blake3.h"

uint8_t digest[BLAKE3_OUT_LEN];   /* 32 bytes */
blake3Hash(input, input_len, digest);
```

### Keyed hash (BLAKE3's native MAC mode)

```c
uint8_t key[BLAKE3_KEY_LEN];      /* exactly 32 bytes */
/* ... populate key from your keystore ... */

uint8_t digest[BLAKE3_OUT_LEN];
blake3HashKeyed(key, input, input_len, digest);
```

### Incremental hashing

```c
blake3Hasher h;
blake3HasherInit(&h);                  /* or InitKeyed(&h, key) */
blake3HasherUpdate(&h, chunk1, chunk1_len);
blake3HasherUpdate(&h, chunk2, chunk2_len);
/* ... */
uint8_t digest[BLAKE3_OUT_LEN];
blake3HasherFinalize(&h, digest, BLAKE3_OUT_LEN);
```

`blake3HasherFinalizeSeek` produces output starting at any byte offset
into the (conceptually infinite) BLAKE3 XOF stream — useful for
deriving multiple independent keys from one root.

## SHA-256 — basic usage

```c
#include "sha256.h"

uint8_t digest[SHA256_DIGEST_LEN]; /* 32 bytes */
sha256Hash(input, input_len, digest);

/* Or incremental: */
sha256Ctx ctx;
sha256Init(&ctx);
sha256Update(&ctx, data, data_len);
sha256Final(&ctx, digest);
```

## HMAC — basic usage

The HMAC module is hash-agnostic. Pick a backend with one of the
shipped `hmacHashAlgo` descriptors:

```c
#include "hmac.h"

uint8_t digest[32];
hmacCompute(&hmacAlgoSHA256,
            key, key_len,
            message, message_len,
            digest);

/* Or HMAC over BLAKE3 (RFC 2104 framing — distinct from BLAKE3's
 * native keyed mode): */
hmacCompute(&hmacAlgoBLAKE3,
            key, key_len,
            message, message_len,
            digest);
```

### Prepared-key HMAC for high-throughput verifies

When you'll verify many messages with the same key (replication
ingest, cluster mailbox, RPC framing), prepare the key once and reuse
the absorbed inner/outer state:

```c
/* One-time setup per key */
uint8_t prep_storage[1024];          /* at least hmacPreparedSize(algo) */
hmacKey prep;
hmacPrepareKey(&prep, &hmacAlgoSHA256,
               prep_storage, sizeof(prep_storage),
               key, key_len);

/* Per-message verify (saves 2 block compressions vs hmacCompute) */
uint8_t digest[32];
hmacComputeWithKey(&prep, message, message_len, digest);
```

The prepared key contains no allocations and is safe to share across
threads as long as nothing mutates it after `hmacPrepareKey` returns.

### HMAC keyring — multiple keys, rotation-friendly

When a service runs with more than one active MAC key — e.g. the
current generation plus the previous generation during a rotation
window, or a small set of per-tenant keys — use the keyring instead of
managing prepared keys by hand:

```c
#include "hmacKeyring.h"

hmacKeyring *ring = hmacKeyringCreate();

/* Install keys by caller-chosen id (1..2^32-1; 0 is reserved).  The
 * 4-byte aead_salt is REQUIRED in production — see "Keyring lifecycle
 * contracts" below for why. */
uint8_t salt1[HMAC_KEYRING_AEAD_SALT_BYTES] = { /* per-key/per-process */ };
hmacKeyringInstall(ring, /* id */ 1, &hmacAlgoSHA256, gen1_key, gen1_len, salt1);

uint8_t salt2[HMAC_KEYRING_AEAD_SALT_BYTES] = { /* distinct */ };
hmacKeyringInstall(ring, /* id */ 2, &hmacAlgoSHA256, gen2_key, gen2_len, salt2);

/* Hot path: look up a prepared key by id and use it directly. */
const hmacKey *prep = hmacKeyringLookup(ring, current_id);
if (prep) {
    uint8_t digest[32];
    hmacComputeWithKey(prep, message, message_len, digest);
}

/* Rotation: install the new generation, then retire the old one.
 * Lookups for the retired id immediately return NULL; existing
 * pointers obtained before retire MUST NOT be used afterward. */
uint8_t salt3[HMAC_KEYRING_AEAD_SALT_BYTES] = { /* distinct */ };
hmacKeyringInstall(ring, 3, &hmacAlgoSHA256, gen3_key, gen3_len, salt3);
hmacKeyringRetire(ring, 1);

hmacKeyringFree(ring);     /* zero-cleans every stored key */
```

The keyring is thread-safe: concurrent `hmacKeyringLookup` calls are
unsynchronized between readers, while `Install` / `Retire` serialize
through an internal rwlock. Capacity is 16 active keys per ring;
re-install after `Retire` reclaims the slot.

The bytes you pass to `Install` are copied into the keyring and zeroed
on `Retire` / `Free` (volatile-pointer wipe), so it is safe to wipe
your own buffer immediately after install.

### Keyring lifecycle contracts

The canonical contract block lives at the top of `hmacKeyring.h` under
"Lifecycle contracts (post-rotation hardening)" — read it once before
wiring a keyring into a long-lived subsystem. Quick reference:

1. **Salted vs. unsalted install.** `hmacKeyringInstall(..., aead_salt)`
   is the production path; the 4-byte salt becomes the suffix of every
   12-byte AEAD nonce and closes the cross-process nonce-collision
   footgun (two processes installing the same key both start their
   counter at zero — without a distinct salt they emit colliding
   nonces). `hmacKeyringInstallUnsalted(...)` is the explicitly-named
   legacy variant for single-process tests; `hmacKeyringNextAeadNonce`
   refuses to issue zero-salt nonces unless the slot was installed via
   the unsalted entry point.
2. **Generation API for attach-style consumers.** When a long-lived
   subsystem (e.g. `persistAttachKeyring`) binds to a `(ring, key_id)`
   pair, it MUST cache `hmacKeyringGetGeneration(ring, id)` alongside.
   Every subsequent consume re-checks the generation; mismatch means
   "the key was rotated since you attached" — a cached raw-key pointer
   would be a use-after-free. The post-rotation recovery entry point
   is `persistRebindKeyring`. Typed failure codes
   (`PERSIST_ERR_KEY_GENERATION_MISMATCH`, `_KEY_NOT_FOUND`,
   `_NO_KEY_ATTACHED`) flow through `persistLastError()`.
3. **Revoked-key floor.**
   `hmacKeyringSetMinValidKeyId(ring, floor)` sets a per-keyring
   monotonically-non-decreasing floor — backward moves are refused.
   `flexEnvelopeVerify` consults the floor via the wire-key-id auto-
   resolution path and rejects below-floor ids with
   `FLEX_ENV_VERIFY_FAILED_KEY_REVOKED` (read via
   `flexEnvelopeLastVerifyError()`). The floor survives
   snapshot/restore via a leading sentinel record.
4. **Persist header `key_id`.** Both snapshot and WAL
   headers carry the attached `key_id`. A cross-key mistake on read
   fails fast with `PERSIST_ERR_KEY_ID_MISMATCH` instead of the opaque
   generic verify-failed; the expected id is recoverable via
   `persistLastExpectedKeyId(p)` so operator tooling can rebind to the
   correct key.
5. **Threading.** Read lock for `Lookup`, `GetRaw`, `Size`,
   `GetGeneration`, `MinValidKeyId`, `NextAeadNonce`,
   `PeekAeadCounter`, `Snapshot`. Write lock for `Install`,
   `InstallUnsalted`, `Retire`, `SetAeadSalt`, `SetMinValidKeyId`,
   `RestoreAeadCounter`, `Restore`. Metrics scrapers and stats
   accessors take the read lock and are safe alongside hot-path
   `NextAeadNonce` / `Lookup` traffic.

### Per-key AEAD nonce counter (foot-gun closure)

For AEAD-class envelope algorithms (e.g. ChaCha20-Poly1305), the caller
is responsible for ensuring every (key, nonce) pair is unique — reuse
destroys both confidentiality and integrity. The keyring tracks a
64-bit monotonic counter per key:

```c
uint8_t nonce[12];
if (!hmacKeyringNextAeadNonce(ring, key_id, nonce)) {
    /* Refused: key id absent OR counter exhausted (2^64 issued). */
    abort();
}
/* Use `nonce` exactly once for flexEnvelopeSeal under `key_id`. */
```

The 12-byte nonce is `[counter 8B LE][salt 4B]`. Install a per-process
salt with `hmacKeyringSetAeadSalt` to give every writer process a
distinct nonce space; this matters across restarts when the counter
goes back to zero (see `hmacKeyringRestoreAeadCounter` for durable
restore).

### AEAD nonce-reuse detector (defense-in-depth)

When you can't fully trust the nonce source — restored counter is
stale, randomly-sampled nonces, mixed sources — observe each nonce
through `aeadNonceLog` BEFORE calling Seal:

```c
#include "aeadNonceLog.h"

aeadNonceLog *log = aeadNonceLogCreate(8192);  /* sliding window */
if (!aeadNonceLogObserve(log, key_id, nonce)) {
    /* Duplicate in window — refuse to seal; rotate key. */
    return CRYPTO_REUSE_REFUSED;
}
chacha20poly1305Seal(...);
```

The window is FIFO-evicting and bounded, so it's a recent-history
guard, not a perfect oracle. Rotate keys well within the window.

### BLAKE3 derive-key — context-separated sub-keys

To produce many independent keys from one root secret without storing
them separately, use the BLAKE3 KDF:

```c
#include "blake3.h"

const uint8_t root[32] = {...}; /* high-entropy long-term secret */
const uint8_t ctx_log[] = {'d', 'k', '-', 'l', 'o', 'g'};
uint8_t log_key[32];
blake3DeriveKey(ctx_log, sizeof(ctx_log), root, sizeof(root), log_key,
                sizeof(log_key));
```

Different context bytes yield independent keys; same context bytes
yield the same key deterministically. Context is a protocol identifier,
not a secret — include the application name and a version stamp so a
protocol upgrade can't collide with the previous version's key.

## flex envelope — integrity-checked wrapper

The envelope is the canonical way to ship a flex (or any byte buffer)
across a trust boundary while being able to detect tampering or
accidental corruption on receive.

### Producer side

```c
#include "flexEnvelope.h"

/* Hash the payload with xxh64 (8-byte checksum — fast, accidental
 * corruption detection only).  For tamper-evidence across an
 * untrusted boundary, use HMAC_SHA256 or BLAKE3_KEYED instead. */
flexEnvelopeWrapParams wp = {
    .algo = FLEX_ENV_ALGO_XXH64,
};

/* Figure out how much space the envelope will need. */
size_t needed = flexEnvelopeWrappedSize(payload_len, wp.algo, 0);
uint8_t *buf = zmalloc(needed);  /* datakit allocator from datakit.h */

/* Wrap. */
size_t written = 0;
if (!flexEnvelopeWrap(buf, needed, payload, payload_len, &wp, &written)) {
    /* couldn't emit envelope (bad params, out_cap too small, etc.) */
}

/* `buf` now contains a self-describing envelope.  Send it. */
```

For a keyed (tamper-evident) envelope:

```c
flexEnvelopeWrapParams wp = {
    .algo    = FLEX_ENV_ALGO_HMAC_SHA256,
    .key     = my_key,
    .key_len = sizeof(my_key),
};
/* rest as above */
```

### Consumer side

```c
flexEnvelopeVerifyParams vp = {
    .key     = my_key,        /* only needed for keyed algorithms */
    .key_len = sizeof(my_key),
};

const uint8_t *payload;
uint64_t payload_len;
if (!flexEnvelopeVerify(buf, buf_len, &vp, &payload, &payload_len)) {
    /* Envelope corrupted, tampered, or malformed — drop. */
    return;
}

/* `payload` points INTO `buf` (no allocation, no copy).  Safe to
 * read.  If you need a mutable copy, memcpy it yourself. */
do_something_with(payload, payload_len);
```

### Convenience: verify and get a flex back

```c
flex *f = flexEnvelopeUnwrapAsFlex(buf, buf_len, &vp);
if (!f) {
    /* verify failed OR flex structure check failed */
}
/* use f normally; free with flexFree */
```

### Verify policy flags (secure-by-default)

The default `flexEnvelopeVerifyParams.opts == 0` is the strictest
sensible behavior. Opt-in flags relax specific checks:

| Flag               | Default behavior           | When to flip ON                                        |
| ------------------ | -------------------------- | ------------------------------------------------------ |
| `ALLOW_TRAILING`   | reject trailing bytes      | envelope is embedded in a larger frame                 |
| `REQUIRE_FLEX`     | payload is opaque          | also run flexValidateBuffer on the payload             |
| `FAST_COMPARE`     | constant-time HMAC compare | trusted local context, you've thought about timing     |
| `REQUIRE_HMAC`     | accept any algo class      | reject non-keyed envelopes (defense against downgrade) |
| `ALLOW_DEPRECATED` | reject deprecated algos    | accept old envelopes during migration                  |
| `PEEK_ONLY`        | full verify                | just want the framing, not the digest                  |
| `ALLOW_NONE_ALGO`  | reject `ALGO_NONE`         | tests only — NEVER in production                       |
| `ALLOW_OVERSIZE`   | reject payloads > 1 GiB    | you've thought about DoS and want larger limits        |
| `BOUND_PAYLOAD`    | use 1 GiB default cap      | enforce your own tighter cap via `max_payload_len`     |

### Combining: prepared HMAC key + envelope

For high-throughput verifies of HMAC-class envelopes:

```c
uint8_t prep_storage[1024];
hmacKey prep;
hmacPrepareKey(&prep, &hmacAlgoSHA256,
               prep_storage, sizeof(prep_storage),
               my_key, sizeof(my_key));

/* Per-envelope verify uses the prepared key — skips the K_ipad/K_opad
 * block compressions on every call.  ~2x faster on small payloads. */
flexEnvelopeVerifyParams vp = { .prepared_key = &prep };

/* In a tight loop: */
for (each incoming envelope) {
    const uint8_t *payload;
    uint64_t payload_len;
    if (flexEnvelopeVerify(buf, buf_len, &vp, &payload, &payload_len)) {
        process(payload, payload_len);
    }
}
```

## End-to-end example: integrity-checked replication frame

A producer wraps a flex containing some structured data with an HMAC
envelope, and a consumer verifies and unwraps it back into a flex.

```c
#include "flex.h"
#include "flexEnvelope.h"
#include "hmac.h"

static const uint8_t cluster_key[32] = { /* shared secret */ };

/* ---- producer ---- */
uint8_t *wrap_replication_frame(const flex *payload, size_t *out_len) {
    const size_t pbytes = flexBytes(payload);
    const size_t total =
        flexEnvelopeWrappedSize(pbytes, FLEX_ENV_ALGO_HMAC_SHA256, 0);
    uint8_t *buf = malloc(total);

    flexEnvelopeWrapParams wp = {
        .algo    = FLEX_ENV_ALGO_HMAC_SHA256,
        .key     = cluster_key,
        .key_len = sizeof(cluster_key),
    };
    size_t written = 0;
    if (!flexEnvelopeWrap(buf, total,
                          (const uint8_t *)payload, pbytes,
                          &wp, &written)) {
        zfree(buf);
        return NULL;
    }
    *out_len = written;
    return buf;
}

/* ---- consumer (hot path; uses prepared key) ---- */
static uint8_t prep_storage[1024];
static hmacKey  cluster_prep;
static int      prep_initialized = 0;

flex *unwrap_replication_frame(const uint8_t *buf, size_t buf_len) {
    if (!prep_initialized) {
        hmacPrepareKey(&cluster_prep, &hmacAlgoSHA256,
                       prep_storage, sizeof(prep_storage),
                       cluster_key, sizeof(cluster_key));
        prep_initialized = 1;
    }
    flexEnvelopeVerifyParams vp = {
        .prepared_key = &cluster_prep,
        .opts         = FLEX_ENV_VERIFY_REQUIRE_FLEX,
    };
    return flexEnvelopeUnwrapAsFlex(buf, buf_len, &vp);
}
```

## Algorithm quick reference

| Algorithm enum                         | Class        | Digest   | Key        | Nonce | Relative speed       |
| -------------------------------------- | ------------ | -------- | ---------- | ----- | -------------------- |
| `FLEX_ENV_ALGO_XXH64`                  | checksum     | 8 B      | n/a        | n/a   | very fast            |
| `FLEX_ENV_ALGO_XXH3_64`                | checksum     | 8 B      | n/a        | n/a   | fastest              |
| `FLEX_ENV_ALGO_XXH3_128`               | checksum     | 16 B     | n/a        | n/a   | fastest              |
| `FLEX_ENV_ALGO_BLAKE3_KEYED`           | MAC (native) | 32 B     | 32 B       | n/a   | fast                 |
| `FLEX_ENV_ALGO_HMAC_SHA256`            | MAC          | 32 B     | any length | n/a   | fast (varies by CPU) |
| `FLEX_ENV_ALGO_HMAC_BLAKE3`            | MAC          | 32 B     | any length | n/a   | fast                 |
| `FLEX_ENV_ALGO_AEAD_CHACHA20_POLY1305` | AEAD         | 16 B tag | 32 B       | 12 B  | fast                 |

Pick by **threat model first**, then by speed. The `xxh*` algorithms
detect accidental corruption only — they are trivially forgeable by
an attacker and must never be used to authenticate data crossing a
trust boundary. Use a MAC-class algorithm (BLAKE3*KEYED or HMAC*\*)
whenever an adversary could tamper with the bytes between producer
and consumer.

Speed is hardware-dependent. HMAC-SHA-256 outperforms BLAKE3 on
hosts with the SHA-256 instruction-set extension (Apple Silicon,
Intel Goldmont+ / AMD Zen+); BLAKE3 wins on hosts without it.

## Common mistakes to avoid

1. **Using `FLEX_ENV_ALGO_NONE` in production.** The default verify
   path rejects it. It exists only for testing the framing-without-
   integrity case. If you find yourself adding `ALLOW_NONE_ALGO` to
   production verify code, stop and reconsider.

2. **Trusting an xxh\* envelope across a security boundary.** xxh\* is
   fast and great for catching bit flips and disk-corruption. It is
   **not** a MAC; an attacker can trivially recompute the digest after
   tampering. Use BLAKE3_KEYED or HMAC for tamper-evidence.

3. **Forgetting the cap on `payload_len`.** The default rejects
   envelopes claiming more than 1 GiB to defend against forged-length
   DoS. If you legitimately need larger envelopes, set
   `BOUND_PAYLOAD` with your own `max_payload_len` rather than
   `ALLOW_OVERSIZE` so the cap is explicit.

4. **Using `hmacCompute` in a tight verify loop.** Each call pays the
   K_ipad/K_opad block compressions. For repeated-key workloads,
   prepare the key once with `hmacPrepareKey` and call
   `hmacComputeWithKey` — ~2× faster on small messages.

5. **Reusing an AEAD nonce.** Reusing the same (key, nonce) pair under
   ChaCha20-Poly1305 (or any AEAD) destroys both confidentiality AND
   integrity. Use `hmacKeyringNextAeadNonce` to allocate, plus
   `aeadNonceLog` as defense-in-depth. After restart, restore the
   counter from durable state (`hmacKeyringRestoreAeadCounter`) before
   issuing any nonce — or rotate the key.

## Cluster mailbox — verified-frame router

`clusterMailbox` composes the envelope + keyring into a typed pipeline
for replication and apply paths. It does:

1. Sealing: `(origin_id, sequence, msg_type, payload)` → envelope bytes
   under a chosen MAC key.
2. Ingest: envelope bytes → verified frame dispatched to a caller-
   supplied apply callback.
3. Per-origin replay tracking (strict-monotonic or 64-bit window).
4. Atomic event counters (`clusterMailboxGetStats`) for security
   monitoring.

```c
#include "clusterMailbox.h"

/* Replay-tolerant mailbox (accepts in-window out-of-order frames). */
clusterMailbox *mb = clusterMailboxCreateEx(
    ring, FLEX_ENV_ALGO_HMAC_SHA256, on_apply, ctx,
    CLUSTER_MAILBOX_FLAG_REPLAY_WINDOW);

/* Producer side: */
size_t cap = clusterMailboxSealedSize(mb, payload_len);
uint8_t *out = zmalloc(cap);
size_t written = 0;
clusterMailboxSeal(mb, origin_id, sequence, msg_type, payload,
                   payload_len, key_id, out, cap, &written);

/* Consumer side: */
clusterMailboxIngestResult rc =
    clusterMailboxIngest(mb, incoming, incoming_len, key_id);
/* rc == CLUSTER_MAILBOX_OK iff on_apply was invoked and returned true. */
```

### Key rotation choreography

For overlap-window rotation across producer and consumer nodes, use
`clusterRotationState` plus the announcement-message helpers:

```c
/* Both producer and consumer hold this small struct. */
clusterRotationState rs;
clusterRotationInit(&rs, /* initial id */ K1);

/* Producer announces the new key id (sealed under K1): */
uint8_t announce[4];
clusterMailboxBuildRotateAnnounce(announce, K2);
clusterMailboxSeal(mb, origin_id, seq, CLUSTER_MAILBOX_MSG_KEY_ROTATE_ANNOUNCE,
                   announce, sizeof(announce), K1, out, cap, &w);

/* Consumer on receipt: */
hmacKeyId new_id;
if (clusterMailboxParseRotateAnnounce(frame->payload, frame->payload_len,
                                      &new_id)) {
    clusterRotationPromote(&rs, new_id);  /* now Accepts() both old + new */
}

/* Once everyone has applied a frame under new_id, retire the old: */
clusterRotationRetirePrevious(&rs);
hmacKeyringRetire(ring, K1);
```

`clusterRotationState` is pure policy — it never reads or writes the
keyring or mailbox. The application drives each step of the rotation
protocol.

## More common mistakes (continued)

6. **Comparing digests with `memcmp` in HMAC paths.** The flex
   envelope's verify path uses constant-time compare by default for
   keyed algorithms. If you're hand-rolling HMAC verification, do
   the same — don't `memcmp` your own digest comparison; use
   `dkCtMemEq` / `dkCtMemEq16` from `ctmem.h`.

7. **Clearing key material with `memset`.** Forbidden in this
   codebase — see
   **[Secure-wipe rule](#secure-wipe-rule--dksecurezero-is-the-only-correct-way-to-clear-secrets)**.
   Use `dkSecureZero` instead; vanilla `memset` on a buffer that is
   about to be freed or go out of scope is legally elidable under
   `-O3 -flto`.

8. **Re-using a `chacha20poly1305StreamCtx` after `Final`.** The
   post-Final ctx is bytewise zero (see the wipe contract above). A
   second `Final`
   returns `false`; an `Update` after `Final` returns `false`. Treat
   the ctx as consumed and allocate a fresh one for the next message.

9. **Skipping the wire `key_id` policy when verifying.** When a
   long-lived consumer is bound to a specific `key_id` via
   `prepared_key`, an incoming envelope that announces a different
   wire `key_id` is rejected by default — pass
   `FLEX_ENV_VERIFY_ACCEPT_WIRE_KEYID` to explicitly opt in to the
   keyring-resolve-from-wire case.
