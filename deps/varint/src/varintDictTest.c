#include "varintDict.h"
#include "ctest.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

int varintDictTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("Dictionary build and lookup") {
        const uint64_t values[] = {100, 200, 100, 300, 200, 100};
        size_t count = 6;

        varintDict *dict = varintDictCreate();
        varintDictBuild(dict, values, count);

        /* Should have 3 unique values */
        if (dict->size != 3) {
            ERR("Dictionary size = %u, expected 3", dict->size);
        }

        /* Test lookups */
        int32_t idx100 = varintDictFind(dict, 100);
        int32_t idx200 = varintDictFind(dict, 200);
        int32_t idx300 = varintDictFind(dict, 300);

        if (idx100 < 0 || idx200 < 0 || idx300 < 0) {
            ERRR("Failed to find values in dictionary");
        }

        /* Non-existent value */
        if (varintDictFind(dict, 999) != -1) {
            ERRR("Found non-existent value in dictionary");
        }

        varintDictFree(dict);
    }

    TEST("Basic dictionary encode/decode") {
        const uint64_t values[] = {10, 20, 10, 30, 20, 10, 30};
        size_t count = 7;
        uint8_t buffer[512];

        size_t encoded = varintDictEncode(buffer, values, count);
        if (encoded == 0) {
            ERRR("Failed to encode dictionary array");
        }

        size_t decoded_count = 0;
        uint64_t *decoded = varintDictDecode(buffer, encoded, &decoded_count);

        if (decoded == NULL) {
            ERRR("Failed to decode dictionary array");
            return err;
        }

        if (decoded_count != count) {
            ERR("Decoded count %zu != original count %zu", decoded_count,
                count);
        }

        for (size_t i = 0; i < count; i++) {
            if (decoded[i] != values[i]) {
                ERR("Decoded[%zu] = %" PRIu64 ", expected %" PRIu64 "", i,
                    decoded[i], values[i]);
            }
        }

        free(decoded);
    }

    TEST("Highly repetitive data") {
        /* Simulate log sources: 5 unique values repeated 100 times */
        uint64_t values[100];
        const uint64_t sources[] = {100, 200, 300, 400, 500};
        for (int i = 0; i < 100; i++) {
            values[i] = sources[i % 5];
        }

        uint8_t buffer[2048];
        size_t encoded = varintDictEncode(buffer, values, 100);

        /* Should be very efficient: ~5 dictionary entries + 100 small indices
         */
        /* Naive: 100 * 8 = 800 bytes */
        if (encoded >= 800) {
            ERR("Dictionary not efficient for repetitive data: %zu bytes",
                encoded);
        }

        size_t count = 0;
        uint64_t *decoded = varintDictDecode(buffer, encoded, &count);

        if (decoded == NULL) {
            ERRR("Failed to decode repetitive data");
            return err;
        }

        if (count != 100) {
            ERR("Repetitive data count = %zu, expected 100", count);
        }

        for (int i = 0; i < 100; i++) {
            if (decoded[i] != values[i]) {
                ERR("Repetitive data[%d] mismatch", i);
                break;
            }
        }

        free(decoded);
    }

    TEST("Single unique value") {
        uint64_t values[50];
        for (int i = 0; i < 50; i++) {
            values[i] = 777;
        }

        uint8_t buffer[1024];
        size_t encoded = varintDictEncode(buffer, values, 50);

        /* Should be very small (1 dict entry + 50 indices of 0) */
        size_t count = 0;
        uint64_t *decoded = varintDictDecode(buffer, encoded, &count);

        if (decoded == NULL) {
            ERRR("Failed to decode single unique value");
            return err;
        }

        for (int i = 0; i < 50; i++) {
            if (decoded[i] != 777) {
                ERR("Single value[%d] = %" PRIu64 ", expected 777", i,
                    decoded[i]);
                break;
            }
        }

        free(decoded);
    }

    TEST("All unique values (poor compression case)") {
        uint64_t values[50];
        for (int i = 0; i < 50; i++) {
            values[i] = (uint64_t)i * 1000;
        }

        uint8_t buffer[2048];
        size_t encoded = varintDictEncode(buffer, values, 50);

        /* Won't compress well, but should still be correct */
        size_t count = 0;
        uint64_t *decoded = varintDictDecode(buffer, encoded, &count);

        if (decoded == NULL) {
            ERRR("Failed to decode unique values");
            return err;
        }

        for (int i = 0; i < 50; i++) {
            if (decoded[i] != values[i]) {
                ERR("Unique values[%d] mismatch", i);
                break;
            }
        }

        free(decoded);
    }

    TEST("Dictionary size calculation") {
        const uint64_t values[] = {1, 2, 1, 3, 2, 1};
        uint8_t buffer[512];

        size_t calculated = varintDictEncodedSize(values, 6);
        size_t encoded = varintDictEncode(buffer, values, 6);

        if (calculated != encoded) {
            ERR("Calculated size %zu != encoded size %zu", calculated, encoded);
        }
    }

    TEST("Dictionary with large values") {
        const uint64_t values[] = {1000000000ULL, 2000000000ULL, 1000000000ULL,
                                   3000000000ULL, 2000000000ULL};
        uint8_t buffer[512];

        size_t encoded = varintDictEncode(buffer, values, 5);
        size_t count = 0;
        uint64_t *decoded = varintDictDecode(buffer, encoded, &count);

        if (decoded == NULL) {
            ERRR("Failed to decode large values");
            return err;
        }

        for (size_t i = 0; i < 5; i++) {
            if (decoded[i] != values[i]) {
                ERR("Large value[%zu] = %" PRIu64 ", expected %" PRIu64 "", i,
                    decoded[i], values[i]);
            }
        }

        free(decoded);
    }

    TEST("Empty dictionary build") {
        const uint64_t value[] = {42};
        varintDict *dict = varintDictCreate();

        varintDictBuild(dict, value, 1);

        if (dict->size != 1) {
            ERR("Single value dict size = %u, expected 1", dict->size);
        }

        if (dict->values[0] != 42) {
            ERR("Dictionary entry = %" PRIu64 ", expected 42", dict->values[0]);
        }

        varintDictFree(dict);
    }

    TEST("Dictionary binary search correctness") {
        /* Create sorted dictionary manually */
        varintDict *dict = varintDictCreate();
        free(dict->values); /* Free the initial allocation from Create */
        dict->capacity = 10;
        dict->values = malloc(sizeof(uint64_t) * dict->capacity);
        if (dict->values == NULL) {
            ERRR("Failed to allocate dictionary values");
            varintDictFree(dict);
            return err;
        }
        dict->size = 5;
        dict->values[0] = 10;
        dict->values[1] = 20;
        dict->values[2] = 30;
        dict->values[3] = 40;
        dict->values[4] = 50;

        /* Test boundary cases */
        if (varintDictFind(dict, 10) != 0) {
            ERRR("Find first element failed");
        }
        if (varintDictFind(dict, 50) != 4) {
            ERRR("Find last element failed");
        }
        if (varintDictFind(dict, 30) != 2) {
            ERRR("Find middle element failed");
        }
        if (varintDictFind(dict, 25) != -1) {
            ERRR("Found non-existent element");
        }

        varintDictFree(dict);
    }

    TEST("Dictionary decode into pre-allocated buffer") {
        const uint64_t values[] = {1, 2, 3, 1, 2};
        uint8_t buffer[512];

        size_t encoded = varintDictEncode(buffer, values, 5);

        uint64_t output[10];
        size_t count = varintDictDecodeInto(buffer, encoded, output, 10);
        if (count != 5) {
            ERR("Dictionary decode count = %zu, expected 5", count);
        }

        for (size_t i = 0; i < count; i++) {
            if (output[i] != values[i]) {
                ERR("DecodeInto[%zu] = %" PRIu64 ", expected %" PRIu64 "", i,
                    output[i], values[i]);
            }
        }
    }

    TEST_FINAL_RESULT;
}

#ifdef VARINT_DICT_TEST_STANDALONE
int main(int argc, char *argv[]) {
    return varintDictTest(argc, argv);
}
#endif
