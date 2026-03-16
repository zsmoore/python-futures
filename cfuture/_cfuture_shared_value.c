/*
 * cfuture/_cfuture_shared_value.c
 * SharedValue encode/decode/copy/free — neutral-heap representation of Python objects.
 */

#include "_cfuture_internal.h"

void sv_free_inline(SharedValue *sv) {
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

void sv_free(SharedValue *sv) {
    if (!sv) return;
    sv_free_inline(sv);
    free(sv);
}

int sv_fill_from_pyobject(SharedValue *sv, PyObject *obj) {
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

SharedValue *sv_from_pyobject(PyObject *obj) {
    SharedValue *sv = malloc(sizeof(SharedValue));
    if (!sv) { PyErr_NoMemory(); return NULL; }
    if (sv_fill_from_pyobject(sv, obj) < 0) {
        free(sv);
        return NULL;
    }
    return sv;
}

void sv_deep_copy_inline(SharedValue *dst, const SharedValue *src) {
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

SharedValue *sv_deep_copy(const SharedValue *src) {
    if (!src) return NULL;
    SharedValue *dst = malloc(sizeof(SharedValue));
    if (!dst) return NULL;
    sv_deep_copy_inline(dst, src);
    return dst;
}

PyObject *sv_to_pyobject(const SharedValue *sv) {
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
