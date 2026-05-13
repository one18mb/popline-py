/* popline_parser.c — PopLine single-pass state machine parser */
#include "popline.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* Internal declarations from popline.c */
pln_value_t *pln_value_new(pln_value_type_t t);
int pln_unescape_to(const char *s, int len, char *dst);
char *pln_unescape(const char *s, int len);

/* ─── Frame stack ────────────────────────────────────────── */

typedef struct {
    pln_value_t *container;
    pln_value_t **tail;
} frame_t;

typedef struct {
    pln_value_t *root;
    frame_t *frames;
    int frame_count, frame_cap;
    char *key;
    char *strbuf;
    int strbuf_len, strbuf_cap;
    int in_string;
    char error[256];
} parse_ctx_t;

static void ctx_init(parse_ctx_t *c) {
    memset(c, 0, sizeof(*c));
    c->frame_cap = 64;
    c->frames = (frame_t *)malloc(c->frame_cap * sizeof(frame_t));
}

static void ctx_free(parse_ctx_t *c) {
    free(c->frames);
    free(c->key);
    free(c->strbuf);
    c->frames = NULL;
}

static void frame_push(parse_ctx_t *c, pln_value_t *v) {
    if (c->frame_count >= c->frame_cap) {
        c->frame_cap *= 2;
        c->frames = (frame_t *)realloc(c->frames, c->frame_cap * sizeof(frame_t));
    }
    c->frames[c->frame_count].container = v;
    c->frames[c->frame_count].tail = v ? &v->child : NULL;
    c->frame_count++;
}

static void frame_add(parse_ctx_t *c, pln_value_t *v) {
    frame_t *fr = &c->frames[c->frame_count - 1];
    v->key = c->key;
    c->key = NULL;
    *fr->tail = v;
    fr->tail = &v->next;
}

static void frame_pop_n(parse_ctx_t *c, int n) {
    if (n > c->frame_count) n = c->frame_count;
    c->frame_count -= n;
}

/* ─── String buffer helpers ──────────────────────────────── */

static void strbuf_grow(parse_ctx_t *c, int extra) {
    while (c->strbuf_len + extra + 1 > c->strbuf_cap) {
        c->strbuf_cap = c->strbuf_cap ? c->strbuf_cap * 2 : 256;
        c->strbuf = (char *)realloc(c->strbuf, c->strbuf_cap);
    }
}

static void strbuf_add(parse_ctx_t *c, const char *s, int n) {
    strbuf_grow(c, n);
    memcpy(c->strbuf + c->strbuf_len, s, n);
    c->strbuf_len += n;
    c->strbuf[c->strbuf_len] = '\0';
}

/* ─── Validate " N" suffix after closing quote ──────────── */

static int check_pop_suffix(const char *s, int len) {
    if (len <= 0 || s[0] != ' ') return -1;
    int n = 0;
    for (int i = 1; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        n = n * 10 + (s[i] - '0');
    }
    return n;
}

/* ─── Single-pass value parser: type + pop suffix + number ── */

static pln_value_t *parse_value(parse_ctx_t *c, const char *s, int len, int *pop_out) {
    *pop_out = 0;
    if (len <= 0) { snprintf(c->error, sizeof(c->error), "empty value"); return NULL; }

    /* String */
    if (s[0] == '"') {
        int i = 1;
        while (i < len) {
            if (s[i] == '"') {
                if (i + 1 < len && s[i + 1] == '"') { i += 2; continue; }
                break;
            }
            i++;
        }
        if (i >= len) {
            c->in_string = 1;
            c->strbuf_len = 0;
            strbuf_add(c, s + 1, len - 1);
            strbuf_add(c, "\n", 1);
            return NULL;
        }
        int after = i + 1;
        if (after < len) {
            int np = check_pop_suffix(s + after, len - after);
            if (np < 0) { snprintf(c->error, sizeof(c->error), "invalid after closing quote"); return NULL; }
            *pop_out = np;
        }
        int has_esc = 0;
        for (int j = 1; j < i; j++) {
            if (s[j] == '"' && j + 1 < i && s[j + 1] == '"') { has_esc = 1; break; }
        }
        if (!has_esc) return pln_value_new_string_len(s + 1, i - 1);
        int out_len = pln_unescape_to(s + 1, i - 1, NULL);
        pln_value_t *v = pln_value_new(PLN_STRING);
        v->data.string_val = (char *)malloc(out_len + 1);
        pln_unescape_to(s + 1, i - 1, v->data.string_val);
        v->data.string_val[out_len] = '\0';
        return v;
    }

    /* Pop suffix: check from end (O(1) for non-digit tail) */
    int value_end = len;
    if (len >= 2 && s[len - 1] >= '0' && s[len - 1] <= '9') {
        int i = len - 1;
        while (i > 0 && s[i - 1] >= '0' && s[i - 1] <= '9') i--;
        if (i > 0 && s[i - 1] == ' ') {
            int n = 0;
            for (int j = i; j < len; j++) n = n * 10 + (s[j] - '0');
            *pop_out = n;
            value_end = i - 1;
        }
    }

    /* Keywords */
    if (value_end == 4 && memcmp(s, "true", 4) == 0) return pln_value_new_bool(1);
    if (value_end == 5 && memcmp(s, "false", 5) == 0) return pln_value_new_bool(0);
    if (value_end == 4 && memcmp(s, "null", 4) == 0) return pln_value_new_null();

    /* Number */
    if (s[0] == '-' || (s[0] >= '0' && s[0] <= '9')) {
        char tmp[64];
        if (value_end >= (int)sizeof(tmp)) goto invalid;
        memcpy(tmp, s, value_end);
        tmp[value_end] = '\0';
        int is_float = 0;
        for (int i = 0; i < value_end; i++) {
            if (tmp[i] == '.' || tmp[i] == 'e' || tmp[i] == 'E') { is_float = 1; break; }
        }
        char *end;
        errno = 0;
        if (is_float) {
            double d = strtod(tmp, &end);
            if (end == tmp + value_end && errno != ERANGE) return pln_value_new_float(d);
        } else {
            long long ll = strtoll(tmp, &end, 10);
            if (end == tmp + value_end && errno != ERANGE) return pln_value_new_int(ll);
        }
    }

invalid:
    snprintf(c->error, sizeof(c->error), "invalid value: '%.*s'", value_end, s);
    return NULL;
}

/* ─── Main parse loop (single-pass state machine) ────────── */

pln_value_t *pln_loads(const char *text) {
    parse_ctx_t c;
    ctx_init(&c);
    const char *pos = text;

    while (*pos) {
        /* ── String continuation (highest priority) ── */
        if (c.in_string) {
            const char *q = strchr(pos, '"');
            if (!q) break;
            if (q > pos) {
                strbuf_grow(&c, (int)(q - pos));
                memcpy(c.strbuf + c.strbuf_len, pos, q - pos);
                c.strbuf_len += (int)(q - pos);
                c.strbuf[c.strbuf_len] = '\0';
            }
            if (q[1] == '"') {
                c.strbuf[c.strbuf_len++] = '"';
                c.strbuf[c.strbuf_len] = '\0';
                pos = q + 2;
                continue;
            }
            c.in_string = 0;
            const char *after_quote = q + 1;
            pos = after_quote + 1;
            const char *nl = strchr(after_quote, '\n');
            int trail_len = nl ? (int)(nl - after_quote) : (int)strlen(after_quote);
            /* Strip trailing whitespace before checking pop suffix */
            while (trail_len > 0 && (after_quote[trail_len - 1] == ' ' || after_quote[trail_len - 1] == '\t'))
                trail_len--;
            int pop = 0;
            if (trail_len > 0) {
                pop = check_pop_suffix(after_quote, trail_len);
                if (pop < 0) { ctx_free(&c); return NULL; }
            }
            char *unesc = pln_unescape(c.strbuf, c.strbuf_len);
            pln_value_t *sv = pln_value_new_string(unesc);
            free(unesc);
            c.strbuf_len = 0;
            frame_add(&c, sv);
            frame_pop_n(&c, pop);
            if (nl) pos = nl + 1;
            continue;
        }

        /* ── Find line boundary ── */
        const char *nl = strchr(pos, '\n');
        int line_len = nl ? (int)(nl - pos) : (int)strlen(pos);
        const char *line = pos;
        if (line_len > 0 && line[line_len - 1] == '\r') line_len--;
        pos = nl ? nl + 1 : pos + line_len;

        /* ── Empty line handling ── */
        if (line_len == 0) {
            if (c.frame_count > 0) { ctx_free(&c); return NULL; }
            continue;
        }

        /* ── Root level ── */
        if (c.frame_count == 0) {
            if (line_len > 1 && line[0] == '[') {
                const char *t = line + 1;
                while (t < line + line_len && (*t == ' ' || *t == '\t')) t++;
                if (t < line + line_len && (t[0] == '[' || t[0] == '{')) {
                    const char *x = line;
                    while (x < line + line_len) {
                        while (x < line + line_len && (*x == ' ' || *x == '\t')) x++;
                        if (x >= line + line_len || (*x != '{' && *x != '[')) break;
                        pln_value_t *cv = *x == '{' ? pln_value_new_object() : pln_value_new_array();
                        c.root = cv;
                        frame_push(&c, cv);
                        x++;
                    }
                    continue;
                }
            }
            if (line_len == 1 && line[0] == '{') {
                c.root = pln_value_new_object();
                frame_push(&c, c.root);
                continue;
            }
            if (line_len == 1 && line[0] == '[') {
                c.root = pln_value_new_array();
                frame_push(&c, c.root);
                continue;
            }
            int pop = 0;
            pln_value_t *sv = parse_value(&c, line, line_len, &pop);
            if (!sv) { ctx_free(&c); return NULL; }
            c.root = sv;
            ctx_free(&c);
            return sv;
        }

        /* ── Non-root: object or array ── */
        int is_obj = c.frames[c.frame_count - 1].container->type == PLN_OBJECT;

        if (is_obj) {
            /* Find ": " separator and validate key in one pass */
            int sep = -1;
            for (int i = 0; i < line_len - 1; i++) {
                char ch = line[i];
                if (ch == ':' && line[i + 1] == ' ') { sep = i; break; }
                if (ch == ':' || ch == '"' || ch == '{' || ch == '[' || ch == '#' || ch == ' ' || ch == '\t') {
                    ctx_free(&c); return NULL;
                }
            }
            if (sep < 0) { ctx_free(&c); return NULL; }
            free(c.key);
            c.key = (char *)malloc(sep + 1);
            memcpy(c.key, line, sep);
            c.key[sep] = '\0';

            const char *val_part = line + sep + 2;
            int val_len = line_len - sep - 2;

            if (val_len == 1 && val_part[0] == '{') {
                pln_value_t *obj = pln_value_new_object();
                frame_add(&c, obj);
                frame_push(&c, obj);
            } else if (val_len == 1 && val_part[0] == '[') {
                pln_value_t *arr = pln_value_new_array();
                frame_add(&c, arr);
                frame_push(&c, arr);
            } else {
                int pop = 0;
                pln_value_t *vv = parse_value(&c, val_part, val_len, &pop);
                if (!vv && !c.in_string) { ctx_free(&c); return NULL; }
                if (vv) { frame_add(&c, vv); frame_pop_n(&c, pop); }
            }
        } else {
            /* Array: inline containers, bare {/[ or scalar */
            if (line_len > 1) {
                char b0 = line[0];
                if (b0 == '[' || b0 == '{') {
                    const char *t = line + 1;
                    while (t < line + line_len && (*t == ' ' || *t == '\t')) t++;
                    if (t < line + line_len && (t[0] == '[' || t[0] == '{')) {
                        const char *x = line;
                        while (x < line + line_len) {
                            while (x < line + line_len && (*x == ' ' || *x == '\t')) x++;
                            if (x >= line + line_len || (*x != '{' && *x != '[')) break;
                            pln_value_t *cv = *x == '{' ? pln_value_new_object() : pln_value_new_array();
                            frame_add(&c, cv);
                            frame_push(&c, cv);
                            x++;
                        }
                        continue;
                    }
                }
            }
            if (line_len == 1 && line[0] == '{') {
                pln_value_t *ov = pln_value_new_object();
                frame_add(&c, ov);
                frame_push(&c, ov);
            } else if (line_len == 1 && line[0] == '[') {
                pln_value_t *av = pln_value_new_array();
                frame_add(&c, av);
                frame_push(&c, av);
            } else {
                int pop = 0;
                pln_value_t *vv = parse_value(&c, line, line_len, &pop);
                if (!vv && !c.in_string) { ctx_free(&c); return NULL; }
                if (vv) { frame_add(&c, vv); frame_pop_n(&c, pop); }
            }
        }
    }

    if (c.in_string) { ctx_free(&c); return NULL; }
    pln_value_t *result = c.root;
    ctx_free(&c);
    return result;
}
