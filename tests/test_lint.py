"""Tests for cfuture.lint CFU001 checker."""
import pytest
from cfuture.lint import check_file, xi_dataclass, CFU001


def test_no_errors_on_empty():
    errors = check_file("")
    assert errors == []


def test_no_errors_clean_lambda():
    code = """
import cfuture
pool = cfuture.ThreadPoolExecutor(workers=1)
pool.submit(lambda: 42)
"""
    errors = check_file(code)
    assert errors == []


def test_cfu001_detects_closure_capture():
    code = """
multiplier = 5
pool.submit(lambda: x * multiplier)
"""
    errors = check_file(code)
    names = [msg for _, _, msg in errors]
    assert any("multiplier" in m for m in names)


def test_cfu001_no_error_when_in_deps():
    code = """
scale = 2
pool.submit(lambda: 0).then(lambda x, d: x * scale, deps=[scale])
"""
    errors = check_file(code)
    # scale is listed in deps, so no error
    assert all("scale" not in msg for _, _, msg in errors)


def test_cfu001_fires_for_then():
    code = """
value = 10
f.then(lambda x, d: x + value, deps=[])
"""
    errors = check_file(code)
    assert any("value" in msg for _, _, msg in errors)


def test_cfu001_fires_for_except():
    code = """
handler = "default"
f.except_(lambda e, d: handler, deps=[])
"""
    errors = check_file(code)
    assert any("handler" in msg for _, _, msg in errors)


def test_cfu001_fires_for_finally():
    code = """
cleanup = True
f.finally_(lambda x, d: cleanup, deps=[])
"""
    errors = check_file(code)
    assert any("cleanup" in msg for _, _, msg in errors)


def test_lambda_params_not_flagged():
    """Lambda's own parameters must not be reported as captures."""
    code = """
f.then(lambda result, deps: result + 1, deps=[])
"""
    errors = check_file(code)
    assert all("result" not in msg and "deps" not in msg for _, _, msg in errors)


def test_syntax_error_returns_empty():
    errors = check_file("def (: pass")
    assert errors == []


def test_xi_dataclass_decorator_importable():
    """xi_dataclass is importable from cfuture.lint."""
    import dataclasses

    @xi_dataclass
    @dataclasses.dataclass
    class Pt:
        x: float

    obj = Pt(1.0)
    assert obj.__xi_encode__() == {"x": 1.0}
    assert Pt.__xi_decode__({"x": 2.0}).x == 2.0
