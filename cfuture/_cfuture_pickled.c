/*
 * cfuture/_cfuture_pickled.c
 * PickledObject type and cfuture_pickled() module function.
 */

#include "_cfuture_internal.h"

typedef struct {
    PyObject_HEAD
    PyObject *value;
} PickledObject;

static void pickled_dealloc(PickledObject *self) {
    Py_XDECREF(self->value);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int pickled_init(PickledObject *self, PyObject *args, PyObject *kwargs) {
    PyObject *value = NULL;
    if (!PyArg_ParseTuple(args, "O", &value)) return -1;
    Py_XDECREF(self->value);
    self->value = value;
    Py_INCREF(value);

    /* Set sentinel attribute */
    if (PyObject_SetAttrString((PyObject *)self, "__cfuture_pickled__", Py_True) < 0) {
        /* ignore — we check type instead */
        PyErr_Clear();
    }
    return 0;
}

static PyObject *pickled_get_cfuture_pickled(PickledObject *self, void *closure) {
    Py_RETURN_TRUE;
}

static PyObject *pickled_get_value(PickledObject *self, void *closure) {
    if (!self->value) Py_RETURN_NONE;
    Py_INCREF(self->value);
    return self->value;
}

static PyGetSetDef pickled_getset[] = {
    {"__cfuture_pickled__", (getter)pickled_get_cfuture_pickled, NULL, "sentinel", NULL},
    {"value", (getter)pickled_get_value, NULL, "wrapped value", NULL},
    {NULL}
};

PyTypeObject PickledType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "cfuture.Pickled",
    .tp_basicsize = sizeof(PickledObject),
    .tp_dealloc   = (destructor)pickled_dealloc,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = "Pickle-transfer wrapper for cfuture deps.",
    .tp_getset    = pickled_getset,
    .tp_init      = (initproc)pickled_init,
    .tp_new       = PyType_GenericNew,
};

PyObject *cfuture_pickled(PyObject *module, PyObject *args) {
    return PyObject_Call((PyObject *)&PickledType, args, NULL);
}
