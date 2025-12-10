/* Allow test prototypes to exist from headers at edit time
 * in addition to at compile time. */
#ifndef DATAKIT_TEST
#define DATAKIT_TEST
#endif

/* Begin */
#include "datakit.h"

/* Add all includes here so we get their test main functions defined. */
#include "../deps/sha1/sha1.h"
#include "atomPool.h"
#include "bbits.h"
#include "compressionBench.h"
#include "databox.h"
#include "databoxLinear.h"
#include "dataspeed.h"
#include "dj.h"
#include "dod.h"
#include "fenwick/fenwickI64.h" /* New 2-tier template system */
#include "fibbuf.h"
#include "flex.h"
#include "float16.h"
#include "floatExtended.h"
#include "hyperloglog.h"
#include "intersectInt.h"
#include "intset.h"
#include "intsetU32.h"
#include "jebuf.h"
#include "linearBloom.h"
#include "list.h"
#include "mds.h"
#include "mdsc.h"
#include "membound.h"
#include "memtest.h"
#include "mflex.h"
#include "multiFenwick.h" /* Separate multi-fenwick system (wraps multiple trees) */
#include "multiOrderedSet.h"
#include "multiTimer.h"
#include "multiarray.h"
#include "multiarrayLarge.h"
#include "multiarrayMedium.h"
#include "multiarraySmall.h"
#include "multidict.h"
#include "multilist.h"
#include "multilistFull.h"
#include "multilistMedium.h"
#include "multilistSmall.h"
#include "multilru.h"
#include "multimap.h"
#include "multimapAtom.h"
#include "multimapFull.h"
#include "multimapMedium.h"
#include "multimapSmall.h"
#include "multiroar.h"
#include "offsetArray.h"
#include "persist.h"
#include "persist/flexP.h"
#include "persist/intsetP.h"
#include "persist/multiOrderedSetP.h"
#include "persist/multidictP.h"
#include "persist/multilistP.h"
#include "persist/multilruP.h"
#include "persist/multimapP.h"
#include "persist/multiroarP.h"
#include "persist/persistCtx.h"
#include "ptrPrevNext.h"
#include "segment/segmentI64.h" /* New 2-tier template system */
#include "str.h"
#include "strDoubleFormat.h"
#include "stringPool.h"
#include "timeUtil.h"
#include "timerWheel.h"
#include "ulid.h"
#include "util.h"
#include "xof.h"

/* intsetBig requires 128-bit integer support (available on all 64-bit
 * platforms) */
#ifdef __SIZEOF_INT128__
#define USE_INTSET_BIG 1
#include "intsetBig.h"
#endif

#include <stdbool.h>
#include <string.h>
#include <strings.h>

/* ====================================================================
 * Test Registry - Single source of truth for all tests
 * ==================================================================== */

typedef int (*TestFunc)(int argc, char *argv[]);

typedef struct {
    const char *name;     /* Primary test name (must match function) */
    const char *aliases;  /* Comma-separated aliases (NULL if none) */
    TestFunc func;        /* Test function pointer */
    bool adjustArgs;      /* Pass argc-2, argv+2 instead of argc, argv */
    const char *category; /* Category for grouping (NULL = default) */
} TestEntry;

/* Helper: check if a name matches test (primary name or any alias) */
static bool testMatches(const TestEntry *t, const char *name) {
    /* Check primary name */
    if (strcasecmp(t->name, name) == 0) {
        return true;
    }

    /* Check aliases */
    if (t->aliases) {
        const char *p = t->aliases;
        while (*p) {
            const char *start = p;
            while (*p && *p != ',') {
                p++;
            }
            size_t len = p - start;
            if (len > 0 && strlen(name) == len &&
                strncasecmp(start, name, len) == 0) {
                return true;
            }
            if (*p == ',') {
                p++;
            }
        }
    }
    return false;
}

/* Macro helpers for concise test registration */
#define T(n) {#n, NULL, n##Test, false, NULL}
#define T_A(n, a) {#n, a, n##Test, false, NULL}
#define T_ADJ(n) {#n, NULL, n##Test, true, NULL}
#define T_A_ADJ(n, a) {#n, a, n##Test, true, NULL}
#define T_END {NULL, NULL, NULL, false, NULL}

/* Wrapper for void(void) benchmark functions */
static int mdsBenchWrapper(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    return mdsBenchMain();
}
static int mdscBenchWrapper(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    return mdscBenchMain();
}

/*
 * The test registry - add new tests here!
 *
 * Format:
 *   T(name)           - Test with function nameTest(argc, argv)
 *   T_A(name, "a,b")  - Test with aliases
 *   T_ADJ(name)       - Test receiving adjusted args (argc-2, argv+2)
 *   T_A_ADJ(name, a)  - Both aliases and adjusted args
 *   T_CAT(name, cat)  - Test with category
 */
static const TestEntry testRegistry[] = {
    /* Core data structures */
    T(flex), T(mflex), T_A(multilist, "ml"), T(multilistFull), T(multidict),
    T_ADJ(multimap), T(multimapFull), T_A_ADJ(multimapAtom, "atom"),
    T_A_ADJ(stringPool, "sp,strpool"), T_A_ADJ(atomPool, "ap,apool"),
    T_ADJ(multiarray), T_ADJ(multiarraySmall), T_ADJ(multiarrayMedium),
    T_ADJ(multiarrayLarge), T_ADJ(multiroar), T_A_ADJ(multiOrderedSet, "mos"),
    T_A_ADJ(multilru, "lru,mlru"), T_ADJ(list), T_A_ADJ(ptrPrevNext, "ppn"),

    /* Numeric types */
    T_A(float16, "f16"), T_A_ADJ(floatExtended, "float128,fe"), T(intset),
    T(intsetU32), T_A_ADJ(fenwickI64, "fw,bit,fenwick"), /* New 2-tier system */
    T_A_ADJ(segmentI64, "seg,segtree,segment"),          /* New 2-tier system */
    T_A_ADJ(multiFenwick, "mfw,mbit"), /* Multi-tree wrapper system */
#if USE_INTSET_BIG
    T(intsetBig),
#endif

    /* Strings */
    T(mds), T(mdsc), T_ADJ(str), T_ADJ(strDoubleFormat),

    /* Algorithms */
    T(dj), T_ADJ(dod), T_ADJ(xof), T_ADJ(intersectInt),
    T_A_ADJ(hyperloglog, "hll"), T_A_ADJ(linearBloom, "bloom"), T(sha1),

    /* Timers */
    T_A_ADJ(timeUtil, "time"), T_A_ADJ(multiTimer, "timer"),
    T_A_ADJ(timerWheel, "tw"),

    /* Buffers */
    T_ADJ(fibbuf), T_ADJ(jebuf),

    /* Utilities */
    T(util), T_ADJ(databox), T_ADJ(databoxLinear), T(offsetArray),
    T_ADJ(membound), T_ADJ(memtest), T_ADJ(bbits), T_ADJ(ulid), T(persist),
    T_A(persistCtx, "pctx"), T_A(multimapP, "mmP"), T_A(multilistP, "mlP"),
    T_A(multidictP, "mdP"), T_A(flexP, "fP"), T_A(intsetP, "isP"),
    T_A(multiOrderedSetP, "mosP"), T_A(multiroarP, "mrP"),
    T_A(multilruP, "mlruP"),

    /* Benchmarks */
    T_A_ADJ(compressionBench, "compress,cbench"),

    T_END};

/* Benchmarks (separate from tests) */
static const TestEntry benchRegistry[] = {
    {"mdsbench", NULL, mdsBenchWrapper, false, NULL},
    {"mdscbench", NULL, mdscBenchWrapper, false, NULL},
    T_END};

/* Pseudo-tests (special multi-test runners) */
typedef struct {
    const char *name;
    const char *tests; /* Comma-separated list of test names */
} MultiTest;

static const MultiTest multiTests[] = {
    {"allstr", "mds,mdsbench,mdsc,mdscbench"}, {NULL, NULL}};

/* ====================================================================
 * Test Runner Implementation
 * ==================================================================== */

static int countTests(void) {
    int count = 0;
    for (int i = 0; testRegistry[i].name; i++) {
        count++;
    }
    return count;
}

static void printUsage(const char *progname) {
    printf("Usage: %s <command> [test] [options]\n\n", progname);
    printf("Commands:\n");
    printf("  test <name>   Run a specific test\n");
    printf("  test ALL      Run all tests\n");
    printf("  list          List all available tests\n");
    printf("  list --json   List tests in JSON format\n");
    printf("  bench <name>  Run a benchmark\n");
    printf("  speed [options] [MB] [N]\n");
    printf("                  Run dataspeed system benchmark\n");
    printf("                  Options:\n");
    printf("                    --json  Output results as JSON\n");
    printf("                    --csv   Output results as CSV\n");
    printf(
        "                  MB = working set size in megabytes (default: 64)\n");
    printf("                  N  = legacy test iterations (default: 0, skip "
           "legacy)\n");
    printf("                  Examples:\n");
    printf("                    speed              Run with defaults\n");
    printf("                    speed 64 10        64MB working set, 10 legacy "
           "iters\n");
    printf("                    speed --json       Output full benchmark as "
           "JSON\n");
    printf("  help          Show this help message\n");
}

static void listTests(bool json) {
    if (json) {
        printf("{\n  \"tests\": [\n");
        bool first = true;
        for (int i = 0; testRegistry[i].name; i++) {
            if (!first) {
                printf(",\n");
            }
            first = false;
            printf("    {\"name\": \"%s\"", testRegistry[i].name);
            if (testRegistry[i].aliases) {
                printf(", \"aliases\": \"%s\"", testRegistry[i].aliases);
            }
            if (testRegistry[i].category) {
                printf(", \"category\": \"%s\"", testRegistry[i].category);
            }
            printf("}");
        }
        printf("\n  ],\n  \"benchmarks\": [\n");
        first = true;
        for (int i = 0; benchRegistry[i].name; i++) {
            if (!first) {
                printf(",\n");
            }
            first = false;
            printf("    {\"name\": \"%s\"}", benchRegistry[i].name);
        }
        printf("\n  ],\n  \"count\": %d\n}\n", countTests());
    } else {
        printf("Available tests (%d):\n", countTests());
        const char *lastCategory = NULL;
        for (int i = 0; testRegistry[i].name; i++) {
            const TestEntry *t = &testRegistry[i];
            const char *cat = t->category ? t->category : "default";
            if (!lastCategory || strcmp(lastCategory, cat) != 0) {
                lastCategory = cat;
                /* Don't print category header for now to keep output clean */
            }
            printf("  %-20s", t->name);
            if (t->aliases) {
                printf(" (aliases: %s)", t->aliases);
            }
            printf("\n");
        }
        printf("\nBenchmarks:\n");
        for (int i = 0; benchRegistry[i].name; i++) {
            printf("  %s\n", benchRegistry[i].name);
        }
        printf("\nSpecial:\n");
        printf("  ALL                  Run all tests\n");
        for (int i = 0; multiTests[i].name; i++) {
            printf("  %-20s -> %s\n", multiTests[i].name, multiTests[i].tests);
        }
    }
}

static int runTest(const TestEntry *t, int argc, char *argv[]) {
    if (t->adjustArgs) {
        return t->func(argc - 2, argv + 2);
    } else {
        return t->func(argc, argv);
    }
}

static const TestEntry *findTest(const char *name) {
    for (int i = 0; testRegistry[i].name; i++) {
        if (testMatches(&testRegistry[i], name)) {
            return &testRegistry[i];
        }
    }
    return NULL;
}

static const TestEntry *findBench(const char *name) {
    for (int i = 0; benchRegistry[i].name; i++) {
        if (strcasecmp(benchRegistry[i].name, name) == 0) {
            return &benchRegistry[i];
        }
    }
    return NULL;
}

static int runMultiTest(const MultiTest *mt, int argc, char *argv[]) {
    int result = 0;
    const char *p = mt->tests;
    char name[64];

    while (*p) {
        const char *start = p;
        while (*p && *p != ',') {
            p++;
        }
        size_t len = p - start;
        if (len >= sizeof(name)) {
            len = sizeof(name) - 1;
        }
        memcpy(name, start, len);
        name[len] = '\0';

        /* Find and run test or bench */
        const TestEntry *t = findTest(name);
        if (t) {
            result += runTest(t, argc, argv);
        } else {
            t = findBench(name);
            if (t) {
                result += t->func(argc, argv);
            }
        }

        if (*p == ',') {
            p++;
        }
    }
    return result;
}

static int runAllTests(int argc, char *argv[]) {
    int result = 0;
    for (int i = 0; testRegistry[i].name; i++) {
        result += runTest(&testRegistry[i], argc, argv);
    }
    return result;
}

int main(int argc, char *argv[]) {
    /* Don't break the databox size contract! */
    assert(sizeof(databox) == 16);

    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    /* Handle commands */
    const char *cmd = argv[1];

    /* list - enumerate available tests */
    if (strcasecmp(cmd, "list") == 0) {
        bool json = (argc >= 3 && strcmp(argv[2], "--json") == 0);
        listTests(json);
        return 0;
    }

    /* help */
    if (strcasecmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 ||
        strcmp(cmd, "--help") == 0) {
        printUsage(argv[0]);
        return 0;
    }

    /* speed benchmark */
    if (strcasecmp(cmd, "speed") == 0) {
        double mb = 64.0; /* Default: 64 MB working set */
        size_t iters = 0; /* Default: skip legacy test */
        bool jsonOutput = false;
        bool csvOutput = false;
        int argIdx = 2;

        /* Parse options */
        while (argIdx < argc) {
            if (strcmp(argv[argIdx], "--json") == 0) {
                jsonOutput = true;
                argIdx++;
            } else if (strcmp(argv[argIdx], "--csv") == 0) {
                csvOutput = true;
                argIdx++;
            } else {
                break;
            }
        }

        if (argIdx < argc) {
            mb = atof(argv[argIdx]);
            if (mb <= 0 || mb > 4096) {
                fprintf(stderr,
                        "Error: MB must be between 1 and 4096 (got: %s)\n",
                        argv[argIdx]);
                fprintf(stderr,
                        "Usage: %s speed [--json|--csv] [MB] [iterations]\n",
                        argv[0]);
                return -1;
            }
            argIdx++;
        }
        if (argIdx < argc) {
            iters = (size_t)atoi(argv[argIdx]);
        }

        /* Run benchmark with appropriate output format */
        if (jsonOutput) {
            dataspeedReport report;
            dataspeedRunAll(&report, false);
            dataspeedPrintReportJSON(&report);
            return 0;
        } else if (csvOutput) {
            dataspeedReport report;
            dataspeedRunAll(&report, false);
            dataspeedPrintReportCSV(&report);
            return 0;
        }

        return dataspeed(mb, iters);
    }

    /* bench <name> */
    if (strcasecmp(cmd, "bench") == 0 && argc >= 3) {
        const TestEntry *t = findBench(argv[2]);
        if (t) {
            return t->func(argc - 2, argv + 2);
        }
        printf("Benchmark not found: %s\n", argv[2]);
        return -3;
    }

    /* test <name> */
    if (strcasecmp(cmd, "test") == 0 && argc >= 3) {
        /* Check if multiple tests requested */
        if (argc > 3) {
            printf("=== Running %d test suites ===\n", argc - 2);
            for (int i = 2; i < argc; i++) {
                printf("  - %s\n", argv[i]);
            }
            printf("\n");

            int totalFailed = 0;
            for (int testIdx = 2; testIdx < argc; testIdx++) {
                const char *testName = argv[testIdx];

                printf("\n=== Test Suite: %s ===\n", testName);

                /* Special: ALL */
                if (strcasecmp(testName, "ALL") == 0) {
                    int result = runAllTests(argc, argv);
                    if (result != 0) {
                        totalFailed++;
                    }
                    continue;
                }

                /* Check multi-tests */
                bool found = false;
                for (int i = 0; multiTests[i].name; i++) {
                    if (strcasecmp(multiTests[i].name, testName) == 0) {
                        int result = runMultiTest(&multiTests[i], argc, argv);
                        if (result != 0) {
                            totalFailed++;
                        }
                        found = true;
                        break;
                    }
                }
                if (found) {
                    continue;
                }

                /* Find and run specific test */
                const TestEntry *t = findTest(testName);
                if (t) {
                    int result = runTest(t, argc, argv);
                    if (result != 0) {
                        totalFailed++;
                    }
                    continue;
                }

                /* Check benchmarks */
                t = findBench(testName);
                if (t) {
                    int result = t->func(argc - 2, argv + 2);
                    if (result != 0) {
                        totalFailed++;
                    }
                    continue;
                }

                printf("Test not found: %s\n", testName);
                totalFailed++;
            }

            printf("\n=== Summary: %d/%d test suites passed ===\n",
                   (argc - 2) - totalFailed, argc - 2);
            return totalFailed > 0 ? 1 : 0;
        }

        /* Single test requested */
        const char *testName = argv[2];

        /* Special: ALL runs all tests */
        if (strcasecmp(testName, "ALL") == 0) {
            return runAllTests(argc, argv);
        }

        /* Check multi-tests */
        for (int i = 0; multiTests[i].name; i++) {
            if (strcasecmp(multiTests[i].name, testName) == 0) {
                return runMultiTest(&multiTests[i], argc, argv);
            }
        }

        /* Find and run specific test */
        const TestEntry *t = findTest(testName);
        if (t) {
            return runTest(t, argc, argv);
        }

        /* Check benchmarks too (allow 'test mdsbench') */
        t = findBench(testName);
        if (t) {
            return t->func(argc - 2, argv + 2);
        }

        printf("Test not found: %s\n", testName);
        printf("Use '%s list' to see available tests.\n", argv[0]);
        return -3;
    }

    /* Maybe user forgot 'test' keyword? Try to be helpful */
    const TestEntry *t = findTest(cmd);
    if (t) {
        printf("Did you mean: %s test %s\n", argv[0], cmd);
        return -3;
    }

    printUsage(argv[0]);
    return -3;
}
