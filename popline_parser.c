/* popline_parser.c — PopLine 解析器：直接 DOM 构建，零拷贝逐行解析 */
#include "popline.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* 从 popline.c 引用的内部函数（非 public API） */
pln_value_t *pln_value_new(pln_value_type_t t);
int pln_unescape_to(const char *s, int len, char *dst);
char *pln_unescape(const char *s, int len);

/* ═══════════════════════════════════════════════════════════════
   逐行解析，直接构建 DOM 树，零事件中转，零拷贝
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    pln_value_t *container;
    pln_value_t **tail;
} fctx_frame_t;

typedef struct {
    pln_value_t *root;
    fctx_frame_t *frames;
    int frames_len, frames_cap;
    char *key;
    int key_len;
    char *strbuf;
    int strbuf_len, strbuf_cap;
    int in_string;
    char error[256];
} fctx_t;

static void fctx_init(fctx_t *f) {
    memset(f, 0, sizeof(*f));
    f->frames_cap = 64;
    f->frames = (fctx_frame_t *)malloc(f->frames_cap * sizeof(fctx_frame_t));
}

static void fctx_free(fctx_t *f) {
    free(f->frames);
    free(f->key);
    free(f->strbuf);
    f->frames = NULL;
}

static int fctx_push(fctx_t *f, pln_value_t *v) {
    if (f->frames_len >= f->frames_cap) {
        f->frames_cap *= 2;
        f->frames = (fctx_frame_t *)realloc(f->frames, f->frames_cap * sizeof(fctx_frame_t));
    }
    f->frames[f->frames_len].container = v;
    f->frames[f->frames_len].tail = v ? &v->child : NULL;
    f->frames_len++;
    return 0;
}

static pln_value_t *fctx_top(fctx_t *f) {
    return f->frames_len > 0 ? f->frames[f->frames_len - 1].container : NULL;
}

static void fctx_add_value(fctx_t *f, pln_value_t *v) {
    if (f->frames_len == 0) {
        f->root = v;
        fctx_push(f, NULL);
    } else {
        fctx_frame_t *frame = &f->frames[f->frames_len - 1];
        v->key = f->key;
        f->key = NULL; f->key_len = 0;
        *frame->tail = v;
        frame->tail = &v->next;
    }
}

static void fctx_pop_layers(fctx_t *f, int n) {
    if (n >= f->frames_len) n = f->frames_len - 1; /* 保留根 */
    if (n < 0) n = 0;
    f->frames_len -= n;
}

static void fsb_ensure(fctx_t *f, int extra) {
    while (f->strbuf_len + extra + 1 > f->strbuf_cap) {
        f->strbuf_cap = f->strbuf_cap ? f->strbuf_cap * 2 : 256;
        f->strbuf = (char *)realloc(f->strbuf, f->strbuf_cap);
    }
}

static void fsb_append(fctx_t *f, const char *s, int n) {
    fsb_ensure(f, n);
    memcpy(f->strbuf + f->strbuf_len, s, n);
    f->strbuf_len += n;
    f->strbuf[f->strbuf_len] = '\0';
}

/* ─── 快速值解析（零拷贝，直接返回 pln_value_t*） ───── */

static inline pln_value_t *fparse_string_body(fctx_t *f, const char *s, int len) {
    int i = 0;
    int has_esc = 0;
    while (1) {
        if (__builtin_expect(i >= len, 0)) {
            f->in_string = 1;
            f->strbuf_len = 0;
            fsb_append(f, s, len);
            fsb_append(f, "\n", 1);
            return NULL;
        }
        if (s[i] == '"') {
            if (i + 1 < len && s[i + 1] == '"') { has_esc = 1; i += 2; continue; }
            break;
        }
        i++;
    }
    if (__builtin_expect(!has_esc, 1)) {
        return pln_value_new_string_len(s, i);
    }
    int out_len = pln_unescape_to(s, i, NULL);
    pln_value_t *v = pln_value_new(PLN_STRING);
    v->data.string_val = (char *)malloc(out_len + 1);
    pln_unescape_to(s, i, v->data.string_val);
    v->data.string_val[out_len] = '\0';
    return v;
}

__attribute__((always_inline))
static inline pln_value_t *fparse_value(fctx_t *f, const char *s, int len) {
    if (len <= 0) return NULL;

    switch (s[0]) {
    case '"':
        return fparse_string_body(f, s + 1, len - 1);
    case 't':
        if (len == 4 && memcmp(s, "true", 4) == 0) return pln_value_new_bool(1);
        goto invalid;
    case 'f':
        if (len == 5 && memcmp(s, "false", 5) == 0) return pln_value_new_bool(0);
        goto invalid;
    case 'n':
        if (len == 4 && memcmp(s, "null", 4) == 0) return pln_value_new_null();
        goto invalid;
    case '{':
        if (len == 1) return pln_value_new_object();
        goto invalid;
    case '[':
        if (len == 1) return pln_value_new_array();
        goto invalid;
    default:
        break;
    }

    if (s[0] == '-' || (s[0] >= '0' && s[0] <= '9')) {
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
            if (end == tmp + len && errno != ERANGE) return pln_value_new_float(d);
        } else {
            long long ll = strtoll(tmp, &end, 10);
            if (end == tmp + len && errno != ERANGE) return pln_value_new_int(ll);
        }
    }

invalid:
    snprintf(f->error, sizeof(f->error), "字符串必须用双引号包裹: '%.*s'", len, s);
    return NULL;
}

/* Forward declarations for pop suffix helpers */
static inline int trim_pop_suffix(const char *s, int *content_len);
static inline int pop_suffix_after(const char *s, int len);

static inline int fhandle_string_line(fctx_t *f, const char *line, int len) {
    int i = 0;
    while (1) {
        if (i >= len) {
            fsb_append(f, line, len);
            fsb_append(f, "\n", 1);
            return 0;
        }
        if (line[i] == '"') {
            if (i + 1 < len && line[i + 1] == '"') { i += 2; continue; }
            break;
        }
        i++;
    }
    fsb_append(f, line, i);

    /* 检查闭合引号后内容：空=0，" N"=弹出数，其他=-1 */
    int after = i + 1;
    if (after < len) {
        int n_pop = pop_suffix_after(line + after, len - after);
        if (n_pop < 0) {
            snprintf(f->error, sizeof(f->error),
                     "字符串闭合引号后非法内容: '%.*s'", len - after, line + after);
            return -1;
        }
        char *unesc = pln_unescape(f->strbuf, f->strbuf_len);
        pln_value_t *v = pln_value_new_string(unesc);
        free(unesc);
        fctx_add_value(f, v);
        if (n_pop > 0) fctx_pop_layers(f, n_pop);
        f->in_string = 0;
        f->strbuf_len = 0;
        return 1;
    }

    char *unesc = pln_unescape(f->strbuf, f->strbuf_len);
    pln_value_t *v = pln_value_new_string(unesc);
    free(unesc);
    fctx_add_value(f, v);
    f->in_string = 0;
    f->strbuf_len = 0;
    return 1;
}

/* ─── 连缀容器解析 ──────────────────────────────── */

__attribute__((always_inline))
static inline int fparse_inline_containers(fctx_t *f, const char *s, int len) {
    while (len > 0) {
        while (len > 0 && (*s == ' ' || *s == '\t')) { s++; len--; }
        if (len <= 0) break;
        if (*s != '{' && *s != '[') {
            snprintf(f->error, sizeof(f->error), "连缀容器只能包含 '{' 或 '['");
            return -1;
        }
        pln_value_t *v = (*s == '{') ? pln_value_new_object() : pln_value_new_array();
        if (f->frames_len == 0) {
            f->root = v;
            fctx_push(f, v);
        } else {
            fctx_frame_t *frame = &f->frames[f->frames_len - 1];
            v->key = f->key; f->key = NULL; f->key_len = 0;
            *frame->tail = v;
            frame->tail = &v->next;
            fctx_push(f, v);
        }
        s++; len--;
    }
    return 0;
}

/* ─── 行末弹出后缀检测 ────────────────────────── */

/* 从内容末尾提取弹出后缀 ` N`，返回弹出数，同时减小 *content_len */
__attribute__((always_inline))
static inline int trim_pop_suffix(const char *s, int *content_len) {
    int clen = *content_len;
    if (clen < 2) return 0;
    int i = clen - 1;
    if (s[i] < '0' || s[i] > '9') return 0;
    while (i > 0 && s[i-1] >= '0' && s[i-1] <= '9') i--;
    if (i == 0 || s[i-1] != ' ') return 0;
    int n = 0;
    for (int j = i; j < clen; j++) n = n * 10 + (s[j] - '0');
    *content_len = i - 1;
    return n;
}

/* 验证字符串闭合引号后内容：空=0，有效" N"=N，无效=-1 */
__attribute__((always_inline))
static inline int pop_suffix_after(const char *s, int len) {
    if (len <= 0) return 0;
    if (s[0] != ' ') return -1;
    if (len < 2 || s[1] < '0' || s[1] > '9') return -1;
    int n = 0;
    for (int i = 1; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        n = n * 10 + (s[i] - '0');
    }
    return n;
}

/* ─── 逐行解析（行末弹出后缀版） ──────────────── */

__attribute__((always_inline))
static inline int fparse_line(fctx_t *f, const char *line, int len) {
    if (len > 0 && line[len - 1] == '\r') len--;

    if (f->in_string) return fhandle_string_line(f, line, len);

    if (len == 0) return 0;

    /* 兼容行首弹出前缀（用于容器开标识或键名行前，如：1 [、1 key: "val"） */
    int n_pop = 0;
    int value_start = 0;
    if (line[0] >= '0' && line[0] <= '9') {
        int pop_end = 1;
        while (pop_end < len && line[pop_end] >= '0' && line[pop_end] <= '9') pop_end++;
        if (pop_end < len && line[pop_end] == ' ') {
            int after = pop_end + 1;
            while (after < len && line[after] == ' ') after++;
            if (after < len) {
                char nc = line[after];
                /* 前缀仅当后跟容器开标识或键值行时生效 */
                if (nc == '{' || nc == '[' ||
                    memchr(line + after, ':', len - after) != NULL) {
                    n_pop = 0;
                    for (int j = 0; j < pop_end; j++) n_pop = n_pop * 10 + (line[j] - '0');
                    value_start = pop_end + 1;
                }
            }
        }
    }
    if (n_pop > 0) {
        fctx_pop_layers(f, n_pop);
        line += value_start;
        len -= value_start;
    }

    const char *rest = line;
    int rest_len = len;

    /* 顶层：必须为 {、[ 或连缀 */
    if (f->frames_len == 0 || (f->frames_len == 1 && f->frames[0].container == NULL)) {
        if (rest_len > 1 && rest[0] == '[') {
            const char *rp = rest + 1;
            int rl = rest_len - 1;
            while (rl > 0 && (*rp == ' ' || *rp == '\t')) { rp++; rl--; }
            if (rl > 0 && (*rp == '[' || *rp == '{'))
                return fparse_inline_containers(f, rest, rest_len);
        }
        pln_value_t *v = fparse_value(f, rest, rest_len);
        if (!v) return f->error[0] ? -1 : 0;
        if (v->type != PLN_OBJECT && v->type != PLN_ARRAY) {
            snprintf(f->error, sizeof(f->error), "顶层必须是对象或数组");
            pln_value_free(v);
            return -1;
        }
        f->root = v;
        fctx_push(f, v);
        return 0;
    }

    pln_value_t *top = fctx_top(f);

    if (top->type == PLN_OBJECT) {
        int key_sep = -1;
        for (int i = 0; i < rest_len - 1; i++) {
            char c = rest[i];
            if (c == ':') {
                if (rest[i + 1] == ' ') { key_sep = i; break; }
                snprintf(f->error, sizeof(f->error), "非法键名: '%.*s'", rest_len, rest);
                return -1;
            }
            if (c == '"' || c == '{' || c == '[' ||
                c == '#' || c == ' ' || c == '\t') {
                snprintf(f->error, sizeof(f->error), "非法键名: '%.*s'", rest_len, rest);
                return -1;
            }
        }
        if (key_sep < 0) {
            snprintf(f->error, sizeof(f->error), "对象内行必须 'key: value': '%.*s'", rest_len, rest);
            return -1;
        }
        int klen = key_sep;
        free(f->key);
        f->key = (char *)malloc(klen + 1);
        memcpy(f->key, rest, klen); f->key[klen] = '\0';
        f->key_len = klen;

        const char *vpart = rest + klen + 2;
        int vlen = rest_len - klen - 2;
        if (vlen <= 0) return 0;

        /* 提取行末弹出后缀（仅叶值，容器开标识不处理） */
        int n_pop = 0;
        if (vpart[0] != '{' && vpart[0] != '[')
            n_pop = trim_pop_suffix(vpart, &vlen);
        if (vlen <= 0) return 0;

        /* 检查值连缀容器 */
        if (vlen > 1 && (vpart[0] == '[' || vpart[0] == '{')) {
            const char *rp = vpart + 1;
            int rl = vlen - 1;
            while (rl > 0 && (*rp == ' ' || *rp == '\t')) { rp++; rl--; }
            if (rl > 0 && (*rp == '[' || *rp == '{'))
                return fparse_inline_containers(f, vpart, vlen);
        }

        pln_value_t *v = fparse_value(f, vpart, vlen);
        if (!v) return f->error[0] ? -1 : 0;
        fctx_add_value(f, v);
        if (v->type == PLN_OBJECT || v->type == PLN_ARRAY) fctx_push(f, v);
        if (n_pop > 0) fctx_pop_layers(f, n_pop);
        return 0;
    }

    if (top->type == PLN_ARRAY) {
        /* 提取行末弹出后缀（仅叶值） */
        int n_pop = 0;
        if (rest_len > 0 && rest[0] != '{' && rest[0] != '[')
            n_pop = trim_pop_suffix(rest, &rest_len);
        if (rest_len <= 0) return 0;

        /* 检查数组元素连缀容器 */
        if (rest_len > 1 && (rest[0] == '[' || rest[0] == '{')) {
            const char *rp = rest + 1;
            int rl = rest_len - 1;
            while (rl > 0 && (*rp == ' ' || *rp == '\t')) { rp++; rl--; }
            if (rl > 0 && (*rp == '[' || *rp == '{'))
                return fparse_inline_containers(f, rest, rest_len);
        }
        pln_value_t *v = fparse_value(f, rest, rest_len);
        if (!v) return f->error[0] ? -1 : 0;
        fctx_add_value(f, v);
        if (v->type == PLN_OBJECT || v->type == PLN_ARRAY) fctx_push(f, v);
        if (n_pop > 0) fctx_pop_layers(f, n_pop);
        return 0;
    }

    snprintf(f->error, sizeof(f->error), "内部错误: 栈顶类型未知");
    return -1;
}

pln_value_t *pln_loads(const char *text) {
    fctx_t f;
    fctx_init(&f);

    const char *s = text;
    const char *line_start = s;

    for (;;) {
        const char *nl = strchr(s, '\n');
        if (nl) {
            int r = fparse_line(&f, line_start, (int)(nl - line_start));
            if (r < 0) { if (f.root) pln_value_free(f.root); f.root = NULL; break; }
            s = nl + 1;
            line_start = s;
        } else {
            if (*line_start) {
                int r = fparse_line(&f, line_start, (int)strlen(line_start));
                if (r < 0) { if (f.root) pln_value_free(f.root); f.root = NULL; }
            }
            break;
        }
    }

    fctx_free(&f);
    return f.root;
}
