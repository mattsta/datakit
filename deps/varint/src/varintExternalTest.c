#include "varintExternal.h"
#include "ctest.h"
#include <inttypes.h>
#include <string.h>

int varintExternalTest(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    int32_t err = 0;

    TEST("varintExternalTryGet decodes every width varintExternalGet "
         "serves") {
        static const uint64_t values[] = {0,
                                          1,
                                          UINT64_C(0x7f),
                                          UINT64_C(0xff),
                                          UINT64_C(0x1234),
                                          UINT64_C(0xabcdef),
                                          UINT64_C(0x0123456789abcdef),
                                          UINT64_MAX};
        for (size_t i = 0; i < sizeof(values) / sizeof(*values); i++) {
            for (varintWidth w = VARINT_WIDTH_8B; w <= VARINT_WIDTH_64B; w++) {
                const uint64_t mask = w == VARINT_WIDTH_64B
                                          ? UINT64_MAX
                                          : (UINT64_C(1) << (w * 8)) - 1;
                const uint64_t expect = values[i] & mask;

                uint8_t buf[8] = {0};
                varintExternalPutFixedWidth(buf, expect, w);

                uint64_t got = ~expect;
                if (!varintExternalTryGet(buf, w, &got)) {
                    ERR("width %d refused for value %" PRIu64, (int)w, expect);
                    continue;
                }

                if (got != expect) {
                    ERR("width %d: expected %" PRIu64 ", got %" PRIu64, (int)w,
                        expect, got);
                }

                if (got != varintExternalGet(buf, w)) {
                    ERR("width %d: disagrees with varintExternalGet", (int)w);
                }
            }
        }
    }

    TEST("varintExternalTryGet refuses widths outside 1..8") {
        /* A NULL 'p' doubles as a check that refused widths read nothing. */
        static const uint64_t sentinel = UINT64_C(0x5a5a5a5a5a5a5a5a);
        for (int w = 0; w <= VARINT_WIDTH_128B + 1; w++) {
            if (w >= VARINT_WIDTH_8B && w <= VARINT_WIDTH_64B) {
                continue;
            }

            uint64_t got = sentinel;
            if (varintExternalTryGet(NULL, (varintWidth)w, &got)) {
                ERR("width %d accepted", w);
            }

            if (got != sentinel) {
                ERR("width %d wrote the output on refusal", w);
            }
        }
    }

#ifdef NDEBUG
    TEST("varintExternalGet returns 0 for widths outside 1..8") {
        /* The loaders assert on these widths; with assertions compiled
         * out they must fail closed instead of overrunning their 8-byte
         * result. */
        uint8_t buf[16];
        for (size_t i = 0; i < sizeof(buf); i++) {
            buf[i] = (uint8_t)(i + 1);
        }

        for (int w = 0; w <= VARINT_WIDTH_128B + 1; w++) {
            if (w >= VARINT_WIDTH_8B && w <= VARINT_WIDTH_64B) {
                continue;
            }

            if (varintExternalGet(buf, (varintWidth)w) != 0) {
                ERR("width %d did not fail closed", w);
            }
        }
    }
#endif

    TEST("varintExternalTryGet refuses a NULL output") {
        const uint8_t buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        if (varintExternalTryGet(buf, VARINT_WIDTH_64B, NULL)) {
            ERRR("NULL output accepted");
        }
    }

    TEST_FINAL_RESULT;
}

#ifdef VARINT_EXTERNAL_TEST_STANDALONE
int main(int argc, char *argv[]) {
    return varintExternalTest(argc, argv);
}
#endif
