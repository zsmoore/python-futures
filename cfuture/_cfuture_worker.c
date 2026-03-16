/*
 * cfuture/_cfuture_worker.c
 * Worker loop, Watchdog, ThreadPoolExecutor, fire_callbacks, all_of.
 */

#include "_cfuture_internal.h"

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

void fire_callbacks(Worker *w, Task *task, int is_error) {
    pthread_mutex_lock(&task->lock);
    Callback *cb = task->callbacks;
    pthread_mutex_unlock(&task->lock);

    while (cb) {
        Callback *next_cb = cb->next;

        int should_fire = (cb->type == CB_FINALLY) ||
                          (cb->type == CB_THEN   && !is_error) ||
                          (cb->type == CB_EXCEPT &&  is_error);

        Task *out_task = cb->out_future->task;

        task_incref(out_task);  /* keep out_task alive across fire_callbacks */

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

        task_decref(out_task);  /* release fire_callbacks reference */

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
    task_incref(task);  /* worker queue holds a reference */
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
            task_decref(task);  /* release worker queue reference */
            continue;
        }
        task->started = 1;
        pthread_mutex_unlock(&task->lock);

        /* Call the function directly — fn_code holds the callable itself */
        PyObject *result = NULL;
        if (task->fn_code) {
            result = PyObject_CallNoArgs(task->fn_code);
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
        task_decref(task);  /* release worker queue reference */
    }

    PyThreadState_Clear(tstate);
    PyEval_SaveThread();  /* release GIL and unset current tstate before deleting */
    PyThreadState_Delete(tstate);
    atomic_store(&w->state, WORKER_DEAD);
    return NULL;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Watchdog thread
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

    /* Store the callable for direct invocation in the worker */
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

PyTypeObject ThreadPoolExecutorType = {
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

PyTypeObject AllOfCallbackType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name      = "cfuture._AllOfCallback",
    .tp_basicsize = sizeof(AllOfCallbackObj),
    .tp_dealloc   = (destructor)allof_callback_dealloc,
    .tp_flags     = Py_TPFLAGS_DEFAULT,
    .tp_call      = (ternaryfunc)allof_callback_call,
};

PyObject *cfuture_all_of(PyObject *module, PyObject *args) {
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
