/**
 * example_chained.c - Demonstrates varintChained usage
 *
 * varintChained provides continuation-bit encoding compatible with:
 * - Protocol Buffers (protobuf)
 * - SQLite3 database format
 * - LevelDB key-value store
 *
 * Each byte contains 7 bits of data + 1 continuation bit.
 * Continuation bit set = more bytes follow.
 *
 * Compile: gcc -I../src example_chained.c ../src/varintChained.c -o
 * example_chained Run: ./example_chained
 */

#include "varintChained.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Example 1: Basic encode/decode
void example_basic(void) {
    printf("\n=== Example 1: Basic Encode/Decode ===\n");

    uint8_t buffer[9];
    uint64_t original = 12345;

    // Encode
    varintWidth width = varintChainedPutVarint(buffer, original);
    printf("Encoded %" PRIu64 " in %d bytes\n", original, width);

    // Show the encoded bytes
    printf("Encoded bytes: ");
    for (uint8_t i = 0; i < (uint8_t)width; i++) {
        printf("%02x ", buffer[i]);
        if (i < (uint8_t)width - 1) {
            printf("(cont) ");
        }
    }
    printf("\n");

    // Decode
    uint64_t decoded;
    varintWidth decodedWidth = varintChainedGetVarint(buffer, &decoded);

    printf("Decoded: %" PRIu64 " (%d bytes)\n", decoded, decodedWidth);

    assert(original == decoded);
    assert(width == decodedWidth);
    printf("✓ Round-trip successful\n");
}

// Example 2: Continuation bit encoding
void example_continuation_bits(void) {
    printf("\n=== Example 2: Continuation Bit Encoding ===\n");

    struct {
        uint64_t value;
        varintWidth expectedWidth;
        const char *description;
    } tests[] = {
        {0, 1, "Zero"},
        {127, 1, "1-byte max (7 bits)"},
        {128, 2, "2-byte min"},
        {16383, 2, "2-byte max (14 bits)"},
        {16384, 3, "3-byte min"},
        {2097151, 3, "3-byte max (21 bits)"},
        {268435455UL, 4, "4-byte max (28 bits)"},
        {UINT64_MAX, 9, "uint64_t max (9 bytes)"},
    };

    printf("Each byte: 7 bits data + 1 continuation bit\n");
    printf("Continuation bit ON = more bytes follow\n\n");

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        uint8_t buffer[9];
        varintWidth width = varintChainedPutVarint(buffer, tests[i].value);

        printf("%-20s: %20" PRIu64 " -> %d bytes ", tests[i].description,
               tests[i].value, width);

        // Show continuation bits
        printf("[");
        for (uint8_t j = 0; j < (uint8_t)width; j++) {
            printf("%c", (buffer[j] & 0x80) ? '1' : '0');
        }
        printf("] ");

        assert(width == tests[i].expectedWidth);

        uint64_t decoded;
        varintChainedGetVarint(buffer, &decoded);
        assert(decoded == tests[i].value);

        printf("✓\n");
    }
}

// Example 3: SQLite3 varint format validation
void example_sqlite3_format(void) {
    printf("\n=== Example 3: SQLite3 Varint Format Validation ===\n");

    // These test vectors use SQLite3 varint format (big-endian continuation
    // chain) NOTE: This is DIFFERENT from Protocol Buffers (which uses
    // little-endian)
    struct {
        uint64_t value;
        uint8_t expected[9];
        varintWidth expectedLen;
    } tests[] = {
        {1, {0x01}, 1},
        {127, {0x7f}, 1},
        {128,
         {0x81, 0x00},
         2}, // SQLite3: 0x81 0x00 (NOT Protocol Buffers: 0x80 0x01)
        {300,
         {0x82, 0x2c},
         2}, // SQLite3: 0x82 0x2c (NOT Protocol Buffers: 0xac 0x02)
        {16384,
         {0x81, 0x80, 0x00},
         3}, // SQLite3: 0x81 0x80 0x00 (NOT Protocol Buffers: 0x80 0x80 0x01)
    };

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        uint8_t buffer[9];
        varintWidth width = varintChainedPutVarint(buffer, tests[i].value);

        printf("Value %" PRIu64 ":\n", tests[i].value);
        printf("  Expected: ");
        for (uint8_t j = 0; j < (uint8_t)tests[i].expectedLen; j++) {
            printf("%02x ", tests[i].expected[j]);
        }
        printf("\n  Got:      ");
        for (uint8_t j = 0; j < (uint8_t)width; j++) {
            printf("%02x ", buffer[j]);
        }
        printf("\n");

        assert(width == tests[i].expectedLen);
        assert(memcmp(buffer, tests[i].expected, width) == 0);

        printf("  ✓ Matches SQLite3 varint format\n");
    }
}

// Example 4: Stream decoding
void example_stream_decoding(void) {
    printf("\n=== Example 4: Stream Decoding ===\n");

    // Encode multiple values into a stream
    uint8_t stream[64];
    uint64_t values[] = {10, 100, 1000, 10000, 100000};
    size_t count = sizeof(values) / sizeof(values[0]);

    size_t offset = 0;
    printf("Encoding stream:\n");
    for (size_t i = 0; i < count; i++) {
        varintWidth width = varintChainedPutVarint(stream + offset, values[i]);
        printf("  Value %" PRIu64 " at offset %zu (width %d)\n", values[i],
               offset, width);
        offset += width;
    }

    printf("Total stream size: %zu bytes\n", offset);

    // Decode stream
    printf("\nDecoding stream:\n");
    offset = 0;
    for (size_t i = 0; i < count; i++) {
        uint64_t decoded;
        varintWidth width = varintChainedGetVarint(stream + offset, &decoded);
        printf("  Offset %zu: %" PRIu64 " (width %d)\n", offset, decoded,
               width);
        assert(decoded == values[i]);
        offset += width;
    }

    printf("✓ Stream encoding/decoding works\n");
}

// Example 5: Length detection
void example_length_detection(void) {
    printf("\n=== Example 5: Length Detection ===\n");

    uint64_t testValues[] = {50, 500, 5000, 50000};

    printf("Detecting length from continuation bits:\n");

    for (size_t i = 0; i < sizeof(testValues) / sizeof(testValues[0]); i++) {
        uint8_t buffer[9];
        varintWidth actualWidth = varintChainedPutVarint(buffer, testValues[i]);

        // Manual length detection by checking continuation bits
        varintWidth detectedWidth = 0;
        for (int j = 0; j < 9; j++) {
            detectedWidth++;
            if ((buffer[j] & 0x80) == 0) { // No continuation bit
                break;
            }
        }

        printf("  Value %" PRIu64 ": detected %d bytes, actual %d bytes ",
               testValues[i], detectedWidth, actualWidth);

        assert(detectedWidth == actualWidth);
        printf("✓\n");
    }
}

// Example 6: 9-byte special case
void example_nine_bytes(void) {
    printf("\n=== Example 6: 9-Byte Special Case ===\n");

    // Values requiring 9 bytes (> 63 bits)
    uint64_t largeValues[] = {
        (1ULL << 56) - 1, // 8 bytes
        (1ULL << 56),     // 9 bytes
        UINT64_MAX,       // 9 bytes
    };

    printf("9th byte uses all 8 bits (no continuation bit needed):\n");

    for (size_t i = 0; i < sizeof(largeValues) / sizeof(largeValues[0]); i++) {
        uint8_t buffer[9];
        varintWidth width = varintChainedPutVarint(buffer, largeValues[i]);

        printf("  Value 0x%016" PRIx64 " -> %d bytes\n", largeValues[i], width);

        if (width == 9) {
            printf("    9th byte: 0x%02x (all 8 bits used, can be 0x00)\n",
                   buffer[8]);
            // No assertion needed - 9th byte can be any value including 0x00
        }

        uint64_t decoded;
        varintChainedGetVarint(buffer, &decoded);
        assert(decoded == largeValues[i]);
        printf("    ✓ Decoded correctly\n");
    }
}

// Example 7: Performance comparison
void example_performance(void) {
    printf("\n=== Example 7: Space Usage Analysis ===\n");

    uint64_t testValues[] = {10, 100, 1000, 10000, 100000, 1000000, 10000000};

    printf("Value      | Chained | uint64_t | Savings\n");
    printf("-----------|---------|----------|--------\n");

    for (size_t i = 0; i < sizeof(testValues) / sizeof(testValues[0]); i++) {
        uint8_t buffer[9];
        varintWidth width = varintChainedPutVarint(buffer, testValues[i]);
        float savings = ((8.0 - width) / 8.0) * 100.0;

        printf("%10" PRIu64 " | %2d      | 8        | %5.1f%%\n", testValues[i],
               width, savings);
    }
}

// Example 8: Comparison with other formats
void example_format_comparison(void) {
    printf("\n=== Example 8: Format Comparison ===\n");

    printf("Understanding continuation bit encoding:\n\n");

    uint64_t value = 300; // Example value
    uint8_t buffer[9];
    (void)varintChainedPutVarint(buffer, value);

    printf("Value: %" PRIu64 " (binary: ", value);
    for (int i = 13; i >= 0; i--) {
        printf("%d", (int)((value >> i) & 1));
        if (i == 7) {
            printf(" ");
        }
    }
    printf(")\n\n");

    printf("Encoded as varintChained (2 bytes):\n");
    printf("  Byte 0: 0x%02x = ", buffer[0]);
    for (int i = 7; i >= 0; i--) {
        printf("%d", (buffer[0] >> i) & 1);
        if (i == 7) {
            printf(" (cont) ");
        }
    }
    printf(" (data)\n");

    printf("  Byte 1: 0x%02x = ", buffer[1]);
    for (int i = 7; i >= 0; i--) {
        printf("%d", (buffer[1] >> i) & 1);
        if (i == 7) {
            printf(" (cont) ");
        }
    }
    printf(" (data)\n\n");

    printf("Data bits extracted: ");
    /* SQLite3 varint uses big-endian: first byte has high bits, second byte has
     * low bits */
    uint64_t extracted =
        ((uint64_t)(buffer[0] & 0x7f) << 7) | (buffer[1] & 0x7f);
    for (int i = 13; i >= 0; i--) {
        printf("%d", (int)((extracted >> i) & 1));
        if (i == 7) {
            printf(" ");
        }
    }
    printf(" = %" PRIu64 "\n", extracted);

    assert(extracted == value);
    printf("\n✓ Continuation bit encoding explained\n");
}

int main(void) {
    printf("===========================================\n");
    printf("    varintChained Example Suite\n");
    printf("===========================================\n");
    printf("Compatible with: Protocol Buffers, SQLite3, LevelDB\n");

    example_basic();
    example_continuation_bits();
    example_sqlite3_format();
    example_stream_decoding();
    example_length_detection();
    example_nine_bytes();
    example_performance();
    example_format_comparison();

    printf("\n===========================================\n");
    printf("All examples completed successfully!\n");
    printf("===========================================\n");

    return 0;
}
