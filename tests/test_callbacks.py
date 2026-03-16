"""Comprehensive callback chaining tests for .then/.except_/.finally_."""
import pytest
import cfuture


def test_then_receives_result():
    pool = cfuture.ThreadPoolExecutor(workers=1)
    f = pool.submit(lambda: 100).then(lambda x, d: x + 1, deps=[])
    assert f.result(timeout=5.0) == 101
    pool.shutdown()


def test_except_skipped_on_success():
    """except_ handler should not fire if task succeeded."""
    pool = cfuture.ThreadPoolExecutor(workers=1)
    f = pool.submit(lambda: 42).except_(lambda e, d: -1, deps=[])
    assert f.result(timeout=5.0) == 42
    pool.shutdown()


def test_then_skipped_on_failure():
    """then handler should not fire if task failed."""
    pool = cfuture.ThreadPoolExecutor(workers=1)

    def fail():
        raise ValueError("nope")

    f = pool.submit(fail).then(lambda x, d: 999, deps=[])
    with pytest.raises(RuntimeError):
        f.result(timeout=5.0)
    pool.shutdown()


def test_chained_then_then():
    pool = cfuture.ThreadPoolExecutor(workers=2)
    f = (
        pool.submit(lambda: 1)
        .then(lambda x, d: x + 1, deps=[])
        .then(lambda x, d: x * 10, deps=[])
    )
    assert f.result(timeout=5.0) == 20
    pool.shutdown()


def test_except_recovers_chain():
    """except_ can recover a chain: downstream .then sees recovered value."""
    pool = cfuture.ThreadPoolExecutor(workers=1)

    def fail():
        raise RuntimeError("oops")

    f = (
        pool.submit(fail)
        .except_(lambda e, d: 0, deps=[])
        .then(lambda x, d: x + 99, deps=[])
    )
    assert f.result(timeout=5.0) == 99
    pool.shutdown()


def test_finally_fires_on_success():
    pool = cfuture.ThreadPoolExecutor(workers=1)
    f = pool.submit(lambda: 5).finally_(lambda x, d: x * 2, deps=[])
    assert f.result(timeout=5.0) == 10
    pool.shutdown()


def test_finally_fires_on_failure():
    pool = cfuture.ThreadPoolExecutor(workers=1)

    def fail():
        raise RuntimeError("err")

    f = pool.submit(fail).finally_(lambda x, d: "cleaned", deps=[])
    assert f.result(timeout=5.0) == "cleaned"
    pool.shutdown()


def test_deps_passed_through_chain():
    pool = cfuture.ThreadPoolExecutor(workers=1)
    multiplier = 3
    f = (
        pool.submit(lambda: 7)
        .then(lambda x, d: x * d[0], deps=[multiplier])
        .then(lambda x, d: x + d[0], deps=[1])
    )
    assert f.result(timeout=5.0) == 22  # (7 * 3) + 1
    pool.shutdown()


def test_multiple_callbacks_on_same_future():
    """Multiple .then() calls on the same future each get the result."""
    pool = cfuture.ThreadPoolExecutor(workers=1)
    base = pool.submit(lambda: 10)
    f1 = base.then(lambda x, d: x + 1, deps=[])
    f2 = base.then(lambda x, d: x + 2, deps=[])
    assert f1.result(timeout=5.0) == 11
    assert f2.result(timeout=5.0) == 12
    pool.shutdown()


def test_pre_completed_then():
    """Callbacks on already-resolved Future.completed() fire synchronously."""
    f = cfuture.Future.completed(50).then(lambda x, d: x * 2, deps=[])
    assert f.result(timeout=1.0) == 100


def test_pre_failed_except():
    """except_ on already-failed Future fires synchronously."""
    f = cfuture.Future.failed("err").except_(lambda e, d: "recovered", deps=[])
    assert f.result(timeout=1.0) == "recovered"
