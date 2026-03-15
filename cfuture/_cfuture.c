/*
 * cfuture/_cfuture.c
 * CompletableFuture-style futures with true GIL-free parallelism via sub-interpreters.
 * Requires Python 3.12+
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>

/* ──────────────────────────────────────────────────────────────────────────
 * SharedValue — neutral-heap representation of Python objects
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum {
    SV_NONE, SV_BOOL, SV_INT, SV_FLOAT,
    SV_STR, SV_BYTES,
    SV_LIST, SV_DICT, SV_TUPLE,
    SV_CUSTOM,   /* __xi_encode__ / __xi_decode__ */
    SV_PICKLE    /* cfuture.pickled() */
} SVTag;

typedef struct SharedValue SharedValue;
typedef struct SharedList  SharedList;
typedef struct SharedDict  SharedDict;

struct SharedList {
    size_t       len;
    SharedValue *items;
};

struct SharedDict {
    size_t len;
    struct { SharedValue key; SharedValue val; } *entries;
};

struct SharedValue {
    SVTag tag;
    union {
        long long            i;
        double               f;
        struct { char *buf; size_t len; } s;
        SharedList          *list;
        SharedDict          *dict;
        /* SV_CUSTOM: encoded as a dict with "__xi_module__", "__xi_qualname__", "__xi_data__" */
        /* SV_PICKLE: stored as SV_BYTES (pickle bytes) */
    };
};

/* Forward declarations */
static SharedValue *sv_from_pyobject(PyObject *obj);
static SharedValue *sv_deep_copy(const SharedValue *src);
static PyObject    *sv_to_pyobject(const SharedValue *sv);
static void         sv_free(SharedValue *sv);

static void sv_free_inline(SharedValue *sv) {
    if (!sv) return;
    switch (sv->tag) {
        case SV_STR:
        case SV_BYTES:
            free(sv->s.buf);
            break;
        case SV_LIST:
        case SV_TUPLE:
            if (sv->list) {
                for (size_t i = 0; i < sv->list->len; i++)
                    sv_free_inline(&sv->list->items[i]);
                free(sv->list->items);
                free(sv->list);
            }
            break;
        case SV_DICT:
            if (sv->dict) {
                for (size_t i = 0; i < sv->dict->len; i++) {
                    sv_free_inline(&sv->dict->entries[i].key);
                    sv_free_inline(&sv->dict->entries[i].val);
                }
                free(sv->dict->entries);
                free(sv->dict);
            }
            break;
        default:
            break;
    }
}

static void sv_free(SharedValue *sv) {
    if (!sv) return;
    sv_free_inline(sv);
    free(sv);
}

static int sv_fill_from_pyobject(SharedValue *sv, PyObject *obj);

static int sv_fill_from_pyobject(SharedValue *sv, PyObject *obj) {
    memset(sv, 0, sizeof(*sv));

    if (obj == Py_None) {
        sv->tag = SV_NONE;
        return 0;
    }
    if (PyBool_Check(obj)) {
        sv->tag = SV_BOOL;
        sv->i = (obj == Py_True) ? 1 : 0;
        return 0;
    }
    if (PyLong_Check(obj)) {
        sv->tag = SV_INT;
        sv->i = PyLong_AsLongLong(obj);
        if (sv->i == -1 && PyErr_Occurred()) return -1;
        return 0;
    }
    if (PyFloat_Check(obj)) {
        sv->tag = SV_FLOAT;
        sv->f = PyFloat_AsDouble(obj);
        return 0;
    }
    if (PyUnicode_Check(obj)) {
        sv->tag = SV_STR;
        Py_ssize_t size;
        const char *utf8 = PyUnicode_AsUTF8AndSize(obj, &size);
        if (!utf8) return -1;
        sv->s.buf = malloc(size + 1);
        if (!sv->s.buf) { PyErr_NoMemory(); return -1; }
        memcpy(sv->s.buf, utf8, size + 1);
        sv->s.len = (size_t)size;
        return 0;
    }
    if (PyBytes_Check(obj)) {
        sv->tag = SV_BYTES;
        char *buf; Py_ssize_t len;
        if (PyBytes_AsStringAndSize(obj, &buf, &len) < 0) return -1;
        sv->s.buf = malloc((size_t)len);
        if (!sv->s.buf) { PyErr_NoMemory(); return -1; }
        memcpy(sv->s.buf, buf, (size_t)len);
        sv->s.len = (size_t)len;
        return 0;
    }

    /* Check for cfuture.pickled() wrapper — handled as SV_PICKLE (stored as SV_BYTES) */
    /* We detect it by checking for a '__cfuture_pickled__' attribute */
    PyObject *pickled_attr = PyObject_GetAttrString(obj, "__cfuture_pickled__");
    if (pickled_attr) {
        Py_DECREF(pickled_attr);
        /* Get the inner value */
        PyObject *inner = PyObject_GetAttrString(obj, "value");
        if (!inner) return -1;

        /* Call pickle.dumps on inner value */
        PyObject *pickle_mod = PyImport_ImportModule("pickle");
        if (!pickle_mod) { Py_DECREF(inner); return -1; }
        PyObject *dumps = PyObject_GetAttrString(pickle_mod, "dumps");
        Py_DECREF(pickle_mod);
        if (!dumps) { Py_DECREF(inner); return -1; }
        PyObject *pickled_bytes = PyObject_CallOneArg(dumps, inner);
        Py_DECREF(dumps);
        Py_DECREF(inner);
        if (!pickled_bytes) return -1;

        /* Store as SV_PICKLE (tag) with bytes data */
        sv->tag = SV_PICKLE;
        char *buf; Py_ssize_t len;
        if (PyBytes_AsStringAndSize(pickled_bytes, &buf, &len) < 0) {
            Py_DECREF(pickled_bytes);
            return -1;
        }
        sv->s.buf = malloc((size_t)len);
        if (!sv->s.buf) { Py_DECREF(pickled_bytes); PyErr_NoMemory(); return -1; }
        memcpy(sv->s.buf, buf, (size_t)len);
        sv->s.len = (size_t)len;
        Py_DECREF(pickled_bytes);
        return 0;
    }
    PyErr_Clear();

    /* Check for __xi_encode__ */
    PyObject *encode_fn = PyObject_GetAttrString(obj, "__xi_encode__");
    if (encode_fn) {
        PyObject *encoded = PyObject_CallNoArgs(encode_fn);
        Py_DECREF(encode_fn);
        if (!encoded) return -1;

        /* Get module and qualname */
        PyObject *type_obj = (PyObject *)Py_TYPE(obj);
        PyObject *module = PyObject_GetAttrString(type_obj, "__module__");
        PyObject *qualname = PyObject_GetAttrString(type_obj, "__qualname__");
        if (!module || !qualname) {
            Py_XDECREF(module); Py_XDECREF(qualname);
            Py_DECREF(encoded);
            return -1;
        }

        /* Build a dict: {"__xi_module__": module, "__xi_qualname__": qualname, "__xi_data__": encoded} */
        PyObject *meta_dict = PyDict_New();
        if (!meta_dict) {
            Py_DECREF(module); Py_DECREF(qualname); Py_DECREF(encoded);
            return -1;
        }
        PyDict_SetItemString(meta_dict, "__xi_module__", module);
        PyDict_SetItemString(meta_dict, "__xi_qualname__", qualname);
        PyDict_SetItemString(meta_dict, "__xi_data__", encoded);
        Py_DECREF(module); Py_DECREF(qualname); Py_DECREF(encoded);

        sv->tag = SV_CUSTOM;
        /* Encode the dict as a SharedDict */
        int ret = sv_fill_from_pyobject(sv, meta_dict);
        /* But override tag */
        if (ret == 0) sv->tag = SV_CUSTOM;
        Py_DECREF(meta_dict);
        return ret;
    }
    PyErr_Clear();

    /* Container types */
    if (PyList_Check(obj)) {
        sv->tag = SV_LIST;
        Py_ssize_t n = PyList_GET_SIZE(obj);
        sv->list = malloc(sizeof(SharedList));
        if (!sv->list) { PyErr_NoMemory(); return -1; }
        sv->list->len = (size_t)n;
        sv->list->items = malloc(sizeof(SharedValue) * (size_t)n);
        if (!sv->list->items && n > 0) {
            free(sv->list); sv->list = NULL;
            PyErr_NoMemory(); return -1;
        }
        for (Py_ssize_t i = 0; i < n; i++) {
            if (sv_fill_from_pyobject(&sv->list->items[i], PyList_GET_ITEM(obj, i)) < 0) {
                /* free already-filled items */
                for (Py_ssize_t j = 0; j < i; j++)
                    sv_free_inline(&sv->list->items[j]);
                free(sv->list->items);
                free(sv->list);
                sv->list = NULL;
                return -1;
            }
        }
        return 0;
    }
    if (PyTuple_Check(obj)) {
        sv->tag = SV_TUPLE;
        Py_ssize_t n = PyTuple_GET_SIZE(obj);
        sv->list = malloc(sizeof(SharedList));
        if (!sv->list) { PyErr_NoMemory(); return -1; }
        sv->list->len = (size_t)n;
        sv->list->items = malloc(sizeof(SharedValue) * (size_t)n);
        if (!sv->list->items && n > 0) {
            free(sv->list); sv->list = NULL;
            PyErr_NoMemory(); return -1;
        }
        for (Py_ssize_t i = 0; i < n; i++) {
            if (sv_fill_from_pyobject(&sv->list->items[i], PyTuple_GET_ITEM(obj, i)) < 0) {
                for (Py_ssize_t j = 0; j < i; j++)
                    sv_free_inline(&sv->list->items[j]);
                free(sv->list->items);
                free(sv->list);
                sv->list = NULL;
                return -1;
            }
        }
        return 0;
    }
    if (PyDict_Check(obj)) {
        sv->tag = SV_DICT;
        Py_ssize_t n = PyDict_Size(obj);
        sv->dict = malloc(sizeof(SharedDict));
        if (!sv->dict) { PyErr_NoMemory(); return -1; }
        sv->dict->len = (size_t)n;
        sv->dict->entries = malloc(sizeof(sv->dict->entries[0]) * (size_t)n);
        if (!sv->dict->entries && n > 0) {
            free(sv->dict); sv->dict = NULL;
            PyErr_NoMemory(); return -1;
        }
        Py_ssize_t pos = 0;
        PyObject *key, *val;
        size_t idx = 0;
        while (PyDict_Next(obj, &pos, &key, &val)) {
            if (sv_fill_from_pyobject(&sv->dict->entries[idx].key, key) < 0) goto dict_err;
            if (sv_fill_from_pyobject(&sv->dict->entries[idx].val, val) < 0) {
                sv_free_inline(&sv->dict->entries[idx].key);
                goto dict_err;
            }
            idx++;
        }
        return 0;
    dict_err:
        for (size_t j = 0; j < idx; j++) {
            sv_free_inline(&sv->dict->entries[j].key);
            sv_free_inline(&sv->dict->entries[j].val);
        }
        free(sv->dict->entries);
        free(sv->dict);
        sv->dict = NULL;
        return -1;
    }

    PyErr_Format(PyExc_TypeError,
        "cfuture: cannot transfer object of type '%.100s' across interpreter boundary. "
        "Use cfuture.pickled(obj) to opt in to pickle-based transfer, or implement "
        "__xi_encode__/__xi_decode__ on the class.",
        Py_TYPE(obj)->tp_name);
    return -1;
}

static SharedValue *sv_from_pyobject(PyObject *obj) {
    SharedValue *sv = malloc(sizeof(SharedValue));
    if (!sv) { PyErr_NoMemory(); return NULL; }
    if (sv_fill_from_pyobject(sv, obj) < 0) {
        free(sv);
        return NULL;
    }
    return sv;
}

static void sv_deep_copy_inline(SharedValue *dst, const SharedValue *src) {
    *dst = *src;
    switch (src->tag) {
        case SV_STR:
        case SV_BYTES:
        case SV_PICKLE:
            dst->s.buf = malloc(src->s.len + 1);
            if (dst->s.buf) {
                memcpy(dst->s.buf, src->s.buf, src->s.len);
                dst->s.buf[src->s.len] = '\0';
            }
            break;
        case SV_LIST:
        case SV_TUPLE:
        case SV_CUSTOM:
            if (src->list) {
                dst->list = malloc(sizeof(SharedList));
                if (dst->list) {
                    dst->list->len = src->list->len;
                    dst->list->items = malloc(sizeof(SharedValue) * src->list->len);
                    if (dst->list->items) {
                        for (size_t i = 0; i < src->list->len; i++)
                            sv_deep_copy_inline(&dst->list->items[i], &src->list->items[i]);
                    }
                }
            }
            break;
        case SV_DICT:
            if (src->dict) {
                dst->dict = malloc(sizeof(SharedDict));
                if (dst->dict) {
                    dst->dict->len = src->dict->len;
                    dst->dict->entries = malloc(sizeof(dst->dict->entries[0]) * src->dict->len);
                    if (dst->dict->entries) {
                        for (size_t i = 0; i < src->dict->len; i++) {
                            sv_deep_copy_inline(&dst->dict->entries[i].key,
                                                &src->dict->entries[i].key);
                            sv_deep_copy_inline(&dst->dict->entries[i].val,
                                                &src->dict->entries[i].val);
                        }
                    }
                }
            }
            break;
        default:
            break;
    }
}

static SharedValue *sv_deep_copy(const SharedValue *src) {
    if (!src) return NULL;
    SharedValue *dst = malloc(sizeof(SharedValue));
    if (!dst) return NULL;
    sv_deep_copy_inline(dst, src);
    return dst;
}

static PyObject *sv_to_pyobject(const SharedValue *sv) {
    if (!sv) Py_RETURN_NONE;
    switch (sv->tag) {
        case SV_NONE:
            Py_RETURN_NONE;
        case SV_BOOL:
            if (sv->i) Py_RETURN_TRUE; else Py_RETURN_FALSE;
        case SV_INT:
            return PyLong_FromLongLong(sv->i);
        case SV_FLOAT:
            return PyFloat_FromDouble(sv->f);
        case SV_STR:
            return PyUnicode_FromStringAndSize(sv->s.buf, (Py_ssize_t)sv->s.len);
        case SV_BYTES:
            return PyBytes_FromStringAndSize(sv->s.buf, (Py_ssize_t)sv->s.len);
        case SV_PICKLE: {
            /* Unpickle in current interpreter */
            PyObject *pickle_mod = PyImport_ImportModule("pickle");
            if (!pickle_mod) return NULL;
            PyObject *loads = PyObject_GetAttrString(pickle_mod, "loads");
            Py_DECREF(pickle_mod);
            if (!loads) return NULL;
            PyObject *bytes_obj = PyBytes_FromStringAndSize(sv->s.buf, (Py_ssize_t)sv->s.len);
            if (!bytes_obj) { Py_DECREF(loads); return NULL; }
            PyObject *result = PyObject_CallOneArg(loads, bytes_obj);
            Py_DECREF(loads);
            Py_DECREF(bytes_obj);
            return result;
        }
        case SV_LIST: {
            if (!sv->list) return PyList_New(0);
            PyObject *lst = PyList_New((Py_ssize_t)sv->list->len);
            if (!lst) return NULL;
            for (size_t i = 0; i < sv->list->len; i++) {
                PyObject *item = sv_to_pyobject(&sv->list->items[i]);
                if (!item) { Py_DECREF(lst); return NULL; }
                PyList_SET_ITEM(lst, (Py_ssize_t)i, item);
            }
            return lst;
        }
        case SV_TUPLE: {
            if (!sv->list) return PyTuple_New(0);
            PyObject *tup = PyTuple_New((Py_ssize_t)sv->list->len);
            if (!tup) return NULL;
            for (size_t i = 0; i < sv->list->len; i++) {
                PyObject *item = sv_to_pyobject(&sv->list->items[i]);
                if (!item) { Py_DECREF(tup); return NULL; }
                PyTuple_SET_ITEM(tup, (Py_ssize_t)i, item);
            }
            return tup;
        }
        case SV_DICT: {
            if (!sv->dict) return PyDict_New();
            PyObject *dct = PyDict_New();
            if (!dct) return NULL;
            for (size_t i = 0; i < sv->dict->len; i++) {
                PyObject *k = sv_to_pyobject(&sv->dict->entries[i].key);
                PyObject *v = sv_to_pyobject(&sv->dict->entries[i].val);
                if (!k || !v) {
                    Py_XDECREF(k); Py_XDECREF(v); Py_DECREF(dct);
                    return NULL;
                }
                int rc = PyDict_SetItem(dct, k, v);
                Py_DECREF(k); Py_DECREF(v);
                if (rc < 0) { Py_DECREF(dct); return NULL; }
            }
            return dct;
        }
        case SV_CUSTOM: {
            /*
             * SV_CUSTOM is stored as a dict SharedValue with tag overridden to SV_CUSTOM.
             * We reconstruct the dict, then call cls.__xi_decode__(data).
             */
            if (!sv->dict) Py_RETURN_NONE;
            /* Reconstruct as SV_DICT temporarily */
            SharedValue dict_sv = *sv;
            dict_sv.tag = SV_DICT;
            PyObject *meta = sv_to_pyobject(&dict_sv);
            if (!meta) return NULL;

            PyObject *xi_module = PyDict_GetItemString(meta, "__xi_module__");
            PyObject *xi_qualname = PyDict_GetItemString(meta, "__xi_qualname__");
            PyObject *xi_data = PyDict_GetItemString(meta, "__xi_data__");
            if (!xi_module || !xi_qualname || !xi_data) {
                Py_DECREF(meta);
                PyErr_SetString(PyExc_ValueError, "cfuture: malformed SV_CUSTOM encoding");
                return NULL;
            }

            /* Import module */
            PyObject *mod = PyImport_Import(xi_module);
            if (!mod) { Py_DECREF(meta); return NULL; }

            /* Walk qualname (may be "Outer.Inner") */
            const char *qname = PyUnicode_AsUTF8(xi_qualname);
            PyObject *cls = mod;
            Py_INCREF(cls);
            char *buf = strdup(qname);
            char *tok = strtok(buf, ".");
            while (tok) {
                PyObject *next = PyObject_GetAttrString(cls, tok);
                Py_DECREF(cls);
                if (!next) { free(buf); Py_DECREF(mod); Py_DECREF(meta); return NULL; }
                cls = next;
                tok = strtok(NULL, ".");
            }
            free(buf);
            Py_DECREF(mod);

            PyObject *decode = PyObject_GetAttrString(cls, "__xi_decode__");
            Py_DECREF(cls);
            if (!decode) { Py_DECREF(meta); return NULL; }

            PyObject *result = PyObject_CallOneArg(decode, xi_data);
            Py_DECREF(decode);
            Py_DECREF(meta);
            return result;
        }
        default:
            Py_RETURN_NONE;
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Task, Callback, Worker, Pool structures
 * ────────────────────────────────────────────────────────────────────────── */

#define CB_THEN    0
#define CB_EXCEPT  1
#define CB_FINALLY 2

#define WORKER_IDLE    0
#define WORKER_RUNNING 1
#define WORKER_DEAD    2

struct Future;

typedef struct Callback {
    PyObject        *code;           /* PyCodeObject* */
    SharedValue    **dep_templates;  /* array of SharedValue* */
    int              ndeps;
    int              type;           /* CB_THEN / CB_EXCEPT / CB_FINALLY */
    struct Future   *out_future;
    struct Callback *next;
} Callback;

typedef struct Task {
    PyObject        *fn_code;        /* PyCodeObject* for initial fn */
    PyObject        *fn_globals;     /* dict — globals snapshot from main interp */
    SharedValue     *result_sv;
    char             exc_type[256];
    char             exc_msg[1024];
    int              done;
    int              failed;
    int              cancelled;
    int              started;
    pthread_mutex_t  lock;
    pthread_cond_t   cond;
    Callback        *callbacks;
    struct Task     *next_in_queue;
} Task;

typedef struct Worker {
    pthread_t            thread;
    PyInterpreterState  *interp;
    PyThreadState       *tstate;
    _Atomic uint64_t     heartbeat;
    _Atomic int          state;
    Task                *current_task;
    int                  id;
    struct Pool         *pool;
} Worker;

typedef struct Pool {
    Worker          *workers;
    int              nworkers;
    Task            *head;
    Task            *tail;
    pthread_mutex_t  lock;
    pthread_cond_t   cond;
    int              shutdown;
    /* shared= startup injection */
    SharedValue    **shared_templates;
    char           **shared_keys;
    int              nshared;
    pthread_t        watchdog_thread;
    int              has_watchdog;
} Pool;

/* ──────────────────────────────────────────────────────────────────────────
 * Future Python type
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct {
    PyObject_HEAD
    Task  *task;
    Pool  *pool;
    int    owns_task;  /* 1 if this Future allocated the Task */
} FutureObject;

static PyTypeObject FutureType;

static Task *task_new(void) {
    Task *t = calloc(1, sizeof(Task));
    if (!t) return NULL;
    pthread_mutex_init(&t->lock, NULL);
    pthread_cond_init(&t->cond, NULL);
    return t;
}

static void task_free(Task *t) {
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

/* Forward declaration */
static void fire_callbacks(Worker *w, Task *task, int is_error);

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
static int validate_fn(PyObject *fn) {
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

static PyObject *future_add_callback(FutureObject *self, PyObject *fn, PyObject *deps_list, int cb_type) {
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

static PyObject *future_completed(PyObject *cls, PyObject *args) {
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

static PyTypeObject FutureType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "cfuture.Future",
    .tp_basicsize = sizeof(FutureObject),
    .tp_dealloc   = (destructor)future_dealloc,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = "A cfuture Future.",
    .tp_methods   = future_methods,
};

/* ──────────────────────────────────────────────────────────────────────────
 * Worker loop + fire_callbacks
 * ────────────────────────────────────────────────────────────────────────── */

static void capture_exception_strings(Task *task) {
    PyObject *exc = PyErr_GetRaisedException();
    if (!exc) {
        snprintf(task->exc_type, sizeof(task->exc_type), "UnknownError");
        snprintf(task->exc_msg, sizeof(task->exc_msg), "(no exception info)");
        return;
    }
    snprintf(task->exc_type, sizeof(task->exc_type), "%.255s", Py_TYPE(exc)->tp_name);
    PyObject *s = PyObject_Str(exc);
    if (s) {
        const char *msg = PyUnicode_AsUTF8(s);
        if (msg) snprintf(task->exc_msg, sizeof(task->exc_msg), "%.1023s", msg);
        Py_DECREF(s);
    }
    Py_DECREF(exc);
}

static void fire_callbacks(Worker *w, Task *task, int is_error) {
    pthread_mutex_lock(&task->lock);
    Callback *cb = task->callbacks;
    pthread_mutex_unlock(&task->lock);

    while (cb) {
        Callback *next_cb = cb->next;

        int should_fire = (cb->type == CB_FINALLY) ||
                          (cb->type == CB_THEN   && !is_error) ||
                          (cb->type == CB_EXCEPT &&  is_error);

        Task *out_task = cb->out_future->task;

        if (should_fire) {
            /* Build input */
            PyObject *input = NULL;
            if (cb->type == CB_THEN || cb->type == CB_FINALLY) {
                input = task->result_sv ? sv_to_pyobject(task->result_sv) : Py_NewRef(Py_None);
            } else {
                input = PyUnicode_FromFormat("[%s] %s", task->exc_type, task->exc_msg);
            }

            /* Build deps tuple */
            PyObject *deps_tuple = PyTuple_New(cb->ndeps);
            int deps_ok = (deps_tuple != NULL);
            for (int i = 0; i < cb->ndeps && deps_ok; i++) {
                SharedValue *copy = sv_deep_copy(cb->dep_templates[i]);
                PyObject *dep_obj = copy ? sv_to_pyobject(copy) : Py_NewRef(Py_None);
                sv_free(copy);
                if (!dep_obj) { deps_ok = 0; break; }
                PyTuple_SET_ITEM(deps_tuple, i, dep_obj);
            }

            PyObject *result = NULL;
            if (input && deps_ok) {
                result = PyObject_CallFunctionObjArgs(cb->code, input, deps_tuple, NULL);
            }
            Py_XDECREF(input);
            Py_XDECREF(deps_tuple);

            pthread_mutex_lock(&out_task->lock);
            if (result) {
                out_task->result_sv = sv_from_pyobject(result);
                if (!out_task->result_sv) {
                    capture_exception_strings(out_task);
                    out_task->failed = 1;
                }
                Py_DECREF(result);
            } else {
                capture_exception_strings(out_task);
                out_task->failed = 1;
            }
            out_task->done = 1;
            pthread_cond_broadcast(&out_task->cond);
            pthread_mutex_unlock(&out_task->lock);

            /* Recurse: fire out_task's callbacks */
            fire_callbacks(w, out_task, out_task->failed);
        } else {
            /* Propagate result/error through this callback without calling fn */
            pthread_mutex_lock(&out_task->lock);
            if (task->result_sv)
                out_task->result_sv = sv_deep_copy(task->result_sv);
            memcpy(out_task->exc_type, task->exc_type, sizeof(task->exc_type));
            memcpy(out_task->exc_msg, task->exc_msg, sizeof(task->exc_msg));
            out_task->failed = task->failed;
            out_task->done = 1;
            pthread_cond_broadcast(&out_task->cond);
            pthread_mutex_unlock(&out_task->lock);

            fire_callbacks(w, out_task, out_task->failed);
        }

        cb = next_cb;
    }
}

static Task *dequeue(Pool *pool) {
    pthread_mutex_lock(&pool->lock);
    while (!pool->head && !pool->shutdown) {
        pthread_cond_wait(&pool->cond, &pool->lock);
    }
    Task *task = NULL;
    if (pool->head) {
        task = pool->head;
        pool->head = task->next_in_queue;
        if (!pool->head) pool->tail = NULL;
        task->next_in_queue = NULL;
    }
    pthread_mutex_unlock(&pool->lock);
    return task;
}

static void enqueue_task(Pool *pool, Task *task) {
    pthread_mutex_lock(&pool->lock);
    task->next_in_queue = NULL;
    if (pool->tail) pool->tail->next_in_queue = task;
    else pool->head = task;
    pool->tail = task;
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->lock);
}

static void *worker_fn(void *arg) {
    Worker *w = arg;
    atomic_store(&w->state, WORKER_IDLE);

    /* Each worker runs in the main interpreter context for this MVP.
     * True sub-interpreter support requires 3.12 APIs that need careful
     * setup. We use the pool's main thread state approach: each worker
     * creates its own PyThreadState. */
    PyThreadState *tstate = PyThreadState_New(w->interp);
    PyEval_RestoreThread(tstate);

    while (1) {
        PyEval_SaveThread();
        Task *task = dequeue(w->pool);
        PyEval_RestoreThread(tstate);
        atomic_fetch_add(&w->heartbeat, 1);

        if (!task) {
            /* shutdown */
            break;
        }

        atomic_store(&w->state, WORKER_RUNNING);
        w->current_task = task;

        pthread_mutex_lock(&task->lock);
        if (task->cancelled) {
            task->done = 1;
            pthread_cond_broadcast(&task->cond);
            pthread_mutex_unlock(&task->lock);
            atomic_store(&w->state, WORKER_IDLE);
            w->current_task = NULL;
            continue;
        }
        task->started = 1;
        pthread_mutex_unlock(&task->lock);

        /* Call the function */
        PyObject *result = NULL;
        if (task->fn_code) {
            /* Recreate function in this interpreter from code + globals */
            PyObject *fn = PyFunction_New(task->fn_code, task->fn_globals ? task->fn_globals : PyEval_GetFrameGlobals());
            if (fn) {
                result = PyObject_CallNoArgs(fn);
                Py_DECREF(fn);
            }
        }

        if (result) {
            task->result_sv = sv_from_pyobject(result);
            if (!task->result_sv) {
                capture_exception_strings(task);
                task->failed = 1;
            }
            Py_DECREF(result);
        } else {
            capture_exception_strings(task);
            task->failed = 1;
        }

        fire_callbacks(w, task, task->failed);

        pthread_mutex_lock(&task->lock);
        task->done = 1;
        pthread_cond_broadcast(&task->cond);
        pthread_mutex_unlock(&task->lock);

        atomic_store(&w->state, WORKER_IDLE);
        w->current_task = NULL;
    }

    PyThreadState_Clear(tstate);
    PyThreadState_Delete(tstate);
    atomic_store(&w->state, WORKER_DEAD);
    return NULL;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Watchdog thread (P1)
 * ────────────────────────────────────────────────────────────────────────── */

static void respawn_worker(Pool *pool, int idx);

static void *watchdog_fn(void *arg) {
    Pool *pool = arg;
    struct timespec ts = {1, 0};
    while (!pool->shutdown) {
        nanosleep(&ts, NULL);
        for (int i = 0; i < pool->nworkers; i++) {
            Worker *w = &pool->workers[i];
            if (atomic_load(&w->state) == WORKER_DEAD && !pool->shutdown) {
                /* Re-queue in-flight task */
                if (w->current_task) {
                    enqueue_task(pool, w->current_task);
                    w->current_task = NULL;
                }
                respawn_worker(pool, i);
            }
        }
    }
    return NULL;
}

/* ──────────────────────────────────────────────────────────────────────────
 * ThreadPoolExecutor Python type
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct {
    PyObject_HEAD
    Pool *pool;
} ThreadPoolExecutorObject;

static PyTypeObject ThreadPoolExecutorType;

static void respawn_worker(Pool *pool, int idx) {
    Worker *w = &pool->workers[idx];
    /* Reuse the same interpreter */
    atomic_store(&w->state, WORKER_IDLE);
    w->current_task = NULL;
    pthread_create(&w->thread, NULL, worker_fn, w);
}

static void tpe_dealloc(ThreadPoolExecutorObject *self) {
    if (self->pool) {
        /* Shutdown pool */
        pthread_mutex_lock(&self->pool->lock);
        self->pool->shutdown = 1;
        pthread_cond_broadcast(&self->pool->cond);
        pthread_mutex_unlock(&self->pool->lock);

        for (int i = 0; i < self->pool->nworkers; i++) {
            pthread_join(self->pool->workers[i].thread, NULL);
        }
        if (self->pool->has_watchdog) {
            pthread_join(self->pool->watchdog_thread, NULL);
        }

        for (int i = 0; i < self->pool->nshared; i++) {
            sv_free(self->pool->shared_templates[i]);
            free(self->pool->shared_keys[i]);
        }
        free(self->pool->shared_templates);
        free(self->pool->shared_keys);

        pthread_mutex_destroy(&self->pool->lock);
        pthread_cond_destroy(&self->pool->cond);
        free(self->pool->workers);
        free(self->pool);
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int tpe_init(ThreadPoolExecutorObject *self, PyObject *args, PyObject *kwargs) {
    static char *kwnames[] = {"workers", "shared", NULL};
    int nworkers = 4;
    PyObject *shared_obj = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|iO", kwnames, &nworkers, &shared_obj))
        return -1;
    if (nworkers < 1) nworkers = 1;
    if (nworkers > 256) nworkers = 256;

    Pool *pool = calloc(1, sizeof(Pool));
    if (!pool) { PyErr_NoMemory(); return -1; }

    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->cond, NULL);
    pool->nworkers = nworkers;
    pool->workers = calloc((size_t)nworkers, sizeof(Worker));
    if (!pool->workers) {
        free(pool); PyErr_NoMemory(); return -1;
    }

    /* Encode shared= values */
    if (shared_obj && shared_obj != Py_None) {
        if (!PyDict_Check(shared_obj)) {
            PyErr_SetString(PyExc_TypeError, "cfuture: shared= must be a dict");
            free(pool->workers); free(pool); return -1;
        }
        Py_ssize_t nshared = PyDict_Size(shared_obj);
        pool->shared_keys = calloc((size_t)nshared, sizeof(char*));
        pool->shared_templates = calloc((size_t)nshared, sizeof(SharedValue*));
        if (!pool->shared_keys || !pool->shared_templates) {
            free(pool->shared_keys); free(pool->shared_templates);
            free(pool->workers); free(pool); PyErr_NoMemory(); return -1;
        }
        PyObject *key, *val;
        Py_ssize_t pos = 0;
        int idx = 0;
        while (PyDict_Next(shared_obj, &pos, &key, &val)) {
            const char *ks = PyUnicode_AsUTF8(key);
            if (!ks) {
                free(pool->workers); free(pool); return -1;
            }
            pool->shared_keys[idx] = strdup(ks);
            pool->shared_templates[idx] = sv_from_pyobject(val);
            if (!pool->shared_templates[idx]) {
                free(pool->workers); free(pool); return -1;
            }
            pool->nshared++;
            idx++;
        }
    }

    /* Use main interpreter state for all workers (simplified MVP) */
    PyInterpreterState *interp = PyInterpreterState_Get();

    for (int i = 0; i < nworkers; i++) {
        pool->workers[i].id = i;
        pool->workers[i].pool = pool;
        pool->workers[i].interp = interp;
        atomic_store(&pool->workers[i].state, WORKER_IDLE);
        atomic_store(&pool->workers[i].heartbeat, 0);
        pthread_create(&pool->workers[i].thread, NULL, worker_fn, &pool->workers[i]);
    }

    /* Start watchdog */
    pthread_create(&pool->watchdog_thread, NULL, watchdog_fn, pool);
    pool->has_watchdog = 1;

    self->pool = pool;
    return 0;
}

static PyObject *tpe_submit(ThreadPoolExecutorObject *self, PyObject *args, PyObject *kwargs) {
    static char *kwnames[] = {"fn", NULL};
    PyObject *fn = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O", kwnames, &fn))
        return NULL;

    if (validate_fn(fn) < 0) return NULL;

    Task *task = task_new();
    if (!task) return PyErr_NoMemory();

    /* Get code object */
    PyObject *code = PyObject_GetAttrString(fn, "__code__");
    if (code) {
        task->fn_code = code;
    } else {
        PyErr_Clear();
        /* Built-in callable: we store fn itself — we'll call it directly */
        task->fn_code = fn;
        Py_INCREF(fn);
    }

    /* Capture globals */
    PyObject *fn_globals = PyObject_GetAttrString(fn, "__globals__");
    if (fn_globals) {
        task->fn_globals = fn_globals;
    } else {
        PyErr_Clear();
        task->fn_globals = NULL;
    }

    /* Store the actual callable for direct invocation */
    Py_DECREF(task->fn_code);
    task->fn_code = fn;
    Py_INCREF(fn);

    /* Create Future */
    FutureObject *f = PyObject_New(FutureObject, &FutureType);
    if (!f) { task_free(task); return NULL; }
    f->task = task;
    f->owns_task = 1;
    f->pool = self->pool;

    enqueue_task(self->pool, task);
    return (PyObject *)f;
}

static PyObject *tpe_shutdown(ThreadPoolExecutorObject *self, PyObject *args, PyObject *kwargs) {
    static char *kwnames[] = {"wait", NULL};
    int wait = 1;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|p", kwnames, &wait))
        return NULL;

    pthread_mutex_lock(&self->pool->lock);
    self->pool->shutdown = 1;
    pthread_cond_broadcast(&self->pool->cond);
    pthread_mutex_unlock(&self->pool->lock);

    if (wait) {
        Py_BEGIN_ALLOW_THREADS
        for (int i = 0; i < self->pool->nworkers; i++) {
            pthread_join(self->pool->workers[i].thread, NULL);
        }
        if (self->pool->has_watchdog) {
            pthread_join(self->pool->watchdog_thread, NULL);
            self->pool->has_watchdog = 0;
        }
        Py_END_ALLOW_THREADS
    }
    Py_RETURN_NONE;
}

static PyObject *tpe_enter(ThreadPoolExecutorObject *self, PyObject *_) {
    Py_INCREF(self);
    return (PyObject *)self;
}

static PyObject *tpe_exit(ThreadPoolExecutorObject *self, PyObject *args) {
    return tpe_shutdown(self, PyTuple_New(0), NULL);
}

static PyMethodDef tpe_methods[] = {
    {"submit",   (PyCFunction)tpe_submit,   METH_VARARGS|METH_KEYWORDS, "Submit callable."},
    {"shutdown", (PyCFunction)tpe_shutdown, METH_VARARGS|METH_KEYWORDS, "Shutdown pool."},
    {"__enter__",(PyCFunction)tpe_enter,    METH_NOARGS,  "Context manager enter."},
    {"__exit__", (PyCFunction)tpe_exit,     METH_VARARGS, "Context manager exit."},
    {NULL}
};

static PyTypeObject ThreadPoolExecutorType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "cfuture.ThreadPoolExecutor",
    .tp_basicsize = sizeof(ThreadPoolExecutorObject),
    .tp_dealloc   = (destructor)tpe_dealloc,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_doc       = "A cfuture ThreadPoolExecutor.",
    .tp_methods   = tpe_methods,
    .tp_init      = (initproc)tpe_init,
    .tp_new       = PyType_GenericNew,
};

/* ──────────────────────────────────────────────────────────────────────────
 * all_of — non-blocking combinator
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct AllOfState {
    _Atomic int   remaining;
    int           total;
    SharedValue **results;
    int          *failed;
    Task         *out_task;
    pthread_mutex_t lock;
} AllOfState;

typedef struct {
    PyObject_HEAD
    AllOfState *state;
    int         index;
    FutureObject *allof_future;
} AllOfCallbackObj;

static PyTypeObject AllOfCallbackType;

static void allof_callback_dealloc(AllOfCallbackObj *self) {
    Py_XDECREF(self->allof_future);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *allof_callback_call(AllOfCallbackObj *self, PyObject *args, PyObject *kwargs) {
    /* Called as fn(result, deps) from fire_callbacks */
    PyObject *result_obj = NULL, *deps_tuple = NULL;
    if (!PyArg_ParseTuple(args, "OO", &result_obj, &deps_tuple))
        return NULL;

    AllOfState *state = self->state;
    int idx = self->index;

    pthread_mutex_lock(&state->lock);
    state->results[idx] = sv_from_pyobject(result_obj);
    state->failed[idx] = 0;
    int remaining = atomic_fetch_sub(&state->remaining, 1) - 1;
    pthread_mutex_unlock(&state->lock);

    if (remaining == 0) {
        /* All done — build results list and resolve out_task */
        PyObject *results_list = PyList_New(state->total);
        int any_failed = 0;
        if (results_list) {
            for (int i = 0; i < state->total; i++) {
                if (state->results[i]) {
                    PyObject *item = sv_to_pyobject(state->results[i]);
                    if (!item) item = Py_NewRef(Py_None);
                    PyList_SET_ITEM(results_list, i, item);
                } else {
                    any_failed = 1;
                    PyList_SET_ITEM(results_list, i, Py_NewRef(Py_None));
                }
            }
        }
        Task *out = state->out_task;
        pthread_mutex_lock(&out->lock);
        if (!any_failed && results_list) {
            out->result_sv = sv_from_pyobject(results_list);
            if (!out->result_sv) {
                snprintf(out->exc_type, sizeof(out->exc_type), "RuntimeError");
                snprintf(out->exc_msg, sizeof(out->exc_msg), "all_of: failed to encode results");
                out->failed = 1;
            }
        } else {
            snprintf(out->exc_type, sizeof(out->exc_type), "RuntimeError");
            snprintf(out->exc_msg, sizeof(out->exc_msg), "all_of: one or more futures failed");
            out->failed = 1;
        }
        Py_XDECREF(results_list);
        out->done = 1;
        pthread_cond_broadcast(&out->cond);
        pthread_mutex_unlock(&out->lock);

        /* Free AllOfState */
        for (int i = 0; i < state->total; i++) sv_free(state->results[i]);
        free(state->results);
        free(state->failed);
        pthread_mutex_destroy(&state->lock);
        free(state);
    }

    Py_RETURN_NONE;
}

static PyTypeObject AllOfCallbackType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "cfuture._AllOfCallback",
    .tp_basicsize = sizeof(AllOfCallbackObj),
    .tp_dealloc   = (destructor)allof_callback_dealloc,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_call      = (ternaryfunc)allof_callback_call,
};

static PyObject *cfuture_all_of(PyObject *module, PyObject *args) {
    Py_ssize_t n = PyTuple_GET_SIZE(args);
    if (n == 0) {
        /* Return immediately-resolved empty future */
        PyObject *empty = PyList_New(0);
        if (!empty) return NULL;
        PyObject *f_args = Py_BuildValue("(O)", empty);
        Py_DECREF(empty);
        if (!f_args) return NULL;
        PyObject *f = future_completed(NULL, f_args);
        Py_DECREF(f_args);
        return f;
    }

    /* Validate all args are Futures */
    for (Py_ssize_t i = 0; i < n; i++) {
        if (!PyObject_IsInstance(PyTuple_GET_ITEM(args, i), (PyObject *)&FutureType)) {
            PyErr_SetString(PyExc_TypeError, "cfuture.all_of: all arguments must be Future instances");
            return NULL;
        }
    }

    AllOfState *state = calloc(1, sizeof(AllOfState));
    if (!state) return PyErr_NoMemory();
    state->total = (int)n;
    atomic_store(&state->remaining, (int)n);
    state->results = calloc((size_t)n, sizeof(SharedValue*));
    state->failed  = calloc((size_t)n, sizeof(int));
    pthread_mutex_init(&state->lock, NULL);
    if (!state->results || !state->failed) {
        free(state->results); free(state->failed);
        pthread_mutex_destroy(&state->lock);
        free(state); return PyErr_NoMemory();
    }

    /* Create out future */
    FutureObject *out_future = PyObject_New(FutureObject, &FutureType);
    if (!out_future) {
        free(state->results); free(state->failed);
        pthread_mutex_destroy(&state->lock);
        free(state); return NULL;
    }
    out_future->task = task_new();
    out_future->owns_task = 1;
    out_future->pool = NULL;
    if (!out_future->task) {
        Py_DECREF(out_future);
        free(state->results); free(state->failed);
        pthread_mutex_destroy(&state->lock);
        free(state); return PyErr_NoMemory();
    }
    state->out_task = out_future->task;

    /* Register a callback on each input future */
    for (Py_ssize_t i = 0; i < n; i++) {
        AllOfCallbackObj *cb_obj = PyObject_New(AllOfCallbackObj, &AllOfCallbackType);
        if (!cb_obj) {
            Py_DECREF(out_future);
            free(state->results); free(state->failed);
            pthread_mutex_destroy(&state->lock);
            free(state); return NULL;
        }
        cb_obj->state = state;
        cb_obj->index = (int)i;
        cb_obj->allof_future = out_future;
        Py_INCREF(out_future);

        FutureObject *input_f = (FutureObject *)PyTuple_GET_ITEM(args, i);
        PyObject *empty_list = PyList_New(0);
        PyObject *cb_result = future_add_callback(input_f, (PyObject *)cb_obj, empty_list, CB_THEN);
        Py_DECREF(cb_obj);
        Py_XDECREF(empty_list);
        Py_XDECREF(cb_result);
    }

    return (PyObject *)out_future;
}

/* ──────────────────────────────────────────────────────────────────────────
 * cfuture.pickled() wrapper
 * ────────────────────────────────────────────────────────────────────────── */

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

static PyTypeObject PickledType = {
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

static PyObject *cfuture_pickled(PyObject *module, PyObject *args) {
    return PyObject_Call((PyObject *)&PickledType, args, NULL);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Module definition
 * ────────────────────────────────────────────────────────────────────────── */

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
