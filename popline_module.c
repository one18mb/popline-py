/* pln_module.c — Python C extension for PopLine (optimized: direct Python object conversion) */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include "popline.h"

/* ═══════════════════════════════════════════════════════════
   Memory helpers (from popline.c)
   ═══════════════════════════════════════════════════════════ */

static void *py_realloc(void *p, size_t sz) {
    void *r = realloc(p, sz);
    if (!r && sz) { free(p); fprintf(stderr, "OOM\n"); abort(); }
    return r;
}

/* ═══════════════════════════════════════════════════════════
   Direct dumps: Python object → PopLine text
   (no intermediate pln_value_t)
   ═══════════════════════════════════════════════════════════ */

typedef struct {
    char *buf;
    int len, cap;
    int *stack;          /* 0=object, 1=array */
    int stack_len, stack_cap;
    int pending_pop;
    int awaiting_value;
    int has_leaf_value;
} py_gen_t;

static void py_gen_init(py_gen_t *g) {
    memset(g, 0, sizeof(*g));
    g->cap = 256;
    g->buf = (char *)malloc(g->cap);
    g->stack_cap = 64;
    g->stack = (int *)malloc(g->stack_cap * sizeof(int));
}

static void py_gen_free(py_gen_t *g) {
    free(g->buf); g->buf = NULL;
    free(g->stack); g->stack = NULL;
}

static void pg_ensure(py_gen_t *g, int extra) {
    if (g->len + extra + 1 > g->cap) {
        do { g->cap *= 2; } while (g->len + extra + 1 > g->cap);
        g->buf = (char *)py_realloc(g->buf, g->cap);
    }
}

static void pg_write(py_gen_t *g, const char *s) {
    int n = (int)strlen(s);
    pg_ensure(g, n);
    memcpy(g->buf + g->len, s, n);
    g->len += n;
}

static void pg_write_len(py_gen_t *g, const char *s, int n) {
    pg_ensure(g, n);
    memcpy(g->buf + g->len, s, n);
    g->len += n;
}

static void pg_writec(py_gen_t *g, char c) {
    pg_ensure(g, 1);
    g->buf[g->len++] = c;
}

static void pg_push(py_gen_t *g, int typ) {
    if (g->stack_len >= g->stack_cap) {
        g->stack_cap *= 2;
        g->stack = (int *)py_realloc(g->stack, g->stack_cap * sizeof(int));
    }
    g->stack[g->stack_len++] = typ;
}

static int pg_top(py_gen_t *g) {
    return g->stack_len > 0 ? g->stack[g->stack_len - 1] : -1;
}

static void pg_flush_pop(py_gen_t *g) {
    if (g->pending_pop > 0) {
        /* Insert " N" before the last \n (line-end pop suffix) */
        if (g->len > 0 && g->buf[g->len - 1] == '\n') {
            char tmp[16];
            int i = 0, x = g->pending_pop;
            int p = g->len - 1;
            if (x >= 10) { if (x >= 100) { tmp[i++] = '0' + x/100; x %= 100; } tmp[i++] = '0' + x/10; x %= 10; }
            tmp[i++] = '0' + x;
            pg_ensure(g, i + 1);
            memmove(g->buf + p + i + 1, g->buf + p, 1);
            g->buf[p] = ' ';
            memcpy(g->buf + p + 1, tmp, i);
            g->len += i + 1;
        }
        g->pending_pop = 0;
    }
}

static void pg_start_container(py_gen_t *g, char typ) {
    if (pg_top(g) == 0 && g->awaiting_value) {
        pg_writec(g, typ == 0 ? '{' : '[');
        g->awaiting_value = 0;
    } else {
        pg_flush_pop(g);
        pg_writec(g, typ == 0 ? '{' : '[');
    }
    pg_writec(g, '\n');
    pg_push(g, typ);
    g->awaiting_value = 0;
}

/* Forward declaration */
static void py_write_value(py_gen_t *g, PyObject *obj);

static PyObject *py_dumps_direct(PyObject *obj) {
    py_gen_t g;
    py_gen_init(&g);
    py_write_value(&g, obj);
    /* Flush remaining pop: all containers including root close explicitly */
    if (g.pending_pop > 0 && g.has_leaf_value) pg_flush_pop(&g);
    pg_ensure(&g, 1);
    g.buf[g.len] = '\0';
    PyObject *result = PyUnicode_FromString(g.buf);
    py_gen_free(&g);
    return result;
}

static void py_write_value(py_gen_t *g, PyObject *obj) {
    if (!obj || obj == Py_None) {
        if (pg_top(g) == 0) {
            g->awaiting_value = 0;
            pg_write(g, "null\n");
        } else {
            g->has_leaf_value = 1;
            pg_flush_pop(g);
            pg_write(g, "null\n");
        }
        return;
    }
    if (PyBool_Check(obj)) {
        const char *s = (obj == Py_True) ? "true" : "false";
        if (pg_top(g) == 0) {
            g->awaiting_value = 0;
            pg_write(g, s);
            pg_writec(g, '\n');
        } else {
            g->has_leaf_value = 1;
            pg_flush_pop(g);
            pg_write(g, s);
            pg_writec(g, '\n');
        }
        return;
    }
    if (PyLong_Check(obj)) {
        long long v = PyLong_AsLongLong(obj);
        char tmp[32]; int pos = 0;
        if (v < 0) { tmp[pos++] = '-'; v = -v; }
        if (v == 0) { tmp[pos++] = '0'; }
        else {
            char rev[24]; int rpos = 0;
            while (v) { rev[rpos++] = '0' + (v % 10); v /= 10; }
            while (rpos) tmp[pos++] = rev[--rpos];
        }
        if (pg_top(g) == 0) {
            g->awaiting_value = 0;
            pg_write_len(g, tmp, pos);
            pg_writec(g, '\n');
        } else {
            g->has_leaf_value = 1;
            pg_flush_pop(g);
            pg_write_len(g, tmp, pos);
            pg_writec(g, '\n');
        }
        return;
    }
    if (PyFloat_Check(obj)) {
        double d = PyFloat_AsDouble(obj);
        char tmp[64];
        int n = snprintf(tmp, sizeof(tmp), "%.15g", d);
        if (pg_top(g) == 0) {
            g->awaiting_value = 0;
            pg_write_len(g, tmp, n);
            pg_writec(g, '\n');
        } else {
            pg_flush_pop(g);
            pg_write_len(g, tmp, n);
            pg_writec(g, '\n');
        }
        return;
    }
    if (PyUnicode_Check(obj)) {
        Py_ssize_t len;
        const char *s = PyUnicode_AsUTF8AndSize(obj, &len);
        int n = (int)len;
        if (pg_top(g) == 0) {
            g->awaiting_value = 0;
        } else {
            pg_flush_pop(g);
        }
        /* Check for embedded quotes */
        int has_quote = 0;
        for (int i = 0; i < n; i++) { if (s[i] == '"') { has_quote = 1; break; } }
        pg_ensure(g, n + 3);
        g->buf[g->len++] = '"';
        if (has_quote) {
            for (int i = 0; i < n; i++) {
                g->buf[g->len++] = s[i];
                if (s[i] == '"') g->buf[g->len++] = '"';
            }
        } else {
            memcpy(g->buf + g->len, s, n); g->len += n;
        }
        g->buf[g->len++] = '"';
        g->buf[g->len++] = '\n';
        return;
    }
    if (PyDict_Check(obj)) {
        pg_start_container(g, 0); /* object */
        PyObject *key, *val;
        Py_ssize_t pos = 0;
        while (PyDict_Next(obj, &pos, &key, &val)) {
            const char *k;
            PyObject *key_str = NULL;
            if (PyUnicode_Check(key)) {
                k = PyUnicode_AsUTF8(key);
            } else {
                key_str = PyObject_Str(key);
                if (!key_str) return;
                k = PyUnicode_AsUTF8(key_str);
            }
            pg_flush_pop(g);
            pg_write(g, k);
            pg_write_len(g, ": ", 2);
            g->awaiting_value = 1;
            py_write_value(g, val);
            Py_XDECREF(key_str);
        }
        g->stack_len--;
        g->pending_pop++;
        return;
    }
    if (PyList_Check(obj) || PyTuple_Check(obj)) {
        pg_start_container(g, 1); /* array */
        Py_ssize_t n = PySequence_Length(obj);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *item = PySequence_GetItem(obj, i);
            if (!item) return;
            py_write_value(g, item);
            Py_DECREF(item);
        }
        g->stack_len--;
        g->pending_pop++;
        return;
    }
    /* Fallback: try str() */
    PyObject *str = PyObject_Str(obj);
    if (str) {
        const char *s = PyUnicode_AsUTF8(str);
        if (pg_top(g) == 0) {
            g->awaiting_value = 0;
        } else {
            pg_flush_pop(g);
        }
        int n = (int)strlen(s);
        pg_ensure(g, n + 3);
        g->buf[g->len++] = '"';
        for (int i = 0; i < n; i++) {
            g->buf[g->len++] = s[i];
            if (s[i] == '"') g->buf[g->len++] = '"';
        }
        g->buf[g->len++] = '"';
        g->buf[g->len++] = '\n';
        Py_DECREF(str);
    }
}

/* ═══════════════════════════════════════════════════════════
   Direct loads: PopLine text → Python objects
   (Fast path, no intermediate pln_value_t)
   ═══════════════════════════════════════════════════════════ */

typedef struct {
    PyObject **frames;       /* stack of dict/list */
    int *types;              /* 0=dict, 1=array */
    int frames_len, frames_cap;
    PyObject *key_obj;       /* current key (PyUnicode) for object context */
    char *strbuf;            /* multi-line string accumulator */
    int strbuf_len, strbuf_cap;
    int in_string;
    char error[256];
} py_parse_ctx_t;

static void pp_init(py_parse_ctx_t *pp) {
    memset(pp, 0, sizeof(*pp));
    pp->frames_cap = 64;
    pp->frames = (PyObject **)malloc(pp->frames_cap * sizeof(PyObject *));
    pp->types = (int *)malloc(pp->frames_cap * sizeof(int));
}

static void pp_free(py_parse_ctx_t *pp) {
    free(pp->frames);
    free(pp->types);
    Py_XDECREF(pp->key_obj);
    free(pp->strbuf);
}

static void pp_push(py_parse_ctx_t *pp, PyObject *container, int typ) {
    if (pp->frames_len >= pp->frames_cap) {
        pp->frames_cap *= 2;
        pp->frames = (PyObject **)py_realloc(pp->frames, pp->frames_cap * sizeof(PyObject *));
        pp->types = (int *)py_realloc(pp->types, pp->frames_cap * sizeof(int));
    }
    pp->frames[pp->frames_len] = container;
    pp->types[pp->frames_len] = typ;
    pp->frames_len++;
}

static PyObject *pp_top(py_parse_ctx_t *pp) {
    return pp->frames_len > 0 ? pp->frames[pp->frames_len - 1] : NULL;
}

static int pp_top_type(py_parse_ctx_t *pp) {
    return pp->frames_len > 0 ? pp->types[pp->frames_len - 1] : -1;
}

/* Forward-scan for " N" pop suffix: when space found, check if rest is all digits */
static int pp_fwd_trim_pop_suffix(const char *s, int len, int *value_len) {
    int in_string = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] == '"') in_string = !in_string;
        if (!in_string && s[i] == ' ') {
            int all_digits = 1;
            for (int j = i + 1; j < len; j++) {
                if (s[j] < '0' || s[j] > '9') { all_digits = 0; break; }
            }
            if (all_digits && i + 1 < len) {
                int pop = 0;
                for (int j = i + 1; j < len; j++) pop = pop * 10 + (s[j] - '0');
                *value_len = i;
                return pop;
            }
        }
    }
    *value_len = len;
    return 0;
}

static void pp_pop_layers(py_parse_ctx_t *pp, int n) {
    if (n > pp->frames_len) n = pp->frames_len;
    pp->frames_len -= n;
}

static void psb_ensure(py_parse_ctx_t *pp, int extra) {
    while (pp->strbuf_len + extra + 1 > pp->strbuf_cap) {
        pp->strbuf_cap = pp->strbuf_cap ? pp->strbuf_cap * 2 : 256;
        pp->strbuf = (char *)py_realloc(pp->strbuf, pp->strbuf_cap);
    }
}

static void psb_append(py_parse_ctx_t *pp, const char *s, int n) {
    psb_ensure(pp, n);
    memcpy(pp->strbuf + pp->strbuf_len, s, n);
    pp->strbuf_len += n;
    pp->strbuf[pp->strbuf_len] = '\0';
}

static int psb_append_line(py_parse_ctx_t *pp, const char *s, int len) {
    psb_ensure(pp, len + 1);
    memcpy(pp->strbuf + pp->strbuf_len, s, len);
    pp->strbuf_len += len;
    pp->strbuf[pp->strbuf_len++] = '\n';
    pp->strbuf[pp->strbuf_len] = '\0';
    return 0;
}

/* Parse a quoted string value (s points past the opening ") */
static PyObject *pp_parse_string(py_parse_ctx_t *pp, const char *s, int len) {
    int i = 0;
    int has_esc = 0;
    while (1) {
        if (i >= len) {
            /* Multi-line string: buffer accumulates lines */
            pp->in_string = 1;
            pp->strbuf_len = 0;
            psb_append(pp, s, len);
            psb_append(pp, "\n", 1);
            return NULL;
        }
        if (s[i] == '"') {
            if (i + 1 < len && s[i + 1] == '"') { has_esc = 1; i += 2; continue; }
            break;
        }
        i++;
    }
    /* Check for pop suffix " N" after closing quote */
    int after_quote = i + 1;
    if (after_quote < len) {
        int trailing_len = len - after_quote;
        int val_tmp = trailing_len;
        if (pp_fwd_trim_pop_suffix(s + after_quote, trailing_len, &val_tmp) > 0) {
            /* Valid pop suffix found — handled by caller's pop_layers */
        } else {
            for (int j = after_quote; j < len; j++) {
                if (s[j] != ' ' && s[j] != '\t') {
                    snprintf(pp->error, sizeof(pp->error), "引号后有多余内容");
                    return NULL;
                }
            }
        }
    }
    if (!has_esc)
        return PyUnicode_FromStringAndSize(s, i);
    /* Unescape "" → " */
    int out_len = 0;
    for (int j = 0; j < i; j++) {
        if (s[j] == '"' && j + 1 < i && s[j + 1] == '"') j++;
        out_len++;
    }
    char *unesc = (char *)malloc(out_len + 1);
    int p = 0;
    for (int j = 0; j < i; j++) {
        if (s[j] == '"' && j + 1 < i && s[j + 1] == '"') { unesc[p++] = '"'; j++; }
        else { unesc[p++] = s[j]; }
    }
    unesc[p] = '\0';
    PyObject *result = PyUnicode_FromString(unesc);
    free(unesc);
    return result;
}

/* Handle a line inside a multi-line string */
static int pp_handle_string_line(py_parse_ctx_t *pp, const char *line, int len) {
    int i = 0;
    while (1) {
        if (i >= len) {
            psb_append_line(pp, line, len);
            return 0;
        }
        if (line[i] == '"') {
            if (i + 1 < len && line[i + 1] == '"') { i += 2; continue; }
            break;
        }
        i++;
    }
    /* Check for pop suffix " N" after closing quote */
    int n_pop = 0;
    int after = i + 1;
    if (after < len) {
        int trailing_len = len - after;
        int val_tmp = trailing_len;
        n_pop = pp_fwd_trim_pop_suffix(line + after, trailing_len, &val_tmp);
        if (n_pop == 0) {
            for (int j = after; j < len; j++) {
                if (line[j] != ' ' && line[j] != '\t') {
                    snprintf(pp->error, sizeof(pp->error), "引号后有多余内容");
                    return -1;
                }
            }
        }
    }
    psb_append(pp, line, i);

    /* Unescape the accumulated buffer */
    int total = pp->strbuf_len;
    int out_len = 0;
    for (int j = 0; j < total; j++) {
        if (pp->strbuf[j] == '"' && j + 1 < total && pp->strbuf[j + 1] == '"') j++;
        out_len++;
    }
    char *unesc = (char *)malloc(out_len + 1);
    int p = 0;
    for (int j = 0; j < total; j++) {
        if (pp->strbuf[j] == '"' && j + 1 < total && pp->strbuf[j + 1] == '"') { unesc[p++] = '"'; j++; }
        else { unesc[p++] = pp->strbuf[j]; }
    }
    unesc[p] = '\0';
    PyObject *v = PyUnicode_FromString(unesc);
    free(unesc);
    pp->in_string = 0;
    pp->strbuf_len = 0;

    /* Add to parent container */
    if (pp_top_type(pp) == 0) {
        if (pp->key_obj) {
            PyDict_SetItem(pp_top(pp), pp->key_obj, v);
            Py_CLEAR(pp->key_obj);
        }
    } else {
        PyList_Append(pp_top(pp), v);
    }
    Py_DECREF(v);
    pp_pop_layers(pp, n_pop);
    return 1;
}

/* Parse a scalar value and return PyObject* */
static PyObject *pp_parse_scalar(py_parse_ctx_t *pp, const char *s, int len) {
    if (len <= 0) {
        snprintf(pp->error, sizeof(pp->error), "空值");
        return NULL;
    }
    if (s[0] == '"') return pp_parse_string(pp, s + 1, len - 1);

    if (len == 4 && memcmp(s, "true", 4) == 0) { Py_RETURN_TRUE; }
    if (len == 4 && memcmp(s, "null", 4) == 0) { Py_RETURN_NONE; }
    if (len == 5 && memcmp(s, "false", 5) == 0) { Py_RETURN_FALSE; }

    /* Try number */
    if ((s[0] >= '0' && s[0] <= '9') || s[0] == '-' || s[0] == '+') {
        char tmp[64];
        if (len >= (int)sizeof(tmp)) goto invalid;
        memcpy(tmp, s, len); tmp[len] = '\0';

        int is_float = 0;
        for (int i = 0; i < len; i++) {
            if (tmp[i] == '.' || tmp[i] == 'e' || tmp[i] == 'E') { is_float = 1; break; }
        }
        char *end;
        errno = 0;
        if (is_float) {
            double d = strtod(tmp, &end);
            if (end == tmp + len && errno != ERANGE) return PyFloat_FromDouble(d);
        } else {
            long long ll = strtoll(tmp, &end, 10);
            if (end == tmp + len && errno != ERANGE) return PyLong_FromLongLong(ll);
        }
    }

invalid:
    snprintf(pp->error, sizeof(pp->error), "非法值(字符串需用引号): '%.*s'", len, s);
    return NULL;
}

/* Parse one line */
static int pp_parse_line(py_parse_ctx_t *pp, const char *line, int len) {
    if (len > 0 && line[len - 1] == '\r') len--;

    if (pp->in_string) return pp_handle_string_line(pp, line, len);

    /* 消息体内不允许空行（frames_len > 1 表示在容器内） */
    if (len == 0) {
        if (pp->frames_len > 1) {
            snprintf(pp->error, sizeof(pp->error), "消息体内不允许空行");
            return -1;
        }
        return 0;
    }

    const char *rest = line;
    int rest_len = len;

    /* Root level: 支持所有类型 */
    if (pp->frames_len == 0 || (pp->frames_len == 1 && pp->types[0] == -1)) {
        if (rest_len == 1 && *rest == '{') {
            PyObject *d = PyDict_New();
            pp_push(pp, d, 0);
            return 0;
        }
        if (rest_len == 1 && *rest == '[') {
            PyObject *a = PyList_New(0);
            pp_push(pp, a, 1);
            return 0;
        }
        /* 标量根值 */
        PyObject *v = pp_parse_scalar(pp, rest, rest_len);
        if (!v) return -1;
        pp->frames[1] = v;
        pp->types[1] = -2;  /* scalar root marker */
        pp->frames_len = 2;
        return 1;  /* message complete */
    }

    int typ = pp_top_type(pp);
    if (typ == 0) {  /* object */
        int key_sep = -1;
        for (int i = 0; i < rest_len - 1; i++) {
            char c = rest[i];
            if (c == ':') {
                if (rest[i + 1] == ' ') { key_sep = i; break; }
                snprintf(pp->error, sizeof(pp->error), "非法键名: '%.*s'", rest_len, rest);
                return -1;
            }
            if (c == '"' || c == '{' || c == '[' ||
                c == '#' || c == ' ' || c == '\t') {
                snprintf(pp->error, sizeof(pp->error), "非法键名: '%.*s'", rest_len, rest);
                return -1;
            }
        }
        if (key_sep < 0) {
            snprintf(pp->error, sizeof(pp->error), "缺少 'key: value': '%.*s'", rest_len, rest);
            return -1;
        }
        /* Save key as PyUnicode (avoids malloc+memcpy+free) */
        Py_XDECREF(pp->key_obj);
        pp->key_obj = PyUnicode_FromStringAndSize(rest, key_sep);

        const char *vpart = rest + key_sep + 2;
        int vlen = rest_len - key_sep - 2;
        if (vlen == 1 && *vpart == '{') {
            PyObject *d = PyDict_New();
            PyDict_SetItem(pp_top(pp), pp->key_obj, d);
            Py_DECREF(d);
            pp_push(pp, d, 0);
            return 0;
        }
        if (vlen == 1 && *vpart == '[') {
            PyObject *a = PyList_New(0);
            PyDict_SetItem(pp_top(pp), pp->key_obj, a);
            Py_DECREF(a);
            pp_push(pp, a, 1);
            return 0;
        }
        /* Scalar value */
        int pop_n = 0;
        int val_len = vlen;
        if (vlen > 0 && vpart[0] != '{' && vpart[0] != '[')
            pop_n = pp_fwd_trim_pop_suffix(vpart, vlen, &val_len);
        if (val_len <= 0) return 0;
        PyObject *v = pp_parse_scalar(pp, vpart, val_len);
        if (!v) return pp->error[0] ? -1 : 0;
        PyDict_SetItem(pp_top(pp), pp->key_obj, v);
        Py_DECREF(v);
        pp_pop_layers(pp, pop_n);
        return 0;
    }

    if (typ == 1) {  /* array */        if (rest_len == 1 && *rest == '{') {
            PyObject *d = PyDict_New();
            PyList_Append(pp_top(pp), d);
            Py_DECREF(d);
            pp_push(pp, d, 0);
            return 0;
        }
        if (rest_len == 1 && *rest == '[') {
            PyObject *a = PyList_New(0);
            PyList_Append(pp_top(pp), a);
            Py_DECREF(a);
            pp_push(pp, a, 1);
            return 0;
        }
        int pop_n = 0;
        int rest_val_len = rest_len;
        if (rest_len > 0 && rest[0] != '{' && rest[0] != '[')
            pop_n = pp_fwd_trim_pop_suffix(rest, rest_len, &rest_val_len);
        if (rest_val_len <= 0) return 0;
        PyObject *v = pp_parse_scalar(pp, rest, rest_val_len);
        if (!v) return pp->error[0] ? -1 : 0;
        PyList_Append(pp_top(pp), v);
        Py_DECREF(v);
        pp_pop_layers(pp, pop_n);
        return 0;
    }

    snprintf(pp->error, sizeof(pp->error), "内部错误");
    return -1;
}

static PyObject *py_loads_direct(const char *text) {
    py_parse_ctx_t pp;
    pp_init(&pp);

    /* Push sentinel frame (type=-1) so root detection works */
    pp_push(&pp, NULL, -1);

    const char *s = text;
    const char *line_start = s;

    for (;;) {
        const char *nl = strchr(s, '\n');
        if (nl) {
            int r = pp_parse_line(&pp, line_start, (int)(nl - line_start));
            if (r < 0) {
                PyErr_SetString(PyExc_ValueError, pp.error[0] ? pp.error : "解析错误");
                /* Only DECREF root (frame[1]); children owned by parent */
                if (pp.frames_len >= 1 && pp.frames[1])
                    Py_XDECREF(pp.frames[1]);
                pp_free(&pp);
                return NULL;
            }
            if (r > 0) break; /* message complete */
            s = nl + 1;
            line_start = s;
        } else {
            if (*line_start) {
                int r = pp_parse_line(&pp, line_start, (int)strlen(line_start));
                if (r < 0) {
                    PyErr_SetString(PyExc_ValueError, pp.error[0] ? pp.error : "解析错误");
                    if (pp.frames_len >= 1 && pp.frames[1])
                        Py_XDECREF(pp.frames[1]);
                    pp_free(&pp);
                    return NULL;
                }
            }
            break;
        }
    }

    /* Result is at frame[1] (after sentinel) — steal its reference */
    PyObject *result = NULL;
    if (pp.frames_len >= 1 && pp.frames[1]) {
        result = pp.frames[1];
        /* result has refcount 1 (from PyDict_New/PyList_New).
           We steal this reference for the caller. */
    }
    /* Don't DECREF children: they're owned by their parent.
       Root (frame[1]) reference is stolen for the return value. */
    pp_free(&pp);

    if (!result) {
        PyErr_SetString(PyExc_ValueError, "空输入");
        return NULL;
    }
    return result;
}

/* ═══════════════════════════════════════════════════════════
   Old DOM-based functions (kept for loads_json/dumps_json)
   ═══════════════════════════════════════════════════════════ */

static PyObject *pln_to_python(pln_value_t *v) {
    if (!v) Py_RETURN_NONE;
    switch (v->type) {
    case PLN_NULL:    Py_RETURN_NONE;
    case PLN_BOOL:
        Py_INCREF(v->data.bool_val ? Py_True : Py_False);
        return v->data.bool_val ? Py_True : Py_False;
    case PLN_INT:     return PyLong_FromLongLong(v->data.int_val);
    case PLN_FLOAT:   return PyFloat_FromDouble(v->data.float_val);
    case PLN_STRING:  return PyUnicode_FromString(v->data.string_val ? v->data.string_val : "");
    case PLN_OBJECT: {
        PyObject *dict = PyDict_New();
        for (pln_value_t *c = v->child; c; c = c->next) {
            PyObject *val = pln_to_python(c);
            PyDict_SetItemString(dict, c->key ? c->key : "", val);
            Py_DECREF(val);
        }
        return dict;
    }
    case PLN_ARRAY: {
        PyObject *list = PyList_New(0);
        for (pln_value_t *c = v->child; c; c = c->next) {
            PyObject *val = pln_to_python(c);
            PyList_Append(list, val);
            Py_DECREF(val);
        }
        return list;
    }
    }
    Py_RETURN_NONE;
}

static pln_value_t *python_to_pl(PyObject *obj) {
    if (!obj || obj == Py_None)    return pln_value_new_null();
    if (PyBool_Check(obj))         return pln_value_new_bool(obj == Py_True ? 1 : 0);
    if (PyLong_Check(obj))         return pln_value_new_int(PyLong_AsLongLong(obj));
    if (PyFloat_Check(obj))        return pln_value_new_float(PyFloat_AsDouble(obj));
    if (PyUnicode_Check(obj)) {
        Py_ssize_t len;
        const char *s = PyUnicode_AsUTF8AndSize(obj, &len);
        return pln_value_new_string_len(s, (int)len);
    }
    if (PyBytes_Check(obj)) {
        char *s = PyBytes_AS_STRING(obj);
        int len = (int)PyBytes_GET_SIZE(obj);
        return pln_value_new_string_len(s, len);
    }
    if (PyDict_Check(obj)) {
        pln_value_t *v = pln_value_new_object();
        PyObject *key, *val;
        Py_ssize_t pos = 0;
        while (PyDict_Next(obj, &pos, &key, &val)) {
            const char *k;
            if (PyUnicode_Check(key)) k = PyUnicode_AsUTF8(key);
            else if (PyBytes_Check(key)) k = PyBytes_AS_STRING(key);
            else {
                PyObject *str = PyObject_Str(key);
                k = PyUnicode_AsUTF8(str);
                Py_DECREF(str);
            }
            pln_value_t *child = python_to_pl(val);
            pln_value_add_to_object(v, k, child);
        }
        return v;
    }
    if (PyList_Check(obj) || PyTuple_Check(obj)) {
        pln_value_t *v = pln_value_new_array();
        Py_ssize_t n = PySequence_Length(obj);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *item = PySequence_GetItem(obj, i);
            pln_value_t *child = python_to_pl(item);
            Py_DECREF(item);
            pln_value_add_to_array(v, child);
        }
        return v;
    }
    PyObject *str = PyObject_Str(obj);
    if (str) {
        const char *s = PyUnicode_AsUTF8(str);
        pln_value_t *v = pln_value_new_string(s ? s : "");
        Py_DECREF(str);
        return v;
    }
    return pln_value_new_null();
}

/* ═══════════════════════════════════════════════════════════
   Module functions
   ═══════════════════════════════════════════════════════════ */

static PyObject *py_loads(PyObject *self, PyObject *args) {
    const char *text;
    if (!PyArg_ParseTuple(args, "s", &text)) return NULL;
    return py_loads_direct(text);
}

static PyObject *py_dumps(PyObject *self, PyObject *args) {
    PyObject *obj;
    if (!PyArg_ParseTuple(args, "O", &obj)) return NULL;
    return py_dumps_direct(obj);
}

/* loads_stream/dumps_stream use the old DOM-based approach for simplicity */

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
                memcpy(msg, msg_start, msglen);
                msg[msglen] = '\0';
                PyObject *pyv = py_loads_direct(msg);
                free(msg);
                if (pyv) PyList_Append(list, pyv);
                Py_XDECREF(pyv);
                msg_start = nl ? nl + 1 : s + line_len;
            } else {
                msg_start = nl ? nl + 1 : s + line_len;
            }
        }

        if (!nl) break;
        s = nl + 1;
    }

    if (*msg_start && s > msg_start) {
        PyObject *pyv = py_loads_direct(msg_start);
        if (pyv) PyList_Append(list, pyv);
        Py_XDECREF(pyv);
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
        PyObject *s = py_dumps_direct(obj);
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

static PyObject *py_loads_json(PyObject *self, PyObject *args) {
    const char *text;
    if (!PyArg_ParseTuple(args, "s", &text)) return NULL;
    pln_value_t *v = pln_loads_json(text);
    if (!v) { PyErr_SetString(PyExc_ValueError, "JSON parse error"); return NULL; }
    PyObject *result = pln_to_python(v);
    pln_value_free(v);
    return result;
}

static PyObject *py_dumps_json(PyObject *self, PyObject *args) {
    PyObject *obj;
    if (!PyArg_ParseTuple(args, "O", &obj)) return NULL;
    pln_value_t *v = python_to_pl(obj);
    if (!v) { PyErr_SetString(PyExc_TypeError, "conversion failed"); return NULL; }
    char *s = pln_dumps_json(v);
    PyObject *result = PyUnicode_FromString(s ? s : "");
    free(s);
    pln_value_free(v);
    return result;
}

/* ═══════════════════════════════════════════════════════════
    Module definition
   ═══════════════════════════════════════════════════════════ */

static PyMethodDef popline_methods[] = {
    {"loads",        py_loads,        METH_VARARGS, "Parse a single PopLine message to Python object (direct, no intermediate DOM)."},
    {"dumps",        py_dumps,        METH_VARARGS, "Serialize Python object to PopLine text (direct, no intermediate DOM)."},
    {"loads_stream", py_loads_stream, METH_VARARGS, "Parse a multi-message PopLine stream."},
    {"dumps_stream", py_dumps_stream, METH_VARARGS, "Serialize multiple objects to PopLine stream."},
    {"loads_json",   py_loads_json,   METH_VARARGS, "Parse JSON text to Python object (via PopLine DOM)."},
    {"dumps_json",   py_dumps_json,   METH_VARARGS, "Serialize Python object to JSON text (via PopLine DOM)."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef pln_module = {
    PyModuleDef_HEAD_INIT,
    "pln",
    "PopLine — Line-oriented serialization format (C extension, Python-direct fast path)",
    -1,
    popline_methods
};

PyMODINIT_FUNC PyInit_pln(void) {
    return PyModule_Create(&pln_module);
}
