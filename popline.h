/* popline.h — PopLine v2 C reference implementation */

#ifndef POPLINE_H
#define POPLINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── 值类型 ─────────────────────────────────────────────── */

typedef enum {
    PLN_NULL,
    PLN_BOOL,
    PLN_INT,
    PLN_FLOAT,
    PLN_STRING,
    PLN_OBJECT,
    PLN_ARRAY
} pln_value_type_t;

/* ─── DOM 值 ───────────────────────────────────────────── */

typedef struct pln_value_t {
    pln_value_type_t type;
    union {
        int bool_val;       /* 0 or 1 */
        long long int_val;
        double float_val;
        char *string_val;
    } data;
    /* for object/array */
    struct pln_value_t *child;   /* first child / first element */
    struct pln_value_t *next;    /* next sibling */
    char *key;                  /* key name if part of object */
} pln_value_t;

/* ─── 生成器 ───────────────────────────────────────────── */

typedef struct {
    char *buf;
    int   len;
    int   cap;
    char *stack;
    int   stack_len;
    int   stack_cap;
    int   pending_pop;
    int   awaiting_value;
    int   has_leaf_value;
} pln_gen_t;

void pln_gen_init(pln_gen_t *g);
void pln_gen_begin_object(pln_gen_t *g);
void pln_gen_end_object(pln_gen_t *g);
void pln_gen_begin_array(pln_gen_t *g);
void pln_gen_end_array(pln_gen_t *g);
void pln_gen_key(pln_gen_t *g, const char *k);
void pln_gen_value_null(pln_gen_t *g);
void pln_gen_value_bool(pln_gen_t *g, int v);
void pln_gen_value_int(pln_gen_t *g, long long v);
void pln_gen_value_float(pln_gen_t *g, double v);
void pln_gen_value_string(pln_gen_t *g, const char *v);
void pln_gen_flush(pln_gen_t *g);
const char *pln_gen_getvalue(pln_gen_t *g);
void pln_gen_free(pln_gen_t *g);

/* ─── DOM 构建 ─────────────────────────────────────────── */

pln_value_t *pln_value_new_object(void);
pln_value_t *pln_value_new_array(void);
pln_value_t *pln_value_new_string(const char *s);
pln_value_t *pln_value_new_string_len(const char *s, int len);
pln_value_t *pln_value_new_int(long long v);
pln_value_t *pln_value_new_float(double v);
pln_value_t *pln_value_new_bool(int v);
pln_value_t *pln_value_new_null(void);
void pln_value_add_to_object(pln_value_t *obj, const char *key, pln_value_t *val);
void pln_value_add_to_object_nocopy(pln_value_t *obj, char *key, pln_value_t *val); /* 接管 key 所有权 */
void pln_value_add_to_array(pln_value_t *arr, pln_value_t *val);
void pln_value_free(pln_value_t *v);

/* ─── 解析 / 序列化 ──────────────────────────────────── */

pln_value_t *pln_loads(const char *text);      /* 解析 PopLine 文本 → DOM 树 */
char       *pln_dumps(pln_value_t *v);         /* DOM 树 → PopLine 文本 */

/* ─── 辅助 ─────────────────────────────────────────────── */

void pln_print(pln_value_t *v);

#ifdef __cplusplus
}
#endif

#endif /* POPLINE_H */
