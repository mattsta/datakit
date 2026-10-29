/* fuzzFlexValidate - untrusted bytes checked as a flex, then walked
 *
 * Input: the putative flex.  flexValidateBuffer and
 * flexValidateBufferDeep take any bytes; a buffer the deep check
 * accepts the shallow one accepts too, and it walks forward with every
 * entry decoding (wide values into wide storage) and as many entries as
 * flexCount reports, as flex.h promises. */

#include "fuzzCommon.h"

#include "../flex.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    const bool shallow = flexValidateBuffer(data, len);
    if (!flexValidateBufferDeep(data, len)) {
        return 0;
    }

    FUZZ_REQUIRE(shallow);
    const flex *f = (const flex *)data;
    size_t walked = 0;
    for (flexEntry *fe = flexHeadIfAny(f); fe; fe = flexNext(f, fe)) {
        databoxBig box;
        DATABOX_BIG_INIT(&box);
        flexGetByType(fe, (databox *)&box);
        walked++;
    }

    FUZZ_REQUIRE(walked == flexCount(f));
    return 0;
}
