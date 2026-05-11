from setuptools import setup, Extension

module = Extension(
    'pln',
    sources=['popline_module.c', 'popline.c', 'popline_parser.c', 'popline_json.c'],
    libraries=['cjson'],
    extra_compile_args=['-O2'],
)

setup(
    name='popline-py',
    version='1.0.0',
    description='PopLine — Line-oriented serialization format',
    ext_modules=[module],
)
