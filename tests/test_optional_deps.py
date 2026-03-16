"""Tests for optional deps= parameter on then/except_/finally_."""
import cfuture


def test_then_without_deps():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(lambda: 5).then(lambda x: x + 1)
        assert f.result(timeout=5.0) == 6


def test_except_without_deps():
    def boom():
        raise ValueError("fail")

    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(boom).except_(lambda e: 99)
        assert f.result(timeout=5.0) == 99


def test_finally_without_deps():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(lambda: 7).finally_(lambda x: x * 2)
        assert f.result(timeout=5.0) == 14


def test_chain_mixed_deps_and_no_deps():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = (
            pool.submit(lambda: 1)
            .then(lambda x, d: x + d[0], deps=[9])
            .then(lambda x: x * 2)
        )
        assert f.result(timeout=5.0) == 20


def test_explicit_empty_deps_still_works():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(lambda: 3).then(lambda x: x + 1, deps=[])
        assert f.result(timeout=5.0) == 4
