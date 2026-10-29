/* fuzzDataboxLinear - a type byte and value bytes decoded as a databox
 *
 * Input: the type byte, then up to a value's width of bytes (zeroes
 * fill the rest).  databoxLinearDecodeParts refuses an unknown type; a
 * fixed-width box it decodes encodes (at the narrowest width, which may
 * differ from the input's) and decodes again to the same value: the same
 * type and bytes, or, for a double a float holds exactly, that float. */

#include "fuzzCommon.h"

#include "../databoxLinear.h"

#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    if (len == 0 || DATABOX_LINEAR_TYPE_IS_BYTES(data[0])) {
        return 0;
    }

    uint8_t value[sizeof(databoxUnion)] = {};
    memcpy(value, data + 1, len - 1 < sizeof(value) ? len - 1 : sizeof(value));

    databox box = {};
    if (!databoxLinearDecodeParts(data[0], value, &box)) {
        return 0;
    }

    databoxLinear linear;
    FUZZ_REQUIRE(databoxLinearEncode(&box, &linear) >= 0);
    databox again = {};
    FUZZ_REQUIRE(databoxLinearDecode(&linear, &again));
    if (box.type == DATABOX_DOUBLE_64 && again.type == DATABOX_FLOAT_32) {
        /* A double a float holds exactly is stored as the float */
        FUZZ_REQUIRE((double)again.data.f32 == box.data.d64);
    } else {
        FUZZ_REQUIRE(again.type == box.type &&
                     memcmp(&again.data, &box.data, sizeof(box.data)) == 0);
    }

    return 0;
}
