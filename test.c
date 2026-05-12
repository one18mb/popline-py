/* test.c — PopLine 完整测试套件：单元测试 + JSON 对比 + 性能基准 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "popline.h"
#include <cjson/cJSON.h>

/* ═══════════════════════════════════════════════════════════════
   辅助函数
   ═══════════════════════════════════════════════════════════════ */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

static char *read_file(const char *path, int *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(len + 1);
    if (fread(buf, 1, len, f) != len) {}
    fclose(f);
    buf[len] = '\0';
    *out_len = (int)len;
    return buf;
}

static int dom_equal(pln_value_t *a, pln_value_t *b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    if (a->type != b->type) return 0;
    switch (a->type) {
    case PLN_NULL:  return 1;
    case PLN_BOOL:  return a->data.bool_val == b->data.bool_val;
    case PLN_INT:   return a->data.int_val == b->data.int_val;
    case PLN_FLOAT: return a->data.float_val == b->data.float_val;
    case PLN_STRING:return strcmp(a->data.string_val, b->data.string_val) == 0;
    case PLN_OBJECT:
    case PLN_ARRAY: {
        pln_value_t *ca = a->child, *cb = b->child;
        while (ca && cb) {
            if ((ca->key && !cb->key) || (!ca->key && cb->key)) return 0;
            if (ca->key && cb->key && strcmp(ca->key, cb->key) != 0) return 0;
            if (!dom_equal(ca, cb)) return 0;
            ca = ca->next; cb = cb->next;
        }
        return ca == NULL && cb == NULL;
    }}
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   单元测试框架
   ═══════════════════════════════════════════════════════════════ */

static int total = 0, passed = 0;

#define TEST(name) do { total++; } while(0)
#define PASS(name) do { passed++; } while(0)
#define FAIL(name, msg) do { printf("  FAIL [%s]: %s\n", name, msg); } while(0)
#define CHECK(cond, name, msg) do { TEST(name); if (cond) PASS(name); else FAIL(name, msg); } while(0)

/* ═══════════════════════════════════════════════════════════════
   单元测试
   ═══════════════════════════════════════════════════════════════ */

static void unit_basic_types(void) {
    printf("── 基本类型 ──\n");
    pln_value_t *v;

    v = pln_loads("{\nname: \"popline\"\n");
    CHECK(v && v->type == PLN_OBJECT, "简单对象", "parse failed");
    CHECK(v->child && strcmp(v->child->key, "name") == 0, "键名", "mismatch");
    CHECK(v->child->type == PLN_STRING, "字符串值", "not string");
    CHECK(strcmp(v->child->data.string_val, "popline") == 0, "字符串内容", "mismatch");
    pln_value_free(v);

    v = pln_loads("{\na: 42\n");
    CHECK(v && v->child->type == PLN_INT && v->child->data.int_val == 42, "整数", "mismatch");
    pln_value_free(v);

    v = pln_loads("{\na: 3.14159\n");
    CHECK(v && v->child->type == PLN_FLOAT, "浮点数", "not float");
    pln_value_free(v);

    v = pln_loads("{\na: 6.022e23\n");
    CHECK(v && v->child->type == PLN_FLOAT, "科学计数浮点", "not float");
    pln_value_free(v);

    v = pln_loads("{\na: -1\n");
    CHECK(v && v->child->type == PLN_INT && v->child->data.int_val == -1, "负数", "mismatch");
    pln_value_free(v);

    v = pln_loads("{\na: true\nb: false\nc: null\n");
    CHECK(v, "true/false/null parse", "failed");
    pln_value_t *c = v->child;
    CHECK(c->type == PLN_BOOL && c->data.bool_val == 1, "true", "mismatch");
    c = c->next;
    CHECK(c->type == PLN_BOOL && c->data.bool_val == 0, "false", "mismatch");
    c = c->next;
    CHECK(c->type == PLN_NULL, "null", "not null");
    pln_value_free(v);
}

static void unit_nesting(void) {
    printf("── 嵌套 ──\n");
    pln_value_t *v;

    v = pln_loads("{\nouter: {\ninner: \"value\"\n");
    CHECK(v && v->child->type == PLN_OBJECT && v->child->child->type == PLN_STRING, "嵌套对象", "failed");
    pln_value_free(v);

    v = pln_loads("[\n[\n1\n2 1\n[\n3\n");
    CHECK(v && v->type == PLN_ARRAY && v->child->type == PLN_ARRAY, "嵌套数组", "failed");
    pln_value_free(v);

    v = pln_loads("{\ntags: [\n\"web\"\n\"primary\"\n");
    CHECK(v && v->child->type == PLN_ARRAY, "对象中数组", "failed");
    CHECK(v->child->child && v->child->child->type == PLN_STRING, "数组元素", "failed");
    pln_value_free(v);
}

static void unit_pop(void) {
    printf("── 弹出机制 ──\n");
    pln_value_t *v;

    v = pln_loads("{\nouter: {\ninner: \"value\"\nmid: \"other\" 1\n");
    CHECK(v && v->child && v->child->type == PLN_OBJECT, "弹出1层(值先入)", "failed");
    CHECK(v->child->child && v->child->child->next, "内层应有2键", "failed");
    CHECK(strcmp(v->child->child->key, "inner") == 0 && strcmp(v->child->child->next->key, "mid") == 0, "键mid在内层(后缀)", "mismatch");
    pln_value_free(v);

    v = pln_loads("{\na: {\nb: {\nc: \"deep\"\nx: \"top\" 2\n");
    CHECK(v && v->child && v->child->type == PLN_OBJECT, "批量弹出2层", "failed");
    pln_value_t *innerb = v->child->child;
    CHECK(innerb->child && innerb->child->next, "b应含c和x", "failed");
    CHECK(strcmp(innerb->child->key, "c") == 0 && strcmp(innerb->child->next->key, "x") == 0, "x在b内(后缀)", "mismatch");
    pln_value_free(v);

    v = pln_loads("{\na: {\nb: 1\n");
    CHECK(v && v->child->type == PLN_OBJECT, "EOF自动关闭(对象)", "should be object");
    pln_value_free(v);

    v = pln_loads("[\n[\n1\n");
    CHECK(v, "EOF自动关闭(数组)", "failed");
    pln_value_free(v);
}

static void unit_strings(void) {
    printf("── 字符串 ──\n");
    pln_value_t *v;

    v = pln_loads("{\nmsg: \"He said: \"\"Hello\"\"\"\n");
    CHECK(v && strcmp(v->child->data.string_val, "He said: \"Hello\"") == 0, "转义双引号", v->child->data.string_val);
    pln_value_free(v);

    v = pln_loads("{\nmsg: \"Line1\nLine2\nLine3\"\n");
    CHECK(v && strcmp(v->child->data.string_val, "Line1\nLine2\nLine3") == 0, "跨行字符串", v->child->data.string_val);
    pln_value_free(v);

    v = pln_loads("{\nkey: \"你好世界\"\n");
    CHECK(v && strcmp(v->child->data.string_val, "你好世界") == 0, "中文", v->child->data.string_val);
    pln_value_free(v);

    v = pln_loads("{\na: \"\"\n");
    CHECK(v && v->child->type == PLN_STRING && strcmp(v->child->data.string_val, "") == 0, "空字符串", "not empty");
    pln_value_free(v);
}

static void unit_keys(void) {
    printf("── 键名 ──\n");
    pln_value_t *v = pln_loads("{\n中文键: 1\nmy-key: 2\na.b.c: 3\nuser_id: 4\n");
    CHECK(v, "扩展键名", "failed");
    int count = 0;
    for (pln_value_t *c = v->child; c; c = c->next) count++;
    CHECK(count == 4, "键数", "not 4");
    pln_value_free(v);
}

static void unit_errors(void) {
    printf("── 错误检测 ──\n");
    CHECK(pln_loads("42\n") == NULL, "顶层标量", "should fail");
    CHECK(pln_loads("\"str\"\n") == NULL, "顶层字符串", "should fail");
    CHECK(pln_loads("true\n") == NULL, "顶层true", "should fail");
    CHECK(pln_loads("{\nbad:key: 1\n") == NULL, "冒号键名", "should fail");
    CHECK(pln_loads("{\n\"key\": 1\n") == NULL, "引号键名", "should fail");
}

static void unit_roundtrip(void) {
    printf("── 往返测试 ──\n");
    struct { const char *name; const char *pl; } cases[] = {
        {"简单",     "{\na: 1\n"},
        {"多键值",   "{\na: 1\nb: 2\nc: 3\n"},
        {"嵌套",     "{\na: {\nb: 1\nc: 2 1\nd: 3\n"},
        {"数组",     "[\n1\n2\n3\n"},
        {"混合",     "{\na: [\n1\n2 1\nb: true\n"},
        {"布尔空",   "{\na: true\nb: false\nc: null\n"},
        {"浮点",     "{\na: 3.14159\n"},
    };
    for (int i = 0; i < 7; i++) {
        pln_value_t *v1 = pln_loads(cases[i].pl);
        char *s = v1 ? pln_dumps(v1) : NULL;
        pln_value_t *v2 = s ? pln_loads(s) : NULL;
        int ok = v1 && s && v2 && dom_equal(v1, v2);
        CHECK(ok, cases[i].name, ok ? "" : "roundtrip failed");
        free(s); pln_value_free(v1); pln_value_free(v2);
    }
}

static void unit_dom(void) {
    printf("── DOM API ──\n");
    pln_value_t *obj = pln_value_new_object();
    pln_value_add_to_object(obj, "k1", pln_value_new_int(1));
    pln_value_add_to_object(obj, "k2", pln_value_new_string("two"));
    CHECK(obj->child && obj->child->next, "对象构建", "count");
    pln_value_free(obj);

    pln_value_t *arr = pln_value_new_array();
    pln_value_add_to_array(arr, pln_value_new_int(1));
    pln_value_add_to_array(arr, pln_value_new_int(2));
    CHECK(arr->child && arr->child->next, "数组构建", "count");
    pln_value_free(arr);

    obj = pln_value_new_object();
    char *key = strdup("dynamic");
    pln_value_add_to_object_nocopy(obj, key, pln_value_new_int(42));
    CHECK(obj->child && strcmp(obj->child->key, "dynamic") == 0, "nocopy键名", "mismatch");
    pln_value_free(obj);
}

static void unit_stream(void) {
    printf("── 流式消息 ──\n");
    pln_value_t *m1 = pln_loads("{\ntype: \"log\"\n");
    pln_value_t *m2 = pln_loads("{\ntype: \"metric\"\n");
    CHECK(m1 && m2, "多条消息解析", "failed");
    pln_value_free(m1); pln_value_free(m2);
}

static void unit_edge(void) {
    printf("── 边缘情况 ──\n");
    pln_value_t *v;

    v = pln_loads("{\n");
    CHECK(v && v->type == PLN_OBJECT && v->child == NULL, "空对象", "not empty");
    pln_value_free(v);

    v = pln_loads("[\n");
    CHECK(v && v->type == PLN_ARRAY && v->child == NULL, "空数组", "not empty");
    pln_value_free(v);

    char buf[4096] = "";
    for (int i = 0; i < 50; i++) strcat(buf, "{\na: ");
    strcat(buf, "\"deep\"\n");
    v = pln_loads(buf);
    CHECK(v, "50层嵌套", "failed");
    pln_value_free(v);

    v = pln_loads("{\r\na: 1\r\n");
    CHECK(v && v->child->data.int_val == 1, "CRLF行尾", "mismatch");
    pln_value_free(v);

    v = pln_loads("{\na: 9223372036854775807\n");
    CHECK(v && v->child->type == PLN_INT, "大整数", "failed");
    pln_value_free(v);

    v = pln_loads("{\na: -3.14\n");
    CHECK(v && v->child->type == PLN_FLOAT, "负浮点", "not float");
    pln_value_free(v);
}

static void unit_json_conversion(void) {
    printf("── JSON 转换 ──\n");
    const char *cases[] = {
        "{\"a\":1}", "{\"a\":true,\"b\":false,\"c\":null}", "{\"a\":\"hello\"}",
        "{\"a\":3.14159}", "{\"a\":[1,2,3]}", "{\"a\":{\"b\":{\"c\":1}}}",
        "[1,\"two\",true,null]", "[]", "{}",
    };
    for (int i = 0; i < 9; i++) {
        char name[64]; snprintf(name, 64, "json-rt-%d", i);
        pln_value_t *v1 = pln_loads_json(cases[i]);
        if (!v1) { FAIL(name, "json parse failed"); continue; }
        char *js = pln_dumps_json(v1);
        if (!js) { FAIL(name, "dumps_json failed"); pln_value_free(v1); continue; }
        pln_value_t *v2 = pln_loads_json(js);
        if (!v2) { FAIL(name, "re-parse failed"); free(js); pln_value_free(v1); continue; }
        CHECK(dom_equal(v1, v2), name, "mismatch");
        free(js); pln_value_free(v1); pln_value_free(v2);
    }
}

/* ═══════════════════════════════════════════════════════════════
   真实数据一致性验证
   ═══════════════════════════════════════════════════════════════ */

static void test_real_data_consistency(const char *json_path, const char *pln_path) {
    printf("\n── 真实数据一致性 ──\n");

    int json_len, pln_len;
    char *json_text = read_file(json_path, &json_len);
    char *pln_text  = read_file(pln_path, &pln_len);

    if (!json_text || !pln_text) {
        printf("  SKIP: 无法读取数据文件\n");
        free(json_text); free(pln_text);
        return;
    }

    printf("  数据: JSON=%d B, PopLine=%d B (%.1f%%)\n",
           json_len, pln_len, pln_len * 100.0 / json_len);

    /* 1. PopLine 往返一致性 */
    {
        pln_value_t *v = pln_loads(pln_text);
        CHECK(v != NULL, "PopLine解析", "failed");
        if (v) {
            char *s = pln_dumps(v);
            pln_value_t *v2 = s ? pln_loads(s) : NULL;
            CHECK(v2 && dom_equal(v, v2), "PopLine往返", v2 ? "mismatch" : "reparse failed");
            free(s); pln_value_free(v2); pln_value_free(v);
        }
    }

    /* 2. JSON ↔ PopLine 一致性 */
    {
        pln_value_t *v_json = pln_loads_json(json_text);
        pln_value_t *v_pln  = pln_loads(pln_text);
        CHECK(v_json != NULL, "JSON解析", "failed");
        CHECK(v_pln  != NULL, "PopLine解析", "failed");
        if (v_json && v_pln) {
            CHECK(dom_equal(v_json, v_pln), "JSON↔PopLine一致", "DOM不相等");
        }
        pln_value_free(v_json); pln_value_free(v_pln);
    }

    /* 3. PopLine → JSON 往返 */
    {
        pln_value_t *v = pln_loads(pln_text);
        if (v) {
            char *j = pln_dumps_json(v);
            pln_value_t *v2 = j ? pln_loads_json(j) : NULL;
            CHECK(v2 && dom_equal(v, v2), "PopLine→JSON→PopLine", v2 ? "mismatch" : "failed");
            free(j); pln_value_free(v2); pln_value_free(v);
        }
    }

    /* 4. JSON → PopLine 往返 */
    {
        pln_value_t *v = pln_loads_json(json_text);
        if (v) {
            char *p = pln_dumps(v);
            pln_value_t *v2 = p ? pln_loads(p) : NULL;
            CHECK(v2 && dom_equal(v, v2), "JSON→PopLine→JSON", v2 ? "mismatch" : "failed");
            free(p); pln_value_free(v2); pln_value_free(v);
        }
    }

    free(json_text); free(pln_text);
}

/* ═══════════════════════════════════════════════════════════════
   性能基准
   ═══════════════════════════════════════════════════════════════ */

static void bench_real_data(const char *json_path, const char *pln_path) {
    int N = 5000;
    printf("\n── 性能基准 (%d次迭代) ──\n", N);

    int json_len, pln_len;
    char *json_text = read_file(json_path, &json_len);
    char *pln_text  = read_file(pln_path, &pln_len);
    if (!json_text || !pln_text) { printf("  SKIP\n"); free(json_text); free(pln_text); return; }
    double t0, t1;

    /* cJSON 解析 */
    t0 = now_ms();
    for (int i = 0; i < N; i++) {
        cJSON *v = cJSON_Parse(json_text);
        if (v) cJSON_Delete(v);
    }
    t1 = now_ms();
    double cjson_parse = t1 - t0;
    printf("  %-26s %8.1f ms\n", "cJSON 解析", cjson_parse);

    /* PopLine 解析 */
    t0 = now_ms();
    for (int i = 0; i < N; i++) {
        pln_value_t *v = pln_loads(pln_text);
        if (v) pln_value_free(v);
    }
    t1 = now_ms();
    double pln_parse = t1 - t0;
    printf("  %-26s %8.1f ms  (%.2fx)\n", "PopLine 解析", pln_parse, pln_parse / cjson_parse);

    /* cJSON 序列化 */
    cJSON *ctree = cJSON_Parse(json_text);
    t0 = now_ms();
    for (int i = 0; i < N; i++) {
        char *s = cJSON_PrintUnformatted(ctree);
        free(s);
    }
    t1 = now_ms();
    cJSON_Delete(ctree);
    double cjson_ser = t1 - t0;
    printf("  %-26s %8.1f ms\n", "cJSON 序列化", cjson_ser);

    /* PopLine 序列化 */
    pln_value_t *ptree = pln_loads(pln_text);
    t0 = now_ms();
    for (int i = 0; i < N; i++) {
        char *s = pln_dumps(ptree);
        free(s);
    }
    t1 = now_ms();
    pln_value_free(ptree);
    double pln_ser = t1 - t0;
    printf("  %-26s %8.1f ms  (%.2fx)\n", "PopLine 序列化", pln_ser, pln_ser / cjson_ser);

    free(json_text); free(pln_text);
}

/* ═══════════════════════════════════════════════════════════════ */

int main(void) {
    printf("PopLine v2 完整测试\n");
    printf("==================\n\n");

    unit_basic_types();
    unit_nesting();
    unit_pop();
    unit_strings();
    unit_keys();
    unit_errors();
    unit_roundtrip();
    unit_dom();
    unit_stream();
    unit_edge();
    unit_json_conversion();

    test_real_data_consistency("package.json", "package.pln");
    bench_real_data("package.json", "package.pln");

    printf("\n──────────────────────\n");
    printf("%d/%d 通过, %d 失败\n", passed, total, total - passed);
    return passed == total ? 0 : 1;
}
