# PopLine Python

Python C extension for the PopLine serialization format.

## Install

```bash
pip install popline-py
```

Or build from source:

```bash
python setup.py build_ext --inplace
```

## Usage

```python
import pln

# Parse
obj = pln.loads('{\nkey: "value"\n')

# Serialize
text = pln.dumps({"key": "value"})

# JSON conversion
obj = pln.loads_json('{"key": "value"}')
text = pln.dumps_json({"key": "value"})
```

## Performance

Data: `package.json` (17011 B) → `package.pln` (13074 B, **76.9%**), 5000 iterations

| Operation | Python json | pln | Ratio |
|-----------|------------|---------|-------|
| Parse | 689 ms (137 µs/op) | 626 ms (125 µs/op) | **0.91x** |
| Serialize | 935 ms (187 µs/op) | 213 ms (42 µs/op) | **0.23x** |

## Test

```bash
python test.py
```
