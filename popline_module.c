/* popline_module.c — Python C extension for PopLine
 * Uses the shared C core's SAX parser (popline_sax.c) and generator API (popline.c)
 * for parsing and serializing PopLine text with direct Python object conversion
 * (no intermediate pln_value_t DOM). */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "popline.h"
#include "popline_sax.h"

/* ══════════════════════════════════════════════════════════════════
   SAX-based parser: PopLine text → Python objects via event callbacks
   ══════════════════════════════════════════════════════════════════ */

typedef struct {
    PyObject **frames;       /* stack of dict/list containers (raw pointers) */
    int *types;              /* 0=dict, 1=array */
    int frames_len, frames_cap;
    PyObject *key_obj;       /* current key (owned ref) for dict context */
    PyObject *root;          /* root object (owned ref) */
    char error[256];
} sax_builder_t;

/* Add value to current container (or save as root). Does NOT Py_INCREF v —
   caller retains ownership of its ref. */
static void sax_add_value(sax_builder_t *b, PyObject *v) {
    if (b->frames_len == 0) {
        b->root = v;
        Py_INCREF(v);
        return;
    }
    PyObject *parent = b->frames[b->frames_len - 1];
    if (b->types[b->frames_len - 1] == 0 && b->key_obj) {
        PyDict_SetItem(parent, b->key_obj, v);
        Py_CLEAR(b->key_obj);
    } else {
        PyList_Append(parent, v);
    }
}

/* Push a new container onto the frame stack and link it to its parent. */
static int sax_push_container(sax_builder_t *b, PyObject *container, int typ) {
    if (b->frames_len > 0) {
        PyObject *parent = b->frames[b->frames_len - 1];
        if (b->types[b->frames_len - 1] == 0 && b->key_obj) {
            PyDict_SetItem(parent, b->key_obj, container);
            Py_CLEAR(b->key_obj);
        } else {
            PyList_Append(parent, container);
        }
        /* We created the container with ref=1 and now the parent
           INCREFed it via SetItem/Append. Our initial ref is released:
           after pushing to frames we "forget" our ref and only keep a
           raw pointer (the parent's ref keeps it alive). */
        Py_DECREF(container);
    } else {
        /* Root container — own the ref */
        b->root = container;
    }
    if (b->frames_len >= b->frames_cap) {
        b->frames_cap *= 2;
        b->frames = (PyObject **)realloc(b->frames, b->frames_cap * sizeof(PyObject *));
        b->types   = (int *)realloc(b->types, b->frames_cap * sizeof(int));
    }
    b->frames[b->frames_len] = container;
    b->types[b->frames_len] = typ;
    b->frames_len++;
    return 0;
}

static void sax_apply_pop(sax_builder_t *b, int n) {
    if (n > b->frames_len) n = b->frames_len;
    b->frames_len -= n;
}

static int sax_callback(const pln_sax_ev_t *ev, void *user_data) {
    sax_builder_t *b = (sax_builder_t *)user_data;

    switch (ev->type) {
    case PLN_SAX_OBJ_BEGIN: {
        PyObject *d = PyDict_New();
        if (!d) return 1;
        return sax_push_container(b, d, 0);
    }
    case PLN_SAX_ARR_BEGIN: {
        PyObject *a = PyList_New(0);
        if (!a) return 1;
        return sax_push_container(b, a, 1);
    }
    case PLN_SAX_KEY: {
        Py_XDECREF(b->key_obj);
        b->key_obj = PyUnicode_FromStringAndSize(ev->data, ev->len);
        return b->key_obj ? 0 : 1;
    }
    case PLN_SAX_STR: {
        PyObject *v = PyUnicode_FromStringAndSize(ev->data, ev->len);
        if (!v) return 1;
        sax_add_value(b, v);
        Py_DECREF(v);
        sax_apply_pop(b, ev->pop);
        return 0;
    }
    case PLN_SAX_INT: {
        PyObject *v = PyLong_FromLongLong(ev->int_val);
        if (!v) return 1;
        sax_add_value(b, v);
        Py_DECREF(v);
        sax_apply_pop(b, ev->pop);
        return 0;
    }
    case PLN_SAX_FLOAT: {
        PyObject *v = PyFloat_FromDouble(ev->float_val);
        if (!v) return 1;
        sax_add_value(b, v);
        Py_DECREF(v);
        sax_apply_pop(b, ev->pop);
        return 0;
    }
    case PLN_SAX_BOOL: {
        PyObject *v = ev->bool_val ? Py_True : Py_False;
        Py_INCREF(v);
        sax_add_value(b, v);
        Py_DECREF(v);
        sax_apply_pop(b, ev->pop);
        return 0;
    }
    case PLN_SAX_NULL: {
        PyObject *v = Py_None;
        Py_INCREF(v);
        sax_add_value(b, v);
        Py_DECREF(v);
        sax_apply_pop(b, ev->pop);
        return 0;
    }
    case PLN_SAX_OBJ_END:
    case PLN_SAX_ARR_END:
        if (b->frames_len > 0) b->frames_len--;
        return 0;
    case PLN_SAX_DONE:
        return 0;
    }
    return 0;
}

static PyObject *sax_loads(const char *text) {
    sax_builder_t b;
    memset(&b, 0, sizeof(b));
    b.frames_cap = 64;
    b.frames = (PyObject **)malloc(b.frames_cap * sizeof(PyObject *));
    b.types   = (int *)malloc(b.frames_cap * sizeof(int));
    if (!b.frames || !b.types) {
        free(b.frames); free(b.types);
        return PyErr_NoMemory();
    }

    int ret = pln_sax_parse(text, sax_callback, &b);

    PyObject *result = NULL;
    if (ret == 0 && b.root) {
        result = b.root;  /* steal ref */
        b.root = NULL;
    }

    Py_XDECREF(b.root);
    Py_XDECREF(b.key_obj);
    free(b.frames);
    free(b.types);

    if (!result) {
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_ValueError,
                            b.error[0] ? b.error : "PopLine parse error");
    }
    return result;
}

/* ══════════════════════════════════════════════════════════════════
   Generator-based serializer: Python objects → PopLine text
   Uses pln_gen_t from the shared C core (popline.c).
   ══════════════════════════════════════════════════════════════════ */

static void gen_write_value(pln_gen_t *g, PyObject *obj) {
    if (!obj || obj == Py_None) {
        pln_gen_value_null(g);
        return;
    }
    if (PyBool_Check(obj)) {
        pln_gen_value_bool(g, obj == Py_True);
        return;
    }
    if (PyLong_Check(obj)) {
        long long v = PyLong_AsLongLong(obj);
        if (v == -1 && PyErr_Occurred()) return;
        pln_gen_value_int(g, v);
        return;
    }
    if (PyFloat_Check(obj)) {
        double d = PyFloat_AsDouble(obj);
        if (d == -1.0 && PyErr_Occurred()) return;
        pln_gen_value_float(g, d);
        return;
    }
    if (PyUnicode_Check(obj)) {
        const char *s = PyUnicode_AsUTF8(obj);
        pln_gen_value_string(g, s);
        return;
    }
    if (PyDict_Check(obj)) {
        pln_gen_begin_object(g);
        PyObject *key, *val;
        Py_ssize_t pos = 0;
        while (PyDict_Next(obj, &pos, &key, &val)) {
            const char *k = PyUnicode_AsUTF8(key);
            if (!k) continue;
            pln_gen_key(g, k);
            gen_write_value(g, val);
        }
        pln_gen_end_object(g);
        return;
    }
    if (PyList_Check(obj) || PyTuple_Check(obj)) {
        pln_gen_begin_array(g);
        Py_ssize_t n = PySequence_Length(obj);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *item = PySequence_GetItem(obj, i);
            if (!item) return;
            gen_write_value(g, item);
            Py_DECREF(item);
        }
        pln_gen_end_array(g);
        return;
    }
    /* Fallback: str() */
    PyObject *str = PyObject_Str(obj);
    if (str) {
        const char *s = PyUnicode_AsUTF8(str);
        if (s) pln_gen_value_string(g, s);
        Py_DECREF(str);
    }
}

static PyObject *gen_dumps(PyObject *obj) {
    pln_gen_t g;
    pln_gen_init(&g);
    gen_write_value(&g, obj);
    /* Flush remaining pop */
    if (g.pending_pop > 0 && g.has_leaf_value) pln_gen_flush(&g);
    const char *s = pln_gen_getvalue(&g);
    PyObject *result = PyUnicode_FromString(s);
    pln_gen_free(&g);
    return result;
}

/* ══════════════════════════════════════════════════════════════════
   Stream functions (simple split/join around single-msg functions)
   ══════════════════════════════════════════════════════════════════ */

static PyObject *py_loads_stream(PyObject *self, PyObject *args) {
    const char *text;
    if (!PyArg_ParseTuple(args, "s", &text)) return NULL;

    PyObject *list = PyList_New(0);
    if (!list) return NULL;

    const char *msg_start = text;
    const char *s = text;

    for (;;) {
        const char *nl = strchr(s, '\n');
        int line_len = nl ? (int)(nl - s) : (int)strlen(s);

        if (line_len == 0 || (line_len == 1 && *s == '\r')) {
            if (s > msg_start) {
                int msglen = (int)(s - msg_start);
                char *msg = (char *)malloc(msglen + 1);
                if (!msg) { Py_DECREF(list); return PyErr_NoMemory(); }
                memcpy(msg, msg_start, msglen);
                msg[msglen] = '\0';
                PyObject *pv = sax_loads(msg);
                free(msg);
                if (pv) {
                    PyList_Append(list, pv);
                    Py_DECREF(pv);
                }
                msg_start = nl ? nl + 1 : s + line_len;
            } else {
                msg_start = nl ? nl + 1 : s + line_len;
            }
        }
        if (!nl) break;
        s = nl + 1;
    }

    if (*msg_start && s > msg_start) {
        PyObject *pv = sax_loads(msg_start);
        if (pv) { PyList_Append(list, pv); Py_DECREF(pv); }
    }
    return list;
}

static PyObject *py_dumps_stream(PyObject *self, PyObject *args) {
    PyObject *objs;
    if (!PyArg_ParseTuple(args, "O", &objs)) return NULL;
    if (!PyList_Check(objs) && !PyTuple_Check(objs)) {
        PyErr_SetString(PyExc_TypeError, "expected list or tuple");
        return NULL;
    }

    Py_ssize_t n = PySequence_Length(objs);
    PyObject *parts = PyList_New(n);
    if (!parts) return NULL;

    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *obj = PySequence_GetItem(objs, i);
        if (!obj) { Py_DECREF(parts); return NULL; }
        PyObject *s = gen_dumps(obj);
        Py_DECREF(obj);
        if (s) {
            PyList_SetItem(parts, i, s);
        } else {
            PyList_SetItem(parts, i, PyUnicode_FromString(""));
        }
    }

    PyObject *sep = PyUnicode_FromString("\n\n");
    PyObject *result = PyUnicode_Join(sep, parts);
    Py_DECREF(sep);
    Py_DECREF(parts);

    if (result) {
        PyObject *nl = PyUnicode_FromString("\n");
        PyObject *tmp = PyUnicode_Concat(result, nl);
        Py_DECREF(result);
        Py_DECREF(nl);
        result = tmp;
    }
    return result;
}

/* ══════════════════════════════════════════════════════════════════
   Module functions
   ══════════════════════════════════════════════════════════════════ */

static PyObject *py_loads(PyObject *self, PyObject *args) {
    const char *text;
    if (!PyArg_ParseTuple(args, "s", &text)) return NULL;
    return sax_loads(text);
}

static PyObject *py_dumps(PyObject *self, PyObject *args) {
    PyObject *obj;
    if (!PyArg_ParseTuple(args, "O", &obj)) return NULL;
    return gen_dumps(obj);
}

static PyMethodDef popline_methods[] = {
    {"loads",        py_loads,        METH_VARARGS,
     "Parse a single PopLine message to Python object (SAX-based, no intermediate DOM)."},
    {"dumps",        py_dumps,        METH_VARARGS,
     "Serialize Python object to PopLine text (generator-based, no intermediate DOM)."},
    {"loads_stream", py_loads_stream, METH_VARARGS,
     "Parse a multi-message PopLine stream."},
    {"dumps_stream", py_dumps_stream, METH_VARARGS,
     "Serialize multiple objects to PopLine stream."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef pln_module = {
    PyModuleDef_HEAD_INIT,
    "pln",
    "PopLine — Line-oriented serialization format (C extension, SAX + generator fast path)",
    -1,
    popline_methods
};

PyMODINIT_FUNC PyInit_pln(void) {
    return PyModule_Create(&pln_module);
}
