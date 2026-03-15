from setuptools import setup, Extension
setup(ext_modules=[Extension(
    "cfuture._cfuture",
    sources=["cfuture/_cfuture.c"],
    extra_compile_args=["-pthread", "-O2"],
    extra_link_args=["-pthread"],
)])
