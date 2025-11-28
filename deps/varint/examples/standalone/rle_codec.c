/**
 * rle_codec.c - Run-Length Encoding with Varint-Encoded Lengths
 *
 * Run-Length Encoding (RLE) compresses data by representing consecutive
 * identical values as (value, count) pairs. Using varints for run lengths
 * provides additional compression since most runs are short (1-2 bytes)
 * but occasional long runs need more space.
 *
 * This example demonstrates:
 * - RLE encoder/decoder with varint run lengths
 * - Byte-oriented RLE (value, length pairs)
 * - Bitmap RLE (1-bit run-length encoding)
 * - Literal escape sequences for non-compressible data
 * - Performance on various data patterns
 * - Comparison with fixed-width run lengths
 *
 * Compile: gcc -I../../src rle_codec.c ../../src/varintExternal.c -o rle_codec
 * Run: ./rle_codec
 */

#include "varintExternal.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// RLE Format:
// - For repeated bytes: [value][run_length as varint]
// - For literal sequences: [0xFF escape][length][literal bytes...]
// - 0xFF is reserved as escape code (remapped to literal sequence)

#define RLE_ESCAPE 0xFF
#define RLE_MAX_LITERAL_RUN 255

// Statistics for compression analysis
typedef struct {
    size_t originalSize;
    size_t compressedSize;
    size_t numRuns;
    size_t numLiterals;
    size_t longestRun;
} RLEStats;

/**
 * Encode data using RLE with varint run lengths
 * Format: [value][width][length_bytes...] for runs
 *         [ESCAPE][count][bytes...] for literals
 * Returns number of bytes written to output
 */
size_t rleEncode(const uint8_t *input, size_t inputLen, uint8_t *output,
                 RLEStats *stats) {
    if (inputLen == 0) {
        return 0;
    }

    size_t outPos = 0;
    size_t i = 0;

    if (stats) {
        memset(stats, 0, sizeof(RLEStats));
        stats->originalSize = inputLen;
    }

    while (i < inputLen) {
        uint8_t value = input[i];
        size_t runLength = 1;

        // Count run length
        while (i + runLength < inputLen && input[i + runLength] == value &&
               runLength < UINT64_MAX) {
            runLength++;
        }

        // Decide: encode as run or literal?
        // Run encoding uses: 1 byte (value) + 1 byte (width) + width bytes
        // (length)
        varintWidth lengthWidth;
        varintExternalUnsignedEncoding(runLength, lengthWidth);
        size_t runEncodedSize = 1 + 1 + lengthWidth;

        // For very short runs, check if literal might be better
        // But always encode runs >= 3 as runs for simplicity
        if (runLength >= 3 ||
            (value != RLE_ESCAPE && runEncodedSize <= runLength)) {
            // Encode as run: [value][width][length]
            if (value == RLE_ESCAPE) {
                // Special encoding for 0xFF runs: [ESCAPE][0][width][length]
                output[outPos++] = RLE_ESCAPE;
                output[outPos++] = 0; // 0 means "this is a run of 0xFF"
                output[outPos++] = (uint8_t)lengthWidth;
                varintExternalPutFixedWidthQuick_(output + outPos, runLength,
                                                  lengthWidth);
                outPos += lengthWidth;
            } else {
                output[outPos++] = value;
                output[outPos++] = (uint8_t)lengthWidth;
                varintExternalPutFixedWidthQuick_(output + outPos, runLength,
                                                  lengthWidth);
                outPos += lengthWidth;
            }

            if (stats) {
                stats->numRuns++;
                if (runLength > stats->longestRun) {
                    stats->longestRun = runLength;
                }
            }

            i += runLength;
        } else {
            // Encode as literal sequence
            size_t literalStart = i;
            size_t literalLen = runLength;

            // Extend literal sequence while it's beneficial
            while (i + literalLen < inputLen &&
                   literalLen < RLE_MAX_LITERAL_RUN) {
                uint8_t nextVal = input[i + literalLen];
                size_t nextRun = 1;

                while (i + literalLen + nextRun < inputLen &&
                       input[i + literalLen + nextRun] == nextVal &&
                       nextRun < 3) {
                    nextRun++;
                }

                if (nextRun >= 3) {
                    break; // Let the run be encoded separately
                }
                literalLen += nextRun;
            }

            // Encode literal sequence: [ESCAPE][length][bytes...]
            output[outPos++] = RLE_ESCAPE;
            output[outPos++] = (uint8_t)literalLen;
            memcpy(output + outPos, input + literalStart, literalLen);
            outPos += literalLen;

            if (stats) {
                stats->numLiterals += literalLen;
            }

            i += literalLen;
        }
    }

    if (stats) {
        stats->compressedSize = outPos;
    }

    return outPos;
}

/**
 * Decode RLE data back to original
 * Format: [value][width][length_bytes...] for runs
 *         [ESCAPE][count][bytes...] for literals
 * Returns number of bytes written to output
 */
size_t rleDecode(const uint8_t *input, size_t inputLen, uint8_t *output) {
    size_t inPos = 0;
    size_t outPos = 0;

    while (inPos < inputLen) {
        uint8_t value = input[inPos++];

        if (value == RLE_ESCAPE) {
            // Literal sequence or 0xFF run
            if (inPos >= inputLen) {
                break;
            }

            uint8_t litLen = input[inPos++];

            if (litLen == 0) {
                // Special case: run of 0xFF: [ESCAPE][0][width][length]
                if (inPos >= inputLen) {
                    break;
                }
                varintWidth width = input[inPos++];

                if (inPos + width <= inputLen) {
                    uint64_t runLength;
                    varintExternalGetQuick_(input + inPos, width, runLength);
                    inPos += width;

                    // Write run of 0xFF
                    for (size_t j = 0; j < runLength; j++) {
                        output[outPos++] = RLE_ESCAPE;
                    }
                }
            } else {
                // Copy literal bytes
                if (inPos + litLen <= inputLen) {
                    memcpy(output + outPos, input + inPos, litLen);
                    outPos += litLen;
                    inPos += litLen;
                }
            }
        } else {
            // Run-length encoded value: [value][width][length_bytes...]
            if (inPos >= inputLen) {
                break;
            }

            varintWidth width = input[inPos++];

            if (inPos + width <= inputLen) {
                uint64_t runLength;
                varintExternalGetQuick_(input + inPos, width, runLength);
                inPos += width;

                // Write the run
                for (size_t j = 0; j < runLength; j++) {
                    output[outPos++] = value;
                }
            }
        }
    }

    return outPos;
}

// Simplified bitmap RLE: runs of 0s and 1s
// Format: [bit_value (0 or 1)][width][run_length_bytes...]
size_t rleBitmapEncode(const uint8_t *bitmap, size_t numBits, uint8_t *output,
                       RLEStats *stats) {
    if (numBits == 0) {
        return 0;
    }

    size_t outPos = 0;
    size_t bitPos = 0;

    if (stats) {
        memset(stats, 0, sizeof(RLEStats));
        stats->originalSize = (numBits + 7) / 8;
    }

    while (bitPos < numBits) {
        // Get current bit
        uint8_t currentBit = (bitmap[bitPos / 8] >> (bitPos % 8)) & 1;
        size_t runLength = 1;

        // Count run
        while (bitPos + runLength < numBits) {
            uint8_t nextBit = (bitmap[(bitPos + runLength) / 8] >>
                               ((bitPos + runLength) % 8)) &
                              1;
            if (nextBit != currentBit) {
                break;
            }
            runLength++;
        }

        // Encode: bit value + width + varint length
        output[outPos++] = currentBit;
        varintWidth width;
        varintExternalUnsignedEncoding(runLength, width);
        output[outPos++] = (uint8_t)width;
        varintExternalPutFixedWidthQuick_(output + outPos, runLength, width);
        outPos += width;

        if (stats) {
            stats->numRuns++;
            if (runLength > stats->longestRun) {
                stats->longestRun = runLength;
            }
        }

        bitPos += runLength;
    }

    if (stats) {
        stats->compressedSize = outPos;
    }

    return outPos;
}

void printCompressionStats(const char *name, const RLEStats *stats) {
    printf("\n%s:\n", name);
    printf("  Original: %zu bytes\n", stats->originalSize);
    printf("  Compressed: %zu bytes\n", stats->compressedSize);
    printf("  Ratio: %.2fx",
           (double)stats->originalSize / stats->compressedSize);
    if (stats->compressedSize > stats->originalSize) {
        printf(" (EXPANSION: %.1f%%)\n",
               ((double)stats->compressedSize / stats->originalSize - 1.0) *
                   100);
    } else {
        printf(" (%.1f%% savings)\n",
               (1.0 - (double)stats->compressedSize / stats->originalSize) *
                   100);
    }
    printf("  Runs: %zu, Literals: %zu, Longest run: %zu\n", stats->numRuns,
           stats->numLiterals, stats->longestRun);
}

void example_simple_runs() {
    printf("\n=== Example 1: Simple Repeated Data ===\n");

    // Data with obvious runs
    uint8_t data[100];

    // Pattern: 20 'A's, 30 'B's, 50 'C's
    memset(data, 'A', 20);
    memset(data + 20, 'B', 30);
    memset(data + 50, 'C', 50);

    uint8_t compressed[200];
    uint8_t decompressed[100];
    RLEStats stats;

    size_t compSize = rleEncode(data, 100, compressed, &stats);
    printCompressionStats("Simple runs (A×20, B×30, C×50)", &stats);

    size_t decompSize = rleDecode(compressed, compSize, decompressed);
    assert(decompSize == 100);
    assert(memcmp(data, decompressed, 100) == 0);
    printf("✓ Round-trip successful\n");
}

void example_sparse_array() {
    printf("\n=== Example 2: Sparse Array (Many Zeros) ===\n");

    uint8_t data[1000];
    memset(data, 0, 1000);

    // Add a few non-zero values
    data[100] = 42;
    data[500] = 255;
    data[501] = 255;
    data[900] = 17;

    uint8_t *compressed = malloc(2000);
    uint8_t *decompressed = malloc(1000);
    if (!compressed || !decompressed) {
        free(compressed);
        free(decompressed);
        printf("Memory allocation failed\n");
        return;
    }
    RLEStats stats;

    size_t compSize = rleEncode(data, 1000, compressed, &stats);
    printCompressionStats("Sparse array (1000 bytes, mostly zeros)", &stats);

    size_t decompSize = rleDecode(compressed, compSize, decompressed);
    assert(decompSize == 1000);
    assert(memcmp(data, decompressed, 1000) == 0);
    printf("✓ Round-trip successful\n");

    free(compressed);
    free(decompressed);
}

void example_bitmap_scanlines() {
    printf("\n=== Example 3: Bitmap Scanlines ===\n");

    // Simulate a 64x8 1-bit bitmap (64 bytes) with horizontal lines
    uint8_t bitmap[64];
    memset(bitmap, 0, 64);

    // Line 0: all white (0s)
    // Line 1: all black (1s)
    memset(bitmap + 8, 0xFF, 8);
    // Line 2: alternating (low compression)
    for (int i = 0; i < 8; i++) {
        bitmap[16 + i] = 0xAA;
    }
    // Line 3-7: various patterns
    memset(bitmap + 24, 0xFF, 8);
    memset(bitmap + 32, 0x00, 8);
    memset(bitmap + 40, 0xFF, 8);
    memset(bitmap + 48, 0x00, 16);

    uint8_t *compressed = malloc(256);
    if (!compressed) {
        printf("Memory allocation failed\n");
        return;
    }
    RLEStats stats;

    rleBitmapEncode(bitmap, 64 * 8, compressed, &stats);
    printCompressionStats("Bitmap (64×8 pixels = 512 bits)", &stats);

    free(compressed);
}

void example_text_repetition() {
    printf("\n=== Example 4: Text with Repetition ===\n");

    const char *text = "AAAAAAA very long run of AAAAAAAAA followed by "
                       "BBBBBBBBBBBBB and then CCCCCCCCCCCCCCCCCC and "
                       "some normal text without much repetition here.";

    size_t textLen = strlen(text);
    uint8_t *compressed = malloc(textLen * 2);
    uint8_t *decompressed = malloc(textLen);
    if (!compressed || !decompressed) {
        free(compressed);
        free(decompressed);
        printf("Memory allocation failed\n");
        return;
    }
    RLEStats stats;

    size_t compSize = rleEncode((uint8_t *)text, textLen, compressed, &stats);
    printCompressionStats("Text with repeated characters", &stats);

    size_t decompSize = rleDecode(compressed, compSize, decompressed);
    assert(decompSize == textLen);
    assert(memcmp(text, decompressed, textLen) == 0);
    printf("✓ Round-trip successful\n");

    free(compressed);
    free(decompressed);
}

void example_random_data() {
    printf("\n=== Example 5: Random Data (Worst Case) ===\n");

    uint8_t data[100];
    // Pseudo-random pattern (no repetition)
    for (int i = 0; i < 100; i++) {
        data[i] = (i * 17 + 23) & 0xFF;
    }

    uint8_t *compressed = malloc(300);
    uint8_t *decompressed = malloc(100);
    if (!compressed || !decompressed) {
        free(compressed);
        free(decompressed);
        printf("Memory allocation failed\n");
        return;
    }
    RLEStats stats;

    size_t compSize = rleEncode(data, 100, compressed, &stats);
    printCompressionStats("Random/non-repeating data", &stats);

    size_t decompSize = rleDecode(compressed, compSize, decompressed);
    assert(decompSize == 100);
    assert(memcmp(data, decompressed, 100) == 0);
    printf("✓ Round-trip successful (despite expansion)\n");

    free(compressed);
    free(decompressed);
}

void example_varint_vs_fixed() {
    printf("\n=== Example 6: Varint vs Fixed-Width Lengths ===\n");

    // Data with varying run lengths
    uint8_t data[1000];
    size_t pos = 0;

    // Short runs (1-2 bytes for varint)
    for (int i = 0; i < 10; i++) {
        memset(data + pos, 'A' + i, 10); // Run of 10
        pos += 10;
    }

    // Medium run (2 bytes for varint)
    memset(data + pos, 'Z', 300);
    pos += 300;

    // Long run (3 bytes for varint)
    memset(data + pos, 'X', 600);
    pos += 600;

    assert(pos == 1000); // Verify we filled the array correctly

    uint8_t *compressed = malloc(2000);
    if (!compressed) {
        printf("Memory allocation failed\n");
        return;
    }
    RLEStats stats;

    size_t varintCompSize = rleEncode(data, 1000, compressed, &stats);

    printf("\nWith VARINT lengths:\n");
    printf("  Compressed size: %zu bytes\n", varintCompSize);
    printf("  Ratio: %.2fx\n", (double)1000 / varintCompSize);

    // Simulate fixed 4-byte lengths
    size_t fixedCompSize =
        stats.numRuns * (1 + 4); // 1 byte value + 4 byte length
    printf("\nWith FIXED 32-bit lengths:\n");
    printf("  Compressed size: %zu bytes\n", fixedCompSize);
    printf("  Ratio: %.2fx\n", (double)1000 / fixedCompSize);

    printf("\nVarint savings vs fixed: %zu bytes (%.1f%%)\n",
           fixedCompSize - varintCompSize,
           ((double)(fixedCompSize - varintCompSize) / fixedCompSize) * 100);

    free(compressed);
}

void example_image_like_data() {
    printf("\n=== Example 7: Image-like Data (Scan Lines) ===\n");

    // Simulate 256×4 8-bit grayscale image with simple patterns
    uint8_t *image = malloc(1024);
    if (!image) {
        printf("Memory allocation failed\n");
        return;
    }

    // Scan line 0: gradient
    for (int i = 0; i < 256; i++) {
        image[i] = i;
    }

    // Scan line 1: solid gray
    memset(image + 256, 128, 256);

    // Scan line 2: black and white stripes (poor compression)
    for (int i = 0; i < 256; i++) {
        image[512 + i] = (i % 2) ? 255 : 0;
    }

    // Scan line 3: solid white
    memset(image + 768, 255, 256);

    uint8_t *compressed = malloc(2048);
    uint8_t *decompressed = malloc(1024);
    if (!compressed || !decompressed) {
        free(image);
        free(compressed);
        free(decompressed);
        printf("Memory allocation failed\n");
        return;
    }
    RLEStats stats;

    size_t compSize = rleEncode(image, 1024, compressed, &stats);
    printCompressionStats("Image-like data (256×4 pixels)", &stats);

    size_t decompSize = rleDecode(compressed, compSize, decompressed);
    assert(decompSize == 1024);
    assert(memcmp(image, decompressed, 1024) == 0);
    printf("✓ Round-trip successful\n");

    free(image);
    free(compressed);
    free(decompressed);
}

void example_extreme_compression() {
    printf("\n=== Example 8: Extreme Compression ===\n");

    // 10KB of same byte
    size_t size = 10240;
    uint8_t *data = malloc(size);
    if (!data) {
        printf("Memory allocation failed\n");
        return;
    }
    memset(data, 'X', size);

    uint8_t *compressed = malloc(100);
    uint8_t *decompressed = malloc(size);
    if (!compressed || !decompressed) {
        free(data);
        free(compressed);
        free(decompressed);
        printf("Memory allocation failed\n");
        return;
    }
    RLEStats stats;

    size_t compSize = rleEncode(data, size, compressed, &stats);
    printCompressionStats("Extreme: 10KB of same byte", &stats);

    printf("  Bytes used for length encoding: %zu bytes\n", compSize - 1);
    printf("  Compression ratio: %.1fx\n", (double)size / compSize);

    size_t decompSize = rleDecode(compressed, compSize, decompressed);
    assert(decompSize == size);
    assert(memcmp(data, decompressed, size) == 0);
    printf("✓ Round-trip successful\n");

    free(data);
    free(compressed);
    free(decompressed);
}

int main() {
    printf("===========================================\n");
    printf("   RLE Codec with Varint Lengths\n");
    printf("===========================================\n");

    example_simple_runs();
    example_sparse_array();
    example_bitmap_scanlines();
    example_text_repetition();
    example_random_data();
    example_varint_vs_fixed();
    example_image_like_data();
    example_extreme_compression();

    printf("\n===========================================\n");
    printf("Key Insights:\n");
    printf("===========================================\n");
    printf("1. RLE excels on data with long runs (>10x compression)\n");
    printf("2. Varint lengths save space vs fixed-width (30-50%%)\n");
    printf("3. Random data causes expansion (literal overhead)\n");
    printf("4. Hybrid approach (runs + literals) handles mixed data\n");
    printf("5. Bitmap RLE can achieve extreme compression ratios\n");
    printf("\n===========================================\n");
    printf("All examples completed successfully!\n");
    printf("===========================================\n");

    return 0;
}
