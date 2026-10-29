/* fuzzPersistRecover - whole persisted files read back by persistRecover
 *
 * Input: one byte naming the type (fuzzPickOps), a little-endian 32-bit
 * snapshot length, the snapshot file's bytes (a short input takes the
 * bytes left) and then the WAL file's bytes.  Both go into memory
 * stores of a persist with the default config, and persistRecover reads
 * them: the headers, checksums, framing and every entry it replays.  A
 * recovery either fails or gives a structure its validate accepts. */

#include "fuzzCommon.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    const persistOps *ops = fuzzPickOps(&data, &len);
    if (!ops || len < 4) {
        return 0;
    }

    size_t snapLen = (size_t)data[0] | (size_t)data[1] << 8 |
                     (size_t)data[2] << 16 | (size_t)data[3] << 24;
    data += 4;
    len -= 4;
    if (snapLen > len) {
        snapLen = len;
    }

    const persistConfig config = persistDefaultConfig();
    persist *p = persistCreate(ops, &config);
    FUZZ_REQUIRE(p);
    persistStore *snap = persistStoreMemory(snapLen);
    persistStore *wal = persistStoreMemory(len - snapLen);
    FUZZ_REQUIRE(snap && wal);
    if (snapLen) {
        FUZZ_REQUIRE(snap->write(snap->ctx, data, snapLen) == (ssize_t)snapLen);
    }

    if (len > snapLen) {
        FUZZ_REQUIRE(wal->write(wal->ctx, data + snapLen, len - snapLen) ==
                     (ssize_t)(len - snapLen));
    }

    FUZZ_REQUIRE(snap->seek(snap->ctx, 0, SEEK_SET) == 0);
    FUZZ_REQUIRE(wal->seek(wal->ctx, 0, SEEK_SET) == 0);
    persistAttachSnapshot(p, snap);
    persistAttachWAL(p, wal);

    void *recovered = persistRecover(p);
    if (recovered) {
        FUZZ_REQUIRE(!ops->validate || ops->validate(recovered));
        ops->free(recovered);
    }

    persistClose(p);
    return 0;
}
