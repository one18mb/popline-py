/* popline_sax.c — SAX-style PopLine parser, single-pass state machine */

#include "popline_sax.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* ─── Helpers ──────────────────────────────────── */

static void *sax_realloc(void *p, size_t sz) {
    void *r = realloc(p, sz);
    if (!r && sz) { free(p); fprintf(stderr, "OOM\n"); abort(); }
    return r;
}

/* Unescape "" → " in src[0..len-1], write to dst, return written length.
   If dst is NULL, only compute length. */
static int unescape_to(const char *src, int len, char *dst) {
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (src[i] == '"' && i + 1 < len && src[i + 1] == '"') {
            if (dst) dst[j] = '"';
            j++; i++;
        } else {
            if (dst) dst[j] = src[i];
            j++;
        }
    }
    return j;
}

/* Validate " N" pop suffix. Returns pop count, or -1 on invalid. */
static int check_pop(const char *s, int len) {
    if (len <= 0 || s[0] != ' ') return -1;
    int n = 0;
    for (int i = 1; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        n = n * 10 + (s[i] - '0');
    }
    return n;
}

/* ─── SAX context ──────────────────────────────── */

typedef struct {
    pln_sax_cb cb;
    void *user;

    char *stack;         /* 'o' / 'a' per container level */
    int   stack_len, stack_cap;

    char *strbuf;        /* multi-line & unescape buffer */
    int   strbuf_len, strbuf_cap;

    int   in_string;     /* currently inside a multi-line string */
    char  error[256];
} sax_ctx_t;

static void ctx_init(sax_ctx_t *c, pln_sax_cb cb, void *user) {
    memset(c, 0, sizeof(*c));
    c->cb   = cb;
    c->user = user;
    c->stack_cap = 64;
    c->stack     = (char *)malloc(c->stack_cap);
    c->strbuf_cap = 4096;
    c->strbuf    = (char *)malloc(c->strbuf_cap);
}

static void ctx_free(sax_ctx_t *c) {
    free(c->stack);   c->stack   = NULL;
    free(c->strbuf);  c->strbuf  = NULL;
}

/* ─── Stack ─────────────────────────────────────── */

static int stack_push(sax_ctx_t *c, char type) {
    if (c->stack_len >= c->stack_cap) {
        c->stack_cap *= 2;
        c->stack = (char *)sax_realloc(c->stack, c->stack_cap);
    }
    c->stack[c->stack_len++] = type;
    return 0;
}

/* ─── String buffer ─────────────────────────────── */

static void strbuf_grow(sax_ctx_t *c, int extra) {
    while (c->strbuf_len + extra + 1 > c->strbuf_cap) {
        c->strbuf_cap = c->strbuf_cap ? c->strbuf_cap * 2 : 256;
        c->strbuf = (char *)realloc(c->strbuf, c->strbuf_cap);
    }
}

static void strbuf_add(sax_ctx_t *c, const char *s, int n) {
    strbuf_grow(c, n);
    memcpy(c->strbuf + c->strbuf_len, s, n);
    c->strbuf_len += n;
    c->strbuf[c->strbuf_len] = '\0';
}

/* ─── Emit ──────────────────────────────────────── */

static int do_emit(sax_ctx_t *c, const pln_sax_ev_t *ev) {
    return c->cb(ev, c->user);
}

static int emit_pop(sax_ctx_t *c, int n) {
    if (n > c->stack_len) n = c->stack_len;
    for (int i = 0; i < n; i++) {
        char t = c->stack[--c->stack_len];
        pln_sax_ev_t ev = { .type = t == 'o' ? PLN_SAX_OBJ_END : PLN_SAX_ARR_END };
        int r = do_emit(c, &ev);
        if (r) return r;
    }
    return 0;
}

/* ─── Inline container handling ─────────────────── */

static int handle_inline_multi(sax_ctx_t *c, const char *line, int len) {
    const char *p = line;
    while (p < line + len) {
        while (p < line + len && (*p == ' ' || *p == '\t')) p++;
        if (p >= line + len || (*p != '{' && *p != '[')) break;
        pln_sax_ev_t ev = { .type = *p == '{' ? PLN_SAX_OBJ_BEGIN : PLN_SAX_ARR_BEGIN };
        int r = do_emit(c, &ev);
        if (r) return r;
        stack_push(c, *p == '{' ? 'o' : 'a');
        p++;
    }
    return 0;
}

/* ─── Parse scalar value (after stripping pop) ──── */

/* Fast integer parser: no allocation, no libc call */
static int parse_int(const char *s, int len, long long *out) {
    if (len <= 0) return -1;
    long long v = 0, sign = 1;
    int i = 0;
    if (s[0] == '-') { if (len < 2) return -1; sign = -1; i = 1; }
    else if (s[0] == '+') { if (len < 2) return -1; i = 1; }
    if (s[i] < '0' || s[i] > '9') return -1;
    while (i < len) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
        i++;
    }
    *out = v * sign;
    return 0;
}

static int parse_scalar(sax_ctx_t *c, const char *s, int len, int pop) {
    pln_sax_ev_t ev;
    ev.pop = pop;

    /* keywords */
    if (len == 4 && memcmp(s, "true", 4) == 0) {
        ev.type = PLN_SAX_BOOL; ev.bool_val = 1;
        return do_emit(c, &ev);
    }
    if (len == 5 && memcmp(s, "false", 5) == 0) {
        ev.type = PLN_SAX_BOOL; ev.bool_val = 0;
        return do_emit(c, &ev);
    }
    if (len == 4 && memcmp(s, "null", 4) == 0) {
        ev.type = PLN_SAX_NULL;
        return do_emit(c, &ev);
    }

    /* number — try int first, then float */
    if (s[0] == '-' || (s[0] >= '0' && s[0] <= '9')) {
        int is_float = 0;
        for (int i = 0; i < len; i++)
            if (s[i] == '.' || s[i] == 'e' || s[i] == 'E') { is_float = 1; break; }
        if (!is_float) {
            long long ll;
            if (parse_int(s, len, &ll) == 0) {
                ev.type = PLN_SAX_INT; ev.int_val = ll;
                return do_emit(c, &ev);
            }
        } else {
            char tmp[64];
            if (len < (int)sizeof(tmp)) {
                memcpy(tmp, s, len); tmp[len] = '\0';
                char *end; errno = 0;
                double d = strtod(tmp, &end);
                if (end == tmp + len && errno != ERANGE) {
                    ev.type = PLN_SAX_FLOAT; ev.float_val = d;
                    return do_emit(c, &ev);
                }
            }
        }
    }

    snprintf(c->error, sizeof(c->error), "invalid value: '%.*s'", len, s);
    return -2; /* parse error */
}

/* ─── Parse value: string, inline container, or scalar ── */

/* Returns 0=ok, -1=callback aborted, -2=parse error, 1=string-continue.
   pop_out carries the pop suffix count for the caller to update the stack. */
static int parse_value(sax_ctx_t *c, const char *line, int len, int *pop_out) {
    *pop_out = 0;
    if (len <= 0) { snprintf(c->error, sizeof(c->error), "empty value"); return -2; }

    /* ── String ── */
    if (line[0] == '"') {
        /* Single pass: find closing " AND detect "" escapes */
        int i = 1, has_esc = 0;
        while (i < len) {
            if (line[i] == '"') {
                if (i + 2 < len && line[i + 1] == '"') { i += 2; has_esc = 1; continue; }
                break;
            }
            i++;
        }
        if (i >= len) {
            /* multi-line string */
            c->in_string = 1;
            c->strbuf_len = 0;
            if (len > 1) strbuf_add(c, line + 1, len - 1);
            strbuf_add(c, "\n", 1);
            return 1;
        }
        /* single-line string */
        int after = i + 1;
        if (after < len) {
            int np = check_pop(line + after, len - after);
            if (np < 0) { snprintf(c->error, sizeof(c->error), "invalid after closing quote"); return -2; }
            *pop_out = np;
        }
        pln_sax_ev_t ev = { .type = PLN_SAX_STR, .pop = *pop_out };
        if (has_esc) {
            int out = unescape_to(line + 1, i - 1, c->strbuf);
            ev.data = c->strbuf;
            ev.len  = out;
        } else {
            ev.data = line + 1;
            ev.len  = i - 1;
        }
        return do_emit(c, &ev) ? -1 : 0;
    }

    /* ── Pop suffix for non-string values ── */
    int value_end = len;
    if (len >= 2 && line[len - 1] >= '0' && line[len - 1] <= '9') {
        int i = len - 1;
        while (i > 0 && line[i - 1] >= '0' && line[i - 1] <= '9') i--;
        if (i > 0 && line[i - 1] == ' ') {
            int n = 0;
            for (int j = i; j < len; j++) n = n * 10 + (line[j] - '0');
            *pop_out = n;
            value_end = i - 1;
        }
    }

    /* ── Inline container ── */
    if (value_end == 1 && line[0] == '{') {
        pln_sax_ev_t ev = { .type = PLN_SAX_OBJ_BEGIN };
        int r = do_emit(c, &ev);
        if (r) return -1;
        stack_push(c, 'o');
        return 0;
    }
    if (value_end == 1 && line[0] == '[') {
        pln_sax_ev_t ev = { .type = PLN_SAX_ARR_BEGIN };
        int r = do_emit(c, &ev);
        if (r) return -1;
        stack_push(c, 'a');
        return 0;
    }

    /* bare pop line not allowed */
    if (value_end == 0) { snprintf(c->error, sizeof(c->error), "bare pop line not allowed"); return -2; }

    /* ── Scalar (pop carried on the event) ── */
    return parse_scalar(c, line, value_end, *pop_out);
}

/* =================================================================
 *                              MAIN API
 * ================================================================= */

int pln_sax_parse(const char *text, pln_sax_cb cb, void *user_data) {
    sax_ctx_t c;
    ctx_init(&c, cb, user_data);
    const char *pos = text;
    int ret = 0;

    while (*pos) {
        /* ── String continuation (highest priority) ── */
        if (c.in_string) {
            const char *q = strchr(pos, '"');
            if (!q) { ret = -2; break; }  /* unterminated */
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
            /* closing quote found */
            c.in_string = 0;
            const char *after = q + 1;
            const char *nl = strchr(after, '\n');
            int trail = nl ? (int)(nl - after) : (int)strlen(after);
            while (trail > 0 && (after[trail - 1] == ' ' || after[trail - 1] == '\t')) trail--;
            int pop = 0;
            if (trail > 0) {
                pop = check_pop(after, trail);
                if (pop < 0) { ret = -2; break; }
            }
            /* emit accumulated string with pop */
            pln_sax_ev_t ev = { .type = PLN_SAX_STR, .data = c.strbuf, .len = c.strbuf_len, .pop = pop };
            int r = do_emit(&c, &ev);
            if (r) { ret = r; break; }
            c.strbuf_len = 0;
            if (pop > 0) {
                if (pop > c.stack_len) pop = c.stack_len;
                c.stack_len -= pop;
            }
            if (nl) pos = nl + 1;
            continue;
        }

        /* ── Line boundary ── */
        const char *nl = strchr(pos, '\n');
        int line_len = nl ? (int)(nl - pos) : (int)strlen(pos);
        const char *line = pos;
        if (line_len > 0 && line[line_len - 1] == '\r') line_len--;
        pos = nl ? nl + 1 : pos + line_len;

        /* empty line = message separator */
        if (line_len == 0) {
            if (c.stack_len > 0) { ret = -2; break; }
            continue;
        }

        /* ── Root level ── */
        if (c.stack_len == 0) {
            /* inline multi-container: { [ etc. */
            if (line_len > 1 && (line[0] == '{' || line[0] == '[')) {
                int r = handle_inline_multi(&c, line, line_len);
                if (r) { ret = r < 0 ? -2 : r; break; }
                continue;
            }
            if (line_len == 1 && line[0] == '{') {
                pln_sax_ev_t ev = { .type = PLN_SAX_OBJ_BEGIN };
                int r = do_emit(&c, &ev);
                if (r) { ret = r; break; }
                stack_push(&c, 'o');
                continue;
            }
            if (line_len == 1 && line[0] == '[') {
                pln_sax_ev_t ev = { .type = PLN_SAX_ARR_BEGIN };
                int r = do_emit(&c, &ev);
                if (r) { ret = r; break; }
                stack_push(&c, 'a');
                continue;
            }
            /* root scalar */
            int pop = 0;
            int r = parse_value(&c, line, line_len, &pop);
            if (r == -2) { ret = -2; break; }
            if (r == -1) { ret = -1; break; }
            if (pop > 0) {
                if (pop > c.stack_len) pop = c.stack_len;
                c.stack_len -= pop;
            }
            continue;
        }

        /* ── Non-root ── */
        int is_obj = c.stack[c.stack_len - 1] == 'o';

        if (is_obj) {
            /* Object: find ": " separator */
            int sep = -1;
            for (int i = 0; i < line_len - 1; i++) {
                char ch = line[i];
                if (ch == ':' && line[i + 1] == ' ') { sep = i; break; }
                if (ch == ':' || ch == '"' || ch == '{' || ch == '[' || ch == '#' || ch == ' ' || ch == '\t') {
                    ret = -2; goto done;
                }
            }
            if (sep < 0) { ret = -2; goto done; }

            pln_sax_ev_t kev = { .type = PLN_SAX_KEY, .data = line, .len = sep };
            int r = do_emit(&c, &kev);
            if (r) { ret = r; goto done; }

            const char *vp = line + sep + 2;
            int vl = line_len - sep - 2;

            /* Inline multi-container? */
            if (vl > 1 && (vp[0] == '{' || vp[0] == '[')) {
                r = handle_inline_multi(&c, vp, vl);
                if (r) { ret = r < 0 ? -2 : r; goto done; }
            } else if (vl == 1 && vp[0] == '{') {
                pln_sax_ev_t ev = { .type = PLN_SAX_OBJ_BEGIN };
                r = do_emit(&c, &ev);
                if (r) { ret = r; goto done; }
                stack_push(&c, 'o');
            } else if (vl == 1 && vp[0] == '[') {
                pln_sax_ev_t ev = { .type = PLN_SAX_ARR_BEGIN };
                r = do_emit(&c, &ev);
                if (r) { ret = r; goto done; }
                stack_push(&c, 'a');
            } else {
                int pop = 0;
                r = parse_value(&c, vp, vl, &pop);
                if (r == -2) { ret = -2; goto done; }
                if (r == -1) { ret = -1; goto done; }
                if (pop > 0) {
                    if (pop > c.stack_len) pop = c.stack_len;
                    c.stack_len -= pop;
                }
            }
        } else {
            /* Array */
            if (line_len > 1 && (line[0] == '{' || line[0] == '[')) {
                int r = handle_inline_multi(&c, line, line_len);
                if (r) { ret = r < 0 ? -2 : r; goto done; }
            } else if (line_len == 1 && line[0] == '{') {
                pln_sax_ev_t ev = { .type = PLN_SAX_OBJ_BEGIN };
                int r = do_emit(&c, &ev);
                if (r) { ret = r; goto done; }
                stack_push(&c, 'o');
            } else if (line_len == 1 && line[0] == '[') {
                pln_sax_ev_t ev = { .type = PLN_SAX_ARR_BEGIN };
                int r = do_emit(&c, &ev);
                if (r) { ret = r; goto done; }
                stack_push(&c, 'a');
            } else {
                int pop = 0;
                int r = parse_value(&c, line, line_len, &pop);
                if (r == -2) { ret = -2; goto done; }
                if (r == -1) { ret = -1; goto done; }
                if (r == 1) continue; /* string continuation */
                if (pop > 0) {
                    if (pop > c.stack_len) pop = c.stack_len;
                    c.stack_len -= pop;
                }
            }
        }
    }

done:
    if (c.in_string) ret = -2;
    if (ret != -2 && ret != -1) {
        /* flush remaining containers at EOF */
        if (c.stack_len > 0) { int r = emit_pop(&c, c.stack_len); if (r) ret = r; }
        pln_sax_ev_t dev = { .type = PLN_SAX_DONE };
        if (!ret) { int r = do_emit(&c, &dev); if (r) ret = r; }
    }
    ctx_free(&c);
    return ret == -2 ? -1 : ret;
}
