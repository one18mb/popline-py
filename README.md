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

## 测试

```bash
python test.py
```
