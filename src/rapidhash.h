#pragma once

/* rapidhash V3 — very fast, high-quality, 64-bit non-cryptographic hash.
 *
 * ============================================================================
 * CRITICAL: Nano / Micro / Main are THREE DIFFERENT HASH FUNCTIONS.
 * ============================================================================
 *
 * They are NOT optimizations of each other.  They produce DIFFERENT 64-bit
 * outputs for the same input bytes once the input exceeds the smaller
 * variant's bulk threshold:
 *
 *               Identical output for inputs in this range:
 *   Nano  ↔ Micro :    0 .. 48 bytes      (both use the small-input path)
 *   Nano  ↔ Main  :    0 .. 48 bytes
 *   Micro ↔ Main  :    0 .. 80 bytes      (Main starts its bulk at 112 B,
 *                                          Micro at 80 B — but both share
 *                                          the same tail mixer for 17..80)
 *
 * Above those thresholds the variants diverge by design (different lane
 * count → different mix-tree shape → different bits in the output).
 *
 * Practical consequence:
 *   Once you pick a variant for any persistent purpose — disk hash, content
 *   address, cluster routing key, hash-table key — you MUST stay with that
 *   variant for the lifetime of that data.  Mixing variants inside one
 *   namespace silently produces collisions where there should be none, and
 *   misses where there should be hits.
 *
 *   If your workload's input size varies, pick ONE variant up front based
 *   on the typical / worst-case size and use it everywhere.  Do not switch
 *   variants per-call based on input length.
 *
 * COMPACT vs UNROLLED, on the other hand, ARE the same hash function:
 * they apply to the Main variant only and produce bit-identical output.
 * The dispatcher (or rapidhashSetForceImpl) selects between them based on
 * input size for throughput.  Swapping freely between COMPACT and UNROLLED
 * is safe and supported — that is the whole point of the dispatcher.
 *
 * ============================================================================
 * Picking a variant
 * ============================================================================
 *
 *   rapidhashNano  — 3-lane bulk, 48 B stride, smallest code footprint.
 *                    Fastest on inputs <= ~48 B.  Loses badly on long
 *                    inputs (~24 GB/s at 1 MB vs Main's ~42 GB/s on the
 *                    same hardware).
 *                    Use when: every byte of code size matters (embedded,
 *                    bootloader), or when 100%% of your inputs fit in
 *                    a single 48 B stride.
 *
 *   rapidhashMicro — 5-lane bulk, 80 B stride, modest code footprint.
 *                    Fastest in the 64..256 B range; ~15-20%% slower than
 *                    Main above 1 KB.
 *                    Use when: HPC / server workloads where the typical
 *                    key is one cache line (64 B) up to a few hundred
 *                    bytes — DNS names, URL paths, short JSON keys.
 *
 *   rapidhash      — 7-lane bulk, 112 B stride.  Best on inputs > 1 KB
 *                    and the right default for unknown-size workloads.
 *                    Loses ~5-15%% on inputs in the 48..256 B range vs
 *                    the smaller variants, but never falls off a cliff.
 *                    Use when: you don't know the input size up front,
 *                    or your hot path operates on >= 1 KB buffers
 *                    (file content, network payload, document body).
 *
 * Hard numbers on Apple Silicon (no sanitizers, MB/s):
 *
 *   One-shot (rapidhash / rapidhashMicro / rapidhashNano):
 *     size:       48 B    112 B    1 KB    16 KB     1 MB
 *     Nano      19,000   20,500  27,000   25,000   24,000
 *     Micro     19,000   18,000  35,000   37,000   35,000
 *     Main      15,200   20,000  37,000   44,000   43,000
 *
 *   Streaming (rapidhashStream{Init,Update,Final}):
 *     size:                       1 KB    16 KB     1 MB
 *     Stream-Nano                6,000    7,000    6,500
 *     Stream-Micro               7,500    8,000    8,000
 *     Stream-Main                7,500    8,900    8,750
 *
 *   Dispatcher (Main variant only): AUTO selects the body to call, then
 *   executes IT — there is no separate AUTO code path.  At len > 224
 *   AUTO is byte-identical to UNROLLED; at len <= 224 it is
 *   byte-identical to COMPACT.  Forcing the impl matters only when you
 *   want a workload pinned regardless of size (benchmarks, regression
 *   testing).  Bench noise dominates any apparent "AUTO is faster"
 *   gap — they cannot differ algorithmically.
 *
 * Streaming throughput is ~5× lower than one-shot because each Update
 * pays buffer-copy and dispatch overhead per stride.  Both numbers
 * still beat sha256 / blake3 by an order of magnitude — see
 * perfRegression rows for the head-to-head.
 *
 * If in doubt, use rapidhash() — it's the documented default.
 *
 * Two mixing modes:
 *   FAST       (default): 64x64 -> 128 multiply, low/high written back.
 *                         Top-of-class throughput; the SMHasher recommended
 *                         setting.
 *   PROTECTED  (opt-in):  multiply result is XOR-ed into the inputs
 *                         instead of overwriting them.  Defends against
 *                         the algebraic-invariant attacks that broke
 *                         wyhash V1.  ~10%% slower; still non-cryptographic.
 *                         PROTECTED is NOT a security upgrade — it does
 *                         NOT make rapidhash a MAC or a PRF, and it does
 *                         NOT defend against an adversary who can choose
 *                         inputs.  If you don't already know what
 *                         "algebraic-invariant attack" means, FAST is
 *                         the right choice and PROTECTED buys you
 *                         nothing.  Cryptographic tamper-evidence lives
 *                         in HMAC-SHA256 / HMAC-BLAKE3 / BLAKE3-keyed.
 *
 * Two main-loop shapes (BOTH compiled in, runtime-dispatched):
 *   COMPACT:   one 7-lane pass per 112-byte chunk.  Smallest code.
 *   UNROLLED:  two 7-lane passes per 224-byte chunk.  Larger code,
 *              ~10%% faster on inputs > 16 KB.  Bit-identical output to
 *              COMPACT — the choice is a code-shape selection, not a
 *              correctness change.
 *
 * Default is AUTO: the Main variant's public entry inspects input
 * length and picks UNROLLED for inputs longer than the auto-threshold
 * (currently 224 B, returned by rapidhashAutoUnrollThreshold), else
 * COMPACT.  Use rapidhashSetForceImpl() to override for A/B benchmarks
 * or to lock a code path in a constrained-i-cache environment.
 *
 * Streaming context (incremental):
 *   rapidhashStream* covers Init / Update / Final and produces the same
 *   64-bit output as the one-shot function for the same byte sequence.
 *
 * Secret derivation:
 *   rapidhashMakeSecret() derives a domain-separated 8-word secret from a
 *   seed using wyrand + popcount-balanced primes (port of upstream
 *   secret.h).  Callers that need cryptographic-style domain separation
 *   between hash tables should pass the derived secret into the
 *   *WithSecret entry points instead of relying on the default secret.
 *
 * Output is 64-bit and endian-invariant (the implementation byteswaps on
 * big-endian targets after each load).  All buffers are caller-owned;
 * the hashing path performs no heap allocation. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RAPIDHASH_SECRET_LEN 8
#define RAPIDHASH_STREAM_BUF_LEN 112 /* max bulk stride across all variants */

/* Returns a pointer to the canonical 8-word secret.  Callers that want
 * to derive related-but-distinct hashes (e.g. by xor-ing a domain tag
 * into one slot) should copy the returned values into their own array
 * and pass that to the *WithSecret entries. */
const uint64_t *rapidhashGetDefaultSecret(void);

/* ---------- One-shot, FAST mode (default) ---------- */

uint64_t rapidhash(const void *key, size_t len);
uint64_t rapidhashWithSeed(const void *key, size_t len, uint64_t seed);
uint64_t rapidhashWithSecret(const void *key, size_t len, uint64_t seed,
                             const uint64_t secret[RAPIDHASH_SECRET_LEN]);

uint64_t rapidhashMicro(const void *key, size_t len);
uint64_t rapidhashMicroWithSeed(const void *key, size_t len, uint64_t seed);
uint64_t rapidhashMicroWithSecret(const void *key, size_t len, uint64_t seed,
                                  const uint64_t secret[RAPIDHASH_SECRET_LEN]);

uint64_t rapidhashNano(const void *key, size_t len);
uint64_t rapidhashNanoWithSeed(const void *key, size_t len, uint64_t seed);
uint64_t rapidhashNanoWithSecret(const void *key, size_t len, uint64_t seed,
                                 const uint64_t secret[RAPIDHASH_SECRET_LEN]);

/* ---------- One-shot, PROTECTED mode ---------- */
/* Same surface as the FAST entries but uses the XOR-in mum.  Outputs are
 * NOT compatible with the FAST entries — callers must pick one mode and
 * stick with it for the lifetime of any persistent data structure keyed
 * by the hash. */

uint64_t rapidhashProtected(const void *key, size_t len);
uint64_t rapidhashProtectedWithSeed(const void *key, size_t len, uint64_t seed);
uint64_t
rapidhashProtectedWithSecret(const void *key, size_t len, uint64_t seed,
                             const uint64_t secret[RAPIDHASH_SECRET_LEN]);

uint64_t rapidhashMicroProtected(const void *key, size_t len);
uint64_t rapidhashMicroProtectedWithSeed(const void *key, size_t len,
                                         uint64_t seed);
uint64_t
rapidhashMicroProtectedWithSecret(const void *key, size_t len, uint64_t seed,
                                  const uint64_t secret[RAPIDHASH_SECRET_LEN]);

uint64_t rapidhashNanoProtected(const void *key, size_t len);
uint64_t rapidhashNanoProtectedWithSeed(const void *key, size_t len,
                                        uint64_t seed);
uint64_t
rapidhashNanoProtectedWithSecret(const void *key, size_t len, uint64_t seed,
                                 const uint64_t secret[RAPIDHASH_SECRET_LEN]);

/* ---------- Streaming (incremental) context ---------- */

/* Per-variant stride for the streaming buffer. */
typedef enum rapidhashVariant {
    RAPIDHASH_VARIANT_MAIN = 0,  /* 7-lane, 112 B stride */
    RAPIDHASH_VARIANT_MICRO = 1, /* 5-lane,  80 B stride */
    RAPIDHASH_VARIANT_NANO = 2   /* 3-lane,  48 B stride */
} rapidhashVariant;

typedef enum rapidhashMode {
    RAPIDHASH_MODE_FAST = 0,
    RAPIDHASH_MODE_PROTECTED = 1
} rapidhashMode;

typedef struct rapidhashCtx rapidhashCtx;

/* Bulk-flush callback installed at Init.  Picked from one of six
 * specializations (3 variants × FAST/PROTECTED) so the per-stride hot
 * loop runs with `prot` and lane count both constant-folded at compile
 * time — no runtime branch on ctx->variant / ctx->mode inside the
 * streaming Update path. */
typedef void (*rapidhashStreamBulkFn)(rapidhashCtx *ctx, const uint8_t *p);

struct rapidhashCtx {
    /* Largest members first to minimize padding. */
    uint64_t lanes[7];                     /* seed, see1..see6 */
    uint64_t secret[RAPIDHASH_SECRET_LEN]; /* working secret copy */
    uint64_t total_len;                    /* bytes Update'd so far */
    uint64_t initial_seed;                 /* for tail re-derivation */
    rapidhashStreamBulkFn bulk_fn;         /* specialized flush callback */
    uint8_t buf[RAPIDHASH_STREAM_BUF_LEN]; /* partial-stride buffer */
    /* Trailing 16 bytes of the most recently flushed bulk stride.
     * The one-shot algorithm's final read can reach (stride - 16 + i)
     * bytes BACKWARD into the last bulk-consumed region when the
     * residual i < 16; streaming reconstructs that read by splicing
     * this trailing window with the head of `buf`. */
    uint8_t bulk_tail[16];
    uint16_t buf_len;     /* bytes currently in buf */
    uint16_t stride;      /* 48 / 80 / 112 */
    uint8_t variant;      /* rapidhashVariant */
    uint8_t mode;         /* rapidhashMode */
    uint8_t bulk_engaged; /* a full stride was flushed */
};

void rapidhashStreamInit(rapidhashCtx *ctx, rapidhashVariant variant,
                         rapidhashMode mode, uint64_t seed,
                         const uint64_t secret[RAPIDHASH_SECRET_LEN]);
void rapidhashStreamUpdate(rapidhashCtx *ctx, const void *data, size_t len);
/* Final consumes the context.  Calling Final on the same ctx a second
 * time without an intervening Init is unsupported and will produce
 * meaningless output — there's no contract that the ctx survives. */
uint64_t rapidhashStreamFinal(rapidhashCtx *ctx);

/* ---------- Runtime dispatch (Main variant only) ---------- */

/* The Main variant ships both bulk-loop shapes; the public entry picks
 * one per call based on input length.  Micro and Nano have no UNROLLED
 * form (their bulk threshold sits below the unroll crossover anyway). */
typedef enum rapidhashImpl {
    RAPIDHASH_IMPL_AUTO = 0,     /* default: pick by input length */
    RAPIDHASH_IMPL_COMPACT = 1,  /* always 7-lane / 112 B stride */
    RAPIDHASH_IMPL_UNROLLED = 2, /* always 14-lane / 224 B stride */
} rapidhashImpl;

/* Override the dispatcher's choice.  Intended for benchmarking and
 * differential testing — production callers should leave the default
 * AUTO in place.
 *
 * Thread safety: backed by a plain process-global; set it once before
 * any thread enters a rapidhash*() entry.  Calling it concurrently
 * with a hash entry on another thread is a data race per the C11
 * memory model.  No save-and-restore protocol is provided — callers
 * that want to test multiple impls in sequence must serialize their
 * own Set/Hash/Restore cycle.  Streaming entries (rapidhashStream*)
 * IGNORE this knob; they always run the variant/mode the caller
 * pinned at Init. */
void rapidhashSetForceImpl(rapidhashImpl impl);
rapidhashImpl rapidhashGetForceImpl(void);

/* Returns the AUTO crossover threshold in bytes — inputs strictly longer
 * than this value engage the UNROLLED body; shorter inputs use COMPACT.
 * Below 225 B the two bodies are functionally identical at the machine
 * level (UNROLLED's outer `while (i > 224)` never fires), so the
 * threshold's only real effect kicks in on long inputs. */
size_t rapidhashAutoUnrollThreshold(void);

/* ---------- Secret derivation ---------- */

/* Generates a popcount-balanced, prime-tested 4-word secret from `seed`
 * into `secret[0..3]`; `secret[4..7]` are filled with the default secret
 * tail (callers that want full domain separation should overwrite those
 * slots themselves).  Deterministic for a given seed.  Uses Miller-Rabin
 * with bases 2,3,5,7,11,13,17,19,23,29,31,37 — sufficient for 64-bit
 * primality up to the well-known SPRP coverage bounds.
 *
 * Note: the internal wyrand PRNG always runs with FAST mixing regardless
 * of which mum-mode the caller plans to use the resulting secret with.
 * Derived secrets are therefore stable across FAST and PROTECTED callers
 * — a deliberate property so a single seed value names the same secret
 * everywhere. */
void rapidhashMakeSecret(uint64_t seed, uint64_t secret[RAPIDHASH_SECRET_LEN]);

/* ---------- Test entry point ---------- */

#ifdef DATAKIT_TEST
int rapidhashTest(int argc, char *argv[]);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif
