from setuptools import setup, Extension
setup(ext_modules=[Extension(
    "cfuture._cfuture",
    sources=[
        "cfuture/_cfuture_shared_value.c",
        "cfuture/_cfuture_pickled.c",
        "cfuture/_cfuture_future.c",
        "cfuture/_cfuture_worker.c",
        "cfuture/_cfuture.c",
    ],
    extra_compile_args=["-pthread", "-O2"],
    extra_link_args=["-pthread"],
)])
