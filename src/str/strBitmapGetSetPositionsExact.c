#include "../str.h"

/* ====================================================================
 * Get set bit positions inside bitmap
 * ==================================================================== */
/* Populate array position[] with set bit positions.
 * Note:
 *   - positions[] must be big enough to hold all positions.
 *     - You can use StrPopCnt to determine how long you should make position[]
 *   - 'Exact' functions don't pre-align data or calculate data beyond
 *     the step size.
 *       - Meaning: this only works if your data is exactly divisible by
 *                  sizeof(unsigned long)
 *   - GetPositionsExact8 limits you to 255 bit positions
 *   - GetPositionsExact16 limits you to 65535 bit positions
 *   - GetPositionsExact32 limits you to 4 billion bit positions
 *   - GetPositionsExact64 limits you to 18 (lots) bit positions */
#define synthesizeGetPositionsExact(POSITION_STORAGE, RETURN_STORAGE,          \
                                    SET_UNSET, IS_INVERSE)                     \
    RETURN_STORAGE StrBitmapGet##SET_UNSET##PositionsExact##POSITION_STORAGE(  \
        const void *data_, size_t len,                                         \
        uint##POSITION_STORAGE##_t position[]) {                               \
        const unsigned long *data = data_;                                     \
        const size_t lenAsLong = len / (sizeof(unsigned long));                \
        size_t idx = 0;                                                        \
                                                                               \
        for (size_t i = 0; i < lenAsLong; i++) {                               \
            unsigned long myword = IS_INVERSE data[i];                         \
            while (myword != 0) {                                              \
                const unsigned long unsetAfterCheck = myword & -myword;        \
                const int r = __builtin_ctzl(myword);                          \
                position[idx++] = i * 64 + r;                                  \
                myword ^= unsetAfterCheck;                                     \
            }                                                                  \
        }                                                                      \
        return idx;                                                            \
    }

/* Stop clang-format from indenting these because they don't have semicolons. */
/* clang-format off */
synthesizeGetPositionsExact(8, uint32_t, Set, )
synthesizeGetPositionsExact(16, uint32_t, Set, )
synthesizeGetPositionsExact(32, uint32_t, Set, )
synthesizeGetPositionsExact(64, uint64_t, Set, )

synthesizeGetPositionsExact(8, uint32_t, Unset, ~)
synthesizeGetPositionsExact(16, uint32_t, Unset, ~)
synthesizeGetPositionsExact(32, uint32_t, Unset, ~)
synthesizeGetPositionsExact(64, uint64_t, Unset, ~)
    /* clang-format on */

    DK_FN_UNUSED static void formattingGuard_______() {
}
