/* datakit._datakit: the module.  Its functions hold no state between
 * calls, so it runs without the GIL and in any number of threads */

#include "dkpy.h"

int dkpyPath(PyObject *arg, bool optional, PyObject **out) {
    *out = nullptr;
    if (optional && arg == Py_None) {
        return 1;
    }
    return PyUnicode_FSConverter(arg, out);
}

const char *dkpyPathChars(PyObject *path) {
    return path ? PyBytes_AsString(path) : nullptr;
}

static int dkpyExec(PyObject *module) {
    return PyModule_AddFunctions(module, dkpyTestingMethods);
}

/* The ABI this module was built for (abi3t), checked at import */
PyABIInfo_VAR(dkpyAbi);

PyMODEXPORT_FUNC PyModExport__datakit(void) {
    static PySlot slots[] = {
        PySlot_STATIC_DATA(Py_mod_abi, &dkpyAbi),
        PySlot_STATIC_DATA(Py_mod_name, "datakit._datakit"),
        PySlot_STATIC_DATA(Py_mod_doc, "The C layer under the datakit package"),
        PySlot_STATIC_DATA(Py_mod_methods, dkpyPersistMethods),
        PySlot_FUNC(Py_mod_exec, dkpyExec),
        PySlot_DATA(Py_mod_gil, Py_MOD_GIL_NOT_USED),
        PySlot_DATA(Py_mod_multiple_interpreters,
                    Py_MOD_MULTIPLE_INTERPRETERS_NOT_SUPPORTED),
        PySlot_END,
    };
    return slots;
}
