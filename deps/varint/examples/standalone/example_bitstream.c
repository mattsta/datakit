/**
 * example_bitstream.c - Demonstrates varintBitstream usage
 *
 * varintBitstream provides arbitrary bit-level read/write operations.
 * Unlike varintPacked (fixed-width arrays), bitstream allows:
 * - Arbitrary bit offsets (not aligned to slot boundaries)
 * - Variable bit widths for each value
 * - Signed value encoding
 *
 * Perfect for: protocol headers, flag packing, custom binary formats,
 * trie node encoding, and any bit-level data structure.
 *
 * Compile: gcc -I../src example_bitstream.c -o example_bitstream
 * Run: ./example_bitstream
 */

#include "varintBitstream.h"
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Example 1: Basic bit-level operations
void example_basic(void) {
    printf("\n=== Example 1: Basic Bit-Level Operations ===\n");

    vbits buffer[8]; // 8 slots × 64 bits = 512 bits
    memset(buffer, 0, sizeof(buffer));

    // Write a 3-bit value at offset 0
    varintBitstreamSet(buffer, 0, 3, 5); // Binary: 101
    printf("Wrote 5 (101 binary) at bit offset 0 (3 bits)\n");

    // Write a 5-bit value at offset 3
    varintBitstreamSet(buffer, 3, 5, 17); // Binary: 10001
    printf("Wrote 17 (10001 binary) at bit offset 3 (5 bits)\n");

    // Write a 7-bit value at offset 8
    varintBitstreamSet(buffer, 8, 7, 100); // Binary: 1100100
    printf("Wrote 100 (1100100 binary) at bit offset 8 (7 bits)\n");

    // Read back
    vbitsVal val1 = varintBitstreamGet(buffer, 0, 3);
    vbitsVal val2 = varintBitstreamGet(buffer, 3, 5);
    vbitsVal val3 = varintBitstreamGet(buffer, 8, 7);

    printf("\nRead back:\n");
    printf("  Offset 0 (3 bits): %" PRIu64 " (expected 5)\n", val1);
    printf("  Offset 3 (5 bits): %" PRIu64 " (expected 17)\n", val2);
    printf("  Offset 8 (7 bits): %" PRIu64 " (expected 100)\n", val3);

    assert(val1 == 5);
    assert(val2 == 17);
    assert(val3 == 100);

    printf("✓ Basic bit-level operations work\n");
}

// Example 2: Bit offset calculations
void example_bit_offsets(void) {
    printf("\n=== Example 2: Arbitrary Bit Offsets ===\n");

    vbits buffer[4];
    memset(buffer, 0, sizeof(buffer));

    // Write values at various offsets
    struct {
        size_t offset;
        size_t bits;
        vbitsVal value;
    } writes[] = {
        {0, 4, 15},     // 0-3
        {4, 6, 33},     // 4-9
        {10, 8, 255},   // 10-17
        {18, 10, 1023}, // 18-27
        {28, 12, 4095}, // 28-39
        {40, 5, 31},    // 40-44
    };

    printf("Writing values at arbitrary offsets:\n");
    for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); i++) {
        varintBitstreamSet(buffer, writes[i].offset, writes[i].bits,
                           writes[i].value);
        printf("  Offset %2zu, %2zu bits: value %" PRIu64 "\n",
               writes[i].offset, writes[i].bits, writes[i].value);
    }

    printf("\nReading back:\n");
    for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); i++) {
        vbitsVal value =
            varintBitstreamGet(buffer, writes[i].offset, writes[i].bits);
        printf("  Offset %2zu, %2zu bits: value %" PRIu64 "\n",
               writes[i].offset, writes[i].bits, value);

        assert(value == writes[i].value);
        printf("✓\n");
    }
}

// Example 3: Cross-slot values
void example_cross_slot(void) {
    printf("\n=== Example 3: Cross-Slot Values ===\n");

    vbits buffer[4];
    memset(buffer, 0, sizeof(buffer));

    // Write a value that spans across 64-bit slot boundary
    size_t offset = 60; // Near the end of first slot
    size_t bits = 10;   // Spans into second slot
    vbitsVal value = 1000;

    printf("Slot size: %zu bits\n", BITS_PER_SLOT);
    printf("Writing %zu-bit value at offset %zu (spans slots)\n", bits, offset);
    printf("Value: %" PRIu64 "\n", value);

    varintBitstreamSet(buffer, offset, bits, value);

    // Read back
    vbitsVal retrieved = varintBitstreamGet(buffer, offset, bits);

    printf("Retrieved: %" PRIu64 "\n", retrieved);
    assert(retrieved == value);

    printf("✓ Cross-slot values work correctly\n");
}

// Example 4: Protocol header packing
void example_protocol_header(void) {
    printf("\n=== Example 4: Protocol Header Packing ===\n");

    // Custom protocol header:
    // - Version: 3 bits (0-7)
    // - Message type: 4 bits (0-15)
    // - Flags: 5 bits
    // - Priority: 2 bits (0-3)
    // - Payload length: 16 bits (0-65535)
    // Total: 30 bits = 4 bytes (vs 8 bytes with normal fields)

    vbits buffer[1];
    memset(buffer, 0, sizeof(buffer));

    size_t offset = 0;

    // Pack header
    uint8_t version = 5;
    uint8_t msgType = 12;
    uint8_t flags = 0x16; /* 0b10110 */
    uint8_t priority = 3;
    uint16_t payloadLen = 1500;

    varintBitstreamSet(buffer, offset, 3, version);
    offset += 3;

    varintBitstreamSet(buffer, offset, 4, msgType);
    offset += 4;

    varintBitstreamSet(buffer, offset, 5, flags);
    offset += 5;

    varintBitstreamSet(buffer, offset, 2, priority);
    offset += 2;

    varintBitstreamSet(buffer, offset, 16, payloadLen);
    offset += 16;

    printf("Packed protocol header (%zu bits = %zu bytes):\n", offset,
           (offset + 7) / 8);
    printf("  Version: %u\n", version);
    printf("  Message Type: %u\n", msgType);
    printf("  Flags: 0x%02x\n", flags);
    printf("  Priority: %u\n", priority);
    printf("  Payload Length: %u\n", payloadLen);

    // Unpack header
    offset = 0;
    uint8_t readVersion = (uint8_t)varintBitstreamGet(buffer, offset, 3);
    offset += 3;

    uint8_t readMsgType = (uint8_t)varintBitstreamGet(buffer, offset, 4);
    offset += 4;

    uint8_t readFlags = (uint8_t)varintBitstreamGet(buffer, offset, 5);
    offset += 5;

    uint8_t readPriority = (uint8_t)varintBitstreamGet(buffer, offset, 2);
    offset += 2;

    uint16_t readPayloadLen = (uint16_t)varintBitstreamGet(buffer, offset, 16);
    offset += 16;

    printf("\nUnpacked:\n");
    printf("  Version: %u ✓\n", readVersion);
    printf("  Message Type: %u ✓\n", readMsgType);
    printf("  Flags: 0x%02x ✓\n", readFlags);
    printf("  Priority: %u ✓\n", readPriority);
    printf("  Payload Length: %u ✓\n", readPayloadLen);

    assert(readVersion == version);
    assert(readMsgType == msgType);
    assert(readFlags == flags);
    assert(readPriority == priority);
    assert(readPayloadLen == payloadLen);
}

// Example 5: Signed values
void example_signed_values(void) {
    printf("\n=== Example 5: Signed Values ===\n");

    vbits buffer[2];
    memset(buffer, 0, sizeof(buffer));

    // Store signed values using sign bit encoding
    int64_t signedValues[] = {-100, -1, 0, 1, 100, -500, 500};
    size_t offset = 0;
    size_t bitsPerValue = 12; // Enough for +/-2047

    printf("Storing signed values (%zu bits each):\n", bitsPerValue);

    for (size_t i = 0; i < sizeof(signedValues) / sizeof(signedValues[0]);
         i++) {
        int64_t original = signedValues[i];

        // Prepare signed value (convert to unsigned representation)
        vbitsVal prepared = (vbitsVal)original;
        if (original < 0) {
            _varintBitstreamPrepareSigned(prepared, bitsPerValue);
        }

        varintBitstreamSet(buffer, offset, bitsPerValue, prepared);
        printf("  Offset %2zu: %5" PRId64 "\n", offset, original);

        offset += bitsPerValue;
    }

    // Read back
    printf("\nReading back:\n");
    offset = 0;

    for (size_t i = 0; i < sizeof(signedValues) / sizeof(signedValues[0]);
         i++) {
        vbitsVal retrieved = varintBitstreamGet(buffer, offset, bitsPerValue);

        // Restore signed value
        _varintBitstreamRestoreSigned(retrieved, bitsPerValue);
        int64_t value = (int64_t)retrieved;

        printf("  Offset %2zu: %5" PRId64 " ", offset, value);
        assert(value == signedValues[i]);
        printf("✓\n");

        offset += bitsPerValue;
    }
}

// Example 6: Trie node encoding
void example_trie_node(void) {
    printf("\n=== Example 6: Trie Node Encoding ===\n");

    // Compact trie node representation:
    // - Is terminal: 1 bit
    // - Wildcard type: 2 bits (0=none, 1=single, 2=multi)
    // - Child count: 5 bits (0-31)
    // - Value ID: 24 bits (if terminal)
    // Total: 8 bits (non-terminal) or 32 bits (terminal)

    vbits buffer[2];
    memset(buffer, 0, sizeof(buffer));

    // Encode terminal node
    bool isTerminal = true;
    uint8_t wildcardType = 1; // Single wildcard
    uint8_t childCount = 3;
    uint32_t valueId = 12345;

    size_t offset = 0;
    varintBitstreamSet(buffer, offset, 1, isTerminal ? 1 : 0);
    offset += 1;

    varintBitstreamSet(buffer, offset, 2, wildcardType);
    offset += 2;

    varintBitstreamSet(buffer, offset, 5, childCount);
    offset += 5;

    if (isTerminal) {
        varintBitstreamSet(buffer, offset, 24, valueId);
        offset += 24;
    }

    printf("Trie node encoded (%zu bits = %zu bytes):\n", offset,
           (offset + 7) / 8);
    printf("  Terminal: %s\n", isTerminal ? "yes" : "no");
    printf("  Wildcard: %u\n", wildcardType);
    printf("  Children: %u\n", childCount);
    printf("  Value ID: %u\n", valueId);

    // Decode
    offset = 0;
    bool readTerminal = varintBitstreamGet(buffer, offset, 1) != 0;
    offset += 1;

    uint8_t readWildcard = (uint8_t)varintBitstreamGet(buffer, offset, 2);
    offset += 2;

    uint8_t readChildren = (uint8_t)varintBitstreamGet(buffer, offset, 5);
    offset += 5;

    uint32_t readValueId = 0;
    if (readTerminal) {
        readValueId = (uint32_t)varintBitstreamGet(buffer, offset, 24);
        (void)offset; // Last use of offset
    }

    assert(readTerminal == isTerminal);
    assert(readWildcard == wildcardType);
    assert(readChildren == childCount);
    assert(readValueId == valueId);

    printf("✓ Trie node encoding works\n");
}

// Example 7: Space efficiency
void example_space_efficiency(void) {
    printf("\n=== Example 7: Space Efficiency ===\n");

    printf("Comparison of different data representations:\n\n");

    // Example: storing 1000 small flags/values
    size_t count = 1000;

    struct {
        const char *description;
        size_t bitsPerValue;
        size_t bytesNeeded;
    } formats[] = {
        {"1-bit flags", 1, (count * 1 + 7) / 8},
        {"3-bit values (0-7)", 3, (count * 3 + 7) / 8},
        {"5-bit values (0-31)", 5, (count * 5 + 7) / 8},
        {"uint8_t", 8, count * 1},
        {"uint16_t", 16, count * 2},
        {"uint32_t", 32, count * 4},
    };

    printf("Format                | Bits/value | Bytes for %zu items | "
           "Efficiency\n",
           count);
    printf("----------------------|------------|---------------------|---------"
           "---\n");

    size_t uint32Size = count * 4;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); i++) {
        float efficiency =
            ((float)(uint32Size - formats[i].bytesNeeded) / uint32Size) * 100;

        printf("%-20s | %10zu | %19zu | %5.1f%% saved\n",
               formats[i].description, formats[i].bitsPerValue,
               formats[i].bytesNeeded, efficiency);
    }
}

int main(void) {
    printf("===========================================\n");
    printf("   varintBitstream Example Suite\n");
    printf("===========================================\n");

    example_basic();
    example_bit_offsets();
    example_cross_slot();
    example_protocol_header();
    example_signed_values();
    example_trie_node();
    example_space_efficiency();

    printf("\n===========================================\n");
    printf("All examples completed successfully!\n");
    printf("===========================================\n");

    return 0;
}
