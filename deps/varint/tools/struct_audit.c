/* Comprehensive Struct Memory Audit Tool
 * Automatically analyzes ALL structs in the varint library using compiler
 * introspection. This tool uses actual compiler layout information via sizeof()
 * and offsetof().
 *
 * Compile with debug symbols: gcc -g -I../src -o struct_audit struct_audit.c
 * Use with pahole (if available): pahole ./struct_audit
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Include all headers to get all struct definitions */
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

/* Terminal colors */
#define C_RED "\033[1;31m"
#define C_YELLOW "\033[1;33m"
#define C_GREEN "\033[1;32m"
#define C_BLUE "\033[1;34m"
#define C_CYAN "\033[1;36m"
#define C_MAGENTA "\033[1;35m"
#define C_RESET "\033[0m"
#define C_BOLD "\033[1m"

/* Struct info for reporting */
typedef struct {
    const char *name;
    const char *file;
    size_t size;
    size_t align;
    int analyzed; /* 1 if we have detailed field analysis */
} StructInfo;

/* Field layout information */
typedef struct {
    const char *name;
    const char *type;
    size_t offset;
    size_t size;
    size_t padding_after;
} FieldInfo;

/* Macro to register a struct */
#define REGISTER_STRUCT(name, file)                                            \
    {#name, file, sizeof(name), _Alignof(name), 0}

/* Macro to analyze field padding */
#define ANALYZE_FIELD(struct_type, field_name, field_type, next_offset,        \
                      struct_size)                                             \
    {#field_name, #field_type, offsetof(struct_type, field_name),              \
     sizeof(((struct_type *)0)->field_name),                                   \
     (next_offset) - (offsetof(struct_type, field_name) +                      \
                      sizeof(((struct_type *)0)->field_name))}

/* Calculate total padding in a struct */
static size_t calculate_padding(const FieldInfo *fields, int field_count,
                                size_t total_size) {
    size_t useful = 0;
    for (int i = 0; i < field_count; i++) {
        useful += fields[i].size;
    }
    return total_size - useful;
}

/* Print detailed field layout */
static void print_field_layout(const char *struct_name, const FieldInfo *fields,
                               int field_count, size_t total_size) {
    printf("\n  %sDetailed Field Layout:%s\n", C_BOLD, C_RESET);
    printf("  "
           "┌────┬───────────────────────┬───────────────────┬────────┬────────"
           "┬─────────┐\n");
    printf("  │ ## │ %-21s │ %-17s │ Offset │   Size │ Padding │\n",
           "Field Name", "Type");
    printf("  "
           "├────┼───────────────────────┼───────────────────┼────────┼────────"
           "┼─────────┤\n");

    size_t total_padding = 0;
    for (int i = 0; i < field_count; i++) {
        const char *color = fields[i].padding_after > 0 ? C_YELLOW : C_GREEN;
        printf("  │ %s%2d%s │ %-21s │ %-17s │ %6zu │ %6zu │ %s%7zu%s │\n",
               C_CYAN, i, C_RESET, fields[i].name, fields[i].type,
               fields[i].offset, fields[i].size, color, fields[i].padding_after,
               C_RESET);
        total_padding += fields[i].padding_after;
    }

    printf("  "
           "└────┴───────────────────────┴───────────────────┴────────┴────────"
           "┴─────────┘\n");

    /* Summary stats */
    size_t useful_size = total_size - total_padding;
    float efficiency = (float)useful_size / total_size * 100.0f;

    const char *eff_color = efficiency >= 95.0f
                                ? C_GREEN
                                : (efficiency >= 85.0f ? C_YELLOW : C_RED);
    printf("  Total Size: %zu bytes | Useful: %zu bytes | Padding: %s%zu "
           "bytes%s | Efficiency: %s%.1f%%%s\n",
           total_size, useful_size, C_YELLOW, total_padding, C_RESET, eff_color,
           efficiency, C_RESET);
}

/* Suggest optimal field ordering */
static void suggest_optimization(const char *struct_name,
                                 const FieldInfo *fields, int field_count,
                                 size_t total_size) {
    /* Count fields by size */
    int count_8 = 0, count_4 = 0, count_2 = 0, count_1 = 0, count_other = 0;
    for (int i = 0; i < field_count; i++) {
        switch (fields[i].size) {
        case 8:
            count_8++;
            break;
        case 4:
            count_4++;
            break;
        case 2:
            count_2++;
            break;
        case 1:
            count_1++;
            break;
        default:
            count_other++;
            break;
        }
    }

    size_t total_padding = calculate_padding(fields, field_count, total_size);
    if (total_padding == 0) {
        printf("  %s✓ Already optimal!%s No padding detected.\n", C_GREEN,
               C_RESET);
        return;
    }

    printf("\n  %sOptimization Recommendation:%s\n", C_BOLD, C_RESET);
    printf("  Current padding: %s%zu bytes%s\n", C_YELLOW, total_padding,
           C_RESET);
    printf("  \n");
    printf("  Optimal field ordering (largest to smallest alignment):\n");
    printf("    1. Place %s%d x 8-byte%s fields first (uint64_t, double, "
           "pointers on 64-bit)\n",
           C_CYAN, count_8 + count_other, C_RESET);
    printf("    2. Then %s%d x 4-byte%s fields (uint32_t, float, int)\n",
           C_CYAN, count_4, C_RESET);
    printf("    3. Then %s%d x 2-byte%s fields (uint16_t, short)\n", C_CYAN,
           count_2, C_RESET);
    printf("    4. Finally %s%d x 1-byte%s fields (uint8_t, char, bool)\n",
           C_CYAN, count_1, C_RESET);

    /* Identify problem fields */
    printf("\n  Fields causing padding:\n");
    for (int i = 0; i < field_count; i++) {
        if (fields[i].padding_after > 0) {
            printf("    • %s%-20s%s (%zu bytes) → %s%zu bytes padding%s\n",
                   C_YELLOW, fields[i].name, C_RESET, fields[i].size, C_RED,
                   fields[i].padding_after, C_RESET);
        }
    }
}

/* Analyze varintFORMeta */
static void analyze_varintFORMeta(void) {
    FieldInfo fields[] = {
        ANALYZE_FIELD(varintFORMeta, minValue, uint64_t,
                      offsetof(varintFORMeta, maxValue), sizeof(varintFORMeta)),
        ANALYZE_FIELD(varintFORMeta, maxValue, uint64_t,
                      offsetof(varintFORMeta, range), sizeof(varintFORMeta)),
        ANALYZE_FIELD(varintFORMeta, range, uint64_t,
                      offsetof(varintFORMeta, offsetWidth),
                      sizeof(varintFORMeta)),
        ANALYZE_FIELD(varintFORMeta, offsetWidth, varintWidth,
                      offsetof(varintFORMeta, count), sizeof(varintFORMeta)),
        ANALYZE_FIELD(varintFORMeta, count, size_t,
                      offsetof(varintFORMeta, encodedSize),
                      sizeof(varintFORMeta)),
        ANALYZE_FIELD(varintFORMeta, encodedSize, size_t, sizeof(varintFORMeta),
                      sizeof(varintFORMeta))};
    int field_count = sizeof(fields) / sizeof(fields[0]);

    print_field_layout("varintFORMeta", fields, field_count,
                       sizeof(varintFORMeta));
    suggest_optimization("varintFORMeta", fields, field_count,
                         sizeof(varintFORMeta));
}

/* Analyze varintPFORMeta */
static void analyze_varintPFORMeta(void) {
    FieldInfo fields[] = {
        ANALYZE_FIELD(varintPFORMeta, min, uint64_t,
                      offsetof(varintPFORMeta, width), sizeof(varintPFORMeta)),
        ANALYZE_FIELD(varintPFORMeta, width, varintWidth,
                      offsetof(varintPFORMeta, count), sizeof(varintPFORMeta)),
        ANALYZE_FIELD(varintPFORMeta, count, uint32_t,
                      offsetof(varintPFORMeta, exceptionCount),
                      sizeof(varintPFORMeta)),
        ANALYZE_FIELD(varintPFORMeta, exceptionCount, uint32_t,
                      offsetof(varintPFORMeta, exceptionMarker),
                      sizeof(varintPFORMeta)),
        ANALYZE_FIELD(varintPFORMeta, exceptionMarker, uint64_t,
                      offsetof(varintPFORMeta, threshold),
                      sizeof(varintPFORMeta)),
        ANALYZE_FIELD(varintPFORMeta, threshold, uint32_t,
                      offsetof(varintPFORMeta, thresholdValue),
                      sizeof(varintPFORMeta)),
        ANALYZE_FIELD(varintPFORMeta, thresholdValue, uint64_t,
                      sizeof(varintPFORMeta), sizeof(varintPFORMeta))};
    int field_count = sizeof(fields) / sizeof(fields[0]);

    print_field_layout("varintPFORMeta", fields, field_count,
                       sizeof(varintPFORMeta));
    suggest_optimization("varintPFORMeta", fields, field_count,
                         sizeof(varintPFORMeta));
}

/* Analyze varintFloatMeta */
static void analyze_varintFloatMeta(void) {
    FieldInfo fields[] = {
        ANALYZE_FIELD(varintFloatMeta, precision, varintFloatPrecision,
                      offsetof(varintFloatMeta, mode), sizeof(varintFloatMeta)),
        ANALYZE_FIELD(varintFloatMeta, mode, varintFloatEncodingMode,
                      offsetof(varintFloatMeta, exponentBits),
                      sizeof(varintFloatMeta)),
        ANALYZE_FIELD(varintFloatMeta, exponentBits, uint8_t,
                      offsetof(varintFloatMeta, mantissaBits),
                      sizeof(varintFloatMeta)),
        ANALYZE_FIELD(varintFloatMeta, mantissaBits, uint8_t,
                      offsetof(varintFloatMeta, count),
                      sizeof(varintFloatMeta)),
        ANALYZE_FIELD(varintFloatMeta, count, size_t,
                      offsetof(varintFloatMeta, encodedSize),
                      sizeof(varintFloatMeta)),
        ANALYZE_FIELD(varintFloatMeta, encodedSize, size_t,
                      offsetof(varintFloatMeta, specialCount),
                      sizeof(varintFloatMeta)),
        ANALYZE_FIELD(varintFloatMeta, specialCount, size_t,
                      offsetof(varintFloatMeta, maxRelativeError),
                      sizeof(varintFloatMeta)),
        ANALYZE_FIELD(varintFloatMeta, maxRelativeError, double,
                      sizeof(varintFloatMeta), sizeof(varintFloatMeta))};
    int field_count = sizeof(fields) / sizeof(fields[0]);

    print_field_layout("varintFloatMeta", fields, field_count,
                       sizeof(varintFloatMeta));
    suggest_optimization("varintFloatMeta", fields, field_count,
                         sizeof(varintFloatMeta));
}

/* Analyze varintAdaptiveDataStats */
static void analyze_varintAdaptiveDataStats(void) {
    FieldInfo fields[] = {
        ANALYZE_FIELD(varintAdaptiveDataStats, count, size_t,
                      offsetof(varintAdaptiveDataStats, minValue),
                      sizeof(varintAdaptiveDataStats)),
        ANALYZE_FIELD(varintAdaptiveDataStats, minValue, uint64_t,
                      offsetof(varintAdaptiveDataStats, maxValue),
                      sizeof(varintAdaptiveDataStats)),
        ANALYZE_FIELD(varintAdaptiveDataStats, maxValue, uint64_t,
                      offsetof(varintAdaptiveDataStats, range),
                      sizeof(varintAdaptiveDataStats)),
        ANALYZE_FIELD(varintAdaptiveDataStats, range, uint64_t,
                      offsetof(varintAdaptiveDataStats, uniqueCount),
                      sizeof(varintAdaptiveDataStats)),
        ANALYZE_FIELD(varintAdaptiveDataStats, uniqueCount, size_t,
                      offsetof(varintAdaptiveDataStats, uniqueRatio),
                      sizeof(varintAdaptiveDataStats)),
        ANALYZE_FIELD(varintAdaptiveDataStats, uniqueRatio, float,
                      offsetof(varintAdaptiveDataStats, isSorted),
                      sizeof(varintAdaptiveDataStats)),
        ANALYZE_FIELD(varintAdaptiveDataStats, isSorted, bool,
                      offsetof(varintAdaptiveDataStats, isReverseSorted),
                      sizeof(varintAdaptiveDataStats)),
        ANALYZE_FIELD(varintAdaptiveDataStats, isReverseSorted, bool,
                      offsetof(varintAdaptiveDataStats, avgDelta),
                      sizeof(varintAdaptiveDataStats)),
        ANALYZE_FIELD(varintAdaptiveDataStats, avgDelta, uint64_t,
                      offsetof(varintAdaptiveDataStats, maxDelta),
                      sizeof(varintAdaptiveDataStats)),
        ANALYZE_FIELD(varintAdaptiveDataStats, maxDelta, uint64_t,
                      offsetof(varintAdaptiveDataStats, outlierCount),
                      sizeof(varintAdaptiveDataStats)),
        ANALYZE_FIELD(varintAdaptiveDataStats, outlierCount, size_t,
                      offsetof(varintAdaptiveDataStats, outlierRatio),
                      sizeof(varintAdaptiveDataStats)),
        ANALYZE_FIELD(varintAdaptiveDataStats, outlierRatio, float,
                      offsetof(varintAdaptiveDataStats, fitsInBitmapRange),
                      sizeof(varintAdaptiveDataStats)),
        ANALYZE_FIELD(varintAdaptiveDataStats, fitsInBitmapRange, bool,
                      sizeof(varintAdaptiveDataStats),
                      sizeof(varintAdaptiveDataStats))};
    int field_count = sizeof(fields) / sizeof(fields[0]);

    print_field_layout("varintAdaptiveDataStats", fields, field_count,
                       sizeof(varintAdaptiveDataStats));
    suggest_optimization("varintAdaptiveDataStats", fields, field_count,
                         sizeof(varintAdaptiveDataStats));
}

/* Analyze varintAdaptiveMeta */
static void analyze_varintAdaptiveMeta(void) {
    FieldInfo fields[] = {
        ANALYZE_FIELD(varintAdaptiveMeta, encodingType,
                      varintAdaptiveEncodingType,
                      offsetof(varintAdaptiveMeta, originalCount),
                      sizeof(varintAdaptiveMeta)),
        ANALYZE_FIELD(varintAdaptiveMeta, originalCount, size_t,
                      offsetof(varintAdaptiveMeta, encodedSize),
                      sizeof(varintAdaptiveMeta)),
        ANALYZE_FIELD(varintAdaptiveMeta, encodedSize, size_t,
                      offsetof(varintAdaptiveMeta, encodingMeta),
                      sizeof(varintAdaptiveMeta)),
        {"encodingMeta", "union", offsetof(varintAdaptiveMeta, encodingMeta),
         sizeof(((varintAdaptiveMeta *)0)->encodingMeta),
         sizeof(varintAdaptiveMeta) -
             offsetof(varintAdaptiveMeta, encodingMeta) -
             sizeof(((varintAdaptiveMeta *)0)->encodingMeta)}};
    int field_count = sizeof(fields) / sizeof(fields[0]);

    print_field_layout("varintAdaptiveMeta", fields, field_count,
                       sizeof(varintAdaptiveMeta));
    suggest_optimization("varintAdaptiveMeta", fields, field_count,
                         sizeof(varintAdaptiveMeta));
}

/* Analyze varintDictStats */
static void analyze_varintDictStats(void) {
    FieldInfo fields[] = {
        ANALYZE_FIELD(varintDictStats, uniqueCount, size_t,
                      offsetof(varintDictStats, totalCount),
                      sizeof(varintDictStats)),
        ANALYZE_FIELD(varintDictStats, totalCount, size_t,
                      offsetof(varintDictStats, dictBytes),
                      sizeof(varintDictStats)),
        ANALYZE_FIELD(varintDictStats, dictBytes, size_t,
                      offsetof(varintDictStats, indexBytes),
                      sizeof(varintDictStats)),
        ANALYZE_FIELD(varintDictStats, indexBytes, size_t,
                      offsetof(varintDictStats, totalBytes),
                      sizeof(varintDictStats)),
        ANALYZE_FIELD(varintDictStats, totalBytes, size_t,
                      offsetof(varintDictStats, originalBytes),
                      sizeof(varintDictStats)),
        ANALYZE_FIELD(varintDictStats, originalBytes, size_t,
                      offsetof(varintDictStats, compressionRatio),
                      sizeof(varintDictStats)),
        ANALYZE_FIELD(varintDictStats, compressionRatio, float,
                      offsetof(varintDictStats, spaceReduction),
                      sizeof(varintDictStats)),
        ANALYZE_FIELD(varintDictStats, spaceReduction, float,
                      sizeof(varintDictStats), sizeof(varintDictStats))};
    int field_count = sizeof(fields) / sizeof(fields[0]);

    print_field_layout("varintDictStats", fields, field_count,
                       sizeof(varintDictStats));
    suggest_optimization("varintDictStats", fields, field_count,
                         sizeof(varintDictStats));
}

/* Analyze varintBitmapStats */
static void analyze_varintBitmapStats(void) {
    FieldInfo fields[] = {
        ANALYZE_FIELD(varintBitmapStats, type, varintBitmapContainerType,
                      offsetof(varintBitmapStats, cardinality),
                      sizeof(varintBitmapStats)),
        ANALYZE_FIELD(varintBitmapStats, cardinality, uint32_t,
                      offsetof(varintBitmapStats, sizeBytes),
                      sizeof(varintBitmapStats)),
        ANALYZE_FIELD(varintBitmapStats, sizeBytes, size_t,
                      offsetof(varintBitmapStats, containerCapacity),
                      sizeof(varintBitmapStats)),
        ANALYZE_FIELD(varintBitmapStats, containerCapacity, uint32_t,
                      sizeof(varintBitmapStats), sizeof(varintBitmapStats))};
    int field_count = sizeof(fields) / sizeof(fields[0]);

    print_field_layout("varintBitmapStats", fields, field_count,
                       sizeof(varintBitmapStats));
    suggest_optimization("varintBitmapStats", fields, field_count,
                         sizeof(varintBitmapStats));
}

/* Print summary table */
static void print_summary(const StructInfo *structs, int count) {
    printf("\n");
    printf("%s╔════════════════════════════════════════════════════════════════"
           "══════════════╗%s\n",
           C_BOLD, C_RESET);
    printf("%s║                    STRUCT MEMORY EFFICIENCY SUMMARY            "
           "              ║%s\n",
           C_BOLD, C_RESET);
    printf("%s╚════════════════════════════════════════════════════════════════"
           "══════════════╝%s\n",
           C_BOLD, C_RESET);
    printf("\n");
    printf("%-35s %8s %8s %12s\n", "Struct Name", "Size", "Align", "Location");
    printf("───────────────────────────────────────────────────────────────────"
           "──────────────\n");

    size_t total_size = 0;
    for (int i = 0; i < count; i++) {
        printf("%-35s %s%8zu%s %8zu    %s%s%s\n", structs[i].name, C_CYAN,
               structs[i].size, C_RESET, structs[i].align, C_BLUE,
               structs[i].file, C_RESET);
        total_size += structs[i].size;
    }

    printf("───────────────────────────────────────────────────────────────────"
           "──────────────\n");
    printf("Total: %d structs, %zu bytes total struct size\n", count,
           total_size);
}

int main(void) {
    printf("\n");
    printf("%s╔════════════════════════════════════════════════════════════════"
           "══════════════╗%s\n",
           C_BOLD, C_RESET);
    printf("%s║              Varint Library Comprehensive Struct Memory Audit  "
           "              ║%s\n",
           C_BOLD, C_RESET);
    printf("%s║                Using Actual Compiler Layout Information        "
           "              ║%s\n",
           C_BOLD, C_RESET);
    printf("%s╚════════════════════════════════════════════════════════════════"
           "══════════════╝%s\n",
           C_BOLD, C_RESET);

    /* Register all known structs */
    StructInfo structs[] = {
        REGISTER_STRUCT(varintFORMeta, "varintFOR.h"),
        REGISTER_STRUCT(varintPFORMeta, "varintPFOR.h"),
        REGISTER_STRUCT(varintFloatMeta, "varintFloat.h"),
        REGISTER_STRUCT(varintAdaptiveDataStats, "varintAdaptive.h"),
        REGISTER_STRUCT(varintAdaptiveMeta, "varintAdaptive.h"),
        REGISTER_STRUCT(varintDictStats, "varintDict.h"),
        REGISTER_STRUCT(varintDict, "varintDict.h"),
        REGISTER_STRUCT(varintBitmapStats, "varintBitmap.h"),
        REGISTER_STRUCT(varintBitmap, "varintBitmap.h"),
        REGISTER_STRUCT(varintBitmapIterator, "varintBitmap.h"),
        REGISTER_STRUCT(perfStateGlobal, "perf.h"),
        REGISTER_STRUCT(perfStateStat, "perf.h"),
        REGISTER_STRUCT(perfState, "perf.h"),
    };
    int struct_count = sizeof(structs) / sizeof(structs[0]);

    print_summary(structs, struct_count);

    /* Detailed analysis of each important struct */
    printf("\n");
    printf("%s╔════════════════════════════════════════════════════════════════"
           "══════════════╗%s\n",
           C_BOLD, C_RESET);
    printf("%s║                        DETAILED FIELD ANALYSIS                 "
           "               ║%s\n",
           C_BOLD, C_RESET);
    printf("%s╚════════════════════════════════════════════════════════════════"
           "══════════════╝%s\n",
           C_BOLD, C_RESET);

    printf("\n%s[1/7] varintFORMeta%s\n", C_MAGENTA, C_RESET);
    analyze_varintFORMeta();

    printf("\n%s[2/7] varintPFORMeta%s\n", C_MAGENTA, C_RESET);
    analyze_varintPFORMeta();

    printf("\n%s[3/7] varintFloatMeta%s\n", C_MAGENTA, C_RESET);
    analyze_varintFloatMeta();

    printf("\n%s[4/7] varintAdaptiveDataStats%s\n", C_MAGENTA, C_RESET);
    analyze_varintAdaptiveDataStats();

    printf("\n%s[5/7] varintAdaptiveMeta%s\n", C_MAGENTA, C_RESET);
    analyze_varintAdaptiveMeta();

    printf("\n%s[6/7] varintDictStats%s\n", C_MAGENTA, C_RESET);
    analyze_varintDictStats();

    printf("\n%s[7/7] varintBitmapStats%s\n", C_MAGENTA, C_RESET);
    analyze_varintBitmapStats();

    printf("\n");
    printf("%s╔════════════════════════════════════════════════════════════════"
           "══════════════╗%s\n",
           C_BOLD, C_RESET);
    printf("%s║                            FINAL RECOMMENDATIONS               "
           "               ║%s\n",
           C_BOLD, C_RESET);
    printf("%s╚════════════════════════════════════════════════════════════════"
           "══════════════╝%s\n",
           C_BOLD, C_RESET);
    printf("\n");
    printf("To use with pahole (DWARF debugger):\n");
    printf("  1. Compile: %sgcc -g -I../src -o struct_audit struct_audit.c%s\n",
           C_CYAN, C_RESET);
    printf("  2. Analyze: %spahole ./struct_audit%s\n", C_CYAN, C_RESET);
    printf("  3. Specific: %spahole -C varintPFORMeta ./struct_audit%s\n",
           C_CYAN, C_RESET);
    printf("\n");
    printf("General optimization rules:\n");
    printf(
        "  • Order fields by alignment: 8-byte → 4-byte → 2-byte → 1-byte\n");
    printf("  • Group same-sized fields together\n");
    printf("  • Use %s_Static_assert%s to prevent regressions\n", C_CYAN,
           C_RESET);
    printf("  • Consider %s__attribute__((packed))%s only if wire format "
           "required\n",
           C_CYAN, C_RESET);
    printf("\n");

    return 0;
}
