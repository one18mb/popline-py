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
import popline

# 解析
obj = popline.loads('{\nkey: "value"\n')

# 序列化
text = popline.dumps({"key": "value"})

# JSON 互转
obj = popline.loads_json('{"key": "value"}')
text = popline.dumps_json({"key": "value"})
```

## 性能

测试数据：`package.json`（17011 字节） / `package.pln`（13074 字节，76.9%）

| 操作 | Python json | popline | 比 |
|------|------------|---------|------|
| 解析 | 656 ms | 519 ms | **0.79x** |
| 序列化 | 867 ms | 193 ms | **0.22x** |

## 测试

```bash
python test.py
```
