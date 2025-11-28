/* Struct Memory Efficiency Analyzer
 * Analyzes all structs in the varint library for padding inefficiencies
 * and provides optimization recommendations.
 *
 * Compile: gcc -I../src -o struct_analyzer struct_analyzer.c
 * Run: ./struct_analyzer
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Include all headers to get struct definitions */
#include "perf.h"
#include "varint.h"
#include "varintAdaptive.h"
#include "varintBitmap.h"
#include "varintDelta.h"
#include "varintDict.h"
#include "varintExternal.h"
#include "varintFOR.h"
#include "varintFloat.h"
#include "varintGroup.h"
#include "varintPFOR.h"
#include "varintTagged.h"

/* Analysis result structure */
typedef struct StructAnalysis {
    const char *name;
    size_t totalSize;
    size_t usefulSize;  /* Sum of field sizes */
    size_t paddingSize; /* Total padding bytes */
    float efficiency;   /* usefulSize / totalSize * 100 */
    int fieldCount;
    const char *recommendation;
} StructAnalysis;

/* Color codes for terminal output */
#define COLOR_RED "\033[1;31m"
#define COLOR_YELLOW "\033[1;33m"
#define COLOR_GREEN "\033[1;32m"
#define COLOR_BLUE "\033[1;34m"
#define COLOR_RESET "\033[0m"

/* Helper macros for field analysis */
#define FIELD_SIZE(type, field) sizeof(((type *)0)->field)
#define FIELD_OFFSET(type, field) offsetof(type, field)

/* Print field layout for a struct */
static void print_field_layout(const char *struct_name,
                               const char **field_names, size_t *field_offsets,
                               size_t *field_sizes, int field_count,
                               size_t total_size) {
    printf("\n  Field Layout:\n");
    printf("  %-4s %-20s %-8s %-8s %-10s\n", "Idx", "Field", "Offset", "Size",
           "Padding");
    printf("  %s\n",
           "------------------------------------------------------------");

    for (int i = 0; i < field_count; i++) {
        size_t next_offset =
            (i < field_count - 1) ? field_offsets[i + 1] : total_size;
        size_t expected_next = field_offsets[i] + field_sizes[i];
        size_t padding = next_offset - expected_next;

        const char *color = padding > 0 ? COLOR_YELLOW : COLOR_GREEN;
        printf("  %s%-4d %-20s %-8zu %-8zu %-10zu%s\n", color, i,
               field_names[i], field_offsets[i], field_sizes[i], padding,
               COLOR_RESET);
    }
}

/* Analyze varintFORMeta */
static StructAnalysis analyze_varintFORMeta(void) {
    StructAnalysis result = {0};
    result.name = "varintFORMeta";
    result.totalSize = sizeof(varintFORMeta);

    const char *fields[] = {"minValue",    "maxValue", "range",
                            "offsetWidth", "count",    "encodedSize"};
    size_t offsets[] = {FIELD_OFFSET(varintFORMeta, minValue),
                        FIELD_OFFSET(varintFORMeta, maxValue),
                        FIELD_OFFSET(varintFORMeta, range),
                        FIELD_OFFSET(varintFORMeta, offsetWidth),
                        FIELD_OFFSET(varintFORMeta, count),
                        FIELD_OFFSET(varintFORMeta, encodedSize)};
    size_t sizes[] = {FIELD_SIZE(varintFORMeta, minValue),
                      FIELD_SIZE(varintFORMeta, maxValue),
                      FIELD_SIZE(varintFORMeta, range),
                      FIELD_SIZE(varintFORMeta, offsetWidth),
                      FIELD_SIZE(varintFORMeta, count),
                      FIELD_SIZE(varintFORMeta, encodedSize)};

    result.fieldCount = 6;
    for (int i = 0; i < result.fieldCount; i++) {
        result.usefulSize += sizes[i];
    }
    result.paddingSize = result.totalSize - result.usefulSize;
    result.efficiency = (float)result.usefulSize / result.totalSize * 100.0f;

    /* Check if offsetWidth (1 byte) is causing alignment issues */
    if (offsets[3] % 8 != 0 && sizes[3] < 8) {
        result.recommendation =
            "Move offsetWidth to end (1-byte field causes 7 bytes padding)";
    } else {
        result.recommendation = "Optimal";
    }

    print_field_layout(result.name, fields, offsets, sizes, result.fieldCount,
                       result.totalSize);

    return result;
}

/* Analyze varintPFORMeta */
static StructAnalysis analyze_varintPFORMeta(void) {
    StructAnalysis result = {0};
    result.name = "varintPFORMeta";
    result.totalSize = sizeof(varintPFORMeta);

    const char *fields[] = {"min",
                            "width",
                            "count",
                            "exceptionCount",
                            "exceptionMarker",
                            "threshold",
                            "thresholdValue"};
    size_t offsets[] = {FIELD_OFFSET(varintPFORMeta, min),
                        FIELD_OFFSET(varintPFORMeta, width),
                        FIELD_OFFSET(varintPFORMeta, count),
                        FIELD_OFFSET(varintPFORMeta, exceptionCount),
                        FIELD_OFFSET(varintPFORMeta, exceptionMarker),
                        FIELD_OFFSET(varintPFORMeta, threshold),
                        FIELD_OFFSET(varintPFORMeta, thresholdValue)};
    size_t sizes[] = {FIELD_SIZE(varintPFORMeta, min),
                      FIELD_SIZE(varintPFORMeta, width),
                      FIELD_SIZE(varintPFORMeta, count),
                      FIELD_SIZE(varintPFORMeta, exceptionCount),
                      FIELD_SIZE(varintPFORMeta, exceptionMarker),
                      FIELD_SIZE(varintPFORMeta, threshold),
                      FIELD_SIZE(varintPFORMeta, thresholdValue)};

    result.fieldCount = 7;
    for (int i = 0; i < result.fieldCount; i++) {
        result.usefulSize += sizes[i];
    }
    result.paddingSize = result.totalSize - result.usefulSize;
    result.efficiency = (float)result.usefulSize / result.totalSize * 100.0f;

    if (offsets[1] % 8 != 0 && sizes[1] < 8) {
        result.recommendation =
            "Move width (1-byte) and uint32_t fields to end to reduce padding";
    } else {
        result.recommendation = "Optimal";
    }

    print_field_layout(result.name, fields, offsets, sizes, result.fieldCount,
                       result.totalSize);

    return result;
}

/* Analyze varintFloatMeta */
static StructAnalysis analyze_varintFloatMeta(void) {
    StructAnalysis result = {0};
    result.name = "varintFloatMeta";
    result.totalSize = sizeof(varintFloatMeta);

    const char *fields[] = {
        "precision", "mode",        "exponentBits", "mantissaBits",
        "count",     "encodedSize", "specialCount", "maxRelativeError"};
    size_t offsets[] = {FIELD_OFFSET(varintFloatMeta, precision),
                        FIELD_OFFSET(varintFloatMeta, mode),
                        FIELD_OFFSET(varintFloatMeta, exponentBits),
                        FIELD_OFFSET(varintFloatMeta, mantissaBits),
                        FIELD_OFFSET(varintFloatMeta, count),
                        FIELD_OFFSET(varintFloatMeta, encodedSize),
                        FIELD_OFFSET(varintFloatMeta, specialCount),
                        FIELD_OFFSET(varintFloatMeta, maxRelativeError)};
    size_t sizes[] = {FIELD_SIZE(varintFloatMeta, precision),
                      FIELD_SIZE(varintFloatMeta, mode),
                      FIELD_SIZE(varintFloatMeta, exponentBits),
                      FIELD_SIZE(varintFloatMeta, mantissaBits),
                      FIELD_SIZE(varintFloatMeta, count),
                      FIELD_SIZE(varintFloatMeta, encodedSize),
                      FIELD_SIZE(varintFloatMeta, specialCount),
                      FIELD_SIZE(varintFloatMeta, maxRelativeError)};

    result.fieldCount = 8;
    for (int i = 0; i < result.fieldCount; i++) {
        result.usefulSize += sizes[i];
    }
    result.paddingSize = result.totalSize - result.usefulSize;
    result.efficiency = (float)result.usefulSize / result.totalSize * 100.0f;

    if (result.paddingSize > 8) {
        result.recommendation = "Move small fields (precision, mode, "
                                "exponentBits, mantissaBits) to end";
    } else {
        result.recommendation = "Optimal";
    }

    print_field_layout(result.name, fields, offsets, sizes, result.fieldCount,
                       result.totalSize);

    return result;
}

/* Analyze varintAdaptiveDataStats */
static StructAnalysis analyze_varintAdaptiveDataStats(void) {
    StructAnalysis result = {0};
    result.name = "varintAdaptiveDataStats";
    result.totalSize = sizeof(varintAdaptiveDataStats);

    const char *fields[] = {"count",
                            "minValue",
                            "maxValue",
                            "range",
                            "uniqueCount",
                            "uniqueRatio",
                            "isSorted",
                            "isReverseSorted",
                            "avgDelta",
                            "maxDelta",
                            "outlierCount",
                            "outlierRatio",
                            "fitsInBitmapRange"};
    size_t offsets[] = {
        FIELD_OFFSET(varintAdaptiveDataStats, count),
        FIELD_OFFSET(varintAdaptiveDataStats, minValue),
        FIELD_OFFSET(varintAdaptiveDataStats, maxValue),
        FIELD_OFFSET(varintAdaptiveDataStats, range),
        FIELD_OFFSET(varintAdaptiveDataStats, uniqueCount),
        FIELD_OFFSET(varintAdaptiveDataStats, uniqueRatio),
        FIELD_OFFSET(varintAdaptiveDataStats, isSorted),
        FIELD_OFFSET(varintAdaptiveDataStats, isReverseSorted),
        FIELD_OFFSET(varintAdaptiveDataStats, avgDelta),
        FIELD_OFFSET(varintAdaptiveDataStats, maxDelta),
        FIELD_OFFSET(varintAdaptiveDataStats, outlierCount),
        FIELD_OFFSET(varintAdaptiveDataStats, outlierRatio),
        FIELD_OFFSET(varintAdaptiveDataStats, fitsInBitmapRange)};
    size_t sizes[] = {FIELD_SIZE(varintAdaptiveDataStats, count),
                      FIELD_SIZE(varintAdaptiveDataStats, minValue),
                      FIELD_SIZE(varintAdaptiveDataStats, maxValue),
                      FIELD_SIZE(varintAdaptiveDataStats, range),
                      FIELD_SIZE(varintAdaptiveDataStats, uniqueCount),
                      FIELD_SIZE(varintAdaptiveDataStats, uniqueRatio),
                      FIELD_SIZE(varintAdaptiveDataStats, isSorted),
                      FIELD_SIZE(varintAdaptiveDataStats, isReverseSorted),
                      FIELD_SIZE(varintAdaptiveDataStats, avgDelta),
                      FIELD_SIZE(varintAdaptiveDataStats, maxDelta),
                      FIELD_SIZE(varintAdaptiveDataStats, outlierCount),
                      FIELD_SIZE(varintAdaptiveDataStats, outlierRatio),
                      FIELD_SIZE(varintAdaptiveDataStats, fitsInBitmapRange)};

    result.fieldCount = 13;
    for (int i = 0; i < result.fieldCount; i++) {
        result.usefulSize += sizes[i];
    }
    result.paddingSize = result.totalSize - result.usefulSize;
    result.efficiency = (float)result.usefulSize / result.totalSize * 100.0f;

    if (result.paddingSize > 8) {
        result.recommendation = "Move bool fields (isSorted, isReverseSorted, "
                                "fitsInBitmapRange) to end";
    } else {
        result.recommendation = "Optimal";
    }

    print_field_layout(result.name, fields, offsets, sizes, result.fieldCount,
                       result.totalSize);

    return result;
}

/* Analyze varintAdaptiveMeta */
static StructAnalysis analyze_varintAdaptiveMeta(void) {
    StructAnalysis result = {0};
    result.name = "varintAdaptiveMeta";
    result.totalSize = sizeof(varintAdaptiveMeta);

    const char *fields[] = {"encodingType", "originalCount", "encodedSize",
                            "encodingMeta"};
    size_t offsets[] = {FIELD_OFFSET(varintAdaptiveMeta, encodingType),
                        FIELD_OFFSET(varintAdaptiveMeta, originalCount),
                        FIELD_OFFSET(varintAdaptiveMeta, encodedSize),
                        FIELD_OFFSET(varintAdaptiveMeta, encodingMeta)};
    size_t sizes[] = {FIELD_SIZE(varintAdaptiveMeta, encodingType),
                      FIELD_SIZE(varintAdaptiveMeta, originalCount),
                      FIELD_SIZE(varintAdaptiveMeta, encodedSize),
                      FIELD_SIZE(varintAdaptiveMeta, encodingMeta)};

    result.fieldCount = 4;
    for (int i = 0; i < result.fieldCount; i++) {
        result.usefulSize += sizes[i];
    }
    result.paddingSize = result.totalSize - result.usefulSize;
    result.efficiency = (float)result.usefulSize / result.totalSize * 100.0f;

    if (offsets[0] % 8 != 0 && sizes[0] < 8) {
        result.recommendation =
            "Move encodingType (enum, 4 bytes) after size_t fields";
    } else {
        result.recommendation = "Optimal";
    }

    print_field_layout(result.name, fields, offsets, sizes, result.fieldCount,
                       result.totalSize);

    return result;
}

/* Analyze varintDictStats */
static StructAnalysis analyze_varintDictStats(void) {
    StructAnalysis result = {0};
    result.name = "varintDictStats";
    result.totalSize = sizeof(varintDictStats);

    const char *fields[] = {
        "uniqueCount", "totalCount",    "dictBytes",        "indexBytes",
        "totalBytes",  "originalBytes", "compressionRatio", "spaceReduction"};
    size_t offsets[] = {FIELD_OFFSET(varintDictStats, uniqueCount),
                        FIELD_OFFSET(varintDictStats, totalCount),
                        FIELD_OFFSET(varintDictStats, dictBytes),
                        FIELD_OFFSET(varintDictStats, indexBytes),
                        FIELD_OFFSET(varintDictStats, totalBytes),
                        FIELD_OFFSET(varintDictStats, originalBytes),
                        FIELD_OFFSET(varintDictStats, compressionRatio),
                        FIELD_OFFSET(varintDictStats, spaceReduction)};
    size_t sizes[] = {FIELD_SIZE(varintDictStats, uniqueCount),
                      FIELD_SIZE(varintDictStats, totalCount),
                      FIELD_SIZE(varintDictStats, dictBytes),
                      FIELD_SIZE(varintDictStats, indexBytes),
                      FIELD_SIZE(varintDictStats, totalBytes),
                      FIELD_SIZE(varintDictStats, originalBytes),
                      FIELD_SIZE(varintDictStats, compressionRatio),
                      FIELD_SIZE(varintDictStats, spaceReduction)};

    result.fieldCount = 8;
    for (int i = 0; i < result.fieldCount; i++) {
        result.usefulSize += sizes[i];
    }
    result.paddingSize = result.totalSize - result.usefulSize;
    result.efficiency = (float)result.usefulSize / result.totalSize * 100.0f;

    /* float fields at end may cause 4 bytes padding */
    if (result.paddingSize >= 4) {
        result.recommendation =
            "Move float fields to end together to minimize padding";
    } else {
        result.recommendation = "Optimal";
    }

    print_field_layout(result.name, fields, offsets, sizes, result.fieldCount,
                       result.totalSize);

    return result;
}

/* Analyze varintBitmapStats */
static StructAnalysis analyze_varintBitmapStats(void) {
    StructAnalysis result = {0};
    result.name = "varintBitmapStats";
    result.totalSize = sizeof(varintBitmapStats);

    const char *fields[] = {"type", "cardinality", "sizeBytes",
                            "containerCapacity"};
    size_t offsets[] = {FIELD_OFFSET(varintBitmapStats, type),
                        FIELD_OFFSET(varintBitmapStats, cardinality),
                        FIELD_OFFSET(varintBitmapStats, sizeBytes),
                        FIELD_OFFSET(varintBitmapStats, containerCapacity)};
    size_t sizes[] = {FIELD_SIZE(varintBitmapStats, type),
                      FIELD_SIZE(varintBitmapStats, cardinality),
                      FIELD_SIZE(varintBitmapStats, sizeBytes),
                      FIELD_SIZE(varintBitmapStats, containerCapacity)};

    result.fieldCount = 4;
    for (int i = 0; i < result.fieldCount; i++) {
        result.usefulSize += sizes[i];
    }
    result.paddingSize = result.totalSize - result.usefulSize;
    result.efficiency = (float)result.usefulSize / result.totalSize * 100.0f;

    if (result.paddingSize > 0) {
        result.recommendation = "Move type (enum) to end to eliminate padding";
    } else {
        result.recommendation = "Optimal";
    }

    print_field_layout(result.name, fields, offsets, sizes, result.fieldCount,
                       result.totalSize);

    return result;
}

/* Print summary report */
static void print_summary(StructAnalysis *results, int count) {
    printf("\n");
    printf("==================================================================="
           "=============\n");
    printf("                    STRUCT MEMORY EFFICIENCY SUMMARY\n");
    printf("==================================================================="
           "=============\n\n");

    size_t total_size = 0;
    size_t total_padding = 0;
    int inefficient_count = 0;

    printf("%-30s %8s %8s %8s %10s\n", "Struct Name", "Size", "Useful",
           "Padding", "Efficiency");
    printf("-------------------------------------------------------------------"
           "-------------\n");

    for (int i = 0; i < count; i++) {
        const char *color;
        if (results[i].efficiency >= 95.0f) {
            color = COLOR_GREEN;
        } else if (results[i].efficiency >= 85.0f) {
            color = COLOR_YELLOW;
        } else {
            color = COLOR_RED;
            inefficient_count++;
        }

        printf("%s%-30s %8zu %8zu %8zu %9.1f%%%s\n", color, results[i].name,
               results[i].totalSize, results[i].usefulSize,
               results[i].paddingSize, results[i].efficiency, COLOR_RESET);

        total_size += results[i].totalSize;
        total_padding += results[i].paddingSize;
    }

    printf("-------------------------------------------------------------------"
           "-------------\n");
    float overall_efficiency =
        (float)(total_size - total_padding) / total_size * 100.0f;
    printf("%-30s %8zu %8zu %8zu %9.1f%%\n", "TOTALS", total_size,
           total_size - total_padding, total_padding, overall_efficiency);

    printf("\n");
    printf("Summary:\n");
    printf("  Total Structs Analyzed: %d\n", count);
    printf("  Structs Needing Optimization: %s%d%s\n",
           inefficient_count > 0 ? COLOR_RED : COLOR_GREEN, inefficient_count,
           COLOR_RESET);
    printf("  Total Memory Wasted: %s%zu bytes%s\n",
           total_padding > 0 ? COLOR_YELLOW : COLOR_GREEN, total_padding,
           COLOR_RESET);
    printf("  Overall Efficiency: %s%.1f%%%s\n",
           overall_efficiency >= 90.0f ? COLOR_GREEN : COLOR_YELLOW,
           overall_efficiency, COLOR_RESET);

    printf("\n");
    printf("Optimization Recommendations:\n");
    printf("-------------------------------------------------------------------"
           "-------------\n");
    for (int i = 0; i < count; i++) {
        if (strcmp(results[i].recommendation, "Optimal") != 0) {
            printf("%s%-30s%s: %s\n", COLOR_YELLOW, results[i].name,
                   COLOR_RESET, results[i].recommendation);
        }
    }
}

int main(void) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════"
           "══════════╗\n");
    printf("║                  Varint Library Struct Memory Analyzer           "
           "         ║\n");
    printf("║                    Compiler Introspection & Optimization         "
           "         ║\n");
    printf("╚══════════════════════════════════════════════════════════════════"
           "══════════╝\n");

    /* Array to store all analysis results */
    StructAnalysis results[20];
    int result_count = 0;

    /* Analyze each struct */
    printf("\n%s[1/7] Analyzing varintFORMeta...%s\n", COLOR_BLUE, COLOR_RESET);
    results[result_count++] = analyze_varintFORMeta();

    printf("\n%s[2/7] Analyzing varintPFORMeta...%s\n", COLOR_BLUE,
           COLOR_RESET);
    results[result_count++] = analyze_varintPFORMeta();

    printf("\n%s[3/7] Analyzing varintFloatMeta...%s\n", COLOR_BLUE,
           COLOR_RESET);
    results[result_count++] = analyze_varintFloatMeta();

    printf("\n%s[4/7] Analyzing varintAdaptiveDataStats...%s\n", COLOR_BLUE,
           COLOR_RESET);
    results[result_count++] = analyze_varintAdaptiveDataStats();

    printf("\n%s[5/7] Analyzing varintAdaptiveMeta...%s\n", COLOR_BLUE,
           COLOR_RESET);
    results[result_count++] = analyze_varintAdaptiveMeta();

    printf("\n%s[6/7] Analyzing varintDictStats...%s\n", COLOR_BLUE,
           COLOR_RESET);
    results[result_count++] = analyze_varintDictStats();

    printf("\n%s[7/7] Analyzing varintBitmapStats...%s\n", COLOR_BLUE,
           COLOR_RESET);
    results[result_count++] = analyze_varintBitmapStats();

    /* Print comprehensive summary */
    print_summary(results, result_count);

    printf("\n");
    printf("To apply optimizations, review recommendations and reorder struct "
           "fields\n");
    printf("from largest to smallest alignment requirements (8-byte → 4-byte → "
           "2-byte → 1-byte).\n");
    printf("\n");

    return 0;
}
