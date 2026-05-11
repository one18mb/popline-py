/* popline.c — PopLine v2 C reference implementation (optimized) */

#include "popline.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>

/* ─── 内存辅助 ─────────────────────────────────────────── */

static void *pln_realloc(void *p, size_t sz) {
    void *r = realloc(p, sz);
    if (!r && sz) { free(p); fprintf(stderr, "OOM\n"); abort(); }
    return r;
}

static char *pln_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (!d) { fprintf(stderr, "OOM\n"); abort(); }
    memcpy(d, s, n);
    return d;
}

static char *pln_strdupl(const char *s, int len) {
    if (!s) return NULL;
    char *d = (char *)malloc(len + 1);
    if (!d) { fprintf(stderr, "OOM\n"); abort(); }
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

/* ─── 字符串转义 ────────────────────────────────────────── */

/* 就地转义：将 src[0..len-1] 写入 dst，处理后返回写入长度。
   如果 dst 为 NULL，只计算需要的长度。*/
int pln_unescape_to(const char *s, int len, char *dst) {
    int j = 0;
    for (int i = 0; i < len; ) {
        if (s[i] == '"' && i + 1 < len && s[i + 1] == '"') {
            if (dst) dst[j] = '"';
            j++; i += 2;
        } else {
            if (dst) dst[j] = s[i];
            j++; i++;
        }
    }
    return j;
}

char *pln_unescape(const char *s, int len) {
    int out_len = pln_unescape_to(s, len, NULL);
    char *out = (char *)malloc(out_len + 1);
    pln_unescape_to(s, len, out);
    out[out_len] = '\0';
    return out;
}



/* ─── 生成器 ─────────────────────────────────────────────── */

#define PLN_GEN_INIT_CAP 256

void pln_gen_init(pln_gen_t *g) {
    memset(g, 0, sizeof(*g));
    g->cap = PLN_GEN_INIT_CAP;
    g->buf = (char *)malloc(g->cap);
    g->stack_cap = 64;
    g->stack = (char *)malloc(g->stack_cap);
}

void pln_gen_free(pln_gen_t *g) {
    free(g->buf); g->buf = NULL;
    free(g->stack); g->stack = NULL;
    g->len = g->cap = g->stack_len = g->stack_cap = 0;
}

static inline void g_ensure(pln_gen_t *g, int extra) {
    if (g->len + extra + 1 > g->cap) {
        do { g->cap *= 2; } while (g->len + extra + 1 > g->cap);
        g->buf = (char *)pln_realloc(g->buf, g->cap);
    }
}

static void g_write_len(pln_gen_t *g, const char *s, int n) {
    g_ensure(g, n);
    memcpy(g->buf + g->len, s, n);
    g->len += n;
}

static void g_write(pln_gen_t *g, const char *s) {
    g_write_len(g, s, (int)strlen(s));
}

static void g_writec(pln_gen_t *g, char c) {
    g_ensure(g, 1);
    g->buf[g->len++] = c;
}

static inline void g_push(pln_gen_t *g, char c) {
    if (g->stack_len >= g->stack_cap) {
        g->stack_cap *= 2;
        g->stack = (char *)pln_realloc(g->stack, g->stack_cap);
    }
    g->stack[g->stack_len++] = c;
}

static inline char g_top(pln_gen_t *g) {
    return g->stack_len > 0 ? g->stack[g->stack_len - 1] : 0;
}

static void g_flush_pop(pln_gen_t *g) {
    if (g->pending_pop > 0) {
        /* inline itoa — faster than snprintf */
        char tmp[16];
        int x = g->pending_pop, pos = 0;
        if (x >= 10) { if (x >= 100) { tmp[pos++] = '0' + x/100; x %= 100; } tmp[pos++] = '0' + x/10; x %= 10; }
        tmp[pos++] = '0' + x;
        tmp[pos++] = ' ';
        g_write_len(g, tmp, pos);
        g->pending_pop = 0;
    }
}

static void g_start_container(pln_gen_t *g, const char *ch, char typ) {
    if (g->stack_len > 0 && g_top(g) == 'o' && g->awaiting_value) {
        g_write_len(g, ch, 1);
        g->awaiting_value = 0;
    } else {
        g_flush_pop(g);
        g_write_len(g, ch, 1);
    }
    g_writec(g, '\n');
    g_push(g, typ);
    g->need_key = (typ == 'o');
    g->awaiting_value = 0;
}

void pln_gen_begin_object(pln_gen_t *g) { g_start_container(g, "{", 'o'); }
void pln_gen_begin_array(pln_gen_t *g)  { g_start_container(g, "[", 'a'); }

void pln_gen_end_object(pln_gen_t *g) {
    if (g_top(g) != 'o') { fprintf(stderr, "end_object mismatch\n"); abort(); }
    g->stack_len--;
    g->pending_pop++;
    if (g_top(g) == 'o') g->need_key = 1;
}

void pln_gen_end_array(pln_gen_t *g) {
    if (g_top(g) != 'a') { fprintf(stderr, "end_array mismatch\n"); abort(); }
    g->stack_len--;
    g->pending_pop++;
    if (g_top(g) == 'o') g->need_key = 1;
}

void pln_gen_key(pln_gen_t *g, const char *k) {
    g_flush_pop(g);
    g_write(g, k);
    g_write_len(g, ": ", 2);
    g->need_key = 0;
    g->awaiting_value = 1;
}

static void g_put_value_len(pln_gen_t *g, const char *s, int n) {
    if (g_top(g) == 'o') {
        g->awaiting_value = 0;
        g_write_len(g, s, n);
        g_writec(g, '\n');
        g->need_key = 1;
    } else {
        g_flush_pop(g);
        g_write_len(g, s, n);
        g_writec(g, '\n');
    }
}

void pln_gen_value_null(pln_gen_t *g)   { g_put_value_len(g, "null", 4); }
void pln_gen_value_bool(pln_gen_t *g, int v) { g_put_value_len(g, v ? "true" : "false", v ? 4 : 5); }
void pln_gen_value_int(pln_gen_t *g, long long v) {
    char tmp[32]; int pos = 0;
    if (v < 0) { tmp[pos++] = '-'; v = -v; }
    if (v == 0) { tmp[pos++] = '0'; }
    else {
        char rev[24]; int rpos = 0;
        while (v) { rev[rpos++] = '0' + (v % 10); v /= 10; }
        while (rpos) tmp[pos++] = rev[--rpos];
    }
    g_put_value_len(g, tmp, pos);
}
void pln_gen_value_float(pln_gen_t *g, double v) {
    char tmp[64]; int pos = snprintf(tmp, sizeof(tmp), "%.15g", v);
    g_put_value_len(g, tmp, pos);
}
void pln_gen_value_string(pln_gen_t *g, const char *v) {
    /* 写前导引号 */
    if (g_top(g) == 'o') {
        g->awaiting_value = 0;
        g->need_key = 1;
    } else {
        g_flush_pop(g);
    }

    /* 直接写入缓冲：先快速扫描是否需要转义 */
    int has_quote = 0, n = (int)strlen(v);
    for (int i = 0; i < n; i++) { if (v[i] == '"') { has_quote = 1; break; } }

    if (!has_quote) {
        /* 常见路径：无转义，一次写入 */
        g_ensure(g, n + 3);
        g->buf[g->len++] = '"';
        memcpy(g->buf + g->len, v, n); g->len += n;
        g->buf[g->len++] = '"';
        g->buf[g->len++] = '\n';
    } else {
        /* 罕见路径：有 ""，逐字符写入（无额外分配） */
        int est = n + (n / 8) + 3;
        g_ensure(g, est);
        g->buf[g->len++] = '"';
        for (int i = 0; i < n; i++) {
            g->buf[g->len++] = v[i];
            if (v[i] == '"') g->buf[g->len++] = '"';
        }
        g->buf[g->len++] = '"';
        g->buf[g->len++] = '\n';
    }
}

const char *pln_gen_getvalue(pln_gen_t *g) {
    g_ensure(g, 1);
    g->buf[g->len] = '\0';
    return g->buf;
}

/* ─── DOM 值 ──────────────────────────────────────────────── */

pln_value_t *pln_value_new(pln_value_type_t t) {
    pln_value_t *v = (pln_value_t *)malloc(sizeof(pln_value_t));
    v->type = t;
    v->data.int_val = 0;
    v->child = NULL;
    v->next = NULL;
    v->key = NULL;
    return v;
}

pln_value_t *pln_value_new_object(void)   { return pln_value_new(PLN_OBJECT); }
pln_value_t *pln_value_new_array(void)    { return pln_value_new(PLN_ARRAY); }
pln_value_t *pln_value_new_null(void)     { return pln_value_new(PLN_NULL); }

pln_value_t *pln_value_new_bool(int b) {
    pln_value_t *v = pln_value_new(PLN_BOOL);
    v->data.bool_val = b ? 1 : 0;
    return v;
}

pln_value_t *pln_value_new_int(long long i) {
    pln_value_t *v = pln_value_new(PLN_INT);
    v->data.int_val = i;
    return v;
}

pln_value_t *pln_value_new_float(double d) {
    pln_value_t *v = pln_value_new(PLN_FLOAT);
    v->data.float_val = d;
    return v;
}

pln_value_t *pln_value_new_string(const char *s) {
    pln_value_t *v = pln_value_new(PLN_STRING);
    v->data.string_val = pln_strdup(s);
    return v;
}

pln_value_t *pln_value_new_string_len(const char *s, int len) {
    pln_value_t *v = pln_value_new(PLN_STRING);
    v->data.string_val = pln_strdupl(s, len);
    return v;
}

void pln_value_add_to_object(pln_value_t *obj, const char *key, pln_value_t *val) {
    val->key = pln_strdup(key);
    if (!obj->child) { obj->child = val; }
    else {
        pln_value_t *sib = obj->child;
        while (sib->next) sib = sib->next;
        sib->next = val;
    }
}

void pln_value_add_to_object_nocopy(pln_value_t *obj, char *key, pln_value_t *val) {
    val->key = key; /* 接管所有权 */
    if (!obj->child) { obj->child = val; }
    else {
        pln_value_t *sib = obj->child;
        while (sib->next) sib = sib->next;
        sib->next = val;
    }
}

void pln_value_add_to_array(pln_value_t *arr, pln_value_t *val) {
    if (!arr->child) { arr->child = val; }
    else {
        pln_value_t *sib = arr->child;
        while (sib->next) sib = sib->next;
        sib->next = val;
    }
}

void pln_value_free(pln_value_t *v) {
    if (!v) return;
    pln_value_free(v->child);
    pln_value_free(v->next);
    free(v->key);
    if (v->type == PLN_STRING) free(v->data.string_val);
    free(v);
}

/* ─── DOM 序列化 ──────────────────────────────────────────── */

static void pln_write_value(pln_gen_t *g, pln_value_t *v) {
    pln_value_t *c;
    switch (v->type) {
    case PLN_OBJECT:
        pln_gen_begin_object(g);
        for (c = v->child; c; c = c->next) {
            if (c->key) pln_gen_key(g, c->key);
            pln_write_value(g, c);
        }
        pln_gen_end_object(g);
        break;
    case PLN_ARRAY:
        pln_gen_begin_array(g);
        for (c = v->child; c; c = c->next) pln_write_value(g, c);
        pln_gen_end_array(g);
        break;
    case PLN_NULL:   pln_gen_value_null(g); break;
    case PLN_BOOL:   pln_gen_value_bool(g, v->data.bool_val); break;
    case PLN_INT:    pln_gen_value_int(g, v->data.int_val); break;
    case PLN_FLOAT:  pln_gen_value_float(g, v->data.float_val); break;
    case PLN_STRING: pln_gen_value_string(g, v->data.string_val); break;
    }
}

char *pln_dumps(pln_value_t *v) {
    pln_gen_t g;
    pln_gen_init(&g);
    pln_write_value(&g, v);
    const char *s = pln_gen_getvalue(&g);
    char *result = pln_strdup(s);
    pln_gen_free(&g);
    return result;
}


/* ─── 打印 ────────────────────────────────────────────────── */

static void pln_print_internal(pln_value_t *v, int depth) {
    if (!v) { printf("null\n"); return; }
    pln_value_t *c;
    for (int i = 0; i < depth; i++) printf("  ");
    if (v->key) printf("\"%s\": ", v->key);
    switch (v->type) {
    case PLN_OBJECT:
        printf("{\n");
        for (c = v->child; c; c = c->next) pln_print_internal(c, depth + 1);
        for (int i = 0; i < depth; i++) printf("  ");
        printf("}\n");
        break;
    case PLN_ARRAY:
        printf("[\n");
        for (c = v->child; c; c = c->next) pln_print_internal(c, depth + 1);
        for (int i = 0; i < depth; i++) printf("  ");
        printf("]\n");
        break;
    case PLN_NULL:   printf("null\n"); break;
    case PLN_BOOL:   printf("%s\n", v->data.bool_val ? "true" : "false"); break;
    case PLN_INT:    printf("%lld\n", v->data.int_val); break;
    case PLN_FLOAT:  printf("%.17g\n", v->data.float_val); break;
    case PLN_STRING: printf("\"%s\"\n", v->data.string_val); break;
    }
}

void pln_print(pln_value_t *v) { pln_print_internal(v, 0); }
