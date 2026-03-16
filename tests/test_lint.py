"""Tests for cfuture.lint CFU001 and CFU002 checker."""
import pytest
from cfuture.lint import check_file, CFU001, CFU002


# ── CFU001: closure capture detection ────────────────────────────────────────

def test_no_errors_on_empty():
    assert check_file("") == []


def test_no_errors_clean_lambda():
    code = """
import cfuture
pool = cfuture.ThreadPoolExecutor(workers=1)
pool.submit(lambda: 42)
"""
    assert check_file(code) == []


def test_cfu001_detects_closure_capture():
    code = """
multiplier = 5
pool.submit(lambda: x * multiplier)
"""
    errors = check_file(code)
    assert any("multiplier" in msg for _, _, msg in errors)


def test_cfu001_no_error_when_in_deps():
    code = """
scale = 2
pool.submit(lambda: 0).then(lambda x, d, s: x * scale, deps=[scale])
"""
    errors = check_file(code)
    assert all("scale" not in msg for _, _, msg in errors)


def test_cfu001_fires_for_then():
    code = """
value = 10
f.then(lambda x, d, s: x + value, deps=[])
"""
    errors = check_file(code)
    assert any("value" in msg for _, _, msg in errors)


def test_cfu001_fires_for_except():
    code = """
handler = "default"
f.except_(lambda e, d, s: handler, deps=[])
"""
    errors = check_file(code)
    assert any("handler" in msg for _, _, msg in errors)


def test_cfu001_fires_for_finally():
    code = """
cleanup = True
f.finally_(lambda x, d, s: cleanup, deps=[])
"""
    errors = check_file(code)
    assert any("cleanup" in msg for _, _, msg in errors)


def test_lambda_params_not_flagged():
    """Lambda's own parameters must not be reported as captures."""
    code = """
f.then(lambda result, deps, shared: result + 1, deps=[])
"""
    errors = check_file(code)
    assert all("result" not in msg and "deps" not in msg and "shared" not in msg
               for _, _, msg in errors)


def test_syntax_error_returns_empty():
    assert check_file("def (: pass") == []


# ── CFU002: xi-protocol class inside function ─────────────────────────────────

def test_cfu002_fires_for_xi_dataclass_inside_function():
    code = """
import dataclasses
from cfuture import xi_dataclass

def my_task():
    @xi_dataclass
    @dataclasses.dataclass
    class Point:
        x: float
        y: float
"""
    errors = check_file(code)
    assert any("CFU002" in msg and "Point" in msg for _, _, msg in errors)


def test_cfu002_fires_for_manual_xi_encode_inside_function():
    code = """
def my_task():
    class Color:
        def __xi_encode__(self): return {}
        @classmethod
        def __xi_decode__(cls, d): return cls()
"""
    errors = check_file(code)
    assert any("CFU002" in msg and "Color" in msg for _, _, msg in errors)


def test_cfu002_no_error_for_module_level_xi_class():
    code = """
import dataclasses
from cfuture import xi_dataclass

@xi_dataclass
@dataclasses.dataclass
class Point:
    x: float
    y: float
"""
    errors = check_file(code)
    assert all("CFU002" not in msg for _, _, msg in errors)


def test_cfu002_no_error_for_plain_class_inside_function():
    code = """
def my_task():
    class Helper:
        def run(self): return 1
"""
    errors = check_file(code)
    assert all("CFU002" not in msg for _, _, msg in errors)


