/**
 * example_bitmap.c - Demonstrates varintBitmap usage
 *
 * varintBitmap provides Roaring-style hybrid dense/sparse encoding for
 * integer sets. Automatically adapts between ARRAY, BITMAP, and RUNS
 * containers based on data density for optimal space efficiency.
 *
 * Compile: gcc -I../../src example_bitmap.c ../../src/varintBitmap.c -o
 * example_bitmap Run: ./example_bitmap
 */

#include "varintBitmap.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================
 * Example 1: Basic operations - add, contains, remove
 * ==================================================================== */
void example_basic() {
    printf("\n=== Example 1: Basic Operations ===\n");

    varintBitmap *vb = varintBitmapCreate();

    /* Add some values */
    printf("Adding values: 1, 100, 200, 300\n");
    varintBitmapAdd(vb, 1);
    varintBitmapAdd(vb, 100);
    varintBitmapAdd(vb, 200);
    varintBitmapAdd(vb, 300);

    /* Check membership */
    printf("Contains 100? %s\n", varintBitmapContains(vb, 100) ? "yes" : "no");
    printf("Contains 50? %s\n", varintBitmapContains(vb, 50) ? "yes" : "no");

    assert(varintBitmapContains(vb, 100) == true);
    assert(varintBitmapContains(vb, 50) == false);

    /* Check cardinality */
    printf("Cardinality: %u\n", varintBitmapCardinality(vb));
    assert(varintBitmapCardinality(vb) == 4);

    /* Remove a value */
    printf("Removing value 100\n");
    bool removed = varintBitmapRemove(vb, 100);
    assert(removed == true);
    assert(varintBitmapContains(vb, 100) == false);
    assert(varintBitmapCardinality(vb) == 3);

    /* Try to remove non-existent value */
    removed = varintBitmapRemove(vb, 999);
    assert(removed == false);
    assert(varintBitmapCardinality(vb) == 3);

    printf("Final cardinality: %u\n", varintBitmapCardinality(vb));
    printf("✓ Basic operations work correctly\n");

    varintBitmapFree(vb);
}

/* ====================================================================
 * Example 2: Container types - demonstrates automatic adaptation
 * ==================================================================== */
void example_container_types() {
    printf("\n=== Example 2: Container Type Adaptation ===\n");

    /* Sparse data: ARRAY container */
    printf("\n-- Sparse Set (ARRAY Container) --\n");
    varintBitmap *sparse = varintBitmapCreate();

    for (int i = 0; i < 100; i += 10) {
        varintBitmapAdd(sparse, (uint16_t)i);
    }

    varintBitmapStats stats;
    varintBitmapGetStats(sparse, &stats);

    printf("Added 10 sparse values\n");
    printf("Container type: %s\n", stats.type == VARINT_BITMAP_ARRAY ? "ARRAY"
                                   : stats.type == VARINT_BITMAP_BITMAP
                                       ? "BITMAP"
                                       : "RUNS");
    printf("Cardinality: %u\n", stats.cardinality);
    printf("Memory used: %zu bytes\n", stats.sizeBytes);

    assert(stats.type == VARINT_BITMAP_ARRAY);

    /* Dense data: BITMAP container */
    printf("\n-- Dense Set (BITMAP Container) --\n");
    varintBitmap *dense = varintBitmapCreate();

    /* Add 5000 values to trigger conversion to bitmap */
    for (int i = 0; i < 5000; i++) {
        varintBitmapAdd(dense, (uint16_t)i);
    }

    varintBitmapGetStats(dense, &stats);

    printf("Added 5000 contiguous values\n");
    printf("Container type: %s\n", stats.type == VARINT_BITMAP_ARRAY ? "ARRAY"
                                   : stats.type == VARINT_BITMAP_BITMAP
                                       ? "BITMAP"
                                       : "RUNS");
    printf("Cardinality: %u\n", stats.cardinality);
    printf("Memory used: %zu bytes\n", stats.sizeBytes);

    assert(stats.type == VARINT_BITMAP_BITMAP);
    assert(stats.cardinality == 5000);

    /* Verify all values are present */
    for (int i = 0; i < 5000; i++) {
        assert(varintBitmapContains(dense, (uint16_t)i));
    }

    printf("✓ Container types adapt automatically\n");

    varintBitmapFree(sparse);
    varintBitmapFree(dense);
}

/* ====================================================================
 * Example 3: Set operations - AND, OR, XOR, AND-NOT
 * ==================================================================== */
void example_set_operations() {
    printf("\n=== Example 3: Set Operations ===\n");

    /* Create two sets */
    varintBitmap *setA = varintBitmapCreate();
    varintBitmap *setB = varintBitmapCreate();

    /* Set A: {1, 2, 3, 4, 5} */
    const uint16_t valuesA[] = {1, 2, 3, 4, 5};
    varintBitmapAddMany(setA, valuesA, 5);

    /* Set B: {4, 5, 6, 7, 8} */
    const uint16_t valuesB[] = {4, 5, 6, 7, 8};
    varintBitmapAddMany(setB, valuesB, 5);

    printf("Set A: {1, 2, 3, 4, 5}\n");
    printf("Set B: {4, 5, 6, 7, 8}\n");

    /* Intersection (AND) */
    varintBitmap *intersection = varintBitmapAnd(setA, setB);
    printf("\nA ∩ B (intersection): ");
    varintBitmapIterator it = varintBitmapCreateIterator(intersection);
    while (varintBitmapIteratorNext(&it)) {
        printf("%u ", it.currentValue);
    }
    printf("\n");
    assert(varintBitmapCardinality(intersection) == 2); /* {4, 5} */
    assert(varintBitmapContains(intersection, 4));
    assert(varintBitmapContains(intersection, 5));

    /* Union (OR) */
    varintBitmap *unionSet = varintBitmapOr(setA, setB);
    printf("A ∪ B (union): ");
    it = varintBitmapCreateIterator(unionSet);
    while (varintBitmapIteratorNext(&it)) {
        printf("%u ", it.currentValue);
    }
    printf("\n");
    assert(varintBitmapCardinality(unionSet) ==
           8); /* {1, 2, 3, 4, 5, 6, 7, 8} */

    /* Symmetric difference (XOR) */
    varintBitmap *xorSet = varintBitmapXor(setA, setB);
    printf("A ⊕ B (XOR): ");
    it = varintBitmapCreateIterator(xorSet);
    while (varintBitmapIteratorNext(&it)) {
        printf("%u ", it.currentValue);
    }
    printf("\n");
    assert(varintBitmapCardinality(xorSet) == 6); /* {1, 2, 3, 6, 7, 8} */

    /* Difference (AND-NOT) */
    varintBitmap *diff = varintBitmapAndNot(setA, setB);
    printf("A \\ B (difference): ");
    it = varintBitmapCreateIterator(diff);
    while (varintBitmapIteratorNext(&it)) {
        printf("%u ", it.currentValue);
    }
    printf("\n");
    assert(varintBitmapCardinality(diff) == 3); /* {1, 2, 3} */

    printf("✓ Set operations work correctly\n");

    varintBitmapFree(setA);
    varintBitmapFree(setB);
    varintBitmapFree(intersection);
    varintBitmapFree(unionSet);
    varintBitmapFree(xorSet);
    varintBitmapFree(diff);
}

/* ====================================================================
 * Example 4: Range operations
 * ==================================================================== */
void example_ranges() {
    printf("\n=== Example 4: Range Operations ===\n");

    varintBitmap *vb = varintBitmapCreate();

    /* Add a range */
    printf("Adding range [100, 200)\n");
    varintBitmapAddRange(vb, 100, 200);

    printf("Cardinality: %u\n", varintBitmapCardinality(vb));
    assert(varintBitmapCardinality(vb) == 100);

    /* Verify range boundaries */
    assert(varintBitmapContains(vb, 100) == true);
    assert(varintBitmapContains(vb, 150) == true);
    assert(varintBitmapContains(vb, 199) == true);
    assert(varintBitmapContains(vb, 99) == false);
    assert(varintBitmapContains(vb, 200) == false);

    /* Check container type */
    varintBitmapStats stats;
    varintBitmapGetStats(vb, &stats);
    printf("Container type: %s\n", stats.type == VARINT_BITMAP_ARRAY ? "ARRAY"
                                   : stats.type == VARINT_BITMAP_BITMAP
                                       ? "BITMAP"
                                       : "RUNS");

    /* Add another range */
    printf("Adding range [500, 600)\n");
    varintBitmapAddRange(vb, 500, 600);

    printf("New cardinality: %u\n", varintBitmapCardinality(vb));
    assert(varintBitmapCardinality(vb) == 200);

    /* Remove a range */
    printf("Removing range [150, 160)\n");
    varintBitmapRemoveRange(vb, 150, 160);

    printf("Final cardinality: %u\n", varintBitmapCardinality(vb));
    assert(varintBitmapCardinality(vb) == 190);
    assert(varintBitmapContains(vb, 149) == true);
    assert(varintBitmapContains(vb, 155) == false);
    assert(varintBitmapContains(vb, 160) == true);

    printf("✓ Range operations work correctly\n");

    varintBitmapFree(vb);
}

/* ====================================================================
 * Example 5: Serialization and deserialization
 * ==================================================================== */
void example_serialization() {
    printf("\n=== Example 5: Serialization ===\n");

    /* Create and populate bitmap */
    varintBitmap *original = varintBitmapCreate();

    uint16_t values[] = {1, 10, 100, 1000, 10000};
    varintBitmapAddMany(original, values, 5);

    printf("Original cardinality: %u\n", varintBitmapCardinality(original));

    /* Serialize */
    size_t bufferSize = varintBitmapSizeBytes(original) + 100;
    uint8_t *buffer = malloc(bufferSize);
    size_t serializedSize = varintBitmapEncode(original, buffer);

    printf("Serialized to %zu bytes\n", serializedSize);

    /* Deserialize */
    varintBitmap *deserialized = varintBitmapDecode(buffer, serializedSize);

    printf("Deserialized cardinality: %u\n",
           varintBitmapCardinality(deserialized));

    /* Verify all values match */
    assert(varintBitmapCardinality(original) ==
           varintBitmapCardinality(deserialized));

    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        assert(varintBitmapContains(deserialized, values[i]));
    }

    printf("✓ Serialization round-trip successful\n");

    free(buffer);
    varintBitmapFree(original);
    varintBitmapFree(deserialized);
}

/* ====================================================================
 * Example 6: Iterator usage
 * ==================================================================== */
void example_iteration() {
    printf("\n=== Example 6: Iteration ===\n");

    varintBitmap *vb = varintBitmapCreate();

    /* Add some values */
    const uint16_t values[] = {5, 15, 25, 35, 45, 55, 65, 75, 85, 95};
    varintBitmapAddMany(vb, values, 10);

    printf("Iterating through bitmap:\n");
    varintBitmapIterator it = varintBitmapCreateIterator(vb);
    int count = 0;
    while (varintBitmapIteratorNext(&it)) {
        if (count > 0) {
            printf(", ");
        }
        printf("%u", it.currentValue);
        count++;
    }
    printf("\n");

    assert(count == 10);

    /* Convert to array */
    uint16_t outputArray[10];
    uint32_t extracted = varintBitmapToArray(vb, outputArray);

    printf("Extracted %u values to array\n", extracted);
    assert(extracted == 10);

    for (uint32_t i = 0; i < extracted; i++) {
        assert(outputArray[i] == values[i]);
    }

    printf("✓ Iteration works correctly\n");

    varintBitmapFree(vb);
}

/* ====================================================================
 * Example 7: Use case - Inverted index posting list
 * ==================================================================== */
typedef struct InvertedIndex {
    const char *term __attribute__((unused));
    varintBitmap *postings; /* Document IDs */
} InvertedIndex;

void example_inverted_index() {
    printf("\n=== Example 7: Inverted Index ===\n");

    /* Create posting lists for terms */
    InvertedIndex terms[3] = {{"varint", varintBitmapCreate()},
                              {"bitmap", varintBitmapCreate()},
                              {"roaring", varintBitmapCreate()}};

    /* Document 1: "varint bitmap"
     * Document 2: "roaring bitmap"
     * Document 3: "varint roaring bitmap"
     * Document 100: "varint"
     * Document 200: "bitmap" */

    varintBitmapAdd(terms[0].postings, 1);   /* varint: doc 1 */
    varintBitmapAdd(terms[0].postings, 3);   /* varint: doc 3 */
    varintBitmapAdd(terms[0].postings, 100); /* varint: doc 100 */

    varintBitmapAdd(terms[1].postings, 1);   /* bitmap: doc 1 */
    varintBitmapAdd(terms[1].postings, 2);   /* bitmap: doc 2 */
    varintBitmapAdd(terms[1].postings, 3);   /* bitmap: doc 3 */
    varintBitmapAdd(terms[1].postings, 200); /* bitmap: doc 200 */

    varintBitmapAdd(terms[2].postings, 2); /* roaring: doc 2 */
    varintBitmapAdd(terms[2].postings, 3); /* roaring: doc 3 */

    /* Query: "varint" AND "bitmap" */
    printf("\nQuery: 'varint' AND 'bitmap'\n");
    varintBitmap *result =
        varintBitmapAnd(terms[0].postings, terms[1].postings);

    printf("Matching documents: ");
    varintBitmapIterator it = varintBitmapCreateIterator(result);
    while (varintBitmapIteratorNext(&it)) {
        printf("%u ", it.currentValue);
    }
    printf("\n");

    assert(varintBitmapCardinality(result) == 2); /* docs 1, 3 */
    assert(varintBitmapContains(result, 1));
    assert(varintBitmapContains(result, 3));

    varintBitmapFree(result);

    /* Query: "varint" OR "roaring" */
    printf("\nQuery: 'varint' OR 'roaring'\n");
    result = varintBitmapOr(terms[0].postings, terms[2].postings);

    printf("Matching documents: ");
    it = varintBitmapCreateIterator(result);
    while (varintBitmapIteratorNext(&it)) {
        printf("%u ", it.currentValue);
    }
    printf("\n");

    assert(varintBitmapCardinality(result) == 4); /* docs 1, 2, 3, 100 */

    printf("✓ Inverted index queries work correctly\n");

    varintBitmapFree(result);
    for (int i = 0; i < 3; i++) {
        varintBitmapFree(terms[i].postings);
    }
}

/* ====================================================================
 * Example 8: Space efficiency comparison
 * ==================================================================== */
void example_space_efficiency() {
    printf("\n=== Example 8: Space Efficiency ===\n");

    printf("\n%-20s | %-12s | %-10s | %-10s\n", "Data Pattern", "Container",
           "Elements", "Bytes");
    printf("---------------------|--------------|------------|------------\n");

    /* Sparse data */
    varintBitmap *sparse = varintBitmapCreate();
    for (int i = 0; i < 100; i += 10) {
        varintBitmapAdd(sparse, (uint16_t)(i * 100));
    }
    varintBitmapStats stats;
    varintBitmapGetStats(sparse, &stats);
    printf("%-20s | %-12s | %10u | %10zu\n", "Sparse (10 vals)",
           stats.type == VARINT_BITMAP_ARRAY ? "ARRAY" : "BITMAP",
           stats.cardinality, stats.sizeBytes);

    /* Medium sparse */
    varintBitmap *medSparse = varintBitmapCreate();
    for (int i = 0; i < 1000; i += 2) {
        varintBitmapAdd(medSparse, (uint16_t)i);
    }
    varintBitmapGetStats(medSparse, &stats);
    printf("%-20s | %-12s | %10u | %10zu\n", "Med sparse (500)",
           stats.type == VARINT_BITMAP_ARRAY ? "ARRAY" : "BITMAP",
           stats.cardinality, stats.sizeBytes);

    /* Dense data */
    varintBitmap *dense = varintBitmapCreate();
    for (int i = 0; i < 10000; i++) {
        varintBitmapAdd(dense, (uint16_t)i);
    }
    varintBitmapGetStats(dense, &stats);
    printf("%-20s | %-12s | %10u | %10zu\n", "Dense (10K contig)",
           stats.type == VARINT_BITMAP_BITMAP ? "BITMAP" : "ARRAY",
           stats.cardinality, stats.sizeBytes);

    /* Range data */
    varintBitmap *range = varintBitmapCreate();
    varintBitmapAddRange(range, 1000, 2000);
    varintBitmapGetStats(range, &stats);
    printf("%-20s | %-12s | %10u | %10zu\n", "Range [1000-2000)",
           stats.type == VARINT_BITMAP_RUNS     ? "RUNS"
           : stats.type == VARINT_BITMAP_BITMAP ? "BITMAP"
                                                : "ARRAY",
           stats.cardinality, stats.sizeBytes);

    printf("\nComparison to naive uint16_t array:\n");
    printf("  Sparse:     %zu bytes vs %zu bytes (%.1fx savings)\n",
           varintBitmapSizeBytes(sparse), (size_t)(10 * 2),
           (float)(10 * 2) / varintBitmapSizeBytes(sparse));

    printf("  Dense:      %zu bytes vs %zu bytes (%.1fx overhead)\n",
           varintBitmapSizeBytes(dense), (size_t)(10000 * 2),
           (float)varintBitmapSizeBytes(dense) / (10000 * 2));

    printf("✓ Space efficiency demonstrated\n");

    varintBitmapFree(sparse);
    varintBitmapFree(medSparse);
    varintBitmapFree(dense);
    varintBitmapFree(range);
}

/* ====================================================================
 * Example 9: Clone and clear operations
 * ==================================================================== */
void example_clone_clear() {
    printf("\n=== Example 9: Clone and Clear ===\n");

    varintBitmap *original = varintBitmapCreate();

    /* Add some values */
    for (uint16_t i = 0; i < 50; i++) {
        varintBitmapAdd(original, i * 10);
    }

    printf("Original cardinality: %u\n", varintBitmapCardinality(original));

    /* Clone */
    varintBitmap *clone = varintBitmapClone(original);

    printf("Clone cardinality: %u\n", varintBitmapCardinality(clone));
    assert(varintBitmapCardinality(clone) == varintBitmapCardinality(original));

    /* Verify all values match */
    for (uint16_t i = 0; i < 50; i++) {
        assert(varintBitmapContains(clone, i * 10));
    }

    /* Modify clone */
    varintBitmapAdd(clone, 999);
    assert(varintBitmapContains(clone, 999));
    assert(!varintBitmapContains(original, 999));

    printf("Clone modified independently from original\n");

    /* Clear original */
    varintBitmapClear(original);
    printf("Original cleared\n");
    assert(varintBitmapIsEmpty(original));
    assert(!varintBitmapIsEmpty(clone));

    printf("✓ Clone and clear work correctly\n");

    varintBitmapFree(original);
    varintBitmapFree(clone);
}

/* ====================================================================
 * Example 10: Comprehensive round-trip test
 * ==================================================================== */
void test_round_trip() {
    printf("\n=== Test: Comprehensive Round-Trip ===\n");

    /* Test various patterns */
    const uint16_t testPatterns[][5] = {
        {1, 2, 3, 4, 5},               /* Sequential */
        {10, 100, 1000, 10000, 50000}, /* Exponential */
        {100, 200, 300, 400, 500},     /* Linear sparse */
        {0, 1, 2, 65534, 65535},       /* Boundaries */
        {42, 42, 42, 42, 42}           /* Duplicates */
    };

    for (size_t pattern = 0; pattern < 5; pattern++) {
        varintBitmap *vb = varintBitmapCreate();

        /* Add values */
        for (int i = 0; i < 5; i++) {
            varintBitmapAdd(vb, testPatterns[pattern][i]);
        }

        /* Serialize */
        size_t bufferSize = varintBitmapSizeBytes(vb) + 100;
        uint8_t *buffer = malloc(bufferSize);
        size_t size = varintBitmapEncode(vb, buffer);

        /* Deserialize */
        varintBitmap *restored = varintBitmapDecode(buffer, size);

        /* Verify */
        assert(varintBitmapCardinality(restored) ==
               varintBitmapCardinality(vb));

        varintBitmapIterator it1 = varintBitmapCreateIterator(vb);
        varintBitmapIterator it2 = varintBitmapCreateIterator(restored);

        while (varintBitmapIteratorNext(&it1)) {
            assert(varintBitmapIteratorNext(&it2));
            assert(it1.currentValue == it2.currentValue);
        }
        assert(!varintBitmapIteratorNext(&it2));

        free(buffer);
        varintBitmapFree(vb);
        varintBitmapFree(restored);
    }

    printf("✓ All round-trip tests passed\n");
}

/* ====================================================================
 * Main
 * ==================================================================== */
int main() {
    printf("===========================================\n");
    printf("   varintBitmap Example Suite\n");
    printf("   Roaring-style Hybrid Encoding\n");
    printf("===========================================\n");

    example_basic();
    example_container_types();
    example_set_operations();
    example_ranges();
    example_serialization();
    example_iteration();
    example_inverted_index();
    example_space_efficiency();
    example_clone_clear();
    test_round_trip();

    printf("\n===========================================\n");
    printf("All examples completed successfully!\n");
    printf("===========================================\n");

    return 0;
}
