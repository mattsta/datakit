#ifndef CTEST_H
#define CTEST_H

#include <stdio.h>
#include <string.h> /* strchr */

/* Generate new string concatenating integer i against string 'prefix' */
#define CTEST_INCLUDE_GEN(named)                                               \
    static char *gen##named(const char *prefix, int i) {                       \
        static char result[64] = {0};                                          \
        memset(result, 0, sizeof(result));                                     \
        snprintf(result, sizeof(result), "%s%d", prefix, i);                   \
        return result;                                                         \
    }

#ifdef CTEST_INCLUDE_KVGEN
#define CTEST_INCLUDE_KEYGEN
#define CTEST_INCLUDE_VALGEN
#endif

#ifdef CTEST_INCLUDE_KEYGEN
CTEST_INCLUDE_GEN(key)
#endif

#ifdef CTEST_INCLUDE_VALGEN
CTEST_INCLUDE_GEN(val)
#endif

/* Only print the filename as debug prefix, not entire file path.
 * Just: detect the last '/' in file path, then increment one after it.
 *       if no '/' detected, use entire filename. */
#define currentFilename()                                                      \
    do {                                                                       \
        char *filenameOnly = strrchr(__FILE__, '/');                           \
        char *filePos = filenameOnly ? ++filenameOnly : __FILE__;              \
        printf("%s:%s:%d\t", filePos, __func__, __LINE__);                     \
    } while (0)

#define ERROR                                                                  \
    do {                                                                       \
        printf("\tERROR!\n");                                                  \
        fflush(stdout);                                                        \
        err++;                                                                 \
    } while (0)

#define ERR(x, ...)                                                            \
    do {                                                                       \
        currentFilename();                                                     \
        printf("ERROR! " x "\n", __VA_ARGS__);                                 \
        fflush(stdout);                                                        \
        err++;                                                                 \
    } while (0)

#define ERRR(x)                                                                \
    do {                                                                       \
        currentFilename();                                                     \
        printf("ERROR! " x "\n");                                              \
        fflush(stdout);                                                        \
        err++;                                                                 \
    } while (0)

#define TEST(name)                                                             \
    printf("test — %s\n", name);                                               \
    fflush(stdout);
#define TEST_DESC(name, ...)                                                   \
    printf("test — " name "\n", __VA_ARGS__);                                  \
    fflush(stdout);

#define TEST_FINAL_RESULT                                                      \
    do {                                                                       \
        if (err) {                                                             \
            ERR("Sorry, not all tests passed!  In fact, %d tests failed.",     \
                err);                                                          \
        } else {                                                               \
            printf("ALL TESTS PASSED!\n");                                     \
        }                                                                      \
        return err;                                                            \
    } while (0)

#endif /* CTEST_H */
