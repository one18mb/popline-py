/* popline_json.c — PopLine ↔ JSON bidirectional conversion */
#include "popline.h"
#include <cjson/cJSON.h>

static cJSON *pln_to_cjson(pln_value_t *v) {
    if (!v) return cJSON_CreateNull();
    switch (v->type) {
    case PLN_NULL:   return cJSON_CreateNull();
    case PLN_BOOL:   return cJSON_CreateBool(v->data.bool_val);
    case PLN_INT:    return cJSON_CreateNumber((double)v->data.int_val);
    case PLN_FLOAT:  return cJSON_CreateNumber(v->data.float_val);
    case PLN_STRING: return cJSON_CreateString(v->data.string_val);
    case PLN_OBJECT: {
        cJSON *obj = cJSON_CreateObject();
        for (pln_value_t *c = v->child; c; c = c->next)
            cJSON_AddItemToObject(obj, c->key ? c->key : "", pln_to_cjson(c));
        return obj;
    }
    case PLN_ARRAY: {
        cJSON *arr = cJSON_CreateArray();
        for (pln_value_t *c = v->child; c; c = c->next)
            cJSON_AddItemToArray(arr, pln_to_cjson(c));
        return arr;
    }
    }
    return cJSON_CreateNull();
}

static pln_value_t *cjson_to_pl(cJSON *c) {
    if (!c) return NULL;
    if (cJSON_IsNull(c))   return pln_value_new_null();
    if (cJSON_IsFalse(c))  return pln_value_new_bool(0);
    if (cJSON_IsTrue(c))   return pln_value_new_bool(1);
    if (cJSON_IsNumber(c)) {
        double d = c->valuedouble;
        long long ll = (long long)d;
        if ((double)ll == d)
            return pln_value_new_int(ll);
        return pln_value_new_float(d);
    }
    if (cJSON_IsString(c)) return pln_value_new_string(c->valuestring);
    if (cJSON_IsObject(c)) {
        pln_value_t *v = pln_value_new_object();
        cJSON *item;
        cJSON_ArrayForEach(item, c) {
            if (item->string)
                pln_value_add_to_object(v, item->string, cjson_to_pl(item));
        }
        return v;
    }
    if (cJSON_IsArray(c)) {
        pln_value_t *v = pln_value_new_array();
        cJSON *item;
        cJSON_ArrayForEach(item, c)
            pln_value_add_to_array(v, cjson_to_pl(item));
        return v;
    }
    return pln_value_new_null();
}

pln_value_t *pln_loads_json(const char *json) {
    cJSON *root = cJSON_Parse(json);
    if (!root) return NULL;
    pln_value_t *v = cjson_to_pl(root);
    cJSON_Delete(root);
    return v;
}

char *pln_dumps_json(pln_value_t *v) {
    if (!v) return NULL;
    cJSON *root = pln_to_cjson(v);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}
