#pragma once

#include "varint.h"

__BEGIN_DECLS

/* ====================================================================
 * Opt-in Codec Telemetry
 * ====================================================================
 * Per-codec atomic counters for diagnostics:
 *   - calls      : times the codec was *evaluated* during competition
 *   - wins       : times it produced the smallest output and was written
 *   - bytesIn    : cumulative input bytes considered
 *   - bytesOut   : cumulative encoded bytes emitted
 *
 * Counters are compiled out entirely unless VARINT_TELEMETRY is defined.
 * When enabled, updates use __atomic_fetch_add (lock-free, thread-safe).
 *
 * Codec IDs are stable. New codecs append to the enum; existing IDs must
 * never be renumbered (so persisted competition frames stay valid). */

/* Stable codec IDs. Numbered to align with varintAdaptiveEncodingType for
 * the first 7 entries so the two can interoperate. New entries append. */
typedef enum varintCodecID {
    VARINT_CODEC_DELTA = 0,
    VARINT_CODEC_FOR = 1,
    VARINT_CODEC_PFOR = 2,
    VARINT_CODEC_DICT = 3,
    VARINT_CODEC_BITMAP = 4,
    VARINT_CODEC_TAGGED = 5,
    VARINT_CODEC_GROUP = 6,
    VARINT_CODEC_DELTA_DELTA = 7,
    VARINT_CODEC_STRIDE = 8,
    VARINT_CODEC_RLE = 9,
    VARINT_CODEC_BP128 = 10,
    VARINT_CODEC_BP128_DELTA = 11,
    VARINT_CODEC_ELIAS_GAMMA = 12,
    VARINT_CODEC_ELIAS_DELTA = 13,
    VARINT_CODEC_EXTERNAL = 14,
    VARINT_CODEC_PALETTE = 15,
    VARINT_CODEC_PALETTE_DELTA = 16,
    /* Reserved range for future codecs. */
    VARINT_CODEC_MAX = 32,
} varintCodecID;

/* Human-readable codec name. Stable across versions. */
const char *varintCodecName(varintCodecID id);

#ifdef VARINT_TELEMETRY

/* Counters are exposed as a plain array so callers can snapshot/reset.
 * Each entry is 32 bytes — fits 2 entries per cache line. */
typedef struct varintTelemetryEntry {
    unsigned long calls;
    unsigned long wins;
    unsigned long bytesIn;
    unsigned long bytesOut;
} varintTelemetryEntry;

_Static_assert(sizeof(varintTelemetryEntry) == 32,
               "varintTelemetryEntry size changed!");

extern varintTelemetryEntry varintTelemetry[VARINT_CODEC_MAX];

/* Mutators — always atomic. Inline so they vanish if you compile a TU
 * without VARINT_TELEMETRY (since the externs would be unused). */
static inline void varintTelemetryRecordCall_(varintCodecID id,
                                              size_t bytesIn) {
    __atomic_fetch_add(&varintTelemetry[id].calls, 1UL, __ATOMIC_RELAXED);
    __atomic_fetch_add(&varintTelemetry[id].bytesIn, (unsigned long)bytesIn,
                       __ATOMIC_RELAXED);
}
static inline void varintTelemetryRecordWin_(varintCodecID id,
                                             size_t bytesOut) {
    __atomic_fetch_add(&varintTelemetry[id].wins, 1UL, __ATOMIC_RELAXED);
    __atomic_fetch_add(&varintTelemetry[id].bytesOut, (unsigned long)bytesOut,
                       __ATOMIC_RELAXED);
}

#define VARINT_TELEMETRY_CALL(id, bytesIn)                                     \
    varintTelemetryRecordCall_((id), (bytesIn))
#define VARINT_TELEMETRY_WIN(id, bytesOut)                                     \
    varintTelemetryRecordWin_((id), (bytesOut))

/* Read a snapshot (non-atomic read, but each field is atomic individually). */
void varintTelemetrySnapshot(varintTelemetryEntry *out, size_t maxEntries);

/* Reset all counters to zero (atomic store per field). */
void varintTelemetryReset(void);

#else /* !VARINT_TELEMETRY */

#define VARINT_TELEMETRY_CALL(id, bytesIn) ((void)(id), (void)(bytesIn))
#define VARINT_TELEMETRY_WIN(id, bytesOut) ((void)(id), (void)(bytesOut))

#endif /* VARINT_TELEMETRY */

__END_DECLS
