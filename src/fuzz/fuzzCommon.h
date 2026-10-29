/* fuzzCommon - what the libFuzzer targets in src/fuzz share
 *
 * Each target is one LLVMFuzzerTestOneInput over bytes from outside the
 * process.  A target asserts the decoder's contract with FUZZ_REQUIRE,
 * which aborts so libFuzzer records the input; a crash, a sanitizer
 * report or a broken contract is a finding.  `dktools fuzz` builds and
 * runs them (the fuzz preset: clang, ASan and UBSan). */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../cleaner.h"
#include "../datakit.h"
#include "../persist.h"

defcleaner(void *, zfree);

/* Abort with the broken contract when 'cond' is false */
#define FUZZ_REQUIRE(cond)                                                     \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "fuzz contract broken at %s:%d: %s\n", __FILE__,   \
                    __LINE__, #cond);                                          \
            abort();                                                           \
        }                                                                      \
    } while (0)

/* The registered persist type the first input byte names (modulo the
 * registry's length), stepping '*data' and '*len' past it; NULL for an
 * empty input */
static inline const persistOps *fuzzPickOps(const uint8_t **data, size_t *len) {
    if (*len == 0) {
        return NULL;
    }

    const persistOps *const *ops;
    const size_t count = persistRegistry(&ops);
    const persistOps *picked = ops[**data % count];
    (*data)++;
    (*len)--;
    return picked;
}
