#include "varintTelemetry.h"

const char *varintCodecName(varintCodecID id) {
    switch (id) {
    case VARINT_CODEC_DELTA:
        return "DELTA";
    case VARINT_CODEC_FOR:
        return "FOR";
    case VARINT_CODEC_PFOR:
        return "PFOR";
    case VARINT_CODEC_DICT:
        return "DICT";
    case VARINT_CODEC_BITMAP:
        return "BITMAP";
    case VARINT_CODEC_TAGGED:
        return "TAGGED";
    case VARINT_CODEC_GROUP:
        return "GROUP";
    case VARINT_CODEC_DELTA_DELTA:
        return "DELTA_DELTA";
    case VARINT_CODEC_STRIDE:
        return "STRIDE";
    case VARINT_CODEC_RLE:
        return "RLE";
    case VARINT_CODEC_BP128:
        return "BP128";
    case VARINT_CODEC_BP128_DELTA:
        return "BP128_DELTA";
    case VARINT_CODEC_ELIAS_GAMMA:
        return "ELIAS_GAMMA";
    case VARINT_CODEC_ELIAS_DELTA:
        return "ELIAS_DELTA";
    case VARINT_CODEC_EXTERNAL:
        return "EXTERNAL";
    case VARINT_CODEC_PALETTE:
        return "PALETTE";
    case VARINT_CODEC_PALETTE_DELTA:
        return "PALETTE_DELTA";
    default:
        return "?";
    }
}

#ifdef VARINT_TELEMETRY

varintTelemetryEntry varintTelemetry[VARINT_CODEC_MAX];

void varintTelemetrySnapshot(varintTelemetryEntry *out, size_t maxEntries) {
    if (!out) {
        return;
    }
    size_t n = maxEntries < VARINT_CODEC_MAX ? maxEntries : VARINT_CODEC_MAX;
    for (size_t i = 0; i < n; i++) {
        out[i].calls =
            __atomic_load_n(&varintTelemetry[i].calls, __ATOMIC_RELAXED);
        out[i].wins =
            __atomic_load_n(&varintTelemetry[i].wins, __ATOMIC_RELAXED);
        out[i].bytesIn =
            __atomic_load_n(&varintTelemetry[i].bytesIn, __ATOMIC_RELAXED);
        out[i].bytesOut =
            __atomic_load_n(&varintTelemetry[i].bytesOut, __ATOMIC_RELAXED);
    }
}

void varintTelemetryReset(void) {
    for (size_t i = 0; i < VARINT_CODEC_MAX; i++) {
        __atomic_store_n(&varintTelemetry[i].calls, 0UL, __ATOMIC_RELAXED);
        __atomic_store_n(&varintTelemetry[i].wins, 0UL, __ATOMIC_RELAXED);
        __atomic_store_n(&varintTelemetry[i].bytesIn, 0UL, __ATOMIC_RELAXED);
        __atomic_store_n(&varintTelemetry[i].bytesOut, 0UL, __ATOMIC_RELAXED);
    }
}

#endif /* VARINT_TELEMETRY */
