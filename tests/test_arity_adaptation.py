"""Tests for arity adaptation: callbacks may declare 1, 2, or 3 parameters."""
import cfuture


def test_one_arg_then():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(lambda: 10).then(lambda x: x + 1)
        assert f.result(timeout=5.0) == 11


def test_two_arg_then():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(lambda: 10).then(lambda x, d: x + d[0], deps=[5])
        assert f.result(timeout=5.0) == 15


def test_three_arg_then():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(lambda: 10).then(lambda x, d, s: x + 1)
        assert f.result(timeout=5.0) == 11


def test_one_arg_except():
    def boom():
        raise ValueError("oops")

    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(boom).except_(lambda e: -1)
        assert f.result(timeout=5.0) == -1


def test_two_arg_except():
    def boom():
        raise ValueError("oops")

    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(boom).except_(lambda e, d: d[0], deps=[42])
        assert f.result(timeout=5.0) == 42


def test_one_arg_finally():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(lambda: 7).finally_(lambda x: x * 3)
        assert f.result(timeout=5.0) == 21


def test_chained_mixed_arity():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = (
            pool.submit(lambda: 2)
            .then(lambda x: x * 10)
            .then(lambda x, d: x + d[0], deps=[5])
            .then(lambda x, d, s: x - 1)
        )
        assert f.result(timeout=5.0) == 24


def test_shared_accessible_with_one_arg_still_works():
    """1-arg callback just ignores shared — pool shared= still encodes fine."""
    with cfuture.ThreadPoolExecutor(workers=1, shared={"key": 99}) as pool:
        f = pool.submit(lambda: 0).then(lambda x: x + 1)
        assert f.result(timeout=5.0) == 1
