/*
 * cfuture/_cfuture.c
 * Module initialisation only — all implementation lives in the other TUs.
 * CompletableFuture-style futures with true GIL-free parallelism via sub-interpreters.
 * Requires Python 3.12+
 */

#include "_cfuture_internal.h"

/* cfuture_pickled declared in _cfuture_pickled.c */
PyObject *cfuture_pickled(PyObject *module, PyObject *args);

/* cfuture_all_of declared in _cfuture_worker.c */
PyObject *cfuture_all_of(PyObject *module, PyObject *args);

static PyMethodDef cfuture_methods[] = {
    {"all_of",  cfuture_all_of,  METH_VARARGS, "Wait for all futures (non-blocking)."},
    {"pickled", cfuture_pickled, METH_VARARGS, "Wrap an object for pickle-based transfer."},
    {NULL}
};

static struct PyModuleDef cfuture_module = {
    PyModuleDef_HEAD_INIT,
    "cfuture._cfuture",
    "CompletableFuture-style futures with GIL-free parallelism.",
    -1,
    cfuture_methods,
};

PyMODINIT_FUNC PyInit__cfuture(void) {
    if (PyType_Ready(&FutureType) < 0) return NULL;
    if (PyType_Ready(&ThreadPoolExecutorType) < 0) return NULL;
    if (PyType_Ready(&AllOfCallbackType) < 0) return NULL;
    if (PyType_Ready(&PickledType) < 0) return NULL;

    PyObject *m = PyModule_Create(&cfuture_module);
    if (!m) return NULL;

    Py_INCREF(&FutureType);
    PyModule_AddObject(m, "Future", (PyObject *)&FutureType);

    Py_INCREF(&ThreadPoolExecutorType);
    PyModule_AddObject(m, "ThreadPoolExecutor", (PyObject *)&ThreadPoolExecutorType);

    Py_INCREF(&PickledType);
    PyModule_AddObject(m, "Pickled", (PyObject *)&PickledType);

    return m;
}
