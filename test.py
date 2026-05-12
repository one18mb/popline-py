"""
PopLine Python C 扩展完整测试
- 单元测试（类型、嵌套、弹出、字符串、键名、错误）
- 往返一致性
- JSON 标准库对比验证
- 真实数据一致性 + 性能基准
"""

import json
import os
import sys
import time

import pln

PASS = 0
FAIL = 0

def test(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        msg = f"  FAIL [{name}]: {detail}" if detail else f"  FAIL [{name}]"
        print(msg)

# ══════════════════════════════════════════════════════════════════
# 辅助函数
# ══════════════════════════════════════════════════════════════════

ETC = {}

def pl_dumps(obj):
    return pln.dumps(obj)

def pl_loads(text):
    return pln.loads(text)

# ══════════════════════════════════════════════════════════════════
# 单元测试
# ══════════════════════════════════════════════════════════════════

def test_basic_types():
    print("── 基本类型 ──")

    # 简单对象
    obj = pl_loads('{\nname: "popline"\n')
    test("简单对象", isinstance(obj, dict) and obj.get("name") == "popline")

    # 整数
    obj = pl_loads('{\na: 42\n')
    test("整数", isinstance(obj, dict) and obj["a"] == 42)

    # 浮点数
    obj = pl_loads('{\na: 3.14159\n')
    test("浮点数", isinstance(obj, dict) and isinstance(obj["a"], float))

    # 科学计数
    obj = pl_loads('{\na: 6.022e23\n')
    test("科学计数", isinstance(obj, dict) and isinstance(obj["a"], float))

    # 负数
    obj = pl_loads('{\na: -1\n')
    test("负数", obj["a"] == -1)

    # true/false/null
    obj = pl_loads('{\na: true\nb: false\nc: null\n')
    test("true", obj["a"] is True)
    test("false", obj["b"] is False)
    test("null", obj["c"] is None)

def test_nesting():
    print("── 嵌套 ──")
    obj = pl_loads('{\nouter: {\ninner: "value"\n')
    test("嵌套对象", obj["outer"]["inner"] == "value")

    obj = pl_loads('[[\n1\n2\n1 [\n3\n')
    test("嵌套数组", obj == [[1, 2], [3]])

    obj = pl_loads('{\ntags: [\n"web"\n"primary"\n')
    test("对象中数组", obj["tags"] == ["web", "primary"])

def test_pop():
    print("── 弹出机制 ──")
    obj = pl_loads('{\nouter: {\ninner: "value"\n1 mid: "other"\n')
    test("弹出1层", obj == {"outer": {"inner": "value"}, "mid": "other"})

    obj = pl_loads('{\na: {\nb: {\nc: "deep"\n2 x: "top"\n')
    test("批量弹出2层", obj["x"] == "top")

def test_strings():
    print("── 字符串 ──")
    obj = pl_loads('{\nmsg: "He said: ""Hello"""\n')
    test("转义双引号", obj["msg"] == 'He said: "Hello"')

    obj = pl_loads('{\nmsg: "Line1\nLine2\nLine3"\n')
    test("跨行字符串", obj["msg"] == "Line1\nLine2\nLine3")

    obj = pl_loads('{\nkey: "你好世界"\n')
    test("中文", obj["key"] == "你好世界")

    obj = pl_loads('{\na: ""\n')
    test("空字符串", obj["a"] == "")

def test_keys():
    print("── 键名 ──")
    obj = pl_loads('{\n中文键: 1\nmy-key: 2\na.b.c: 3\nuser_id: 4\n')
    test("扩展键名", len(obj) == 4 and obj["my-key"] == 2)

def test_errors():
    print("── 错误检测 ──")
    for expr in ["42\n", '"str"\n', "true\n", '{\nbad:key: 1\n', '{\n"key": 1\n']:
        try:
            pl_loads(expr)
            test(f"应报错: {expr[:20]}", False, "应该抛出异常")
        except ValueError:
            test(f"正确报错: {expr[:20]}", True)

def test_roundtrip():
    print("── 往返测试 ──")
    cases = [
        ("简单", {"a": 1}),
        ("多键", {"a": 1, "b": 2, "c": 3}),
        ("嵌套", {"a": {"b": 1, "c": 2, "d": 3}}),
        ("数组", [1, 2, 3]),
        ("混合", {"a": [1, 2], "b": True}),
        ("null", {"a": None, "b": True, "c": False}),
        ("浮点", {"a": 3.14159}),
        ("中文", {"msg": "你好世界"}),
        ("转义", {"msg": 'He said: "Hello"'}),
        ("跨行", {"msg": "Line1\nLine2"}),
        ("深层嵌套", {"a": {"b": {"c": {"d": {"e": 1}}}}}),
    ]
    for name, obj in cases:
        text = pl_dumps(obj)
        try:
            parsed = pl_loads(text)
            test(f"往返-{name}", parsed == obj, str(parsed))
        except Exception as e:
            test(f"往返-{name}", False, str(e))

def test_json_consistency():
    """与 json 模块对比验证"""
    print("── JSON 一致性 ──")
    cases = [
        {"a": 1},
        {"a": True, "b": False, "c": None},
        {"a": "hello"},
        {"a": 3.14159},
        {"a": [1, 2, 3]},
        {"a": {"b": {"c": 1}}},
        [1, "two", True, None],
        [],
        {},
        {"中文": "值"},
        {"nested": {"list": [1, {"x": "y"}]}},
    ]
    for i, obj in enumerate(cases):
        pl_text = pl_dumps(obj)
        try:
            parsed = pl_loads(pl_text)
            test(f"JSON一致-{i}", parsed == obj, str(parsed))
        except Exception as e:
            test(f"JSON一致-{i}", False, str(e))

# ══════════════════════════════════════════════════════════════════
# 真实数据测试
# ══════════════════════════════════════════════════════════════════

def test_real_data():
    print("\n── 真实数据一致性 ──")
    json_path = "package.json"
    pln_path = "package.pln"

    if not os.path.exists(json_path) or not os.path.exists(pln_path):
        print("  SKIP: 数据文件不存在")
        return

    with open(json_path, encoding="utf-8") as f:
        json_text = f.read()
    with open(pln_path, encoding="utf-8") as f:
        pln_text = f.read()

    json_obj = json.loads(json_text)

    print(f"  数据: JSON={len(json_text)} B, PopLine={len(pln_text)} B ({len(pln_text)/len(json_text)*100:.1f}%)")

    # 1. PopLine 解析一致
    pl_obj = pl_loads(pln_text)
    test("真实-解析", pl_obj == json_obj, "PopLine解析结果与JSON不一致")

    # 2. PopLine 往返
    rt = pl_dumps(pl_obj)
    rt_obj = pl_loads(rt)
    test("真实-往返", rt_obj == json_obj)

    # 3. JSON → dumps → loads 一致
    pl_from_json = pl_dumps(json_obj)
    back = pl_loads(pl_from_json)
    test("真实-JSON→PopLine→对象", back == json_obj)

    # 4. dumps_json → loads_json 往返
    rt_json = json.dumps(json_obj, sort_keys=True, ensure_ascii=False)
    test("真实-json.dumps往返", json.loads(rt_json) == json_obj)

# ══════════════════════════════════════════════════════════════════
# 性能基准
# ══════════════════════════════════════════════════════════════════

def bench(label, fn, n=5000):
    fn()  # warmup
    t0 = time.perf_counter()
    for _ in range(n):
        fn()
    t1 = time.perf_counter()
    total_ms = (t1 - t0) * 1000
    avg_us = total_ms / n * 1000
    return total_ms, avg_us

def bench_real_data():
    print("\n── 性能基准 ──")
    json_path = "package.json"
    pln_path = "package.pln"

    if not os.path.exists(json_path) or not os.path.exists(pln_path):
        print("  SKIP: 数据文件不存在")
        return

    with open(json_path, encoding="utf-8") as f:
        json_text = f.read()
    with open(pln_path, encoding="utf-8") as f:
        pln_text = f.read()

    json_obj = json.loads(json_text)
    N = 5000

    print(f"  {'' :26s} {'总耗时':>10s}  {'每次':>10s}")
    print(f"  {'─'*26} {'─'*10}  {'─'*10}")

    js_time, js_avg = bench("json.dumps", lambda: json.dumps(json_obj), N)
    print(f"  {'json.dumps':26s} {js_time:8.1f} ms  {js_avg:8.1f} us")

    pl_ser_time, pl_ser_avg = bench("pln.dumps", lambda: pl_dumps(json_obj), N)
    print(f"  {'pln.dumps':26s} {pl_ser_time:8.1f} ms  {pl_ser_avg:8.1f} us")
    print(f"  {'PopLine/JSON':26s} {pl_ser_time/js_time:7.2f}x")

    jl_time, jl_avg = bench("json.loads", lambda: json.loads(json_text), N)
    print(f"  {'json.loads':26s} {jl_time:8.1f} ms  {jl_avg:8.1f} us")

    pl_par_time, pl_par_avg = bench("pln.loads", lambda: pl_loads(pln_text), N)
    print(f"  {'pln.loads':26s} {pl_par_time:8.1f} ms  {pl_par_avg:8.1f} us")
    print(f"  {'PopLine/JSON':26s} {pl_par_time/jl_time:7.2f}x")

# ══════════════════════════════════════════════════════════════════

def main():
    global PASS, FAIL
    print("PopLine Python 完整测试\n")

    test_basic_types()
    test_nesting()
    test_pop()
    test_strings()
    test_keys()
    test_errors()
    test_roundtrip()
    test_json_consistency()
    test_real_data()
    bench_real_data()

    print(f"\n{'─'*46}")
    print(f"{PASS}/{PASS+FAIL} 通过, {FAIL} 失败")
    return 0 if FAIL == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
