/*
 * tests/c/test_shared_value.c
 *
 * Unity unit tests for the SharedValue encode/decode/copy/free layer.
 *
 * Strategy: #include the implementation source directly so we can reach all
 * static functions without changing the production code. We suppress the
 * PyMODINIT_FUNC symbol to avoid a duplicate-symbol link error.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "unity.h"

#define PyInit__cfuture PyInit__cfuture_HIDDEN
#include "../../cfuture/_cfuture.c"
#undef PyInit__cfuture

/* -------------------------------------------------------------------------
 * Unity required setUp / tearDown (called before/after every test)
 * -------------------------------------------------------------------------*/

void setUp(void)    {}
void tearDown(void) {}

/* -------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------*/

static PyObject *make_list(long a, long b) {
    PyObject *lst = PyList_New(2);
    PyList_SET_ITEM(lst, 0, PyLong_FromLong(a));
    PyList_SET_ITEM(lst, 1, PyLong_FromLong(b));
    return lst;
}

/* -------------------------------------------------------------------------
 * sv_fill_from_pyobject / sv_to_pyobject round-trips
 * -------------------------------------------------------------------------*/

static void test_none_roundtrip(void) {
    SharedValue sv;
    TEST_ASSERT_EQUAL_INT(0, sv_fill_from_pyobject(&sv, Py_None));
    TEST_ASSERT_EQUAL_INT(SV_NONE, sv.tag);
    PyObject *out = sv_to_pyobject(&sv);
    TEST_ASSERT_EQUAL_PTR(Py_None, out);
    Py_DECREF(out);
    sv_free_inline(&sv);
}

static void test_bool_true_roundtrip(void) {
    SharedValue sv;
    TEST_ASSERT_EQUAL_INT(0, sv_fill_from_pyobject(&sv, Py_True));
    TEST_ASSERT_EQUAL_INT(SV_BOOL, sv.tag);
    TEST_ASSERT_EQUAL_INT(1, sv.i);
    PyObject *out = sv_to_pyobject(&sv);
    TEST_ASSERT_EQUAL_PTR(Py_True, out);
    Py_DECREF(out);
    sv_free_inline(&sv);
}

static void test_bool_false_roundtrip(void) {
    SharedValue sv;
    TEST_ASSERT_EQUAL_INT(0, sv_fill_from_pyobject(&sv, Py_False));
    TEST_ASSERT_EQUAL_INT(SV_BOOL, sv.tag);
    TEST_ASSERT_EQUAL_INT(0, sv.i);
    PyObject *out = sv_to_pyobject(&sv);
    TEST_ASSERT_EQUAL_PTR(Py_False, out);
    Py_DECREF(out);
    sv_free_inline(&sv);
}

static void test_int_roundtrip(void) {
    PyObject *obj = PyLong_FromLong(42);
    SharedValue sv;
    TEST_ASSERT_EQUAL_INT(0, sv_fill_from_pyobject(&sv, obj));
    TEST_ASSERT_EQUAL_INT(SV_INT, sv.tag);
    TEST_ASSERT_EQUAL_INT(42, sv.i);
    PyObject *out = sv_to_pyobject(&sv);
    TEST_ASSERT_EQUAL_INT(42, PyLong_AsLong(out));
    Py_DECREF(out);
    Py_DECREF(obj);
    sv_free_inline(&sv);
}

static void test_negative_int_roundtrip(void) {
    PyObject *obj = PyLong_FromLong(-999);
    SharedValue sv;
    TEST_ASSERT_EQUAL_INT(0, sv_fill_from_pyobject(&sv, obj));
    TEST_ASSERT_EQUAL_INT(-999, sv.i);
    PyObject *out = sv_to_pyobject(&sv);
    TEST_ASSERT_EQUAL_INT(-999, PyLong_AsLong(out));
    Py_DECREF(out);
    Py_DECREF(obj);
    sv_free_inline(&sv);
}

static void test_float_roundtrip(void) {
    PyObject *obj = PyFloat_FromDouble(3.14);
    SharedValue sv;
    TEST_ASSERT_EQUAL_INT(0, sv_fill_from_pyobject(&sv, obj));
    TEST_ASSERT_EQUAL_INT(SV_FLOAT, sv.tag);
    /* Use bitwise equality — same double literal, no precision loss in encode */
    TEST_ASSERT_TRUE(sv.f == 3.14);
    PyObject *out = sv_to_pyobject(&sv);
    TEST_ASSERT_TRUE(PyFloat_AsDouble(out) == 3.14);
    Py_DECREF(out);
    Py_DECREF(obj);
    sv_free_inline(&sv);
}

static void test_str_roundtrip(void) {
    PyObject *obj = PyUnicode_FromString("hello");
    SharedValue sv;
    TEST_ASSERT_EQUAL_INT(0, sv_fill_from_pyobject(&sv, obj));
    TEST_ASSERT_EQUAL_INT(SV_STR, sv.tag);
    TEST_ASSERT_EQUAL_STRING("hello", sv.s.buf);
    TEST_ASSERT_EQUAL_UINT(5, sv.s.len);
    PyObject *out = sv_to_pyobject(&sv);
    TEST_ASSERT_EQUAL_STRING("hello", PyUnicode_AsUTF8(out));
    Py_DECREF(out);
    Py_DECREF(obj);
    sv_free_inline(&sv);
}

static void test_str_empty_roundtrip(void) {
    PyObject *obj = PyUnicode_FromString("");
    SharedValue sv;
    TEST_ASSERT_EQUAL_INT(0, sv_fill_from_pyobject(&sv, obj));
    TEST_ASSERT_EQUAL_UINT(0, sv.s.len);
    PyObject *out = sv_to_pyobject(&sv);
    TEST_ASSERT_EQUAL_STRING("", PyUnicode_AsUTF8(out));
    Py_DECREF(out);
    Py_DECREF(obj);
    sv_free_inline(&sv);
}

static void test_bytes_roundtrip(void) {
    PyObject *obj = PyBytes_FromStringAndSize("\x01\x02\x03", 3);
    SharedValue sv;
    TEST_ASSERT_EQUAL_INT(0, sv_fill_from_pyobject(&sv, obj));
    TEST_ASSERT_EQUAL_INT(SV_BYTES, sv.tag);
    TEST_ASSERT_EQUAL_UINT(3, sv.s.len);
    TEST_ASSERT_EQUAL_UINT8(0x01, (unsigned char)sv.s.buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02, (unsigned char)sv.s.buf[1]);
    PyObject *out = sv_to_pyobject(&sv);
    char *buf; Py_ssize_t len;
    PyBytes_AsStringAndSize(out, &buf, &len);
    TEST_ASSERT_EQUAL_INT(3, len);
    TEST_ASSERT_EQUAL_UINT8(0x03, (unsigned char)buf[2]);
    Py_DECREF(out);
    Py_DECREF(obj);
    sv_free_inline(&sv);
}

static void test_list_roundtrip(void) {
    PyObject *lst = make_list(1, 2);
    SharedValue sv;
    TEST_ASSERT_EQUAL_INT(0, sv_fill_from_pyobject(&sv, lst));
    TEST_ASSERT_EQUAL_INT(SV_LIST, sv.tag);
    TEST_ASSERT_EQUAL_UINT(2, sv.list->len);
    TEST_ASSERT_EQUAL_INT(1, sv.list->items[0].i);
    TEST_ASSERT_EQUAL_INT(2, sv.list->items[1].i);
    PyObject *out = sv_to_pyobject(&sv);
    TEST_ASSERT_TRUE(PyList_Check(out));
    TEST_ASSERT_EQUAL_INT(2, PyList_GET_SIZE(out));
    TEST_ASSERT_EQUAL_INT(1, PyLong_AsLong(PyList_GET_ITEM(out, 0)));
    Py_DECREF(out);
    Py_DECREF(lst);
    sv_free_inline(&sv);
}

static void test_empty_list_roundtrip(void) {
    PyObject *lst = PyList_New(0);
    SharedValue sv;
    TEST_ASSERT_EQUAL_INT(0, sv_fill_from_pyobject(&sv, lst));
    TEST_ASSERT_EQUAL_UINT(0, sv.list->len);
    PyObject *out = sv_to_pyobject(&sv);
    TEST_ASSERT_TRUE(PyList_Check(out));
    TEST_ASSERT_EQUAL_INT(0, PyList_GET_SIZE(out));
    Py_DECREF(out);
    Py_DECREF(lst);
    sv_free_inline(&sv);
}

static void test_tuple_roundtrip(void) {
    PyObject *tup = PyTuple_New(2);
    PyTuple_SET_ITEM(tup, 0, PyUnicode_FromString("a"));
    PyTuple_SET_ITEM(tup, 1, PyLong_FromLong(7));
    SharedValue sv;
    TEST_ASSERT_EQUAL_INT(0, sv_fill_from_pyobject(&sv, tup));
    TEST_ASSERT_EQUAL_INT(SV_TUPLE, sv.tag);
    PyObject *out = sv_to_pyobject(&sv);
    TEST_ASSERT_TRUE(PyTuple_Check(out));
    TEST_ASSERT_EQUAL_INT(2, PyTuple_GET_SIZE(out));
    TEST_ASSERT_EQUAL_STRING("a", PyUnicode_AsUTF8(PyTuple_GET_ITEM(out, 0)));
    Py_DECREF(out);
    Py_DECREF(tup);
    sv_free_inline(&sv);
}

static void test_dict_roundtrip(void) {
    PyObject *dct = PyDict_New();
    PyDict_SetItemString(dct, "key", PyLong_FromLong(99));
    SharedValue sv;
    TEST_ASSERT_EQUAL_INT(0, sv_fill_from_pyobject(&sv, dct));
    TEST_ASSERT_EQUAL_INT(SV_DICT, sv.tag);
    TEST_ASSERT_EQUAL_UINT(1, sv.dict->len);
    PyObject *out = sv_to_pyobject(&sv);
    TEST_ASSERT_TRUE(PyDict_Check(out));
    PyObject *val = PyDict_GetItemString(out, "key");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_EQUAL_INT(99, PyLong_AsLong(val));
    Py_DECREF(out);
    Py_DECREF(dct);
    sv_free_inline(&sv);
}

static void test_nested_list_in_dict(void) {
    PyObject *inner = PyList_New(1);
    PyList_SET_ITEM(inner, 0, PyLong_FromLong(5));
    PyObject *dct = PyDict_New();
    PyDict_SetItemString(dct, "nums", inner);
    Py_DECREF(inner);
    SharedValue sv;
    TEST_ASSERT_EQUAL_INT(0, sv_fill_from_pyobject(&sv, dct));
    PyObject *out = sv_to_pyobject(&sv);
    PyObject *nums = PyDict_GetItemString(out, "nums");
    TEST_ASSERT_NOT_NULL(nums);
    TEST_ASSERT_TRUE(PyList_Check(nums));
    TEST_ASSERT_EQUAL_INT(5, PyLong_AsLong(PyList_GET_ITEM(nums, 0)));
    Py_DECREF(out);
    Py_DECREF(dct);
    sv_free_inline(&sv);
}

/* -------------------------------------------------------------------------
 * sv_from_pyobject / sv_free (heap-allocated path)
 * -------------------------------------------------------------------------*/

static void test_sv_from_pyobject_allocates(void) {
    PyObject *obj = PyLong_FromLong(10);
    SharedValue *sv = sv_from_pyobject(obj);
    TEST_ASSERT_NOT_NULL(sv);
    TEST_ASSERT_EQUAL_INT(SV_INT, sv->tag);
    TEST_ASSERT_EQUAL_INT(10, sv->i);
    sv_free(sv);
    Py_DECREF(obj);
}

static void test_sv_free_null_is_safe(void) {
    sv_free(NULL);
}

static void test_sv_free_inline_null_is_safe(void) {
    sv_free_inline(NULL);
}

/* -------------------------------------------------------------------------
 * sv_deep_copy
 * -------------------------------------------------------------------------*/

static void test_deep_copy_int(void) {
    PyObject *obj = PyLong_FromLong(77);
    SharedValue *orig = sv_from_pyobject(obj);
    SharedValue *copy = sv_deep_copy(orig);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQUAL_INT(SV_INT, copy->tag);
    TEST_ASSERT_EQUAL_INT(77, copy->i);
    orig->i = 0;
    TEST_ASSERT_EQUAL_INT(77, copy->i);  /* copy is independent */
    sv_free(orig);
    sv_free(copy);
    Py_DECREF(obj);
}

static void test_deep_copy_str_independent(void) {
    PyObject *obj = PyUnicode_FromString("world");
    SharedValue *orig = sv_from_pyobject(obj);
    SharedValue *copy = sv_deep_copy(orig);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_NOT_EQUAL(orig->s.buf, copy->s.buf);  /* different heap alloc */
    TEST_ASSERT_EQUAL_STRING("world", copy->s.buf);
    sv_free(orig);
    TEST_ASSERT_EQUAL_STRING("world", copy->s.buf);   /* still valid after orig freed */
    sv_free(copy);
    Py_DECREF(obj);
}

static void test_deep_copy_list_independent(void) {
    PyObject *lst = PyList_New(1);
    PyList_SET_ITEM(lst, 0, PyLong_FromLong(3));
    SharedValue *orig = sv_from_pyobject(lst);
    SharedValue *copy = sv_deep_copy(orig);
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_NOT_EQUAL(orig->list, copy->list);
    TEST_ASSERT_EQUAL_INT(3, copy->list->items[0].i);
    sv_free(orig);
    TEST_ASSERT_EQUAL_INT(3, copy->list->items[0].i);
    sv_free(copy);
    Py_DECREF(lst);
}

static void test_deep_copy_null_returns_null(void) {
    TEST_ASSERT_NULL(sv_deep_copy(NULL));
}

/* -------------------------------------------------------------------------
 * Non-transferable type sets TypeError
 * -------------------------------------------------------------------------*/

static void test_unsupported_type_sets_error(void) {
    PyObject *obj = PyObject_CallNoArgs((PyObject *)&PyBaseObject_Type);
    SharedValue sv;
    int rc = sv_fill_from_pyobject(&sv, obj);
    TEST_ASSERT_EQUAL_INT(-1, rc);
    TEST_ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
    PyErr_Clear();
    Py_DECREF(obj);
}

/* -------------------------------------------------------------------------
 * pickled() wrapper encodes as SV_PICKLE and decodes back
 * -------------------------------------------------------------------------*/

static void test_pickled_encodes_as_sv_pickle(void) {
    if (PyType_Ready(&PickledType) < 0) {
        PyErr_Print();
        TEST_FAIL_MESSAGE("PyType_Ready(PickledType) failed");
    }
    PyObject *inner = PyLong_FromLong(42);
    PyObject *args  = PyTuple_Pack(1, inner);
    PyObject *wrapped = PyObject_Call((PyObject *)&PickledType, args, NULL);
    Py_DECREF(args);
    Py_DECREF(inner);
    TEST_ASSERT_NOT_NULL(wrapped);

    SharedValue sv;
    TEST_ASSERT_EQUAL_INT(0, sv_fill_from_pyobject(&sv, wrapped));
    TEST_ASSERT_EQUAL_INT(SV_PICKLE, sv.tag);
    TEST_ASSERT_NOT_NULL(sv.s.buf);
    TEST_ASSERT_GREATER_THAN(0, (int)sv.s.len);

    PyObject *out = sv_to_pyobject(&sv);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_INT(42, PyLong_AsLong(out));

    Py_DECREF(out);
    Py_DECREF(wrapped);
    sv_free_inline(&sv);
}

/* -------------------------------------------------------------------------
 * main
 * -------------------------------------------------------------------------*/

int main(void) {
    Py_Initialize();

    UNITY_BEGIN();

    RUN_TEST(test_none_roundtrip);
    RUN_TEST(test_bool_true_roundtrip);
    RUN_TEST(test_bool_false_roundtrip);
    RUN_TEST(test_int_roundtrip);
    RUN_TEST(test_negative_int_roundtrip);
    RUN_TEST(test_float_roundtrip);
    RUN_TEST(test_str_roundtrip);
    RUN_TEST(test_str_empty_roundtrip);
    RUN_TEST(test_bytes_roundtrip);
    RUN_TEST(test_list_roundtrip);
    RUN_TEST(test_empty_list_roundtrip);
    RUN_TEST(test_tuple_roundtrip);
    RUN_TEST(test_dict_roundtrip);
    RUN_TEST(test_nested_list_in_dict);
    RUN_TEST(test_sv_from_pyobject_allocates);
    RUN_TEST(test_sv_free_null_is_safe);
    RUN_TEST(test_sv_free_inline_null_is_safe);
    RUN_TEST(test_deep_copy_int);
    RUN_TEST(test_deep_copy_str_independent);
    RUN_TEST(test_deep_copy_list_independent);
    RUN_TEST(test_deep_copy_null_returns_null);
    RUN_TEST(test_unsupported_type_sets_error);
    RUN_TEST(test_pickled_encodes_as_sv_pickle);

    int result = UNITY_END();
    Py_Finalize();
    return result;
}
