/* dkFormat - identity and version of the byte formats datakit writes
 *
 * See dkFormat.h for the contract. */

#include "dkFormat.h"

#include "cleaner.h"
#include "dkEndian.h"
#include "flex.h"

#include "../deps/xxHash/xxhash.h"

#include <string.h>

defcleaner(flex *, flexFree);

dkFormatStamp dkFormatStampFor(const dkFormatSpec *spec, uint64_t features) {
    return (dkFormatStamp){
        .id = spec->id, .version = spec->version, .features = features};
}

dkFormatResult dkFormatCheck(const dkFormatSpec *spec,
                             const dkFormatStamp *stamp) {
    if (stamp->id != spec->id) {
        return DK_FORMAT_WRONG_ID;
    }

    if (stamp->version > spec->version) {
        return DK_FORMAT_VERSION_TOO_NEW;
    }

    if (stamp->version < spec->minReadVersion) {
        return DK_FORMAT_VERSION_TOO_OLD;
    }

    /* Unknown ignorable bits are fine: a reader skips what they add */
    if (stamp->features & ~spec->knownFeatures & DK_FORMAT_MUST_UNDERSTAND) {
        return DK_FORMAT_FEATURE_UNKNOWN;
    }

    return DK_FORMAT_OK;
}

const char *dkFormatResultName(dkFormatResult result) {
    switch (result) {
    case DK_FORMAT_OK:
        return "OK";
    case DK_FORMAT_MALFORMED:
        return "MALFORMED";
    case DK_FORMAT_WRONG_ID:
        return "WRONG_ID";
    case DK_FORMAT_VERSION_TOO_NEW:
        return "VERSION_TOO_NEW";
    case DK_FORMAT_VERSION_TOO_OLD:
        return "VERSION_TOO_OLD";
    case DK_FORMAT_FEATURE_UNKNOWN:
        return "FEATURE_UNKNOWN";
    case DK_FORMAT_DIGEST_MISMATCH:
        return "DIGEST_MISMATCH";
    }

    return "UNKNOWN";
}

/* The prefix flex for `stamp` (caller frees) */
static flex *dkFormatPrefixFlex(const dkFormatStamp *stamp) {
    /* @own: caller; free=flexFree */
    flex *f = flexNew();
    flexRow row = FLEX_ROW_INIT;
    flexRowUnsigned(&row, stamp->id);
    flexRowUnsigned(&row, stamp->version);
    flexRowUnsigned(&row, stamp->features);
    flexRowAppendExact(&f, &row);
    return f;
}

size_t dkFormatPrefixSize(const dkFormatStamp *stamp) {
    cleaner(flexFree) flex *f = dkFormatPrefixFlex(stamp);
    return flexBytes(f);
}

size_t dkFormatPrefixWrite(const dkFormatStamp *stamp, uint8_t *out,
                           size_t cap) {
    cleaner(flexFree) flex *f = dkFormatPrefixFlex(stamp);
    const size_t n = flexBytes(f);
    if (n > cap) {
        return 0;
    }

    memcpy(out, f, n);
    return n;
}

/* The unsigned 64-bit value of `fe`, refusing every other type (96/128-bit
 * values included) */
static bool dkFormatReadUnsigned(const flexEntry *fe, uint64_t *out) {
    databoxBig box;
    flexGetByTypeBig(fe, &box);
    if (box.type != DATABOX_UNSIGNED_64) {
        return false;
    }

    *out = box.data.u64;
    return true;
}

dkFormatResult dkFormatPrefixRead(const dkFormatSpec *spec, const uint8_t *buf,
                                  size_t len, dkFormatStamp *stamp,
                                  size_t *prefixLen) {
    /* The prefix is a flex at the head of a longer buffer: bound it by its
     * own length claim, then check every entry before reading any */
    size_t total = 0;
    if (!buf || !flexPeekTotalBytes(buf, len, &total) ||
        !flexValidateBufferDeep(buf, total)) {
        return DK_FORMAT_MALFORMED;
    }

    const flex *f = (const flex *)buf;
    if (flexCount(f) != 3) {
        return DK_FORMAT_MALFORMED;
    }

    uint64_t fields[3];
    flexEntry *fe = flexHead(f);
    for (size_t i = 0; i < 3; i++, fe = flexNext(f, fe)) {
        if (!dkFormatReadUnsigned(fe, &fields[i])) {
            return DK_FORMAT_MALFORMED;
        }
    }

    if (fields[0] > UINT32_MAX || fields[1] > UINT32_MAX) {
        return DK_FORMAT_MALFORMED;
    }

    *stamp = (dkFormatStamp){.id = (uint32_t)fields[0],
                             .version = (uint32_t)fields[1],
                             .features = fields[2]};
    const dkFormatResult result = dkFormatCheck(spec, stamp);
    if (result == DK_FORMAT_OK) {
        *prefixLen = total;
    }

    return result;
}

size_t dkFormatSealedSize(const dkFormatStamp *stamp, size_t payloadLen) {
    return dkFormatPrefixSize(stamp) + payloadLen + DK_FORMAT_SEAL_DIGEST_LEN;
}

uint8_t *dkFormatSealBegin(const dkFormatStamp *stamp, uint8_t *out, size_t cap,
                           size_t payloadLen) {
    if (!out || cap < dkFormatSealedSize(stamp, payloadLen)) {
        return nullptr;
    }

    return out + dkFormatPrefixWrite(stamp, out, cap);
}

/* The digest of the `len` bytes a seal covers */
static uint64_t dkFormatSealDigest(const uint8_t *buf, size_t len) {
    return (uint64_t)XXH3_64bits(buf, len);
}

size_t dkFormatSealEnd(uint8_t *out, size_t sealedLen) {
    const size_t covered = sealedLen - DK_FORMAT_SEAL_DIGEST_LEN;
    dkStoreLe64(out + covered, dkFormatSealDigest(out, covered));
    return sealedLen;
}

dkFormatResult dkFormatOpen(const dkFormatSpec *spec, const uint8_t *buf,
                            size_t len, dkFormatStamp *stamp,
                            const uint8_t **payload, size_t *payloadLen) {
    if (!buf || len < DK_FORMAT_SEAL_DIGEST_LEN) {
        return DK_FORMAT_MALFORMED;
    }

    const size_t covered = len - DK_FORMAT_SEAL_DIGEST_LEN;
    if (dkLoadLe64(buf + covered) != dkFormatSealDigest(buf, covered)) {
        return DK_FORMAT_DIGEST_MISMATCH;
    }

    size_t prefix = 0;
    const dkFormatResult result =
        dkFormatPrefixRead(spec, buf, covered, stamp, &prefix);
    if (result == DK_FORMAT_OK) {
        *payload = buf + prefix;
        *payloadLen = covered - prefix;
    }
    return result;
}

#ifdef DATAKIT_TEST
#include "ctest.h"
#include "datakit.h" /* zmalloc / zfree */
#include "persist.h" /* persistRegistry */

#include <inttypes.h>
#include <stdio.h>

defcleanerName(uint8_t *, zfree, FormatBytes);

int dkFormatTest(int, char *[]) {
    int err = 0;

    /* Reads versions 2 and 3, knows must-understand bit 0 and ignorable
     * bit 32 */
    const dkFormatSpec spec = {.name = "test",
                               .id = 77,
                               .version = 3,
                               .minReadVersion = 2,
                               .knownFeatures = 1 | (UINT64_C(1) << 32)};

    TEST("every check result has its own name") {
        const dkFormatResult results[] = {DK_FORMAT_OK,
                                          DK_FORMAT_MALFORMED,
                                          DK_FORMAT_WRONG_ID,
                                          DK_FORMAT_VERSION_TOO_NEW,
                                          DK_FORMAT_VERSION_TOO_OLD,
                                          DK_FORMAT_FEATURE_UNKNOWN,
                                          DK_FORMAT_DIGEST_MISMATCH};
        const size_t n = countof(results);
        for (size_t i = 0; i < n; i++) {
            const char *name = dkFormatResultName(results[i]);
            for (size_t j = 0; j < i; j++) {
                if (!name || !*name ||
                    strcmp(name, dkFormatResultName(results[j])) == 0) {
                    ERR("result %zu has no name of its own", i);
                }
            }
        }
        if (strcmp(dkFormatResultName(DK_FORMAT_WRONG_ID), "WRONG_ID") ||
            strcmp(dkFormatResultName((dkFormatResult)99), "UNKNOWN")) {
            ERR("an unknown result is not named as such");
        }
    }

    TEST("check: version range, id and feature rules") {
        const struct {
            dkFormatStamp stamp;
            dkFormatResult want;
        } cases[] = {
            {{77, 3, 0}, DK_FORMAT_OK},
            {{77, 2, 0}, DK_FORMAT_OK},
            {{77, 4, 0}, DK_FORMAT_VERSION_TOO_NEW},
            {{77, 1, 0}, DK_FORMAT_VERSION_TOO_OLD},
            {{78, 3, 0}, DK_FORMAT_WRONG_ID},
            /* known must-understand and known ignorable bits */
            {{77, 3, 1 | (UINT64_C(1) << 32)}, DK_FORMAT_OK},
            /* an unknown must-understand bit, low and high end */
            {{77, 3, 2}, DK_FORMAT_FEATURE_UNKNOWN},
            {{77, 3, UINT64_C(1) << 31}, DK_FORMAT_FEATURE_UNKNOWN},
            /* unknown ignorable bits only */
            {{77, 3, UINT64_C(0xFFFFFFFF00000000)}, DK_FORMAT_OK},
        };
        for (size_t i = 0; i < countof(cases); i++) {
            const dkFormatResult got = dkFormatCheck(&spec, &cases[i].stamp);
            if (got != cases[i].want) {
                ERR("case %zu: %s, want %s", i, dkFormatResultName(got),
                    dkFormatResultName(cases[i].want));
            }
        }

        const dkFormatStamp written = dkFormatStampFor(&spec, 1);
        if (written.id != 77 || written.version != 3 || written.features != 1) {
            ERR("StampFor did not stamp the written version");
        }
    }

    TEST("prefix: round trip ahead of a payload, every stamp width") {
        const dkFormatStamp stamps[] = {
            {77, 2, 0},
            {77, 3, 1},
            {77, 3, UINT64_MAX & ~(DK_FORMAT_MUST_UNDERSTAND & ~UINT64_C(1))},
        };
        static constexpr uint8_t payload[] = "payload bytes";
        for (size_t s = 0; s < countof(stamps); s++) {
            const size_t prefix = dkFormatPrefixSize(&stamps[s]);
            const size_t total = prefix + sizeof(payload);
            cleanerName(zfree, FormatBytes) uint8_t *buf = zmalloc(total);
            if (dkFormatPrefixWrite(&stamps[s], buf, prefix - 1) != 0 ||
                dkFormatPrefixWrite(&stamps[s], buf, prefix) != prefix) {
                ERR("stamp %zu: write ignored the capacity", s);
                continue;
            }
            memcpy(buf + prefix, payload, sizeof(payload));

            dkFormatStamp got;
            size_t gotLen = 0;
            if (dkFormatPrefixRead(&spec, buf, total, &got, &gotLen) !=
                    DK_FORMAT_OK ||
                gotLen != prefix || got.id != stamps[s].id ||
                got.version != stamps[s].version ||
                got.features != stamps[s].features ||
                memcmp(buf + gotLen, payload, sizeof(payload)) != 0) {
                ERR("stamp %zu did not round-trip", s);
            }

            /* Every truncation of the prefix is malformed */
            for (size_t cut = 0; cut < prefix; cut++) {
                if (dkFormatPrefixRead(&spec, buf, cut, &got, &gotLen) !=
                    DK_FORMAT_MALFORMED) {
                    ERR("stamp %zu: a %zu-byte prefix was read", s, cut);
                }
            }
        }
    }

    TEST("prefix: refusals report the stamp they found") {
        const dkFormatStamp newer = {77, 9, 0};
        uint8_t buf[64];
        const size_t n = dkFormatPrefixWrite(&newer, buf, sizeof(buf));
        dkFormatStamp got = {};
        size_t gotLen = 0;
        if (dkFormatPrefixRead(&spec, buf, n, &got, &gotLen) !=
                DK_FORMAT_VERSION_TOO_NEW ||
            got.version != 9) {
            ERR("a newer version was not refused with its stamp");
        }
    }

    TEST("prefix: entries that are not small unsigned integers") {
        /* A 4-entry flex, a signed entry, a bytes entry, an id past 32
         * bits and a 128-bit features value are all malformed */
        databoxBig wide;
        DATABOX_BIG_UNSIGNED_128(&wide, (__uint128_t)1 << 64);
        const databox fields[][4] = {
            {DATABOX_UNSIGNED(77), DATABOX_UNSIGNED(3), DATABOX_UNSIGNED(0),
             DATABOX_UNSIGNED(0)},
            {DATABOX_SIGNED(-77), DATABOX_UNSIGNED(3), DATABOX_UNSIGNED(0)},
            {databoxNewBytesString("77"), DATABOX_UNSIGNED(3),
             DATABOX_UNSIGNED(0)},
            {DATABOX_UNSIGNED(UINT64_C(1) << 32 | 77), DATABOX_UNSIGNED(3),
             DATABOX_UNSIGNED(0)},
            {DATABOX_UNSIGNED(77), DATABOX_UNSIGNED(3), *(databox *)&wide},
        };
        const size_t counts[] = {4, 3, 3, 3, 3};
        for (size_t c = 0; c < countof(counts); c++) {
            cleaner(flexFree) flex *f = flexNew();
            flexRow row = FLEX_ROW_INIT;
            for (size_t i = 0; i < counts[c]; i++) {
                flexRowAdd(&row, &fields[c][i]);
            }
            flexRowAppendExact(&f, &row);
            dkFormatStamp got;
            size_t gotLen = 0;
            if (dkFormatPrefixRead(&spec, (const uint8_t *)f, flexBytes(f),
                                   &got, &gotLen) != DK_FORMAT_MALFORMED) {
                ERR("case %zu was read as a stamp", c);
            }
        }

        if (dkFormatPrefixRead(&spec, nullptr, 10, nullptr, nullptr) !=
            DK_FORMAT_MALFORMED) {
            ERR("a NULL buffer was read");
        }
    }

    TEST("sealed: round trip, and any one byte changed reads as a digest "
         "mismatch") {
        const dkFormatStamp stamp = dkFormatStampFor(&spec, 0);
        static constexpr uint8_t body[] = "sealed payload bytes";
        const size_t sealed = dkFormatSealedSize(&stamp, sizeof(body));
        uint8_t buf[128];
        uint8_t *at = dkFormatSealBegin(&stamp, buf, sealed, sizeof(body));
        if (!at || dkFormatSealBegin(&stamp, buf, sealed - 1, sizeof(body)) ||
            dkFormatSealBegin(&stamp, nullptr, sealed, sizeof(body))) {
            ERR("SealBegin: room for the sealed size, and no less");
        }
        memcpy(at, body, sizeof(body));
        if (dkFormatSealEnd(buf, sealed) != sealed) {
            ERR("SealEnd did not return the sealed size");
        }

        dkFormatStamp got = {};
        const uint8_t *payload = nullptr;
        size_t payloadLen = 0;
        if (dkFormatOpen(&spec, buf, sealed, &got, &payload, &payloadLen) !=
                DK_FORMAT_OK ||
            payloadLen != sizeof(body) || memcmp(payload, body, payloadLen) ||
            got.id != stamp.id) {
            ERR("a sealed payload did not open as written");
        }

        for (size_t i = 0; i < sealed; i++) {
            buf[i] ^= 0x10;
            if (dkFormatOpen(&spec, buf, sealed, &got, &payload, &payloadLen) !=
                DK_FORMAT_DIGEST_MISMATCH) {
                ERR("byte %zu changed did not read as a digest mismatch", i);
            }
            buf[i] ^= 0x10;
        }

        if (dkFormatOpen(&spec, buf, sealed - 1, &got, &payload, &payloadLen) !=
                DK_FORMAT_DIGEST_MISMATCH ||
            dkFormatOpen(&spec, buf, 7, &got, &payload, &payloadLen) !=
                DK_FORMAT_MALFORMED ||
            dkFormatOpen(&spec, nullptr, 64, &got, &payload, &payloadLen) !=
                DK_FORMAT_MALFORMED) {
            ERR("a cut, short or NULL seal was not refused");
        }
    }

    /* The catalog: every format named once, with a sane read range; the
     * table is the operator's view of what this build writes and reads */
    TEST("manifest: every format once, versions in range") {
        /* Every persisted type's format (persistRegistry) and the fixed
         * ones */
        dkFormatSpec specs[128];
        const size_t n = dkFormatManifest(specs, countof(specs));
        const persistOps *const *registered = nullptr;
        const size_t types = persistRegistry(&registered);
        if (n != dkFormatManifest(nullptr, 0) || n <= types ||
            n > countof(specs)) {
            ERR("manifest lists %zu formats for %zu persisted types", n, types);
        }
        printf("%-34s %8s %8s %8s %18s\n", "format", "id", "writes",
               "reads>=", "known features");
        for (size_t i = 0; i < n && i < countof(specs); i++) {
            const dkFormatSpec *s = &specs[i];
            printf("%-34s %8" PRIu32 " %8" PRIu32 " %8" PRIu32 " 0x%016" PRIx64
                   "\n",
                   s->name ? s->name : "?", s->id, s->version,
                   s->minReadVersion, s->knownFeatures);
            if (!s->name || s->version == 0 || s->minReadVersion > s->version ||
                s->minReadVersion == 0) {
                ERR("format %zu has a bad spec", i);
            }
            for (size_t j = 0; j < i; j++) {
                if (specs[j].id == s->id) {
                    ERR("format id %" PRIu32 " listed twice", s->id);
                }
            }

            /* Every format reads what it writes */
            const dkFormatStamp own = dkFormatStampFor(s, 0);
            if (dkFormatCheck(s, &own) != DK_FORMAT_OK) {
                ERR("%s does not read its own stamp", s->name);
            }
        }
    }

    TEST_FINAL_RESULT;
}
#endif /* DATAKIT_TEST */
