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
import popline

# Parse
obj = popline.loads('{\nkey: "value"\n')

# Serialize
text = popline.dumps({"key": "value"})

# JSON conversion
obj = popline.loads_json('{"key": "value"}')
text = popline.dumps_json({"key": "value"})
```

## Performance

Data: `package.json` (17011 B) / `package.pln` (13074 B, 76.9%)

| Operation | Python json | popline | Ratio |
|-----------|------------|---------|-------|
| Parse | 656 ms | 519 ms | **0.79x** |
| Serialize | 867 ms | 193 ms | **0.22x** |

## Test

```bash
python test.py
```
