# PopLine Python

PopLine 序列化格式的 Python C 扩展实现。

## 安装

```bash
pip install popline-py
```

或从源码构建：

```bash
python setup.py build_ext --inplace
```

## 使用

```python
import pln

# 解析
obj = pln.loads('{\nkey: "value"\n')

# 序列化
text = pln.dumps({"key": "value"})

# JSON 互转
obj = pln.loads_json('{"key": "value"}')
text = pln.dumps_json({"key": "value"})
```

## 性能

测试数据：`package.json`（17011 B）→ `package.pln`（13074 B，**76.9%**），5000 次迭代

| 操作 | Python json | pln | 比 |
|------|------------|---------|------|
| 解析 | 656 ms (131 µs/op) | 519 ms (104 µs/op) | **0.79x** |
| 序列化 | 867 ms (173 µs/op) | 193 ms (39 µs/op) | **0.22x** |

## 测试

```bash
python test.py
```
