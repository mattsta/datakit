/**
 * example_group.c - Demonstrates varintGroup usage
 *
 * varintGroup provides efficient encoding of related values with shared
 * metadata. Perfect for struct-like data, multi-column rows, network packets,
 * and batched operations where multiple values are logically grouped together.
 *
 * Compile: gcc -I../../src -o example_group example_group.c
 * ../../src/varintGroup.c ../../src/varintExternal.c Run: ./example_group
 */

#include "varintGroup.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Example 1: Basic encode/decode
void example_basic() {
    printf("\n=== Example 1: Basic Encode/Decode ===\n");

    const uint64_t values[] = {25, 50000, 94102};
    uint8_t fieldCount = 3;
    uint8_t buffer[64];

    // Encode group
    size_t encoded = varintGroupEncode(buffer, values, fieldCount);
    printf("Encoded %d fields in %zu bytes\n", fieldCount, encoded);
    printf("  Fields: [%" PRIu64 ", %" PRIu64 ", %" PRIu64 "]\n", values[0],
           values[1], values[2]);

    // Show encoding breakdown
    printf("  Breakdown:\n");
    printf("    - Field count: 1 byte\n");
    printf("    - Width bitmap: %zu byte(s)\n",
           varintGroupBitmapSize_(fieldCount));
    printf("    - Values: %zu bytes\n",
           encoded - 1 - varintGroupBitmapSize_(fieldCount));

    // Decode group
    uint64_t decoded[3];
    uint8_t decodedCount;
    size_t consumed = varintGroupDecode(buffer, decoded, &decodedCount, 3);

    printf("Decoded %d fields (%zu bytes consumed)\n", decodedCount, consumed);
    printf("  Fields: [%" PRIu64 ", %" PRIu64 ", %" PRIu64 "]\n", decoded[0],
           decoded[1], decoded[2]);

    assert(consumed == encoded);
    assert(decodedCount == fieldCount);
    for (int i = 0; i < fieldCount; i++) {
        assert(decoded[i] == values[i]);
    }

    printf("✓ Round-trip successful\n");
}

// Example 2: Struct encoding
typedef struct {
    uint64_t age;
    uint64_t salary;
    uint64_t zipcode;
    uint64_t timestamp;
} PersonRecord;

void encodePersonRecord(uint8_t *dst, const PersonRecord *record,
                        size_t *size) {
    const uint64_t values[4] = {record->age, record->salary, record->zipcode,
                                record->timestamp};
    *size = varintGroupEncode(dst, values, 4);
}

void decodePersonRecord(const uint8_t *src, PersonRecord *record) {
    uint64_t values[4];
    uint8_t count;
    varintGroupDecode(src, values, &count, 4);

    assert(count == 4);
    record->age = values[0];
    record->salary = values[1];
    record->zipcode = values[2];
    record->timestamp = values[3];
}

void example_struct_encoding() {
    printf("\n=== Example 2: Struct Encoding ===\n");

    PersonRecord people[] = {
        {25, 50000, 94102, 1700000000},
        {42, 120000, 10001, 1700000060},
        {31, 75000, 60601, 1700000120},
    };

    printf("Encoding %zu person records:\n",
           sizeof(people) / sizeof(people[0]));

    size_t totalEncoded = 0;
    size_t totalNative = 0;

    for (size_t i = 0; i < 3; i++) {
        uint8_t buffer[64];
        size_t size;
        encodePersonRecord(buffer, &people[i], &size);

        printf("  Record %zu: age=%" PRIu64 ", salary=%" PRIu64 ", zip=%" PRIu64
               ", time=%" PRIu64 "\n",
               i, people[i].age, people[i].salary, people[i].zipcode,
               people[i].timestamp);
        printf("    Encoded: %zu bytes (vs %zu native)\n", size,
               sizeof(PersonRecord));

        totalEncoded += size;
        totalNative += sizeof(PersonRecord);

        // Verify round-trip
        PersonRecord decoded;
        decodePersonRecord(buffer, &decoded);
        assert(decoded.age == people[i].age);
        assert(decoded.salary == people[i].salary);
        assert(decoded.zipcode == people[i].zipcode);
        assert(decoded.timestamp == people[i].timestamp);
    }

    printf("\nTotal: %zu bytes encoded (vs %zu native)\n", totalEncoded,
           totalNative);
    printf("Savings: %.1f%%\n",
           ((float)(totalNative - totalEncoded) / totalNative) * 100);
    printf("✓ All records encoded and decoded correctly\n");
}

// Example 3: Multi-column table rows
void example_table_rows() {
    printf("\n=== Example 3: Multi-Column Table Rows ===\n");

    // Table: [id, quantity, price, category_id]
    uint64_t rows[][4] = {
        {1, 5, 1999, 10},
        {2, 1, 49999, 25},
        {3, 100, 299, 10},
        {4, 3, 15000, 42},
    };
    size_t rowCount = 4;
    size_t colCount = 4;

    printf("Encoding table with %zu rows, %zu columns:\n", rowCount, colCount);

    uint8_t *encoded[4];
    size_t sizes[4];
    size_t totalSize = 0;

    for (size_t i = 0; i < rowCount; i++) {
        encoded[i] = malloc(64);
        sizes[i] = varintGroupEncode(encoded[i], rows[i], colCount);
        totalSize += sizes[i];

        printf("  Row %zu: [%" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64
               "] -> %zu bytes\n",
               i, rows[i][0], rows[i][1], rows[i][2], rows[i][3], sizes[i]);
    }

    printf("\nTotal size: %zu bytes\n", totalSize);
    printf("Native size: %zu bytes (4 rows * 4 cols * 8 bytes)\n",
           rowCount * colCount * 8);
    printf("Compression ratio: %.2fx\n",
           (float)(rowCount * colCount * 8) / totalSize);

    // Verify decoding
    for (size_t i = 0; i < rowCount; i++) {
        uint64_t decoded[4];
        uint8_t count;
        varintGroupDecode(encoded[i], decoded, &count, 4);

        assert(count == colCount);
        for (size_t j = 0; j < colCount; j++) {
            assert(decoded[j] == rows[i][j]);
        }
        free(encoded[i]);
    }

    printf("✓ All rows verified\n");
}

// Example 4: Network packet fields
typedef struct {
    uint64_t version;
    uint64_t msgType;
    uint64_t msgId;
    uint64_t timestamp;
    uint64_t payloadLen;
} PacketHeader;

void encodePacketHeader(uint8_t *dst, const PacketHeader *header,
                        size_t *size) {
    const uint64_t values[5] = {header->version, header->msgType, header->msgId,
                                header->timestamp, header->payloadLen};
    *size = varintGroupEncode(dst, values, 5);
}

void decodePacketHeader(const uint8_t *src, PacketHeader *header) {
    uint64_t values[5];
    uint8_t count;
    varintGroupDecode(src, values, &count, 5);
    assert(count == 5);

    header->version = values[0];
    header->msgType = values[1];
    header->msgId = values[2];
    header->timestamp = values[3];
    header->payloadLen = values[4];
}

void example_network_packets() {
    printf("\n=== Example 4: Network Packet Headers ===\n");

    PacketHeader packets[] = {
        {1, 10, 1001, 1700000000, 256},
        {1, 11, 1002, 1700000001, 1024},
        {1, 12, 1003, 1700000002, 64},
    };

    printf("Encoding %zu packet headers:\n",
           sizeof(packets) / sizeof(packets[0]));

    size_t totalVarint = 0;
    size_t totalFixed = sizeof(PacketHeader) * 3;

    for (size_t i = 0; i < 3; i++) {
        uint8_t buffer[64];
        size_t size;
        encodePacketHeader(buffer, &packets[i], &size);

        printf("  Packet %zu: ver=%" PRIu64 ", type=%" PRIu64 ", id=%" PRIu64
               ", time=%" PRIu64 ", len=%" PRIu64 "\n",
               i, packets[i].version, packets[i].msgType, packets[i].msgId,
               packets[i].timestamp, packets[i].payloadLen);
        printf("    Size: %zu bytes (vs %zu fixed)\n", size,
               sizeof(PacketHeader));

        totalVarint += size;

        // Verify
        PacketHeader decoded;
        decodePacketHeader(buffer, &decoded);
        assert(decoded.version == packets[i].version);
        assert(decoded.msgType == packets[i].msgType);
        assert(decoded.msgId == packets[i].msgId);
        assert(decoded.timestamp == packets[i].timestamp);
        assert(decoded.payloadLen == packets[i].payloadLen);
    }

    printf("\nTotal: %zu bytes (varint) vs %zu bytes (fixed)\n", totalVarint,
           totalFixed);
    printf("Savings: %.1f%%\n",
           ((float)(totalFixed - totalVarint) / totalFixed) * 100);
    printf("✓ All packets verified\n");
}

// Example 5: Field extraction without full decode
void example_field_extraction() {
    printf("\n=== Example 5: Fast Field Extraction ===\n");

    // Encode a large record
    const uint64_t values[] = {10, 20, 30, 40, 50, 60, 70, 80};
    uint8_t fieldCount = 8;
    uint8_t buffer[128];

    size_t encoded = varintGroupEncode(buffer, values, fieldCount);
    printf("Encoded %d fields in %zu bytes\n", fieldCount, encoded);

    // Extract specific fields without decoding everything
    printf("\nExtracting individual fields:\n");

    for (uint8_t i = 0; i < fieldCount; i += 2) {
        uint64_t value;
        size_t consumed = varintGroupGetField(buffer, i, &value);

        printf("  Field %d: %" PRIu64 " (accessed %zu bytes)\n", i, value,
               consumed);
        assert(value == values[i]);
    }

    printf("\n✓ Field extraction successful\n");
}

// Example 6: Size calculation
void example_size_calculation() {
    printf("\n=== Example 6: Size Calculation ===\n");

    uint64_t testGroups[][5] = {
        {1, 2, 3, 4, 5},                     // All small
        {100, 200, 300, 400, 500},           // Medium
        {10000, 20000, 30000, 40000, 50000}, // Large
    };

    printf("Calculating sizes for different value ranges:\n\n");

    for (size_t i = 0; i < 3; i++) {
        size_t predictedSize = varintGroupSize(testGroups[i], 5);

        uint8_t buffer[128];
        size_t actualSize = varintGroupEncode(buffer, testGroups[i], 5);

        printf("Group %zu: ", i);
        for (int j = 0; j < 5; j++) {
            printf("%" PRIu64 " ", testGroups[i][j]);
        }
        printf("\n");
        printf("  Predicted: %zu bytes\n", predictedSize);
        printf("  Actual:    %zu bytes\n", actualSize);

        assert(predictedSize == actualSize);

        // Also test GetSize
        size_t queriedSize = varintGroupGetSize(buffer);
        printf("  Queried:   %zu bytes\n", queriedSize);
        assert(queriedSize == actualSize);

        printf("\n");
    }

    printf("✓ Size calculations accurate\n");
}

// Example 7: Space efficiency comparison
void example_space_efficiency() {
    printf("\n=== Example 7: Space Efficiency Analysis ===\n");

    struct {
        const char *description;
        uint64_t values[4];
    } tests[] = {
        {"Small values", {1, 2, 3, 4}},
        {"Mixed small/med", {10, 100, 1000, 10000}},
        {"All medium", {5000, 6000, 7000, 8000}},
        {"Large values", {1000000, 2000000, 3000000, 4000000}},
    };

    printf("%-20s | Group | Separate | Native | Overhead | vs Native\n",
           "Test Case");
    printf("---------------------|-------|----------|--------|----------|------"
           "----\n");

    for (size_t i = 0; i < 4; i++) {
        uint8_t buffer[128];
        size_t groupSize = varintGroupEncode(buffer, tests[i].values, 4);

        // Calculate size if encoded separately with varintExternal
        size_t separateSize = 0;
        for (int j = 0; j < 4; j++) {
            varintWidth w;
            varintExternalUnsignedEncoding(tests[i].values[j], w);
            separateSize += w;
        }

        size_t nativeSize = 4 * sizeof(uint64_t);

        // Group has overhead vs separate (for metadata), but massive savings vs
        // native
        float overheadVsSep =
            ((float)(groupSize - separateSize) / separateSize) * 100;
        float savingsVsNative =
            ((float)(nativeSize - groupSize) / nativeSize) * 100;

        printf("%-20s | %5zu | %8zu | %6zu | %7.1f%% | %8.1f%%\n",
               tests[i].description, groupSize, separateSize, nativeSize,
               overheadVsSep, savingsVsNative);
    }

    printf("\n");
    printf("Notes:\n");
    printf("  - 'Group' = varintGroup encoding (with shared metadata)\n");
    printf("  - 'Separate' = individual varintExternal encodings\n");
    printf("  - 'Native' = 4 x 8-byte uint64_t values\n");
    printf("  - Group encoding adds overhead but enables fast field access\n");
    printf(
        "  - Best for small groups (2-16 fields) with varying value sizes\n");
}

// Example 8: Boundary testing
void example_boundaries() {
    printf("\n=== Example 8: Boundary Values ===\n");

    struct {
        const char *description;
        uint64_t value;
    } boundaries[] = {
        {"Zero", 0},
        {"1-byte max", 255},
        {"2-byte min", 256},
        {"2-byte max", 65535},
        {"4-byte min", 65536},
        {"4-byte max", 4294967295UL},
        {"8-byte min", 4294967296UL},
        {"8-byte max", UINT64_MAX},
    };

    printf("Testing width encoding at boundaries:\n\n");

    for (size_t i = 0; i < 8; i++) {
        const uint64_t values[] = {boundaries[i].value};
        uint8_t buffer[32];

        size_t size = varintGroupEncode(buffer, values, 1);
        varintWidth width = varintGroupGetFieldWidth(buffer, 0);

        printf("%-15s: %20" PRIu64 " -> width=%d, total=%zu bytes\n",
               boundaries[i].description, boundaries[i].value, width, size);

        // Verify
        uint64_t decoded[1];
        uint8_t count;
        varintGroupDecode(buffer, decoded, &count, 1);
        assert(decoded[0] == boundaries[i].value);
    }

    printf("\n✓ All boundaries handled correctly\n");
}

// Main test harness
int main() {
    printf("===========================================\n");
    printf("    varintGroup Example Suite\n");
    printf("===========================================\n");
    printf("\nvarintGroup encodes multiple related values\n");
    printf("with shared metadata for efficient storage.\n");

    example_basic();
    example_struct_encoding();
    example_table_rows();
    example_network_packets();
    example_field_extraction();
    example_size_calculation();
    example_space_efficiency();
    example_boundaries();

    printf("\n===========================================\n");
    printf("All examples completed successfully!\n");
    printf("===========================================\n");

    return 0;
}
