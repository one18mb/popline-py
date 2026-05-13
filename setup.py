from setuptools import setup, Extension
from os import path

this_dir = path.abspath(path.dirname(__file__))
readme_path = path.join(this_dir, 'README.md')
long_desc = ''
if path.exists(readme_path):
    with open(readme_path, encoding='utf-8') as f:
        long_desc = f.read()

module = Extension(
    'pln',
    sources=[
        'popline_module.c',
        'popline.c',
        'popline_parser.c',
    ],
    include_dirs=['.'],
    extra_compile_args=['-O2'],
)

setup(
    name='popline-py',
    version='0.4.0',
    description='PopLine — Line-oriented serialization format (C extension)',
    long_description=long_desc,
    long_description_content_type='text/markdown',
    author='one18mb',
    author_email='1915182174@qq.com',
    url='https://github.com/one18mb/popline-py',
    project_urls={
        'Source': 'https://github.com/one18mb/popline-py',
        'Spec': 'https://github.com/one18mb/popline',
    },
    license='MIT',
    ext_modules=[module],
    python_requires='>=3.6',
    classifiers=[
        'Development Status :: 4 - Beta',
        'Intended Audience :: Developers',
        'License :: OSI Approved :: MIT License',
        'Programming Language :: C',
        'Programming Language :: Python :: 3',
        'Topic :: Software Development :: Libraries',
        'Topic :: Text Processing :: Markup',
    ],
)
