/* datakit.persist's C layer: read-only views of persisted files.  Every
 * function opens what it reads, reads it through read-only stores and
 * closes it again, so no file is ever changed and nothing is shared
 * between calls.  Results are plain tuples, dicts and lists; the Python
 * module gives them their types */

#include "dkpy.h"

#include "persist.h"

/* ---- registry() -> [(type_id, name, format_version, min_read_version)] */

static PyObject *dkpyRegistry(PyObject *, PyObject *) {
    const persistOps *const *ops = nullptr;
    const size_t n = persistRegistry(&ops);
    PyObject *out = PyList_New(0);
    for (size_t i = 0; out && i < n; i++) {
        PyObject *row = Py_BuildValue(
            "(NsNN)", DKPY_U64(ops[i]->type), persistTypeName(ops[i]->type),
            DKPY_U64(ops[i]->formatVersion), DKPY_U64(ops[i]->minReadVersion));
        if (!row || PyList_Append(out, row) < 0) {
            Py_XDECREF(row);
            Py_CLEAR(out);
            break;
        }
        Py_DECREF(row);
    }
    return out;
}

/* ---- inspect(path) -> (status, info or None) ------------------------- */

static PyObject *dkpyInspect(PyObject *, PyObject *arg) {
    PyObject *path = nullptr;
    if (!dkpyPath(arg, false, &path)) {
        return nullptr;
    }

    persistFileInfo info = {};
    int64_t fileBytes = -1;
    bool opened = false;
    bool read = false;
    Py_BEGIN_ALLOW_THREADS;
    persistStore *store = persistStoreFileReadOnly(dkpyPathChars(path));
    opened = store != nullptr;
    if (store) {
        fileBytes = store->size(store->ctx);
        read = persistInspect(store, &info);
        persistStoreClose(store);
    }
    Py_END_ALLOW_THREADS;
    Py_DECREF(path);

    if (!opened) {
        return Py_BuildValue("(sO)", "NO_FILE", Py_None);
    }
    if (!read) {
        return Py_BuildValue("(sO)", "NOT_PERSIST", Py_None);
    }

    /* Whether this build reads the framing and the structure format */
    const persistOps *ops = persistOpsForType((persistType)info.format.id);
    const dkFormatSpec containerSpec =
        info.wal ? persistFormatWALFile : persistFormatSnapshotFile;
    const char *containerCheck =
        dkFormatResultName(dkFormatCheck(&containerSpec, &info.container));
    const char *formatCheck = "UNKNOWN_TYPE";
    if (ops) {
        const dkFormatSpec spec = persistOpsFormat(ops);
        formatCheck = dkFormatResultName(dkFormatCheck(&spec, &info.format));
    }

    return Py_BuildValue(
        "(s{s:s,s:N,s:N,s:N,s:N,s:s,s:N,s:s,s:N,s:N,s:s,s:N,s:N,s:N,s:N,s:N})",
        "OK", "kind", info.wal ? "wal" : "snapshot", "file_bytes",
        DKPY_I64(fileBytes), "header_bytes", DKPY_U64(info.headerLen),
        "container_version", DKPY_U64(info.container.version),
        "container_features", DKPY_U64(info.container.features),
        "container_check", containerCheck, "type_id", DKPY_U64(info.format.id),
        "type_name", persistTypeName((persistType)info.format.id),
        "format_version", DKPY_U64(info.format.version), "format_features",
        DKPY_U64(info.format.features), "format_check", formatCheck, "key_id",
        DKPY_U64(info.keyId), "count", DKPY_U64(info.count), "body_bytes",
        DKPY_U64(info.envelopeLen), "sequence", DKPY_U64(info.sequence),
        "readable",
        PyBool_FromLong(ops && dkFormatCheck(&containerSpec, &info.container) ==
                                   DK_FORMAT_OK));
}

/* ---- Opening a snapshot and a WAL for one call ----------------------- */

static char *const dkpyOpenKeywords[] = {"snapshot", "wal", nullptr};

typedef struct dkpyOpened {
    PyObject *snapshot; /* path bytes, or NULL */
    PyObject *wal;
    persist *p;
    persistOpenResult why;
} dkpyOpened;

/* Opens the snapshot and WAL paths (None: absent): 0 with an exception set
 * when a path is not a path; otherwise 1 with o->p NULL and o->why saying
 * why when the files do not open */
static int dkpyOpen(PyObject *snapshotArg, PyObject *walArg, dkpyOpened *o) {
    *o = (dkpyOpened){.why = PERSIST_OPEN_NO_PATH};
    if (!dkpyPath(snapshotArg, true, &o->snapshot) ||
        !dkpyPath(walArg, true, &o->wal)) {
        Py_CLEAR(o->snapshot);
        return 0;
    }
    Py_BEGIN_ALLOW_THREADS;
    o->p = persistOpenFiles(dkpyPathChars(o->snapshot), dkpyPathChars(o->wal),
                            &o->why);
    Py_END_ALLOW_THREADS;
    return 1;
}

static void dkpyClose(dkpyOpened *o) {
    persistClose(o->p);
    Py_CLEAR(o->snapshot);
    Py_CLEAR(o->wal);
}

/* ---- verify(snapshot, wal) -> (open, snapshot_ok, wal_ok, error) ----- */

static PyObject *dkpyVerify(PyObject *, PyObject *args, PyObject *kwargs) {
    PyObject *snapshotArg = Py_None;
    PyObject *walArg = Py_None;
    dkpyOpened o;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|OO:verify",
                                     dkpyOpenKeywords, &snapshotArg, &walArg) ||
        !dkpyOpen(snapshotArg, walArg, &o)) {
        return nullptr;
    }
    if (!o.p) {
        dkpyClose(&o);
        return Py_BuildValue("(sOOs)", persistOpenResultName(o.why), Py_None,
                             Py_None, "NONE");
    }

    int snapshotOk = -1;
    int walOk = -1;
    persistErr err = PERSIST_ERR_NONE;
    Py_BEGIN_ALLOW_THREADS;
    if (o.snapshot) {
        snapshotOk = persistVerifySnapshot(o.p);
        err = persistLastError(o.p);
    }
    if (o.wal) {
        walOk = persistVerifyWAL(o.p);
        if (err == PERSIST_ERR_NONE) {
            err = persistLastError(o.p);
        }
    }
    Py_END_ALLOW_THREADS;
    dkpyClose(&o);

    PyObject *snap =
        snapshotOk < 0 ? Py_NewRef(Py_None) : PyBool_FromLong(snapshotOk);
    PyObject *wal = walOk < 0 ? Py_NewRef(Py_None) : PyBool_FromLong(walOk);
    return Py_BuildValue("(sNNs)", "OK", snap, wal, persistErrName(err));
}

/* ---- wal_walk(snapshot, wal, limit) -> (open, entries, report) ------- */

typedef struct dkpyWalk {
    PyObject *entries;
    Py_ssize_t taken;
    Py_ssize_t limit; /* entries to take (0: all) */
    bool failed;      /* a Python object could not be made */
} dkpyWalk;

static bool dkpyWalkEntry(const persistWALEntry *entry, void *ctx) {
    dkpyWalk *w = ctx;
    PyObject *row = Py_BuildValue(
        "(NNNsy#)", DKPY_U64(entry->sequence), DKPY_I64(entry->offset),
        DKPY_U64(entry->op), persistOpName(entry->op),
        (const char *)entry->data, (Py_ssize_t)entry->len);
    if (!row || PyList_Append(w->entries, row) < 0) {
        Py_XDECREF(row);
        w->failed = true;
        return false;
    }
    Py_DECREF(row);
    w->taken++;
    return !w->limit || w->taken < w->limit;
}

static PyObject *dkpyWalWalk(PyObject *, PyObject *args, PyObject *kwargs) {
    static char *const keywords[] = {"snapshot", "wal", "limit", nullptr};
    PyObject *snapshotArg = Py_None;
    PyObject *walArg = Py_None;
    Py_ssize_t limit = 0;
    dkpyOpened o;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|OOn:wal_walk", keywords,
                                     &snapshotArg, &walArg, &limit) ||
        !dkpyOpen(snapshotArg, walArg, &o)) {
        return nullptr;
    }
    if (limit < 0) {
        dkpyClose(&o);
        PyErr_SetString(PyExc_ValueError, "limit must be 0 (all) or more");
        return nullptr;
    }
    if (!o.p) {
        dkpyClose(&o);
        return Py_BuildValue("(sOO)", persistOpenResultName(o.why), Py_None,
                             Py_None);
    }

    /* The callback makes Python objects: the walk keeps the thread state */
    dkpyWalk w = {.entries = PyList_New(0), .limit = limit};
    persistWALWalkReport r = {};
    if (w.entries) {
        persistIterateWAL(o.p, dkpyWalkEntry, &w, &r);
    }
    dkpyClose(&o);
    if (!w.entries || w.failed) {
        Py_XDECREF(w.entries);
        return nullptr;
    }
    return Py_BuildValue("(sN{s:s,s:s,s:N,s:N,s:N})", "OK", w.entries, "stop",
                         persistWALStopName(r.stop), "bad_kind",
                         persistBadEntryKindName(r.badKind), "good_end",
                         DKPY_I64(r.goodEnd), "wal_end", DKPY_I64(r.walEnd),
                         "entries", DKPY_U64(r.entries));
}

/* ---- recover(snapshot, wal) -> (open, summary or None) --------------- */

static PyObject *dkpyRecover(PyObject *, PyObject *args, PyObject *kwargs) {
    PyObject *snapshotArg = Py_None;
    PyObject *walArg = Py_None;
    dkpyOpened o;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|OO:recover",
                                     dkpyOpenKeywords, &snapshotArg, &walArg) ||
        !dkpyOpen(snapshotArg, walArg, &o)) {
        return nullptr;
    }
    if (!o.p) {
        dkpyClose(&o);
        return Py_BuildValue("(sO)", persistOpenResultName(o.why), Py_None);
    }

    persistRecoverSummary sum = {};
    persistStats stats = {};
    Py_BEGIN_ALLOW_THREADS;
    persistRecoverSummarize(o.p, &sum);
    persistGetStats(o.p, &stats);
    Py_END_ALLOW_THREADS;
    dkpyClose(&o);

    return Py_BuildValue("(s{s:N,s:N,s:N,s:N,s:N,s:N})", "OK", "recovered",
                         PyBool_FromLong(sum.recovered), "valid",
                         PyBool_FromLong(sum.valid), "count",
                         DKPY_U64(sum.count), "bytes", DKPY_U64(sum.bytes),
                         "entries_recovered", DKPY_U64(stats.entriesRecovered),
                         "entries_skipped", DKPY_U64(stats.entriesSkipped));
}

/* ---- quarantine(path) -> (status, records, end, file_bytes) ---------- */

static PyObject *dkpyQuarantine(PyObject *, PyObject *arg) {
    PyObject *path = nullptr;
    if (!dkpyPath(arg, false, &path)) {
        return nullptr;
    }
    persistStore *store = nullptr;
    Py_BEGIN_ALLOW_THREADS;
    store = persistStoreFileReadOnly(dkpyPathChars(path));
    Py_END_ALLOW_THREADS;
    Py_DECREF(path);
    if (!store) {
        return Py_BuildValue("(sOOO)", "NO_FILE", Py_None, Py_None, Py_None);
    }

    PyObject *records = PyList_New(0);
    persistQuarantineRead end = PERSIST_QUARANTINE_END;
    while (records) {
        persistQuarantineRecord rec;
        uint8_t *bytes = nullptr;
        end = persistQuarantineNext(store, &rec, &bytes);
        if (end != PERSIST_QUARANTINE_RECORD) {
            break;
        }
        PyObject *row =
            Py_BuildValue("(NNNy#)", DKPY_U64(rec.at), DKPY_I64(rec.walOffset),
                          DKPY_U64(rec.nextSequence), (const char *)bytes,
                          (Py_ssize_t)rec.bytes);
        zfree(bytes);
        if (!row || PyList_Append(records, row) < 0) {
            Py_XDECREF(row);
            Py_CLEAR(records);
            break;
        }
        Py_DECREF(row);
    }
    const int64_t fileBytes = store->size(store->ctx);
    persistStoreClose(store);
    if (!records) {
        return nullptr;
    }
    return Py_BuildValue("(sNsN)", "OK", records,
                         persistQuarantineReadName(end), DKPY_I64(fileBytes));
}

PyMethodDef dkpyPersistMethods[] = {
    {"registry", dkpyRegistry, METH_NOARGS,
     "Every persisted type: (type_id, name, format_version, "
     "min_read_version)."},
    {"inspect", dkpyInspect, METH_O,
     "A file's header: (status, info dict or None)."},
    {"verify", (PyCFunction)(void (*)(void))dkpyVerify,
     METH_VARARGS | METH_KEYWORDS,
     "Check every envelope: (open status, snapshot ok, WAL ok, error)."},
    {"wal_walk", (PyCFunction)(void (*)(void))dkpyWalWalk,
     METH_VARARGS | METH_KEYWORDS,
     "A WAL's entries and where the walk stopped: (open status, entries, "
     "report)."},
    {"recover", (PyCFunction)(void (*)(void))dkpyRecover,
     METH_VARARGS | METH_KEYWORDS,
     "What recovery rebuilds: (open status, summary dict or None)."},
    {"quarantine", dkpyQuarantine, METH_O,
     "A quarantine file's records: (status, records, end, file bytes)."},
    {nullptr, nullptr, 0, nullptr},
};
