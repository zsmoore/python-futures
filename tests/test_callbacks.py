"""Comprehensive callback chaining tests for .then/.except_/.finally_."""
import pytest
import cfuture
from cfuture import Future


def test_then_receives_result():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f: Future[int] = pool.submit(lambda: 100).then(lambda x: x + 1)
        assert f.result(timeout=5.0) == 101


def test_except_skipped_on_success():
    """except_ handler should not fire if task succeeded."""
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f: Future[int] = pool.submit(lambda: 42).except_(lambda e: -1)
        assert f.result(timeout=5.0) == 42


def test_then_skipped_on_failure():
    """then handler should not fire if task failed."""
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        def fail() -> int:
            raise ValueError("nope")

        f: Future[int] = pool.submit(fail).then(lambda x: 999)
        with pytest.raises(RuntimeError):
            f.result(timeout=5.0)


def test_chained_then_then():
    with cfuture.ThreadPoolExecutor(workers=2) as pool:
        f: Future[int] = (
            pool.submit(lambda: 1)
            .then(lambda x: x + 1)
            .then(lambda x: x * 10)
        )
        assert f.result(timeout=5.0) == 20


def test_except_recovers_chain():
    """except_ can recover a chain: downstream .then sees recovered value."""
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        def fail() -> int:
            raise RuntimeError("oops")

        f: Future[int] = (
            pool.submit(fail)
            .except_(lambda e: 0)
            .then(lambda x: x + 99)
        )
        assert f.result(timeout=5.0) == 99


def test_finally_fires_on_success():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        f: Future[int] = pool.submit(lambda: 5).finally_(lambda x: x * 2)
        assert f.result(timeout=5.0) == 10


def test_finally_fires_on_failure():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        def fail() -> str:
            raise RuntimeError("err")

        f: Future[str] = pool.submit(fail).finally_(lambda x: "cleaned")
        assert f.result(timeout=5.0) == "cleaned"


def test_deps_passed_through_chain():
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        multiplier: int = 3
        f: Future[int] = (
            pool.submit(lambda: 7)
            .then(lambda x, d: x * d[0], deps=[multiplier])
            .then(lambda x, d: x + d[0], deps=[1])
        )
        assert f.result(timeout=5.0) == 22  # (7 * 3) + 1


def test_multiple_callbacks_on_same_future():
    """Multiple .then() calls on the same future each get the result."""
    with cfuture.ThreadPoolExecutor(workers=1) as pool:
        base: Future[int] = pool.submit(lambda: 10)
        f1: Future[int] = base.then(lambda x: x + 1)
        f2: Future[int] = base.then(lambda x: x + 2)
        assert f1.result(timeout=5.0) == 11
        assert f2.result(timeout=5.0) == 12


def test_pre_completed_then():
    """Callbacks on already-resolved Future.completed() fire synchronously."""
    f: Future[int] = cfuture.Future.completed(50).then(lambda x: x * 2)
    assert f.result(timeout=1.0) == 100


def test_pre_failed_except():
    """except_ on already-failed Future fires synchronously."""
    f: Future[str] = cfuture.Future.failed("err").except_(lambda e: "recovered")
    assert f.result(timeout=1.0) == "recovered"
