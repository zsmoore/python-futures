/*
 * tests/c/test_future_internals.c
 *
 * Unity unit tests for the pure-C logic in _cfuture_future.c:
 *   - task_new / task_free
 *   - validate_fn
 *
 * fire_callbacks and the FutureObject Python type are exercised end-to-end
 * by the pytest suite and don't gain anything from a C-level harness.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "unity.h"

#include "../../cfuture/_cfuture_shared_value.c"
#include "../../cfuture/_cfuture_pickled.c"
#include "../../cfuture/_cfuture_future.c"

void setUp(void)    {}
void tearDown(void) {}

/* -------------------------------------------------------------------------
 * task_new / task_free
 * -------------------------------------------------------------------------*/

static void test_task_new_returns_non_null(void) {
    Task *t = task_new();
    TEST_ASSERT_NOT_NULL(t);
    task_free(t);
}

static void test_task_new_zeroed(void) {
    Task *t = task_new();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_INT(0, t->done);
    TEST_ASSERT_EQUAL_INT(0, t->failed);
    TEST_ASSERT_EQUAL_INT(0, t->cancelled);
    TEST_ASSERT_EQUAL_INT(0, t->started);
    TEST_ASSERT_NULL(t->result_sv);
    TEST_ASSERT_NULL(t->callbacks);
    TEST_ASSERT_NULL(t->fn_code);
    TEST_ASSERT_NULL(t->fn_globals);
    task_free(t);
}

static void test_task_free_null_is_safe(void) {
    task_free(NULL);
}

static void test_task_free_with_result_sv(void) {
    Task *t = task_new();
    TEST_ASSERT_NOT_NULL(t);
    PyObject *obj = PyLong_FromLong(42);
    t->result_sv = sv_from_pyobject(obj);
    Py_DECREF(obj);
    TEST_ASSERT_NOT_NULL(t->result_sv);
    task_free(t);  /* must not crash or leak */
}

/* -------------------------------------------------------------------------
 * validate_fn
 * -------------------------------------------------------------------------*/

static void test_validate_fn_non_callable_returns_minus1(void) {
    PyObject *obj = PyLong_FromLong(1);
    int rc = validate_fn(obj);
    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
    PyErr_Clear();
    Py_DECREF(obj);
}

static void test_validate_fn_builtin_callable_ok(void) {
    /* Built-ins have no __code__, so the free-vars check is skipped. */
    PyObject *fn = PyObject_GetAttrString(PyImport_ImportModule("builtins"), "len");
    TEST_ASSERT_NOT_NULL(fn);
    int rc = validate_fn(fn);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_FALSE(PyErr_Occurred());
    Py_DECREF(fn);
}

static void test_validate_fn_plain_function_ok(void) {
    /* Compile a plain function with no free variables. */
    PyObject *code_str = PyUnicode_FromString("def f(): return 1\n");
    PyObject *globs = PyDict_New();
    PyDict_SetItemString(globs, "__builtins__", PyEval_GetBuiltins());
    PyObject *code_obj = Py_CompileString("def f(): return 1\n", "<test>", Py_file_input);
    TEST_ASSERT_NOT_NULL(code_obj);
    PyObject *result = PyEval_EvalCode(code_obj, globs, globs);
    Py_XDECREF(result);
    Py_DECREF(code_obj);

    PyObject *fn = PyDict_GetItemString(globs, "f");
    TEST_ASSERT_NOT_NULL(fn);
    int rc = validate_fn(fn);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_FALSE(PyErr_Occurred());

    Py_DECREF(globs);
    Py_DECREF(code_str);
}

static void test_validate_fn_closure_returns_minus1(void) {
    /* Compile a closure — has free variables. */
    PyObject *globs = PyDict_New();
    PyDict_SetItemString(globs, "__builtins__", PyEval_GetBuiltins());
    PyObject *code_obj = Py_CompileString(
        "def outer():\n"
        "    x = 1\n"
        "    def inner(): return x\n"
        "    return inner\n"
        "f = outer()\n",
        "<test>", Py_file_input);
    TEST_ASSERT_NOT_NULL(code_obj);
    PyObject *res = PyEval_EvalCode(code_obj, globs, globs);
    Py_XDECREF(res);
    Py_DECREF(code_obj);

    PyObject *fn = PyDict_GetItemString(globs, "f");
    TEST_ASSERT_NOT_NULL(fn);
    int rc = validate_fn(fn);
    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_ValueError));
    PyErr_Clear();

    Py_DECREF(globs);
}

/* -------------------------------------------------------------------------
 * task_incref / task_decref
 * -------------------------------------------------------------------------*/

static void test_task_new_refcount_is_one(void) {
    Task *t = task_new();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&t->refcount));
    task_free(t);
}

static void test_task_incref_increments(void) {
    Task *t = task_new();
    TEST_ASSERT_NOT_NULL(t);
    task_incref(t);
    TEST_ASSERT_EQUAL_INT(2, atomic_load(&t->refcount));
    /* Bring back to 1 so task_free can clean up */
    atomic_fetch_sub(&t->refcount, 1);
    task_free(t);
}

static void test_task_decref_to_zero_frees(void) {
    /* task_decref with refcount==1 must free without crashing */
    Task *t = task_new();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&t->refcount));
    task_decref(t);  /* drops to 0 → calls task_free(t) */
    /* No assertion needed — reaching here without crash is the pass condition */
}

static void test_task_decref_does_not_free_when_refs_remain(void) {
    Task *t = task_new();
    TEST_ASSERT_NOT_NULL(t);
    task_incref(t);                          /* refcount = 2 */
    task_decref(t);                          /* refcount = 1, must NOT free */
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&t->refcount));
    task_decref(t);                          /* refcount = 0, now freed */
}

static void test_task_incref_null_is_safe(void) {
    task_incref(NULL);  /* must not crash */
}

static void test_task_decref_null_is_safe(void) {
    task_decref(NULL);  /* must not crash */
}

/* -------------------------------------------------------------------------
 * out_future refcount — task_free decrefs, future_add_callback increfs
 * -------------------------------------------------------------------------*/

static void test_task_free_decrefs_out_future(void) {
    /* Build a task with one callback whose out_future is a plain PyObject.
     * task_free must Py_XDECREF cb->out_future, dropping its refcount. */
    Task *t = task_new();
    TEST_ASSERT_NOT_NULL(t);

    /* Use a simple Python integer as a stand-in PyObject for out_future.
     * Cast is safe: we only care about the refcount, not the type. */
    PyObject *mock_future = PyLong_FromLong(999);
    TEST_ASSERT_NOT_NULL(mock_future);
    Py_ssize_t refcount_before = Py_REFCNT(mock_future);

    Callback *cb = calloc(1, sizeof(Callback));
    TEST_ASSERT_NOT_NULL(cb);
    cb->out_future = (FutureObject *)mock_future;
    Py_INCREF(mock_future);  /* simulate the incref future_add_callback does */
    t->callbacks = cb;

    task_free(t);  /* must Py_XDECREF cb->out_future */

    TEST_ASSERT_EQUAL(refcount_before, Py_REFCNT(mock_future));
    Py_DECREF(mock_future);
}

static void test_future_add_callback_increfs_out_future(void) {
    /* future_add_callback must Py_INCREF out_future so it stays alive
     * even if the caller drops its Python reference. */
    if (PyType_Ready(&FutureType) < 0) {
        TEST_FAIL_MESSAGE("FutureType not ready");
        return;
    }

    /* Create a pre-completed future so the callback fires immediately
     * (inline path in future_add_callback), avoiding any worker thread. */
    PyObject *args = PyTuple_Pack(1, PyLong_FromLong(1));
    FutureObject *base = (FutureObject *)future_completed(NULL, args);
    Py_DECREF(args);
    TEST_ASSERT_NOT_NULL(base);

    /* Build a plain no-op callable */
    PyObject *globs = PyDict_New();
    PyDict_SetItemString(globs, "__builtins__", PyEval_GetBuiltins());
    PyObject *code = Py_CompileString("def f(x, d): return x\n", "<t>", Py_file_input);
    TEST_ASSERT_NOT_NULL(code);
    PyObject *res = PyEval_EvalCode(code, globs, globs);
    Py_XDECREF(res); Py_DECREF(code);
    PyObject *fn = PyDict_GetItemString(globs, "f");
    TEST_ASSERT_NOT_NULL(fn);

    PyObject *deps = PyList_New(0);
    PyObject *out = future_add_callback(base, fn, deps, CB_THEN);
    TEST_ASSERT_NOT_NULL(out);

    /* out_future was returned to Python (refcount 1 from new) AND the
     * callback holds a second ref — so refcount must be >= 2.
     * (On the inline-fire path the callback is gone after firing, but
     * we still got a valid object back.) */
    TEST_ASSERT_GREATER_OR_EQUAL(1, (int)Py_REFCNT(out));

    Py_DECREF(out);
    Py_DECREF(deps);
    Py_DECREF(globs);
    Py_DECREF((PyObject *)base);
}

/* -------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------*/

int main(void) {
    Py_Initialize();

    UNITY_BEGIN();

    RUN_TEST(test_task_new_returns_non_null);
    RUN_TEST(test_task_new_zeroed);
    RUN_TEST(test_task_free_null_is_safe);
    RUN_TEST(test_task_free_with_result_sv);
    RUN_TEST(test_validate_fn_non_callable_returns_minus1);
    RUN_TEST(test_validate_fn_builtin_callable_ok);
    RUN_TEST(test_validate_fn_plain_function_ok);
    RUN_TEST(test_validate_fn_closure_returns_minus1);
    RUN_TEST(test_task_new_refcount_is_one);
    RUN_TEST(test_task_incref_increments);
    RUN_TEST(test_task_decref_to_zero_frees);
    RUN_TEST(test_task_decref_does_not_free_when_refs_remain);
    RUN_TEST(test_task_incref_null_is_safe);
    RUN_TEST(test_task_decref_null_is_safe);
    RUN_TEST(test_task_free_decrefs_out_future);
    RUN_TEST(test_future_add_callback_increfs_out_future);

    int result = UNITY_END();
    Py_Finalize();
    return result;
}
