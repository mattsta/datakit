/* fuzzFlexEnvelope - untrusted bytes verified and opened as an envelope
 *
 * Input: the putative envelope.  flexEnvelopeVerify runs with no params
 * (the strictest defaults) and with a fixed 32-byte key; a frame it
 * accepts has its payload inside the input.  flexEnvelopeOpen runs with
 * the same key into a buffer as long as the input, and writes no more
 * than that buffer holds. */

#include "fuzzCommon.h"

#include "../flexEnvelope.h"

static const uint8_t fuzzKey[32] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
    0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};

static void fuzzVerify(const uint8_t *data, size_t len,
                       const flexEnvelopeVerifyParams *params) {
    const uint8_t *payload = NULL;
    uint64_t payloadLen = 0;
    if (flexEnvelopeVerify(data, len, params, &payload, &payloadLen)) {
        FUZZ_REQUIRE(payloadLen <= len);
        FUZZ_REQUIRE(payloadLen == 0 ||
                     (payload >= data && payload + payloadLen <= data + len));
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    const flexEnvelopeVerifyParams keyed = {.key = fuzzKey,
                                            .key_len = sizeof(fuzzKey)};
    fuzzVerify(data, len, NULL);
    fuzzVerify(data, len, &keyed);

    cleaner(zfree) void *plain = zmalloc(len ? len : 1);
    size_t written = 0;
    if (flexEnvelopeOpen(data, len, &keyed, plain, len, &written)) {
        FUZZ_REQUIRE(written <= len);
    }

    return 0;
}
