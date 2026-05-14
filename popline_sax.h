/* popline_sax.h — SAX-style PopLine parser */

#ifndef POPLINE_SAX_H
#define POPLINE_SAX_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLN_SAX_OBJ_BEGIN,    /* { */
    PLN_SAX_OBJ_END,      /* } via pop suffix */
    PLN_SAX_ARR_BEGIN,    /* [ */
    PLN_SAX_ARR_END,      /* ] via pop suffix */
    PLN_SAX_KEY,          /* key name, data/len */
    PLN_SAX_STR,          /* string value, data/len */
    PLN_SAX_INT,          /* integer value, int_val */
    PLN_SAX_FLOAT,        /* float value, float_val */
    PLN_SAX_BOOL,         /* boolean value, bool_val */
    PLN_SAX_NULL,         /* null */
    PLN_SAX_DONE,         /* parse complete */
} pln_sax_event_t;

typedef struct {
    pln_sax_event_t type;
    const char *data;       /* string data for KEY/STR, valid during callback */
    int         len;        /* byte length for KEY/STR */
    long long   int_val;    /* for INT */
    double      float_val;  /* for FLOAT */
    int         bool_val;   /* for BOOL */
    int         pop;        /* N from pop suffix (attached to value events) */
} pln_sax_ev_t;

/* Return 0 to continue, non-zero to abort. pln_sax_parse returns the
   callback's return value, or -1 on internal parse error. */
typedef int (*pln_sax_cb)(const pln_sax_ev_t *ev, void *user_data);

/* Parse PopLine text in one pass. Calls cb for each event.
   Returns 0 on success, -1 on parse error, or callback's non-zero return. */
int pln_sax_parse(const char *text, pln_sax_cb cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* POPLINE_SAX_H */
