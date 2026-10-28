#pragma once

/* The C layer under the datakit Python package: shared pieces.  Each
 * function calls datakit and converts at the boundary; every decode, check
 * and repair is datakit's own, tested there. */

#include <Python.h>

#include <stdint.h>

/* An int from a fixed-width integer (no long long at the boundary) */
#define DKPY_U64(v) PyLong_FromUInt64((uint64_t)(v))
#define DKPY_I64(v) PyLong_FromInt64((int64_t)(v))

/* The filesystem path in 'arg' (str, bytes or os.PathLike) as a bytes
 * object (*out, a new reference) whose buffer is the path; None gives
 * *out == NULL when 'optional'.  0 with an exception set on failure */
int dkpyPath(PyObject *arg, bool optional, PyObject **out);

/* The buffer of a dkpyPath result, or NULL for an absent path */
const char *dkpyPathChars(PyObject *path);

/* The functions of datakit.persist's C layer (the module's table) and
 * of test support (added when the module executes) */
extern PyMethodDef dkpyPersistMethods[];
extern PyMethodDef dkpyTestingMethods[];
