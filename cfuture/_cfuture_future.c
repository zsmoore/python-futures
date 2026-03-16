/*
 * cfuture/_cfuture_future.c
 * FutureObject Python type, task_new/free, callbacks, validate_fn.
 */

#include "_cfuture_internal.h"

/* Forward declaration — FutureType is defined at bottom of this file */
PyTypeObject FutureType;

Task *task_new(void) {
    Task *t = calloc(1, sizeof(Task));
    if (!t) return NULL;
    pthread_mutex_init(&t->lock, NULL);
    pthread_cond_init(&t->cond, NULL);
    return t;
}

void task_free(Task *t) {
    if (!t) return;
    if (t->result_sv) sv_free(t->result_sv);
    /* free callbacks */
    Callback *cb = t->callbacks;
    while (cb) {
        Callback *next = cb->next;
        Py_XDECREF(cb->code);
        for (int i = 0; i < cb->ndeps; i++)
            sv_free(cb->dep_templates[i]);
        free(cb->dep_templates);
        /* out_future is managed by Python refcount */
        free(cb);
        cb = next;
    }
    Py_XDECREF(t->fn_code);
    Py_XDECREF(t->fn_globals);
    pthread_mutex_destroy(&t->lock);
    pthread_cond_destroy(&t->cond);
    free(t);
}

static void future_dealloc(FutureObject *self) {
    if (self->owns_task)
        task_free(self->task);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *future_done(FutureObject *self, PyObject *_) {
    return PyBool_FromLong(self->task->done);
}

static PyObject *future_cancelled(FutureObject *self, PyObject *_) {
    return PyBool_FromLong(self->task->cancelled);
}

static PyObject *future_cancel(FutureObject *self, PyObject *_) {
    pthread_mutex_lock(&self->task->lock);
    int can_cancel = !self->task->started && !self->task->done;
    if (can_cancel) {
        self->task->cancelled = 1;
        self->task->done = 1;
        pthread_cond_broadcast(&self->task->cond);
    }
    pthread_mutex_unlock(&self->task->lock);
    return PyBool_FromLong(can_cancel);
}

static PyObject *future_result(FutureObject *self, PyObject *args, PyObject *kwargs) {
    static char *kwnames[] = {"timeout", NULL};
    PyObject *timeout_obj = Py_None;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O", kwnames, &timeout_obj))
        return NULL;

    struct timespec deadline;
    int has_timeout = 0;
    if (timeout_obj != Py_None) {
        has_timeout = 1;
        double secs = PyFloat_AsDouble(timeout_obj);
        if (secs < 0 && PyErr_Occurred()) {
            /* try as int */
            PyErr_Clear();
            secs = (double)PyLong_AsLong(timeout_obj);
            if (PyErr_Occurred()) return NULL;
        }
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec  += (time_t)secs;
        deadline.tv_nsec += (long)((secs - (time_t)secs) * 1e9);
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    Task *task = self->task;

    Py_BEGIN_ALLOW_THREADS
    pthread_mutex_lock(&task->lock);
    while (!task->done) {
        if (has_timeout) {
            int rc = pthread_cond_timedwait(&task->cond, &task->lock, &deadline);
            if (rc == ETIMEDOUT) break;
        } else {
            pthread_cond_wait(&task->cond, &task->lock);
        }
    }
    pthread_mutex_unlock(&task->lock);
    Py_END_ALLOW_THREADS

    if (!task->done) {
        PyErr_SetString(PyExc_TimeoutError, "cfuture: future timed out");
        return NULL;
    }
    if (task->cancelled) {
        PyErr_SetString(PyExc_RuntimeError, "cfuture: future was cancelled");
        return NULL;
    }
    if (task->failed) {
        PyErr_Format(PyExc_RuntimeError, "cfuture: task failed: [%s] %s",
                     task->exc_type, task->exc_msg);
        return NULL;
    }
    if (!task->result_sv) Py_RETURN_NONE;
    return sv_to_pyobject(task->result_sv);
}

/* validate callback fn: no free vars */
int validate_fn(PyObject *fn) {
    /* callable check */
    if (!PyCallable_Check(fn)) {
        PyErr_SetString(PyExc_TypeError, "cfuture: callback must be callable");
        return -1;
    }
    /* Check co_freevars */
    PyObject *code = PyObject_GetAttrString(fn, "__code__");
    if (!code) { PyErr_Clear(); return 0; } /* built-ins have no __code__, OK */
    PyObject *freevars = PyObject_GetAttrString(code, "co_freevars");
    Py_DECREF(code);
    if (!freevars) { PyErr_Clear(); return 0; }
    Py_ssize_t nfree = PyTuple_Check(freevars) ? PyTuple_GET_SIZE(freevars) : 0;
    Py_DECREF(freevars);
    if (nfree > 0) {
        PyErr_SetString(PyExc_ValueError,
            "cfuture: callback has free variables (closure). "
            "Pass captured values via deps=[...] instead.");
        return -1;
    }
    return 0;
}

PyObject *future_add_callback(FutureObject *self, PyObject *fn, PyObject *deps_list, int cb_type) {
    if (validate_fn(fn) < 0) return NULL;

    Callback *cb = calloc(1, sizeof(Callback));
    if (!cb) return PyErr_NoMemory();

    /* Extract code object */
    PyObject *code_attr = PyObject_GetAttrString(fn, "__code__");
    if (code_attr) {
        cb->code = code_attr; /* borrowed ref → we own it */
    } else {
        PyErr_Clear();
        cb->code = NULL;
    }
    /* Store the actual callable too, as PyObject* on a separate pointer:
     * We can't safely cross interpreter boundaries with a closure, but for
     * pure functions (no free vars) we store the code object and recreate
     * in the worker. However, recreating a function from code needs globals.
     * Instead we'll store the function itself and use _Py_CallableCheck style.
     *
     * Simplification: store fn as cb->code (a regular PyObject*) and call
     * directly in the same interpreter where the callback fires.
     *
     * For sub-interpreter safety, the proper approach would be to use
     * _PyObject_GetXIData / code objects. For this MVP we use the main
     * interpreter's callable but call it in worker context by temporarily
     * switching — this works only if fn has no GIL-shared mutable state.
     * The no-freevars rule enforces this.
     */
    Py_XDECREF(cb->code);
    cb->code = fn;
    Py_INCREF(fn);

    /* Encode deps */
    Py_ssize_t ndeps = 0;
    if (deps_list && deps_list != Py_None) {
        if (!PyList_Check(deps_list) && !PyTuple_Check(deps_list)) {
            PyErr_SetString(PyExc_TypeError, "cfuture: deps must be a list");
            Py_DECREF(fn);
            free(cb);
            return NULL;
        }
        ndeps = PySequence_Length(deps_list);
    }
    cb->ndeps = (int)ndeps;
    if (ndeps > 0) {
        cb->dep_templates = malloc(sizeof(SharedValue*) * (size_t)ndeps);
        if (!cb->dep_templates) {
            Py_DECREF(fn); free(cb);
            return PyErr_NoMemory();
        }
        for (Py_ssize_t i = 0; i < ndeps; i++) {
            PyObject *dep = PySequence_GetItem(deps_list, i);
            if (!dep) {
                for (Py_ssize_t j = 0; j < i; j++) sv_free(cb->dep_templates[j]);
                free(cb->dep_templates); Py_DECREF(fn); free(cb);
                return NULL;
            }
            cb->dep_templates[i] = sv_from_pyobject(dep);
            Py_DECREF(dep);
            if (!cb->dep_templates[i]) {
                for (Py_ssize_t j = 0; j < i; j++) sv_free(cb->dep_templates[j]);
                free(cb->dep_templates); Py_DECREF(fn); free(cb);
                return NULL;
            }
        }
    }
    cb->type = cb_type;

    /* Create output future */
    FutureObject *out = PyObject_New(FutureObject, &FutureType);
    if (!out) {
        for (int i = 0; i < cb->ndeps; i++) sv_free(cb->dep_templates[i]);
        free(cb->dep_templates); Py_DECREF(fn); free(cb);
        return NULL;
    }
    out->task = task_new();
    out->owns_task = 1;
    out->pool = self->pool;
    if (!out->task) {
        Py_DECREF(out);
        for (int i = 0; i < cb->ndeps; i++) sv_free(cb->dep_templates[i]);
        free(cb->dep_templates); Py_DECREF(fn); free(cb);
        return PyErr_NoMemory();
    }
    cb->out_future = out;

    /* Append callback to task */
    pthread_mutex_lock(&self->task->lock);
    Callback **p = &self->task->callbacks;
    while (*p) p = &(*p)->next;
    *p = cb;

    /* If task already done, fire immediately (inline) */
    int already_done = self->task->done;
    int already_failed = self->task->failed;
    int already_cancelled = self->task->cancelled;
    pthread_mutex_unlock(&self->task->lock);

    if (already_done && !already_cancelled) {
        /* We can fire synchronously here since we're in main interp */
        /* For simplicity we handle it here: */
        if ((cb_type == CB_THEN && !already_failed) ||
            (cb_type == CB_EXCEPT && already_failed) ||
            cb_type == CB_FINALLY)
        {
            PyObject *input = NULL;
            if (cb_type == CB_THEN || cb_type == CB_FINALLY) {
                input = self->task->result_sv ? sv_to_pyobject(self->task->result_sv) : Py_NewRef(Py_None);
            } else {
                /* CB_EXCEPT: pass exception info as string */
                input = PyUnicode_FromFormat("[%s] %s",
                    self->task->exc_type, self->task->exc_msg);
            }
            if (input) {
                /* Build deps tuple */
                PyObject *deps_tuple = PyTuple_New(cb->ndeps);
                int deps_ok = 1;
                for (int i = 0; i < cb->ndeps && deps_ok; i++) {
                    SharedValue *copy = sv_deep_copy(cb->dep_templates[i]);
                    PyObject *dep_obj = sv_to_pyobject(copy);
                    sv_free(copy);
                    if (!dep_obj) { deps_ok = 0; break; }
                    PyTuple_SET_ITEM(deps_tuple, i, dep_obj);
                }
                if (deps_ok) {
                    PyObject *result = PyObject_CallFunctionObjArgs(cb->code, input, deps_tuple, NULL);
                    if (result) {
                        out->task->result_sv = sv_from_pyobject(result);
                        Py_DECREF(result);
                    } else {
                        PyObject *exc = PyErr_GetRaisedException();
                        if (exc) {
                            PyObject *s = PyObject_Str(exc);
                            if (s) {
                                snprintf(out->task->exc_msg, sizeof(out->task->exc_msg),
                                         "%s", PyUnicode_AsUTF8(s));
                                Py_DECREF(s);
                            }
                            snprintf(out->task->exc_type, sizeof(out->task->exc_type),
                                     "%s", Py_TYPE(exc)->tp_name);
                            Py_DECREF(exc);
                        }
                        out->task->failed = 1;
                    }
                }
                Py_XDECREF(deps_tuple);
                Py_DECREF(input);
            }
            out->task->done = 1;
        } else {
            /* Callback type doesn't match — propagate result/error */
            if (self->task->result_sv)
                out->task->result_sv = sv_deep_copy(self->task->result_sv);
            memcpy(out->task->exc_type, self->task->exc_type, sizeof(self->task->exc_type));
            memcpy(out->task->exc_msg, self->task->exc_msg, sizeof(self->task->exc_msg));
            out->task->failed = self->task->failed;
            out->task->done = 1;
        }
        pthread_cond_broadcast(&out->task->cond);
    }

    return (PyObject *)out;
}

static PyObject *future_then(FutureObject *self, PyObject *args, PyObject *kwargs) {
    static char *kwnames[] = {"fn", "deps", NULL};
    PyObject *fn = NULL, *deps = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O", kwnames, &fn, &deps))
        return NULL;
    return future_add_callback(self, fn, deps, CB_THEN);
}

static PyObject *future_except_(FutureObject *self, PyObject *args, PyObject *kwargs) {
    static char *kwnames[] = {"fn", "deps", NULL};
    PyObject *fn = NULL, *deps = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O", kwnames, &fn, &deps))
        return NULL;
    return future_add_callback(self, fn, deps, CB_EXCEPT);
}

static PyObject *future_finally_(FutureObject *self, PyObject *args, PyObject *kwargs) {
    static char *kwnames[] = {"fn", "deps", NULL};
    PyObject *fn = NULL, *deps = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O", kwnames, &fn, &deps))
        return NULL;
    return future_add_callback(self, fn, deps, CB_FINALLY);
}

PyObject *future_completed(PyObject *cls, PyObject *args) {
    PyObject *value = Py_None;
    if (!PyArg_ParseTuple(args, "|O", &value)) return NULL;
    FutureObject *f = PyObject_New(FutureObject, &FutureType);
    if (!f) return NULL;
    f->task = task_new();
    f->owns_task = 1;
    f->pool = NULL;
    if (!f->task) { Py_DECREF(f); return PyErr_NoMemory(); }
    f->task->result_sv = sv_from_pyobject(value);
    f->task->done = 1;
    return (PyObject *)f;
}

static PyObject *future_failed_cls(PyObject *cls, PyObject *args) {
    const char *msg = "failed";
    if (!PyArg_ParseTuple(args, "|s", &msg)) return NULL;
    FutureObject *f = PyObject_New(FutureObject, &FutureType);
    if (!f) return NULL;
    f->task = task_new();
    f->owns_task = 1;
    f->pool = NULL;
    if (!f->task) { Py_DECREF(f); return PyErr_NoMemory(); }
    snprintf(f->task->exc_type, sizeof(f->task->exc_type), "RuntimeError");
    snprintf(f->task->exc_msg, sizeof(f->task->exc_msg), "%s", msg);
    f->task->failed = 1;
    f->task->done = 1;
    return (PyObject *)f;
}

static PyMethodDef future_methods[] = {
    {"done",      (PyCFunction)future_done,      METH_NOARGS,  "Return True if done."},
    {"cancelled", (PyCFunction)future_cancelled,  METH_NOARGS,  "Return True if cancelled."},
    {"cancel",    (PyCFunction)future_cancel,     METH_NOARGS,  "Cancel if not started."},
    {"result",    (PyCFunction)future_result,     METH_VARARGS|METH_KEYWORDS, "Block for result."},
    {"then",      (PyCFunction)future_then,       METH_VARARGS|METH_KEYWORDS, "Chain callback on success."},
    {"except_",   (PyCFunction)future_except_,    METH_VARARGS|METH_KEYWORDS, "Chain callback on failure."},
    {"finally_",  (PyCFunction)future_finally_,   METH_VARARGS|METH_KEYWORDS, "Chain callback always."},
    {"completed", (PyCFunction)future_completed,  METH_VARARGS|METH_CLASS,    "Pre-resolved future."},
    {"failed",    (PyCFunction)future_failed_cls, METH_VARARGS|METH_CLASS,    "Pre-failed future."},
    {NULL}
};

PyTypeObject FutureType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "cfuture.Future",
    .tp_basicsize = sizeof(FutureObject),
    .tp_dealloc   = (destructor)future_dealloc,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = "A cfuture Future.",
    .tp_methods   = future_methods,
};
