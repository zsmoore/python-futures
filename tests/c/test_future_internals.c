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

    int result = UNITY_END();
    Py_Finalize();
    return result;
}
