"""Comprehensive callback chaining tests for .then/.except_/.finally_."""
import pytest
import cfuture


def test_then_receives_result():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(lambda: 100).then(lambda x, d, s: x + 1, deps=[])
        assert f.result(timeout=5.0) == 101


def test_except_skipped_on_success():
    """except_ handler should not fire if task succeeded."""
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(lambda: 42).except_(lambda e, d, s: -1, deps=[])
        assert f.result(timeout=5.0) == 42


def test_then_skipped_on_failure():
    """then handler should not fire if task failed."""
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        def fail():
            raise ValueError("nope")

        f = pool.submit(fail).then(lambda x, d, s: 999, deps=[])
        with pytest.raises(RuntimeError):
            f.result(timeout=5.0)


def test_chained_then_then():
    with cfuture.ThreadPoolExecutor(workers=2) as pool:
        f = (
            pool.submit(lambda: 1)
            .then(lambda x, d, s: x + 1, deps=[])
            .then(lambda x, d, s: x * 10, deps=[])
        )
        assert f.result(timeout=5.0) == 20


def test_except_recovers_chain():
    """except_ can recover a chain: downstream .then sees recovered value."""
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        def fail():
            raise RuntimeError("oops")

        f = (
            pool.submit(fail)
            .except_(lambda e, d, s: 0, deps=[])
            .then(lambda x, d, s: x + 99, deps=[])
        )
        assert f.result(timeout=5.0) == 99


def test_finally_fires_on_success():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f = pool.submit(lambda: 5).finally_(lambda x, d, s: x * 2, deps=[])
        assert f.result(timeout=5.0) == 10


def test_finally_fires_on_failure():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        def fail():
            raise RuntimeError("err")

        f = pool.submit(fail).finally_(lambda x, d, s: "cleaned", deps=[])
        assert f.result(timeout=5.0) == "cleaned"


def test_deps_passed_through_chain():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        multiplier = 3
        f = (
            pool.submit(lambda: 7)
            .then(lambda x, d, s: x * d[0], deps=[multiplier])
            .then(lambda x, d, s: x + d[0], deps=[1])
        )
        assert f.result(timeout=5.0) == 22  # (7 * 3) + 1


def test_multiple_callbacks_on_same_future():
    """Multiple .then() calls on the same future each get the result."""
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        base = pool.submit(lambda: 10)
        f1 = base.then(lambda x, d, s: x + 1, deps=[])
        f2 = base.then(lambda x, d, s: x + 2, deps=[])
        assert f1.result(timeout=5.0) == 11
        assert f2.result(timeout=5.0) == 12


def test_pre_completed_then():
    """Callbacks on already-resolved Future.completed() fire synchronously."""
    f = cfuture.Future.completed(50).then(lambda x, d, s: x * 2, deps=[])
    assert f.result(timeout=1.0) == 100


def test_pre_failed_except():
    """except_ on already-failed Future fires synchronously."""
    f = cfuture.Future.failed("err").except_(lambda e, d, s: "recovered", deps=[])
    assert f.result(timeout=1.0) == "recovered"
