/*
 * tests/c/test_build_shared_dict.c
 *
 * Unity unit tests for build_shared_dict() in _cfuture_worker.c.
 * Covers: NULL pool, empty pool, single value, multiple values.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "unity.h"

/* Include all TUs in dependency order so symbols resolve without the linker
   needing a full build.  PyInit__cfuture is guarded to suppress redefinition. */
#include "../../cfuture/_cfuture_shared_value.c"
#include "../../cfuture/_cfuture_pickled.c"

/* Stub: future.c needs build_shared_dict but we define it in worker.c below.
   Use a forward declaration so future.c compiles without the stub. */
PyObject *build_shared_dict(Pool *pool);

#define PyInit__cfuture PyInit__cfuture_HIDDEN_FUTURE
#include "../../cfuture/_cfuture_future.c"
#undef PyInit__cfuture

#define PyInit__cfuture PyInit__cfuture_HIDDEN_WORKER
#include "../../cfuture/_cfuture_worker.c"
#undef PyInit__cfuture

void setUp(void)    {}
void tearDown(void) {}

/* -------------------------------------------------------------------------
 * Helper: build a Pool with n shared key/value pairs (string keys, int values)
 * -------------------------------------------------------------------------*/
static Pool *make_pool_with_shared(const char **keys, long *vals, int n) {
    Pool *p = calloc(1, sizeof(Pool));
    if (!p) return NULL;
    p->nshared = n;
    p->shared_keys      = calloc(n, sizeof(char *));
    p->shared_templates = calloc(n, sizeof(SharedValue *));
    for (int i = 0; i < n; i++) {
        p->shared_keys[i] = strdup(keys[i]);
        PyObject *obj = PyLong_FromLong(vals[i]);
        p->shared_templates[i] = sv_from_pyobject(obj);
        Py_DECREF(obj);
    }
    return p;
}

static void free_test_pool(Pool *p) {
    for (int i = 0; i < p->nshared; i++) {
        free(p->shared_keys[i]);
        sv_free(p->shared_templates[i]);
    }
    free(p->shared_keys);
    free(p->shared_templates);
    free(p);
}

/* -------------------------------------------------------------------------
 * Tests
 * -------------------------------------------------------------------------*/

static void test_null_pool_returns_empty_dict(void) {
    PyObject *d = build_shared_dict(NULL);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_TRUE(PyDict_Check(d));
    TEST_ASSERT_EQUAL_INT(0, (int)PyDict_Size(d));
    Py_DECREF(d);
}

static void test_empty_pool_returns_empty_dict(void) {
    Pool p = {0};  /* nshared == 0 */
    PyObject *d = build_shared_dict(&p);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_TRUE(PyDict_Check(d));
    TEST_ASSERT_EQUAL_INT(0, (int)PyDict_Size(d));
    Py_DECREF(d);
}

static void test_single_value_present(void) {
    const char *keys[] = {"answer"};
    long vals[]        = {42};
    Pool *p = make_pool_with_shared(keys, vals, 1);
    TEST_ASSERT_NOT_NULL(p);

    PyObject *d = build_shared_dict(p);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_INT(1, (int)PyDict_Size(d));

    PyObject *val = PyDict_GetItemString(d, "answer");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT(42, (int)PyLong_AsLong(val));

    Py_DECREF(d);
    free_test_pool(p);
}

static void test_multiple_values_present(void) {
    const char *keys[] = {"a", "b", "c"};
    long vals[]        = {1, 2, 3};
    Pool *p = make_pool_with_shared(keys, vals, 3);
    TEST_ASSERT_NOT_NULL(p);

    PyObject *d = build_shared_dict(p);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_INT(3, (int)PyDict_Size(d));

    TEST_ASSERT_EQUAL_INT(1, (int)PyLong_AsLong(PyDict_GetItemString(d, "a")));
    TEST_ASSERT_EQUAL_INT(2, (int)PyLong_AsLong(PyDict_GetItemString(d, "b")));
    TEST_ASSERT_EQUAL_INT(3, (int)PyLong_AsLong(PyDict_GetItemString(d, "c")));

    Py_DECREF(d);
    free_test_pool(p);
}

static void test_returned_dicts_are_independent_objects(void) {
    /* Two calls to build_shared_dict must return different dict objects. */
    const char *keys[] = {"x"};
    long vals[]        = {99};
    Pool *p = make_pool_with_shared(keys, vals, 1);
    TEST_ASSERT_NOT_NULL(p);

    PyObject *d1 = build_shared_dict(p);
    PyObject *d2 = build_shared_dict(p);
    TEST_ASSERT_NOT_NULL(d1);
    TEST_ASSERT_NOT_NULL(d2);
    /* Each call must return a distinct dict object */
    TEST_ASSERT_NOT_EQUAL(d1, d2);

    /* Both should have the correct value */
    TEST_ASSERT_EQUAL_INT(99, (int)PyLong_AsLong(PyDict_GetItemString(d1, "x")));
    TEST_ASSERT_EQUAL_INT(99, (int)PyLong_AsLong(PyDict_GetItemString(d2, "x")));

    Py_DECREF(d1);
    Py_DECREF(d2);
    free_test_pool(p);
}

/* -------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------*/

int main(void) {
    Py_Initialize();

    UNITY_BEGIN();
    RUN_TEST(test_null_pool_returns_empty_dict);
    RUN_TEST(test_empty_pool_returns_empty_dict);
    RUN_TEST(test_single_value_present);
    RUN_TEST(test_multiple_values_present);
    RUN_TEST(test_returned_dicts_are_independent_objects);
    int result = UNITY_END();

    Py_Finalize();
    return result;
}
