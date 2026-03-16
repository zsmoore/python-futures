/*
 * cfuture/_cfuture_internal.h
 * Shared types, constants, and cross-TU forward declarations.
 * Included by every _cfuture_*.c translation unit.
 */

#ifndef CFUTURE_INTERNAL_H
#define CFUTURE_INTERNAL_H

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>

/* ──────────────────────────────────────────────────────────────────────────
 * SharedValue types
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

/* SharedValue must be fully defined before SharedDict uses it inline */
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

struct SharedList {
    size_t       len;
    SharedValue *items;
};

struct SharedDict {
    size_t len;
    struct { SharedValue key; SharedValue val; } *entries;
};

/* ──────────────────────────────────────────────────────────────────────────
 * Constants
 * ────────────────────────────────────────────────────────────────────────── */

#define CB_THEN    0
#define CB_EXCEPT  1
#define CB_FINALLY 2

#define WORKER_IDLE    0
#define WORKER_RUNNING 1
#define WORKER_DEAD    2

/* ──────────────────────────────────────────────────────────────────────────
 * Forward declarations for struct types
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct FutureObject FutureObject;

typedef struct Callback {
    PyObject        *code;           /* PyCodeObject* */
    SharedValue    **dep_templates;  /* array of SharedValue* */
    int              ndeps;
    int              type;           /* CB_THEN / CB_EXCEPT / CB_FINALLY */
    FutureObject    *out_future;
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

struct FutureObject {
    PyObject_HEAD
    Task  *task;
    Pool  *pool;
    int    owns_task;  /* 1 if this Future allocated the Task */
};

/* ──────────────────────────────────────────────────────────────────────────
 * Cross-TU function declarations
 * ────────────────────────────────────────────────────────────────────────── */

/* _cfuture_shared_value.c */
void         sv_free_inline(SharedValue *sv);
void         sv_free(SharedValue *sv);
int          sv_fill_from_pyobject(SharedValue *sv, PyObject *obj);
SharedValue *sv_from_pyobject(PyObject *obj);
void         sv_deep_copy_inline(SharedValue *dst, const SharedValue *src);
SharedValue *sv_deep_copy(const SharedValue *src);
PyObject    *sv_to_pyobject(const SharedValue *sv);

/* _cfuture_pickled.c */
extern PyTypeObject PickledType;

/* _cfuture_future.c */
extern PyTypeObject FutureType;
Task     *task_new(void);
void      task_free(Task *t);
int       validate_fn(PyObject *fn);
PyObject *future_add_callback(FutureObject *self, PyObject *fn, PyObject *deps_list, int cb_type);
PyObject *future_completed(PyObject *cls, PyObject *args);

/* _cfuture_worker.c */
extern PyTypeObject ThreadPoolExecutorType;
extern PyTypeObject AllOfCallbackType;
void fire_callbacks(Worker *w, Task *task, int is_error);

#endif /* CFUTURE_INTERNAL_H */
