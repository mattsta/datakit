#include "varintGroup.h"
#include "ctest.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

int varintGroupTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("Width encoding/decoding") {
        /* Test that width encoding/decoding works correctly */
        if (varintGroupWidthDecode_(varintGroupWidthEncode_(VARINT_WIDTH_8B)) !=
            VARINT_WIDTH_8B) {
            ERRR("Encode/Decode 8B failed");
        }
        if (varintGroupWidthDecode_(varintGroupWidthEncode_(
                VARINT_WIDTH_16B)) != VARINT_WIDTH_16B) {
            ERRR("Encode/Decode 16B failed");
        }
        if (varintGroupWidthDecode_(varintGroupWidthEncode_(
                VARINT_WIDTH_32B)) != VARINT_WIDTH_32B) {
            ERRR("Encode/Decode 32B failed");
        }
        if (varintGroupWidthDecode_(varintGroupWidthEncode_(
                VARINT_WIDTH_64B)) != VARINT_WIDTH_64B) {
            ERRR("Encode/Decode 64B failed");
        }
    }

    TEST("Basic group encode/decode") {
        uint64_t *values = (uint64_t *)malloc(4 * sizeof(uint64_t));
        if (!values) {
            ERRR("Failed to allocate memory for values");
        }
        values[0] = 10;
        values[1] = 1000;
        values[2] = 100000;
        values[3] = 10000000;
        uint8_t fieldCount = 4;
        uint8_t buffer[256];

        size_t encoded = varintGroupEncode(buffer, values, fieldCount);
        if (encoded == 0) {
            ERRR("Failed to encode group");
        }

        uint64_t *decoded = (uint64_t *)malloc(4 * sizeof(uint64_t));
        if (!decoded) {
            free(values);
            ERRR("Failed to allocate memory for decoded");
        }
        uint8_t decoded_count;
        size_t decoded_size =
            varintGroupDecode(buffer, decoded, &decoded_count, 4);

        if (decoded_size != encoded) {
            ERR("Decoded size %zu != encoded size %zu", decoded_size, encoded);
        }

        if (decoded_count != fieldCount) {
            ERR("Decoded count %d != field count %d", decoded_count,
                fieldCount);
        }

        for (int i = 0; i < fieldCount; i++) {
            if (decoded[i] != values[i]) {
                ERR("Decoded[%d] = %" PRIu64 ", expected %" PRIu64 "", i,
                    decoded[i], values[i]);
            }
        }

        free(decoded);
        free(values);
    }

    TEST("Single field group") {
        const uint64_t value[] = {42};
        uint8_t buffer[64];

        size_t encoded = varintGroupEncode(buffer, value, 1);
        (void)encoded; /* Intentionally unused in test */
        uint64_t decoded[1];
        uint8_t count;
        varintGroupDecode(buffer, decoded, &count, 1);

        if (count != 1) {
            ERR("Decoded count = %d, expected 1", count);
        }
        if (decoded[0] != value[0]) {
            ERR("Decoded value = %" PRIu64 ", expected %" PRIu64 "", decoded[0],
                value[0]);
        }
    }

    TEST("Random field access") {
        const uint64_t values[] = {100, 200, 300, 400, 500};
        uint8_t buffer[256];

        varintGroupEncode(buffer, values, 5);

        /* Access individual fields without full decode */
        for (uint8_t i = 0; i < 5; i++) {
            uint64_t val;
            size_t bytes = varintGroupGetField(buffer, i, &val);
            if (bytes == 0) {
                ERR("Failed to get field %d", i);
            }
            if (val != values[i]) {
                ERR("GetField(%d) = %" PRIu64 ", expected %" PRIu64 "", i, val,
                    values[i]);
            }
        }
    }

    TEST("Mixed size values") {
        /* Small, medium, large values */
        const uint64_t values[] = {1, 256, 65536, 16777216};
        uint8_t buffer[256];

        size_t encoded = varintGroupEncode(buffer, values, 4);
        (void)encoded; /* Intentionally unused in test */
        uint64_t decoded[4];
        uint8_t count;
        varintGroupDecode(buffer, decoded, &count, 4);

        for (int i = 0; i < 4; i++) {
            if (decoded[i] != values[i]) {
                ERR("Mixed value[%d] = %" PRIu64 ", expected %" PRIu64 "", i,
                    decoded[i], values[i]);
            }
        }
    }

    TEST("All zero values") {
        const uint64_t values[8] = {0};
        uint8_t buffer[256];

        size_t encoded = varintGroupEncode(buffer, values, 8);
        (void)encoded; /* Intentionally unused in test */
        uint64_t decoded[8];
        uint8_t count;
        varintGroupDecode(buffer, decoded, &count, 8);

        if (count != 8) {
            ERR("Decoded count = %d, expected 8", count);
        }

        for (int i = 0; i < 8; i++) {
            if (decoded[i] != 0) {
                ERR("Zero value[%d] = %" PRIu64 ", expected 0", i, decoded[i]);
            }
        }
    }

    TEST("Max field count") {
        uint64_t values[VARINT_GROUP_MAX_FIELDS];
        for (int i = 0; i < VARINT_GROUP_MAX_FIELDS; i++) {
            values[i] = (uint64_t)i * 100;
        }

        uint8_t buffer[2048];
        size_t encoded =
            varintGroupEncode(buffer, values, VARINT_GROUP_MAX_FIELDS);

        if (encoded == 0) {
            ERRR("Failed to encode max fields");
        }

        uint64_t decoded[VARINT_GROUP_MAX_FIELDS];
        uint8_t count;
        varintGroupDecode(buffer, decoded, &count, VARINT_GROUP_MAX_FIELDS);

        if (count != VARINT_GROUP_MAX_FIELDS) {
            ERR("Max fields count = %d, expected %d", count,
                VARINT_GROUP_MAX_FIELDS);
        }

        for (int i = 0; i < VARINT_GROUP_MAX_FIELDS; i++) {
            if (decoded[i] != values[i]) {
                ERR("Max field[%d] mismatch", i);
                break;
            }
        }
    }

    TEST("Group size calculation") {
        const uint64_t values[] = {1, 2, 3, 4};
        uint8_t buffer[256];

        size_t encoded = varintGroupEncode(buffer, values, 4);
        (void)encoded; /* Intentionally unused in test */
        size_t calculated = varintGroupGetSize(buffer);

        if (calculated != encoded) {
            ERR("Calculated size %zu != encoded size %zu", calculated, encoded);
        }
    }

    TEST("Round-trip encoding verification") {
        /* Verify group encoding produces correct round-trip results */
        const uint64_t values[] = {100000, 200000, 300000, 400000,
                                   500000, 600000, 700000, 800000};
        uint8_t buffer[256];

        /* Encode */
        size_t encoded = varintGroupEncode(buffer, values, 8);
        if (encoded == 0) {
            ERRR("Failed to encode group");
        }

        /* Decode and verify */
        uint64_t decoded[8];
        uint8_t count;
        varintGroupDecode(buffer, decoded, &count, 8);

        if (count != 8) {
            ERR("Decoded count = %d, expected 8", count);
        }

        for (int i = 0; i < 8; i++) {
            if (decoded[i] != values[i]) {
                ERR("Round-trip failed: decoded[%d] = %" PRIu64
                    ", expected %" PRIu64 "",
                    i, decoded[i], values[i]);
            }
        }
    }

    TEST_FINAL_RESULT;
}

#ifdef VARINT_GROUP_TEST_STANDALONE
int main(int argc, char *argv[]) {
    return varintGroupTest(argc, argv);
}
#endif
