# PopLine Python

PopLine 序列化格式的 Python C 扩展实现。零外部依赖。

**v0.4.0：** 解析改用 SAX 接口（`pln_sax_parse`）直接构建 Python 对象，序列化使用 C 核心生成器 API，均无中间 `pln_value_t` DOM。

## 安装

```bash
pip install popline-py
```

## 使用

```python
import pln

# PopLine → Python 对象（SAX 解析，零 DOM）
obj = pln.loads('{\nkey: "value"\n')
# → {"key": "value"}

# Python 对象 → PopLine（生成器 API，不经过中间 DOM）
text = pln.dumps({"key": "value"})
# → '{\nkey: "value"\n'
```

## 性能

测试数据：`test.json`（17011 B）→ `test.pln`（13076 B，**76.9%**），5000 次迭代

| 操作 | Python json | pln | 比 |
|------|------------|---------|------|
| 解析 | 701 ms (140 µs/op) | 623 ms (125 µs/op) | **0.89x** |
| 序列化 | 943 ms (189 µs/op) | 237 ms (47 µs/op) | **0.25x** |

## 测试

```bash
python test.py
```

## 致谢
本项目的开发得到了以下 AI 工具的大力协助：
- [Claude Code](https://claude.ai)（Anthropic）
- [DeepSeek](https://deepseek.com)（深度求索）
