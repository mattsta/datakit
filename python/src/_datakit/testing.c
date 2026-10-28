/* Test support: persisted files written through datakit's own API, so
 * the package's tests read real files without carrying binary fixtures */

#include "dkpy.h"

#include "intset.h"
#include "persist.h"

/* Appends each int of 'values' (a sequence) through 'each'; false with an
 * exception set when an item is not an int that fits int64 */
static bool dkpyEachInt64(PyObject *values, bool (*each)(int64_t, void *),
                          void *ctx) {
    PyObject *seq = PySequence_Fast(values, "values must be a sequence");
    if (!seq) {
        return false;
    }
    const Py_ssize_t n = PySequence_Size(seq);
    bool ok = n >= 0;
    for (Py_ssize_t i = 0; ok && i < n; i++) {
        PyObject *item = PySequence_GetItem(seq, i);
        int64_t v = 0;
        ok = item && PyLong_AsInt64(item, &v) == 0 && each(v, ctx);
        Py_XDECREF(item);
    }
    Py_DECREF(seq);
    return ok;
}

static bool dkpyAddToSet(int64_t v, void *ctx) {
    intset **is = ctx;
    intsetAdd(is, v, nullptr);
    return true;
}

static bool dkpyLogInsert(int64_t v, void *ctx) {
    persist *p = ctx;
    if (persistLogOp(p, PERSIST_OP_INSERT, &v, 1) == PERSIST_LOG_REFUSED) {
        PyErr_SetString(PyExc_OSError, "the WAL refused an insert");
        return false;
    }
    return true;
}

/* _write_intset(snapshot, wal, members, inserts, key=None): a snapshot of
 * an intset holding 'members' and a WAL logging an insert of each of
 * 'inserts'; with a 32-byte 'key', both under HMAC-SHA256 */
static PyObject *dkpyWriteIntset(PyObject *, PyObject *args) {
    PyObject *snapshotArg;
    PyObject *walArg;
    PyObject *members;
    PyObject *inserts;
    const char *key = nullptr;
    Py_ssize_t keyLen = 0;
    if (!PyArg_ParseTuple(args, "OOOO|z#:_write_intset", &snapshotArg, &walArg,
                          &members, &inserts, &key, &keyLen)) {
        return nullptr;
    }
    PyObject *snapshot = nullptr;
    PyObject *wal = nullptr;
    if (!dkpyPath(snapshotArg, false, &snapshot) ||
        !dkpyPath(walArg, false, &wal)) {
        Py_XDECREF(snapshot);
        return nullptr;
    }

    persistConfig cfg = persistDefaultConfig();
    if (key) {
        cfg.checksumType = PERSIST_CHECKSUM_HMAC_SHA256;
    }
    persist *p = persistCreate(&persistOpsIntset, &cfg);
    if (key &&
        !persistSetIntegrityKey(p, (const uint8_t *)key, (size_t)keyLen)) {
        persistClose(p);
        Py_DECREF(snapshot);
        Py_DECREF(wal);
        PyErr_SetString(PyExc_ValueError, "the key was refused");
        return nullptr;
    }
    persistAttachSnapshot(p, persistStoreFile(dkpyPathChars(snapshot), true));
    persistAttachWAL(p, persistStoreFile(dkpyPathChars(wal), true));
    intset *is = intsetNew();
    bool ok = dkpyEachInt64(members, dkpyAddToSet, &is);
    if (ok && !persistSnapshot(p, is)) {
        PyErr_SetString(PyExc_OSError, "the snapshot was not written");
        ok = false;
    }
    ok = ok && dkpyEachInt64(inserts, dkpyLogInsert, p);
    if (ok && !persistSync(p)) {
        PyErr_SetString(PyExc_OSError, "the WAL did not sync");
        ok = false;
    }
    intsetFree(is);
    persistClose(p);
    Py_DECREF(snapshot);
    Py_DECREF(wal);
    return ok ? Py_NewRef(Py_None) : nullptr;
}

PyMethodDef dkpyTestingMethods[] = {
    {"_write_intset", dkpyWriteIntset, METH_VARARGS,
     "Test support: write an intset snapshot and a WAL of inserts."},
    {nullptr, nullptr, 0, nullptr},
};
